#pragma once
//  ADVERSE-PROTECTION: trail-by-design (backtested). In-flight protection = HARD
//    trailing stop (HARD_PCT / ATR-trail ATR_LEN*ATR_MULT) + MAXHOLD time-stop +
//    feed-independent watchdog. bigcap_exit_compare.cpp / bigcap_sweep.cpp showed a
//    WIDER ATR-trail beats a tighter cut (PF2.30->4.72, rides winners) -> a cold
//    loss-cut LOWERS net, deliberately NOT added (cf omega-bigcap-exit-givesback).
//    + equity-mkt price-bear long-block S-2026-06-24k (index_market_regime).
// ─────────────────────────────────────────────────────────────────────────────
// PumpScalpEngine — micro-cap "big-jump" momentum scalp, backtest-found.
//
//   Thesis: a US micro-cap that has ALREADY expanded violently intraday (a
//   "pump") keeps making tradeable continuation thrusts; and when it finally
//   exhausts at a fresh high-of-day it gives a clean fade. Trade the ignition
//   thrust long, the exhaustion top short, ride each with a HARD trailing stop,
//   and take NOTHING unless the name is a confirmed EXTREME mover.
//
//   ENTRY runs on this engine's timeframe (TF_SEC = 5/10/15 min — three separate
//   instances). EXIT runs on every price update (on_price) so a position bails
//   IMMEDIATELY when the move turns, regardless of the entry timeframe.
//
//   Steps (all objective, all causal):
//     0. DAY-EXPANSION GATE  = take NO trade until the running high is
//        >= DAY_GATE_PCT above the day's open. THE lever — separates the durable
//        edge (extreme movers) from the bleed (modest +50% names). Use ~100%.
//     1. IGNITION (long)   = close up >= IG_PCT over LB bars + volume surge
//        (v >= VOLX * 20-bar avg) + bar strength (close in top of range).
//     2. EXHAUSTION (short) = runup >= RUNUP_PCT, extended >= EXT_PCT above EMA9,
//        FRESH high-of-day (<= NEWHOD_M bars), bearish break. STRICT top-fade
//        ONLY — continuation shorts were tested and LOSE badly (whipsaw).
//     3. EXIT = HARD trailing stop off the peak/trough (TRAIL_PCT, default 3%,
//        checked every tick) or hard stop (HARD_PCT) or MAXHOLD time stop. NO
//        fixed take-profit — it amputates the fat-tail runners that ARE the edge
//        (Jun-09 trailing +1261% vs TP8 +60%). The trailing stop IS "get out when
//        profit is made": locks gains, exits on the turn, keeps the upside.
//        Trail-tightness sweep: tighter = better on every day (2% best; 3%
//        default leaves margin for thin-AH fill slippage).
//
//   PYRAMID (PYR_ADDS, default 0 = OFF): add a unit each time price advances
//     PYR_STEP% beyond the last add; the whole stack shares ONE trailing exit
//     (turns -> all units out at once). Tested: it's conditional LEVERAGE, not
//     edge — helps the extreme this-week days (Jun-09 +1306->+1640) but HURTS the
//     durable Apr-08 case (+27->+20, the add caught on the turn). Same lesson as
//     omega-pyramiding-result (gold). KEEP OFF for the durable regime.
//
//   BACKTEST (2026-06-10, IBKR 5m, memory pump-scalp-ah-momentum-edge):
//     5/10/15m all viable (1m is a SLIPPAGE TRAP). Survives 1-2%/side slip with
//     the gate. Multi-month OOS: edge PREDATES this week — INHD Apr-08 (+247%,
//     2mo prior) gate100 trail3 = PF 30. Scales with magnitude; this-week PF
//     17-270 is rule-amplified and will NOT persist. Durable expectation ~PF 2-30
//     on EXTREME movers. C++ port fidelity-matched to the Python harness ±0.3%.
//
//   CAVEATS (=> shadow_mode=true, NOT live-sized): AH fills modelled not measured;
//     short borrow/SSR/halt unmodellable; real-time dud-rate unmeasured. Shadow
//     on the next live pump first; measure fills + dud-rate before any live size.
//
//   FEED: trades a DYNAMIC universe (whatever explodes today), not a fixed symbol.
//   One instance PER active pump symbol, created by the manager. Driven by CLOSED
//   TF OHLCV bars (on_entry_bar) for entries + fast price (on_price) for exits.
// ─────────────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"
#include "RegimeState.hpp"   // S-2026-06-24: equity-mkt bear long-block (item-2 coverage)

namespace omega {

class PumpScalpEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol      = "";            // set per active pump symbol by the manager
    std::string engine_name = "PumpScalp";   // manager suffixes _3m/_5m/_15m

