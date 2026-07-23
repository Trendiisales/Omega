#!/usr/bin/env python3
# fail_verdict_guard.py (S-2026-07-12, never-again audit) -- FAIL-verdict enforcement.
#
# WHY: the tombstone guard (tools/tombstone_guard.py) blocks TOMBSTONED engines, but on
# 2026-07-12 three engines with recorded true-cost FAIL backtests (GOLD_PHASE1B) were
# still enabled=true and live until the operator ordered a cull. A FAIL verdict carried
# no enforcement -- an engine could fail a true-cost backtest and keep trading.
#
# WHAT: read backtest/FAIL_VERDICTS.tsv (the FAIL disable-list). Resolve every enabled
# engine identity in engine_init.hpp + omega_main.hpp. If any listed identity is enabled
# -> FAIL (exit 1), LOUD, with the verdict ref. Re-enabling requires removing the row
# WITH a new passing backtest ref in the same commit (see the tsv header).
#
# ENABLE SHAPES RECOGNIZED (S-2026-07-14 widen, latent-class sweep item 11 -- the
# original parser saw ONLY the literal `g_X.enabled = true`, so the sleeve/cell boot
# shapes below were INVISIBLE and a FAIL-verdict sleeve engine could be re-enabled
# without tripping this guard):
#   (a) direct:  g_X.enabled = true            -> g_X
#       dotted:  g_edges.tod.enabled = true    -> g_edges.tod   (member sub-engine)
#   (b) boot-lambda sleeve (engine_init.hpp:3018 idx_seas_boot family, seed_xs,
#       tom_boot, idx_fomc_boot, cfg_mr):
#         auto NAME = [..](Type& P, ...){ ... P.enabled = true; ... };
#         NAME(g_Y, ...)                       -> g_Y
#       The roster is DERIVED from the call sites in the same file that builds it
#       (derive-don't-copy) -- never a hand-copied name list.
#   (c) roster loop (engine_init.hpp MondayRiskOn mons[] idiom):
#         const T ARR[] = { { &g_Z, ... }, ... };
#         for (const auto& V : ARR) { ... V.<member>->enabled = true; ... }
#                                              -> g_Z (every &g_ in ARR's initializer)
#   (d) known non-global singleton accessors (KNOWN_NONGLOBAL_SHAPES below) --
#       documented, cannot currently carry a g_-keyed FAIL_VERDICTS row.
# ZERO-PARSE GUARDS (unknown shape may NOT pass silently -- the persistence_audit
# idiom): a sleeve lambda that sets P.enabled=true but resolves 0 g_ call sites, a
# roster loop whose array initializer yields 0 &g_ identities, or ANY other
# `X.enabled = true` line not attributable to (a)-(d), exits 2 naming file:line.
# A final catch-all pass re-scans for EVERY `.enabled = true` / `->enabled = true`
# token and fails on any occurrence the chain classifier did not consume (e.g. a
# subscripted LHS like `book.cells[0].enabled = true` that no chain regex matches).
#
# Wired into scripts/mac_canary_engines.sh (after the tombstone guard) so a FAIL-verdict
# engine cannot be re-enabled and committed silently.
# Run: python3 tools/fail_verdict_guard.py [--repo <dir>]
import os, re, sys

REPO = "."
for i, a in enumerate(sys.argv):
    if a == "--repo" and i + 1 < len(sys.argv):
        REPO = sys.argv[i + 1]

def rd(p):
    try:
        return open(os.path.join(REPO, p), errors="ignore").read()
    except Exception:
        return ""

