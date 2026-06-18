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
#include "ClusterGate.hpp"            // cross-engine same-direction cluster cap (S-2026-06-11)
#include "IndexRiskGate.hpp"          // omega::index_risk_off() -- bull-only regime gate

namespace omega {

class FvgContinuationEngine {
public:
    // ── Identity ─────────────────────────────────────────────────────────────
    std::string symbol      = "NAS100";
    std::string engine_name = "FvgContinuation";

    // ── Config (backtest-found defaults; NAS100 15m NY killzone) ─────────────
    int    HTF_SEC       = 900;     // 15-minute HTF FVG bars
    int    TRENDN        = 0;       // daily-trend gate: long only mid>SMA(htf.c,TRENDN), short only mid<. 0=off. (backtest: 15m wants 288=3day, 10m wants OFF)
    int    SESS_OPEN_HM  = 1330;    // NY killzone open  (UTC hhmm)
    int    SESS_CLOSE_HM = 1500;    // NY killzone close (UTC hhmm)
    double MIN_GAP_ATR   = 1.0;     // FVG size floor (ATR units) — displacement quality
    double MAX_DOL_ATR   = 3.0;     // DOL must be within this many ATR (reachable)
    int    MAX_FVG_AGE   = 8;       // FVG must be this fresh (HTF bars) at entry
    int    FVG_TTL       = 24;      // FVG expires after this many HTF bars
    double STOP_BUF_ATR  = 0.10;    // structural stop sits this far beyond the gap far edge
    double FILL_TOL_ATR  = 0.25;    // 2026-06-10 fill-fidelity: only fill when price is within
                                    //   this of the entry edge. Live previously booked the edge
                                    //   price the instant mid entered the gap (even mid-gap) ->
                                    //   a fill the market never traded (the 4s -87 NAS short).
                                    //   The backtest (fvg_core) fills only when the bar reaches
                                    //   the edge; this makes the live engine match it.
    int    ATR_LEN       = 14;      // HTF ATR lookback
    int    SWING_LB      = 48;      // HTF bars for the swing-high/low DOL
    // 2026-06-12 min-retrace-depth gate (fib "golden-zone" idea, tested) -- OFF by
    //   default; an available, backtested shadow lever, NOT a cleared deploy change.
    //   A YouTube strat proposed entering FVGs only inside the 0.618-0.786 fib
    //   "golden pocket". Backtested on NAS (fvg_core.cpp / fvg_trend.cpp, --fibgate):
    //   - The LITERAL golden pocket is WRONG: 0.618-0.786 leaves n=4-11 (noise) and
    //     >=0.618 FAILS outright (PF 0.78, H1 0% WR). The video's specific levels die.
    //   - The underlying principle (wait for a real pullback) DOES help the bare
    //     mechanism: require entry to have retraced >= MIN_RETRACE of the recent
    //     RETRACE_LB-bar leg. On the SIMPLE config (no trend/MACD gate), NAS
    //     age12/dol2.0: OFF n79 PF1.63 (H1 1.45/H2 1.79) -> >=0.382 n58 PF2.01
    //     (H1 1.68/H2 2.35), 3x-cost-robust, and it cuts the 2022-bear bleed
    //     (PF 0.56->0.72).
    //   - BUT on the LIVE engine config (TRENDN=288 + MACD_GATE already active), the
    //     fvg_trend.cpp engine-faithful run shows the gate does NOT clear walk-forward:
    //     OFF n55 PF1.23 (H1 0.82 / H2 1.72) -> >=0.382 n34 PF1.30 (H1 0.65 / H2 2.51).
    //     The trend+MACD gates already do the selectivity; stacking retrace on top
    //     thins to n34 and worsens H1 -- both-halves FAILS (as does the OFF baseline
    //     on this window). Per the never-deploy-without-both-halves rule, it is NOT
    //     enabled. Revisit if a cleaner cross-window run clears H1.
    double MIN_RETRACE   = 0.0;     // entry must have retraced >= this of recent leg (0=OFF; 0.382 = tested lever)
    double MAX_RETRACE   = 1.0;     // upper cap (1.0 = no golden-pocket ceiling; the deep pocket failed)
    int    RETRACE_LB    = 12;      // HTF bars defining the recent impulse leg for the gate
    double MIN_RR        = 1.0;     // require DOL >= this many R away
    double COST_RATIO    = 1.5;     // ExecutionCostGuard min gross/cost
    bool   ALLOW_SHORT   = true;    // bidirectional; longs are the bull-validated side
    bool   MACD_GATE     = true;    // gate entries by MACD(12,26,9) direction on the HTF
                                    // close (long only if MACD>signal, short if <).
    bool   REV_EXIT      = true;    // S-2026-06-11 reversal-exit: cut an OPEN position early
                                    // when the HTF MACD flips AGAINST it, but ONLY before +1R
                                    // (mfe < risk) -- rescues failing counter-trend trades,
                                    // never cuts a winner. Backtest (fvg_trend.cpp, engine-
                                    // faithful MACD+TRENDN288, NAS): avgLoss -50->-42 (-16%),
                                    // PF held 1.22->1.23. Tighter stops were proven catastrophic
                                    // (PF 1.22->0.95); this is the one safe loss-containment.
                                    // 2026-06-04: lifts the noisier NQ-future feed
                                    // PF 0.90->1.07 by cutting counter-momentum entries.
    // ── 2026-06-16 trend-beta config — CODED BUT FALSIFIED, DO NOT ENABLE ──────────────
    //   A bar-replay sweep (backtest/fvg_regime.cpp) suggested M30 + TRAIL + ER-gate was
    //   cross-regime ROBUST (NAS PF1.25 incl 2022 bear). FALSIFIED by the engine-faithful
    //   tick backtest (backtest/fvg_engine_bt.cpp, which drives THIS class): net-NEGATIVE
    //   in every regime (PF 0.76-1.04). The same driver reproduces the ORIGINAL 15m config
    //   at PF0.95 — matching its live-losing tombstone, NOT the fvg_core PF1.65 claim.
    //   CONCLUSION: the bar-replay harnesses (fvg_core/fvg_regime) overstate FVG by ~0.5-0.7
    //   PF via within-bar trail look-ahead + optimistic edge-fills the live tick engine can't
    //   reproduce. The TRAIL/ER/RISK_OFF_BLOCK fields below are kept as default-OFF optionality
    //   but are NOT validated. Engine remains TOMBSTONED (enabled=false in engine_init).
    //   See memory omega-fvg-trend-beta-regime-gate (corrected) + fvg_engine_bt.cpp. ──
    bool   TRAIL_EXIT  = false;   // ride winners with an ATR trail (arms only after +1R fav);
                                  //   replaces hold-to-DOL TP. DOL still gates entry rr (room).
    double TRAIL_ATR   = 2.0;     // trail distance from peak-favorable (ATR units)
    bool   ER_GATE     = false;   // Kaufman efficiency-ratio trend-gate: enter only when the HTF
                                  //   is cleanly trending (ER over ER_PERIOD >= ER_THR). Skips
                                  //   chop; direction-agnostic (FVG dir carries bull/bear).
    int    ER_PERIOD   = 20;      // ER lookback (HTF bars)
    double ER_THR      = 0.30;    // ER >= this == trending
    bool   RISK_OFF_BLOCK = true; // legacy bull-only safety: block ALL entries in macro risk-off.
                                  //   MUST be false for the trend-beta config — the edge's bear
                                  //   robustness comes from SHORT continuation in a clean bear
                                  //   (NAS-2022 shorts = +1919); risk-off-block kills exactly that.
                                  //   Keep true only for a long-bias bull-regime overlay.
    double lot           = 1.0;

