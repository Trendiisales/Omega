// xau_bracket_tf_bt.cpp — operator bracket-OCO idea, ALL timeframes (S-2026-07-07w order 4).
//
// Spec under test (operator, MGC gold focus): "symbol shows direction -> open bracket
// (buy-stop above + sell-stop below), triggered side cancels other (OCO), move to BE
// fast, then tight trails". Adversarial harness: real duka ticks -> m1 bars (mid OHLC +
// real per-bar median spread) -> TF bars; positions managed on the m1 path with
// ADVERSE-FIRST intrabar ordering (o, adverse extreme, favorable extreme, c) so tight
// stops/BE churn is NOT hidden by bar-close evaluation (the registry §5 intrabar lesson).
//
// Triggers:
//   A "range straddle": flat -> bracket at N=20 TF-bar high/low ± 0.1*ATR (Donchian OCO).
//   B "impulse straddle" (operator literal "shows direction"): |ret(3 TF bars)| >= 1.5*ATR
//     -> bracket at cur ± 0.25*ATR, pending 3 TF bars.
// Protections (the operator's core levers):
//   P1 BE-fast(0.5 ATR -> entry+cost) + 30%-giveback trail   (his spec)
//   P2 BE-fast(0.5 ATR) + fixed 0.5*ATR peak trail           (tighter)
//   P3 no BE, 50%-giveback trail (arm 0.5 ATR)               (looser control)
//   P4 fixed SL 1*ATR / TP 2*ATR                             (no-BE no-trail control)
// Initial stop always = opposite bracket level. Both-levels-in-one-m1-bar = WHIPSAW,
// booked worst-case (enter one side, exit at the other level) and counted.
//
// Fills: level-to-level on mid; per-trade cost = real half-spread both sides (entry+exit
// bar median spread) + comm+slip bp param. MGC mode (--mgc) overrides the spot spread
// with 0.20 (2-tick MGC book) — futures-grade costs, the reason this retest is legit
// (GoldOrbRetrace died on SPOT costs). Stress = 2x total cost.
//
// Usage: xau_bracket_tf_bt <m1.csv> [comm_slip_bp=1.2] [--mgc]
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

struct M1 { long long ts; double o, h, l, c, spr; };

static std::vector<M1> load(const char* p) {
    std::vector<M1> v; v.reserve(800000);
    FILE* f = fopen(p, "rb");
    if (!f) { perror(p); exit(1); }
    char line[256]; fgets(line, sizeof line, f);
    M1 b;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%lld,%lf,%lf,%lf,%lf,%lf", &b.ts, &b.o, &b.h, &b.l, &b.c, &b.spr) == 6)
            v.push_back(b);
    fclose(f);
    return v;
}

struct Trade { long long xts; double net_bp; bool whip; };

