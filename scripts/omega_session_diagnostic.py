#!/usr/bin/env python3
# ==============================================================================
#  omega_session_diagnostic.py
#
#  Forensic diagnostic for the Omega Asian session, 2026-04-17 (UTC 00:00-07:00)
#  Target question: "why did Omega miss the 101pt Apr 17 Asia move?"
#
#  Inputs
#  ------
#    --log        Path to dated Omega log (default: C:\Omega\logs\omega_2026-04-17.log)
#    --date       Trading date YYYY-MM-DD (default: 2026-04-17)
#    --start      Session start HH:MM UTC  (default: 00:00)
#    --end        Session end   HH:MM UTC  (default: 07:00)
#    --out-dir    Output directory         (default: same dir as the log)
#    --bin-min    Timeline bin granularity in minutes (default: 5)
#
#  Outputs (written side-by-side next to the log)
#  ----------------------------------------------
#    omega_session_diagnostic_2026-04-17.html   -- full forensic report
#    omega_session_tag_inventory_2026-04-17.csv -- every unique [TAG] with count
#    omega_session_gate_rejects_2026-04-17.csv  -- (engine, reason) -> count
#    omega_session_entries_2026-04-17.csv       -- 12 entries + pre-entry context
#
#  The 12 entries from the 2026-04-17 Asia tape are embedded as ENTRIES below.
#  If a different date is passed, the script still runs inventory + gate-reject
#  aggregation across the window, but per-entry windows will be empty.
#
#  Hard rules
#  ----------
#    * Python stdlib only. No pandas. No external deps.
#    * No sed. No grep. Pure str / re operations.
#    * Read log line-by-line; never slurp a several-hundred-MB file into RAM.
#    * HTML output is self-contained (inline CSS, no external assets).
# ==============================================================================

from __future__ import annotations

import argparse
import csv
import html
import os
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple


# ------------------------------------------------------------------------------
#  Embedded known state for 2026-04-17 Asia session (from prior diagnostic)
# ------------------------------------------------------------------------------

# Each entry: (HH:MM:SS UTC, symbol, side, entry_px, engine, exit_reason, net_pnl)
ENTRIES: List[Tuple[str, str, str, float, str, str, float]] = [
    ("00:16:00", "XAUUSD", "SHORT", 4787.91, "EMACross",         "SL",             -25.80),
    ("00:18:56", "XAUUSD", "SHORT", 4791.25, "EMACross",         "TO",              -5.30),
    ("00:53:00", "XAUUSD", "SHORT", 4795.71, "EMACross",         "TO",              -1.20),
    ("01:12:22", "XAUUSD", "SHORT", 4770.09, "HybridBracketGold","TRAIL",           -0.75),
    ("01:12:22", "XAUUSD", "SHORT", 4769.98, "XAUUSD_BRACKET",   "BREAKOUT_FAIL",   -7.89),
    ("01:22:50", "XAUUSD", "SHORT", 4774.98, "XAUUSD_BRACKET",   "BE",              -0.28),
    ("01:34:12", "XAUUSD", "SHORT", 4771.07, "XAUUSD_BRACKET",   "SL",             -11.71),
    ("05:04:27", "XAUUSD", "SHORT", 4800.55, "CompBreakout",     "TO",              -8.77),
    ("05:16:17", "XAUUSD", "SHORT", 4803.32, "CompBreakout",     "TO",              -1.91),
    ("05:30:30", "XAUUSD", "SHORT", 4802.41, "CompBreakout",     "TRAIL",          +19.40),
    ("06:18:30", "XAUUSD", "SHORT", 4786.21, "CompBreakout",     "TO",              +0.43),
    ("06:24:25", "XAUUSD", "LONG",  4788.17, "DomPersist",       "SL",             -36.00),
]

