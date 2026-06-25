#!/bin/bash
# S-2026-06-25 qlib DATA refresh (greens the data-health qlib flag). Pulls fresh OHLCV for the
# model's bigcap universe from IBKR (tunnel up) and re-dumps the qlib dataset, then re-exports the
# panel. NOTE: this refreshes qlib DATA + the panel export; fresh model PREDICTIONS (rankings) still
# need the rdagent inference pipeline (heavier, separate). Tunnel-gated: skips cleanly if IBKR down.
set -uo pipefail
PY=/opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python
TOOLS="$HOME/Omega/tools/rdagent"; QD="$HOME/.qlib/qlib_data/omega_data"; TS="$(date '+%Y-%m-%d %H:%M')"
if ! nc -z -G3 127.0.0.1 4001 2>/dev/null; then echo "[$TS] qlib_refresh: no IBKR tunnel — skip"; exit 0; fi
TMP=$(mktemp -d)
python3 "$TOOLS/refresh_close_ibkr.py" --tickers bigcap >/dev/null 2>&1 || true   # also freshens close basket
# pull OHLCV for the bigcap universe -> qlib
python3 - "$QD" "$TMP" <<'PYEOF'
import sys, os
from ibapi.client import EClient; from ibapi.wrapper import EWrapper; from ibapi.contract import Contract
import threading, time
qd, tmp = sys.argv[1], sys.argv[2]
uni=[l.split()[0] for l in open(f"{qd}/instruments/bigcap.txt")]
class A(EWrapper,EClient):
    def __init__(s): EClient.__init__(s,s); s.r=False; s.b=[]; s.d=False; s.e=False
    def error(s,i,c,m,*a):
        if c in (162,200,354): s.e=True
    def nextValidId(s,o): s.r=True
    def historicalData(s,i,bar): s.b.append(bar)
    def historicalDataEnd(s,i,a,b): s.d=True
app=A(); app.connect('127.0.0.1',4001,clientId=1410)
threading.Thread(target=app.run,daemon=True).start()
t0=time.time()
while not app.r and time.time()-t0<15: time.sleep(0.1)
if not app.r: print("no handshake"); sys.exit(0)
for k,s in enumerate(uni):
    c=Contract(); c.symbol=s; c.secType='STK'; c.exchange='SMART'; c.currency='USD'
    app.b=[]; app.d=False; app.e=False
    app.reqHistoricalData(900+k,c,'','2 Y','1 day','ADJUSTED_LAST',1,1,False,[])
    t0=time.time()
    while not app.d and not app.e and time.time()-t0<15: time.sleep(0.1)
    if len(app.b)>200:
        with open(f"{tmp}/{s}.csv","w") as f:
            f.write("date,open,high,low,close,volume\n")
            for bar in app.b:
                d=str(bar.date)[:10]
                if '-' not in d: d=f"{d[:4]}-{d[4:6]}-{d[6:8]}"
                f.write(f"{d},{bar.open},{bar.high},{bar.low},{bar.close},{int(bar.volume) if bar.volume>0 else 1}\n")
    time.sleep(11)
app.disconnect()
PYEOF
if [ "$(ls $TMP/*.csv 2>/dev/null | wc -l)" -ge 20 ]; then
  $PY "$TOOLS/omega_to_qlib.py" --input "$TMP" --out "$QD" --universe BIGCAP && echo "[$TS] qlib re-dumped through $(tail -1 $QD/calendars/day.txt)"
  bash "$TOOLS/refresh_gui.sh" >/dev/null 2>&1 || true
else echo "[$TS] qlib_refresh: thin pull ($(ls $TMP/*.csv 2>/dev/null|wc -l)) — kept existing qlib"; fi
rm -rf "$TMP"
