#pragma once
//  ADVERSE-PROTECTION: SHORT-only bear engine, structural stop (backtested). In-flight
//    protection = Donchian structural SL. ibs_real_engine.cpp faithful 2022 bear
//    PF1.26 (NAS) / 1.59 (SPX) -- this engine EXISTS to trade the bear, so the
//    long-bear-gate is N/A; its risk is a short squeeze, capped by the structural SL.
//    BEAR-ONLY (bull leg disconfirmed) -> never bull-size (AUDITED_CONFIGS).
// =============================================================================
// IndexBearShortEngine.hpp -- SHORT the index breakdown on bad days (risk-off)
// =============================================================================
//
// 2026-06-12 SESSION DESIGN (Claude / Jo):
//   The long-only panic-bounce FAILED on indices (buying dips fights the
//   downtrend). The money on "bad days" is on the SHORT side: ride the
//   breakdown, GRAB profit fast (fixed 2R TP -- a chandelier trail gives it all
//   back on the violent bear counter-rally; tested, PF 0.87 vs fixed-TP 1.60).
//
//   ALWAYS-ON MONITOR + HARD REGIME GATE: every H1 bar the engine checks a
//   sustained-bear gate (price<EMA200, EMA200 falling over PERSIST bars,
//   EMA50<EMA200). A bull *correction* dips below EMA200 briefly; a real bear
//   keeps EMA200 falling for a long stretch -> the gate is near-silent in a
//   bull and active through 2022-style bears. Optionally tightened by the shared
//   macro risk-off read (omega::index_risk_off(): VIX backwardation + credit +
//   dollar) when its feed is live.
//
//   ENTRY (short): in bear-regime, a Donchian breakdown -- close below the prior
//   DON-bar low, with local down-momentum (close<EMA50, EMA50 falling).
//   STOP = recent swing high (or entry + SL_ATR*ATR). TP = TP_R * risk (fixed).
//
//   BACKTEST (backtest/index_bear_short_bt.cpp, H1, cost-incl):
//     NAS 2022 bear: PF 1.60 net +1623pt n=18, BOTH halves+ (1.70/1.50).
//     Bull corpus 24-26 (regime-gated): pooled +702pt -- NOT bleeding (vs the
//       ungated short which bled -9025pt). NAS still leaks on bull-correction
//       recoveries (irreducible without the live VIX/credit gate).
//   CAVEAT: validated on ONE bear instrument (NAS 2022). SPX/GER 2022 cross-
//   validation pending. -> SHADOW until cross-instrument bear confirms.
//
// LOG NAMESPACE: [IBS]. tr.engine="IndexBearShort". tr.regime="BEAR_BREAKDOWN".
// =============================================================================

#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <deque>
#include <functional>
#include <fstream>
#include <string>
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot (persist_save/restore)
#include "OmegaCostGuard.hpp"
#include "OmegaTradeLedger.hpp"
#include "IndexRiskGate.hpp"

namespace omega {

class IndexBearShortEngine {
public:
    static constexpr int BAR_SECS = 3600;          // H1 decision bars

    std::string symbol      = "NAS100";
    std::string engine_name = "IndexBearShort";

    // --- params (baked to the validated cfg; tunable per symbol) ---
    int    ATR_N      = 24;
    int    DON        = 48;    // Donchian breakdown lookback (H1)
    int    EMA_FAST   = 50;
    int    EMA_SLOW   = 200;
    int    PERSIST    = 100;   // EMA_SLOW must be below its value PERSIST bars ago
    double SL_ATR     = 2.0;
    double TP_R       = 2.0;   // fixed TP = TP_R * initial risk
    int    MAX_HOLD   = 240;
    int    COOLDOWN   = 12;
    bool   USE_RISKOFF_GATE = false;  // also require omega::index_risk_off() (feed-dependent)

    double COST_PTS   = 2.0;   // round-trip pts (per-symbol; NAS~2, SPX~0.6, GER~1.5)
    double lot        = 1.0;   // index lot (USD_PER_PT via ExecutionCostGuard table)

    bool shadow_mode = true;
    bool enabled     = true;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;
    CloseCallback on_close_cb;

    struct LivePos {
        bool    active = false;
        double  entry = 0.0, stop = 0.0, tp = 0.0, ll = 0.0, atr_at_entry = 0.0, size = 0.0, mfe = 0.0;
        int64_t entry_ts = 0, entry_bar_seq = 0;
    } m_pos;
    bool has_open_position() const noexcept { return m_pos.active; }

