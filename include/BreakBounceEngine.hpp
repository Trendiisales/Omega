#pragma once
// =============================================================================
//  BreakBounceEngine.hpp -- Break-and-Retest trend engine (2026-05-31)
// =============================================================================
//
//  PROVENANCE
//
//  Ported from the operator's MT5 "BreakBounce" EA (BreakBounceScalper.mq5)
//  into Omega's self-managing on_tick() engine pattern (cf.
//  Ger40LondonBreakoutEngine). The EA logic:
//
//    1. BIAS  (slow TF, default D1): EMA(fast) vs EMA(slow) + close alignment.
//         bias = +1 if  close > emaFast > emaSlow
//         bias = -1 if  close < emaFast < emaSlow
//    2. BREAK (mid TF, default M15): on a CLOSED bar, price closes beyond the
//         prior `lookback`-bar range by BreakBufferATR*ATR with a body
//         >= MinBreakBodyATR*ATR, in the direction of bias  ->  ARM.
//    3. RETEST(fast TF, default M5): while armed (and not expired), a CLOSED
//         bar pulls back into the broken level (+/- RetestZoneATR*ATR) and
//         closes back through it (bounce)  ->  ENTER market.
//    4. EXIT: ATR stop (min of bounce-low-0.25*ATR and entry-StopATR*ATR),
//         TP at RewardRisk * risk. R-based break-even + ATR chandelier trail.
//
//  TWO DELIBERATE DEVIATIONS FROM THE EA (both correctness fixes):
//
//    A. LOOKAHEAD-SAFE. The EA's offline C++ port evaluated the *forming*
//       mid-TF bar (whose OHLC was already complete in the pre-built array),
//       i.e. it read ~15 min into the future. This engine only ever acts on
//       a bar AFTER it has CLOSED (the next tick rolls the bucket), so there
//       is no lookahead by construction.
//
//    B. R-BASED MANAGEMENT, not fixed "points". The EA used fixed point
//       offsets for break-even / trailing (BreakEvenStartPoints=120, etc.).
//       On XAUUSD (point=0.001, price ~4700, avg spread ~0.48) those equal
//       ~$0.12 -- smaller than the spread, so every trade insta-locks. This
//       engine expresses BE/trail as multiples of the trade's own risk (R)
//       and ATR, which is robust across instrument, price level, and TF.
//
//  ARCHITECTURE
//      Self-managing engine. Does NOT emit CrossSignal. Aggregates its three
//      timeframes online from the tick stream, keeps incremental EMA + Wilder
//      ATR, and runs the arm/retest state machine on closed bars only. Fires
//      TradeRecord + on_close callbacks exactly like Ger40LondonBreakoutEngine.
//
//      The three timeframes are configurable in SECONDS so the same engine can
//      be run as D1/M15/M5 (scalp) or D1/H1/M15 / W1/H4/H1 (swing) -- used by
//      the backtest sweep to choose the timeframe that actually has edge on
//      gold before the engine is wired live.
//
//  SAFETY
//      shadow_mode = true by default; promotion requires operator auth.
//      Slow-TF EMA(slow) needs warm-up (e.g. EMA200 on D1 = ~200 days). For
//      live deploy, warm-seed the bias TF (see Engine Warm-Seed Mandate).
//
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"

namespace omega {

class BreakBounceEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol = "XAUUSD";
    std::string engine_name = "BreakBounce";

    // ── Timeframes (seconds). ────────────────────────────────────────────────
    // DEFAULT = the gold-VALIDATED config, NOT the EA's native D1/M15/M5 scalp.
    // The M5 scalp loses on XAUUSD (PF 0.86); structure on H1 with an M20
    // retest is the edge (2yr IS/OOS sweep 2026-05-31: IS PF 1.86 -> OOS PF
    // 1.54, WR 55%, DD 46pts, n=56 OOS). Override per-symbol if needed.
    int64_t BIAS_TF_SEC   = 86400;  // D1 trend bias
    int64_t BREAK_TF_SEC  = 3600;   // H1 breakout structure
    int64_t RETEST_TF_SEC = 1200;   // M20 retest entry

    // ── Indicators ───────────────────────────────────────────────────────────
    int    FAST_EMA   = 50;    // on BIAS_TF
    int    SLOW_EMA   = 200;   // on BIAS_TF
    int    ATR_PERIOD = 14;    // Wilder, on BREAK_TF and RETEST_TF
    int    LOOKBACK   = 32;    // BREAK_TF bars defining the range

