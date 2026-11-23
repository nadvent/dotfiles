#!/usr/bin/env sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PICKER_BIN="$SCRIPT_DIR/../bin/rmpc-picker"

if [ -x "$PICKER_BIN" ]; then
    exec "$PICKER_BIN" "$@"
fi

rmpc remote status -l error "rmpc-picker binary missing (run make -C ~/.config/rmpc/picker)" >/dev/null 2>&1 || true
exit 1
