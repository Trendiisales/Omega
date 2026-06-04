#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FvgContinuationEngine — ICT "continuation model", mechanized + backtest-found.
//
//   Thesis: when price displaces toward an UNTAPPED draw-on-liquidity (DOL) and
//   leaves a fresh higher-timeframe fair-value gap (FVG) in that direction, a
//   retrace into the gap continues toward the DOL more often than it reverses.
//
//   Steps (all objective):
//     1. DOL  = nearest UNTAPPED level in the displacement direction = prior-day
//        high/low or a recent 15m swing extreme, within MAX_DOL_ATR of price.
//     2. HTF FVG (15m): 3-candle gap (bull: high[i-2] < low[i]) of size
//        >= MIN_GAP_ATR, fresh (<= MAX_FVG_AGE bars old) and pointing at the DOL.
//     3. Entry = first retrace (mitigation) back into the gap zone; fill at the
//        near edge. Stop = structural (just beyond the gap's far edge).
//        Target = the DOL. Hold to target/stop (NO break-even, NO fixed TP —
//        BE was proven to destroy this edge).
//
//   Session: NY killzone (13:30–15:00 UTC) for ENTRIES; positions may hold past.
//
//   BACKTEST (2026-06-04, backtest/fvg_core.cpp; see memory
//   omega-fvg-continuation-nas-edge): NAS100/NQ only — PF 1.65–1.88, WR ~50%,
//   both halves +, 3×-cost-robust, 9/9 param-plateau, cross-validated on two
//   independent NAS datasets. NOT SPX (PF 0.79–0.94), NOT gold, NOT 5m HTF.
//   CAVEAT: validated only across the 2025–26 tech-bull regime (no bear tape).
//   => shadow_mode=true by default. Do NOT live-size until shadow + a bear/chop
//   stretch confirm. Bidirectional (long+short) but long is where the bull data
//   lives; shorts carry the same logic, unproven out-of-bull.
// ─────────────────────────────────────────────────────────────────────────────
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
#include "OpenPositionRegistry.hpp"   // PositionSnapshot (persist)
#include "OmegaCostGuard.hpp"         // ExecutionCostGuard::is_viable (entry gate)

namespace omega {

class FvgContinuationEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol      = "NAS100";
    std::string engine_name = "FvgContinuation";

    // ── Config (backtest-found defaults; NAS100 15m NY killzone) ─────────────
    int    HTF_SEC       = 900;     // 15-minute HTF FVG bars
    int    SESS_OPEN_HM  = 1330;    // NY killzone open  (UTC hhmm)
    int    SESS_CLOSE_HM = 1500;    // NY killzone close (UTC hhmm)
    double MIN_GAP_ATR   = 1.0;     // FVG size floor (ATR units) — displacement quality
    double MAX_DOL_ATR   = 3.0;     // DOL must be within this many ATR (reachable)
    int    MAX_FVG_AGE   = 8;       // FVG must be this fresh (HTF bars) at entry
    int    FVG_TTL       = 24;      // FVG expires after this many HTF bars
    double STOP_BUF_ATR  = 0.10;    // structural stop sits this far beyond the gap far edge
    int    ATR_LEN       = 14;      // HTF ATR lookback
    int    SWING_LB      = 48;      // HTF bars for the swing-high/low DOL
    double MIN_RR        = 1.0;     // require DOL >= this many R away
    double COST_RATIO    = 1.5;     // ExecutionCostGuard min gross/cost
    bool   ALLOW_SHORT   = true;    // bidirectional; longs are the bull-validated side
    bool   MACD_GATE     = true;    // gate entries by MACD(12,26,9) direction on the HTF
                                    // close (long only if MACD>signal, short if <).
                                    // 2026-06-04: lifts the noisier NQ-future feed
                                    // PF 0.90->1.07 by cutting counter-momentum entries.
    double lot           = 1.0;

    bool   enabled       = true;
    bool   shadow_mode   = true;    // gated shadow on first deploy (bull-only caveat)
    bool   verbose       = false;

    // ── Callbacks ────────────────────────────────────────────────────────────
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // ── Position ─────────────────────────────────────────────────────────────
    struct Position { bool active=false; int dir=0; double entry_px=0, stop_px=0,
                      tp_px=0, size=0; int64_t entry_ms=0; double mfe=0, mae=0; } pos;

