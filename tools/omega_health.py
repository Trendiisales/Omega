#!/usr/bin/env python3
"""
Omega unified health aggregator + LOUD surfacing.  v2 (comprehensive).

Covers EVERY critical piece of the trading stack, not just bigcap. The system
had watchdogs that only wrote to logs nobody tails, so failures (BIGCAP_STALE,
L2 loss, telemetry dead) ran for hours unseen. This aggregates all of it into
one health.json + a RED/AMBER/GREEN page on :7790, raises HEALTH_RED.flag and
appends health_alerts.log on every RED transition. A mac-side notifier polls
:7790 and fires a native notification on RED (the actual "loud" channel).

  python omega_health.py --once     # single check, print + write health.json
  python omega_health.py            # loop every 45s + serve :7790
"""
from __future__ import annotations
import json, os, re, time, socket, subprocess, threading, datetime, glob
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

OMEGA   = Path(r"C:\Omega")
LOGS    = OMEGA / "logs"
LATEST  = LOGS / "latest.log"
BCBRIDGE= LOGS / "bigcap_bridge.log"
WATCHDOG= LOGS / "watchdog.log"
HEALTH  = LOGS / "health.json"
ALERTS  = LOGS / "health_alerts.log"
REDFLAG = OMEGA / "HEALTH_RED.flag"
PORT    = 7790

RED, AMBER, GREEN, INFO = "RED", "AMBER", "GREEN", "INFO"
RANK = {GREEN: 0, INFO: 0, AMBER: 1, RED: 2}

def _now(): return datetime.datetime.now(datetime.timezone.utc)

def _us_rth() -> bool:
    n=_now()
    if n.weekday()>=5: return False
    m=n.hour*60+n.minute
    return 13*60+30 <= m <= 20*60

def _fx_open() -> bool:
    # BlackBull forex/metals ~ Sun 21:00 UTC -> Fri 21:00 UTC continuous
    n=_now(); wd=n.weekday(); h=n.hour
    if wd==5: return False                      # Sat closed
    if wd==6 and h<21: return False             # Sun before open
    if wd==4 and h>=21: return False            # Fri after close
    return True

def _nq_globex_open() -> bool:
    # NQ/MGC futures (CME Globex) tape -- the source aurora_NQ.json is built from.
    # Trades ~23h/day Sun->Fri with a daily ~1h maintenance break, NOT just NYSE
    # RTH. aurora's two live consumers (AuroraGate, crypto NDX mark) want a fresh
    # mark whenever this tape is live; gating freshness on _us_rth() blinds the
    # monitor for the ~17h/day the futures trade outside RTH -- the exact
    # silent-stale failure mode of 2026-06-28. Daily maintenance break ~21:00-22:00
    # UTC (5-6pm ET); weekend closed Fri 21:00 UTC -> Sun 22:00 UTC. (Fixed UTC
    # offsets, EDT-anchored like _fx_open; +/-1h slop at the break edge in winter
    # is acceptable -- it errs toward alarming, never toward a silent stale.)
    n=_now(); wd=n.weekday(); h=n.hour
    if wd==5: return False                      # Sat closed
    if wd==4 and h>=21: return False            # Fri after 21:00 UTC close
    if wd==6 and h<22: return False             # Sun before 22:00 UTC open
    if 21<=h<22: return False                   # daily maintenance break
    return True

def _tasks() -> dict:
    out=subprocess.run('schtasks /query /fo CSV /nh', capture_output=True, text=True, shell=True).stdout
    st={}
    for ln in out.splitlines():
        p=ln.split('","')
        if len(p)>=3:
            st[p[0].strip('"').lstrip('\\')]=p[2].strip('"')
    return st

def _proc(needle: str) -> bool:
    out=subprocess.run('tasklist', capture_output=True, text=True, shell=True).stdout
    return needle.lower() in out.lower()

def _port(p: int, host="127.0.0.1") -> bool:
    s=socket.socket(); s.settimeout(3)
    try: s.connect((host,p)); return True
    except Exception: return False
    finally: s.close()

def _tail(path: Path, n=4000) -> list[str]:
    try:
        with open(path,"rb") as f:
            f.seek(0,2); sz=f.tell(); f.seek(max(0,sz-600_000))
            return f.read().decode("utf-8","replace").splitlines()[-n:]
    except Exception: return []

def _age(path: Path):
    try: return time.time()-path.stat().st_mtime
    except Exception: return None

# ---------------- checks ----------------

def chk_omega_proc():
    return (GREEN,"Omega.exe running") if _proc("Omega.exe") else (RED,"Omega.exe DOWN -- engine not running")

