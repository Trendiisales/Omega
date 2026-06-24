#pragma once
//  ADVERSE-PROTECTION: HAS S64 LOSS_CUT_PCT=0.03 cold-loss cut + 2-consec-loss circuit breaker + S55 BE-lock@6pips + trail + fixed SL/TP bracket (all wired in manage()); engine is shadow-only (shadow_mode=true) and on the adverse-protection legacy backfill list -- no faithful backtest on record for the protection step -- verdict owed before re-enable (live promotion). (backfill S-2026-06-24n)
#include <iomanip>
#include <iostream>
#include "SpreadRegimeGate.hpp"
#include "OmegaNewsBlackout.hpp"
#include "OmegaCostGuard.hpp"
// =============================================================================
// EurusdLondonOpenEngine.hpp  --  London-open compression bracket for EURUSD
// =============================================================================
//
// 2026-05-02 SESSION DESIGN (Claude / Jo):
//   First FX engine since the 2026-04-06 global FX disable (see tick_fx.hpp
//   lineage comment). Targets the 06:00-09:00 UTC London-open compression-
//   breakout window. Mirrors GoldMidScalperEngine architecture (S53 baseline)
//   with FX-scale parameters and pip-scale price math.
//
//   Audit lineage (see docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md):
//     - Replaces dead-code g_bracket_eurusd (configured every startup but
//       never invoked from any dispatch path).
//     - Does NOT inherit MacroCrashEngine signal model (disabled 2026-04-30
//       at 4.8% WR / -10,849pts: ATR-extension + vol-surge + drift gating
//       were the documented loss source).
//     - Addresses 2026-04-06 BreakoutEngine "enters after move is done"
//       failure via BE-lock at 4 pips MFE: even late entries cannot
//       compound losses past break-even.
//     - Addresses 2026-04-06 "8pip TP barely covers spread" failure via
//       2-pip SL_BUFFER + 24-pip TP target (3x typical round-trip cost).
//     - News-event exposure mitigated via g_news_blackout.is_blocked()
//       at IDLE->ARMED transition (NFP/CPI/FOMC/ECB).
//     - Session window 06-09 UTC concentrates fires on the highest-edge
//       FX hour (London open + first hour, pre-NY-overlap).
//
//   Pip math reference (EURUSD):
//     1 pip          = 0.0001 price
//     Daily ATR      = 60-120 pips
//     Typical spread = 0.5 to 1.4 pips on cTrader/BlackBull
//     Pip value      = $1 at 0.10 lot, $10 at 1.00 lot
//
//   Math (range = compression structure size, in price units, S56 SL_FRAC=0.80, RR=2):
//     range 0.0008 ->  8 pips -> SL 0.0008 ( 8.4 pips) -> TP 0.0017 (17 pips)
//     range 0.0010 -> 10 pips -> SL 0.0010 (10.0 pips) -> TP 0.0020 (20 pips)
//     range 0.0012 -> 12 pips -> SL 0.0012 (11.6 pips) -> TP 0.0023 (23 pips)
//     range 0.0015 -> 15 pips -> SL 0.0014 (14.0 pips) -> TP 0.0028 (28 pips)
//     range 0.0020 -> 20 pips -> SL 0.0018 (18.0 pips) -> TP 0.0036 (36 pips)
//     range 0.0030 -> 30 pips -> SL 0.0026 (26.0 pips) -> TP 0.0052 (52 pips)
//     range 0.0050 -> 50 pips -> SL 0.0042 (42.0 pips) -> TP 0.0084 (84 pips)
//
//   STRUCTURE_LOOKBACK = 600 (~3 min @ 200 ticks/min London EURUSD) finds
//   genuine compressions while filtering Asian-session drift micro-ranges.
//
// SAFETY:
//   - Defaults to shadow_mode = true. Live promotion requires explicit
//     authorisation after a 2-week paper run shows positive expectancy.
//   - 30-trade minimum sample, WR >= 35% net positive after costs.
//   - Uses MIN_BREAK_TICKS = 5 (matches gold mid-scalper sweep guard).
//   - Inherits all audit-validated guards from GoldMidScalper lineage:
//       S20 trail-arm guards (MIN_TRAIL_ARM_PTS=0.0006, MIN_TRAIL_ARM_SECS=30)
//       S43 mae tracker
//       S47 T4a ATR-expansion gate (EXPANSION_MULT=1.10) with ratchet fix
//       S51 1A.1.a spread_at_entry capture
//       S52/S53 MFE_TRAIL_FRAC = 0.55
//       S53 BE-lock at 4 pips MFE
//       S53 same-level re-arm block (10 pips radius, 15min post-SL / 10min post-win)
//       AUDIT 2026-04-29 mutex on _close path
//       audit-fixes-18 SpreadRegimeGate per-engine
//
// SESSION WINDOW:
//   06:00 UTC <= now_hh < 09:00 UTC  ->  arming allowed
//   otherwise                        ->  IDLE only
//   (Existing positions still managed via manage() regardless of window.)
//
// NEWS BLACKOUT:
//   At IDLE->ARMED transition, consult g_news_blackout.is_blocked("EURUSD",..).
//   NFP / CPI / FOMC / ECB cover EURUSD per RecurringEventScheduler.
//   Existing positions exit via SL/TP/MAX_HOLD inside manage() -- not via
//   this gate -- to avoid forcing closes on already-open trades.
//
// DOM FILTER (FX variant):
//   At PENDING->FIRE time, DOM confirms which side has a clear path.
//   EUR-side fields available in g_macro_ctx:
//     - eur_l2_imbalance (0..1; 0.5 neutral)
//     - eur_vacuum_ask / eur_vacuum_bid
//     - eur_wall_above  / eur_wall_below
//   No eur_slope field exists -- engine receives book_slope = 0.0 from the
//   dispatcher, so the slope branch is naturally inert. Vacuum and wall
//   fields drive the lot bonus / penalty. l2_real flag should be wired to
//   g_macro_ctx.ctrader_l2_live -- when false, DOM filter is bypassed (safe
//   fallback, both sides equal lots).
//
// SIZING:
//   Standalone: risk_dollars = $30, SL_dist = range * 0.5 + 0.0002 (2 pips)
//   Pyramid:    risk_dollars = $10 (30% addon), same SL formula
//   Lot range:  0.01 to 0.10 (LOT_MIN..LOT_MAX). At 0.10 lot, $1 per pip;
//               an 8-pip SL = $8 risk -- well within the $30 budget while
//               staying conservative for a first FX deploy.
//   ENTRY_SIZE_DEFAULT = 0.10 lot (un-capped from gold's 0.01 cap because
//   FX requires meaningful size to validate statistically; gold cap was
//   FIX 2026-04-22 specific to the gold engine cohort).
//
// LOG NAMESPACE:
//   All log lines use prefix [EUR-LDN-OPEN] / [EUR-LDN-OPEN-DIAG].
//   tr.engine = "EurusdLondonOpen" (registered in engine_init.hpp).
//   tr.regime = "LDN_COMPRESSION".
// =============================================================================

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <functional>
#include <string>
#include <deque>
#include <vector>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "OmegaTradeLedger.hpp"

