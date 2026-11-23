#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

exec ghostty \
    --config-file="$SCRIPT_DIR/../ghostty.conf" \
    -e rmpc "$@"
