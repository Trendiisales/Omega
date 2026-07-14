#!/usr/bin/env python3
"""close_csv_guard — content tripwire for every sp500_long_close.csv writer.

WHY (S-2026-07-14q incident): refresh_close_ibkr.py used ONE reqId (9000) for
every reqHistoricalData call, never cancelled on timeout, and didn't filter
callbacks by reqId — a symbol that timed out kept streaming its bars into the
NEXT symbol's buffer. CRM directly precedes ADBE in BIGCAP, so ADBE's column
became CRM's series for 251 rows (a full '1 Y' pull). The GUI then marked ADBE
at CRM's price: row showed -$835 on a position that was really +$174, and a
paper fill was booked at a price ADBE never traded.

WHAT: before any writer persists the frame, refuse column pairs whose recent
values are identical — two distinct tickers closing identically to the cent for
8+ consecutive sessions does not happen; it is the signature of a cross-wired
pull (IBKR reqId bleed, yfinance throttle aliasing). The writer must ABORT
WITHOUT WRITING (last-good CSV stays; staleness monitors then flag the feed),
never "clean up" the frame silently.

Import from the sibling dir (works on Mac and on the VPS checkout):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from close_csv_guard import assert_no_aliased_columns
"""
from __future__ import annotations

TAIL = 20        # rows inspected at the end of the frame
MIN_OVERLAP = 8  # identical non-null closes needed to call two columns aliased


def find_aliased_columns(df, tail: int = TAIL, min_overlap: int = MIN_OVERLAP):
    """Return list of column groups whose last `tail` rows are identical.

    Only groups with >= min_overlap shared non-null values count — thin/new
    columns can't trigger it, and a one-day price coincidence (which real
    tickers do produce) can't either.
    """
    t = df.tail(tail)
    sig = {}
    for c in t.columns:
        col = t[c].dropna()
        if len(col) < min_overlap:
            continue
        key = tuple(zip(col.index.astype(str), col.round(4)))
        sig.setdefault(key, []).append(c)
    return [cols for cols in sig.values() if len(cols) > 1]


def assert_no_aliased_columns(df, context: str = ""):
    """Raise SystemExit(4) naming the pairs if any aliased column group exists."""
    dups = find_aliased_columns(df)
    if dups:
        raise SystemExit(
            f"[close-csv-guard]{context} ALIASED COLUMNS {dups} -- two tickers "
            f"identical over >= {MIN_OVERLAP} sessions = cross-wired pull "
            "(S-2026-07-14q ADBE<-CRM). NOT WRITING; last-good CSV kept. "
            "Root-cause the puller before retrying."
        )
