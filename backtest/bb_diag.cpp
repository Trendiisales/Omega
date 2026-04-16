// bb_diag.cpp -- Bollinger Band Mean Reversion trade-level diagnostic
//
// GOAL: Understand WHY WR is 29-40%. Find which subsets win vs bleed.
//
// Best sweep config: BP=25, STD=2.0, OB=70, OS=30, DIV=N, SL=1.0, TMO=300
// Problem: 100/148 are SL hits. Need to find the winning subset.
//
// Analysis axes:
//   1.  Direction (LONG vs SHORT) -- are we symmetrically wrong or one-sided?
//   2.  Hour of day (UTC) -- which hours win vs bleed?
//   3.  Session slot (Asia/London/NY)
//   4.  Day of week
//   5.  Drift alignment -- is ewm_drift confirming the trade direction?
//   6.  RSI bucket at entry (50-60, 60-70, 70-80, 80+)
//   7.  BB overextension -- how far outside the band? (mild vs extreme)
//   8.  Bar range relative to ATR (small/medium/large bar)
//   9.  Distance to midline at entry (TP target distance vs SL)
//  10.  Consecutive SL streak context (does losing streak predict next loss?)
//  11.  Exit type analysis per subset
//  12.  MFE analysis (trades that hit SL: how far did price go our way first?)
//  13.  VWAP context -- is price above/below VWAP (daily rolling) at entry?
//  14.  Momentum: RSI slope 3 bars (rising/falling) at entry
//  15.  Band width at entry (tight vs wide bands)
//
// Key hypothesis to test:
//   H1: LONG trades win, SHORT trades bleed (gold has upward drift in 2026)
//   H2: Drift-aligned entries have 50%+ WR; counter-drift entries bleed
//   H3: NY session (13-17 UTC) entries have better WR than Asia entries
//   H4: Extreme overextension (>1.5x outside band) has better snap-back WR
//   H5: Tight bands (BB width < 1.5) = chop = bad
//
// Build: clang++ -O3 -std=c++20 -o /tmp/bb_diag bb_diag.cpp
// Run:   /tmp/bb_diag ~/Downloads/l2_ticks_2026-04-*.csv

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <numeric>
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
    if (!line.empty() && line.back() == '\r') line.pop_back();
    int cm=-1, cb=-1, ca=-1, cd=-1, ci=0;
    { std::istringstream h(line);
      while (std::getline(h, tok, ',')) {
          if (tok=="ts_ms") cm=ci; if (tok=="bid") cb=ci;
          if (tok=="ask")   ca=ci; if (tok=="ewm_drift") cd=ci;
          ++ci; } }
    if (cb < 0 || ca < 0) return false;
    size_t before = out.size();
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size() >= sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for (char* c=buf; *c && nf<32; ++c)
            if (*c==',') { *c='\0'; flds[nf++]=c+1; }
        int need = std::max({cm,cb,ca,cd});
        if (nf <= need) continue;
        try {
            Tick t;
            t.ms   = (int64_t)std::stod(flds[cm]);
            t.bid  = std::stod(flds[cb]);
            t.ask  = std::stod(flds[ca]);
            t.drift= (cd>=0 && nf>cd) ? std::stod(flds[cd]) : 0.0;
            out.push_back(t);
        } catch (...) {}
    }
    fprintf(stderr, "Loaded %s: %zu\n", path, out.size()-before);
    return true;
}

static int utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }
static int dow(int64_t ms)      { return (int)((ms/1000LL/86400LL + 4) % 7); } // 0=Sun

static const char* session(int64_t ms) {
    int h = utc_hour(ms);
    if (h>=0  && h<7)  return "Asia";
    if (h>=7  && h<13) return "London";
    if (h>=13 && h<20) return "NY";
    return "OffHours";
}

// ─── M1 Bar + BB + RSI + VWAP ─────────────────────────────────────────────────
struct M1Bar { double open, high, low, close; int64_t bms; };

struct Indicators {
    // BB
    std::deque<double> closes;
    double bb_mid=0, bb_upper=0, bb_lower=0, bb_width=0;

    // RSI
    std::deque<double> rg, rl;
    double rp=0, rsi=50;

    // RSI bar history (for slope)
    std::deque<double> rsi_hist;

    // ATR
    double atr=2.0;

