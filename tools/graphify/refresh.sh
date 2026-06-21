#!/usr/bin/env bash
# graphify refresh + validate — keep the Omega knowledge graph VIABLE (fresh), VALIDATED
# (integrity-tested), and NEVER STALE — fully local, zero external egress.
#
# Reindex path = `graphify extract --backend ollama`:
#   - code  -> on-device tree-sitter AST (cached; only changed files re-parsed)
#   - docs  -> LOCAL Ollama LLM (only new/changed docs; cache is content-keyed so the
#              original Claude-extracted doc nodes are preserved, Ollama only fills deltas)
#   - nothing leaves the machine (OLLAMA_BASE_URL pinned to localhost, validated below)
#
# Then it ALWAYS tests (integrity gate + node-count regression guard) and stamps on success.
# A non-green gate is a hard failure — a stale/broken graph is never silently blessed.
#
# Usage:
#   tools/graphify/refresh.sh            # reindex (code+docs) + test + stamp
#   tools/graphify/refresh.sh --check    # test only (no reindex) — pre-query / CI gate
#   tools/graphify/refresh.sh --code     # fast code-only reindex (no LLM) + test + stamp
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$REPO/graphify-out"
cd "$REPO"
export PYTHONHASHSEED=0   # deterministic clustering (matches the commit hook)

MODE="${1:-reindex}"
DROP_THRESHOLD_PCT=10
RED=$'\033[31m'; GRN=$'\033[32m'; YEL=$'\033[33m'; NC=$'\033[0m'
say(){ printf '%s\n' "$*"; }
die(){ printf '%s[graphify] FAIL: %s%s\n' "$RED" "$*" "$NC" >&2; exit 1; }

[ -f "$OUT/graph.json" ]        || die "no graph.json — run a full build first (/graphify . in Claude Code)"
[ -f "$OUT/.graphify_python" ]  || die "no .graphify_python — graph dir incomplete"
PY="$(cat "$OUT/.graphify_python")"
command -v graphify >/dev/null 2>&1 || export PATH="$HOME/.local/bin:$PATH"

# --- local LLM backend (zero egress) ---
export OLLAMA_BASE_URL="${OLLAMA_BASE_URL:-http://localhost:11434/v1}"
export OLLAMA_API_KEY="${OLLAMA_API_KEY:-local}"          # any non-empty value; silences the egress warning
export OLLAMA_MODEL="${OLLAMA_MODEL:-qwen2.5-coder:7b}"
case "$OLLAMA_BASE_URL" in
  http://localhost:*|http://127.0.0.1:*) : ;;
  *) die "OLLAMA_BASE_URL=$OLLAMA_BASE_URL is not localhost — refusing (would egress Omega docs)";;
esac

# ---- single-flight lock — hook + 2h timer + manual runs must never overlap ----
# (concurrent extracts race on graphify-out/ and can corrupt graph.json). --check
# is read-only and skips the lock. macOS has no flock, so use an atomic mkdir lock.
LOCK="$OUT/.graphify_refresh.lock"
if [ "$MODE" != "--check" ]; then
  STALE=1800
  if ! mkdir "$LOCK" 2>/dev/null; then
    started=$(cat "$LOCK/epoch" 2>/dev/null || echo 0)
    now=$(date +%s); age=$((now - started))
    if [ "$started" -gt 0 ] && [ "$age" -lt "$STALE" ]; then
      say "${YEL}[graphify] another reindex running (lock held ${age}s) — skipping.${NC}"; exit 0
    fi
    say "${YEL}[graphify] stale lock (${age}s) — reclaiming.${NC}"
    rm -rf "$LOCK"; mkdir "$LOCK" 2>/dev/null || { say "${YEL}[graphify] lock race — skipping.${NC}"; exit 0; }
  fi
  date +%s > "$LOCK/epoch"; echo $$ > "$LOCK/pid"
  trap 'rm -rf "$LOCK"' EXIT INT TERM
fi

node_count(){ "$PY" -c "import json;print(len(json.load(open('$OUT/graph.json'))['nodes']))" 2>/dev/null || echo 0; }
BEFORE="$(node_count)"

# ---- 1. reindex ----
if [ "$MODE" = "--code" ]; then
  say "[graphify] code-only reindex (AST, no LLM)…"
  graphify update "$REPO" 2>&1 | sed 's/^/  /' || die "graphify update errored"
