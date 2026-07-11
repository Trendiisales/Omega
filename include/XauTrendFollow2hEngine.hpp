#pragma once
// =============================================================================
//  XauTrendFollow2hEngine.hpp -- XAU 2h trend-follow ensemble (S33k 2026-05-11)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-11 from Pass-8 deep_dive results. Same trend regime as
//  the 4h/D1 ensembles, denser cadence. Realistic bid/ask fills, $0.06/RT
//  cost, 0.01 lot. All cells positive in all 3 Duka years (2023 Q4 → 2025
//  Q3):
//
//      [A] Keltner   K=2.0      sl2.0tp4.0    n=179  net=+$950   BE=$5.37
//      [B] Donchian  N=20       sl2.0tp4.0    n=204  net=+$872   BE=$4.33
//      [C] Donchian  N=50       sl2.0tp4.0    n=142  net=+$833   BE=$5.93
//      [D] InsideBar             sl2.0tp4.0    n=239  net=+$725   BE=$3.09
//
//  Combined ~764 trades over 30 months, ~$3,380 historical net at 0.01 lot.
//  Roughly 25 trades/month combined across the 4 cells. Pure XAU
//  concentration -- this engine is correlated with XauTrendFollow4hEngine
//  and XauTrendFollowD1Engine; treat the three as one XAU multi-TF
//  position rather than independent diversification.
//
//  ARCHITECTURE
//
//  Takes H1 bars via on_h1_bar() (tick_gold.hpp already publishes these
//  to g_donchian and others). Internally groups 2 H1 bars into 1 H2 bar
//  by aligning on UTC even hours.
//
//  SAFETY
//      - shadow_mode = true by default
//      - 0.01 lot per cell, max 4 concurrent positions
//      - 1-bar cooldown per cell
//      - Spread cap 1.0 USD; no entries above
//      - No protected file touched
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauTrendFollow2hEngine g_xau_tf_2h;
//
//      // engine_init.hpp:
//      g_xau_tf_2h.shadow_mode = kShadowDefault;
//      g_xau_tf_2h.enabled     = true;
//      g_xau_tf_2h.lot         = 0.01;
//      g_xau_tf_2h.max_spread  = 1.0;
//      g_xau_tf_2h.init();
//
//      // tick_gold.hpp (in H1-close branch, after the donchian H1 dispatch):
//      omega::XauTf2hBar tf_h1{};
//      tf_h1.bar_start_ms = s_bar_h1_ms;
//      tf_h1.open  = s_cur_h1.open;
//      tf_h1.high  = s_cur_h1.high;
//      tf_h1.low   = s_cur_h1.low;
//      tf_h1.close = s_cur_h1.close;
//      g_xau_tf_2h.on_h1_bar(tf_h1, bid, ask, now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp per-tick:
//      g_xau_tf_2h.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S37 Phase H STAGE-TRAIL TOMBSTONE-BY-ANALOGY (2026-05-27b).
//  Multi-cell engine; cells use TPs at 3-6*ATR. Predicted NEGATIVE by analogy
//  to XauPullbackContH4 (trail -36% Sharpe at TP=5N). Trail STAYS OFF and is
//  NOT implemented to keep diff small. See XauTrendFollow4hEngine.hpp
//  tombstone for full reasoning.
// =============================================================================
//
//  S34 P1 FIXES (2026-05-12) -- close-path bug class from HANDOFF_S34.md §3.2,
//  §4.2. See XauTrendFollow4hEngine.hpp for full diagnosis. Bug numbers
//  below match the table in §3.2.
//
//    #1 tr.pnl was never assigned in _close. Fix: pts_move = (long?
//       exit-entry : entry-exit); tr.pnl = pts_move * lot. Downstream
//       handle_closed_trade applies tick_value_multiplier(symbol) for USD.
//    #3 tr.engine was bare "XauTrendFollow2h" for all 4 cells. Fix:
//       tr.engine = "XauTrendFollow2h_" + cell.name.
//    #4 tr.side was BUY/SELL. Fix: LONG/SHORT.
//    #5 MFE/MAE never tracked. Fix: mfe/mae fields on XauTf2hPos, updated
//       per tick in _manage_open with mid = (bid+ask)/2, propagated to
//       TradeRecord.mfe/.mae in _close.
//
//  Bug #2 (symbol key) NOT applicable per handoff §4.2 -- "XAUUSD" is
//  the right sizing-table key. S34-B guards NOT carried over (5m-USTEC
//  calibrated, meaningless on 2h XAU).
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "GoldD1TrendState.hpp"
#include "RegimeState.hpp"       // 2026-06-21: macro-hostile long-block (BearProtect coverage)
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"  // S-2026-06-03 PositionSnapshot persist/restore
#include "GoldTrendMimicLadder.hpp"  // one-way mimic trigger (fire-and-forget on open)
#include <vector>
#include <cstdlib>

