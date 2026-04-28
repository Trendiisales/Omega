# Stage 1A — Trade Quality Audit (FINAL)

**Session:** 2026-04-28 NZST
**Scope:** Diagnose bad trades from 2026-04-27 + 2026-04-28 UTC.
**Method:** Source-level audit (VPS commit `ed95e27c` byte-identical to HEAD `33e4ffe3` for `GoldEngineStack.hpp`) + ledger CSV analysis on actual close data.
**Status:** FINAL findings. Recommendations are RANKED, not approved. No code changes proposed for execution this session.

---

## Headline

**73 trades over 2 days produced -$56.75 net.** 20 wins, 11 scratches, 42 losses.

The single dominant pathology across the system: **76% of stop-outs had favourable excursion before reversing** (67% had MFE > 3 points). Decent entries are being given back to fixed SLs. This is the same defence-gap pattern flagged in CHOSEN.md for the research stack — but it is also occurring in the live system.

A second distinct pathology is engine-level whipsawing on indices, where IndexFlow flipped its own direction on NAS100 13 times within 5-minute windows over the trading session. See Section 4a for the whipsaw fix recommendation shape (specification pending source read).

---

## 1. Live engine roster vs userMemory

userMemory states: HBG + MacroCrash (disabled) + RSIReversal + MicroMomentum (disabled) = 4 engines.

**Reality at VPS commit `ed95e27c`:** 18 engines live on Gold via GoldEngineStack.hpp:

| Engine | Constructor state | Comments |
|---|---|---|
| SessionMomentum | LIVE (default) | 07:15-10:30, 13:15-15:30 UTC windows |
| MomentumContinuation | LIVE (default) | Comment says "shadow first" but no shadow flag set |
| IntradaySeasonality | LIVE (default) | |
| DonchianBreakout | LIVE (default) | |
| NR3Breakout | LIVE (default) | |
| SpikeFade | LIVE (default) | |
| AsianRange | LIVE (default) | |
| DynamicRange | LIVE (default) | |
| NR3Tick | LIVE (default) | |
| TwoBarReversal | LIVE (default) | |
| MeanReversion | LIVE (default) | |
| **VWAPSnapback** | **LIVE (explicit)** | "Re-enabled: 1T sample too small for judgment" |
| LiquiditySweepPro | LIVE (default) | |
| **LiquiditySweepPressure** | **DISABLED (explicit)** | "DISABLED: 51T 29%WR -$12" |
| LondonFixMomentum | LIVE (default) | |
| VWAPStretchReversion | LIVE (default) | |
| OpeningRangeBreakoutNY | LIVE (default) | |
| DXYDivergence | LIVE (default) | "Shadow for 30 trades to validate" — no shadow flag set |
| SessionOpenMomentum | LIVE (default) | |

`EngineBase` default is `enabled_=true` (GoldEngineStack.hpp L263). Comments saying "shadow first" do not gate anything; they are documentation only.

Plus HBG bracket layer, plus from `engine_init.hpp`: RSIReversal (live), MacroCrash (disabled), TrendPullbackSP/NQ (re-enabled S14), MinimalH4 Gold/US30 (live), and several more. Ledger evidence shows IndexFlow, HybridBracketIndex, TrendPullback, DonchianBreakout firing — confirming the broader cross-asset roster.

**Implication for any future work:** assume ~18-20 engines firing concurrently, not 4. Treat memory's engine list as informational not authoritative.

---

## 2. Bleed ranked by net P&L impact

| # | Engine | n | wins | scr | loss | net | avg loss | comment |
|--:|---|--:|--:|--:|--:|--:|--:|---|
| 1 | HybridBracketIndex | 8 | 2 | 0 | 6 | **-$36.23** | -$24.69 | Largest avg loss in system. Sized 0.5-1.6 contracts. |
| 2 | SessionMomentum | 4 | 1 | 0 | 3 | -$13.65 | -$4.79 | Wins tiny, losses near-full SL |
| 3 | IndexFlow | 35 | 5 | 9 | 21 | -$13.18 | -$0.77 | Whipsaws on NAS100, but losses small |
| 4 | DonchianBreakout | 2 | 0 | 0 | 2 | -$8.04 | -$4.02 | Small sample |
| 5 | VWAP_SNAPBACK | 1 | 0 | 0 | 1 | -$5.28 | -$5.28 | One trade, no signal |
| 6 | HybridBracketGold | 3 | 1 | 1 | 1 | -$4.45 | -$7.04 | Two trail-locks too tight ($0.04, $2.55) |
| 7 | VWAPStretchReversion | 4 | 2 | 0 | 2 | -$3.22 | -$3.33 | |
| 8 | DynamicRange | 12 | 6 | 0 | 6 | **+$6.99** | -$3.42 | Best high-frequency producer |
| 9 | TrendPullback | 3 | 2 | 1 | 0 | **+$9.68** | n/a | NQ engine, working |
| 10 | LondonFixMomentum | 1 | 1 | 0 | 0 | **+$10.63** | n/a | Single TP_HIT |

**Tier 1 problem (immediate action candidate): HybridBracketIndex.** -$36 over 8 trades, -$24 avg loss. Sized 100x larger than HBG. The big TRAIL_HIT win of +$89 saved this engine from being far worse — but two such wins out of 8 trades is not a sustainable record.

**Tier 2 problem (volume-driven): IndexFlow.** Small individual losses but 35 trades = highest frequency engine in the system. Whipsaws on NAS100 with same engine flipping LONG/SHORT/LONG within 4-minute windows. 9 of 35 trades scratched at breakeven (working trail-to-BE) but 21 went to original SL. 5 wins net only +$0.62 average, vs 21 losses at -$0.77 average — slight negative edge but very high turnover means it can compound badly in any sustained chop.

**Tier 3 problem (entry quality): SessionMomentum.** 4 trades, 1 win at +$0.72, 3 losses near full SL. The IMPULSE/COMPRESSION classification is firing the engine in regimes where the move dies — needs entry-condition audit (IMPULSE_MIN=3.50, VWAP_DEV_MIN=1.50 per source L288-297).

---

## 3. The dominant pathology: MFE > 1pt SL_HITs

Across the entire dataset, of **51 SL_HIT trades**:
- **39 (76%)** had MFE > 1 point (favourable excursion before reverse)
- **34 (67%)** had MFE > 2 points
- **34 (67%)** had MFE > 3 points

Engine-by-engine, in PRICE POINT units (note: MFE/MAE units differ between engines — see Section 6):

| Engine | SLs | SL with MFE>1 | SL with MFE>3 |
|---|---|---|---|
| IndexFlow | 35 | 25 | 22 |
| HybridBracketIndex | 6 | 5 | 3 |
| TrendPullback | 3 | 3 | 3 |
| SessionMomentum | 2 | 2 | 2 |
| DynamicRange | 2 | 2 | 2 |

Concrete example (IndexFlow 14:07:17 NAS100 SHORT):
- Entry 27251.40, MFE 20.75 points (price moved 20.75 in favour)
- Exit at 27251.40 (sl was already moved to entry — breakeven trail fired)
- Net P&L: $0.00

So IndexFlow does have a breakeven trail. But not a runner trail. After locking BE, it doesn't trail any further toward TP; any retrace to entry exits flat. The engine cannot capture multi-point favourable moves even when they're 20 points in favour.

Other engines (TrendPullback, SessionMomentum, DonchianBreakout) appear to have **no trail at all** — MFE > 3 going to SL means the trade rode a 3+ point favourable excursion all the way back through entry to original stop.

**This is the live-system mirror of the CHOSEN.md defence gap.** The research-stack `sim_lib.py` has no trail/spike-exit/vol-adaptive SL. Several live engines have the same gap.

---

## 4. Concurrent opposite-direction fires

8 cases in the dataset where the same symbol had opposite-side entries within 120 seconds:

- **6 of 8 are IndexFlow flipping itself** — same engine taking LONG, then SHORT, then LONG on NAS100 within 1-3 min. The engine is whipsawing in chop.
- 1 case: IndexFlow LONG → HybridBracketIndex SHORT (different engines)
- 1 case: HBG SHORT → VWAP_SNAPBACK LONG (the original 04-28 example you flagged)

**There is no global cross-engine direction gate** preventing concurrent opposite-side positions on the same symbol. Source confirmation pending — `on_tick.hpp` (128 KB) not yet fully read this session. Provisional finding only.

The IndexFlow self-whipsaw is a separate problem from cross-engine fights — it's an internal mean-reversion behaviour where the engine's signal flips with every micro-pivot.

---

## 4a. Whipsaw fix — recommendation SHAPE (NOT YET SPECIFIED)

**Provenance disclosure:** I have not yet read `IndexFlowEngine.hpp` (58,961 bytes) or `IndexHybridBracketEngine.hpp` (25,639 bytes). The recommendation below is the SHAPE of a fix derived from ledger evidence only. Specification of actual code, parameters, and integration points requires the source read in Stage 1A.1. Per memory rule 30, I am flagging this provenance gap explicitly.

### Whipsaw evidence on NAS100

Of 34 IndexFlow NAS100 trades, broken down by gap-to-next-trade:

| Same-symbol direction flip | Gap | Count |
|---|---|---|
| IndexFlow → opposite IndexFlow | < 60s | 0 |
| IndexFlow → opposite IndexFlow | < 120s | 6 |
| IndexFlow → opposite IndexFlow | < 180s | 10 |
| IndexFlow → opposite IndexFlow | < 300s | 13 |

**13 direction flips within 5 minutes on the same symbol from the same engine** in a single trading session. Median price gap between flipped entries: **15 NAS100 points**. Net P&L of the second trade in each flip pair: **-$3.72** (small dollars but 100% loss-sided pattern).

US500.F: zero flips (only 1 trade). The whipsaw is NAS100-specific in this sample, almost certainly because NAS100 had a chop regime during the 14:00-15:00 UTC window where 11 of the 13 flips occurred. The engine itself is presumably working as designed elsewhere — the issue is "how does it behave in chop?"

### Recommendation shape (not specification)

The fix should have three components, in priority order:

**A. Same-engine same-symbol direction-flip cooldown.**
After IndexFlow opens a position on a symbol with a given direction, block opposite-direction IndexFlow entries on that same symbol for a cooldown window. Cooldown candidate: **120-180 seconds** based on the data (would have blocked 6-10 of the 13 flips). Provisional only — actual cooldown should be calibrated against backtest of the engine's intended hold time, which I don't yet know without reading the source.

This is engine-internal. It does not involve the cross-engine gate (separate Tier 2 issue in Section 4).

**B. Range/regime-based suppression.**
The 14:00-15:00 UTC window where 11 of 13 flips occurred was a NAS100 chop regime. IndexFlow's chop behaviour is the failure mode. Fix candidate: detect range-bound conditions on the engine's own input series (e.g. ATR-of-recent-bars below threshold, or Bollinger-band-width compression, or NR-bar-count > N) and suppress IndexFlow entries while that condition holds. **This is the higher-leverage fix** — the cooldown only catches the *next* flip; the regime gate prevents the entire chop sequence.

**C. (Optional, decide after A+B) — runner trail addition.**
Per Section 3, IndexFlow loses MFE > 1pt to SL on 25 of 35 trades. Even with whipsaw fixed, leaving favourable runners on the table is the larger expected-value fix long-term. But this is a separate concern from the whipsaw pattern and should be specified separately, not bundled.

### What this recommendation explicitly does NOT cover

