#!/usr/bin/env python3
"""Trade Sentinel — automatic per-trade loss triage + win-improvement miner (Omega + Chimera).

Operator mandate (2026-07-18c): every negative trade — genuine or error — must be surfaced
automatically WITH the trade shown and a concrete suggestion to either FIX (if error class) or
TUNE (if genuine, mitigation candidate). Wins are mined too: capture ratio, regime/session
concentration, mimic/ladder companion candidacy per feedback-extend-winning-engines-standing-order.

Division of labour in tools/ml_loss_miner/:
  trade_sentinel.py  (this)  — incremental per-TRADE triage, runs on a cron cadence; state-file
                               diff so each closed trade is triaged exactly once; ML = per-engine
                               IsolationForest / robust-z outlier scoring + rule-based error classes.
  mine_losses.py             — batch per-ENGINE pattern miner (decision tree / groupby);
                               run weekly or on demand for the deep dimensional view.

Doctrine (hard, standing):
  * NEVER auto-wires a fix. Every suggestion is a proposal; threshold/gate/config changes require
    a certified faithful backtest first (ENGINE_BACKTEST_REGISTRY.md / BACKTEST_TRUTH_CRYPTO.md).
  * Read-only data pulls only (feedback-audit-read-only-never-mutate).
  * Shadow books = live severity (feedback-shadow-treated-as-live).
  * Companion books judged STANDALONE, never vs WIDE (feedback-companion-independent-engine).
  * BE-floor = config property, not execution guarantee — FLOOR_CLIP books honest fills and CAN
    be negative on gaps/restarts (S-17f). Such tails are GENUINE unless anomalously large.
  * goldmimic_*_closed.csv is a dead append-only audit log — NEVER ingested (double-count,
    project-goldmimic-closed-csv-dedup).

Usage:
  python3 tools/ml_loss_miner/trade_sentinel.py                 # live pull, incremental
  python3 tools/ml_loss_miner/trade_sentinel.py --full          # ignore state, triage everything
  python3 tools/ml_loss_miner/trade_sentinel.py --csv f.csv --system omega   # offline test
  python3 tools/ml_loss_miner/trade_sentinel.py --notify        # macOS banner when new negatives

Env: /opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python (sklearn+pandas).
"""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from io import StringIO
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[2]
STATE_PATH = REPO / "outputs" / "sentinel_state.json"
ALERT_LOG = REPO / "outputs" / "sentinel_alerts.log"

OMEGA_SSH_HOST = "omega-new"
CHIMERA_SSH_HOST = "chimera-direct"
CHIMERA_LEDGER_PATH = "~/ChimeraCrypto/data/chimera_inbound.csv"   # LEGACY shadow feed (truncated 20f)
CHIMERA_LIVE_LEDGER_PATH = "~/ChimeraCrypto/data/live_trades.csv"  # ledger of record since S-2026-07-20f
                                                                   # (LiveMimicMirror::book_close_ appends
                                                                   #  every real Binance SELL)

MIN_ENGINE_N_FOR_IFOREST = 20   # below this, robust z-score vs engine history instead
OUTLIER_Z = 3.0                 # |robust z| beyond this = statistical outlier flag
GIVEBACK_MFE_MIN = 5.0          # min MFE (native pnl units) before a neg trade counts as giveback
CAPTURE_MIMIC_THRESHOLD = 0.55  # median winner capture below this -> giveback-clip mimic candidate
MIN_WINS_FOR_CANDIDACY = 8      # wins needed before win-side candidacy is proposed


# ── data pulls (read-only) ────────────────────────────────────────────────────
def _run(cmd: list[str], timeout: int = 60) -> str | None:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except Exception as e:  # noqa: BLE001
        print(f"  WARN: {' '.join(cmd[:2])} failed: {e}", file=sys.stderr)
        return None
    if out.returncode != 0 or not out.stdout.strip():
        return None
    return out.stdout


def pull_omega() -> list[tuple[str, str, str]]:
    """One ssh round-trip for ALL omega sources. Returns [(book, source_name, csv_text)].
    Read-only Get-Content only (feedback-audit-read-only-never-mutate)."""
    ps = (
        "powershell -Command \""
        "'===F===live===omega_trade_closes.csv'; Get-Content -Raw 'C:\\Omega\\logs\\trades\\omega_trade_closes.csv'; "
        "'===F===shadow===omega_shadow.csv'; Get-Content -Raw 'C:\\Omega\\logs\\shadow\\omega_shadow.csv'; "
        "Get-ChildItem 'C:\\Omega\\stall' -Recurse -Filter companion_closed.csv | ForEach-Object { "
        "'===F===companion==='+$_.Directory.Name; Get-Content -Raw $_.FullName }\""
    )
    raw = _run(["ssh", OMEGA_SSH_HOST, ps], timeout=90)
    if not raw:
        print("  WARN: omega pull failed entirely", file=sys.stderr)
        return []
    out = []
    for chunk in raw.split("===F===")[1:]:
        header, _, body = chunk.partition("\n")
        book, _, src = header.strip().partition("===")
        if body.strip():
            out.append((book, src.strip(), body))
    return out


