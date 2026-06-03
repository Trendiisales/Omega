#pragma once
// ============================================================================
// GoldSeasonalEngine.hpp -- early-week long seasonality on XAUUSD (S-2026-06-03)
// ----------------------------------------------------------------------------
// Edge (incidents/2026-06-02-x1-overlay-validation/GOLD_SEASONALITY_FINDING.md):
//   gold rises early-week. 2yr daily XAUUSD, long the Mon + Tue sessions:
//     Mon +0.26%/day t2.27 win61%, Tue +0.24% t1.90 win62%  (Wed +0.16% t1.41)
//   Mon+Tue combined: +24%/yr Sharpe 1.84 (cost 0.02%), POSITIVE every year
//   (2024/25/26), both walk-forward halves +, cost-robust to 5x, DSR-survives.
//   Gold is 24h (open[D] ~= close[D-1]) so open->close == close->close: long the
//   whole eligible UTC day, flat otherwise. Different axis (calendar) -> survives
//   where every price/book signal died.
//
// Direct analog of IndexSeasonalEngine (Tue/Fri indices) / FxSeasonalEngine.
// Long-only. Tick-driven: on a new UTC day, close any held position (~prev
// close) then open long if the new weekday is in entry_mask. Risk gate OFF by
// default (gold is a safe-haven; index_risk_off would wrongly skip gold's best
// risk-off days). Warm-seed via seed_from_d1_csv. shadow default.
//
// Portable weekday (MSVC-safe, no gmtime_r): 1970-01-01 = Thursday.
//   wd = ((days_since_epoch % 7) + 4) % 7  -> Sun=0 Mon=1 Tue=2 ... Sat=6
// ============================================================================
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <fstream>
#include <string>
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord
#include "IndexRiskGate.hpp"      // omega::index_risk_off (optional gate)

namespace omega {

struct GoldSeasonalEngine {
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double usd_per_pt  = 100.0;          // XAUUSD: $100 per 1.0 price-pt per lot
    double max_spread  = 1.0;            // skip fills wider than this (gold daily
                                         // ~21:00 UTC break / illiquid ticks)
    // entry weekdays bitmask (Sun=0..Sat=6). Default Mon(1)+Tue(2) = 0b0110 = 6.
    int    entry_mask  = (1 << 1) | (1 << 2);
    // optional macro risk-off gate (default OFF — gold often does BEST risk-off).
    bool   gate_risk_off = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    bool has_open_position() const noexcept { return pos_.active; }
    double  pos_entry() const noexcept { return pos_.entry_px; }
    double  pos_lot()   const noexcept { return pos_.lot; }
    int64_t pos_entry_ts_ms() const noexcept { return pos_.entry_ts; }
    const std::string symbol = "XAUUSD";

    static int weekday(int64_t ts_ms) noexcept {
        return (int)((((ts_ms / 86400000LL) % 7) + 4 + 7) % 7);   // Sun=0..Sat=6
    }

    void on_tick(double bid, double ask, int64_t ts_ms, OnCloseFn cb) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const int64_t day = ts_ms / 86400000LL;
        if (day != cur_day_) {
            // ---- new UTC day: exit (~prev close, at the fresh tick) + arm entry ----
            if (pos_.active) _close(bid, ask, ts_ms, "SEASONAL_EXIT", cb);
            cur_day_ = day;
            const int wd = weekday(ts_ms);
            const bool blocked = gate_risk_off && omega::index_risk_off();
            want_entry_ = enabled && (entry_mask & (1 << wd)) != 0 && !blocked;
            entry_wd_   = wd;
        }
        // Fill the armed entry on the first TRADEABLE tick of the day. The
        // gold ~21:00 UTC daily break (and illiquid ticks) blow out the spread;
        // requiring spread<=max_spread means we never open at a break/stale price
        // and an open position simply HOLDS through the break (we only act on the
        // day-flip), exiting at the next day's first fresh tick.
        if (want_entry_ && !pos_.active) {
            const double sp = ask - bid;
            if (sp > 0.0 && sp <= max_spread) { _open(ask, ts_ms, entry_wd_); want_entry_ = false; }
        }
    }

    void force_close(int64_t ts_ms, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double b = last_bid_ > 0 ? last_bid_ : pos_.entry_px;
        const double a = last_ask_ > 0 ? last_ask_ : pos_.entry_px;
        _close(b, a, ts_ms, "FORCE_CLOSE", cb);
    }

    // Warm-seed: replay a daily CSV (ts,o,h,l,c) as one tick/day so cur_day_ is
    // current at boot and live doesn't re-fire a stale day. enabled forced off.
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SEED-FATAL] GoldSeasonal: cannot open %s\n", path.c_str()); std::fflush(stdout); return 0; }
        const bool was = enabled; enabled = false;
        auto nub = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line); size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
            if (c <= 0.0) continue;
            const int64_t ts_ms = (ts > 1e11) ? (int64_t)ts        // already ms
                                              : (int64_t)(ts * 1000.0);  // epoch sec
            const double sp = c * 0.00010;
            on_tick(c - sp, c + sp, ts_ms, nub);
            ++n;
        }
        enabled = was;
        std::printf("[SEED][GoldSeasonal] %zu daily bars replayed, cur_day set -- hot\n", n);
        std::fflush(stdout);
        return n;
    }

private:
    struct Pos { bool active=false; double entry_px=0, lot=0; int64_t entry_ts=0; int wday=0; } pos_;
    int64_t cur_day_ = -1;
    double  last_bid_ = 0, last_ask_ = 0;
    bool    want_entry_ = false;     // armed at day-flip, filled on first tradeable tick
    int     entry_wd_ = 0;
    std::string engine_name_ = "GoldSeasonal";

    void _open(double ask, int64_t ts_ms, int wd) noexcept {
        pos_ = Pos{}; pos_.active = true; pos_.entry_px = ask; pos_.lot = lot;
        pos_.entry_ts = ts_ms; pos_.wday = wd;
        std::printf("[GoldSeasonal] ENTRY LONG wd=%d px=%.2f lot=%.3f%s\n",
                    wd, ask, lot, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }
    void _close(double bid, double ask, int64_t ts_ms, const char* why, OnCloseFn cb) noexcept {
        if (!pos_.active) return;
        const double exit_px = bid;                              // long exits at bid
        const double pnl = (exit_px - pos_.entry_px) * pos_.lot * usd_per_pt;
        const double spread = std::fabs(ask - bid);
        const double cost = spread * pos_.lot * usd_per_pt;
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = "LONG"; tr.engine = engine_name_;
        tr.exitReason = why; tr.entryPrice = pos_.entry_px; tr.exitPrice = exit_px;
        tr.size = pos_.lot; tr.pnl = pnl; tr.net_pnl = pnl - cost;
        tr.entryTs = pos_.entry_ts / 1000LL; tr.exitTs = ts_ms / 1000LL;
        tr.spreadAtEntry = spread; tr.shadow = shadow_mode;
        std::printf("[GoldSeasonal] EXIT %s wd=%d pnl=%.2f%s\n",
                    why, pos_.wday, pnl, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (cb) cb(tr);
        pos_ = Pos{};
    }
};

} // namespace omega
