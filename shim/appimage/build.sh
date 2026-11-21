#!/usr/bin/env bash
set -euo pipefail
mode=${1:-}
if [[ $# -gt 1 || ( -n $mode && $mode != --payload-only ) ]]; then
 printf 'Usage: %s [--payload-only]\n' "$0" >&2
 exit 2
fi
cd "$(dirname -- "$0")/../.."
inputs=$(mktemp -d)
trap 'rm -rf -- "$inputs"' EXIT
python3 shim/appimage/verify_build_inputs.py --stage "$inputs"
podman build -t localhost/muxless-vr-bridge-build:glibc228 -f shim/appimage/Containerfile shim/appimage
podman run --rm --security-opt label=disable -v "$PWD:/work:rw" \
 --network none -v "$inputs:/inputs:ro" \
 -w /work \
 localhost/muxless-vr-bridge-build:glibc228 bash -c '
 set -e
 bash shim/build.sh --portable
 mkdir -p shim/build-portable/licenses
 rm -f -- shim/build-portable/licenses/COPYING.wayland-protocols shim/build-portable/licenses/COPYING.wayland-protocol-notices
 cp /usr/share/licenses/libgcc/COPYING* shim/build-portable/licenses/
 cp /inputs/licenses/COPYING* shim/build-portable/licenses/
 getconf GNU_LIBC_VERSION
 '
if [[ $mode != --payload-only ]]; then
 python3 shim/appimage/assemble.py
fi
