#pragma once
#include <iomanip>
#include <iostream>
#include "SpreadRegimeGate.hpp"
#include "OmegaNewsBlackout.hpp"
// =============================================================================
// XauusdFvgEngine.hpp  --  Fair-Value-Gap engine on XAUUSD 15-minute bars
// =============================================================================
//
// 2026-05-XX SESSION DESIGN (Claude / Jo):
//   First production-track FVG engine. C++ port of scripts/fvg_pnl_backtest_v3.py
//   (v3 #5 ACCEPTED config) into the live tick harness, alongside (NOT
//   displacing) the S59 USDJPY Asian-Open shadow.
//
//   Lineage (full backtest evidence):
//     - HANDOFF_FVG_BACKTEST.md (2026-05-02): XAUUSD 15m cleared the six-gate
//       in-sample bar (PF 1.67, n 131, DD 4.46%) AND cleared the four-gate
//       walk-forward bar at TWO independent train/test cutoffs:
//         WF train->2025-11-15  Top PF 1.95  cost-stress-2x 1.84  4/4 PASS
//         WF train->2025-12-01  Top PF 2.44  cost-stress-2x 2.31  4/4 PASS
//       Realistic shadow expectation: PF 1.5-1.8, ~50% WR, low DD,
//       ~25 trades/month.
//     - DESIGN_XAUUSD_FVG_ENGINE.md: this engine's design plan.
//     - scripts/fvg_pnl_backtest_v3.py: bit-exact reference for entry/exit
//       logic. Imports from scripts/usdjpy_xauusd_fvg_signal_test.py (core,
//       NEVER MODIFIED).
//
//   Sister engine (class shape / shadow pattern / _close mutex / news
//   blackout / spread gate):
//     include/UsdjpyAsianOpenEngine.hpp  (S59)
//
//   FROZEN ACCEPTED CONFIG (from v3 #5; see HANDOFF doc for evidence):
//     - 15-min bars, top decile pool, score gate >= 0.48
//     - SL 2.5*ATR, TP 5.0*ATR, time-stop 60 bars (= 15 hours)
//     - BE off (BE+1.0 actively destroys edge on every backtest)
//     - 0.5% risk per trade on $10,000 notional sizing baseline
//     - Cost model: 0.5 pips/side slippage at pip = 0.10 USD
//     - Score weights: gap_size=1.5, displacement=1.0, tick_volume=1.0,
//                      trend_align=0.0 (DROPPED), age_decay=0.0 (DROPPED)
//
//   PRIMARY DIVERGENCE FROM SISTER ENGINES:
//     This is a 15-minute-bar engine on a tick stream, not a tick-window
//     structural engine. The engine accumulates ticks into 15-min OHLC bars
//     internally (UTC-aligned to floor(now_s / 900)), runs FVG detection on
//     bar close, queues unmitigated FVGs, and opens a position when a future
//     bar's range first re-enters the FVG zone (mitigation entry, bar-level).
//     Live SL/TP/time-stop checks run per-tick on bid/ask once a position is
//     open -- this is more reactive than the backtest's bar-high/low check
//     and may produce one-tick-of-noise differences in exits. Shadow output
//     is logged for live-vs-backtest reconciliation.
//
//   SAFETY:
//     - shadow_mode = true by default. Pinned to true (NOT kShadowDefault) in
//       engine_init.hpp on first deployment. Promote to LIVE only after a
//       3-month shadow run that clears the four-gate quarterly re-validation
//       (n>=50, PF>=1.2, PF>All, cost-stress-2x PF>=1.0) per
//       HANDOFF_FVG_BACKTEST.md  Section 2.
//     - Engine respects gold_any_open mutual exclusion; gold cohort engines
//       see has_open_position() in their gate -- one position at a time
//       across the gold cohort.
//     - News blackout consulted at bar-close mitigation entry only. Live
//       positions still managed via manage_position() through any blackout
//       (don't force-close on pre-existing trades).
//     - SpreadRegimeGate fed every tick, consulted at entry only.
//
//   QUARTERLY RE-VALIDATION LOGGING:
//     The engine exposes a LastClosedExtras snapshot via last_extras() that
//     captures score_at_entry, atr_at_entry, session, and fvg_age_bars at
//     close time. The on_close callback in engine_init.hpp combines this
//     with the omega::TradeRecord and writes a side-channel CSV
//     (live_xauusd_fvg.csv) with the columns required by the v3 walk-forward
//     re-feed at month 3 / 6 / 9 etc.
//
// LOG NAMESPACE:
//   All log lines use prefix [XAU-FVG] / [XAU-FVG-DIAG].
//   tr.engine = "XauusdFvg" (registered in engine_init.hpp).
//   tr.regime = "FVG_15M".
// =============================================================================

#include <cstdint>
#include <mutex>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <array>
#include <functional>
#include <string>
#include <deque>
#include <vector>
#include "OmegaTradeLedger.hpp"

namespace omega {

class XauusdFvgEngine {
public:
    // ---- Symbol / timing ---------------------------------------------------
    // 15-minute bars, UTC-aligned to floor(now_s / BAR_SECS).
    static constexpr int    BAR_SECS               = 900;       // 15 min
    static constexpr int    ATR_PERIOD             = 14;        // RMA, alpha=1/14
    static constexpr int    TICK_VOLUME_WINDOW     = 20;        // 20-bar rolling mean

    // ---- FVG sizing filters (v3 / core defaults) --------------------------
    static constexpr double MIN_GAP_ATR            = 0.10;
    static constexpr double MAX_GAP_ATR            = 5.0;

    // ---- Score weights (v3 production / Phase-0 reweight v1) --------------
    // trend_align and age_decay drop out at weight 0; only gap, disp, tv
    // contribute. EMA20/EMA50 not required for production scoring.
    static constexpr double W_GAP_SIZE             = 1.5;
    static constexpr double W_DISPLACEMENT         = 1.0;
    static constexpr double W_TICK_VOLUME          = 1.0;
    static constexpr double W_SUM                  = W_GAP_SIZE
                                                   + W_DISPLACEMENT
                                                   + W_TICK_VOLUME;
    // FROZEN cutoff at 0.48 (IS / WF average score_top across 11/15 and 12/01
    // cuts). Quarterly re-validation re-checks; do NOT slide without a passed
    // re-fit per HANDOFF doc.
    static constexpr double SCORE_CUTOFF           = 0.48;

    // ---- Risk / exit parameters (v3 #5 ACCEPTED) --------------------------
    static constexpr double SL_ATR_MULT            = 2.5;
    static constexpr double TP_ATR_MULT            = 5.0;
    static constexpr int    TIME_STOP_BARS         = 60;        // 15h on 15m

    // ---- Sizing ($10k baseline, 0.5%/trade) -------------------------------
    static constexpr double STARTING_EQUITY        = 10000.0;
    static constexpr double RISK_PER_TRADE_PCT     = 0.005;
    static constexpr double RISK_DOLLARS           = STARTING_EQUITY
                                                   * RISK_PER_TRADE_PCT;  // $50
    // tick_value_multiplier for XAUUSD per OmegaTradeLedger.hpp:88: 100 USD
    // per price-point per lot (1 unit price * 1 lot = $100).
    static constexpr double USD_PER_PRICE_PER_LOT  = 100.0;
    static constexpr double LOT_MIN                = 0.01;
    static constexpr double LOT_MAX                = 0.20;

