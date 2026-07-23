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

FILL-DROUGHT check (S-2026-07-24, operator "checks-a-proxy-misses-the-outcome" audit):
  The per-engine watch above counts CLOSED TRADES. With ZERO trades it can only say
  WAITING (silent green) for the whole eval window -- which is EXACTLY the overnight
  blind spot: the engines fired signals all night, every order bounced off a broker
  filter -> 0 fills, nothing closed, and a "closed-trade" monitor saw nothing to alarm
  on. Silence read as healthy. This is the canonical "expected activity but got zero"
  failure. check_fill_drought() closes it by watching the OUTCOME of the signal->fill
  pipeline directly:
    * omega_shadow_signals.csv  -> allow=1 rows  = entries the system WANTED to take
    * ibkr_fills.csv            -> broker fills   = entries that actually CLEARED
  If the box emitted many entry-signals across several symbols over the window but ZERO
  broker fills landed, the pipeline is broken (orders not placed / rejected / venue
  down) -> RED, NOT a quiet green. The signal count IS the activity gate: a genuinely
  quiet/closed market emits few signals and never trips this, so no false alarm.
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


# ── FILL-DROUGHT (outcome check: signals fired but nothing filled) ──────────────
import os as _os

SIGNALS_CSV = r"C:\Omega\logs\shadow\omega_shadow_signals.csv"
FILLS_CSV   = r"C:\Omega\logs\trades\ibkr_fills.csv"
# window + thresholds (env-overridable so the healthy path is easy to demonstrate)
DROUGHT_HRS    = float(_os.environ.get("EPW_DROUGHT_HRS", "24"))
DROUGHT_MINSIG = int(_os.environ.get("EPW_DROUGHT_MINSIG", "20"))   # allow=1 signals in window
DROUGHT_MINSYM = int(_os.environ.get("EPW_DROUGHT_MINSYM", "2"))    # across >=N distinct symbols
DROUGHT_INJECT_FILL = _os.environ.get("EPW_INJECT_FILL", "") == "1"  # pretend a fill exists (green-path proof)


def _count_allow_signals(text, since_ts):
    """(n_allow, {symbols}) for allow=1 entry-signals stamped >= since_ts."""
    n, syms = 0, set()
    if not text.strip():
        return n, syms
    try:
        rdr = csv.DictReader(io.StringIO(text))
        for row in rdr:
            if (row.get("allow") or "").strip() != "1":
                continue
            try:
                ts = float(row.get("ts_unix") or 0)
            except ValueError:
                continue
            if ts >= since_ts:
                n += 1
                s = (row.get("symbol") or "").strip()
                if s:
                    syms.add(s)
    except csv.Error:
        pass
    return n, syms


def _count_fills(text, since_ts):
    """n broker fills (ibkr_fills.csv, col0=ts_unix) stamped >= since_ts; -1 if file empty/unreadable."""
    lines = [ln for ln in (text or "").splitlines() if ln.strip()]
    if not lines:
        return -1
    n = 0
    for ln in lines:
        try:
            row = next(csv.reader([ln]))
        except Exception:
            continue
        if not row or row[0] == "ts_unix":
            continue
        try:
            ts = float(row[0])
        except ValueError:
            continue
        if ts >= since_ts:
            n += 1
    return n


def check_fill_drought(now):
    """OUTCOME check: entry-signals fired but ZERO broker fills over the window = RED.

    Prints one block and returns 1 (RED) / 0 (ok). The presence of >= DROUGHT_MINSIG
    allow=1 signals across >= DROUGHT_MINSYM symbols proves the market was live and the
    engines armed; ZERO fills in the same window means the signal->order->fill pipeline
    is not delivering (the overnight reject-storm class). A quiet/closed market emits few
    signals and never trips this -> no false alarm."""
    since = now.timestamp() - DROUGHT_HRS * 3600.0
    sig_txt = fetch(SIGNALS_CSV)
    fil_txt = fetch(FILLS_CSV)
    n_sig, syms = _count_allow_signals(sig_txt, since)
    n_fill = _count_fills(fil_txt, since)
    if DROUGHT_INJECT_FILL:
        n_fill = max(n_fill, 1)   # green-path proof knob

    hdr = f"  FILL-DROUGHT (signal->fill pipeline, last {DROUGHT_HRS:.0f}h):"
    # can we even read the inputs?
    if not sig_txt.strip() and not fil_txt.strip():
        print(f"{hdr} SKIP -- could not read signals/fills on {HOST} (ssh down?)")
        return 0
    active = (n_sig >= DROUGHT_MINSIG and len(syms) >= DROUGHT_MINSYM)
    drought = active and (n_fill == 0)
    if drought:
        print(f"  RED  [FILL-DROUGHT ] engines fired {n_sig} entry-signals across "
              f"{len(syms)} syms ({','.join(sorted(syms))}) but 0 broker fills in {DROUGHT_HRS:.0f}h")
        print(f"        signals want to trade, NOTHING is clearing the broker -- orders "
              f"rejected/not-placed (the overnight 0-fill blind spot). Check the exec path, not the ledger.")
        return 1
    # healthy / benign explanations
    if not active:
        print(f"  PASS [FILL-DROUGHT ] only {n_sig} signals / {len(syms)} syms in window "
              f"(< {DROUGHT_MINSIG}/{DROUGHT_MINSYM} activity gate) -- market quiet, drought not asserted")
    elif n_fill < 0:
        print(f"  PASS [FILL-DROUGHT ] {n_sig} signals / {len(syms)} syms but fills file empty/unreadable "
              f"-- cannot assert drought (no false RED)")
    else:
        print(f"  PASS [FILL-DROUGHT ] {n_sig} signals / {len(syms)} syms -> {n_fill} broker fills in window "
              f"(pipeline delivering)")
    return 0


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
    # OUTCOME check: signals fired but nothing filled (the overnight 0-fill blind spot).
    # Independent of the per-engine WATCH list -> catches the whole-book pipeline stall
    # that "closed-trade" counting reads as silent-green.
    worst |= check_fill_drought(now)
    if worst:
        print("  -> RED: an engine is NOT turning (or never produced inside its window),")
        print("     or the signal->fill pipeline is dead (signals fired, 0 broker fills).")
        print("     Operator standing order: bring a BETTER SOLUTION, don't let it ride.")
    return worst


if __name__ == "__main__":
    sys.exit(main())
