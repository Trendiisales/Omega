// clip_path_xau_rider4h.cpp -- per-H1-close PATH csv for the XauTrendRider4h
// bank-and-reload companion (S-2026-07-17 giveback-cover certification).
//
// Drives the REAL omega::XauTrendFollow4hEngine (spot prod config, engine_init
// L1183-1279: mask 0xC9, LOSS_CUT_PCT=1.5, vol-band 0.30/0.85 mask 0x8,
// min_impulse_atr=0.3, min_adx_entry=15.0, lot 0.01, max_spread 1.0) over an
// XAUUSD H1 file (ts,o,h,l,c), aggregating H4 buckets in-harness, feeding the
// production-order intrabar ticks (l,h,c -- MgcFastDonchianFeed precedent) and
// polling the REAL omega::XauTrendRiderEngine (N=2.5, lot 0.01,
// init(kXauTfNumCells)) after every host on_tick/on_h4_bar exactly as
// tick_gold.hpp L1728 does live.
//
// Live gates replicated where standalone-reproducible:
//   - gold_d1_trend() D1-EMA200 gate: fed per H4 close (deterministic).
//   - gold_regime()   sustained-bull short-block: fed per H1 close.
//   - gold_wt() WaveTrend gate: fails OPEN (bars_<warmup) -- live-only state.
//   - L2 entry gate / L2 trail flip: inert at atomics defaults -- live-only.
//
// Each rider LEG (initial arm AND every bank-reload segment) = its own
// companion leg, matching how the live snapshot keys StallCompanion rows
// (engine tag + leg open px). Output rows = the IBS clip-path format:
//   trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
// px = H1 close per bar from entry to exit; the FINAL row carries the leg's
// ACTUAL exit px (bank level / host-exit fill) -- the live 60s companion drive
// sees ~that price as its last upnl. cost_rt = IBKR XAU RT as a fraction of
// entry: 2*0.00015*px + spread (memory project-ibkr-cost-basis).
// $-basis: lot 0.01 x tick_value_multiplier(XAUUSD)=100 => $1.00 per XAU point,
// so the sweep's upnl$ = d*(px-entry) holds 1:1 (same convention as IBS $1/pt).
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/clip_path_xau_rider4h.cpp -o /tmp/cpxr
// usage: cpxr <h1.csv> <out_legs.csv> [half_spread=0.15] [intrabar=0]
//   intrabar=1 -> emit 3 path rows per H1 bar (ADVERSE extreme first, then
//   favourable extreme, then close) = the worse-of whipsaw stress for the
//   companion trail (live drives at 60s and CAN clip on an intrabar dip that
//   an H1-close path smooths away). Certification requires the chosen cell to
//   hold on BOTH grains.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendRiderEngine.hpp"

struct Bar { int64_t ts; double o, h, l, c; };

static std::vector<Bar> load_h1(const std::string& p) {
    std::vector<Bar> v; std::ifstream f(p); if (!f) return v;
    std::string ln;
    while (std::getline(f, ln)) {
        if (ln.empty() || ln[0] < '0' || ln[0] > '9') continue;
        Bar b; const char* s = ln.c_str(); char* e;
        b.ts = std::strtoll(s, &e, 10); if (*e != ',') continue; s = e + 1;
        b.o = std::strtod(s, &e); if (*e != ',') continue; s = e + 1;
        b.h = std::strtod(s, &e); if (*e != ',') continue; s = e + 1;
        b.l = std::strtod(s, &e); if (*e != ',') continue; s = e + 1;
        b.c = std::strtod(s, &e); v.push_back(b);
    }
    return v;
}

struct RiderLeg { int64_t entryTs, exitTs; int dir; double entry_px, exit_px; std::string reason; };

