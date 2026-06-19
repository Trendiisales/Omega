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
    // ADVERSE-PROTECTION: (S-2026-06-19, backtested verdict) NONE needed / by-design exempt.
    // Long-only daily MEAN-REVERSION gated by close>SMA(TREND_SMA) — the trend filter IS the
    // adverse protection (no dip-buying below the 200d SMA = sits out bears; faithful 2022:
    // 4 trades −454, SMA200 sat out the bear). A cold loss-cut on a mean-reverter would cut
    // exactly the dip it is paid to buy (the rejected-protection class). Exit is the SMA(SHORT)
    // bounce + MAXHOLD cap. Verdict = "trend-filter-gated, no cold cut — backtested".
    double lot         = 1.0;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    struct Position { bool active=false; double entry_px=0, size=0; int64_t entry_ms=0; int held=0; double mfe=0,mae=0; } pos;

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=0.0; o.tp=0.0; o.entry_ts=pos.entry_ms/1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {   // fixed-lot: re-assert config lot
        pos = Position{}; pos.active=true; pos.entry_px=ps.entry; pos.size=lot;
        pos.entry_ms=ps.entry_ts*1000; return true;
    }
    bool has_open_position() const { return pos.active; }
    double pos_entry() const { return pos.entry_px; }
    double pos_lot()   const { return pos.size; }
    int64_t pos_entry_ts_ms() const { return pos.entry_ms; }

    void init() { m_closes.clear(); m_in_session=false; m_done_today=false; pos=Position{}; }

    size_t seed_from_d1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s\n", engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        size_t n=0; long long ts; double o,h,l,c;
        while (std::getline(f, line))
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && c>0) {
                m_closes.push_back(c); if ((int)m_closes.size()>TREND_SMA+5) m_closes.pop_front(); ++n; }
        printf("[SEED] %s (%s): %zu daily closes -- ready=%d\n", engine_name.c_str(), symbol.c_str(),
               n, (int)((int)m_closes.size()>=TREND_SMA)); fflush(stdout); return n;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid<=0.0 || ask<=0.0 || ask<bid) return;
        const double mid=(bid+ask)*0.5;
        const int et=_et_hm(now_ms/1000);
        const bool in_rth=(et>=930 && et<1600);

        if (in_rth) { m_day_close=mid; m_have_day=true; if(!m_in_session){m_in_session=true; m_done_today=false;} }

        // act once per day, at the cash close transition
        if (!in_rth && m_in_session) {
            m_in_session=false;
            if (!m_have_day || m_day_close<=0) return;
            const double close=m_day_close;
            // 1) push today's close FIRST so the SMA(SHORT_SMA) exit sees it
            m_closes.push_back(close); if ((int)m_closes.size()>TREND_SMA+5) m_closes.pop_front();
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
                if (do_exit) _close(bid>0?bid:close, now_ms, "RSI2_EXIT");
            }
            // 3) entry: uptrend + deep-oversold + flat
            if (!pos.active && (int)m_closes.size()>=TREND_SMA && !m_done_today) {
                const int N=(int)m_closes.size();
                double sma=0; for (int i=N-TREND_SMA;i<N;++i) sma+=m_closes[i]; sma/=TREND_SMA;
                const double r=_rsi(RSI_LEN);
                if (close>sma && r<RSI_IN) {
                    pos=Position{}; pos.active=true; pos.entry_px=ask>0?ask:close; pos.size=lot; pos.entry_ms=now_ms; pos.held=0;
                    m_done_today=true;
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
    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;
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
    static int _et_hm(int64_t sec) {
        std::time_t t=(std::time_t)sec; std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g,&t);
#else
        gmtime_r(&t,&g);
#endif
        const int m=g.tm_mon+1, dd=g.tm_mday, wd=g.tm_wday; bool dst;
        if (m<3||m>11) dst=false; else if (m>3&&m<11) dst=true;
        else { const int fd=(((wd-((dd-1)%7))%7)+7)%7;
               if (m==3){int s=1+((7-fd)%7)+7; dst=dd>=s;} else {int s=1+((7-fd)%7); dst=dd<s;} }
        int etmin=((g.tm_hour*60+g.tm_min)+(dst?-4:-5)*60); etmin=((etmin%1440)+1440)%1440;
        return (etmin/60)*100+etmin%60;
    }
    std::deque<double> m_closes;
    double m_day_close=0; bool m_have_day=false, m_in_session=false, m_done_today=false;
};

}  // namespace omega