# Non-global enable shapes that are KNOWN and documented (regex, reason). If one of
# these ever earns a FAIL verdict, it needs a resolvable identity mapping added here
# in the same commit as the tsv row.
KNOWN_NONGLOBAL_SHAPES = [
    (re.compile(r'^(omega::)?gold_wt\(\)$'),
     "GoldWaveTrend momentum-confirm GATE singleton (GoldWaveTrend.hpp:133 accessor); "
     "a filter, not a position engine"),
    (re.compile(r'^gd\.cfg$'),
     "GoldDailyCbe local ref (auto& gd = g_gold_daily_cbe, engine_init S-22i block); "
     "resolves to global g_gold_daily_cbe -- CERTIFIED PASS engine "
     "(backtest/GOLD_DAILY_CBE_FINDINGS_2026-07-22.md), no FAIL verdict exists for it"),
    (re.compile(r'^gdm\.cfg$'),
     "GoldDailyCbeMimic local ref (auto& gdm = omega::gold_daily_cbe_mimic(), engine_init "
     "S-22i block); x2 companion CERTIFIED PASS standalone (same findings doc, MIMIC=1 "
     "grid), no FAIL verdict exists for it"),
    (re.compile(r'^dm\.cfg$'),
     "DualMomentumEngine local ref (auto& dm = omega::dual_momentum_engine(), engine_init "
     "S-23a block); CERTIFIED PASS (backtest/dualmom_sweep.py + DD/whipsaw lever passes; "
     "cert RESTATED S-23l to keff mechanism Sharpe 1.66/mdd 29.9, still beats ctrl 1.34), "
     "no FAIL verdict exists for it"),
    (re.compile(r'^dm7\.cfg$'),
     "DayMover7Engine local ref (auto& dm7 = omega::day_mover7_engine(), engine_init S-23m "
     "block); CERTIFIED PASS thr8/cap32 DD-min cell (BULLGATE_PROTECTION_SWEEPS_2026-07-23.md "
     "PF 4.34, 2022 TRADED +164, ex-WDC PASS), no FAIL verdict exists for it"),
    (re.compile(r'^b3\.cfg$'),
     "Bigcap3G4Engine local ref (auto& b3 = omega::bigcap3_g4_engine(), engine_init S-23 "
     "block); CERTIFIED PASS G4+VS cell (same doc: PF 1.29 MAR 8.3, 2022 TRADED +23.5 "
     "PF 1.10, ex-RGTI PASS). NOTE the ungated 3%-family FAIL verdicts (BIGCAP_3PCT_HONEST_"
     "EDGE_2026-07-21e) applied to the UNGATED multi-day config -- the G4 composite gate + "
     "vol-shorten-hold cell is a DIFFERENT certified config, traded-2022-positive"),
]

def find_brace_span(text, open_idx):
    """Given index of '{', return index one past its matching '}' (or None)."""
    depth = 0
    for j in range(open_idx, len(text)):
        c = text[j]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return j + 1
    return None

def line_of(text, idx):
    return text.count("\n", 0, idx) + 1

# 1. FAIL-verdict list
FAILS = {}  # identity -> dict(engine, date, ref, reason)
for ln in rd("backtest/FAIL_VERDICTS.tsv").splitlines():
    if ln.startswith("#") or not ln.strip():
        continue
    c = ln.split("\t")
    if len(c) >= 5 and c[0].startswith("g_"):
        FAILS[c[0].strip()] = dict(engine=c[1].strip(), date=c[2].strip(),
                                   ref=c[3].strip(), reason=c[4].strip())

# 2. enabled identities (all recognized shapes)
enabled = set()
structural = []   # [(src, line, msg)] -- zero-parse / unknown-shape failures
n_direct = n_sleeve = n_roster = n_known = 0

# LHS chain before ".enabled = true" / "->enabled = true":
#   g_connors_ger | g_edges.tod | e | m.e | omega::gold_wt()
EN_RE = re.compile(
    r'([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*(?:\(\))?'
    r'(?:(?:\.|->)[A-Za-z_][A-Za-z0-9_]*(?:\(\))?)*)\s*(?:\.|->)\s*enabled\s*=\s*true')
LAMBDA_RE = re.compile(
    r'auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(\s*(?:const\s+)?[A-Za-z_][\w:]*&\s*(\w+)\b')
FOR_RE = re.compile(r'for\s*\(\s*(?:const\s+)?auto&\s*(\w+)\s*:\s*(\w+)\s*\)')