def chk_log_fresh():
    a=_age(LATEST)
    if a is None: return (RED,"latest.log missing")
    if a>180: return (RED,f"latest.log stale {a:.0f}s -- engine frozen")
    if a>90:  return (AMBER,f"latest.log {a:.0f}s old")
    return (GREEN,f"fresh {a:.0f}s")

def chk_gateway():
    # Key on the API LISTENER, not a process name. Post-2026-07-07 migration the ForexVPS box
    # runs IB Gateway as java.exe (install4j i4j_jres JRE) -- there is NO ibgateway.exe process,
    # so the old _proc("ibgateway.exe") check returned RED on EVERY run even with 4002 live and
    # feeds fresh. That false RED wrote HEALTH_RED.flag; when this daemon then died (15min task
    # ExecutionTimeLimit) the flag orphaned and the GUI/alarm showed RED forever (2026-07-15
    # recurring HEALTH RED root cause). The listener is the correct, migration-proof signal and
    # matches omega_health_alarm.ps1 ($gwUp=listener) + healthcheck.ps1 ib.port_4002. Operator
    # mandate 2026-07-01: "alarm on the LISTENER, not the process."
    listen=_port(4002)
    if listen: return (GREEN,":4002 listening -- IBKR API up")
    if not _fx_open(): return (AMBER,":4002 down -- broker closed (weekend), expected")
    return (RED,":4002 NOT listening -- IBKR API down, all IBKR feeds blind")

def chk_fix_feed():
    last=None
    for ln in reversed(_tail(LATEST,5000)):
        if "FIX-IN-RAW-TRADE" in ln or "[FIX-" in ln:
            m=re.match(r"(\d\d):(\d\d):(\d\d)",ln)
            if m: last=ln; break
    if not _fx_open():
        return (INFO,"fx/metals closed (weekend)")
    if not last:
        return (RED,"no FIX heartbeat in tail -- gold/FX feed DOWN")
    # crude age from HH:MM:SS vs now UTC
    hh,mm,ss=int(last[0:2]),int(last[3:5]),int(last[6:8])
    n=_now(); age=(n.hour*3600+n.minute*60+n.second)-(hh*3600+mm*60+ss)
    if age<0: age+=86400
    if age>300: return (RED,f"FIX stale {age}s -- gold/FX feed stalled")
    return (GREEN,f"FIX live ({age}s)")

def chk_bigcap_data():
    hb=None
    for ln in reversed(_tail(BCBRIDGE,50)):
        if "[HB" in ln and "mdtype=" in ln: hb=ln.strip(); break
    md=re.search(r"mdtype=(\d)",hb or ""); trd=re.search(r"tradeable=(\S+)",hb or "")
    rth=_us_rth()
    if not hb: return (AMBER,"no bigcap heartbeat")
    mdt=md.group(1) if md else "?"; trv=trd.group(1) if trd else "?"
    if mdt=="1" and trv not in ("nan","0"): return (GREEN,f"RT data, tradeable={trv}")
    if mdt=="1": return (GREEN if not rth else AMBER, f"RT, tradeable={trv}")
    if not rth: return (INFO,f"mkt closed; mdtype={mdt} tradeable={trv}")
    if mdt=="3": return (RED,"DELAYED data (mdtype=3) in RTH -- engine cannot trade")
    return (RED,f"bigcap unhealthy: {hb[-60:]}")

def chk_rec_xau():
    t=_tasks().get("OmegaIbkrBridge","?")
    return (GREEN,"recording") if t=="Running" else (RED,f"OmegaIbkrBridge={t} -- XAU/MGC recording GAP (P0)")

def chk_rec_mgc():
    t=_tasks().get("OmegaMgcLiveBars","?")
    files=glob.glob(str(LOGS/"ibkr_l2"/"ibkr_l2_MGC_*.csv"))
    fresh=max((time.time()-os.path.getmtime(f) for f in files), default=9e9) if files else 9e9
    fresh=min((time.time()-os.path.getmtime(f) for f in files), default=9e9) if files else 9e9
    if t!="Running": return (RED,f"OmegaMgcLiveBars={t} -- MGC bars recording GAP (P0)")
    if _fx_open() and fresh>600: return (AMBER,f"MGC csv stale {fresh:.0f}s")
    return (GREEN,"recording")

def chk_dom_bridge():
    if _port(9701): return (GREEN,":9701 up")
    if not _fx_open(): return (INFO,":9701 down -- fx/metals/futures closed (weekend), expected")
    return (RED,"DOM bridge :9701 down (gold/MGC/NAS L2)")

