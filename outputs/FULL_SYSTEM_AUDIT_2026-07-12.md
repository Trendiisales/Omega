# FULL SYSTEM AUDIT — 2026-07-12

Operator order: "a complete audit to ensure we have nothing missing, check entire
system and report." READ-ONLY diagnosis across Omega + ChimeraCrypto + Crypto.
Audit window: 2026-07-12 ~00:18–00:30 UTC (Sunday, markets closed — weekend guards active).

Systems:
1. **Omega** — /Users/jo/Omega (Mac) + live box `omega-new` (45.85.3.79, Windows). `omega-vps`/185 = dead box, not touched.
2. **ChimeraCrypto** — /Users/jo/ChimeraCrypto (Mac) + live box `josgp1`/`chimera-direct` (143.198.89.54, systemd). SHADOW.
3. **Crypto** — /Users/jo/Crypto (Mac ibkrcrypto book, cron-driven).

---

## 1. RED — broken now, needs action (ranked)

### R1. SELF-INFLICTED: audit's tokenless kill-test fired a REAL kill on the chimera shadow book
- During the §J security check I POSTed `https://localhost:9443/api/kill` (tokenless) expecting a 401. It returned **200 and FIRED** — box nginx injects `X-Auth-Token` on every `/api/` proxied request, so a tokenless request *through nginx* is authorized by design.
- Effect (josgp1 journal, 2026-07-12 00:24:33 UTC): `kill_all` flattened the 2 open shadow legs — **DOGE-UPJUMP4-H4 EXIT reason=KILL net −253.17bp** and **ETH-UPJUMP2-H1 EXIT reason=KILL net −149.12bp** — and set `halted_=true` on those two EdgeEngine slots.
- This is SHADOW (no real broker money), and the loss was already open/unrealised, but two engines are **now latched halted** and will not re-enter until cleared.
- **Remediation (owed, operator call):** `POST /api/session_reset` with the token (warm reset — clears all halts, keeps lifetime equity/peak) restores trading. I did NOT run it (not a zero-risk change — it also rolls the 24h risk-window epoch). One command, operator's word.
- Lesson: never probe a mutating control endpoint through the token-injecting proxy. The correct 401 test is direct-to-`:8080` (see G-note below — that path *does* return 401).

---

## 2. AMBER — drift / debt / owed (ranked, with where tracked)

### A1. `com.omega.supervisor` launchd agent is a dead crash-loop — MISSING FROM ALL TRACKING
- `launchctl print` → `last exit code = 78 (EX_CONFIG)`, **runs = 61075**, KeepAlive=true.
- Its plist (`~/Library/LaunchAgents/com.omega.supervisor.plist`) points to `/Users/jo/omega-supervisor/omega_supervisor.py` — **that directory does not exist.** launchd has been respawn-looping an unresolvable program 61k times.
- Not referenced in any vault entity or log. **New finding.** Harmless (fails instantly, no side-effects) but should be `launchctl bootout` + plist removed, or the target restored. Not tracked anywhere.

### A2. omega-new box git tree is 2 commits behind origin/main (benign — Mac tooling only)
- Running binary `85280d8` (built Jul 11 23:55, `Git hash:` line confirms) == box working-tree HEAD. Mac HEAD == origin/main == **`3f1075cf`**, i.e. box is 2 commits behind.
- The 2 commits are `3f1075cf` (display_truth_selftest.py + manifest) and `db31ac16` (omega_deploy.sh + deploy_detached.ps1) — **Mac-side tooling only, zero C++/engine change** → no rebuild required, binary is functionally current. But the box repo should be `git pull`ed to `3f1075cf` to keep hash-alignment honest per DEPLOY_HYGIENE.

### A3. Warm-seed staleness: `gold.mgc_30m_seed` 17d old (limit 7d)
- Flagged by data_health_monitor (advisory/RED, non-blocking) inside staleness_scan. MGC 30m warm-seed CSV is 17 days old. `OmegaSeedRefresh` scheduled task exists (Ready, fires 23:30) but the seed file isn't being refreshed to spec. Tracked only as an advisory line, no owed-item entry.

