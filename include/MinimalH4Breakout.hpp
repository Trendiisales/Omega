// =============================================================================
//  MinimalH4Breakout.hpp  --  Pure H4 Donchian breakout engine for XAUUSD
//
//  Created 2026-04-24. Derived from backtest/htf_bt_minimal.cpp.
//
//  DO NOT INTERNALLY FILTER. This engine was deliberately designed stripped-down
//  because the existing H4RegimeEngine (ADX + EMA-sep + RSI + M15 ATR expansion
//  + RSI extreme + spread + weekend + struct-SL) produced 0 trades over 2yrs in
//  the deep quant sweep. The 27-config walk-forward sweep on pure Donchian
//  breakout produced 27/27 profitable configs on 2yr XAUUSD tick data, with
//  best config (D=10 SL=1.5x TP=4.0x) holding PF 1.35 out-of-sample and PF 1.31
//  under pessimistic cost stress. See backtest/FINDING_E_CULL.md and
//  backtest/htf_bt_minimal.cpp for full validation record.
//
//  Strategy:
//    1. H4 bar close above Donchian-N channel high  -> LONG
//    2. H4 bar close below Donchian-N channel low   -> SHORT
//    3. SL = sl_mult * H4_ATR behind entry  (fixed-distance, NOT structural)
//    4. TP = tp_mult * H4_ATR ahead of entry
//    5. No ADX gate, no EMA-sep, no RSI, no M15 expansion, no struct-SL
//    6. Weekend entry gate only (no new entries Fri 20:00 UTC through Sun 22:00 UTC)
//    7. Fixed $10 risk per trade, 0.01 lot cap
//
//  Exit:
//    - TP hit  (ask <= TP short, bid >= TP long)
//    - SL hit  (ask >= SL short, bid <= SL long)
//    - Timeout after timeout_h4_bars H4 bars (safety valve, should rarely fire)
//    - Weekend force-close on Friday 20:00 UTC+ if profitable
//
//  Notes:
//    - Shadow_mode=true default. NEVER set to false without live validation
//      of at least 5-10 shadow trades in production conditions matching
//      backtest expectation (0.23 trades/day = ~5-7 trades/month).
//    - Does NOT replace H4RegimeEngine -- both run in parallel for now.
//      MinimalH4Breakout gets priority via gold_any_open gate if already open.
//
//  S47 T4b 2026-04-27: long_only flag
//    New parameter `long_only` (default false). When true, bear-break entries
//    are silently skipped (return without firing). Bull-break entries
//    unaffected. Gate is configurable via [minimal_h4] long_only=true in
//    omega_config.ini. Default-false preserves backtest behaviour byte-exact.
// =============================================================================

#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <algorithm>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "OmegaTradeLedger.hpp"
#include "OHLCBarEngine.hpp"
#include "OmegaCostGuard.hpp"  // see on_h4_bar() entry guard

namespace omega {

// =============================================================================
//  Parameter struct -- per-instrument; gold defaults derived from walk-forward
//  best OOS config (Year 1 train best, Year 2 test PF 1.35).
// =============================================================================
struct MinimalH4Params {
    double risk_dollars        = 10.0;  // fixed $ risk per trade
    double max_lot             = 0.01;  // shadow-mode cap; matches rest of HTF stack
    int    donchian_bars       = 10;    // N for channel lookback
    double sl_mult             = 1.5;   // SL = sl_mult * H4 ATR behind entry
    double tp_mult             = 4.0;   // TP = tp_mult * H4 ATR ahead of entry
    double max_spread          = 2.0;   // reject entry if spread > this
    int    timeout_h4_bars     = 24;    // safety cap: 24 H4 bars = 4 days
    int    cooldown_h4_bars    = 2;     // bars between trades
    bool   weekend_close_gate  = true;  // close profitable positions Fri 20:00+ UTC
    // S47 T4b 2026-04-27: when true, only bull-break entries are taken; bear
    // breaks are silently skipped. Default false preserves prior behaviour.
    bool   long_only           = false;
};

inline MinimalH4Params make_minimal_h4_gold_params() {
    MinimalH4Params p;
    // Gold defaults above are OOS-validated. Keep identical unless ini overrides.
    return p;
}

// =============================================================================
//  Signal struct (matches omega::HTFSignal shape but kept local for isolation)
// =============================================================================
struct MinimalH4Signal {
    bool        valid   = false;
    bool        is_long = false;
    double      entry   = 0.0;
    double      sl      = 0.0;
    double      tp      = 0.0;
    double      size    = 0.0;
    const char* reason  = "";
};

// =============================================================================
//  MinimalH4Breakout engine
// =============================================================================
struct MinimalH4Breakout {

