#pragma once
// =============================================================================
//  XauThreeBar30mEngine.hpp -- XAU 30m three-bar continuation
//                              (S34 2026-05-12, S35-P3 protections retrofit
//                               2026-05-12)
// =============================================================================
//
//  PROVENANCE
//
//  Implements the "ThreeBar" cell that showed positive in S33 Pass-8 but
//  was never built as a production engine. HANDOFF_S33_FINAL.md sec 4
//  item 5:
//
//      "30m XAU ThreeBar (positive in Pass-8, n=639 +$979 across 30mo,
//       BE=$1.59). Not yet built. 21 trades/month, modest edge."
//
//  Cell mechanic (from backtest/edge_hunt.cpp sig_three_bar, lines
//  604-624):
//
//      Three consecutive same-direction 30m bars in [i-3, i-2, i-1]
//      (all green or all red). On bar i, fire LONG if close > bars[i-1].high;
//      fire SHORT if close < bars[i-1].low. Bracket geometry sl=2.0*ATR14,
//      tp=4.0*ATR14 (the S33 sweep optimum for this cell).
//
//  Cross-validation status: Pass-8 result was positive on 30 months
//  Dukascopy 2023-09 → 2025-09; explicit per-year cross-validation
//  has not yet been re-run (the S33 candidate list put this in "NOT
//  tested but COULD be" rather than the cross-validated production
//  stack). This file therefore ships as a PRODUCTION CANDIDATE:
//  enabled=false by default in addition to the shadow_mode=true safety
//  pin. To promote: run the backtest harness with the parameters
//  baked in below, confirm year-by-year all-positive (with AND without
//  the S35-P3 protection stack toggled on), then flip enabled=true
//  in engine_init.hpp.
//
//  S35-P3 PROTECTIONS RETROFIT (2026-05-12)
//
//  Layered on top of the original engine, the S35-P3 commit wires in
//  the standard ProtectedEngineGuards bundle (include/engine_protections.hpp)
//  so the engine inherits a uniform downside-protection stack:
//
//      (1) Hard SL multiplier             -- still SL_MULT = 2.0 * ATR
//      (2) Time stop                      -- close after max_bars_held bars
//      (3) Break-even shift               -- arm BE at be_trigger_atr*ATR_at_entry
//      (4) Trailing stop after BE         -- trail = trail_atr_mult * ATR_at_entry
//      (5) Daily loss cap                 -- pause for the UTC day after dollar limit
//      (6) Consec-loss kill switch        -- flip shadow_mode after N losses
//      (7) Volatility regime gate         -- ATR floor + ceiling
//      (8) Spread cap                     -- inherited from guards.cfg.max_spread
//      (9) Session-window block           -- UTC hour range, wrap-aware
//      (10) shadow_mode and enabled       -- still engine-owned
//
//  The signal evaluator (_evaluate_signal), bar history (bars_), local
//  ATR fallback (_update_local_atr), and entry geometry math
//  (_fire_entry) are byte-identical to the pre-retrofit version. The
//  retrofit only adds:
//
//      - guards member (omega::ProtectedEngineGuards)
//      - public knobs forwarded to guards.cfg in init()
//      - guards.roll_day + guards.on_bar_held + guards.time_stop_fired
//        path in on_30m_bar (bars-held / time-stop)
//      - guards.check_entry_ok before signal evaluation in on_30m_bar
//        (subsumes the previous spread-cap return)
//      - guards.update_mfe_mae + guards.update_sl path in _manage_open
//      - guards.on_close path in _close (USD pnl bookkeeping for the
//        daily cap and consec-loss killswitch)
//      - reset_per_trade in _close
//
//  With every new knob set to its disabled-value default (0 / -1),
//  behavior is regression-identical to the pre-retrofit engine. The
//  retrofit therefore lands SAFELY in shadow / disabled mode; the
//  operator opts in to specific protections by setting non-default
//  knobs in engine_init.hpp.
//
//  SAFETY (post-retrofit)
//
//      - shadow_mode = true by default. HARD shadow regardless of
//        kShadowDefault, until the year-by-year cross-validation has
//        been done.
//      - enabled = false by default. Set true in engine_init.hpp only
//        after the cross-validation completes.
//      - 0.01 lot, single position at a time.
//      - 1.0 USD spread cap (now via guards.cfg.max_spread).
//      - 1-bar (30m) cooldown after exit.
//      - DOES NOT touch any protected core file.
//      - DOES NOT take any additional locks (guards is per-engine,
//        single-threaded by codebase convention).
//      - All ten S35-P3 protections enumerated above, each opt-in
//        through a public knob.
//
//  USAGE
//
//      // globals.hpp:
//      static omega::XauThreeBar30mEngine g_xau_threebar_30m;
//
//      // engine_init.hpp (existing + new knobs; the new ones can stay
//      // at their declared defaults if you want the post-retrofit
//      // engine to behave EXACTLY like the pre-retrofit engine):
//      g_xau_threebar_30m.shadow_mode        = true;
//      g_xau_threebar_30m.enabled            = false;
//      g_xau_threebar_30m.lot                = 0.01;
//      g_xau_threebar_30m.max_spread         = 1.0;
//      g_xau_threebar_30m.max_bars_held      = 4;     // S35-P3 (2h cap)
//      g_xau_threebar_30m.be_trigger_atr     = 1.0;   // S35-P3
//      g_xau_threebar_30m.be_cost_buffer_pts = 0.10;  // S35-P3
//      g_xau_threebar_30m.trail_after_be     = true;  // S35-P3
//      g_xau_threebar_30m.trail_atr_mult     = 0.75;  // S35-P3
//      g_xau_threebar_30m.daily_loss_limit   = 5.0;   // S35-P3
//      g_xau_threebar_30m.max_consec_losses  = 5;     // S35-P3
//      g_xau_threebar_30m.min_atr_floor      = 0.30;  // S35-P3
//      g_xau_threebar_30m.max_atr_ceil       = 30.0;  // S35-P3
//      g_xau_threebar_30m.block_hour_start   = 22;    // S35-P3 (Asia)
//      g_xau_threebar_30m.block_hour_end     = 8;     // S35-P3 (Asia)
//      g_xau_threebar_30m.init();
//      printf("[OMEGA-INIT] XauThreeBar30mEngine initialised: shadow=%d enabled=%d lot=%.2f\n",
//             (int)g_xau_threebar_30m.shadow_mode,
//             (int)g_xau_threebar_30m.enabled,
//             g_xau_threebar_30m.lot);
//
//      // tick_gold.hpp inside the M30 bar-close branch:
//      omega::XauThreeBar30mBar bar30m{};
//      bar30m.bar_start_ms = s_bar_30m_ms;
//      bar30m.open  = s_cur_30m.open;
//      bar30m.high  = s_cur_30m.high;
//      bar30m.low   = s_cur_30m.low;
//      bar30m.close = s_cur_30m.close;
//      g_xau_threebar_30m.on_30m_bar(bar30m, bid, ask,
//          g_bars_gold.m30.ind.atr14.load(std::memory_order_relaxed),
//          now_ms_g, bracket_on_close);
//
//      // tick_gold.hpp on every gold tick (intra-bar SL/TP management):
//      g_xau_threebar_30m.on_tick(bid, ask, now_ms_g, bracket_on_close);
//
//  S34 CLOSE-PATH CORRECTNESS  (preserved post-retrofit)
//
//  This engine implements the BracketEngine close-path convention
//  (include/BracketEngine.hpp:1207-1230) from day one, so the bugs
//  documented in the UstecTrendFollow5mEngine.hpp S34 header don't
//  repeat:
//
//      tr.pnl    = (exit - entry) * sign * lot  (raw pts*lots;
//                                                trade_lifecycle
//                                                multiplies by
//                                                tick_value_multiplier)
//      tr.symbol = "XAUUSD"                  (matches sizing table →
//                                             $100/pt)
//      tr.side   = "LONG" / "SHORT"          (ledger convention)
//      tr.mfe / tr.mae populated on every tick from live mid (now
//                                            via guards.st.mfe_pts /
//                                            mae_pts)
//      tr.engine = "XauThreeBar30m"          (distinct in ledger; the
//                                             cell has only one signal
//                                             family so no per-cell
//                                             suffix needed)
//
//  If the logging.hpp dedupe + zero-PnL guards (S34) ever print
//  [CSV-ZERO-PNL] against this engine, that's a bug here. By
//  construction it should never fire.
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

