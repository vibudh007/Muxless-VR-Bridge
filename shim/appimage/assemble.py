#!/usr/bin/env python3
from pathlib import Path
import shutil,json,hashlib,urllib.request,os,subprocess,tempfile
root=Path(__file__).resolve().parents[2]; version=(root/'shim/VERSION').read_text().strip()
assert ('#define MUXLESS_VR_BRIDGE_VERSION \"'+version+'\"') in (root/'shim/version.hpp').read_text(), 'VERSION/header mismatch'
dist=root/'shim/dist';dist.mkdir(parents=True,exist_ok=True)
staging=tempfile.TemporaryDirectory(dir=dist)
app=Path(staging.name)/'Muxless-VR-Bridge.AppDir';(app/'usr/bin').mkdir(parents=True);(app/'usr/lib').mkdir()
for name in ['wmr-gpu-bridge','muxless-vr-bridge-manager','muxless-vr-bridge-probe']:
 shutil.copy2(root/'shim/build-portable'/name,app/'usr/bin'/name)
shutil.copy2(root/'shim/build-portable/libmuxless_vr_bridge.so',app/'usr/lib/libmuxless_vr_bridge.so')
for name in ['AppRun','muxless-vr-bridge.desktop','muxless-vr-bridge.png']:shutil.copy2(root/'shim/appimage'/name,app/name)
icon=app/'.DirIcon'
icon.symlink_to('muxless-vr-bridge.png')
licenses=app/'usr/share/licenses';licenses.mkdir(parents=True,exist_ok=True)
shutil.copy2(root/"LICENSE",licenses/"Muxless-VR-Bridge.GPL-3.0-or-later")
shutil.copy2(root/'shim/third_party/nlohmann/LICENSE.MIT',licenses/'nlohmann-json.MIT')
shutil.copy2(root/'shim/third_party/picosha2/LICENSE',licenses/'PicoSHA2.MIT')
for p in (root/'shim/build-portable/licenses').glob('COPYING*'):shutil.copy2(p,licenses/p.name)
files=[p for p in sorted(app.rglob('*')) if p.is_file() and p.name!='payload.json']
payload={'version':version,'baseline':'glibc 2.28; x86-64; host Vulkan and Wayland libraries','files':[{'path':str(p.relative_to(app)),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in files]}
(app/'payload.json').write_text(json.dumps(payload,indent=2)+'\n')
lock=json.loads((root/'shim/appimage/toolchain-lock.json').read_text());tool=Path('/tmp/muxless-vr-bridge-appimagetool.AppImage')
if not tool.exists():tool.write_bytes(urllib.request.urlopen(lock['tool_url']).read())
if hashlib.sha256(tool.read_bytes()).hexdigest()!=lock['tool_sha256']:raise SystemExit('appimagetool integrity check failed')
tool.chmod(0o755);output=dist/f'Muxless-VR-Bridge-{version}-x86_64.AppImage'
subprocess.run([str(tool),'-n','--mksquashfs-opt','-processors','--mksquashfs-opt','2',str(app),str(output)],env={**os.environ,'APPIMAGE_EXTRACT_AND_RUN':'1','ARCH':'x86_64'},check=True)
output.with_name(output.name+'.sha256').write_text(hashlib.sha256(output.read_bytes()).hexdigest()+'  '+output.name+'\n')
staging.cleanup()
print(output)