    // ---- Cost model (mirrors v3 backtest) ---------------------------------
    static constexpr double SLIPPAGE_PIPS          = 0.5;
    static constexpr double PIP_SIZE               = 0.10;      // XAUUSD pip
    static constexpr double SLIPPAGE_PRICE         = SLIPPAGE_PIPS * PIP_SIZE;
                                                                // = 0.05 USD

    // ---- Queue limits -----------------------------------------------------
    // v3 max_age_for_test = 500 bars (~5.2 days on 15m). After 500 bars
    // without mitigation, drop the FVG.
    static constexpr int    MAX_AGE_BARS           = 500;
    static constexpr int    MAX_PENDING_FVGS       = 64;

    // ---- Diagnostics throttling ------------------------------------------
    static constexpr int    DIAG_EVERY_N_TICKS     = 600;

    // -- Public state -------------------------------------------------------
    // 2026-05-XX: shadow ON by default. Promote to live ONLY after a 3-month
    //   shadow validation showing positive expectancy at the four-gate
    //   walk-forward bar (n>=50, PF>=1.2, PF>All, cost-stress-2x PF>=1.0).
    //   Live promotion via engine_init.hpp override -- do NOT change this
    //   default in the header.
    bool shadow_mode = true;

    // VERIFIER HOOK (added 2026-05-02 with §7 wiring): allow a non-production
    // caller (the synthetic-trace verifier in backtest/verify_xauusd_fvg.cpp)
    // to substitute the score-cutoff at runtime so it can replay against a
    // v3-backtest top-decile pool (trades_top.csv was generated with a
    // train-window-derived cutoff > the production-frozen 0.48). Default
    // value <=0 means "use the static SCORE_CUTOFF" -- production code paths
    // therefore see byte-identical behaviour to the pre-hook engine.
    double score_cutoff_override = -1.0;

    // VERIFIER HOOK 2 (added 2026-05-02 second pass, after diagnosing the
    // count divergence in the first verifier run). Disable the
    // SpreadRegimeGate at the mitigation entry path. The gate's adaptive
    // thresholds (ABS_FLOOR 0.40, ABS_CEIL 0.70) are tuned for forex pairs
    // whose raw-price spreads sit at sub-pip scale. XAUUSD spreads run
    // ~1+ USD raw, structurally above the gate's ABS_CEIL ceiling, so
    // the gate's hysteresis state machine sits in CLOSED for most of the
    // dataset and only opens during low-spread Asian-quiet windows. v3
    // (the bit-exact reference) has no spread gate at all -- so to replay
    // trades_top.csv row-for-row, the verifier must bypass the gate.
    // Production callers leave this `false` and behaviour is byte-identical
    // to today's engine (the gate fires unchanged).
    bool spread_gate_disabled = false;

    // VERIFIER HOOK 3 (added 2026-05-02 second pass). Adopt v3's
    // run_backtest "first-touch only, overlap-skip" mitigation semantics
    // INSTEAD OF the engine's queue-and-wait semantics.
    //
    //   PRODUCTION (flag false):
    //     try_mitigate_pending() runs only when no position is active. An
    //     FVG that first-touches its zone DURING an open trade is left in
    //     the pending queue and will fire on the first post-close bar
    //     where the bar enters its zone. This gives every FVG a second
    //     chance, but takes more trades than v3 across an open-position
    //     timeline.
    //
    //   VERIFIER (flag true):
    //     try_mitigate_pending() runs on every bar regardless of position
    //     state. Inside the function, an FVG whose zone the just-closed
    //     bar entered is consumed exactly once: if no position is active
    //     it attempts entry; if a position IS active it is dropped
    //     (= v3's overlap-skip). FVG detection is similarly ungated from
    //     the !m_pos.active condition so the candidate pool is built from
    //     every bar, matching v3's add_indicators -> detect_fvgs sweep.
    //
    // This is a verifier-only override; the production engine's
    // queue-and-wait semantics are preserved when the flag is left false.
    bool first_touch_only_mode = false;

    // Cancel callback retained for API parity with the cohort. The FVG engine
    // does not place pending stop orders (entry is at zone boundary as a
    // limit fill), so this is unused in v1; kept for future intrabar entry.
    std::function<void(const std::string&)> cancel_fn;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    // on_close_cb is the close hook the engine fires from _close(). Wired in
    // engine_init.hpp to (a) call handle_closed_trade(tr) for the standard
    // ledger / shadow-guard path, and (b) write the side-channel
    // live_xauusd_fvg.csv row using the engine's last_extras() snapshot.
    CloseCallback on_close_cb;

    // Quarterly re-validation extras. Captured at _close(); read by the
    // close callback wired in engine_init.hpp to write the side-channel CSV.
    struct LastClosedExtras {
        double score_at_entry = 0.0;
        double atr_at_entry   = 0.0;
        double gap_height     = 0.0;
        char   session        = 'X';   // 'A','L','N','O' for asian/london/ny/off
        int    fvg_age_bars   = 0;
        int    bars_held      = 0;
        char   direction      = 'X';   // 'B' bull, 'S' bear
    };
    LastClosedExtras last_extras() const noexcept { return m_last_extras; }

    bool has_open_position() const noexcept { return m_pos.active; }

    // -- Live position layout (mirrors S59 LivePos minus BE) ----------------
    struct LivePos {
        bool    active            = false;
        bool    is_long           = false;
        double  entry             = 0.0;   // gross entry (zone boundary)
        double  entry_with_cost   = 0.0;   // gross entry + side_cost
        double  tp                = 0.0;
        double  sl                = 0.0;
        double  size              = 0.0;
        double  atr_at_entry      = 0.0;
        double  score_at_entry    = 0.0;
        double  spread_at_entry   = 0.0;
        double  side_cost         = 0.0;   // half_spread + slippage
        double  gap_height        = 0.0;
        double  mfe               = 0.0;
        double  mae               = 0.0;
        int64_t entry_ts          = 0;
        int64_t entry_bar_idx     = 0;
        // Sequential CLOSED-bar counter at entry. 2026-05-03 v3-alignment fix
        // for the TIME_STOP weekend-jump bug: entry_bar_idx is the absolute
        // UTC bar index (= floor(now_s/900)), which advances by ~192 bars
        // across the Fri-22:00 -> Sun-22:00 forex weekend gap even though no
        // trading bars exist in that window. v3 simulates exits via pandas
        // ARRAY indexing on the bars DataFrame, which is sequential through
        // trading bars only and skips weekends naturally. Comparing
        // m_bars_seen - entry_bar_seq counts the same sequential trading
        // bars v3 walks, so weekend-spanning trades hold the full
        // TIME_STOP_BARS window instead of getting cut off at the first
        // post-weekend bar close. Entry-bar tracking via entry_bar_idx is
        // RETAINED for SL/TP step-5 bar-skip semantics, which compare
        // against m_recent_bars[2].bar_idx (also UTC-floored) -- those run
        // bar-by-bar through trading bars so the UTC delta is fine there.
        int64_t entry_bar_seq     = 0;
        int     fvg_age_bars      = 0;
        char    session           = 'X';
        char    direction         = 'X';   // 'B' bull, 'S' bear
        bool    time_stop_pending = false;
    } m_pos;

    // -- Main tick ----------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 CloseCallback on_close) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;
        m_last_tick_s = now_s;

