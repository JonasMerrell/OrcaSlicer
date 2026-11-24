#!/usr/bin/env bash
# End-to-end check that Z-pinning runs headless from the CLI: every z_pin_* setting from a process
# file and per object from a 3MF, read back out of the G-code. See test_cli_z_pins.py.
#
# usage: test_cli_z_pins.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

exec "$PY" "$(dirname "$0")/test_cli_z_pins.py" "$BIN" "$PROFILES"
