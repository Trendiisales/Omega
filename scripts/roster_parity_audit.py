#!/usr/bin/env python3
# ROSTER-PARITY AUDIT (S-2026-07-14, latent-class sweep item 14a/14b)
# ---------------------------------------------------------------------------
# FAILURE CLASS: hand-mirrored duplicates that are in-sync today and WILL drift.
# A roster/contract-map is copied into a second (third) file "to match" the first;
# the next edit lands on one copy only and the divergence is invisible until a
# live symptom (wrong contract subscribed, a ladder name with no feed, ...).
#
# This audit DERIVES every copy from its source file (zero-parse = FAIL naming
# the file/anchor -- a parser silently matching nothing is exactly the blindness
# that let warmup_USTEC.F_H4 rot 94d) and fails on any mismatch.
#
# CHECKS (relationship direction is encoded per check, with the why):
#
#  [1] BIGCAP roster EQUALITY:
#        tools/ibkr_dom_bridge.py  STOCKS = {...}        (L1 capability map)
#        include/engine_init.hpp   BIGCAP_LAD[] = {...}  (ladder book roster)
#      The bridge docstring states "The full 45-name roster mirrors engine_init.hpp
#      BIGCAP_LAD" -> the intended relationship is EXACT set equality. A name only
#      in BIGCAP_LAD = a ladder cell whose live-confirmation gate can never get a
#      quote; a name only in STOCKS = a dead capability entry masking the first case.
#
#  [2] FUTURES CONTRACT MAP -- C++ execution copy vs python feed copy:
#        tools/ibkr_dom_bridge.py        INDEX_FUTURES = {...}   (data-feed contracts)
#        include/IbkrExecutionEngine.hpp init_specs() add(...)   (execution contracts)
#      Relationship: every C++ "FUT" spec (omega sym normalized by stripping a
#      trailing ".F") must exist in INDEX_FUTURES with the SAME (symbol, exchange,
#      currency) -- i.e. C++ SUBSET-OF python. The bridge feeds strictly more
#      instruments than the execution engine trades (VIX/DX/energies are feed-only),
#      so python-only keys are expected and NOT a failure.
#      DOCUMENTED EXCEPTIONS (EXEC_ONLY_FUT): XAUUSD -> GC and XAUUSD.M -> MGC.
#      Gold EXECUTION rides COMEX futures while the gold FEED is spot CMDTY
#      (XAUUSD/SMART special-case in make_contract, not in INDEX_FUTURES); the
#      bridge's own MGC key is the depth-feed contract, a different role. These two
#      rows have no feed counterpart by design -> skipped, listed here on purpose.
#      Per the standing rule the C++ copy is PARSE-AND-COMPARE ONLY -- no
#      single-source refactor of engine headers.
#
#  [3] REGISTER-SCRIPT roster + hand-mirrored comment map:
#        tools/register_omega_ibkr_bridge.ps1
#      (3a) every name in $Symbols = @(...) must be RESOLVABLE by the bridge's
#           make_contract(): INDEX_FUTURES key, STOCKS key, XAUUSD/XAGUSD CMDTY
#           special-case, or a 6-letter FX pair. An unresolvable name = the bridge
#           task boots and throws "Unknown symbol mapping" for it every 5 minutes.
#           Direction: $Symbols SUBSET-OF bridge-resolvable (the bridge capability
#           map is deliberately wider than the subscribed roster -- line budget).
#      (3b) the header comments hand-mirror contract symbols as "SYM(CONTRACT)"
#           tokens (e.g. DJ30(YM), UKBRENT(IPE: COIL)). For every such token whose
#           SYM is an INDEX_FUTURES key, the mapped IBKR symbol must appear inside
#           the parentheses -- catches the comment drifting from the real map.
#
# Not checked on purpose: the C++ CASH rows (FX execution) -- the bridge derives
# FX contracts generically from the 6-letter pair, there is no second literal copy
# to drift. The ps1 prose that merely NAMES symbols without a (CONTRACT) suffix
# carries no mapping claim.
#
# Wired into scripts/mac_canary_engines.sh. Exit 0 = parity, exit 1 = drift or
# a parser matched nothing (fail-closed).
#
# Negative-test hooks: --bridge/--engine-init/--ibkr-exec/--ps1 point a single
# check at a mutated temp copy (used by the S-2026-07-14 proof runs).
# ---------------------------------------------------------------------------
import argparse
import os
import re
import sys

