// =============================================================================
//  OmegaMonteCarlo.hpp
//  Offline bootstrap P&L analysis + Benjamini-Hochberg FDR correction.
//
//  PURPOSE:
//    After a backtest or live session, assess whether observed strategy
//    performance is statistically significant or attributable to luck.
//    Two analyses:
//
//    1. BOOTSTRAP RESAMPLING
//       Resample the per-trade P&L vector B times with replacement.
//       For each bootstrap replicate compute: total P&L, Sharpe ratio,
//       max drawdown, win rate, expectancy.
//       Returns confidence intervals (2.5th and 97.5th percentiles = 95% CI).
//       Also computes the one-sided p-value:
//         p = fraction of bootstrap replicates with total P&L <= 0
//       i.e. "what fraction of the time would this strategy have been
//       unprofitable by chance?"
//
//    2. BENJAMINI-HOCHBERG FDR CORRECTION
//       When testing multiple strategies or parameter sets simultaneously,
//       naive per-test p-values are inflated by multiple comparisons.
//       BH controls the False Discovery Rate at level q (default 0.10).
//       Input: vector of (name, p-value) pairs.
//       Output: which tests pass BH at the given FDR level.
//       This prevents selecting a winner from 100 random strategies and
//       calling it significant (the "p-hacking" problem).
//
//  USAGE:
//    // Single strategy analysis:
//    omega::mc::BootstrapResult r = omega::mc::bootstrap_pnl(trades, 10000);
//    r.print();
//
//    // Multiple strategy comparison with FDR:
//    std::vector<omega::mc::TestResult> tests = {
//        {"GoldFlow_v1", 0.031},
//        {"GoldFlow_v2", 0.048},
//        {"GoldStack",   0.012},
//        {"BreakoutSP",  0.180},
//    };
//    auto bh = omega::mc::benjamini_hochberg(tests, 0.10);
//    for (const auto& t : bh) {
//        printf("%s: p=%.4f %s\n", t.name.c_str(), t.p_value,
//               t.significant ? "SIGNIFICANT" : "rejected");
//    }
//
//    // Load P&L from CSV:
//    auto trades = omega::mc::load_pnl_csv("logs/trade_close_log.csv", "pnl_raw");
//    if (!trades.empty()) { auto r = omega::mc::bootstrap_pnl(trades); r.print(); }
//
//  DEPENDENCIES:
//    Header-only. Requires C++17. Uses <random> for reproducible RNG.
//    No external dependencies.
//
//  RUNTIME:
//    10,000 bootstrap replicates on 1,000 trades ≈ 50ms on a modern CPU.
//    Not designed for the hot tick path — call from analysis tools or shutdown.
// =============================================================================

