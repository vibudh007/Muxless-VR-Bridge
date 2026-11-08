#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p prototype/build
wayland-scanner client-header /usr/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml prototype/build/drm-lease-client.h
wayland-scanner private-code /usr/share/wayland-protocols/staging/drm-lease/drm-lease-v1.xml prototype/build/drm-lease-protocol.c
cc -c prototype/build/drm-lease-protocol.c -o prototype/build/drm-lease-protocol.o $(pkg-config --cflags wayland-client)
python3 prototype/compile_shader.py
c++ -std=c++17 -O2 -g -Wall -Wextra -Wno-missing-field-initializers prototype/main.cpp prototype/build/drm-lease-protocol.o -Iprototype/build $(pkg-config --cflags --libs vulkan wayland-client) -o prototype/build/bridge-toy
