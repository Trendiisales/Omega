# S12 Side-Issue Investigations

_Drafted: 2026-05-07. No code changes applied — investigations only, per "never modify core code unless instructed clearly". Each section ends with a recommended action you can authorise next session._

---

## 3a — `[OMEGA] Shutdown` after broker FIX disconnect (00:51:28 UTC pre-deploy)

**Original hypothesis (from S12 brief):** "handler exits process instead of supervised reconnect; OmegaWatchdog wasn't running."

**What the code actually does.** `[OMEGA] Shutdown` is printed at `include/omega_main.hpp:1035`, fired only after `quote_loop()` returns. `quote_loop()` (entry at `include/quote_loop.hpp:5`) is an infinite outer loop — `while (g_running.load())` — that retries `connect_ssl()` with exponential backoff on every disconnect. There is no FIX-disconnect path that exits the loop. Disconnects only break the inner `while (g_running.load())` at `quote_loop.hpp:40` and fall through to the next outer-loop reconnect attempt.

`g_running.store(false)` has only two writers in the entire source tree:
- `sig_handler` (`include/trade_lifecycle.hpp:494`) — POSIX signal handler
- Windows console-control handler (`include/trade_lifecycle.hpp:508`) — fires for `CTRL_C_EVENT`, `CTRL_BREAK_EVENT`, `CTRL_CLOSE_EVENT`, `CTRL_LOGOFF_EVENT`, `CTRL_SHUTDOWN_EVENT`

So the brief's hypothesis is wrong: there is no broker-disconnect → process-exit code path. The 00:51:28 UTC shutdown was triggered by a Windows control event that *coincided* with the FIX disconnect. Plausible candidates: RDP session disconnect, host sleep/wake, scheduled OS update, manual close.

**OmegaWatchdog.** `OmegaWatchdog.ps1` exists and is healthy: 15s heartbeat, SHA256 stamp verification before restart, crash-loop pause after 3 crashes in 10 min. The user noted it wasn't running — that's an operations issue (autostart not configured), not a code defect. Run `INSTALL_WATCHDOG.ps1` to register it as a startup task.

**Recommended action (non-destructive, code-side):** add a one-line marker in both `g_running.store(false)` writers that prints which event triggered the shutdown. Example:
```cpp
// trade_lifecycle.hpp:494
static void sig_handler(int sig) noexcept {
    std::cerr << "[OMEGA] g_running=false from signal " << sig << "\n";
    g_running.store(false);
}
```
And in the Windows handler, log which `CtrlType` fired (CTRL_CLOSE_EVENT vs CTRL_LOGOFF_EVENT etc.). After this lands, the next surprise shutdown identifies its own cause from `latest.log`.

**Recommended action (operations-side):** ensure `INSTALL_WATCHDOG.ps1` has been run on the VPS so the watchdog auto-starts after the next host reboot.

---

## 3c — Startup-banner content (`[TRENDR]`, `[STARTUP-SELFTEST]`, etc.) not in `latest.log`

**Why.** The tee that wires `std::cout` → file is installed at `include/engine_init.hpp:1856`:
```
g_orig_cout = std::cout.rdbuf();
g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_dir);
...
std::cout.rdbuf(g_tee_buf);
std::cerr.rdbuf(g_tee_buf);
```
This happens inside `init_engines(cfg_path)`. Anything printed *before* `init_engines()` runs in `main()` lands on stdout but never reaches `latest.log`. The `[TRENDR]` and `[STARTUP-SELFTEST]` banners (defined in `include/TrendRiderEngine.hpp` + `include/EngineHeartbeat.hpp` startup self-test) are emitted from setup blocks that run before the tee is installed.

**Recommended action:** move the tee install to the *first* line of `main()` — before any other initialization. Three lines moved:
```cpp
// at top of main(), before any other init
g_orig_cout = std::cout.rdbuf();
g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_root_dir());
if (g_tee_buf->is_open()) {
    std::cout.rdbuf(g_tee_buf);
    std::cerr.rdbuf(g_tee_buf);
}
```
Then drop the tee block from `init_engines`. Single-commit change, no functional risk; if the file open fails the rest of `init_engines` proceeds as before.

**Caveat:** `log_root_dir()` may not be safe to call before config load. Check call dependencies before moving — if it requires `g_cfg`, install the tee just after config parse instead of literally first.

---

## 3d — UNMATCHED security IDs (`UK100.F`, `UKBRENT.F`, `ESTC`)

**Root cause per ID:**

| Broker symbol | Broker ID | Existing config | Mismatch |
|---|---|---|---|
| `UK100.F` | 4461 | `[UK100]` in `config/symbols.ini:182`, alias map has `UK100`/`FTSE`/`FTSE100` (per `SYMBOLS.md:32`) | `.F` suffix not in alias map |
| `UKBRENT.F` | 2634 | `[BRENT]` in symbols.ini, alias map has `BRENT`/`UKBRENT`/`BRENT.F` (per `SYMBOLS.md:36`) | `UKBRENT.F` (.F variant of UKBRENT) not in alias map |
| `ESTC` | 3229 | No symbol entry, no alias | Unrelated equity ticker (Elastic NV) — broker exposes the watchlist ID; we don't trade it |

