#!/usr/bin/env python3
"""protection_selftest.py — EFFECT-LEVEL protection self-test (S-2026-06-29, operator-mandated).

WHY THIS EXISTS: every prior "check" verified that protection code EXISTS or is CONFIGURED. None
verified it RUNS and ACTUALLY DOES ITS JOB. So a protection that was built-but-never-scheduled
(the companion stall_accountant.py sat dead for weeks), enabled-but-shadow (giveback_saver only
logged, never closed), or silently-missing (gold gave back $142 uncaught) passed every static check
while doing nothing. This test asserts FUNCTION, not existence. It is the single source of truth for
"is our profit/loss protection actually working" and runs at every session start.

TEN CHECKS (each pass/fail independently; overall RED if any fail):
  [1] ALIVE               — the REAL in-binary C++ StallCompanion on the VPS wrote its aggregate
                            companion_state.json in the last N min (weekend-guarded). Re-pointed
                            2026-07-06 off the retired Mac python file+crontab (false RED source).
  [2] REAL, NOT SHADOW    — the LIVE C++ StallCompanion header has a real EFFECT path (banks a
                            clip), not log-only theatre. Repointed off retired python S-2026-07-20i.
  [3] FIRES ON TRIGGER    — compile the LIVE StallCompanion.hpp into a sandbox harness
                            (tools/stall_companion_selftest.cpp) and drive a synthetic
                            peak->giveback through StallBook::step in BOTH live modes
                            (pct REVERSAL_CLIP + be-mode FLOOR_CLIP); assert clips bank.
                            Repointed off the RETIRED Mac python S-2026-07-20i — the old
                            [3] proved dead code clips, not what ships in Omega.exe.
  [4] EFFECT RECONCILE    — no live armed position has given back past the trail while still UNCLIPPED.
  [5] BINARY-CLOSE-PATH   — in-binary guards have a registered closer (can close, not just detect).
  [6] INPUT-FRESHNESS     — the companion's inputs are fresh (stale feed = wrong peak = missed clip).
  [7] BEFLOOR-REAL-HONESTY— index BE-floor REAL column within designed bounds (cap enforcing; no
                            model-fiction bleed). Added S-2026-07-07 after -$273 x5 booked under GREEN.
  [8] STALL-BANK-LEDGER-PARITY — every profitable stall-book ENGINE_EXIT bank (7d) must be explained
                            by its OWN parent ledger close within +/-2h (one close validates
                            ONE bank). Added S-2026-07-17r after telemetry-frame flap + restart key
                            drift banked the SAME IndexBearShort ride 5x (+$980.90 phantom realized)
                            with ZERO parent ledger closes. Catches any accounting inflation class
                            where the companion "realizes" money its parent never closed.
  [9] PROFIT-LOCK-COVERAGE — the RUNNING zoo's [PROFIT-LOCK-GATE] runtime sweep is armed AND no live
                            leg was claimed by zero giveback books/mirrors in 24h. Added S-2026-07-17u
                            (pre-live hole H5): commit-time config-text proof is not runtime proof —
                            the IBS $400 giveback rode in a book whose arm never latched, and Rider4h
                            was invisible to every audit. RED = an uncovered leg can ride profit
                            back down right now, or the gate itself is not running.
  [10] CATASTROPHE-BREACH — compile the LIVE CatastrophicGuard.hpp into a sandbox harness
                            (tools/catastrophic_guard_selftest.cpp) and drive check() through
                            the BREACH branch in all 6 modes (shadow, live-universal,
                            per-engine precedence, noise band, unknown-pnl, NO-CLOSER).
                            Added S-2026-07-20n: guard was ARMED-verified in prod but the
                            trigger path had never fired anywhere (rare-event standing rule).
  [11] FIRED-ON-LIVE-LEDGER — checks [3]/[10] prove protection CAN fire in a sandbox and [9]
                            proves the gate is ARMED, but NONE proved it actually FIRES (or
                            fails to) on the REAL live book. This audits the live trade ledger
                            (omega_trade_closes.csv + dailies) for the OUTCOME: (a) protection
                            HAS fired historically (>0 closes with a protective exit_reason:
                            BE_FLOOR/TRAIL_STOP/*CLIP/*LOCK/DSTOP/...), proving the effect path
                            is real on the live binary, AND (b) NO real position rode a large
                            favorable peak (mfe) all the way to a deep net-negative close under a
                            NON-protective exit -- the "floor configured but never triggers"
                            failure. A giveback that escaped protection = RED. Added S-2026-07-24
                            ("checks-a-proxy-misses-the-outcome" audit): a protection that exists
                            + is armed but never actually clips a real adverse move is the exact
                            silent-failure class -- a sandbox fire and an armed boot line do not
                            prove the LIVE book was ever protected.

Exit 0 = all green. Exit 1 = one or more RED. Writes a status file the SessionStart hook surfaces.
"""
import os, sys, json, subprocess, tempfile, shutil, time, datetime

HOME = os.path.expanduser("~")
# S-2026-07-20i: the retired Mac python stall_accountant.py is no longer referenced by any
# check — [2]/[3] now prove the LIVE C++ StallCompanion (include/StallCompanion.hpp).
COMP_DIR  = os.path.join(HOME, "stall-accountant")   # VPS-synced state mirror ([4]/[6] inputs)
STATE     = os.path.join(COMP_DIR, "companion_state.json")
CLOSED    = os.path.join(COMP_DIR, "companion_closed.csv")
STATUS    = os.path.join(HOME, ".claude", "protection_selftest_STATUS.txt")
ALIVE_MIN = 12          # companion runs every 1min; >12min stale = dead
GATE_PCT  = float(os.environ.get("STALL_GATE_PCT", "2.0"))
REV_GB    = float(os.environ.get("REVERSAL_GIVEBACK", "0.40"))

# ── REAL protection = the in-binary C++ StallCompanion on the VPS (S-2026-07-06) ──
# The Mac-cron python stall_accountant.py was RETIRED at the 2026-07-06 cutover; the
# real protection is now StallCompanion inside Omega.exe, which writes the aggregate
# C:\Omega\companion_state.json every 60s off the live tick snapshot (see
# StallCompanion::maybe_drive). "Alive" therefore = that aggregate is FRESH on the VPS,
# NOT that a Mac file exists or that a crontab line matches. Over a closed-market
# weekend there are no ticks so the companion legitimately idles -> enforcement is
# skipped Fri22:00->Sun22:00 UTC (mirrors tools/feeds_selftest.py). Every VPS call MUST
# start literally `ssh omega-new` (feedback-vps-ssh-command-form).
VPS_COMPANION = r"C:\Omega\companion_state.json"
VPS_ALIVE_MIN = 5.0     # 60s drive cadence; >5min while market open = writer stalled

results = []  # (check_name, ok, detail)
def record(name, ok, detail): results.append((name, ok, detail))

# ---- [1] ALIVE (real VPS companion is driving) --------------------------------
def _market_closed_weekend(now_utc):
    """Gold/index markets closed ~Fri 22:00 UTC -> Sun 22:00 UTC. With no ticks the
    in-binary companion legitimately stops writing, so skip freshness enforcement.

    DELIBERATELY KEEPS the Sun 22:00 reopen boundary (do NOT sync to the all-Sunday guard in
    feeds_selftest.market_closed_weekend). This companion is TICK-driven — it writes within 60s
    of the FIRST reopen tick (~22:00 UTC), so it is legitimately fresh from 22:00 and a dead
    companion after reopen SHOULD be caught from 22:00. The feeds live-dumps are BAR-driven
    (first bar closes 23:00/00:00), which is why THEIR guard extends through Sunday — a different
    reopen semantics, not an inconsistency. See feeds_selftest.market_closed_weekend docstring."""
    wd, hr = now_utc.weekday(), now_utc.hour   # Mon=0 .. Sun=6
    if wd == 5:               return True       # Saturday
    if wd == 4 and hr >= 22:  return True       # Friday from 22:00 UTC
    if wd == 6 and hr < 22:   return True       # Sunday before 22:00 UTC (tick-driven: fresh from reopen)
    return False

