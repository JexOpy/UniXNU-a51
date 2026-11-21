// SPDX-License-Identifier: BSD-2-Clause
// Exynos 9611 (S5E9611) UFSHCI 2.1 IOBlockStorageDevice driver for UniXNU.
// Assumes uefi-a51 (PlatformUfsLib / UfsPassThruDxe) has already powered on
// the UFS PHY and established the UniPro link (HCE=1, HCS.DP=1, HCS.UCRDY=1).

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSByteOrder.h>

namespace {

// Standard JEDEC UFSHCI 2.1 registers (base 0x13520000)
constexpr uint32_t REG_CAP      = 0x00;
constexpr uint32_t REG_VER      = 0x08;
constexpr uint32_t REG_IS       = 0x20;
constexpr uint32_t REG_IE       = 0x24;
constexpr uint32_t REG_HCS      = 0x30;
constexpr uint32_t REG_HCE      = 0x34;
constexpr uint32_t REG_UTRLBA   = 0x50;
constexpr uint32_t REG_UTRLBAU  = 0x54;
constexpr uint32_t REG_UTRLDBR  = 0x58;
constexpr uint32_t REG_UTRLCLR  = 0x5C;
constexpr uint32_t REG_UTRLRSR  = 0x60;

// Exynos vendor-specific HCI registers (base 0x13521100)
constexpr uint32_t VS_TXPRDT_ENTRY_SIZE = 0x00;
constexpr uint32_t VS_RXPRDT_ENTRY_SIZE = 0x04;
constexpr uint32_t VS_UTRL_NEXUS_TYPE   = 0x40;

constexpr uint32_t HCS_DP    = (1u << 0);
constexpr uint32_t HCS_UCRDY = (1u << 1);

constexpr uint32_t kUfsBlockSize   = 4096;
constexpr uint32_t kBounceBytes    = 64 * 1024; // 16 blocks per transfer
constexpr uint32_t kMaxPrdtEntries = 16;

struct __attribute__((packed)) Utrd {
  uint32_t dw0;      // [28:25]=DD, [31:28]=CT (1=UFS storage)
  uint32_t dw1;
  uint32_t ocs;      // Overall Command Status (0x0 = success, 0xF = invalid)
  uint32_t dw3;
  uint32_t ucdba;    // UCD base address low (128B aligned)
  uint32_t ucdbau;   // UCD base address high
  uint16_t resp_len; // Response UPIU length in dwords
  uint16_t resp_off; // Response UPIU offset in dwords
  uint16_t prdt_len; // Number of PRDT entries
  uint16_t prdt_off; // PRDT offset in dwords
};

struct __attribute__((packed)) PrdtEntry {
  uint32_t base_lo;
  uint32_t base_hi;
  uint32_t reserved;
  uint32_t size; // byte count - 1
};

struct __attribute__((packed)) CommandUpiu {
  uint8_t  txn_type; // 0x01
  uint8_t  flags;    // bit 6 = R, bit 5 = W
  uint8_t  lun;
  uint8_t  task_tag;
  uint8_t  iid_cmdset;
  uint8_t  reserved0[3];
  uint8_t  ehs_len;
  uint8_t  reserved1;
  uint16_t data_seg_len;
  uint32_t expected_xfer_len_be;
  uint8_t  cdb[16];
};

struct __attribute__((packed)) Ucd {
  CommandUpiu cmd;
  uint8_t     cmd_pad[128 - sizeof(CommandUpiu)];
  uint8_t     resp[128];
  PrdtEntry   prdt[kMaxPrdtEntries];
};

static_assert(sizeof(Utrd) == 32, "UTRD must be 32 bytes");
static_assert(sizeof(PrdtEntry) == 16, "PRDT entry must be 16 bytes");

} // namespace

class ExynosUfs : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(ExynosUfs)

private:
  IOMemoryMap *hciMap_   = nullptr;
  IOMemoryMap *vsHciMap_ = nullptr;
  volatile uint32_t *hci_   = nullptr;
  volatile uint32_t *vsHci_ = nullptr;

  IOBufferMemoryDescriptor *ringBuf_   = nullptr;
  IOBufferMemoryDescriptor *bounceBuf_ = nullptr;
  IOLock *ioLock_ = nullptr;

  Utrd *utrd_ = nullptr;
  Ucd  *ucd_  = nullptr;
  uint64_t utrdPhys_   = 0;
  uint64_t ucdPhys_    = 0;
  uint64_t bouncePhys_ = 0;
  uint8_t *bounceVirt_ = nullptr;

  uint64_t blockCount_ = 0;
  uint32_t blockSize_  = kUfsBlockSize;

  inline uint32_t readHci(uint32_t off) const {
    return hci_[off >> 2];
  }
  inline void writeHci(uint32_t off, uint32_t val) {
    hci_[off >> 2] = val;
    __builtin_arm_dsb(0xf);
  }
  inline void writeVs(uint32_t off, uint32_t val) {
    vsHci_[off >> 2] = val;
    __builtin_arm_dsb(0xf);
  }

  IOReturn submitScsi(uint8_t lun, const uint8_t cdb[16],
                      bool isWrite, uint32_t xferBytes);
  bool readCapacity16(uint8_t lun);

