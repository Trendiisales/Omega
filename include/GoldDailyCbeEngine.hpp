#pragma once
// GoldDailyCbeEngine.hpp — Gold Daily engine: Asian-range break->retrace->confirm
// LONG entries, daily-ATR management, multi-day ATR trail. Operator-spec build
// (2026-07-22 pasted spec), certified LONG-only by faithful M1 backtest
// backtest/gold_daily_cbe_bt.cpp on the CERTIFIED 2022-2026 splice:
//   BE-off / multi-day cell: n79 netR +21.7 PF 2.39, ALL THREE regimes positive
//   (2022bear 1.79 / 2023chop 1.28 / 2024-26bull 3.22), WF halves 1.68/3.19,
//   2x-cost PF 2.30, SLxTRAIL plateau 2.04-2.51 (all 4 neighbors WF-both+).
//   SHORT side FAILS every config (PF<=0.60; BE-off 1.20 n21 thin) — NOT built.
//   Full grid: backtest/GOLD_DAILY_CBE_FINDINGS_2026-07-22.md.
//
// COST GATE: every live entry routes through the injected gate_fn_ =
//   ExecutionCostGuard::is_viable (wired in engine_init) before open_fn_ fires.
//
// ADVERSE-PROTECTION: SL = 1.75x ATR14(D1) from entry (initial hard stop) +
//   50% partial de-risk at +1R + 2.0x ATR trail off peak close (0.75x tighten
//   after +2R) — backtested; BE-ratchet variants LOWER net (PF 2.39 -> 1.61-1.85
//   at BE 0.3-0.5xATR; the spec's $1 BE trigger collapses it to PF 0.00), and a
//   tighter cold-cut is strictly dominated by the 1.75xATR stop on this grid.
//
// VENUE: IBKR SPOT gold ("XAUUSD.S" -> CMDTY XAUUSD/SMART, fractional-oz), NOT
//   MGC futures (operator 2026-07-22: commodities-segment margin blocks MGC at
//   current account size). Cost basis: 1.5bp/side comm + measured $0.34 spread.
//
// FEED: on_tick_gold() XAUUSD mid ticks -> internal M1 aggregation (backtest
//   parity: all decisions on M1 closes; day roll at 17:00 ET DST-correct).
// SEED: phase1/signal_discovery/warmup_XAUUSD_D1_OHLC.csv (2015-2026 real-OHLC
//   dailies, ts SECONDS, for EMA200/ATR14/ATR-band warmup — own file; the shared
//   warmup_XAUUSD_D1.csv is a different ms-ts flat-close format) + own live daily
//   dump appended forward (deploy-forward; forward bars self-recorded).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace omega {

class GoldDailyCbeEngine {
public:
    struct Config {
        bool        enabled       = false;
        bool        live_book     = false;   // false = book/ledger only, no broker orders
        std::string live_sym      = "XAUUSD.S";
        std::string engine_tag    = "GoldDailyCBE";
        double      lot_oz        = 1.0;     // ounces per position (spot, fractional OK)
        double      sl_mult       = 1.75;    // x ATR14(D1)
        double      trail_mult    = 2.0;     // x ATR14(D1) off peak M1 close, D1-close ratchet
        double      trail_tighten = 0.75;    // trail_mult multiplier after +2R
        double      partial_r     = 1.0;     // 50% off at +1R
        double      retrace_frac  = 0.25;    // retrace into range before confirm
        double      minrng_pct    = 0.004;   // Asian range >= 0.4% of price
        bool        atr_band      = true;    // ATR14 within P10-P90 of trailing 120d
        std::string state_path    = "golddailycbe_live.txt";
        std::string dump_path     = "golddailycbe_daily.csv";
        // S-22j GLD->MGC auto-switch (operator: "keep gld until the 5k lands then
        // switch to mgc"): when the GOLD-PROBE sees an MGC whatIf clear margin,
        // it flips use_mgc — NEW entries route mgc_sym/mgc_lot (1 contract = 10 oz,
        // 23h session, no ETF-hours gap). An OPEN GLD position finishes its ride
        // on GLD (token-matched close). Flag is atomic; string members are never
        // mutated after boot.
        std::string mgc_sym       = "XAUUSD.M";
        double      mgc_lot       = 1.0;
        // boot Asian-range backfill sources (VPS-relative; missing = fallback)
        std::string l2_prefix     = "logs/l2_ticks_XAUUSD_";
        std::string h4_csv_path   = "logs/gold_d1_trend_h4.csv";
    };
    Config cfg;
    std::atomic<bool> use_mgc{false};          // flipped by the GOLD-PROBE on margin fit
    const std::string& eff_sym() const { return use_mgc.load() ? cfg.mgc_sym : cfg.live_sym; }
    double eff_lot() const { return use_mgc.load() ? cfg.mgc_lot : cfg.lot_oz; }

