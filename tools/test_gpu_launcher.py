#!/usr/bin/env python3
import os,subprocess,tempfile,shutil
from pathlib import Path
with tempfile.TemporaryDirectory(prefix='wmr-gpu-launch-') as tmp:
 root=Path(tmp);(root/'bin').mkdir();stub=root/'bin/steam'
 stub.write_text('#!/bin/sh\nprintf "%s\\n" "$DRI_PRIME" "$@"\n');stub.chmod(0o755)
 env={**os.environ,'PATH':str(root/'bin')+':'+os.environ['PATH']}
 cli=Path('shim/build/wmr-gpu-bridge').resolve()
 p=subprocess.run([str(cli),'--steam','steam://rungameid/250820'],env=env,text=True,capture_output=True)
 assert p.returncode==0 and p.stdout.splitlines()==['1','steam://rungameid/250820'],(p.returncode,p.stdout,p.stderr)
 # A real /proc process named steam with an incompatible startup environment.
 fake=root/'steam';shutil.copy2('/bin/sleep',fake)
 process=subprocess.Popen([str(fake),'20'],env={**os.environ,'DRI_PRIME':'0'})
 try:
  p=subprocess.run([str(cli),'--steam'],env=env,text=True,capture_output=True)
  assert p.returncode==1 and 'already running without verified DRI_PRIME=1' in p.stderr,(p.returncode,p.stdout,p.stderr)
 finally:process.terminate();process.wait(timeout=5)
print('PASS: Steam entrypoint exports DRI_PRIME=1 and preserves arguments; actual incompatible Steam process blocks launch.')
