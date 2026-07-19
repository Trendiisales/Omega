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
// native engine compiled into Omega.exe, mirroring the crypto MimicLadderCompanion
// architecture (C++ in the trading binary, own state file + own /api endpoint).
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-
// engine). Observe-only, shadow: it NEVER opens / moves / shrinks / closes a real
// position and is NEVER read by any parent. It self-detects its OWN up/down move
// windows (2h / +/-1%) from the gold H1 close stream it is fed, and runs x2 BE-floor
// tiers per direction (20bp banker / 150bp runner / 400bp wide). Judge STANDALONE (net>0, both
// regimes) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: RETIRED S-2026-07-07e (real-fill re-validation). The BE-floor is honest
//   dual-column accounting, but the EDGE is dead: no config in thr 0.3-3.0% x be 6/10/20 x
//   exec {close-only, buf10, buf25} x {ungated, sustained-bull-gated} is net-positive on real
//   fills in BOTH eras (2022-23 certified histdata ticks; 2024-26 m5-synth) with both WF halves
//   and both flavors positive. Live 1.0%/be6: -$68.5k (22-23) / +$68k but H1+Pos negative
//   (24-26). Bull-gate rule tested (feedback-companion-bull-gate-not-reject): rescues 22-23,
//   fails 24-26 WF. enabled=false in engine_init; state serves real history only.
//   Evidence outputs/BEFLOOR_FAMILY_REALFILL_2026-07-07.txt · registry §5.
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
#include <deque>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

class GoldBeFloorCompanion {
public:
    struct Config {
        int    W          = 2;        // detector window (H1 bars) -> "2h"
        double thr        = 0.01;     // detector threshold -> "+/-1%"
        double be_bp      = 6.0;      // GOLD_RT_BP: gross bp move to cover cost & open a leg
        double rt_cost_bp = 6.0;      // REAL round-trip cost (spread+slip+comm, bp of entry) debited
                                      //   from every clip's pts_real. be_bp only DELAYS the arm; it is
                                      //   not a cost credit — a floor exit at entry is a real -rt_cost_bp.
        double min_gb_mult= 3.0;      // TIER VIABILITY GATE: a tier arms only if its giveback
                                      //   LIVE_GB_[ti] >= min_gb_mult * rt_cost_bp -- a trail whose
                                      //   giveback is within a few multiples of the round-trip cost
                                      //   cannot clear costs on its typical clip. Non-viable tiers
                                      //   never open (open legs still managed to close); shown as
                                      //   "viable":false in the state JSON. 0 disables the gate.
        double lot        = 1.0;      // 1.0 std XAUUSD lot = 100 oz
        double dpp_per_lot = 100.0;   // $/point for 1.0 lot
        std::string sym    = "XAUUSD";// symbol the live legs trade (order path)
        double live_gb_bp  = 150.0;   // live trail giveback = runner tier (the prescribed backtested exit;
                                      //   banker 20bp is the tight variant — accounting panel still shows both).
        std::string state_path   = "gold_companion_state.json";       // served by /api/gold_companion
        std::string deploy_path  = "gold_companion_deploy_ts.txt";    // deploy-forward anchor (persisted)
        std::string book_path    = "gold_companion_book.txt";         // REAL forward book (persists across restarts)
        std::string live_path    = "gold_companion_live.txt";         // OPEN window+leg arm-state (persists across restarts)
        std::string closed_path  = "gold_companion_closed.csv";       // CLOSED forward trades list (persists; the "trades log")
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
    //   (2) BE-floor adverse protection (floor = order target; REAL booking can go negative on
    //   a gap through the floor), (3) shadow-ledger record on close at the REAL fill
    //   (ledger_fn_ -> TRADE HISTORY + shadow equity), (4) order path on entry+exit.
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
        load_fwd_book_();    // persisted REAL forward book (cumulative clips/pts per flavor)
        load_closed_();      // persisted CLOSED forward trades (the desk "trades log") -> survives restart
        load_live_state_();  // persisted OPEN window+leg arm-state -> RESUME across restart (no "reset after every deploy")
    }

