#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OvernightDriftEngine — the index "night effect", trend-gated.
//
//   Long at the US cash CLOSE → flat at the next cash OPEN, ONLY when the index
//   is in an uptrend (last close > SMA(N)). Captures the documented overnight
//   drift: index returns accrue overnight, intraday is ~flat. The trend gate is
//   load-bearing — it lifts Sharpe and keeps us FLAT overnight in downtrends
//   (built-in bear protection).
//
//   BACKTEST (2026-06-04, see memory omega-overnight-drift-index-edge):
//     NDX cash >SMA20 net Sharpe 1.62 (both halves +); NQ FUTURE >SMA20
//     Sharpe ~1.0 (no financing on futures, cost-robust to 4pt). Works on cash
//     AND future — orthogonal to every intraday engine. shadow_mode default ON.
//
//   Wired to the live NAS100 tick feed for shadow validation. The preferred live
//   vehicle is an IBKR MNQ/NQ future (no overnight financing).
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

class OvernightDriftEngine {
public:
    std::string symbol      = "NAS100";
    std::string engine_name = "OvernightDrift";
    int    SMA_LEN     = 20;     // trend gate: hold overnight only if close > SMA(N)
    double lot         = 1.0;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    struct Position { bool active=false; double entry_px=0, size=0; int64_t entry_ms=0; double mfe=0,mae=0; } pos;

    // FIXED-LOT engine: re-assert configured lot on restore, never trust snapshot
    // size (see GoldOversold $1589 lot-restore bug, 2026-06-04).
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=0.0; o.tp=0.0; o.entry_ts=pos.entry_ms/1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos = Position{}; pos.active=true; pos.entry_px=ps.entry; pos.size=lot;
        pos.entry_ms=ps.entry_ts*1000; return true;
    }
    bool has_open_position() const { return pos.active; }
    double pos_entry() const { return pos.entry_px; }
    double pos_lot()   const { return pos.size; }
    int64_t pos_entry_ts_ms() const { return pos.entry_ms; }

    void init() {
        m_closes.clear(); m_day=-1; m_day_close=0; m_have_day=false;
        m_in_session=false; m_entered_tonight=false; pos=Position{};
    }

    // Warm-seed SMA from a daily CSV (ts,o,h,l,c | bar_start_ms,o,h,l,c).
    size_t seed_from_d1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s\n", engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        size_t n=0; long long ts; double o,h,l,c;
        while (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && c>0) {
                m_closes.push_back(c); if ((int)m_closes.size()>SMA_LEN+5) m_closes.pop_front(); ++n;
            }
        }
        printf("[SEED] %s (%s): %zu daily closes -- SMA ready=%d\n",
               engine_name.c_str(), symbol.c_str(), n, (int)((int)m_closes.size()>=SMA_LEN));
        fflush(stdout); return n;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid<=0.0 || ask<=0.0 || ask<bid) return;
        const double mid=(bid+ask)*0.5;
        const int64_t sec=now_ms/1000;
        const int et=_et_hm(sec);                       // US-Eastern hh*100+mm (DST-aware)
        const bool in_rth = (et>=930 && et<1600);
        const int64_t d = sec/86400;

        // ---- session-open transition: exit the overnight position, start fresh day ----
        if (in_rth && !m_in_session) {
            if (pos.active) _close(bid, now_ms, "CASH_OPEN");   // flat at the open
            m_in_session=true; m_entered_tonight=false;
            m_day=d; m_day_close=mid; m_have_day=true;
        }
        if (in_rth) { m_day_close=mid; m_have_day=true; }       // track running close

        // ---- session-close transition: finalize day, arm + take the overnight long ----
        if (!in_rth && m_in_session) {
            m_in_session=false;
            if (m_have_day && m_day_close>0) {                  // finalize today's close
                m_closes.push_back(m_day_close);
                if ((int)m_closes.size()>SMA_LEN+5) m_closes.pop_front();
            }
            // enter overnight long if trend gate passes
            if (!pos.active && !m_entered_tonight && (int)m_closes.size()>=SMA_LEN) {
                double sma=0; for (int i=(int)m_closes.size()-SMA_LEN; i<(int)m_closes.size(); ++i) sma+=m_closes[i];
                sma/=SMA_LEN;
                if (m_day_close > sma) {                        // uptrend -> hold overnight
                    pos=Position{}; pos.active=true; pos.entry_px=ask; pos.size=lot; pos.entry_ms=now_ms;
                    m_entered_tonight=true;
                    if (verbose) printf("[%s] %s OVERNIGHT LONG entry=%.2f close=%.2f sma=%.2f%s\n",
                        engine_name.c_str(), symbol.c_str(), ask, m_day_close, sma, shadow_mode?" [SHADOW]":"");
                }
            }
        }
        // track MFE/MAE while held
        if (pos.active) { const double fav=bid-pos.entry_px; if(fav>pos.mfe)pos.mfe=fav; if(fav<pos.mae)pos.mae=fav; }
    }

    void force_close(double bid, double /*ask*/, int64_t now_ms) { if (pos.active) _close(bid, now_ms, "FORCE_CLOSE"); }

private:
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
    // US-Eastern hh*100+mm with DST (2nd Sun Mar 07:00 UTC -> 1st Sun Nov 06:00 UTC).
    static int _et_hm(int64_t sec) {
        std::time_t t=(std::time_t)sec; std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g,&t);
#else
        gmtime_r(&t,&g);
#endif
        const int m=g.tm_mon+1, dd=g.tm_mday, wd=g.tm_wday;
        bool dst;
        if (m<3||m>11) dst=false;
        else if (m>3&&m<11) dst=true;
        else { const int firstdow=( (wd - ((dd-1)%7)) %7 +7)%7;        // dow of the 1st of this month
               if (m==3){ int sun=1+((7-firstdow)%7)+7; dst=dd>=sun; } // 2nd Sunday
               else     { int sun=1+((7-firstdow)%7);    dst=dd<sun; } // before 1st Sunday
        }
        int off = dst? -4 : -5;
        int etmin = ((g.tm_hour*60+g.tm_min) + off*60); etmin=((etmin%1440)+1440)%1440;
        return (etmin/60)*100 + etmin%60;
    }
    std::deque<double> m_closes;
    int64_t m_day=-1; double m_day_close=0; bool m_have_day=false;
    bool m_in_session=false, m_entered_tonight=false;
};

}  // namespace omega
