// =============================================================================
// gold_sub30_mimic_1m_bt.cpp — BE-mimic re-check for the two WIRED sub-30m gold
// engines (S-2026-07-14, operator re-open order):
//   GoldDon15m_60_35_stop3.5ATR   (15m DONf 60/35 stop3.5ATR)
//   GoldDon10m_30_35_stop3ATR     (10m DONf 30/35 stop3.0ATR)
//
// Prior verdict (14u): be=0.10 grid (216 cells) NOT viable — 10m 0/216; 15m
// close-grade passers collapsed at 1m truth. Tombstone allows re-open on a NEW
// basis: this harness sweeps the UNTESTED higher-BE space (be 0.25/0.5/1.0 —
// the conservative family that passed all 5 H1/M30 books in bb) and manages at
// 1m TRUTH GRAIN ONLY (no close-grade column at all — the optimism that faked
// the 15m survivors is structurally absent here).
//
// MIMIC MODEL — same exact GoldTrendMimicBook::on_h1_bar() replica as
// gold_newengine_mimic_bt.cpp (legs PENDING at parent trigger px; ENTER at the
// BE level once the favorable 1m extreme clears +be%; managed from next 1m bar;
// pre-arm LOSS_CUT −lc%; arm at +arm% MFE; peak-giveback trail (1−gb)·peak
// BE-floored; window-cap flush; adverse-first intrabar seq; pend cancel).
// pend/cap are in PARENT bars, scaled ×tf_minutes onto the 1m stream.
//
// Windows = the parent study's (gold_subh30_tf_bt): 6mo gate window
// 2026-01-14..2026-07-15, WF split 2026-04-14, FULL check from 2024-06-01.
// Gate per leg: n>=25, net>0 @1x(5bp) AND @2x(10bp), PF>=1.3, WF both halves
// >0, FULL net >0. Book passes when T(gb=.08) and W(gb=.20) legs BOTH pass at
// the same (be,arm,lc,cap,pend). Judged STANDALONE (companion rule).
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_sub30_mimic_1m_bt.cpp \
//            -o /tmp/gold_sub30_mimic_1m_bt
// RUN:   /tmp/gold_sub30_mimic_1m_bt <entries.csv> <m1_bars.csv> <tf_minutes>
//        entries.csv rows: dir,entry_px,entry_ts  (ts = parent trigger-bar start)
// ENV:   COSTBP=5 | BE_LIST/ARM_LIST/GB_LIST/LC_LIST/CAP_LIST/PEND_LIST
// OUT:   R,be,arm,gb,lc,cap,pend,n6,net1,net2,pf,wr,wf1,wf2,worst,dd,usd1,usd2,nF,fullnet1
//        (net/wf/worst/dd in %-points summed over the 6mo window at 1x unless
//         suffixed; usd at 10oz*px notional; fullnet1 = FULL-window net1 %-pts)
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

static const int64_t T_FULL0 = 1717200000; // 2024-06-01
static const int64_t T_6MO0  = 1768348800; // 2026-01-14
static const int64_t T_MID   = 1776124800; // 2026-04-14 WF split
static const int64_t T_END   = 1784073600; // 2026-07-15
static double COSTBP = 5.0;

struct Bar { int64_t ts; double o,h,l,c; };
struct Entry { int dir; double px; int64_t ts; };

static std::vector<Bar> load_bars(const char* path){
    std::vector<Bar> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); std::exit(1); }
    std::string line;
    while(std::getline(f,line)){
        Bar b; long long ts;
        if(std::sscanf(line.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)==5){
            b.ts=ts; v.push_back(b);
        }
    }
    return v;
}
static std::vector<Entry> load_entries(const char* path){
    std::vector<Entry> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); std::exit(1); }
    std::string line;
    while(std::getline(f,line)){
        Entry e; long long ts;
        if(std::sscanf(line.c_str(),"%d,%lf,%lld",&e.dir,&e.px,&ts)==3){
            e.ts=ts;
            if(e.ts>=T_FULL0 && e.px>0.0 && e.dir!=0) v.push_back(e);
        }
    }
    std::sort(v.begin(),v.end(),[](const Entry&a,const Entry&b){return a.ts<b.ts;});
    return v;
}
static std::vector<double> parse_list(const char* env, const char* dflt){
    const char* s = getenv(env); if(!s) s=dflt;
    std::vector<double> v; char buf[256]; std::snprintf(buf,sizeof buf,"%s",s);
    for(char* t=strtok(buf,","); t; t=strtok(nullptr,",")) v.push_back(atof(t));
    return v;
}

