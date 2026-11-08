#!/usr/bin/env bash
# One supported package format and installation owner.
set -euo pipefail
exec "$(dirname -- "$0")/appimage/build.sh" "$@"
