// connors_widestop_bt.cpp — WIDE catastrophic-stop test on the REAL ConnorsRSI2Engine.
// Operator-approved (S-2026-07-17i handoff item 1): MR parents have NO stop (o.sl=0);
// standing "no cold cut on MR" verdict was about TIGHT cuts. Question: does a WIDE
// disaster stop (K x ATR20 below avg entry) cap the tail without killing the edge?
//
// FAITHFUL: drives the REAL engine exactly like the certified harnesses
// (connors_regime_gate_audit.cpp NAS n=142; connors_ger_gate_audit.cpp GER) —
// identical tick pattern, so K=off reproduces the certified baselines. The stop is
// a HARNESS OVERLAY on daily bar h/l (the engine itself is untouched):
//   - stop_px = avg_entry - K*ATR20(at initial entry); re-anchored to new avg on scale-in
//   - live from the day AFTER entry (entry fires at the cash close; that day's low is past)
//   - worse-of gap fill: bar OPEN <= stop -> fill at open (gap-through), else fill at stop
//   - force_close() routes the real _close() -> TradeRecord -> cost applied per round trip
//   - engine may re-enter at the same day's close if still signal (no cooldown, as live)
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/connors_widestop_bt.cpp -o /tmp/connors_widestop
// run:   /tmp/connors_widestop                     (both hosts, 1x + 2x cost)
#include "ConnorsRSI2Engine.hpp"
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <cmath>
#include <ctime>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load(const std::string& p){
    std::vector<Bar> v; std::ifstream f(p); std::string ln;
    long long ts; double o,h,l,c;
    while(std::getline(f,ln)){
        if(std::sscanf(ln.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5 && c>0)
            v.push_back({ts,o,h,l,c});
    }
    return v;
}

static int year_of(int64_t sec){ std::time_t t=(std::time_t)sec; std::tm g{}; gmtime_r(&t,&g); return g.tm_year+1900; }

struct Stat { int n=0,w=0; double gp=0,gl=0,net=0,peak=0,dd=0,eq=0,worst=0; };
static void rec(Stat&s,double pnl){ s.n++; if(pnl>0){s.w++;s.gp+=pnl;}else s.gl+=-pnl; s.net+=pnl;
    if(pnl<s.worst)s.worst=pnl; s.eq+=pnl; if(s.eq>s.peak)s.peak=s.eq; double d=s.peak-s.eq; if(d>s.dd)s.dd=d; }
static double pf(const Stat&s){ return s.gl>0? s.gp/s.gl : (s.gp>0?999.0:0.0); }

struct Closed { int64_t entry_ts; double pnl_pts; bool stopped; };

struct HostCfg {
    const char* name; const char* path; double cost;
    int regime_gate; bool scalein;
    int sess_open, sess_close, tz_off; bool eu_dst;
    int in_hour_utc, out_hour_utc;   // daily drive ticks (match the certified harnesses)
};

// ATR20 (simple mean of true range) ending at bar index i (inclusive).
static double atr20(const std::vector<Bar>& b, int i){
    int n=0; double s=0;
    for(int k=i;k>i-20 && k>=1;--k){
        double tr=std::max(b[k].h-b[k].l,
                  std::max(std::fabs(b[k].h-b[k-1].c),std::fabs(b[k].l-b[k-1].c)));
        s+=tr; ++n;
    }
    return n? s/n : 0.0;
}

// Drive the real engine over bars; K<=0 disables the stop (certified baseline).
// mode 0 = INTRADAY resting stop (wick/gap fills, worse-of). mode 1 = CLOSE-EVAL:
// exit at today's cash close if close<=stop (no wick-selling; fill at close px) —
// the strongest defensible variant for an MR engine that only acts at the close.
static std::vector<Closed> run(const HostCfg& hc, const std::vector<Bar>& bars,
                               double K, double cost_pts, int* stop_hits, int mode=0){
    omega::ConnorsRSI2Engine e;
    e.symbol=hc.name; e.engine_name="ConnorsRSI2";
    e.TREND_SMA=200; e.RSI_IN=10.0; e.SHORT_SMA=5; e.MAXHOLD=10;
    e.SCALEIN=hc.scalein; e.MAX_UNITS=2; e.lot=1.0; e.enabled=true; e.shadow_mode=true;
    e.REGIME_GATE=hc.regime_gate; e.BEAR_VETO_K=20;
    e.SESS_OPEN_HM=hc.sess_open; e.SESS_CLOSE_HM=hc.sess_close;
    e.TZ_STD_OFF_MIN=hc.tz_off; e.TZ_EU_DST=hc.eu_dst;
    e.init();
    std::vector<Closed> out;
    bool cur_stopped=false;   // tag the in-flight close as a stop exit
    e.on_trade_record=[&](const omega::TradeRecord& tr){
        double pnl = tr.pnl - cost_pts*tr.size;
        out.push_back({tr.entryTs, pnl, cur_stopped});
    };
    if(stop_hits)*stop_hits=0;

    double stop_px=0, atr_at_entry=0, prev_avg=0; bool armed=false;
    int entry_bar=-1;

    for(int i=0;i<(int)bars.size();++i){
        const Bar& b=bars[i];
        // ---- overlay: check the resting disaster stop BEFORE today's session ticks ----
        if(K>0 && armed && e.has_open_position() && entry_bar<i){
            double fill=0;
            if(mode==0){
                if(b.o<=stop_px)      fill=b.o;      // gap-through: worse-of, fill at the open
                else if(b.l<=stop_px) fill=stop_px;  // intraday pierce: fill at the stop
            } else {
                if(b.c<=stop_px)      fill=b.c;      // close-eval: exit at the cash close px
            }
            if(fill>0){
                std::time_t t=(std::time_t)b.ts; std::tm g{}; gmtime_r(&t,&g);
                g.tm_hour=hc.in_hour_utc; g.tm_min=30; g.tm_sec=0;   // intraday timestamp
                cur_stopped=true;
                e.force_close(fill,fill,(int64_t)timegm(&g)*1000);
                cur_stopped=false;
                armed=false; if(stop_hits)++*stop_hits;
            }
        }
        // ---- drive the engine exactly like the certified harness ----
        std::time_t t=(std::time_t)b.ts; std::tm g{}; gmtime_r(&t,&g);
        g.tm_hour=hc.in_hour_utc; g.tm_min=0; g.tm_sec=0;
        int64_t in_utc=(int64_t)timegm(&g);
        std::tm g2=g; g2.tm_hour=hc.out_hour_utc;
        int64_t out_utc=(int64_t)timegm(&g2);
        e.on_tick(b.c,b.c,in_utc*1000);
        e.on_tick(b.c,b.c,out_utc*1000);
        // ---- overlay bookkeeping: (re)anchor after the close-transition acted ----
        if(K>0){
            if(e.has_open_position()){
                double avg=e.pos_entry();
                if(!armed){                     // fresh entry today
                    atr_at_entry=atr20(bars,i);
                    stop_px=avg-K*atr_at_entry; armed=true; entry_bar=i; prev_avg=avg;
                } else if(avg!=prev_avg){       // scale-in moved the avg -> re-anchor
                    stop_px=avg-K*atr_at_entry; prev_avg=avg;
                }
            } else armed=false;
        }
    }
    e.force_close(bars.back().c,bars.back().c,bars.back().ts*1000);
    return out;
}

