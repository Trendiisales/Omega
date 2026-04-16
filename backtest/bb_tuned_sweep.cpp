// bb_tuned_sweep.cpp -- BB Mean Reversion TUNED sweep
//
// Filters derived from bb_diag.cpp analysis:
//
//   FINDING 1: London session only (H07-H12) is the profit driver.
//              London: 38.1% WR, $486. NY: 27.3%, $-63. Asia: 25.5%, $-91.
//
//   FINDING 2: Wednesday is catastrophic: 6.2% WR, -$212 on 16 trades.
//              Kill Wednesday.
//
//   FINDING 3: H13, H15, H02, H03 are structural bleeders (0-12% WR).
//              Kill those hours individually.
//
//   FINDING 4: SHORT earns $305, LONG earns $-13 overall.
//              LONG only viable in London session. Kill LONG in Asia/NY.
//
//   FINDING 5: BB overshoot > 0.30 has worst WR (25%). Barely/Moderate win.
//              Cap overshoot at 0.30 (avoids chasing trend continuations).
//
//   FINDING 6: SHORT_RSI > 80 is best RSI bucket (32.7% WR, $204).
//              SHORT_RSI 75-80 is 16.7% WR. Raising SHORT threshold to 80
//              concentrates on the best entries.
//
//   FINDING 7: LONG_RSI < 20 is worse than LONG_RSI 20-25 ($-74 vs $+96).
//              Tighten LONG to RSI 20-25 range only.
//
// NEW SWEEP PARAMETERS (derived from diagnostics, not guesses):
//   session_mode:  London_only | London_NY | All
//   kill_wed:      true/false
//   long_london_only: true/false  (LONG only in London, kill LONG in Asia/NY)
//   max_overshoot: 0.20, 0.30, 0.50, none
//   short_rsi_min: 75, 78, 80      (raise SHORT threshold)
//   long_rsi_max:  22, 25, 28      (tighten LONG threshold)
//   hour_kill_set: conservative (H13,H15) | aggressive (H02,H03,H13,H15,H22)
//
// Fixed from original sweep (confirmed optimal):
//   BB period=25, STD=2.0, SL=1.0, TMO=300, DIV=N, cooldown=30s
//
// Build: clang++ -O3 -std=c++20 -o /tmp/bb_tuned backtest/bb_tuned_sweep.cpp
// Run:   /tmp/bb_tuned ~/Downloads/l2_ticks_2026-04-*.csv

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ─────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return false; }
    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,ci=0;
    { std::istringstream h(line);
      while (std::getline(h,tok,',')) {
          if (tok=="ts_ms") cm=ci; if (tok=="bid") cb=ci;
          if (tok=="ask")   ca=ci; if (tok=="ewm_drift") cd=ci;
          ++ci; } }
    if (cb<0||ca<0) return false;
    size_t before=out.size();
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for (char* c=buf; *c&&nf<32; ++c)
            if (*c==',') { *c='\0'; flds[nf++]=c+1; }
        int need=std::max({cm,cb,ca,cd});
        if (nf<=need) continue;
        try {
            Tick t;
            t.ms   =(int64_t)std::stod(flds[cm]);
            t.bid  =std::stod(flds[cb]);
            t.ask  =std::stod(flds[ca]);
            t.drift=(cd>=0&&nf>cd)?std::stod(flds[cd]):0.0;
            out.push_back(t);
        } catch(...) {}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

static int  utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }
static int  dow(int64_t ms)      { return (int)((ms/1000LL/86400LL+4)%7); } // 0=Sun

// ─── M1 Bar + BB + RSI ────────────────────────────────────────────────────────
struct M1Bar { double open,high,low,close; int64_t bms; };

struct Indicators {
    std::deque<double> closes;
    double bb_mid=0,bb_upper=0,bb_lower=0,bb_width=0;
    std::deque<double> rg,rl; double rp=0,rsi=50;
    std::deque<double> rsi_hist;
    double atr=2.0;
    std::deque<M1Bar> bars;
    M1Bar cur{}; int64_t cbms=0;

