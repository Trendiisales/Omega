@echo off
REM Run the pump shadow pipeline: Python IBKR feed bridge -> C++ shadow engine.
REM SHADOW ONLY — no orders. Logs to pump_shadow_trades.csv + pump_shadow.log.
cd /d C:\Omega\pump
python pump_feed_bridge.py | pump_shadow.exe --gate 100 --trail 3 --pyr 0 >> pump_shadow.log 2>&1
