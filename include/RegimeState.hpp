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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
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
    // 2026-06-17: optional EXTERNAL macro-hostile tightening (real-yield + dollar
    // rising hard => de-risk gold longs). Set each tick from g_macro_gold_gate
    // (fail-safe: defaults false, cleared on stale/missing feed). It ONLY adds a
    // long block ON TOP of the price core -- it can never UNblock, so the
    // "cannot silently degrade to all-clear in a bear" guarantee is preserved.
    // Validated daily-overlay, marginal value confirmed OVER the price gate:
    // bull near-free (Sharpe 1.11->1.14, maxDD -20.2%->-14.7%), bear flips
    // -2.6%->+7.8% maxDD -35.9%->-29.1% (backtest/macro_gold_regime.py).
    bool   macro_hostile_ = false;

    // tick->H1 accumulator
    int64_t acc_bar_ = -1; double a_c_ = 0.0; int a_n_ = 0;
    double last_mid_ = 0.0;   // live mid, updated every tick (for %-of-price thresholds)

    // Live price for %-of-price threshold scaling. Falls back to the last H1 close
    // (warm-seed) before the first live tick. >0 once fed.
    double price() const noexcept { return last_mid_ > 0.0 ? last_mid_ : last_close_; }

    // ---- queries (graceful: until warm => neutral => blocks nothing) ----
    bool warm()  const noexcept { return bars_ >= EMA_SLOW + PERSIST; }
    bool is_bear() const noexcept { return bear_; }         // PRICE core only (a bear engine keys off this, not macro)
    bool is_bull() const noexcept { return bull_; }
    // long_blocked = price-bear OR macro-hostile (the asymmetric bear-insurance tightening).
    bool long_blocked()  const noexcept { return bear_ || macro_hostile_; }
    bool short_blocked() const noexcept { return bull_; }   // don't short a sustained uptrend
    bool macro_hostile() const noexcept { return macro_hostile_; }
    void set_macro_hostile(bool h) noexcept { macro_hostile_ = h; }
    const char* regime_name() const noexcept { return bear_ ? "BEAR" : (bull_ ? "BULL" : (macro_hostile_ ? "MACRO-HOSTILE" : "NEUTRAL")); }

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
        last_mid_ = mid;
        const int64_t bar = now_ms/1000/BAR_SECS;
        if (a_n_ == 0) { a_c_ = mid; acc_bar_ = bar; a_n_ = 1; }
        else if (bar == acc_bar_) { a_c_ = mid; ++a_n_; }
        else { append_live_bar_(acc_bar_ * BAR_SECS, a_c_); on_h1_bar(0,0,0,a_c_); a_c_ = mid; acc_bar_ = bar; a_n_ = 1; }
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

    // ---- persist LIVE regime state across restarts (mirrors the bar-indicator auto-save) ----
    //   WHY (2026-06-23 stale-seed incident): without this the regime resets to the warm-seed
    //   CSV on every restart. With a stale seed (was April-1, gold 4692, while live=4120) +
    //   frequent restarts it never accumulates the ~13d of live H1 ticks needed to re-converge,
    //   so long_blocked() stays NEUTRAL through a real downtrend and long-only gold engines buy
    //   into it unprotected. save_state() runs every 60s from quote_loop; load_state() at boot
    //   restores the live-accurate EMAs (overriding the seed) so protection survives restarts.
    bool save_state(const std::string& path) const noexcept {
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return false;
        f << "saved_ts="   << (long long)std::time(nullptr) << "\n"
          << "emaS="       << emaS_       << "\n"
          << "emaF="       << emaF_       << "\n"
          << "bars="       << bars_       << "\n"
          << "last_close=" << last_close_ << "\n"
          << "have_ema="   << (have_ema_ ? 1 : 0) << "\n"
          << "hist_n="     << emaS_hist_.size() << "\n";
        for (double v : emaS_hist_) f << v << "\n";
        return true;
    }
    // Restore a FRESH (<= max_age_s) valid state. Returns false on missing/stale/corrupt so the
    // caller falls back to seed_from_h1_csv(). Default 12h: H1 EMAs tolerate a modest gap and
    // live ticks refill it; older than that, re-seed.
    bool load_state(const std::string& path, int max_age_s = 12 * 3600) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        int64_t saved_ts = 0; double eS = 0, eF = 0, lc = 0; int bn = 0, he = 0;
        std::vector<double> hist; std::string line;
        auto kv = [](const char* k, const std::string& s, double& out) -> bool {
            const size_t L = std::strlen(k);
            if (s.size() > L && s.compare(0, L, k) == 0 && s[L] == '=') { out = std::atof(s.c_str() + L + 1); return true; }
            return false;
        };
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            double d;
            if      (line.compare(0, 9, "saved_ts=") == 0) saved_ts = (int64_t)std::atoll(line.c_str() + 9);
            else if (kv("emaS", line, d))       eS = d;
            else if (kv("emaF", line, d))       eF = d;
            else if (kv("bars", line, d))       bn = (int)d;
            else if (kv("last_close", line, d)) lc = d;
            else if (kv("have_ema", line, d))   he = (int)d;
            else if (line.compare(0, 7, "hist_n=") == 0) { /* count implicit */ }
            else if (line[0] == '-' || line[0] == '.' || std::isdigit((unsigned char)line[0])) hist.push_back(std::atof(line.c_str()));
        }
        const int64_t now_ts = (int64_t)std::time(nullptr);
        const int64_t age = now_ts - saved_ts;
        if (saved_ts <= 0 || age < 0 || age > max_age_s || eS <= 0.0 || eF <= 0.0 || bn < EMA_SLOW) {
            std::printf("[REGIME:%s][LOAD] reject (age=%llds stale/invalid) -> will warm-seed\n", name.c_str(), (long long)age);
            return false;
        }
        emaS_ = eS; emaF_ = eF; bars_ = bn; last_close_ = lc; have_ema_ = (he != 0);
        emaS_hist_.assign(hist.begin(), hist.end());
        while ((int)emaS_hist_.size() > PERSIST + 1) emaS_hist_.pop_front();
        // re-classify from the restored state (same rule as on_h1_bar)
        bear_ = bull_ = false;
        if (warm() && (int)emaS_hist_.size() >= PERSIST + 1) {
            const double emaS_old = emaS_hist_.front();
            if (last_close_ < emaS_ && emaS_ < emaS_old && emaF_ < emaS_) bear_ = true;
            else if (last_close_ > emaS_ && emaS_ > emaS_old && emaF_ > emaS_) bull_ = true;
        }
        std::printf("[REGIME:%s][LOAD] restored emaS=%.2f emaF=%.2f bars=%d regime=%s age=%llds\n",
                    name.c_str(), emaS_, emaF_, bars_, regime_name(), (long long)age);
        std::fflush(stdout);
        return true;
    }

    // reset to cold (for re-seeding from a different source after a rejected/short seed).
    void reset() noexcept {
        emaS_hist_.clear(); emaS_ = emaF_ = 0.0; have_ema_ = false;
        bars_ = 0; last_close_ = 0.0; bear_ = bull_ = false;
    }

    // ---- self-recorded LIVE H1 seed (A+C: keep the warm-seed permanently fresh with NO external
    //   data pipeline). on_tick appends each COMPLETED live H1 bar (ts,o,h,l,c -- o=h=l=c=close,
    //   the regime only uses close). On boot we PREFER this over the static tsmom CSV once it holds
    //   >= EMA_SLOW+PERSIST bars, so the gate never re-blinds on a stale CSV after the dump fills
    //   (~13 days). Append-only; tiny (~one line/hour). Only LIVE bars are recorded (seed replay
    //   does NOT call this), so the dump is pure live history. ----
    std::string live_dump_path_;
    void set_live_dump(const std::string& p) noexcept { live_dump_path_ = p; }
    // Optional H1-close sink: fires once per COMPLETED LIVE H1 bar with (ts_sec, close) —
    // the SAME bars written to the live dump. Used to feed the GoldBeFloorCompanion so its
    // book sees exactly gold_regime_h1.csv (no duplicate aggregation, no drift). Seed replay
    // does NOT call append_live_bar_, so the sink only ever gets pure forward live bars.
    std::function<void(int64_t, double)> h1_sink_;
    void set_h1_sink(std::function<void(int64_t, double)> s) noexcept { h1_sink_ = std::move(s); }
    void append_live_bar_(int64_t ts_sec, double c) const noexcept {
        if (c <= 0.0) return;
        if (!live_dump_path_.empty()) {
            std::ofstream f(live_dump_path_, std::ios::app);
            if (f.is_open()) f << ts_sec << ',' << c << ',' << c << ',' << c << ',' << c << "\n";
        }
        if (h1_sink_) h1_sink_(ts_sec, c);
    }
};