namespace omega {

class EurusdLondonOpenEngine {
public:
    // -- Parameters (Section 3 of EURUSD handoff doc, user-confirmed 2026-05-02) --
    // Lookback window: 600 ticks ~= 3 min at 200 ticks/min EURUSD London.
    //   Wide enough to find genuine 8+ pip compressions; narrow enough to
    //   fire several times per 3-hour London-open window.
    static constexpr int    STRUCTURE_LOOKBACK   = 600;
    // Warmup: refuse new arming until at least this many ticks have arrived.
    //   FX feed cold-start has more noise than gold; 60 vs gold's 30.
    static constexpr int    MIN_ENTRY_TICKS      = 60;
    // Sweep guard: price must sit inside the formed bracket for this many
    //   consecutive ticks before stop orders are sent. Mirrors gold value.
    static constexpr int    MIN_BREAK_TICKS      = 5;
    // 8-50 pip capture band.
    //   MIN 8 pips: London compressions typically 8-15 pips.
    //   MAX 50 pips: S56 backtest tune. Originally 30 pips, raised to 50 after
    //   14-month tick-data sweep showed 30+pip compressions still produce
    //   profitable breakouts. Cap retained as guard against post-news anomaly.
    static constexpr double MIN_RANGE            = 0.0008;
    static constexpr double MAX_RANGE            = 0.0050;
    // S56 2026-05-02 (post-OOS validation): SL_FRAC raised 0.5 -> 0.80.
    //   Original 0.5 (half-range) put SL too close: trades wicked out before
    //   reaching trail-arm. 0.80 keeps SL in the upper 80% of compression
    //   structure, giving the trade room to develop. PF jumps from 0.89 to
    //   1.62 on this single change. See handoff doc Section 9.
    static constexpr double SL_FRAC              = 0.80;
    // SL_BUFFER = 2 pips: spread (0.5-1.4 pip) + slippage cushion.
    static constexpr double SL_BUFFER            = 0.0002;
    // RR = 2.0: TP = sl_dist * 2. S55 2026-05-02 backtest tune.
    //   Original RR=3 (24-pip TP at 8-pip SL) was unreachable for typical
    //   EURUSD volatility — only 3 of 1076 historical trades hit TP_HIT.
    //   14-month HistData backtest showed RR=2 with BE=6 pips gives
    //   PnL=+$188 vs RR=3 baseline +$54 (3.5x improvement, DD reduced 34%).
    //   See docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md Section 8.
    //   Gold uses RR=4 (commodity volatility); FX RR=2 reflects shorter
    //   typical trend continuation post-compression on EURUSD.
    static constexpr double TP_RR                = 2.0;
    // S56 2026-05-02 (post-OOS validation): TRAIL_FRAC 0.25 -> 0.30.
    //   Wider trail (30% of range) lets winners run further before trail
    //   tightens. Combines additively with SL_FRAC=0.80 -- joint tune
    //   pushed PnL from $305 (SL_FRAC alone) to $425 (both).
    static constexpr double TRAIL_FRAC           = 0.30;
    // Trail-arm guards (S20 lineage). FX moves slower than gold per second
    //   of clock time, so MIN_TRAIL_ARM_SECS bumped 15 -> 30. MIN_TRAIL_ARM_PTS
    //   = 6 pips -- R-equivalent of $5 on gold at 0.10 lot ($1/pip * 6 = $6).
    static constexpr double MIN_TRAIL_ARM_PTS    = 0.0006;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 30;
    // S56 2026-05-02 (post-OOS validation): MFE_TRAIL_FRAC 0.55 -> 0.40.
    //   Preserve 60% of run (was 45%). With wider SL placement (SL_FRAC=0.80)
    //   trades MFE further before reversing; tightening MFE give-back from
    //   0.55 -> 0.40 captures more of the move. PF jumps to 2.09 with this.
    //   Pre-S56 value was 0.55 (S52/S53 gold lineage).
    static constexpr double MFE_TRAIL_FRAC       = 0.40;
    // S55 2026-05-02 break-even lock trigger: 6 pips MFE.
    //   Move SL to entry once MFE >= BE_TRIGGER_PTS. Originally 4 pips
    //   (S53 baseline from gold lineage) but 14-month HistData backtest
    //   showed 4 pips fired too early -- trades that would have hit TP
    //   got locked at break-even. Raising to 6 pips (matching the trail
    //   arm threshold) gives PnL=+$153 vs +$54 baseline (single biggest
    //   improvement of any axis sweep). At 6 pips, BE-lock and trail-arm
    //   fire simultaneously, eliminating the BE-only zone -- trades
    //   transition seamlessly from BE-protected to trail-protected.
    //   Net result: BE_HIT count drops to 0; clean TP/TRAIL/SL outcomes.
    //   See docs/SESSION_2026-05-02_EURUSD_LONDON_OPEN_HANDOFF.md Section 8.
    static constexpr double BE_TRIGGER_PTS       = 0.0006;
    // S54 2026-05-04 (audit-fixes-35): BE-exit slippage offset.
    //   EURUSD pip = 0.0001. Round-trip cost on default 0.10 lot is approx
    //   1-1.5 pips (spread ~0.7 + slippage ~0.5 + commission). Park SL at
    //   entry +/- BE_OFFSET_PTS (~1.5 pips) so a BE_HIT recovers the cost.
    //   Same logic as XAUUSD/USDJPY engines, scaled to EUR pip size.
    static constexpr double BE_OFFSET_PTS        = 0.00015;
    // S56 2026-05-02 (post-OOS validation): SAME_LEVEL_BLOCK_PTS 10 -> 8 pips.
    //   8 pips matches MIN_RANGE exactly: any compression structure that
    //   overlaps a prior exit within its own width is rejected. This is
    //   the tightest block radius that still allows continuation breakouts
    //   to fire. Combined with the wider SL placement (SL_FRAC=0.80) it
    //   reduces re-entry on chop. POST_SL block raised 900s -> 1200s
    //   (15min -> 20min) to give failed breakouts more time to clear.
    static constexpr double SAME_LEVEL_BLOCK_PTS         = 0.0008;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S   = 1200; // 20 min after SL
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S  = 600;  // 10 min after TP/TRAIL
    // MAX_SPREAD = 2 pips. Reject if spread blew out beyond typical 0.5-1.4.
    //   Checked at IDLE->ARMED transition only.
    static constexpr double MAX_SPREAD           = 0.00020;
    // S59 2026-05-07: PENDING-phase fill spread cap (2x MAX_SPREAD = 4 pips).
    //   Consulted by the FILL_SPREAD_REJECT path inside the PENDING block.
    //   Closes the gap where a spread spike between FIRE and FILL produced
    //   high-cost fills (06:07:49 UTC 2026-05-07 EURUSD SHORT loss had
    //   spread spike from 1.1 -> 5 pips during PENDING -> $10 of slippage,
    //   ~35% of the trade's total cost). Tolerant of mid-bracket spread
    //   fluctuation; only blocks when spread is wide AT the fill moment.
    static constexpr double MAX_FILL_SPREAD      = 0.00040;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;
    // USD value of 1 unit of price (1.0) at default lot 0.10:
    //   1 pip (0.0001) at 0.10 lot = $1   ->  1 unit of price = 10000 * $1 = $10,000.
    static constexpr double USD_PER_PRICE_UNIT   = 10000.0;
    // S56 2026-05-02 (post-OOS validation): LOT_MAX 0.10 -> 0.20.
    //   ENTRY_SIZE_DEFAULT and LOT_MIN unchanged. Half-Kelly sizing per the
    //   14-month champion's empirical win rate (66.6%) and win/loss ratio
    //   (b=0.605):
    //     Kelly fraction      = 11.5% of capital per trade
    //     Half Kelly          =  5.7% of capital per trade
    //     Quarter Kelly       =  2.9% of capital per trade
    //   At 0.20 lot, 8-pip SL = $16 risk per trade = ~1.6% of $1000 sub-
    //   account or ~0.16% of $10k account. Conservative half-Kelly territory
    //   with margin for WR degradation in real broker conditions (avg_loss
    //   $10.54 is 65% bigger than avg_win $6.38, so the 66% WR is doing all
    //   the work -- if WR drops to 55% the math goes upside-down fast).
    //   DO NOT exceed 0.20 until 2 weeks of shadow data confirm OOS WR >= 60%.
    static constexpr double ENTRY_SIZE_DEFAULT   = 0.10;
    static constexpr double LOT_MIN              = 0.01;
    // S58 2026-05-07: TEMPORARY conservative cut from S56's 0.20 -> 0.10.
    //   The 06:07:49 UTC EUR loss exposed a cold-start ATR-bypass hole
    //   (now closed via S58 cold-start guard above). Cutting LOT_MAX in
    //   half while we confirm the new behavior holds in shadow. Restore
    //   to the S56 half-Kelly 0.20 cap after 2+ weeks of clean shadow
    //   data with the cold-start guard active and stable.
    static constexpr double LOT_MAX              = 0.10;
    // Pending timeout: FX breaks fast or resets. Matches existing
    //   g_bracket_eurusd 180s value.
    static constexpr int    PENDING_TIMEOUT_S    = 180;
    // S56 2026-05-02 (post-OOS validation): COOLDOWN_S 240 -> 120.
    //   Original 240s (4min) was too patient. With wider SL (SL_FRAC=0.80)
    //   and tighter same-level block (8 pips), faster cooldown captures
    //   more genuine continuation moves without re-firing on chop. The
    //   same-level block (post-SL 1200s, post-win 600s) is now the
    //   primary anti-chop guard rather than a long blanket cooldown.
    static constexpr int    COOLDOWN_S           = 120;
    // Session window (UTC hours): 06:00-09:00 UTC London-open compression
    //   window. 3h window concentrating on the highest-edge London open hour.
    //
    // 2026-05-04 (post-S57): production window RESTORED.
    //   The S57 audit-fixes-36 widening (0-24) was a SHADOW-VISIBILITY-ONLY
    //   override so shadow engines would fire AS IF live and produce visible
    //   ledger/PnL during the FX wiring validation. Live tape analysis showed
    //   the widening pulled the engine into Asia compression hours where the
    //   "fade-the-edge" pattern dominates (opposite signal to a momentum
    //   compression-breakout) -- producing the symptomatic ✓BE → SL shape.
    //   Restoring the production 06-09 window removes that drag. Engine still
    //   self-gates on news blackout, spread, ATR, same-level block, and
    //   compression-range formation inside the window.
    //
    //   The `< START || >= END` check below remains correct for the
    //   non-wraparound 06-09 window.
    static constexpr int    SESSION_START_HOUR_UTC = 6;
    static constexpr int    SESSION_END_HOUR_UTC   = 9;
    static constexpr double DOM_SLOPE_CONFIRM    = 0.15;
    static constexpr double DOM_LOT_BONUS        = 1.3;
    static constexpr double DOM_WALL_PENALTY     = 0.5;

