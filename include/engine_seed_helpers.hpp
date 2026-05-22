// =============================================================================
//  engine_seed_helpers.hpp -- generic warm-seed helper for D1/H4 engines
//
//  Solves cold-start problem: D1 engines with EMA100 / Donchian40 / etc need
//  many days of historical bars before they can fire first signal. Without
//  warm seed they sit idle waiting for the bar buffer to fill.
//
//  USAGE
//    From engine_init.hpp, AFTER configuring each engine:
//      omega::seed_h4_engine(g_xau_turtle_d1,
//                            "phase1/signal_discovery/warmup_XAUUSD_H4.csv",
//                            "XauTurtleD1");
//
//  Works for any engine with method signature:
//      on_h4_bar(double h4_high, double h4_low, double h4_close,
//                double bid, double ask, int64_t h4_close_ms,
//                CloseCallback cb)
//  AND a public bool field `enabled` (toggled to suppress entries during seed).
//
//  CSV format (matches phase1/signal_discovery/warmup_XAUUSD_H4.csv):
//      bar_start_ms,open,high,low,close
//      1709251200000,2044.41350,2046.91500,2044.15500,2046.60000
//      ...
//
//  Mechanism:
//    1. Capture engine.enabled state, set false (suppress entries).
//    2. Replay every H4 bar through on_h4_bar(). Engine updates internal
//       state (ATR, EMA, Donchian deques, prior-bar refs) but does NOT
//       open positions because the entry block checks `enabled`.
//    3. Restore engine.enabled.
//    4. Engine is now hot, can fire on FIRST live H4 close.
// =============================================================================

#pragma once
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdint>
#include "OmegaTradeLedger.hpp"
#include "SeedGuard.hpp"

namespace omega {

template<typename Engine>
size_t seed_h4_engine(Engine& eng, const std::string& path, const char* name) {
    const std::string actual = omega::resolve_seed_path(path);
    std::ifstream f(actual);
    if (!f.is_open()) {
        omega::seed_die(name, actual);  // [[noreturn]]
    }
    std::string line;
    std::getline(f, line);  // header

    const bool was_enabled = eng.enabled;
    eng.enabled = false;

    auto null_cb = [](const omega::TradeRecord& /*tr*/){};
    size_t n = 0;
    while (std::getline(f, line)) {
        long long ts_ms_ll = 0;
        double o=0, h=0, l=0, c=0;
        if (sscanf(line.c_str(), "%lld,%lf,%lf,%lf,%lf",
                   &ts_ms_ll, &o, &h, &l, &c) == 5) {
            // Synthetic bid/ask spread of 0.30 (XAU typical) around close
            const double bid = c - 0.15;
            const double ask = c + 0.15;
            eng.on_h4_bar(h, l, c, bid, ask,
                          static_cast<int64_t>(ts_ms_ll), null_cb);
            ++n;
        }
    }
    eng.enabled = was_enabled;
    if (n == 0) {
        omega::seed_die(name, actual);  // [[noreturn]]
    }
    printf("[SEED] %s: %zu H4 bars replayed from %s -- engine hot\n",
           name, n, actual.c_str());
    fflush(stdout);
    return n;
}

}  // namespace omega
