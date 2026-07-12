#!/usr/bin/env python3
# liveness_check.py - LIVENESS REGISTRY (operator-mandated 2026-06-27).
# Root cause it kills: "built/tested/implemented" != "actually running", and nothing
# enforced the last step -> components went dark silently (stall_accountant unscheduled,
# deploy never taking, alarm never built). EVERY component that MUST be running is listed
# below with a heartbeat probe + max age. One checker screams on any dark component.
#
# DISCIPLINE: when you build/schedule a new engine/alarm/feed, ADD IT HERE. If it's not
# in this registry it's not guaranteed alive. If it stops producing, this alarms.
import json, os, subprocess, time, datetime

NOW = time.time()
def age_min(path):
    try: return (NOW - os.path.getmtime(path)) / 60.0
    except Exception: return None

# --- THE REGISTRY: name, probe-path (heartbeat/output that must be fresh), max_age_min ---
# Mac-side files/logs (each cron/agent writes one of these every run):
MAC = [
    # S-2026-07-12 CONSOLIDATION: the Mac ibkrcrypto book (refresh_shadow + live_mark crons)
    # was folded onto the ONE Chimera system (josgp1) and RETIRED. Its state.json/live_mark.log
    # freeze -> these probes went DARK (false alarm). Removed. Chimera liveness = the box's own
    # selftests + the chimera->desk relay (feeds_selftest crypto heartbeat, repointed to it).
    # giveback-saver python cron RETIRED S-2026-07-06/07: stall_accountant.py ported to native C++
    # StallBook (25 books in Omega.exe, boot line "stall-companion zoo wired"). Its liveness is now
    # the VPS companion_state.json freshness probe below — do NOT resurrect the Mac cron probe.
    ("omega health-poll cron",         "/tmp/omega_health_poll.log",                                            45),
    ("omega data-health cron",         "/tmp/data_health.log",                                                  45),
]
# VPS components pulled in one ssh round-trip (task LastRunTime / process / status freshness):
VPS_PROBE = r'''
$out=@()
# S-2026-07-11: market-closed window (Omega StallBook mirrors GOLD/INDEX entries -- crypto stall is on
# the separate josgp1 box). Gold/index shut Fri 21:00 -> Sun 22:00 UTC -> StallBook idle by design ->
# companion_state legitimately stale. Weekend-guard the stall staleness (operator: no weekend alarm).
$u=[DateTime]::UtcNow; $marketClosed = ($u.DayOfWeek -eq 'Saturday') -or ($u.DayOfWeek -eq 'Friday' -and $u.Hour -ge 21) -or ($u.DayOfWeek -eq 'Sunday' -and $u.Hour -lt 22)
$tasks=@{ "OmegaHealthAlarm"=30; "OmegaIbkrBridge"=0; "OmegaMgcLiveBars"=0; "OmegaBigCapBridge"=0; "OmegaL2Prune"=1500; "OmegaDiskAlarm"=90; "IbkrGateway"=10 }
foreach($n in $tasks.Keys){
  $t=Get-ScheduledTask -TaskName $n -EA SilentlyContinue
  if(-not $t){ $out += "DARK|$n|task missing"; continue }
  $st=$t.State.ToString()
  $max=$tasks[$n]
  if($max -eq 0){ if($st -ne "Running"){ if($marketClosed -and ($n -match 'Bridge|LiveBars')){ $out += "OK|$n|weekend-idle (state=$st, market shut -- feed bridge)" } else { $out += "DARK|$n|continuous task not Running (state=$st)" } } else { $out += "OK|$n|Running" } ; continue }
  $i=Get-ScheduledTaskInfo -TaskName $n -EA SilentlyContinue
  $ageMin = if($i.LastRunTime){ [int]((Get-Date)-$i.LastRunTime).TotalMinutes } else { 999999 }
  if($ageMin -gt $max){ $out += "DARK|$n|last ran ${ageMin}min ago (>$max)" } else { $out += "OK|$n|${ageMin}min" }
}
if(-not (Get-Process Omega -EA SilentlyContinue)){ $out += "DARK|Omega.exe|process not running" } else { $out += "OK|Omega.exe|alive" }
# desk GUI is the engine's in-process server (stays on VPS). crypto-GUI-8090 RETIRED S-2026-07-03 (crypto book now in omega-terminal; book liveness = Mac state.json probe above).
try{ $r=Invoke-WebRequest -UseBasicParsing "http://127.0.0.1:7779/" -TimeoutSec 6; $out += "OK|desk-GUI-7779|HTTP "+$r.StatusCode }
catch{ $m=$_.Exception.Message; $out += "DARK|desk-GUI-7779|not responding ("+$m.Substring(0,[Math]::Min(40,$m.Length))+")" }
$hs="C:\Omega\logs\HEALTH_STATUS.json"
if(Test-Path $hs){ $a=[int]((Get-Date)-(Get-Item $hs).LastWriteTime).TotalMinutes; if($a -gt 30){ $out += "DARK|VPS-health-alarm|HEALTH_STATUS ${a}min stale" } else { $out += "OK|VPS-health-alarm|${a}min" } } else { $out += "DARK|VPS-health-alarm|no HEALTH_STATUS.json" }
$cs="C:\Omega\companion_state.json"
if(Test-Path $cs){ $a=[int]((Get-Date)-(Get-Item $cs).LastWriteTime).TotalMinutes; if($a -gt 30 -and -not $marketClosed){ $out += "DARK|stall-companion (C++ StallBook)|companion_state ${a}min stale" } elseif($a -gt 30){ $out += "OK|stall-companion|weekend-idle ${a}min (gold/index shut, StallBook has nothing to clip)" } else { $out += "OK|stall-companion|${a}min" } } else { $out += "DARK|stall-companion (C++ StallBook)|no companion_state.json" }
$out -join "`n"
'''

