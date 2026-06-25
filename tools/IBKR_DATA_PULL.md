# IBKR top-200 universe + history pull (read-only)

Pulls the live high-ADR universe + daily history straight from the IBKR gateway —
the EXACT names the BigCapMomo/Luke engine trades, no Yahoo throttle, split/div-adjusted.

## Setup
- Gateway on the VPS (localhost:4001). From Mac: tunnel
  `ssh -f -N -L 4001:127.0.0.1:4001 omega-vps`
- `pip install --break-system-packages ibapi`
- READ-ONLY: only reqScannerSubscription + reqHistoricalData. Unique clientId (not 86/87 = live engine). NO orders.

## 1. Universe — tools/ibkr_universe_scan.py
Union of 7 scan codes (TOP_PERC_GAIN/LOSE, MOST_ACTIVE, HOT_BY_VOLUME, HIGH_OPT_IMP_VOLAT[_OVER_HIST],
TOP_TRADE_COUNT), filtered marketCapAbove=20000 ($20B, MILLIONS unit) + abovePrice=10. ~188 rows ->
~120 after ETF filter. HIGH_OPT_IMP_VOLAT is the key add: surfaces high-ADR names IN A PAUSE (when
setup C fires), which TOP_PERC_GAIN (today's movers) misses. Writes /tmp/ib_uni.json.

## 2. History — tools/ibkr_history_pull.py
Per stock: reqHistoricalData '5 Y' '1 day' whatToShow=ADJUSTED_LAST useRTH=1. Pacing 11s/req
(~54/10min, under the 60/10min limit; auto-retry on err 162). Writes /tmp/ib_data/<SYM>.csv.

## Caveat — survivorship
Scanner scans NOW -> today's universe history is survivorship-biased. Fix: run the scanner DAILY
forward (cron) to accumulate a point-in-time universe; judge live on the shadow ledger (BACKTEST_TRUTH).