namespace omega {

// Input: H1 bar (caller provides; engine builds 2h internally)
struct XauTf2hBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

struct XauTf2hPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34 P1 fix #5: per-position MFE/MAE in price units (>=0).
    double      mfe                = 0.0;
    double      mae                = 0.0;
    double      size_mult          = 1.0;   // S-2026-06-03: regression-slope sizing overlay
    std::string broker_position_id;
    std::string entry_clOrdId;
};

enum class XauTf2hFamily { Keltner20, Donchian20, Donchian50, InsideBar };
struct XauTf2hCellConfig {
    XauTf2hFamily family;
    double        sl_mult;
    double        tp_mult;
    const char*   name;
};

// 4 validated cells, all 3/3 Duka years positive at sl2.0/tp4.0
static constexpr XauTf2hCellConfig kXauTf2hCells[] = {
    { XauTf2hFamily::Keltner20,  2.0, 4.0, "2h_Keltner_K2_sl2.0tp4.0"   },
    { XauTf2hFamily::Donchian20, 2.0, 4.0, "2h_Donchian_N20_sl2.0tp4.0" },
    { XauTf2hFamily::Donchian50, 2.0, 4.0, "2h_Donchian_N50_sl2.0tp4.0" },
    { XauTf2hFamily::InsideBar,  2.0, 4.0, "2h_InsideBar_sl2.0tp4.0"    },
};
static constexpr int kXauTf2hNumCells =
    static_cast<int>(sizeof(kXauTf2hCells) / sizeof(kXauTf2hCells[0]));

// Internal 2h bar synthesised from two consecutive H1 bars
struct XauTf2hSynthBar {
    long long bucket_ms = 0;  // ts of bar start; aligned to even hours
    double    open  = 0.0;
    double    high  = 0.0;
    double    low   = 0.0;
    double    close = 0.0;
    int       h1_count = 0;
};

