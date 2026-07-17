// clip_path_mgc_venue_mimic.cpp — S-2026-07-17: certify (or kill) BE-floored
// GoldTrendMimicBook books for the three uncovered MGC-venue gold parents:
//   MgcTF4h (XauTrendFollow4hEngine, MGC port, live cfg omega_main.hpp L296)
//   MgcSlowDonchian30m (live cfg omega_main.hpp L220, Nin55/Nout27)
//   MgcTF2h (XauTrendFollow2hEngine, MGC port, live cfg L313; BULL-GATED book only)
//
// NOT the retired books: MgcFastDon mimic + XauTf2h SPOT mimic were retired
// S-2026-07-17 with full-grid evidence (engine_init.hpp L1630/L1648). This is
// the MGC VENUE port of the 2h parent behind the book-standard H1-SMA200 bull
// gate (bull_only=true) — a different basis, judged fresh.
//
// MODE entries  : drive the REAL parent engines over the certified MGC 30m file
//   with the EXACT production feed ordering (MgcFastDonchianFeed.hpp poll loop:
//   fastdon -> slowdon -> tf on_tick l/h/c -> H1 bucket -> H4 bucket), plus an
//   optional gold_regime() feed (REGIME=1: H1 closes drive the live bear-gate;
//   REGIME=0 reproduces the ungated parity refs 4h n291 +$4209 / 2h n596 +$3533).
//   Dumps per-parent entries CSVs (entry_ts_sec,dir,entry_px) from the closed
//   TradeRecords + prints parent stats for parity vs registry §7.
//   env: REGIME=0/1  REGIME_PRESEED=<h1 csv fed for ts < first-row ts>
//   argv: entries <base_csv(ts,o,h,l,c[,v])> <outdir>
//
// MODE mimic    : run the REAL GoldTrendMimicBook (include/GoldTrendMimicLadder.hpp,
//   read-only) floored-on-open (no_prebe_loss=true, BE-ENTRY) over the dumped
//   entries + the parent's NATIVE bar file, intrabar h->l->c management (the
//   header's own SL-first bar path). Sweep arm x cap x pend x gb x be at RT and
//   2xRT. Gate per config: net>0, PF>=1.3, WF halves both >0, within-dataset
//   regime split (SMA200) bull>0 & bear>=0-or-n0. The 2022 bear-axis dataset is
//   a separate invocation; verdicts combined in the findings doc.
//   env: RT=15 SWEEP=0/1 BULL_ONLY=0/1 ARM LC CAP PEND BE GBT GBW TAG
//   argv: mimic <entries_csv> <bars_csv>
//
// HONEST FRAMING (S-2026-07-17f rule): floored exits book AT the floor LEVEL
// (the shipped GoldTrendMimicBook convention all 12 live books were certified
// under). worst/nNeg are reported at the NET level (gross - rt cost): a
// BE-floored clip books net = -cost, and a real gap through the floor would
// fill worse than the level. nNeg=0 is NEVER claimed here.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/clip_path_mgc_venue_mimic.cpp \
//            -o /tmp/mgcmimic/cpm
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>
#include <sstream>

#include "OmegaTradeLedger.hpp"
#include "RegimeState.hpp"
#include "MgcFastDonchian30mEngine.hpp"
#include "MgcSlowDonchian30mEngine.hpp"
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"
#include "GoldTrendMimicLadder.hpp"

struct Row { int64_t ts; double o,h,l,c,v; };
static std::vector<Row> load_rows(const std::string& path){
    std::vector<Row> out; std::ifstream f(path);
    if(!f){ std::fprintf(stderr,"MISSING %s\n",path.c_str()); return out; }
    std::string ln;
    while(std::getline(f,ln)){
        if(ln.empty()||ln[0]<'0'||ln[0]>'9') continue;
        Row r; r.v=0; std::vector<std::string> k; std::string t; std::stringstream ss(ln);
        while(std::getline(ss,t,',')) k.push_back(t);
        if(k.size()<5) continue;
        r.ts=std::strtoll(k[0].c_str(),nullptr,10);
        if(r.ts>4000000000LL) r.ts/=1000;
        r.o=std::strtod(k[1].c_str(),nullptr); r.h=std::strtod(k[2].c_str(),nullptr);
        r.l=std::strtod(k[3].c_str(),nullptr); r.c=std::strtod(k[4].c_str(),nullptr);
        if(k.size()>=6) r.v=std::strtod(k[5].c_str(),nullptr);
        if(r.c>0) out.push_back(r);
    }
    return out;
}