# Known gate-reject tag families at HEAD abd786e (per session memory)
KNOWN_GATE_TAGS: List[str] = [
    "MCE-NOSIG",
    "CFE-HMM-GATE",
    "CFE-TOD-DEAD",
    "CFE-STARTUP-LOCK",
    "CFE-BAR-BLOCK",
    "CFE-EXHAUSTED",
    "CFE-ASIA-ADVERSE",
    "CFE-CVD-BLOCK",
    "RSI-REV-BLOCK",
    "RSI-REV-DIAG",
    "CBE-STATE",
    "MCE-NOSIG",
    "GFE-SIGNAL-FAIL",
    "GFE-NOSIG",
    "GFE-PERSIST-RESET",
    "GF-BAR-BLOCK",
    "GF-OUTER",
    "ASIA-GATE",
    "MME-ASIA-BLOCK",
    "OMEGA-TRADE",
    "SHADOW",
    "FEED-STALE",
    "L2-DEAD",
    "CTRADER",
    "SUPERVISOR-XAUUSD",
    "ARMED-BYPASS",
    "GOLD-DIAG",
    "DOM-DIAG",
    "DOMPERSIST",
    "BB-MR",
]

# Pre-entry window applied to each entry timestamp for [FEED context dump]
PRE_ENTRY_SECONDS   = 30
POST_ENTRY_SECONDS  = 5


# ------------------------------------------------------------------------------
#  Log line parsing
# ------------------------------------------------------------------------------

# Matches "[HH:MM:SS.ffffff]" or "[HH:MM:SS]" or "YYYY-MM-DD HH:MM:SS.ffffff"
TS_PATTERNS = [
    re.compile(r"^\[(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?\]"),
    re.compile(r"^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?"),
    re.compile(r"^(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,6}))?"),
]

# Matches [TAG] at the start of the log body (after the timestamp has been stripped)
TAG_RE = re.compile(r"\[([A-Z][A-Z0-9_\-]{1,30})\]")

# Matches engine hints inside tags like [CFE-...] or [GFE-...]
ENGINE_FROM_TAG_RE = re.compile(r"^([A-Z]{2,6})[-_]")


def parse_ts(line: str, trading_date: datetime) -> Optional[datetime]:
    """Extract a UTC datetime from the head of a log line.

    The log is recorded in UTC. Date is taken from the trading_date argument
    unless the line itself carries a full YYYY-MM-DD stamp.
    """
    for pat in TS_PATTERNS:
        m = pat.match(line)
        if not m:
            continue
        g = m.groups()
        try:
            if pat is TS_PATTERNS[1]:
                # Full YYYY-MM-DD ...
                year, month, day = int(g[0]), int(g[1]), int(g[2])
                hh, mm, ss = int(g[3]), int(g[4]), int(g[5])
                us = int((g[6] or "0").ljust(6, "0")[:6])
                return datetime(year, month, day, hh, mm, ss, us, tzinfo=timezone.utc)
            else:
                hh, mm, ss = int(g[0]), int(g[1]), int(g[2])
                us = int((g[3] or "0").ljust(6, "0")[:6])
                return trading_date.replace(
                    hour=hh, minute=mm, second=ss, microsecond=us,
                    tzinfo=timezone.utc,
                )
        except (ValueError, IndexError):
            return None
    return None


def extract_tags(body: str) -> List[str]:
    """Return every bracketed tag from a line body, in order."""
    return TAG_RE.findall(body)


def infer_engine(tag: str) -> str:
    """Best-effort engine prefix from a tag like 'CFE-HMM-GATE' -> 'CFE'."""
    m = ENGINE_FROM_TAG_RE.match(tag)
    if m:
        return m.group(1)
    return tag.split("-")[0] if "-" in tag else tag


# ------------------------------------------------------------------------------
#  Session classifier (gate-reject categoriser)
# ------------------------------------------------------------------------------

REJECT_HINT_WORDS = (
    "BLOCK", "BLOCKED", "GATE", "NOSIG", "SIGNAL-FAIL", "STARTUP-LOCK",
    "TOD-DEAD", "ADVERSE", "EXHAUSTED", "STALE", "DEAD", "RESET",
    "BAR-BLOCK", "CVD-BLOCK", "HMM-GATE", "ASIA-BLOCK",
)


def is_reject_line(tags: List[str], body: str) -> bool:
    """Heuristic: does this line represent a gate rejection / entry block?"""
    body_upper = body.upper()
    for hint in REJECT_HINT_WORDS:
        if hint in body_upper:
            return True
    for tag in tags:
        tu = tag.upper()
        if any(h in tu for h in REJECT_HINT_WORDS):
            return True
    return False


