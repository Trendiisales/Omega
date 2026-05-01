# Omega Session Handoff — 2026-05-01

**Open this with the next chat session.** Single document; everything
needed to pick up cleanly is here.

---

## State at end of session

### Production (origin/main, deployed on VPS)

| Commit | Description |
|---|---|
| `044a09a9` | cell-refactor-phase-2: CellEngine<Strategy> + TsmomStrategy + g_tsmom_v2 shadow |
| `1e0d50f0` | ops: bulletproof cmake discovery in QUICK_RESTART.ps1 (glob paths + fail-loud) |
| `68e2a07e` | S52: HybridGold trail fix (MIN_TRAIL_ARM_PTS 1.5->3.0, MFE_TRAIL_FRAC 0.20->0.40) |
| `<this commit>` | S52b: disable g_rsi_reversal + g_rsi_extreme (negative-EV); session handoff |

### Phase 2a live shadow window — IN FLIGHT
Started 2026-05-01 ~22:55 UTC at commit `1e0d50f0`. Runs 5 trading days
minimum at `max_pos_per_cell=10` (matches V1 behaviour exactly under the
portfolio-level `max_concurrent=5` cap). Day-5 ledger diff command:

```powershell
$LEDGER = "C:\Omega\trade_ledger.csv"   # <-- adjust to actual path on VPS
Import-Csv $LEDGER | Where-Object { $_.engine -like "Tsmom_*" -and $_.engine -notlike "Tsmom_v2_*" } |
    Sort-Object @{e='entryTs'}, @{e='id'} | Export-Csv -NoTypeInformation v1.csv
Import-Csv $LEDGER | Where-Object { $_.engine -like "Tsmom_v2_*" } | ForEach-Object {
    $_.engine = $_.engine -replace 'Tsmom_v2_', 'Tsmom_'; $_
} | Sort-Object @{e='entryTs'}, @{e='id'} | Export-Csv -NoTypeInformation v2.csv
fc.exe v1.csv v2.csv
```

Pass = `FC: no differences encountered` (or only `id` column differs —
V1/V2 increment trade ids independently, expected).

### HybridGold S52 A/B — IN FLIGHT (Mac side, user running)
Script in earlier message. Runs 26-month Dukascopy backtest twice (pre-S52
at commit `1e0d50f0`, post-S52 at current HEAD) and compares. Last
intermediate output (baseline run, ~55M of ~80M ticks):

```
55000000 ticks, 12 trades, $6.28 (2599s elapsed)
```

Trade count plateauing around 12, PnL bouncing $6-$15 — final result
expected once both runs complete (~10-15 min remaining each). User to
report headline numbers when done.

### Disabled engines (this commit)

- `g_rsi_reversal.enabled = false`
  - Evidence: source comment `// Real-tick backtest: 4320 trades / 2yr,
    -$3.8k. Momentum = negative EV.` (engine_init.hpp ~line 219, post-
    move: ~line 227). Robust sample (4320 trades), unambiguous verdict.
- `g_rsi_extreme.enabled = false`
  - Evidence: 153-combo parameter sweep run this session on
    `data/l2_ticks_2026-04-16.csv`. 0/153 combos profitable. Best
    -$4.30, worst -$225, mean -$81. Original 12-trade / 2-day backtest
    that justified shipping was a lucky window.
- Files NOT deleted; both engines remain in tree, just inert. Reactivate
  by flipping `enabled = true` if a future audit reverses the verdict.

### VPS state

- Service `Omega` running, NSSM-wrapped, binary hash matches origin/main.
- `QUICK_RESTART.ps1` now uses `cmake-discover.ps1` (glob-based discovery
  + fail-loud guard). Resilient against vcpkg moves and cmake version
  bumps. Fix is committed as `1e0d50f0`.
- VPS pull + restart needed after this commit lands to pick up the RSI
  disables.

### Mac state

- Branch `fix/trade-quality-2026-05-01` was the Phase 2 work branch;
  merged + pushed via origin/main path. Working tree may have residual
  modified files; no action needed.
- Tag `pre-omega-terminal` placed before any GUI work — recovery point.
- Backup at `archive/gui_pre_terminal_<DATE>/` per the user's run.

---

## Decisions locked

1. **CellEngine refactor §7 decisions** all locked 2026-04-30 — see
   `docs/CELL_ENGINE_REFACTOR_PLAN.md`. Phase 1 + Phase 2 shipped.
   Phase 2a is byte-identical-ledger validation (live, 5 days).
   Phase 2b will be the multi-position policy rollout; Phase 3 ports
   Donchian / EmaPullback / TrendRider; Phase 4 deprecates V1.
2. **HybridGold trail tuning S52** — `MIN_TRAIL_ARM_PTS = 3.0` and new
   `MFE_TRAIL_FRAC = 0.40`. Awaiting Dukascopy A/B verdict.
3. **g_rsi_reversal + g_rsi_extreme** — disabled, file kept for reference.
4. **Omega Terminal** — full rewrite of GUI as React/Vite SPA, single
   source (no parallel/patched GUI), full BB-Terminal market data
   surface + Omega-native engine/position/ledger functions. Build plan
   below. Two mockups designed and approved (see
   `docs/omega_terminal/CC_mockup.html` and `TRADE_mockup.html`).

