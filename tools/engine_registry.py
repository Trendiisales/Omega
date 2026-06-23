#!/usr/bin/env python3
# ENGINE REGISTRY (S-2026-06-24) -- the single map the system was missing.
#
# WHY: "what is enabled" (engine_init.hpp), "what is validated + its verdict"
# (backtest/AUDITED_CONFIGS.tsv), and "what counts as the book" (LiveBook.hpp
# allowlist) lived in THREE separate places with nothing joining them -- so an
# engine could be enabled+trading while UNAUDITED and nobody had one view of it.
#
# WHAT: join the three sources into one table. For every enabled entry-engine:
#   verdict (EDGE/MARGINAL/DEAD/SHADOW-CANDIDATE/UNAUDITED) + in-the-book? +
#   bear-gated? (validated engines clear the price-bear gate; the rest are blocked).
# Flags UNAUDITED enabled engines (the real answer to "what's not validated").
# Writes ENGINE_REGISTRY.md. Exit 1 if any enabled engine is UNAUDITED or DEAD.
#
# Usage: python3 tools/engine_registry.py [--repo <dir>]
import os, re, sys

REPO = "."
for i,a in enumerate(sys.argv):
    if a == "--repo" and i+1 < len(sys.argv): REPO = sys.argv[i+1]

def rd(p):
    try: return open(os.path.join(REPO, p), errors="ignore").read()
    except Exception: return ""

# 1. enabled engines: every `g_<name>.enabled = true` in engine_init.hpp + omega_main.hpp
enabled = {}   # global -> source line
for src in ("include/engine_init.hpp", "include/omega_main.hpp"):
    txt = rd(src)
    for m in re.finditer(r'(g_[A-Za-z0-9_]+)\.enabled\s*=\s*true', txt):
        enabled.setdefault(m.group(1), src)

# 2. audit verdicts: AUDITED_CONFIGS.tsv  (engine \t pf \t date \t harness \t verdict \t note)
verdicts = {}  # audit_engine_name -> (verdict, pf)
for ln in rd("backtest/AUDITED_CONFIGS.tsv").splitlines():
    if ln.startswith("#") or not ln.strip(): continue
    c = ln.split("\t")
    if len(c) >= 5: verdicts[c[0].strip()] = (c[4].strip(), c[1].strip())

# non-engines that match the g_<x>.enabled pattern but are infra, not entry engines
DENY = {"g_regime_adaptor", "g_aurora_gate", "g_macro_gold_gate", "g_engine_heartbeat",
        "g_risk_monitor", "g_macro_ctx", "g_adaptive_risk", "g_corr_matrix"}

# 3. livebook allowlist (the validated both-regime book). Each kEdge entry has a
#    trailing comment naming its AUDIT engine (e.g. "NasTurtleD1_US500.F" // SpxTurtleD1).
#    Build the book as the set of AUDIT names so it joins the verdict table cleanly.
lb = rd("include/LiveBook.hpp")
book_audit = set()   # audit-names that are in the validated book
m = re.search(r'kEdge\[\]\s*=\s*\{(.*?)\}', lb, re.S)
if m:
    for line in m.group(1).splitlines():
        tagm = re.search(r'"([A-Za-z0-9_.]+)"', line)
        cmtm = re.search(r'//\s*([A-Za-z0-9_]+)', line)
        if cmtm: book_audit.add(cmtm.group(1))          # the audit-name in the comment

def norm(s): return re.sub(r'[^a-z0-9]', '', s.lower())
audit_norm = {norm(k): k for k in verdicts}

def match_audit(g):
    n = norm(g[2:] if g.startswith("g_") else g)
    if n in audit_norm: return audit_norm[n]
    for an_norm, an in audit_norm.items():
        if an_norm in n or n in an_norm: return an
    return None
def in_book(audit_name):
    return audit_name in book_audit if audit_name else False

