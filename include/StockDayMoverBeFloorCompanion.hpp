#pragma once
// =============================================================================
// StockDayMoverBeFloorCompanion — per-NAME <SYM>Pos (up-mover long) + <SYM>Neg
// (down-mover short) BE-floor books for the RDAgent BIGCAP single-stock universe.
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math   : tools/rdagent/daymover_befloor_x3_v2.py + daymover_pername_screen.py
//            (day-mover detector: +/-3% day arms a parent window; leg_book() BE-floor
//            walk) — the SAME BE-floor mechanism as gold/xag/usoil/fx/index, DAILY-tuned.
//   screen : every BIGCAP name is net>0, gross-neg=0, both WF halves + on its OWN book
//            (r150 gate). Edge survives de-survivorship (457/529 SP500 names also net>0).
//
// Multi-name like IndexBeFloorCompanion (own aggregate state file + /api endpoint), BUT
// books in RETURN units (not price-points): equities have no fixed $/pt, and the research
// itself measures each clip as stop/entry-1 (a return). USD = return * fixed notional/clip
// (default $10k, shadow-illustrative; operator rescales). This is the correct name-agnostic
// analog of the gold/index $/pt convention across a $30..$1000 price range.
//
// FEED (the equity-specific part): NO single-stock intraday exists in /Users/jo/Tick, so the
// trail is DAILY-CLOSE grade. Live daily closes arrive from the external RDAgent refresh
// (tools/rdagent/refresh_close_ibkr.py, IBKR port 4002) which appends to
// data/rdagent/sp500_long_close.csv. A background poller thread (mirrors OmegaHotReload)
// re-reads that wide CSV, dispatches each NEW date's closes to every name's on_daily_bar.
// r20 (0.2%) is DAILY-coarse ("exit first down-close"); r150/r400 are > daily-noise and robust.
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-engine).
// Observe-only, shadow: NEVER opens / moves / shrinks / closes a real position, NEVER read by
// any parent. Judge STANDALONE (net>0, both WF halves) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: RETIRED S-2026-07-07e (real-fill re-validation). Faithful daily-close
//   replay over data/rdagent/sp500_long_close.csv 2019-06..2026-06 (rt=8bp, $10k notional):
//   39-name book -$110.7k real vs +$1.57M model; Neg flavor -$325k at every thr 3/4/5%;
//   Pos-only +$214k but its 2019-2022 half (contains the 2020-21 bull) is negative at every
//   thr -- daily granularity + overnight gaps eat the trail. No names wired in engine_init
//   (empty aggregate). Evidence outputs/BEFLOOR_FAMILY_REALFILL_2026-07-07.txt · registry §5.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>
#include <deque>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cmath>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

// ── one stock name's Pos/Neg BE-floor book (returns-based) ───────────────────
class StockMoverSym {
public:
    struct Config {
        std::string sym      = "NVDA";    // ticker -> flavor names <sym>Pos/<sym>Neg + persist paths
        std::string live_sym = "NVDA";    // symbol the live legs trade (order path)
        int    W             = 1;         // detector window (daily bars) -> "1-day move"
        double thr           = 0.03;      // day-mover arm threshold (3%; research best, both WF halves +)
        double be_bp         = 10.0;      // RT cost floor (bp) to open a leg (single-name IBKR ~3-8bp)
        double rt_cost_bp    = 8.0;       // REAL round-trip cost (bp of entry) debited from every clip's
                                          //   ret_real. be_bp only DELAYS the arm; it is not a cost credit —
                                          //   a floor exit at entry is a real -rt_cost_bp.
        double min_gb_mult= 3.0;      // TIER VIABILITY GATE: a tier arms only if its giveback
                                      //   LIVE_GB_[ti] >= min_gb_mult * rt_cost_bp -- a trail whose
                                      //   giveback is within a few multiples of the round-trip cost
                                      //   cannot clear costs on its typical clip. Non-viable tiers
                                      //   never open (open legs still managed to close); shown as
                                      //   "viable":false in the state JSON. 0 disables the gate.
        double notional      = 10000.0;   // $ per clip; USD = return * notional (name-agnostic sizing)
        double lot           = 1.0;       // order-path lot (shares/CFD units decided at flip)
        std::string deploy_path;          // per-name persisted deploy-forward anchor
        std::string bars_path;            // per-name persisted LIVE forward daily bars (survives restart)
        std::string book_path;            // per-name persisted REAL forward book (3 tiers x 2 dirs)
        std::string live_path;            // per-name persisted OPEN window+leg arm-state (survives restart)
        std::string closed_path;          // per-name persisted CLOSED forward trades log
    };

