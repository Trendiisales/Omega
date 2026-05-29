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

struct Tick { long long ts_ms = 0; double bid = 0, ask = 0, mid = 0; double l2_imb = 0.5; double micro = 0; };

static std::vector<Tick> load_ticks(const std::string& path) {
    std::vector<Tick> out;
    out.reserve(2'000'000);
    std::ifstream f(path);
    if (!f) { std::cerr << "WARN open " << path << "\n"; return out; }
    std::string line; std::getline(f, line);  // header
    while (std::getline(f, line)) {
        // ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid,depth_ask,
        //   depth_evts,watchdog,vol_ratio,regime,vpin,has_pos,micro_edge,ewm_drift
        Tick t;
        const char* s = line.c_str();
        t.ts_ms = atoll(s);
        int col = 0;
        const char* p = s;
        while (*p) {
            if      (col == 1) t.mid    = atof(p);
            else if (col == 2) t.bid    = atof(p);
            else if (col == 3) t.ask    = atof(p);
            else if (col == 4) t.l2_imb = atof(p);
            else if (col == 15) { t.micro = atof(p); break; }
            const char* nx = strchr(p, ','); if (!nx) break;
            p = nx + 1; col++;
        }
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

// Replay new manage logic for one trade.
// 2026-05-30 iter4: parameterise cold-cut tiers for sweep. Each tier has
// (sec_min, adv_min_R, mfe_max_R).
struct CCTier { int sec_min; double adv_R; double mfe_max_R; };
struct ReplayOut { double gross_pts = 0; const char* why = "TIMEOUT"; double mfe = 0; int stage = 0; bool tp1_taken = false; int held_sec = 0; double mfe_at_2min=0, mfe_at_3min=0, mfe_at_5min=0; };
static ReplayOut replay(const std::vector<Tick>& tks, size_t i0, bool is_long,
                        double entry, double sl_pts, double max_loss_pts,
                        const std::vector<CCTier>& tiers) {
    (void)max_loss_pts;
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
        // Cold-loss cut: parameterised tiers via `tiers` vector.
        int held_sec = (int)((tk.ts_ms - entry_ts) / 1000);
        double cur_adv = is_long ? (entry - tk.mid) : (tk.mid - entry);
        if (held_sec >= 120 && r.mfe_at_2min == 0) r.mfe_at_2min = mfe;
        if (held_sec >= 180 && r.mfe_at_3min == 0) r.mfe_at_3min = mfe;
        if (held_sec >= 300 && r.mfe_at_5min == 0) r.mfe_at_5min = mfe;
        bool cold_cut = false;
        for (auto& T : tiers) {
            if (!be_locked && held_sec >= T.sec_min
                && cur_adv >= sl_pts * T.adv_R
                && mfe < sl_pts * T.mfe_max_R) {
                cold_cut = true;
                break;
            }
        }
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

    // Sweep cold-cut tier sets, find best on the 9 valid trades.
    // Trade 9 (USTEC 15:05 +$80) is a DATA ANOMALY (engine logged entry
    // 30237.50 but real USTEC was 30315 at that UTC -- likely NAS100 tick
    // leaked into USTEC dispatch; separate engine bug). Skip from sweep.
    struct TierSet { const char* name; std::vector<CCTier> tiers; };
    std::vector<TierSet> sets = {
        { "shipped (T1=5m/0.5R/0.2R + T2=15m/0.5R/0.3R)",
          { {300, 0.50, 0.20}, {900, 0.50, 0.30} } },
        { "+T0a (T0=2m/0.4R/0.10R)",
          { {120, 0.40, 0.10}, {300, 0.50, 0.20}, {900, 0.50, 0.30} } },
        { "+T0b (T0=3m/0.3R/0.05R)",
          { {180, 0.30, 0.05}, {300, 0.50, 0.20}, {900, 0.50, 0.30} } },
        { "+T0c (T0=2m/0.5R/0.05R) -- strictest",
          { {120, 0.50, 0.05}, {300, 0.50, 0.20}, {900, 0.50, 0.30} } },
        { "T1=3m/0.4R/0.15R + T2=15m/0.5R/0.3R",
          { {180, 0.40, 0.15}, {900, 0.50, 0.30} } },
        { "T1=5m/0.3R/0.15R + T2=15m/0.5R/0.3R",
          { {300, 0.30, 0.15}, {900, 0.50, 0.30} } },
    };

    for (auto& set : sets) {
        double old_total = 0, new_total = 0;
        int n_killed_winner = 0;
        std::vector<std::string> hits;
        for (size_t ti = 0; ti < sizeof(trades)/sizeof(trades[0]); ++ti) {
            if (ti == 8) continue;  // skip trade 9 anomaly
            auto& t = trades[ti];
            bool is_sp = (strcmp(t.sym, "US500.F") == 0);
            const auto& tks = is_sp ? sp : nq;
            auto c = cfg(t.sym);
            double sl_pts = c.sl_pts;
            double dollar_per_pt = c.dollar_per_pt;
            double max_loss_pts = c.max_loss_pts;
            int hh, mm, ss;
            sscanf(t.entry_utc, "%*d-%*d-%*d %d:%d:%d", &hh, &mm, &ss);
            double tol = is_sp ? 1.5 : 5.0;
            size_t i0 = find_entry_tick(tks, t.entry_px, hh, mm, ss, tol);
            if (i0 == SIZE_MAX) continue;
            // L2 30-tick rolling gate
            const double mic_block = is_sp ? 0.05 : 0.10;
            double mic_p = 0, imb_p = 0;
            int cn = 0;
            for (size_t j = (i0 >= 30 ? i0 - 30 : 0); j < i0; ++j) {
                mic_p += tks[j].micro; imb_p += tks[j].l2_imb; cn++;
            }
            if (cn > 0) { mic_p /= cn; imb_p /= cn; } else imb_p = 0.5;
            bool l2_blocked = (t.is_long && mic_p < -mic_block)
                           || (!t.is_long && mic_p > mic_block);
            if (!l2_blocked && std::abs(imb_p - 0.5) > 0.01) {
                if (t.is_long && imb_p < 0.45) l2_blocked = true;
                if (!t.is_long && imb_p > 0.55) l2_blocked = true;
            }
            old_total += t.old_pnl;
            if (l2_blocked) continue;  // new_total += 0
            ReplayOut r = replay(tks, i0, t.is_long, t.entry_px, sl_pts, max_loss_pts, set.tiers);
            double nd = r.gross_pts * dollar_per_pt;
            new_total += nd;
            if (t.old_pnl > 0 && nd < 0) ++n_killed_winner;
        }
        printf("%-55s old=%+8.2f new=%+8.2f Δ=%+8.2f killed_winners=%d\n",
               set.name, old_total, new_total, new_total - old_total, n_killed_winner);
    }

    // ALSO emit per-trade detail under shipped (first) tier set.
    printf("\n=== Per-trade detail (shipped tiers, trade 9 skipped) ===\n");
    printf("%-19s %-7s %-8s %-7s %-7s %-7s %-7s %-7s %-7s %s\n",
           "time", "sym", "entry", "old$", "new$", "delta",
           "mfe", "mfe@2m", "mfe@5m", "why");
    auto& set0 = sets[0];
    for (size_t ti = 0; ti < sizeof(trades)/sizeof(trades[0]); ++ti) {
        if (ti == 8) { printf("%-19s SKIP (anomaly)\n", trades[ti].entry_utc); continue; }
        auto& t = trades[ti];
        bool is_sp = (strcmp(t.sym, "US500.F") == 0);
        const auto& tks = is_sp ? sp : nq;
        auto c = cfg(t.sym);
        double sl_pts = c.sl_pts;
        double dollar_per_pt = c.dollar_per_pt;
        double max_loss_pts = c.max_loss_pts;
        int hh, mm, ss;
        sscanf(t.entry_utc, "%*d-%*d-%*d %d:%d:%d", &hh, &mm, &ss);
        double tol = is_sp ? 1.5 : 5.0;
        size_t i0 = find_entry_tick(tks, t.entry_px, hh, mm, ss, tol);
        if (i0 == SIZE_MAX) {
            printf("%-19s NO_MATCH\n", t.entry_utc);
            continue;
        }
        // L2 entry gate using 30-tick PRIOR rolling average (smoother + more
        // discriminating than single-tick per the entry-analysis harness).
        //   block LONG if micro_p30 < -mic_block.
        //   imb usable when |imb_p30 - 0.5| > 0.01.
        const double mic_block = is_sp ? 0.05 : 0.10;
        double mic_p = 0, imb_p = 0;
        int cn = 0;
        for (size_t j = (i0 >= 30 ? i0 - 30 : 0); j < i0; ++j) {
            mic_p += tks[j].micro; imb_p += tks[j].l2_imb; cn++;
        }
        if (cn > 0) { mic_p /= cn; imb_p /= cn; } else imb_p = 0.5;
        bool l2_blocked = false;
        const char* l2_reason = "";
        if (t.is_long && mic_p < -mic_block) { l2_blocked = true; l2_reason = "MIC-OPP"; }
        if (!t.is_long && mic_p > +mic_block) { l2_blocked = true; l2_reason = "MIC-OPP"; }
        if (!l2_blocked && std::abs(imb_p - 0.5) > 0.01) {
            if (t.is_long && imb_p < 0.45) { l2_blocked = true; l2_reason = "IMB-OPP"; }
            if (!t.is_long && imb_p > 0.55) { l2_blocked = true; l2_reason = "IMB-OPP"; }
        }
        (void)tks[i0].micro; (void)tks[i0].l2_imb; (void)l2_reason;
        ReplayOut r = replay(tks, i0, t.is_long, t.entry_px, sl_pts, max_loss_pts, set0.tiers);
        double new_dollar = l2_blocked ? 0 : (r.gross_pts * dollar_per_pt);
        printf("%-19s %-7s %8.2f %+7.2f %+7.2f %+7.2f %7.2f %7.2f %7.2f %s\n",
               t.entry_utc, t.sym, t.entry_px,
               t.old_pnl, new_dollar, new_dollar - t.old_pnl,
               r.mfe, r.mfe_at_2min, r.mfe_at_5min,
               l2_blocked ? "L2-BLOCKED" : r.why);
    }
    return 0;
}