    const Config& config() const { return cfg_; }

    // RETIREMENT helper (S-2026-07-07e): drop any persisted open window/leg arm-state so the
    // published state shows no frozen "open" legs after the engine stops being fed. Shadow arms
    // only (never a real position); closed-trade history and the real forward book are untouched.
    void clear_open_arms() noexcept {
        win_[0] = win_[1] = win_pend_[0] = win_pend_[1] = false;
        for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) live_[fi][ti] = LiveLeg{};
        save_live_state_();
    }

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

    // ── live execution state — runner legs per flavor ──
    //   Legs are BE-floored: they arm ONLY once price covers be_bp and the trail floor is pinned
    //   >= entry (long) / <= entry (short). The floor bounds the MODEL column only: real fills book
    //   worse-of(floor, observed close) minus rt_cost_bp, so pts_real CAN be negative (gap-through,
    //   or a floor exit that never covered the spread). The runners differ only in trail giveback.
    //   Each is its OWN independent position through the order path -> its OWN ledger row.
    static constexpr int    NT_ = 5;                       // runners per flavor (r20 banker / r150 runner / r400 wide / r50 / r100)
    static constexpr double LIVE_GB_[NT_] = { 20.0, 150.0, 400.0, 50.0, 100.0 };  // 50/100 APPENDED at end (persistence keys by index -> never reorder)
    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    bool     win_[2]      = { false, false };              // parent 2h window, per flavor (shared by all runners)
    bool     win_pend_[2] = { false, false };              // signal fired; ref anchors at the NEXT bar's close
                                                           //   (parity with detect_: ei = i+1, ref = c[ei])
    struct LiveLeg { bool has_entry = false; double entry = 0, wm = 0, ref = 0; int64_t entry_ts = 0; std::string token; };
    LiveLeg live_[2][NT_];   // [flavor 0=AUPOS/long, 1=AUNEG/short][runner 0=r20, 1=r150]
    static std::string LEG_ENGINE_(int fi, int ti) {
        return std::string(fi == 0 ? "GoldBeFloorAUPOS" : "GoldBeFloorAUNEG") + ("_r" + std::to_string((long)LIVE_GB_[ti]));
    }

    // ── REAL forward book: the desk headline. Accumulates ONLY the live-traded clips (forward of
    //   deploy_ts, the 2 runner positions per flavor), identical set to what ledger_fn_ books. This is
    //   the TRUE forward record — it excludes the replay accounting artifact (a pre-deploy entry
    //   whose run got credited to a post-deploy close, e.g. the fake $6k). Persisted so it accrues
    //   across restarts/deploys (in-memory legs reset on restart; closed-clip history must not). ──
    struct FwdBook { double pts = 0.0; int clips = 0; int wins = 0; double pts_real = 0.0; };
    FwdBook fwd_[2][NT_];   // pts = model (fill-at-floor, no cost); pts_real = observed fill - rt_cost_bp
                            // wins counts REAL wins (pts_real > 0) since the honest-accounting fix

    // CLOSED forward trades — the operator's "list of trades". Each completed leg (real forward
    // clip) is pushed here on close, then the engine leg is reset for the next trade. Persisted to
    // closed_path (append one CSV line per close) + last N reloaded at construction so the desk's
    // trades log survives restarts. RAM ring capped so the JSON stays bounded.
    struct Closed { int fi = 0, ti = 0; double entry = 0, exit = 0, pts = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason;
                    double pts_real = 0, usd_real = 0; };   // REAL columns (observed fill - cost)
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;

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
        // GAP GUARD: the W-bar jump is index-based, so across a weekend/holiday/outage it spans days,
        // arming into gap-spread regimes the calibration never contained. Block NEW windows on a
        // non-contiguous span (2x slack for a single missing bar); exits stay honoured — flushing on
        // a gap is protective, arming into one is not.
        const bool contig = (ts_[N - 1] - ts_[N - 1 - W]) <= (int64_t)W * 3600 * 2;
        const double thr = cfg_.thr, be = cfg_.be_bp;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            const bool enter = contig && (up ? (j >=  thr) : (j <= -thr));
            const bool exit  = up ? (j <= -thr) : (j >=  thr);
            if (win_pend_[fi]) {                                  // window opens the bar AFTER the signal:
                win_pend_[fi] = false; win_[fi] = true;           // ref = c[ei] (parity with detect_/book_;
                for (int ti = 0; ti < NT_; ++ti) {                //  live previously anchored one bar early)
                    LiveLeg& L = live_[fi][ti]; L.has_entry = false; L.wm = 0; L.ref = cur;
                }
            }
            if (!win_[fi] && !win_pend_[fi] && enter) win_pend_[fi] = true;   // parent 2h window signal
            for (int ti = 0; ti < NT_ && win_[fi]; ++ti) {
                LiveLeg& L = live_[fi][ti];
                const double gb = LIVE_GB_[ti];                  // runner giveback (20bp tight / 150bp wide)
                if (!L.has_entry) {
                    if (!tier_viable_(ti)) continue;   // weeded out: giveback < min_gb_mult x real cost
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
                    // REAL FILL: the floor is an order target; the observed close is the only tradable
                    // mark. Book at the WORSE of the two (gap-through books the gap, not the wish).
                    if (hit) { close_leg_(fi, ti, up, stop, cur, ts_sec, fwd, "TRAIL_STOP"); L.ref = stop; }
                }
            }
            if (win_[fi] && exit) {                              // parent window ended -> flush any open runners
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; if (L.has_entry) close_leg_(fi, ti, up, cur, cur, ts_sec, fwd, "WINDOW_EXIT"); }
                win_[fi] = false;
            }
        }
        save_live_state_();   // snapshot window+leg arm-state every live bar -> restart RESUMES, never re-zeroes
    }

    // TIER VIABILITY: giveback must exceed the instrument's real RT cost by min_gb_mult.
    bool tier_viable_(int ti) const noexcept {
        return cfg_.min_gb_mult <= 0.0 || LIVE_GB_[ti] >= cfg_.min_gb_mult * cfg_.rt_cost_bp;
    }
    // px_floor = model floor/stop level; px_obs = observed H1 close (the only tradable mark).
    // REAL booking: fill = worse-of(floor, observed) per side, cost = rt_cost_bp of entry.
    void close_leg_(int fi, int ti, bool up, double px_floor, double px_obs,
                    int64_t ts_sec, bool fwd, const char* reason) noexcept {
        LiveLeg& L = live_[fi][ti];
        const double fill = up ? std::min(px_floor, px_obs) : std::max(px_floor, px_obs);
        if (fwd) {
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.sym, up, cfg_.lot, fill, L.token);
            // Ledger records the REAL fill — the shadow ledger is the system of record and must be
            // able to disagree with the model (it previously received the floor, a circular check).
            if (ledger_fn_) ledger_fn_(LEG_ENGINE_(fi, ti), cfg_.sym, up, L.entry, fill, cfg_.lot, L.entry_ts, ts_sec, reason);
            const double p      = up ? (px_floor - L.entry) : (L.entry - px_floor);   // MODEL (>=0 by algebra)
            const double p_real = (up ? (fill - L.entry) : (L.entry - fill))
                                  - L.entry * (cfg_.rt_cost_bp / 1e4);                // REAL (can be negative)
            fwd_[fi][ti].pts += p; fwd_[fi][ti].pts_real += p_real;
            fwd_[fi][ti].clips += 1; fwd_[fi][ti].wins += (p_real > 1e-6 ? 1 : 0);
            save_fwd_book_();
            // completed trade -> push to the trades list (desk "trades log"), persist, then reset leg below.
            const double dpp = cfg_.dpp_per_lot * cfg_.lot;
            Closed rec{fi, ti, L.entry, fill, p, p * dpp, L.entry_ts, ts_sec, reason, p_real, p_real * dpp};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[AUGOLD][CLOSE] %s %s entry=%.2f fill=%.2f pts_model=%.2f pts_real=%.2f (%s)\n",
                        LEG_ENGINE_(fi, ti).c_str(), up ? "LONG" : "SHORT", L.entry, fill, p, p_real, reason);
            std::fflush(stdout);
        }
        L.has_entry = false; L.wm = 0; L.token.clear(); L.entry_ts = 0;
        save_live_state_();   // persist the leg reset NOW: a crash before the end-of-step snapshot must
                              // not resurrect this leg on restart and double-book the clip (book+CSV+ledger).
    }

    // ── persist / restore the REAL forward book (sidecar; one line per flavor: "fi pts clips wins").
    //   Written on every forward close (atomic tmp+rename), loaded once at construction. Keeps the
    //   desk headline monotonic across restarts without parsing the full shadow ledger CSV (the
    //   ledger remains the independent record to verify against; this matches it by construction). ──
    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int fi = -1, ti = -1; double pts = 0; int clips = 0, wins = 0; double preal = 0;
        std::string line;
        while (std::getline(f, line)) {
            // 6-field "fi ti pts clips wins pts_real"; old 5-field rows load with pts_real=0
            // (real accounting accrues from the honest-accounting deploy forward — we cannot
            // reconstruct real fills for clips that were only ever booked at the model floor).
            const int n = std::sscanf(line.c_str(), "%d %d %lf %d %d %lf", &fi, &ti, &pts, &clips, &wins, &preal);
            if (n >= 5 && fi >= 0 && fi < 2 && ti >= 0 && ti < NT_) {
                fwd_[fi][ti].pts = pts; fwd_[fi][ti].clips = clips; fwd_[fi][ti].wins = wins;
                fwd_[fi][ti].pts_real = (n >= 6) ? preal : 0.0;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti)
              f << fi << " " << ti << " " << fwd_[fi][ti].pts << " " << fwd_[fi][ti].clips << " "
                << fwd_[fi][ti].wins << " " << fwd_[fi][ti].pts_real << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    // ── CLOSED forward trades log: append one CSV row per completed clip; reload last N at ctor. ──
    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.fi << "," << r.ti << "," << r.entry << "," << r.exit << "," << r.pts << ","
          << r.usd << "," << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << ","
          << r.pts_real << "," << r.usd_real << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            long long ets = 0, xts = 0; double preal = 0, ureal = 0;   // no aliasing casts on int64_t
            const int n = std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n],%lf,%lf",
                                      &r.fi, &r.ti, &r.entry, &r.exit, &r.pts, &r.usd,
                                      &ets, &xts, reason, &preal, &ureal);
            if (n >= 8 && r.fi >= 0 && r.fi < 2 && r.ti >= 0 && r.ti < NT_) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                // pre-fix rows carry only the model figure; display it in both columns (marked by
                // the row's position before the honest-accounting deploy; real accrual is forward-only).
                r.pts_real = (n >= 11) ? preal : r.pts;
                r.usd_real = (n >= 11) ? ureal : r.usd;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

    // ── persist / restore the OPEN arm-state (window flags + open runner legs). THIS is the fix for
    //   "engine resets after every deploy": win_/live_ were RAM-only, so every VPS restart wiped any
    //   in-progress 2h window + open legs -> the engine re-zeroed to idle and had to wait for a fresh
    //   move to re-arm. On assets that actually move (crypto/FX) a multi-bar window rarely survived a
    //   restart, so forward clips stayed ~0. Now the live state is snapshotted (atomic tmp+rename) on
    //   every mutation and reloaded at construction, so a restart RESUMES the open window/legs.
    //   Format: "win <w0> <w1>" then one "leg <fi> <ti> <has> <entry> <wm> <ref> <ets> <token|->" line
    //   per leg. token '-' == none/shadow. Restored legs keep their broker token so a LIVE position that
    //   outlived the process is managed to its trail stop instead of being orphaned. ──
    void load_live_state_() noexcept {
        std::ifstream f(cfg_.live_path);
        if (!f.is_open()) return;
        std::string kind;
        while (f >> kind) {
            if (kind == "win") { int w0 = 0, w1 = 0; f >> w0 >> w1; win_[0] = (w0 != 0); win_[1] = (w1 != 0); }
            else if (kind == "winp") { int p0 = 0, p1 = 0; f >> p0 >> p1; win_pend_[0] = (p0 != 0); win_pend_[1] = (p1 != 0); }
            else if (kind == "leg") {
                int fi = -1, ti = -1, has = 0; double entry = 0, wm = 0, ref = 0; long long ets = 0; std::string tok;
                f >> fi >> ti >> has >> entry >> wm >> ref >> ets >> tok;
                if (fi >= 0 && fi < 2 && ti >= 0 && ti < NT_) {
                    LiveLeg& L = live_[fi][ti];
                    L.has_entry = (has != 0); L.entry = entry; L.wm = wm; L.ref = ref; L.entry_ts = (int64_t)ets;
                    L.token = (tok == "-") ? std::string() : tok;
                }
            } else { std::string rest; std::getline(f, rest); }   // tolerate unknown lines
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << "win " << (win_[0] ? 1 : 0) << " " << (win_[1] ? 1 : 0) << "\n";
          f << "winp " << (win_pend_[0] ? 1 : 0) << " " << (win_pend_[1] ? 1 : 0) << "\n";
          for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) {
              const LiveLeg& L = live_[fi][ti];
              f << "leg " << fi << " " << ti << " " << (L.has_entry ? 1 : 0) << " "
                << L.entry << " " << L.wm << " " << L.ref << " " << (long long)L.entry_ts << " "
                << (L.token.empty() ? "-" : L.token) << "\n";
          } }
