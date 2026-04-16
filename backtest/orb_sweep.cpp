// orb_sweep.cpp -- Opening Range Breakout sweep for XAUUSD
//
// STRATEGY (most quantitatively validated gold strategy, 74% WR in independent backtest):
//
//   Each session has two phases:
//
//   PHASE 1 - RANGE FORMATION (first N minutes after session open):
//     Mark the HIGH and LOW of every tick during this window.
//     This is the "opening range" - the price discovery zone.
//
//   PHASE 2 - BREAKOUT ENTRY (remainder of session window):
//     Wait for a M1 bar that:
//       (a) closes decisively ABOVE range_hi or BELOW range_lo
//       (b) candle body >= body_pct * total candle range (strong close, no wick reversal)
//       (c) candle body >= atr_mult * ATR14 (minimum size - filters noise breaks)
//     Enter on next tick after qualifying bar close.
//     SL: opposite end of the opening range (range_lo for longs, range_hi for shorts)
//     TP: entry + (entry - SL) * tp_rr
//     One trade per session per day maximum.
//
//   SESSIONS (UTC):
//     London: open 07:00, range 07:00-07:XX, trade 07:XX-09:00
//     NY:     open 13:00, range 13:00-13:XX, trade 13:XX-15:00
//     (NY confirmed winning from EMA diag: UTC13=$101, UTC15=$123)
//
//   SWEEP PARAMETERS:
//     range_mins:    2, 3, 5          (range formation window)
//     body_pct:      0.50, 0.60, 0.70 (min body/range ratio)
//     atr_mult:      0.3, 0.5, 0.8    (min body in ATR units)
//     tp_rr:         1.5, 2.0, 2.5, 3.0
//     london:        true/false
//     ny:            true/false
//     require_trend: true/false (drift must align with breakout direction)
//
// Build: clang++ -O3 -std=c++20 -o /tmp/orb_sweep orb_sweep.cpp
// Run:   /tmp/orb_sweep ~/Downloads/l2_ticks_2026-04-09.csv \
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
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if (!f) { fprintf(stderr,"Cannot open %s\n",path); return false; }
    std::string line, tok; std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,ci=0;
    { std::istringstream h(line); while(std::getline(h,tok,',')) {
        if(tok=="ts_ms")cm=ci; if(tok=="bid")cb=ci;
        if(tok=="ask")ca=ci; if(tok=="ewm_drift")cd=ci; ++ci; }}
    if(cb<0||ca<0) return false;
    size_t before=out.size();
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512]; if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c) if(*c==','){*c='\0';flds[nf++]=c+1;}
        int need=std::max({cm,cb,ca,cd}); if(nf<=need) continue;
        try { Tick t; t.ms=(int64_t)std::stod(flds[cm]);
              t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
              t.drift=(cd>=0&&nf>cd)?std::stod(flds[cd]):0.0;
              out.push_back(t); } catch(...) {}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

// ─── UTC helpers ─────────────────────────────────────────────────────────────
static int utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }
static int utc_min(int64_t ms)  { return (int)(((ms/1000LL)%3600LL)/60LL); }
static int utc_mins(int64_t ms) { return utc_hour(ms)*60 + utc_min(ms); }
static int64_t day_key(int64_t ms){ return (ms/1000LL)/86400LL; }

// ─── M1 bar tracker with ATR ─────────────────────────────────────────────────
struct M1Bar { double open,high,low,close; int64_t bms; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=2.0;
    M1Bar cur{}; int64_t cbms=0;

    bool update(double bid, double ask, int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid,bms};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur);
            if((int)bars.size()>50) bars.pop_front();
            if((int)bars.size()>=2){
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,
                                   std::fabs(b.high-pb.close),
                                   std::fabs(b.low-pb.close)});}
                atr=sum/n;
            }
            cur={mid,mid,mid,mid,bms}; cbms=bms; nb=true;
        } else {
            if(mid>cur.high)cur.high=mid;
            if(mid<cur.low)cur.low=mid;
            cur.close=mid;
        }
        return nb;
    }
};

