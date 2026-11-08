#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname -- "$0")/../.."
python3 tools/verify_build_inputs.py
podman build -t localhost/wmr-bridge-build:glibc228 -f shim/appimage/Containerfile .
mkdir -p compat/steamvr/build
podman run --rm --security-opt label=disable -v "$PWD:/work:rw" \
 -v /usr/include/vulkan:/headers/vulkan:ro -v /usr/include/vk_video:/headers/vk_video:ro \
 -w /work localhost/wmr-bridge-build:glibc228 \
 g++ -I/headers -std=c++17 -O2 -shared -fPIC -fvisibility=hidden -static-libstdc++ \
 -static-libgcc -Wl,-z,defs -Wl,--version-script=compat/steamvr/exports.map \
 compat/steamvr/layer.cpp -pthread -o compat/steamvr/build/libwmr-steamvr-compat.so