def pull_chimera() -> list[tuple[str, str]]:
    """One ssh round-trip for both crypto ledgers. Returns [(kind, csv_text)].
    live_trades.csv = ledger of record since 20f; chimera_inbound.csv kept as legacy
    (writer removed + truncated at source, but old rows may exist on a restore)."""
    sh = (
        f"echo '===F===live==='; cat {CHIMERA_LIVE_LEDGER_PATH} 2>/dev/null; "
        f"echo '===F===legacy==='; cat {CHIMERA_LEDGER_PATH} 2>/dev/null"
    )
    raw = _run(["ssh", CHIMERA_SSH_HOST, sh], timeout=60)
    if not raw:
        print("  WARN: chimera pull failed entirely", file=sys.stderr)
        return []
    out = []
    for chunk in raw.split("===F===")[1:]:
        header, _, body = chunk.partition("\n")
        if body.strip() and len(body.strip().splitlines()) > 1:  # header-only file = empty
            out.append((header.strip().rstrip("="), body))
    return out


# ── normalization to one schema ───────────────────────────────────────────────
# unified: system, book, engine, symbol, side, entry_ts, exit_ts, net_pnl, mfe, mae,
#          hold_sec, exit_reason, regime, spread_at_entry, slippage_exit_pct, key
def norm_omega_live(text: str) -> pd.DataFrame:
    df = pd.read_csv(StringIO(text))
    if df.empty:
        return df
    out = pd.DataFrame({
        "system": "omega", "book": "live",
        "engine": df.get("engine"), "symbol": df.get("symbol"), "side": df.get("side"),
        "entry_ts": pd.to_numeric(df.get("entry_ts_unix"), errors="coerce"),
        "exit_ts": pd.to_numeric(df.get("exit_ts_unix"), errors="coerce"),
        "net_pnl": pd.to_numeric(df.get("net_pnl"), errors="coerce"),
        "mfe": pd.to_numeric(df.get("mfe"), errors="coerce"),
        "mae": pd.to_numeric(df.get("mae"), errors="coerce"),
        "hold_sec": pd.to_numeric(df.get("hold_sec"), errors="coerce"),
        "exit_reason": df.get("exit_reason"), "regime": df.get("regime"),
        "spread_at_entry": pd.to_numeric(df.get("spread_at_entry"), errors="coerce"),
        "slippage_exit_pct": pd.to_numeric(df.get("slip_exit_pct"), errors="coerce"),
        "entry_px": pd.to_numeric(df.get("entry_px"), errors="coerce"),
        "exit_px": pd.to_numeric(df.get("exit_px"), errors="coerce"),
        "commission": pd.to_numeric(df.get("commission"), errors="coerce"),
    })
    out["key"] = "OL|" + df["trade_ref"].astype(str) + "|" + df["exit_ts_unix"].astype(str)
    return out


def norm_omega_shadow(text: str) -> pd.DataFrame:
    df = pd.read_csv(StringIO(text))
    if df.empty:
        return df
    df = df[df.get("side", pd.Series(dtype=str)).astype(str).str.upper() != "HEARTBEAT"]
    df = df[~df.get("exit_reason", pd.Series(dtype=str)).astype(str).str.contains("boot_writetest", case=False, na=False)]
    if df.empty:
        return pd.DataFrame()
    ts = pd.to_numeric(df["ts_unix"], errors="coerce")
    hold = pd.to_numeric(df.get("hold_sec"), errors="coerce")
    out = pd.DataFrame({
        "system": "omega", "book": "shadow",
        "engine": df.get("engine"), "symbol": df.get("symbol"), "side": df.get("side"),
        "entry_ts": ts - hold.fillna(0), "exit_ts": ts,
        "net_pnl": pd.to_numeric(df.get("pnl"), errors="coerce"),
        "mfe": pd.to_numeric(df.get("mfe"), errors="coerce"),
        "mae": pd.to_numeric(df.get("mae"), errors="coerce"),
        "hold_sec": hold,
        "exit_reason": df.get("exit_reason"), "regime": df.get("regime"),
        "spread_at_entry": pd.to_numeric(df.get("spread_at_entry"), errors="coerce"),
        "slippage_exit_pct": np.nan,
        "entry_px": pd.to_numeric(df.get("entry_px"), errors="coerce"),
        "exit_px": pd.to_numeric(df.get("exit_px"), errors="coerce"),
        "commission": np.nan,
    })
    out["key"] = ("OS|" + df["ts_unix"].astype(str) + "|" + df["engine"].astype(str) + "|"
                  + df["symbol"].astype(str) + "|" + df["pnl"].astype(str))
    return out


