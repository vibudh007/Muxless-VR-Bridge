#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 shim/generate_trace.py
python3 shim/generate_bridge.py
c++ -std=c++17 -O2 -g -shared -fPIC -fvisibility=hidden -Wl,--version-script=shim/exports.map -Wall -Wextra -Wno-missing-field-initializers shim/layer.cpp -Ishim/build -o shim/build/libwmr_gpu_bridge.so.tmp -pthread -lvulkan $(pkg-config --cflags --libs libdrm)
mv shim/build/libwmr_gpu_bridge.so.tmp shim/build/libwmr_gpu_bridge.so

c++ -std=c++17 -O2 -Wall -Wextra shim/launcher.cpp -o shim/build/wmr-gpu-bridge.tmp
mv shim/build/wmr-gpu-bridge.tmp shim/build/wmr-gpu-bridge
