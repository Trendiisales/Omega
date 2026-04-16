// bb_sweep.cpp -- Bollinger Band Mean Reversion sweep for XAUUSD
//
// STRATEGY (FTMO-published, widely backtested on gold, fires multiple times/session):
//
//   Bollinger Bands (SMA20, 2SD) on M1 bars detect overextension.
//   Entry when price CLOSES OUTSIDE the band AND momentum is waning (RSI divergence).
//   Target: price returns to the BB midline (SMA20).
//   Both LONG and SHORT. No session restriction.
//
// ENTRY CONDITIONS (SHORT example -- price too high):
//   1. M1 bar closes ABOVE upper BB (price overextended)
//   2. RSI > rsi_ob threshold (overbought, e.g. 65)
//   3. RSI is LOWER than it was 2 bars ago (momentum weakening -- divergence)
//   4. EMA gap filter: fast EMA NOT still expanding strongly (not in pure trend)
//   5. ATR filter: bar size reasonable (not a news spike)
//   SL: above the bar high + buffer
//   TP: BB midline (SMA20) -- mean reversion target
//
// Why this fits gold in 2026:
//   - EMA diag confirmed gold M1 mean-reverts: rr=1.0 beat rr=2.0
//   - 47 timeouts had MFE p50=1.58 -- price overshot then snapped back
//   - Hours 09,10,13,15,17 UTC are all liquid -- BB fires throughout
//   - No session restriction needed -- overextensions happen everywhere
//
// SWEEP PARAMETERS:
//   bb_period:    15, 20, 25       (SMA period for bands)
//   bb_std:       1.5, 2.0, 2.5   (standard deviation multiplier)
//   rsi_ob:       60, 65, 70       (overbought threshold for shorts)
//   rsi_os:       30, 35, 40       (oversold threshold for longs)
//   rsi_div:      true/false       (require RSI divergence / weakening)
//   sl_mult:      0.5, 1.0, 1.5   (SL = bar_range * mult beyond the wick)
//   require_close_outside: true   (bar must CLOSE outside, not just wick)
//   hour_kill:    true/false       (block hours 05,12,14,16,20 -- confirmed bad)
//   min_bb_width: 0.5, 1.0        (minimum band width to trade -- avoids chop)
//
// Build: clang++ -O3 -std=c++20 -o /tmp/bb_sweep bb_sweep.cpp
// Run:   /tmp/bb_sweep ~/Downloads/l2_ticks_2026-04-09.csv ... (6 files)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if(!f){fprintf(stderr,"Cannot open %s\n",path);return false;}
    std::string line,tok; std::getline(f,line);
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,ci=0;
    {std::istringstream h(line);while(std::getline(h,tok,',')){
        if(tok=="ts_ms")cm=ci;if(tok=="bid")cb=ci;
        if(tok=="ask")ca=ci;if(tok=="ewm_drift")cd=ci;++ci;}}
    if(cb<0||ca<0)return false;
    size_t before=out.size();
    while(std::getline(f,line)){
        if(line.empty())continue;
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        static char buf[512];if(line.size()>=sizeof(buf))continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32];int nf=0;flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c)if(*c==','){*c='\0';flds[nf++]=c+1;}
        int need=std::max({cm,cb,ca,cd});if(nf<=need)continue;
        try{Tick t;t.ms=(int64_t)std::stod(flds[cm]);
            t.bid=std::stod(flds[cb]);t.ask=std::stod(flds[ca]);
            t.drift=(cd>=0&&nf>cd)?std::stod(flds[cd]):0.0;
            out.push_back(t);}catch(...){}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

static int utc_hour(int64_t ms){return(int)(((ms/1000LL)%86400LL)/3600LL);}

// ─── M1 bar + BB + RSI tracker ───────────────────────────────────────────────
struct M1Bar{double open,high,low,close;int64_t bms;};

struct Indicators {
    // BB
    std::deque<double> closes;
    double bb_mid=0,bb_upper=0,bb_lower=0,bb_width=0;
    // RSI (tick-level for responsiveness)
    std::deque<double> rg,rl; double rp=0,rsi=50;
    // RSI history for divergence
    std::deque<double> rsi_hist;
    // ATR
    double atr=2.0;
    std::deque<M1Bar> bars;
    M1Bar cur{}; int64_t cbms=0;

