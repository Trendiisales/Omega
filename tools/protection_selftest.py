#!/usr/bin/env python3
"""protection_selftest.py — EFFECT-LEVEL protection self-test (S-2026-06-29, operator-mandated).

WHY THIS EXISTS: every prior "check" verified that protection code EXISTS or is CONFIGURED. None
verified it RUNS and ACTUALLY DOES ITS JOB. So a protection that was built-but-never-scheduled
(the companion stall_accountant.py sat dead for weeks), enabled-but-shadow (giveback_saver only
logged, never closed), or silently-missing (gold gave back $142 uncaught) passed every static check
while doing nothing. This test asserts FUNCTION, not existence. It is the single source of truth for
"is our profit/loss protection actually working" and runs at every session start.

FOUR CHECKS (each pass/fail independently; overall RED if any fail):
  [1] SCHEDULED + ALIVE   — the companion engine is in cron AND wrote its state in the last N min.
  [2] REAL, NOT SHADOW    — the companion has a real EFFECT path (banks a clip), not log-only theatre.
  [3] FIRES ON TRIGGER    — inject a synthetic peak->giveback into a sandbox companion; assert it clips.
  [4] EFFECT RECONCILE    — no live armed position has given back past the trail while still UNCLIPPED.

Exit 0 = all green. Exit 1 = one or more RED. Writes a status file the SessionStart hook surfaces.
"""
import os, sys, json, subprocess, tempfile, shutil, time

HOME = os.path.expanduser("~")
COMPANION = os.path.join(HOME, "stall-accountant", "stall_accountant.py")
COMP_DIR  = os.path.join(HOME, "stall-accountant")
STATE     = os.path.join(COMP_DIR, "companion_state.json")
CLOSED    = os.path.join(COMP_DIR, "companion_closed.csv")
STATUS    = os.path.join(HOME, ".claude", "protection_selftest_STATUS.txt")
ALIVE_MIN = 12          # companion runs every 1min; >12min stale = dead
GATE_PCT  = float(os.environ.get("STALL_GATE_PCT", "2.0"))
REV_GB    = float(os.environ.get("REVERSAL_GIVEBACK", "0.40"))

results = []  # (check_name, ok, detail)
def record(name, ok, detail): results.append((name, ok, detail))

# ---- [1] SCHEDULED + ALIVE ----------------------------------------------------
def check_scheduled_alive():
    try: cron = subprocess.run(["crontab","-l"], capture_output=True, text=True).stdout
    except Exception as e: cron = ""
    scheduled = "stall_accountant.py" in cron
    age_min = None
    if os.path.exists(STATE):
        age_min = (time.time() - os.path.getmtime(STATE)) / 60.0
    alive = (age_min is not None and age_min <= ALIVE_MIN)
    ok = scheduled and alive
    detail = (f"companion in cron={scheduled}; state age="
              + (f"{age_min:.1f}min (<= {ALIVE_MIN})" if age_min is not None else "MISSING"))
    if not scheduled: detail += "  *** NOT SCHEDULED -- protection is DEAD ***"
    elif not alive:   detail += "  *** STALE -- companion not running ***"
    record("[1] SCHEDULED+ALIVE", ok, detail)

# ---- [2] REAL, NOT SHADOW -----------------------------------------------------
def check_real_not_shadow():
    # the scheduled protection must have a real EFFECT path (write a banked clip), not just log/track.
    try: src = open(COMPANION).read()
    except Exception as e: record("[2] REAL-NOT-SHADOW", False, f"cannot read companion: {e}"); return
    banks = ("companion_closed" in src) and ("_close(" in src or "CLOSED" in src)
    # shadow tell-tale: a protection that ONLY tracks peak/locked with no close/bank action
    only_logs = ("locked" in src and "_close(" not in src and "companion_closed" not in src)
    ok = banks and not only_logs
    detail = f"companion has real bank/close path={banks}; log-only-shadow={only_logs}"
    if not ok: detail += "  *** SHADOW THEATRE -- records but never closes ***"
    record("[2] REAL-NOT-SHADOW", ok, detail)

