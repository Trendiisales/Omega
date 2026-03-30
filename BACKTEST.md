# Omega C++ Backtester

Native C++ replacement for the Python `backtest.py`.

| | Python | C++ Backtester |
|---|---|---|
| Speed | ~21 K t/s | **500 K – 2 M t/s** |
| 120M tick run | ~5,700 s (95 min) | **60 – 240 s** |
| Engines tested | GoldStack only | All 5 engine families |
| Clock accuracy | Wall clock | Simulated (CSV timestamps) |

---

## Prerequisites

Same build environment as the main Omega binary. No extra dependencies — the backtest target has **no OpenSSL or WebSocket requirement**.

---

## Build

```powershell
# From the repo root — builds only the backtest binary
cmake --build build --target OmegaBacktest --config Release
```

The binary lands at `build/Release/OmegaBacktest.exe` (MSVC) or `build/OmegaBacktest` (GCC/Clang).

---

## Run

```powershell
# Basic — all engines, defaults
.\build\Release\OmegaBacktest.exe xauusd_merged_24months.csv

# Specific engine only
.\build\Release\OmegaBacktest.exe xauusd_merged_24months.csv --engine gold

# All options
.\build\Release\OmegaBacktest.exe xauusd_merged_24months.csv `
    --latency 1.0 `
    --warmup  5000 `
    --trades  bt_trades.csv `
    --report  bt_report.csv `
    --engine  gold,flow,latency,cross,breakout
```

### Options

| Flag | Default | Description |
|---|---|---|
| `--latency <ms>` | `1.0` | Simulated execution latency |
| `--warmup <n>` | `5000` | Ticks before recording trades (engine warm-up) |
| `--trades <file>` | `bt_trades.csv` | All trade records (shadow_analysis.py compatible) |
| `--report <file>` | `bt_report.csv` | Per-engine aggregate stats |
| `--engine <list>` | all | Comma-separated: `gold`, `flow`, `latency`, `cross`, `breakout` |

---

## Tick CSV Formats

The parser auto-detects the format from the first data row:

| Format | Columns | Source |
|---|---|---|
| A | `timestamp_ms, bid, ask` | Most brokers / custom export |
| B | `timestamp_ms, bid, ask, vol` | MT4/MT5 tick export |
| C | `YYYY.MM.DD, HH:MM:SS.mmm, bid, ask, vol` | Dukascopy |
| D | `timestamp_ms, open, high, low, close, vol` | OHLCV (uses `close ± 0.15` as spread) |

---

## Engines Tested

| Runner | Engines | Notes |
|---|---|---|
| `gold` | GoldEngineStack — all 23 sub-engines | CompressionBreakout, WickRejection, DonchianBreakout, TurtleTick, etc. |
| `flow` | GoldFlowEngine | L2 order-flow engine; uses neutral `l2_imb=0.5` (no L2 in CSV) |
| `latency` | LatencyEdgeStack | GoldSilverLeadLag, SpreadDislocation, EventCompression (gold leg only) |
| `cross` | OpeningRange, VWAPReversion, TrendPullback, NoiseBandMomentum | Single-symbol compatible CrossAsset engines |
| `breakout` | BreakoutEngine + GoldBracketEngine | CRTP compression breakout + bracket both-sides |

---

## Output

### Console (live)
```
  [ 42.3%]  50500000 ticks |   87s |  580 K t/s |  68241 trades | ETA  119s
```

### bt_report.csv
```
engine,trades,win_rate_pct,gross_pnl,avg_pnl,max_dd,avg_hold_sec,sharpe
TurtleTick,12847,61.3%,+18420.50,1.43,312.00,184,7.60
DonchianBreakout,3383,29.6%,+3125.00,0.92,124.00,421,2.34
...
```

### bt_trades.csv
Compatible with `scripts/shadow_analysis.py`:
```powershell
python scripts/shadow_analysis.py bt_trades.csv
python scripts/shadow_analysis.py bt_trades.csv --csv   # write shadow_report.csv
```

---

## How the Clock Shim Works

The key correctness problem with any fast C++ backtest against these engines:

- `GoldEngineStack` uses `std::time(nullptr)` for 90s entry-gap gates and 120s SL cooldowns
- All sub-engines use `std::chrono::steady_clock::now()` for 1s anti-spam gates
- `CrossAssetEngines` uses `system_clock::now()` for session detection (hour-of-day)

At 1M t/s, 120M ticks complete in ~120 seconds of wall time.  
A 90s cooldown would correctly expire — but a **1-second** anti-spam would only allow **~120 fires per engine** across 2 years of data, instead of the expected ~10,000+.

`backtest/OmegaTimeShim.hpp` solves this by:

1. Redefining `std::chrono::steady_clock` and `system_clock` in the `std::chrono` namespace **before** any engine header is compiled — injected via `/FI` (MSVC) or `-include` (GCC/Clang) in `CMakeLists.txt`.
2. Both clocks' `now()` return a `time_point` derived from `omega::bt::g_sim_now_ms`, which the tick loop advances from the CSV timestamp before each `on_tick()` call.
3. `time()` (C function) is overridden in the same translation unit to return `g_sim_now_ms / 1000`.

Result: every cooldown, hold-time gate, and session filter advances with simulated time, producing correct trade counts.

---

## After Running — Validate Spot Price Fix

The backtest data is `xauusd_merged_24months.csv` — XAUUSD spot prices, which now correctly map to `GOLD.F` engines after the ID 41/2660 routing fix (commit `08c0cf6`).

Shadow P&L history before that fix is **unreliable** (calculated on futures price). Use this backtest as the authoritative baseline going forward.

```powershell
# Full breakdown with per-engine equity curve
python scripts/shadow_analysis.py bt_trades.csv
```

---

## Troubleshooting

**Build error: `steady_clock` redefinition**  
Ensure `/FI` (MSVC) or `-include` (GCC) is in the `OmegaBacktest` compile flags in `CMakeLists.txt` — the shim must be force-included before `<chrono>` is pulled in by any other header.

**Zero trades from an engine**  
Increase `--warmup` — some engines (TurtleTick N=40, DonchianBreakout) need 40+ bars before the first signal. 5000 ticks is sufficient for all current engines.

**Results differ from live shadow**  
Expected — the live system feeds real L2 imbalance to `GoldFlowEngine` and `LatencyEdgeEngines`. The backtest uses neutral `l2_imb=0.5`. Flow and latency engine results will be lower quality than live. All GoldStack sub-engines are unaffected (no L2 input).
