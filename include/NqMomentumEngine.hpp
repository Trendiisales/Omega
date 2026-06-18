// =============================================================================
//  NqMomentumEngine.hpp — regime-gated intraday momentum-continuation on NQ
//  (NAS100 / Nasdaq futures). Liquid-futures sibling of BigCapMomo: same
//  validated exit (ATR-trail + BE-ratchet + ride-in-profit) but a SINGLE liquid
//  instrument -> no micro-cap slippage. Long-only (up-momentum), gated to a bull
//  regime (price > REGIME_SMA) — the gate is load-bearing: ungated it is bull-beta
//  and loses in a bear (2022 PF0.87 -572); gated it flips bear-positive (PF1.16
//  +273) while keeping the bull edge (PF2.08 +1211, both WF halves+). Discovery:
//  backtest/momo_cont_nq.cpp; engine-faithful BT: backtest/nq_momentum_faithful.cpp.
//
//  Self-aggregates 5m bars from on_tick (mid), like XauStraddleM30Engine::on_tick_agg
//  — one call drives signal + intrabar exit. Emits omega::TradeRecord on close.
//  SHADOW by default. NOT wired live until faithful BT + shadow confirm.
//
//  ADVERSE-PROTECTION: (S-2026-06-19 — backtested verdict, MANDATORY per CLAUDE.md gate)
//    Levers (this file): loss_cut_atr = hard cold-loss cut; be_arm_atr/be_floor_atr =
//    ATR-scaled BE ratchet (the legacy %-BE never armed on an index -> the live -71 give-back).
//    Faithful sweep (nq_momentum_faithful.cpp, NAS100 2022-2026): a cold loss-cut LOWERS net on
//    this trail-momentum engine (1.0/1.5/2.0 ATR all bled ~-30k across hundreds of fires; net
//    +7949 -> +3826/+4609/+5530), and the ATR-BE alone also costs net without reducing the
//    -464pt worst trade. Matches the standing swing-protection finding: tightening hurts
//    trend/trail engines. The original BT data was DOWNSAMPLE-GRADE (10x + a 2023 gap) -> the
//    engine is marginal / bull-biased (WF not both-halves+) on it, an EDGE problem protection
//    cannot fix. DECISION + chosen config: see engine_init.hpp NqMomentum block.
// =============================================================================
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <string>
#include "OmegaTradeLedger.hpp"

namespace omega {

struct NqMomentumParams {
    double ig_pct        = 0.30;   // ignition: close up >= this % over lb bars
    int    lb            = 6;      // ignition lookback (6 x 5m = 30 min)
    int    atr_len       = 14;     // ATR (5m bars)
    double atr_mult      = 3.0;    // trailing stop = peak - atr_mult * ATR (S-2026-06-18 tuned 4.0->3.0:
                                   // tighter trail, bull PF 2.60->3.24, bear stays both-WF-halves+ PF1.18)
    double be_arm_pct    = 0.03;   // arm BE-floor once +3% in profit (LEGACY %-BE; ~mis-scaled for an index)
    double be_floor_pct  = 0.02;   // floor stop at entry +2% (net-BE)
    // --- in-flight adverse protection (S-2026-06-19) — MANDATORY per CLAUDE.md adverse-protection rule ---
    // The engine previously had NO cold-loss cut: a straight-adverse trade rode the 3xATR trail (~107pt on
    // NAS100) before stopping, and the %-BE above never armed on an index (3% = +900pt @30000). These ATR-scaled
    // levers fix both. Backward-compatible: all 0 => legacy %-BE + wide-trail behaviour exactly.
    double loss_cut_atr  = 0.0;    // HARD cold-loss cut: close if adverse >= loss_cut_atr * entry-ATR, BEFORE BE arms (0=off)
    double be_arm_atr    = 0.0;    // arm BE once peak >= entry + be_arm_atr*ATR  (0 => fall back to be_arm_pct)
    double be_floor_atr  = 0.0;    // BE floor stop at entry + be_floor_atr*ATR
    int    maxhold_bars  = 48;     // 48 x 5m = 4h backstop (skipped while in profit)
    int    regime_sma    = 200;    // bull-regime gate: trade only when close > SMA(regime_sma)
    bool   regime_gate   = true;   // LOAD-BEARING — false = bull-beta
    double dollars_per_pt= 1.0;    // NQ pt value (shadow scale; real NQ = $20/pt, set per deploy)
    double lot           = 1.0;
};
inline NqMomentumParams make_nq_momentum_params() { return NqMomentumParams{}; }

struct NqMomentumEngine {
    bool   shadow_mode = true;
    bool   enabled     = true;
    std::string symbol = "NAS100";
    std::string engine_name = "NqMomentum";
    NqMomentumParams p;
    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    // 5m bar aggregation
    int64_t bar_ms_ = -1; double o_=0,h_=0,l_=0,c_=0;
    std::deque<double> closes_;            // completed 5m closes (ignition + regime SMA)
    double atr_=0, atr_sum_=0, prev_c_=0; int atr_n_=0;

