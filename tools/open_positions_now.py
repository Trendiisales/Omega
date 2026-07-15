#!/usr/bin/env python3
"""OPEN-POSITIONS-NOW — the ONE authoritative "what is open right now" check.

WHY THIS EXISTS (operator, furious, recurring — 2026-07-16):
Sessions kept answering "nothing open / we're flat" by reading Mac-LOCAL files —
`open_positions.json` (parent only) and stale `fxladder_companion_*_live.txt`
snapshots left in the repo root from when the companion ran on Mac. Those files
have not updated since 2026-07-09/10; the live companion books run on the VPS.
On 2026-07-15 the parent ledger said flat and the local companion txt said
"win 0 0 0 0" while the VPS desk had 4 GBPUSD ladder legs + an armed ETH leg
OPEN. The session misread "flat". Same discussion every day.

ROOT CAUSE: companion books are SEPARATE INDEPENDENT ENGINES with their own
open-leg state (CompanionDominanceError separation). Parent open_positions=0 does
NOT mean the companion books are flat. And the authoritative state lives on the
VPS desk API, never in a Mac-local file.

THIS TOOL: hits the authoritative VPS desk API (read-only GET) and prints EVERY
open position / open leg across the PARENT and ALL companion books + crypto, from
ONE source that cannot be faked by a stale local file. It also scans the repo for
stale local companion artifacts and LOUDLY flags them so they get killed, not read.

Run it whenever you (or the operator) ask "what's open?":
    python3 tools/open_positions_now.py
Override the desk with OMEGA_DESK_URL. READ-ONLY — never POSTs (audit-safe).
"""
from __future__ import annotations
import json, os, sys, time, glob, subprocess, urllib.request

# ---- authoritative desk (VPS omega-new = 45.85.3.79, telemetry http 7779) ----
def desk_url() -> str:
    if os.environ.get("OMEGA_DESK_URL"):
        return os.environ["OMEGA_DESK_URL"]
    # resolve the live host the same way the other selftests do
    try:
        out = subprocess.run(["ssh", "-G", "omega-new"], capture_output=True,
                             text=True, timeout=5).stdout
        for ln in out.splitlines():
            if ln.startswith("hostname "):
                return "http://%s:7779" % ln.split()[1]
    except Exception:
        pass
    return "http://45.85.3.79:7779"   # last-known live IP

DESK = desk_url()

def get(path: str):
    """GET-only. Returns parsed json or None (never mutates)."""
    try:
        with urllib.request.urlopen(DESK + path, timeout=12) as r:
            return json.load(r)
    except Exception as e:
        return {"__error__": str(e)}

# ---- collectors: each returns a list of open-leg dicts -----------------------
def _ladder_pairs(j, book, key="pairs"):
    rows = []
    if not isinstance(j, dict) or "__error__" in j:
        return rows, j.get("__error__") if isinstance(j, dict) else "no-data"
    for p in j.get(key, []):
        sym = p.get("live_sym") or p.get("pair") or p.get("sym") or "?"
        for o in p.get("open", []):
            rows.append(dict(book=book, sym=sym, leg=o.get("tier") or o.get("flavor") or "-",
                             dir=o.get("dir", "?"), entry=o.get("entry"),
                             cur=o.get("cur"), armed=o.get("armed"),
                             upnl_usd=o.get("upnl_usd")))
    return rows, None

def collect_parent():
    j = get("/api/telemetry")
    rows = []
    if isinstance(j, dict) and "__error__" not in j:
        for o in j.get("open_positions", []):
            rows.append(dict(book="PARENT", sym=o.get("sym") or o.get("symbol") or "?",
                             leg=o.get("engine") or "-", dir=o.get("dir", "?"),
                             entry=o.get("entry"), cur=o.get("cur"),
                             armed=None, upnl_usd=o.get("upnl_usd") or o.get("unrealised")))
        return rows, None
    return rows, j.get("__error__") if isinstance(j, dict) else "no-data"

def collect_crypto():
    j = get("/api/crypto_companion")
    rows = []
    if not isinstance(j, dict) or "__error__" in j:
        return rows, j.get("__error__") if isinstance(j, dict) else "no-data"
    for l in j.get("legs", []):
        # crypto: a leg is "open" when it has an armed/open subleg (banking in flight)
        subs = l.get("sublegs", [])
        live = [s for s in subs if s.get("armed") or s.get("open")]
        if not live:
            continue
        for s in live:
            rows.append(dict(book="CRYPTO", sym=l.get("sym") or l.get("coin") or "?",
                             leg="%s/%s" % (l.get("tag", "?"), s.get("id", "?")),
                             dir="long", entry=None, cur=None,
                             armed=s.get("armed"),
                             upnl_usd=None, mfe=s.get("peak_mfe_pct")))
    return rows, None

