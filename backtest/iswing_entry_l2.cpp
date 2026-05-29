// =====================================================================
// backtest/iswing_entry_l2.cpp -- L2 entry-quality analysis on 10-trade sample
//
// For each trade: read l2_imb and micro_edge at +/- 30 ticks of entry.
// Tag trade as L2_ALIGNED (imb/micro agree with side) or L2_OPPOSED.
// Show: does L2 filter separate winners from losers?
// =====================================================================
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

struct Tick {
    long long ts_ms = 0;
    double mid = 0, bid = 0, ask = 0;
    double l2_imb = 0.5;   // col 5
    double micro = 0;       // col 16 (micro_edge)
};

static std::vector<Tick> load_ticks(const std::string& path) {
    std::vector<Tick> out;
    out.reserve(2'000'000);
    std::ifstream f(path);
    if (!f) return out;
    std::string line; std::getline(f, line);  // header
    while (std::getline(f, line)) {
        const char* s = line.c_str();
        Tick t;
        t.ts_ms = atoll(s);
        int col = 0;
        const char* p = s;
        while (*p) {
            if (col == 1) t.mid = atof(p);
            else if (col == 2) t.bid = atof(p);
            else if (col == 3) t.ask = atof(p);
            else if (col == 4) t.l2_imb = atof(p);
            else if (col == 15) { t.micro = atof(p); break; }
            const char* nx = strchr(p, ','); if (!nx) break;
            p = nx + 1; col++;
        }
        if (t.bid <= 0 || t.ask <= 0) continue;
        out.push_back(t);
    }
    return out;
}

static size_t find_entry(const std::vector<Tick>& tks, double entry_px,
                         int hh, int mm, int ss, double tol) {
    long long best = -1;
    double best_score = 1e18;
    int tod_t = hh * 3600 + mm * 60 + ss;
    for (size_t i = 0; i < tks.size(); ++i) {
        time_t t = tks[i].ts_ms / 1000;
        struct tm tm{}; gmtime_r(&t, &tm);
        int tod = tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
        int dt = std::abs(tod - tod_t);
        if (dt > 60) continue;
        double dp = std::abs(tks[i].mid - entry_px);
        if (dp > tol) continue;
        double sc = dp + dt * 0.01;
        if (sc < best_score) { best_score = sc; best = (long long)i; }
    }
    return (best < 0) ? SIZE_MAX : (size_t)best;
}

struct Tr {
    const char* tag;     // operator entry time string (UTC-shifted)
    const char* sym;
    int hh, mm, ss;
    bool is_long;
    double entry_px;
    double old_pnl;
};

