# OMEGA Deployment Guide

## Architecture
```
[Mac] GUI (React/Vite) ←── WebSocket ──→ [VPS] OMEGA Engine (C++)
                           Port 8765
```

## VPS Setup (Windows)

### 1. Copy Files to VPS
```bash
scp omega.exe config.ini user@45.85.3.38:C:/OMEGA/
```

### 2. Configure
Edit `config.ini`:
```ini
[engine]
symbol = BTCUSDT
mode = live
ws_port = 8765

[binance]
api_key = YOUR_API_KEY
secret_key = YOUR_SECRET_KEY
```

### 3. Run Engine
```cmd
cd C:\OMEGA
omega.exe config.ini
```

## Mac GUI Setup

### 1. Install Dependencies
```bash
cd GUI
npm install
```

### 2. Configure Connection
Edit `src/api/omegaClient.ts`:
```typescript
const WS_URL = "ws://45.85.3.38:8765";
```

### 3. Run GUI
```bash
npm run dev
```
Access at: http://localhost:5173

## Build from Source

### Linux/Mac Native
```bash
mkdir build && cd build
cmake ..
make -j4
```

### Windows Cross-Compile (Zig)
```bash
./build_windows_zig.sh
```

### Windows Native (MSVC)
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022"
cmake --build . --config Release
```

## Performance Tuning

### VPS Network
- Ensure VPS is in NY data center (Equinix NY5 preferred)
- Target latency: <0.5ms to Binance

### Engine Settings
```ini
[risk]
cooldown_ms = 50        # Minimum time between orders
max_position = 1        # Max open positions
min_confidence = 0.01   # Signal threshold
```

## Monitoring

GUI provides real-time:
- Latency metrics
- Position status
- Order flow
- Strategy signals
- Risk exposure

## Troubleshooting

### Connection Issues
1. Check firewall allows port 8765
2. Verify VPS IP in GUI config
3. Check engine is running: `netstat -an | grep 8765`

### Performance Issues
1. Monitor latency in GUI
2. Check VPS CPU/memory
3. Verify Binance API limits
