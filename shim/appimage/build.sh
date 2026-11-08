#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname -- "$0")/../.."
python3 shim/appimage/verify_build_inputs.py
podman build -t localhost/muxless-vr-bridge-build:glibc228 -f shim/appimage/Containerfile .
podman run --rm --security-opt label=disable -v "$PWD:/work:rw" \
 -v /usr/include/vulkan:/headers/vulkan:ro -v /usr/include/vk_video:/headers/vk_video:ro \
 -v /usr/share/vulkan/registry:/usr/share/vulkan/registry:ro -w /work \
 localhost/muxless-vr-bridge-build:glibc228 bash -c '
 set -e
 python3 shim/generate_trace.py
 python3 shim/generate_bridge.py
 mkdir -p shim/build-portable
 flags="-std=c++17 -O2 -static-libstdc++ -static-libgcc"
 g++ $flags -shared -fPIC -fvisibility=hidden -Wl,--exclude-libs,ALL -Wl,--version-script=shim/exports.map -I/headers -Ishim/build shim/layer.cpp -o shim/build-portable/libmuxless_vr_bridge.so -pthread -lvulkan $(pkg-config --cflags --libs libdrm) -lstdc++fs
 g++ $flags shim/launcher.cpp -o shim/build-portable/wmr-gpu-bridge -lstdc++fs
 g++ $flags shim/appimage/manager.cpp -o shim/build-portable/muxless-vr-bridge-manager -ldl -lstdc++fs
 g++ $flags -I/headers shim/appimage/probe.cpp -o shim/build-portable/muxless-vr-bridge-probe -lvulkan -lwayland-client
 mkdir -p shim/build-portable/licenses
 cp /usr/share/licenses/libgcc/COPYING* shim/build-portable/licenses/
 getconf GNU_LIBC_VERSION
 '
python3 shim/appimage/assemble.py
