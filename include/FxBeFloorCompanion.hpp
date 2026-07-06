#pragma once
// =============================================================================
// FxBeFloorCompanion — per-pair <PAIR>Pos (long) + <PAIR>Neg (short) BE-floor books
// for EURUSD / GBPUSD / USDJPY / AUDUSD / NZDUSD.
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math : backtest/fx_befloor_ls.py  (parent() detector + leg_book() BE-floor walk)
//          — itself the gold mechanism (backtest/gold_befloor_ls.py) FX-tuned.
// Mirrors GoldBeFloorCompanion (native C++, own aggregate state file + /api endpoint).
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-
// engine). Observe-only, shadow: NEVER opens / moves / shrinks / closes a real
// position, NEVER read by any parent. Each pair self-detects its OWN 2h up/down move
// windows (W=2 H1, +/-thr) from that pair's H1 close stream, and runs x2 BE-floor
// tiers per direction (20bp banker / 150bp runner / 400bp wide). Judge STANDALONE (net>0, both WF
// halves) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: BE-FLOOR + REAL-FILL ACCOUNTING (honest-accounting fix, S-2026-07-07).
//   A leg stays FLAT until price clears +be_bp from its ref, opens THERE, and its trail
//   floor sits at-or-above entry (long) / at-or-below entry (short), trailing favourably
//   only. The floor is an ORDER TARGET, not a guaranteed fill: bars are H1 CLOSES, so a
//   close can gap THROUGH the floor (news candle, weekend open). Booking is therefore
//   dual-column:
//     pct/usd            = MODEL (legacy fill-at-floor, zero cost; >=0 by algebra) —
//                          comparison column only, NOT a performance claim.
//     pct_real/usd_real  = REAL  (fill = worse-of(floor, observed close), minus
//                          rt_cost_bp round-trip cost) — CAN BE NEGATIVE; this is the
//                          column the engine is judged on, and the ledger records it.
//   The old "neg=0 by construction" wording described the model column only. Backtest
//   reference backtest/fx_befloor_ls.py is model-fill (GBPUSD 2022-H2 Neg +52% etc. are
//   model figures): re-run with real fills + cost before any LIVE flip.
//
// FX vs gold deltas: thr LOWER (0.30% vs gold 1% — majors rarely move 1% in 2h),
//   be_bp SMALLER (2bp vs gold 6 — major RT spread+comm), and the book accumulates
//   PERCENT of notional (leg_book returns g/100), not absolute price points. Desk USD
//   = pct% * (notional/100) * lot = pct% * $1000/lot for a USD-quote major (research
//   convention: 1% ~ $1000/std lot; approximate for the JPY-quote USDJPY, documented).
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <deque>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

// ── one pair's Pos/Neg BE-floor book ─────────────────────────────────────────
class FxBeFloorPair {
public:
    struct Config {
        std::string pair    = "EURUSD";
        int    W            = 2;          // detector window (H1 bars) -> "2h"
        double thr          = 0.003;      // 2h jump arm threshold (0.30%; research best 0.20-0.30)
        double be_bp        = 2.0;        // RT cost floor (bp) to open a leg (research best be=2)
        double rt_cost_bp   = 2.0;        // REAL round-trip cost (spread+slip+comm, bp of entry) debited
                                          //   from every clip's pct_real. be_bp only DELAYS the arm; it is
                                          //   not a cost credit — a floor exit at entry is a real -rt_cost_bp.
        double min_gb_mult= 3.0;      // TIER VIABILITY GATE: a tier arms only if its giveback
                                      //   LIVE_GB_[ti] >= min_gb_mult * rt_cost_bp -- a trail whose
                                      //   giveback is within a few multiples of the round-trip cost
                                      //   cannot clear costs on its typical clip. Non-viable tiers
                                      //   never open (open legs still managed to close); shown as
                                      //   "viable":false in the state JSON. 0 disables the gate.
        double notional     = 100000.0;   // std-lot notional -> 1% == $1000 (USD-quote major)
        double lot          = 1.0;
        std::string deploy_path;          // per-pair persisted deploy-forward anchor
        std::string bars_path;            // per-pair persisted LIVE forward H1 bars (survives restart)
        std::string book_path;            // per-pair persisted REAL forward book (2 runners x 2 dirs)
        std::string live_path;            // per-pair persisted OPEN window+leg arm-state (survives restart)
        std::string closed_path;          // per-pair persisted CLOSED forward trades log (the "trades log")
    };
    struct Tier { double gb_bp; const char* tag; };