public:
  bool start(IOService *provider) override;
  void stop(IOService *provider) override;

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32   doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  IOReturn doLockUnlockMedia(bool) override { return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() { return kIOReturnSuccess; }
  char    *getVendorString() override { return const_cast<char *>("Samsung"); }
  char    *getProductString() override { return const_cast<char *>("Exynos9611-UFS"); }
  char    *getRevisionString() override { return const_cast<char *>("2.0"); }
  char    *getAdditionalDeviceInfoString() override { return const_cast<char *>("UFSHCI"); }
  IOReturn reportBlockSize(UInt64 *blockSize) override {
    *blockSize = blockSize_;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *isEjectable) override {
    *isEjectable = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *maxBlock) override {
    *maxBlock = blockCount_ > 0 ? (blockCount_ - 1) : 0;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *mediaPresent, bool *changedState) override {
    *mediaPresent = (blockCount_ > 0);
    *changedState = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *isRemovable) override {
    *isRemovable = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *isWriteProtected) override {
    *isWriteProtected = false;
    return kIOReturnSuccess;
  }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                            UInt64 nblks, IOStorageAttributes *attributes,
                            IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(ExynosUfs, IOBlockStorageDevice)

bool ExynosUfs::start(IOService *provider) {
  if (!IOBlockStorageDevice::start(provider)) {
    return false;
  }
  hciMap_ = provider->mapDeviceMemoryWithIndex(0);
  vsHciMap_ = provider->mapDeviceMemoryWithIndex(1);
  if (!hciMap_ || !vsHciMap_) {
    IOLog("ExynosUfs: failed to map HCI / VS_HCI MMIO\n");
    return false;
  }
  hci_ = reinterpret_cast<volatile uint32_t *>(hciMap_->getVirtualAddress());
  vsHci_ = reinterpret_cast<volatile uint32_t *>(vsHciMap_->getVirtualAddress());

  uint32_t hce = readHci(REG_HCE);
  uint32_t hcs = readHci(REG_HCS);
  IOLog("ExynosUfs: VER=0x%x CAP=0x%x HCE=0x%x HCS=0x%x\n",
        readHci(REG_VER), readHci(REG_CAP), hce, hcs);
  if ((hce & 1u) == 0 || (hcs & (HCS_DP | HCS_UCRDY)) != (HCS_DP | HCS_UCRDY)) {
    IOLog("ExynosUfs: link not ready from UEFI (HCE=0x%x HCS=0x%x)\n", hce, hcs);
    return false;
  }

  const IOOptionBits dmaOpts =
      static_cast<IOOptionBits>(kIODirectionInOut) |
      static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
  ringBuf_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, dmaOpts,
      8192, 0x00000000FFFFF000ULL);
  bounceBuf_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
      kernel_task, dmaOpts,
      kBounceBytes, 0x00000000FFFFF000ULL);
  if (!ringBuf_ || !bounceBuf_) {
    IOLog("ExynosUfs: DMA buffer allocation failed\n");
    return false;
  }
  bzero(ringBuf_->getBytesNoCopy(), 8192);
  utrd_ = reinterpret_cast<Utrd *>(ringBuf_->getBytesNoCopy());
  ucd_  = reinterpret_cast<Ucd *>(reinterpret_cast<uint8_t *>(utrd_) + 4096);
  utrdPhys_   = ringBuf_->getPhysicalAddress();
  ucdPhys_    = utrdPhys_ + 4096;
  bounceVirt_ = reinterpret_cast<uint8_t *>(bounceBuf_->getBytesNoCopy());
  bouncePhys_ = bounceBuf_->getPhysicalAddress();

  // Stop existing UTRL, install our 1-slot UTRL ring, and re-enable
  writeHci(REG_UTRLRSR, 0);
  writeHci(REG_IE, 0);
  writeHci(REG_IS, readHci(REG_IS));
  writeVs(VS_TXPRDT_ENTRY_SIZE, kUfsBlockSize);
  writeVs(VS_RXPRDT_ENTRY_SIZE, kUfsBlockSize);
  writeVs(VS_UTRL_NEXUS_TYPE, 0xFFFFFFFFu);
  writeHci(REG_UTRLBA, static_cast<uint32_t>(utrdPhys_ & 0xFFFFFFFFu));
  writeHci(REG_UTRLBAU, static_cast<uint32_t>(utrdPhys_ >> 32));
  writeHci(REG_UTRLRSR, 1);

  ioLock_ = IOLockAlloc();
  if (!readCapacity16(0)) {
    IOLog("ExynosUfs: READ CAPACITY(16) on LUN 0 failed\n");
    return false;
  }
  IOLog("ExynosUfs: LUN 0 online: %llu blocks x %u bytes\n",
        (unsigned long long)blockCount_, blockSize_);
  registerService();
  return true;
}

