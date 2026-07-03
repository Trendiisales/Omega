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
    # S-2026-07-02: ~/Crypto after the 2026-07-01 consolidation (old path kept as runtime fallback below)
    ("crypto-book (refresh_shadow)",   os.path.expanduser("~/Crypto/backtest/data/ibkrcrypto/state.json")
        if os.path.exists(os.path.expanduser("~/Crypto/backtest/data/ibkrcrypto/state.json"))
        else os.path.expanduser("~/IBKRCrypto/backtest/data/ibkrcrypto/state.json"),               90),
    ("live-mark cron (intraday px)",   "/tmp/live_mark.log",                                                    20),
    ("giveback-saver cron",            "/tmp/giveback_saver.log",                                               20),
    ("crypto staleness-alarm cron",    "/tmp/crypto_staleness_alarm.log",                                       90),
    ("omega health-poll cron",         "/tmp/omega_health_poll.log",                                            45),
    ("omega data-health cron",         "/tmp/data_health.log",                                                  45),
]
# VPS components pulled in one ssh round-trip (task LastRunTime / process / status freshness):
VPS_PROBE = r'''
$out=@()
$tasks=@{ "OmegaHealthAlarm"=30; "OmegaIbkrBridge"=0; "OmegaMgcLiveBars"=0; "OmegaBigCapBridge"=0; "OmegaL2Prune"=1500; "OmegaDiskAlarm"=90; "IbkrGateway"=10 }
foreach($n in $tasks.Keys){
  $t=Get-ScheduledTask -TaskName $n -EA SilentlyContinue
  if(-not $t){ $out += "DARK|$n|task missing"; continue }
  $st=$t.State.ToString()
  $max=$tasks[$n]
  if($max -eq 0){ if($st -ne "Running"){ $out += "DARK|$n|continuous task not Running (state=$st)" } else { $out += "OK|$n|Running" } ; continue }
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
    r = subprocess.run(["ssh", "-o", "ConnectTimeout=20", "omega-vps",
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

ts = datetime.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")
MARKER = "/tmp/omega_liveness_DARK.txt"
with open("/tmp/liveness_check.log", "a") as f:
    f.write(f"{ts} dark={len(dark)} {('; '.join(dark)) if dark else 'ALL-ALIVE'}\n")

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
