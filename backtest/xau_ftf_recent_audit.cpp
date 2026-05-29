// =============================================================================
// xau_ftf_recent_audit.cpp -- $ value translation + recent-period FTF audit.
//
// Two purposes:
//   1. Translate the 2yr Sharpe +4.69 verdict into concrete $ values at
//      several position sizes (0.01, 0.10, 1.0, 10.0 lots).
//   2. Slice FTF trades by entry date and report stats for the last
//      30 / 90 / 180 calendar days, to confirm edge holds in the current
//      $4700-era regime (not just the 2024 portion of the corpus).
//
// Build: cmake --build build --target xau_ftf_recent_audit
// Run:   ./build/xau_ftf_recent_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <ctime>

#include "XauForecastToFillD1Engine.hpp"

struct H4Bar { int64_t ts_sec; double o, h, l, c; };

static std::vector<H4Bar> load_h4(const std::string& path) {
    std::vector<H4Bar> bars;
    std::ifstream f(path);
    if (!f) return bars;
    std::string line; std::getline(f, line);
    while (std::getline(f, line)) {
        H4Bar b; const char* pp = line.c_str(); char* e;
        b.ts_sec = std::strtoll(pp, &e, 10); if (*e == ',') pp = e + 1;
        b.o = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.h = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.l = std::strtod(pp, &e); if (*e == ',') pp = e + 1;
        b.c = std::strtod(pp, &e);
        if (b.ts_sec > 10'000'000'000LL) b.ts_sec /= 1000;
        bars.push_back(b);
    }
    return bars;
}

struct Trade { int64_t entry_ts, exit_ts; double entry, exit, pnl; const char* exit_reason; };

struct Stats {
    int n = 0, wins = 0;
    double sum = 0, sumsq = 0, worst = 0, peak = 0, running = 0, mdd = 0;
    void add(double pnl) {
        ++n;
        sum += pnl; sumsq += pnl * pnl;
        if (pnl > 0) ++wins;
        if (pnl < worst) worst = pnl;
        running += pnl;
        if (running > peak) peak = running;
        if (peak - running > mdd) mdd = peak - running;
    }
    double mean() const { return n ? sum / n : 0.0; }
    double stddev() const {
        if (n < 2) return 0.0;
        const double m = mean();
        const double v = (sumsq - n * m * m) / (n - 1);
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    double sharpe_ann(double tr_per_yr) const {
        const double s = stddev();
        if (s < 1e-9 || n < 2) return 0.0;
        return (mean() / s) * std::sqrt(tr_per_yr);
    }
    double wr() const { return n ? 100.0 * wins / n : 0.0; }
};

static std::string fmt_date(int64_t ts_sec) {
    const time_t t = (time_t)ts_sec;
    std::tm tm{}; gmtime_r(&t, &tm);
    char buf[16]; std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

static Stats slice_stats(const std::vector<Trade>& trades, int64_t since_ts) {
    Stats s;
    for (const auto& t : trades) if (t.entry_ts >= since_ts) s.add(t.pnl);
    return s;
}

int main(int argc, char** argv) {
    const std::string path = (argc >= 2) ? argv[1]
        : std::string("/Users/jo/Tick/2yr_XAUUSD_tick_fresh.h4.csv");
    const std::string rpt_path = (argc >= 3) ? argv[2]
        : std::string("/tmp/xau_ftf_recent.txt");

    auto bars = load_h4(path);
    std::fprintf(stderr, "[FTFR] loaded %zu H4 bars\n", bars.size());
    if (bars.size() < 100) return 1;

    omega::XauForecastToFillD1Engine eng;
    eng.shadow_mode = true; eng.enabled = true; eng.symbol = "XAUUSD";
    eng.p = omega::make_xau_forecast_to_fill_d1_params();

    std::vector<Trade> trades;
    auto cb = [&](const omega::TradeRecord& tr) {
        Trade t;
        t.entry_ts    = tr.entryTs;
        t.exit_ts     = tr.exitTs;
        t.entry       = tr.entryPrice;
        t.exit        = tr.exitPrice;
        t.pnl         = tr.pnl;
        t.exit_reason = tr.exitReason.c_str();
        trades.push_back(t);
    };

    const double half = 0.15;
    for (size_t i = 0; i < bars.size(); ++i) {
        const auto& b = bars[i];
        const int64_t ts_ms = b.ts_sec * 1000LL;
        eng.on_h4_bar(b.h, b.l, b.c, b.c - half, b.c + half, ts_ms, cb);
        if (i + 1 < bars.size()) {
            const auto& nb = bars[i + 1];
            const int64_t nts = nb.ts_sec * 1000LL;
            eng.on_tick(nb.l - half, nb.l + half, nts, cb);
            eng.on_tick(nb.h - half, nb.h + half, nts, cb);
        }
    }

    const int64_t corpus_start = bars.front().ts_sec;
    const int64_t corpus_end   = bars.back().ts_sec;
    const double corpus_yrs    = (corpus_end - corpus_start) / (365.25 * 86400.0);

    Stats full;
    for (auto& t : trades) full.add(t.pnl);
    const double tr_per_yr_full = full.n / corpus_yrs;
    const double sharpe_full = full.sharpe_ann(tr_per_yr_full);

    FILE* RPT = std::fopen(rpt_path.c_str(), "w");
    if (!RPT) RPT = stderr;
    std::fprintf(RPT, "==========================================================================\n");
    std::fprintf(RPT, "  xau_ftf_recent_audit -- FTF $-value translation + recent-period slices\n");
    std::fprintf(RPT, "  corpus: %s -> %s  (%.2f years, %zu H4 bars)\n",
                 fmt_date(corpus_start).c_str(), fmt_date(corpus_end).c_str(),
                 corpus_yrs, bars.size());
    std::fprintf(RPT, "==========================================================================\n\n");

    // ── Full-corpus + $ translation ────────────────────────────────────────
    std::fprintf(RPT, "FULL CORPUS (entire 2yr backtest)\n");
    std::fprintf(RPT, "  n=%-4d  WR=%5.2f%%  Sharpe_ann=%+.3f  trades/yr=%.1f\n",
                 full.n, full.wr(), sharpe_full, tr_per_yr_full);
    std::fprintf(RPT, "  gross=+$%.2f  mean=+$%.3f/trade  worst=$%.2f  MaxDD=$%.2f\n\n",
                 full.sum, full.mean(), full.worst, full.mdd);

    std::fprintf(RPT, "  $-VALUE BY LOT SIZE (raw pnl scales linearly; Sharpe invariant)\n");
    std::fprintf(RPT, "  ----------------------------------------------------------\n");
    std::fprintf(RPT, "  %-8s  %12s  %12s  %12s  %12s\n",
                 "Lot", "Gross 2yr", "Per Year", "Worst Trade", "MaxDD");
    const double lots[5] = {0.01, 0.10, 1.0, 5.0, 10.0};
    // Engine emits pnl in dollars at 0.01 lot via dollars_per_pt=1.0. To rescale
    // to other lot sizes we multiply by (target_lot / 0.01) = target_lot * 100.
    for (double L : lots) {
        const double scale = L / 0.01;
        std::fprintf(RPT, "  %-8.2f  $%11.2f  $%11.2f  $%11.2f  $%11.2f\n",
                     L,
                     full.sum * scale,
                     (full.sum / corpus_yrs) * scale,
                     full.worst * scale,
                     full.mdd * scale);
    }
    std::fprintf(RPT, "\n");

    // ── Recent-period slices ────────────────────────────────────────────────
    struct Slice { const char* label; int days; };
    Slice slices[5] = {
        {"Last 30 days",   30},
        {"Last 60 days",   60},
        {"Last 90 days",   90},
        {"Last 180 days", 180},
        {"Last 365 days", 365}
    };
    std::fprintf(RPT, "RECENT-PERIOD SLICES (entries on or after cutoff)\n");
    std::fprintf(RPT, "  %-15s  %-12s  %5s  %5s  %8s  %8s  %8s\n",
                 "Window", "Since", "n", "WR%", "Sharpe", "gross$", "mean$");
    std::fprintf(RPT, "  %s\n", std::string(73, '-').c_str());
    for (const auto& sl : slices) {
        const int64_t since = corpus_end - (int64_t)sl.days * 86400;
        Stats st = slice_stats(trades, since);
        const double yrs = sl.days / 365.0;
        const double tpy = (yrs > 0) ? (st.n / yrs) : 0;
        const double sh  = st.sharpe_ann(tpy);
        std::fprintf(RPT, "  %-15s  %-12s  %5d  %5.1f  %+8.3f  %+8.2f  %+8.3f\n",
                     sl.label, fmt_date(since).c_str(),
                     st.n, st.wr(), sh, st.sum, st.mean());
    }
    std::fprintf(RPT, "\n");

    // ── Recent trade journal (last 365 days) ────────────────────────────────
    const int64_t since_year = corpus_end - 365 * 86400;
    std::fprintf(RPT, "RECENT TRADE JOURNAL (last 365 days)\n");
    std::fprintf(RPT, "  %-3s  %-10s -> %-10s  %8s -> %8s  %s  %s\n",
                 "#", "Entry", "Exit", "Px in", "Px out", "PnL ", "Exit reason");
    int idx = 1;
    for (const auto& t : trades) {
        if (t.entry_ts < since_year) continue;
        std::fprintf(RPT, "  %-3d  %-10s -> %-10s  %8.2f -> %8.2f  %+7.3f  %s\n",
                     idx++,
                     fmt_date(t.entry_ts).c_str(), fmt_date(t.exit_ts).c_str(),
                     t.entry, t.exit, t.pnl, t.exit_reason ? t.exit_reason : "");
    }

    // ── Verdict ─────────────────────────────────────────────────────────────
    std::fprintf(RPT, "\nVERDICT\n");
    Stats last90  = slice_stats(trades, corpus_end - 90  * 86400);
    Stats last180 = slice_stats(trades, corpus_end - 180 * 86400);
    const bool live90  = last90.sum  > 0 && last90.n  >= 2;
    const bool live180 = last180.sum > 0 && last180.n >= 4;
    const bool full_pos = full.sum > 0;
    if (full_pos && live90 && live180) {
        std::fprintf(RPT, "  GREEN -- full corpus, last 180d, and last 90d all positive.\n");
        std::fprintf(RPT, "          Edge persists in current $4700-era regime.\n");
        std::fprintf(RPT, "          Recommend: wire shadow live + flip enabled after 30d shadow track.\n");
    } else {
        std::fprintf(RPT, "  CAUTION -- recent slice does not confirm full-corpus edge.\n");
    }

    if (RPT != stderr) std::fclose(RPT);
    std::fprintf(stderr, "[FTFR] -> %s  (full n=%d, sharpe=%+.2f)\n",
                 rpt_path.c_str(), full.n, sharpe_full);
    return 0;
}