    bool update(double bid, double ask, int64_t ms,
                int bb_period, double bb_std) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        if (rp>0) {
            double chg=mid-rp;
            rg.push_back(chg>0?chg:0); rl.push_back(chg<0?-chg:0);
            if ((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if ((int)rg.size()==14) {
                double ag=0,al=0;
                for(auto x:rg)ag+=x; for(auto x:rl)al+=x;
                ag/=14; al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al);
            }
        }
        rp=mid;
        bool nb=false;
        if (cbms==0){cur={mid,mid,mid,mid,bms};cbms=bms;}
        else if (bms!=cbms) {
            bars.push_back(cur); if((int)bars.size()>50)bars.pop_front();
            if ((int)bars.size()>=2) {
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,
                                   std::fabs(b.high-pb.close),
                                   std::fabs(b.low-pb.close)});}
                atr=sum/n;
            }
            closes.push_back(cur.close);
            if ((int)closes.size()>bb_period) closes.pop_front();
            if ((int)closes.size()==bb_period) {
                double sum=0; for(auto x:closes)sum+=x;
                bb_mid=sum/bb_period;
                double var=0; for(auto x:closes)var+=(x-bb_mid)*(x-bb_mid);
                double sd=std::sqrt(var/bb_period);
                bb_upper=bb_mid+bb_std*sd;
                bb_lower=bb_mid-bb_std*sd;
                bb_width=bb_upper-bb_lower;
            }
            rsi_hist.push_back(rsi);
            if ((int)rsi_hist.size()>5)rsi_hist.pop_front();
            cur={mid,mid,mid,mid,bms}; cbms=bms; nb=true;
        } else {
            if(mid>cur.high)cur.high=mid;
            if(mid<cur.low) cur.low=mid;
            cur.close=mid;
        }
        return nb;
    }

    // overshoot: how far outside band relative to half-band width
    double overshoot(bool is_short) const {
        if (bb_width<=0||(int)closes.size()<1) return 0;
        double half=bb_width/2.0;
        if (half<=0) return 0;
        if (is_short) return (closes.back()-bb_upper)/half;
        else          return (bb_lower-closes.back())/half;
    }
};

// ─── Config ───────────────────────────────────────────────────────────────────
enum SessionMode { ALL_SESSIONS=0, LONDON_NY=1, LONDON_ONLY=2 };

struct Config {
    // Fixed (confirmed optimal from original sweep)
    int    bb_period    = 25;
    double bb_std       = 2.0;
    double sl_mult      = 1.0;
    int    timeout_s    = 300;
    int    cooldown_s   = 30;

    // Tunable (from diagnostic findings)
    SessionMode session_mode   = ALL_SESSIONS;
    bool   kill_wednesday      = false;
    bool   long_london_only    = false;  // kill LONG outside London
    double max_overshoot       = 99.0;   // cap on bb_overshoot (99=disabled)
    double short_rsi_min       = 70.0;   // raise to concentrate on RSI>80 bucket
    double long_rsi_max        = 30.0;   // tighten to RSI<22 or RSI<25
    bool   kill_hours_conserv  = false;  // kill H13, H15
    bool   kill_hours_aggrssv  = false;  // kill H02, H03, H13, H15, H22
};

// ─── Stats ────────────────────────────────────────────────────────────────────
struct Stats {
    int T=0,W=0; double pnl=0,max_dd=0,peak=0;
    int tp=0,sl=0,tmo=0;
    void record(double p, const char* r) {
        ++T; if(p>0)++W; pnl+=p;
        peak=std::max(peak,pnl); max_dd=std::max(max_dd,peak-pnl);
        if(strcmp(r,"TP")==0)++tp;
        else if(strcmp(r,"SL")==0||strcmp(r,"BE")==0)++sl;
        else ++tmo;
    }
    double wr()  const { return T>0?100.0*W/T:0; }
    double avg() const { return T>0?pnl/T:0; }
};

