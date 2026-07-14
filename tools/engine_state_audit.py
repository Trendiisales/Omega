#!/usr/bin/env python3
# ENGINE STATE AUDIT (2026-06-23) -- check EVERY engine/gate for the restart-blindness class:
#   (A) STALE-SEED      : warm-seeds from a CSV; flagged if that CSV is stale (boots blind)
#   (B) NO-PERSISTENCE  : holds EMA/ADX/regime/warm()-gated state but no save_state/load_state
#                         -> resets on restart; only OK if it re-seeds FRESH each boot
#   (C) COLD-GATE       : has a `warm() &&` entry guard but NO warm-seed path -> warm() may never
#                         be true -> the gate is permanently OFF (the TrendRider BullGate bug)
# Prints a per-engine matrix + the gaps to fix. Heuristic (grep-based) -- a flag = inspect.
import os, re, glob, time, importlib.util

REPO = "."
SEED_DIR = os.path.join(REPO, "phase1", "signal_discovery")

# per-TF staleness thresholds (days) -- IMPORTED from tools/seed_freshness_audit.py, the
# SINGLE copy (its OVERRIDES table + MAX_AGE_DAYS default). History (latent-class sweep
# S-2026-07-14 item 12): this file carried a hand-synced thr() duplicate that DRIFTED --
# it was missing the warmup_XAUUSD_M1 45d override, so a normal-age (17-40d) histdata M1
# seed read as false STALE here while the real audit passed it. Derive-don't-copy.
# NO try/except fallback BY DESIGN: if the sibling module or threshold_for goes missing,
# this audit must die loudly -- silently reverting to stale hardcoded thresholds is the
# exact failure class this import removes.
_sfa_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seed_freshness_audit.py")
_spec = importlib.util.spec_from_file_location("seed_freshness_audit", _sfa_path)
_sfa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_sfa)   # import-safe: its audit body is under main()/__main__ guard
thr = _sfa.threshold_for         # AttributeError (loud) if the shared def is ever renamed

def seed_age_days(csv):
    cand = csv if os.path.exists(csv) else os.path.join(REPO, csv)
    if not os.path.exists(cand): return None
    try:
        with open(cand, "rb") as f:
            f.seek(0, 2); sz = f.tell(); f.seek(max(0, sz-2048)); tail = f.read().decode("latin-1")
        for line in reversed(tail.splitlines()):
            a = line.split(",")
            try: ts = float(a[0])
            except: continue
            if ts <= 0: continue
            if ts > 1e11: ts /= 1000.0
            return (time.time() - ts) / 86400.0
    except: return None
    return None

STATE_RX = re.compile(r"\bema[SF]?_|\bema200_|\badx_|\bregime_|\bslope_|\bemaS_hist_|warm\(\)", re.I)
SEED_RX  = re.compile(r'(?:seed_from[a-z0-9_]*|warmup_or_die|seed_bull_gate|load_or_seed)[^\n]*?"([^"]+\.csv)"')
SEEDCALL_RX = re.compile(r"seed_from[a-z0-9_]*\(|warmup_or_die|seed_bull_gate|load_or_seed|seed_h1|seed_h4|seed_d1")
PERSIST_RX = re.compile(r"\bsave_state\b|\bload_state\b|save_atr_state|save_indicators|load_or_seed")
COLDGATE_RX = re.compile(r"warm\(\)\s*&&|if\s*\(\s*[a-z_]*\.?warm\(\)")

# seed paths referenced anywhere in engine_init (maps engines seeded externally)
init_txt = ""
for p in ["include/engine_init.hpp", "include/globals.hpp", "include/tick_gold.hpp", "include/tick_indices.hpp"]:
    fp = os.path.join(REPO, p)
    if os.path.exists(fp): init_txt += open(fp, errors="ignore").read()

rows = []
for h in sorted(glob.glob(os.path.join(REPO, "include", "*Engine.hpp")) +
                glob.glob(os.path.join(REPO, "include", "*State.hpp")) +
                [os.path.join(REPO, "include", "BullGate.hpp"), os.path.join(REPO, "include", "GoldEngineStack.hpp")]):
    cls = os.path.basename(h)[:-4]
    txt = open(h, errors="ignore").read()
    has_state   = bool(STATE_RX.search(txt))
    has_seed    = bool(SEEDCALL_RX.search(txt)) or (cls in init_txt and "seed" in init_txt.lower())
    has_persist = bool(PERSIST_RX.search(txt))
    cold_gate   = bool(COLDGATE_RX.search(txt))
    # seed file (from this header or engine_init)
    seeds = re.findall(r'"([^"]+warmup[^"]*\.csv|[^"]*tsmom[^"]*\.csv|[^"]*hist\.csv)"', txt) or re.findall(r'"([^"]+warmup[^"]*\.csv)"', init_txt if cls in init_txt else "")
    seed_stale = None
    for s in seeds:
        a = seed_age_days(s)
        if a is not None and a > thr(s): seed_stale = f"{os.path.basename(s)} {a:.0f}d"
        break
    # flags
    flags = []
    if has_state and not has_persist: flags.append("NO-PERSIST")
    if cold_gate and not has_seed:     flags.append("COLD-GATE")
    if seed_stale:                     flags.append(f"STALE-SEED({seed_stale})")
    if flags or has_state:
        rows.append((cls, "Y" if has_state else "-", "Y" if has_seed else "-",
                     "Y" if has_persist else "-", "Y" if cold_gate else "-", ",".join(flags)))

print("ENGINE STATE AUDIT  (state=holds EMA/regime/warm  seed=has warm-seed  persist=save/load  cold=warm()-gate)\n")
print(f"{'engine/gate':<34}{'state':>6}{'seed':>5}{'persist':>8}{'cold':>5}  flags")
gaps = []
for r in sorted(rows, key=lambda x: (0 if x[5] else 1, x[0])):
    print(f"{r[0]:<34}{r[1]:>6}{r[2]:>5}{r[3]:>8}{r[4]:>5}  {r[5]}")
    if r[5]: gaps.append(r)
print(f"\n{len(rows)} state-holding engines/gates; {len(gaps)} with flags to inspect.")