def reject_reason(tags: List[str], body: str) -> str:
    """Summarise the gate/rejection reason with the most specific tag first."""
    for tag in tags:
        tu = tag.upper()
        if any(h in tu for h in REJECT_HINT_WORDS):
            return tag
    # Fallback: first hint word found in body
    body_upper = body.upper()
    for hint in REJECT_HINT_WORDS:
        if hint in body_upper:
            return hint
    return "UNKNOWN"


# ------------------------------------------------------------------------------
#  Core data structures
# ------------------------------------------------------------------------------

@dataclass
class BinStats:
    bin_start: datetime
    total_lines: int = 0
    reject_lines: int = 0
    rejects_by_engine: Counter = field(default_factory=Counter)
    rejects_by_reason: Counter = field(default_factory=Counter)
    top_tags: Counter = field(default_factory=Counter)


@dataclass
class EntryRecord:
    ts: datetime
    symbol: str
    side: str
    entry_px: float
    engine: str
    exit_reason: str
    net_pnl: float
    pre_window: List[Tuple[datetime, str]] = field(default_factory=list)


# ------------------------------------------------------------------------------
#  Single-pass log scan
# ------------------------------------------------------------------------------

def scan_log(
    log_path: Path,
    trading_date: datetime,
    window_start: datetime,
    window_end: datetime,
    bin_minutes: int,
    entries: List[EntryRecord],
) -> Tuple[
    Counter,                      # global tag inventory (tag -> count)
    Dict[Tuple[str, str], int],   # (engine, reason) -> count
    List[BinStats],               # timeline bins
    int,                          # lines scanned
    int,                          # lines in-window
]:
    """Single pass over the log.

    Produces:
      * Tag inventory counter
      * Gate-reject (engine, reason) histogram
      * Per-bin timeline stats
      * Per-entry pre-entry window lines (mutated onto entries[...])
    """
    tag_inventory: Counter = Counter()
    gate_rejects: Dict[Tuple[str, str], int] = defaultdict(int)
    bins: List[BinStats] = []

    # Pre-build bins covering [window_start, window_end)
    bin_delta = timedelta(minutes=bin_minutes)
    cur = window_start
    while cur < window_end:
        bins.append(BinStats(bin_start=cur))
        cur += bin_delta

    # Precompute pre-entry window ranges
    entry_windows: List[Tuple[datetime, datetime, EntryRecord]] = [
        (
            e.ts - timedelta(seconds=PRE_ENTRY_SECONDS),
            e.ts + timedelta(seconds=POST_ENTRY_SECONDS),
            e,
        )
        for e in entries
    ]

    def bin_for(ts: datetime) -> Optional[BinStats]:
        if ts < window_start or ts >= window_end:
            return None
        idx = int((ts - window_start).total_seconds() // (bin_minutes * 60))
        if 0 <= idx < len(bins):
            return bins[idx]
        return None

    total_lines = 0
    in_window_lines = 0

    # Stream the log. encoding='utf-8' with errors='replace' handles stray bytes
    # the Windows log writer can emit.
    with log_path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            total_lines += 1
            line = raw.rstrip("\r\n")
            if not line:
                continue

            ts = parse_ts(line, trading_date)
            if ts is None:
                continue

            if ts < window_start or ts >= window_end:
                continue

            in_window_lines += 1

            # Strip off the leading timestamp token for tag extraction
            # (keeps body readable in the HTML dump too)
            body = line
            # common form: "[HH:MM:SS.xxx] rest..."
            if line.startswith("[") and "]" in line:
                body = line[line.index("]") + 1:].lstrip()

            tags = extract_tags(body)
            for tag in tags:
                tag_inventory[tag] += 1

            # Timeline bin accounting
            stats = bin_for(ts)
            if stats is not None:
                stats.total_lines += 1
                for tag in tags:
                    stats.top_tags[tag] += 1

                if is_reject_line(tags, body):
                    stats.reject_lines += 1
                    reason = reject_reason(tags, body)
                    engine = infer_engine(tags[0]) if tags else "UNKNOWN"
                    stats.rejects_by_engine[engine] += 1
                    stats.rejects_by_reason[reason] += 1
                    gate_rejects[(engine, reason)] += 1

            # Per-entry pre-window capture
            for win_start, win_end, rec in entry_windows:
                if win_start <= ts <= win_end:
                    rec.pre_window.append((ts, line))
                    break  # a line belongs to at most one entry window

    return tag_inventory, dict(gate_rejects), bins, total_lines, in_window_lines


# ------------------------------------------------------------------------------
#  CSV writers
# ------------------------------------------------------------------------------

def write_tag_inventory_csv(path: Path, tag_inventory: Counter) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["tag", "count"])
        for tag, count in tag_inventory.most_common():
            w.writerow([tag, count])