    // ── LIVE EXECUTION WIRING — identical contract to Index/Gold/FxBeFloorCompanion. Set ONLY
    //   in the live main TU; null in the backtest TU -> live_step_ short-circuits (pure
    //   accounting, canary unaffected). SHADOW today (send_live_order no-ops while mode!=LIVE),
    //   LIVE on flip. Each tier is its OWN position -> OWN ledger row -> headline PnL. ──
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit StockMoverSym(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "stockmover_companion_" + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "stockmover_companion_" + s + "_daily.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = "stockmover_companion_" + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = "stockmover_companion_" + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "stockmover_companion_" + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();
        load_live_state_();
    }

    const std::string& sym() const { return cfg_.sym; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_ret() const {   // MODEL total forward return summed across dirs+tiers (headline fold)
        double r = 0.0; for (int fi=0; fi<2; ++fi) for (int ti=0; ti<NT_; ++ti) r += fwd_[fi][ti].ret; return r;
    }
    double  book_usd() const { return book_ret() * cfg_.notional; }
    double  book_ret_real() const {   // REAL total (observed fills - cost) — the judgeable headline
        double r = 0.0; for (int fi=0; fi<2; ++fi) for (int ti=0; ti<NT_; ++ti) r += fwd_[fi][ti].ret_real; return r;
    }
    double  book_usd_real() const { return book_ret_real() * cfg_.notional; }

    // seed one historical daily close (primes the detector history; books nothing — deploy-forward gate).
    void seed_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ingest_(norm_ts_(ts_sec), close);
    }

