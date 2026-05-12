#pragma once
// =============================================================================
//  UstecTrendFollowHtfEngine.hpp -- USTEC.F multi-timeframe trend-follow
//                                   (S35-P6 2026-05-12)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-12 from edge_hunt.cpp + a 3-period intersection test on
//  16 months of NSXUSD HISTDATA tick data (Jan 2025 -> Apr 2026, 89.6M ticks).
//  Periods: 2025H1, 2025H2, 2026 partial. Cells included here are positive
//  in EVERY one of those 3 periods (after $0.06 RT cost, realistic bid/ask
//  fills, 0.1 lot).
//
//  This is the "stable across regimes" companion to UstecTrendFollow5mEngine,
//  which ships M5 cells (Donchian N=20 + Keltner K=2.0) validated only on a
//  15-day L2 sample. The 16-month re-test on HISTDATA shows those M5 cells
//  do NOT survive the 3-period intersection test (M5 Donchian was negative
//  in all 3 periods; M5 Keltner was negative in 2025H1 by -$3329). The
//  operator is aware; the M5 engine has not been touched here, only flagged
//  in the [OMEGA-INIT] line of this engine (see engine_init.hpp).
//
//  CELLS (3 cells; each independently positive in 2025H1, 2025H2, 2026)
//
//      [A] 2h  InsideBar              sl1.5_tp3.0
//          n=74/75/33   net=+5894/+1972/+3407   minP=+1972  ★ top cell
//      [C] 1h  ATR_Mom mom=50;atr_band=0.2-0.8 sl2.0_tp4.0
//          n=101/106/52 net=+2110/+2441/+924    minP=+924   ★ strongest wrapped
//      [E] 4h  Stochastic lo=20;hi=80 sl2.0_tp4.0
//          n=29/30/13   net=+965/+3625/+1643    minP=+965
//
//  Sum across cells, worst-period basis: ~$3.9K per period after cost.
//
//  S36-P1a (2026-05-12) DROPPED CELLS
//      [B] 1h  Stochastic lo=20;hi=80 sl2.0_tp4.0
//          Bare cell minP=+1599 BUT wrapped backtest: 2025H1=-$1115,
//          2025H2=+$260, 2026=+$2920 (ALL=+$2065). Per operator directive
//          (drop any cell unprofitable in ANY period), removed.
//      [D] 15m Donchian N=20          sl2.0_tp4.0
//          Bare cell positive in all 3 periods; wrapped flipped to -$4002
//          across ALL (BE+trail clips winners while letting full-SL losers
//          through). Per operator directive, removed.
//
//  POST-DROP PROJECTION (5-cell ALL +$11,733 minus the two dropped cells):
//      +$11,733 - (-$4,002) - (+$2,065) = +$13,670 across same 16mo data.
//      Re-backtest under S36-P1a-verify is the next confirmation step.
//
//  CAVEAT: cells were chosen for FAMILY DIVERSITY. Other Stochastic variants
//  (2h sl1.5, 2h sl2.0) survived the intersection test and may be revisited
//  in S36-P3 if the 3-cell ensemble proves too sparse on live shadow tape.
//
//  SAFETY (ProtectedEngineGuards bundle)
//      shadow_mode = true (HARD shadow until operator validates shadow-live)
//      enabled     = true (engine runs in shadow)
//      lot         = 0.1   (USTEC.F pt_size = 0.1 -> $20/pt -> $2/pt @ 0.1 lot)
//      max_spread  = 5.0
//      Spread cap, ATR floor, BE arm, trail-after-BE come from
//      include/engine_protections.hpp -- same bundle XauThreeBar30m uses.
//
//      Daily-loss-cap and consec-loss killswitch are DISABLED by default in
//      engine_init.hpp (per the S35-P4 finding that strict thresholds trip
//      prematurely on 35-50% native-WR strategies). Re-enable selectively
//      after collecting 1+ months of shadow-live trace.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::UstecTrendFollowHtfEngine g_ustec_tf_htf;
//
//      // engine_init.hpp:
//      g_ustec_tf_htf.shadow_mode      = true;
//      g_ustec_tf_htf.enabled          = true;
//      g_ustec_tf_htf.lot              = 0.1;
//      g_ustec_tf_htf.max_spread       = 5.0;
//      g_ustec_tf_htf.be_trigger_atr   = 1.0;
//      g_ustec_tf_htf.be_cost_buffer_pts = 0.50;
//      g_ustec_tf_htf.trail_after_be   = true;
//      g_ustec_tf_htf.trail_atr_mult   = 0.75;
//      g_ustec_tf_htf.min_atr_floor    = 5.0;
//      g_ustec_tf_htf.init();
//
//      // tick_indices.hpp -- USTEC.F bar-builder, M15 close branch:
//      omega::UstecTfHtfBar bar15m{};
//      bar15m.bar_start_ms = s_nq15_start;
//      bar15m.open  = s_nq15.open;
//      bar15m.high  = s_nq15.high;
//      bar15m.low   = s_nq15.low;
//      bar15m.close = s_nq15.close;
//      g_ustec_tf_htf.on_15m_bar(bar15m, bid, ask, /*atr15m_external=*/0.0,
//                                now_ms, ca_on_close);
//
//      // tick_indices.hpp -- every USTEC.F tick:
//      g_ustec_tf_htf.on_tick(bid, ask, now_ms, ca_on_close);
//
//  ARCHITECTURE
//
//  Single-base-TF dispatch: tick_indices.hpp feeds M15 bars. The engine
//  internally synthesizes H1 (4 M15), H2 (8 M15), and H4 (16 M15) bars
//  by aggregating consecutive M15s within each higher-TF window. The first
//  M15 bar inside a new H1/H2/H4 window opens the synthesized bar; each
//  subsequent M15 within the same window updates high/low/close. When the
//  M15 bar that completes the window arrives, the synthesized bar is
//  pushed onto its TF's deque and cell evaluation runs for that TF.
//
//  This is the same synthesis pattern used by XauTrendFollow2hEngine
//  (H1->H2) and XauTrendFollowD1Engine (H4->D1).
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "engine_protections.hpp"
#include "OmegaTradeLedger.hpp"