def write_gate_rejects_csv(path: Path, gate_rejects: Dict[Tuple[str, str], int]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["engine", "reason", "count"])
        for (engine, reason), count in sorted(
            gate_rejects.items(), key=lambda kv: -kv[1]
        ):
            w.writerow([engine, reason, count])


def write_entries_csv(path: Path, entries: List[EntryRecord]) -> None:
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow([
            "entry_ts_utc", "symbol", "side", "entry_px", "engine",
            "exit_reason", "net_pnl", "pre_window_line_count",
        ])
        for e in entries:
            w.writerow([
                e.ts.strftime("%H:%M:%S"),
                e.symbol, e.side, f"{e.entry_px:.2f}", e.engine,
                e.exit_reason, f"{e.net_pnl:+.2f}", len(e.pre_window),
            ])


# ------------------------------------------------------------------------------
#  HTML report
# ------------------------------------------------------------------------------

HTML_CSS = """
:root {
  --bg: #0f1115;
  --panel: #161a22;
  --panel-alt: #1b212c;
  --border: #2a313d;
  --text: #dfe4ee;
  --muted: #8891a3;
  --accent: #4da3ff;
  --red: #ff6464;
  --amber: #ffb347;
  --green: #5fd07a;
}
* { box-sizing: border-box; }
body {
  font-family: -apple-system, 'SF Mono', Menlo, Consolas, monospace;
  background: var(--bg);
  color: var(--text);
  margin: 0;
  padding: 24px 32px;
  font-size: 13px;
  line-height: 1.55;
}
h1, h2, h3 { color: var(--text); margin-top: 28px; font-weight: 600; }
h1 { font-size: 22px; border-bottom: 1px solid var(--border); padding-bottom: 8px; }
h2 { font-size: 17px; margin-top: 36px; }
h3 { font-size: 14px; color: var(--accent); }
.meta { color: var(--muted); font-size: 12px; margin-bottom: 16px; }
.panel {
  background: var(--panel);
  border: 1px solid var(--border);
  border-radius: 6px;
  padding: 14px 18px;
  margin: 12px 0 20px;
  overflow-x: auto;
}
table { border-collapse: collapse; width: 100%; font-size: 12px; }
th, td {
  text-align: left; padding: 6px 10px;
  border-bottom: 1px solid var(--border);
  white-space: nowrap;
}
th { color: var(--muted); font-weight: 500; background: var(--panel-alt); position: sticky; top: 0; }
tr:hover td { background: var(--panel-alt); }
.num { text-align: right; font-variant-numeric: tabular-nums; }
.bad  { color: var(--red); }
.good { color: var(--green); }
.warn { color: var(--amber); }
.pill {
  display: inline-block; padding: 1px 8px; border-radius: 10px;
  background: var(--panel-alt); font-size: 11px; color: var(--muted);
  border: 1px solid var(--border);
}
.timeline {
  display: grid;
  grid-template-columns: 80px 1fr 60px;
  gap: 2px 8px;
  align-items: center;
  font-size: 11px;
}
.timeline .bar {
  height: 14px; background: var(--panel-alt); border-radius: 3px;
  position: relative; overflow: hidden;
}
.timeline .bar .fill {
  position: absolute; left: 0; top: 0; bottom: 0;
  background: linear-gradient(90deg, #3a6fb3, #4da3ff);
}
.timeline .bar .entry-marker {
  position: absolute; top: -2px; bottom: -2px; width: 2px;
  background: var(--green);
}
pre.log {
  background: var(--panel-alt);
  border: 1px solid var(--border);
  padding: 10px 12px;
  border-radius: 4px;
  font-size: 11px;
  line-height: 1.45;
  max-height: 360px;
  overflow: auto;
  white-space: pre;
  color: #cdd3e0;
}
.log-entry  { color: var(--green); font-weight: 600; }
.log-reject { color: var(--amber); }
.log-stale  { color: var(--red); }
.toc { columns: 2; column-gap: 28px; font-size: 12px; }
.toc a { color: var(--accent); text-decoration: none; }
.footnote { color: var(--muted); font-size: 11px; margin-top: 32px; }
"""


