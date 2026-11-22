#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname -- "$0")/.."

out=shim/build
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -Wno-missing-field-initializers)
shared=(-shared -fPIC -fvisibility=hidden -Wl,-z,defs)
filesystem=()
desktop_sources=()
case "${1:-}" in
    --portable)
        out=shim/build-portable
        flags+=(-static-libstdc++ -static-libgcc -I/inputs/vulkan-headers/include)
        shared+=(-Wl,--exclude-libs,ALL)
        filesystem+=(-lstdc++fs)
        ;;
    --desktop-fixture)
        out=shim/build-desktop-fixture
        flags+=(-DMUXLESS_DESKTOP_FIXTURE)
        desktop_sources+=(src/desktop.cpp)
        ;;
    "") ;;
    *) printf 'Usage: %s [--portable|--desktop-fixture]\n' "$0" >&2; exit 2 ;;
esac

protocols="$out/protocols"
mkdir -p "$out"
modules=(libdrm)
if [[ ${#desktop_sources[@]} -ne 0 ]]; then
    bash src/desktop-protocols.sh "$protocols"
    modules+=(wayland-client)
fi
drm_cflags=$(pkg-config --cflags "${modules[@]}")
drm_libraries=$(pkg-config --libs "${modules[@]}")
read -ra include_flags <<< "$drm_cflags"
read -ra libraries <<< "$drm_libraries"
objects=()
if [[ ${#desktop_sources[@]} -ne 0 ]]; then
    for source in "$protocols/"*-protocol.c; do
        object=${source%.c}.o
        "${CC:-cc}" -O2 -fPIC "${include_flags[@]}" -c "$source" -o "$object"
        objects+=("$object")
    done
fi

cxx=${CXX:-c++}
"$cxx" "${flags[@]}" "${shared[@]}" -pthread -I"$protocols" "${include_flags[@]}" \
    src/layer.cpp src/transport.cpp src/virtual-display.cpp src/headset.cpp \
    src/output-audit.cpp "${desktop_sources[@]}" "${objects[@]}" "${libraries[@]}" -ldl "${filesystem[@]}" \
    -o "$out/libmuxless_vr_bridge.so"
"$cxx" "${flags[@]}" "${shared[@]}" patches/steamvr/layer.cpp -pthread -Wl,-z,nodelete \
    -o "$out/libsteamvr_runtime_patches.so"
"$cxx" "${flags[@]}" shim/launcher.cpp "${filesystem[@]}" -o "$out/wmr-gpu-bridge"
"$cxx" "${flags[@]}" shim/appimage/manager.cpp -ldl "${filesystem[@]}" -o "$out/muxless-vr-bridge-manager"
"$cxx" "${flags[@]}" shim/appimage/probe.cpp -lvulkan -lwayland-client -o "$out/muxless-vr-bridge-probe"
"$cxx" "${flags[@]}" runtime/profile_helper.cpp "${filesystem[@]}" -o "$out/muxless-vr-profile"
