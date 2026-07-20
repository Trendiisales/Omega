#!/usr/bin/env python
"""refresh_qlib_yf.py — DATA-ONLY qlib OHLCV pull for the bigcap universe via yfinance.

Fallback for qlib_refresh.sh when the IBKR-4002 handshake is dead (port open but
API not accepting sessions -> "no handshake" -> thin pull -> basket froze silently).
Operator rule (feedback-no-bulk-pulls-production-gateway): never bulk-pull the live
exec gateway; use a data-only source. yfinance on the ~23-name bigcap set is a tiny,
reliable batch (unlike the 744-name refresh_close_yf bulk that chokes on Mac DNS).

Writes one <SYM>.csv (date,open,high,low,close,volume) per name into --out, in the
exact shape omega_to_qlib.py ingests. Prints a summary line; exit 0 only if >= MIN
names came back fresh so the caller can gate on it (no silent partial dumps).
"""
import argparse
import sys
from pathlib import Path

import yfinance as yf

MIN_NAMES = 20  # same floor as qlib_refresh.sh's IBKR branch


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--universe", required=True,
                    help="path to qlib instruments file (SYMBOL per line, first col)")
    ap.add_argument("--out", required=True, help="output dir for per-symbol CSVs")
    ap.add_argument("--period", default="60d", help="yfinance lookback (default 60d)")
    a = ap.parse_args()

    syms = [ln.split()[0].strip() for ln in Path(a.universe).read_text().splitlines()
            if ln.strip()]
    if not syms:
        print("[refresh_qlib_yf] empty universe", flush=True)
        return 3
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)

    # single batched download (threads) — small set, reliable.
    df = yf.download(syms, period=a.period, interval="1d", auto_adjust=True,
                     progress=False, threads=True, group_by="ticker")

    written, last_dates = 0, []
    for s in syms:
        try:
            sub = df[s] if len(syms) > 1 else df
            sub = sub.dropna(subset=["Close"])
            if len(sub) < 30:
                continue
            lines = ["date,open,high,low,close,volume"]
            for ts, row in sub.iterrows():
                d = ts.strftime("%Y-%m-%d")
                vol = int(row["Volume"]) if row["Volume"] == row["Volume"] and row["Volume"] > 0 else 1
                lines.append(f"{d},{row['Open']:.4f},{row['High']:.4f},"
                             f"{row['Low']:.4f},{row['Close']:.4f},{vol}")
            (out / f"{s}.csv").write_text("\n".join(lines) + "\n")
            written += 1
            last_dates.append(sub.index[-1].strftime("%Y-%m-%d"))
        except Exception as e:  # noqa: BLE001 -- skip a bad name, keep the batch
            print(f"[refresh_qlib_yf] {s} skipped: {e}", flush=True)

    newest = max(last_dates) if last_dates else "none"
    print(f"[refresh_qlib_yf] wrote {written}/{len(syms)} names through {newest}", flush=True)
    return 0 if written >= MIN_NAMES else 4


if __name__ == "__main__":
    sys.exit(main())
