# RESEARCH ONLY — NOT LIVE TRUTH  (S-2026-07-16)

Files in this directory are the **Mac rdagent research + paper-basket book** (SEPARATE book;
it is the SOURCE of the desk STOCK BASKET panel via cron push_basket_to_desk.sh). They can LAG
(bulk yfinance chokes, no watcher). **Never reconstruct live equity state / "yesterday's close" /
fills from any file here.**

LIVE TRUTH (single source):
  - daily equity closes : VPS  C:\Omega\data\rdagent\sp500_long_close.csv  (OmegaStockMoverFeed)
  - realized fills      : ledger  ~/Omega-vps-mirror/logs/trades/omega_trade_closes.csv

Guard: run  python3 tools/live_truth_guard.py  before any live-state reconstruction.
Do NOT delete this directory — refresh_close_yf.py / blend_daily.sh / daymover / push_basket_to_desk.sh
(4 cron jobs) produce+consume it and it feeds the live desk panel.
