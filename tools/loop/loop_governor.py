#!/usr/bin/env python3
"""
loop_governor.py — circuit-breaker for Omega research loops.

WHY THIS EXISTS
  A runaway / stuck loop has taken the box down before (deploy-mutex hung at
  [9/12] and never released; bash `until/while+sleep` poll loops are
  hook-blocked). The cure is structural, not vigilance:

    1. The LOOP IS THE SCHEDULE, not a spinning process. Each cron tick runs
       exactly ONE bounded iteration and exits. Nothing spins; the box cannot
       hang on a loop body.
    2. Every limit is enforced here, in one place, so a loop body physically
       cannot run forever even if its own logic is wrong.

  This module is pure stdlib (runs on Mac cron-agent and on the VPS under
  python3). No deps.

GUARANTEES (the things that shut us down, each closed)
  - LOCK with STALE AUTO-RELEASE: two ticks never stack; but a dead/expired
    holder's lock is reclaimed (the deploy-mutex bug was a lock that NEVER
    released -> next run blocked forever). We never block forever.
  - KILL SWITCH: `touch ~/.omega_loops/HALT` aborts every loop at the next tick.
  - DRY-STREAK STOP: a discovery/improvement loop that makes no progress for K
    consecutive ticks disarms itself (loop-until-dry, never loop-forever).
  - HARD ITERATION + WALL-CLOCK CEILINGS per run.
  - DEAD-MAN HEARTBEAT: each tick stamps a heartbeat file; an external watchdog
    (or the next tick) flags a holder that exceeded its wall-clock as hung and
    reclaims it, instead of waiting on it.

USAGE (from a loop runner)
    from loop_governor import Governor
    g = Governor("tier2_improvement", max_iter=200, max_seconds=1200,
                 dry_streak_stop=3)
    if not g.acquire():            # kill-switch set, or another tick live
        sys.exit(0)
    try:
        ok, reason = g.should_continue()
        if not ok:
            print(f"[governor] stop: {reason}"); sys.exit(0)
        progressed = run_one_iteration()   # the loop body — ONE iteration
        g.tick(progressed=progressed)      # records iter + dry-streak + heartbeat
    finally:
        g.release()

CLI (status / reset / halt / resume — for the operator)
    python3 loop_governor.py status [name]
    python3 loop_governor.py halt          # set global kill switch
    python3 loop_governor.py resume        # clear it
    python3 loop_governor.py reset <name>  # clear a loop's state + stale lock
"""
import json, os, sys, time, signal

STATE_DIR = os.environ.get("OMEGA_LOOP_DIR",
                           os.path.expanduser("~/.omega_loops"))
HALT_FILE = os.path.join(STATE_DIR, "HALT")


def _now():
    return time.time()


def _pid_alive(pid):
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)          # signal 0 = existence check, no-op
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True              # exists, owned by someone else
    except Exception:
        return False