def norm_omega_companion(text: str, book_dir: str) -> pd.DataFrame:
    # ts,book,reason,engine,symbol,side,entry,realized_pnl,mfe_peak_pct,bars_held
    df = pd.read_csv(StringIO(text))
    if df.empty:
        return df
    out = pd.DataFrame({
        "system": "omega", "book": f"companion:{book_dir}",
        "engine": df.get("engine"), "symbol": df.get("symbol"), "side": df.get("side"),
        "entry_ts": np.nan, "exit_ts": pd.to_numeric(df.get("ts"), errors="coerce"),
        "net_pnl": pd.to_numeric(df.get("realized_pnl"), errors="coerce"),
        "mfe": pd.to_numeric(df.get("mfe_peak_pct"), errors="coerce"),  # pct units, peak fav
        "mae": np.nan,
        "hold_sec": pd.to_numeric(df.get("bars_held"), errors="coerce"),
        "exit_reason": df.get("reason"), "regime": None,
        "spread_at_entry": np.nan, "slippage_exit_pct": np.nan,
        "entry_px": pd.to_numeric(df.get("entry"), errors="coerce"), "exit_px": np.nan,
        "commission": np.nan,
    })
    out["key"] = ("OC|" + book_dir + "|" + df["ts"].astype(str) + "|" + df["engine"].astype(str)
                  + "|" + df["symbol"].astype(str) + "|" + df["realized_pnl"].astype(str))
    return out


def norm_chimera(text: str) -> pd.DataFrame:
    # id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd,reason
    df = pd.read_csv(StringIO(text))
    if df.empty:
        return df
    ets = pd.to_numeric(df.get("entry_ts"), errors="coerce")
    xts = pd.to_numeric(df.get("exit_ts"), errors="coerce")
    out = pd.DataFrame({
        "system": "chimera", "book": "companion",
        "engine": df.get("strat"), "symbol": df.get("sym"), "side": df.get("side"),
        "entry_ts": ets, "exit_ts": xts,
        "net_pnl": pd.to_numeric(df.get("net_usd"), errors="coerce"),
        "mfe": np.nan, "mae": np.nan,
        "hold_sec": xts - ets,
        "exit_reason": df.get("reason"), "regime": None,
        "spread_at_entry": np.nan, "slippage_exit_pct": np.nan,
        "entry_px": pd.to_numeric(df.get("entry"), errors="coerce"),
        "exit_px": pd.to_numeric(df.get("exit"), errors="coerce"),
        "commission": np.nan,
    })
    out["key"] = "CH|" + df["id"].astype(str)
    return out


def norm_chimera_live(text: str) -> pd.DataFrame:
    # exit_ts_ms,entry_ts_ms,coin,tag,qty,entry_px,exit_px,realized_usd,reason
    # (data/live_trades.csv — real Binance fills, fees folded; S-2026-07-20f ledger of record)
    df = pd.read_csv(StringIO(text))
    if df.empty:
        return df
    ets = pd.to_numeric(df.get("entry_ts_ms"), errors="coerce") / 1000.0
    xts = pd.to_numeric(df.get("exit_ts_ms"), errors="coerce") / 1000.0
    out = pd.DataFrame({
        "system": "chimera", "book": "live",
        "engine": "LiveMimic", "symbol": df.get("coin"), "side": "BUY",  # long-only spot
        "entry_ts": ets, "exit_ts": xts,
        "net_pnl": pd.to_numeric(df.get("realized_usd"), errors="coerce"),
        "mfe": np.nan, "mae": np.nan,
        "hold_sec": xts - ets,
        "exit_reason": df.get("reason"), "regime": None,
        "spread_at_entry": np.nan, "slippage_exit_pct": np.nan,
        "entry_px": pd.to_numeric(df.get("entry_px"), errors="coerce"),
        "exit_px": pd.to_numeric(df.get("exit_px"), errors="coerce"),
        "commission": np.nan,
    })
    # FIFO backfill legitimately yields identical rows (two fills, same px/qty) — suffix an
    # ordinal so each row keeps a distinct state key instead of collapsing to one.
    base = ("CL|" + df["tag"].astype(str) + "|" + df["exit_ts_ms"].astype(str) + "|"
            + df["qty"].astype(str) + "|" + df["realized_usd"].astype(str))
    out["key"] = base + "#" + base.groupby(base).cumcount().astype(str)
    return out


