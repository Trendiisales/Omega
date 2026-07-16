// clip_path_noprebe_floor.cpp — validate the NO-PRE-BE-LOSS floor on the REAL
// production header GoldTrendMimicBook (include/GoldTrendMimicLadder.hpp).
//
// Drives the ACTUAL engine class (no re-implementation of the clip mechanics)
// over real per-cell entries + the real bar stream, with cfg_.no_prebe_loss
// toggled OFF vs ON. For every cell it reports: all-6 STANDALONE verdict
// (net+/PF/WF-both-halves/regime-both/2022 or bull-bear), the worst BOOKED
// gross clip, and nNeg = # of clips booked at gross<0. The floor's contract
// is that with be_entry_pct>0 + no_prebe_loss=true NO clip can book gross<0.
//
// Entries:
//   gold  : real trend-engine entries (clip_path_xau_tf seq0 rows) OR a faithful
//           Donchian-N breakout reconstruction on the native TF (labelled REcon).
//   stock : faithful StockDip DIP (close>SMA200 & RSI2<10 Cutler) / StockTurtle
//           (close>max prior 20 closes) LONG entries — exact port of the two
//           StockDipTurtleSym signals. StockDip mimic is fed CLOSE-ONLY bars in
//           production (on_bar(close,close,close)); gold is fed intrabar (h,l,c).
//
// Cost: net = gross - rt_cost_bp/100 (%). worst-clip / nNeg are GROSS (booked
// return level) — the be_entry_pct>=rt_cost buffer covers the round-trip cost so
// a gross-BE exit is >= break-even in the trigger frame.
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
    std::string line; std::getline(f,line);            // header
    while(std::getline(f,line)){
        if(line.empty())continue; Bar b; std::vector<std::string> p; std::string t; std::stringstream ss(line);
        while(std::getline(ss,t,',')) p.push_back(t);
        if(p.size()<5) continue;
        b.ts=std::strtoll(p[0].c_str(),nullptr,10);
        if(b.ts>0 && b.ts<30000000LL){                 // YYYYMMDD -> epoch (unify ts space)
            struct tm tmv; std::memset(&tmv,0,sizeof tmv);
            tmv.tm_year=(int)(b.ts/10000)-1900; tmv.tm_mon=(int)((b.ts/100)%100)-1; tmv.tm_mday=(int)(b.ts%100);
            b.ts=(int64_t)timegm(&tmv);
        }
        b.o=std::strtod(p[1].c_str(),nullptr); b.h=std::strtod(p[2].c_str(),nullptr);
        b.l=std::strtod(p[3].c_str(),nullptr); b.c=std::strtod(p[4].c_str(),nullptr);
        if(b.c>0) v.push_back(b);
    }
    return v;
}
static int year_of(int64_t ts){
    if(ts>30000000LL){ time_t t=(time_t)ts; struct tm* g=gmtime(&t); return g?g->tm_year+1900:0; }
    return (int)(ts/10000);   // YYYYMMDD
}
static std::vector<double> sma_closes(const std::vector<Bar>&B,int n){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=n) sum-=B[i-n].c; s[i]= i>=n-1? sum/n : B[i].c; }
    return s;
}
static std::vector<char> bull_vec(const std::vector<Bar>&B){       // close>=SMA200
    auto s=sma_closes(B,200); std::vector<char> v(B.size(),1);
    for(size_t i=0;i<B.size();++i) v[i]= (i>=200 && B[i].c < s[i]) ? 0 : 1;
    return v;
}

struct Entry { int idx; int dir; double px; };