    // ── Config (backtest-found defaults) ─────────────────────────────────────
    int    TF_SEC       = 300;     // ENTRY timeframe: 300/600/900 = 5/10/15m (all validated)
    double DAY_GATE_PCT = 100.0;   // THE lever: no trade until run_high >= this% over day-open
    int    LB           = 3;       // ignition lookback bars
    double IG_PCT       = 3.0;     // ignition: % up over LB bars
    double VOLX         = 3.0;     // ignition: volume surge vs 20-bar avg
    double STRENGTH     = 0.60;    // ignition: close must sit in top (1-STRENGTH) of bar range
    double RUNUP_PCT    = 20.0;    // exhaustion: min runup from day-open
    double EXT_PCT      = 5.0;     // exhaustion: min % extension above EMA9
    int    NEWHOD_M     = 8;       // exhaustion: HOD must be this fresh (bars)
    double TRAIL_PCT    = 2.0;     // HARD trailing stop off peak/trough (checked every tick).
                                   // 3->2 2026-06-11: pump_exit_bt.py BE2T2 beats TRAIL3 on
                                   // EVERY basket day, both slip levels (net 1294->1342 @1%,
                                   // PF 26.7->35.1). Original sweep already said 2% best.
    double HARD_PCT     = 6.0;     // hard stop from entry
    // BE-lock (operator ask 2026-06-11, validated pump_exit_bt.py BE2T2): once
    // the move runs BE_ARM_PCT past entry, the stop floors at NET break-even
    // (BE_FLOOR_PCT ~ 2x per-side slip) — a pop that fades exits ~flat instead
    // of trailing to a negative. WR 55->67 at 2% slip, never hurts a runner
    // (floor only binds when the trail would exit BELOW it). 0 = disabled.
    double BE_ARM_PCT   = 2.0;     // arm once peak/trough is this % past entry
    double BE_FLOOR_PCT = 2.0;     // stop floor: entry +/- this % (net-BE at ~1%/side slip)
    int    MAXHOLD_SEC  = 30*300;  // time stop (default 30 x 5m bars worth of seconds)
    // When true, the MAXHOLD time-stop is SKIPPED for a position still in net profit
    // (beyond round-trip slip) — let winners ride to the trail/BE-lock turn instead of
    // a wall-clock cut. Default OFF (deployed behaviour unchanged). 2026-06-18: live
    // ledger showed the 240-min cap exiting QURE/NTLA/PRAX mid-run (caught the mover,
    // clocked out before the fade). Gate on faithful sweep before enable.
    bool   MAXHOLD_SKIP_IF_PROFIT = false;
    bool   ALLOW_SHORT  = true;    // strict exhaustion fade ONLY (continuation LOSES — never add)
    int    PYR_ADDS     = 0;       // pyramid adds onto a winner (0=OFF; leverage not edge)
    double PYR_STEP     = 8.0;     // pyramid: % advance beyond last add to trigger next unit
    bool   VOL_REG_FILTER = true;  // volume-regression filter: require VWAP + regression-slope agreement.
                                   // Backtest 2026-06-10 (06-08/06-09 + OOS Apr-08): cuts ~half the trades
                                   // (the false ones) -> avg/trade +30-67%, win% up, PF 2-14x, lower DD.
                                   // ~10-15% less total net (fewer trades) — quality over quantity.
    int    REG_LB       = 12;      // regression-slope lookback bars
    double SLOPE_MIN    = 0.0;     // min |slope| (%/bar) to allow a trade (0 = just sign agreement)
    double lot          = 1.0;
    // S-2026-06-11 honest-accounting upgrade (operator: "pennies per trade,
    // real costs higher"). Lifetime 5m shadow was n=46 net=+$1.72 — 1 SHARE
    // per trade with ZERO spread modeled = meaningless paper. Now:
    double NOTIONAL_USD = 1000.0;  // shares per trade = NOTIONAL_USD/entry_px (0 = legacy 1-lot)
    double SLIP_PCT     = 1.0;     // %/side haircut baked into recorded PnL — matches the
                                   // pump_*_bt.py backtest cost model (1% base, 2% stress).
                                   // Shadow PnL is now comparable to backtest + survives gate
                                   // honestly: an engine that can't beat 2% round-trip is dead.
    // S-2026-06-11 ANTI-SLIPPAGE liquidity gate (operator: "we cannot be caught
    // with the 5% issue"). On illiquid sub-$1 / thin names a $-order walks a
    // paper-thin book + reopens through LULD halts => 5%+/side slip => the month
    // sim flipped to -$185k@5k / -$37k@1k. liq_calib.py: requiring price>=$1 AND
    // bar $-volume>=$2M (our $1k order is then <0.05% of a bar's flow, fills near
    // the quote) RAISED net@2% to +$22.7k AND cut the @5% tail from -$37k to
    // -$7.4k. Only trade names deep enough that the 2% trail exits cleanly.
    double MIN_DVOL_USD = 0.0;     // entry requires close*bar_volume >= this ($ liquidity floor; 0=off)
    double PRICE_MIN    = 0.0;     // entry requires close >= this (skip ultra-thin sub-$X; 0=off)
    // S-2026-06-11 RE-ENTRY CAP: max entries per name per session. Live shadow
    // showed CHOW entered 4x (+50,-32,-38,-39 = re-entry chop bleed). Backtest
    // (16-day basket, deployed cfg, reentry_cap_bt.py): unlimited=57tr net$13.6k
    // PF18; cap1=10tr $1.7k PF9 (TOO TIGHT — monster continuations need a re-entry,
    // kills the edge); cap2=18tr $11.5k PF42 = keeps 84% of net, cuts the chop
    // bleed, best PF. 0 = unlimited (old leaky behaviour).
    int    MAX_ENTRIES_PER_DAY = 2;