    using OpenFn   = std::function<std::string(const std::string&, bool, double, double)>;
    using CloseFn  = std::function<void(const std::string&, bool, double, double, const std::string&)>;
    using GateFn   = std::function<bool(const std::string&, double, double)>;
    using LedgerFn = std::function<void(const std::string&, const std::string&, bool,
                                        double, double, double, int64_t, int64_t, const char*)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) {
        open_fn_ = std::move(o); close_fn_ = std::move(c);
        gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    // ---- daily seed (ts,o,h,l,c; header ok). Returns rows consumed. ----
    size_t seed_daily_csv(const std::string& path) {
        size_t n = 0;
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[GOLD-CBE][SEED] MISS %s\n", path.c_str()); return 0; }
        std::string ln;
        while (std::getline(f, ln)) {
            long long ts; double o, h, l, c;
            if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) == 5) {
                push_daily_(ts, h, l, c); ++n;
            }
        }
        // replay own forward dump (bars closed live since the seed was cut)
        std::ifstream d(cfg.dump_path);
        while (d.is_open() && std::getline(d, ln)) {
            long long ts; double o, h, l, c;
            if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) == 5) {
                if (ts > last_daily_ts_) { push_daily_(ts, h, l, c); ++n; }
            }
        }
        load_state_();
        std::printf("[GOLD-CBE][SEED] %zu daily bars (ema200 %s, atr %.2f), pos=%s\n",
                    n, ema_n_ >= 200 ? "ready" : "COLD", atr_ready_() ? atr_() : 0.0,
                    pos_.active ? "OPEN(restored)" : "flat");
        std::fflush(stdout);
        return n;
    }

    // ---- live tick (mid of the spot XAUUSD stream) ----
    void on_tick(double bid, double ask, int64_t now_ms) {
        if (!cfg.enabled || bid <= 0.0 || ask <= 0.0) return;
        const double mid = 0.5 * (bid + ask);
        const int64_t m1 = now_ms / 60000;
        if (m1_id_ == 0) { m1_id_ = m1; m1_o_ = m1_h_ = m1_l_ = m1_c_ = mid; return; }
        if (m1 != m1_id_) {
            on_m1_close_(m1_id_ * 60, m1_o_, m1_h_, m1_l_, m1_c_);
            m1_id_ = m1; m1_o_ = m1_h_ = m1_l_ = m1_c_ = mid;
        } else {
            m1_h_ = std::max(m1_h_, mid); m1_l_ = std::min(m1_l_, mid); m1_c_ = mid;
        }
    }

    bool position_open() const { return pos_.active; }

    // ── companion event broadcast (S-22i mimic x2) ──────────────────────────
    // Fire-and-forget hooks; the parent's decisions are NEVER affected by the
    // companion (feedback-companion-independent-engine). Wired in engine_init.
    std::function<void(double, int64_t)> on_open_hook;    // parent entry px, ts
    std::function<void(double, int64_t)> on_m1_hook;      // M1 close, ts
    std::function<void(double, int64_t)> on_close_hook;   // parent exit px, ts

    // ── Boot backfill (operator order 2026-07-22: "it goes live now, why wait
    // for asian") — reconstruct TODAY's Asian range + post-08:00 break/retrace
    // state from on-box intraday files instead of waiting to observe a full
    // 00-08 UTC session live. Best-effort sources (VPS cwd C:\Omega):
    //   1. logs/l2_ticks_XAUUSD_<YYYY-MM-DD>.csv  header ts_ms,mid,bid,ask,...
    //   2. logs/gold_d1_trend_h4.csv              ts_ms,o,h,l,c (4h bars; the
    //      00:00 UTC bar patches any tick-file gap back to midnight)
    // NEVER enters or books during backfill — only the range + break/retrace
    // flags; entries fire on live M1 confirm closes after it. Missing files =
    // graceful fallback to live observation (Mac/backtest TUs unaffected).
    void backfill_asian_today(int64_t now_ms) {
        if (!cfg.enabled) return;
        const int64_t now_s = now_ms / 1000;
        struct tm gu; utc_tm_(now_s, gu);
        const int64_t day0 = now_s - (gu.tm_hour * 3600 + gu.tm_min * 60 + gu.tm_sec);
        const int64_t a_end = day0 + 8 * 3600;
        const long udk = (long)(gu.tm_year + 1900) * 10000 + (gu.tm_mon + 1) * 100 + gu.tm_mday;
        double hi = 0.0, lo = 0.0; int64_t earliest = 0;
        bool h4_00 = false;
        // (2) H4 bars: 00:00 (+ forming 04:00) bar of today
        {
            std::ifstream f(cfg.h4_csv_path);
            std::string ln;
            while (f.is_open() && std::getline(f, ln)) {
                long long ts; double o, h, l, c;
                if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
                const int64_t s = ts >= 100000000000LL ? ts / 1000 : ts;
                if (s == day0 || s == day0 + 4 * 3600) {
                    if (hi == 0.0 || h > hi) hi = std::max(hi, h);
                    if (lo == 0.0 || l < lo) lo = (lo == 0.0) ? l : std::min(lo, l);
                    if (s == day0) h4_00 = true;
                }
            }
        }
        // (1) tick file: range inside [00:00,08:00) + break/retrace replay after
        char datebuf[16];
        std::snprintf(datebuf, sizeof datebuf, "%04d-%02d-%02d",
                      gu.tm_year + 1900, gu.tm_mon + 1, gu.tm_mday);
        const std::string tick_path = cfg.l2_prefix + datebuf + ".csv";
        std::vector<double> post;                     // post-08:00 mids for state replay
        {
            std::ifstream f(tick_path);
            std::string ln; bool hdr = true;
            while (f.is_open() && std::getline(f, ln)) {
                if (hdr) { hdr = false; if (!ln.empty() && (ln[0] < '0' || ln[0] > '9')) continue; }
                long long ts; double mid;
                if (std::sscanf(ln.c_str(), "%lld,%lf", &ts, &mid) != 2 || mid <= 0.0) continue;
                const int64_t s = ts >= 100000000000LL ? ts / 1000 : ts;
                if (s < day0 || s > now_s) continue;
                if (s < a_end) {
                    if (earliest == 0 || s < earliest) earliest = s;
                    if (hi == 0.0 || mid > hi) hi = std::max(hi, mid);
                    if (lo == 0.0 || mid < lo) lo = (lo == 0.0) ? mid : std::min(lo, mid);
                } else {
                    post.push_back(mid);
                }
            }
        }
        if (hi <= 0.0 || lo <= 0.0 || hi <= lo) {
            std::printf("[GOLD-CBE][ASIAN-BACKFILL] no usable intraday data (%s / %s) -- live observation fallback\n",
                        tick_path.c_str(), cfg.h4_csv_path.c_str());
            std::fflush(stdout);
            return;
        }
        const bool full = h4_00 || (earliest > 0 && earliest <= day0 + 1800);
        asian_day_ = udk; a_h_ = hi; a_l_ = lo; a_live_ = true;
        a_seen_open_ = full;
        a_done_ = (now_s >= a_end);
        a_full_ = a_done_ ? full : false;             // pre-08:00 boot: flags finalize live
        broke_ = retraced_ = false;
        // replay post-Asian mids through the break/retrace flags only
        const double rng = a_h_ - a_l_;
        for (double m : post) {
            if (!broke_) { if (m > a_h_) broke_ = true; }
            else if (!retraced_) { if (m <= a_h_ - cfg.retrace_frac * rng) retraced_ = true; }
        }
        std::printf("[GOLD-CBE][ASIAN-BACKFILL] day=%ld range %.2f-%.2f (%.2f) full=%d done=%d "
                    "broke=%d retraced=%d (ticks_from=%lld h4_00=%d post_mids=%zu)\n",
                    udk, a_l_, a_h_, rng, (int)full, (int)a_done_, (int)broke_, (int)retraced_,
                    (long long)earliest, (int)h4_00, post.size());
        std::fflush(stdout);
    }

    // KILL-ALL panic closer (on_tick.hpp fan-out): force-close the open position
    // through the normal close/ledger path at the live mid (0 = last M1 close).
    int kill_all(double px, int64_t now_sec) {
        if (!pos_.active) return 0;
        const double fill = px > 0.0 ? px : (m1_c_ > 0.0 ? m1_c_ : pos_.entry);
        if (!pos_.token.empty() && close_fn_)
            close_fn_(pos_.sym.empty() ? cfg.live_sym : pos_.sym, true,
                      (pos_.lot > 0 ? pos_.lot : cfg.lot_oz) * pos_.size_frac, fill, pos_.token);
        if (ledger_fn_)
            ledger_fn_(cfg.engine_tag, pos_.sym.empty() ? cfg.live_sym : pos_.sym, true, pos_.entry, fill,
                       (pos_.lot > 0 ? pos_.lot : cfg.lot_oz) * pos_.size_frac, pos_.entry_ts, now_sec, "MANUAL_KILL_ALL");
        pos_ = Pos{};
        save_state_();
        if (on_close_hook) on_close_hook(fill, now_sec);
        return 1;
    }

