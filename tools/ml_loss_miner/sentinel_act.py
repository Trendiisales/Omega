#!/usr/bin/env python3
"""
sentinel_act.py — REAL-TIME ANOMALY CONTROL LOOP (the ACT layer)
================================================================================
WHY THIS EXISTS (operator, 2026-07-24): every monitor in this repo is ALERT-ONLY.
They detect fill-drought / phantom / reject-storm / mis-size and then... print a
banner. "What use is the ML if nothing is done." This module is the missing L4 ACT
layer: it INGESTS the un-fakeable outcome signals, CLASSIFIES + SCORES the anomaly,
DECIDES an action, and ACTS — by emitting a halt CONTROL-FLAG the trading binary
honors — while writing a DECISION LOG that is the PROOF the failure was caught and
acted on in real time.

DESIGN PRINCIPLES (non-negotiable):
  * FAIL-SAFE: every action can only HALT / BLOCK / CANCEL. It NEVER initiates an
    order (the gap-5 lesson — untested order-placement is the failure class we are
    fixing, not adding to).
  * RULES ARE THE SPINE: deterministic, auditable thresholds decide the ACT. The ML
    (trade_sentinel iForest / robust-z) augments severity scoring, never the sole
    authority on a live halt.
  * DEFAULT DRY-RUN: decides + logs + alerts but writes NO halt flag until --arm.
    The operator sees the decisions (the proof) before the auto-halt is armed.
  * BOTH SYSTEMS: Omega (IBKR) + Chimera (Binance). One control channel each.
  * SINGLE SOURCE OF TRUTH: reuses trade_sentinel.check_execution_outcome so the
    detection logic is not forked.

CONTROL CHANNEL (how the ACT reaches the binary):
  A halt flag is written to  outputs/control/halt_<system>[_<scope>].flag  and (when
  --push) scp'd to the box control dir. The binary polls that dir and on a fresh
  halt flag trips its circuit / disables the named engine (fail-safe; see
  BINARY_POLL_SPEC below). Until that poll ships, --arm still writes the flag +
  escalates loudly, so the halt is one deploy away, and the decision log proves the
  loop works today.

USAGE:
  python3 tools/ml_loss_miner/sentinel_act.py                 # DRY-RUN: decide+log+alert
  python3 tools/ml_loss_miner/sentinel_act.py --arm           # ARMED: also emit halt flags
  python3 tools/ml_loss_miner/sentinel_act.py --arm --push    # ARMED + scp flag to box
  python3 tools/ml_loss_miner/sentinel_act.py --loop 60       # real-time: re-run every 60s
  python3 tools/ml_loss_miner/sentinel_act.py --self-test     # offline: synthetic anomalies

BINARY_POLL_SPEC (deploy-gated, both systems — the actuator side, ~20 lines each):
  Omega  include/IbkrExecutionEngine.hpp: a watchdog-thread poll of the control dir;
         a fresh halt_omega*.flag -> trip_circuit_() (already halts all place_order).
  Crypto include/live/ExecutionGateway.hpp: poll -> drive EngineRegistry to HALTED /
         set the DrawdownGovernor halt latch. Exits never blocked.
================================================================================
"""
from __future__ import annotations
import argparse, json, subprocess, sys, time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CONTROL_DIR = REPO / "outputs" / "control"
DECISION_LOG = REPO / "outputs" / "sentinel_decisions.jsonl"
ALERT_LOG = REPO / "outputs" / "sentinel_act_alerts.log"

# ── ACTION TIERS ─────────────────────────────────────────────────────────────
# severity -> action. Every action is HALT/BLOCK/CANCEL/ALERT — never order-initiating.
TIER_HALT   = "HALT"     # trip the exec circuit: block ALL new entries (sticky)
TIER_DISABLE= "DISABLE"  # disable ONE engine (scoped)
TIER_CANCEL = "CANCEL"   # request global-cancel of resting orders
TIER_ALERT  = "ALERT"    # escalate to operator, no auto-halt

