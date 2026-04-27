"""
edgefinder/analytics/cli.py
===========================

Pipeline orchestrator. Five subcommands, run in order:

    build-predicates  TRAIN-fit regime cuts + quantile cuts. Build the
                      predicate catalogue. Cache to disk. Cheap; safe to
                      re-run.

    scan-train        Compute TRAIN drift baseline. Score every catalogue
                      predicate on TRAIN against drift. Apply economic
                      filter + MTC. Cache to disk.

    gate-val          Re-score TRAIN survivors on VAL (against VAL drift).
                      Apply gate (sign match + min_n + pf > 1).

    probe-oos         Re-score VAL survivors on OOS (against OOS drift).
                      Single-touch, sentinel-protected.

                      Optional --candidates-file PATH overrides the default
                      val_survivors source. Used when an upstream selection
                      step (e.g. C4 inspect_c4) has produced a curated
                      candidate set distinct from the raw VAL survivors.

    report            Merge TRAIN + VAL + OOS prospect tables, write
                      edges_ranked.csv and edges_summary.md.

Persistence
-----------
Prospect tables are persisted as Parquet if pyarrow is available, else
pickle. The CLI tries Parquet first and falls back automatically. Both
formats are read transparently.
"""
from __future__ import annotations

import argparse
import json
import pickle
import sys
import time
from pathlib import Path

import numpy as np
import pandas as pd

from .load import load_panel
from .regime import (
    apply_regimes, build_predicate_catalogue, CatalogueArtifact,
    classify_features, fit_quantile_cuts, fit_regime_cuts,
    PredicateSpec,
)
from .prospect import (
    compute_drift_baseline, score_predicates, reapply_to_partition,
)
from .mtc import apply_mtc
from .walkforward import (
    DEFAULT_OOS_TO, DEFAULT_TRAIN_TO, DEFAULT_VAL_TO,
    OosAlreadyConsumed, PartitionBounds,
    gate_val_survivors,
    oos_partition, train_partition, val_partition,
)
from .report import write_edges_ranked, write_edges_summary


DEFAULT_PANEL = 'backtest/edgefinder/output/bars_xauusd_full.bin'
DEFAULT_OUTPUT_DIR = 'backtest/edgefinder/output'
DEFAULT_WORKDIR = 'backtest/edgefinder/output/work'

CATALOGUE_JSON = 'catalogue.json'
CATALOGUE_PICKLE = 'catalogue.pkl'
TRAIN_PROSPECTS_BASE = 'train_prospects'
VAL_SURVIVORS_BASE   = 'val_survivors'
OOS_SURVIVORS_BASE   = 'oos_survivors'
DRIFT_TRAIN_JSON = 'drift_train.json'

# Columns required on any candidate set fed into probe-oos.
REQUIRED_CANDIDATE_COLUMNS = ('pid', 'bracket_id', 'side')


def _ts() -> str:
    return time.strftime('%H:%M:%S')


# -----------------------------------------------------------------------------
# Persistence helpers (parquet → pickle fallback)
# -----------------------------------------------------------------------------
def _save_df(df: pd.DataFrame, base_path: Path) -> Path:
    """Try parquet, fall back to pickle. Returns the actual path written."""
    parquet_path = base_path.with_suffix('.parquet')
    pickle_path  = base_path.with_suffix('.pkl')
    try:
        df.to_parquet(parquet_path, index=False)
        # If both formats exist from a prior run, remove the stale pickle so
        # _load_df doesn't get confused.
        if pickle_path.exists():
            pickle_path.unlink()
        return parquet_path
    except (ImportError, ValueError) as e:
        # Parquet engine missing or column type unsupported: fall back to pickle.
        print(f"  [persist] parquet unavailable ({e}); using pickle", flush=True)
        df.to_pickle(pickle_path)
        if parquet_path.exists():
            parquet_path.unlink()
        return pickle_path


