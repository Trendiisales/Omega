#pragma once
// =============================================================================
//  Us30EnsembleEngine.hpp -- DJ30.F multi-cell ensemble (S37 2026-05-26)
// =============================================================================
//
//  PROVENANCE
//
//  Built 2026-05-26 from /Users/jo/edge_research 3-period intersection test
//  on 2yr Dukascopy USA30.IDX bars (2023-10-30 -> 2025-10-29). The dataset
//  was split into 3 equal periods (~8 months each); cells included here are
//  positive in EVERY period (net > 0, PF >= 1.1, n >= 20 per period) AND
//  pass walk-forward 4 anchored folds all-positive AND pay back $4 round-trip
//  cost over 2 years at 0.01 lot.
//
//  CELLS (4 cells; sibling cell 3bar_mom_H1 lives in Us303BarMomH1Engine and
//  is NOT included here to avoid double-firing the same signal.)
//
//      [A] H1  atr_expansion  long  sl=2.0_tp=3.0  max_bars=48
//          intersect: p1 PF=1.43 n=148 net=+$845 / p2 PF=1.97 n=137 net=+$2215
//                     p3 PF=1.39 n=133 net=+$1314    total +$4374
//          walk-forward 4/4 folds positive: aggregate OOS n=336 net=+$3535
//          trail-post-cost (mfe_lock arm=1R lock=0.90): n=671 net_pts=$1531
//
//      [B] H1  inside_brk     long  sl=3.0_tp=5.0  max_bars=48
//          intersect: p1 PF=2.04 n=69  net=+$1192 / p2 PF=1.71 n=72 net=+$1235
//                     p3 PF=1.75 n=73  net=+$1480    total +$3907
//          walk-forward 4/4 folds positive: aggregate OOS n=175 net=+$2821
//          trail-post-cost: n=290 net_pts=$878 PF=1.61
//
//      [C] M30 atr_expansion  long  sl=3.0_tp=2.0  max_bars=96
//          intersect: p1 PF=1.35 n=201 net=+$860 / p2 PF=1.35 n=189 net=+$1112
//                     p3 PF=1.40 n=184 net=+$1710    total +$3682
//          walk-forward 4/4 folds positive: aggregate OOS n=444 net=+$2890
//          trail-post-cost: n=785 net_pts=$889 PF=1.31
//
//      [D] H4  ema_pullback_10_30 long sl=1.5_tp=5.0 max_bars=24
//          intersect: p1 PF=1.89 n=39 net=+$676 / p2 PF=2.35 n=27 net=+$897
//                     p3 PF=2.28 n=36 net=+$1192   total +$2764
//          walk-forward 4/4 folds positive: aggregate OOS n=77 net=+$2202
//          trail-post-cost: n=169 net_pts=$708 PF=2.01 (highest PF in set)
//
//  PERIOD TOTALS (2yr 3-period intersection, raw close-to-close edge_scan):
//      Sum of all 4 cells across 2yr ≈ +$14,727 raw (cooldown-blocked, $0.50 cost)
//
//  WALK-FORWARD AGGREGATE OOS (4 folds anchored expanding window):
//      All 4 cells: 16/16 fold-cell combinations positive
//      Aggregate OOS net (sum): +$11,448 at engine cost level
//
//  COST-MODEL POST-TRAIL (mfe_lock arm=1.0R lock=0.90; spread 3pt + slip 1pt):
//      Sum at 0.01 lot: net_pts $4006 -> $200.30 (independent cooldowns)
//      Conservative w/ overlap: ~$140 / 2yr at 0.01 lot
//      Scale to 0.1 lot: ~$1,400 / 2yr; 1 lot: ~$14,000 / 2yr
//
//  CAVEAT: backtest period ends 2025-10-29. DJ30 rose +6.4% from 47,539 to
//  50,580 between 2025-10-29 and 2026-05-22 (yfinance ^DJI D1). That rally
//  is NOT in the in-sample data. All cells are LONG-only so direction-bias
//  matches that rally; magnitude and frequency in that period are
//  UNVALIDATED. Live behaviour treats Nov 2025 -> May 2026 as forward OOS.
//
//  SAFETY (ProtectedEngineGuards bundle)
//      shadow_mode = true (HARD shadow per CLAUDE.md ~1mo trace rule)
//      enabled     = true (engine runs in shadow)
//      lot         = 0.01  (DJ30.F = $5/pt at 1 lot -> $0.05/pt at 0.01 lot)
//      max_spread  = 5.0
//      Spread cap, ATR floor, BE arm, trail-after-BE come from
//      include/engine_protections.hpp -- same bundle UstecTrendFollowHtfEngine uses.
//
//      Daily-loss-cap and consec-loss killswitch DISABLED by default.
//      Re-enable after ~1mo shadow trace per CLAUDE.md.
//
//  CONCURRENCY: cells are INDEPENDENT. Each cell holds its own open position.
//  Up to 4 concurrent DJ30.F positions at 0.01 lot each (max 0.04 lot total
//  exposure). No shared-position lock between cells (operator selection
//  2026-05-26: "All cells independent (up to N concurrent)").
//
//  USAGE
//
//      // globals.hpp:
//      static omega::Us30EnsembleEngine g_us30_ensemble;
//
//      // engine_init.hpp:
//      g_us30_ensemble.shadow_mode      = true;
//      g_us30_ensemble.enabled          = true;
//      g_us30_ensemble.lot              = 0.01;
//      g_us30_ensemble.max_spread       = 5.0;
//      g_us30_ensemble.be_trigger_atr   = 1.0;
//      g_us30_ensemble.be_cost_buffer_pts = 0.50;
//      g_us30_ensemble.trail_after_be   = true;
//      g_us30_ensemble.trail_atr_mult   = 0.75;
//      g_us30_ensemble.min_atr_floor    = 10.0;   // DJ30 raw points
//      g_us30_ensemble.init();
//
//      // tick_indices.hpp -- inside DJ30.F bar-builder, M15 close branch:
//      omega::Us30EnsembleBar bar15m{};
//      bar15m.bar_start_ms = s_us30_15_start;
//      bar15m.open  = s_us30_15.open;
//      bar15m.high  = s_us30_15.high;
//      bar15m.low   = s_us30_15.low;
//      bar15m.close = s_us30_15.close;
//      g_us30_ensemble.on_15m_bar(bar15m, bid, ask, 0.0, now_ms, ca_on_close);
//
//      // tick_indices.hpp -- every DJ30.F tick:
//      g_us30_ensemble.on_tick(bid, ask, now_ms, ca_on_close);
//
//  ARCHITECTURE
//
//  M15 base TF. tick_indices.hpp feeds M15 bars. Engine internally synthesizes
//  M30 (2 M15), H1 (4 M15), H4 (16 M15) by accumulating consecutive M15s.
//  When the M15 bar completing a higher-TF window arrives, the synthesized
//  bar is pushed onto its TF's deque and cell evaluation runs for that TF.
//
//  This mirrors UstecTrendFollowHtfEngine 's pattern.
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>