    bool            shadow_mode = true;
    bool            enabled     = true;
    MinimalH4Params p;
    std::string     symbol      = "XAUUSD";

    using CloseCallback = std::function<void(const omega::TradeRecord&)>;

    struct OpenPos {
        bool    active       = false;
        bool    is_long      = false;
        double  entry        = 0.0;
        double  sl           = 0.0;
        double  tp           = 0.0;
        double  size         = 0.0;
        double  h4_atr       = 0.0;
        int64_t entry_ts_ms  = 0;
        int     h4_bars_held = 0;
    } pos_;

    // Session/state
    double  daily_pnl_          = 0.0;
    int64_t daily_reset_day_    = 0;
    int     h4_bar_count_       = 0;
    int     cooldown_until_bar_ = 0;
    std::deque<double> h4_highs_;
    std::deque<double> h4_lows_;
    double  channel_high_       = 0.0;
    double  channel_low_        = 1e9;
    int     m_trade_id_         = 0;

    bool has_open_position() const noexcept { return pos_.active; }

    // Seed Donchian channel from saved H4 bars on startup. Without this the engine
    // needs donchian_bars fresh H4 bars (e.g. 10 * 4hr = 40hrs) before it can
    // produce any signal. With seeding: ready from first live H4 close if saved
    // H4 bar history is present.
    void seed_channel_from_bars(const std::deque<OHLCBar>& bars) noexcept {
        h4_highs_.clear();
        h4_lows_ .clear();
        const int n = static_cast<int>(bars.size());
        const int start = std::max(0, n - p.donchian_bars);
        for (int i = start; i < n; ++i) {
            h4_highs_.push_back(bars[i].high);
            h4_lows_ .push_back(bars[i].low);
        }
        if ((int)h4_highs_.size() >= p.donchian_bars) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
            printf("[MINIMAL_H4-%s] Channel seeded from %d H4 bars: high=%.2f low=%.2f\n",
                   symbol.c_str(), (int)h4_highs_.size(), channel_high_, channel_low_);
        } else {
            printf("[MINIMAL_H4-%s] Partial channel: %d/%d bars (needs %d more H4 bars)\n",
                   symbol.c_str(), (int)h4_highs_.size(), p.donchian_bars,
                   p.donchian_bars - (int)h4_highs_.size());
        }
        fflush(stdout);
    }

