#!/bin/bash
# S-2026-06-26: DURABLE qlib model RETRAIN. THE missing piece behind the recurring "model 8d stale".
# qlib_refresh.sh refreshes DATA; export_signals re-applies the OLD pred.pkl -> as_of never advances.
# Nothing retrained the model. The original config lived in ephemeral /tmp (wiped on reboot). This:
#   1. renders the durable template with end_time = latest qlib calendar date (dynamic, no hardcoded date)
#   2. qrun -> fresh pred.pkl in the mlruns store export_signals reads
#   3. export_signals -> latest.json   4. execute_basket -> fresh as_of  -> data-health flips GO
# Logs every step to retrain_qlib.log so a failure is never silent again.
set -uo pipefail
export MLFLOW_ALLOW_FILE_STORE=true   # S-2026-06-26: mlflow upgrade now blocks the file-store backend by
# default -> every retrain since silently failed here (THE recurrence root cause). Opt back in to file store.
PY=/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python
QRUN=/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/qrun
TOOLS="$HOME/Omega/tools/rdagent"; QD="$HOME/.qlib/qlib_data/omega_data"
WORK=/tmp/omega_factors; LOG="$TOOLS/retrain_qlib.log"; TS="$(date '+%Y-%m-%d %H:%M')"
exec > >(tee -a "$LOG") 2>&1
trap 'echo "[$TS] retrain_qlib: EXIT status=$?"' EXIT
LATEST="$(tail -1 "$QD/calendars/day.txt")"
if [ -z "$LATEST" ]; then echo "[$TS] retrain: no calendar date — abort"; exit 1; fi
echo "[$TS] retrain: START — training through $LATEST"
mkdir -p "$WORK"
sed "s/@@END@@/$LATEST/g" "$TOOLS/factor_conf.yaml.template" > "$WORK/conf.yaml"
cd "$WORK" || exit 1
if ! "$QRUN" conf.yaml >/dev/null 2>"$WORK/qrun.err"; then
  echo "[$TS] retrain: qrun FAILED — $(tail -2 "$WORK/qrun.err" | tr '\n' ' ')"; exit 1
fi
echo "[$TS] retrain: qrun done — fresh pred.pkl"
# re-export latest.json from the freshest run, then re-fill the paper basket off the new model
"$PY" "$TOOLS/export_signals.py" --mlruns "$WORK/mlruns" --provider "$QD" --region us \
    --universe BIGCAP --topk 5 --cost-bps 5 >/dev/null 2>&1 && echo "[$TS] retrain: latest.json re-exported" || echo "[$TS] retrain: export_signals FAILED"
"$PY" "$TOOLS/execute_basket.py" --topk 5 --capital 10000 --mode shadow >/tmp/rda_basket.json 2>&1 \
  && echo "[$TS] retrain: basket re-filled -> as_of $($PY -c "import json;print(json.load(open('$HOME/Omega/data/rdagent/factor_basket_result.json')).get('as_of'))" 2>/dev/null)" \
  || echo "[$TS] retrain: execute_basket FAILED"
