#!/usr/bin/env python3
"""Exercise v14 adapter migration and both generations of AppImage remover."""
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
    assert "0.14.0" in bridge.read_text()
    legacy.write_bytes(original)
    run(new, "--install", "--no-steam-config")
    assert list(registration.glob("*.json")) == [bridge]
    assert not library.exists() and not receipt.exists()
    assert "0.15.0" in bridge.read_text()
    run(new, "--verify")
    assert not run(old, "--uninstall")["preserved_modified_files"]
    assert not bridge.exists()

    run(old, "--install", "--no-steam-config")
    assert not run(new, "--uninstall")["preserved_modified_files"]
    assert not bridge.exists()

print("PASS: v14 migration, modified-file protection, and both cross-version removal directions.")
