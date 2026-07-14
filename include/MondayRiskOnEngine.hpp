#pragma once
//  ADVERSE-PROTECTION: in-flight protection is a same-day time-stop only (FLAT at Monday close via _close MON_CLOSE) -- no LOSS_CUT/BE/trail/SL-TP bracket; entry has SMA50 risk-on gate (partial bear protection) + cost gate, but no cold-loss cut; shadow-only (engine_init g_monday_*.enabled=true, shadow_mode=true), basis is a 2024-26 m5 data-mining find (anomaly_scan/monday_test/monday_gated) -- no faithful backtest on record -- verdict owed before re-enable (live-size) (backfill S-2026-06-24n)
// =============================================================================
// MondayRiskOnEngine.hpp -- cross-asset MONDAY risk-on calendar anomaly.
// Self-contained (aggregates daily closes from ticks), m-tick driven. SHADOW cell.
//
// Origin: backtest/anomaly_scan.cpp + monday_test.cpp + monday_gated.cpp (2026-06-07).
// Data-mining find, cross-validated: risk-on assets (US equity + commodity/risk FX)
// rally on Monday; safe-haven JPY null, OIL negative -> a weekend-risk-reset flow.
//   LONG at Monday UTC-day open, FLAT at Monday close, ONLY if prior daily close >
//   SMA(sma_len) of daily closes (risk-on regime gate -- improves the edge AND gives
//   partial bear protection). The gate is LOAD-BEARING: ungated flips negative in a
//   bear (gold 2022 Monday -0.27%/t-2.06).
//
// VALIDATED (m5 2024-26, cost-incl, SMA50 gate):
//   NAS    avg+0.26%/Mon t2.59 WR67% both-halves+ all-years+
//   GBPUSD avg+0.14%/Mon t2.04 WR71% both+    AUDUSD avg+0.18% t2.45 WR65% both+
//   CAVEAT: 2024-26 = risk-on bull tape; own-SMA gate is partial bear protection only.
//   A macro risk-on gate (IndexRiskGate) is the real bear defense BEFORE live-size.
//
// WARM-SEED SAFE: seed_from_csv warms the daily-SMA with enabled=false; the entry
// path checks `enabled` so NO phantom entries fire on historical bars (the 2026-06-07
// GoldOrbRetrace phantom class). The central boot-stamp net (trade_lifecycle) backstops.
// =============================================================================
#include <string>
#include <deque>
#include <functional>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <ctime>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"  // omega::PositionSnapshot (persist; S-2026-07-14)

namespace omega {

struct MondayRiskOnEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;

    int    sma_len     = 50;       // daily-close SMA for the risk-on gate
    double lot         = 1.0;
    std::string symbol      = "NAS100";
    std::string engine_name = "MondayRiskOn";
    std::string tag         = "MONRISK";
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;
    bool verbose = false;

    // ---- daily aggregation ----
    int64_t cur_day_   = -1;       // UTC day index
    double  day_close_ = 0.0;      // running last price of the current UTC day
    double  prev_day_close_ = 0.0; // finalized close of the prior UTC day
    std::deque<double> sma_q_; double sma_sum_ = 0.0;

    // ---- open position (Monday) ----
    struct OpenPos { bool active=false; double entry=0.0,lot=0.0,mfe=0.0; int64_t entry_ts=0; } pos_;
    bool has_open_position() const noexcept { return pos_.active; }

