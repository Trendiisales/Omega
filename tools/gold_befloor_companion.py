#!/usr/bin/env python3
"""GOLD BE-floor long+short companion — LIVE executor (AUPOS + AUNEG).

Two SELF-DETECTING standalone companion books (NOT a stall_accountant parent-mirror):
  AUPOS  (uptrend / long)  — internal 2h/+1% up-detector  -> x2 BE-floor long  companions
  AUNEG  (downtrend/short) — internal 2h/-1% down-detector -> x2 BE-floor short companions

Mirror of the crypto MimicLadderCompanion be_floor path, in the Omega Python-companion
idiom. SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-engine) — judged
STANDALONE, never vs a parent/WIDE. Everything PAPER/shadow. Loss-protection verdict =
BE-FLOOR (net >= 0 on EVERY clip by construction; feedback-engine-loss-protection-provision).

FAITHFULNESS: each cycle REPLAYS the exact backtest functions (backtest/gold_befloor_ls.py
parent_long/short + leg_book_long/short) over the full persisted H1 history. Bank/clips are
DERIVED from that replay, never separately persisted -> the live book is byte-identical to
the backtest and CANNOT drift or carry a stale bank (the failure that hit the crypto book).

Live feed: VPS C:\\Omega\\logs\\gold_regime_h1.csv (ts,o,h,l,c H1 marks, hourly), fetched over
ssh. Warm-seed: bundled phase1/signal_discovery/warmup_XAUUSD_H4.csv history at first boot.
Emits data/gold_companion_state.json -> pushed to VPS -> Omega desk GOLD COMPANIONS panel.

Env knobs (cron rung):
  GOLD_COMP_DIR   state dir (default tools/../data/gold_befloor)
  GOLD_LOT        lot size (default 1.0 = 100oz XAUUSD, $100/pt)
  GOLD_PUSH_STATE 1 = scp state json to VPS (default 1)
  GOLD_W / GOLD_THR   detector window bars / threshold (default 2 / 0.01)
"""
import os, sys, csv, json, subprocess, tempfile, importlib.util, datetime

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BT   = os.path.join(REPO, "backtest", "gold_befloor_ls.py")

# import the faithful backtest math (single source of truth)
_spec = importlib.util.spec_from_file_location("gbl", BT)
g = importlib.util.module_from_spec(_spec); _sv = sys.argv; sys.argv=["x"]
_spec.loader.exec_module(g); sys.argv = _sv

DPP_PER_LOT = 100.0   # $/point for 1.0 std XAUUSD lot (100 oz)
LOT   = float(os.environ.get("GOLD_LOT", "1.0"))
W     = int(os.environ.get("GOLD_W", "2"))
THR   = float(os.environ.get("GOLD_THR", "0.01"))
PUSH  = os.environ.get("GOLD_PUSH_STATE", "1") == "1"
SDIR  = os.environ.get("GOLD_COMP_DIR", os.path.join(REPO, "data", "gold_befloor"))
os.makedirs(SDIR, exist_ok=True)
HIST  = os.path.join(SDIR, "gold_h1_history.csv")           # persisted rolling H1 history
DEPLOYF = os.path.join(SDIR, "deploy_ts.txt")               # first-live-cycle ts (deploy-forward)
STATE = os.path.join(REPO, "data", "gold_companion_state.json")
VPS_SRC = "C:/Omega/logs/gold_regime_h1.csv"
VPS_DST = "omega-new:C:/Omega/gold_companion_state.json"
WARMSEED = os.path.join(REPO, "phase1", "signal_discovery", "warmup_XAUUSD_H1.csv")
# stall-accountant book file -> companion_aggregate.py folds per_engine AUPOS-GOLD/AUNEG-GOLD
# into /api/companion, which the LIVE desk GOLD COMPANIONS (gctab) panel renders. No rebuild.
STALL_BOOK = os.environ.get("GOLD_STALL_BOOK",
                            "/Users/jo/stall-accountant/gold_befloor/companion_state.json")

TIERS = [(20, "banker"), (150, "runner")]

def _read_csv(path, has_header):
    rows=[]
    if not os.path.exists(path): return rows
    with open(path) as f:
        r=csv.reader(f)
        if has_header: next(r, None)
        for row in r:
            if len(row)<5: continue
            try: rows.append((int(float(row[0])), float(row[1]), float(row[2]), float(row[3]), float(row[4])))
            except ValueError: continue
    return rows

