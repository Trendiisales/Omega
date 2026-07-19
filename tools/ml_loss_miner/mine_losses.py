#!/usr/bin/env python3
"""ML loss miner — finds patterns in negative-pnl trades across Omega + Chimera and proposes
certifiable mitigations. Never auto-wires a fix: every proposal must be backtest-certified
(ENGINE_BACKTEST_REGISTRY.md / BACKTEST_TRUTH_CRYPTO.md) before touching engine config or code.

Complements tools/analytics/ledger_analytics.py (fixed-metric rule-based diagnostics: capture
ratio, expectancy, MAE-p90, cost%, Sharpe/Sortino/Calmar; Omega-only). This tool instead LEARNS
which dimension combinations (exit_reason x regime x weekday x symbol) predict a loss, across
BOTH systems, via an interpretable decision tree + groupby fallback for small samples — it finds
patterns you wouldn't think to hardcode a fixed metric for. Run both; they answer different
questions ("how efficient is this engine" vs "what specifically correlates with its losses").

Data sources (pulled read-only, per feedback-audit-read-only-never-mutate):
  Omega   ssh omega-new  C:\\Omega\\logs\\trades\\omega_trade_closes.csv (+ daily variants)
                          C:\\Omega\\logs\\shadow\\omega_shadow.csv
  Chimera ssh chimera-direct  ~/ChimeraCrypto/data/live_trades.csv (ledger of record, S-20f)
                              + chimera_inbound.csv (legacy, truncated)

Usage:
  python3 tools/ml_loss_miner/mine_losses.py --system omega
  python3 tools/ml_loss_miner/mine_losses.py --system chimera
  python3 tools/ml_loss_miner/mine_losses.py --system both
  python3 tools/ml_loss_miner/mine_losses.py --csv path/to/trades.csv --system omega   # offline/backtest export

Requires the rdagent4qlib conda env (has sklearn+pandas already):
  /opt/homebrew/Caskroom/miniforge/base/envs/rdagent4qlib/bin/python tools/ml_loss_miner/mine_losses.py ...
"""
from __future__ import annotations
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

MIN_ROWS_FOR_TREE = 40       # below this, a tree overfits noise -> fall back to groupby stats
MIN_ROWS_FOR_STATS = 8       # below this, don't report a rate at all (too few to mean anything)
LOW_CONFIDENCE_N = 100       # tree results under this n get a low-confidence flag in the report

OMEGA_SSH_HOST = "omega-new"
OMEGA_LEDGER_PATHS = [
    r"C:\Omega\logs\trades\omega_trade_closes.csv",
    r"C:\Omega\logs\shadow\omega_shadow.csv",
]
CHIMERA_SSH_HOST = "chimera-direct"
CHIMERA_LEDGER_PATH = "~/ChimeraCrypto/data/chimera_inbound.csv"   # LEGACY (truncated S-20f)
CHIMERA_LIVE_LEDGER_PATH = "~/ChimeraCrypto/data/live_trades.csv"  # ledger of record since S-20f


# ── data loading ──────────────────────────────────────────────────────────────
def _ssh_cat(host: str, path: str) -> str | None:
    """Read-only remote file pull. No mutating commands, ever (feedback-audit-read-only-never-mutate)."""
    is_windows = path.startswith("C:") or "\\" in path
    if is_windows:
        cmd = ["ssh", host, f'powershell -Command "Get-Content -Raw \'{path}\'"']
    else:
        cmd = ["ssh", host, f"cat {path}"]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except Exception as e:  # noqa: BLE001
        print(f"  WARN: ssh {host}:{path} failed: {e}", file=sys.stderr)
        return None
    if out.returncode != 0 or not out.stdout.strip():
        return None
    return out.stdout


