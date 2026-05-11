// =============================================================================
// option_a_chop_gated_2026-05-09.cpp
// =============================================================================
// OPTION A test: chop engine (rk12) with regime filter only -- no trend engine.
//
// Hypothesis: if we shadow the chop engine when the regime classifier says
// "trend", we should preserve May (chop-regime) profits while reducing April
// (trend-regime) losses. The Tier 3 portfolio test failed because the trend
// engine itself had no edge AND the regime classifier barely filtered anything
// at ER threshold 0.25. This test isolates the regime classifier alone and
// tries multiple thresholds to find one that actually distinguishes the two
// regimes on your data.
//
// OUTPUT (per tape):
//   (A) CHOP-ONLY            no gate, baseline = current production
//   (B) CHOP-GATED ER<0.18   aggressive gate (filter more aggressively)
//   (C) CHOP-GATED ER<0.25   default gate (same as portfolio test)
//   (D) CHOP-GATED ER<0.32   loose gate (only filter the most extreme trends)
//   Plus regime tick stats.
//
// What "GATED" means here: the chop engine refuses NEW entries when the regime
// classifier says current ER >= threshold. Existing positions are managed
// normally regardless of regime.
//
// BUILD:
//   clang++ -std=c++17 -O3 -DNDEBUG \
//       backtest/option_a_chop_gated_2026-05-09.cpp \
//       -o backtest/option_a_chop_gated_2026-05-09
//
// USAGE:
//   ./backtest/option_a_chop_gated_2026-05-09 [--no-session] <tape.csv>
//
// AUTHORISATION TRAIL: produced for user request 2026-05-09 in chat
// ("try option a").
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

// -----------------------------------------------------------------------------
// Common types
// -----------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

struct Stats {
    int    trades         = 0;
    int    wins           = 0;
    double gross_pnl_pts  = 0.0;
    double mfe_sum_pts    = 0.0;
    double mae_sum_pts    = 0.0;
    int    tp_hits        = 0;
    int    be_hits        = 0;
    int    trail_hits     = 0;
    int    sl_hits        = 0;
    int    rev_exits      = 0;
    int    max_hold_exits = 0;
    double max_dd_pts     = 0.0;
    double avg_hold_s     = 0.0;
};

struct ChopGeom {
    double entry_z          = 0.75;
    double tp_dist          = 0.79;
    double sl_dist          = 3.00;
    double be_trigger       = 0.50;
    double be_offset        = 0.30;
    double trail_dist       = 0.50;
    double reversal_delta   = 0.30;
    int    entry_lookback   = 20;
    int    reversal_lb      = 5;
    double max_spread       = 0.5;
    int    max_hold_sec     = 60;
    int    cooldown_s       = 5;
    int    min_entry_ticks  = 30;
    double min_sd           = 0.05;
    double live_lot         = 0.01;
    int    sess_start       = 6;
    int    sess_end         = 22;
    bool   session_enabled  = true;
};

// Kaufman Efficiency Ratio regime detector
struct Regime {
    int    window;
    double threshold;
    std::deque<double> prices;
    double current_er  = 0.0;
    bool   is_trend    = false;
    int    chop_ticks  = 0;
    int    trend_ticks = 0;
    int    warm_ticks  = 0;

    Regime(int w, double t) : window(w), threshold(t) {}

    void on_tick(double mid) {
        prices.push_back(mid);
        if ((int)prices.size() > window) prices.pop_front();
        if ((int)prices.size() < window) {
            warm_ticks++;
            return;
        }
        const double net = std::fabs(prices.back() - prices.front());
        double gross = 0.0;
        for (size_t i = 1; i < prices.size(); ++i) {
            gross += std::fabs(prices[i] - prices[i - 1]);
        }
        current_er = (gross > 1e-9) ? (net / gross) : 0.0;
        is_trend = (current_er >= threshold);
        if (is_trend) trend_ticks++; else chop_ticks++;
    }
};

