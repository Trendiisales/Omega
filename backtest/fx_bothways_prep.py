#!/usr/bin/env python3
"""fx_bothways_prep.py — derived-data prep for the FX both-ways sweep (S-2026-07-08, research only).

Builds gate-able H1 CSVs (ts,o,h,l,c; ts epoch s) from raw Tick sources:
  1. NZDUSD_2025_h1.csv  — rebuilt from /Users/jo/Tick/NZDUSD/DAT_ASCII_NZDUSD_T_2025*.csv
     (histdata ticks "YYYYMMDD HHMMSSmmm,bid,ask,vol", naive ts used as-is like the
     befloor files; mid=(bid+ask)/2). Reason: NZDUSD_befloor_h1.csv is gate-REJECTED
     (90d hole Jan-Mar 2026); 2025 tick months are complete, 2026 months are not.
  2. USDJPY_<year>_h1.csv for 2018/2020/2022/2024 — folded from
     /Users/jo/Tick/usdjpy_m1/DAT_ASCII_USDJPY_M1_<year>.csv
     (histdata M1 bars "YYYYMMDD HHMMSS;o;h;l;c;vol"). 2022 = dollar-rally regime window.

Output dir: $FX_DERIV (default: /Users/jo/Tick/fx_bothways_deriv). Idempotent.
Run backtest/data_integrity_gate.py on every output before use (BACKTEST_TRUTH).
"""
import calendar, glob, os, time

OUT = os.environ.get("FX_DERIV", "/Users/jo/Tick/fx_bothways_deriv")
TICK = "/Users/jo/Tick"


def _ts(datepart, timepart):
    st = time.struct_time((int(datepart[:4]), int(datepart[4:6]), int(datepart[6:8]),
                           int(timepart[:2]), int(timepart[2:4]), int(timepart[4:6]), 0, 0, 0))
    return calendar.timegm(st)


def fold(buckets, ts, o, h, l, c):
    b = (ts // 3600) * 3600
    e = buckets.get(b)
    if e is None:
        buckets[b] = [o, h, l, c]
    else:
        if h > e[1]: e[1] = h
        if l < e[2]: e[2] = l
        e[3] = c


def write_h1(buckets, path):
    with open(path, "w") as f:
        f.write("ts,o,h,l,c\n")
        for b in sorted(buckets):
            o, h, l, c = buckets[b]
            f.write(f"{b},{o:.6f},{h:.6f},{l:.6f},{c:.6f}\n")
    print(f"wrote {path}: {len(buckets)} bars")


def nzd_2025():
    buckets = {}
    for fp in sorted(glob.glob(f"{TICK}/NZDUSD/DAT_ASCII_NZDUSD_T_2025*.csv")):
        n = 0
        with open(fp, errors="replace") as f:
            for line in f:
                p = line.strip().split(',')
                if len(p) < 3 or ' ' not in p[0]:
                    continue
                d, t = p[0].split(' ')
                try:
                    ts = _ts(d, t)  # HHMMSSmmm — first 6 digits are HHMMSS
                    bid = float(p[1]); ask = float(p[2])
                except ValueError:
                    continue
                m = (bid + ask) / 2.0
                if m > 0:
                    fold(buckets, ts, m, m, m, m); n += 1
        print(f"  {os.path.basename(fp)}: {n} ticks")
    write_h1(buckets, f"{OUT}/NZDUSD_2025_h1.csv")


def jpy_year(year):
    buckets = {}
    fp = f"{TICK}/usdjpy_m1/DAT_ASCII_USDJPY_M1_{year}.csv"
    with open(fp, errors="replace") as f:
        for line in f:
            p = line.strip().split(';')
            if len(p) < 5 or ' ' not in p[0]:
                continue
            d, t = p[0].split(' ')
            try:
                ts = _ts(d, t)
                o, h, l, c = float(p[1]), float(p[2]), float(p[3]), float(p[4])
            except ValueError:
                continue
            if c > 0:
                fold(buckets, ts, o, h, l, c)
    write_h1(buckets, f"{OUT}/USDJPY_{year}_h1.csv")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    nzd_2025()
    for y in (2018, 2020, 2022, 2024):
        jpy_year(y)
