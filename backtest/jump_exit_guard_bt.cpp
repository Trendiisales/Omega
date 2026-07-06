// =============================================================================
// jump_exit_guard_bt.cpp — DOMINANCE backtest for a JumpExitGuard overlay:
//   "if a parent trade is UNDERWATER and the jump detector fires AGAINST it,
//    close the parent at that H1 close" — vs what the trade actually did.
//
// This is a GUARD/OVERLAY test (edits ONE contract's exits) — the ONE case where
// a dominance comparison is fair per feedback-companion-independent-engine.
// It is NOT a companion book; companions are never judged this way.
//
// Trades come from the REAL shadow ledger (omega_trade_closes.csv archives,
// pulled from the VPS). Fills are HONEST: guard exits fill at the OBSERVED H1
// close (never at a model level). Cost identical on both sides (same RT debit),
// so the delta isolates the exit-price effect.
//
// Variants:
//   G0: fire when uPnL < 0 at an adverse jump          (any losing trade)
//   G1: fire when uPnL < -0.25*thr*entry at adverse jump (deeper adverse only)
//
// Usage:
//   ./jump_exit_guard_bt --sym XAUUSD --W 8 --thr 0.015 [--ledger path]
//   ./jump_exit_guard_bt --sym NAS100 --W 4 --thr 0.0075
// Symbols: XAUUSD | NAS100 (NAS100 also maps ledger USTEC/USTEC.F).
// Data sources are hard-wired per symbol below (best-first per H1 bucket).
//
// VERDICT RULE (operator-agreed): guard wires into a parent ONLY if it does not
// lower that parent's net (both WF halves), with >= 5 trades of evidence.
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

// ── H1 bar store: bucket(ts_hour) -> OHLC. First source to claim a bucket wins
//   (sources are listed best-first), so tick-derived bars never clobber real H1.
struct Bar { double o, h, l, c; };
static std::map<int64_t, Bar> g_bars;

static void add_tick(int64_t ts_sec, double px) {
    if (px <= 0.0) return;
    const int64_t b = (ts_sec / 3600) * 3600;
    auto it = g_bars.find(b);
    if (it == g_bars.end()) { g_bars[b] = {px, px, px, px}; return; }
    Bar& e = it->second;
    if (px > e.h) e.h = px;
    if (px < e.l) e.l = px;
    e.c = px;
}

// format: 'B' = H1 bar csv ts,o,h,l,c (epoch s|ms)  'D' = duka tick ts_ms,ask,bid
//         'L' = ibkr_l2 csv header ts_ms,mid,...
struct Src { char fmt; const char* path; };

static size_t load_src(const Src& s, std::map<int64_t, Bar>& fresh) {
    FILE* f = std::fopen(s.path, "rb");
    if (!f) { std::printf("  [data] MISSING %s\n", s.path); return 0; }
    char line[512]; size_t n = 0;
    while (std::fgets(line, sizeof line, f)) {
        if (!isdigit((unsigned char)line[0])) continue;         // header / junk
        double c1 = 0, c2 = 0, c3 = 0, c4 = 0; long long t = 0;
        if (s.fmt == 'B') {
            if (std::sscanf(line, "%lld,%lf,%lf,%lf,%lf", &t, &c1, &c2, &c3, &c4) != 5) continue;
            while (t > 40000000000LL) t /= 1000;                // ms/us -> s
            const int64_t b = (t / 3600) * 3600;
            if (!fresh.count(b)) { fresh[b] = {c1, c2, c3, c4}; n++; }
        } else if (s.fmt == 'D') {                              // ts_ms,ask,bid (ASK FIRST)
            if (std::sscanf(line, "%lld,%lf,%lf", &t, &c1, &c2) != 3) continue;
            while (t > 40000000000LL) t /= 1000;
            const int64_t b = (t / 3600) * 3600; const double mid = 0.5 * (c1 + c2);
            if (mid <= 0) continue;
            auto it = fresh.find(b);
            if (it == fresh.end()) { fresh[b] = {mid, mid, mid, mid}; n++; }
            else { Bar& e = it->second; if (mid > e.h) e.h = mid; if (mid < e.l) e.l = mid; e.c = mid; }
        } else {                                                // 'L': ts_ms,mid,...
            if (std::sscanf(line, "%lld,%lf", &t, &c1) != 2) continue;
            while (t > 40000000000LL) t /= 1000;
            const int64_t b = (t / 3600) * 3600;
            if (c1 <= 0) continue;
            auto it = fresh.find(b);
            if (it == fresh.end()) { fresh[b] = {c1, c1, c1, c1}; n++; }
            else { Bar& e = it->second; if (c1 > e.h) e.h = c1; if (c1 < e.l) e.l = c1; e.c = c1; }
        }
    }
    std::fclose(f);
    std::printf("  [data] %s -> %zu new H1 buckets\n", s.path, n);
    return n;
}

