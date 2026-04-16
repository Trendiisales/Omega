// ema_cross_sweep.cpp -- EMA Crossover Scalper sweep for XAUUSD
// Strategy: Most widely-used gold scalping engine across retail/prop/commercial EAs
//
// LOGIC:
//   Entry: Fast EMA crosses above/below Slow EMA AND RSI confirms direction
//   LONG:  fast > slow (crossover), RSI > rsi_lo (not oversold entry into down move)
//          AND RSI < rsi_hi (not overbought -- good room to run)
//   SHORT: fast < slow (crossunder), RSI < rsi_hi (not overbought entry into up move)
//          AND RSI > rsi_lo (not oversold -- good room to run)
//   SL:    ATR * sl_mult behind entry
//   TP:    SL * tp_rr
//   Exit:  Also exit on EMA cross reversal (common variant)
//
// Sweep parameters:
//   fast_ema: 5,7,9,12,14
//   slow_ema: 15,20,21,25,28
//   rsi_period: 7,9,14
//   rsi_lo: 40,45,50  (long requires RSI > this)
//   rsi_hi: 50,55,60  (short requires RSI < this)
//   sl_mult: 0.5,0.7,1.0,1.5
//   tp_rr: 1.0,1.5,2.0,2.5
//   cross_exit: true/false (exit when EMA crosses back)
//   both_dirs: true (both LONG and SHORT) -- the defining feature vs CBE
//
// Build: clang++ -O3 -std=c++20 -o /tmp/ema_sweep ema_cross_sweep.cpp
// Run:   /tmp/ema_sweep ~/Downloads/l2_ticks_2026-04-09.csv \
//                       ~/Downloads/l2_ticks_2026-04-10.csv \
//                       ~/Downloads/l2_ticks_2026-04-13.csv \
//                       ~/Downloads/l2_ticks_2026-04-14.csv \
//                       ~/Downloads/l2_ticks_2026-04-15.csv \
//                       ~/Downloads/l2_ticks_2026-04-16.csv

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; int slot; };

static int slot_from_ms(int64_t ms) {
    int h=(int)(((ms/1000LL)%86400LL)/3600LL);
    if(h>=7&&h<9)  return 1;
    if(h>=9&&h<11) return 2;
    if(h>=11&&h<13)return 3;
    if(h>=13&&h<17)return 4;
    if(h>=17&&h<20)return 5;
    return 6;
}

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if(!f) return false;
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
            t.slot=slot_from_ms(t.ms);out.push_back(t);}catch(...){}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

// ─── M1 Bar tracker ──────────────────────────────────────────────────────────
struct M1Bar { double open,high,low,close; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=0,rsi=50;
    std::deque<double> rg,rl; double rp=0;
    M1Bar cur={}; int64_t cbms=0; bool has_open=false;

    bool update(double bid,double ask,int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        // Tick RSI
        if(rp>0){double chg=mid-rp;
            rg.push_back(chg>0?chg:0);rl.push_back(chg<0?-chg:0);
            if((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if((int)rg.size()==14){double ag=0,al=0;
                for(auto x:rg)ag+=x;for(auto x:rl)al+=x;ag/=14;al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al);}}
        rp=mid;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid};cbms=bms;has_open=true;}
        else if(bms!=cbms){
            bars.push_back(cur);if((int)bars.size()>200)bars.pop_front();
            // ATR14 from bars
            if((int)bars.size()>=2){double sum=0;int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i];auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,std::fabs(b.high-pb.close),std::fabs(b.low-pb.close)});}
                atr=sum/n;}
            cur={mid,mid,mid,mid};cbms=bms;has_open=true;nb=true;
        } else {if(mid>cur.high)cur.high=mid;if(mid<cur.low)cur.low=mid;cur.close=mid;}
        return nb;
    }
};

// ─── EMA ─────────────────────────────────────────────────────────────────────
// Computed from bar closes for clean signals
struct EMACalc {
    int period;
    double val=0; bool warmed=false; int count=0;
    double alpha;
    EMACalc(int p): period(p), alpha(2.0/(p+1.0)) {}
    void update(double price) {
        if(!warmed){val=price;++count;if(count>=period)warmed=true;}
        else val=price*alpha+val*(1.0-alpha);
    }
};

