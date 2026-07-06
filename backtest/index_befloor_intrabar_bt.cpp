// =============================================================================
// index_befloor_intrabar_bt.cpp — IndexBeFloorCompanion INTRABAR FLOOR EXECUTION sweep
//
// WHY (S-2026-07-07, operator P1): US500Pos_r20/r50/r100/r150/r400 each booked
// -$273 on ONE 2026-07-06 18:00Z window. Mechanism (include/IndexBeFloorCompanion.hpp
// live_step_): pre-progress legs have wm==entry -> stop==entry for EVERY tier, exits
// are evaluated at H1 CLOSE only, and the honest-accounting fill is
// worse-of(floor, close). One -5pt hourly close through the floor booked the FULL
// bar move x5 correlated tiers. The floor is an order target the engine never
// actually rests in the market.
//
// QUESTION: does executing the floor INTRABAR (resting stop at the floor, or at
// floor - buffer_bp catastrophe cap) net MORE real $ than close-only execution,
// or does clipping intrabar dips kill enough dip-then-run winners to cost more
// than the close-throughs it saves? (2026-06-17 finding: tightening usually hurts
// trail engines — must be tested, not assumed.)
//
// FAITHFUL PORT of IndexBeFloorSym::live_step_ (W=2 thr=0.30% be=6bp,
// tiers gb={20,150,400,50,100}bp, min_gb_mult=3, gap-guard contig<=2xW h,
// win_pend ref-anchor at NEXT close, ref=stop after clip re-arm, WINDOW_EXIT flush,
// p_real = dir*(fill-entry) - entry*rt_bp/1e4). Entry/arm stays CLOSE-based in all
// modes — only exit execution differs:
//   mode A  (buf<0)  : status quo — exit iff H1 close breaches stop; fill=worse-of(stop,close)
//   mode buf>=0 bp   : SAME close-based exit PLUS a resting intrabar stop at
//                      stop - entry*buf/1e4 (long; mirrored short). Tick touch -> fill AT
//                      the resting level. buf=0 = pure intrabar floor; buf>0 = catastrophe
//                      cap under the floor (close-logic keeps dip tolerance inside the buffer).
//
// Data: HISTDATA "YYYYMMDD HHMMSSmmm,bid,ask,vol" or duka "ts_ms,ask,bid,..."
// (x1000 auto-descale). Files must be passed in chronological order and pre-certified
// by backtest/data_integrity_gate.py (registry gate #2).
//
// Build: g++ -O2 -std=c++17 -o /tmp/index_befloor_intrabar_bt backtest/index_befloor_intrabar_bt.cpp
// Run:   /tmp/index_befloor_intrabar_bt <rt_cost_bp> <dpp> <tick.csv> [tick2.csv ...]
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

static constexpr int    NT  = 5;
static constexpr double GB[NT] = { 20.0, 150.0, 400.0, 50.0, 100.0 };
static const char* TIER[NT] = { "r20", "r150", "r400", "r50", "r100" };
static constexpr int    W   = 2;
static constexpr double MIN_GB_MULT = 3.0;
// salvage grid (real-fill re-validation): thr x be x exec. Live config = 0.30%/be6.
static constexpr double THRS[] = { 0.003, 0.005, 0.007, 0.010, 0.015 };
static constexpr double BES[]  = { 6.0, 10.0, 20.0 };

struct ClosedTr { int64_t xts; int fi, ti; double pts_real; double pts_model; bool intrabar; };

struct Leg { bool has = false; double entry = 0, wm = 0, ref = 0, stop = 0; };

// One full engine replica (one exit mode).
struct Engine {
    double buf_bp;              // <0 = close-only (mode A)
    double rt_bp;
    double thr = 0.003, be = 6.0;
    bool win[2] = { false, false }, winp[2] = { false, false };
    Leg leg[2][NT];
    std::vector<ClosedTr> closed;
    // per-mode diagnostics
    long close_through = 0;     // mode-A style fills below floor (close breached past stop)

    void book(int64_t xts, int fi, int ti, double entry, double fill, double floor_px, bool up, bool intrabar) {
        const double p_real  = (up ? (fill - entry) : (entry - fill)) - entry * (rt_bp / 1e4);
        const double p_model = up ? (floor_px - entry) : (entry - floor_px);   // engine "pts" column (fill-at-floor, no cost)
        closed.push_back({ xts, fi, ti, p_real, p_model, intrabar });
    }

