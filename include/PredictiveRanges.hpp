#pragma once
// ============================================================================
// PredictiveRanges.hpp -- LuxAlgo-style "Predictive Ranges" computed from the
// live OHLCBarEngine deques and snapshotted to logs/predictive_ranges.json
// for the desk GUI (:7779 /api/predictive_ranges).
//
// Faithful port of the published Pine v5 recurrence (stepped, NON-repainting --
// not an EMA/ATR envelope). Per bar:
//   tr      = max(high-low, |high-prevClose|, |low-prevClose|)
//   ATR     = Wilder RMA(tr, length)  (NaN warmup, SMA-seeded at length-1)
//   nATR    = (isnan(ATR) ? 0 : ATR) * factor          (step size AND threshold)
//   avgPrev = (no prior avg || prior == 0) ? src : avg[-1]
//   avg     = src-avgPrev >  nATR ? avgPrev + nATR
//           : avgPrev-src >  nATR ? avgPrev - nATR : avgPrev
//   hold    = (avg stepped this bar) ? nATR/2 : hold[-1]   (frozen between steps)
//   R2/R1/S1/S2 = avg +/- hold*rangeMult / hold
// Levels are flat between steps -> once printed they hold (non-repainting).
// Signals: outer-band breakout (step-direction-agreeing) + reversal re-entry,
// evaluated only when bands have width (post ATR warmup).
//
// PARENT-TU HEADER (row-12 pattern, like sizing.hpp/order_exec.hpp): zero local
// includes; relies on the including TU for OHLCBar/SymBarState (OHLCBarEngine.hpp),
// <fstream>/<sstream>/<cmath>/<deque>/<vector>/<string>, and on Windows
// MoveFileExA. Called from the on_tick.hpp [BAR-SAVE] 60s block -- the same
// thread that mutates the bar deques, so reads here are race-free by
// construction (same posture as save_indicators).
//
// Provenance: operator-supplied predictive_range_charts.cpp (2026-06-12),
// presets per asset from the same file. GUI-only surface -- no engine reads
// these values; zero trading-path impact.
// ============================================================================

namespace omega_pr {

struct PrPreset {
    const char* tag;        // dataset key in JSON ("XAUUSD")
    int         atr_length; // Wilder length
    double      factor;     // step size = factor * ATR
    double      range_mult; // outer band = hold * range_mult
    int         source;     // 0=close, 1=hlc3
};

struct PrRow {
    int64_t ts = 0;         // bar open, unix seconds
    double o = 0, h = 0, l = 0, c = 0;
    double avg = 0, r1 = 0, r2 = 0, s1 = 0, s2 = 0;
    int step_dir = 0;       // +1/-1 on a step bar, else 0
    int signal = 0;         // +1 long, -1 short, 0 none
};

// Compute the full recurrence over a bar deque; returns at most `keep` tail rows.
inline std::vector<PrRow> pr_compute(const std::deque<OHLCBar>& bars,
                                     const PrPreset& ps, std::size_t keep) {
    std::vector<PrRow> out;
    const std::size_t n = bars.size();
    if (n < 5) return out;

    // True range + Wilder ATR (NaN warmup, SMA seed at length-1).
    std::vector<double> tr(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0) { tr[i] = bars[i].high - bars[i].low; continue; }
        const double pc = bars[i - 1].close;
        tr[i] = std::max({bars[i].high - bars[i].low,
                          std::fabs(bars[i].high - pc),
                          std::fabs(bars[i].low  - pc)});
    }
    std::vector<double> atr(n, std::numeric_limits<double>::quiet_NaN());
    const int len = ps.atr_length;
    if (len >= 1 && n >= static_cast<std::size_t>(len)) {
        double seed = 0.0;
        for (int k = 0; k < len; ++k) seed += tr[static_cast<std::size_t>(k)];
        atr[static_cast<std::size_t>(len) - 1] = seed / len;
        for (std::size_t i = static_cast<std::size_t>(len); i < n; ++i)
            atr[i] = (atr[i - 1] * (len - 1.0) + tr[i]) / len;
    }

