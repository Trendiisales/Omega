# Validation Watchlist  (S37-Z, 2026-05-28)

Engines that need extended live-shadow or extended-corpus measurement
before permanent flag decisions. Source: this session's CRTP audit +
xau_d1_zoo_audit re-run with NET-cost correction.

## Cost model used in audits

`xau_d1_zoo_audit.cpp` patched 2026-05-28 to subtract round-trip spread
from `TradeRecord.pnl`:

- Non-TP exits (SL / TIMEOUT / OTHER): RT cost = $0.30
- TP exits: RT cost = $0.15 (TP fills at limit, no exit slip)

Cost derived from `apply_realistic_costs` XAU preset (0.010% one-way slip
at typical $0.30 broker spread).

`disabled_engine_audit.cpp` uses the same model derived from
`TradeRecord.spreadAtEntry` per-trade.

The original `xau_d1_zoo_audit` reported GROSS Sharpe -- inflated every
engine's apparent edge. Six engines were promoted as VIABLE that are
NET losers under realistic cost.

---

## ROBUST SURVIVORS  (large sample, net-cost positive, walk-fwd holds)

| Engine | Status | NET evidence | n | Next gate |
|---|---|---|---|---|
| **EmaPullback** (`g_ema_pullback`) | LIVE shadow | PF ~1.45 net of $0.30 RT cost; $7088 net / 475 trades | 475 | None -- robust sample, keep live. |
| **IDD_SPX** (`g_idd_sp`) | LIVE shadow (S37-Z 2026-05-28) | walk-fwd Sharpe +1.28 / +0.48 net; cumret +28.67% over 2.3yr | 586 days | 30 live-shadow trades net positive -> promote `shadow_mode=false`. |
| **IDD_USA30** (`g_idd_us30`) | LIVE shadow (S37-Z) | walk-fwd Sharpe +1.04 / +1.53 net; cumret +10.57% over 7mo | 148 days | Same gate. Strongest WF of the basket. |
| **IDD_UK100** (`g_idd_uk100`) | LIVE shadow (S37-Z) | walk-fwd Sharpe +1.01 / +1.46 net; cumret +14.51% over 12mo | 256 days | Same gate. |

## THIN SAMPLES  (positive NET Sharpe but n=20-35 over 2yr -- needs more data)

These show real-class edge with cost subtracted but the sample is too
thin to draw stable conclusions. Track until n >= 60 with consistent
edge.

| Engine | Status | NET Sharpe | n | Next gate |
|---|---|---|---|---|
| **XauTurtleD1** (`g_xau_turtle_d1`) | LIVE shadow | +1.45 | 28 | n >= 60 with Sharpe >= 1.0 -- accumulate 32 more shadow trades. |
| **XauOutsideBarD1** (`g_xau_outside_bar_d1`) | LIVE shadow | +1.21 | 34 | n >= 60, Sharpe >= 1.0. |
| **XauDojiRejD1** (`g_xau_doji_rej_d1`) | LIVE shadow | +0.61 | 46 | Marginal. n >= 100 with Sharpe >= 0.8. If drift below: disable. |

## DISABLE-CONFLICTS  (my audit disagrees with prior session calls)

The xau_d1_zoo audit's NET-cost re-run produced positive Sharpe for
three engines that prior sessions disabled. The disagreement may be:
- Different cost model (prior sessions may have used different presets)
- Different corpus window
- Sample artifact (small n)

Do NOT auto-re-enable. Re-audit explicitly with the disagreement
documented as a hypothesis to test.

| Engine | Current | Prior disable | My NET Sharpe | Action |
|---|---|---|---|---|
| `g_xau_stop_run_d1` | DISABLED (S57 regime LOW neg) | S57 | +1.68 (n=28) | Re-audit with S57 regime split before re-enable. |
| `g_xau_pullback_cont_d1` | DISABLED (S54 WF OOS fail) | S54 | +1.28 (n=33) | Re-audit with explicit WF split before re-enable. |
| `g_xau_ema_cross_h4` | DISABLED (S57 regime LOW catastrophic) | S57 | +0.74 (n=20) | Marginal Sharpe + sample tiny. Skip -- evidence not strong enough to overturn S57. |
| `g_gold_ultimate_engine` | DISABLED (S99b "off-hours edge-hour design bleeds") | S99b | OOS PF 1.28 / IS PF 0.85 over 2024-2026 | GoldUltimateBacktest STRONG PASS on OOS half (Mar-2026..Apr-2026, 293 trades). S99b live evidence may have been different regime. Re-audit before re-enable. |

## DISABLED THIS SESSION  (S37-Z 2026-05-28 commits)

| Engine | NET evidence | Commit |
|---|---|---|
| `g_xau_pullback_cont_h4` | Sharpe -1.02, gross -$6.91 over n=102 (robust) | (this commit) |

Other disables already in prior sessions (S53/54/57) all CONFIRMED by
the cost-net audit. No re-enables.