    // ── EXIT-RESEARCH LEVERS (2026-06-18). ALL DEFAULT-OFF => on_price()/
    //   on_entry_bar() behave BYTE-IDENTICAL to the live engine. pump_exit_sweep
    //   .cpp toggles these to find a faster-reversal / more-runner-capture exit.
    //   Operator ask: "try every lever/setting until you find the best." Tested,
    //   not shipped — a winner must beat the live exit at BOTH slip levels AND
    //   both basket halves before any flag flips on the live engine.
    //   • ATR trail: trail distance = ATR_MULT * ATR(ATR_LEN bars) instead of a
    //     fixed %. Adapts to each name's volatility (a +400% name's 2% is noise).
    //     Set ATR_LEN>0 to enable; pair with TRAIL_PCT=0 to test ATR-only.
    int    ATR_LEN       = 0;
    double ATR_MULT      = 0.0;
    //   • Profit-scaled trail (S-2026-06-24, default-OFF): the ATR-trail multiplier
    //     ramps from ATR_MULT (wide, gives the ignition room) DOWN to ATR_MULT_TIGHT
    //     as the open gain grows from 0 to PSCALE_FULL_PCT. Rides small winners on a
    //     wide trail, protects the fat part of a runner with a tighter one — banks
    //     more on a reversal WITHOUT choking the normal post-breakout pullback.
    //     Off when ATR_MULT_TIGHT<=0 or PSCALE_FULL_PCT<=0 (flat ATR_MULT, live behavior).
    double ATR_MULT_TIGHT  = 0.0;   // trail mult once gain >= PSCALE_FULL_PCT (e.g. 2.5)
    double PSCALE_FULL_PCT = 0.0;   // gain% at which the trail reaches ATR_MULT_TIGHT (e.g. 15)
    //   • Structure exit: close on a bar that closes through the extreme of the
    //     last STRUCT_LB closed bars (long: below the min low). 0 = off.
    int    STRUCT_LB     = 0;
    //   • Rollover exit: close on a bar that closes back below VWAP / EMA9 (long).
    bool   ROLLOVER_VWAP = false;
    bool   ROLLOVER_EMA  = false;
    //   • Give-back exit: close once price retraces GIVEBACK_FRAC of the peak gain
    //     (MFE) back from the peak. 0 = off. Locks more of a runner on the turn.
    double GIVEBACK_FRAC = 0.0;
    //   • Give-back ON CLOSE (S-2026-06-24): same idea as GIVEBACK_FRAC but evaluated on a
    //     CLOSED 5m bar (in on_entry_bar) instead of every tick — so a noise down-wick can't
    //     trigger it (the tick-based GIVEBACK_FRAC fired on noise: WR2.5% in the sweep). Banks
    //     a runner once the bar CLOSES this far retraced from the peak gain. 0 = off. This is
    //     the "bank the reversal without exiting on every down price" lever for the A/B.
    double GIVEBACK_CLOSE_FRAC = 0.0;
    //   • COLD-CUT (S-2026-06-24): cut a trade that has NEVER gone meaningfully green
    //     (peak favourable < COLD_CUT_GREEN_PCT) after COLD_CUT_SEC seconds. Targets the
    //     "bought it, went straight adverse, rode the wide stop down for hours" losers
    //     (IONQ -$58/160m, HUN -$43/240m, NOK -$29/210m never green) WITHOUT touching
    //     winners (they go green -> peak exceeds the threshold -> exempt). A BLUNT loss-
    //     cut (tighter HARD/MAXHOLD) lowers net + RAISES maxDD (dip-then-recover trades
    //     become realised losses); this gates on never-green so it can't do that.
    //     0 = off (live behavior). Evaluated on a CLOSED bar (noise-proof).
    double COLD_CUT_SEC       = 0.0;   // cut after this many seconds if still never-green
    double COLD_CUT_GREEN_PCT = 0.5;   // "green" = peak favourable >= this % of entry

