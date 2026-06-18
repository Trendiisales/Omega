// straddle_tf_audit.cpp — FAITHFUL TF re-check of the REAL XauStraddleM30Engine.
// Cull-audit: "XauStraddleM15" was tombstoned UNEVALUABLE. The M30 sibling is the
// known survivor (PF1.65 2yr). Question: does the straddle hold at M15, or die on
// cost like the M5 did (thousand-cuts)? Drives the production engine via its own
// self-aggregation path on_tick_agg(tf_min) — fully faithful (box + intrabar SL/TP
// internal). Feeds 2yr XAUUSD tick. NOT the inline straddle_breakout_sweep reimpl.
//
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/sttf backtest/straddle_tf_audit.cpp
// run:   /tmp/sttf <xau_tick.csv> <tf_min> [cost_rt_pts=0.37]
//   tick format: ts,ask,bid  (combined dataset is ASK-FIRST)
#include "XauStraddleM30Engine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

static int yr(int64_t sec){ time_t t=sec; struct tm g; gmtime_r(&t,&g); return g.tm_year+1900; }

int main(int argc, char** argv){
    if(argc<3){ fprintf(stderr,"usage: %s <tick.csv> <tf_min> [cost]\n",argv[0]); return 2; }
    const char* path=argv[1]; const int tf=atoi(argv[2]);
    const double cost=argc>3?atof(argv[3]):0.37;
    std::ifstream f(path); if(!f){ fprintf(stderr,"open fail %s\n",path); return 1; }

    omega::XauStraddleM30Engine eng;
    eng.shadow_mode=true; eng.enabled=true; eng.symbol="XAUUSD";
    eng.box_n=15; eng.stop_atr=3.0; eng.tp_r=1.0; eng.max_spread=1.0; eng.lot=0.01;
    eng.partial_frac=0.30; eng.partial_r=0.7; eng.obi_tilt=false;
    eng.tf_min=tf; eng.engine_name = (tf==30?"XauStraddleM30":"XauStraddleM15");

    struct Tr{ int64_t ts; double pnl; };
    std::vector<Tr> trs;
    auto cb=[&](const omega::TradeRecord& tr){ trs.push_back({tr.exitTs, tr.pnl}); };

    std::string line; long nt=0;
    while(std::getline(f,line)){
        if(line.size()<8) continue;
        char* p=line.data();
        // ts,ask,bid  (ASK FIRST)
        long long ts=strtoll(p,&p,10); if(*p!=',')continue; ++p;
        double ask=strtod(p,&p); if(*p!=',')continue; ++p;
        double bid=strtod(p,&p);
        if(bid<=0||ask<=0) continue;
        int64_t ms = ts>1000000000000LL ? ts : ts*1000LL;
        eng.on_tick_agg(bid,ask,ms,cb);
        ++nt;
    }
    fprintf(stderr,"# %ld ticks, %zu trades, tf=%dm\n", nt, trs.size(), tf);

    // stats (cost per fully-closed trade; partials are fractional pnl rows too — cost on each fill)
    auto stats=[&](long y0,long y1,double& net,long& n,double& pf,double& wr){
        double gw=0,gl=0; long w=0; net=0; n=0;
        for(auto&t:trs){ int y=yr(t.ts); if(y<y0||y>y1) continue;
            double v=t.pnl - cost*eng.lot;  // approx RT cost per fill in pts*lot
            net+=v; ++n; if(v>0){gw+=v;++w;} else gl+=-v; }
        pf = gl>0?gw/gl:(gw>0?999:0); wr=n?100.0*w/n:0;
    };
    double net; long n; double pf,wr;
    printf("\n=== XauStraddle tf=%dm FAITHFUL (real engine, box15/stop3/tp1R/partial30@0.7R, cost=%.2f) ===\n",tf,cost);
    for(int y=2024;y<=2026;++y){ stats(y,y,net,n,pf,wr); printf("  %d   n=%4ld WR=%.0f%% PF=%.2f net=%+.1f\n",y,n,wr,pf,net); }
    // WF halves by trade order
    double h1=0,h2=0; for(size_t i=0;i<trs.size();++i){ double v=trs[i].pnl-cost*eng.lot; (i<trs.size()/2?h1:h2)+=v; }
    stats(2024,2026,net,n,pf,wr);
    printf("  ALL  n=%4ld WR=%.0f%% PF=%.2f net=%+.1f   WF-H1=%+.1f H2=%+.1f %s\n",
           n,wr,pf,net,h1,h2,(h1>0&&h2>0)?"BOTH+":"NOT both+");
    return 0;
}
