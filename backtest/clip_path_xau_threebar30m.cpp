// clip_path_xau_threebar30m.cpp -- per-BAR PATH csv for XauThreeBar30mEngine
// (S-2026-07-17 dedicated-FLOORED-clip certification; IBS S-17t pattern).
//
// Drives the REAL omega::XauThreeBar30mEngine at the FULL LIVE engine_init.hpp
// config (L4001 block, verbatim: long_only, lot 0.01, S35-P4 TUNED be_trigger
// 1.0*ATR / trail 0.75*ATR / atr_floor 0.30, S63 LOSS_CUT 0.05 / BE_ARM 0.03 /
// BE_BUFFER 0.012, S88 slope_12 + vol_band 0.30-0.85 gates, HMM off) over
// XAUUSD M30 bars (ts,o,h,l,c). Each bar is fed as 4 intrabar ticks o,l,h,c
// (adverse-first for the long-only engine) through the engine's own on_tick
// manage path, then on_30m_bar at bar close with an EXTERNAL Wilder ATR14
// (mirrors g_bars_gold.m30.ind.atr14 -- required or the vol_band window never
// fills and the live gate silently drops out). gold_regime() is tick-fed from
// the same stream so REGIME_BEAR_LONG_BLOCK is live (as in prod; ~300 H1 bars
// to warm, neutral before that == prod cold-start fail-open).
//
// Output: per-close path entry->natural exit, IBS schema:
//   trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
// cost_rt = IBKR XAU basis as fraction of entry: 2*0.00015 + 2*half/entry
// (project-ibkr-cost-basis; at lot 0.01 => $1/pt so cost_usd = cost_rt*entry).
//
// PATH GRAIN: with an optional <m1.csv> the path rows are M1 closes in
// [entryTs, exitTs) -- the honest grain for this parent: median hold < 30min
// (S63 LOSS_CUT 0.05% cuts fast), so M30-close paths are 1-2 rows (vacuous),
// while the live StallCompanion drives at 60s = exactly M1 cadence. The last
// row is the last M1 close BEFORE the parent exit (the <=60s-stale mark the
// live book banks on ENGINE_EXIT -- never the parent's own fill). Without
// <m1.csv> falls back to M30-close rows (IBS-style).
//
//   usage: clip_path_xau_threebar30m <m30.csv> <out.csv> [half_spread=0.15] [m1.csv]
//   build: g++ -O2 -std=c++17 -I include backtest/clip_path_xau_threebar30m.cpp -o <bin>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "XauThreeBar30mEngine.hpp"