    // ── ENTRY-RESEARCH lever (2026-06-18, default-OFF). Anti-late-chase: skip an
    //   ignition long whose close is already > this % above VWAP (we'd be buying
    //   the top of a thrust that already ran). The exit sweep proved no exit fixes
    //   the bleed; the leak is entering late/extended. 0 = off (live behavior).
    double ENTRY_MAX_EXT_PCT = 0.0;

    bool   enabled      = true;
    bool   shadow_mode  = true;    // gated shadow — AH fills + borrow + dud-rate all unmeasured
    bool   verbose      = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // S-2026-06-11 concurrency cap: manager-injected entry permit. The 5/10/15m
    // trio used to take the SAME thrust 3x (HCAI: 3 simultaneous positions =
    // 3x concentration + triple-counted shadow stats). Returns false when a
    // sibling TF engine already holds this symbol -> entry suppressed.
    std::function<bool()> entry_permit;

    // ── cross-sectional BREADTH gate (S-2026-06-23): called when THIS symbol meets
    // the gate+ignition entry condition. Registers the ignition with the manager's
    // shared per-day counter and returns true iff >= min_breadth DISTINCT symbols
    // have ignited today (causal: only same-day prior info). Null = no gate (live).
    // Fixes the chop-bleed (isolated single-name false breakouts are skipped) and
    // is self-protecting in a bear (few broad-ignition days -> engine sits out).
    std::function<bool(int64_t /*session_day*/, const std::string& /*sym*/)> breadth_register;

    // ── Position: a stack of units sharing ONE trailing exit ─────────────────
    struct Position { bool active=false; int dir=0; double size_each=0;
                      std::vector<double> units;          // entry px per unit (1 base + adds)
                      double peak=0, trough=0, last_add=0;
                      int64_t entry_ms=0; double mfe=0, mae=0; } pos;

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active || pos.units.empty()) return false;
        o.engine=eng; o.symbol=sym; o.side = pos.dir>0 ? "LONG" : "SHORT";
        o.size=pos.size_each*(double)pos.units.size(); o.entry=pos.units.front();
        o.sl = pos.dir>0 ? pos.units.front()*(1-HARD_PCT/100) : pos.units.front()*(1+HARD_PCT/100);
        o.tp=0; o.entry_ts=pos.entry_ms/1000;
        // Real standing cost (2026-06-18): mark at last known price, costs in —
        // NOT 0. A stale-feed position now shows its true unrealized loss/gain.
        o.current = mark_px();
        o.unrealized_pnl = mark_pnl(o.current);
        o.mfe = pos.mfe*pos.size_each; o.mae = std::fabs(pos.mae)*pos.size_each;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos = Position{}; pos.active=true; pos.dir = (ps.side=="SHORT") ? -1 : 1;
        pos.units = {ps.entry}; pos.size_each=ps.size; pos.peak=ps.entry; pos.trough=ps.entry;
        pos.last_add=ps.entry; pos.entry_ms=ps.entry_ts*1000;
        return true;
    }
    bool has_open_position() const { return pos.active; }

    // Mark-to-market PnL of the open stack at `px`, costs included — IDENTICAL to
    // what _close() books at exit_px=px. Used for the live unrealized mark so a
    // frozen-feed position shows its REAL standing cost instead of a misleading
    // +$0 (2026-06-18: dead feed marked current=0 -> GUI hid the true loss).
    double mark_pnl(double px) const {
        if (!pos.active || pos.units.empty() || px <= 0) return 0.0;
        const double s = SLIP_PCT/100.0; double p = 0;
        for (double u : pos.units) p += (pos.dir>0 ? (px-u) : (u-px)) - (u+px)*s;
        return p * pos.size_each;
    }
    // Best available mark price: last live tick, else entry (never 0 -> the GUI
    // never shows a $0.00 price / fake +$0 PnL on a stale-feed position).
    double mark_px() const { return m_last_px > 0 ? m_last_px : (pos.units.empty()?0.0:pos.units.front()); }

    void init() { _new_day(-1); pos = Position{}; }

    // Clean re-warm before a seed REPLAY (bridge 'R' line): wipe all bar/day
    // state so the incoming seed batch never double-counts bars/EMA/VWAP, but
    // KEEP any open position — its management continues on live ticks.
    void reset_for_reseed() { _new_day(m_day); }

    // Health introspection (cold-anchor detection, 2026-06-10 failure class):
    // the engine's own view of today's expansion + bar warmup depth.
    double day_up_pct() const {
        return (m_day_open > 0 && m_run_high > 0) ? (m_run_high/m_day_open - 1.0)*100.0 : -1.0;
    }
    int bars_seen() const { return (int)m_bars.size(); }

    // ── FAST path: every price update. Manages the open position so it exits
    //   IMMEDIATELY on the turn (all three TF engines share this behaviour). ──
    // Profit-scaled ATR-trail multiplier: ramps ATR_MULT (wide) -> ATR_MULT_TIGHT as the
    // open gain grows 0 -> PSCALE_FULL_PCT. Returns flat ATR_MULT when the scaling is off.
    double _eff_atr_mult(double base, double extreme) const {
        if (ATR_MULT_TIGHT <= 0.0 || PSCALE_FULL_PCT <= 0.0 || base <= 0.0) return ATR_MULT;
        const double gain_pct = std::fabs(extreme - base) / base * 100.0;   // peak gain (long) / trough (short)
        double f = gain_pct / PSCALE_FULL_PCT; if (f < 0) f = 0; if (f > 1) f = 1;
        return ATR_MULT + (ATR_MULT_TIGHT - ATR_MULT) * f;                   // wide -> tight
    }
    void on_price(double px, int64_t ts_ms) {
        if (px <= 0) return;
        m_last_px = px; m_last_px_ms = ts_ms;          // watchdog: last good tick (feed-stale force-close)
        const int64_t day = _session_day(ts_ms);
        if (day != m_day) _new_day(day);
        if (m_day_open <= 0) m_day_open = px;          // fallback only; bars set the true session open
        m_run_high = std::max(m_run_high, px);
        if (!pos.active) return;

        const double base = pos.units.front();
        const double fav = pos.dir>0 ? (px-base) : (base-px);
        if (fav>pos.mfe) pos.mfe=fav; if (fav<pos.mae) pos.mae=fav;

        if (pos.dir>0) {
            pos.peak = std::max(pos.peak, px);
            if (PYR_ADDS>0 && (int)pos.units.size()<1+PYR_ADDS && px >= pos.last_add*(1+PYR_STEP/100)) {
                pos.units.push_back(px); pos.last_add=px;
            }
            double stop = base*(1-HARD_PCT/100);                                    // hard floor (always)
            if (TRAIL_PCT > 0)        stop = std::max(stop, pos.peak*(1-TRAIL_PCT/100));   // % trail
            if (ATR_LEN > 0 && m_atr>0) stop = std::max(stop, pos.peak - _eff_atr_mult(base, pos.peak)*m_atr);  // ATR trail (profit-scaled)
            if (BE_ARM_PCT > 0 && pos.peak >= base*(1+BE_ARM_PCT/100))
                stop = std::max(stop, base*(1+BE_FLOOR_PCT/100));                   // BE-lock armed
            if (px <= stop) { _close(stop, ts_ms, "TRAIL"); return; }
            if (GIVEBACK_FRAC > 0 && pos.peak > base &&
                (pos.peak - px) >= GIVEBACK_FRAC*(pos.peak - base)) { _close(px, ts_ms, "GIVEBACK"); return; }
        } else {
            pos.trough = std::min(pos.trough, px);
            if (PYR_ADDS>0 && (int)pos.units.size()<1+PYR_ADDS && px <= pos.last_add*(1-PYR_STEP/100)) {
                pos.units.push_back(px); pos.last_add=px;
            }
            double stop = base*(1+HARD_PCT/100);                                    // hard ceiling (always)
            if (TRAIL_PCT > 0)        stop = std::min(stop, pos.trough*(1+TRAIL_PCT/100));
            if (ATR_LEN > 0 && m_atr>0) stop = std::min(stop, pos.trough + _eff_atr_mult(base, pos.trough)*m_atr);
            if (BE_ARM_PCT > 0 && pos.trough <= base*(1-BE_ARM_PCT/100))
                stop = std::min(stop, base*(1-BE_FLOOR_PCT/100));                   // BE-lock armed
            if (px >= stop) { _close(stop, ts_ms, "TRAIL"); return; }
            if (GIVEBACK_FRAC > 0 && pos.trough < base &&
                (px - pos.trough) >= GIVEBACK_FRAC*(base - pos.trough)) { _close(px, ts_ms, "GIVEBACK"); return; }
        }
        if (ts_ms - pos.entry_ms >= (int64_t)MAXHOLD_SEC*1000) {
            if (MAXHOLD_SKIP_IF_PROFIT) {
                // skip the clock-cut while the trade is still net-profitable — let the
                // trail / BE-lock exit it on the turn (ride until it fades).
                const double net_be = 2.0 * SLIP_PCT / 100.0;   // round-trip slip as a fraction
                const bool in_profit = pos.dir>0 ? (px > base*(1+net_be))
                                                 : (px < base*(1-net_be));
                if (!in_profit) _close(px, ts_ms, "TIME");
            } else {
                _close(px, ts_ms, "TIME");
            }
        }
    }

    // ── ENTRY path: one CLOSED TF OHLCV bar (with volume).
    //   is_seed=true => warm history/EMA/day-open/gate state ONLY, fire NO entries
    //   (warm-seed mandate: historical bars must never open a phantom trade). ──
    void on_entry_bar(double o, double h, double l, double c, double v, int64_t ts_ms, bool is_seed=false) {
        if (h < l || c <= 0) return;
        const int64_t day = _session_day(ts_ms);
        if (day != m_day) _new_day(day);
        if (m_day_open <= 0) m_day_open = o;             // session open = first bar's open
        m_run_high = std::max(m_run_high, h);
        const int idx = (int)m_bars.size();
        if (h > m_hod) { m_hod=h; m_hod_idx=idx; }
        const double k = 2.0/(9.0+1.0);
        m_ema9 = m_ema_init ? (c-m_ema9)*k + m_ema9 : c;
        m_ema_init = true;
        const Bar prev = m_bars.empty() ? Bar{o,h,l,c,v} : m_bars.back();
        m_bars.push_back({o,h,l,c,v});
        if (m_bars.size() > 64) m_bars.pop_front();
        m_cum_pv += (h+l+c)/3.0 * v; m_cum_v += v;        // intraday VWAP accumulation
        if (ATR_LEN > 0) {                                // research: ATR for the ATR-trail exit
            double tr_sum=0; int cnt=0;
            for (int i=(int)m_bars.size()-1; i>=1 && cnt<ATR_LEN; --i,++cnt) {
                const Bar& cu=m_bars[i]; const Bar& pr=m_bars[i-1];
                tr_sum += std::max(cu.h-cu.l, std::max(std::fabs(cu.h-pr.c), std::fabs(cu.l-pr.c)));
            }
            m_atr = cnt>0 ? tr_sum/cnt : 0;
        }

        // ── BAR-CLOSE EXITS (research, default-off): reversal cuts evaluated on a
        //   CLOSED bar — faster than waiting for a %/ATR give-back from the peak.
        //   Skipped on seed bars and when no position is open. ───────────────────
        if (!is_seed && pos.active) {
            bool ex=false; const char* r="";
            if      (ROLLOVER_VWAP && _vwap()>0 && (pos.dir>0 ? c < _vwap() : c > _vwap())) { ex=true; r="ROLL_VWAP"; }
            else if (ROLLOVER_EMA  && m_ema_init && (pos.dir>0 ? c < m_ema9 : c > m_ema9))  { ex=true; r="ROLL_EMA"; }
            else if (STRUCT_LB > 0) {
                const double sw = _swing_extreme(STRUCT_LB, pos.dir>0);
                if (sw>0 && (pos.dir>0 ? c < sw : c > sw)) { ex=true; r="STRUCT"; }
            }
            if (!ex && GIVEBACK_CLOSE_FRAC > 0) {            // close-based give-back (bank the reversal)
                const double bse = pos.units.front();
                const double pk  = pos.dir>0 ? pos.peak : pos.trough;
                const double gain= pos.dir>0 ? (pk-bse) : (bse-pk);
                const double retr= pos.dir>0 ? (pk-c)   : (c-pk);
                if (gain>0 && retr >= GIVEBACK_CLOSE_FRAC*gain) { ex=true; r="GB_CLOSE"; }
            }
            if (!ex && COLD_CUT_SEC > 0 && (ts_ms - pos.entry_ms) >= (int64_t)(COLD_CUT_SEC*1000)) {
                const double bse = pos.units.front();          // cold-cut: never-green loser, cut small
                if (pos.mfe < bse * COLD_CUT_GREEN_PCT/100.0) { ex=true; r="COLD_CUT"; }
            }
            if (ex) { _close(c, ts_ms, r); return; }
        }

        if (is_seed) return;                             // SEED: warm only — never enter on history
        if (pos.active || !enabled) return;              // exits are on_price's job
        if (MAX_ENTRIES_PER_DAY > 0 && m_entries_today >= MAX_ENTRIES_PER_DAY) return;  // re-entry cap (chop-bleed guard)
        if (entry_permit && !entry_permit()) return;     // sibling TF holds this symbol (S-2026-06-11)
        if (m_day_open<=0 || (m_run_high/m_day_open - 1.0)*100.0 < DAY_GATE_PCT) return;  // gate
        if (idx < LB + 21) return;
        if (PRICE_MIN > 0 && c < PRICE_MIN) return;            // anti-slippage: skip sub-$X thin names
        if (MIN_DVOL_USD > 0 && c*v < MIN_DVOL_USD) return;    // anti-slippage: bar $-volume floor

        // volume-regression filter: VWAP + slope must agree with the trade direction
        const double vwap = _vwap(), slope = _slope();
        const bool long_ok  = (!VOL_REG_FILTER) || (vwap<=0) || (c > vwap && slope >=  SLOPE_MIN);
        const bool short_ok = (!VOL_REG_FILTER) || (vwap<=0) || (c < vwap && slope <= -SLOPE_MIN);

        double avgv=0; int n=0;
        for (int i=(int)m_bars.size()-2; i>=0 && n<20; --i,++n) avgv += m_bars[i].v;
        avgv = n>0 ? avgv/n : 1.0; if (avgv<=0) avgv=1.0;

        // S-2026-06-24 universal price-bear long-block (item-2 bear coverage).
        // BigCapMomo is a bull-beta equity-momentum engine that self-enters (its own
        // scanner/bridge path, NOT enter_directional) -- so it needs its own bear
        // gate. Block NEW longs in a confirmed EQUITY-market bear/macro-hostile regime
        // (index_market_regime = NAS-MKT price-brain, same regime the universal
        // enter_directional gate uses for non-gold). Manage/exit (on_price/watchdog)
        // and the short side below are untouched. Fail-open while the brain is cold.
        const bool equity_bear_block = omega::index_market_regime().long_blocked();

        // IGNITION long
        const double c_lb = m_bars[(int)m_bars.size()-1-LB].c;
        const bool strong = (h>l) ? (c >= l + STRENGTH*(h-l)) : true;
        const bool not_extended = (ENTRY_MAX_EXT_PCT<=0) || (vwap<=0) || ((c/vwap - 1.0)*100.0 <= ENTRY_MAX_EXT_PCT);
        if (!equity_bear_block && long_ok && not_extended && (c/c_lb - 1.0)*100.0 >= IG_PCT && v >= VOLX*avgv && strong) {
            if (breadth_register && !breadth_register(_session_day(ts_ms), symbol)) return;  // chop/bear gate: need >=min_breadth names igniting today
            _open(+1, c, ts_ms); return;
        }

        // EXHAUSTION short (strict top-fade only)
        const double runup = (c/m_day_open - 1.0)*100.0;
        const double ext   = (c/m_ema9   - 1.0)*100.0;
        const bool new_hod = (idx - m_hod_idx) <= NEWHOD_M;
        const bool bear_brk = (c < o) && (c < prev.l);
        if (short_ok && ALLOW_SHORT && runup>=RUNUP_PCT && ext>=EXT_PCT && new_hod && bear_brk) { _open(-1, c, ts_ms); }
    }

    void force_close(double px, int64_t now_ms) { if (pos.active) _close(px, now_ms, "FORCE_CLOSE"); }

    // ── WATCHDOG: feed-INDEPENDENT exit. Driven by the main-loop heartbeat, NOT
    //   the price feed. ALL the tick exits (trail / hard / MAXHOLD) live inside
    //   on_price(), so when the pump feed goes stale — after-hours, halt, bridge
    //   or IBKR drop — on_price() stops firing and a position hangs open forever
    //   (2026-06-18: 7 names held 200+min, GUI now=0, +$0; the 15-min MAXHOLD
    //   never fired because it too was tick-gated). This closes such a position
    //   at the last known price:
    //     • MAXHOLD elapsed (wall clock) — the time-stop the engine ALWAYS meant
    //       to enforce, now independent of whether a tick arrived.
    //     • feed stale >= stale_ms with no new tick — the feed is dead, get flat.
    //   No-op when a live tick is arriving (on_price keeps m_last_px_ms fresh).
    void watchdog(int64_t now_ms, int64_t stale_ms) {
        if (!pos.active || m_last_px <= 0) return;
        if ((now_ms - pos.entry_ms) >= (int64_t)MAXHOLD_SEC*1000) { _close(m_last_px, now_ms, "TIME_WD"); return; }
        if (stale_ms > 0 && (now_ms - m_last_px_ms) >= stale_ms)  { _close(m_last_px, now_ms, "STALE_WD"); return; }
    }

