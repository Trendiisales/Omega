#!/usr/bin/env python3
"""
tier3_bt_integrity.py — ONE governed iteration of the backtest-integrity loop.

The decay catcher. Per tick it runs the cheap, deterministic integrity checks
that keep the BACKTEST_TRUTH regime honest, and flags anything that rotted:

  1. data_integrity_gate.py on every fresh tick file (x1000 glitch / col-swap /
     gap). A REJECTED file is named and must not be used downstream.
  2. abs-pt calibration drift: engines whose pt-thresholds were tuned in an old
     price era ($2400 gold -> $4700 now) are out of window. [[feedback-abs-pt-calibration]]
  3. adverse_protection_audit.sh backfill: NEW entry-engines lacking a verdict
     fail; legacy owe annotation. [[feedback-new-engine-adverse-protection-step]]
  4. live-shadow-PF vs last-known-BT-PF divergence -> engine decayed (hook left
     for when a BT-PF snapshot file is wired; absent => skipped, not failed).

FLAGS ONLY — re-BT/re-tune/deploy stay session+operator gated.

Progress semantics: "progressed" if the set of failing checks changed since the
last tick. Steady-state clean (or steady-state same failures) advances the
dry-streak and disarms — re-arm after acting.

Run: python3 tier3_bt_integrity.py [--ticks <dir-or-glob>]
"""
import glob, hashlib, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loop_governor import Governor  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GATE = os.path.join(REPO, "backtest", "data_integrity_gate.py")
ADV = os.path.join(REPO, "scripts", "adverse_protection_audit.sh")
OUT_DIR = os.path.join(REPO, "outputs", "loops")
LOG = os.path.join(OUT_DIR, "tier3_bt_integrity.log")


def _ticks():
    if "--ticks" in sys.argv:
        return sys.argv[sys.argv.index("--ticks") + 1]
    return os.environ.get("OMEGA_TICKS",
                          os.path.expanduser("~/Tick"))


def _run(cmd, timeout=600):
    try:
        p = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    except subprocess.TimeoutExpired:
        return 124, "TIMEOUT"
    except FileNotFoundError:
        return 127, "MISSING"


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    g = Governor("tier3_bt_integrity", max_iter=104, max_seconds=1800,
                 dry_streak_stop=4)
    if not g.acquire():
        return 0
    try:
        ok, why = g.should_continue()
        if not ok:
            _logline(f"stop: {why}")
            return 0

        failures = []

        # 1. data-integrity gate on a sample of fresh tick files (cap the scan;
        #    silent truncation is logged, never hidden).
        ticks = _ticks()
        files = (sorted(glob.glob(ticks)) if any(c in ticks for c in "*?[")
                 else sorted(glob.glob(os.path.join(ticks, "**", "*.csv"),
                                       recursive=True))
                 if os.path.isdir(ticks) else [])
        CAP = 40
        scanned = files[:CAP]
        if len(files) > CAP:
            _logline(f"NOTE: {len(files)} tick files, scanning first {CAP} "
                     f"(rest deferred to next tick)")
        if os.path.exists(GATE):
            for f in scanned:
                rc, _ = _run([sys.executable, GATE, f], timeout=120)
                if rc != 0:
                    failures.append(f"DATA_REJECT {os.path.basename(f)}")
        else:
            failures.append("data_integrity_gate.py MISSING")

        # 3. adverse-protection audit (new-engine build gate)
        if os.path.exists(ADV):
            rc, out = _run(["bash", ADV])
            if rc != 0:
                bad = [l for l in out.splitlines()
                       if "FAIL" in l or "owe" in l.lower()]
                failures += [f"ADV {l.strip()}" for l in bad[:20]]

        # 2 & 4 are hooks: abs-pt drift + shadow/BT-PF divergence need a wired
        # snapshot of (engine -> tuned price era / last BT PF). When that file
        # exists, compare here. Absent => not a failure, just unchecked.
        snap = os.path.join(REPO, "outputs", "loops", "bt_pf_snapshot.json")
        if not os.path.exists(snap):
            _logline("note: bt_pf_snapshot.json absent — drift/divergence "
                     "checks skipped (wire a snapshot to enable)")

        fp = hashlib.sha1("\n".join(sorted(failures)).encode()).hexdigest()[:12]
        progressed = fp != g.state.get("last_fp")
        g.state["last_fp"] = fp

        stamp = time.strftime("%Y-%m-%d")
        report = os.path.join(OUT_DIR, f"tier3_integrity_{stamp}.md")
        with open(report, "w") as fh:
            fh.write(f"# Tier3 BT-integrity — {stamp}\n\n")
            fh.write(f"ticks: `{ticks}` scanned={len(scanned)} "
                     f"failures={len(failures)} fp=`{fp}`\n\n")
            if failures:
                fh.write("## FAILURES (act before trusting any BT)\n\n")
                for x in failures:
                    fh.write(f"- {x}\n")
            else:
                fh.write("## CLEAN — no integrity failures this tick\n")
        _logline(f"report={os.path.basename(report)} failures={len(failures)} "
                 f"changed={progressed}")
        g.tick(progressed=progressed)
        return 0
    finally:
        g.release()


def _logline(msg):
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(LOG, "a") as fh:
        fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  {msg}\n")
    print(f"[tier3] {msg}")


if __name__ == "__main__":
    sys.exit(main())
