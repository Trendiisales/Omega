// =============================================================================
//  CandleFlowEngine.hpp
//
//  Strategy (updated -- sweep-optimised against real L2 data 2026-04-10):
//    ENTRY:  expansion candle (quality gate) + RSI trend direction
//    EXIT:   L2 imbalance reversal (sustained N ticks) OR stagnation
//
//  Entry conditions (ALL required):
//    1. Expansion candle: body >= 60% of range, close breaks prev high/low
//    2. Bar range >= 2.5 * cost (spread + slippage + commission)
//    3. RSI slope EMA direction agrees with candle (period=30, ema=10, thresh=6.0)
//       RSI replaces old DOM entry confirmation -- DOM is EXIT only
//
//  Exit conditions:
//    Primary: L2 imbalance flips against position for >= 2 consecutive ticks
//             imbalance threshold = 0.08 (calibrated to cTrader level-count data)
//             AND minimum hold time of 20s elapsed (prevents noise exits)
//    Safety:  move < cost * 1.0 within 60s stagnation window
//    Hard SL: 1x ATR behind entry
//
//  Sweep result (20,736 configs, real cTrader L2 data 2026-04-10):
//    RSI_P=30  EMA=10  THRESH=6.0  IMB=0.08  IMB_TICKS=2
//    BODY=0.60  COST_MULT=2.5  STAG=60s
//    -> 14 trades/day, 71.4% WR, R:R=1.90, exp=$8.13/trade, maxDD=$19.68
//
//  2026-04-11 fixes:
//    - CFE_IMB_EXIT_TICKS restored to 2 (was changed to 1 -- too hair-trigger)
//    - CFE_IMB_MIN_HOLD_MS = 20000: IMB_EXIT cannot fire before 20s in trade
//      Root cause of 8s/37s exits: single-tick imbalance noise fired immediately
//      after entry. 20s minimum hold eliminates sub-10s exits entirely.
//
//  2026-04-13 fixes:
//    - DFE RSI level gate: block LONG when rsi_cur < 35 (oversold bounce),
//      block SHORT when rsi_cur > 65 (overbought bounce). Prevents DFE entering
//      into spent mean-reversion moves that look like trends by slope EMA alone.
//      Root cause: 08:01 UTC LONG entry at rsi_cur~28 (rsi_trend=9.05) on a
//      dead-cat bounce at London open. rsi_trend looked bullish but raw RSI said
//      oversold reversal -- position lost $88 in 16s.
//    - DFE drift persistence gate: require drift >= threshold for >= 2 consecutive
//      ticks before DFE arms. Eliminates single-tick London-open spike entries
//      (drift shot from -0.14 -> +2.11 in one tick, delta=2.25 passed accel gate).
//    - DFE price-action confirmation: require last 3 mid prices moving in drift
//      direction. Prevents ghost drift entries where EWM reads bullish but price
//      is actually dropping.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <functional>
#include <deque>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iomanip>
#include "OmegaTradeLedger.hpp"
#include "OmegaFIX.hpp"          // L2Book, L2Level
#include "GoldHMM.hpp"            // 3-state regime HMM for CFE entry gating

namespace omega {

// -----------------------------------------------------------------------------
//  Config -- sweep-optimised values
// -----------------------------------------------------------------------------
static constexpr double  CFE_BODY_RATIO_MIN    = 0.60;
static constexpr double  CFE_COST_SLIPPAGE     = 0.10;
static constexpr double  CFE_COMMISSION_PTS    = 0.10;
static constexpr double  CFE_COST_MULT         = 2.0;   // lowered 2.5->2.0: 2.5x was blocking
                                                          // borderline setups (1.47pt range blocked
                                                          // when move went $8+). 2.0x still requires
                                                          // bar covers cost with margin.
static constexpr int64_t CFE_STAGNATION_MS     = 90000;  // Asia default -- London/NY uses 180s (session-aware in on_tick)
                                                           // 60s was exiting before moves developed.
                                                           // 0.37pt MFE in 60s -> exited, move went $8.
static constexpr double  CFE_STAGNATION_MULT   = 1.0;   // exit if mfe < cost*1.0
static constexpr double  CFE_RISK_DOLLARS      = 30.0;
static constexpr double  CFE_MIN_LOT           = 0.01;
// MAX_LOT reduced from 0.50 to 0.20 to 0.10: at $30 risk, dollar_stop=$50,
// correct size = 30/(1.4*100) = 0.214 lots. 0.20 is a hard safety ceiling
// so even if ATR computation goes wrong again, damage is capped.
// 0.50 lots was 3x the correct size and caused $111 loss on a stale ATR tick.
static constexpr double  CFE_MAX_LOT           = 0.10;  // reduced 0.20->0.10: dollar_stop=$50 at 0.20=2.5pt room, at 0.10=5pt room

// RSI trend (entry direction signal)
static constexpr int     CFE_RSI_PERIOD        = 30;    // tick RSI lookback
static constexpr int     CFE_RSI_EMA_N         = 10;    // slope EMA smoothing
static constexpr double  CFE_RSI_THRESH        = 3.0;   // min slope EMA to enter (multi-day sweep: rn=30 rt=3.0 best $47.50/6d)

// RSI LEVEL gates for DFE (2026-04-13):
// Block DFE LONG when raw RSI < 35 -- oversold bounce territory.
// Block DFE SHORT when raw RSI > 65 -- overbought bounce territory.
// These are not for bar-based entries (those already require expansion candle
// breaking prev high/low). DFE specifically fires on drift before candle close
// -- without this gate it enters into moves that already spent most of their energy.
// Evidence: 08:01 UTC LONG rsi_cur~28, rsi_trend=+9.05, lost $88 in 16s.
static constexpr double  CFE_DFE_RSI_LEVEL_LONG_MIN  = 35.0;  // DFE LONG blocked below this RSI level
static constexpr double  CFE_DFE_RSI_LEVEL_SHORT_MAX = 65.0;  // DFE SHORT blocked above this RSI level

// DFE drift persistence (2026-04-13):
// DFE requires drift >= threshold for this many consecutive ticks before arming.
// Eliminates single-tick London-open spike entries.
// 2 ticks = ~0.2-0.5s at London open tick rate -- real drift sustains, spikes don't.
static constexpr int     CFE_DFE_DRIFT_PERSIST_TICKS = 2;

// DFE sustained-drift path (2026-04-13):
// Secondary entry path for slow grinding trends that never spike above the main threshold.
// Evidence: 08:01-08:08 UTC downtrend, drift stayed at -0.8 to -1.7 for 5+ minutes
// but never hit -1.5 for 2 consecutive ticks -- main DFE never fired.
// Logic: if drift has been consistently in [-0.8, -1.5) range for >= SUSTAINED_MS,
// that IS a trend -- enter without requiring the spike gate.
// Threshold lower (-0.8) to catch early sustained moves.
// Hold duration >= 45s required to distinguish trend from noise oscillation.
// Same RSI level + price confirmation gates still apply.
static constexpr double  CFE_DFE_DRIFT_SUSTAINED_THRESH = 0.2;   // 6-day sweep: st=0.20 sm=60s best $31.50/day
static constexpr int64_t CFE_DFE_DRIFT_SUSTAINED_MS     = 60000; // 6-day sweep: sm=60s best $31.50/day
// ATR-normalised SUS entry threshold: max(0.8, atr * 0.30)
// At ATR=2: max(0.8, 0.60) = 0.80pt   At ATR=3: max(0.8, 0.90) = 0.90pt
// At ATR=4: max(0.8, 1.20) = 1.20pt   Evidence: 0.8pt fixed threshold fires
// on normal noise when ATR=2-3. Backtest: 3223 SUS entries, 21% WR, -6k over 2yr.
// Bar-based entry trend context gate (2026-04-13):
// Block bar LONG entries when drift has been sustainedly negative (>= 45s below -0.5).
// Block bar SHORT entries when drift has been sustainedly positive (>= 45s above +0.5).
// Prevents CFE firing a LONG bar entry into a 7-minute downtrend (08:08 incident).
static constexpr double  CFE_BAR_TREND_BLOCK_DRIFT = 0.5;   // sustained drift threshold
static constexpr int64_t CFE_BAR_TREND_BLOCK_MS    = 45000; // how long drift must persist to block

// DFE price-action confirmation (2026-04-13):
// Last N mid prices must be moving in the drift direction.
// Prevents entries where EWM drift is bullish but price is actually falling.
static constexpr int     CFE_DFE_PRICE_CONFIRM_TICKS = 3;    // how many recent ticks to check
static constexpr double  CFE_DFE_PRICE_CONFIRM_MIN   = 0.05; // min net move in drift direction

// L2 imbalance exit (cTrader depth level-count signal)
// imbalance = dom.l2_imb converted to -1..+1: (l2_imb - 0.5) * 2
// Exit long when imb < -CFE_IMB_EXIT_THRESH for >= CFE_IMB_EXIT_TICKS ticks
static constexpr double  CFE_IMB_EXIT_THRESH   = 0.08;  // calibrated to cTrader level-count data (0.4545/0.5455 = imb +/-0.091)
static constexpr int     CFE_IMB_EXIT_TICKS    = 2;     // restored to 2: single tick is noise, need 2 consecutive
static constexpr int64_t CFE_IMB_MIN_HOLD_MS   = 20000; // IMB_EXIT blocked for first 20s after entry
                                                          // prevents immediate noise exits (8s/37s trades confirmed as noise)

// Drift fast-entry (DFE) -- pre-empts bar-close requirement
static constexpr double  CFE_DFE_DRIFT_THRESH   = 1.5;   // |ewm_drift| >= 1.5pts to arm
static constexpr double  CFE_DFE_DRIFT_ACCEL     = 0.2;   // drift must be growing
static constexpr double  CFE_DFE_RSI_THRESH      = 3.0;   // RSI trend EMA minimum
static constexpr double  CFE_DFE_RSI_TREND_MAX   = 12.0;  // RSI trend EMA maximum
                                                            // Above 12 = momentum exhausted,
                                                            // entering late into a spent move.
                                                            // Data: rsi_trend=20.62 on -$59 loss,
                                                            // rsi_trend=6-9 on all winners.
static constexpr double  CFE_DFE_SL_MULT         = 0.4;   // SL = 0.4 * ATR (sweep-optimised: sl=0.40 best $122.90)
static constexpr double  CFE_MAX_ATR_ENTRY       = 6.0;   // block ALL entries when ATR > 6pt
                                                            // At ATR=6pt: SL=4.2pt, 4 max loss at 0.20 lots
                                                            // At ATR=12pt: SL=8.4pt, 68 loss -- not a scalp
                                                            // High-ATR regimes belong to GoldFlow/MacroCrash
static constexpr int64_t CFE_DFE_COOLDOWN_MS     = 120000; // 120s block after DFE loss
static constexpr double  CFE_DFE_MIN_SPREAD_MULT = 1.5;   // max spread vs cost

// -----------------------------------------------------------------------------
struct CandleFlowEngine {