// -----------------------------------------------------------------------------
// CSV parser
// -----------------------------------------------------------------------------
static bool parse_tick(const std::string& line, Tick& t) {
    int field = 0;
    size_t pos = 0;
    int64_t ts_ms = 0;
    double bid = 0.0, ask = 0.0;
    while (pos <= line.size()) {
        size_t nxt = line.find(',', pos);
        if (nxt == std::string::npos) nxt = line.size();
        const char* s = line.c_str() + pos;
        switch (field) {
            case 0: ts_ms = std::strtoll(s, nullptr, 10); break;
            case 2: bid   = std::strtod (s, nullptr);     break;
            case 3: ask   = std::strtod (s, nullptr);     break;
            default: break;
        }
        ++field;
        if (nxt == line.size()) break;
        pos = nxt + 1;
    }
    if (ts_ms <= 0 || bid <= 0.0 || ask <= 0.0 || ask <= bid) return false;
    t.ts_ms = ts_ms;
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

// -----------------------------------------------------------------------------
// Session helper
// -----------------------------------------------------------------------------
static bool in_session(int64_t now_s, const ChopGeom& g) {
    if (!g.session_enabled) return true;
    const std::time_t t = (std::time_t)now_s;
    std::tm utc{};
    gmtime_r(&t, &utc);
    const int  h = utc.tm_hour;
    return (g.sess_end > g.sess_start)
        ? (h >= g.sess_start && h <  g.sess_end)
        : (h >= g.sess_start || h <  g.sess_end);
}

// -----------------------------------------------------------------------------
// Position state
// -----------------------------------------------------------------------------
struct Pos {
    bool    active     = false;
    bool    is_long    = false;
    double  entry      = 0.0;
    double  sl         = 0.0;
    double  tp         = 0.0;
    double  mfe        = 0.0;
    double  mae        = 0.0;
    int64_t entry_ts   = 0;
    bool    be_locked  = false;
};

// -----------------------------------------------------------------------------
// CHOP engine simulation (rk12) with optional regime gate.
//   regime_threshold = 1.01  -> no gate (entries always allowed)
//   regime_threshold < 1.0   -> gate ON: refuse entries when ER >= threshold
// -----------------------------------------------------------------------------
static void simulate_chop(const std::vector<Tick>& ticks,
                          const ChopGeom& g,
                          double regime_threshold,
                          int    regime_window,
                          Stats& out,
                          int&   filtered_by_regime,
                          int&   trend_ticks_total,
                          int&   chop_ticks_total)
{
    const bool gated = (regime_threshold < 1.0);
    Regime regime(regime_window, gated ? regime_threshold : 1.01);

    enum class Phase { IDLE, COOLDOWN, LIVE };
    Phase   phase = Phase::IDLE;
    int64_t cooldown_start = 0;
    int     cooldown_dir   = 0;

    Pos pos;
    std::deque<double> window;
    std::deque<double> micro;

    int    ticks_received = 0;
    double cum            = 0.0;
    double peak           = 0.0;
    double total_hold_s   = 0.0;

    filtered_by_regime = 0;

    for (const auto& tk : ticks) {
        const double  mid    = (tk.bid + tk.ask) * 0.5;
        const double  spread = tk.ask - tk.bid;
        const int64_t now_s  = tk.ts_ms / 1000;

        regime.on_tick(mid);

        ++ticks_received;
        window.push_back(mid);
        if ((int)window.size() > g.entry_lookback * 4) window.pop_front();
        micro.push_back(mid);
        if ((int)micro.size() > g.reversal_lb * 4) micro.pop_front();

        if (phase == Phase::COOLDOWN) {
            if (now_s - cooldown_start >= g.cooldown_s) {
                phase = Phase::IDLE;
                cooldown_dir = 0;
            }
        }

        // ---- Manage existing position (regime gate doesn't affect mgmt) ----
        if (phase == Phase::LIVE && pos.active) {
            const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
            if (move > pos.mfe) pos.mfe = move;
            if (move < pos.mae) pos.mae = move;

            if (!pos.be_locked && pos.mfe >= g.be_trigger) {
                const double off = (pos.mfe >= g.be_offset) ? g.be_offset : 0.0;
                const double bt = pos.is_long
                    ? (pos.entry + off)
                    : (pos.entry - off);
                if ( pos.is_long && bt > pos.sl) pos.sl = bt;
                if (!pos.is_long && bt < pos.sl) pos.sl = bt;
                pos.be_locked = true;
            }
            if (pos.be_locked) {
                const double trail_sl = pos.is_long
                    ? (pos.entry + pos.mfe - g.trail_dist)
                    : (pos.entry - pos.mfe + g.trail_dist);
                if ( pos.is_long && trail_sl > pos.sl) pos.sl = trail_sl;
                if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
            }
            if (pos.be_locked) {
                const int n = (int)micro.size();
                if (n >= g.reversal_lb) {
                    const double last  = micro[n - 1];
                    const double prior = micro[n - g.reversal_lb];
                    const double delta = last - prior;
                    bool rev = false;
                    if ( pos.is_long && delta <= -g.reversal_delta) rev = true;
                    if (!pos.is_long && delta >=  g.reversal_delta) rev = true;
                    if (rev) {
                        const double exit_px = pos.is_long ? tk.bid : tk.ask;
                        const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                                       : (pos.entry - exit_px)) * g.live_lot;
                        out.gross_pnl_pts += pnl;
                        out.trades++;
                        if (pnl > 0) out.wins++;
                        out.rev_exits++;
                        cum += pnl;
                        if (cum > peak) peak = cum;
                        if ((peak - cum) > out.max_dd_pts) out.max_dd_pts = peak - cum;
                        total_hold_s += (double)(now_s - pos.entry_ts);
                        cooldown_start = now_s;
                        cooldown_dir = pos.is_long ? +1 : -1;
                        pos.active = false;
                        phase = Phase::COOLDOWN;
                        continue;
                    }
                }
            }
            const bool tp_hit = pos.is_long ? (tk.ask >= pos.tp) : (tk.bid <= pos.tp);
            if (tp_hit) {
                const double pnl = (pos.is_long ? (pos.tp - pos.entry)
                                                : (pos.entry - pos.tp)) * g.live_lot;
                out.gross_pnl_pts += pnl;
                out.trades++;
                if (pnl > 0) out.wins++;
                out.tp_hits++;
                cum += pnl;
                if (cum > peak) peak = cum;
                if ((peak - cum) > out.max_dd_pts) out.max_dd_pts = peak - cum;
                total_hold_s += (double)(now_s - pos.entry_ts);
                cooldown_start = now_s;
                cooldown_dir = pos.is_long ? +1 : -1;
                pos.active = false;
                phase = Phase::COOLDOWN;
                continue;
            }
            const bool sl_hit = pos.is_long ? (tk.bid <= pos.sl) : (tk.ask >= pos.sl);
            if (sl_hit) {
                const double exit_px = pos.is_long ? tk.bid : tk.ask;
                const bool sl_at_be = std::fabs(pos.sl - pos.entry) <= 0.05;
                const bool trail_in_prof = pos.is_long
                    ? (pos.sl > pos.entry + 0.05)
                    : (pos.sl < pos.entry - 0.05);
                const double pnl = (pos.is_long ? (exit_px - pos.entry)
                                                : (pos.entry - exit_px)) * g.live_lot;
                out.gross_pnl_pts += pnl;
                out.trades++;
                if (pnl > 0) out.wins++;
                if      (sl_at_be)      out.be_hits++;
                else if (trail_in_prof) out.trail_hits++;
                else                    out.sl_hits++;
                cum += pnl;
                if (cum > peak) peak = cum;
                if ((peak - cum) > out.max_dd_pts) out.max_dd_pts = peak - cum;
                total_hold_s += (double)(now_s - pos.entry_ts);
                cooldown_start = now_s;
                cooldown_dir = pos.is_long ? +1 : -1;
                pos.active = false;
                phase = Phase::COOLDOWN;
                continue;
            }
            if ((now_s - pos.entry_ts) >= g.max_hold_sec) {
                const double pnl = (pos.is_long ? (mid - pos.entry)
                                                : (pos.entry - mid)) * g.live_lot;
                out.gross_pnl_pts += pnl;
                out.trades++;
                if (pnl > 0) out.wins++;
                out.max_hold_exits++;
                cum += pnl;
                if (cum > peak) peak = cum;
                if ((peak - cum) > out.max_dd_pts) out.max_dd_pts = peak - cum;
                total_hold_s += (double)(now_s - pos.entry_ts);
                cooldown_start = now_s;
                cooldown_dir = pos.is_long ? +1 : -1;
                pos.active = false;
                phase = Phase::COOLDOWN;
                continue;
            }
            continue;
        }

        // ---- New entry ----
        if (ticks_received < g.min_entry_ticks) continue;
        if ((int)window.size() < g.entry_lookback) continue;
        if (spread > g.max_spread) continue;
        if (!in_session(now_s, g)) continue;

        // z-score
        const int  n  = (int)window.size();
        const int  st = n - g.entry_lookback;
        double sum = 0.0;
        for (int i = st; i < n; ++i) sum += window[i];
        const double mean = sum / (double)g.entry_lookback;
        double var = 0.0;
        for (int i = st; i < n; ++i) {
            const double d = window[i] - mean;
            var += d * d;
        }
        const double sd = std::sqrt(var / (double)g.entry_lookback);
        if (sd < g.min_sd) continue;
        const double z = (mid - mean) / sd;

        const bool block_long  = (phase == Phase::COOLDOWN && cooldown_dir == +1);
        const bool block_short = (phase == Phase::COOLDOWN && cooldown_dir == -1);
        const bool z_long      = (z <= -g.entry_z);
        const bool z_short     = (z >=  g.entry_z);
        const bool fire_long   = z_long  && !block_long;
        const bool fire_short  = z_short && !block_short;
        if (!fire_long && !fire_short) continue;
        if ( fire_long &&  fire_short) continue;

        // REGIME GATE: this is the only thing that differentiates
        // gated runs from the no-gate baseline.
        if (gated && regime.is_trend) {
            ++filtered_by_regime;
            continue;
        }

        const bool   is_long = fire_long;
        const double fill_px = is_long ? tk.ask : tk.bid;
        pos.active     = true;
        pos.is_long    = is_long;
        pos.entry      = fill_px;
        pos.sl         = is_long ? (fill_px - g.sl_dist) : (fill_px + g.sl_dist);
        pos.tp         = is_long ? (fill_px + g.tp_dist) : (fill_px - g.tp_dist);
        pos.mfe        = 0.0;
        pos.mae        = 0.0;
        pos.be_locked  = false;
        pos.entry_ts   = now_s;
        phase          = Phase::LIVE;
    }

    out.avg_hold_s = (out.trades > 0) ? (total_hold_s / (double)out.trades) : 0.0;
    trend_ticks_total = regime.trend_ticks;
    chop_ticks_total  = regime.chop_ticks;
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------
static void report(const char* name, const Stats& s, double live_lot,
                   double threshold, int filtered, int trend_t, int chop_t)
{
    const double raw_pts        = (live_lot > 0.0)
        ? (s.gross_pnl_pts / live_lot) : s.gross_pnl_pts;
    const double avg_pt         = (s.trades > 0) ? (raw_pts / s.trades) : 0.0;
    const double wr             = (s.trades > 0) ? (100.0 * s.wins / s.trades) : 0.0;
    const double gross_usd_001  = raw_pts * 100.0 * 0.01;
    const double gross_usd_030  = raw_pts * 100.0 * 0.30;

    std::printf("\n=================================================================\n");
    std::printf("%s\n", name);
    if (threshold < 1.0) {
        std::printf("  ER threshold        : %.3f  (entries blocked when ER >= this)\n",
                    threshold);
        std::printf("  filtered by regime  : %d entries\n", filtered);
    } else {
        std::printf("  ER threshold        : NONE (no regime gate)\n");
    }
    if (trend_t + chop_t > 0) {
        const double trend_pct = 100.0 * trend_t / (trend_t + chop_t);
        std::printf("  regime tape stats   : %d trend ticks (%.1f%%) / %d chop ticks\n",
                    trend_t, trend_pct, chop_t);
    }
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  trades              : %d\n", s.trades);
    std::printf("  wins                : %d  (%.2f%% WR)\n", s.wins, wr);
    std::printf("  avg pt/trade        : %+.4f pt\n", avg_pt);
    std::printf("  avg hold            : %.1f s\n", s.avg_hold_s);
    std::printf("  exit reasons        : TP=%d  BE=%d  TRAIL=%d  SL=%d  REV=%d  MAX=%d\n",
                s.tp_hits, s.be_hits, s.trail_hits, s.sl_hits,
                s.rev_exits, s.max_hold_exits);
    std::printf("  raw pt total        : %+.2f pt\n", raw_pts);
    std::printf("  GROSS USD @ 0.01    : %+.2f\n", gross_usd_001);
    std::printf("  GROSS USD @ 0.30    : %+.2f\n", gross_usd_030);
}

int main(int argc, char** argv) {
    bool        no_session = false;
    const char* csv_path   = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-session"))  no_session = true;
        else if (!std::strcmp(argv[i], "--help") ||
                 !std::strcmp(argv[i], "-h")) {
            std::fprintf(stderr,
                "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n", argv[0]);
            return 0;
        }
        else if (argv[i][0] == '-') {
            std::fprintf(stderr, "[err] unknown flag: %s\n", argv[i]);
            return 2;
        }
        else csv_path = argv[i];
    }
    if (!csv_path) {
        std::fprintf(stderr,
            "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n", argv[0]);
        return 2;
    }

    std::ifstream f(csv_path);
    if (!f) {
        std::fprintf(stderr, "[err] cannot open: %s\n", csv_path);
        return 1;
    }
    std::vector<Tick> ticks;
    ticks.reserve(2'000'000);
    std::string line;
    std::getline(f, line);
    int parsed = 0, skipped = 0;
    while (std::getline(f, line)) {
        Tick t;
        if (parse_tick(line, t)) { ticks.push_back(t); ++parsed; }
        else                     { ++skipped; }
    }
    if (ticks.empty()) {
        std::fprintf(stderr, "[err] no parseable ticks\n");
        return 1;
    }

    const int64_t first_ms = ticks.front().ts_ms;
    const int64_t last_ms  = ticks.back().ts_ms;
    const double  span_h   = (last_ms - first_ms) / 1000.0 / 3600.0;
    std::time_t fst = (std::time_t)(first_ms / 1000);
    std::time_t lst = (std::time_t)(last_ms  / 1000);
    char fst_buf[64], lst_buf[64];
    std::tm fut{}, lut{};
    gmtime_r(&fst, &fut);
    gmtime_r(&lst, &lut);
    std::strftime(fst_buf, sizeof(fst_buf), "%Y-%m-%d %H:%M:%S UTC", &fut);
    std::strftime(lst_buf, sizeof(lst_buf), "%Y-%m-%d %H:%M:%S UTC", &lut);
    std::fprintf(stderr,
        "[ok] loaded %d ticks (skipped %d)\n"
        "     span    = %.2f hours\n"
        "     first   = %s\n"
        "     last    = %s\n"
        "     session-mode: %s\n",
        parsed, skipped, span_h, fst_buf, lst_buf,
        no_session ? "DISABLED (--no-session)" : "06-22 UTC ENABLED");

    ChopGeom g;
    g.session_enabled = !no_session;

    const int er_window = 200;

    // Run each variant in turn
    struct Variant {
        const char* name;
        double      threshold;
    };
    Variant variants[] = {
        {"(A) CHOP-ONLY  (no regime gate, baseline = current production)", 1.01},
        {"(B) CHOP-GATED  ER<0.18 (aggressive)",                            0.18},
        {"(C) CHOP-GATED  ER<0.25 (default, same as Tier 3 portfolio)",     0.25},
        {"(D) CHOP-GATED  ER<0.32 (loose, only filter strong trends)",      0.32},
    };

    for (const auto& v : variants) {
        Stats s;
        int filtered = 0, trend_t = 0, chop_t = 0;
        simulate_chop(ticks, g, v.threshold, er_window, s,
                      filtered, trend_t, chop_t);
        report(v.name, s, g.live_lot, v.threshold, filtered, trend_t, chop_t);
    }

    std::printf("\nDone.\n");
    return 0;
}
