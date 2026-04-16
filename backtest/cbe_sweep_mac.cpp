// cbe_sweep_mac.cpp -- CBE parameter sweep, self-contained, no Omega headers needed
// Build: clang++ -O3 -std=c++20 -o /tmp/cbe_sweep cbe_sweep_mac.cpp
// Run:   /tmp/cbe_sweep ~/Downloads/l2_ticks_2026-04-09.csv \
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

// ─── Data types ───────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; int slot; };

static int slot_from_ms(int64_t ms) {
    int h = (int)(((ms / 1000LL) % 86400LL) / 3600LL);
    if (h >= 7  && h < 9)  return 1;
    if (h >= 9  && h < 11) return 2;
    if (h >= 11 && h < 13) return 3;
    if (h >= 13 && h < 17) return 4;
    if (h >= 17 && h < 20) return 5;
    return 6; // Asia / dead zone
}

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
        ++ci; }}
    if (cb<0||ca<0) { fprintf(stderr,"No bid/ask in %s\n",path); return false; }
    size_t before = out.size();
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size() >= sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);
        const char* flds[32]; int nf=0;
        flds[nf++] = buf;
        for (char* c=buf; *c && nf<32; ++c)
            if (*c==',') { *c='\0'; flds[nf++]=c+1; }
        int need = std::max({cm,cb,ca,cd});
        if (nf <= need) continue;
        try {
            Tick t;
            t.ms    = cm>=0 ? (int64_t)std::stod(flds[cm]) : 0;
            t.bid   = std::stod(flds[cb]);
            t.ask   = std::stod(flds[ca]);
            t.drift = (cd>=0 && nf>cd) ? std::stod(flds[cd]) : 0.0;
            t.slot  = slot_from_ms(t.ms);
            out.push_back(t);
        } catch (...) {}
    }
    fprintf(stderr, "Loaded %s: %zu ticks\n", path, out.size()-before);
    return true;
}

// ─── Bar tracker ─────────────────────────────────────────────────────────────
struct M1Bar { double hi, lo, cl; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=0, rsi=50;
    std::deque<double> rg, rl; double rp=0;
    M1Bar cur={}; int64_t cbms=0;

    bool update(double bid, double ask, int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        // Tick RSI
        if (rp > 0) {
            double chg=mid-rp;
            rg.push_back(chg>0?chg:0); rl.push_back(chg<0?-chg:0);
            if ((int)rg.size()>14) { rg.pop_front(); rl.pop_front(); }
            if ((int)rg.size()==14) {
                double ag=0,al=0;
                for (auto x:rg) ag+=x; for (auto x:rl) al+=x;
                ag/=14; al/=14;
                rsi = (al==0) ? 100.0 : 100.0-100.0/(1.0+ag/al);
            }
        }
        rp = mid;
        bool nb = false;
        if (cbms==0) { cur={mid,mid,mid}; cbms=bms; }
        else if (bms != cbms) {
            bars.push_back(cur);
            if ((int)bars.size()>50) bars.pop_front();
            if ((int)bars.size()>=2) {
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for (int i=(int)bars.size()-1; i>=(int)bars.size()-n; --i) {
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.hi-b.lo,
                                   std::fabs(b.hi-pb.cl),
                                   std::fabs(b.lo-pb.cl)});
                }
                atr = sum/n;
            }
            cur={mid,mid,mid}; cbms=bms; nb=true;
        } else {
            if (mid>cur.hi) cur.hi=mid;
            if (mid<cur.lo) cur.lo=mid;
            cur.cl=mid;
        }
        return nb;
    }
};

// ─── Engine ──────────────────────────────────────────────────────────────────
struct Eng {
    // Params
    int    comp_bars;
    double comp_mult, break_frac, tp_rr;
    bool   block_long;

    // Position
    struct Pos {
        bool   active=false, is_long=false, be_done=false, trail_armed=false;
        double entry=0, sl=0, tp=0, size=0, mfe=0, trail_sl=0, atr=0;
        double chi=0, clo=0;
        int64_t ets=0;
    } pos;

    // Compression state
    std::deque<double> bhi, blo;
    int    cc=0;
    bool   armed=false;
    double chi=0, clo=0, atr_c=0, rsi_c=50;
    int64_t last_exit=0, startup=0;
    double  dpnl=0; int64_t dday=0;

    using CB = std::function<void(double,bool,const char*)>;

    void on_bar(double hi, double lo, double atr14, double rsi14) {
        atr_c = (atr14 > 0.5) ? atr14 : atr_c;
        rsi_c = rsi14;
        bhi.push_back(hi); blo.push_back(lo);
        if ((int)bhi.size() > comp_bars) { bhi.pop_front(); blo.pop_front(); }
        if ((int)bhi.size() < 2) return;
        double whi = *std::max_element(bhi.begin(), bhi.end());
        double wlo = *std::min_element(blo.begin(), blo.end());
        double rng = whi-wlo, thr = atr_c*comp_mult;
        if (atr_c > 0 && rng < thr) {
            ++cc;
            if (cc >= 3) { armed=true; chi=whi; clo=wlo; }
        } else {
            cc = 0;
            if (!pos.active) armed = false;
        }
    }

