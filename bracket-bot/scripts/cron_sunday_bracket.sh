#!/bin/bash
# Cron entry for Sunday-open gold bracket strategy.
# Schedules launch at Sun 22:55 UTC (5min before CME globex resume).
#
# Install:
#   crontab -e
#   55 22 * * 0  /Users/jo/omega_repo/scripts/cron_sunday_bracket.sh >> /Users/jo/omega_repo/logs/cron.log 2>&1
#
# Args: pass --paper or --live as $1, --qty as $2
#   /Users/jo/omega_repo/scripts/cron_sunday_bracket.sh --paper 1
#   /Users/jo/omega_repo/scripts/cron_sunday_bracket.sh --live 5

set -e

OMEGA=/Users/jo/omega_repo
MODE=${1:---paper}
QTY=${2:-1}
INSTRUMENT=${3:-MGC}

cd "$OMEGA"
source .venv/bin/activate

LOG="logs/cron_$(date -u +%Y%m%d_%H%M%S).log"
echo "[$(date -u)] launching sunday_bracket --instrument=$INSTRUMENT --qty=$QTY $MODE" | tee -a "$LOG"

python live/sunday_bracket.py $MODE --instrument "$INSTRUMENT" --qty "$QTY" 2>&1 | tee -a "$LOG"

echo "[$(date -u)] done" | tee -a "$LOG"
