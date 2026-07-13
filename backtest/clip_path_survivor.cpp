// =============================================================================
// clip_path_survivor.cpp -- per-trade PATH csv for the LIVE SurvivorPortfolio
// cells (S-2026-07-14 operator ask: "mimic engine for the companion-clips book:
// as soon as the trade is profitable open a mimic, it trades until it reverses
// and exits -- independent, zero effect on the real trade").
//
// Drives the REAL omega::survivor::Portfolio in the LIVE-FAITHFUL config
// (engine_init.hpp S-2026-07-08c): init_default_cells, dedup_mode=1,
// USTEC_4h_RSI_N7 culled, entry_veto = asym sustained-bear long-veto on BOTH
// USTEC and XAUUSD (daily close<SMA200 AND SMA falling, K=20 -- the same proxy
// survivor_gated_bt.cpp validated 1:1 against the live
// index_market_regime()/gold_regime() chokepoint).
//
// Output: one PATH csv per cell that traded:  <out_prefix>_<CELLTAG>.csv
//   trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt
// (same schema as every clip_path_*.cpp; feeds mimic_ladder_overlay which
// implements the shipped GoldTrendMimicLadder BE-ENTRY leg mechanism: leg is
// PENDING at parent open, opens only once price clears +be favourable = "we
// have a profitable trade", then peak-profit giveback trail = reversal exit,
// pre-arm lc cancel, independent window cap. Zero feedback to the parent.)
//
// build: clang++ -std=c++17 -O2 -I/Users/jo/Omega/include backtest/clip_path_survivor.cpp -o /tmp/clip_path_survivor
// run:   /tmp/clip_path_survivor /Users/jo/Tick/XAUUSD_2022_2026.h4.csv \
//            /Users/jo/Tick/NSXUSD_2022_2026.h4.csv \
//            /Users/jo/Tick/NDX_daily_2016_2026.csv <out_prefix>
// =============================================================================
#include "SurvivorPortfolio.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

struct Tick { long long ts_ms; std::string sym; double px; };
struct Bar4 { long long ts_sec; double o,h,l,c; };

static int load_bars(const std::string& path, const std::string& sym,
                     std::vector<Tick>& out, std::vector<Bar4>& bars) {
    std::ifstream f(path);
    if (!f) { std::printf("[load] CANNOT OPEN %s\n", path.c_str()); return 0; }
    std::string line; int n = 0;
    while (std::getline(f, line)) {
        if (line.empty() || (line[0] < '0' || line[0] > '9')) continue;
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss, t, ',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        long long ts = std::atoll(tok[0].c_str());
        long long ts_ms = ts > 100000000000LL ? ts : ts * 1000LL;
        double o = std::atof(tok[1].c_str()), h = std::atof(tok[2].c_str());
        double l = std::atof(tok[3].c_str()), c = std::atof(tok[4].c_str());
        if (h <= 0) continue;
        out.push_back({ ts_ms + 0,   sym, o });
        out.push_back({ ts_ms + 250, sym, h });
        out.push_back({ ts_ms + 500, sym, l });
        out.push_back({ ts_ms + 750, sym, c });
        bars.push_back({ ts_ms / 1000, o, h, l, c });
        ++n;
    }
    std::printf("[load] %-8s %6d bars  %s\n", sym.c_str(), n, path.c_str());
    return n;
}

// asym sustained-bear regime proxy (identical to survivor_gated_bt.cpp)
struct BearState {
    std::deque<double> closes;
    double prev_sma = 0; int k_bear = 0; bool bear = false;
    long long cur_day = -1; double day_close = 0;
    void push_daily(double c) {
        closes.push_back(c);
        if (closes.size() > 210) closes.pop_front();
        if (closes.size() < 200) { prev_sma = 0; return; }
        double s = 0; for (size_t i = closes.size() - 200; i < closes.size(); ++i) s += closes[i];
        double sma = s / 200.0;
        if (prev_sma > 0 && c < sma && sma < prev_sma) k_bear++; else k_bear = 0;
        bear = (k_bear >= 20);
        prev_sma = sma;
    }
    void on_tick(long long ts_ms, double px) {
        long long day = (ts_ms / 1000) / 86400;
        if (cur_day < 0) { cur_day = day; day_close = px; return; }
        if (day != cur_day) { push_daily(day_close); cur_day = day; }
        day_close = px;
    }
};

struct PTrade { std::string cell, sym; long long entryTs, exitTs; int dir; double entry_px; };

