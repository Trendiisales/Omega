"""
edgefinder/analytics/load.py
============================

Loads the binary panel produced by OmegaEdgeFinderExtract into a pandas
DataFrame. The on-disk layout is documented in extractor/PanelSchema.hpp.

This module is the SINGLE point where the C++ <-> Python schema correspondence
lives. If you change PanelSchema.hpp, you must change PANEL_DTYPE here.
"""
from __future__ import annotations

import os
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np
import pandas as pd

# -----------------------------------------------------------------------------
# Schema constants — must mirror PanelSchema.hpp
# -----------------------------------------------------------------------------
PANEL_SCHEMA_VERSION = 1
PANEL_HEADER_BYTES   = 64
N_BRACKETS           = 6
N_FWD_RET            = 5
N_FIRST_TOUCH        = 3

# NumPy structured dtype mirroring PanelRow byte-for-byte.
# Order, names, and widths must match the struct exactly. uint8_t is 'u1',
# int32_t is 'i4', int64_t is 'i8', double is 'f8'.
# Pad fields use the same names as the C++ struct so the numpy fields cover
# the entire row size (otherwise numpy will report a size mismatch).
PANEL_DTYPE = np.dtype([
    # ---- timestamps ----
    ('ts_close_ms',          '<i8'),
    ('utc_hour',              '<i4'),
    ('utc_minute_of_day',     '<i4'),
    ('dow',                   '<i4'),
    ('dom',                   '<i4'),
    ('yday',                  '<i4'),
    ('session',               'u1'),
    ('_pad_session',          'u1', 3),

    # ---- bar-internal ----
    ('open',                  '<f8'),
    ('high',                  '<f8'),
    ('low',                   '<f8'),
    ('close',                 '<f8'),
    ('bar_range_pts',         '<f8'),
    ('bar_body_pts',          '<f8'),
    ('bar_upper_wick_pts',    '<f8'),
    ('bar_lower_wick_pts',    '<f8'),
    ('bar_direction',         '<i4'),
    ('tick_count',            '<i4'),
    ('spread_median_pts',     '<f8'),
    ('spread_max_pts',        '<f8'),

    # ---- trailing technicals ----
    ('ema_9',                 '<f8'),
    ('ema_21',                '<f8'),
    ('ema_50',                '<f8'),
    ('ema_200',               '<f8'),
    ('ema_9_minus_50',        '<f8'),
    ('ema_50_slope',          '<f8'),
    ('rsi_14',                '<f8'),
    ('atr_14',                '<f8'),
    ('atr_50',                '<f8'),
    ('range_20bar_hi',        '<f8'),
    ('range_20bar_lo',        '<f8'),
    ('range_20bar_position',  '<f8'),
    ('bb_upper',              '<f8'),
    ('bb_lower',              '<f8'),
    ('bb_position',           '<f8'),
    ('vol_60bar_stddev',      '<f8'),
    ('vol_5bar_stddev',       '<f8'),
    ('vol_5_vs_60_ratio',     '<f8'),

    # ---- session/structural ----
    ('session_open_price',    '<f8'),
    ('session_open_dist_pts', '<f8'),
    ('session_high',          '<f8'),
    ('session_low',           '<f8'),
    ('session_range_pts',     '<f8'),
    ('session_position',      '<f8'),
    ('vwap_session',          '<f8'),
    ('vwap_dist_pts',         '<f8'),
    ('vwap_z',                '<f8'),
    ('pdh',                   '<f8'),
    ('pdl',                   '<f8'),
    ('above_pdh',             'u1'),
    ('below_pdl',             'u1'),
    ('_pad_pdhl',             'u1', 2),
    ('dist_to_pdh_pts',       '<f8'),
    ('dist_to_pdl_pts',       '<f8'),
    ('asian_hi',              '<f8'),
    ('asian_lo',              '<f8'),
    ('asian_range_pts',       '<f8'),
    ('asian_built',           'u1'),
    ('above_asian_hi',        'u1'),
    ('below_asian_lo',        'u1'),
    ('_pad_asian',            'u1', 1),

    # ---- recent-move / pattern ----
    ('ret_1bar_pts',          '<f8'),
    ('ret_5bar_pts',          '<f8'),
    ('ret_15bar_pts',         '<f8'),
    ('ret_60bar_pts',         '<f8'),
    ('consecutive_up_bars',   '<i4'),
    ('consecutive_down_bars', '<i4'),
    ('nr4',                   'u1'),
    ('nr7',                   'u1'),
    ('inside_bar',            'u1'),
    ('outside_bar',           'u1'),
    ('gap_from_prev_close',   '<f8'),

    # ---- transitions ----
    ('cross_above_pdh',       'u1'),
    ('cross_below_pdl',       'u1'),
    ('cross_above_asian_hi',  'u1'),
    ('cross_below_asian_lo',  'u1'),
    ('cross_above_vwap',      'u1'),
    ('cross_below_vwap',      'u1'),
    ('ema_9_50_bull_cross',   'u1'),
    ('ema_9_50_bear_cross',   'u1'),
    ('enter_bb_upper',        'u1'),
    ('enter_bb_lower',        'u1'),
    ('_pad_xs',               'u1', 6),

    # ---- forward returns ----
    ('fwd_ret_1m_pts',        '<f8'),
    ('fwd_ret_5m_pts',        '<f8'),
    ('fwd_ret_15m_pts',       '<f8'),
    ('fwd_ret_60m_pts',       '<f8'),
    ('fwd_ret_240m_pts',      '<f8'),

    ('first_touch_5m',        '<i4'),
    ('first_touch_15m',       '<i4'),
    ('first_touch_60m',       '<i4'),

    ('fwd_bracket_pts',       '<f8', N_BRACKETS),
    ('fwd_bracket_outcome',   '<i4', N_BRACKETS),

    # ---- quality flags ----
    ('warmed_up',             'u1'),
    ('fwd_complete',          'u1'),
    ('_pad_tail',             'u1', 6),
])

