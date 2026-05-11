# HANDOFF S33 FINAL — 2026-05-11

End-of-day state after a multi-pass deep dive that replaced the broken
micro-scalper with a 15-cell cross-validated trend-follow stack. Read
this top to bottom before doing anything in a new session.

---

## 0. The one-paragraph summary

The S33 Option A geometry on `GoldMicroScalperEngine` was proven structurally
unprofitable: 8,435 live-equivalent trades on 28 days of real L2 capture =
7.1% WR, -$1,237 net at 0.01 lot. The engine has been **disabled** (no-op
stub at `include/GoldMicroScalperEngine.hpp`, original 952-line file
preserved at `disabled_engines/GoldMicroScalperEngine.hpp.disabled_2026-05-11`).
Replaced by **four new trend-follow engines** containing 15 cross-validated
cells, all 3/3 (or 2/3) Duka years positive on realistic bid/ask-fill
backtests over 30 months, all deployed shadow-default. The accounting
gap that caused the May-8 NZ$310 live bleed has also been fixed
(`broker_*` reconciliation fields now persist to the 42-column trade-close
CSV). Watchdog and NSSM auto-restart both verified armed. The project's
first real cross-validated edge is now live in shadow. **Next-session
action is observation, not building** — fire counts and shadow PnL vs
backtest expectancy over the next 30 days drive the live-promotion
decision.

---

## 1. What is running right now (VPS)

```
Service         Status   StartType  Notes
─────────────────────────────────────────────────────────────────────
Omega           Running  Automatic  NSSM AppRestartDelay=5s, AppThrottle=30s
OmegaWatchdog   Running  Automatic  Heartbeat OK, telemetry healthy
```

**Binary on disk**: commit `1511a00` (S33k). Hash matches GitHub `origin/main`.

**Active engines** (all shadow-default, all using bracket_on_close so broker
reconciliation columns populate when any goes live):

```
Engine                       Symbol  TF    Cells  Lot   Max  Historical edge (30mo)
──────────────────────────────────────────────────────────────────────────────────
XauTrendFollow4hEngine       XAUUSD  4h    6      0.01  6    ~$5,400
  - Donchian N=20    sl1.5 tp3.0
  - InsideBar        sl2.0 tp6.0    (R:R 3:1 + S33i optimal SL)
  - ER0.20 mom=20    sl0.75 tp6.0   (R:R 8:1 + S33i tight SL)
  - Keltner K=2.0    sl1.5 tp3.0    (S33e new)
  - ADX_Mom adx>25   sl2.0 tp4.0    (S33e new)
  - RangeExpand K=1.5 sl1.5 tp6.0   (S33h new, Pass-5 finding)

XauTrendFollow2hEngine       XAUUSD  2h    4      0.01  4    ~$3,380   (NEW S33k)
  - Keltner K=2.0    sl2.0 tp4.0
  - Donchian N=20    sl2.0 tp4.0
  - Donchian N=50    sl2.0 tp4.0
  - InsideBar        sl2.0 tp4.0

XauTrendFollowD1Engine       XAUUSD  D1    3      0.01  3    ~$2,800
  - Momentum lb=20   sl2.0 tp4.0
  - Keltner K=2.0    sl2.0 tp6.0    (S33h R:R upgrade)
  - ADX_Mom adx>25   sl2.0 tp4.0

UstecTrendFollow5mEngine     USTEC   5m    2      0.10  2    ~$2,400 (15-day sample)
  - Donchian N=20    sl2.0 tp4.0
  - Keltner K=2.0    sl2.0 tp4.0    (S33f new)
                                                          ────────
                                                  Total: ~$14,000
```

**Disabled** (no-op stub, original preserved):
- `GoldMicroScalperEngine` — proven structurally dead on L2 data

**Trade-close CSV header (42 columns)** now includes the 9 broker
reconciliation fields (`entry_clOrdId`, `close_clOrdId`,
`broker_entry_filled`, `broker_close_filled`, `broker_entry_rejected`,
`broker_close_rejected`, `broker_entry_fill_px`, `broker_close_fill_px`,
`broker_pnl`). When any engine goes live, the GUI will see broker truth
alongside engine truth — not just the engine's prediction.