def html_escape(s: str) -> str:
    return html.escape(s, quote=True)


def render_timeline(bins: List[BinStats]) -> str:
    if not bins:
        return "<div class='panel'><em>No bins in window.</em></div>"
    max_total = max((b.total_lines for b in bins), default=1)
    rows: List[str] = []
    for b in bins:
        width_pct = (b.total_lines / max_total * 100) if max_total else 0
        top_reject = ""
        if b.rejects_by_reason:
            r, c = b.rejects_by_reason.most_common(1)[0]
            top_reject = f" · top reject: <span class='warn'>{html_escape(r)}</span> ×{c}"
        label = b.bin_start.strftime("%H:%M")
        rows.append(f"""
          <div>{label} UTC</div>
          <div class='bar'>
            <div class='fill' style='width:{width_pct:.1f}%'></div>
          </div>
          <div class='num'>{b.total_lines}</div>
          <div></div><div style='font-size:10px;color:var(--muted)'>{b.reject_lines} rejects{top_reject}</div><div></div>
        """)
    return f"<div class='panel'><div class='timeline'>{''.join(rows)}</div></div>"


def render_gate_table(gate_rejects: Dict[Tuple[str, str], int]) -> str:
    if not gate_rejects:
        return "<div class='panel'><em>No gate rejections detected.</em></div>"
    rows = "".join(
        f"<tr><td>{html_escape(e)}</td><td>{html_escape(r)}</td>"
        f"<td class='num'>{c:,}</td></tr>"
        for (e, r), c in sorted(gate_rejects.items(), key=lambda kv: -kv[1])
    )
    return f"""
      <div class='panel'>
        <table>
          <thead><tr><th>Engine</th><th>Reason</th><th class='num'>Count</th></tr></thead>
          <tbody>{rows}</tbody>
        </table>
      </div>
    """


def render_tag_inventory(tag_inventory: Counter, top_n: int = 50) -> str:
    if not tag_inventory:
        return "<div class='panel'><em>No tags found.</em></div>"
    rows = "".join(
        f"<tr><td>{html_escape(tag)}</td><td class='num'>{count:,}</td></tr>"
        for tag, count in tag_inventory.most_common(top_n)
    )
    total_unique = len(tag_inventory)
    total_count = sum(tag_inventory.values())
    return f"""
      <div class='panel'>
        <div class='meta'>
          Top {min(top_n, total_unique)} of {total_unique} unique tags
          ({total_count:,} total occurrences)
        </div>
        <table>
          <thead><tr><th>Tag</th><th class='num'>Count</th></tr></thead>
          <tbody>{rows}</tbody>
        </table>
      </div>
    """


def render_entries_table(entries: List[EntryRecord]) -> str:
    rows: List[str] = []
    for e in entries:
        pnl_cls = "good" if e.net_pnl > 0 else ("bad" if e.net_pnl < 0 else "")
        rows.append(f"""
          <tr>
            <td>{e.ts.strftime('%H:%M:%S')}</td>
            <td>{html_escape(e.symbol)}</td>
            <td>{html_escape(e.side)}</td>
            <td class='num'>{e.entry_px:.2f}</td>
            <td>{html_escape(e.engine)}</td>
            <td>{html_escape(e.exit_reason)}</td>
            <td class='num {pnl_cls}'>{e.net_pnl:+.2f}</td>
            <td class='num'>{len(e.pre_window)}</td>
            <td><a href='#entry-{e.ts.strftime("%H%M%S")}'>dump ↓</a></td>
          </tr>
        """)
    return f"""
      <div class='panel'>
        <table>
          <thead><tr>
            <th>Time UTC</th><th>Sym</th><th>Side</th><th class='num'>Entry</th>
            <th>Engine</th><th>Exit</th><th class='num'>Net $</th>
            <th class='num'>Pre-window lines</th><th></th>
          </tr></thead>
          <tbody>{''.join(rows)}</tbody>
        </table>
      </div>
    """


