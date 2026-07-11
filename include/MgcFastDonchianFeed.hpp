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
#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <vector>

static omega::MgcFastDonchian30mEngine g_mgc_fastdon;   // single definition (this TU)
// S-2026-06-23 MGC BOOK: 2nd engine on the same MGC feed -- GoldVolBreakoutM30
// (EMA200-gated Donchian vol-breakout runner). Faithful BT on MGC 30m: PF2.10
// n37 mdd0.78 (selective, orthogonal to the Donchian-runner above). H1 trend is
// aggregated from 30m buckets inside poll_mgc_feed.
// S-2026-07-11 PHASE 1b: g_mgc_volbrk DECLARATION MOVED to globals.hpp (before
// PositionPersistence.hpp in main.cpp include order) so its open MGC leg
// persists across restart -- same include-order fix as MgcTF4h/2h + MgcSlowDon.
// This header only DRIVES it. Standalone harnesses get a local instance below.
// S-2026-07-07 MGC VENUE PORT: 3rd + 4th engines on the same MGC feed -- the
// validated gold trend family (XauTrendFollow4h/2h classes, production spot
// config) re-instanced on MGC futures prices + costs. Faithful venue BT
// (backtest/XauTrendFollow4h2hBacktest.cpp, MGC=1, real MGC H4/H1 bars
// 2024-06..2026-06, spread 0.10 + $0.208/oz RT comm): 4h PF1.54 +$4404
// maxDD $1331 both-halves+, 2h PF1.21 +$4281 both-halves+; 2x-cost PASS both
// (cost-insensitive); 2022-23 bear at MGC cost: 4h +$650 PF1.22 / 2h flat --
// same bull-positive bear-flat profile as the spot family. SHADOW.
// H1/H4 buckets are aggregated from the 30m feed inside poll_mgc_feed;
// warm-seed = engine warmup_csv_path (data/mgc_h1_hist.csv / mgc_h4_hist.csv),
// configured in omega_main.hpp next to the volbrk block.
// S-2026-07-08c: g_mgc_tf_4h / g_mgc_tf_2h / g_mgc_tf_floor_ts declarations MOVED
// to globals.hpp (before PositionPersistence.hpp in main.cpp include order) so the
// MGC TF instances are persistence-registered. This header only DRIVES them.
// Standalone harnesses (backtest/mgc_tf_feed_parity.cpp) that include this header
// WITHOUT globals.hpp define MGC_FEED_STANDALONE to get local instances:
#ifdef MGC_FEED_STANDALONE
static omega::XauTrendFollow4hEngine g_mgc_tf_4h;
static omega::XauTrendFollow2hEngine g_mgc_tf_2h;
static int64_t g_mgc_tf_floor_ts = 0;
#include "MgcSlowDonchian30mEngine.hpp"
static omega::MgcSlowDonchian30mEngine g_mgc_slowdon;
static omega::GoldVolBreakoutM30Engine g_mgc_volbrk;   // S-2026-07-11: moved to globals.hpp for persistence
#endif
// S-2026-07-08c: 5th engine on the same MGC feed -- MgcSlowDonchian30m (deep-dive
// candidate #1, Nin40/Nout20 slow sibling, next-bar-open + 3xATR adverse-first
// stop, dedup vs g_mgc_fastdon). Instance lives in globals.hpp (persistence
// include-order); config in omega_main.hpp. This header only DRIVES it.

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

// Latest live MGC L2 imbalance (book bid-share, 0..1; >0.5 bid-heavy). Reads the
// last row of today's logs/ibkr_l2/ibkr_l2_MGC_<UTCdate>.csv (col4=l2_imb, written
// by the L2 recorder). Fail-OPEN (0.5 = neutral) on any error so a missing/stale
// L2 file never blocks entries. Cheap: seeks the file tail, not a full read.
inline double _mgc_latest_l2_imb() {
    std::time_t now = std::time(nullptr); std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &now);
#else
    gmtime_r(&now, &g);
#endif
    char path[256];
    std::snprintf(path, sizeof(path), "logs/ibkr_l2/ibkr_l2_MGC_%04d-%02d-%02d.csv",
                  g.tm_year + 1900, g.tm_mon + 1, g.tm_mday);
    std::ifstream f(path, std::ios::binary); if (!f) return 0.5;
    f.seekg(0, std::ios::end); std::streamoff sz = f.tellg();
    if (sz <= 2) return 0.5;
    std::streamoff back = sz > 3000 ? 3000 : sz; f.seekg(-back, std::ios::end);
    std::string chunk((size_t)back, '\0'); f.read(&chunk[0], back);
    std::size_t e = chunk.find_last_not_of("\r\n"); if (e == std::string::npos) return 0.5;
    std::size_t s = chunk.find_last_of('\n', e);
    std::string line = chunk.substr(s == std::string::npos ? 0 : s + 1);
    int comma = 0; const char* p = line.c_str();
    for (; *p; ++p) if (*p == ',' && ++comma == 4) { double v = std::atof(p + 1); return (v > 0.0 && v < 1.0) ? v : 0.5; }
    return 0.5;
}

