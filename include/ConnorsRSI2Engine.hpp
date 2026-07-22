#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ConnorsRSI2Engine — the classic systematic mean-reversion dip-buy (Larry
// Connors RSI-2). Buy the index at the cash CLOSE when it is in an UPTREND
// (close > SMA_TREND) AND deeply short-term oversold (RSI(2) < RSI_IN); exit at
// the NEXT cash close (1-day hold). The SMA trend filter only buys dips inside
// an uptrend -> built-in bear protection (no dip-buying in a downtrend).
//
//   BACKTEST (2026-06-04, see memory omega-overnight-drift-index-edge / connors):
//     NDX cash RSI2<10 >SMA200: Sharpe 2.46 both halves+ (cost-incl, 1-day hold).
//     IBKR CFD >SMA200: 2.33 both+. NQ FUTURE >SMA50: 2.71 both+. Instrument-
//     agnostic (cash+CFD+future). Orthogonal to FVGcont (intraday momentum) and
//     OvernightDrift (close->open) -> the DAILY MEAN-REVERSION family (MR fails
//     intraday on the index but works on the 2-5 day swing dip). shadow default.
// ─────────────────────────────────────────────────────────────────────────────
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"

namespace omega {

class ConnorsRSI2Engine {
public:
    std::string symbol      = "NAS100";
    std::string engine_name = "ConnorsRSI2";
    int    TREND_SMA   = 200;    // uptrend filter (cash: 200; future-style: 50)
    int    RSI_LEN     = 2;
    double RSI_IN      = 10.0;   // canonical Connors deep-oversold threshold
    int    HOLD_DAYS   = 1;      // fallback exit (used only if SHORT_SMA<=0)
    // S-2026-06-19 ENHANCED EXIT (faithful 10yr-daily sweep, connors_opt.cpp): exit when
    // today's close climbs back above SMA(SHORT_SMA) — the mean-reversion has completed —
    // OR a MAXHOLD safety cap. This beats the fixed 1-day hold massively: NDX PF 1.33->1.90
    // both-halves+ 3x(8pt)-robust; also REVIVES GER40 (PF1.39 both+, 2022 +290). SHORT_SMA<=0
    // reverts to the legacy HOLD_DAYS exit.
    int    SHORT_SMA   = 5;      // exit when close > SMA(SHORT_SMA)
    int    MAXHOLD     = 10;     // safety cap on hold (days)
    // S-2026-06-19 v2 SCALE-IN (Connors cumulative, default OFF). While the position is
    // open and STILL oversold (RSI<RSI_IN, close>SMA(TREND_SMA)), average a 2nd unit in.
    // Faithful 10yr NDX: PF1.90->2.27 WR81% (connors_opt.cpp). COST: up to MAX_UNITS x
    // size + averages into a continued dip -> larger per-episode risk. Default off; enable
    // per-instrument after faithful confirm.
    bool   SCALEIN     = false;
    int    MAX_UNITS   = 2;
    // S-2026-06-19 v2 SESSION PARAM (default = NAS cash ET 09:30-16:00). For non-US
    // instruments (e.g. GER40 DAX Xetra 09:00-17:30 CET) set the local window + tz.
    // TZ_STD_OFF_MIN: standard-time UTC offset in minutes (ET=-300, CET=+60).
    // TZ_EU_DST: false = US DST rules (2nd-Sun-Mar..1st-Sun-Nov), true = EU (last-Sun).
    int    SESS_OPEN_HM   = 930;
    int    SESS_CLOSE_HM  = 1600;
    int    TZ_STD_OFF_MIN = -300;   // ET
    bool   TZ_EU_DST      = false;  // US rules
    // S-2026-06-19 v3 ENTRY_MODE — the oversold-dip-in-uptrend FAMILY (mr_hunt.cpp, 10yr daily,
    // all 8pt-cost-robust both-halves+). 0=RSI2(<RSI_IN) 1=IBS(<IBS_IN) 2=STREAK(>=STREAK_N down
    // closes) 3=PCTB(%b<PCTB_IN) 4=RSI3(<RSI3_IN) 5=DOUBLE(IBS<DBL_IBS & RSI2<DBL_RSI).
    // NDX: %b PF5.46 / streak 3.53 (2022+) / double 3.14 / IBS 3.64; SPX: streak 2.23 / double 2.19.
    // Family is CORRELATED (overlapping dip days) — ship the distinct ones (streak+double+IBS).
    int    ENTRY_MODE  = 0;
    double IBS_IN      = 0.10;
    int    STREAK_N    = 3;
    double PCTB_IN     = 0.0;
    int    RSI3_LEN    = 3;   double RSI3_IN = 15.0;
    double DBL_IBS     = 0.20; double DBL_RSI = 15.0;
    // S-2026-06-20 asym sustained-bear veto regime gate (faithful-confirmed 6/6 index×host,
    // next-open fills + 2x cost: beats close>SMA200 on net AND 2022 bucket — SMA200 amputates
    // the winning bear dip-bounces, flipping 2022 from -3..-6% to +1.3..+12.9%). REGIME_GATE=1
    // buys shallow/temporary sub-SMA dips and vetoes ONLY genuine sustained bear.
    int    REGIME_GATE = 0;   // 0 = close>SMA(TREND_SMA) (current); 1 = asym sustained-bear veto
    int    BEAR_VETO_K = 20;  // consec bars (close<SMA AND SMA falling) before a dip-buy is vetoed
    int    m_bear_below = 0;  // runtime: consecutive sustained-falling-bear bars (REGIME_GATE=1)
    // S-2026-06-20 book-level concurrent-position cap (the freq/DD-frontier DD lever):
    // shared across ALL ConnorsRSI2 instances so the correlated MR family can't stack on
    // one risk-off dip. cap=3 -> ~2.4% portfolio maxDD; cap=2 -> ~1.75%; 0 = uncapped.
    static inline int s_book_open = 0;   // shared open-position count across the family
    static inline int BOOK_CAP    = 0;   // 0 = no cap; >0 = max concurrent family positions
    // ADVERSE-PROTECTION: (S-2026-06-19, backtested verdict) NONE needed / by-design exempt.
    // Long-only daily MEAN-REVERSION gated by close>SMA(TREND_SMA) — the trend filter IS the
    // adverse protection (no dip-buying below the 200d SMA = sits out bears; faithful 2022:
    // 4 trades −454, SMA200 sat out the bear). A cold loss-cut on a mean-reverter would cut
    // exactly the dip it is paid to buy (the rejected-protection class). Exit is the SMA(SHORT)
    // bounce + MAXHOLD cap. Verdict = "trend-filter-gated, no cold cut — backtested".
    // S-2026-07-17: WIDE catastrophic stop (K x ATR20, K=2..6, wick + close-eval, worse-of gap
    // fills) also tested and REJECTED on BOTH hosts (backtest/connors_widestop_bt.cpp, baselines
    // reproduce the certified figures exactly). NAS: zero saves in 10.5y — every stop-hit lost
    // more than riding to the MR exit; worst/maxDD worse in every cell. GER: the stop MANUFACTURED
    // the new worst trade (2025-04-03 gap-through −2969 vs −1158 ridden) + extra stopped re-entries;
    // COVID-class saves < damage ~1:2. 2022 identical in every cell = the regime gate, not a price
    // stop, is the bear protection. No stop of ANY width; tail lever = BOOK_CAP / lot / gate.
    // Ref: backtest/CONNORS_WIDESTOP_FINDINGS_2026-07-17.md.
    double lot         = 1.0;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // ── REAL EXEC ROUTING (S-2026-07-22j, operator "live and actionable") ─────
    // Injected in engine_init: open -> send_live_order, close -> opposite via
    // token, gate -> ExecutionCostGuard::is_viable. shadow_mode=true or unset
    // fns = book-only (backtest TUs unaffected). Every entry/scale-in/exit
    // transition below routes through these — the LIVE flag now means ORDERS.
    using OpenFn  = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn  = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    OpenFn exec_open; CloseFn exec_close; GateFn exec_gate;
    void set_exec(OpenFn o, CloseFn c, GateFn g) {
        exec_open = std::move(o); exec_close = std::move(c); exec_gate = std::move(g);
    }