**Recommended action:** in the symbol-alias load (find via `g_ctrader_depth.name_alias` pattern; same site that maps `STOXX50`/`SX5E`/`EUSTX50` → `ESTX50` at `omega_main.hpp:532-534`), add the two `.F` variants:
```cpp
g_ctrader_depth.name_alias["UK100.F"]     = "UK100";
g_ctrader_depth.name_alias["UKBRENT.F"]   = "BRENT";
```
For `ESTC`, add it to a "known-unknown" suppress list so the `[OMEGA-SECURITY] UNMATCHED` line stops firing without losing visibility on genuinely-new unknowns. Preferred pattern: a small `static const std::set<std::string> g_security_ignore_list = {"ESTC"};` checked before the warn print.

---

## Finding A (carry-forward from S11) — cTrader DOM L2 watchdog dead on `ctid=43014358`

**Where it lives.** Watchdog thread defined at `include/omega_main.hpp:843-972`. Triggers `[L2-WATCHDOG] *** DEAD ***` after 120 s of no `depth_active` event-count growth from `g_ctrader_depth`. Ledger entry at `g_l2_watchdog_dead.store(true)` (line 952) gates downstream L2-using engines.

**Recovery path.** Watchdog is *detection-only* — it does not attempt reconnection. Recovery requires either a process restart or for the cTrader Open API stream to spontaneously resume (token still valid + TCP re-established). The print at line 958 explicitly says "ACTION REQUIRED: restart Omega or check ctid=43014358".

**Plausible upstream causes:**
1. Token expiry — `ctid=43014358`'s OAuth token expired; renewal isn't automated
2. cBot DOM streamer (cTrader-side, runs at `localhost:8765`) dropped its subscription — independent of token
3. TCP idle disconnect on the wire — should auto-reconnect at the SSL layer; if it doesn't, `CTraderDepthClient` is missing a reconnect loop

**Recommended action — short-term:** add a self-recovery loop inside `CTraderDepthClient`. On 60 s of no events, tear down the SSL connection and rebuild. This is the same pattern `quote_loop()` uses for FIX. Lower risk than the alternative of refactoring everything onto FIX-only L2.

**Recommended action — medium-term (your S11 brief's option 2):** refactor to FIX-only L2 across all symbols. Removes the cTrader-Open-API dependency entirely. Larger surgery but eliminates this whole failure class. The S8 FIX-only pattern that works for gold (FIX 264=0) is the model.

---

## Finding B (carry-forward from S11) — Estx50 startup-self-test false alarm

**Why it fires.** The startup self-test at `EngineHeartbeat.hpp::run_startup_self_test()` checks every engine flagged `live_required=true` for at least one heartbeat pulse within ~78 s of init. Estx50 is session-gated (07:00-22:00 UTC). When startup happens outside that window — the failing run was at 00:28 UTC — Estx50 doesn't dispatch any ticks because the session-slot gate blocks them, so it doesn't pulse, so the self-test reports it as silent.

**The bug:** the self-test treats "didn't pulse" as a failure regardless of whether the engine was *eligible* to dispatch in its session.

**Recommended action:** in the self-test loop, skip the pulse check for engines whose session window doesn't include the current `session_slot`. Two ways to wire it:

1. Add a `session_window` field on the heartbeat-registration record. Default `0xFFFFFF` (all 24 hours). Engines that are session-gated set their actual window during `register_engine()`. Self-test checks `now_session_slot ∈ session_window` before requiring a pulse.

2. Simpler interim: gate the entire startup-self-test output by `current_session_slot`. Print "skipped — out of session: Estx50, ..." instead of FAIL. Doesn't fix the underlying classification but stops the false alarm noise.

Option 1 is the right fix; option 2 is a 5-line patch that buys time.

---

## Side-issue priority (suggested S12+ order)

1. **3a marker prints + run INSTALL_WATCHDOG.ps1** — operationally most important. Without these you can't diagnose the next surprise shutdown.
2. **Finding A short-term reconnect loop** — restores L2-dependent engines without manual restart. Highest revenue impact.
3. **3c tee-install reorder** — small fix, big debugging quality-of-life win.
4. **3d alias additions** — 5-line config fix, removes warn-noise.
5. **Finding B option 2 (then option 1)** — lowest urgency; cosmetic noise on startup logs.

---

## What was NOT changed in this session

Per "never modify core code unless instructed clearly": every recommendation above is documented but not applied. Apply each only when you explicitly authorise it. The Phase C + D work in `DEPLOY_S12_PHASE_CD.sh` is the only ready-to-ship change.
