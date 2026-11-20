# UniXNU-a51

UniXNU Format 2 device port for the **Samsung Galaxy A51 (`SM-A515F`, codename `a51`, Exynos 9611 / `S5E9611`, 6 GB RAM / 128 GB UFS)**.

> **Current status: initial Format 2 scaffold & UEFI bring-up base (`planned`).**
> Hardware addresses and partition bounds are derived from public Samsung SM-A515F kernel DTS, LineageOS/TWRP `BoardConfig.mk`, and `Sota4Ever/Mu-Silicium` (`S5E9610Pkg` / Galaxy A50). Complete [`PHASE0.md`](PHASE0.md) on your device before the first flash.

- Authoritative porting record: [`porting.toml`](porting.toml)
- On-device verification checklist: [`PHASE0.md`](PHASE0.md)
- Shared SoC layer: `../UniXNU-exynos9611-common`
- UEFI firmware tree: `../uefi-a51` (Mu-Silicium `S5E9610Pkg` + `a51Pkg` with UniXNU integration)

## Hardware summary

| Subsystem | Value | Source |
| --- | --- | --- |
| SoC | Exynos 9611 (`S5E9611`), 4× Cortex-A53 + 4× Cortex-A73 (ARMv8.0-A) | `exynos9610.dts` |
| Target OS profile | `ipados-17.7.11-21H461` (`iPad7,1`, `arm64`) | ARMv8.0 constraint (no `arm64e`/PAC) |
| Interrupts / Timer | GIC-400 (**GICv2**, `0x12301000` / `0x12302000`), **26 MHz** timer, PSCI SMC | `exynos9610.dts` |
| RAM (6 GB variant) | 2 GiB low (`0x80000000`, TZ holes `0xBAB00000` & `0xE0000000`) + 4 GiB high (`0x880000000`–`0x980000000`) | `exynos9610.dts` / `S5E9610Pkg` |
| Storage | 128 GB Exynos UFS 2.0 (`0x13520000`, 4096 B logical blocks) + SD (`0x13550000`) | `exynos9610.dts` |
| Display | 1080×2400 Super AMOLED (`s6e3fa9` / `ea8076`), DECON `0x148B0000`, SimpleFB `0xCA000000` | `display-lcd_a51_common.dtsi` |
| Input | Buttons `gpa1-5` (VolUp), `gpa1-6` (VolDown), `gpa1-7` (Power); Touch Melfas `mss100` (`i2c_4` @ `0x48`, IRQ `gpa0-0`) | `exynos9611-a51_common.dtsi` |
| PMIC / USB / GPU | S2MPU09 (Speedy `0x11A10000`), DWC3 USB (`0x13200000`), Mali-G72 MP3 (`0x11500000`) | `exynos9610.dts` |
| Bootloader | Samsung S-Boot (**no fastboot**; Odin `.tar` or TWRP `dd` to `recovery` 67.8 MiB / `boot` 59.0 MiB) | LineageOS/TWRP `BoardConfig.mk` |

## Bring-up sequence (GitHub Actions)

1. **Phase 0 (On-device dump)**: Run `scripts/dump-a51-hardware.ps1` with SM-A515F in TWRP/root to capture `/proc/iomem`, `/sys/firmware/fdt`, `dmesg`, GPT `userdata` LBAs, and stock images.
2. **Stage 1 (UEFI Bring-up)**: Push `UniXNU-exynos9611-common`, `UniXNU-a51`, and `uefi-a51` to GitHub, then trigger **Actions → UEFI bring-up (Stage 1)** (`uefi-bringup.yml`) to compile `a51Pkg` on Ubuntu and download `a51-uefi-DEBUG-recovery.tar`. Flash to `recovery` via Odin and verify display, buttons, and UFS detection.
3. **Stage 2 (XNU Kernel + UniXNU Package)**: Pin the published `UniXNU-exynos9611-common` commit in `feeds.toml`, set repository secret `UNIXNU_UNIQUE_CHIP_ID` (8 hex digits), and trigger **Actions → package** (`package.yml`) on macOS runner.

## Local validation commands

From the sibling `UniXNU` checkout:

```sh
./unixnu defconfig a51_defconfig -D ../UniXNU-a51
./unixnu status --json -D ../UniXNU-a51
python3 scripts/porting-record.py check --device ../UniXNU-a51
```

## Documentation

- [UniXNU Porting Guide](https://unixnu.com/porting/)
- [Device repository reference](https://unixnu.com/porting/device-repository)
- [UEFI firmware integration](https://unixnu.com/porting/uefi-firmware)
- [Porting records](https://unixnu.com/porting/records)
