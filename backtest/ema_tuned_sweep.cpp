// ema_tuned_sweep.cpp -- EMA 9/15 tuned sweep
// Apply filters found in ema_diag.cpp diagnostic run.
//
// KEY FINDINGS from 277-trade diagnostic:
//   1. SHORT bleeds: $-126 vs LONG $+207 -- test LONG-only
//   2. cross_exit is CATASTROPHIC: $-256 on 16 trades = $-16/trade
//   3. lag>30s loses $398: only enter within 30s of cross (153 trades, +$479)
//   4. ATR 2-3 bleeds $-247: ATR sweet spots are 1-2 ($+118) and 3-5 ($+196)
//   5. RSI windows:
//      GOOD:  40-45 (+$202), 55-60 (+$146), 70-75 (+$143)
//      BAD:   35-40 (-$128), 50-55 (-$113), 65-70 (-$111), 30-35 (-$56)
//   6. EMA gap 0.30-0.50 = -$260 (late entries). Block it. gap>0.50 = +$106 (breakouts)
//   7. drift_against = -$91 on only 10 trades -- strong signal, block it
//   8. Hours: 05,12,14,16,20 UTC all bleed. 09,10,13,15 UTC win strongly.
//   9. TIMEOUT MFE p50=1.58: half of timeouts have touched 1.58pts -- tight TP helps
//
// Sweep axes:
//   long_only:      true/false
//   max_lag_s:      30, 60, 180 (cross-to-entry max)
//   atr_block_mid:  true/false (block ATR 2-3)
//   rsi_windows:    tight vs loose (see below)
//   gap_block:      0.30, 0.50, off (block stale entries by EMA spread)
//   drift_gate:     true/false (block drift_against)
//   hour_kill:      true/false (block hours 05,12,14,16,20)
//   cross_exit:     false (confirmed bad -- always off)
//   tp_rr:          0.8, 1.0, 1.2 (try tighter TP given MFE p50=1.58)
//
// Build: clang++ -O3 -std=c++20 -o /tmp/ema_tuned ema_tuned_sweep.cpp
// Run:   /tmp/ema_tuned ~/Downloads/l2_ticks_2026-04-09.csv ... (6 files)

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

struct Tick { int64_t ms; double bid, ask, drift; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if (!f) return false;
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

static int utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }

struct M1Bar { double open,high,low,close; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=0, rsi=50;
    std::deque<double> rg, rl; double rp=0;
    M1Bar cur{}; int64_t cbms=0;
    bool update(double bid, double ask, int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        if(rp>0){ double chg=mid-rp;
            rg.push_back(chg>0?chg:0); rl.push_back(chg<0?-chg:0);
            if((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if((int)rg.size()==14){ double ag=0,al=0;
                for(auto x:rg)ag+=x; for(auto x:rl)al+=x; ag/=14;al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al); }}
        rp=mid;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur); if((int)bars.size()>50) bars.pop_front();
            if((int)bars.size()>=2){
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,std::fabs(b.high-pb.close),std::fabs(b.low-pb.close)});}
                atr=sum/n;}
            cur={mid,mid,mid,mid}; cbms=bms; nb=true;
        } else { if(mid>cur.high)cur.high=mid; if(mid<cur.low)cur.low=mid; cur.close=mid; }
        return nb;
    }
};

struct EMA {
    int period; double val=0,alpha; bool warmed=false; int count=0;
    EMA(int p): period(p), alpha(2.0/(p+1.0)) {}
    void update(double p) {
        if(!warmed){val=(val*count+p)/(count+1);++count;if(count>=period)warmed=true;}
        else val=p*alpha+val*(1.0-alpha);
    }
};

struct Config {
    bool   long_only      = false;
    int    max_lag_s      = 180;   // max seconds from cross to entry
    bool   atr_block_mid  = false; // block ATR 2.0-3.0 zone
    double rsi_lo         = 40.0;  // LONG: RSI > this
    double rsi_hi         = 50.0;  // SHORT: RSI < this
    bool   rsi_exclude_bad= false; // exclude RSI 30-35, 50-55, 65-70 buckets
    double gap_block      = 999.0; // block EMA gap > this (0.30 or 0.50)
    bool   drift_gate     = false; // block drift_against
    bool   hour_kill      = false; // block hours 05,12,14,16,20
    double tp_rr          = 1.0;   // TP multiplier
};

// RSI is "good" according to diagnostic
static bool rsi_good(double rsi, bool is_long, const Config& cfg) {
    if (!cfg.rsi_exclude_bad) {
        // Original filter: LONG RSI 40-75, SHORT RSI 25-50
        if (is_long  && (rsi < cfg.rsi_lo || rsi > 75.0)) return false;
        if (!is_long && (rsi > cfg.rsi_hi || rsi < 25.0)) return false;
        return true;
    }
    // Exclude bad RSI buckets confirmed by diagnostic:
    // 30-35 (-$56), 35-40 (-$128), 50-55 (-$113), 65-70 (-$111)
    if (rsi >= 30.0 && rsi < 35.0) return false;
    if (rsi >= 35.0 && rsi < 40.0) return false;
    if (rsi >= 50.0 && rsi < 55.0) return false;
    if (rsi >= 65.0 && rsi < 70.0) return false;
    // Also keep original direction gates
    if (is_long  && (rsi < cfg.rsi_lo || rsi > 75.0)) return false;
    if (!is_long && (rsi > cfg.rsi_hi || rsi < 25.0)) return false;
    return true;
}

struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,timeout=0;
    void record(double p, const char* r) {
        ++T; if(p>0)++W; pnl+=p;
        peak=std::max(peak,pnl); max_dd=std::max(max_dd,peak-pnl);
        if(strcmp(r,"TP")==0)++tp;
        else if(strcmp(r,"SL")==0||strcmp(r,"BE")==0)++sl;
        else ++timeout;
    }
    double wr()  const{return T>0?100.0*W/T:0;}
    double avg() const{return T>0?pnl/T:0;}
};

static Stats run(const std::vector<Tick>& ticks, Config cfg) {
    EMA fast{9}, slow{15};
    double prev_fast=0, prev_slow=0;
    double atr=0, rsi=50;
    int cross_dir=0; int64_t cross_ms=0;

    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0;
        int64_t ets=0;
    } pos;

    int64_t last_exit=0, startup=0;
    double dpnl=0; int64_t dday=0;
    BarTracker bt; Stats s;

    for (auto& t : ticks) {
        bool nb = bt.update(t.bid, t.ask, t.ms);
        if (nb && !bt.bars.empty()) {
            if (bt.atr > 0.5 && bt.atr < 50.0) atr = bt.atr;
            rsi = bt.rsi;
            double pf=fast.val, ps=slow.val;
            fast.update(bt.bars.back().close);
            slow.update(bt.bars.back().close);
            if (fast.warmed && slow.warmed) {
                bool lx=(fast.val>slow.val)&&(pf<=ps);
                bool sx=(fast.val<slow.val)&&(pf>=ps);
                if(lx){cross_dir=1;cross_ms=t.ms;}
                if(sx){cross_dir=-1;cross_ms=t.ms;}
            }
            prev_fast=pf; prev_slow=ps;
        }

        if (startup==0) startup=t.ms;
        if (t.ms-startup<120000LL) continue;
        int64_t day=(t.ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-200.0) continue;

        double mid=(t.bid+t.ask)*0.5, spread=t.ask-t.bid;

        if (pos.active) {
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.50){pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?t.bid:t.ask;
            if(pos.is_long?(t.bid>=pos.tp):(t.ask<=pos.tp)){
                dpnl+=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                s.record((pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100,"TP");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            if(pos.is_long?(t.bid<=pos.sl):(t.ask>=pos.sl)){
                double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=pnl; s.record(pnl,pos.be_done?"BE":"SL");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            if(t.ms-pos.ets>180000LL){
                double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=pnl; s.record(pnl,"TIMEOUT");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            // cross_exit disabled (data confirmed harmful)
            continue;
        }

        if (!fast.warmed||!slow.warmed||cross_dir==0||atr<=0) continue;
        if (t.ms/1000-last_exit<20) continue;
        if (spread>atr*0.30) continue;
        if (t.ms-cross_ms>(int64_t)cfg.max_lag_s*1000) { cross_dir=0; continue; }

        bool isl=(cross_dir==1);

        // LONG-only gate
        if (cfg.long_only && !isl) { cross_dir=0; continue; }

        // RSI gate
        if (!rsi_good(rsi, isl, cfg)) continue;

        // ATR mid-zone block (ATR 2-3 loses $247)
        if (cfg.atr_block_mid && atr>=2.0 && atr<3.0) continue;

        // EMA gap gate (stale entries lose)
        double ema_gap = std::fabs(fast.val - slow.val);
        if (ema_gap > cfg.gap_block) continue;

        // Drift against gate
        if (cfg.drift_gate) {
            double drift = t.drift;
            bool against = (isl && drift < -0.5) || (!isl && drift > 0.5);
            if (against) continue;
        }

        // Hour kill gate (05,12,14,16,20 UTC all bleed)
        if (cfg.hour_kill) {
            int h = utc_hour(t.ms);
            if (h==5||h==12||h==14||h==16||h==20) continue;
        }

        double sl_dist=atr*1.5, tp_dist=sl_dist*cfg.tp_rr;
        double cost=spread+0.20;
        if(tp_dist<=cost) continue;

        double sz=std::max(0.01,std::min(0.10,std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
        double ep=isl?t.ask:t.bid;
        pos.active=true; pos.is_long=isl; pos.entry=ep;
        pos.sl=isl?(ep-sl_dist):(ep+sl_dist);
        pos.tp=isl?(ep+tp_dist):(ep-tp_dist);
        pos.size=sz; pos.mfe=0; pos.be_done=false; pos.ets=t.ms;
        cross_dir=0;
    }
    if(pos.active&&!ticks.empty()){
        auto& lt=ticks.back();
        double ep=pos.is_long?lt.bid:lt.ask;
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
        s.record(pnl,"TIMEOUT");
    }
    return s;
}

int main(int argc, char** argv) {
    if(argc<2){fprintf(stderr,"Usage: ema_tuned file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());

    // Baseline (original best config from sweep)
    Config base;
    Stats bs=run(ticks,base);
    printf("BASELINE (original 9/15 sweep winner):\n");
    printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f TP=%d SL=%d TMO=%d\n\n",
           bs.T,bs.W,bs.wr(),bs.pnl,bs.avg(),bs.max_dd,bs.tp,bs.sl,bs.timeout);

    struct Result { Config c; Stats s; std::string label; };
    std::vector<Result> results;

    // Full sweep of all filter combinations
    for (bool lo  : {false, true})          // long_only
    for (int lag  : {30, 60, 180})          // max_lag_s
    for (bool atb : {false, true})          // atr_block_mid
    for (bool rsi_ex : {false, true})       // rsi_exclude_bad
    for (double gap : {0.30, 0.50, 999.0}) // gap_block
    for (bool dg  : {false, true})          // drift_gate
    for (bool hk  : {false, true})          // hour_kill
    for (double rr : {0.8, 1.0, 1.2})      // tp_rr
    {
        Config c;
        c.long_only       = lo;
        c.max_lag_s       = lag;
        c.atr_block_mid   = atb;
        c.rsi_exclude_bad = rsi_ex;
        c.gap_block       = gap;
        c.drift_gate      = dg;
        c.hour_kill       = hk;
        c.tp_rr           = rr;
        Stats s = run(ticks, c);
        results.push_back({c, s, ""});
    }

    std::sort(results.begin(), results.end(),
        [](auto& a, auto& b){ return a.s.pnl > b.s.pnl; });

    printf("%-4s %-3s %-4s %-4s %-5s %-4s %-4s %-4s %5s %4s %8s %5s %6s %7s %4s %4s %4s\n",
           "LO","LAG","ATB","RSIX","GAP","DG","HK","RR",
           "T","W","PNL","WR%","Avg","MaxDD","TP","SL","TMO");
    printf("%s\n", std::string(100,'-').c_str());

    int shown=0;
    for (auto& r : results) {
        if (r.s.T < 20) continue;
        if (shown++ >= 40) break;
        printf("%-4s %-3d %-4s %-4s %-5.2f %-4s %-4s %-3.1f "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f %4d %4d %4d\n",
               r.c.long_only?"Y":"N", r.c.max_lag_s,
               r.c.atr_block_mid?"Y":"N", r.c.rsi_exclude_bad?"Y":"N",
               r.c.gap_block, r.c.drift_gate?"Y":"N", r.c.hour_kill?"Y":"N",
               r.c.tp_rr,
               r.s.T, r.s.W, r.s.pnl, r.s.wr(), r.s.avg(), r.s.max_dd,
               r.s.tp, r.s.sl, r.s.timeout);
    }

    // Top 3 detail
    printf("\n=== TOP 3 DETAIL ===\n");
    shown=0;
    for (auto& r : results) {
        if (r.s.T < 20) continue;
        if (shown++ >= 3) break;
        printf("\n#%d: long_only=%s lag=%d atr_block=%s rsi_excl=%s gap=%.2f drift=%s hour_kill=%s rr=%.1f\n",
               shown, r.c.long_only?"Y":"N", r.c.max_lag_s,
               r.c.atr_block_mid?"Y":"N", r.c.rsi_exclude_bad?"Y":"N",
               r.c.gap_block, r.c.drift_gate?"Y":"N", r.c.hour_kill?"Y":"N", r.c.tp_rr);
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f TP=%d SL=%d TMO=%d\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd,
               r.s.tp,r.s.sl,r.s.timeout);
    }

    // Positive PnL table
    printf("\n=== POSITIVE PnL >= 20 TRADES ===\n");
    printf("%-4s %-3s %-4s %-4s %-5s %-4s %-4s %-4s %5s %4s %8s %5s %6s %7s\n",
           "LO","LAG","ATB","RSIX","GAP","DG","HK","RR","T","W","PNL","WR%","Avg","MaxDD");
    shown=0;
    for (auto& r : results) {
        if (r.s.T < 20 || r.s.pnl <= 0) continue;
        if (shown++ >= 30) break;
        printf("%-4s %-3d %-4s %-4s %-5.2f %-4s %-4s %-3.1f "
               "%5d %4d %8.2f %5.1f %6.2f %7.2f\n",
               r.c.long_only?"Y":"N", r.c.max_lag_s,
               r.c.atr_block_mid?"Y":"N", r.c.rsi_exclude_bad?"Y":"N",
               r.c.gap_block, r.c.drift_gate?"Y":"N", r.c.hour_kill?"Y":"N",
               r.c.tp_rr,
               r.s.T, r.s.W, r.s.pnl, r.s.wr(), r.s.avg(), r.s.max_dd);
    }

    return 0;
}
