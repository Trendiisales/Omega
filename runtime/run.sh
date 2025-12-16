#!/usr/bin/env bash
set -euo pipefail

BIN="./chimera"
CORE_ENGINE=2
CORE_EXEC=3

if [[ ! -x "$BIN" ]]; then
  echo "binary not found or not executable"
  exit 1
fi

"$BIN" &
PID="$!"

sleep 0.2

"$(dirname "$0")/pin.sh" "$PID" "$CORE_ENGINE" "$CORE_EXEC"

wait "$PID"