def classify_log_line(line: str) -> str:
    """Return a CSS class for colouring a log line in the per-entry dump."""
    upper = line.upper()
    if "[OMEGA-TRADE]" in upper or "ENTRY" in upper or "OPEN LONG" in upper or "OPEN SHORT" in upper:
        return "log-entry"
    if "FEED-STALE" in upper or "L2-DEAD" in upper:
        return "log-stale"
    for hint in REJECT_HINT_WORDS:
        if hint in upper:
            return "log-reject"
    return ""


def render_entry_dumps(entries: List[EntryRecord]) -> str:
    blocks: List[str] = []
    for e in entries:
        anchor = e.ts.strftime("%H%M%S")
        if e.pre_window:
            lines_html: List[str] = []
            for ts, raw in e.pre_window:
                cls = classify_log_line(raw)
                prefix = f"[{ts.strftime('%H:%M:%S.%f')[:-3]}] "
                # if line already starts with "[HH:..." don't double it up
                if raw.lstrip().startswith("["):
                    disp = html_escape(raw)
                else:
                    disp = html_escape(prefix + raw)
                if cls:
                    lines_html.append(f"<span class='{cls}'>{disp}</span>")
                else:
                    lines_html.append(disp)
            dump_html = "<pre class='log'>" + "\n".join(lines_html) + "</pre>"
        else:
            dump_html = (
                "<div class='panel'><em class='warn'>"
                "No log lines matched this entry's pre-window. "
                "Log is either truncated, time-offset, or the entry never actually fired.</em></div>"
            )

        pnl_cls = "good" if e.net_pnl > 0 else ("bad" if e.net_pnl < 0 else "")
        blocks.append(f"""
          <h3 id='entry-{anchor}'>
            {e.ts.strftime('%H:%M:%S')} UTC ·
            <span class='pill'>{html_escape(e.engine)}</span>
            {html_escape(e.symbol)} {html_escape(e.side)} @ {e.entry_px:.2f}
            → {html_escape(e.exit_reason)}
            <span class='num {pnl_cls}'>{e.net_pnl:+.2f}</span>
          </h3>
          {dump_html}
        """)
    return "\n".join(blocks)


def render_entries_toc(entries: List[EntryRecord]) -> str:
    links = "".join(
        f"<div>· <a href='#entry-{e.ts.strftime('%H%M%S')}'>"
        f"{e.ts.strftime('%H:%M:%S')} {html_escape(e.engine)} {html_escape(e.side)}"
        f"</a></div>"
        for e in entries
    )
    return f"<div class='panel toc'>{links}</div>"


def render_summary_banner(
    entries: List[EntryRecord],
    tag_inventory: Counter,
    gate_rejects: Dict[Tuple[str, str], int],
    bins: List[BinStats],
    total_lines: int,
    in_window_lines: int,
) -> str:
    total_rejects = sum(gate_rejects.values())
    net_pnl = sum(e.net_pnl for e in entries)
    wins = sum(1 for e in entries if e.net_pnl > 0)
    losses = sum(1 for e in entries if e.net_pnl < 0)
    wr = (wins / (wins + losses) * 100.0) if (wins + losses) else 0.0
    top_reject = ""
    if gate_rejects:
        (e, r), c = max(gate_rejects.items(), key=lambda kv: kv[1])
        top_reject = f"{e}/{r} ({c:,})"
    top_tag = tag_inventory.most_common(1)[0] if tag_inventory else ("-", 0)
    return f"""
      <div class='panel'>
        <table>
          <tr><td>Lines scanned</td><td class='num'>{total_lines:,}</td>
              <td>In window</td><td class='num'>{in_window_lines:,}</td></tr>
          <tr><td>Unique tags</td><td class='num'>{len(tag_inventory):,}</td>
              <td>Top tag</td><td>{html_escape(top_tag[0])} ({top_tag[1]:,})</td></tr>
          <tr><td>Gate rejects (total)</td><td class='num'>{total_rejects:,}</td>
              <td>Top reject</td><td>{html_escape(top_reject)}</td></tr>
          <tr><td>Entries recorded</td><td class='num'>{len(entries)}</td>
              <td>Wins / losses</td><td>{wins} / {losses} ({wr:.1f}% WR)</td></tr>
          <tr><td>Timeline bins</td><td class='num'>{len(bins)}</td>
              <td>Net PnL</td>
              <td class='num {"good" if net_pnl >= 0 else "bad"}'>{net_pnl:+.2f} USD</td></tr>
        </table>
      </div>
    """


