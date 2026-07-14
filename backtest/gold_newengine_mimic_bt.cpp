// =============================================================================
// gold_newengine_mimic_bt.cpp — BE-entry mimic validation for the 5 NEW gold
// engines (S-2026-07-14bb). BACKTEST ONLY — wires nothing.
//
// Parents (entry streams emitted by the REAL harnesses via DUMP_ENTRIES=1):
//   MgcTf1h       backtest/mgc_tf1h_port_bt.cpp        (VT=0 LC=0, real engine)
//   GoldKeltM30   gold_shorttf_bothways_bt.cpp  KELT m30 k1.25 trail2.5
//   GoldTfBw1040  gold_shorttf_bothways_bt.cpp  TF1H ema10/40 trail2.0
//   GoldTfBw20100 gold_shorttf_bothways_bt.cpp  TF1H ema20/100 trail2.0
//   GoldDonH1     gold_shorttf_bothways_bt.cpp  DON h1 20/10 stop3ATR
//
// MIMIC MODEL — EXACT replica of include/GoldTrendMimicLadder.hpp
// GoldTrendMimicBook::on_h1_bar() close-grade semantics:
//   * parent OPEN -> legs PENDING at trigger px; a leg ENTERS only once the
//     favorable bar extreme clears +be_entry_pct% off the trigger (entry AT the
//     BE level; managed from the NEXT bar); cancel after pend_bars.
//   * BOTH directions (short parent: BE level = trig*(1 - be/100)).
//   * entered: adverse-first intrabar seq (l->h->c for longs), pre-arm LOSS_CUT
//     books exactly -lc_pct, arm at +arm_pct MFE, then peak-giveback trail
//     books exactly (1-gb)*peak (BE-floored by construction), window-cap flush
//     at the close after cap_bars native bars.
//   * cost: rt_cost_bp per clip (env COSTBP, default 5 = the conservative
//     XAU_4h_DonchN20 live-book debit; 2x stress = 10bp reported in-line).
//
// GRAIN: mgmt native bar per book (M30 for KELT, H1 for the rest). FINE=1
// manages on the M30 stream with pend/cap scaled x2 for H1-native books —
// the intrabar-truth honesty check (engine_init trap: close-grade survivor
// mimic figures collapsed at intrabar truth; USTEC_4h_ZMR tombstoned on it).
//
// Windows: FULL 2024-09-01..2026-07-13 (spliced CERTIFIED M30), WF split
// 2025-07-01. Judged STANDALONE (companion independent-engine rule).
//
// BUILD: c++ -std=c++17 -O2 backtest/gold_newengine_mimic_bt.cpp \
//            -o /tmp/gold_newengine_mimic_bt
// RUN:   /tmp/gold_newengine_mimic_bt <entries.csv> <m30_bars.csv> <h1|m30>
//        entries.csv rows: dir,entry_px,entry_ts   (parent opens, ts=bar start)
// ENV:   FINE=1 | COSTBP=5 | BE_LIST/ARM_LIST/GB_LIST/LC_LIST/CAP_LIST/
//        PEND_LIST (comma lists; defaults = the operator sweep spec)
// OUT:   one line per (be,arm,lc,cap,pend,gb) cell:
//        R,be,arm,gb,lc,cap,pend,n,net1,net2,pf,wr,wf1,wf2,worst,dd,usd1,usd2
//        (net/wf/worst/dd in %-points summed per leg; usd at 10oz*px notional)
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

