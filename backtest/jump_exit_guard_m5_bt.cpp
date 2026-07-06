// =============================================================================
// jump_exit_guard_m5_bt.cpp — M5 variant of the JumpExitGuard dominance backtest.
//
// The H1 version (jump_exit_guard_bt.cpp) could not see 130/185 XAUUSD ledger
// trades because they are SUB-HOUR (LondonFixMomentum, straddles, Tsmom_H1
// scratches...). This variant folds ticks into M5 buckets so the guard can act
// inside the hour, and tests ONLY those sub-hour trades (duration < 1h).
//
// Same dominance framing as the H1 test — a guard/overlay editing ONE
// contract's exits is the ONE case where dominance-vs-actual is fair per
// feedback-companion-independent-engine. Fills are HONEST: guard exits at the
// OBSERVED M5 close. Cost identical both sides, so delta isolates exit timing.
//
// Data (sub-hour capable sources ONLY — H1 files cannot make M5 bars):
//   May 2026        duka_ticks/XAUUSD_2026_04/05.csv   (ts_ms,ask,bid)
//   May27 - Jun 8   ibkr_l2_vps_june/*.csv             (ts_ms,mid,...)
//   after Jun 8     NO sub-hour data locally -> trades SKIPPED, counted honestly
//
// Sweep: W in M5 bars {6,12,24} (=30m/1h/2h lookback) x thr {0.10..0.50}%,
// variants G0 (uPnL<0) / G1 (uPnL < -0.25*thr*entry).
//
// VERDICT RULE (operator-agreed, same as H1): wire ONLY if delta >= 0 in BOTH
// WF halves and per-engine delta not negative (n>=5).
//
// Usage: ./jump_exit_guard_m5_bt [--ledger path]
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

struct Bar { double o, h, l, c; };

// format: 'D' = duka tick ts_ms,ask,bid (ASK FIRST)  'L' = ibkr_l2 ts_ms,mid,...
struct Src { char fmt; std::string path; };

static size_t load_src(const Src& s, std::map<int64_t, Bar>& bars) {
    FILE* f = std::fopen(s.path.c_str(), "rb");
    if (!f) { std::printf("  [data] MISSING %s\n", s.path.c_str()); return 0; }
    char line[512]; size_t n = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (!isdigit((unsigned char)line[0])) continue;
        long long t = 0; double c1 = 0, c2 = 0, px = 0;
        if (s.fmt == 'D') {
            if (std::sscanf(line, "%lld,%lf,%lf", &t, &c1, &c2) != 3) continue;
            px = 0.5 * (c1 + c2);
        } else {                                    // 'L'
            if (std::sscanf(line, "%lld,%lf", &t, &c1) != 2) continue;
            px = c1;
        }
        if (px <= 0) continue;
        while (t > 40000000000LL) t /= 1000;        // ms/us -> s
        const int64_t b = (t / 300) * 300;          // M5 bucket
        auto it = bars.find(b);
        if (it == bars.end()) { bars[b] = {px, px, px, px}; n++; }
        else { Bar& e = it->second; if (px > e.h) e.h = px; if (px < e.l) e.l = px; e.c = px; }
    }
    std::fclose(f);
    std::printf("  [data] %s -> %zu new M5 buckets\n", s.path.c_str(), n);
    return n;
}

struct Trade {
    int64_t ets, xts; int side;
    double entry, exit_px; std::string engine;
};

