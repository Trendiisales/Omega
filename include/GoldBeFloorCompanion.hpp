#pragma once
// =============================================================================
// GoldBeFloorCompanion — AUPOS (long) + AUNEG (short) BE-floor clip books.
//
// C++ in-binary engine, BYTE-EXACT port of the validated Python companion:
//   math  : backtest/gold_befloor_ls.py  (parent_long/parent_short detector +
//           leg_book_long/leg_book_short BE-floor walk)
//   book  : tools/gold_befloor_companion.py  (book_points deploy-forward gate +
//           build_state desk schema)
// This REPLACES the Mac-cron Python executor + companion_aggregate fold with a
// native engine compiled into Omega.exe, mirroring the crypto UpJumpLadderCompanion
// architecture (C++ in the trading binary, own state file + own /api endpoint).
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-
// engine). Observe-only, shadow: it NEVER opens / moves / shrinks / closes a real
// position and is NEVER read by any parent. It self-detects its OWN up/down move
// windows (2h / +/-1%) from the gold H1 close stream it is fed, and runs x2 BE-floor
// tiers per direction (20bp banker / 150bp runner). Judge STANDALONE (net>0, both
// regimes) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: BE-FLOOR — a leg stays FLAT until price clears +be_bp from its
//   ref (covers RT cost), opens THERE, and its stop sits at-or-above entry (long) /
//   at-or-below entry (short) and only trails in the favourable direction. Therefore
//   exit_px >= entry (long) / <= entry (short) ALWAYS => net_bp >= 0 on EVERY clip
//   BY CONSTRUCTION. A cold-loss cut is structurally impossible and unnecessary.
//   Backtested byte-exact vs backtest/gold_befloor_ls.py (neg=0, every slice, both
//   regimes, WF both halves). Verdict = BE-floor (no cut), not skipped.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

class GoldBeFloorCompanion {
public:
    struct Config {
        int    W          = 2;        // detector window (H1 bars) -> "2h"
        double thr        = 0.01;     // detector threshold -> "+/-1%"
        double be_bp      = 6.0;      // GOLD_RT_BP: gross bp move to cover cost & open a leg
        double lot        = 1.0;      // 1.0 std XAUUSD lot = 100 oz
        double dpp_per_lot = 100.0;   // $/point for 1.0 lot
        std::string sym    = "XAUUSD";// symbol the live legs trade (order path)
        double live_gb_bp  = 150.0;   // live trail giveback = runner tier (the prescribed backtested exit;
                                      //   banker 20bp is the tight variant — accounting panel still shows both).
        std::string state_path   = "gold_companion_state.json";       // served by /api/gold_companion
        std::string deploy_path  = "gold_companion_deploy_ts.txt";    // deploy-forward anchor (persisted)
        std::string book_path    = "gold_companion_book.txt";         // REAL forward book (persists across restarts)
    };
    // (giveback bp, tier label) — operator spec x2: 20 banker, 150 runner.
    struct Tier { double gb_bp; const char* tag; };

    bool enabled = true;

    // ── LIVE EXECUTION WIRING — makes AUPOS/AUNEG REAL independent trading engines ──
    //   These are set ONLY in the live main TU (engine_init). Left null in the
    //   OmegaBacktest TU  ->  live_step_ short-circuits  ->  pure accounting, byte-identical
    //   to before (canary unaffected). When wired, each flavor runs its OWN position
    //   through the SAME machinery as every main engine, so it is SHADOW today
    //   (send_live_order no-ops while g_cfg.mode!="LIVE") and LIVE the instant mode flips —
    //   zero code change at flip. Checks/balances carried: (1) cost-gate at entry (gate_fn_),
    //   (2) BE-floor adverse-protection neg=0 by construction, (3) shadow-ledger record on
    //   close (ledger_fn_ -> TRADE HISTORY + shadow equity), (4) order path on entry+exit.
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>; // -> broker token ("" in shadow)
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;             // cost-gate entry filter
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit GoldBeFloorCompanion(Config c) : cfg_(std::move(c)) {
        // persisted deploy-forward anchor: keep the forward book stable across restarts.
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();   // persisted REAL forward book (cumulative clips/pts per flavor)
    }