FAILS = []


def fail(msg):
    FAILS.append(msg)
    print(f"  [PARITY-FAIL] {msg}")


def read_lines(path):
    try:
        with open(path, errors="ignore") as fh:
            return fh.read().splitlines()
    except OSError as e:
        fail(f"{path}: unreadable ({e})")
        return None


def block_after_anchor(lines, anchor, end_pat, path):
    """Return (start_lineno_1based, block_lines) between the anchor line and the
    first subsequent line matching end_pat. Missing anchor -> None (caller FAILs)."""
    for i, ln in enumerate(lines):
        if anchor in ln:
            for j in range(i + 1, len(lines)):
                if re.search(end_pat, lines[j]):
                    return i + 1, lines[i:j + 1]
            fail(f"{path}: found anchor '{anchor}' at line {i+1} but no terminator /{end_pat}/")
            return None
    fail(f"{path}: anchor '{anchor}' not found -- roster moved/renamed, parser is blind. "
         f"Update scripts/roster_parity_audit.py.")
    return None


def strip_cxx_comment(ln):
    return ln.split("//", 1)[0]


def strip_ps1_comment(ln):
    return ln.split("#", 1)[0]


# ---------------------------------------------------------------- check 1
def parse_bridge_stocks(bridge_path):
    lines = read_lines(bridge_path)
    if lines is None:
        return None
    blk = block_after_anchor(lines, "STOCKS = {", r"^\s*\}", bridge_path)
    if blk is None:
        return None
    lineno, body = blk
    names = set()
    for ln in body:
        names.update(re.findall(r'"([A-Z][A-Z0-9.]*)"', ln))
    if not names:
        fail(f"{bridge_path}:{lineno}: STOCKS block parsed to ZERO tickers -- format drifted, parser blind.")
        return None
    return names, lineno


def parse_bigcap_lad(engine_init_path):
    lines = read_lines(engine_init_path)
    if lines is None:
        return None
    blk = block_after_anchor(lines, "static const char* BIGCAP_LAD[] = {", r"\};", engine_init_path)
    if blk is None:
        return None
    lineno, body = blk
    names = set()
    for ln in body:
        names.update(re.findall(r'"([A-Z][A-Z0-9.]*)"', strip_cxx_comment(ln)))
    if not names:
        fail(f"{engine_init_path}:{lineno}: BIGCAP_LAD block parsed to ZERO tickers -- format drifted, parser blind.")
        return None
    return names, lineno


def check_bigcap(bridge_path, engine_init_path):
    print("[1] BIGCAP roster: bridge STOCKS  ==  engine_init BIGCAP_LAD")
    a = parse_bridge_stocks(bridge_path)
    b = parse_bigcap_lad(engine_init_path)
    if a is None or b is None:
        return
    stocks, sline = a
    bigcap, bline = b
    only_bridge = sorted(stocks - bigcap)
    only_engine = sorted(bigcap - stocks)
    if only_bridge:
        fail(f"{bridge_path}:{sline}: STOCKS has names NOT in BIGCAP_LAD ({engine_init_path}:{bline}): "
             f"{', '.join(only_bridge)} -- dead capability entries (or a BIGCAP_LAD cull missed the bridge).")
    if only_engine:
        fail(f"{engine_init_path}:{bline}: BIGCAP_LAD has names NOT in bridge STOCKS ({bridge_path}:{sline}): "
             f"{', '.join(only_engine)} -- ladder cells whose live-confirmation gate can NEVER get an L1 quote.")
    if not only_bridge and not only_engine:
        print(f"    ok: {len(stocks)} names, exact match")


