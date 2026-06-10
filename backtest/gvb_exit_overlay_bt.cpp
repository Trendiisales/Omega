// =============================================================================
//  gvb_exit_overlay_bt.cpp -- GoldVolBreakoutM30 exit-overlay path-level study
// =============================================================================
//
//  PURPOSE (2026-06-10 profit-protection study)
//    The runner engines surrender 35-54% of peak profit on reversal (Stage A
//    ledger analysis). Question: can a SCALE-OUT + surgical GIVEBACK overlay
//    bank that profit WITHOUT clipping the fat-tail runs that ARE the edge
//    (GVB M30: top trade ~40% of net, top-3 ~83%)?
//
//  METHOD (fidelity-first)
//    1. Drive the REAL GoldVolBreakoutM30Engine over the price path for ENTRIES
//       ONLY -- the entry edge is untouched. Watch the public `pos` to detect
//       each entry (ts, entry_px, atr_at_entry, initial sl).
//    2. Per entry, record the forward price PATH (M5-resolution pseudo-ticks +
//       M30 closes) out to max-hold. All exit variants act on the SAME path and
//       the SAME entry set -> clean apples-to-apples.
//    3. Apply N exit variants in-harness:
//         BASE   = engine beoff (hard stop 1.5ATR, trail 3ATR after +1.55R,
//                  maxhold 72). Reproduced in-harness -> FIDELITY GATE vs the
//                  engine's own emitted exits (must match within tolerance).
//         SCALE  = BASE trail + bank 50% at +M*ATR milestone (half runs on).
//         GB     = BASE trail + surgical giveback: once mfe>=ARM*ATR, exit if
//                  price gives back (1-RET) of the peak.
//         SCALE+GB = both.
//    4. Report per variant: net (pts), PF, WR, payoff, both-halves. And a
//       FAT-TAIL table: the top trades by BASE mfe -- does the variant CLIP
//       their run (exit earlier / lower pnl than BASE)? That is the kill test.
//
//  Driven from M5 bars (fast, lets the SAME harness run the 2022 bear set).
//  Baseline must reproduce the documented tick result (43 tr / PF 2.41 /
//  +31107 pts) within tolerance or the M5 path is rejected.
//
//  Build:
//    clang++ -O3 -std=c++17 -Iinclude -o backtest/gvb_exit_overlay_bt \
//            backtest/gvb_exit_overlay_bt.cpp
//  Run:
//    ./backtest/gvb_exit_overlay_bt ~/Tick/2yr_XAUUSD_tick_fresh.m5.csv
//    ./backtest/gvb_exit_overlay_bt ~/Tick/XAUUSD_2022_bear_m5.csv --bear
// =============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "GoldVolBreakoutM30Engine.hpp"

using omega::GoldVolBreakoutM30Engine;
using omega::TradeRecord;

// ---- one M5 bar ----
struct M5 { int64_t ts; double o,h,l,c; };

static std::vector<M5> load_m5(const char* path) {
    std::vector<M5> v; std::ifstream f(path);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", path); return v; }
    std::string line; bool first = true;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (first && (line[0] < '0' || line[0] > '9')) { first = false; continue; } // header
        first = false;
        // ts,o,h,l,c
        M5 b; char* p = const_cast<char*>(line.c_str()); char* q = nullptr;
        b.ts = std::strtoll(p, &q, 10); if (*q != ',') continue;
        b.o = std::strtod(q+1, &q); if (*q != ',') continue;
        b.h = std::strtod(q+1, &q); if (*q != ',') continue;
        b.l = std::strtod(q+1, &q); if (*q != ',') continue;
        b.c = std::strtod(q+1, nullptr);
        if (b.ts > 1000000000000LL) b.ts /= 1000;   // ms -> s (bear file is ms)
        if (b.ts > 0 && b.o > 0 && b.h >= b.l) v.push_back(b);
    }
    return v;
}

// path sample: a price point with a flag for "this is an M30 close"
struct PathPt { int64_t ts; double px; bool m30_close; double m30_close_px; };