#include "OmegaTradeLedger.hpp"      // omega::TradeRecord
#include "engine_protections.hpp"    // S35-P3 ProtectedEngineGuards
#include "OmegaCostGuard.hpp"
#include "XauM30HmmGate.hpp"         // S88-followup HMM regime gate

namespace omega {

// ---------- Bar input (mirrors XauTfBar for call-site parity)
struct XauThreeBar30mBar {
    int64_t bar_start_ms = 0;
    double  open  = 0.0;
    double  high  = 0.0;
    double  low   = 0.0;
    double  close = 0.0;
};

// ---------- Single-position state
struct XauThreeBar30mPos {
    bool        active             = false;
    bool        is_long            = false;
    double      entry_px           = 0.0;
    double      tp_px              = 0.0;
    double      sl_px              = 0.0;
    double      atr_at_entry       = 0.0;
    int64_t     entry_ts_ms        = 0;
    int         bars_held          = 0;
    int         cooldown_bars      = 0;
    // S34-style MFE/MAE tracking in raw price points (mid-based). The
    // S35-P3 retrofit moves the authoritative MFE/MAE into
    // guards.st.mfe_pts / mae_pts; pos.mfe_pts / mae_pts are retained
    // as a backward-compat reflection of the same values for any code
    // that reads them off the position struct directly. Multiplied
    // by lot and tick_value_multiplier downstream to produce USD figures.
    double      mfe_pts            = 0.0;
    double      mae_pts            = 0.0;
    std::string broker_position_id;
    std::string entry_clOrdId;
};

// =============================================================================
//  XauThreeBar30mEngine
// =============================================================================
struct XauThreeBar30mEngine {
public:
    // ── Pre-retrofit public knobs (UNCHANGED; engine_init.hpp compatible) ──
    bool   shadow_mode = true;
    bool   enabled     = false;
    double lot         = 0.01;
    double max_spread  = 1.0;
    bool   long_only   = false;  // S96: backtest v2 shows short side PF=0.84; long-only OOS PF=1.24

