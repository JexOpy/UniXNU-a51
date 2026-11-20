# Phase-0 Hardware Reconnaissance Script for Samsung Galaxy A51 (SM-A515F)
# Run on Windows PowerShell while the phone is connected via USB in TWRP
# OR in rooted Android (Magisk / su) with USB debugging enabled.
#
# Output is saved to: UniXNU-a51\phase0\<timestamp>\ (git-ignored)

param(
    [string]$Adb = "adb",
    [string]$Serial = "",
    [switch]$DumpImages = $true
)

$ErrorActionPreference = "Continue"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$OutDir = Join-Path $RepoRoot "phase0\$Stamp"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$AdbBase = @()
if ($Serial) { $AdbBase += @("-s", $Serial) }

function Invoke-Adb {
    param([string[]]$ArgsList)
    & $Adb @AdbBase @ArgsList
}

Write-Host "=== Checking ADB connection ===" -ForegroundColor Cyan
$devOut = Invoke-Adb @("devices", "-l")
$devOut | Out-File (Join-Path $OutDir "adb-devices.txt") -Encoding utf8
Write-Host ($devOut -join "`n")

$state = (Invoke-Adb @("get-state") 2>&1) | Out-String
if ($state -notmatch "device|recovery") {
    Write-Error "No SM-A515F detected in ADB device or recovery mode. Boot into TWRP or enable USB debugging."
    exit 1
}

# Detect whether shell is already root (TWRP) or needs su (rooted Android)
$uid = ((Invoke-Adb @("shell", "id -u")) -join "").Trim()
$UseSu = $false
if ($uid -ne "0") {
    $suTest = ((Invoke-Adb @("shell", "su -c 'id -u'")) -join "").Trim()
    if ($suTest -eq "0") {
        $UseSu = $true
        Write-Host "Detected rooted Android (using su)." -ForegroundColor Green
    } else {
        Write-Warning "Shell is not root (uid=$uid). Some reads (/proc/iomem, dmesg, partitions) may fail. Boot into TWRP for a complete dump."
    }
} else {
    Write-Host "Detected root shell (TWRP / adbd root)." -ForegroundColor Green
}

function Run-RootShell {
    param([string]$Cmd, [string]$OutFile)
    Write-Host "  -> $OutFile"
    if ($UseSu) {
        $escaped = $Cmd.Replace('"', '\"')
        Invoke-Adb @("shell", "su -c `"$escaped`"") | Out-File (Join-Path $OutDir $OutFile) -Encoding utf8
    } else {
        Invoke-Adb @("shell", $Cmd) | Out-File (Join-Path $OutDir $OutFile) -Encoding utf8
    }
}

Write-Host "=== Capturing hardware & kernel tables ===" -ForegroundColor Cyan
Run-RootShell "getprop" "getprop.txt"
Run-RootShell "uname -a; cat /proc/version" "kernel-version.txt"
Run-RootShell "cat /proc/cmdline" "cmdline.txt"
Run-RootShell "cat /proc/cpuinfo" "cpuinfo.txt"
Run-RootShell "cat /proc/meminfo" "meminfo.txt"
Run-RootShell "cat /proc/iomem" "iomem.txt"
Run-RootShell "cat /proc/interrupts" "interrupts.txt"
Run-RootShell "dmesg" "dmesg.txt"
Run-RootShell "dmesg | grep -iE 'decon|dsim|fb|frame|lcd|panel|s6e3|ea8076|ufs|exynos-ufs|memory|reserved|hole|initrd|gic|timer|cntfrq|s2mpu09|melfas|mms|tsp'" "dmesg-filtered.txt"

Write-Host "=== Capturing UFS / GPT partition geometry ===" -ForegroundColor Cyan
Run-RootShell 'ls -la /dev/block/by-name /dev/block/platform/*/by-name 2>&1' "by-name.txt"
Run-RootShell 'cat /proc/partitions' "proc-partitions.txt"
Run-RootShell 'head -n 5 /sys/block/sda/size /sys/block/sda/queue/logical_block_size /sys/block/sda/queue/physical_block_size 2>&1' "ufs-block-sizes.txt"
Run-RootShell 'sgdisk --print /dev/block/sda 2>&1 || parted /dev/block/sda unit s print 2>&1 || fdisk -l /dev/block/sda 2>&1' "gpt-sda.txt"

Write-Host "=== Capturing live FDT (/sys/firmware/fdt) & panel info ===" -ForegroundColor Cyan
$TmpDir = ((Invoke-Adb @("shell", "if [ -d /data/local/tmp ]; then echo /data/local/tmp; else echo /tmp; fi")) -join "").Trim()
Run-RootShell "cat /sys/class/lcd/panel/lcd_type /sys/class/lcd/panel/window_type /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel 2>&1" "panel-info.txt"
Run-RootShell "cat /sys/firmware/fdt > $TmpDir/a51-live.dtb && chmod 644 $TmpDir/a51-live.dtb && ls -la $TmpDir/a51-live.dtb" "fdt-check.txt"
Invoke-Adb @("pull", "$TmpDir/a51-live.dtb", (Join-Path $OutDir "a51-live.dtb")) | Out-Null
Invoke-Adb @("shell", "rm -f $TmpDir/a51-live.dtb") | Out-Null

if ($DumpImages) {
    Write-Host "=== Backing up stock boot, recovery, dtbo, vbmeta (read-only) ===" -ForegroundColor Cyan
    foreach ($part in @("boot", "recovery", "dtbo", "vbmeta")) {
        Write-Host "  -> pulling $part.img"
        $cmd = "dd if=/dev/block/by-name/$part of=$TmpDir/a51-$part.img bs=4M 2>&1 && chmod 644 $TmpDir/a51-$part.img"
        if ($UseSu) {
            Invoke-Adb @("shell", "su -c `"$cmd`"") | Out-Null
        } else {
            Invoke-Adb @("shell", $cmd) | Out-Null
        }
        Invoke-Adb @("pull", "$TmpDir/a51-$part.img", (Join-Path $OutDir "stock-$part.img")) | Out-Null
        Invoke-Adb @("shell", "rm -f $TmpDir/a51-$part.img") | Out-Null
    }
}

Write-Host ""
Write-Host "Phase-0 dump complete: $OutDir" -ForegroundColor Green
Write-Host "Next step: inspect iomem.txt, dmesg-filtered.txt, gpt-sda.txt, partition-bytes.txt, and stock-boot.img header." -ForegroundColor Yellow