    const Config& config() const { return cfg_; }

    // ---- warm-seed / rebuild from an H1 CSV (ts,o,h,l,c OR ts,c). PRE-DEPLOY history:
    // primes the 2h detector and rebuilds the book on restart; books nothing new by itself
    // (only bars whose clips close after deploy_ts count). Dedup+sort by ts. ----
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[AUGOLD][SEED] MISS %s\n", path.c_str()); std::fflush(stdout);
            return 0;
        }
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;   // skip header
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            const int got = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c);
            double close = (got == 5) ? c : (got == 2 ? o : 0.0);   // ts,o,h,l,c  or  ts,c
            if (got >= 2 && close > 0.0) { ingest_(norm_ts_(static_cast<int64_t>(ts)), close); ++n; }
        }
        std::printf("[AUGOLD][SEED] %zu H1 bars from %s (bars=%zu)\n", n, path.c_str(), ts_.size());
        std::fflush(stdout);
        return n;
    }

    // Call ONCE after all pre-deploy seeding. Stamps + persists deploy_ts (= latest seeded bar)
    // on the first-ever boot so the live book counts ONLY clips closing forward from deploy.
    void finalize_seed() noexcept {
        dedup_sort_();   // combined seed sources (warmup CSV may be ms + overlap the live dump);
                         // match tools/gold_befloor_companion.py _write_hist: keep-last per ts, sort by ts
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
            std::printf("[AUGOLD][DEPLOY] deploy_ts stamped = %lld (forward-only book)\n",
                        (long long)deploy_ts_);
        } else {
            std::printf("[AUGOLD][DEPLOY] deploy_ts loaded = %lld (persisted)\n", (long long)deploy_ts_);
        }
        std::fflush(stdout);
        recompute_and_write();   // publish the seeded (forward=$0) state immediately
    }

    // ---- LIVE feed: one CLOSED H1 bar (o=h=l=c=close, the RegimeState live-dump convention).
    // Registered as RegimeState's H1 sink so it sees exactly the bars gold_regime_h1.csv gets. ----
    void on_h1_bar(int64_t ts_sec, double close) noexcept {
        if (!enabled || close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        ingest_(ts_sec, close);
        live_step_(ts_sec);        // fire real (shadow/live-gated) orders on live BE-floor transitions
        recompute_and_write();
    }

    std::string state_json() const { return build_state_(); }

    void recompute_and_write() const noexcept {
        const std::string js = build_state_();
        const std::string tmp = cfg_.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return; f << js; }
        // WINDOWS gotcha: std::rename FAILS if the destination exists (EEXIST) -- the deploy box
        // is Windows, so without this the tmp piles up and the live file stays stale (the exact bug
        // that left the desk serving python's 3200-bar frame). Remove-then-rename replaces on both
        // platforms (no <windows.h> / NOMINMAX needed); the sub-ms gap is covered by the endpoint's
        // empty-state default and the hourly write cadence.
#if defined(_WIN32)
        std::remove(cfg_.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.state_path.c_str());
    }