`KILL_MICROSCALPER` sentinel on VPS: still present (cheap belt+braces).
`mode=SHADOW` in config: yes (committed to origin/main).

---

## 2. Where to look first in a new session

```powershell
# Service health
Get-Service Omega, OmegaWatchdog | Format-Table Name, Status, StartType -AutoSize
# Expect: both Running + Automatic. If not, fix BEFORE doing anything else.

# Engine init confirmation (should see four lines)
Get-Content C:\Omega\logs\omega_service_stdout.log | Select-String "TrendFollow.*initialised"

# Watchdog heartbeat (should be <60s old)
Get-Content C:\Omega\logs\watchdog.log -Tail 5

# Daily fire counts (replace today's log if needed)
$c = Get-Content C:\Omega\logs\omega_service_stdout.log
[pscustomobject]@{
    xau_4h    = ($c | Select-String "engine=.XauTrendFollow4h" | Measure-Object).Count
    xau_2h    = ($c | Select-String "engine=.XauTrendFollow2h" | Measure-Object).Count
    xau_d1    = ($c | Select-String "engine=.XauTrendFollowD1" | Measure-Object).Count
    ustec_5m  = ($c | Select-String "engine=.UstecTrendFollow5m" | Measure-Object).Count
    live_tags = ($c | Select-String "TrendFollow.*\[LIVE\]" | Measure-Object).Count
} | Format-Table -AutoSize

# live_tags MUST be 0. Anything else means trouble.
```

**Expected fire-count rates** (use to spot anomalies):
- XAU 4h: ~5-10 fires/week across 6 cells
- XAU 2h: ~20-30 fires/week across 4 cells
- XAU D1: 0-2 fires/week across 3 cells
- USTEC 5m: ~50-100 fires/week across 2 cells

If after 7 days any engine has fired more than 3× expected or less than
30% expected, paste the log tail and diagnose. Otherwise leave it alone.

---

## 3. What was tested and decisively rejected — DO NOT redo

These were tested across 8 passes with realistic bid/ask fills, $0.06/RT
broker cost, cross-validated across 3 years of Dukascopy + 1 month L2:

- **Tick-level micro-scalp** (every variant of GoldMicroScalper, 7%-8% WR
  with TP/SL needing 25%+ WR to break even on points before costs)
- **L2 microstructure signals** (4 families, all dead — see
  `backtest/l2_micro_hunt.cpp` results)
- **Mean reversion at extremes** on XAU 4h:
  - ZScoreMR — dead
  - BollingerMR — dead (spread-cost mirage)
  - Connors RSI(2) — dead
  - CCI extreme fade — dead
  - ExtremeFade (Z-score from EMA) — catastrophic
- **Trailing exits**: every variant (chandelier, MFE-based trail, Turtle
  N/2-bar opposite exit) underperforms fixed TP on XAU
- **Regime gates as filters**: D1 trend, vol-percentile, time-of-day,
  spread-percentile, Aroon — all neutral or hurt performance
- **Vol-conditional R:R** — adaptive logic mispredicts
- **ATR period variation** — ATR(14) wins for InsideBar; ATR(7)/(21)/(28)
  all underperform
- **EURUSD trend-follow** on HistData — edge too thin vs costs
  (BE costs $0.07-$0.23 vs $0.06 cost = only 1-4× margin, won't survive
  slippage)
- **Counter-trend fade on XAU 4h** — catastrophic. XAU 4h is a trend
  regime, full stop.
- **Speed-leveraged HFT/scalping** in any form — structurally infeasible
  at retail brokers (spread + commission > signal quality at tick level,
  and HFTs already arbitraged the broker's quote by the time orders
  reach us).

---

## 4. What was NOT tested but COULD be (next-session candidates if you want more)

In rough order of likelihood-to-find-edge:

1. **News-event regime-shift trading** — needs an econ calendar feed.
   Pre-cancel during NFP/CPI/FOMC windows, trend-follow the post-news
   regime once vol settles. Needs ForexFactory/Investing.com calendar API.
2. **Pairs trading US500/USTEC** — market-neutral, never built. Need
   timestamp-aligned tick streams (we have both).