    bool   enabled       = true;
    bool   shadow_mode   = true;    // gated shadow on first deploy (bull-only caveat)
    bool   verbose       = false;

    // ── Callbacks ────────────────────────────────────────────────────────────
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;
    // S-2026-06-13g FAMILY DEDUP: optional veto checked immediately before an
    // entry executes. Wired in engine_init across the 10m/15m/30m NAS A/B/C
    // instances so only ONE variant holds a position at a time (first-to-fire
    // wins -- same tradeoff the operator accepted for the pump trio 2026-06-11).
    // Cause: 2026-06-12 ledger showed FvgContinuation + FvgCont30m booking the
    // IDENTICAL NAS entry (13:30:03 + 13:30:19) -- ClusterGate caps at 2 so the
    // pair passed; this closes the intra-family hole. No permit wired = no veto.
    using EntryPermit = std::function<bool()>;
    EntryPermit entry_permit;

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
            // reversal-exit: HTF MACD flipped against us AND still below +1R -> cut now.
            if (REV_EXIT) {
                const double risk = std::fabs(pos.entry_px - pos.stop_px);
                const bool flipped = pos.dir>0 ? !m_macd_bull : m_macd_bull;
                if (flipped && risk > 0.0 && pos.mfe < risk) {
                    _close(pos.dir>0?bid:ask, now_ms, "MACD_REV"); return;
                }
            }
            // TRAIL exit (2026-06-16 re-validation): ride the trend; arm an ATR trail only
            //   after +1R favorable so a winner is never cut early. NO DOL TP in this mode --
            //   the trail (or the structural stop pre-1R) is the only exit. This is the lever
            //   that made the edge cross-regime robust (rides the 2022-bear shorts to the floor).
            if (TRAIL_EXIT) {
                const double risk = std::fabs(pos.entry_px - pos.stop_px);
                const bool armed = (m_atr>0.0 && risk>0.0 && pos.mfe >= risk);
                if (armed) {   // ratchet the structural stop toward peak-favorable
                    if (pos.dir>0) { const double ts=(pos.entry_px+pos.mfe)-TRAIL_ATR*m_atr; if (ts>pos.stop_px) pos.stop_px=ts; }
                    else           { const double ts=(pos.entry_px-pos.mfe)+TRAIL_ATR*m_atr; if (ts<pos.stop_px) pos.stop_px=ts; }
                }
                if (pos.dir>0) { if (bid<=pos.stop_px){ _close(bid,now_ms, armed?"TRAIL":"STOP"); return; } }
                else           { if (ask>=pos.stop_px){ _close(ask,now_ms, armed?"TRAIL":"STOP"); return; } }
                return;   // one position at a time; no DOL TP in trail mode
            }
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
        // 2026-06-09: FVG validated bull-only -> no NEW entries in macro risk-off/bear regime.
        // (open positions above are still managed). Blocks the bear-regime longs that bled.
        if (RISK_OFF_BLOCK && omega::index_risk_off()) return;
        // ER trend-gate (2026-06-16): only enter when the HTF is cleanly trending. Chop is
        //   where FVG continuation dies (SPX-2022 grind); the gate skips it. Direction-agnostic.
        if (ER_GATE) { const double er=_efficiency_ratio(ER_PERIOD); if (er>=0.0 && er<ER_THR) return; }