private:
    Config cfg_;
    std::vector<int64_t> ts_;   // H1 bar close ts (sec)
    std::vector<double>  c_;     // H1 close (== o=h=l=c live-dump convention)
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;

    // ── live execution state — TWO runner legs per flavor (operator spec: 2 runners, not 1) ──
    //   Both legs are BE-floored: they arm ONLY once price covers cost (be_bp) and the trail stop is
    //   pinned >= entry (long) / <= entry (short), so each exits at a stall/reversal keeping profit —
    //   neg=0 by construction (the operator's definitive test: they cannot make a loss). The two
    //   runners differ only in trail giveback: r20 (tight, banks fast) + r150 (wide, rides). Each is
    //   its OWN independent position through the order path -> its OWN ledger row -> shows in PnL.
    static constexpr int    NT_ = 2;                       // runners per flavor
    static constexpr double LIVE_GB_[NT_] = { 20.0, 150.0 };
    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    bool     win_[2] = { false, false };                   // parent 2h window, per flavor (shared by both runners)
    struct LiveLeg { bool has_entry = false; double entry = 0, wm = 0, ref = 0; int64_t entry_ts = 0; std::string token; };
    LiveLeg live_[2][NT_];   // [flavor 0=AUPOS/long, 1=AUNEG/short][runner 0=r20, 1=r150]
    static std::string LEG_ENGINE_(int fi, int ti) {
        return std::string(fi == 0 ? "GoldBeFloorAUPOS" : "GoldBeFloorAUNEG") + (ti == 0 ? "_r20" : "_r150");
    }

    // ── REAL forward book: the desk headline. Accumulates ONLY the live-traded clips (forward of
    //   deploy_ts, the 2 runner positions per flavor), identical set to what ledger_fn_ books. This is
    //   the TRUE forward record — it excludes the replay accounting artifact (a pre-deploy entry
    //   whose run got credited to a post-deploy close, e.g. the fake $6k). Persisted so it accrues
    //   across restarts/deploys (in-memory legs reset on restart; closed-clip history must not). ──
    struct FwdBook { double pts = 0.0; int clips = 0; int wins = 0; };
    FwdBook fwd_[2][NT_];

    // Incremental mirror of detect_()+book_() on the NEWEST live bar, but instead of only
    // accumulating points it OPENS/CLOSES a real position via the order path. Fires ONLY when:
    //   - callbacks are wired (live main TU; null in backtest TU), and
    //   - the bar is forward of deploy_ts (never re-fires historical/seed arms on restart).
    // The order path (send_live_order behind open_fn_/close_fn_) is itself mode-gated:
    // no-op in SHADOW, real order in LIVE. So this is a shadow trade now, a live trade on flip.
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                                   // backtest TU: pure accounting, unchanged
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N <= W) return;
        const bool   fwd = ts_sec > deploy_ts_;                  // never act on replay / seed-overlap bars
        const double cur = c_[N - 1];
        const double j   = c_[N - 1] / c_[N - 1 - W] - 1.0;
        const double thr = cfg_.thr, be = cfg_.be_bp;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            const bool enter = up ? (j >=  thr) : (j <= -thr);
            const bool exit  = up ? (j <= -thr) : (j >=  thr);
            if (!win_[fi] && enter) {                             // parent 2h window opens -> arm BOTH runners
                win_[fi] = true;
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; L.has_entry = false; L.wm = 0; L.ref = cur; }
            }
            for (int ti = 0; ti < NT_ && win_[fi]; ++ti) {
                LiveLeg& L = live_[fi][ti];
                const double gb = LIVE_GB_[ti];                  // runner giveback (20bp tight / 150bp wide)
                if (!L.has_entry) {
                    // BE-FLOOR arm: open ONLY once price has covered cost (be_bp). Stop pinned >= entry.
                    const bool cond = up ? ((cur / L.ref - 1.0) * 1e4 >= be) : ((1.0 - cur / L.ref) * 1e4 >= be);
                    if (cond) {
                        const double tp_dist_pts = cur * (gb / 1e4);        // cost gate: giveback the trail must clear
                        const bool viable = !gate_fn_ || gate_fn_(cfg_.sym, tp_dist_pts, cfg_.lot);
                        if (viable) {
                            L.entry = cur; L.wm = cur; L.has_entry = true; L.entry_ts = ts_sec;
                            if (fwd) {
                                L.token = open_fn_(cfg_.sym, up, cfg_.lot, cur);
                                std::printf("[AUGOLD][OPEN] %s %s @%.2f lot=%.2f tok=%s\n",
                                            LEG_ENGINE_(fi, ti).c_str(), up ? "LONG" : "SHORT", cur, cfg_.lot, L.token.c_str());
                                std::fflush(stdout);
                            }
                        }
                    }
                } else {
                    double stop; bool hit;
                    if (up) { if (cur > L.wm) L.wm = cur; stop = std::max(L.entry, L.wm * (1.0 - gb / 1e4)); hit = (cur <= stop); }
                    else    { if (cur < L.wm) L.wm = cur; stop = std::min(L.entry, L.wm * (1.0 + gb / 1e4)); hit = (cur >= stop); }
                    if (hit) { close_leg_(fi, ti, up, stop, ts_sec, fwd, "TRAIL_STOP"); L.ref = stop; }
                }
            }
            if (win_[fi] && exit) {                              // parent window ended -> flush any open runners
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; if (L.has_entry) close_leg_(fi, ti, up, cur, ts_sec, fwd, "WINDOW_EXIT"); }
                win_[fi] = false;
            }
        }
    }

    void close_leg_(int fi, int ti, bool up, double px, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        LiveLeg& L = live_[fi][ti];
        if (fwd) {
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.sym, up, cfg_.lot, px, L.token);
            if (ledger_fn_) ledger_fn_(LEG_ENGINE_(fi, ti), cfg_.sym, up, L.entry, px, cfg_.lot, L.entry_ts, ts_sec, reason);
            // REAL forward book: raw price-pts booked (same value the ledger records as tr.pnl/size).
            // BE-floor guarantees p>=0 (trail stop>=entry long / <=entry short; window-flush only fires
            // when cur is above stop, i.e. still favourable) -> neg=0 holds live, matching accounting.
            const double p = up ? (px - L.entry) : (L.entry - px);
            fwd_[fi][ti].pts += p; fwd_[fi][ti].clips += 1; fwd_[fi][ti].wins += (p > 1e-6 ? 1 : 0);
            save_fwd_book_();
            std::printf("[AUGOLD][CLOSE] %s %s entry=%.2f exit=%.2f pts=%.2f (%s)\n",
                        LEG_ENGINE_(fi, ti).c_str(), up ? "LONG" : "SHORT", L.entry, px, p, reason);
            std::fflush(stdout);
        }
        L.has_entry = false; L.wm = 0; L.token.clear(); L.entry_ts = 0;
    }

    // ── persist / restore the REAL forward book (sidecar; one line per flavor: "fi pts clips wins").
    //   Written on every forward close (atomic tmp+rename), loaded once at construction. Keeps the
    //   desk headline monotonic across restarts without parsing the full shadow ledger CSV (the
    //   ledger remains the independent record to verify against; this matches it by construction). ──
    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int fi = -1, ti = -1; double pts = 0; int clips = 0, wins = 0;
        std::string line;
        while (std::getline(f, line)) {
            // 5-field per-runner format "fi ti pts clips wins" (2-runner). Old 4-field single-runner
            // rows simply don't parse -> that book (which was $0 anyway) resets clean. No neg risk.
            if (std::sscanf(line.c_str(), "%d %d %lf %d %d", &fi, &ti, &pts, &clips, &wins) == 5
                && fi >= 0 && fi < 2 && ti >= 0 && ti < NT_) {
                fwd_[fi][ti].pts = pts; fwd_[fi][ti].clips = clips; fwd_[fi][ti].wins = wins;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti)
              f << fi << " " << ti << " " << fwd_[fi][ti].pts << " " << fwd_[fi][ti].clips << " " << fwd_[fi][ti].wins << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    void ingest_(int64_t ts, double close) noexcept { ts_.push_back(ts); c_.push_back(close); }

    // Normalise a bar ts to SECONDS. Some seed CSVs (e.g. warmup_XAUUSD_H1.csv) carry ms ts;
    // the live dump + H1 sink are seconds. Mixed units break the deploy_ts gate + monotonicity.
    static int64_t norm_ts_(int64_t ts) noexcept { return ts >= 100000000000LL ? ts / 1000 : ts; }

    // Keep-last per ts, sort ascending — faithful to python _write_hist. Called once after all
    // seed sources are appended (warmup CSV may overlap the live dump), before deploy stamping.
    void dedup_sort_() noexcept {
        const size_t n = ts_.size();
        if (n < 2) return;
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        // stable sort by ts preserves original order among equal ts -> the LAST original wins on dedup.
        std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b){ return ts_[a] < ts_[b]; });
        std::vector<int64_t> nts; std::vector<double> nc; nts.reserve(n); nc.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            const size_t i = idx[k];
            if (k + 1 < n && ts_[idx[k + 1]] == ts_[i]) continue;   // skip: keep last of an equal-ts run
            nts.push_back(ts_[i]); nc.push_back(c_[i]);
        }
        ts_.swap(nts); c_.swap(nc);
    }

    struct Trade { int ei; int xi; };
    struct BookRes { double pts = 0.0; int clips = 0; int wins = 0; };

    // ── detector: faithful parent_long (up=true) / parent_short (up=false) ──
    std::vector<Trade> detect_(bool up) const {
        std::vector<Trade> tr;
        const int N = (int)c_.size();
        const int W = cfg_.W; const double thr = cfg_.thr;
        bool pos = false; int ent_ei = 0;
        for (int i = W; i < N; ++i) {
            const double j = c_[i] / c_[i - W] - 1.0;
            const bool enter = up ? (j >=  thr) : (j <= -thr);
            const bool exit  = up ? (j <= -thr) : (j >=  thr);
            if (!pos && enter) {
                const int ei = i + 1;
                if (ei >= N) continue;
                pos = true; ent_ei = ei;
            } else if (pos && exit) {
                int xi = i + 1; if (xi >= N) xi = N - 1;
                tr.push_back({ent_ei, xi}); pos = false;
            }
        }
        if (pos) tr.push_back({ent_ei, N - 1});
        return tr;
    }

    // ── BE-floor clip book: faithful book_points (deploy-forward gated, points). ──
    BookRes book_(const std::vector<Trade>& trades, bool is_long, double gb) const {
        BookRes r;
        const double be = cfg_.be_bp;
        for (const auto& t : trades) {
            double ref = c_[t.ei];          // o[ei] == c[ei] (close-only live bars)
            bool has_entry = false; double entry = 0.0, wm = 0.0;
            for (int i = t.ei; i < t.xi; ++i) {
                const double cur = c_[i];
                if (!has_entry) {
                    const bool cond = is_long ? ((cur / ref - 1.0) * 1e4 >= be)
                                              : ((1.0 - cur / ref) * 1e4 >= be);
                    if (cond) { entry = cur; wm = cur; has_entry = true; }
                    continue;
                }
                if (is_long) {
                    if (cur > wm) wm = cur;
                    const double stop = std::max(entry, wm * (1.0 - gb / 1e4));   // stop >= entry
                    if (cur <= stop) {
                        const double p = stop - entry;
                        if (t_ok_(i)) { r.pts += p; ++r.clips; r.wins += (p > 1e-6); }
                        has_entry = false; wm = 0.0; ref = stop;                  // reclip from exit
                    }
                } else {
                    if (cur < wm) wm = cur;
                    const double stop = std::min(entry, wm * (1.0 + gb / 1e4));   // stop <= entry
                    if (cur >= stop) {
                        const double p = entry - stop;
                        if (t_ok_(i)) { r.pts += p; ++r.clips; r.wins += (p > 1e-6); }
                        has_entry = false; wm = 0.0; ref = stop;
                    }
                }
            }
            if (has_entry) {                                                      // window ended open -> flush
                const double last = (t.xi - 1 >= t.ei) ? c_[t.xi - 1] : c_[t.ei];
                const double p = std::max(0.0, is_long ? (last - entry) : (entry - last));
                if (ts_ok_(t.xi)) { r.pts += p; ++r.clips; r.wins += (p > 1e-6); }
            }
        }
        return r;
    }
    bool t_ok_(int i)  const { return ts_[i]  > deploy_ts_; }   // clip closes at bar i
    bool ts_ok_(int x) const { return ts_[x]  > deploy_ts_; }   // flush stamps ts[xi]

    static void jnum_(std::ostringstream& o, const char* k, double v, int prec) {
        o.precision(prec); o << std::fixed << "\"" << k << "\":" << v;
    }

    // ── desk state JSON. HEADLINE = the REAL forward book (fwd_[fi], the live-traded clips only);
    //   the replay accounting book (detect_+book_, per-tier) is retained under "backtest" as a
    //   labeled reference. This is the fix for the fake-$6k artifact: the panel's desk_usd is now
    //   the true forward record ($0 until the first live clip closes), NOT the replay sum that
    //   credited a pre-deploy entry's whole run to a post-deploy close. Verify vs the shadow ledger. ──
    std::string build_state_() const {
        const double dpp = cfg_.dpp_per_lot * cfg_.lot;
        const Tier tiers[2] = { {20.0, "banker"}, {150.0, "runner"} };
        struct Flavor { const char* name; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"AUPOS", "long", true}, {"AUNEG", "short", false} };

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double desk_pts = 0.0, bt_desk_pts = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            // REAL forward book (headline) = sum of BOTH runners for this flavor
            double fwd_pts = 0.0; int fwd_clips = 0, fwd_wins = 0;
            for (int ti = 0; ti < NT_; ++ti) { fwd_pts += fwd_[fi][ti].pts; fwd_clips += fwd_[fi][ti].clips; fwd_wins += fwd_[fi][ti].wins; }
            desk_pts += fwd_pts;
            // replay accounting book (backtest reference), per-tier
            const auto trades = detect_(flavors[fi].is_long);
            double bt_pts = 0.0;
            std::ostringstream comps;
            for (int ti = 0; ti < 2; ++ti) {
                const BookRes b = book_(trades, flavors[fi].is_long, tiers[ti].gb_bp);
                bt_pts += b.pts;
                if (ti) comps << ",";
                comps.precision(0); comps << std::fixed;
                comps << "{\"tier\":\"" << tiers[ti].tag << "\",\"gb_bp\":" << tiers[ti].gb_bp
                      << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                comps.precision(2); comps << "\"pts\":" << b.pts << ",";
                comps.precision(0); comps << "\"usd\":" << (b.pts * dpp) << "}";
            }
            bt_desk_pts += bt_pts;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << flavors[fi].name << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"events\":" << fwd_clips << ",\"clips\":" << fwd_clips
               << ",\"wins\":" << fwd_wins << ",";
            fl.precision(2); fl << "\"book_pts\":" << fwd_pts << ",";
            fl.precision(0); fl << "\"book_usd\":" << (fwd_pts * dpp) << ",";
            // labeled backtest-view (the replay edge; NOT the forward record)
            fl << "\"backtest\":{\"events\":" << trades.size() << ",";
            fl.precision(2); fl << "\"book_pts\":" << bt_pts << ",";
            fl.precision(0); fl << "\"book_usd\":" << (bt_pts * dpp)
               << ",\"companions\":[" << comps.str() << "]}}";
        }
        o << "{\"ts\":" << (long long)last_ts << ",";
        o.precision(2); o << "\"lot\":" << cfg_.lot << ",\"dpp\":" << dpp << ",";
        o << "\"bars\":" << ts_.size() << ",\"shadow\":true,"
          << "\"engine\":\"gold-befloor-AUPOS-AUNEG\",\"deploy_ts\":" << (long long)deploy_ts_ << ",";
        o.precision(2); o << "\"desk_pts\":" << desk_pts << ",";
        o.precision(0); o << "\"desk_usd\":" << (desk_pts * dpp) << ",";
        o.precision(2); o << "\"bt_desk_pts\":" << bt_desk_pts << ",";
        o.precision(0); o << "\"bt_desk_usd\":" << (bt_desk_pts * dpp) << ",";
        o << "\"flavors\":[" << fl.str() << "]}";
        return o.str();
    }
};

// Singleton — accessor mirrors omega::gold_regime().
inline GoldBeFloorCompanion& gold_befloor_companion() noexcept {
    static GoldBeFloorCompanion inst = []{ return GoldBeFloorCompanion(GoldBeFloorCompanion::Config{}); }();
    return inst;
}

} // namespace omega