    // intrabar tick inside the CURRENT (forming) hour: resting stops from last close's levels
    void on_tick(int64_t ts, double bid, double ask) {
        if (buf_bp < 0) return;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            for (int ti = 0; ti < NT; ++ti) {
                Leg& L = leg[fi][ti];
                if (!L.has) continue;
                const double rest = up ? (L.stop - L.entry * (buf_bp / 1e4))
                                       : (L.stop + L.entry * (buf_bp / 1e4));
                const bool touch = up ? (bid <= rest) : (ask >= rest);
                if (touch) {
                    book(ts, fi, ti, L.entry, rest, L.stop, up, true);
                    L.ref = rest; L.has = false; L.wm = 0;
                }
            }
        }
    }

    // one CLOSED H1 bar (mirrors live_step_ exactly; ts/close history passed in)
    void on_close(const std::vector<int64_t>& ts, const std::vector<double>& c) {
        const int N = (int)c.size();
        if (N <= W) return;
        const int64_t t = ts[N - 1];
        const double cur = c[N - 1];
        const double j = c[N - 1] / c[N - 1 - W] - 1.0;
        const bool contig = (ts[N - 1] - ts[N - 1 - W]) <= (int64_t)W * 3600 * 2;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            const bool enter = contig && (up ? (j >= thr) : (j <= -thr));
            const bool exi   = up ? (j <= -thr) : (j >= thr);
            if (winp[fi]) {
                winp[fi] = false; win[fi] = true;
                for (int ti = 0; ti < NT; ++ti) { Leg& L = leg[fi][ti]; L.has = false; L.wm = 0; L.ref = cur; }
            }
            if (!win[fi] && !winp[fi] && enter) winp[fi] = true;
            for (int ti = 0; ti < NT && win[fi]; ++ti) {
                Leg& L = leg[fi][ti];
                const double gb = GB[ti];
                if (gb < MIN_GB_MULT * rt_bp) continue;   // tier viability gate
                if (!L.has) {
                    const bool cond = up ? ((cur / L.ref - 1.0) * 1e4 >= be) : ((1.0 - cur / L.ref) * 1e4 >= be);
                    if (cond) { L.entry = cur; L.wm = cur; L.has = true;
                                L.stop = L.entry; }   // pre-progress stop = entry (BE floor)
                } else {
                    if (up) { if (cur > L.wm) L.wm = cur; L.stop = std::max(L.entry, L.wm * (1.0 - gb / 1e4)); }
                    else    { if (cur < L.wm) L.wm = cur; L.stop = std::min(L.entry, L.wm * (1.0 + gb / 1e4)); }
                    const bool hit = up ? (cur <= L.stop) : (cur >= L.stop);
                    if (hit) {
                        const double fill = up ? std::min(L.stop, cur) : std::max(L.stop, cur);
                        if ((up && fill < L.stop) || (!up && fill > L.stop)) ++close_through;
                        book(t, fi, ti, L.entry, fill, L.stop, up, false);
                        L.ref = L.stop; L.has = false; L.wm = 0;
                    }
                }
            }
            if (win[fi] && exi) {
                for (int ti = 0; ti < NT; ++ti) { Leg& L = leg[fi][ti];
                    if (L.has) { book(t, fi, ti, L.entry, cur, up ? std::max(cur, L.entry) : std::min(cur, L.entry), up, false); L.has = false; L.wm = 0; } }
                win[fi] = false;
            }
        }
    }
};