struct Trade {
    int64_t ets, xts; int side;                 // +1 long / -1 short
    double entry, exit_px; std::string engine;
};

int main(int argc, char** argv) {
    std::string sym = "XAUUSD";
    std::string ledger = "";                     // default set below
    int W = 8; double thr = 0.015; double cost_bp = 6.0;
    for (int i = 1; i < argc - 1; ++i) {
        if (!std::strcmp(argv[i], "--sym"))    sym = argv[i + 1];
        if (!std::strcmp(argv[i], "--W"))      W = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--thr"))    thr = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--cost"))   cost_bp = std::atof(argv[i + 1]);
        if (!std::strcmp(argv[i], "--ledger")) ledger = argv[i + 1];
    }
    const char* HOME = std::getenv("HOME");
    const std::string T = std::string(HOME) + "/Tick/";
    const std::string SCR = "/private/tmp/claude-501/-Users-jo-Omega/f4746f2b-55f5-4610-a538-c93d9cd9e019/scratchpad/";
    if (ledger.empty()) ledger = SCR + "ledger_all_raw.csv";

    // ── per-symbol source lists (BEST-FIRST: real H1 before tick-derived) ──
    std::vector<Src> srcs; std::vector<std::string> ledger_syms;
    if (sym == "XAUUSD") {
        ledger_syms = {"XAUUSD"};
        srcs = {
            {'B', "phase1/signal_discovery/warmup_XAUUSD_H1.csv"},           // -> 2026-04-24
            {'B', (SCR + "xau_june_h1_from_l2.csv").c_str()},                 // Jun 12-27
            {'B', (SCR + "gold_regime_h1.csv").c_str()},                      // Jun 23 - Jul 6
        };
        // tick/L2 fills for the gaps (May + early June)
        static std::string d1 = T + "duka_ticks/XAUUSD_2026_04.csv";
        static std::string d2 = T + "duka_ticks/XAUUSD_2026_05.csv";
        srcs.push_back({'D', d1.c_str()});
        srcs.push_back({'D', d2.c_str()});
        static std::vector<std::string> l2files;
        for (const char* d : {"2026-05-27","2026-05-28","2026-05-29","2026-06-01","2026-06-02",
                              "2026-06-03","2026-06-04","2026-06-08"})
            l2files.push_back(T + "ibkr_l2_vps_june/ibkr_l2_XAUUSD_" + d + ".csv");
        for (auto& p : l2files) srcs.push_back({'L', p.c_str()});
        if (cost_bp == 6.0) cost_bp = 6.0;
    } else if (sym == "NAS100") {
        ledger_syms = {"NAS100", "USTEC", "USTEC.F"};
        static std::string u1 = T + "duka_multiyear/usatechidxusd-tick-2022-01-01-2026-06-15T04-10.csv";
        static std::string u2 = T + "duka_multiyear/usatechidxusd-tick-2022-01-01-2026-06-15T04-25.csv";
        srcs = { {'D', u1.c_str()}, {'D', u2.c_str()} };
        if (cost_bp == 6.0) cost_bp = 3.0;                        // NAS100 real RT
    } else { std::printf("unknown --sym %s\n", sym.c_str()); return 1; }

    std::printf("=== JumpExitGuard dominance bt  sym=%s  W=%d thr=%.2f%% cost=%.1fbp  (honest close fills) ===\n",
                sym.c_str(), W, thr * 100.0, cost_bp);
    for (const auto& s : srcs) { std::map<int64_t, Bar> tmp; load_src(s, tmp);
        for (auto& kv : tmp) if (!g_bars.count(kv.first)) g_bars[kv.first] = kv.second; }
    std::printf("  merged H1 buckets: %zu\n", g_bars.size());
    if (g_bars.size() < 100) { std::printf("  INSUFFICIENT DATA\n"); return 1; }

    // flatten to arrays for the jump lookback
    std::vector<int64_t> bts; std::vector<double> bc;
    bts.reserve(g_bars.size()); bc.reserve(g_bars.size());
    for (auto& kv : g_bars) { bts.push_back(kv.first); bc.push_back(kv.second.c); }

    // ── ledger ──
    std::vector<Trade> trades;
    { FILE* f = std::fopen(ledger.c_str(), "rb");
      if (!f) { std::printf("no ledger %s\n", ledger.c_str()); return 1; }
      char line[4096];
      while (std::fgets(line, sizeof line, f)) {
          // split on ',' (fields in this ledger carry no embedded commas), strip quotes
          std::vector<std::string> fld; fld.reserve(42); std::string cur;
          for (char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
              if (*p == ',') { fld.push_back(cur); cur.clear(); }
              else if (*p != '"') cur.push_back(*p);
          }
          fld.push_back(cur);
          if (fld.size() < 13 || fld[2].empty() || !isdigit((unsigned char)fld[2][0])) continue;
          bool symhit = false; for (auto& s : ledger_syms) if (fld[8] == s) symhit = true;
          if (!symhit) continue;
          Trade t; t.ets = std::atoll(fld[2].c_str()); t.xts = std::atoll(fld[5].c_str());
          t.engine = fld[9];
          t.side = (fld[10] == "LONG" || fld[10] == "BUY") ? +1 : -1;
          t.entry = std::atof(fld[11].c_str()); t.exit_px = std::atof(fld[12].c_str());
          if (t.entry > 0 && t.exit_px > 0 && t.xts > t.ets) trades.push_back(t);
      }
      std::fclose(f); }
    std::printf("  ledger trades for %s: %zu\n", sym.c_str(), trades.size());
    if (trades.empty()) return 0;

    // ── replay each trade under the guard ──
    struct Acc { int n = 0, fired = 0, flips_saved = 0; double act = 0, grd = 0, worst_act = 0, worst_grd = 0; };
    auto run_variant = [&](double depth_frac, std::map<std::string, Acc>& per_eng,
                           Acc& tot, Acc& h1, Acc& h2, int64_t tmed, int& skipped) {
        for (const auto& t : trades) {
            // entry bar index: first bucket whose CLOSE lands after entry
            size_t i0 = std::lower_bound(bts.begin(), bts.end(), t.ets) - bts.begin();
            size_t i1 = std::upper_bound(bts.begin(), bts.end(), t.xts) - bts.begin();
            if (i0 >= bts.size() || i0 == i1) { skipped++; continue; }        // no bar coverage
            // coverage check: bars must actually span the window without a hole > 3h
            if (bts[i0] - t.ets > 3 * 3600) { skipped++; continue; }
            const double cost_pts = t.entry * cost_bp / 1e4;
            const double act_pts = t.side * (t.exit_px - t.entry) - cost_pts;
            double grd_pts = act_pts; bool fired = false;
            for (size_t i = i0; i < i1 && i < bts.size(); ++i) {
                if (i < (size_t)W) continue;
                const double j = bc[i] / bc[i - W] - 1.0;
                const double upnl = t.side * (bc[i] - t.entry);
                const bool adverse = (t.side > 0) ? (j <= -thr) : (j >= thr);
                if (adverse && upnl < -depth_frac * thr * t.entry) {
                    grd_pts = t.side * (bc[i] - t.entry) - cost_pts; fired = true; break;
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

    std::vector<int64_t> ets_sorted; for (auto& t : trades) ets_sorted.push_back(t.ets);
    std::sort(ets_sorted.begin(), ets_sorted.end());
    const int64_t tmed = ets_sorted[ets_sorted.size() / 2];

    for (double depth : {0.0, 0.25}) {
        std::map<std::string, Acc> per_eng; Acc tot, h1, h2; int skipped = 0;
        run_variant(depth, per_eng, tot, h1, h2, tmed, skipped);
        const double scale = 1e4 / trades[0].entry;   // rough pts->bp on first entry (per-trade mix varies; totals also shown in pts)
        std::printf("\n--- variant %s (fire when uPnL < -%.0f%% of thr) ---\n",
                    depth == 0.0 ? "G0" : "G1", depth * 100.0);
        std::printf("  covered=%d skipped(no-bars)=%d fired=%d loss-trades-improved=%d\n",
                    tot.n, skipped, tot.fired, tot.flips_saved);
        std::printf("  TOTAL   actual %+9.2f pts | guarded %+9.2f pts | delta %+9.2f pts (%+.0f bp-of-first-entry)\n",
                    tot.act, tot.grd, tot.grd - tot.act, (tot.grd - tot.act) * scale);
        std::printf("  WORST   actual %+9.2f pts | guarded %+9.2f pts\n", tot.worst_act, tot.worst_grd);
        std::printf("  WF-H1   actual %+9.2f | guarded %+9.2f | delta %+9.2f  (n=%d)\n", h1.act, h1.grd, h1.grd - h1.act, h1.n);
        std::printf("  WF-H2   actual %+9.2f | guarded %+9.2f | delta %+9.2f  (n=%d)\n", h2.act, h2.grd, h2.grd - h2.act, h2.n);
        std::printf("  per-engine (n>=5):\n");
        for (auto& kv : per_eng) if (kv.second.n >= 5)
            std::printf("    %-46s n=%3d fired=%3d  act %+9.2f  grd %+9.2f  delta %+8.2f  %s\n",
                        kv.first.c_str(), kv.second.n, kv.second.fired, kv.second.act, kv.second.grd,
                        kv.second.grd - kv.second.act,
                        (kv.second.grd - kv.second.act) > 1e-9 ? "GUARD+" :
                        ((kv.second.grd - kv.second.act) < -1e-9 ? "GUARD-" : "flat"));
    }
    std::printf("\nVERDICT RULE: wire the guard into a parent ONLY if delta >= 0 in BOTH WF halves\n"
                "and per-engine delta not negative (n>=5). Honest close fills; cost identical both sides.\n");
    return 0;
}