def load_omega() -> pd.DataFrame:
    frames = []
    for path in OMEGA_LEDGER_PATHS:
        raw = _ssh_cat(OMEGA_SSH_HOST, path)
        if not raw:
            print(f"  skip (unreachable/empty): {path}", file=sys.stderr)
            continue
        from io import StringIO
        df = pd.read_csv(StringIO(raw))
        df["_source"] = path.split("\\")[-1]
        frames.append(df)
    if not frames:
        return pd.DataFrame()
    df = pd.concat(frames, ignore_index=True, sort=False)
    # unify pnl column name across live (net_pnl) and shadow (pnl)
    if "net_pnl" not in df.columns and "pnl" in df.columns:
        df["net_pnl"] = df["pnl"]
    if "entry_ts_unix" not in df.columns and "ts_unix" in df.columns:
        df["entry_ts_unix"] = df["ts_unix"]
    # drop synthetic boot/heartbeat rows (shadow ledger writes a boot_writetest sentinel)
    if "exit_reason" in df.columns:
        df = df[~df["exit_reason"].astype(str).str.contains("boot_writetest", case=False, na=False)]
    if "side" in df.columns:
        df = df[df["side"].astype(str).str.upper() != "HEARTBEAT"]
    df["_system"] = "omega"
    return df.reset_index(drop=True)


def load_chimera() -> pd.DataFrame:
    from io import StringIO
    frames = []
    for path in (CHIMERA_LIVE_LEDGER_PATH, CHIMERA_LEDGER_PATH):
        raw = _ssh_cat(CHIMERA_SSH_HOST, path)
        if not raw or len(raw.strip().splitlines()) < 2:  # missing or header-only
            print(f"  skip (unreachable/empty): {path}", file=sys.stderr)
            continue
        df = pd.read_csv(StringIO(raw))
        if "realized_usd" in df.columns:
            # live_trades.csv (S-20f): exit_ts_ms,entry_ts_ms,coin,tag,qty,entry_px,exit_px,realized_usd,reason
            df["net_pnl"] = df["realized_usd"]
            df["sym"] = df.get("coin")
            df["strat"] = "LiveMimic"
            df["entry_ts"] = pd.to_numeric(df.get("entry_ts_ms"), errors="coerce") / 1000.0
            df["exit_ts"] = pd.to_numeric(df.get("exit_ts_ms"), errors="coerce") / 1000.0
        elif "net_pnl" not in df.columns:
            for cand in ("pnl_usd", "pnl", "net_usd"):
                if cand in df.columns:
                    df["net_pnl"] = df[cand]
                    break
        frames.append(df)
    if not frames:
        return pd.DataFrame()
    df = pd.concat(frames, ignore_index=True, sort=False)
    df["_system"] = "chimera"
    return df.reset_index(drop=True)


def load_csv(path: str, system: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    if "net_pnl" not in df.columns:
        for cand in ("pnl_usd", "pnl", "net_usd", "net_bp"):
            if cand in df.columns:
                df["net_pnl"] = df[cand]
                break
    df["_system"] = system
    return df


# ── featurization ─────────────────────────────────────────────────────────────
def featurize(df: pd.DataFrame) -> pd.DataFrame:
    df = df.copy()
    if "net_pnl" not in df.columns:
        raise ValueError("no pnl column found (expected net_pnl/pnl/pnl_usd)")
    df["net_pnl"] = pd.to_numeric(df["net_pnl"], errors="coerce")
    df = df.dropna(subset=["net_pnl"])
    df["is_loss"] = (df["net_pnl"] < 0).astype(int)

    ts_col = None
    for cand in ("entry_ts_unix", "ts_unix", "entry_ts"):
        if cand in df.columns:
            ts_col = cand
            break
    if ts_col:
        ts = pd.to_numeric(df[ts_col], errors="coerce")
        dt = pd.to_datetime(ts, unit="s", errors="coerce", utc=True)
        df["hour_utc"] = dt.dt.hour
        df["weekday_utc"] = dt.dt.day_name()
    else:
        df["hour_utc"] = np.nan
        df["weekday_utc"] = None

    for col in ("mfe", "mae", "hold_sec", "spread_at_entry", "latency_ms"):
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors="coerce")
        else:
            df[col] = np.nan

    # giveback: how much of the favorable excursion was surrendered by exit (only meaningful when mfe>0)
    if "mfe" in df.columns:
        df["mfe_giveback"] = np.where(
            df["mfe"] > 0, (df["mfe"] - df["net_pnl"]) / df["mfe"].replace(0, np.nan), np.nan
        )

    for col in ("engine", "symbol", "side", "exit_reason", "regime"):
        if col not in df.columns:
            df[col] = None
        df[col] = df[col].astype(str).replace({"nan": None, "": None})

    return df


