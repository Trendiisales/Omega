// =============================================================================
// l2_obi_overlay_test.cpp -- does requiring OBI-agreement at entry lift a
// breakout's win rate? Replays captured IBKR DOM (ibkr_l2_<SYM>_*.csv), runs the
// XauStraddle box-break logic (M15, boxN15, Wilder ATR14, stop=3*ATR, TP=1R) on
// the mid series, tags each entry with the slow-EMA OBI dir at fill, and splits
// outcomes into OBI-AGREE / DISAGREE / NEUTRAL buckets. If AGREE win% >> DISAGREE,
// the overlay (filter + size-up on agreement) is real -> wire it. Else don't.
//
//   g++ -std=c++17 -O3 -o backtest/l2_obi_overlay_test backtest/l2_obi_overlay_test.cpp
//   ./backtest/l2_obi_overlay_test <ibkr_l2.csv> [more.csv ...]
// env: TFMIN(15) BOXN(15) STOPATR(3) TPR(1) OBI_ALPHA(0.05) OBI_TH(0.15) SLIP(0.0)
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <algorithm>

struct Row { int64_t ts; double mid, imb; };

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <ibkr_l2.csv> [...]\n", argv[0]); return 1; }
    const int    TFMIN  = getenv("TFMIN")    ? atoi(getenv("TFMIN"))    : 15;
    const int    BOXN   = getenv("BOXN")     ? atoi(getenv("BOXN"))     : 15;
    const double STOPATR= getenv("STOPATR")  ? atof(getenv("STOPATR"))  : 3.0;
    const double TPR    = getenv("TPR")      ? atof(getenv("TPR"))      : 1.0;
    const double ALPHA  = getenv("OBI_ALPHA")? atof(getenv("OBI_ALPHA")): 0.05;
    const double OBI_TH = getenv("OBI_TH")   ? atof(getenv("OBI_TH"))   : 0.15;
    const double SLIP   = getenv("SLIP")     ? atof(getenv("SLIP"))     : 0.0;
    const int64_t TFMS  = (int64_t)TFMIN * 60000;

    std::vector<Row> rows;
    for (int a = 1; a < argc; ++a) {
        std::ifstream f(argv[a]); if (!f) { std::fprintf(stderr, "open fail %s\n", argv[a]); continue; }
        std::string ln; std::getline(f, ln);
        while (std::getline(f, ln)) {
            if (ln.empty() || ln[0] == 't') continue;
            const char* s = ln.c_str(); char* e = nullptr;
            int64_t ts = strtoll(s, &e, 10); if (*e != ',') continue;
            double mid = strtod(e + 1, &e); if (*e != ',') continue;
            e = strchr(e + 1, ','); if (!e) continue;          // skip bid
            e = strchr(e + 1, ','); if (!e) continue;          // skip ask
            double imb = strtod(e + 1, nullptr);
            if (mid > 0) rows.push_back({ts, mid, imb});
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row&a,const Row&b){return a.ts<b.ts;});
    if (rows.size() < 1000) { std::fprintf(stderr, "few rows %zu\n", rows.size()); return 1; }

    // M15 bar aggregation + Wilder ATR + box
    std::deque<double> highs, lows;
    double atr = 0, prevc = 0; int atrn = 0; double atrsum = 0;
    int64_t cur_bar = -1; double bh = 0, bl = 0, bc = 0;
    bool armed = false; double buy_stop = 0, sell_stop = 0;

    // OBI EMA
    double obi = 0;
    // position
    bool pos = false; int dir = 0; double entry = 0, sl = 0, tp = 0; int entry_obi = 0;

    struct Bucket { int tr = 0, w = 0; double net = 0, gw = 0, gl = 0; };
    Bucket agree, disagree, neutral, all;
    auto rec = [&](Bucket& b, double pnl) { b.tr++; if (pnl > 0) { b.w++; b.gw += pnl; } else b.gl += -pnl; b.net += pnl; };

    auto close = [&](double px) {
        double pnl = dir * (px - entry) - SLIP * 2.0;
        rec(all, pnl);
        if (entry_obi == 0)        rec(neutral, pnl);
        else if (entry_obi == dir) rec(agree, pnl);
        else                       rec(disagree, pnl);
        pos = false;
    };

    auto roll_bar = [&](double h, double l, double c) {
        highs.push_back(h); lows.push_back(l);
        const int keep = BOXN + 2;
        while ((int)highs.size() > keep) { highs.pop_front(); lows.pop_front(); }
        if (prevc > 0) {
            double tr = std::max({h - l, std::fabs(h - prevc), std::fabs(l - prevc)});
            if (atrn < 14) { atrsum += tr; if (++atrn == 14) atr = atrsum / 14; }
            else atr = (atr * 13 + tr) / 14;
        }
        prevc = c;
        armed = false;
        if (atr <= 0 || (int)highs.size() < BOXN + 1) return;
        const int sz = (int)highs.size();
        double H = highs[sz - 1 - BOXN], L = lows[sz - 1 - BOXN];
        for (int i = sz - BOXN; i < sz - 1; ++i) { if (highs[i] > H) H = highs[i]; if (lows[i] < L) L = lows[i]; }
        buy_stop = H; sell_stop = L;
        double mid_now = c;
        armed = (buy_stop > mid_now && sell_stop < mid_now);   // straddle arm guard
    };

    for (size_t i = 0; i < rows.size(); ++i) {
        const double mid = rows[i].mid;
        obi = ALPHA * (rows[i].imb - 0.5) + (1 - ALPHA) * obi;
        const int odir = (obi > OBI_TH) ? 1 : (obi < -OBI_TH) ? -1 : 0;

        int64_t b = (rows[i].ts / TFMS) * TFMS;
        if (cur_bar < 0) { cur_bar = b; bh = bl = bc = mid; }
        else if (b != cur_bar) { roll_bar(bh, bl, bc); cur_bar = b; bh = bl = bc = mid; }
        else { if (mid > bh) bh = mid; if (mid < bl) bl = mid; bc = mid; }

        if (pos) {
            if (dir > 0) { if (mid <= sl) close(sl); else if (mid >= tp) close(tp); }
            else         { if (mid >= sl) close(sl); else if (mid <= tp) close(tp); }
        }
        if (!pos && armed && atr > 0) {
            int side = 0; double fill = 0;
            if (mid >= buy_stop)       { side = 1;  fill = buy_stop; }
            else if (mid <= sell_stop) { side = -1; fill = sell_stop; }
            if (side != 0) {
                double risk = STOPATR * atr;
                entry = fill; dir = side; sl = fill - side * risk; tp = fill + side * TPR * risk;
                entry_obi = odir; pos = true; armed = false;
            }
        }
    }
    if (pos) close(rows.back().mid);

    auto line = [](const char* name, const Bucket& b) {
        double pf = b.gl > 0 ? b.gw / b.gl : (b.gw > 0 ? 999 : 0);
        double win = b.tr ? 100.0 * b.w / b.tr : 0;
        std::printf("%-10s tr=%-4d win=%4.0f%%  PF=%.2f  net=%+9.3f  avg=%+.3f\n",
            name, b.tr, win, pf, b.net, b.tr ? b.net / b.tr : 0);
    };
    std::printf("rows=%zu  OBI(alpha=%.2f th=%.2f)  straddle M%d boxN%d stop%.1f TP%.1fR\n",
        rows.size(), ALPHA, OBI_TH, TFMIN, BOXN, STOPATR, TPR);
    line("ALL", all);
    line("AGREE", agree);       // OBI dir == trade side at entry
    line("DISAGREE", disagree); // OBI dir == opposite
    line("NEUTRAL", neutral);   // OBI dir == 0
    return 0;
}