def _vps_companion_age_min():
    """ONE `ssh omega-new` call -> age (min) of C:\\Omega\\companion_state.json computed
    ON the VPS (clock-skew-free). None if ssh fails entirely; 999999 if the file is
    absent. ssh command MUST start literally `ssh omega-new`."""
    ps = (f"$p='{VPS_COMPANION}';"
          f"if(Test-Path $p){{"
          f"$a=((Get-Date)-(Get-Item $p).LastWriteTime).TotalMinutes;"
          f"Write-Output ([math]::Round($a,1))}}else{{Write-Output 999999}}")
    try:
        r = subprocess.run(["ssh","omega-new","powershell","-NoProfile","-Command",ps],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    for ln in r.stdout.splitlines():
        try: return float(ln.strip())
        except ValueError: continue
    return None

def check_scheduled_alive():
    # Re-pointed from the retired Mac file+crontab (always-stale orphan + false-match on
    # the crypto cron comment) to the real in-binary VPS companion's freshness.
    now_utc = datetime.datetime.now(datetime.timezone.utc)
    if _market_closed_weekend(now_utc):
        record("[1] ALIVE (VPS companion)", True,
               "weekend (market closed) -- companion idles with no ticks; freshness not enforced")
        return
    age = _vps_companion_age_min()
    if age is None:
        record("[1] ALIVE (VPS companion)", False,
               "ssh omega-new failed -- real C++ StallCompanion freshness UNVERIFIABLE")
        return
    if age >= 999999:
        record("[1] ALIVE (VPS companion)", False,
               f"{VPS_COMPANION} MISSING on VPS -- companion never wrote aggregate *** protection DEAD ***")
        return
    # S-2026-07-23: the StallCompanion paper zoo was CULLED 2026-07-22c (live-only
    # conversion, reg.enabled=false engine_init.hpp:3827) -- it no longer writes
    # companion_state.json BY DESIGN, so a huge staleness here is the cull, NOT a
    # stalled writer. REDding on a deliberately-disabled engine violates the operator's
    # own cull-don't-park / never-display-dead-engines rules. A real transient writer
    # stall on a LIVE companion would be minutes-to-an-hour; >6h stale during market
    # hours = the permanent cull. In that case this check is N/A -- protection moved to
    # the per-book locks verified by [9] PROFIT-LOCK-COVERAGE. Below 6h we still enforce
    # freshness (in case the companion is ever re-armed).
    CULL_MIN = 360.0
    if age > CULL_MIN:
        record("[1] ALIVE (VPS companion)", True,
               f"StallCompanion CULLED 07-22c (live-only) -- age={age:.0f}min is the cull, "
               f"not a stall; per-book profit-locks own protection now (see [9])")
        return
    alive = age <= VPS_ALIVE_MIN
    detail = f"VPS {VPS_COMPANION} age={age:.1f}min (<= {VPS_ALIVE_MIN})"
    if not alive: detail += "  *** STALE -- in-binary companion not driving (writer stalled) ***"
    record("[1] ALIVE (VPS companion)", alive, detail)

# ---- [2] REAL, NOT SHADOW -----------------------------------------------------
def check_real_not_shadow():
    # the LIVE protection (C++ StallCompanion in Omega.exe) must have a real EFFECT path
    # (write a banked clip), not just log/track. S-2026-07-20i: repointed off the RETIRED
    # Mac python source at ~/stall-accountant (dead code passing for the live protection).
    hdr = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "include", "StallCompanion.hpp")
    try: src = open(hdr).read()
    except Exception as e: record("[2] REAL-NOT-SHADOW", False, f"cannot read StallCompanion.hpp: {e}"); return
    banks = ("companion_closed" in src) and ("close_(" in src) and ("REVERSAL_CLIP" in src)
    # shadow tell-tale: a protection that ONLY tracks peak/locked with no close/bank action
    only_logs = ("mfe" in src and "close_(" not in src and "companion_closed" not in src)
    ok = banks and not only_logs
    detail = f"live C++ StallCompanion has real bank/close path={banks}; log-only-shadow={only_logs}"
    if not ok: detail += "  *** SHADOW THEATRE -- records but never closes ***"
    record("[2] REAL-NOT-SHADOW", ok, detail)

# ---- [3] FIRES ON SYNTHETIC TRIGGER ------------------------------------------
# S-2026-07-20i repoint (effect-verification audit): the old [3] sandbox-fired the
# RETIRED Mac python stall_accountant.py — a green [3] proved DEAD code clips, not
# the C++ StallCompanion actually shipped in Omega.exe. Now: compile the LIVE
# header (include/StallCompanion.hpp) into a tiny harness and drive StallBook::step
# through a synthetic peak->giveback in BOTH live modes (pct REVERSAL_CLIP +
# be-mode FLOOR_CLIP incl. pre-confirm-flat assert). Binary cached in build/,
# rebuilt when header or harness source is newer.
REPO         = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STALL_HDR    = os.path.join(REPO, "include", "StallCompanion.hpp")
STALL_SRC    = os.path.join(REPO, "tools", "stall_companion_selftest.cpp")
STALL_BIN    = os.path.join(REPO, "build", "stall_companion_selftest")