    // ── Entry geometry (ATR multiples) ───────────────────────────────────────
    double BREAK_BUFFER_ATR        = 0.10;
    double RETEST_ZONE_ATR         = 0.25;
    double BOUNCE_CLOSE_BUFFER_ATR = 0.02;
    double MIN_BREAK_BODY_ATR      = 0.30;
    int    BREAK_VALID_BARS        = 12;   // arm lifetime, in RETEST_TF bars

    // ── Exit geometry ────────────────────────────────────────────────────────
    double STOP_ATR    = 1.20;   // initial stop = STOP_ATR * ATR(retest)
    double REWARD_RISK = 1.50;   // TP = entry +/- REWARD_RISK * risk
    int    MAX_HOLD_SEC = 0;     // 0 = no time stop

    // ── Management (R / ATR based -- NOT fixed points) ───────────────────────
    bool   USE_BREAKEVEN = true;
    double BE_ARM_R      = 1.0;   // arm BE once open profit >= BE_ARM_R * risk
    double BE_LOCK_R     = 0.10;  // lock stop at entry + BE_LOCK_R * risk

    bool   USE_TRAIL     = true;
    double TRAIL_START_R = 1.5;   // start trailing once profit >= TRAIL_START_R*R
    double TRAIL_ATR     = 2.0;   // chandelier distance = TRAIL_ATR * ATR(retest)

    // ── Regime guard (bear/chop protection) ──────────────────────────────────
    // ADX(14) on BREAK_TF (Wilder). Only ARM a breakout when trend strength is
    // adequate -- ADX is low in chop/consolidation, exactly where a
    // break-and-retest whipsaws. 0 = off. Set from the IS/OOS regime sweep, and
    // only if the sweep shows it is NOT subtractive (the ER chop-gate dead-end
    // on the gold trend book cut winners -- ADX is validated separately here,
    // not assumed). The D1 EMA bias already handles DIRECTION; this handles the
    // trend-vs-chop axis the EMA stack does not.
    double REGIME_ADX_MIN = 0.0;

    // ── L2 profit-protect (OFF by default) ───────────────────────────────────
    // Lock gains when live order-book flow turns hostile AND price gives back
    // from its peak. Designed to NOT sacrifice the edge:
    //   * armed only once the trade is >= L2_ARM_R * risk in profit (the
    //     load-bearing guard -- below 1R it does nothing, so losers and small
    //     winners keep the validated ATR-stop geometry untouched);
    //   * can only TIGHTEN the stop toward profit, never widen, never exit at
    //     a loss -> it cannot turn a winner into a loser;
    //   * requires BOTH hostile imbalance AND a real give-back from peak (not
    //     a single noisy print);
    //   * snaps the stop to a tight chandelier (lock, not market-dump) so a
    //     continued move can still run; only a real reversal takes you out.
    // CANNOT be backtested on the 2yr tick file (top-of-book only, no depth) --
    // validate via the live L2 replay framework before enabling.
    bool   USE_L2_PROTECT  = false;
    double L2_ARM_R        = 1.0;   // only active after profit >= L2_ARM_R * risk
    double L2_HOSTILE_IMB  = 0.35;  // LONG: g_l2 imbalance <= this (ask-heavy); SHORT mirrored
    double L2_GIVEBACK_ATR = 0.40;  // adverse move from peak >= this*ATR = "sudden drop"
    double L2_LOCK_ATR     = 0.50;  // snap stop to peak -/+ L2_LOCK_ATR*ATR

    // ── L2 capture (shadow data collection for the replay A/B) ───────────────
    // While a position is open, emit a throttled (entry_ms, now, bid/ask, imb,
    // fav, sl, risk, atr) sample via on_l2_sample. Lets us accumulate the real
    // order-book stream around live shadow trades, then replay the protect on
    // vs off offline (bb_l2_replay) once enough data lands. Independent of
    // USE_L2_PROTECT -- you capture first, validate, THEN enable the protect.
    bool    USE_L2_CAPTURE = false;
    int64_t L2_CAPTURE_SEC = 5;

    // ── Filters / sizing ─────────────────────────────────────────────────────
    bool   USE_SESSION    = true;
    int    SESSION_START_H = 7;   // UTC inclusive
    int    SESSION_END_H   = 18;  // UTC exclusive
    double MAX_SPREAD      = 0.60; // price units (XAU avg ~0.48)
    double lot             = 1.0;  // size multiplier for pnl reporting
    int64_t MIN_SEC_BETWEEN = 300; // cooldown from last entry