// Per-asset singletons (resolve include-order; engines call the accessor).
inline RegimeState& gold_regime() noexcept {
    static RegimeState inst = []{ RegimeState r; r.name = "XAU"; return r; }();
    return inst;
}

// ── %-of-price threshold helper (settings-drift fix, 2026-06-12) ──────────────
//   Convert a percent-of-price band into points at the CURRENT gold price, so an
//   engine expresses a threshold once as a % and never needs re-editing when gold
//   drifts ($2400->$4700->$4213 forced two manual abs-pt bumps; this ends that).
//   Usage in a gold engine:  const double cap = omega::gold_pct_to_pts(0.40);
//   Returns 0 if price is not yet known (caller should treat 0 as "gate disabled"
//   or fall back to its abs default, exactly like the BracketEngine eff_* pattern).
inline double gold_pct_to_pts(double pct) noexcept {
    const double p = gold_regime().price();
    return p > 0.0 ? pct * 0.01 * p : 0.0;
}

// Market-wide equity regime proxy (NAS100 = bellwether). Used by IndexRiskGate as
// the PRICE-BASED FALLBACK when the macro feed (VIX/credit/dollar) is dead/stale, so
// index long engines stay protected in a real bear even with no feed. Fed from the
// NAS100 tick handler (tick_indices.hpp), warm-seeded in engine_init.
inline RegimeState& index_market_regime() noexcept {
    static RegimeState inst = []{ RegimeState r; r.name = "NAS-MKT"; return r; }();
    return inst;
}

} // namespace omega
