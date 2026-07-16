// omega_becascade_bt.cpp — Omega port of the crypto self-trigger BE-cascade mimic (S-2026-07-16).
//
// PURPOSE: research-only effect table. Ports the validated crypto becascade
// (Crypto/backtest/eth_ujmimic_15_becascade_bt.cpp, ChimeraCrypto build dcb645e) to Omega
// symbols to SHOW its standalone effect. NOT wired, NOT deployed — operator chose
// "backtest table only, no wiring" (2026-07-16). This resurrects the shape hard-culled from
// Omega live on 2026-07-13; kept read-only pending operator sign-off.
//
// Byte-identical BE-cascade Config to the crypto harness run() (mimic_floor + stagger_mode=1 +
// stagger_be_bp=20 + reclip=0 + confirm_bp=0), driven through the SAME real engine header.
// Judged STANDALONE (own book, own cost) — companion is an independent additive book, NEVER
// vs a parent (feedback-companion-independent-engine).
//
// EDITS vs crypto source (only two, per the port recon):
//   1. Data path  -> env SYM_PATH (full path to an Omega H1 OHLC csv: ts,o,h,l,c).
//   2. Timestamp  -> Omega ts is epoch SECONDS; crypto engine wants ms. Normalize: ts<1e11 => *1000
//                    (same rule as backtest/data_integrity_gate.py).
// Per-symbol REAL round-trip cost passed via UM_RT (bp) — NOT the crypto 28bp proxy.
//
// Build: clang++ -std=c++17 -O2 -I/Users/jo/ChimeraCrypto/include omega_becascade_bt.cpp -o omega_becascade_bt
// Env:   SYM_PATH(req) SYM(label) UM_RT(bp) UM_THR(frac) UM_W(list) UM_G(list) UM_LEGS(list) UM_LOSSCUT(bp)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unistd.h>
#include <fcntl.h>
#include "core/UpJumpLadderCompanion.hpp"
using chimera::UpJumpLadderCompanion;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<std::string> split(const std::string&s){std::vector<std::string>v;std::stringstream ss(s);std::string t;while(std::getline(ss,t,','))v.push_back(t);return v;}
static std::vector<Bar> load(){
    const char* sp=getenv("SYM_PATH");
    std::vector<Bar> b; if(!sp){std::fprintf(stderr,"SYM_PATH not set\n");return b;}
    std::ifstream f(sp);
    if(!f){std::fprintf(stderr,"no data at %s\n",sp);return b;} std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){auto v=split(ln);if(v.size()<5)continue;
        Bar x;
        // tolerate header rows / bad lines
        char* end=nullptr; long double t0=std::strtold(v[0].c_str(),&end); if(end==v[0].c_str())continue;
        int64_t ts=(int64_t)t0;
        if(ts>=19000101 && ts<=21001231){ // YYYYMMDD calendar date (bigcap daily OHLC) -> epoch ms
            struct tm tmv{}; tmv.tm_year=(int)(ts/10000)-1900; tmv.tm_mon=(int)((ts/100)%100)-1; tmv.tm_mday=(int)(ts%100);
            tmv.tm_hour=0; ts=(int64_t)timegm(&tmv)*1000;
        } else if(ts<100000000000LL) ts*=1000; // Omega epoch seconds -> ms (data_integrity_gate.py L60 rule)
        x.ts=ts;
        x.o=std::stod(v[1]);x.h=std::stod(v[2]);x.l=std::stod(v[3]);x.c=std::stod(v[4]);
        if(x.c>0)b.push_back(x);} return b;
}
static int year_of(int64_t ms){time_t t=(time_t)(ms/1000);struct tm g;gmtime_r(&t,&g);return 1900+g.tm_year;}

struct Clip { int64_t ts; double net_bp; };
struct Agg { int n; double net,pf,worst,h1,h2,floormin; int neg; double negsum; double yr[8]; };
static int yidx(int y){int k=y-2021;return k<0?0:(k>7?7:k);}