# ── mining ─────────────────────────────────────────────────────────────────────
def groupby_loss_rates(df: pd.DataFrame, by: str) -> pd.DataFrame:
    if by not in df.columns:
        return pd.DataFrame()
    g = df.dropna(subset=[by]).groupby(by).agg(
        n=("is_loss", "size"),
        loss_rate=("is_loss", "mean"),
        net_pnl_sum=("net_pnl", "sum"),
        net_pnl_mean=("net_pnl", "mean"),
    ).reset_index()
    g = g[g["n"] >= MIN_ROWS_FOR_STATS]
    return g.sort_values("loss_rate", ascending=False)


def mine_engine(df_eng: pd.DataFrame, engine_name: str) -> dict:
    """Per-engine analysis: tree rules if n is large enough, else simple groupby stats."""
    n = len(df_eng)
    result = {"engine": engine_name, "n": n, "loss_rate": float(df_eng["is_loss"].mean()),
              "net_pnl_sum": float(df_eng["net_pnl"].sum()), "method": None,
              "findings": [], "low_confidence": n < LOW_CONFIDENCE_N}

    if n < MIN_ROWS_FOR_STATS:
        result["method"] = "insufficient_data"
        return result

    if n < MIN_ROWS_FOR_TREE:
        result["method"] = "groupby_stats"
        for by in ("exit_reason", "regime", "weekday_utc", "symbol"):
            g = groupby_loss_rates(df_eng, by)
            if g.empty:
                continue
            baseline = df_eng["is_loss"].mean()
            worst = g.iloc[0]
            if worst["loss_rate"] > baseline + 0.15 and worst["n"] >= MIN_ROWS_FOR_STATS:
                result["findings"].append({
                    "type": "groupby", "dimension": by, "value": worst[by],
                    "n": int(worst["n"]), "loss_rate": float(worst["loss_rate"]),
                    "baseline_loss_rate": float(baseline), "net_pnl_sum": float(worst["net_pnl_sum"]),
                })
        return result

    # enough rows for an interpretable tree
    from sklearn.tree import DecisionTreeClassifier, export_text
    from sklearn.preprocessing import LabelEncoder

    # NOTE: mfe_giveback is EXCLUDED from tree features — it is computed from net_pnl
    # ((mfe - net_pnl)/mfe), i.e. a function of the target; including it is leakage and
    # makes the tree trivially "predict" losses. It remains available as a report-side stat.
    feat_cols_num = [c for c in ("hour_utc", "hold_sec", "mfe", "mae",
                                  "spread_at_entry", "latency_ms") if c in df_eng.columns]
    feat_cols_cat = [c for c in ("exit_reason", "regime", "weekday_utc", "side", "symbol")
                      if c in df_eng.columns]

    X_parts = []
    feat_names = []
    for c in feat_cols_num:
        vals = df_eng[c].fillna(df_eng[c].median() if df_eng[c].notna().any() else 0.0)
        X_parts.append(vals.to_numpy().reshape(-1, 1))
        feat_names.append(c)
    encoders = {}
    for c in feat_cols_cat:
        vals = df_eng[c].fillna("__missing__")
        if vals.nunique() < 2:
            continue
        le = LabelEncoder()
        X_parts.append(le.fit_transform(vals).reshape(-1, 1))
        feat_names.append(c)
        encoders[c] = le

    if not X_parts:
        result["method"] = "no_usable_features"
        return result

    X = np.hstack(X_parts)
    y = df_eng["is_loss"].to_numpy()
    if len(np.unique(y)) < 2:
        result["method"] = "no_variance"  # all wins or all losses -- nothing to split on
        return result

    clf = DecisionTreeClassifier(max_depth=3, min_samples_leaf=max(5, n // 15), random_state=0)
    clf.fit(X, y)
    result["method"] = "decision_tree"
    result["tree_accuracy"] = float(clf.score(X, y))
    result["feature_importances"] = sorted(
        zip(feat_names, clf.feature_importances_.tolist()), key=lambda t: -t[1]
    )
    result["tree_rules"] = export_text(clf, feature_names=feat_names)

    # translate the top-importance leaf paths into readable groupby-style findings too,
    # so the report reads the same way whether it came from a tree or simple stats
    for by in feat_cols_cat:
        g = groupby_loss_rates(df_eng, by)
        if g.empty:
            continue
        baseline = df_eng["is_loss"].mean()
        worst = g.iloc[0]
        if worst["loss_rate"] > baseline + 0.15 and worst["n"] >= MIN_ROWS_FOR_STATS:
            result["findings"].append({
                "type": "groupby", "dimension": by, "value": worst[by],
                "n": int(worst["n"]), "loss_rate": float(worst["loss_rate"]),
                "baseline_loss_rate": float(baseline), "net_pnl_sum": float(worst["net_pnl_sum"]),
            })
    return result


# ── mitigation proposals (NEVER auto-applied — proposal text only) ─────────────
MITIGATION_MAP = {
    "exit_reason": {
        "LOSS_CUT": "Pre-arm loss-cut is firing disproportionately here. Candidates to backtest: "
                    "widen LOSS_CUT_PCT (per-engine, see CLAUDE.md LOSS_CUT/BE_ARM/BE_BUFFER comment-block "
                    "rule), or move to BE-ENTRY (confirm_bp >= 2x round-trip cost) so the leg never opens "
                    "into a position that can trigger this cut pre-BE (feedback-no-prebe-loss-ever).",
        "TRAIL_STOP": "Trail giveback may be too tight for this engine's volatility. Backtest a giveback "
                      "sweep (mimic_giveback / wide_gb_frac) before retuning.",
        "PREBE_CUT": "Pre-BE cut is booking real losses — this engine may need the BE-floor-on-open "
                     "foundation recipe (confirm>=2xcost + anchor + reclip-off, see BeFloorOnOpenFoundation).",
    },
    "regime": "Loss concentration in one regime value suggests a missing or mistuned regime gate "
              "(feedback-companion-bull-gate-not-reject: gate new windows in the adverse regime, "
              "don't reject the engine outright). Backtest a regime-conditional entry filter.",
    "weekday_utc": "Loss concentration on one weekday may indicate a session/news-calendar effect "
                   "(e.g. weekend-gap carry, NFP/FOMC days). Backtest a day-of-week or event-calendar "
                   "filter (see WEEKEND_RISK_LAYERS_FINDINGS.md for the existing weekend-gap precedent).",
    "symbol": "Loss concentration in one symbol may be a data-quality or venue-cost issue rather than a "
              "strategy flaw — check data_integrity_gate.py on that symbol's feed and confirm the "
              "round-trip cost assumption is current before touching engine logic.",
}


def propose(finding: dict) -> str:
    dim = finding["dimension"]
    if dim == "exit_reason":
        return MITIGATION_MAP["exit_reason"].get(
            finding["value"],
            f"Exit reason '{finding['value']}' over-indexes on losses — inspect the exit path for this "
            f"engine before retuning any threshold.",
        )
    return MITIGATION_MAP.get(dim, "No mapped mitigation pattern for this dimension — manual review needed.")


# ── report ──────────────────────────────────────────────────────────────────────
def render_report(all_results: list[dict], out_path: Path) -> None:
    lines = []
    lines.append("# ML Loss Mining Report")
    lines.append("")
    lines.append("Generated by `tools/ml_loss_miner/mine_losses.py`. **Every finding below is a "
                  "PROPOSAL, not a fix.** Per project standing doctrine (ENGINE_BACKTEST_REGISTRY.md / "
                  "BACKTEST_TRUTH_CRYPTO.md / feedback-test-operator-spec-before-verdict), no threshold, "
                  "gate, or config change gets wired without a certified faithful backtest first. This "
                  "tool mines the FORWARD ledger for candidate loss patterns worth backtesting — it does "
                  "not itself certify anything.")
    lines.append("")

    for r in all_results:
        lines.append(f"## {r['engine']}  (n={r['n']}, loss_rate={r['loss_rate']:.1%}, "
                      f"net_pnl={r['net_pnl_sum']:+.2f})")
        if r["low_confidence"]:
            lines.append(f"> ⚠️ LOW CONFIDENCE — only {r['n']} closed trades. Treat any pattern below as "
                          f"a hypothesis to watch, not a conclusion. Re-run as more forward data accrues.")
        lines.append(f"- method: `{r['method']}`")
        if r["method"] == "insufficient_data":
            lines.append(f"- fewer than {MIN_ROWS_FOR_STATS} closed trades — nothing statistically "
                          f"meaningful to report yet.")
            lines.append("")
            continue
        if r["method"] == "decision_tree":
            lines.append(f"- tree self-accuracy: {r['tree_accuracy']:.1%} (fit quality, not "
                          f"out-of-sample — small forward samples, read directionally only)")
            lines.append("- top features by importance: " +
                          ", ".join(f"{name} ({imp:.2f})" for name, imp in r["feature_importances"][:4]))
            lines.append("<details><summary>full tree rules</summary>\n\n```")
            lines.append(r["tree_rules"])
            lines.append("```\n</details>")
        if r["findings"]:
            lines.append("")
            lines.append("**Surfaced loss-driver patterns + proposed next step (backtest-certify before wiring):**")
            lines.append("")
            for f in r["findings"]:
                lines.append(f"- `{f['dimension']}={f['value']}`: {f['n']} trades, "
                              f"{f['loss_rate']:.1%} loss rate (vs {f['baseline_loss_rate']:.1%} baseline), "
                              f"net {f['net_pnl_sum']:+.2f}")
                lines.append(f"  - **proposal:** {propose(f)}")
        else:
            lines.append("- no dimension over-indexed on losses by >15pp vs baseline at current sample size.")
        lines.append("")

    out_path.write_text("\n".join(lines))
    print(f"wrote {out_path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--system", choices=["omega", "chimera", "both"], default="both")
    ap.add_argument("--csv", help="offline CSV (backtest export or downloaded ledger) instead of live ssh pull")
    ap.add_argument("--out", default=None, help="output report path (default: outputs/ML_LOSS_MINING_REPORT_<date>.md)")
    args = ap.parse_args()

    frames = []
    if args.csv:
        frames.append(load_csv(args.csv, args.system if args.system != "both" else "offline"))
    else:
        if args.system in ("omega", "both"):
            print("Pulling Omega ledger (read-only ssh)...", file=sys.stderr)
            frames.append(load_omega())
        if args.system in ("chimera", "both"):
            print("Pulling Chimera ledger (read-only ssh)...", file=sys.stderr)
            frames.append(load_chimera())

    frames = [f for f in frames if not f.empty]
    if not frames:
        print("No data loaded from any source. Nothing to mine.", file=sys.stderr)
        sys.exit(1)

    df = pd.concat(frames, ignore_index=True, sort=False)
    df = featurize(df)
    print(f"Loaded {len(df)} closed trades across {df['_system'].nunique()} system(s).", file=sys.stderr)

    engine_col = "engine" if "engine" in df.columns else None
    results = []
    if engine_col:
        for name, grp in df.groupby(engine_col):
            if name is None or name == "None":
                continue
            results.append(mine_engine(grp, name))
    results.append(mine_engine(df, "__ALL_ENGINES_COMBINED__"))
    results.sort(key=lambda r: r["net_pnl_sum"])  # worst-losing engines first

    from datetime import date
    out_path = Path(args.out) if args.out else Path("outputs") / f"ML_LOSS_MINING_REPORT_{date.today().isoformat()}.md"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    render_report(results, out_path)


if __name__ == "__main__":
    main()
