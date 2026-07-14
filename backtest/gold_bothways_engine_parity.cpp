// =============================================================================
// gold_bothways_engine_parity.cpp -- S-2026-07-14bc wiring parity check.
// Replays the CERTIFIED MGC 30m splice through the WIRED
// include/GoldBothWaysShortTfEngine.hpp instances (exact omega_main configs)
// and compares the 6MO window vs the S-2026-07-14ax study harness
// (backtest/gold_shorttf_bothways_bt.cpp). Expected: LONG legs match to the
// dollar (close-grade logic identical); SHORT legs ~5-8% lower (the engine
// checks level stops on every 30m sub-bar -- finer than the harness native
// bar, conservative); n-1 = the final still-open trade (engine holds, the
// harness force-closes at data end).
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/gold_bothways_engine_parity.cpp \
//        -o /tmp/gold_bothways_engine_parity
// RUN:   /tmp/gold_bothways_engine_parity backtest/data/mgc_30m_spliced_2024_2026.csv
// Verified 2026-07-14: KELT n239 +11780 L+4414.5(EXACT) / TF1040 L+6708.5(EXACT)
// / TF20100 L+3830.0(EXACT) / DONH1 L+4683.0(EXACT).
// =============================================================================
#include "GoldBothWaysShortTfEngine.hpp"
#include <cstdio>
#include <fstream>
#include <sstream>

using omega::GoldBothWaysShortTfEngine;

static const int64_t T6 = 1768348800, TE = 1784073600; // 2026-01-14 .. 2026-07-15
static const double COST = 0.41;

int main(int argc, char** argv) {
    const char* csv = argc > 1 ? argv[1] : "backtest/data/mgc_30m_spliced_2024_2026.csv";

    struct Cfg { const char* name; GoldBothWaysShortTfEngine e; };
    GoldBothWaysShortTfEngine kelt; kelt.mech = GoldBothWaysShortTfEngine::Mech::KELT;
    kelt.tf_secs = 1800; kelt.atr_n = 20; kelt.kelt_k = 1.25; kelt.stop_atr = 2.0;
    kelt.trail_atr = 2.5; kelt.time_stop_bars = 96; kelt.warm_bars = 100;
    kelt.engine_tag = "KELT"; kelt.enabled = true; kelt.retire_net_pts = -1e9;

    GoldBothWaysShortTfEngine t1; t1.mech = GoldBothWaysShortTfEngine::Mech::EMA;
    t1.tf_secs = 3600; t1.atr_n = 14; t1.ema_fast_n = 10; t1.ema_slow_n = 40;
    t1.imp_atr = 0.5; t1.stop_atr = 2.0; t1.trail_atr = 2.0; t1.warm_bars = 120;
    t1.engine_tag = "TF1040"; t1.enabled = true; t1.retire_net_pts = -1e9;

    GoldBothWaysShortTfEngine t2 = t1; t2.ema_fast_n = 20; t2.ema_slow_n = 100;
    t2.warm_bars = 300; t2.engine_tag = "TF20100";

    GoldBothWaysShortTfEngine don; don.mech = GoldBothWaysShortTfEngine::Mech::DON;
    don.tf_secs = 3600; don.atr_n = 14; don.don_in = 20; don.don_out = 10;
    don.stop_atr = 3.0; don.trail_atr = 0.0; don.warm_bars = 40;
    don.engine_tag = "DONH1"; don.enabled = true; don.retire_net_pts = -1e9;

    GoldBothWaysShortTfEngine* engs[4] = { &kelt, &t1, &t2, &don };
    struct Acc { int n = 0; double net = 0, lng = 0, sht = 0; } acc[4];

    std::ifstream f(csv); std::string ln;
    if (!f.is_open()) { std::fprintf(stderr, "no csv\n"); return 1; }
    while (std::getline(f, ln)) {
        if (ln.empty() || ln[0] < '0' || ln[0] > '9') continue;
        long long ts; double o, h, l, c;
        if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) != 5) continue;
        for (int i = 0; i < 4; ++i) {
            int idx = i;
            engs[i]->on_30m_bar(o, h, l, c, ts, [idx, &acc](const omega::TradeRecord& tr) {
                if (tr.entryTs >= T6 && tr.entryTs < TE) {
                    const double pts = tr.pnl - COST;   // engine pnl is gross pts (lot=1 default... lot=1.0)
                    acc[idx].n++; acc[idx].net += pts * 10.0;
                    if (tr.side == "LONG") acc[idx].lng += pts * 10.0; else acc[idx].sht += pts * 10.0;
                }
            });
        }
    }
    const char* names[4] = { "KELT k1.25 t2.5", "TF1H 10/40 t2.0", "TF1H 20/100 t2.0", "DON h1 20/10" };
    const char* ref[4] = { "study n=240 +12225 L+4415/S+7811", "study n=205 +11464 L+6709/S+4755",
                           "study n=176 +11367 L+3830/S+7537", "study n=77 +7403 L+4683/S+2721" };
    for (int i = 0; i < 4; ++i)
        std::printf("%-18s n=%-4d net=$%+9.1f L=$%+9.1f S=$%+9.1f   | %s\n",
                    names[i], acc[i].n, acc[i].net, acc[i].lng, acc[i].sht, ref[i]);
    return 0;
}