# ── ANOMALY RULEBOOK (deterministic spine) ───────────────────────────────────
# name: (detector_key, severity 0-100, action_tier, scope, human)
# Detector keys map to signals from ingest() below.
RULEBOOK = {
    "NO_FILL_DESPITE_INTENTS": (
        90, TIER_HALT, "book",
        "Engines fired order intents but ZERO broker fills — orders bouncing/blocked. "
        "Halt new entries; the exec/route is broken (LOT_SIZE / socket / reject storm)."),
    "REJECT_STORM": (
        95, TIER_HALT, "book",
        "Broker reject rate spiked — orders rejected en masse. Halt before the retry "
        "loop becomes an order storm."),
    "LOT_SIZE_REJECTS": (
        88, TIER_HALT, "book",
        "Binance -1013 LOT_SIZE filter rejects CLUSTERING — orders bouncing off the "
        "lot-size/precision filter (the recurring class). The existing outcome thresholds "
        "MISS this when intent-count is low; caught here on the raw -1013 count. Halt + fix "
        "the qty floor/precision mapping."),
    "PHANTOM_POSITION": (
        85, TIER_DISABLE, "engine",
        "Engine reports an open position the broker does NOT hold (phantom). Disable that "
        "engine so its exit does not 'fire on nothing' and its intent stops rendering live."),
    "INGEST_ERROR": (
        82, TIER_ALERT, "monitor",
        "The control loop's INGEST FAILED this run — it is BLIND (dependency/ssh/parse "
        "error). A blind monitor silently reads as 'book healthy' (the exact false-green "
        "class); surfaced LOUD + RED instead so the wrapper notifies. Fix the loop, not the book."),
    "MIS_SIZE_CLAMPED": (
        70, TIER_ALERT, "engine",
        "An order was size-cap CLAMPED (engine computed a wildly wrong qty — the 25K class). "
        "The cap already contained it; escalate so the sizing bug is fixed."),
    "STALE_FEED_WITH_INTENT": (
        80, TIER_HALT, "book",
        "Order intents while the feed is stale/frozen — trading off a bad price. Halt new "
        "entries until the feed recovers."),
    "UNPROTECTED_POSITION": (
        75, TIER_ALERT, "engine",
        "A live broker position has no resting protective stop — unprotected if the process "
        "dies. Escalate (native-stop wiring is gap 5)."),
}


def _utcnow() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ── INGEST: pull the un-fakeable outcome signals (single source of truth) ──────
def ingest(self_test: bool = False) -> list[dict]:
    """Returns a list of raw anomaly dicts: {system, name, evidence}. Reuses
    trade_sentinel.check_execution_outcome so detection is not forked."""
    if self_test:
        return [
            {"system": "omega",  "name": "NO_FILL_DESPITE_INTENTS",
             "evidence": {"intents": 232, "fills": 0, "syms": ["BRENT", "USOIL.F", "XAUUSD"]}},
            {"system": "omega",  "name": "PHANTOM_POSITION",
             "evidence": {"phantoms": ["AAPL(StockDip)", "INTC(StockDip)"]}},
            {"system": "chimera", "name": "REJECT_STORM",
             "evidence": {"intents": 40, "rejects": 34, "rate": 0.85, "code": -1013}},
        ]
    anomalies: list[dict] = []
    # ── CRYPTO: reuse trade_sentinel's exec-outcome (single source of truth). Real
    #    API is check_execution_outcome(new_lines) -> (alerts, counts); alert dicts
    #    are {level, code, counts, msg} with code in {NO_FILL_DESPITE_INTENTS,
    #    REJECT_RATE_SPIKE}. pull_exec_events() ssh-greps the crypto executor log.
    try:
        import trade_sentinel as ts
        lines = _crypto_exec_lines_since_boot()   # AGE-GATE: post-restart lines only
        if lines is None:                          # pull failed -> fail-loud, not false-healthy
            raise RuntimeError("crypto exec-log pull returned no data (ssh/log unreadable)")
        alerts, counts = ts.check_execution_outcome(lines)
        for a in alerts:
            mapped = {"NO_FILL_DESPITE_INTENTS": "NO_FILL_DESPITE_INTENTS",
                      "REJECT_RATE_SPIKE": "REJECT_STORM"}.get(a.get("code"))
            if mapped:
                anomalies.append({"system": "chimera", "name": mapped,
                                  "evidence": a.get("counts", {}) | {"msg": a.get("msg", "")}})
        # LOT_SIZE -1013 cluster — the recurring class the thresholds above MISS when
        # intent-count is low. Fire on the raw -1013 count directly (independent of rate).
        lot1013 = int(counts.get("lot1013", 0)) if isinstance(counts, dict) else 0
        if lot1013 >= 3 and not any(x["name"] == "REJECT_STORM" for x in anomalies):
            anomalies.append({"system": "chimera", "name": "LOT_SIZE_REJECTS",
                              "evidence": {"lot1013": lot1013,
                                           "reject_orders": counts.get("reject_orders"),
                                           "intents": counts.get("intents")}})
    except Exception as e:  # noqa: BLE001
        # FAIL-LOUD: a swallowed ingest error must NOT read as "book healthy" (the
        # silent-monitor class). Surface it as a high-sev anomaly so the run exits RED
        # and the cron wrapper notifies, instead of a false green.
        print(f"  ERROR: crypto exec-outcome ingest FAILED: {e}", file=sys.stderr)
        anomalies.append({"system": "chimera", "name": "INGEST_ERROR",
                          "evidence": {"error": str(e), "half": "crypto"}})
    # ── OMEGA: the sentinel has NO Omega exec-outcome pull, so classify the live
    #    [IBKR-EXEC] log directly (same intents-vs-fills-vs-rejects logic).
    try:
        anomalies.extend(_omega_exec_outcome())
    except Exception as e:  # noqa: BLE001
        print(f"  ERROR: omega exec-outcome ingest FAILED: {e}", file=sys.stderr)
        anomalies.append({"system": "omega", "name": "INGEST_ERROR",
                          "evidence": {"error": str(e), "half": "omega"}})
    return anomalies


