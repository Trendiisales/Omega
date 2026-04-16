// silver_bullet_sweep.cpp -- ICT Silver Bullet / Liquidity Sweep + FVG engine
//
// STRATEGY (most backtested profitable XAUUSD strategy 2021-2025):
//   1. Mark session range (Asian hi/lo: 00:00-07:00 UTC by default)
//   2. During killzone window, detect liquidity sweep of that range
//   3. Detect Market Structure Shift (MSS) -- reversal after sweep
//   4. Detect Fair Value Gap (FVG) -- 3-bar imbalance in MSS direction
//   5. Enter on FVG retest (price pulls back into gap)
//   6. SL: beyond sweep wick. TP: 2R (or next liquidity)
//
// KILLZONES (UTC):
//   London:  07:00-09:00
//   NY AM:   14:00-16:00  (10-11 AM EST = 15-16 UTC)
//   NY PM:   18:00-20:00  (2-3 PM EST = 19-20 UTC)
//
// Sweep grid:
//   sweep_pts: 0.10, 0.20, 0.30, 0.50  (pts beyond range to confirm sweep)
//   mss_pts:   0.20, 0.30, 0.50, 0.70  (reversal needed to confirm MSS)
//   fvg_min:   0.10, 0.20, 0.30        (min FVG size to be tradeable)
//   tp_rr:     1.5, 2.0, 2.5
//   session:   asian range / prev_bar range
//
// Build: clang++ -O3 -std=c++20 -o /tmp/sb_sweep silver_bullet_sweep.cpp
// Run:   /tmp/sb_sweep ~/Downloads/l2_ticks_2026-04-09.csv ... (6 files)

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

// ─── Data ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if (!f) { fprintf(stderr,"Cannot open %s\n",path); return false; }
    std::string line, tok; std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,ci=0;
    { std::istringstream h(line); while(std::getline(h,tok,',')) {
        if(tok=="ts_ms")cm=ci; if(tok=="bid")cb=ci; if(tok=="ask")ca=ci; ++ci; }}
    if(cb<0||ca<0) return false;
    size_t before=out.size();
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512]; if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c) if(*c==','){*c='\0';flds[nf++]=c+1;}
        int need=std::max(cm,std::max(cb,ca)); if(nf<=need) continue;
        try { Tick t; t.ms=(int64_t)std::stod(flds[cm]);
              t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
              out.push_back(t); } catch(...) {}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

// ─── UTC helpers ─────────────────────────────────────────────────────────────
static int utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }
static int utc_min(int64_t ms)  { return (int)(((ms/1000LL)%3600LL)/60LL); }
static int utc_mins(int64_t ms) { return utc_hour(ms)*60 + utc_min(ms); }
static int64_t day_start_ms(int64_t ms) { return (ms/1000LL/86400LL)*86400LL*1000LL; }

// ─── M1 bar tracker ──────────────────────────────────────────────────────────
struct M1Bar { double open,high,low,close; int64_t bms; };
struct BarTracker {
    std::deque<M1Bar> bars;
    M1Bar cur{}; int64_t cbms=0;
    bool update(double bid,double ask,int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid,bms};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur); if((int)bars.size()>50) bars.pop_front();
            cur={mid,mid,mid,mid,bms}; cbms=bms; nb=true;
        } else { if(mid>cur.high)cur.high=mid; if(mid<cur.low)cur.low=mid; cur.close=mid; }
        return nb;
    }
    // Returns FVG if last 3 bars form one, direction: +1=bullish, -1=bearish, 0=none
    // Bullish FVG: bar[i-2].low > bar[i].high  (gap between bar 1 low and bar 3 high)
    // Bearish FVG: bar[i-2].high < bar[i].low
    int fvg_direction(double* fvg_lo, double* fvg_hi) const {
        if ((int)bars.size() < 3) return 0;
        auto& b1 = bars[bars.size()-3];
        auto& b3 = bars[bars.size()-1];
        // Bullish FVG: gap above b3, below b1 -- price imbalance going up
        if (b1.low > b3.high) { *fvg_lo=b3.high; *fvg_hi=b1.low; return 1; }
        // Bearish FVG: gap below b3, above b1 -- price imbalance going down
        if (b1.high < b3.low) { *fvg_lo=b1.high; *fvg_hi=b3.low; return -1; }
        return 0;
    }
};

// ─── Config ──────────────────────────────────────────────────────────────────
struct Config {
    double sweep_pts = 0.20;  // pts beyond range to confirm sweep
    double mss_pts   = 0.30;  // reversal pts to confirm MSS
    double fvg_min   = 0.15;  // min FVG size
    double tp_rr     = 2.0;   // TP = SL * RR
    int    timeout_s = 180;   // max hold
    // Killzone windows (UTC minutes)
    bool   use_london = true;  // 07:00-09:00 UTC = 420-540
    bool   use_ny_am  = true;  // 14:00-16:00 UTC = 840-960  (10-11 EST + buffer)
    bool   use_ny_pm  = false; // 18:00-20:00 UTC = 1080-1200
};

