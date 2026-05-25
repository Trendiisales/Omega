# Omega Bracket Bot — Session Handoff

**Date:** 2026-05-25
**Repo:** https://github.com/Trendiisales/Omega — `bracket-bot/` subfolder
**Status:** Deployed to the Windows VPS and verified. One fix queued — see "NEXT FIX".

---

## What this is

`bracket-bot/` is the Python execution component of Omega — it runs the live
gold-bracket trading strategies. It is intentionally Python, not C++ (rationale
is in `bracket-bot/README.md`). The C++ tick-processing engine in the rest of the
repo is a separate codebase.

Strategies (OCA bracket orders on gold):
- `live/daily_bracket.py` — fires at a scheduled UTC hour (13:00 / 14:00 configs).
- `live/sunday_bracket.py` — fires at the Sunday CME globex reopen.

---

## Done this session

The bot was previously developed in a standalone Mac folder `~/omega_repo` that
was not under version control and not connected to GitHub. This session:

- Committed the bot into `Trendiisales/Omega` under `bracket-bot/`.
- Added a Windows VPS deployment package (`bracket-bot/deploy/`).
- Added `bracket-bot/README.md` marking it as a deliberate Python component.
- Deployed to the Windows VPS and verified end-to-end.

Relevant commits on `main`: `c26a521` (bot), `b70e533` (deploy package),
`ced33fa` (README), `5757ab6` + `280908a` (deploy-script hardening).

---

## Deployed state (Windows VPS)

- Repo cloned at `C:\Omega`; bot at `C:\Omega\bracket-bot`.
- venv at `C:\Omega\bracket-bot\.venv` with `ib_insync==0.9.86` (+ eventkit,
  nest_asyncio, numpy). `flask` is NOT installed — see "Open items".
- VPS clock set to UTC.
- IB Gateway running on the VPS, paper port 4002, API enabled.
- Three Windows Task Scheduler jobs registered, all `--paper`:
  - `Omega Daily Bracket 1300` — Mon–Fri 13:00 UTC
  - `Omega Daily Bracket 1400` — Mon–Fri 14:00 UTC
  - `Omega Sunday Bracket` — Sunday 22:55 UTC
- Smoke test passed: `daily_bracket.py --paper --dry-run` connected to IB
  Gateway, resolved the MGC Jun-2026 contract, fetched a gold price, exited clean.

Full deploy steps: `bracket-bot/deploy/DEPLOY-WINDOWS.md`.

---

## NEXT FIX — switch from delayed to real-time market data

Priority task for the next session.

**Problem.** The bot prices off delayed market data:
- `live/daily_bracket.py` → `get_price()` calls `ib.reqMarketDataType(4)`
  (4 = delayed-frozen).
- `live/sunday_bracket.py` → `get_current_price()` calls `ib.reqTickers()` with
  no market-data type set.

The bracket is built around a reference "open" price, so a ~10–15 min delay
misaligns live behaviour against the backtest the configs were validated on.

**Goal.** Price off real-time data. Real-time data IS available on this account —
it is already used by the Omega C++ engine's L2 / depth-of-market feed
("we have this via L2").

**Option A — use IBKR real-time directly (minimal change, recommended first).**
- In `daily_bracket.py`, change `reqMarketDataType(4)` → `reqMarketDataType(1)`
  (1 = real-time / live).
- In `sunday_bracket.py`, add `ib.reqMarketDataType(1)` before `reqTickers()`.
- Keep a graceful fallback: try type 1, fall back to 2 (frozen) / 3 / 4 if the
  account is not subscribed for that contract, so a missing subscription never
  hard-fails a scheduled run.
- Verify the IBKR account has real-time COMEX / MGC market-data permissions.

**Option B — read the reference price from the Omega L2 bridge.**
- The C++ engine maintains an L2/depth feed and writes an "Omega L2 CSV" (see
  C++ repo history: "wire IBKR L2 to GUI depth panel + Omega L2 CSV", "add IBKR
  DOM bridge"). The bracket bot could read its reference price from that feed
  instead of making its own IBKR market-data request.
- Pro: one shared real-time source with the engine. Con: needs the CSV path +
  format and a staleness/freshness check.

**Before implementing, confirm:**
1. IBKR account real-time data permissions for MGC / COMEX.
2. If choosing Option B: the path and format of the Omega L2 CSV.
3. Both scripts need the change — `daily_bracket.py` AND `sunday_bracket.py`.
4. Re-run `--dry-run` afterward and confirm the printed price matches a live
   quote, not a stale one.

**Constraint.** User rule: "never modify core code unless clearly instructed."
This fix is authorised for the pricing path only — do NOT change strategy
parameters (offsets, TP/SL distances, hold time).

---

## Open items (not blocking)

- **Dashboard not deployed.** `server.py` needs `flask`. `flask` has a
  console-script entry point; generating its launcher is expected to hit the
  same `ValueError: Unable to find resource t64.exe` that broke the earlier
  `pip install --upgrade pip` on this VPS — pip cannot create console-script
  `.exe` launchers, most likely Windows Defender quarantining pip's launcher
  stubs. `ib_insync` installed fine because it has no console scripts. Fix:
  add a Defender exclusion for the venv and retry, or run the dashboard on the
  Mac. Do NOT run `pip install --upgrade pip` on the VPS — it corrupts the venv.
- **`sunday_bracket.py` hard-coded expiry.** `get_contract()` uses
  `Future('MGC', expiry or '20260618', 'COMEX')`. `daily_bracket.py` uses
  `ContFuture` (auto front-month, resolved to 20260626 in testing). Align the
  Sunday bot to `ContFuture`, or verify the expiry before the next Sunday run.
- **`datetime.utcnow()` deprecation.** Both scripts use the deprecated
  `datetime.utcnow()`; harmless on Python 3.12, modernise to
  `datetime.now(datetime.UTC)` eventually.
- **Mac cron.** Remove any `cron_sunday_bracket.sh` line from the Mac crontab
  (`crontab -l` / `crontab -e`) so the Sunday bracket does not double-run.
- **Stray `~/omega_repo` on the Mac.** Original dev folder; no longer the
  canonical home. Has a broken half-initialised `.git`. Recommend
  `rm -rf ~/omega_repo/.git` and, going forward, edit the bot inside a clone of
  `Trendiisales/Omega` so changes push normally.
- **GitHub token.** The token in the global CLAUDE.md is dead; a fresh one was
  used this session and should be rotated and CLAUDE.md updated.
- **Paper vs live.** Everything is `--paper`. Do not switch tasks to `--live`
  until the real-time-data fix is in and the strategy is validated on paper.

---

## Key commands

Update the VPS to the latest code:

    cd C:\Omega
    git pull origin main

Re-run the smoke test (places no orders):

    cd C:\Omega\bracket-bot
    & ".\.venv\Scripts\python.exe" live\daily_bracket.py --paper --dry-run

Re-register scheduled tasks (Administrator PowerShell):

    cd C:\Omega\bracket-bot
    powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1

---

## Suggested skills for the next session

- `mp-skills:diagnose` — if the real-time-data switch misbehaves (wrong price,
  subscription errors, stale ticks).
- `mp-skills:review` — to validate the pricing change before anything goes
  near `--live`.