# ---- [3] FIRES ON SYNTHETIC TRIGGER ------------------------------------------
def check_fires_on_trigger():
    sb = tempfile.mkdtemp(prefix="protselftest_")
    try:
        fake_crypto = os.path.join(sb, "fake_crypto.json")
        def write_state(px):
            json.dump({"slots":[{"key":"TEST|SelfTest|TESTSYM","sym":"TESTSYM","strat":"SelfTest",
                                 "pos":1,"entry_px":100.0,"px":px,"unreal_usd":(px-100.0)*10}]},
                      open(fake_crypto,"w"))
        env = dict(os.environ, COMPANION_DIR=sb, CRYPTO_STATE=fake_crypto, SKIP_OMEGA="1",
                   STALL_GATE_PCT="2.0", REVERSAL_GIVEBACK="0.40")
        # cycle 1: position +8% -> companion opens + arms (peak)
        write_state(108.0); subprocess.run([sys.executable, COMPANION], env=env, capture_output=True, text=True)
        # cycle 2: gives back to +1% (<= peak 8 * (1-0.4)=4.8) -> must REVERSAL_CLIP
        write_state(101.0); r2 = subprocess.run([sys.executable, COMPANION], env=env, capture_output=True, text=True)
        clipped = ""
        sb_closed = os.path.join(sb, "companion_closed.csv")
        if os.path.exists(sb_closed): clipped = open(sb_closed).read()
        fired = "REVERSAL_CLIP" in clipped or "STALL_CLIP" in clipped
        ok = fired
        detail = ("synthetic +8%->+1% giveback -> companion "
                  + ("CLIPPED (fired correctly)" if fired else "DID NOT FIRE *** broken ***"))
        if not fired and r2.stdout: detail += f" | last: {r2.stdout.strip().splitlines()[-1][:120]}"
        record("[3] FIRES-ON-TRIGGER", ok, detail)
    finally:
        shutil.rmtree(sb, ignore_errors=True)

# ---- [4] EFFECT RECONCILIATION (no unclipped giveback) -----------------------
def check_effect_reconcile():
    if not os.path.exists(STATE):
        record("[4] EFFECT-RECONCILE", False, "no companion_state.json -- cannot reconcile"); return
    try: d = json.load(open(STATE))
    except Exception as e: record("[4] EFFECT-RECONCILE", False, f"state unreadable: {e}"); return
    misses = []
    for p in d.get("open_detail", []):
        mfe = float(p.get("mfe_pct", 0) or 0)
        upnl = float(p.get("upnl", 0) or 0)
        # armed (reached gate) AND given back past the trail but STILL OPEN = a miss
        if mfe >= GATE_PCT and "fav" in p:
            fav = float(p.get("fav", mfe))
            if fav <= mfe * (1.0 - REV_GB):
                misses.append(f"{p.get('eng')}/{p.get('sym')} peak={mfe:.1f}% now={fav:.1f}% UNCLIPPED")
    ok = (len(misses) == 0)
    detail = ("no armed position has given back past trail while open"
              if ok else f"*** {len(misses)} UNCLIPPED GIVEBACK(S): " + "; ".join(misses[:4]) + " ***")
    record("[4] EFFECT-RECONCILE", ok, detail)

def main():
    check_scheduled_alive()
    check_real_not_shadow()
    check_fires_on_trigger()
    check_effect_reconcile()
    overall = all(ok for _,ok,_ in results)
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    lines = [f"PROTECTION SELF-TEST {'GREEN -- all protection FUNCTIONAL' if overall else 'RED -- PROTECTION NOT WORKING'}  ({ts})"]
    for name, ok, detail in results:
        lines.append(f"  {'PASS' if ok else 'FAIL'} {name}: {detail}")
    out = "\n".join(lines)
    print(out)
    try:
        os.makedirs(os.path.dirname(STATUS), exist_ok=True)
        open(STATUS, "w").write(out + "\n")
    except Exception: pass
    sys.exit(0 if overall else 1)

if __name__ == "__main__":
    main()