static Stats run(const std::vector<Tick>& ticks, const Config& cfg) {
    Indicators ind;
    Stats s;

    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0;
        int64_t ets=0;
    } pos;

    int64_t last_exit=0,startup=0;
    double dpnl=0; int64_t dday=0;

    for (auto& t:ticks) {
        bool nb=ind.update(t.bid,t.ask,t.ms,cfg.bb_period,cfg.bb_std);
        if (startup==0) startup=t.ms;
        if (t.ms-startup<120000LL) continue;

        int64_t day=(t.ms/1000LL)/86400LL;
        if (day!=dday){dpnl=0;dday=day;}
        if (dpnl<=-200.0) continue;

        double mid=(t.bid+t.ask)*0.5, spread=t.ask-t.bid;

        // Position management
        if (pos.active) {
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.40){pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?t.bid:t.ask;
            if(pos.is_long?(t.bid>=pos.tp):(t.ask<=pos.tp)){
                dpnl+=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                s.record((pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100,"TP");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            if(pos.is_long?(t.bid<=pos.sl):(t.ask>=pos.sl)){
                double p=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=p; s.record(p,pos.be_done?"BE":"SL");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            if(t.ms-pos.ets>(int64_t)cfg.timeout_s*1000){
                double p=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl+=p; s.record(p,"TIMEOUT");
                last_exit=t.ms/1000; pos=Pos{}; continue;}
            continue;
        }

        if (!nb) continue;
        if (ind.bb_width<0.5) continue;
        if (ind.bars.empty()) continue;
        auto& lb=ind.bars.back();
        if (t.ms/1000-last_exit<cfg.cooldown_s) continue;
        if (spread>ind.atr*0.25) continue;

        double bar_range=lb.high-lb.low;
        if (bar_range>ind.atr*3.0) continue;
        if (bar_range<0.05) continue;

        int h   = utc_hour(t.ms);
        int day_of_week = dow(t.ms);

        // ── Wednesday kill ────────────────────────────────────────────────────
        if (cfg.kill_wednesday && day_of_week==3) continue;

        // ── Hour kill sets ────────────────────────────────────────────────────
        if (cfg.kill_hours_aggrssv) {
            if (h==2||h==3||h==13||h==15||h==22) continue;
        } else if (cfg.kill_hours_conserv) {
            if (h==13||h==15) continue;
        }

        // ── Session gate ──────────────────────────────────────────────────────
        bool in_london = (h>=7 && h<13);
        bool in_ny     = (h>=13 && h<20);
        if (cfg.session_mode==LONDON_ONLY && !in_london) continue;
        if (cfg.session_mode==LONDON_NY   && !in_london && !in_ny) continue;

        // ── SHORT entry ───────────────────────────────────────────────────────
        if (lb.close>ind.bb_upper && lb.close>lb.open) {
            if (ind.rsi < cfg.short_rsi_min) goto check_long;

            // Overshoot cap
            double ov=ind.overshoot(true);
            if (ov>cfg.max_overshoot) goto check_long;

            double sl_px  =lb.high+bar_range*cfg.sl_mult*0.5;
            double sl_dist=std::fabs(t.bid-sl_px);
            if (sl_dist<=0) goto check_long;
            double tp_px  =ind.bb_mid;
            double tp_dist=t.bid-tp_px;
            if (tp_dist<sl_dist*0.5) goto check_long;
            double cost=spread+0.20;
            if (tp_dist<=cost) goto check_long;

            double sz=std::max(0.01,std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
            pos={true,false,false,t.bid,sl_px,tp_px,sz,0,t.ms};
            continue;
        }

        check_long:
        // ── LONG entry ────────────────────────────────────────────────────────
        if (lb.close<ind.bb_lower && lb.close<lb.open) {
            if (ind.rsi > cfg.long_rsi_max) continue;

            // Long london-only gate
            if (cfg.long_london_only && !in_london) continue;

            // Overshoot cap
            double ov=ind.overshoot(false);
            if (ov>cfg.max_overshoot) continue;

            double sl_px  =lb.low-bar_range*cfg.sl_mult*0.5;
            double sl_dist=std::fabs(t.ask-sl_px);
            if (sl_dist<=0) continue;
            double tp_px  =ind.bb_mid;
            double tp_dist=tp_px-t.ask;
            if (tp_dist<sl_dist*0.5) continue;
            double cost=spread+0.20;
            if (tp_dist<=cost) continue;

            double sz=std::max(0.01,std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
            pos={true,true,false,t.ask,sl_px,tp_px,sz,0,t.ms};
            continue;
        }
    }

    if (pos.active&&!ticks.empty()) {
        auto& lt=ticks.back();
        double ep=pos.is_long?lt.bid:lt.ask;
        double p=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
        s.record(p,"TIMEOUT");
    }
    return s;
}

// ─── Label helper ─────────────────────────────────────────────────────────────
static std::string label(const Config& c) {
    char buf[128];
    char ov_str[16];
    if (c.max_overshoot>=99) snprintf(ov_str,16,"off");
    else                     snprintf(ov_str,16,"%.2f",c.max_overshoot);
    snprintf(buf,128,
        "sess=%s wed=%s llong=%s ov=%s srsi=%.0f lrsi=%.0f hk=%s",
        c.session_mode==LONDON_ONLY?"Lon":c.session_mode==LONDON_NY?"L+NY":"All",
        c.kill_wednesday?"Y":"N",
        c.long_london_only?"Y":"N",
        ov_str,
        c.short_rsi_min,
        c.long_rsi_max,
        c.kill_hours_aggrssv?"Agg":c.kill_hours_conserv?"Con":"N");
    return std::string(buf);
}

int main(int argc, char** argv) {
    if (argc<2) { fprintf(stderr,"Usage: bb_tuned file1.csv ...\n"); return 1; }
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for (int i=1;i<argc;++i) load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());
    if (ticks.empty()) return 1;

    // Baseline (original best config, no new filters)
    {
        Config base;
        Stats bs=run(ticks,base);
        printf("BASELINE (no tuning): T=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f TP=%d SL=%d\n\n",
               bs.T,bs.wr(),bs.pnl,bs.avg(),bs.max_dd,bs.tp,bs.sl);
    }

    struct Result { Config c; Stats s; };
    std::vector<Result> results;

    // ── Full grid sweep of diagnostic-derived parameters ──────────────────────
    for (auto sess : {ALL_SESSIONS, LONDON_NY, LONDON_ONLY})
    for (bool  kw  : {false, true})
    for (bool  ll  : {false, true})
    for (double ov : {99.0, 0.50, 0.30, 0.20})
    for (double sr : {70.0, 75.0, 78.0, 80.0})
    for (double lr : {30.0, 27.0, 25.0, 22.0})
    for (int   hk  : {0, 1, 2})   // 0=none 1=conservative 2=aggressive
    {
        Config c;
        c.session_mode        = sess;
        c.kill_wednesday      = kw;
        c.long_london_only    = ll;
        c.max_overshoot       = ov;
        c.short_rsi_min       = sr;
        c.long_rsi_max        = lr;
        c.kill_hours_conserv  = (hk==1);
        c.kill_hours_aggrssv  = (hk==2);
        results.push_back({c, run(ticks,c)});
    }

    std::sort(results.begin(),results.end(),
              [](auto& a,auto& b){ return a.s.pnl>b.s.pnl; });

    // ── Top 40 by PnL ─────────────────────────────────────────────────────────
    printf("%-50s %5s %4s %6s %5s %6s %7s %3s %3s %3s\n",
           "Config","T","W","PnL","WR%","Avg","MaxDD","TP","SL","TMO");
    printf("%s\n",std::string(105,'-').c_str());
    int shown=0;
    for (auto& r:results) {
        if (r.s.T<10) continue;
        if (shown++>=40) break;
        printf("%-50s %5d %4d %6.2f %5.1f %6.2f %7.2f %3d %3d %3d\n",
               label(r.c).c_str(),
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd,
               r.s.tp,r.s.sl,r.s.tmo);
    }

    // ── Top results filtered: WR >= 40% ───────────────────────────────────────
    printf("\n=== WR >= 40%% (min 10 trades) ===\n");
    printf("%-50s %5s %4s %6s %5s %6s %7s\n",
           "Config","T","W","PnL","WR%","Avg","MaxDD");
    printf("%s\n",std::string(88,'-').c_str());
    shown=0;
    for (auto& r:results) {
        if (r.s.T<10||r.s.wr()<40.0) continue;
        if (shown++>=20) break;
        printf("%-50s %5d %4d %6.2f %5.1f %6.2f %7.2f\n",
               label(r.c).c_str(),
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }
    if (shown==0) printf("(none -- try lowering WR threshold)\n");

    // ── Top results filtered: WR >= 35% ───────────────────────────────────────
    printf("\n=== WR >= 35%% (min 10 trades) ===\n");
    printf("%-50s %5s %4s %6s %5s %6s %7s\n",
           "Config","T","W","PnL","WR%","Avg","MaxDD");
    printf("%s\n",std::string(88,'-').c_str());
    shown=0;
    for (auto& r:results) {
        if (r.s.T<10||r.s.wr()<35.0) continue;
        if (shown++>=20) break;
        printf("%-50s %5d %4d %6.2f %5.1f %6.2f %7.2f\n",
               label(r.c).c_str(),
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }
    if (shown==0) printf("(none)\n");

    // ── Best by WR (ranked) ───────────────────────────────────────────────────
    printf("\n=== TOP 10 BY WIN RATE (min 10 trades) ===\n");
    printf("%-50s %5s %4s %6s %5s %6s %7s\n",
           "Config","T","W","PnL","WR%","Avg","MaxDD");
    printf("%s\n",std::string(88,'-').c_str());
    std::sort(results.begin(),results.end(),
              [](auto& a,auto& b){ return a.s.wr()>b.s.wr(); });
    shown=0;
    for (auto& r:results) {
        if (r.s.T<10) continue;
        if (shown++>=10) break;
        printf("%-50s %5d %4d %6.2f %5.1f %6.2f %7.2f\n",
               label(r.c).c_str(),
               r.s.T,r.s.W,r.s.pnl,r.s.wr(),r.s.avg(),r.s.max_dd);
    }

    // ── Detail on the single best WR config ───────────────────────────────────
    std::sort(results.begin(),results.end(),
              [](auto& a,auto& b){
                  // rank by WR if T>=10, then PnL as tiebreak
                  bool qa=(a.s.T>=10), qb=(b.s.T>=10);
                  if(qa!=qb)return qa>qb;
                  if(std::fabs(a.s.wr()-b.s.wr())>0.1)return a.s.wr()>b.s.wr();
                  return a.s.pnl>b.s.pnl;});

    for (auto& r:results) {
        if (r.s.T<10) continue;
        printf("\n=== BEST CONFIG (WR-ranked, T>=10) ===\n");
        printf("  %s\n",label(r.c).c_str());
        printf("  T=%d W=%d WR=%.1f%% PnL=$%.2f Avg=$%.2f MaxDD=$%.2f\n",
               r.s.T,r.s.W,r.s.wr(),r.s.pnl,r.s.avg(),r.s.max_dd);
        printf("  TP=%d SL=%d TMO=%d\n",r.s.tp,r.s.sl,r.s.tmo);
        break;
    }

    return 0;
}
