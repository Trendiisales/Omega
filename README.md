# Omega — Commodities & Indices Trading System

**Strategy:** Compression Breakout (CRTP engine, zero virtual dispatch)  
**Broker:** BlackBull Markets — same FIX stack as ChimeraMetals  
**Primary symbols:** MES · MNQ · MCL  
**Confirmation:** ES · NQ · CL · VIX · DX · ZN · YM · RTY  
**GUI:** HTTP :7779 / WebSocket :7780  
**Mode:** Shadow (default)

## Build (Windows + MSVC)
```
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Run
```
build\Release\Omega.exe omega_config.ini
```

## GUI
Open `http://localhost:7779` in browser.

## Baseline Report (PowerShell on VPS)
Run expectancy/profit-factor summary from the full trade CSV:

```powershell
Set-Location C:\Omega
powershell -ExecutionPolicy Bypass -File .\BASELINE_REPORT.ps1 -CsvPath "C:\Omega\build\Release\logs\trades\omega_trade_closes.csv" -MinTrades 30
```
