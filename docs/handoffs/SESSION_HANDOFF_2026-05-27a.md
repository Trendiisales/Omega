# Session Handoff — 2026-05-27 part A (NZST)

Read first next session. This session: **full rigour-audit pass over XAU
trend zoo + strict cull**. Operator policy: engines must pass ALL rigour
tests or get disabled. Started with 11 XAU edge engines, finished with
**6 LIVE-eligible**. PortfolioGuard concurrency cap=2 wired and active.

VPS deployed `e7175f6`, service running SHADOW mode, startup verifier
clean (2 yellow WARNs — VIX no tick yet, RSI shadow as designed).

---

## Final state — 6 engines pass ALL 4 rigour tests

| # | Engine | Real-Sh | WF | Regime | Spread |
|---|---|---|---|---|---|
| 1 | XauTurtleD1 | +3.92 | ROBUST | ✓ all 3 | ✓ |
| 2 | XauDojiRejD1 | +4.15 | ROBUST | ✓ all 3 | ✓ |
| 3 | XauPullbackContH4 | +2.69 | ROBUST | ✓ all 3 | ✓ |
| 4 | XauOutsideBarD1 | +5.85 | ROBUST | ✓ all 3 | ✓ |
| 5 | XauTrendFollow1h | +3.58 | PASS | ✓ all 3 | ✓ |
| 6 | XauTrendFollow4h | +1.97 | PASS | ✓ all 3 | ✓ |

**Tier 1 (regime-robust most defensible):** Turtle, DojiRej.

---

## Disabled this session (12 engines)

| Engine | Failure | Commit |
|---|---|---|
| Xau3BarMomGatedH4 | Real-class Sh -1.81 | S50 |
| XauDonchian55GatedM30 | Real-class Sh -1.47 | S50 |
| XauTrendFollowD1 | MDD/gross 95% + regime HIGH -0.80 | S52 |
| XauTrendFollow2h | MDD/gross 75% | S52 |
| XauBBScalpD1 | MDD/gross 240% (worst in zoo) | S53 |
| XauNbmD1 | MDD/gross 111% | S53 |
| XauSwingBreakD1 | MDD/gross 101% | S53 |
| XauPullbackContD1 | WF OOS sign-flip (+4.27 → -1.12) | S54 |
| XauTsmomFastD1 | Regime MID Sh -0.86 | S57 |
| XauStopRunD1 | Regime LOW Sh -3.68 | S57 |
| XauEmaCrossH4 | Regime LOW Sh -8.48 (worst single-regime fail) | S57 |
| XauInsideBarD1 | Regime HIGH Sh -0.31 | S57 |

---

## Commits pushed this session (origin/main)

```
e7175f68 S58: fix VPS build break in S48 kill-switch refresh call site
fa71c915 S57: disable 4 regime-fail engines + add TF cohort rigour audit
9b89ab9e S54+S55+S56: walk-forward + regime split + spread stress
4c1de244 S53: disable 3 more XAU engines failing MDD-to-gross ratio audit
33658d70 S52: disable TrendFollowD1 + TrendFollow2h — MDD-to-gross fails
ff50ea95 S51: wire PortfolioGuard concurrency cap=2 into 12 XAU edge engines
4a726d5d S50: disable 2 real-class-fail XAU engines + extend zoo audit
8b10dedd S49: real-class audit harnesses for XAU D1 zoo + 4/4 confirmed
d88b4407 S48: PortfolioGuard module — cap + vol-scaled + HTF scalar + kill-file
```

---

## Backtest harnesses built (all in `backtest/`)

| Binary | Coverage |
|---|---|
| `test_portfolio_guard` | unit: cap/vol-scale/HTF/kill-file (25/25 PASS) |
| `test_zoo_concurrency` | 4-engine concurrent run, cap=2 verification |
| `xau_d1_zoo_audit` | 13 D1/H4 engines, real-class baseline |
| `xau_trendfollow_audit` | 4 TF engines, real-class baseline |
| `xau_donchian55_m30_audit` | M30 Donchian55 (failed) |
| `xau_zoo_walkforward` | 70/30 IS/OOS split on 9 D1/H4 engines |
| `xau_zoo_regime_split` | 120-bar rolling vol LOW/MID/HIGH terciles |
| `xau_zoo_spread_stress` | $0.30/$0.60/$1.00 round-trip stress |
| `xau_tf_rigour_audit` | full battery for 4 TF cells |

All build with: `clang++ -O3 -std=c++17 -I include backtest/<src>.cpp -o backtest/<bin>`

---

## PortfolioGuard config (active on VPS)

```cpp
omega::pg::g_pg_cfg.max_concurrent_positions = 2;
omega::pg::g_pg_cfg.kill_file_enabled        = true;
omega::pg::g_pg_cfg.kill_file_path           = "C:/Omega/KILL_SWITCH.lock";
omega::pg::g_pg_cfg.kill_file_recheck_sec    = 30;
omega::pg::g_pg_cfg.vol_scale_enabled        = false;  // opt-in
omega::pg::g_pg_cfg.htf_scalar_enabled       = false;  // S44 lesson
```

Engines with concurrency cap wired (`can_open_new_position()` + register
calls): XauTurtleD1, XauTsmomFastD1, XauStopRunD1, XauNbmD1,
XauPullbackContH4, XauPullbackContD1, XauEmaCrossH4, XauBBScalpD1,
XauSwingBreakD1, XauDojiRejD1, XauOutsideBarD1, XauInsideBarD1. Even
disabled engines retain the wiring — no-op when `enabled=false`.