int main() {
    auto sp  = load_ticks("/tmp/replay/us500_merged.csv");
    auto nq  = load_ticks("/tmp/replay/ustec_merged.csv");
    auto xau = load_ticks("/tmp/replay/l2_ticks_XAUUSD_2026-05-29.csv");
    auto xau28 = load_ticks("/tmp/replay/l2_ticks_XAUUSD_2026-05-28.csv");
    xau.insert(xau.end(), xau28.begin(), xau28.end());
    std::sort(sp.begin(), sp.end(),  [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    std::sort(nq.begin(), nq.end(),  [](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    std::sort(xau.begin(), xau.end(),[](const Tick& a, const Tick& b){ return a.ts_ms < b.ts_ms; });
    fprintf(stderr, "loaded SP=%zu NQ=%zu XAU=%zu\n", sp.size(), nq.size(), xau.size());

    // True UTC entry times (derived earlier from price-match).
    Tr trades[] = {
        // tag         sym       hh mm ss long? entry_px   old_pnl
        { "17:12:33",  "US500.F", 17,12,33, true, 7598.41,  -200.00 },
        { "15:25:02",  "US500.F", 15,25, 2, true, 7599.53,  +128.00 },
        { "17:50:00",  "USTEC.F", 17,50, 0, true, 30339.85, -100.00 },
        { "14:16:36",  "US500.F", 14,16,36, true, 7600.53,  -200.00 },
        { "11:53:44",  "US500.F", 11,53,44, true, 7590.53,  +346.50 },
        { "15:25:53",  "USTEC.F", 15,25,53, true, 30252.15, -100.00 },
        { "15:32:14",  "USTEC.F", 15,32,14, true, 30277.00, -100.00 },
        { "10:11:26",  "US500.F", 10,11,26, true, 7587.03,  +130.88 },
        { "15:05:00",  "USTEC.F", 15, 5, 0, true, 30237.50, +80.00 },
        { "15:05:00",  "XAUUSD",  15, 5, 0, true, 4579.93,    -5.28 },  // DonchianBreakoutSL
    };

    printf("%-9s %-7s %-8s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %s\n",
           "time", "sym", "entry", "old_$",
           "imb@e", "imb_p", "imb_n",
           "mic@e", "mic_p", "mic_n",
           "verdict");
    printf("%-9s %-7s %-8s %-7s %-7s %-7s %-7s %-7s %-7s %-7s %s\n",
           "----", "---", "-----", "-----",
           "-----", "-----", "-----",
           "-----", "-----", "-----",
           "-------");

    int aligned_wins = 0, aligned_losses = 0;
    int opposed_wins = 0, opposed_losses = 0;

    for (auto& tr : trades) {
        const std::vector<Tick>* tks;
        double tol;
        if (strcmp(tr.sym, "US500.F") == 0) { tks = &sp;  tol = 1.5; }
        else if (strcmp(tr.sym, "USTEC.F") == 0) { tks = &nq;  tol = 5.0; }
        else { tks = &xau; tol = 0.5; }
        size_t i = find_entry(*tks, tr.entry_px, tr.hh, tr.mm, tr.ss, tol);
        if (i == SIZE_MAX) {
            printf("%-9s %-7s %-8.2f NO_MATCH\n", tr.tag, tr.sym, tr.entry_px);
            continue;
        }
        // L2 at entry + avg over prior 30 ticks + post 30 ticks
        double imb_e = (*tks)[i].l2_imb;
        double mic_e = (*tks)[i].micro;
        double imb_p = 0, imb_n = 0, mic_p = 0, mic_n = 0;
        int cp = 0, cn = 0;
        for (size_t j = (i >= 30 ? i - 30 : 0); j < i; ++j) {
            imb_p += (*tks)[j].l2_imb;
            mic_p += (*tks)[j].micro;
            cp++;
        }
        for (size_t j = i + 1; j < std::min(i + 31, (*tks).size()); ++j) {
            imb_n += (*tks)[j].l2_imb;
            mic_n += (*tks)[j].micro;
            cn++;
        }
        imb_p = cp ? imb_p / cp : 0.5;
        imb_n = cn ? imb_n / cn : 0.5;
        mic_p = cp ? mic_p / cp : 0;
        mic_n = cn ? mic_n / cn : 0;
        // Alignment: long trade should have imb_p > 0.5 OR micro_p > 0.
        // We use available signal per symbol.
        bool imb_usable = std::abs(imb_p - 0.5) > 0.01;  // not stuck at 0.5
        bool mic_usable = std::abs(mic_p) > 0.001;
        bool aligned = false, opposed = false;
        if (mic_usable) {
            bool m_long_ok = mic_p > 0;
            aligned = aligned || m_long_ok;
            opposed = opposed || !m_long_ok;
        }
        if (imb_usable) {
            bool i_long_ok = imb_p > 0.5;
            aligned = aligned || i_long_ok;
            opposed = opposed || !i_long_ok;
        }
        const char* verdict;
        if (!mic_usable && !imb_usable) verdict = "NO_L2";
        else if (aligned && !opposed) verdict = "ALIGNED";
        else if (opposed && !aligned) verdict = "OPPOSED";
        else verdict = "MIXED";
        printf("%-9s %-7s %-8.2f %+7.2f %7.4f %7.4f %7.4f %+7.4f %+7.4f %+7.4f %s\n",
               tr.tag, tr.sym, tr.entry_px, tr.old_pnl,
               imb_e, imb_p, imb_n,
               mic_e, mic_p, mic_n,
               verdict);
        bool is_win = tr.old_pnl > 0;
        if (strcmp(verdict, "ALIGNED") == 0) { if (is_win) ++aligned_wins; else ++aligned_losses; }
        if (strcmp(verdict, "OPPOSED") == 0) { if (is_win) ++opposed_wins; else ++opposed_losses; }
    }
    int al_n = aligned_wins + aligned_losses;
    int op_n = opposed_wins + opposed_losses;
    printf("\nALIGNED: n=%d W=%d L=%d WR=%.0f%%\n", al_n, aligned_wins, aligned_losses,
           al_n ? 100.0 * aligned_wins / al_n : 0);
    printf("OPPOSED: n=%d W=%d L=%d WR=%.0f%%\n", op_n, opposed_wins, opposed_losses,
           op_n ? 100.0 * opposed_wins / op_n : 0);
    return 0;
}