    // ---- restart persistence (wire_cross archetype, S-2026-07-14 sweep item 5) ----
    // The display source is registered with a RUNTIME-BUILT name ("MondayRiskOn_"+sym,
    // engine_init.hpp MondayRiskOn block) so the persistence audit's literal grep never
    // saw this engine -- enabled shadow engine holding a Monday open->close leg (up to
    // ~24h) with NO persist wire: a restart mid-Monday orphaned the leg and MON_CLOSE
    // then fired on nothing (position vanished, no ledger close). LONG-only, no SL/TP
    // bracket (time-stop exit) -> sl/tp persist as 0; mfe carried in the mfe field.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const noexcept {
        if (!pos_.active) return false;
        o.engine = eng; o.symbol = sym; o.side = "LONG"; o.size = pos_.lot;
        o.entry = pos_.entry; o.sl = 0.0; o.tp = 0.0;
        o.entry_ts = pos_.entry_ts / 1000;   // engine keeps ms; snapshot is seconds
        o.mfe = pos_.mfe;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) noexcept {
        if (pos_.active) return false;       // adopt won't double an open slot
        pos_ = OpenPos{};
        pos_.active   = true;
        pos_.entry    = ps.entry;
        pos_.lot      = ps.size;
        pos_.mfe      = ps.mfe;
        pos_.entry_ts = ps.entry_ts * 1000;  // snapshot seconds -> engine ms
        return true;
    }

    static int64_t utc_day(int64_t ms) noexcept { return (ms/1000LL)/86400LL; }
    // weekday of a UTC-day index: 1970-01-01 (day 0) = Thursday(4). 0=Sun..6=Sat.
    static int weekday(int64_t day) noexcept { return (int)(((day % 7) + 4) % 7 + 7) % 7; }

    void _push_sma(double close) noexcept {
        sma_q_.push_back(close); sma_sum_ += close;
        while ((int)sma_q_.size() > sma_len) { sma_sum_ -= sma_q_.front(); sma_q_.pop_front(); }
    }
    double sma() const noexcept { return sma_q_.empty() ? 0.0 : sma_sum_/(double)sma_q_.size(); }

    void _close(double exit_px, const char* reason, int64_t now_ms) noexcept {
        const double pnl = (exit_px - pos_.entry) * pos_.lot;   // LONG only; RAW pts*lot
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.side="LONG"; tr.engine=engine_name; tr.exitReason=reason;
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px; tr.size=pos_.lot; tr.pnl=pnl;
        tr.entryTs=pos_.entry_ts/1000LL; tr.exitTs=now_ms/1000LL; tr.mfe=pos_.mfe; tr.shadow=shadow_mode;
        std::printf("[%s] CLOSE %s LONG @ %.4f entry=%.4f pnl=%.4f %s%s\n", tag.c_str(),
                    symbol.c_str(), exit_px, pos_.entry, pnl, reason, shadow_mode?" [SHADOW]":"");
        std::fflush(stdout);
        if (on_trade_record) on_trade_record(tr);
        pos_ = OpenPos{};
    }

    // ---- tick: roll daily close, on UTC-day change finalize + act on Monday transitions ----
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        const int64_t day = utc_day(now_ms);

        if (cur_day_ < 0) { cur_day_ = day; day_close_ = mid; return; }

        if (day != cur_day_) {
            // finalize the day that just ended
            const double finished_close = day_close_;
            const int ended_wd = weekday(cur_day_);

            // if a Monday just ended and we are long -> FLAT at the day's close
            if (pos_.active && ended_wd == 1) _close(finished_close, "MON_CLOSE", now_ms);

            // roll SMA with the finished day's close
            _push_sma(finished_close);
            prev_day_close_ = finished_close;
            cur_day_ = day; day_close_ = mid;

            // a new day starts: if it's Monday + risk-on + enabled -> open LONG at the open
            const int new_wd = weekday(day);
            if (enabled && !pos_.active && new_wd == 1 && (int)sma_q_.size() >= sma_len) {
                // cost gate: 1-day hold; expected move proxy = 0.5% of price (pct-based)
                if (prev_day_close_ > sma()
                    && ExecutionCostGuard::is_viable(symbol.c_str(), ask-bid, mid*0.005, lot, 1.5)) {
                    pos_.active=true; pos_.entry=mid; pos_.lot=lot; pos_.mfe=0.0; pos_.entry_ts=now_ms;
                    std::printf("[%s] ENTRY %s LONG @ %.4f (Mon risk-on: prevC %.4f > SMA%d %.4f)%s\n",
                                tag.c_str(), symbol.c_str(), mid, prev_day_close_, sma_len, sma(),
                                shadow_mode?" [SHADOW]":"");
                    std::fflush(stdout);
                } else if (verbose && prev_day_close_ <= sma()) {
                    std::printf("[%s] SKIP %s Mon: prevC %.4f <= SMA%d %.4f (risk-off)\n",
                                tag.c_str(), symbol.c_str(), prev_day_close_, sma_len, sma());
                }
            }
            return;
        }

        // same day: update running close + MFE
        day_close_ = mid;
        if (pos_.active) { const double m = mid - pos_.entry; if (m > pos_.mfe) pos_.mfe = m; }
    }

    // ---- warm-seed: replay daily bars (ts[ms|s],o,h,l,c) to warm the SMA. enabled=false -> no entries.
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[%s] SEED FAIL '%s'\n", tag.c_str(), path.c_str()); return 0; }
        const bool was = enabled; enabled = false;
        std::string line; std::getline(f, line);
        int fed = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0]=='t' || line[0]=='b') continue;
            std::stringstream ss(line); std::string a,o,h,l,c;
            std::getline(ss,a,','); std::getline(ss,o,','); std::getline(ss,h,',');
            std::getline(ss,l,','); std::getline(ss,c,',');
            if (c.empty()) continue;
            double cl = std::strtod(c.c_str(), nullptr);
            int64_t ms = std::strtoll(a.c_str(), nullptr, 10); if (ms < 100000000000LL) ms *= 1000LL;
            if (cl <= 0) continue;
            const int64_t day = utc_day(ms);
            if (cur_day_ < 0) { cur_day_ = day; day_close_ = cl; }
            else if (day != cur_day_) { _push_sma(day_close_); prev_day_close_ = day_close_; cur_day_ = day; day_close_ = cl; }
            else day_close_ = cl;
            ++fed;
        }
        enabled = was;
        std::printf("[%s] SEED %s fed=%d sma%d=%.4f n=%d\n", tag.c_str(), symbol.c_str(), fed, sma_len, sma(), (int)sma_q_.size());
        std::fflush(stdout);
        return fed;
    }
};

} // namespace omega