namespace omega {

// ----------------------------------------------------------------------------
//  Bar structure (the M15 input bar -- higher TFs are synthesized)
// ----------------------------------------------------------------------------
struct UstecTfHtfBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ----------------------------------------------------------------------------
//  Per-cell open position
// ----------------------------------------------------------------------------
struct UstecTfHtfPos {
    bool        active        = false;
    bool        is_long       = false;
    double      entry_px      = 0.0;
    double      sl_px         = 0.0;
    double      tp_px         = 0.0;
    double      atr_at_entry  = 0.0;
    int64_t     entry_ts_ms   = 0;
    int         bars_held     = 0;
    int         cooldown_bars = 0;
    double      mfe_pts       = 0.0;
    double      mae_pts       = 0.0;
    bool        be_armed      = false;   // mirrored from guards for ledger
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ----------------------------------------------------------------------------
//  Cell catalogue
// ----------------------------------------------------------------------------
enum class UstecTfHtfFamily {
    InsideBar_2h,                    // [A]
    AtrMom_1h_50,                    // [C]   (S36-P1a: [B] Stochastic_1h_2080 dropped)
    Stochastic_4h_2080,              // [E]   (S36-P1a: [D] Donchian20_15m dropped)
};

enum class UstecTfHtfTf { M15, H1, H2, H4 };

struct UstecTfHtfCellConfig {
    UstecTfHtfFamily family;
    UstecTfHtfTf     tf;
    double           sl_mult;
    double           tp_mult;
    const char*      name;        // full descriptor for tr.regime
    const char*      short_name;  // distinct tag in tr.engine
};

static constexpr UstecTfHtfCellConfig kUstecTfHtfCells[] = {
    { UstecTfHtfFamily::InsideBar_2h,        UstecTfHtfTf::H2,  1.5, 3.0,
      "InsideBar_2h_sl1.5tp3.0",        "InsideBar2h"  },
    { UstecTfHtfFamily::AtrMom_1h_50,        UstecTfHtfTf::H1,  2.0, 4.0,
      "ATR_Mom_1h_m50_sl2.0tp4.0",      "AtrMom1h"     },
    { UstecTfHtfFamily::Stochastic_4h_2080,  UstecTfHtfTf::H4,  2.0, 4.0,
      "Stochastic_4h_2080_sl2.0tp4.0",  "Stoch4h"      },
    // S36-P1a (2026-05-12) -- dropped two cells per operator directive
    // (any period unprofitable on wrapped backtest => drop):
    //   { UstecTfHtfFamily::Stochastic_1h_2080, ... }   (2025H1 wrapped: -$1115)
    //   { UstecTfHtfFamily::Donchian20_15m,     ... }   (ALL wrapped:    -$4002)
};
static constexpr int kUstecTfHtfNumCells =
    static_cast<int>(sizeof(kUstecTfHtfCells) / sizeof(kUstecTfHtfCells[0]));

// ----------------------------------------------------------------------------
//  Engine
// ----------------------------------------------------------------------------
struct UstecTrendFollowHtfEngine {
public:
    // Engine-owned knobs (forwarded into guards.cfg in init()).
    bool   shadow_mode       = true;
    bool   enabled           = false;
    double lot               = 0.1;
    double max_spread        = 5.0;

