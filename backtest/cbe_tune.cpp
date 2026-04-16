// cbe_tune.cpp -- Advanced CBE tuning sweep
// Focus: fix 17/43 timeouts, improve WR, test additional gates
// Build: clang++ -O3 -std=c++20 -o /tmp/cbe_tune cbe_tune.cpp
// Run:   /tmp/cbe_tune ~/Downloads/l2_ticks_2026-04-09.csv ... (all 6 files)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include <map>

struct Tick { int64_t ms; double bid, ask, drift; int slot; double vol_ratio; };

static int slot_from_ms(int64_t ms) {
    int h = (int)(((ms / 1000LL) % 86400LL) / 3600LL);
    if (h >= 7  && h < 9)  return 1;  // London open
    if (h >= 9  && h < 11) return 2;  // London mid
    if (h >= 11 && h < 13) return 3;  // London/NY overlap
    if (h >= 13 && h < 17) return 4;  // NY core
    if (h >= 17 && h < 20) return 5;  // NY close
    return 6;
}

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return false; }
    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,cvr=-1,ci=0;
    { std::istringstream h(line);
      while (std::getline(h,tok,',')) {
        if(tok=="ts_ms")    cm=ci;
        if(tok=="bid")      cb=ci;
        if(tok=="ask")      ca=ci;
        if(tok=="ewm_drift")cd=ci;
        if(tok=="vol_ratio")cvr=ci;
        ++ci; }}
    if (cb<0||ca<0) return false;
    size_t before=out.size();
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0;
        flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c)
            if(*c==','){*c='\0';flds[nf++]=c+1;}
        int need=std::max({cm,cb,ca,cd});
        if(nf<=need) continue;
        try {
            Tick t;
            t.ms   =(int64_t)std::stod(flds[cm]);
            t.bid  =std::stod(flds[cb]);
            t.ask  =std::stod(flds[ca]);
            t.drift=(cd>=0&&nf>cd)?std::stod(flds[cd]):0.0;
            t.vol_ratio=(cvr>=0&&nf>cvr)?std::stod(flds[cvr]):1.0;
            t.slot =slot_from_ms(t.ms);
            out.push_back(t);
        } catch(...) {}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

// ─── Bar tracker ─────────────────────────────────────────────────────────────
struct M1Bar { double hi,lo,cl; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=0,rsi=50;
    std::deque<double> rg,rl; double rp=0;
    M1Bar cur={}; int64_t cbms=0;

    bool update(double bid,double ask,int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        if(rp>0){double chg=mid-rp;
            rg.push_back(chg>0?chg:0);rl.push_back(chg<0?-chg:0);
            if((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if((int)rg.size()==14){double ag=0,al=0;
                for(auto x:rg)ag+=x;for(auto x:rl)al+=x;ag/=14;al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al);}}
        rp=mid;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur);
            if((int)bars.size()>50)bars.pop_front();
            if((int)bars.size()>=2){double sum=0;int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i];auto& pb=bars[i-1];
                    sum+=std::max({b.hi-b.lo,std::fabs(b.hi-pb.cl),std::fabs(b.lo-pb.cl)});}
                atr=sum/n;}
            cur={mid,mid,mid};cbms=bms;nb=true;
        } else {if(mid>cur.hi)cur.hi=mid;if(mid<cur.lo)cur.lo=mid;cur.cl=mid;}
        return nb;
    }
};

// ─── Config ───────────────────────────────────────────────────────────────────
struct Config {
    // Compression
    int    comp_bars   = 3;
    double comp_mult   = 1.5;
    // Break
    double break_frac  = 0.30;
    // Trade
    double tp_rr       = 1.5;
    bool   block_long  = true;
    // NEW: minimum drift strength at entry
    double min_drift   = 0.5;    // |drift| must be >= this at entry
    // NEW: timeout variation
    int    timeout_s   = 300;    // seconds (300=5min, 120=2min, 180=3min)
    // NEW: session filter (which slots allowed)
    // 0=all, 1=London-only(1-3), 2=NY-only(4-5), 3=London+NY(1-5)
    int    session_filter = 3;
    // NEW: vol_ratio gate -- block if vol_ratio > threshold (chop)
    // 0=disabled, else block when vol_ratio > val and |drift| < 1.0
    double chop_vol_ratio = 0.0;
    // NEW: minimum compression bars (stricter = 4+)
    int    min_comp_consec = 3;
    // NEW: re-entry after compression break (require N bars compression again)
    // always 3 in this version
};

