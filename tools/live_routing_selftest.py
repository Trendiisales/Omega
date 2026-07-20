#!/usr/bin/env python3
"""
live_routing_selftest.py -- VERIFIED live-vs-shadow routing check, straight off the box.

Purpose: prove which engines route REAL money vs only the shadow ledger, from the
RUNNING BINARY's own boot log on omega-new -- not from source comments, not from any
session's claim. Run it yourself:  python3 tools/live_routing_selftest.py

It is deliberately dumb and un-fakeable: it SSHes omega-new, greps the live log for
  (a) [IBKR-EXEC] execution_broker=... port=NNNN paper_only=N   (the real exec state)
  (b) [OMEGA-INIT] <engine>: shadow=N enabled=N                 (per-engine routing)
and cross-checks every LIVE (shadow=0) engine against CERTIFIED_LIVE below.

RED if:
  - exec is not on the expected live account (4001 / paper_only=0), OR
  - an engine routes REAL money (shadow=0) but is NOT in CERTIFIED_LIVE, OR
  - a CERTIFIED_LIVE engine is sitting in shadow (shadow=1) i.e. not actually live.

CERTIFIED_LIVE is the allowlist of engines with a current PASS verdict. Editing it is
the ONLY way to bless a live engine -- so a stale/uncertified engine on real money
shows RED until someone consciously certifies + lists it. That is the anti-lie gate.
"""
import subprocess, re, sys, base64

HOST = "omega-new"                      # THE live box (never the retired dead box)
EXPECT_PORT = 4001                      # real IBKR account
EXPECT_PAPER_ONLY = 0                   # 0 = real money

# --- CERTIFIED_LIVE allowlist -----------------------------------------------
# key substring that appears in the engine's [OMEGA-INIT] line -> (verdict, note).
# Only engines with a CURRENT faithful-retest PASS belong here. Everything else
# routing real money is a RED until re-certified. PENDING = live but retest not
# yet complete this pass -> reported AMBER, must resolve to PASS or be scrapped.
CERTIFIED_LIVE = {
    "ConnorsRSI2 NAS100":        ("PASS",    "PF4.18 faithful retest 2026-07-20"),
    "XauTrendFollowD1":          ("PASS",    "PF1.62 robust 6/6 blocks 2026-07-20"),
    "DJ30+SPX D1 turtles":       ("PASS",    "re-cert 2026-07-21 faithful: DJ30 PF1.81 / SPX PF1.77, both regimes+ both WF halves+ 2x-cost survives"),
}
# Engines explicitly SCRAPPED / DISARMED (must NOT be live). RED if seen shadow=0 enabled=1.
SCRAPPED = {
    "NasTurtleD1":       "PF1.26 marginal -- scrap",
    "ConnorsRSI2 GER40": "PF1.35 marginal -- scrap or keep shadow",
    "TrendRider companions": "PF1.26 bear-neg PF0.75, not bull-gated -- FAIL retest 2026-07-20 -- DISARMED S-07-21",
    "GoldVolBreakoutM30":    "bear-neg, not bull-gated -- FAIL retest 2026-07-20 -- DISARMED S-07-21",
    "XauThreeBar30m":        "PF1.29 bear-neg, live cold-cut unverified -- FAIL retest 2026-07-20 -- DISARMED S-07-21",
    "XauTrendFollow4h":      "full-span +$ is pure bull beta; 2022 bear net-neg, not bull-gated, WF early-half neg -- FAIL re-cert 2026-07-21 -- DISARMED S-07-21",
    "XauTrendFollow1h":      "bull-only edge; 2022 bear -$1412 both WF halves neg, D1-EMA200 gate does not rescue bear -- FAIL re-cert 2026-07-21 -- DISARMED S-07-21",
}

def sh(cmd):
    return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=60)