// StockDip: close>SMA200(prior 200) AND Cutler RSI2<10 -> LONG at close.
static std::vector<Entry> dip_entries(const std::vector<Bar>&B){
    std::vector<Entry> e; int N=B.size();
    for(int i=0;i<N;i++){
        if(i<201) continue;
        double sma=0; for(int k=i-200;k<i;k++) sma+=B[k].c; sma/=200.0;
        double g=0,l=0; for(int k=i-1;k<=i;k++){ double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch; }
        double rsi = (l==0)?100.0 : 100.0-100.0/(1.0+g/l);
        if(B[i].c>sma && rsi<10.0) e.push_back({i,+1,B[i].c});
    }
    return e;
}
// StockTurtle: close>max(prior 20 closes) -> LONG at close.
static std::vector<Entry> turtle_entries(const std::vector<Bar>&B){
    std::vector<Entry> e; int N=B.size();
    for(int i=0;i<N;i++){ if(i<21) continue; double mx=0; for(int k=i-20;k<i;k++) mx=std::max(mx,B[k].c);
        if(B[i].c>mx) e.push_back({i,+1,B[i].c}); }
    return e;
}
// Donchian-N upper breakout on highs (faithful gold trend-entry recon): close>max(prior N highs).
static std::vector<Entry> donch_entries(const std::vector<Bar>&B,int n){
    std::vector<Entry> e; int N=B.size();
    for(int i=0;i<N;i++){ if(i<n+1) continue; double mx=0; for(int k=i-n;k<i;k++) mx=std::max(mx,B[k].h);
        if(B[i].c>mx) e.push_back({i,+1,B[i].c}); }
    return e;
}
// Real entries from a clip_path seq0 dump: "entry_ts_sec,dir,entry_px". Matched to nearest bar idx (ts<=).
static std::vector<Entry> read_real_entries(const std::string& path,const std::vector<Bar>&B){
    std::vector<Entry> e; std::ifstream f(path); if(!f){fprintf(stderr,"MISSING entries %s\n",path.c_str());return e;}
    std::string line;
    while(std::getline(f,line)){ if(line.empty())continue; std::vector<std::string>p; std::string t; std::stringstream ss(line);
        while(std::getline(ss,t,',')) p.push_back(t); if(p.size()<3) continue;
        int64_t ts=std::strtoll(p[0].c_str(),nullptr,10); int dir=std::atoi(p[1].c_str()); double px=std::strtod(p[2].c_str(),nullptr);
        int lo=0,hi=(int)B.size()-1,r=0; while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;}
        e.push_back({r,dir,px});
    }
    return e;
}

struct Clip { double gross, net; int64_t ets, xts; std::string reason; char bull; int year; };

// Drive the REAL GoldTrendMimicBook over (entries, bars). close_only => feed on_bar(c,c,c).
static std::vector<Clip> run_cell(GoldTrendMimicBook::Config cfg, const std::vector<Entry>& ents,
                                  const std::vector<Bar>& B, const std::vector<char>& bull,
                                  bool no_prebe, bool close_only, int uid){
    cfg.no_prebe_loss = no_prebe;
    char sp[256], cp[256];
    std::snprintf(sp,sizeof sp,"/tmp/npt/state_%d_%d.txt",uid,(int)no_prebe);
    std::snprintf(cp,sizeof cp,"/tmp/npt/closed_%d_%d.csv",uid,(int)no_prebe);
    std::remove(sp); std::remove(cp); std::string spo=std::string(sp)+".open"; std::remove(spo.c_str());
    cfg.state_path=sp; cfg.closed_path=cp;
    GoldTrendMimicBook book(cfg);
    std::vector<Clip> clips;
    double rt=cfg.rt_cost_bp/100.0;   // % cost
    // lambda captures for regime/year lookup keyed by exit ts
    auto bull_at=[&](int64_t ts)->char{ int lo=0,hi=(int)B.size()-1,r=0; while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;} return bull[r]; };
    book.set_exec(nullptr,nullptr,nullptr,
        [&](const std::string&,const std::string&,bool is_long,double entry,double fill,double,
            int64_t ets,int64_t xts,const char* reason,double,double){
            double dir=is_long?1.0:-1.0;
            double gross=dir*(fill/entry-1.0)*100.0;
            clips.push_back({gross, gross-rt, ets, xts, reason, bull_at(xts), year_of(xts)});
        });
    // build entry-idx -> entry list
    std::map<int,std::vector<Entry>> byidx; for(auto&e:ents) byidx[e.idx].push_back(e);
    for(size_t i=0;i<B.size();++i){
        const Bar& b=B[i];
        char bl=bull[i];
        if(close_only) book.on_h1_bar(b.c,b.c,b.c,b.ts,bl,true);
        else           book.on_h1_bar(b.h,b.l,b.c,b.ts,bl,true);
        auto it=byidx.find((int)i);
        if(it!=byidx.end()) for(auto&e:it->second) book.on_trend_open(e.dir,e.px,b.ts);
    }
    std::remove(sp); std::remove(cp); std::remove(spo.c_str());
    return clips;
}