def _load_df(base_path: Path) -> pd.DataFrame:
    """Load whichever of parquet/pickle exists, preferring parquet."""
    parquet_path = base_path.with_suffix('.parquet')
    pickle_path  = base_path.with_suffix('.pkl')
    if parquet_path.exists():
        try:
            return pd.read_parquet(parquet_path)
        except (ImportError, ValueError) as e:
            print(f"  [persist] parquet read failed ({e}); trying pickle", flush=True)
    if pickle_path.exists():
        return pd.read_pickle(pickle_path)
    raise FileNotFoundError(f"neither {parquet_path} nor {pickle_path} exists")


def _exists_df(base_path: Path) -> bool:
    return (base_path.with_suffix('.parquet').exists() or
            base_path.with_suffix('.pkl').exists())


def _load_candidates_file(path: Path) -> pd.DataFrame:
    """
    Load an explicit candidates file by absolute or relative path.

    Accepts .parquet or .pkl. Unlike _load_df, this honors the exact suffix
    the caller specified so the audit trail is unambiguous.
    """
    if not path.exists():
        raise FileNotFoundError(f"candidates file not found: {path}")
    suffix = path.suffix.lower()
    if suffix == '.parquet':
        return pd.read_parquet(path)
    if suffix == '.pkl':
        return pd.read_pickle(path)
    raise ValueError(
        f"candidates file must end in .parquet or .pkl, got: {path.suffix}"
    )


def _drift_to_jsonable(d: dict[tuple[str, int], float]) -> dict:
    return {f"{side}|{b}": v for (side, b), v in d.items()}


def _drift_from_jsonable(d: dict) -> dict[tuple[str, int], float]:
    out = {}
    for k, v in d.items():
        side, b = k.split("|")
        out[(side, int(b))] = float(v)
    return out