struct XauTrendFollow2hEngine {
public:
    bool   shadow_mode = true;
    bool   enabled     = false;
    // S88-followup (2026-05-27): two regime gates, defaults OFF.
    //   vol_band: skip when ATR-pct outside [low, high]
    //   adx_gate: skip when ADX14 < adx_min (trend too weak)
    // Full Duka 2yr backtest: on 2h cells, ADX25 outperforms vol_band
    // (PF 1.29 -> 1.42 with ADX vs 1.29 -> 1.27 with vol_band). Wire ADX
    // for 2h in engine_init.hpp; leave vol_band off here.
    bool   use_vol_band_gate = false;
    double vol_band_low_pct  = 0.30;
    double vol_band_high_pct = 0.85;
    std::deque<double> atr_vol_window_;
    bool   use_adx_gate      = false;
    double adx_min           = 25.0;
    // S88-followup post-sweep 2026-05-27: per-cell gate masks (default
    // all-ones = regression-safe, gate applies to all enabled cells when
    // use_X_gate=true). Sweep showed 2h cells split: Keltner/Donch20/
    // InsideBar prefer ADX25 (PF lifts); Donch50 is HURT by ADX (1.37->
    // 1.22) but lifts under vol_band (1.37->1.54, DD -44%). With per-cell
    // masks operator can apply ADX to bits {0,1,3} (0xB) and vol_band to
    // bit 2 (0x4). Bit i corresponds to kXauTf2hCells[i].
    uint32_t cell_adx_mask      = 0xFFFFFFFF;
    uint32_t cell_vol_band_mask = 0xFFFFFFFF;
    // ADX Wilder state
    static constexpr int kAdxPeriod = 14;
    double adx14_           = 0.0;
    double adx_atr_sum_     = 0.0;
    double adx_pdm_sum_     = 0.0;
    double adx_mdm_sum_     = 0.0;
    double adx_dx_sum_      = 0.0;
    int    adx_warmup_count_ = 0;
    int    adx_dx_count_     = 0;
    double lot         = 0.01;
    // S-2026-06-03: regression-slope sizing overlay. Default OFF = byte-identical
    // behaviour. When ON: down-size to reg_size_floor when the 128-bar regression
    // slope of close is negative (breakout against the macro trend-fit).
    bool   reg_slope_size = true;   // S-2026-06-03: ENABLED (shadow engines -> live shadow validation of the prod-validated overlay)
    double reg_size_floor = 0.4;
    double max_spread  = 1.0;

    // S63 2026-05-14 (part W): VWR-pattern in-flight protection (full trio).
    //   Defaults to ALL ZERO -- S63 is OFF on production until per-instrument
    //   backtest evidence justifies enabling. The XauTrendFollow trio
    //   (2h/4h/D1) is in STATE B (hooks declared + mgmt-path implemented +
    //   deliberately zeroed at class default) until the planned Phase 3
    //   walk-forward sweep (scripts/xtf_*_wf_t1.py + the dedicated
    //   XauTrendFollowBacktest harness) produces baseline-vs-tuned evidence.
    //   The S63-adverse pattern from VWR USTEC (S71 Phase 3) and UTF5m
    //   USTEC (S73 Phase 3) is the prior; the XTF trio sweep is the test
    //   of whether the pattern generalises to XAU trend-follow at higher
    //   timeframes, or whether XAU's tail shape flips the sign. Pre-commit
    //   pass criterion: aggregate PF >= 1.20 AND >=3/4 windows pass the
    //   per-window avg_pnl >= +0.001 threshold (same protocol as UTF5m S73).
    //
    //   LOSS_CUT_PCT  -- cut when adverse >= entry*pct/100. Phase 2 (cold-loss).
    //   BE_ARM_PCT    -- mfe % of entry that arms BE ratchet. Phase 1 (giveback).
    //   BE_BUFFER_PCT -- BE_CUT triggers when move <= entry*pct/100 after arm.
    //   Override via the backtest harness CLI for the sweep; set _PCT = 0.0
    //   to disable a phase entirely.
    double LOSS_CUT_PCT  = 0.0;
    double BE_ARM_PCT    = 0.0;
    double BE_BUFFER_PCT = 0.0;

    std::array<XauTf2hPos, kXauTf2hNumCells> pos{};

    // 2h-bar history (last 80; need 50 for Donchian N=50 + warmup)
    static constexpr int kBarHistory = 80;
    std::deque<XauTf2hSynthBar> bars_;

    XauTf2hSynthBar cur_2h_{};
    bool            cur_2h_open_ = false;

    // Indicators on 2h bars
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    static constexpr int kEmaPeriod = 20;
    double ema20_ = 0.0;
    bool   ema_initialised_ = false;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // S102: warmup entry guard. True while warmup_from_csv is running.
    // Blocks _fire_entry so indicator-priming bars don't open stale
    // positions at historical price levels. Without this, the first
    // entry per cell fires at e.g. $2,158 (Feb 2024 warmup data) and
    // then TP-exits at tp_px=$2,188 the instant the first live tick
    // arrives at $4,534 — producing phantom trades at wrong prices.
    bool warmup_active_ = false;