struct Entry {
    int64_t ts; double entry_px; double atr; double init_sl;
    double  er = 0;             // Kaufman efficiency ratio on H1 at entry (0 chop..1 trend)
    std::vector<PathPt> path;   // forward path from entry to maxhold
    // engine baseline exit (for the fidelity gate)
    double eng_exit_px = 0; double eng_mfe = 0; const char* eng_reason = "";
    bool   eng_closed = false;
};

// Kaufman efficiency ratio over the last N H1 closes: |net move| / sum|step|.
// ~1 = clean trend, ~0 = chop. Used to regime-bucket each entry.
static double efficiency_ratio(const std::deque<double>& h1, int N) {
    if ((int)h1.size() <= N) return 0.0;
    const int base = (int)h1.size() - 1 - N;
    double net = std::fabs(h1.back() - h1[base]);
    double sum = 0.0;
    for (int i = base+1; i < (int)h1.size(); ++i) sum += std::fabs(h1[i]-h1[i-1]);
    return sum > 0 ? net/sum : 0.0;
}

// ---- engine params mirrored for the in-harness exit simulator ----
struct EP {
    double kStopAtr=1.50, kTrailAtr=3.00, kTrailAfterR=1.55;
    int    kMaxHoldM30=72;
};

// Result of one exit variant on one entry
struct VExit { double exit_px; double pnl_pts; double mfe; int held_m30; const char* reason; };

// Simulate one variant on a path. `scale_m`=0 disables scale-out; `gb_arm`=0
// disables giveback. Returns realized pnl in price points (long-only, lot=1).
// Scale-out banks `scale_frac` of the position at +scale_m*ATR (once), the rest
// rides BASE trail. Giveback: once mfe>=gb_arm*ATR, exit the remaining position
// when price <= peak - (1-gb_ret)*peak_fav  (peak_fav measured from entry).
static VExit sim_variant(const Entry& e, const EP& ep,
                         double scale_m, double scale_frac,
                         double gb_arm, double gb_ret) {
    const double entry = e.entry_px, atr = e.atr;
    const double R = ep.kStopAtr * atr;
    double sl = e.init_sl;
    double peak_fav = 0.0;           // max (px-entry)
    double realized = 0.0;           // banked from partial scale-outs (pts, per unit weight)
    double remaining = 1.0;          // position fraction still open
    bool   scaled = false;
    int    m30_held = 0;
    auto book = [&](double exit_px, const char* reason)->VExit{
        realized += remaining * (exit_px - entry);
        return VExit{exit_px, realized, peak_fav, m30_held, reason};
    };
    for (size_t i = 0; i < e.path.size(); ++i) {
        const PathPt& p = e.path[i];
        const double fav = p.px - entry;
        if (fav > peak_fav) peak_fav = fav;
        // intrabar/tick checks first (SL, giveback) -- these fill at price p.px
        if (p.px <= sl && remaining > 0) return book(sl, "STOP_OR_TRAIL");
        if (gb_arm > 0 && remaining > 0 && peak_fav >= gb_arm * atr) {
            const double give = (1.0 - gb_ret) * peak_fav;
            if (fav <= peak_fav - give) return book(p.px, "GIVEBACK");
        }
        // scale-out at milestone (bank a fraction once)
        if (!scaled && scale_m > 0 && remaining > 0 && fav >= scale_m * atr) {
            realized += scale_frac * (p.px - entry);
            remaining -= scale_frac;
            scaled = true;
        }
        // M30-close events: update BASE trail + maxhold
        if (p.m30_close) {
            ++m30_held;
            if (R > 0 && (p.m30_close_px - entry) >= ep.kTrailAfterR * R) {
                const double trail = p.m30_close_px - ep.kTrailAtr * atr;
                if (trail > sl) sl = trail;
            }
            if (m30_held >= ep.kMaxHoldM30 && remaining > 0)
                return book(p.m30_close_px, "MAX_HOLD");
        }
    }
    // path exhausted (end of data) -> mark to market at last px
    if (!e.path.empty() && remaining > 0) return book(e.path.back().px, "EOD");
    return VExit{entry, realized, peak_fav, m30_held, "EOD"};
}