static std::vector<double> atr_pct(const std::vector<Bar4>& B) {
    int N = (int)B.size(); std::vector<double> ap(N, 0.0); double a = 0;
    for (int i = 1; i < N; i++) {
        double tr = std::max({ B[i].h - B[i].l, std::fabs(B[i].h - B[i-1].c), std::fabs(B[i].l - B[i-1].c) });
        a = i < 15 ? (a * (i - 1) + tr) / i : (a * 13 + tr) / 14;
        ap[i] = B[i].c > 0 ? a / B[i].c : 0.0;
    }
    return ap;
}
static std::vector<double> sma200(const std::vector<Bar4>& B) {
    int N = (int)B.size(); std::vector<double> s(N, 0.0); double sum = 0;
    for (int i = 0; i < N; i++) {
        sum += B[i].c; if (i >= 200) sum -= B[i-200].c;
        s[i] = i >= 199 ? sum / 200.0 : B[i].c;
    }
    return s;
}
static int idx_at(const std::vector<Bar4>& B, long long ts) {
    int lo = 0, hi = (int)B.size() - 1, r = 0;
    while (lo <= hi) { int m = (lo + hi) / 2; if (B[m].ts_sec <= ts) { r = m; lo = m + 1; } else hi = m - 1; }
    return r;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: %s xau_h4 ustec_h4 ndx_daily out_prefix\n", argv[0]);
        return 2;
    }
    std::vector<Tick> ticks;
    std::map<std::string, std::vector<Bar4>> barmap;
    load_bars(argv[1], "XAUUSD",  ticks, barmap["XAUUSD"]);
    load_bars(argv[2], "USTEC.F", ticks, barmap["USTEC.F"]);
    std::stable_sort(ticks.begin(), ticks.end(),
        [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });

    // seed the USTEC bear SMA from NDX daily closes strictly before tape start
    long long t0_s = ticks.front().ts_ms / 1000;
    std::map<std::string, BearState> reg;
    { std::ifstream f(argv[3]); std::string ln;
      int nseed = 0;
      while (std::getline(f, ln)) {
          long long ts; double o, h, l, c;
          if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c) == 5 && c > 0 && ts < t0_s) {
              reg["USTEC.F"].push_daily(c); ++nseed;
          }
      }
      std::printf("[seed] USTEC daily closes pre-tape: %d\n", nseed);
    }

    // LIVE-FAITHFUL portfolio (engine_init.hpp S-2026-07-08c)
    omega::survivor::Portfolio p;
    p.init_default_cells();
    p.dedup_mode = 1;
    for (auto& c : p.cells)
        if (std::strcmp(c.cfg.tag, "USTEC_4h_RSI_N7") == 0) c.st.enabled = false;
    p.entry_veto = [&reg](const char* sym, int side) -> bool {
        if (side <= 0) return false;                 // longs only; shorts never blocked
        auto it = reg.find(sym);
        return it != reg.end() && it->second.bear;
    };

    std::vector<PTrade> trades;
    auto cb = [&](const omega::TradeRecord& tr) {
        trades.push_back({ tr.engine, tr.symbol, tr.entryTs, tr.exitTs,
                           tr.side == "LONG" ? +1 : -1, tr.entryPrice });
    };
    for (const auto& tk : ticks) {
        reg[tk.sym].on_tick(tk.ts_ms, tk.px);
        p.on_tick(tk.sym, tk.px, tk.px, tk.ts_ms, cb);
    }
    std::printf("[run] closed trades total=%zu\n", trades.size());

    // per-symbol ATR%/bull precompute
    std::map<std::string, std::vector<double>> apm, smm;
    for (auto& kv : barmap) { apm[kv.first] = atr_pct(kv.second); smm[kv.first] = sma200(kv.second); }

    // emit one PATH csv per cell
    std::map<std::string, FILE*> outs;
    std::map<std::string, int> tids, counts;
    std::string prefix = argv[4];
    for (const auto& t : trades) {
        const auto& B = barmap[t.sym];
        if (B.empty()) continue;
        FILE*& out = outs[t.cell];
        if (!out) {
            std::string fn = prefix + "_" + t.cell + ".csv";
            out = std::fopen(fn.c_str(), "w");
            if (!out) { std::printf("[emit] cannot open %s\n", fn.c_str()); return 1; }
            std::fprintf(out, "trade_id,seq,exit_ms,dir,entry_px,px,atr_pct,bull,cost_rt\n");
        }
        int ei = idx_at(B, t.entryTs), xi = idx_at(B, t.exitTs);
        if (xi < ei) xi = ei;
        double atrp = apm[t.sym][ei];
        int bull = (B[ei].c > smm[t.sym][ei]) ? 1 : 0;
        // round-trip cost fraction: XAU spot CFD 2*1.5bp comm + 0.30 spread;
        // USTEC index CFD 2*0.5bp comm + 2.0pt spread (clip_path_idx_turtle convention)
        double cost_rt = (t.sym == "XAUUSD")
            ? 2.0 * 0.00015 + 0.30 / t.entry_px
            : 2.0 * 0.00005 + 2.00 / t.entry_px;
        int& tid = tids[t.cell]; int seq = 0;
        for (int i = ei; i <= xi && i < (int)B.size(); ++i) {
            std::fprintf(out, "%d,%d,%lld,%d,%.5f,%.5f,%.6f,%d,%.6f\n",
                tid, seq, (long long)B[i].ts_sec * 1000LL, t.dir, t.entry_px, B[i].c,
                atrp, bull, cost_rt);
            ++seq;
        }
        ++tid; ++counts[t.cell];
    }
    for (auto& kv : outs) std::fclose(kv.second);
    for (auto& kv : counts)
        std::printf("[emit] %-20s trades=%d -> %s_%s.csv\n",
                    kv.first.c_str(), kv.second, prefix.c_str(), kv.first.c_str());
    return 0;
}