**Not yet wired:** XauTrendFollow1h/2h/4h/D1 (multi-cell pos[] arrays
need per-cell counting — separate task).

---

## Backtest rigor scoreboard

✅ Real engine class (not inline reimpl)
✅ 26mo XAUUSD H4/H1 tape with pessimistic intra-bar (low-first SL)
✅ Walk-forward 70/30 IS/OOS split
✅ Regime split (LOW/MID/HIGH vol terciles)
✅ Spread stress $0.30/$0.60/$1.00
✅ Concurrency cap multi-engine portfolio test
⚠ TF cohort concurrency cap not wired (multi-cell)
⚠ No commission/swap modelled
⚠ No weekend gap simulation
⚠ Multi-engine concurrency only tested with 4 engines (not full 6-engine
  live set)
⚠ No Monte Carlo / trade resampling for CIs
⚠ WF OOS trade counts thin (4-13 for some engines) — sign-only
  meaningful, specific Sharpe values are noisy

---

## VPS deploy verification (e7175f6 build, 00:34:35 UTC)

```
[OK] Service=Running
[OK] EXE timestamp matches built binary (+0s)
[OK] Git hash confirmed in log: e7175f6 == e7175f6
[PASS] L2 Tick CSV XAUUSD age=0s imb=0.922 real=9/20
[PASS] L2 Tick CSV US500 age=0s
[PASS] L2 Tick CSV USTEC age=0s
[PASS] Bar State e9=4518.287565 atr=2.694821 rsi=49.8044 age=1min
[PASS] Bar Periodic Save
[WARN] VIX Level: No VIX.F tick in first 10s (will update when arrives)
[WARN] RSI Reversal in SHADOW (designed-as)
```

All engines should be in shadow_mode=true regardless. No engine pinned
live. Boot logs need confirmation:
- `[OMEGA-INIT] PortfolioGuard: cap=2 kill_file=C:/Omega/KILL_SWITCH.lock recheck=30s`
- 6 `[SEED]` lines for survivors
- No engines emitting `[XAU_TURTLE_D1] ENTRY ...` etc on first hour
  (D1 engines fire once per day max)

---

## What's pending / next session candidates

### Immediate (operator decision needed)
1. **Shadow forward-test duration**: 2-4 weeks recommended before any
   LIVE flip. Compare predicted vs actual on the 6 survivors.
2. **LIVE flip policy**: Tier 1 only (Turtle + DojiRej) is safest first
   step. Tier 2 (PullbackH4 + OutsideBar) regime-positive but HIGH
   regime weak. Tier 3 (TF1h/4h) clean — defensible LIVE candidates.

### Carry-forward tasks
- **#17 (pending)**: max_daily_loss_cuts per-engine count cap. Operator
  asked for this in S43; never built.
- **TF concurrency wiring**: extend `omega::pg::register_position_*`
  pattern to multi-cell TrendFollow engines. Per-cell counting needs
  iterating `pos[]` array.
- **Commission/swap model**: add to backtest harnesses. BB OTC swap on
  XAUUSD ~-$5/0.01lot/night for longs. ~5d hold avg = -$25 drag/trade.
  Could erase Tier 3 edge.
- **Weekend gap sim**: Sunday open gaps on XAUUSD can be $5-15. Engines
  holding through weekend exposed.

### Open MGC subscription (deferred to month-end 2026-06-01)
- CME COMEX MGC futures top-of-book at IBKR
- Bridge already wired (`tools/ibkr_dom_bridge.py` line 39)
- Subscribe via TWS, then rerun L2 microstructure probes that died on
  S13 cTrader cull.

---

## Files modified this session (commit-by-commit)

**S48 (prior commit `d88b4407`):** PortfolioGuard.hpp + test
**S49 (`8b10dedd`):** xau_turtle_d1_audit.cpp, xau_d1_zoo_audit.cpp
**S50 (`4a726d5d`):** engine_init.hpp (2 disables) + extended zoo audit
**S51 (`ff50ea95`):** 12 engine .hpp files + engine_init.hpp + test_zoo_concurrency.cpp
**S52 (`33658d70`):** engine_init.hpp (TFD1+TF2h disable)
**S53 (`4c1de244`):** engine_init.hpp (BBScalp+Nbm+SwingBreak disable)
**S54+55+56 (`9b89ab9e`):** xau_zoo_walkforward.cpp + regime_split.cpp + spread_stress.cpp + engine_init.hpp (PullbackD1 disable)
**S57 (`fa71c915`):** engine_init.hpp (4 disables) + xau_tf_rigour_audit.cpp
**S58 (`e7175f68`):** tick_gold.hpp (VPS build fix `now_ms_g` -> `_pg_now_ms()`)

---

## Boot log paste request

Next session ask operator to run on VPS:

```powershell
Get-Content C:\Omega\logs\latest.log -Tail 200 | Select-String "OMEGA-INIT|SEED|PortfolioGuard|ENTRY|cap="
```

Should show:
- 1 `[OMEGA-INIT] PortfolioGuard: cap=2` line
- 6 `[SEED]` lines for surviving engines
- No `ENTRY` lines for first 4-24h (D1 engines slow)