    void on_tick(double bid, double ask, int64_t ms, double drift, int slot, CB on_close) {
        if (startup==0) startup=ms;
        if (ms-startup < 90000LL) return;

        int64_t day=(ms/1000LL)/86400LL;
        if (day!=dday) { dpnl=0; dday=day; }
        if (dpnl <= -150.0) return;

        double mid=(bid+ask)*0.5;

        if (pos.active) {
            double mv = pos.is_long ? (mid-pos.entry) : (pos.entry-mid);
            if (mv > pos.mfe) pos.mfe = mv;
            double td = std::fabs(pos.tp-pos.entry);
            // BE at 40%
            if (!pos.be_done && td>0 && pos.mfe >= td*0.40) {
                pos.sl=pos.entry; pos.trail_sl=pos.entry; pos.be_done=true;
            }
            // Trail arms at 50%
            if (!pos.trail_armed && td>0 && pos.mfe >= td*0.50)
                pos.trail_armed=true;
            if (pos.trail_armed) {
                double tdist=pos.atr*0.40;
                double nt=pos.is_long?(mid-tdist):(mid+tdist);
                if (pos.is_long  && nt>pos.trail_sl) pos.trail_sl=nt;
                if (!pos.is_long && nt<pos.trail_sl) pos.trail_sl=nt;
                if (pos.is_long  && pos.trail_sl>pos.sl) pos.sl=pos.trail_sl;
                if (!pos.is_long && pos.trail_sl<pos.sl) pos.sl=pos.trail_sl;
            }
            double ep = pos.is_long ? bid : ask;
            if (pos.is_long ? (bid>=pos.tp) : (ask<=pos.tp)) {
                _close(ep,"TP",ms,on_close); return;
            }
            bool sh = pos.is_long ? (bid<=pos.sl) : (ask>=pos.sl);
            if (sh) {
                const char* r = pos.trail_armed?"TRAIL" : pos.be_done?"BE" : "SL";
                _close(ep,r,ms,on_close); return;
            }
            if (ms-pos.ets > 300000LL) { _close(ep,"TIMEOUT",ms,on_close); return; }
            return;
        }

        // Entry guards
        if (!armed || atr_c<=0) return;
        if (ms-last_exit < 30000LL) return;
        if ((ask-bid) > atr_c*0.30) return;
        if (slot<1 || slot>5) return;

        // Break detection
        double bm = atr_c * break_frac;
        bool lb=(bid >= chi+bm), sb=(ask <= clo-bm);
        if (!lb && !sb) return;
        bool isl = lb;

        // Direction gates
        if (isl  && drift <= 0) return;
        if (!isl && drift >= 0) return;
        if (isl  && rsi_c > 72) return;
        if (!isl && rsi_c < 22) return;
        if (block_long && isl)  return;

        // SL geometry
        double ep=isl?ask:bid, buf=0.50;
        double sld = isl ? (ep-(clo-buf)) : ((chi+buf)-ep);
        if (sld<=0 || sld>atr_c*4.0) return;
        double tpp = sld * tp_rr;
        if (tpp <= (ask-bid)+0.20) return;

        // Size
        double sz = std::max(0.01, std::min(0.10,
            std::floor(30.0/(sld*100.0)/0.001)*0.001));

        pos.active=true; pos.is_long=isl; pos.entry=ep;
        pos.sl=isl?(ep-sld):(ep+sld); pos.tp=isl?(ep+tpp):(ep-tpp);
        pos.size=sz; pos.mfe=0; pos.be_done=false; pos.trail_armed=false;
        pos.trail_sl=pos.sl; pos.atr=atr_c; pos.chi=chi; pos.clo=clo;
        pos.ets=ms;
        armed=false;
    }

    void force_close(double bid, double ask, int64_t ms, CB cb) {
        if (!pos.active) return;
        _close(pos.is_long?bid:ask, "FC", ms, cb);
    }

    void _close(double ep, const char* r, int64_t ms, CB cb) {
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        dpnl+=pnl; last_exit=ms;
        if (cb) cb(pnl, pnl>0, r);
        pos=Pos{};
    }
};

// ─── Run one config against all ticks ────────────────────────────────────────
struct Stats {
    int T=0, W=0;
    double pnl=0, max_dd=0;
    double peak=0, trough=0;
    int tp_hits=0, sl_hits=0, trail_hits=0, be_hits=0, timeout=0;
};