// ─── Config ──────────────────────────────────────────────────────────────────
struct Config {
    int    range_mins    = 3;     // minutes to build opening range
    double body_pct      = 0.60;  // breakout bar: body/range >= this
    double atr_mult      = 0.5;   // breakout bar: body >= ATR * this
    double tp_rr         = 2.0;   // TP = (entry-SL) * rr
    bool   london        = true;  // trade London open (07:00 UTC)
    bool   ny            = true;  // trade NY open (13:00 UTC)
    bool   require_trend = false; // ewm_drift must align with direction
    int    trade_timeout_mins = 120; // close trade after N mins
};

// ─── Session state ────────────────────────────────────────────────────────────
struct Session {
    bool  active       = false;  // currently in range-formation or trade window
    bool  range_done   = false;  // range formation complete
    bool  traded       = false;  // already took a trade this session
    double range_hi    = 0.0;
    double range_lo    = 1e9;
    int64_t open_ms    = 0;      // session open timestamp
    int64_t day        = 0;
};

// ─── Trade record ─────────────────────────────────────────────────────────────
struct Trade {
    bool is_long; double entry,sl,tp,size,pnl,mfe; int held_mins; std::string reason;
};

// ─── Stats ───────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp_count=0,sl_count=0,timeout_count=0;
    void record(const Trade& t){
        ++T; if(t.pnl>0)++W; pnl+=t.pnl;
        peak=std::max(peak,pnl); max_dd=std::max(max_dd,peak-pnl);
        if(t.reason=="TP")++tp_count;
        else if(t.reason=="SL")++sl_count;
        else ++timeout_count;
    }
    double wr()  const{return T>0?100.0*W/T:0;}
    double avg() const{return T>0?pnl/T:0;}
};

