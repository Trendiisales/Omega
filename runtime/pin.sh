#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: pin.sh <pid> <core_engine> <core_exec>"
  exit 1
fi

PID="$1"
CORE_ENGINE="$2"
CORE_EXEC="$3"

OS="$(uname -s)"

if [[ "$OS" == "Linux" ]]; then
  taskset -cp "$CORE_ENGINE" "$PID" >/dev/null
  taskset -cp "$CORE_EXEC" "$PID" >/dev/null
  taskset -cp "$PID" | grep -q "$CORE_ENGINE" || exit 1
  taskset -cp "$PID" | grep -q "$CORE_EXEC" || exit 1
elif [[ "$OS" == "Darwin" ]]; then
  /usr/bin/true
else
  echo "unsupported OS"
  exit 1
fi
