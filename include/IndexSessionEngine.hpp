#pragma once
// =============================================================================
//  IndexSessionEngine.hpp -- intraday cash-session LONG for equity indices
// =============================================================================
//
//  PROVENANCE (2026-06-01)
//
//  Edge hunt over GER40 / SPXUSD / NSXUSD tick data decomposed each index's
//  return into OVERNIGHT (RTH close -> next open) vs INTRADAY (RTH open ->
//  close). The gains live in the CASH SESSION; overnight is low-Sharpe (US) or
//  NEGATIVE (GER40 -2.5%). So: be LONG the cash session, FLAT overnight.
//
//  VALIDATION -- LIVE on_tick engine (not a sim), long-only, ATR stop, skip-Fri,
//  real bid/ask, IS/OOS 60/40. The edge is HOLDING INTO THE US CLOSE (entering
//  mid-morning, exiting ~22:00 UTC / US close), flat overnight -- NOT the local
//  cash session alone (GER40 07-15 actually LOSES). Per-symbol window + honest
//  out-of-sample (the OOS number is what to trust):
//      SPXUSD 14-22 UTC : PF 1.14  Sharpe 0.65   IS 0.66 / OOS 0.67  DD 11%  <- most robust
//      NSXUSD 14-22 UTC : PF 1.09  Sharpe 0.46   IS 0.54 / OOS 0.34  DD 15%
//      GER40  09-20 UTC : PF 1.41  Sharpe 1.87   IS 2.50 / OOS 0.60  DD 6%   (Sharpe IS-inflated)
//  DIP-BUY FILTER (ENTER_ON_WEAK_ONLY) -- the edge is concentrated AFTER
//  weakness (prior session down OR gap down -> next-session Sharpe +2.3..3.0;
//  after an up day it's flat). Conditioning entry on it (validated OOS):
//      SPXUSD 14-22 : OOS Sharpe 0.67 -> 1.48,  PF 1.28,  DD 8.3%
//      NSXUSD 14-22 : OOS Sharpe 0.34 -> 0.66,  PF 1.11
//      GER40  09-20 : OOS Sharpe 0.60 -> 0.62,  PF 1.12,  DD 5.2%
//  ON by default in engine_init. (Turn-of-month = no edge; vol-target = flat.)
//  2024-26 data = bull only; down-side profit is dip-RECOVERY longs, NOT shorts.
//
//  PROFITS IN UP *AND* DOWN -- but LONG-only, NOT by shorting (shorting indices
//  loses, Sharpe -1 to -2.8; they are structurally long-biased). The down-side
//  profit comes from BUYING the beaten-down sessions: when price is below its
//  50d MA, the session-long Sharpe is HIGHEST (1.38-1.53) -- intraday dip
//  recovery. Counter-cyclical alpha, strongest when the market is weak.
//
//  PROTECTION (4 layers):
//    1. FLAT OVERNIGHT  -- exit at RTH close, no gap/overnight risk.
//    2. INTRADAY ATR STOP -- cut a session that goes against us.
//    3. RISK-OFF GATE   -- host sets set_risk_off(true) from IndexRiskGate /
//       VIX; the engine stands aside. This is the BEAR guard -- dip-buying
//       works in bull pullbacks but must switch OFF in a sustained bear
//       (the 2024-26 data is bull-only; do not buy dips into a real crash).
//    4. DAY-OF-WEEK     -- skip Friday (negative across all three indices).
//
//  ARCHITECTURE: self-managing on_tick() engine (cf. Ger40LondonBreakoutEngine).
//  One position/day, long-only. shadow_mode=true by default.
//
// =============================================================================

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <string>

#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"   // S-2026-06-03: persist

namespace omega {

class IndexSessionEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol = "GER40";
    std::string engine_name = "IndexSession";

    // ── Session (UTC hours). Edge = hold INTO the US close, flat overnight.
    //   Per-symbol (set in engine_init): SPX/NAS 14-22, GER40 09-20.
    int RTH_OPEN_H  = 14;
    int RTH_CLOSE_H = 22;

