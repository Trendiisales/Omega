// fx_upjump_be_floor_sweep.cpp — BE-FLOOR-ON-OPEN lever sweep for the FX + INDEX
// up-jump LADDER companion (include/FxUpJumpLadderCompanion.hpp, class FxLadderPair).
//
// Purpose (operator hard rule feedback-no-prebe-loss-ever, the GBPUSD "not allowed to
// be negative" fix): prove the new Config.be_floor_on_open flag makes EVERY booked clip
// net>=0 (nNeg==0, worst-clip bp >= 0) while PRESERVING the validated edge — because the
// floor is a book-time clamp only (booked_pct == max(unfloored_pct, 0) per clip), so it
// strictly dominates the unfloored book on net, both WF halves and worst-clip.
//
// Drives the PRODUCTION header standalone over the Tick H1 CSVs each cell was validated
// on, with deploy_ts=0 (books everything) and the LIVE exit params (wide_gb_frac 0.10,
// be_entry 0.08, wide_arm FX 1.0 / index 0.5, weekend Layers 2+3 ON = matches engine_init).
// For each cell it runs {floor OFF, floor ON} x {1x cost, 2x cost} and reports per-symbol:
//   clips | booked net% | worst-clip bp | nNeg | WF-H1 / WF-H2 halves | 2x-cost net%.
//
// Per-clip nets are read from the engine's own persisted <prefix><pair>_closed.csv (col 4
// = pct, net of rt cost AND the Layer-3 weekend haircut — the judged column), one fresh
// prefix per run so runs never collide.
//
// Build: g++ -std=c++17 -O2 -Iinclude -o /tmp/fx_befloor_sweep backtest/fx_upjump_be_floor_sweep.cpp
// Run from a scratch dir (engine writes swp_* persistence files there).
#include "../include/FxUpJumpLadderCompanion.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

struct Cell {
    const char* tag;     // display name
    const char* pair;    // engine pair id / live_sym
    const char* csv;     // Tick H1 file (ts,o,h,l,c)
    int    W;
    double thr;
    double rt;
    bool   short_dj;
    bool   is_index;     // index cells use wide_arm 0.5; FX use 1.0 (matches engine_init)
};

// All FX pairs (GBPUSD live; EURUSD/NZDUSD/AUDUSD/USDCAD disabled but validated for coverage)
// + all index ladders (US500/NAS100/GER40 bull+bear/M2K). GER40 is bull-gated live; validated
// on its bull file (represents the gated cell) and bear file (shows why it is gated).
static const Cell CELLS[] = {
    // -- FX (validated on the 3Y IBKR H1 midpoint feed) --
    {"GBPUSD[LIVE]", "GBPUSD", "/Users/jo/Tick/GBPUSD_IBKR_H1.csv", 48, 0.75, 2.0, false, false},
    {"EURUSD",       "EURUSD", "/Users/jo/Tick/EURUSD_IBKR_H1.csv", 48, 0.5,  2.0, false, false},
    {"NZDUSD",       "NZDUSD", "/Users/jo/Tick/NZDUSD_IBKR_H1.csv", 24, 1.5,  2.5, false, false},
    {"AUDUSD",       "AUDUSD", "/Users/jo/Tick/AUDUSD_IBKR_H1.csv", 72, 0.75, 2.0, false, false},
    {"USDCAD[short]","USDCAD", "/Users/jo/Tick/USDCAD_IBKR_H1.csv", 96, 0.5,  2.0, true,  false},
    // -- INDEX (validated on the research H1, index_upjump_ladder_sweep.py mapping) --
    {"US500[LIVE]",  "US500",  "/Users/jo/Tick/SPXUSD_2022_2026.h1.csv", 24, 2.0, 4.0, false, true},
    {"NAS100[LIVE]", "NAS100", "/Users/jo/Tick/NSXUSD_2022_2026.h1.csv", 24, 1.5, 3.0, false, true},
    {"GER40_bull[LIVE]","GER40","/Users/jo/Tick/GRXEUR_merged.h1.csv",   12, 1.5, 2.0, false, true},
    {"GER40_bear",   "GER40",  "/Users/jo/Tick/DAX2022_merged.h1.csv",   12, 1.5, 2.0, false, true},
    {"M2K[LIVE]",    "M2K",    "/Users/jo/Tick/M2K_h1.csv",              24, 1.0, 4.0, false, true},
};

struct Result {
    long   clips = 0;
    double net = 0.0;      // sum of per-clip net % (booked column)
    double worst_bp = 0.0; // min clip net in bp (>=0 required when floor ON)
    long   nNeg = 0;       // clips with net < 0
    double wf1 = 0.0, wf2 = 0.0;
    bool   ran = false;
};

