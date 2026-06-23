#pragma once
// =============================================================================
//  MgcFastDonchianFeed.hpp  (S-2026-06-03)
//  File-poll feed for MgcFastDonchian30m. The tools/mgc_live_bars.py producer
//  (IB Gateway, real MGC 30m TRADES bars) appends closed bars to
//  data/mgc_30m_live.csv and writes prior-day HVN to data/mgc_hvn.json. This
//  polls both and drives the engine — same TRADES-bar source as the backtest,
//  so live == backtest fidelity. Self-contained (no engine_init/globals edits).
//  Owns the engine instance here to avoid bundling with WIP in globals.hpp.
// =============================================================================
#include "MgcFastDonchian30mEngine.hpp"
#include "GoldVolBreakoutM30Engine.hpp"
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static omega::MgcFastDonchian30mEngine g_mgc_fastdon;   // single definition (this TU)
// S-2026-06-23 MGC BOOK: 2nd engine on the same MGC feed -- GoldVolBreakoutM30
// (EMA200-gated Donchian vol-breakout runner). Faithful BT on MGC 30m: PF2.10
// n37 mdd0.78 (selective, orthogonal to the Donchian-runner above). H1 trend is
// aggregated from 30m buckets inside poll_mgc_feed.
static omega::GoldVolBreakoutM30Engine g_mgc_volbrk;

// crude but dependency-free JSON scrape for {"poc":x,"hvn":[a,b,...]}
inline bool _mgc_read_hvn(const std::string& path, double& poc, std::vector<double>& hvn) {
    std::ifstream f(path); if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
    auto num_after = [&](const std::string& key, double& out) -> bool {
        auto p = s.find(key); if (p == std::string::npos) return false;
        p = s.find(':', p); if (p == std::string::npos) return false;
        out = std::atof(s.c_str() + p + 1); return true;
    };
    num_after("\"poc\"", poc);
    auto p = s.find("\"hvn\""); if (p == std::string::npos) return false;
    p = s.find('[', p); auto e = s.find(']', p); if (p == std::string::npos || e == std::string::npos) return false;
    std::string arr = s.substr(p + 1, e - p - 1);
    std::stringstream as(arr); std::string t; hvn.clear();
    while (std::getline(as, t, ',')) { if (!t.empty()) hvn.push_back(std::atof(t.c_str())); }
    return true;
}

// Poll new closed bars + latest HVN, drive the engine. Tracks last-seen ts in a
// static so repeated calls only feed fresh bars. cb routes closed trades (ledger).
inline void poll_mgc_feed(const std::string& bars_csv, const std::string& hvn_json,
                          omega::MgcFastDonchian30mEngine::OnCloseFn cb) {
    if (!g_mgc_fastdon.enabled) return;
    // refresh prior-day HVN (injected; engine won't rebuild from DOM)
    double poc = 0; std::vector<double> hvn;
    if (_mgc_read_hvn(hvn_json, poc, hvn) && !hvn.empty()) g_mgc_fastdon.set_prior_hvn(hvn, poc);

    static int64_t last_ts = 0; static long s_poll = 0; int s_fed = 0; ++s_poll;
    std::ifstream f(bars_csv);
    if (!f) { if (s_poll % 20 == 1) { std::printf("[MGC-FEED] poll#%ld: cannot open '%s' (cwd issue?)\n", s_poll, bars_csv.c_str()); std::fflush(stdout); } return; }
    std::string ln; bool first = true; int64_t newest = 0; int total = 0;
    while (std::getline(f, ln)) {
        if (first) { first = false; if (!ln.empty() && (ln[0] < '0' || ln[0] > '9')) continue; }
        std::stringstream s(ln); std::string t; std::vector<std::string> k;
        while (std::getline(s, t, ',')) k.push_back(t);
        if (k.size() < 6) continue;
        int64_t ts = std::atoll(k[0].c_str());
        ++total; if (ts > newest) newest = ts;
        if (ts <= last_ts) continue;
        last_ts = ts; ++s_fed;
        g_mgc_fastdon.on_30m_bar(std::atof(k[1].c_str()), std::atof(k[2].c_str()),
                                 std::atof(k[3].c_str()), std::atof(k[4].c_str()),
                                 std::atof(k[5].c_str()), ts, cb);

        // --- 2nd MGC engine: GoldVolBreakoutM30 (EMA200-gated Donchian runner) ---
        // Drive on_m30_bar(high,low,close,bid,ask,now_ms,cb) each bar; aggregate H1
        // from 30m buckets and emit on_h1_close on each H1 boundary (the EMA200
        // trend gate). bid=ask=close (MGC shadow; real fills captured live in ledger).
        if (g_mgc_volbrk.enabled) {
            static int64_t vb_h1_bucket = 0; static double vb_h1_close = 0.0;
            const double hi = std::atof(k[2].c_str()), lo = std::atof(k[3].c_str()), cl = std::atof(k[4].c_str());
            const int64_t h1b = (ts / 3600) * 3600;
            if (vb_h1_bucket != 0 && h1b != vb_h1_bucket) g_mgc_volbrk.on_h1_close(vb_h1_close);
            vb_h1_bucket = h1b; vb_h1_close = cl;
            g_mgc_volbrk.on_m30_bar(hi, lo, cl, cl, cl, ts, cb);
        }
    }
    // HEARTBEAT: proves the poll is reading the live MGC feed. Logs on any new
    // bars, else every 20th poll (~10min). newest_ts confirms freshness.
    if (s_fed > 0 || s_poll % 20 == 1) {
        std::printf("[MGC-FEED] poll#%ld: file_bars=%d new_fed=%d newest_ts=%lld (mgc book alive; 0 trades is correct in a gold downtrend -- long-biased)\n",
                    s_poll, total, s_fed, (long long)newest);
        std::fflush(stdout);
    }
}
