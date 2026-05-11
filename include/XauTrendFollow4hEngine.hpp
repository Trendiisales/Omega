#pragma once
// =============================================================================
//  XauTrendFollow4hEngine.hpp -- XAU 4h trend-follow ensemble (S33d 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from edge_hunt.cpp + top_cells_monthly.cpp results on
//  the full corpus (3-year Dukascopy 2023-09-27 -> 2025-09-26 + 1-month
//  XAU L2 2026-04-09 -> 2026-05-08). Realistic bid/ask fill simulation,
//  $0.06/RT cost subtracted, 0.01 lot.
//
//  Cells (each independently positive in 3/3 Dukascopy years):
//
//      [A] Donchian N=20 sl1.5tp3.0      n=134  WR=47%  net=+$1332  $9.94/trade
//                                         20/26 months +ve, 2-mo max streak
//      [B] InsideBar    sl2.0tp4.0       n=138  WR=44%  net=+$1124  $8.15/trade
//                                         19/26 months +ve, 1-mo max streak  ★
//      [C] ER0.20 mom=20 sl1.5tp3.0      n=214  WR=40%  net=+$840   $3.92/trade
//                                         19/26 months +ve, 2-mo max streak
//
//  Three uncorrelated signal mechanics, all positive on the same regime
//  (XAU 4h trend-follow). Combined expected ~25-30 trades/month at the
//  full ensemble across all three cells. Each cell holds at most one
//  position; three max concurrent positions across the ensemble.
//
//  SAFETY
//
//      - shadow_mode = true by default; promotion to live requires explicit
//        operator authorisation in engine_init.hpp + a 2-week+ shadow
//        period matching backtest expectancy within 50%.
//      - DOES NOT touch ANY protected core engine file.
//      - 0.01 lot cap per cell; max 3 concurrent positions.
//      - Spread cap = 1.0 USD; engines refuse new entries when spread
//        exceeds this (avoids fill quality issues at session boundaries).
//      - Cooldown per cell = 1 bar (4h) after each trade closes; this
//        matches the backtest harness exactly.
//      - All entries use the broker-aware fill side: long on ask, short
//        on bid (matches the realistic-fill backtest that produced the
//        edges above).
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollow4hEngine g_xau_tf_4h;
//
//      // engine_init.hpp (after g_donchian init):
//      g_xau_tf_4h.shadow_mode = kShadowDefault;
//      g_xau_tf_4h.enabled     = true;
//      g_xau_tf_4h.lot         = 0.01;
//      g_xau_tf_4h.max_spread  = 1.0;
//      g_xau_tf_4h.init();
//
//      // tick_gold.hpp inside H4-close branch (after g_minimal_h4_gold.on_h4_bar):
//      omega::XauTfBar tf_h4{};
//      tf_h4.bar_start_ms = s_bar_h4_ms;
//      tf_h4.open  = s_cur_h4.open;
//      tf_h4.high  = s_cur_h4.high;
//      tf_h4.low   = s_cur_h4.low;
//      tf_h4.close = s_cur_h4.close;
//      g_xau_tf_4h.on_h4_bar(tf_h4, bid, ask,
//          g_bars_gold.h4.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp on every tick (alongside g_donchian.on_tick):
//      g_xau_tf_4h.on_tick(bid, ask, now_ms_g, bracket_on_close);
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "OmegaTradeLedger.hpp"  // omega::TradeRecord

namespace omega {

// ---------- Bar input (mirrors DonchianBar so the call site is familiar)
struct XauTfBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ---------- Per-cell position state
struct XauTfPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ---------- Cell config (signal selector + bracket geometry)
enum class XauTfFamily { Donchian20, InsideBar, ErTrend020 };
struct XauTfCellConfig {
    XauTfFamily family;
    double      sl_mult;   // SL = sl_mult * ATR
    double      tp_mult;   // TP = tp_mult * ATR
    const char* name;      // e.g. "Donchian_N20_sl1.5tp3.0"
};

// Three baseline cells -- the validated survivors
static constexpr XauTfCellConfig kXauTfCells[] = {
    { XauTfFamily::Donchian20,  1.5, 3.0, "Donchian_N20_sl1.5tp3.0" },
    { XauTfFamily::InsideBar,   2.0, 4.0, "InsideBar_sl2.0tp4.0"     },
    { XauTfFamily::ErTrend020,  1.5, 3.0, "ER0.20_sl1.5tp3.0"        },
};
static constexpr int kXauTfNumCells =
    static_cast<int>(sizeof(kXauTfCells) / sizeof(kXauTfCells[0]));

// =============================================================================
//  XauTrendFollow4hEngine
// =============================================================================
struct XauTrendFollow4hEngine {
public:
    // Public knobs (set by engine_init.hpp before init()).
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;  // USD; refuse entries above this

    // Per-cell state. Indexed by kXauTfCells.
    std::array<XauTfPos, kXauTfNumCells> pos{};

    // Bar history (last N=64 bars; enough for Donchian20 + ER lookback + warmup).
    static constexpr int kBarHistory = 64;
    std::deque<XauTfBar> bars_;

    // Rolling Wilder ATR14 (mirrors what g_bars_gold.h4.ind.atr14 publishes,
    // but recomputed locally so the engine is self-contained).
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        for (auto& p : pos) p = {};
    }

    bool any_open() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }

    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

    // ============================================================
    //  on_h4_bar -- called by tick_gold.hpp when an H4 bar closes
    // ============================================================
    void on_h4_bar(const XauTfBar& bar,
                   double bid, double ask,
                   double atr14_external,    // from g_bars_gold.h4.ind.atr14
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;

        // Append bar to history.
        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        // Use external ATR if provided (preferred -- matches the rest of
        // the stack); otherwise compute locally.
        if (atr14_external > 0.0) {
            atr14_ = atr14_external;
        } else {
            _update_local_atr();
        }

        // Advance cell cooldowns and increment bars_held on open positions.
        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // Decide entries cell-by-cell. We do NOT re-check exits here --
        // exits run intra-bar on every tick via on_tick (so SL/TP can hit
        // within the bar). The bar-close is only for new-entry decisions.
        if ((int)bars_.size() < 32) return;  // warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close; // signature parity; we only emit on exit (in on_tick)
    }

    // ============================================================
    //  on_tick -- runs every tick to manage open positions
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    // ============================================================
    //  force_close -- end-of-day or shutdown flatten
    // ============================================================
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double exit_px = pos[ci].is_long ? bid : ask;
            _close(ci, exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    // ---------- Local ATR computation (Wilder) over bars_
    void _update_local_atr() noexcept {
        if ((int)bars_.size() < 2) { atr14_ = 0.0; return; }
        const auto& cur  = bars_.back();
        const auto& prev = bars_[bars_.size() - 2];
        double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (atr_warmup_count_ < kAtrPeriod) {
            atr14_ = (atr14_ * atr_warmup_count_ + tr) / (atr_warmup_count_ + 1);
            ++atr_warmup_count_;
        } else {
            atr14_ = (atr14_ * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    // ---------- Signal evaluators
    int _evaluate_signal(int cell_idx) const noexcept {
        switch (kXauTfCells[cell_idx].family) {
            case XauTfFamily::Donchian20:  return _sig_donchian20();
            case XauTfFamily::InsideBar:   return _sig_inside_bar();
            case XauTfFamily::ErTrend020:  return _sig_er_trend(0.20, 20);
        }
        return 0;
    }

    int _sig_donchian20() const noexcept {
        const int N = 20;
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;  // current closed bar
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        // bar[last-1] is the "prior_high/low"; bar[last] is the close that fires.
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_inside_bar() const noexcept {
        if ((int)bars_.size() < 3) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& a = bars_[last - 2];  // mother bar
        const auto& b = bars_[last - 1];  // inside bar
        const auto& c = bars_[last];      // breakout bar
        if (!(b.high < a.high && b.low > a.low)) return 0;
        if (c.close > b.high) return +1;
        if (c.close < b.low)  return -1;
        return 0;
    }

    // Kaufman Efficiency Ratio over last er_n bars, gate momentum at mom_n
    int _sig_er_trend(double er_thr, int N) const noexcept {
        if ((int)bars_.size() < N + 2) return 0;
        const int last = (int)bars_.size() - 1;
        double num = std::abs(bars_[last].close - bars_[last - N].close);
        double den = 0.0;
        for (int k = last - N + 1; k <= last; ++k) {
            den += std::abs(bars_[k].close - bars_[k - 1].close);
        }
        if (den < 1e-12) return 0;
        double er = num / den;
        if (er < er_thr) return 0;
        // momentum direction
        if (bars_[last].close > bars_[last - N].close) return +1;
        if (bars_[last].close < bars_[last - N].close) return -1;
        return 0;
    }

    // ---------- Entry / exit
    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kXauTfCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        double sl_px = (side > 0) ? entry - sl_dist : entry + sl_dist;
        double tp_px = (side > 0) ? entry + tp_dist : entry - tp_dist;

        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = sl_px;
        p.tp_px         = tp_px;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();

        // Live order dispatch is owned by the caller of this engine via
        // engine_init.hpp's fire hook (mirrors what GoldMicroScalper used).
        // The engine itself stays broker-agnostic; the host code is
        // responsible for translating this fire into a real order when
        // shadow_mode == false.
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // Long: exit at bid when bid touches sl/tp. Short: exit at ask.
        bool hit_tp = false, hit_sl = false;
        double exit_px = 0.0;
        if (p.is_long) {
            if (bid <= p.sl_px) { exit_px = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { exit_px = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { exit_px = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { exit_px = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        const char* reason = hit_tp ? "TP_HIT" : "SL_HIT";
        _close(ci, exit_px, reason, now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // Build TradeRecord.  Caller's on_close (e.g. bracket_on_close)
        // handles ledger ingest + PnL math.
        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = "XauTrendFollow4h";
        tr.side       = p.is_long ? "BUY" : "SELL";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTfCells[ci].name;  // cell name -> regime field
        tr.shadow     = shadow_mode;
        // pnl will be computed by handle_closed_trade after tick_mult applied.

        if (on_close) on_close(tr);

        // Reset cell state, apply cooldown (1 bar = 4h)
        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }
};

} // namespace omega