def load_all(offline_csv: str | None, offline_system: str) -> pd.DataFrame:
    frames: list[pd.DataFrame] = []
    if offline_csv:
        raw = Path(offline_csv).read_text()
        head = raw.splitlines()[0] if raw else ""
        if "trade_ref" in head:
            frames.append(norm_omega_live(raw))
        elif "realized_pnl" in head:
            frames.append(norm_omega_companion(raw, "offline"))
        elif "net_usd" in head:
            frames.append(norm_chimera(raw))
        elif "realized_usd" in head:
            frames.append(norm_chimera_live(raw))
        else:
            frames.append(norm_omega_shadow(raw))
    else:
        for book, src, text in pull_omega():
            try:
                if book == "live":
                    frames.append(norm_omega_live(text))
                elif book == "shadow":
                    frames.append(norm_omega_shadow(text))
                elif book == "companion":
                    frames.append(norm_omega_companion(text, src))
            except Exception as e:  # noqa: BLE001
                print(f"  WARN: parse failed for omega {book}/{src}: {e}", file=sys.stderr)
        for kind, text in pull_chimera():
            try:
                frames.append(norm_chimera_live(text) if kind == "live" else norm_chimera(text))
            except Exception as e:  # noqa: BLE001
                print(f"  WARN: parse failed for chimera {kind}: {e}", file=sys.stderr)
    frames = [f for f in frames if f is not None and not f.empty]
    if not frames:
        return pd.DataFrame()
    df = pd.concat(frames, ignore_index=True, sort=False)
    df = df.dropna(subset=["net_pnl"]).reset_index(drop=True)
    for c in ("engine", "symbol", "side", "exit_reason", "regime"):
        df[c] = df[c].astype(str).replace({"nan": "", "None": ""})
    return df


# ── per-trade loss triage ─────────────────────────────────────────────────────
def robust_z(x: float, hist: np.ndarray) -> float:
    if len(hist) < 8:
        return 0.0
    med = np.median(hist)
    mad = np.median(np.abs(hist - med))
    if mad == 0:
        return 0.0
    return float((x - med) / (1.4826 * mad))


def outlier_scores(df: pd.DataFrame) -> pd.Series:
    """Anomaly score per row vs its OWN engine's full history. IsolationForest when the engine
    has enough closes, robust z on net_pnl otherwise. Higher = more anomalous (0..1-ish)."""
    scores = pd.Series(0.0, index=df.index)
    for eng, grp in df.groupby("engine"):
        pnl = grp["net_pnl"].to_numpy()
        if len(grp) >= MIN_ENGINE_N_FOR_IFOREST:
            try:
                from sklearn.ensemble import IsolationForest
                feats = grp[["net_pnl", "mfe", "mae", "hold_sec"]].copy()
                for c in feats.columns:
                    feats[c] = pd.to_numeric(feats[c], errors="coerce")
                    feats[c] = feats[c].fillna(feats[c].median() if feats[c].notna().any() else 0.0)
                iso = IsolationForest(n_estimators=100, contamination="auto", random_state=0)
                iso.fit(feats)
                # decision_function: negative = anomalous. Map to 0..1.
                raw = -iso.decision_function(feats)
                rng = raw.max() - raw.min()
                scores.loc[grp.index] = (raw - raw.min()) / rng if rng > 0 else 0.0
                continue
            except Exception:  # noqa: BLE001
                pass
        for idx, val in zip(grp.index, pnl):
            z = abs(robust_z(val, pnl))
            scores.loc[idx] = min(z / (OUTLIER_Z * 2), 1.0)
    return scores