struct Bar{ int64_t ts_sec; double o,h,l,c; };
static std::vector<Bar> load_csv(const std::string& p){
    std::vector<Bar> b; std::ifstream f(p); if(!f) return b;
    std::string line;
    while(std::getline(f,line)){
        if(line.empty()||!(line[0]>='0'&&line[0]<='9')) continue;
        Bar x; const char* s=line.c_str(); char* e;
        x.ts_sec=std::strtoll(s,&e,10); if(*e!=',')continue; s=e+1;
        x.o=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.h=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.l=std::strtod(s,&e); if(*e!=',')continue; s=e+1;
        x.c=std::strtod(s,&e); b.push_back(x);
    } return b;
}
struct Trade{ int64_t entryTs,exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> ap(N,0.0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a=i<15?(a*(i-1)+tr)/i:(a*13+tr)/14; ap[i]=B[i].c>0?a/B[i].c:0.0; } return ap;
}
static std::vector<double> sma200(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200)sum-=B[i-200].c; s[i]=i>=199?sum/200.0:B[i].c; } return s;
}
static int idx_at(const std::vector<Bar>&B,int64_t ts){
    int lo=0,hi=B.size()-1,r=0;
    while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts_sec<=ts){r=m;lo=m+1;}else hi=m-1;} return r;
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s <m30.csv> <out.csv> [half] [m1.csv]\n",argv[0]); return 2; }
    std::string path=argv[1], outp=argv[2]; double half=argc>3?std::atof(argv[3]):0.15;
    auto B=load_csv(path);
    fprintf(stderr,"loaded M30=%zu bars\n",B.size());
    if(B.size()<500){ fprintf(stderr,"too few bars\n"); return 1; }
    std::vector<Bar> M1;
    if(argc>4){ M1=load_csv(argv[4]); fprintf(stderr,"loaded M1=%zu bars (60s companion-drive grain)\n",M1.size()); }

    // ── LIVE engine_init.hpp config, verbatim (engine_init.hpp ~L4001) ──
    omega::XauThreeBar30mEngine eng;
    eng.shadow_mode        = true;   // sim tag only; live is false (irrelevant to path)
    eng.enabled            = true;
    eng.long_only          = true;   // S96
    eng.lot                = 0.01;
    eng.max_spread         = 1.0;
    eng.be_trigger_atr     = 1.0;    // S35-P4 TUNED
    eng.be_cost_buffer_pts = 0.10;
    eng.trail_after_be     = true;
    eng.trail_atr_mult     = 0.75;
    eng.min_atr_floor      = 0.30;
    eng.max_bars_held      = 0;
    eng.daily_loss_limit   = 0.0;
    eng.max_consec_losses  = 0;
    eng.max_atr_ceil       = 0.0;
    eng.block_hour_start   = -1;
    eng.block_hour_end     = -1;
    eng.use_slope_gate      = true;  // S88
    eng.slope_lookback_bars = 12;
    eng.use_vol_band_gate   = true;
    eng.vol_band_low_pct    = 0.30;
    eng.vol_band_high_pct   = 0.85;
    // S63 class defaults re-affirmed in engine_init (LOSS_CUT/BE_ARM/BE_BUFFER)
    eng.LOSS_CUT_PCT  = 0.05;
    eng.BE_ARM_PCT    = 0.03;
    eng.BE_BUFFER_PCT = 0.012;
    eng.init();

    std::vector<Trade> trades;
    auto cb=[&](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs; // seconds
        t.dir=(tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice; trades.push_back(t);
    };

    // External Wilder ATR14 (mirrors g_bars_gold.m30.ind.atr14 feed).
    double atr=0.0; int warm=0;
    for(size_t i=0;i<B.size();++i){
        const auto&b=B[i]; const int64_t base=b.ts_sec*1000LL;
        // intrabar ticks o,l,h,c (adverse-first for the long-only engine):
        // manage path + regime H1 aggregation, exactly one tick stream.
        const double seq[4]={b.o,b.l,b.h,b.c};
        for(int k=0;k<4;k++){
            const int64_t tms=base+(int64_t)k*450000LL;
            eng.on_tick(seq[k]-half,seq[k]+half,tms,cb);
            omega::gold_regime().on_tick(seq[k]-half,seq[k]+half,tms);
        }
        // Wilder ATR14 including this bar, then bar-close dispatch.
        if(i>0){
            double tr=std::max({b.h-b.l,std::fabs(b.h-B[i-1].c),std::fabs(b.l-B[i-1].c)});
            if(warm<14){ atr=(atr*warm+tr)/(warm+1); ++warm; }
            else atr=(atr*13.0+tr)/14.0;
        }
        omega::XauThreeBar30mBar bar; bar.bar_start_ms=base;
        bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
        eng.on_30m_bar(bar,b.c-half,b.c+half,(warm>=14?atr:0.0),base+1800000LL,cb);
    }
    fprintf(stderr,"XauThreeBar30m LIVE-config: %zu trades\n",trades.size());

    FILE* out=fopen(outp.c_str(),"w"); if(!out){fprintf(stderr,"cannot open %s\n",outp.c_str());return 1;}
    auto ap=atr_pct(B); auto sm=sma200(B);
    fprintf(out,"trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0;
    for(const auto& t:trades){
        int ei=idx_at(B,t.entryTs); int xi=idx_at(B,t.exitTs); if(xi<ei)xi=ei;
        double atrp=ap[ei]; int bull=(B[ei].c>sm[ei])?1:0;
        // IBKR XAU RT cost as a fraction of entry: commission 2*1.5bp + spread.
        double cost_rt=2.0*0.00015 + (2.0*half)/t.entry_px;
        int seq=0;
        if(!M1.empty()){
            // M1 closes in [entryTs, exitTs): the 60s marks the live book sees.
            int mi=idx_at(M1,t.entryTs);
            while(mi>0 && M1[mi-1].ts_sec>=t.entryTs) --mi;          // first M1 close >= entry
            while(mi<(int)M1.size() && M1[mi].ts_sec<t.entryTs) ++mi;
            for(; mi<(int)M1.size() && M1[mi].ts_sec<t.exitTs; ++mi){
                fprintf(out,"%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                    tid,seq,(long long)M1[mi].ts_sec*1000LL,t.dir,t.entry_px,M1[mi].c,atrp,bull,cost_rt);
                seq++;
            }
            if(seq==0)   // sub-minute hold: one row at entry (mark ~= entry, never confirms)
                fprintf(out,"%d,0,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                    tid,(long long)t.entryTs*1000LL,t.dir,t.entry_px,t.entry_px,atrp,bull,cost_rt);
        } else {
            for(int i=ei;i<=xi && i<(int)B.size();++i){
                fprintf(out,"%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                    tid,seq,(long long)B[i].ts_sec*1000LL,t.dir,t.entry_px,B[i].c,atrp,bull,cost_rt);
                seq++;
            }
        }
        tid++;
    }
    fclose(out); return 0;
}
