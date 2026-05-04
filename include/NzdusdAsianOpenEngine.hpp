#pragma once
#include <iomanip>
#include <iostream>
#include "SpreadRegimeGate.hpp"
#include "OmegaNewsBlackout.hpp"
// =============================================================================
// NzdusdAsianOpenEngine.hpp  --  Wellington/Tokyo Asian-open compression NZDUSD
// =============================================================================
//
// 2026-05-04 SESSION DESIGN (Claude / Jo):
//   Sister engine to AudusdSydneyOpenEngine and UsdjpyAsianOpenEngine. Targets
//   the 22:00-04:00 UTC Wellington-open + Tokyo-handoff window for NZDUSD.
//   1:1 architectural port of AudusdSydneyOpenEngine with NZDUSD-specific
//   DOM plumbing (vacuum-only -- mirrors AUD/JPY DOM shape).
//
//   Audit lineage:
//     - Final piece of the FX re-enable wave that covered EURUSD (2026-05-02),
//       USDJPY (2026-05-02), GBPUSD (2026-05-04), AUDUSD (2026-05-04). NZDUSD
//       was the last remaining [FX-NO-ENGINE] diag stub in tick_fx.hpp's
//       on_tick_audusd handler.
//     - Inherits all S55-S59 USDJPY tuned constants via AudusdSydneyOpen as
//       PRE-SWEEP defaults (already rescaled from JPY 0.01-pip units to AUD/
//       NZD 0.0001-pip units). NZD-optimal config will be determined by a
//       parallel sweep harness before live promotion.
//     - Does NOT inherit MacroCrashEngine signal model (disabled 2026-04-30
//       at 4.8% WR / -10,849pts).
//     - Same BE-lock + same-level block + news-blackout pattern as AUDUSD.
//
//   Pip math reference (NZDUSD):
//     1 pip          = 0.0001 price (same as EUR/GBP/AUD; kiwi is also a
//                                     USD-quote major, four-decimal standard)
//     Daily ATR      = 50-90 pips (slightly tighter than AUD's 50-100; the
//                                     kiwi tracks AUD closely but with lower
//                                     headline vol and thinner liquidity)
//     Typical spread = 0.8 to 2.5 pips on cTrader/BlackBull (wider than AUD's
//                                     0.7-2.0 due to lower depth, especially
//                                     during the pre-Sydney 21-22 UTC gap)
//     Pip value      = $1 at 0.10 lot, $10 at 1.00 lot (USD-quoted, identical
//                                     to AUD/EUR/GBP pip value -- same quote
//                                     currency)
//
//   Math (range = compression structure size, in price units, S59 SL_FRAC=1.00, RR=0.5):
//     range 0.0020 -> 20 pips -> SL 0.0022 (22 pips) -> TP 0.0011 (11 pips)
//     range 0.0025 -> 25 pips -> SL 0.0027 (27 pips) -> TP 0.0014 (14 pips)
//     range 0.0030 -> 30 pips -> SL 0.0032 (32 pips) -> TP 0.0016 (16 pips)
//     range 0.0040 -> 40 pips -> SL 0.0042 (42 pips) -> TP 0.0021 (21 pips)
//     range 0.0050 -> 50 pips -> SL 0.0052 (52 pips) -> TP 0.0026 (26 pips)
//
//   STRUCTURE_LOOKBACK = 600 (PRE-SWEEP DEFAULT, matches AUDUSD/USDJPY).
//   Wellington/Tokyo NZDUSD tick rate is the lowest of the FX-major Asian
//   handoff cohort (typically 30-70 ticks/min -- AUD runs 40-90, JPY 50-100
//   in Tokyo proper), so 600 ticks ~= 8-20 min structural lookback. Sweep
//   should retest {300, 400, 600, 900} once NZD-specific data is collected.
//
// SAFETY:
//   - Defaults to shadow_mode = true. Live promotion requires explicit
//     authorisation after a 2-week paper run shows positive expectancy.
//   - 30-trade minimum sample, WR >= 60% per the USDJPY S56 promotion gate
//     (NZD engine inherits the same gate via AUD lineage).
//   - Uses MIN_BREAK_TICKS = 5 (matches AUDUSD/USDJPY/EURUSD/gold).
//   - Inherits all audit-validated guards from the AUDUSD/USDJPY lineage:
//       S20 trail-arm guards (MIN_TRAIL_ARM_PTS=0.0006, MIN_TRAIL_ARM_SECS=30)
//       S43 mae tracker
//       S47 T4a ATR-expansion gate (EXPANSION_MULT=1.10) with ratchet fix
//       S51 1A.1.a spread_at_entry capture
//       S55 BE-lock at 6 pips MFE
//       S56 TRAIL_FRAC=0.30, S58 MFE_TRAIL_FRAC=0.15, S59 SL_FRAC=1.00 / TP_RR=0.5
//       S56 same-level re-arm block (20-pip radius, 20min post-SL / 10min post-win)
//       AUDIT 2026-04-29 mutex on _close path
//       audit-fixes-18 SpreadRegimeGate per-engine
//
// SESSION WINDOW:
//   LIVE TARGET: 22:00 UTC <= now_hh < 04:00 UTC (next day, wraparound)
//                                                ->  arming allowed.
//   Wellington opens 22:00 UTC winter / 21:00 UTC summer (NZDT/NZST shift).
//   Sydney joins at 22:00 UTC (already open through Wellington's first hour
//   in NZST). Tokyo at 00:00 UTC catches the NZD-side liquidity injection.
//   The 22-04 window stretches one hour later than AUDUSD's 22-02 because
//   the kiwi's strongest compression-to-breakout transitions tend to occur
//   in the post-Tokyo-open hours when AUDNZD cross flows settle. 04:00 UTC
//   is the pre-Frankfurt cutoff beyond which NZDUSD shifts toward European-
//   session USD-driven moves -- a different signal regime.
//
//   WRAPAROUND NOTE: the existing `< START || >= END` window check does NOT
//   handle a wraparound window (START=22, END=4 would reject all hours).
//   When this engine is promoted to live, the session check needs to switch
//   to a wraparound-aware form, e.g.:
//       const bool in_window = (SESSION_END_HOUR_UTC > SESSION_START_HOUR_UTC)
//           ? (utc.tm_hour >= SESSION_START_HOUR_UTC && utc.tm_hour < SESSION_END_HOUR_UTC)
//           : (utc.tm_hour >= SESSION_START_HOUR_UTC || utc.tm_hour < SESSION_END_HOUR_UTC);
//       if (!in_window) return;
//   For the shadow phase the window is widened to 0-24 (S57 audit-fixes-36),
//   so the wraparound issue is moot until promotion.
//
//   SHADOW (current): full 24h (START=0, END=24). Same S57 audit-fixes-36
//   widening as EURUSD/GBPUSD/USDJPY/AUDUSD -- shadow engines fire AS IF
//   live so we get visible ledger/PnL during validation. Engine still
//   self-gates on news blackout, spread, ATR, same-level block, and
//   compression-range formation. It will fire whenever NZDUSD presents a
//   valid setup, exactly like the unrestricted gold engines do.
//   (Existing positions still managed via manage() regardless of window.)
//
// NEWS BLACKOUT:
//   At IDLE->ARMED transition, consult g_news_blackout.is_blocked("NZDUSD",..).
//   NZDUSD is in the USD symbol set (NFP/CPI/FOMC) AND the NZD symbol set
//   (RBNZ, NZ CPI, NZ jobs via country="NZD") per OmegaNewsBlackout.hpp:
//   392-399. The blackout layer auto-includes both currency sides without
//   any per-engine filter list -- the single is_blocked("NZDUSD",..) call
//   covers RBNZ, NZ events, NFP, CPI, FOMC, and the Forex Factory live
//   calendar overlay (config.hpp:278 refresh path).
//   Existing positions exit via SL/TP/MAX_HOLD inside manage() -- not via
//   this gate -- to avoid forcing closes on already-open trades.
//
// DOM FILTER (NZDUSD variant):
//   At PENDING->FIRE time, DOM confirms which side has a clear path.
//   NZD-side fields available in g_macro_ctx (mirrors AUD/JPY -- vacuum-only):
//     - nzd_vacuum_ask / nzd_vacuum_bid
//   NOT available: nzd_wall_above / nzd_wall_below / nzd_slope.
//   The dispatcher passes book_slope = 0.0, wall_above = wall_below = false
//   so the slope/wall branches are naturally inert. Vacuum fields drive
//   the lot bonus. l2_real flag should be wired to
//   g_macro_ctx.ctrader_l2_live -- when false, DOM filter is bypassed
//   (safe fallback, both sides equal lots).
//
// SIZING:
//   Standalone: risk_dollars = $30, SL_dist = range * 1.00 + 0.0002 (2 pips)
//   Pyramid:    risk_dollars = $10 (30% addon), same SL formula
//   Lot range:  0.01 to 0.20 (LOT_MIN..LOT_MAX, S56 half-Kelly cap).
//               At 0.10 lot baseline, 1 pip = $1; a 22-pip SL = $22 risk
//               -- within the $30 budget but tighter than EUR/GBP because
//               S59 SL_FRAC=1.00 places SL at the full opposite end of
//               the compression structure.
//   ENTRY_SIZE_DEFAULT = 0.10 lot (matches EURUSD/GBPUSD/AUDUSD/USDJPY).
//
//   USD_PER_PRICE_UNIT = 10000.0 -- at 0.10 lot, 1 unit of price (1.0 NZD)
//   = 10000 pips * $1/pip = $10,000. Identical to EURUSD/GBPUSD/AUDUSD
//   because NZD is a USD-quote major. Real PnL conversion uses the standard
//   pipeline via tick_value_multiplier in the close pipeline (no live-rate
//   conversion needed -- USD is the quote currency).
//
// LOG NAMESPACE:
//   All log lines use prefix [NZD-ASN-OPEN] / [NZD-ASN-OPEN-DIAG].
//   tr.engine = "NzdusdAsianOpen" (registered in engine_init.hpp).
//   tr.regime = "ASN_COMPRESSION".
// =============================================================================