# Omega exec-log outcome classifier (read-only ssh grep of the live [IBKR-EXEC] log).
OMEGA_EXEC_LOG = "C:/Omega/logs/omega_service_stderr.log"
OMEGA_EXEC_TAIL = 800
def _omega_exec_outcome() -> list[dict]:
    # ssh MUST start literally 'ssh omega-new' (feedback-vps-ssh-command-form).
    sh = (f"powershell -Command \"Get-Content '{OMEGA_EXEC_LOG}' -Tail {OMEGA_EXEC_TAIL} | "
          f"Select-String -Pattern '\\[IBKR-EXEC\\]'\"")
    try:
        out = subprocess.run(["ssh", "omega-new", sh], capture_output=True, text=True, timeout=45)
        raw = out.stdout if out.returncode == 0 else ""
    except Exception as e:  # noqa: BLE001
        print(f"  WARN: omega exec-log pull failed: {e}", file=sys.stderr)
        return []
    # AGE-GATE (2026-07-24b): only lines AFTER the last connect/boot marker, so a
    # STALE reject from a previous session can't false-fire (the crypto-side bug,
    # applied here too). nextValidId is logged on every (re)connect.
    all_lines = raw.splitlines()
    boot_idx = 0
    for i, ln in enumerate(all_lines):
        if "nextValidId" in ln:
            boot_idx = i + 1
    lines = all_lines[boot_idx:]
    placed = fills = rejects = blocked = 0
    for ln in lines:
        if "FILL " in ln:                                   fills += 1
        elif "REJECT oid" in ln or " err 201" in ln or " err 460" in ln: rejects += 1
        elif "BLOCKED " in ln:                              blocked += 1
        elif ("] LIVE " in ln or "] PAPER " in ln) and "qty=" in ln:     placed += 1
    out_anoms: list[dict] = []
    ev = {"placed": placed, "fills": fills, "rejects": rejects, "blocked": blocked}
    if placed >= 3 and fills == 0:
        out_anoms.append({"system": "omega", "name": "NO_FILL_DESPITE_INTENTS", "evidence": ev})
    if rejects >= 3 and placed and rejects / max(placed, 1) >= 0.5:
        out_anoms.append({"system": "omega", "name": "REJECT_STORM", "evidence": ev})
    return out_anoms


# AGE-GATE (2026-07-24b): only crypto exec lines AFTER the current binary's last
# [STARTUP] boot. A STALE pre-restart -1013 (from an old build) must NOT false-fire —
# that exact bug made the loop cry wolf on dead log lines. A live reject is post-boot
# by definition. Returns list[str], [] if none post-boot, or None on a real pull failure
# (so ingest fails LOUD instead of false-healthy).
CHIMERA_LOG = "~/ChimeraCrypto/logs/chimera.log"
def _crypto_exec_lines_since_boot():
    import trade_sentinel as ts
    grep = getattr(ts, "_EXEC_GREP",
                   r'INTENT\] (tag=|submit failed)|order rejected|Order failed|EXECUTOR\] FILLED|"code":-1013')
    sh = (f"boot=$(grep -an 'STARTUP.*build=' {CHIMERA_LOG} 2>/dev/null | tail -1 | cut -d: -f1); "
          f"[ -z \"$boot\" ] && boot=1; "
          f"tail -n +$boot {CHIMERA_LOG} 2>/dev/null | grep -aE '{grep}' | tail -600")
    try:
        out = subprocess.run(["ssh", "chimera-direct", sh], capture_output=True, text=True, timeout=45)
    except Exception:
        return None
    if out.returncode != 0:
        return None
    return [l for l in out.stdout.splitlines() if l.strip()]


