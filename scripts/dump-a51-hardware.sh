#!/usr/bin/env bash
# Phase-0 Hardware Reconnaissance Script for Samsung Galaxy A51 (SM-A515F).
# Run on Linux/macOS/WSL while connected via USB in TWRP or rooted Android.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
STAMP=$(date +%Y%m%d-%H%M%S)
OUT_DIR="$REPO_ROOT/phase0/$STAMP"
mkdir -p "$OUT_DIR"

ADB_CMD=(adb)
if [ -n "${ANDROID_SERIAL:-}" ]; then
  ADB_CMD+=(-s "$ANDROID_SERIAL")
fi

"${ADB_CMD[@]}" devices -l | tee "$OUT_DIR/adb-devices.txt"
UID_VAL=$("${ADB_CMD[@]}" shell id -u | tr -d '\r\n')
SU_PREFIX=""
if [ "$UID_VAL" != "0" ]; then
  if [ "$("${ADB_CMD[@]}" shell "su -c 'id -u'" | tr -d '\r\n')" = "0" ]; then
    SU_PREFIX="su -c"
  else
    echo "warning: shell is not root; boot into TWRP for a complete dump" >&2
  fi
fi

run_root() {
  local cmd="$1"
  local file="$2"
  echo "  -> $file"
  if [ -n "$SU_PREFIX" ]; then
    "${ADB_CMD[@]}" shell "su -c \"$cmd\"" > "$OUT_DIR/$file" 2>&1 || true
  else
    "${ADB_CMD[@]}" shell "$cmd" > "$OUT_DIR/$file" 2>&1 || true
  fi
}

run_root "getprop" "getprop.txt"
run_root "uname -a; cat /proc/version" "kernel-version.txt"
run_root "cat /proc/cmdline" "cmdline.txt"
run_root "cat /proc/cpuinfo" "cpuinfo.txt"
run_root "cat /proc/meminfo" "meminfo.txt"
run_root "cat /proc/iomem" "iomem.txt"
run_root "cat /proc/interrupts" "interrupts.txt"
run_root "dmesg" "dmesg.txt"
run_root "dmesg | grep -iE 'decon|dsim|fb|frame|lcd|panel|s6e3|ea8076|ufs|exynos-ufs|memory|reserved|hole|initrd|gic|timer|cntfrq|s2mpu09|melfas|mms|tsp'" "dmesg-filtered.txt"
run_root "ls -la /dev/block/by-name /dev/block/platform/*/by-name" "by-name.txt"
run_root "cat /proc/partitions" "proc-partitions.txt"
run_root "sgdisk --print /dev/block/sda || parted /dev/block/sda unit s print || fdisk -l /dev/block/sda" "gpt-sda.txt"
TMP_DIR=$("${ADB_CMD[@]}" shell "if [ -d /data/local/tmp ]; then echo /data/local/tmp; else echo /tmp; fi" | tr -d '\r\n')
run_root "cat /sys/firmware/fdt > $TMP_DIR/a51-live.dtb && chmod 644 $TMP_DIR/a51-live.dtb && ls -la $TMP_DIR/a51-live.dtb" "fdt-check.txt"
"${ADB_CMD[@]}" pull "$TMP_DIR/a51-live.dtb" "$OUT_DIR/a51-live.dtb" || true
"${ADB_CMD[@]}" shell "rm -f $TMP_DIR/a51-live.dtb" || true

for part in boot recovery dtbo vbmeta; do
  run_root "dd if=/dev/block/by-name/$part of=$TMP_DIR/a51-$part.img bs=4M && chmod 644 $TMP_DIR/a51-$part.img" "dd-$part.log"
  "${ADB_CMD[@]}" pull "$TMP_DIR/a51-$part.img" "$OUT_DIR/stock-$part.img" || true
  "${ADB_CMD[@]}" shell "rm -f $TMP_DIR/a51-$part.img" || true
done

echo "Phase-0 dump complete: $OUT_DIR"