# ---------------------------------------------------------------- check 2
def parse_index_futures(bridge_path):
    lines = read_lines(bridge_path)
    if lines is None:
        return None
    blk = block_after_anchor(lines, "INDEX_FUTURES = {", r"^\}", bridge_path)
    if blk is None:
        return None
    lineno, body = blk
    rx = re.compile(r'^\s*"([A-Z0-9.]+)":\s*dict\(\s*symbol="([^"]+)",\s*'
                    r'exchange="([^"]+)",\s*currency="([^"]+)"\s*\)')
    out = {}
    for ln in body:
        m = rx.match(ln)
        if m:
            out[m.group(1)] = (m.group(2), m.group(3), m.group(4))
    if not out:
        fail(f"{bridge_path}:{lineno}: INDEX_FUTURES parsed to ZERO entries -- format drifted, parser blind.")
        return None
    return out, lineno


# Execution-only futures rows with NO feed counterpart BY DESIGN (see header):
# gold execution = COMEX futures; gold feed = spot CMDTY special-case + the MGC
# depth-feed key, which is a different role than XAUUSD.M order routing.
EXEC_ONLY_FUT = {"XAUUSD", "XAUUSD.M"}

# S-2026-07-18 real-money cutover: EXECUTION trades the MICRO tier while the
# FEED keeps watching the deep full-size book (same underlying, prices track
# 1:1; signals stay identical to the backtested feed). A micro exec contract
# is therefore parity-OK against its documented mini feed counterpart — and
# ONLY these pairs; any other divergence is still a FAIL.
MICRO_EXEC_OF_FEED_MINI = {"MNQ": "NQ", "MES": "ES", "MYM": "YM", "MGC": "GC"}


def parse_exec_specs(exec_path):
    lines = read_lines(exec_path)
    if lines is None:
        return None
    rx = re.compile(r'^\s*add\("([^"]+)",\s*\{"([^"]+)",\s*"([^"]+)",\s*"([^"]+)",\s*"([^"]+)"')
    out = {}
    first = None
    for i, ln in enumerate(lines):
        m = rx.match(strip_cxx_comment(ln))
        if m:
            first = first or (i + 1)
            om, ib, sec, exch, cur = m.groups()
            out[om] = (ib, sec, exch, cur, i + 1)
    if not out:
        fail(f"{exec_path}: init_specs() add(...) table parsed to ZERO rows -- format drifted, parser blind.")
        return None
    return out


def check_futures_map(bridge_path, exec_path):
    print("[2] futures contract map: IbkrExecutionEngine FUT rows  SUBSET-OF  bridge INDEX_FUTURES")
    idx = parse_index_futures(bridge_path)
    specs = parse_exec_specs(exec_path)
    if idx is None or specs is None:
        return
    index_futures, iline = idx
    fut_rows = {k: v for k, v in specs.items() if v[1] == "FUT"}
    if not fut_rows:
        fail(f"{exec_path}: parsed {len(specs)} spec rows but ZERO with secType FUT -- parser blind to futures.")
        return
    checked = 0
    for om, (ib, _sec, exch, cur, lineno) in sorted(fut_rows.items()):
        if om in EXEC_ONLY_FUT:
            continue
        key = om[:-2] if om.endswith(".F") else om
        if key not in index_futures:
            fail(f"{exec_path}:{lineno}: execution FUT spec '{om}' ({ib}/{exch}/{cur}) has NO "
                 f"INDEX_FUTURES entry '{key}' in {bridge_path}:{iline} -- the engine would trade a "
                 f"contract the feed bridge does not know.")
            continue
        want = index_futures[key]
        got = (ib, exch, cur)
        micro_ok = (MICRO_EXEC_OF_FEED_MINI.get(ib) == want[0]
                    and (exch, cur) == (want[1], want[2]))
        if want != got and not micro_ok:
            fail(f"{exec_path}:{lineno}: '{om}' = {got} but {bridge_path} INDEX_FUTURES['{key}'] = {want} "
                 f"-- feed and execution disagree on the contract.")
        else:
            checked += 1
            if micro_ok:
                print(f"    ok(micro): exec '{om}' trades {ib} against feed {want[0]} "
                      f"(documented micro-exec/mini-feed pair)")
    print(f"    ok: {checked} FUT rows match (+{len(EXEC_ONLY_FUT & set(fut_rows))} documented exec-only gold rows skipped; "
          f"{len(index_futures) - checked} bridge-only feed entries allowed by direction)")