// ─── Engine ──────────────────────────────────────────────────────────────────
struct TradeResult {
    bool   is_long;
    double entry, exit_px, pnl, mfe;
    int    held_s;
    int    slot;
    std::string reason;
};

struct Eng {
    Config cfg;

    struct Pos {
        bool   active=false,is_long=false,be_done=false,trail_armed=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0,trail_sl=0,atr=0;
        double chi=0,clo=0;
        int64_t ets=0;
        int slot=0;
    } pos;

    std::deque<double> bhi,blo;
    int    cc=0; bool armed=false;
    double chi=0,clo=0,atr_c=0,rsi_c=50;
    int64_t last_exit=0,startup=0;
    double  dpnl=0; int64_t dday=0;

    using CB=std::function<void(const TradeResult&)>;

    void on_bar(double hi,double lo,double atr14,double rsi14) {
        atr_c=(atr14>0.5)?atr14:atr_c; rsi_c=rsi14;
        bhi.push_back(hi); blo.push_back(lo);
        if((int)bhi.size()>cfg.comp_bars){bhi.pop_front();blo.pop_front();}
        if((int)bhi.size()<2) return;
        double whi=*std::max_element(bhi.begin(),bhi.end());
        double wlo=*std::min_element(blo.begin(),blo.end());
        double rng=whi-wlo, thr=atr_c*cfg.comp_mult;
        if(atr_c>0&&rng<thr){++cc;
            if(cc>=cfg.min_comp_consec){armed=true;chi=whi;clo=wlo;}}
        else{cc=0;if(!pos.active)armed=false;}
    }

    void on_tick(double bid,double ask,int64_t ms,double drift,int slot,
                 double vol_ratio,CB on_close) {
        if(startup==0)startup=ms;
        if(ms-startup<90000LL)return;
        int64_t day=(ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-150.0)return;
        double mid=(bid+ask)*0.5;

        if(pos.active){
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.40){pos.sl=pos.entry;pos.trail_sl=pos.entry;pos.be_done=true;}
            if(!pos.trail_armed&&td>0&&pos.mfe>=td*0.50)pos.trail_armed=true;
            if(pos.trail_armed){double tdist=pos.atr*0.40;
                double nt=pos.is_long?(mid-tdist):(mid+tdist);
                if(pos.is_long&&nt>pos.trail_sl)pos.trail_sl=nt;
                if(!pos.is_long&&nt<pos.trail_sl)pos.trail_sl=nt;
                if(pos.is_long&&pos.trail_sl>pos.sl)pos.sl=pos.trail_sl;
                if(!pos.is_long&&pos.trail_sl<pos.sl)pos.sl=pos.trail_sl;}
            double ep=pos.is_long?bid:ask;
            if(pos.is_long?(bid>=pos.tp):(ask<=pos.tp)){_close(ep,"TP",ms,on_close);return;}
            bool sh=pos.is_long?(bid<=pos.sl):(ask>=pos.sl);
            if(sh){const char* r=pos.trail_armed?"TRAIL":pos.be_done?"BE":"SL";
                _close(ep,r,ms,on_close);return;}
            int64_t to=((int64_t)cfg.timeout_s)*1000LL;
            if(ms-pos.ets>to){_close(ep,"TIMEOUT",ms,on_close);return;}
            return;
        }

        if(!armed||atr_c<=0)return;
        if(ms-last_exit<30000LL)return;
        if((ask-bid)>atr_c*0.30)return;