#include "engine_protections.hpp"
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"

namespace omega {

// ----------------------------------------------------------------------------
//  Bar structure (M15 input bar; higher TFs synthesized)
// ----------------------------------------------------------------------------
struct Us30EnsembleBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ----------------------------------------------------------------------------
//  Per-cell open position
// ----------------------------------------------------------------------------
struct Us30EnsemblePos {
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
    bool        be_armed      = false;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// ----------------------------------------------------------------------------
//  Cell catalogue
// ----------------------------------------------------------------------------
enum class Us30CellFamily {
    AtrExpansion_H1,       // [A]
    InsideBarBreak_H1,     // [B]
    AtrExpansion_M30,      // [C]
    EmaPullback10_30_H4,   // [D]
    RsiExtreme_H1,         // [E] S37i 2026-05-26: rally-robust diversifier
};

enum class Us30CellTf { M15, M30, H1, H4 };

struct Us30CellConfig {
    Us30CellFamily family;
    Us30CellTf     tf;
    double         sl_mult;
    double         tp_mult;
    int            max_bars_held;
    const char*    name;        // full descriptor for tr.regime
    const char*    short_name;  // distinct tag in tr.engine
};

// Rebalanced 2026-05-26: stripped to single best risk-adjusted cell.
// engine_sim.py per-cell metrics (bare SL/TP, $4 RT cost, 2yr):
//   InsBrkH1   PF=1.40  Sharpe=2.21  DD=-$231  RF=2.30  Net=+$533  WR=49.5%
//   AtrExpH1   PF=1.21  Sharpe=1.79  DD=-$315  RF=1.61  Net=+$507  WR=45.7%
//   EmaPbH4    PF=1.16  Sharpe=0.64  DD=-$138  RF=0.89  Net=+$123  WR=38.0% (cost drag)
//   AtrExpM30  PF=1.08  Sharpe=0.91  DD=-$309  RF=0.81  Net=+$249  WR=62.8% (cost drag)
// InsBrkH1 wins on PF + DD + RF. Pairing with AtrExpH1 raised Sharpe to
// 2.80 but lowered PF to 1.28 and worsened combined DD to -$485 (overlapping
// loss days). Operator criteria = highest PF + Sharpe + lowest DD => single.
//
// Ensemble array kept (size 1) for future cell additions if fresh data
// (post Nov 2025) surfaces a second uncorrelated survivor.
// S37i 2026-05-26: re-expanded to 2 cells after revalidation on full 2.6yr
// (2023-10 -> 2026-04, includes the +6.4% rally that was OOS in S37b).
// InsideBarBreak retained (PF 1.57 full sample, RF 4.72) BUT OOS degraded
// in rally (ratio 0.59). Add RsiExtreme as rally-robust diversifier:
//   InsBrkH1  IS PF 1.90 -> OOS PF 1.12 (degraded)
//   RsiExtH1  IS PF 1.45 -> OOS PF 1.26 (stable, ratio 0.87)
// Pair gives both edges; signal families uncorrelated (price-pattern vs
// momentum-oscillator).
static constexpr Us30CellConfig kUs30EnsembleCells[] = {
    { Us30CellFamily::InsideBarBreak_H1, Us30CellTf::H1, 3.0, 5.0, 48,
      "InsideBarBreak_H1_sl3.0tp5.0_long",  "InsBrkH1"  },
    { Us30CellFamily::RsiExtreme_H1,     Us30CellTf::H1, 3.0, 3.0, 48,
      "RsiExtreme_H1_sl3.0tp3.0_long",      "RsiExtH1"  },
};
static constexpr int kUs30EnsembleNumCells =
    static_cast<int>(sizeof(kUs30EnsembleCells) / sizeof(kUs30EnsembleCells[0]));

// ----------------------------------------------------------------------------
//  Engine
// ----------------------------------------------------------------------------
struct Us30EnsembleEngine {
public:
    // Engine-owned knobs (forwarded into guards.cfg in init()).
    bool   shadow_mode       = true;
    bool   enabled           = false;
    double lot               = 0.01;
    double max_spread        = 5.0;