# ---------------------------------------------------------------- check 3
def parse_ps1_symbols(ps1_path):
    lines = read_lines(ps1_path)
    if lines is None:
        return None
    blk = block_after_anchor(lines, "$Symbols = @(", r"\)\s*-join", ps1_path)
    if blk is None:
        return None
    lineno, body = blk
    syms = []
    for ln in body:
        syms.extend(re.findall(r"'([A-Z0-9.]+)'", strip_ps1_comment(ln)))
    if not syms:
        fail(f"{ps1_path}:{lineno}: $Symbols block parsed to ZERO symbols -- format drifted, parser blind.")
        return None
    return syms, lineno


def check_ps1(bridge_path, ps1_path):
    print("[3] register_omega_ibkr_bridge.ps1: $Symbols resolvable + comment map agrees")
    idx = parse_index_futures(bridge_path)
    st = parse_bridge_stocks(bridge_path)
    ps = parse_ps1_symbols(ps1_path)
    if idx is None or st is None or ps is None:
        return
    index_futures, _ = idx
    stocks, _ = st
    syms, sline = ps

    # (3a) every subscribed symbol must be resolvable by make_contract()
    bad = []
    for s in syms:
        u = s[:-2] if s.endswith(".F") else s
        resolvable = (u in index_futures or u in stocks
                      or u in ("XAUUSD", "XAGUSD")               # CMDTY special-cases
                      or (len(u) == 6 and u.isalpha()))          # FX pair fallback
        if not resolvable:
            bad.append(s)
    if bad:
        fail(f"{ps1_path}:{sline}: $Symbols entries the bridge can NOT resolve: {', '.join(bad)} "
             f"-- the task would throw 'Unknown symbol mapping' on boot.")
    else:
        print(f"    ok: all {len(syms)} $Symbols resolvable by the bridge")

    # (3b) hand-mirrored comment tokens "SYM(CONTRACT)" must agree with INDEX_FUTURES
    lines = read_lines(ps1_path) or []
    checked = drifted = 0
    for i, ln in enumerate(lines):
        for sym, paren in re.findall(r"\b([A-Z][A-Z0-9]+)\(([^)]*)\)", ln):
            if sym not in index_futures:
                continue  # not a mapping claim about a known feed contract
            words = set(re.findall(r"[A-Z0-9]+", paren))
            want = index_futures[sym][0]
            checked += 1
            if want not in words:
                drifted += 1
                fail(f"{ps1_path}:{i+1}: comment claims {sym}({paren}) but INDEX_FUTURES maps "
                     f"{sym} -> {want} -- hand-mirrored comment drifted from the real map.")
    if checked == 0:
        fail(f"{ps1_path}: found ZERO 'SYM(CONTRACT)' comment tokens for known INDEX_FUTURES keys -- "
             f"the header format drifted, comment-parity parser is blind.")
    elif drifted == 0:
        print(f"    ok: {checked} comment mapping token(s) agree with INDEX_FUTURES")


