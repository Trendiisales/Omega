#!/usr/bin/env python3
"""
tier2_improvement.py — ONE governed iteration of the continuous-improvement loop.

Implements the standing directive [[omega-continuous-improvement-loop]] as a
cron-tickable, circuit-broken iteration:

    locate live shadow ledger -> ledger_analytics.py -> rank flags
    -> write dated flag report -> append loop log.

It FLAGS ONLY. It never edits an engine, never deploys. Acting on the top flag
stays a session/operator decision (BACKTEST_TRUTH + never-deploy-without-backtest
gate the act, not the analysis).

Progress semantics: a tick "progressed" if the ranked top-flag fingerprint
changed since the last tick. A run of unchanged ticks advances the governor's
dry-streak and auto-disarms the loop (no point re-flagging the same thing daily).

Run: python3 tier2_improvement.py [--ledger <path-or-glob>]
Default ledger: $OMEGA_LEDGER or ./logs/trades/ (dir -> all daily close files).
"""
import hashlib, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from loop_governor import Governor  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ANALYTICS = os.path.join(REPO, "tools", "analytics", "ledger_analytics.py")
OUT_DIR = os.path.join(REPO, "outputs", "loops")
LOG = os.path.join(OUT_DIR, "tier2_improvement.log")


def _ledger():
    if "--ledger" in sys.argv:
        return sys.argv[sys.argv.index("--ledger") + 1]
    return os.environ.get("OMEGA_LEDGER",
                          os.path.join(REPO, "logs", "trades"))


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    g = Governor("tier2_improvement", max_iter=365, max_seconds=900,
                 dry_streak_stop=5)
    if not g.acquire():
        return 0
    try:
        ok, why = g.should_continue()
        if not ok:
            _logline(f"stop: {why}")
            return 0

        ledger = _ledger()
        if not (os.path.exists(ledger) or any(c in ledger for c in "*?[")):
            _logline(f"no ledger at {ledger} — no-progress tick")
            g.tick(progressed=False)
            return 0

        try:
            out = subprocess.run(
                [sys.executable, ANALYTICS, ledger, "--min-n", "5"],
                capture_output=True, text=True, timeout=600).stdout
        except subprocess.TimeoutExpired:
            _logline("ledger_analytics TIMEOUT — no-progress tick")
            g.tick(progressed=False)
            return 0

        # the analytics tail is the ranked flag list; fingerprint it
        flags = "\n".join(l for l in out.splitlines()
                          if "FLAG" in l or "WORST" in l or "⚠" in l)
        fp = hashlib.sha1(flags.encode()).hexdigest()[:12]
        last_fp = g.state.get("last_fp")
        progressed = fp != last_fp
        g.state["last_fp"] = fp

        stamp = time.strftime("%Y-%m-%d")
        report = os.path.join(OUT_DIR, f"tier2_flags_{stamp}.md")
        with open(report, "w") as fh:
            fh.write(f"# Tier2 improvement flags — {stamp}\n\n")
            fh.write(f"ledger: `{ledger}`  fingerprint: `{fp}` "
                     f"(changed={progressed})\n\n## Ranked flags\n\n```\n")
            fh.write(flags or "(no flags emitted)\n")
            fh.write("\n```\n\n## ACT (session-gated, NOT auto)\n"
                     "Top flag -> backtest the fix faithfully (both-halves, "
                     "both-regimes, real cost) before any wire. File verdict to "
                     "Memory-Omega.\n")
        _logline(f"report={os.path.basename(report)} fp={fp} "
                 f"changed={progressed}")
        g.tick(progressed=progressed)
        return 0
    finally:
        g.release()


def _logline(msg):
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(LOG, "a") as fh:
        fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')}  {msg}\n")
    print(f"[tier2] {msg}")


if __name__ == "__main__":
    sys.exit(main())
