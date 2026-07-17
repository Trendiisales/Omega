#!/usr/bin/env python3
"""
ENGINE PERFORMANCE WATCH -- operator-ordered per-engine turn monitor (S-2026-07-17s).

Origin: operator 2026-07-17: "i want the engine connorsrsi2 engine monitored, if this
does not turn i want a better solution". This watch makes that decision visible at every
session start instead of relying on someone remembering to check a ledger.

What it does (read-only, skew-free VPS-side data via `ssh omega-new`):
  * Pulls the LIVE ledger (C:\\Omega\\logs\\trades\\omega_trade_closes.csv) and the
    SHADOW ledger (C:\\Omega\\logs\\shadow\\omega_shadow.csv).
  * For each WATCH entry (regex over the engine tag), reports: n trades, net $, win
    rate, last close date, days since watch start.
  * Verdict per entry:
      TURNING      net > 0 with >= min_trades closes
      NOT-TURNING  net <= 0 with >= min_trades closes  -> operator wants a better solution
      WAITING      < min_trades closes, still inside eval_days
      STALLED      < min_trades closes AND eval_days exceeded -> engine never produced;
                   treat like NOT-TURNING (a silent engine cannot "turn")
  * Exit 0 all TURNING/WAITING; exit 1 any NOT-TURNING/STALLED (hook prints RED).

Notes:
  * Read-only (AUDIT_PROBE_SAFETY: GET/type only, no mutation).
  * Remote commands are PIPELINE-FREE (cmd.exe splits '|' even in single quotes).
  * Connors silence context: ConnorsRSI2 is SMA200 bull-gated -- with NAS100 in a bear
    leg (IndexBearShort short open) ZERO fires is the GATE working, not the engine dead.
    That is why WAITING has a generous eval window instead of alarming on day one.
"""
import subprocess
import sys
import re
import csv
import io
from datetime import datetime, timezone

HOST = "omega-new"  # LIVE box. omega-vps = retired dead box, never use.

WATCH = [
    # name              tag regex (case-ins)   watch-start   eval  min
    {"name": "ConnorsRSI2 NAS100 (LIVE)", "re": r"connors.*nas|nas.*connors|ConnorsRSI2$|ConnorsRSI2_NAS",
     "start": "2026-07-08", "eval_days": 30, "min_trades": 3,
     "note": "SMA200 bull-gated dip-buy; silent-by-gate while NAS100 rides a bear leg"},
    {"name": "ConnorsRSI2 GER40 (shadow)", "re": r"connors.*ger|ger.*connors",
     "start": "2026-07-08", "eval_days": 30, "min_trades": 3,
     "note": "shadow instance, same gate"},
    {"name": "Connors breadth book (12 cells, shadow)", "re": r"connors(ibs|streak|double|rsi3)|Connors(IBS|STREAK|DOUBLE|RSI3)",
     "start": "2026-07-14", "eval_days": 30, "min_trades": 3,
     "note": "NAS/SPX/DJ30 MR cells, BOOK_CAP=3"},
]


def fetch(path):
    """Read a VPS file via ssh (cmd.exe `type`, pipeline-free). Returns text or ''."""
    try:
        r = subprocess.run(["ssh", HOST, f"type {path}"],
                           capture_output=True, text=True, timeout=60)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def parse_ledger(text, engine_col, net_col, exit_ts_col):
    """Yield (engine_tag, net_pnl, exit_dt) rows from a ledger CSV body."""
    out = []
    if not text.strip():
        return out
    try:
        rdr = csv.DictReader(io.StringIO(text))
        for row in rdr:
            eng = (row.get(engine_col) or "").strip()
            if not eng:
                continue
            try:
                net = float(row.get(net_col) or 0.0)
            except ValueError:
                continue
            ts_raw = (row.get(exit_ts_col) or "").strip()
            dt = None
            if ts_raw:
                try:
                    dt = datetime.fromtimestamp(int(float(ts_raw)), tz=timezone.utc)
                except (ValueError, OSError):
                    try:
                        dt = datetime.fromisoformat(ts_raw.replace("Z", "+00:00"))
                    except ValueError:
                        dt = None
            out.append((eng, net, dt))
    except csv.Error:
        pass
    return out


def main():
    live_txt = fetch(r"C:\Omega\logs\trades\omega_trade_closes.csv")
    shad_txt = fetch(r"C:\Omega\logs\shadow\omega_shadow.csv")
    if not live_txt and not shad_txt:
        print("ENGINE PERF WATCH: [error] could not read either VPS ledger -- ssh omega-new down?")
        return 1

    rows = (parse_ledger(live_txt, "engine", "net_pnl", "exit_ts_unix")
            + parse_ledger(shad_txt, "engine", "net_pnl", "exit_ts_unix"))
    # shadow csv may use different headers -- second attempt on common variants
    if shad_txt and not any(r for r in parse_ledger(shad_txt, "engine", "net_pnl", "exit_ts_unix")):
        for eng_c, net_c, ts_c in (("engine", "pnl", "exit_ts"), ("tag", "net_pnl", "exit_ts_unix")):
            extra = parse_ledger(shad_txt, eng_c, net_c, ts_c)
            if extra:
                rows += extra
                break

    now = datetime.now(timezone.utc)
    worst = 0
    print(f"ENGINE PERF WATCH (ref {now.date()}) -- operator turn-monitor")
    for w in WATCH:
        rx = re.compile(w["re"], re.IGNORECASE)
        hits = [r for r in rows if rx.search(r[0])]
        n = len(hits)
        net = sum(r[1] for r in hits)
        wins = sum(1 for r in hits if r[1] > 0)
        last = max((r[2] for r in hits if r[2]), default=None)
        start = datetime.strptime(w["start"], "%Y-%m-%d").replace(tzinfo=timezone.utc)
        age_d = (now - start).days
        if n >= w["min_trades"]:
            verdict = "TURNING" if net > 0 else "NOT-TURNING"
        else:
            verdict = "WAITING" if age_d <= w["eval_days"] else "STALLED"
        flag = "PASS" if verdict in ("TURNING", "WAITING") else "RED "
        if flag == "RED ":
            worst = 1
        wr = f"{100.0*wins/n:.0f}%" if n else "-"
        last_s = last.strftime("%Y-%m-%d") if last else "never"
        print(f"  {flag} [{verdict:11s}] {w['name']}: n={n} net=${net:+.2f} WR={wr} "
              f"last={last_s} watch-day {age_d}/{w['eval_days']}")
        print(f"        {w['note']}")
    if worst:
        print("  -> RED: an engine is NOT turning (or never produced inside its window).")
        print("     Operator standing order: bring a BETTER SOLUTION, don't let it ride.")
    return worst


if __name__ == "__main__":
    sys.exit(main())
