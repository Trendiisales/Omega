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

## 6. BIGCAP upjump LADDER companion (S-2026-07-07w — the no-floor successor to §5's stock retiree)

`include/StockDayMoverLadderCompanion.hpp` (LONG-only, wired SHADOW in engine_init).
Research: `backtest/bigcap_upjump_ladder_bt.py` over `data/rdagent/sp500_long_close.csv`
(2019-06..2026-06 daily closes). Survivor TIGHT a0.5/s2/g0 + WIDE a8/s0/g50 + ladder
cap5 reclip5% + LOSS_CUT 15, RT 8bp: n=4,981 net +7,044% of clip notional PF 1.58,
all-6 + 2x-cost + ex-semis + full-565 controls PASS. Evidence
`outputs/BIGCAP_UPJUMP_LADDER_2026-07-07.md`, vault `BigCapUpJumpLadder`.

Faithful-port PARITY TEST (do this for every research->C++ port): drive the engine
header standalone over the same wide CSV with no-op exec fns and deploy_ts=0 (books
everything) and compare net/clips/per-name vs the python harness. This wire:
C++ +6,994%/5,201 clips vs py +7,044%/4,981 (LOSS_CUT variant +7,057) — deviations
are the engine's own guards (below) + end-of-data flush. Same per-name ranking.

Traps:
- Backtests at DAILY-CLOSE grade only — do NOT re-test it on ticks/H1 you don't have;
  the live engine trails at daily closes by design (in-calibration).
- The >50%-jump reject guard is SELF-HEALING (3 consecutive rejects = split/vendor
  seam -> accept level, VOID open window unbooked). The parity test caught the sticky
  version bricking CRWD forever at its 492->124 seam. Never book clips across a seam.
- ExecutionCostGuard has no single-name equity rows; the validated cost gate is the
  8bp RT debit inside the engine (2x=16bp still PASS). Real cost row owed before LIVE.
- Feed = RDAgent wide CSV (refresh_close_ibkr.py, IBKR 4002); stale since 2026-06-29
  (IBKR sub lapse). Engine seeds+arms; books resume when the feed does.

## 7. MGC venue port of the XauTrendFollow4h/2h family (S-2026-07-07w)

Production spot config re-instanced on MGC futures (10oz micro, $10/pt, tick 0.10,
comm ~$1.04/side -> ~0.31pt RT vs spot ~1.4pt at 4000). Two-layer validation:

1. **Faithful harness**: `backtest/XauTrendFollow4h2hBacktest.cpp` gained `MGC=1`
   (fixed $0.208/oz RT comm; pair with `SPREAD=0.10`). Real MGC H4/H1 bars from
   `backtest/mgc_30m_to_h1_h4.py` over `/Users/jo/Tick/mgc_30m_hist.csv`
   (2024-06..2026-06, integrity-gated). Prod cfg (MASK=0xC9 IMP=0.5 ADX=15 VB=1):
   4h PF1.54 +$4404 DD$1331 both-halves+; 2h PF1.21 +$4281 both-halves+;
   2x-cost PASS both. 2022-23 bear at MGC cost: 4h +$650 PF1.22 (H1-half -$162,
   halves FAIL -- structural bear-flat, same as spot), 2h flat.
2. **Feed-path parity** (registry §6 mandate): `backtest/mgc_tf_feed_parity.cpp`
   drives the PRODUCTION MgcFastDonchianFeed poll path (30m rows -> on_tick l/h/c
   + H1/H4 bucket aggregation) over the same 30m file. LC4=1.5/LC2=0:
   4h n291 PF1.50 +$4209 DD$1064 (harness n285 PF1.54) / 2h n596 PF1.23 +$3533
   DD$2390 (harness n801 PF1.21) -- granularity-level deviation, PASS.

