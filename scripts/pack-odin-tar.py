#!/usr/bin/env python3
"""Pack a boot.img or recovery.img into a Samsung Odin-flashable .tar archive."""
import argparse
import os
from pathlib import Path
import tarfile
import sys

PARTITION_LIMITS = {
    'boot': 61865984,      # SM-A515F BOARD_BOOTIMAGE_PARTITION_SIZE (~59.0 MiB)
    'recovery': 71106560,  # SM-A515F BOARD_RECOVERYIMAGE_PARTITION_SIZE (~67.8 MiB)
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', '-i', type=Path, required=True, help='Source Android boot image (.img)')
    parser.add_argument('--partition', '-p', choices=('boot', 'recovery'), default='recovery',
                        help='Target Odin partition name inside tar (default: recovery)')
    parser.add_argument('--output', '-o', type=Path, required=True, help='Destination Odin .tar path')
    parser.add_argument('--allow-oversize', action='store_true',
                        help='Skip SM-A515F partition size check')
    args = parser.parse_args()

    if not args.input.is_file():
        sys.exit(f'error: input image not found: {args.input}')

    size = args.input.stat().st_size
    limit = PARTITION_LIMITS[args.partition]
    if size > limit and not args.allow_oversize:
        sys.exit(
            f'error: {args.input} is {size} bytes, exceeding SM-A515F {args.partition} '
            f'partition size {limit} bytes ({limit / (1024 * 1024):.1f} MiB)'
        )

    member_name = f'{args.partition}.img'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, 'w', format=tarfile.USTAR_FORMAT) as archive:
        info = archive.gettarinfo(str(args.input), arcname=member_name)
        info.uid = 0
        info.gid = 0
        info.uname = 'root'
        info.gname = 'root'
        info.mtime = 0
        with args.input.open('rb') as handle:
            archive.addfile(info, handle)

    print(f'Packed {member_name} ({size} / {limit} bytes) -> {args.output}')


if __name__ == '__main__':
    main()