static bool in_killzone(int64_t ms, const Config& cfg) {
    int m = utc_mins(ms);
    if (cfg.use_london && m >= 420 && m < 540) return true;  // 07:00-09:00
    if (cfg.use_ny_am  && m >= 840 && m < 960) return true;  // 14:00-16:00
    if (cfg.use_ny_pm  && m >= 1080 && m < 1200) return true; // 18:00-20:00
    return false;
}

// ─── Engine ──────────────────────────────────────────────────────────────────
struct TradeResult {
    bool is_long; double entry,exit_px,pnl,mfe; int held_s; std::string reason;
};

struct SBEngine {
    Config cfg;

    // Session range (Asian: 00:00-07:00 UTC)
    double range_hi=0, range_lo=1e9;
    int64_t range_day=-1;
    bool range_valid=false;

    // Sweep state
    bool swept_hi=false, swept_lo=false;
    double sweep_extreme=0;  // the extreme tick of the sweep
    int64_t sweep_ms=0;

    // MSS state
    bool mss_confirmed=false;
    int  mss_dir=0;  // +1=bullish (swept lo, reversed up), -1=bearish
    int64_t mss_ms=0;
    double mss_ref=0;  // price at MSS confirmation

    // FVG state
    bool   fvg_armed=false;
    int    fvg_dir=0;
    double fvg_lo=0, fvg_hi=0;
    int64_t fvg_ms=0;