    // VWAP (daily rolling)
    double vwap_num=0, vwap_den=0;
    int64_t vwap_day=0;

    // Bars
    std::deque<M1Bar> bars;
    M1Bar cur{}; int64_t cbms=0;

    static const int BB_PERIOD = 25;
    static const double BB_STD;  // 2.0

    bool update(double bid, double ask, int64_t ms) {
        double mid = (bid+ask)*0.5;
        int64_t bms = (ms/60000LL)*60000LL;

        // RSI tick update
        if (rp > 0) {
            double chg = mid - rp;
            rg.push_back(chg>0 ? chg : 0);
            rl.push_back(chg<0 ? -chg : 0);
            if ((int)rg.size()>14) { rg.pop_front(); rl.pop_front(); }
            if ((int)rg.size()==14) {
                double ag=0, al=0;
                for (auto x:rg) ag+=x; for (auto x:rl) al+=x;
                ag/=14; al/=14;
                rsi = (al==0) ? 100.0 : 100.0 - 100.0/(1.0+ag/al);
            }
        }
        rp = mid;

        // VWAP daily reset
        int64_t day = (ms/1000LL)/86400LL;
        if (day != vwap_day) { vwap_num=0; vwap_den=0; vwap_day=day; }
        vwap_num += mid; vwap_den += 1.0;

        bool nb = false;
        if (cbms==0) { cur={mid,mid,mid,mid,bms}; cbms=bms; }
        else if (bms != cbms) {
            bars.push_back(cur);
            if ((int)bars.size()>50) bars.pop_front();

            // ATR
            if ((int)bars.size()>=2) {
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for (int i=(int)bars.size()-1; i>=(int)bars.size()-n; --i) {
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,
                                   std::fabs(b.high-pb.close),
                                   std::fabs(b.low-pb.close)});
                }
                atr = sum/n;
            }

            // BB on bar closes
            closes.push_back(cur.close);
            if ((int)closes.size()>BB_PERIOD) closes.pop_front();
            if ((int)closes.size()==BB_PERIOD) {
                double sum=0; for (auto x:closes) sum+=x;
                bb_mid = sum/BB_PERIOD;
                double var=0; for (auto x:closes) var+=(x-bb_mid)*(x-bb_mid);
                double sd = std::sqrt(var/BB_PERIOD);
                bb_upper = bb_mid + 2.0*sd;
                bb_lower = bb_mid - 2.0*sd;
                bb_width = bb_upper - bb_lower;
            }

            // RSI history for slope
            rsi_hist.push_back(rsi);
            if ((int)rsi_hist.size()>5) rsi_hist.pop_front();

            cur = {mid,mid,mid,mid,bms}; cbms=bms; nb=true;
        } else {
            if (mid>cur.high) cur.high=mid;
            if (mid<cur.low)  cur.low=mid;
            cur.close=mid;
        }
        return nb;
    }

    double vwap() const { return vwap_den>0 ? vwap_num/vwap_den : 0; }

    // RSI slope: positive = rising, negative = falling
    double rsi_slope() const {
        if ((int)rsi_hist.size()<3) return 0;
        int n = (int)rsi_hist.size();
        return rsi_hist[n-1] - rsi_hist[n-3];
    }

    // How many standard deviations outside the band?
    double band_overshoot(bool is_short) const {
        if (bb_width <= 0 || (int)closes.size()<BB_PERIOD) return 0;
        double half = bb_width/2.0;
        if (half <= 0) return 0;
        if (is_short) return (closes.back() - bb_upper) / half;
        else          return (bb_lower - closes.back()) / half;
    }
};
const double Indicators::BB_STD = 2.0;

// ─── Trade record ─────────────────────────────────────────────────────────────
struct Trade {
    bool is_long;
    double entry, exit_px, pnl, mfe, mae; // mae = max adverse excursion
    double entry_atr, entry_rsi, entry_drift;
    double bb_mid_dist;      // distance from entry to bb_mid (TP target)
    double sl_dist;          // distance from entry to SL
    double rr_at_entry;      // tp_dist / sl_dist
    double bb_overshoot;     // how far outside band (in half-band units)
    double bb_width;
    double vwap_dist;        // price - vwap at entry (positive = above vwap)
    double rsi_slope;        // RSI rising/falling
    bool   drift_aligned;    // drift confirms direction
    bool   vwap_aligned;     // for short: price > vwap; for long: price < vwap
    int    hour, slot_id;
    int    dow_id;
    int    held_s;
    std::string reason;
    std::string session_name;
};

