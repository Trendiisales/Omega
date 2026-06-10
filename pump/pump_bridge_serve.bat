@echo off
REM Integrated mode: run the pump feed bridge as a TCP server for the in-Omega
REM PumpFeedConsumer (OMEGA_PUMP_BRIDGE=1). Also serves the scanner web page on
REM http://<vps>:7783. No standalone pump_shadow.exe here -- Omega is the consumer.
cd /d C:\Omega\pump
python pump_feed_bridge.py --serve 7782 >> pump_bridge.log 2>&1
