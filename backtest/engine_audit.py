#!/usr/bin/env python3
# =============================================================================
# engine_audit.py  -- Multi-day engine performance audit for Omega.
#
# Handles TWO log formats observed in the 10-day window (04-14 to 04-23):
#
#   OLD FORMAT (04-14 to ~04-20):
#     HH:MM:SS [TAG] EXIT (LONG|SHORT) @ <px> reason=<R> pnl_usd=<p> mfe=<m> held=<N>s [SHADOW]
#     HH:MM:SS [TRADE-COST] XAUUSD gross=$<g> slip_in=... slip_out=... net=$<n> exit=<R>
#
#     Two lines at the same HH:MM:SS. We pair them (same ts, same side -> match).
#     Engine identity comes from the [TAG] prefix (ECE, CFE, DPE, GOLD-FLOW, ...).
#
#   NEW FORMAT (04-21 onwards, after the [SHADOW-CLOSE] refactor):
#     HH:MM:SS [SHADOW-CLOSE] XAUUSD engine=<Eng> side=<S> gross=$<g> net=$<n> exit=<R>
#
#     Single line, engine named canonically. Rich EXIT line still exists for
#     mfe/held context but is decoupled.
#
# Unified schema: Trade(date, ts_ms, engine, side, gross, net, exit_reason, mfe, held_s).
#
# OUTPUTS (audit_results/):
#   engine_summary.csv   per-engine 10-day aggregate
#   engine_by_day.csv    per-engine per-day breakdown
#   engine_ranked.txt    human-readable ranked table
#
# SCORING (closest-to-productive):
#   productivity_score = (net / n) * sqrt(n) * win_rate_weight(wr, n)
#     win_rate_weight: 0.3 if WR<30%, ramp 0.5->1.0 over 30-45%, 1.0->1.5 over
#     45-55%, 1.5 for WR>=55%. Under-sampled (n<5) engines get neutral 1.0.
#
# USAGE:
#   python engine_audit.py logs/omega_2026-04-14.log ... logs/omega_2026-04-23.log
# =============================================================================

import csv
import math
import os
import re
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path


BRACKET_ENGINES = {"XAUUSD_BRACKET", "HybridBracketGold"}

ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# NEW FORMAT: single-line canonical close.
CLOSE_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[SHADOW-CLOSE\]\s+XAUUSD\s+"
    r"engine=(?P<engine>\S+)\s+"
    r"side=(?P<side>LONG|SHORT)\s+"
    r"gross=\$(?P<gross>-?\d+(?:\.\d+)?)\s+"
    r"net=\$(?P<net>-?\d+(?:\.\d+)?)\s+"
    r"exit=(?P<exit>\S+)"
)

# OLD FORMAT a: [TAG] EXIT with pnl_usd / mfe / held.
# Not all lines have [SHADOW] tag on the end; some (GOLD-FLOW) don't.
EXIT_TAG_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[(?P<tag>[A-Z][A-Z0-9-]+)\]\s+"
    r"EXIT\s+(?P<side>LONG|SHORT)\b"
    r".*?pnl_usd=(?P<pnl>-?\d+(?:\.\d+)?)"
    r".*?mfe=(?P<mfe>-?\d+(?:\.\d+)?)"
    r".*?held=(?P<held>\d+(?:\.\d+)?)s"
)

# OLD FORMAT b: [TRADE-COST] companion with gross / net / exit reason.
TRADE_COST_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[TRADE-COST\]\s+XAUUSD\s+"
    r"gross=\$(?P<gross>-?\d+(?:\.\d+)?)\s+"
    r"slip_in=\$(?P<slip_in>-?\d+(?:\.\d+)?)\s+"
    r"slip_out=\$(?P<slip_out>-?\d+(?:\.\d+)?)\s+"
    r"net=\$(?P<net>-?\d+(?:\.\d+)?)\s+"
    r"exit=(?P<exit>\S+)"
)

# Rich EXIT line on NEW format days (when paired with [SHADOW-CLOSE] the
# [TAG] EXIT still fires but without pnl_usd; with the older `pnl=$X` style).
EXIT_NEWFMT_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[(?P<tag>[A-Z][A-Z0-9-]+)\]\s+"
    r"EXIT\s+(?P<side>LONG|SHORT)\b"
    r".*?mfe=(?P<mfe>-?\d+(?:\.\d+)?)"
    r".*?held=(?P<held>\d+(?:\.\d+)?)s"
)