#pragma once
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega { namespace mc {

// ---------------------------------------------------------------------------
//  TradeRecord — minimal per-trade data for bootstrap analysis
// ---------------------------------------------------------------------------
struct TradeRecord {
    double  pnl    = 0.0;   // realised P&L in USD (negative = loss)
    double  lot    = 0.0;   // position size (optional, for per-lot normalisation)
    std::string sym;        // symbol (optional, for per-symbol breakdown)
};

// ---------------------------------------------------------------------------
//  BootstrapResult — output of bootstrap_pnl()
// ---------------------------------------------------------------------------
struct BootstrapResult {
    int    n_trades          = 0;
    int    n_replicates      = 0;
    double observed_pnl      = 0.0;  // actual total P&L
    double observed_sharpe   = 0.0;  // actual Sharpe (annualised, 252 trading days)
    double observed_winrate  = 0.0;  // actual win rate
    double observed_expectancy = 0.0; // actual mean P&L per trade

    // 95% confidence intervals from bootstrap distribution
    double ci_pnl_lo         = 0.0;  // 2.5th percentile
    double ci_pnl_hi         = 0.0;  // 97.5th percentile
    double ci_sharpe_lo      = 0.0;
    double ci_sharpe_hi      = 0.0;
    double ci_maxdd_lo       = 0.0;  // (worst drawdown at 2.5th pct)
    double ci_maxdd_hi       = 0.0;  // (best drawdown at 97.5th pct)

    // One-sided p-value: P(total P&L <= 0 | H0: random draw)
    double p_value           = 0.0;
    bool   significant_95    = false; // p < 0.05
    bool   significant_99    = false; // p < 0.01

    // Bootstrap distribution summary
    double boot_pnl_mean     = 0.0;
    double boot_pnl_std      = 0.0;

    void print() const {
        std::printf("\n=== Bootstrap Analysis (%d trades, %d replicates) ===\n",
                    n_trades, n_replicates);
        std::printf("  Observed total P&L : $%.2f\n", observed_pnl);
        std::printf("  Observed Sharpe    : %.3f\n",  observed_sharpe);
        std::printf("  Win rate           : %.1f%%\n", observed_winrate * 100.0);
        std::printf("  Expectancy/trade   : $%.2f\n", observed_expectancy);
        std::printf("  95%% CI P&L         : [$%.2f, $%.2f]\n", ci_pnl_lo, ci_pnl_hi);
        std::printf("  95%% CI Sharpe      : [%.3f, %.3f]\n", ci_sharpe_lo, ci_sharpe_hi);
        std::printf("  95%% CI Max DD      : [$%.2f, $%.2f]\n", ci_maxdd_lo, ci_maxdd_hi);
        std::printf("  Bootstrap P&L mean : $%.2f  std: $%.2f\n",
                    boot_pnl_mean, boot_pnl_std);
        std::printf("  p-value (one-sided): %.4f%s\n", p_value,
                    significant_99 ? "  ** SIGNIFICANT p<0.01 **" :
                    significant_95 ? "  * SIGNIFICANT p<0.05 *"  : "  (NOT significant)");
        std::printf("==============================================\n\n");
        std::fflush(stdout);
    }
};

// ---------------------------------------------------------------------------
//  Internal helpers
// ---------------------------------------------------------------------------
namespace detail {

inline double compute_sharpe(const std::vector<double>& pnl_vec) {
    if (pnl_vec.size() < 2) return 0.0;
    const double n = static_cast<double>(pnl_vec.size());
    double sum = 0.0, sum_sq = 0.0;
    for (double v : pnl_vec) { sum += v; sum_sq += v * v; }
    const double mean = sum / n;
    const double var  = (sum_sq / n) - (mean * mean);
    if (var <= 1e-18) return 0.0;
    const double std_dev = std::sqrt(var);
    // Annualise: assume each trade is one "period".
    // 252 trades/year is a rough approximation for daily-frequency systems.
    // For intraday, caller can normalise by setting a custom annualisation factor.
    // We use sqrt(252) as standard.
    return (mean / std_dev) * std::sqrt(252.0);
}

inline double compute_maxdd(const std::vector<double>& pnl_vec) {
    double peak = 0.0, cum = 0.0, dd = 0.0;
    for (double v : pnl_vec) {
        cum += v;
        if (cum > peak) peak = cum;
        const double drawdown = peak - cum;
        if (drawdown > dd) dd = drawdown;
    }
    return dd;
}

inline double compute_winrate(const std::vector<double>& pnl_vec) {
    if (pnl_vec.empty()) return 0.0;
    int wins = 0;
    for (double v : pnl_vec) if (v > 0.0) ++wins;
    return static_cast<double>(wins) / static_cast<double>(pnl_vec.size());
}

} // namespace detail

// ---------------------------------------------------------------------------
//  bootstrap_pnl — main bootstrap analysis function.
//  trades     : per-trade P&L vector.
//  n_replicates: number of bootstrap replicates (default 10,000).
//  seed       : RNG seed for reproducibility (0 = random seed).
// ---------------------------------------------------------------------------
inline BootstrapResult bootstrap_pnl(
        const std::vector<TradeRecord>& trades,
        int    n_replicates = 10000,
        uint64_t seed       = 42) {

    BootstrapResult res;
    res.n_trades     = static_cast<int>(trades.size());
    res.n_replicates = n_replicates;

    if (trades.empty()) {
        std::printf("[MONTE-CARLO] No trades to analyse.\n");
        return res;
    }

    // Extract P&L vector
    std::vector<double> pnl;
    pnl.reserve(trades.size());
    for (const auto& t : trades) pnl.push_back(t.pnl);

    // Observed statistics
    res.observed_pnl      = std::accumulate(pnl.begin(), pnl.end(), 0.0);
    res.observed_sharpe   = detail::compute_sharpe(pnl);
    res.observed_winrate  = detail::compute_winrate(pnl);
    res.observed_expectancy = res.observed_pnl / static_cast<double>(pnl.size());

    // Bootstrap loop
    std::mt19937_64 rng(seed > 0 ? seed : std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(pnl.size()) - 1);

    std::vector<double> boot_pnls, boot_sharpes, boot_maxdds;
    boot_pnls.reserve(n_replicates);
    boot_sharpes.reserve(n_replicates);
    boot_maxdds.reserve(n_replicates);

    std::vector<double> sample(pnl.size());
    int n_below_zero = 0;

    for (int b = 0; b < n_replicates; ++b) {
        for (size_t i = 0; i < pnl.size(); ++i)
            sample[i] = pnl[static_cast<size_t>(dist(rng))];

        const double boot_total = std::accumulate(sample.begin(), sample.end(), 0.0);
        boot_pnls.push_back(boot_total);
        boot_sharpes.push_back(detail::compute_sharpe(sample));
        boot_maxdds.push_back(detail::compute_maxdd(sample));

        if (boot_total <= 0.0) ++n_below_zero;
    }

    // Sort for percentiles
    std::sort(boot_pnls.begin(),   boot_pnls.end());
    std::sort(boot_sharpes.begin(), boot_sharpes.end());
    std::sort(boot_maxdds.begin(),  boot_maxdds.end());

    const int lo_idx = static_cast<int>(0.025 * n_replicates);
    const int hi_idx = static_cast<int>(0.975 * n_replicates);

    res.ci_pnl_lo    = boot_pnls[lo_idx];
    res.ci_pnl_hi    = boot_pnls[hi_idx];
    res.ci_sharpe_lo = boot_sharpes[lo_idx];
    res.ci_sharpe_hi = boot_sharpes[hi_idx];
    res.ci_maxdd_lo  = boot_maxdds[lo_idx];
    res.ci_maxdd_hi  = boot_maxdds[hi_idx];

    // Bootstrap distribution stats
    double bp_sum = 0.0, bp_sq = 0.0;
    for (double v : boot_pnls) { bp_sum += v; bp_sq += v * v; }
    res.boot_pnl_mean = bp_sum / n_replicates;
    res.boot_pnl_std  = std::sqrt(bp_sq / n_replicates
                                  - res.boot_pnl_mean * res.boot_pnl_mean);

    // p-value: fraction of replicates with total P&L <= 0
    res.p_value       = static_cast<double>(n_below_zero) / n_replicates;
    res.significant_95 = (res.p_value < 0.05);
    res.significant_99 = (res.p_value < 0.01);

    return res;
}

// Convenience overload: takes raw P&L vector
inline BootstrapResult bootstrap_pnl(
        const std::vector<double>& raw_pnl,
        int    n_replicates = 10000,
        uint64_t seed       = 42) {
    std::vector<TradeRecord> trades;
    trades.reserve(raw_pnl.size());
    for (double v : raw_pnl) { TradeRecord t; t.pnl = v; trades.push_back(t); }
    return bootstrap_pnl(trades, n_replicates, seed);
}

// ---------------------------------------------------------------------------
//  BHTestResult — output row for Benjamini-Hochberg correction
// ---------------------------------------------------------------------------
struct BHTestResult {
    std::string name;
    double      p_value       = 0.0;
    double      p_adjusted    = 0.0;   // BH-adjusted p-value (Simes)
    int         bh_rank       = 0;     // rank in sorted order (1 = smallest p)
    bool        significant   = false; // passes BH at FDR level q
};

// ---------------------------------------------------------------------------
//  benjamini_hochberg — BH FDR correction for multiple strategy p-values.
//
//  tests  : vector of (name, p_value) pairs (any order).
//  q      : FDR level (default 0.10 = allow 10% false discoveries).
//
//  Returns sorted results (most significant first) with significant flag set
//  for tests that pass BH correction at level q.
//
//  Procedure:
//    Sort tests by p-value ascending.
//    Find largest k such that p_(k) <= (k/m) * q.
//    Reject H0 for tests 1..k (i.e. they are significant).
// ---------------------------------------------------------------------------
inline std::vector<BHTestResult> benjamini_hochberg(
        const std::vector<std::pair<std::string, double>>& tests,
        double q = 0.10) {

    const int m = static_cast<int>(tests.size());
    if (m == 0) return {};

    // Build and sort
    std::vector<BHTestResult> results;
    results.reserve(m);
    for (const auto& t : tests) {
        BHTestResult r;
        r.name    = t.first;
        r.p_value = t.second;
        results.push_back(r);
    }
    std::sort(results.begin(), results.end(),
              [](const BHTestResult& a, const BHTestResult& b){
                  return a.p_value < b.p_value;
              });

    // Assign ranks and BH-adjusted p-values (Benjamini-Yekutieli / Simes formula)
    int last_sig = -1;
    for (int k = 0; k < m; ++k) {
        results[k].bh_rank    = k + 1;
        results[k].p_adjusted = results[k].p_value * static_cast<double>(m)
                                 / static_cast<double>(k + 1);
        // Cap adjusted p at 1.0
        results[k].p_adjusted = std::min(1.0, results[k].p_adjusted);
        if (results[k].p_value <= (static_cast<double>(k + 1) / m) * q)
            last_sig = k;
    }

    // Mark all tests up to last_sig as significant
    for (int k = 0; k <= last_sig; ++k)
        results[k].significant = true;

    return results;
}

// ---------------------------------------------------------------------------
//  print_bh_results — pretty-print BH results table.
// ---------------------------------------------------------------------------
inline void print_bh_results(const std::vector<BHTestResult>& results, double q = 0.10) {
    std::printf("\n=== Benjamini-Hochberg FDR Correction (q=%.2f) ===\n", q);
    std::printf("%-24s  %6s  %8s  %8s  %s\n",
                "Strategy", "Rank", "p-value", "p-adj", "Result");
    std::printf("%-24s  %6s  %8s  %8s  %s\n",
                "------------------------", "------",
                "--------", "--------", "--------");
    for (const auto& r : results) {
        std::printf("%-24s  %6d  %8.4f  %8.4f  %s\n",
                    r.name.c_str(), r.bh_rank, r.p_value, r.p_adjusted,
                    r.significant ? "SIGNIFICANT *" : "rejected");
    }
    int n_sig = 0;
    for (const auto& r : results) if (r.significant) ++n_sig;
    std::printf("%-24s  %d/%d significant at FDR q=%.2f\n\n",
                "SUMMARY:", n_sig, static_cast<int>(results.size()), q);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
//  load_pnl_csv — load per-trade P&L from a CSV file.
//
//  Reads the file and extracts the column named `pnl_col` (default "pnl_raw").
//  The CSV must have a header row. Skips lines where the column cannot be
//  parsed. Returns empty vector on file-open failure.
//
//  Expected CSV format (Omega trade_close_log.csv):
//    timestamp,symbol,side,entry,exit,pnl_raw,lot,engine,...
// ---------------------------------------------------------------------------
inline std::vector<TradeRecord> load_pnl_csv(
        const std::string& path,
        const std::string& pnl_col  = "pnl_raw",
        const std::string& sym_col  = "symbol",
        const std::string& lot_col  = "lot") {

    std::vector<TradeRecord> out;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::printf("[MONTE-CARLO] Cannot open %s\n", path.c_str());
        return out;
    }

    // Parse header to find column indices
    std::string line;
    if (!std::getline(f, line)) return out;

    auto split = [](const std::string& s, char delim) {
        std::vector<std::string> toks;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, delim)) toks.push_back(tok);
        return toks;
    };

    const auto headers = split(line, ',');
    int pnl_idx = -1, sym_idx = -1, lot_idx = -1;
    for (int i = 0; i < (int)headers.size(); ++i) {
        // Strip whitespace/quotes
        std::string h = headers[i];
        h.erase(std::remove_if(h.begin(), h.end(), [](char c){
            return c == '"' || c == '\r' || c == ' '; }), h.end());
        if (h == pnl_col) pnl_idx = i;
        if (h == sym_col) sym_idx = i;
        if (h == lot_col) lot_idx = i;
    }

    if (pnl_idx < 0) {
        std::printf("[MONTE-CARLO] Column '%s' not found in %s\n",
                    pnl_col.c_str(), path.c_str());
        return out;
    }

    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto cols = split(line, ',');
        if (pnl_idx >= (int)cols.size()) continue;
        TradeRecord t;
        try {
            t.pnl = std::stod(cols[static_cast<size_t>(pnl_idx)]);
        } catch (...) { continue; }
        if (sym_idx >= 0 && sym_idx < (int)cols.size())
            t.sym = cols[static_cast<size_t>(sym_idx)];
        if (lot_idx >= 0 && lot_idx < (int)cols.size()) {
            try { t.lot = std::stod(cols[static_cast<size_t>(lot_idx)]); }
            catch (...) {}
        }
        out.push_back(t);
    }

    std::printf("[MONTE-CARLO] Loaded %d trades from %s\n",
                (int)out.size(), path.c_str());
    return out;
}

// ---------------------------------------------------------------------------
//  per_symbol_bootstrap — run bootstrap_pnl for each symbol separately and
//  collect p-values for BH correction across symbols.
//
//  Usage:
//    auto syms = omega::mc::per_symbol_bootstrap(trades, 10000);
//    auto bh   = omega::mc::benjamini_hochberg(syms, 0.10);
//    omega::mc::print_bh_results(bh);
// ---------------------------------------------------------------------------
inline std::vector<std::pair<std::string, double>> per_symbol_bootstrap(
        const std::vector<TradeRecord>& trades,
        int n_replicates = 5000,
        uint64_t seed    = 42) {

    // Group by symbol
    std::unordered_map<std::string, std::vector<double>> by_sym;
    for (const auto& t : trades) {
        const std::string& s = t.sym.empty() ? "UNKNOWN" : t.sym;
        by_sym[s].push_back(t.pnl);
    }

    std::vector<std::pair<std::string, double>> results;
    for (const auto& kv : by_sym) {
        if (kv.second.size() < 5) continue;  // need at least 5 trades
        const auto r = bootstrap_pnl(kv.second, n_replicates, seed);
        results.emplace_back(kv.first, r.p_value);
    }
    return results;
}

}} // namespace omega::mc