### A4. nginx 9443 exposes a credential-free kill path (public bind) — accepted-design vs exposure, operator decision owed
- `:9443` binds `0.0.0.0` and its `/api/` block hard-injects the control token, so **anyone who can reach 9443 can POST /api/kill with no credential** (that is how R1 fired). Direct `:8080` is `127.0.0.1`-only and returns 401 tokenless (auth genuinely enforced there).
- The vault frames control-auth as "enforced" (`[[ChimeraSmallDebts]]`), which is true for the *direct* path but not the *public proxied* path. Worth an explicit operator ruling: is browser-GUI-kill-without-login the intended posture, or should 9443 `/api/kill` require a client token / IP-allowlist?

### A5. Open OWED items confirmed still owed (all tracked in vault — burn-down register)
| Item | Tracked | Status |
|------|---------|--------|
| Relay scp atomicity (crypto_companion transient-empty ~2/10) | Memory-Omega `[[DisplayTruthSelftest]]`/`[[CryptoCompanionsGuiPanel]]` | OWED (selftest retries as workaround) |
| Stall/wave 2 FIX-IN-FLIGHT strands (`crypto_stall_companion_3books`, `wave_companion`) | `trade_visibility_manifest.tsv` rows 52–53 | Operator decision owed: WIRE (needs desk endpoint) or RETIRED-ok |
| Engine-ledger row-parity residue | `[[DisplayTruthSelftest]]` | OWED (no independent record to reconcile against) |
| Slot `g_trade_log` wiring verdict | Memory-Chimera `[[ChimeraDeskTradeFeed]]` | Verdict owed (wiring = risk change) |
| Gold Phase 2 (session svc + net-edge gate + gold allocator + MgcDonchian netting) | `GOLD_BOOK_ROADMAP.md`, GOLD_PHASE1B doc | Deferred — next phase |
| Revisit lot sizes ($10k placeholder notional) | memory `project-revisit-lot-sizes` | Deferred — do NOT touch mid-stabilisation |
| IBKR MGC/COMEX Error-354 monthly-billing lapse | `[[IbkrDomBridge]]` | Resolved per-instance; **no automated guard** against the billing-turn lapse (structural recurrence) |
| UK100 / USOIL IBKR subs (ICE-FTSE + NYMEX) | `[[FeedMigrationBlackBullToIBKR]]` | OWED — operator adds paid subs; auto-flip on activation |
| OmegaSeedRefresh still Python | log 10-07 | C++ port owed (no-python mandate) |
| Crypto 8A hardcap default-flip sign-off / 8E XSec2 forward / 8F recorder | CRYPTO_PHASE8_ROADMAP | Forward-sample gated (not code) |
| PR #4 `claude/vps-address-migration-cnxcnb` merge state | `[[VpsMigration_ForexVpsEdge]]` | Changes applied manually; draft branch merge **unverified** |

### A6. Doc drift: "81-engine adverse-protection backfill" figure is STALE
- CLAUDE.md + MEMORY.md still cite "81 legacy entry-engines owe an annotation." Live canary reports **86 annotated, 3 legacy backfill-owed, 0 violations** — the debt is effectively closed (3 left, not 81). The "81" framing is untracked-as-stale doc drift.

### A7. Minor tree drift (benign)
- **Chimera box** working tree: `config/live_config.json` (+1 line), `include/version_generated.hpp` (build artifact); untracked `CMakeLists.txt.bak`, `backtest/ENGINE_PROMOTION_GATE.md`, `backtest/decay_monitor.sh`. Box binary hash still == Mac/origin `5ca5d53`. Worth a glance at the live_config.json 1-line delta.
- **Crypto (Mac)**: many `backtest/data/*.csv` modified uncommitted — expected fetch-cron data churn, not code.
- **Omega (Mac)**: `src/gui/OmegaTelemetryServer.cpp` modified in working tree (pre-existing, per session start status) + large untracked `backtest/` research pile. No code drift on tracked engine files.

