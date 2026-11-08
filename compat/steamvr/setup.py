#!/usr/bin/env python3
"""Install or remove the optional SteamVR shader layer for the current user."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as file:
        file.write(data)
        staging = Path(file.name)
    try:
        staging.replace(path)
    finally:
        staging.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "uninstall"))
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    folder = Path.home() / ".local/lib/wmr-steamvr-compat"
    library = folder / "libwmr-steamvr-compat.so"
    receipt = folder / "receipt.json"
    data = Path(os.getenv("XDG_DATA_HOME") or Path.home() / ".local/share")
    manifest = data / "vulkan/implicit_layer.d/wmr_steamvr_shader_compat.json"

    previous = json.loads(receipt.read_text()) if receipt.is_file() else {}
    for path, key in ((manifest, "manifest"), (library, "library")):
        if path.is_symlink() or (path.exists() and (not path.is_file() or digest(path) not in (previous.get(key), previous.get(f"previous_{key}")))):
            parser.error(f"refusing to change an untracked or modified file: {path}")
    if args.action == "uninstall":
        manifest.unlink(missing_ok=True)
        library.unlink(missing_ok=True)
        receipt.unlink(missing_ok=True)
        return

    layer = {"file_format_version": "1.2.0", "layer": {
        "name": "VK_LAYER_WMR_steamvr_shader_compat", "type": "GLOBAL",
        "library_path": str(library), "api_version": "1.3.0",
        "implementation_version": "1",
        "description": "Optional SteamVR shader compatibility",
        "disable_environment": {"DISABLE_WMR_STEAMVR_COMPAT": "1"},
    }}
    manifest_data = (json.dumps(layer, indent=2) + "\n").encode()
    output = source / "build" / library.name
    if not output.is_file():
        parser.error("build the optional layer with compat/steamvr/build.sh first")
    library_data = output.read_bytes()
    desired = {
        "library": hashlib.sha256(library_data).hexdigest(),
        "manifest": hashlib.sha256(manifest_data).hexdigest(),
    }
    # Record both acceptable hashes before replacing either file. A retry can
    # recover an interrupted replacement without adopting untracked contents.
    pending = {**desired}
    for path, key in ((manifest, "manifest"), (library, "library")):
        if path.is_file():
            pending[f"previous_{key}"] = digest(path)
    atomic_write(receipt, json.dumps(pending).encode())
    atomic_write(library, library_data)
    atomic_write(manifest, manifest_data)
    atomic_write(receipt, json.dumps(desired).encode())
    print(manifest)


if __name__ == "__main__":
    main()
