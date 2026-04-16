// cbe_bt.cpp -- CompressionBreakoutEngine backtest against L2 tick data
// Columns: ts_ms bid ask ... ewm_drift(15)
// Build: clang++ -O3 -std=c++20 -I../include -o /tmp/cbe_bt cbe_bt.cpp
// Run:   /tmp/cbe_bt ~/Omega/data/l2_ticks_2026-04-16.csv

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <iostream>

#include "../include/OmegaTradeLedger.hpp"
#include "../include/CompressionBreakoutEngine.hpp"

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick {
    int64_t ms;
    double  bid, ask, drift;
    int     session_slot;  // derived from UTC hour
    bool    cvd_bear_div = false;
    bool    cvd_bull_div = false;
};

static int session_slot_from_ms(int64_t ms) {
    int64_t sec = ms / 1000LL;
    int hour = (int)((sec % 86400LL) / 3600LL);
    if (hour >= 7  && hour < 9)  return 1;  // London open
    if (hour >= 9  && hour < 11) return 2;  // London mid
    if (hour >= 11 && hour < 13) return 3;  // London/NY overlap
    if (hour >= 13 && hour < 17) return 4;  // NY core
    if (hour >= 17 && hour < 20) return 5;  // NY close
    if (hour >= 20 || hour < 7)  return 6;  // Asia
    return 0;
}

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return out; }

    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    int col_ms=-1, col_bid=-1, col_ask=-1, col_drift=-1, ci=0;
    {
        std::istringstream h(line);
        while (std::getline(h, tok, ',')) {
            if (tok=="ts_ms"||tok=="timestamp_ms") col_ms=ci;
            if (tok=="bid")       col_bid=ci;
            if (tok=="ask")       col_ask=ci;
            if (tok=="ewm_drift") col_drift=ci;
            ++ci;
        }
    }
    if (col_bid<0||col_ask<0) { fprintf(stderr,"No bid/ask cols\n"); return out; }

    out.reserve(200000);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back()=='\r') line.pop_back();

        static char buf[512];
        if (line.size() >= sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);

        std::vector<const char*> fields;
        fields.reserve(20);
        char* p = buf;
        fields.push_back(p);
        for (char* c = buf; *c; ++c) {
            if (*c==',') { *c='\0'; fields.push_back(c+1); }
        }

        int nc = (int)fields.size();
        int need = std::max({col_ms, col_bid, col_ask, col_drift});
        if (nc <= need) continue;

        try {
            Tick t;
            t.ms    = col_ms>=0   ? (int64_t)std::stod(fields[col_ms]) : 0;
            t.bid   = std::stod(fields[col_bid]);
            t.ask   = std::stod(fields[col_ask]);
            t.drift = col_drift>=0 && nc>col_drift ? std::stod(fields[col_drift]) : 0.0;
            t.session_slot = session_slot_from_ms(t.ms);
            out.push_back(t);
        } catch(...) { continue; }
    }
    return out;
}

// ─── ATR14 from M1 bars ───────────────────────────────────────────────────────
struct M1Bar {
    double hi, lo, cl;
    int64_t bar_ms;
};

struct ATRTracker {
    std::deque<M1Bar> bars;
    double atr = 0.0;
    double rsi = 50.0;

    // Tick RSI state (14-period)
    std::deque<double> rsi_gains, rsi_losses;
    double rsi_prev_mid = 0.0;

    M1Bar cur = {};
    int64_t cur_bar_ms = 0;

    // Returns true if a new M1 bar just closed
    bool update(double bid, double ask, int64_t ms) {
        const double mid = (bid + ask) * 0.5;
        const int64_t bar_ms = (ms / 60000LL) * 60000LL;

        // Update tick RSI
        if (rsi_prev_mid > 0.0) {
            double chg = mid - rsi_prev_mid;
            rsi_gains.push_back(chg > 0.0 ? chg : 0.0);
            rsi_losses.push_back(chg < 0.0 ? -chg : 0.0);
            if ((int)rsi_gains.size() > 14) { rsi_gains.pop_front(); rsi_losses.pop_front(); }
            if ((int)rsi_gains.size() == 14) {
                double ag=0, al=0;
                for (auto x : rsi_gains) ag+=x;
                for (auto x : rsi_losses) al+=x;
                ag/=14; al/=14;
                rsi = (al==0.0) ? 100.0 : 100.0 - 100.0/(1.0 + ag/al);
            }
        }
        rsi_prev_mid = mid;

        bool new_bar = false;
        if (cur_bar_ms == 0) {
            cur = {mid, mid, mid, bar_ms};
            cur_bar_ms = bar_ms;
        } else if (bar_ms != cur_bar_ms) {
            // Close current bar, start new one
            _push_bar(cur);
            _compute_atr();
            cur = {mid, mid, mid, bar_ms};
            cur_bar_ms = bar_ms;
            new_bar = true;
        } else {
            if (mid > cur.hi) cur.hi = mid;
            if (mid < cur.lo) cur.lo = mid;
            cur.cl = mid;
        }
        return new_bar;
    }

