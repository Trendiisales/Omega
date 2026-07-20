#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SOURCE="$SCRIPT_DIR/AurumBreakPullback.cpp"
OUTPUT="$HOME/Downloads/aurum_break_pullback"

if ! command -v clang++ >/dev/null 2>&1; then
  echo "clang++ was not found. Install Apple's command-line tools first:"
  echo "xcode-select --install"
  exit 1
fi

clang++ -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wpedantic \
  "$SOURCE" \
  -o "$OUTPUT"

chmod +x "$OUTPUT"

echo
echo "Build complete:"
echo "$OUTPUT"
echo
echo "Engine help:"
"$OUTPUT" --help