#include <cstdint>
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
#include "OmegaTradeLedger.hpp"

namespace omega {

class NzdusdAsianOpenEngine {
public:
    // -- Parameters (PRE-SWEEP defaults; mirror AudusdSydneyOpen with NZD pip scale) --
    // Lookback window: 600 ticks. NOTE -- Wellington/Tokyo NZDUSD has the
    //   lowest tick rate of the FX-major Asian cohort; 600 ticks ~= 8-20
    //   min in NZD vs ~7-15 min in AUD. Sweep candidate set:
    //   {300, 400, 600, 900}.
    static constexpr int    STRUCTURE_LOOKBACK   = 600;
    // Warmup: refuse new arming until at least this many ticks have arrived.
    //   Wellington NZDUSD cold-start is the quietest of any FX major; 60
    //   ticks may be too few -- sweep should also try 90/120.
    static constexpr int    MIN_ENTRY_TICKS      = 60;
    // Sweep guard: price must sit inside the formed bracket for this many
    //   consecutive ticks before stop orders are sent. Mirrors
    //   AUDUSD/USDJPY/EURUSD/gold.
    static constexpr int    MIN_BREAK_TICKS      = 5;
    // S59 lineage (USDJPY/AUDUSD): MIN_RANGE = 20 pips, MAX_RANGE = 50 pips.
    //   NZDUSD typical Wellington-Tokyo compressions run 15-30 pips (very
    //   close to AUD); 20-pip floor cuts dribble micro-ranges that lack
    //   genuine breakout edge. 50-pip ceiling rejects post-news anomaly
    //   compressions (RBNZ spike, NZ CPI gap) that span events cleared
    //   during the lookback window.
    static constexpr double MIN_RANGE            = 0.0020;
    static constexpr double MAX_RANGE            = 0.0050;
    // S59 lineage: SL_FRAC = 1.00. SL placed at the full opposite side of
    //   the compression structure (no fractional shrink). Wider SL means
    //   bigger individual losses but materially fewer of them; net WR
    //   jumped 79.5% -> 83.9% on the USDJPY 14mo backtest. NZD inherits
    //   as pre-sweep default via AUD lineage.
    static constexpr double SL_FRAC              = 1.00;
    // SL_BUFFER = 2 pips: spread (0.8-2.5 pip on NZD) + slippage cushion.
    //   Slightly tight at the upper-spread end of the NZD distribution;
    //   the MAX_SPREAD gate (below) will reject those moments anyway.
    static constexpr double SL_BUFFER            = 0.0002;
    // S59 lineage: TP_RR = 0.5. TP sits at ~10-15 pips (= p75 winner-MFE
    //   on USDJPY backtest), banking early gains on trades that would
    //   otherwise dribble back to a smaller trail exit. NZD inherits as
    //   pre-sweep default; sweep should validate against NZD's slightly
    //   tighter daily ATR.
    static constexpr double TP_RR                = 0.5;
    // S56 lineage: TRAIL_FRAC = 0.30 (wider trail; lets winners run).
    static constexpr double TRAIL_FRAC           = 0.30;
    // Trail-arm guards (S20 lineage). MIN_TRAIL_ARM_PTS = 6 pips.
    //   NZDUSD pip-per-second comparable to AUD/USDJPY in Asian session;
    //   30-second hold gate carries over unchanged.
    static constexpr double MIN_TRAIL_ARM_PTS    = 0.0006;
    static constexpr int    MIN_TRAIL_ARM_SECS   = 30;
    // S58 lineage: MFE_TRAIL_FRAC = 0.15 (tighter trail captures more of
    //   the win on small-MFE runs typical of Asian-session moves).
    static constexpr double MFE_TRAIL_FRAC       = 0.15;
    // S55 lineage: BE-lock at 6 pips MFE. With BE-trigger == trail-arm
    //   threshold, BE-lock and trail-arm fire simultaneously, eliminating
    //   the BE-only zone -- trades transition seamlessly from BE-protected
    //   to trail-protected.
    static constexpr double BE_TRIGGER_PTS       = 0.0006;
    // S54 2026-05-04 (audit-fixes-35): BE-exit slippage offset.
    //   NZDUSD pip = 0.0001 (same as EUR/GBP/AUD). Round-trip cost on
    //   default 0.10 lot is approx 1.5-3 pips on NZD (spread ~1.4 + slip
    //   ~0.5 + commission). Park SL at entry +/- BE_OFFSET_PTS (~1.5 pips)
    //   so a BE_HIT recovers the cost. Same logic as the EUR/GBP/AUD/JPY
    //   engines, scaled to NZD pip size (identical to AUD).
    static constexpr double BE_OFFSET_PTS        = 0.00015;
    // S56 lineage: SAME_LEVEL_BLOCK_PTS = MIN_RANGE = 20 pips.
    //   POST_SL block 1200s (20 min); POST_WIN block 600s (10 min).
    //   Wider block radius than EUR (8 pips) because NZD's wider
    //   compression floor (20 pips vs EUR's 8) needs a proportionally
    //   wider exclusion zone to avoid re-firing on the same level.
    static constexpr double SAME_LEVEL_BLOCK_PTS         = 0.0020;
    static constexpr int    SAME_LEVEL_POST_SL_BLOCK_S   = 1200; // 20 min after SL
    static constexpr int    SAME_LEVEL_POST_WIN_BLOCK_S  = 600;  // 10 min after TP/TRAIL
    // MAX_SPREAD = 2.5 pips. Reject if spread blew out beyond typical 0.8-2.5.
    //   Wider than AUD's 2-pip cap because NZD's normal upper bound is
    //   wider; this preserves the same headroom above typical conditions.
    //   NZD spreads can spike sharply during the pre-Wellington 20-22 UTC
    //   illiquidity gap and the NY 5pm rollover (21:00 UTC); the cap
    //   blocks arming during those windows even when the live target
    //   session is widened.
    static constexpr double MAX_SPREAD           = 0.00025;
    static constexpr double RISK_DOLLARS         = 30.0;
    static constexpr double RISK_DOLLARS_PYRAMID = 10.0;
    // USD value of 1 unit of price (1.0) at default lot 0.10:
    //   1 pip (0.0001) at 0.10 lot = $1   ->  1 unit of price = 10000 * $1 = $10,000.
    //   Identical to EURUSD/GBPUSD/AUDUSD: NZD is a USD-quote major with
    //   the same pip convention.
    static constexpr double USD_PER_PRICE_UNIT   = 10000.0;
    // S56 lineage: ENTRY_SIZE_DEFAULT 0.10, LOT_MIN 0.01, LOT_MAX 0.20.
    //   PRE-SWEEP default; NZDUSD-specific Kelly analysis from the parallel
    //   sweep should confirm (or downgrade) this LOT_MAX before live
    //   promotion. DO NOT exceed 0.20 until 2 weeks of shadow data confirm
    //   OOS WR >= 60%.
    static constexpr double ENTRY_SIZE_DEFAULT   = 0.10;
    static constexpr double LOT_MIN              = 0.01;
    static constexpr double LOT_MAX              = 0.20;
    // Pending timeout: FX breaks fast or resets. Same value as
    //   EURUSD/GBPUSD/AUDUSD/USDJPY.
    static constexpr int    PENDING_TIMEOUT_S    = 180;
    // S56 lineage: COOLDOWN_S = 120s. Same-level block (post-SL 1200s,
    //   post-win 600s) is the primary anti-chop guard.
    static constexpr int    COOLDOWN_S           = 120;
    // Session window (UTC hours).
    // S57 2026-05-04 (audit-fixes-36): session window widened to full 24h.
    //   PRIOR (live target): 22:00-04:00 UTC (6h Wellington open + Tokyo
    //   handoff wraparound window -- one hour wider than AUD's 22-02 to
    //   capture post-Tokyo-open hours when AUDNZD cross flows settle).
    //   The wraparound form (START=22, END=4) needs the wraparound-aware
    //   check described in the SESSION WINDOW header comment block above
    //   when promoted to live.
    //   Per Jo (2026-05-04): shadow engines must fire AS IF live and
    //   produce visible ledger/PnL like the gold shadow engines. Same fix
    //   as EurusdLondonOpen / GbpusdLondonOpen / UsdjpyAsianOpen /
    //   AudusdSydneyOpen -- widen to 24h so the engine self-gates on
    //   news/spread/ATR/range formation rather than wall-clock hour.
    //   Re-tighten back to 22-04 when promoting to live (and add the
    //   wraparound check above). Engine still respects all other gates.
    static constexpr int    SESSION_START_HOUR_UTC = 0;
    static constexpr int    SESSION_END_HOUR_UTC   = 24;
    static constexpr double DOM_SLOPE_CONFIRM    = 0.15;
    static constexpr double DOM_LOT_BONUS        = 1.3;
    static constexpr double DOM_WALL_PENALTY     = 0.5;