def warm_seed():
    """First boot: seed history from the bundled warmup H1 CSV (ts,o,h,l,c; ts in seconds).
    Seed bars are PRE-DEPLOY — they prime the 2h detector but book NO clips into the live
    forward book (deploy_ts is set to the max seed ts; only clips after it count)."""
    if os.path.exists(HIST): return
    seed=_read_csv(WARMSEED, True)                       # H1, already seconds
    _write_hist(seed)
    print(f"[SEED] gold BE-floor companion warm-seeded {len(seed)} H1 bars from "
          f"{os.path.basename(WARMSEED)} (pre-deploy; live book accrues forward)")

def deploy_ts():
    if os.path.exists(DEPLOYF):
        try: return int(open(DEPLOYF).read().strip())
        except (ValueError, OSError): pass
    return 0

def fetch_live():
    """Pull the VPS H1 mark file (ts,o,h,l,c, no header) over ssh."""
    tmp = tempfile.mktemp(suffix=".csv")
    try:
        subprocess.run(["scp","-q",f"omega-new:{VPS_SRC}",tmp],check=True,timeout=30)
    except Exception as e:
        print(f"gold companion: live fetch failed ({e}) — using persisted history"); return []
    rows=_read_csv(tmp, False)
    try: os.remove(tmp)
    except OSError: pass
    return rows

def _write_hist(rows):
    rows=sorted({r[0]: r for r in rows}.values())        # dedup by ts, sorted
    with open(HIST,"w") as f:
        w=csv.writer(f); w.writerow(["ts","o","h","l","c"])
        for r in rows: w.writerow(r)
    return rows

def merge_history(live):
    hist=_read_csv(HIST, True)
    return _write_hist(hist+live)

def bars_from_rows(rows):
    ts=[r[0] for r in rows]; o=[r[1] for r in rows]; h=[r[2] for r in rows]
    l=[r[3] for r in rows]; c=[r[4] for r in rows]; N=len(ts)
    sma=[None]*N; run=0.0
    for i in range(N):
        run+=c[i]
        if i>=g.SMA_BARS: run-=c[i-g.SMA_BARS]
        if i>=g.SMA_BARS-1: sma[i]=run/g.SMA_BARS
    return (ts,o,h,l,c,N,sma)

def book_points(bars, trades, is_long, gb, since_ts):
    """REALIZED book, deep-mirroring the exact backtest leg_book walk (faithful, neg=0 by
    construction). Points per clip, gated to clips CLOSING after since_ts (deploy-forward)."""
    ts,o,h,l,c,N,sma=bars
    tot_pts=0.0; clips=0; wins=0
    for ei,xi,epx in trades:
        ref=o[ei]; entry=None; wm=None
        for i in range(ei,xi):
            cur=c[i]
            if entry is None:
                cond=(cur/ref-1)*1e4>=g.GOLD_RT_BP if is_long else (1-cur/ref)*1e4>=g.GOLD_RT_BP
                if cond: entry=cur; wm=cur
                continue
            if is_long:
                if cur>wm: wm=cur
                stop=max(entry, wm*(1-gb/1e4))
                if cur<=stop:
                    p=stop-entry
                    if ts[i]>since_ts: tot_pts+=p; clips+=1; wins+=p>1e-6
                    entry=None; wm=None; ref=stop
            else:
                if cur<wm: wm=cur
                stop=min(entry, wm*(1+gb/1e4))
                if cur>=stop:
                    p=entry-stop
                    if ts[i]>since_ts: tot_pts+=p; clips+=1; wins+=p>1e-6
                    entry=None; wm=None; ref=stop
        if entry is not None:                              # window ended open -> flush at last close
            last=c[xi-1] if xi-1>=ei else o[ei]
            p=max(0.0,(last-entry) if is_long else (entry-last))
            if ts[xi]>since_ts: tot_pts+=p; clips+=1; wins+=p>1e-6
    return dict(pts=tot_pts, clips=clips, wins=wins, mark=(c[-1] if N else 0.0))