    // -------------------------------------------------------------------------
    // Public config
    double risk_dollars = CFE_RISK_DOLLARS;
    bool   shadow_mode  = true;

    // HMM regime gate -- 3-state Gaussian HMM (CONTINUATION/MEAN_REV/NOISE).
    // Seeded with session-aware priors from backtest evidence.
    // update() called at M1 bar close; p_continuation() gates both DFE + bar entry.
    // Fail-open: !warmed() -> no gating, returns p=1.0.
    // Shadow gate: when HMM_GATING_LIVE=false, logs [CFE-HMM-GATE] but never blocks.
    static constexpr bool HMM_GATING_LIVE = false;  // flip to true after shadow validation
    omega::GoldHMM m_hmm;
    double         m_hmm_last_p   = 1.0;   // last p_continuation (for logging)
    int            m_hmm_last_state = omega::HMM_CONTINUATION;

    // -------------------------------------------------------------------------
    enum class Phase { IDLE, LIVE, COOLDOWN } phase = Phase::IDLE;

    struct OpenPos {
        bool    active        = false;
        bool    is_long       = false;
        double  entry         = 0.0;
        double  sl            = 0.0;
        double  size          = 0.01;     // full size (set at entry, halved after partial)
        double  full_size     = 0.01;     // original size before any partial exit
        double  cost_pts      = 0.0;
        int64_t entry_ts_ms   = 0;
        double  mfe           = 0.0;
        bool    trail_active  = false;    // true once MFE >= 1x cost -> trail engaged
        double  trail_sl      = 0.0;     // current trailing SL price
        bool    partial_done  = false;   // true after 50% partial exit at 2x cost
        double  atr_pts       = 0.0;     // ATR at entry, used for trail distance
    } pos;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // -------------------------------------------------------------------------
    // DOM snapshot -- passed in every tick from the L2Book
    // l2_imb: level-count imbalance from imbalance_level(), range 0..1
    //         (bid_count / (bid_count + ask_count))
    //         0.5 = balanced, >0.5 = bid pressure, <0.5 = ask pressure
    struct DOMSnap {
        int    bid_count    = 0;
        int    ask_count    = 0;
        double bid_vol      = 0.0;
        double ask_vol      = 0.0;
        double l2_imb       = 0.5;   // level-count imbalance 0..1
        bool   vacuum_ask   = false;
        bool   vacuum_bid   = false;
        bool   wall_above   = false;
        bool   wall_below   = false;
        int    prev_bid_count = 0;
        int    prev_ask_count = 0;
        double prev_bid_vol   = 0.0;
        double prev_ask_vol   = 0.0;
    };

    // -------------------------------------------------------------------------
    // Bar snapshot -- one completed M1 candle
    struct BarSnap {
        double open      = 0.0;
        double high      = 0.0;
        double low       = 0.0;
        double close     = 0.0;
        double prev_high = 0.0;
        double prev_low  = 0.0;
        bool   valid     = false;
    };

    // -------------------------------------------------------------------------
    // Main tick function
    // Called on every XAUUSD tick. RSI is computed internally from mid price.
    // bar: last closed M1 bar
    // dom: current DOM snapshot (build_dom() from L2Book)
    // now_ms: epoch ms
    // atr_pts: current ATR for SL sizing
    // -------------------------------------------------------------------------
    void on_tick(double bid, double ask,
                 const BarSnap& bar,
                 const DOMSnap& dom,
                 int64_t now_ms,
                 double  atr_pts,
                 CloseCallback on_close,
                 double ewm_drift  = 0.0,
                 double tick_rate  = 0.0) noexcept
    {
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;

        // -- RSI update (unconditional, every tick) ---------------------------
        rsi_update(mid);

        // -- Price history update (unconditional, every tick) -----------------
        // Used by DFE price-action confirmation gate.
        m_recent_mid.push_back(mid);
        if ((int)m_recent_mid.size() > CFE_DFE_PRICE_CONFIRM_TICKS + 2)
            m_recent_mid.pop_front();

        // -- DOM history ------------------------------------------------------
        m_dom_prev = m_dom_cur;
        m_dom_cur  = dom;

        // -- Cooldown ---------------------------------------------------------
        if (phase == Phase::COOLDOWN) {
            if (now_ms - m_cooldown_start_ms >= m_cooldown_ms)
                phase = Phase::IDLE;
            else
                return;
        }

        // -- Manage open position ---------------------------------------------
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, dom, now_ms, on_close);
            return;
        }

        // -- IDLE: check for entry --------------------------------------------

        // -- DRIFT FAST-ENTRY (DFE) ----------------------------------------------------
        // Pre-empts bar-close when drift >= threshold and RSI confirms.
        //
        // SESSION-AWARE + ATR-NORMALISED THRESHOLD (2026-04-13, updated):
        //   Asia (22:00-07:00 UTC): drift must be >= max(4.0, atr * 0.40).
        //     Rationale: Asia ewm_drift oscillates on thin liquidity. A 1.5pt
        //     reading at 02:00 UTC is noise -- the same reading at 09:00 UTC
        //     London is a real signal. Two consecutive bad DFE trades in Asia
        //     (01:40 LONG -$59, 01:59 SHORT -$29) both fired on sub-2pt drift.
        //     4.0pt requires a genuine sustained directional move, not a bounce.
        //     ATR-normalised: max(4.0, atr*0.40) -- on a 12pt crash day Asia
        //     threshold rises to 4.8pt, further protecting against noise entries.
        //   London/NY (07:00-22:00 UTC): max(CFE_DFE_DRIFT_THRESH, atr * 0.30).
        //     On a 5pt ATR day  : max(1.5, 1.5) = 1.5pt (unchanged).
        //     On a 10pt ATR day : max(1.5, 3.0) = 3.0pt (volatile session).
        //     On a 12pt ATR day : max(1.5, 3.6) = 3.6pt (crash/spike day).
        //     Prevents DFE firing on normal drift during high-vol sessions.
        //     atr_safe fallback = 5.0pt when ATR not yet warmed (first few ticks).
        //
        // UTC hour derived from now_ms -- no external dependency needed.
        {
            const int64_t utc_sec  = now_ms / 1000LL;
            const int      utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
            const bool     in_asia        = (utc_hour >= 22 || utc_hour < 7);
            // London open (07:00-08:30 UTC): raise DFE threshold to Asia level.
            // Gap spikes at London open look like drift but reverse immediately.
            // Evidence: 07:46 SHORT -$77, 08:01 SHORT -$57 both fired on gap spikes.
            // Threshold raised to Asia level: requires genuine sustained move, not a gap.
            const int64_t utc_min      = static_cast<int>((utc_sec % 3600LL) / 60LL);
            const int     utc_mins_day = utc_hour * 60 + static_cast<int>(utc_min);
            const bool     lon_open    = (utc_mins_day >= 420 && utc_mins_day < 510); // 07:00-08:30
            const double   atr_safe = (atr_pts > 0.0) ? atr_pts : 5.0;
            m_dfe_eff_thresh = (in_asia || lon_open)
                ? std::max(4.0,                  atr_safe * 0.40)
                : std::max(CFE_DFE_DRIFT_THRESH, atr_safe * 0.30);
        }

        // DFE drift persistence tracking (2026-04-13):
        // Count consecutive ticks where |drift| >= threshold in same direction.
        // Reset counter when drift drops below threshold or changes direction.
        if (std::fabs(ewm_drift) >= m_dfe_eff_thresh) {
            const int drift_dir = (ewm_drift > 0) ? 1 : -1;
            if (drift_dir == m_dfe_persist_dir) {
                m_dfe_persist_ticks++;
            } else {
                // Direction changed or first tick above threshold
                m_dfe_persist_dir   = drift_dir;
                m_dfe_persist_ticks = 1;
            }
        } else {
            // Drift dropped below threshold -- reset persistence
            m_dfe_persist_dir   = 0;
            m_dfe_persist_ticks = 0;
        }

        // Sustained-drift tracking (2026-04-13):
        // Update m_drift_sustained_start_ms / m_drift_sustained_dir every tick.
        // Tracks how long drift has been continuously above CFE_DFE_DRIFT_SUSTAINED_THRESH
        // in one direction. Used by sustained-drift DFE path and bar trend-block gate.
        {
            const int new_sus_dir = (ewm_drift >= CFE_DFE_DRIFT_SUSTAINED_THRESH)  ?  1
                                  : (ewm_drift <= -CFE_DFE_DRIFT_SUSTAINED_THRESH) ? -1
                                  : 0;
            if (new_sus_dir != 0 && new_sus_dir == m_drift_sustained_dir) {
                // Continuing same direction -- track peak
                if (std::fabs(ewm_drift) > m_sus_drift_peak)
                    m_sus_drift_peak = std::fabs(ewm_drift);
            } else if (new_sus_dir != 0) {
                // New direction started
                m_drift_sustained_dir      = new_sus_dir;
                m_drift_sustained_start_ms = now_ms;
                m_sus_drift_peak           = std::fabs(ewm_drift);
            } else {
                // Drift below threshold -- reset
                m_drift_sustained_dir      = 0;
                m_drift_sustained_start_ms = 0;
                m_sus_drift_peak           = 0.0;
            }
        }
        const int64_t drift_sustained_ms = (m_drift_sustained_dir != 0 && m_drift_sustained_start_ms > 0)
            ? (now_ms - m_drift_sustained_start_ms) : 0;

        // HMM update: call at every tick but only re-runs EM every HMM_UPDATE_INTERVAL bars.
        // Builds feature vector from current tick inputs and adds to rolling window.
        {
            const omega::HmmFeature hmm_feat = omega::HmmFeature::make(
                ewm_drift, atr_pts, m_rsi_trend, tick_rate,
                (bar.valid ? (bar.high - bar.low) : 0.0),
                drift_sustained_ms);
            m_hmm.update(hmm_feat);
            m_hmm_last_p     = m_hmm.p_continuation(hmm_feat);
            m_hmm_last_state = m_hmm.current_state(hmm_feat);
            m_hmm.log_params(now_ms);
        }