    struct Position { bool active=false; double entry_px=0, size=0; int64_t entry_ms=0; int held=0; int units=0; double mfe=0,mae=0; std::string token; bool sent=false; } pos;

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=0.0; o.tp=0.0; o.entry_ts=pos.entry_ms/1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {   // fixed-lot: re-assert config lot
        pos = Position{}; pos.active=true; pos.entry_px=ps.entry;
        // S-2026-07-18f: restore units from the saved size (size = units*lot at save time).
        // units was left at 0 here, so the scale-in average (entry*units+px)/(units+1)
        // REPLACED entry_px on the first post-restart scale-in instead of averaging
        // (live NAS 2026-07-17: entry 29214.75 -> 28769.50, $1,336 of open drawdown
        // silently erased from the displayed unrealized). Clamp 1..MAX_UNITS.
        int u = lot > 0.0 ? (int)(ps.size / lot + 0.5) : 1;
        if (u < 1) u = 1; if (MAX_UNITS >= 1 && u > MAX_UNITS) u = MAX_UNITS;
        pos.units = u; pos.size = u * lot;
        pos.entry_ms=ps.entry_ts*1000; ++s_book_open; return true;
    }
    bool has_open_position() const { return pos.active; }
    double pos_entry() const { return pos.entry_px; }
    double pos_lot()   const { return pos.size; }
    int64_t pos_entry_ts_ms() const { return pos.entry_ms; }