    out.reserve(n);
    bool have_avg = false;
    double avg = 0.0, hold = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const OHLCBar& b = bars[i];
        const double src = (ps.source == 1) ? (b.high + b.low + b.close) / 3.0 : b.close;
        const double natr = (std::isnan(atr[i]) ? 0.0 : atr[i]) * ps.factor;

        const double avg_prev = (!have_avg || avg == 0.0) ? src : avg;
        double new_avg = avg_prev;
        if      (src - avg_prev > natr) new_avg = avg_prev + natr;
        else if (avg_prev - src > natr) new_avg = avg_prev - natr;

        int step_dir = 0;
        bool stepped = false;
        if (!have_avg)            { stepped = true; }
        else if (new_avg > avg)   { stepped = true; step_dir = +1; }
        else if (new_avg < avg)   { stepped = true; step_dir = -1; }
        if (stepped) hold = natr / 2.0;
        avg = new_avg; have_avg = true;

        PrRow r;
        r.ts = b.ts_min * 60LL;
        r.o = b.open; r.h = b.high; r.l = b.low; r.c = b.close;
        r.avg = avg;
        r.r1 = avg + hold;             r.r2 = avg + hold * ps.range_mult;
        r.s1 = avg - hold;             r.s2 = avg - hold * ps.range_mult;
        r.step_dir = step_dir;

        // Signals only once bands have real width, vs the non-repainting outer levels.
        if (i > 0 && r.r2 > r.avg && !out.empty()) {
            const PrRow& p = out.back();
            const bool breakout_long  = (step_dir >= 0) && (p.c <= p.r2) && (r.c > r.r2);
            const bool breakout_short = (step_dir <= 0) && (p.c >= p.s2) && (r.c < r.s2);
            const bool reversal_long  = (b.low  <= r.s2) && (r.c > r.s2) && (r.c > p.c);
            const bool reversal_short = (b.high >= r.r2) && (r.c < r.r2) && (r.c < p.c);
            if      (breakout_long)  r.signal = +1;
            else if (breakout_short) r.signal = -1;
            else if (reversal_long)  r.signal = +1;
            else if (reversal_short) r.signal = -1;
        }
        out.push_back(r);
    }

    if (out.size() > keep) out.erase(out.begin(), out.end() - static_cast<long>(keep));
    return out;
}

inline void pr_append_dataset(std::ostringstream& js, const char* tf,
                              const std::deque<OHLCBar>& bars,
                              const PrPreset& ps, bool& first_tf) {
    const std::vector<PrRow> rows = pr_compute(bars, ps, 160);
    if (rows.empty()) return;
    if (!first_tf) js << ",";
    first_tf = false;
    js << "\"" << tf << "\":{\"len\":" << ps.atr_length
       << ",\"factor\":" << ps.factor << ",\"rmult\":" << ps.range_mult
       << ",\"source\":\"" << (ps.source == 1 ? "hlc3" : "close") << "\",\"bars\":[";
    char buf[256];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const PrRow& r = rows[i];
        std::snprintf(buf, sizeof(buf),
            "%s[%lld,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d]",
            i ? "," : "", static_cast<long long>(r.ts),
            r.o, r.h, r.l, r.c, r.avg, r.r1, r.r2, r.s1, r.s2, r.step_dir, r.signal);
        js << buf;
    }
    js << "]}";
}