void ExynosUfs::stop(IOService *provider) {
  if (hci_) {
    writeHci(REG_UTRLRSR, 0);
  }
  if (ioLock_) { IOLockFree(ioLock_); ioLock_ = nullptr; }
  if (bounceBuf_) { bounceBuf_->release(); bounceBuf_ = nullptr; }
  if (ringBuf_) { ringBuf_->release(); ringBuf_ = nullptr; }
  if (vsHciMap_) { vsHciMap_->release(); vsHciMap_ = nullptr; }
  if (hciMap_) { hciMap_->release(); hciMap_ = nullptr; }
  IOBlockStorageDevice::stop(provider);
}

IOReturn ExynosUfs::submitScsi(uint8_t lun, const uint8_t cdb[16],
                               bool isWrite, uint32_t xferBytes) {
  bzero(utrd_, sizeof(Utrd));
  bzero(ucd_, sizeof(Ucd));

  uint32_t dd = 0;
  if (xferBytes > 0) {
    dd = isWrite ? (1u << 25) : (2u << 25); // 01b=Sys->Dev, 10b=Dev->Sys
  }
  utrd_->dw0      = (1u << 28) | dd; // CT=1 (UFS Storage)
  utrd_->ocs      = 0xFu;
  utrd_->ucdba    = static_cast<uint32_t>(ucdPhys_ & 0xFFFFFFFFu);
  utrd_->ucdbau   = static_cast<uint32_t>(ucdPhys_ >> 32);
  utrd_->resp_off = 128 / 4;
  utrd_->resp_len = 128 / 4;
  utrd_->prdt_off = 256 / 4;

  if (xferBytes > 0) {
    utrd_->prdt_len = 1;
    ucd_->prdt[0].base_lo  = static_cast<uint32_t>(bouncePhys_ & 0xFFFFFFFFu);
    ucd_->prdt[0].base_hi  = static_cast<uint32_t>(bouncePhys_ >> 32);
    ucd_->prdt[0].reserved = 0;
    ucd_->prdt[0].size     = xferBytes - 1;
  }

  ucd_->cmd.txn_type = 0x01;
  ucd_->cmd.flags    = (xferBytes > 0) ? (isWrite ? 0x20 : 0x40) : 0x00;
  ucd_->cmd.lun      = lun;
  ucd_->cmd.task_tag = 0;
  ucd_->cmd.expected_xfer_len_be = OSSwapHostToBigInt32(xferBytes);
  bcopy(cdb, ucd_->cmd.cdb, 16);

  __builtin_arm_dsb(0xf);
  writeHci(REG_UTRLDBR, 1u);

  for (uint32_t i = 0; i < 500000; ++i) {
    if ((readHci(REG_UTRLDBR) & 1u) == 0) {
      __builtin_arm_dsb(0xf);
      if ((utrd_->ocs & 0xFFu) != 0) {
        return kIOReturnIOError;
      }
      return kIOReturnSuccess;
    }
    IODelay(2);
  }
  writeHci(REG_UTRLCLR, ~1u);
  return kIOReturnTimeout;
}

bool ExynosUfs::readCapacity16(uint8_t lun) {
  uint8_t cdb[16] = {};
  cdb[0]  = 0x9E; // SERVICE ACTION IN(16)
  cdb[1]  = 0x10; // READ CAPACITY(16)
  cdb[13] = 32;
  if (submitScsi(lun, cdb, false, 32) != kIOReturnSuccess) {
    return false;
  }
  uint64_t lastLba = OSReadBigInt64(bounceVirt_, 0);
  uint32_t blkLen  = OSReadBigInt32(bounceVirt_, 8);
  if (blkLen == 0) {
    return false;
  }
  blockCount_ = lastLba + 1;
  blockSize_  = blkLen;
  return true;
}

IOReturn ExynosUfs::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                                     UInt64 nblks, IOStorageAttributes *,
                                     IOStorageCompletion *completion) {
  bool isWrite = (buffer->getDirection() == kIODirectionOut);
  UInt64 doneBytes = 0;
  IOReturn status = kIOReturnSuccess;

  IOLockLock(ioLock_);
  UInt64 remaining = nblks;
  UInt64 curLba = block;
  while (remaining > 0) {
    UInt32 chunkBlks = static_cast<UInt32>(
        remaining > (kBounceBytes / blockSize_) ? (kBounceBytes / blockSize_) : remaining);
    UInt32 chunkBytes = chunkBlks * blockSize_;

    if (isWrite) {
      buffer->readBytes(doneBytes, bounceVirt_, chunkBytes);
    }
    uint8_t cdb[16] = {};
    cdb[0] = isWrite ? 0x2A : 0x28; // WRITE(10) / READ(10)
    OSWriteBigInt32(cdb, 2, static_cast<uint32_t>(curLba));
    OSWriteBigInt16(cdb, 7, static_cast<uint16_t>(chunkBlks));

    status = submitScsi(0, cdb, isWrite, chunkBytes);
    if (status != kIOReturnSuccess) {
      break;
    }
    if (!isWrite) {
      buffer->writeBytes(doneBytes, bounceVirt_, chunkBytes);
    }
    doneBytes += chunkBytes;
    curLba    += chunkBlks;
    remaining -= chunkBlks;
  }
  IOLockUnlock(ioLock_);

  IOStorage::complete(completion, status, doneBytes);
  return kIOReturnSuccess;
}