for src in ("include/engine_init.hpp", "include/omega_main.hpp"):
    text = rd(src)
    if not text:
        continue

    # -- (b) boot-lambda sleeves: name -> (param, body_span, decl_line)
    sleeves = []
    for m in LAMBDA_RE.finditer(text):
        name, param = m.group(1), m.group(2)
        ob = text.find("{", m.end())
        if ob < 0:
            continue
        end = find_brace_span(text, ob)
        if end is None:
            continue
        body = text[ob:end]
        if re.search(r'\b%s\s*(?:\.|->)\s*enabled\s*=\s*true' % re.escape(param), body):
            sleeves.append(dict(name=name, param=param, span=(ob, end),
                                line=line_of(text, m.start())))
    for s in sleeves:
        # derive the roster from the call sites in this same file
        calls = re.findall(r'\b%s\s*\(\s*(g_[A-Za-z0-9_]+)' % re.escape(s["name"]), text)
        if not calls:
            structural.append((src, s["line"],
                "sleeve lambda '%s' sets %s.enabled=true but 0 g_ call sites resolved "
                "(roster derivation came up empty -- parser or code shape changed)"
                % (s["name"], s["param"])))
            continue
        for g in calls:
            enabled.add(g); n_sleeve += 1

    # -- (c) roster loops: loop-var -> (array name, body span, line)
    rosters = []
    for m in FOR_RE.finditer(text):
        var, arr = m.group(1), m.group(2)
        ob = text.find("{", m.end())
        if ob < 0:
            continue
        end = find_brace_span(text, ob)
        if end is None:
            continue
        rosters.append(dict(var=var, arr=arr, span=(ob, end),
                            line=line_of(text, m.start())))

    def roster_for(pos, chain_first):
        for r in rosters:
            if r["span"][0] <= pos < r["span"][1] and r["var"] == chain_first:
                return r
        return None

    def sleeve_for(pos, chain):
        for s in sleeves:
            if s["span"][0] <= pos < s["span"][1] and s["param"] == chain:
                return s
        return None

    # -- classify every enabled=true assignment
    consumed = []   # spans of assignments the chain classifier saw
    for m in EN_RE.finditer(text):
        consumed.append(m.span())
        chain = m.group(1)
        if chain.startswith("g_"):                      # (a) direct / dotted
            enabled.add(chain.replace("->", "."))
            n_direct += 1
            continue
        first = re.split(r'\.|->', chain)[0]
        if sleeve_for(m.start(), chain):                # (b) inside a resolved sleeve
            continue                                    #     (roster added above)
        r = roster_for(m.start(), first)                # (c) roster loop member
        if r is not None:
            init_m = re.search(r'\b%s\s*\[\s*\]\s*=\s*{' % re.escape(r["arr"]), text)
            ids = []
            if init_m:
                init_end = find_brace_span(text, init_m.end() - 1)
                if init_end is not None:
                    ids = re.findall(r'&\s*(g_[A-Za-z0-9_]+)', text[init_m.end() - 1:init_end])
            if not ids:
                structural.append((src, line_of(text, m.start()),
                    "roster loop '%s : %s' enables via '%s' but 0 &g_ identities resolved "
                    "from the %s[] initializer (derive came up empty)"
                    % (r["var"], r["arr"], chain, r["arr"])))
                continue
            for g in ids:
                enabled.add(g); n_roster += 1
            continue
        known = next((k for k, _why in KNOWN_NONGLOBAL_SHAPES if k.match(chain)), None)
        if known is not None:                           # (d) documented non-global
            n_known += 1
            continue
        structural.append((src, line_of(text, m.start()),
            "UNKNOWN enable shape '%s.enabled = true' -- not a g_ global, not a "
            "resolved sleeve param, not a roster loop member, not in "
            "KNOWN_NONGLOBAL_SHAPES. Teach the parser this shape (do NOT allowlist "
            "blindly)." % chain))

    # -- catch-all: any `.enabled = true` token the chain classifier did NOT see
    #    (e.g. subscripted LHS `book.cells[0].enabled = true`) = unknown shape.
    for m in re.finditer(r'(?:\.|->)\s*enabled\s*=\s*true', text):
        if not any(a <= m.start() < b for a, b in consumed):
            structural.append((src, line_of(text, m.start()),
                "enable assignment NOT consumed by the chain classifier "
                "(LHS shape unparseable, e.g. subscript/complex expression): '%s'. "
                "Teach the parser this shape."
                % text[max(0, m.start() - 60):m.end()].splitlines()[-1].strip()))

# 3. structural failures (zero-parse / unknown shape) -- fail LOUD before verdicts
if structural:
    print("[fail-verdict-guard] *** STRUCTURAL PARSE FAILURE -- guard cannot certify ***")
    for src, ln, msg in structural:
        print(f"  {src}:{ln}: {msg}")
    print("  A shape this parser cannot resolve means a FAIL-verdict engine could be")
    print("  enabled invisibly. Fix the parser/shape in the same commit.")
    sys.exit(2)

# 4. violations
violations = [(g, FAILS[g]) for g in sorted(enabled) if g in FAILS]

print(f"[fail-verdict-guard] {len(FAILS)} FAIL-verdict globals | {len(enabled)} enabled "
      f"(direct {n_direct}, sleeve {n_sleeve}, roster {n_roster}, known-nonglobal {n_known}) | "
      f"{len(violations)} violation(s)")

if violations:
    print("\n  *** FAIL-VERDICT ENGINE ENABLED -- BLOCKED ***")
    for g, f in violations:
        print(f"  {g} ({f['engine']}) -- true-cost FAIL {f['date']} ({f['ref']})")
        print(f"      {f['reason']}")
    print("\n  An engine with a recorded true-cost FAIL may NOT be enabled. To re-enable:")
    print("  remove its FAIL_VERDICTS.tsv row IN THE SAME COMMIT as a NEW passing faithful")
    print("  true-cost backtest ref (both WF halves + both regimes), named in the commit msg.")
else:
    print("  CLEAN -- no FAIL-verdict engine is enabled.")

sys.exit(1 if violations else 0)