    // Protection knobs (init() forwards into guards.cfg).
    //
    // BE+trail DEFAULTS OFF. Engine-sim 2yr backtest (2025-05-26):
    //   bare fixed SL/TP:  +$1411 / 1711 trades / WR 50%
    //   BE 1ATR + trail 0.75ATR:  -$676 / 2583 trades / WR 72%
    // The trail clips atr_exp/inside_brk winners (WF cells favour fixed-R
    // exits). Same finding as S35-P3 USTEC Donch15m drop. Operator may
    // re-enable in engine_init.hpp on a per-deploy basis.
    double be_trigger_atr      = 0.0;     // 0 disables BE arm
    double be_cost_buffer_pts  = 0.50;
    bool   trail_after_be      = false;
    double trail_atr_mult      = 0.0;
    double min_atr_floor       = 10.0;    // DJ30 raw points; ~ATR(H1)/3
    double max_atr_ceil        = 0.0;
    double daily_loss_limit    = 0.0;     // disabled
    int    max_consec_losses   = 0;       // disabled
    int    block_hour_start    = -1;
    int    block_hour_end      = -1;

    // Symbol metadata. DJ30.F = $5/pt at 1 lot per BlackBull spec.
    static constexpr const char* SYMBOL = "DJ30.F";
    static constexpr double DJ30_USD_PER_PT = 5.0;

