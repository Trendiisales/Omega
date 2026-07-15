// clip_path_bigcap_impulse.cpp -- FAITHFUL C++ validation of the REAL
// omega::BigCapImpulseSym engine (include/BigCap2pctImpulseCompanion.hpp).
//
// Drives the ACTUAL engine class (not a re-implementation) over the wide
// daily-close matrix data/rdagent/sp500_long_close.csv, with deploy_ts=0 so it
// books EVERY row (no deploy-forward suppression) and exec wired to no-op fns
// whose LEDGER callback captures each closed clip. Then computes the all-6:
//   n, PF, net% (of clip notional), WF H1/H2, bull, bear, worst.
//
// This C++ harness IS the validation (per the C++-only mandate). Reports the
// C++ numbers as truth.
//
// usage: clip_path_bigcap_impulse <wide_daily_close.csv> [thr] [hi_window] [gb] [max_hold] [catastrophe] [rt_bp] [mimic_be] [mimic_pend]
//   mimic_be > 0 (S-2026-07-13s) enables the 2 BE-mimic legs (M a2/gb75 + W8
//   a8/gb50) inside the REAL engine and reports the mimic book STANDALONE.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <ctime>
#include "BigCap2pctImpulseCompanion.hpp"

// The exact 45-name wired BIGCAP universe (engine_init.hpp ladder list).
static const char* UNIVERSE[] = {
    "NVDA","AMD","AVGO","MU","MRVL","SMCI","ARM","PLTR","TSLA","META","NFLX","CRWD",
    "SHOP","COIN","MSTR","SNOW","NOW","PANW","UBER","ABNB","DELL","ORCL","QCOM","INTC",
    "AMZN","GOOGL","MSFT","AAPL","CRM","ADBE","IONQ","RGTI","QBTS","ASTS","RKLB","NBIS",
    "CRWV","ALAB","CRDO","WDC","STX","DD","TPR","BMY","SWKS"
};

struct Trade { int64_t ets, xts; double gross, net; const char* reason; };
static std::vector<Trade> g_trades;
static std::vector<Trade> g_mtrades;   // S-2026-07-13s: mimic-book clips (tag "BigCap2pctMimic")
static double g_rt_bp = 20.0;