// ───────────────────────────── MODE: entries ─────────────────────────────
struct PStat{ int n=0; double net=0,gw=0,gl=0,cur=0,peak=0,mdd=0;
    void rec(double u){ n++; net+=u; if(u>=0)gw+=u; else gl-=u;
        cur+=u; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur; }
    double pf()const{ return gl>0?gw/gl:99.9; } };

static int run_entries(const std::string& base_csv, const std::string& outdir){
    const int REGIME = getenv("REGIME")?atoi(getenv("REGIME")):1;
    auto rows = load_rows(base_csv);
    if(rows.empty()){ std::fprintf(stderr,"no rows\n"); return 1; }

    // ---- live config mirror (omega_main.hpp, S-2026-07-11/17 state) ----
    static omega::MgcFastDonchian30mEngine fastdon;
    fastdon.enabled=true; fastdon.shadow_mode=true; fastdon.lot=1.0;
    fastdon.Nin=40; fastdon.Nout=20; fastdon.Kov=1.5; fastdon.use_hvn_skip=true;
    // l2 gate inert standalone (l2_imb_ defaults 0.5 >= 0.30)
    fastdon.l2_gate_=0.30;

    static omega::MgcSlowDonchian30mEngine slowdon;
    slowdon.enabled=true; slowdon.shadow_mode=true; slowdon.lot=1.0;
    slowdon.Nin=55; slowdon.Nout=27; slowdon.retire_net_pts=-560.0;
    // NODEDUP=1: drop the live peer-dedup (cert-cell parity diagnostic; the
    // certified n158 was the NAKED cell -- dedup/costguard/regime are live-only)
    if(!(getenv("NODEDUP")&&atoi(getenv("NODEDUP"))))
        slowdon.peer_holds_pos=[](){ return fastdon.has_open_position(); };

    static omega::XauTrendFollow4hEngine tf4h;
    tf4h.enabled=true; tf4h.shadow_mode=true; tf4h.lot=1.0; tf4h.max_spread=1.50;
    tf4h.LOSS_CUT_PCT=1.5; tf4h.cell_enable_mask=0xC9;
    tf4h.use_vol_band_gate=true; tf4h.vol_band_low_pct=0.30; tf4h.vol_band_high_pct=0.85;
    tf4h.cell_vol_band_mask=0x8; tf4h.min_impulse_atr=0.5; tf4h.min_adx_entry=15.0;
    tf4h.ledger_prefix="MgcTF4h_"; tf4h.ledger_symbol="MGC"; tf4h.init();

    static omega::XauTrendFollow2hEngine tf2h;
    tf2h.enabled=true; tf2h.shadow_mode=true; tf2h.lot=1.0; tf2h.max_spread=1.50;
    tf2h.LOSS_CUT_PCT=0.0;                       // MGC 2h wired LC=0 (spot 0.5% trap)
    tf2h.use_adx_gate=true; tf2h.adx_min=25.0; tf2h.cell_adx_mask=0xB;
    tf2h.use_vol_band_gate=true; tf2h.vol_band_low_pct=0.30; tf2h.vol_band_high_pct=0.85;
    tf2h.cell_vol_band_mask=0x4;
    tf2h.ledger_prefix="MgcTF2h_"; tf2h.ledger_symbol="MGC"; tf2h.init();

    // regime preseed (2022 axis: warm the brain on H1 bars before the base stream)
    if(const char* ps=getenv("REGIME_PRESEED"); ps && REGIME){
        auto pre=load_rows(ps); int fed=0;
        for(const auto& r:pre){ if(r.ts>=rows.front().ts) break;
            omega::gold_regime().on_h1_bar(r.o,r.h,r.l,r.c); fed++; }
        std::fprintf(stderr,"[regime] preseeded %d H1 bars, regime=%s warm=%d\n",
                     fed, omega::gold_regime().regime_name(), (int)omega::gold_regime().warm());
    }

    // entry dumps + parity stats
    std::ofstream e4(outdir+"/entries_tf4h.csv"), e2(outdir+"/entries_tf2h.csv"),
                  es(outdir+"/entries_slowdon.csv");
    PStat s4,s2,ss_;
    auto cb=[&](const omega::TradeRecord& tr){
        int64_t ets=tr.entryTs; if(ets>4000000000LL) ets/=1000;
        int dir = (tr.side=="LONG")?+1:-1;
        const double usd = tr.pnl - 0.208;       // parity formula (mgc_tf_feed_parity.cpp)
        if(tr.engine.rfind("MgcTF4h_",0)==0){ e4<<ets<<","<<dir<<","<<tr.entryPrice<<"\n"; s4.rec(usd); }
        else if(tr.engine.rfind("MgcTF2h_",0)==0){ e2<<ets<<","<<dir<<","<<tr.entryPrice<<"\n"; s2.rec(usd); }
        else if(tr.engine=="MgcSlowDonchian30m"){ es<<ets<<","<<dir<<","<<tr.entryPrice<<"\n"; ss_.rec(usd); }
    };

    // ---- production feed ordering (MgcFastDonchianFeed.hpp poll loop replica) ----
    const double sprd=0.10;                      // MGC 1 exchange tick
    int64_t h1_b=0; double h1o=0,h1h=0,h1l=0,h1c=0;
    int64_t h4_b=0; double h4o=0,h4h=0,h4l=0,h4c=0;
    for(const auto& r:rows){
        const int64_t ts=r.ts, ts_ms=ts*1000LL;
        // regime brain fed at H1 boundaries (live: tick_gold spot H1; here base-stream H1)
        const int64_t hb=(ts/3600)*3600;
        if(REGIME && h1_b!=0 && hb!=h1_b) omega::gold_regime().on_h1_bar(h1o,h1h,h1l,h1c);
        fastdon.on_30m_bar(r.o,r.h,r.l,r.c,r.v,ts,cb);
        slowdon.on_30m_bar(r.o,r.h,r.l,r.c,ts,cb);
        // TF engines: intrabar manage low -> high -> close (SL-first, feed order)
        tf4h.on_tick(r.l,r.l+sprd,ts_ms,cb); tf4h.on_tick(r.h,r.h+sprd,ts_ms,cb); tf4h.on_tick(r.c,r.c+sprd,ts_ms,cb);
        tf2h.on_tick(r.l,r.l+sprd,ts_ms,cb); tf2h.on_tick(r.h,r.h+sprd,ts_ms,cb); tf2h.on_tick(r.c,r.c+sprd,ts_ms,cb);
        // H1 bucket -> 2h engine
        if(h1_b!=0 && hb!=h1_b){
            omega::XauTf2hBar b1{}; b1.bar_start_ms=h1_b*1000LL;
            b1.open=h1o; b1.high=h1h; b1.low=h1l; b1.close=h1c;
            tf2h.on_h1_bar(b1,h1c,h1c+sprd,ts_ms,cb);
        }
        if(hb!=h1_b){ h1_b=hb; h1o=r.o; h1h=r.h; h1l=r.l; h1c=r.c; }
        else { if(r.h>h1h)h1h=r.h; if(r.l<h1l)h1l=r.l; h1c=r.c; }
        // H4 bucket -> 4h engine
        const int64_t hb4=(ts/14400)*14400;
        if(h4_b!=0 && hb4!=h4_b){
            omega::XauTfBar b4{}; b4.bar_start_ms=h4_b*1000LL;
            b4.open=h4o; b4.high=h4h; b4.low=h4l; b4.close=h4c;
            tf4h.on_h4_bar(b4,h4c,h4c+sprd,0.0,ts_ms,cb);
        }
        if(hb4!=h4_b){ h4_b=hb4; h4o=r.o; h4h=r.h; h4l=r.l; h4c=r.c; }
        else { if(r.h>h4h)h4h=r.h; if(r.l<h4l)h4l=r.l; h4c=r.c; }
    }
    const auto& last=rows.back();
    tf4h.force_close(last.c,last.c+sprd,last.ts*1000LL,cb,"EOD_FLAT");
    tf2h.force_close(last.c,last.c+sprd,last.ts*1000LL,cb,"EOD_FLAT");
    slowdon.force_close(last.c,last.c+sprd,last.ts,cb);

    std::fprintf(stderr,"[entries] %s rows=%zu REGIME=%d regime_end=%s\n",
                 base_csv.c_str(), rows.size(), REGIME, omega::gold_regime().regime_name());
    std::fprintf(stderr,"  MgcTF4h            n=%-4d net=$%+8.1f PF=%.2f mdd=$%.0f  (REGIME=0 parity ref: n291 +$4209 PF1.50 DD$1064)\n", s4.n,s4.net,s4.pf(),s4.mdd);
    std::fprintf(stderr,"  MgcTF2h            n=%-4d net=$%+8.1f PF=%.2f mdd=$%.0f  (REGIME=0 parity ref: n596 +$3533 PF1.23 DD$2390)\n", s2.n,s2.net,s2.pf(),s2.mdd);
    std::fprintf(stderr,"  MgcSlowDonchian30m n=%-4d net=$%+8.1f PF=%.2f mdd=$%.0f  (cert ref 55/27: n158 +1504.6pt PF1.78)\n", ss_.n,ss_.net,ss_.pf(),ss_.mdd);
    return 0;
}