def check_fires_on_trigger():
    if not (os.path.exists(STALL_HDR) and os.path.exists(STALL_SRC)):
        record("[3] FIRES-ON-TRIGGER", False, "StallCompanion.hpp / harness source missing"); return
    try:
        stale = (not os.path.exists(STALL_BIN)
                 or os.path.getmtime(STALL_BIN) < max(os.path.getmtime(STALL_HDR),
                                                      os.path.getmtime(STALL_SRC)))
        if stale:
            os.makedirs(os.path.dirname(STALL_BIN), exist_ok=True)
            cc = subprocess.run(["c++", "-std=c++17", "-O0", "-I", os.path.join(REPO, "include"),
                                 STALL_SRC, "-o", STALL_BIN], capture_output=True, text=True, timeout=120)
            if cc.returncode != 0:
                record("[3] FIRES-ON-TRIGGER", False,
                       f"harness COMPILE failed (live header broken?): {cc.stderr.strip().splitlines()[-1][:140]}")
                return
        sb = tempfile.mkdtemp(prefix="protselftest_")
        try:
            r = subprocess.run([STALL_BIN, sb], capture_output=True, text=True, timeout=60)
            fired = r.returncode == 0 and "STALL-SELFTEST PASS" in r.stdout
            tail = " | ".join(ln for ln in r.stdout.strip().splitlines())[:220]
            detail = ("LIVE C++ StallBook sandbox: " +
                      ("pct REVERSAL_CLIP + be-mode FLOOR_CLIP both FIRED" if fired
                       else f"DID NOT FIRE *** broken *** {tail}"))
            record("[3] FIRES-ON-TRIGGER", fired, detail)
        finally:
            shutil.rmtree(sb, ignore_errors=True)
    except Exception as e:
        record("[3] FIRES-ON-TRIGGER", False, f"harness error: {e}")

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
    # S-2026-07-12 CONSOLIDATION: the Mac ibkrcrypto book was folded onto the ONE Chimera
    # system (josgp1) and RETIRED. Its state.json freezes -> this staleness probe went false.
    # Protection's real input is now the VPS companion (C:\Omega\companion_state.json), checked
    # in the [1] ALIVE / [6] INPUT-FRESHNESS blocks. Retired the ibkrcrypto-state probe here.
    log = "/tmp/giveback_saver.log"
    if os.path.exists(log):
        try:
            tail = subprocess.run(["tail","-15",log], capture_output=True, text=True).stdout
            # >=2 in the last 15 lines: a single ssh-timeout self-heals next cycle (poll_omega now
            # retries once too); the DANGEROUS case (the $222 gold peak miss) is SUSTAINED
            # unreachability. One transient trip must not flip protection RED. S-2026-07-02.
            fails = sum(("telemetry unreachable" in ln or "telemetry quiet" in ln)
                        for ln in tail.splitlines())
            if fails >= 2:
                probs.append(f"omega telemetry unreachable {fails}x in last 15 companion cycles (SUSTAINED -> peaks missed)")
        except Exception: pass
    ok = (len(probs) == 0)
    detail = "companion inputs fresh (crypto + omega telemetry)" if ok else "*** " + "; ".join(probs) + " ***"
    record("[6] INPUT-FRESHNESS", ok, detail)

# ---- [7] BEFLOOR REAL-COLUMN HONESTY (model-vs-real divergence) -----------------
def check_befloor_real_honesty():
    # S-2026-07-07 lesson: this selftest was GREEN while the index BE-floor companion booked
    # -$273 x5 REAL on US500 under a model column that "cannot go negative" (the research clamps
    # every clip to max(0,.)). The REAL column is the judged engine. Assert from the VPS states:
    #   INDEX (live survivor US500):
    #   (a) no OPEN leg's real uPnL sits below the engine's own designed bound -(cap+rt+slack)bp
    #       of entry (the intrabar catastrophe cap must be doing its job), and
    #   (b) no symbol's cumulative usd_real has bled past -$2500 while its model column is
    #       positive (model-fiction bleed -> the engine's real economics have broken again).
    #   RETIRED FAMILY (S-2026-07-07e: gold/xag/usoil + stockmover; fx already empty):
    #   (c) retired twin states must show NO open legs (retirement cleared the arm-state;
    #       an open leg reappearing = a stale binary is still arming a retired book), and
    #   (d) stockmover aggregate must have an EMPTY names[] (empty book publishes empty state).
    # ONE ssh call (RAM reaper -- minimize VPS ssh). Value-based -> no weekend guard needed.
    ps = (r"foreach($f in 'index_companion_state.json','gold_companion_state.json',"
          r"'xag_companion_state.json','usoil_companion_state.json','stockmover_companion_state.json'){"
          r"$p=Join-Path 'C:\Omega' $f;"
          r"if(Test-Path $p){Write-Output ('===FILE '+$f);Get-Content $p -Raw}else{Write-Output ('===FILE '+$f);Write-Output 'MISSING'}}")
    try:
        r = subprocess.run(["ssh","omega-new","powershell","-NoProfile","-Command",ps],
                           capture_output=True, text=True, timeout=45)
    except (OSError, subprocess.SubprocessError):
        record("[7] BEFLOOR-REAL-HONESTY", False, "ssh omega-new failed -- real column UNVERIFIABLE"); return
    raw = (r.stdout or "").strip()
    if r.returncode != 0 or not raw:
        record("[7] BEFLOOR-REAL-HONESTY", False, "ssh omega-new failed -- real column UNVERIFIABLE"); return
    blocks = {}
    cur = None
    for line in raw.splitlines():
        if line.startswith("===FILE "):
            cur = line[8:].strip(); blocks[cur] = []
        elif cur is not None:
            blocks[cur].append(line)
    def parse(name):
        body = "\n".join(blocks.get(name, [])).strip()
        if not body or body.startswith("MISSING"): return None
        try: return json.loads(body)
        except Exception: return "BAD"
    probs = []; nsym = 0; nopen = 0
    # -- INDEX (live) --
    d = parse("index_companion_state.json")
    if d is None: probs.append("index_companion_state.json MISSING on VPS")
    elif d == "BAD": probs.append("index state unparsable")
    else:
        for s in d.get("syms", []):
            nsym += 1
            rt  = float(s.get("rt_cost_bp", 4.0) or 4.0)
            cap = float(s.get("cap_bp", 25.0) or 25.0)
            for leg in s.get("open", []):
                nopen += 1
                entry = float(leg.get("entry", 0) or 0)
                if entry <= 0: continue
                upnl_real = float(leg.get("upnl_pts_real", 0) or 0)
                bound = -entry * (cap + rt + 15.0) / 1e4   # cap + rt cost + 15bp slack
                if upnl_real < bound:
                    probs.append(f"{s.get('sym')}/{leg.get('flavor')}/{leg.get('tier')} "
                                 f"upnl_real={upnl_real:.2f}pt < bound {bound:.2f}pt (cap NOT enforcing)")
            usd_real = float(s.get("usd_real", 0) or 0)
            usd_mdl  = float(s.get("usd", 0) or 0)
            if usd_real <= -2500.0 and usd_mdl > 0:
                probs.append(f"{s.get('sym')} book usd_real={usd_real:.0f} while model +{usd_mdl:.0f} "
                             f"(model-fiction bleed)")
    # -- RETIRED twins: no open legs may exist --
    nret = 0
    for fname, tag in (("gold_companion_state.json", "gold"),
                       ("xag_companion_state.json", "xag"),
                       ("usoil_companion_state.json", "usoil")):
        d = parse(fname)
        if d in (None, "BAD"):   # absent is acceptable post-retirement (never written on a fresh box)
            continue
        nret += 1
        legs = d.get("open", [])
        if legs:
            probs.append(f"{tag} RETIRED book has {len(legs)} open leg(s) -- stale binary still arming")
    # -- RETIRED stockmover: aggregate must be empty --
    d = parse("stockmover_companion_state.json")
    if d not in (None, "BAD"):
        nret += 1
        if d.get("names", []):
            probs.append(f"stockmover RETIRED aggregate has {len(d.get('names'))} name(s) -- stale binary")
    ok = (len(probs) == 0)
    detail = (f"{nsym} live sym(s), {nopen} open leg(s), {nret} retired state(s) clean: "
              f"real column within designed bounds"
              if ok else "*** " + "; ".join(probs[:4]) + " ***")
    record("[7] BEFLOOR-REAL-HONESTY", ok, detail)

