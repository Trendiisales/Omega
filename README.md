# CHIMERA HFT v1.3.1

High-Frequency Trading System with Dual-Engine Architecture

## Overview

Chimera is a production-grade HFT system featuring:
- **Dual isolated engines** (Crypto + CFD) on separate CPU cores
- **Unified VenueHealth model** for protocol-agnostic venue management
- **10-bucket strategy voting** with CRTP-optimized microstructure analysis
- **Lock-free architecture** with atomic-only hot paths
- **Zero-copy FIX parsing** with preallocated resend buffer
- **Fast numeric parsing** - locale-free, allocation-free parsers
- **HFT-grade resend** - FIXResendRing wired into CTraderFIXClient runtime

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         CHIMERA HFT v1.2.2                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────┐              ┌─────────────────┐              │
│  │  CryptoEngine   │              │    CfdEngine    │              │
│  │   (CPU 1)       │              │    (CPU 2)      │              │
│  │                 │              │                 │              │
│  │  Binance WS     │              │    FIX 4.4      │              │
│  │       │         │              │        │        │              │
│  │       ▼         │              │        ▼        │              │
│  │  VenueHealth    │              │   VenueHealth   │              │
│  └────────┬────────┘              └────────┬────────┘              │
│           │                                │                        │
│           └───────────► Arbiter ◄──────────┘                        │
│                     (venue-agnostic)                                │
└─────────────────────────────────────────────────────────────────────┘
```

## Key Components

| Component | File | Description |
|-----------|------|-------------|
| CryptoEngine | `src/engine/CryptoEngine.hpp` | Binance WebSocket trading engine |
| CfdEngine | `src/engine/CfdEngine.hpp` | cTrader FIX 4.4 trading engine |
| VenueHealth | `src/venue/VenueHealth.hpp` | Unified venue health model |
| Arbiter | `src/arbiter/Arbiter.hpp` | Protocol-agnostic routing |
| Strategies | `src/strategy/Strategies_Bucket.hpp` | 10-bucket voting system |
| MicroEngines | `src/micro/MicroEngines_CRTP.hpp` | CRTP microstructure analysis |
| FIXSession (HOT) | `src/fix/session/FIXSession.hpp` | Zero-alloc FIX session with resend ring |
| FIXSession (COLD) | `src/fix/FIXSession.hpp` | Legacy session for admin/subscription |

## VenueState Model

```cpp
enum class VenueState : uint8_t {
    HEALTHY     = 0,   // Normal operation
    DEGRADED    = 1,   // Partial data - low urgency only
    UNAVAILABLE = 2    // No valid market view - NEVER trade
};
```

## Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Output: `build/bin/chimera`

## Configuration

Edit `config.ini`:
```ini
[binance]
symbols=BTCUSDT,ETHUSDT,SOLUSDT

[ctrader]
host=fix.blackbull.com
port=5201
sender=YOUR_SENDER
target=cServer
```

## Run

```bash
./build/bin/chimera --binance --ctrader --gui
```

## Directory Structure

```
src/
├── arbiter/      # Venue routing (protocol-agnostic)
├── binance/      # Binance WebSocket client
├── engine/       # CryptoEngine, CfdEngine
├── fix/          # FIX 4.4 protocol implementation
├── market/       # Tick, OrderBook structures
├── micro/        # CRTP microstructure engines
├── net/          # SSL WebSocket, TCP clients
├── risk/         # Risk guardian, kill switch
├── server/       # HTTP/WebSocket servers
├── strategy/     # 10-bucket strategy system
└── venue/        # Unified VenueHealth model
```

## Performance Targets

| Metric | Target |
|--------|--------|
| Tick-to-decision | < 1μs |
| Order latency (Binance) | 0.2ms (co-located) |
| Hot path allocations | 0 |
| Hot path locks | 0 |

## Version History

- **v1.3.1** - FIX Resend Ring WIRED to runtime
  - Embedded FIXResendRing directly into CTraderFIXClient (real runtime path)
  - All sends go through sendFIX() which stores in preallocated ring
  - handleResendRequest() replays from ring on disconnect storms
  - Deleted dead legacy FIXSession.cpp/hpp (was completely unused)

- **v1.3.0** - Phase 3 complete
  - Binance WS hot path fully migrated to fast_parse (locale-free, no alloc)
  - Created BinanceFastParse.hpp for JSON numeric parsing
  - Added MutexPolicy.hpp for compile-time hot path enforcement
  - Annotated 70+ mutex declarations as COLD_PATH_ONLY
  - Removed stringstream from Binance WS handlers

- **v1.2.2** - Final Phase 2 fixes
  - Fixed LatencyRiskBridge include paths (was dangling)
  - Wired FIXResendRing into FIXSession (now actually used)
  - Converted FIXReject to zero-copy getView() API
  - Deleted dead heartbeat helpers (FIXKeepAlive, FIXHeartbeatTransport)

- **v1.2.1** - Phase 2 HFT hardening
  - Zero-copy FIX field access (FIXFieldView, getView())
  - Fast numeric parsers (fast_parse_int/double/uint/bool)
  - Preallocated FIX resend ring buffer (4096 msgs)
  - CI hardening verification script

- **v1.2.0** - Phase 1 hardening
  - Migrated hot paths from system_clock to steady_clock
  - Lock-free throttle and latency tracking
  - Eliminated m.get() from FIX hot paths

- **v1.0** - Initial release (renamed from OMEGA v6.4)
  - Unified VenueHealth model
  - Protocol-agnostic Arbiter
  - Cleaned codebase (removed legacy stubs)
  - 290 source files (from 463)

## License

Proprietary - All rights reserved
