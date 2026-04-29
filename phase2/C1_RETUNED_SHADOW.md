# C1_retuned Live Shadow — Operations

**Status:** SWITCHED ON 2026-04-29.
**Verdict source:** [`phase2/donchian_postregime/CHOSEN.md`](donchian_postregime/CHOSEN.md)
**Walk-forward source:** [`phase2/optionD/walkforward_C_report.txt`](optionD/walkforward_C_report.txt)

---

## What's running

C1_retuned is a four-cell long-only XAUUSD portfolio:

| cell                         | timeframe | rule                                          |
|------------------------------|-----------|-----------------------------------------------|
| `donchian_H1_long_retuned`   | H1        | period=20 breakout, sl=3.0 ATR, tp=5.0 ATR    |
| `bollinger_H2_long`          | H2        | canonical Bollinger long, exit on midline     |
| `bollinger_H4_long`          | H4        | canonical Bollinger long, exit on midline     |
| `bollinger_H6_long`          | H6        | canonical Bollinger long, exit on midline     |

Portfolio rules: 0.5% risk per trade, max 4 concurrent positions,
$10,000 starting equity, $1,000 margin call, no pyramiding.

Backtest baseline (full corpus 2024-03..2026-04):
return +74.12%, max DD -5.85%, PF 1.486, Sharpe 2.651, WR 55.2%.
Walk-forward TRAIN/VALIDATE/TEST all PASS; post-regime PF 1.334 → 1.630.

---

## Files

```
phase2/
  c1_retuned_shadow.py          live shadow runner (idempotent)
  c1_retuned_halt_monitor.py    deeper halt-criteria audit (read-only)
  C1_RETUNED_SHADOW.md          this file
  portfolio_C1_C2.py            canonical simulator (DO NOT MODIFY)
  donchian_postregime/CHOSEN.md the verdict that locked C1_retuned
  live_shadow/                  runtime outputs (created on first run)
    c1_retuned_shadow.csv         cumulative trade ledger
    c1_retuned_equity.csv         cumulative equity curve
    c1_retuned_summary_<UTC>.md   per-run summary (one per invocation)
    halt_report_<UTC>.md          per-monitor-invocation halt report
    last_run.json                 metadata + history pointer
    HALT.flag                     present iff a halt has been tripped
```

The `live_shadow/*.csv` outputs are gitignored (root `.gitignore` ignores
`*.csv`). Track results via the markdown summaries in `live_shadow/` if
you want a git history of shadow performance — copy the desired
summary out of `live_shadow/` to a tracked location (e.g.,
`incidents/` or a dedicated `phase2/live_shadow_audits/` folder you
create).

---

## How to switch on

Bar-refresh prerequisites:

1. The shadow runner reads from `phase1/trades_net/*.parquet`. Those
   ledgers are derived from `phase0/bars_<TF>_final.parquet`, which are
   built from the Dukascopy tick CSV.
2. New shadow trades only appear when bars are refreshed. The bar
   refresh chain is:
   - `download_dukascopy.py` → tick CSV
   - tick reindex (`phase1/tick_index.parquet` rebuild)
   - bar build (M15/H1/H2/H4/H6/D1 final parquets)
   - signal generation (`phase1/signals/*.parquet`)
   - per-cell ledger build (`phase1/build_*` scripts incl.
     `phase1/build_donchian_H1_long_retuned.py`)
3. Once those parquets are current, run:

```bash
cd /path/to/omega_repo
python3 phase2/c1_retuned_shadow.py
```

First invocation creates `phase2/live_shadow/` and uses today
00:00 UTC as `shadow_start`. To pin a specific start date, pass it
explicitly **once**; subsequent runs reuse it from `last_run.json`:

```bash
python3 phase2/c1_retuned_shadow.py --shadow-start 2026-04-29T00:00:00Z
```

---

## Cron / launchd recipe

Run hourly during XAUUSD-active sessions on the same host that does the
bar refresh. Example crontab entry (UTC):

```cron
# Refresh + shadow at minute 5 of each hour, Sun-Fri
5 * * * 0-5 /path/to/refresh_bars.sh && \
            cd /path/to/omega_repo && \
            /usr/bin/python3 phase2/c1_retuned_shadow.py \
              >> phase2/live_shadow/cron.log 2>&1

# Halt audit twice daily
0 0,12 * * * cd /path/to/omega_repo && \
             /usr/bin/python3 phase2/c1_retuned_halt_monitor.py \
               --exit-on-breach \
               || mail -s "C1_retuned HALT BREACH" you@example.com \
                  < phase2/live_shadow/halt_report_*.md | tail -1
```

`refresh_bars.sh` is your existing (or to-be-written) bar-refresh
glue — out of scope for this module. The shadow runner is intentionally
decoupled from data-fetch so it can be tested against static parquets.

---

## Halt criteria

Per `CHOSEN.md`:

> **Halt criteria during shadow:** cluster days stacking >2× expected
> frequency in first 2 weeks → pause and re-evaluate.

This is implemented in two places:

1. The runner does a coarse check each invocation (cluster ratio + DD).
   On breach it writes `live_shadow/HALT.flag` and exits with code 3.
2. The monitor does a deeper audit (cell concentration, daily
   distribution, silent cells, full cluster table) and emits a
   timestamped report.

Numeric thresholds (locked in code):

| check                | threshold                                          |
|----------------------|----------------------------------------------------|
| max DD               | <= -7.5% (1.5x backtest -5.85%)                    |
| cluster days         | >= 4 cells losing same UTC session                 |
| cluster rate breach  | observed/expected >= 2.0x (expected ~0.23/month)   |
| cell concentration   | one cell <= -50% of total P&L                      |
| silent cell          | no fire in >= 21 days                              |

---

## Operational notes

- **Idempotent.** Re-running with no new bars is a no-op. The summary
  file is still emitted but contains "_No new trades since previous run._"
- **No core-code modification.** The runner imports
  `phase2/portfolio_C1_C2.py` as a library and reuses `simulate()`
  unchanged. Sizing and fill behaviour stay identical to backtest.
- **Reset.** `python3 phase2/c1_retuned_shadow.py --reset` clears
  `live_shadow/*.csv`, `last_run.json`, and `HALT.flag` and starts a
  fresh shadow from today.
- **Dry run.** `--dry-run` prints status without writing.
- **The 2026-03-18 cluster post-mortem is still pending** (see
  `CHOSEN.md` checklist). Do that work in parallel with the shadow run.
- **Independent of the C++ Omega runtime.** This shadow does NOT use
  `omega_config.ini`, the cTrader/BlackBull FIX feed, or the C++ engine
  binaries. It's a pure parquet-driven Python sim of the locked
  C1_retuned portfolio.

---

## Promotion criteria (live capital)

Per the handoff: 4–8 weeks of shadow with halt criteria intact. After
that window, if all of the following are true:

- HALT.flag has never been written (or was cleared and didn't recur);
- max DD has stayed inside -7.5%;
- cluster rate has stayed below 2x expected;
- cell concentration is within bounds;
- the 2026-03-18 cluster post-mortem has produced an explanation and
  (if needed) a guard rule;

then C1_retuned can be promoted from shadow to live capital. The
promotion itself is a separate decision and config change — explicitly
out of scope for this shadow module.
