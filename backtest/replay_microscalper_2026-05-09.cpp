// =============================================================================
// replay_microscalper_2026-05-09.cpp
// =============================================================================
// One-shot A/B replay tool. Reads an L2 tick CSV (XAUUSD format) and runs the
// MicroScalperGold engine logic over it twice: once with the PRE-S22 geometry
// (Z=0.75 / TP=0.79 / SL=3.00 / BE=0.50 / TR=0.50 / MAX_HOLD=60) and once with
// the S22 OPTION B geometry (Z=1.75 / TP=1.75 / SL=3.00 / BE=0.80 / TR=0.50 /
// MAX_HOLD=180). Prints a side-by-side comparison.
//
// The engine logic is a direct port of include/GoldMicroScalperEngine.hpp on
// branch 2026-05-09 S22 RE-GEOMETRY commit. The production header is NOT
// modified by this tool. CSV schema follows the standard XAUUSD L2 capture
// format produced by the Omega capture tap (header line: ts_ms, mid, bid,
// ask, l2_imb, ...). L2 columns are read but only used in degraded mode (the
// Friday tape has all-zero depth, so the engine runs the z-only path -- same
// as the production engine when l2_real=false).
//
// BUILD (macOS / Linux):
//   clang++ -std=c++17 -O3 -DNDEBUG \
//       backtest/replay_microscalper_2026-05-09.cpp \
//       -o backtest/replay_2026-05-09
//
// RUN:
//   ./backtest/replay_2026-05-09 data/l2_ticks_XAUUSD_2026-05-08.csv
//
// AUTHORISATION TRAIL: produced for the user's request 2026-05-09 in chat
// ("run this through the same data from friday and give me results") to
// validate the S22 RE-GEOMETRY change against the same tape that produced
// Friday's paper P&L.
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
// Tick + Geometry + Stats data structures
// -----------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

struct Geom {
    const char* name;
    double entry_z;
    double tp_dist;
    double sl_dist;
    double be_trigger;
    double be_offset;
    double trail_dist;
    double reversal_delta;
    int    entry_lookback;
    int    reversal_lookback;
    double max_spread;
    int    max_hold_sec;
    int    cooldown_s;
    int    min_entry_ticks;
    double min_sd;
    double live_lot;       // for USD conversion only
    double usd_per_pt;     // 100 for XAUUSD (full lot)
    int    sess_start;     // UTC hour
    int    sess_end;       // UTC hour, wraparound-aware
    bool   session_enabled = true;  // when false, accept entries any UTC hour
};

struct Stats {
    int    trades         = 0;
    int    wins           = 0;
    double gross_pnl_pts  = 0.0;     // sum of (exit-entry)*lot in pt*lot units
    double mfe_sum_pts    = 0.0;
    double mae_sum_pts    = 0.0;
    int    tp_hits        = 0;
    int    be_hits        = 0;
    int    trail_hits     = 0;
    int    sl_hits        = 0;
    int    rev_exits      = 0;
    int    max_hold_exits = 0;
    double max_dd_pts     = 0.0;     // peak-to-trough drawdown in pt*lot
    double avg_hold_s     = 0.0;
};

