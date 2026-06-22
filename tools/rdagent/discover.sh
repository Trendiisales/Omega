#!/bin/bash
# Weekly RD-Agent factor DISCOVERY + truth-gate, then refresh the GUI.
# MUST run in a context where `claude` is logged in (the discovery loop uses the
# Claude Code subscription via `claude -p`). Test once by hand before scheduling.
#
#   bash discover.sh                 # one discovery loop + export + validate
#   schedule: see com.omega.rdagent-discover.plist (launchd) — load only after a manual run works.
set -uo pipefail
source /opt/homebrew/Caskroom/miniforge/base/etc/profile.d/conda.sh
TOOLS="$HOME/Omega/tools/rdagent"
TS="$(date '+%Y-%m-%d %H:%M')"
echo "[$TS] RD-Agent discovery starting"

# 1. discovery loop (LLM via claude subscription) — the part that needs your login
conda activate rdagent
cd "$HOME/RD-Agent"
if ! rdagent fin_factor --loop_n 1; then
  echo "[$TS] discovery FAILED — is 'claude' logged in in this shell? (claude -p must work)"
  exit 1
fi

# 2. locate the freshest qlib run and export it to the GUI
RUN=$(find "$HOME/RD-Agent" -name pred.pkl -type f -newermt '-2 hours' 2>/dev/null \
        | xargs -I{} dirname {} | head -1)
if [ -z "$RUN" ]; then echo "[$TS] no fresh pred.pkl found"; exit 1; fi
conda run -n rdagent4qlib python "$TOOLS/export_signals.py" \
  --mlruns "$RUN" --provider "$HOME/.qlib/qlib_data/omega_data" --region us \
  --universe BIGCAP --topk 5 --cost-bps 5

# 3. truth-gate the standard factor set on a real universe (statistical power) for context
echo "[$TS] validation context (CSI300, both-regime, cost-aware):"
conda run -n rdagent4qlib python "$TOOLS/validate_factors.py" \
  --provider "$HOME/.qlib/qlib_data/cn_data" --region cn --market csi300 --cost-bps 5 2>/dev/null \
  | grep -E "factor|rev_5|combo|PASS" || true

# 4. ensure GUI up
curl -sf -o /dev/null http://127.0.0.1:7799/ 2>/dev/null || \
  nohup python3 "$TOOLS/gui/serve.py" --port 7799 >/tmp/rda-gui.log 2>&1 &
echo "[$TS] done — GUI http://127.0.0.1:7799"
