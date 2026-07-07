// mgc_mirror_dump.cpp — parent-trade dumper for the MGC book engines
// (MgcFastDonchian30m deployed Nin40/skip + GoldVolBreakoutM30 on the MGC feed)
// for the S-2026-07-07t mirror dump-check (backtest/mirror_parents_bt.py).
// Drives the REAL engine classes over MGC 30m bars exactly as
// MgcFastDonchianFeed.hpp drives them live (on_30m_bar / on_m30_bar + H1 agg).
//
// usage: mgc_mirror_dump <mgc_30m_csv> <out_csv>
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/mgc_mirror_dump.cpp -o /tmp/mgc_mirror_dump
#include "MgcFastDonchian30mEngine.hpp"
#include "GoldVolBreakoutM30Engine.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Row{ long long ts; double o,h,l,c,v; };

int main(int argc,char**argv){
    if(argc<3){ std::fprintf(stderr,"usage: %s <mgc_30m_csv> <out_csv>\n",argv[0]); return 1; }
    std::ifstream f(argv[1]); if(!f){ std::fprintf(stderr,"no %s\n",argv[1]); return 1; }
    std::vector<Row> rows; std::string ln; bool fst=true;
    while(std::getline(f,ln)){ if(fst){fst=false;continue;}
        std::stringstream s(ln); std::string t; std::vector<std::string> k;
        while(std::getline(s,t,',')) k.push_back(t);
        if(k.size()<6) continue;
        Row r{std::atoll(k[0].c_str()),std::atof(k[1].c_str()),std::atof(k[2].c_str()),
              std::atof(k[3].c_str()),std::atof(k[4].c_str()),std::atof(k[5].c_str())};
        if(r.h>0) rows.push_back(r);
    }
    std::fprintf(stderr,"[mgc] %zu 30m bars\n",rows.size());

    FILE* fo=fopen(argv[2],"a");
    const bool fresh = ftell(fo)==0;
    if(fresh) std::fprintf(fo,"entryTs,symbol,side,engine,entryPrice,exitPrice,pnl,mfe,mae,hold_sec,exitReason\n");
    auto mk=[&](const char* eng){
        return [fo,eng](const omega::TradeRecord& tr){
            std::fprintf(fo,"%lld,MGC,%s,%s,%.5f,%.5f,%.4f,0,0,%lld,%s\n",
                (long long)tr.entryTs,tr.side.c_str(),eng,tr.entryPrice,tr.exitPrice,
                tr.pnl,(long long)(tr.exitTs-tr.entryTs),tr.exitReason.c_str());
        };
    };

    // MgcFastDonchian30m — deployed config (omega_main: Nin40/Nout20/Kov1.5/hvn_skip)
    omega::MgcFastDonchian30mEngine fd;
    fd.enabled=true; fd.shadow_mode=true; fd.lot=1.0;
    fd.Nin=40; fd.Nout=20; fd.Kov=1.5; fd.use_hvn_skip=true;
    auto fd_cb=mk("MgcFastDonchian30m");

    // GoldVolBreakoutM30 on MGC — live drive pattern (H1 agg from 30m buckets)
    omega::GoldVolBreakoutM30Engine vb;
    vb.enabled=true; vb.shadow_mode=true; vb.lot=0.01; vb.max_spread=1.50; vb.init();
    auto vb_cb=mk("MgcVolBrkM30");
    int64_t vb_h1_bucket=0; double vb_h1_close=0.0;

    for(const auto& r: rows){
        fd.on_30m_bar(r.o,r.h,r.l,r.c,r.v,r.ts,fd_cb);
        const int64_t h1b=(r.ts/3600)*3600;
        if(vb_h1_bucket!=0 && h1b!=vb_h1_bucket) vb.on_h1_close(vb_h1_close);
        vb_h1_bucket=h1b; vb_h1_close=r.c;
        vb.on_m30_bar(r.h,r.l,r.c,r.c,r.c,r.ts,vb_cb);
    }
    fclose(fo);
    std::fprintf(stderr,"wrote %s\n",argv[2]);
    return 0;
}