    // ── Risk geometry ────────────────────────────────────────────────────────
    int    ATR_PERIOD = 14;     // EWM of session range (high-low)
    double STOP_ATR   = 2.0;    // intraday stop = STOP_ATR * session-ATR below entry
    // S-2026-06-03: hard intraday catastrophe cap. The ATR stop (2*session-ATR
    // ~2-3% on indices) is wide enough that a normal down-session never hits it
    // and the position bleeds to SESSION_CLOSE (e.g. GER40 -168 on 2026-06-02).
    // This caps the per-trade loss independent of ATR. 0 = off.
    // NEEDS A BACKTEST (index_session_test) before live-enable — it changes exits.
    double MAX_LOSS_PCT = 1.0;  // exit if price falls > this % below entry intraday
    bool   SKIP_FRIDAY = true;  // Fri is negative across GER40/SPX/NAS

    // Dip-buy filter: the session edge is concentrated AFTER weakness (prior
    // session down OR gap down -> next session Sharpe +2.3 to +3.0; after an up
    // day it's flat/negative). Only enter when the prior session closed below
    // WEAK_PREV_RET%% OR today opens below the prior close. Off by default.
    bool   ENTER_ON_WEAK_ONLY = false;
    double WEAK_PREV_RET = 0.0; // prior-session return %% below which counts as "down"

    // ── Sizing / control ─────────────────────────────────────────────────────
    double lot         = 1.0;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    // ── Callbacks ────────────────────────────────────────────────────────────
    using CloseCallback = std::function<void(double exit_px, bool is_long,
                                             double size, const std::string& reason)>;
    CloseCallback on_close;
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // ── Risk-off gate (bear guard) -- host sets from IndexRiskGate/VIX ───────
    void set_risk_off(bool off) { m_risk_off = off; }

    // ── Position ─────────────────────────────────────────────────────────────
    struct Position { bool active=false; double entry_px=0, stop_px=0, size=0;
                      int64_t entry_ms=0; double mfe=0, mae=0; } pos;