def triage_loss(row: pd.Series, df_all: pd.DataFrame, score: float) -> tuple[str, list[str], list[str]]:
    """Returns (verdict, error_flags, suggestions). verdict: ERROR-SUSPECT | GENUINE | REVIEW."""
    flags: list[str] = []
    sugg: list[str] = []
    reason = (row["exit_reason"] or "").upper()
    mfe, mae, hold = row["mfe"], row["mae"], row["hold_sec"]
    net = row["net_pnl"]
    is_companion = str(row["book"]).startswith("companion")

    # 1. duplicate booking (same engine+symbol+exit_ts+pnl appears >1x)
    dup = df_all[(df_all["engine"] == row["engine"]) & (df_all["symbol"] == row["symbol"])
                 & (df_all["exit_ts"] == row["exit_ts"]) & (df_all["net_pnl"] == net)]
    if len(dup) > 1:
        flags.append("DUPLICATE_BOOKING")
        sugg.append("FIX: same (engine,symbol,exit_ts,pnl) booked more than once — inspect the "
                    "writer for append-idempotency (cf. goldmimic triplication class, S-15q fix "
                    "pattern: dedup key + self-heal on load). Do NOT double-count in PnL.")

    # 2. profit-lock violation candidate: rode meaningful profit back to a LOSS
    if pd.notna(mfe) and mfe > GIVEBACK_MFE_MIN and net < 0 and not is_companion:
        flags.append("PROFIT_GIVEBACK_TO_LOSS")
        sugg.append(f"FIX/TUNE: trade peaked at mfe={mfe:.2f} then closed {net:+.2f} — a live book "
                    "must not ride profit back down (feedback-profit-lock-mandatory). Verify the "
                    "profit-lock giveback clamp is armed for this book ([PROFIT-LOCK-GATE]); if armed, "
                    "backtest-certify a tighter per-cell giveback g<1.0 (never uniform).")

    # 3. hold anomaly
    if pd.notna(hold) and hold < 0:
        flags.append("NEGATIVE_HOLD")
        sugg.append("FIX: exit_ts < entry_ts — timestamp/booking bug, not a market loss.")

    # 4. cost-only churn: never went favorable, tiny adverse, net ~ costs
    if pd.notna(mfe) and pd.notna(mae) and mfe <= 0 and abs(mae) <= abs(net) * 2 and net < 0:
        flags.append("COST_CHURN")
        sugg.append("TUNE: trade never went favorable and loss ≈ round-trip cost — candidate for a "
                    "stricter entry/cost gate (OmegaCostGuard threshold or BE-ENTRY confirm >= 2x RT "
                    "cost) on this engine. Backtest-certify before wiring.")

    # 5. slippage anomaly (live book only)
    slip = row.get("slippage_exit_pct")
    if pd.notna(slip) and slip > 0.05:
        flags.append("SLIPPAGE_ANOMALY")
        sugg.append(f"FIX: exit slippage {slip:.3%} is extreme — check venue/fill path and whether "
                    "the cost model (measured, per CryptoCostLedger/IBKR basis) still matches reality.")

    # 6. floored-book tail (honest, usually genuine) — any book whose exit path is a floor
    if "FLOOR" in reason or (is_companion and "CLIP" in reason):
        sugg.append("GENUINE (S-17f class): floored book booked an honest below-floor fill — the floor "
                    "reduces, not eliminates, the tail (gap/restart pierce). If it coincides with a "
                    "service restart window, avoid mid-market deploys with open legs; if recurring "
                    "without restarts, backtest a wider confirm or tighter reclip on this cell.")

    # 7. designed-stop exits
    if reason in ("LOSS_CUT", "SL", "SL_HIT", "STOP", "PREBE_CUT"):
        sugg.append("TUNE: designed stop fired. Candidates to backtest: LOSS_CUT_PCT width (read the "
                    "comment block above the config line first, per repo rule), BE-ENTRY conversion "
                    "(feedback-no-prebe-loss-ever), or a regime gate if losses cluster in one regime "
                    "(run mine_losses.py for the dimensional view).")
    elif reason == "TRAIL_STOP" and net < 0:
        sugg.append("TUNE: trail stop closed negative — trail may arm before BE is covered. Backtest "
                    "BE-floor-on-open recipe (confirm>=2xRT + anchor le=epx) for this engine.")

    # 8. statistical outlier vs own history
    if score >= 0.8:
        flags.append("STAT_OUTLIER")
        sugg.append(f"REVIEW: anomaly score {score:.2f} vs this engine's own history — loss is "
                    "statistically unusual in size/shape. Rule out data glitch (x1000 class, "
                    "data_integrity_gate.py) and booking error before treating as market loss.")

    # 9. companion/mimic reversal-exit into a loss — the DOMINANT class on the crypto ledger,
    #    which has no mfe/mae so rules 2/4 can't fire. ENGINE_EXIT on a companion = the mimic
    #    closed on its reversal signal net-negative (gave a favorable move back to a loss).
    if net < 0 and reason in ("ENGINE_EXIT", "REVERSAL", "REVERSAL_EXIT", "ENGINE_FLIP", "FLIP"):
        flags.append("REVERSAL_GIVEBACK")
        if is_companion:
            sugg.append("TUNE: companion REVERSAL-EXIT closed net-negative — the mimic rode a favorable "
                        "move then gave it back to a loss on its reversal signal. Backtest-certify (a) a "
                        "tighter per-cell giveback-lock g<1.0 so profit locks BEFORE the reversal "
                        "(feedback-profit-lock-mandatory; NEVER uniform — certify per cell, uniform 0.3-0.5 "
                        "killed 5/7 books) and (b) the reversal-exit threshold/timing. Judge the companion "
                        "STANDALONE, never vs WIDE (feedback-companion-independent-engine).")
        else:
            sugg.append("TUNE: engine reversal/flip exit closed net-negative — backtest a profit-lock "
                        "giveback clamp (g<1.0, per-cell) and the flip threshold before wiring "
                        "(feedback-profit-lock-mandatory).")

    if not sugg:
        sugg.append("REVIEW: no mapped rule for this (book, exit_reason) — the crypto ledger lacks "
                    "mfe/mae/regime so the giveback/cost/regime rules can't evaluate. Enrich the export "
                    "with those columns to unlock them; meanwhile run mine_losses.py for the dimensional "
                    "split and inspect the raw fill.")

    error_flags = [f for f in flags if f in ("DUPLICATE_BOOKING", "NEGATIVE_HOLD", "SLIPPAGE_ANOMALY")]
    verdict = ("ERROR-SUSPECT" if error_flags
               else ("REVIEW" if ("STAT_OUTLIER" in flags or "PROFIT_GIVEBACK_TO_LOSS" in flags)
                     else "GENUINE"))
    return verdict, flags, sugg