    // Protection knobs (init() forwards into guards.cfg).
    double be_trigger_atr      = 0.0;    // 0 disables BE arm
    double be_cost_buffer_pts  = 0.50;
    bool   trail_after_be      = false;
    double trail_atr_mult      = 0.0;
    double min_atr_floor       = 0.0;    // raw price points; per the smallest TF
    double max_atr_ceil        = 0.0;
    double daily_loss_limit    = 0.0;    // disabled by default
    int    max_consec_losses   = 0;      // disabled by default
    int    max_bars_held       = 0;      // disabled by default
    int    block_hour_start    = -1;
    int    block_hour_end      = -1;

    // Symbol metadata. USTEC.F maps to $20/pt in sizing.hpp's
    // tick_value_multiplier. We store the multiplier here so the engine
    // can compute its own daily_pnl_usd for guards.on_close().
    static constexpr const char* SYMBOL = "USTEC.F";
    static constexpr double USTEC_USD_PER_PT = 20.0;

    // ProtectionState for the engine as a whole (shared across cells).
    ProtectedEngineGuards guards;

    // Per-cell open positions.
    std::array<UstecTfHtfPos, kUstecTfHtfNumCells> pos{};

    // Per-TF bar histories (synthesized internally from M15).
    static constexpr int kHistM15 = 64;     // need 22+ for Donchian N=20
    static constexpr int kHistH1  = 200;    // need 152+ for ATR_Mom mom=50 with 100-bar percentile
    static constexpr int kHistH2  = 64;
    static constexpr int kHistH4  = 64;
    std::deque<UstecTfHtfBar> bars_m15_;
    std::deque<UstecTfHtfBar> bars_h1_;
    std::deque<UstecTfHtfBar> bars_h2_;
    std::deque<UstecTfHtfBar> bars_h4_;

    // Per-TF Wilder ATR14 (local fallback; engines may pass external ATR
    // for the M15 TF, in which case it's used; H1/H2/H4 are always local).
    static constexpr int kAtrPeriod = 14;
    double atr14_m15_ = 0.0;
    double atr14_h1_  = 0.0;
    double atr14_h2_  = 0.0;
    double atr14_h4_  = 0.0;
    int    atr_warmup_m15_ = 0;
    int    atr_warmup_h1_  = 0;
    int    atr_warmup_h2_  = 0;
    int    atr_warmup_h4_  = 0;