    M1Bar last_bar() const {
        if (bars.empty()) return {};
        return bars.back();
    }
    M1Bar prev_bar() const {
        if (bars.size() < 2) return {};
        return bars[bars.size()-2];
    }
    int bar_count() const { return (int)bars.size(); }

private:
    void _push_bar(const M1Bar& b) {
        bars.push_back(b);
        if ((int)bars.size() > 50) bars.pop_front();
    }
    void _compute_atr() {
        if ((int)bars.size() < 2) return;
        // True range
        double sum = 0.0;
        int n = std::min(14, (int)bars.size()-1);
        for (int i = (int)bars.size()-1; i >= (int)bars.size()-n; --i) {
            const M1Bar& b  = bars[i];
            const M1Bar& pb = bars[i-1];
            double tr = std::max({b.hi - b.lo,
                                  std::fabs(b.hi - pb.cl),
                                  std::fabs(b.lo - pb.cl)});
            sum += tr;
        }
        atr = sum / n;
    }
};

// ─── Main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "data/l2_ticks_2026-04-16.csv";

    printf("Loading %s...\n", path);
    auto ticks = load_csv(path);
    printf("Loaded %zu ticks\n", ticks.size());
    if (ticks.empty()) { fprintf(stderr, "No ticks\n"); return 1; }

    omega::CompressionBreakoutEngine cbe;
    cbe.shadow_mode = true;  // backtest runs in shadow mode

    ATRTracker atr_tracker;

    // Trade tracking
    std::vector<omega::TradeRecord> trades;
    double total_pnl = 0.0;
    int total_trades = 0;
    int wins = 0;

    auto on_close = [&](const omega::TradeRecord& tr) {
        trades.push_back(tr);
        double pnl_usd = (tr.pnl * 100.0);  // pnl is in lots*points, 1 lot gold = $100/pt
        total_pnl += pnl_usd;
        ++total_trades;
        if (pnl_usd > 0) ++wins;
        printf("[TRADE] %s %s entry=%.2f exit=%.2f pnl=$%.2f reason=%s held=%llds\n",
               tr.side.c_str(), tr.engine.c_str(),
               tr.entryPrice, tr.exitPrice, pnl_usd,
               tr.exitReason.c_str(),
               (long long)(tr.exitTs - tr.entryTs));
    };

    int64_t prev_bar_ms = 0;

    for (size_t i = 0; i < ticks.size(); ++i) {
        const Tick& t = ticks[i];
        if (t.bid <= 0.0 || t.ask <= 0.0) continue;

        // Update M1 bar tracker
        bool new_bar = atr_tracker.update(t.bid, t.ask, t.ms);

        // On new bar: feed bar to CBE
        if (new_bar && atr_tracker.bar_count() >= 2) {
            const M1Bar lb = atr_tracker.last_bar();
            cbe.on_bar(lb.cl, lb.hi, lb.lo, lb.cl,
                       atr_tracker.atr,
                       atr_tracker.rsi,
                       t.ms);
        }

        // Feed tick to CBE
        cbe.on_tick(t.bid, t.ask, t.ms,
                    t.drift,
                    t.cvd_bear_div,
                    t.cvd_bull_div,
                    t.session_slot,
                    on_close);
    }

    // Force close if still open at end
    if (cbe.has_open_position() && !ticks.empty()) {
        const Tick& last = ticks.back();
        cbe.force_close(last.bid, last.ask, last.ms, on_close);
    }

    // ── Results ───────────────────────────────────────────────────────────────
    printf("\n========= CBE BACKTEST RESULTS =========\n");
    printf("File:     %s\n", path);
    printf("Ticks:    %zu\n", ticks.size());
    printf("Trades:   %d\n", total_trades);
    if (total_trades > 0) {
        printf("Wins:     %d (%.0f%%)\n", wins, 100.0*wins/total_trades);
        printf("Total PnL: $%.2f\n", total_pnl);
        printf("Avg/trade: $%.2f\n", total_pnl/total_trades);

        // Per-trade detail
        printf("\n%-6s %-6s %-10s %-10s %-8s %-15s %-5s\n",
               "ID","Side","Entry","Exit","PnL$","Reason","Held");
        for (auto& tr : trades) {
            double pnl_usd = tr.pnl * 100.0;
            printf("%-6d %-6s %-10.2f %-10.2f %-8.2f %-15s %-5llds\n",
                   tr.id,
                   tr.side.c_str(),
                   tr.entryPrice,
                   tr.exitPrice,
                   pnl_usd,
                   tr.exitReason.c_str(),
                   (long long)(tr.exitTs - tr.entryTs));
        }
    } else {
        printf("No trades fired.\n");
        printf("Check [CBE-STATE] logs for compression detection status.\n");
    }

    return 0;
}
