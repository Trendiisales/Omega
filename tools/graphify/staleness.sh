#!/usr/bin/env bash
# Surface Omega knowledge-graph DRIFT so it never rots silently.
# Mirrors the Memory-Wiki staleness banner: reports how far graphify-out/ lags HEAD.
# Read-only. Prints nothing if the graph is fresh (no noise on a clean session).
#
# Intended as a Claude Code SessionStart hook. To wire it (operator decision):
#   add to ~/.claude/settings.json  hooks.SessionStart[].hooks[]:
#     { "type": "command", "command": "/Users/jo/Omega/tools/graphify/staleness.sh" }
# Or run it manually any time:  tools/graphify/staleness.sh
REPO="/Users/jo/Omega"
OUT="$REPO/graphify-out"
STAMP="$OUT/.graphify_stamp"
[ -d "$REPO/.git" ] || exit 0
[ -f "$OUT/graph.json" ] || { echo "📉 GRAPHIFY: Omega has no knowledge graph yet — run '/graphify .' to build it."; exit 0; }

cd "$REPO" 2>/dev/null || exit 0
HEAD="$(git rev-parse --short HEAD 2>/dev/null)"

if [ ! -f "$STAMP" ]; then
  echo "📉 GRAPHIFY: graph built but never validated (no stamp). Run tools/graphify/refresh.sh to validate+stamp."
  exit 0
fi

LAST_SHA="$(awk 'NR==1{print $1}' "$STAMP")"
LAST_AT="$(awk 'NR==1{print $2}' "$STAMP")"
if ! git rev-parse "$LAST_SHA" >/dev/null 2>&1; then
  echo "📉 GRAPHIFY: graph stamp ($LAST_SHA) not in history (rebase/force-push?) — re-validate with tools/graphify/refresh.sh."
  exit 0
fi

BEHIND="$(git rev-list --count "$LAST_SHA"..HEAD 2>/dev/null || echo 0)"
CODE_CHG="$(git diff --name-only "$LAST_SHA" HEAD -- '*.cpp' '*.hpp' '*.h' '*.cc' '*.py' '*.ts' '*.tsx' 2>/dev/null | wc -l | tr -d ' ')"
DOC_CHG="$(git diff --name-only "$LAST_SHA" HEAD -- '*.md' '*.txt' 2>/dev/null | wc -l | tr -d ' ')"

if [ "${BEHIND:-0}" -eq 0 ]; then
  exit 0   # fresh — say nothing
fi

echo "📉 GRAPHIFY DRIFT — Omega graph is ${BEHIND} commit(s) behind HEAD (${LAST_SHA:0:8} @ ${LAST_AT} -> ${HEAD})."
[ "${CODE_CHG:-0}" -gt 0 ] && echo "   • ${CODE_CHG} code file(s) changed -> refresh (free): tools/graphify/refresh.sh"
[ "${DOC_CHG:-0}" -gt 0 ]  && echo "   • ${DOC_CHG} doc file(s) changed -> re-index in a session: /graphify . --update"
echo "   Stale graph -> stale answers. Refresh before trusting /graphify query on touched areas."
exit 0