        const double dolUp = _dol_up(mid), dolDn = _dol_dn(mid);
        const int    cur_idx = (int)m_htf.size()-1;     // index of last CLOSED htf bar
        // 2026-06-09 daily-trend gate (backtested: 15m+288 -> PF1.41 ret/DD3.38 both-halves+;
        // 10m OFF -> stronger ungated). long only above trend-SMA, short only below.
        double trendMA = 0.0;
        if (TRENDN > 0) { const int nn=(int)m_htf.size();
            if (nn >= TRENDN) { double ss=0.0; for(int k=nn-TRENDN;k<nn;++k) ss+=m_htf[k].c; trendMA=ss/TRENDN; } }
        for (auto& f : m_fvgs) {
            if (f.used) continue;
            if (cur_idx - f.bar_idx > MAX_FVG_AGE) continue;          // freshness
            if (f.dir>0 && dolUp<0) continue;
            if (f.dir<0 && dolDn<0) continue;
            if (TRENDN>0 && trendMA>0.0) { if (f.dir>0 && mid<trendMA) continue; if (f.dir<0 && mid>trendMA) continue; }
            if (mid < f.lo || mid > f.hi) continue;                  // retrace into gap (mitigation)
            // 2026-06-10 fill-fidelity: the entry is booked AT the gap edge (f.lo short /
            //   f.hi long). Only fill once price has actually retraced TO that edge -- else
            //   we'd record a price the market never traded (the 4s -87 NAS short, where
            //   mid was mid-gap but entry was booked at the far edge). f.used stays unset
            //   so the gap remains a live candidate until price reaches the edge. Matches
            //   the backtest (fvg_core), which fills only when the bar's range reaches it.
            {
                const double _edge = (f.dir>0) ? f.hi : f.lo;
                if (m_atr > 0.0 && std::fabs(mid - _edge) > FILL_TOL_ATR * m_atr) continue;
            }

            // price IS mitigating a fresh, correctly-pointed gap → a live candidate.
            // Log the first such candidate's verdict ONCE per HTF bar (verbose only).
            const bool diag = verbose && (cur_idx != m_diag_bar);
            #define FVG_DIAG(fmt, ...) do{ if(diag){ printf("[%s] %s BLOCK " fmt, engine_name.c_str(), symbol.c_str(), ##__VA_ARGS__); fflush(stdout); m_diag_bar=cur_idx; } }while(0)

            const double entry = (f.dir>0) ? f.hi : f.lo;            // fill at near edge
            // min-retrace-depth gate (see MIN_RETRACE provenance above): skip shallow
            //   mitigations -- only take FVGs entered after a >=MIN_RETRACE pullback of
            //   the recent impulse leg. Lifts NAS PF 1.63->2.01, both halves +.
            if (MIN_RETRACE > 0.0 && !_retrace_ok(f.dir, entry)) {
                FVG_DIAG("retrace too shallow (need >=%.3f of %d-bar leg)\n", MIN_RETRACE, RETRACE_LB);
                continue;   // gap stays live; a deeper mitigation may still qualify
            }
            const double stop  = (f.dir>0) ? (f.lo - STOP_BUF_ATR*m_atr)
                                           : (f.hi + STOP_BUF_ATR*m_atr);
            const double r     = (f.dir>0) ? (entry-stop) : (stop-entry);
            if (r <= 0.05*m_atr) { FVG_DIAG("dir=%d r~0 (degenerate gap)\n", f.dir); f.used=true; continue; }
            const double dol   = (f.dir>0) ? dolUp : dolDn;
            const double rr    = (f.dir>0) ? (dol-entry)/r : (entry-dol)/r;
            if (rr < MIN_RR || rr > 20.0) { FVG_DIAG("dir=%d rr=%.2f outside[%.1f,20] dol=%.2f entry=%.2f atr=%.2f\n", f.dir, rr, MIN_RR, dol, entry, m_atr); continue; }
            if (f.dir<0 && !ALLOW_SHORT)  { FVG_DIAG("short setup but ALLOW_SHORT=false\n"); continue; }
            if (MACD_GATE) { if (f.dir>0 && !m_macd_bull) { FVG_DIAG("long but MACD bearish (macd<sig)\n"); continue; }     // momentum direction gate
                             if (f.dir<0 &&  m_macd_bull) { FVG_DIAG("short but MACD bullish\n"); continue; } }

            const double tp_dist = std::fabs(dol-entry);
            const double spread  = std::fabs(ask-bid);
            if (!ExecutionCostGuard::is_viable(symbol.c_str(), spread, tp_dist, lot, COST_RATIO)) {
                FVG_DIAG("cost gate: tp_dist=%.2f spread=%.2f lot=%.2f\n", tp_dist, spread, lot);
                f.used=true; continue;                               // cost gate
            }
            if (!omega::ClusterGate::allow_entry(symbol.c_str(), f.dir>0, engine_name.c_str())) {
                FVG_DIAG("cluster gate: same-direction cap reached\n");
                f.used=true; continue;                               // correlation cap
            }
            if (entry_permit && !entry_permit()) {
                FVG_DIAG("family dedup: sibling FVG variant already holds a position\n");
                f.used=true; continue;                               // S-2026-06-13g intra-family dedup
            }
            #undef FVG_DIAG
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
        if ((int)m_htf.size() > std::max(SWING_LB+ATR_LEN+8, TRENDN+4)) m_htf.pop_front();   // bound memory (keep >=TRENDN for trend gate)

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
    // min-retrace-depth gate: did `entry` retrace >= MIN_RETRACE of the recent
    //   RETRACE_LB-bar impulse leg? Mirrors fvg_core.cpp --fibgate exactly: the leg
    //   is [hidx-RETRACE_LB .. hidx-2] over closed HTF bars (hidx = last closed).
    bool _retrace_ok(int dir, double entry) const {
        const int n=(int)m_htf.size(); if (n<4) return false;
        const int hidx=n-1;
        double fHi=-1, fLo=-1;
        for (int k=std::max(0,hidx-RETRACE_LB); k<=hidx-2; ++k) {
            fHi=std::max(fHi,m_htf[k].h); if(fLo<0||m_htf[k].l<fLo) fLo=m_htf[k].l;
        }
        if (fHi<=fLo || fLo<=0) return false;
        const double rng=fHi-fLo;
        const double retr = dir>0 ? (fHi-entry)/rng : (entry-fLo)/rng;
        return retr>=MIN_RETRACE && retr<=MAX_RETRACE;
    }
    // Kaufman efficiency ratio over the last `n` CLOSED HTF closes: |net move| / sum(|bar moves|).
    //   1 = clean trend, 0 = chop. Direction-agnostic. -1 when insufficient history.
    double _efficiency_ratio(int n) const {
        const int sz=(int)m_htf.size(); if (n<1 || sz < n+1) return -1.0;
        const double dir = std::fabs(m_htf[sz-1].c - m_htf[sz-1-n].c);
        double vol=0.0; for (int k=sz-n; k<sz; ++k) vol += std::fabs(m_htf[k].c - m_htf[k-1].c);
        return vol>0.0 ? dir/vol : 0.0;
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
    int     m_diag_bar=-1;   // verbose reject-reason latch: at most one diag line per HTF bar
};

}  // namespace omega