# -- HONEST bear-enforcement map (S-2026-06-24, fixes the "bear_cleared = booked"
#    lie) -------------------------------------------------------------------------
# The OLD code asserted every non-booked engine was "BLOCKED-in-bear". That was
# false: the universal price-bear hard-gate lives INSIDE enter_directional
# (trade_lifecycle.hpp), so ONLY engines that route through that chokepoint are
# actually blocked there. The self-entering engines (gold-family, equity, riders)
# bypass enter_directional and rely on their OWN per-engine gates -- some solid
# (trend/regime gates), some LEAKY (the 2022-NEG bull-beta engines). Reporting
# them as chokepoint-"BLOCKED" hid exactly the gap item-2 is about.
#
# So we now report the REAL enforcement path per global. Maintained by hand from
# verified code/AUDITED_CONFIGS evidence; any enabled global NOT listed here is
# surfaced as "?? UNCLASSIFIED" so routing drift can't hide.
#   clears        -> validated both-regime EDGE, allowed in bear (auto from book)
#   chokepoint    -> routes via enter_directional -> hard-blocked in price-bear
#   trend/regime  -> self-enters but has a backtested trend/regime gate (solid)
#   leaky         -> self-enters, own gate is imperfect (2022-NEG) -> item-2 gap
#   overlay       -> overlay on a gated host, inherits host gating
#   SHORT         -> short-only engine, long-bear-gate N/A (trades the bear)
GATE_PATH = {
    "g_dj30_turtle_d1":   "clears (EDGE)",
    "g_spx_turtle_d1":    "clears (EDGE)",
    "g_xau_tf_4h":        "clears (EDGE)",
    "g_fx_xrev_eurgbp":   "clears (EDGE)",
    "g_gold_volbrk_m30":  "self-gate: trend (backtested)",
    "g_mgc_volbrk":       "self-gate: trend (backtested)",
    "g_mgc_fastdon":      "self-gate: EMA100-trend + gold_regime",
    "g_trend_rider":      "self-gate: bull-gate (flat in chop/bear)",
    "g_connors_nas":      "self-gate: SMA200 (sits out bear)",
    "g_rider_4h":         "overlay: host-gated (xau_tf)",
    "g_rider_d1":         "overlay: host-gated (xau_tf)",
    "g_idx_bear_short_nas": "SHORT (trades bear)",
    "g_idx_bear_short_sp":  "SHORT (trades bear)",
    "g_xau_tf_1h":        "LEAKY: D1-EMA200 macro gate (bear-NEG)",
    "g_xau_tf_2h":        "LEAKY: D1-EMA200 macro gate (bear-NEG)",
    "g_xau_tf_d1":        "LEAKY: D1-EMA200 macro gate (bear-NEG)",
    "g_xau_threebar_30m": "LEAKY: own gate, bull-biased (bear-NEG)",
    "g_xau_sess_nypm":    "LEAKY: session gate (both bear halves NEG)",
    "g_bigcap_momo":      "LEAKY: equity, no hard index-bear gate",
}

rows, unaudited, dead, leaky = [], [], [], []
for g in sorted(enabled):
    if g in DENY: continue                            # infra, not an entry engine
    an = match_audit(g)
    if an: verdict, pf = verdicts[an]
    else:  verdict, pf = "UNAUDITED", "-"
    booked = in_book(an)
    # HONEST bear enforcement: validated -> clears; else the real per-engine path.
    if booked:
        bear = "clears (EDGE)"
    else:
        bear = GATE_PATH.get(g, "?? UNCLASSIFIED (verify routing)")
    rows.append((g, verdict, pf, "YES" if booked else "no", bear))
    if verdict == "UNAUDITED": unaudited.append(g)
    if verdict == "DEAD": dead.append(g)
    if bear.startswith("LEAKY") or bear.startswith("??"): leaky.append((g, bear))

out = ["# ENGINE REGISTRY (auto-generated by tools/engine_registry.py)", "",
       f"enabled entry-engines: {len(enabled)}  |  in-book (validated EDGE): {sum(1 for r in rows if r[3]=='YES')}  |  UNAUDITED: {len(unaudited)}  |  DEAD-but-enabled: {len(dead)}  |  LEAKY/unclassified bear-gate: {len(leaky)}", "",
       "bear-enforcement = the REAL path each engine is gated by in a price-bear (not",
       "the old assumption that non-booked == chokepoint-blocked). 'clears' = validated",
       "both-regime EDGE. 'chokepoint' = blocked inside enter_directional. 'self-gate' =",
       "own backtested trend/regime gate. 'LEAKY' = self-enters, imperfect gate (2022-NEG",
       "= the item-2 bear-coverage gap). 'overlay' = inherits a gated host.", "",
       "| engine (global) | verdict | faithful_pf | in book? | bear enforcement |",
       "|---|---|---|---|---|"]
for g, v, pf, b, bg in rows:
    out.append(f"| {g} | {v} | {pf} | {b} | {bg} |")
out += ["", "## UNAUDITED enabled engines (owe a verdict or disable):",
        ("\n".join(f"- {g}" for g in unaudited) if unaudited else "- none"),
        "", "## LEAKY / unclassified bear-gate (item-2 gap: self-enter + imperfect long-bear gate):",
        ("\n".join(f"- {g} -- {why}" for g, why in leaky) if leaky else "- none")]
md = "\n".join(out) + "\n"
open(os.path.join(REPO, "ENGINE_REGISTRY.md"), "w").write(md)
print(md)
print(f"[engine-registry] enabled={len(enabled)} unaudited={len(unaudited)} dead={len(dead)} leaky={len(leaky)}")
sys.exit(1 if (unaudited or dead) else 0)
