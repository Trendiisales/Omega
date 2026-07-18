# SESSION HANDOFF — 2026-07-18ad (overnight restart-loop incident + watch hardening; OPERATOR DEMANDS BETTER CHECK)

Caveman mode. Predecessor: outputs/SESSION_HANDOFF_2026-07-18ac.md (copied into repo this session).
Operator FURIOUS (4th-5th "assured working, wasn't" recurrence). Last words: "handoff ... and 15 minutes
i want a better check". FIRST DELIVERABLE of next session = the better check, below, before anything else.

## INCIDENT FOUND THIS SESSION (root cause of "not 1 crypto fired")

- **93 chimera restarts 21:11Z Jul17 → 05:39Z Jul18** on josgp1 (journalctl). Engine was
  down/bouncing through the ENTIRE overnight pump (MANA +2.6%/1h W1, +5.5% W8; ADA/UNI/INJ
  +2.5–4%; BTC +1.25% W2 — all over cell thresholds). 0/144 books fired. Cells NOT broken:
  every bounce zeroes det windows; warm-seed catch-up BY DESIGN skips windows whose confirm
  crossed during outage. `[SHUTDOWN] aggregate trades=0` every run.
- **05:11–05:16Z: 60-crash FATAL loop**: `FATAL mode conflict: mode=SHADOW but
  binance_credentials.json shadow_mode=false. Refusing to start.` live_config.json flipped to
  SHADOW 02:26Z by SOME OTHER overnight non-interactive ssh session (bash_history shows
  restart/config one-liners, no attribution; no session active now), creds reverted 03:35Z,
  live_config fixed 05:16Z.
- **Existing watch chimera_executor_watch.sh DID banner 02:40Z** (RUNTIME MODE = SHADOW) but was
  GREEN through the crash-loop: `systemctl is-active` reads active while systemd flaps; grep of
  last RUNTIME MODE line is STALE from previous good boot.
- **NOW: HEALTHY-VERIFIED**: RUNTIME MODE = LIVE, 41-sym pilot $100/$500, 144/144 floored,
  build=01a30d2 == repo HEAD, stable since 05:39:59Z.

## SHIPPED (Omega repo, pushed)

1. `6a059570` (S-18ad): chimera_executor_watch + RESTART-LOOP check (>3 systemd starts/30min,
   `sudo -n journalctl`, checked FIRST — everything else lies in a loop) + CONFIG MODE-CONFLICT
   check (live_config shadow_mode/mode vs creds shadow_mode at FILE level — alarms before next
   restart FATALs). Live-run GREEN.
2. `02dec677` (S-18ae): + BUILD-MISMATCH check — running binary's boot `build=` stamp must
   prefix-match josgp1 repo HEAD. Stale-binary deploy now banners ≤20min. Live GREEN
   (01a30d2==01a30d2); negative path tested.
3. Vaults: Memory-Omega ChimeraExecutorWatch.md updated (+index +log 18.00); Memory-Chimera
   log.md incident entry. Handoff-ac copied to outputs/.

## DEMANDED NEXT — "BETTER CHECK" (operator ultimatum, do FIRST)

Operator wants to SEE IMMEDIATELY that correct software is loaded — banners not enough (missed
the 02:40Z one). Build a GUI-visible, always-on truth chip:

- **Desk header chip on omega-new GUI**: `CC <build> LIVE up=<h>m ✓` green / RED text on ANY of:
  build != josgp1 HEAD, mode != LIVE, restarts>3/30min, config-conflict, service down, state
  relay stale. Data path: extend the josgp1 → omega-new 120s companion relay payload (or a tiny
  side file) with {build, headsha, mode, uptime_s, starts30, config_ok, ts}; GUI reads it like
  __compBooks. **GUI edit flow trap: tools/gui/omega_desk.html → gen_index_html.py → gui_drift
  gate; NEVER edit OmegaIndexHtml.hpp directly.** No-backtest-in-live-GUI rule irrelevant here
  (health chip, not PnL).
- Watch cadence 10min → **1min** (ibkr_login_watch precedent 18z: cron every 1min, 3-strike)
  via tools/install_chimera_executor_watch_cron.sh (marker OMEGA-CHIMERA-EXECUTOR-WATCH,
  feedback-crontab-edit-via-script).
- Consider same chip for Omega side (build hash already in stderr log; header has TRADING beacon
  precedent e23713fb).

## TRAPS

- ssh forms: literal `ssh omega-new` (Omega VPS), `ssh chimera-direct` (josgp1). chimera.log is
  root-owned dir but file world-readable; journalctl needs `sudo -n` (passwordless OK).
- NEVER git reset on josgp1 (live uncommitted live_config). Protected branch = no force-push.
- Desk API: http://45.85.3.79:7779/api/crypto_companion = {ts, legs:[...]} (not bare list).
- Naming decode (operator asked): UJ05/10/15/20=0.5/1.0/1.5/2.0% det thr; W<N>=window hours;
  legacy 2.0% quad: UJ2-BECASC=W4, UJ20-F=W2, UJ20-S=W12, UJ2W8-MIM=W8. Rename offered, needs
  state-key migration — not approved yet.
- Speed rule: when operator asks "why no fires", curl the desk API + check journalctl restarts
  FIRST (seconds), answer, then investigate.
- Protection selftest [1] weekend-skips companion freshness — wrong for 24/7 crypto; unfixed gap.
- Memory-Omega N commits BEHIND per hook — auto-ingest per feedback-vault-backfill-auto-ingest.

## OPEN (carried from ac)

- WINDOW-state chip on mimic panel (det-window-open vs idle) — pairs naturally with health chip.
- Forward-watch 16 BTC books; first confirms ring entryBell ≤2min.
- make_becascade_cell rt=28 per-coin measured-cost note owed.
- Lower-thr quads other coins (operator may ask); judge standalone.
