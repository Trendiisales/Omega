#!/usr/bin/env bash
# commented_optin_audit.sh -- G2 guardrail (added 2026-07-21c).
#
# WHY: the NAS100 arm0 profit-lock was RESEARCHED (+34.4%), CODED, then left COMMENTED
# OUT and never enabled -- it bled giveback for weeks while a session "reported it done".
# No gate objected because a commented-out CERTIFIED IMPROVEMENT is invisible to the build.
# This audit makes that class fail the canary: every commented-out member-config assignment
# in engine_init.hpp must be in the allowlist with a documented "dead because..." reason.
# A NEW commented assignment (the next arm0) -> UNKNOWN -> FAIL -> forces a conscious decision:
# wire it, or delete it, or allowlist it with a reason. It can no longer silently rot.
#
# Exit 0 = clean. Exit 1 = an un-allowlisted commented config assignment (P1 regression).
set -euo pipefail
cd "$(dirname "$0")/.."

TARGET="include/engine_init.hpp"
ALLOW="scripts/commented_optin_allowlist.txt"

# commented member-assignment, with optional `if (...)` guard prefix (the exact arm0 form:
#   `// if (ic.tag=="NAS100") c.wide_arm_pct = 0.0;`). `=[^=]` excludes `==` comparisons.
RE='^[[:space:]]*//[[:space:]]*(if[[:space:]]*\(.*\)[[:space:]]*)?[A-Za-z_][A-Za-z0-9_.]*\.[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[^=]'

[ -f "$TARGET" ] || { echo "[commented-optin-audit] FAIL: $TARGET missing"; exit 2; }
[ -f "$ALLOW" ]  || { echo "[commented-optin-audit] FAIL: allowlist $ALLOW missing"; exit 2; }

# allowed signatures = allowlist lines with leading whitespace stripped, ignoring #-comments/blanks
norm() { sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//'; }

violations=0
while IFS= read -r line; do
    sig="$(printf '%s' "$line" | norm)"
    # is this trimmed hit present (as a trimmed line) in the allowlist?
    if ! grep -vE '^[[:space:]]*#' "$ALLOW" | norm | grep -qxF "$sig"; then
        if [ "$violations" -eq 0 ]; then
            echo "[commented-optin-audit] FAIL: un-allowlisted commented config assignment(s) in $TARGET"
            echo "  A commented-out config line is the arm0 class: certified fix left OFF, silently."
            echo "  Fix: wire it (uncomment + certify), DELETE it, or add to $ALLOW with a 'dead because' reason."
            echo "  ---"
        fi
        echo "  UNKNOWN: $sig"
        violations=$((violations+1))
    fi
done < <(grep -nE "$RE" "$TARGET" | sed -E 's/^[0-9]+://')

if [ "$violations" -gt 0 ]; then
    echo "  ---"
    echo "[commented-optin-audit] $violations un-allowlisted line(s) => FAIL (exit 1)"
    exit 1
fi
echo "[commented-optin-audit] OK -- all commented config assignments in $TARGET are allowlisted (documented-dead)."
exit 0