# ── win-side improvement miner ────────────────────────────────────────────────
def mine_wins(df: pd.DataFrame) -> list[dict]:
    out: list[dict] = []
    for (eng, book), grp in df.groupby(["engine", "book"]):
        wins = grp[grp["net_pnl"] > 0]
        if len(wins) < MIN_WINS_FOR_CANDIDACY:
            continue
        rec: dict = {"engine": eng, "book": book, "n": len(grp), "n_wins": len(wins),
                     "win_rate": float((grp["net_pnl"] > 0).mean()),
                     "net_sum": float(grp["net_pnl"].sum()), "ideas": []}
        # capture ratio: how much of peak favorable excursion did winners keep?
        wm = wins[pd.to_numeric(wins["mfe"], errors="coerce") > 0].copy()
        if len(wm) >= MIN_WINS_FOR_CANDIDACY:
            cap = (wm["net_pnl"] / wm["mfe"]).clip(-1, 1.5)
            med_cap = float(cap.median())
            rec["median_capture"] = med_cap
            if med_cap < CAPTURE_MIMIC_THRESHOLD:
                surrendered = float((wm["mfe"] - wm["net_pnl"]).sum())
                rec["ideas"].append(
                    f"MIMIC CANDIDATE: winners keep only {med_cap:.0%} of peak MFE (≈{surrendered:.1f} "
                    "surrendered across winners). Build a giveback-clip companion — SEPARATE additive "
                    "book, judged STANDALONE (never vs WIDE), floored-on-open from day one "
                    "(BE-ENTRY confirm>=2xRT + anchor + reclip anchored-or-off, BeFloorOnOpenFoundation). "
                    "Backtest-certify standalone net>0 after costs, WF both halves, both regimes.")
        # regime / session concentration of wins -> extension candidate
        for dim in ("regime", "symbol"):
            vals = wins[dim].replace("", np.nan).dropna()
            all_vals = grp[dim].replace("", np.nan).dropna()
            if len(vals) >= MIN_WINS_FOR_CANDIDACY and all_vals.nunique() > 1:
                top = vals.value_counts()
                if top.iloc[0] / len(vals) >= 0.75:
                    rec["ideas"].append(
                        f"CONCENTRATION: {top.iloc[0]}/{len(vals)} wins in {dim}={top.index[0]} — "
                        "if the engine is net-positive there, sweep EXTENSIONS (more symbols/regimes "
                        "with the same edge, per feedback-extend-winning-engines-standing-order) and "
                        "consider gating the weak cells rather than widening blindly.")
        # strong performer -> standing order
        if rec["net_sum"] > 0 and rec["win_rate"] >= 0.55 and len(grp) >= 12:
            rec["ideas"].append(
                "PASS-PATTERN: engine is forward net-positive with a solid win rate — standing order "
                "applies (feedback-extend-winning-engines-standing-order): sweep symbol/regime "
                "extensions + mimic/ladder/stacked-arm overlays with max protection; package what "
                "certifies as a standard engine addition.")
        if rec["ideas"]:
            out.append(rec)
    return out


# ── state ─────────────────────────────────────────────────────────────────────
def load_state() -> dict:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text())
        except Exception:  # noqa: BLE001
            pass
    return {"seen_keys": [], "last_run_utc": None}


def save_state(state: dict) -> None:
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(state))


# ── report ────────────────────────────────────────────────────────────────────
def fmt_ts(ts) -> str:
    try:
        return datetime.fromtimestamp(float(ts), tz=timezone.utc).strftime("%Y-%m-%d %H:%MZ")
    except Exception:  # noqa: BLE001
        return "?"


def cluster_guidance(book: str, reason: str, worst_eng: str, worst_net: float) -> str:
    """One certifiable PROPOSAL per (book, exit_reason) cluster. Never a fix — always 'backtest X'."""
    is_comp = str(book).startswith("companion")
    r = (reason or "UNKNOWN").upper()
    tail = f" Worst cell: `{worst_eng}` {worst_net:+.0f}."
    if r in ("ENGINE_EXIT", "REVERSAL", "REVERSAL_EXIT", "ENGINE_FLIP", "FLIP") and is_comp:
        return ("**Companion reversal-exit is the dominant $ drain here.** The mimics rode favorable "
                "moves then gave them back to losses on the reversal signal (a coordinated market "
                "reversal hits every cell at once). Backtest-certify, PER CELL: (1) a tighter giveback-lock "
                "g<1.0 so profit locks before the reversal (feedback-profit-lock-mandatory — uniform 0.3-0.5 "
                "killed 5/7 books, so certify each cell), (2) the reversal-exit threshold/timing. Judge each "
                "companion STANDALONE, never vs WIDE (feedback-companion-independent-engine)." + tail)
    if "FLOOR" in r or "CLIP" in r:
        return ("S-17f honest floored tails — the floor REDUCES not eliminates the gap/restart pierce. "
                "Individually tiny; act only if this aggregate keeps growing or the tails cluster in a "
                "service-restart window (then avoid mid-market deploys with open legs). If recurring "
                "WITHOUT restarts, backtest a wider confirm / tighter reclip on the worst cells." + tail)
    if r in ("LOSS_CUT", "SL", "SL_HIT", "STOP", "PREBE_CUT"):
        return ("Designed-stop cluster — backtest LOSS_CUT_PCT width (READ the config comment block above "
                "the line first, per repo rule) or a BE-ENTRY conversion (feedback-no-prebe-loss-ever). "
                "Run mine_losses.py for the regime/session split before wiring." + tail)
    if r in ("TRAIL_STOP", "TRAIL"):
        return ("Trail-stop cluster closing red — the trail may arm before BE is covered. Backtest the "
                "BE-floor-on-open recipe (confirm>=2xRT + anchor le=epx) for this engine." + tail)
    return ("No mapped fix pattern for this (book, exit_reason) yet. Run mine_losses.py for the "
            "dimensional (regime x weekday x symbol) split; enrich the ledger with mfe/mae/regime to "
            "unlock the giveback/cost/regime rules." + tail)


