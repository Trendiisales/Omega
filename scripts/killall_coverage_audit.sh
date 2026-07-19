#!/usr/bin/env bash
# =============================================================================
# killall_coverage_audit.sh — KILL-ALL family-coverage gate (S-2026-07-20)
#
# WHY: the desk KILL ALL (/api/flatten -> g_flatten_all_request) flattens ONLY
# what g_open_positions.snapshot_all() can see. The set_exec book families
# (mimic/ladder/companion books wired straight to send_live_order) have NO
# register_source by design (independent-engine rule), so their live broker
# legs were INVISIBLE to a panic kill: real MGC/XAU/single-stock positions
# survived the button and the books kept managing/reopening
# (SESSION_HANDOFF_2026-07-20h). Fixed by per-family kill_all() fan-out in the
# on_tick flatten block. THIS gate makes the fix structural: a NEW book wired
# with set_exec whose singleton accessor is not killed in on_tick FAILS the
# mac canary — the S-17u Rider4h "patched individually, family never swept"
# class cannot recur silently.
#
# DERIVE-DON'T-COPY (persistence_audit.sh pattern): the book list is parsed
# from engine_init.hpp at runtime. Zero parsed set_exec sites = the regex
# rotted = FAIL exit 2, never a silent pass.
#
# Allowlist: scripts/killall_coverage_allowlist.txt — one entry per line
# (<var-or-accessor> <reason...>), for documented exceptions only.
# =============================================================================
set -u
cd "$(dirname "$0")/.."
EI=include/engine_init.hpp
OT=include/on_tick.hpp
ALLOW=scripts/killall_coverage_allowlist.txt

fail=0
vars=$(grep -oE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*\.set_exec\(' "$EI" \
       | sed -E 's/^[[:space:]]*//; s/\.set_exec\($//' | sort -u)
if [ -z "$vars" ]; then
    echo "[KILLALL-AUDIT] FAIL: zero set_exec sites parsed from $EI -- regex rot (derive-don't-copy gate)"
    exit 2
fi

allowed() {  # $1 = token; allowlisted iff a line starts with it
    [ -f "$ALLOW" ] && grep -qE "^$1([[:space:]]|$)" "$ALLOW"
}

n_ok=0
for v in $vars; do
    acc=$(grep -oE "auto& ${v} = omega::[A-Za-z0-9_]+\(\);" "$EI" | head -1 \
          | sed -E 's/.*omega::([A-Za-z0-9_]+)\(\);.*/\1/')
    if [ -z "$acc" ]; then
        if allowed "$v"; then continue; fi
        echo "[KILLALL-AUDIT] FAIL: set_exec book var '$v' has no 'auto& $v = omega::<accessor>();'"
        echo "                 decl in $EI and no allowlist entry -- cannot prove KILL-ALL coverage."
        fail=1; continue
    fi
    if grep -q "omega::${acc}().kill_all(" "$OT"; then
        n_ok=$((n_ok + 1))
    else
        if allowed "$acc" || allowed "$v"; then continue; fi
        echo "[KILLALL-AUDIT] FAIL: live-order book omega::${acc}() (var '$v') is NOT covered by"
        echo "                 the on_tick g_flatten_all_request family fan-out. Its open legs are"
        echo "                 INVISIBLE to the desk KILL ALL. Add <Book>::kill_all() + the fan-out"
        echo "                 call in $OT (S-2026-07-20 pattern), or document in $ALLOW."
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "[KILLALL-AUDIT] PASS: $n_ok set_exec book familie(s) covered by the on_tick KILL-ALL fan-out"
exit 0