struct Res {
    int n = 0, wins = 0, whips = 0;
    double net = 0, gw = 0, gl = 0, worst = 0;
    double h1 = 0, h2 = 0;
    void add(const Trade& t, long long mid_ts) {
        ++n; net += t.net_bp;
        if (t.net_bp > 0) { ++wins; gw += t.net_bp; } else gl -= t.net_bp;
        if (t.net_bp < worst) worst = t.net_bp;
        if (t.whip) ++whips;
        (t.xts < mid_ts ? h1 : h2) += t.net_bp;
    }
    double pf() const { return gl > 0 ? gw / gl : (gw > 0 ? 999 : 0); }
};

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s m1.csv [comm_slip_bp] [--mgc]\n", argv[0]); return 1; }
    double comm_slip_bp = 1.2;
    bool mgc = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--mgc")) mgc = true;
        else comm_slip_bp = atof(argv[i]);
    }
    auto m1 = load(argv[1]);
    if (m1.size() < 1000) { fprintf(stderr, "too few bars\n"); return 1; }
    const long long mid_ts = m1[m1.size() / 2].ts;
    printf("bars=%zu  %s  comm+slip=%.1fbp  spread=%s\n", m1.size(), argv[1], comm_slip_bp,
           mgc ? "MGC 0.20 fixed" : "real spot");

    const int TFS[] = {1, 5, 15, 30, 60, 240};
    printf("%-4s %-8s %-28s %6s %8s %6s %8s %8s %5s %7s %6s\n",
           "TF", "trig", "protection", "n", "net_bp", "PF", "H1", "H2", "WR%", "worst", "whip%");

    for (int tf_min : TFS) {
        const long long tf_s = (long long)tf_min * 60;
        // aggregate TF bars (index of last m1 bar in each TF bar)
        struct TFB { double o, h, l, c; long long ts; };
        std::vector<TFB> tfb; tfb.reserve(m1.size() / tf_min + 2);
        std::vector<int> tf_end;   // m1 index where TF bar completes
        {
            long long cur = -1; TFB b{};
            for (size_t i = 0; i < m1.size(); ++i) {
                long long k = m1[i].ts / tf_s;
                if (k != cur) {
                    if (cur >= 0) { tfb.push_back(b); tf_end.push_back((int)i - 1); }
                    cur = k; b = {m1[i].o, m1[i].h, m1[i].l, m1[i].c, m1[i].ts};
                }
                if (m1[i].h > b.h) b.h = m1[i].h;
                if (m1[i].l < b.l) b.l = m1[i].l;
                b.c = m1[i].c;
            }
            tfb.push_back(b); tf_end.push_back((int)m1.size() - 1);
        }
        // ATR14 per TF bar
        std::vector<double> atr(tfb.size(), 0);
        { double a = 0; int warm = 0;
          for (size_t i = 1; i < tfb.size(); ++i) {
              double tr = std::max({tfb[i].h - tfb[i].l, std::fabs(tfb[i].h - tfb[i - 1].c), std::fabs(tfb[i].l - tfb[i - 1].c)});
              if (warm < 14) { a += tr; if (++warm == 14) a /= 14; }
              else a = (a * 13 + tr) / 14;
              atr[i] = (warm >= 14) ? a : 0;
          } }

        for (int trig = 0; trig < 2; ++trig) {
            const char* TRIG = trig == 0 ? "range" : "impulse";
            struct Prot { const char* name; bool be; double be_arm_atr; int mode; double p1, p2; };
            // mode 0 = giveback trail (p1=arm_atr, p2=giveback frac)
            // mode 1 = fixed peak trail (p1=arm_atr, p2=trail_atr)
            // mode 2 = fixed SL/TP (p1=sl_atr, p2=tp_atr)
            const Prot prots[] = {
                {"P1 BE.5 + gb30 trail",  true,  0.5, 0, 0.5, 0.30},
                {"P2 BE.5 + 0.5ATR trail",true,  0.5, 1, 0.5, 0.50},
                {"P3 noBE + gb50 trail",  false, 0.0, 0, 0.5, 0.50},
                {"P4 SL1/TP2 control",    false, 0.0, 2, 1.0, 2.00},
            };
            for (const auto& P : prots) {
                Res res;
                // state
                bool pend = false; double bs = 0, ss = 0; int pend_ttl = 0; double atr0 = 0;
                bool in_pos = false, lng = false;
                double entry = 0, stop = 0, tp = 0, peak = 0, spr_e = 0;
                long long ets = 0;
                size_t ti = 0;   // next TF completion index
                auto close_tr = [&](double px, long long ts, bool whip) {
                    double gross = (lng ? (px / entry - 1.0) : (entry / px - 1.0)) * 1e4;
                    double se = mgc ? 0.20 : spr_e, sx = mgc ? 0.20 : spr_e;
                    double cost = ((se + sx) * 0.5 / entry) * 1e4 + comm_slip_bp;
                    Trade t{ts, gross - cost, whip};
                    res.add(t, mid_ts);
                    in_pos = false;
                };
                for (size_t i = 0; i < m1.size(); ++i) {
                    const M1& b = m1[i];
                    // manage position / pending on the m1 path, ADVERSE-FIRST ordering
                    double marks_l[4] = {b.o, b.l, b.h, b.c};   // long-adverse first
                    double marks_s[4] = {b.o, b.h, b.l, b.c};   // short-adverse first
                    for (int mi = 0; mi < 4; ++mi) {
                        if (in_pos) {
                            double px = lng ? marks_l[mi] : marks_s[mi];
                            if (lng) {
                                if (px > peak) peak = px;
                                bool armed = (peak - entry) >= P.p1 * atr0;
                                if (P.be && stop < entry && (peak - entry) >= P.be_arm_atr * atr0) stop = entry;
                                if (P.mode == 0 && armed) stop = std::max(stop, peak - P.p2 * (peak - entry));
                                if (P.mode == 1 && armed) stop = std::max(stop, peak - P.p2 * atr0);
                                if (px <= stop) { close_tr(stop, b.ts, false); break; }
                                if (P.mode == 2 && px >= tp) { close_tr(tp, b.ts, false); break; }
                            } else {
                                if (px < peak) peak = px;
                                bool armed = (entry - peak) >= P.p1 * atr0;
                                if (P.be && stop > entry && (entry - peak) >= P.be_arm_atr * atr0) stop = entry;
                                if (P.mode == 0 && armed) stop = std::min(stop, peak + P.p2 * (entry - peak));
                                if (P.mode == 1 && armed) stop = std::min(stop, peak + P.p2 * atr0);
                                if (px >= stop) { close_tr(stop, b.ts, false); break; }
                                if (P.mode == 2 && px <= tp) { close_tr(tp, b.ts, false); break; }
                            }
                        } else if (pend) {
                            bool hit_b = b.h >= bs, hit_s = b.l <= ss;
                            if (hit_b && hit_s) {
                                // WHIPSAW worst case: enter one side, stopped at the other level
                                lng = true; entry = bs; spr_e = b.spr;
                                close_tr(ss, b.ts, true);
                                pend = false; break;
                            } else if (hit_b || hit_s) {
                                lng = hit_b;
                                entry = lng ? bs : ss;
                                stop = lng ? ss : bs;      // initial stop = opposite bracket
                                if (P.mode == 2) { stop = lng ? entry - P.p1 * atr0 : entry + P.p1 * atr0;
                                                   tp   = lng ? entry + P.p2 * atr0 : entry - P.p2 * atr0; }
                                peak = entry; spr_e = b.spr; ets = b.ts;
                                in_pos = true; pend = false;
                                // continue managing remaining marks this bar
                            }
                        }
                    }
                    // TF bar completion -> update/place brackets
                    if (ti < tf_end.size() && (int)i == tf_end[ti]) {
                        size_t k = ti;
                        if (!in_pos && atr[k] > 0 && k >= 21) {
                            if (trig == 0) {
                                double hh = -1e18, ll = 1e18;
                                for (size_t j = k - 19; j <= k; ++j) { hh = std::max(hh, tfb[j].h); ll = std::min(ll, tfb[j].l); }
                                bs = hh + 0.1 * atr[k]; ss = ll - 0.1 * atr[k];
                                atr0 = atr[k]; pend = true; pend_ttl = 1;   // refreshed every TF close
                            } else {
                                double ret3 = tfb[k].c - tfb[k - 3].c;
                                if (std::fabs(ret3) >= 1.5 * atr[k]) {
                                    bs = tfb[k].c + 0.25 * atr[k]; ss = tfb[k].c - 0.25 * atr[k];
                                    atr0 = atr[k]; pend = true; pend_ttl = 3;
                                } else if (pend && --pend_ttl <= 0) pend = false;
                            }
                        }
                        ++ti;
                    }
                }
                if (in_pos) close_tr(m1.back().c, m1.back().ts, false);
                printf("%-4d %-8s %-28s %6d %+8.0f %6.2f %+8.0f %+8.0f %5.0f %+7.0f %6.1f\n",
                       tf_min, TRIG, P.name, res.n, res.net, res.pf(), res.h1, res.h2,
                       res.n ? 100.0 * res.wins / res.n : 0, res.worst,
                       res.n ? 100.0 * res.whips / res.n : 0);
            }
        }
    }
    return 0;
}
