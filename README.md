# CHIMERA HFT Engine v4.0

## Quick Start

```bash
# Extract and run
tar xzf Chimera_v4_AUDITED.tar.gz
cd Chimera
chmod +x start.sh
./start.sh
```

This will:
1. Build the engine (first time only)
2. Start the metrics server on port 9001
3. Start the dashboard on port 8081
4. Open your browser to the dashboard

## Start Options

```bash
./start.sh                  # Default: testnet, shadow mode
./start.sh --build          # Force rebuild
./start.sh --production     # Use Binance production (real data)
./start.sh --live           # DANGER: Enable live trading
./start.sh --gui-only       # Start dashboard without engine
```

## Manual Build

```bash
cd Chimera
mkdir build && cd build
cmake -DBINANCE_USE_TESTNET=ON ..
make -j8
./chimera
```

In another terminal:
```bash
cd Chimera
python3 -m http.server 8081
# Open: http://localhost:8081/chimera_dashboard_v4.html
```

## Endpoints

| Endpoint | URL |
|----------|-----|
| Dashboard | http://localhost:8081/chimera_dashboard_v4.html |
| Metrics | http://localhost:9001/metrics |
| Health | http://localhost:9001/health |
| System | http://localhost:9001/system |
| Config | http://localhost:9001/config |

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    CHIMERA HFT ENGINE                       │
├─────────────┬───────────────────────────────┬───────────────┤
│  BINANCE    │         ENGINE CORE           │    cTRADER    │
│  WebSocket  │  ┌─────────┐  ┌───────────┐   │      FIX      │
│   (SSL)     │  │ Strategy│→ │  Intent   │   │     (SSL)     │
│     ↓       │  │ Runner  │  │   Queue   │   │       ↓       │
│  ┌─────┐    │  └─────────┘  └───────────┘   │   ┌─────┐     │
│  │Trade│    │       ↑             ↓         │   │Market│    │
│  │Feed │────┼───→ Ingress    Execution ─────┼───│ Data │    │
│  └─────┘    │      Queue      Router        │   └─────┘     │
└─────────────┴───────────────────────────────┴───────────────┘
                           ↓
                    ┌─────────────┐
                    │   Metrics   │ ← Dashboard
                    │   Server    │   (port 8081)
                    │  (port 9001)│
                    └─────────────┘
```

## Config File

Create `config/chimera.ini`:

```ini
[Binance]
Symbols = BTCUSDT, ETHUSDT, SOLUSDT
Enabled = true

[cTrader]
Host = demo-uk-eqx-02.p.c-trader.com
PortSSL = 5212
SenderCompID = demo.blackbull.2067070
TargetCompID = cServer
Username = 2067070
Password = your_password
Symbols = XAUUSD, EURUSD
Enabled = false
```

## Controls

- **Kill Engine**: POST to `/engine/kill` with `{"engine_id": 1}`
- **Restart Engine**: POST to `/engine/restart` with `{"engine_id": 1}`
- **Update Config**: POST to `/config` with new settings

## Requirements

- macOS or Linux
- CMake 3.16+
- C++17 compiler (clang/gcc)
- OpenSSL
- Python 3 (for dashboard server)