    // Position
    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0;
        int64_t ets=0;
    } pos;

    int64_t last_exit=0;
    double dpnl=0; int64_t dday=0;
    int64_t startup=0;

    using CB = std::function<void(const TradeResult&)>;

    void on_bar(const M1Bar& lb, const M1Bar* prev2, int64_t ms) {
        // Update Asian range (00:00-07:00 UTC = mins 0-419)
        int64_t today = day_start_ms(ms);
        if (today != range_day) {
            range_hi = 0; range_lo = 1e9;
            range_day = today; range_valid = false;
            swept_hi=swept_lo=mss_confirmed=fvg_armed=false;
        }
        int m = utc_mins(ms);
        if (m >= 0 && m < 420) {  // Asian session building range
            if (lb.high > range_hi) range_hi = lb.high;
            if (lb.low  < range_lo) range_lo = lb.low;
            if (range_hi > 0 && range_lo < 1e8) range_valid = true;
        }

        // FVG detection after MSS (needs 3 bars)
        if (mss_confirmed && !fvg_armed && prev2) {
            // Check last 3 bars for FVG in MSS direction
            double flo=0, fhi=0;
            // Bullish FVG (mss_dir=+1): prev2.low > lb.high
            if (mss_dir == 1 && prev2->low > lb.high && (prev2->low - lb.high) >= cfg.fvg_min) {
                fvg_dir=1; fvg_lo=lb.high; fvg_hi=prev2->low;
                fvg_armed=true; fvg_ms=ms;
            }
            // Bearish FVG (mss_dir=-1): prev2.high < lb.low
            else if (mss_dir == -1 && prev2->high < lb.low && (lb.low - prev2->high) >= cfg.fvg_min) {
                fvg_dir=-1; fvg_lo=prev2->high; fvg_hi=lb.low;
                fvg_armed=true; fvg_ms=ms;
            }
            (void)flo; (void)fhi;
        }

        // Expire FVG after 10 bars (not retested = no longer valid)
        if (fvg_armed && (ms - fvg_ms) > 600000LL) { // 10 min
            fvg_armed=false;
        }
    }

    void on_tick(double bid, double ask, int64_t ms, CB on_close) {
        if (startup==0) startup=ms;
        if (ms-startup<120000LL) return;
        int64_t day=(ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-200.0)return;

        double mid=(bid+ask)*0.5;
        double spread=ask-bid;

        // Position management
        if (pos.active) {
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe) pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.50){pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?bid:ask;
            if(pos.is_long?(bid>=pos.tp):(ask<=pos.tp)){_close(ep,"TP",ms,on_close);return;}
            if(pos.is_long?(bid<=pos.sl):(ask>=pos.sl)){
                const char* r=pos.be_done?"BE":"SL";_close(ep,r,ms,on_close);return;}
            if(ms-pos.ets>(int64_t)cfg.timeout_s*1000){_close(ep,"TIMEOUT",ms,on_close);return;}
            return;
        }

        if (!range_valid) return;
        if (spread > 0.60) return;  // wide spread gate
        if (ms/1000 - last_exit < 60) return;  // 60s cooldown

        // ── Sweep detection ─────────────────────────────────────────────────
        if (!swept_hi && !swept_lo) {
            if (!in_killzone(ms, cfg)) return;
            if (ask >= range_hi + cfg.sweep_pts) {
                swept_hi=true; sweep_extreme=ask; sweep_ms=ms;
            } else if (bid <= range_lo - cfg.sweep_pts) {
                swept_lo=true; sweep_extreme=bid; sweep_ms=ms;
            }
            return;
        }

        // Update sweep extreme
        if (swept_hi && ask > sweep_extreme) sweep_extreme=ask;
        if (swept_lo && bid < sweep_extreme) sweep_extreme=bid;

        // Expire sweep after 5 min
        if (ms - sweep_ms > 300000LL) {
            swept_hi=swept_lo=mss_confirmed=fvg_armed=false; return;
        }

        // ── MSS detection ───────────────────────────────────────────────────
        if (!mss_confirmed) {
            if (swept_hi) {
                // Swept high → look for bearish reversal (MSS bearish)
                double reversal = sweep_extreme - mid;
                if (reversal >= cfg.mss_pts) {
                    mss_confirmed=true; mss_dir=-1; mss_ms=ms; mss_ref=mid;
                }
            } else if (swept_lo) {
                // Swept low → look for bullish reversal (MSS bullish)
                double reversal = mid - sweep_extreme;
                if (reversal >= cfg.mss_pts) {
                    mss_confirmed=true; mss_dir=+1; mss_ms=ms; mss_ref=mid;
                }
            }
            return;
        }

        // ── FVG retest entry ────────────────────────────────────────────────
        if (!fvg_armed) return;

        // Check if price is retesting the FVG
        bool retest_long  = (fvg_dir==1)  && (bid <= fvg_hi) && (ask >= fvg_lo);
        bool retest_short = (fvg_dir==-1) && (ask >= fvg_lo) && (bid <= fvg_hi);

        if (!retest_long && !retest_short) return;

        bool isl = (fvg_dir == 1);  // bullish FVG = long entry
        double ep = isl ? ask : bid;

        // SL: beyond the sweep extreme + buffer
        double sl_dist;
        if (isl)  sl_dist = ep - (sweep_extreme - 0.30);  // SL below sweep low
        else      sl_dist = (sweep_extreme + 0.30) - ep;  // SL above sweep high
        if (sl_dist <= 0.10) sl_dist = 0.50;  // minimum SL

        double tp_dist = sl_dist * cfg.tp_rr;
        double cost = spread + 0.20;
        if (tp_dist <= cost) return;

        double sz = std::max(0.01, std::min(0.10,
            std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));

        pos.active=true; pos.is_long=isl; pos.entry=ep;
        pos.sl=isl?(ep-sl_dist):(ep+sl_dist);
        pos.tp=isl?(ep+tp_dist):(ep-tp_dist);
        pos.size=sz; pos.mfe=0; pos.be_done=false; pos.ets=ms;

        // Reset state after entry
        swept_hi=swept_lo=mss_confirmed=fvg_armed=false;
    }

    void fc(double bid,double ask,int64_t ms,CB cb){
        if(!pos.active)return;_close(pos.is_long?bid:ask,"FC",ms,cb);}

    void _close(double ep,const char* r,int64_t ms,CB cb){
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        dpnl+=pnl; last_exit=ms/1000;
        if(cb){TradeResult t;t.is_long=pos.is_long;t.entry=pos.entry;
            t.exit_px=ep;t.pnl=pnl;t.mfe=pos.mfe;
            t.held_s=(int)((ms-pos.ets)/1000);t.reason=r;cb(t);}
        pos=Pos{};}
};

// ─── Stats ────────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,be=0,timeout=0,fc=0;
    void record(const TradeResult& t){
        ++T;if(t.pnl>0)++W;pnl+=t.pnl;
        peak=std::max(peak,pnl);max_dd=std::max(max_dd,peak-pnl);
        if(t.reason=="TP")++tp;
        else if(t.reason=="SL")++sl;
        else if(t.reason=="BE")++be;
        else if(t.reason=="TIMEOUT")++timeout;
        else ++fc;
    }
    double wr()const{return T>0?100.0*W/T:0;}
    double avg()const{return T>0?pnl/T:0;}
};