# Bracket spec metadata, mirrored from PanelSchema.hpp BRACKETS[].
BRACKET_SPECS = [
    {'horizon_min':   5, 'sl_pts':  10.0, 'tp_pts':  20.0},
    {'horizon_min':  15, 'sl_pts':  20.0, 'tp_pts':  50.0},
    {'horizon_min':  15, 'sl_pts':  30.0, 'tp_pts':  60.0},
    {'horizon_min':  60, 'sl_pts':  50.0, 'tp_pts': 100.0},
    {'horizon_min':  60, 'sl_pts': 100.0, 'tp_pts': 200.0},
    {'horizon_min': 240, 'sl_pts': 100.0, 'tp_pts': 300.0},
]

SESSION_NAMES = ['ASIAN', 'LONDON', 'OVERLAP', 'NY_AM', 'NY_PM']


@dataclass
class PanelMeta:
    schema_version: int
    rows: int
    row_size: int
    file_size: int
    path: Path


def _parse_header(hdr: bytes) -> tuple[int, int]:
    """Parse the 64-byte ASCII header. Returns (schema_version, row_count_or_-1)."""
    if len(hdr) != PANEL_HEADER_BYTES:
        raise ValueError(f"header must be {PANEL_HEADER_BYTES} bytes, got {len(hdr)}")
    text = hdr.decode('ascii', errors='strict').rstrip()
    # Format: "EDGEFINDER_PANEL_VNN ROWS=NNNN..."
    if not text.startswith('EDGEFINDER_PANEL_V'):
        raise ValueError(f"bad magic in header: {text[:32]!r}")
    parts = text.split()
    ver = int(parts[0][len('EDGEFINDER_PANEL_V'):])
    rows = -1
    for tok in parts[1:]:
        if tok.startswith('ROWS='):
            try:
                rows = int(tok[5:])
            except ValueError:
                rows = -1
    return ver, rows


def panel_meta(path: str | os.PathLike) -> PanelMeta:
    """Read just the header and return metadata; cheap, no body parse."""
    p = Path(path)
    file_size = p.stat().st_size
    with open(p, 'rb') as f:
        hdr = f.read(PANEL_HEADER_BYTES)
    ver, rows_hdr = _parse_header(hdr)
    body = file_size - PANEL_HEADER_BYTES
    if body % PANEL_DTYPE.itemsize != 0:
        raise ValueError(
            f"panel body size {body} not divisible by row size {PANEL_DTYPE.itemsize}; "
            f"schema mismatch?"
        )
    rows_inferred = body // PANEL_DTYPE.itemsize
    if rows_hdr >= 0 and rows_hdr != rows_inferred:
        # Header says one count but body says another — trust the body.
        pass
    return PanelMeta(
        schema_version=ver,
        rows=rows_inferred,
        row_size=PANEL_DTYPE.itemsize,
        file_size=file_size,
        path=p,
    )