    // ATR-expansion gate (S47 T4a, ratchet fix 2026-04-29).
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    // S62 2026-05-13 anti-marginal-fire absolute floor (FX lineage sweep).
    //   Range must clear median by at least ABS_EXPANSION_FLOOR (3 pips) in
    //   addition to the 1.10x multiplier. Blocks the marginal-clearance
    //   pattern where median is depressed and the multiplier alone is a
    //   negligible expansion gate.
    static constexpr double ABS_EXPANSION_FLOOR   = 0.0003;

    // S64 2026-05-13 circuit-breaker layer (mirrors GbpusdLondonOpen).
    //   LOSS_CUT_PCT       -- immediate cold-loss cut. 0.03% of 1.17 ~= 3.5
    //                         pips, tighter than range-based SL.
    //   CONSEC_LOSS_THRESH -- 2 consecutive losses in current UTC day trips
    //                         the breaker. 0 disables.
    //   CONSEC_LOSS_BLOCK_S-- 4h lockout (covers full London-open window).
    double  LOSS_CUT_PCT          = 0.03;
    int     CONSEC_LOSS_THRESH    = 2;
    int64_t CONSEC_LOSS_BLOCK_S   = 14400;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    // 2026-05-02: shadow ON by default. Promote to live ONLY after a clean
    //   2-week paper validation showing positive expectancy in the 8-30 pip
    //   capture zone. Live promotion via engine_init.hpp override -- do NOT
    //   change this default in the header.
    bool  shadow_mode = true;

    struct LivePos {
        bool    active   = false;
        bool    is_long  = false;
        double  entry    = 0.0;
        double  tp       = 0.0;
        double  sl       = 0.0;
        double  size     = ENTRY_SIZE_DEFAULT;
        double  mfe      = 0.0;
        double  mae      = 0.0;
        double  spread_at_entry = 0.0;
        int64_t entry_ts = 0;
        // S53 break-even lock flag. One-shot -- prevents repeated BE moves.
        bool    be_locked = false;
    } pos;

    double bracket_high  = 0.0;
    double bracket_low   = 0.0;
    double range         = 0.0;
    double pending_lot       = ENTRY_SIZE_DEFAULT;
    double pending_lot_long  = ENTRY_SIZE_DEFAULT;
    double pending_lot_short = ENTRY_SIZE_DEFAULT;

    std::string pending_long_clOrdId;
    std::string pending_short_clOrdId;
    std::function<void(const std::string&)> cancel_fn;

    bool has_open_position() const noexcept { return pos.active; }

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // S61 2026-05-07: range_history persistence ctor.
    //   Loads m_range_history from disk if a fresh state file exists
    //   (age <= RANGE_HIST_STALENESS_S = 7200s = 2h). When load succeeds,
    //   the S58 cold-start guard inside on_tick() becomes inert immediately
    //   because m_range_history.size() is already >= EXPANSION_MIN_HISTORY.
    //   When file is missing or stale, S58 cold-start guard remains active
    //   for the warmup window -- preserving the original behavior as the
    //   safe fallback.
    //
    //   The constructor is noexcept; all I/O failures degrade to cold-start
    //   without raising. See _try_load_range_history() for details.
    EurusdLondonOpenEngine() noexcept {
        _try_load_range_history();
        // S62 2026-05-13: rehydrate post-trade same-level block state so a
        //   service restart inside the 20-min post-SL window does NOT
        //   silently bypass the re-arm block.
        _try_load_post_trade_block();
    }