    void init() noexcept {
        bars_.clear();
        cur_2h_ = {};
        cur_2h_open_ = false;
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        ema20_ = 0.0;
        ema_initialised_ = false;
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
    //  One snapshot per ACTIVE cell, tagged "<base_engine>#<cellIdx>".
    //  Engine supports both sides -> side from is_long.
    // ============================================================
    void persist_save_all(const char* base_engine, const char* sym,
                          std::vector<omega::PositionSnapshot>& out) const {
        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            const auto& p = pos[ci];
            if (!p.active) continue;
            omega::PositionSnapshot ps;
            ps.engine   = std::string(base_engine) + "#" + std::to_string(ci);
            ps.symbol   = sym;
            ps.side     = p.is_long ? "LONG" : "SHORT";
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
        if (ci < 0 || ci >= kXauTf2hNumCells) return false;
        auto& p = pos[ci];
        if (p.active) return true;   // already holding -- don't double
        p.active        = true;
        p.is_long       = (ps.side == "LONG");
        p.entry_px      = ps.entry;
        p.sl_px         = ps.sl;
        p.tp_px         = ps.tp;
        p.entry_ts_ms   = ps.entry_ts * 1000;
        p.bars_held     = 0;
        p.mfe           = 0.0;
        p.mae           = 0.0;
        p.size_mult     = 1.0;
        return true;
    }

    // ============================================================
    //  on_h1_bar -- aggregates 2 H1 bars into 1 H2 bar by even-hour
    //               UTC bucket, then fires when H2 closes.
    // ============================================================
    void on_h1_bar(const XauTf2hBar& bar,
                   double bid, double ask,
                   int64_t now_ms,
                   OnCloseFn on_close) noexcept {
        if (!enabled) return;

        long long bucket = (bar.bar_start_ms / 7200000LL) * 7200000LL;  // 2h = 7200s

        if (!cur_2h_open_) {
            cur_2h_ = {};
            cur_2h_.bucket_ms = bucket;
            cur_2h_.open  = bar.open;
            cur_2h_.high  = bar.high;
            cur_2h_.low   = bar.low;
            cur_2h_.close = bar.close;
            cur_2h_.h1_count = 1;
            cur_2h_open_ = true;
            return;
        }

        if (bucket != cur_2h_.bucket_ms) {
            // 2h bar closed: finalise + evaluate
            _finalise_2h_and_evaluate(bid, ask, now_ms, on_close);
            // Start new 2h with current H1
            cur_2h_ = {};
            cur_2h_.bucket_ms = bucket;
            cur_2h_.open  = bar.open;
            cur_2h_.high  = bar.high;
            cur_2h_.low   = bar.low;
            cur_2h_.close = bar.close;
            cur_2h_.h1_count = 1;
            cur_2h_open_ = true;
            return;
        }

        // Same 2h bucket: extend current bar
        if (bar.high > cur_2h_.high) cur_2h_.high = bar.high;
        if (bar.low  < cur_2h_.low)  cur_2h_.low  = bar.low;
        cur_2h_.close = bar.close;
        ++cur_2h_.h1_count;
    }

    // Per-tick exit management
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    void _finalise_2h_and_evaluate(double bid, double ask,
                                   int64_t now_ms,
                                   OnCloseFn on_close) noexcept {
        if (!cur_2h_open_) return;
        bars_.push_back(cur_2h_);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        // Feed the GoldTrendMimicLadder legs on the NATIVE 2h close cadence (the cadence the
        // mimic was backtested on). One-way; no-op if the "XauTf2h" book/tag isn't registered.
        omega::gold_trend_mimic().on_bar("XauTf2h", cur_2h_.high, cur_2h_.low, cur_2h_.close, now_ms / 1000);

        _update_atr14();
        _update_ema20();
        _update_adx14();  // S88-followup

        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        if ((int)bars_.size() < 52) return;   // need 50 for Donchian + warmup
        if (atr14_ <= 0.0) return;
        if (ask - bid > max_spread) return;

        // S88-followup: rolling ATR window for vol-band gate.
        if (use_vol_band_gate && atr14_ > 0.0) {
            atr_vol_window_.push_back(atr14_);
            if ((int)atr_vol_window_.size() > 200) atr_vol_window_.pop_front();
        }

        for (int ci = 0; ci < kXauTf2hNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            // S88-followup vol-band gate (per-cell mask)
            if (use_vol_band_gate && (cell_vol_band_mask & (1u << ci))
                && (int)atr_vol_window_.size() >= 200) {
                int below = 0;
                const int n = (int)atr_vol_window_.size();
                for (int i = 0; i < n; ++i) if (atr_vol_window_[i] < atr14_) ++below;
                const double pct = static_cast<double>(below) / n;
                if (pct < vol_band_low_pct || pct > vol_band_high_pct) {
                    continue;  // VOL_BAND_OUT
                }
            }
            // S88-followup ADX gate (per-cell mask)
            if (use_adx_gate && (cell_adx_mask & (1u << ci))
                && adx_dx_count_ >= kAdxPeriod && adx14_ < adx_min) {
                continue;  // ADX_TOO_WEAK
            }
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void _update_atr14() noexcept {
        if (bars_.size() < 2) { atr14_ = 0.0; return; }
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

    void _update_ema20() noexcept {
        if (bars_.empty()) return;
        double c = bars_.back().close;
        if (!ema_initialised_) { ema20_ = c; ema_initialised_ = true; }
        else {
            double a = 2.0 / (kEmaPeriod + 1);
            ema20_ = a * c + (1.0 - a) * ema20_;
        }
    }

    // S88-followup: Wilder ADX14 update (mirror of XauTrendFollow4hEngine).
    // Maintains running ATR / +DM / -DM sums + Wilder-smoothed DX -> ADX.
    void _update_adx14() noexcept {
        if (bars_.size() < 2) return;
        const auto& cur  = bars_[bars_.size() - 1];
        const auto& prev = bars_[bars_.size() - 2];
        double up = cur.high - prev.high;
        double dn = prev.low - cur.low;
        double pdm = (up > dn && up > 0) ? up : 0.0;
        double mdm = (dn > up && dn > 0) ? dn : 0.0;
        double tr  = std::max(cur.high - cur.low,
                              std::max(std::abs(cur.high - prev.close),
                                       std::abs(cur.low  - prev.close)));
        if (adx_warmup_count_ < kAdxPeriod) {
            adx_atr_sum_ += tr;
            adx_pdm_sum_ += pdm;
            adx_mdm_sum_ += mdm;
            ++adx_warmup_count_;
        } else {
            adx_atr_sum_ = adx_atr_sum_ - adx_atr_sum_ / kAdxPeriod + tr;
            adx_pdm_sum_ = adx_pdm_sum_ - adx_pdm_sum_ / kAdxPeriod + pdm;
            adx_mdm_sum_ = adx_mdm_sum_ - adx_mdm_sum_ / kAdxPeriod + mdm;
        }
        if (adx_warmup_count_ >= kAdxPeriod && adx_atr_sum_ > 1e-12) {
            double pdi = 100.0 * adx_pdm_sum_ / adx_atr_sum_;
            double mdi = 100.0 * adx_mdm_sum_ / adx_atr_sum_;
            double sum = pdi + mdi;
            double dx  = (sum > 1e-12) ? 100.0 * std::abs(pdi - mdi) / sum : 0.0;
            if (adx_dx_count_ < kAdxPeriod) {
                adx_dx_sum_ += dx;
                ++adx_dx_count_;
                if (adx_dx_count_ == kAdxPeriod) adx14_ = adx_dx_sum_ / kAdxPeriod;
            } else {
                adx14_ = (adx14_ * (kAdxPeriod - 1) + dx) / kAdxPeriod;
            }
        }
    }

    int _evaluate_signal(int ci) const noexcept {
        switch (kXauTf2hCells[ci].family) {
            case XauTf2hFamily::Keltner20:  return _sig_keltner();
            case XauTf2hFamily::Donchian20: return _sig_donchian(20);
            case XauTf2hFamily::Donchian50: return _sig_donchian(50);
            case XauTf2hFamily::InsideBar:  return _sig_inside_bar();
        }
        return 0;
    }

    int _sig_keltner() const noexcept {
        if (!ema_initialised_ || atr14_ <= 0.0) return 0;
        if (bars_.size() < 22) return 0;
        const auto& cur = bars_.back();
        double up = ema20_ + 2.0 * atr14_;
        double lo = ema20_ - 2.0 * atr14_;
        if (cur.close > up) return +1;
        if (cur.close < lo) return -1;
        return 0;
    }

    int _sig_donchian(int N) const noexcept {
        if ((int)bars_.size() < N + 1) return 0;
        const int last = (int)bars_.size() - 1;
        double hi = bars_[last - N].high, lo = bars_[last - N].low;
        for (int k = last - N + 1; k <= last - 1; ++k) {
            if (bars_[k].high > hi) hi = bars_[k].high;
            if (bars_[k].low  < lo) lo = bars_[k].low;
        }
        if (bars_[last].close > hi) return +1;
        if (bars_[last].close < lo) return -1;
        return 0;
    }

    int _sig_inside_bar() const noexcept {
        if (bars_.size() < 3) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& a = bars_[last - 2];
        const auto& b = bars_[last - 1];
        const auto& c = bars_[last];
        if (!(b.high < a.high && b.low > a.low)) return 0;
        if (c.close > b.high) return +1;
        if (c.close < b.low)  return -1;
        return 0;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        // S102: block entries during warmup — warmup primes indicators only.
        if (warmup_active_) return;
        // 2026-05-21: D1 EMA200 regime gate (chokepoint -- ALL cells gated here).
        // Added after -$52.31 InsideBar SHORT loss in gold uptrend 2026-05-20.
        // Suppresses entries against the daily trend across all 4 cells
        // (Keltner / Donchian20 / Donchian50 / InsideBar).
        if (side > 0 && !omega::gold_d1_trend().long_allowed())  return;
        if (side < 0 && !omega::gold_d1_trend().short_allowed()) return;
        // 2026-06-21: macro-hostile long-block (BearProtect coverage). Fail-safe.
        if (side > 0 && omega::gold_regime().long_blocked()) return;
        const auto& cfg = kXauTf2hCells[ci];
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;
        double sl_dist = cfg.sl_mult * atr14_;
        double tp_dist = cfg.tp_mult * atr14_;
        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        // S-2026-07-11 GOLD PHASE 1: gate on ledger_symbol (spot instances =
        // "XAUUSD" row, unchanged; MGC venue instances = the explicit MGC row
        // instead of the ~10x-misscaled spot proxy).
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    ledger_symbol.c_str(), spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }
        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr14_;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        // S34 P1 fix #5: reset MFE/MAE per new entry.
        p.mfe           = 0.0;
        p.mae           = 0.0;
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
        // GoldTrendMimicLadder (S-2026-07-10): one-way fire-and-forget — spawn INDEPENDENT
        // mimic legs at this entry. No-op if the "XauTf2h" book/tag isn't registered. Never
        // reads/moves/closes this position (additive, judged STANDALONE). Validated: legs
        // T gb8 +76.9%/W gb30 +88.5% (arm0.25/lc1.0/cap24/be0.15), WF both halves + ,
        // both REAL regimes + (bull+51.6/bear+25.2 T; bull+55.2/bear+33.3 W over 2022 bear + bull).
        omega::gold_trend_mimic().on_trend_open("XauTf2h", p.is_long ? 1 : -1, p.entry_px, p.entry_ts_ms / 1000);
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];

        // S34 P1 fix #5: update MFE/MAE on every tick using mid price.
        const double mid = (bid + ask) * 0.5;
        const double favourable = p.is_long ? (mid - p.entry_px)
                                            : (p.entry_px - mid);
        if (favourable > p.mfe) p.mfe = favourable;
        if (-favourable > p.mae) p.mae = -favourable;

        // S63 2026-05-14 (part W): VWR-pattern in-flight protection. Runs
        //   BEFORE the SL/TP exit check so cold-loss/giveback cuts take
        //   priority. Mirrors IndexMacroCrashEngine pattern at
        //   IndexFlowEngine.hpp:1123-1150. Both phases gated on their _PCT
        //   field being > 0.0 so defaulting all three to 0.0 (state B) is
        //   a structural no-op.
        // Phase 1: BE_RATCHET -- after mfe crosses arm threshold, exit at BE
        //          if current move falls back to within buffer.
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && p.entry_px > 0.0) {
            const double arm_pts    = p.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = p.entry_px * BE_BUFFER_PCT / 100.0;
            if (p.mfe >= arm_pts && favourable <= buffer_pts) {
                const double exit_px = p.is_long ? bid : ask;
                _close(ci, exit_px, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        // Phase 2: LOSS_CUT -- cold-loss cut for trades that go straight
        //          adverse without ever reaching the ATR-based SL.
        if (LOSS_CUT_PCT > 0.0 && p.entry_px > 0.0) {
            const double adverse       = -favourable;
            const double loss_cut_dist = p.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = p.is_long ? bid : ask;
                _close(ci, exit_px, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0;
        if (p.is_long) {
            if (bid <= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px) { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;
        _close(ci, xp, hit_tp ? "TP_HIT" : "SL_HIT", now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        // S34 P1 fix #1: pts_move signed against direction so profit is +ve.
        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);

        omega::TradeRecord tr;
        tr.symbol     = ledger_symbol;
        // S34 P1 fix #3: per-cell engine string.
        tr.engine     = ledger_prefix + kXauTf2hCells[ci].name;
        // S34 P1 fix #4: LONG/SHORT, not BUY/SELL.
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kXauTf2hCells[ci].name;
        tr.shadow     = shadow_mode;
        // S34 P1 fix #1: gross pnl in price-points*lot; USD via tick_mult.
        tr.pnl        = pts_move * lot * p.size_mult;   // S-2026-06-03: regression-slope sizing overlay
        // S34 P1 fix #5: propagate MFE/MAE.
        tr.mfe        = p.mfe;
        tr.mae        = p.mae;

        if (on_close) on_close(tr);

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }

public:
    // S101: warmup from pre-built H1 bar CSV. Engine synthesizes 2h bars
    //   internally from H1 input so we feed H1 bars, same as live path.
    std::string warmup_csv_path;

    // S-2026-07-07 MGC venue port: a second instance of this class runs on the
    // MGC futures feed (MgcFastDonchianFeed.hpp). These give that instance a
    // distinct ledger tag + symbol; defaults keep the spot instance byte-identical.
    std::string ledger_prefix = "XauTrendFollow2h_";
    std::string ledger_symbol = "XAUUSD";

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[XauTF2h-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[XauTF2h-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[XauTF2h-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
        warmup_active_ = true;  // S102: block entries during indicator priming

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
            // ts may be seconds or milliseconds depending on which writer last touched
            // the CSV (repo ships ms; the VPS seed_refresh write_mgc_hist regenerates in
            // SECONDS + volume col and clobbers it). Seconds ts fed raw collapses every
            // bar into one 2h bucket (atr=0, bars_2h=1) -- the 2026-07-07 poisoned-warmup
            // deploy. Normalise here so both formats are safe.
            if (ms > 0 && ms < 100000000000LL) ms *= 1000LL;

            XauTf2hBar bar; bar.bar_start_ms = ms; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
            on_h1_bar(bar, c, c, ms + 3600LL*1000, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = false;  // S102: live entries now permitted
        printf("[XauTF2h-WARMUP] fed=%d H1 bars, atr=%.4f bars_2h=%d path='%s'\n",
               fed, atr14_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega
