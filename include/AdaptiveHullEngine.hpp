#pragma once
//  ADVERSE-PROTECTION: trail-only by design (3x ATR initial stop KATR=3.0 + HMA-line trailing stop, no LOSS_CUT/BE ratchet); faithful BT on record (backtest/adaptive_hull.cpp 2026-06-05: XAU 60m PF1.52 Sh1.61, GER40 60m PF2.03, both halves +, cost-robust 3x) -- per swing-protection sweep 2026-06-17 a cold cut lowers net on trend/trail engines; verdict = keep trail-only (backfill S-2026-06-24n)
// ─────────────────────────────────────────────────────────────────────────────
// AdaptiveHullEngine — Ehlers Phase-Accumulation dominant cycle -> adaptive Hull
// MA -> slope-flip trend follower (the "PA Adaptive Hull Parabolic" idea),
// LONG-ONLY, trail-at-HMA exit, ATR initial stop.
//
//   BACKTEST (2026-06-05, backtest/adaptive_hull.cpp; memory omega-pa-adaptive-
//   hull-eval): the video config (gold/1m/flip/fast) is whipsaw-dead. The real
//   edge = LONG-ONLY + slow period (pmul2) + trail exit on 60m:
//     XAU 60m all-session: PF 1.52, Sharpe 1.61, both halves +, cost-robust 3x.
//     GER40 60m EU-session(07-16 UTC): PF 2.03, Sharpe 1.94.
//   Bull-biased (shorts drag); ~0.6 correlated with our trend book = a partial
//   diversifier, not a duplicate. shadow_mode default ON.
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

namespace omega {

class AdaptiveHullEngine {
public:
    std::string symbol      = "XAUUSD";
    std::string engine_name = "AdaptiveHull";
    int    TF_SEC      = 3600;   // 60-minute bars
    double PMUL        = 2.0;    // slow the Ehlers cycle period (peak; 2.5 dies, 1.5 weaker)
    double KATR        = 3.0;    // initial ATR stop multiple
    int    SESS0       = -1;     // entry UTC-hour window [SESS0,SESS1) (-1 = all). GER40: 7..16
    int    SESS1       = -1;
    double lot         = 1.0;
    bool   enabled     = true;
    bool   shadow_mode = true;
    bool   verbose     = false;

    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    struct Position { bool active=false; double entry_px=0, stop_px=0, trail_px=0, size=0;
                      int64_t entry_ms=0; double mfe=0, mae=0; } pos;

    // FIXED-LOT engine: re-assert configured lot on restore (GoldOversold lesson).
    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side="LONG"; o.size=pos.size; o.entry=pos.entry_px;
        o.sl=pos.stop_px; o.tp=0.0; o.entry_ts=pos.entry_ms/1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos = Position{}; pos.active=true; pos.entry_px=ps.entry; pos.stop_px=ps.sl;
        pos.trail_px=ps.sl; pos.size=lot; pos.entry_ms=ps.entry_ts*1000; return true;
    }
    bool has_open_position() const { return pos.active; }
    double pos_entry() const { return pos.entry_px; }
    double pos_lot()   const { return pos.size; }
    double pos_stop()  const { return pos.stop_px; }
    int64_t pos_entry_ts_ms() const { return pos.entry_ms; }

    void init() { m_px.clear(); m_sm.clear(); m_dt.clear(); m_q1.clear(); m_i1.clear();
        m_ph.clear(); m_close.clear(); m_raw.clear(); m_hma.clear();
        m_cur_bucket=-1; m_o=m_h=m_l=m_c=0; m_per=15; m_atr=0; m_atr_n=0; pos=Position{}; }

    // Warm-seed from a bar CSV (ts,o,h,l,c | bar_start_ms,...). Replays bars while
    // disabled so the cycle/HMA/ATR are hot on day one.
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
        printf("[SEED] %s (%s): %zu bars replayed -- per=%.1f hma=%zu atr=%.2f\n",
               engine_name.c_str(), symbol.c_str(), n, m_per, m_hma.size(), m_atr);
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
        // intrabar trail/stop management on the live tick
        if (pos.active) {
            const double fav=bid-pos.entry_px; if(fav>pos.mfe)pos.mfe=fav; if(fav<pos.mae)pos.mae=fav;
            if (bid<=pos.stop_px) { _close(bid, now_ms, "STOP"); return; }
            if (pos.trail_px>0 && bid<=pos.trail_px && pos.mfe>0) { _close(bid, now_ms, "TRAIL"); return; }
        }
    }

    void force_close(double bid, double, int64_t now_ms) { if (pos.active) _close(bid, now_ms, "FORCE_CLOSE"); }

private:
    static double _wma(const std::deque<double>& v, int len) {
        int n=(int)v.size(); if(len<1)len=1; if(len>n)len=n; double num=0,den=0;
        for(int k=0;k<len;++k){ double w=len-k; num+=w*v[n-1-k]; den+=w; } return den>0?num/den:0; }
    static double _at(const std::deque<double>& v, int back) { int n=(int)v.size(); return (back<n)?v[n-1-back]:(n?v[0]:0); }

    // Finalize one closed TF bar: update Ehlers cycle, HMA, ATR, run the signal.
    void _finalize_bar(int64_t ts, double o, double h, double l, double c) {
        const double price=(h+l)*0.5;
        m_px.push_back(price);
        // ATR(14)
        if (m_prev_close>0) { double trv=std::max(h-l,std::max(std::fabs(h-m_prev_close),std::fabs(l-m_prev_close)));
            if(m_atr_n<14){m_atr+=trv;if(++m_atr_n==14)m_atr/=14;}else m_atr=(m_atr*13+trv)/14; }
        m_prev_close=c;
        // Ehlers smooth/detrender/Q1/I1
        double sm = (m_px.size()>=4)? (4*_at(m_px,0)+3*_at(m_px,1)+2*_at(m_px,2)+_at(m_px,3))/10.0 : price;
        m_sm.push_back(sm);
        const double mul=0.075*m_per+0.54;
        double dt = (m_sm.size()>=7)? (.0962*_at(m_sm,0)+.5769*_at(m_sm,2)-.5769*_at(m_sm,4)-.0962*_at(m_sm,6))*mul : 0;
        m_dt.push_back(dt);
        double q1 = (m_dt.size()>=7)? (.0962*_at(m_dt,0)+.5769*_at(m_dt,2)-.5769*_at(m_dt,4)-.0962*_at(m_dt,6))*mul : 0;
        double i1 = (m_dt.size()>=4)? _at(m_dt,3) : 0;
        m_q1.push_back(q1); m_i1.push_back(i1);
        constexpr double kPi=3.14159265358979323846;   // MSVC lacks M_PI without _USE_MATH_DEFINES
        double ph=0; if(std::fabs(i1)>1e-9) ph=std::atan(q1/i1)/(kPi/180.0);
        if(i1<0&&q1>0)ph=180-ph; else if(i1<0&&q1<0)ph=180+ph; else if(i1>0&&q1<0)ph=-ph;
        m_ph.push_back(ph);
        // phase accumulation -> period
        double sum=0; int cnt=0; double per=m_per;
        int H=(int)m_ph.size();
        for(int k=0;k+1<H && k<40;++k){ double p1=_at(m_ph,k+1), p0=_at(m_ph,k); double pk=p1-p0;
            if(p1<90&&p0>270)pk=360+p1-p0; if(pk<1)pk=1; if(pk>60)pk=60; sum+=pk; cnt++; if(sum>360){per=cnt;break;} }
        if(per<6)per=6; if(per>50)per=50; m_per=0.2*per+0.8*m_per;
        // adaptive HMA: hma=wma(2*wma(c,p/2)-wma(c,p), sqrt(p)), p=period*PMUL
        m_close.push_back(c);
        int p=std::max(2,(int)std::lround(m_per*PMUL)); int ph2=std::max(1,p/2); int ps=std::max(1,(int)std::lround(std::sqrt((double)p)));
        double raw=2*_wma(m_close,ph2)-_wma(m_close,p); m_raw.push_back(raw);
        double hma=_wma(m_raw,ps); m_hma.push_back(hma);
        _bound();
        // signal on the just-closed bar
        if((int)m_hma.size()<5 || m_atr<=0) return;
        const double hcur=_at(m_hma,0), hp1=_at(m_hma,1), hp2=_at(m_hma,2);
        const int slope=(hcur>hp1)?1:((hcur<hp1)?-1:0), pslope=(hp1>hp2)?1:((hp1<hp2)?-1:0);
        const bool flipUp = slope>0 && pslope<=0;
        // manage on bar close: update trail to HMA line
        if (pos.active) { if (hcur>pos.trail_px) pos.trail_px=hcur;
            if (c<pos.trail_px && pos.mfe>0) { _close(c, ts*1000+TF_SEC*1000, "TRAIL"); return; } }
        // entry: long-only on flip-up, candle-confirm, in session.
        // enabled-guard: seed_from_csv replays bars with enabled=false -> no
        // entries on history (warm-seed mandate).
        if (enabled && !pos.active && flipUp && c>o) {
            if (SESS0>=0) { int hr=(int)(((ts%86400)+86400)%86400)/3600;
                bool in=(SESS0<=SESS1)?(hr>=SESS0&&hr<SESS1):(hr>=SESS0||hr<SESS1); if(!in) return; }
            const double stop=c-KATR*m_atr; if (c-stop<0.1*m_atr) return;
            // cost gate: no-TP runner -> use stop distance as the gross proxy (GVB precedent)
            if (!ExecutionCostGuard::is_viable(symbol.c_str(), m_spread, c-stop, lot, 1.5)) return;
            pos=Position{}; pos.active=true; pos.entry_px=c; pos.stop_px=stop; pos.trail_px=hcur; pos.size=lot; pos.entry_ms=ts*1000+TF_SEC*1000;
            if (verbose) printf("[%s] %s LONG entry=%.2f stop=%.2f hma=%.2f per=%.1f%s\n",
                engine_name.c_str(), symbol.c_str(), c, stop, hcur, m_per, shadow_mode?" [SHADOW]":"");
        }
    }
    void _bound(){ const size_t CAP=160;
        auto cap=[&](std::deque<double>&d){ while(d.size()>CAP) d.pop_front(); };
        cap(m_px);cap(m_sm);cap(m_dt);cap(m_q1);cap(m_i1);cap(m_ph);cap(m_close);cap(m_raw);cap(m_hma); }
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
    std::deque<double> m_px,m_sm,m_dt,m_q1,m_i1,m_ph,m_close,m_raw,m_hma;
    int64_t m_cur_bucket=-1; double m_o=0,m_h=0,m_l=0,m_c=0;
    double m_spread=0;   // last live spread (entry cost gate)
    double m_per=15, m_atr=0, m_prev_close=0; int m_atr_n=0;
};

}  // namespace omega