// ── tick parsing ─────────────────────────────────────────────────────────────
static int64_t civil_epoch(int y, int mo, int d) {   // days since 1970 (Howard Hinnant civil)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: %s <rt_cost_bp> <dpp> [dump=h1.csv] <tick.csv> [...]\n", argv[0]); return 1; }
    const double rt_bp = std::atof(argv[1]);
    const double dpp   = std::atof(argv[2]);
    int argi = 3; const char* dump_path = nullptr;
    if (argc > 4 && std::strncmp(argv[3], "dump=", 5) == 0) { dump_path = argv[3] + 5; argi = 4; }

    // grid: thr x be x exec-mode. Exec: A(close-only, live status quo) + resting-stop buf 10/25 bp.
    const double BUFS[] = { -1.0, 10.0, 25.0 };
    std::vector<Engine> eng;
    for (double t : THRS) for (double b : BES) for (double buf : BUFS) {
        Engine e; e.thr = t; e.be = b; e.buf_bp = buf; e.rt_bp = rt_bp; eng.push_back(e);
    }

    std::vector<int64_t> bts; std::vector<double> bc;   // shared H1 close history
    int64_t cur_hour = -1; double last_mid = 0;
    long nticks = 0;
    int64_t first_ts = 0, last_ts = 0;

    char line[512];
    for (int a = argi; a < argc; ++a) {
        FILE* f = std::fopen(argv[a], "rb");
        if (!f) { std::fprintf(stderr, "MISS %s\n", argv[a]); return 1; }
        bool duka = false, hdr_checked = false;
        double scale = 1.0; bool scale_set = false;   // PER FILE: duka DJ30 months mix x1000 and raw scale
        while (std::fgets(line, sizeof line, f)) {
            if (!hdr_checked) {
                hdr_checked = true;
                if (line[0] < '0' || line[0] > '9') { duka = true; continue; }  // duka header line
                duka = (std::strchr(line, ' ') == nullptr);                     // histdata has "date time" field
            }
            int64_t ts; double p1, p2;
            if (duka) {
                long long tms; double av, bv;
                if (std::sscanf(line, "%lld,%lf,%lf", &tms, &av, &bv) != 3) continue;
                ts = tms / 1000; p1 = bv; p2 = av;                              // bid, ask
            } else {
                long ymd, hmsms; double bv, av;
                if (std::sscanf(line, "%ld %ld,%lf,%lf", &ymd, &hmsms, &bv, &av) != 4) continue;
                const int y = (int)(ymd / 10000), mo = (int)((ymd / 100) % 100), d = (int)(ymd % 100);
                const long t = hmsms / 1000;
                ts = civil_epoch(y, mo, d) * 86400 + (t / 10000) * 3600 + ((t / 100) % 100) * 60 + (t % 100);
                p1 = bv; p2 = av;
            }
            if (!scale_set) { scale_set = true; double m = (p1 + p2) / 2; while (m > 200000.0) { scale *= 1000.0; m /= 1000.0; } }
            const double bid = p1 / scale, ask = p2 / scale;
            if (bid <= 0 || ask <= 0 || ask < bid * 0.98) continue;
            const double mid = 0.5 * (bid + ask);
            ++nticks; if (!first_ts) first_ts = ts; last_ts = ts;

            const int64_t hour = ts / 3600;
            if (cur_hour < 0) cur_hour = hour;
            if (hour != cur_hour) {                       // H1 close = last mid of the finished hour
                bts.push_back((cur_hour + 1) * 3600); bc.push_back(last_mid);
                for (auto& e : eng) e.on_close(bts, bc);
                cur_hour = hour;
            }
            for (auto& e : eng) e.on_tick(ts, bid, ask);  // forming hour: resting stops
            last_mid = mid;
        }
        std::fclose(f);
        std::fprintf(stderr, "  loaded %s (ticks so far %ld, H1 bars %zu)\n", argv[a], nticks, bts.size());
    }
    if (last_mid > 0 && cur_hour >= 0) { bts.push_back((cur_hour + 1) * 3600); bc.push_back(last_mid); for (auto& e : eng) e.on_close(bts, bc); }
    if (dump_path) {   // H1 series for cross-validation vs backtest/index_befloor_ls.py (ts,o,h,l,c: o/h/l=c)
        FILE* d = std::fopen(dump_path, "w");
        if (d) { std::fprintf(d, "ts,o,h,l,c\n");
                 for (size_t i = 0; i < bts.size(); ++i) std::fprintf(d, "%lld,%.4f,%.4f,%.4f,%.4f\n", (long long)bts[i], bc[i], bc[i], bc[i], bc[i]);
                 std::fclose(d); } }

    const int64_t mid_ts = (first_ts + last_ts) / 2;
    std::printf("\n=== index_befloor REAL-FILL salvage grid | rt=%.1fbp dpp=%.2f | ticks=%ld H1=%zu | %lld..%lld ===\n",
                rt_bp, dpp, nticks, bts.size(), (long long)first_ts, (long long)last_ts);
    std::printf("exec A = close-eval, fill worse-of(floor,close) [live status quo]; bufN = + resting intrabar stop N bp under floor\n");
    std::printf("%5s %3s %-6s %6s %5s %10s %10s | %10s %10s | %9s || per-tier usd_real: %s %s %s %s %s\n",
                "thr%", "be", "exec", "clips", "wins", "usd_real", "usd_model", "H1_usd", "H2_usd", "worst_pts",
                TIER[0], TIER[1], TIER[2], TIER[3], TIER[4]);
    for (auto& e : eng) {
        double pts = 0, mdl = 0, h1 = 0, h2 = 0, worst = 0; long wins = 0;
        double tp[NT] = {0}; double fp[2] = {0, 0};
        for (const auto& c : e.closed) {
            pts += c.pts_real; mdl += c.pts_model; if (c.pts_real > 1e-6) ++wins;
            if (c.xts <= mid_ts) h1 += c.pts_real; else h2 += c.pts_real;
            if (c.pts_real < worst) worst = c.pts_real;
            tp[c.ti] += c.pts_real; fp[c.fi] += c.pts_real;
        }
        char tag[16]; if (e.buf_bp < 0) std::snprintf(tag, sizeof tag, "A"); else std::snprintf(tag, sizeof tag, "buf%.0f", e.buf_bp);
        std::printf("%5.2f %3.0f %-6s %6zu %5ld %10.0f %10.0f | %10.0f %10.0f | Pos %8.0f Neg %8.0f | %9.2f || %8.0f %8.0f %8.0f %8.0f %8.0f\n",
                    e.thr * 100, e.be, tag, e.closed.size(), wins, pts * dpp, mdl * dpp, h1 * dpp, h2 * dpp,
                    fp[0] * dpp, fp[1] * dpp, worst,
                    tp[0] * dpp, tp[1] * dpp, tp[2] * dpp, tp[3] * dpp, tp[4] * dpp);
    }
    return 0;
}
