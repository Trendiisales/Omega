// =============================================================================
// iceberg_detect_mgc.cpp -- offline iceberg / hidden-liquidity detector for
// IBKR MGC recordings (Tier: research, 2026-06-12).
//
// METHOD (per research survey, Christensen & Woodmansey 2013 adapted to
// conflated MBP + Zotikov n>=2 confirmation + Bookmap-style absorption):
// our feed is ~250ms-conflated 5-level MBP snapshots + tick-by-tick trade
// prints on SEPARATE unsynchronized streams. CME native iceberg refresh
// latency (~76ms mean) is sub-snapshot, so dip-and-refill is invisible;
// the detectable signature is CONSERVATION VIOLATION per bucket:
//
//     traded volume at touch price P  >  observed decrease in displayed size at P
//
// Accumulated per (side, price) while P remains the touch:
//   refresh_count = #buckets where prints exceeded display decrease (+eps)
//   SIGNAL when refresh_count >= 2 (n>=2 FP rule)
//          AND cum_traded >= K * max_displayed_seen   (default K=1.5)
//          AND level alive >= MIN_BUCKETS snapshots
//   KILLED when price trades through (display->0 WITH prints)
//   PULLED when display->0 with NO prints (cancel kills hidden remainder)
//
// IMPACT MEASUREMENT: for each signal, forward mid move at +1m/+5m/+15m,
// signed so that "+ = price moved AWAY from the iceberg side" (bid iceberg
// => bounce up = +). Baseline = same stats over random non-signal buckets.
//
// INPUTS:
//   --levels ibkr_l2levels_MGC_YYYY-MM-DD.csv   (ts_ms,b1p,b1s..b5p,b5s,a1p..a5s)
//   --trades ibkr_trades_MGC_YYYY-MM-DD.csv     (ts_ms,price,size,exch,spec)
//   repeatable; pass multiple day pairs.
// Usage:
//   iceberg_detect_mgc --levels f1 [--levels f2 ...] --trades g1 [...]
//                      [--k 1.5] [--minbuckets 3] [--eps 0.5] [--csv out.csv]
//
// Build: clang++ -O3 -std=c++17 backtest/iceberg_detect_mgc.cpp -o backtest/iceberg_detect_mgc
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <fstream>

struct Snap {
    int64_t ts = 0;
    double bp[5]{}, bs[5]{}, ap[5]{}, as[5]{};
};
struct Print { int64_t ts = 0; double px = 0, sz = 0; };

