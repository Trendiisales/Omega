# Omega Bracket Bot — Session Handoff

**Date:** 2026-05-27
**Repo:** https://github.com/Trendiisales/Omega — `bracket-bot/` subfolder
**Status:** Live-execution hardened. Real-time data path landed (L2 mid +
mdt cascade) and now wrapped in pre-flight gates, staleness guard,
order-state persistence, kill-switch, heartbeat, and failure webhook.

---

## What this is

`bracket-bot/` is the Python execution component of Omega — it runs the live
gold-bracket trading strategies. It is intentionally Python, not C++ (rationale
in `bracket-bot/README.md`). The C++ tick-processing engine in the rest of the
repo is a separate codebase.

Strategies (OCA bracket orders on gold):
- `live/daily_bracket.py` — fires at a scheduled UTC hour (13:00 / 14:00 configs).
- `live/sunday_bracket.py` — fires at the Sunday CME globex reopen.

---

## Done this session (2026-05-27)

Hardened the live-execution path. Everything below sits at the top of both
bracket scripts (via `live/_common.py`) so they share identical gates and
identical failure semantics.

### Real-time market data
- L2 best-bid/ask midpoint first (existing IBKR L2 sub).
- Top-of-book mdt cascade `(1, 2, 3, 4)` w/ historical 1m fallback.
- Staleness guard rejects live/frozen ticks whose `tk.time` is more than
  30 s behind wall-clock — catches IBKR's silent downgrade-to-delayed when
  the account lacks a real-time sub.
- `log_mdt_status()` logs the type actually requested at each step.

### Pre-flight gates (both scripts)
- `HALT` kill-switch — `bracket-bot/HALT` exists → exit cleanly w/ webhook.
- NTP drift check vs `pool.ntp.org` (warn-only, non-fatal).
- Weekday + CME holiday calendar (2026 + 2027). Sunday bracket exempts
  weekend check.
- `verify_account_mode()` — `--live` only against ports `{4001, 7496}`,
  `--paper` only against `{4002, 7497}`. Refuses mismatch.
- `assert_account_label()` — cross-checks `ib.managedAccounts()` (paper
  = `DU…`, live = `U…`). Catches Gateway logged into the wrong account
  on the right port.
- `assert_live_allowed()` — `--live` blocked unless `BRACKET_GO_LIVE=1`
  AND a recent (< 30 days) paper trade exists in `data/trades.ndjson`.

### Order-state persistence
- `data/state/<STRATEGY>.json` written at `placing_brackets`,
  `awaiting_trigger`, `in_position`. Cleared on clean exit.
- `scripts/recover_state.py` — reads stale state files, lists open
  orders via `reqAllOpenOrders` (clientId 0), flags the P0 case
  (`in_position` with no child exits), optional `--cancel-orphans`.

### Heartbeat + failure notification
- `logs/heartbeat.ndjson` — one record per stage transition
  (`start`, `placed`, `no_trigger`, `in_position`, `done`, `no_price`,
  `dry_run_ok`).
- `scripts/run_with_heartbeat.ps1` wraps every scheduled task. Captures
  stdout/stderr to `logs/scheduled_<task>_<utc>.log[.err]`, writes
  wrapper start/end heartbeats, posts to `BRACKET_WEBHOOK_URL` on
  non-zero exit.
- `notify_failure()` from inside the bracket script also posts to the
  same webhook on connect / contract / no-price / mode-mismatch failures.
- `register_tasks.ps1` updated — all three tasks now run through the
  wrapper (must be re-registered on the VPS to pick this up).

### Deploy hygiene
- `requirements.lock` — fully pinned transitive closure
  (blinker, click, eventkit, Flask 3.1.3, ib-insync 0.9.86,
  itsdangerous, Jinja2, MarkupSafe, nest-asyncio, numpy, Werkzeug).
  Install w/ `pip install --no-deps -r requirements.lock`.
- `deploy/DEFENDER-EXCLUSION.md` — exact steps to whitelist the venv
  so `flask` (and any other console-script package) installs.

### Unification
- `sunday_bracket.get_contract()` now takes `ib` and qualifies inside,
  matching `daily_bracket.get_contract()`. Both share the same path:
  explicit `--expiry` overrides; otherwise `ContFuture` (auto front-month).
- Both scripts source `utcnow`, `LOG_DIR`, `DATA_DIR` from `_common`.

---

## Deploying these changes to the VPS

    # On the VPS, in C:\Omega:
    git pull origin main

    # Install any new pins (no-op if already on lock):
    cd C:\Omega\bracket-bot
    .\.venv\Scripts\python.exe -m pip install --no-deps -r requirements.lock

    # Re-register the scheduled tasks so they go through the wrapper:
    powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1

    # Optional: configure the webhook for failure alerts (Slack-compatible).
    setx BRACKET_WEBHOOK_URL "https://hooks.slack.com/services/..."

    # Smoke test (places no orders):
    .\.venv\Scripts\python.exe -m live.daily_bracket --paper --dry-run --strategy SMOKE

---

## Open items

These still belong to the operator, not the bot:

- **Defender exclusion on the venv.** Follow
  `deploy/DEFENDER-EXCLUSION.md`, then install Flask so the dashboard
  works.
- **IBKR market-data subscriptions.** Confirm real-time MGC / COMEX
  permissions on the IBKR account. Without them the staleness guard
  will reject every live tick and the bot will fall through to delayed.
  Verify via account portal or by reading the first `[INFO] price
  [live]: …` line in a real run.
- **Mac cron.** Remove `cron_sunday_bracket.sh` from the Mac crontab
  (`crontab -e`) if still present. The VPS is now authoritative.
- **Stray `~/omega_repo` on the Mac.** Delete the half-initialised
  `.git` there: `rm -rf ~/omega_repo/.git`. Edit the bot inside a
  clone of `Trendiisales/Omega` from now on.
- **GitHub token.** The token in global CLAUDE.md is dead; the fresh
  one used last session should be rotated and CLAUDE.md updated.
- **CME holiday list.** Hard-coded for 2026 + 2027 in `live/_common.py`.
  Refresh annually.
- **Going live.** Don't flip `--paper` → `--live` until:
    1. `BRACKET_GO_LIVE=1` set on the VPS (`setx`).
    2. Recent paper trade in `data/trades.ndjson`.
    3. `scripts\live_guard.py` exits 0.
    4. Real-time subs confirmed (see above).

---

## Key commands

Smoke test (places no orders):

    cd C:\Omega\bracket-bot
    & ".\.venv\Scripts\python.exe" -m live.daily_bracket --paper --dry-run --strategy SMOKE

Crash recovery (after any unscheduled bot death):

    & ".\.venv\Scripts\python.exe" scripts\recover_state.py
    & ".\.venv\Scripts\python.exe" scripts\recover_state.py --cancel-orphans

Halt all scheduled runs without unregistering tasks:

    New-Item C:\Omega\bracket-bot\HALT
    # …re-enable with…
    Remove-Item C:\Omega\bracket-bot\HALT

Tail the heartbeat:

    Get-Content C:\Omega\bracket-bot\logs\heartbeat.ndjson -Tail 20

Re-register scheduled tasks (Administrator PowerShell):

    cd C:\Omega\bracket-bot
    powershell -ExecutionPolicy Bypass -File deploy\register_tasks.ps1

---

## Suggested skills for the next session

- `mp-skills:diagnose` — if the staleness guard rejects every live
  tick on first VPS deploy (likely cause: missing real-time sub).
- `mp-skills:review` — to audit any future live-path change before
  it goes near `--live`.