class Governor:
    def __init__(self, name, max_iter=500, max_seconds=1800,
                 dry_streak_stop=3):
        self.name = name
        self.max_iter = max_iter
        self.max_seconds = max_seconds
        self.dry_streak_stop = dry_streak_stop
        os.makedirs(STATE_DIR, exist_ok=True)
        self.state_path = os.path.join(STATE_DIR, f"{name}.state.json")
        self.lock_path = os.path.join(STATE_DIR, f"{name}.lock")
        self.beat_path = os.path.join(STATE_DIR, f"{name}.heartbeat")
        self.state = self._load_state()
        self._held = False

    # ---- persistent state across cron ticks --------------------------------
    def _load_state(self):
        try:
            with open(self.state_path) as fh:
                return json.load(fh)
        except Exception:
            return {"iter": 0, "dry_streak": 0, "started": _now(),
                    "last_progress": None, "disarmed": None}

    def _save_state(self):
        tmp = self.state_path + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(self.state, fh, indent=2)
        os.replace(tmp, self.state_path)  # atomic

    # ---- lock with STALE AUTO-RELEASE --------------------------------------
    def acquire(self):
        """Return True if this tick may run. False if halted or a *live* tick
        already holds the lock. A stale lock (dead PID, or older than 2x the
        wall-clock budget = presumed hung) is reclaimed — we never block
        forever, which is exactly the deploy-mutex failure mode."""
        if os.path.exists(HALT_FILE):
            print(f"[governor:{self.name}] HALT file present — refusing to run")
            return False
        if os.path.exists(self.lock_path):
            try:
                with open(self.lock_path) as fh:
                    held = json.load(fh)
            except Exception:
                held = {}
            pid = int(held.get("pid", -1))
            age = _now() - float(held.get("ts", 0))
            hung = age > (2 * self.max_seconds)
            if _pid_alive(pid) and not hung:
                print(f"[governor:{self.name}] live tick pid={pid} "
                      f"age={age:.0f}s — skipping this tick")
                return False
            # reclaim: dead holder OR exceeded 2x budget => presumed hung
            print(f"[governor:{self.name}] reclaiming stale lock "
                  f"(pid={pid} alive={_pid_alive(pid)} age={age:.0f}s "
                  f"hung={hung})")
        with open(self.lock_path, "w") as fh:
            json.dump({"pid": os.getpid(), "ts": _now()}, fh)
        self._held = True
        self._beat()
        return True

    def release(self):
        if self._held and os.path.exists(self.lock_path):
            try:
                os.remove(self.lock_path)
            except Exception:
                pass
        self._held = False

    def _beat(self):
        with open(self.beat_path, "w") as fh:
            json.dump({"pid": os.getpid(), "ts": _now(),
                       "iter": self.state["iter"]}, fh)

    # ---- the ceilings ------------------------------------------------------
    def should_continue(self):
        if os.path.exists(HALT_FILE):
            return False, "HALT file present"
        if self.state.get("disarmed"):
            return False, f"disarmed: {self.state['disarmed']}"
        if self.state["iter"] >= self.max_iter:
            self._disarm("max_iter reached")
            return False, "max_iter reached"
        if _now() - self.state["started"] > self.max_seconds * self.max_iter:
            # absolute campaign cap: budget per tick x max ticks
            self._disarm("campaign wall-clock exceeded")
            return False, "campaign wall-clock exceeded"
        if (self.dry_streak_stop and
                self.state["dry_streak"] >= self.dry_streak_stop):
            self._disarm(f"dry streak {self.state['dry_streak']}")
            return False, f"dry streak {self.state['dry_streak']} — loop is dry"
        return True, "ok"

    def _disarm(self, reason):
        self.state["disarmed"] = reason
        self._save_state()

    def tick(self, progressed):
        """Record one completed iteration. progressed=True resets the dry
        streak; False increments it (loop-until-dry)."""
        self.state["iter"] += 1
        if progressed:
            self.state["dry_streak"] = 0
            self.state["last_progress"] = _now()
        else:
            self.state["dry_streak"] += 1
        self._save_state()
        self._beat()

    def rearm(self):
        """Operator/explicit re-arm: clear disarm + counters for a fresh run."""
        self.state = {"iter": 0, "dry_streak": 0, "started": _now(),
                      "last_progress": None, "disarmed": None}
        self._save_state()


# ---- CLI -------------------------------------------------------------------
def _cli():
    if len(sys.argv) < 2:
        print(__doc__); return
    cmd = sys.argv[1]
    if cmd == "halt":
        os.makedirs(STATE_DIR, exist_ok=True)
        open(HALT_FILE, "w").close()
        print(f"HALT set: {HALT_FILE} — all loops will refuse to run")
    elif cmd == "resume":
        if os.path.exists(HALT_FILE):
            os.remove(HALT_FILE); print("HALT cleared — loops may run")
        else:
            print("no HALT file")
    elif cmd == "reset":
        if len(sys.argv) < 3:
            print("usage: reset <name>"); return
        name = sys.argv[2]
        for suf in (".state.json", ".lock", ".heartbeat"):
            p = os.path.join(STATE_DIR, name + suf)
            if os.path.exists(p):
                os.remove(p); print(f"removed {p}")
        print(f"{name} reset")
    elif cmd == "status":
        names = ([sys.argv[2]] if len(sys.argv) > 2 else
                 sorted({f.split(".")[0] for f in os.listdir(STATE_DIR)
                         if f.endswith(".state.json")}
                        if os.path.isdir(STATE_DIR) else []))
        print(f"HALT: {'SET' if os.path.exists(HALT_FILE) else 'clear'}")
        for name in names:
            try:
                with open(os.path.join(STATE_DIR,
                                       f"{name}.state.json")) as fh:
                    st = json.load(fh)
            except Exception:
                st = {}
            beat = os.path.join(STATE_DIR, f"{name}.heartbeat")
            age = (f"{_now()-os.path.getmtime(beat):.0f}s ago"
                   if os.path.exists(beat) else "—")
            print(f"  {name}: iter={st.get('iter')} "
                  f"dry={st.get('dry_streak')} "
                  f"disarmed={st.get('disarmed')} heartbeat={age}")
    else:
        print(__doc__)


if __name__ == "__main__":
    _cli()
