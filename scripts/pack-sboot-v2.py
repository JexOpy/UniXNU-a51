#!/usr/bin/env python3
"""Build a Samsung Galaxy A51 (SM-A515F / A515FXXU8HWI1) S-Boot v2 boot/recovery image + Odin .tar.

Samsung Exynos 9611 S-Boot expects Android Boot Image header_version = 2 carrying
a Samsung DT_TABLE (magic 0xd7b7ab1e) in the --dtb slot, plus --recovery_dtbo when
flashed to the recovery partition. This tool extracts the verified DT_TABLE,
recovery_dtbo, os_version, and board fields from your Phase-0 donor image
(stock-recovery.img or stock-boot.img) and repacks a UEFI / UniXNU kernel blob
(BootShim + SILICIUM_UEFI.fd) and optional payload ramdisk into a bootable v2 image.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile

PARTITION_LIMITS = {
    'boot': 61865984,      # SM-A515F sda12 (59.0 MiB)
    'recovery': 71106560,  # SM-A515F sda13 (67.8 MiB)
}


def align(n: int, page: int) -> int:
    return ((n + page - 1) // page) * page


def parse_donor(donor_path: Path):
    data = donor_path.read_bytes()
    if len(data) < 1660 or data[:8] != b'ANDROID!':
        sys.exit(f'error: {donor_path} is not an Android boot image')
    (k_sz, k_addr, r_sz, r_addr, s_sz, s_addr, tags, page_sz, hdr_ver, os_ver) = struct.unpack_from('<10I', data, 8)
    if hdr_ver != 2:
        sys.exit(f'error: expected header_version=2 donor, got {hdr_ver}')
    board = data[48:64].split(b'\0', 1)[0].decode('ascii', errors='ignore')
    rec_dtbo_sz, _rec_dtbo_off, _hdr_sz = struct.unpack_from('<IQI', data, 1632)
    dtb_sz, dtb_addr = struct.unpack_from('<IQ', data, 1648)
    rec_off = page_sz + align(k_sz, page_sz) + align(r_sz, page_sz) + align(s_sz, page_sz)
    dtb_off = rec_off + align(rec_dtbo_sz, page_sz)
    rec_dtbo = data[rec_off:rec_off + rec_dtbo_sz] if rec_dtbo_sz else b''
    dtb = data[dtb_off:dtb_off + dtb_sz] if dtb_sz else b''
    if dtb[:4] != b'\xd7\xb7\xab\x1e':
        sys.exit(f'error: donor DTB does not have DT_TABLE magic 0xd7b7ab1e (got {dtb[:4].hex()})')
    base = 0x10000000
    ver = os_ver >> 11
    patch = os_ver & 0x7ff
    os_version_str = f'{(ver >> 14) & 0x7f}.{(ver >> 7) & 0x7f}.{ver & 0x7f}'
    os_patch_str = f'{2000 + (patch >> 4):04d}-{patch & 0xf:02d}'
    return {
        'base': base,
        'pagesize': page_sz,
        'kernel_offset': k_addr - base,
        'ramdisk_offset': r_addr - base,
        'tags_offset': tags - base,
        'dtb_offset': dtb_addr - base,
        'board': board,
        'os_version': os_version_str,
        'os_patch_level': os_patch_str,
        'recovery_dtbo': rec_dtbo,
        'dtb': dtb,
    }


def extract_kernel_from_img(img_path: Path) -> bytes:
    data = img_path.read_bytes()
    if data[:8] != b'ANDROID!':
        return data
    k_sz, _, _, _, _, _, _, page_sz, hdr_ver, _ = struct.unpack_from('<10I', data, 8)
    if hdr_ver >= 3:
        sys.exit('error: v3/v4 source image not supported')
    return data[page_sz:page_sz + k_sz]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', '-k', type=Path, required=True,
                        help='Raw BootShim+UEFI blob OR Mu-a51-0.img / UniXNU boot.img (kernel section extracted)')
    parser.add_argument('--donor', '-d', type=Path, required=True,
                        help='Phase-0 stock-recovery.img or stock-boot.img supplying Samsung DT_TABLE and recovery_dtbo')
    parser.add_argument('--mkbootimg', type=Path,
                        default=Path(__file__).resolve().parents[2] / 'uefi-a51/Resources/Scripts/mkbootimg.py',
                        help='Path to mkbootimg.py')
    parser.add_argument('--ramdisk', '-r', type=Path, default=None, help='Optional payload ramdisk file')
    parser.add_argument('--partition', '-p', choices=('boot', 'recovery'), default='recovery')
    parser.add_argument('--output', '-o', type=Path, required=True, help='Output .img path')
    parser.add_argument('--odin-tar', type=Path, default=None, help='Optional Odin .tar output path')
    args = parser.parse_args()

    if not args.mkbootimg.is_file():
        sys.exit(f'error: mkbootimg.py not found at {args.mkbootimg}')

    meta = parse_donor(args.donor)
    kernel_bytes = extract_kernel_from_img(args.kernel)
    if kernel_bytes[56:60] != b'ARM\x64':
        print(f'warning: kernel ARM64 magic at +0x38 is {kernel_bytes[56:60]!r}, expected b"ARMd"', file=sys.stderr)

    with tempfile.TemporaryDirectory(prefix='a51-sboot-v2-') as td:
        tmp = Path(td)
        k_file = tmp / 'kernel'
        k_file.write_bytes(kernel_bytes)
        r_file = args.ramdisk if args.ramdisk else (tmp / 'empty-ramdisk')
        if not args.ramdisk:
            r_file.write_bytes(b'')
        dtb_file = tmp / 'dtb.img'
        dtb_file.write_bytes(meta['dtb'])

        cmd = [
            sys.executable, str(args.mkbootimg),
            '--header_version', '2',
            '--base', hex(meta['base']),
            '--pagesize', str(meta['pagesize']),
            '--kernel_offset', hex(meta['kernel_offset']),
            '--ramdisk_offset', hex(meta['ramdisk_offset']),
            '--tags_offset', hex(meta['tags_offset']),
            '--dtb_offset', hex(meta['dtb_offset']),
            '--os_version', meta['os_version'],
            '--os_patch_level', meta['os_patch_level'],
            '--board', meta['board'],
            '--kernel', str(k_file),
            '--ramdisk', str(r_file),
            '--dtb', str(dtb_file),
            '-o', str(args.output),
        ]
        if meta['recovery_dtbo'] and args.partition == 'recovery':
            dtbo_file = tmp / 'recovery_dtbo.img'
            dtbo_file.write_bytes(meta['recovery_dtbo'])
            cmd += ['--recovery_dtbo', str(dtbo_file)]

        args.output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(cmd, check=True)

    size = args.output.stat().st_size
    limit = PARTITION_LIMITS[args.partition]
    if size > limit:
        sys.exit(f'error: {args.output} is {size} bytes, exceeding {args.partition} limit ({limit} bytes)')
    print(f'Built S-Boot v2 {args.partition} image: {args.output} ({size} / {limit} bytes)')

    if args.odin_tar:
        member = f'{args.partition}.img'
        args.odin_tar.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.odin_tar, 'w', format=tarfile.USTAR_FORMAT) as tar:
            info = tar.gettarinfo(str(args.output), arcname=member)
            info.uid = info.gid = 0
            info.uname = info.gname = 'root'
            info.mtime = 0
            with args.output.open('rb') as handle:
                tar.addfile(info, handle)
        print(f'Packed Odin tar ({member}): {args.odin_tar}')


if __name__ == '__main__':
    main()