    void init() { m_closes.clear(); m_highs.clear(); m_lows.clear(); m_in_session=false; m_done_today=false; m_bear_below=0; pos=Position{}; }

    size_t seed_from_d1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s\n", engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        size_t n=0; long long ts; double o,h,l,c;
        while (std::getline(f, line))
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && c>0) {
                m_closes.push_back(c); m_highs.push_back(h>0?h:c); m_lows.push_back(l>0?l:c);
                if ((int)m_closes.size()>TREND_SMA+5) { m_closes.pop_front(); m_highs.pop_front(); m_lows.pop_front(); } ++n; }
        printf("[SEED] %s (%s): %zu daily closes -- ready=%d\n", engine_name.c_str(), symbol.c_str(),
               n, (int)((int)m_closes.size()>=TREND_SMA)); fflush(stdout); return n;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid<=0.0 || ask<=0.0 || ask<bid) return;
        const double mid=(bid+ask)*0.5;
        const int et=_local_hm(now_ms/1000, TZ_STD_OFF_MIN, TZ_EU_DST);
        const bool in_rth=(et>=SESS_OPEN_HM && et<SESS_CLOSE_HM);

        if (in_rth) {
            m_day_close=mid; m_have_day=true;
            if(!m_in_session){ m_in_session=true; m_done_today=false; m_day_high=mid; m_day_low=mid; }
            else { if(mid>m_day_high)m_day_high=mid; if(mid<m_day_low)m_day_low=mid; }
        }

        // act once per day, at the cash close transition
        if (!in_rth && m_in_session) {
            m_in_session=false;
            if (!m_have_day || m_day_close<=0) return;
            const double close=m_day_close;
            // 1) push today's OHLC FIRST so SMA/IBS see it (highs/lows parallel to closes)
            m_closes.push_back(close); m_highs.push_back(m_day_high); m_lows.push_back(m_day_low);
            if ((int)m_closes.size()>TREND_SMA+5) { m_closes.pop_front(); m_highs.pop_front(); m_lows.pop_front(); }
            // 1b) REGIME_GATE state — count consecutive sustained-bear bars (close<SMA AND SMA falling).
            //     Runs once per day at the close transition so it advances even on no-signal days.
            {
                const int N=(int)m_closes.size();
                if (N>=TREND_SMA+5) {
                    double sma=0;  for (int i=N-TREND_SMA;   i<N;   ++i) sma  += m_closes[i]; sma  /= TREND_SMA;
                    double sma5=0; for (int i=N-5-TREND_SMA; i<N-5; ++i) sma5 += m_closes[i]; sma5 /= TREND_SMA;
                    if (close < sma && sma < sma5) m_bear_below += 1; else m_bear_below = 0;
                } else m_bear_below = 0;
            }
            // 2) manage open: ENHANCED exit — close back above SMA(SHORT_SMA) (MR complete)
            //    OR MAXHOLD cap. SHORT_SMA<=0 -> legacy HOLD_DAYS exit.
            if (pos.active) {
                pos.held += 1;
                bool do_exit;
                if (SHORT_SMA > 0) {
                    const int N=(int)m_closes.size(); const int kk = N<SHORT_SMA?N:SHORT_SMA;
                    double ss=0; for (int i=N-kk;i<N;++i) ss+=m_closes[i]; ss/=kk;
                    do_exit = (close > ss) || (pos.held >= MAXHOLD);
                } else do_exit = (pos.held >= HOLD_DAYS);
                if (do_exit) { _close(bid>0?bid:close, now_ms, "RSI2_EXIT"); }
                else if (SCALEIN && pos.units < MAX_UNITS && (int)m_closes.size()>=TREND_SMA) {
                    // still oversold inside the uptrend -> average a unit in (Connors cumulative)
                    const int N=(int)m_closes.size();
                    double sma=0; for (int i=N-TREND_SMA;i<N;++i) sma+=m_closes[i]; sma/=TREND_SMA;
                    const bool regime_ok = (REGIME_GATE==1) ? (m_bear_below < BEAR_VETO_K) : (close>sma);
                    if (regime_ok && _signal()) {
                        const double px=ask>0?ask:close;
                        pos.entry_px=(pos.entry_px*pos.units + px)/(pos.units+1);
                        pos.units+=1; pos.size=pos.units*lot;
                        if (!shadow_mode && exec_open) {                 // real scale-in unit
                            if (!exec_gate || exec_gate(symbol, px*0.01, lot)) {
                                const std::string t = exec_open(symbol, true, lot, px);
                                if (pos.token.empty()) pos.token = t;
                                pos.sent = pos.sent || !t.empty();
                            }
                        }
                        if (verbose) printf("[%s] %s SCALE-IN unit%d avg=%.2f\n",
                            engine_name.c_str(), symbol.c_str(), pos.units, pos.entry_px);
                    }
                }
            }
            // 3) entry: uptrend + oversold-signal (ENTRY_MODE) + flat
            if (!pos.active && (int)m_closes.size()>=TREND_SMA && !m_done_today) {
                const int N=(int)m_closes.size();
                double sma=0; for (int i=N-TREND_SMA;i<N;++i) sma+=m_closes[i]; sma/=TREND_SMA;
                const double r=_rsi(RSI_LEN);
                const bool regime_ok = (REGIME_GATE==1) ? (m_bear_below < BEAR_VETO_K) : (close>sma);
                const bool cap_ok = (BOOK_CAP <= 0 || s_book_open < BOOK_CAP);
                if (regime_ok && cap_ok && _signal()) {
                    pos=Position{}; pos.active=true; pos.entry_px=ask>0?ask:close; pos.size=lot; pos.units=1; pos.entry_ms=now_ms; pos.held=0;
                    if (!shadow_mode && exec_open) {
                        if (!exec_gate || exec_gate(symbol, pos.entry_px*0.01, lot)) {
                            pos.token = exec_open(symbol, true, lot, pos.entry_px);
                            pos.sent  = !pos.token.empty();
                        }
                    }
                    m_done_today=true; ++s_book_open;
                    if (verbose) printf("[%s] %s DIP-BUY entry=%.2f rsi2=%.1f close=%.2f sma%d=%.2f%s\n",
                        engine_name.c_str(), symbol.c_str(), pos.entry_px, r, close, TREND_SMA, sma, shadow_mode?" [SHADOW]":"");
                }
            }
        }
        if (pos.active) { const double fav=bid-pos.entry_px; if(fav>pos.mfe)pos.mfe=fav; if(fav<pos.mae)pos.mae=fav; }
    }

    void force_close(double bid, double, int64_t now_ms) { if (pos.active) _close(bid, now_ms, "FORCE_CLOSE"); }