    // ---- S-2026-06-26 PERSISTENCE (wire_cross archetype): survive restart/deploy. This engine showed
    // positions (register_source) but did NOT persist -> its SHORT vanished with no ledger close on every
    // restart. persist_save emits full state; persist_restore re-adopts it. Short-only (risk-off SHORT).
    bool persist_save(const char* tag, const char* sym, omega::PositionSnapshot& ps) const {
        if (!m_pos.active) return false;
        ps.engine = tag; ps.symbol = sym; ps.side = "SHORT";
        ps.size = m_pos.size; ps.entry = m_pos.entry; ps.sl = m_pos.stop; ps.tp = m_pos.tp;
        // PositionSnapshot.entry_ts is EPOCH SECONDS; engine clock is ms. Saving raw ms
        // put an ms value into g_restored_entry_ts while trade_lifecycle compares
        // tr.entryTs in seconds -> the phantom-drop exemption never matched and a
        // restored position's close was silently dropped (2026-07-17 boot: NAS SHORT
        // TIME_STOP +$111.75 vanished from the ledger).
        ps.entry_ts = m_pos.entry_ts / 1000; return true;
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        m_pos = LivePos{};
        m_pos.active = true; m_pos.entry = ps.entry; m_pos.stop = ps.sl; m_pos.tp = ps.tp;
        m_pos.size = ps.size; m_pos.entry_ts = ps.entry_ts * 1000;
        m_pos.ll = ps.entry;
        // bar_seq_ resets each boot and the H1 seed replay advances it before restore
        // runs (omega_main restores AFTER init_engines seeding). Leaving entry_bar_seq
        // at 0 makes bar_seq_ - entry_bar_seq >= MAX_HOLD instantly true -> spurious
        // TIME_STOP on the first live bar (2026-07-17 boot closed the live NAS SHORT
        // 2.8h into a 240-bar max hold). Restart the hold clock at restore instead.
        m_pos.entry_bar_seq = bar_seq_;
        return true;
    }

    // ---- warm seed (mandate, pattern 2): replay H1 CSV (ts,o,h,l,c) ----
    void seed_from_h1_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[IBS][SEED] MISS %s\n", path.c_str()); return; }
        bool was = enabled; enabled = false;
        std::string line; long n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
            char* p=(char*)line.c_str(); char* e;
            int64_t ts=std::strtoll(p,&e,10); if(*e!=',')continue; p=e+1;
            double o=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double h=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double l=std::strtod(p,&e); if(*e!=',')continue; p=e+1;
            double c=std::strtod(p,&e);
            if(o>0&&h>0&&l>0&&c>0){ _push_bar(ts*1000,o,h,l,c); ++n; }
        }
        enabled = was;
        std::printf("[IBS][SEED] %s %ld H1 bars from %s (buf=%zu)\n", symbol.c_str(), n, path.c_str(), c_.size());
    }

    // ---- tick entry (lightweight house convention; closes via on_close_cb) ----
    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!enabled) return;
        const double mid = (bid + ask) * 0.5;
        const double spread = ask - bid;
        _accumulate(mid, now_ms);
        const int64_t bar = now_ms / 1000 / BAR_SECS;
        if (acc_.n > 0 && bar != acc_bar_) {
            _close_bar();
            _on_bar_close(spread);
            acc_bar_ = bar;
        }
    }

