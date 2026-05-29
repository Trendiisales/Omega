// =====================================================================
// backtest/iswing_replay.cpp -- replay 10 IndexSwing trades from 2026-05-29
// under new exit logic (stage_trail + TP1 partial + cold_loss_cut).
//
// Input: L2 tick CSVs (ts_ms,mid,bid,ask,...) for US500 + USTEC.
// Hardcoded list of 10 trades from operator log:
//   entry_time_utc, sym, side, entry_px, exit_px_old, old_pnl, hold_sec.
// For each trade: load ticks from entry_ts to entry_ts + 8h max_hold,
// then walk through the new manage logic to compute new_exit_px + new_pnl.
//
// Output: side-by-side comparison table.
// =====================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct Tick { long long ts_ms = 0; double bid = 0, ask = 0, mid = 0; };

static std::vector<Tick> load_ticks(const std::string& path) {
    std::vector<Tick> out;
    out.reserve(2'000'000);
    std::ifstream f(path);
    if (!f) { std::cerr << "WARN open " << path << "\n"; return out; }
    std::string line; std::getline(f, line);  // header
    while (std::getline(f, line)) {
        // ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,...
        const char* s = line.c_str();
        Tick t;
        t.ts_ms = atoll(s);
        const char* p = strchr(s, ','); if (!p) continue; ++p;
        t.mid = atof(p);
        p = strchr(p, ','); if (!p) continue; ++p;
        t.bid = atof(p);
        p = strchr(p, ','); if (!p) continue; ++p;
        t.ask = atof(p);
        if (t.bid <= 0 || t.ask <= 0) continue;
        out.push_back(t);
    }
    return out;
}

// Find tick index >= ts_ms via binary search.
static size_t idx_from_ts(const std::vector<Tick>& tks, long long ts_ms) {
    auto it = std::lower_bound(tks.begin(), tks.end(), ts_ms,
        [](const Tick& a, long long v){ return a.ts_ms < v; });
    return (size_t)(it - tks.begin());
}

// Find best matching tick for (entry_px, hh:mm:ss UTC) across multi-day data.
// Looks for tick where time-of-day matches +- 60s AND mid is within +- 1 pt of
// entry_px. Returns first such tick. If multiple days match, picks the best.
static size_t find_entry_tick(const std::vector<Tick>& tks, double entry_px,
                              int hh, int mm, int ss, double tol_pts) {
    if (tks.empty()) return 0;
    long long best_idx = -1;
    double best_diff = 1e18;
    int tod_target = hh * 3600 + mm * 60 + ss;
    for (size_t i = 0; i < tks.size(); ++i) {
        time_t t = tks[i].ts_ms / 1000;
        struct tm tm{}; gmtime_r(&t, &tm);
        int tod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        int dt = std::abs(tod - tod_target);
        if (dt > 60) continue;
        double dpx = std::abs(tks[i].mid - entry_px);
        if (dpx > tol_pts) continue;
        double score = dpx + (double)dt * 0.01;
        if (score < best_diff) { best_diff = score; best_idx = (long long)i; }
    }
    if (best_idx < 0) return SIZE_MAX;
    return (size_t)best_idx;
}

struct TradeIn {
    const char* entry_utc;   // "YYYY-MM-DD HH:MM:SS"
    const char* sym;         // "US500.F" / "USTEC.F"
    bool is_long;
    double entry_px;
    double exit_px_old;
    double old_pnl;
    int hold_sec;
};

