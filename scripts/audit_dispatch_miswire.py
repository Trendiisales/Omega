#!/usr/bin/env python3
# Audit on_tick_* dispatch functions for engines invoked with symbol-mismatched
# suffix. Pattern: g_X_<suf>.on_tick(...) inside on_tick_<other_sym>.
# Real bugs class: like the g_iswing_nq leak fixed in commit ba088df8.
import re, sys

SUF_TO_FN = {
    'sp':    'us500',
    'nq':    'ustec',
    'nas':   'nas100',
    'nas100':'nas100',
    'us30':  'dj30',
    'us500': 'us500',
    'us':    'us500',  # ambiguous; we'll let it pass
    'ger':   'ger40',
    'ger40': 'ger40',
    'uk':    'uk100',
    'uk100': 'uk100',
    'estx':  'estx50',
    'estx50':'estx50',
    'dj':    'dj30',
    'dj30':  'dj30',
    'brent': 'brent',
    'gold':  'gold',
    'eur':   'eurusd',
    'eurusd':'eurusd',
    'gbp':   'gbpusd',
    'gbpusd':'gbpusd',
    'jpy':   'usdjpy',
    'usdjpy':'usdjpy',
    'aud':   'audusd',
    'audusd':'audusd',
    'nzd':   'audusd',  # shared handler
    'nzdusd':'audusd',
    'cad':   'usdcad',
    'usdcad':'usdcad',
    'oil':   'oil',
    'usoil': 'oil',
}

# Allowlist: cross-asset signal reads are intentional (engine for sym A
# reading sym B's bars for trend context). Only flag .on_tick(bid, ask, ...)
# calls -- those execute trading logic.
EXEC_CALL = re.compile(r'\bg_([a-z][a-z0-9_]*)_([a-z]+[0-9]*)\.on_tick\(')

FN_DEF = re.compile(r'static\s+void\s+on_tick_([a-z]+[0-9]*)\(')

def main(path):
    cur_fn = None
    issues = []
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            m = FN_DEF.search(line)
            if m:
                cur_fn = m.group(1)
                continue
            if cur_fn is None: continue
            for m in EXEC_CALL.finditer(line):
                engine_class, suf = m.group(1), m.group(2)
                # Try mapping suf -> expected on_tick_ name
                expected = SUF_TO_FN.get(suf)
                if expected and expected != cur_fn:
                    issues.append((lineno, m.group(0), cur_fn, expected))
    if not issues:
        print(f"[OK] no execution-call mismatches in {path}")
        return 0
    print(f"[MISMATCH FOUND] in {path}:")
    for ln, snippet, fn, exp in issues:
        print(f"  L{ln} {snippet} inside on_tick_{fn} (expected on_tick_{exp})")
    return 1

if __name__ == '__main__':
    paths = sys.argv[1:] or ['include/tick_indices.hpp', 'include/tick_fx.hpp', 'include/tick_gold.hpp']
    rc = 0
    for p in paths:
        try:
            rc |= main(p)
        except FileNotFoundError:
            pass
    sys.exit(rc)
