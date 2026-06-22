# RD-Agent ↔ Omega bridge

Connects Microsoft RD-Agent (autonomous factor/model R&D, runs in the `rdagent`
conda env at `~/RD-Agent`) to Omega. Two directions + a standalone GUI.

**Discipline reminder:** RD-Agent backtests are daily bar-replay. Nothing here
is a trade signal. Every factor/model it produces is a CANDIDATE that must clear
Omega's faithful tick BT on the real universe before it can be sized. The GUI
flags non-Omega universes red on purpose. Deploy stays gated.

## Pieces

| File | Role | Status |
|---|---|---|
| `export_signals.py` | Path A: read a finished qlib run → `~/Omega/data/rdagent/latest.json` (factors, metrics, signal, provenance) | verified |
| `omega_to_qlib.py` + `dump_bin.py` | Path B: convert Omega OHLCV bars → a qlib dataset so RD-Agent mines on YOUR universe | verified (synthetic) |
| `gui/serve.py` + `gui/index.html` | Standalone research panel (sidecar — NOT in core OmegaApiServer) | verified |

## Path A — factor generator (light, BACKTEST_TRUTH-aligned)

1. In a terminal where `claude` is logged in:
   ```
   conda activate rdagent && cd ~/RD-Agent && rdagent fin_factor
   ```
2. Export the latest result for the GUI:
   ```
   conda run -n rdagent4qlib python ~/Omega/tools/rdagent/export_signals.py \
       --mlruns ~/RD-Agent/log/<...>/mlruns \
       --factors ~/RD-Agent/git_ignore_folder/RD-Agent_workspace/<factor>.py
   ```
3. Read the discovered alpha expression → port it into Omega's faithful tick BT
   (`backtest/`) on gold/index/bigcap. Only a survivor there is a real candidate.

## Path B — RD-Agent on the Omega universe (heavy)

1. Get Omega bars (per-symbol CSV: date,open,high,low,close,volume[,factor];
   pull from VPS `C:\Omega\data` — local has none) into a folder.
2. Convert to qlib:
   ```
   conda run -n rdagent4qlib python ~/Omega/tools/rdagent/omega_to_qlib.py \
       --input <bars-dir> --out ~/.qlib/qlib_data/omega_data --universe omega
   ```
3. Point an RD-Agent qlib conf at `provider_uri: ~/.qlib/qlib_data/omega_data`,
   `market: omega`. Then RD-Agent mines + predicts on YOUR names → the exported
   signal becomes directly tradeable (still after faithful cost validation).
   TODO: a faithful cost model in the qlib `exchange_kwargs` matching IBKR.

## GUI

- **Sidecar panel (built here):** `python3 ~/Omega/tools/rdagent/gui/serve.py --port 7799`
  → http://127.0.0.1:7799 . Reads `latest.json`, auto-refreshes. Can later be
  iframed as a route in omega-terminal (frontend-only change; no core C++ edit).
- **RD-Agent's own dashboard (zero build):** `rdagent ui --port 19899` — watch the
  discovery loop (hypotheses, code, evolution).

## Trading system (regime-switching, flat-by-close, no overnight risk)

Backtested on real bigcap (15m + daily, 2024-06..2026-06). **All flat-by-close — nothing held overnight.** 3-state regime brain (`export_signals._regime_strategy`, shown live in the GUI):

- **LONG** (calm bull): long top-K, enter open / exit close. + **high-vol gate** (sit out top-quartile-vol days) lifted Sharpe 0.85→1.18. No profit-target (capping winners tested worst).
- **CASH** (soft-bear / high-vol chop): sit out.
- **SHORT** (fast drop >4%/5d, or violent bear): the **bear engine** arms — short the index intraday, flat by close. Sharpe 0.49, maxDD −5%, +20bps on armed days, idle ~86% of the time. A crash hedge, not standalone alpha (thin sample). `always-short` = −98% (gate is essential); cross-sectional short had no edge.

Backtest scripts: `intraday_bt.py` (overnight-vs-intraday split), `bear_regime_bt.py` (regime gates + long/short), `bear_engine_bt.py` (crash-hedge arming), `backtest_trail.py` (trailing/bracket — all hurt vs daily rebalance).

**Daily runner:** `bash daily_runner.sh` (schedule pre-open) refreshes the export + today's LONG/CASH/SHORT decision + keeps the GUI up. Proposes only — execution gated. Still a bull-beta base signal: structure validated, edge not.

## How it informs "what/when to trade"

The model emits a daily `score` per instrument → rank → top-K = buy basket
(`when` = daily rebalance). That is the signal. On CSI300 those names aren't
yours (Path A = mine ideas, re-test in Omega). On the Omega universe (Path B)
the basket is your names — size only what the faithful BT confirms.

## Mac setup notes (patches applied to ~/RD-Agent)

- `scenarios/qlib/experiment/utils.py` — Docker → conda
- `utils/env.py` — GNU `timeout` → `gtimeout`
- `scenarios/qlib/experiment/factor_data_template/generate.py` — `kernels=1`
- `oai/backend/claude_code.py` — chat via `claude -p` (subscription, no API key)
- `.env` — conda exec + Claude backend + Ollama embeddings + `MLFLOW_ALLOW_FILE_STORE`