    // ── Engine control ───────────────────────────────────────────────────────
    bool enabled     = true;
    bool shadow_mode = true;
    bool verbose     = false;

    // ── Callbacks ────────────────────────────────────────────────────────────
    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // Throttled order-book sample emitted while a position is open (for L2
    // capture). entry_ms keys the trade; the rest is the live snapshot.
    using L2SampleCallback = std::function<void(
        int64_t entry_ms, int64_t now_ms, double bid, double ask, double imb,
        double fav, double sl, double risk, double atr, double adx, bool is_long)>;
    L2SampleCallback on_l2_sample;

    // ── Position ─────────────────────────────────────────────────────────────
    struct Position {
        bool   active   = false;
        bool   is_long  = false;
        double entry_px = 0.0;
        double sl_px    = 0.0;
        double tp_px    = 0.0;
        double risk     = 0.0;   // price units, entry->initial stop
        double atr      = 0.0;   // ATR(retest) at entry (for chandelier)
        double size     = 0.0;
        int64_t entry_ms = 0;
        double mfe      = 0.0;
        double mae      = 0.0;
        double spread_at_entry = 0.0;
        bool   be_armed = false;
        double peak_px  = 0.0;   // best favourable price seen (for L2 give-back)
        double adx_entry = 0.0;  // ADX(BREAK_TF) at entry (regime tag for capture)
    } pos;

    bool has_open_position() const { return pos.active; }

    // Live L2 order-book imbalance feed (bid_size/(bid_size+ask_size), 0.5
    // neutral). Host pushes this each tick from g_l2_<sym>.imbalance. Only
    // consulted when USE_L2_PROTECT is on.
    void set_l2_imbalance(double imb) { m_l2_imb = imb; }

    // ── Lifecycle ────────────────────────────────────────────────────────────
    void init() {
        m_bias = Ema{}; m_bias.fast_n = FAST_EMA; m_bias.slow_n = SLOW_EMA;
        m_atr_break = Atr{}; m_atr_break.period = ATR_PERIOD;
        m_atr_retest = Atr{}; m_atr_retest.period = ATR_PERIOD;
        m_adx = Adx{}; m_adx.period = ATR_PERIOD;
        m_agg_bias = Agg{}; m_agg_break = Agg{}; m_agg_retest = Agg{};
        m_range.clear();
        m_bias_dir = 0; m_bias_ready = false;
        m_arm_dir = 0; m_arm_level = 0.0; m_arm_expire_ms = 0;
        m_last_entry_ms = 0;
        pos = Position{};
    }

    // ── Main tick handler ────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0 || ask < bid) return;
        const double mid = (bid + ask) * 0.5;

        // 1) Manage any open position first (every tick, for stop/BE/trail).
        if (pos.active) _manage(bid, ask, now_ms);

        // 2) Roll the three timeframe aggregators; act on CLOSED bars only.
        Bar cb;
        if (m_agg_bias.update(mid, mid, now_ms, BIAS_TF_SEC, cb))     _on_bias_close(cb);
        if (m_agg_break.update(mid, mid, now_ms, BREAK_TF_SEC, cb))   _on_break_close(cb);
        if (m_agg_retest.update(mid, mid, now_ms, RETEST_TF_SEC, cb)) _on_retest_close(cb, bid, ask, now_ms);
    }

    void force_close(double bid, double ask, int64_t now_ms) {
        if (!pos.active) return;
        _close(pos.is_long ? bid : ask, now_ms, "FORCE_CLOSE");
    }

