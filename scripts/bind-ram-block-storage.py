#!/usr/bin/env python3
"""Bind libRamBlock.a (NativeRamBlock + ExynosUfsWindow) to prelinked IOStorageFamily on iPadOS 17.7.11."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np
sys.path.insert(0, str(Path(__file__).resolve().parent))
from kernel_target import TRIPLE

# Verified com.apple.iokit.IOStorageFamily IOBlockStorageDevice addresses for
# ipados-17.7.11-21H461 (kernelcache sha256 f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7,
# static prelink slide -0x1000000; __TEXT_EXEC @ 0xfffffff00496c000, __DATA_CONST @ 0xfffffff005cb3200).
IOBLOCK_SLOT_ADDRESSES = {
    7:   0xfffffff00496edc4,
    21:  0xfffffff00496ee30,
    33:  0xfffffff00496ee88,
    44:  0xfffffff00496ef38,
    171: 0xfffffff00496f074,
    182: 0xfffffff00496f038,
    183: 0xfffffff00496f044,
    185: 0xfffffff00496f050,
    186: 0xfffffff00496f05c,
    187: 0xfffffff00496f068,
    188: 0xfffffff00496f080,
    189: 0xfffffff00496f08c,
}
IOBLOCK_PURE_SLOTS = {168, 169, 170, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 184}
IOBLOCK_EXTRA_ADDRESSES = {
    '__ZN20IOBlockStorageDeviceC2EPK11OSMetaClass': 0xfffffff00496ed98,
    '__ZN20IOBlockStorageDeviceD2Ev': 0xfffffff00496edb8,
    '__ZN20IOBlockStorageDevice10gMetaClassE': 0xfffffff006add1d0,
    '__ZTV20IOBlockStorageDevice': 0xfffffff005cb44c0,
}


def parse_symbols_and_vtable(obj_bytes, vtable_name):
    image = np.macho(obj_bytes)
    symcmd = next(at for cmd, at, size in image['commands'] if cmd == 2)
    symoff, count, stroff, _ = struct.unpack_from('<4I', obj_bytes, symcmd + 8)
    symbols = []
    for i in range(count):
        ix, kind, _, _, value = struct.unpack_from('<IBBHQ', obj_bytes, symoff + i * 16)
        name = obj_bytes[stroff + ix:obj_bytes.index(b'\0', stroff + ix)].decode()
        symbols.append((name, value, kind))
    vtable = next(v for name, v, kind in symbols if name == vtable_name)
    relocations = {}
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= vtable < section['va'] + section['size']:
                ro, rc = struct.unpack_from('<II', obj_bytes, section['command_offset'] + 56)
                for i in range(rc):
                    address, word = struct.unpack_from('<II', obj_bytes, ro + i * 8)
                    if (word >> 27) & 1 and (word >> 28) == 0 and ((word >> 25) & 3) == 3:
                        relocations[section['va'] + address] = symbols[word & 0xffffff][0]
    return symbols, vtable, relocations


def compile_reference_object(out):
    source = ROOT / 'refer/IOStorageFamily/IOBlockStorageDevice.cpp'
    if not source.is_file():
        raise SystemExit(f'Missing pinned IOStorageFamily source: {source}')
    record = json.loads(
        (ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
        .read_text().strip().rstrip(',')
    )
    ref_obj = out / 'IOBlockStorageDevice-reference.o'
    arguments = record['arguments']
    command, index = [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(ref_obj)])
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
            index += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        index += 1
    command.extend([
        '-I', str(ROOT / 'build/storage-abi/include'),
        '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-deprecated-declarations',
        '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-unnecessary-virtual-specifier',
        '-Wno-deprecated-enum-enum-conversion',
    ])
    res = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
    if res.returncode != 0:
        raise SystemExit((res.stdout + res.stderr)[-3500:])
    return ref_obj


def main():
    out = ROOT / 'build/ram-block'
    ram_obj = out / 'NativeRamBlock.o'
    if not ram_obj.is_file():
        raise SystemExit(f'Missing compiled RamBlock object: {ram_obj}')

    ref_obj = compile_reference_object(out)
    _, ref_vtable, ref_relocs = parse_symbols_and_vtable(ref_obj.read_bytes(), '__ZTV20IOBlockStorageDevice')
    methods = []
    for i in range(190):
        name = ref_relocs.get(ref_vtable + 16 + i * 8)
        if not name:
            raise SystemExit(f'IOBlockStorageDevice reference vtable truncated at slot {i}')
        methods.append(name)
    if ref_vtable + 16 + 190 * 8 in ref_relocs:
        raise SystemExit('IOBlockStorageDevice reference vtable has more than 190 slots')

    pure_slots = {i for i, n in enumerate(methods) if n == '___cxa_pure_virtual'}
    if pure_slots != IOBLOCK_PURE_SLOTS:
        raise SystemExit(f'IOBlockStorageDevice pure virtual slot mismatch: {sorted(pure_slots)}')

    possible = dict(IOBLOCK_EXTRA_ADDRESSES)
    for slot, addr in IOBLOCK_SLOT_ADDRESSES.items():
        name = methods[slot]
        if 'IOBlockStorageDevice' not in name:
            raise SystemExit(f'Slot {slot} expected IOBlockStorageDevice method, got {name}')
        possible[name] = addr

    ram_symbols, _, _ = parse_symbols_and_vtable(ram_obj.read_bytes(), '__ZTV14NativeRamBlock')
    undefined = sorted({name for name, _, kind in ram_symbols if kind == 1 and 'IOBlockStorageDevice' in name})
    if any(re.search(r'IOBlockStorageDeviceD[01]Ev$', n) for n in undefined):
        raise SystemExit(f'Concrete storage subclass must not reference trapping D0/D1 destructor: {undefined}')
    missing = [name for name in undefined if name not in possible]
    if missing:
        raise SystemExit(f'Unrecognized IOBlockStorageDevice imports in NativeRamBlock.o: {missing}')

    asm_path = out / 'original-bindings.S'
    asm_path.write_text(''.join(f'.globl {name}\n.set {name}, {possible[name]:#x}\n' for name in undefined))
    bindings_obj = out / 'original-bindings.o'
    subprocess.run(
        ['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot',
         '-c', str(asm_path), '-o', str(bindings_obj)],
        check=True,
    )
    archive = out / 'libRamBlock.a'
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(ram_obj), str(bindings_obj)], check=True)
    print(f'PASS: bound {len(undefined)} IOBlockStorageDevice symbols into {archive.name}: {undefined}')


if __name__ == '__main__':
    main()