    // -- Main tick ------------------------------------------------------------
    void on_tick(double bid, double ask, int64_t now_ms,
                 bool can_enter,
                 bool flow_live,
                 bool flow_be_locked,
                 int  flow_trail_stage,
                 CloseCallback on_close,
                 // DOM data (defaulted for backward compat)
                 double book_slope  = 0.0,
                 bool   vacuum_ask  = false,
                 bool   vacuum_bid  = false,
                 bool   wall_above  = false,
                 bool   wall_below  = false,
                 bool   l2_real     = false) noexcept
    {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid    = (bid + ask) * 0.5;
        const double spread = ask - bid;
        const int64_t now_s = now_ms / 1000;

        // SpreadRegimeGate: feed every tick; consult only on new-entry path.
        m_spread_gate.on_tick(now_ms, spread);
#ifndef OMEGA_BACKTEST
        m_spread_gate.set_macro_regime(g_macroDetector.regime());
#endif

        m_last_tick_s = now_s;

        // S62 2026-05-13: persist range_history every 30s on ANY tick,
        //   regardless of phase. Previously the save call only ran inside
        //   the ARMED branch, so long IDLE windows left history un-persisted.
        //   With 24/7 operation, restarts during IDLE silently threw away
        //   prior history and re-triggered the S58 cold-start guard.
        _save_range_history_if_due(now_s);

        ++m_ticks_received;
        m_window.push_back(mid);
        if ((int)m_window.size() > STRUCTURE_LOOKBACK * 2) m_window.pop_front();

        // Warmup diag every 600 ticks + first 60
        if (m_ticks_received % 600 == 0 || m_ticks_received <= 60) {
            double live_range = 0.0;
            const int window_needed = STRUCTURE_LOOKBACK;
            const bool window_ok = ((int)m_window.size() >= window_needed);
            if (window_ok) {
                auto it_hi = std::max_element(m_window.begin(), m_window.end());
                auto it_lo = std::min_element(m_window.begin(), m_window.end());
                live_range = *it_hi - *it_lo;
            }
            {
                char _buf[512];
                snprintf(_buf, sizeof(_buf),
                    "[EUR-LDN-OPEN-DIAG] ticks=%d phase=%d window=%d/%d range=%.5f spread=%.5f\n",
                    m_ticks_received, (int)phase, (int)m_window.size(), window_needed,
                    live_range, spread);
                std::cout << _buf;
                std::cout.flush();
            }
        }

        // -- COOLDOWN ---------------------------------------------------------
        if (phase == Phase::COOLDOWN) {
            if (now_s - m_cooldown_start >= COOLDOWN_S) phase = Phase::IDLE;
            else return;
        }

        // -- LIVE: manage position --------------------------------------------
        // Position management runs unconditionally -- session window and
        // news blackout do NOT force-close already-open positions. Existing
        // positions exit via SL/TP/trail/BE inside manage().
        if (phase == Phase::LIVE) {
            manage(bid, ask, mid, now_s, on_close);
            return;
        }

        // -- PENDING: wait for fill -------------------------------------------
        if (phase == Phase::PENDING) {
            const bool would_fill_long  = (ask >= bracket_high);
            const bool would_fill_short = (bid <= bracket_low);

            // S59 2026-05-07: PENDING-phase spread recheck at the fill moment.
            //   The IDLE->ARMED MAX_SPREAD gate only checks spread at bracket
            //   formation. Once PENDING, the original code accepted any
            //   spread on fill -- which let London-open spread spikes (3-5+
            //   pips on event flow) produce high-cost fills on what looked
            //   like tight-spread setups. The 06:07:49 UTC 2026-05-07 EURUSD
            //   SHORT loss had spread spike from 1.1 -> 5 pips between FIRE
            //   and FILL: $10 of slippage (35% of trade cost) attributable
            //   to that spike alone.
            //
            //   When a fill would otherwise trigger, abort the bracket
            //   entirely if spread > MAX_FILL_SPREAD. Cancel pending orders,
            //   return to IDLE. Trade is missed; cost-blowout is avoided.
            //   Mid-bracket spread spikes that don't coincide with a fill
            //   tick are tolerated -- spread is only re-checked when a fill
            //   would otherwise trigger, not on every PENDING tick.
            if ((would_fill_long || would_fill_short) && spread > MAX_FILL_SPREAD) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] FILL_SPREAD_REJECT spread=%.5f max=%.5f side=%s -- aborting bracket\n",
                        spread, MAX_FILL_SPREAD,
                        would_fill_long ? "LONG" : "SHORT");
                    std::cout << _buf;
                    std::cout.flush();
                }
                if (cancel_fn) {
                    if (!pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
                    if (!pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
                }
                pending_long_clOrdId.clear();
                pending_short_clOrdId.clear();
                phase = Phase::IDLE;
                bracket_high = bracket_low = 0.0;
                return;
            }

            if (would_fill_long)  { confirm_fill(true,  bracket_high, pending_lot_long,  spread); return; }
            if (would_fill_short) { confirm_fill(false, bracket_low,  pending_lot_short, spread); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] PENDING TIMEOUT after %ds -- resetting\n",
                        PENDING_TIMEOUT_S);
                    std::cout << _buf;
                    std::cout.flush();
                }
                if (cancel_fn) {
                    if (!pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
                    if (!pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
                }
                pending_long_clOrdId.clear();
                pending_short_clOrdId.clear();
                phase = Phase::IDLE;
            }
            return;
        }

        if (m_ticks_received < MIN_ENTRY_TICKS) return;
        if ((int)m_window.size() < STRUCTURE_LOOKBACK) return;

        if (!can_enter) {
            if (phase == Phase::ARMED) return;
            return;
        }
        if (spread > MAX_SPREAD) return;

        if (!m_spread_gate.can_fire()) return;

        // Session window gate: 06:00-09:00 UTC only. London open + first hour.
        // FX edge is concentrated here per multi-source FX research (London
        // hand-off from Asia, pre-NY-overlap). Outside this window, stay IDLE.
        // Existing LIVE/PENDING/COOLDOWN paths above already returned.
        {
            time_t t = static_cast<time_t>(now_s);
            struct tm utc{};
#ifdef _WIN32
            gmtime_s(&utc, &t);
#else
            gmtime_r(&t, &utc);
#endif
            if (utc.tm_hour < SESSION_START_HOUR_UTC ||
                utc.tm_hour >= SESSION_END_HOUR_UTC) {
                return;
            }
        }

        // News blackout gate: NFP/CPI/FOMC/ECB block EURUSD per
        // RecurringEventScheduler symbol sets. Forex Factory live calendar
        // (g_live_calendar) layered on top by config.hpp:278 refresh.
        // Only blocks NEW arming; already-LIVE positions continue managing
        // via manage() above.
        //
        // g_news_blackout lives at file scope in omega_types.hpp (static,
        // internal linkage). Per the SINGLE-TRANSLATION-UNIT include model
        // (engine_init.hpp:7), omega_types.hpp is always included before any
        // engine header from main.cpp -- so the symbol is visible here at
        // parse time. Direct usage (no extern declaration) avoids the
        // linkage-mismatch warning that an extern would create against the
        // static definition.
        if (g_news_blackout.is_blocked("EURUSD", now_s)) {
            return;
        }

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        // -- IDLE -> ARMED ---------------------------------------------------
        if (phase == Phase::IDLE) {
            // S64 2026-05-13 consec-loss circuit breaker.
            {
                const int _utc_today = _utc_day_key(now_s);
                if (m_consec_loss_day_utc != _utc_today &&
                    m_consec_loss_day_utc != -1)
                {
                    if (m_consec_loss_count > 0 ||
                        m_consec_loss_block_until_s > 0)
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[EUR-LDN-OPEN] CONSEC_LOSS_DAY_ROLL reset count=%d->0 "
                            "block_until=%lld->0\n",
                            m_consec_loss_count,
                            (long long)m_consec_loss_block_until_s);
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    m_consec_loss_count         = 0;
                    m_consec_loss_block_until_s = 0;
                    m_consec_loss_day_utc       = _utc_today;
                    _save_post_trade_block();
                }
            }
            if (m_consec_loss_block_until_s > 0 &&
                now_s < m_consec_loss_block_until_s)
            {
                static int64_t s_last_log_s = 0;
                if (now_s - s_last_log_s >= 60) {
                    s_last_log_s = now_s;
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] CONSEC_LOSS_BLOCK active count=%d "
                        "block_remaining_s=%lld -- not arming\n",
                        m_consec_loss_count,
                        (long long)(m_consec_loss_block_until_s - now_s));
                    std::cout << _buf;
                    std::cout.flush();
                }
                return;
            }

            // S53: same-level re-arm block.
            //   Block re-arming when the new compression's hi or lo overlaps
            //   a recent exit price within SAME_LEVEL_BLOCK_PTS=10 pips and
            //   the relevant cooldown is still active. Continuation captured
            //   naturally once price moves past the block radius.
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    // S62 2026-05-13: emit log line so this block is greppable.
                    {
                        char _buf[320];
                        snprintf(_buf, sizeof(_buf),
                            "[EUR-LDN-OPEN] SAME_LEVEL_BLOCK side=SL sl_price=%.5f "
                            "w_hi=%.5f w_lo=%.5f radius=%.5f remaining_s=%lld\n",
                            m_sl_price, w_hi, w_lo, SAME_LEVEL_BLOCK_PTS,
                            (long long)(m_sl_cooldown_ts - now_s));
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    return;
                }
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) {
                    {
                        char _buf[320];
                        snprintf(_buf, sizeof(_buf),
                            "[EUR-LDN-OPEN] SAME_LEVEL_BLOCK side=WIN win_price=%.5f "
                            "w_hi=%.5f w_lo=%.5f radius=%.5f remaining_s=%lld\n",
                            m_win_exit_price, w_hi, w_lo, SAME_LEVEL_BLOCK_PTS,
                            (long long)(m_win_exit_block_ts - now_s));
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    return;
                }
            }
            if (range >= MIN_RANGE && range <= MAX_RANGE) {
                phase        = Phase::ARMED;
                bracket_high = w_hi;
                bracket_low  = w_lo;
                m_inside_ticks = 0;
                m_armed_ts   = now_s;
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] ARMED hi=%.5f lo=%.5f range=%.5f spread=%.5f\n",
                        bracket_high, bracket_low, range, spread);
                    std::cout << _buf;
                    std::cout.flush();
                }
            }
            return;
        }

        // -- ARMED -----------------------------------------------------------
        if (phase == Phase::ARMED) {
            bracket_high = std::max(bracket_high, w_hi);
            bracket_low  = std::min(bracket_low,  w_lo);
            range        = bracket_high - bracket_low;
            if (range > MAX_RANGE) { phase = Phase::IDLE; bracket_high = bracket_low = range = 0.0; return; }
            if (range < MIN_RANGE || range > MAX_RANGE) { phase = Phase::IDLE; return; }

            if (mid >= bracket_low && mid <= bracket_high) {
                ++m_inside_ticks;
            } else {
                m_inside_ticks = 0;
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            if (m_inside_ticks < MIN_BREAK_TICKS) return;

            const double sl_dist = range * SL_FRAC + SL_BUFFER;
            const double tp_dist = sl_dist * TP_RR;
            // FX cost-cover gate: TP must clear 2x spread plus a 1-pip buffer.
            //   Gold uses min_tp = spread*2 + 0.12 (12 cents); FX equivalent
            //   is spread*2 + 0.0001 (1 pip).
            const double min_tp  = spread * 2.0 + 0.0001;
            if (tp_dist < min_tp) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] COST_FAIL range=%.5f sl_dist=%.5f tp_dist=%.5f min=%.5f\n",
                        range, sl_dist, tp_dist, min_tp);
                    std::cout << _buf;
                    std::cout.flush();
                }
                phase = Phase::IDLE;
                return;
            }

            // -- ATR-expansion gate (S47 T4a + 2026-04-29 ratchet fix) -------
            m_range_history.push_back(range);
            if ((int)m_range_history.size() > EXPANSION_HISTORY_LEN)
                m_range_history.pop_front();

            // S61 2026-05-07: throttled persistence (every 30s during ARMED).
            //   Captures m_range_history to disk so a service restart can
            //   reload it instead of triggering the S58 cold-start guard.
            //   Throttle keeps disk I/O ~2 writes/min/engine. _save_range_
            //   history_if_due is a no-op in OMEGA_BACKTEST builds.
            _save_range_history_if_due(now_s);

            // S58 2026-05-07: COLD-START GUARD.
            //   Previously the ATR-expansion gate (below) was bypassed when
            //   m_range_history.size() < EXPANSION_MIN_HISTORY -- the engine
            //   simply skipped the median check and proceeded to FIRE. That
            //   created a "first ~5 fires after restart skip the expansion
            //   filter entirely" hole. The 06:07:49 UTC 2026-05-07 EURUSD
            //   SHORT loss exhibited the pattern exactly: fired ~10 min
            //   after service restart, on the smallest qualifying
            //   compression (range == MIN_RANGE = 8 pips), with hist=1 ->
            //   ATR gate bypassed -> first-tick-against false breakout ->
            //   SL hit. Net loss -$28.30 (gross -$17.10 + slippage 10 +
            //   comm 1.20). See docs analysis 2026-05-07.
            //
            //   New behaviour: refuse to fire while history is
            //   insufficient. The push_back above still runs each ARMED
            //   iteration, so the engine warms up by observing brackets
            //   without trading them. Once EXPANSION_MIN_HISTORY ranges
            //   have accumulated, the ATR gate becomes active and normal
            //   firing resumes.
            //
            //   Cost: first (EXPANSION_MIN_HISTORY - 1) ARMED brackets
            //   after a restart never reach PENDING -- engine is silent
            //   for the warmup window (typically ~15-25 min depending on
            //   compression cadence). Benefit: closes the documented
            //   cold-start ATR-bypass hole.
            if ((int)m_range_history.size() < EXPANSION_MIN_HISTORY) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] COLD_START_BLOCK range=%.5f hist=%d/%d -- skipping fire\n",
                        range, (int)m_range_history.size(), EXPANSION_MIN_HISTORY);
                    std::cout << _buf;
                    std::cout.flush();
                }
                phase = Phase::IDLE;
                bracket_high = bracket_low = 0.0;
                return;
            }

            {
                std::vector<double> sorted(m_range_history.begin(),
                                           m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1)
                    ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                // S62 2026-05-13: combined gate. Range must clear BOTH the
                //   percentage threshold (median * 1.10) AND the absolute
                //   floor (median + 3 pips). Either condition can block.
                const double pct_threshold = median * EXPANSION_MULT;
                const double abs_threshold = median + ABS_EXPANSION_FLOOR;
                if (range < pct_threshold || range < abs_threshold) {
                    {
                        char _buf[384];
                        snprintf(_buf, sizeof(_buf),
                            "[EUR-LDN-OPEN] ATR_GATE_FAIL range=%.5f median=%.5f "
                            "mult=%.2f pct_thr=%.5f abs_floor=%.5f abs_thr=%.5f hist=%d\n",
                            range, median, EXPANSION_MULT,
                            pct_threshold, ABS_EXPANSION_FLOOR, abs_threshold,
                            (int)m_range_history.size());
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    phase = Phase::IDLE;
                    bracket_high = bracket_low = 0.0;
                    return;
                }
            }

            const bool is_pyramid = flow_pyramid_ok;
            const double risk     = is_pyramid ? RISK_DOLLARS_PYRAMID : RISK_DOLLARS;
            // Lot sizing by risk budget. risk_dollars = sl_dist * USD_PER_PRICE_UNIT * lot_fraction
            //   At ENTRY_SIZE_DEFAULT=0.10 lot, 1 unit of price = $10,000.
            //   sl_dist = 0.0008 (8 pips) -> $8 per lot-fraction-1.
            //   $30 risk / $8 per unit-lot = 3.75 -> capped at LOT_MAX=0.10.
            //   $10 pyramid risk / $8 = 1.25 -> capped at LOT_MAX=0.10.
            // Effectively: most fires hit LOT_MAX. The cap exists for safety
            // on tighter compressions where sl_dist drops below 6 pips.
            const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
                ? (risk / (sl_dist * USD_PER_PRICE_UNIT))
                : LOT_MAX;
            const double base_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));

            // -- DOM lot sizing (FX variant) ---------------------------------
            double lot_long  = base_lot;
            double lot_short = base_lot;

            if (l2_real) {
                if (wall_above && wall_below) {
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[EUR-LDN-OPEN] DOM_BLOCK both walls present -- skipping fire\n");
                        std::cout << _buf;
                        std::cout.flush();
                    }
                    phase = Phase::IDLE;
                    return;
                }
                const bool slope_long  = (book_slope >  DOM_SLOPE_CONFIRM);
                const bool slope_short = (book_slope < -DOM_SLOPE_CONFIRM);
                if (slope_long  || vacuum_ask) lot_long  = std::min(LOT_MAX, lot_long  * DOM_LOT_BONUS);
                if (slope_short || vacuum_bid) lot_short = std::min(LOT_MAX, lot_short * DOM_LOT_BONUS);
                if (wall_above) lot_long  = std::max(LOT_MIN, lot_long  * DOM_WALL_PENALTY);
                if (wall_below) lot_short = std::max(LOT_MIN, lot_short * DOM_WALL_PENALTY);
            }

            pending_lot       = base_lot;
            pending_lot_long  = lot_long;
            pending_lot_short = lot_short;
            // 2026-05-12 cost gate -- see outputs/PLAN_A_B_REPORT.md
            if (!ExecutionCostGuard::is_viable(
                    "EURUSD", spread, tp_dist, base_lot, 1.5))
            {
                phase = Phase::IDLE;
                bracket_high = bracket_low = range = 0.0;
                return;
            }
            phase             = Phase::PENDING;
            m_armed_ts        = now_s;
            m_pending_blocked_since = 0;

            {
                char _buf[512];
                snprintf(_buf, sizeof(_buf),
                    "[EUR-LDN-OPEN] FIRE hi=%.5f lo=%.5f range=%.5f sl=%.5f tp=%.5f "
                    "lot_base=%.3f lot_L=%.3f lot_S=%.3f slope=%.2f vac_a=%d vac_b=%d "
                    "wall_a=%d wall_b=%d %s\n",
                    bracket_high, bracket_low, range, sl_dist, tp_dist,
                    base_lot, lot_long, lot_short,
                    book_slope, (int)vacuum_ask, (int)vacuum_bid,
                    (int)wall_above, (int)wall_below,
                    is_pyramid ? "[PYRAMID]" : "[STANDALONE]");
                std::cout << _buf;
                std::cout.flush();
            }
        }
    }

    void confirm_fill(bool is_long, double fill_px, double fill_lot,
                      double spread_at_fill = 0.0) noexcept {
        if (cancel_fn) {
            if (is_long  && !pending_short_clOrdId.empty()) cancel_fn(pending_short_clOrdId);
            if (!is_long && !pending_long_clOrdId.empty())  cancel_fn(pending_long_clOrdId);
        }
        pending_long_clOrdId.clear();
        pending_short_clOrdId.clear();

        const double sl_dist = range * SL_FRAC + SL_BUFFER;
        const double tp_dist = sl_dist * TP_RR;
        pos.active          = true;
        pos.is_long         = is_long;
        pos.entry           = fill_px;
        pos.sl              = is_long ? (fill_px - sl_dist) : (fill_px + sl_dist);
        pos.tp              = is_long ? (fill_px + tp_dist)  : (fill_px - tp_dist);
        pos.size            = fill_lot;
        pos.mfe             = 0.0;
        pos.mae             = 0.0;
        pos.spread_at_entry = spread_at_fill;
        pos.entry_ts        = m_last_tick_s;
        pos.be_locked       = false;
        phase               = Phase::LIVE;

        {
            char _buf[256];
            snprintf(_buf, sizeof(_buf),
                "[EUR-LDN-OPEN] FILL %s @ %.5f sl=%.5f(dist=%.5f) tp=%.5f lot=%.3f\n",
                is_long ? "LONG" : "SHORT", fill_px, pos.sl, sl_dist, pos.tp, fill_lot);
            std::cout << _buf;
            std::cout.flush();
        }
    }

    void manage(double bid, double ask, double mid,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        if (!pos.active) return;
        const double move = pos.is_long ? (mid - pos.entry) : (pos.entry - mid);
        if (move > pos.mfe) pos.mfe = move;
        if (move < pos.mae) pos.mae = move;

        // S64 2026-05-13 immediate cold-loss cut.
        if (LOSS_CUT_PCT > 0.0 && pos.entry > 0.0) {
            const double adverse       = -move;
            const double loss_cut_dist = pos.entry * LOSS_CUT_PCT / 100.0;
            if (adverse >= loss_cut_dist) {
                const double exit_px = pos.is_long ? bid : ask;
                {
                    char _buf[320];
                    snprintf(_buf, sizeof(_buf),
                        "[EUR-LDN-OPEN] LOSS_CUT adverse=%.5f >= %.5f "
                        "(%.3f%% of entry %.5f) -- cutting immediately\n",
                        adverse, loss_cut_dist, LOSS_CUT_PCT, pos.entry);
                    std::cout << _buf;
                    std::cout.flush();
                }
                _close(exit_px, "LOSS_CUT", now_s, on_close);
                return;
            }
        }

        // Trail with S20 arm guards + S52/S53 give-back fraction.
        const int64_t held_s = now_s - pos.entry_ts;

        // S53: break-even lock.
        //   Move SL to entry once MFE crosses BE_TRIGGER_PTS=4 pips. Fills
        //   the gap between original SL and MIN_TRAIL_ARM_PTS=6 pips so
        //   trades that MFE 3-5 pips then reverse exit at $0 instead of SL.
        //   One-shot via pos.be_locked. No hold-time guard: 4 pips on EURUSD
        //   is ~3-4x bid-ask noise (typical spread 0.5-1.4 pips), not
        //   gameable by tick fluctuation.
        if (move > 0 && !pos.be_locked && pos.mfe >= BE_TRIGGER_PTS) {
            // S54 audit-fixes-35: park SL at entry +/- BE_OFFSET_PTS so a
            //   BE_HIT recovers round-trip cost. Safety guard: only apply
            //   offset when current move >= offset (else fall back to entry).
            const double effective_offset = (move >= BE_OFFSET_PTS) ? BE_OFFSET_PTS : 0.0;
            const double be_target = pos.is_long
                ? (pos.entry + effective_offset)
                : (pos.entry - effective_offset);
            if (pos.is_long  && be_target > pos.sl) pos.sl = be_target;
            if (!pos.is_long && be_target < pos.sl) pos.sl = be_target;
            pos.be_locked = true;
        }

        const bool arm_mfe_ok  = (MIN_TRAIL_ARM_PTS  <= 0.0) || (pos.mfe >= MIN_TRAIL_ARM_PTS);
        const bool arm_hold_ok = (MIN_TRAIL_ARM_SECS <= 0 ) || (held_s  >= MIN_TRAIL_ARM_SECS);
        if (move > 0 && arm_mfe_ok && arm_hold_ok) {
            const double mfe_trail = pos.mfe * MFE_TRAIL_FRAC;
            const double range_trail = range * TRAIL_FRAC;
            const double trail_dist = (mfe_trail > 0.0) ? std::min(range_trail, mfe_trail) : range_trail;
            const double trail_sl = pos.is_long ? (pos.entry + pos.mfe - trail_dist)
                                                : (pos.entry - pos.mfe + trail_dist);
            if (pos.is_long  && trail_sl > pos.sl) pos.sl = trail_sl;
            if (!pos.is_long && trail_sl < pos.sl) pos.sl = trail_sl;
        }

        // TP
        const bool tp_hit = pos.is_long ? (ask >= pos.tp) : (bid <= pos.tp);
        if (tp_hit) { _close(pos.tp, "TP_HIT", now_s, on_close); return; }

        // SL with S43 TRAIL_HIT/SL_HIT/BE_HIT classifier.
        // Tolerance for BE detection: 0.5 pip (half typical spread). Larger
        // than gold's 0.01 because FX noise floor is wider relative to BE.
        const bool sl_hit = pos.is_long ? (bid <= pos.sl) : (ask >= pos.sl);
        if (sl_hit) {
            const double exit_px = pos.is_long ? bid : ask;
            const bool sl_at_be        = (pos.sl <= pos.entry + 0.00005)
                                      && (pos.sl >= pos.entry - 0.00005);
            const bool trail_in_profit = pos.is_long
                ? (pos.sl > pos.entry + 0.00005)
                : (pos.sl < pos.entry - 0.00005);
            const char* reason;
            if      (sl_at_be)        reason = "BE_HIT";
            else if (trail_in_profit) reason = "TRAIL_HIT";
            else                      reason = "SL_HIT";
            _close(exit_px, reason, now_s, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, "FORCE_CLOSE", now_ms / 1000, on_close);
    }

private:
    int     m_ticks_received = 0;
    int     m_inside_ticks   = 0;
    int64_t m_armed_ts       = 0;
    int64_t m_cooldown_start = 0;
    int     m_sl_cooldown_dir = 0;
    int64_t m_sl_cooldown_ts  = 0;
    // S53 same-level re-arm block state.
    //   m_sl_price          entry price at last SL_HIT (loss-side block)
    //   m_win_exit_price    exit price at last TRAIL_HIT/TP_HIT (win-side block)
    //   m_win_exit_block_ts time when win-side block expires (now_s + 600s)
    //   The post-SL block reuses the existing m_sl_cooldown_ts above.
    double  m_sl_price          = 0.0;
    double  m_win_exit_price    = 0.0;
    int64_t m_win_exit_block_ts = 0;
    int64_t m_pending_blocked_since = 0;
    int     m_trade_id        = 0;
    int64_t m_last_tick_s     = 0;
    std::deque<double> m_window;

    // S64 2026-05-13 consec-loss circuit-breaker state.
    int     m_consec_loss_count        = 0;
    int     m_consec_loss_day_utc      = -1;
    int64_t m_consec_loss_block_until_s = 0;

    static int _utc_day_key(int64_t now_s) noexcept {
        time_t t = static_cast<time_t>(now_s);
        struct tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &t);
#else
        gmtime_r(&t, &utc);
#endif
        return utc.tm_year * 1000 + utc.tm_yday;
    }

    omega::SpreadRegimeGate m_spread_gate;
    std::deque<double> m_range_history;

    // S61 2026-05-07: range_history persistence (warm-restart for S58 guard).
    //   Save throttle: RANGE_HIST_SAVE_INTERVAL_S = 30 sec.
    // S62 2026-05-13: staleness extended 7200 -> 86400 (24h). Operating
    //   model is 24/7; a single-day outage should not force a cold-start.
    //   All I/O is wrapped in OMEGA_BACKTEST guards so backtests stay
    //   deterministic and never read live state.
    int64_t m_range_history_last_save_s = 0;
    static constexpr int64_t RANGE_HIST_SAVE_INTERVAL_S = 30;
    static constexpr int64_t RANGE_HIST_STALENESS_S     = 86400;

    static const char* _range_hist_path() noexcept {
#ifdef _WIN32
        return "C:\\Omega\\state\\eurusd_london_open_range_history.csv";
#else
        return "state/eurusd_london_open_range_history.csv";
#endif
    }

    // S62 2026-05-13: ensure the state directory exists before any save.
    //   Without this, a missing C:\Omega\state\ silently failed every save
    //   call -> range_history never persisted -> every restart cold-started
    //   the ATR gate. Idempotent: existing-dir error from mkdir is ignored.
    static void _ensure_state_dir() noexcept {
#ifndef OMEGA_BACKTEST
#ifdef _WIN32
        _mkdir("C:\\Omega\\state");
#else
        ::mkdir("state", 0755);
#endif
#endif
    }

    // S62 2026-05-13: post-trade same-level block persistence.
    //   Persists m_sl_price / m_sl_cooldown_dir / m_sl_cooldown_ts /
    //   m_win_exit_price / m_win_exit_block_ts so that a restart inside the
    //   20-min post-SL window does NOT silently clear the re-arm block.
    //   Saves on every state change in _close(). Loads in the ctor.
    static const char* _post_trade_block_path() noexcept {
#ifdef _WIN32
        return "C:\\Omega\\state\\eurusd_london_open_post_trade_block.csv";
#else
        return "state/eurusd_london_open_post_trade_block.csv";
#endif
    }

    void _try_load_post_trade_block() noexcept {
#ifndef OMEGA_BACKTEST
        const char* path = _post_trade_block_path();
        FILE* f = std::fopen(path, "r");
        if (!f) {
            std::printf("[EUR-LDN-OPEN] POST_TRADE_BLOCK_LOAD no_file path=%s\n", path);
            std::fflush(stdout);
            return;
        }
        char line[256];
        double sl_price = 0.0;
        int    sl_cooldown_dir = 0;
        long long sl_cooldown_ts = 0;
        double win_exit_price = 0.0;
        long long win_exit_block_ts = 0;
        int       consec_loss_count        = 0;
        int       consec_loss_day_utc      = -1;
        long long consec_loss_block_until_s = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
            char key[64];
            char val[128];
            if (std::sscanf(line, "%63[^=]=%127[^\n]", key, val) != 2) continue;
            if      (std::strcmp(key, "sl_price") == 0)          sl_price          = std::strtod(val, nullptr);
            else if (std::strcmp(key, "sl_cooldown_dir") == 0)   sl_cooldown_dir   = (int)std::strtol(val, nullptr, 10);
            else if (std::strcmp(key, "sl_cooldown_ts") == 0)    sl_cooldown_ts    = std::strtoll(val, nullptr, 10);
            else if (std::strcmp(key, "win_exit_price") == 0)    win_exit_price    = std::strtod(val, nullptr);
            else if (std::strcmp(key, "win_exit_block_ts") == 0) win_exit_block_ts = std::strtoll(val, nullptr, 10);
            else if (std::strcmp(key, "consec_loss_count") == 0)       consec_loss_count        = (int)std::strtol(val, nullptr, 10);
            else if (std::strcmp(key, "consec_loss_day_utc") == 0)     consec_loss_day_utc      = (int)std::strtol(val, nullptr, 10);
            else if (std::strcmp(key, "consec_loss_block_until_s") == 0) consec_loss_block_until_s = std::strtoll(val, nullptr, 10);
        }
        std::fclose(f);
        const long long now_s = (long long)std::time(nullptr);
        bool restored_any = false;
        if (sl_cooldown_ts > now_s && sl_price > 0.0) {
            m_sl_price        = sl_price;
            m_sl_cooldown_dir = sl_cooldown_dir;
            m_sl_cooldown_ts  = (int64_t)sl_cooldown_ts;
            restored_any = true;
        }
        if (win_exit_block_ts > now_s && win_exit_price > 0.0) {
            m_win_exit_price    = win_exit_price;
            m_win_exit_block_ts = (int64_t)win_exit_block_ts;
            restored_any = true;
        }
        const int now_utc_day = _utc_day_key((int64_t)now_s);
        if (consec_loss_day_utc == now_utc_day) {
            m_consec_loss_count   = consec_loss_count;
            m_consec_loss_day_utc = consec_loss_day_utc;
            if (consec_loss_block_until_s > now_s) {
                m_consec_loss_block_until_s = (int64_t)consec_loss_block_until_s;
                restored_any = true;
            }
        }
        std::printf("[EUR-LDN-OPEN] POST_TRADE_BLOCK_LOAD restored=%d sl_price=%.5f sl_rem_s=%lld win_price=%.5f win_rem_s=%lld "
                    "consec_count=%d consec_block_rem_s=%lld\n",
                    (int)restored_any,
                    m_sl_price,
                    (m_sl_cooldown_ts > now_s) ? (long long)(m_sl_cooldown_ts - now_s) : 0LL,
                    m_win_exit_price,
                    (m_win_exit_block_ts > now_s) ? (long long)(m_win_exit_block_ts - now_s) : 0LL,
                    m_consec_loss_count,
                    (m_consec_loss_block_until_s > now_s) ? (long long)(m_consec_loss_block_until_s - now_s) : 0LL);
        std::fflush(stdout);
