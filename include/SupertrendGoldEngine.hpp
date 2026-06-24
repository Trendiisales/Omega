#pragma once
//  ADVERSE-PROTECTION: trail-only by design -- the Supertrend(10,3) line IS the trailing stop (pos.stop_px=m_st_line; exits ST_FLIP/ST_STOP), NO BE/TP/time-stop (swing-protection sweep 2026-06-17 showed a cold cut lowers net on trend/trail engines); only a bar-replay header PF (adaptive_hull.cpp 2026-06-05), no faithful backtest on record -- verdict owed before re-enable (backfill S-2026-06-24n)
// ─────────────────────────────────────────────────────────────────────────────
// SupertrendGoldEngine — Supertrend(10,3) trend follower on XAUUSD 60m, LONG-ONLY,
// gated by an EMA-regime filter. The Supertrend line IS the trailing stop; exit
// is the flip-down. NO break-even, NO fixed TP, NO time-stop (all proven to kill
// trend edge — see memory). The EMA filter is the chop protection.
//
//   BACKTEST (2026-06-05, backtest/adaptive_hull.cpp MODE=super; memory omega-pa-
//   adaptive-hull-eval): XAU 60m long-only, Supertrend(10,3), flip exit, +EMA100
//   regime filter = PF 2.04, Sharpe 2.49, both halves + (2.14/2.00), maxDD 211.
//   Cost-robust: PF 2.04@0.5pt -> 1.89@2pt, both halves+. Plateau STMULT 3-4 x
//   STLEN 10-14. Gold-specific (other symbols weak with the EMA pair). The
//   strongest single-indicator result of the session. shadow_mode default ON.
// ─────────────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"
#include "RegimeState.hpp"       // 2026-06-21: macro-hostile long-block (BearProtect coverage)

namespace omega {

class SupertrendGoldEngine {
public:
    std::string symbol      = "XAUUSD";
    std::string engine_name = "SupertrendGold";
    int    TF_SEC      = 3600;   // 60-minute bars
    int    ST_LEN      = 10;     // Supertrend ATR period
    double ST_MULT     = 3.0;    // Supertrend ATR multiple
    int    EMA_LEN     = 100;    // regime filter: long only if close > EMA(N)
    double lot         = 0.01;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    struct Position { bool active=false; double entry_px=0, stop_px=0, size=0; int64_t entry_ms=0; double mfe=0,mae=0; } pos;

    // FIXED-LOT restore (GoldOversold lesson). stop = the live Supertrend line.
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=pos.stop_px; o.tp=0.0; o.entry_ts=pos.entry_ms/1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos=Position{}; pos.active=true; pos.entry_px=ps.entry; pos.stop_px=ps.sl;
        pos.size=lot; pos.entry_ms=ps.entry_ts*1000; return true;
    }
    bool has_open_position() const { return pos.active; }
    double pos_entry() const { return pos.entry_px; }
    double pos_lot()   const { return pos.size; }
    double pos_stop()  const { return pos.stop_px; }
    int64_t pos_entry_ts_ms() const { return pos.entry_ms; }

    void init() { m_close.clear(); m_cur_bucket=-1; m_o=m_h=m_l=m_c=0;
        m_atr=0; m_atr_n=0; m_prev_close=0; m_pFU=0; m_pFL=0; m_pST=0; m_st_init=false;
        m_dir=1; m_st_line=0; m_ema=0; m_ema_init=false; pos=Position{}; }

    size_t seed_from_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s\n", engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        const bool save_en=enabled; enabled=false;
        size_t n=0; long long ts; double o,h,l,c;
        while (std::getline(f, line))
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && h>=l) {
                int64_t tsec=(ts>100000000000LL)?ts/1000:ts; _finalize_bar(tsec,o,h,l,c); ++n; }
        enabled=save_en;
        printf("[SEED] %s (%s): %zu bars -- dir=%d st=%.2f ema=%.2f atr=%.2f\n",
               engine_name.c_str(), symbol.c_str(), n, m_dir, m_st_line, m_ema, m_atr);
        fflush(stdout); return n;
    }

    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid<=0.0 || ask<=0.0 || ask<bid) return;
        const double mid=(bid+ask)*0.5; const int64_t sec=now_ms/1000;
        m_spread = ask - bid;                       // last live spread for the entry cost gate
        const int64_t bucket=(sec/TF_SEC)*TF_SEC;
        if (m_cur_bucket<0) { m_cur_bucket=bucket; m_o=m_h=m_l=m_c=mid; }
        else if (bucket!=m_cur_bucket) { _finalize_bar(m_cur_bucket,m_o,m_h,m_l,m_c); m_cur_bucket=bucket; m_o=m_h=m_l=m_c=mid; }
        else { m_h=std::max(m_h,mid); m_l=std::min(m_l,mid); m_c=mid; }
        // intrabar protection: the Supertrend line is the hard stop
        if (pos.active) {
            const double fav=bid-pos.entry_px; if(fav>pos.mfe)pos.mfe=fav; if(fav<pos.mae)pos.mae=fav;
            if (pos.stop_px>0 && bid<=pos.stop_px) { _close(bid, now_ms, "ST_STOP"); return; }
        }
    }
    void force_close(double bid, double, int64_t now_ms) { if (pos.active) _close(bid, now_ms, "FORCE_CLOSE"); }