    // ── LIVE EXECUTION WIRING — makes <PAIR>Pos/<PAIR>Neg REAL trading engines (2 runners each).
    //   Identical contract to GoldBeFloorCompanion: set ONLY in the live main TU; null in backtest
    //   TU -> live_step_ short-circuits (pure accounting, canary unaffected). Each runner opens its
    //   OWN position via the order path (SHADOW today: send_live_order no-ops while mode!=LIVE; LIVE
    //   on flip) and records EACH close to the shadow ledger at the REAL fill (worse-of(floor,
    //   observed close); can go negative on a gap through the floor) -> ENGINE LEDGER + headline PnL. ──
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit FxBeFloorPair(Config c) : cfg_(std::move(c)) {
        if (cfg_.deploy_path.empty())
            cfg_.deploy_path = "fx_companion_" + lower_(cfg_.pair) + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())
            cfg_.bars_path = "fx_companion_" + lower_(cfg_.pair) + "_h1.csv";
        if (cfg_.book_path.empty())
            cfg_.book_path = "fx_companion_" + lower_(cfg_.pair) + "_book.txt";
        if (cfg_.live_path.empty())
            cfg_.live_path = "fx_companion_" + lower_(cfg_.pair) + "_live.txt";
        if (cfg_.closed_path.empty())
            cfg_.closed_path = "fx_companion_" + lower_(cfg_.pair) + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();       // CLOSED forward trades log -> survives restart
        load_live_state_();   // RESUME open window+legs across restart (fix "resets every deploy")
    }

    const std::string& pair() const { return cfg_.pair; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }

    // warm-seed from an H1 CSV (ts,o,h,l,c OR ts,c). Primes the detector + rebuilds the
    // book across restarts; books nothing new by itself (deploy-forward gate). Dedup+sort.
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) return 0;
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue; // skip header
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            const int got = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c);
            const double close = (got == 5) ? c : (got == 2 ? o : 0.0);
            if (got >= 2 && close > 0.0) { ingest_(norm_ts_((int64_t)ts), close); ++n; }
        }
        return n;
    }

    // Call ONCE after all pre-deploy seeding: stamp+persist deploy_ts on first-ever boot;
    // load the persisted anchor on restart. Then publish the seeded (forward=0) state.
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
        }
    }

    // LIVE feed: one CLOSED H1 bar for this pair (close = mid at H1 close).
    void on_h1_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);   // PERSIST the forward bar so the book survives restart
        live_step_(ts_sec);            // fire real (shadow/live-gated) orders on live BE-floor transitions
    }

    // Reload persisted LIVE forward bars (written by on_h1_bar) from the cwd dump CSV.
    // Direct-path read (NOT resolve_seed_path -- this file lives in the working dir, C:\Omega).
    // Books nothing new by itself (deploy-forward gate); the recompute replays these forward
    // bars so the FX book is NON-VOLATILE across restarts (mirrors gold's persisted forward book).
    size_t seed_dump() noexcept {
        std::ifstream f(cfg_.bars_path);
        if (!f.is_open()) return 0;
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue;
            double ts = 0, cl = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf", &ts, &cl) == 2 && cl > 0.0) {
                ingest_(norm_ts_((int64_t)ts), cl); ++n;
            }
        }
        return n;
    }

    // Emit this pair's desk JSON object. REAL TRADES ONLY — no backtest/replay number (operator rule:
    // if it is not a forward trade it does not belong in the live GUI). "usd"/"pct" = the REAL forward
    // book ($0 until the first live clip closes). Per-flavor "runners" = both r20 banker + r150 runner
    // (forward-only). "open" = live legs open right now. "trades" = closed forward trades (recent first).
    // Flavors named <PAIR>Pos / <PAIR>Neg (research convention, distinct from gold AUPOS/AUNEG).
    std::string pair_json() const {
        const double usd_per_pct = cfg_.notional / 100.0 * cfg_.lot;   // %-point -> USD
        const char* TIER_TAG[NT_] = { "banker", "runner", "wide", "r50", "r100" };
        struct Flavor { const char* suffix; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"Pos", "long", true}, {"Neg", "short", false} };
        const double cur = c_.empty() ? 0.0 : c_.back();

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double pair_pct = 0.0, pair_pct_real = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            double book_pct = 0.0, book_pct_real = 0.0; int fwd_clips = 0, fwd_wins = 0;
            std::ostringstream runs;
            for (int ti = 0; ti < NT_; ++ti) {
                const FwdBook& b = fwd_[fi][ti];   // .pts holds pct-return points (FX)
                book_pct += b.pts; book_pct_real += b.pts_real; fwd_clips += b.clips; fwd_wins += b.wins;
                if (ti) runs << ",";
                runs.precision(0); runs << std::fixed;
                runs << "{\"tier\":\"" << TIER_TAG[ti] << "\",\"gb_bp\":" << (long)LIVE_GB_[ti]
                     << ",\"viable\":" << (tier_viable_(ti) ? "true" : "false")
                     << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                runs.precision(3); runs << "\"pct\":" << b.pts << ",\"pct_real\":" << b.pts_real << ",";
                runs.precision(0); runs << "\"usd\":" << (b.pts * usd_per_pct)
                                        << ",\"usd_real\":" << (b.pts_real * usd_per_pct) << "}";
            }
            pair_pct += book_pct; pair_pct_real += book_pct_real;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << cfg_.pair << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"clips\":" << fwd_clips << ",\"wins\":" << fwd_wins << ",";
            fl.precision(3); fl << "\"book_pct\":" << book_pct << ",\"book_pct_real\":" << book_pct_real << ",";
            fl.precision(0); fl << "\"book_usd\":" << (book_pct * usd_per_pct)
               << ",\"book_usd_real\":" << (book_pct_real * usd_per_pct)
               << ",\"runners\":[" << runs.str() << "]}";
        }

        // OPEN legs right now (empty = idle/reset).
        std::ostringstream op; int nopen = 0;
        for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) {
            const LiveLeg& L = live_[fi][ti];
            if (!L.has_entry) continue;
            const bool up = (fi == 0);
            const double p = up ? (cur - L.entry) : (L.entry - cur);
            const double upct = (L.entry != 0.0) ? (p / L.entry) * 100.0 : 0.0;
            const double upct_real = upct - cfg_.rt_cost_bp / 100.0;   // real uPnL (cost debited)
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << cfg_.pair << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"tier\":\"" << TIER_TAG[ti] << "\",";
            op.precision(5); op << "\"entry\":" << L.entry << ",\"wm\":" << L.wm << ",\"cur\":" << cur << ",";
            op.precision(3); op << "\"upnl_pct\":" << upct << ",\"upnl_pct_real\":" << upct_real << ",";
            op.precision(0); op << "\"upnl_usd\":" << (upct * usd_per_pct)
               << ",\"entry_ts\":" << (long long)L.entry_ts << "}";
        }

        // CLOSED forward trades log — most-recent first.
        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c = *it;
            const int cfi = (c.fi >= 0 && c.fi < 2) ? c.fi : 0;
            if (ntr++) tr << ",";
            tr.precision(0); tr << std::fixed;
            tr << "{\"flavor\":\"" << cfg_.pair << flavors[cfi].suffix << "\",\"dir\":\""
               << flavors[cfi].dir << "\",\"tier\":\"" << TIER_TAG[(c.ti >= 0 && c.ti < NT_) ? c.ti : 0] << "\",";
            tr.precision(5); tr << "\"entry\":" << c.entry << ",\"exit\":" << c.exit << ",";
            tr.precision(3); tr << "\"pct\":" << c.pct << ",\"pct_real\":" << c.pct_real << ",";
            tr.precision(0); tr << "\"usd\":" << c.usd << ",\"usd_real\":" << c.usd_real
               << ",\"reason\":\"" << c.reason
               << "\",\"entry_ts\":" << (long long)c.ets << ",\"exit_ts\":" << (long long)c.xts << "}";
        }

        o << "{\"pair\":\"" << cfg_.pair << "\",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_ << ",\"ts\":" << (long long)last_ts << ",";
        o.precision(2); o << "\"rt_cost_bp\":" << cfg_.rt_cost_bp << ",";
        o.precision(3); o << "\"pct\":" << pair_pct << ",\"pct_real\":" << pair_pct_real << ",";
        o.precision(0); o << "\"usd\":" << (pair_pct * usd_per_pct)
                          << ",\"usd_real\":" << (pair_pct_real * usd_per_pct) << ",";
        o << "\"open\":[" << op.str() << "],\"trades\":[" << tr.str() << "],";
        o << "\"flavors\":[" << fl.str() << "]}";
        return o.str();
    }