// ─── Bucket analysis helper ───────────────────────────────────────────────────
struct Bucket {
    int T=0, W=0; double pnl=0, mfe_sum=0, mae_sum=0;
    void add(const Trade& t) {
        ++T; if(t.pnl>0) ++W;
        pnl += t.pnl; mfe_sum += t.mfe; mae_sum += t.mae;
    }
    double wr()  const { return T>0 ? 100.0*W/T : 0; }
    double avg() const { return T>0 ? pnl/T : 0; }
    double mfe_avg() const { return T>0 ? mfe_sum/T : 0; }
    double mae_avg() const { return T>0 ? mae_sum/T : 0; }
};

// ─── Engine (fixed at best sweep params: BP=25 STD=2.0 OB=70 OS=30 SL=1.0 TMO=300) ──
static std::vector<Trade> run_diag(const std::vector<Tick>& ticks) {
    Indicators ind;
    std::vector<Trade> trades;

    const double RSI_OB   = 70.0;
    const double RSI_OS   = 30.0;
    const double SL_MULT  = 1.0;
    const int    TIMEOUT_S= 300;
    const int    COOLDOWN_S=30;

    struct Pos {
        bool active=false, is_long=false, be_done=false;
        double entry=0, sl=0, tp=0, size=0, mfe=0, mae=0;
        int64_t ets=0;
        // context at entry
        double entry_atr, entry_rsi, entry_drift;
        double bb_mid_dist, sl_dist, rr_at_entry;
        double bb_overshoot, bb_width, vwap_dist, rsi_slope;
        bool   drift_aligned, vwap_aligned;
        int    hour, slot_id, dow_id;
        std::string session_name;
    } pos;

    int64_t last_exit=0, startup=0;
    double dpnl=0; int64_t dday=0;

    auto record_trade = [&](double exit_px, const std::string& reason, int64_t exit_ms) {
        Trade t;
        t.is_long       = pos.is_long;
        t.entry         = pos.entry;
        t.exit_px       = exit_px;
        double raw_pnl  = (pos.is_long ? (exit_px-pos.entry) : (pos.entry-exit_px));
        t.pnl           = raw_pnl * pos.size * 100.0;
        t.mfe           = pos.mfe;
        t.mae           = pos.mae;
        t.entry_atr     = pos.entry_atr;
        t.entry_rsi     = pos.entry_rsi;
        t.entry_drift   = pos.entry_drift;
        t.bb_mid_dist   = pos.bb_mid_dist;
        t.sl_dist       = pos.sl_dist;
        t.rr_at_entry   = pos.rr_at_entry;
        t.bb_overshoot  = pos.bb_overshoot;
        t.bb_width      = pos.bb_width;
        t.vwap_dist     = pos.vwap_dist;
        t.rsi_slope     = pos.rsi_slope;
        t.drift_aligned = pos.drift_aligned;
        t.vwap_aligned  = pos.vwap_aligned;
        t.hour          = pos.hour;
        t.slot_id       = pos.slot_id;
        t.dow_id        = pos.dow_id;
        t.session_name  = pos.session_name;
        t.held_s        = (int)((exit_ms - pos.ets)/1000LL);
        t.reason        = reason;
        trades.push_back(t);
    };

    for (auto& tk : ticks) {
        bool nb = ind.update(tk.bid, tk.ask, tk.ms);
        if (startup==0) startup=tk.ms;
        if (tk.ms - startup < 120000LL) continue;

        int64_t day = (tk.ms/1000LL)/86400LL;
        if (day != dday) { dpnl=0; dday=day; }
        if (dpnl <= -200.0) continue;

        double mid    = (tk.bid+tk.ask)*0.5;
        double spread = tk.ask - tk.bid;

        // Position management
        if (pos.active) {
            double mv = pos.is_long ? (mid-pos.entry) : (pos.entry-mid);
            if (mv > pos.mfe) pos.mfe = mv;
            double adv = pos.is_long ? (pos.entry-mid) : (mid-pos.entry);
            if (adv > pos.mae) pos.mae = adv;

            double td = std::fabs(pos.tp - pos.entry);
            if (!pos.be_done && td>0 && pos.mfe >= td*0.40) {
                pos.sl=pos.entry; pos.be_done=true;
            }

            double ep = pos.is_long ? tk.bid : tk.ask;
            if (pos.is_long ? (tk.bid>=pos.tp) : (tk.ask<=pos.tp)) {
                dpnl += (pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                record_trade(ep, "TP", tk.ms);
                last_exit=tk.ms/1000; pos=Pos{}; continue;
            }
            if (pos.is_long ? (tk.bid<=pos.sl) : (tk.ask>=pos.sl)) {
                double p=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl += p;
                record_trade(ep, pos.be_done?"BE":"SL", tk.ms);
                last_exit=tk.ms/1000; pos=Pos{}; continue;
            }
            if (tk.ms - pos.ets > (int64_t)TIMEOUT_S*1000) {
                double p=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100;
                dpnl += p;
                record_trade(ep, "TIMEOUT", tk.ms);
                last_exit=tk.ms/1000; pos=Pos{}; continue;
            }
            continue;
        }

        if (!nb) continue;  // entry only at bar close
        if (ind.bb_width < 0.5) continue;
        if (ind.bars.empty()) continue;
        auto& lb = ind.bars.back();
        if (tk.ms/1000 - last_exit < COOLDOWN_S) continue;
        if (spread > ind.atr*0.25) continue;

        double bar_range = lb.high - lb.low;
        if (bar_range > ind.atr*3.0) continue;
        if (bar_range < 0.05) continue;

        int    h       = utc_hour(tk.ms);
        int    d       = dow(tk.ms);
        double vw      = ind.vwap();
        double rs      = ind.rsi_slope();
        double drift   = tk.drift;

        // slot: 0=Asia 1=London 2=NY 3=OffHours
        int slot_id = 0;
        if (h>=7  && h<13) slot_id=1;
        if (h>=13 && h<20) slot_id=2;
        if (h>=20 || h<0)  slot_id=3;

        // ── SHORT entry ──────────────────────────────────────────────────────
        if (lb.close > ind.bb_upper && lb.close > lb.open) {
            if (ind.rsi < RSI_OB) goto check_long;

            double sl_px   = lb.high + bar_range * SL_MULT * 0.5;
            double sl_dist = std::fabs(tk.bid - sl_px);
            if (sl_dist <= 0) goto check_long;

            double tp_px   = ind.bb_mid;
            double tp_dist = tk.bid - tp_px;
            if (tp_dist < sl_dist*0.5) goto check_long;

            double cost = spread + 0.20;
            if (tp_dist <= cost) goto check_long;

            double sz = std::max(0.01, std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));

            pos.active       = true;
            pos.is_long      = false;
            pos.entry        = tk.bid;
            pos.sl           = sl_px;
            pos.tp           = tp_px;
            pos.size         = sz;
            pos.mfe=0; pos.mae=0; pos.be_done=false;
            pos.ets          = tk.ms;
            pos.entry_atr    = ind.atr;
            pos.entry_rsi    = ind.rsi;
            pos.entry_drift  = drift;
            pos.bb_mid_dist  = tp_dist;
            pos.sl_dist      = sl_dist;
            pos.rr_at_entry  = tp_dist / sl_dist;
            pos.bb_overshoot = ind.band_overshoot(true);
            pos.bb_width     = ind.bb_width;
            pos.vwap_dist    = (vw>0) ? (tk.bid - vw) : 0;
            pos.rsi_slope    = rs;
            pos.drift_aligned= (drift < 0);   // short: want negative drift
            pos.vwap_aligned = (vw>0) ? (tk.bid > vw) : false; // short: want price above vwap
            pos.hour         = h;
            pos.slot_id      = slot_id;
            pos.dow_id       = d;
            pos.session_name = session(tk.ms);
            continue;
        }

        check_long:
        // ── LONG entry ───────────────────────────────────────────────────────
        if (lb.close < ind.bb_lower && lb.close < lb.open) {
            if (ind.rsi > RSI_OS) continue;

            double sl_px   = lb.low - bar_range * SL_MULT * 0.5;
            double sl_dist = std::fabs(tk.ask - sl_px);
            if (sl_dist <= 0) continue;

            double tp_px   = ind.bb_mid;
            double tp_dist = tp_px - tk.ask;
            if (tp_dist < sl_dist*0.5) continue;

            double cost = spread + 0.20;
            if (tp_dist <= cost) continue;

            double sz = std::max(0.01, std::min(0.10,
                std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));

            pos.active       = true;
            pos.is_long      = true;
            pos.entry        = tk.ask;
            pos.sl           = sl_px;
            pos.tp           = tp_px;
            pos.size         = sz;
            pos.mfe=0; pos.mae=0; pos.be_done=false;
            pos.ets          = tk.ms;
            pos.entry_atr    = ind.atr;
            pos.entry_rsi    = ind.rsi;
            pos.entry_drift  = drift;
            pos.bb_mid_dist  = tp_dist;
            pos.sl_dist      = sl_dist;
            pos.rr_at_entry  = tp_dist / sl_dist;
            pos.bb_overshoot = ind.band_overshoot(false);
            pos.bb_width     = ind.bb_width;
            pos.vwap_dist    = (vw>0) ? (tk.ask - vw) : 0;
            pos.rsi_slope    = rs;
            pos.drift_aligned= (drift > 0);   // long: want positive drift
            pos.vwap_aligned = (vw>0) ? (tk.ask < vw) : false; // long: want price below vwap
            pos.hour         = h;
            pos.slot_id      = slot_id;
            pos.dow_id       = d;
            pos.session_name = session(tk.ms);
            continue;
        }
    }

    // Close open position at end
    if (pos.active && !ticks.empty()) {
        auto& lt = ticks.back();
        double ep = pos.is_long ? lt.bid : lt.ask;
        record_trade(ep, "TIMEOUT", lt.ms);
    }

    return trades;
}

