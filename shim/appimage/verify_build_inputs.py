#!/usr/bin/env python3
"""Reject changed vendored inputs before using the pinned build toolchain."""
import hashlib,json
from pathlib import Path
root=Path(__file__).resolve().parents[2]
lock=json.loads((root/'shim/appimage/toolchain-lock.json').read_text())
for relative,expected in lock['third_party'].items():
    path=root/relative
    actual=hashlib.sha256(path.read_bytes()).hexdigest()
    if actual!=expected:raise SystemExit('Build input checksum mismatch: '+relative)
container=(root/'shim/appimage/Containerfile').read_text()
if ('FROM '+lock['base_image']+'\n') not in container:
    raise SystemExit('Container base does not match toolchain-lock.json')
print('PASS: vendored source hashes and pinned container base match the lock file.')