private:
    // ── Online bar aggregator ────────────────────────────────────────────────
    struct Bar { int64_t ts=0; double o=0,h=0,l=0,c=0; };
    struct Agg {
        int64_t bucket = -1;
        double o=0,h=0,l=0,c=0;
        bool have = false;
        // Feed a value; if the bucket rolls, write the just-closed bar to `out`
        // and return true. `hi`/`lo` let the caller pass a high/low proxy; here
        // we aggregate mid so hi==lo==mid each tick.
        bool update(double hi, double lo, int64_t now_ms, int64_t tf_sec, Bar& out) {
            const int64_t b = (now_ms / 1000 / tf_sec) * tf_sec;
            const double v = hi; // mid
            if (!have) { bucket=b; o=h=l=c=v; have=true; return false; }
            if (b != bucket) {
                out.ts=bucket; out.o=o; out.h=h; out.l=l; out.c=c;
                bucket=b; o=h=l=c=v;
                return true;
            }
            if (hi > h) h = hi;
            if (lo < l) l = lo;
            c = v;
            return false;
        }
    } m_agg_bias, m_agg_break, m_agg_retest;

    // ── Incremental EMA pair (bias TF) ───────────────────────────────────────
    struct Ema {
        int fast_n=50, slow_n=200;
        double fast=0, slow=0;
        double fseed=0, sseed=0; int fcnt=0, scnt=0;
        bool fready=false, sready=false;
        void push(double c) {
            if (!fready) { fseed+=c; if(++fcnt>=fast_n){ fast=fseed/fast_n; fready=true; } }
            else { double k=2.0/(fast_n+1.0); fast += k*(c-fast); }
            if (!sready) { sseed+=c; if(++scnt>=slow_n){ slow=sseed/slow_n; sready=true; } }
            else { double k=2.0/(slow_n+1.0); slow += k*(c-slow); }
        }
        bool ready() const { return fready && sready; }
    } m_bias;

    // ── Incremental Wilder ATR ───────────────────────────────────────────────
    struct Atr {
        int period=14;
        double atr=0, prev_close=0, seed=0; int cnt=0; bool have_prev=false, ready=false;
        void push(const Bar& bar) {
            if (!have_prev) { prev_close=bar.c; have_prev=true; return; }
            const double tr = std::max(bar.h-bar.l,
                                std::max(std::fabs(bar.h-prev_close),
                                         std::fabs(bar.l-prev_close)));
            prev_close = bar.c;
            if (!ready) { seed+=tr; if(++cnt>=period){ atr=seed/period; ready=true; } }
            else { atr = (atr*(period-1)+tr)/period; }
        }
    } m_atr_break, m_atr_retest;

    // ── Wilder ADX (trend strength, on BREAK_TF) ─────────────────────────────
    struct Adx {
        int period=14;
        double trS=0, pdmS=0, ndmS=0;     // Wilder-smoothed TR / +DM / -DM sums
        double prevH=0, prevL=0, prevC=0; bool have_prev=false;
        double seed_tr=0, seed_pdm=0, seed_ndm=0; int warm=0; bool di_ready=false;
        double adx=0, dx_seed=0; int dx_cnt=0; bool ready=false;
        void push(double h, double l, double c) {
            if (!have_prev) { prevH=h; prevL=l; prevC=c; have_prev=true; return; }
            const double up = h - prevH, dn = prevL - l;
            const double pDM = (up > dn && up > 0) ? up : 0.0;
            const double nDM = (dn > up && dn > 0) ? dn : 0.0;
            const double tr  = std::max(h-l, std::max(std::fabs(h-prevC), std::fabs(l-prevC)));
            prevH=h; prevL=l; prevC=c;
            if (!di_ready) {
                seed_tr+=tr; seed_pdm+=pDM; seed_ndm+=nDM;
                if (++warm >= period) { trS=seed_tr; pdmS=seed_pdm; ndmS=seed_ndm; di_ready=true; }
                return;
            }
            trS  = trS  - trS/period  + tr;
            pdmS = pdmS - pdmS/period + pDM;
            ndmS = ndmS - ndmS/period + nDM;
            if (trS <= 0) return;
            const double pdi = 100.0*pdmS/trS, ndi = 100.0*ndmS/trS;
            const double dx  = (pdi+ndi > 0) ? 100.0*std::fabs(pdi-ndi)/(pdi+ndi) : 0.0;
            if (!ready) { dx_seed+=dx; if (++dx_cnt >= period) { adx=dx_seed/period; ready=true; } }
            else { adx = (adx*(period-1)+dx)/period; }
        }
    } m_adx;

    // ── Closed-bar handlers ──────────────────────────────────────────────────
    void _on_bias_close(const Bar& b) {
        m_bias.push(b.c);
        if (!m_bias.ready()) { m_bias_ready=false; return; }
        m_bias_ready = true;
        if (b.c > m_bias.fast && m_bias.fast > m_bias.slow)      m_bias_dir = 1;
        else if (b.c < m_bias.fast && m_bias.fast < m_bias.slow) m_bias_dir = -1;
        else m_bias_dir = 0;
    }

    void _on_break_close(const Bar& b) {
        m_atr_break.push(b);
        m_adx.push(b.h, b.l, b.c);
        // Regime guard: don't arm a breakout in chop. ADX low = consolidation,
        // where break-and-retest whipsaws. 0 = off.
        const bool regime_ok = (REGIME_ADX_MIN <= 0.0) ||
                               (m_adx.ready && m_adx.adx >= REGIME_ADX_MIN);
        // Need a full lookback window of PRIOR closed bars + warm ATR + bias.
        if (regime_ok && (int)m_range.size() >= LOOKBACK && m_atr_break.ready && m_bias_ready && m_bias_dir != 0) {
            double resistance = -1e18, support = 1e18;
            for (const auto& r : m_range) { resistance=std::max(resistance,r.h); support=std::min(support,r.l); }
            const double body   = std::fabs(b.c - b.o);
            const double atr    = m_atr_break.atr;
            const double buffer = atr * BREAK_BUFFER_ATR;
            const double minbody= atr * MIN_BREAK_BODY_ATR;

            if (m_bias_dir == 1 && b.c > resistance + buffer && body >= minbody) {
                m_arm_dir = 1; m_arm_level = resistance;
                m_arm_expire_ms = b.ts*1000 + (BREAK_TF_SEC + (int64_t)BREAK_VALID_BARS*RETEST_TF_SEC)*1000;
                if (verbose) printf("[%s] ARM LONG level=%.3f atr=%.3f\n", engine_name.c_str(), m_arm_level, atr);
            } else if (m_bias_dir == -1 && b.c < support - buffer && body >= minbody) {
                m_arm_dir = -1; m_arm_level = support;
                m_arm_expire_ms = b.ts*1000 + (BREAK_TF_SEC + (int64_t)BREAK_VALID_BARS*RETEST_TF_SEC)*1000;
                if (verbose) printf("[%s] ARM SHORT level=%.3f atr=%.3f\n", engine_name.c_str(), m_arm_level, atr);
            }
        }
        // Maintain the trailing range window of the last LOOKBACK closed bars.
        m_range.push_back(b);
        while ((int)m_range.size() > LOOKBACK) m_range.pop_front();
    }

    void _on_retest_close(const Bar& b, double bid, double ask, int64_t now_ms) {
        m_atr_retest.push(b);
        if (pos.active) return;
        if (m_arm_dir == 0) return;
        if (now_ms > m_arm_expire_ms) { m_arm_dir = 0; return; }
        if (!m_atr_retest.ready) return;
        if (m_bias_dir != m_arm_dir) return;
        if (now_ms/1000 - m_last_entry_ms/1000 < MIN_SEC_BETWEEN) return;
        if (USE_SESSION && !_in_session(now_ms)) return;
        if ((ask - bid) > MAX_SPREAD) return;

        const double atr = m_atr_retest.atr;
        const double zone = atr * RETEST_ZONE_ATR;
        const double cbuf = atr * BOUNCE_CLOSE_BUFFER_ATR;

        if (m_arm_dir == 1) {
            const bool touched = (b.l <= m_arm_level + zone);
            const bool bounced = (b.c > b.o && b.c > m_arm_level + cbuf);
            if (!touched || !bounced) return;
            const double entry = ask;                       // buy at ask
            const double stop  = std::min(b.l - atr*0.25, entry - atr*STOP_ATR);
            const double risk  = entry - stop;
            if (risk <= 0.0) return;
            _open(true, entry, stop, entry + risk*REWARD_RISK, risk, atr, ask-bid, now_ms);
        } else {
            const bool touched = (b.h >= m_arm_level - zone);
            const bool bounced = (b.c < b.o && b.c < m_arm_level - cbuf);
            if (!touched || !bounced) return;
            const double entry = bid;                       // sell at bid
            const double stop  = std::max(b.h + atr*0.25, entry + atr*STOP_ATR);
            const double risk  = stop - entry;
            if (risk <= 0.0) return;
            _open(false, entry, stop, entry - risk*REWARD_RISK, risk, atr, ask-bid, now_ms);
        }
    }

    // ── Position open / manage / close ───────────────────────────────────────
    void _open(bool is_long, double entry, double sl, double tp, double risk,
               double atr, double spread, int64_t now_ms) {
        pos = Position{};
        pos.active=true; pos.is_long=is_long; pos.entry_px=entry; pos.sl_px=sl;
        pos.tp_px=tp; pos.risk=risk; pos.atr=atr; pos.size=lot; pos.entry_ms=now_ms;
        pos.spread_at_entry=spread;
        pos.peak_px = entry;           // seed give-back peak at entry
        pos.adx_entry = m_adx.ready ? m_adx.adx : 0.0;  // regime tag for capture
        m_last_capture_ms = 0;         // capture the first manage tick
        m_arm_dir = 0;                 // consume the arm
        m_last_entry_ms = now_ms;
        if (verbose) printf("[%s] %s entry=%.3f sl=%.3f tp=%.3f risk=%.3f\n",
            engine_name.c_str(), is_long?"BUY":"SELL", entry, sl, tp, risk);
    }

    void _manage(double bid, double ask, int64_t now_ms) {
        const double fav = pos.is_long ? (bid - pos.entry_px) : (pos.entry_px - ask);
        if (fav > pos.mfe) pos.mfe = fav;
        if (fav < pos.mae) pos.mae = fav;

        // L2 capture: throttled snapshot of the open position + live imbalance.
        if (USE_L2_CAPTURE && on_l2_sample &&
            now_ms - m_last_capture_ms >= L2_CAPTURE_SEC * 1000) {
            m_last_capture_ms = now_ms;
            on_l2_sample(pos.entry_ms, now_ms, bid, ask, m_l2_imb, fav,
                         pos.sl_px, pos.risk, pos.atr, pos.adx_entry, pos.is_long);
        }

        // R-based break-even.
        if (USE_BREAKEVEN && !pos.be_armed && fav >= BE_ARM_R * pos.risk) {
            pos.be_armed = true;
            const double be = pos.is_long ? pos.entry_px + BE_LOCK_R*pos.risk
                                          : pos.entry_px - BE_LOCK_R*pos.risk;
            if (pos.is_long) pos.sl_px = std::max(pos.sl_px, be);
            else             pos.sl_px = std::min(pos.sl_px, be);
        }
        // ATR chandelier trail.
        if (USE_TRAIL && fav >= TRAIL_START_R * pos.risk) {
            const double t = pos.is_long ? bid - TRAIL_ATR*pos.atr
                                         : ask + TRAIL_ATR*pos.atr;
            if (pos.is_long) pos.sl_px = std::max(pos.sl_px, t);
            else             pos.sl_px = std::min(pos.sl_px, t);
        }

        // L2 profit-protect. Armed only once >= L2_ARM_R in profit. Locks the
        // stop to a tight chandelier when book flow turns hostile AND price
        // has given back from its peak. Only tightens toward profit -- never
        // widens, never exits at a loss.
        if (USE_L2_PROTECT && fav >= L2_ARM_R * pos.risk && pos.atr > 0.0) {
            const double px = pos.is_long ? bid : ask;
            pos.peak_px = pos.is_long ? std::max(pos.peak_px, px)
                                      : std::min(pos.peak_px, px);
            const bool hostile = pos.is_long ? (m_l2_imb <= L2_HOSTILE_IMB)
                                             : (m_l2_imb >= 1.0 - L2_HOSTILE_IMB);
            const double giveback = pos.is_long ? (pos.peak_px - px)
                                                : (px - pos.peak_px);
            if (hostile && giveback >= L2_GIVEBACK_ATR * pos.atr) {
                const double lock = pos.is_long ? pos.peak_px - L2_LOCK_ATR*pos.atr
                                                : pos.peak_px + L2_LOCK_ATR*pos.atr;
                if (pos.is_long) pos.sl_px = std::max(pos.sl_px, lock);
                else             pos.sl_px = std::min(pos.sl_px, lock);
            }
        }

        if (pos.is_long) {
            if (bid <= pos.sl_px) { _close(bid, now_ms, "SL"); return; }
            if (bid >= pos.tp_px) { _close(bid, now_ms, "TP"); return; }
        } else {
            if (ask >= pos.sl_px) { _close(ask, now_ms, "SL"); return; }
            if (ask <= pos.tp_px) { _close(ask, now_ms, "TP"); return; }
        }
        if (MAX_HOLD_SEC > 0 && (now_ms - pos.entry_ms)/1000 >= MAX_HOLD_SEC)
            _close(pos.is_long ? bid : ask, now_ms, "TIMEOUT");
    }

    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;
        const double pnl_px = pos.is_long ? (exit_px - pos.entry_px) : (pos.entry_px - exit_px);

        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.engine     = engine_name;
        tr.side       = pos.is_long ? "LONG" : "SHORT";
        tr.entryPrice = pos.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = pos.tp_px;
        tr.sl         = pos.sl_px;
        tr.size       = pos.size;
        tr.pnl        = pnl_px * pos.size;
        tr.mfe        = pos.mfe * pos.size;
        tr.mae        = std::fabs(pos.mae) * pos.size;
        tr.entryTs    = pos.entry_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.spreadAtEntry = pos.spread_at_entry;
        tr.atr_at_entry  = pos.atr;
        tr.shadow     = shadow_mode;

        if (on_trade_record) on_trade_record(tr);
        if (on_close) on_close(exit_px, pos.is_long, pos.size, std::string(reason));
        pos = Position{};
    }

    // ── Helpers ──────────────────────────────────────────────────────────────
    bool _in_session(int64_t now_ms) const {
        std::time_t t = (std::time_t)(now_ms/1000);
        std::tm* ti = std::gmtime(&t);
        if (!ti) return true;
        const int h = ti->tm_hour;
        if (SESSION_START_H == SESSION_END_H) return true;
        if (SESSION_START_H < SESSION_END_H) return h>=SESSION_START_H && h<SESSION_END_H;
        return h>=SESSION_START_H || h<SESSION_END_H;
    }

    // ── State ────────────────────────────────────────────────────────────────
    std::deque<Bar> m_range;       // last LOOKBACK closed BREAK_TF bars
    int  m_bias_dir = 0; bool m_bias_ready = false;
    int  m_arm_dir = 0; double m_arm_level = 0.0; int64_t m_arm_expire_ms = 0;
    int64_t m_last_entry_ms = 0;
    int64_t m_last_capture_ms = 0;  // L2 capture throttle
    double m_l2_imb = 0.5;          // latest L2 imbalance pushed by the host