    // Opposite-direction cooldown: after a SHORT closes, block new LONG for 60s
        // (and vice versa). Prevents the rapid flip-flop pattern (09:45 / 10:16).
        if (m_last_closed_ms > 0 && m_last_closed_dir != 0) {
            const int64_t since_close = now_ms - m_last_closed_ms;
            if (since_close < CFE_OPPOSITE_DIR_COOLDOWN_MS) {
                const int intended_dir = (ewm_drift > 0) ? +1 : -1;
                if (intended_dir != m_last_closed_dir) {
                    static int64_t s_opp_log = 0;
                    if (now_ms - s_opp_log > 20000) {
                        s_opp_log = now_ms;
                        {
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg), "[CFE-OPP-DIR] %s blocked: last_dir=%+d %llds ago\n",                                (intended_dir>0?"LONG":"SHORT"), m_last_closed_dir,                                (long long)since_close/1000);
                            std::cout << _msg;
                            std::cout.flush();
                        }
                    }
                    m_prev_ewm_drift = ewm_drift;
                    return;
                }
            }
        }

        // Adverse-excursion re-entry filter (2026-04-15)
        // After a loss where price moved >= 0.5 ATR against us, block same-direction
        // re-entry until price recovers to within 0.5x ATR of the loss exit price.
        // Structurally prevents entering into ongoing adverse moves.
        if (m_adverse_block && m_last_loss_dir != 0 && m_last_loss_exit_px > 0.0) {
            const double mid_now = (bid + ask) * 0.5;
            // Distance from current price back to where we lost
            const double dist_to_exit = (m_last_loss_dir == +1)
                ? (m_last_loss_exit_px - mid_now)   // LONG loss: exit was below, need price near exit
                : (mid_now - m_last_loss_exit_px);  // SHORT loss: exit was above
            const double recovery_thresh = m_last_loss_atr * 0.5;
            const bool intended_same_dir = (ewm_drift > 0 && m_last_loss_dir == +1)
                                        || (ewm_drift < 0 && m_last_loss_dir == -1);
            if (intended_same_dir && dist_to_exit > recovery_thresh) {
                static int64_t s_adv_log = 0;
                if (now_ms - s_adv_log > 20000) {
                    s_adv_log = now_ms;
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[CFE-ADVERSE-BLOCK] %s blocked: loss_exit=%.2f dist=%.2f need=%.2f atr=%.2f\n",                            (m_last_loss_dir==+1?"LONG":"SHORT"),                            m_last_loss_exit_px, dist_to_exit, recovery_thresh, m_last_loss_atr);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
                return;
            }
            if (!intended_same_dir || dist_to_exit <= recovery_thresh)
                m_adverse_block = false;
        }

        // HMM regime gate for DFE entry.
        // Blocks entry when P(CONTINUATION) < HMM_MIN_PROB and model is warmed.
        // HMM_GATING_LIVE=false: logs only (shadow). =true: blocks entry.
        if (m_hmm.warmed()) {
            if (m_hmm_last_p < omega::HMM_MIN_PROB) {
                static int64_t s_hmm_dfe_log = 0;
                if (now_ms - s_hmm_dfe_log > 5000) {
                    s_hmm_dfe_log = now_ms;
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[CFE-HMM-GATE] DFE blocked: state=%s p_cont=%.3f < %.2f drift=%.2f%s\n",                            omega::GoldHMM::state_name(m_hmm_last_state),                            m_hmm_last_p, omega::HMM_MIN_PROB, ewm_drift,                            HMM_GATING_LIVE ? "" : " [SHADOW-LOG-ONLY]");
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
                if (HMM_GATING_LIVE) {
                    m_prev_ewm_drift = ewm_drift;
                    return;
                }
            }
        }

        if (m_rsi_warmed && std::fabs(ewm_drift) >= m_dfe_eff_thresh) {
            const double drift_delta = ewm_drift - m_prev_ewm_drift;
            const bool drift_accel = m_dfe_warmed &&
                ((ewm_drift > 0 && drift_delta >= CFE_DFE_DRIFT_ACCEL) ||
                 (ewm_drift < 0 && drift_delta <= -CFE_DFE_DRIFT_ACCEL));
            m_prev_ewm_drift = ewm_drift; m_dfe_warmed = true;
            const bool dfe_long = (ewm_drift > 0);

            // RSI slope direction + exhaustion gate (unchanged)
            const bool rsi_ok = dfe_long
                ? (m_rsi_trend > CFE_DFE_RSI_THRESH
                   && m_rsi_trend < CFE_DFE_RSI_TREND_MAX)   // not exhausted
                : (m_rsi_trend < -CFE_DFE_RSI_THRESH
                   && m_rsi_trend > -CFE_DFE_RSI_TREND_MAX); // not exhausted
            if (!rsi_ok && std::fabs(m_rsi_trend) > CFE_DFE_RSI_TREND_MAX) {
                // Log RSI exhaustion block so it's visible in logs
                static int64_t s_exhaust_log = 0;
                if (now_ms - s_exhaust_log > 10000) {
                    s_exhaust_log = now_ms;
                    std::cout << "[CFE-EXHAUSTED] DFE blocked rsi_trend="
                              << std::fixed << std::setprecision(2) << m_rsi_trend
                              << " > max=" << CFE_DFE_RSI_TREND_MAX
                              << " drift=" << ewm_drift << " -- move already spent\n";
                    std::cout.flush();
                }
            }

            // RSI LEVEL gate (2026-04-13):
            // Block DFE LONG when raw RSI is in oversold territory (< 35).
            // An oversold RSI + positive slope EMA = dead-cat bounce entering late.
            // Block DFE SHORT when raw RSI is in overbought territory (> 65).
            // Evidence: 08:01 UTC LONG with rsi_cur~28, rsi_trend=9.05 -> -$88.
            const bool rsi_level_ok = dfe_long
                ? (m_rsi_cur >= CFE_DFE_RSI_LEVEL_LONG_MIN)   // LONG: RSI must be >= 35
                : (m_rsi_cur <= CFE_DFE_RSI_LEVEL_SHORT_MAX);  // SHORT: RSI must be <= 65
            if (!rsi_level_ok) {
                static int64_t s_level_log = 0;
                if (now_ms - s_level_log > 10000) {
                    s_level_log = now_ms;
                    std::cout << "[CFE-RSI-LEVEL] DFE blocked rsi_cur="
                              << std::fixed << std::setprecision(2) << m_rsi_cur
                              << (dfe_long ? " < min=" : " > max=")
                              << (dfe_long ? CFE_DFE_RSI_LEVEL_LONG_MIN : CFE_DFE_RSI_LEVEL_SHORT_MAX)
                              << " drift=" << ewm_drift
                              << " -- oversold/overbought bounce blocked\n";
                    std::cout.flush();
                }
            }

            // Drift persistence gate (2026-04-13):
            // Require drift >= threshold for >= 2 consecutive ticks.
            // Eliminates single-tick London-open spike entries.
            const bool persist_ok = (m_dfe_persist_ticks >= CFE_DFE_DRIFT_PERSIST_TICKS);
            if (!persist_ok) {
                static int64_t s_persist_log = 0;
                if (now_ms - s_persist_log > 5000) {
                    s_persist_log = now_ms;
                    std::cout << "[CFE-DFE-PERSIST] drift=" << std::fixed << std::setprecision(2)
                              << ewm_drift << " persist_ticks=" << m_dfe_persist_ticks
                              << " < required=" << CFE_DFE_DRIFT_PERSIST_TICKS
                              << " -- single-tick spike blocked\n";
                    std::cout.flush();
                }
            }

            // Price-action confirmation gate (2026-04-13):
            // Last CFE_DFE_PRICE_CONFIRM_TICKS mid prices must be moving in drift direction.
            // Prevents ghost drift entries where EWM reads bullish but price is falling.
            bool price_confirms = false;
            if ((int)m_recent_mid.size() >= CFE_DFE_PRICE_CONFIRM_TICKS) {
                const double oldest = m_recent_mid[m_recent_mid.size() - CFE_DFE_PRICE_CONFIRM_TICKS];
                const double net_move = mid - oldest;
                price_confirms = dfe_long
                    ? (net_move >= CFE_DFE_PRICE_CONFIRM_MIN)   // LONG: price rising
                    : (net_move <= -CFE_DFE_PRICE_CONFIRM_MIN);  // SHORT: price falling
            } else {
                // Not enough history yet -- don't block (edge case on startup)
                price_confirms = true;
            }
            if (!price_confirms) {
                static int64_t s_price_log = 0;
                if (now_ms - s_price_log > 5000) {
                    s_price_log = now_ms;
                    const double oldest = (int)m_recent_mid.size() >= CFE_DFE_PRICE_CONFIRM_TICKS
                        ? m_recent_mid[m_recent_mid.size() - CFE_DFE_PRICE_CONFIRM_TICKS] : mid;
                    std::cout << "[CFE-DFE-PRICE] drift=" << std::fixed << std::setprecision(2)
                              << ewm_drift << " but price net=" << (mid - oldest)
                              << " over " << CFE_DFE_PRICE_CONFIRM_TICKS
                              << " ticks -- price moving against drift, blocked\n";
                    std::cout.flush();
                }
            }

            if (drift_accel && rsi_ok && rsi_level_ok && persist_ok && price_confirms &&
                (now_ms >= m_dfe_cooldown_until)) {
                // ATR cap: block CFE entry when ATR > CFE_MAX_ATR_ENTRY.
                // At ATR=12pt: SL=8.4pt = $168 loss at 0.20 lots -- not a scalp.
                const double dfe_atr_check = (atr_pts > 0.0) ? atr_pts : 5.0;
                if (dfe_atr_check > CFE_MAX_ATR_ENTRY) {
                    static int64_t s_atr_cap_log = 0;
                    if (now_ms - s_atr_cap_log > 30000) {
                        s_atr_cap_log = now_ms;
                        {
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg), "[CFE-ATR-CAP] DFE blocked: atr=%.2f > max=%.1f\n",                                dfe_atr_check, CFE_MAX_ATR_ENTRY);
                            std::cout << _msg;
                            std::cout.flush();
                        }
                    }
                } else {
                const double dfe_cost = spread + CFE_COST_SLIPPAGE*2.0 + CFE_COMMISSION_PTS*2.0;
                if (spread < dfe_cost * CFE_DFE_MIN_SPREAD_MULT) {
                    // Counter-spike block for DFE: if last 3 ticks moving hard against
                    // intended direction, price is spiking the wrong way -- block entry.
                    // Same logic as bar entry counter-spike gate.
                    if ((int)m_recent_mid.size() >= 3) {
                        const double dfe_spike_oldest = m_recent_mid[m_recent_mid.size() - 3];
                        const double dfe_spike_move   = mid - dfe_spike_oldest;
                        const double dfe_spike_thresh = (atr_pts > 0.0 ? atr_pts : 2.0) * 0.5;
                        const bool dfe_counter_spike  = dfe_long
                            ? (dfe_spike_move <= -dfe_spike_thresh)
                            : (dfe_spike_move >= dfe_spike_thresh);
                        if (dfe_counter_spike) {
                            static int64_t s_dfe_spike = 0;
                            if (now_ms - s_dfe_spike > 5000) {
                                s_dfe_spike = now_ms;
                                std::cout << "[CFE-DFE-SPIKE-BLOCK] " << (dfe_long?"LONG":"SHORT")
                                          << " blocked: counter-spike move=" << std::fixed
                                          << std::setprecision(2) << dfe_spike_move
                                          << " thresh=" << dfe_spike_thresh << "\n";
                                std::cout.flush();
                            }
                            m_prev_ewm_drift = ewm_drift;
                            goto cfe_sustained_skip;
                        }
                    }
                    const double dfe_atr    = (atr_pts > 0.0) ? atr_pts : spread * 5.0;
                    const double dfe_sl_pts = dfe_atr * CFE_DFE_SL_MULT;
                    const double entry_px   = dfe_long ? ask : bid;
                    const double sl_px      = dfe_long
                        ? (entry_px - dfe_sl_pts) : (entry_px + dfe_sl_pts);
                    double size = risk_dollars / (dfe_sl_pts * 100.0);
                    size = std::floor(size/0.001)*0.001;
                    size = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, size));
                    pos.active=true; pos.is_long=dfe_long; pos.entry=entry_px;
                    pos.sl=sl_px; pos.size=size; pos.full_size=size;
                    pos.cost_pts=dfe_cost; pos.entry_ts_ms=now_ms;
                    pos.mfe=0.0; pos.trail_active=false; pos.trail_sl=sl_px;
                    pos.partial_done=false; pos.atr_pts=dfe_atr;
                    ++m_trade_id; phase=Phase::LIVE;
                    m_imb_against_ticks=0;
                    m_prev_depth_bid=m_dom_cur.bid_count;
                    m_prev_depth_ask=m_dom_cur.ask_count;
                    std::cout << "[CFE] DRIFT-ENTRY " << (dfe_long?"LONG":"SHORT")
                              << " @ " << std::fixed << std::setprecision(2) << entry_px
                              << " sl=" << sl_px << " drift=" << ewm_drift
                              << " delta=" << drift_delta << " rsi=" << m_rsi_trend
                              << " rsi_cur=" << m_rsi_cur
                              << " thresh=" << m_dfe_eff_thresh
                              << " persist=" << m_dfe_persist_ticks
                              << " size=" << std::setprecision(3) << size
                              << (shadow_mode?" [SHADOW]":"") << "\n";
                    std::cout.flush(); return;
                }
                } // end ATR-cap else
            }
        } else { m_prev_ewm_drift=ewm_drift; m_dfe_warmed=true; }

        // -- SUSTAINED-DRIFT ENTRY (secondary DFE path) ----------------------------
        // Fires when drift has been consistently in one direction for >= 45s but
        // never spiked high enough for the main DFE gate.
        // Evidence: 08:01-08:08 UTC SHORT missed because drift peaked at -1.72 once
        // but persistence gate needed 2 consecutive ticks. 5 minutes of -0.8 to -1.7
        // drift = a real trend, not noise.
        // Gates: sustained >= 45s, RSI level ok, price confirming, not in cooldown.
        // Does NOT require drift_accel -- sustained slow drift IS the signal.
        //
        // ASIA BLOCK (2026-04-14): sustained-drift entry DISABLED in Asia (22:00-07:00 UTC).
        // Root cause of 5 consecutive losses on 2026-04-14 (00:15-00:31 UTC):
        // In thin Asia tape, drift of 0.8pt sustained for 45s is pure chop noise.
        // The same 0.8pt reading at London open (08:00 UTC) is a real grinding trend.
        // Asia chop oscillates above/below 0.8pt threshold continuously -- this path
        // fired 3-4 times in 16 minutes on meaningless sub-1pt drift wiggles.
        // Fix: only allow sustained-drift entries during London/NY sessions (07:00-22:00 UTC).
        // The spike DFE path (threshold = max(4.0, atr*0.40) in Asia) still runs.
        {
            const int64_t sus_utc_sec  = now_ms / 1000LL;
            const int     sus_utc_hour = static_cast<int>((sus_utc_sec % 86400LL) / 3600LL);
            const bool    sus_in_asia  = (sus_utc_hour >= 22 || sus_utc_hour < 7);
            if (sus_in_asia) {
                static int64_t s_sus_asia_log = 0;
                if (now_ms - s_sus_asia_log > 120000) {
                    s_sus_asia_log = now_ms;
                    std::cout << "[CFE-SUS-ASIA-BLOCK] sustained-drift entry disabled in Asia session\n";
                    std::cout.flush();
                }
                // Skip sustained-drift check entirely in Asia -- go straight to bar-based entry
                goto cfe_sustained_skip;
            }
        }
        if (m_rsi_warmed &&
            m_drift_sustained_dir != 0 &&
            drift_sustained_ms >= CFE_DFE_DRIFT_SUSTAINED_MS &&
            now_ms >= m_dfe_cooldown_until)
        {
            // REVERSED (2026-04-15): 2yr backtest showed 14.8% WR following drift direction.
            // Below-random WR = entries are ANTI-correlated with outcome.
            // Sustained drift = exhaustion signal, not continuation signal.
            // When drift sustains positive for 90s, the move is spent -> fade SHORT.
            // When drift sustains negative for 90s, reversal due -> fade LONG.
            const bool sus_long = (m_drift_sustained_dir == -1);  // FADE: negative drift = go LONG
            // RSI slope must be reversing (fading the move)
            const bool sus_rsi_ok = sus_long
                ? (m_rsi_trend > CFE_DFE_RSI_THRESH && m_rsi_trend < CFE_DFE_RSI_TREND_MAX)
                : (m_rsi_trend < -CFE_DFE_RSI_THRESH && m_rsi_trend > -CFE_DFE_RSI_TREND_MAX);
            // RSI level gate: same as spike DFE
            const bool sus_rsi_level_ok = sus_long
                ? (m_rsi_cur >= CFE_DFE_RSI_LEVEL_LONG_MIN)
                : (m_rsi_cur <= CFE_DFE_RSI_LEVEL_SHORT_MAX);
            // Price-action confirmation: last 3 ticks moving in drift direction
            bool sus_price_ok = false;
            if ((int)m_recent_mid.size() >= CFE_DFE_PRICE_CONFIRM_TICKS) {
                const double oldest = m_recent_mid[m_recent_mid.size() - CFE_DFE_PRICE_CONFIRM_TICKS];
                const double net = mid - oldest;
                sus_price_ok = sus_long ? (net >= CFE_DFE_PRICE_CONFIRM_MIN)
                                        : (net <= -CFE_DFE_PRICE_CONFIRM_MIN);
            } else {
                sus_price_ok = true;
            }
            const double sus_spread = ask - bid;
            const double sus_cost = sus_spread + CFE_COST_SLIPPAGE*2.0 + CFE_COMMISSION_PTS*2.0;
            const bool sus_spread_ok = (sus_spread < sus_cost * CFE_DFE_MIN_SPREAD_MULT);

            if (sus_rsi_ok && sus_rsi_level_ok && sus_price_ok && sus_spread_ok) {
                const double sus_atr    = (atr_pts > 0.0) ? atr_pts : sus_spread * 5.0;
                // ATR-normalised threshold: require drift >= max(0.8, atr*0.30) to confirm trend
                const double sus_drift_min = std::max(CFE_DFE_DRIFT_SUSTAINED_THRESH, sus_atr * 0.30);
                if (std::fabs(ewm_drift) < sus_drift_min) {
                    static int64_t s_sus_drift_log = 0;
                    if (now_ms - s_sus_drift_log > 30000) {
                        s_sus_drift_log = now_ms;
                        {
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg), "[CFE-SUS-DRIFT-BLOCK] |drift|=%.2f < min=%.2f (atr=%.2f)\n",                                std::fabs(ewm_drift), sus_drift_min, sus_atr);
                            std::cout << _msg;
                            std::cout.flush();
                        }
                    }
                    goto cfe_sustained_skip;
                }
                // Drift momentum check: require drift is still >= 70% of its peak
                // during this sustained run. If drift has fallen to 50% of peak,
                // the move is exhausting -- we are entering at the reversal not the trend.
                // Evidence: 14.8% WR over 2yr = entering at drift exhaustion consistently.
                // m_sus_drift_peak tracks the max |drift| seen in the current sustained run.
                if (m_sus_drift_peak > 0.0 && std::fabs(ewm_drift) < m_sus_drift_peak * 0.70) {
                    static int64_t s_sus_peak_log = 0;
                    if (now_ms - s_sus_peak_log > 30000) {
                        s_sus_peak_log = now_ms;
                        {
                            char _msg[512];
                            snprintf(_msg, sizeof(_msg), "[CFE-SUS-PEAK] drift=%.2f < 70pct peak=%.2f -- exhausting\n", std::fabs(ewm_drift), m_sus_drift_peak);
                            std::cout << _msg;
                            std::cout.flush();
                        }
                    }
                    goto cfe_sustained_skip;
                }
                // SL raised 0.7->1.5x ATR: 0.7xATR was too tight for noise at entry ATR.
                // Evidence: 74% SL rate over 2yr backtest. At ATR=2: old SL=.40, new=.00.
                // The same ATR that sustained the drift signal must be the SL floor.
                const double sus_sl_pts = sus_atr * 1.5;
                const double entry_px   = sus_long ? ask : bid;
                const double sl_px      = sus_long ? (entry_px - sus_sl_pts) : (entry_px + sus_sl_pts);
                double size = risk_dollars / (sus_sl_pts * 100.0);
                size = std::floor(size/0.001)*0.001;
                size = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, size));
                pos.active=true; pos.is_long=sus_long; pos.entry=entry_px;
                pos.sl=sl_px; pos.size=size; pos.full_size=size;
                pos.cost_pts=sus_cost; pos.entry_ts_ms=now_ms;
                pos.mfe=0.0; pos.trail_active=false; pos.trail_sl=sl_px;
                pos.partial_done=false; pos.atr_pts=sus_atr;
                ++m_trade_id; phase=Phase::LIVE;
                m_imb_against_ticks=0;
                m_prev_depth_bid=m_dom_cur.bid_count;
                m_prev_depth_ask=m_dom_cur.ask_count;
                // Reset sustained tracker to avoid re-firing immediately
                m_drift_sustained_start_ms = now_ms;
                std::cout << "[CFE] SUSTAINED-DRIFT-ENTRY " << (sus_long?"LONG":"SHORT")
                          << " @ " << std::fixed << std::setprecision(2) << entry_px
                          << " sl=" << sl_px << " drift=" << ewm_drift
                          << " sustained_ms=" << drift_sustained_ms
                          << " rsi=" << m_rsi_trend
                          << " rsi_cur=" << m_rsi_cur
                          << " size=" << std::setprecision(3) << size
                          << (shadow_mode?" [SHADOW]":"") << "\n";
                std::cout.flush(); return;
            }
        }

        cfe_sustained_skip:
        // -- Standard bar-based entry -----------------------------------------------
        if (!bar.valid) return;
        if (!m_rsi_warmed) return;

        // Gate -2: Asia session bar quality gate (22:00-05:00 UTC)
        // Asia bar entries allowed ONLY with very tight confirmation:
        //   1. Drift must be >= max(3.0, atr*0.40) -- real macro move, not chop
        //   2. RSI trend must strongly agree with direction (|rsi_trend| >= 8.0)
        //   3. Last 3 ticks price confirming direction
        // This catches genuine overnight macro moves (gold +$30 on news) while
        // blocking the noise entries that caused consistent overnight losses.
        {
            const int64_t asia_sec  = now_ms / 1000LL;
            const int     asia_hour = static_cast<int>((asia_sec % 86400LL) / 3600LL);
            const bool    in_asia   = (asia_hour >= 22 || asia_hour < 5);
            if (in_asia) {
                const double asia_atr_safe = (atr_pts > 0.0) ? atr_pts : 5.0;
                // Gate 1: high drift threshold
                const double asia_drift_min = std::max(3.0, asia_atr_safe * 0.40);
                if (std::fabs(ewm_drift) < asia_drift_min) {
                    static int64_t s_asia_drift_log = 0;
                    if (now_ms - s_asia_drift_log > 120000) {
                        s_asia_drift_log = now_ms;
                        std::cout << "[CFE-ASIA-BAR-BLOCK] drift=" << std::fixed
                                  << std::setprecision(2) << ewm_drift
                                  << " < min=" << asia_drift_min << " (Asia)\n";
                        std::cout.flush();
                    }
                    return;
                }
                // Gate 2: RSI trend must strongly agree
                const bool asia_bar_long = (ewm_drift > 0);
                const bool asia_rsi_ok = asia_bar_long
                    ? (m_rsi_trend >= 8.0)
                    : (m_rsi_trend <= -8.0);
                if (!asia_rsi_ok) {
                    static int64_t s_asia_rsi_log = 0;
                    if (now_ms - s_asia_rsi_log > 60000) {
                        s_asia_rsi_log = now_ms;
                        std::cout << "[CFE-ASIA-BAR-BLOCK] rsi_trend=" << std::fixed
                                  << std::setprecision(2) << m_rsi_trend
                                  << " insufficient for Asia entry\n";
                        std::cout.flush();
                    }
                    return;
                }
                // Gate 3: last 3 ticks price confirming direction
                if ((int)m_recent_mid.size() >= 3) {
                    const double asia_oldest = m_recent_mid[m_recent_mid.size() - 3];
                    const double asia_move   = mid - asia_oldest;
                    const double asia_thresh = asia_atr_safe * 0.15;
                    const bool asia_price_ok = asia_bar_long
                        ? (asia_move >= asia_thresh)
                        : (asia_move <= -asia_thresh);
                    if (!asia_price_ok) {
                        static int64_t s_asia_px_log = 0;
                        if (now_ms - s_asia_px_log > 30000) {
                            s_asia_px_log = now_ms;
                            std::cout << "[CFE-ASIA-BAR-BLOCK] price not confirming: move="
                                      << std::fixed << std::setprecision(2) << asia_move
                                      << " thresh=" << asia_thresh << " (Asia)\n";
                            std::cout.flush();
                        }
                        return;
                    }
                }
                // All Asia gates passed -- log it so we can validate
                static int64_t s_asia_pass_log = 0;
                if (now_ms - s_asia_pass_log > 5000) {
                    s_asia_pass_log = now_ms;
                    std::cout << "[CFE-ASIA-BAR-PASS] Asia bar entry gates passed:"
                              << " drift=" << std::fixed << std::setprecision(2) << ewm_drift
                              << " rsi_trend=" << m_rsi_trend
                              << " hour=" << asia_hour << "\n";
                    std::cout.flush();
                }
            }
        }

        // Gate -1: Post-NY dead-tape block (2026-04-15)
        // Block ALL bar entries 19:00-22:00 UTC. Evidence: 2026-04-14 session,
        // 10 consecutive losing longs 18:51-19:50 UTC in a 4839-4844 dead range.
        // Post-NY is thin tape with no institutional flow -- CFE bar signals are noise.
        // DFE path (high drift threshold) still runs in this window.
        {
            const int64_t pny_sec  = now_ms / 1000LL;
            const int     pny_hour = static_cast<int>((pny_sec % 86400LL) / 3600LL);
            const bool    post_ny  = (pny_hour >= 19 && pny_hour < 22);
            if (post_ny) {
                static int64_t s_pny_log = 0;
                if (now_ms - s_pny_log > 120000) {
                    s_pny_log = now_ms;
                    std::cout << "[CFE-POST-NY-BLOCK] bar entry blocked: UTC hour=" << pny_hour
                              << " (19-22 dead zone)\n";
                    std::cout.flush();
                }
                return;
            }
        }

        // Gate -0.75: Pre-London dead zone bar block (05:00-07:00 UTC)
        // Sydney close to London open -- thin liquidity, wide spreads, frequent spikes.
        // Bar entries in this window get caught by gap moves (05:59 SHORT -$56 evidence).
        // DFE path already blocked by Asia threshold. Block bar entries too.
        {
            const int64_t pre_lon_sec  = now_ms / 1000LL;
            const int     pre_lon_hour = static_cast<int>((pre_lon_sec % 86400LL) / 3600LL);
            const bool    pre_london   = (pre_lon_hour >= 5 && pre_lon_hour < 7);
            if (pre_london) {
                static int64_t s_pre_lon_log = 0;
                if (now_ms - s_pre_lon_log > 120000) {
                    s_pre_lon_log = now_ms;
                    std::cout << "[CFE-PRE-LON-BLOCK] bar entry blocked: UTC hour=" << pre_lon_hour
                              << " (05:00-07:00 dead zone)\n";
                    std::cout.flush();
                }
                return;
            }
        }

        // Gate -0.5: London open bar quality gate (07:00-08:30 UTC)
        // Bar entries during London open require drift sustained >= 90s.
        // Normal bar gate only requires 45s -- too loose for gap-spike environment.
        // DFE path already raised to Asia threshold above.
        // Evidence: 07:46 SHORT and 08:01 SHORT both fired within seconds of a gap spike.
        {
            const int64_t lon_utc_sec  = now_ms / 1000LL;
            const int     lon_utc_hour = static_cast<int>((lon_utc_sec % 86400LL) / 3600LL);
            const int     lon_utc_min  = static_cast<int>((lon_utc_sec % 3600LL) / 60LL);
            const int     lon_mins_day = lon_utc_hour * 60 + lon_utc_min;
            const bool    in_lon_open  = (lon_mins_day >= 420 && lon_mins_day < 510);
            if (in_lon_open && drift_sustained_ms < 90000LL) {
                static int64_t s_lon_log = 0;
                if (now_ms - s_lon_log > 30000) {
                    s_lon_log = now_ms;
                    std::cout << "[CFE-LON-QUALITY] bar blocked: London open, drift sustained="
                              << drift_sustained_ms/1000LL << "s < 90s\n";
                    std::cout.flush();
                }
                return;
            }
        }

        // Gate 0: Trend context gate (2026-04-13)
        // Block bar LONG entries when drift has been sustainedly negative for >= 45s.
        // Block bar SHORT entries when drift has been sustainedly positive for >= 45s.
        // Prevents entering against a confirmed ongoing trend on a single-bar signal.
        // Evidence: 08:08 UTC LONG bar entry fired into a 7-minute downtrend.
        if (drift_sustained_ms >= CFE_BAR_TREND_BLOCK_MS) {
            if (m_drift_sustained_dir == -1) {
                // Sustained downtrend -- block bar LONGs, allow bar SHORTs
                static int64_t s_bar_block_log = 0;
                if (now_ms - s_bar_block_log > 15000) {
                    s_bar_block_log = now_ms;
                    std::cout << "[CFE-BAR-TREND-BLOCK] LONG bar blocked: sustained downtrend "
                              << drift_sustained_ms/1000 << "s drift=" << ewm_drift << "\n";
                    std::cout.flush();
                }
                // Only proceed if the bar itself is bearish (allow SHORT with-trend entry)
                // If bar would produce LONG, skip. We'll check after rsi_dir.
                // Store the context so rsi_dir check can use it.
                // Simplest approach: pre-check candle direction here.
                if (bar.valid) {
                    const bool would_be_long = (bar.close > bar.open);
                    if (would_be_long) return;  // block counter-trend LONG bar entry
                }
            } else if (m_drift_sustained_dir == 1) {
                // Sustained uptrend -- block bar SHORTs
                if (bar.valid) {
                    const bool would_be_short = (bar.close < bar.open);
                    if (would_be_short) return;  // block counter-trend SHORT bar entry
                }
            }
        }

        // Gate 0b: Minimum drift gate for bar entry (2026-04-15)
        // Bar-close fires on RSI + candle shape alone -- no drift requirement.
        // Drift near zero means market is going nowhere. Require |drift| >= 0.3
        // AND drift sign must agree with intended direction.
        // Evidence: 19:xx longs drift~0.2-0.4, 14:xx longs in active downtrend.
        {
            const double bar_drift_abs = std::fabs(ewm_drift);
            if (bar_drift_abs < 0.3) {
                static int64_t s_drift_min_log = 0;
                if (now_ms - s_drift_min_log > 15000) {
                    s_drift_min_log = now_ms;
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[CFE-DRIFT-MIN] bar entry blocked: |drift|=%.2f < 0.3\n",                            bar_drift_abs);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
                return;
            }
        }

        // HMM regime gate for bar-based entry.
        // Same gate as DFE: block entry when P(CONTINUATION) < HMM_MIN_PROB.
        if (m_hmm.warmed()) {
            if (m_hmm_last_p < omega::HMM_MIN_PROB) {
                static int64_t s_hmm_bar_log = 0;
                if (now_ms - s_hmm_bar_log > 5000) {
                    s_hmm_bar_log = now_ms;
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[CFE-HMM-GATE] BAR blocked: state=%s p_cont=%.3f < %.2f drift=%.2f%s\n",                            omega::GoldHMM::state_name(m_hmm_last_state),                            m_hmm_last_p, omega::HMM_MIN_PROB, ewm_drift,                            HMM_GATING_LIVE ? "" : " [SHADOW-LOG-ONLY]");
                        std::cout << _msg;
                        std::cout.flush();
                    }
                }
                if (HMM_GATING_LIVE) return;
            }
        }

        // Gate 1: RSI trend direction
        const int rsi_dir = rsi_direction();
        if (rsi_dir == 0) return;

        // Gate 1b: RSI direction must agree with drift sign (2026-04-15)
        // Prevents: positive RSI slope (bullish) but negative drift (price falling).
        // This is the canonical false signal: RSI bounces on dead-cat, drift still negative.
        if (rsi_dir == +1 && ewm_drift < 0.0) {
            static int64_t s_rsi_drift_log = 0;
            if (now_ms - s_rsi_drift_log > 15000) {
                s_rsi_drift_log = now_ms;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[CFE-RSI-DRIFT-CONFLICT] LONG blocked: rsi_dir=+1 but drift=%.2f\n",                        ewm_drift);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
            return;
        }
        if (rsi_dir == -1 && ewm_drift > 0.0) {
            static int64_t s_rsi_drift_log2 = 0;
            if (now_ms - s_rsi_drift_log2 > 15000) {
                s_rsi_drift_log2 = now_ms;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[CFE-RSI-DRIFT-CONFLICT] SHORT blocked: rsi_dir=-1 but drift=%.2f\n",                        ewm_drift);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
            return;
        }

        // Gate 2: expansion candle agreeing with RSI direction
        const bool bullish = candle_is_bullish(bar);
        const bool bearish = candle_is_bearish(bar);
        if (rsi_dir == +1 && !bullish) return;
        if (rsi_dir == -1 && !bearish) return;

        // Gate 3: cost coverage
        const double cost_pts = spread + CFE_COST_SLIPPAGE * 2.0 + CFE_COMMISSION_PTS * 2.0;
        const double bar_range = bar.high - bar.low;
        if (bar_range < CFE_COST_MULT * cost_pts) {
            static int64_t s_log = 0;
            if (now_ms - s_log > 10000) {
                s_log = now_ms;
                std::cout << "[CFE-COST-BLOCK] range=" << std::fixed << std::setprecision(3)
                          << bar_range << " < min=" << CFE_COST_MULT * cost_pts
                          << " spread=" << spread << "\n";
                std::cout.flush();
            }
            return;
        }

        // All gates passed
        // ATR cap: block bar entries in high-vol regime (same as DFE)
        if (atr_pts > 0.0 && atr_pts > CFE_MAX_ATR_ENTRY) {
            static int64_t s_bar_atr_log = 0;
            if (now_ms - s_bar_atr_log > 30000) {
                s_bar_atr_log = now_ms;
                {
                    char _msg[512];
                    snprintf(_msg, sizeof(_msg), "[CFE-ATR-CAP] Bar entry blocked: atr=%.2f > max=%.1f\n",                        atr_pts, CFE_MAX_ATR_ENTRY);
                    std::cout << _msg;
                    std::cout.flush();
                }
            }
            return;
        }
        // Gate 4: counter-spike block (2026-04-15)
        // If price has moved >= 0.5x ATR against the intended entry direction
        // in the last 3 ticks, the market is spiking the wrong way -- block entry.
        // Evidence: 09:27:12 SHORT entry, price immediately spiked +2.66pts = dollar stop.
        // The bar closed bearish but the NEW bar was aggressively bullish -- bar signal was stale.
        {
            const bool bar_long = (rsi_dir == +1);
            if ((int)m_recent_mid.size() >= 3) {
                const double spike_oldest = m_recent_mid[m_recent_mid.size() - 3];
                const double spike_move   = mid - spike_oldest;
                const double spike_thresh = (atr_pts > 0.0 ? atr_pts : 2.0) * 0.5;
                const bool counter_spike  = bar_long
                    ? (spike_move <= -spike_thresh)   // LONG blocked: price falling fast
                    : (spike_move >= spike_thresh);    // SHORT blocked: price rising fast
                if (counter_spike) {
                    static int64_t s_spike_log = 0;
                    if (now_ms - s_spike_log > 5000) {
                        s_spike_log = now_ms;
                        std::cout << "[CFE-SPIKE-BLOCK] " << (bar_long ? "LONG" : "SHORT")
                                  << " blocked: counter-spike move=" << std::fixed << std::setprecision(2)
                                  << spike_move << " thresh=" << spike_thresh
                                  << " atr=" << atr_pts << "\n";
                        std::cout.flush();
                    }
                    return;
                }
            }
        }

        enter(rsi_dir == +1, bid, ask, spread, cost_pts, atr_pts, now_ms);
    }

    bool has_open_position() const noexcept { return phase == Phase::LIVE; }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!has_open_position()) return;
        close_pos(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

    // -------------------------------------------------------------------------
    // Build DOMSnap from L2Book + previous book
    // Uses imbalance_level() -- level-count based, correct for cTrader feed
    // where size_raw=0 is sent on depth quotes (level count is the real signal)
    // -------------------------------------------------------------------------
    static DOMSnap build_dom(const L2Book& book,
                              const L2Book& prev_book,
                              double /*mid_price*/) noexcept
    {
        DOMSnap d;
        d.bid_count = book.bid_count;
        d.ask_count = book.ask_count;
        d.bid_vol   = static_cast<double>(book.bid_count);
        d.ask_vol   = static_cast<double>(book.ask_count);

        d.prev_bid_count = prev_book.bid_count;
        d.prev_ask_count = prev_book.ask_count;
        d.prev_bid_vol   = static_cast<double>(prev_book.bid_count);
        d.prev_ask_vol   = static_cast<double>(prev_book.ask_count);

        // Level-count imbalance: primary signal for cTrader L2 feed
        d.l2_imb = book.imbalance_level();

        d.vacuum_ask = book.liquidity_vacuum_ask();
        d.vacuum_bid = book.liquidity_vacuum_bid();
        d.wall_above = (book.ask_count >= 4 && book.bid_count <= 2);
        d.wall_below = (book.bid_count >= 4 && book.ask_count <= 2);
        return d;
    }

private:
    // -------------------------------------------------------------------------
    // State
    int64_t m_cooldown_start_ms = 0;
    int64_t m_cooldown_ms       = 15000;
    int     m_trade_id          = 0;
    DOMSnap m_dom_cur;
    DOMSnap m_dom_prev;

    // RSI state
    std::deque<double> m_rsi_gains;
    std::deque<double> m_rsi_losses;
    double m_rsi_prev_mid  = 0.0;
    double m_rsi_cur       = 50.0;
    double m_rsi_prev      = 50.0;
    double m_rsi_trend     = 0.0;   // EMA of RSI slope
    bool   m_rsi_warmed    = false;
    double m_rsi_ema_alpha = 2.0 / (CFE_RSI_EMA_N + 1);

    // L2 imbalance exit state
    int m_imb_against_ticks = 0;   // consecutive ticks imb is against position
    int m_prev_depth_bid    = 0;
    int m_prev_depth_ask    = 0;

    // Drift fast-entry state
    double  m_prev_ewm_drift     = 0.0;
    int64_t m_dfe_cooldown_until   = 0;
    int     m_last_closed_dir      = 0;    // +1=was LONG, -1=was SHORT, 0=none
    int64_t m_last_closed_ms       = 0;    // when last position closed
    static constexpr int64_t CFE_OPPOSITE_DIR_COOLDOWN_MS = 60000; // 60s before opposite direction allowed
    static constexpr int64_t CFE_WINNER_COOLDOWN_MS       = 30000; // 30s after winner
    bool    m_dfe_warmed         = false;
    double  m_dfe_eff_thresh     = CFE_DFE_DRIFT_THRESH;  // session-adjusted threshold

    // DFE drift persistence state (2026-04-13)
    int     m_dfe_persist_ticks  = 0;   // consecutive ticks drift >= threshold
    int     m_dfe_persist_dir    = 0;   // direction of current persistence run (+1/-1/0)

    // DFE sustained-drift state (2026-04-13)
    // Tracks how long drift has been continuously above/below the lower sustained threshold.
    // Used for both the sustained-drift entry path and the bar trend-block gate.
    int64_t m_drift_sustained_start_ms = 0;  // when current sustained drift run began (0=none)
    int     m_drift_sustained_dir      = 0;  // +1 or -1, direction of current run
    double  m_sus_drift_peak           = 0.0; // peak |drift| seen during current sustained run

    // DFE price-action confirmation state (2026-04-13)
    std::deque<double> m_recent_mid;    // ring buffer of recent mid prices

    // Adverse-excursion re-entry filter (2026-04-15)
    // After a loss where price moved > 0.5x ATR against us, record exit price
    // and direction. Block same-direction re-entry until price returns within
    // 0.5x ATR of that exit level -- proves the move has exhausted/reversed.
    // Structurally prevents cascade re-entry into ongoing adverse moves.
    // Evidence: 14:25-14:32 FC cluster, 17:05 FC -- all entered into live downtrends.
    double  m_last_loss_exit_px  = 0.0;   // exit price of last losing trade
    int     m_last_loss_dir      = 0;     // +1=was LONG, -1=was SHORT
    double  m_last_loss_atr      = 0.0;   // ATR at time of loss (for threshold)
    bool    m_adverse_block      = false; // currently blocking same-dir re-entry

    // -------------------------------------------------------------------------
    // RSI slope EMA -- updated every tick unconditionally
    void rsi_update(double mid) noexcept {
        if (m_rsi_prev_mid == 0.0) { m_rsi_prev_mid = mid; return; }
        const double chg = mid - m_rsi_prev_mid;
        m_rsi_prev_mid   = mid;
        m_rsi_gains.push_back(chg > 0 ? chg : 0.0);
        m_rsi_losses.push_back(chg < 0 ? -chg : 0.0);
        if ((int)m_rsi_gains.size() > CFE_RSI_PERIOD) {
            m_rsi_gains.pop_front();
            m_rsi_losses.pop_front();
        }
        if ((int)m_rsi_gains.size() < CFE_RSI_PERIOD) return;
        double ag = 0.0, al = 0.0;
        for (auto x : m_rsi_gains)  ag += x;
        for (auto x : m_rsi_losses) al += x;
        ag /= CFE_RSI_PERIOD; al /= CFE_RSI_PERIOD;
        m_rsi_prev = m_rsi_cur;
        m_rsi_cur  = (al == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
        const double slope = m_rsi_cur - m_rsi_prev;
        if (!m_rsi_warmed) { m_rsi_trend = slope; m_rsi_warmed = true; }
        else m_rsi_trend = slope * m_rsi_ema_alpha + m_rsi_trend * (1.0 - m_rsi_ema_alpha);
    }

    // Returns +1 (up trend), -1 (down trend), 0 (no signal)
    int rsi_direction() const noexcept {
        if (!m_rsi_warmed) return 0;
        // Both floor (signal exists) and ceiling (not exhausted) required.
        // rsi_trend > 12 = RSI has been rising steeply for many ticks = late entry.
        if (m_rsi_trend >  CFE_RSI_THRESH && m_rsi_trend <  CFE_DFE_RSI_TREND_MAX) return +1;
        if (m_rsi_trend < -CFE_RSI_THRESH && m_rsi_trend > -CFE_DFE_RSI_TREND_MAX) return -1;
        return 0;
    }

    // -------------------------------------------------------------------------
    // Candle direction
    static bool candle_is_bullish(const BarSnap& b) noexcept {
        if (b.close <= b.open) return false;
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        if ((b.close - b.open) / range < CFE_BODY_RATIO_MIN) return false;
        if (b.close <= b.prev_high) return false;
        return true;
    }

    static bool candle_is_bearish(const BarSnap& b) noexcept {
        if (b.close >= b.open) return false;
        const double range = b.high - b.low;
        if (range <= 0.0) return false;
        if ((b.open - b.close) / range < CFE_BODY_RATIO_MIN) return false;
        if (b.close >= b.prev_low) return false;
        return true;
    }

    // -------------------------------------------------------------------------
    // L2 imbalance exit
    // imb = l2_imb converted to -1..+1 centered: (l2_imb - 0.5) * 2
    // Exit long when imb < -thresh for >= N ticks AND hold >= CFE_IMB_MIN_HOLD_MS
    // Exit short when imb > +thresh for >= N ticks AND hold >= CFE_IMB_MIN_HOLD_MS
    // Also exits on imb against for >= 1 tick AND depth drop AND hold >= min
    bool check_imb_exit(bool is_long, const DOMSnap& dom, int64_t hold_ms) noexcept {
        // Minimum hold guard: IMB_EXIT is blocked for the first CFE_IMB_MIN_HOLD_MS
        // milliseconds after entry. This eliminates the sub-10s noise exits caused by
        // a single imbalance blip immediately after entry (8s/37s losses on 2026-04-11).
        if (hold_ms < CFE_IMB_MIN_HOLD_MS) {
            // Still track counter ticks for continuity -- just don't act on them yet
            const double imb_track = (dom.l2_imb - 0.5) * 2.0;
            const bool against_track = is_long
                ? (imb_track < -CFE_IMB_EXIT_THRESH)
                : (imb_track >  CFE_IMB_EXIT_THRESH);
            if (against_track) m_imb_against_ticks++;
            else               m_imb_against_ticks = 0;
            m_prev_depth_bid = dom.bid_count;
            m_prev_depth_ask = dom.ask_count;
            return false;
        }

        // Use level-count imbalance from cTrader depth feed directly
        const double imb = (dom.l2_imb - 0.5) * 2.0;  // -1..+1
        const bool against = is_long
            ? (imb < -CFE_IMB_EXIT_THRESH)
            : (imb >  CFE_IMB_EXIT_THRESH);

        if (against) m_imb_against_ticks++;
        else         m_imb_against_ticks = 0;

        const bool depth_drop = is_long
            ? (dom.bid_count < m_prev_depth_bid)
            : (dom.ask_count < m_prev_depth_ask);

        m_prev_depth_bid = dom.bid_count;
        m_prev_depth_ask = dom.ask_count;

        return (m_imb_against_ticks >= CFE_IMB_EXIT_TICKS)
            || (m_imb_against_ticks >= 1 && depth_drop);
    }

    // -------------------------------------------------------------------------
    void enter(bool is_long, double bid, double ask,
               double spread, double cost_pts,
               double atr_pts, int64_t now_ms) noexcept
    {
        const double entry_px = is_long ? ask : bid;
        const double sl_pts   = (atr_pts > 0.0) ? atr_pts * CFE_DFE_SL_MULT : spread * 5.0;
        const double sl_px    = is_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        double size = risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(CFE_MIN_LOT, std::min(CFE_MAX_LOT, size));

        pos.active        = true;
        pos.is_long       = is_long;
        pos.entry         = entry_px;
        pos.sl            = sl_px;
        pos.size          = size;
        pos.full_size     = size;
        pos.cost_pts      = cost_pts;
        pos.entry_ts_ms   = now_ms;
        pos.mfe           = 0.0;
        pos.trail_active  = false;
        pos.trail_sl      = sl_px;   // initialise trail SL to hard SL
        pos.partial_done  = false;
        pos.atr_pts       = atr_pts;
        ++m_trade_id;
        phase = Phase::LIVE;

        // Reset imbalance exit state
        m_imb_against_ticks = 0;
        m_prev_depth_bid    = m_dom_cur.bid_count;
        m_prev_depth_ask    = m_dom_cur.ask_count;

        std::cout << "[CFE] ENTRY " << (is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << entry_px
                  << " sl=" << sl_px << " sl_pts=" << sl_pts
                  << " size=" << std::setprecision(3) << size
                  << " cost=" << cost_pts
                  << " rsi_trend=" << std::setprecision(2) << m_rsi_trend
                  << " atr=" << atr_pts
                  << " spread=" << spread
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();
    }

    // -------------------------------------------------------------------------
    void manage(double bid, double ask, double mid,
                const DOMSnap& dom,
                int64_t now_ms,
                CloseCallback on_close) noexcept
    {
        if (!pos.active) return;

        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;

        const int64_t hold_ms = now_ms - pos.entry_ts_ms;

        // -- PARTIAL EXIT: 50% of position at MFE >= 2x cost ------------------
        // Locks in guaranteed profit on half the position. Remaining half is
        // trailed. Safe: only fires once, only when sufficiently in profit,
        // residual position is still protected by hard SL / trail SL.
        if (!pos.partial_done && pos.mfe >= pos.cost_pts * 2.0) {
            const double partial_px   = pos.is_long ? bid : ask;
            const double partial_size = pos.full_size * 0.5;
            // Emit partial close record
            omega::TradeRecord ptr;
            ptr.id         = m_trade_id;
            ptr.symbol     = "XAUUSD";
            ptr.side       = pos.is_long ? "LONG" : "SHORT";
            ptr.entryPrice = pos.entry;
            ptr.exitPrice  = partial_px;
            ptr.sl         = pos.sl;
            ptr.size       = partial_size;
            ptr.pnl        = (pos.is_long ? (partial_px - pos.entry) : (pos.entry - partial_px))
                             * partial_size;
            ptr.mfe        = pos.mfe * partial_size;
            ptr.mae        = 0.0;
            ptr.entryTs    = pos.entry_ts_ms / 1000;
            ptr.exitTs     = now_ms / 1000;
            ptr.exitReason = "PARTIAL_TP";
            ptr.engine     = "CandleFlowEngine";
            ptr.regime     = "CANDLE_FLOW";
            ptr.l2_live    = true;
            omega::apply_realistic_costs(ptr, pos.entry, partial_size);
            std::cout << "[CFE] PARTIAL-TP " << (pos.is_long?"LONG":"SHORT")
                      << " @ " << std::fixed << std::setprecision(2) << partial_px
                      << " size=" << std::setprecision(3) << partial_size
                      << " pnl_usd=" << std::setprecision(2) << (ptr.pnl * 100.0)
                      << " mfe=" << std::setprecision(3) << pos.mfe
                      << (shadow_mode?" [SHADOW]":"") << "\n";
            std::cout.flush();
            pos.partial_done = true;
            pos.size         = partial_size;   // remaining half
            if (on_close) on_close(ptr);
        }

        // -- TRAILING SL: engage once MFE >= 1x cost --------------------------
        // Trail distance = 0.7 * ATR (tight enough to lock profit, wide enough
        // to survive normal tick noise on gold). Once engaged, SL only moves
        // in the profitable direction -- never back toward entry.
        // Trail: arm only when MFE >= 2x ATR (real profit locked in).
        // Old: armed at 1x cost ($0.62), dist=0.7xATR=1.40pts.
        // At MFE=0.67 trail_SL = entry+0.67-1.40 = entry-0.73 (BELOW entry).
        // Trail was wider than profit -- fired on noise for $1 instead of riding move.
        // New: arm at MFE >= 2x ATR so trail only engages on real moves.
        // At ATR=2pt: arms when MFE >= 4pts. dist=0.5xATR=1pt.
        // Trail_SL = mid - 1pt = entry+4-1 = entry+3 -- locked $3 profit minimum.
        if (pos.mfe >= pos.atr_pts * 2.0) {
            const double trail_dist = pos.atr_pts * 0.5;
            const double new_trail = pos.is_long
                ? (mid - trail_dist)
                : (mid + trail_dist);
            if (!pos.trail_active) {
                // First engagement -- only activate if trail SL is better than hard SL
                if (pos.is_long ? (new_trail > pos.sl) : (new_trail < pos.sl)) {
                    pos.trail_sl     = new_trail;
                    pos.trail_active = true;
                    std::cout << "[CFE] TRAIL-ENGAGED " << (pos.is_long?"LONG":"SHORT")
                              << " trail_sl=" << std::fixed << std::setprecision(2) << pos.trail_sl
                              << " dist=" << std::setprecision(3) << trail_dist
                              << " mfe=" << pos.mfe << "\n";
                    std::cout.flush();
                }
            } else {
                // Ratchet: only move trail SL in profitable direction
                if (pos.is_long ? (new_trail > pos.trail_sl) : (new_trail < pos.trail_sl)) {
                    pos.trail_sl = new_trail;
                }
            }
        }

        // -- HARD SL or TRAIL SL -----------------------------------------------
        const double effective_sl = pos.trail_active ? pos.trail_sl : pos.sl;
        if (pos.is_long ? (bid <= effective_sl) : (ask >= effective_sl)) {
            const char* sl_reason = pos.trail_active ? "TRAIL_SL" : "SL_HIT";
            std::cout << "[CFE] " << sl_reason << " " << (pos.is_long?"LONG":"SHORT")
                      << " @ " << std::fixed << std::setprecision(2) << (pos.is_long?bid:ask)
                      << " sl=" << std::setprecision(2) << effective_sl
                      << " trail=" << (pos.trail_active?1:0) << "\n";
            std::cout.flush();
            close_pos(pos.is_long ? bid : ask, sl_reason, now_ms, on_close);
            return;
        }

        // L2 imbalance exit -- requires CFE_IMB_MIN_HOLD_MS elapsed
        if (check_imb_exit(pos.is_long, dom, hold_ms)) {
            const double px = pos.is_long ? bid : ask;
            std::cout << "[CFE] IMB-EXIT " << (pos.is_long?"LONG":"SHORT")
                      << " imb_ticks=" << m_imb_against_ticks
                      << " l2_imb=" << std::fixed << std::setprecision(4) << dom.l2_imb
                      << " hold_ms=" << hold_ms
                      << " @ " << std::setprecision(2) << px
                      << " mfe=" << std::setprecision(3) << pos.mfe << "\n";
            std::cout.flush();
            close_pos(px, "IMB_EXIT", now_ms, on_close);
            return;
        }

        // Early-adverse exit in Asia (22:00-07:00 UTC):
        // If price moves > 0.5x ATR against us within first 60s AND MFE=0,
        // the entry was wrong -- exit immediately rather than waiting 90s stagnation.
        // Evidence: 04:26 -$27 and 04:47 -$0.60 both stagnated in Asia chop.
        // Fast adverse exit cuts the loss before it compounds.
        {
            const int64_t utc_sec_ae   = now_ms / 1000LL;
            const int     utc_hour_ae  = static_cast<int>((utc_sec_ae % 86400LL) / 3600LL);
            const bool    in_asia_ae   = (utc_hour_ae >= 22 || utc_hour_ae < 7);
            if (in_asia_ae && !pos.trail_active && pos.mfe <= 0.0) {
                const double adverse    = pos.is_long ? (pos.entry - mid) : (mid - pos.entry);
                const double atr_thresh = pos.atr_pts * 0.5;
                if (adverse > atr_thresh && hold_ms > 30000) {
                    const double px = pos.is_long ? bid : ask;
                    {
                        char _msg[512];
                        snprintf(_msg, sizeof(_msg), "[CFE-ASIA-ADVERSE] Early exit: adverse=%.2f > %.2f (0.5xATR) hold=%lldms\n",                            adverse, atr_thresh, (long long)hold_ms);
                        std::cout << _msg;
                        std::cout.flush();
                    }
                    close_pos(px, "STAGNATION", now_ms, on_close);
                    return;
                }
            }
        }

        // Stagnation safety exit
        // Stagnation exit -- cut losers, protect winners.
        //
        // DESIGN: time-based stagnation is wrong because it can't distinguish
        // "consolidating before move" from "wrong call, not going anywhere".
        // The key insight: if MFE ever exceeded 1x cost, the thesis WAS right --
        // the move started. Let the trail handle exit, not the timer.
        //
        // Two-tier logic:
        //
        // TIER 1 -- Never showed profit (mfe < 0.5x cost):
        //   Position never moved in our favour at all.
        //   Thesis was wrong from the start. Exit at 90s (Asia) / 120s (London).
        //   This is a genuine stagnation -- cut the loss quickly.
        //
        // TIER 2 -- Showed some profit (mfe >= 0.5x cost) but trail not engaged:
        //   Move started but hasn't reached trail threshold yet (2x ATR).
        //   Give it more time -- 180s (Asia) / 300s (London/NY).
        //   This prevents killing the 04:47 SHORT that had mfe=0.52 at 90s.
        //   If it still hasn't moved after 300s with mfe < cost, exit.
        //
        // TIER 3 -- Trail engaged (pos.trail_active = true):
        //   Don't stagnation-exit at all. Trail manages the exit.
        //   This is the "let winners run" path.
        {
            if (!pos.trail_active) {  // trail handles exit once engaged
                const int64_t utc_sec_stag   = now_ms / 1000LL;
                const int     utc_hour_stag  = static_cast<int>((utc_sec_stag % 86400LL) / 3600LL);
                const bool    in_asia_stag   = (utc_hour_stag >= 22 || utc_hour_stag < 7);
                const double  half_cost      = pos.cost_pts * 0.5;
                const bool    showed_profit  = (pos.mfe >= half_cost);
                // Tier 1: never showed profit -- exit quickly
                const int64_t t1_ms = in_asia_stag ?  90000LL : 120000LL;
                // Tier 2: showed some profit but trail not yet armed -- give more time
                const int64_t t2_ms = in_asia_stag ? 180000LL : 300000LL;
                const int64_t eff_stag_ms = showed_profit ? t2_ms : t1_ms;
                if (hold_ms >= eff_stag_ms) {
                    if (pos.mfe < pos.cost_pts * CFE_STAGNATION_MULT) {
                        const double px = pos.is_long ? bid : ask;
                        std::cout << "[CFE] STAGNATION-EXIT " << (pos.is_long?"LONG":"SHORT")
                                  << " held=" << hold_ms
                                  << "ms mfe=" << std::fixed << std::setprecision(3) << pos.mfe
                                  << " < need=" << pos.cost_pts * CFE_STAGNATION_MULT
                                  << " tier=" << (showed_profit ? "2" : "1")
                                  << " timeout=" << eff_stag_ms/1000 << "s\n";
                        std::cout.flush();
                        close_pos(px, "STAGNATION", now_ms, on_close);
                        return;
                    }
                }
            }
        }
    }

    // -------------------------------------------------------------------------
    void close_pos(double exit_px, const char* reason,
                   int64_t now_ms, CloseCallback on_close) noexcept
    {
        omega::TradeRecord tr;
        tr.id         = m_trade_id;
        tr.symbol     = "XAUUSD";
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry;
        tr.exitPrice  = exit_px;
        tr.sl         = pos.sl;
        tr.size       = pos.size;
        tr.pnl        = (pos.is_long ? (exit_px - pos.entry) : (pos.entry - exit_px))
                        * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        tr.mae        = 0.0;
        tr.entryTs    = pos.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.engine     = "CandleFlowEngine";
        tr.regime     = "CANDLE_FLOW";
        tr.l2_live    = true;
        omega::apply_realistic_costs(tr, pos.entry, pos.size);

        std::cout << "[CFE] EXIT " << (pos.is_long?"LONG":"SHORT")
                  << " @ " << std::fixed << std::setprecision(2) << exit_px
                  << " reason=" << reason
                  << " pnl_raw=" << std::setprecision(4) << tr.pnl
                  << " pnl_usd=" << std::setprecision(2) << (tr.pnl * 100.0)
                  << " mfe=" << std::setprecision(3) << pos.mfe
                  << " held=" << std::setprecision(0)
                  << static_cast<double>((now_ms - pos.entry_ts_ms) / 1000) << "s"
                  << (shadow_mode?" [SHADOW]":"") << "\n";
        std::cout.flush();

        // Track direction for opposite-direction cooldown
        m_last_closed_dir = (tr.side == std::string("LONG")) ? +1 : -1;
        m_last_closed_ms  = now_ms;

        // Adverse-excursion filter: record loss if price moved >= 0.5 ATR against us
        {
            const double actual_adverse = pos.is_long
                ? (pos.entry - exit_px)   // LONG: loss = exit below entry
                : (exit_px - pos.entry);  // SHORT: loss = exit above entry
            if (actual_adverse > 0.0 && pos.atr_pts > 0.0
                    && actual_adverse >= pos.atr_pts * 0.5) {
                m_last_loss_exit_px = exit_px;
                m_last_loss_dir     = pos.is_long ? +1 : -1;
                m_last_loss_atr     = pos.atr_pts;
                m_adverse_block     = true;
            }
        }

        pos = OpenPos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start_ms = now_ms;
        const bool is_winner = (strcmp(reason,"PARTIAL_TP")==0||
                                strcmp(reason,"TRAIL_SL")==0||
                                strcmp(reason,"IMB_EXIT")==0);
        // STAGNATION cooldown raised 10s->60s: entry quality failed, wait for real signal
        // Winner cooldown 30s: prevent immediate re-entry on momentum exhaustion
        // FORCE_CLOSE cooldown 300s (2026-04-15): price moved hard against position.
        //   Re-entering same direction 15s later = fighting the tape.
        //   Evidence: 14:25-14:32 UTC -- 4 consecutive FC longs 15s apart = -05.
        // IMB_EXIT cooldown raised 5s->30s (2026-04-15): was re-entering immediately
        //   after losing IMB exits. Evidence: 19:02-19:45 -- 5 losing longs 5s apart.
        m_cooldown_ms = (strcmp(reason,"STAGNATION")  == 0) ? 60000
                      : (strcmp(reason,"FORCE_CLOSE") == 0) ? 300000
                      : (strcmp(reason,"IMB_EXIT")    == 0) ? 30000
                      : is_winner                            ? 30000
                      :                                        15000;
        // DFE rearm: 120s after SL loss, 30s after winner, 300s after FC
        if (strcmp(reason,"FORCE_CLOSE")==0)
            m_dfe_cooldown_until = now_ms + 300000LL;
        else if (strcmp(reason,"SL_HIT")==0||strcmp(reason,"TRAIL_SL")==0)
            m_dfe_cooldown_until = now_ms + CFE_DFE_COOLDOWN_MS;
        else if (is_winner)
            m_dfe_cooldown_until = now_ms + CFE_WINNER_COOLDOWN_MS;
        m_imb_against_ticks = 0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega






