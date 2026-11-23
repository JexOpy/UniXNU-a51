// SPDX-License-Identifier: BSD-2-Clause
// Exynos 9611 (S5E9611) UFSHCI 2.1 controller and SHNGROOT window driver for UniXNU.
//
// Assumes uefi-a51 (UfsDxe / PlatformUfsLib) has powered on the UFS PHY, established
// the UniPro link in HS Gear mode, completed fDeviceInit, and left HCE=1, HCS.DP=1,
// HCS.UTRLRDY=1. Register layout, UTRD byte units, 1024-byte aligned UPIUs, and
// 4 KiB PRDT scatter-gather segments match Samsung's S5E9610/9611 UFSHCI hardware
// (verified against uefi-a51 Silicon/Samsung/SamsungPkg/Drivers/UfsDxe).

#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <libkern/OSByteOrder.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/crypto/sha2.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>

namespace {

// Standard JEDEC UFSHCI registers (physical base 0x13520000 on Exynos 9611)
constexpr uint32_t kRegCap     = 0x00;
constexpr uint32_t kRegVer     = 0x08;
constexpr uint32_t kRegIs      = 0x20;
constexpr uint32_t kRegIe      = 0x24;
constexpr uint32_t kRegHcs     = 0x30;
constexpr uint32_t kRegHce     = 0x34;
constexpr uint32_t kRegUtrlba  = 0x50;
constexpr uint32_t kRegUtrlbau = 0x54;
constexpr uint32_t kRegUtrldbr = 0x58;
constexpr uint32_t kRegUtrlclr = 0x5C;
constexpr uint32_t kRegUtrlrsr = 0x60;

// Exynos 9611 vendor-specific HCI registers (physical base 0x13521100)
constexpr uint32_t kVsTxPrdtEntrySize = 0x00; // bit shift: 12 => 4096 B
constexpr uint32_t kVsRxPrdtEntrySize = 0x04; // bit shift: 12 => 4096 B
constexpr uint32_t kVsUtrlNexusType   = 0x40;
constexpr uint32_t kVsDataReorder     = 0x60;

constexpr uint32_t kHcsDp      = (1u << 0);
constexpr uint32_t kHcsUtrlrdy = (1u << 1);

constexpr uint32_t kIsUtrcs          = (1u << 0);
constexpr uint32_t kIsUicLinkLost    = (1u << 7);
constexpr uint32_t kIsDevFatalErr    = (1u << 11);
constexpr uint32_t kIsCtrlFatalErr   = (1u << 16);
constexpr uint32_t kIsSysBusFatalErr = (1u << 17);
constexpr uint32_t kIsFatalMask =
    kIsUicLinkLost | kIsDevFatalErr | kIsCtrlFatalErr | kIsSysBusFatalErr;

// Exynos 9611 UTRD DW0 flags (matching UfsDxe.h)
constexpr uint32_t kUtpNoData         = 0x00000000u;
constexpr uint32_t kUtpHostToDevice   = 0x02000000u;
constexpr uint32_t kUtpDeviceToHost   = 0x04000000u;
constexpr uint32_t kUtpScsiCommand    = 0x00000000u;
constexpr uint32_t kUtpReqDescIntCmd  = 0x01000000u;

constexpr uint32_t kSgBlockSizeBit  = 12u;
constexpr uint32_t kSgBlockSize     = (1u << kSgBlockSizeBit); // 4096 B
constexpr uint32_t kAlignedUpiuSize = 1024u;
constexpr uint32_t kMaxSgSegments   = 128u;
constexpr uint32_t kBounceBytes     = 64u * 1024u; // 16 x 4 KiB segments

struct __attribute__((packed)) ExynosUfsUpiuHeader {
  uint8_t  type;
  uint8_t  flags;
  uint8_t  lun;
  uint8_t  tag;
  uint8_t  cmd_type;
  uint8_t  function;
  uint8_t  response;
  uint8_t  status;
  uint8_t  ehs_length;
  uint8_t  device_info;
  uint16_t data_length;
};
static_assert(sizeof(ExynosUfsUpiuHeader) == 12, "UPIU header must be 12 bytes");

struct __attribute__((packed)) ExynosUfsUpiu {
  ExynosUfsUpiuHeader header;
  uint8_t             tsf[20];
  uint8_t             data[kAlignedUpiuSize - 20 - sizeof(ExynosUfsUpiuHeader)];
};
static_assert(sizeof(ExynosUfsUpiu) == 1024, "Aligned UPIU must be 1024 bytes");

struct __attribute__((packed)) ExynosUfsPrdt {
  uint32_t base_addr;
  uint32_t upper_addr;
  uint32_t reserved;
  uint32_t size; // byte count - 1
};
static_assert(sizeof(ExynosUfsPrdt) == 16, "PRDT entry must be 16 bytes");

struct __attribute__((packed)) ExynosUfsCmdDesc {
  ExynosUfsUpiu command_upiu;               // +0x000 (1024 B)
  ExynosUfsUpiu response_upiu;              // +0x400 (1024 B)
  ExynosUfsPrdt prd_table[kMaxSgSegments];  // +0x800 (2048 B)
};
static_assert(sizeof(ExynosUfsCmdDesc) == 4096, "CmdDesc must be 4096 bytes");

struct __attribute__((packed)) ExynosUfsUtrd {
  uint32_t dw[4];
  uint32_t cmd_desc_addr_l;
  uint32_t cmd_desc_addr_h;
  uint16_t rsp_upiu_len; // Exynos takes bytes (1024)
  uint16_t rsp_upiu_off; // Exynos takes bytes (1024)
  uint16_t prdt_len;     // Exynos takes bytes (sg_segs * 16)
  uint16_t prdt_off;     // Exynos takes bytes (2048)
};
static_assert(sizeof(ExynosUfsUtrd) == 32, "UTRD must be 32 bytes");

static inline void exynosUfsFlushCache(const void *va, size_t len) {
  uintptr_t start = reinterpret_cast<uintptr_t>(va) & ~static_cast<uintptr_t>(63u);
  uintptr_t end   = reinterpret_cast<uintptr_t>(va) + len;
  for (uintptr_t p = start; p < end; p += 64u) {
    __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
}

} // namespace

class ExynosUfs : public IOService {
  OSDeclareDefaultStructors(ExynosUfs)

private:
  IOMemoryMap *hciMap_   = nullptr;
  IOMemoryMap *vsHciMap_ = nullptr;
  volatile uint32_t *hci_   = nullptr;
  volatile uint32_t *vsHci_ = nullptr;