# -----------------------------------------------------------------------------
# build-predicates
# -----------------------------------------------------------------------------
def cmd_build_predicates(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    print(f"[{_ts()}] panel rows={len(df)} cols={len(df.columns)}")

    bounds = PartitionBounds.from_iso(args.train_to, args.val_to, args.oos_to)
    df_train = train_partition(df, bounds)
    print(f"[{_ts()}] TRAIN rows: {len(df_train)} "
          f"({df_train.index.min()} .. {df_train.index.max()})")

    rcuts = fit_regime_cuts(df_train)
    print(f"[{_ts()}] regime cuts: vol_lo={rcuts.vol_lo:.4g} "
          f"vol_hi={rcuts.vol_hi:.4g} trend_down={rcuts.trend_down:.4g} "
          f"trend_up={rcuts.trend_up:.4g}")

    df_train_r = apply_regimes(df_train, rcuts)
    numeric, boolean = classify_features(df_train_r)
    print(f"[{_ts()}] features: {len(numeric)} numeric, {len(boolean)} boolean")

    qcuts = fit_quantile_cuts(df_train_r, numeric)
    print(f"[{_ts()}] fit quantile cuts for {len(qcuts)} numeric features")

    catalogue = build_predicate_catalogue(numeric, boolean, qcuts)
    print(f"[{_ts()}] catalogue: {len(catalogue)} predicates")

    art = CatalogueArtifact(
        regime_cuts=rcuts,
        quantile_cuts=qcuts,
        numeric_features=numeric,
        boolean_features=boolean,
        n_predicates=len(catalogue),
        train_to_iso=args.train_to,
        val_to_iso=args.val_to,
        oos_to_iso=args.oos_to,
    )
    art.save(workdir / CATALOGUE_JSON)
    with open(workdir / CATALOGUE_PICKLE, 'wb') as f:
        pickle.dump(catalogue, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(f"[{_ts()}] wrote {workdir / CATALOGUE_JSON}")
    print(f"[{_ts()}] wrote {workdir / CATALOGUE_PICKLE}")
    return 0


# -----------------------------------------------------------------------------
# scan-train
# -----------------------------------------------------------------------------
def cmd_scan_train(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    art = CatalogueArtifact.load(workdir / CATALOGUE_JSON)
    with open(workdir / CATALOGUE_PICKLE, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)

    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    bounds = PartitionBounds.from_iso(art.train_to_iso, art.val_to_iso, art.oos_to_iso)
    df_train = train_partition(df, bounds)
    df_train = apply_regimes(df_train, art.regime_cuts)
    print(f"[{_ts()}] TRAIN rows: {len(df_train)}")

    # Drift baseline on TRAIN
    drift = compute_drift_baseline(df_train)
    print(f"[{_ts()}] TRAIN drift baseline (LONG side):")
    for b in range(6):
        print(f"           bracket {b}: long={drift[('LONG',b)]:+.4f} "
              f"short={drift[('SHORT',b)]:+.4f}")
    (workdir / DRIFT_TRAIN_JSON).write_text(json.dumps(_drift_to_jsonable(drift), indent=2))

    print(f"[{_ts()}] scoring {len(catalogue)} predicates "
          f"(min_n={args.min_n}, drift-relative null)...")
    t0 = time.time()
    prospects = score_predicates(df_train, catalogue,
                                 min_n=args.min_n,
                                 drift_baseline=drift)
    elapsed = time.time() - t0
    print(f"[{_ts()}] scored in {elapsed:.1f}s, {len(prospects)} kept rows")

    # Save the raw scoring output BEFORE MTC, so a downstream MTC tweak does
    # not require rerunning the 18-min scan.
    raw_path = _save_df(prospects, workdir / (TRAIN_PROSPECTS_BASE + '_raw'))
    print(f"[{_ts()}] wrote {raw_path} (pre-MTC)")

    if prospects.empty:
        print(f"[{_ts()}] WARNING: no predicates passed min_n={args.min_n}")
        out = _save_df(prospects, workdir / TRAIN_PROSPECTS_BASE)
        print(f"[{_ts()}] wrote empty {out}")
        return 0

    print(f"[{_ts()}] applying economic filter "
          f"(min_excess_pts={args.min_excess_pts}, min_sharpe={args.min_sharpe}) "
          f"+ MTC (per-bracket)...")
    prospects = apply_mtc(
        prospects, alpha=args.alpha, per='bracket',
        min_excess_pts=args.min_excess_pts,
        min_sharpe=args.min_sharpe,
    )
    economic_pass  = int(prospects['economic_pass'].sum())
    survivors_bh   = int(prospects['survives_bh'].sum())
    survivors_bonf = int(prospects['survives_bonf'].sum())
    print(f"[{_ts()}] economic-pass: {economic_pass} of {len(prospects)} "
          f"({100*economic_pass/max(len(prospects),1):.1f}%)")
    print(f"[{_ts()}] BH survivors: {survivors_bh} | "
          f"Bonferroni survivors: {survivors_bonf}")

    out = _save_df(prospects, workdir / TRAIN_PROSPECTS_BASE)
    print(f"[{_ts()}] wrote {out}")
    return 0


# -----------------------------------------------------------------------------
# rescore-only — re-apply MTC to existing _raw prospects without rerunning
#                the 18-min scan. Useful for tweaking economic thresholds.
# -----------------------------------------------------------------------------
def cmd_rescore(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    raw_path_base = workdir / (TRAIN_PROSPECTS_BASE + '_raw')
    if not _exists_df(raw_path_base):
        print(f"ERROR: {raw_path_base}.{{parquet,pkl}} not found. "
              f"Run scan-train first.", file=sys.stderr)
        return 2
    prospects = _load_df(raw_path_base)
    print(f"[{_ts()}] loaded {len(prospects)} pre-MTC rows")
    print(f"[{_ts()}] applying economic filter "
          f"(min_excess_pts={args.min_excess_pts}, min_sharpe={args.min_sharpe}) "
          f"+ MTC...")
    prospects = apply_mtc(
        prospects, alpha=args.alpha, per='bracket',
        min_excess_pts=args.min_excess_pts,
        min_sharpe=args.min_sharpe,
    )
    economic_pass  = int(prospects['economic_pass'].sum())
    survivors_bh   = int(prospects['survives_bh'].sum())
    survivors_bonf = int(prospects['survives_bonf'].sum())
    print(f"[{_ts()}] economic-pass: {economic_pass}")
    print(f"[{_ts()}] BH survivors: {survivors_bh} | Bonferroni: {survivors_bonf}")
    out = _save_df(prospects, workdir / TRAIN_PROSPECTS_BASE)
    print(f"[{_ts()}] wrote {out}")
    return 0


# -----------------------------------------------------------------------------
# gate-val
# -----------------------------------------------------------------------------
def cmd_gate_val(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    art = CatalogueArtifact.load(workdir / CATALOGUE_JSON)
    with open(workdir / CATALOGUE_PICKLE, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)
    catalogue_by_pid = {s.pid: s for s in catalogue}

    train_prospects = _load_df(workdir / TRAIN_PROSPECTS_BASE)
    if 'survives_bh' not in train_prospects.columns:
        print("ERROR: train_prospects missing MTC columns. Run scan-train first.",
              file=sys.stderr)
        return 2

    train_survivors = train_prospects[
        (train_prospects['survives_bh']) | (train_prospects['survives_bonf'])
    ].copy()
    print(f"[{_ts()}] TRAIN survivors going to VAL: {len(train_survivors)}")

    if train_survivors.empty:
        empty = train_survivors.copy()
        empty['expectancy_train'] = []
        empty['gate_passed'] = []
        out = _save_df(empty, workdir / VAL_SURVIVORS_BASE)
        print(f"[{_ts()}] no survivors; wrote empty {out}")
        return 0

    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    bounds = PartitionBounds.from_iso(art.train_to_iso, art.val_to_iso, art.oos_to_iso)
    df_val = val_partition(df, bounds)
    df_val = apply_regimes(df_val, art.regime_cuts)
    print(f"[{_ts()}] VAL rows: {len(df_val)}")

    val_drift = compute_drift_baseline(df_val)
    print(f"[{_ts()}] VAL drift baseline (LONG side):")
    for b in range(6):
        print(f"           bracket {b}: long={val_drift[('LONG',b)]:+.4f}")

    val_prospects = reapply_to_partition(df_val, train_survivors,
                                         catalogue_by_pid,
                                         drift_baseline=val_drift)
    print(f"[{_ts()}] reapplied to VAL: {len(val_prospects)} rows")

    val_survivors = gate_val_survivors(
        val_prospects, train_survivors,
        min_n_val=args.min_n_val,
        require_sign_match=True,
        require_pf_gt_one=True,
    )
    print(f"[{_ts()}] VAL gate survivors: {len(val_survivors)}")

    out = _save_df(val_survivors, workdir / VAL_SURVIVORS_BASE)
    print(f"[{_ts()}] wrote {out}")
    return 0


# -----------------------------------------------------------------------------
# probe-oos
# -----------------------------------------------------------------------------
def cmd_probe_oos(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    art = CatalogueArtifact.load(workdir / CATALOGUE_JSON)
    with open(workdir / CATALOGUE_PICKLE, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)
    catalogue_by_pid = {s.pid: s for s in catalogue}

    # ------------------------------------------------------------------
    # Candidate-set selection: explicit --candidates-file overrides the
    # default workdir/val_survivors source. Schema must still satisfy the
    # reapply_to_partition contract (pid + bracket_id + side).
    # ------------------------------------------------------------------
    if args.candidates_file:
        cand_path = Path(args.candidates_file)
        print(f"[{_ts()}] loading candidates from explicit path: {cand_path}")
        val_survivors = _load_candidates_file(cand_path)
        candidates_source = str(cand_path.resolve())
    else:
        print(f"[{_ts()}] loading candidates from default: "
              f"{workdir / VAL_SURVIVORS_BASE}.{{parquet,pkl}}")
        val_survivors = _load_df(workdir / VAL_SURVIVORS_BASE)
        candidates_source = str((workdir / VAL_SURVIVORS_BASE).resolve())

    if val_survivors.empty:
        print(f"[{_ts()}] no candidates ({candidates_source}); OOS step skipped.")
        out = _save_df(val_survivors, workdir / OOS_SURVIVORS_BASE)
        return 0

    # Validate required columns up-front, before we burn the OOS sentinel.
    missing_cols = [c for c in REQUIRED_CANDIDATE_COLUMNS
                    if c not in val_survivors.columns]
    if missing_cols:
        print(f"[{_ts()}] ERROR: candidates file missing required columns: "
              f"{missing_cols}. Source: {candidates_source}",
              file=sys.stderr)
        return 2

    # Validate every pid exists in the catalogue. A pid missing here means
    # the candidates file came from a different catalogue build than the
    # one in workdir — running OOS would silently produce empty rows.
    cand_pids = set(int(p) for p in val_survivors['pid'].unique())
    cat_pids = set(catalogue_by_pid.keys())
    orphan_pids = cand_pids - cat_pids
    if orphan_pids:
        print(f"[{_ts()}] ERROR: {len(orphan_pids)} candidate pids absent from "
              f"catalogue. Catalogue/candidates mismatch — refusing to burn OOS.",
              file=sys.stderr)
        print(f"           sample orphan pids: {sorted(orphan_pids)[:10]}",
              file=sys.stderr)
        print(f"           candidates source: {candidates_source}", file=sys.stderr)
        print(f"           catalogue source:  {workdir / CATALOGUE_PICKLE}",
              file=sys.stderr)
        return 2

    print(f"[{_ts()}] candidates going to OOS: {len(val_survivors)} rows, "
          f"{len(cand_pids)} unique pids")

    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    bounds = PartitionBounds.from_iso(art.train_to_iso, art.val_to_iso, art.oos_to_iso)

    output_dir = Path(args.output_dir)
    try:
        df_oos = oos_partition(df, bounds, output_dir, force=args.force)
    except OosAlreadyConsumed as e:
        print(f"[{_ts()}] REFUSED: {e}", file=sys.stderr)
        return 3
    df_oos = apply_regimes(df_oos, art.regime_cuts)
    print(f"[{_ts()}] OOS rows: {len(df_oos)} "
          f"({df_oos.index.min() if len(df_oos) else 'n/a'} .. "
          f"{df_oos.index.max() if len(df_oos) else 'n/a'})")

    oos_drift = compute_drift_baseline(df_oos)
    print(f"[{_ts()}] OOS drift baseline (LONG side):")
    for b in range(6):
        print(f"           bracket {b}: long={oos_drift[('LONG',b)]:+.4f}")

    oos_prospects = reapply_to_partition(df_oos, val_survivors,
                                         catalogue_by_pid,
                                         drift_baseline=oos_drift)
    print(f"[{_ts()}] OOS prospects: {len(oos_prospects)}")

    out = _save_df(oos_prospects, workdir / OOS_SURVIVORS_BASE)
    print(f"[{_ts()}] wrote {out}")
    print(f"[{_ts()}] candidates source recorded: {candidates_source}")
    return 0


# -----------------------------------------------------------------------------
# report
# -----------------------------------------------------------------------------
def cmd_report(args: argparse.Namespace) -> int:
    workdir = Path(args.workdir)
    output_dir = Path(args.output_dir)

    with open(workdir / CATALOGUE_PICKLE, 'rb') as f:
        catalogue: list[PredicateSpec] = pickle.load(f)
    catalogue_by_pid = {s.pid: s for s in catalogue}

    train_prospects = _load_df(workdir / TRAIN_PROSPECTS_BASE)

    val_path = workdir / VAL_SURVIVORS_BASE
    val_prospects = _load_df(val_path) if _exists_df(val_path) else None

    oos_path = workdir / OOS_SURVIVORS_BASE
    oos_prospects = _load_df(oos_path) if _exists_df(oos_path) else None
    oos_consumed = (output_dir / '.oos_consumed').exists()

    ranked = write_edges_ranked(
        train_prospects=train_prospects,
        val_prospects=val_prospects,
        oos_prospects=oos_prospects,
        catalogue_by_pid=catalogue_by_pid,
        out_path=output_dir / 'edges_ranked.csv',
    )
    print(f"[{_ts()}] wrote {output_dir / 'edges_ranked.csv'} ({len(ranked)} rows)")

    write_edges_summary(
        ranked=ranked,
        out_path=output_dir / 'edges_summary.md',
        top_k=args.top_k,
        note_no_oos=not oos_consumed,
    )
    print(f"[{_ts()}] wrote {output_dir / 'edges_summary.md'}")
    return 0


# -----------------------------------------------------------------------------
# main / argparse
# -----------------------------------------------------------------------------
def _add_common(p: argparse.ArgumentParser) -> None:
    p.add_argument('--panel', default=DEFAULT_PANEL,
                   help='binary panel file produced by OmegaEdgeFinderExtract')
    p.add_argument('--workdir', default=DEFAULT_WORKDIR,
                   help='intermediate artefact directory')
    p.add_argument('--output-dir', default=DEFAULT_OUTPUT_DIR,
                   help='final output directory (also stores OOS sentinel)')


def _add_mtc_args(p: argparse.ArgumentParser) -> None:
    p.add_argument('--alpha', type=float, default=0.05)
    p.add_argument('--min-excess-pts', type=float, default=1.0,
                   help='minimum |excess_expectancy| (in points) to pass economic filter')
    p.add_argument('--min-sharpe', type=float, default=0.05,
                   help='minimum |sharpe| to pass economic filter')


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog='edgefinder')
    sp = p.add_subparsers(dest='cmd', required=True)

    p_build = sp.add_parser('build-predicates')
    _add_common(p_build)
    p_build.add_argument('--train-to', default=DEFAULT_TRAIN_TO)
    p_build.add_argument('--val-to',   default=DEFAULT_VAL_TO)
    p_build.add_argument('--oos-to',   default=DEFAULT_OOS_TO)
    p_build.set_defaults(func=cmd_build_predicates)

    p_scan = sp.add_parser('scan-train')
    _add_common(p_scan)
    p_scan.add_argument('--min-n', type=int, default=1000)
    _add_mtc_args(p_scan)
    p_scan.set_defaults(func=cmd_scan_train)

    p_rescore = sp.add_parser('rescore-train',
                              help='re-apply MTC to cached _raw scoring (no rescan)')
    _add_common(p_rescore)
    _add_mtc_args(p_rescore)
    p_rescore.set_defaults(func=cmd_rescore)

    p_val = sp.add_parser('gate-val')
    _add_common(p_val)
    p_val.add_argument('--min-n-val', type=int, default=50)
    p_val.set_defaults(func=cmd_gate_val)

    p_oos = sp.add_parser('probe-oos')
    _add_common(p_oos)
    p_oos.add_argument('--force', action='store_true',
                       help='override OOS sentinel (audit-stamped)')
    p_oos.add_argument('--candidates-file', default=None,
                       help='explicit path to candidate set (.parquet or .pkl) '
                            'overriding the default workdir/val_survivors source. '
                            'Used when an upstream selection step has produced a '
                            'curated candidate set distinct from raw VAL survivors.')
    p_oos.set_defaults(func=cmd_probe_oos)

    p_rep = sp.add_parser('report')
    _add_common(p_rep)
    p_rep.add_argument('--top-k', type=int, default=20)
    p_rep.set_defaults(func=cmd_report)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