---

## 3. GREEN — checked and healthy (evidence refs)

**Git/deploy alignment (A):**
- Omega: Mac HEAD == origin/main == `3f1075cf`; no unpushed commits. Box binary `85280d8` == box HEAD, `Git hash:` line matches (2-commit tooling lag = A2, benign).
- Chimera: Mac HEAD == origin/main == `5ca5d53`; box `build=5ca5d53` (journal STARTUP) == HEAD. No unpushed.
- Crypto: Mac HEAD == origin/main == `03c3e84`. No unpushed (only data-CSV churn).

**Services + health (B):**
- `Omega.exe` running (PID 4356, started Jul 11 23:56), no `FAIL` lines in startup; RISK-MON loaded 4 threshold rows + auto-pin callbacks. All `[SEED]` lines present and hot (BE-floor retired-states, GoldTrendMimicLadder 7 books, FX GBPUSD ladder 7928 bars, INDEX ladder US500/NAS100/GER40/M2K, BIGCAP 45-name + daily-close writer, Stock dip/turtle, ConnorsMirror/SpxTurtleMirror/XauTF2h/4h mirrors, NasTurtleD1 x3, IndexSeasonal x5).
- chimera.service active 37min, **NRestarts=0**, no error/exception lines in journal.
- omega-new scheduled tasks all present per design: IbkrBridge / BigCapBridge / MgcLiveBars / Healthcheck = Running; snapshot/prune/backup/gate tasks = Ready; GapShort / NYShadowTally / PumpShadow / StockMoverFeed = intentionally Disabled.
- Mac launchd/crons alive: crypto-companion-push (120s, runs 4652, exit 0), book-refresh (300s, exit 0), book-watchdog, rdagent-gui-refresh, cockpit running. Cron log freshness confirmed firing on schedule: fetch 12:00, live_mark 12:20, stall 12:24, wave 12:07, pull_vps 12:20, rda_basket 12:11 (NZ).

**Monitors (C) — all GREEN/explained, all crash-safe-wrapped (S-2026-07-12):**
- protection_selftest GREEN (7/7 checks). feeds_selftest all live+research PASS, vps-live SKIP (weekend). feedpath GREEN (primary IBKR path; consumer/bridge SKIP weekend). liveness_check ALL-ALIVE. display_truth_selftest **RESULT: GREEN** (trade-recon 0→3→3, all symbol-coverage panels, 32/32 config-labels). staleness_scan RESULT GREEN(live) (1 advisory research-stale = A3). wave_companion GREEN (6/6). crypto_staleness OK daily+intraday. Weekend guards behaving correctly (SKIP on market-closed).

**Guards / canary (D):**
- Mac canary EXIT 0. 37 engine headers ok. adverse-protection 86 annotated / 3 legacy / **0 violations**. tombstone-guard 13 tombstoned / 21 enabled / **0 resurrections**. fail-verdict-guard 3 FAIL globals / **0 enabled**. live-dump 3 monitored / 0 unmonitored. mimic drawdown 5 annotated / 6 retired-grandfathered / **0 violations**. pnl-completeness 9 endpoints / 9 totals / **0 gaps**. trade-visibility 29 rows / 2 FIX-IN-FLIGHT warn / **0 gaps**. GUI-drift in sync. persistence OK.
- Chimera `run_all_tests.sh`: **ALL SUITES PASS (10/10)** — Phase 1–8A + CI-matrix (incl user_stream_autohalt / allocator_vs_legacy).

**Books + display truth (E):**
- display_truth_selftest confirms every desk panel serves post-zero honest state (ibkrcrypto recon 0→3→3, all 8 chimera coins + 15 daily + 34 intraday + 1 fx + 4 idx + 45 bigcap slots present).
- Complete-zero holding: daily book realized $0.00; intraday small new-era realized ($−13.28) = legitimate post-t0 flow. Backups verified present on all 3 boxes (`crypto_zero_backup_20260712_110000/` on Mac + josgp1 + omega-new).