struct Stat{ int n=0; double net=0,pf=0,worstg=1e9,worstn=1e9; int nneg_g=0,nneg_n=0,wins=0; double mdd=0; };
static Stat stats(const std::vector<Clip>& c){
    Stat s; s.n=c.size(); if(!s.n) return s;
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

struct Cell { std::string name; GoldTrendMimicBook::Config cfg; std::string entry_src; std::string bars; bool close_only; int donchN; std::string real_entries; bool use_2022cal; };

static void eval_and_print(const Cell& C,int uid){
    auto B=load_bars(C.bars); if(B.empty()){ fprintf(stderr,"  %-22s DATA MISSING (%s)\n",C.name.c_str(),C.bars.c_str()); return; }
    auto bl=bull_vec(B);
    std::vector<Entry> ents;
    if(C.entry_src=="dip") ents=dip_entries(B);
    else if(C.entry_src=="turtle") ents=turtle_entries(B);
    else if(C.entry_src=="donch") ents=donch_entries(B,C.donchN);
    else if(C.entry_src=="real") ents=read_real_entries(C.real_entries,B);
    if(ents.empty()){ fprintf(stderr,"  %-22s 0 ENTRIES\n",C.name.c_str()); return; }
    auto off=run_cell(C.cfg,ents,B,bl,false,C.close_only,uid);
    auto on =run_cell(C.cfg,ents,B,bl,true ,C.close_only,uid);
    auto so=stats(off), sn=stats(on);
    // all-6 for ON
    std::vector<Clip> h1,h2; halves(on,h1,h2); auto s1=stats(h1),s2=stats(h2);
    Stat sb=stats(filt_bull(on,1)), sr=stats(filt_bull(on,0));
    Stat y22=stats(filt_year(on,2022));
    bool reg_ok = sb.net>0 && (sr.n==0 || sr.net>0);
    bool cal22 = (!C.use_2022cal) || (y22.n==0 || y22.net>0);
    bool all6 = sn.net>0 && sn.pf>=1.3 && s1.net>0 && s2.net>0 && reg_ok && cal22;
    const char* pf = sn.pf>=1e8?"inf":nullptr; char pfb[16]; if(!pf){snprintf(pfb,sizeof pfb,"%.2f",sn.pf);pf=pfb;}
    std::fprintf(stderr,"  %-22s ent=%4d | OFF net=%+8.1f wc=%+6.2f nNeg=%4d | ON net=%+8.1f wc=%+6.2f nNeg(g/n)=%d/%d PF=%s mdd=%+7.1f | H1%+6.0f H2%+6.0f bull%+6.0f bear%+6.0f %s%+6.0f | %s\n",
        C.name.c_str(), (int)ents.size(),
        so.net, so.worstg, so.nneg_g,
        sn.net, sn.worstg, sn.nneg_g, sn.nneg_n, pf, sn.mdd,
        s1.net,s2.net,sb.net,sr.net, C.use_2022cal?"y22":"---", C.use_2022cal?y22.net:0.0,
        all6?"ALL6-PASS":"FAIL");
}

int main(int argc,char**argv){
    if(!freopen("/dev/null","w",stdout)){}  // silence header [GMIMIC] spam; report is on stderr
    system("mkdir -p /tmp/npt");
    const std::string TICK="/Users/jo/Tick/";
    const std::string SD="/Users/jo/Omega/backtest/data/bigcap_daily_ohlc/";
    std::vector<Cell> cells;
    auto GC=[&](std::string tag,std::vector<GoldTrendMimicBook::LegCfg> legs,double arm,double lc,int cap,
                double be,double rt,std::string esrc,std::string bars,int dN,std::string real,bool bull_only)->Cell{
        Cell c; c.name=tag; c.cfg.trigger_tag=tag; c.cfg.legs=legs; c.cfg.arm_pct=arm; c.cfg.lc_pct=lc;
        c.cfg.cap_bars=cap; c.cfg.be_entry_pct=be; c.cfg.rt_cost_bp=rt; c.cfg.pend_bars=6; c.cfg.notional=10000;
        c.cfg.bull_only=bull_only; c.entry_src=esrc; c.bars=bars; c.donchN=dN; c.real_entries=real; c.close_only=false; c.use_2022cal=false; return c; };

    // ---- GOLD cells (intrabar h,l,c; regime = bull/bear via SMA200) ----
    std::fprintf(stderr,"=== GOLD MIMIC CELLS (real header, intrabar h/l/c, regime bull/bear) ===\n");
    cells.push_back(GC("XauTf4h",{{"T1",0.08},{"T2",0.10},{"W1",0.20},{"W2",0.25}},0.25,1.5,12,0.15,15,"real",TICK+"2yr_XAUUSD_tick_fresh.h4.csv",0,"/tmp/npt/entries_xautf4h.csv",false));
    cells.push_back(GC("XauTfD1",{{"T",0.08},{"W",0.20}},0.25,2.0,8,0.15,15,"real",TICK+"2yr_XAUUSD_daily.csv",0,"/tmp/npt/entries_xautfd1.csv",false));
    cells.push_back(GC("XauTf2h",{{"T",0.08},{"W",0.30}},0.25,1.0,24,0.15,15,"real",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",0,"/tmp/npt/entries_xautf2h.csv",false));
    cells.push_back(GC("MgcFastDon",{{"T",0.08},{"W",0.20}},0.15,1.0,24,0.15,15,"real",TICK+"2yr_XAUUSD_tick_fresh.m30.csv",0,"/tmp/npt/entries_mgcfastdon.csv",false));
    cells.push_back(GC("XAU_4h_DonchN20*LIVE",{{"T",0.10}},0.25,2.0,30,1.0,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.h4.csv",20,"",true));
    cells.push_back(GC("MgcTf1h",{{"T",0.08},{"W",0.20}},0.50,2.0,24,0.10,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",20,"",false));
    cells.push_back(GC("GoldKeltM30",{{"T",0.08},{"W",0.20}},0.25,2.0,96,0.10,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.m30.csv",20,"",false));
    cells.push_back(GC("GoldTfBw1040",{{"T",0.08},{"W",0.20}},0.15,1.0,48,0.10,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",20,"",false));
    cells.push_back(GC("GoldTfBw20100",{{"T",0.08},{"W",0.20}},0.15,2.0,48,0.10,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",20,"",false));
    cells.push_back(GC("GoldDonH1",{{"T",0.08},{"W",0.20}},0.50,2.0,12,0.10,5,"donch",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",20,"",false));
    int uid=1; for(auto&c:cells) eval_and_print(c,uid++);

    // ---- STOCKDIP cells (close-only c,c,c; calendar 2022 test; be_entry SET >= rt_cost 8bp) ----
    // Two runs each: (A) legacy shipped be_entry=0 pre_arm_be (shows the pre-BE loss hole), and
    // (B) BE-ENTRY be_entry=0.08 + no_prebe (the fix). We report the FIX cell here.
    const char* DIP_NAMES[] = {"MU","NVDA","AVGO","DELL","CRDO","STX","INTC","AMD","AAPL","TPR","MSFT"};
    const char* TUR_NAMES[] = {"NVDA","AVGO","STX","DD","AMD","AAPL","TPR","BMY","SWKS","MSFT","QCOM"};
    struct SFam{ std::string fam; std::string sig; double arm,lc,gb,prearm; };
    std::vector<SFam> dipfam={{"T","dip",2.0,2.0,0.50,1.0},{"W","dip",3.0,3.0,0.70,1.5},{"X","dip",1.5,1.5,0.70,0.75},{"Y","dip",2.5,2.5,0.60,1.25}};
    std::vector<SFam> turfam={{"A","turtle",1.5,1.5,0.50,0.75},{"B","turtle",2.0,2.0,0.50,1.0},{"C","turtle",2.5,2.5,0.40,1.25},{"D","turtle",3.5,3.5,0.40,1.75}};

    // Pool per family across all names (report pooled all-6 like the findings docs).
    auto run_family=[&](const std::string& label,const SFam& F,const char** names,int nn,double be,bool no_prebe,bool prearm_on)->Stat{
        std::vector<Clip> pool;
        for(int k=0;k<nn;k++){
            std::string bars=SD+names[k]+".csv"; auto B=load_bars(bars); if(B.empty())continue; auto bl=bull_vec(B);
            std::vector<Entry> ents = (F.sig=="dip")?dip_entries(B):turtle_entries(B); if(ents.empty())continue;
            GoldTrendMimicBook::Config c; c.trigger_tag="SD"+label+"_"+names[k]; c.live_sym=names[k];
            c.legs={{"",F.gb}}; c.arm_pct=F.arm; c.lc_pct=F.lc; c.cap_bars=10;
            c.pre_arm_be_pct = prearm_on? F.prearm : 0.0; c.be_entry_pct=be; c.rt_cost_bp=8.0; c.notional=10000; c.pend_bars=6; c.bull_only=false;
            auto cl=run_cell(c,ents,B,bl,no_prebe,true,1000+k+(int)(be*100)+(no_prebe?7:0));
            for(auto&x:cl) pool.push_back(x);
        }
        return stats(pool);
    };
    auto print_fam=[&](const std::string& label,const SFam& F,const char** names,int nn){
        // OFF = legacy shipped (be=0, pre_arm_be on, no_prebe off) ; ON = fix (be=0.08, no_prebe on)
        Stat off=run_family(label,F,names,nn,0.0,false,true);
        // pooled all-6 for ON:
        std::vector<Clip> onpool;
        for(int k=0;k<nn;k++){ std::string bars=SD+names[k]+".csv"; auto B=load_bars(bars); if(B.empty())continue; auto bl=bull_vec(B);
            std::vector<Entry> ents=(F.sig=="dip")?dip_entries(B):turtle_entries(B); if(ents.empty())continue;
            GoldTrendMimicBook::Config c; c.trigger_tag="SD"+label+"_"+names[k]; c.legs={{"",F.gb}}; c.arm_pct=F.arm; c.lc_pct=F.lc;
            c.cap_bars=10; c.pre_arm_be_pct=0.0; c.be_entry_pct=0.08; c.rt_cost_bp=8.0; c.notional=10000; c.pend_bars=6;
            auto cl=run_cell(c,ents,B,bl,true,true,2000+k); for(auto&x:cl) onpool.push_back(x); }
        Stat on=stats(onpool);
        std::vector<Clip> h1,h2; halves(onpool,h1,h2); auto s1=stats(h1),s2=stats(h2);
        Stat sb=stats(filt_bull(onpool,1)), sr=stats(filt_bull(onpool,0)), y22=stats(filt_year(onpool,2022));
        bool all6 = on.net>0 && on.pf>=1.3 && s1.net>0 && s2.net>0 && sb.net>0 && (sr.n==0||sr.net>0) && (y22.n==0||y22.net>0);
        char pfb[16]; if(on.pf>=1e8)snprintf(pfb,sizeof pfb,"inf"); else snprintf(pfb,sizeof pfb,"%.2f",on.pf);
        std::fprintf(stderr,"  StockDip%-4s ent=%4d | OFF(legacy) net=%+8.1f wc=%+6.2f nNeg=%4d | ON(fix be.08) net=%+8.1f wc=%+6.2f nNeg(g/n)=%d/%d PF=%s | H1%+7.0f H2%+7.0f bull%+7.0f bear%+7.0f y22%+7.0f | %s\n",
            (label).c_str(), on.n, off.net, off.worstg, off.nneg_g, on.net, on.worstg, on.nneg_g, on.nneg_n, pfb,
            s1.net,s2.net,sb.net,sr.net,y22.net, all6?"ALL6-PASS":"FAIL");
    };
    std::fprintf(stderr,"\n=== STOCKDIP DIP MIMIC CELLS (real header, CLOSE-ONLY, calendar 2022) ===\n");
    for(auto&F:dipfam) print_fam(F.fam,F,DIP_NAMES,11);
    std::fprintf(stderr,"\n=== STOCKTURTLE MIMIC CELLS (real header, CLOSE-ONLY, calendar 2022) ===\n");
    for(auto&F:turfam) print_fam(F.fam,F,TUR_NAMES,11);
    return 0;
}
