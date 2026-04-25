# US30 H4 Warm-Start Seeder

`seed_us30_h4.cpp` -- bootstrap the `MinimalH4US30Breakout` engine's warm-restart
state from a Dukascopy USA30 H4 OHLC CSV.

## Why

The `MinimalH4US30Breakout` engine (added S25, `cf0dc891`) is **self-contained**:
it builds its own H4 OHLC bars and Wilder ATR14 from the live tick stream
because no `g_bars_us30` external bar feed exists (BlackBull rejects index
trendbar subscriptions).

On a cold start, the engine needs:
- 10 closed H4 bars before the Donchian channel is valid (40 hrs)
- 14 closed H4 bars before Wilder ATR14 is seeded (56 hrs)

That's ~56 hrs of waiting before the first signal can fire. Unacceptable.

This seeder replays a small Dukascopy USA30 H4 CSV through **the exact same
Wilder/Donchian math the engine uses** and writes a fully-primed
`bars_us30_h4.dat`. The engine's `load_state()` accepts it on startup as if
Omega had been running for days. **First H4 close after the seed is hot.**

## Build

```bash
cd /path/to/Omega
g++ -std=c++17 -O2 -I include backtest/seed_us30_h4.cpp -o seed_us30_h4
```

(Or whatever build setup you prefer -- it's a single self-contained .cpp.)

## Get Dukascopy data

### Option A: dukascopy-node CLI (recommended -- easy from a terminal)

Requires Node.js 18+. Run on Mac, then SCP the CSV to VPS, *or* install Node
on the VPS and run there directly.

```bash
# Last 5 days of USA30 H4 bid-price bars in CSV
# (Verified flags as of dukascopy-cli 2025: -p only accepts bid|ask, not mid;
#  output dir flag is -dir not -o.)
npx dukascopy-node -i usa30idxusd -from 2026-04-19 -to 2026-04-25 \
                    -t h4 -p bid -f csv -dir ./
```

Outputs `usa30idxusd-h4-bid-...csv` in the current dir. (Or `./download/`
subdir if you omit `-dir ./`.)

**Note on bid vs mid:** the engine's H4 bars are built from mid-price ticks,
but for Donchian breakouts the difference is irrelevant. DJ30.F spreads are
tiny relative to H4 ranges (~3pt vs ~150pt ATR), and the channel/ATR math
is built from differences so any consistent offset cancels out. If you want
to be paranoid, run the seeder once with bid and once with ask and confirm
ATR/channel values agree to within 0.5%.

### Option B: Dukascopy web tool

https://www.dukascopy.com/swiss/english/marketwatch/historical/

Pick `USA30.IDX/USD`, period `H4`, format `CSV`, your date range, download.

## Run the seeder

```bash
./seed_us30_h4 path/to/usa30idxusd-h4-mid-bid.csv \
               C:/Omega/logs/bars_us30_h4.dat
```

(On Windows VPS: replace the output path with wherever `log_root_dir()` resolves
on your install -- normally `C:\Omega\logs\`.)

The seeder prints the channel high/low, ATR value, and seeded bar count. If
ATR is not fully seeded (you didn't supply enough bars), it warns -- supply
at least 14 bars (3 days) for full ATR seed.

## Deploy

1. **Stop Omega** (`sc.exe stop Omega`).
2. **Copy the .dat** to `C:\Omega\logs\bars_us30_h4.dat`.
3. **Start Omega**. On startup look for:
   ```
   [MINIMAL_H4-DJ30.F] load_state OK: age=Xs donchian_bars=10/10 atr=Y.YY ...
   [STARTUP] MinimalH4US30Breakout warm-loaded from .../bars_us30_h4.dat -- engine hot, can fire on first H4 close.
   ```

If you see instead:
```
[STARTUP] MinimalH4US30Breakout cold start -- needs ~40hrs of live DJ30.F H4 bars before first signal.
```
the .dat was missing, malformed, or older than 8 hours. Re-run the seeder
with fresher data (the saved_ts in the .dat is set to `now` so as long as
you copy and start within 8 hours of running the seeder you're fine).

## Price-feed caveat

Dukascopy USA30 and BlackBull DJ30.F both reference the Dow Jones Industrial
Average cash index. Numerical prints can drift +/-2-5 points between providers
but H4 OHLC structure (highs, lows, ATR magnitude) tracks within ~1%. For
Donchian breakouts this is fine -- the strategy fires on
bar-close-vs-prior-channel, and ATR is computed from differences only.

After load, each new live H4 close from BlackBull replaces the oldest seeded
Dukascopy bar in the deque. After `donchian_bars` (10) live closes (= 40 hrs),
the engine is operating purely on broker-feed prices. **And ATR is hot from
the very first live bar**, so signals can fire any time after the first H4
boundary crossing (i.e. within 4 hours of restart, not 56 hours).

## Format

The seeder writes the same key=value text format the engine itself writes
in `MinimalH4US30Breakout::save_state()`. If the format ever changes, bump
`version=` in both files at once.

Once Omega is running, the engine takes over: every 10 minutes the periodic
save loop in `omega_main.hpp` overwrites this file with the live state, so
the seeder's bootstrap data is purely for the very first deploy / clean
shutdown without any prior runtime.
