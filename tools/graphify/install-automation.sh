#!/usr/bin/env bash
# Wire the Omega graphify automation: post-commit reindex hook + launchd never-stale timer.
# Idempotent. Run from anywhere.   Uninstall:  tools/graphify/install-automation.sh --uninstall
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
HOOK_DST="$REPO/.git/hooks/post-commit"
PLIST_SRC="$HERE/com.omega.graphify-refresh.plist"
PLIST_DST="$HOME/Library/LaunchAgents/com.omega.graphify-refresh.plist"
GRN=$'\033[32m'; YEL=$'\033[33m'; NC=$'\033[0m'

if [ "${1:-}" = "--uninstall" ]; then
  [ -f "$HOOK_DST" ] && grep -q graphify-omega-hook "$HOOK_DST" 2>/dev/null && rm -f "$HOOK_DST" && echo "removed post-commit hook"
  if [ -f "$PLIST_DST" ]; then
    launchctl bootout "gui/$(id -u)/com.omega.graphify-refresh" 2>/dev/null || launchctl unload "$PLIST_DST" 2>/dev/null || true
    rm -f "$PLIST_DST" && echo "removed launchd timer"
  fi
  echo "${GRN}graphify automation uninstalled${NC}"
  exit 0
fi

# 1. post-commit hook
mkdir -p "$REPO/.git/hooks"
cp "$HERE/post-commit" "$HOOK_DST"
chmod +x "$HOOK_DST"
echo "post-commit hook -> $HOOK_DST"

# 2. launchd never-stale timer
mkdir -p "$HOME/Library/LaunchAgents" "$HOME/.cache"
cp "$PLIST_SRC" "$PLIST_DST"
launchctl bootout "gui/$(id -u)/com.omega.graphify-refresh" 2>/dev/null || true
if launchctl bootstrap "gui/$(id -u)" "$PLIST_DST" 2>/dev/null; then
  echo "launchd timer  -> loaded (every 2h + at login)"
else
  launchctl load "$PLIST_DST" 2>/dev/null && echo "launchd timer  -> loaded (legacy load)" \
    || echo "${YEL}launchd load failed — load manually: launchctl bootstrap gui/$(id -u) $PLIST_DST${NC}"
fi

echo "${GRN}graphify automation installed:${NC}"
echo "  • every commit  -> full reindex (code + local-Ollama docs) + integrity test + stamp"
echo "  • every 2h      -> same, as never-stale safety net (launchd)"
echo "  • log: ~/.cache/graphify-rebuild.log"
echo "  uninstall: tools/graphify/install-automation.sh --uninstall"