def aggregate_losses(triage: list[tuple]) -> list[dict]:
    """Cluster the triaged losses by (system, book, exit_reason) and rank by TOTAL $ lost.
    This is the 'act here first' view: 117 per-trade lines collapse into a handful of ranked
    systemic drivers, each with ONE certifiable proposal. Uses only always-present fields
    (net_pnl / system / book / exit_reason / symbol / engine) so it survives the crypto ledger's
    missing mfe/mae/regime."""
    from collections import defaultdict
    clusters: dict = defaultdict(lambda: {"n": 0, "sum": 0.0, "coins": defaultdict(float),
                                          "worst_eng": "", "worst_net": 0.0})
    for row, _verdict, _flags, _sugg, _score in triage:
        reason = (str(row["exit_reason"]) or "UNKNOWN").upper()
        key = (str(row["system"]), str(row["book"]), reason)
        c = clusters[key]
        net = float(row["net_pnl"])
        c["n"] += 1
        c["sum"] += net
        c["coins"][str(row["symbol"])] += net
        if net < c["worst_net"]:
            c["worst_net"] = net
            c["worst_eng"] = str(row["engine"])
    out = []
    for (system, book, reason), c in clusters.items():
        top = sorted(c["coins"].items(), key=lambda kv: kv[1])[:5]
        out.append({"system": system, "book": book, "reason": reason, "n": c["n"],
                    "sum": c["sum"], "avg": c["sum"] / c["n"] if c["n"] else 0.0,
                    "top_coins": top, "worst_eng": c["worst_eng"], "worst_net": c["worst_net"],
                    "guidance": cluster_guidance(book, reason, c["worst_eng"], c["worst_net"])})
    return sorted(out, key=lambda d: d["sum"])  # most negative total first


def render(new_neg: pd.DataFrame, triage: list[tuple], win_ideas: list[dict],
           n_new: int, n_total: int, out_path: Path) -> None:
    now = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    L: list[str] = []
    L.append(f"# Trade Sentinel Report — {now}")
    L.append("")
    L.append(f"Scanned {n_total} closed trades across both systems; **{n_new} new since last run**, "
             f"of which **{len(triage)} negative** (triaged below). Every suggestion is a PROPOSAL — "
             "backtest-certify before wiring anything (standing doctrine).")
    cov = globals().get("SYS_COVERAGE", "")
    if cov:
        L.append("")
        L.append(f"_Per-system coverage (closed trades pulled): {cov}._ An empty ledger means that "
                 "system had no closed trades in range — it is pulled every run, never skipped.")
    L.append("")
    if triage:
        clusters = aggregate_losses(triage)
        tot = sum(c["sum"] for c in clusters)
        L.append("## 📊 Loss clusters — ranked by $ impact (ACT HERE FIRST)")
        L.append("")
        L.append(f"The {len(triage)} negative trades collapse into **{len(clusters)} systemic "
                 f"driver(s)**, total **{tot:+.2f}**. Each carries ONE certifiable proposal — "
                 "backtest before wiring (nothing here is auto-applied).")
        L.append("")
        for c in clusters:
            coins = ", ".join(f"{s} {v:+.0f}" for s, v in c["top_coins"])
            L.append(f"### `{c['system']}/{c['book']}` · exit `{c['reason']}` — "
                     f"**{c['sum']:+.2f}** / {c['n']} trades (avg {c['avg']:+.2f})")
            L.append(f"- top coins by $: {coins}")
            L.append(f"- ➤ {c['guidance']}")
            L.append("")
        L.append("## 🔴 Per-trade triage (detail)")
        L.append("")
        for row, verdict, flags, sugg, score in triage:
            L.append(f"### {row['engine']} {row['symbol']} {row['side']} → **{row['net_pnl']:+.2f}** "
                     f"[{verdict}]")
            L.append(f"- book: `{row['system']}/{row['book']}` | exit: {fmt_ts(row['exit_ts'])} | "
                     f"reason: `{row['exit_reason']}` | entry_px: {row['entry_px']} | "
                     f"mfe: {row['mfe']} | mae: {row['mae']} | hold: {row['hold_sec']} | "
                     f"anomaly: {score:.2f}")
            if flags:
                L.append(f"- flags: {', '.join('`'+f+'`' for f in flags)}")
            for s in sugg:
                L.append(f"- ➤ {s}")
            L.append("")
    else:
        L.append("## ✅ No new negative trades since last run.")
        L.append("")
    if win_ideas:
        L.append("## 🟢 Win-improvement candidates (full-history view)")
        L.append("")
        for rec in win_ideas:
            cap = f", median_capture={rec['median_capture']:.0%}" if "median_capture" in rec else ""
            L.append(f"### {rec['engine']} (`{rec['book']}`) — n={rec['n']}, "
                     f"WR={rec['win_rate']:.0%}, net={rec['net_sum']:+.2f}{cap}")
            for idea in rec["ideas"]:
                L.append(f"- ➤ {idea}")
            L.append("")
    L.append("---")
    L.append("Sentinel = incremental per-trade triage. Deep dimensional pattern mining = "
             "`mine_losses.py` (weekly cron). Nothing here is auto-applied.")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(L))
    print(f"wrote {out_path}")