private:
    // ---- indicators over COMEX-roll dailies ----
    double ema200_ = 0.0; int ema_n_ = 0;
    std::deque<double> tr_; double prev_dc_ = 0.0; bool has_dc_ = false;
    std::deque<double> atr_hist_;
    int64_t last_daily_ts_ = 0;
    double prev_close_ = 0.0, ema_prev_ = 0.0, atr_prev_ = 0.0;
    bool ind_valid_ = false;

    bool  atr_ready_() const { return tr_.size() >= 14; }
    double atr_() const { double s = 0; for (double x : tr_) s += x; return s / (double)tr_.size(); }

    void push_daily_(int64_t ts, double h, double l, double c) {
        double t = h - l;
        if (has_dc_) t = std::max(t, std::max(std::fabs(h - prev_dc_), std::fabs(l - prev_dc_)));
        tr_.push_back(t); if (tr_.size() > 14) tr_.pop_front();
        prev_dc_ = c; has_dc_ = true;
        if (ema_n_ == 0) ema200_ = c; else ema200_ += (2.0 / 201.0) * (c - ema200_);
        ++ema_n_;
        if (atr_ready_()) { atr_hist_.push_back(atr_()); if (atr_hist_.size() > 120) atr_hist_.pop_front(); }
        last_daily_ts_ = ts;
        prev_close_ = c; ema_prev_ = ema200_; atr_prev_ = atr_ready_() ? atr_() : 0.0;
        ind_valid_ = (ema_n_ >= 200) && atr_ready_();
    }

    // ---- live day/M1 state ----
    int64_t m1_id_ = 0; double m1_o_ = 0, m1_h_ = 0, m1_l_ = 0, m1_c_ = 0;
    long    day_key_ = -1;                       // COMEX-roll day
    double  d_o_ = 0, d_h_ = 0, d_l_ = 0, d_c_ = 0; bool d_live_ = false;
    int64_t d_ts_ = 0;
    long    asian_day_ = -1; double a_h_ = 0, a_l_ = 0;
    bool    a_live_ = false, a_done_ = false, a_full_ = false, a_seen_open_ = false;
    bool    broke_ = false, retraced_ = false, traded_today_ = false;

    struct Pos {
        bool active = false; double entry = 0, sl = 0, rdist = 0;
        double size_frac = 1.0;                  // 1.0 -> 0.5 after partial
        bool partial = false; double peak_close = 0, trail = 0; bool trail_on = false;
        int64_t entry_ts = 0; double atr_e = 0; std::string token;
        std::string sym; double lot = 0;         // venue captured AT OPEN (GLD vs MGC switch)
    } pos_;

    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    static void utc_tm_(int64_t ts_sec, struct tm& g) {
#if defined(_WIN32)
        time_t tt = (time_t)ts_sec; gmtime_s(&g, &tt);
#else
        time_t tt = (time_t)ts_sec; gmtime_r(&tt, &g);
#endif
    }
    static int et_off_(int64_t ts_sec) {
        // 2nd-Sun-Mar / 1st-Sun-Nov, computed on the UTC calendar (matches backtest)
        struct tm g; utc_tm_(ts_sec, g);
        const int y = g.tm_year + 1900;
        auto nth_sun = [&](int m, int n) {
            for (int d = 1, c = 0; d <= 31; ++d) {
                struct tm t{}; t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d; t.tm_hour = 12;
#if defined(_WIN32)
                time_t x = _mkgmtime(&t); struct tm gg; gmtime_s(&gg, &x);
#else
                time_t x = timegm(&t); struct tm gg; gmtime_r(&x, &gg);
#endif
                if (gg.tm_mon == m - 1 && gg.tm_wday == 0 && ++c == n) return d;
            }
            return -1;
        };
        const long k  = (long)(y) * 10000 + (g.tm_mon + 1) * 100 + g.tm_mday;
        const long s_ = (long)(y) * 10000 + 3 * 100 + nth_sun(3, 2);
        const long e_ = (long)(y) * 10000 + 11 * 100 + nth_sun(11, 1);
        return (k >= s_ && k < e_) ? -4 : -5;
    }

    void on_m1_close_(int64_t ts_sec, double o, double h, double l, double c) {
        (void)o;
        // -- COMEX day roll --
        const int et = et_off_(ts_sec);
        const int64_t et_ts = ts_sec + (int64_t)et * 3600;
        struct tm ge; utc_tm_(et_ts, ge);
        long dk = (long)(ge.tm_year + 1900) * 10000 + (ge.tm_mon + 1) * 100 + ge.tm_mday;
        if (ge.tm_hour >= 17) {
            struct tm gn; utc_tm_(et_ts + 7 * 3600, gn);
            dk = (long)(gn.tm_year + 1900) * 10000 + (gn.tm_mon + 1) * 100 + gn.tm_mday;
        }
        if (dk != day_key_) {
            if (d_live_) {
                push_daily_(d_ts_, d_h_, d_l_, d_c_);
                append_dump_(d_ts_, d_o_, d_h_, d_l_, d_c_);
                trail_ratchet_(d_c_);
            }
            day_key_ = dk; d_live_ = false; traded_today_ = false;
            broke_ = retraced_ = false;
        }
        if (!d_live_) { d_o_ = o; d_h_ = h; d_l_ = l; d_c_ = c; d_ts_ = ts_sec; d_live_ = true; }
        else { d_h_ = std::max(d_h_, h); d_l_ = std::min(d_l_, l); d_c_ = c; }

        // -- Asian range 00:00-08:00 UTC (spec literal), full-session-only --
        struct tm gu; utc_tm_(ts_sec, gu);
        const long udk = (long)(gu.tm_year + 1900) * 10000 + (gu.tm_mon + 1) * 100 + gu.tm_mday;
        if (gu.tm_hour < 8) {
            if (asian_day_ != udk) {
                asian_day_ = udk; a_h_ = h; a_l_ = l; a_live_ = true; a_done_ = false;
                a_seen_open_ = (gu.tm_hour == 0);   // engine watched the session start
                broke_ = retraced_ = false;
            } else { a_h_ = std::max(a_h_, h); a_l_ = std::min(a_l_, l); }
        } else if (a_live_ && asian_day_ == udk && !a_done_) {
            a_done_ = true; a_full_ = a_seen_open_;   // partial session (boot mid-Asia) never trades
        }

        manage_(h, l, ts_sec);
        maybe_enter_(h, l, c, ts_sec, gu.tm_hour);
        if (on_m1_hook) on_m1_hook(c, ts_sec);
    }

    void trail_ratchet_(double day_close) {
        if (!pos_.active || !pos_.trail_on) return;
        double m = cfg.trail_mult;
        if (day_close - pos_.entry > 2.0 * pos_.rdist) m *= cfg.trail_tighten;
        const double lvl = pos_.peak_close - m * pos_.atr_e;
        if (lvl > pos_.trail) { pos_.trail = lvl; save_state_(); }
    }

    void manage_(double h, double l, int64_t ts_sec) {
        if (!pos_.active) return;
        const double R = pos_.rdist;
        // 50% partial at +1R (level fill)
        if (!pos_.partial && h - pos_.entry >= R * cfg.partial_r) {
            const double px = pos_.entry + R * cfg.partial_r;
            if (!pos_.token.empty() && close_fn_)
                close_fn_(pos_.sym, true, pos_.lot * 0.5, px, pos_.token);
            if (ledger_fn_)
                ledger_fn_(cfg.engine_tag, pos_.sym, true, pos_.entry, px,
                           pos_.lot * 0.5, pos_.entry_ts, ts_sec, "PARTIAL_1R");
            pos_.partial = true; pos_.size_frac = 0.5; pos_.trail_on = true;
            pos_.peak_close = px;
            pos_.trail = pos_.entry - cfg.trail_mult * pos_.atr_e;
            std::printf("[GOLD-CBE][PARTIAL] 50%% off @%.2f (+1R), trail armed\n", px);
            std::fflush(stdout);
            save_state_();
        }
        if (pos_.trail_on && m1_c_ > pos_.peak_close) pos_.peak_close = m1_c_;
        const double stop = pos_.trail_on ? std::max(pos_.sl, pos_.trail) : pos_.sl;
        if (l <= stop) {
            const double px = stop;   // worse-of gap handled by real fill reconciliation
            if (!pos_.token.empty() && close_fn_)
                close_fn_(pos_.sym, true, pos_.lot * pos_.size_frac, px, pos_.token);
            if (ledger_fn_)
                ledger_fn_(cfg.engine_tag, pos_.sym, true, pos_.entry, px,
                           pos_.lot * pos_.size_frac, pos_.entry_ts, ts_sec,
                           pos_.trail_on ? "TRAIL_STOP" : "SL_HIT");
            std::printf("[GOLD-CBE][CLOSE] %s @%.2f entry=%.2f\n",
                        pos_.trail_on ? "TRAIL_STOP" : "SL_HIT", px, pos_.entry);
            std::fflush(stdout);
            pos_ = Pos{};
            save_state_();
            if (on_close_hook) on_close_hook(px, ts_sec);
        }
    }

    void maybe_enter_(double h, double l, double c, int64_t ts_sec, int utc_hour) {
        if (pos_.active || traded_today_ || !a_done_ || !a_full_ || utc_hour < 8 || !ind_valid_) return;
        const double rng = a_h_ - a_l_;
        if (rng < cfg.minrng_pct * c) return;
        if (prev_close_ <= ema_prev_) return;                      // D1 EMA200 trend gate
        if (cfg.atr_band && atr_hist_.size() >= 60) {
            std::vector<double> s(atr_hist_.begin(), atr_hist_.end());
            std::sort(s.begin(), s.end());
            if (atr_prev_ < s[s.size() / 10] || atr_prev_ > s[s.size() * 9 / 10]) return;
        }
        if (!broke_) { if (h > a_h_) broke_ = true; return; }
        if (!retraced_) { if (l <= a_h_ - cfg.retrace_frac * rng) retraced_ = true; return; }
        if (c <= a_h_) return;                                     // confirm: M1 close back above
        const double sl = c - cfg.sl_mult * atr_prev_;
        const double R  = c - sl;
        if (R <= 0) return;
        traded_today_ = true;
        const std::string vsym = eff_sym();
        const double      vlot = eff_lot();
        std::string tok;
        if (cfg.live_book && open_fn_) {
            if (gate_fn_ && !gate_fn_(vsym, R, vlot)) {
                std::printf("[GOLD-CBE][GATE] entry blocked by cost gate @%.2f\n", c);
                std::fflush(stdout);
                return;
            }
            tok = open_fn_(vsym, true, vlot, c);
        }
        pos_.active = true; pos_.entry = c; pos_.sl = sl; pos_.rdist = R;
        pos_.size_frac = 1.0; pos_.partial = false; pos_.trail_on = false;
        pos_.peak_close = c; pos_.trail = 0; pos_.entry_ts = ts_sec;
        pos_.atr_e = atr_prev_; pos_.token = tok;
        pos_.sym = vsym; pos_.lot = vlot;
        std::printf("[GOLD-CBE][OPEN] LONG @%.2f sl=%.2f (1.75xATR %.2f) rng=%.2f tok=%s\n",
                    c, sl, atr_prev_, rng, tok.empty() ? "(book-only)" : tok.c_str());
        std::fflush(stdout);
        save_state_();
        if (on_open_hook) on_open_hook(c, ts_sec);
    }

    // ---- persistence ----
    void append_dump_(int64_t ts, double o, double h, double l, double c) const {
        std::ofstream f(cfg.dump_path, std::ios::app);
        if (f.is_open()) f << (long long)ts << "," << o << "," << h << "," << l << "," << c << "\n";
    }
    void save_state_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << (pos_.active ? 1 : 0) << " " << pos_.entry << " " << pos_.sl << " "
            << pos_.rdist << " " << pos_.size_frac << " " << (pos_.partial ? 1 : 0) << " "
            << (pos_.trail_on ? 1 : 0) << " " << pos_.peak_close << " " << pos_.trail << " "
            << (long long)pos_.entry_ts << " " << pos_.atr_e << " "
            << (pos_.token.empty() ? "-" : pos_.token) << " "
            << (pos_.sym.empty() ? "-" : pos_.sym) << " " << pos_.lot << "\n"; }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
    void load_state_() {
        std::ifstream f(cfg.state_path);
        if (!f.is_open()) return;
        int act = 0, pa = 0, tr = 0; long long ets = 0; std::string tok;
        if (f >> act >> pos_.entry >> pos_.sl >> pos_.rdist >> pos_.size_frac >> pa >> tr
              >> pos_.peak_close >> pos_.trail >> ets >> pos_.atr_e >> tok) {
            pos_.active = (act != 0); pos_.partial = (pa != 0); pos_.trail_on = (tr != 0);
            pos_.entry_ts = (int64_t)ets; pos_.token = (tok == "-") ? std::string() : tok;
            std::string vs; double vl;
            if (f >> vs >> vl) { pos_.sym = (vs == "-") ? std::string() : vs; pos_.lot = vl; }
        }
    }
};

} // namespace omega
