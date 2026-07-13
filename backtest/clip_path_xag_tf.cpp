// clip_path_xag_tf.cpp -- SILVER (XAGUSD) per-BAR PATH csv for the XauTrendFollow
// chassis (4h/d1/2h), by driving the REAL engine classes over CERTIFIED XAG bars
// (backtest/data/XAGUSD_2022_2026.{h4,h1}.csv, staged via stage_certified_data.sh
// 2026-07-13 after the locality-aware integrity-gate fix; the 2025-26 silver rally
// 36->119->62 is REAL -- bar-continuous, matches independent IBKR L2 capture).
//
// The XauTf engines are ATR/EMA-relative (SL/TP = mult*ATR) -> scale-free, so the
// chassis transfers to silver unchanged; only data + spread differ. Silver spread:
// IBKR XAGUSD ~0.025 USD -> HALF_SPREAD 0.0125 (argv-overridable for sensitivity).
// cost_rt = 2*1.5bp commission + spread/price (same formula as clip_path_xau_tf).
//
// PATH row: trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
// Output feeds mimic_ladder_overlay.py -- judged STANDALONE (never vs WIDE).
//
// Usage: clip_path_xag_tf <4h|d1|2h> [half_spread] [h4_csv] [h1_csv]
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "XauTrendFollowD1Engine.hpp"
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"

struct Bar { int64_t ts_sec; double o,h,l,c; };
static std::vector<Bar> load_csv(const std::string& path){
    std::vector<Bar> bars; std::ifstream f(path); if(!f) return bars;
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){ Bar b; const char* p=line.c_str(); char* e;
        b.ts_sec=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
        b.o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
        b.c=std::strtod(p,&e); bars.push_back(b);
    } return bars;
}
struct Trade{ int64_t entryTs,exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> ap(N,0.0); double a=0;
    for(int i=1;i<N;i++){ double tr=std::max({B[i].h-B[i].l,std::fabs(B[i].h-B[i-1].c),std::fabs(B[i].l-B[i-1].c)});
        a = i<15 ? (a*(i-1)+tr)/i : (a*13+tr)/14;
        ap[i]= B[i].c>0 ? a/B[i].c : 0.0; }
    return ap;
}
static std::vector<double> sma200(const std::vector<Bar>&B){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200) sum-=B[i-200].c;
        s[i]= i>=199 ? sum/200.0 : B[i].c; }
    return s;
}
static int idx_at(const std::vector<Bar>&B,int64_t ts){
    int lo=0,hi=B.size()-1,r=0;
    while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts_sec<=ts){r=m;lo=m+1;}else hi=m-1;} return r;
}

static void emit(FILE* out,int& tid,const std::vector<Trade>&trades,const std::vector<Bar>&B,double half){
    auto ap=atr_pct(B); auto sm=sma200(B);
    for(const auto& t:trades){
        int ei=idx_at(B,t.entryTs); int xi=idx_at(B,t.exitTs);
        if(xi<ei) xi=ei;
        double atrp = ap[ei];
        int bull = (B[ei].c > sm[ei]) ? 1 : 0;
        double cost_rt = 2.0*0.00015 + (2.0*half)/t.entry_px;
        int seq=0;
        for(int i=ei;i<=xi && i<(int)B.size(); ++i){
            fprintf(out,"%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid,seq,(long long)B[i].ts_sec*1000LL,t.dir,t.entry_px,B[i].c,
                atrp,bull,cost_rt);
            seq++;
        }
        tid++;
    }
}

int main(int argc,char**argv){
    const char* which = argc>1?argv[1]:"4h";
    const double half = argc>2?std::atof(argv[2]):0.0125;   // XAG half-spread USD
    const char* h4path = argc>3?argv[3]:"backtest/data/XAGUSD_2022_2026.h4.csv";
    const char* h1path = argc>4?argv[4]:"backtest/data/XAGUSD_2022_2026.h1.csv";
    auto h4=load_csv(h4path);
    auto h1=load_csv(h1path);
    fprintf(stderr,"loaded H4=%zu H1=%zu half=%.4f\n",h4.size(),h1.size(),half);
    if(h4.empty()){ fprintf(stderr,"no h4\n"); return 1; }

    std::vector<Trade> trades;
    auto mkcb=[&](const std::vector<Bar>&){ return [&trades](const omega::TradeRecord& tr){
        Trade t; t.entryTs=tr.entryTs; t.exitTs=tr.exitTs;
        t.dir = (tr.side=="LONG")?+1:-1; t.entry_px=tr.entryPrice;
        trades.push_back(t);
    };};

    const std::vector<Bar>* barsFor = &h4;

    if(std::string(which)=="d1"){
        omega::XauTrendFollowD1Engine eng; eng.shadow_mode=true; eng.enabled=true;
        eng.lot=0.01; eng.max_spread=1.0; eng.init(); auto cb=mkcb(h4);
        for(size_t i=0;i<h4.size();++i){ const auto&b=h4[i]; int64_t ts=b.ts_sec*1000LL;
            omega::XauTfD1Bar bar; bar.bar_start_ms=ts; bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
            eng.on_h4_bar(bar,b.c-half,b.c+half,ts,cb);
            if(i+1<h4.size()){const auto&nb=h4[i+1];int64_t nt=nb.ts_sec*1000LL;
                eng.on_tick(nb.l-half,nb.l+half,nt,cb); eng.on_tick(nb.h-half,nb.h+half,nt,cb);} }
        barsFor=&h4;
    } else if(std::string(which)=="4h"){
        omega::XauTrendFollow4hEngine eng; eng.shadow_mode=true; eng.enabled=true;
        eng.cell_enable_mask=0x49; eng.lot=0.01; eng.max_spread=1.0; eng.init(); auto cb=mkcb(h4);
        for(size_t i=0;i<h4.size();++i){ const auto&b=h4[i]; int64_t ts=b.ts_sec*1000LL;
            omega::XauTfBar bar; bar.bar_start_ms=ts; bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
            eng.on_h4_bar(bar,b.c-half,b.c+half,0.0,ts,cb);
            if(i+1<h4.size()){const auto&nb=h4[i+1];int64_t nt=nb.ts_sec*1000LL;
                eng.on_tick(nb.l-half,nb.l+half,nt,cb); eng.on_tick(nb.h-half,nb.h+half,nt,cb);} }
        barsFor=&h4;
    } else if(std::string(which)=="2h"){
        omega::XauTrendFollow2hEngine eng; eng.shadow_mode=true; eng.enabled=true;
        eng.lot=0.01; eng.max_spread=1.0; eng.init(); auto cb=mkcb(h1);
        for(size_t i=0;i<h1.size();++i){ const auto&b=h1[i]; int64_t ts=b.ts_sec*1000LL;
            omega::XauTf2hBar bar; bar.bar_start_ms=ts; bar.open=b.o;bar.high=b.h;bar.low=b.l;bar.close=b.c;
            eng.on_h1_bar(bar,b.c-half,b.c+half,ts,cb);
            if(i+1<h1.size()){const auto&nb=h1[i+1];int64_t nt=nb.ts_sec*1000LL;
                eng.on_tick(nb.l-half,nb.l+half,nt,cb); eng.on_tick(nb.h-half,nb.h+half,nt,cb);} }
        barsFor=&h1;
    }

    fprintf(stderr,"%s: %zu trades\n",which,trades.size());
    printf("trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid=0; emit(stdout,tid,trades,*barsFor,half);
    return 0;
}