// ─── Engine ──────────────────────────────────────────────────────────────────
static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    BarTracker bt;
    Stats s;

    Session london_sess, ny_sess;
    // Active position
    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0;
        int64_t ets=0;
    } pos;

    double daily_pnl=0; int64_t daily_day=0;

    auto close_pos=[&](double ep, const char* reason, int64_t ms){
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        daily_pnl+=pnl;
        Trade t; t.is_long=pos.is_long; t.entry=pos.entry; t.sl=pos.sl;
        t.tp=pos.tp; t.size=pos.size; t.pnl=pnl; t.mfe=pos.mfe;
        t.held_mins=(int)((ms-pos.ets)/60000); t.reason=reason;
        s.record(t);
        pos=Pos{};
    };

    auto try_enter=[&](bool is_long, double bid, double ask, double sl_px,
                        double tp_px, int64_t ms){
        if(pos.active) return;
        if(daily_pnl<=-200.0) return;
        double ep=is_long?ask:bid;
        double sl_dist=std::fabs(ep-sl_px);
        if(sl_dist<0.10) return;  // range too tight
        double sz=std::max(0.01,std::min(0.10,
            std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
        pos.active=true; pos.is_long=is_long; pos.entry=ep;
        pos.sl=sl_px; pos.tp=tp_px; pos.size=sz;
        pos.mfe=0; pos.be_done=false; pos.ets=ms;
    };

    for(auto& t:ticks){
        bool nb=bt.update(t.bid,t.ask,t.ms);
        double mid=(t.bid+t.ask)*0.5;
        int64_t d=day_key(t.ms);

        // Daily reset
        if(d!=daily_day){ daily_pnl=0; daily_day=d; }

        // Session open detection
        int um=utc_mins(t.ms);

        // London: open at 07:00 UTC = 420 mins
        if(cfg.london){
            if(um==420 && london_sess.day!=d){
                london_sess={true,false,false,0,1e9,t.ms,d};
            }
            // Range formation
            if(london_sess.active && !london_sess.range_done && london_sess.day==d){
                int elapsed_mins=(int)((t.ms-london_sess.open_ms)/60000);
                if(elapsed_mins<cfg.range_mins){
                    if(mid>london_sess.range_hi)london_sess.range_hi=mid;
                    if(mid<london_sess.range_lo)london_sess.range_lo=mid;
                } else {
                    london_sess.range_done=true;
                }
            }
        }

        // NY: open at 13:00 UTC = 780 mins
        if(cfg.ny){
            if(um==780 && ny_sess.day!=d){
                ny_sess={true,false,false,0,1e9,t.ms,d};
            }
            if(ny_sess.active && !ny_sess.range_done && ny_sess.day==d){
                int elapsed_mins=(int)((t.ms-ny_sess.open_ms)/60000);
                if(elapsed_mins<cfg.range_mins){
                    if(mid>ny_sess.range_hi)ny_sess.range_hi=mid;
                    if(mid<ny_sess.range_lo)ny_sess.range_lo=mid;
                } else {
                    ny_sess.range_done=true;
                }
            }
        }

        // Position management
        if(pos.active){
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            // BE at 50% of TP
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.50){
                pos.sl=pos.entry; pos.be_done=true;}
            double ep=pos.is_long?t.bid:t.ask;
            if(pos.is_long?(t.bid>=pos.tp):(t.ask<=pos.tp)){close_pos(ep,"TP",t.ms);continue;}
            if(pos.is_long?(t.bid<=pos.sl):(t.ask>=pos.sl)){
                close_pos(ep,pos.be_done?"BE":"SL",t.ms);continue;}
            // Timeout
            if((t.ms-pos.ets)/60000>=cfg.trade_timeout_mins){
                close_pos(ep,"TIMEOUT",t.ms);continue;}
            // Session end close (don't hold overnight)
            // London session ends 09:00 UTC = 540 mins
            // NY session ends 15:00 UTC = 900 mins
            if(um==540||um==900){close_pos(ep,"SESSION_END",t.ms);continue;}
            continue;
        }

        // Breakout detection -- check on each new M1 bar close
        if(!nb || bt.bars.empty()) continue;
        auto& lb=bt.bars.back();
        double body=std::fabs(lb.close-lb.open);
        double range=lb.high-lb.low;
        if(range<=0) continue;

        // Body quality filters
        bool body_ok=(body/range>=cfg.body_pct) && (body>=bt.atr*cfg.atr_mult);
        if(!body_ok) continue;

        // Spread gate
        double spread=t.ask-t.bid;
        if(spread>bt.atr*0.25) continue;

        auto check_breakout=[&](Session& sess, int trade_end_mins){
            if(!sess.active||!sess.range_done||sess.traded||sess.day!=d) return;
            if(sess.range_hi<=sess.range_lo) return;
            double range_size=sess.range_hi-sess.range_lo;
            if(range_size<0.10||range_size>bt.atr*3.0) return; // sanity

            // Bullish breakout: bar closed above range hi
            if(lb.close>sess.range_hi && lb.close>lb.open){
                if(cfg.require_trend && t.drift<0.0) return;
                double sl_px=sess.range_lo;
                double sl_dist=t.ask-sl_px;
                if(sl_dist<=0) return;
                double tp_px=t.ask+sl_dist*cfg.tp_rr;
                try_enter(true,t.bid,t.ask,sl_px,tp_px,t.ms);
                if(pos.active) sess.traded=true;
            }
            // Bearish breakout: bar closed below range lo
            else if(lb.close<sess.range_lo && lb.close<lb.open){
                if(cfg.require_trend && t.drift>0.0) return;
                double sl_px=sess.range_hi;
                double sl_dist=sl_px-t.bid;
                if(sl_dist<=0) return;
                double tp_px=t.bid-sl_dist*cfg.tp_rr;
                try_enter(false,t.bid,t.ask,sl_px,tp_px,t.ms);
                if(pos.active) sess.traded=true;
            }
        };

        check_breakout(london_sess, 540);
        check_breakout(ny_sess, 900);
    }

    // Force close any open position at end
    if(pos.active&&!ticks.empty()){
        auto& lt=ticks.back();
        close_pos(pos.is_long?lt.bid:lt.ask,"FORCE_CLOSE",lt.ms);
    }
    return s;
}