        // SpreadRegimeGate: feed every tick; consult only on entry path.
        m_spread_gate.on_tick(now_ms, spread);
#ifndef OMEGA_BACKTEST
        m_spread_gate.set_macro_regime(g_macroDetector.regime());
#endif

        ++m_ticks_received;

        // -- (1) Bar accumulator ---------------------------------------------
        const int64_t bar_idx = now_s / BAR_SECS;
        bool new_bar_started = false;
        if (bar_idx != m_current_bar_idx) {
            if (m_current_bar_idx >= 0 && m_cur_bar.tick_count > 0) {
                close_current_bar();
                on_bar_close(can_enter, on_close);
            }
            open_new_bar(bar_idx, now_s, mid, spread);
            new_bar_started = true;
        } else {
            update_current_bar(mid, spread);
        }

        // Diagnostics every N ticks + early in startup.
        if (m_ticks_received % DIAG_EVERY_N_TICKS == 0 || m_ticks_received <= 60) {
            char _buf[512];
            snprintf(_buf, sizeof(_buf),
                "[XAU-FVG-DIAG] ticks=%d bars=%d atr_ready=%d atr=%.4f tv_mean=%.1f "
                "pending=%zu pos_active=%d spread=%.3f\n",
                m_ticks_received, m_bars_seen, (int)m_atr_ready,
                m_atr14, m_tv_mean, m_pending.size(),
                (int)m_pos.active, spread);
            std::cout << _buf;
            std::cout.flush();
        }

        // -- (2) Time-stop deferred close on first tick of a new bar --------
        // v3 exits at the open of bar (entry_bar + TIME_STOP_BARS + 1).
        // First tick of the new bar IS the open of that bar in this engine's
        // bar accumulator. Honour the pending flag set by manage_position().
        if (new_bar_started && m_pos.active && m_pos.time_stop_pending) {
            _close(mid, "TIME_STOP", now_s, on_close ? on_close : on_close_cb);
            return;
        }

        // -- (3) Position management on every tick when LIVE ----------------
        if (m_pos.active) {
            manage_position(bid, ask, mid, now_s,
                            on_close ? on_close : on_close_cb);
            return;  // one position at a time across the engine
        }
    }

    // Force-close hook -- mirrors S59. Gold supervisor / cohort can call this
    // to unwind on emergency shutdown or session-policy override.
    void force_close(double bid, double ask, int64_t now_ms,
                     CloseCallback on_close) noexcept
    {
        if (!m_pos.active) return;
        const double exit_px = m_pos.is_long ? bid : ask;
        _close(exit_px, "FORCE_CLOSE", now_ms / 1000,
               on_close ? on_close : on_close_cb);
    }

private:
    // -- Bar accumulator state ----------------------------------------------
    struct BarAcc {
        int64_t bar_idx     = -1;
        int64_t start_ts    = 0;
        double  open        = 0.0;
        double  high        = 0.0;
        double  low         = 0.0;
        double  close       = 0.0;
        double  spread_sum  = 0.0;
        int     tick_count  = 0;
    };
    BarAcc m_cur_bar;

    struct ClosedBar {
        double  open        = 0.0;
        double  high        = 0.0;
        double  low         = 0.0;
        double  close       = 0.0;
        double  spread_mean = 0.0;
        int     tick_count  = 0;
        int64_t bar_idx     = 0;
        int64_t start_ts    = 0;
    };
    // Three most-recent closed bars: [0] = i-2, [1] = i-1, [2] = i (newest).
    std::array<ClosedBar, 3> m_recent_bars{};
    int     m_bars_seen      = 0;
    int64_t m_current_bar_idx = -1;

    // -- ATR(14) RMA + tick-volume rolling mean -----------------------------
    // Matches pandas ewm(alpha=1/14, adjust=False) with seed = first sample.
    double  m_atr14         = 0.0;
    double  m_prev_close    = 0.0;
    bool    m_atr_ready     = false;
    int     m_atr_warmup    = 0;
    std::deque<int> m_tick_count_history;
    double  m_tv_mean       = 0.0;
    // 2026-05-02 verifier-bug fix (score_at_entry off-by-0.001149):
    //   v3 / core reads tv_mean AT THE MIDDLE BAR (i-1) when scoring an FVG
    //   formed at bar i -- i.e., the rolling 20-bar mean over [i-20 ... i-1],
    //   which EXCLUDES the formation bar. The engine's update_indicators()
    //   pushes bar i's tick_count into the deque BEFORE try_detect_fvg() runs,
    //   so m_tv_mean post-update reflects [i-19 ... i] (= v3's tv_mean[i],
    //   one bar AHEAD of v3's tv_mean[i-1]).
    //
    //   This snapshot is taken inside update_indicators() BEFORE the deque
    //   push, so it preserves the rolling mean over [i-20 ... i-1]. That
    //   value is what try_detect_fvg() consumes for the s_tv score component,
    //   matching v3's   tv_ratio = tc[i-1] / tv_mean[i-1]   exactly.
    //
    //   Note: ATR is intentionally NOT shifted -- v3 uses a[i] (ATR at the
    //   formation bar) for both s_gap and s_disp, and the engine's m_atr14
    //   already reflects bar i after update_indicators() runs first.
    double  m_tv_mean_at_prev_bar = 0.0;

    // -- Pending FVG queue --------------------------------------------------
    struct PendingFvg {
        int64_t bar_idx_formed    = 0;
        int64_t formed_ts         = 0;
        char    direction         = 'B';      // 'B' bull, 'S' bear
        double  zone_low          = 0.0;
        double  zone_high         = 0.0;
        double  gap_height        = 0.0;
        // Score components frozen at formation. With production weights
        // (W_AGE_DECAY=0), score_at_entry == score_at_formation.
        double  s_gap_size        = 0.0;
        double  s_displacement    = 0.0;
        double  s_tick_volume     = 0.0;
        double  score_at_formation = 0.0;
    };
    std::deque<PendingFvg> m_pending;

    // -- Diagnostics + safety state -----------------------------------------
    int     m_ticks_received = 0;
    int     m_trade_id       = 0;
    int64_t m_last_tick_s    = 0;

    // 2026-05-02 third pass (verifier-only consumer). Tracks the bar_idx
    // of the most recent _close. v3 run_backtest's overlap rule is
    //   next_open_after = exit_idx + 1
    // so any FVG with entry_idx <= exit_idx is overlap-skipped. The
    // engine's existing m_pos.active check in try_mitigate_pending covers
    // SL/TP exits naturally (mitigation at step 3 of on_bar_close runs
    // BEFORE the exit at step 5, while m_pos.active is still true). It
    // does NOT cover TIME_STOP, which fires from on_tick BEFORE the
    // exit-bar's on_bar_close runs -- meaning by the time mitigation
    // executes for the exit bar, m_pos.active is already false. Reading
    // m_last_exit_bar_idx in the first_touch_only_mode overlap branch
    // closes that gap. Only consumed in verifier mode.
    int64_t m_last_exit_bar_idx = -1;

    // S59 lineage: serialise the entire _close path. Inherited to avoid the
    // historical phantom-pnl race.
    mutable std::mutex m_close_mtx;

    omega::SpreadRegimeGate m_spread_gate;

    LastClosedExtras m_last_extras{};