    // Call ONCE after all seeding: stamp+persist deploy_ts on first-ever boot; load anchor on restart.
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
        }
    }

    // LIVE feed: one CLOSED daily bar for this name (close = official daily close).
    void on_daily_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only (idempotent re-poll)
        // DATA-INTEGRITY GUARD (live path only): a torn read of the externally-written wide CSV
        // ("45.67" truncated to "4.5") or an unadjusted split parses as a valid >0 close and would
        // arm a phantom day-mover window PERMANENTLY (the corrected same-date row is then rejected
        // as stale). Reject any single-day move > 50% and log loudly — a genuine >50% BIGCAP day
        // is far rarer than a torn read/split, and integrity wins (CLAUDE.md x1000-glitch class).
        if (!c_.empty()) {
            const double jump = std::fabs(close / c_.back() - 1.0);
            if (jump > 0.50) {
                std::printf("[AUSTK][REJECT] %s daily close %.4f vs prev %.4f (%.0f%% jump) — torn read/split? bar dropped\n",
                            cfg_.sym.c_str(), close, c_.back(), jump * 100.0);
                std::fflush(stdout);
                return;
            }
        }
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);
        live_step_(ts_sec);
    }

    // Reload persisted LIVE forward daily bars (written by on_daily_bar). Books nothing new
    // (deploy-forward gate); the recompute replays these so the book survives restart.
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

    // Emit this name's desk JSON object. REAL FORWARD TRADES ONLY ($0 until first live clip).
    // Books are RETURNS: "ret" = summed return fraction, "pct" = ret*100, "usd" = ret*notional.
    std::string sym_json() const {
        const double notl = cfg_.notional;
        const char* TIER_TAG[NT_] = { "banker", "runner", "wide", "r50", "r100" };
        struct Flavor { const char* suffix; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"Pos", "long", true}, {"Neg", "short", false} };
        const double cur = c_.empty() ? 0.0 : c_.back();

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double sym_ret = 0.0, sym_ret_real = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            double book_ret = 0.0, book_ret_real = 0.0; int fwd_clips = 0, fwd_wins = 0;
            std::ostringstream runs;
            for (int ti = 0; ti < NT_; ++ti) {
                const FwdBook& b = fwd_[fi][ti];
                book_ret += b.ret; book_ret_real += b.ret_real; fwd_clips += b.clips; fwd_wins += b.wins;
                if (ti) runs << ",";
                runs.precision(0); runs << std::fixed;
                runs << "{\"tier\":\"" << TIER_TAG[ti] << "\",\"gb_bp\":" << (long)LIVE_GB_[ti]
                     << ",\"viable\":" << (tier_viable_(ti) ? "true" : "false")
                     << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                runs.precision(3); runs << "\"pct\":" << (b.ret * 100.0)
                                        << ",\"pct_real\":" << (b.ret_real * 100.0) << ",";
                runs.precision(0); runs << "\"usd\":" << (b.ret * notl)
                                        << ",\"usd_real\":" << (b.ret_real * notl) << "}";
            }
            sym_ret += book_ret; sym_ret_real += book_ret_real;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << cfg_.sym << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"clips\":" << fwd_clips << ",\"wins\":" << fwd_wins << ",";
            fl.precision(3); fl << "\"book_pct\":" << (book_ret * 100.0)
                                << ",\"book_pct_real\":" << (book_ret_real * 100.0) << ",";
            fl.precision(0); fl << "\"book_usd\":" << (book_ret * notl)
               << ",\"book_usd_real\":" << (book_ret_real * notl)
               << ",\"runners\":[" << runs.str() << "]}";
        }

        // OPEN legs right now (empty = idle/reset).
        std::ostringstream op; int nopen = 0;
        for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) {
            const LiveLeg& L = live_[fi][ti];
            if (!L.has_entry) continue;
            const bool up = (fi == 0);
            // short uPnL on the same notional basis as long: (entry-cur)/entry (NOT entry/cur-1).
            const double u  = up ? (cur / L.entry - 1.0) : (L.entry - cur) / L.entry;
            const double ur = u - cfg_.rt_cost_bp / 1e4;   // real uPnL (cost debited)
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << cfg_.sym << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"tier\":\"" << TIER_TAG[ti] << "\",";
            op.precision(2);
            op << "\"entry\":" << L.entry << ",\"wm\":" << L.wm << ",\"cur\":" << cur << ",";
            op.precision(3); op << "\"upnl_pct\":" << (u * 100.0)
                                << ",\"upnl_pct_real\":" << (ur * 100.0) << ",";
            op.precision(0); op << "\"upnl_usd\":" << (u * notl) << ",\"entry_ts\":" << (long long)L.entry_ts << "}";
        }

        // CLOSED forward trades log — most-recent first.
        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c = *it;
            const int cfi = (c.fi >= 0 && c.fi < 2) ? c.fi : 0;
            if (ntr++) tr << ",";
            tr.precision(0); tr << std::fixed;
            tr << "{\"flavor\":\"" << cfg_.sym << flavors[cfi].suffix << "\",\"dir\":\""
               << flavors[cfi].dir << "\",\"tier\":\"" << TIER_TAG[(c.ti >= 0 && c.ti < NT_) ? c.ti : 0] << "\",";
            tr.precision(2);
            tr << "\"entry\":" << c.entry << ",\"exit\":" << c.exit << ",";
            tr.precision(3); tr << "\"pct\":" << (c.ret * 100.0)
                                << ",\"pct_real\":" << (c.ret_real * 100.0) << ",";
            tr.precision(0);
            tr << "\"usd\":" << c.usd << ",\"usd_real\":" << c.usd_real
               << ",\"reason\":\"" << c.reason << "\",\"entry_ts\":" << (long long)c.ets
               << ",\"exit_ts\":" << (long long)c.xts << "}";
        }

        o << "{\"sym\":\"" << cfg_.sym << "\",\"live_sym\":\"" << cfg_.live_sym << "\",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_ << ",\"ts\":" << (long long)last_ts << ",";
        o.precision(0); o << "\"notional\":" << notl << ",";
        o.precision(1); o << "\"rt_cost_bp\":" << cfg_.rt_cost_bp << ",";
        o.precision(3); o << "\"pct\":" << (sym_ret * 100.0)
                          << ",\"pct_real\":" << (sym_ret_real * 100.0) << ",";
        o.precision(0); o << "\"usd\":" << (sym_ret * notl)
                          << ",\"usd_real\":" << (sym_ret_real * notl) << ",";
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

    // ── live execution — tiers per direction, BE-floored (arm only once price covers be_bp;
    //   trail floor pinned >= entry long / <= entry short). The floor bounds the MODEL column
    //   only: real fills book worse-of(floor, observed close) minus rt_cost_bp, so ret_real
    //   CAN be negative — at daily grade the overnight gap through the floor is the normal
    //   adverse case for ±3% day-movers. ──
    static constexpr int    NT_ = 5;   // r20 banker / r150 runner / r400 wide / r50 / r100
    static constexpr double LIVE_GB_[NT_] = { 20.0, 150.0, 400.0, 50.0, 100.0 };  // 50/100 APPENDED at end (persistence keys by index -> never reorder)
    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    bool     win_[2]      = { false, false };
    bool     win_pend_[2] = { false, false };   // signal fired; ref anchors at the NEXT bar's close
                                                //   (parity with daymover_pername_screen.py ei = i+1)
    struct LiveLeg { bool has_entry = false; double entry = 0, wm = 0, ref = 0; int64_t entry_ts = 0; std::string token; };
    LiveLeg live_[2][NT_];
    struct FwdBook { double ret = 0.0; int clips = 0; int wins = 0; double ret_real = 0.0; };
    FwdBook  fwd_[2][NT_];   // .ret = MODEL summed return fraction (fill-at-floor, no cost);
                             // .ret_real = REAL (observed fill - rt_cost_bp). wins counts REAL wins.

    struct Closed { int fi = 0, ti = 0; double entry = 0, exit = 0, ret = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason;
                    double ret_real = 0, usd_real = 0; };   // REAL columns (observed fill - cost)
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;
    std::string leg_engine_(int fi, int ti) const {
        return cfg_.sym + (fi == 0 ? "Pos" : "Neg") + ("_r" + std::to_string((long)LIVE_GB_[ti]));
    }

    // Incremental live BE-floor state machine on the NEWEST bar (returns-based; multiplicative
    // stop/be/jump identical to Index/Gold/Fx; only the booked quantity is a RETURN not price-pts).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                               // backtest TU / not wired: pure accounting
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N <= W) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];
        const double j   = c_[N - 1] / c_[N - 1 - W] - 1.0;
        // GAP GUARD: daily bars normally span weekends/holidays (in-calibration), but a multi-week
        // data outage would make the "1-day move" a multi-week move — block NEW windows on a span
        // over ~1 week; exits stay honoured.
        const bool contig = (ts_[N - 1] - ts_[N - 1 - W]) <= (int64_t)W * 86400 * 7;
        const double thr = cfg_.thr, be = cfg_.be_bp;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            const bool enter = contig && (up ? (j >=  thr) : (j <= -thr));
            const bool exit  = up ? (j <= -thr) : (j >=  thr);
            if (win_pend_[fi]) {                              // window opens the bar AFTER the mover day:
                win_pend_[fi] = false; win_[fi] = true;       // ref = c[ei] (parity with research ei = i+1)
                for (int ti = 0; ti < NT_; ++ti) {
                    LiveLeg& L = live_[fi][ti]; L.has_entry = false; L.wm = 0; L.ref = cur;
                }
            }
            if (!win_[fi] && !win_pend_[fi] && enter) win_pend_[fi] = true;   // day-mover window signal
            for (int ti = 0; ti < NT_ && win_[fi]; ++ti) {
                LiveLeg& L = live_[fi][ti];
                const double gb = LIVE_GB_[ti];
                if (!L.has_entry) {
                    if (!tier_viable_(ti)) continue;   // weeded out: giveback < min_gb_mult x real cost
                    const bool cond = up ? ((cur / L.ref - 1.0) * 1e4 >= be) : ((1.0 - cur / L.ref) * 1e4 >= be);
                    if (cond) {
                        const double tp_dist_pts = cur * (gb / 1e4);
                        const bool viable = !gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot);
                        if (viable) {
                            L.entry = cur; L.wm = cur; L.has_entry = true; L.entry_ts = ts_sec;
                            if (fwd) {
                                L.token = open_fn_(cfg_.live_sym, up, cfg_.lot, cur);
                                std::printf("[AUSTK][OPEN] %s %s @%.2f lot=%.2f tok=%s\n",
                                            leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", cur, cfg_.lot, L.token.c_str());
                                std::fflush(stdout);
                            }
                        }
                    }
                } else {
                    double stop; bool hit;
                    if (up) { if (cur > L.wm) L.wm = cur; stop = std::max(L.entry, L.wm * (1.0 - gb / 1e4)); hit = (cur <= stop); }
                    else    { if (cur < L.wm) L.wm = cur; stop = std::min(L.entry, L.wm * (1.0 + gb / 1e4)); hit = (cur >= stop); }
                    // REAL FILL: the floor is an order target; the daily close is the only tradable
                    // mark. Book at the WORSE of the two (an overnight gap books the gap, not the wish).
                    if (hit) { close_leg_(fi, ti, up, stop, cur, ts_sec, fwd, "TRAIL_STOP"); L.ref = stop; }
                }
            }
            if (win_[fi] && exit) {
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; if (L.has_entry) close_leg_(fi, ti, up, cur, cur, ts_sec, fwd, "WINDOW_EXIT"); }
                win_[fi] = false;
            }
        }
        save_live_state_();
    }

    // TIER VIABILITY: giveback must exceed the instrument's real RT cost by min_gb_mult.
    bool tier_viable_(int ti) const noexcept {
        return cfg_.min_gb_mult <= 0.0 || LIVE_GB_[ti] >= cfg_.min_gb_mult * cfg_.rt_cost_bp;
    }
    // px_floor = model floor/stop level; px_obs = observed daily close (the only tradable mark).
    // REAL booking: fill = worse-of(floor, observed) per side, cost = rt_cost_bp of notional.
    // NOTE short return is (entry - px) / entry — return on the SAME notional basis as the long
    // side. The old entry/px - 1 convexly overstated short winners (a 100->50 cover booked +100%).
    void close_leg_(int fi, int ti, bool up, double px_floor, double px_obs,
                    int64_t ts_sec, bool fwd, const char* reason) noexcept {
        LiveLeg& L = live_[fi][ti];
        const double fill = up ? std::min(px_floor, px_obs) : std::max(px_floor, px_obs);
        if (fwd) {
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.live_sym, up, cfg_.lot, fill, L.token);
            // Ledger records the REAL fill — the shadow ledger must be able to disagree with the model.
            if (ledger_fn_) ledger_fn_(leg_engine_(fi, ti), cfg_.live_sym, up, L.entry, fill, cfg_.lot, L.entry_ts, ts_sec, reason);
            const double r      = up ? (px_floor / L.entry - 1.0)
                                     : (L.entry - px_floor) / L.entry;            // MODEL (>=0 by algebra)
            const double r_real = (up ? (fill / L.entry - 1.0) : (L.entry - fill) / L.entry)
                                  - cfg_.rt_cost_bp / 1e4;                        // REAL (can be negative)
            fwd_[fi][ti].ret += r; fwd_[fi][ti].ret_real += r_real;
            fwd_[fi][ti].clips += 1; fwd_[fi][ti].wins += (r_real > 1e-9 ? 1 : 0);
            save_fwd_book_();
            Closed rec{fi, ti, L.entry, fill, r, r * cfg_.notional, L.entry_ts, ts_sec, reason,
                       r_real, r_real * cfg_.notional};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[AUSTK][CLOSE] %s %s entry=%.2f fill=%.2f ret_model=%.4f ret_real=%.4f (%s)\n",
                        leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", L.entry, fill, r, r_real, reason);
            std::fflush(stdout);
        }
        L.has_entry = false; L.wm = 0; L.token.clear(); L.entry_ts = 0;
        save_live_state_();   // persist the leg reset NOW: a crash before the end-of-step snapshot must
                              // not resurrect this leg on restart and double-book the clip (book+CSV+ledger).
    }

    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int fi = -1, ti = -1; double ret = 0; int clips = 0, wins = 0; double rreal = 0;
        std::string line;
        while (std::getline(f, line)) {
            // 6-field "fi ti ret clips wins ret_real"; old 5-field rows load with ret_real=0
            // (real accounting accrues forward from the honest-accounting deploy).
            const int n = std::sscanf(line.c_str(), "%d %d %lf %d %d %lf", &fi, &ti, &ret, &clips, &wins, &rreal);
            if (n >= 5 && fi >= 0 && fi < 2 && ti >= 0 && ti < NT_) {
                fwd_[fi][ti].ret = ret; fwd_[fi][ti].clips = clips; fwd_[fi][ti].wins = wins;
                fwd_[fi][ti].ret_real = (n >= 6) ? rreal : 0.0;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti)
              f << fi << " " << ti << " " << fwd_[fi][ti].ret << " " << fwd_[fi][ti].clips << " "
                << fwd_[fi][ti].wins << " " << fwd_[fi][ti].ret_real << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.fi << "," << r.ti << "," << r.entry << "," << r.exit << "," << r.ret << ","
          << r.usd << "," << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << ","
          << r.ret_real << "," << r.usd_real << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            long long ets = 0, xts = 0; double rreal = 0, ureal = 0;   // no aliasing casts on int64_t
            const int n = std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n],%lf,%lf",
                                      &r.fi, &r.ti, &r.entry, &r.exit, &r.ret, &r.usd,
                                      &ets, &xts, reason, &rreal, &ureal);
            if (n >= 8 && r.fi >= 0 && r.fi < 2 && r.ti >= 0 && r.ti < NT_) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                r.ret_real = (n >= 11) ? rreal : r.ret;   // pre-fix rows: model only (display fallback)
                r.usd_real = (n >= 11) ? ureal : r.usd;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

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
};