struct Clip { double ret; double px; int64_t ets, xts; };
struct Params { double be, arm, gb, lc; int cap, pend; };

// exact GoldTrendMimicBook replica over the 1m stream; pend/cap in parent bars,
// scale = tf_minutes (parent bar = scale 1m bars)
static std::vector<Clip> sim(const std::vector<Entry>& entries,
                             const std::vector<Bar>& bars,
                             const std::vector<int>& spawn_idx,
                             const Params& P, int scale){
    struct Leg { int dir, bars=0, pbars=0; double trig, entry=0, peak=0;
                 bool armed=false, pending=true; int64_t ets=0; };
    std::vector<Clip> clips;
    std::vector<Leg> legs;
    const int pend_max = P.pend*scale, cap_max = P.cap*scale;
    size_t ei=0;
    for(size_t i=0;i<bars.size();++i){
        const Bar& b=bars[i];
        std::vector<Leg> still; still.reserve(legs.size());
        for(Leg& L : legs){
            if(L.pending){
                L.pbars += 1;
                const double fav  = L.dir>0 ? b.h : b.l;
                const double fret = L.dir*(fav/L.trig-1.0)*100.0;
                if(fret >= P.be){
                    L.entry = L.trig*(1.0 + L.dir*P.be/100.0);
                    L.pending=false; L.bars=0; L.peak=0.0; L.armed=false; L.ets=b.ts;
                    still.push_back(L); continue;
                }
                if(L.pbars >= pend_max) continue;
                still.push_back(L); continue;
            }
            L.bars += 1;
            const double seq[3] = { L.dir>0?b.l:b.h, L.dir>0?b.h:b.l, b.c };
            bool closed=false;
            for(int k=0;k<3 && !closed;++k){
                const double ret = L.dir*(seq[k]/L.entry-1.0)*100.0;
                if(ret>L.peak) L.peak=ret;
                if(!L.armed){
                    if(ret <= -P.lc){ clips.push_back({-P.lc, L.entry, L.ets, b.ts}); closed=true; break; }
                    if(L.peak >= P.arm) L.armed=true;
                } else {
                    const double stop_ret = (1.0-P.gb)*L.peak;
                    if(ret <= stop_ret){ clips.push_back({stop_ret, L.entry, L.ets, b.ts}); closed=true; break; }
                }
            }
            if(!closed && L.bars >= cap_max){
                const double ret = L.dir*(b.c/L.entry-1.0)*100.0;
                clips.push_back({ret, L.entry, L.ets, b.ts}); closed=true;
            }
            if(!closed) still.push_back(L);
        }
        legs.swap(still);
        while(ei<entries.size() && spawn_idx[ei]==(int)i){
            Leg L; L.dir=entries[ei].dir; L.trig=entries[ei].px; L.pending=true;
            L.pbars=0; L.ets=b.ts;
            legs.push_back(L); ++ei;
        }
        while(ei<entries.size() && spawn_idx[ei]<(int)i) ++ei;
    }
    return clips;
}