    struct Pos { bool active=false; double entry=0, peak=0, sl_atr=0, mfe=0, lot=0; int64_t entry_ts=0; int bars_held=0; } pos_;
    int trade_id_=0;
    bool has_open_position() const noexcept { return pos_.active; }

    void _close(double exit_px, const char* reason, int64_t now_ms, CloseCallback cb) noexcept {
        if(!pos_.active) return;
        double pts = exit_px - pos_.entry;
        omega::TradeRecord tr{};
        tr.symbol=symbol; tr.side="LONG"; tr.engine=engine_name;
        tr.entryPrice=pos_.entry; tr.exitPrice=exit_px; tr.size=pos_.lot;
        tr.pnl = pts * pos_.lot * p.dollars_per_pt;   // RAW pts*lot*dpp (ledger owns tick-value)
        tr.mfe=pos_.mfe; tr.entryTs=pos_.entry_ts/1000; tr.exitTs=now_ms/1000; tr.exitReason=reason;
        printf("[NQ-MOMO] EXIT %s entry=%.2f exit=%.2f pts=%.2f pnl=%.2f bars=%d%s\n",
               reason,pos_.entry,exit_px,pts,tr.pnl,pos_.bars_held, shadow_mode?" [SHADOW]":"");
        fflush(stdout);
        if(cb) cb(tr);
        pos_ = Pos{};
    }

    void force_close(double bid,double ask,int64_t now_ms,CloseCallback cb) noexcept {
        if(pos_.active) _close((bid+ask)*0.5, "FORCE_CLOSE", now_ms, cb);
    }

    void _on_bar_close(double bo,double bh,double bl,double bc,int64_t now_ms,CloseCallback cb) noexcept {
        // ATR (Wilder) on completed bar
        double tr=bh-bl; if(prev_c_>0){ double a=bh-prev_c_; if(a<0)a=-a; double b=prev_c_-bl; if(b<0)b=-b; if(a>tr)tr=a; if(b>tr)tr=b; }
        prev_c_=bc;
        if(atr_n_<p.atr_len){ atr_sum_+=tr; if(++atr_n_==p.atr_len) atr_=atr_sum_/p.atr_len; }
        else atr_=(atr_*(p.atr_len-1)+tr)/p.atr_len;
        // bars_held increments per completed bar while in position; maxhold backstop (ride if profit)
        if(pos_.active){
            if(++pos_.bars_held >= p.maxhold_bars && !(bc > pos_.entry))
                { _close(bc,"MAXHOLD",now_ms,cb); }
        }
        // record the completed close
        closes_.push_back(bc);
        int keep = (p.regime_sma>p.lb?p.regime_sma:p.lb)+2;
        while((int)closes_.size()>keep) closes_.pop_front();
        // signal: ignition + green + bull-regime  (entry fills on the NEXT tick)
        if(!pos_.active && enabled && atr_>0 && (int)closes_.size()>p.lb){
            double base = closes_[closes_.size()-1-p.lb];
            if(base>0){
                double up=(bc/base-1)*100.0;
                bool green = bc>bo;
                bool regime_ok=true;
                if(p.regime_gate && (int)closes_.size()>=p.regime_sma){
                    double sma=0; for(int k=0;k<p.regime_sma;k++) sma+=closes_[closes_.size()-1-k];
                    sma/=p.regime_sma; regime_ok=(bc>sma);
                }
                if(up>=p.ig_pct && green && regime_ok) armed_=true;  // arm; fill next tick
            }
        }
    }
    bool armed_=false;

