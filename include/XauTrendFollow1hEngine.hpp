#pragma once
// =============================================================================
//  XauTrendFollow1hEngine.hpp -- XAU H1 long-only trend ensemble (S118 2026-05-19)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-19 from the S114 long-trend ensemble research.
//  Two cells, both long-only:
//
//      [A] EmaCross_20_80    -- EMA(20) > EMA(80) with EMA(80) rising
//                               over the last 3 bars. ATR(14)x4 stop.
//                               Python (S114): +$25,478 (+25.48%) Sharpe +1.97
//                                              MDD -2.48% / 181 trades / 25mo
//                               C++    (S115): +$26,024 (+26.02%) / 173 trades
//
//      [C] Donchian_N40      -- Long breakout above 40-bar prior high.
//                               Exit on close below 40-bar prior low.
//                               ATR(14)x5 stop.
//                               Python (S114): +$29,634 (+29.63%) Sharpe +2.11
//                                              MDD -2.33% / 75 trades / 25mo
//                               C++    (S115): +$27,969 (+27.97%) / 70 trades
//
//  Companion to XauTrendFollow4hEngine::EmaCross_8_21 (S116) added on
//  the H4 timeframe.  Together they form the 3-cell ensemble from the
//  research: A on H1, B on H4 (in the 4h engine), C on H1.
//
//  SAFETY
//
//      - shadow_mode = true by default; engine_init.hpp sets the actual
//        value.  Service-level mode=SHADOW (config.ini) is the outer
//        safety net.
//      - DOES NOT touch ANY protected core engine file.
//      - 0.01 lot cap per cell; max 2 concurrent positions in this engine.
//      - Spread cap = 1.0 USD; engines refuse new entries when spread
//        exceeds this (avoids fill quality issues at session boundaries).
//      - Cooldown per cell = 1 bar (H1) after each trade closes.
//      - Long-only by design.  Slow-EMA-rising filter and Donchian-low
//        exit signal both suppress short-side entries entirely.
//      - All entries use the broker-aware fill side: long on ask.
//      - ExecutionCostGuard::is_viable() gates every entry, matching the
//        4hEngine pattern.
//      - Cell C (Donchian40) does NOT use a bracketed TP -- exit happens
//        when close drops below the 40-bar prior low (handled in
//        on_h1_bar bar-close logic).  Cell A uses the standard bracket
//        with tp_mult=20.0 (effectively no TP) -- runners exit only on
//        the ATR-based stop.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollow1hEngine g_xau_tf_1h;
//
//      // engine_init.hpp:
//      g_xau_tf_1h.shadow_mode = kShadowDefault;
//      g_xau_tf_1h.enabled     = true;
//      g_xau_tf_1h.cell_enable_mask = 0x03;  // both cells on
//      g_xau_tf_1h.lot         = 0.01;
//      g_xau_tf_1h.max_spread  = 1.0;
//      g_xau_tf_1h.warmup_csv_path = "phase1/signal_discovery/warmup_XAUUSD_H1.csv";
//      g_xau_tf_1h.init();
//      g_xau_tf_1h.warmup_from_csv(g_xau_tf_1h.warmup_csv_path);
//
//      // tick_gold.hpp at H1 close (after g_bars_gold.h1.add_bar):
//      omega::XauTfBar1h tf_h1{};
//      tf_h1.bar_start_ms = s_bar_h1_ms;
//      tf_h1.open  = s_cur_h1.open;
//      tf_h1.high  = s_cur_h1.high;
//      tf_h1.low   = s_cur_h1.low;
//      tf_h1.close = s_cur_h1.close;
//      g_xau_tf_1h.on_h1_bar(tf_h1, bid, ask,
//          g_bars_gold.h1.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp on every tick (alongside g_xau_tf_4h.on_tick):
//      g_xau_tf_1h.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  Multi-cell engine; default cells have TPs at 3-6*ATR. Predicted NEGATIVE
//  by analogy to XauPullbackContH4 (trail -36% Sharpe at TP=5N).
//  Trail STAYS OFF and is NOT implemented to keep diff small. EmaCross
//  no-TP cell (if present) defer to operator activation + dedicated harness.
//  See XauTrendFollow4hEngine.hpp tombstone for full reasoning.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"  // omega::TradeRecord
#include "OmegaCostGuard.hpp"
#include "RegimeState.hpp"       // 2026-06-12: shared price-based bull/bear gate
#include "GoldWaveTrend.hpp"   // S-2026-06-03 momentum-confirm gate (omega::gold_wt())
#include "OpenPositionRegistry.hpp"  // S-2026-06-03 PositionSnapshot persist/restore
#include "GoldTrendMimicLadder.hpp"  // S-2026-07-14bc: one-way mimic notify (MGC venue instance)
#include <vector>
#include <cstdlib>

