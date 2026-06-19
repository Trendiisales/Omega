#!/usr/bin/env python3
"""
monitor.py — dead-man watchdog over all Omega research loops.

This is the answer to "ensure we do not get stuck in an endless loop which has
shut us down before." The governor already makes a single loop self-limiting
(iteration/wall-clock/dry-streak ceilings + stale-lock auto-release). This
monitor is the SECOND layer: it inspects every loop's state and surfaces, loudly,

  - STUCK    : a lock is held but its heartbeat is older than the budget
               (a tick that hung mid-body) -> reclaim + alert.
  - DISARMED : a loop hit a ceiling and stopped (expected end-of-campaign, or a
               dry streak). Listed so it's never a silent stop.
  - STALE    : no successful tick within the expected cadence -> the schedule
               itself may be broken (launchd/cron not firing).

Exit code: 0 = all healthy/idle, 2 = something needs operator attention. Writes a
one-screen STATUS file so the state is greppable without running anything.

Run: python3 monitor.py            # check + write STATUS, exit 0/2
     python3 monitor.py --reclaim  # also clear any STUCK lock found
"""
import json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loop_governor import STATE_DIR, HALT_FILE  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STATUS = os.path.join(REPO, "outputs", "loops", "LOOP_STATUS.md")

# expected cadence (seconds) — a loop with no tick within this is STALE
CADENCE = {"tier2_improvement": 36 * 3600,    # daily, 1.5x grace
           "tier3_bt_integrity": 9 * 24 * 3600}  # weekly, 1.3x grace
# wall-clock budget per tick (must match the runners) for STUCK detection
BUDGET = {"tier2_improvement": 900, "tier3_bt_integrity": 1800}


def _read(name, suf):
    try:
        with open(os.path.join(STATE_DIR, name + suf)) as fh:
            return json.load(fh)
    except Exception:
        return None


def main():
    reclaim = "--reclaim" in sys.argv
    now = time.time()
    if not os.path.isdir(STATE_DIR):
        os.makedirs(os.path.dirname(STATUS), exist_ok=True)
        open(STATUS, "w").write("# LOOP STATUS\n\nNo loops have run yet.\n")
        print("no loops yet"); return 0

    names = sorted({f.split(".")[0] for f in os.listdir(STATE_DIR)
                    if f.endswith(".state.json")})
    rows, attention = [], False
    halted = os.path.exists(HALT_FILE)

    for name in names:
        st = _read(name, ".state.json") or {}
        lock = _read(name, ".lock")
        beat_path = os.path.join(STATE_DIR, name + ".heartbeat")
        beat_age = now - os.path.getmtime(beat_path) if os.path.exists(beat_path) else None
        status = "ok"

        if lock:
            lock_age = now - float(lock.get("ts", now))
            if lock_age > BUDGET.get(name, 1800):
                status = "STUCK"; attention = True
                if reclaim:
                    try:
                        os.remove(os.path.join(STATE_DIR, name + ".lock"))
                        status = "STUCK->reclaimed"
                    except Exception:
                        pass
        if st.get("disarmed"):
            status = f"DISARMED ({st['disarmed']})"; attention = True
        elif (name in CADENCE and beat_age is not None
              and beat_age > CADENCE[name]):
            status = "STALE (schedule not firing?)"; attention = True

        rows.append((name, st.get("iter"), st.get("dry_streak"),
                     f"{beat_age:.0f}s" if beat_age is not None else "—", status))

    os.makedirs(os.path.dirname(STATUS), exist_ok=True)
    with open(STATUS, "w") as fh:
        fh.write(f"# LOOP STATUS — {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
        fh.write(f"HALT switch: {'SET (all loops paused)' if halted else 'clear'}\n\n")
        fh.write("| loop | iter | dry | last beat | status |\n|---|---|---|---|---|\n")
        for r in rows:
            fh.write(f"| {r[0]} | {r[1]} | {r[2]} | {r[3]} | {r[4]} |\n")
        if attention:
            fh.write("\n**ATTENTION**: a loop is STUCK/DISARMED/STALE. "
                     "Inspect, act, then `loop_governor.py reset <name>` to re-arm.\n")
    print(open(STATUS).read())
    return 2 if attention else 0


if __name__ == "__main__":
    sys.exit(main())