// ───────────────────────────── MODE: mimic ─────────────────────────────
using omega::GoldTrendMimicBook;
struct Bar { int64_t ts; double o,h,l,c; };
struct Entry { int idx; int dir; double px; };
struct Clip { double gross, net; int64_t ets, xts; char bull; };

static std::vector<double> sma_closes(const std::vector<Bar>&B,int n){
    int N=B.size(); std::vector<double> s(N,0.0); double sum=0;
    for(int i=0;i<N;i++){ sum+=B[i].c; if(i>=n) sum-=B[i-n].c; s[i]= i>=n-1? sum/n : B[i].c; }
    return s;
}
static std::vector<char> bull_vec(const std::vector<Bar>&B){
    auto s=sma_closes(B,200); std::vector<char> v(B.size(),1);
    for(size_t i=0;i<B.size();++i) v[i]= (i>=200 && B[i].c < s[i]) ? 0 : 1;
    return v;
}
static std::vector<Clip> run_cell(GoldTrendMimicBook::Config cfg, const std::vector<Entry>& ents,
                                  const std::vector<Bar>& B, const std::vector<char>& bull, int uid){
    char sp[256], cp[256];
    std::snprintf(sp,sizeof sp,"/tmp/mgcmimic/state_%d.txt",uid);
    std::snprintf(cp,sizeof cp,"/tmp/mgcmimic/closed_%d.csv",uid);
    std::remove(sp); std::remove(cp); std::string spo=std::string(sp)+".open"; std::remove(spo.c_str());
    cfg.state_path=sp; cfg.closed_path=cp;
    GoldTrendMimicBook book(cfg);
    std::vector<Clip> clips;
    const double rt=cfg.rt_cost_bp/100.0;
    auto bull_at=[&](int64_t ts)->char{ int lo=0,hi=(int)B.size()-1,r=0;
        while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;} return bull[r]; };
    book.set_exec(nullptr,nullptr,nullptr,
        [&](const std::string&,const std::string&,bool is_long,double entry,double fill,double,
            int64_t ets,int64_t xts,const char*,double,double){
            double dir=is_long?1.0:-1.0;
            double gross=dir*(fill/entry-1.0)*100.0;
            clips.push_back({gross, gross-rt, ets, xts, bull_at(xts)});
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
struct Stat{ int n=0; double net=0,pf=0,worstn=1e9,mdd=0; int nneg_n=0,wins=0; };
static Stat stats(const std::vector<Clip>& c){
    Stat s; s.n=c.size(); if(!s.n){ s.worstn=0; return s; }
    double gp=0,gn=0; for(auto&x:c){ s.net+=x.net; if(x.net>0)gp+=x.net; else gn+=-x.net;
        if(x.net>1e-9)s.wins++; s.worstn=std::min(s.worstn,x.net);
        if(x.net<-1e-9)s.nneg_n++; }
    s.pf = gn>1e-9? gp/gn : 1e9;
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&a,const Clip&b){return a.xts<b.xts;});
    double cum=0,pk=0; for(auto&x:sc){ cum+=x.net; if(cum>pk)pk=cum; if(cum-pk<s.mdd)s.mdd=cum-pk; }
    return s;
}
static void halves(const std::vector<Clip>&c,std::vector<Clip>&a,std::vector<Clip>&b){
    std::vector<Clip> sc=c; std::sort(sc.begin(),sc.end(),[](const Clip&x,const Clip&y){return x.xts<y.xts;});
    size_t mid=sc.size()/2; a.assign(sc.begin(),sc.begin()+mid); b.assign(sc.begin()+mid,sc.end());
}
static std::vector<Clip> filt_bull(const std::vector<Clip>&c,char b){
    std::vector<Clip> r; for(auto&x:c) if(x.bull==b) r.push_back(x); return r; }