  IOBufferMemoryDescriptor *ringBuf_   = nullptr;
  IOBufferMemoryDescriptor *bounceBuf_ = nullptr;
  IOLock *ioLock_ = nullptr;

  ExynosUfsUtrd    *utrd_       = nullptr;
  ExynosUfsCmdDesc *ucd_        = nullptr;
  uint64_t          utrdPhys_   = 0;
  uint64_t          ucdPhys_    = 0;
  uint64_t          bouncePhys_ = 0;
  uint8_t          *bounceVirt_ = nullptr;

  uint64_t blockCount_ = 0;
  uint32_t blockSize_  = 4096;

  inline uint32_t readHci(uint32_t off) const {
    uint32_t val;
    __asm__ volatile("ldr %w0, [%1]"
                     : "=r"(val)
                     : "r"(reinterpret_cast<uintptr_t>(hci_) + off)
                     : "memory");
    return val;
  }
  inline void writeHci(uint32_t off, uint32_t val) {
    __asm__ volatile("str %w0, [%1]"
                     :: "r"(val), "r"(reinterpret_cast<uintptr_t>(hci_) + off)
                     : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
  }
  inline void writeVs(uint32_t off, uint32_t val) {
    __asm__ volatile("str %w0, [%1]"
                     :: "r"(val), "r"(reinterpret_cast<uintptr_t>(vsHci_) + off)
                     : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
  }

  bool mapRegisters();
  IOReturn submitScsi(uint8_t lun, const uint8_t cdb[16],
                      bool isWrite, uint32_t xferBytes);
  void requestSense(uint8_t lun);
  bool readCapacity(uint8_t lun);
  void parseShngroot();

public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;

  IOReturn readWindowBytes(uint64_t byteOffset, uint64_t byteLen,
                           IOMemoryDescriptor *dst);
};

class ExynosUfsWindow : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(ExynosUfsWindow)

private:
  ExynosUfs *parent_    = nullptr;
  uint64_t   baseByte_  = 0;
  uint64_t   sizeByte_  = 0;
  uint32_t   blkSize_   = 512;
  char       name_[32]  = {};
  uint8_t    sha256_[32] = {};

public:
  bool initWindow(ExynosUfs *parent, const char *name,
                  uint64_t baseByte, uint64_t sizeByte, uint32_t blkSize,
                  const uint8_t sha256[32]);
  const char    *windowName() const { return name_; }
  const uint8_t *windowSha256() const { return sha256_; }
  bool start(IOService *provider) override;

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32   doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char    *getVendorString() override { return const_cast<char *>("Samsung"); }
  char    *getProductString() override { return name_; }
  char    *getRevisionString() override { return const_cast<char *>("1.0"); }
  char    *getAdditionalDeviceInfoString() override { return const_cast<char *>("Exynos9611-SHNGROOT"); }
  IOReturn reportBlockSize(UInt64 *blockSize) override {
    *blockSize = blkSize_;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *isEjectable) override {
    *isEjectable = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *maxBlock) override {
    uint64_t n = sizeByte_ / blkSize_;
    *maxBlock = n > 0 ? (n - 1) : 0;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *mediaPresent, bool *changedState) override {
    *mediaPresent = (sizeByte_ > 0);
    if (changedState) {
      *changedState = false;
    }
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *isRemovable) override {
    *isRemovable = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *isWriteProtected) override {
    *isWriteProtected = true;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override {
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                            UInt64 nblks, IOStorageAttributes *attributes,
                            IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(ExynosUfs, IOService)
OSDefineMetaClassAndStructors(ExynosUfsWindow, IOBlockStorageDevice)

bool ExynosUfsWindow::initWindow(ExynosUfs *parent, const char *name,
                                 uint64_t baseByte, uint64_t sizeByte,
                                 uint32_t blkSize, const uint8_t sha256[32]) {
  if (!IOBlockStorageDevice::init(nullptr)) {
    return false;
  }
  parent_   = parent;
  baseByte_ = baseByte;
  sizeByte_ = sizeByte;
  blkSize_  = blkSize ? blkSize : 512u;
  strlcpy(name_, name ? name : "Window", sizeof(name_));
  if (sha256) {
    memcpy(sha256_, sha256, sizeof(sha256_));
  }
  return true;
}

bool ExynosUfsWindow::start(IOService *provider) {
  if (!IOBlockStorageDevice::start(provider)) {
    return false;
  }
  OSDictionary *characteristics = OSDictionary::withCapacity(1);
  OSString *medium = OSString::withCString(kIOPropertyMediumTypeSolidStateKey);
  if (characteristics && medium && characteristics->setObject(kIOPropertyMediumTypeKey, medium)) {
    setProperty(kIOPropertyDeviceCharacteristicsKey, characteristics);
  }
  OSSafeReleaseNULL(medium);
  OSSafeReleaseNULL(characteristics);
  return true;
}

IOReturn ExynosUfsWindow::doAsyncReadWrite(IOMemoryDescriptor *buffer,
                                           UInt64 block, UInt64 nblks,
                                           IOStorageAttributes *,
                                           IOStorageCompletion *completion) {
  if (!buffer || !completion || !completion->action || !nblks) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection() == kIODirectionOut) {
    completion->action(completion->target, completion->parameter, kIOReturnNotWritable, 0);
    return kIOReturnSuccess;
  }
  uint64_t totalBlks = sizeByte_ / blkSize_;
  if (block >= totalBlks || nblks > totalBlks - block) {
    completion->action(completion->target, completion->parameter, kIOReturnBadArgument, 0);
    return kIOReturnSuccess;
  }
  uint64_t byteOff = block * blkSize_;
  uint64_t byteLen = nblks * blkSize_;
  if (buffer->getLength() < byteLen) {
    return kIOReturnBadArgument;
  }
  IOReturn st = parent_->readWindowBytes(baseByte_ + byteOff, byteLen, buffer);
  completion->action(completion->target, completion->parameter, st,
                     (st == kIOReturnSuccess) ? byteLen : 0);
  return kIOReturnSuccess;
}

bool ExynosUfs::mapRegisters() {
  IORegistryEntry *soc = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
  IORegistryEntry *ufs = IORegistryEntry::fromPath("/arm-io/ufs", gIODTPlane);
  if (!soc || !ufs) {
    if (soc) soc->release();
    if (ufs) ufs->release();
    kprintf("ExynosUfs: /arm-io or /arm-io/ufs missing from ADT\n");
    return false;
  }
  OSData *ranges = OSDynamicCast(OSData, soc->getProperty("ranges"));
  OSData *reg    = OSDynamicCast(OSData, ufs->getProperty("reg"));
  uint64_t bus[3] = {};
  uint64_t regions[4] = {};
  bool valid = (ranges != nullptr) && (ranges->getLength() == sizeof(bus)) &&
               (reg != nullptr) && (reg->getLength() >= sizeof(regions));
  if (valid) {
    memcpy(bus, ranges->getBytesNoCopy(), sizeof(bus));
    memcpy(regions, reg->getBytesNoCopy(), sizeof(regions));
  }
  soc->release();
  ufs->release();
  if (!valid) {
    kprintf("ExynosUfs: invalid /arm-io/ufs reg or ranges\n");
    return false;
  }

  uint64_t hciPhys = bus[1] + regions[0] - bus[0];
  uint64_t vsPhys  = bus[1] + regions[2] - bus[0];
  uint64_t hciPage = hciPhys & ~0xFFFULL;
  uint64_t vsPage  = vsPhys & ~0xFFFULL;
  uint64_t hciOff  = hciPhys & 0xFFFULL;
  uint64_t vsOff   = vsPhys & 0xFFFULL;

  IOMemoryDescriptor *hciDesc = IOMemoryDescriptor::withPhysicalAddress(
      static_cast<IOPhysicalAddress>(hciPage), 0x1000, kIODirectionInOut);
  IOMemoryDescriptor *vsDesc = IOMemoryDescriptor::withPhysicalAddress(
      static_cast<IOPhysicalAddress>(vsPage), 0x1000, kIODirectionInOut);
  if (!hciDesc || !vsDesc) {
    if (hciDesc) hciDesc->release();
    if (vsDesc) vsDesc->release();
    kprintf("ExynosUfs: failed to create MMIO descriptors\n");
    return false;
  }
  hciMap_   = hciDesc->map(kIOMapAnywhere | kIOMapInhibitCache);
  vsHciMap_ = vsDesc->map(kIOMapAnywhere | kIOMapInhibitCache);
  hciDesc->release();
  vsDesc->release();
  if (!hciMap_ || !vsHciMap_) {
    kprintf("ExynosUfs: failed to map HCI / VS_HCI MMIO\n");
    return false;
  }
  hci_   = reinterpret_cast<volatile uint32_t *>(hciMap_->getVirtualAddress() + hciOff);
  vsHci_ = reinterpret_cast<volatile uint32_t *>(vsHciMap_->getVirtualAddress() + vsOff);
  kprintf("ExynosUfs: mapped hci=0x%llx vs=0x%llx\n",
          static_cast<unsigned long long>(hciPhys),
          static_cast<unsigned long long>(vsPhys));
  return true;
}

bool ExynosUfs::start(IOService *provider) {
  if (!IOService::start(provider)) {
    return false;
  }
  if (!mapRegisters()) {
    return false;
  }

  uint32_t ver = readHci(kRegVer);
  uint32_t cap = readHci(kRegCap);
  uint32_t hce = readHci(kRegHce);
  uint32_t hcs = readHci(kRegHcs);
  kprintf("ExynosUfs: VER=0x%x CAP=0x%x HCE=0x%x HCS=0x%x\n", ver, cap, hce, hcs);
  IOLog("ExynosUfs: VER=0x%x CAP=0x%x HCE=0x%x HCS=0x%x\n", ver, cap, hce, hcs);
  if ((hce & 1u) == 0 || (hcs & (kHcsDp | kHcsUtrlrdy)) != (kHcsDp | kHcsUtrlrdy)) {
    kprintf("ExynosUfs: link not ready from UEFI (HCE=0x%x HCS=0x%x)\n", hce, hcs);
    return false;
  }

  const IOOptionBits dmaOpts =
      static_cast<IOOptionBits>(kIODirectionInOut) |
      static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
  ringBuf_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, dmaOpts, 8192, 0x00000000FFFFF000ULL);
  bounceBuf_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, dmaOpts, kBounceBytes, 0x00000000FFFFF000ULL);
  if (!ringBuf_ || !bounceBuf_) {
    kprintf("ExynosUfs: DMA buffer allocation failed\n");
    return false;
  }
  if (ringBuf_->prepare(kIODirectionInOut) != kIOReturnSuccess ||
      bounceBuf_->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    kprintf("ExynosUfs: DMA buffer prepare failed\n");
    return false;
  }

  IOByteCount ringSegLen = 0;
  IOByteCount bounceSegLen = 0;
  utrdPhys_   = ringBuf_->getPhysicalSegment(0, &ringSegLen, kIOMemoryMapperNone);
  bouncePhys_ = bounceBuf_->getPhysicalSegment(0, &bounceSegLen, kIOMemoryMapperNone);
  if (!utrdPhys_ || ringSegLen < 8192 || !bouncePhys_ || bounceSegLen < kBounceBytes) {
    kprintf("ExynosUfs: DMA physical segments invalid (ring=%llx/%llu bounce=%llx/%llu)\n",
            static_cast<unsigned long long>(utrdPhys_),
            static_cast<unsigned long long>(ringSegLen),
            static_cast<unsigned long long>(bouncePhys_),
            static_cast<unsigned long long>(bounceSegLen));
    return false;
  }
  memset(ringBuf_->getBytesNoCopy(), 0, 8192);
  utrd_       = reinterpret_cast<ExynosUfsUtrd *>(ringBuf_->getBytesNoCopy());
  ucd_        = reinterpret_cast<ExynosUfsCmdDesc *>(
      reinterpret_cast<uint8_t *>(utrd_) + 4096);
  ucdPhys_    = utrdPhys_ + 4096;
  bounceVirt_ = reinterpret_cast<uint8_t *>(bounceBuf_->getBytesNoCopy());
  kprintf("ExynosUfs: DMA utrd=0x%llx ucd=0x%llx bounce=0x%llx\n",
          static_cast<unsigned long long>(utrdPhys_),
          static_cast<unsigned long long>(ucdPhys_),
          static_cast<unsigned long long>(bouncePhys_));

  // Drain any UEFI request, install our UTRL ring, and apply Exynos 9611 vendor setup
  for (uint32_t i = 0; i < 10000 && readHci(kRegUtrldbr) != 0; ++i) {
    IODelay(10);
  }
  writeHci(kRegUtrlrsr, 0u);
  for (uint32_t i = 0; i < 1000 && (readHci(kRegUtrlrsr) & 1u) != 0; ++i) {
    IODelay(10);
  }
  writeHci(kRegIe, 0u);
  writeHci(kRegIs, readHci(kRegIs));
  writeHci(kRegUtrlba, static_cast<uint32_t>(utrdPhys_ & 0xFFFFFFFFu));
  writeHci(kRegUtrlbau, static_cast<uint32_t>(utrdPhys_ >> 32));
  writeVs(kVsDataReorder, 0xAu);
  writeHci(kRegUtrlrsr, 1u);
  writeVs(kVsTxPrdtEntrySize, kSgBlockSizeBit);
  writeVs(kVsRxPrdtEntrySize, kSgBlockSizeBit);
  writeVs(kVsUtrlNexusType, 0xFFFFFFFFu);

  ioLock_ = IOLockAlloc();
  if (!ioLock_) {
    return false;
  }

  requestSense(0);
  if (!readCapacity(0)) {
    kprintf("ExynosUfs: READ CAPACITY on LUN 0 failed\n");
    return false;
  }
  kprintf("ExynosUfs: LUN 0 online: %llu blocks x %u bytes\n",
          static_cast<unsigned long long>(blockCount_), blockSize_);
  IOLog("ExynosUfs: LUN 0 online: %llu blocks x %u bytes\n",
        static_cast<unsigned long long>(blockCount_), blockSize_);
  parseShngroot();
  return true;
}

void ExynosUfs::stop(IOService *provider) {
  if (hci_) {
    writeHci(kRegUtrlrsr, 0u);
  }
  if (ioLock_) { IOLockFree(ioLock_); ioLock_ = nullptr; }
  if (bounceBuf_) { bounceBuf_->complete(); bounceBuf_->release(); bounceBuf_ = nullptr; }
  if (ringBuf_) { ringBuf_->complete(); ringBuf_->release(); ringBuf_ = nullptr; }
  if (vsHciMap_) { vsHciMap_->release(); vsHciMap_ = nullptr; }
  if (hciMap_) { hciMap_->release(); hciMap_ = nullptr; }
  IOService::stop(provider);
}

IOReturn ExynosUfs::submitScsi(uint8_t lun, const uint8_t cdb[16],
                               bool isWrite, uint32_t xferBytes) {
  if (xferBytes > kBounceBytes) {
    return kIOReturnBadArgument;
  }
  memset(utrd_, 0, sizeof(ExynosUfsUtrd));
  memset(ucd_, 0, sizeof(ExynosUfsCmdDesc));

  // Build Command UPIU (matches UfsDxe.c UfsWriteCmdUcd)
  ucd_->command_upiu.header.type  = 0x01u; // UPIU_TRANSACTION_COMMAND
  ucd_->command_upiu.header.flags = (xferBytes > 0) ? (isWrite ? 0x20u : 0x40u) : 0x00u;
  ucd_->command_upiu.header.lun   = lun;
  ucd_->command_upiu.header.tag   = 0u;
  uint32_t dlenBe = OSSwapHostToBigInt32(xferBytes);
  memcpy(&ucd_->command_upiu.tsf[0], &dlenBe, sizeof(dlenBe));
  memcpy(&ucd_->command_upiu.tsf[4], cdb, 16);

  // Build 4 KiB PRDT segments (matches UfsDxe.c UfsMapSg)
  uint32_t sgSegs = 0;
  if (xferBytes > 0) {
    sgSegs = (xferBytes + kSgBlockSize - 1u) / kSgBlockSize;
    for (uint32_t i = 0; i < sgSegs; ++i) {
      uint64_t phys = bouncePhys_ + static_cast<uint64_t>(i) * kSgBlockSize;
      uint32_t rem  = xferBytes - i * kSgBlockSize;
      uint32_t seg  = (rem < kSgBlockSize) ? rem : kSgBlockSize;
      ucd_->prd_table[i].base_addr  = static_cast<uint32_t>(phys & 0xFFFFFFFFu);
      ucd_->prd_table[i].upper_addr = static_cast<uint32_t>(phys >> 32);
      ucd_->prd_table[i].reserved   = 0u;
      ucd_->prd_table[i].size       = seg - 1u;
    }
  }

  // Build UTRD in Exynos byte units (matches UfsDxe.c UfsUtpInit + UfsWriteUtrd)
  uint32_t dd = kUtpNoData;
  if (xferBytes > 0) {
    dd = isWrite ? kUtpHostToDevice : kUtpDeviceToHost;
  }
  utrd_->dw[0]           = dd | kUtpScsiCommand | kUtpReqDescIntCmd;
  utrd_->dw[2]           = 0x0Fu; // OCS_INVALID_COMMAND_STATUS
  utrd_->cmd_desc_addr_l = static_cast<uint32_t>(ucdPhys_ & 0xFFFFFFFFu);
  utrd_->cmd_desc_addr_h = static_cast<uint32_t>(ucdPhys_ >> 32);
  utrd_->rsp_upiu_len    = kAlignedUpiuSize;
  utrd_->rsp_upiu_off    = kAlignedUpiuSize;
  if (xferBytes > 0) {
    utrd_->prdt_len      = static_cast<uint16_t>(sgSegs * sizeof(ExynosUfsPrdt));
    utrd_->prdt_off      = kAlignedUpiuSize * 2u;
  } else {
    utrd_->prdt_len      = 0u;
    utrd_->prdt_off      = 0u;
  }

  // Clean+invalidate DMA structures and bounce buffer before ringing doorbell
  exynosUfsFlushCache(utrd_, sizeof(ExynosUfsUtrd));
  exynosUfsFlushCache(ucd_, sizeof(ExynosUfsCmdDesc));
  if (xferBytes > 0) {
    exynosUfsFlushCache(bounceVirt_, xferBytes);
  }

  // Clear pending IS, ensure SCSI nexus type, ring slot 0
  uint32_t staleIs = readHci(kRegIs);
  if (staleIs) {
    writeHci(kRegIs, staleIs);
  }
  writeVs(kVsUtrlNexusType, 0xFFFFFFFFu);
  writeHci(kRegUtrldbr, 1u);

  for (uint32_t i = 0; i < 500000; ++i) {
    uint32_t is = readHci(kRegIs);
    if (is & kIsFatalMask) {
      writeHci(kRegIs, is);
      kprintf("ExynosUfs: fatal IS=0x%x cdb0=0x%02x\n", is, cdb[0]);
      return kIOReturnIOError;
    }
    if ((is & kIsUtrcs) != 0 && (readHci(kRegUtrldbr) & 1u) == 0) {
      writeHci(kRegIs, is);
      exynosUfsFlushCache(utrd_, sizeof(ExynosUfsUtrd));
      exynosUfsFlushCache(&ucd_->response_upiu, sizeof(ExynosUfsUpiu));
      if (!isWrite && xferBytes > 0) {
        exynosUfsFlushCache(bounceVirt_, xferBytes);
      }
      uint32_t ocs    = utrd_->dw[2] & 0xFFu;
      uint8_t  resp   = ucd_->response_upiu.header.response;
      uint8_t  scsiSt = ucd_->response_upiu.header.status;
      if (ocs != 0 || resp != 0 || scsiSt != 0) {
        kprintf("ExynosUfs: SCSI err cdb0=0x%02x ocs=0x%x resp=0x%x st=0x%x\n",
                cdb[0], ocs, resp, scsiSt);
        return kIOReturnIOError;
      }
      return kIOReturnSuccess;
    }
    IODelay(2);
  }

  uint32_t lastIs = readHci(kRegIs);
  uint32_t lastDbr = readHci(kRegUtrldbr);
  writeHci(kRegUtrlclr, ~1u);
  if (lastIs) {
    writeHci(kRegIs, lastIs);
  }
  kprintf("ExynosUfs: SCSI timeout cdb0=0x%02x IS=0x%x DBR=0x%x\n",
          cdb[0], lastIs, lastDbr);
  return kIOReturnTimeout;
}

void ExynosUfs::requestSense(uint8_t lun) {
  uint8_t cdb[16] = {};
  cdb[0] = 0x03u; // REQUEST SENSE
  cdb[4] = 18u;
  memset(bounceVirt_, 0, 18);
  (void)submitScsi(lun, cdb, false, 18u);
}

bool ExynosUfs::readCapacity(uint8_t lun) {
  // First try READ CAPACITY(10) (exact command UfsDxe uses on SM-A515F)
  uint8_t cdb10[16] = {};
  cdb10[0] = 0x25u;
  if (submitScsi(lun, cdb10, false, 8u) == kIOReturnSuccess) {
    uint32_t lastLba32 = OSReadBigInt32(bounceVirt_, 0);
    uint32_t blkLen    = OSReadBigInt32(bounceVirt_, 4);
    if (lastLba32 != 0xFFFFFFFFu && blkLen >= 512u) {
      blockCount_ = static_cast<uint64_t>(lastLba32) + 1ULL;
      blockSize_  = blkLen;
      return true;
    }
  }
  // Fallback to READ CAPACITY(16)
  uint8_t cdb16[16] = {};
  cdb16[0]  = 0x9Eu; // SERVICE ACTION IN(16)
  cdb16[1]  = 0x10u; // READ CAPACITY(16)
  cdb16[13] = 32u;
  if (submitScsi(lun, cdb16, false, 32u) != kIOReturnSuccess) {
    return false;
  }
  uint64_t lastLba = OSReadBigInt64(bounceVirt_, 0);
  uint32_t blkLen  = OSReadBigInt32(bounceVirt_, 8);
  if (blkLen < 512u) {
    return false;
  }
  blockCount_ = lastLba + 1ULL;
  blockSize_  = blkLen;
  return true;
}

IOReturn ExynosUfs::readWindowBytes(uint64_t byteOffset, uint64_t byteLen,
                                    IOMemoryDescriptor *dst) {
  IOLockLock(ioLock_);
  IOReturn status = kIOReturnSuccess;
  uint64_t done = 0;
  uint32_t maxChunkBlks = kBounceBytes / blockSize_;
  while (done < byteLen) {
    uint64_t curByte = byteOffset + done;
    uint64_t lba = curByte / blockSize_;
    uint32_t inBlkOff = static_cast<uint32_t>(curByte % blockSize_);
    uint64_t remBytes = byteLen - done;
    uint64_t needBlks64 = (inBlkOff + remBytes + blockSize_ - 1u) / blockSize_;
    uint32_t needBlks = (needBlks64 > maxChunkBlks)
        ? maxChunkBlks
        : static_cast<uint32_t>(needBlks64);

    uint8_t cdb[16] = {};
    cdb[0] = 0x28u; // READ(10)
    OSWriteBigInt32(cdb, 2, static_cast<uint32_t>(lba));
    OSWriteBigInt16(cdb, 7, static_cast<uint16_t>(needBlks));
    status = submitScsi(0, cdb, false, needBlks * blockSize_);
    if (status != kIOReturnSuccess) {
      break;
    }
    uint32_t avail   = needBlks * blockSize_ - inBlkOff;
    uint32_t copyLen = (remBytes < avail) ? static_cast<uint32_t>(remBytes) : avail;
    IOByteCount written = dst->writeBytes(done, bounceVirt_ + inBlkOff, copyLen);
    if (written != copyLen) {
      status = kIOReturnIOError;
      break;
    }
    done += copyLen;
  }
  IOLockUnlock(ioLock_);
  return status;
}

void ExynosUfs::parseShngroot() {
  // Measured SM-A515F 128 GB UFS userdata (sda32) first LBA = 2061568 (4096 B/LBA)
  constexpr uint64_t kUserdataFirstLba = 2061568ULL;
  uint8_t *hdr = static_cast<uint8_t *>(IOMallocZero(4096));
  if (!hdr) {
    kprintf("ExynosUfs: out of memory allocating SHNGROOT header\n");
    return;
  }

  uint8_t cdb[16] = {};
  cdb[0] = 0x28u; // READ(10)
  OSWriteBigInt32(cdb, 2, static_cast<uint32_t>(kUserdataFirstLba));
  OSWriteBigInt16(cdb, 7, 1u);

  IOLockLock(ioLock_);
  IOReturn st = submitScsi(0, cdb, false, blockSize_);
  if (st == kIOReturnSuccess) {
    memcpy(hdr, bounceVirt_, 4096);
  }
  IOLockUnlock(ioLock_);

  if (st != kIOReturnSuccess) {
    kprintf("ExynosUfs: failed to read SHNGROOT manifest at LBA %llu (0x%x)\n",
            static_cast<unsigned long long>(kUserdataFirstLba),
            static_cast<unsigned>(st));
    IOFree(hdr, 4096);
    return;
  }
  if (memcmp(hdr, "SHNGROOT", 8) != 0) {
    kprintf("ExynosUfs: no SHNGROOT magic at LBA %llu (got %02x%02x%02x%02x)\n",
            static_cast<unsigned long long>(kUserdataFirstLba),
            hdr[0], hdr[1], hdr[2], hdr[3]);
    IOFree(hdr, 4096);
    return;
  }

  uint8_t hdrHash[32] = {};
  SHA256_CTX hctx;
  SHA256_Init(&hctx);
  SHA256_Update(&hctx, hdr, 4064);
  SHA256_Final(hdrHash, &hctx);
  if (memcmp(hdrHash, hdr + 4064, 32) != 0) {
    kprintf("ExynosUfs: SHNGROOT header SHA-256 mismatch!\n");
    IOFree(hdr, 4096);
    return;
  }

  uint32_t version = OSReadLittleInt32(hdr, 8);
  uint32_t count   = OSReadLittleInt32(hdr, 12);
  if (count > 16u) {
    count = 16u;
  }
  kprintf("ExynosUfs: SHNGROOT v%u verified (%u entries)\n", version, count);
  IOLog("ExynosUfs: SHNGROOT v%u verified (%u entries)\n", version, count);

  uint64_t udBaseByte = kUserdataFirstLba * blockSize_;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *e = hdr + 24 + i * 128;
    char wname[33] = {};
    memcpy(wname, e, 32);
    uint64_t woff  = OSReadLittleInt64(e, 32);
    uint64_t wsize = OSReadLittleInt64(e, 40);
    uint32_t wblk  = OSReadLittleInt32(e, 48);
    uint32_t wflg  = OSReadLittleInt32(e, 52);
    const uint8_t *wsha = e + 56;
    if (wflg != 1u || wsize == 0) {
      continue;
    }
    kprintf("ExynosUfs: publishing window '%s' off=0x%llx size=0x%llx blk=%u\n",
            wname, static_cast<unsigned long long>(woff),
            static_cast<unsigned long long>(wsize), wblk);
    IOLog("ExynosUfs: publishing window '%s' off=0x%llx size=0x%llx blk=%u\n",
          wname, static_cast<unsigned long long>(woff),
          static_cast<unsigned long long>(wsize), wblk);
    ExynosUfsWindow *win = OSTypeAlloc(ExynosUfsWindow);
    if (win && win->initWindow(this, wname, udBaseByte + woff, wsize, wblk, wsha)) {
      if (win->attach(this)) {
        if (win->start(this)) {
          win->registerService();
        } else {
          win->detach(this);
        }
      }
    }
    if (win) {
      win->release();
    }
  }
  IOFree(hdr, 4096);
}

static ExynosUfsWindow *exynosUfsFindWindowForDev(dev_t device) {
  if (device == NODEV) {
    return nullptr;
  }
  uint32_t wantMaj = static_cast<uint32_t>(major(device));
  uint32_t wantMin = static_cast<uint32_t>(minor(device));
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  if (!matching) {
    return nullptr;
  }
  OSIterator *it = IOService::getMatchingServices(matching);
  matching->release();
  if (!it) {
    return nullptr;
  }
  ExynosUfsWindow *found = nullptr;
  while (OSObject *obj = it->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, obj);
    if (!media) {
      continue;
    }
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!mjr || !mnr ||
        mjr->unsigned32BitValue() != wantMaj ||
        mnr->unsigned32BitValue() != wantMin) {
      continue;
    }
    unsigned depth = 0;
    for (IOService *prov = media; prov && depth++ < 64; prov = prov->getProvider()) {
      ExynosUfsWindow *win = OSDynamicCast(ExynosUfsWindow, prov);
      if (win) {
        win->retain();
        found = win;
        break;
      }
    }
    if (found) {
      break;
    }
  }
  it->release();
  return found;
}