// Run one cell config once, return the per-clip aggregate parsed from the closed CSV.
static Result run_cell(const Cell& C, bool floor_on, double cost_mult, int run_id) {
    Result R;
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "swp_%d_", run_id);
    std::string lp = C.pair; for (auto& ch : lp) ch = (char)std::tolower((unsigned char)ch);
    const std::string closed = std::string(prefix) + lp + "_closed.csv";
    std::remove(closed.c_str());   // fresh start (defensive; unique prefix already isolates)

    omega::FxLadderPair::Config cfg;
    cfg.pair = C.pair; cfg.live_sym = C.pair;
    cfg.W = C.W; cfg.thr = C.thr; cfg.rt_cost_bp = C.rt * cost_mult;
    cfg.short_downjump = C.short_dj;
    cfg.file_prefix = prefix;
    // LIVE exit params (engine_init parity):
    cfg.wide_gb_frac = 0.10;
    cfg.wide_arm_pct = C.is_index ? 0.5 : 1.0;
    cfg.be_entry_pct = 0.08; cfg.pend_bars = 4;
    cfg.block_weekend_arms = true;   // Layer 2
    cfg.weekend_carry_frac = 0.0;    // Layer 3 (carry zero size across the weekend gap)
    cfg.be_floor_on_open = floor_on; // <-- the lever under test

    omega::FxLadderPair p(cfg);
    p.set_exec(
        [](const std::string&, bool, double, double) -> std::string { return "x"; }, // non-empty ok
        [](const std::string&, bool, double, double, const std::string&) {},
        [](const std::string&, double, double) -> bool { return true; },
        [](const std::string&, const std::string&, bool, double, double, double, int64_t, int64_t, const char*) {});

    std::ifstream f(C.csv);
    if (!f.is_open()) { std::printf("  !! MISSING %s\n", C.csv); return R; }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue;
        double ts=0,o=0,h=0,l=0,c=0;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c) >= 5 && c>0)
            p.on_h1_bar((int64_t)ts, h, l, c, o);   // pass bar open -> Layer-3 gap capture faithful
    }
    R.ran = true;

    // Parse per-clip nets from the closed CSV: ti,entry,exit,pct,usd,ets,xts,reason
    std::vector<std::pair<int64_t,double>> clips;   // (xts, net%)
    std::ifstream cf(closed);
    for (std::string cl; std::getline(cf, cl); ) {
        if (cl.empty()) continue;
        int ti=0; double entry=0,exit=0,pct=0,usd=0; long long ets=0,xts=0; char reason[32]={0};
        if (std::sscanf(cl.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n]",
                        &ti,&entry,&exit,&pct,&usd,&ets,&xts,reason) >= 7)
            clips.push_back({(int64_t)xts, pct});
    }
    R.clips = (long)clips.size();
    if (clips.empty()) return R;

    double worst = clips[0].second;
    int64_t tmin = clips[0].first, tmax = clips[0].first;
    for (auto& cl : clips) { R.net += cl.second; if (cl.second < worst) worst = cl.second;
                             tmin = std::min(tmin, cl.first); tmax = std::max(tmax, cl.first);
                             if (cl.second < -1e-9) R.nNeg++; }
    R.worst_bp = worst * 100.0;
    const int64_t mid = tmin + (tmax - tmin) / 2;
    for (auto& cl : clips) (cl.first <= mid ? R.wf1 : R.wf2) += cl.second;
    return R;
}

int main() {
    std::printf("BE-FLOOR-ON-OPEN sweep — FxLadderPair (FX + INDEX up-jump ladder companion)\n");
    std::printf("LIVE exit params (wide_gb 0.10 / be_entry 0.08 / wide_arm FX1.0 idx0.5 / weekend L2+L3 ON), deploy_ts=0.\n\n");
    std::printf("%-16s %6s | %-38s | %-38s | %-16s\n", "cell", "clips",
                "OFF: net%  worst_bp  nNeg  WFh1/h2", "ON : net%  worst_bp  nNeg  WFh1/h2", "2x-cost net%");
    std::printf("%s\n", std::string(120,'-').c_str());

    int rid = 0;
    bool all_nneg0 = true, all_edge_ok = true;
    for (const Cell& C : CELLS) {
        Result off1 = run_cell(C, false, 1.0, rid++);
        Result on1  = run_cell(C, true,  1.0, rid++);
        Result off2 = run_cell(C, false, 2.0, rid++);
        Result on2  = run_cell(C, true,  2.0, rid++);
        if (!off1.ran) { std::printf("%-16s  (data missing)\n", C.tag); continue; }

        if (on1.nNeg != 0 || on1.worst_bp < -1e-6) all_nneg0 = false;
        // edge preserved: floor ON net >= OFF net (strict domination), both halves too.
        if (on1.net < off1.net - 1e-6 || on1.wf1 < off1.wf1 - 1e-6 || on1.wf2 < off1.wf2 - 1e-6)
            all_edge_ok = false;

        std::printf("%-16s %6ld | %+8.2f %8.1f %4ld  %+6.1f/%+6.1f | %+8.2f %8.1f %4ld  %+6.1f/%+6.1f | OFF%+7.2f ON%+7.2f\n",
            C.tag, on1.clips,
            off1.net, off1.worst_bp, off1.nNeg, off1.wf1, off1.wf2,
            on1.net,  on1.worst_bp,  on1.nNeg,  on1.wf1,  on1.wf2,
            off2.net, on2.net);

        if (off1.clips != on1.clips)
            std::printf("   !! clip-count changed OFF=%ld ON=%ld (floor must not alter trade count)\n",
                        off1.clips, on1.clips);
    }
    std::printf("%s\n", std::string(120,'-').c_str());
    std::printf("VERDICT: nNeg==0 & worst>=0 on every floor-ON cell : %s\n", all_nneg0 ? "PASS" : "FAIL");
    std::printf("VERDICT: floor-ON net & both WF halves >= floor-OFF : %s (strict domination = edge preserved)\n",
                all_edge_ok ? "PASS" : "FAIL");
    return 0;
}
