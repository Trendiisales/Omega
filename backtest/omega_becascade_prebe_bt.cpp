// omega_becascade_prebe_bt.cpp — NEVER-PRE-BE-LOSS validation for the Omega BE-CASCADE book.
//
// Drives the OMEGA header (include/BeCascadeCompanionEngine.hpp, namespace chimera) — NOT the
// crypto sibling — so it validates exactly what Omega live will book. For every cell it runs the
// live config TWO ways and prints them side by side:
//   OFF  = current live: confirm_bp=0, confirm_anchor_epx=false, loss_cut_bp=150 (PREBE_CUT path)
//   ON   = the fix:      confirm_bp=CONF (>RT), confirm_anchor_epx=true, loss_cut_bp=0 (floor-on-open)
// Everything else identical to OmegaBeCascadeBook::add_cell (mimic_floor + mimic_stagger +
// stagger_mode=1 + stagger_be_bp=20 + reclip=0 + g + legs).
//
// GOAL: prove ON gives worst-clip >= 0 (nNeg=0) AND stays standalone net-positive after cost,
// WF both halves, omit-2022, at base + 2x cost. Companion judged STANDALONE (own book, own cost),
// never vs a parent (feedback-companion-independent-engine).
//
// Build: g++ -std=c++17 -O2 -Iinclude backtest/omega_becascade_prebe_bt.cpp -o backtest/omega_becascade_prebe_bt
// Env:   SYM_PATH(req) SYM(label) UM_RT(bp) UM_THR(frac) UM_W UM_G UM_LEGS UM_TF UM_CONF(bp, ON confirm)
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
#include "BeCascadeCompanionEngine.hpp"
using chimera::MimicLadderCompanion;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<std::string> split(const std::string&s){std::vector<std::string>v;std::stringstream ss(s);std::string t;while(std::getline(ss,t,','))v.push_back(t);return v;}
static std::vector<Bar> load(){
    const char* sp=getenv("SYM_PATH");
    std::vector<Bar> b; if(!sp){std::fprintf(stderr,"SYM_PATH not set\n");return b;}
    std::ifstream f(sp);
    if(!f){std::fprintf(stderr,"no data at %s\n",sp);return b;} std::string ln; std::getline(f,ln);
    while(std::getline(f,ln)){auto v=split(ln);if(v.size()<5)continue;
        Bar x;
        char* end=nullptr; long double t0=std::strtold(v[0].c_str(),&end); if(end==v[0].c_str())continue;
        int64_t ts=(int64_t)t0;
        if(ts>=19000101 && ts<=21001231){ // YYYYMMDD -> epoch ms
            struct tm tmv{}; tmv.tm_year=(int)(ts/10000)-1900; tmv.tm_mon=(int)((ts/100)%100)-1; tmv.tm_mday=(int)(ts%100);
            tmv.tm_hour=0; ts=(int64_t)timegm(&tmv)*1000;
        } else if(ts<100000000000LL) ts*=1000;
        x.ts=ts;
        x.o=std::stod(v[1]);x.h=std::stod(v[2]);x.l=std::stod(v[3]);x.c=std::stod(v[4]);
        if(x.c>0)b.push_back(x);} return b;
}
static int year_of(int64_t ms){time_t t=(time_t)(ms/1000);struct tm g;gmtime_r(&t,&g);return 1900+g.tm_year;}

struct Clip { int64_t ts; double net_bp; };
struct Agg { int n; double net,pf,worst,h1,h2; int neg; double negsum; double yr[8]; };
static int yidx(int y){int k=y-2021;return k<0?0:(k>7?7:k);}

static int    g_tf=3600;
static double g_stagbe=20.0;