TAG_TO_ENGINE = {
    "ECE": "EMACross",
    "CFE": "CandleFlowEngine",
    "DPE": "DomPersistEngine",
    "GFE": "GoldFlowEngine",
    "GOLD-FLOW": "GoldFlowEngine",
    "MCE": "MacroCrash",
    "MCE-SHADOW": "MacroCrash",
    "CBE": "CompBreakout",
    "TSE": "TickScalp",
    "ORB": "OpenRangeBreakout",
    "BBMR": "BBMeanReversion",
    "BBE": "BBMeanReversion",
    "PCE": "PullbackCont",
}


class Trade:
    __slots__ = ("date", "ts_ms", "engine", "side", "gross", "net", "exit_reason",
                 "mfe", "held_s")

    def __init__(self, date, ts_ms, engine, side, gross, net, exit_reason,
                 mfe=float("nan"), held_s=-1):
        self.date = date
        self.ts_ms = ts_ms
        self.engine = engine
        self.side = side
        self.gross = gross
        self.net = net
        self.exit_reason = exit_reason
        self.mfe = mfe
        self.held_s = held_s


def date_from_filename(path):
    m = re.search(r"(\d{4}-\d{2}-\d{2})", os.path.basename(path))
    if not m:
        raise ValueError(f"cannot parse date from {path}")
    return m.group(1)


def day_epoch_ms(date_str):
    dt = datetime.strptime(date_str, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1000)


def hms_to_ms(day_ms, h, m, s):
    return day_ms + (h * 3600 + m * 60 + s) * 1000


def parse_log(path):
    """Parse one daily log into a list of Trade records. Handles both formats."""
    date = date_from_filename(path)
    day_ms = day_epoch_ms(date)

    # --- Collect NEW-format closes first ---
    new_closes = []  # list of (ts_ms, engine, side, gross, net, exit_reason)
    # --- Collect OLD-format [TAG] EXIT lines keyed by (ts_ms, side) -> (engine, pnl, mfe, held) ---
    old_exits = {}   # (ts_ms, side) -> dict
    # --- Collect OLD-format [TRADE-COST] lines keyed by (ts_ms, exit_reason) for pairing ---
    old_costs = {}   # (ts_ms) -> list of dicts (multiple per second possible but rare)
    # --- Rich mfe/held index for NEW format ---
    newfmt_rich = {}  # (ts_ms, engine, side) -> (mfe, held_s)

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = ANSI_RE.sub("", raw.rstrip("\r\n"))

            m = CLOSE_RE.match(line)
            if m:
                ts_ms = hms_to_ms(day_ms, int(m.group("h")), int(m.group("m")),
                                  int(m.group("s")))
                new_closes.append({
                    "ts_ms": ts_ms,
                    "engine": m.group("engine"),
                    "side": m.group("side"),
                    "gross": float(m.group("gross")),
                    "net": float(m.group("net")),
                    "exit_reason": m.group("exit"),
                })
                continue

            m = EXIT_TAG_RE.match(line)
            if m:
                tag = m.group("tag")
                engine = TAG_TO_ENGINE.get(tag)
                if engine:
                    ts_ms = hms_to_ms(day_ms, int(m.group("h")), int(m.group("m")),
                                      int(m.group("s")))
                    old_exits[(ts_ms, m.group("side"))] = {
                        "engine": engine,
                        "pnl_usd": float(m.group("pnl")),
                        "mfe": float(m.group("mfe")),
                        "held_s": int(float(m.group("held"))),
                    }
                # Also note for newfmt rich lookup (same line pattern, different pairing).
                # fall through.

            m2 = EXIT_NEWFMT_RE.match(line)
            if m2:
                tag = m2.group("tag")
                engine = TAG_TO_ENGINE.get(tag)
                if engine:
                    ts_ms = hms_to_ms(day_ms, int(m2.group("h")), int(m2.group("m")),
                                      int(m2.group("s")))
                    newfmt_rich[(ts_ms, engine, m2.group("side"))] = (
                        float(m2.group("mfe")),
                        int(float(m2.group("held"))),
                    )

            m = TRADE_COST_RE.match(line)
            if m:
                ts_ms = hms_to_ms(day_ms, int(m.group("h")), int(m.group("m")),
                                  int(m.group("s")))
                old_costs.setdefault(ts_ms, []).append({
                    "gross": float(m.group("gross")),
                    "net": float(m.group("net")),
                    "exit_reason": m.group("exit"),
                })

    trades = []

    # Ghost-trade sanity filter: drop anything with |net| > $1000. No XAUUSD
    # position at our lot sizes (0.01-0.05) can realize that magnitude.
    # Observed on 04-21: two CFE exits with pnl_usd=-$4810.93 and -$4767.37
    # caused by a held=<epoch_ms>s bug in CFE's shadow-PnL path. Without this
    # filter CFE's 10-day net is wildly distorted.
    def sane(gross, net):
        return abs(gross) <= 1000.0 and abs(net) <= 1000.0

    # --- Emit NEW format trades (with rich mfe/held join) ---
    for c in new_closes:
        if not sane(c["gross"], c["net"]):
            continue
        key = (c["ts_ms"], c["engine"], c["side"])
        mfe, held = newfmt_rich.get(key, (float("nan"), -1))
        # Clamp absurd held values (ghost-trade bug: held stored as epoch_ms).
        if held > 86400:
            held = -1
        trades.append(Trade(date, c["ts_ms"], c["engine"], c["side"],
                            c["gross"], c["net"], c["exit_reason"], mfe, held))

    # --- Emit OLD format trades: pair each EXIT with the nearest TRADE-COST at same ts_ms ---
    consumed_cost_idx = defaultdict(int)
    for (ts_ms, side), ex in old_exits.items():
        held = ex["held_s"]
        if held > 86400:
            held = -1
        costs = old_costs.get(ts_ms, [])
        idx = consumed_cost_idx[ts_ms]
        if idx < len(costs):
            cc = costs[idx]
            consumed_cost_idx[ts_ms] = idx + 1
            if not sane(cc["gross"], cc["net"]):
                continue
            trades.append(Trade(date, ts_ms, ex["engine"], side,
                                cc["gross"], cc["net"], cc["exit_reason"],
                                ex["mfe"], held))
        else:
            if not sane(ex["pnl_usd"], ex["pnl_usd"]):
                continue
            trades.append(Trade(date, ts_ms, ex["engine"], side,
                                ex["pnl_usd"], ex["pnl_usd"], "unknown",
                                ex["mfe"], held))

    return trades


