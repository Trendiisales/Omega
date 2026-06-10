#pragma once
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

namespace omega {

class PumpScalpEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol      = "";            // set per active pump symbol by the manager
    std::string engine_name = "PumpScalp";   // manager suffixes _5m/_10m/_15m

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
    double TRAIL_PCT    = 3.0;     // HARD trailing stop off peak/trough (checked every tick)
    double HARD_PCT     = 6.0;     // hard stop from entry
    int    MAXHOLD_SEC  = 30*300;  // time stop (default 30 x 5m bars worth of seconds)
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

    bool   enabled      = true;
    bool   shadow_mode  = true;    // gated shadow — AH fills + borrow + dud-rate all unmeasured
    bool   verbose      = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

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
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos = Position{}; pos.active=true; pos.dir = (ps.side=="SHORT") ? -1 : 1;
        pos.units = {ps.entry}; pos.size_each=ps.size; pos.peak=ps.entry; pos.trough=ps.entry;
        pos.last_add=ps.entry; pos.entry_ms=ps.entry_ts*1000;
        return true;
    }
    bool has_open_position() const { return pos.active; }

    void init() { _new_day(-1); pos = Position{}; }

    // ── FAST path: every price update. Manages the open position so it exits
    //   IMMEDIATELY on the turn (all three TF engines share this behaviour). ──
    void on_price(double px, int64_t ts_ms) {
        if (px <= 0) return;
        const int64_t day = (ts_ms/1000)/86400;
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
            const double stop = std::max(pos.peak*(1-TRAIL_PCT/100), base*(1-HARD_PCT/100));
            if (px <= stop) { _close(stop, ts_ms, "TRAIL"); return; }
        } else {
            pos.trough = std::min(pos.trough, px);
            if (PYR_ADDS>0 && (int)pos.units.size()<1+PYR_ADDS && px <= pos.last_add*(1-PYR_STEP/100)) {
                pos.units.push_back(px); pos.last_add=px;
            }
            const double stop = std::min(pos.trough*(1+TRAIL_PCT/100), base*(1+HARD_PCT/100));
            if (px >= stop) { _close(stop, ts_ms, "TRAIL"); return; }
        }
        if (ts_ms - pos.entry_ms >= (int64_t)MAXHOLD_SEC*1000) _close(px, ts_ms, "TIME");
    }

    // ── ENTRY path: one CLOSED TF OHLCV bar (with volume).
    //   is_seed=true => warm history/EMA/day-open/gate state ONLY, fire NO entries
    //   (warm-seed mandate: historical bars must never open a phantom trade). ──
    void on_entry_bar(double o, double h, double l, double c, double v, int64_t ts_ms, bool is_seed=false) {
        if (h < l || c <= 0) return;
        const int64_t day = (ts_ms/1000)/86400;
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

        if (is_seed) return;                             // SEED: warm only — never enter on history
        if (pos.active || !enabled) return;              // exits are on_price's job
        if (m_day_open<=0 || (m_run_high/m_day_open - 1.0)*100.0 < DAY_GATE_PCT) return;  // gate
        if (idx < LB + 21) return;

        // volume-regression filter: VWAP + slope must agree with the trade direction
        const double vwap = _vwap(), slope = _slope();
        const bool long_ok  = (!VOL_REG_FILTER) || (vwap<=0) || (c > vwap && slope >=  SLOPE_MIN);
        const bool short_ok = (!VOL_REG_FILTER) || (vwap<=0) || (c < vwap && slope <= -SLOPE_MIN);

        double avgv=0; int n=0;
        for (int i=(int)m_bars.size()-2; i>=0 && n<20; --i,++n) avgv += m_bars[i].v;
        avgv = n>0 ? avgv/n : 1.0; if (avgv<=0) avgv=1.0;

        // IGNITION long
        const double c_lb = m_bars[(int)m_bars.size()-1-LB].c;
        const bool strong = (h>l) ? (c >= l + STRENGTH*(h-l)) : true;
        if (long_ok && (c/c_lb - 1.0)*100.0 >= IG_PCT && v >= VOLX*avgv && strong) { _open(+1, c, ts_ms); return; }

        // EXHAUSTION short (strict top-fade only)
        const double runup = (c/m_day_open - 1.0)*100.0;
        const double ext   = (c/m_ema9   - 1.0)*100.0;
        const bool new_hod = (idx - m_hod_idx) <= NEWHOD_M;
        const bool bear_brk = (c < o) && (c < prev.l);
        if (short_ok && ALLOW_SHORT && runup>=RUNUP_PCT && ext>=EXT_PCT && new_hod && bear_brk) { _open(-1, c, ts_ms); }
    }

    void force_close(double px, int64_t now_ms) { if (pos.active) _close(px, now_ms, "FORCE_CLOSE"); }

private:
    struct Bar { double o,h,l,c,v; };

    void _new_day(int64_t day) {                          // single source of session reset
        m_day=day; m_day_open=0; m_run_high=0; m_hod=-1e18; m_hod_idx=-1;
        m_bars.clear(); m_ema9=0; m_ema_init=false;
        m_cum_pv=0; m_cum_v=0;
    }

    double _vwap() const { return m_cum_v>0 ? m_cum_pv/m_cum_v : 0.0; }

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
        pos = Position{}; pos.active=true; pos.dir=dir; pos.size_each=lot;
        pos.units = {px}; pos.peak=px; pos.trough=px; pos.last_add=px; pos.entry_ms=ts_ms;
        if (verbose) printf("[%s] %s %s entry=%.4f gate=%.0f%%\n", engine_name.c_str(),
            symbol.c_str(), dir>0?"LONG":"SHORT", px, (m_run_high/m_day_open-1)*100);
    }

    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active || pos.units.empty()) return;
        double pnl_px = 0;                                            // sum over the unit stack
        for (double u : pos.units) pnl_px += pos.dir>0 ? (exit_px-u) : (u-exit_px);
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
    double  m_ema9=0; bool m_ema_init=false;
    double  m_cum_pv=0, m_cum_v=0;   // intraday VWAP accumulators
};

}  // namespace omega