// pr_aggregate -- roll a finer bar deque up into `mins`-minute buckets and return
// a LOCAL deque. GUI fallback ONLY: never mutates shared state, never seen by any
// engine. Used to synthesise an m5 series from the always-fresh m1 deque when the
// real m5 deque is still warming up (the first minutes after a market open or a
// restart), so the desk m5 chart is never blank while m1 already has data. If the
// finer deque is thin (e.g. indices that don't feed m1), this returns few/zero
// rows and the caller keeps whatever real m5 bars exist -- no worse than before.
inline std::deque<OHLCBar> pr_aggregate(const std::deque<OHLCBar>& src, int64_t mins) {
    std::deque<OHLCBar> out;
    if (src.empty() || mins <= 0) return out;
    int64_t cur_bucket = -1;
    for (const OHLCBar& b : src) {
        const int64_t bucket = (b.ts_min / mins) * mins;   // bucket open minute
        if (bucket != cur_bucket) {                         // first bar of bucket
            OHLCBar nb = b;                                 // open = this bar's open
            nb.ts_min = bucket;
            out.push_back(nb);
            cur_bucket = bucket;
        } else {                                            // fold into open bucket
            OHLCBar& a = out.back();
            if (b.high > a.high) a.high = b.high;
            if (b.low  < a.low ) a.low  = b.low;
            a.close   = b.close;                            // close = last bar's close
            a.volume += b.volume;
        }
    }
    return out;
}

// Build the full snapshot JSON and atomically swap it into place.
// Datasets: XAUUSD / US500 / USTEC x { m5, m15, h1 }. Presets per the
// operator-supplied predictive_range_charts.cpp (gold ATR20 f2.5 hlc3,
// SP ATR14 f2.0 close, NQ ATR14 f3.0 close).
inline void pr_write_snapshot(const std::string& path) {
    struct SymSet { const char* key; SymBarState* st; PrPreset ps; };
    static SymSet sets[] = {
        {"XAUUSD", &g_bars_gold, {"XAUUSD", 20, 2.5, 2.0, 1}},
        {"US500",  &g_bars_sp,   {"US500",  14, 2.0, 2.0, 0}},
        {"USTEC",  &g_bars_nq,   {"USTEC",  14, 3.0, 2.0, 0}},
    };

    std::ostringstream js;
    js.setf(std::ios::fixed); js.precision(4);
    js << "{\"updated\":" << nowSec() << ",\"datasets\":{";
    bool first_sym = true;
    for (auto& s : sets) {
        std::ostringstream sym_js;
        bool first_tf = true;
        // m5: prefer the real m5 deque, but if it's still warming up (fewer than
        // atr_length+5 bars -> pr_compute can't seed ATR -> blank chart), fall
        // back to an m5 series synthesised from the deeper m1 deque so the desk
        // chart is never empty at/after a market open. GUI-only; shared m5 (read
        // by GoldScalpPyramidEngine) is never touched. Pick whichever has more bars.
        const std::deque<OHLCBar>& real_m5 = s.st->m5.get_bars();
        const std::size_t m5_warm = static_cast<std::size_t>(s.ps.atr_length) + 5;
        if (real_m5.size() >= m5_warm) {
            pr_append_dataset(sym_js, "m5", real_m5, s.ps, first_tf);
        } else {
            const std::deque<OHLCBar> m5_from_m1 = pr_aggregate(s.st->m1.get_bars(), 5);
            pr_append_dataset(sym_js, "m5",
                              m5_from_m1.size() > real_m5.size() ? m5_from_m1 : real_m5,
                              s.ps, first_tf);
        }
        pr_append_dataset(sym_js, "m15", s.st->m15.get_bars(), s.ps, first_tf);
        pr_append_dataset(sym_js, "h1",  s.st->h1.get_bars(),  s.ps, first_tf);
        if (first_tf) continue;  // no usable bars for this symbol yet
        if (!first_sym) js << ",";
        first_sym = false;
        js << "\"" << s.key << "\":{" << sym_js.str() << "}";
    }
    js << "}}";

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << js.str();
    }
#ifdef _WIN32
    MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
    std::rename(tmp.c_str(), path.c_str());
#endif
}

} // namespace omega_pr
