#!/usr/bin/env python3
"""Omega log rotation/cleanup. Run once now + scheduled daily (OmegaLogRotate).
Deletes stale logs; truncates oversized logs (bouncing the owner task for held-open ones)."""
import os, glob, subprocess, time, re
LOGS=r"C:\Omega\logs"
def free_gb(): 
    return round(int(subprocess.run('wmic logicaldisk where "DeviceID=\'C:\'" get FreeSpace',shell=True,capture_output=True,text=True).stdout.split()[1])/1e9,2)
b4=free_gb(); freed=0
# 1. delete clearly-stale files (NOT today's, NOT the live ones)
today=time.strftime("%Y%m%d")
stale_pat=["deploy_2026*.log","deploy?.log","deploy.log","watchdog-2026*.log","omega_2026-*.log","*.bak","*.broken*","*.old"]
keep_live={"latest.log","watchdog.log","omega_service_stdout.log","omega_service_stderr.log","bigcap_bridge.log","health_alerts.log"}
for pat in stale_pat:
    for f in glob.glob(os.path.join(LOGS,pat)):
        bn=os.path.basename(f)
        if bn in keep_live: continue
        # keep files modified in last 2 days (avoid nuking an in-flight deploy log)
        if time.time()-os.path.getmtime(f) < 2*86400: continue
        try: sz=os.path.getsize(f); os.remove(f); freed+=sz; print(f"  del {bn} ({sz/1e6:.1f}MB)")
        except Exception as e: print(f"  skip {bn}: {e}")
# 2. rotate oversized HELD-OPEN logs (bounce owner). bigcap_bridge.log = OmegaBigCapBridge task.
CAP=40*1024*1024  # 40MB
bb=os.path.join(LOGS,"bigcap_bridge.log")
if os.path.exists(bb) and os.path.getsize(bb)>CAP:
    sz0=os.path.getsize(bb)
    print(f"  bigcap_bridge.log {sz0/1e6:.0f}MB > cap -> bounce+truncate")
    subprocess.run("schtasks /End /TN OmegaBigCapBridge",shell=True,capture_output=True)
    # kill stray pythonw running the bridge so the file handle releases
    out=subprocess.run('wmic process where "name=\'pythonw.exe\'" get ProcessId,CommandLine',shell=True,capture_output=True,text=True).stdout
    for ln in out.splitlines():
        if "bigcap_feed_bridge" in ln:
            m=re.search(r"(\d+)\s*$",ln.strip())
            if m: subprocess.run(f"taskkill /F /PID {m.group(1)}",shell=True,capture_output=True)
    time.sleep(2)
    try:
        with open(bb,encoding="utf-8",errors="replace") as fh: tail=fh.readlines()[-1500:]
        open(bb,"w",encoding="utf-8").writelines(tail); freed+=sz0-os.path.getsize(bb)
        print(f"  truncated -> {os.path.getsize(bb)/1e6:.1f}MB (kept 1500 lines)")
    except Exception as e: print(f"  truncate fail: {e}")
    subprocess.run("schtasks /Run /TN OmegaBigCapBridge",shell=True,capture_output=True)
    print("  bridge restarted")
print(f"# free {b4} -> {free_gb()} GB (reclaimed ~{freed/1e6:.0f}MB)")