Traps:
- **Spot LOSS_CUT 0.5% KILLS the MGC 2h** (parity: PF0.86 -$2095 vs PF1.23 +$3533
  at LC=0): 0.5% of ~4400 = ~22pt inside the 2x-ATR SL; the 30m intrabar path
  trips it constantly. MGC 2h wired LC=0 (engine 2xATR SL = the protection).
  4h keeps LC=1.5 (net -8% for DD -20%, consistent with the spot verdict).
- MGC history has NO 2022 bear -- the bear axis runs on SPOT bars + MGC costs.
- CONTFUT reqHistoricalData: end-date paging NOT allowed (err 10339) and
  useRTH=1 starves bars -- pull with useRTH=0, single request, no end date.
- Warmup: `data/mgc_h1_hist.csv` (H1, ts-sec) + `data/mgc_h4_hist.csv` (H4, ms) --
  regenerate via the scratch IBKR pull before deploy so the live-CSV replay floor
  (g_mgc_tf_floor_ts) covers the gap. Ledger tags MgcTF4h_/MgcTF2h_<cell>.

## 8. FX upjump LADDER companion (S-2026-07-07x resume — the FX member of §6's family)

`include/FxUpJumpLadderCompanion.hpp` (LONG-only, wired SHADOW in engine_init; feed =
tick_fx.hpp H1 roll h/l/c). Research: `backtest/omega_upjump_ladder_bt.py` +
expanded entry/exit sweep `backtest/fx_upjump_ladder_sweep2.py`
(`outputs/FX_UPJUMP_SWEEP2_2026-07-07.txt`). Cells (Tick H1 multiyear, WF halves +
2x-cost + 5-seed random-window control PASS):
EURUSD W48/0.5 +39.7% PF1.47 n507 · GBPUSD W48/1.0 +37.4% PF2.20 n240 (random ZERO)
· NZDUSD W24/1.5 +41.2% PF4.35 n100 · AUDUSD W96/1.0 +30.9% PF1.51 n220 (PASS-thin).
Exits insensitive on plateau (g35-65, LC 3/5/8/off, Ttrail, reclip, cap all within
noise) -> mechanism locked at research ratios; LOSS_CUT 5thr kept as free insurance.

PARITY (per §6 mandate): `backtest/fx_upjump_parity.cpp` drives the header standalone,
no-op exec fns, deploy_ts=0 — EXACT: booked+open_mtm == python net to 0.1% on all 3
survivors; clip-count delta == end-of-data open legs only.

Traps:
- Manage is INTRABAR l->h->c inside the H1 bar (SL-first). Do NOT re-test close-only —
  trail/LC exits book AT the stop level (resting-stop convention, in-calibration).
- Detector needs W bars of LOWS: warmup CSVs are ts,o,h,l,c; the engine's own forward
  dump is ts,h,l,c (4-col). Close-only seeds (2-col) degrade the detector — acceptable
  for continuity only.
- Live gap guard (deviation, documented in header): NEW windows blocked when the W-bar
  span exceeds W hours + 4 days (multi-day outage), exits honoured. Harness data had
  no such gaps.
- USDJPY/USDCAD: DEAD (9/9 negative cells) — do not resurrect without new basis.
  XAU = bull-beta (random captures it); GER40 = bull-only (index axis wire).

## 9. INDEX upjump LADDER companion (S-2026-07-07x resume — the index member of §8's family)

Same class (`FxLadderPair` via `omega::index_upjump_ladder_book()`, prefix
`idxladder_companion_`), feed = tick_indices.hpp IdxH1Agg (extended to track h/l).
Research: `backtest/index_upjump_ladder_sweep.py` over H1 built fresh from the tick
corpus by `backtest/histdata_tick_to_h1.cpp` (evidence `outputs/INDEX_UPJUMP_LADDER_
2026-07-07.txt`). Wired cells (WF halves + 2x-cost + gap-masked 5-seed random control):
US500 W24/2.0 +123.2% PF1.34 n854 (random -24) · NAS100 W24/1.5 +242.9% PF1.23 n2129
(random -5; most lucrative index) · GER40 W12/1.5 +72.4% PF3.51 bull file, bear 24/24
negative -> BULL-GATED behind `omega::index_risk_off()`.

