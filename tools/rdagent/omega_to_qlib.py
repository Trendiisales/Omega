#!/usr/bin/env python3
"""
Omega bars -> qlib dataset (Path B adapter).

Converts Omega OHLCV bar data into a qlib binary dataset so RD-Agent can mine
factors and predict on YOUR universe (gold / index / big-cap) instead of CSI300.
Wraps the official qlib `dump_bin.py` (vendored next to this file).

Input: a directory of per-symbol CSVs, OR one combined CSV. Required columns
(case-insensitive): date, open, high, low, close, volume. A `symbol` column is
required for a combined CSV; for per-symbol files the filename stem is the symbol.
An optional `factor` column (adjustment) defaults to 1.0.

Output: a qlib provider dir (default ~/.qlib/qlib_data/omega_data) with an
`all`/`omega` instruments universe, ready for `provider_uri` in an RD-Agent conf.

NOTE: this only does the mechanical bar->bin conversion. It does NOT impose
Omega's faithful cost model — any factor/model RD-Agent finds on this data is
still a CANDIDATE that must clear Omega's faithful tick BT before trading.

Usage:
    python omega_to_qlib.py --input <dir-of-csvs|combined.csv> \
        --freq day --out ~/.qlib/qlib_data/omega_data --universe omega
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import pandas as pd

HERE = Path(__file__).resolve().parent
FIELDS = ["open", "high", "low", "close", "volume"]


def _norm_one(df: pd.DataFrame, symbol: str) -> pd.DataFrame:
    df = df.rename(columns={c: c.lower().strip() for c in df.columns})
    # tolerate common aliases (incl. Omega's epoch-seconds `ts`)
    aliases = {"datetime": "date", "time": "date", "timestamp": "date", "ts": "date", "vol": "volume"}
    df = df.rename(columns={k: v for k, v in aliases.items() if k in df.columns})
    missing = {"date", *FIELDS} - set(df.columns)
    if missing:
        raise SystemExit(f"{symbol}: missing columns {sorted(missing)} (have {list(df.columns)})")
    df["symbol"] = symbol
    if "factor" not in df.columns:
        df["factor"] = 1.0
    # numeric date column => epoch seconds (Omega `ts`); else parse normally
    if pd.api.types.is_numeric_dtype(df["date"]):
        df["date"] = pd.to_datetime(df["date"], unit="s")
    else:
        df["date"] = pd.to_datetime(df["date"])
    df["date"] = df["date"].dt.strftime("%Y-%m-%d")
    return df[["symbol", "date", *FIELDS, "factor"]]


def _collect(input_path: Path) -> pd.DataFrame:
    if input_path.is_file():
        raw = pd.read_csv(input_path)
        raw.columns = [c.lower().strip() for c in raw.columns]
        if "symbol" not in raw.columns:
            raise SystemExit("combined CSV needs a `symbol` column")
        return pd.concat(
            [_norm_one(g.drop(columns=["symbol"]), str(sym)) for sym, g in raw.groupby("symbol")],
            ignore_index=True,
        )
    csvs = sorted(input_path.glob("*.csv"))
    if not csvs:
        raise SystemExit(f"no CSVs under {input_path}")
    # filename stem -> symbol, stripping Omega timeframe suffixes (AAPL_d1 -> AAPL)
    import re as _re

    def _sym(stem: str) -> str:
        return _re.sub(r"_(d1|15m|5m|30m|1h|h1|h4|m30)$", "", stem, flags=_re.I)

    return pd.concat([_norm_one(pd.read_csv(f), _sym(f.stem)) for f in csvs], ignore_index=True)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", default=str(Path.home() / ".qlib" / "qlib_data" / "omega_data"))
    ap.add_argument("--freq", default="day")
    ap.add_argument("--universe", default="omega")
    a = ap.parse_args()

    data = _collect(Path(a.input).expanduser())
    syms = sorted(data["symbol"].unique())
    print(f"normalized {len(data)} rows across {len(syms)} symbols: {syms[:8]}{'...' if len(syms) > 8 else ''}")

    with tempfile.TemporaryDirectory() as tmp:
        # dump_bin expects one CSV per symbol in a directory
        tmp = Path(tmp)
        for sym, g in data.groupby("symbol"):
            g.drop(columns=["symbol"]).to_csv(tmp / f"{sym}.csv", index=False)
        cmd = [
            sys.executable, str(HERE / "dump_bin.py"), "dump_all",
            "--data_path", str(tmp),
            "--qlib_dir", str(Path(a.out).expanduser()),
            "--freq", a.freq,
            "--date_field_name", "date",
            "--symbol_field_name", "symbol",  # per-file mode keys symbol off filename
            "--include_fields", ",".join(FIELDS + ["factor"]),
        ]
        print("running qlib dump_bin ...")
        subprocess.check_call(cmd)

    # write a named universe = all symbols (full history span)
    out = Path(a.out).expanduser()
    inst_dir = out / "instruments"
    inst_dir.mkdir(parents=True, exist_ok=True)
    span = data.groupby("symbol")["date"].agg(["min", "max"])
    lines = [f"{s.upper()}\t{r['min']}\t{r['max']}" for s, r in span.iterrows()]
    (inst_dir / f"{a.universe}.txt").write_text("\n".join(lines) + "\n")
    # also ensure an 'all' alias exists
    if not (inst_dir / "all.txt").exists():
        (inst_dir / "all.txt").write_text("\n".join(lines) + "\n")
    print(f"done -> {out}")
    print(f"  provider_uri: {out}")
    print(f"  market/universe: {a.universe} ({len(syms)} names)")


if __name__ == "__main__":
    main()