    // -- Bar accumulator helpers -------------------------------------------
    void open_new_bar(int64_t bar_idx, int64_t start_ts,
                      double mid, double spread) noexcept
    {
        m_current_bar_idx     = bar_idx;
        m_cur_bar.bar_idx     = bar_idx;
        m_cur_bar.start_ts    = start_ts;
        m_cur_bar.open        = mid;
        m_cur_bar.high        = mid;
        m_cur_bar.low         = mid;
        m_cur_bar.close       = mid;
        m_cur_bar.spread_sum  = spread;
        m_cur_bar.tick_count  = 1;
    }

    void update_current_bar(double mid, double spread) noexcept
    {
        if (mid > m_cur_bar.high) m_cur_bar.high = mid;
        if (mid < m_cur_bar.low ) m_cur_bar.low  = mid;
        m_cur_bar.close       = mid;
        m_cur_bar.spread_sum += spread;
        ++m_cur_bar.tick_count;
    }

    void close_current_bar() noexcept
    {
        // Slide ring: [0] <- [1] <- [2] <- new.
        m_recent_bars[0] = m_recent_bars[1];
        m_recent_bars[1] = m_recent_bars[2];
        ClosedBar& nb = m_recent_bars[2];
        nb.open        = m_cur_bar.open;
        nb.high        = m_cur_bar.high;
        nb.low         = m_cur_bar.low;
        nb.close       = m_cur_bar.close;
        nb.spread_mean = (m_cur_bar.tick_count > 0)
                       ? (m_cur_bar.spread_sum / m_cur_bar.tick_count)
                       : 0.0;
        nb.tick_count  = m_cur_bar.tick_count;
        nb.bar_idx     = m_cur_bar.bar_idx;
        nb.start_ts    = m_cur_bar.start_ts;
        ++m_bars_seen;
    }

    // Update ATR(14) and tv_mean from m_recent_bars[2] (the bar that just
    // closed). Matches v3's add_indicators() bit-equivalently:
    //   tr   = max(|h-l|, |h-prev_close|, |l-prev_close|)
    //   atr  = tr.ewm(alpha=1/14, adjust=False).mean()  -- seed at first sample
    //   tv_mean = tick_count.rolling(20).mean()
    void update_indicators() noexcept
    {
        const ClosedBar& nb = m_recent_bars[2];

        // True range. On the very first bar there is no prev_close; v3 uses
        // pandas which yields NaN for the abs(prev_close) terms on bar 0,
        // making TR[0] = (h-l) effectively. We replicate by skipping the
        // prev_close terms when m_bars_seen == 1.
        double tr;
        if (m_bars_seen == 1) {
            tr = std::fabs(nb.high - nb.low);
        } else {
            const double a = std::fabs(nb.high - nb.low);
            const double b = std::fabs(nb.high - m_prev_close);
            const double c = std::fabs(nb.low  - m_prev_close);
            tr = std::max(a, std::max(b, c));
        }
        m_prev_close = nb.close;

        constexpr double ALPHA = 1.0 / static_cast<double>(ATR_PERIOD);
        if (m_atr_warmup == 0) {
            // ewm(adjust=False) seeds with the first observation.
            m_atr14 = tr;
        } else {
            m_atr14 = m_atr14 * (1.0 - ALPHA) + tr * ALPHA;
        }
        ++m_atr_warmup;
        if (m_atr_warmup >= ATR_PERIOD) m_atr_ready = true;

        // Tick-volume rolling mean (window 20). Matches pandas
        //   tick_count.rolling(20).mean()  -- NaN until 20 observations,
        // then the simple mean of the last 20.
        //
        // BAR-INDEX ALIGNMENT (verifier-bug fix 2026-05-02):
        //   Snapshot the CURRENT m_tv_mean (= rolling mean over the previous
        //   20 bars, ending one bar BEFORE the just-closed bar) into
        //   m_tv_mean_at_prev_bar before pushing the new bar's tick_count.
        //   try_detect_fvg() uses that snapshot so its s_tv computation
        //   reads v3's tv_mean[i-1] (the MIDDLE-bar rolling mean) rather
        //   than v3's tv_mean[i] (one bar ahead). The post-push m_tv_mean
        //   becomes the snapshot value for the NEXT bar's detection cycle.
        m_tv_mean_at_prev_bar = m_tv_mean;

        m_tick_count_history.push_back(nb.tick_count);
        if ((int)m_tick_count_history.size() > TICK_VOLUME_WINDOW)
            m_tick_count_history.pop_front();
        if ((int)m_tick_count_history.size() == TICK_VOLUME_WINDOW) {
            double sum = 0.0;
            for (int v : m_tick_count_history) sum += v;
            m_tv_mean = sum / static_cast<double>(TICK_VOLUME_WINDOW);
        } else {
            m_tv_mean = 0.0;  // sentinel: not ready
        }
    }