namespace omega {

// ---------- Bar input
struct XauTfBar1h {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ---------- Per-cell position state
struct XauTfPos1h {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    double      mfe                = 0.0;
    double      mae                = 0.0;
    double      size_mult          = 1.0;   // S-2026-06-03: regression-slope sizing overlay
    // S39 vol-target + pyramiding (active only when the engine knobs are on;
    // size defaults to the fixed `lot` so legacy behaviour is unchanged).
    double      size               = 0.0;   // this position's lot (vol-targeted or fixed)
    int         n_adds             = 0;     // pyramid units added beyond the base
    double      last_add_px        = 0.0;   // price of the most recent add (or base entry)
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ---------- Cell config
enum class XauTf1hFamily { EmaCross20_80, Donchian40, PullbackEma20, KeltnerEma50 };
struct XauTf1hCellConfig {
    XauTf1hFamily family;
    double        sl_mult;
    double        tp_mult;    // for EmaCross use 20.0 (no-TP); Donchian uses signal-based exit so this is moot
    const char*   name;
};

// Four cells, all long-only.  Ordering matters and is load-bearing for the
// backtest harness attribution (bit 0=EmaCross, 1=Donchian, 2=Pullback,
// 3=Keltner -- see XauTrendFollowBacktest.cpp run_1h() cell parsing).
//
// S40 (2026-05-30) ENSEMBLE BUILD: added the 2 remaining robust trend-entry
// mechanics found in gold_regime_edges.cpp (WF both halves, >=5/6 blocks,
// 3x-cost-robust, param plateaus). With the existing Donchian40 breakout cell
// these form the 3-uncorrelated-mechanic ensemble the S39b handoff called for:
// breakout (Donchian) + pullback (EMA dip) + channel (Keltner).
//   Pullback_EMA20_pb0.5: PF 2.68, +2098 gross (highest total), more trades --
//     BEATS breakout-chasing. Buy the dip in an EMA20>EMA50 uptrend.
//   Keltner_EMA50_k2.0:   PF 2.46, 6/6 robust. Close > EMA50 + 2*ATR.
// Both are stop-only RUNNERS: tp_mult=20.0 is effectively-no-TP (same as the
// EmaCross cell), so the exit is the ATR stop, EXACTLY matching the validation
// harness (which had no TP -- exit on SL or series end). sl=2.5*ATR (validated
// optimum). They flow through the same vol-target/pyramid framework as cells
// 0/1, so the harness sweep shows their pyramiding/lot-sizing enhancement too.
static constexpr XauTf1hCellConfig kXauTf1hCells[] = {
    { XauTf1hFamily::EmaCross20_80, 4.0, 20.0, "EmaCross_20_80_sl4.0_S118" },
    { XauTf1hFamily::Donchian40,    5.0, 20.0, "Donchian_N40_sl5.0_S118"   },
    { XauTf1hFamily::PullbackEma20, 2.5, 20.0, "Pullback_EMA20_pb0.5_S40"  },
    { XauTf1hFamily::KeltnerEma50,  2.5, 20.0, "Keltner_EMA50_k2.0_S40"    },
};
static constexpr int kXauTf1hNumCells =
    static_cast<int>(sizeof(kXauTf1hCells) / sizeof(kXauTf1hCells[0]));

// =============================================================================
//  XauTrendFollow1hEngine
// =============================================================================
struct XauTrendFollow1hEngine {
public:
    // Public knobs (set by engine_init.hpp before init()).
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    // S-2026-06-03: regression-slope sizing overlay. Default OFF = byte-identical
    // behaviour. When ON: down-size to reg_size_floor when the 128-bar regression
    // slope of close is negative (breakout against the macro trend-fit).
    bool   reg_slope_size = true;   // S-2026-06-03: ENABLED (shadow engines -> live shadow validation of the prod-validated overlay)
    double reg_size_floor = 0.4;
    double max_spread  = 1.0;
    uint32_t cell_enable_mask = 0x0F;  // S40: all four ensemble cells on; engine.enabled gates overall
    // S-2026-07-14bc MGC VENUE PORT (same fields the 4h/2h classes grew for the
    // S-2026-07-07 port): defaults keep the spot instance byte-identical; the MGC
    // instance sets "MgcTF1h_"/"MGC" so the cost gate + ledger key the explicit
    // MGC cost row instead of the spot proxy.
    std::string ledger_prefix = "XauTrendFollow1h_";
    std::string ledger_symbol = "XAUUSD";
    // Non-empty => one-way GoldTrendMimicLadder notify (entry trigger + native
    // H1 bar feed). Empty (default/spot) = no mimic hook.
    std::string mimic_tag;

