#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname -- "$0")/../.."
python3 shim/appimage/verify_build_inputs.py
podman build -t localhost/muxless-vr-bridge-build:glibc228 -f shim/appimage/Containerfile .
podman run --rm --security-opt label=disable -v "$PWD:/work:rw" \
 -v /usr/include/vulkan:/headers/vulkan:ro -v /usr/include/vk_video:/headers/vk_video:ro \
 -w /work \
 localhost/muxless-vr-bridge-build:glibc228 bash -c '
 set -e
 mkdir -p shim/build-portable
 flags="-std=c++17 -O2 -static-libstdc++ -static-libgcc"
 g++ $flags -O3 -DNDEBUG -shared -fPIC -fvisibility=hidden -Wl,--exclude-libs,ALL -Wl,--no-undefined -I/headers -DVK_NO_PROTOTYPES src/layer.cpp -o shim/build-portable/libmuxless_vr_bridge.so -pthread -ldl $(pkg-config --cflags --libs libdrm) -lstdc++fs
 g++ $flags -shared -fPIC -fvisibility=hidden -Wl,--exclude-libs,ALL -Wl,--no-undefined -I/headers patches/steamvr/layer.cpp -o shim/build-portable/libsteamvr_runtime_patches.so -pthread
 g++ $flags runtime/profile_helper.cpp -o shim/build-portable/muxless-vr-profile -lstdc++fs
 g++ $flags shim/launcher.cpp -o shim/build-portable/wmr-gpu-bridge -lstdc++fs
 g++ $flags shim/appimage/manager.cpp -o shim/build-portable/muxless-vr-bridge-manager -ldl -lstdc++fs
 g++ $flags -I/headers shim/appimage/probe.cpp -o shim/build-portable/muxless-vr-bridge-probe -lvulkan -lwayland-client
 mkdir -p shim/build-portable/licenses
 cp /usr/share/licenses/libgcc/COPYING* shim/build-portable/licenses/
 getconf GNU_LIBC_VERSION
 '
python3 shim/appimage/assemble.py