int main(int argc, char** argv) {
    std::string ledger = "/private/tmp/claude-501/-Users-jo-Omega/f4746f2b-55f5-4610-a538-c93d9cd9e019/scratchpad/ledger_all_raw.csv";
    double cost_bp = 6.0;
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--ledger")) ledger = argv[i + 1];
        if (!std::strcmp(argv[i], "--cost"))   cost_bp = std::atof(argv[i + 1]);
    }
    const std::string T = std::string(std::getenv("HOME")) + "/Tick/";

    std::vector<Src> srcs = {
        {'D', T + "duka_ticks/XAUUSD_2026_04.csv"},
        {'D', T + "duka_ticks/XAUUSD_2026_05.csv"},
    };
    for (const char* d : {"2026-05-27","2026-05-28","2026-05-29","2026-06-01","2026-06-02",
                          "2026-06-03","2026-06-04","2026-06-08"})
        srcs.push_back({'L', T + "ibkr_l2_vps_june/ibkr_l2_XAUUSD_" + d + ".csv"});

    std::printf("=== JumpExitGuard M5 dominance bt  sym=XAUUSD  cost=%.1fbp  SUB-HOUR trades only  (honest M5-close fills) ===\n", cost_bp);
    std::map<int64_t, Bar> mbars;
    for (const auto& s : srcs) load_src(s, mbars);
    std::printf("  merged M5 buckets: %zu\n", mbars.size());
    if (mbars.size() < 500) { std::printf("  INSUFFICIENT DATA\n"); return 1; }

    std::vector<int64_t> bts; std::vector<double> bc;
    bts.reserve(mbars.size()); bc.reserve(mbars.size());
    for (auto& kv : mbars) { bts.push_back(kv.first); bc.push_back(kv.second.c); }

    // ── ledger: XAUUSD, sub-hour only ──
    std::vector<Trade> trades; int subhour_total = 0;
    { FILE* f = std::fopen(ledger.c_str(), "rb");
      if (!f) { std::printf("no ledger %s\n", ledger.c_str()); return 1; }
      char line[4096];
      while (std::fgets(line, sizeof line, f)) {
          std::vector<std::string> fld; fld.reserve(42); std::string cur;
          for (char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
              if (*p == ',') { fld.push_back(cur); cur.clear(); }
              else if (*p != '"') cur.push_back(*p);
          }
          fld.push_back(cur);
          if (fld.size() < 13 || fld[2].empty() || !isdigit((unsigned char)fld[2][0])) continue;
          if (fld[8] != "XAUUSD") continue;
          Trade t; t.ets = std::atoll(fld[2].c_str()); t.xts = std::atoll(fld[5].c_str());
          t.engine = fld[9];
          t.side = (fld[10] == "LONG" || fld[10] == "BUY") ? +1 : -1;
          t.entry = std::atof(fld[11].c_str()); t.exit_px = std::atof(fld[12].c_str());
          if (!(t.entry > 0 && t.exit_px > 0 && t.xts > t.ets)) continue;
          if (t.xts - t.ets >= 3600) continue;          // sub-hour only
          subhour_total++;
          trades.push_back(t);
      }
      std::fclose(f); }
    std::printf("  sub-hour XAUUSD ledger trades: %d\n", subhour_total);
    if (trades.empty()) return 0;

    std::vector<int64_t> ets_sorted; for (auto& t : trades) ets_sorted.push_back(t.ets);
    std::sort(ets_sorted.begin(), ets_sorted.end());
    const int64_t tmed = ets_sorted[ets_sorted.size() / 2];

    struct Acc { int n = 0, fired = 0, flips_saved = 0; double act = 0, grd = 0, worst_act = 0, worst_grd = 0; };

    auto run = [&](int W, double thr, double depth_frac, std::map<std::string, Acc>& per_eng,
                   Acc& tot, Acc& h1, Acc& h2, int& skipped) {
        for (const auto& t : trades) {
            size_t i0 = std::lower_bound(bts.begin(), bts.end(), t.ets) - bts.begin();
            size_t i1 = std::upper_bound(bts.begin(), bts.end(), t.xts) - bts.begin();
            if (i0 >= bts.size() || i0 == i1) { skipped++; continue; }
            if (bts[i0] - t.ets > 3 * 300) { skipped++; continue; }   // hole > 15min at entry
            const double cost_pts = t.entry * cost_bp / 1e4;
            const double act_pts = t.side * (t.exit_px - t.entry) - cost_pts;
            double grd_pts = act_pts; bool fired = false;
            for (size_t i = i0; i < i1 && i < bts.size(); ++i) {
                if (i < (size_t)W) continue;
                const double j = bc[i] / bc[i - W] - 1.0;
                const double upnl = t.side * (bc[i] - t.entry);
                const bool adverse = (t.side > 0) ? (j <= -thr) : (j >= thr);
                if (adverse && upnl < -depth_frac * thr * t.entry) {
                    grd_pts = t.side * (bc[i] - t.entry) - cost_pts; fired = true;
                    if (std::getenv("DEBUG_FIRES"))
                        std::printf("    [fire] %-30s ets=%lld side=%+d entry=%.2f exit=%.2f act=%+.2f grd=%+.2f d=%+.2f\n",
                                    t.engine.c_str(), (long long)t.ets, t.side, t.entry, t.exit_px,
                                    act_pts, grd_pts, grd_pts - act_pts);
                    break;
                }
            }
            auto upd = [&](Acc& a) {
                a.n++; a.fired += fired; a.act += act_pts; a.grd += grd_pts;
                if (act_pts < a.worst_act) a.worst_act = act_pts;
                if (grd_pts < a.worst_grd) a.worst_grd = grd_pts;
                if (act_pts < 0 && grd_pts > act_pts) a.flips_saved++;
            };
            upd(tot); upd(per_eng[t.engine]); upd(t.ets < tmed ? h1 : h2);
        }
    };

    // ── sweep ──
    std::printf("\n  W(M5) thr%%  var |   n  skip fire |  actual   guarded    delta |  WF-H1d   WF-H2d | verdict\n");
    struct Best { int W; double thr, depth, delta; };
    std::vector<Best> passers;
    for (int W : {6, 12, 24})
      for (double thr : {0.0010, 0.0020, 0.0030, 0.0050})
        for (double depth : {0.0, 0.25}) {
            std::map<std::string, Acc> per_eng; Acc tot, h1, h2; int skipped = 0;
            run(W, thr, depth, per_eng, tot, h1, h2, skipped);
            const double d1 = h1.grd - h1.act, d2 = h2.grd - h2.act;
            const bool pass = (d1 >= 0 && d2 >= 0 && tot.fired > 0);
            std::printf("  %2d   %4.2f  %s | %3d  %3d  %3d | %+8.2f %+8.2f %+8.2f | %+7.2f/%-3d %+7.2f/%-3d | %s\n",
                        W, thr * 100.0, depth == 0.0 ? "G0" : "G1",
                        tot.n, skipped, tot.fired, tot.act, tot.grd, tot.grd - tot.act,
                        d1, h1.n, d2, h2.n, pass ? "PASS-both-halves" : (tot.fired == 0 ? "no-fire" : "FAIL"));
            if (pass) passers.push_back({W, thr, depth, tot.grd - tot.act});
        }

    // per-engine detail for passing combos (or best 2 by delta if none pass)
    if (!passers.empty()) {
        std::sort(passers.begin(), passers.end(), [](const Best& a, const Best& b){ return a.delta > b.delta; });
        std::printf("\n  -- per-engine detail for WF-passing combos --\n");
        for (auto& p : passers) {
            std::map<std::string, Acc> per_eng; Acc tot, h1, h2; int skipped = 0;
            run(p.W, p.thr, p.depth, per_eng, tot, h1, h2, skipped);
            std::printf("  [W=%d thr=%.2f%% %s] total delta %+.2f\n", p.W, p.thr * 100.0,
                        p.depth == 0.0 ? "G0" : "G1", p.delta);
            for (auto& kv : per_eng) if (kv.second.n >= 5)
                std::printf("    %-46s n=%3d fired=%3d  act %+9.2f  grd %+9.2f  delta %+8.2f  %s\n",
                            kv.first.c_str(), kv.second.n, kv.second.fired, kv.second.act, kv.second.grd,
                            kv.second.grd - kv.second.act,
                            (kv.second.grd - kv.second.act) > 1e-9 ? "GUARD+" :
                            ((kv.second.grd - kv.second.act) < -1e-9 ? "GUARD-" : "flat"));
        }
    } else {
        std::printf("\n  NO combo passed both WF halves.\n");
    }

    std::printf("\nVERDICT RULE: wire ONLY if delta >= 0 in BOTH WF halves and per-engine delta\n"
                "not negative (n>=5). Skipped trades = no sub-hour data (post Jun 8) — counted honestly.\n");
    return 0;
}