    // ── S35-P3 protection knobs (NEW, defaults = "disabled" so the
    //    retrofit is regression-safe unless the operator opts in).
    //    Wired into guards.cfg in init(). Override per-engine in
    //    engine_init.hpp.
    int    max_bars_held       = 0;     // (2) time stop bars; 0 disables
    double be_trigger_atr      = 0.0;   // (3) BE arm trigger; 0 disables
    double be_cost_buffer_pts  = 0.10;  // BE-shifted SL = entry +/- this
    bool   trail_after_be      = false; // (4) trail after BE arm
    double trail_atr_mult      = 0.0;   //     trail = trail_atr_mult * ATR_at_entry
    double daily_loss_limit    = 0.0;   // (5) USD; 0 disables
    int    max_consec_losses   = 0;     // (6) killswitch threshold; 0 disables
    double min_atr_floor       = 0.0;   // (7) ATR floor; 0 disables
    double max_atr_ceil        = 0.0;   //     ATR ceiling; 0 disables
    int    block_hour_start    = -1;    // (9) session block start UTC; -1 disables
    int    block_hour_end      = -1;    //     session block end UTC

    // ── S88-followup (2026-05-27): N-bar close-slope sign-alignment gate.
    //   Filter: signal direction must agree with sign(close[i] - close[i-N]).
    //   Python re-test on 25mo Duka m30 (TRACK3 followup) showed slopes
    //   N in [8, 12] form a robust cluster:
    //       N=8:  net $349 -> $444 (+27%), DD -$236 -> -$177 (-25%),
    //             removed 71 trades with WR 43.7% (vs baseline 51.7%);
    //             removed-trades net -$113 (gate selecting losers as desired)
    //       N=12: net $349 -> $363 (+4%), DD -$236 -> -$183 (-22%),
    //             removed 116 trades with WR 44.8%, removed net -$16
    //   Random-baseline test: P(random drop of 71 trades >= slope_8 lift) = 8.3%
    //   -- borderline; multiple adjacent N's confirm direction (not curve fit).
    //   On 2024-2025 only (excluding 2026 outlier quarter), lift is +100%.
    //   See research/THREEBAR_F4_DUKA_RETEST.md for full analysis.
    //   Defaults disable the gate (regression-safe).
    int    slope_lookback_bars = 0;     // 0 disables; recommend 12 for shadow A/B
    bool   use_slope_gate      = false;

