# AGENTS.md — UniXNU-a51

## Responsibility

This repository holds the UniXNU Format 2 device configuration, hardware device tree, porting record, packaging workflow, and diagnostic scripts for the **Samsung Galaxy A51 (`SM-A515F`, `a51`, Exynos 9611 / `S5E9611`)**.

## Required reading

- `porting.toml` (authoritative record for every subsystem)
- `PHASE0.md` (hardware verification checklist and Samsung Odin flashing rules)
- `../UniXNU-exynos9611-common/AGENTS.md`
- [UniXNU Porting Guide](https://unixnu.com/porting/)
- [Device repository reference](https://unixnu.com/porting/device-repository)
- [Porting records](https://unixnu.com/porting/records)

## Rules

1. Never edit `scripts/package.sh`; it must remain identical to `UniXNU/templates/package.sh`.
2. Update `porting.toml` (`findings` or `evidence`) whenever changing code or configuration files, and verify with `python3 ../UniXNU/scripts/porting-record.py check --device .`.
3. Never claim `host-verified` or `device-verified` without real command output, 40-character commit SHA, and accessible log evidence.
4. Do not commit proprietary firmware binaries, Apple IPSWs, device dumps (`phase0/`), or `local.toml`.
5. Exynos 9611 is ARMv8.0-A (`arm64` only, `CONFIG_XNU_NATIVE_PAN=n`) with GIC-400 (GICv2) and 26 MHz timer.