def write_html_report(
    path: Path,
    log_path: Path,
    trading_date: datetime,
    window_start: datetime,
    window_end: datetime,
    bin_minutes: int,
    entries: List[EntryRecord],
    tag_inventory: Counter,
    gate_rejects: Dict[Tuple[str, str], int],
    bins: List[BinStats],
    total_lines: int,
    in_window_lines: int,
) -> None:
    now_utc = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    doc = f"""<!DOCTYPE html>
<html lang='en'>
<head>
<meta charset='utf-8'>
<title>Omega session diagnostic · {trading_date.strftime('%Y-%m-%d')}</title>
<style>{HTML_CSS}</style>
</head>
<body>
  <h1>Omega session diagnostic · {trading_date.strftime('%Y-%m-%d')} ·
      {window_start.strftime('%H:%M')}-{window_end.strftime('%H:%M')} UTC</h1>
  <div class='meta'>
    Log: <code>{html_escape(str(log_path))}</code> ·
    Generated: {now_utc} ·
    Bin granularity: {bin_minutes} min
  </div>

  <h2>Summary</h2>
  {render_summary_banner(entries, tag_inventory, gate_rejects, bins, total_lines, in_window_lines)}

  <h2>Timeline — activity and gate rejects per {bin_minutes}-minute bin</h2>
  {render_timeline(bins)}

  <h2>Gate rejections by engine × reason</h2>
  {render_gate_table(gate_rejects)}

  <h2>Tag inventory (top 50)</h2>
  {render_tag_inventory(tag_inventory)}

  <h2>Entries — 12 Asia session trades</h2>
  {render_entries_table(entries)}

  <h3>Jump to per-entry dumps</h3>
  {render_entries_toc(entries)}

  <h2>Per-entry pre-window log dumps
      (-{PRE_ENTRY_SECONDS}s / +{POST_ENTRY_SECONDS}s)</h2>
  {render_entry_dumps(entries)}

  <div class='footnote'>
    Generated by omega_session_diagnostic.py · stdlib-only · HEAD abd786e target ·
    Pre-entry windows: -{PRE_ENTRY_SECONDS}s / +{POST_ENTRY_SECONDS}s around each entry.
  </div>
</body>
</html>
"""
    path.write_text(doc, encoding="utf-8")


# ------------------------------------------------------------------------------
#  Entry point
# ------------------------------------------------------------------------------

def build_entries(trading_date: datetime) -> List[EntryRecord]:
    out: List[EntryRecord] = []
    for hms, sym, side, px, engine, exit_reason, pnl in ENTRIES:
        hh, mm, ss = (int(x) for x in hms.split(":"))
        ts = trading_date.replace(
            hour=hh, minute=mm, second=ss, microsecond=0, tzinfo=timezone.utc,
        )
        out.append(EntryRecord(
            ts=ts, symbol=sym, side=side, entry_px=px,
            engine=engine, exit_reason=exit_reason, net_pnl=pnl,
        ))
    return out