int main(int argc, char** argv){
    if(argc<2){fprintf(stderr,"Usage: orb_sweep file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i) load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if(ticks.empty()) return 1;

    // Baseline
    Config base;
    Stats bs=run(ticks,base);
    printf("BASELINE (range=3min body=0.60 atr=0.5 rr=2.0 London+NY):\n");
    printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f "
           "TP=%d SL=%d TMO=%d\n\n",
           bs.T,bs.W,bs.wr(),bs.pnl,bs.avg(),bs.max_dd,
           bs.tp_count,bs.sl_count,bs.timeout_count);

    // Full sweep
    struct Result{Config c;Stats s;};
    std::vector<Result> results;

    for(int rm  :{2,3,5})
    for(double bp:{0.50,0.60,0.70})
    for(double am:{0.3,0.5,0.8})
    for(double rr:{1.5,2.0,2.5,3.0})
    for(bool lon:{true,false})
    for(bool ny :{true,false})
    for(bool rt :{false,true})
    for(int tmo :{60,120,180})
    {
        if(!lon&&!ny) continue;
        Config c; c.range_mins=rm; c.body_pct=bp; c.atr_mult=am;
        c.tp_rr=rr; c.london=lon; c.ny=ny;
        c.require_trend=rt; c.trade_timeout_mins=tmo;
        results.push_back({c,run(ticks,c)});
    }

    std::sort(results.begin(),results.end(),
        [](auto& a,auto& b){return a.s.pnl>b.s.pnl;});

    printf("%-4s %-4s %-4s %-4s %-3s %-3s %-3s %-4s %5s %4s %8s %5s %6s %7s %4s %4s %4s\n",
           "RNG","BDY","ATR","RR","LON","NY","TRD","TMO",
           "T","W","PNL","WR%","Avg","MaxDD","TP","SL","TMO");
    printf("%s\n",std::string(98,'-').c_str());

    int shown=0;
    for(auto& r:results){
        if(r.s.T<5) continue;
        if(shown++>=40) break;
        printf("%-4d %-4.2f %-4.1f %-4.1f %-3s %-3s %-3s %-4d "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f %4d %4d %4d\n",
               r.c.range_mins,r.c.body_pct,r.c.atr_mult,r.c.tp_rr,
               r.c.london?"Y":"N",r.c.ny?"Y":"N",r.c.require_trend?"Y":"N",
               r.c.trade_timeout_mins,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd,
               r.s.tp_count,r.s.sl_count,r.s.timeout_count);
    }

    // Top 3 detail
    printf("\n=== TOP 3 DETAIL ===\n");
    shown=0;
    for(auto& r:results){
        if(r.s.T<5) continue;
        if(shown++>=3) break;
        printf("\n#%d: range=%dmin body=%.2f atr=%.1f rr=%.1f lon=%s ny=%s trend=%s tmo=%d\n",
               shown,r.c.range_mins,r.c.body_pct,r.c.atr_mult,r.c.tp_rr,
               r.c.london?"Y":"N",r.c.ny?"Y":"N",r.c.require_trend?"Y":"N",
               r.c.trade_timeout_mins);
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  TP=%d SL=%d TIMEOUT=%d\n",
               r.s.tp_count,r.s.sl_count,r.s.timeout_count);
    }

    // Session breakdown for top config
    printf("\n=== POSITIVE PnL >= 5 TRADES ===\n");
    printf("%-4s %-4s %-4s %-4s %-3s %-3s %-3s %-4s %5s %4s %8s %5s %6s %7s\n",
           "RNG","BDY","ATR","RR","LON","NY","TRD","TMO","T","W","PNL","WR%","Avg","MaxDD");
    shown=0;
    for(auto& r:results){
        if(r.s.T<5||r.s.pnl<=0) continue;
        if(shown++>=25) break;
        printf("%-4d %-4.2f %-4.1f %-4.1f %-3s %-3s %-3s %-4d "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f\n",
               r.c.range_mins,r.c.body_pct,r.c.atr_mult,r.c.tp_rr,
               r.c.london?"Y":"N",r.c.ny?"Y":"N",r.c.require_trend?"Y":"N",
               r.c.trade_timeout_mins,
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }

    // London vs NY breakdown for baseline
    printf("\n=== LONDON vs NY SESSIONS (baseline params) ===\n");
    Config lon_only=base; lon_only.ny=false;
    Config ny_only=base;  ny_only.london=false;
    Stats slon=run(ticks,lon_only);
    Stats sny=run(ticks,ny_only);
    printf("  London only: T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
           slon.T,slon.W,slon.wr(),slon.pnl,slon.avg(),slon.max_dd);
    printf("  NY only:     T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
           sny.T,sny.W,sny.wr(),sny.pnl,sny.avg(),sny.max_dd);

    return 0;
}