NAS refill (2026-07-08): the 7 missing months (2022-04, 2024-04/07/08/09/11/12) were
fetched straight from histdata.com (`get.php` POST with the per-page `tk` token — the
old dirs held only the status-report .txt; the site has the csv), H1 rebuilt from all
52 months (301M ticks, `histdata_tick_to_h1.cpp`) → `NSXUSD_2022_2026.h1.csv` 24,407
bars **CERTIFIED CLEAN** (gap-masked predecessor backed up as `.gapmasked_20260708`).
Wired cell RE-VERIFIED on full data: W24/1.5 n2442 +245.7% PF1.20 WF+ 2x-cost +172.4 —
holds; random control rose -5 → +59.6 (full-data bull-beta), so quote the edge as
**~+186% over random**, not raw net. US500 W24/2.0 unchanged (same data) = harness
sanity pass. Evidence `outputs/INDEX_UPJUMP_LADDER_NASREFILL_2026-07-08.txt`.

Traps:
- Most of the NAS100 grid is BULL-BETA (random control +40..+100 on full data): only
  the W24 thr1.5-3.0 pocket meaningfully beats random. Do not promote other NAS cells
  off net% alone; always quote net-over-random.
- The old merged `NSXUSD_2022_2026.csv` tick file predates the newer monthly
  downloads AND /Tick has duplicate months across two roots (incl. a "(2)" dir with a
  space) — rebuild H1 via the python month-dedupe in this session's log, never a bare
  `$var` glob (zsh no-word-split).
- GER40: never un-gate — bear file universally negative at every cell.

## 10. BE-CASCADE ports — indices D1 long + gold H1 OCO bracket (S-2026-07-12b/c)

Crypto up-jump BE-cascade mimic (UpJumpLadderCompanion mechanism) ported to Omega
instruments. Engines: `include/BeCascadeEngines.hpp` — `XsBeCascade_{USTEC.F,US500.F,DJ30.F}`
(D1 long-only, W=10d, thr 2/3/4%) + `XauBracketCascade` (gold H1, two-sided OCO bracket
2%/±0.3%, W=240). All SHADOW. Findings: `backtest/XS_BECASCADE_GOLD_INDEX_FINDINGS.md`.

- **Index harness** = `Crypto/backtest/upjump_earlyarm_bt.cpp` modes `xsgrid`/`xsrandom`
  (drives the REAL crypto header; data `Crypto/backtest/data/{XAU,SPX,DJ30,NDX}USDT_1d.csv`).
  Run: `cd /Users/jo/Crypto/backtest && UJW_TF=1d ./upjump_earlyarm_bt xsgrid`.
  Random-entry beta control is MANDATORY here — gold long-only passed every gate on pure
  bull beta (z=1.1); indices are real (z 2.3–3.2).
- **Gold bracket harness** = `backtest/xau_bracket_becascade_bt.cpp` (H1 OHLC, self-contained,
  plain g++). Bear files: XAU2013/2015/2022_bear_h1.csv. Long-only control built in.
  TRAPS: (a) thr=3% kills 2015/2022 — 2% is the anchor; (b) W=120 collapses in chop;
  (c) both-side-touch H1 bars are AMBIGUOUS — cancel + count, never coin-flip;
  (d) costs are per-LEG RT (XAU 5bp, idx 3bp) — crypto's 20bp strawman-kills.
- **Live fidelity notes**: signals/cascade on FINALIZED bar closes, executions at live mid
  (next-open convention); gold bracket fills are tick-level. Warm-seed REBASES to first live
  mid (cash-index seed vs .F feed; MGC seed vs spot) — without it the venue offset fakes a
  jump through the W-window. Gold seed lags (MGC to 2026-06-03): honest after 240 live H1.

## HARD DATA GATE — no green on rejected data (added 2026-07-13, operator-mandated)