    bool persist_save(const char* eng, const char* sym, omega::PositionSnapshot& o) const {
        if (!pos.active) return false;
        o.engine=eng; o.symbol=sym; o.side = pos.dir>0 ? "LONG" : "SHORT";
        o.size=pos.size; o.entry=pos.entry_px; o.sl=pos.stop_px; o.tp=pos.tp_px;
        o.entry_ts=pos.entry_ms/1000;
        return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        pos = Position{};
        pos.active=true; pos.dir = (ps.side=="SHORT") ? -1 : 1;
        pos.entry_px=ps.entry; pos.stop_px=ps.sl; pos.tp_px=ps.tp; pos.size=ps.size;
        pos.entry_ms=ps.entry_ts*1000;
        return true;
    }
    bool has_open_position() const { return pos.active; }
    bool atr_ready() const { return m_atr_ready; }
    double atr_value() const { return m_atr; }

    void init() {
        m_htf.clear(); m_fvgs.clear();
        m_cur_bucket=-1; m_o=m_h=m_l=m_c=0;
        m_atr=0; m_atr_ready=false;
        m_day=-1; m_day_hi=-1e18; m_day_lo=1e18; m_pdh=-1; m_pdl=-1; m_have_pd=false;
        m_ema12=0; m_ema26=0; m_macd_sig=0; m_macd_init=false; m_macd_bull=false;
        pos = Position{};
    }

    // ── Warm-seed: replay a 15m CSV (ts_unix,o,h,l,c) while disabled so the HTF
    //   history / ATR / FVG queue / prior-day levels are hot on day one. ──────
    size_t seed_from_m15_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { printf("[SEED] %s: WARN cannot open %s (warms from live)\n",
                                   engine_name.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);   // header
        const bool save_en = enabled; enabled = false;   // no entries on history
        size_t n=0; long long ts; double o,h,l,c;
        while (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c)==5 && h>=l) {
                int64_t tsec = (ts > 100000000000LL) ? ts/1000 : ts;   // accept ms or sec ts
                _finalize_bar(tsec, o,h,l,c); ++n;
            }
        }
        enabled = save_en;
        printf("[SEED] %s (%s): %zu 15m bars replayed -- ATR hot=%d htf=%zu fvgs=%zu\n",
               engine_name.c_str(), symbol.c_str(), n, (int)m_atr_ready, m_htf.size(), m_fvgs.size());
        fflush(stdout);
        return n;
    }

    // ── Main tick handler ─────────────────────────────────────────────────────
    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        if (bid<=0.0 || ask<=0.0 || ask<bid) return;
        const double mid = (bid+ask)*0.5;
        const int64_t sec = now_ms/1000;
        const int64_t bucket = (sec/HTF_SEC)*HTF_SEC;

        // roll the 15m bar
        if (m_cur_bucket<0) { m_cur_bucket=bucket; m_o=m_h=m_l=m_c=mid; }
        else if (bucket!=m_cur_bucket) {
            _finalize_bar(m_cur_bucket, m_o,m_h,m_l,m_c);
            m_cur_bucket=bucket; m_o=m_h=m_l=m_c=mid;
        } else { m_h=std::max(m_h,mid); m_l=std::min(m_l,mid); m_c=mid; }

        // ── manage open position on every tick ──────────────────────────────
        if (pos.active) {
            const double fav = pos.dir>0 ? (bid-pos.entry_px) : (pos.entry_px-ask);
            if (fav>pos.mfe) pos.mfe=fav; if (fav<pos.mae) pos.mae=fav;
            if (pos.dir>0) {
                if (bid<=pos.stop_px) { _close(bid, now_ms, "STOP"); return; }
                if (bid>=pos.tp_px)   { _close(bid, now_ms, "DOL");  return; }
            } else {
                if (ask>=pos.stop_px) { _close(ask, now_ms, "STOP"); return; }
                if (ask<=pos.tp_px)   { _close(ask, now_ms, "DOL");  return; }
            }
            return;   // one position at a time
        }

        // ── entry: NY killzone only, ATR ready ───────────────────────────────
        if (!m_atr_ready || m_atr<=0.0) return;
        const int hm = _utc_hm(sec);
        if (hm < SESS_OPEN_HM || hm >= SESS_CLOSE_HM) return;

        const double dolUp = _dol_up(mid), dolDn = _dol_dn(mid);
        const int    cur_idx = (int)m_htf.size()-1;     // index of last CLOSED htf bar
        for (auto& f : m_fvgs) {
            if (f.used) continue;
            if (cur_idx - f.bar_idx > MAX_FVG_AGE) continue;          // freshness
            if (f.dir>0 && dolUp<0) continue;
            if (f.dir<0 && dolDn<0) continue;
            if (mid < f.lo || mid > f.hi) continue;                  // retrace into gap (mitigation)

            const double entry = (f.dir>0) ? f.hi : f.lo;            // fill at near edge
            const double stop  = (f.dir>0) ? (f.lo - STOP_BUF_ATR*m_atr)
                                           : (f.hi + STOP_BUF_ATR*m_atr);
            const double r     = (f.dir>0) ? (entry-stop) : (stop-entry);
            if (r <= 0.05*m_atr) { f.used=true; continue; }
            const double dol   = (f.dir>0) ? dolUp : dolDn;
            const double rr    = (f.dir>0) ? (dol-entry)/r : (entry-dol)/r;
            if (rr < MIN_RR || rr > 20.0) continue;
            if (f.dir<0 && !ALLOW_SHORT)  continue;
            if (MACD_GATE) { if (f.dir>0 && !m_macd_bull) continue;     // momentum direction gate
                             if (f.dir<0 &&  m_macd_bull) continue; }

            const double tp_dist = std::fabs(dol-entry);
            const double spread  = std::fabs(ask-bid);
            if (!ExecutionCostGuard::is_viable(symbol.c_str(), spread, tp_dist, lot, COST_RATIO)) {
                f.used=true; continue;                               // cost gate
            }
            pos = Position{}; pos.active=true; pos.dir=f.dir;
            pos.entry_px=entry; pos.stop_px=stop; pos.tp_px=dol; pos.size=lot; pos.entry_ms=now_ms;
            f.used=true;
            if (verbose) printf("[%s] %s %s entry=%.2f stop=%.2f dol=%.2f rr=%.2f atr=%.2f\n",
                engine_name.c_str(), symbol.c_str(), f.dir>0?"LONG":"SHORT", entry, stop, dol, rr, m_atr);
            break;
        }
    }

    void force_close(double bid, double ask, int64_t now_ms) {
        if (pos.active) _close(pos.dir>0?bid:ask, now_ms, "FORCE_CLOSE");
    }