    // ATR-expansion gate (S47 T4a, ratchet fix 2026-04-29).
    static constexpr int    EXPANSION_HISTORY_LEN = 20;
    static constexpr int    EXPANSION_MIN_HISTORY = 5;
    static constexpr double EXPANSION_MULT        = 1.10;

    enum class Phase { IDLE, ARMED, PENDING, LIVE, COOLDOWN };
    Phase phase = Phase::IDLE;

    // 2026-05-04: shadow ON by default. Promote to live ONLY after a clean
    //   2-week paper validation showing positive expectancy in the 20-50
    //   pip capture zone with WR >= 60%. Live promotion via engine_init.hpp
    //   override -- do NOT change this default in the header.
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
                    "[NZD-ASN-OPEN-DIAG] ticks=%d phase=%d window=%d/%d range=%.5f spread=%.5f\n",
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
            if (ask >= bracket_high) { confirm_fill(true,  bracket_high, pending_lot_long,  spread); return; }
            if (bid <= bracket_low)  { confirm_fill(false, bracket_low,  pending_lot_short, spread); return; }
            if (now_s - m_armed_ts > PENDING_TIMEOUT_S) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[NZD-ASN-OPEN] PENDING TIMEOUT after %ds -- resetting\n",
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

        // Session window gate: 22:00-04:00 UTC live target (Wellington +
        // Tokyo handoff). Outside this window, stay IDLE. Existing LIVE/
        // PENDING/COOLDOWN paths above already returned. (Currently widened
        // to 0-24 in shadow mode -- see SESSION_START_HOUR_UTC /
        // SESSION_END_HOUR_UTC comments. The wraparound-aware check noted
        // in the header block must be added here when promoting to live
        // with START=22 / END=4.)
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

        // News blackout gate: RBNZ / NZ CPI / NZ jobs block NZDUSD via the
        // NZD currency set; NFP/CPI/FOMC block via the USD set. NZDUSD is
        // in both per OmegaNewsBlackout.hpp:392-399. Forex Factory live
        // calendar (g_live_calendar) layered on top by config.hpp:278
        // refresh.
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
        if (g_news_blackout.is_blocked("NZDUSD", now_s)) {
            return;
        }

        const bool flow_pyramid_ok = flow_live && flow_be_locked && flow_trail_stage >= 1;
        if (flow_live && !flow_pyramid_ok && phase == Phase::IDLE) return;

        double w_hi = *std::max_element(m_window.begin(), m_window.end());
        double w_lo = *std::min_element(m_window.begin(), m_window.end());
        range = w_hi - w_lo;

        // -- IDLE -> ARMED ---------------------------------------------------
        if (phase == Phase::IDLE) {
            // S53/S56: same-level re-arm block.
            //   Block re-arming when the new compression's hi or lo overlaps
            //   a recent exit price within SAME_LEVEL_BLOCK_PTS=20 pips and
            //   the relevant cooldown is still active. Continuation captured
            //   naturally once price moves past the block radius.
            if (m_sl_price > 0.0 && now_s < m_sl_cooldown_ts) {
                if (std::fabs(w_hi - m_sl_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_sl_price) < SAME_LEVEL_BLOCK_PTS) {
                    return;
                }
            }
            if (m_win_exit_price > 0.0 && now_s < m_win_exit_block_ts) {
                if (std::fabs(w_hi - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS ||
                    std::fabs(w_lo - m_win_exit_price) < SAME_LEVEL_BLOCK_PTS) {
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
                        "[NZD-ASN-OPEN] ARMED hi=%.5f lo=%.5f range=%.5f spread=%.5f\n",
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
            //   EUR/GBP/AUD use spread*2 + 0.0001 (1 pip in their units);
            //   NZDUSD equivalent is spread*2 + 0.0001 (1 pip in NZD units --
            //   same 0.0001 pip scale as the other USD-quote majors).
            const double min_tp  = spread * 2.0 + 0.0001;
            if (tp_dist < min_tp) {
                {
                    char _buf[256];
                    snprintf(_buf, sizeof(_buf),
                        "[NZD-ASN-OPEN] COST_FAIL range=%.5f sl_dist=%.5f tp_dist=%.5f min=%.5f\n",
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

            if ((int)m_range_history.size() >= EXPANSION_MIN_HISTORY) {
                std::vector<double> sorted(m_range_history.begin(),
                                           m_range_history.end());
                std::sort(sorted.begin(), sorted.end());
                const size_t n = sorted.size();
                const double median = (n % 2 == 1)
                    ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
                const double threshold = median * EXPANSION_MULT;
                if (range < threshold) {
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[NZD-ASN-OPEN] ATR_GATE_FAIL range=%.5f median=%.5f mult=%.2f threshold=%.5f hist=%d\n",
                            range, median, EXPANSION_MULT, threshold,
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
            //   sl_dist = 0.0022 (22 pips) -> $22 per lot-fraction-1.
            //   $30 risk / $22 per unit-lot = 1.36 -> capped at LOT_MAX=0.20.
            //   $10 pyramid risk / $22 = 0.45 -> ~0.045 lot, then clamped to LOT_MIN.
            // Same tighter sizing as AUD because S59 SL_FRAC=1.00 places SL
            // at the full opposite side of the compression structure.
            const double risk_lot = (sl_dist * USD_PER_PRICE_UNIT > 0.0)
                ? (risk / (sl_dist * USD_PER_PRICE_UNIT))
                : LOT_MAX;
            const double base_lot = std::max(LOT_MIN, std::min(LOT_MAX, risk_lot));

            // -- DOM lot sizing (NZDUSD variant) -----------------------------
            // Note: nzd_wall_above / nzd_wall_below do not exist in g_macro_ctx;
            //   the dispatcher in tick_fx.hpp passes wall_above = wall_below =
            //   false unconditionally. The wall branches below remain inert
            //   for NZDUSD (no DOM_BLOCK / no DOM_WALL_PENALTY scaling).
            //   Same DOM-shape as AUDUSD/USDJPY: vacuum-only.
            double lot_long  = base_lot;
            double lot_short = base_lot;

            if (l2_real) {
                if (wall_above && wall_below) {
                    {
                        char _buf[256];
                        snprintf(_buf, sizeof(_buf),
                            "[NZD-ASN-OPEN] DOM_BLOCK both walls present -- skipping fire\n");
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
            phase             = Phase::PENDING;
            m_armed_ts        = now_s;
            m_pending_blocked_since = 0;

            {
                char _buf[512];
                snprintf(_buf, sizeof(_buf),
                    "[NZD-ASN-OPEN] FIRE hi=%.5f lo=%.5f range=%.5f sl=%.5f tp=%.5f "
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
                "[NZD-ASN-OPEN] FILL %s @ %.5f sl=%.5f(dist=%.5f) tp=%.5f lot=%.3f\n",
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

        // Trail with S20 arm guards + S52/S53 give-back fraction.
        const int64_t held_s = now_s - pos.entry_ts;

        // S55: break-even lock.
        //   Move SL to entry once MFE crosses BE_TRIGGER_PTS=6 pips. With
        //   BE-trigger == trail-arm, BE-lock and trail-arm fire simultaneously
        //   on the same tick -- trades transition seamlessly from BE-protected
        //   to trail-protected. One-shot via pos.be_locked. No hold-time
        //   guard: 6 pips on NZDUSD is ~2.5-7x bid-ask noise (typical spread
        //   0.8-2.5 pips), not gameable by tick fluctuation.
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
        // Tolerance for BE detection: 0.5 pip (half typical spread). Same
        // 0.00005 as EUR/GBP/AUD because NZD shares the same 0.0001 pip
        // scale.
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

    omega::SpreadRegimeGate m_spread_gate;
    std::deque<double> m_range_history;

    // AUDIT 2026-04-29: serialise the whole _close path. Inherited from
    //   GoldMidScalper / EurusdLondonOpen / UsdjpyAsianOpen / AudusdSydneyOpen
    //   to avoid the Apr-7 -$3,008.38 phantom-pnl race.
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

        // PnL math: emit raw price_distance * size and let handle_closed_trade
        //   apply the FX tick-value multiplication via tick_value_multiplier.
        //   For NZDUSD this resolves through the standard USD-quote path
        //   (no live cross-rate conversion needed -- USD is the quote
        //   currency). Matches the trade_lifecycle.hpp convention used by
        //   the EURUSD/GBPUSD/AUDUSD engines.
        const double pnl = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;

        // Sanity check: clamp anomalous PnL. NZDUSD with size <= 0.20 and
        //   move <= MAX_RANGE should never produce |pnl_raw| > 0.0050 * 0.20
        //   = 0.0010. Cap at ~10x that for safety: 0.05 * size_ matches the
        //   EUR/GBP/AUD multiplier (same 0.0001 pip scale, same per-pip
        //   USD value).
        const double sane_max = std::max(0.01, size_) * 0.05;
        double pnl_to_emit = pnl;
        if (std::fabs(pnl) > sane_max) {
            const double recomputed = (is_long_ ? (exit_px - entry_) : (entry_ - exit_px)) * size_;
            std::ostringstream warn;
            warn << "[NZD-ASN-OPEN][SANITY] anomalous pnl=" << pnl
                 << " (size=" << size_ << " entry=" << entry_ << " exit=" << exit_px
                 << "). Recomputed=" << recomputed
                 << ". Emitting recomputed value.\n";
            std::cout << warn.str();
            std::cout.flush();
            pnl_to_emit = recomputed;
        }

        {
            std::ostringstream os;
            os << "[NZD-ASN-OPEN] EXIT " << (is_long_ ? "LONG" : "SHORT")
               << " @ " << std::fixed << std::setprecision(5) << exit_px
               << " reason=" << reason
               << " pnl_raw=" << std::setprecision(6) << pnl_to_emit
               << " mfe="    << std::setprecision(5) << mfe_
               << " mae="    << mae_
               << "\n";
            std::cout << os.str();
            std::cout.flush();
        }

        // S53/S56: same-level re-arm block stamps.
        //   SL_HIT -> 20-min block at entry price (rejected level).
        //   TRAIL_HIT or TP_HIT -> 10-min block at exit price (exhaustion).
        //   BE_HIT -> no stamp.
        if (reason == std::string("SL_HIT")) {
            m_sl_cooldown_dir = is_long_ ? 1 : -1;
            m_sl_cooldown_ts  = now_s + SAME_LEVEL_POST_SL_BLOCK_S;
            m_sl_price        = entry_;
        }
        if (reason == std::string("TRAIL_HIT") || reason == std::string("TP_HIT")) {
            m_win_exit_price    = exit_px;
            m_win_exit_block_ts = now_s + SAME_LEVEL_POST_WIN_BLOCK_S;
        }

        omega::TradeRecord tr;
        tr.id           = ++m_trade_id;
        tr.symbol       = "NZDUSD";
        tr.side         = is_long_ ? "LONG" : "SHORT";
        tr.engine       = "NzdusdAsianOpen";
        tr.regime       = "ASN_COMPRESSION";
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