    bool update(double bid, double ask, int64_t ms, int bb_period, double bb_std) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        // RSI tick update
        if(rp>0){double chg=mid-rp;
            rg.push_back(chg>0?chg:0);rl.push_back(chg<0?-chg:0);
            if((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if((int)rg.size()==14){double ag=0,al=0;
                for(auto x:rg)ag+=x;for(auto x:rl)al+=x;ag/=14;al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al);}}
        rp=mid;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid,bms};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur);if((int)bars.size()>50)bars.pop_front();
            // ATR
            if((int)bars.size()>=2){
                double sum=0;int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i];auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,
                                   std::fabs(b.high-pb.close),
                                   std::fabs(b.low-pb.close)});}
                atr=sum/n;}
            // BB on bar closes
            closes.push_back(cur.close);
            if((int)closes.size()>bb_period)closes.pop_front();
            if((int)closes.size()==bb_period){
                double sum2=0;for(auto x:closes)sum2+=x;
                bb_mid=sum2/bb_period;
                double var=0;for(auto x:closes)var+=(x-bb_mid)*(x-bb_mid);
                double sd=std::sqrt(var/bb_period);
                bb_upper=bb_mid+bb_std*sd;
                bb_lower=bb_mid-bb_std*sd;
                bb_width=bb_upper-bb_lower;}
            // RSI history
            rsi_hist.push_back(rsi);
            if((int)rsi_hist.size()>5)rsi_hist.pop_front();
            cur={mid,mid,mid,mid,bms};cbms=bms;nb=true;
        } else {
            if(mid>cur.high)cur.high=mid;
            if(mid<cur.low)cur.low=mid;
            cur.close=mid;}
        return nb;
    }

    bool rsi_weakening_bearish() const {
        // RSI peaked 2+ bars ago and is now lower (momentum fading on high)
        if((int)rsi_hist.size()<3)return false;
        int n=(int)rsi_hist.size();
        return rsi_hist[n-1]<rsi_hist[n-2]&&rsi_hist[n-2]<rsi_hist[n-3];
    }
    bool rsi_weakening_bullish() const {
        if((int)rsi_hist.size()<3)return false;
        int n=(int)rsi_hist.size();
        return rsi_hist[n-1]>rsi_hist[n-2]&&rsi_hist[n-2]>rsi_hist[n-3];
    }
};

// ─── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int    bb_period   = 20;
    double bb_std      = 2.0;
    double rsi_ob      = 65.0;  // overbought: short when RSI > this
    double rsi_os      = 35.0;  // oversold: long when RSI < this
    bool   rsi_div     = true;  // require RSI weakening (divergence)
    double sl_mult     = 1.0;   // SL = bar_range * sl_mult beyond wick
    double min_bb_width= 1.0;   // min band width (filters chop)
    bool   hour_kill   = true;  // block hours 05,12,14,16,20 (EMA-confirmed bad)
    int    timeout_s   = 180;
    int    cooldown_s  = 30;
};

// ─── Stats ───────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,timeout=0;
    void record(double p,const char* r){
        ++T;if(p>0)++W;pnl+=p;
        peak=std::max(peak,pnl);max_dd=std::max(max_dd,peak-pnl);
        if(strcmp(r,"TP")==0)++tp;
        else if(strcmp(r,"SL")==0||strcmp(r,"BE")==0)++sl;
        else ++timeout;}
    double wr()const{return T>0?100.0*W/T:0;}
    double avg()const{return T>0?pnl/T:0;}
};