extern "C" int native_verified_block_root_authorized(dev_t device) {
  ExynosUfsWindow *win = exynosUfsFindWindowForDev(device);
  if (!win) {
    kprintf("ExynosUfs: root_authorized(dev=0x%x) -> 0\n", static_cast<unsigned>(device));
    return 0;
  }
  kprintf("ExynosUfs: root_authorized(dev=0x%x) -> 1 ('%s')\n",
          static_cast<unsigned>(device), win->windowName());
  win->release();
  return 1;
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device,
                                                   char *path, size_t size) {
  if (!uuid || strlen(uuid) != 36 || !device || !path || size < 6) {
    return 0;
  }
  *device = NODEV;
  path[0] = 0;

  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    OSDictionary *matching   = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString     *wanted     = OSString::withCString(uuid);
    if (!matching || !properties || !wanted) {
      OSSafeReleaseNULL(wanted);
      OSSafeReleaseNULL(properties);
      OSSafeReleaseNULL(matching);
      return 0;
    }
    bool ready = properties->setObject("UUID", wanted) &&
                 matching->setObject("IOPropertyMatch", properties);
    wanted->release();
    properties->release();
    OSIterator *it = ready ? IOService::getMatchingServices(matching) : nullptr;
    matching->release();
    if (!it) {
      return 0;
    }
    bool selected = false;
    unsigned matches = 0;
    dev_t candidate = NODEV;
    char candidatePath[64] = {};
    while (OSObject *obj = it->getNextObject()) {
      IOService *media = OSDynamicCast(IOService, obj);
      if (!media) {
        continue;
      }
      if (++matches != 1) {
        selected = false;
        break;
      }
      OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
      OSString *name     = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
      OSNumber *mjr      = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
      OSNumber *mnr      = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
      if (!identity || strcmp(identity->getCStringNoCopy(), uuid) != 0 ||
          !name || !mjr || !mnr || !name->getLength() ||
          name->getLength() >= sizeof(candidatePath) - 6 ||
          name->getLength() >= size - 6 ||
          mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) {
        continue;
      }
      unsigned depth = 0;
      for (IOService *prov = media; prov && depth++ < 64; prov = prov->getProvider()) {
        if (OSDynamicCast(ExynosUfsWindow, prov) != nullptr) {
          candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
          snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
          selected = true;
          break;
        }
      }
    }
    it->release();
    if (selected && matches == 1) {
      *device = candidate;
      strlcpy(path, candidatePath, size);
      kprintf("ExynosUfs: volume_device(%s) -> %s (dev=0x%x)\n",
              uuid, path, static_cast<unsigned>(candidate));
      return 1;
    }
    IOSleep(50);
  }
  kprintf("ExynosUfs: volume_device(%s) -> NOT FOUND\n", uuid);
  return 0;
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid,
                                                     uint8_t digest[48]) {
  if (!uuid || !digest) {
    return 0;
  }
  ExynosUfsWindow *win = exynosUfsFindWindowForDev(device);
  if (!win) {
    return 0;
  }
  static const char domain[] = "ExynosUfs.SHNGROOT.v1";
  SHA384_CTX ctx;
  SHA384_Init(&ctx);
  SHA384_Update(&ctx, domain, sizeof(domain));
  SHA384_Update(&ctx, win->windowName(), strlen(win->windowName()) + 1);
  SHA384_Update(&ctx, win->windowSha256(), 32);
  SHA384_Update(&ctx, uuid, strlen(uuid));
  SHA384_Final(digest, &ctx);
  win->release();
  return 1;
}

extern "C" void exynos_ufs_try_attach(void *fallbackProvider) {
  static bool attempted = false;
  if (attempted) {
    return;
  }
  attempted = true;

  IORegistryEntry *entry = IORegistryEntry::fromPath("/arm-io/ufs", gIODTPlane);
  IOService *provider = OSDynamicCast(IOService, entry);
  bool releaseProvider = (provider != nullptr);
  if (!provider) {
    if (entry) {
      entry->release();
    }
    provider = reinterpret_cast<IOService *>(fallbackProvider);
  }
  if (!provider) {
    kprintf("ExynosUfs: no provider available\n");
    return;
  }
  ExynosUfs *drv = OSTypeAlloc(ExynosUfs);
  if (drv && drv->init(nullptr)) {
    if (drv->attach(provider)) {
      if (!drv->start(provider)) {
        drv->detach(provider);
      }
    }
  }
  if (drv) {
    drv->release();
  }
  if (releaseProvider) {
    provider->release();
  }
}