static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    SBEngine eng; eng.cfg=cfg;
    BarTracker bt; Stats s;
    auto cb=[&](const TradeResult& t){s.record(t);};
    for (auto& t : ticks) {
        bool nb = bt.update(t.bid, t.ask, t.ms);
        if (nb && (int)bt.bars.size() >= 3) {
            const M1Bar* prev2 = &bt.bars[bt.bars.size()-3];
            eng.on_bar(bt.bars.back(), prev2, t.ms);
        } else if (nb) {
            eng.on_bar(bt.bars.back(), nullptr, t.ms);
        }
        eng.on_tick(t.bid, t.ask, t.ms, cb);
    }
    if (eng.pos.active && !ticks.empty())
        eng.fc(ticks.back().bid, ticks.back().ask, ticks.back().ms, cb);
    return s;
}

int main(int argc, char** argv) {
    if (argc<2){fprintf(stderr,"Usage: sb_sweep file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if(ticks.empty())return 1;

    // Baseline
    Config base;
    Stats bs=run(ticks,base);
    printf("BASELINE (sweep=0.20 mss=0.30 fvg=0.15 rr=2.0 London+NYAM):\n");
    printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f "
           "TP=%d SL=%d BE=%d TMO=%d\n\n",
           bs.T,bs.W,bs.wr(),bs.pnl,bs.avg(),bs.max_dd,
           bs.tp,bs.sl,bs.be,bs.timeout);

    struct Result{Config c;Stats s;};
    std::vector<Result> results;

    for (double sw : {0.10,0.20,0.30,0.50})
    for (double ms : {0.20,0.30,0.50,0.70})
    for (double fg : {0.10,0.15,0.20,0.30})
    for (double rr : {1.5,2.0,2.5,3.0})
    for (bool lon : {true,false})
    for (bool nya : {true,false})
    for (bool nyp : {false,true}) {
        if (!lon && !nya && !nyp) continue;
        Config c; c.sweep_pts=sw;c.mss_pts=ms;c.fvg_min=fg;
        c.tp_rr=rr;c.use_london=lon;c.use_ny_am=nya;c.use_ny_pm=nyp;
        Stats s=run(ticks,c);
        results.push_back({c,s});
    }

    std::sort(results.begin(),results.end(),
        [](auto& a,auto& b){return a.s.pnl>b.s.pnl;});

    printf("%-5s %-5s %-5s %-4s %-3s %-3s %-3s %5s %4s %8s %5s %6s "
           "%4s %4s %4s %4s\n",
           "SW","MSS","FVG","RR","LON","NYA","NYP",
           "T","W","PNL","WR%","Avg",
           "TP","SL","BE","TMO");
    printf("%s\n",std::string(90,'-').c_str());

    int shown=0;
    for(auto& r:results){
        if(r.s.T<10)continue;
        if(shown++>=40)break;
        printf("%-5.2f %-5.2f %-5.2f %-4.1f %-3s %-3s %-3s "
               "%5d %4d %8.2f %5.1f %6.2f "
               "%4d %4d %4d %4d\n",
               r.c.sweep_pts,r.c.mss_pts,r.c.fvg_min,r.c.tp_rr,
               r.c.use_london?"Y":"N",r.c.use_ny_am?"Y":"N",r.c.use_ny_pm?"Y":"N",
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),
               r.s.tp,r.s.sl,r.s.be,r.s.timeout);
    }

    printf("\n=== TOP 3 DETAIL ===\n");
    shown=0;
    for(auto& r:results){
        if(r.s.T<10)continue;
        if(shown++>=3)break;
        printf("\n#%d: sweep=%.2f mss=%.2f fvg=%.2f rr=%.1f lon=%s nya=%s nyp=%s\n",
               shown,r.c.sweep_pts,r.c.mss_pts,r.c.fvg_min,r.c.tp_rr,
               r.c.use_london?"Y":"N",r.c.use_ny_am?"Y":"N",r.c.use_ny_pm?"Y":"N");
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  TP=%d SL=%d BE=%d TIMEOUT=%d\n",
               r.s.tp,r.s.sl,r.s.be,r.s.timeout);
    }

    printf("\n=== POSITIVE PnL >= 10 TRADES ===\n");
    printf("%-5s %-5s %-5s %-4s %-3s %-3s %-3s %5s %4s %8s %5s %6s %7s\n",
           "SW","MSS","FVG","RR","LON","NYA","NYP","T","W","PNL","WR%","Avg","MaxDD");
    shown=0;
    for(auto& r:results){
        if(r.s.T<10||r.s.pnl<=0)continue;
        if(shown++>=25)break;
        printf("%-5.2f %-5.2f %-5.2f %-4.1f %-3s %-3s %-3s "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f\n",
               r.c.sweep_pts,r.c.mss_pts,r.c.fvg_min,r.c.tp_rr,
               r.c.use_london?"Y":"N",r.c.use_ny_am?"Y":"N",r.c.use_ny_pm?"Y":"N",
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }
    return 0;
}