// ── registry: owns the names, seeds/polls the wide daily-close CSV, writes the merged
//   stockmover_companion_state.json (served by /api/stockmover_companion). ──
class StockMoverBook {
public:
    ~StockMoverBook() { stop_poller(); }

    void add(StockMoverSym::Config c) {
        col_[c.sym] = syms_.size();   // name -> index (also the wide-CSV column lookup key)
        syms_.emplace_back(std::move(c));
    }

    StockMoverSym* find(const std::string& sym) {
        auto it = col_.find(sym);
        return (it == col_.end()) ? nullptr : &syms_[it->second];
    }

    // Wire the live order path into EVERY name (call after add(), before finalize).
    void set_exec(StockMoverSym::OpenFn o, StockMoverSym::CloseFn c,
                  StockMoverSym::GateFn g, StockMoverSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }

    // ── warm-seed from the WIDE daily-close CSV (data/rdagent/sp500_long_close.csv):
    //   header row = ",NAME1,NAME2,...", each data row = "YYYY-MM-DD,close1,close2,...".
    //   For every configured name present, dispatch its column as historical daily bars.
    //   Also records the last date seeded -> the live poller resumes from there. ──
    size_t seed_from_wide_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[AUSTK][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string header;
        if (!std::getline(f, header)) return 0;
        // map csv-column-index -> sym-index (only for names we track)
        std::unordered_map<int, size_t> colsym;
        { std::stringstream hs(header); std::string tok; int ci = 0;
          while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = it->second; ++ci; } }
        std::string line; size_t rows = 0;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const int64_t ts = parse_date_ts_(line);
            if (ts <= 0) continue;
            dispatch_row_(line, colsym, ts, /*live=*/false);
            if (ts > last_seed_ts_) last_seed_ts_ = ts;
            ++rows;
        }
        std::printf("[AUSTK][SEED] wide-csv %s: %zu rows, %zu/%zu names mapped, last=%lld\n",
                    path.c_str(), rows, colsym.size(), syms_.size(), (long long)last_seed_ts_);
        std::fflush(stdout);
        return rows;
    }

    size_t seed_dumps_all() { size_t n = 0; for (auto& s : syms_) n += s.seed_dump(); return n; }
    void   finalize_all() { for (auto& s : syms_) s.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed daily bar for `sym`. (Poller calls the row variant; this is for tests.)
    void on_daily_bar(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* s = find(sym)) { s->on_daily_bar(ts_sec, close); recompute_unlocked_(); }
    }

    // ── background poller: re-read the wide CSV every poll_ms; dispatch every date newer than
    //   last_seen_ (handles gaps + is idempotent — each sym's on_daily_bar drops stale ts).
    //   Mirrors OmegaHotReload's watch thread. Process-lifetime; stopped in dtor. ──
    void start_poller(const std::string& rel, int poll_ms = 900000 /*15min*/) {
        csv_rel_ = rel; poll_ms_ = poll_ms;
        last_seen_ts_ = last_seed_ts_;   // resume forward from the last seeded date
        // GUARD: if the boot seed found NO file (last_seed_ts_==0 -> deploy_ts never stamped),
        // the FIRST poller load must run as a SEED (history only), not book years of history as
        // fake forward PnL. Set the flag so poll_loop_ seeds+stamps once, then goes live.
        seed_missed_ = (last_seed_ts_ == 0);
        running_.store(true);
        thread_ = std::thread([this]{ poll_loop_(); });
        std::printf("[AUSTK][POLL] watching %s (poll=%dms, resume-from=%lld)\n",
                    rel.c_str(), poll_ms_, (long long)last_seen_ts_);
        std::fflush(stdout);
    }
    void stop_poller() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    std::string state_json() const { std::lock_guard<std::mutex> lk(mu_); return state_json_unlocked_(); }
    void recompute_and_write() const { std::lock_guard<std::mutex> lk(mu_); recompute_unlocked_(); }
    double total_usd() const {
        std::lock_guard<std::mutex> lk(mu_);
        double t = 0.0; for (const auto& s : syms_) t += s.book_usd(); return t;
    }
    double total_usd_real() const {
        std::lock_guard<std::mutex> lk(mu_);
        double t = 0.0; for (const auto& s : syms_) t += s.book_usd_real(); return t;
    }