static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    Indicators ind;
    Stats s;

    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0;
        int64_t ets=0;
    } pos;

    int64_t last_exit=0, startup=0;
    double dpnl=0; int64_t dday=0;

    for(auto& t:ticks){
        bool nb=ind.update(t.bid,t.ask,t.ms,cfg.bb_period,cfg.bb_std);
        if(startup==0)startup=t.ms;
        if(t.ms-startup<120000LL)continue;
        int64_t day=(t.ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-200.0)continue;

        double mid=(t.bid+t.ask)*0.5, spread=t.ask-t.bid;

        // Position management
        if(pos.active){
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.40){
                pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?t.bid:t.ask;
            if(pos.is_long?(t.bid>=pos.tp):(t.ask<=pos.tp)){
                dpnl+=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                s.record((pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100,"TP");
                last_exit=t.ms/1000;pos=Pos{};continue;}
            if(pos.is_long?(t.bid<=pos.sl):(t.ask>=pos.sl)){
                double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=pnl;s.record(pnl,pos.be_done?"BE":"SL");
                last_exit=t.ms/1000;pos=Pos{};continue;}
            if(t.ms-pos.ets>(int64_t)cfg.timeout_s*1000){
                double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=pnl;s.record(pnl,"TIMEOUT");
                last_exit=t.ms/1000;pos=Pos{};continue;}
            continue;
        }

        if(!nb)continue;  // only check entry at bar close
        if(ind.bb_width<cfg.min_bb_width)continue;
        if(ind.bars.empty())continue;
        auto& lb=ind.bars.back();
        if(t.ms/1000-last_exit<cfg.cooldown_s)continue;
        if(spread>ind.atr*0.25)continue;

        // Hour kill gate (EMA-confirmed bad hours)
        if(cfg.hour_kill){
            int h=utc_hour(t.ms);
            if(h==5||h==12||h==14||h==16||h==20)continue;}

        // Bar must be a reasonable size -- filter news spikes
        double bar_range=lb.high-lb.low;
        if(bar_range>ind.atr*3.0)continue;
        if(bar_range<0.05)continue;

        // ── SHORT: close above upper BB + RSI overbought ──────────────────
        if(lb.close>ind.bb_upper && lb.close>lb.open){
            if(ind.rsi<cfg.rsi_ob)goto check_long;
            if(cfg.rsi_div&&!ind.rsi_weakening_bearish())goto check_long;
            // SL above bar high + buffer
            double sl_px=lb.high+bar_range*cfg.sl_mult*0.5;
            double sl_dist=t.bid-sl_px;  // negative -- sl is above entry
            if(sl_dist>=0)goto check_long;
            sl_dist=std::fabs(sl_dist);
            double tp_px=ind.bb_mid;  // target: mean reversion to midline
            double tp_dist=t.bid-tp_px;
            if(tp_dist<sl_dist*0.5)goto check_long;  // need at least 0.5RR to midline
            double cost=spread+0.20;
            if(tp_dist<=cost)goto check_long;
            double sz=std::max(0.01,std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
            pos.active=true;pos.is_long=false;pos.entry=t.bid;
            pos.sl=sl_px;pos.tp=tp_px;pos.size=sz;
            pos.mfe=0;pos.be_done=false;pos.ets=t.ms;
            continue;
        }

        check_long:
        // ── LONG: close below lower BB + RSI oversold ─────────────────────
        if(lb.close<ind.bb_lower && lb.close<lb.open){
            if(ind.rsi>cfg.rsi_os)continue;
            if(cfg.rsi_div&&!ind.rsi_weakening_bullish())continue;
            double sl_px=lb.low-bar_range*cfg.sl_mult*0.5;
            double sl_dist=t.ask-sl_px;
            if(sl_dist<=0)continue;
            double tp_px=ind.bb_mid;
            double tp_dist=tp_px-t.ask;
            if(tp_dist<sl_dist*0.5)continue;
            double cost=spread+0.20;
            if(tp_dist<=cost)continue;
            double sz=std::max(0.01,std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
            pos.active=true;pos.is_long=true;pos.entry=t.ask;
            pos.sl=sl_px;pos.tp=tp_px;pos.size=sz;
            pos.mfe=0;pos.be_done=false;pos.ets=t.ms;
            continue;
        }
    }
    if(pos.active&&!ticks.empty()){
        auto& lt=ticks.back();
        double ep=pos.is_long?lt.bid:lt.ask;
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
        s.record(pnl,"TIMEOUT");}
    return s;
}

int main(int argc,char** argv){
    if(argc<2){fprintf(stderr,"Usage: bb_sweep file1.csv ...\n");return 1;}
    std::vector<Tick> ticks;ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if(ticks.empty())return 1;

    // Baseline
    Config base;
    Stats bs=run(ticks,base);
    printf("BASELINE (bb=20/2.0 rsi_ob=65 rsi_os=35 div=Y sl=1.0 bbw=1.0 hk=Y tmo=180):\n");
    printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f TP=%d SL=%d TMO=%d\n\n",
           bs.T,bs.W,bs.wr(),bs.pnl,bs.avg(),bs.max_dd,bs.tp,bs.sl,bs.timeout);

    struct Result{Config c;Stats s;};
    std::vector<Result> results;

    for(int bp  :{15,20,25})
    for(double bs2:{1.5,2.0,2.5})
    for(double ob:{60.0,65.0,70.0})
    for(double os:{30.0,35.0,40.0})
    for(bool  div:{true,false})
    for(double sl:{0.5,1.0,1.5})
    for(double bw:{0.5,1.0,1.5})
    for(bool  hk :{true,false})
    for(int   tmo:{120,180,300})
    {
        Config c;c.bb_period=bp;c.bb_std=bs2;c.rsi_ob=ob;c.rsi_os=os;
        c.rsi_div=div;c.sl_mult=sl;c.min_bb_width=bw;
        c.hour_kill=hk;c.timeout_s=tmo;
        results.push_back({c,run(ticks,c)});
    }

    std::sort(results.begin(),results.end(),
        [](auto& a,auto& b){return a.s.pnl>b.s.pnl;});

    printf("%-4s %-4s %-4s %-4s %-3s %-4s %-4s %-3s %-3s %5s %4s %8s %5s %6s %7s %3s %3s %3s\n",
           "BP","STD","OB","OS","DIV","SL","BBW","HK","TMO",
           "T","W","PNL","WR%","Avg","MaxDD","TP","SL","TMO");
    printf("%s\n",std::string(105,'-').c_str());

    int shown=0;
    for(auto& r:results){
        if(r.s.T<15)continue;
        if(shown++>=40)break;
        printf("%-4d %-4.1f %-4.0f %-4.0f %-3s %-4.1f %-4.1f %-3s %-3d "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f %3d %3d %3d\n",
               r.c.bb_period,r.c.bb_std,r.c.rsi_ob,r.c.rsi_os,
               r.c.rsi_div?"Y":"N",r.c.sl_mult,r.c.min_bb_width,
               r.c.hour_kill?"Y":"N",r.c.timeout_s,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd,
               r.s.tp,r.s.sl,r.s.timeout);
    }

    printf("\n=== TOP 3 DETAIL ===\n");
    shown=0;
    for(auto& r:results){
        if(r.s.T<15)continue;
        if(shown++>=3)break;
        printf("\n#%d: bb=%d/%.1f rsi_ob=%.0f rsi_os=%.0f div=%s sl=%.1f bbw=%.1f hk=%s tmo=%d\n",
               shown,r.c.bb_period,r.c.bb_std,r.c.rsi_ob,r.c.rsi_os,
               r.c.rsi_div?"Y":"N",r.c.sl_mult,r.c.min_bb_width,
               r.c.hour_kill?"Y":"N",r.c.timeout_s);
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  TP=%d SL=%d TMO=%d\n",r.s.tp,r.s.sl,r.s.timeout);
    }

    printf("\n=== POSITIVE PnL >= 15 TRADES ===\n");
    printf("%-4s %-4s %-4s %-4s %-3s %-4s %-4s %-3s %-3s %5s %4s %8s %5s %6s %7s\n",
           "BP","STD","OB","OS","DIV","SL","BBW","HK","TMO","T","W","PNL","WR%","Avg","MaxDD");
    shown=0;
    for(auto& r:results){
        if(r.s.T<15||r.s.pnl<=0)continue;
        if(shown++>=30)break;
        printf("%-4d %-4.1f %-4.0f %-4.0f %-3s %-4.1f %-4.1f %-3s %-3d "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f\n",
               r.c.bb_period,r.c.bb_std,r.c.rsi_ob,r.c.rsi_os,
               r.c.rsi_div?"Y":"N",r.c.sl_mult,r.c.min_bb_width,
               r.c.hour_kill?"Y":"N",r.c.timeout_s,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }
    return 0;
}