// ─── Print bucket map ─────────────────────────────────────────────────────────
static void print_buckets(const std::map<std::string, Bucket>& m,
                          const char* title, int min_trades=5) {
    printf("\n── %s ─────────────────────────────────────────────────────\n", title);
    printf("%-24s %5s %4s  %6s  %5s  %6s  MFE_avg  MAE_avg\n",
           "Bucket", "T", "W", "PnL", "WR%", "Avg$");
    printf("%s\n", std::string(75,'-').c_str());
    // Sort by WR descending (only buckets meeting min_trades)
    std::vector<std::pair<std::string,Bucket>> sorted(m.begin(), m.end());
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b){ return a.second.wr() > b.second.wr(); });
    for (auto& [k,v] : sorted) {
        if (v.T < min_trades) continue;
        printf("%-24s %5d %4d  %6.2f  %5.1f  %6.2f  %7.2f  %7.2f\n",
               k.c_str(), v.T, v.W, v.pnl, v.wr(), v.avg(),
               v.mfe_avg(), v.mae_avg());
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: bb_diag file1.csv ...\n");
        return 1;
    }

    std::vector<Tick> ticks;
    ticks.reserve(2000000);
    for (int i=1; i<argc; ++i) load_csv(argv[i], ticks);
    fprintf(stderr, "Total: %zu ticks\n\n", ticks.size());
    if (ticks.empty()) return 1;

    auto trades = run_diag(ticks);

    // ── Summary ───────────────────────────────────────────────────────────────
    int T=0, W=0; double pnl=0, mfe_sum=0, mae_sum=0;
    int tp_ct=0, sl_ct=0, be_ct=0, tmo_ct=0;
    for (auto& t : trades) {
        ++T; if(t.pnl>0) ++W; pnl+=t.pnl;
        mfe_sum+=t.mfe; mae_sum+=t.mae;
        if (t.reason=="TP")      ++tp_ct;
        else if(t.reason=="SL")  ++sl_ct;
        else if(t.reason=="BE")  ++be_ct;
        else                     ++tmo_ct;
    }
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("BB MEAN REVERSION DIAGNOSTIC -- BP=25 STD=2.0 OB=70 OS=30 SL=1.0 TMO=300\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("Total: %d  Win: %d  WR: %.1f%%  PnL: $%.2f  Avg: $%.2f\n",
           T, W, T>0?100.0*W/T:0, pnl, T>0?pnl/T:0);
    printf("Exits: TP=%d  SL=%d  BE=%d  TMO=%d\n", tp_ct, sl_ct, be_ct, tmo_ct);
    printf("MFE avg: %.2f  MAE avg: %.2f\n",
           T>0?mfe_sum/T:0, T>0?mae_sum/T:0);

    // ── Axis 1: Direction ─────────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) m[t.is_long?"LONG":"SHORT"].add(t);
        print_buckets(m, "1. DIRECTION (LONG vs SHORT)", 3);
    }

    // ── Axis 2: Hour ──────────────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            char buf[8]; snprintf(buf,8,"H%02d",t.hour);
            m[buf].add(t);
        }
        print_buckets(m, "2. HOUR (UTC)", 3);
    }

    // ── Axis 3: Session ───────────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) m[t.session_name].add(t);
        print_buckets(m, "3. SESSION", 3);
    }

    // ── Axis 4: Day of week ───────────────────────────────────────────────────
    {
        const char* dnames[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        std::map<std::string,Bucket> m;
        for (auto& t:trades) m[dnames[t.dow_id]].add(t);
        print_buckets(m, "4. DAY OF WEEK", 3);
    }

    // ── Axis 5: Drift alignment ───────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades)
            m[t.drift_aligned ? "DriftAligned" : "DriftCounter"].add(t);
        print_buckets(m, "5. DRIFT ALIGNMENT (ewm_drift confirms direction)", 3);
    }

    // ── Axis 6: RSI bucket at entry ───────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            double r = t.entry_rsi;
            std::string k;
            if      (t.is_long) {
                if      (r<20) k="LONG_RSI<20";
                else if (r<25) k="LONG_RSI_20-25";
                else           k="LONG_RSI_25-30";
            } else {
                if      (r>80) k="SHORT_RSI>80";
                else if (r>75) k="SHORT_RSI_75-80";
                else           k="SHORT_RSI_70-75";
            }
            m[k].add(t);
        }
        print_buckets(m, "6. RSI AT ENTRY", 3);
    }

    // ── Axis 7: BB Overshoot ──────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            std::string k;
            double ov = t.bb_overshoot;
            if      (ov < 0.05) k="Barely_outside";
            else if (ov < 0.15) k="Mild_0.05-0.15";
            else if (ov < 0.30) k="Moderate_0.15-0.30";
            else                k="Extreme_>0.30";
            m[k].add(t);
        }
        print_buckets(m, "7. BB OVERSHOOT (how far outside band)", 3);
    }

    // ── Axis 8: BB Width ──────────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            std::string k;
            double w = t.bb_width;
            if      (w < 1.0) k="Tight_<1.0";
            else if (w < 2.0) k="Medium_1-2";
            else if (w < 3.5) k="Wide_2-3.5";
            else              k="VeryWide_>3.5";
            m[k].add(t);
        }
        print_buckets(m, "8. BB WIDTH AT ENTRY", 3);
    }

    // ── Axis 9: RR at entry ───────────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            std::string k;
            double rr = t.rr_at_entry;
            if      (rr < 0.8)  k="Poor_RR_<0.8";
            else if (rr < 1.2)  k="Fair_RR_0.8-1.2";
            else if (rr < 2.0)  k="Good_RR_1.2-2.0";
            else                k="Great_RR_>2.0";
            m[k].add(t);
        }
        print_buckets(m, "9. RR AT ENTRY (TP_dist / SL_dist)", 3);
    }

    // ── Axis 10: VWAP alignment ───────────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades)
            m[t.vwap_aligned ? "VWAPAligned" : "VWAPCounter"].add(t);
        print_buckets(m, "10. VWAP ALIGNMENT (short=above VWAP, long=below VWAP)", 3);
    }

    // ── Axis 11: RSI slope direction ─────────────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            bool slope_confirms = (t.is_long && t.rsi_slope < -2.0) ||
                                  (!t.is_long && t.rsi_slope > 2.0);
            bool slope_neutral  = std::fabs(t.rsi_slope) <= 2.0;
            std::string k = slope_confirms ? "SlopeConfirms" :
                            slope_neutral  ? "SlopeNeutral"  : "SlopeAgainst";
            m[k].add(t);
        }
        print_buckets(m, "11. RSI SLOPE (confirms = reversal momentum present)", 3);
    }

    // ── Axis 12: Combined LONG + drift-aligned + session ─────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            std::string dir = t.is_long ? "L" : "S";
            std::string da  = t.drift_aligned ? "DA" : "DC";
            std::string sess= t.session_name.substr(0,2);
            m[dir+"|"+da+"|"+sess].add(t);
        }
        print_buckets(m, "12. COMBINED: Direction|Drift|Session", 3);
    }

    // ── Axis 13: MFE before SL -- trades that ALMOST made it ─────────────────
    {
        printf("\n── 13. SL TRADES: MFE before hitting stop ──────────────────────────────\n");
        printf("(How far did price move our way before stopping out?)\n");
        std::vector<double> mfes;
        for (auto& t:trades)
            if (t.reason=="SL") mfes.push_back(t.mfe);
        if (!mfes.empty()) {
            std::sort(mfes.begin(), mfes.end());
            int n=(int)mfes.size();
            double sum=0; for(auto x:mfes) sum+=x;
            printf("SL count: %d  MFE p10=%.2f p25=%.2f p50=%.2f p75=%.2f p90=%.2f avg=%.2f\n",
                   n, mfes[n*10/100], mfes[n*25/100], mfes[n/2],
                   mfes[n*75/100], mfes[n*90/100], sum/n);
            printf("(p50=%.2f means half of SL trades moved %.2f in our direction first)\n",
                   mfes[n/2], mfes[n/2]);
        }
    }

    // ── Axis 14: Hour + Direction combined ───────────────────────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            char buf[16];
            snprintf(buf,16,"H%02d_%s",t.hour, t.is_long?"L":"S");
            m[buf].add(t);
        }
        print_buckets(m, "14. HOUR + DIRECTION combined", 3);
    }

    // ── Axis 15: Extreme RSI + drift aligned (H1+H2 combined) ────────────────
    {
        std::map<std::string,Bucket> m;
        for (auto& t:trades) {
            bool extreme_rsi = (t.is_long  && t.entry_rsi < 25.0) ||
                               (!t.is_long && t.entry_rsi > 75.0);
            std::string k = (extreme_rsi && t.drift_aligned) ? "ExtremeRSI+DriftOK" :
                            (extreme_rsi)                    ? "ExtremeRSI_NoDrift" :
                            (t.drift_aligned)                ? "NormalRSI+DriftOK"  :
                                                               "NormalRSI_NoDrift";
            m[k].add(t);
        }
        print_buckets(m, "15. EXTREME RSI x DRIFT ALIGNMENT (key filter combo)", 3);
    }

    // ── Trade-level CSV for offline analysis ─────────────────────────────────
    printf("\n── TRADE LOG (last 30) ──────────────────────────────────────────────────\n");
    printf("%-5s %-6s %-5s %-6s %-6s %-6s %-5s %-4s %-4s %-4s %-6s %-6s %-6s %s\n",
           "#","Dir","Hour","PnL","MFE","MAE","RSI","DA","VA","RR","Over","BBW","Drift","Reason");
    int start = std::max(0, (int)trades.size()-30);
    for (int i=start; i<(int)trades.size(); ++i) {
        auto& t = trades[i];
        printf("%-5d %-6s %-5d %-6.2f %-6.2f %-6.2f %-5.1f %-4s %-4s %-4.2f %-6.2f %-6.2f %-6.3f %s\n",
               i+1, t.is_long?"LONG":"SHORT", t.hour,
               t.pnl, t.mfe, t.mae, t.entry_rsi,
               t.drift_aligned?"Y":"N",
               t.vwap_aligned?"Y":"N",
               t.rr_at_entry,
               t.bb_overshoot,
               t.bb_width,
               t.entry_drift,
               t.reason.c_str());
    }

    printf("\n═══════════════════════════════════════════════════════════════\n");
    printf("FILTER RECOMMENDATIONS (check after reviewing axes above):\n");
    printf("  - If LONG WR >> SHORT WR: add is_long filter or short kill\n");
    printf("  - If DriftAligned WR > 50%%: add drift gate\n");
    printf("  - If specific hours bleed: add to hour_kill list\n");
    printf("  - If ExtremeRSI+DriftOK WR > 50%%: tighten RSI threshold\n");
    printf("  - If Extreme overshoot WR > moderate: raise bb_overshoot_min\n");
    printf("  - If VWAPAligned WR > VWAPCounter: add VWAP alignment gate\n");
    printf("═══════════════════════════════════════════════════════════════\n");

    return 0;
}