private:
    // ── all mutation/read of syms_ after start_poller() goes through mu_: the poller thread
    //   dispatches bars + rewrites state while the main thread may query totals/state. NOTE the
    //   exec callbacks (open/close/gate/ledger) are invoked FROM the poller thread — before any
    //   LIVE flip the wired functions must themselves be thread-safe (shadow no-ops are). ──
    mutable std::mutex mu_;

    std::string state_json_unlocked_() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot_usd = 0.0, tot_usd_real = 0.0;
        for (const auto& s : syms_) { last_ts = std::max(last_ts, s.last_ts()); tot_usd += s.book_usd(); tot_usd_real += s.book_usd_real(); }
        o << "{\"engine\":\"stockmover-befloor-pos-neg\",\"shadow\":true,\"grade\":\"daily-close\",";
        o.precision(0); o << std::fixed << "\"total_usd\":" << tot_usd
                          << ",\"total_usd_real\":" << tot_usd_real << ",\"names\":[";
        for (size_t i = 0; i < syms_.size(); ++i) { if (i) o << ","; o << syms_[i].sym_json(); }
        o << "],\"ts\":" << (long long)last_ts << "}";
        return o.str();
    }

    void recompute_unlocked_() const {
        const std::string js = state_json_unlocked_();
        const std::string tmp = state_path_ + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return; f << js; }