struct Stat {
    int n=0,wins=0; double net1=0,net2=0,gw=0,gl=0,wf1=0,wf2=0,worst=0,usd1=0,usd2=0;
    double dd=0; int nfull=0; double fullnet1=0;
};
static Stat digest(std::vector<Clip> clips){
    Stat s;
    std::sort(clips.begin(),clips.end(),[](const Clip&a,const Clip&b){return a.xts<b.xts;});
    const double c1=COSTBP/100.0, c2=2.0*COSTBP/100.0;
    double eq=0, pk=0;
    for(const auto& c : clips){
        const double r1=c.ret-c1, r2=c.ret-c2;
        if(c.ets>=T_FULL0){ s.nfull++; s.fullnet1+=r1; }
        if(c.ets<T_6MO0 || c.ets>=T_END) continue;   // 6mo gate window stats
        s.n++; s.net1+=r1; s.net2+=r2;
        if(r1>1e-9){ s.wins++; s.gw+=r1; } else s.gl+=-r1;
        if(r1<s.worst) s.worst=r1;
        (c.ets<T_MID ? s.wf1 : s.wf2) += r1;
        s.usd1 += r1/100.0*c.px*10.0;
        s.usd2 += r2/100.0*c.px*10.0;
        eq+=r1; if(eq>pk)pk=eq; if(pk-eq>s.dd)s.dd=pk-eq;
    }
    return s;
}

int main(int argc, char** argv){
    if(argc<4){ std::fprintf(stderr,"usage: %s <entries.csv> <m1_bars.csv> <tf_minutes>\n",argv[0]); return 1; }
    if(getenv("COSTBP")) COSTBP=atof(getenv("COSTBP"));
    const int tf = atoi(argv[3]);
    if(tf<2 || tf>60){ std::fprintf(stderr,"bad tf_minutes %d\n",tf); return 1; }

    auto bars = load_bars(argv[2]);
    auto entries = load_entries(argv[1]);

    // spawn: parent opened at the CLOSE of its trigger bar [ts, ts+tf*60) ->
    // the LAST 1m bar of that window; legs managed from the next 1m bar.
    const int64_t step = 60LL*tf;
    std::vector<int> spawn(entries.size(),-1);
    {
        size_t j=0;
        for(size_t e=0;e<entries.size();++e){
            const int64_t want=entries[e].ts;
            while(j<bars.size() && (bars[j].ts/step)*step < want) ++j;
            if(j>=bars.size()) break;
            if((bars[j].ts/step)*step!=want) continue;
            size_t k=j; while(k+1<bars.size() && (bars[k+1].ts/step)*step==want) ++k;
            spawn[e]=(int)k;
        }
        std::vector<Entry> e2; std::vector<int> s2;
        for(size_t e=0;e<entries.size();++e) if(spawn[e]>=0){ e2.push_back(entries[e]); s2.push_back(spawn[e]); }
        entries.swap(e2); spawn.swap(s2);
    }

    auto BEs  = parse_list("BE_LIST",  "0.10,0.25,0.50,1.00");
    auto ARMs = parse_list("ARM_LIST", "0.15,0.25,0.50,1.00");
    auto GBs  = parse_list("GB_LIST",  "0.08,0.10,0.20,0.30");
    auto LCs  = parse_list("LC_LIST",  "0.5,1.0,1.5,2.0");
    auto CAPs = parse_list("CAP_LIST", "12,24,48,96");
    auto PNDs = parse_list("PEND_LIST","6,12");

    std::fprintf(stderr,"[SUB30-MIMIC-1M] entries=%zu bars(1m)=%zu tf=%dm scale=%d COSTBP=%.1f\n",
                 entries.size(), bars.size(), tf, tf, COSTBP);
    std::printf("R,be,arm,gb,lc,cap,pend,n6,net1,net2,pf,wr,wf1,wf2,worst,dd,usd1,usd2,nF,fullnet1\n");
    for(double be : BEs) for(double arm : ARMs) for(double lc : LCs)
    for(double capd : CAPs) for(double pndd : PNDs) for(double gb : GBs){
        Params P{be,arm,gb,lc,(int)capd,(int)pndd};
        Stat s = digest(sim(entries,bars,spawn,P,tf));
        const double pf = s.gl>0? s.gw/s.gl : (s.gw>0?999.0:0.0);
        std::printf("R,%.2f,%.2f,%.2f,%.1f,%d,%d,%d,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,%d,%.3f\n",
            be,arm,gb,lc,P.cap,P.pend,s.n,s.net1,s.net2,pf,
            s.n? 100.0*s.wins/s.n:0.0, s.wf1,s.wf2,s.worst,s.dd,s.usd1,s.usd2,
            s.nfull,s.fullnet1);
    }
    return 0;
}