private:
    Config cfg_;
    std::vector<int64_t> ts_;
    std::vector<double>  c_;
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;

    struct Trade { int ei; int xi; };
    struct BookRes { double pct = 0.0; int clips = 0; int wins = 0; };

    // ── live execution — runner legs per direction, BE-floored
    //   (arm only once price covers be_bp; trail floor pinned >= entry long / <= entry short).
    //   The floor bounds the MODEL column only: real fills book worse-of(floor, observed close)
    //   minus rt_cost_bp, so pct_real CAN be negative (gap-through, or a floor exit that never
    //   covered the spread). Each runner is its OWN position -> OWN ledger row -> shows in PnL. ──
    static constexpr int    NT_ = 5;   // r20 banker / r150 runner / r400 wide / r50 / r100
    static constexpr double LIVE_GB_[NT_] = { 20.0, 150.0, 400.0, 50.0, 100.0 };  // 50/100 APPENDED at end (persistence keys by index -> never reorder)
    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    bool     win_[2]      = { false, false };            // parent 2h window per direction (0=Pos/long,1=Neg/short)
    bool     win_pend_[2] = { false, false };            // signal fired; ref anchors at the NEXT bar's close
                                                         //   (parity with detect_: ei = i+1, ref = c[ei])
    struct LiveLeg { bool has_entry = false; double entry = 0, wm = 0, ref = 0; int64_t entry_ts = 0; std::string token; };
    LiveLeg live_[2][NT_];
    struct FwdBook { double pts = 0.0; int clips = 0; int wins = 0; double pts_real = 0.0; };
    FwdBook  fwd_[2][NT_];   // .pts = MODEL pct-return points (fill-at-floor, no cost);
                             // .pts_real = REAL pct (observed fill - rt_cost_bp). wins counts REAL
                             // wins (pts_real > 0) since the honest-accounting fix.

    // CLOSED forward trades — the "trades log". Each completed leg pushed on close, leg reset for next.
    // .pct = MODEL trade %-return (fill-at-floor, >=0 by algebra); .pct_real/.usd_real = REAL columns
    // (observed fill - cost; can be negative); .usd = pct * usd_per_pct at close time.
    struct Closed { int fi = 0, ti = 0; double entry = 0, exit = 0, pct = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason;
                    double pct_real = 0, usd_real = 0; };   // REAL columns (observed fill - cost)
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;
    std::string leg_engine_(int fi, int ti) const {
        return cfg_.pair + (fi == 0 ? "Pos" : "Neg") + ("_r" + std::to_string((long)LIVE_GB_[ti]));
    }

    // Incremental live BE-floor state machine on the NEWEST bar (mirrors GoldBeFloorCompanion).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                               // backtest TU / not wired: pure accounting
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N <= W) return;
        const bool   fwd = ts_sec > deploy_ts_;
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
                const double gb = LIVE_GB_[ti];
                if (!L.has_entry) {
                    if (!tier_viable_(ti)) continue;   // weeded out: giveback < min_gb_mult x real cost
                    const bool cond = up ? ((cur / L.ref - 1.0) * 1e4 >= be) : ((1.0 - cur / L.ref) * 1e4 >= be);
                    if (cond) {
                        const double tp_dist_pts = cur * (gb / 1e4);
                        const bool viable = !gate_fn_ || gate_fn_(cfg_.pair, tp_dist_pts, cfg_.lot);
                        if (viable) {
                            L.entry = cur; L.wm = cur; L.has_entry = true; L.entry_ts = ts_sec;
                            if (fwd) {
                                L.token = open_fn_(cfg_.pair, up, cfg_.lot, cur);
                                std::printf("[AUFX][OPEN] %s %s @%.5f lot=%.2f tok=%s\n",
                                            leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", cur, cfg_.lot, L.token.c_str());
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
            if (win_[fi] && exit) {
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
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.pair, up, cfg_.lot, fill, L.token);
            // Ledger records the REAL fill — the shadow ledger is the system of record and must be
            // able to disagree with the model (it previously received the floor, a circular check).
            if (ledger_fn_) ledger_fn_(leg_engine_(fi, ti), cfg_.pair, up, L.entry, fill, cfg_.lot, L.entry_ts, ts_sec, reason);
            const double p      = up ? (px_floor - L.entry) : (L.entry - px_floor);   // MODEL pts (>=0 by algebra)
            const double p_real = (up ? (fill - L.entry) : (L.entry - fill))
                                  - L.entry * (cfg_.rt_cost_bp / 1e4);                // REAL (can be negative)
            const double pct      = (L.entry != 0.0) ? (p      / L.entry) * 100.0 : 0.0;   // %-return (USD-scalable)
            const double pct_real = (L.entry != 0.0) ? (p_real / L.entry) * 100.0 : 0.0;
            fwd_[fi][ti].pts += pct; fwd_[fi][ti].pts_real += pct_real;
            fwd_[fi][ti].clips += 1; fwd_[fi][ti].wins += (pct_real > 1e-9 ? 1 : 0);
            save_fwd_book_();
            // completed trade -> push to the trades list, persist, then reset leg below.
            const double usd_per_pct = cfg_.notional / 100.0 * cfg_.lot;
            Closed rec{fi, ti, L.entry, fill, pct, pct * usd_per_pct, L.entry_ts, ts_sec, reason,
                       pct_real, pct_real * usd_per_pct};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[AUFX][CLOSE] %s %s entry=%.5f fill=%.5f pct_model=%.4f pct_real=%.4f (%s)\n",
                        leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", L.entry, fill, pct, pct_real, reason);
            std::fflush(stdout);
        }
        L.has_entry = false; L.wm = 0; L.token.clear(); L.entry_ts = 0;
        save_live_state_();   // persist the leg reset NOW: a crash before the end-of-step snapshot must
                              // not resurrect this leg on restart and double-book the clip (book+CSV+ledger).
    }

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
        f << r.fi << "," << r.ti << "," << r.entry << "," << r.exit << "," << r.pct << ","
          << r.usd << "," << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << ","
          << r.pct_real << "," << r.usd_real << "\n";
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
                                      &r.fi, &r.ti, &r.entry, &r.exit, &r.pct, &r.usd,
                                      &ets, &xts, reason, &preal, &ureal);
            if (n >= 8 && r.fi >= 0 && r.fi < 2 && r.ti >= 0 && r.ti < NT_) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                // pre-fix rows carry only the model figure; display it in both columns (marked by
                // the row's position before the honest-accounting deploy; real accrual is forward-only).
                r.pct_real = (n >= 11) ? preal : r.pct;
                r.usd_real = (n >= 11) ? ureal : r.usd;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

    // ── persist / restore OPEN arm-state (window flags + open legs) — same fix as GoldBeFloorCompanion:
    //   win_/live_ were RAM-only, so every VPS restart wiped an in-progress 2h window/legs and the pair
    //   re-zeroed to idle. Snapshot on every live bar (atomic tmp+rename), reload at construction. ──
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
            } else { std::string rest; std::getline(f, rest); }
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
    // Append one live forward bar to the persist CSV (ts_sec,close). Append-only; reloaded
    // by seed_dump() on the next boot. Best-effort -- a failed open silently no-ops (shadow book).
    void append_dump_(int64_t ts_sec, double close) const noexcept {
        std::ofstream f(cfg_.bars_path, std::ios::app);
        if (f.is_open()) f << (long long)ts_sec << "," << close << "\n";
    }
    static int64_t norm_ts_(int64_t ts) noexcept { return ts >= 100000000000LL ? ts / 1000 : ts; }
    static std::string lower_(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }

    void dedup_sort_() noexcept {
        const size_t n = ts_.size();
        if (n < 2) return;
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b){ return ts_[a] < ts_[b]; });
        std::vector<int64_t> nts; std::vector<double> nc; nts.reserve(n); nc.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            const size_t i = idx[k];
            if (k + 1 < n && ts_[idx[k + 1]] == ts_[i]) continue;   // keep last of an equal-ts run
            nts.push_back(ts_[i]); nc.push_back(c_[i]);
        }
        ts_.swap(nts); c_.swap(nc);
    }

    // ── detector: faithful parent() (up / down 2h jump windows) ──
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

    // ── BE-floor clip book: faithful leg_book() (deploy-forward gated, PERCENT). ──
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
                        const double g = std::max(0.0, (stop / entry - 1.0) * 1e4); // bp
                        if (t_ok_(i)) { r.pct += g / 100.0; ++r.clips; r.wins += (g > 1e-6); }
                        has_entry = false; wm = 0.0; ref = stop;                  // reclip from exit
                    }
                } else {
                    if (cur < wm) wm = cur;
                    const double stop = std::min(entry, wm * (1.0 + gb / 1e4));   // stop <= entry
                    if (cur >= stop) {
                        const double g = std::max(0.0, (1.0 - stop / entry) * 1e4);
                        if (t_ok_(i)) { r.pct += g / 100.0; ++r.clips; r.wins += (g > 1e-6); }
                        has_entry = false; wm = 0.0; ref = stop;
                    }
                }
            }
            if (has_entry) {                                                      // window ended open -> flush
                const double last = (t.xi - 1 >= t.ei) ? c_[t.xi - 1] : c_[t.ei];
                const double g = is_long ? std::max(0.0, (last / entry - 1.0) * 1e4)
                                         : std::max(0.0, (1.0 - last / entry) * 1e4);
                if (ts_ok_(t.xi)) { r.pct += g / 100.0; ++r.clips; r.wins += (g > 1e-6); }
            }
        }
        return r;
    }
    bool t_ok_(int i)  const { return ts_[i] > deploy_ts_; }
    bool ts_ok_(int x) const { return ts_[x] > deploy_ts_; }
};