static int idx_at(const std::vector<Bar>& B, int64_t ts) {
    int lo = 0, hi = (int)B.size() - 1, r = 0;
    while (lo <= hi) { int m = (lo + hi) / 2; if (B[m].ts <= ts) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s <h1.csv> <out_legs.csv> [half_spread]\n", argv[0]); return 2; }
    const std::string h1p = argv[1], outp = argv[2];
    const double half = argc > 3 ? std::atof(argv[3]) : 0.15;
    const bool intrabar = argc > 4 && std::atoi(argv[4]) != 0;

    auto B = load_h1(h1p);
    std::fprintf(stderr, "[xau_rider4h] loaded %zu H1 bars\n", B.size());
    if (B.size() < 1000) { std::fprintf(stderr, "too few bars\n"); return 1; }

    // ── REAL parent, spot prod config (engine_init.hpp L1183-1279) ──
    static omega::XauTrendFollow4hEngine eng;
    eng.shadow_mode = true; eng.enabled = true;
    eng.LOSS_CUT_PCT = 1.5;                 // engine_init L1191
    eng.cell_enable_mask = 0xC9;            // Donchian + Keltner20 + EmaCross8_21 + KeltnerEMA50
    eng.lot = 0.01; eng.max_spread = 1.0;
    eng.use_vol_band_gate = true; eng.vol_band_low_pct = 0.30; eng.vol_band_high_pct = 0.85;
    eng.cell_vol_band_mask = 0x8;
    eng.min_impulse_atr = 0.3;
    eng.min_adx_entry = 15.0;
    eng.init();

    // ── REAL rider, engine_init L1517-1518 ──
    static omega::XauTrendRiderEngine rider;
    rider.enabled = true; rider.shadow_mode = false;
    rider.N = 2.5; rider.lot = 0.01; rider.tag = "XauTrendRider4h";
    rider.init(omega::kXauTfNumCells);

    int parent_n = 0; double parent_net = 0.0;
    auto host_cb = [&](const omega::TradeRecord& tr) {
        parent_n++; parent_net += tr.pnl * 100.0;   // ledger x100 -> USD at 0.01 lot
    };
    std::vector<RiderLeg> legs;
    double rider_net = 0.0;
    auto rider_cb = [&](const omega::TradeRecord& tr) {
        RiderLeg L;
        L.entryTs = tr.entryTs; L.exitTs = tr.exitTs;
        L.dir = (tr.side == "LONG") ? +1 : -1;
        L.entry_px = tr.entryPrice; L.exit_px = tr.exitPrice;
        L.reason = tr.exitReason;
        legs.push_back(L);
        rider_net += tr.pnl * 100.0;
    };

    // ── stream: H4 aggregation + prod tick order (l,h,c) + rider poll ──
    int64_t h4_bucket = -1; Bar h4{};
    for (size_t i = 0; i < B.size(); ++i) {
        const auto& b = B[i];
        const int64_t bkt = b.ts / 14400;
        if (h4_bucket >= 0 && bkt != h4_bucket) {
            // finalize the completed H4 bar BEFORE this H1 bar's ticks (live: the
            // H4-close branch runs on the first tick of the new period).
            const int64_t close_ms = (h4_bucket * 14400 + 14400) * 1000LL - 1000;
            omega::gold_d1_trend().on_h4_bar(h4.h, h4.l, h4.c, close_ms);
            omega::XauTfBar bar;
            bar.bar_start_ms = h4.ts * 1000LL; bar.open = h4.o; bar.high = h4.h; bar.low = h4.l; bar.close = h4.c;
            eng.on_h4_bar(bar, h4.c - half, h4.c + half, 0.0, close_ms, host_cb);
            rider.on_host(eng.pos, h4.c - half, h4.c + half, close_ms, rider_cb);
            h4_bucket = -1;
        }
        if (h4_bucket < 0) { h4_bucket = bkt; h4 = b; h4.ts = bkt * 14400; }
        else { h4.h = std::max(h4.h, b.h); h4.l = std::min(h4.l, b.l); h4.c = b.c; }

        // intrabar ticks at production order l, h, c
        const int64_t t_l = (b.ts + 600) * 1000LL, t_h = (b.ts + 1800) * 1000LL, t_c = (b.ts + 3599) * 1000LL;
        eng.on_tick(b.l - half, b.l + half, t_l, host_cb);
        rider.on_host(eng.pos, b.l - half, b.l + half, t_l, rider_cb);
        eng.on_tick(b.h - half, b.h + half, t_h, host_cb);
        rider.on_host(eng.pos, b.h - half, b.h + half, t_h, rider_cb);
        eng.on_tick(b.c - half, b.c + half, t_c, host_cb);
        rider.on_host(eng.pos, b.c - half, b.c + half, t_c, rider_cb);
        // regime feed per CLOSED H1 bar (live: tick accumulator)
        omega::gold_regime().on_h1_bar(b.o, b.h, b.l, b.c);
    }
    std::fprintf(stderr, "[xau_rider4h] parent trades n=%d net=$%.1f | rider legs n=%zu gross_net=$%.1f (no-cost)\n",
                 parent_n, parent_net, legs.size(), rider_net);

    // ── H1 SMA200 for the bull/bear split column ──
    std::vector<double> sm(B.size(), 0.0); double sum = 0;
    for (size_t i = 0; i < B.size(); ++i) {
        sum += B[i].c; if (i >= 200) sum -= B[i - 200].c;
        sm[i] = i >= 199 ? sum / 200.0 : B[i].c;
    }

    FILE* out = std::fopen(outp.c_str(), "w");
    if (!out) { std::fprintf(stderr, "cannot open %s\n", outp.c_str()); return 1; }
    std::fprintf(out, "trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
    int tid = 0;
    for (const auto& L : legs) {
        int ei = idx_at(B, L.entryTs); int xi = idx_at(B, L.exitTs); if (xi < ei) xi = ei;
        const int bull = (B[ei].c > sm[ei]) ? 1 : 0;
        // IBKR XAU RT cost as a fraction of entry: commission 1.5bp/side + full spread
        const double cost_rt = 2.0 * 0.00015 + (2.0 * half) / L.entry_px;
        int seq = 0;
        for (int i = ei; i <= xi && i < (int)B.size(); ++i) {
            const long long ms = (long long)B[i].ts * 1000LL;
            if (intrabar) {
                const double adv = (L.dir > 0) ? B[i].l : B[i].h;
                const double fav = (L.dir > 0) ? B[i].h : B[i].l;
                std::fprintf(out, "%d,%d,%lld,%d,%.4f,%.4f,0,%d,%.6f\n",
                             tid, seq++, ms, L.dir, L.entry_px, adv, bull, cost_rt);
                std::fprintf(out, "%d,%d,%lld,%d,%.4f,%.4f,0,%d,%.6f\n",
                             tid, seq++, ms, L.dir, L.entry_px, fav, bull, cost_rt);
            }
            const double px = (i == xi) ? L.exit_px : B[i].c;   // final row = ACTUAL leg exit fill
            std::fprintf(out, "%d,%d,%lld,%d,%.4f,%.4f,0,%d,%.6f\n",
                         tid, seq++, ms, L.dir, L.entry_px, px, bull, cost_rt);
        }
        tid++;
    }
    std::fclose(out);
    std::fprintf(stderr, "[xau_rider4h] wrote %d legs -> %s\n", tid, outp.c_str());
    return 0;
}
