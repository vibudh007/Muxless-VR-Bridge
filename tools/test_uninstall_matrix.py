#!/usr/bin/env python3
"""Actual historical managers, isolated HOME/XDG/prefix and private /proc.

Run with unshare --user --map-root-user --pid --fork --mount-proc.
Never installs into the user's real prefix or stops their Steam/VR processes.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def invoke(manager, env, *args):
    result = subprocess.run([str(manager), *args, '--json'], env=env,
                            text=True, capture_output=True, timeout=90)
    try:
        body = json.loads(result.stdout) if result.stdout.strip() else {}
    except ValueError:
        body = {'unparsed': result.stdout}
    return result.returncode, body, result.stderr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--images', type=Path, default=Path('shim/dist'))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    # Must have a private PID namespace with this Python process as PID 1.
    if os.getpid() != 1:
        parser.error('run through unshare --user --map-root-user --pid --fork --mount-proc')
    images = sorted(args.images.resolve().glob('*.AppImage'))
    candidate = args.candidate.resolve()
    assert candidate in images
    report = {'images': [], 'matrix': [], 'edge_cases': []}
    with tempfile.TemporaryDirectory(prefix='wmr-uninstall-matrix-') as tmp:
        temp = Path(tmp)
        managers = {}
        for index, image in enumerate(images):
            folder = temp / f'extract-{index}'
            folder.mkdir()
            subprocess.run([str(image), '--appimage-extract'], cwd=folder,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True, timeout=90)
            appdir = folder / 'squashfs-root'
            manager = appdir / 'usr/bin/muxless-vr-bridge-manager'
            if not manager.is_file():
                manager = appdir / 'usr/bin/wmr-gpu-bridge-manager'
            managers[image.name] = manager
            report['images'].append({'name': image.name, 'sha256': hashlib.sha256(image.read_bytes()).hexdigest()})
        candidate_name = candidate.name
        for installed_name, installer in managers.items():
            work = temp / 'case'
            if work.exists():
                shutil.rmtree(work)
            work.mkdir()
            env = {**os.environ, 'HOME': str(work / 'home'),
                   'XDG_CONFIG_HOME': str(work / 'config'), 'XDG_DATA_HOME': str(work / 'data'),
                   'XDG_STATE_HOME': str(work / 'state'), 'WMR_BRIDGE_PREFIX': str(work / 'prefix'),
                   'DISABLE_WMR_GPU_BRIDGE': '1'}
            # No real runtime needed: installation checks paths; probe uses host
            # capabilities read-only. Actual packaged payload and verification run.
            steam = work / 'home/.local/share/Steam'
            (steam / 'steamapps').mkdir(parents=True)
            runtime = work / 'SteamVR'
            oasis = work / 'Oasis'
            for path in (runtime / 'bin/linux64/vrcompositor', oasis / 'bin/linux64/driver_oasis.so'):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('fixture: discovery only')
            env.update(WMR_STEAMVR_PATH=str(runtime), WMR_OASIS_PATH=str(oasis),
                       APPDIR=str(installer.parents[2]))
            profile = steam / 'userdata/1/config/localconfig.vdf'
            profile.parent.mkdir(parents=True)
            profile.write_text('"apps" { "250820" { "LaunchOptions" "QT_QPA_PLATFORM=xcb %command% -vr" } }')
            code, body, error = invoke(installer, env, '--install')
            if code:
                raise AssertionError(('installation failed', installed_name, code, body, error))
            receipt_file = work / 'state/wmr-gpu-bridge/install-receipt.json'
            receipt = json.loads(receipt_file.read_text())
            if installed_name == candidate_name:
                code, doctor, error = invoke(installer, env, '--doctor')
                assert code == 0, (doctor, error)
                checks = {c['name']: c for c in doctor['checks']}
                assert checks['Oasis']['level'] == 'ok'
                assert checks['SteamVR']['level'] == 'ok'
                assert 'WMR device checks' not in checks
                assert 'udev rules' not in checks and 'libusb-1.0.so.0' not in checks
                assert receipt['schema'] == 1 and receipt['removal_protocol'] == 1
                assert all(f.get('kind') != 'desktop-symlink' for f in receipt['files'])
                missing_env = {**env, 'WMR_STEAMVR_PATH': '', 'WMR_OASIS_PATH': ''}
                code, doctor, error = invoke(installer, missing_env, '--doctor')
                assert code == 1, (doctor, error)
                checks = {c['name']: c['level'] for c in doctor['checks']}
                assert checks['Oasis'] == checks['SteamVR'] == 'error'
                code, _, _ = invoke(installer, missing_env, '--install')
                assert code == 1 and json.loads(receipt_file.read_text()) == receipt
                report['edge_cases'].append('candidate requires SteamVR and Oasis and installs no Steam desktop shortcut')
            backup = temp / 'backup'
            if backup.exists():
                shutil.rmtree(backup)
            shutil.copytree(work, backup, symlinks=True)
            for removing_name, remover in managers.items():
                shutil.rmtree(work)
                shutil.copytree(backup, work, symlinks=True)
                removal_env = {**env, 'APPDIR': str(remover.parents[2]),
                               'WMR_STEAMVR_PATH': str(work / 'missing-runtime'),
                               'WMR_OASIS_PATH': str(work / 'missing-oasis')}
                code, result, error = invoke(remover, removal_env, '--uninstall')
                leftovers = [f['path'] for f in receipt['files'] if os.path.lexists(f['path'])]
                profile_ok = profile.read_text() == '"apps" { "250820" { "LaunchOptions" "QT_QPA_PLATFORM=xcb %command% -vr" } }'
                success = code == 0 and not leftovers and profile_ok
                report['matrix'].append({'installed': installed_name, 'remover': removing_name,
                                         'success': success, 'exit': code, 'leftovers': leftovers,
                                         'profile_preserved': profile_ok, 'error': error[:1000]})
                # The candidate must remove all history, and all historical
                # removers must remove the candidate. Immutable history/history
                # failures are reported explicitly rather than rewritten.
                if candidate_name in (installed_name, removing_name):
                    assert success, report['matrix'][-1]
            print('matrix installation:', installed_name, flush=True)
            if installed_name == candidate_name:
                current = managers[candidate_name]
                current_env = {**env, 'APPDIR': str(current.parents[2])}
                def restore():
                    shutil.rmtree(work)
                    shutil.copytree(backup, work, symlinks=True)
                def require_ok(manager, environment, *arguments):
                    code, body, error = invoke(manager, environment, *arguments)
                    assert code == 0, (arguments, body, error)
                    return body
                restore()
                future = json.loads(receipt_file.read_text())
                future['version'] = 'future-999'
                future['future_metadata'] = {'unknown': ['retained', 123]}
                receipt_file.write_text(json.dumps(future))
                # Receipt prefix, not the remover build/version, controls removal.
                no_override = dict(current_env)
                no_override.pop('WMR_BRIDGE_PREFIX')
                result = require_ok(current, no_override, '--uninstall')
                assert not result['preserved_modified_files']
                assert require_ok(current, current_env, '--uninstall')['already_uninstalled']
                report['edge_cases'].append('future additive metadata, recorded custom prefix, repeated uninstall')
                for marker, old in managers.items():
                    if marker == candidate_name:
                        continue
                    for direction in ('upgrade', 'downgrade'):
                        restore()
                        path = oasis / 'bin/linux64/driver_oasis.so'
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text('fixture: discovery only')
                        old_env = {**current_env, 'APPDIR': str(old.parents[2]), 'WMR_STEAMVR_PATH': str(runtime)}
                        if direction == 'upgrade':
                            require_ok(current, current_env, '--uninstall')
                            require_ok(old, old_env, '--install')
                            require_ok(current, current_env, '--install')
                        else:
                            require_ok(old, old_env, '--install')
                        tracked = json.loads(receipt_file.read_text())['files']
                        result = require_ok(current, current_env, '--uninstall')
                        assert not result['preserved_modified_files']
                        assert not any(os.path.lexists(f['path']) for f in tracked)
                        report['edge_cases'].append(direction + ' through ' + marker + ' with all-version cleanup')
                # Force a real mid-install rename failure after earlier copies.
                restore()
                require_ok(current, current_env, '--uninstall')
                version = receipt['version']
                collision = work / 'prefix/lib/wmr-gpu-bridge' / version / 'muxless-vr-bridge-manager'
                collision.mkdir(parents=True)
                (collision / 'unrelated').write_text('preserve me')
                code, body, error = invoke(current, current_env, '--install')
                assert code != 0 and receipt_file.exists(), (body, error)
                partial = json.loads(receipt_file.read_text())
                result = require_ok(current, current_env, '--uninstall')
                assert result['preserved_modified_files'] == [str(collision)]
                assert (collision / 'unrelated').read_text() == 'preserve me'
                assert all(not os.path.lexists(f['path']) for f in partial['files'] if f['path'] != str(collision))
                assert not list(collision.parent.glob('*.new-*'))
                shutil.rmtree(collision)
                require_ok(current, current_env, '--uninstall')
                report['edge_cases'].append('actual mid-install failure leaves removal journal; owned files removed, collision preserved, copy temporary cleaned')
        report['required_pairs_passed'] = all(r['success'] for r in report['matrix']
                                              if candidate_name in (r['installed'], r['remover']))
        report['historical_pair_failures'] = sum(not r['success'] for r in report['matrix'])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: candidate/history in both directions; historical failures recorded:', report['historical_pair_failures'])


if __name__ == '__main__':
    main()
