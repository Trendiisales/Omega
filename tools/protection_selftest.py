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
import os, sys, json, subprocess, tempfile, shutil, time, datetime

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
    # DETERMINISTIC: pre-seed an already-OPEN + ARMED companion (peak +8%), then feed a single state
    # where it has given back to +1% -> one run MUST REVERSAL_CLIP. (Two-cycle live timing was flaky;
    # pre-seeding removes the subprocess-timing race so a real break can't hide behind a flake.)
    sb = tempfile.mkdtemp(prefix="protselftest_")
    try:
        fake_crypto = os.path.join(sb, "fake_crypto.json")
        entry = 100.0
        key = f"CRYPTO|SelfTest|TESTSYM|{round(entry,4)}"
        now = int(time.time()); bar = now // (4*3600)
        # pre-seeded open position: peaked at +8%, still open, armed
        json.dump({key: {"book":"CRYPTO","eng":"SelfTest","sym":"TESTSYM","side":"LONG","entry":entry,
                         "open_ts":now,"open_bar":bar,"mfe_pct":8.0,"ext_bar":bar,"last_upnl":80.0}},
                  open(os.path.join(sb,"companion_positions.json"),"w"))
        # live mark now +1% (<= peak 8 * (1-0.4)=4.8) -> reversal
        json.dump({"slots":[{"key":"k","sym":"TESTSYM","strat":"SelfTest","pos":1,
                             "entry_px":entry,"px":101.0,"unreal_usd":10.0}]}, open(fake_crypto,"w"))
        env = dict(os.environ, COMPANION_DIR=sb, CRYPTO_STATE=fake_crypto, SKIP_OMEGA="1",
                   STALL_GATE_PCT="2.0", REVERSAL_GIVEBACK="0.40")
        r = subprocess.run([sys.executable, COMPANION], env=env, capture_output=True, text=True)
        clipped = ""
        sb_closed = os.path.join(sb, "companion_closed.csv")
        if os.path.exists(sb_closed): clipped = open(sb_closed).read()
        fired = "REVERSAL_CLIP" in clipped or "STALL_CLIP" in clipped
        detail = ("pre-armed +8% peak -> +1% giveback -> companion "
                  + ("CLIPPED (fired correctly)" if fired else "DID NOT FIRE *** broken ***"))
        if not fired and r.stdout: detail += f" | last: {r.stdout.strip().splitlines()[-1][:120]}"
        record("[3] FIRES-ON-TRIGGER", fired, detail)
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

# ---- [5] IN-BINARY CLOSE-PATH WIRED (guards CAN actually close) ---------------
def check_binary_close_path():
    # In-binary guards (AccountingGuard runaway-net, GivebackGuard reversal-stop) can only CLOSE a
    # position if a closer is REGISTERED for it. 0 registered = guards detect but log NO-CLOSER (the
    # catastrophe-net gap). Verify the close path is wired: register_closer/wire_livepos/wire_cross
    # calls exist AND the guards use close_matching. (Config-level: the companion checks 1-4 are the
    # runtime/effect layer; this asserts the binary's close PLUMBING is present.)
    repo = "/Users/jo/Omega"
    def grep_count(path, pat):
        try: return open(path).read().count(pat)
        except Exception: return 0
    pp = repo + "/include/PositionPersistence.hpp"
    closers = grep_count(pp, "register_closer")
    uses_close = grep_count(repo + "/include/GivebackGuard.hpp", "close_matching") > 0 and \
                 grep_count(repo + "/include/AccountingGuard.hpp", "close_matching") > 0
    ok = closers > 0 and uses_close
    detail = f"register_closer call-sites={closers}; guards use close_matching={uses_close}"
    if closers == 0: detail += "  *** NO closers wired -- guards can detect but NOT close ***"
    record("[5] BINARY-CLOSE-PATH", ok, detail)

# ---- [6] COMPANION INPUT FRESHNESS (no stale-feed blind spot) -----------------
def check_input_freshness():
    # The companion can be ALIVE (check 1) yet fed STALE input -> tracks a wrong/old peak. That is
    # EXACTLY how the $222 gold peak was missed (VPS telemetry unreachable). Assert its inputs are
    # fresh: crypto state.json recent AND the companion's last cycles weren't telemetry-skips.
    probs = []
    # Live crypto state moved to ~/Crypto on the 2026-07-01 IBKRCrypto->Crypto consolidation.
    # Heartbeat = max(updated, live_mark_ts) parsed from the JSON, NOT file-mtime: refresh_shadow
    # momentarily drops live_mark_ts while `updated` stays fresh, and mtime alone false-alarms
    # (the naive check GUIStalenessGuard already proved wrong).
    crypto = os.path.join(HOME, "Crypto/backtest/data/ibkrcrypto/state.json")
    if not os.path.exists(crypto):
        crypto = os.path.join(HOME, "IBKRCrypto/backtest/data/ibkrcrypto/state.json")  # pre-cutover fallback
    if os.path.exists(crypto):
        try:
            d = json.load(open(crypto))
            dh = d.get("data_health", {})
            if dh and dh.get("all_fresh") is False:
                probs.append("crypto data_health.all_fresh=False (" + ",".join(dh.get("stale_sources", [])) + ")")
            else:
                def _age_min(v):
                    if not v: return None
                    try:
                        dt = datetime.datetime.strptime(v.replace(" UTC", ""), "%Y-%m-%d %H:%M").replace(tzinfo=datetime.timezone.utc)
                        return (datetime.datetime.now(datetime.timezone.utc) - dt).total_seconds() / 60.0
                    except Exception:
                        return None
                ages = [a for a in (_age_min(d.get("updated")), _age_min(d.get("live_mark_ts"))) if a is not None]
                if ages:
                    age = min(ages)  # freshest heartbeat wins (max timestamp = min age)
                    if age > 30: probs.append(f"crypto state {age:.0f}min stale")
        except Exception as e:
            probs.append(f"crypto state unreadable ({e})")
    else:
        probs.append("crypto state missing")
    log = "/tmp/giveback_saver.log"
    if os.path.exists(log):
        try:
            # Only the MOST RECENT cycle matters. A reboot/VPS blip logs a transient
            # "telemetry unreachable", then healthy cycles resume once the box returns.
            # The old check flagged ANY unreachable line in the last 15 -> a guaranteed
            # false RED after every reboot even after full recovery (2026-07-02). Scan
            # newest->oldest and flag ONLY if the current state is still unreachable/quiet
            # (an unreachable line reached before any healthy cycle line). This still
            # catches a genuinely-CURRENT telemetry outage, just not a recovered blip.
            tail = subprocess.run(["tail","-30",log], capture_output=True, text=True).stdout.splitlines()
            for ln in reversed(tail):
                if "telemetry unreachable" in ln or "telemetry quiet" in ln:
                    probs.append("omega telemetry unreachable in recent companion runs (stale-input -> peaks missed)")
                    break
                if ("omega telemetry empty" in ln) or ("banked-now" in ln) or ("harvest" in ln):
                    break  # most recent cycle is healthy -> recovered blip, not a problem
        except Exception: pass
    ok = (len(probs) == 0)
    detail = "companion inputs fresh (crypto + omega telemetry)" if ok else "*** " + "; ".join(probs) + " ***"
    record("[6] INPUT-FRESHNESS", ok, detail)

def main():
    check_scheduled_alive()
    check_real_not_shadow()
    check_fires_on_trigger()
    check_effect_reconcile()
    check_binary_close_path()
    check_input_freshness()
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
