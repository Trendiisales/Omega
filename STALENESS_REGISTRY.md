# Omega Staleness & Brittleness Registry

**Purpose:** the single catalogue of everything in the Omega trading system that can go
STALE, silently freeze, or become brittle — with its producer, max acceptable age, the
monitor that watches it, and known gaps. Born 2026-07-10 after the two-box incident
([[TwoBoxIncident-20260710]] in Memory-Omega) where deploys landed on a retired box and
multiple mtime-based monitors false-greened.

**How to use:** run `bash tools/staleness_scan.sh` for a full sweep (also runs at session
start + on cron). RED = a live feed is stale → fix the producer before trusting signals.
Any NEW feed (`set_live_dump`, `start_poller`, seed CSV) or producer MUST be added here.

**Legend:** SEED = warm-start replay · LIVE = ongoing feed · STATE = persisted `.dat/.json` ·
GATE = feed that gates entries. Fail-mode: SAFE = stale → conservative (blocks/reverts) ·
OPEN = stale → permissive (allows) ← OPEN is the dangerous one.

---

## 1. LIVE feeds & gates (stale → engine mis-informed) — HIGHEST RISK

| Feed | Producer | Max age | Fail-mode | Monitor | Method |
|---|---|---|---|---|---|
| `logs/ibkr_l1/l2_*.csv` (XAU/DJ30/NAS100/MGC + FX + index) | **OmegaIbkrBridge** (`ibkr_dom_bridge.py`, task, 5min restart99) | 2–20min | — | `OmegaIbkrL2Freshness` (2min, self-heals) + feedpath cron | mtime+size |
| `data/rdagent/sp500_long_close.csv` (feeds 4 stock/index engines, 15min poll) | **in-binary daily-close writer** (20:00 UTC) + `refresh_close_ibkr.py`/`_yf.py` (Mac) | 1 td | quiet | **`vps_stock_book_health`** (content: last-row date + per-name bars=0) | **content** ✓ |
| `logs/macro/macro_gold_gate.tsv` (GATE gold macro) | `OmegaMacroGoldGate` (20:40 UTC) + Mac cron 4h | 36h → 26h alert | **SAFE** (→price core) | feeds_selftest manifest (1560min) | mtime |
| `logs/aurora/aurora_gate.tsv` (GATE) | `OmegaAuroraSnapshot` (1min) | 15min | **OPEN** ⚠ | omega_health.py chk_aurora | mtime |
| `logs/gold_regime_h1.csv` + `.dat` (GATE gold long-block) | in-binary B2 (per H1) | 120min / state 12h | SAFE | feeds_selftest manifest | mtime |
| `logs/index_mkt_regime_h1.csv` + `.dat` (GATE index risk-off) | in-binary B3 (per H1) | 120min / state 12h | SAFE | feeds_selftest manifest | mtime |
| `logs/gold_d1_trend_h4.csv` + `.dat` | in-binary B4 (per H4) | 300min / state 48h | SAFE | feeds_selftest manifest | mtime |
| `data/mgc_30m_live.csv` + `mgc_hvn.json` (30s poll) | `OmegaMgcLiveBars` (`mgc_live_bars.py`, 5min restart) | ~10min | **OPEN** (0.5 neutral) | omega_health.py chk_mgc | mtime |
| `data/stocks/{SYM}_15m/d1.csv` | `OmegaPullStockBars` (21:00 UTC daily) | 24h | quiet | data_health_monitor (percol) | content |
| `data/NDX_15m/5m.csv` | `OmegaPullNasCash` (21:00 UTC daily) | 24h | quiet | data_health_monitor | content |
| `companion_state.json` (VPS, protection) | in-binary companion loop | 5min | — | protection_selftest [1] | VPS mtime |
| `crypto_companion_state.json` (cockpit crypto pane) | `com.omega.crypto-companion-push` (chimera→VPS, 120s) | 1 td | — | feeds_selftest crypto heartbeat | content |
| VIX ratio / `index_regime.txt` (GATE, IndexSeasonal opt) | `fetch_vix_ratio.py` / `fetch_macro_regime.py` | 4d → ungated | OPEN | **⚠ UNSCHEDULED — see gaps** | — |
| Companion `*_daily.csv` / `*_h1.csv` (stock/fx/gold/index/usoil/xag BE-floor + ladders) | in-binary append + Mac pushers | 60s live_stale | drops stale bars | protection [6] input-freshness | content |

## 2. SEED (warm-start) CSVs — stale → engine COLD-STARTS (silent for 14–100d)

`phase1/signal_discovery/warmup_*.csv` (gold M5–D1, index D1/H1/M15/M30/M240, FX H1, M2K).
Resolved via `resolve_seed_path` (VPS-cwd robust). Boot log MUST show one `[SEED]` line/engine.
- Producer: `seed_refresh.py` / `refresh_daily_feeds.py` / bundled-in-repo (committed).
- Monitor: **`seed_freshness_audit.py`** — thresholds H1/regime 5d, D1/seasonal 45d, H4 14d.
- **GAP: runs ONLY at deploy, NOT on a cron** (fixed 2026-07-10 → cron added). Between deploys a
  seed could rot; this is the exact 2026-05-20 cold-start class.