private:
    struct HBar { int64_t ts; double o,h,l,c; };
    struct FVG  { int dir; double lo,hi; int bar_idx; bool used; };

    static int _utc_hm(int64_t sec){ int s=(int)(((sec%86400)+86400)%86400); return (s/3600)*100 + (s%3600)/60; }
    static int64_t _utc_day(int64_t sec){ return sec/86400; }

    // Finalize one CLOSED 15m bar: append to history, update ATR, prior-day
    // levels, and run FVG detection on the latest 3-bar window.
    void _finalize_bar(int64_t ts, double o, double h, double l, double c) {
        // prior-day high/low (UTC calendar)
        const int64_t d = _utc_day(ts);
        if (d != m_day) {
            if (m_day>=0 && m_day_hi>m_day_lo) { m_pdh=m_day_hi; m_pdl=m_day_lo; m_have_pd=true; }
            m_day=d; m_day_hi=-1e18; m_day_lo=1e18;
        }
        m_day_hi=std::max(m_day_hi,h); m_day_lo=std::min(m_day_lo,l);

        m_htf.push_back({ts,o,h,l,c});
        if ((int)m_htf.size() > SWING_LB+ATR_LEN+8) m_htf.pop_front();   // bound memory

        // MACD(12,26,9) on the HTF close -> m_macd_bull
        if (!m_macd_init) { m_ema12=c; m_ema26=c; m_macd_sig=0; m_macd_init=true; }
        else { m_ema12=c*(2.0/13)+m_ema12*(1-2.0/13); m_ema26=c*(2.0/27)+m_ema26*(1-2.0/27); }
        { const double macd=m_ema12-m_ema26; m_macd_sig=macd*(2.0/10)+m_macd_sig*(1-2.0/10);
          m_macd_bull=(macd>m_macd_sig); }

        // ATR (simple mean of TR over ATR_LEN)
        const int n=(int)m_htf.size();
        if (n>=2) {
            double trsum=0; int cnt=0;
            for (int k=std::max(1,n-ATR_LEN); k<n; ++k) {
                const auto&b=m_htf[k]; const double pc=m_htf[k-1].c;
                trsum += std::max(b.h-b.l, std::max(std::fabs(b.h-pc), std::fabs(b.l-pc))); ++cnt;
            }
            if (cnt>0){ m_atr=trsum/cnt; if(cnt>=ATR_LEN-1) m_atr_ready=true; }
        }
        // FVG detection on the last 3 closed bars (i-2,i-1,i)
        if (n>=3 && m_atr>0) {
            const auto& a0=m_htf[n-3]; const auto& a2=m_htf[n-1];
            const int idx=n-1;
            if (a0.h < a2.l) { double g=a2.l-a0.h; if (g>=MIN_GAP_ATR*m_atr && g<=6*m_atr)
                _push_fvg({+1, a0.h, a2.l, idx, false}); }
            if (a0.l > a2.h) { double g=a0.l-a2.h; if (g>=MIN_GAP_ATR*m_atr && g<=6*m_atr)
                _push_fvg({-1, a2.h, a0.l, idx, false}); }
        }
        // expire stale FVGs
        const int cur=(int)m_htf.size()-1;
        for (auto& f : m_fvgs) if (!f.used && cur-f.bar_idx>FVG_TTL) f.used=true;
        while (m_fvgs.size()>128) m_fvgs.pop_front();
    }
    void _push_fvg(const FVG& f){ m_fvgs.push_back(f); }

    // nearest untapped DOL above/below price within MAX_DOL_ATR (PDH/PDL + swing)
    double _dol_up(double px) const {
        double best=-1;
        const double swHi=_swing_hi();
        for (double lvl : {m_have_pd?m_pdh:-1.0, swHi})
            if (lvl>px+0.3*m_atr && (lvl-px)<=MAX_DOL_ATR*m_atr) best = (best<0? lvl : std::min(best,lvl));
        return best;
    }
    double _dol_dn(double px) const {
        double best=-1;
        const double swLo=_swing_lo();
        for (double lvl : {m_have_pd?m_pdl:-1.0, swLo})
            if (lvl>0 && lvl<px-0.3*m_atr && (px-lvl)<=MAX_DOL_ATR*m_atr) best = (best<0? lvl : std::max(best,lvl));
        return best;
    }
    double _swing_hi() const { int n=(int)m_htf.size(); if(n<4) return -1; double hi=-1;
        for (int k=std::max(0,n-2-SWING_LB); k<=n-2; ++k) hi=std::max(hi,m_htf[k].h); return hi; }
    double _swing_lo() const { int n=(int)m_htf.size(); if(n<4) return -1; double lo=-1;
        for (int k=std::max(0,n-2-SWING_LB); k<=n-2; ++k){ if(lo<0||m_htf[k].l<lo) lo=m_htf[k].l; } return lo; }

    void _close(double exit_px, int64_t now_ms, const char* reason) {
        if (!pos.active) return;
        const double pnl_px = pos.dir>0 ? (exit_px-pos.entry_px) : (pos.entry_px-exit_px);
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.engine=engine_name; tr.side = pos.dir>0?"LONG":"SHORT";
        tr.entryPrice=pos.entry_px; tr.exitPrice=exit_px; tr.sl=pos.stop_px; tr.tp=pos.tp_px;
        tr.size=pos.size; tr.pnl=pnl_px*pos.size;
        tr.mfe=pos.mfe*pos.size; tr.mae=std::fabs(pos.mae)*pos.size;
        tr.entryTs=pos.entry_ms/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason; tr.shadow=shadow_mode;
        if (on_trade_record) on_trade_record(tr);
        if (verbose) printf("[%s] CLOSE %s exit=%.2f pnl=%.2f\n", engine_name.c_str(), reason, exit_px, pnl_px);
        pos = Position{};
    }

    // HTF state
    std::deque<HBar> m_htf;
    std::deque<FVG>  m_fvgs;
    int64_t m_cur_bucket=-1; double m_o=0,m_h=0,m_l=0,m_c=0;
    double  m_atr=0; bool m_atr_ready=false;
    int64_t m_day=-1; double m_day_hi=-1e18, m_day_lo=1e18, m_pdh=-1, m_pdl=-1; bool m_have_pd=false;
    double  m_ema12=0, m_ema26=0, m_macd_sig=0; bool m_macd_init=false, m_macd_bull=false;
};

}  // namespace omega
