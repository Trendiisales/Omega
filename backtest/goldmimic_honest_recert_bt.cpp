// =============================================================================
// goldmimic_honest_recert_bt.cpp — S-2026-07-20 HONEST-LEDGER re-certification
// of ALL 13 wired GoldTrendMimicLadder books (engine_init.hpp ~L1646-1881).
//
// Parity COPY of backtest/clip_path_noprebe_floor.cpp with the drifts fixed:
//   * pend_bars is PER-CELL (engine_init parity: 12 for the 5 new books,
//     4 for XauTfD1, 5 for turtles, 6 for XauTf4h/MgcTF4h/DonchN20) — the old
//     harness hardcoded 6 for every cell.
//   * retired XauTf2h / MgcFastDon cells DROPPED (tombstoned; hands-off).
//   * every book runs on its REAL parent entry stream (DUMP_ENTRIES / clip_path
//     producers) except XAU_4h_DonchN20 (Donchian-20 recon = its real trigger).
//   * cost reported at 1x AND 2x rt (rt does not alter the header's exit
//     decisions — only the booked net — so both come from one clip stream).
//   * exit-reason split (BE_FLOOR/LOSS_CUT/TRAIL_STOP/WINDOW_CAP n + sum) to
//     quantify the two old inflation channels (TRAIL level-vs-extreme,
//     BE_FLOOR 0.0-vs-observed).
//
// Drives the REAL include/GoldTrendMimicLadder.hpp GoldTrendMimicBook — the
// header already books HONESTLY since 58a478e2 (S-2026-07-20z2 HONEST LEDGER):
// BE_FLOOR books the real sub-0 ret, LOSS_CUT/TRAIL_STOP book the pierce
// extreme, WINDOW_CAP books the raw close. So the figures here ARE the honest
// re-cert; delta vs the old engine_init/vault baselines = booking inflation.
//
// usage: goldmimic_honest_recert_bt <entries_dir> <derived_dir>
//   entries_dir: entries_<book>.csv rows "entry_ts_sec,dir,entry_px"
//   derived_dir: xau_d1_from_h1.csv mgc_h1_from_m30.csv xau_10m_from_1m.csv
// Report on stderr (stdout silenced: header [GMIMIC] spam).
// =============================================================================
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
    std::string line;
    while(std::getline(f,line)){
        if(line.empty()||line[0]<'0'||line[0]>'9') continue;
        Bar b; std::vector<std::string> p; std::string t; std::stringstream ss(line);
        while(std::getline(ss,t,',')) p.push_back(t);
        if(p.size()<5) continue;
        b.ts=std::strtoll(p[0].c_str(),nullptr,10);
        if(b.ts>4000000000LL) b.ts/=1000;
        b.o=std::strtod(p[1].c_str(),nullptr); b.h=std::strtod(p[2].c_str(),nullptr);
        b.l=std::strtod(p[3].c_str(),nullptr); b.c=std::strtod(p[4].c_str(),nullptr);
        if(b.c>0) v.push_back(b);
    }
    return v;
}
static std::vector<double> sma_closes(const std::vector<Bar>&B,int n){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=n) sum-=B[i-n].c; s[i]= i>=n-1? sum/n : B[i].c; }
    return s;
}
static std::vector<char> bull_vec(const std::vector<Bar>&B){       // close>=SMA200 of the fed TF
    auto s=sma_closes(B,200); std::vector<char> v(B.size(),1);
    for(size_t i=0;i<B.size();++i) v[i]= (i>=200 && B[i].c < s[i]) ? 0 : 1;
    return v;
}

struct Entry { int idx; int dir; double px; };
// Donchian-N upper breakout on highs (XAU_4h_DonchN20 real trigger recon): close>max(prior N highs).
static std::vector<Entry> donch_entries(const std::vector<Bar>&B,int n){
    std::vector<Entry> e; int N=B.size();
    for(int i=0;i<N;i++){ if(i<n+1) continue; double mx=0; for(int k=i-n;k<i;k++) mx=std::max(mx,B[k].h);
        if(B[i].c>mx) e.push_back({i,+1,B[i].c}); }
    return e;
}
// entries csv "entry_ts_sec,dir,entry_px" -> nearest bar idx (ts<=).
static std::vector<Entry> read_real_entries(const std::string& path,const std::vector<Bar>&B){
    std::vector<Entry> e; std::ifstream f(path); if(!f){fprintf(stderr,"MISSING entries %s\n",path.c_str());return e;}
    std::string line;
    while(std::getline(f,line)){ if(line.empty())continue; std::vector<std::string>p; std::string t; std::stringstream ss(line);
        while(std::getline(ss,t,',')) p.push_back(t); if(p.size()<3) continue;
        int64_t ts=std::strtoll(p[0].c_str(),nullptr,10); int dir=std::atoi(p[1].c_str()); double px=std::strtod(p[2].c_str(),nullptr);
        if(px<=0||dir==0) continue;
        int lo=0,hi=(int)B.size()-1,r=0; while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;}
        e.push_back({r,dir,px});
    }
    return e;
}