private:
    struct Bar { double o,h,l,c,v; };

    void _new_day(int64_t day) {                          // single source of session reset
        m_day=day; m_day_open=0; m_run_high=0; m_hod=-1e18; m_hod_idx=-1;
        m_bars.clear(); m_ema9=0; m_ema_init=false;
        m_cum_pv=0; m_cum_v=0; m_entries_today=0; m_atr=0;  // reset re-entry cap + ATR each session
    }

    // US-session day, rolled at 08:00 UTC (4am EDT / 3am EST — always inside the
    // overnight dead zone, never mid-session). Plain UTC midnight = 8pm ET =
    // MID-after-hours: names tracked through it re-anchored m_day_open at the AH
    // trough, inflating day_up_pct -> the +100% gate passed on +8-60% names
    // (CHSN/CDTG/HCAI, 2026-06-11). Matches the bridge's "1 D" premarket-anchored
    // seed window = the backtest's day-expansion baseline.
    static int64_t _session_day(int64_t ts_ms) { return (ts_ms/1000 - 8*3600)/86400; }

    double _vwap() const { return m_cum_v>0 ? m_cum_pv/m_cum_v : 0.0; }

    // Min low (long) / max high (short) over the `lb` CLOSED bars BEFORE the
    // current one — the swing the current close would break (structure exit).
    double _swing_extreme(int lb, bool isLong) const {
        const int n=(int)m_bars.size();
        if (n < 2) return 0.0;
        double ext = isLong ? 1e18 : -1e18; int cnt=0;
        for (int i=n-2; i>=0 && cnt<lb; --i,++cnt)
            ext = isLong ? std::min(ext, m_bars[i].l) : std::max(ext, m_bars[i].h);
        return cnt>0 ? ext : 0.0;
    }

    // least-squares slope of the last REG_LB closes, normalized to %/bar
    double _slope() const {
        const int n = std::min((int)m_bars.size(), REG_LB);
        if (n < 3) return 0.0;
        const int base = (int)m_bars.size() - n;
        double sx=0,sy=0,sxx=0,sxy=0;
        for (int k=0;k<n;++k){ double x=k, y=m_bars[base+k].c; sx+=x; sy+=y; sxx+=x*x; sxy+=x*y; }
        const double den = n*sxx - sx*sx;
        if (std::fabs(den) < 1e-9) return 0.0;
        const double b = (n*sxy - sx*sy)/den;
        const double mean = sy/n;
        return mean>0 ? b/mean*100.0 : 0.0;
    }

    void _open(int dir, double px, int64_t ts_ms) {
        pos = Position{}; pos.active=true; pos.dir=dir;
        pos.size_each = (NOTIONAL_USD > 0 && px > 0) ? NOTIONAL_USD/px : lot;
        pos.units = {px}; pos.peak=px; pos.trough=px; pos.last_add=px; pos.entry_ms=ts_ms;
        ++m_entries_today;                                // re-entry cap: count this session entry
        if (verbose) printf("[%s] %s %s entry=%.4f gate=%.0f%%\n", engine_name.c_str(),
            symbol.c_str(), dir>0?"LONG":"SHORT", px, (m_run_high/m_day_open-1)*100);
    }

    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active || pos.units.empty()) return;
        double pnl_px = 0;                                            // sum over the unit stack
        const double s = SLIP_PCT/100.0;                              // %/side cost haircut
        for (double u : pos.units)
            pnl_px += (pos.dir>0 ? (exit_px-u) : (u-exit_px)) - (u + exit_px)*s;
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.engine=engine_name; tr.side = pos.dir>0?"LONG":"SHORT";
        tr.entryPrice=pos.units.front(); tr.exitPrice=exit_px;
        tr.sl = pos.dir>0 ? pos.units.front()*(1-HARD_PCT/100) : pos.units.front()*(1+HARD_PCT/100);
        tr.tp=0; tr.size=pos.size_each*(double)pos.units.size();
        tr.pnl=pnl_px*pos.size_each;                                  // RAW pts*lot — ledger multiplies
        tr.mfe=pos.mfe*pos.size_each; tr.mae=std::fabs(pos.mae)*pos.size_each;
        tr.entryTs=pos.entry_ms/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason; tr.shadow=shadow_mode;
        if (on_trade_record) on_trade_record(tr);
        if (verbose) printf("[%s] %s CLOSE %s exit=%.4f pnl=%.4f units=%zu\n",
            engine_name.c_str(), symbol.c_str(), reason, exit_px, pnl_px, pos.units.size());
        pos = Position{};
    }

    std::deque<Bar> m_bars;
    int64_t m_day=-1; double m_day_open=0, m_run_high=0, m_hod=-1e18; int m_hod_idx=-1;
    int m_entries_today=0;                               // re-entry cap counter (reset in _new_day)
    double  m_ema9=0; bool m_ema_init=false;
    double  m_cum_pv=0, m_cum_v=0;   // intraday VWAP accumulators
    double  m_atr=0;                 // research: ATR(ATR_LEN) for the ATR-trail exit (0 when ATR_LEN=0)
    double  m_last_px=0; int64_t m_last_px_ms=0;   // watchdog: last tick seen (feed-stale force-close)
};

}  // namespace omega