static const int64_t T_FULL0 = 1725148800; // 2024-09-01
static const int64_t T_SPLIT = 1751328000; // 2025-07-01 WF split
static double COSTBP = 5.0;                // rt cost, bp of notional, per clip

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
static std::vector<Bar> to_h1(const std::vector<Bar>& m30){
    std::vector<Bar> out;
    for(const auto& b : m30){
        int64_t bucket=(b.ts/3600)*3600;
        if(out.empty()||out.back().ts!=bucket) out.push_back({bucket,b.o,b.h,b.l,b.c});
        else { Bar& x=out.back(); x.h=std::max(x.h,b.h); x.l=std::min(x.l,b.l); x.c=b.c; }
    }
    return out;
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

// ── exact GoldTrendMimicBook::on_h1_bar replica over the mgmt bar stream ──
static std::vector<Clip> sim(const std::vector<Entry>& entries,
                             const std::vector<Bar>& bars,
                             const std::vector<int>& spawn_idx, // per entry: bar idx AT whose close the parent opened
                             const Params& P, int scale){
    struct Leg { int dir, bars=0, pbars=0; double trig, entry=0, peak=0;
                 bool armed=false, pending=true; int64_t ets=0; double refpx; };
    std::vector<Clip> clips;
    std::vector<Leg> legs;
    const int pend_max = P.pend*scale, cap_max = P.cap*scale;
    size_t ei=0;
    for(size_t i=0;i<bars.size();++i){
        const Bar& b=bars[i];
        // manage legs spawned strictly before this bar
        std::vector<Leg> still; still.reserve(legs.size());
        for(Leg& L : legs){
            if(L.pending){
                L.pbars += 1;
                const double fav  = L.dir>0 ? b.h : b.l;
                const double fret = L.dir*(fav/L.trig-1.0)*100.0;
                if(fret >= P.be){                       // BE made -> ENTER at the BE level
                    L.entry = L.trig*(1.0 + L.dir*P.be/100.0);
                    L.pending=false; L.bars=0; L.peak=0.0; L.armed=false; L.ets=b.ts;
                    still.push_back(L); continue;       // manage from the NEXT bar
                }
                if(L.pbars >= pend_max) continue;       // BE never made -> cancel, no book
                still.push_back(L); continue;
            }
            L.bars += 1;
            const double seq[3] = { L.dir>0?b.l:b.h, L.dir>0?b.h:b.l, b.c }; // adverse first
            bool closed=false;
            for(int k=0;k<3 && !closed;++k){
                const double ret = L.dir*(seq[k]/L.entry-1.0)*100.0;
                if(ret>L.peak) L.peak=ret;
                if(!L.armed){
                    if(ret <= -P.lc){ clips.push_back({-P.lc, L.entry, L.ets, b.ts}); closed=true; break; }
                    if(L.peak >= P.arm) L.armed=true;
                } else {
                    const double stop_ret = (1.0-P.gb)*L.peak;   // keep (1-gb) of peak, BE-floored
                    if(ret <= stop_ret){ clips.push_back({stop_ret, L.entry, L.ets, b.ts}); closed=true; break; }
                }
            }
            if(!closed && L.bars >= cap_max){            // window flush at the close
                const double ret = L.dir*(b.c/L.entry-1.0)*100.0;
                clips.push_back({ret, L.entry, L.ets, b.ts}); closed=true;
            }
            if(!closed) still.push_back(L);
        }
        legs.swap(still);
        // spawn parents whose trigger bar closed AT this bar (managed from next bar)
        while(ei<entries.size() && spawn_idx[ei]==(int)i){
            Leg L; L.dir=entries[ei].dir; L.trig=entries[ei].px; L.pending=true;
            L.pbars=0; L.ets=b.ts; L.refpx=entries[ei].px;
            legs.push_back(L); ++ei;
        }
        // entries whose spawn bar is missing from the stream (shouldn't happen): skip
        while(ei<entries.size() && spawn_idx[ei]<(int)i) ++ei;
    }
    return clips;   // legs still open at data end are dropped (un-booked, like live)
}

struct Stat {
    int n=0,wins=0; double net1=0,net2=0,gw=0,gl=0,wf1=0,wf2=0,worst=0,usd1=0,usd2=0;
    double dd=0;
};
static Stat digest(std::vector<Clip> clips){
    Stat s;
    std::sort(clips.begin(),clips.end(),[](const Clip&a,const Clip&b){return a.xts<b.xts;});
    const double c1=COSTBP/100.0, c2=2.0*COSTBP/100.0; // bp -> %-points
    double eq=0, pk=0;
    for(const auto& c : clips){
        const double r1=c.ret-c1, r2=c.ret-c2;
        s.n++; s.net1+=r1; s.net2+=r2;
        if(r1>1e-9){ s.wins++; s.gw+=r1; } else s.gl+=-r1;
        if(r1<s.worst) s.worst=r1;
        (c.ets<T_SPLIT ? s.wf1 : s.wf2) += r1;
        s.usd1 += r1/100.0*c.px*10.0;  // 1 MGC = 10oz, notional = 10*px
        s.usd2 += r2/100.0*c.px*10.0;
        eq+=r1; if(eq>pk)pk=eq; if(pk-eq>s.dd)s.dd=pk-eq;
    }
    return s;
}

int main(int argc, char** argv){
    if(argc<4){ std::fprintf(stderr,"usage: %s <entries.csv> <m30_bars.csv> <h1|m30>\n",argv[0]); return 1; }
    if(getenv("COSTBP")) COSTBP=atof(getenv("COSTBP"));
    const bool native_h1 = std::strcmp(argv[3],"h1")==0;
    const bool fine = getenv("FINE") && atoi(getenv("FINE"))!=0;

    auto m30 = load_bars(argv[2]);
    auto entries = load_entries(argv[1]);
    // mgmt stream + scale: M30-native books always manage on M30 (scale 1 —
    // M30 IS the finest grain in the data). H1 books: H1 close-grade, or the
    // M30 sub-bar stream with pend/cap x2 when FINE=1.
    std::vector<Bar> bars;
    int scale=1;
    if(!native_h1)            { bars=m30; }
    else if(!fine)            { bars=to_h1(m30); }
    else                      { bars=m30; scale=2; }

    // spawn index per entry: the mgmt bar AT whose close the parent opened.
    // native: bar.ts == entry.ts. fine(H1 book on M30): LAST m30 bar of the
    // trigger hour (the H1 close moment) -> managed from the next m30 bar.
    std::vector<int> spawn(entries.size(),-1);
    {
        size_t j=0;
        for(size_t e=0;e<entries.size();++e){
            const int64_t want=entries[e].ts;
            while(j<bars.size() && ((scale==2? (bars[j].ts/3600)*3600 : bars[j].ts) < want)) ++j;
            if(j>=bars.size()) break;
            if(scale==2){
                if((bars[j].ts/3600)*3600!=want) continue;
                size_t k=j; while(k+1<bars.size() && (bars[k+1].ts/3600)*3600==want) ++k;
                spawn[e]=(int)k;
            } else {
                if(bars[j].ts!=want) continue;
                spawn[e]=(int)j;
            }
        }
        // drop unmatched entries
        std::vector<Entry> e2; std::vector<int> s2;
        for(size_t e=0;e<entries.size();++e) if(spawn[e]>=0){ e2.push_back(entries[e]); s2.push_back(spawn[e]); }
        entries.swap(e2); spawn.swap(s2);
    }

    auto BEs  = parse_list("BE_LIST",  "0.10,0.15,0.25,0.5,1.0");
    auto ARMs = parse_list("ARM_LIST", "0.15,0.25,0.5");
    auto GBs  = parse_list("GB_LIST",  "0.08,0.10,0.20,0.30");
    auto LCs  = parse_list("LC_LIST",  "1.0,1.5,2.0");
    auto CAPs = parse_list("CAP_LIST", native_h1 ? "12,24,48" : "24,48,96");
    auto PNDs = parse_list("PEND_LIST","6,12");

    std::fprintf(stderr,"[MIMIC-BT] entries=%zu bars=%zu grain=%s fine=%d scale=%d COSTBP=%.1f\n",
                 entries.size(), bars.size(), argv[3], fine?1:0, scale, COSTBP);
    std::printf("R,be,arm,gb,lc,cap,pend,n,net1,net2,pf,wr,wf1,wf2,worst,dd,usd1,usd2\n");
    for(double be : BEs) for(double arm : ARMs) for(double lc : LCs)
    for(double capd : CAPs) for(double pndd : PNDs) for(double gb : GBs){
        Params P{be,arm,gb,lc,(int)capd,(int)pndd};
        Stat s = digest(sim(entries,bars,spawn,P,scale));
        const double pf = s.gl>0? s.gw/s.gl : (s.gw>0?999.0:0.0);
        std::printf("R,%.2f,%.2f,%.2f,%.1f,%d,%d,%d,%.3f,%.3f,%.3f,%.1f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f\n",
            be,arm,gb,lc,P.cap,P.pend,s.n,s.net1,s.net2,pf,
            s.n? 100.0*s.wins/s.n:0.0, s.wf1,s.wf2,s.worst,s.dd,s.usd1,s.usd2);
    }
    return 0;
}