---

## Omega Terminal — Build Plan (next focus)

**Architecture.** One React/Vite SPA, two backends:

- `/api/v1/omega/*` → Omega.exe :7779 (engine state, positions, ledger)
- `/api/v1/market/*` → openbb-api :6900 (Yahoo, FRED, SEC, ...)

**Branch rules.** Build on `omega-terminal` branch; rebase regularly on
`main`; main keeps current C++ GUI live throughout. Cutover is one
single commit: merge → delete src/gui/ → rename omega-terminal/ → src/gui-react/
→ update CMakeLists.txt + QUICK_RESTART.ps1 → push → smoke-test. If
smoke fails: `git revert HEAD`, old GUI back in <5 minutes; the
`pre-omega-terminal` tag is the second safety net.

**Steps (~7-10 sessions total):**

| Step | Scope | Sessions |
|---|---|---:|
| 1 | React/Vite shell + amber-on-black Tailwind theme + command bar with autocomplete + WorkspaceTabs + function-code router. Empty function panels with placeholders. | 1-2 |
| 2 | Omega C++ JSON endpoints: `/api/v1/omega/engines`, `/positions`, `/ledger`, `/equity`. New `g_engines.register(...)` table in globals.hpp so engines self-report status to the GUI. | 1-2 |
| 3 | First three Omega functions wired to live data: CC, ENG, POS. | 1 |
| 4 | LDG (trade ledger with filtering) + TRADE drill-down + PnL equity curve + CELL grid (per-cell drill for Tsmom/Donchian/Epb/TrendRider). | 1 |
| 5 | OpenBB Python wrapped as second NSSM service on :6900. Vite proxy to it in dev. Port BB's INTEL + CURV + WEI + MOV first (highest leverage for WATCH workflow). | 1-2 |
| 6 | Remaining BB functions: OMON, FA, KEY, DVD, EE, NI, GP, QR, HP, DES, FXC, CRYPTO, HELP. Plus WATCH (runs INTEL rules nightly across S&P 500 + NDX components, exposes flagged candidates). | 1-2 |
| 7 | Cutover: merge omega-terminal → main as one commit, delete `src/gui/OmegaTelemetryServer.{hpp,cpp}`, rename `omega-terminal/` → `src/gui-react/`, update CMakeLists.txt + QUICK_RESTART.ps1. Deploy. Smoke-test. | 1 |

**Reserved tree at end of this session:**

```
omega_repo/
├── docs/
│   ├── SESSION_2026-05-01_HANDOFF.md  (this file)
│   ├── CELL_ENGINE_REFACTOR_PLAN.md   (existing)
│   └── omega_terminal/
│       ├── CC_mockup.html             (Command Center reference)
│       └── TRADE_mockup.html          (TRADE drill-down reference)
├── src/gui/                           (current C++ GUI — UNTOUCHED)
└── (omega-terminal/ branch will hold the new SPA)
```

**Next-session opener prompt:**

> Read `docs/SESSION_2026-05-01_HANDOFF.md` to catch up. Then start
> Omega Terminal step 1: React/Vite shell + amber-on-black Tailwind
> theme + command bar with autocomplete + WorkspaceTabs + function-code
> router + empty placeholder panels. Mockups for visual reference are at
> `docs/omega_terminal/CC_mockup.html` and `TRADE_mockup.html`. Branch
> name: `omega-terminal`. Do not modify `main` or `src/gui/`.

---

## Pending action items (carried forward)

1. [ ] **VPS deploy this commit** — `git pull origin main && .\QUICK_RESTART.ps1`
       to pick up the RSI disables.
2. [ ] **HBG A/B finishes** — user reports headline numbers from Mac
       backtest. If S52 net PnL ≥ baseline → keep. Else → revert with
       `git revert 68e2a07e`.
3. [ ] **Phase 2a 5-day window** — daily `fc.exe` ledger comparison.
       Day 5 is ~2026-05-06. Pass → proceed to Phase 2b. Fail → halt
       and investigate refactor bug.
4. [ ] **Omega Terminal build** — Step 1 starts in next chat session per
       opener prompt above.

---

## Things to NOT forget

- The Omega Terminal build is **single-source**: never patch the old
  C++ GUI to add new fields once `omega-terminal` branch opens. All new
  GUI work flows into the new tree only.
- Phase 2a validation diffs ledger CSVs **modulo trade-id** — V1 and V2
  increment trade ids independently, that's expected and not a fail.
- `kShadowDefault = (g_cfg.mode != "LIVE")`. Flipping `omega_config.ini`
  to `mode=LIVE` enables LIVE mode for all engines whose `shadow_mode`
  is tied to it. Currently only MCE and RSIReversal have hardcoded
  `shadow_mode = true`. Audit before flipping live.
- The Dukascopy harness `backtest/hbg_duka_bt.cpp` requires
  `-DOMEGA_BACKTEST` to compile (gates the `g_macroDetector` reference).