    // single faithful entry point: feed every tick (mid from bid/ask)
    void on_tick(double bid,double ask,int64_t now_ms,CloseCallback cb) noexcept {
        if(bid<=0||ask<=0) return;
        const double mid=(bid+ask)*0.5;
        // manage open position intrabar
        if(pos_.active){
            if(mid>pos_.peak) pos_.peak=mid;
            double fav=mid-pos_.entry; if(fav>pos_.mfe) pos_.mfe=fav;
            const double a  = atr_>0 ? atr_ : 0.0;
            const double a0 = pos_.sl_atr>0 ? pos_.sl_atr : a;   // entry-time ATR -> fixed cold-loss stop distance
            // BE armed? (ATR-scaled if be_arm_atr set, else legacy %-based)
            const bool be_armed = (p.be_arm_atr>0 && a>0)
                                    ? (pos_.peak >= pos_.entry + p.be_arm_atr*a)
                                    : (p.be_arm_pct>0 && pos_.peak >= pos_.entry*(1+p.be_arm_pct));
            // (1) HARD COLD-LOSS CUT (S-2026-06-19) — fixed stop at entry - loss_cut_atr*ATR(entry), only before BE
            //     arms. Caps the straight-adverse loss the wide trail used to let run. Tighter than the trail early.
            if(p.loss_cut_atr>0 && a0>0 && !be_armed){
                double hardstop = pos_.entry - p.loss_cut_atr*a0;
                if(bid<=hardstop){ _close(hardstop,"LOSS_CUT",now_ms,cb); }
            }
            // (2) trailing stop + BE ratchet (ATR-scaled BE if be_arm_atr set, else legacy %-BE)
            if(pos_.active){
                double tstop = (p.atr_len>0 && a>0) ? (pos_.peak - p.atr_mult*a) : 0.0;
                if(p.be_arm_atr>0 && a>0){
                    if(pos_.peak >= pos_.entry + p.be_arm_atr*a){ double bf=pos_.entry + p.be_floor_atr*a; if(bf>tstop)tstop=bf; }
                } else if(p.be_arm_pct>0 && pos_.peak >= pos_.entry*(1+p.be_arm_pct)){
                    double bf=pos_.entry*(1+p.be_floor_pct); if(bf>tstop)tstop=bf;
                }
                if(tstop>0 && bid<=tstop){ _close(tstop,"TRAIL",now_ms,cb); /* fall through to bar agg */ }
            }
        }
        // fill a pending entry at this tick
        if(armed_ && !pos_.active){
            pos_.active=true; pos_.entry=ask; pos_.peak=ask; pos_.sl_atr=atr_; pos_.lot=p.lot;
            pos_.mfe=0; pos_.entry_ts=now_ms; pos_.bars_held=0; ++trade_id_; armed_=false;
            printf("[NQ-MOMO] ENTRY LONG @ %.2f atr=%.2f%s\n",pos_.entry,atr_,shadow_mode?" [SHADOW]":""); fflush(stdout);
        }
        // 5m bar aggregation
        int64_t bk=(now_ms/300000LL)*300000LL;
        if(bar_ms_<0){ bar_ms_=bk; o_=h_=l_=c_=mid; return; }
        if(bk!=bar_ms_){ _on_bar_close(o_,h_,l_,c_,now_ms,cb); bar_ms_=bk; o_=h_=l_=c_=mid; }
        else { if(mid>h_)h_=mid; if(mid<l_)l_=mid; c_=mid; }
    }
};

} // namespace omega
