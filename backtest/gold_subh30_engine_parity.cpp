// =============================================================================
// gold_subh30_engine_parity.cpp -- S-2026-07-14 sub-30m BIG GO wiring parity.
// Replays the CERTIFIED spot-1m splice (resampled to native 15m/10m rows, the
// study harness's exact resample) through the WIRED
// include/GoldBothWaysShortTfEngine.hpp instances (exact omega_main configs:
// row_secs == tf_secs pass-through) and compares the 6MO window vs the sweep
// (backtest/gold_subh30_tf_bt.cpp DON15_STOP=1 / DON10_SWEEP=1).
// Expected: EXACT match both legs (pass-through row = native bar, so the
// engine's per-row level-stop check == the harness's intrabar check on the
// same bar); n may differ by 1 (final still-open trade: engine holds, the
// harness force-closes at data end).
//   ref 15m DON 60/35 stop3.5: n=116 net +20131 L +4658 / S +15473
//   ref 10m DON 30/35 stop3.0: n=262 net +21281 L +7239 / S +14042
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/gold_subh30_engine_parity.cpp \
//        -o /tmp/gold_subh30_engine_parity
// RUN:   /tmp/gold_subh30_engine_parity /Users/jo/Tick/xau_1m_spliced_2024_2026.csv
// =============================================================================
#include "GoldBothWaysShortTfEngine.hpp"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using omega::GoldBothWaysShortTfEngine;

static const int64_t T6 = 1768348800, TE = 1784073600; // 2026-01-14 .. 2026-07-15
static const double COST = 0.41;

struct Bar { int64_t ts; double o, h, l, c; };

static std::vector<Bar> resample(const std::vector<Bar>& m1, int minutes) {
    std::vector<Bar> out; const int64_t step = 60LL * minutes;
    for (const auto& b : m1) {
        const int64_t bucket = (b.ts / step) * step;
        if (out.empty() || out.back().ts != bucket) out.push_back({bucket, b.o, b.h, b.l, b.c});
        else { Bar& x = out.back(); if (b.h > x.h) x.h = b.h; if (b.l < x.l) x.l = b.l; x.c = b.c; }
    }
    return out;
}

int main(int argc, char** argv) {
    const char* csv = argc > 1 ? argv[1] : "/Users/jo/Tick/xau_1m_spliced_2024_2026.csv";
    std::vector<Bar> m1;
    {
        std::ifstream f(csv); std::string ln;
        if (!f.is_open()) { std::fprintf(stderr, "no csv\n"); return 1; }
        while (std::getline(f, ln)) {
            Bar b; long long ts;
            if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &b.o, &b.h, &b.l, &b.c) == 5) {
                b.ts = ts; m1.push_back(b);
            }
        }
    }
    std::printf("1m bars=%zu\n", m1.size());

    GoldBothWaysShortTfEngine d15; d15.mech = GoldBothWaysShortTfEngine::Mech::DON;
    d15.tf_secs = 900; d15.row_secs = 900; d15.atr_n = 14;
    d15.don_in = 60; d15.don_out = 35; d15.stop_atr = 3.5; d15.trail_atr = 0.0;
    d15.warm_bars = 80; d15.engine_tag = "DON15"; d15.enabled = true; d15.retire_net_pts = -1e9;

    GoldBothWaysShortTfEngine d10; d10.mech = GoldBothWaysShortTfEngine::Mech::DON;
    d10.tf_secs = 600; d10.row_secs = 600; d10.atr_n = 14;
    d10.don_in = 30; d10.don_out = 35; d10.stop_atr = 3.0; d10.trail_atr = 0.0;
    d10.warm_bars = 80; d10.engine_tag = "DON10"; d10.enabled = true; d10.retire_net_pts = -1e9;

    struct Acc { int n = 0; double net = 0, lng = 0, sht = 0; } acc[2];
    GoldBothWaysShortTfEngine* engs[2] = { &d15, &d10 };
    const int mins[2] = { 15, 10 };
    for (int i = 0; i < 2; ++i) {
        auto rows = resample(m1, mins[i]);
        int idx = i;
        for (const auto& r : rows)
            engs[i]->on_30m_bar(r.o, r.h, r.l, r.c, r.ts, [idx, &acc](const omega::TradeRecord& tr) {
                if (tr.entryTs >= T6 && tr.entryTs < TE) {
                    const double pts = tr.pnl - COST;
                    acc[idx].n++; acc[idx].net += pts * 10.0;
                    if (tr.side == "LONG") acc[idx].lng += pts * 10.0; else acc[idx].sht += pts * 10.0;
                }
            });
    }
    const char* names[2] = { "DON 15m 60/35 s3.5", "DON 10m 30/35 s3.0" };
    const char* ref[2] = { "sweep n=116 +20131 L+4658/S+15473", "sweep n=262 +21281 L+7239/S+14042" };
    for (int i = 0; i < 2; ++i)
        std::printf("%-20s n=%-4d net=$%+9.1f L=$%+9.1f S=$%+9.1f   | %s\n",
                    names[i], acc[i].n, acc[i].net, acc[i].lng, acc[i].sht, ref[i]);
    return 0;
}