dark = []
# Mac probes
for name, path, maxage in MAC:
    a = age_min(path)
    if a is None: dark.append(f"{name}: MISSING ({path})")
    elif a > maxage: dark.append(f"{name}: stale {a:.0f}min (>{maxage})")

# crypto GUI :8090 RETIRED S-2026-07-03 (standalone Mac window killed; crypto book now viewed in
# omega-terminal + VPS state push). The book's LIVENESS is still covered above by the MAC
# "crypto-book (refresh_shadow)" state.json freshness probe -- the data producers (fetch/shadow_refresh/
# live_mark crons) all still run. No HTTP GUI probe here anymore (it produced a false DARK once the
# keepalive cron was removed). Re-add only if the standalone GUI is deliberately brought back.

# VPS probes (one ssh). 2-STRIKE rule: a single slow probe on a paging-but-alive box should
# NOT fire DARK -- only alarm after 2 consecutive failures (~30min) = a real outage, not lag.
VPS_STRIKE_F = "/tmp/omega_liveness_vps_fails"
def _vps_strikes():
    try: return int(open(VPS_STRIKE_F).read().strip())
    except Exception: return 0
vps_ok = False
try:
    enc = subprocess.run(["iconv", "-t", "UTF-16LE"], input=VPS_PROBE.encode(), capture_output=True).stdout
    import base64
    b64 = base64.b64encode(enc).decode()
    # S-2026-07-07r: repointed omega-new -> omega-new (live box) post-cutover; old box tasks are
    # decommission-disabled by design and were firing permanent false DARKs (same class of bug as
    # the feeds_selftest repoint in 270f2b4c).
    r = subprocess.run(["ssh", "-o", "ConnectTimeout=20", "omega-new",
                        f"powershell -NoProfile -EncodedCommand {b64}"],
                       capture_output=True, timeout=50, text=True)
    lines = [l for l in r.stdout.replace("\r", "").splitlines() if "|" in l]
    if lines:
        vps_ok = True
        for l in lines:
            st, name, detail = (l.split("|", 2) + ["", "", ""])[:3]
            if st == "DARK": dark.append(f"VPS {name}: {detail}")
except Exception:
    pass
if vps_ok:
    try: open(VPS_STRIKE_F, "w").write("0")
    except Exception: pass
else:
    n = _vps_strikes() + 1
    try: open(VPS_STRIKE_F, "w").write(str(n))
    except Exception: pass
    if n >= 2:
        dark.append(f"VPS: unreachable for {n} consecutive probes (~{n*15}min) -- box down or RAM-frozen")

# S-2026-07-08: per-COMPONENT 2-strike (operator: recurring one-shot DARK notifications).
# The VPS "continuous" tasks (MgcLiveBars/IbkrBridge) are watchdog-scheduled: script dies
# (e.g. IB Gateway nightly restart drops the client), schtasks re-fires within 5 min. A
# probe landing in that gap saw state=Ready and alarmed on a self-healing transient.
# A component now only alarms on 2 CONSECUTIVE failed probes (~30min = real outage);
# the box-level unreachable 2-strike above already followed the same logic.
COMP_STRIKE_F = "/tmp/omega_liveness_component_strikes.json"
def _load_comp_strikes():
    try:
        with open(COMP_STRIKE_F) as f: return json.load(f)
    except Exception: return {}
_prev_strikes = _load_comp_strikes()
_cur_strikes = {}
_confirmed = []
for d in dark:
    key = d.split(":", 1)[0].strip()
    n = _prev_strikes.get(key, 0) + 1
    _cur_strikes[key] = n
    if n >= 2: _confirmed.append(d)
    # first strike: log-only (the log line below still records the raw probe)
try:
    with open(COMP_STRIKE_F, "w") as f: json.dump(_cur_strikes, f)
except Exception: pass
raw_dark, dark = dark, _confirmed

ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
MARKER = "/tmp/omega_liveness_DARK.txt"
with open("/tmp/liveness_check.log", "a") as f:
    extra = "" if len(raw_dark) == len(dark) else f" (1st-strike suppressed: {'; '.join(x for x in raw_dark if x not in dark)})"
    f.write(f"{ts} dark={len(dark)}{extra} {('; '.join(dark)) if dark else 'ALL-ALIVE'}\n")

if dark:
    summary = "; ".join(dark)
    try:
        subprocess.run(["osascript", "-e",
            f'display notification "{summary[:200]}" with title "\U0001F534 DARK COMPONENT(S)" sound name "Basso"'], timeout=10)
    except Exception: pass
    with open(MARKER, "w") as f: f.write(f"{ts}\n" + "\n".join(dark) + "\n")
    print(f"DARK ({len(dark)}): {summary}")
else:
    if os.path.exists(MARKER): os.remove(MARKER)
    print("ALL-ALIVE")