## 3. Producers by scheduler (if it dies, its feed freezes)

**Windows tasks (VPS):** OmegaIbkrBridge·OmegaBigCapBridge·OmegaMgcLiveBars (5min restart99) ·
OmegaIbkrL2Freshness (2min watchdog) · OmegaMacroGoldGate (20:40 UTC) · OmegaPullStockBars /
OmegaPullNasCash (21:00 UTC) · OmegaAuroraSnapshot (1min) · OmegaSeedRefresh (23:30 UTC) ·
OmegaStockMoverFeed **DISABLED** (replaced by in-binary writer).
**In-binary (C++):** daily-close writer (B1) · regime dumps B2/B3/B4 · companion persistence B6.
**Mac launchd:** close-full (03:10) · qlib-refresh (03:40) · data-health (1h) · blend/regime/
reversal books (wkday 09:00) · crypto-companion-push (120s) · chimera book-refresh/watchdog.
**Mac cron:** feedpath (30min) · gold_befloor (1min) · macro_gold_gate (4h) · feeds/protection/
liveness/data_health self-tests (15–30min).

## 4. Monitors (what already watches what)

| Monitor | Covers | Cadence | Method |
|---|---|---|---|
| `feeds_selftest.py` | tick dailies, companion tele, crypto, qlib, rdagent, sp500 (content) + 5 VPS dumps (mtime) + IBKR-exec + **stock-book (content)** | session + 30min cron | mostly content |
| `protection_selftest.py` | companion alive/real/fires/closer/input-fresh/befloor-honesty | session + 15min | mixed |
| `feedpath_selftest.py` | IBKR primary path env/consumer/bridge/downstream/no-fallback | session + 30min | mixed |
| `liveness_check.py` | 5 Mac logs + VPS tasks/proc/GUI/HEALTH json | 15min | **mtime** |
| `omega_health.py` (VPS :7790) + `omega_health_poll.sh` | heartbeat/FIX/MGC/L2/aurora | VPS 45s + Mac 15min | mostly mtime |
| `data_health_monitor.py` | rdagent per-column, tick dailies, macro stamp, mgc seeds | 15min | content |
| `ibkr_l2_freshness_check.ps1` | per-symbol L2 csv, auto-restarts bridge | 2min | mtime+size |
| deploy: `seed_freshness_audit` + `live_dump_freshness_audit` | warm-seeds + set_live_dump manifest coverage | deploy/canary | content / static |

---

## 5. KNOWN GAPS & BRITTLENESS (the "never repeat" list) — ranked

| # | Sev | Item | Risk | Status |
|---|---|---|---|---|
| G1 | HIGH | **Hardcoded VPS host in 4+ files** (`feeds/protection/liveness/feedpath_selftest`) + `feedpath DESK="http://45.85.3.79:7779"` (IP!) | A re-IP/cutover silently breaks monitors → the two-box incident | **FIXING** → centralized `tools/omega_hosts.sh` |
| G2 | HIGH | **Crypto push + crypto crons still scp to RETIRED omega-vps** | `crypto_companion_state.json` on live box **3.4d stale**; scp to dead box silently fails | **FIXING** → repoint to omega-new |
| G3 | HIGH | **seed_freshness_audit not on cron** (deploy-only) | Warm-seed rots between deploys → cold-start (2026-05-20 class) | **FIXING** → cron added |
| G4 | MED | **mtime false-greens**: feeds `companion telemetry`, all 5 `liveness_check` Mac probes, feedpath L1, VPS manifest, omega_health | A touched-but-frozen file / an error-line append reads GREEN | Documented; migrate to content opportunistically |
| G5 | MED | **Unscheduled producers**: `fetch_vix_ratio`, `fetch_macro_regime`, `gapper_recorder`, `ibkr_universe_logger` | Feed live gates but no scheduler found → silent stale | **VERIFY** scheduling; add cron or retire |
| G6 | MED | **Duplicate producer processes** on VPS: `mgc_live_bars`, `bigcap_feed_bridge`, `ibkr_dom_bridge` each ×2 (.venv + Python312) | Two bridges fight over IBKR market-data lines → may cause the bigcap 30-line cap | **INVESTIGATE** — single-instance guard |
| G7 | MED | **Bigcap bridge MAX_SYMBOLS=30 < 45 ladder names** | Tail 15 names get no live mid → daily cell blank going forward | Backstop: `refresh_close_ibkr` 45; guard REDs stalls; bump deferred (line limits) |
| G8 | LOW | `live_dump_freshness_audit` WARN-only in canary (not STRICT) | A new unmonitored `set_live_dump()` can be committed | Set `STRICT=1` in canary |
| G9 | LOW | Event-driven dumps (`omega_trade_closes/fill/day_results`) unmonitorable by design | Dead writer here is silent | Accepted; add a heartbeat row if needed |
| G10 | LOW | `wave_companion_selftest.py` lives in `~/Crypto`, outside Omega audit | Rename/breakage invisible to Omega gates | Note in registry |
| G11 | LOW | FX pairs != GBPUSD, crypto intraday books | No dedicated live-path monitor | Add when re-enabled |

*(Live status of every item is produced by `tools/staleness_scan.sh`; this table tracks
structural gaps, not the moment-to-moment freshness.)*
