// clip_path_ext_mimic.cpp — S-2026-07-17k roster-extension mimic certification.
//
// Validates the 4 SHIPPED DIP mimic cells (T/W/X/Y) on the DIP ext-11 names and
// the 4 SHIPPED TURTLE mimic cells (A/B/C/D) on the TURTLE ext-14 names, on the
// REAL production header GoldTrendMimicBook (include/GoldTrendMimicLadder.hpp),
// with the operator's EXACT floored spec (feedback-test-operator-spec-before-verdict):
//   be_entry_pct=0.08 (>= rt 8bp) + no_prebe_loss=true (BE-ENTRY, floored on open)
//   + arm/gb/lc/cap from the shipped engine_init cells + close-only bars (c,c,c —
//   the live sp500_long_close.csv feed is CLOSE-ONLY).
//
// ENTRY CADENCE = FAITHFUL LIVE (flat-gated): the live mimic fires ONLY when the
// real StockDipTurtleSym book OPENS, and that book is flat-gated (no re-entry while
// in position; DIP exits SMA5-bounce/10d, TURTLE exits 10-close-low). Both entry
// generators replicate that state machine exactly (verify-kill-replicates-mechanism).
// This is stricter/denser-honest than the prior mimic_cell_sweep DIP convention
// (which fired every signal day).
//
// Verdict per family: pooled all-6 STANDALONE (net>0, PF>=1.3, WF both halves,
// bull+ & bear+ (SMA200 self-regime), calendar-2022+ when n>0) at 8bp RT — the
// live cells' validated cost basis — plus a 2x-cost (16bp) stress line.
// Judged STANDALONE (feedback-companion-independent-engine) — never vs parent/WIDE.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
#include "GoldTrendMimicLadder.hpp"

using omega::GoldTrendMimicBook;

struct Bar { int64_t ts; double o,h,l,c; };
static std::vector<Bar> load_bars(const std::string& path){
    std::vector<Bar> v; std::ifstream f(path); if(!f){fprintf(stderr,"MISSING %s\n",path.c_str());return v;}
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){
        if(line.empty())continue; Bar b; std::vector<std::string> p; std::string t; std::stringstream ss(line);
        while(std::getline(ss,t,',')) p.push_back(t);
        if(p.size()<5) continue;
        // bigcap_daily_ohlc rows: YYYY-MM-DD,o,h,l,c
        int y=0,m=0,d=0;
        if(std::sscanf(p[0].c_str(),"%d-%d-%d",&y,&m,&d)==3){
            struct tm tmv; std::memset(&tmv,0,sizeof tmv);
            tmv.tm_year=y-1900; tmv.tm_mon=m-1; tmv.tm_mday=d;
            b.ts=(int64_t)timegm(&tmv);
        } else b.ts=std::strtoll(p[0].c_str(),nullptr,10);
        b.o=std::strtod(p[1].c_str(),nullptr); b.h=std::strtod(p[2].c_str(),nullptr);
        b.l=std::strtod(p[3].c_str(),nullptr); b.c=std::strtod(p[4].c_str(),nullptr);
        if(b.c>0) v.push_back(b);
    }
    return v;
}
static int year_of(int64_t ts){ time_t t=(time_t)ts; struct tm* g=gmtime(&t); return g?g->tm_year+1900:0; }
static std::vector<char> bull_vec(const std::vector<Bar>&B){       // close>=SMA200 (self-regime)
    int N=(int)B.size(); std::vector<char> v(N,1); double sum=0;
    std::vector<double> s(N,0);
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=200)sum-=B[i-200].c; s[i]= i>=199? sum/200.0 : B[i].c; }
    for(int i=0;i<N;i++) v[i]=(i>=200 && B[i].c<s[i])?0:1;
    return v;
}

struct Entry { int idx; int dir; double px; };

// FAITHFUL StockDipTurtleSym DIP state machine (flat-gated): enter LONG at close when
// FLAT & close>SMA200(prior 200) & CutlerRSI2(incl)<10; exit first close>SMA5(incl,
// same-day allowed after entry day) or 10 trading days. Matches live_step_ exactly
// (in-position -> exit check ONLY; else entry check; no same-day exit->re-entry).
static std::vector<Entry> dip_entries_gated(const std::vector<Bar>&B){
    std::vector<Entry> e; int N=(int)B.size(); int ei=-1; int held=0;
    for(int i=0;i<N;i++){
        if(ei>=0){
            held+=1;
            double s5=0; if(i>=4){ for(int k=i-4;k<=i;k++)s5+=B[k].c; s5/=5.0; }
            if((s5>0 && B[i].c>s5) || held>=10){ ei=-1; held=0; }
        } else if(i>=201){
            double sma=0; for(int k=i-200;k<i;k++)sma+=B[k].c; sma/=200.0;
            double g=0,l=0; for(int k=i-1;k<=i;k++){ double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch; }
            double rsi=(l==0)?100.0:100.0-100.0/(1.0+g/l);
            if(B[i].c>sma && rsi<10.0){ e.push_back({i,+1,B[i].c}); ei=i; held=0; }
        }
    }
    return e;
}
// FAITHFUL StockDipTurtleSym TURTLE state machine (flat-gated): enter LONG at close
// when FLAT & close>max(prior 20 closes); exit first close<min(prior 10 closes).
static std::vector<Entry> turtle_entries_gated(const std::vector<Bar>&B){
    std::vector<Entry> e; int N=(int)B.size(); bool inp=false;
    for(int i=0;i<N;i++){
        if(i<21) continue;
        if(!inp){
            double mx=0; for(int k=i-20;k<i;k++)mx=std::max(mx,B[k].c);
            if(B[i].c>mx){ e.push_back({i,+1,B[i].c}); inp=true; }
        } else {
            double mn=1e18; for(int k=i-10;k<i;k++)mn=std::min(mn,B[k].c);
            if(B[i].c<mn) inp=false;
        }
    }
    return e;
}