def win_rate_weight(wr, n):
    if n < 5:
        return 1.0
    if wr >= 0.55:
        return 1.5
    if wr >= 0.45:
        return 1.0 + (wr - 0.45) * 5.0
    if wr >= 0.30:
        return 0.5 + (wr - 0.30) * (0.5 / 0.15)
    return 0.3


def productivity_score(net, n, wr):
    if n == 0:
        return float("-inf")
    per_trade = net / n
    return per_trade * math.sqrt(n) * win_rate_weight(wr, n)


def main():
    if len(sys.argv) < 2:
        print("usage: engine_audit.py <log1> <log2> ...", file=sys.stderr)
        return 2

    out_dir = Path("audit_results")
    out_dir.mkdir(exist_ok=True)

    all_trades = []
    for log_path in sys.argv[1:]:
        print(f"[INFO] parsing {log_path}", file=sys.stderr)
        tr = parse_log(log_path)
        print(f"       -> {len(tr)} trades", file=sys.stderr)
        all_trades.extend(tr)

    print(f"[INFO] total trades: {len(all_trades)}", file=sys.stderr)

    by_engine = defaultdict(list)
    for t in all_trades:
        by_engine[t.engine].append(t)

    summary_rows = []
    for engine, trades in by_engine.items():
        n = len(trades)
        wins = sum(1 for t in trades if t.net > 0)
        losses = sum(1 for t in trades if t.net < 0)
        be = n - wins - losses
        net_total = sum(t.net for t in trades)
        gross_total = sum(t.gross for t in trades)
        wr = wins / n if n else 0.0
        per_trade = net_total / n if n else 0.0
        mfes = [t.mfe for t in trades if not math.isnan(t.mfe)]
        helds = [t.held_s for t in trades if t.held_s >= 0]
        mean_mfe = sum(mfes) / len(mfes) if mfes else float("nan")
        mean_held = sum(helds) / len(helds) if helds else -1.0

        by_day = defaultdict(float)
        for t in trades:
            by_day[t.date] += t.net
        days_sorted = sorted(by_day.items(), key=lambda kv: kv[1])
        worst_day = f"{days_sorted[0][0]} ${days_sorted[0][1]:.2f}" if days_sorted else ""
        best_day = f"{days_sorted[-1][0]} ${days_sorted[-1][1]:.2f}" if days_sorted else ""

        exit_counts = defaultdict(int)
        for t in trades:
            exit_counts[t.exit_reason] += 1
        top_exits = "; ".join(f"{r}:{c}" for r, c in
                              sorted(exit_counts.items(), key=lambda kv: -kv[1])[:3])

        score = productivity_score(net_total, n, wr)

        summary_rows.append({
            "engine": engine,
            "n_trades": n,
            "wins": wins,
            "losses": losses,
            "be": be,
            "win_rate": wr,
            "gross_total": gross_total,
            "net_total": net_total,
            "per_trade": per_trade,
            "mean_mfe": mean_mfe,
            "mean_held_s": mean_held,
            "best_day": best_day,
            "worst_day": worst_day,
            "top_exits": top_exits,
            "prod_score": score,
            "is_bracket": engine in BRACKET_ENGINES,
        })

    summary_rows.sort(key=lambda r: r["prod_score"], reverse=True)

    with open(out_dir / "engine_summary.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=[
            "engine", "n_trades", "wins", "losses", "be", "win_rate",
            "gross_total", "net_total", "per_trade", "mean_mfe", "mean_held_s",
            "best_day", "worst_day", "top_exits", "prod_score", "is_bracket",
        ])
        w.writeheader()
        for r in summary_rows:
            out = dict(r)
            out["win_rate"] = f"{r['win_rate']:.3f}"
            out["gross_total"] = f"{r['gross_total']:.2f}"
            out["net_total"] = f"{r['net_total']:.2f}"
            out["per_trade"] = f"{r['per_trade']:.3f}"
            out["mean_mfe"] = f"{r['mean_mfe']:.3f}" if not math.isnan(r['mean_mfe']) else ""
            out["mean_held_s"] = f"{r['mean_held_s']:.1f}" if r['mean_held_s'] >= 0 else ""
            out["prod_score"] = f"{r['prod_score']:.3f}"
            w.writerow(out)

    with open(out_dir / "engine_by_day.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["date", "engine", "n_trades", "wins", "losses", "net"])
        agg = defaultdict(lambda: {"n": 0, "w": 0, "l": 0, "net": 0.0})
        for t in all_trades:
            a = agg[(t.date, t.engine)]
            a["n"] += 1
            if t.net > 0:
                a["w"] += 1
            elif t.net < 0:
                a["l"] += 1
            a["net"] += t.net
        for (date, engine) in sorted(agg.keys()):
            a = agg[(date, engine)]
            w.writerow([date, engine, a["n"], a["w"], a["l"], f"{a['net']:.2f}"])

    # Ranked human-readable
    lines = []
    n_days = len(sys.argv) - 1
    total_net = sum(t.net for t in all_trades)
    lines.append(f"ENGINE AUDIT -- {n_days} days")
    lines.append(f"Total trades: {len(all_trades)}   Grand net: ${total_net:+.2f}")
    lines.append("")
    lines.append("RANKED BY PRODUCTIVITY SCORE (closest to profitable first)")
    lines.append("score = (net/n) * sqrt(n) * wr_weight")
    lines.append("")
    hdr = (f"{'engine':<26}{'n':>5}{'wr':>8}{'net':>12}{'/trd':>10}"
           f"{'mfe':>8}{'held':>8}{'score':>9}  flags")
    lines.append(hdr)
    lines.append("-" * len(hdr))
    for r in summary_rows:
        mfe_str = f"{r['mean_mfe']:7.2f}" if not math.isnan(r['mean_mfe']) else "      -"
        held_str = f"{r['mean_held_s']:6.1f}s" if r['mean_held_s'] >= 0 else "      -"
        flags = " BRK" if r["is_bracket"] else ""
        lines.append(
            f"{r['engine']:<26}"
            f"{r['n_trades']:>5d}"
            f"{r['win_rate']*100:>7.1f}%"
            f"${r['net_total']:>+10.2f}"
            f"${r['per_trade']:>+8.3f}"
            f"{mfe_str}"
            f"{held_str}"
            f"{r['prod_score']:>+8.2f}"
            f"{flags}"
        )
    lines.append("")
    lines.append("Interpretation:")
    lines.append("  score > 0   -- net-profitable across the window")
    lines.append("  score ~= 0  -- break-even; small entry-filter tweak may tip positive")
    lines.append("  score < 0   -- net loser; magnitude = severity")
    lines.append("  BRK         -- bracket engine (separate codepath, not a 'stack engine')")

    out_txt = "\n".join(lines) + "\n"
    with open(out_dir / "engine_ranked.txt", "w", encoding="utf-8") as f:
        f.write(out_txt)
    print("\n" + out_txt)

    print(f"[DONE] wrote {out_dir}/engine_summary.csv, engine_by_day.csv, engine_ranked.txt",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