struct Clip { double gross; double entry_px; int64_t ets, xts; std::string reason; char bull; int li; };

// Drive the REAL GoldTrendMimicBook over (entries, bars), intrabar h/l/c.
static std::vector<Clip> run_cell(GoldTrendMimicBook::Config cfg, const std::vector<Entry>& ents,
                                  const std::vector<Bar>& B, const std::vector<char>& bull, int uid){
    char sp[256], cp[256];
    std::snprintf(sp,sizeof sp,"/tmp/gmh/state_%d.txt",uid);
    std::snprintf(cp,sizeof cp,"/tmp/gmh/closed_%d.csv",uid);
    std::remove(sp); std::remove(cp); std::string spo=std::string(sp)+".open"; std::remove(spo.c_str());
    cfg.state_path=sp; cfg.closed_path=cp;
    GoldTrendMimicBook book(cfg);
    std::vector<Clip> clips;
    auto bull_at=[&](int64_t ts)->char{ int lo=0,hi=(int)B.size()-1,r=0; while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;} return bull[r]; };
    const auto& legs = cfg.legs;
    book.set_exec(nullptr,nullptr,nullptr,
        [&](const std::string& eng,const std::string&,bool is_long,double entry,double fill,double,
            int64_t ets,int64_t xts,const char* reason,double,double){
            double dir=is_long?1.0:-1.0;
            double gross=dir*(fill/entry-1.0)*100.0;
            int li=0;   // recover leg index from engine name suffix (tag+"Mimic"+legtag)
            for(size_t k=0;k<legs.size();++k){ std::string suf=std::string("Mimic")+legs[k].tag;
                if(eng.size()>=suf.size() && eng.compare(eng.size()-suf.size(),suf.size(),suf)==0){ li=(int)k; break; } }
            clips.push_back({gross, entry, ets, xts, reason, bull_at(xts), li});
        });
    std::map<int,std::vector<Entry>> byidx; for(auto&e:ents) byidx[e.idx].push_back(e);
    for(size_t i=0;i<B.size();++i){
        const Bar& b=B[i];
        book.on_h1_bar(b.h,b.l,b.c,b.ts,bull[i],true);
        auto it=byidx.find((int)i);
        if(it!=byidx.end()) for(auto&e:it->second) book.on_trend_open(e.dir,e.px,b.ts);
    }
    std::remove(sp); std::remove(cp); std::remove(spo.c_str());
    return clips;
}

struct Stat{ int n=0; double net=0,pf=0,worstg=0,worstn=0; int nneg_g=0,nneg_n=0,wins=0; double mdd=0, usd=0; };
static Stat stats(const std::vector<Clip>& c, double rt /*% cost per clip*/){
    Stat s; s.n=c.size(); if(!s.n) return s;
    s.worstg=1e9; s.worstn=1e9;
    double gp=0,gn=0;
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&a,const Clip&b){return a.xts<b.xts;});
    double cum=0,pk=0;
    for(auto&x:sc){ double net=x.gross-rt;
        s.net+=net; if(net>0)gp+=net; else gn+=-net;
        if(net>1e-9)s.wins++;
        s.worstg=std::min(s.worstg,x.gross); s.worstn=std::min(s.worstn,net);
        if(x.gross<-1e-9)s.nneg_g++; if(net<-1e-9)s.nneg_n++;
        s.usd += net/100.0*x.entry_px*10.0;   // 10oz (1 MGC) per clip gauge
        cum+=net; if(cum>pk)pk=cum; if(cum-pk<s.mdd)s.mdd=cum-pk; }
    s.pf = gn>1e-9? gp/gn : 1e9;
    return s;
}
static std::vector<Clip> filt_bull(const std::vector<Clip>&c,char b){ std::vector<Clip> r; for(auto&x:c) if(x.bull==b) r.push_back(x); return r; }
static std::vector<Clip> filt_li(const std::vector<Clip>&c,int li){ std::vector<Clip> r; for(auto&x:c) if(x.li==li) r.push_back(x); return r; }
static std::vector<Clip> filt_win(const std::vector<Clip>&c,int64_t a,int64_t b){ std::vector<Clip> r; for(auto&x:c) if(x.ets>=a&&x.ets<b) r.push_back(x); return r; }
static void halves(const std::vector<Clip>&c,std::vector<Clip>&a,std::vector<Clip>&b){
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&x,const Clip&y){return x.xts<y.xts;});
    size_t mid=sc.size()/2; a.assign(sc.begin(),sc.begin()+mid); b.assign(sc.begin()+mid,sc.end());
}
static const char* pfs(double pf,char* buf,size_t n){ if(pf>=1e8){snprintf(buf,n,"inf");} else snprintf(buf,n,"%.2f",pf); return buf; }