# ── CLASSIFY + SCORE + DECIDE ─────────────────────────────────────────────────
def decide(anom: dict) -> dict | None:
    rule = RULEBOOK.get(anom["name"])
    if not rule:
        return None
    severity, tier, scope, human = rule
    return {
        "ts": _utcnow(),
        "system": anom["system"],
        "anomaly": anom["name"],
        "severity": severity,
        "action": tier,
        "scope": scope,
        "rationale": human,
        "evidence": anom.get("evidence", {}),
    }


# ── ACT ───────────────────────────────────────────────────────────────────────
def log_decision(dec: dict) -> None:
    DECISION_LOG.parent.mkdir(parents=True, exist_ok=True)
    with DECISION_LOG.open("a") as fh:
        fh.write(json.dumps(dec) + "\n")


def emit_halt_flag(dec: dict, push: bool) -> Path:
    CONTROL_DIR.mkdir(parents=True, exist_ok=True)
    # ONE book-wide flag per system — the binary poll opens exactly halt_<system>.flag.
    # A scope-tagged name (2026-07-24 audit) was written but NEVER read → ignored halt.
    # Any halt-worthy decision (HALT/DISABLE/CANCEL) maps to the one book-wide halt the
    # binary can honor; the decision's scope/rationale is preserved in the flag body + log.
    flag = CONTROL_DIR / f"halt_{dec['system']}.flag"
    flag.write_text(json.dumps({
        "ts": dec["ts"], "action": dec["action"], "anomaly": dec["anomaly"],
        "severity": dec["severity"], "reason": dec["rationale"],
    }) + "\n")
    if push:
        # scp to the box control dir (fire-and-forget; box binary polls it).
        box = {"omega": "omega-new", "chimera": "chimera-direct"}.get(dec["system"])
        dest = {"omega": "C:/Omega/control/", "chimera": "~/ChimeraCrypto/control/"}.get(dec["system"])
        if box and dest:
            subprocess.run(["scp", "-q", str(flag), f"{box}:{dest}"],
                           capture_output=True, timeout=25)
    return flag


def escalate(dec: dict, armed: bool) -> None:
    line = (f"[{dec['ts']}] {'ARMED' if armed else 'DRY-RUN'} sev={dec['severity']} "
            f"{dec['action']} {dec['system']}/{dec['scope']} {dec['anomaly']}: {dec['rationale']}")
    ALERT_LOG.parent.mkdir(parents=True, exist_ok=True)
    with ALERT_LOG.open("a") as fh:
        fh.write(line + "\n")
    print(("🛑 " if dec["action"] in (TIER_HALT, TIER_DISABLE) else "⚠️  ") + line)


def run_once(armed: bool, push: bool, self_test: bool) -> int:
    anomalies = ingest(self_test=self_test)
    decisions = [d for d in (decide(a) for a in anomalies) if d]
    if not decisions:
        print(f"[{_utcnow()}] sentinel-act: no anomalies — book healthy.")
        return 0
    worst = 0
    for dec in decisions:
        log_decision(dec)                    # PROOF: always logged
        escalate(dec, armed)                 # ALWAYS escalate
        if armed and dec["action"] in (TIER_HALT, TIER_DISABLE, TIER_CANCEL):
            flag = emit_halt_flag(dec, push)  # ACT: only when armed
            print(f"      -> halt flag written: {flag}")
        worst = max(worst, dec["severity"])
    print(f"[{_utcnow()}] sentinel-act: {len(decisions)} decision(s), worst severity {worst}. "
          f"Proof -> {DECISION_LOG}")
    return 2 if worst >= 80 else 1


def main() -> None:
    ap = argparse.ArgumentParser(description="Real-time anomaly control loop (the ACT layer).")
    ap.add_argument("--arm", action="store_true",
                    help="ARM the auto-halt: write control flags on HALT/DISABLE/CANCEL "
                         "(default is DRY-RUN: decide+log+alert only).")
    ap.add_argument("--push", action="store_true",
                    help="scp the halt flag to the box control dir (implies the binary poll).")
    ap.add_argument("--loop", type=int, metavar="SEC", default=0,
                    help="run continuously every SEC seconds (real-time cadence).")
    ap.add_argument("--self-test", action="store_true",
                    help="offline: run against synthetic anomalies (no ssh).")
    args = ap.parse_args()

    if args.loop:
        print(f"sentinel-act loop: every {args.loop}s, {'ARMED' if args.arm else 'DRY-RUN'}. Ctrl-C to stop.")
        while True:
            run_once(args.arm, args.push, args.self_test)
            time.sleep(args.loop)
    else:
        sys.exit(run_once(args.arm, args.push, args.self_test))


if __name__ == "__main__":
    main()