static Stats run(const std::vector<Tick>& ticks,
                 int comp_bars, double comp_mult,
                 double break_frac, double tp_rr, bool block_long)
{
    Eng eng;
    eng.comp_bars=comp_bars; eng.comp_mult=comp_mult;
    eng.break_frac=break_frac; eng.tp_rr=tp_rr;
    eng.block_long=block_long;

    BarTracker bt;
    Stats s;
    double equity=0;

    auto on_close = [&](double pnl, bool win, const char* reason) {
        s.pnl += pnl; ++s.T;
        if (win) ++s.W;
        equity += pnl;
        if (equity > s.peak) s.peak=equity;
        double dd = s.peak - equity;
        if (dd > s.max_dd) s.max_dd=dd;
        std::string r(reason);
        if (r=="TP")    ++s.tp_hits;
        else if (r=="SL") ++s.sl_hits;
        else if (r=="TRAIL") ++s.trail_hits;
        else if (r=="BE")    ++s.be_hits;
        else if (r=="TIMEOUT") ++s.timeout;
    };

    for (auto& t : ticks) {
        bool nb = bt.update(t.bid, t.ask, t.ms);
        if (nb && (int)bt.bars.size()>=2) {
            auto& lb = bt.bars.back();
            eng.on_bar(lb.hi, lb.lo, bt.atr, bt.rsi);
        }
        eng.on_tick(t.bid, t.ask, t.ms, t.drift, t.slot, on_close);
    }
    if (eng.pos.active && !ticks.empty()) {
        auto& lt = ticks.back();
        eng.force_close(lt.bid, lt.ask, lt.ms, on_close);
    }
    return s;
}

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cbe_sweep file1.csv [file2.csv ...]\n");
        return 1;
    }

    std::vector<Tick> ticks;
    ticks.reserve(2000000);
    for (int i=1; i<argc; ++i) load_csv(argv[i], ticks);
    fprintf(stderr, "Total: %zu ticks\n\n", ticks.size());
    if (ticks.empty()) return 1;

    struct Result {
        int cb; double cm,bf,tr; bool bl;
        Stats s;
    };
    std::vector<Result> results;

    for (int cb  : {3,4,5,6})
    for (double cm : {1.5, 2.0, 2.5, 3.0})
    for (double bf : {0.15, 0.20, 0.30, 0.40, 0.50})
    for (double tr : {1.5, 2.0, 2.5, 3.0})
    for (bool bl   : {true, false}) {
        Stats s = run(ticks, cb, cm, bf, tr, bl);
        results.push_back({cb, cm, bf, tr, bl, s});
    }

    // Sort by pnl descending
    std::sort(results.begin(), results.end(),
              [](auto& a, auto& b){ return a.s.pnl > b.s.pnl; });

    // Header
    printf("%-4s %-5s %-5s %-4s %-4s %5s %4s %8s %5s %7s %4s %4s %4s %4s %4s\n",
           "CB","MULT","BRK","RR","LONG",
           "T","W","PNL","WR%","MaxDD",
           "TP","SL","TRL","BE","TMO");
    printf("%s\n", std::string(85,'-').c_str());

    int shown=0;
    for (auto& r : results) {
        if (r.s.T == 0) continue;
        if (shown++ >= 40) break;
        double wr = 100.0*r.s.W/r.s.T;
        printf("%-4d %-5.1f %-5.2f %-4.1f %-4s %5d %4d %8.2f %5.1f %7.2f %4d %4d %4d %4d %4d\n",
               r.cb, r.cm, r.bf, r.tr, r.bl?"N":"Y",
               r.s.T, r.s.W, r.s.pnl, wr, r.s.max_dd,
               r.s.tp_hits, r.s.sl_hits, r.s.trail_hits,
               r.s.be_hits, r.s.timeout);
    }

    // Best config detail
    printf("\n=== BEST CONFIG ===\n");
    for (auto& r : results) {
        if (r.s.T == 0) continue;
        printf("comp_bars=%d comp_mult=%.1f break_frac=%.2f tp_rr=%.1f block_long=%s\n",
               r.cb, r.cm, r.bf, r.tr, r.bl?"true":"false");
        printf("Trades=%d Wins=%d WR=%.1f%% PnL=$%.2f AvgTrade=$%.2f MaxDD=$%.2f\n",
               r.s.T, r.s.W, 100.0*r.s.W/r.s.T, r.s.pnl,
               r.s.T>0?r.s.pnl/r.s.T:0, r.s.max_dd);
        printf("TP=%d SL=%d TRAIL=%d BE=%d TIMEOUT=%d\n",
               r.s.tp_hits, r.s.sl_hits, r.s.trail_hits,
               r.s.be_hits, r.s.timeout);
        break;
    }

    // Show configs with >20 trades and positive PnL
    printf("\n=== POSITIVE + >= 20 TRADES ===\n");
    printf("%-4s %-5s %-5s %-4s %-4s %5s %4s %8s %5s %7s\n",
           "CB","MULT","BRK","RR","LONG","T","W","PNL","WR%","MaxDD");
    for (auto& r : results) {
        if (r.s.T < 20 || r.s.pnl <= 0) continue;
        double wr = 100.0*r.s.W/r.s.T;
        printf("%-4d %-5.1f %-5.2f %-4.1f %-4s %5d %4d %8.2f %5.1f %7.2f\n",
               r.cb, r.cm, r.bf, r.tr, r.bl?"N":"Y",
               r.s.T, r.s.W, r.s.pnl, wr, r.s.max_dd);
    }

    return 0;
}