    // S88-followup (2026-05-27): vol-band + ADX gates (defaults OFF).
    bool   use_vol_band_gate = false;
    double vol_band_low_pct  = 0.30;
    double vol_band_high_pct = 0.85;
    // S-2026-06-20 IMPULSE FILTER: breakout cells (Donchian/Keltner/EmaCross — NOT the
    // Pullback dip-buy) require the entry bar to thrust >= min_impulse_atr*ATR
    // ((bar.high - prior close) >= mult*ATR14). Filters weak/stalling breakouts. XAU H1
    // confirmed: DonchN20+BullGate PF 1.60->2.00, maxDD 292->155 (~halved); bear unaffected.
    // 0 = OFF. Activated to 1.0 on g_xau_tf_1h in engine_init.
    double min_impulse_atr   = 0.0;
    bool   use_adx_gate      = false;
    double adx_min           = 25.0;
    std::deque<double> atr_vol_window_;

    // S-2026-06-21 ER TREND/CHOP GATE (defaults OFF). Kaufman efficiency ratio over
    // er_gate_n bars; breakout cells (NOT Pullback dip-buy) skip entry when ER <
    // er_gate_min (low ER = choppy/non-directional = whipsaw). Harness er_gate_trend_bt:
    // gold H1 trend PF 1.05->1.13, edge density 2.4x, maxDD down at ER~0.40 in TREND tape.
    // NOT a bear fix (gate filters chop, not direction) -- relies on existing macro/vol
    // gates for the 2022-class bleed. 0 = OFF (byte-identical). Set 0.40 in engine_init.
    double er_gate_min = 0.0;
    int    er_gate_n   = 20;

    // S63-pattern in-flight protection (defaults disabled).
    double LOSS_CUT_PCT  = 0.0;
    double BE_ARM_PCT    = 0.0;
    double BE_BUFFER_PCT = 0.0;

    // S39 VOL-TARGET SIZING (default OFF -> uses fixed `lot`). When on, the entry
    // lot = clamp(vol_target_unit / atr14, min, max), rounded to 0.01 -- risk is
    // normalised across vol regimes (validated edge: gold_regime_edges.cpp).
    bool   use_vol_target  = false;
    double vol_target_unit = 0.10;   // lot * ATR target; 0.10/ATR$5 ~= 0.02 lot
    double vol_target_min  = 0.01;
    double vol_target_max  = 0.08;

    // S39 PYRAMIDING (default OFF -> pyramid_max_adds=0, single-unit as before).
    // While a cell position runs, add a unit each time price advances
    // +pyramid_step_atr*ATR above the last add, up to pyramid_max_adds, and TRAIL
    // the stop up to (last_add - pyramid_sl_atr*ATR). Lifts avg-win ~3x at K2-K3
    // (harness) at the cost of higher DD/lower PF -- start conservative in shadow.
    int    pyramid_max_adds = 0;
    double pyramid_step_atr = 1.0;
    double pyramid_sl_atr   = 3.0;
    // pullback buy-zone depth in ATRs below EMA20 (was constexpr 0.5; member
    // 2026-06-03 so a shallower depth can be backtested to catch tight grind-up
    // trends that never dip 0.5*ATR). Default 0.5 = unchanged live behavior.
    double pullback_pb_atr  = 0.5;

