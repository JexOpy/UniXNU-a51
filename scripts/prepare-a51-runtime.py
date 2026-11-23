#!/usr/bin/env python3
"""Prepare iPadOS 17.7.11 trust cache bundle and 128 MiB sparse APFS Data seed for SM-A515F."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid


def merge_plist(mount, home, name, values):
    path = mount / home / 'Library/Preferences' / (name + '.plist')
    path.parent.mkdir(parents=True, exist_ok=True)
    content = plistlib.loads(path.read_bytes()) if path.is_file() else {}
    content.update(values)
    path.write_bytes(plistlib.dumps(content, fmt=plistlib.FMT_BINARY))
    path.chmod(0o644)


def find_ipsw_component(ipsw_cache, component):
    for rec in sorted(ipsw_cache.glob('*.json')):
        try:
            info = json.loads(rec.read_text())
        except Exception:
            continue
        if info.get('component') == component:
            target = rec.with_suffix('')
            if target.is_file():
                return target
    raise SystemExit(f'Missing extracted IPSW component {component} in {ipsw_cache}')


def extract_seed_uuid(packed_ramdisk, dbr_path):
    _, extents = packed_ramdisk.regions(dbr_path.read_bytes())
    identities = set()
    for _, data in extents:
        for at in range(0, len(data), 4096):
            block = data[at:at + 4096]
            if block[32:36] == b'APSB':
                identities.add(str(uuid.UUID(bytes=bytes(block[240:256]))).upper())
    if len(identities) != 1:
        raise SystemExit(f'{dbr_path}: expected 1 APFS volume UUID, found {sorted(identities)}')
    return identities.pop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--device', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--data-mib', type=int, default=128)
    parser.add_argument('--build', default='21H461')
    args = parser.parse_args()

    unixnu = args.unixnu.resolve()
    device = args.device.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(unixnu / 'drivers/DarwinBoot/Test'))
    import packed_ramdisk

    # 1. Ensure SoC feed is fetched, then fetch IPSW trust caches + OS disk image
    subprocess.run(
        [sys.executable, str(unixnu / 'unixnu'), 'fetch', '-D', str(device), '--os', '17', 'exynos9611'],
        cwd=unixnu, check=True,
    )
    subprocess.run(
        [sys.executable, str(unixnu / 'unixnu'), 'ipsw', '-D', str(device), '--os', '17',
         'fetch', 'StaticTrustCache', 'Cryptex1,SystemTrustCache', 'OS'],
        cwd=unixnu, check=True,
    )
    ipsw_cache = unixnu / 'out/ipsw' / args.build
    static_tc = find_ipsw_component(ipsw_cache, 'StaticTrustCache')
    system_tc = find_ipsw_component(ipsw_cache, 'Cryptex1,SystemTrustCache')
    os_dmg = find_ipsw_component(ipsw_cache, 'OS')
    print(f'IPSW inputs: static_tc={static_tc.name} system_tc={system_tc.name} os_dmg={os_dmg.name}', flush=True)

    # 2. Bundle Apple IPSW trust caches for DarwinBoot
    trust_bundle = out / 'trust.bundle'
    subprocess.run(
        [sys.executable, str(unixnu / 'scripts/bundle-ipsw-trust.py'),
         str(trust_bundle), str(static_tc), str(system_tc)],
        cwd=unixnu, check=True,
    )
    trust_sha = hashlib.sha256(trust_bundle.read_bytes()).hexdigest()
    print(f'Trust bundle: {trust_bundle} ({trust_bundle.stat().st_size} bytes, sha256={trust_sha})', flush=True)

    # 3. Create a compact volatile-RAM-compatible sparse APFS Data seed (<= 2 GiB) from OS /private/var
    data_dbr = out / 'data.dbr'
    with tempfile.TemporaryDirectory(prefix='a51-data-seed-', dir=out) as td:
        work = Path(td)
        os_mnt = work / 'os-mount'
        os_mnt.mkdir()
        os_attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', str(os_mnt), '-plist', str(os_dmg)]
        ))
        os_dev = os_attached['system-entities'][0]['dev-entry']
        actual_os_mnt = next(
            (Path(e['mount-point']) for e in os_attached.get('system-entities', [])
             if e.get('mount-point') and (Path(e['mount-point']) / 'sbin/launchd').is_file()),
            os_mnt,
        )
        try:
            source_var = actual_os_mnt / 'private/var'
            if not (actual_os_mnt / 'sbin/launchd').is_file() or not source_var.is_dir():
                raise SystemExit(f'Mounted OS image at {actual_os_mnt} missing /sbin/launchd or /private/var')

            raw = work / 'seed.cdr'
            subprocess.run(
                ['hdiutil', 'create', '-size', f'{args.data_mib}m', '-layout', 'NONE',
                 '-fs', 'APFS', '-volname', 'Data', '-type', 'UDTO', str(raw)],
                check=True,
            )
            data_mnt = work / 'data-mount'
            data_mnt.mkdir()
            data_attached = plistlib.loads(subprocess.check_output(
                ['hdiutil', 'attach', '-nobrowse', '-owners', 'off',
                 '-mountpoint', str(data_mnt), '-plist', str(raw)]
            ))
            data_dev = data_attached['system-entities'][0]['dev-entry']
            actual_data_mnt = next(
                (Path(e['mount-point']) for e in data_attached.get('system-entities', [])
                 if e.get('mount-point')),
                data_mnt,
            )
            try:
                for current, dirs, files in os.walk(source_var, followlinks=False):
                    dirs[:] = [name for name in dirs if name != 'staged_system_apps']
                    dest = actual_data_mnt / Path(current).relative_to(source_var)
                    dest.mkdir(exist_ok=True)
                    dest.chmod(stat.S_IMODE(Path(current).stat().st_mode))
                    for name in list(dirs) + files:
                        src, dst = Path(current) / name, dest / name
                        if src.is_symlink():
                            dst.symlink_to(os.readlink(src))
                            if name in dirs:
                                dirs.remove(name)
                        elif src.is_file():
                            shutil.copy2(src, dst)
                for name in ('keybags', 'mobile/tmp', 'tmp', 'run', 'containers/Bundle',
                             'containers/Data/System', 'containers/Shared/SystemGroup', 'containers/Temp'):
                    (actual_data_mnt / name).mkdir(parents=True, exist_ok=True)
                (actual_data_mnt / 'tmp').chmod(0o1777)

                # Skip Setup Assistant + disable compressed/deep framebuffer (matching make-data-seed.py)
                merge_plist(actual_data_mnt, 'mobile', 'com.apple.purplebuddy', dict(
                    SetupDone=True, SetupFinishedAllSteps=True, SetupVersion=11, daemonizeNeedsToRun=False))
                merge_plist(actual_data_mnt, 'mobile', 'com.apple.purplebuddy.notbackedup', {
                    n: False for n in (
                        'ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                        'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                        'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy', 'SilentICDPUpgrade',
                    )})
                merge_plist(actual_data_mnt, 'mobile', 'com.apple.managedconfiguration.notbackedup',
                            dict(LockdownActivationIndicatesCloudConfigurationAvailable=False))
                merge_plist(actual_data_mnt, 'mobile', 'com.apple.springboard', dict(
                    BKNoWatchdogs=True,
                    FBLaunchWatchdogExceptions={'com.apple.purplebuddy': 100.0},
                    MCForceShowCloudConfigurationUI=False))
                merge_plist(actual_data_mnt, 'mobile', 'com.apple.migration', dict(
                    LastSystemVersion=args.build,
                    DMUserDataDisposition=dict(SyntheticDidUpgrade=False, buildVersion=args.build)))
                for home in ('root', 'mobile'):
                    for pname in ('backboardd', '.GlobalPreferences'):
                        merge_plist(actual_data_mnt, home, pname, dict(
                            CAEnableDeepFramebuffer=False,
                            CAEnableFramebufferCompression=False,
                            CAEnableIOSurfaceCompression=False))
            finally:
                subprocess.run(['hdiutil', 'detach', data_dev, '-force'], check=True)

            packed_ramdisk.pack(raw, data_dbr)
        finally:
            subprocess.run(['hdiutil', 'detach', os_dev, '-force'], check=True)

    data_uuid = extract_seed_uuid(packed_ramdisk, data_dbr)
    print(f'Data seed: {data_dbr} ({data_dbr.stat().st_size} bytes, UUID={data_uuid})', flush=True)

    # 4. Bind the generated Data volume UUID into configs/a51_defconfig for this build
    defconfig = device / 'configs/a51_defconfig'
    text = defconfig.read_text()
    updated, count = re.subn(
        r'^CONFIG_XNU_OWNER_DATA_VOLUME_UUID=.*$',
        f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{data_uuid}"',
        text, flags=re.M,
    )
    if count != 1:
        raise SystemExit(f'Expected 1 CONFIG_XNU_OWNER_DATA_VOLUME_UUID line in {defconfig}, found {count}')
    defconfig.write_text(updated)

    summary = dict(
        trust_cache=str(trust_bundle),
        trust_cache_sha256=trust_sha,
        data_ramdisk=str(data_dbr),
        data_ramdisk_bytes=data_dbr.stat().st_size,
        data_uuid=data_uuid,
    )
    (out / 'runtime.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == '__main__':
    main()
