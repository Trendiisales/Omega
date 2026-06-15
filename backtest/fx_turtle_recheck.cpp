// fx_turtle_recheck.cpp — standalone recheck of FxTurtleH4Engine on more data.
// Operator mandate 2026-06-16: "recheck this engine, run more data; if we cannot
// improve it, it goes — marginal is not good enough."
//
// Drives FxTurtleH4Engine over a histdata tick file (YYYYMMDD HHMMSSmmm,bid,ask,vol).
// Long-only Donchian H4 turtle. Reports cost-inclusive net, PF, WR, both-halves,
// and a small param sweep to see if the marginal edge can be lifted.
//
// Build: clang++ -O2 -std=c++20 -DOMEGA_BACKTEST -Iinclude -o /tmp/fxturtle backtest/fx_turtle_recheck.cpp
// Run:   /tmp/fxturtle <datafile> <SYMBOL> [lookback hold sl_atr tp_atr]

#include "FxTurtleH4Engine.hpp"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <fstream>

using omega::TradeRecord;

// IBKR FX commission: ~0.2 bps/side notional. On 1.0 lot (100k) ~ $2/side, $4 RT.
// Spread is already embedded (entry at ask / exit at bid). Add commission haircut.
static const double FX_COMMISSION_RT = 4.0;  // USD per 1.0 lot round-trip

struct Stats {
    int n=0, wins=0;
    double gross_win=0, gross_loss=0, net=0;
    double net_h1=0, net_h2=0;
    int n_h1=0, n_h2=0;
};

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <file> <SYMBOL> [lb hold sl tp]\n", argv[0]); return 1; }
    const std::string path = argv[1];
    const std::string sym  = argv[2];

    omega::FxTurtleH4Engine eng;
    eng.p = omega::make_eurusd_turtle_h4_params();  // EUR/GBP identical params
    eng.symbol = sym;
    eng.shadow_mode = true;
    eng.enabled = true;
    eng.warmup_csv_path = "";  // no warm-seed: long file builds its own state
    if (argc >= 7) {
        eng.p.lookback_bars = atoi(argv[3]);
        eng.p.hold_max_h4   = atoi(argv[4]);
        eng.p.sl_atr_mult   = atof(argv[5]);
        eng.p.tp_atr_mult   = atof(argv[6]);
    }

    std::vector<TradeRecord> trades;
    auto on_close = [&](const TradeRecord& tr){ trades.push_back(tr); };

    std::ifstream f(path);
    if (!f.is_open()) { fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }

    std::string line;
    int64_t nticks=0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // YYYYMMDD HHMMSSmmm,bid,ask,vol
        // parse datetime -> epoch ms
        // chars: 0-7 date, 9-14 HHMMSS, 15-17 ms
        if (line.size() < 18) continue;
        int Y=(line[0]-'0')*1000+(line[1]-'0')*100+(line[2]-'0')*10+(line[3]-'0');
        int Mo=(line[4]-'0')*10+(line[5]-'0');
        int D=(line[6]-'0')*10+(line[7]-'0');
        int hh=(line[9]-'0')*10+(line[10]-'0');
        int mm=(line[11]-'0')*10+(line[12]-'0');
        int ss=(line[13]-'0')*10+(line[14]-'0');
        int ms=(line[15]-'0')*100+(line[16]-'0')*10+(line[17]-'0');
        // days from civil (Howard Hinnant)
        int y = Y - (Mo <= 2);
        int era = (y>=0?y:y-399)/400;
        unsigned yoe = (unsigned)(y - era*400);
        unsigned doy = (153*(Mo + (Mo>2?-3:9)) + 2)/5 + D-1;
        unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
        int64_t days = (int64_t)era*146097 + (int64_t)doe - 719468;
        int64_t epoch_ms = (days*86400LL + hh*3600LL + mm*60LL + ss)*1000LL + ms;

        // parse bid,ask
        size_t c1 = line.find(',', 18);
        if (c1==std::string::npos) continue;
        size_t c2 = line.find(',', c1+1);
        if (c2==std::string::npos) continue;
        double bid = atof(line.c_str()+c1+1);
        double ask = atof(line.c_str()+c2+1);
        if (bid<=0||ask<=0) continue;
        nticks++;
        eng.on_tick(bid, ask, epoch_ms, on_close);
        eng.check_weekend_close(bid, ask, epoch_ms, on_close);
    }

    // stats — cost-inclusive (subtract commission per trade scaled by lot)
    Stats s;
    int64_t span_lo=0, span_hi=0;
    if (!trades.empty()) { span_lo=trades.front().entryTs; span_hi=trades.back().exitTs; }
    int64_t mid_ts = span_lo + (span_hi-span_lo)/2;
    for (auto& tr : trades) {
        double net = tr.pnl - FX_COMMISSION_RT * tr.size;
        s.n++; s.net += net;
        if (net>0){ s.wins++; s.gross_win+=net; } else { s.gross_loss+=-net; }
        if (tr.exitTs < mid_ts){ s.net_h1+=net; s.n_h1++; } else { s.net_h2+=net; s.n_h2++; }
    }
    double pf = s.gross_loss>0 ? s.gross_win/s.gross_loss : (s.gross_win>0?999:0);
    double wr = s.n>0 ? 100.0*s.wins/s.n : 0;
    double avg = s.n>0 ? s.net/s.n : 0;

    printf("\n===== FxTurtleH4 %s  lb=%d hold=%d sl=%.1f tp=%.1f  ticks=%lld =====\n",
           sym.c_str(), eng.p.lookback_bars, eng.p.hold_max_h4, eng.p.sl_atr_mult, eng.p.tp_atr_mult, (long long)nticks);
    printf("trades=%d  net=$%.0f  PF=%.2f  WR=%.0f%%  avg=$%.1f/trade\n", s.n, s.net, pf, wr, avg);
    printf("  H1: n=%d net=$%.0f   H2: n=%d net=$%.0f   (both-halves %s)\n",
           s.n_h1, s.net_h1, s.n_h2, s.net_h2,
           (s.net_h1>0 && s.net_h2>0) ? "POSITIVE ✓" : "FAIL ✗");
    return 0;
}
