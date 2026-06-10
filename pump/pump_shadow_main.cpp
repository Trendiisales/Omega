// ─────────────────────────────────────────────────────────────────────────────
// pump_shadow_main — standalone C++ shadow runner for PumpScalpManager.
//
//   Reads the Python IBKR feed bridge on stdin and drives the real Omega engine
//   classes (PumpScalpManager -> PumpScalpEngine x3 per symbol). SHADOW ONLY:
//   places no orders, just logs the trades the live engine WOULD have taken so we
//   can measure real-time signal quality / dud-rate / fills before any live size.
//
//   Runs as its OWN process — NOT bolted into the live Omega trading loop — so it
//   cannot disrupt the running service or recording. Same C++ codebase though:
//   when shadow proves out, the same manager wires into engine_init.
//
//   Feed protocol (one record per line, from pump_feed_bridge.py):
//     B,SYM,TF_SEC,o,h,l,c,v,ts_ms      one CLOSED timeframe bar
//     P,SYM,px,ts_ms                    one price tick (drives the fast exit)
//
//   Build (VPS): see build_pump_shadow.bat.  Run: bridge | pump_shadow.exe
// ─────────────────────────────────────────────────────────────────────────────
#include "PumpScalpManager.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    omega::PumpScalpManager mgr;
    mgr.shadow_mode  = true;
    mgr.verbose      = true;
    mgr.day_gate_pct = 100.0;        // overridable below
    mgr.trail_pct    = 3.0;
    mgr.pyr_adds     = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--gate") && i+1 < argc) mgr.day_gate_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--trail") && i+1 < argc) mgr.trail_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pyr") && i+1 < argc) mgr.pyr_adds = atoi(argv[++i]);
    }

    FILE* log = fopen("pump_shadow_trades.csv", "a");
    if (log && ftell(log) == 0)
        fprintf(log, "exit_ts,engine,symbol,side,entry,exit,pnl_pts,size,reason\n");

    long n_trades = 0; double cum = 0.0;
    mgr.on_trade_record = [&](const omega::TradeRecord& t) {
        const double gross_pct = t.entryPrice > 0
            ? (t.side == "LONG" ? (t.exitPrice - t.entryPrice) : (t.entryPrice - t.exitPrice)) / t.entryPrice * 100.0
            : 0.0;
        ++n_trades; cum += gross_pct;
        printf("[SHADOW] %-13s %-6s %-5s %.4f->%.4f  %+.2f%%  %s  (n=%ld cum=%+.1f%%)\n",
               t.engine.c_str(), t.symbol.c_str(), t.side.c_str(),
               t.entryPrice, t.exitPrice, gross_pct, t.exitReason.c_str(), n_trades, cum);
        fflush(stdout);
        if (log) {
            fprintf(log, "%lld,%s,%s,%s,%.4f,%.4f,%.4f,%.2f,%s\n",
                    (long long)t.exitTs, t.engine.c_str(), t.symbol.c_str(), t.side.c_str(),
                    t.entryPrice, t.exitPrice, t.pnl, t.size, t.exitReason.c_str());
            fflush(log);
        }
    };

    printf("[pump_shadow] running. gate=%.0f%% trail=%.1f%% pyr=%d shadow=ON. reading feed...\n",
           mgr.day_gate_pct, mgr.trail_pct, mgr.pyr_adds);
    fflush(stdout);

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        if (line[0] == 'B' || line[0] == 'S') {           // B=live closed bar, S=seed (warm only)
            const bool is_seed = (line[0] == 'S');
            char sym[64]; int tf; double o,h,l,c,v; long long ts;
            if (sscanf(line+1, ",%63[^,],%d,%lf,%lf,%lf,%lf,%lf,%lld", sym, &tf, &o,&h,&l,&c,&v,&ts) == 8)
                mgr.on_bar(sym, tf, o,h,l,c,v, (int64_t)ts, is_seed);
        } else if (line[0] == 'P') {
            char sym[64]; double px; long long ts;
            if (sscanf(line, "P,%63[^,],%lf,%lld", sym, &px, &ts) == 3)
                mgr.on_price(sym, px, (int64_t)ts);
        }
        // anything else (comments/heartbeats) ignored
    }
    printf("[pump_shadow] feed closed. %ld shadow trades, cum %+.1f%%.\n", n_trades, cum);
    if (log) fclose(log);
    return 0;
}