# ---------------------------------------------------------------- check 4
# The BIGCAP roster is ALSO hand-copied into the Python data toolchain (the 5m
# puller, the daily-close feed, the IBKR close refresher, the day-mover screen).
# check [1] only polices the two C++/bridge copies -- a name added to BIGCAP_LAD
# but missed in vps_stockmover_feed.py means the new ladder cell gets NO daily
# close (the sp500_long_close frozen-mark class); missed in ibkr_stock5m_pull.py
# means the backtest silently omits it. Direction is per-file:
#   EXACT ( == BIGCAP_LAD ): every Python copy that touches the 45-name universe.
#     (S-2026-07-17: daymover_pername_screen.py reconciled 39->45 and moved from
#     SUBSET to EXACT on operator instruction -- "ensure it cannot drift again".
#     All four python copies are now locked exact to the ladder roster.)
PY_ROSTERS_EXACT = [
    os.path.join("backtest", "ibkr_stock5m_pull.py"),
    os.path.join("tools", "rdagent", "vps_stockmover_feed.py"),
    os.path.join("tools", "rdagent", "refresh_close_ibkr.py"),
    os.path.join("tools", "rdagent", "daymover_pername_screen.py"),
]
PY_ROSTERS_SUBSET = []


def parse_py_bigcap(path):
    """Names in a `BIGCAP = ( "..." "..." ).split()` / `BIGCAP = "...".split()`
    assignment, python comments stripped. Zero-parse -> FAIL (parser blind)."""
    lines = read_lines(path)
    if lines is None:
        return None
    text = "\n".join(strip_ps1_comment(ln) for ln in lines)  # '#' comment stripper
    m = re.search(r"BIGCAP\s*=\s*(.*?)\.split\(", text, re.S)
    if m is None:
        fail(f"{path}: no `BIGCAP = ... .split(` assignment found -- roster moved/renamed, "
             f"parser blind. Update scripts/roster_parity_audit.py.")
        return None
    names = set(re.findall(r"[A-Z][A-Z0-9.]{1,6}", m.group(1)))
    if not names:
        fail(f"{path}: BIGCAP assignment parsed to ZERO tickers -- format drifted, parser blind.")
        return None
    return names


def check_py_rosters(engine_init_path, repo):
    print("[4] python BIGCAP copies  vs  engine_init BIGCAP_LAD")
    b = parse_bigcap_lad(engine_init_path)
    if b is None:
        return
    bigcap, bline = b
    for rel in PY_ROSTERS_EXACT:
        p = os.path.join(repo, rel)
        s = parse_py_bigcap(p)
        if s is None:
            continue
        extra = sorted(s - bigcap)
        missing = sorted(bigcap - s)
        if extra:
            fail(f"{p}: BIGCAP has names NOT in BIGCAP_LAD ({engine_init_path}:{bline}): "
                 f"{', '.join(extra)} -- a stale name pulled/fed but no longer in the ladder.")
        if missing:
            fail(f"{p}: BIGCAP MISSING ladder names ({engine_init_path}:{bline}): "
                 f"{', '.join(missing)} -- this data path silently omits them "
                 f"(new ladder cell gets no feed / backtest skips the name).")
        if not extra and not missing:
            print(f"    ok (exact): {rel} == {len(bigcap)} names")
    for rel in PY_ROSTERS_SUBSET:
        p = os.path.join(repo, rel)
        s = parse_py_bigcap(p)
        if s is None:
            continue
        outside = sorted(s - bigcap)
        if outside:
            fail(f"{p}: BIGCAP has names OUTSIDE the ladder roster ({engine_init_path}:{bline}): "
                 f"{', '.join(outside)} -- a screen name that is not a real ladder cell.")
        else:
            print(f"    ok (subset<=BIGCAP_LAD): {rel} = {len(s)}/{len(bigcap)} names")


