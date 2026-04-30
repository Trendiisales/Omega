// =============================================================================
//  CellPrimitives.hpp -- Phase 1 of CellEngine refactor.
//
//  Source of truth: docs/CELL_ENGINE_REFACTOR_PLAN.md (APPROVED 2026-04-30).
//  Status:          Phase 1 -- ADDITIVE ONLY. No behaviour change.
//
//  This header defines the canonical shared types used by the four cell-style
//  engines (Tsmom, Donchian, EmaPullback, TrendRider). At Phase 1 each engine
//  still keeps its own private XBar / XBarSynth / XATR14 types; this header
//  is purely additive. Each engine adds a static_assert at file scope to
//  confirm its private type is layout-compatible with the canonical type
//  defined here -- the assertions are the structural-sanity gate that keeps
//  Phase 2 honest.
//
//  Phase 2 will introduce CellEngine.hpp (CellBase<Strategy> +
//  CellPortfolio<Strategy>) and start migrating engines to use these
//  canonical types directly. See plan §4 for the full migration sequence.
//
//  Naming convention: namespace omega::cell. Use this prefix everywhere to
//  keep the new types from clashing with the existing per-engine types
//  during the parallel-shadow window in Phase 2-3.
//
//  Standard required: C++20 (uses nested namespace + designated initialiser
//  patterns elsewhere in the refactor; the file itself compiles cleanly at
//  C++17).
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace omega::cell {

// =============================================================================
//  Bar -- canonical OHLC bar.
//
//  Layout (8-byte aligned, 40 bytes total):
//      offset  0  int64_t bar_start_ms
//      offset  8  double  open
//      offset 16  double  high
//      offset 24  double  low
//      offset 32  double  close
//
//  Identical to TsmomBar / DonchianBar / EpbBar / TrBar. The four engine
//  headers static_assert this layout match.
// =============================================================================
struct Bar {
    int64_t bar_start_ms = 0;
    double  open         = 0.0;
    double  high         = 0.0;
    double  low          = 0.0;
    double  close        = 0.0;
};

// =============================================================================
//  BarSynth -- aggregates N consecutive H1 bars into one parent-TF bar
//  (H2 stride=2, H4 stride=4, H6 stride=6, D1 stride=24).
//
//  Identical aggregation logic to TsmomBarSynth / DonchianBarSynth /
//  EpbBarSynth / TrBarSynth. The first H1 of a parent group seeds the
//  parent bar's open + bar_start_ms; subsequent H1s extend high/low and
//  overwrite close. On reaching `stride`, the parent bar is emitted.
// =============================================================================
struct BarSynth {
    int  stride       = 1;
    int  accum_count_ = 0;
    Bar  cur_{};
    using EmitCallback = std::function<void(const Bar&)>;

    void on_h1_bar(const Bar& h1, EmitCallback emit) noexcept {
        if (accum_count_ == 0) {
            cur_              = h1;
            cur_.bar_start_ms = h1.bar_start_ms;
            cur_.open         = h1.open;
        } else {
            cur_.high  = std::max(cur_.high, h1.high);
            cur_.low   = std::min(cur_.low,  h1.low);
            cur_.close = h1.close;
        }
        accum_count_++;
        if (accum_count_ >= stride) {
            if (emit) emit(cur_);
            accum_count_ = 0;
            cur_         = Bar{};
        }
    }
};

// =============================================================================
//  ATR14 -- Wilder's Average True Range with N=14.
//
//  Identical math to TsmomATR14 / DonchianATR14 / EpbATR14 / TrATR14:
//      tr_t   = max(high-low, |high - prev_close|, |low - prev_close|)
//      atr_0..13   = simple running mean of tr
//      atr_14+     = Wilder smoothing: atr = (atr * 13 + tr) / 14
//
//  ready() returns true once 14 bars have been observed.
// =============================================================================
struct ATR14 {
    static constexpr int ATR_P = 14;
    bool   has_prev_close_ = false;
    double prev_close_     = 0.0;
    double atr_            = 0.0;
    int    n_              = 0;

    bool ready() const noexcept { return n_ >= ATR_P; }

    void on_bar(const Bar& b) noexcept {
        const double tr = has_prev_close_
            ? std::max({ b.high - b.low,
                         std::fabs(b.high - prev_close_),
                         std::fabs(b.low  - prev_close_) })
            : (b.high - b.low);
        if (n_ < ATR_P) { atr_ = (atr_ * n_ + tr) / (n_ + 1); n_++; }
        else            { atr_ = (atr_ * (ATR_P - 1) + tr) / ATR_P; }
        prev_close_     = b.close;
        has_prev_close_ = true;
    }

    double value() const noexcept { return atr_; }
};

// =============================================================================
//  EMA -- pandas.ewm(span=N, adjust=False) reproduced.
//
//  alpha = 2 / (span + 1).
//  First on_close: ema = x_0.
//  Subsequent:     ema_t = alpha * x_t + (1 - alpha) * ema_{t-1}.
//
//  Identical to EpbEMA. Parameterised at runtime via init(span). EmaPullback
//  uses fast_span=9 and slow_span=21; nothing in the design forces those
//  particular values, so this struct keeps span as a settable field.
// =============================================================================
struct EMA {
    int    span   = 9;
    double alpha  = 0.2;       // 2/(span+1) -- recomputed in init()
    bool   has_   = false;
    double value_ = 0.0;
    double prev_  = 0.0;       // ema value BEFORE the most recent on_close

    void init(int s) noexcept {
        span   = s;
        alpha  = 2.0 / (static_cast<double>(s) + 1.0);
        has_   = false;
        value_ = 0.0;
        prev_  = 0.0;
    }

    void on_close(double x) noexcept {
        prev_ = value_;
        if (!has_) { value_ = x; has_ = true; }
        else       { value_ = alpha * x + (1.0 - alpha) * value_; }
    }

    bool   has()     const noexcept { return has_; }
    double value()   const noexcept { return value_; }
    double prev()    const noexcept { return prev_; }
    bool   rising()  const noexcept { return has_ && value_ > prev_; }
    bool   falling() const noexcept { return has_ && value_ < prev_; }
};

// =============================================================================
//  Position -- canonical multi-position state.
//
//  Layout intentionally DIVERGES from TsmomCell::Position by adding two
//  fields needed by Phase 3 strategies:
//
//    initial_sl  -- snapshot of SL at entry. TrendRiderStrategy needs this
//                   to compute R-multiple after the SL has trailed; you
//                   cannot recover initial risk from the live `sl` field
//                   once stage trail has moved it.
//
//    tp          -- take-profit price. DonchianStrategy + EpbStrategy use
//                   a TP; TsmomStrategy + TrendRiderStrategy do not. By
//                   convention 0.0 means "no TP" (the existing scalar
//                   pos_tp_ in Donchian/Epb cells already uses this
//                   convention; TsmomCell::Position has no TP at all).
//
//  Phase 1 does NOT use this struct -- it is defined here purely as
//  forward-declared shape for Phase 2 review. The four existing engines'
//  in-cell position storage is unchanged in Phase 1.
//
//  Layout (8-byte aligned, 96 bytes total):
//      offset  0  double  entry
//      offset  8  double  sl
//      offset 16  double  initial_sl
//      offset 24  double  tp
//      offset 32  double  size
//      offset 40  double  atr
//      offset 48  int64_t entry_ms
//      offset 56  int     bars_held    (+ 4 byte pad)
//      offset 64  double  mfe
//      offset 72  double  mae
//      offset 80  double  spread_at
//      offset 88  int     id           (+ 4 byte pad)
// =============================================================================
struct Position {
    double  entry      = 0.0;
    double  sl         = 0.0;
    double  initial_sl = 0.0;
    double  tp         = 0.0;
    double  size       = 0.0;
    double  atr        = 0.0;
    int64_t entry_ms   = 0;
    int     bars_held  = 0;
    double  mfe        = 0.0;
    double  mae        = 0.0;
    double  spread_at  = 0.0;
    int     id         = 0;
};

// =============================================================================
//  SizingParams -- returned by Strategy::sizing(cfg) in Phase 2+.
//
//  Phase 1 defines this for forward-compatibility only; not yet referenced
//  by any engine. Per plan §7.3 (sizing decision), each Strategy returns
//  hybrid values: hardcoded DEFAULT_RISK_PCT / DEFAULT_MAX_LOT_CAP unless
//  the per-cell config supplies a positive override.
//
//  Concrete defaults locked in plan §7.3:
//      Tsmom        risk_pct=0.005  max_lot_cap=0.05
//      Donchian     risk_pct=0.005  max_lot_cap=0.05
//      EmaPullback  risk_pct=0.005  max_lot_cap=0.05
//      TrendRider   risk_pct=0.040  max_lot_cap=0.50  (Option B)
// =============================================================================
struct SizingParams {
    double risk_pct;
    double max_lot_cap;
};

}  // namespace omega::cell
