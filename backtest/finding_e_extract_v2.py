#!/usr/bin/env python3
# =============================================================================
# finding_e_extract_v2.py  -- Use GOLD-STACK-ENTRY as the true entry marker.
#
# Finding E targets GoldEngineStack entries (16 live engines that fire through
# the bracket supervisor). Those entries emit:
#
#   HH:MM:SS [GOLD-STACK-ENTRY] LONG entry=PRICE tp=... sl=... eng=<Engine>
#       reason=... regime=...
#
# (There's also an ANSI-coloured duplicate line with additional fields -- we
# dedup by (ts,eng,side,entry_px).)
#
# To get pnl, we pair each entry to the next SHADOW-CLOSE for the same
# (engine, side) in chronological order. If no match, pnl=0.
#
# Bracket exits are still from [SHADOW-CLOSE] XAUUSD engine={XAUUSD_BRACKET,
# HybridBracketGold}, identical to v1.
# =============================================================================

import argparse
import csv
import re
import sys
from collections import defaultdict, deque
from datetime import datetime, timezone


BRACKET_ENGINES = {"XAUUSD_BRACKET", "HybridBracketGold"}

# Strip ANSI colour codes from log lines.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# HH:MM:SS [GOLD-STACK-ENTRY] (LONG|SHORT) entry=<px> ... eng=<Engine> ...
STACK_ENTRY_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[GOLD-STACK-ENTRY\]\s+"
    r"(?P<side>LONG|SHORT)\s+"
    r"entry=(?P<px>\d+(?:\.\d+)?)"
    r".*?eng=(?P<engine>\S+)"
)

# Shadow-close (unchanged from v1).
CLOSE_RE = re.compile(
    r"^(?P<h>\d{2}):(?P<m>\d{2}):(?P<s>\d{2})\s+"
    r"\[SHADOW-CLOSE\]\s+XAUUSD\s+"
    r"engine=(?P<engine>\S+)\s+"
    r"side=(?P<side>LONG|SHORT)\s+"
    r"gross=\$(?P<gross>-?\d+(?:\.\d+)?)\s+"
    r"net=\$(?P<net>-?\d+(?:\.\d+)?)"
)


def parse_date(date_str: str) -> int:
    dt = datetime.strptime(date_str, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    return int(dt.timestamp() * 1000)


def hms_to_ts_ms(day_epoch_ms: int, h: int, m: int, s: int) -> int:
    return day_epoch_ms + (h * 3600 + m * 60 + s) * 1000


def outcome_from_net(net: float) -> int:
    if net > 0.0:
        return 1
    if net < 0.0:
        return -1
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--date", required=True)
    ap.add_argument("--out-exits", default="finding_e_bracket_exits.csv")
    ap.add_argument("--out-entries", default="finding_e_all_entries.csv")
    args = ap.parse_args()

    day_ms = parse_date(args.date)

    bracket_exits: list[tuple[int, int, int]] = []
    # Dedup entries: key (ts_ms, engine, side, px). 1 row per unique key.
    entry_set: dict[tuple[int, str, str, str], None] = {}
    # Close FIFO queues by (engine, side).
    close_q: dict[tuple[str, str], deque] = defaultdict(deque)

    lines_scanned = 0
    with open(args.log, "r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            lines_scanned += 1
            line = ANSI_RE.sub("", raw.rstrip("\r\n"))

            mc = CLOSE_RE.match(line)
            if mc:
                engine = mc.group("engine")
                side = mc.group("side")
                net = float(mc.group("net"))
                close_ts = hms_to_ts_ms(day_ms,
                                        int(mc.group("h")),
                                        int(mc.group("m")),
                                        int(mc.group("s")))
                if engine in BRACKET_ENGINES:
                    is_long = 1 if side == "LONG" else 0
                    bracket_exits.append((close_ts, is_long, outcome_from_net(net)))
                else:
                    close_q[(engine, side)].append((close_ts, net))
                continue

            me = STACK_ENTRY_RE.match(line)
            if me:
                ts_ms = hms_to_ts_ms(day_ms,
                                     int(me.group("h")),
                                     int(me.group("m")),
                                     int(me.group("s")))
                engine = me.group("engine")
                side = me.group("side")
                px = me.group("px")
                entry_set.setdefault((ts_ms, engine, side, px), None)

    # Dedupe + order entries.
    entries = sorted(entry_set.keys(), key=lambda e: e[0])

    # Pair entries to closes FIFO per (engine, side), dropping orphans.
    joined: list[tuple[int, str, str, float]] = []
    for (ts_ms, engine, side, _px) in entries:
        q = close_q.get((engine, side))
        pnl = 0.0
        while q:
            close_ts, close_net = q[0]
            if close_ts < ts_ms:
                q.popleft()
                continue
            q.popleft()
            pnl = close_net
            break
        joined.append((ts_ms, engine, side, pnl))

    bracket_exits.sort(key=lambda e: e[0])

    with open(args.out_exits, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ts_ms", "is_long", "outcome"])
        for ts_ms, is_long, outcome in bracket_exits:
            w.writerow([ts_ms, is_long, outcome])

    with open(args.out_entries, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["ts_ms", "engine", "side", "pnl_usd"])
        for ts_ms, engine, side, pnl in joined:
            w.writerow([ts_ms, engine, side, f"{pnl:.2f}"])

    print(f"[INFO] lines scanned  : {lines_scanned}", file=sys.stderr)
    print(f"[INFO] bracket exits  : {len(bracket_exits)}", file=sys.stderr)
    print(f"[INFO] stack entries  : {len(entries)}", file=sys.stderr)
    print(f"[INFO] entries with pnl match: {sum(1 for e in joined if e[3] != 0.0)}",
          file=sys.stderr)
    print(f"[INFO] wrote {args.out_exits}", file=sys.stderr)
    print(f"[INFO] wrote {args.out_entries}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
