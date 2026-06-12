#pragma once
// =============================================================================
//  RegimeState.hpp -- shared, self-contained PRICE-BASED bull/bear regime brain.
//
//  PROVENANCE (2026-06-12, gold_regime_gate_bt.cpp on XAUUSD H1 2020-2023):
//    The deep regime investigation tested two ways to gate long-only engines so
//    they stop buying into a downtrend:
//      (a) D1 EMA200-SLOPE (GoldD1TrendState): slope<-thr => DOWNTREND. LAGS --
//          stays DOWNTREND into the recovery, blocks the best bounce longs. On
//          XAU 2020-2023 it made BOTH a Donchian-breakout long AND an RSI-
//          oversold long WORSE (Donchian +141->+52, RSI -262->-299). REJECTED.
//      (b) SUSTAINED-BEAR (the IndexBearShortEngine gate): close<EMA200 AND
//          EMA200 falling over PERSIST bars AND EMA50<EMA200. Releases as soon as
//          price reclaims EMA200, so it protects the downtrend WITHOUT eating the
//          recovery. On XAU 2020-2023 it IMPROVED both (Donchian +141->+153
//          PF1.07->1.09, H2/bear bleed -189->-85; RSI -262->-172, bleed cut 35%).
//          ACCEPTED. PERSIST=100 H1 bars (the IBS-validated value; robust 100-150,
//          P200+ starts to hurt the mean-rev side).
//
//  This module packages (b) as a singleton per asset class so EVERY directional
//  engine can consult ONE shared read instead of re-deriving it. It is PRICE-only
//  and self-contained -- NO external feed, so (unlike IndexRiskGate's macro feed)
//  it can never silently degrade to "all-clear" during a real bear. The macro
//  IndexRiskGate is layered ON TOP as an optional further-tightening, never a
//  replacement (decision 2026-06-12: price core + macro tighten).
//
//  USAGE (long-only engine, before taking a NEW long):
//      if (omega::gold_regime().long_blocked()) return;   // skip long in bear
//  Bidirectional engine:
//      if (sideLong  && omega::gold_regime().long_blocked())  skip;
//      if (sideShort && omega::gold_regime().short_blocked()) skip;  // don't short a bull
//  A future bear engine activates when omega::gold_regime().is_bear().
//
//  FEED: call on_h1_bar() once per CLOSED H1 bar, OR on_tick() every tick (it
//  aggregates H1 internally, same convention as IndexBearShortEngine). Warm-seed
//  from an H1 CSV at startup (CLAUDE.md mandate) so the regime is queryable on the
//  first live tick instead of cold-warming for ~13 days.
// =============================================================================
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <string>
#include "SeedGuard.hpp"   // resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

struct RegimeState {
    // --- params (IBS-validated; tunable per asset before wiring) ---
    int    EMA_SLOW = 200;     // H1 EMA, the trend spine (~8.5 calendar days on gold)
    int    EMA_FAST = 50;
    int    PERSIST  = 100;     // EMA_SLOW must be below its value PERSIST bars ago
    int    BAR_SECS = 3600;    // H1 decision bars

    std::string name = "REGIME";

    // --- state ---
    std::deque<double> emaS_hist_;   // last PERSIST+1 EMA_SLOW values (for the falling check)
    double emaS_ = 0.0, emaF_ = 0.0;
    bool   have_ema_ = false;
    int    bars_ = 0;
    double last_close_ = 0.0;
    bool   bear_ = false, bull_ = false;

    // tick->H1 accumulator
    int64_t acc_bar_ = -1; double a_c_ = 0.0; int a_n_ = 0;

    // ---- queries (graceful: until warm => neutral => blocks nothing) ----
    bool warm()  const noexcept { return bars_ >= EMA_SLOW + PERSIST; }
    bool is_bear() const noexcept { return bear_; }
    bool is_bull() const noexcept { return bull_; }
    bool long_blocked()  const noexcept { return bear_; }   // don't buy a sustained downtrend
    bool short_blocked() const noexcept { return bull_; }   // don't short a sustained uptrend
    const char* regime_name() const noexcept { return bear_ ? "BEAR" : (bull_ ? "BULL" : "NEUTRAL"); }

    // ---- feed: one CLOSED H1 bar ----
    void on_h1_bar(double /*o*/, double /*h*/, double /*l*/, double c) noexcept {
        const double kS = 2.0/(EMA_SLOW+1), kF = 2.0/(EMA_FAST+1);
        if (!have_ema_) { emaS_ = c; emaF_ = c; have_ema_ = true; }
        else { emaS_ += kS*(c-emaS_); emaF_ += kF*(c-emaF_); }
        emaS_hist_.push_back(emaS_);
        while ((int)emaS_hist_.size() > PERSIST+1) emaS_hist_.pop_front();
        last_close_ = c; ++bars_;
        // classify (symmetric): sustained bear / sustained bull
        bear_ = bull_ = false;
        if (warm() && (int)emaS_hist_.size() >= PERSIST+1) {
            const double emaS_old = emaS_hist_.front();
            if (c < emaS_ && emaS_ < emaS_old && emaF_ < emaS_) bear_ = true;
            else if (c > emaS_ && emaS_ > emaS_old && emaF_ > emaS_) bull_ = true;
        }
    }

    // ---- feed: every tick (aggregates H1 like IndexBearShortEngine) ----
    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        const double mid = (bid+ask)*0.5;
        const int64_t bar = now_ms/1000/BAR_SECS;
        if (a_n_ == 0) { a_c_ = mid; acc_bar_ = bar; a_n_ = 1; }
        else if (bar == acc_bar_) { a_c_ = mid; ++a_n_; }
        else { on_h1_bar(0,0,0,a_c_); a_c_ = mid; acc_bar_ = bar; a_n_ = 1; }
    }

    // ---- warm-seed from H1 CSV (ts,o,h,l,c; ts in sec or ms) ----
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);   // VPS-cwd robust
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[REGIME:%s][SEED] MISS %s (regime cold-warms ~13d -- blocks nothing until then)\n", name.c_str(), path.c_str()); return 0; }
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]))) continue;
            double ts=0,o=0,h=0,l=0,c=0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) == 5 && c > 0) {
                on_h1_bar(o,h,l,c); ++n;
            }
        }
        std::printf("[REGIME:%s][SEED] %zu H1 bars -> EMA200=%.2f EMA50=%.2f regime=%s warm=%d\n",
                    name.c_str(), n, emaS_, emaF_, regime_name(), (int)warm());
        std::fflush(stdout);
        return n;
    }
};

// Per-asset singletons (resolve include-order; engines call the accessor).
inline RegimeState& gold_regime() noexcept {
    static RegimeState inst = []{ RegimeState r; r.name = "XAU"; return r; }();
    return inst;
}

} // namespace omega