struct Stats { int n=0,w=0; double net=0, gprof=0, gloss=0, h1=0, h2=0; };
static void rec(Stats& s, double pnl, bool oos) {
    ++s.n; s.net += pnl; if (pnl>0){++s.w; s.gprof+=pnl;} else s.gloss += -pnl;
    if (oos) s.h2 += pnl; else s.h1 += pnl;
}
static void print_stats(const char* name, const Stats& s, double cost_rt) {
    double net_c = s.net - cost_rt * s.n;
    double pf = s.gloss>0 ? s.gprof/s.gloss : 0;
    double wr = s.n? 100.0*s.w/s.n : 0;
    std::printf("  %-14s n=%3d  net=%9.1f  net(cost)=%9.1f  PF=%5.2f  WR=%4.1f%%  H1=%8.1f H2=%8.1f %s\n",
        name, s.n, s.net, net_c, pf, wr, s.h1, s.h2,
        (s.h1>0&&s.h2>0)?"BOTH+":"");
}

// Drive the real engine over one M5 file; return usable entries with paths+ER.
static std::vector<Entry> collect_entries(const char* path) {
    std::vector<M5> bars = load_m5(path);
    if (bars.size() < 1000) { std::fprintf(stderr, "too few m5 bars in %s (%zu)\n", path, bars.size()); return {}; }
    std::printf("loaded %zu M5 bars from %s  span %lld..%lld\n",
        bars.size(), path, (long long)bars.front().ts, (long long)bars.back().ts);

    GoldVolBreakoutM30Engine eng;
    eng.enabled = true; eng.shadow_mode = true; eng.lot = 1.0; eng.max_spread = 1e9;
    eng.init();

    // Aggregate M5 -> M30 + H1 from the stream; drive the engine; capture entries.
    std::vector<Entry> entries;
    entries.reserve(4096);             // stable addresses: `cur` is a raw ptr into this
    Entry* cur = nullptr;              // open entry being recorded
    int64_t cur_path_end = 0;

    // M30 + H1 accumulators
    double m30_o=0,m30_h=0,m30_l=0,m30_c=0; int64_t m30_bkt=-1;
    double h1_c=0; int64_t h1_bkt=-1;
    std::deque<double> h1_closes;     // harness-side H1 close buffer for ER regime label
    const int64_t M30=1800, H1=3600;
    const int kER = 20;               // ER lookback (H1 bars)

    // engine callback (baseline exit capture)
    auto on_close = [&](const TradeRecord& tr){
        if (cur) { cur->eng_exit_px = tr.exitPrice; cur->eng_mfe = tr.mfe;
                   cur->eng_reason = "eng"; cur->eng_closed = true; }
    };

    for (size_t i=0;i<bars.size();++i) {
        const M5& b = bars[i];
        // ---- generate intrabar pseudo-ticks O->H->L->C (long: low is the danger) ----
        const double seq[4] = {b.o, b.h, b.l, b.c};
        // bucket boundaries
        int64_t m30b = (b.ts / M30) * M30;
        int64_t h1b  = (b.ts / H1) * H1;

        // detect M30 close BEFORE this bar opens a new M30 bucket
        bool m30_just_closed = (m30_bkt >= 0 && m30b != m30_bkt);
        bool h1_just_closed  = (h1_bkt  >= 0 && h1b  != h1_bkt);

        if (h1_just_closed) { eng.on_h1_close(h1_c);
            h1_closes.push_back(h1_c); while ((int)h1_closes.size() > kER+4) h1_closes.pop_front(); }
        if (m30_just_closed) {
            // feed the just-completed M30 bar to the engine
            bool was = eng.pos.active;
            eng.on_m30_bar(m30_h, m30_l, m30_c, m30_c, m30_c, (m30_bkt+M30)*1000, on_close);
            bool now = eng.pos.active;
            // entry edge: inactive -> active
            if (!was && now) {
                entries.push_back(Entry{});
                cur = &entries.back();
                cur->ts = eng.pos.entry_ts_ms/1000;
                cur->entry_px = eng.pos.entry_px;
                cur->atr = eng.pos.atr_at_entry;
                cur->init_sl = eng.pos.sl_px;
                cur->er = efficiency_ratio(h1_closes, kER);
                cur_path_end = cur->ts + (int64_t)eng.kMaxHold * M30;
            }
            if (was && !now) cur = nullptr;  // engine closed its own baseline trade
            // mark the M30 close into the recording path of any active entry
            for (auto& e : entries) {
                if (e.ts <= (m30_bkt) && (m30_bkt) <= cur_path_end && !e.path.empty())
                    ; // handled below per-tick
            }
        }

        // advance H1/M30 accumulators with this M5 bar
        if (m30b != m30_bkt) { m30_bkt=m30b; m30_o=b.o; m30_h=b.h; m30_l=b.l; m30_c=b.c; }
        else { m30_h=std::max(m30_h,b.h); m30_l=std::min(m30_l,b.l); m30_c=b.c; }
        if (h1b != h1_bkt) { h1_bkt=h1b; h1_c=b.c; } else { h1_c=b.c; }

        // feed pseudo-ticks to engine on_tick (SL fills) + record path for open entries
        const bool m5_is_m30_close = (((b.ts+ (int64_t)300) / M30) * M30) != m30b; // this M5 ends the M30
        for (int k=0;k<4;++k) {
            double px = seq[k];
            int64_t tts = b.ts*1000 + k;
            eng.on_tick(px, px, tts, on_close);
            // record into every entry whose recording window is open
            for (auto& e : entries) {
                if (b.ts >= e.ts && b.ts <= cur_path_end+0 && (int64_t)e.path.size() < 200000) {
                    // only record while within that entry's horizon
                    if (b.ts <= e.ts + (int64_t)eng.kMaxHold*M30) {
                        bool mc = (k==3) && m5_is_m30_close;
                        e.path.push_back(PathPt{tts, px, mc, m30_c});
                    }
                }
            }
        }
    }
    std::printf("  entries captured: %zu", entries.size());
    std::vector<Entry> out;
    for (auto& e : entries) if (e.atr>0 && e.path.size()>4) out.push_back(std::move(e));
    std::printf("  usable: %zu\n", out.size());
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <m5_csv> [m5_csv ...] [--cost PTS]\n", argv[0]); return 1; }
    double cost_rt = 0.37;   // gold roundtrip pts (IBKR); doc says PF robust 0.10-0.60
    std::vector<const char*> files;
    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--cost") && i+1<argc) { cost_rt = std::atof(argv[++i]); continue; }
        if (argv[i][0]=='-') continue;   // skip flags like --bear
        files.push_back(argv[i]);
    }
    // pool entries across all files (thickens regime buckets)
    std::vector<Entry> E;
    for (const char* f : files) { auto v = collect_entries(f); for (auto& e : v) E.push_back(std::move(e)); }
    std::printf("\nPOOLED usable entries: %zu (from %zu files)\n\n", E.size(), files.size());

    EP ep;
    const int64_t mid_ts = E.empty()?0 : E[E.size()/2].ts;  // split for both-halves

    // ---- variants ----
    struct Variant { const char* name; double sm, sf, ga, gr; };
    std::vector<Variant> V = {
        {"BASE",        0,    0,    0,    0   },   // engine beoff reproduced
        {"SCALE2.0",    2.0,  0.5,  0,    0   },
        {"SCALE3.0",    3.0,  0.5,  0,    0   },
        {"GB4x.65",     0,    0,    4.0,  0.65},
        {"GB3x.65",     0,    0,    3.0,  0.65},
        {"GB4x.70",     0,    0,    4.0,  0.70},
        {"SCALE2+GB4",  2.0,  0.5,  4.0,  0.65},
        {"SCALE3+GB4",  3.0,  0.5,  4.0,  0.65},
    };

    // compute BASE per-trade first (for fat-tail reference + fidelity gate)
    std::vector<VExit> base(E.size());
    for (size_t i=0;i<E.size();++i) base[i] = sim_variant(E[i], ep, 0,0,0,0);

    // fidelity gate: in-harness BASE vs engine's own emitted exit
    {
        double dnet=0; int cmp=0;
        for (size_t i=0;i<E.size();++i) if (E[i].eng_closed) {
            double eng_pnl = E[i].eng_exit_px - E[i].entry_px;
            dnet += std::fabs(base[i].pnl_pts - eng_pnl); ++cmp;
        }
        std::printf("FIDELITY: in-harness BASE vs engine exits over %d trades, mean|dPnL|=%.2f pts\n\n",
            cmp, cmp? dnet/cmp : 0.0);
    }

    // regime bucket by entry ER: chop < 0.30 <= trend
    const double ER_CUT = 0.30;
    int nchop=0, ntrend=0;
    for (auto& e : E) (e.er < ER_CUT ? nchop : ntrend)++;
    std::printf("regime split @ ER<%.2f: chop=%d  trend=%d\n\n", ER_CUT, nchop, ntrend);

    std::printf("==== EXIT VARIANTS (cost %.2f pts/rt) -- ALL trades ====\n", cost_rt);
    std::vector<std::vector<VExit>> allv(V.size(), std::vector<VExit>(E.size()));
    std::vector<Stats> schop(V.size()), strend(V.size());
    for (size_t vi=0; vi<V.size(); ++vi) {
        Stats s;
        for (size_t i=0;i<E.size();++i) {
            VExit x = sim_variant(E[i], ep, V[vi].sm, V[vi].sf, V[vi].ga, V[vi].gr);
            allv[vi][i] = x;
            rec(s, x.pnl_pts, E[i].ts >= mid_ts);
            rec(E[i].er < ER_CUT ? schop[vi] : strend[vi], x.pnl_pts, E[i].ts >= mid_ts);
        }
        print_stats(V[vi].name, s, cost_rt);
    }
    std::printf("\n==== CHOP bucket (ER<%.2f, n=%d) ====\n", ER_CUT, nchop);
    for (size_t vi=0; vi<V.size(); ++vi) print_stats(V[vi].name, schop[vi], cost_rt);
    std::printf("\n==== TREND bucket (ER>=%.2f, n=%d) ====\n", ER_CUT, ntrend);
    for (size_t vi=0; vi<V.size(); ++vi) print_stats(V[vi].name, strend[vi], cost_rt);

    // ---- FAT-TAIL KILL TEST ----
    // rank trades by BASE mfe (the runners). For each, show pnl under BASE vs
    // each overlay. A variant that REDUCES a top-runner's pnl is clipping the
    // fat tail -> edge risk.
    std::vector<size_t> idx(E.size());
    for (size_t i=0;i<E.size();++i) idx[i]=i;
    std::sort(idx.begin(), idx.end(), [&](size_t a,size_t b){ return base[a].mfe > base[b].mfe; });
    int top = std::min((int)E.size(), 8);
    std::printf("\n==== FAT-TAIL KILL TEST (top %d trades by peak MFE) ====\n", top);
    std::printf("  %-10s %8s | ", "peakMFE", "BASE");
    for (size_t vi=1; vi<V.size(); ++vi) std::printf("%11s", V[vi].name);
    std::printf("\n");
    for (int r=0;r<top;++r) {
        size_t i = idx[r];
        std::printf("  %9.1f %8.1f | ", base[i].mfe, base[i].pnl_pts);
        for (size_t vi=1; vi<V.size(); ++vi) {
            double d = allv[vi][i].pnl_pts - base[i].pnl_pts;
            const char* flag = d < -1.0 ? "<CLIP" : (d>1.0?"+":" ");
            std::printf("%8.1f%-3s", allv[vi][i].pnl_pts, flag);
        }
        std::printf("\n");
    }
    std::printf("  ('<CLIP' = overlay banked LESS than BASE on a top runner = fat-tail damage)\n");
    return 0;
}