    // S-2026-06-03: persist/resume (long-only; ATR stop -> sl, no tp).
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=pos.stop_px; o.tp=0.0; o.entry_ts=pos.entry_ms/1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos.active=true; pos.entry_px=ps.entry; pos.stop_px=ps.sl; pos.size=ps.size;
        pos.entry_ms=ps.entry_ts*1000;
        return true;
    }
    bool has_open_position() const { return pos.active; }
    bool   atr_ready() const { return m_atr_ready; }   // diagnostic
    double atr_value() const { return m_atr; }         // diagnostic

    void init() {
        m_day=-1; m_entered_today=false; m_in_session=false;
        m_sess_hi=-1e18; m_sess_lo=1e18; m_have_sess=false;
        m_atr=0; m_atr_seed=0; m_atr_cnt=0; m_atr_ready=false;
        m_sess_open=0; m_sess_close=0; m_prev_sret=0; m_prev_close=0; m_prev_ready=false;
        pos = Position{};
    }

    // Optional warm-seed: feed historical session ranges to prime the ATR so the
    // engine can trade from day one (no 14-session cold start).
    void warm_session_range(double range) {
        if (!m_atr_ready) { m_atr_seed+=range; if(++m_atr_cnt>=ATR_PERIOD){ m_atr=m_atr_seed/ATR_PERIOD; m_atr_ready=true; } }
        else m_atr = (m_atr*(ATR_PERIOD-1)+range)/ATR_PERIOD;
    }

    // Warm the session-range ATR from a D1 bar CSV (bar_start_ms,o,h,l,c).
    // The full-day high-low is a slightly-wide proxy for the session range --
    // good enough to prime the stop so the engine trades on day one. Emits a
    // [SEED] line (Warm-Seed Mandate). Returns bars replayed.
    size_t seed_from_d1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s (ATR warms from live)\n",
                                   engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);  // header
        size_t n = 0; long long ts; double o,h,l,c;
        while (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) == 5 && h > l) {
                warm_session_range(h - l); ++n;
            }
        }
        printf("[SEED] %s (%s): %zu D1 ranges replayed -- ATR hot=%d\n",
               engine_name.c_str(), symbol.c_str(), n, (int)m_atr_ready);
        fflush(stdout);
        return n;
    }

    // ── Main tick handler ────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid <= 0.0 || ask <= 0.0 || ask < bid) return;
        const double mid = (bid + ask) * 0.5;

        std::time_t t = (std::time_t)(now_ms/1000);
        std::tm* ti = std::gmtime(&t);
        if (!ti) return;
        const int wday = ti->tm_wday, hour = ti->tm_hour, yday = ti->tm_yday;

        // ── Day boundary: finalize prior session range -> ATR + prev-return ──
        if (yday != m_day) {
            if (m_have_sess && m_sess_hi > m_sess_lo)
                warm_session_range(m_sess_hi - m_sess_lo);
            if (m_have_sess && m_sess_open > 0.0) {
                m_prev_sret  = (m_sess_close/m_sess_open - 1.0) * 100.0;
                m_prev_close = m_sess_close; m_prev_ready = true;
            }
            m_day = yday; m_entered_today = false;
            m_sess_hi = -1e18; m_sess_lo = 1e18; m_have_sess = false;
            m_sess_open = 0; m_sess_close = 0;
        }

        const bool in_session = (hour >= RTH_OPEN_H && hour < RTH_CLOSE_H);
        if (in_session) { m_sess_hi=std::max(m_sess_hi,mid); m_sess_lo=std::min(m_sess_lo,mid); m_have_sess=true;
                          if(m_sess_open<=0) m_sess_open=mid; m_sess_close=mid; }

        // ── Manage open position ─────────────────────────────────────────────
        if (pos.active) {
            const double fav = bid - pos.entry_px;
            if (fav > pos.mfe) pos.mfe = fav;
            if (fav < pos.mae) pos.mae = fav;
            if (bid <= pos.stop_px)      { _close(bid, now_ms, "STOP"); return; }
            if (MAX_LOSS_PCT > 0.0 && bid <= pos.entry_px * (1.0 - MAX_LOSS_PCT / 100.0)) {
                _close(bid, now_ms, "MAX_LOSS"); return;   // S-2026-06-03 catastrophe cap
            }
            if (hour >= RTH_CLOSE_H || !in_session) { _close(bid, now_ms, "SESSION_CLOSE"); return; }
            return;   // hold through the session
        }

        // ── Entry: once per day, at the RTH open, long only ──────────────────
        if (m_entered_today || !in_session || !m_atr_ready) return;
        if (m_risk_off) return;                         // bear guard
        if (SKIP_FRIDAY && wday == 5) return;           // skip Friday
        if (m_atr <= 0.0) return;

        const double entry = ask;                       // buy at ask
        // Dip-buy filter: require the prior session to have closed DOWN, or
        // today to open below the prior close (gap down). The edge lives here.
        if (ENTER_ON_WEAK_ONLY && m_prev_ready) {
            const double gap = (entry / m_prev_close - 1.0) * 100.0;
            if (m_prev_sret >= WEAK_PREV_RET && gap >= 0.0) return;
        }
        pos = Position{};
        pos.active=true; pos.entry_px=entry; pos.stop_px=entry - STOP_ATR*m_atr;
        pos.size=lot; pos.entry_ms=now_ms;
        m_entered_today = true;
        if (verbose) printf("[%s] %s LONG entry=%.2f stop=%.2f atr=%.2f\n",
            engine_name.c_str(), symbol.c_str(), entry, pos.stop_px, m_atr);
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms) {
        if (pos.active) _close(bid, now_ms, "FORCE_CLOSE");
    }

private:
    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;
        const double pnl_px = exit_px - pos.entry_px;   // long
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.engine=engine_name; tr.side="LONG";
        tr.entryPrice=pos.entry_px; tr.exitPrice=exit_px; tr.sl=pos.stop_px;
        tr.size=pos.size; tr.pnl=pnl_px*pos.size; tr.mfe=pos.mfe*pos.size; tr.mae=std::fabs(pos.mae)*pos.size;
        tr.entryTs=pos.entry_ms/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason; tr.shadow=shadow_mode;
        if (on_trade_record) on_trade_record(tr);
        if (on_close) on_close(exit_px, true, pos.size, std::string(reason));
        if (verbose) printf("[%s] CLOSE %s exit=%.2f pnl=%.2f\n", engine_name.c_str(), reason, exit_px, pnl_px);
        pos = Position{};
    }

    int     m_day=-1; bool m_entered_today=false, m_in_session=false;
    double  m_sess_hi=-1e18, m_sess_lo=1e18; bool m_have_sess=false;
    double  m_atr=0, m_atr_seed=0; int m_atr_cnt=0; bool m_atr_ready=false;
    double  m_sess_open=0, m_sess_close=0, m_prev_sret=0, m_prev_close=0; bool m_prev_ready=false;
    bool    m_risk_off=false;
};

}  // namespace omega