private:
    double _rsi(int n) const {
        const int N=(int)m_closes.size(); if (N<n+1) return 50.0;
        double g=0,l=0; for (int k=N-n;k<N;++k){ double ch=m_closes[k]-m_closes[k-1]; if(ch>0)g+=ch; else l+=-ch; }
        return l==0?100.0:100.0-100.0/(1.0+g/l);
    }
    double _ibs() const {   // today's internal bar strength = (c-l)/(h-l)
        const int N=(int)m_closes.size(); if (N<1) return 0.5;
        const double h=m_highs.back(), l=m_lows.back(), c=m_closes.back();
        return (h<=l)?0.5:(c-l)/(h-l);
    }
    int _downstreak() const {   // consecutive down closes ending today
        const int N=(int)m_closes.size(); int s=0;
        for (int k=N-1;k>0 && m_closes[k]<m_closes[k-1];--k) ++s; return s;
    }
    double _pctb(int n) const { // %b on close Bollinger(n,2)
        const int N=(int)m_closes.size(); if (N<n) return 0.5;
        double m=0; for(int k=N-n;k<N;++k) m+=m_closes[k]; m/=n;
        double sd=0; for(int k=N-n;k<N;++k){double d=m_closes[k]-m; sd+=d*d;} sd=std::sqrt(sd/n);
        const double lo=m-2*sd, hi=m+2*sd; return (hi<=lo)?0.5:(m_closes.back()-lo)/(hi-lo);
    }
    // ENTRY_MODE dispatcher — the oversold-dip-in-uptrend family (mr_hunt.cpp).
    bool _signal() const {
        switch (ENTRY_MODE) {
            case 1:  return _ibs() < IBS_IN;                              // IBS
            case 2:  return _downstreak() >= STREAK_N;                    // STREAK
            case 3:  return _pctb(20) < PCTB_IN;                          // %b
            case 4:  return _rsi(RSI3_LEN) < RSI3_IN;                     // RSI3
            case 5:  return _ibs() < DBL_IBS && _rsi(RSI_LEN) < DBL_RSI;  // DOUBLE
            default: return _rsi(RSI_LEN) < RSI_IN;                       // 0 = RSI2
        }
    }
    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;
        if (pos.sent && exec_close)            // real broker close (full size incl scale-ins)
            exec_close(symbol, true, pos.size, exit_px, pos.token);
        if (s_book_open > 0) --s_book_open;   // release the book-cap slot
        const double pnl=(exit_px-pos.entry_px)*pos.size;
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.engine=engine_name; tr.side="LONG";
        tr.entryPrice=pos.entry_px; tr.exitPrice=exit_px; tr.sl=0; tr.tp=0;
        tr.size=pos.size; tr.pnl=pnl; tr.mfe=pos.mfe*pos.size; tr.mae=std::fabs(pos.mae)*pos.size;
        tr.entryTs=pos.entry_ms/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason; tr.shadow=shadow_mode;
        if (on_trade_record) on_trade_record(tr);
        if (verbose) printf("[%s] CLOSE %s exit=%.2f pnl=%.2f\n", engine_name.c_str(), reason, exit_px, pnl);
        pos=Position{};
    }
    // Local hh:mm (HHMM int) at a given UTC sec, with std offset + DST rules.
    // eu_dst=false -> US rules (2nd-Sun-Mar 02:00 .. 1st-Sun-Nov); true -> EU (last-Sun-Mar..last-Sun-Oct).
    // Day-granularity DST (the 1-hour intraday transition moment is immaterial for a daily-close engine).
    static int _local_hm(int64_t sec, int std_off_min, bool eu_dst) {
        std::time_t t=(std::time_t)sec; std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g,&t);