// ─── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int    fast_ema   = 9;
    int    slow_ema   = 21;
    int    rsi_period = 14;  // uses bar tracker's tick RSI for now
    double rsi_lo     = 45;  // LONG: RSI must be > this
    double rsi_hi     = 55;  // SHORT: RSI must be < this
    double sl_mult    = 0.7; // SL = ATR * sl_mult
    double tp_rr      = 1.5; // TP = SL * tp_rr
    bool   cross_exit = true; // exit when EMA crosses back
    int    timeout_s  = 180;
    int    cooldown_s = 20;
};

// ─── Engine ──────────────────────────────────────────────────────────────────
struct TradeResult {
    bool is_long; double entry,exit_px,pnl,mfe; int held_s,slot; std::string reason;
};

struct EMAEngine {
    Config cfg;
    EMACalc fast_ema, slow_ema;

    EMAEngine(const Config& c): cfg(c), fast_ema(c.fast_ema), slow_ema(c.slow_ema) {}

    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0,atr=0;
        int64_t ets=0; int slot=0;
    } pos;

    double prev_fast=0,prev_slow=0;
    bool emas_warmed=false;
    int64_t last_exit=0,startup=0;
    double dpnl=0; int64_t dday=0;
    int64_t last_cross_ms=0;  // prevent re-entry on same cross tick

    using CB=std::function<void(const TradeResult&)>;

    void on_bar(double bar_close, double bar_rsi, double bar_atr) {
        double pf=fast_ema.val, ps=slow_ema.val;
        fast_ema.update(bar_close);
        slow_ema.update(bar_close);
        if(fast_ema.warmed&&slow_ema.warmed) emas_warmed=true;
        prev_fast=pf; prev_slow=ps;
    }

    void on_tick(double bid,double ask,int64_t ms,double rsi,double atr,int slot,CB on_close){
        if(startup==0)startup=ms;
        if(ms-startup<120000LL)return;  // 2 min warmup
        int64_t day=(ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-200.0)return;
        if(!emas_warmed)return;
        double mid=(bid+ask)*0.5;

        if(pos.active){
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            // BE at 50% of TP
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.50){
                pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?bid:ask;
            // TP
            if(pos.is_long?(bid>=pos.tp):(ask<=pos.tp)){_close(ep,"TP",ms,on_close);return;}
            // SL
            if(pos.is_long?(bid<=pos.sl):(ask>=pos.sl)){_close(ep,"SL",ms,on_close);return;}
            // Timeout
            if(ms-pos.ets>(int64_t)cfg.timeout_s*1000){_close(ep,"TIMEOUT",ms,on_close);return;}
            // Cross-exit: if EMA crosses against us
            if(cfg.cross_exit){
                bool cross_against=(pos.is_long&&fast_ema.val<slow_ema.val)||
                                   (!pos.is_long&&fast_ema.val>slow_ema.val);
                if(cross_against&&pos.mfe>0){_close(ep,"CROSS_EXIT",ms,on_close);return;}
            }
            return;
        }

        // Entry
        if(atr<=0)return;
        if((ask-bid)>atr*0.30)return;  // spread gate
        if(ms/1000-last_exit<cfg.cooldown_s)return;
        if(slot<1||slot>5)return;
        if(ms==last_cross_ms)return;  // same tick as cross -- skip

        // Cross detection: fast just crossed slow on bar close
        bool long_cross  = (fast_ema.val>slow_ema.val)&&(prev_fast<=prev_slow);
        bool short_cross = (fast_ema.val<slow_ema.val)&&(prev_fast>=prev_slow);

        if(!long_cross&&!short_cross)return;

        // RSI filter
        if(long_cross  && (rsi<cfg.rsi_lo||rsi>75.0))return;  // oversold or extreme OB
        if(short_cross && (rsi>cfg.rsi_hi||rsi<25.0))return;  // overbought or extreme OS

        bool isl=long_cross;
        double ep=isl?ask:bid;
        double sl_dist=atr*cfg.sl_mult;
        if(sl_dist<=0)return;
        double tp_dist=sl_dist*cfg.tp_rr;
        double cost=(ask-bid)+0.20;
        if(tp_dist<=cost)return;

        double sz=std::max(0.01,std::min(0.10,std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
        pos.active=true;pos.is_long=isl;pos.entry=ep;
        pos.sl=isl?(ep-sl_dist):(ep+sl_dist);
        pos.tp=isl?(ep+tp_dist):(ep-tp_dist);
        pos.size=sz;pos.mfe=0;pos.be_done=false;pos.atr=atr;
        pos.ets=ms;pos.slot=slot;
        last_cross_ms=ms;
    }

    void fc(double bid,double ask,int64_t ms,CB cb){
        if(!pos.active)return;_close(pos.is_long?bid:ask,"FC",ms,cb);}

    void _close(double ep,const char* r,int64_t ms,CB cb){
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        dpnl+=pnl;last_exit=ms/1000;
        if(cb){TradeResult t;t.is_long=pos.is_long;t.entry=pos.entry;
            t.exit_px=ep;t.pnl=pnl;t.mfe=pos.mfe;
            t.held_s=(int)((ms-pos.ets)/1000);t.slot=pos.slot;t.reason=r;cb(t);}
        pos=Pos{};}
};

// ─── Stats ────────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,cross_exit=0,timeout=0,fc=0;
    void record(const TradeResult& t){
        ++T;if(t.pnl>0)++W;pnl+=t.pnl;
        peak=std::max(peak,pnl);max_dd=std::max(max_dd,peak-pnl);
        if(t.reason=="TP")++tp;
        else if(t.reason=="SL")++sl;
        else if(t.reason=="CROSS_EXIT")++cross_exit;
        else if(t.reason=="TIMEOUT")++timeout;
        else ++fc;
    }
    double wr()const{return T>0?100.0*W/T:0;}
    double avg()const{return T>0?pnl/T:0;}
};

