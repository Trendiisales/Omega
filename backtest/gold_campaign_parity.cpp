// gold_campaign_parity.cpp -- REAL-engine parity driver for GoldCampaignD1AnchEngine.
// Feeds an M1 research file (ts,o,h,l,c,spr) straight into on_m1_bar() with
// bid/ask = c -/+ spr/2 (the exact harness fill convention) and prints each
// closed trade, for a row-by-row diff against the frozen-cell dump produced by
// gold_pullback_core_bt.cpp (env cell, DUMP=...). Registry parity mandate.
//
// BUILD: clang++ -std=c++17 -O2 -DOMEGA_BACKTEST -Iinclude backtest/gold_campaign_parity.cpp -o /tmp/gc_parity
// RUN:   /tmp/gc_parity /Users/jo/Tick/xau_m1_2024_2026.csv
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include "GoldCampaignD1AnchEngine.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <m1_csv> [out_csv]\n", argv[0]); return 1; }
    omega::GoldCampaignD1AnchEngine eng;
    eng.enabled = true; eng.shadow_mode = true; eng.lot = 0.01;

    FILE* out = argc > 2 ? std::fopen(argv[2], "w") : nullptr;
    if (out) std::fprintf(out, "ts_in,side,stop_bp,hod,gross_bp,ts_out\n");
    int ntr = 0; double net_bp = 0;
    auto cb = [&](const omega::TradeRecord& tr) {
        const int side = tr.side == "LONG" ? 1 : -1;
        const double gross = (side > 0 ? (tr.exitPrice - tr.entryPrice)
                                       : (tr.entryPrice - tr.exitPrice)) / tr.entryPrice * 1e4;
        const double stop0 = 0.0; (void)stop0;
        ++ntr; net_bp += gross;
        std::printf("TRADE %s in=%lld out=%lld px_in=%.3f px_out=%.3f gross=%+.2fbp reason=%s\n",
                    tr.side.c_str(), (long long)tr.entryTs, (long long)tr.exitTs,
                    tr.entryPrice, tr.exitPrice, gross, tr.exitReason.c_str());
        if (out) std::fprintf(out, "%lld,%d,,%d,%.2f,%lld\n",
                              (long long)tr.entryTs, side,
                              (int)((tr.entryTs % 86400) / 3600), gross, (long long)tr.exitTs);
    };

    std::ifstream f(argv[1]);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    std::string line; std::getline(f, line); size_t n = 0;
    while (std::getline(f, line)) {
        double ts = 0, o = 0, h = 0, l = 0, c = 0, sp = 0;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c, &sp) != 6) continue;
        if (c <= 0.0) continue;
        eng.on_m1_bar((int64_t)ts, o, h, l, c, sp, c - 0.5 * sp, c + 0.5 * sp, cb);
        ++n;
    }
    if (out) std::fclose(out);
    std::printf("[parity] %zu bars -> %d trades net=%+.0fbp (gross, pad0)\n", n, ntr, net_bp);
    return 0;
}
