#!/usr/bin/env python3
"""Isolated AppImage install/verify/uninstall tests. Never edits the user's Steam profiles."""
from pathlib import Path
import os,json,subprocess,tempfile,sys
image=Path(sys.argv[1]).resolve()
probe=subprocess.run([str(image),'--doctor','--json'],text=True,capture_output=True,timeout=90)
paths=json.loads(probe.stdout)['paths']
with tempfile.TemporaryDirectory(prefix='wmr-appimage-test-') as tmp:
 root=Path(tmp);env={**os.environ,'APPIMAGE_EXTRACT_AND_RUN':'1','WMR_BRIDGE_PREFIX':str(root/'prefix'),'XDG_CONFIG_HOME':str(root/'config'),'XDG_DATA_HOME':str(root/'data'),'XDG_STATE_HOME':str(root/'state'),'WMR_STEAMVR_PATH':paths['steamvr'],'WMR_OASIS_PATH':paths['oasis']}
 def run(*args,code=0):
  p=subprocess.run([str(image),*args,'--json'],env=env,text=True,capture_output=True,timeout=90)
  if p.returncode!=code:raise AssertionError((args,p.returncode,p.stdout,p.stderr))
  return json.loads(p.stdout) if p.stdout.strip() else None
 manifest=root/'data/vulkan/implicit_layer.d/wmr_gpu_bridge.json';manifest.parent.mkdir(parents=True);foreign='{"layer":{"name":"OTHER","library_path":"/unknown"}}';manifest.write_text(foreign)
 run('--install','--no-steam-config',code=1);assert manifest.read_text()==foreign;assert not (root/'prefix/bin/wmr-gpu-bridge').exists();manifest.unlink()
 manifest.write_text('malformed registration');run('--install','--no-steam-config',code=1);assert manifest.read_text()=='malformed registration';manifest.unlink()
 installed=run('--install','--no-steam-config');assert installed['installed'];assert not installed['verification']['checks']==[]
 report=run('--verify');assert report['runtime']['state']=='unverified'
 run('--verify','--runtime',code=2)
 desktop=root/'data/applications/wmr-gpu-bridge-steam.desktop'
 assert desktop.exists() and '--steam %U' in desktop.read_text()
 config=root/'config/wmr-gpu-bridge/settings.conf';cli=root/'prefix/bin/wmr-gpu-bridge'
 config.write_text(config.read_text()+'unlit_missing_ubo=0\nperformance_logging=1\ncustom_option=retained\n')
 subprocess.run([str(cli),'--disable'],env=env,check=True,capture_output=True)
 subprocess.run([str(cli),'/bin/sh','-c','test "$DISABLE_WMR_GPU_BRIDGE" = 1'],env=env,check=True)
 assert 'enabled=0' in config.read_text()
 assert 'performance_logging=1' in config.read_text() and 'custom_option=retained' in config.read_text() and 'unlit_missing_ubo=0' in config.read_text()
 run('--install','--no-steam-config')
 assert 'enabled=0' in config.read_text() and 'performance_logging=1' in config.read_text() and 'unlit_missing_ubo=0' in config.read_text()
 run('--verify')
 result=run('--uninstall');assert not result['preserved_modified_files'];assert not manifest.exists();assert not cli.exists();assert not desktop.exists()
 run('--install','--no-steam-config');library=Path(json.loads(manifest.read_text())['layer']['library_path']);library.write_bytes(library.read_bytes()+b'changed-by-test')
 run('--verify',code=1);result=run('--uninstall');assert str(library) in result['preserved_modified_files'];assert library.exists()
 print('PASS: foreign/malformed manifests protected; upgrade and enable/disable preserve options; install checksums and loader checked; runtime-unverified distinguished; disable persists; clean uninstall; modified file preserved.')