// -----------------------------------------------------------------------------
// CSV parser -- tolerant to extra columns; reads ts_ms, bid, ask only.
// Schema: ts_ms, mid, bid, ask, l2_imb, l2_bid_vol, l2_ask_vol, ...
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
// Single-pass engine simulation. Faithful port of GoldMicroScalperEngine.hpp:
// ARMING -> entry on z-score fade, BE-arm at MFE>=BE_TRIGGER, post-BE trail +
// reversal-exit, TP/SL hit detection, MAX_HOLD timeout, per-direction cooldown.
// -----------------------------------------------------------------------------
static void simulate(const std::vector<Tick>& ticks, const Geom& g, Stats& out) {
    enum class Phase { IDLE, COOLDOWN, LIVE };
    Phase   phase           = Phase::IDLE;
    int64_t cooldown_start  = 0;
    int     cooldown_dir    = 0;

    bool    pos_active      = false;
    bool    pos_long        = false;
    double  pos_entry       = 0.0;
    double  pos_sl          = 0.0;
    double  pos_tp          = 0.0;
    double  pos_mfe         = 0.0;
    double  pos_mae         = 0.0;
    int64_t pos_entry_ts    = 0;
    bool    pos_be_locked   = false;

    std::deque<double> window;
    std::deque<double> micro;

    int     ticks_received  = 0;
    double  cum_pnl         = 0.0;
    double  peak_pnl        = 0.0;
    double  total_hold_s    = 0.0;

    auto close_pos = [&](double exit_px, const char* reason, int64_t now_s) {
        const double pnl = (pos_long
            ? (exit_px - pos_entry)
            : (pos_entry - exit_px)) * g.live_lot;
        out.gross_pnl_pts  += pnl;
        out.mfe_sum_pts    += pos_mfe * g.live_lot;
        out.mae_sum_pts    += pos_mae * g.live_lot;
        out.trades++;
        if (pnl > 0) out.wins++;
        if      (!std::strcmp(reason, "TP_HIT"))         out.tp_hits++;
        else if (!std::strcmp(reason, "BE_HIT"))         out.be_hits++;
        else if (!std::strcmp(reason, "TRAIL_HIT"))      out.trail_hits++;
        else if (!std::strcmp(reason, "SL_HIT"))         out.sl_hits++;
        else if (!std::strcmp(reason, "REVERSAL_EXIT"))  out.rev_exits++;
        else if (!std::strcmp(reason, "MAX_HOLD_EXIT"))  out.max_hold_exits++;

        cum_pnl += pnl;
        if (cum_pnl > peak_pnl) peak_pnl = cum_pnl;
        const double dd = peak_pnl - cum_pnl;
        if (dd > out.max_dd_pts) out.max_dd_pts = dd;
        total_hold_s += (double)(now_s - pos_entry_ts);

        cooldown_start = now_s;
        cooldown_dir   = pos_long ? +1 : -1;
        pos_active     = false;
        phase          = Phase::COOLDOWN;
    };

    for (const auto& tk : ticks) {
        const double  mid    = (tk.bid + tk.ask) * 0.5;
        const double  spread = tk.ask - tk.bid;
        const int64_t now_s  = tk.ts_ms / 1000;

        ++ticks_received;
        window.push_back(mid);
        if ((int)window.size() > g.entry_lookback * 4) window.pop_front();
        micro.push_back(mid);
        if ((int)micro.size() > g.reversal_lookback * 4) micro.pop_front();

        // Cooldown decay
        if (phase == Phase::COOLDOWN) {
            if (now_s - cooldown_start >= g.cooldown_s) {
                phase = Phase::IDLE;
                cooldown_dir = 0;
            }
        }

        // -- LIVE management ------------------------------------------------
        if (phase == Phase::LIVE && pos_active) {
            const double move = pos_long ? (mid - pos_entry) : (pos_entry - mid);
            if (move > pos_mfe) pos_mfe = move;
            if (move < pos_mae) pos_mae = move;

            // BE-arm
            if (!pos_be_locked && pos_mfe >= g.be_trigger) {
                const double effective_offset = (pos_mfe >= g.be_offset) ? g.be_offset : 0.0;
                const double be_target = pos_long
                    ? (pos_entry + effective_offset)
                    : (pos_entry - effective_offset);
                if ( pos_long && be_target > pos_sl) pos_sl = be_target;
                if (!pos_long && be_target < pos_sl) pos_sl = be_target;
                pos_be_locked = true;
            }

            // Trail (post-BE only)
            if (pos_be_locked) {
                const double trail_sl = pos_long
                    ? (pos_entry + pos_mfe - g.trail_dist)
                    : (pos_entry - pos_mfe + g.trail_dist);
                if ( pos_long && trail_sl > pos_sl) pos_sl = trail_sl;
                if (!pos_long && trail_sl < pos_sl) pos_sl = trail_sl;
            }

            // Reversal-exit (post-BE only, price-based; L2 disabled in this
            // replay since Friday tape has zero L2 depth)
            if (pos_be_locked) {
                const int n = (int)micro.size();
                if (n >= g.reversal_lookback) {
                    const double last  = micro[n - 1];
                    const double prior = micro[n - g.reversal_lookback];
                    const double delta = last - prior;
                    bool rev = false;
                    if ( pos_long && delta <= -g.reversal_delta) rev = true;
                    if (!pos_long && delta >=  g.reversal_delta) rev = true;
                    if (rev) {
                        const double exit_px = pos_long ? tk.bid : tk.ask;
                        close_pos(exit_px, "REVERSAL_EXIT", now_s);
                        continue;
                    }
                }
            }

            // TP_HIT (limit-style at TP_DIST_PTS from entry)
            const bool tp_hit = pos_long ? (tk.ask >= pos_tp) : (tk.bid <= pos_tp);
            if (tp_hit) {
                close_pos(pos_tp, "TP_HIT", now_s);
                continue;
            }

            // SL_HIT (classify BE / TRAIL / SL by where SL sits at exit)
            const bool sl_hit = pos_long ? (tk.bid <= pos_sl) : (tk.ask >= pos_sl);
            if (sl_hit) {
                const double exit_px       = pos_long ? tk.bid : tk.ask;
                const bool   sl_at_be      = std::fabs(pos_sl - pos_entry) <= 0.05;
                const bool   trail_in_prof = pos_long
                    ? (pos_sl > pos_entry + 0.05)
                    : (pos_sl < pos_entry - 0.05);
                const char*  reason = sl_at_be      ? "BE_HIT"
                                    : trail_in_prof ? "TRAIL_HIT"
                                                    : "SL_HIT";
                close_pos(exit_px, reason, now_s);
                continue;
            }

            // MAX_HOLD safety
            if ((now_s - pos_entry_ts) >= g.max_hold_sec) {
                close_pos(mid, "MAX_HOLD_EXIT", now_s);
                continue;
            }

            continue;  // remain in LIVE
        }

        // -- New-entry path --------------------------------------------------
        if (ticks_received < g.min_entry_ticks) continue;
        if ((int)window.size() < g.entry_lookback) continue;
        if (spread > g.max_spread) continue;

        // UTC session window (skipped when --no-session)
        if (g.session_enabled) {
            const std::time_t t = (std::time_t)now_s;
            std::tm utc{};
            gmtime_r(&t, &utc);
            const int  h = utc.tm_hour;
            const bool in_window =
                (g.sess_end > g.sess_start)
                    ? (h >= g.sess_start && h <  g.sess_end)
                    : (h >= g.sess_start || h <  g.sess_end);
            if (!in_window) continue;
        }

        // Rolling stats over ENTRY_LOOKBACK
        const int  n     = (int)window.size();
        const int  start = n - g.entry_lookback;
        double sum = 0.0;
        for (int i = start; i < n; ++i) sum += window[i];
        const double mean = sum / (double)g.entry_lookback;
        double var = 0.0;
        for (int i = start; i < n; ++i) {
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

        // Open
        const bool   is_long = fire_long;
        const double fill_px = is_long ? tk.ask : tk.bid;
        pos_active     = true;
        pos_long       = is_long;
        pos_entry      = fill_px;
        pos_sl         = is_long ? (fill_px - g.sl_dist) : (fill_px + g.sl_dist);
        pos_tp         = is_long ? (fill_px + g.tp_dist) : (fill_px - g.tp_dist);
        pos_mfe        = 0.0;
        pos_mae        = 0.0;
        pos_be_locked  = false;
        pos_entry_ts   = now_s;
        phase          = Phase::LIVE;
    }

    out.avg_hold_s = (out.trades > 0) ? (total_hold_s / (double)out.trades) : 0.0;
}

// -----------------------------------------------------------------------------
// Reporting
// -----------------------------------------------------------------------------
static void report(const Geom& g, const Stats& s) {
    const double gross_pts_total  = s.gross_pnl_pts;                     // pt*lot
    const double gross_usd_001    = gross_pts_total * 100.0 / g.live_lot * 0.01;
    const double gross_usd_010    = gross_pts_total * 100.0 / g.live_lot * 0.10;
    const double gross_usd_030    = gross_pts_total * 100.0 / g.live_lot * 0.30;
    const double cost_per_trade   = 0.75;  // pt, real-world estimate
    const double cost_usd_001     = s.trades * cost_per_trade * 100.0 * 0.01;
    const double cost_usd_010     = s.trades * cost_per_trade * 100.0 * 0.10;
    const double cost_usd_030     = s.trades * cost_per_trade * 100.0 * 0.30;
    const double net_usd_001      = gross_usd_001 - cost_usd_001;
    const double net_usd_010      = gross_usd_010 - cost_usd_010;
    const double net_usd_030      = gross_usd_030 - cost_usd_030;
    const double wr               = s.trades ? 100.0 * s.wins / s.trades : 0.0;
    const double avg_pt_per_trade = s.trades ? (gross_pts_total / g.live_lot) / s.trades : 0.0;

    std::printf("\n=================================================================\n");
    std::printf("%s\n", g.name);
    std::printf("  Z=%.2f TP=%.2f SL=%.2f BE_TRIG=%.2f TRAIL=%.2f MAX_HOLD=%ds\n",
                g.entry_z, g.tp_dist, g.sl_dist, g.be_trigger, g.trail_dist, g.max_hold_sec);
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  trades            : %d\n", s.trades);
    std::printf("  wins              : %d  (%.2f%% WR)\n", s.wins, wr);
    std::printf("  avg pt/trade      : %+.4f pt\n", avg_pt_per_trade);
    std::printf("  avg hold          : %.1f s\n", s.avg_hold_s);
    std::printf("  exit reasons      : TP=%d  BE=%d  TRAIL=%d  SL=%d  REV=%d  MAX=%d\n",
                s.tp_hits, s.be_hits, s.trail_hits, s.sl_hits, s.rev_exits, s.max_hold_exits);
    std::printf("  max drawdown (pt) : %.2f pt @ %.2fx lot\n",
                s.max_dd_pts / g.live_lot, g.live_lot);
    std::printf("\n");
    std::printf("  GROSS USD (pre-cost, matches dashboard paper P&L logic):\n");
    std::printf("    @ 0.01 lot      : %+.2f\n", gross_usd_001);
    std::printf("    @ 0.10 lot      : %+.2f\n", gross_usd_010);
    std::printf("    @ 0.30 lot      : %+.2f   <- Friday's actual lot size\n", gross_usd_030);
    std::printf("\n");
    std::printf("  REAL COST ESTIMATE (0.75pt/trade):\n");
    std::printf("    @ 0.01 lot      : %.2f\n", -cost_usd_001);
    std::printf("    @ 0.10 lot      : %.2f\n", -cost_usd_010);
    std::printf("    @ 0.30 lot      : %.2f\n", -cost_usd_030);
    std::printf("\n");
    std::printf("  NET USD (gross - real cost, what live trading would actually clear):\n");
    std::printf("    @ 0.01 lot      : %+.2f\n", net_usd_001);
    std::printf("    @ 0.10 lot      : %+.2f\n", net_usd_010);
    std::printf("    @ 0.30 lot      : %+.2f\n", net_usd_030);
}

int main(int argc, char** argv) {
    bool        no_session = false;
    const char* csv_path   = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-session"))   no_session = true;
        else if (!std::strcmp(argv[i], "--help") ||
                 !std::strcmp(argv[i], "-h")) {
            std::fprintf(stderr,
                "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n"
                "  --no-session   ignore the 06-22 UTC session gate; replay\n"
                "                 every tick (use this when the tape covers\n"
                "                 only Asia/dead hours).\n",
                argv[0]);
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
            "usage: %s [--no-session] <l2_ticks_XAUUSD_*.csv>\n",
            argv[0]);
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
    std::getline(f, line);  // skip header
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

    // Tape pre-flight: report UTC range and how many ticks fall inside the
    // 06-22 UTC session gate. If a small fraction is in-session, print a loud
    // warning so the user knows the result will not match production trading.
    int in_session_ticks = 0;
    {
        for (const auto& tk : ticks) {
            const std::time_t t = (std::time_t)(tk.ts_ms / 1000);
            std::tm utc{};
            gmtime_r(&t, &utc);
            if (utc.tm_hour >= 6 && utc.tm_hour < 22) ++in_session_ticks;
        }
    }
    const double pct_in_sess = 100.0 * in_session_ticks / (double)ticks.size();

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
        "     in 06-22 UTC session gate: %d ticks (%.1f%%)\n"
        "     session-mode: %s\n",
        parsed, skipped, span_h, fst_buf, lst_buf,
        in_session_ticks, pct_in_sess,
        no_session ? "DISABLED (--no-session)" : "06-22 UTC ENABLED");

    if (!no_session && pct_in_sess < 5.0) {
        std::fprintf(stderr,
            "[warn] less than 5%% of this tape is inside the 06-22 UTC\n"
            "       session window. Production engine will skip those ticks.\n"
            "       To replay anyway, re-run with --no-session.\n");
    }

    // Geometry definitions ---------------------------------------------------
    Geom pre_s22 = {
        "PRE-S22 (Friday's actual live geometry)",
        /*Z*/ 0.75, /*TP*/ 0.79, /*SL*/ 3.00, /*BE*/ 0.50,
        /*BE_OFF*/ 0.30, /*TR*/ 0.50, /*REV*/ 0.30,
        /*lookback*/ 20, /*rev_lb*/ 5,
        /*max_sp*/ 0.5, /*max_hold*/ 60, /*cooldown*/ 5,
        /*min_entry*/ 30, /*min_sd*/ 0.05,
        /*lot*/ 0.01, /*usd_pt*/ 100.0,
        /*sess*/ 6, 22, /*session_enabled*/ !no_session,
    };
    Geom option_b = {
        "S22 OPTION B (the new geometry just deployed)",
        /*Z*/ 1.75, /*TP*/ 1.75, /*SL*/ 3.00, /*BE*/ 0.80,
        /*BE_OFF*/ 0.30, /*TR*/ 0.50, /*REV*/ 0.30,
        /*lookback*/ 20, /*rev_lb*/ 5,
        /*max_sp*/ 0.5, /*max_hold*/ 180, /*cooldown*/ 5,
        /*min_entry*/ 30, /*min_sd*/ 0.05,
        /*lot*/ 0.01, /*usd_pt*/ 100.0,
        /*sess*/ 6, 22, /*session_enabled*/ !no_session,
    };

    Stats sa, sb;
    simulate(ticks, pre_s22, sa);
    simulate(ticks, option_b, sb);

    report(pre_s22, sa);
    report(option_b, sb);

    // Side-by-side delta -----------------------------------------------------
    const double a_pts = sa.gross_pnl_pts / pre_s22.live_lot;   // raw price pts
    const double b_pts = sb.gross_pnl_pts / option_b.live_lot;
    const double a_usd_030 = a_pts * 100.0 * 0.30;
    const double b_usd_030 = b_pts * 100.0 * 0.30;
    const double a_cost_030 = sa.trades * 0.75 * 100.0 * 0.30;
    const double b_cost_030 = sb.trades * 0.75 * 100.0 * 0.30;
    const double a_net_030  = a_usd_030 - a_cost_030;
    const double b_net_030  = b_usd_030 - b_cost_030;

    std::printf("\n=================================================================\n");
    std::printf("DELTA  (Option B  -  Pre-S22)\n");
    std::printf("-----------------------------------------------------------------\n");
    std::printf("  trades            : %+d  (%.1f%% of pre)\n",
                sb.trades - sa.trades,
                sa.trades ? 100.0 * (sb.trades - sa.trades) / sa.trades : 0.0);
    std::printf("  raw pt total      : %+.2f pt   (Pre %.2f -> OptB %.2f)\n",
                b_pts - a_pts, a_pts, b_pts);
    std::printf("  gross USD @0.30   : %+.2f      (Pre %.2f -> OptB %.2f)\n",
                b_usd_030 - a_usd_030, a_usd_030, b_usd_030);
    std::printf("  net USD @0.30     : %+.2f      (Pre %.2f -> OptB %.2f)\n",
                b_net_030 - a_net_030, a_net_030, b_net_030);
    std::printf("\nDone.\n");
    return 0;
}