static int year_of(int64_t ts_sec) {
    time_t t = (time_t)ts_sec; struct tm g; gmtime_r(&t, &g); return g.tm_year + 1900;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <wide_daily_close.csv> [thr] [hi_window] [gb] [max_hold] [catastrophe] [rt_bp]\n", argv[0]); return 2; }
    const std::string csv = argv[1];
    const double thr   = argc > 2 ? std::atof(argv[2]) : 0.02;
    const int    hiw   = argc > 3 ? std::atoi(argv[3]) : 20;
    const double gb    = argc > 4 ? std::atof(argv[4]) : 0.90;
    const int    mh    = argc > 5 ? std::atoi(argv[5]) : 60;
    const double cata  = argc > 6 ? std::atof(argv[6]) : 15.0;
    g_rt_bp            = argc > 7 ? std::atof(argv[7]) : 20.0;
    const double mbe   = argc > 8 ? std::atof(argv[8]) : 0.0;   // 0 = mimics off (legacy parity)
    const int    mpend = argc > 9 ? std::atoi(argv[9]) : 5;

    // ── parse the wide daily-close matrix in C++ ──
    std::ifstream f(csv);
    if (!f.is_open()) { std::fprintf(stderr, "cannot open %s\n", csv.c_str()); return 1; }
    std::string header;
    if (!std::getline(f, header)) { std::fprintf(stderr, "empty file\n"); return 1; }
    std::vector<std::string> cols;
    { std::stringstream hs(header); std::string t; while (std::getline(hs, t, ',')) cols.push_back(t); }

    // map wanted names -> column index
    std::vector<int> want_col; std::vector<std::string> want_name;
    for (const char* nm : UNIVERSE) {
        for (size_t ci = 0; ci < cols.size(); ++ci) if (cols[ci] == nm) { want_col.push_back((int)ci); want_name.push_back(nm); break; }
    }

    // read all rows into (ts, values-per-wanted-col)
    struct Row { int64_t ts; std::vector<double> v; };
    std::vector<Row> rows;
    auto parse_ts = [](const std::string& line) -> int64_t {
        int y=0,m=0,d=0; if (std::sscanf(line.c_str(), "%d-%d-%d", &y,&m,&d) != 3) return 0;
        // Howard Hinnant days_from_civil
        int yy = y - (m <= 2); int64_t era = (yy>=0?yy:yy-399)/400;
        unsigned yoe = (unsigned)(yy - era*400);
        unsigned doy = (153*(m + (m>2?-3:9)) + 2)/5 + d - 1;
        unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
        return (era*146097LL + (int64_t)doe - 719468LL) * 86400LL;
    };
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        int64_t ts = parse_ts(line); if (ts <= 0) continue;
        std::vector<std::string> fields;
        { std::stringstream ls(line); std::string t; while (std::getline(ls, t, ',')) fields.push_back(t); }
        Row r; r.ts = ts; r.v.reserve(want_col.size());
        for (int ci : want_col) {
            double val = (ci < (int)fields.size() && !fields[ci].empty()) ? std::strtod(fields[ci].c_str(), nullptr) : 0.0;
            r.v.push_back(val);
        }
        rows.push_back(std::move(r));
    }
    std::fprintf(stderr, "[bc2] parsed %zu rows, %zu/%zu names mapped\n", rows.size(), want_name.size(), sizeof(UNIVERSE)/sizeof(UNIVERSE[0]));
    if (rows.size() < 50) { std::fprintf(stderr, "too few rows\n"); return 1; }

    // ── MEGACAP OVERLAY (durable 45/45 self-heal, S-2026-07-15) ──
    // The wide close matrix data/rdagent/sp500_long_close.csv is a FROZEN-2019 S&P
    // block: 24 of the 45 wired BIGCAP names (TSLA/META/PLTR/DELL/...) are PRESENT as
    // header columns but NEAR-EMPTY (0..513 of ~1788 rows) -> those engines are never
    // fed (on_daily_bar guarded by r.v[k]>0) -> the backtest SILENTLY exercised only
    // ~21 names, blind to the very high-flyers that carry the edge. This is the
    // "22/45" trap the shipped 2% validation kept hitting.
    // SELF-HEAL: for any wanted name that is under-filled in the wide matrix AND has a
    // real split-adjusted per-name file in the OHLC dir, overlay that name's closes
    // from the OHLC file. The engine + basket regime use only WITHIN-name % returns
    // (each name normalized to its first valid close), so a different split-adjust
    // anchor is invariant (see scratchpad/build_merged_close.py basis note). Data-
    // driven, not a hardcoded patch list: whatever is deficient + has an OHLC source
    // gets healed, so a future roster change can't silently reopen the 22/45 hole.
    {
        const char* od = std::getenv("BIGCAP_OHLC_DIR");
        const std::string odir = od ? od : "backtest/data/bigcap_daily_ohlc";
        auto load_ohlc = [&](const std::string& sym, std::unordered_map<int64_t,double>& out)->bool {
            std::ifstream of(odir + "/" + sym + ".csv");
            if (!of.is_open()) return false;
            std::string ln; std::getline(of, ln);            // header: date,o,h,l,c
            while (std::getline(of, ln)) {
                if (ln.empty()) continue;
                std::vector<std::string> fs; std::stringstream ss(ln); std::string t;
                while (std::getline(ss, t, ',')) fs.push_back(t);
                if (fs.size() < 5 || fs[0].size() < 8) continue;   // need YYYYMMDD + close
                const std::string iso = fs[0].substr(0,4)+"-"+fs[0].substr(4,2)+"-"+fs[0].substr(6,2);
                int64_t ts = parse_ts(iso); if (ts <= 0) continue;
                double c = std::strtod(fs[4].c_str(), nullptr);
                if (c > 0) out[ts] = c;
            }
            return true;
        };
        const int half = (int)(rows.size() / 2);
        int patched_names = 0, patched_cells = 0;
        for (size_t k = 0; k < want_name.size(); ++k) {
            int nz = 0; for (const auto& r : rows) if (r.v[k] > 0.0) ++nz;
            if (nz >= half) continue;                        // well-filled -> keep wide-CSV column
            std::unordered_map<int64_t,double> om;
            if (!load_ohlc(want_name[k], om) || om.empty()) {
                std::fprintf(stderr, "[bc2] OVERLAY %-6s under-filled (%d/%zu) but NO OHLC source -> still blind\n",
                             want_name[k].c_str(), nz, rows.size());
                continue;
            }
            int applied = 0;
            for (auto& r : rows) { auto it = om.find(r.ts); if (it != om.end()) { r.v[k] = it->second; ++applied; } }
            if (applied > 0) {
                ++patched_names; patched_cells += applied;
                std::fprintf(stderr, "[bc2] OVERLAY %-6s wide=%d/%zu -> OHLC filled %d rows\n",
                             want_name[k].c_str(), nz, rows.size(), applied);
            }
        }
        // count names that will actually trade (>=1 valid bar) after the overlay
        int live_names = 0;
        for (size_t k = 0; k < want_name.size(); ++k)
            for (const auto& r : rows) if (r.v[k] > 0.0) { ++live_names; break; }
        std::fprintf(stderr, "[bc2] megacap overlay: %d name(s) patched (%d cells) from %s ; %d/%zu names now have data\n",
                     patched_names, patched_cells, odir.c_str(), live_names, want_name.size());
    }

    // ── principled REGIME label: equal-weight basket (each name normalized to its
    //   first valid close) vs its own 200-day SMA. Bear = basket < 200DMA that day.
    //   A per-trade regime label by ENTRY DAY, independent of the calendar-2022 proxy.
    std::vector<double> basket(rows.size(), 0.0);
    {
        std::vector<double> first(want_col.size(), 0.0);
        for (size_t r = 0; r < rows.size(); ++r) {
            double sum = 0; int cnt = 0;
            for (size_t k = 0; k < want_col.size(); ++k) {
                double v = rows[r].v[k]; if (v <= 0) continue;
                if (first[k] <= 0) first[k] = v;
                sum += v / first[k]; ++cnt;
            }
            basket[r] = cnt ? sum / cnt : (r ? basket[r-1] : 1.0);
        }
    }
    std::vector<char> is_bear200(rows.size(), 0);   // 1 = basket < 200DMA
    { double run = 0; for (size_t r = 0; r < rows.size(); ++r) {
        run += basket[r]; if (r >= 200) run -= basket[r-200];
        double sma = r >= 199 ? run/200.0 : basket[r];
        is_bear200[r] = (basket[r] < sma) ? 1 : 0; } }
    // ts -> row index for entry-day regime lookup
    std::unordered_map<int64_t,int> ts2row;
    for (size_t r = 0; r < rows.size(); ++r) ts2row[rows[r].ts] = (int)r;

    // scratch dir for per-name persistence (fresh -> deploy_ts loads as 0 -> books everything)
    const char* scr = std::getenv("BC2_SCRATCH");
    const std::string sdir = scr ? scr : "/tmp/bc2_bt";
    std::system(("rm -rf " + sdir + " && mkdir -p " + sdir).c_str());

    // ── instantiate the REAL engine, one per name; wire capturing ledger ──
    std::vector<omega::BigCapImpulseSym> engines;
    engines.reserve(want_name.size());
    for (const auto& nm : want_name) {
        omega::BigCapImpulseSym::Config c;
        c.sym = nm; c.live_sym = nm;
        c.thr = thr; c.hi_window = hiw; c.gb = gb; c.max_hold = mh; c.catastrophe = cata; c.rt_cost_bp = g_rt_bp;
        c.mimic_be_pct = mbe; c.mimic_pend = mpend;
        const std::string base = sdir + "/" + nm;
        c.deploy_path = base + "_dep.txt";  c.bars_path = base + "_bars.csv";
        c.book_path   = base + "_book.txt"; c.live_path = base + "_live.txt";
        c.closed_path = base + "_closed.csv";
        engines.emplace_back(std::move(c));
    }
    // deploy_ts stays 0 (fresh scratch) -> every on_daily_bar row is fwd -> booked.
    auto ledger = [](const std::string& engine, const std::string& /*sym*/, bool /*is_long*/,
                     double entry_px, double exit_px, double /*lots*/,
                     int64_t entry_ts, int64_t exit_ts, const char* reason) {
        const double gross = (entry_px > 0) ? (exit_px / entry_px - 1.0) : 0.0;
        const double net   = gross - g_rt_bp / 1e4;
        if (engine == "BigCap2pctMimic") g_mtrades.push_back({entry_ts, exit_ts, gross, net, reason});
        else                             g_trades.push_back({entry_ts, exit_ts, gross, net, reason});
    };
    for (auto& e : engines)
        e.set_exec(
            /*open */ [](const std::string&, bool, double, double)->std::string { return "BT"; },
            /*close*/ [](const std::string&, bool, double, double, const std::string&){},
            /*gate */ [](const std::string&, double, double)->bool { return true; },
            /*ledger*/ ledger);

    // ── feed every daily close through the LIVE path (fwd=true, books all) ──
    for (const auto& r : rows)
        for (size_t k = 0; k < engines.size(); ++k)
            if (r.v[k] > 0.0) engines[k].on_daily_bar(r.ts, r.v[k]);

    // ── aggregate the captured clips into the all-6 ──
    const int n = (int)g_trades.size();
    if (n == 0) { std::printf("NO TRADES\n"); return 0; }
    int64_t tmin = g_trades[0].ets, tmax = g_trades[0].ets;
    for (const auto& t : g_trades) { tmin = std::min(tmin, t.ets); tmax = std::max(tmax, t.ets); }
    const int64_t mid = (tmin + tmax) / 2;

    double net_sum=0, gpos=0, gneg=0, worst=1e9;
    double h1=0, h2=0, bull=0, bear=0;
    int wins=0;
    double stall=0, gb90=0, cataS=0; int nstall=0, ngb=0, ncata=0;
    double bull200=0, bear200=0; int nbull200=0, nbear200=0;
    for (const auto& t : g_trades) {
        net_sum += t.net;
        if (t.net > 0) { gpos += t.net; ++wins; } else gneg += -t.net;
        worst = std::min(worst, t.net);
        (t.ets <= mid ? h1 : h2) += t.net;
        (year_of(t.ets) == 2022 ? bear : bull) += t.net;
        { auto it = ts2row.find(t.ets);
          bool bearday = (it != ts2row.end()) && is_bear200[it->second];
          if (bearday) { bear200 += t.net; ++nbear200; } else { bull200 += t.net; ++nbull200; } }
        if      (!std::strcmp(t.reason,"MAX_HOLD"))          { stall += t.net; ++nstall; }
        else if (!std::strcmp(t.reason,"GB90_TRAIL"))        { gb90  += t.net; ++ngb; }
        else if (!std::strcmp(t.reason,"CATASTROPHE_FLOOR")) { cataS += t.net; ++ncata; }
    }
    const double PF = gneg > 1e-12 ? gpos / gneg : (gpos > 0 ? 1e9 : 0.0);

    auto ymd = [](int64_t ts){ time_t tt=(time_t)ts; struct tm g; gmtime_r(&tt,&g); char b[16]; std::strftime(b,sizeof b,"%Y-%m-%d",&g); return std::string(b); };

    std::printf("\n=== BigCap2pctImpulse — FAITHFUL C++ backtest (REAL engine) ===\n");
    std::printf("data: %s   span %s .. %s   names=%zu\n", csv.c_str(), ymd(tmin).c_str(), ymd(tmax).c_str(), want_name.size());
    std::printf("cfg : thr=%.0f%%  new-%dd-high  gb=%.2f(keep %.0f%%)  max_hold=%dd  catastrophe=-%.0f%%  rt=%.0fbp  LONG-only UNGATED\n",
                thr*100, hiw, gb, (1-gb)*100, mh, cata, g_rt_bp);
    std::printf("--------------------------------------------------------------\n");
    std::printf("n (clips)      : %d\n", n);
    std::printf("net%% (of clip notional, real) : %+.1f%%\n", net_sum*100);
    std::printf("PF             : %.2f\n", PF);
    std::printf("win rate       : %.1f%% (%d/%d)\n", 100.0*wins/n, wins, n);
    std::printf("WF H1 (<= mid) : %+.1f%%   WF H2 (> mid) : %+.1f%%   (mid=%s)\n", h1*100, h2*100, ymd(mid).c_str());
    std::printf("bull (ex-2022) : %+.1f%%   bear (2022)   : %+.1f%%   [regime=calendar-2022]\n", bull*100, bear*100);
    std::printf("bull (>200DMA) : %+.1f%% (n=%d)   bear (<200DMA): %+.1f%% (n=%d)   [regime=basket-200DMA]\n",
                bull200*100, nbull200, bear200*100, nbear200);
    std::printf("worst clip     : %+.1f%% (daily-close fill)\n", worst*100);
    std::printf("exit mix       : GB90 n=%d %+.1f%% | MAX_HOLD n=%d %+.1f%% | CATASTROPHE n=%d %+.1f%%\n",
                ngb, gb90*100, nstall, stall*100, ncata, cataS*100);
    std::printf("--------------------------------------------------------------\n");
    // ALL-6 uses the PRINCIPLED market-regime label (basket vs 200DMA) — that is what
    // "both regimes+" means. calendar-2022 is reported above as a stricter stress slice.
    const bool a1 = net_sum>0, a2 = PF>=1.3, a3 = h1>0, a4 = h2>0, a5 = bull200>0, a6 = bear200>0;
    std::printf("ALL-6 : net>0=%d  PF>=1.3=%d  H1>0=%d  H2>0=%d  bull(200DMA)>0=%d  bear(200DMA)>0=%d  => %s\n",
                a1,a2,a3,a4,a5,a6, (a1&&a2&&a3&&a4&&a5&&a6)?"PASS":"FAIL");
    std::printf("        (stricter calendar-2022 entry slice: bear=%+.1f%% — a harsh stress cut, informational)\n", bear*100);
    std::printf("==============================================================\n");

    // ── S-2026-07-13s: MIMIC book STANDALONE (judged on its OWN clips) ──
    if (mbe > 0.0) {
        const int mn = (int)g_mtrades.size();
        std::printf("\n=== BigCap2pctMimic — STANDALONE (be%.1f%%/pend%d, M a2/gb75 + W8 a8/gb50, cap5) ===\n", mbe, mpend);
        if (mn == 0) { std::printf("NO MIMIC TRADES\n"); return 0; }
        double mnet=0, mgp=0, mgn=0, mworst=1e9, mh1=0, mh2=0, mbull=0, mbear=0;
        int mwins=0, mnb=0, mnbr=0;
        for (const auto& t : g_mtrades) {
            mnet += t.net;
            if (t.net > 0) { mgp += t.net; ++mwins; } else mgn += -t.net;
            mworst = std::min(mworst, t.net);
            (t.ets <= mid ? mh1 : mh2) += t.net;
            auto it = ts2row.find(t.ets);
            bool bearday = (it != ts2row.end()) && is_bear200[it->second];
            if (bearday) { mbear += t.net; ++mnbr; } else { mbull += t.net; ++mnb; }
        }
        const double mPF = mgn > 1e-12 ? mgp / mgn : (mgp > 0 ? 1e9 : 0.0);
        std::printf("n=%d  net=%+.1f%%  PF=%.2f  win=%.1f%%  H1=%+.1f%%  H2=%+.1f%%\n",
                    mn, mnet*100, mPF, 100.0*mwins/mn, mh1*100, mh2*100);
        std::printf("bull(>200DMA)=%+.1f%% (n=%d)  bear(<200DMA)=%+.1f%% (n=%d)  worst=%+.1f%%\n",
                    mbull*100, mnb, mbear*100, mnbr, mworst*100);
        const bool m1 = mnet>0, m2 = mPF>=1.3, m3 = mh1>0, m4 = mh2>0, m5 = mbull>0, m6 = mbear>0;
        std::printf("ALL-6 : net>0=%d PF>=1.3=%d H1>0=%d H2>0=%d bull>0=%d bear>0=%d => %s\n",
                    m1,m2,m3,m4,m5,m6, (m1&&m2&&m3&&m4&&m5&&m6)?"PASS":"FAIL");
    }
    return 0;
}