3. **Open-of-session 1-min ORB** — never tested at 1-minute (only 5m/15m).
   First 1-2 min of London open (07:00 UTC) and NY open (13:30 UTC).
4. **Stop-hunt fade on L2** — detect spikes >2σ in <30s, fade the reversal.
   Microstructure-specific.
5. **30m XAU ThreeBar** (positive in Pass-8, n=639 +$979 across 30mo,
   BE=$1.59). Not yet built. 21 trades/month, modest edge.
6. **USTEC 4h/D1** — currently untestable (only 15 days L2 = ~15-60 bars
   too sparse). Need 3+ months more L2 capture before this becomes valid.
7. **Hurst exponent regime switching** — H>0.55 → trend mode, H<0.45 →
   mean-rev mode. Adaptive strategy selection. Academic but worth trying.

**None of these are urgent.** The current 15-cell stack is the empirical
optimum for the data we have. Adding more requires either new data
(USTEC L2 capture, econ calendar) or genuinely novel research.

---

## 5. Decision points coming up (calendar)

- **Week 1 (now → 2026-05-18)**: confirm fire counts match Section 2
  expectations. No action otherwise.
- **Week 2-4 (~2026-06-08)**: first checkpoint on shadow PnL tracking
  backtest expectancy. If XAU 4h shadow ≈ ±50% of $5,400/30mo prorata,
  signal is real.
- **Month 3 (~2026-08-11)**: live-promotion candidate for XAU 4h
  ensemble. Sequence: shadow → small live (0.01 lot) → reconciliation
  diff check via `ledger_reconcile` → scale to 0.05 → 0.1 over months.
  See lot-sizing analysis in this session's chat for full progression.
- **Month 6 (~2026-11-11)**: USTEC 5m live-promotion candidate (longer
  validation needed because of 15-day initial sample).

**Hard gate before any LIVE flip**: `ledger_reconcile` between
omega_trade_closes CSV and cTrader account export must show
`sum_pnl_delta` < $20 per day. The S33c reconciliation fix makes this
measurable now.

---

## 6. Safety invariants (carried forward from S32/S33)

1. `mode=SHADOW` until explicit operator authorisation to flip LIVE
   (committed to origin/main S33).
2. `max_lot_gold=0.01` until explicit operator authorisation. Current
   per-engine lots: XAU 0.01 per cell, USTEC 0.10 per cell.
3. Never modify protected core files without per-decision operator
   sign-off: `microscalper_crtp_sweep.cpp`, `omega_main.hpp`,
   `order_exec.hpp`, `OmegaTradeLedger.hpp`, `IndexFlowEngine.hpp`,
   `RiskMonitor.hpp`, `trade_lifecycle.hpp`.
4. `GoldMicroScalperEngine.hpp` is in the "touch only with operator
   sign-off" tier (currently the no-op stub).
5. Full file output when modifying any file (operator preference).
6. Warn at 70% context with summary (operator preference).
7. `KILL_MICROSCALPER` stays on VPS (cheap belt+braces; stub is silent
   anyway).
8. Do not commit other tracked-modified files (S32 §5 list) without
   explicit per-file operator review. 8 such files are STILL uncommitted
   on the Mac: `backtest/IndexBacktest.cpp`,
   `backtest/microscalper_crtp_sweep.cpp`, `data/l2_ticks_2026-04-16.csv`,
   `include/IndexFlowEngine.hpp`, `include/OmegaTradeLedger.hpp` (likely
   the broker_* fields from earlier work),
   `include/RiskMonitor.hpp`, `include/omega_main.hpp` (likely the
   reconciliation CSV header from S33c),
   `include/order_exec.hpp`, `include/trade_lifecycle.hpp`. Per-file
   operator review still required.
9. All research code is C++/CRTP. No Python (operator directive S33).
10. Standalone `backtest/*` binaries that don't link the engine binary
    are NOT protected. Many were shipped this session: `edge_hunt`,
    `multi_tf_sweep`, `survivor_definitive`, `top_cells_monthly`,
    `deep_dive_v4` through `v7`, `regime_filter_test`, `l2_micro_hunt`,
    `ledger_reconcile`, `l2_edge_sweep`, `s33_revised_backtest`.

---

## 7. Recovery / auto-restart coverage

