#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname -- "$0")/.."
mkdir -p shim/build
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)
c++ "${flags[@]}" -O3 -DNDEBUG -shared -fPIC -fvisibility=hidden -DVK_NO_PROTOTYPES src/layer.cpp -o shim/build/libmuxless_vr_bridge.so -pthread -ldl $(pkg-config --cflags --libs libdrm)
c++ "${flags[@]}" -shared -fPIC -fvisibility=hidden patches/steamvr/layer.cpp -o shim/build/libsteamvr_runtime_patches.so -pthread
c++ "${flags[@]}" shim/launcher.cpp -o shim/build/wmr-gpu-bridge
c++ "${flags[@]}" shim/appimage/manager.cpp -o shim/build/muxless-vr-bridge-manager -ldl
c++ "${flags[@]}" shim/appimage/probe.cpp -o shim/build/muxless-vr-bridge-probe -lvulkan -lwayland-client

c++ "${flags[@]}" runtime/profile_helper.cpp -o shim/build/muxless-vr-profile