    // Synthesized higher-TF bar accumulators.
    UstecTfHtfBar  cur_h1_{};   bool   h1_open_ = false;
    UstecTfHtfBar  cur_h2_{};   bool   h2_open_ = false;
    UstecTfHtfBar  cur_h4_{};   bool   h4_open_ = false;
    int            cur_h1_n_   = 0;
    int            cur_h2_n_   = 0;
    int            cur_h4_n_   = 0;
    static constexpr int kM15PerH1 = 4;
    static constexpr int kM15PerH2 = 8;
    static constexpr int kM15PerH4 = 16;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_m15_.clear(); bars_h1_.clear();
        bars_h2_.clear();  bars_h4_.clear();
        atr14_m15_ = atr14_h1_ = atr14_h2_ = atr14_h4_ = 0.0;
        atr_warmup_m15_ = atr_warmup_h1_ = atr_warmup_h2_ = atr_warmup_h4_ = 0;
        cur_h1_ = cur_h2_ = cur_h4_ = {};
        h1_open_ = h2_open_ = h4_open_ = false;
        cur_h1_n_ = cur_h2_n_ = cur_h4_n_ = 0;
        for (auto& p : pos) p = {};

        guards.cfg.sl_atr_mult         = 2.0;   // baseline; cells override per-trade
        guards.cfg.max_bars_held       = max_bars_held;
        guards.cfg.be_trigger_atr      = be_trigger_atr;
        guards.cfg.be_cost_buffer_pts  = be_cost_buffer_pts;
        guards.cfg.trail_after_be      = trail_after_be;
        guards.cfg.trail_atr_mult      = trail_atr_mult;
        guards.cfg.daily_loss_limit    = daily_loss_limit;
        guards.cfg.max_consec_losses   = max_consec_losses;
        guards.cfg.min_atr_floor       = min_atr_floor;
        guards.cfg.max_atr_ceil        = max_atr_ceil;
        guards.cfg.max_spread          = max_spread;
        guards.cfg.block_hour_start    = block_hour_start;
        guards.cfg.block_hour_end      = block_hour_end;
        guards.reset_all();
    }

    bool has_open_position() const noexcept {
        for (const auto& p : pos) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos) if (p.active) ++n; return n;
    }

    // -------------------------------------------------------------------------
    //  on_15m_bar  -- called by tick_indices.hpp at every M15 close.
    //  Internally synthesizes H1/H2/H4 by accumulating consecutive M15s.
    // -------------------------------------------------------------------------
    void on_15m_bar(const UstecTfHtfBar& bar,
                    double bid, double ask,
                    double atr15m_external,
                    int64_t now_ms,
                    OnCloseFn on_close) noexcept
    {
        if (!enabled) return;

        // Roll the UTC-day index for daily-cap accounting.
        guards.roll_day(now_ms / 1000);

        // Append M15 bar + update its ATR.
        _push_bar(bars_m15_, bar, kHistM15);
        if (atr15m_external > 0.0) atr14_m15_ = atr15m_external;
        else _update_atr(bars_m15_, atr14_m15_, atr_warmup_m15_);

        // Synthesize and possibly close H1.
        bool h1_just_closed = _ingest_into_synth(cur_h1_, h1_open_, cur_h1_n_,
                                                  bar, kM15PerH1);
        if (h1_just_closed) {
            _push_bar(bars_h1_, cur_h1_, kHistH1);
            _update_atr(bars_h1_, atr14_h1_, atr_warmup_h1_);
            cur_h1_n_ = 0; h1_open_ = false;
        }

        // Synthesize and possibly close H2.
        bool h2_just_closed = _ingest_into_synth(cur_h2_, h2_open_, cur_h2_n_,
                                                  bar, kM15PerH2);
        if (h2_just_closed) {
            _push_bar(bars_h2_, cur_h2_, kHistH2);
            _update_atr(bars_h2_, atr14_h2_, atr_warmup_h2_);
            cur_h2_n_ = 0; h2_open_ = false;
        }

        // Synthesize and possibly close H4.
        bool h4_just_closed = _ingest_into_synth(cur_h4_, h4_open_, cur_h4_n_,
                                                  bar, kM15PerH4);
        if (h4_just_closed) {
            _push_bar(bars_h4_, cur_h4_, kHistH4);
            _update_atr(bars_h4_, atr14_h4_, atr_warmup_h4_);
            cur_h4_n_ = 0; h4_open_ = false;
        }

        // Per-cell bar accounting (cooldown + bars_held).
        for (auto& p : pos) {
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) ++p.bars_held;
        }

        // Entry guards (spread, ATR regime, daily cap, killswitch, session).
        // The smallest TF (M15) ATR is the meaningful "regime" filter.
        if (const char* why = guards.check_entry_ok(bid, ask, atr14_m15_,
                                                     now_ms / 1000)) {
            log_entry_block("UstecTrendFollowHtf", why);
            return;
        }

        // Try each cell. M15 cells fire on every M15 bar; H1/H2/H4 cells
        // only fire when their TF window just closed.
        for (int ci = 0; ci < kUstecTfHtfNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            const auto& cfg = kUstecTfHtfCells[ci];
            // Skip cells whose TF didn't tick this call.
            if (cfg.tf == UstecTfHtfTf::H1  && !h1_just_closed) continue;
            if (cfg.tf == UstecTfHtfTf::H2  && !h2_just_closed) continue;
            if (cfg.tf == UstecTfHtfTf::H4  && !h4_just_closed) continue;
            // (M15 cells always tick.)

            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;  // exits run intra-bar in on_tick
    }

    // -------------------------------------------------------------------------
    //  on_tick -- runs every USTEC.F tick to manage open positions.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept
    {
        if (!enabled) return;
        for (int ci = 0; ci < kUstecTfHtfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    // Force-close all open positions (operator command, weekend close, etc).
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept
    {
        for (int ci = 0; ci < kUstecTfHtfNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    // -------------------------------------------------------------------------
    //  Bar / ATR helpers
    // -------------------------------------------------------------------------
    void _push_bar(std::deque<UstecTfHtfBar>& dq, const UstecTfHtfBar& bar,
                    int max_size) noexcept {
        dq.push_back(bar);
        while ((int)dq.size() > max_size) dq.pop_front();
    }

    void _update_atr(const std::deque<UstecTfHtfBar>& dq,
                      double& atr14, int& warmup_count) noexcept {
        if ((int)dq.size() < 2) { atr14 = 0.0; return; }
        const auto& cur  = dq.back();
        const auto& prev = dq[dq.size() - 2];
        double tr = std::max(cur.high - cur.low,
                             std::max(std::abs(cur.high - prev.close),
                                      std::abs(cur.low  - prev.close)));
        if (warmup_count < kAtrPeriod) {
            atr14 = (atr14 * warmup_count + tr) / (warmup_count + 1);
            ++warmup_count;
        } else {
            atr14 = (atr14 * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    // Returns true if this M15 bar completes the synth window (i.e. the
    // synthesized bar is now ready to push onto its TF deque).
    bool _ingest_into_synth(UstecTfHtfBar& cur, bool& open, int& n,
                             const UstecTfHtfBar& m15, int m15_per_synth) noexcept {
        if (!open) {
            cur = m15;
            open = true;
            n = 1;
        } else {
            if (m15.high > cur.high) cur.high = m15.high;
            if (m15.low  < cur.low ) cur.low  = m15.low;
            cur.close = m15.close;
            ++n;
        }
        return (n >= m15_per_synth);
    }

    // -------------------------------------------------------------------------
    //  Indicator helpers (mid-based, mirroring edge_hunt.cpp definitions)
    // -------------------------------------------------------------------------
    static double _bar_mid_h(const UstecTfHtfBar& b) noexcept { return b.high; }
    static double _bar_mid_l(const UstecTfHtfBar& b) noexcept { return b.low;  }
    static double _bar_mid_c(const UstecTfHtfBar& b) noexcept { return b.close;}

    // Stochastic %K period 14, %D smoothing 3. Returns false if not enough
    // bars; otherwise fills cur_k/d and prev_k/d.
    //
    // Implementation note: to compute the "current" K (last bar) we need the
    // last N bars; to compute K[i-1] we need the bar before, etc. We need K
    // values for the last (N + M) positions to be able to smooth the last 2
    // D values. The minimum dq size is therefore N + M + N - 1 = 2*N + M - 1
    // bars (the earliest K position needs N bars BEFORE it). We use a
    // generous floor of 2*N + M = 31 to be safe.
    bool _compute_stoch(const std::deque<UstecTfHtfBar>& dq,
                        double& cur_k, double& cur_d,
                        double& prev_k, double& prev_d) const noexcept {
        constexpr int N = 14, M = 3;
        constexpr int MIN_BARS = 2 * N + M;     // 31 bars minimum
        const int sz = (int)dq.size();
        if (sz < MIN_BARS) return false;

        // Compute K for last (N + M) bars (so we can build D series of length M).
        std::array<double, N + M + 4> k_buf{};
        const int k_start = sz - (N + M);    // first bar index for which we
                                              // compute K (and we have N bars
                                              // before it for the lookback).
        for (int i = 0; i < N + M; ++i) {
            const int idx = k_start + i;     // bar index in dq
            const int win_start = idx - N + 1;
            // Defensive guard (should never trigger given the size check above)
            if (win_start < 0 || idx >= sz) return false;
            double hh = _bar_mid_h(dq[win_start]);
            double ll = _bar_mid_l(dq[win_start]);
            for (int k = win_start + 1; k <= idx; ++k) {
                if (_bar_mid_h(dq[k]) > hh) hh = _bar_mid_h(dq[k]);
                if (_bar_mid_l(dq[k]) < ll) ll = _bar_mid_l(dq[k]);
            }
            const double rng = hh - ll;
            k_buf[i] = (rng > 1e-12) ? 100.0 * (_bar_mid_c(dq[idx]) - ll) / rng : 50.0;
        }

        // D[i] = SMA of k over last M, computed at last 2 indices.
        auto d_at = [&](int kbuf_idx) -> double {
            double s = 0.0;
            for (int j = kbuf_idx - M + 1; j <= kbuf_idx; ++j) s += k_buf[j];
            return s / M;
        };
        const int last_kb_idx = N + M - 1;
        cur_k  = k_buf[last_kb_idx];
        cur_d  = d_at(last_kb_idx);
        prev_k = k_buf[last_kb_idx - 1];
        prev_d = d_at(last_kb_idx - 1);
        return true;
    }

    // -------------------------------------------------------------------------
    //  Per-cell signal evaluators.  Return +1 long, -1 short, 0 none.
    // -------------------------------------------------------------------------
    int _evaluate_signal(int ci) const noexcept {
        switch (kUstecTfHtfCells[ci].family) {
            case UstecTfHtfFamily::InsideBar_2h:          return _sig_inside_bar_h2();
            case UstecTfHtfFamily::AtrMom_1h_50:          return _sig_atr_mom_h1();
            case UstecTfHtfFamily::Stochastic_4h_2080:    return _sig_stoch(bars_h4_);
            // S36-P1a -- dropped:
            //   case UstecTfHtfFamily::Stochastic_1h_2080: return _sig_stoch(bars_h1_);
            //   case UstecTfHtfFamily::Donchian20_15m:     return _sig_donchian_15m();
        }
        return 0;
    }

    // S36-P1a (2026-05-12) -- _sig_donchian_15m() removed along with the
    // Donchian20_15m cell. M15 history (bars_m15_, atr14_m15_) is retained
    // because atr14_m15_ is still used as the entry regime filter via
    // guards.check_entry_ok() and M15 is the base-TF feed that synthesizes
    // H1/H2/H4 bars internally.

    int _sig_inside_bar_h2() const noexcept {
        const int sz = (int)bars_h2_.size();
        if (sz < 3) return 0;
        const auto& a = bars_h2_[sz - 3];   // bar i-2
        const auto& b = bars_h2_[sz - 2];   // bar i-1 (the "inside" bar)
        const auto& cur = bars_h2_[sz - 1]; // bar i (the breakout candidate)
        if (!(b.high < a.high && b.low > a.low)) return 0;
        if (cur.close > b.high) return +1;
        if (cur.close < b.low ) return -1;
        return 0;
    }

    int _sig_stoch(const std::deque<UstecTfHtfBar>& dq) const noexcept {
        constexpr double LO = 20.0, HI = 80.0;
        double cur_k, cur_d, prev_k, prev_d;
        if (!_compute_stoch(dq, cur_k, cur_d, prev_k, prev_d)) return 0;
        const bool cross_up   = (prev_k <= prev_d) && (cur_k >  cur_d);
        const bool cross_down = (prev_k >= prev_d) && (cur_k <  cur_d);
        if (cross_up   && cur_k < LO && cur_d < LO) return +1;
        if (cross_down && cur_k > HI && cur_d > HI) return -1;
        return 0;
    }

    int _sig_atr_mom_h1() const noexcept {
        constexpr int MOM_N = 50;
        constexpr int LOOKBACK = 100;
        constexpr double LO_PCT = 0.20, HI_PCT = 0.80;
        const int sz = (int)bars_h1_.size();
        if (sz < LOOKBACK + MOM_N + 2) return 0;
        if (atr14_h1_ <= 0.0) return 0;

        // Build ATR snapshot deque for percentile computation. We only have
        // a single live atr14_h1_ value updated per H1 bar, so as a faithful
        // mirror of edge_hunt's implementation we recompute per-bar TR
        // history and apply the same Wilder-style smoothing on the fly.
        // Cheaper alternative: maintain a rolling deque of recent ATR values.
        // For now we rebuild over the last LOOKBACK bars (acceptable: H1
        // closes happen only once per hour).
        std::vector<double> recent_atr;
        recent_atr.reserve(LOOKBACK);
        // Wilder-style ATR rolled forward.
        double atr_running = 0.0;
        int    warmup = 0;
        const int start = sz - LOOKBACK - 1;
        for (int i = start; i < sz; ++i) {
            if (i == 0) continue;
            const auto& cur  = bars_h1_[i];
            const auto& prev = bars_h1_[i - 1];
            double tr = std::max(cur.high - cur.low,
                                 std::max(std::abs(cur.high - prev.close),
                                          std::abs(cur.low  - prev.close)));
            if (warmup < kAtrPeriod) {
                atr_running = (atr_running * warmup + tr) / (warmup + 1);
                ++warmup;
            } else {
                atr_running = (atr_running * (kAtrPeriod - 1) + tr) / kAtrPeriod;
            }
            if (i >= sz - LOOKBACK) recent_atr.push_back(atr_running);
        }
        if ((int)recent_atr.size() < LOOKBACK / 2) return 0;
        std::sort(recent_atr.begin(), recent_atr.end());
        const double pl_v = recent_atr[(size_t)(LO_PCT * recent_atr.size())];
        const double ph_v = recent_atr[(size_t)
            (std::min<double>(HI_PCT, 0.999) * recent_atr.size())];
        if (atr14_h1_ < pl_v || atr14_h1_ > ph_v) return 0;

        const double cur_close  = bars_h1_[sz - 1].close;
        const double prev_close = bars_h1_[sz - 1 - MOM_N].close;
        if (cur_close > prev_close * 1.001) return +1;
        if (cur_close < prev_close * 0.999) return -1;
        return 0;
    }

    // -------------------------------------------------------------------------
    //  Per-cell ATR snapshot (used at fire-entry time for SL/TP geometry).
    // -------------------------------------------------------------------------
    double _atr_for_cell(int ci) const noexcept {
        switch (kUstecTfHtfCells[ci].tf) {
            case UstecTfHtfTf::M15: return atr14_m15_;
            case UstecTfHtfTf::H1:  return atr14_h1_;
            case UstecTfHtfTf::H2:  return atr14_h2_;
            case UstecTfHtfTf::H4:  return atr14_h4_;
        }
        return 0.0;
    }

    // -------------------------------------------------------------------------
    //  Trade lifecycle
    // -------------------------------------------------------------------------
    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kUstecTfHtfCells[ci];
        const double atr = _atr_for_cell(ci);
        const double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr <= 0.0) return;

        const double sl_dist = cfg.sl_mult * atr;
        const double tp_dist = cfg.tp_mult * atr;

        auto& p = pos[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        p.mfe_pts       = 0.0;
        p.mae_pts       = 0.0;
        p.be_armed      = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && p.entry_px > 0.0) {
            const double favourable = p.is_long ? (mid - p.entry_px)
                                                : (p.entry_px - mid);
            if (favourable > p.mfe_pts) p.mfe_pts = favourable;
            if (favourable < p.mae_pts) p.mae_pts = favourable;
        }

        // BE arm + trail (uses guards bundle so behaviour matches XauThreeBar30m).
        // We snapshot guards.st around the call to detect a new BE arm event.
        const bool was_be_armed = guards.st.be_armed;
        guards.update_mfe_mae(p.is_long, p.entry_px, mid);
        const double new_sl = guards.update_sl(p.is_long, p.entry_px,
                                                p.sl_px, mid, p.atr_at_entry);
        if (p.is_long  && new_sl > p.sl_px) p.sl_px = new_sl;
        if (!p.is_long && new_sl < p.sl_px) p.sl_px = new_sl;
        if (!was_be_armed && guards.st.be_armed) {
            p.be_armed = true;
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0.0;
        if (p.is_long) {
            if (bid <= p.sl_px)      { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px)      { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        const char* reason = "SL_HIT";
        if (hit_tp)               reason = "TP_HIT";
        else if (hit_sl && p.be_armed) reason = "TRAIL_HIT";  // BE arm => trail-class exit
        _close(ci, xp, reason, now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);
        const double net_pnl_usd = pts_move * lot * USTEC_USD_PER_PT;

        omega::TradeRecord tr;
        tr.symbol     = SYMBOL;       // "USTEC.F"
        tr.engine     = std::string("UstecTrendFollowHtf_") +
                        kUstecTfHtfCells[ci].short_name;
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;     // raw pts*lots; downstream multiplier
        tr.net_pnl    = tr.pnl;             // trade_lifecycle overwrites after costs
        tr.mfe        = p.mfe_pts * lot;
        tr.mae        = p.mae_pts * lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kUstecTfHtfCells[ci].name;
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        // Update guards bookkeeping (daily pnl, killswitch). If guards trip
        // the engine to shadow, mirror that into shadow_mode.
        const bool was_killswitch_armed = guards.st.killswitch_tripped;
        const bool was_daily_capped     = guards.st.daily_capped;
        guards.on_close(net_pnl_usd);
        if (!was_killswitch_armed && guards.st.killswitch_tripped) {
            log_killswitch("UstecTrendFollowHtf",
                           guards.st.consec_losses, guards.st.daily_pnl_usd);
            shadow_mode = true;
        }
        if (!was_daily_capped && guards.st.daily_capped) {
            log_daily_cap("UstecTrendFollowHtf",
                          guards.st.daily_pnl_usd, guards.cfg.daily_loss_limit);
        }
        guards.reset_per_trade();

        p.active        = false;
        p.broker_position_id.clear();
        p.entry_clOrdId.clear();
        p.cooldown_bars = 1;
    }
};

} // namespace omega
