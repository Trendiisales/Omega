// =============================================================================
// xau_session_bias_audit.cpp -- session-of-day return analysis on 2yr XAU tape.
//
// Hypothesis (research agent task #20):
//   Asia (22:00-06:00 UTC) drifts positive in bull regimes; London (07:00-10:00
//   UTC) tends to fade NY high; NY/London overlap (12:00-17:00 UTC) is the
//   volatility window. Operator wants empirical net-of-cost confirmation before
//   coding a full engine.
//
// Method:
//   1. Walk DukasCopy XAU tick CSV. Bucket each tick by UTC hour.
//   2. Build per-day session OHLC for {Asia, London, Overlap, NY, Late_NY}.
//   3. For each session-day, record session-close - session-open price move.
//   4. Tag each day's regime by 20-day rolling D1 close trend (bull/bear/flat).
//   5. Report mean / hit-rate / Sharpe per (session, regime) cell.
//
// Costs:
//   Apply 0.30 round-trip on entries (BlackBull XAU live spread).
//
// Build:
//   cmake --build build --target xau_session_bias_audit -j
// Run:
//   ./build/xau_session_bias_audit /Users/jo/Tick/2yr_XAUUSD_tick_fresh.csv
// =============================================================================

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <vector>
#include <cmath>
#include <chrono>

struct Tick { int64_t ts_ms; double bid; double ask; };

static bool parse_duka_row(const char* line, Tick& out) {
    int Y, M, D, h, m, s; double bid, ask;
    if (std::sscanf(line, "%4d%2d%2d,%d:%d:%d,%lf,%lf",
                    &Y, &M, &D, &h, &m, &s, &bid, &ask) != 8) return false;
    if (bid <= 0.0 || ask <= 0.0 || ask < bid) return false;
    std::tm tm{};
    tm.tm_year = Y - 1900; tm.tm_mon = M - 1; tm.tm_mday = D;
    tm.tm_hour = h;        tm.tm_min  = m;    tm.tm_sec  = s;
    const time_t epoch_s = timegm(&tm);
    if (epoch_s <= 0) return false;
    out.ts_ms = static_cast<int64_t>(epoch_s) * 1000LL;
    out.bid = bid; out.ask = ask;
    return true;
}

enum Session { SESS_ASIA = 0, SESS_LONDON = 1, SESS_OVERLAP = 2,
               SESS_NY = 3, SESS_LATE = 4, SESS_DEAD = 5 };
static const char* SESS_NAME[6] = {"Asia", "London", "Overlap", "NY", "Late_NY", "DEAD"};

static Session session_of_utc_hour(int hr) {
    if (hr >= 22 || hr < 6)  return SESS_ASIA;     // 22:00-06:00
    if (hr >= 7  && hr < 10) return SESS_LONDON;   // 07:00-10:00
    if (hr >= 10 && hr < 12) return SESS_OVERLAP;  // 10:00-12:00
    if (hr >= 12 && hr < 17) return SESS_NY;       // 12:00-17:00
    if (hr >= 17 && hr < 22) return SESS_LATE;     // 17:00-22:00
    return SESS_DEAD;
}

struct SessionDay {
    int64_t day_utc       = 0;
    Session sess          = SESS_DEAD;
    double  open_mid      = 0.0;
    double  close_mid     = 0.0;
    double  spread_open   = 0.0;
    bool    active        = false;
};

struct CellStats {
    int    n         = 0;
    int    n_pos     = 0;
    double sum       = 0.0;
    double sum_sq    = 0.0;
    double worst     = 0.0;
    double best      = 0.0;
    void add(double r) {
        ++n;
        if (r > 0) ++n_pos;
        sum    += r;
        sum_sq += r * r;
        if (r < worst) worst = r;
        if (r > best)  best  = r;
    }
    double mean() const { return n ? sum / n : 0.0; }
    double std()  const {
        if (n < 2) return 0.0;
        const double m = mean();
        const double v = (sum_sq - n * m * m) / (n - 1);
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    double wr() const { return n ? 100.0 * n_pos / n : 0.0; }
    // Sharpe approximation: mean / std * sqrt(252 / sessions_per_year).
    // ~1 sample per trading day per session = 252/yr.
    double sharpe_ann() const {
        const double s = std();
        return (s > 1e-9) ? (mean() / s) * std::sqrt(252.0) : 0.0;
    }
};

// Bull / Bear / Flat regime from 20-day rolling D1 close trend.
// 20d log-return > +5% = bull, < -5% = bear, otherwise flat.
enum Regime { REG_BULL = 0, REG_FLAT = 1, REG_BEAR = 2 };
static const char* REG_NAME[3] = {"Bull", "Flat", "Bear"};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <xau_tick_csv> [report_file]\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];
    const std::string report_path = (argc >= 3) ? argv[2]
        : std::string("/tmp/xau_session_bias_audit.txt");
    FILE* RPT = std::fopen(report_path.c_str(), "w");
    if (!RPT) { std::fprintf(stderr, "ERROR: cannot open %s\n", report_path.c_str()); return 3; }
    std::fprintf(stderr, "[SBA] report -> %s\n", report_path.c_str());

    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) { std::fprintf(stderr, "ERROR: cannot open %s\n", path.c_str()); return 2; }

    // Cost model: round-trip spread, treated as flat per-session deduction.
    const double RT_COST = 0.30;  // BlackBull XAU live RT slip estimate

    // Walk ticks. Hold current per-session accumulators.
    SessionDay cur{};
    int64_t cur_day = 0;
    int     cur_hr  = -1;
    Session cur_sess = SESS_DEAD;

    // D1 close history for regime tagging
    std::map<int64_t, double> d1_close;     // day_utc -> last_mid_of_day
    double last_mid_today = 0.0;
    int64_t last_day_seen = 0;

    // Per (session, regime) cells
    CellStats cells[6][3];

    // Per-day per-session moves (for joint analysis)
    struct SessionMove { int64_t day; Session sess; double r; Regime reg; };
    std::vector<SessionMove> moves;
    moves.reserve(200000);

    char line[1024];
    int64_t lines = 0, parsed = 0;
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int64_t PROGRESS_EVERY = 10'000'000;

    while (std::fgets(line, sizeof(line), f)) {
        ++lines;
        Tick t;
        if (!parse_duka_row(line, t)) continue;
        ++parsed;
        const time_t t_s = static_cast<time_t>(t.ts_ms / 1000);
        std::tm tm{}; gmtime_r(&t_s, &tm);
        const int64_t day_utc = t.ts_ms / 86400000LL;
        const int hr = tm.tm_hour;
        const Session sess = session_of_utc_hour(hr);
        const double mid = (t.bid + t.ask) * 0.5;

        // Track last mid of the day for D1 close.
        if (day_utc != last_day_seen) {
            if (last_day_seen > 0 && last_mid_today > 0.0) {
                d1_close[last_day_seen] = last_mid_today;
            }
            last_day_seen = day_utc;
        }
        last_mid_today = mid;

        // Session bucket switch -- close prior bucket, open new
        if (sess != cur_sess || day_utc != cur_day) {
            // Close prior bucket
            if (cur.active && cur.sess != SESS_DEAD && cur.open_mid > 0.0) {
                cur.close_mid = mid;  // close at first tick of new bucket
                const double gross = cur.close_mid - cur.open_mid;
                const double net   = gross - RT_COST;
                // Regime: 20d D1 trend ending at (cur.day_utc - 1)
                Regime reg = REG_FLAT;
                auto it_today = d1_close.find(cur.day_utc - 1);
                auto it_prior = d1_close.find(cur.day_utc - 21);
                if (it_today != d1_close.end() && it_prior != d1_close.end()) {
                    const double r20 = std::log(it_today->second / it_prior->second);
                    if      (r20 >  0.05) reg = REG_BULL;
                    else if (r20 < -0.05) reg = REG_BEAR;
                    else                  reg = REG_FLAT;
                }
                cells[cur.sess][reg].add(net);
                moves.push_back({cur.day_utc, cur.sess, net, reg});
            }
            // Open new bucket
            cur.active     = true;
            cur.day_utc    = day_utc;
            cur.sess       = sess;
            cur.open_mid   = mid;
            cur.spread_open= t.ask - t.bid;
            cur_day        = day_utc;
            cur_sess       = sess;
        }
        cur_hr = hr;
        (void)cur_hr;  // silence unused warning if we add no per-hour stats later

        if (lines % PROGRESS_EVERY == 0) {
            const auto wall = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            std::fprintf(stderr, "\r[SBA] %lldM lines  %.1fs wall  rate=%.1fM/s  ",
                         (long long)(lines / 1'000'000), wall, (lines / 1e6) / wall);
        }
    }
    std::fclose(f);
    std::fprintf(stderr, "\n[SBA] done. %lld lines, %lld parsed, %zu moves\n",
                 (long long)lines, (long long)parsed, moves.size());

    // -----------------------------------------------------------------------
    // Report
    // -----------------------------------------------------------------------
    std::fprintf(RPT, "==============================================================================\n");
    std::fprintf(RPT, "  xau_session_bias_audit  --  %s\n", path.c_str());
    std::fprintf(RPT, "  corpus: %lld lines parsed, %zu session-moves, RT cost = $%.2f\n",
                 (long long)parsed, moves.size(), RT_COST);
    std::fprintf(RPT, "  regime: 20d D1 trend  bull >+5%%  flat in band  bear <-5%%\n");
    std::fprintf(RPT, "==============================================================================\n\n");

    std::fprintf(RPT, "PER-SESSION x REGIME (net of $%.2f RT cost):\n\n", RT_COST);
    std::fprintf(RPT, "  %-8s %-6s  %5s  %6s  %8s  %7s  %7s  %7s  %7s\n",
                 "Session", "Regime", "n", "WR%", "mean$", "std$", "Sharpe", "worst$", "best$");
    std::fprintf(RPT, "  %s\n", std::string(85, '-').c_str());
    for (int s = 0; s < 5; ++s) {
        for (int r = 0; r < 3; ++r) {
            const CellStats& c = cells[s][r];
            if (c.n == 0) continue;
            std::fprintf(RPT, "  %-8s %-6s  %5d  %6.2f  %+8.3f  %7.3f  %+7.3f  %+7.3f  %+7.3f\n",
                         SESS_NAME[s], REG_NAME[r], c.n, c.wr(),
                         c.mean(), c.std(), c.sharpe_ann(), c.worst, c.best);
        }
    }

    // Aggregated per-session (across all regimes)
    std::fprintf(RPT, "\nAGGREGATED PER-SESSION (all regimes pooled):\n\n");
    std::fprintf(RPT, "  %-8s  %5s  %6s  %8s  %7s  %7s\n",
                 "Session", "n", "WR%", "mean$", "std$", "Sharpe");
    std::fprintf(RPT, "  %s\n", std::string(56, '-').c_str());
    for (int s = 0; s < 5; ++s) {
        CellStats agg;
        for (int r = 0; r < 3; ++r) {
            const CellStats& c = cells[s][r];
            agg.n      += c.n;
            agg.n_pos  += c.n_pos;
            agg.sum    += c.sum;
            agg.sum_sq += c.sum_sq;
            if (c.worst < agg.worst) agg.worst = c.worst;
            if (c.best  > agg.best)  agg.best  = c.best;
        }
        if (agg.n == 0) continue;
        std::fprintf(RPT, "  %-8s  %5d  %6.2f  %+8.3f  %7.3f  %+7.3f\n",
                     SESS_NAME[s], agg.n, agg.wr(),
                     agg.mean(), agg.std(), agg.sharpe_ann());
    }

    // Verdict guide
    std::fprintf(RPT, "\nVERDICT GUIDE:\n");
    std::fprintf(RPT, "  Sharpe > +0.5 with n > 100 = potentially codeable bias\n");
    std::fprintf(RPT, "  WR > 55%% AND mean > $0.50 = positive expectancy net of cost\n");
    std::fprintf(RPT, "  Negative Sharpe with high n = directional fade trade\n");

    std::fclose(RPT);
    std::fprintf(stderr, "[SBA] report written to %s\n", report_path.c_str());
    return 0;
}