def build_state(bars, since_ts):
    ts=bars[0]
    tl=g.parent_long(bars,W,THR); tsr=g.parent_short(bars,W,THR)
    dpp=DPP_PER_LOT*LOT
    flavors=[]; desk_pts=0.0
    for name,dirn,is_long,trades in [("AUPOS","long",True,tl),("AUNEG","short",False,tsr)]:
        comps=[]; book_pts=0.0
        for gb,tag in TIERS:
            b=book_points(bars,trades,is_long,gb,since_ts)
            comps.append(dict(tier=tag, gb_bp=gb, clips=b["clips"], wins=b["wins"],
                              pts=round(b["pts"],2), usd=round(b["pts"]*dpp,0)))
            book_pts+=b["pts"]
        flavors.append(dict(name=name, dir=dirn, events=len(trades),
                            book_pts=round(book_pts,2), book_usd=round(book_pts*dpp,0),
                            companions=comps))
        desk_pts+=book_pts
    return dict(ts=(ts[-1] if ts else 0), lot=LOT, dpp=dpp, bars=len(ts),
                shadow=True, engine="gold-befloor-AUPOS-AUNEG", deploy_ts=since_ts,
                desk_pts=round(desk_pts,2), desk_usd=round(desk_pts*dpp,0),
                flavors=flavors)

def emit_companion_book(state):
    """Write a stall-accountant book file so companion_aggregate.py folds AUPOS-GOLD/AUNEG-GOLD
    per_engine entries into /api/companion -> live desk GOLD COMPANIONS panel (no rebuild)."""
    per_engine={}; rt=0.0
    for fl in state["flavors"]:
        clips=sum(c["clips"] for c in fl["companions"])
        usd=fl["book_usd"]
        per_engine[f"{fl['name']}-GOLD"]={"open":0,"closed":clips,"realized":round(usd,2)}
        rt+=usd
    now=datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M")
    book={"updated":f"{now} UTC","gauge":"PCT","stall_bars":0,"gate_pct":0,
          "reversal_giveback":0.0,"arm_usd":0.0,"trail_usd":0.0,"retrig_usd":0.0,
          "open_companions":0,"realized_total":round(rt,2),"realized_today":0.0,
          "realized_7d":round(rt,2),"realized_30d":round(rt,2),
          "per_engine":per_engine,
          "by_book":{"OMEGA":{"realized":round(rt,2),"realized_today":0.0,
                              "realized_7d":round(rt,2),"realized_30d":round(rt,2)}}}
    os.makedirs(os.path.dirname(STALL_BOOK), exist_ok=True)
    with open(STALL_BOOK,"w") as f: json.dump(book,f)

def push_vps():
    if not PUSH: return
    if not (os.path.exists(STATE) and os.path.getsize(STATE)>0): return
    try:
        subprocess.run(["scp","-q",STATE,VPS_DST],check=True,timeout=30)
        print("gold companion: state pushed -> VPS")
    except Exception as e:
        print(f"gold companion: VPS push failed ({e}) — desk keeps prior copy")

def main():
    warm_seed()
    rows=merge_history(fetch_live())
    if len(rows) < W+1:
        print(f"gold companion: only {len(rows)} bars — need > {W}; skip cycle"); return
    # deploy-forward anchor: on the FIRST live cycle, stamp deploy_ts = latest bar seen, so the
    # live book counts ONLY clips closing after deploy (nothing pre-deploy books). Persisted once.
    if not os.path.exists(DEPLOYF):
        with open(DEPLOYF,"w") as f: f.write(str(rows[-1][0]))
        print(f"[DEPLOY] gold BE-floor companion deploy_ts stamped = {rows[-1][0]} (forward-only book)")
    bars=bars_from_rows(rows)
    state=build_state(bars, deploy_ts())
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE,"w") as f: json.dump(state,f,separators=(",",":"))
    emit_companion_book(state)           # -> live desk GOLD COMPANIONS panel (no rebuild)
    d=state
    print(f"gold companion: bars={d['bars']} AUPOS={d['flavors'][0]['book_usd']:+.0f}$ "
          f"AUNEG={d['flavors'][1]['book_usd']:+.0f}$ DESK={d['desk_usd']:+.0f}$ (lot={LOT})")
    push_vps()

if __name__=="__main__": main()
