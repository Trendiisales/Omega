#!/usr/bin/env bash
set -euo pipefail

OS="$(uname -s)"

if [[ "$OS" == "Linux" ]]; then
  command -v taskset >/dev/null || { echo "taskset not found"; exit 1; }
elif [[ "$OS" == "Darwin" ]]; then
  command -v sysctl >/dev/null || { echo "sysctl not found"; exit 1; }
else
  echo "unsupported OS: $OS"
  exit 1
fi

echo "Runtime validation passed"
