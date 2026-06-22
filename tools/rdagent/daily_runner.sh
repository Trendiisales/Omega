#!/bin/bash
# Daily RD-Agent → Omega runner. Refreshes the signal export + regime decision so
# the GUI shows today's LONG / CASH / SHORT call. Schedule pre-open (e.g. launchd/cron).
# Does NOT place orders — it proposes; execution stays gated/manual.
#
#   bash daily_runner.sh            # uses existing model run
#   crontab:  30 9 * * 1-5  bash /Users/jo/Omega/tools/rdagent/daily_runner.sh >> /tmp/rda-daily.log 2>&1
set -euo pipefail
source /opt/homebrew/Caskroom/miniforge/base/etc/profile.d/conda.sh

TOOLS="$HOME/Omega/tools/rdagent"
MLRUNS="${RDA_MLRUNS:-/tmp/omega_factors/mlruns}"
PROVIDER="$HOME/.qlib/qlib_data/omega_data"
TS="$(date '+%Y-%m-%d %H:%M')"

echo "[$TS] refreshing RD-Agent export + regime decision"
conda run -n rdagent4qlib python "$TOOLS/export_signals.py" \
  --mlruns "$MLRUNS" --provider "$PROVIDER" --region us \
  --universe BIGCAP --topk 5 --cost-bps 5 \
  --factors /tmp/omega_factors/discovered_factors.py

# surface today's decision in the log
python3 - "$HOME/Omega/data/rdagent/latest.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
t = d["strategy"]["today"]
print(f"  DECISION: {t['action']}  ({t['label']}, vol pctile {t['vol_percentile']}, "
      f"{'crash' if t.get('crash') else 'no-crash'})  -> {t['engine']}")
PY

# ensure the GUI sidecar is up
if ! curl -sf -o /dev/null http://127.0.0.1:7799/ 2>/dev/null; then
  echo "  starting GUI sidecar on :7799"
  nohup python3 "$TOOLS/gui/serve.py" --port 7799 >/tmp/rda-gui.log 2>&1 &
fi
echo "  GUI: http://127.0.0.1:7799"