    // -- Bar-close pipeline -------------------------------------------------
    void on_bar_close(bool can_enter, CloseCallback on_close) noexcept
    {
        // 1. Update ATR + tv_mean from the just-closed bar.
        update_indicators();

        // 2. If a position is currently live, check whether 60 elapsed bars
        //    have crossed and a TIME_STOP exit is now due on the next bar's
        //    open. The actual close fires on the FIRST tick of the new bar.
        //
        //    BAR-COUNTER ALIGNMENT (2026-05-03 v3-alignment fix):
        //    `elapsed` counts CLOSED TRADING bars since entry, NOT a UTC
        //    delta. The previous formulation
        //        m_recent_bars[2].bar_idx - m_pos.entry_bar_idx
        //    used UTC-floored bar indices, which advance by ~192 across the
        //    Fri-22:00 -> Sun-22:00 forex weekend gap even though no trading
        //    bars exist there -- causing the FIRST Sunday bar close on a
        //    weekend-spanning trade to immediately trip the 60-bar threshold
        //    and fire TIME_STOP ~30 trading bars early relative to v3
        //    (which sees only sequential trading bars via pandas array
        //    indexing). m_bars_seen increments once per close_current_bar()
        //    and therefore counts trading bars only -- weekend gaps don't
        //    advance it. Using m_bars_seen - entry_bar_seq matches v3's
        //    `range(entry_idx + 1, entry_idx + TIME_STOP_BARS + 1)` window.
        if (m_pos.active && !m_pos.time_stop_pending) {
            const int64_t elapsed = static_cast<int64_t>(m_bars_seen)
                                  - m_pos.entry_bar_seq;
            if (elapsed >= TIME_STOP_BARS) {
                m_pos.time_stop_pending = true;
            }
        }

        // 3. Mitigation check on existing pending FVGs against the just-
        //    closed bar (= m_recent_bars[2]). Production single-position:
        //    only run when no position is active (queue-and-wait semantics).
        //    Verifier first-touch-only mode: run on every bar so FVGs that
        //    first-touch during an open trade are overlap-skipped inside
        //    the function (matching v3 run_backtest semantics).
        const bool mit_warm     = m_atr_ready && m_tv_mean > 0.0;
        const bool mit_pos_ok   = first_touch_only_mode || !m_pos.active;
        if (mit_warm && mit_pos_ok) {
            try_mitigate_pending(can_enter, on_close);
        }

        // 4. Detect a new FVG formed at m_recent_bars[2]. The new FVG is
        //    NOT eligible for mitigation against the bar in which it forms
        //    (v3 measure_reactions starts at formed_idx + 1).
        //    Production: only detect when no position is active (matches
        //    the queue-and-wait posture of mitigation). Verifier
        //    first-touch-only mode: detect on every bar so the candidate
        //    pool matches v3 detect_fvgs(), which is trade-state-agnostic.
        const bool det_warm     = m_atr_ready && m_bars_seen >= 3;
        const bool det_pos_ok   = first_touch_only_mode || !m_pos.active;
        if (det_warm && det_pos_ok) {
            try_detect_fvg();
        }

        // 5. Bar-level SL/TP exit (verifier first-touch-only mode).
        //    Bit-equivalent to v3 simulate_trade's per-bar
        //      for k in range(entry_idx + 1, last + 1):
        //          sl_hit = bar_l <= sl   (long); bar_h >= sl  (short)
        //          tp_hit = bar_h >= tp   (long); bar_l <= tp  (short)
        //          if sl_hit and tp_hit -> SL wins; gross_exit = sl
        //    Order matters: this runs AFTER mitigation/detection (steps 3-4)
        //    so an FVG that first-touches its zone on the SAME bar this
        //    position closes is overlap-skipped at step 3 (m_pos.active is
        //    still true at that point), matching v3's
        //      next_open_after = exit_idx + 1
        //    semantics. Skipped only on the entry bar itself because v3's
        //    iteration starts at entry_idx + 1.
        //    NOT skipped when time_stop_pending is set: v3's SL/TP range
        //    is `range(entry_idx + 1, last + 1)` with `last = entry_idx +
        //    60`, INCLUDING bar entry_idx + 60 -- the same bar the
        //    engine's time-stop check at step 2 sets the pending flag on.
        //    If SL/TP fires here, _close runs first and the deferred
        //    TIME_STOP at the next bar's first tick is suppressed by its
        //    own `m_pos.active` guard.
        if (first_touch_only_mode && m_pos.active
            && m_recent_bars[2].bar_idx > m_pos.entry_bar_idx) {
            const ClosedBar& b = m_recent_bars[2];
            const bool sl_hit = m_pos.is_long ? (b.low  <= m_pos.sl)
                                               : (b.high >= m_pos.sl);
            const bool tp_hit = m_pos.is_long ? (b.high >= m_pos.tp)
                                               : (b.low  <= m_pos.tp);
            if (sl_hit) {
                // SL-wins-on-tie (matches v3's simulate_trade ordering).
                // Exit at gross sl; _close applies side_cost so the
                // recorded tr.exitPrice equals v3's
                //   exit_price = gross_exit - side_cost (long) /
                //              = gross_exit + side_cost (short).
                _close(m_pos.sl, "SL_HIT", b.start_ts,
                       on_close ? on_close : on_close_cb);
            } else if (tp_hit) {
                _close(m_pos.tp, "TP_HIT", b.start_ts,
                       on_close ? on_close : on_close_cb);
            }
        }

        // 6. Age out pending FVGs whose age exceeds MAX_AGE_BARS.
        age_out_pending();
    }

    // -- FVG detection (matches detect_fvgs() in core) ----------------------
    void try_detect_fvg() noexcept
    {
        const ClosedBar& b0 = m_recent_bars[0];   // i-2
        const ClosedBar& b1 = m_recent_bars[1];   // i-1 (displacement bar)
        const ClosedBar& b2 = m_recent_bars[2];   // i   (formation bar)

        char   direction;
        double zone_low, zone_high, gap;
        if (b0.high < b2.low) {                   // bullish FVG
            direction = 'B';
            gap       = b2.low - b0.high;
            zone_low  = b0.high;
            zone_high = b2.low;
        } else if (b0.low > b2.high) {            // bearish FVG
            direction = 'S';
            gap       = b0.low - b2.high;
            zone_low  = b2.high;
            zone_high = b0.low;
        } else {
            return;                               // no FVG
        }

        if (m_atr14 <= 0.0) return;
        const double ratio = gap / m_atr14;
        if (ratio < MIN_GAP_ATR) return;
        if (ratio > MAX_GAP_ATR) return;

        // Score components, all clipped to [0, 1].
        // (a) Gap-size: (ratio - 0.10) / (2.0 - 0.10), clipped.
        const double s_gap = clamp01((ratio - MIN_GAP_ATR) / (2.0 - MIN_GAP_ATR));

        // (b) Displacement: |close-open|/range, weighted by clamp(range/ATR/2, 0, 1).
        const double mid_body  = std::fabs(b1.close - b1.open);
        const double mid_range = b1.high - b1.low;
        double s_disp = 0.0;
        if (mid_range > 0.0 && m_atr14 > 0.0) {
            const double body_frac  = mid_body / mid_range;
            const double range_atr  = mid_range / m_atr14;
            const double range_term = clamp01(range_atr / 2.0);
            s_disp = clamp01(body_frac * range_term);
        }

        // (c) Tick-volume: middle-bar tick_count / rolling 20-bar mean.
        //     1x -> 0, 3x -> 1.
        //
        //     BAR-INDEX ALIGNMENT (verifier-bug fix 2026-05-02):
        //     v3 / core's detect_fvgs() does:
        //         tv_ratio = tc[i - 1] / tv_mean[i - 1]
        //     i.e., it reads tv_mean AT THE MIDDLE BAR (i-1) -- the rolling
        //     mean over bars [i-20 ... i-1], EXCLUDING the formation bar.
        //     m_tv_mean_at_prev_bar holds exactly that snapshot (taken
        //     inside update_indicators() BEFORE the formation bar's
        //     tick_count was pushed into the rolling deque). Reading
        //     m_tv_mean here would use [i-19 ... i] (= v3's tv_mean[i]),
        //     which is what produced the row-1 score_at_entry diff of
        //     0.001149 against trades_top.csv.
        double s_tv = 0.0;
        if (m_tv_mean_at_prev_bar > 0.0) {
            const double tv_ratio = static_cast<double>(b1.tick_count)
                                  / m_tv_mean_at_prev_bar;
            s_tv = clamp01((tv_ratio - 1.0) / 2.0);
        }

        // Production weighted average. trend_align and age_decay drop out
        // at weight 0; we omit them entirely (matches v3 numerically).
        const double score = (W_GAP_SIZE * s_gap
                            + W_DISPLACEMENT * s_disp
                            + W_TICK_VOLUME * s_tv) / W_SUM;

        // Queue size guard (soft -- shouldn't ever bind on XAUUSD 15m, but
        // a misbehaved data feed could spam FVGs and we don't want to grow
        // unbounded). Drop oldest.
        if ((int)m_pending.size() >= MAX_PENDING_FVGS) {
            m_pending.pop_front();
        }

        PendingFvg fv{};
        fv.bar_idx_formed    = b2.bar_idx;
        fv.formed_ts         = b2.start_ts;
        fv.direction         = direction;
        fv.zone_low          = zone_low;
        fv.zone_high         = zone_high;
        fv.gap_height        = gap;
        fv.s_gap_size        = s_gap;
        fv.s_displacement    = s_disp;
        fv.s_tick_volume     = s_tv;
        fv.score_at_formation = score;
        m_pending.push_back(fv);

        char _buf[512];
        snprintf(_buf, sizeof(_buf),
            "[XAU-FVG] FVG_FORMED dir=%c zone=[%.3f,%.3f] gap=%.3f "
            "atr=%.3f ratio=%.2f score=%.3f s_gap=%.3f s_disp=%.3f s_tv=%.3f "
            "queue=%zu\n",
            direction, zone_low, zone_high, gap, m_atr14, ratio,
            score, s_gap, s_disp, s_tv, m_pending.size());
        std::cout << _buf;
        std::cout.flush();
    }