def notify_mac(n_neg: int, worst: float, report: Path) -> None:
    msg = f"{n_neg} new negative trade(s), worst {worst:+.2f} — {report.name}"
    subprocess.run(["osascript", "-e",
                    f'display notification "{msg}" with title "Omega Trade Sentinel"'],
                   capture_output=True)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", help="offline CSV instead of live ssh pull")
    ap.add_argument("--system", default="omega", help="system label for --csv mode")
    ap.add_argument("--full", action="store_true", help="ignore state; triage every negative trade")
    ap.add_argument("--notify", action="store_true", help="macOS notification when new negatives found")
    ap.add_argument("--out", default=None)
    ap.add_argument("--state", default=None, help="override state file path (tests)")
    args = ap.parse_args()

    global STATE_PATH
    if args.state:
        STATE_PATH = Path(args.state)

    df = load_all(args.csv, args.system)
    if df.empty:
        print("No data loaded. Nothing to triage.", file=sys.stderr)
        sys.exit(1)

    # both-systems coverage — surface which system actually contributed (operator: the tool must
    # cover BOTH; an empty Omega ledger is stated, never silently hidden behind a chimera-only report).
    sys_counts = df.groupby("system").size().to_dict()
    for s in ("omega", "chimera"):
        sys_counts.setdefault(s, 0)
    global SYS_COVERAGE
    SYS_COVERAGE = ", ".join(f"{s}={n}" for s, n in sorted(sys_counts.items()))

    state = load_state()
    seen = set(state["seen_keys"]) if not args.full else set()
    new_mask = ~df["key"].isin(seen)
    n_new = int(new_mask.sum())

    df["anomaly"] = outlier_scores(df)

    new_neg = df[new_mask & (df["net_pnl"] < 0)]
    triage = []
    for _, row in new_neg.sort_values("net_pnl").iterrows():
        verdict, flags, sugg = triage_loss(row, df, float(row["anomaly"]))
        triage.append((row, verdict, flags, sugg, float(row["anomaly"])))

    win_ideas = mine_wins(df)

    out_path = Path(args.out) if args.out else (
        REPO / "outputs" / f"TRADE_SENTINEL_REPORT_{datetime.now(timezone.utc).date().isoformat()}.md")
    render(new_neg, triage, win_ideas, n_new, len(df), out_path)

    if triage:
        ALERT_LOG.parent.mkdir(parents=True, exist_ok=True)
        with ALERT_LOG.open("a") as fh:
            for row, verdict, flags, _, score in triage:
                fh.write(f"{datetime.now(timezone.utc).isoformat()} {verdict} "
                         f"{row['system']}/{row['book']} {row['engine']} {row['symbol']} "
                         f"{row['net_pnl']:+.2f} reason={row['exit_reason']} "
                         f"flags={','.join(flags) or '-'} anomaly={score:.2f}\n")
        if args.notify:
            notify_mac(len(triage), float(new_neg["net_pnl"].min()), out_path)

    if not args.csv:  # never advance state on offline test data
        state["seen_keys"] = sorted(set(state["seen_keys"]) | set(df["key"].tolist()))
        state["last_run_utc"] = datetime.now(timezone.utc).isoformat()
        save_state(state)

    print(f"triage: {len(triage)} new negative | new trades {n_new}/{len(df)} | "
          f"win-idea engines: {len(win_ideas)}", file=sys.stderr)


if __name__ == "__main__":
    main()