elif [ "$MODE" != "--check" ]; then
  # ensure a local extraction model is present before routing docs to Ollama
  if ! curl -s -m 3 "http://localhost:11434/api/tags" 2>/dev/null | grep -q "\"${OLLAMA_MODEL%%:*}"; then
    say "${YEL}[graphify] Ollama model '$OLLAMA_MODEL' not found — falling back to code-only reindex.${NC}"
    say "${YEL}            Pull it for doc reindex:  ollama pull $OLLAMA_MODEL${NC}"
    graphify update "$REPO" 2>&1 | sed 's/^/  /' || die "graphify update errored"
  else
    say "[graphify] full reindex — code (AST) + docs (local Ollama: $OLLAMA_MODEL)…"
    graphify extract "$REPO" --backend ollama 2>&1 | sed 's/^/  /' || die "graphify extract errored"
  fi
fi

# ---- 1b. deletion sweep — drop ghost nodes whose source_file no longer exists ----
# graphify's incremental CLI does not reliably prune deleted files, so we guarantee
# it here: any node citing a source_file that is gone from disk is removed, along
# with its links and hyperedge memberships. This is what keeps deletions from leaving
# stale answers in the graph.
if [ "$MODE" != "--check" ]; then
  "$PY" - "$OUT/graph.json" "$REPO" <<'PYEOF'
import json,sys,os
gp,repo=sys.argv[1],sys.argv[2]
g=json.load(open(gp))
def missing(sf):
    if not sf: return False
    return not (os.path.exists(sf) or os.path.exists(os.path.join(repo,sf)))
ghosts={n['id'] for n in g['nodes'] if missing(n.get('source_file',''))}
if ghosts:
    g['nodes']=[n for n in g['nodes'] if n['id'] not in ghosts]
    g['links']=[e for e in g.get('links',[]) if e.get('source') not in ghosts and e.get('target') not in ghosts]
    he=[]
    for h in g.get('hyperedges',[]):
        members=[m for m in h.get('nodes',[]) if m not in ghosts]
        if len(members)>=2:
            h['nodes']=members; he.append(h)
    g['hyperedges']=he
    json.dump(g,open(gp,'w'),ensure_ascii=False)
    print(f"[graphify] deletion sweep: pruned {len(ghosts)} ghost node(s) from deleted files")
PYEOF
fi

# ---- 2. integrity gate (ALWAYS) ----
say "[graphify] integrity test…"
DIAG="$("$PY" - "$OUT/graph.json" <<'PYEOF'
import json,sys,subprocess
out=subprocess.run(["graphify","diagnose","multigraph","--json","--graph",sys.argv[1]],
                   capture_output=True,text=True)
try:
    s=json.loads(out.stdout)["summary"]
except Exception:
    print("PARSE_FAIL"); sys.exit(0)
fail_keys=["missing_endpoint_edges","dangling_endpoint_edges","non_object_edges"]
vals={k:int(s.get(k) or 0) for k in fail_keys}
pberr=s.get("post_build_error") or ""
bad=sum(vals.values()) + (1 if pberr else 0)
nodes=s.get("node_count"); edges=s.get("raw_edge_count")
extra=f" post_build_error='{pberr}'" if pberr else ""
print(f"NODES={nodes} EDGES={edges} BAD={bad} " + " ".join(f"{k}={vals[k]}" for k in fail_keys) + extra)
PYEOF
)"
say "  $DIAG"
case "$DIAG" in
  *"BAD=0"*) : ;;
  PARSE_FAIL*) die "diagnose JSON unparsed — cannot validate graph" ;;
  *) die "graph integrity broken: $DIAG" ;;
esac

# ---- 3. regression guard ----
AFTER="$(node_count)"
if [ "$BEFORE" -gt 0 ] && [ "$AFTER" -gt 0 ]; then
  DROP=$(( (BEFORE - AFTER) * 100 / BEFORE ))
  if [ "$DROP" -gt "$DROP_THRESHOLD_PCT" ]; then
    die "node count dropped ${DROP}% ($BEFORE -> $AFTER) > ${DROP_THRESHOLD_PCT}% — refusing. If intentional, rebuild with /graphify . "
  fi
  say "[graphify] node count $BEFORE -> $AFTER (Δ $((AFTER-BEFORE)), ok)"
fi

# ---- 4. stamp on success ----
if [ "$MODE" = "--check" ]; then
  say "${GRN}[graphify] OK (check-only)${NC}"
else
  printf '%s %sZ\n' "$(git rev-parse HEAD 2>/dev/null || echo nogit)" "$(date -u +%Y-%m-%dT%H:%M:%S)" > "$OUT/.graphify_stamp"
  say "${GRN}[graphify] OK — reindexed + tested + stamped $(awk '{print $1}' "$OUT/.graphify_stamp" | cut -c1-8) @ $(awk '{print $2}' "$OUT/.graphify_stamp")${NC}"
fi
