# Engine Backtest Registry — READ BEFORE BACKTESTING ANY ENGINE

Authoritative reference for faithfully backtesting Omega engines. Created 2026-06-15
after repeated catastrophic re-derivation failures (testing disabled engines,
missing init calls, data glitches). **If you are about to backtest an engine, read
this first. If you discover a new engine quirk, ADD IT HERE.**

## 0. The two data sources (USE THE LEDGER FIRST)

1. **LIVE SHADOW LEDGER = the primary record of real engine performance.**
   `<log_root>/trades/omega_trade_closes.csv` (cumulative, all-time) + daily
   `omega_trade_closes_YYYY-MM-DD.csv` + `<log_root>/shadow/omega_shadow.csv`.
   On the VPS: `C:\Omega\logs\trades\...`. EVERY enabled engine (shadow or live)
   writes its closed trades here with the engine tag. **For "is this engine
   viable" — read this first.** It is the real forward record, no harness-fidelity
   risk. Pull it before building any backtest.
2. **Tick-replay backtest** (`backtest/ShadowBook_mi.cpp`) — historical sim. Only
   as faithful as its wiring (see traps below). Use for cross-regime / pre-deploy
   validation on `/Users/jo/Tick/` corpus.

## 1. MANDATORY pre-flight gates (every backtest, no exceptions)

1. **Enabled-status check.** A backtest must test ONLY what's `enabled=true` in
   `include/engine_init.hpp`. The harness runner list is INDEPENDENT of engine_init
   — it will happily test disabled/tombstoned engines. Cross-check every dispatched
   engine: `grep '<global>.enabled' include/engine_init.hpp | tail -1`. (2026-06-15:
   first run tested 6 tombstoned + the whole DJ30 graveyard → garbage.)
2. **Data-integrity gate.** Run `backtest/data_integrity_gate.py <file>` on EVERY
   tick file before use. Catches ×1000 price glitches (DJ30 had 25% corrupted →
   fake +$2.4M trade), column swaps, dead/negative spreads, gaps, dupes, price band.
   REJECTED file = do not use.

## 2. Recurring traps (each cost hours on 2026-06-15)

- **`init_default_cells()` / init methods.** Many engines DON'T build their cells in
  the constructor. SurvivorPortfolio needs `g_survivor.init_default_cells()`;
  XauTrendFollow/Tsmom/Donchian/EmaPullback portfolios need `.init()`; Us30Ensemble
  `.init()`; Ger40Keltner `.init()`. Skip it → ZERO trades (looks "dead" but is
  unwired). ALWAYS replicate the init call from engine_init.hpp.
- **Column order.** Duka `XAUUSD_..combined.csv` and `USA30IDXUSD_*` are
  `ts,ask,bid` (ASK FIRST). HISTDATA is `YYYYMMDD HHMMSSmmm,bid,ask`. Verify
  bid<ask after parse; the loader auto-detects via header but raw files can lie.
- **pnl units differ per engine family.** XAU/index engine TradeRecord.pnl =
  `points × lot` (RAW) → multiply by `tick_value_multiplier(symbol)` for USD
  (sizing.hpp: XAU=100, US500=50, USTEC=20, DJ30=5, NAS100=1, GER40=1.10€,
  UK100=1.33£, FX=100000). FX engines emit pnl ALREADY in USD (notional applied) —
  do NOT multiply. Verify per engine: ratio = pnl/price-move (≈lot → raw; ≈notional
  → already $). The live ledger applies the ×mult in handle_closed_trade — a harness
  using store::add directly does NOT, so apply it in post.
- **Cold-start.** Engines warm their bar buffers from the tick stream; D1/H4/100-bar
  Donchian need weeks. Either call the engine's `seed_from_*_csv()` (warmup CSVs in
  `phase1/signal_discovery/`) or drop the first N days of trades. Skipped seed +
  short window = artificially few trades.
- **Emit mechanism varies.** 4-arg `on_tick(bid,ask,ts,cb)` → pass the cb. 3-arg
  `on_tick(bid,ask,ts)` → set `.on_close_cb` or `.on_trade_record` member first.
  SurvivorPortfolio: `on_tick(sym,bid,ask,ts,cb)`, cb fires on close regardless of
  shadow_mode.
- **Cost.** Real IBKR cost is NOT the engine_init COST_RT_PTS for gold (0.37 was
  ~4× low). See `~/.claude/.../omega-ibkr-real-costs.md`: XAU = 1.5bps/side commission
  + measured spread. Index/FX measured spreads documented there.

## 3. Faithful-backtest recipe

1. Read engine's config block in `engine_init.hpp` — replicate params + init + seed.
2. Instantiate the engine header (Mac-clean under `-DOMEGA_BACKTEST`); do NOT
   include globals.hpp/on_tick.hpp (winsock, not Mac-buildable).
3. Dispatch exactly as `include/tick_gold.hpp` / `tick_indices.hpp` / `tick_fx.hpp`
   drive it live (bar engines need external bar feed; most build own bars from on_tick).
4. Set `shadow_mode=false` so the cb logging path fires (only deviation from live).
5. Gate data through `data_integrity_gate.py`. Apply real per-instrument cost in post.
6. Cross-regime: include 2022 bear (NAS2022, DEUIDXEUR_2022, multi-year duka).

## 4. Harnesses
- `backtest/ShadowBook_mi.cpp` — multi-instrument, live-enabled engines, real configs.
- `backtest/OmegaBacktest.cpp` — older gold-family (its runner list ≠ engine_init; stale).
- `backtest/data_integrity_gate.py` — the pre-flight gate.
- Aggregation: `/tmp/agg_full.py` (per-instrument mult + cost).

## 5. Crypto (BTC/ETH, long-only) — separate harness

The crypto book is Python (~/Crypto, not the C++ engine stack). Its backtest
lives in `backtest/crypto_bear_bounce/` — **read `FINDINGS.md` there before any
crypto-long backtest**. Key standing results (S-2026-07-03, Coinbase 1h
2017-2026, 3 full bears): (a) KNIFE-PHASE LAW — below the 200D SMA and not
above a rising 50D SMA, NO long-only entry family survives costs (7 families
tested; any new knife-long proposal must beat that study first); (b) the
deployable engine is **BearRecovery** (recovery sub-regime EMA9-reclaim,
BE-and-ride floor arm 2%, no giveback clips — they are proven harmful on
crypto); signals: `tools/crypto_bear_recovery.py`. Bull regime belongs to the
Luke system (`backtest/luke_system/MATRIX_FINDINGS.md`).
