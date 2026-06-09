# Omega — momentum / breakout scanner + backtester (C++17)

Find stocks that are **trending hard or exploding**, confirm with **institutional flow**,
size with **ATR risk**, and **exit on the first sign of reversal/distribution** to keep
max profit with minimum drawdown.

Pure, deterministic C++17. The core has **no external dependencies** — identical input
produces identical output on every run and machine (this is the fix for the variance you
saw before). Interactive Brokers is an **optional** data source bolted on behind a compile
flag; everything else runs from CSV.

---

## The method (long-only)

**ENTER** only when *every* gate agrees (slow to enter):

| Gate        | Rule                                                              |
|-------------|-------------------------------------------------------------------|
| Trend stack | `close > EMA10 > EMA20 > EMA50`, and EMA50 rising                  |
| Strength    | `ADX > 25` (real trend, not noise)                                |
| Ignition    | `RVOL > 2`  **or**  breakout above the prior-20-bar high          |
| Flow        | `CMF > 0` — net accumulation ("money in", the dark-money read)    |
| Regime      | benchmark (SPY) above its rising 200-EMA — turns longs off in bear markets |

**EXIT** on the *first* failure (fast to leave):

| Exit              | Rule                                                        |
|-------------------|-------------------------------------------------------------|
| Trailing stop     | Chandelier: `highest_high(22) − 3·ATR`, ratchets up only    |
| Hard stop         | `entry − 2·ATR` (gap-through fills at the open)             |
| Trend break       | `close < EMA10`                                             |
| Momentum fade     | MACD crosses below its signal line                         |
| Distribution      | `CMF < 0` — money leaving                                   |

**Sizing / risk:** each trade risks a fixed fraction of equity (default 1%);
shares = `risk$ ÷ (entry − initial stop)`, so volatile names get smaller size automatically.

---

## Layout

```
omega/
  include/omega/   Bar, CsvLoader, Indicators, Strategy, Backtester, Scanner, IbkrClient
  src/             implementations + 3 CLI mains
  data/            deterministic synthetic fixtures (TREND, CHOP, SPY) + gen.awk
  CMakeLists.txt   builds the library + tools (IBKR optional)
```

Indicators included: SMA, EMA, ROC, RSI, ATR, ADX (+DI/−DI), OBV, CMF, RVOL,
MACD, Chandelier exit, rolling prior-high/low. All Wilder-correct.

---

## Build (CSV-only — no IBKR needed)

With CMake:
```bash
cd omega
cmake -B build -S .
cmake --build build -j
```

Or straight g++ (what the demo used):
```bash
cd omega
g++ -std=c++17 -O2 -I include \
    src/CsvLoader.cpp src/Indicators.cpp src/Strategy.cpp \
    src/Backtester.cpp src/Scanner.cpp src/IbkrClient.cpp \
    src/main_backtest.cpp -o build/omega_backtest
g++ -std=c++17 -O2 -I include \
    src/CsvLoader.cpp src/Indicators.cpp src/Strategy.cpp \
    src/Backtester.cpp src/Scanner.cpp src/IbkrClient.cpp \
    src/main_scan.cpp -o build/omega_scan
```

## Run

```bash
# Backtest one symbol, using SPY for the regime filter, dump the equity curve:
./build/omega_backtest data/TREND.csv data/SPY.csv --equity-out build/equity.csv

# Scan a folder of CSVs for entry candidates (SPY.csv pulled out as benchmark):
./build/omega_scan data --benchmark SPY --top 50
```

Backtest flags: `--risk 0.01`, `--equity 100000`, `--no-regime`, `--equity-out FILE`.
Scan flags: `--benchmark SYM`, `--top N`, `--no-regime`, `--rvol 2.0`.

### CSV format
`date,open,high,low,close,volume` (header required, oldest row first, extra columns ignored).

---

## IBKR build (live data)

The IBKR adapter (`IbkrClient`, `omega_ibkr_fetch`) pulls history straight from TWS or
IB Gateway. Install the **TWS API C++ source** from IBKR, then:

```bash
cmake -B build -S . -DOMEGA_WITH_IBKR=ON -DTWSAPI_DIR=/path/to/twsapi
cmake --build build -j

# Start TWS/IB Gateway with the API enabled (paper port 7497), then:
./build/omega_ibkr_fetch data AAPL NVDA TSLA --port 7497 --duration "2 Y" --bar "1 day"
./build/omega_scan data --benchmark SPY
```

The TWS API `error()` callback signature differs across SDK releases; `IbkrClient.cpp`
targets the 10.x signature and notes where to adjust for older versions. The rest of the
codebase never touches IBKR.

---

## Important

This is a research/engineering framework, **not financial advice and not a guarantee of
profit**. The bundled `data/*.csv` are *synthetic* fixtures that prove the plumbing — the
metrics they produce are meaningless as a track record. Before risking capital: feed real
IBKR history, run **walk-forward / out-of-sample** tests with realistic slippage and
commission (both are already modelled in `BacktestConfig`), then paper-trade.
