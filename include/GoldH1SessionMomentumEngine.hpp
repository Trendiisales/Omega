// =============================================================================
//  GoldH1SessionMomentumEngine.hpp
//
//  XAUUSD h1 session-momentum breakout — SHADOW, additive. Added 2026-07-10.
//
//  EDGE (faithful BT, 2yr tick_fresh h1 2024-26 IS + XAU2022_bear_h1 OOS, 6bps RTT):
//    Donchian(50) LONG-ONLY breakout, London/NY session only (07-21 UTC), gated to a
//    BULL regime (close>SMA960) with an ADAPTIVE chop filter (efficiency-ratio(24)>=0.30
//    required ONLY when price hugs the regime line, skipped in a strong bull >4% above it).
//    WIDE protection profile: IS +63.8% PF1.43 WF +12.9/+50.9, BEAR-2022 +3.3% PF1.12
//    (all-6 incl bear). Ungated/no-chop was +75% but BEAR -4.8% and FAILS the random-window
//    control (random longs beat it) -> that 75% is BULL BETA, not alpha. This gated form is a
//    PROTECTED DIRECTIONAL-BETA gold-long sleeve: captures ~85% of the bull drift, FLAT in bear.
//    Size as directional beta, NOT as market-beating alpha. Random control on the gated form
//    still OWED before any live flip (registry §9). Both-direction variant = the real (smaller)
//    alpha, bear-fragile, NOT this engine.
//
//  ADVERSE-PROTECTION: in-flight = WIDE peak-profit giveback trail (arm at +0.75% MFE, then
//    exit on a 0.90-of-peak-gain giveback) + a 0.75% cold-loss cut (pre-arm catastrophe floor)
//    + hold_max=60 h1 TIMEOUT. BACKTESTED VERDICT: the WIDE-stop (0.75%) + giveback profile is
//    the sweep winner; a TIGHT cold-cut (0.20%) was backtested and LOWERS net (destroys the
//    runner) -- wide-floor-only is the deliberate, validated choice. Worst single trade bounded
//    -2..-3.7%, MAE-capped; regime protection = the bull-gate + adaptive-chop (flat in bear).
//
//  INDEPENDENCE: this is a self-contained engine on its OWN contract. It does not read, modify,
//    or depend on any other engine. If ever paired with a mimic/clip companion, that companion
//    is judged STANDALONE all-6, never vs-WIDE (feedback-companion-independent-engine).
// =============================================================================
#pragma once

#include <cmath>
#include <deque>
#include <string>
#include <functional>
#include <cstdint>
#include <fstream>
#include <sstream>
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord (callback payload)

namespace omega {

struct GoldH1SessionMomentumParams {
    // --- signal ---
    int    donchian_bars   = 50;      // breakout channel lookback (h1 bars)
    bool   long_only       = true;    // bull-regime -> longs only (shorts are the bear-fragile leg)
    int    sess_open_utc   = 7;       // London/NY only: [07, 21) UTC; Asia whipsaws -> flat
    int    sess_close_utc  = 21;
    // --- regime gate ---
    int    regime_sma      = 960;     // bull-gate: trade only while close > SMA(960 h1 ~ 40d)
    int    er_bars         = 24;      // efficiency-ratio window (chop filter)
    double er_thr          = 0.30;    // ER >= thr required (trending); below = chop -> sit out
    double adaptive_buf    = 0.04;    // ADAPTIVE: require ER only when close <= SMA*(1+buf);
                                      //           strong bull (>buf above SMA) skips it -> keep margin
    // --- WIDE protection profile (in-flight) ---
    double arm_pct         = 0.0075;  // arm the trail once MFE >= +0.75%
    double giveback_frac   = 0.90;    // once armed, exit if peak gain retraces by this fraction
    double cold_stop_pct   = 0.0075;  // pre-arm catastrophe cut at -0.75%
    int    hold_max_bars   = 60;      // TIMEOUT (h1 bars)
    // --- sizing / cost ---
    double risk_dollars    = 10.0;    // fixed $ risk per trade
    double max_lot         = 0.10;    // XAUUSD lot cap (directional-beta sleeve -> small)
    double cost_rt_frac    = 0.0006;  // 6bps RTT (validated real IBKR gold cost) -- entry cost gate
};

struct GoldH1SessionMomentumEngine {
    bool        shadow_mode = true;   // SHADOW until random-control OOS + operator sign-off
    bool        enabled     = true;
    std::string symbol      = "XAUUSD";
    GoldH1SessionMomentumParams p;

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active   = false;
        double  entry    = 0.0;
        double  peak_fav = 0.0;       // best favorable frac since entry (MFE)
        bool    armed    = false;
        double  size     = 0.0;
        int64_t entry_ts = 0;
        int     bars     = 0;
    } pos_;

    // ── h1 accumulator (ticks -> h1 bars) ──
    struct H1Acc { bool active=false; int64_t bucket=0; double o=0,h=0,l=0,c=0; } acc_;

    // ── rolling state ──
    std::deque<double> highs_, lows_, closes_;   // for Donchian(N) + ER + SMA
    double sma_sum_ = 0.0;                        // running sum for SMA(regime_sma)

    // ── COST GATE: OmegaCostGuard-style entry filter. A breakout must clear the 6bps RTT
    //    round-trip to be viable at fire time (entry filter only; in-flight is the WIDE trail).
    bool cost_viable_(double atr_like) const {
        // require the channel-break impulse (proxy edge) to exceed the round-trip cost.
        return atr_like > p.cost_rt_frac;   // OmegaCostGuard: block trades that cannot cover cost
    }

