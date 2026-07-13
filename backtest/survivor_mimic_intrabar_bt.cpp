// =============================================================================
// survivor_mimic_intrabar_bt.cpp -- INTRABAR RE-CHECK of the two survivor-cell
// mimic books wired SHADOW in S-2026-07-14h (commit 280f59e7):
//   XAU_4h_DonchN20  be1.0 / arm0.25 / lc2 / cap30 / gb0.10 / rt 5bp / pend6
//   USTEC_4h_ZMR     be0.15 / arm1.0 / lc2 / cap20 / gb0.08 / rt 3bp / pend6
//
// The S-2026-07-14e validation (clip_path_survivor.cpp + mimic_ladder_overlay
// --cfg) was CLOSE-GRADE: BE-entry needed the H4 CLOSE to clear +be, exits
// booked AT the H4 close where the trigger was detected. The DEPLOYED engine
// (GoldTrendMimicLadder.hpp on_h1_bar) is NOT close-grade: it walks each native
// H4 bar [adverse extreme -> favorable extreme -> close], BE-enters at the BE
// LEVEL when the bar's favorable EXTREME touches it, and books trail/lc exits
// AT the stop LEVEL (resting-stop convention). This harness quantifies the gap
// and resolves the true intra-H4 path on fine (H1 / M1) bars with GAP-AWARE
// worse-of(level, bar-open) fills = executable truth (registry section 5
// model-fill lesson).
//
// Modes per book (each also at 2x cost):
//   CLOSE-M   close-grade, window truncated at parent exit  (parity target =
//             the study figures: XAU +14.6%/PF2.05/n60, USTEC +27.2%/PF3.61/n51)
//   LIVE-M    deployed h/l/c 3-pt walk, level fills, matched window
//   LIVE-F    deployed h/l/c 3-pt walk, level fills, FULL window (the engine
//             never reads the parent; legs run to trail/lc/cap)
//   FINE-M    true path on fine bars, gap-aware fills, matched window
//   FINE-F    true path on fine bars, gap-aware fills, full window
//
// Parent trades: IDENTICAL to clip_path_survivor.cpp (REAL survivor::Portfolio,
// live S-2026-07-08c config, dedup_mode=1, RSI_N7 culled, asym bear long-veto,
// NDX-daily-seeded). Parity n=445 asserted.
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include \
//          backtest/survivor_mimic_intrabar_bt.cpp -o /tmp/surv_mimic_intrabar
// run:   /tmp/surv_mimic_intrabar <xau_h4> <ustec_h4> <ndx_daily> \
//          <xau_fine|-> <nsx_fine|-> [FINE_LABEL]
// =============================================================================
#include "SurvivorPortfolio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <cmath>

struct Tick { long long ts_ms; std::string sym; double px; };
struct Bar4 { long long ts_sec; double o,h,l,c; };

static int load_bars(const std::string& path, const std::string& sym,
                     std::vector<Tick>* out, std::vector<Bar4>& bars) {
    std::ifstream f(path);
    if (!f) { std::printf("[load] CANNOT OPEN %s\n", path.c_str()); return 0; }
    std::string line; int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || (line[0] < '0' || line[0] > '9')) continue;
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        long long ts = std::atoll(tok[0].c_str());
        long long ts_ms = ts > 100000000000LL ? ts : ts * 1000LL;
        double o = std::atof(tok[1].c_str()), h = std::atof(tok[2].c_str());
        double l = std::atof(tok[3].c_str()), c = std::atof(tok[4].c_str());
        if (h <= 0) continue;
        if (out) {
            out->push_back({ ts_ms + 0,   sym, o });
            out->push_back({ ts_ms + 250, sym, h });
            out->push_back({ ts_ms + 500, sym, l });
            out->push_back({ ts_ms + 750, sym, c });
        }
        bars.push_back({ ts_ms / 1000, o, h, l, c });
        ++n;
    }
    std::printf("[load] %-8s %7d bars  %s\n", sym.c_str(), n, path.c_str());
    return n;
}