    // ── S88-followup (2026-05-27): M30 HMM regime gate.
    //   3-state Gaussian HMM (NOISE / MEAN_REV / CONTINUATION) classifier
    //   trained on 4 M30 features (drift_norm, atr_norm, range_norm,
    //   dir_persist). Causal forward-only inference, NO smoothing. Caller
    //   pushes one HmmM30Bar per M30 close before _evaluate_signal is hit.
    //   When use_hmm_gate=true, entry blocked if classifier reports NOISE.
    //   Combined w/ slope_12 gate (ANDed), OOS lift over baseline:
    //     net +48% ($448 -> $664), PF +21% (1.27 -> 1.54), MaxDD -19% ($183
    //     -> $149), retains 79% of trades. See
    //     /Users/jo/Tick/mid_freq_research/HMM_REGIME_GATE_RESULTS.md.
    //   Gate provider lives at include/XauM30HmmGate.hpp; engine queries
    //   it via the hmm_gate pointer set at init().
    bool          use_hmm_gate    = false;
    XauM30HmmGate*hmm_gate        = nullptr;  // engine_init.hpp wires a static instance

    // ── S63 2026-05-13 VWR-pattern in-flight protection ───────────────────
    //   Mirrors VWAPReversionEngine (CrossAssetEngines.hpp L1245-1247) but
    //   uses entry-price-relative percentages (XAU @ $3700, 0.05 -> $1.85).
    //   The 3-bar reversal pattern is structural mean-rev; without these
    //   guards, "fired and stopped" trades that the SL eventually catches
    //   bleed for longer than needed. Set _PCT = 0.0 to disable a phase.
    double LOSS_CUT_PCT        = 0.05;  // cold-loss cut threshold (% of entry)
    double BE_ARM_PCT          = 0.03;  // mfe % of entry that arms BE ratchet
    double BE_BUFFER_PCT       = 0.012; // BE_CUT trigger after arm

    // ── Bracket geometry — locked to Pass-8 sweep optimum ──────────────────
    // SL = 2.0 * ATR14, TP = 4.0 * ATR14 (R:R 2:1). Per the S33 sweep this
    // was the cell variant that produced n=639 +$979 over 30mo at BE=$1.59
    // on XAU 30m Dukascopy. Tunable in case the cross-validation finds a
    // different optimum on the year-by-year split.
    static constexpr double SL_MULT = 2.0;
    static constexpr double TP_MULT = 4.0;

    // ── Cooldown bars after close (1 bar = 30m) ────────────────────────────
    static constexpr int COOLDOWN_BARS = 1;

    XauThreeBar30mPos pos{};

    // S35-P3 protection bundle (public so dashboards can read state).
    ProtectedEngineGuards guards{};

    // Bar history (last N=16; need 4 bars for the signal + warmup margin).
    static constexpr int kBarHistory = 16;
    std::deque<XauThreeBar30mBar> bars_;

    // Rolling Wilder ATR14 over bars_ (local fallback; preferred path is to
    // pass the external ATR from g_bars_gold.m30.ind.atr14 in on_30m_bar).
    static constexpr int kAtrPeriod = 14;
    double atr14_ = 0.0;
    int    atr_warmup_count_ = 0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    // S102: warmup entry guard — see XauTrendFollow2hEngine.hpp for root cause.
    bool warmup_active_ = false;