def parse_args(argv: List[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__, add_help=True)
    p.add_argument(
        "--log", default=r"C:\Omega\logs\omega_2026-04-17.log",
        help="Path to the dated Omega log file.",
    )
    p.add_argument(
        "--date", default="2026-04-17",
        help="Trading date (UTC) in YYYY-MM-DD.",
    )
    p.add_argument(
        "--start", default="00:00",
        help="Session start HH:MM UTC (inclusive).",
    )
    p.add_argument(
        "--end", default="07:00",
        help="Session end HH:MM UTC (exclusive).",
    )
    p.add_argument(
        "--out-dir", default=None,
        help="Output directory. Defaults to the directory containing the log.",
    )
    p.add_argument(
        "--bin-min", type=int, default=5,
        help="Timeline bin size in minutes.",
    )
    return p.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)

    log_path = Path(args.log)
    if not log_path.is_file():
        sys.stderr.write(f"ERROR: log not found: {log_path}\n")
        return 2

    try:
        trading_date = datetime.strptime(args.date, "%Y-%m-%d").replace(tzinfo=timezone.utc)
    except ValueError:
        sys.stderr.write(f"ERROR: bad --date '{args.date}' (want YYYY-MM-DD)\n")
        return 2

    try:
        sh, sm = (int(x) for x in args.start.split(":"))
        eh, em = (int(x) for x in args.end.split(":"))
    except ValueError:
        sys.stderr.write(f"ERROR: bad --start/--end (want HH:MM)\n")
        return 2

    window_start = trading_date.replace(hour=sh, minute=sm, second=0, microsecond=0, tzinfo=timezone.utc)
    window_end   = trading_date.replace(hour=eh, minute=em, second=0, microsecond=0, tzinfo=timezone.utc)
    if window_end <= window_start:
        sys.stderr.write("ERROR: --end must be after --start\n")
        return 2

    out_dir = Path(args.out_dir) if args.out_dir else log_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    # Only pre-populate the 12 entries when the requested date matches 2026-04-17
    if trading_date.date().isoformat() == "2026-04-17":
        entries = build_entries(trading_date)
    else:
        entries = []

    sys.stdout.write(
        f"[omega_session_diagnostic] log       : {log_path}\n"
        f"[omega_session_diagnostic] date      : {trading_date.strftime('%Y-%m-%d')}\n"
        f"[omega_session_diagnostic] window    : {window_start.strftime('%H:%M')}"
        f"-{window_end.strftime('%H:%M')} UTC\n"
        f"[omega_session_diagnostic] bin size  : {args.bin_min} min\n"
        f"[omega_session_diagnostic] entries   : {len(entries)} known\n"
        f"[omega_session_diagnostic] out dir   : {out_dir}\n"
        f"[omega_session_diagnostic] scanning...\n"
    )

    tag_inventory, gate_rejects, bins, total_lines, in_window_lines = scan_log(
        log_path=log_path,
        trading_date=trading_date,
        window_start=window_start,
        window_end=window_end,
        bin_minutes=args.bin_min,
        entries=entries,
    )

    sys.stdout.write(
        f"[omega_session_diagnostic] scanned   : {total_lines:,} lines "
        f"({in_window_lines:,} in window)\n"
        f"[omega_session_diagnostic] tags      : {len(tag_inventory):,} unique\n"
        f"[omega_session_diagnostic] rejects   : {sum(gate_rejects.values()):,} across "
        f"{len(gate_rejects):,} engine/reason pairs\n"
    )

    stem = f"omega_session_{trading_date.strftime('%Y-%m-%d')}"
    html_path     = out_dir / f"{stem}_diagnostic.html"
    tags_csv      = out_dir / f"{stem}_tag_inventory.csv"
    rejects_csv   = out_dir / f"{stem}_gate_rejects.csv"
    entries_csv   = out_dir / f"{stem}_entries.csv"

    write_tag_inventory_csv(tags_csv, tag_inventory)
    write_gate_rejects_csv(rejects_csv, gate_rejects)
    write_entries_csv(entries_csv, entries)

    write_html_report(
        path=html_path,
        log_path=log_path,
        trading_date=trading_date,
        window_start=window_start,
        window_end=window_end,
        bin_minutes=args.bin_min,
        entries=entries,
        tag_inventory=tag_inventory,
        gate_rejects=gate_rejects,
        bins=bins,
        total_lines=total_lines,
        in_window_lines=in_window_lines,
    )

    sys.stdout.write(
        f"[omega_session_diagnostic] wrote     : {html_path}\n"
        f"[omega_session_diagnostic] wrote     : {tags_csv}\n"
        f"[omega_session_diagnostic] wrote     : {rejects_csv}\n"
        f"[omega_session_diagnostic] wrote     : {entries_csv}\n"
        f"[omega_session_diagnostic] done.\n"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