// Convert "YYYY-MM-DD HH:MM:SS" UTC to unix ms.
static long long parse_utc(const char* s) {
    struct tm tm{};
    sscanf(s, "%d-%d-%d %d:%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
    tm.tm_year -= 1900; tm.tm_mon -= 1;
    return (long long)timegm(&tm) * 1000LL;
}

// Replay new manage logic for one trade. Returns new exit pts + reason.
// 2026-05-30 iter3: initial SL stays at 1R (gap protection). Layered cold-cuts
// give early escape when MFE doesn't develop (= signal failed):
//   T0: held >= 60s,  adverse >= 0.25R, mfe < 0.1R -> exit at -0.25R
//   T1: held >= 5min, adverse >= 0.5R,  mfe < 0.2R -> exit at -0.5R
//   T2: held >= 15min, adverse >= 0.5R, mfe < 0.3R -> exit at current (~ -0.5R)
struct ReplayOut { double gross_pts = 0; const char* why = "TIMEOUT"; double mfe = 0; int stage = 0; bool tp1_taken = false; int held_sec = 0; };
static ReplayOut replay(const std::vector<Tick>& tks, size_t i0, bool is_long,
                        double entry, double sl_pts, double max_loss_pts) {
    (void)max_loss_pts;  // unused in iter3; kept for signature compat
    ReplayOut r;
    if (i0 >= tks.size()) return r;
    // Initial SL = 1R (original, wide), cold-cuts trim losses sooner.
    double sl       = is_long ? (entry - sl_pts) : (entry + sl_pts);
    double trail_sl = sl;
    double mfe = 0;
    int stage = 0;
    bool be_locked = false;
    bool tp1_taken = false;
    double tp1_pts = 0;
    long long entry_ts = tks[i0].ts_ms;
    const long long max_hold_ms = 8 * 3600 * 1000LL;  // 8h

    for (size_t j = i0 + 1; j < tks.size(); ++j) {
        const auto& tk = tks[j];
        if (tk.ts_ms - entry_ts > max_hold_ms) {
            // TIMEOUT-with-loss check
            double cur_move = is_long ? (tk.mid - entry) : (entry - tk.mid);
            if (cur_move <= 0.0) {
                r.gross_pts = tp1_taken ? (tp1_pts + 0.5 * cur_move) : cur_move;
                r.why = "TIMEOUT"; r.mfe = mfe; r.stage = stage;
                r.tp1_taken = tp1_taken; r.held_sec = (int)((tk.ts_ms - entry_ts) / 1000);
                return r;
            }
            // else keep going (winner-exemption)
        }
        double move = is_long ? (tk.mid - entry) : (entry - tk.mid);
        if (move > mfe) mfe = move;

        // BE lock at 1R
        if (!be_locked && mfe >= sl_pts) {
            be_locked = true;
            trail_sl = entry;
        }
        // TP1 partial 50% at 1.5R
        if (!tp1_taken && mfe >= sl_pts * 1.5) {
            tp1_taken = true;
            tp1_pts   = sl_pts * 1.5 * 0.5;
        }
        // Stage trail
        int new_stage = stage;
        if      (mfe >= sl_pts * 10.0) new_stage = 3;
        else if (mfe >= sl_pts *  5.0) new_stage = std::max(new_stage, 2);
        else if (mfe >= sl_pts *  2.0) new_stage = std::max(new_stage, 1);
        stage = new_stage;
        double trail_dist = sl_pts * 0.5;
        if      (stage == 1) trail_dist = sl_pts * 1.0;
        else if (stage == 2) trail_dist = sl_pts * 2.0;
        else if (stage == 3) trail_dist = sl_pts * 3.0;
        if (be_locked) {
            double new_sl = is_long ? (entry + mfe - trail_dist)
                                    : (entry - mfe + trail_dist);
            if (is_long  && new_sl > trail_sl) trail_sl = new_sl;
            if (!is_long && new_sl < trail_sl) trail_sl = new_sl;
        }
        // Cold-loss cut (3 tiers, no BE). Initial SL stays at 1R.
        int held_sec = (int)((tk.ts_ms - entry_ts) / 1000);
        double cur_adv = is_long ? (entry - tk.mid) : (tk.mid - entry);
        // T1 at 5min: requires mfe < 0.2R = signal failed
        // T2 at 15min: existing
        bool cc_t1 = !be_locked && held_sec >= 300 && cur_adv >= sl_pts * 0.50
                                && mfe < sl_pts * 0.20;
        bool cc_t2 = !be_locked && held_sec >= 900 && cur_adv >= sl_pts * 0.50
                                && mfe < sl_pts * 0.30;
        bool cold_cut = cc_t1 || cc_t2;
        // SL/trail hit
        bool sl_hit = is_long ? (tk.bid <= trail_sl) : (tk.ask >= trail_sl);

        if (sl_hit) {
            double exit_px = trail_sl;
            double rem = is_long ? (exit_px - entry) : (entry - exit_px);
            r.gross_pts = tp1_taken ? (tp1_pts + 0.5 * rem) : rem;
            r.why = "SL_HIT"; r.mfe = mfe; r.stage = stage;
            r.tp1_taken = tp1_taken; r.held_sec = held_sec;
            return r;
        }
        if (cold_cut) {
            double exit_px = tk.mid;
            double rem = is_long ? (exit_px - entry) : (entry - exit_px);
            r.gross_pts = tp1_taken ? (tp1_pts + 0.5 * rem) : rem;
            r.why = "COLD_CUT"; r.mfe = mfe; r.stage = stage;
            r.tp1_taken = tp1_taken; r.held_sec = held_sec;
            return r;
        }
    }
    // Data ran out -- treat as TIMEOUT at last tick
    if (!tks.empty()) {
        const auto& tk = tks.back();
        double cur_move = is_long ? (tk.mid - entry) : (entry - tk.mid);
        r.gross_pts = tp1_taken ? (tp1_pts + 0.5 * cur_move) : cur_move;
        r.why = "DATA_END"; r.mfe = mfe; r.stage = stage;
        r.tp1_taken = tp1_taken;
        r.held_sec = (int)((tk.ts_ms - entry_ts) / 1000);
    }
    return r;
}

int main() {
    auto sp = load_ticks("/tmp/replay/us500_merged.csv");
    auto nq = load_ticks("/tmp/replay/ustec_merged.csv");
    std::sort(sp.begin(), sp.end(), [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    std::sort(nq.begin(), nq.end(), [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    std::cerr << "[load] SP=" << sp.size() << " NQ=" << nq.size() << "\n";

    // 10 trades from operator log. Operator entry timestamps don't match
    // tick-data UTC -- the UI may show a non-UTC clock. Below we use actual
    // UTC times derived by price-match: USTEC 30237@15:05, 30252@15:25,
    // 30277@15:32, 30339@17:50 (all 2026-05-29 UTC). US500 entries match
    // op log timestamps directly (the SP block was UTC).
    TradeIn trades[] = {
        // entry_utc (CORRECTED to actual UTC)  sym       long? entry      exit_old   old_pnl  hold_sec
        { "2026-05-29 17:12:33", "US500.F", true, 7598.41,  7590.41, -200.00,  1830 },
        { "2026-05-29 15:25:02", "US500.F", true, 7599.53,  7604.65, +128.00,  4648 },
        { "2026-05-29 17:50:00", "USTEC.F", true, 30339.85, 30314.85, -100.00,  237 },
        { "2026-05-29 14:16:36", "US500.F", true, 7600.53,  7592.53, -200.00,  505 },
        { "2026-05-29 11:53:44", "US500.F", true, 7590.53,  7604.39, +346.50,  6772 },
        { "2026-05-29 15:25:53", "USTEC.F", true, 30252.15, 30227.15, -100.00,  180 },
        { "2026-05-29 15:32:14", "USTEC.F", true, 30277.00, 30252.00, -100.00,  1141 },
        { "2026-05-29 10:11:26", "US500.F", true, 7587.03,  7592.27, +130.88,  4338 },
        { "2026-05-29 15:05:00", "USTEC.F", true, 30237.50, 30257.50, +80.00,   1806 },
    };

    // Per-symbol cfg. Adds max_loss_pts = cap so loss never exceeds $50.
    // SP: 25$/pt * 2pts = $50.  NQ: 4$/pt * 12.5pts = $50.
    struct Cfg { double sl_pts; double dollar_per_pt; double max_loss_pts; };
    auto cfg = [](const char* s) -> Cfg {
        if (strcmp(s, "US500.F") == 0) return { 8.0,  25.0,  2.0 };
        if (strcmp(s, "USTEC.F") == 0) return { 25.0,  4.0, 12.5 };
        return { 8.0, 25.0, 2.0 };
    };

    printf("%-19s %-7s %-9s %-9s %-9s %-9s %-9s %-9s %-7s %-7s %-6s\n",
           "entry_utc", "sym", "entry", "old_exit", "new_exit", "old_$", "new_$", "delta_$",
           "mfe_pts", "why", "stage");
    printf("%-19s %-7s %-9s %-9s %-9s %-9s %-9s %-9s %-7s %-7s %-6s\n",
           "-----", "---", "-----", "--------", "--------", "-----", "-----", "------",
           "-------", "---", "-----");

    double old_total = 0, new_total = 0;
    for (auto& t : trades) {
        bool is_sp = (strcmp(t.sym, "US500.F") == 0);
        const auto& tks = is_sp ? sp : nq;
        auto c = cfg(t.sym);
        double sl_pts = c.sl_pts;
        double dollar_per_pt = c.dollar_per_pt;
        double max_loss_pts = c.max_loss_pts;
        // Parse hh:mm:ss from entry_utc string.
        int hh, mm, ss;
        sscanf(t.entry_utc, "%*d-%*d-%*d %d:%d:%d", &hh, &mm, &ss);
        // Tolerance: 1.5 pts for SP, 3 pts for NQ (broker spread + slip).
        double tol = is_sp ? 1.5 : 5.0;
        size_t i0 = find_entry_tick(tks, t.entry_px, hh, mm, ss, tol);
        if (i0 == SIZE_MAX) {
            printf("%-19s %-7s %9.2f  NO_MATCH (time=%02d:%02d:%02d entry=%.2f tol=%.1f)\n",
                   t.entry_utc, t.sym, t.entry_px, hh, mm, ss, t.entry_px, tol);
            continue;
        }
        ReplayOut r = replay(tks, i0, t.is_long, t.entry_px, sl_pts, max_loss_pts);
        double new_dollar = r.gross_pts * dollar_per_pt;
        double new_exit_px = t.entry_px + (t.is_long ? r.gross_pts : -r.gross_pts);
        old_total += t.old_pnl;
        new_total += new_dollar;
        printf("%-19s %-7s %9.2f %9.2f %9.2f %+9.2f %+9.2f %+9.2f %7.2f %-7s %6d\n",
               t.entry_utc, t.sym, t.entry_px, t.exit_px_old, new_exit_px,
               t.old_pnl, new_dollar, new_dollar - t.old_pnl,
               r.mfe, r.why, r.stage);
    }
    printf("\n%-29s %38s %+9.2f %+9.2f %+9.2f\n",
           "TOTAL:", "", old_total, new_total, new_total - old_total);
    return 0;
}