struct Clip { double gross, net; int64_t ets, xts; std::string reason; char bull; int year; };

static std::vector<Clip> run_cell(GoldTrendMimicBook::Config cfg, const std::vector<Entry>& ents,
                                  const std::vector<Bar>& B, const std::vector<char>& bull,
                                  double rt_bp, int uid){
    cfg.no_prebe_loss = true;
    cfg.be_entry_pct  = 0.08;
    cfg.pre_arm_be_pct= 0.0;    // inert under BE-ENTRY (floor precedes) — matches the re-val convention
    cfg.rt_cost_bp    = rt_bp;
    char sp[256], cp[256];
    std::snprintf(sp,sizeof sp,"/tmp/extm/state_%d.txt",uid);
    std::snprintf(cp,sizeof cp,"/tmp/extm/closed_%d.csv",uid);
    std::remove(sp); std::remove(cp); std::string spo=std::string(sp)+".open"; std::remove(spo.c_str());
    cfg.state_path=sp; cfg.closed_path=cp;
    GoldTrendMimicBook book(cfg);
    std::vector<Clip> clips;
    double rt=rt_bp/100.0;   // % cost
    auto bull_at=[&](int64_t ts)->char{ int lo=0,hi=(int)B.size()-1,r=0; while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;} return bull[r]; };
    book.set_exec(nullptr,nullptr,nullptr,
        [&](const std::string&,const std::string&,bool is_long,double entry,double fill,double,
            int64_t ets,int64_t xts,const char* reason,double,double){
            double dir=is_long?1.0:-1.0;
            double gross=dir*(fill/entry-1.0)*100.0;
            clips.push_back({gross, gross-rt, ets, xts, reason, bull_at(xts), year_of(xts)});
        });
    std::map<int,std::vector<Entry>> byidx; for(auto&e:ents) byidx[e.idx].push_back(e);
    for(size_t i=0;i<B.size();++i){
        const Bar& b=B[i];
        book.on_h1_bar(b.c,b.c,b.c,b.ts,bull[i],true);   // CLOSE-ONLY (live feed fidelity)
        auto it=byidx.find((int)i);
        if(it!=byidx.end()) for(auto&e:it->second) book.on_trend_open(e.dir,e.px,b.ts);
    }
    std::remove(sp); std::remove(cp); std::remove(spo.c_str());
    return clips;
}

struct Stat{ int n=0; double net=0,pf=0,worstg=1e9,worstn=1e9; int nneg_g=0,nneg_n=0,wins=0; double mdd=0; };
static Stat stats(const std::vector<Clip>& c){
    Stat s; s.n=(int)c.size(); if(!s.n){s.worstg=s.worstn=0;return s;}
    double gp=0,gn=0; for(auto&x:c){ s.net+=x.net; if(x.net>0)gp+=x.net; else gn+=-x.net;
        if(x.net>1e-9)s.wins++; s.worstg=std::min(s.worstg,x.gross); s.worstn=std::min(s.worstn,x.net);
        if(x.gross<-1e-9)s.nneg_g++; if(x.net<-1e-9)s.nneg_n++; }
    s.pf = gn>1e-9? gp/gn : 1e9;
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&a,const Clip&b){return a.xts<b.xts;});
    double cum=0,pk=0; for(auto&x:sc){ cum+=x.net; if(cum>pk)pk=cum; if(cum-pk<s.mdd)s.mdd=cum-pk; }
    return s;
}
static std::vector<Clip> filt_year(const std::vector<Clip>&c,int y){ std::vector<Clip> r; for(auto&x:c) if(x.year==y) r.push_back(x); return r; }
static std::vector<Clip> filt_bull(const std::vector<Clip>&c,char b){ std::vector<Clip> r; for(auto&x:c) if(x.bull==b) r.push_back(x); return r; }
static void halves(const std::vector<Clip>&c,std::vector<Clip>&a,std::vector<Clip>&b){
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&x,const Clip&y){return x.xts<y.xts;});
    size_t mid=sc.size()/2; a.assign(sc.begin(),sc.begin()+mid); b.assign(sc.begin()+mid,sc.end());
}