#if defined(_WIN32)
        std::remove(state_path_.c_str());
#endif
        std::rename(tmp.c_str(), state_path_.c_str());
    }

    std::vector<StockMoverSym> syms_;
    std::unordered_map<std::string, size_t> col_;     // name -> sym index
    std::string state_path_ = "stockmover_companion_state.json";
    int64_t last_seed_ts_ = 0;

    // poller
    std::string      csv_rel_;
    int              poll_ms_ = 900000;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> last_seen_ts_{0};
    bool             seed_missed_ = false;   // boot-seed found no CSV -> first poll load seeds, not books
    std::thread      thread_;

    // parse leading "YYYY-MM-DD" of a CSV line -> epoch seconds (UTC midnight). 0 on failure.
    static int64_t parse_date_ts_(const std::string& line) noexcept {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(line.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
        return days_from_civil_(y, m, d) * 86400LL;
    }
    // Howard Hinnant's days_from_civil (proleptic Gregorian, 1970-01-01 = 0).
    static int64_t days_from_civil_(int y, unsigned m, unsigned d) noexcept {
        y -= (m <= 2);
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097LL + (int64_t)doe - 719468LL;
    }

    // dispatch one wide-CSV data row's closes to the mapped names. live=true -> on_daily_bar (books
    // forward); live=false -> seed_bar (history only). Empty/NaN fields skipped.
    void dispatch_row_(const std::string& line, const std::unordered_map<int, size_t>& colsym,
                       int64_t ts, bool live) {
        std::stringstream ls(line); std::string tok; int ci = 0;
        while (std::getline(ls, tok, ',')) {
            auto it = colsym.find(ci);
            if (it != colsym.end() && !tok.empty()) {
                char* end = nullptr; const double v = std::strtod(tok.c_str(), &end);
                if (end != tok.c_str() && v > 0.0) {
                    if (live) syms_[it->second].on_daily_bar(ts, v);
                    else      syms_[it->second].seed_bar(ts, v);
                }
            }
            ++ci;
        }
    }

    void poll_loop_() {
        // rebuild the column map once (header is stable)
        while (running_.load()) {
            for (int slept = 0; slept < poll_ms_ && running_.load(); slept += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!running_.load()) break;
            const std::string path = omega::resolve_seed_path(csv_rel_);
            std::ifstream f(path);
            if (!f.is_open()) continue;
            std::string header;
            if (!std::getline(f, header)) continue;
            std::unordered_map<int, size_t> colsym;
            { std::stringstream hs(header); std::string tok; int ci = 0;
              while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = it->second; ++ci; } }
            const int64_t seen = last_seen_ts_.load();
            const bool as_seed = seed_missed_;   // first load after a missed boot-seed = SEED, not live
            int64_t newest = seen; int fresh = 0;
            std::string line;
            {
                std::lock_guard<std::mutex> lk(mu_);   // dispatch mutates syms_ from this thread
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    const int64_t ts = parse_date_ts_(line);
                    if (!as_seed && ts <= seen) continue;     // live: already processed
                    dispatch_row_(line, colsym, ts, /*live=*/!as_seed);
                    if (ts > newest) newest = ts;
                    ++fresh;
                }
                if (fresh > 0 && as_seed) {
                    // seeded the history (booked nothing); stamp deploy_ts so only genuinely-new dates book.
                    for (auto& s : syms_) s.finalize_seed();
                    last_seen_ts_.store(newest); seed_missed_ = false;
                    recompute_unlocked_();
                } else if (fresh > 0) {
                    last_seen_ts_.store(newest);
                    recompute_unlocked_();
                }
            }
            if (fresh > 0 && as_seed) {
                std::printf("[AUSTK][POLL] first-load SEED %d rows (boot-seed missed), stamped deploy=%lld\n",
                            fresh, (long long)newest);
                std::fflush(stdout);
            } else if (fresh > 0) {
                std::printf("[AUSTK][POLL] %d new daily row(s), newest=%lld\n", fresh, (long long)newest);
                std::fflush(stdout);
            }
        }
    }
};

// Singleton — accessor mirrors omega::index_befloor_book().
inline StockMoverBook& stockmover_befloor_book() noexcept {
    static StockMoverBook inst;
    return inst;
}

} // namespace omega