    // -------------------------------------------------------------------------
    // P1-11 (S18) -- Cold-start CSV warm-load fallback for the Donchian channel.
    //
    // PURPOSE
    //   When `g_bars_<sym>.h4` has no persisted binary state (.dat file missing,
    //   first deploy on a fresh VPS, etc.) the engine cannot use
    //   seed_channel_from_bars(g_bars_<sym>.h4.get_bars()) because the deque is
    //   empty -- and the engine then needs `donchian_bars * 4hrs` (default
    //   40 hours) of fresh live H4 closes before it can fire its first signal.
    //   This CSV loader provides a one-shot bootstrap: read H4 OHLC bars from
    //   a CSV file dropped in a known location, seed the channel from them,
    //   and the engine is ready on the first live H4 close.
    //
    // CSV SCHEMA (matches backtest/seed_us30_h4.cpp for cross-engine consistency)
    //   One bar per line, comma/tab/semicolon separated:
    //       timestamp, open, high, low, close [, volume]
    //   Header row(s) and lines beginning with '#' are skipped.
    //   Timestamp accepts epoch seconds (10 digits), epoch ms (>=13 digits),
    //   or ISO8601-like strings ("2026-04-18T08:00:00", "2026-04-18 08:00:00",
    //   "2026/04/18 08:00:00"). Volume is ignored.
    //   Zero-range bars (high==low==open==close) are silently dropped --
    //   Dukascopy emits these for closed-market hours and they would skew
    //   the channel if included.
    //
    // SOURCING THE CSV
    //   For XAUUSD: download H4 from Dukascopy (XAUUSD), or any other vendor
    //   with H4 OHLC data. The bar prices need not match the broker feed
    //   exactly -- the channel is rebuilt from live broker bars over the next
    //   `donchian_bars * 4hrs` of operation, so any vendor drift is
    //   self-correcting. Need at least `donchian_bars` bars; more is fine
    //   (only the last `donchian_bars` are used).
    //
    // RETURN
    //   true  -> CSV was opened and at least `donchian_bars` valid bars were
    //            parsed; channel is now seeded and ready.
    //   false -> file missing OR insufficient valid bars; channel state
    //            unchanged. Caller should fall back to cold start.
    //
    // FAILURE MODES (logged to stdout, no exceptions thrown)
    //   - File missing or unreadable -> warn, return false
    //   - All lines unparseable      -> warn, return false
    //   - Fewer than donchian_bars valid bars after parse -> warn, return false
    // -------------------------------------------------------------------------
    bool seed_channel_from_csv(const std::string& path) noexcept {
        std::ifstream fin(path);
        if (!fin.is_open()) {
            printf("[MINIMAL_H4-%s] CSV warm-load skipped: cannot open %s\n",
                   symbol.c_str(), path.c_str());
            fflush(stdout);
            return false;
        }

        std::vector<OHLCBar> parsed;
        parsed.reserve(2048);
        std::string line;
        int parsed_count = 0;
        int skipped      = 0;
        while (std::getline(fin, line)) {
            OHLCBar b{};
            if (_parse_csv_h4_line(line, b)) {
                parsed.push_back(b);
                ++parsed_count;
            } else {
                ++skipped;
            }
        }

        if (parsed_count < p.donchian_bars) {
            printf("[MINIMAL_H4-%s] CSV warm-load failed: parsed %d valid bars,"
                   " skipped %d lines, need at least %d for Donchian.\n",
                   symbol.c_str(), parsed_count, skipped, p.donchian_bars);
            fflush(stdout);
            return false;
        }

        // Sort by timestamp ascending (Dukascopy CSVs are usually already
        // sorted, but be defensive against vendor variation).
        std::sort(parsed.begin(), parsed.end(),
                  [](const OHLCBar& a, const OHLCBar& b){ return a.ts_min < b.ts_min; });

        // seed_channel_from_bars takes a deque -- copy from the vector.
        std::deque<OHLCBar> dq(parsed.begin(), parsed.end());
        printf("[MINIMAL_H4-%s] CSV warm-load: parsed %d bars from %s, skipped"
               " %d lines, last_ts_min=%lld\n",
               symbol.c_str(), parsed_count, path.c_str(), skipped,
               (long long)dq.back().ts_min);
        fflush(stdout);
        seed_channel_from_bars(dq);
        return true;
    }

private:
    // Parse a single CSV line into an OHLCBar. Returns true if a valid bar was
    // extracted. Mirrors backtest/seed_us30_h4.cpp::parse_csv_line behaviour
    // so the same source CSVs work for both engines. ts_min is filled in
    // minutes-since-epoch (matching OHLCBar's storage convention).
    static bool _parse_csv_h4_line(const std::string& raw, OHLCBar& out) noexcept {
        // Skip blank, header, comment lines
        size_t pos = 0;
        while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\t')) ++pos;
        if (pos >= raw.size()) return false;
        const char first = raw[pos];
        if (first == '#' || first == '\r' || first == '\n') return false;

        // Accept lines whose first non-whitespace char is a digit, '-', or quote.
        // Other leading chars are header/text rows -- skip silently.
        if (!((first >= '0' && first <= '9') || first == '-' || first == '"' ||
              first == '\''))
            return false;

        // Make a mutable copy and strip trailing CR/LF/whitespace.
        std::string buf(raw, pos);
        while (!buf.empty() && (buf.back() == '\r' || buf.back() == '\n' ||
                                buf.back() == ' '  || buf.back() == '\t')) {
            buf.pop_back();
        }
        if (buf.empty()) return false;

        // Strip surrounding quotes from the entire line if present.
        if (buf.front() == '"' || buf.front() == '\'') buf.erase(0, 1);
        if (!buf.empty() && (buf.back() == '"' || buf.back() == '\'')) buf.pop_back();

        // Tokenise on comma / tab / semicolon (defensive against vendor variation).
        std::vector<std::string> fields;
        fields.reserve(8);
        std::string cur;
        for (char c : buf) {
            if (c == ',' || c == '\t' || c == ';') {
                fields.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        fields.push_back(cur);
        if (fields.size() < 5) return false;

        // Trim each field and strip surrounding quotes.
        for (std::string& f : fields) {
            while (!f.empty() && (f.front() == ' ' || f.front() == '\t')) f.erase(0, 1);
            while (!f.empty() && (f.back()  == ' ' || f.back()  == '\t')) f.pop_back();
            if (!f.empty() && (f.front() == '"' || f.front() == '\'')) f.erase(0, 1);
            if (!f.empty() && (f.back()  == '"' || f.back()  == '\'')) f.pop_back();
        }

        // ── Timestamp parse ───────────────────────────────────────────────────
        const std::string& ts_str = fields[0];
        if (ts_str.empty()) return false;
        int64_t ts_sec = 0;
        bool ts_ok = false;

        // Try epoch numeric first (allow optional leading minus, but H4 epochs
        // are positive in practice).
        bool all_digits = true;
        for (char c : ts_str) {
            if (c < '0' || c > '9') { all_digits = false; break; }
        }
        if (all_digits) {
            const long long v = std::atoll(ts_str.c_str());
            if (ts_str.length() >= 13)      { ts_sec = static_cast<int64_t>(v / 1000LL); ts_ok = true; }
            else if (ts_str.length() >= 9)  { ts_sec = static_cast<int64_t>(v);          ts_ok = true; }
        }

        if (!ts_ok) {
            // Try ISO8601: YYYY-MM-DD[T| ]HH:MM:SS[.fff][Z]
            std::tm tm{};
            int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
            if (std::sscanf(ts_str.c_str(), "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6 ||
                std::sscanf(ts_str.c_str(), "%d-%d-%d %d:%d:%d",   &Y, &M, &D, &h, &m, &s) == 6 ||
                std::sscanf(ts_str.c_str(), "%d/%d/%d %d:%d:%d",   &Y, &M, &D, &h, &m, &s) == 6)
            {
                tm.tm_year = Y - 1900;
                tm.tm_mon  = M - 1;
                tm.tm_mday = D;
                tm.tm_hour = h;
                tm.tm_min  = m;
                tm.tm_sec  = s;
#ifdef _WIN32
                ts_sec = static_cast<int64_t>(_mkgmtime(&tm));
#else
                ts_sec = static_cast<int64_t>(timegm(&tm));
#endif
                if (ts_sec > 0) ts_ok = true;
            }
        }
        if (!ts_ok) return false;

        // ── OHLC parse and validation ─────────────────────────────────────────
        const double op = std::atof(fields[1].c_str());
        const double hi = std::atof(fields[2].c_str());
        const double lo = std::atof(fields[3].c_str());
        const double cl = std::atof(fields[4].c_str());
        if (op <= 0.0 || hi <= 0.0 || lo <= 0.0 || cl <= 0.0) return false;
        if (hi < lo) return false;
        // Reject zero-range bars (closed-market hours from Dukascopy).
        if (hi == lo && hi == op && hi == cl) return false;

        out.ts_min = ts_sec / 60LL;  // OHLCBar stores minutes-since-epoch
        out.open   = op;
        out.high   = hi;
        out.low    = lo;
        out.close  = cl;
        out.volume = 0;
        return true;
    }

public:

    // ── Called on every H4 bar close ─────────────────────────────────────────
    // Uses PRIOR window (not including this bar) for Donchian comparison,
    // which matches the backtest implementation exactly.
    MinimalH4Signal on_h4_bar(
        double  h4_bar_high, double h4_bar_low, double h4_bar_close,
        double  bid, double ask,
        double  h4_atr,
        int64_t now_ms,
        CloseCallback on_close) noexcept
    {
        ++h4_bar_count_;
        _daily_reset(now_ms);

        // PRIOR-window Donchian: capture channel state BEFORE updating with this bar
        const bool channel_ready = ((int)h4_highs_.size() >= p.donchian_bars);
        const double prior_high = channel_high_;
        const double prior_low  = channel_low_;

        // Now update channel with this closing bar for next call
        h4_highs_.push_back(h4_bar_high);
        h4_lows_ .push_back(h4_bar_low);
        if ((int)h4_highs_.size() > p.donchian_bars) {
            h4_highs_.pop_front();
            h4_lows_ .pop_front();
        }
        if ((int)h4_highs_.size() >= p.donchian_bars) {
            channel_high_ = *std::max_element(h4_highs_.begin(), h4_highs_.end());
            channel_low_  = *std::min_element(h4_lows_ .begin(), h4_lows_ .end());
        }

        // Manage open position first (timeout)
        if (pos_.active) {
            pos_.h4_bars_held++;
            _manage(bid, ask, now_ms, on_close);
            return MinimalH4Signal{};
        }

        MinimalH4Signal sig{};
        if (!enabled) return sig;
        if (h4_bar_count_ < cooldown_until_bar_) return sig;
        if (!channel_ready) return sig;
        if (prior_high <= 0.0 || prior_low >= 1e8) return sig;
        if (h4_atr <= 0.0) return sig;

        // Weekend entry gate -- block Fri 20:00 UTC through Sun 22:00 UTC
        if (_is_weekend_gated(now_ms)) {
            static int64_t s_wk = 0;
            if (now_ms - s_wk > 3600000LL) {
                s_wk = now_ms;
                printf("[MINIMAL_H4-%s] Weekend gate: no new entries\n", symbol.c_str());
                fflush(stdout);
            }
            return sig;
        }

        // Spread gate
        if ((ask - bid) > p.max_spread) return sig;

        // Donchian break on THIS bar's close vs PRIOR channel
        const bool bull_break = (h4_bar_close > prior_high);
        const bool bear_break = (h4_bar_close < prior_low);
        if (!bull_break && !bear_break) return sig;

        const bool intend_long = bull_break;

        // S47 T4b 2026-04-27: long_only gate. When p.long_only is true, drop
        //   bear-break entries silently. Bull entries are unaffected.
        if (p.long_only && !intend_long) {
            static int64_t s_lo = 0;
            if (now_ms - s_lo > 3600000LL) {
                s_lo = now_ms;
                printf("[MINIMAL_H4-%s] long_only=true: dropping SHORT bear-break entry\n",
                       symbol.c_str());
                fflush(stdout);
            }
            return sig;
        }

        // Entry at bar-close mid ± half spread (use bid/ask to approximate)
        const double entry_px = intend_long ? ask : bid;
        const double sl_pts   = h4_atr * p.sl_mult;
        const double tp_pts   = h4_atr * p.tp_mult;
        const double sl_px    = intend_long ? (entry_px - sl_pts) : (entry_px + sl_pts);
        const double tp_px    = intend_long ? (entry_px + tp_pts) : (entry_px - tp_pts);

        // Size: $risk / (sl_pts * $100/pt)  -> clamp to [0.01, max_lot]
        double size = p.risk_dollars / (sl_pts * 100.0);
        size = std::floor(size / 0.001) * 0.001;
        size = std::max(0.01, std::min(p.max_lot, size));

        // === Cost gate (unified per-symbol layer; cost_ratio_min=1.5 matches
        // on_tick.hpp:1065 cost_ok lambda). On block: return empty signal;
        // engine re-evaluates next H4 bar close. ===
        if (!ExecutionCostGuard::is_viable(symbol.c_str(), (ask - bid),
                                           tp_pts, size, 1.5)) {
            return sig;  // sig is still empty (valid=false)
        }

        pos_.active        = true;
        pos_.is_long       = intend_long;
        pos_.entry         = entry_px;
        pos_.sl            = sl_px;
        pos_.tp            = tp_px;
        pos_.size          = size;
        pos_.h4_atr        = h4_atr;
        pos_.entry_ts_ms   = now_ms;
        pos_.h4_bars_held  = 0;
        ++m_trade_id_;

        const double ch_level = intend_long ? prior_high : prior_low;
        printf("[MINIMAL_H4-%s] ENTRY %s @ %.2f sl=%.2f tp=%.2f size=%.3f"
               " h4_atr=%.2f channel_%s=%.2f close=%.2f%s\n",
               symbol.c_str(), intend_long ? "LONG" : "SHORT",
               entry_px, sl_px, tp_px, size, h4_atr,
               intend_long ? "high" : "low", ch_level, h4_bar_close,
               shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        sig.valid   = true;
        sig.is_long = intend_long;
        sig.entry   = entry_px;
        sig.sl      = sl_px;
        sig.tp      = tp_px;
        sig.size    = size;
        sig.reason  = "MINIMAL_H4_DONCHIAN_BREAK";
        return sig;
    }

    void patch_size(double lot) noexcept { if (pos_.active) pos_.size = lot; }
    void cancel()                noexcept { pos_ = OpenPos{}; }

    // Tick-level management -- TP/SL check only, no trailing (matches backtest)
    void on_tick(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active) return;
        if (pos_.is_long) {
            if (bid <= pos_.sl)      _close(bid, "SL_HIT", now_ms, on_close);
            else if (bid >= pos_.tp) _close(bid, "TP_HIT", now_ms, on_close);
        } else {
            if (ask >= pos_.sl)      _close(ask, "SL_HIT", now_ms, on_close);
            else if (ask <= pos_.tp) _close(ask, "TP_HIT", now_ms, on_close);
        }
    }

    // Weekend close: close profitable positions Friday 20:00+ UTC
    void check_weekend_close(double bid, double ask,
                              int64_t now_ms, CloseCallback on_close) noexcept {
        if (!pos_.active || !p.weekend_close_gate) return;
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        const bool is_friday   = (utc_dow == 4);
        if (!is_friday || utc_hour < 20) return;
        const double mid  = (bid + ask) * 0.5;
        const double move = pos_.is_long ? (mid - pos_.entry) : (pos_.entry - mid);
        if (move > 0.0) {
            static int64_t s_wk_close = 0;
            if (now_ms - s_wk_close > 3600000LL) {
                s_wk_close = now_ms;
                printf("[MINIMAL_H4-%s] Weekend close: profitable position closed Friday 20:00+\n",
                       symbol.c_str());
                fflush(stdout);
                _close(pos_.is_long ? bid : ask, "WEEKEND_CLOSE", now_ms, on_close);
            }
        }
    }

    void force_close(double bid, double ask, int64_t now_ms, CloseCallback cb) noexcept {
        if (!pos_.active) return;
        _close(pos_.is_long ? bid : ask, "FORCE_CLOSE", now_ms, cb);
    }

private:
    // Weekend gate: Fri 20:00 UTC+ through Sun 22:00 UTC blocked
    // Mapping: Mon=0 Tue=1 Wed=2 Thu=3 Fri=4 Sat=5 Sun=6.
    // Epoch day 0 = Thu 1970-01-01, so dow = (day + 3) % 7.
    static bool _is_weekend_gated(int64_t now_ms) noexcept {
        const int64_t utc_sec  = now_ms / 1000LL;
        const int     utc_dow  = static_cast<int>((utc_sec / 86400LL + 3) % 7);
        const int     utc_hour = static_cast<int>((utc_sec % 86400LL) / 3600LL);
        if (utc_dow == 4 && utc_hour >= 20) return true;  // Fri 20:00+
        if (utc_dow == 5) return true;                     // Sat all day
        if (utc_dow == 6 && utc_hour < 22)  return true;   // Sun before 22:00
        return false;
    }

    void _daily_reset(int64_t now_ms) noexcept {
        const int64_t day = (now_ms / 1000LL) / 86400LL;
        if (day != daily_reset_day_) { daily_pnl_ = 0.0; daily_reset_day_ = day; }
    }

    void _manage(double bid, double ask, int64_t now_ms, CloseCallback on_close) noexcept {
        if (pos_.h4_bars_held >= p.timeout_h4_bars) {
            // 2026-05-13 (part L): VWR-pattern winner exemption. Only cut on
            // H4-bar timeout when current mid is at/below entry. Winners ride
            // to TP / SL / trail.
            const double mid = (bid + ask) * 0.5;
            const double cur_move = pos_.is_long
                ? (mid - pos_.entry)
                : (pos_.entry - mid);
            if (cur_move <= 0.0) {
                printf("[MINIMAL_H4-%s] TIMEOUT %d H4 bars\n",
                       symbol.c_str(), pos_.h4_bars_held);
                fflush(stdout);
                _close(pos_.is_long ? bid : ask, "TIMEOUT", now_ms, on_close);
                return;
            }
        }
    }

    void _close(double exit_px, const char* reason,
                int64_t now_ms, CloseCallback on_close) noexcept {
        const double pnl_pts = (pos_.is_long
            ? (exit_px - pos_.entry)
            : (pos_.entry - exit_px)) * pos_.size;
        daily_pnl_ += pnl_pts * 100.0;

        printf("[MINIMAL_H4-%s] EXIT %s @ %.2f %s pnl=$%.2f bars=%d%s\n",
               symbol.c_str(), pos_.is_long ? "LONG" : "SHORT",
               exit_px, reason, pnl_pts * 100.0,
               pos_.h4_bars_held, shadow_mode ? " [SHADOW]" : "");
        fflush(stdout);

        if (on_close) {
            omega::TradeRecord tr;
            tr.id         = m_trade_id_;
            tr.symbol     = symbol.c_str();
            tr.side       = pos_.is_long ? "LONG" : "SHORT";
            tr.engine     = "MinimalH4Breakout";
            tr.entryPrice = pos_.entry;
            tr.exitPrice  = exit_px;
            tr.sl         = pos_.sl;
            tr.size       = pos_.size;
            tr.pnl        = pnl_pts;
            tr.mfe        = 0.0;
            tr.mae        = 0.0;
            tr.entryTs    = pos_.entry_ts_ms / 1000;
            tr.exitTs     = now_ms / 1000;
            tr.exitReason = reason;
            tr.regime     = "MINIMAL_H4";
            tr.l2_live    = false;
            tr.shadow     = shadow_mode;
            on_close(tr);
        }
        cooldown_until_bar_ = h4_bar_count_ + p.cooldown_h4_bars;
        pos_ = OpenPos{};
    }
};

} // namespace omega