struct Cell {
    std::string name;
    GoldTrendMimicBook::Config cfg;
    std::string entry_src;      // "real" | "donch"
    std::string bars;           // bar csv path (native TF of the book feed)
    int donchN=0;
    std::string real_entries;
    std::string note;
};

static void eval_and_print(const Cell& C,int uid){
    auto B=load_bars(C.bars); if(B.empty()){ fprintf(stderr,"%-18s SKIPPED: DATA MISSING (%s)\n",C.name.c_str(),C.bars.c_str()); return; }
    auto bl=bull_vec(B);
    std::vector<Entry> ents;
    if(C.entry_src=="donch") ents=donch_entries(B,C.donchN);
    else ents=read_real_entries(C.real_entries,B);
    if(ents.empty()){ fprintf(stderr,"%-18s SKIPPED: 0 ENTRIES (%s)\n",C.name.c_str(),C.real_entries.c_str()); return; }
    auto clips=run_cell(C.cfg,ents,B,bl,uid);
    const double rt1=C.cfg.rt_cost_bp/100.0, rt2=2.0*rt1;
    Stat s1=stats(clips,rt1), s2=stats(clips,rt2);
    std::vector<Clip> h1,h2; halves(clips,h1,h2);
    Stat sh1=stats(h1,rt1), sh2=stats(h2,rt1);
    Stat sb=stats(filt_bull(clips,1),rt1), sr=stats(filt_bull(clips,0),rt1);
    bool reg_ok = (sb.n==0||sb.net>0) && (sr.n==0 || sr.net>0);
    bool pass1 = s1.n>0 && s1.net>0 && s1.pf>=1.3 && sh1.net>0 && sh2.net>0 && reg_ok;
    bool pass2 = s2.net>0;
    char p1[16],p2[16];
    std::fprintf(stderr,"%-18s ent=%4d n=%4d | 1x net=%+8.1f%% PF=%-5s worstG=%+6.2f worstN=%+6.2f nNeg(g/n)=%3d/%3d mdd=%+7.1f usd10oz=$%+8.0f | 2x net=%+8.1f%% PF=%-5s | WF %+7.1f/%+7.1f bull%+7.1f(n%d) bear%+7.1f(n%d) | %s%s %s\n",
        C.name.c_str(),(int)ents.size(),s1.n,
        s1.net,pfs(s1.pf,p1,sizeof p1),s1.worstg,s1.worstn,s1.nneg_g,s1.nneg_n,s1.mdd,s1.usd,
        s2.net,pfs(s2.pf,p2,sizeof p2),
        sh1.net,sh2.net,sb.net,sb.n,sr.net,sr.n,
        pass1?"ALL-CORE-PASS":"CORE-FAIL", pass2?"":" 2x-FAIL", C.note.c_str());
    // per-leg split at 1x
    for(size_t k=0;k<C.cfg.legs.size();++k){
        Stat sl1=stats(filt_li(clips,(int)k),rt1), sl2=stats(filt_li(clips,(int)k),rt2);
        char pa[16];
        std::fprintf(stderr,"    leg %-3s gb%.2f  n=%4d net1x=%+8.1f%% net2x=%+8.1f%% PF=%-5s worstN=%+6.2f nNegN=%3d usd=$%+8.0f\n",
            C.cfg.legs[k].tag,C.cfg.legs[k].gb,sl1.n,sl1.net,sl2.net,pfs(sl1.pf,pa,sizeof pa),sl1.worstn,sl1.nneg_n,sl1.usd);
    }
    // reason split at 1x
    std::map<std::string,std::pair<int,double>> rs;
    for(auto&x:clips){ auto&e=rs[x.reason]; e.first++; e.second+=x.gross-rt1; }
    std::fprintf(stderr,"    reasons:");
    for(auto&kv:rs) std::fprintf(stderr,"  %s n=%d net=%+.1f",kv.first.c_str(),kv.second.first,kv.second.second);
    std::fprintf(stderr,"\n");
    // 6mo gate window (GoldDon10m-family baseline comparability)
    static const int64_t T6A=1768348800, T6B=1784073600;
    auto w6=filt_win(clips,T6A,T6B);
    if(!w6.empty() && (C.name.find("Don10m")!=std::string::npos)){
        for(size_t k=0;k<C.cfg.legs.size();++k){
            Stat w1=stats(filt_li(w6,(int)k),rt1), w2=stats(filt_li(w6,(int)k),rt2);
            char pa[16];
            std::fprintf(stderr,"    6mo leg %-3s n=%4d usd1x=$%+8.0f usd2x=$%+8.0f PF=%-5s net1x=%+7.1f%% worstN=%+6.2f\n",
                C.cfg.legs[k].tag,w1.n,w1.usd,w2.usd,pfs(w1.pf,pa,sizeof pa),w1.net,w1.worstn);
        }
    }
}

