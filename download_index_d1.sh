#!/usr/bin/env bash
# Resilient Dukascopy D1 index downloader (S44 indices mission). Mirrors
# download_fx_m1.sh discipline: --cache resumes, until-loop reruns past blips.
# 2019-2026 span deliberately includes the 2020 covid crash + 2022 bear so
# trend/short edges get a REAL regime test (the 2024-26 Tick data is all-bull).
set -u
cd /Users/jo/Omega
export PATH="$PATH:/Users/jo/.npm-global/bin"
OUT=download
# inst -> friendly name mapping handled by harness; download by inst id.
INSTS="${INSTS:-usa500idxusd usatechidxusd deuidxeur usa30idxusd gbridxgbp eusidxeur}"
FROM=2019-01-01
TO=2026-05-31

for inst in $INSTS; do
  f="$OUT/${inst}-d1-bid-${FROM}-${TO}.csv"
  echo "=== $inst d1 ==="
  tries=0
  until dukascopy-node -i "$inst" -from "$FROM" -to "$TO" -t d1 -p bid -f csv -dir "$OUT" \
          --cache --retries 50 --retry-pause 2500 --no-fail-after-retries --batch-pause 1200; do
    tries=$((tries+1))
    echo "  [retry $tries] $inst blipped — resuming from cache in 5s..."
    sleep 5
    [ "$tries" -ge 200 ] && { echo "  GIVE UP $inst after 200 tries"; break; }
  done
  rows=$([ -f "$f" ] && wc -l < "$f" | tr -d ' ' || echo 0)
  echo "  DONE $inst rows=$rows"
done
echo "ALL INDEX D1 DOWNLOADS COMPLETE"