static std::vector<Snap> load_levels(const std::vector<std::string>& paths) {
    std::vector<Snap> out;
    for (const auto& p : paths) {
        std::ifstream f(p); if (!f) { std::fprintf(stderr, "open fail %s\n", p.c_str()); continue; }
        std::string ln; std::getline(f, ln);
        while (std::getline(f, ln)) {
            Snap s; const char* c = ln.c_str(); char* e;
            s.ts = std::strtoll(c, &e, 10); if (*e != ',') continue; c = e + 1;
            bool ok = true;
            for (int i = 0; i < 5 && ok; ++i) {
                s.bp[i] = std::strtod(c, &e); if (*e != ',') { ok = false; break; } c = e + 1;
                s.bs[i] = std::strtod(c, &e); if (*e != ',') { ok = false; break; } c = e + 1;
            }
            for (int i = 0; i < 5 && ok; ++i) {
                s.ap[i] = std::strtod(c, &e); if (*e != ',' && !(i == 4 && (*e == 0 || *e == '\r'))) { ok = false; break; }
                c = e + 1;
                s.as[i] = std::strtod(c, &e);
                if (i < 4) { if (*e != ',') { ok = false; break; } c = e + 1; }
            }
            if (ok && s.bp[0] > 0 && s.ap[0] > 0) out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end(), [](const Snap& a, const Snap& b){ return a.ts < b.ts; });
    return out;
}

static std::vector<Print> load_trades(const std::vector<std::string>& paths) {
    std::vector<Print> out;
    for (const auto& p : paths) {
        std::ifstream f(p); if (!f) { std::fprintf(stderr, "open fail %s\n", p.c_str()); continue; }
        std::string ln; std::getline(f, ln);
        while (std::getline(f, ln)) {
            Print t; const char* c = ln.c_str(); char* e;
            t.ts = std::strtoll(c, &e, 10); if (*e != ',') continue; c = e + 1;
            t.px = std::strtod(c, &e); if (*e != ',') continue; c = e + 1;
            t.sz = std::strtod(c, &e);
            if (t.px > 0 && t.sz > 0) out.push_back(t);
        }
    }
    std::sort(out.begin(), out.end(), [](const Print& a, const Print& b){ return a.ts < b.ts; });
    return out;
}

struct LevelTrack {
    bool   active = false;
    double price = 0;
    double disp_prev = 0;        // displayed size at last snapshot
    double max_disp = 0;
    double cum_traded = 0;
    int    refresh_count = 0;
    int    buckets_alive = 0;
    int64_t born_ts = 0;
    void reset() { *this = LevelTrack{}; }
};

struct Signal {
    int64_t ts; int side; double price;
    double cum_traded, max_disp; int refreshes;
    double fwd1m = 0, fwd5m = 0, fwd15m = 0;  // signed: + = away from iceberg
};

int main(int argc, char** argv) {
    std::vector<std::string> lv, tr;
    double K = 1.5, EPS = 0.5; int MINB = 3;
    const char* out_csv = nullptr;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--levels" && i + 1 < argc) lv.push_back(argv[++i]);
        else if (a == "--trades" && i + 1 < argc) tr.push_back(argv[++i]);
        else if (a == "--k" && i + 1 < argc) K = std::atof(argv[++i]);
        else if (a == "--eps" && i + 1 < argc) EPS = std::atof(argv[++i]);
        else if (a == "--minbuckets" && i + 1 < argc) MINB = std::atoi(argv[++i]);
        else if (a == "--csv" && i + 1 < argc) out_csv = argv[++i];
    }
    if (lv.empty() || tr.empty()) {
        std::fprintf(stderr, "usage: %s --levels f.csv [...] --trades g.csv [...] [--k 1.5] [--eps 0.5] [--minbuckets 3] [--csv out]\n", argv[0]);
        return 1;
    }
    auto snaps  = load_levels(lv);
    auto prints = load_trades(tr);
    std::fprintf(stderr, "loaded %zu snaps, %zu prints\n", snaps.size(), prints.size());
    if (snaps.size() < 100) { std::fprintf(stderr, "too few snaps\n"); return 1; }

    LevelTrack bid_t, ask_t;
    std::vector<Signal> sigs;
    size_t pi = 0;

    auto mid_at = [&](int64_t ts) -> double {
        // binary search nearest snap at/after ts
        size_t lo = 0, hi = snaps.size();
        while (lo < hi) { size_t m = (lo + hi) / 2; if (snaps[m].ts < ts) lo = m + 1; else hi = m; }
        if (lo >= snaps.size()) lo = snaps.size() - 1;
        return (snaps[lo].bp[0] + snaps[lo].ap[0]) * 0.5;
    };

    for (size_t i = 1; i < snaps.size(); ++i) {
        const Snap& prev = snaps[i - 1];
        const Snap& cur  = snaps[i];
        // prints in (prev.ts, cur.ts]  (bucketed accounting; streams unsynced,
        // bucket tolerance is inherent: a print belongs to the bucket it lands in)
        double traded_at_bid = 0, traded_at_ask = 0;
        while (pi < prints.size() && prints[pi].ts <= cur.ts) {
            const Print& t = prints[pi];
            if (t.ts > prev.ts) {
                // classify by proximity to prev touch (quote rule on conflated data)
                const double db = std::fabs(t.px - prev.bp[0]);
                const double da = std::fabs(t.px - prev.ap[0]);
                if (db < da)      traded_at_bid += t.sz;
                else if (da < db) traded_at_ask += t.sz;
                // tie -> drop (ambiguous inside-spread print)
            }
            ++pi;
        }

        auto step = [&](LevelTrack& T, int side, double px_prev, double sz_prev,
                        double px_cur, double sz_cur, double traded) {
            if (!T.active || T.price != px_prev) {
                // touch changed: did the old level die?
                if (T.active) {
                    // trades through vs pulled -- recorded implicitly; reset.
                    T.reset();
                }
                T.active = true; T.price = px_prev; T.disp_prev = sz_prev;
                T.max_disp = sz_prev; T.born_ts = prev.ts;
            }
            if (px_cur == T.price) {
                ++T.buckets_alive;
                const double decrease = std::max(0.0, T.disp_prev - sz_cur);
                if (traded > decrease + EPS) ++T.refresh_count;   // conservation violation
                T.cum_traded += traded;
                T.max_disp = std::max(T.max_disp, sz_cur);
                T.disp_prev = sz_cur;
                if (T.refresh_count >= 2 && T.buckets_alive >= MINB
                    && T.cum_traded >= K * T.max_disp && T.max_disp > 0) {
                    sigs.push_back({cur.ts, side, T.price, T.cum_traded,
                                    T.max_disp, T.refresh_count});
                    T.reset();   // one signal per episode; re-arm fresh
                }
            }
            // px_cur != price handled on next call (touch-changed branch)
        };
        step(bid_t, +1, prev.bp[0], prev.bs[0], cur.bp[0], cur.bs[0], traded_at_bid);
        step(ask_t, -1, prev.ap[0], prev.as[0], cur.ap[0], cur.as[0], traded_at_ask);
    }

    // Forward impact: + = price moved away from the iceberg (bounce).
    for (auto& s : sigs) {
        const double m0 = mid_at(s.ts);
        s.fwd1m  = (mid_at(s.ts + 60'000)   - m0) * s.side;
        s.fwd5m  = (mid_at(s.ts + 300'000)  - m0) * s.side;
        s.fwd15m = (mid_at(s.ts + 900'000)  - m0) * s.side;
    }

    // Baseline: every Nth snapshot as pseudo-signal on both sides.
    double b1 = 0, b5 = 0, b15 = 0; int bn = 0;
    for (size_t i = 100; i + 4000 < snaps.size(); i += 997) {
        const double m0 = (snaps[i].bp[0] + snaps[i].ap[0]) * 0.5;
        b1  += std::fabs(mid_at(snaps[i].ts + 60'000)  - m0);
        b5  += std::fabs(mid_at(snaps[i].ts + 300'000) - m0);
        b15 += std::fabs(mid_at(snaps[i].ts + 900'000) - m0);
        ++bn;
    }

    std::printf("ICEBERG DETECT  k=%.2f eps=%.2f minbuckets=%d\n", K, EPS, MINB);
    std::printf("signals=%zu\n", sigs.size());
    if (!sigs.empty()) {
        double f1 = 0, f5 = 0, f15 = 0; int pos5 = 0;
        for (auto& s : sigs) { f1 += s.fwd1m; f5 += s.fwd5m; f15 += s.fwd15m; if (s.fwd5m > 0) ++pos5; }
        std::printf("mean fwd (signed, + = bounce off iceberg):\n");
        std::printf("  +1m %+0.3f   +5m %+0.3f (winrate %0.1f%%)   +15m %+0.3f\n",
                    f1 / sigs.size(), f5 / sigs.size(), 100.0 * pos5 / sigs.size(),
                    f15 / sigs.size());
    }
    if (bn) std::printf("baseline |move|: +1m %.3f  +5m %.3f  +15m %.3f  (n=%d)\n",
                        b1 / bn, b5 / bn, b15 / bn, bn);
    if (out_csv) {
        std::ofstream o(out_csv);
        o << "ts_ms,side,price,cum_traded,max_disp,refreshes,fwd1m,fwd5m,fwd15m\n";
        for (auto& s : sigs)
            o << s.ts << ',' << s.side << ',' << s.price << ',' << s.cum_traded << ','
              << s.max_disp << ',' << s.refreshes << ',' << s.fwd1m << ',' << s.fwd5m << ',' << s.fwd15m << "\n";
        std::printf("events -> %s\n", out_csv);
    }
    return 0;
}