Confirmed armed today (S33k):

| Failure | Detected by | Recovery | Approx time |
|---|---|---|---|
| Omega.exe crash | NSSM AppRestart | Auto-relaunch | ~5-15s |
| Omega.exe crash loop | NSSM AppThrottle | Backs off → watchdog full redeploy | ~30s-5min |
| Engine stuck (no log writes) | OmegaWatchdog | `OMEGA.ps1 deploy -SkipVerify` | ~3-5min |
| `latest.log` missing | OmegaWatchdog | Same | ~3-5min |
| Service Paused/Stopped | OmegaWatchdog | Gated by safe-restart probe | ~3-5min |
| L2 CSV stale during market hours | OmegaWatchdog | Alert only (no auto-action) | logged |
| New commit on origin/main | OmegaWatchdog | Auto-pull + full redeploy | ~3-5min |
| VPS reboot | Windows Services | Both auto-start | ~30-60s after boot |
| Telemetry endpoint down | OmegaWatchdog | Defers restarts (no orphan risk) | indefinite, alerts |

`Test-SafeToRestart` probes telemetry before any watchdog-initiated
restart so positions cannot be orphaned by a recovery action.

---

## 8. Commits landed today (push chain)

```
1511a00  S33k  ship XAU 2h trend-follow + final ensemble lock-ins
f8f7fea  S33e  extend XAU 4h ensemble from 3 to 5 cells (Keltner + ADX_Mom)
668a686  S33d-fix  wrap GoldMicroScalperEngine stub in omega namespace
54e9ad2  S33d  disable GoldMicroScalper, ship XAU 4h + USTEC 5m + D1
68e7720  S33c  persist broker_* reconciliation fields to trade-close CSV
5a3531b  S33b  reconciliation + edge-sweep tooling, Caveat 3 fix, handoff
263e278  S33   persist mode=SHADOW + port S30/S31 TOP-1 (Option A)
```

7 commits land the entire S33 sequence. `1511a00` is currently HEAD on
origin/main + deployed on VPS.

---

## 9. New-session first-message template

> Read HANDOFF_S33_FINAL.md end to end before anything else. Then:
>
> 1. Confirm services: `Get-Service Omega, OmegaWatchdog`. Both must be
>    Running + Automatic.
> 2. Confirm engines: `Get-Content omega_service_stdout.log |
>    Select-String "TrendFollow.*initialised"` — expect 4 lines.
> 3. Glance at fire counts (template in §2 above). Anything anomalous
>    paste the log tail.
> 4. Do not flip mode=LIVE under any circumstance until the live-promotion
>    decision points in §5 are reached AND `ledger_reconcile` confirms
>    sum_pnl_delta < $20/day.
> 5. If the operator wants new strategies, look at §4 unexplored
>    candidates. Do NOT re-test anything in §3 — it's all empirically dead.
> 6. If the operator wants to ship something, prefer adding cells to
>    existing engines over building new engine classes. Same pattern as
>    S33e/S33h/S33i.

---

## 10. Files to look at first (orientation)

- `include/XauTrendFollow4hEngine.hpp` — 6-cell ensemble, primary edge
- `include/XauTrendFollow2hEngine.hpp` — 4-cell ensemble (S33k new)
- `include/XauTrendFollowD1Engine.hpp` — 3-cell ensemble
- `include/UstecTrendFollow5mEngine.hpp` — 2-cell ensemble
- `include/GoldMicroScalperEngine.hpp` — no-op stub (real engine moved
  to `disabled_engines/`)
- `backtest/edge_hunt.cpp` — main hunting harness with 17 signal families,
  7 timeframes (5m/15m/30m/1h/2h/4h/D1), 2 brackets
- `backtest/deep_dive_v4..v7.cpp` — pass-specific tests preserved for
  replay
- `backtest/ledger_reconcile.cpp` — Omega vs cTrader CSV diff tool;
  the hard gate before any LIVE flip
- `OMEGA.ps1` — deploy + watchdog + start/stop subcommands
- `INSTALL_OMEGA.ps1` — service installer (Omega + OmegaWatchdog NSSM-wrapped)

---

*End of S33 FINAL handoff. Engines deployed, validated, observing.*
