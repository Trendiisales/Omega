#!/usr/bin/env bash
# staleness_scan.sh — ONE sweep over the whole Omega staleness surface (STALENESS_REGISTRY.md).
#
# Born 2026-07-10 after the two-box incident: the freshness monitors existed but were
# scattered (feeds/protection/feedpath/data-health) and one pointed at a dead box, so
# nothing gave a single "is anything stale?" answer. This runs them all, aggregates, and
# exits non-zero if a LIVE-TRADING feed is stale. Runs at session start AND on cron
# (tools/install_staleness_cron.sh).
#
# Tiers:
#   LIVE     (blocking): feeds_selftest + protection_selftest — content-based, poll the
#            live box (omega-new), gate real trading. RED here => exit 1.
#   ADVISORY (non-blocking): feedpath (known to false-RED after a log rotation drops the
#            [IBKR-CONSUMER] boot line — see registry G-feedpath) + data_health (RESEARCH
#            feeds: rdagent model / qlib / crypto book — stale hurts research, not live
#            execution). Surfaced, but do NOT flip the live verdict.
#   DEPLOY-GATED (not run here): warm-seed freshness (seed_freshness_audit checks REPO
#            baselines that are old by design; the LIVE VPS seeds are refreshed by
#            OmegaSeedRefresh daily + re-audited at every deploy). Running it Mac-side
#            gives false REDs, so it is intentionally excluded — see registry §2.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY=/usr/bin/python3
live_red=0; adv=0; NL=$'\n'; SUMM=""

run() {  # tier  name  cmd...
  local tier="$1" name="$2"; shift 2
  local out rc tag="PASS"
  out="$("$@" 2>&1)"; rc=$?
  if [ $rc -ne 0 ] || echo "$out" | grep -qiE '\bRED\b|STALE|FAIL\b|NO-GO'; then
    tag="RED"; [ "$tier" = LIVE ] && live_red=$((live_red+1)) || adv=$((adv+1))
  fi
  SUMM+="  [${tier}/${tag}] ${name}${NL}"
  if [ "$tag" != PASS ]; then
    SUMM+="$(echo "$out" | grep -iE 'SELF-TEST|RED|STALE|FAIL|NO-GO|PRIMARY|MODEL' | head -3 | sed 's/^/       /')${NL}"
  fi
}

echo "🩺 OMEGA STALENESS SCAN — $(date '+%Y-%m-%d %H:%M') — registry: STALENESS_REGISTRY.md"
run LIVE     "feeds"       $PY "$ROOT/tools/feeds_selftest.py" --quiet
run LIVE     "protection"  $PY "$ROOT/tools/protection_selftest.py" --quiet
run LIVE     "deploy-drift" bash "$ROOT/tools/deploy_drift_check.sh"   # RED if running binary != origin/main (undeployed commits)
run ADVISORY "feedpath"    $PY "$ROOT/tools/feedpath_selftest.py"
[ -f "$ROOT/tools/data_health_monitor.py" ] && run ADVISORY "data-health(research)" $PY "$ROOT/tools/data_health_monitor.py" --quiet

echo "$SUMM"
if [ $live_red -gt 0 ]; then
  echo "RESULT: RED — ${live_red} LIVE feed(s) stale. Fix the producer (STALENESS_REGISTRY.md §5) before trusting signals."
  exit 1
fi
[ $adv -gt 0 ] && echo "RESULT: GREEN (live) — ${adv} advisory/research item(s) stale (non-blocking; see above)." && exit 0
echo "RESULT: GREEN — every monitored live + research feed fresh."
exit 0