#if defined(_WIN32)
        std::remove(cfg_.live_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.live_path.c_str());
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

    // ── desk state JSON. REAL TRADES ONLY. There is NO backtest/replay number in here anymore —
    //   the operator's rule: if it is not a real forward trade it does not belong in the live GUI.
    //   Emits (1) desk_usd/desk_pts = the REAL forward book ($0 until the first live clip closes),
    //   (2) per-flavor per-runner forward rows (both r20 banker + r150 runner -> 2 companions each),
    //   (3) "open" = the live legs open RIGHT NOW (entry/wm/cur/uPnL), (4) "trades" = the closed-
    //   forward-trades log (most-recent first). On close a leg is booked into (4) and reset for the
    //   next trade. Verify vs the shadow ledger — this matches it by construction. ──
    std::string build_state_() const {
        const double dpp = cfg_.dpp_per_lot * cfg_.lot;
        struct Flavor { const char* name; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"AUPOS", "long", true}, {"AUNEG", "short", false} };
        const char* TIER_TAG[NT_] = { "banker", "runner", "wide", "r50", "r100" };
        const double cur = c_.empty() ? 0.0 : c_.back();

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double desk_pts = 0.0, desk_pts_real = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            double fwd_pts = 0.0, fwd_pts_real = 0.0; int fwd_clips = 0, fwd_wins = 0;
            std::ostringstream runs;
            for (int ti = 0; ti < NT_; ++ti) {
                const FwdBook& b = fwd_[fi][ti];
                fwd_pts += b.pts; fwd_pts_real += b.pts_real; fwd_clips += b.clips; fwd_wins += b.wins;
                if (ti) runs << ",";
                runs.precision(0); runs << std::fixed;
                runs << "{\"tier\":\"" << TIER_TAG[ti] << "\",\"gb_bp\":" << (long)LIVE_GB_[ti]
                     << ",\"viable\":" << (tier_viable_(ti) ? "true" : "false")
                     << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                runs.precision(2); runs << "\"pts\":" << b.pts << ",\"pts_real\":" << b.pts_real << ",";
                runs.precision(0); runs << "\"usd\":" << (b.pts * dpp)
                                        << ",\"usd_real\":" << (b.pts_real * dpp) << "}";
            }
            desk_pts += fwd_pts; desk_pts_real += fwd_pts_real;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << flavors[fi].name << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"clips\":" << fwd_clips << ",\"wins\":" << fwd_wins << ",";
            fl.precision(2); fl << "\"book_pts\":" << fwd_pts << ",\"book_pts_real\":" << fwd_pts_real << ",";
            fl.precision(0); fl << "\"book_usd\":" << (fwd_pts * dpp)
               << ",\"book_usd_real\":" << (fwd_pts_real * dpp)
               << ",\"runners\":[" << runs.str() << "]}";
        }

        // OPEN legs right now — the live position detail (empty array = engine idle/reset, nothing open).
        std::ostringstream op; int nopen = 0;
        for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) {
            const LiveLeg& L = live_[fi][ti];
            if (!L.has_entry) continue;
            const bool up = (fi == 0);
            const double u  = up ? (cur - L.entry) : (L.entry - cur);
            const double ur = u - L.entry * (cfg_.rt_cost_bp / 1e4);   // real uPnL (cost debited)
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << flavors[fi].name << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"tier\":\"" << TIER_TAG[ti] << "\",";
            op.precision(2);
            op << "\"entry\":" << L.entry << ",\"wm\":" << L.wm << ",\"cur\":" << cur
               << ",\"upnl_pts\":" << u << ",\"upnl_pts_real\":" << ur << ",";
            op.precision(0);
            op << "\"upnl_usd\":" << (u * dpp) << ",\"entry_ts\":" << (long long)L.entry_ts << "}";
        }

        // CLOSED forward trades log — most-recent first.
        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c = *it;
            const int cfi = (c.fi >= 0 && c.fi < 2) ? c.fi : 0;
            if (ntr++) tr << ",";
            tr.precision(0); tr << std::fixed;
            tr << "{\"flavor\":\"" << flavors[cfi].name << "\",\"dir\":\"" << flavors[cfi].dir
               << "\",\"tier\":\"" << TIER_TAG[(c.ti >= 0 && c.ti < NT_) ? c.ti : 0] << "\",";
            tr.precision(2);
            tr << "\"entry\":" << c.entry << ",\"exit\":" << c.exit << ",\"pts\":" << c.pts
               << ",\"pts_real\":" << c.pts_real << ",";
            tr.precision(0);
            tr << "\"usd\":" << c.usd << ",\"usd_real\":" << c.usd_real
               << ",\"reason\":\"" << c.reason << "\",\"entry_ts\":" << (long long)c.ets
               << ",\"exit_ts\":" << (long long)c.xts << "}";
        }

        o << "{\"ts\":" << (long long)last_ts << ",";
        o.precision(2); o << "\"lot\":" << cfg_.lot << ",\"dpp\":" << dpp
                          << ",\"rt_cost_bp\":" << cfg_.rt_cost_bp << ",";
        o << "\"bars\":" << ts_.size() << ",\"shadow\":true,"
          << "\"engine\":\"gold-befloor-AUPOS-AUNEG\",\"deploy_ts\":" << (long long)deploy_ts_ << ",";
        o.precision(2); o << "\"desk_pts\":" << desk_pts << ",\"desk_pts_real\":" << desk_pts_real << ",";
        o.precision(0); o << "\"desk_usd\":" << (desk_pts * dpp)
                          << ",\"desk_usd_real\":" << (desk_pts_real * dpp) << ",";
        o << "\"open\":[" << op.str() << "],\"trades\":[" << tr.str() << "],";
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