static double g_losscut=0.0;
static int g_tf=3600; // UM_TF: bar timeframe in seconds (3600 H1 default; 86400 daily for stocks)
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt){
    UpJumpLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag="OMEGA-BC"; c.symbol="omega";
    c.det_w=W; c.det_thr=thr; c.tf_secs=g_tf; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=true; c.stagger_mode=1; c.stagger_be_bp=20.0;
    c.reclip_pct=0.0; c.loss_cut_bp=g_losscut; c.confirm_bp=0.0; c.be_floor=false;
    c.mimic_giveback=g;
    c.tight={0.2,0,0.0,0,0.0}; c.wide={0.2,0,0.0,0,0.0};
    for(int k=2;k<legs;k++) c.extra_base.push_back({0.2,0,0.0,0,0.0});
    c.cap=legs;

    std::vector<Clip> rows; int64_t cur=0;
    fflush(stdout); int saved=dup(1); {int dn=open("/dev/null",O_WRONLY);dup2(dn,1);close(dn);}
    UpJumpLadderCompanion eng(c);
    eng.set_on_clip([&](const UpJumpLadderCompanion::ClipRecord& r){ rows.push_back({cur, r.net_bp_real}); });
    for(const auto& k : b){ cur=k.ts; eng.stop_check_only(k.l,k.ts); eng.observe(true,0.0,k.c,k.ts); }
    if(!b.empty()){ cur=b.back().ts; eng.observe(false,0.0,b.back().c,b.back().ts+c.tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    std::sort(rows.begin(),rows.end(),[](const Clip&a,const Clip&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; a.floormin=1e18; for(int i=0;i<8;i++)a.yr[i]=0;
    for(size_t k=0;k<rows.size();k++){ double p=rows[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;a.negsum+=p/100.0;} if(p<a.worst)a.worst=p;
        if(p<a.floormin)a.floormin=p; a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0); if(rows.empty())a.floormin=0;
    return a;
}

int main(){
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):5.0;
    double thr= getenv("UM_THR")?atof(getenv("UM_THR")):0.005;
    g_losscut = getenv("UM_LOSSCUT")?atof(getenv("UM_LOSSCUT")):150.0;
    g_tf      = getenv("UM_TF")?atoi(getenv("UM_TF")):3600;
    const char* sym=getenv("SYM"); std::string label=sym?sym:"SYM";
    std::vector<int> Ws; { const char*e=getenv("UM_W"); std::string s=e?e:"4";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ws.push_back(atoi(t.c_str())); }
    std::vector<double> Gs; { const char*e=getenv("UM_G"); std::string s=e?e:"0.5";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Gs.push_back(atof(t.c_str())); }
    std::vector<int> Ls; { const char*e=getenv("UM_LEGS"); std::string s=e?e:"8";
        std::stringstream ss(s); std::string t; while(std::getline(ss,t,','))if(!t.empty())Ls.push_back(atoi(t.c_str())); }

    std::vector<Bar> b=load(); if(b.empty())return 1;
    std::printf("%s BE-CASCADE MIMIC (Omega port) — bars=%zu thr=%.2f%% RT=%.0f/%.0fbp lc=%.0f\n",
        label.c_str(), b.size(), thr*100, rt, rt*2, g_losscut);
    std::printf("%-3s %-4s %-4s | %6s %8s %5s %9s | %5s %8s | %8s %8s | %8s %8s | %6s %6s %6s %6s %6s %6s | %s\n",
        "W","g","leg","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","floorMinBp","2xnet%","2021","2022","2023","2024","2025","2026","GATE");
    for(int L:Ls)for(int W:Ws)for(double g:Gs){
        Agg a  = run(b,W,thr,g,L,rt);
        Agg a2 = run(b,W,thr,g,L,rt*2.0);
        double net_x22  = a.net  - a.yr[yidx(2022)];
        double net2_x22 = a2.net - a2.yr[yidx(2022)];
        bool gate = net_x22>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && net2_x22>0 && a2.pf>=1.3;
        std::printf("%-3d %-4.2f %-4d | %6d %+8.0f %5.2f %+9.1f | %5d %+8.1f | %+8.0f %+8.0f | %+8.0f %+8.0f | %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f %+6.0f | %s\n",
            W,g,L, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.h1,a.h2, a.floormin,a2.net,
            a.yr[0],a.yr[1],a.yr[2],a.yr[3],a.yr[4],a.yr[5], gate?"PASS":"fail");
    }
    return 0;
}