**A REJECTED integrity verdict is TERMINAL. There is no override.** On 2026-07-13 a silver
up-jump backtest reported +1645% "CERTIFIED z=5" on data the integrity gate REJECTED (111
x1000-glitch ticks) — a subagent saw "REJECTED", called it a "false-positive squeeze", and
ran anyway. The verdict was void; the operator called it shit data. Root cause: the gate was
correct (exit 1) but nothing STOPPED a rejected file from being backtested.

RULE (non-negotiable):
1. NEVER `cp` raw Tick/histdata files directly into a harness data dir (`Crypto/backtest/data/`
   or any `<SYM>USDT_<tf>.csv`). Stage them ONLY via:
   `bash backtest/stage_certified_data.sh <src.csv> <dest.csv>`
   It runs `data_integrity_gate.py` and copies ONLY on CERTIFIED CLEAN; a REJECT refuses the
   copy (exit 2) and writes NO file. No `--force`, no override, no "false-positive precedent".
2. If you believe a reject is a false positive, FIX THE DATA (de-glitch the x1000 ticks,
   re-pull a clean month) until it passes the gate — do NOT bypass it.
3. A staged file carries a `<dest>.certified` stamp (source + sha). No stamp = not certified =
   its backtest result is VOID. Never present a green whose data lacks the stamp.
4. GATE HEURISTIC IS LOCALITY-AWARE (fixed 2026-07-13, same session as this rule): the
   original ">3x global median = glitch" check false-rejected any legit multi-year trend
   (XAGUSD 22->119 over 2022-26 tripped it; the file was clean — 0 row-to-row jumps >15%,
   matches an independent IBKR L2 capture at 76.11 on 2026-05-26, gold/silver ratio 56-82
   throughout). Now: >3x jump vs PREVIOUS row = FAIL, >50x off global median = FAIL
   (x1000 block), >3x off global median alone = WARN. Synthetic x1000 corruption (scattered
   + block) verified still REJECTED. This is a GATE fix, not an override — rule 1-3 stand.

## 11. SILVER (XAGUSD) XauTrendFollow chassis + mimic ladder — DEAD, no wire (S-2026-07-13)

Certified data: `backtest/data/XAGUSD_2022_2026.{h1,h4}.csv` (+.certified stamps), staged
after the locality-aware gate fix above. The 2025-26 silver rally (36->94 peak 119->62) is
REAL — verified 3 independent ways (bar continuity, second build <0.3% diff, IBKR L2 capture).
The old +1645% silver "z=5" up-jump result stays VOID regardless (up-jump family banned
everywhere, S-2026-07-13m).

Harness: `backtest/clip_path_xag_tf.cpp` (real XauTf 4h/D1/2h engine classes over XAG bars —
chassis is ATR/EMA-relative so scale-free; half-spread 0.0125 USD, cost_rt=3bp+spread/px)
-> `mimic_ladder_overlay.py`. Verdict:
- PARENT streams gross-positive (4h +163% gross/+86% net, D1 +123% net) but ALL fail all-6:
  every dollar of edge is the 2025-26 parabola; the 2022-24 half is decisively negative
  (4h WF-H1 -76% on n~326, D1 bear-slice -263%). One-regime wonder.
- MIMIC legs (shipped GoldTrendMimicLadder mechanism incl BE-ENTRY pend): shipped gold params
  FAIL hard (-68..-146%/leg); full 864-config sweep (arm x lc x cap x gb x be_entry, 4h+D1)
  = ZERO all-6 passes. A mimic cannot clip a one-regime parent into an edge.
- 2h overtrades (1667 trades, ~12bp avg cost = 198% cumulative drag): net negative gross-of-edge.

Kill is mechanism-faithful (real engine classes, certified data, shipped mimic mechanism,
shipped params inside the sweep). Consistent with the §5 BeFloor XAG RETIRED verdict. Do not
re-open without a NEW basis (different chassis family or a genuine silver-native edge, judged
on BOTH the 2022-24 range AND a post-rally regime).