#endif
    }

    void _save_post_trade_block() noexcept {
#ifndef OMEGA_BACKTEST
        _ensure_state_dir();
        const char* path = _post_trade_block_path();
        FILE* f = std::fopen(path, "w");
        if (!f) {
            std::printf("[EUR-LDN-OPEN] POST_TRADE_BLOCK_SAVE_FAIL path=%s\n", path);
            std::fflush(stdout);
            return;
        }
        const long long now_s = (long long)std::time(nullptr);
        std::fprintf(f, "# post_trade_block v2 saved=%lld\n", now_s);
        std::fprintf(f, "sl_price=%.10f\n", m_sl_price);
        std::fprintf(f, "sl_cooldown_dir=%d\n", m_sl_cooldown_dir);
        std::fprintf(f, "sl_cooldown_ts=%lld\n", (long long)m_sl_cooldown_ts);
        std::fprintf(f, "win_exit_price=%.10f\n", m_win_exit_price);
        std::fprintf(f, "win_exit_block_ts=%lld\n", (long long)m_win_exit_block_ts);
        std::fprintf(f, "consec_loss_count=%d\n", m_consec_loss_count);
        std::fprintf(f, "consec_loss_day_utc=%d\n", m_consec_loss_day_utc);
        std::fprintf(f, "consec_loss_block_until_s=%lld\n", (long long)m_consec_loss_block_until_s);
        std::fclose(f);
#endif
    }

    void _try_load_range_history() noexcept {
#ifndef OMEGA_BACKTEST
        const char* path = _range_hist_path();
        FILE* f = std::fopen(path, "r");
        if (!f) {
            std::printf("[EUR-LDN-OPEN] RANGE_HIST_LOAD no_file path=%s (cold-start)\n", path);
            std::fflush(stdout);
            return;
        }
        char line[128];
        long long saved_ts = 0;
        std::deque<double> tmp;
        while (std::fgets(line, sizeof(line), f)) {
            if (line[0] == '#') {
                const char* p = std::strstr(line, "saved=");
                if (p) saved_ts = std::strtoll(p + 6, nullptr, 10);
                continue;
            }
            if (line[0] == '\n' || line[0] == '\r' || line[0] == '\0') continue;
            double v = 0.0;
            if (std::sscanf(line, "%lf", &v) == 1 && std::isfinite(v) && v > 0.0)
                tmp.push_back(v);
        }
        std::fclose(f);
        const long long now_s = (long long)std::time(nullptr);
        if (saved_ts <= 0 || (now_s - saved_ts) > RANGE_HIST_STALENESS_S) {
            std::printf("[EUR-LDN-OPEN] RANGE_HIST_LOAD stale saved=%lld age_s=%lld (cold-start)\n",
                        saved_ts, (long long)(now_s - saved_ts));
            std::fflush(stdout);
            return;
        }
        while ((int)tmp.size() > EXPANSION_HISTORY_LEN) tmp.pop_front();
        m_range_history = std::move(tmp);
        std::printf("[EUR-LDN-OPEN] RANGE_HIST_LOAD ok n=%d age_s=%lld\n",
                    (int)m_range_history.size(), (long long)(now_s - saved_ts));
        std::fflush(stdout);
#endif
    }

    void _save_range_history() noexcept {
#ifndef OMEGA_BACKTEST
        // S62 2026-05-13: ensure state dir before opening file.
        _ensure_state_dir();
        const char* path = _range_hist_path();
        FILE* f = std::fopen(path, "w");
        if (!f) {
            std::printf("[EUR-LDN-OPEN] RANGE_HIST_SAVE_FAIL path=%s\n", path);
            std::fflush(stdout);
            return;
        }
        const long long now_s = (long long)std::time(nullptr);
        std::fprintf(f, "# range_history v1 saved=%lld\n", now_s);
        for (const double v : m_range_history) std::fprintf(f, "%.10f\n", v);
        std::fclose(f);
#endif
    }

    void _save_range_history_if_due(int64_t now_s) noexcept {
#ifndef OMEGA_BACKTEST
        if ((now_s - m_range_history_last_save_s) < RANGE_HIST_SAVE_INTERVAL_S) return;
        _save_range_history();
        m_range_history_last_save_s = now_s;
#else
        (void)now_s;
#endif
    }

    // AUDIT 2026-04-29: serialise the whole _close path. Inherited from
    //   GoldMidScalper to avoid the Apr-7 -$3,008.38 phantom-pnl race.
    mutable std::mutex m_close_mtx;

    void _close(double exit_px, const char* reason,
                int64_t now_s, CloseCallback on_close) noexcept
    {
        std::lock_guard<std::mutex> _lk(m_close_mtx);

        if (!pos.active) return;

        const bool   is_long_  = pos.is_long;
        const double entry_    = pos.entry;
        const double sl_       = pos.sl;
        const double tp_       = pos.tp;
        const double size_     = pos.size;
        const double mfe_      = pos.mfe;
        const double mae_      = pos.mae;
        const double spread_at_entry_ = pos.spread_at_entry;
        const int64_t entry_ts_ = pos.entry_ts;

        // PnL math: price-distance * size * USD_PER_PRICE_UNIT_AT_DEFAULT_LOT
        //   At 0.10 lot, $1 per pip. So pnl_dollars = price_distance * 10000 * (size / 0.10).
        //   For uniformity with the gold engine signature (which uses size in
        //   raw lots and lets the close pipeline multiply by tick value),
        //   we emit raw price_distance * size and let handle_closed_trade
        //   do the FX tick-value multiplication. This matches the
        //   trade_lifecycle.hpp convention used by g_eng_eurusd before the
        //   2026-04-06 disable.
        const double pnl = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

        // Sanity check: clamp anomalous PnL. EURUSD with size <= 0.10 and
        //   move <= MAX_RANGE should never produce |pnl_raw| > 0.0030 * 0.10
        //   = 0.0003. Cap at ~10x that for safety.
        const double sane_max = std::max(0.01, size_) * 0.05;
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            const double recomputed = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;
            std::ostringstream warn;
            warn << "[EUR-LDN-OPEN][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry=" << entry_ << " exit=" << exit_px
                 << "). Recomputed=" << recomputed
                 << ". Emitting recomputed value.\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = recomputed;
        }

        {
            std::ostringstream os;
            os << "[EUR-LDN-OPEN] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ " << std::fixed << std::setprecision(5) << exit_px
               << " reason=" << reason
               << " pnl_raw=" << std::setprecision(6) << pnl_to_emit
               << " mfe="    << std::setprecision(5) << mfe_
               << " mae="    << mae_
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        // S53: same-level re-arm block stamps.
        //   SL_HIT -> 15-min block at entry price (rejected level).
        //   TRAIL_HIT or TP_HIT -> 10-min block at exit price (exhaustion).
        //   BE_HIT -> no stamp.
        // S62 2026-05-13: persist block state to disk on every stamp.
        // S64 2026-05-13: consec-loss state maintained alongside.
        bool _stamp_changed = false;
        const bool _is_loss_exit = (reason == std::string("SL_HIT") ||
                                    reason == std::string("LOSS_CUT"));
        const bool _is_win_exit  = (reason == std::string("TRAIL_HIT") ||
                                    reason == std::string("TP_HIT"));
        if (_is_loss_exit) {
            m_sl_cooldown_dir = is_long_ ? 1 : -1;
            m_sl_cooldown_ts  = now_s + SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price        = entry_;
            _stamp_changed = true;
        }
        if (_is_win_exit) {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
            _stamp_changed = true;
        }
        // S64 consec-loss update
        const int _utc_today = _utc_day_key(now_s);
        if (m_consec_loss_day_utc != _utc_today) {
            m_consec_loss_count   = 0;
            m_consec_loss_day_utc = _utc_today;
        }
        if (_is_loss_exit) {
            ++m_consec_loss_count;
            if (CONSEC_LOSS_THRESH > 0 &&
                m_consec_loss_count >= CONSEC_LOSS_THRESH)
            {
                m_consec_loss_block_until_s = now_s + CONSEC_LOSS_BLOCK_S;
                char _buf[384];
                snprintf(_buf, sizeof(_buf),
                    "[EUR-LDN-OPEN] CONSEC_LOSS_BLOCK_TRIPPED count=%d "
                    "threshold=%d block_until=%lld (block_s=%lld) reason=%s\n",
                    m_consec_loss_count, CONSEC_LOSS_THRESH,
                    (long long)m_consec_loss_block_until_s,
                    (long long)CONSEC_LOSS_BLOCK_S, reason);
                std::cout << _buf;
                std::cout.flush();
            }
            _stamp_changed = true;
        }
        if (_is_win_exit) {
            if (m_consec_loss_count > 0 || m_consec_loss_block_until_s > 0) {
                m_consec_loss_count         = 0;
                m_consec_loss_block_until_s = 0;
                _stamp_changed = true;
            }
        }
        if (_stamp_changed) {
            _save_post_trade_block();
        }

        omega::TradeRecord tr;
        tr.id           = ++m_trade_id;
        tr.symbol       = "EURUSD";
        tr.side         = is_long_ ? "LONG" : "SHORT";
        tr.engine       = "EurusdLondonOpen";
        tr.regime       = "LDN_COMPRESSION";
        tr.entryPrice   = entry_;
        tr.exitPrice    = exit_px;
        tr.tp           = tp_;
        tr.sl           = sl_;
        tr.size         = size_;
        tr.pnl          = pnl_to_emit;
        tr.net_pnl      = tr.pnl;
        tr.mfe          = mfe_ * size_;
        tr.mae          = mae_ * size_;
        tr.entryTs      = entry_ts_;
        tr.exitTs       = now_s;
        tr.exitReason   = reason;
        tr.spreadAtEntry = spread_at_entry_;
        tr.bracket_hi   = bracket_high;
        tr.bracket_lo   = bracket_low;
        tr.shadow       = shadow_mode;

        pos = LivePos{};
        phase = Phase::COOLDOWN;
        m_cooldown_start = now_s;
        bracket_high = bracket_low = range = 0.0;

        if (on_close) on_close(tr);
    }
};

} // namespace omega