- **The cross-engine direction gate** (HBG vs VWAP_SNAPBACK case from 04-28). That's a different problem with a different fix surface (likely in `on_tick.hpp` or `tick_gold.hpp`), and it's blocked on the on_tick.hpp full read in Stage 1A.1.
- **HybridBracketIndex.** HBI has only 8 trades in the sample with no internal flip pattern — its bleed is from sizing × loss-per-trade, not whipsaw. Different fix entirely (Section 8 Tier 1A).
- **Specification of parameter values, code locations, or integration points.** Stage 1A.1 source read produces those.
- **Backtest validation of the proposed cooldown/regime gate.** Required before any deploy. Once the source is read and the fix is specified, the backtest harness (`OmegaSweepHarness` per memory #29) can sweep cooldown/regime parameters on the historical NAS100 data before any live change.

### Open questions to answer in Stage 1A.1

1. Does IndexFlow already have any cooldown mechanism? If yes, what's the current value, and why isn't it firing on these 120-300s flips?
2. Does the engine have an internal regime classifier? Memory mentions `IFLOW` regime tag — what triggers it, and is there a "no-trade" regime tag possible?
3. Is the engine's signal logic event-driven (each tick re-evaluated) or threshold-driven (signal latches until X)? Different fix shapes apply.
4. Per memory #23, "IndexFlow gates loosened 3a6b7ff7" — what was loosened? Was that loosening a contributor to current chop sensitivity?

---

## 5. HBG trail behaviour analysis

3 HBG trades on 04-28:

| # | entry | exit | MFE (pts) | gross | hold |
|--|---|---|---|---|---|
| 1 | 4649.51 SHORT | 4656.49 (SL) | 0.50 | -$6.98 | 13m50s |
| 2 | 4648.78 SHORT | 4646.17 (TRAIL) | 3.59 | +$2.61 | 16s |
| 3 | 4632.13 SHORT | 4632.03 (TRAIL) | 3.43 | +$0.10 | 15s |

Trade #1: MFE was only 0.50 — trade was simply wrong-direction from the start. Entry quality issue, not a trail issue.

Trades #2 and #3: trail fired but locked tiny amounts. If trail logic locks 80% of MFE per memory, MFE 3.43 should permit retrace of 0.69 from peak. Trade #3 actual: peak ≈ 4628.70, exit 4632.03 = +3.33 from peak, far more than 0.69. So either:
- Trail is NOT locking 80% of MFE in current code
- Trail uses different reference point
- There's a min-lock floor at ~$0.10 from entry

**Action:** read GoldHybridBracketEngine.hpp (28,794 bytes, not yet pulled) to verify current trail formula. **Do not assume the 80% figure from userMemory is current.**

---

## 6. Schema defects in trade ledger

### 6a. MFE/MAE units inconsistent between engines

For XAUUSD trades:
- **HybridBracketGold**: MFE/MAE in PRICE POINTS (HBG #1: MFE=0.50 = 0.50 USD/oz movement)
- **VWAPSnapback / SessionMomentum**: MFE/MAE in TICKS (VWAP_SNAPBACK: MFE=162.00 ticks = $1.62 movement)

For NAS100/USTEC.F trades:
- **IndexFlow / HybridBracketIndex**: MFE/MAE in PRICE POINTS

This is a 100x scale mismatch within the same column of the same CSV. Aggregating MFE/MAE statistics naively (as I initially did) produces meaningless results when XAUUSD non-bracket engines are mixed with bracket engines.

**Action:** trace the MFE/MAE write path for each engine in `tick_gold.hpp` (130 KB) and `trade_lifecycle.hpp` (92 KB). Standardise to one unit.

### 6b. Fill quality fields zeroed for bracket engines

| Engine | trades | spread=0 | slip_total=0 | mae=0 |
|---|---:|---:|---:|---:|
| HybridBracketIndex | 8 | 8 | 8 | 8 |
| HybridBracketGold | 3 | 3 | 3 | 0 |
| IndexFlow | 35 | 35 | 35 | 0 |

HybridBracketIndex trades have ZERO data populated for any fill-quality field. Cannot diagnose entry/exit quality, slippage, or true execution cost for this engine from the ledger. HybridBracketGold has spread/slippage missing. IndexFlow has spread/slippage missing.

**Implication:** any cost-of-trading analysis on these engines is impossible from current ledger data. The "all bracket fills appear free" pattern strongly suggests the bracket-order fill path bypasses the slippage capture code that engine-driven entries go through.

### 6c. Opens-logger silent-fail path

`logging.hpp` L82-88: opens-CSV write is gated on `if (g_trade_open_csv.is_open())` with no else branch logging the failure. If the file isn't open, opens are silently dropped.

`omega_trade_opens_2026-04-28.csv` is 260 bytes (less than one row). Only one open written, despite 5 closes the same day. Either the daily file rotation didn't happen or the handle silently failed. Not yet investigated further (parked per session decision).

### 6d. omega_trade_opens_2026-04-26.csv missing

Closes file for 04-26 exists (672 bytes); opens file for 04-26 does not exist. Sunday/low-volume day, possibly no opens that day, but the closes file has content (positions opened earlier closed on 04-26). Logging asymmetry to investigate.

---

## 7. Things confirmed working

- HBG trail mechanism — fires on favourable retrace (mechanical correctness, magnitudes need verification per Section 5)
- IndexFlow breakeven-trail — fires after favourable excursion to lock BE (9 of 35 trades scratched cleanly)
- HybridBracketIndex trail on big winners — 2 of 8 trades captured +$22.73 and +$89.17 via TRAIL_HIT
- TrendPullback (NAS) — 3 trades, 2 wins, 1 BE scratch, +$9.68 net. Re-enable was a good call.
- LondonFixMomentum — 1 trade TP_HIT for +$10.63
- DynamicRange — 12 trades, 50% WR, +$6.99 net. Best high-frequency producer.

---

## 8. Recommended next steps (ranked, not approved)

### Immediate (Stage 1A.1 — pre-action investigation)

1. **Read GoldHybridBracketEngine.hpp** to verify current trail formula. Memory says "MFE-proportional 80%". Live behaviour suggests different. Resolve before any HBG change.
2. **Read full on_tick.hpp gate chain** to confirm presence/absence of cross-engine direction gate. Currently provisional finding only.
3. **Read IndexFlowEngine.hpp** (58 KB, not yet pulled) to specify the whipsaw cooldown + regime gate per Section 4a. Answer the four open questions in Section 4a.
4. **Read IndexHybridBracketEngine.hpp** (25 KB, not yet pulled). Why is sizing 100x HBG? Why are fill-quality fields zero?

### Tier 1 candidate actions (after Stage 1A.1 investigation)

**A. HybridBracketIndex sizing/gate review** — high avg-loss engine. Either resize down (move from 1.0-1.6 contracts to something proportionate to HBG's risk per trade) or tighten gate conditions. Current 25% WR with -$24 avg loss is unsustainable even with the occasional big TRAIL_HIT win.

**B. IndexFlow whipsaw fix (Section 4a)** — same-symbol direction-flip cooldown + range/regime suppression. Specification produced after source read. Highest ratio of "patterns observable in ledger" to "fix complexity" of any item in this list.

**C. System-wide trail audit** — every engine in the bleed list above (TrendPullback, SessionMomentum, DonchianBreakout, VWAP_SNAPBACK, IndexFlow, HBI, HBG) needs explicit answer to: "does this engine have a runner trail (lock-in-favour past breakeven), and if so what's the formula?" Engines that lack one are giving back MFE>3 to SL_HIT routinely.

### Tier 2 candidate actions

**D. Schema cleanup** — standardise MFE/MAE units across all engines. Fix bracket-engine fill-quality field population. Investigate opens-logger silent-fail.

**E. MomentumContinuation and DXYDivergence shadow gating** — comments in source intend shadow mode but no flag is set. Either add real shadow gating or remove the misleading comments.

**F. Cross-engine direction gate** (Section 4 finding). Lower priority than B because the same-symbol same-engine flip is the higher-frequency pattern in this dataset (13 cases vs 2 cross-engine cases), but worth specifying once on_tick.hpp is read.

### Out of scope for this audit

- MacroCrash re-enable (Stage 1B — separate audit)
- sim_lib defence parity (Stage 2)
- Full ecosystem audit (Stages 3-5)

---

## 9. What this audit explicitly did NOT do

- Modify any code
- Recommend a fix beyond ranked candidate actions for review
- Read all 18+ engine source files (only ones supported by ledger evidence, and even those not exhaustively)
- Pull stdout log gate traces (file too large for this session, can be done next session if needed)
- Resolve the regime-tag disagreement (HBG=COMPRESSION vs VWAP_SNAPBACK=MEAN_REVERSION 43s apart) — this needs source verification of when `regime` is captured into TradeRecord (entry-time or exit-time)
- Investigate the opens-logger gap (parked per session decision)
- Specify the whipsaw fix's actual code, parameter values, or integration points (Section 4a is shape only — Stage 1A.1 produces the spec)

---

## 10. Provenance

**Source files read (full content via GitHub contents API at commit ed95e27c):**
- include/GoldEngineStack.hpp (230,079 bytes) — 18 engines confirmed, EngineBase default verified
- include/engine_init.hpp (98,243 bytes) — global flags reviewed
- include/tick_gold.hpp (130,685 bytes) — partial: opens-logger call sites located
- include/trade_lifecycle.hpp (92,738 bytes) — opens-logger call site located, definition not present
- include/omega_runtime.hpp (35,834 bytes) — opens-logger forward declaration found
- include/logging.hpp (8,672 bytes) — opens-logger definition found, silent-fail confirmed
- include/OmegaTradeLedger.hpp (12,803 bytes) — no opens references

**Source files NOT read (required for Stage 1A.1 follow-up):**
- include/GoldHybridBracketEngine.hpp (28,794 bytes) — for trail formula
- include/IndexFlowEngine.hpp (58,961 bytes) — for whipsaw fix specification
- include/IndexHybridBracketEngine.hpp (25,639 bytes) — for sizing/fill-quality
- include/on_tick.hpp (128,307 bytes) — for cross-engine gate chain (full read needed)

**Ledger data analysed:**
- omega_trade_closes_2026-04-27.csv (22,727 bytes, 68 trades) — uploaded by user
- omega_trade_closes_2026-04-28.csv (2,025 bytes, 5 trades) — uploaded by user

**Ledger data NOT read:**
- omega_trade_opens_*.csv files (parked)
- omega_service_stdout.log (27.8 MB, too large for one upload — keyed gate-trace reads possible next session)
- omega_gold_trade_closes_*.csv (likely subset of omega_trade_closes — duplicate data)
- L2 tick data (out of scope)

---

## 11. Provisional plan update

- **Stage 0** ✅ done — CHOSEN.md defence-gap footnote
- **Stage 1A** ✅ done — this document
- **Stage 1A.1** (next session, pending review) — investigation reads (4 files listed above) producing concrete fix specifications for: (i) IndexFlow whipsaw cooldown + regime gate, (ii) HBG trail formula audit, (iii) HBI sizing/fill-quality, (iv) on_tick cross-engine gate. Each spec backtested against historical data BEFORE any push.
- **Stage 1B** (subsequent session) — MacroCrash re-enable audit
- **Stage 2** — sim_lib defence parity
- **Stages 3-5** — full ecosystem audit by subsystem

**No code changes are recommended for execution this session.** Stage 1A.1 is the next session's job — concrete source reads with concrete questions, no freelancing.