    // -- Mitigation entry on the just-closed bar -----------------------------
    // For each pending FVG, ask: did m_recent_bars[2] re-enter the zone?
    //   bar.high >= fv.zone_low  AND  bar.low <= fv.zone_high
    // First match that passes the score gate AND the entry overlays opens
    // a position. v3 measure_reactions uses the same first-touch convention.
    void try_mitigate_pending(bool can_enter, CloseCallback on_close) noexcept
    {
        if (m_pending.empty()) return;
        const ClosedBar& bar = m_recent_bars[2];

        for (auto it = m_pending.begin(); it != m_pending.end(); ) {
            const PendingFvg& fv = *it;
            // Skip if the FVG was formed at this very bar -- v3 starts the
            // mitigation search at formed_idx + 1.
            if (fv.bar_idx_formed >= bar.bar_idx) { ++it; continue; }

            const bool entered = (bar.high >= fv.zone_low)
                              && (bar.low  <= fv.zone_high);
            if (!entered) { ++it; continue; }

            // With production weights (W_AGE_DECAY = 0), score_at_entry
            // equals score_at_formation -- age decay drops out.
            const double score_at_entry = fv.score_at_formation;

            // Verifier hook: positive `score_cutoff_override` substitutes the
            // static SCORE_CUTOFF so the synthetic-trace verifier can replay
            // against a top-decile pool. Production callers leave the override
            // at <=0 and behaviour is byte-identical to the static gate.
            const double effective_cutoff =
                (score_cutoff_override > 0.0) ? score_cutoff_override
                                              : SCORE_CUTOFF;
            if (score_at_entry < effective_cutoff) {
                // Gate failed. v3 marks the FVG entered but does not trade
                // sub-cutoff scores. Drop it from pending (one-shot).
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] MIT_GATE_FAIL dir=%c score=%.3f cutoff=%.2f "
                    "zone=[%.3f,%.3f]\n",
                    fv.direction, score_at_entry, SCORE_CUTOFF,
                    fv.zone_low, fv.zone_high);
                std::cout << _buf; std::cout.flush();
                it = m_pending.erase(it);
                continue;
            }

            // -- First-touch overlap-skip (verifier-only) -----------------
            // When first_touch_only_mode is on, this function runs even
            // while a position is active. An FVG that first-touches its
            // zone now is consumed: if it falls inside the previous
            // trade's overlap window we drop it (overlap-skip, matching
            // v3 run_backtest's
            //   `if int(fv.entry_idx) < next_open_after: skipped_overlap`
            // where next_open_after = exit_idx + 1).
            //
            // Two conditions trigger overlap-skip:
            //   (a) m_pos.active -- a trade is currently open, covers
            //       entry-bar overlap and SL/TP-exit-bar overlap (the
            //       SL/TP exit happens at step 5 of the same on_bar_close
            //       AFTER mitigation at step 3, so m_pos.active is still
            //       true here).
            //   (b) bar.bar_idx <= m_last_exit_bar_idx -- covers
            //       TIME_STOP, which fires at the FIRST TICK of the exit
            //       bar (= bar entry_idx + 61) BEFORE that bar's
            //       on_bar_close runs. By the time mitigation reaches
            //       this point, m_pos.active is false but the exit bar's
            //       overlap claim is recorded in m_last_exit_bar_idx.
            //
            // In production (flag false) both branches are unreachable
            // because the on_bar_close gate already prevents the
            // function from running while m_pos.active is true.
            if (first_touch_only_mode &&
                (m_pos.active || bar.bar_idx <= m_last_exit_bar_idx)) {
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] MIT_OVERLAP_SKIP dir=%c score=%.3f "
                    "zone=[%.3f,%.3f]\n",
                    fv.direction, score_at_entry,
                    fv.zone_low, fv.zone_high);
                std::cout << _buf; std::cout.flush();
                it = m_pending.erase(it);
                continue;
            }

            // -- Entry-overlay gates ---------------------------------------
            if (!can_enter) {
                // Cohort-side block (gold_any_open / supervisor / latency).
                // Don't drop the FVG -- mitigation already happened, but
                // we cannot enter. v3 has no equivalent; mark as one-shot
                // taken to avoid re-firing on subsequent bars (which would
                // be a forward-looking entry that the backtest would NOT
                // have taken).
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] MIT_BLOCKED_COHORT dir=%c score=%.3f "
                    "zone=[%.3f,%.3f]\n",
                    fv.direction, score_at_entry, fv.zone_low, fv.zone_high);
                std::cout << _buf; std::cout.flush();
                it = m_pending.erase(it);
                continue;
            }
            // Spread gate: verifier hook can disable this entirely. v3 (the
            // bit-exact reference) has no spread gate, and the engine's
            // adaptive ABS_FLOOR/ABS_CEIL thresholds were tuned for forex
            // pairs at sub-pip price scale, not for XAUUSD whose raw
            // spreads run ~1+ USD. Production callers leave the flag off.
            if (!spread_gate_disabled && !m_spread_gate.can_fire()) {
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] MIT_BLOCKED_SPREAD dir=%c score=%.3f\n",
                    fv.direction, score_at_entry);
                std::cout << _buf; std::cout.flush();
                it = m_pending.erase(it);
                continue;
            }
#ifndef OMEGA_BACKTEST
            if (g_news_blackout.is_blocked("XAUUSD", m_last_tick_s)) {
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] MIT_BLOCKED_NEWS dir=%c score=%.3f\n",
                    fv.direction, score_at_entry);
                std::cout << _buf; std::cout.flush();
                it = m_pending.erase(it);
                continue;
            }