// Poll new closed bars + latest HVN, drive the engine. Tracks last-seen ts in a
// static so repeated calls only feed fresh bars. cb routes closed trades (ledger).
inline void poll_mgc_feed(const std::string& bars_csv, const std::string& hvn_json,
                          omega::MgcFastDonchian30mEngine::OnCloseFn cb) {
    if (!g_mgc_fastdon.enabled) return;
    // refresh prior-day HVN (injected; engine won't rebuild from DOM)
    double poc = 0; std::vector<double> hvn;
    if (_mgc_read_hvn(hvn_json, poc, hvn) && !hvn.empty()) g_mgc_fastdon.set_prior_hvn(hvn, poc);
    // push the live MGC L2 imbalance to both engines (their L2 confirmation gate).
    double mgc_imb = _mgc_latest_l2_imb();
    g_mgc_fastdon.set_l2_imb(mgc_imb);
    g_mgc_volbrk.set_l2_imb(mgc_imb);

    static int64_t last_ts = 0; static long s_poll = 0; int s_fed = 0; ++s_poll;
    // S-2026-07-08c BOOT-REPLAY GUARD: the FIRST poll after a restart re-reads the
    // whole live 30m CSV (last_ts starts 0). Bars above g_mgc_tf_floor_ts replay
    // into the TF engines -- load-bearing for indicator continuity (the warmup CSVs
    // only cover <= floor), but WITHOUT this guard the replay also re-fired entries,
    // re-booking round-trips already in the shadow ledger from before the restart.
    // warmup_active_ = the engines' own S102 seed flag: indicators + position
    // management run, _fire_entry is blocked. Open legs come back via
    // PositionPersistence (MgcTF4h/2h registered same commit) and get managed
    // through the replayed bars at their historical prices. Cleared after the
    // first poll completes -> live bars behave exactly as before.
    const bool tf_boot_replay = (s_poll == 1);
    if (tf_boot_replay) { g_mgc_tf_4h.warmup_active_ = true; g_mgc_tf_2h.warmup_active_ = true;
                          g_mgc_slowdon.warmup_active_ = true; }   // S-2026-07-08c: same guard (entries blocked, restored pos managed)
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

        // --- 5th MGC engine: MgcSlowDonchian30m (S-2026-07-08c, deep-dive #1) ---
        // Internal ts-dedup skips rows already covered by its data/mgc_30m_hist.csv
        // warm-seed; warmup_active_ (set on the first poll above) blocks boot-replay
        // entries while restored positions are managed through the replayed bars.
        if (g_mgc_slowdon.enabled)
            g_mgc_slowdon.on_30m_bar(std::atof(k[1].c_str()), std::atof(k[2].c_str()),
                                     std::atof(k[3].c_str()), std::atof(k[4].c_str()),
                                     ts, cb);

        // --- 2nd MGC engine: GoldVolBreakoutM30 (EMA200-gated Donchian runner) ---
        // Drive on_m30_bar(high,low,close,bid,ask,now_ms,cb,open) each bar; aggregate
        // H1 from 30m buckets and emit on_h1_close on each H1 boundary (the EMA200
        // trend gate). bid=ask=close (MGC shadow; real fills captured live in ledger).
        // S-2026-07-11 PHASE 1b FIXES (roadmap #6):
        //  * ts*1000 -- this call previously passed SECONDS into now_ms, so the
        //    engine's London/NY session gate (load-bearing: sess-off PF 0.76)
        //    computed a garbage hour that drifted through the day-cycle over
        //    ~2.7 YEARS, and ledger entry/exit timestamps were 1970-scale.
        //  * open passed -- feeds the gap-honest resting-stop fill (stop_mode=2,
        //    the decision-test winner; see mgc_volbrk_tickstop_decision.cpp).
        if (g_mgc_volbrk.enabled) {
            static int64_t vb_h1_bucket = 0; static double vb_h1_close = 0.0;
            const double op = std::atof(k[1].c_str()), hi = std::atof(k[2].c_str()),
                         lo = std::atof(k[3].c_str()), cl = std::atof(k[4].c_str());
            const int64_t h1b = (ts / 3600) * 3600;
            if (vb_h1_bucket != 0 && h1b != vb_h1_bucket) g_mgc_volbrk.on_h1_close(vb_h1_close);
            vb_h1_bucket = h1b; vb_h1_close = cl;
            g_mgc_volbrk.on_m30_bar(hi, lo, cl, cl, cl, ts * 1000LL, cb, op);
        }

        // --- 3rd/4th MGC engines: XauTrendFollow4h/2h venue instances
        //     (S-2026-07-07 MGC port). H1 + H4 buckets aggregated from the 30m
        //     stream; per-30m on_tick(l,h,c) gives intrabar SL/manage fidelity
        //     (finer than the harness's per-H4 l/h/c drive, same SL-first
        //     order). Bars at/below g_mgc_tf_floor_ts are covered by the
        //     warmup CSVs (data/mgc_h1_hist.csv / mgc_h4_hist.csv, regenerated
        //     at deploy) and skipped so boot replay of the live CSV neither
        //     double-feeds indicators nor books stale entries.
        if ((g_mgc_tf_4h.enabled || g_mgc_tf_2h.enabled) && ts > g_mgc_tf_floor_ts) {
            const double sprd = 0.10;   // MGC 1 exchange tick
            const double hi = std::atof(k[2].c_str()), lo = std::atof(k[3].c_str()),
                         op = std::atof(k[1].c_str()), cl = std::atof(k[4].c_str());
            const int64_t ts_ms = ts * 1000LL;
            // intrabar manage: low -> high -> close (SL-first, harness order)
            g_mgc_tf_4h.on_tick(lo, lo + sprd, ts_ms, cb);
            g_mgc_tf_4h.on_tick(hi, hi + sprd, ts_ms, cb);
            g_mgc_tf_4h.on_tick(cl, cl + sprd, ts_ms, cb);
            g_mgc_tf_2h.on_tick(lo, lo + sprd, ts_ms, cb);
            g_mgc_tf_2h.on_tick(hi, hi + sprd, ts_ms, cb);
            g_mgc_tf_2h.on_tick(cl, cl + sprd, ts_ms, cb);
            // H1 bucket -> 2h engine (it builds 2h internally from H1 bars)
            static int64_t tf_h1_b = 0; static double h1o=0, h1h=0, h1l=0, h1c=0;
            const int64_t h1b2 = (ts / 3600) * 3600;
            if (tf_h1_b != 0 && h1b2 != tf_h1_b) {
                omega::XauTf2hBar b1{}; b1.bar_start_ms = tf_h1_b * 1000LL;
                b1.open = h1o; b1.high = h1h; b1.low = h1l; b1.close = h1c;
                g_mgc_tf_2h.on_h1_bar(b1, h1c, h1c + sprd, ts_ms, cb);
            }
            if (h1b2 != tf_h1_b) { tf_h1_b = h1b2; h1o = op; h1h = hi; h1l = lo; h1c = cl; }
            else { if (hi > h1h) h1h = hi; if (lo < h1l) h1l = lo; h1c = cl; }
            // H4 bucket -> 4h engine
            static int64_t tf_h4_b = 0; static double h4o=0, h4h=0, h4l=0, h4c=0;
            const int64_t h4b = (ts / 14400) * 14400;
            if (tf_h4_b != 0 && h4b != tf_h4_b) {
                omega::XauTfBar b4{}; b4.bar_start_ms = tf_h4_b * 1000LL;
                b4.open = h4o; b4.high = h4h; b4.low = h4l; b4.close = h4c;
                g_mgc_tf_4h.on_h4_bar(b4, h4c, h4c + sprd, 0.0, ts_ms, cb);
            }
            if (h4b != tf_h4_b) { tf_h4_b = h4b; h4o = op; h4h = hi; h4l = lo; h4c = cl; }
            else { if (hi > h4h) h4h = hi; if (lo < h4l) h4l = lo; h4c = cl; }
        }
    }
    // S-2026-07-08c: boot replay done -> re-arm live entries (see guard above).
    if (tf_boot_replay) {
        g_mgc_tf_4h.warmup_active_ = false; g_mgc_tf_2h.warmup_active_ = false;
        g_mgc_slowdon.warmup_active_ = false;
        std::printf("[MGC-FEED] boot replay complete: %d bar(s) re-fed entry-blocked; live entries armed\n", s_fed);
        std::fflush(stdout);
    }
#ifndef MGC_FEED_STANDALONE
    // Liveness pulse for the slow Donchian book (registered in omega_main).
    g_engine_heartbeat.pulse("MgcSlowDonchian30m");
#endif
    // HEARTBEAT: proves the poll is reading the live MGC feed. Logs on any new
    // bars, else every 20th poll (~10min). newest_ts confirms freshness.
    if (s_fed > 0 || s_poll % 20 == 1) {
        std::printf("[MGC-FEED] poll#%ld: file_bars=%d new_fed=%d newest_ts=%lld (mgc book alive; 0 trades is correct in a gold downtrend -- long-biased)\n",
                    s_poll, total, s_fed, (long long)newest);
        std::fflush(stdout);
    }
}
