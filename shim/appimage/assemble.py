#!/usr/bin/env python3
from pathlib import Path
import shutil,json,hashlib,urllib.request,os,subprocess
root=Path(__file__).resolve().parents[2]; version=(root/'shim/VERSION').read_text().strip()
assert ('#define WMR_BRIDGE_VERSION \"'+version+'\"') in (root/'shim/version.hpp').read_text(), 'VERSION/header mismatch'
app=root/'shim/dist/WMR-GPU-Bridge.AppDir';(app/'usr/bin').mkdir(parents=True,exist_ok=True);(app/'usr/lib').mkdir(exist_ok=True)
for name in ['wmr-gpu-bridge','wmr-gpu-bridge-manager','wmr-gpu-bridge-probe']:
 shutil.copy2(root/'shim/build-portable'/name,app/'usr/bin'/name)
shutil.copy2(root/'shim/build-portable/libwmr_gpu_bridge.so',app/'usr/lib/libwmr_gpu_bridge.so')
for name in ['AppRun','wmr-gpu-bridge.desktop','wmr-gpu-bridge.svg']:shutil.copy2(root/'shim/appimage'/name,app/name)
icon=app/'.DirIcon'
if not icon.exists():icon.symlink_to('wmr-gpu-bridge.svg')
licenses=app/'usr/share/licenses';licenses.mkdir(parents=True,exist_ok=True)
shutil.copy2(root/'shim/third_party/nlohmann/LICENSE.MIT',licenses/'nlohmann-json.MIT')
shutil.copy2(root/'shim/third_party/picosha2/LICENSE',licenses/'PicoSHA2.MIT')
for p in (root/'shim/build-portable/licenses').glob('COPYING*'):shutil.copy2(p,licenses/p.name)
files=[p for p in sorted(app.rglob('*')) if p.is_file() and p.name!='payload.json']
payload={'version':version,'baseline':'glibc 2.28; x86-64; host Vulkan and Wayland libraries','files':[{'path':str(p.relative_to(app)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]}
(app/'payload.json').write_text(json.dumps(payload,indent=2)+'\n')
lock=json.loads((root/'shim/appimage/toolchain-lock.json').read_text());tool=Path('/tmp/wmr-appimagetool.AppImage')
if not tool.exists():tool.write_bytes(urllib.request.urlopen(lock['tool_url']).read())
if hashlib.sha256(tool.read_bytes()).hexdigest()!=lock['tool_sha256']:raise SystemExit('appimagetool integrity check failed')
tool.chmod(0o755);output=root/'shim/dist'/f'WMR-GPU-Bridge-{version}-x86_64.AppImage'
subprocess.run([str(tool),'-n','--mksquashfs-opt','-processors','--mksquashfs-opt','2',str(app),str(output)],env={**os.environ,'APPIMAGE_EXTRACT_AND_RUN':'1','ARCH':'x86_64'},check=True)
output.with_name(output.name+'.sha256').write_text(hashlib.sha256(output.read_bytes()).hexdigest()+'  '+output.name+'\n')
print(output)