static void report(const char* tag,const std::vector<Closed>& tr,int stop_hits){
    Stat bull,bear,all,h1,h2; int total=(int)tr.size();
    for(int i=0;i<total;i++){
        int y=year_of(tr[i].entry_ts); double p=tr[i].pnl_pts;
        rec(all,p);
        if(y==2022) rec(bear,p);
        if(y>=2024) rec(bull,p);
        if(i<total/2) rec(h1,p); else rec(h2,p);
    }
    printf("%-14s n=%3d WR=%5.1f%% PF=%5.2f net=%9.1f maxDD=%8.1f worst=%8.1f | 2022: n=%2d net=%8.1f | H1=%8.1f H2=%8.1f both+=%s | stops=%d\n",
        tag, all.n, all.n?100.0*all.w/all.n:0, pf(all), all.net, all.dd, all.worst,
        bear.n, bear.net, h1.net, h2.net, (h1.net>0&&h2.net>0)?"YES":"NO ", stop_hits);
}

int main(){
    HostCfg hosts[2]={
        // NAS: deployed config — REGIME_GATE=1 asym bear-veto, SCALEIN on, ~8pt RT (audit basis)
        {"NAS100","/Users/jo/Tick/NDX_daily_2016_2026.csv",8.0,1,true, 930,1600,-300,false,14,22},
        // GER: deployed config — close>SMA200 gate, no scale-in, ~3pt RT (certified basis)
        {"GER40","/Users/jo/Tick/GER40_daily_2016_2026.csv",3.0,0,false,900,1730,  60,true, 12,19},
    };
    double Ks[]={0,2,3,4,5,6};
    for(const auto& hc: hosts){
        auto bars=load(hc.path);
        printf("\n########## %s  (%zu bars, gate=%d scalein=%d, cost=%.0fpt RT) ##########\n",
            hc.name,bars.size(),hc.regime_gate,(int)hc.scalein,hc.cost);
        for(double mult:{1.0,2.0}){
            printf("---- %.0fx cost (%.0fpt) ----\n",mult,hc.cost*mult);
            for(double K:Ks){
                int hits=0; auto tr=run(hc,bars,K,hc.cost*mult,&hits,0);
                char tag[32]; if(K<=0) snprintf(tag,32,"BASE no-stop"); else snprintf(tag,32,"K=%.0f wick",K);
                report(tag,tr,hits);
            }
            for(double K:Ks){
                if(K<=0) continue;   // baseline identical across modes
                int hits=0; auto tr=run(hc,bars,K,hc.cost*mult,&hits,1);
                char tag[32]; snprintf(tag,32,"K=%.0f close-ev",K);
                report(tag,tr,hits);
            }
        }
    }
    // ---- mechanism dump: the actual stop-hit trades (K=4 wick), date + pnl ----
    printf("\n########## STOP-HIT EPISODES (K=4 wick, 1x cost) ##########\n");
    for(const auto& hc: hosts){
        auto bars=load(hc.path);
        int hits=0; auto tr=run(hc,bars,4.0,hc.cost,&hits,0);
        printf("-- %s --\n",hc.name);
        for(const auto& c: tr) if(c.stopped){
            std::time_t t=(std::time_t)c.entry_ts; std::tm g{}; gmtime_r(&t,&g);
            printf("  entry %04d-%02d-%02d  pnl=%.1f (STOPPED)\n",g.tm_year+1900,g.tm_mon+1,g.tm_mday,c.pnl_pts);
        }
        // baseline pnl of the same-entry trades for comparison
        auto base=run(hc,bars,0,hc.cost,nullptr,0);
        for(const auto& c: tr) if(c.stopped)
            for(const auto& b0: base) if(b0.entry_ts==c.entry_ts)
                printf("  base same-entry %lld -> pnl=%.1f (rode to MR exit)\n",(long long)c.entry_ts,b0.pnl_pts);
    }
    return 0;
}