struct SFam{ std::string fam; std::string sig; double arm,lc,gb; };

int main(){
    if(!freopen("/dev/null","w",stdout)){}
    system("mkdir -p /tmp/extm");
    const std::string SD="/Users/jo/Omega/backtest/data/bigcap_daily_ohlc/";
    // S-2026-07-17k scan PASS sets (BIGCAP45_ENGINE_SCAN_FINDINGS_2026-07-17.md)
    const char* EXT_DIP[] = {"MRVL","PLTR","META","NFLX","SHOP","MSTR","NOW","AMZN","GOOGL","WDC","DD"};
    const char* EXT_TUR[] = {"MU","MRVL","PLTR","TSLA","META","NFLX","CRWD","PANW","DELL","AMZN","GOOGL","ASTS","RKLB","WDC"};
    const int N_DIP=11, N_TUR=14;
    // shipped engine_init cell specs (S-2026-07-16l/p, be_entry .08 + no_prebe added 17)
    std::vector<SFam> dipfam={{"T","dip",2.0,2.0,0.50},{"W","dip",3.0,3.0,0.70},{"X","dip",1.5,1.5,0.70},{"Y","dip",2.5,2.5,0.60}};
    std::vector<SFam> turfam={{"A","turtle",1.5,1.5,0.50},{"B","turtle",2.0,2.0,0.50},{"C","turtle",2.5,2.5,0.40},{"D","turtle",3.5,3.5,0.40}};

    int uid=1;
    auto run_set=[&](const char* title,const std::vector<SFam>& fams,const char** names,int nn){
        std::fprintf(stderr,"\n=== %s (real header, CLOSE-ONLY, flat-gated live cadence, be.08+no_prebe) ===\n",title);
        for(const auto&F:fams){
            for(double rt : {8.0, 16.0}){
                std::vector<Clip> pool; int tot_ents=0;
                std::vector<std::pair<std::string,double>> pername;
                for(int k=0;k<nn;k++){
                    auto B=load_bars(SD+names[k]+".csv"); if(B.empty())continue;
                    auto bl=bull_vec(B);
                    auto ents=(F.sig=="dip")?dip_entries_gated(B):turtle_entries_gated(B);
                    if(ents.empty())continue;
                    tot_ents+=(int)ents.size();
                    GoldTrendMimicBook::Config c; c.trigger_tag=std::string("EXT")+F.fam+"_"+names[k]; c.live_sym=names[k];
                    c.legs={{"",F.gb}}; c.arm_pct=F.arm; c.lc_pct=F.lc; c.cap_bars=10;
                    c.notional=10000; c.pend_bars=6; c.bull_only=false;
                    auto cl=run_cell(c,ents,B,bl,rt,uid++);
                    double nsum=0; for(auto&x:cl){pool.push_back(x); nsum+=x.net;}
                    pername.push_back({names[k],nsum});
                }
                Stat on=stats(pool);
                std::vector<Clip> h1,h2; halves(pool,h1,h2); auto s1=stats(h1),s2=stats(h2);
                Stat sb=stats(filt_bull(pool,1)), sr=stats(filt_bull(pool,0)), y22=stats(filt_year(pool,2022));
                bool all6 = on.net>0 && on.pf>=1.3 && s1.net>0 && s2.net>0 && sb.net>0 && (sr.n==0||sr.net>0) && (y22.n==0||y22.net>0);
                char pfb[16]; if(on.pf>=1e8)snprintf(pfb,sizeof pfb,"inf"); else snprintf(pfb,sizeof pfb,"%.2f",on.pf);
                std::fprintf(stderr,"  %s rt=%2.0fbp ent=%4d clips=%4d net=%+8.1f wc(g)=%+6.2f nNeg(g/n)=%d/%d PF=%s mdd=%+7.1f | H1%+7.0f H2%+7.0f bull%+7.0f bear%+7.0f y22%+7.0f | %s\n",
                    F.fam.c_str(), rt, tot_ents, on.n, on.net, on.worstg, on.nneg_g, on.nneg_n, pfb, on.mdd,
                    s1.net,s2.net,sb.net,sr.net,y22.net, all6?"ALL6-PASS":"FAIL");
                if(rt==8.0){
                    std::fprintf(stderr,"      per-name net: ");
                    for(auto&pn:pername) std::fprintf(stderr,"%s%+.0f ",pn.first.c_str(),pn.second);
                    std::fprintf(stderr,"\n");
                }
            }
        }
    };
    run_set("DIP ext-11 MIMIC CELLS T/W/X/Y",dipfam,EXT_DIP,N_DIP);
    run_set("TURTLE ext-14 MIMIC CELLS A/B/C/D",turfam,EXT_TUR,N_TUR);
    return 0;
}
