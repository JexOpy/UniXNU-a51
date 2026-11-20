# Phase 0: On-Device Hardware Verification for SM-A515F

Before flashing any custom UEFI or XNU image to your Samsung Galaxy A51 (`SM-A515F`, 6 GB / 128 GB), collect measured hardware values from your actual unit and keep a complete stock backup.

## 1. Prerequisites on SM-A515F

1. Bootloader unlocked (OEM Unlock enabled in Developer Options and unlocked in Download mode).
2. Either **TWRP** installed (recommended: gives direct root ADB shell and safe recovery backups) or rooted Android (Magisk) with USB debugging enabled.
3. **Never overwrite `boot` first.** Flash bring-up images to the **`recovery`** partition (`71,106,560` bytes / 67.8 MiB) so stock Android remains bootable if UEFI hangs. Keep a stock `recovery.tar` / TWRP `.tar` ready in Odin to restore at any time.

## 2. Run the Phase-0 ADB Dump

On Windows PowerShell (with SM-A515F connected in TWRP or rooted Android):

```powershell
cd C:\Users\Admin\Documents\Unixnu\UniXNU-a51
powershell -ExecutionPolicy Bypass -File .\scripts\dump-a51-hardware.ps1
```

On Linux / macOS / WSL:

```sh
./scripts/dump-a51-hardware.sh
```

This creates a git-ignored directory `phase0/<timestamp>/` containing:
- `iomem.txt` — exact DRAM banks, TrustZone secure holes, and MMIO ranges
- `dmesg.txt` and `dmesg-filtered.txt` — bootloader framebuffer address, panel type (`s6e3fa9` vs `ea8076`), UFS init, and memory reservations
- `gpt-sda.txt` & `partition-bytes.txt` — exact `userdata` `first_lba` / `last_lba` (4096-byte UFS blocks) and exact `boot` / `recovery` byte limits
- `a51-live.dtb` — live FDT handed off by S-Boot (decompile with `dtc -I dtb -O dts a51-live.dtb`)
- `stock-boot.img`, `stock-recovery.img`, `stock-dtbo.img`, `stock-vbmeta.img` — safety backups and stock Android boot header reference

## 3. Values to Verify & Update

Compare your dump against these files before your first flash:

| Measurement | File to check/update |
| --- | --- |
| `System RAM` banks & TZ holes (`0xBAB00000`, `0xE0000000`) | `dts/a51.dts` and `uefi-a51/Platforms/Samsung/a51Pkg/Library/MemoryMapLib/MemoryMapLib.c` |
| Bootloader simple framebuffer base (expected `0xCA000000`) | `uefi-a51/Platforms/Samsung/a51Pkg/Library/MemoryMapLib/MemoryMapLib.c` (`Display Reserved`) and `dts/a51.dts` |
| `userdata` `first_lba`, `last_lba`, `lba_bytes` (`4096`) | `board.toml` `[userdata]` |
| `boot` / `recovery` exact byte sizes & stock `boot.img` header version (`v1` vs `v2`) | `board.toml` `[bootimg.image]`, `[package]`, and `uefi-a51/Resources/Configs/a51.toml` |
| Stock DTB (`a51-live.dtb` or DTB unpacked from `stock-boot.img`) | Passed via `--dtb` during `unixnu bootimg` |

## 4. Flashing via Odin (Samsung Download Mode)

Samsung Exynos devices do **not** have fastboot. Pack any `.img` into an Odin `.tar`:

```sh
python3 scripts/pack-odin-tar.py --input Mu-a51-0.img --partition recovery --output a51-uefi-recovery.tar
```

Flash `a51-uefi-recovery.tar` in the **AP** slot of Odin (with **Auto Reboot** unchecked so you can immediately hold **Volume Up + Power** while USB is connected to enter `recovery` directly without Android restoring stock recovery).