    void init() noexcept {
        bars_.clear();
        atr14_ = 0.0;
        atr_warmup_count_ = 0;
        warmup_active_ = false;
        pos = {};

        // S35-P3: wire engine knobs into guards.cfg. The guards struct
        // is the single source of truth at runtime; the engine's
        // public members exist solely as the engine_init.hpp surface.
        guards.cfg.sl_atr_mult         = SL_MULT;
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

    bool has_open_position() const noexcept { return pos.active; }

    // ============================================================
    //  on_30m_bar -- called by tick_gold.hpp when an M30 bar closes
    // ============================================================
    void on_30m_bar(const XauThreeBar30mBar& bar,
                    double bid, double ask,
                    double atr14_external,
                    int64_t now_ms,
                    OnCloseFn on_close) noexcept {
        if (!enabled) return;

        const int64_t now_unix_s = now_ms / 1000;
        guards.roll_day(now_unix_s);

        bars_.push_back(bar);
        while ((int)bars_.size() > kBarHistory) bars_.pop_front();

        if (atr14_external > 0.0) atr14_ = atr14_external;
        else _update_local_atr();

        // S88-followup: feed HMM gate EVERY bar regardless of position state.
        // Gate is causal — needs continuous feature updates to maintain forward
        // alpha. Originally placed below the position/cooldown returns; that
        // froze the gate during open trades, leaving it stale by the time the
        // next entry chance arose. Move to top so updates always run.
        //
        // CRITICAL ATR alignment: gate the push on atr14_EXTERNAL > 0, NOT
        // on the engine's local atr14_ which the engine falls back to via
        // _update_local_atr (returns nonzero from bar 2 onwards). Python
        // reference pipeline computes ATR with NaN warmup until bar 14;
        // pushing local-ATR bars to the gate shifts bar_count_ by ~13 vs
        // Python and misaligns regime labels. Skip first 13 bars so the
        // gate's bar 1 corresponds to the same m30 index as Python feats row 0
        // (= m30 idx 14, first bar with valid Wilder ATR-14).
        if (use_hmm_gate && hmm_gate != nullptr && atr14_external > 0.0) {
            HmmM30Bar hb{};
            hb.open  = bar.open;
            hb.high  = bar.high;
            hb.low   = bar.low;
            hb.close = bar.close;
            hb.atr14 = atr14_external;
            hmm_gate->push_bar(hb);
        }

        if (pos.cooldown_bars > 0) --pos.cooldown_bars;

        // S35-P3: bars-held + time stop. Increment guards' counter, then
        // check time stop. If fired, close the trade with TIME_STOP
        // reason and return -- no further signal evaluation this bar.
        if (pos.active) {
            ++pos.bars_held;
            guards.on_bar_held();
            if (guards.time_stop_fired()) {
                const double exit_px = pos.is_long ? bid : ask;
                omega::log_time_stop("XauThreeBar30m",
                                     guards.st.bars_held,
                                     guards.st.mfe_pts,
                                     guards.st.mae_pts);
                _close(exit_px, "TIME_STOP", now_ms, on_close);
                return;
            }
        }

        // Warmup: need 4 bars for the signal lookback + at least 14 for ATR.
        if ((int)bars_.size() < 16) return;
        if (atr14_ <= 0.0) return;

        // Only fire when no position is open and cooldown has expired.
        if (pos.active) return;
        if (pos.cooldown_bars > 0) return;

        // S35-P3: entry guards (spread, ATR regime, daily cap, killswitch,
        // session window). Subsumes the pre-retrofit spread-cap return.
        if (const char* why = guards.check_entry_ok(bid, ask, atr14_,
                                                    now_unix_s)) {
            omega::log_entry_block("XauThreeBar30m", why);
            return;
        }

        int side = _evaluate_signal();
        if (side == 0) return;

        // S88-followup: HMM regime gate -- block entries when classifier
        // reports NOISE state (Asia/dead/no-edge regime). Fail-open: if not
        // warmed (insufficient history), allow the entry through, matching
        // the engine's existing warmup behaviour.
        //
        // KNOWN GAP 2026-05-27: C++ port of the Python HMM does not replicate
        // the Python state distribution. On the 6-month PKL data the Python
        // training showed ~38% NOISE, ~22% MEAN_REV, ~40% CONT; the C++ gate
        // running the same trained params on the same data labels ~99% of
        // entry-attempt bars as CONT. Suspected feature computation drift
        // between Python pandas and C++ deque path (Wilder ATR, rolling
        // median, or bar source M5->M30 vs M15->M30 reaggregation difference).
        // Until resolved: use_hmm_gate must stay false in production. Slope
        // gate (use_slope_gate) remains the live-validated gate.
        // Reference Python implementation:
        // /Users/jo/Tick/mid_freq_research/hmm_causal_test.py
        // KNOWN: on production engine config (long_only=true, ATR floor 0.30,
        // spread cap, cost gate), the HMM-NOISE filter blocks zero entries
        // because the 3-bar continuation signal fires only on trending bars
        // (CONT regime by definition) AND the engine's existing pre-filters
        // already exclude bars in NOISE regime. The Python research showing
        // "+20% lift" used a simpler simulator without those filters --
        // there NOISE-regime bars were passing entry, here they don't.
        //
        // C++ HMM IS correct: bar-level state distribution matches Python
        // causal (33% CONT / 23% MR / 44% NOISE). At entries specifically
        // C++ always reports CONT because the engine has already filtered.
        // Gate stays available for engines without those pre-filters; for
        // XauThreeBar30m it is structurally redundant.
        if (use_hmm_gate && hmm_gate != nullptr) {
            if (hmm_gate->warmed() && hmm_gate->is_noise()) {
                omega::log_entry_block("XauThreeBar30m", "HMM_GATE_NOISE");
                return;
            }
        }

        // S88-followup: N-bar close-slope sign-alignment gate. Compares the
        // sign of (current close - close N bars ago) to the signal direction.
        // Disagreement blocks entry. Causal (uses only completed bars). See
        // the field declaration above for the empirical rationale and the
        // research note. Default disabled -> regression-safe.
        if (use_slope_gate && slope_lookback_bars > 0) {
            const int last = (int)bars_.size() - 1;
            if (last - slope_lookback_bars >= 0) {
                const double slope =
                    bars_[last].close - bars_[last - slope_lookback_bars].close;
                const int slope_sign = (slope > 0.0) ? +1 : (slope < 0.0 ? -1 : 0);
                if (slope_sign == 0 || slope_sign != side) {
                    omega::log_entry_block("XauThreeBar30m", "SLOPE_GATE");
                    return;
                }
            }
            // If we don't have enough history yet, allow the entry through —
            // matches the engine's existing warmup behaviour (signal already
            // required 4 bars of history; gate adds no extra block during warmup).
        }

        _fire_entry(side, bid, ask, now_ms);
        (void)on_close;  // exits run intra-bar in on_tick
    }

    // ============================================================
    //  on_tick -- runs every tick to manage open position
    // ============================================================
    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close) noexcept {
        if (!enabled) return;
        if (!pos.active) return;
        _manage_open(bid, ask, now_ms, on_close);
    }