# ---------------------------------------------------------------- check 5
# Inside the BIGCAP ladder loop (engine_init.hpp ~2399) two more hand-lists set
# per-name TRADE TREATMENT keyed off the roster:
#   AGG_ELITE[] -> 2x notional / 2x retirement bar
#   AGG_OUT[]   -> ranked_out (no new windows)
# A name in either that is NOT in BIGCAP_LAD is a silent no-op (the inner match
# loop never fires) -- so dropping a name from the roster while leaving it in
# AGG_OUT/AGG_ELITE means the ranking list drifts from the traded list unseen.
# elite and out must also be DISJOINT (a name can't be both 2x and ranked-out).
def parse_cxx_name_array(path, anchor):
    lines = read_lines(path)
    if lines is None:
        return None
    blk = block_after_anchor(lines, anchor, r"\};", path)
    if blk is None:
        return None
    lineno, body = blk
    names = set()
    for ln in body:
        names.update(re.findall(r'"([A-Z][A-Z0-9.]*)"', strip_cxx_comment(ln)))
    if not names:
        fail(f"{path}:{lineno}: '{anchor}' parsed to ZERO tickers -- format drifted, parser blind.")
        return None
    return names, lineno


def check_bigcap_ranking(engine_init_path):
    print("[5] BIGCAP per-name ranking lists (AGG_ELITE / AGG_OUT)  SUBSET-OF  BIGCAP_LAD, disjoint")
    b = parse_bigcap_lad(engine_init_path)
    e = parse_cxx_name_array(engine_init_path, "AGG_ELITE[] = {")
    o = parse_cxx_name_array(engine_init_path, "AGG_OUT[]   = {")
    if b is None or e is None or o is None:
        return
    bigcap, bline = b
    elite, eline = e
    out, oline = o
    e_bad = sorted(elite - bigcap)
    o_bad = sorted(out - bigcap)
    both = sorted(elite & out)
    if e_bad:
        fail(f"{engine_init_path}:{eline}: AGG_ELITE names NOT in BIGCAP_LAD ({bline}): "
             f"{', '.join(e_bad)} -- 2x-notional tag on a name the ladder no longer trades (silent no-op).")
    if o_bad:
        fail(f"{engine_init_path}:{oline}: AGG_OUT names NOT in BIGCAP_LAD ({bline}): "
             f"{', '.join(o_bad)} -- ranked-out tag on a name not in the roster (silent no-op).")
    if both:
        fail(f"{engine_init_path}: name(s) in BOTH AGG_ELITE and AGG_OUT: {', '.join(both)} "
             f"-- a name cannot be 2x-elite and ranked-out at once.")
    if not e_bad and not o_bad and not both:
        print(f"    ok: AGG_ELITE ({len(elite)}) + AGG_OUT ({len(out)}) all in BIGCAP_LAD, disjoint")


def main():
    ap = argparse.ArgumentParser(description="hand-mirrored roster/contract-map parity audit")
    ap.add_argument("--repo", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--bridge", help="override tools/ibkr_dom_bridge.py (negative tests)")
    ap.add_argument("--engine-init", help="override include/engine_init.hpp")
    ap.add_argument("--ibkr-exec", help="override include/IbkrExecutionEngine.hpp")
    ap.add_argument("--ps1", help="override tools/register_omega_ibkr_bridge.ps1")
    a = ap.parse_args()
    repo = os.path.abspath(a.repo)
    bridge = a.bridge or os.path.join(repo, "tools", "ibkr_dom_bridge.py")
    einit = a.engine_init or os.path.join(repo, "include", "engine_init.hpp")
    iexec = a.ibkr_exec or os.path.join(repo, "include", "IbkrExecutionEngine.hpp")
    ps1 = a.ps1 or os.path.join(repo, "tools", "register_omega_ibkr_bridge.ps1")

    print("===== ROSTER-PARITY AUDIT (hand-mirrored duplicates) =====")
    check_bigcap(bridge, einit)
    check_futures_map(bridge, iexec)
    check_ps1(bridge, ps1)
    check_py_rosters(einit, repo)
    check_bigcap_ranking(einit)

    if FAILS:
        print(f"\nFAIL: {len(FAILS)} parity violation(s) -- a hand-mirrored copy drifted (or a parser went blind).")
        return 1
    print("\nPASS: all hand-mirrored rosters/contract maps in sync.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