private:
    std::deque<double> o_, h_, l_, c_, atr_, ema_, emaS_;
    std::deque<int64_t> ts_;
    std::deque<double> tr_; double tr_sum_ = 0.0;
    struct Acc { double o=0,h=0,l=0,c=0; int64_t ts=0; int n=0; } acc_;
    int64_t acc_bar_ = -1, bar_seq_ = 0, last_exit_seq_ = -100000;

    void _accumulate(double mid, int64_t now_ms) {
        const int64_t bar = now_ms / 1000 / BAR_SECS;
        if (acc_.n == 0) { acc_.o=acc_.h=acc_.l=acc_.c=mid; acc_.ts=now_ms; acc_.n=1; }
        else if (bar == acc_.ts/1000/BAR_SECS) { if(mid>acc_.h)acc_.h=mid; if(mid<acc_.l)acc_.l=mid; acc_.c=mid; acc_.n++; }
    }
    void _close_bar() { _push_bar(acc_.ts, acc_.o, acc_.h, acc_.l, acc_.c); acc_ = Acc{}; }

    void _push_bar(int64_t ts, double o, double h, double l, double c) {
        double tr = h - l;
        if (!c_.empty()) tr = std::max(tr, std::max(std::fabs(h-c_.back()), std::fabs(l-c_.back())));
        o_.push_back(o); h_.push_back(h); l_.push_back(l); c_.push_back(c); ts_.push_back(ts);
        tr_.push_back(tr); tr_sum_ += tr;
        if ((int)tr_.size() > ATR_N) { tr_sum_ -= tr_.front(); tr_.pop_front(); }
        atr_.push_back(tr_.empty()?0.0:tr_sum_/(double)tr_.size());
        const double kf = 2.0/(EMA_FAST+1), kls = 2.0/(EMA_SLOW+1);
        ema_.push_back (ema_.empty()?  c : ema_.back()  + kf *(c-ema_.back()));
        emaS_.push_back(emaS_.empty()? c : emaS_.back() + kls*(c-emaS_.back()));
        const size_t cap = (size_t)std::max(PERSIST, DON) + EMA_SLOW + 8;
        while (o_.size() > cap) { o_.pop_front();h_.pop_front();l_.pop_front();c_.pop_front();ts_.pop_front();atr_.pop_front();ema_.pop_front();emaS_.pop_front(); }
        ++bar_seq_;
    }

    void _on_bar_close(double spread) {
        const int N = (int)c_.size();
        if (N < std::max(PERSIST, DON) + 2) return;
        const double A = atr_.back(); if (A <= 0) { if(m_pos.active) _manage(); return; }
        if (m_pos.active) { _manage(); return; }
        if (bar_seq_ - last_exit_seq_ < COOLDOWN) return;

        // ALWAYS-ON regime monitor: sustained-bear gate
        const double cl = c_[N-1];
        const bool bear_regime = cl < emaS_[N-1] && emaS_[N-1] < emaS_[N-1-PERSIST] && ema_[N-1] < emaS_[N-1];
        if (!bear_regime) return;
        if (USE_RISKOFF_GATE && omega::g_index_regime_valid.load(std::memory_order_relaxed) && !omega::index_risk_off()) return;
        // local down-momentum
        if (!(cl < ema_[N-1] && ema_[N-1] < ema_[N-6])) return;

        // Donchian breakdown
        double lo = 1e18; for (int kk=N-1-DON; kk<N-1; ++kk) if (l_[kk] < lo) lo = l_[kk];
        if (cl >= lo) return;

        // size + structural stop
        double sh = h_[N-1]; for (int kk=N-2; kk>=N-4 && kk>=0; --kk) if (h_[kk] > sh) sh = h_[kk];
        const double entry = cl;
        double stop = sh > entry ? sh : entry + SL_ATR * A;
        const double risk = stop - entry; if (risk <= 0) return;
        const double tp = entry - TP_R * risk;

        // cost gate (entry filter): the TP move must clear cost
        if (!ExecutionCostGuard::is_viable(symbol.c_str(), spread, TP_R * risk, lot, 1.2)) return;

        m_pos = LivePos{};
        m_pos.active=true; m_pos.entry=entry; m_pos.stop=stop; m_pos.tp=tp; m_pos.ll=l_[N-1];
        m_pos.atr_at_entry=A; m_pos.size=lot; m_pos.entry_ts=ts_.back(); m_pos.entry_bar_seq=bar_seq_;
        std::printf("[IBS] %s SHORT %s breakdown<%.1f entry=%.1f stop=%.1f tp=%.1f (risk=%.1f)\n",
                    shadow_mode?"SHADOW":"LIVE", symbol.c_str(), lo, entry, stop, tp, risk);
        std::fflush(stdout);
    }

    void _manage() {
        const int N = (int)c_.size();
        const double curH = h_[N-1], curL = l_[N-1], curC = c_[N-1];
        if (curL < m_pos.ll) m_pos.ll = curL;
        const double fav = m_pos.entry - curL; if (fav > m_pos.mfe) m_pos.mfe = fav;

        double exit_px = 0.0; const char* why = nullptr;
        if (curH >= m_pos.stop)                                  { exit_px = m_pos.stop; why = "SL_HIT"; }
        else if (curL <= m_pos.tp)                               { exit_px = m_pos.tp;   why = "TP_HIT"; }
        else if (bar_seq_ - m_pos.entry_bar_seq >= MAX_HOLD)     { exit_px = curC;       why = "TIME_STOP"; }
        if (!why) return;

        const double pnl_pts = (m_pos.entry - exit_px) - COST_PTS;   // SHORT pnl
        omega::TradeRecord tr{};
        tr.engine="IndexBearShort"; tr.regime="BEAR_BREAKDOWN"; tr.symbol=symbol;
        tr.side="SHORT"; tr.entryPrice=m_pos.entry; tr.exitPrice=exit_px; tr.size=m_pos.size;
        tr.pnl = pnl_pts * m_pos.size;       // RAW pts*lot; ledger applies tick value
        tr.mfe=m_pos.mfe; tr.atr_at_entry=m_pos.atr_at_entry; tr.shadow=shadow_mode;
        tr.exitReason=why; tr.entryTs=m_pos.entry_ts/1000; tr.exitTs=ts_.back()/1000;
        if (on_close_cb) on_close_cb(tr);
        std::printf("[IBS] EXIT %s %s %s entry=%.1f exit=%.1f pnl=%.1fpt hold=%lldbars\n",
                    shadow_mode?"SHADOW":"LIVE", symbol.c_str(), why, m_pos.entry, exit_px, pnl_pts,
                    (long long)(bar_seq_ - m_pos.entry_bar_seq));
        std::fflush(stdout);
        m_pos = LivePos{}; last_exit_seq_ = bar_seq_;
    }
};

} // namespace omega