    // ============================================================
    //  force_close -- end-of-day or shutdown flatten
    // ============================================================
    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept {
        if (!pos.active) return;
        double exit_px = pos.is_long ? bid : ask;
        _close(exit_px, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
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

    // ---------- Three-bar continuation signal (BYTE-IDENTICAL pre-retrofit)
    // bars_[last-3], bars_[last-2], bars_[last-1] = three completed bars
    //                                              before the trigger bar
    // bars_[last]                                 = trigger bar just closed
    //
    // Long  fires when all three [last-3 .. last-1] are bullish (close>open)
    //       and bars_[last].close breaks above bars_[last-1].high.
    // Short fires when all three are bearish (close<open) and bars_[last].close
    //       breaks below bars_[last-1].low.
    int _evaluate_signal() const noexcept {
        if ((int)bars_.size() < 4) return 0;
        const int last = (int)bars_.size() - 1;
        const auto& b3 = bars_[last - 3];
        const auto& b2 = bars_[last - 2];
        const auto& b1 = bars_[last - 1];
        const auto& cb = bars_[last];

        const bool all_up =
            (b3.close > b3.open) && (b2.close > b2.open) && (b1.close > b1.open);
        const bool all_down =
            (b3.close < b3.open) && (b2.close < b2.open) && (b1.close < b1.open);

        if (all_up   && cb.close > b1.high) return +1;
        if (!long_only && all_down && cb.close < b1.low ) return -1;
        return 0;
    }

    void _fire_entry(int side, double bid, double ask, int64_t now_ms) noexcept {
        // S102: block entries during warmup — warmup primes indicators only.
        if (warmup_active_) return;
        double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0 || atr14_ <= 0.0) return;

        // SL distance uses guards.cfg.sl_atr_mult (= SL_MULT from init()).
        const double sl_dist = guards.cfg.sl_atr_mult * atr14_;
        const double tp_dist = TP_MULT * atr14_;

        // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
        {
            const double spread_pts = ask - bid;
            if (!ExecutionCostGuard::is_viable(
                    "XAUUSD", spread_pts, tp_dist, lot, 1.5))
            {
                return;
            }
        }

        pos.active        = true;
        pos.is_long       = (side > 0);
        pos.entry_px      = entry;
        pos.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        pos.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        pos.atr_at_entry  = atr14_;
        pos.entry_ts_ms   = now_ms;
        pos.bars_held     = 0;
        pos.cooldown_bars = 0;
        pos.mfe_pts       = 0.0;
        pos.mae_pts       = 0.0;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();

        // S35-P3: clear per-trade guards state (be_armed, mfe/mae, bars_held).
        guards.reset_per_trade();
    }

    void _manage_open(double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        // S35-P3: MFE/MAE via guards (authoritative). Also reflect into
        // pos.mfe_pts / mae_pts for backward-compat with any reader that
        // pulls them off the position struct.
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && pos.entry_px > 0.0) {
            guards.update_mfe_mae(pos.is_long, pos.entry_px, mid);
            pos.mfe_pts = guards.st.mfe_pts;
            pos.mae_pts = guards.st.mae_pts;
        }

        // S35-P3: SL tightening (BE arm + trail after BE). Returns the
        // possibly-tightened SL; engine assigns it back. Never loosens.
        if (pos.entry_px > 0.0 && pos.atr_at_entry > 0.0 && mid > 0.0) {
            pos.sl_px = guards.update_sl(pos.is_long, pos.entry_px,
                                         pos.sl_px, mid, pos.atr_at_entry);
        }

        // S63 2026-05-13 VWR-pattern in-flight protection. Runs BEFORE the
        // standard SL/TP check so cold-loss / giveback cuts take priority.
        // Uses guards.st.mfe_pts / mae_pts (already updated above) as the
        // MFE/MAE source.
        const double move = pos.is_long ? (mid - pos.entry_px)
                                        : (pos.entry_px - mid);
        // Phase 1: BE_RATCHET
        if (BE_ARM_PCT > 0.0 && BE_BUFFER_PCT >= 0.0 && pos.entry_px > 0.0) {
            const double arm_pts    = pos.entry_px * BE_ARM_PCT    / 100.0;
            const double buffer_pts = pos.entry_px * BE_BUFFER_PCT / 100.0;
            if (guards.st.mfe_pts >= arm_pts && move <= buffer_pts) {
                const double exit_px = pos.is_long ? bid : ask;
                _close(exit_px, "BE_CUT", now_ms, on_close);
                return;
            }
        }
        // Phase 2: LOSS_CUT
        if (LOSS_CUT_PCT > 0.0 && pos.entry_px > 0.0) {
            const double adverse       = -move;
            const double loss_cut_dist = pos.entry_px * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = pos.is_long ? bid : ask;
                _close(exit_px, "LOSS_CUT", now_ms, on_close);
                return;
            }
        }