// asym sustained-bear regime proxy (identical to survivor_gated_bt / clip_path_survivor)
struct BearState {
    std::deque<double> closes;
    double prev_sma = 0; int k_bear = 0; bool bear = false;
    long long cur_day = -1; double day_close = 0;
    void push_daily(double c) {
        closes.push_back(c);
        if (closes.size() > 210) closes.pop_front();
        if (closes.size() < 200) { prev_sma = 0; return; }
        double s = 0; for (size_t i = closes.size() - 200; i < closes.size(); ++i) s += closes[i];
        double sma = s / 200.0;
        if (prev_sma > 0 && c < sma && sma < prev_sma) k_bear++; else k_bear = 0;
        bear = (k_bear >= 20);
        prev_sma = sma;
    }
    void on_tick(long long ts_ms, double px) {
        long long day = (ts_ms / 1000) / 86400;
        if (cur_day < 0) { cur_day = day; day_close = px; return; }
        if (day != cur_day) { push_daily(day_close); cur_day = day; }
        day_close = px;
    }
};

struct PT { std::string cell, sym; long long entryTs, exitTs; int dir; double entry_px; };

static std::vector<double> sma200v(const std::vector<Bar4>& B) {
    int N = (int)B.size(); std::vector<double> s(N, 0.0); double sum = 0;
    for (int i = 0; i < N; i++) {
        sum += B[i].c; if (i >= 200) sum -= B[i-200].c;
        s[i] = i >= 199 ? sum / 200.0 : B[i].c;
    }
    return s;
}
static int idx_at(const std::vector<Bar4>& B, long long ts_sec) {
    int lo = 0, hi = (int)B.size() - 1, r = 0;
    while (lo <= hi) { int m = (lo + hi) / 2; if (B[m].ts_sec <= ts_sec) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}
static int fine_lb(const std::vector<Bar4>& F, long long ts_sec) { // first idx with ts >= ts_sec
    int lo = 0, hi = (int)F.size();
    while (lo < hi) { int m = (lo + hi) / 2; if (F[m].ts_sec < ts_sec) lo = m + 1; else hi = m; }
    return lo;
}

// ── leg evaluation ───────────────────────────────────────────────────────────
struct BookCfg { std::string tag, sym; double be, arm, lc, gb; int cap, pend; double rt_bp; };
struct LegOut { bool booked=false; long long xms=0; double pnl=0; int reason=0; double mae=0; };
// reason: 0=cancelled 1=LC 2=TRAIL 3=CAP 4=MARKOUT(window end)

// MODE CLOSE (exact mimic_ladder_overlay.cpp --cfg semantics, incl. its per-trade
// study cost). Window ei_trig..xi on H4 closes. seq0 (trigger bar) skipped.
static LegOut eval_close(const PT& t, const std::vector<Bar4>& B, const BookCfg& k,
                         int ei, int xi, double cost) {
    LegOut o;
    int d = t.dir; double trig = t.entry_px;
    double be = k.be/100.0, arm = k.arm/100.0, lc = k.lc/100.0, gb = k.gb;
    // BE-entry on closes
    double E = 0; int open_seq = -1;
    for (int i = ei; i <= xi; ++i) {
        int seq = i - ei; if (seq == 0) continue;
        double fav = d * (B[i].c / trig - 1.0);
        if (fav >= be) { E = B[i].c; open_seq = seq; break; }
        if (seq >= k.pend) break;
    }
    if (open_seq < 0) return o; // cancelled
    double peak = 0; bool armed = false;
    for (int i = ei; i <= xi; ++i) {
        int seq = i - ei; if (seq <= open_seq) continue;
        int rseq = seq - open_seq;
        double fav = d * (B[i].c / E - 1.0);
        if (fav > peak) peak = fav;
        if (fav - 0.0 < o.mae) o.mae = fav;
        if (!armed) {
            if (peak >= arm) armed = true;
            else if (fav <= -lc) { o = {true, B[i].ts_sec*1000, fav - cost, 1, o.mae}; return o; }
        }
        if (armed && fav <= peak * (1.0 - gb)) { o = {true, B[i].ts_sec*1000, fav - cost, 2, o.mae}; return o; }
        if (rseq >= k.cap) { o = {true, B[i].ts_sec*1000, fav - cost, 3, o.mae}; return o; }
    }
    double fav = d * (B[xi].c / E - 1.0);
    o = {true, B[xi].ts_sec*1000, fav - cost, 4, o.mae};
    return o;
}

// MODE LIVE (deployed GoldTrendMimicBook on H4 h/l/c: 3-pt adverse-first walk,
// LEVEL fills, BE-enter at level on the bar EXTREME, manage from next bar).
// matched=true stops feeding at xi (mark-out at B[xi].c); else runs to cap/tape end.
static LegOut eval_live(const PT& t, const std::vector<Bar4>& B, const BookCfg& k,
                        int ei, int xi, double cost, bool matched) {
    LegOut o;
    int d = t.dir; double trig = t.entry_px;
    double be = k.be/100.0, arm = k.arm/100.0, lc = k.lc/100.0, gb = k.gb;
    int N = (int)B.size();
    int last_feed = matched ? xi : N - 1;
    // pending: bars ei+1 .. (pbars 1..pend), entry AT be level on the bar extreme
    double E = 0; int kbar = -1;
    for (int i = ei + 1; i <= last_feed && i < N; ++i) {
        int pbars = i - ei;
        double fx = d > 0 ? B[i].h : B[i].l;
        double fret = d * (fx / trig - 1.0);
        if (fret >= be) { E = trig * (1.0 + d * be); kbar = i; break; }
        if (pbars >= k.pend) return o; // cancel
    }
    if (kbar < 0) return o; // window/tape ended pending -> cancel
    double peak = 0; bool armed = false;
    for (int i = kbar + 1; i < N; ++i) {
        if (matched && i > xi) break;
        int bars = i - kbar;
        double adv = d > 0 ? B[i].l : B[i].h;
        double fx  = d > 0 ? B[i].h : B[i].l;
        double seq3[3] = { adv, fx, B[i].c };
        for (int p = 0; p < 3; ++p) {
            double ret = d * (seq3[p] / E - 1.0);
            if (ret < o.mae) o.mae = ret;
            if (ret > peak) peak = ret;
            if (!armed) {
                if (ret <= -lc) { o = {true, B[i].ts_sec*1000, -lc - cost, 1, o.mae}; return o; }
                if (peak >= arm) armed = true;
            } else {
                double stop = (1.0 - gb) * peak;
                if (ret <= stop) { o = {true, B[i].ts_sec*1000, stop - cost, 2, o.mae}; return o; }
            }
        }
        if (bars >= k.cap) {
            double ret = d * (B[i].c / E - 1.0);
            o = {true, B[i].ts_sec*1000, ret - cost, 3, o.mae}; return o;
        }
    }
    int endi = std::min(matched ? xi : N - 1, N - 1);
    double ret = d * (B[endi].c / E - 1.0);
    o = {true, B[endi].ts_sec*1000, ret - cost, 4, o.mae};
    return o;
}

// MODE LIVE-MKT (deployed engine detection + MARKET fills): identical h/l/c
// 3-pt walk detection to eval_live, but every fill happens at the CLOSE of the
// detecting bar (the engine only evaluates at the H4 close event and the wired
// exec path sends market orders -> fill ~= close, not the level the shadow
// books). This is the honest real-fill model of the CURRENT wiring at flip.
static LegOut eval_live_mkt(const PT& t, const std::vector<Bar4>& B, const BookCfg& k,
                            int ei, int xi, double cost, bool matched) {
    LegOut o;
    int d = t.dir; double trig = t.entry_px;
    double be = k.be/100.0, arm = k.arm/100.0, lc = k.lc/100.0, gb = k.gb;
    int N = (int)B.size();
    int last_feed = matched ? xi : N - 1;
    double E = 0; int kbar = -1;
    for (int i = ei + 1; i <= last_feed && i < N; ++i) {
        int pbars = i - ei;
        double fx = d > 0 ? B[i].h : B[i].l;
        double fret = d * (fx / trig - 1.0);
        if (fret >= be) { E = B[i].c; kbar = i; break; }   // MARKET fill at detection close
        if (pbars >= k.pend) return o;
    }
    if (kbar < 0) return o;
    double peak = 0; bool armed = false;
    for (int i = kbar + 1; i < N; ++i) {
        if (matched && i > xi) break;
        int bars = i - kbar;
        double adv = d > 0 ? B[i].l : B[i].h;
        double fx  = d > 0 ? B[i].h : B[i].l;
        double cret = d * (B[i].c / E - 1.0);
        double seq3[3] = { adv, fx, B[i].c };
        for (int p = 0; p < 3; ++p) {
            double ret = d * (seq3[p] / E - 1.0);
            if (ret < o.mae) o.mae = ret;
            if (ret > peak) peak = ret;
            if (!armed) {
                if (ret <= -lc) { o = {true, B[i].ts_sec*1000, cret - cost, 1, o.mae}; return o; }
                if (peak >= arm) armed = true;
            } else {
                double stop = (1.0 - gb) * peak;
                if (ret <= stop) { o = {true, B[i].ts_sec*1000, cret - cost, 2, o.mae}; return o; }
            }
        }
        if (bars >= k.cap) { o = {true, B[i].ts_sec*1000, cret - cost, 3, o.mae}; return o; }
    }
    int endi = std::min(matched ? xi : N - 1, N - 1);
    double ret = d * (B[endi].c / E - 1.0);
    o = {true, B[endi].ts_sec*1000, ret - cost, 4, o.mae};
    return o;
}

// MODE FINE (true path on fine bars, GAP-AWARE fills: worse-of(level, bar open)).
// Pend/cap boundaries counted in H4 tape bars; cap flush at B[k+cap].c (live
// cadence); mark-out (matched) at B[xi].c.
static LegOut eval_fine(const PT& t, const std::vector<Bar4>& B, const std::vector<Bar4>& F,
                        const BookCfg& k, int ei, int xi, double cost, bool matched) {
    LegOut o;
    int d = t.dir; double trig = t.entry_px;
    double be = k.be/100.0, arm = k.arm/100.0, lc = k.lc/100.0, gb = k.gb;
    int N = (int)B.size(), NF = (int)F.size();
    long long t_open = B[std::min(ei + 1, N - 1)].ts_sec; // entry-bar open time
    int fi = fine_lb(F, t_open);
    // ── pending: fine bars whose H4 idx is in [ei+1, ei+pend]; enter at be level
    //    (gap-through: fill at fine-bar OPEN when it opens beyond the level)
    double E = 0; int kbar = -1; long long e_ms = 0; int f_ent = -1;
    double lvl = trig * (1.0 + d * be);
    for (int f = fi; f < NF; ++f) {
        int j = idx_at(B, F[f].ts_sec);
        if (j <= ei) continue;
        if (j - ei > k.pend) return o;                       // pend exhausted -> cancel
        if (matched && j > xi) return o;                     // parent window ended -> cancel
        double ro = d * (F[f].o / trig - 1.0);
        double fx = d > 0 ? F[f].h : F[f].l;
        double rf = d * (fx / trig - 1.0);
        if (ro >= be)      { E = F[f].o; kbar = j; f_ent = f; e_ms = F[f].ts_sec*1000; break; } // gap fill at open
        else if (rf >= be) { E = lvl;    kbar = j; f_ent = f; e_ms = F[f].ts_sec*1000; break; } // limit at level
    }
    if (kbar < 0) return o;
    (void)e_ms;
    // ── manage from the NEXT fine bar
    double peak = 0; bool armed = false;
    for (int f = f_ent + 1; f < NF; ++f) {
        int j = idx_at(B, F[f].ts_sec);
        if (matched && j > xi) break;                        // mark-out below
        if (j - kbar > k.cap) {                              // cap flush happened at B[kbar+cap].c
            int ci = std::min(kbar + k.cap, N - 1);
            double ret = d * (B[ci].c / E - 1.0);
            o = {true, B[ci].ts_sec*1000, ret - cost, 3, o.mae}; return o;
        }
        // 4-pt walk: open (gap fills), adverse (level fills), favorable, close (level)
        double adv = d > 0 ? F[f].l : F[f].h;
        double fx  = d > 0 ? F[f].h : F[f].l;
        double pts[4]  = { F[f].o, adv, fx, F[f].c };
        for (int p = 0; p < 4; ++p) {
            double ret = d * (pts[p] / E - 1.0);
            if (ret < o.mae) o.mae = ret;
            if (!armed) {
                if (ret <= -lc) {
                    double fill_ret = (p == 0) ? ret : -lc;  // gap at open, else resting level
                    o = {true, F[f].ts_sec*1000, fill_ret - cost, 1, std::min(o.mae, fill_ret)}; return o;
                }
                if (ret > peak) peak = ret;
                if (peak >= arm) armed = true;
            } else {
                double stop = (1.0 - gb) * peak;
                if (ret <= stop) {
                    double fill_ret = (p == 0 && ret < stop) ? ret : stop;
                    o = {true, F[f].ts_sec*1000, fill_ret - cost, 2, o.mae}; return o;
                }
                if (ret > peak) peak = ret;
            }
        }
    }
    // window / data end while open
    int endi = std::min(matched ? xi : N - 1, N - 1);
    double ret = d * (B[endi].c / E - 1.0);
    o = {true, B[endi].ts_sec*1000, ret - cost, 4, o.mae};
    return o;
}

// MODE REST (deployed H4 cadence + RESTING orders): entry = resting stop-entry
// at the be level (intrabar touch fills, gap = worse-of open); trail/lc stops
// REST at levels but the level only UPDATES at H4 closes (peak folds in the
// just-closed H4 bar's extreme, exactly the live walk cadence); intrabar touch
// of the resting level fills AT the level (gap = worse-of open); cap flush =
// market at the H4 close. This is what the CURRENT engine would earn if the
// LIVE flip used resting orders instead of market-at-close.
static LegOut eval_rest(const PT& t, const std::vector<Bar4>& B, const std::vector<Bar4>& F,
                        const BookCfg& k, int ei, int xi, double cost, bool matched) {
    LegOut o;
    int d = t.dir; double trig = t.entry_px;
    double be = k.be/100.0, arm = k.arm/100.0, lc = k.lc/100.0, gb = k.gb;
    int N = (int)B.size(), NF = (int)F.size();
    long long t_open = B[std::min(ei + 1, N - 1)].ts_sec;
    int fi = fine_lb(F, t_open);
    double E = 0; int kbar = -1; int f_ent = -1;
    double lvl = trig * (1.0 + d * be);
    for (int f = fi; f < NF; ++f) {
        int j = idx_at(B, F[f].ts_sec);
        if (j <= ei) continue;
        if (j - ei > k.pend) return o;
        if (matched && j > xi) return o;
        double ro = d * (F[f].o / trig - 1.0);
        double fx = d > 0 ? F[f].h : F[f].l;
        double rf = d * (fx / trig - 1.0);
        if (ro >= be)      { E = F[f].o; kbar = j; f_ent = f; break; }
        else if (rf >= be) { E = lvl;    kbar = j; f_ent = f; break; }
    }
    if (kbar < 0) return o;
    double peak = 0, period_peak = 0; bool armed = false;
    double stop_ret = -lc;              // resting: pre-arm lc, post-arm trail
    int jprev = kbar;
    for (int f = f_ent + 1; f < NF; ++f) {
        int j = idx_at(B, F[f].ts_sec);
        if (matched && j > xi) break;
        if (j != jprev) {               // H4 close event(s): fold period extremes, update stops
            if (period_peak > peak) peak = period_peak;
            if (!armed && peak >= arm) armed = true;
            if (armed) { double s = (1.0 - gb) * peak; if (s > stop_ret) stop_ret = s; }
            if (j - kbar > k.cap) {           // cap close passed -> flushed at B[kbar+cap].c
                int ci = std::min(kbar + k.cap, N - 1);
                double ret = d * (B[ci].c / E - 1.0);
                o = {true, B[ci].ts_sec*1000, ret - cost, 3, o.mae}; return o;
            }
            period_peak = 0; jprev = j;
        }
        double ro  = d * (F[f].o / E - 1.0);
        double adv = d > 0 ? F[f].l : F[f].h;
        double ra  = d * (adv / E - 1.0);
        double fx  = d > 0 ? F[f].h : F[f].l;
        double rf  = d * (fx / E - 1.0);
        if (ra < o.mae) o.mae = ra;
        if (ro <= stop_ret) { o = {true, F[f].ts_sec*1000, ro - cost, armed?2:1, std::min(o.mae,ro)}; return o; }
        if (ra <= stop_ret) { o = {true, F[f].ts_sec*1000, stop_ret - cost, armed?2:1, o.mae}; return o; }
        if (rf > period_peak) period_peak = rf;
    }
    int endi = std::min(matched ? xi : N - 1, N - 1);
    double ret = d * (B[endi].c / E - 1.0);
    o = {true, B[endi].ts_sec*1000, ret - cost, 4, o.mae};
    return o;
}

// ── metrics (identical conventions to mimic_ladder_overlay.cpp) ─────────────
struct Metrics { int n=0; double pf=0, net=0, dd=0, worst=0; };
static Metrics metrics(std::vector<std::pair<long long,double>> rows) {
    Metrics m;
    if (rows.empty()) return m;
    std::sort(rows.begin(), rows.end());
    double w=0,l=0,eq=0,pk=0,dd=0,sum=0,worst=0;
    for (auto& r : rows) { double p = r.second;
        if (p>0) w+=p; else l+=-p;
        if (p<worst) worst=p;
        sum+=p; eq+=p; if (eq>pk) pk=eq; if (eq-pk<dd) dd=eq-pk; }
    m.n=(int)rows.size(); m.pf = l>0 ? w/l : (w>0 ? 1e9 : 0);
    m.net = 100.0*sum; m.dd = 100.0*dd; m.worst = 100.0*worst;
    return m;
}

struct ModeRow {
    std::string name; int n=0, cx=0, r_lc=0, r_tr=0, r_cap=0, r_mo=0;
    Metrics all,h1,h2,bull,bear; bool ok=false;
    Metrics x2; bool ok2=false; double mae_worst=0;
};

int main(int argc, char** argv) {
    if (argc < 6) {
        std::printf("usage: %s xau_h4 ustec_h4 ndx_daily xau_fine|- nsx_fine|- [FINE_LABEL]\n", argv[0]);
        return 2;
    }
    std::string fine_label = argc > 6 ? argv[6] : "H1";
    std::vector<Tick> ticks;
    std::map<std::string, std::vector<Bar4>> tape, fine;
    load_bars(argv[1], "XAUUSD",  &ticks, tape["XAUUSD"]);
    load_bars(argv[2], "USTEC.F", &ticks, tape["USTEC.F"]);
    if (std::strcmp(argv[4], "-")) load_bars(argv[4], "XAUUSD",  nullptr, fine["XAUUSD"]);
    if (std::strcmp(argv[5], "-")) load_bars(argv[5], "USTEC.F", nullptr, fine["USTEC.F"]);
    std::stable_sort(ticks.begin(), ticks.end(),
        [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });

    long long t0_s = ticks.front().ts_ms / 1000;
    std::map<std::string, BearState> reg;
    { std::ifstream f(argv[3]); std::string ln; int nseed = 0;
      while (std::getline(f, ln)) {
          long long ts; double o,h,l,c;
          if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) == 5 && c > 0 && ts < t0_s) {
              reg["USTEC.F"].push_daily(c); ++nseed;
          }
      }
      std::printf("[seed] USTEC daily closes pre-tape: %d\n", nseed);
    }

    omega::survivor::Portfolio p;
    p.init_default_cells();
    p.dedup_mode = 1;
    for (auto& c : p.cells)
        if (std::strcmp(c.cfg.tag, "USTEC_4h_RSI_N7") == 0) c.st.enabled = false;
    p.entry_veto = [&reg](const char* sym, int side) -> bool {
        if (side <= 0) return false;
        auto it = reg.find(sym);
        return it != reg.end() && it->second.bear;
    };
    std::vector<PT> trades;
    auto cb = [&](const omega::TradeRecord& tr) {
        trades.push_back({ tr.engine, tr.symbol, tr.entryTs, tr.exitTs,
                           tr.side == "LONG" ? +1 : -1, tr.entryPrice });
    };
    for (const auto& tk : ticks) {
        reg[tk.sym].on_tick(tk.ts_ms, tk.px);
        p.on_tick(tk.sym, tk.px, tk.px, tk.ts_ms, cb);
    }
    std::printf("[run] closed trades total=%zu (parity target 445)\n", trades.size());

    std::map<std::string, std::vector<double>> smm;
    for (auto& kv : tape) smm[kv.first] = sma200v(kv.second);

    // the two DEPLOYED books (engine_init.hpp S-2026-07-14h)
    std::vector<BookCfg> books = {
        { "XAU_4h_DonchN20", "XAUUSD",  1.00, 0.25, 2.0, 0.10, 30, 6, 5.0 },
        { "USTEC_4h_ZMR",    "USTEC.F", 0.15, 1.00, 2.0, 0.08, 20, 6, 3.0 },
    };

    for (const auto& bk : books) {
        const auto& B = tape[bk.sym];
        const auto& S = smm[bk.sym];
        bool have_fine = fine.count(bk.sym) && !fine[bk.sym].empty();
        const std::vector<Bar4>* F = have_fine ? &fine[bk.sym] : nullptr;

        // fine coverage per H4 bar (for the M1 holes)
        std::vector<int> fcnt;
        if (F) {
            fcnt.assign(B.size(), 0);
            for (const auto& fb : *F) {
                int j = idx_at(B, fb.ts_sec);
                if (j >= 0 && j < (int)fcnt.size()) fcnt[j]++;
            }
        }

        struct TT { PT t; int ei, xi; bool bull, covered; };
        std::vector<TT> tl;
        for (const auto& t : trades) {
            if (t.cell != bk.tag) continue;
            int ei = idx_at(B, t.entryTs), xi = idx_at(B, t.exitTs);
            if (xi < ei) xi = ei;
            bool bull = B[ei].c > S[ei];
            bool cov = true;
            if (F) {
                int wend = std::min(ei + bk.pend + bk.cap + 1, (int)B.size() - 1);
                for (int i = ei + 1; i <= wend; ++i)
                    if (fcnt[i] == 0) { cov = false; break; }
            }
            tl.push_back({t, ei, xi, bull, cov});
        }
        int ncov = 0; for (auto& x : tl) if (x.covered) ncov++;
        std::printf("\n================================================================================\n");
        std::printf("BOOK %s  (%zu parent trades, fine=%s covered=%d/%zu)  cfg be%.2f arm%.2f lc%.1f cap%d gb%.2f rt%.0fbp pend%d\n",
                    bk.tag.c_str(), tl.size(), F ? fine_label.c_str() : "none", ncov, tl.size(),
                    bk.be, bk.arm, bk.lc, bk.cap, bk.gb, bk.rt_bp, bk.pend);

        auto run_mode = [&](const std::string& name, int mode, bool matched, bool cov_only,
                            double cost_mult) -> ModeRow {
            ModeRow r; r.name = name;
            std::vector<std::pair<long long,double>> all, bull, bear;
            for (const auto& x : tl) {
                if (cov_only && !x.covered) continue;
                double cost;
                if (mode == 0) // study per-trade cost formula (parity)
                    cost = (bk.sym == "XAUUSD") ? 2.0*0.00015 + 0.30/x.t.entry_px
                                                : 2.0*0.00005 + 2.00/x.t.entry_px;
                else cost = bk.rt_bp / 1e4;
                cost *= cost_mult;
                LegOut o;
                if      (mode == 0) o = eval_close   (x.t, B, bk, x.ei, x.xi, cost);
                else if (mode == 1) o = eval_live    (x.t, B, bk, x.ei, x.xi, cost, matched);
                else if (mode == 3) o = eval_live_mkt(x.t, B, bk, x.ei, x.xi, cost, matched);
                else if (mode == 4) o = eval_rest    (x.t, B, *F, bk, x.ei, x.xi, cost, matched);
                else                o = eval_fine    (x.t, B, *F, bk, x.ei, x.xi, cost, matched);
                if (!o.booked) { r.cx++; continue; }
                r.n++;
                if (o.reason == 1) r.r_lc++; else if (o.reason == 2) r.r_tr++;
                else if (o.reason == 3) r.r_cap++; else r.r_mo++;
                if (o.mae < r.mae_worst) r.mae_worst = o.mae;
                all.push_back({o.xms, o.pnl});
                if (x.bull) bull.push_back({o.xms, o.pnl}); else bear.push_back({o.xms, o.pnl});
            }
            std::sort(all.begin(), all.end());
            int mid = (int)all.size()/2;
            std::vector<std::pair<long long,double>> h1(all.begin(), all.begin()+mid),
                                                     h2(all.begin()+mid, all.end());
            r.all = metrics(all); r.h1 = metrics(h1); r.h2 = metrics(h2);
            r.bull = metrics(bull); r.bear = metrics(bear);
            r.ok = r.all.net>0 && r.all.pf>=1.3 && r.h1.net>0 && r.h2.net>0
                && r.bull.net>0 && r.bear.net>0;
            return r;
        };

        auto pr = [&](const ModeRow& r, const ModeRow& r2) {
            std::printf("  %-9s n=%3d cx=%3d | net=%+8.1f%% PF=%5.2f DD=%6.1f%% worst=%+6.2f%% "
                        "H1=%+7.1f H2=%+7.1f bull=%+7.1f(n%d) bear=%+7.1f(n%d) | lc/tr/cap/mo=%d/%d/%d/%d maeW=%+.2f%% %s\n",
                        r.name.c_str(), r.n, r.cx, r.all.net, r.all.pf, r.all.dd, r.all.worst,
                        r.h1.net, r.h2.net, r.bull.net, r.bull.n, r.bear.net, r.bear.n,
                        r.r_lc, r.r_tr, r.r_cap, r.r_mo, 100.0*r.mae_worst,
                        r.ok ? "PASS-all6" : "FAIL");
            std::printf("  %-9s 2x-cost: net=%+8.1f%% PF=%5.2f  %s\n", "",
                        r2.all.net, r2.all.pf, (r.ok && r2.all.net>0 && r2.all.pf>0) ?
                        (r2.all.net>0 ? "2x PASS" : "2x FAIL") : (r2.all.net>0?"2x net+":"2x FAIL"));
        };

        std::printf("  -- ALL parent trades --\n");
        pr(run_mode("CLOSE-M", 0, true,  false, 1.0), run_mode("CLOSE-M", 0, true,  false, 2.0));
        pr(run_mode("LIVE-M",  1, true,  false, 1.0), run_mode("LIVE-M",  1, true,  false, 2.0));
        pr(run_mode("LIVE-F",  1, false, false, 1.0), run_mode("LIVE-F",  1, false, false, 2.0));
        pr(run_mode("MKT-M",   3, true,  false, 1.0), run_mode("MKT-M",   3, true,  false, 2.0));
        pr(run_mode("MKT-F",   3, false, false, 1.0), run_mode("MKT-F",   3, false, false, 2.0));
        if (F) {
            bool sub = ncov < (int)tl.size();
            if (sub) {
                std::printf("  -- COVERED subset (n=%d parents; fine=%s holes excluded; all modes on same subset) --\n",
                            ncov, fine_label.c_str());
                pr(run_mode("CLOSE-M", 0, true,  true, 1.0), run_mode("CLOSE-M", 0, true,  true, 2.0));
                pr(run_mode("LIVE-M",  1, true,  true, 1.0), run_mode("LIVE-M",  1, true,  true, 2.0));
                pr(run_mode("LIVE-F",  1, false, true, 1.0), run_mode("LIVE-F",  1, false, true, 2.0));
            }
            pr(run_mode(("FINE-M/"+fine_label).c_str(), 2, true,  true, 1.0),
               run_mode("FINE-M", 2, true,  true, 2.0));
            pr(run_mode(("FINE-F/"+fine_label).c_str(), 2, false, true, 1.0),
               run_mode("FINE-F", 2, false, true, 2.0));
            pr(run_mode(("REST-M/"+fine_label).c_str(), 4, true,  true, 1.0),
               run_mode("REST-M", 4, true,  true, 2.0));
            pr(run_mode(("REST-F/"+fine_label).c_str(), 4, false, true, 1.0),
               run_mode("REST-F", 4, false, true, 2.0));
            // diagnostic gb/arm probe at fine grade (is a FAIL parameter-local or
            // mechanism-deep?). Not a re-tune -- printed for the findings doc only.
            if (std::getenv("PROBE")) {
                std::printf("  -- FINE-F gb/arm probe (diagnostic) --\n");
                BookCfg save = bk;
                for (double gbp : {0.08, 0.10, 0.15, 0.20, 0.30, 0.50})
                    for (double armp : {save.arm, save.arm*2.0}) {
                        const_cast<BookCfg&>(bk).gb = gbp;
                        const_cast<BookCfg&>(bk).arm = armp;
                        char nm[48]; std::snprintf(nm, sizeof nm, "gb%.2f/a%.2f", gbp, armp);
                        pr(run_mode(nm, 2, false, true, 1.0), run_mode(nm, 2, false, true, 2.0));
                    }
                const_cast<BookCfg&>(bk) = save;
            }
        }
    }
    return 0;
}