def chk_dashboard():
    return (GREEN,":7779 up") if _port(7779) else (AMBER,"main dashboard :7779 down")

def chk_scanner():
    if _port(7783): return (GREEN,":7783 up")
    if not _us_rth(): return (INFO,":7783 down -- US mkt closed (weekend/after-hours), expected")
    return (AMBER,"bigcap scanner :7783 down")

def chk_l2():
    for ln in reversed(_tail(WATCHDOG,60)):
        if "L2-CSV-STALE" in ln:
            if not _fx_open(): return (INFO,"gold L2 quiet -- fx/metals closed (weekend), expected")
            return (AMBER,"L2 CSV stale (gold L2 degraded; gold still on FIX)")
        if "HEARTBEAT" in ln: break
    return (GREEN,"L2 ok")

def chk_telemetry():
    for ln in reversed(_tail(WATCHDOG,30)):
        m=re.search(r"telemetry_healthy=(\w+)",ln)
        if m: return (GREEN,"healthy") if m.group(1)=="True" else (AMBER,"telemetry_healthy=False")
    return (AMBER,"no telemetry status")

def chk_disk():
    try:
        free=subprocess.run('powershell -NoProfile -Command "[math]::Round((Get-PSDrive C).Free/1GB,2)"',
                            capture_output=True,text=True,shell=True).stdout.strip()
        g=float(free)
    except Exception: return (AMBER,"disk free unknown")
    if g<1.5: return (RED,f"DISK {g}GB free -- critical, recordings will fail")
    if g<3.0: return (AMBER,f"disk {g}GB free -- low")
    return (GREEN,f"{g}GB free")

def chk_log_bloat():
    big=[(os.path.getsize(f),f) for f in glob.glob(str(LOGS/"*.log"))]
    big=[(s,f) for s,f in big if s>400_000_000]
    if big:
        s,f=max(big); return (AMBER,f"{os.path.basename(f)} {s/1e6:.0f}MB -- rotate")
    return (GREEN,"logs sane")

STDOUT=LOGS/"omega_service_stdout.log"
def chk_ibkr_engine():
    # IBKR in-process engine (BigCapMomo): parse its ALIVE heartbeat from stdout.log so a
    # silent "connected but never scans/trades" failure can't sit unnoticed (S-2026-06-23).
    if (_age(STDOUT) or 9e9) > 300:
        return (AMBER, "stdout.log stale -- engine not heartbeating")
    alive=None
    for ln in reversed(_tail(STDOUT,300)):
        if "BIGCAP_IBKR_DOWN" in ln or "[BigCapMomo] DOWN" in ln:
            return (RED, "IBKR engine DOWN (no API handshake)")
        if "[BigCapMomo] ALIVE" in ln: alive=ln; break
    if not alive:
        return (INFO, "no IBKR ALIVE line in tail (engine may be off)")
    m=re.search(r"scan_hits=(\d+).*last_data=(\S+)", alive)
    sh=int(m.group(1)) if m else 0; ld=(m.group(2) if m else "?")
    if not _us_rth():
        return (INFO, f"mkt closed; scan_hits={sh}")
    if sh==0 or ld=="NEVER":
        return (AMBER, f"RTH but scan_hits={sh} last_data={ld} -- scanner/data gap")
    return (GREEN, f"scanning, scan_hits={sh}")

def chk_xs_index():
    # Cross-sectional bear sleeve (XsIndex MrLS): regime-gated -> DORMANT in bull (logs only on
    # trades, so silence in bull is CORRECT, not a fault). Can't verify bear-activity until a
    # regime turn. So check it LOADED + warm-seeded (catches the real failure: dropped from build
    # / didn't init). S-2026-06-23.
    # FIX 2026-06-27: the boot line + [SEED] lines print ONCE at startup. A blind tail scrolls them
    # out on a long-running process -> false AMBER. Anchor to the LAST boot (RUNNING COMMIT); if the
    # boot marker itself has scrolled out, the engine booted earlier and is fine (not a fault).
    lines=_tail(STDOUT,30000)
    boot_idx=[i for i,ln in enumerate(lines) if "RUNNING COMMIT" in ln]
    if not boot_idx:
        return (INFO,"boot block scrolled out of log -- booted earlier, cannot re-verify (not a fault)")
    blk=lines[boot_idx[-1]:]
    booted=any("CrossSectionalIndex x3" in ln for ln in blk)
    seeded=len({ln.split("XsIndex_MrLS-")[1].split("]")[0] for ln in blk if "[SEED][XsIndex_MrLS-" in ln})
    if not booted:
        return (AMBER,"no CrossSectionalIndex boot line -- bear sleeve not loaded?")
    if seeded<3:
        return (AMBER,f"loaded but only {seeded}/5 MrLS legs warm-seeded")
    return (GREEN,f"loaded, {seeded}/5 legs seeded (gated -- dormant in bull by design)")

AURORA=LOGS/"aurora"/"aurora_NQ.json"
def chk_aurora():
    # Aurora order-flow snapshot freshness. aurora_NQ.json feeds TWO LIVE trading
    # consumers -- AuroraGate.hpp (gold ORB + index entry gate, NQ proxies
    # NAS100/US500) and the Crypto NDX live mark (shadow_refresh.cpp read_live_nq).
    # OmegaAuroraSnapshot rewrites it every 60s (--once periodic). If it stalls
    # while the NQ Globex tape is live (~23h/day, not just US RTH) the gate goes
    # fail-open AND the crypto NDX mark silently falls back to the stale daily
    # close. Gate on _nq_globex_open() not _us_rth() -- the snapshotter is meant
    # to run whenever the futures tape ticks; an RTH-only gate was the blind spot
    # that let it run stale ~4 days outside RTH with no alarm (2026-06-28 leak).
    # History (S-2026-06-30): the task was disabled 2026-06-28 to stop a 257MB
    # duplicate-loop leak and ran stale ~4 days with NO alarm -- because this exact
    # check had been removed at the same time. Re-added + leak root-caused (task now
    # runs --once periodically, cannot orphan/pileup). mtime tracks "is the
    # snapshotter alive" independent of the separate NQ depth-freeze issue.
    a=_age(AURORA)
    if a is None:
        if not _nq_globex_open(): return (INFO,"aurora_NQ.json missing (NQ tape closed/maintenance break)")
        return (RED,"aurora_NQ.json MISSING -- AuroraGate blind + crypto NDX on daily-close")
    if not _nq_globex_open(): return (INFO,f"aurora {a/60:.0f}min old (NQ tape closed/maintenance break)")
    if a>600: return (RED,f"aurora STALE {a/60:.0f}min -- snapshotter dead; gate fail-open + NDX daily-close fallback")
    if a>240: return (AMBER,f"aurora {a/60:.0f}min old -- snapshotter lagging")
    return (GREEN,f"aurora fresh {a:.0f}s")

# Neither Aurora NOR OmegaHealthMonitor is in CRIT_TASKS: both are periodic --once
# tasks whose State is "Ready" between runs (not "Running"), so a task-state check
# false-REDs them. OmegaHealthMonitor was moved to the --once pattern 2026-07-15 (it
# was a resident loop that a 15min ExecutionTimeLimit kept killing) and self-checking
# its own task-state is doubly wrong -- if it were dead it could not run to report, and
# as --once it is "Ready" not "Running". Its real liveness backstop is the HEALTH_RED.flag
# heartbeat + omega_health_alarm.ps1 staleness guard. A task's OUTPUT freshness (e.g.
# chk_aurora) is the correct liveness signal; do not re-add a --once task here.
CRIT_TASKS=["OmegaBigCapBridge","OmegaIbkrBridge","OmegaMgcLiveBars"]
def chk_crit_tasks():
    st=_tasks(); bad=[t for t in CRIT_TASKS if st.get(t)!="Running"]
    if bad: return (RED,"NOT running: "+", ".join(f"{t}={st.get(t,'missing')}" for t in bad))
    return (GREEN,"all critical tasks Running")

CHECKS=[
    ("omega_proc",chk_omega_proc), ("log_fresh",chk_log_fresh),
    ("ibkr_gateway",chk_gateway), ("fix_feed",chk_fix_feed),
    ("bigcap_data",chk_bigcap_data), ("ibkr_engine",chk_ibkr_engine), ("xs_index",chk_xs_index), ("rec_xau",chk_rec_xau), ("rec_mgc",chk_rec_mgc),
    ("dom_bridge",chk_dom_bridge), ("dashboard",chk_dashboard), ("scanner",chk_scanner),
    ("l2_feed",chk_l2), ("aurora",chk_aurora), ("telemetry",chk_telemetry),
    ("disk",chk_disk), ("log_bloat",chk_log_bloat), ("crit_tasks",chk_crit_tasks),
]