// on=false -> current live (confirm 0, anchor off, lc150). on=true -> fix (confirm CONF, anchor on, lc0).
static Agg run(const std::vector<Bar>& b, int W, double thr, double g, int legs, double rt,
               bool on, double conf){
    MimicLadderCompanion::Config c;
    c.parent_tag="SELF"; c.tag="OMEGA-BC"; c.symbol="omega";
    c.det_w=W; c.det_thr=thr; c.tf_secs=g_tf; c.round_trip_bp=rt;
    c.mimic_floor=true; c.mimic_stagger=true; c.stagger_mode=1; c.stagger_be_bp=g_stagbe;
    c.reclip_pct=0.0; c.be_floor=false; c.mimic_giveback=g;
    if(on){ c.confirm_bp=conf; c.confirm_anchor_epx=true; c.loss_cut_bp=0.0; }
    else  { c.confirm_bp=0.0;  c.confirm_anchor_epx=false; c.loss_cut_bp=150.0; }
    c.tight={0.2,0,0.0,0,0.0}; c.wide={0.2,0,0.0,0,0.0};
    for(int k=2;k<legs;k++) c.extra_base.push_back({0.2,0,0.0,0,0.0});
    c.cap=legs;

    std::vector<Clip> rows; int64_t cur=0;
    fflush(stdout); int saved=dup(1); {int dn=open("/dev/null",O_WRONLY);dup2(dn,1);close(dn);}
    MimicLadderCompanion eng(c);
    eng.set_on_clip([&](const MimicLadderCompanion::ClipRecord& r){ rows.push_back({cur, r.net_bp_real}); });
    for(const auto& k : b){ cur=k.ts; eng.stop_check_only(k.l,k.ts); eng.observe(true,0.0,k.c,k.ts); }
    if(!b.empty()){ cur=b.back().ts; eng.observe(false,0.0,b.back().c,b.back().ts+c.tf_secs*1000+2000); }
    fflush(stdout); dup2(saved,1); close(saved);

    std::sort(rows.begin(),rows.end(),[](const Clip&a,const Clip&b){return a.ts<b.ts;});
    Agg a{}; double gw=0,gl=0; for(int i=0;i<8;i++)a.yr[i]=0;
    for(size_t k=0;k<rows.size();k++){ double p=rows[k].net_bp; a.net+=p/100.0;
        if(p>0)gw+=p; else{gl-=p;a.neg++;a.negsum+=p/100.0;} if(p<a.worst)a.worst=p;
        a.yr[yidx(year_of(rows[k].ts))]+=p/100.0;
        if(k<rows.size()/2)a.h1+=p/100.0; else a.h2+=p/100.0; }
    a.n=(int)rows.size(); a.pf=gl>0?gw/gl:(gw>0?999:0);
    return a;
}

static void line(const char* tag, const Agg& a, const Agg& a2){
    double net_x22  = a.net  - a.yr[yidx(2022)];
    double net2_x22 = a2.net - a2.yr[yidx(2022)];
    // GATE: never-neg (worst>=0), net-positive omit-2022 both halves both costs, PF>=1.3
    bool nneg = (a.neg==0);
    bool gate = nneg && net_x22>0 && a.pf>=1.3 && a.h1>0 && a.h2>0 && net2_x22>0 && a2.pf>=1.3;
    std::printf("  %-4s | %6d %+9.0f %6.2f %+10.2f | %5d %+9.2f | %+9.0f %+9.0f | %+9.0f | %s\n",
        tag, a.n,a.net,a.pf,a.worst, a.neg,a.negsum, a.h1,a.h2, a2.net, gate?"PASS":(nneg?"nneg":"NEG!"));
}

int main(){
    double rt = getenv("UM_RT")?atof(getenv("UM_RT")):5.0;
    double thr= getenv("UM_THR")?atof(getenv("UM_THR")):0.005;
    double g  = getenv("UM_G")?atof(getenv("UM_G")):0.5;
    int    W  = getenv("UM_W")?atoi(getenv("UM_W")):4;
    int    L  = getenv("UM_LEGS")?atoi(getenv("UM_LEGS")):8;
    g_tf      = getenv("UM_TF")?atoi(getenv("UM_TF")):3600;
    double conf = getenv("UM_CONF")?atof(getenv("UM_CONF")):(rt*3.0);  // ON confirm bp (default 3x RT)
    const char* sym=getenv("SYM"); std::string label=sym?sym:"SYM";

    std::vector<Bar> b=load(); if(b.empty()){std::fprintf(stderr,"%s: no bars\n",label.c_str());return 1;}
    std::printf("== %s  bars=%zu W=%d thr=%.3f%% RT=%.0f/%.0fbp g=%.2f legs=%d conf(ON)=%.0fbp ==\n",
        label.c_str(), b.size(), W, thr*100, rt, rt*2, g, L, conf);
    std::printf("  %-4s | %6s %9s %6s %10s | %5s %9s | %9s %9s | %9s | %s\n",
        "cfg","n","net%","PF","worst_bp","nNeg","sumNeg%","H1%","H2%","2xnet%","GATE");
    Agg off  = run(b,W,thr,g,L,rt, false,conf);
    Agg off2 = run(b,W,thr,g,L,rt*2, false,conf);
    Agg on   = run(b,W,thr,g,L,rt, true, conf);
    Agg on2  = run(b,W,thr,g,L,rt*2, true, conf);
    line("OFF", off, off2);
    line("ON",  on,  on2);
    return 0;
}