static std::vector<Entry> read_entries(const std::string& path,const std::vector<Bar>&B){
    std::vector<Entry> e; std::ifstream f(path);
    if(!f){ std::fprintf(stderr,"MISSING entries %s\n",path.c_str()); return e; }
    std::string ln;
    while(std::getline(f,ln)){ if(ln.empty())continue;
        std::vector<std::string>p; std::string t; std::stringstream ss(ln);
        while(std::getline(ss,t,',')) p.push_back(t); if(p.size()<3) continue;
        int64_t ts=std::strtoll(p[0].c_str(),nullptr,10); if(ts>4000000000LL) ts/=1000;
        int dir=std::atoi(p[1].c_str()); double px=std::strtod(p[2].c_str(),nullptr);
        int lo=0,hi=(int)B.size()-1,r=0;
        while(lo<=hi){int m=(lo+hi)/2; if(B[m].ts<=ts){r=m;lo=m+1;}else hi=m-1;}
        e.push_back({r,dir,px});
    }
    return e;
}

static int g_uid=1;
static void eval_cfg(const std::string& tag,const std::vector<Entry>&ents,
                     const std::vector<Bar>&B,const std::vector<char>&bl,
                     double arm,double lc,int cap,int pend,double be,double gbT,double gbW,
                     double rt,bool bull_only){
    GoldTrendMimicBook::Config c; c.trigger_tag=tag; c.live_sym="XAUUSD.M";
    c.legs={{"T",gbT},{"W",gbW}};
    c.arm_pct=arm; c.lc_pct=lc; c.cap_bars=cap; c.pend_bars=pend;
    c.be_entry_pct=be; c.rt_cost_bp=rt; c.no_prebe_loss=true;
    c.bull_only=bull_only; c.notional=10000;
    auto clips=run_cell(c,ents,B,bl,g_uid++);
    auto s=stats(clips);
    std::vector<Clip> h1,h2; halves(clips,h1,h2); auto s1=stats(h1),s2=stats(h2);
    Stat sb=stats(filt_bull(clips,1)), sr=stats(filt_bull(clips,0));
    bool reg_ok = (sb.n==0 || sb.net>0) && (sr.n==0 || sr.net>=0);
    bool pass = s.n>0 && s.net>0 && s.pf>=1.3 && s1.net>0 && s2.net>0 && reg_ok;
    char pfb[16]; if(s.pf>=1e8) snprintf(pfb,sizeof pfb,"inf"); else snprintf(pfb,sizeof pfb,"%.2f",s.pf);
    std::fprintf(stderr,"  arm%.2f cap%-3d pend%-2d be%.2f gb%.2f/%.2f rt%2.0f | n=%4d net=%+8.1f PF=%-5s worstN=%+5.2f nNeg=%4d mdd=%+7.1f | H1%+7.1f H2%+7.1f bull%+7.1f bear%+7.1f | %s\n",
        arm,cap,pend,be,gbT,gbW,rt, s.n,s.net,pfb,s.worstn,s.nneg_n,s.mdd,
        s1.net,s2.net,sb.net,sr.net, pass?"PASS":"FAIL");
}