    std::array<XauTfPos1h, kXauTf1hNumCells> pos{};

    static constexpr int kBarHistory = 128;
    std::deque<XauTfBar1h> bars_;

    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    // EMA(20) / EMA(80) for cell A.  Slow-rising filter uses ema80_hist_.
    static constexpr int kEmaFast = 20;
    static constexpr int kEmaSlow = 80;
    static constexpr int kSlowRiseLookback = 3;
    double ema_fast_ = 0.0;
    double ema_slow_ = 0.0;
    bool   ema_initialised_ = false;
    std::deque<double> ema_slow_hist_;

    // S40: EMA(50) -- shared by the Pullback cell (slow-trend gate: EMA20>EMA50)
    // and the Keltner cell (channel mid-line, EMA50 +/- k*ATR). Updated in
    // _update_emas() under the same ema_initialised_ flag as fast/slow.
    static constexpr int kEmaMid = 50;
    double ema50_ = 0.0;

    // Donchian-N for cell C.  Computed on each bar close.
    static constexpr int kDonchN = 40;
    double don_high_ = 0.0;
    double don_low_  = 0.0;
    bool   don_ready_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    bool warmup_active_ = false;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema_fast_ = ema_slow_ = 0.0;
        ema50_ = 0.0;
        ema_initialised_ = false;
        ema_slow_hist_.clear();
        don_high_ = don_low_ = 0.0;
        don_ready_ = false;
        warmup_active_ = false;
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
    //  S-2026-06-03 open-position PERSISTENCE (save/restore).
    //  One snapshot per ACTIVE cell, tagged "<base_engine>#<cellIdx>" so
    //  restore routes to the right cell. Long-only engine -> side="LONG".
    // ============================================================
    void persist_save_all(const char* base_engine, const char* sym,
                          std::vector<omega::PositionSnapshot>& out) const {
        for (int ci = 0; ci < kXauTf1hNumCells; ++ci) {
            const auto& p = pos[ci];
            if (!p.active) continue;
            omega::PositionSnapshot ps;
            ps.engine   = std::string(base_engine) + "#" + std::to_string(ci);
            ps.symbol   = sym;
            ps.side     = "LONG";
            ps.size     = lot;
            ps.entry    = p.entry_px;
            ps.sl       = p.sl_px;
            ps.tp       = p.tp_px;
            ps.entry_ts = p.entry_ts_ms / 1000;
            out.push_back(ps);
        }
    }

    bool persist_restore(const omega::PositionSnapshot& ps) {
        const auto hash = ps.engine.rfind('#');
        if (hash == std::string::npos) return false;
        const int ci = std::atoi(ps.engine.c_str() + hash + 1);
        if (ci < 0 || ci >= kXauTf1hNumCells) return false;
        auto& p = pos[ci];
        if (p.active) return true;   // already holding -- don't double
        p.active        = true;
        p.is_long       = true;
        p.entry_px      = ps.entry;
        p.sl_px         = ps.sl;
        p.tp_px         = ps.tp;
        p.entry_ts_ms   = ps.entry_ts * 1000;
        p.bars_held     = 0;
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.size_mult     = 1.0;       // restore at full size (overlay re-evaluates on next entry)
        p.size          = lot;
        p.n_adds        = 0;
        p.last_add_px   = ps.entry;
        return true;
    }

    // ============================================================
    // Kaufman efficiency ratio over the last n bar-closes (directional travel /
    // total path). >=0..1; higher = more trending. -1 if insufficient history.
    double _kaufman_er(int n) const noexcept {
        const int sz = (int)bars_.size();
        if (sz < n + 1) return -1.0;
        const double dir = std::fabs(bars_[sz-1].close - bars_[sz-1-n].close);
        double path = 0.0;
        for (int i = sz-n; i < sz; ++i) path += std::fabs(bars_[i].close - bars_[i-1].close);
        return path > 0.0 ? dir / path : 0.0;
    }