public:
    // ── Warm-seed (Engine Warm-Seed Mandate) ─────────────────────────────────
    // The slow-TF EMA(slow) needs ~SLOW_EMA bias bars before bias is ready
    // (EMA200 on D1 = ~200 days). These replay CLOSED historical bars straight
    // into the indicators/range -- NO arming, NO entries, independent of the
    // `enabled` flag -- so the engine boots hot. Call seed_from_csvs() once at
    // init with the bias / break / retest warm CSVs.
    void warm_bias(double close)              { _on_bias_close(Bar{0,0,0,0,close}); }
    void warm_break(double o,double h,double l,double c) {
        Bar b{0,o,h,l,c}; m_atr_break.push(b); m_adx.push(h,l,c);
        m_range.push_back(b); while ((int)m_range.size() > LOOKBACK) m_range.pop_front();
    }
    void warm_retest(double o,double h,double l,double c) {
        Bar b{0,o,h,l,c}; m_atr_retest.push(b);
    }

    // Replay the three warm CSVs (format: "bar_start_ms,open,high,low,close").
    // Returns total bars replayed. Emits one [SEED] line. Missing paths skip
    // that TF (a warning, not fatal -- live ticks will still warm it, slowly).
    size_t seed_from_csvs(const std::string& bias_csv,
                          const std::string& break_csv,
                          const std::string& retest_csv) {
        size_t n = 0;
        n += _seed_one(bias_csv,   0);   // bias: close only
        n += _seed_one(break_csv,  1);   // break: OHLC -> ATR + range
        n += _seed_one(retest_csv, 2);   // retest: OHLC -> ATR
        printf("[SEED] %s: %zu warm bars replayed (D1/H1/retest) -- engine hot\n",
               engine_name.c_str(), n);
        fflush(stdout);
        return n;
    }

private:
    size_t _seed_one(const std::string& path, int which) {
        if (path.empty()) return 0;
        std::ifstream f(path);
        if (!f.is_open()) {
            printf("[SEED] %s: WARN cannot open %s (TF will warm from live ticks)\n",
                   engine_name.c_str(), path.c_str());
            return 0;
        }
        std::string line; std::getline(f, line);  // header
        size_t n = 0; long long ts; double o,h,l,c;
        while (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) == 5) {
                if (which == 0)      warm_bias(c);
                else if (which == 1) warm_break(o,h,l,c);
                else                 warm_retest(o,h,l,c);
                ++n;
            }
        }
        return n;
    }

public:
};

}  // namespace omega