        // Session filter
        bool slot_ok=false;
        switch(cfg.session_filter){
            case 1: slot_ok=(slot>=1&&slot<=3); break; // London
            case 2: slot_ok=(slot>=4&&slot<=5); break; // NY
            default: slot_ok=(slot>=1&&slot<=5); break; // all
        }
        if(!slot_ok)return;

        // Chop gate
        if(cfg.chop_vol_ratio>0&&vol_ratio>cfg.chop_vol_ratio&&std::fabs(drift)<1.0)return;

        double bm=atr_c*cfg.break_frac;
        bool lb=(bid>=chi+bm),sb=(ask<=clo-bm);
        if(!lb&&!sb)return;
        bool isl=lb;
        if(isl&&drift<=0)return;
        if(!isl&&drift>=0)return;
        // Min drift strength
        if(std::fabs(drift)<cfg.min_drift)return;
        if(isl&&rsi_c>72)return;
        if(!isl&&rsi_c<22)return;
        if(cfg.block_long&&isl)return;

        double ep=isl?ask:bid,buf=0.50;
        double sld=isl?(ep-(clo-buf)):((chi+buf)-ep);
        if(sld<=0||sld>atr_c*4.0)return;
        double tpp=sld*cfg.tp_rr;
        if(tpp<=(ask-bid)+0.20)return;
        double sz=std::max(0.01,std::min(0.10,std::floor(30.0/(sld*100.0)/0.001)*0.001));

        pos.active=true;pos.is_long=isl;pos.entry=ep;
        pos.sl=isl?(ep-sld):(ep+sld);pos.tp=isl?(ep+tpp):(ep-tpp);
        pos.size=sz;pos.mfe=0;pos.be_done=false;pos.trail_armed=false;
        pos.trail_sl=pos.sl;pos.atr=atr_c;pos.chi=chi;pos.clo=clo;
        pos.ets=ms;pos.slot=slot;
        armed=false;
    }

    void fc(double bid,double ask,int64_t ms,CB cb){
        if(!pos.active)return;_close(pos.is_long?bid:ask,"FC",ms,cb);}

    void _close(double ep,const char* r,int64_t ms,CB cb){
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        dpnl+=pnl;last_exit=ms;
        if(cb){TradeResult tr;tr.is_long=pos.is_long;tr.entry=pos.entry;
            tr.exit_px=ep;tr.pnl=pnl;tr.mfe=pos.mfe;
            tr.held_s=(int)((ms-pos.ets)/1000);tr.slot=pos.slot;tr.reason=r;cb(tr);}
        pos=Pos{};}
};

// ─── Stats ────────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0;double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,trail=0,be=0,tmo=0,fc=0;
    double avg_mfe_win=0,avg_mfe_loss=0;
    int mfe_win_n=0,mfe_loss_n=0;
    std::map<int,std::pair<int,double>> by_slot; // slot -> {trades,pnl}

    void record(const TradeResult& t){
        ++T;if(t.pnl>0)++W;pnl+=t.pnl;
        peak=std::max(peak,pnl);
        max_dd=std::max(max_dd,peak-pnl);
        if(t.reason=="TP")++tp;
        else if(t.reason=="SL")++sl;
        else if(t.reason=="TRAIL")++trail;
        else if(t.reason=="BE")++be;
        else if(t.reason=="TIMEOUT")++tmo;
        else if(t.reason=="FC")++fc;
        if(t.pnl>0){avg_mfe_win+=t.mfe;++mfe_win_n;}
        else{avg_mfe_loss+=t.mfe;++mfe_loss_n;}
        by_slot[t.slot].first++;
        by_slot[t.slot].second+=t.pnl;
    }
    double wr()const{return T>0?100.0*W/T:0;}
    double avg()const{return T>0?pnl/T:0;}
    double mfe_w()const{return mfe_win_n>0?avg_mfe_win/mfe_win_n:0;}
    double mfe_l()const{return mfe_loss_n>0?avg_mfe_loss/mfe_loss_n:0;}
};