def main():
    ps = (r"cd C:\Omega; git rev-parse --short HEAD; "
          r"Select-String -Path logs\omega_service_stdout.log,logs\omega_2026-07-20.log "
          r"-Pattern '\[IBKR-EXEC\].*paper_only|\[OMEGA-INIT\].*shadow=' -ErrorAction SilentlyContinue "
          r"| ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique")
    # base64 UTF-16LE -EncodedCommand -- avoids the omega-new nested-quote mangle ($_ eaten)
    b64 = base64.b64encode(ps.encode("utf-16-le")).decode()
    r = sh(f'ssh {HOST} powershell -NoProfile -EncodedCommand {b64}')
    if r.returncode != 0 and not r.stdout.strip():
        print(f"RED: could not reach {HOST} / no log lines\n{r.stderr[:400]}"); sys.exit(2)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    running_hash = lines[0].strip() if lines and re.match(r'^[0-9a-f]{7,}$', lines[0].strip()) else "?"

    red, amber = [], []

    # (a) exec state
    exec_ln = next((l for l in lines if "[IBKR-EXEC]" in l and "paper_only" in l), None)
    print(f"=== LIVE ROUTING SELF-TEST (box={HOST}, running={running_hash}) ===\n")
    if not exec_ln:
        print("RED: no [IBKR-EXEC] line -- exec state unknown"); red.append("no exec line")
    else:
        # port may render as `port=4001` OR only as `connected :4001` -> accept either
        port = int(m.group(1)) if (m := re.search(r'port=(\d+)', exec_ln)) \
               else (int(m.group(1)) if (m := re.search(r':(\d{4})\b', exec_ln)) else -1)
        po   = int(m.group(1)) if (m := re.search(r'paper_only=(\d+)', exec_ln)) else -1
        ok = (port == EXPECT_PORT and po == EXPECT_PAPER_ONLY)
        print(f"EXEC: port={port} paper_only={po}  ->  {'LIVE REAL-MONEY' if ok else 'NOT the expected live account'}  [{'GREEN' if ok else 'RED'}]")
        if not ok: red.append(f"exec port={port} paper_only={po} (want {EXPECT_PORT}/0)")

    # (b) per-engine routing
    print("\n--- per-engine routing (shadow=0 => routes REAL 4001 orders) ---")
    inits = [l for l in lines if "[OMEGA-INIT]" in l and "shadow=" in l]
    for l in inits:
        body = l.split("[OMEGA-INIT]",1)[1].strip()
        name = body.split(":",1)[0].strip()
        sh_m = re.search(r'shadow=(\d|true|false)', body)
        en_m = re.search(r'enabled=(\d)', body)
        is_shadow = sh_m and sh_m.group(1) in ("1","true")
        enabled = (en_m.group(1) == "1") if en_m else True   # some lines omit enabled=; assume on
        if is_shadow or not enabled:
            continue   # shadow ledger or off -> no real money -> not this check's concern
        # LIVE (shadow=0, enabled): must be certified
        key = next((k for k in CERTIFIED_LIVE if k in name or name.startswith(k)), None)
        scrap = next((k for k in SCRAPPED if k in name), None)
        if scrap:
            print(f"  RED    LIVE {name:34s} <- SCRAPPED ({SCRAPPED[scrap]}) but routing real money")
            red.append(f"{name} scrapped-but-live")
        elif key:
            verdict, note = CERTIFIED_LIVE[key]
            tag = "GREEN " if verdict == "PASS" else "AMBER "
            print(f"  {tag} LIVE {name:34s} <- {verdict} ({note})")
            if verdict != "PASS": amber.append(f"{name} {verdict}")
        else:
            print(f"  RED    LIVE {name:34s} <- NOT in CERTIFIED_LIVE (uncertified real money)")
            red.append(f"{name} uncertified-live")

    # (c) certified engines that are NOT live (should be)
    live_names = " | ".join(l.split("[OMEGA-INIT]",1)[1] for l in inits
                            if (m := re.search(r'shadow=(\d|true|false)', l)) and m.group(1) in ("0","false"))
    for k,(v,_) in CERTIFIED_LIVE.items():
        if v == "PASS" and k not in live_names:
            print(f"  RED    certified '{k}' is NOT routing live (in shadow / absent)")
            red.append(f"{k} certified-but-not-live")

    print("\n=== VERDICT ===")
    if red:
        print(f"RED  ({len(red)} issue(s)):"); [print(f"   - {x}") for x in red]
        if amber: print(f"AMBER ({len(amber)} pending-cert):"); [print(f"   - {x}") for x in amber]
        sys.exit(1)
    if amber:
        print(f"AMBER ({len(amber)} live-but-cert-pending -- finish retest or scrap):")
        [print(f"   - {x}") for x in amber]; sys.exit(3)
    print("GREEN -- every real-money engine is certified; exec on the live account."); sys.exit(0)

if __name__ == "__main__":
    main()