**Engines (F):**
- 3 culled gold engines confirmed disabled in engine_init.hpp: `g_rider_d1.enabled=false`, `g_xau_tf_2h.enabled=false`, `g_gold_panic_bounce.enabled=false` (fail-verdict guard corroborates 0 enabled). Warm-seeds hot (all `[SEED]` lines present, F-note A3 for the one 17d-stale mgc seed).

**Feeds/data (G):**
- feeds_selftest live+research all PASS/fresh; vps-live SKIP by weekend guard (correct). Direct `:8080` control API returns **401 tokenless** and **200 with token** — auth genuinely enforced on the direct path; :8080 bound 127.0.0.1 only.

**Security (J):**
- chimera `nginx -t` syntax OK; TLS 9443 active (self-signed pair present). Listening ports: 22/53/80/8080(localhost)/9443 only — no new exposed ports beyond the accepted set. Direct control-API auth enforced (401 tokenless). Caveat = A4 (public 9443 injection).

---

## 4. MISSING FROM ALL TRACKING (the operator's real question)
1. **`com.omega.supervisor` dead crash-loop** (A1) — plist targets a deleted directory, 61k respawns, in no vault/log. NEW.
2. **nginx 9443 credential-free public kill path** (A4/R1) — the exposure that let the audit itself fire a kill is not captured as a risk decision; vault only records the *direct*-path auth.
3. **"81-engine adverse-protection backfill"** (A6) — CLAUDE.md/MEMORY.md figure is stale; real owed count is 3. Doc-drift, untracked as such.
4. **`gold.mgc_30m_seed` 17d warm-seed staleness** (A3) — lives only as an advisory monitor line, no owed-item entry despite OmegaSeedRefresh existing.

---

*Read-only audit. The ONE state mutation was accidental (R1) and is disclosed above; nothing else was changed.*

---

## 5. REMEDIATION ADDENDUM (same day, follow-up session)

| Finding | Status | Fix |
|---------|--------|-----|
| R1 kill accident | **CLOSED** | `session_reset` (with token) → 200; `/api/state` 0 halted slots; book re-armed shadow. Guard: `tools/AUDIT_PROBE_SAFETY.md` |
| A4 public credential-free kill | **CLOSED** | nginx basic-auth gate on the 4 mutating endpoints (`kill\|ratchet_reset\|daily_kill_clear\|session_reset`) before token injection. Verified 401-at-nginx credless from localhost AND public path; reads/GUI/WS 200 unchanged. Cred: user `jo`, pass in box `/root/chimera_ctl_password.txt`. Template synced token-free (ChimeraCrypto `ca0601b`) |
| A1 supervisor crash-loop | **CLOSED** | `launchctl bootout` + plist archived to `~/Library/LaunchAgents/retired/` (61,217 respawns at removal) |
| A2 box 2 commits behind | **CLOSED** | Box pulled to `3f1075cf` == origin (hash-identical untracked `deploy_detached.ps1` removed first) |
| A3 mgc seed 17d stale | **CLOSED** | Root cause: monitor watched the Mac repo copy the engine never loads — box seed was fresh nightly. `data_health_monitor` repointed to `~/Omega-vps-mirror/data/` (pull_vps_display.sh now mirrors mgc_30m + mgc_h1). Verified OK/OK. Omega `62d5376b` |
| A6 stale "81" figure | **CLOSED** | CLAUDE.md updated 81→3 (canary-verified), Omega `62d5376b` |

nginx GOTCHA discovered during A4 verification: a `return` directive inside a location
fires in the rewrite phase BEFORE `auth_basic` — an auth-gated `return 204` self-check
silently bypasses auth. Auth self-checks must proxy a harmless read instead.
