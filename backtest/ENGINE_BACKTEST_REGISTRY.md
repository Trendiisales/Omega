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

## 5. BE-floor companions — MODEL-FILL TRAP (S-2026-07-07, load-bearing)

The BE-floor research scripts (`index_befloor_ls.py`, `gold_befloor_ls.py`,
`fx_befloor_ls.py`) book every clip `max(0, floor_fill)` with 1.2-1.5bp cost.
"neg=0 by construction" is a CODE CLAMP, not an execution property: live exits
evaluate at H1 CLOSE and the honest fill is worse-of(floor, close) — 90% of DJ30
exits pierced the floor. Real-fill re-validation on certified tick data
(`backtest/index_befloor_intrabar_bt.cpp` — the real-fill reference harness:
H1-close detector parity, worse-of fills, per-symbol rt_bp, optional intrabar
resting stop) showed the live 0.30%/be6 config STRUCTURALLY NEGATIVE everywhere:
US500 3.4yr -$482k real vs +$2.34M model; DJ30 7mo -$100k vs +$341k.
The 2026-07-06 `US500Pos_* -$273 x5` ledger rows were this mechanism.

RULES:
1. NEVER accept a BE-floor/companion viability verdict from the *_ls.py scripts
   alone — they cannot see a loss. Re-run the real-fill harness on tick data.
2. A resting stop AT the floor (0 buffer) self-triggers: entry is a close MID,
   bid sits half-spread below → instant -cost exit every arm (verified: wins
   collapse to ~0). Any intrabar floor needs a buffer (25bp validated on US500).
3. Surviving config (both WF halves +, both flavors +, all tiers +):
   US500 thr=1.5% be=10 cap=25bp only. NAS100/DJ30/GER40 retired 2026-07-07.
   Evidence: outputs/INDEX_BEFLOOR_REALFILL_2026-07-07.txt.
4. FAMILY ROLLOUT COMPLETE (S-2026-07-07e) — the audit debt is PAID and the family
   is DEAD everywhere except US500. Evidence outputs/BEFLOOR_FAMILY_REALFILL_2026-07-07.txt:
   - GOLD (AUPOS/AUNEG): RETIRED. No config in thr 0.3-3.0% x be 6/10/20 x exec
     A/buf10/buf25 x {ungated, EMA200/50-P100 sustained-bull-gated} positive in BOTH
     eras (2022-23 histdata ticks / 2024-26 m5-synth) with halves+flavors +. Each
     era's winner loses the other era. Bull-gate tested per operator rule — rescues
     2022-23 (+$85k @1.0/6/buf25+BG) but fails 2024-26 (-$61.5k, H1 -$79k).
   - XAG: RETIRED. Every cell negative INCLUDING the 2025-26 silver squeeze; best
     1.5/6/A -$486k real vs +$5.8M model. (Gate note: XAGUSD_h1_clean gate FAIL is a
     false positive — 3x-median heuristic vs real 5x squeeze; 0 jumps >10%/h.)
   - USOIL: RETIRED. 2026-only grid sea of red; lone + cell (0.70/20/buf25 +$41k)
     REFUTED on 16mo certified Brent BCOUSD real ticks (same cell -$138k, all cells
     negative). Single positive cells on thin data = grid mining; always cross-check.
   - STOCK DayMover (39 BIGCAP): RETIRED. Real -$110.7k vs +$1.57M model (7yr daily);
     Neg flavor -$325k at every thr; Pos-only +$214k but 2019-2022 half (incl the
     2020-21 bull) negative at every thr 3/4/5% — daily closes + overnight gaps.
   - FX: RETIRED S-2026-07-07d (see rule 3 commit); JumpRider is a DIFFERENT honest
     book (single real column, locked ea4a746f) — not part of this family.
5. m5/H1-OHLC SYNTH-TICK TRICK (validated S-2026-07-07e): the harness's resting-stop
   levels update ONLY at H1 close, so within the forming hour only breach-or-not
   matters. Feeding o/h/l/c of finer bars as 4 synthetic ticks (c LAST) reproduces
   every intrabar touch exactly at that bar's resolution and the H1 closes exactly —
   usable when raw ticks don't exist (gold 2024-26 m5, XAG/USOIL H1).