static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    Eng eng; eng.cfg=cfg;
    BarTracker bt; Stats s;
    auto cb=[&](const TradeResult& t){s.record(t);};
    for(auto& t:ticks){
        bool nb=bt.update(t.bid,t.ask,t.ms);
        if(nb&&(int)bt.bars.size()>=2){auto& lb=bt.bars.back();
            eng.on_bar(lb.hi,lb.lo,bt.atr,bt.rsi);}
        eng.on_tick(t.bid,t.ask,t.ms,t.drift,t.slot,t.vol_ratio,cb);
    }
    if(eng.pos.active&&!ticks.empty()){auto& lt=ticks.back();
        eng.fc(lt.bid,lt.ask,lt.ms,cb);}
    return s;
}

int main(int argc,char** argv){
    if(argc<2){fprintf(stderr,"Usage: cbe_tune file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if(ticks.empty())return 1;

    // ── Phase 1: baseline ─────────────────────────────────────────────────────
    Config baseline;
    baseline.comp_bars=3;baseline.comp_mult=1.5;baseline.break_frac=0.30;
    baseline.tp_rr=1.5;baseline.block_long=true;baseline.min_drift=0.0;
    baseline.timeout_s=300;baseline.session_filter=3;baseline.chop_vol_ratio=0.0;
    Stats base=run(ticks,baseline);
    printf("BASELINE: T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f DD=$%.2f "
           "TP=%d SL=%d TRAIL=%d BE=%d TMO=%d\n\n",
           base.T,base.W,base.wr(),base.pnl,base.avg(),base.max_dd,
           base.tp,base.sl,base.trail,base.be,base.tmo);

    // ── Phase 2: targeted sweep ────────────────────────────────────────────────
    struct Result{Config c;Stats s;};
    std::vector<Result> results;

    // Sweep dimensions focused on the problems:
    // 1. Timeout (17/43): test 60s,90s,120s,180s,240s,300s
    // 2. Min drift gate: 0.0,0.3,0.5,0.8,1.0,1.5
    // 3. TP RR: 1.0,1.5,2.0,2.5
    // 4. Break frac: 0.20,0.30,0.40,0.50
    // 5. Session: London-only vs NY-only vs all
    // 6. Chop vol gate: 0(off),1.2,1.5

    for(int to_s  : {60,90,120,180,240,300})
    for(double md : {0.0,0.3,0.5,0.8,1.0,1.5})
    for(double rr : {1.0,1.5,2.0,2.5})
    for(double bf : {0.20,0.30,0.40,0.50})
    for(int sf    : {1,2,3})          // London, NY, all
    for(double cv : {0.0,1.2,1.5}) {  // chop gate off / 1.2 / 1.5
        Config c;
        c.comp_bars=3;c.comp_mult=1.5;c.break_frac=bf;
        c.tp_rr=rr;c.block_long=true;c.min_drift=md;
        c.timeout_s=to_s;c.session_filter=sf;c.chop_vol_ratio=cv;
        c.min_comp_consec=3;
        Stats s=run(ticks,c);
        results.push_back({c,s});
    }

    std::sort(results.begin(),results.end(),
        [](auto& a,auto& b){return a.s.pnl>b.s.pnl;});

    // Top 30
    printf("%-4s %-4s %-5s %-4s %-3s %-4s %-4s %5s %4s %8s %5s %6s "
           "%4s %4s %4s %4s %4s\n",
           "TO","MD","BRK","RR","SF","CHV","CC",
           "T","W","PNL","WR%","Avg",
           "TP","SL","TRL","BE","TMO");
    printf("%s\n",std::string(100,'-').c_str());

    int shown=0;
    for(auto& r:results){
        if(r.s.T<15)continue;
        if(shown++>=30)break;
        printf("%-4d %-4.1f %-5.2f %-4.1f %-3d %-4.1f %-4d "
               "%5d %4d %8.2f %5.1f %6.2f "
               "%4d %4d %4d %4d %4d\n",
               r.c.timeout_s,r.c.min_drift,r.c.break_frac,r.c.tp_rr,
               r.c.session_filter,r.c.chop_vol_ratio,r.c.min_comp_consec,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),
               r.s.tp,r.s.sl,r.s.trail,r.s.be,r.s.tmo);
    }

    // Best config detail
    printf("\n=== TOP 3 CONFIGS DETAIL ===\n");
    shown=0;
    for(auto& r:results){
        if(r.s.T<15)continue;
        if(shown++>=3)break;
        printf("\n#%d: to=%ds md=%.1f bf=%.2f rr=%.1f sf=%d chv=%.1f\n",
               shown,r.c.timeout_s,r.c.min_drift,r.c.break_frac,
               r.c.tp_rr,r.c.session_filter,r.c.chop_vol_ratio);
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  Exit: TP=%d SL=%d TRAIL=%d BE=%d TIMEOUT=%d\n",
               r.s.tp,r.s.sl,r.s.trail,r.s.be,r.s.tmo);
        printf("  AvgMFE win=%.2f loss=%.2f\n",r.s.mfe_w(),r.s.mfe_l());
        printf("  By slot:");
        for(auto& kv:r.s.by_slot)
            printf(" s%d:T%d($%.0f)",kv.first,kv.second.first,kv.second.second);
        printf("\n");
    }

    // Timeout analysis on baseline
    printf("\n=== TIMEOUT BREAKDOWN (baseline) ===\n");
    printf("Timeout at %ds: %d trades (%.0f%% of total)\n",
           baseline.timeout_s,base.tmo,base.T>0?100.0*base.tmo/base.T:0);
    printf("Reducing timeout: how many trades survive vs timeout at shorter windows?\n");
    for(int to_s:{30,60,90,120,180,240,300}){
        Config c=baseline;c.timeout_s=to_s;
        Stats s=run(ticks,c);
        printf("  to=%3ds: T=%d W=%d WR=%.1f%% PnL=$%7.2f Avg=$%.2f "
               "TMO=%d(%.0f%%)\n",
               to_s,s.T,s.W,s.wr(),s.pnl,s.avg(),
               s.tmo,s.T>0?100.0*s.tmo/s.T:0);
    }

    // Min drift analysis
    printf("\n=== MIN DRIFT GATE (baseline + vary drift) ===\n");
    for(double md:{0.0,0.2,0.3,0.5,0.8,1.0,1.2,1.5,2.0}){
        Config c=baseline;c.min_drift=md;
        Stats s=run(ticks,c);
        printf("  drift>=%.1f: T=%d W=%d WR=%.1f%% PnL=$%7.2f Avg=$%.2f\n",
               md,s.T,s.W,s.wr(),s.pnl,s.avg());
    }

    // Session analysis
    printf("\n=== SESSION FILTER ===\n");
    for(int sf:{1,2,3}){
        Config c=baseline;c.session_filter=sf;
        Stats s=run(ticks,c);
        const char* sn=(sf==1)?"London":(sf==2)?"NY":"All";
        printf("  %s: T=%d W=%d WR=%.1f%% PnL=$%7.2f Avg=$%.2f\n",
               sn,s.T,s.W,s.wr(),s.pnl,s.avg());
    }

    // Combined best
    printf("\n=== POSITIVE PnL >= 20 TRADES ===\n");
    printf("%-4s %-4s %-5s %-4s %-3s %-4s %5s %4s %8s %5s %6s %6s\n",
           "TO","MD","BRK","RR","SF","CHV","T","W","PNL","WR%","Avg","MaxDD");
    for(auto& r:results){
        if(r.s.T<20||r.s.pnl<=0)continue;
        printf("%-4d %-4.1f %-5.2f %-4.1f %-3d %-4.1f "
               "%5d %4d %8.2f %5.1f %6.2f %6.2f\n",
               r.c.timeout_s,r.c.min_drift,r.c.break_frac,r.c.tp_rr,
               r.c.session_filter,r.c.chop_vol_ratio,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }

    return 0;
}