#endif

            // -- Open position --------------------------------------------
            const int fvg_age_bars = static_cast<int>(bar.bar_idx
                                                    - fv.bar_idx_formed);
            open_position(fv, bar, score_at_entry, fvg_age_bars);
            (void)on_close;  // close fires from manage_position(), not here

            // First-touch-only mode: keep iterating remaining pending FVGs
            // so any that ALSO entered their zones on this same bar see
            // m_pos.active = true and are overlap-skipped. This matches
            // v3 run_backtest where multiple FVGs sharing the same
            // entry_idx are sorted-stable in formation order; the first
            // candidate fills, subsequent candidates with entry_idx <
            // next_open_after are skipped.
            if (first_touch_only_mode) {
                it = m_pending.erase(it);
                continue;
            }

            // Production single-position semantics: stop after first fill.
            // Other pending FVGs that also entered their zones on this bar
            // stay in the queue and will be re-evaluated on the next bar
            // (after the position closes).
            m_pending.erase(it);
            return;
        }
    }

    void open_position(const PendingFvg& fv,
                       const ClosedBar& bar,
                       double score_at_entry,
                       int fvg_age_bars) noexcept
    {
        const double half_spread = (bar.spread_mean > 0.0)
                                 ? (bar.spread_mean * 0.5)
                                 : 0.0;
        const double side_cost   = half_spread + SLIPPAGE_PRICE;

        double gross_entry, sl, tp;
        bool   is_long;
        if (fv.direction == 'B') {
            gross_entry = fv.zone_high;          // long: buy-limit at top of gap
            sl          = gross_entry - SL_ATR_MULT * m_atr14;
            tp          = gross_entry + TP_ATR_MULT * m_atr14;
            is_long     = true;
        } else {
            gross_entry = fv.zone_low;           // short: sell-limit at bottom of gap
            sl          = gross_entry + SL_ATR_MULT * m_atr14;
            tp          = gross_entry - TP_ATR_MULT * m_atr14;
            is_long     = false;
        }
        const double entry_with_cost = is_long
                                     ? (gross_entry + side_cost)
                                     : (gross_entry - side_cost);

        const double risk_per_unit = SL_ATR_MULT * m_atr14;
        if (risk_per_unit <= 0.0) return;        // safety

        // Sizing: $50 / (risk_per_unit * 100 USD per price-point per lot).
        const double calc_lot = RISK_DOLLARS
                              / (risk_per_unit * USD_PER_PRICE_PER_LOT);
        const double size     = std::max(LOT_MIN, std::min(LOT_MAX, calc_lot));

        m_pos = LivePos{};
        m_pos.active            = true;
        m_pos.is_long           = is_long;
        m_pos.entry             = gross_entry;
        m_pos.entry_with_cost   = entry_with_cost;
        m_pos.tp                = tp;
        m_pos.sl                = sl;
        m_pos.size              = size;
        m_pos.atr_at_entry      = m_atr14;
        m_pos.score_at_entry    = score_at_entry;
        m_pos.spread_at_entry   = bar.spread_mean;
        m_pos.side_cost         = side_cost;
        m_pos.gap_height        = fv.gap_height;
        // entry_ts is the START timestamp of the mitigation bar, NOT the
        // moment of the first tick of the next bar (when on_bar_close
        // happens to fire). This makes entry_ts consistent with
        // entry_bar_idx (which already references the mitigation bar) and
        // matches the v3 backtest reference, where entry_time = times[entry_idx]
        // for the bar that mitigated. Without this fix, entry_ts was off
        // by exactly BAR_SECS (one bar later) compared to the v3 ledger.
        m_pos.entry_ts          = bar.start_ts;
        m_pos.entry_bar_idx     = bar.bar_idx;
        // Snapshot the sequential closed-bar counter at entry. The TIME_STOP
        // check in on_bar_close() compares against this (NOT entry_bar_idx)
        // so weekend gaps don't artificially advance the counter. See the
        // entry_bar_seq field comment in the LivePos struct for the full
        // rationale. m_bars_seen is incremented in close_current_bar(),
        // which runs BEFORE on_bar_close() (and therefore before this
        // open_position() call), so at this moment m_bars_seen already
        // reflects the entry bar inclusively -- the next sequential close
        // (entry bar + 1) will see m_bars_seen == entry_bar_seq + 1.
        m_pos.entry_bar_seq     = static_cast<int64_t>(m_bars_seen);
        m_pos.fvg_age_bars      = fvg_age_bars;
        // Session label is the FORMATION bar's session, NOT the entry bar's.
        // 2026-05-03 v3-alignment fix: v3's Trade.session is sourced from
        // fv.session, which detect_fvgs() set via classify_session(times[i])
        // at the FORMATION bar (index i). When formation and entry straddle
        // a session boundary -- common for late-Off / early-Asian FVGs that
        // form at 23:45 UTC and mitigate at 00:15 UTC the next bar -- the
        // labels differ. Previously this used session_for_ts(bar.start_ts)
        // (entry bar), which produced "asian" where v3 reported "off" on
        // exactly those straddle cases (e.g. trades_top.csv row 3 in the
        // XAUUSD 15min top10 backtest). The side-channel CSV writer feeds
        // this into the quarterly re-validation pipeline, which compares
        // against trades_top.csv -- so reporting formation session here
        // keeps the verifier and any future re-feed in alignment with v3.
        m_pos.session           = session_for_ts(fv.formed_ts);
        m_pos.direction         = fv.direction;
        m_pos.time_stop_pending = false;

        char _buf[512];
        snprintf(_buf, sizeof(_buf),
            "[XAU-FVG] FILL %s @ %.3f sl=%.3f tp=%.3f lot=%.3f atr=%.3f "
            "score=%.3f age=%d sess=%c gap=%.3f spread=%.3f cost=%.4f\n",
            is_long ? "LONG" : "SHORT",
            gross_entry, sl, tp, size, m_atr14,
            score_at_entry, fvg_age_bars, m_pos.session,
            fv.gap_height, bar.spread_mean, side_cost);
        std::cout << _buf;
        std::cout.flush();
    }

    // -- Per-tick management (mirrors S59 minus BE / minus trailing) --------
    // Live SL/TP/time-stop on every tick using bid/ask. Time-stop is set as
    // a deferred flag inside on_bar_close() and fires on the FIRST tick of
    // the next bar (= open of the new bar in the bar accumulator).
    void manage_position(double bid, double ask, double mid,
                         int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!m_pos.active) return;

        // MFE / MAE tracking on mid (matches v3 fav_excursion semantics).
        const double move = m_pos.is_long
                          ? (mid - m_pos.entry)
                          : (m_pos.entry - mid);
        if (move > m_pos.mfe) m_pos.mfe = move;
        if (move < m_pos.mae) m_pos.mae = move;

        // Verifier first-touch-only mode: SL/TP fires AT BAR CLOSE from
        // on_bar_close using bar.high/low against gross sl/tp prices --
        // bit-equivalent to v3 simulate_trade's `bar_l <= sl` / `bar_h >= tp`
        // check and `gross_exit = sl/tp` price. Skipping per-tick here is
        // what eliminates the half-spread early-trigger drift that ends the
        // engine's overlap window earlier than v3's exit_idx and lets
        // subsequent FVGs that v3 would have overlap-skipped fire.
        if (first_touch_only_mode) return;

        // SL takes precedence on intrabar tie -- mirrors v3 simulate_trade()
        // which classifies "if sl_hit and tp_hit" as a stop.
        const bool sl_hit = m_pos.is_long ? (bid <= m_pos.sl)
                                          : (ask >= m_pos.sl);
        if (sl_hit) {
            const double exit_px = m_pos.is_long ? bid : ask;
            // Differentiate from BE -- v3 uses "be_sl" only when sl was
            // slid to entry. With BE off in production, all stops are SL.
            _close(exit_px, "SL_HIT", now_s, on_close);
            return;
        }
        const bool tp_hit = m_pos.is_long ? (ask >= m_pos.tp)
                                          : (bid <= m_pos.tp);
        if (tp_hit) {
            const double exit_px = m_pos.is_long ? ask : bid;
            _close(exit_px, "TP_HIT", now_s, on_close);
            return;
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> _lk(m_close_mtx);
        if (!m_pos.active) return;

        // Snapshot under lock.
        const bool   is_long_         = m_pos.is_long;
        const double entry_           = m_pos.entry;
        const double entry_w_cost_    = m_pos.entry_with_cost;
        const double sl_              = m_pos.sl;
        const double tp_              = m_pos.tp;
        const double size_            = m_pos.size;
        const double mfe_             = m_pos.mfe;
        const double mae_             = m_pos.mae;
        const double atr_at_entry_    = m_pos.atr_at_entry;
        const double score_at_entry_  = m_pos.score_at_entry;
        const double spread_at_entry_ = m_pos.spread_at_entry;
        const double side_cost_       = m_pos.side_cost;
        const double gap_height_      = m_pos.gap_height;
        const int64_t entry_ts_       = m_pos.entry_ts;
        const int64_t entry_bar_      = m_pos.entry_bar_idx;
        const int    fvg_age_bars_    = m_pos.fvg_age_bars;
        const char   session_         = m_pos.session;
        const char   direction_       = m_pos.direction;

        // Net exit price after side cost (mirrors v3: exit_price minus
        // side_cost on exit). Reported exitPrice on TradeRecord uses the net
        // value so the cost is captured in the PnL difference exitPrice-entryPrice.
        const double exit_with_cost = is_long_
                                    ? (exit_px - side_cost_)
                                    : (exit_px + side_cost_);

        // PnL math: emit the raw price-distance * size and let
        // handle_closed_trade apply tick_value_multiplier (XAUUSD = 100).
        // For consistency with v3's net_pnl model, we use the cost-adjusted
        // entry/exit prices in the difference.
        const double pnl = (is_long_
                            ? (exit_with_cost - entry_w_cost_)
                            : (entry_w_cost_  - exit_with_cost)) * size_;

        // Sanity clamp (S59 lineage). XAUUSD at size <= 0.20 with reasonable
        // SL/TP shouldn't produce |pnl_raw| > 0.20 * 100 = 20. Cap at 5x.
        const double sane_max = std::max(0.01, size_) * 100.0;
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            std::ostringstream warn;
            warn << "[XAU-FVG][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry_w_cost=" << entry_w_cost_
                 << " exit_w_cost=" << exit_with_cost
                 << " raw_exit=" << exit_px
                 << "). Emitting clamped to sane_max=" << sane_max << ".\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = (pnl > 0.0) ? sane_max : -sane_max;
        }

        const int bars_held = static_cast<int>(
            (m_recent_bars[2].bar_idx >= entry_bar_)
            ? (m_recent_bars[2].bar_idx - entry_bar_)
            : 0);

        {
            std::ostringstream os;
            os << "[XAU-FVG] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ " << std::fixed << std::setprecision(3) << exit_px
               << " reason=" << reason
               << " pnl_raw=" << std::setprecision(5) << pnl_to_emit
               << " mfe="    << std::setprecision(3) << mfe_
               << " mae="    << mae_
               << " bars="   << bars_held
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        omega::TradeRecord tr;
        tr.id            = ++m_trade_id;
        tr.symbol        = "XAUUSD";
        tr.side          = is_long_ ? "LONG" : "SHORT";
        tr.engine        = "XauusdFvg";
        tr.regime        = "FVG_15M";
        tr.entryPrice    = entry_w_cost_;
        tr.exitPrice     = exit_with_cost;
        tr.tp            = tp_;
        tr.sl            = sl_;
        tr.size          = size_;
        tr.pnl           = pnl_to_emit;
        tr.net_pnl       = tr.pnl;
        tr.mfe           = mfe_ * size_;
        tr.mae           = mae_ * size_;
        tr.entryTs       = entry_ts_;
        // exitTs is the START timestamp of the bar in which the exit
        // triggered (matches v3 backtest's exit_time = times[exit_idx]).
        // Using m_cur_bar.start_ts gives:
        //   * SL/TP intrabar exit -- start of the bar whose tick crossed
        //     the level (the engine's per-tick check fires inside this bar
        //     before close_current_bar() runs, so m_cur_bar still refers
        //     to that bar).
        //   * TIME_STOP exit -- start of the next bar after the time-stop
        //     bar boundary (the engine's deferred-close head-of-on_tick
        //     fires after open_new_bar(), so m_cur_bar.start_ts is the
        //     new bar's start, == v3's o_arr[time_stop_idx + 1] timestamp).
        // Previously this used `now_s` (the tick's exact timestamp), which
        // drifted up to BAR_SECS-1 from the bar boundary the v3 ledger
        // records.
        tr.exitTs        = m_cur_bar.start_ts;
        tr.exitReason    = reason;
        tr.spreadAtEntry = spread_at_entry_;
        tr.atr_at_entry  = atr_at_entry_;
        tr.shadow        = shadow_mode;
        // Bracket fields not used by FVG (not a bracket engine).
        tr.bracket_hi    = 0.0;
        tr.bracket_lo    = 0.0;

        // Quarterly re-validation extras for the side-channel CSV writer.
        m_last_extras.score_at_entry = score_at_entry_;
        m_last_extras.atr_at_entry   = atr_at_entry_;
        m_last_extras.gap_height     = gap_height_;
        m_last_extras.session        = session_;
        m_last_extras.fvg_age_bars   = fvg_age_bars_;
        m_last_extras.bars_held      = bars_held;
        m_last_extras.direction      = direction_;

        // Reset position state BEFORE firing the callback so any check
        // inside the callback sees a clean engine.
        m_pos = LivePos{};

        // Track exit bar for verifier first-touch overlap-skip extension.
        // m_cur_bar.bar_idx at the moment of _close reflects the bar in
        // which the exit was decided:
        //   * SL/TP from on_bar_close step 5 -- m_cur_bar still holds the
        //     just-closed bar (close_current_bar copied to m_recent_bars
        //     but did not reset m_cur_bar; open_new_bar runs AFTER
        //     on_bar_close returns) so m_cur_bar.bar_idx == the exit bar.
        //   * SL/TP from manage_position (production path) -- m_cur_bar
        //     is the in-progress bar where the tick fired.
        //   * TIME_STOP from on_tick first-tick path -- open_new_bar has
        //     already run, so m_cur_bar.bar_idx == bar of the exit price
        //     (= v3's exit_idx for the time_stop case).
        // In all three, this aligns with v3 simulate_trade's exit_idx.
        m_last_exit_bar_idx = m_cur_bar.bar_idx;

        if (on_close) on_close(tr);
    }

    // -- Aging / housekeeping ----------------------------------------------
    void age_out_pending() noexcept
    {
        if (m_pending.empty()) return;
        const int64_t cur = m_recent_bars[2].bar_idx;
        while (!m_pending.empty()) {
            const PendingFvg& f = m_pending.front();
            if (cur - f.bar_idx_formed > MAX_AGE_BARS) {
                char _buf[256];
                snprintf(_buf, sizeof(_buf),
                    "[XAU-FVG] AGEOUT dir=%c zone=[%.3f,%.3f] age=%lld bars\n",
                    f.direction, f.zone_low, f.zone_high,
                    static_cast<long long>(cur - f.bar_idx_formed));
                std::cout << _buf; std::cout.flush();
                m_pending.pop_front();
            } else {
                break;  // remainder are newer; queue is FIFO by formation
            }
        }
    }

    // -- Utilities ----------------------------------------------------------
    static double clamp01(double x) noexcept
    {
        if (x < 0.0) return 0.0;
        if (x > 1.0) return 1.0;
        return x;
    }

    // UTC-hour session classification matching core
    // usdjpy_xauusd_fvg_signal_test.py:classify_session().
    //   Asian  00-07 UTC  -> 'A'
    //   London 07-12 UTC  -> 'L'
    //   NY     12-21 UTC  -> 'N'
    //   Off    21-24 UTC  -> 'O'
    static char session_for_ts(int64_t now_s) noexcept
    {
        time_t t = static_cast<time_t>(now_s);
        struct tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        const int h = utc.tm_hour;
        if (h >= 0  && h <  7)  return 'A';
        if (h >= 7  && h < 12)  return 'L';
        if (h >= 12 && h < 21)  return 'N';
        return 'O';
    }
};

} // namespace omega