#else
        gmtime_r(&t,&g);
#endif
        const int m=g.tm_mon+1, dd=g.tm_mday, wd=g.tm_wday; bool dst;
        if (!eu_dst) { // US
            if (m<3||m>11) dst=false; else if (m>3&&m<11) dst=true;
            else { const int fd=(((wd-((dd-1)%7))%7)+7)%7;
                   if (m==3){int s=1+((7-fd)%7)+7; dst=dd>=s;} else {int s=1+((7-fd)%7); dst=dd<s;} }
        } else {       // EU: last Sunday of Mar .. last Sunday of Oct
            if (m<3||m>10) dst=false; else if (m>3&&m<10) dst=true;
            else { const int L=31; const int wdL=(((wd+(L-dd))%7)+7)%7; const int lastSun=L-wdL;
                   dst = (m==3) ? (dd>=lastSun) : (dd<lastSun); }
        }
        int lm=((g.tm_hour*60+g.tm_min)+std_off_min+(dst?60:0)); lm=((lm%1440)+1440)%1440;
        return (lm/60)*100+lm%60;
    }
    std::deque<double> m_closes, m_highs, m_lows;
    double m_day_close=0, m_day_high=0, m_day_low=0;
    bool m_have_day=false, m_in_session=false, m_done_today=false;
};

}  // namespace omega
