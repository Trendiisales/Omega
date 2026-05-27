// =============================================================================
// gold_bracket_counterfactual.cpp -- one-shot harness asking "what would
//                                     g_bracket_gold have made today?"
//
// Built 2026-05-28 after the S37-X observability fix surfaced that the
// XAUUSD bracket engine has been hardcoded off since 2026-04-30 while the
// supervisor was approving its signals ~50% of cycles. Operator wants the
// counterfactual: today's tick tape replayed through the live class.
//
// Self-contained -- instantiates the production GoldBracketEngine and feeds
// it the L2 tick stream from C:\Omega\logs\l2_ticks_XAUUSD_YYYY-MM-DD.csv.
//
// Build:
//   cmake --build build --target gold_bracket_counterfactual -j
// Run:
//   ./build/gold_bracket_counterfactual <ticks.csv>
//   ./build/gold_bracket_counterfactual /tmp/cf/xau_2026-05-27.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>

#include "OmegaTradeLedger.hpp"
#include "BracketEngine.hpp"

struct Tick { int64_t ts_ms; double bid, ask; };

static std::vector<Tick> load_ticks(const std::string& path) {
    std::vector<Tick> v;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); std::exit(2); }
    char line[1024];
    if (std::fgets(line, sizeof(line), f)) {} // skip header
    while (std::fgets(line, sizeof(line), f)) {
        Tick t{};
        if (std::sscanf(line, "%lld,%lf,%lf", (long long*)&t.ts_ms, &t.bid, &t.ask) == 3
            && t.ts_ms > 0 && t.bid > 0 && t.ask > t.bid) {
            v.push_back(t);
        }
    }
    std::fclose(f);
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <xau_ticks_csv>\n", argv[0]);
        return 1;
    }

    auto ticks = load_ticks(argv[1]);
    std::fprintf(stderr, "[CF] loaded %zu XAUUSD ticks from %s\n",
                 ticks.size(), argv[1]);
    if (ticks.empty()) return 1;
    std::fprintf(stderr, "[CF] tape: %lld -> %lld (%.1fh)\n",
                 (long long)ticks.front().ts_ms, (long long)ticks.back().ts_ms,
                 (ticks.back().ts_ms - ticks.front().ts_ms) / 3600000.0);

    // Build the actual production engine -- no inline re-impl.
    omega::GoldBracketEngine eng;
    eng.symbol      = "XAUUSD";
    eng.shadow_mode = true;    // per BracketEngine.hpp:254, enables price-triggered fill sim in PENDING
    eng.ENTRY_SIZE  = 0.01;

    // Trade ledger via on_close callback.
    std::vector<omega::TradeRecord> trades;
    auto cb = [&](const omega::TradeRecord& t){ trades.push_back(t); };

    // Pump ticks.
    int64_t last_progress = 0;
    const int64_t N = (int64_t)ticks.size();
    for (int64_t i = 0; i < N; ++i) {
        const Tick& t = ticks[(size_t)i];
        eng.on_tick(t.bid, t.ask, t.ts_ms,
                    /*can_enter*/ true,
                    /*macro_regime*/ "NEUTRAL",
                    cb);
        if (i - last_progress >= 50000) {
            last_progress = i;
            std::fprintf(stderr, "\r[CF] %lld/%lld ticks  %zu trades  phase=%d  ",
                         (long long)i, (long long)N, trades.size(), (int)eng.phase);
        }
    }
    std::fprintf(stderr, "\n[CF] done. %zu trades.\n", trades.size());

    // Report
    double gross = 0, gw = 0, gl = 0;
    int wins = 0, losses = 0;
    double best = 0, worst = 0;
    for (const auto& t : trades) {
        gross += t.pnl;
        if (t.pnl > 0) { ++wins; gw += t.pnl; if (t.pnl > best) best = t.pnl; }
        else           { ++losses; gl += -t.pnl; if (t.pnl < worst) worst = t.pnl; }
    }
    const double wr = trades.empty() ? 0 : 100.0 * wins / trades.size();
    const double pf = gl > 0 ? gw / gl : 0;

    std::printf("\n");
    std::printf("=== COUNTERFACTUAL: g_bracket_gold on %s ===\n", argv[1]);
    std::printf("  trades       : %zu  (W=%d L=%d  WR=%.1f%%)\n",
                trades.size(), wins, losses, wr);
    std::printf("  gross_pnl    : %.2f\n", gross);
    std::printf("  profit_factor: %.3f\n", pf);
    std::printf("  best/worst   : %.2f / %.2f\n", best, worst);
    std::printf("  final phase  : %d (0=IDLE 1=ARMED 2=PENDING 3=CONFIRM 4=LIVE 5=COOLDOWN)\n",
                (int)eng.phase);
    std::printf("  bracket hi/lo: %.2f / %.2f\n", eng.bracket_high, eng.bracket_low);

    // Per-trade dump
    std::printf("\nTrade log:\n");
    for (size_t i = 0; i < trades.size(); ++i) {
        const auto& t = trades[i];
        std::printf("  #%zu %s entry=%.2f exit=%.2f pnl=%+.2f reason=%s\n",
                    i+1, t.side.c_str(), t.entryPrice, t.exitPrice, t.pnl,
                    t.exitReason.c_str());
    }
    return 0;
}
