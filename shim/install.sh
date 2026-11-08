#!/usr/bin/env bash
set -euo pipefail
base="$(cd -- "$(dirname -- "$0")" && pwd)"
version="$(cat "$base/VERSION")"
image="$base/dist/WMR-GPU-Bridge-$version-x86_64.AppImage"
[[ -x "$image" ]] || { echo 'Build with shim/package.sh, then install the resulting AppImage.' >&2; exit 1; }
exec "$image" --install "$@"