## CONFIRMED VIABLE (already live, no action)

* `g_idd_sp/us30/uk100` -- see above
* `g_ema_pullback` -- see above
* `g_xau_turtle_d1` -- see THIN
* `g_xau_outside_bar_d1` -- see THIN
* `g_xau_doji_rej_d1` -- see THIN

## CONFIRMED THIS SESSION

| Run | Result |
|---|---|
| TrendPullback_NQ on USA30 (21M ticks, 7mo)   | PF 0.241, -$250.26, all mo neg | -> disabled c0dbcfa4 |
| TrendPullback_NQ on NSXUSD (89M ticks, 16mo) | PF 0.448, -$127.36, all 16 mo neg | confirms USA30 verdict |
| VWAPRev_EURUSD via CRTP harness   | PF 0.952, ~$0 | -> disabled c0dbcfa4 |
| VWAPRev_EURUSD via VWAPReversionBacktest direct | gross 0.000757 over 2431 trades | independently confirms |
| **GoldUltimateBacktest** on 2yr XAU | IS PF 0.85 (40 tr) -> OOS PF **1.28** (293 tr); retention 150% STRONG PASS | DISABLE-CONFLICT (S99b off; flag for re-audit) |
| Nas100UltimateBacktest on NSX 89M | IS PF 2.36 / 43 SHORT trades; OOS n=0 | inconclusive; not in production |
| Spx500UltimateBacktest on SPX H1 | n=0 (H1 bars not tick) | needs SPX tick corpus |

## DEFERRED  (need per-engine cmake target build + format adapter)

| Engine | Why deferred | Need |
|---|---|---|
| `g_tsmom_v2` | TsmomCellBacktest gross +74% but cost subtraction not verified | TSMOM Cell BT cost-model audit |
| `g_donchian` (DonchianPortfolio) | Manage-only path in current CRTP harness | on_bar dispatch wiring + bar replay |
| `g_xau_tf_1h/2h/4h/d1` (XauTrendFollow 4 tfs) | S37 trail tombstoned; base path not zoo-audited | Tf-specific harness or extend zoo |
| `g_us30_ensemble` | DJ30 ensemble, multiple cells | Per-cell audit on USA30 corpus |
| `g_ger40_turtle_h4` / `g_eurusd_turtle_h4` / `g_gbpusd_turtle_h4` | Per-symbol H4 bar replay needed | FX/EU H4 corpora + Turtle harness |
| `g_eur_gbp_pairs` | Pairs engine multi-leg | EurGbpPairs harness extension |
| `g_minimal_h4_us30` / `g_minimal_h4_ger40` | M4 breakout per-index | Index H4 bar replay |
| `g_ustec_tf_htf` | M15/H1/H2/H4 ensemble | Multi-tf harness on NSX corpus |
| `g_ger40_london_brk` | London-open breakout on GER40 | GER40 H1/H4 bars (no tick corpus) |
| `g_c1_retuned` | C1Retuned cohort | Symbol-specific harness |
| `g_xau_threebar_30m` | M30 momentum gate | M30 bar harness |
| `g_vwap_rev_ger40` | LIVE shadow | GER40 tick corpus |
| GoldScalpPyramid / FxScalpPyramid x5 | All SHADOW; family proven unviable | L2 synthesizer to formally audit |
| `XauTrendFollow` 4 tfs | XauTrendFollowBacktest returns n=0 (tape format / init issue); separate engine path needed | Debug binary init + run |
| `UstecTrendFollowHTF` | UstecTrendFollow5mBacktest binary built but rejects ms-epoch ASK_BID; needs ts,bid,ask | Format converter or harness flag |
| `IndexBacktest` (NAS100/NSX) | Built; 100% parse-fail on ms_ask_bid -- expects HistData YYYYMMDD format | Convert NSX combined to HistData |
| Per-symbol `*UltimateBacktest` (Gold/SPX/NSX/GER40) | Source files exist; no cmake target | Add 4 add_executable() blocks |
| `IndexFlowBacktest` / `IndexBracketBacktest` / `IndexORBBacktest` / `IndexVwapRev` / `IndexNbm` | Source files exist; no cmake target | Add 5 add_executable() blocks |
| `XauEmaPullbackBacktest` / `XauFvgBacktest` | Source files exist; no cmake target | Add 2 add_executable() blocks |
| `GoldTrendEnsembleBacktest` | Source file exists; no cmake target | Add 1 add_executable() block |

## RE-VALIDATION DISCIPLINE (for any flag flip)

1. Real production class (no inline-reimpl). See HARNESS_FIDELITY_CHECKLIST.
2. OmegaTimeShim active (engine session/cooldown resolve vs tape time).
3. Cost subtraction matching apply_realistic_costs preset.
4. Walk-fwd train/test split. Both halves Sharpe >= 0.5.
5. Sample-size floor n >= 60.
6. Per-month stability scan (no single month carries the gross).
7. Update this doc + globals.hpp comment with evidence + commit hash.