def load_panel(path: str | os.PathLike,
               from_ts_ms: Optional[int] = None,
               to_ts_ms:   Optional[int] = None,
               drop_padding: bool = True) -> pd.DataFrame:
    """
    Load the panel file into a pandas DataFrame.

    Parameters
    ----------
    path
        Binary panel file written by OmegaEdgeFinderExtract.
    from_ts_ms, to_ts_ms
        Optional timestamp filters (epoch ms). If both are None, all rows.
    drop_padding
        Drop the _pad_* columns from the output frame (default True).
    """
    meta = panel_meta(path)
    if meta.schema_version != PANEL_SCHEMA_VERSION:
        raise ValueError(
            f"panel schema v{meta.schema_version} does not match loader v{PANEL_SCHEMA_VERSION}; "
            f"re-extract with the matching binary, or update load.py"
        )
    arr = np.fromfile(meta.path, dtype=PANEL_DTYPE, offset=PANEL_HEADER_BYTES)
    if arr.shape[0] != meta.rows:
        raise RuntimeError(f"row count mismatch: meta={meta.rows} array={arr.shape[0]}")

    if from_ts_ms is not None or to_ts_ms is not None:
        mask = np.ones(len(arr), dtype=bool)
        if from_ts_ms is not None:
            mask &= arr['ts_close_ms'] >= from_ts_ms
        if to_ts_ms is not None:
            mask &= arr['ts_close_ms'] <  to_ts_ms
        arr = arr[mask]

    # Convert to DataFrame. Multi-element fields (fwd_bracket_pts, etc.) are
    # promoted to per-bracket columns so downstream code can do:
    #   df['fwd_bracket_pts_0'], df['fwd_bracket_pts_1'], ...
    series_dict = {}
    for name in PANEL_DTYPE.names:
        if drop_padding and name.startswith('_pad'):
            continue
        col = arr[name]
        if col.ndim == 2:
            for i in range(col.shape[1]):
                series_dict[f"{name}_{i}"] = col[:, i]
        else:
            series_dict[name] = col

    df = pd.DataFrame(series_dict)

    # Convert ts_close_ms to a Pandas Timestamp index for easy time slicing.
    df['ts_close'] = pd.to_datetime(df['ts_close_ms'], unit='ms', utc=True)
    df.set_index('ts_close', inplace=True)

    # Map session enum to readable string for downstream groupby's.
    df['session_name'] = pd.Categorical.from_codes(
        df['session'].clip(0, len(SESSION_NAMES)-1).astype(int),
        categories=SESSION_NAMES,
    )

    return df


def partition_train_val_oos(df: pd.DataFrame,
                            train_to_iso='2025-12-31T23:59:59',
                            val_to_iso  ='2026-01-31T23:59:59',
                            oos_to_iso  ='2026-04-30T23:59:59') -> tuple[pd.DataFrame, pd.DataFrame, pd.DataFrame]:
    """
    Slice the panel into the three partitions defined in the design doc.
    Boundaries are inclusive of the day shown.
    """
    train_to = pd.Timestamp(train_to_iso, tz='UTC')
    val_to   = pd.Timestamp(val_to_iso,   tz='UTC')
    oos_to   = pd.Timestamp(oos_to_iso,   tz='UTC')

    train = df[df.index <= train_to]
    val   = df[(df.index > train_to) & (df.index <= val_to)]
    oos   = df[(df.index > val_to)   & (df.index <= oos_to)]
    return train, val, oos


if __name__ == '__main__':
    # Quick CLI sanity: print schema info and (if path given) panel meta.
    import sys
    print(f"PANEL_DTYPE.itemsize = {PANEL_DTYPE.itemsize}")
    print(f"  fields = {len(PANEL_DTYPE.names)}")
    if len(sys.argv) > 1:
        m = panel_meta(sys.argv[1])
        print(f"  path   = {m.path}")
        print(f"  ver    = {m.schema_version}")
        print(f"  rows   = {m.rows}")
        print(f"  row sz = {m.row_size}")
        print(f"  file sz= {m.file_size}")