// ── registry: owns the pairs, writes the merged aggregate fx_companion_state.json ──
//   (served by /api/fx_companion -> desk FX COMPANIONS panel). Mirrors the gold
//   companion's single-file publish. Atomic remove-then-rename (Windows-safe).
class FxBeFloorBook {
public:
    void add(FxBeFloorPair::Config c) { pairs_.emplace_back(std::move(c)); }

    FxBeFloorPair* find(const std::string& pair) {
        for (auto& p : pairs_) if (p.pair() == pair) return &p;
        return nullptr;
    }

    size_t seed_pair(const std::string& pair, const std::string& csv) {
        if (auto* p = find(pair)) return p->seed_from_h1_csv(csv);
        return 0;
    }
    // Reload every pair's persisted LIVE forward bars (call AFTER warmup seed, BEFORE finalize_all)
    // so the book is restored across restarts. Returns total forward bars restored.
    size_t seed_dumps_all() { size_t n = 0; for (auto& p : pairs_) n += p.seed_dump(); return n; }
    // Wire the live order path into EVERY pair (call after add(), before live). Makes each
    // <PAIR>Pos/<PAIR>Neg a real 2-runner engine -> trades flow to the shadow ledger + PnL.
    void set_exec(FxBeFloorPair::OpenFn o, FxBeFloorPair::CloseFn c,
                  FxBeFloorPair::GateFn g, FxBeFloorPair::LedgerFn l) {
        for (auto& p : pairs_) p.set_exec(o, c, g, l);
    }
    void finalize_all() { for (auto& p : pairs_) p.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed H1 bar for `pair`. Rewrites the aggregate.
    void on_h1_bar(const std::string& pair, int64_t ts_sec, double close) {
        if (auto* p = find(pair)) { p->on_h1_bar(ts_sec, close); recompute_and_write(); }
    }

    std::string state_json() const {
        std::ostringstream o;
        int64_t last_ts = 0;
        for (const auto& p : pairs_) last_ts = std::max(last_ts, p.last_ts());
        o << "{\"engine\":\"fx-befloor-pos-neg\",\"shadow\":true,\"pairs\":[";
        for (size_t i = 0; i < pairs_.size(); ++i) {
            if (i) o << ",";
            o << pairs_[i].pair_json();
        }
        o << "],\"ts\":" << (long long)last_ts << "}";
        return o.str();
    }

    void recompute_and_write() const {
        const std::string js = state_json();
        const std::string tmp = state_path_ + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return; f << js; }
#if defined(_WIN32)
        std::remove(state_path_.c_str());
#endif
        std::rename(tmp.c_str(), state_path_.c_str());
    }

private:
    std::vector<FxBeFloorPair> pairs_;
    std::string state_path_ = "fx_companion_state.json";   // served by /api/fx_companion (cwd = C:\Omega)
};

// Singleton — accessor mirrors omega::gold_befloor_companion().
inline FxBeFloorBook& fx_befloor_book() noexcept {
    static FxBeFloorBook inst;
    return inst;
}

} // namespace omega
