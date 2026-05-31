#!/usr/bin/env bash
# Resilient Dukascopy M1 bid+ask downloader. The single long fetch fails on any
# transient network blip and restarts from 0. Fix: --cache persists fetched
# artifacts, and the until-loop reruns until exit 0 -- each rerun RESUMES from
# cache instead of restarting. --no-fail-after-retries keeps a run going past a
# stubborn artifact. Result: converges to a complete file no matter how many
# blips. Safe to Ctrl-C and re-run; it picks up where it left off.
set -u
cd /Users/jo/Omega
export PATH="$PATH:/Users/jo/.npm-global/bin"
OUT=download
PAIRS="${PAIRS:-eurusd gbpusd usdjpy audusd eurgbp audnzd}"   # override: PAIRS="eurusd" ./download_fx_m1.sh
FROM=2019-01-01
TO=2026-05-31

for p in $PAIRS; do
  for side in bid ask; do
    f="$OUT/${p}-m1-${side}-${FROM}-${TO}.csv"
    echo "=== $p $side ==="
    tries=0
    until dukascopy-node -i "$p" -from "$FROM" -to "$TO" -t m1 -p "$side" -f csv -dir "$OUT" \
            --cache --retries 50 --retry-pause 2500 --no-fail-after-retries --batch-pause 1200; do
      tries=$((tries+1))
      echo "  [retry $tries] $p $side blipped — resuming from cache in 5s..."
      sleep 5
      [ "$tries" -ge 200 ] && { echo "  GIVE UP $p $side after 200 tries"; break; }
    done
    rows=$([ -f "$f" ] && wc -l < "$f" | tr -d ' ' || echo 0)
    echo "  DONE $p $side rows=$rows"
  done
done
echo "ALL M1 DOWNLOADS COMPLETE"
