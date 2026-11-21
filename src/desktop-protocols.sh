#!/usr/bin/env bash
set -euo pipefail

output=${1:?Usage: bash src/desktop-protocols.sh BUILD_DIRECTORY [PROTOCOL_XML_DIRECTORY]}
protocols=${2:-}
if [[ -z $protocols ]] && ! protocols=$(pkg-config --variable=pkgdatadir wayland-protocols); then
    printf '%s\n' 'Install wayland-protocols or specify its XML directory.' >&2
    exit 1
fi
mkdir -p "$output"

for protocol in stable/xdg-shell/xdg-shell stable/viewporter/viewporter \
                stable/linux-dmabuf/linux-dmabuf-v1 stable/presentation-time/presentation-time \
                staging/linux-drm-syncobj/linux-drm-syncobj-v1; do
    xml="$protocols/$protocol.xml"
    if [[ ! -r $xml ]]; then
        printf 'Missing Wayland protocol %s\n' "$xml" >&2
        exit 1
    fi
    name=${protocol##*/}
    wayland-scanner client-header "$xml" "$output/$name-client-protocol.h"
    wayland-scanner private-code "$xml" "$output/$name-protocol.c"
done
