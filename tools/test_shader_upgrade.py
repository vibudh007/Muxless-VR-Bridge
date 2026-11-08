#!/usr/bin/env python3
"""Exercise legacy adapter migration and both generations of AppImage remover."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

old, new, adapter = (Path(arg).resolve() for arg in sys.argv[1:])
assert os.getpid() == 1, "Run in a private PID namespace using unshare"

with tempfile.TemporaryDirectory(prefix="wmr-shader-upgrade-") as tmp:
    home = Path(tmp)
    env = {
        **os.environ, "HOME": tmp, "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CONFIG_HOME": str(home / ".config"), "XDG_STATE_HOME": str(home / ".local/state"),
        "WMR_BRIDGE_PREFIX": str(home / ".local"), "WMR_STEAMVR_PATH": "",
        "WMR_OASIS_PATH": "", "APPIMAGE_EXTRACT_AND_RUN": "1",
    }

    for key, filename in (("WMR_STEAMVR_PATH", "vrcompositor"),
                          ("WMR_OASIS_PATH", "driver_oasis.so")):
        install = home / filename
        fixture = install / "bin/linux64" / filename
        fixture.parent.mkdir(parents=True)
        fixture.touch()
        env[key] = str(install)

    def run(image, *args, expected=0):
        result = subprocess.run([str(image), *args, "--json"], env=env,
                                capture_output=True, text=True, timeout=90)
        assert result.returncode == expected, (result.stdout, result.stderr)
        return json.loads(result.stdout) if result.stdout.strip() else None

    registration = home / ".local/share/vulkan/implicit_layer.d"
    bridge = registration / "wmr_gpu_bridge.json"
    legacy = registration / "wmr_steamvr_shader_compat.json"
    library = home / ".local/lib/wmr-steamvr-compat/libwmr-steamvr-compat.so"
    receipt = library.with_name("receipt.json")

    run(old, "--install", "--no-steam-config")
    subprocess.run(["python3", str(adapter), "install"], env=env, check=True)
    original = legacy.read_bytes()
    original_bridge = bridge.read_bytes()
    shortcut = home / ".local/share/applications/wmr-gpu-bridge-steam.desktop"
    assert shortcut.is_symlink()
    isolated = home / "other"
    normal_env = env.copy()
    env.update(XDG_DATA_HOME=str(isolated / "data"),
               XDG_CONFIG_HOME=str(isolated / "config"),
               XDG_STATE_HOME=str(isolated / "state"),
               WMR_BRIDGE_PREFIX=str(isolated / "prefix"))
    run(new, "--install", "--no-steam-config")
    run(new, "--uninstall")
    env = normal_env
    assert legacy.read_bytes() == original and library.exists() and receipt.exists()

    legacy.write_bytes(original + b"\n")
    run(new, "--install", "--no-steam-config", expected=1)
    assert legacy.read_bytes() == original + b"\n"
    assert bridge.read_bytes() == original_bridge
    legacy.write_bytes(original)
    run(new, "--install", "--no-steam-config")
    assert list(registration.glob("*.json")) == [bridge]
    assert not library.exists() and not receipt.exists()
    assert bridge.read_bytes() != original_bridge
    assert not os.path.lexists(shortcut)
    run(new, "--verify")
    assert not run(old, "--uninstall")["preserved_modified_files"]
    assert not bridge.exists()

    run(old, "--install", "--no-steam-config")
    assert not run(new, "--uninstall")["preserved_modified_files"]
    assert not bridge.exists()

print("PASS: legacy adapter migration, modified-file protection, and both cross-version removal directions.")