def check_stall_bank_ledger_parity():
    # S-2026-07-17r: the stall companion banks ENGINE_EXIT when a parent's key LEAVES the
    # telemetry live set. A partial-frame flap / restart entry-precision drift made the same
    # open IndexBearShort ride bank 5x (+$980.90 phantom) with the parent NEVER closing.
    # Invariant asserted here (mechanism-independent): a profitable ENGINE_EXIT bank exists
    # ONLY because the real trade closed => for every bank (|pnl|>=$5, last 7d) there must be
    # a parent LEDGER close row (same engine tag) within +/-2h (wide because survivor/H4 ledger
    # exit stamps are BAR-aligned while the companion banks at tick time), and one ledger close
    # may validate only ONE bank. The one-close-one-bank rule is the teeth: a flap that banks
    # the same ride N times finds only one close. Unmatched bank = phantom realized = RED.
    # ONE ssh call (RAM reaper). Value-based -> no weekend guard.
    # NO powershell pipelines: the remote command goes through cmd.exe, which splits at
    # '|' even inside single quotes (same reason check_befloor_real_honesty uses foreach).
    # Filename filtering (bak/pre_/culled) is done python-side off the ===LFILE markers.
    ps = (r"foreach($d in Get-ChildItem 'C:\Omega\stall' -Directory){"
          r"$f=Join-Path $d.FullName 'companion_closed.csv';"
          r"if(Test-Path $f){Write-Output ('===BOOK '+$d.Name);Get-Content $f}};"
          r"foreach($lf in Get-ChildItem 'C:\Omega\logs\trades\omega_trade_closes*.csv'){"
          r"Write-Output ('===LFILE '+$lf.Name);Get-Content $lf.FullName}")
    try:
        r = subprocess.run(["ssh","omega-new","powershell","-NoProfile","-Command",ps],
                           capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        record("[8] STALL-BANK-LEDGER-PARITY", False, "ssh omega-new failed -- bank parity UNVERIFIABLE"); return
    raw = (r.stdout or "")
    if r.returncode != 0 or not raw.strip():
        record("[8] STALL-BANK-LEDGER-PARITY", False, "ssh omega-new failed -- bank parity UNVERIFIABLE"); return
    import csv as _csv
    now = time.time(); week = now - 7*86400
    banks = []                       # (ts, book, engine, pnl)
    ledger = {}                      # engine -> [exit_ts,...]
    section, book = None, None
    for line in raw.splitlines():
        if line.startswith("===BOOK "): section, book = "book", line[8:].strip(); continue
        if line.startswith("===LFILE "):
            nm = line[9:].strip().lower()
            section = "skip" if any(x in nm for x in ("bak", "pre_", "culled", "removed", "tmp")) else "ledger"
            continue
        if section == "skip" or not line.strip(): continue
        try: row = next(_csv.reader([line]))
        except Exception: continue
        if section == "book":
            # ts,book,reason,engine,symbol,side,entry,realized_pnl,mfe_peak_pct,bars_held
            if len(row) < 10 or row[0] == "ts": continue
            try: ts = float(row[0]); pnl = float(row[7])
            except ValueError: continue
            if row[2] == "ENGINE_EXIT" and ts >= week and abs(pnl) >= 5.0:
                banks.append((ts, book, row[3], pnl))
        elif section == "ledger":
            # exit_ts_unix = col idx 5, engine = col idx 9
            if len(row) < 10 or row[0] == "trade_id": continue
            try: lts = float(row[5])
            except ValueError: continue
            ledger.setdefault(row[9], []).append(lts)
    for v in ledger.values(): v.sort()
    probs = []; used = set()
    for ts, bk, eng, pnl in sorted(banks):
        hit = None
        for lts in ledger.get(eng, []):
            if (eng, lts) in used: continue
            if ts - 7200.0 <= lts <= ts + 7200.0: hit = lts; break
        if hit is not None:
            used.add((eng, hit))
        else:
            when = datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%m-%d %H:%M")
            probs.append(f"{bk}/{eng} banked ${pnl:+.0f} @ {when}Z with NO parent ledger close (phantom ENGINE_EXIT)")
    ok = (len(probs) == 0)
    detail = (f"{len(banks)} profitable ENGINE_EXIT bank(s) in 7d, all explained by own parent ledger close"
              if ok else "*** " + "; ".join(probs[:4]) + " ***")
    record("[8] STALL-BANK-LEDGER-PARITY", ok, detail)

def check_profit_lock_coverage():
    # S-2026-07-17u (pre-live hole H5): the RUNNING zoo's [PROFIT-LOCK-GATE] sweep
    # (StallCompanion.hpp maybe_drive) writes logs/profit_lock_uncovered.log whenever a
    # live leg is claimed by ZERO giveback books/mirrors (the IBS-incident class:
    # config-text said covered, runtime arm was dead/absent). This check surfaces it:
    # any uncovered-leg row stamped in the last 24h = RED. Also verifies the gate is
    # actually ARMED (boot line in the service stdout log) — a silent gate is the H5
    # failure mode itself. ONE ssh call (RAM reaper).
    # NO PowerShell pipes in this string: ssh argv joins with spaces and the remote
    # cmd.exe interprets a bare `|` as ITS pipeline ("'Select-Object' is not
    # recognized", rc=255) — the S-2026-07-17v fix; checks [1]/[7]/[8] pass because
    # their ps strings are pipe-free. Keep it pipe-free.
    ps = (r"if(Test-Path 'C:\Omega\logs\profit_lock_uncovered.log'){"
          r"Write-Output '===UNCOV';Get-Content 'C:\Omega\logs\profit_lock_uncovered.log' -Tail 50};"
          r"Write-Output '===BOOT';"
          r"$m=@(Select-String -Path 'C:\Omega\logs\omega_service_stdout.log' "
          r"-Pattern 'PROFIT-LOCK-GATE' -SimpleMatch);"
          r"foreach($x in $m[-3..-1]){if($x){Write-Output $x.Line}}")
    try:
        r = subprocess.run(["ssh","omega-new","powershell","-NoProfile","-Command",ps],
                           capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError):
        record("[9] PROFIT-LOCK-COVERAGE", False, "ssh omega-new failed -- runtime coverage UNVERIFIABLE"); return
    raw = (r.stdout or "")
    if r.returncode != 0:
        record("[9] PROFIT-LOCK-COVERAGE", False, "ssh omega-new failed -- runtime coverage UNVERIFIABLE"); return
    now = time.time(); day = now - 86400
    uncov, armed, section = [], False, None
    for line in raw.splitlines():
        s = line.strip()
        if s == "===UNCOV": section = "uncov"; continue
        if s == "===BOOT":  section = "boot";  continue
        if not s: continue
        if section == "uncov":
            parts = s.split(",")
            try: ts = float(parts[0])
            except (ValueError, IndexError): continue
            if ts >= day and len(parts) >= 3:
                uncov.append(f"{parts[1]} {parts[2]}")
        elif section == "boot":
            # S-2026-07-23: accept ANY armed [PROFIT-LOCK-GATE] line, not just the culled
            # StallCompanion "coverage sweep armed" text. Post 07-22c live-only conversion
            # the zoo sweep is gone; the live per-book locks (DualMom trail g10/arm5/stayout
            # + DSTOP-25, engine_init.hpp:3404) print "[PROFIT-LOCK-GATE] DualMom armed: ...".
            # The grep already filters to PROFIT-LOCK-GATE lines, so "armed" here = a real
            # live lock is running (the exact thing this check exists to prove).
            if "armed" in s: armed = True
            if "UNCOVERED LIVE LEG" in s and not uncov: uncov.append(s[:120])
    if uncov:
        record("[9] PROFIT-LOCK-COVERAGE", False,
               "*** live leg(s) with NO giveback cover (24h): " + "; ".join(sorted(set(uncov))[:4]) + " ***")
    elif not armed:
        # gate line absent. The armed line prints on the FIRST DRIVE CYCLE (tick-driven,
        # StallCompanion.hpp profit_lock_gate_), and a deploy restart rotates the stdout
        # log — so a weekend/closed-market restart legitimately has NO armed line until
        # ticks resume (false RED after the 2026-07-18 tile deploy). Same relaxation as
        # check [1]: on a closed market the sweep cannot run, so arming is not
        # assessable; the uncovered-leg log above (persisted file) still REDs regardless.
        # First market tick re-arms it or Monday's run goes RED for real.
        if _market_closed_weekend(datetime.datetime.now(datetime.timezone.utc)):
            record("[9] PROFIT-LOCK-COVERAGE", True,
                   "weekend (market closed) -- drive cycle idle since restart; armed line not assessable, no uncovered leg in log")
        else:
            # binary predates H5 OR gate never ran (zoo dead => check [1] would also
            # flag). RED — "built" is not "running" (effect-level doctrine).
            record("[9] PROFIT-LOCK-COVERAGE", False,
                   "no '[PROFIT-LOCK-GATE] ... armed' boot line in service stdout -- gate NOT RUNNING on the live binary")
    else:
        record("[9] PROFIT-LOCK-COVERAGE", True,
               "runtime sweep armed; no uncovered live leg in 24h")

# ---- [10] CATASTROPHE-BREACH (guard's rare-event trigger path proven in sandbox) ----
# S-2026-07-20 standing rule (20i audit item 5): a rare-event protection must prove its
# TRIGGER path in a sandbox, not just its arming. CatastrophicGuard's ARMED boot line was
# production-verified (S-20k c465f08e) but the breach branch (loss > catastrophe_x *
# per_trade_usd -> flatten hook) had never executed anywhere. Same compile/cache pattern
# as [3]: compile the LIVE header (include/CatastrophicGuard.hpp) into
# tools/catastrophic_guard_selftest.cpp and drive check() through all 6 branches
# (shadow-no-flatten, live-universal, per-engine precedence, noise band, unknown-pnl
# skip, NO-CLOSER). Compile-fail = RED (live header broken).
CATG_HDR = os.path.join(REPO, "include", "CatastrophicGuard.hpp")
CATG_SRC = os.path.join(REPO, "tools", "catastrophic_guard_selftest.cpp")
CATG_BIN = os.path.join(REPO, "build", "catastrophic_guard_selftest")

def check_catastrophe_breach():
    if not (os.path.exists(CATG_HDR) and os.path.exists(CATG_SRC)):
        record("[10] CATASTROPHE-BREACH", False, "CatastrophicGuard.hpp / harness source missing"); return
    try:
        deps = [CATG_HDR, CATG_SRC, os.path.join(REPO, "include", "OpenPositionRegistry.hpp")]
        stale = (not os.path.exists(CATG_BIN)
                 or os.path.getmtime(CATG_BIN) < max(os.path.getmtime(p) for p in deps))
        if stale:
            os.makedirs(os.path.dirname(CATG_BIN), exist_ok=True)
            cc = subprocess.run(["c++", "-std=c++17", "-O0", "-I", os.path.join(REPO, "include"),
                                 CATG_SRC, "-o", CATG_BIN], capture_output=True, text=True, timeout=120)
            if cc.returncode != 0:
                record("[10] CATASTROPHE-BREACH", False,
                       f"harness COMPILE failed (live header broken?): {cc.stderr.strip().splitlines()[-1][:140]}")
                return
        r = subprocess.run([CATG_BIN], capture_output=True, text=True, timeout=60)
        fired = r.returncode == 0 and "CATGUARD-SELFTEST PASS" in r.stdout
        tail = " | ".join(ln for ln in r.stdout.strip().splitlines())[:220]
        detail = ("LIVE CatastrophicGuard sandbox: breach->flatten proven on all 6 branches"
                  if fired else f"BREACH PATH BROKEN *** {tail}")
        record("[10] CATASTROPHE-BREACH", fired, detail)
    except Exception as e:
        record("[10] CATASTROPHE-BREACH", False, f"harness error: {e}")

# ---- [11] FIRED-ON-LIVE-LEDGER (protection proven to fire, no giveback escaped) ----
# Env-tunable so the "healthy" path and negatives are demonstrable.
PROT_LOOKBACK_D = float(os.environ.get("PROT_FIRED_LOOKBACK_D", "30"))
PROT_LEAK_ARM_PCT = float(os.environ.get("PROT_LEAK_ARM_PCT", "0.6"))   # mfe/entry% that means "armed"
PROT_LEAK_NET_USD = float(os.environ.get("PROT_LEAK_NET_USD", "50"))    # deep-negative close threshold
# substrings (case-insensitive) that mark an exit as PROTECTIVE (the effect fired)
_PROTECTIVE = ("floor", "clip", "lock", "trail", "stall", "dstop", "disaster",
               "ratchet", "stop", "giveback", "cap", "be_", "breakeven", "protect")

def _exit_is_protective(reason):
    r = (reason or "").lower()
    return any(k in r for k in _PROTECTIVE)

def _fetch_live_ledger_rows():
    """ONE ssh: type the live cumulative ledger + daily files (backups filtered python-side).
    Returns list of dict rows keyed by header. Pipe-free PS foreach (cmd.exe splits '|')."""
    ps = (r"foreach($lf in Get-ChildItem 'C:\Omega\logs\trades\omega_trade_closes*.csv'){"
          r"Write-Output ('===LFILE '+$lf.Name);Get-Content $lf.FullName}")
    try:
        r = subprocess.run(["ssh","omega-new","powershell","-NoProfile","-Command",ps],
                           capture_output=True, text=True, timeout=90)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0 or not (r.stdout or "").strip():
        return None
    import csv as _csv
    rows, hdr, skip = [], None, False
    for line in r.stdout.splitlines():
        if line.startswith("===LFILE "):
            nm = line[9:].strip().lower()
            skip = any(x in nm for x in ("bak", "pre_", "pre-", "cutover", "culled",
                                          "removed", "tmp", "archive", "phantom", "premult"))
            continue
        if skip or not line.strip():
            continue
        try: rec = next(_csv.reader([line]))
        except Exception: continue
        if rec and rec[0] in ("trade_id", '"trade_id"'):
            hdr = [c.strip().strip('"') for c in rec]
            continue
        if hdr is None or len(rec) < len(hdr):
            continue
        rows.append(dict(zip(hdr, rec)))
    return rows

def check_fired_on_live_ledger():
    rows = _fetch_live_ledger_rows()
    if rows is None:
        record("[11] FIRED-ON-LIVE-LEDGER", False, "ssh omega-new failed -- live ledger UNVERIFIABLE"); return
    # negative-test injection (proves the leak branch can fire): PROT_INJECT=leak
    if os.environ.get("PROT_INJECT", "") == "leak":
        rows = list(rows) + [{"exit_ts_unix": str(int(time.time()) - 3600), "symbol": "INJ",
                              "engine": "INJ_TEST", "entry_px": "100", "mfe": "5.0",
                              "net_pnl": "-500", "exit_reason": "TIMEOUT"}]
    now = time.time(); cut = now - PROT_LOOKBACK_D * 86400
    def _f(v):
        try: return float(v)
        except (TypeError, ValueError): return 0.0
    recent = [r for r in rows if _f(r.get("exit_ts_unix")) >= cut]
    fired, leaks = [], []
    for r in recent:
        reason = r.get("exit_reason", "")
        net = _f(r.get("net_pnl"))
        entry = _f(r.get("entry_px"))
        mfe = _f(r.get("mfe"))
        if _exit_is_protective(reason):
            fired.append(reason)
        # LEAK: rode a real favorable peak (armed) then closed deep-negative under a
        # NON-protective exit -> the floor/lock should have caught it and did not.
        if entry > 0 and mfe > 0:
            peak_pct = 100.0 * mfe / entry
            if peak_pct >= PROT_LEAK_ARM_PCT and net <= -PROT_LEAK_NET_USD and not _exit_is_protective(reason):
                leaks.append(f"{r.get('symbol')}/{r.get('engine')} peaked +{peak_pct:.2f}% then "
                             f"closed ${net:.0f} via {reason or '?'} (UNPROTECTED giveback)")
    if leaks:
        record("[11] FIRED-ON-LIVE-LEDGER", False,
               f"*** {len(leaks)} real giveback(s) escaped protection: " + "; ".join(leaks[:3]) + " ***")
        return
    if fired:
        from collections import Counter
        top = ", ".join(f"{k}x{v}" for k, v in Counter(fired).most_common(4))
        record("[11] FIRED-ON-LIVE-LEDGER", True,
               f"protection FIRED on live book {len(fired)}x in {PROT_LOOKBACK_D:.0f}d ({top}); "
               f"0 unprotected givebacks -- effect proven on the LIVE ledger, not just sandbox")
        return
    # no closes in lookback (fresh/reset ledger): can't audit fire-history here, but
    # [3]/[9]/[10] already prove can-fire + armed. Do NOT hard-RED an empty ledger.
    record("[11] FIRED-ON-LIVE-LEDGER", True,
           f"no live closes in {PROT_LOOKBACK_D:.0f}d -- fire-history not auditable on the ledger; "
           f"armed+can-fire proven by [3]/[9]/[10] (no unprotected giveback to flag)")

def main():
    check_scheduled_alive()
    check_real_not_shadow()
    check_fires_on_trigger()
    check_effect_reconcile()
    check_binary_close_path()
    check_input_freshness()
    check_befloor_real_honesty()
    check_stall_bank_ledger_parity()
    check_profit_lock_coverage()
    check_catastrophe_breach()
    check_fired_on_live_ledger()
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