def collect():
    comps=[]
    for name,fn in CHECKS:
        try: st,detail=fn()
        except Exception as e: st,detail=AMBER,f"check error: {e}"
        comps.append({"name":name,"status":st,"detail":detail})
    overall=max((c["status"] for c in comps), key=lambda s:RANK.get(s,0))
    # Maintenance suppression: a deploy (service restart) or the 00:00 UTC broker/data
    # rollover both trip transient REDs (fix_feed/dashboard/bigcap/crit_tasks). Downgrade
    # those to AMBER so the mac notifier doesn't cry wolf on PLANNED maintenance. A real
    # RED (e.g. disk) during the window still shows AMBER on the page + in the log.
    n=_now(); note=""
    deploy = os.path.exists(str(OMEGA/"deploy_in_progress.flag"))
    rollover = (n.hour==0 and n.minute<6)
    if overall==RED and (deploy or rollover):
        overall=AMBER; note=" (RED suppressed: %s)" % ("deploy in progress" if deploy else "00:00 rollover")
    return {"ts":_now().isoformat(timespec="seconds"),"overall":overall,"maint":note,
            "rth":_us_rth(),"fx":_fx_open(),"components":comps}

def _flag_msg(r):
    return "\n".join([f"{r['ts']} OVERALL={r['overall']}"]+[
        f"  [{c['status']}] {c['name']}: {c['detail']}" for c in r["components"] if c["status"] in (RED,AMBER)])

def send_alert(r):
    msg=_flag_msg(r)
    with open(ALERTS,"a") as f: f.write(msg+"\n")
    REDFLAG.write_text(msg)
    # push channel (ntfy/telegram) hooks here once operator picks one

def write_and_alert(r):
    prev={}
    try: prev=json.loads(HEALTH.read_text())
    except Exception: pass
    HEALTH.write_text(json.dumps(r,indent=2))
    if r["overall"]==RED:
        # Refresh HEALTH_RED.flag EVERY loop while RED (heartbeat), so its mtime tracks this
        # daemon's liveness. If the daemon dies mid-RED the flag goes stale and the alarm
        # (omega_health_alarm.ps1) treats a stale flag as an orphan and clears it -- self-heals
        # instead of a phantom RED forever. Log/push still fire ONLY on transition (no spam).
        if prev.get("overall")!=RED: send_alert(r)
        else:
            try: REDFLAG.write_text(_flag_msg(r))
            except Exception: pass
    elif REDFLAG.exists():
        try: REDFLAG.unlink()
        except Exception: pass

PAGE="""<!doctype html><meta http-equiv=refresh content=15><title>OMEGA HEALTH</title>
<style>body{{background:#0b0e11;color:#cdd6dd;font:13px ui-monospace,Menlo,monospace;padding:22px}}
h1{{font-size:18px;letter-spacing:3px}}.RED{{color:#ff5252;font-weight:700}}.AMBER{{color:#ffb300}}
.GREEN{{color:#34d399}}.INFO{{color:#7a8a99}}.big{{font-size:38px;margin:6px 0 16px}}
table{{border-collapse:collapse;width:100%;max-width:880px}}td{{padding:5px 10px;border-bottom:1px solid #1c242c}}</style>
<h1>OMEGA HEALTH</h1><div class=big><span class="{ov}">{ov}</span></div>
<div class=INFO>{ts} · US-RTH={rth} FX={fx}</div><table>{rows}</table>"""

class H(BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_GET(self):
        if self.path.rstrip("/") in ("/health.json","/health"):
            body=HEALTH.read_bytes() if HEALTH.exists() else b"{}"; ct="application/json"
        else:
            try: r=json.loads(HEALTH.read_text())
            except Exception: r=collect()
            rows="".join(f"<tr><td class={c['status']}>{c['status']}</td><td>{c['name']}</td><td>{c['detail']}</td></tr>"
                         for c in r.get("components",[]))
            body=PAGE.format(ov=r.get("overall","?"),ts=r.get("ts",""),rth=r.get("rth",""),
                             fx=r.get("fx",""),rows=rows).encode(); ct="text/html"
        self.send_response(200); self.send_header("Content-Type",ct)
        self.send_header("Cache-Control","no-store"); self.send_header("Content-Length",str(len(body)))
        self.end_headers(); self.wfile.write(body)

def main():
    import sys
    if "--once" in sys.argv:
        r=collect(); write_and_alert(r); print(json.dumps(r,indent=2)); return
    threading.Thread(target=lambda:ThreadingHTTPServer(("0.0.0.0",PORT),H).serve_forever(),daemon=True).start()
    while True:
        try: write_and_alert(collect())
        except Exception as e: print("loop err",e)
        time.sleep(45)

if __name__=="__main__": main()