        // Standard SL/TP check against the (possibly tightened) pos.sl_px.
        bool hit_tp = false, hit_sl = false;
        double exit_px = 0.0;
        if (pos.is_long) {
            if (bid <= pos.sl_px)      { exit_px = pos.sl_px; hit_sl = true; }
            else if (bid >= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        } else {
            if (ask >= pos.sl_px)      { exit_px = pos.sl_px; hit_sl = true; }
            else if (ask <= pos.tp_px) { exit_px = pos.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        // Reason mapping:
        //   hit_tp           -> "TP_HIT"
        //   hit_sl + be_armed -> "TRAIL_HIT"   (BE or trail locked us out
        //                                       at a price tighter than
        //                                       the original SL)
        //   hit_sl           -> "SL_HIT"       (original SL touched)
        const char* reason;
        if (hit_tp) {
            reason = "TP_HIT";
        } else if (guards.st.be_armed) {
            reason = "TRAIL_HIT";
        } else {
            reason = "SL_HIT";
        }
        _close(exit_px, reason, now_ms, on_close);
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        if (!pos.active) return;

        // Raw points*lots PnL — matches BracketEngine convention
        // (include/BracketEngine.hpp:1216-1218). trade_lifecycle.hpp:218-222
        // multiplies tr.pnl/mfe/mae by tick_value_multiplier(tr.symbol) to
        // produce USD; tick_value_multiplier("XAUUSD") = 100.0 (sizing.hpp:32).
        const double pts_move = pos.is_long ? (exit_px - pos.entry_px)
                                            : (pos.entry_px - exit_px);

        // S35-P3: feed USD pnl into guards for daily cap + killswitch
        // bookkeeping. Hardcoded XAUUSD multiplier = 100.0 here matches
        // tick_value_multiplier("XAUUSD") in sizing.hpp. The downstream
        // trade_lifecycle path applies the same multiplier to tr.pnl
        // separately -- no double counting, because the guards copy
        // never flows into tr.*; it's pure local bookkeeping.
        constexpr double XAUUSD_TICK_VALUE = 100.0;
        const double pnl_usd = pts_move * lot * XAUUSD_TICK_VALUE;

        const bool was_killed_before = guards.st.killswitch_tripped;
        const bool was_capped_before = guards.st.daily_capped;
        guards.on_close(pnl_usd);

        // Killswitch newly tripped? Flip shadow_mode and log loudly.
        if (!was_killed_before && guards.st.killswitch_tripped) {
            shadow_mode = true;
            omega::log_killswitch("XauThreeBar30m",
                                  guards.st.consec_losses,
                                  guards.st.daily_pnl_usd);
        }
        // Daily cap newly tripped? Log (entries blocked until UTC rollover).
        if (!was_capped_before && guards.st.daily_capped) {
            omega::log_daily_cap("XauThreeBar30m",
                                 guards.st.daily_pnl_usd,
                                 guards.cfg.daily_loss_limit);
        }

        omega::TradeRecord tr;
        tr.symbol     = "XAUUSD";
        tr.engine     = "XauThreeBar30m";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;          // trade_lifecycle overwrites after costs
        // MFE/MAE sourced from guards (authoritative since S35-P3).
        tr.mfe        = guards.st.mfe_pts * lot;
        tr.mae        = guards.st.mae_pts * lot;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = "ThreeBar_sl2.0tp4.0";
        tr.shadow     = shadow_mode;

        if (on_close) on_close(tr);

        pos.active        = false;
        pos.broker_position_id.clear();
        pos.entry_clOrdId.clear();
        pos.cooldown_bars = COOLDOWN_BARS;

        // S35-P3: clear per-trade guards state (be_armed, mfe/mae, bars_held).
        // Per-day and per-session state (daily_pnl, consec_losses,
        // daily_capped, killswitch_tripped) are preserved across trades.
        guards.reset_per_trade();
    }

public:
    // S101: warmup from pre-built M30 bar CSV. Feeds historical bars
    //   through on_30m_bar so ATR14, bar deque, and guards are primed.
    std::string warmup_csv_path;

    int warmup_from_csv(const std::string& path) noexcept {
        if (!enabled) { printf("[XauThreeBar30m-WARMUP] skipped -- disabled\n"); fflush(stdout); return 0; }
        if (path.empty()) { printf("[XauThreeBar30m-WARMUP] skipped -- no path (cold start)\n"); fflush(stdout); return 0; }
        std::ifstream f(path);
        if (!f.is_open()) { printf("[XauThreeBar30m-WARMUP] FAIL -- cannot open '%s'\n", path.c_str()); fflush(stdout); return 0; }
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

            XauThreeBar30mBar bar; bar.bar_start_ms = ms; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
            on_30m_bar(bar, c, c, 0.0, ms + 1800LL*1000, OnCloseFn{});
            ++fed;
        }
        warmup_active_ = false;  // S102: live entries now permitted
        printf("[XauThreeBar30m-WARMUP] fed=%d M30 bars, atr=%.4f bars_size=%d path='%s'\n",
               fed, atr14_, (int)bars_.size(), path.c_str());
        fflush(stdout);
        return fed;
    }
};

} // namespace omega