static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    EMAEngine eng(cfg);
    BarTracker bt; Stats s;
    double cur_rsi=50,cur_atr=2.0;

    auto cb=[&](const TradeResult& t){s.record(t);};
    for(auto& t:ticks){
        bool nb=bt.update(t.bid,t.ask,t.ms);
        if(nb&&(int)bt.bars.size()>=2){
            cur_rsi=bt.rsi; cur_atr=bt.atr>0?bt.atr:cur_atr;
            auto& lb=bt.bars.back();
            eng.on_bar(lb.close,cur_rsi,cur_atr);}
        eng.on_tick(t.bid,t.ask,t.ms,cur_rsi,cur_atr,t.slot,cb);
    }
    if(eng.pos.active&&!ticks.empty()){auto& lt=ticks.back();
        eng.fc(lt.bid,lt.ask,lt.ms,cb);}
    return s;
}

int main(int argc,char** argv){
    if(argc<2){fprintf(stderr,"Usage: ema_sweep file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if(ticks.empty())return 1;

    // ── Baseline: industry standard 9/21 EMA ─────────────────────────────────
    Config baseline;
    Stats base=run(ticks,baseline);
    printf("BASELINE (fast=9 slow=21 rsi_lo=45 rsi_hi=55 sl=0.7 rr=1.5 cross_exit=Y):\n");
    printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f "
           "TP=%d SL=%d CROSS=%d TMO=%d\n\n",
           base.T,base.W,base.wr(),base.pnl,base.avg(),base.max_dd,
           base.tp,base.sl,base.cross_exit,base.timeout);

    // ── Full sweep ────────────────────────────────────────────────────────────
    struct Result{Config c;Stats s;};
    std::vector<Result> results;

    for(int fe:{5,7,9,12,14})
    for(int se:{15,20,21,25,28})
    for(double rlo:{40,45,50})
    for(double rhi:{50,55,60})
    for(double sl:{0.5,0.7,1.0,1.5})
    for(double rr:{1.0,1.5,2.0,2.5})
    for(bool cx:{true,false}){
        if(fe>=se)continue;  // fast must be < slow
        if(rlo>=rhi)continue; // lo must be < hi
        Config c; c.fast_ema=fe;c.slow_ema=se;
        c.rsi_lo=rlo;c.rsi_hi=rhi;c.sl_mult=sl;c.tp_rr=rr;c.cross_exit=cx;
        Stats s=run(ticks,c);
        results.push_back({c,s});
    }

    std::sort(results.begin(),results.end(),[](auto& a,auto& b){return a.s.pnl>b.s.pnl;});

    printf("%-5s %-5s %-5s %-5s %-5s %-4s %-2s %5s %4s %8s %5s %6s "
           "%4s %4s %4s %4s\n",
           "FAST","SLOW","RLO","RHI","SL","RR","CX",
           "T","W","PNL","WR%","Avg",
           "TP","SL","CX","TMO");
    printf("%s\n",std::string(92,'-').c_str());

    int shown=0;
    for(auto& r:results){
        if(r.s.T<20)continue;
        if(shown++>=40)break;
        printf("%-5d %-5d %-5.0f %-5.0f %-5.1f %-4.1f %-2s "
               "%5d %4d %8.2f %5.1f %6.2f "
               "%4d %4d %4d %4d\n",
               r.c.fast_ema,r.c.slow_ema,r.c.rsi_lo,r.c.rsi_hi,r.c.sl_mult,r.c.tp_rr,
               r.c.cross_exit?"Y":"N",
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),
               r.s.tp,r.s.sl,r.s.cross_exit,r.s.timeout);
    }

    // ── Depth analysis on top config ──────────────────────────────────────────
    printf("\n=== TOP 3 CONFIG DETAIL ===\n");
    shown=0;
    for(auto& r:results){
        if(r.s.T<20)continue;
        if(shown++>=3)break;
        printf("\n#%d: fast=%d slow=%d rsi_lo=%.0f rsi_hi=%.0f sl=%.1f rr=%.1f cross_exit=%s\n",
               shown,r.c.fast_ema,r.c.slow_ema,r.c.rsi_lo,r.c.rsi_hi,
               r.c.sl_mult,r.c.tp_rr,r.c.cross_exit?"Y":"N");
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  TP=%d SL=%d CROSS_EXIT=%d TIMEOUT=%d\n",
               r.s.tp,r.s.sl,r.s.cross_exit,r.s.timeout);
    }

    // ── Trade frequency analysis ──────────────────────────────────────────────
    printf("\n=== TRADE FREQUENCY vs PnL ===\n");
    printf("How many trades/day does each trade count bracket produce?\n");
    printf("%-10s %5s %4s %8s %5s %6s\n","T/6days","T","W","PNL","WR%","Avg");
    // Bucket by trade count
    for(auto& r:results){
        if(r.s.T<5)continue;
        // Show configs that produce 50+ trades (8+/day) -- the high-frequency ones
        if(r.s.T<50)continue;
        if(shown++>=10)break;
        printf("%-10d %5d %4d %8.2f %5.1f %6.2f  fast=%d slow=%d\n",
               r.s.T/6,r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),
               r.c.fast_ema,r.c.slow_ema);
    }

    // ── vs CBE comparison at bottom ───────────────────────────────────────────
    printf("\n=== POSITIVE PnL >= 20 TRADES ===\n");
    printf("%-5s %-5s %-4s %-4s %-5s %-4s %-2s %5s %4s %8s %5s %6s %7s\n",
           "FAST","SLOW","RLO","RHI","SL","RR","CX","T","W","PNL","WR%","Avg","MaxDD");
    shown=0;
    for(auto& r:results){
        if(r.s.T<20||r.s.pnl<=0)continue;
        if(shown++>=30)break;
        printf("%-5d %-5d %-4.0f %-4.0f %-5.1f %-4.1f %-2s "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f\n",
               r.c.fast_ema,r.c.slow_ema,r.c.rsi_lo,r.c.rsi_hi,
               r.c.sl_mult,r.c.tp_rr,r.c.cross_exit?"Y":"N",
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }
    return 0;
}