    //  on_h1_bar -- called by tick_gold.hpp when an H1 bar closes
    // ============================================================
    void on_h1_bar(const XauTfBar1h& bar,
                   double bid, double ask,
                   double atr14_external,    // from g_bars_gold.h1.ind.atr14
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;

        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        // GoldTrendMimicLadder native H1 feed (S-2026-07-14bc; MGC venue instance
        // only, spot mimic_tag empty = no-op). Fed BEFORE any entry this bar can
        // fire, so a fresh mimic leg first sees the NEXT bar (overlay seq0-skip
        // semantics, same convention as SurvivorPortfolio).
        if (!mimic_tag.empty())
            omega::gold_trend_mimic().on_bar(mimic_tag, bar.high, bar.low, bar.close, now_ms / 1000);

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else                       _update_local_atr();

        _update_emas();
        _update_donchian();

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // Donchian C exit on bar close below don_low_ takes priority over a new entry.
        if (don_ready_ && pos[1].active && bar.close < don_low_) {
            _close(1, bar.close, "DONCH_LOW_EXIT", now_ms, on_close);
        }

        // S39 pyramiding: scale into still-open positions on favourable bar closes.
        for (int ci = 0; ci < kXauTf1hNumCells; ++ci)
            if (pos[ci].active) _maybe_pyramid(ci, bar.close, now_ms);

        if ((int)bars_.size() < kEmaSlow + 4) return;  // need EMA80 + slow-rise lookback
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) {
            // S-2026-07-02 observability: this silently zeroed ALL entries for the
            // bar (zero-trades post-mortem rule: every total-veto must log).
            std::printf("[XTF1H-BLOCK] spread %.2f > max %.2f -- whole-bar entry skip\n",
                        ask - bid, max_spread);
            return;
        }

        // S88-followup: rolling ATR for vol-band.
        if (use_vol_band_gate && atr14_ > 0.0) {
            atr_vol_window_.push_back(atr14_);
            if ((int)atr_vol_window_.size() > 200) atr_vol_window_.pop_front();
        }