int main(int argc,char**argv){
    if(argc<3){ fprintf(stderr,"usage: %s <entries_dir> <derived_dir>\n",argv[0]); return 2; }
    if(!freopen("/dev/null","w",stdout)){}
    system("mkdir -p /tmp/gmh");
    const std::string E=std::string(argv[1])+"/", D=std::string(argv[2])+"/";
    const std::string TICK="/Users/jo/Tick/";
    std::vector<Cell> cells;
    auto GC=[&](std::string tag,std::vector<GoldTrendMimicBook::LegCfg> legs,double arm,double lc,int cap,
                double be,double rt,int pend,std::string esrc,std::string bars,int dN,std::string real,
                bool bull_only,std::string note)->Cell{
        Cell c; c.name=tag; c.cfg.trigger_tag=tag; c.cfg.legs=legs; c.cfg.arm_pct=arm; c.cfg.lc_pct=lc;
        c.cfg.cap_bars=cap; c.cfg.be_entry_pct=be; c.cfg.rt_cost_bp=rt; c.cfg.pend_bars=pend;
        c.cfg.no_prebe_loss=true; c.cfg.notional=10000; c.cfg.bull_only=bull_only;
        c.entry_src=esrc; c.bars=bars; c.donchN=dN; c.real_entries=real; c.note=note; return c; };

    std::fprintf(stderr,"=== GOLDMIMIC HONEST RE-CERT (real header 58a478e2+, per-cell pend parity, 1x/2x cost) ===\n");
    // engine_init.hpp parity configs (S-2026-07-20 HEAD)
    cells.push_back(GC("XauTf4h",{{"T1",0.08},{"T2",0.10},{"W1",0.20},{"W2",0.25}},0.25,1.5,12,0.15,15,6,
        "real",TICK+"2yr_XAUUSD_tick_fresh.h4.csv",0,E+"entries_xautf4h.csv",false,"[H4, real XauTf4h entries]"));
    cells.push_back(GC("XauTfD1",{{"T",0.08},{"W",0.20}},0.25,2.0,8,0.15,15,4,
        "real",D+"xau_d1_from_h1.csv",0,E+"entries_xautfd1.csv",false,"[D1 derived from certified H1; pend=4 parity]"));
    cells.push_back(GC("MgcTF4h",{{"T",0.08},{"W",0.20}},0.25,1.5,12,0.15,5,6,
        "real",TICK+"mgc_2024_2026.h4.csv",0,E+"entries_mgctf4h.csv",false,"[MGC H4, REGIME=1 live parent entries, venue rt5]"));
    cells.push_back(GC("NAS100Turtle",{{"T",0.08},{"W",0.20}},0.5,3.0,10,0.10,4,5,
        "real",TICK+"NDX_daily_2016_2026.csv",0,E+"entries_idx_nas.csv",false,"[D1, real NasTurtleD1 entries; pend=5 parity]"));
    cells.push_back(GC("US500Turtle",{{"T",0.08},{"W",0.20}},0.5,3.0,10,0.10,4,5,
        "real",TICK+"SPX_daily_2016_2026.csv",0,E+"entries_idx_spx.csv",false,"[D1; pend=5 parity]"));
    cells.push_back(GC("DJ30Turtle",{{"T",0.08},{"W",0.20}},0.5,3.0,10,0.10,4,5,
        "real",TICK+"DJ30_daily_2016_2026.csv",0,E+"entries_idx_dj30.csv",false,"[D1; pend=5 parity]"));
    cells.push_back(GC("XAU_4h_DonchN20",{{"T",0.10}},0.25,2.0,30,1.0,5,6,
        "donch",TICK+"2yr_XAUUSD_tick_fresh.h4.csv",20,"",true,"[LIVE 1MGC; Donch-20 recon, bull_only, H4 grain]"));
    cells.push_back(GC("MgcTf1h",{{"T",0.08},{"W",0.20}},0.50,2.0,24,0.10,5,12,
        "real",TICK+"mgc_2024_2026.h1.csv",0,E+"entries_mgctf1h.csv",false,"[MGC H1, real MgcTf1h port entries; pend=12 parity]"));
    cells.push_back(GC("GoldKeltM30",{{"T",0.08},{"W",0.20}},0.25,2.0,96,0.10,5,12,
        "real","/Users/jo/Omega/backtest/data/mgc_30m_spliced_2024_2026.csv",0,E+"entries_goldkeltm30.csv",false,"[M30, real KELT k1.25 entries; pend=12 parity]"));
    cells.push_back(GC("GoldTfBw1040",{{"T",0.08},{"W",0.20}},0.15,1.0,48,0.10,5,12,
        "real",D+"mgc_h1_from_m30.csv",0,E+"entries_goldtfbw1040.csv",false,"[H1 from certified M30, real ema10/40 entries]"));
    cells.push_back(GC("GoldTfBw20100",{{"T",0.08},{"W",0.20}},0.15,2.0,48,0.10,5,12,
        "real",D+"mgc_h1_from_m30.csv",0,E+"entries_goldtfbw20100.csv",false,"[H1 from certified M30, real ema20/100 entries]"));
    cells.push_back(GC("GoldDonH1",{{"T",0.08},{"W",0.20}},0.50,2.0,12,0.10,5,12,
        "real",D+"mgc_h1_from_m30.csv",0,E+"entries_golddonh1.csv",false,"[H1 from certified M30, real DON 20/10 entries]"));
    // XAU_4h_DonchN20 fine-grain variant: the LIVE book is resting_exec (tick-driven) — H4
    // bar-extreme worst-of overstates its pierce tails. H1 grain, pend/cap x4 scaled,
    // bull_only via fed-bars SMA200 (H1-SMA200 = the actual production regime gate).
    cells.push_back(GC("XAU_4h_DonchN20@H1",{{"T",0.10}},0.25,2.0,120,1.0,5,24,
        "real",TICK+"2yr_XAUUSD_tick_fresh.h1.csv",0,E+"entries_donch20_h4.csv",true,"[H1 grain x4-scaled windows, H1-SMA200 gate]"));
    // GoldDon10m: (a) native 10m grain (production on_bar cadence)
    cells.push_back(GC("GoldDon10m",{{"T",0.08},{"W",0.20}},1.00,1.0,144,0.10,5,12,
        "real",D+"xau_10m_from_1m.csv",0,E+"entries_golddon10m.csv",false,"[LIVE 2MGC; native 10m grain, real DONf 30/35 entries]"));
    // GoldDon10m: (b) 1m truth grain — pend/cap scaled x10 (windows preserved), finer pierce truth
    cells.push_back(GC("GoldDon10m@1m",{{"T",0.08},{"W",0.20}},1.00,1.0,1440,0.10,5,120,
        "real",TICK+"xau_1m_spliced_2024_2026.csv",0,E+"entries_golddon10m.csv",false,"[1m TRUTH grain, pend/cap x10 scaled]"));

    int uid=1; for(auto&c:cells) eval_and_print(c,uid++);
    return 0;
}
