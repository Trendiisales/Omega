#!/usr/bin/env bash
# accessor_resolution_audit.sh (S-2026-07-23w) — closes the MSVC-only C2039 class.
#
# WHY: OmegaBacktest (the Mac canary's compile target) does NOT compile
# engine_init.hpp / on_tick.hpp (Windows-only TU), so a call to a misspelled or
# missing omega::accessor() passes the Mac canary and dies at MSVC on the VPS
# deploy. This happened THREE times on 2026-07-23 alone:
#   * on_tick n_dm7/n_b3 (missing includes -> C2039)  [fixed by direct includes]
#   * engine_init dual_mom_mimic_book vs the header's dualmom_mimic_book (typo'd
#     accessor name -> C2039, deploy 1c918e1f FAILED, recovery restart)
# This audit greps every `omega::<name>()` niladic call in the Windows-only TU
# headers and requires a matching `<name>()` declaration somewhere in include/.
# Grep-level (not a compiler), but it catches exactly the class that burned us.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TU_HEADERS=("$ROOT/include/engine_init.hpp" "$ROOT/include/on_tick.hpp" "$ROOT/include/omega_main.hpp")
fails=0
calls=$(grep -hoE 'omega::[a-zA-Z_][a-zA-Z0-9_]*\(\)' "${TU_HEADERS[@]}" 2>/dev/null \
        | sed 's/omega:://; s/()//' | sort -u)
n=0
for fn in $calls; do
  n=$((n+1))
  # declaration forms: `Type& fn()` / `Type &fn()` / `fn() noexcept` etc — require
  # the token immediately followed by `(` somewhere in include/ outside the TU headers.
  if ! grep -rqE "(^|[^a-zA-Z0-9_])${fn}[[:space:]]*\(" \
        --include='*.hpp' "$ROOT/include" \
        --exclude=engine_init.hpp --exclude=on_tick.hpp --exclude=omega_main.hpp; then
    echo "[accessor-audit] UNRESOLVED: omega::${fn}() called in a Windows-only TU header but no '${fn}(' declaration found in include/ -- MSVC C2039 waiting to happen"
    fails=$((fails+1))
  fi
done
echo "[accessor-audit] ${n} distinct omega::accessor() calls checked, ${fails} unresolved"
[ "$fails" -eq 0 ] || exit 1
exit 0