        for (int ci = 0; ci < kXauTf1hNumCells; ++ci) {
            if (!(cell_enable_mask & (1u << ci))) continue;
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side <= 0) continue;     // long-only: never short
            // S-2026-06-20 IMPULSE FILTER on breakout cells (NOT the Pullback dip-buy):
            // the entry bar must thrust >= min_impulse_atr*ATR (XAU H1: PF up, maxDD ~halved).
            if (min_impulse_atr > 0.0 && kXauTf1hCells[ci].family != XauTf1hFamily::PullbackEma20
                && atr14_ > 0.0 && (int)bars_.size() >= 2) {
                const double prev_close = bars_[bars_.size()-2].close;
                if ((bar.high - prev_close) < min_impulse_atr * atr14_) continue;  // weak breakout
            }
            // S-2026-06-21 ER trend/chop gate on breakout cells (NOT Pullback dip-buy):
            // skip when Kaufman efficiency ratio < er_gate_min (low ER = chop whipsaw).
            if (er_gate_min > 0.0 && kXauTf1hCells[ci].family != XauTf1hFamily::PullbackEma20) {
                const double er = _kaufman_er(er_gate_n);
                if (er >= 0.0 && er < er_gate_min) continue;
            }
            // S88-followup vol-band
            if (use_vol_band_gate && (int)atr_vol_window_.size() >= 200) {
                int below = 0;
                const int n = (int)atr_vol_window_.size();
                for (int i = 0; i < n; ++i) if (atr_vol_window_[i] < atr14_) ++below;
                const double pct = static_cast<double>(below) / n;
                if (pct < vol_band_low_pct || pct > vol_band_high_pct) continue;
            }
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    // Tick-level position management (SL/TP/BE/LOSS_CUT)
    void on_tick(double bid, double ask, int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTf1hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTf1hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double exit_px = pos[ci].is_long ? bid : ask;
            _close(ci, exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
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

    void _update_emas() noexcept {
        if (bars_.empty()) return;
        const double c = bars_.back().close;
        if (!ema_initialised_) {
            ema_fast_ = ema_slow_ = ema50_ = c;
            ema_initialised_ = true;
        } else {
            const double af = 2.0 / (kEmaFast + 1);
            const double as = 2.0 / (kEmaSlow + 1);
            const double am = 2.0 / (kEmaMid + 1);
            ema_fast_ = af * c + (1.0 - af) * ema_fast_;
            ema_slow_ = as * c + (1.0 - as) * ema_slow_;
            ema50_    = am * c + (1.0 - am) * ema50_;
        }
        ema_slow_hist_.push_back(ema_slow_);
        while ((int)ema_slow_hist_.size() > (kSlowRiseLookback + 4)) {
            ema_slow_hist_.pop_front();
        }
    }

    void _update_donchian() noexcept {
        const int N = kDonchN;
        if ((int)bars_.size() < N + 1) { don_ready_ = false; return; }
        // Donchian over the prior N bars (excluding the current bar at back()).
        double hi = -1e18, lo = 1e18;
        const int last = (int)bars_.size() - 1;
        for (int i = last - N; i < last; ++i) {
            if (bars_[i].high > hi) hi = bars_[i].high;
            if (bars_[i].low  < lo) lo = bars_[i].low;
        }
        don_high_ = hi;
        don_low_  = lo;
        don_ready_ = true;
    }

    // ---------- Signal evaluators (long-only)
    int _evaluate_signal(int cell_idx) const noexcept {
        switch (kXauTf1hCells[cell_idx].family) {
            case XauTf1hFamily::EmaCross20_80: return _sig_ema_20_80();
            case XauTf1hFamily::Donchian40:    return _sig_donchian40();
            case XauTf1hFamily::PullbackEma20: return _sig_pullback_ema20();
            case XauTf1hFamily::KeltnerEma50:  return _sig_keltner_ema50();
        }
        return 0;
    }

    int _sig_ema_20_80() const noexcept {
        if (!ema_initialised_) return 0;
        if ((int)ema_slow_hist_.size() <= kSlowRiseLookback) return 0;
        const double slow_now  = ema_slow_hist_.back();
        const double slow_then = ema_slow_hist_[ema_slow_hist_.size() - 1 - kSlowRiseLookback];
        const bool fast_above  = (ema_fast_ > ema_slow_);
        const bool slow_rising = (slow_now > slow_then);
        if (fast_above && slow_rising) return +1;
        return 0;
    }

    int _sig_donchian40() const noexcept {
        if (!don_ready_) return 0;
        const auto& cur = bars_.back();
        if (cur.close > don_high_) return +1;
        return 0;
    }

    // S40 Pullback-continuation (long-only). EXACT port of gold_regime_edges.cpp
    // sig_pullback() with the validated Pullback_EMA20_pb0.5 params
    // (ema_fast=20, ema_slow=50, pb_atr=0.5): require an EMA20>EMA50 uptrend,
    // the PRIOR bar to have dipped to/below (EMA20 - 0.5*ATR) -- the buy zone --
    // and the CURRENT bar to close back above EMA20 (resumed dip-buy).
    int _sig_pullback_ema20() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if ((int)bars_.size() < 3) return 0;
        if (!(ema_fast_ > ema50_)) return 0;                 // uptrend gate
        const double dip_line = ema_fast_ - pullback_pb_atr * atr14_;
        const auto& prev = bars_[bars_.size() - 2];
        const auto& cur  = bars_.back();
        if (prev.low <= dip_line && cur.close > ema_fast_) return +1;
        return 0;
    }

    // S40 Keltner channel breakout (long-only). EXACT port of
    // gold_regime_edges.cpp sig_keltner() with the validated Keltner_EMA50_k2.0
    // params (ema=50, k=2.0): fire when close > EMA50 + 2.0*ATR.
    static constexpr double kKeltnerK = 2.0;
    int _sig_keltner_ema50() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if ((int)bars_.size() < kEmaMid + 2) return 0;
        const auto& cur = bars_.back();
        if (cur.close > ema50_ + kKeltnerK * atr14_) return +1;
        return 0;
    }