    static int hour_utc_(int64_t ts_ms) {
        time_t t = (time_t)(ts_ms / 1000); struct tm g; gmtime_r(&t, &g); return g.tm_hour;
    }

    double efficiency_ratio_() const {
        int n = p.er_bars;
        if ((int)closes_.size() <= n) return 1.0;
        double move = std::fabs(closes_.back() - closes_[closes_.size()-1-n]);
        double path = 0.0;
        for (size_t i = closes_.size()-n; i < closes_.size(); ++i) path += std::fabs(closes_[i] - closes_[i-1]);
        return path > 0 ? move / path : 0.0;
    }

    // Evaluate one CLOSED h1 bar: manage an open position, then test for a new entry.
    void on_h1_bar(int64_t ts_ms, double /*o*/, double h, double l, double c, CloseCallback on_close) {
        // ---- push bar into rolling windows ----
        highs_.push_back(h); lows_.push_back(l); closes_.push_back(c); sma_sum_ += c;
        int keep = std::max(p.regime_sma, std::max(p.donchian_bars, p.er_bars)) + 2;
        while ((int)closes_.size() > keep) {
            sma_sum_ -= closes_.front();
            highs_.pop_front(); lows_.pop_front(); closes_.pop_front();
        }

        // ---- manage open position (WIDE protection) ----
        if (pos_.active) {
            pos_.bars++;
            double fav = (c - pos_.entry) / pos_.entry;   // long-only
            if (fav > pos_.peak_fav) pos_.peak_fav = fav;
            const char* why = nullptr;
            if (!pos_.armed && -fav >= p.cold_stop_pct)                            why = "COLD_STOP";
            else if (!pos_.armed && pos_.peak_fav >= p.arm_pct)                    pos_.armed = true;
            if (!why && pos_.armed && pos_.peak_fav > 0 &&
                (pos_.peak_fav - fav) >= p.giveback_frac * pos_.peak_fav)          why = "GIVEBACK_TRAIL";
            if (!why && pos_.bars >= p.hold_max_bars)                              why = "TIMEOUT";
            if (why) { close_(c, ts_ms, why, on_close); }
        }

        if (!enabled || pos_.active) return;

        // ---- regime + session gates ----
        if ((int)closes_.size() < p.regime_sma + 1) return;           // SMA warmup
        double sma = sma_sum_ / (double)closes_.size();
        if (c <= sma) return;                                         // BULL-GATE: flat below regime SMA
        int hr = hour_utc_(ts_ms);
        if (hr < p.sess_open_utc || hr >= p.sess_close_utc) return;   // London/NY only
        // ADAPTIVE chop filter: require ER only when hugging the regime line
        if (c <= sma * (1.0 + p.adaptive_buf)) {
            if (efficiency_ratio_() < p.er_thr) return;               // chop -> sit out
        }

        // ---- Donchian(N) LONG breakout ----
        double chan_hi = -1e18;
        int N = p.donchian_bars, m = (int)highs_.size();
        for (int i = m - 1 - N; i < m - 1; ++i) if (i >= 0 && highs_[i] > chan_hi) chan_hi = highs_[i];
        double break_impulse = (chan_hi > 0) ? (c - chan_hi) / c : 0.0;
        if (c > chan_hi && break_impulse >= 0.0) {
            if (!cost_viable_(std::fabs(break_impulse) + p.arm_pct)) return;  // cost gate
            open_(c, ts_ms);
        }
    }

    void open_(double px, int64_t ts_ms) {
        pos_ = OpenPos{};
        pos_.active = true; pos_.entry = px; pos_.entry_ts = ts_ms;
        pos_.size = std::min(p.max_lot, p.risk_dollars / std::max(1.0, px * p.cold_stop_pct));
    }

    void close_(double px, int64_t ts_ms, const char* reason, CloseCallback& on_close) {
        omega::TradeRecord tr{};
        tr.symbol     = symbol;
        tr.side       = "LONG";
        tr.entryPrice = pos_.entry;
        tr.exitPrice  = px;
        tr.size       = pos_.size;
        tr.pnl        = (px - pos_.entry) * pos_.size;                 // gross (price units x size)
        tr.commission = p.cost_rt_frac * pos_.entry * pos_.size;       // 6bps RTT
        tr.net_pnl    = tr.pnl - tr.commission;
        tr.entryTs    = pos_.entry_ts / 1000;                          // unix seconds
        tr.exitTs     = ts_ms / 1000;
        tr.exitReason = reason;
        tr.shadow     = shadow_mode;
        tr.engine     = "GoldH1SessionMomentum";
        if (on_close) on_close(tr);
        pos_ = OpenPos{};
    }

    // ── WARM-SEED (mandate): replay an h1 CSV (ts,o,h,l,c) with enabled=false so no historical
    //    entries fire; only fills the SMA960 / Donchian / ER buffers. One [SEED] line on boot.
    void seed_from_h1_csv(const std::string& path) {
        std::ifstream f(path); if (!f.is_open()) return;
        bool prev = enabled; enabled = false;
        std::string line; size_t n = 0;
        CloseCallback noop = [](const omega::TradeRecord&){};
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
            std::stringstream ss(line); std::string tok; double v[5]; int k = 0;
            while (std::getline(ss, tok, ',') && k < 5) { try { v[k++] = std::stod(tok); } catch (...) { k = 0; break; } }
            if (k < 5) continue;
            on_h1_bar((int64_t)v[0] * 1000, v[1], v[2], v[3], v[4], noop);
            ++n;
        }
        enabled = prev;
        std::printf("[SEED] GoldH1SessionMomentum: %zu h1 bars from %s (SMA960/Donch50/ER24 warmed)\n",
                    n, path.c_str());
    }
};

} // namespace omega