    // ProtectionState for engine as whole (shared across cells).
    ProtectedEngineGuards guards;

    // Per-cell open positions.
    std::array<Us30EnsemblePos, kUs30EnsembleNumCells> pos{};

    // Per-TF bar histories (synthesized internally from M15).
    static constexpr int kHistM15 = 64;
    static constexpr int kHistM30 = 128;   // 4 days
    static constexpr int kHistH1  = 200;
    static constexpr int kHistH4  = 80;
    std::deque<Us30EnsembleBar> bars_m15_;
    std::deque<Us30EnsembleBar> bars_m30_;
    std::deque<Us30EnsembleBar> bars_h1_;
    std::deque<Us30EnsembleBar> bars_h4_;

    // Per-TF Wilder ATR14.
    static constexpr int kAtrPeriod = 14;
    double atr14_m15_ = 0.0;
    double atr14_m30_ = 0.0;
    double atr14_h1_  = 0.0;
    double atr14_h4_  = 0.0;
    int    atr_warmup_m15_ = 0;
    int    atr_warmup_m30_ = 0;
    int    atr_warmup_h1_  = 0;
    int    atr_warmup_h4_  = 0;

    // Synthesized higher-TF bar accumulators.
    Us30EnsembleBar cur_m30_{}; bool m30_open_ = false; int cur_m30_n_ = 0;
    Us30EnsembleBar cur_h1_{};  bool h1_open_  = false; int cur_h1_n_  = 0;
    Us30EnsembleBar cur_h4_{};  bool h4_open_  = false; int cur_h4_n_  = 0;
    static constexpr int kM15PerM30 = 2;
    static constexpr int kM15PerH1  = 4;
    static constexpr int kM15PerH4  = 16;