    // Vol-targeted entry lot (or fixed `lot` when use_vol_target is off).
    double _entry_size() const noexcept {
        if (!use_vol_target || atr14_ <= 0.0) return lot;
        double s = vol_target_unit / atr14_;
        s = std::round(s * 100.0) / 100.0;                 // round to 0.01 lot
        if (s < vol_target_min) s = vol_target_min;
        if (s > vol_target_max) s = vol_target_max;
        return s;
    }

    // ---------- Entry / exit
    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        if (warmup_active_) return;
        // 2026-06-12 regime gate: long-only engine -> skip ALL entries in a sustained
        //   gold bear (shared price brain, gold_regime_gate_bt-validated). Inert in bull.
        // S-2026-07-02: this veto ran SILENT for weeks while regime=BEAR suppressed the
        // entire long-only gold book (zero-trades post-mortem). Every block now logs.
        if (omega::gold_regime().long_blocked()) {
            std::printf("[XTF1H-BLOCK] cell=%d regime long_blocked (%s) -- long entry vetoed\n",
                        ci, omega::gold_regime().regime_name());
            return;
        }
        const auto& cfg = kXauTf1hCells[ci];
        double entry = ask;  // long-only
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        double sl_px = entry - sl_dist;
        double tp_px = entry + tp_dist;
        const double size = _entry_size();

        // Cost gate (matches 4hEngine pattern)
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(ledger_symbol.c_str(), spread_pts, tp_dist, size, 1.5)) {
                return;
            }
        }

        // Momentum-confirm gate (S-2026-06-03): gold-validated WaveTrend filter
        // (incidents/2026-06-02-x1-overlay-validation). Long-only engine -> confirm
        // the LONG side. Fails open during WaveTrend warmup.
        if (omega::gold_wt().gate_enabled && !omega::gold_wt().confirms(true)) {
            omega::gold_wt().record_skip();
            printf("[GOLD-MOMGATE] XauTF1h cell=%d SKIP long (no momentum confirm) "
                   "wt1=%.1f regime_up=%d bars=%ld\n",
                   ci, omega::gold_wt().wt1(), (int)omega::gold_wt().regime_up(),
                   omega::gold_wt().bars_seen());
            fflush(stdout);
            return;
        }
        if (omega::gold_wt().gate_enabled) omega::gold_wt().record_pass();

        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = true;
        p.entry_px      = entry;
        p.sl_px         = sl_px;
        p.tp_px         = tp_px;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.size          = size;     // S39: vol-targeted (or fixed) lot
        p.n_adds        = 0;        // S39: pyramid base
        p.last_add_px   = entry;
        // S-2026-06-03: regression-slope sizing overlay (default OFF). Down-size
        // when the 128-bar regression slope of close is negative.
        p.size_mult = 1.0;
        if (reg_slope_size) {
            const int W = 128;
            if ((int)bars_.size() >= W) {
                const int s0 = (int)bars_.size() - W;
                double Sx=0, Sy=0, Sxx=0, Sxy=0;
                for (int k=0; k<W; ++k) { double x=k, y=bars_[s0+k].close; Sx+=x; Sy+=y; Sxx+=x*x; Sxy+=x*y; }
                const double den = (double)W*Sxx - Sx*Sx;
                const double slope = (den != 0.0) ? ((double)W*Sxy - Sx*Sy)/den : 0.0;
                if (slope < 0.0) p.size_mult = reg_size_floor;
            }
        }
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        // GoldTrendMimicLadder (S-2026-07-14bc): one-way fire-and-forget -- spawn
        // INDEPENDENT mimic legs at this entry (MGC venue instance only; spot
        // mimic_tag empty = no-op). Never reads/moves/closes this position.
        if (!mimic_tag.empty())
            omega::gold_trend_mimic().on_trend_open(mimic_tag, +1, p.entry_px, now_ms / 1000);
        (void)side;
    }

