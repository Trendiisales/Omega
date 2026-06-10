#pragma once
// =============================================================================
//  FxCarryEngine.hpp -- FX carry-only single-pair engine (S43, FX-native edge)
//
//  THE FIRST VALIDATED OMEGA FX EDGE. Carry = hold the high-yield side of a
//  pair, earn the rate differential (swap) daily. The most COST-SURVIVABLE FX
//  edge: positions hold weeks, turnover is tiny (~40 trades / 7yr across 11
//  pairs), so retail spread amortises to ~zero -- the opposite of the churn
//  that killed every prior FX engine (session / turtle / scalp).
//
//  VALIDATION (backtest/fx_carry_momentum.cpp, Dukascopy D1 2019-2026, 11 pairs):
//    CARRY-ONLY mom-gate-OFF, reb5, carry_floor 0.75-1.0:
//      full-11  Sharpe 0.52-0.54  +12-14k bp  5/6 blocks  both-halves+
//      cost 1x/2x/3x  +12356 / ~ / +stable  (cost-proof)
//    A/B proved the momentum-agreement gate HURTS (churns, cuts winning carry
//    trends) -> this engine is CARRY-ONLY by design. JPY crosses drive most of
//    the edge (USDJPY/EURJPY/GBPJPY ~105%); full-11 is more regime-robust.
//
//  DESIGN -- one instance per FX symbol (mirrors FxEnsembleEngine). The strategy
//  decomposes per-pair (no cross-sectional ranking: take EVERY pair whose
//  |carry| clears the floor), so a per-symbol engine is the natural fit.
//    signal:  dir = sign(carry)  iff |carry| >= carry_floor_pct   (carry-only)
//    sizing:  vol-target -> lot = clamp(target_vol_bps/ATR_bps * lot, .., max)
//    cadence: re-evaluate every rebal_days D1 closes; trade only on dir change
//    carry:   accrued daily into the open position's pnl (rate/365 per day)
//    NO price-momentum gate, NO fixed TP (carry has no target -- state-driven).
//
//  Carry signal source: FxRateTable.hpp (central-bank policy-rate step tables).
//
//  WARM-SEED (mandate): seed_from_d1_csv() replays a bundled D1 close CSV while
//  enabled=false so ATR + day-count warm without firing. Ship a warmup CSV in
//  phase1/signal_discovery/ per pair before merge.
//
//  USAGE
//    // globals.hpp
//    static omega::FxCarryEngine g_fx_carry_usdjpy("USDJPY");
//    // engine_init.hpp
//    g_fx_carry_usdjpy.shadow_mode = true;
//    g_fx_carry_usdjpy.p.carry_floor_pct = 0.75;
//    g_fx_carry_usdjpy.enabled = true;
//    g_fx_carry_usdjpy.seed_from_d1_csv("phase1/signal_discovery/warmup_USDJPY_D1.csv");
//    // tick path
//    g_fx_carry_usdjpy.on_tick(bid, ask, now_ms, ca_on_close);
// =============================================================================
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "FxRateTable.hpp"

namespace omega {

struct FxCarryParams {
    double  carry_floor_pct = 0.75;     // min |rate diff| (% p.a.) to hold a position
    int     rebal_days      = 5;        // re-evaluate dir every N D1 closes
    double  target_vol_bps  = 50.0;     // vol-target: scales lot by inverse ATR
    double  max_lot         = 0.10;
    int     atr_period      = 14;
    double  usd_per_pt      = 100000.0; // standard FX lot scaling (1.0 lot = 100k units)
};

class FxCarryEngine {
public:
    bool          shadow_mode = true;
    bool          enabled     = false;
    double        lot         = 0.01;   // base lot (scaled by vol-target)
    FxCarryParams p;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit FxCarryEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN") { engine_name_ = "FxCarry_" + symbol_; }

    const std::string& symbol() const noexcept { return symbol_; }
    bool has_open_position() const noexcept { return pos_.active; }

    // -------------------------------------------------------------------------
    //  on_tick -- aggregate D1 bars (UTC day buckets) from the live quote, fire
    //  on_d1_bar at each day boundary. bid/ask drive fills + spread cost.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        const int64_t day = (now_ms / 86400000LL) * 86400000LL;
        if (!acc_open_) {
            acc_open_ = true; acc_day_ = day;
            acc_o_ = acc_h_ = acc_l_ = acc_c_ = mid;
            return;
        }
        if (day != acc_day_) {
            on_d1_bar(acc_o_, acc_h_, acc_l_, acc_c_, bid, ask, acc_day_, on_close);
            acc_day_ = day; acc_o_ = acc_h_ = acc_l_ = acc_c_ = mid;
        } else {
            if (mid > acc_h_) acc_h_ = mid;
            if (mid < acc_l_) acc_l_ = mid;
            acc_c_ = mid;
        }
    }

    // -------------------------------------------------------------------------
    //  on_d1_bar -- one closed D1 bar. Updates ATR, accrues carry on the open
    //  position, and re-evaluates the carry signal on the rebalance cadence.
    // -------------------------------------------------------------------------
    void on_d1_bar(double o, double h, double l, double c,
                   double bid, double ask, int64_t day_ms,
                   OnCloseFn on_close) noexcept {
        (void)o;
        last_bid_ = bid; last_ask_ = ask;   // keep quotes fresh for force_close (bar-driven path)
        update_atr(h, l, c);
        prev_close_ = c;
        ++day_count_;
        const int64_t ts_sec = day_ms / 1000;
        const double carry = pair_carry(symbol_.c_str(), ts_sec);   // % p.a.

        // Accrue today's carry into the open position (rate/365 per day, signed).
        if (pos_.active) {
            ++pos_.bars_held;
            const double dir = pos_.is_long ? 1.0 : -1.0;
            pos_.accrued_carry_bp += dir * carry * 100.0 / 365.0;   // % p.a. -> bp/day
        }

        if (!enabled || atr_ <= 0.0 || day_count_ < p.atr_period) return;
        if (day_count_ % p.rebal_days != 0) return;                 // only on cadence

        // Desired direction: carry-only.
        double want = 0.0;
        if (std::fabs(carry) >= p.carry_floor_pct) want = (carry > 0) ? 1.0 : -1.0;
        const double have = pos_.active ? (pos_.is_long ? 1.0 : -1.0) : 0.0;
        if (want == have) return;                                   // no change

        // Close existing leg (if any) then open the new desired leg.
        if (pos_.active) close_position(c, bid, ask, day_ms, "CARRY_FLIP", on_close);
        if (want != 0.0) open_position(want > 0.0, c, bid, ask, day_ms, carry);
    }

    void force_close(int64_t day_ms, OnCloseFn on_close) noexcept {
        if (!pos_.active) return;
        const double bid = (last_bid_ > 0.0) ? last_bid_ : prev_close_;   // fallback to last close
        const double ask = (last_ask_ > 0.0) ? last_ask_ : prev_close_;
        close_position(prev_close_, bid, ask, day_ms, "FORCE_CLOSE", on_close);
    }
    void cancel() noexcept { pos_ = Pos{}; }

    // -------------------------------------------------------------------------
    //  Warm-seed from a D1 close CSV. Two formats accepted:
    //    Dukascopy:  "timestamp,open,high,low,close" with ts in MILLISECONDS
    //    legacy:     "ts,o,h,l,c"                     with ts in SECONDS
    //  Replays bars with enabled=false so ATR + day_count warm without firing.
    // -------------------------------------------------------------------------
    size_t seed_from_d1_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[SEED-FATAL] FxCarry %s: cannot open %s\n",
                        symbol_.c_str(), path.c_str());
            std::fflush(stdout);
            return 0;
        }
        const bool was_enabled = enabled;
        enabled = false;
        auto null_cb = [](const omega::TradeRecord&){};
        std::string line; std::getline(f, line);   // header
        size_t n = 0;
        while (std::getline(f, line)) {
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5)
                continue;
            if (c <= 0.0) continue;
            int64_t day_ms = (ts > 1e11) ? (int64_t)ts : (int64_t)(ts * 1000.0);  // ms vs sec
            // Drop flat Saturday bars (Dukascopy D1 artifact) so the seed matches
            // live (FX has no Saturday ticks -> no Saturday bar live). Portable
            // weekday (no gmtime_r -- not in MSVC): 1970-01-01 = Thursday(4), Sat=6.
            { int64_t dd=day_ms/86400000LL; int wd=(int)(((dd%7)+4+7)%7); if(wd==6) continue; }
            day_ms = (day_ms / 86400000LL) * 86400000LL;
            const double sp = c * 0.00005;                  // ~0.5bp synthetic spread for seed
            on_d1_bar(o, h, l, c, c - sp, c + sp, day_ms, null_cb);
            ++n;
        }
        enabled = was_enabled;
        std::printf("[SEED][FxCarry-%s] %zu D1 bars replayed  atr=%.6f day_count=%d -- hot\n",
                    symbol_.c_str(), n, atr_, day_count_);
        std::fflush(stdout);
        return n;
    }

private:
    struct Pos {
        bool    active           = false;
        bool    is_long          = false;
        double  entry_px         = 0.0;
        double  lot              = 0.0;
        int64_t entry_ts_ms      = 0;
        int     bars_held        = 0;
        double  accrued_carry_bp = 0.0;
        double  carry_at_entry   = 0.0;
    } pos_;

    void update_atr(double h, double l, double c) noexcept {
        if (prev_close_ <= 0.0) { prev_close_ = c; return; }
        const double tr = std::fmax(h - l,
                              std::fmax(std::fabs(h - prev_close_), std::fabs(l - prev_close_)));
        if (atr_warm_ < p.atr_period) {
            atr_sum_ += tr; ++atr_warm_;
            if (atr_warm_ == p.atr_period) atr_ = atr_sum_ / p.atr_period;
        } else {
            atr_ = (atr_ * (p.atr_period - 1) + tr) / p.atr_period;     // Wilder
        }
    }

    double sized_lot(double price) const noexcept {
        if (atr_ <= 0.0 || price <= 0.0) return lot;
        const double atr_bps = atr_ / price * 10000.0;
        if (atr_bps <= 0.0) return lot;
        double L = (p.target_vol_bps / atr_bps) * lot;
        if (L < 0.01)        L = 0.01;
        if (L > p.max_lot)   L = p.max_lot;
        return L;
    }

    void open_position(bool is_long, double close_px, double bid, double ask,
                       int64_t day_ms, double carry) noexcept {
        const double L = sized_lot(close_px);
        // cost gate: 1-ATR expected move proxy (daily-rebalance hold)
        if (atr_ > 0.0 && !ExecutionCostGuard::is_viable(symbol_.c_str(), ask - bid, atr_, L, 1.5)) return;
        pos_ = Pos{};
        pos_.active           = true;
        pos_.is_long          = is_long;
        pos_.entry_px         = is_long ? ask : bid;     // cross the spread in
        pos_.lot              = L;
        pos_.entry_ts_ms      = day_ms;
        pos_.carry_at_entry   = carry;
        std::printf("[FxCarry-%s] ENTRY %s carry=%+.2f%% px=%.5f lot=%.3f%s\n",
                    symbol_.c_str(), is_long ? "LONG" : "SHORT", carry,
                    pos_.entry_px, pos_.lot, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
    }

    void close_position(double close_px, double bid, double ask, int64_t day_ms,
                        const char* reason, OnCloseFn on_close) noexcept {
        if (!pos_.active) return;
        const double exit_px = pos_.is_long ? bid : ask;            // cross the spread out
        const double dir     = pos_.is_long ? 1.0 : -1.0;
        const double price_bp = dir * (exit_px - pos_.entry_px) / pos_.entry_px * 10000.0;
        const double total_bp = price_bp + pos_.accrued_carry_bp;   // price + accrued carry
        // USD-equivalent, price-LEVEL independent (1.0 lot = usd_per_pt notional).
        // Including entry_px would inflate JPY-quote pairs (~150) ~100x vs EUR (~1.1).
        const double notional = pos_.lot * p.usd_per_pt;
        const double pnl_usd  = total_bp / 10000.0 * notional;
        const double spread_bp = std::fabs(ask - bid) / pos_.entry_px * 10000.0;
        const double cost_usd  = spread_bp / 10000.0 * notional;     // 1 spread round-trip

        std::printf("[FxCarry-%s] EXIT %s reason=%s price_bp=%+.1f carry_bp=%+.1f"
                    " total_bp=%+.1f pnl=%.2f bars=%d%s\n",
                    symbol_.c_str(), pos_.is_long ? "LONG" : "SHORT", reason,
                    price_bp, pos_.accrued_carry_bp, total_bp, pnl_usd, pos_.bars_held,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);

        omega::TradeRecord tr{};
        tr.symbol        = symbol_;
        tr.side          = pos_.is_long ? "LONG" : "SHORT";
        tr.entryPrice    = pos_.entry_px;
        tr.exitPrice     = exit_px;
        tr.size          = pos_.lot;
        tr.pnl           = pnl_usd;
        tr.net_pnl       = pnl_usd - cost_usd;
        tr.entryTs       = pos_.entry_ts_ms / 1000;
        tr.exitTs        = day_ms / 1000;
        tr.engine        = engine_name_;
        tr.exitReason    = reason;
        tr.spreadAtEntry = std::fabs(ask - bid);
        tr.shadow        = shadow_mode;
        if (on_close) on_close(tr);

        pos_ = Pos{};
    }

    std::string symbol_;
    std::string engine_name_;   // "FxCarry_<SYMBOL>", set in ctor

    // D1 aggregation
    bool    acc_open_ = false;
    int64_t acc_day_  = 0;
    double  acc_o_ = 0, acc_h_ = 0, acc_l_ = 0, acc_c_ = 0;
    double  last_bid_ = 0, last_ask_ = 0;

    // ATR (Wilder) over D1
    double  atr_ = 0.0, atr_sum_ = 0.0;
    int     atr_warm_ = 0;
    double  prev_close_ = 0.0;
    int     day_count_ = 0;
};

} // namespace omega