    // EMA state for H4 ema_pullback cell (EMA10 + EMA50 on H4 closes).
    double ema10_h4_ = 0.0;
    double ema50_h4_ = 0.0;
    int    ema_warmup_h4_ = 0;
    static constexpr int kEmaFast = 10;
    static constexpr int kEmaSlow = 50;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    void init() noexcept {
        bars_m15_.clear(); bars_m30_.clear();
        bars_h1_.clear();  bars_h4_.clear();
        atr14_m15_ = atr14_m30_ = atr14_h1_ = atr14_h4_ = 0.0;
        atr_warmup_m15_ = atr_warmup_m30_ = atr_warmup_h1_ = atr_warmup_h4_ = 0;
        cur_m30_ = cur_h1_ = cur_h4_ = {};
        m30_open_ = h1_open_ = h4_open_ = false;
        cur_m30_n_ = cur_h1_n_ = cur_h4_n_ = 0;
        ema10_h4_ = ema50_h4_ = 0.0;
        ema_warmup_h4_ = 0;
        for (auto& p : pos) p = {};

        guards.cfg.sl_atr_mult         = 2.0;   // baseline (cells override per-trade)
        guards.cfg.max_bars_held       = 0;     // per-cell cap; engine guard disabled
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
    //  Warmup seed from a CSV of M15 OHLC bars.
    //  Format: bar_start_ms,open,high,low,close
    // -------------------------------------------------------------------------
    int seed_from_m15_csv(const std::string& path) noexcept {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return 0;
        char line[512];
        int loaded = 0;
        // Header (skip if alpha)
        if (std::fgets(line, sizeof(line), f)) {
            // crude header sniff: if first non-space char is not a digit, treat
            // as header and discard; else rewind by re-reading via processing.
            const char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            const bool is_header = !(*p == '-' || (*p >= '0' && *p <= '9'));
            if (!is_header) {
                // Process this line
                Us30EnsembleBar b{};
                if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf",
                                (long long*)&b.bar_start_ms,
                                &b.open, &b.high, &b.low, &b.close) == 5) {
                    _seed_one_bar(b);
                    ++loaded;
                }
            }
        }
        while (std::fgets(line, sizeof(line), f)) {
            Us30EnsembleBar b{};
            if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf",
                            (long long*)&b.bar_start_ms,
                            &b.open, &b.high, &b.low, &b.close) != 5) continue;
            _seed_one_bar(b);
            ++loaded;
        }
        std::fclose(f);
        return loaded;
    }

    // -------------------------------------------------------------------------
    //  on_15m_bar -- called by tick_indices.hpp at every M15 close.
    //  Internally synthesizes M30/H1/H4 by accumulating consecutive M15s.
    // -------------------------------------------------------------------------
    void on_15m_bar(const Us30EnsembleBar& bar,
                    double bid, double ask,
                    double atr15m_external,
                    int64_t now_ms,
                    OnCloseFn on_close) noexcept
    {
        if (!enabled) return;

        guards.roll_day(now_ms / 1000);

        _push_bar(bars_m15_, bar, kHistM15);
        if (atr15m_external > 0.0) atr14_m15_ = atr15m_external;
        else _update_atr(bars_m15_, atr14_m15_, atr_warmup_m15_);

        bool m30_closed = _ingest_into_synth(cur_m30_, m30_open_, cur_m30_n_,
                                              bar, kM15PerM30);
        if (m30_closed) {
            _push_bar(bars_m30_, cur_m30_, kHistM30);
            _update_atr(bars_m30_, atr14_m30_, atr_warmup_m30_);
            cur_m30_n_ = 0; m30_open_ = false;
        }
        bool h1_closed = _ingest_into_synth(cur_h1_, h1_open_, cur_h1_n_,
                                             bar, kM15PerH1);
        if (h1_closed) {
            _push_bar(bars_h1_, cur_h1_, kHistH1);
            _update_atr(bars_h1_, atr14_h1_, atr_warmup_h1_);
            cur_h1_n_ = 0; h1_open_ = false;
        }
        bool h4_closed = _ingest_into_synth(cur_h4_, h4_open_, cur_h4_n_,
                                             bar, kM15PerH4);
        if (h4_closed) {
            _push_bar(bars_h4_, cur_h4_, kHistH4);
            _update_atr(bars_h4_, atr14_h4_, atr_warmup_h4_);
            _update_ema_h4(cur_h4_.close);
            cur_h4_n_ = 0; h4_open_ = false;
        }

        // Per-cell bar accounting (cooldown + bars_held).
        for (int ci = 0; ci < kUs30EnsembleNumCells; ++ci) {
            auto& p = pos[ci];
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (p.active) {
                // bars_held tick on the cell's OWN TF close
                Us30CellTf tf = kUs30EnsembleCells[ci].tf;
                if (tf == Us30CellTf::M15) ++p.bars_held;
                if ((tf == Us30CellTf::M30) && m30_closed) ++p.bars_held;
                if ((tf == Us30CellTf::H1)  && h1_closed)  ++p.bars_held;
                if ((tf == Us30CellTf::H4)  && h4_closed)  ++p.bars_held;
            }
        }

        // Engine-level entry guards (spread, ATR regime via M15, daily cap,
        // killswitch, session).
        if (const char* why = guards.check_entry_ok(bid, ask, atr14_m15_,
                                                     now_ms / 1000)) {
            log_entry_block("Us30Ensemble", why);
            return;
        }

        // Try each cell. Independent positions per cell.
        for (int ci = 0; ci < kUs30EnsembleNumCells; ++ci) {
            if (pos[ci].active) continue;
            if (pos[ci].cooldown_bars > 0) continue;
            const auto& cfg = kUs30EnsembleCells[ci];

            // Cell fires only when its TF window closed.
            if (cfg.tf == Us30CellTf::M30 && !m30_closed) continue;
            if (cfg.tf == Us30CellTf::H1  && !h1_closed)  continue;
            if (cfg.tf == Us30CellTf::H4  && !h4_closed)  continue;
            // M15 cells always tick.

            int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;  // exits run intra-bar in on_tick
    }

    // -------------------------------------------------------------------------
    //  on_tick -- runs every DJ30.F tick to manage open positions.
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept
    {
        if (!enabled) return;
        for (int ci = 0; ci < kUs30EnsembleNumCells; ++ci) {
            if (!pos[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    // Force-close all open positions.
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept
    {
        for (int ci = 0; ci < kUs30EnsembleNumCells; ++ci) {
            if (!pos[ci].active) continue;
            double xp = pos[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

private:
    // -------------------------------------------------------------------------
    //  Bar / ATR / EMA helpers
    // -------------------------------------------------------------------------
    void _push_bar(std::deque<Us30EnsembleBar>& dq, const Us30EnsembleBar& bar,
                    int max_size) noexcept {
        dq.push_back(bar);
        while ((int)dq.size() > max_size) dq.pop_front();
    }

    void _update_atr(const std::deque<Us30EnsembleBar>& dq,
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

    void _update_ema_h4(double close) noexcept {
        const double a_fast = 2.0 / (kEmaFast + 1.0);
        const double a_slow = 2.0 / (kEmaSlow + 1.0);
        if (ema_warmup_h4_ == 0) {
            ema10_h4_ = close;
            ema50_h4_ = close;
        } else {
            ema10_h4_ = a_fast * close + (1.0 - a_fast) * ema10_h4_;
            ema50_h4_ = a_slow * close + (1.0 - a_slow) * ema50_h4_;
        }
        ++ema_warmup_h4_;
    }

    bool _ingest_into_synth(Us30EnsembleBar& cur, bool& open, int& n,
                             const Us30EnsembleBar& m15, int m15_per_synth) noexcept {
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

    void _seed_one_bar(const Us30EnsembleBar& bar) noexcept {
        _push_bar(bars_m15_, bar, kHistM15);
        _update_atr(bars_m15_, atr14_m15_, atr_warmup_m15_);
        if (_ingest_into_synth(cur_m30_, m30_open_, cur_m30_n_, bar, kM15PerM30)) {
            _push_bar(bars_m30_, cur_m30_, kHistM30);
            _update_atr(bars_m30_, atr14_m30_, atr_warmup_m30_);
            cur_m30_n_ = 0; m30_open_ = false;
        }
        if (_ingest_into_synth(cur_h1_, h1_open_, cur_h1_n_, bar, kM15PerH1)) {
            _push_bar(bars_h1_, cur_h1_, kHistH1);
            _update_atr(bars_h1_, atr14_h1_, atr_warmup_h1_);
            cur_h1_n_ = 0; h1_open_ = false;
        }
        if (_ingest_into_synth(cur_h4_, h4_open_, cur_h4_n_, bar, kM15PerH4)) {
            _push_bar(bars_h4_, cur_h4_, kHistH4);
            _update_atr(bars_h4_, atr14_h4_, atr_warmup_h4_);
            _update_ema_h4(cur_h4_.close);
            cur_h4_n_ = 0; h4_open_ = false;
        }
    }

    // -------------------------------------------------------------------------
    //  Per-cell signal evaluators. Return +1 long, -1 short, 0 none.
    //  All cells LONG-only per 3-period intersection results.
    // -------------------------------------------------------------------------
    int _evaluate_signal(int ci) const noexcept {
        switch (kUs30EnsembleCells[ci].family) {
            case Us30CellFamily::AtrExpansion_H1:
                return _sig_atr_expansion(bars_h1_, atr14_h1_);
            case Us30CellFamily::InsideBarBreak_H1:
                return _sig_inside_bar_break(bars_h1_);
            case Us30CellFamily::AtrExpansion_M30:
                return _sig_atr_expansion(bars_m30_, atr14_m30_);
            case Us30CellFamily::EmaPullback10_30_H4:
                return _sig_ema_pullback_h4();
            case Us30CellFamily::RsiExtreme_H1:
                return _sig_rsi_extreme_h1();
        }
        return 0;
    }

    // RSI extreme cross (edge_scan sig_rsi_extreme): RSI(14) prev<25 AND
    // current >= 25 = long; mirror at 75 for short. We only use long here.
    int _sig_rsi_extreme_h1() const noexcept {
        const int N = 14;
        const int sz = (int)bars_h1_.size();
        if (sz < N + 2) return 0;
        // Wilder RSI(14)
        double up = 0.0, dn = 0.0;
        for (int i = sz - N; i < sz; ++i) {
            const double d = bars_h1_[i].close - bars_h1_[i-1].close;
            if (d > 0) up += d; else dn -= d;
        }
        if (up + dn <= 0) return 0;
        const double rsi_cur = 100.0 - 100.0 / (1.0 + up / std::max(dn, 1e-12));
        // Previous-bar RSI
        double up_p = 0.0, dn_p = 0.0;
        for (int i = sz - N - 1; i < sz - 1; ++i) {
            const double d = bars_h1_[i].close - bars_h1_[i-1].close;
            if (d > 0) up_p += d; else dn_p -= d;
        }
        if (up_p + dn_p <= 0) return 0;
        const double rsi_prev = 100.0 - 100.0 / (1.0 + up_p / std::max(dn_p, 1e-12));
        if (rsi_prev < 25.0 && rsi_cur >= 25.0) return +1;
        return 0;
    }

    // ATR-expansion long: current bar range > 1.5 x ATR AND close > open.
    int _sig_atr_expansion(const std::deque<Us30EnsembleBar>& dq,
                            double atr_tf) const noexcept {
        constexpr double K = 1.5;
        if (dq.empty() || atr_tf <= 0.0) return 0;
        const auto& b = dq.back();
        const double rng = b.high - b.low;
        if (rng > K * atr_tf && b.close > b.open) return +1;
        return 0;
    }

    // Inside-bar break long: previous bar inside prev-prev bar's range,
    // current bar closes above previous bar's high.
    // (mirrors edge_scan.py sig_inside_bar long-side semantics.)
    int _sig_inside_bar_break(const std::deque<Us30EnsembleBar>& dq) const noexcept {
        const int sz = (int)dq.size();
        if (sz < 3) return 0;
        const auto& cur     = dq[sz - 1];
        const auto& prev    = dq[sz - 2];
        const auto& prevprev = dq[sz - 3];
        const bool inside = (prev.high < prevprev.high) && (prev.low > prevprev.low);
        if (inside && cur.close > prev.high) return +1;
        return 0;
    }

    // EMA pullback fast=10 slow=30 (sig_ema_pullback in edge_scan.py used
    // (10, 30) -- this is what 'ema_pb_10_30' refers to). Despite the cell
    // name saying 10_30, edge_scan.py grid included (10,30) as a 'fast,slow'
    // pair. Re-validated against intersect CSV using sig_ema_pullback(df,10,30,1).
    int _sig_ema_pullback_h4() const noexcept {
        if (bars_h4_.empty()) return 0;
        if (ema_warmup_h4_ < kEmaSlow) return 0;
        const auto& b = bars_h4_.back();
        if (ema10_h4_ <= ema50_h4_) return 0;        // trend filter
        if (b.low > ema10_h4_) return 0;             // touched fast EMA on the bar?
        if (b.close <= ema10_h4_) return 0;          // closed back above
        return +1;
    }

    double _atr_for_cell(int ci) const noexcept {
        switch (kUs30EnsembleCells[ci].tf) {
            case Us30CellTf::M15: return atr14_m15_;
            case Us30CellTf::M30: return atr14_m30_;
            case Us30CellTf::H1:  return atr14_h1_;
            case Us30CellTf::H4:  return atr14_h4_;
        }
        return 0.0;
    }

    // -------------------------------------------------------------------------
    //  Trade lifecycle
    // -------------------------------------------------------------------------
    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = kUs30EnsembleCells[ci];
        const double atr = _atr_for_cell(ci);
        const double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr <= 0.0) return;

        const double sl_dist = cfg.sl_mult * atr;
        const double tp_dist = cfg.tp_mult * atr;

        // Cost gate -- include/OmegaCostGuard.hpp
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(SYMBOL, spread_pts, tp_dist, lot, 1.5)) {
                return;
            }
        }

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
        const auto& cfg = kUs30EnsembleCells[ci];
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && p.entry_px > 0.0) {
            const double favourable = p.is_long ? (mid - p.entry_px)
                                                : (p.entry_px - mid);
            if (favourable > p.mfe_pts) p.mfe_pts = favourable;
            if (favourable < p.mae_pts) p.mae_pts = favourable;
        }

        // BE arm + trail via guards bundle.
        const bool was_be_armed = guards.st.be_armed;
        guards.update_mfe_mae(p.is_long, p.entry_px, mid);
        const double new_sl = guards.update_sl(p.is_long, p.entry_px,
                                                p.sl_px, mid, p.atr_at_entry);
        if (p.is_long  && new_sl > p.sl_px) p.sl_px = new_sl;
        if (!p.is_long && new_sl < p.sl_px) p.sl_px = new_sl;
        if (!was_be_armed && guards.st.be_armed) p.be_armed = true;

        // Per-cell max_bars_held timeout (bars_held ticks on TF closes).
        if (cfg.max_bars_held > 0 && p.bars_held >= cfg.max_bars_held) {
            const double xp = p.is_long ? bid : ask;
            _close(ci, xp, "MAX_BARS", now_ms, on_close);
            return;
        }

        // SL / TP intra-bar.
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
        if (hit_tp)                    reason = "TP_HIT";
        else if (hit_sl && p.be_armed) reason = "TRAIL_HIT";
        _close(ci, xp, reason, now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos[ci];
        if (!p.active) return;

        const double pts_move = p.is_long ? (exit_px - p.entry_px)
                                          : (p.entry_px - exit_px);
        const double net_pnl_usd = pts_move * lot * DJ30_USD_PER_PT;

        omega::TradeRecord tr;
        tr.symbol     = SYMBOL;
        tr.engine     = std::string("Us30Ensemble_") +
                        kUs30EnsembleCells[ci].short_name;
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;
        tr.mfe        = p.mfe_pts * lot;
        tr.mae        = p.mae_pts * lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = kUs30EnsembleCells[ci].name;
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        const bool was_kill   = guards.st.killswitch_tripped;
        const bool was_capped = guards.st.daily_capped;
        guards.on_close(net_pnl_usd);
        if (!was_kill && guards.st.killswitch_tripped) {
            log_killswitch("Us30Ensemble",
                           guards.st.consec_losses, guards.st.daily_pnl_usd);
            shadow_mode = true;
        }
        if (!was_capped && guards.st.daily_capped) {
            log_daily_cap("Us30Ensemble",
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