static int run_mimic(const std::string& entries_csv,const std::string& bars_csv){
    system("mkdir -p /tmp/mgcmimic");
    std::vector<Bar> B; { auto rows=load_rows(bars_csv); for(auto&r:rows) B.push_back({r.ts,r.o,r.h,r.l,r.c}); }
    if(B.empty()){ std::fprintf(stderr,"no bars\n"); return 1; }
    auto bl=bull_vec(B);
    // BOOK-STANDARD regime gate (production GoldTrendMimicLadder registry feeds
    // books the XAU H1 close vs SMA200(H1)): env REGIME_H1=<h1 csv> replaces the
    // fed-bar-file SMA200 with the H1-SMA200 bull flag sampled at each bar ts.
    if(const char* rh1=getenv("REGIME_H1")){
        std::vector<Bar> H; { auto rows=load_rows(rh1); for(auto&r:rows) H.push_back({r.ts,r.o,r.h,r.l,r.c}); }
        if(!H.empty()){
            auto hbl=bull_vec(H);
            for(size_t i=0;i<B.size();++i){
                int lo=0,hi=(int)H.size()-1,r=0;
                while(lo<=hi){int m=(lo+hi)/2; if(H[m].ts<=B[i].ts){r=m;lo=m+1;}else hi=m-1;}
                bl[i]=hbl[r];
            }
            std::fprintf(stderr,"[mimic] regime gate = H1-SMA200 from %s (%zu H1 bars)\n",rh1,H.size());
        }
    }
    auto ents=read_entries(entries_csv,B);
    if(ents.empty()){ std::fprintf(stderr,"0 ENTRIES\n"); return 1; }
    const double RT   = getenv("RT")?atof(getenv("RT")):15.0;
    const int SWEEP   = getenv("SWEEP")?atoi(getenv("SWEEP")):0;
    const bool BULLG  = getenv("BULL_ONLY")?atoi(getenv("BULL_ONLY"))!=0:false;
    const std::string TAG = getenv("TAG")?getenv("TAG"):"MgcBook";
    std::fprintf(stderr,"[mimic] %s over %s  bars=%zu ents=%zu RT=%.0fbp bull_only=%d floored(no_prebe_loss)\n",
                 TAG.c_str(),bars_csv.c_str(),B.size(),ents.size(),RT,(int)BULLG);
    if(!SWEEP){
        const double arm=getenv("ARM")?atof(getenv("ARM")):0.25;
        const double lc =getenv("LC") ?atof(getenv("LC")) :1.5;
        const int cap   =getenv("CAP")?atoi(getenv("CAP")):12;
        const int pend  =getenv("PEND")?atoi(getenv("PEND")):6;
        const double be =getenv("BE") ?atof(getenv("BE")) :0.15;
        const double gbT=getenv("GBT")?atof(getenv("GBT")):0.08;
        const double gbW=getenv("GBW")?atof(getenv("GBW")):0.20;
        eval_cfg(TAG,ents,B,bl,arm,lc,cap,pend,be,gbT,gbW,RT,BULLG);
        eval_cfg(TAG,ents,B,bl,arm,lc,cap,pend,be,gbT,gbW,2.0*RT,BULLG);
        return 0;
    }
    for(double be : {0.15,0.30})
    for(double arm : {0.15,0.25,0.50,1.00})
    for(int cap : {6,12,24,48})
    for(int pend : {6,12})
    for(auto gb : std::vector<std::pair<double,double>>{{0.08,0.20},{0.10,0.30}}){
        eval_cfg(TAG,ents,B,bl,arm,1.5,cap,pend,be,gb.first,gb.second,RT,BULLG);
        eval_cfg(TAG,ents,B,bl,arm,1.5,cap,pend,be,gb.first,gb.second,2.0*RT,BULLG);
    }
    return 0;
}

int main(int argc,char**argv){
    if(!freopen("/dev/null","w",stdout)){}   // engine/init printf spam -> report on stderr
    if(argc<2){ std::fprintf(stderr,"usage: %s entries <base_csv> <outdir> | mimic <entries_csv> <bars_csv>\n",argv[0]); return 1; }
    std::string mode=argv[1];
    if(mode=="entries" && argc>=4) return run_entries(argv[2],argv[3]);
    if(mode=="mimic"   && argc>=4) return run_mimic(argv[2],argv[3]);
    std::fprintf(stderr,"bad args\n"); return 1;
}
