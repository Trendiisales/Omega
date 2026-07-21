#!/usr/bin/env bash
# deadbox_ref_audit.sh -- DEAD-BOX REFERENCE GATE (added S-2026-07-14, latent-class
# sweep item 13; runs inside scripts/mac_canary_engines.sh).
#
# THE PROBLEM: the live production box is omega-new = 45.85.3.79; omega-vps =
# 185.167.119.59 is the RETIRED old box from the 2026-07-07 migration. On
# 2026-07-10 a whole session's deploys silently landed on the dead box because
# tooling still said `omega-vps` (see CLAUDE.md "WHICH BOX IS LIVE"). The refs
# were repointed piecemeal, but nothing STRUCTURAL stopped a new script (or a
# copy-pasted ssh line) from reintroducing the dead box. This gate does.
#
# WHAT IT DOES: greps operational source (tools/ scripts/ include/ src/ recursively
# + top-level *.ps1/*.sh/*.py) for `omega-vps` or `185.167.119.59`. Every hit must
# be covered by the ALLOWLIST below (file + EXACT expected line count + reason).
#   - file with hits, not allowlisted            -> FAIL (new dead-box ref)
#   - allowlisted file, count ABOVE expected     -> FAIL (a NEW ref crept in)
#   - allowlisted file, count BELOW expected     -> FAIL (stale allowlist: refs were
#     cleaned up -- tighten the entry so the gate stays truthful, content-parity rule)
# outputs/, docs, handoffs, incident postmortems are OUT of scope: dated history
# keeps the old address as record, deliberately.
#
# Adding a NEW intentional mention? It must be a comment explaining history or a
# guard that names the dead box to refuse it -- never a live target. Add the file
# here with the exact count and a reason, in the same commit.
set -uo pipefail
cd "$(dirname "$0")/.."

PATTERN='omega-vps|185\.167\.119\.59'
SELF="scripts/deadbox_ref_audit.sh"

# path|expected_line_count|reason
ALLOWLIST='
tools/omega_deploy.sh|3|hard-fail guard naming the dead box (refuses HOST=omega-vps) + S-2026-07-10 repoint history comment
scripts/migrate_stall_ledgers.sh|3|HISTORICAL one-shot (already ran at cutover); hard-fail guard at top refuses re-run; body kept as migration record
tools/gui/refresh_crypto_companion.sh|1|history comment: "was omega-vps=retired, S-2026-07-10" -- intentional record of the repoint
DEPLOY_S12_PS1_CONSOLIDATION.sh|3|HISTORICAL S12-era one-shot (already ran); old-box URLs in printed epilogue + HISTORICAL header naming them
tools/feeds_selftest.py|2|intentional history: L38 cutover explanation + L268 alias-DISABLED note (stale ssh-form docstrings repointed S-2026-07-14q)
scripts/mac_canary_engines.sh|5|this gate own wiring: comment block + echo lines NAME the dead box to explain what the gate blocks
tools/engine_watermark_audit.py|1|guard comment on VPS="omega-new" naming the dead box ("never omega-vps/185") to warn future editors -- intentional, mirrors the refresh_crypto_companion.sh entry
tools/engine_perf_watch.py|1|guard comment on HOST="omega-new" naming the dead box ("omega-vps = retired dead box, never use") -- intentional, mirrors the engine_watermark_audit.py entry (S-2026-07-17t; the S-17s commit landed before this gate saw it)
tools/omega_ps.sh|1|hard-fail case-guard refusing HOST=omega-vps/185.167.119.59 (FATAL exit; live desk is omega-new) -- intentional, mirrors the omega_deploy.sh entry (S-2026-07-21g helper shipped the guard before this gate saw it)
'

allowed_count() {  # $1 = path -> echoes expected count, or empty if not allowlisted
    echo "$ALLOWLIST" | awk -F'|' -v f="$1" '$1 == f { print $2 }'
}

fail=0
seen_files=""
while IFS= read -r -d '' f; do
    f="${f#./}"
    [ "$f" = "$SELF" ] && continue   # this gate defines the pattern; skip self
    n=$(grep -cE "$PATTERN" "$f" 2>/dev/null || true)
    [ "${n:-0}" -eq 0 ] && continue
    seen_files="$seen_files $f"
    want=$(allowed_count "$f")
    if [ -z "$want" ]; then
        echo "[deadbox-audit] FAIL: NEW dead-box reference(s) in NON-allowlisted file: $f ($n line(s))"
        grep -nE "$PATTERN" "$f" | sed 's/^/    /'
        echo "    -> repoint to omega-new, delete the dead path, or (intentional history/guard"
        echo "       ONLY) add an allowlist entry with exact count + reason in $SELF."
        fail=1
    elif [ "$n" -gt "$want" ]; then
        echo "[deadbox-audit] FAIL: $f has $n dead-box ref line(s), allowlist expects $want -- a NEW ref crept in:"
        grep -nE "$PATTERN" "$f" | sed 's/^/    /'
        fail=1
    elif [ "$n" -lt "$want" ]; then
        echo "[deadbox-audit] FAIL: $f has $n dead-box ref line(s), allowlist expects $want -- refs were"
        echo "    cleaned up; tighten (or drop) its allowlist entry in $SELF so the gate stays truthful."
        fail=1
    fi
done < <({ find tools scripts include src -type f \( -name '*.ps1' -o -name '*.sh' -o -name '*.py' \
                -o -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) -print0 2>/dev/null;
           find . -maxdepth 1 -type f \( -name '*.ps1' -o -name '*.sh' -o -name '*.py' \) -print0; })

# Allowlist entries whose file vanished or now has zero hits = stale allowlist.
while IFS='|' read -r path want reason; do
    [ -z "$path" ] && continue
    case " $seen_files " in *" $path "*) : ;; *)
        echo "[deadbox-audit] FAIL: allowlist entry '$path' matched no file/hits -- remove or fix the entry."
        fail=1;;
    esac
done <<< "$(echo "$ALLOWLIST" | grep -v '^[[:space:]]*$')"

if [ "$fail" -ne 0 ]; then
    echo "[deadbox-audit] RESULT: FAIL -- the dead box (omega-vps / 185.167.119.59) must not be a target."
    exit 1
fi
echo "[deadbox-audit] PASS: no unexpected omega-vps / 185.167.119.59 references (allowlisted intentional mentions only)."
exit 0
