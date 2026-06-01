// =============================================================================
// l2_obi_replay.cpp -- replay captured IBKR DOM (ibkr_l2_<SYM>_*.csv) and test
// whether order-book imbalance (OBI) predicts the next-N-ms mid move. This is the
// gate on "can we trade L2 aggressively" -- if OBI has no predictive edge here,
// no live engine will either. Preliminary by nature (sparse book, partial days);
// treat PF as relative, shadow is the real test [[chimera-harness-optimism]].
//
// Captured schema:
//   ts_ms,mid,bid,ask,l2_imb,l2_bid_vol,l2_ask_vol,depth_bid_levels,depth_ask_levels,depth_events_total
//   l2_imb = bid_vol/(bid_vol+ask_vol)  in [0,1]  (1 = all bids, 0 = all asks)
//
// Strategy (symmetric OBI scalp): when smoothed OBI > 0.5+TH -> LONG; < 0.5-TH ->
// SHORT. Exit on: opposite signal, OR HORIZON_MS elapsed, OR TP/SL in price pts.
// One position. Cost = spread crossed + SLIP per side (price units).
//
//   g++ -std=c++17 -O3 -o backtest/l2_obi_replay backtest/l2_obi_replay.cpp
//   ./backtest/l2_obi_replay <csv> [csv2 ...]
// env: TH(0.15) EMA(0.0=off, else alpha) HORIZON_MS(3000) TP(0=off) SL(0=off)
//      SLIP(0.02) MINEVENTS(1) FRESHMS(1500) OOS(0.0)
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <fstream>
#include <algorithm>

struct Row { int64_t ts; double mid, bid, ask, imb; long events; };

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <ibkr_l2.csv> [more.csv ...]\n", argv[0]); return 1; }
    const double TH      = getenv("TH")       ? atof(getenv("TH"))       : 0.15;
    const double EMA     = getenv("EMA")      ? atof(getenv("EMA"))      : 0.0;   // 0=raw imb
    const int64_t HORIZON= getenv("HORIZON_MS")? atoll(getenv("HORIZON_MS")): 3000;
    const double TP      = getenv("TP")       ? atof(getenv("TP"))       : 0.0;   // price pts, 0=off
    const double SL      = getenv("SL")       ? atof(getenv("SL"))       : 0.0;
    const double SLIP    = getenv("SLIP")     ? atof(getenv("SLIP"))     : 0.02;
    const long  MINEV    = getenv("MINEVENTS")? atol(getenv("MINEVENTS")): 1;
    const int64_t FRESH  = getenv("FRESHMS")  ? atoll(getenv("FRESHMS")) : 1500;
    const double OOS     = getenv("OOS")      ? atof(getenv("OOS"))      : 0.0;

    std::vector<Row> rows;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a]); if (!f) { std::fprintf(stderr, "open fail %s\n", argv[a]); continue; }
        std::string ln; std::getline(f, ln);                 // header
        while (std::getline(f, ln)) {
            if (ln.empty() || ln[0] == 't') continue;
            const char* s = ln.c_str(); char* e = nullptr;
            int64_t ts = strtoll(s, &e, 10); if (*e != ',') continue;
            double mid = strtod(e + 1, &e); if (*e != ',') continue;
            double bid = strtod(e + 1, &e); if (*e != ',') continue;
            double ask = strtod(e + 1, &e); if (*e != ',') continue;
            double imb = strtod(e + 1, &e); if (*e != ',') continue;
            // skip l2_bid_vol, l2_ask_vol, depth_bid_levels, depth_ask_levels
            for (int k = 0; k < 4 && e; ++k) { e = strchr(e + 1, ','); }
            long ev = e ? strtol(e + 1, nullptr, 10) : 0;
            if (mid > 0 && bid > 0 && ask > 0 && ask >= bid)
                rows.push_back({ts, mid, bid, ask, imb, ev});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b){ return a.ts < b.ts; });
    if (rows.size() < 200) { std::fprintf(stderr, "too few rows (%zu)\n", rows.size()); return 1; }
    const size_t start = (OOS > 0 && OOS < 1) ? (size_t)(rows.size() * (1.0 - OOS)) : 0;

    // smoothed OBI
    std::vector<double> obi(rows.size());
    double e0 = rows[0].imb;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (EMA > 0.0) { e0 = EMA * rows[i].imb + (1 - EMA) * e0; obi[i] = e0; }
        else obi[i] = rows[i].imb;
    }

    bool pos = false; int dir = 0; double entry = 0; int64_t entry_ts = 0;
    double cum = 0, peak = 0, mdd = 0; int nw = 0, nl = 0; double gw = 0, gl = 0;
    std::vector<double> pnls;

    auto close = [&](double exit_mid) {
        double raw = dir * (exit_mid - entry);
        double pnl = raw - SLIP * 2.0;                       // round-trip slippage
        cum += pnl; if (cum > peak) peak = cum; double dd = peak - cum; if (dd > mdd) mdd = dd;
        if (pnl > 0) { nw++; gw += pnl; } else if (pnl < 0) { nl++; gl += -pnl; }
        pnls.push_back(pnl); pos = false; dir = 0;
    };

    for (size_t i = start; i < rows.size(); ++i) {
        const Row& r = rows[i];
        const double o = obi[i];
        if (pos) {
            double mv = dir * (r.mid - entry);
            if (SL > 0 && mv <= -SL)               close(entry - dir * SL);
            else if (TP > 0 && mv >= TP)            close(entry + dir * TP);
            else if (r.ts - entry_ts >= HORIZON)    close(r.mid);
            else {
                int want = (o > 0.5 + TH) ? +1 : (o < 0.5 - TH) ? -1 : 0;
                if (want != 0 && want != dir)       close(r.mid);   // flip
            }
        }
        if (!pos && r.events >= MINEV) {
            int sig = (o > 0.5 + TH) ? +1 : (o < 0.5 - TH) ? -1 : 0;
            if (sig != 0) { pos = true; dir = sig; entry = r.mid; entry_ts = r.ts; }
        }
    }
    if (pos) close(rows.back().mid);

    double pf = gl > 0 ? gw / gl : (gw > 0 ? 999 : 0);
    double hit = (nw + nl) ? 100.0 * nw / (nw + nl) : 0;
    int n = (int)pnls.size();
    double secs = (rows.back().ts - rows[start].ts) / 1000.0, hrs = secs / 3600.0;
    double sh = 0;
    if (n >= 2) { double m = 0; for (double v : pnls) m += v; m /= n;
        double s = 0; for (double v : pnls) s += (v - m) * (v - m); double sd = std::sqrt(s / (n - 1));
        if (sd > 0) sh = (m / sd) * std::sqrt((double)n); }
    std::printf("TH%.2f EMA%.2f H%lldms TP%.2f SL%.2f | rows=%zu hrs=%.1f | tr=%-5d net=%-9.3f PF=%.2f Sh=%+.2f win=%.0f%% mdd=%.3f trd/hr=%.1f\n",
        TH, EMA, (long long)HORIZON, TP, SL, rows.size() - start, hrs,
        n, cum, pf, sh, hit, mdd, hrs > 0 ? n / hrs : 0);
    return 0;
}