# ---- stale local decoy scan --------------------------------------------------
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Companion book state files. The live books run on the VPS — NOTHING on Mac writes
# these. Any match in the Mac repo is a decoy, at ANY age (a FRESH one is the most
# dangerous: it looks authoritative). We do NOT list parent open_positions.json here:
# parent being flat was correct on 2026-07-15; the miss was not checking the companions.
STALE_PATTERNS = [
    "fxladder_companion_*_live.txt", "fxladder_companion_*_book.txt",
    "fxladder_companion_*_h1.csv", "fxladder_companion_*_closed.csv",
    "*_companion_state.json",
]

def scan_stale():
    hits = []
    now = time.time()
    for pat in STALE_PATTERNS:
        for f in glob.glob(os.path.join(REPO, pat)) + glob.glob(os.path.join(REPO, "data", pat)):
            age_h = (now - os.path.getmtime(f)) / 3600.0
            hits.append((os.path.relpath(f, REPO), age_h))   # flag at any age
    return sorted(hits)

# ---- main --------------------------------------------------------------------
def main():
    print("OPEN-POSITIONS-NOW  (authoritative: %s)  READ-ONLY" % DESK)
    print("=" * 78)
    all_rows, errs = [], []
    collectors = [
        ("parent",            collect_parent),
        ("fx-ladder",         lambda: _ladder_pairs(get("/api/fxladder_companion"), "FX-LADDER")),
        ("index-ladder",      lambda: _ladder_pairs(get("/api/idxladder_companion"), "IDX-LADDER")),
        ("stock-ladder",      lambda: _ladder_pairs(get("/api/stockladder_companion"), "STK-LADDER", "names")),
        ("bigcap-2pct",       lambda: _ladder_pairs(get("/api/bigcap2pct_companion"), "BIGCAP2PCT", "names")),
        ("crypto-companion",  collect_crypto),
    ]
    for name, fn in collectors:
        rows, err = fn()
        all_rows += rows
        if err:
            errs.append((name, err))

    if all_rows:
        print("%-11s %-9s %-16s %-5s %-11s %-6s %-9s" %
              ("BOOK", "SYM", "LEG", "DIR", "ENTRY", "ARMED", "uPnL$"))
        print("-" * 78)
        for r in all_rows:
            entry = "%.5f" % r["entry"] if isinstance(r.get("entry"), (int, float)) else "-"
            up = ("%+.0f" % r["upnl_usd"]) if isinstance(r.get("upnl_usd"), (int, float)) else "-"
            armed = {True: "yes", False: "no", None: "-"}.get(r.get("armed"), "-")
            extra = ("  mfe=%.2f%%" % r["mfe"]) if r.get("mfe") is not None else ""
            print("%-11s %-9s %-16s %-5s %-11s %-6s %-9s%s" %
                  (r["book"], r["sym"], str(r["leg"])[:16], r["dir"], entry, armed, up, extra))
    print("-" * 78)
    print("OPEN LEGS TOTAL: %d   across %d book(s)" %
          (len(all_rows), len({r["book"] for r in all_rows})))

    if errs:
        print("\nENDPOINT ERRORS (state UNKNOWN for these — do NOT assume flat):")
        for name, e in errs:
            print("  ! %-18s %s" % (name, e))

    stale = scan_stale()
    if stale:
        print("\n⚠ STALE LOCAL DECOY FILES in repo (DO NOT READ — authoritative source is the API above):")
        for f, age in stale:
            print("  DECOY  %-46s  %.1fh old" % (f, age))
        print("  -> these are Mac leftovers; the live books run on the VPS. Delete or ignore them.")

    # exit codes so a cron/caller can distinguish:
    #   3 = stale local decoy files present (the recurring misread source) -> ALERT
    #   2 = an endpoint errored (state UNKNOWN, do NOT assume flat)
    #   0 = clean read (flat or open, both normal)
    if stale:
        sys.exit(3)
    sys.exit(2 if errs else 0)

if __name__ == "__main__":
    main()