    // S39 pyramiding: called per active cell on each bar close. Adds a unit when
    // price has advanced +pyramid_step_atr*ATR above the last add (up to
    // pyramid_max_adds), updates the size-weighted average entry, and trails the
    // stop up. No-op when pyramid_max_adds==0 (default).
    void _maybe_pyramid(int ci, double bar_close, int64_t now_ms) noexcept {
        if (pyramid_max_adds <= 0 || warmup_active_) return;
        auto& p = pos[ci];
        if (!p.active || p.atr_at_entry <= 0.0 || atr14_ <= 0.0) return;
        if (p.n_adds >= pyramid_max_adds) return;
        if (bar_close < p.last_add_px + pyramid_step_atr * atr14_) return;
        const double add_sz  = _entry_size();
        const double add_px  = bar_close;               // fill at bar close (shadow)
        const double new_sz  = p.size + add_sz;
        if (new_sz <= 0.0) return;
        p.entry_px    = (p.entry_px * p.size + add_px * add_sz) / new_sz; // wtd-avg
        p.size        = new_sz;
        p.last_add_px = add_px;
        ++p.n_adds;
        const double trail = add_px - pyramid_sl_atr * atr14_;            // trail stop up
        if (trail > p.sl_px) p.sl_px = trail;
        (void)now_ms;
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        const double mid = (bid + ask) * 0.5;
        const double favourable = mid - p.entry_px;  // long-only
        if (favourable > p.mfe) p.mfe = favourable;
        if (-favourable > p.mae) p.mae = -favourable;

        // S63 in-flight protection (off by default)
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && p.entry_px > 0.0) {
            const double arm_pts    = p.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = p.entry_px * BE_BUFFER_PCT / 100.0;
            if (p.mfe >= arm_pts && favourable <= buffer_pts) {
                _close(ci, bid, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        if (LOSS_CUT_PCT > 0.0 && p.entry_px > 0.0) {
            const double adverse       = -favourable;
            const double loss_cut_dist = p.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                _close(ci, bid, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        // SL/TP (long-only).  Bid hits for both since we exit long at bid.
        if (bid <= p.sl_px) { _close(ci, p.sl_px, "SL_HIT", now_ms, on_close); return; }
        if (bid >= p.tp_px) { _close(ci, p.tp_px, "TP_HIT", now_ms, on_close); return; }
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        const double pts_move = exit_px - p.entry_px;  // long-only

        omega::TradeRecord tr;
        tr.symbol     = ledger_symbol;
        tr.engine     = ledger_prefix + kXauTf1hCells[ci].name;
        tr.side       = "LONG";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = (p.size > 0.0 ? p.size : lot) * p.size_mult;   // S39: vol-targeted/pyramided size; S-2026-06-03 reg-slope overlay
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTf1hCells[ci].name;
        tr.shadow     = shadow_mode;
        tr.pnl        = pts_move * (p.size > 0.0 ? p.size : lot) * p.size_mult;   // S-2026-06-03 reg-slope overlay
        tr.mfe        = p.mfe;
        tr.mae        = p.mae;

        if (on_close) on_close(tr);

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }

public:
    // ---------- Warmup from CSV (matches 4hEngine pattern).
    //   CSV format: bar_start_ms,open,high,low,close
    std::string warmup_csv_path;

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[XauTF1h-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[XauTF1h-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[XauTF1h-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;

        int fed = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#' || line[0] == 'b') continue;
            char* p1; long long ms = std::strtoll(line.c_str(), &p1, 10);
            if (!p1 || *p1 != ',') continue;
            char* p2; double o = std::strtod(p1+1, &p2); if (!p2 || *p2 != ',') continue;
            char* p3; double h = std::strtod(p2+1, &p3); if (!p3 || *p3 != ',') continue;
            char* p4; double l = std::strtod(p3+1, &p4); if (!p4 || *p4 != ',') continue;
            char* p5; double c = std::strtod(p4+1, &p5);
            if (!std::isfinite(o) || !std::isfinite(h) || !std::isfinite(l) || !std::isfinite(c)) continue;

            XauTfBar1h bar; bar.bar_start_ms = ms; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
            on_h1_bar(bar, c, c, 0.0, ms + 3600LL*1000, OnCloseFn{});
            ++fed;
            (void)p5;
        }
        warmup_active_ = false;
        printf("[XauTF1h-WARMUP] fed=%d bars, atr=%.4f ema_fast=%.4f ema_slow=%.4f bars_size=%d path='%s'\n",
               fed, atr14_, ema_fast_, ema_slow_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega
