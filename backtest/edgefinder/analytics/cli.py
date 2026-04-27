"""
edgefinder/analytics/cli.py
===========================

Pipeline orchestrator. Five subcommands, run in order:

    build-predicates  TRAIN-fit regime cuts + quantile cuts. Build the
                      predicate catalogue. Cache to disk. Cheap; safe to
                      re-run.

    scan-train        Score every catalogue predicate on the TRAIN
                      partition. Apply MTC. Cache to disk. The slow step
                      (~tens of minutes for ~90k predicates).

    gate-val          Re-score TRAIN survivors on VAL. Apply gate
                      (sign match + min_n + pf > 1). Cache to disk.

    probe-oos         Re-score VAL survivors on OOS. THIS IS THE SINGLE-
                      TOUCH STEP. Sentinel file enforced.

    report            Merge TRAIN + VAL + OOS prospect tables, write
                      edges_ranked.csv and edges_summary.md.

Run as:

    python -m backtest.edgefinder.analytics.cli build-predicates --panel ...
    python -m backtest.edgefinder.analytics.cli scan-train
    python -m backtest.edgefinder.analytics.cli gate-val
    python -m backtest.edgefinder.analytics.cli probe-oos
    python -m backtest.edgefinder.analytics.cli report

All intermediate artefacts live under --workdir (default
backtest/edgefinder/output/work/).
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
    score_predicates, reapply_to_partition,
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
CATALOGUE_PICKLE = 'catalogue.pkl'    # PredicateSpec list
TRAIN_PROSPECTS = 'train_prospects.parquet'
VAL_SURVIVORS   = 'val_survivors.parquet'
OOS_SURVIVORS   = 'oos_survivors.parquet'


def _ts() -> str:
    return time.strftime('%H:%M:%S')


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

    # 1. Regime cuts (vol terciles, trend terciles).
    rcuts = fit_regime_cuts(df_train)
    print(f"[{_ts()}] regime cuts: vol_lo={rcuts.vol_lo:.4g} "
          f"vol_hi={rcuts.vol_hi:.4g} trend_down={rcuts.trend_down:.4g} "
          f"trend_up={rcuts.trend_up:.4g}")

    # 2. Classify features.
    df_train_r = apply_regimes(df_train, rcuts)
    numeric, boolean = classify_features(df_train_r)
    print(f"[{_ts()}] features: {len(numeric)} numeric, {len(boolean)} boolean")

    # 3. Quantile cuts on TRAIN.
    qcuts = fit_quantile_cuts(df_train_r, numeric)
    print(f"[{_ts()}] fit quantile cuts for {len(qcuts)} numeric features")

    # 4. Build catalogue.
    catalogue = build_predicate_catalogue(numeric, boolean, qcuts)
    print(f"[{_ts()}] catalogue: {len(catalogue)} predicates")

    # 5. Persist.
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

    print(f"[{_ts()}] scoring {len(catalogue)} predicates (min_n={args.min_n})...")
    t0 = time.time()
    prospects = score_predicates(df_train, catalogue, min_n=args.min_n)
    elapsed = time.time() - t0
    print(f"[{_ts()}] scored in {elapsed:.1f}s, {len(prospects)} kept rows")

    if prospects.empty:
        print(f"[{_ts()}] WARNING: no predicates passed min_n={args.min_n}")
        prospects.to_parquet(workdir / TRAIN_PROSPECTS, index=False)
        return 0

    print(f"[{_ts()}] applying MTC (per-bracket)...")
    prospects = apply_mtc(prospects, alpha=args.alpha, per='bracket')
    survivors_bh   = int(prospects['survives_bh'].sum())
    survivors_bonf = int(prospects['survives_bonf'].sum())
    print(f"[{_ts()}] BH survivors: {survivors_bh} | Bonferroni survivors: {survivors_bonf}")

    prospects.to_parquet(workdir / TRAIN_PROSPECTS, index=False)
    print(f"[{_ts()}] wrote {workdir / TRAIN_PROSPECTS}")
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

    train_prospects = pd.read_parquet(workdir / TRAIN_PROSPECTS)
    if 'survives_bh' not in train_prospects.columns:
        print("ERROR: train_prospects missing MTC columns. Run scan-train first.",
              file=sys.stderr)
        return 2

    # Take everything that survived EITHER BH OR Bonferroni — we'll let
    # the report distinguish later.
    train_survivors = train_prospects[
        (train_prospects['survives_bh']) | (train_prospects['survives_bonf'])
    ].copy()
    print(f"[{_ts()}] TRAIN survivors going to VAL: {len(train_survivors)}")

    if train_survivors.empty:
        empty_cols = list(train_prospects.columns) + ['expectancy_train', 'gate_passed']
        empty = pd.DataFrame(columns=empty_cols)
        empty.to_parquet(workdir / VAL_SURVIVORS, index=False)
        print(f"[{_ts()}] no survivors; wrote empty {workdir / VAL_SURVIVORS}")
        return 0

    print(f"[{_ts()}] loading panel: {args.panel}")
    df = load_panel(args.panel)
    bounds = PartitionBounds.from_iso(art.train_to_iso, art.val_to_iso, art.oos_to_iso)
    df_val = val_partition(df, bounds)
    df_val = apply_regimes(df_val, art.regime_cuts)
    print(f"[{_ts()}] VAL rows: {len(df_val)}")

    val_prospects = reapply_to_partition(df_val, train_survivors, catalogue_by_pid)
    print(f"[{_ts()}] reapplied to VAL: {len(val_prospects)} rows")

    val_survivors = gate_val_survivors(
        val_prospects, train_survivors,
        min_n_val=args.min_n_val,
        require_sign_match=True,
        require_pf_gt_one=True,
    )
    print(f"[{_ts()}] VAL gate survivors: {len(val_survivors)}")

    val_survivors.to_parquet(workdir / VAL_SURVIVORS, index=False)
    print(f"[{_ts()}] wrote {workdir / VAL_SURVIVORS}")
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

    val_survivors = pd.read_parquet(workdir / VAL_SURVIVORS)
    if val_survivors.empty:
        print(f"[{_ts()}] no VAL survivors; OOS step skipped.")
        empty = pd.DataFrame(columns=val_survivors.columns)
        empty.to_parquet(workdir / OOS_SURVIVORS, index=False)
        return 0

    print(f"[{_ts()}] VAL survivors going to OOS: {len(val_survivors)}")

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

    oos_prospects = reapply_to_partition(df_oos, val_survivors, catalogue_by_pid)
    print(f"[{_ts()}] OOS prospects: {len(oos_prospects)}")

    oos_prospects.to_parquet(workdir / OOS_SURVIVORS, index=False)
    print(f"[{_ts()}] wrote {workdir / OOS_SURVIVORS}")
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

    train_prospects = pd.read_parquet(workdir / TRAIN_PROSPECTS)

    val_path = workdir / VAL_SURVIVORS
    val_prospects = pd.read_parquet(val_path) if val_path.exists() else None

    oos_path = workdir / OOS_SURVIVORS
    oos_prospects = pd.read_parquet(oos_path) if oos_path.exists() else None
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
    p_scan.add_argument('--alpha', type=float, default=0.05)
    p_scan.set_defaults(func=cmd_scan_train)

    p_val = sp.add_parser('gate-val')
    _add_common(p_val)
    p_val.add_argument('--min-n-val', type=int, default=50)
    p_val.set_defaults(func=cmd_gate_val)

    p_oos = sp.add_parser('probe-oos')
    _add_common(p_oos)
    p_oos.add_argument('--force', action='store_true',
                       help='override OOS sentinel (audit-stamped)')
    p_oos.set_defaults(func=cmd_probe_oos)

    p_rep = sp.add_parser('report')
    _add_common(p_rep)
    p_rep.add_argument('--top-k', type=int, default=20)
    p_rep.set_defaults(func=cmd_report)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