private:
    void _finalize_bar(int64_t ts, double o, double h, double l, double c) {
        // ATR(ST_LEN)
        if (m_prev_close>0) { double trv=std::max(h-l,std::max(std::fabs(h-m_prev_close),std::fabs(l-m_prev_close)));
            if(m_atr_n<ST_LEN){m_atr+=trv;if(++m_atr_n==ST_LEN)m_atr/=ST_LEN;}else m_atr=(m_atr*(ST_LEN-1)+trv)/ST_LEN; }
        // EMA regime
        if(!m_ema_init){m_ema=c;m_ema_init=true;} else { double k=2.0/(EMA_LEN+1); m_ema=c*k+m_ema*(1-k); }
        // canonical Supertrend
        const double hl2=(h+l)*0.5, bU=hl2+ST_MULT*m_atr, bL=hl2-ST_MULT*m_atr;
        int prevDir=m_dir;
        if(!m_st_init){ m_pFU=bU; m_pFL=bL; m_pST=bL; m_dir=1; m_st_line=bL; m_st_init=true; m_prev_close=c; return; }
        double fU=(bU<m_pFU||m_prev_close>m_pFU)?bU:m_pFU;
        double fL=(bL>m_pFL||m_prev_close<m_pFL)?bL:m_pFL;
        double st=(m_pST==m_pFU)?((c<=fU)?fU:fL):((c>=fL)?fL:fU);
        m_dir = c>st ? 1 : -1; m_st_line=st; m_pFU=fU; m_pFL=fL; m_pST=st; m_prev_close=c;
        const bool flipUp = m_dir>0 && prevDir<=0, flipDn = m_dir<0 && prevDir>=0;
        const int64_t close_ms = ts*1000 + (int64_t)TF_SEC*1000;
        // exit: Supertrend flip-down (the trend ended) — NO BE/TP/time-stop
        if (pos.active) { pos.stop_px=m_st_line;            // trail the stop to the ST line
            if (flipDn) { _close(c, close_ms, "ST_FLIP"); return; } }
        // entry: flip-up + uptrend regime (close>EMA). enabled-guard for seed.
        if (enabled && !pos.active && flipUp && c>m_ema) {
            // 2026-06-21: macro-hostile long-block (LONG-ONLY engine). Fail-safe false.
            if (omega::gold_regime().long_blocked()) return;
            // cost gate: no-TP runner -> use ST-line stop distance as gross proxy
            if (c-m_st_line<=0 || !ExecutionCostGuard::is_viable(symbol.c_str(), m_spread, c-m_st_line, lot, 1.5)) return;
            pos=Position{}; pos.active=true; pos.entry_px=c; pos.stop_px=m_st_line; pos.size=lot; pos.entry_ms=close_ms;
            if (verbose) printf("[%s] %s LONG entry=%.2f st=%.2f ema=%.2f%s\n",
                engine_name.c_str(), symbol.c_str(), c, m_st_line, m_ema, shadow_mode?" [SHADOW]":"");
        }
    }
    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if(!pos.active) return; const double pnl=(exit_px-pos.entry_px)*pos.size;
        omega::TradeRecord tr{}; tr.symbol=symbol; tr.engine=engine_name; tr.side="LONG";
        tr.entryPrice=pos.entry_px; tr.exitPrice=exit_px; tr.sl=pos.stop_px; tr.tp=0;
        tr.size=pos.size; tr.pnl=pnl; tr.mfe=pos.mfe*pos.size; tr.mae=std::fabs(pos.mae)*pos.size;
        tr.entryTs=pos.entry_ms/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason; tr.shadow=shadow_mode;
        if(on_trade_record) on_trade_record(tr);
        if(verbose) printf("[%s] CLOSE %s exit=%.2f pnl=%.2f\n", engine_name.c_str(), reason, exit_px, pnl);
        pos=Position{};
    }
    std::deque<double> m_close;
    int64_t m_cur_bucket=-1; double m_o=0,m_h=0,m_l=0,m_c=0;
    double m_spread=0;   // last live spread (entry cost gate)
    double m_atr=0, m_prev_close=0; int m_atr_n=0;
    double m_pFU=0,m_pFL=0,m_pST=0,m_st_line=0; bool m_st_init=false; int m_dir=1;
    double m_ema=0; bool m_ema_init=false;
};

}  // namespace omega
