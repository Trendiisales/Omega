#pragma once
// =============================================================================
// BigCap2pctImpulseCompanion — per-NAME BIGCAP +2%-impulse / 20-day-breakout
// LONG-only LOOSE-RIDE book. A SEPARATE INDEPENDENT engine (not the up-jump
// ladder): one position per name, entered on a strong impulse day, ridden with
// a DELIBERATELY LOOSE 3-layer exit so a big continuation runs to the end.
//
//   ENTRY (ungated, LONG-only): the official daily close is >= +thr (+2%)
//     close-to-close AND is a NEW 20-day closing high (impulse + continuation).
//     NO bull/bear/200-day regime gate — the book fires in every regime.
//   EXIT (loose, three layers, NO tight leg):
//     1. gb90 peak-profit give-back trail — once the position has a positive
//        peak MFE, clip when the open gain falls to (1-0.90) = 10% of that peak
//        (give back 90% of the peak). This is the ONLY profit-taking leg.
//     2. 60-day max-hold cap — flush at the close on the 60th daily bar held.
//     3. -15% catastrophe hard floor — cut the position at -15% (per-trade tail
//        bound). Fills at the observed close (a gap through books the gap).
//   NO tight give-back leg and NO tight (3-8%)+BE loss-cut: those were proven to
//   AMPUTATE this impulse signal (they cut the very continuations the entry is
//   built to capture). Protection here = the catastrophe floor + the giveback
//   trail + strength-entry + diversification, NOT a tight stop.
//
// ADVERSE-PROTECTION: loose gb90 ride + 60d cap + -15% catastrophe floor; tight
//   loss-cut (3-8%)+BE AMPUTATE this impulse signal; -15% widest floor that keeps
//   all-6 + bounds tail; protection = floor+trail+diversification+strength-entry,
//   not a tight stop. (Backtested C++: backtest/clip_path_bigcap_impulse.cpp over
//   data/rdagent/sp500_long_close.csv 2019-06..2026-06 — verdict recorded from the
//   faithful C++ harness numbers, not the prior python.)
//
// Books in RETURN units (equities have no fixed $/pt); USD = return * fixed
// notional/clip (default $10k, shadow-illustrative; operator rescales at flip).
//
// FEED: same wide daily-close CSV as the ladder (data/rdagent/sp500_long_close.csv,
//   RDAgent refresh_close_ibkr.py, IBKR 4002). Deploy-forward: seed primes the
//   detector (rolling 20-day high + prev close) but books nothing; the poller
//   dispatches each NEW date live.
//
// COST GATE: 20bp RT (bp of entry) debited from every clip's ret_real — the
//   validated gate for this looser book (single-name IBKR RT ~3-8bp, so 20bp is
//   conservative; ExecutionCostGuard has NO single-name equity rows — the befloor
//   lesson). Entry also passes the injected GateFn before any live order.
//
// LIVE EXECUTION: routes through the SAME set_exec / send_live_order machinery as
//   the ladder + befloor companions (send_live_order hard-gates on mode!=LIVE, so
//   SHADOW today, LIVE on flip with ZERO code change; the live-order call-site is
//   wired in engine_init in the same change). Ledger tag: "BigCap2pctImpulse".
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-
//   independent-engine): observe-only SHADOW, never opens/moves/shrinks/closes any
//   real position, never read by any parent, judged STANDALONE / ADDITIVE.
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

// BOOT CATCH-UP order suppression (mirrors the ladder's g_aulad_catchup): rows
// that arrived while the process was DOWN are replayed through the LIVE logic at
// boot so entries/exits land on their correct closes; during that replay broker
// orders + central-ledger writes are suppressed (stale-close fills must not hit
// the live path), while the book's own totals/closed deque still record.
static std::atomic<bool> g_bc2_catchup{false};

// ── one stock name's LONG-only +2%-impulse loose-ride book (returns-based) ────
class BigCapImpulseSym {
public:
    struct Config {
        std::string sym       = "NVDA";   // ticker -> book name + persist paths
        std::string live_sym  = "NVDA";   // symbol the live position trades (order path)
        double thr            = 0.02;     // +2% close-to-close impulse day to arm
        int    hi_window      = 20;       // must be a NEW high over the prior N closes
        // LOOSE 3-layer exit — no tight leg (tight cut/BE amputate this signal):
        double gb             = 0.90;     // giveback fraction of peak MFE (0.90 = keep 10%)
        int    max_hold       = 60;       // daily bars held -> flush at the close
        double catastrophe    = 15.0;     // -% hard floor (per-trade tail bound)
        double rt_cost_bp     = 20.0;     // REAL round-trip cost (bp of entry) debited per clip
        double notional       = 10000.0;  // $ per clip; USD = return * notional
        double lot            = 1.0;      // order-path lot (shares/CFD units decided at flip)
        std::string deploy_path;          // per-name deploy-forward anchor
        std::string bars_path;            // per-name persisted LIVE forward daily bars
        std::string book_path;            // per-name persisted REAL forward book totals
        std::string live_path;            // per-name persisted OPEN position state
        std::string closed_path;          // per-name persisted CLOSED forward clips log
    };

    // LIVE EXECUTION WIRING — identical contract to the ladder/befloor companions.
    // Set ONLY in the live main TU; null in a backtest TU -> live_step_ books but
    // fires no orders. SHADOW today (send_live_order no-ops while mode!=LIVE).
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit BigCapImpulseSym(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "bigcap2pct_companion_" + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "bigcap2pct_companion_" + s + "_daily.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = "bigcap2pct_companion_" + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = "bigcap2pct_companion_" + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "bigcap2pct_companion_" + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();
        load_live_state_();
    }

    const std::string& sym() const { return cfg_.sym; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_ret() const { return fwd_.ret; }                     // GROSS total forward return
    double  book_usd() const { return fwd_.ret * cfg_.notional; }
    double  book_ret_real() const { return fwd_.ret_real; }           // REAL (close fills - rt cost)
    double  book_usd_real() const { return fwd_.ret_real * cfg_.notional; }
    const Config& cfg() const { return cfg_; }

    // seed one historical daily close (primes detector history; books nothing).
    void seed_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ingest_(norm_ts_(ts_sec), close);
    }

    // Call ONCE after all seeding: stamp+persist deploy_ts on first-ever boot.
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
            // CARRY-FORWARD PENDING ARM: if the LAST seed day is itself a valid
            // +thr / new-20d-high signal, the faithful next action is a live entry
            // at the NEXT live close (the impulse just printed). Arm a pending entry
            // now; it books NOTHING (deploy_ts stamped at this bar, so the next LIVE
            // close's entry ts > deploy_ts => real fwd fill). First-boot only.
            if (signal_at_(-1)) {
                entry_pend_ = true; save_live_state_();
                std::printf("[BC2PCT][SEED-ARM] %s last seed day is a +%.1f%% new-%dd-high signal -> pending entry carried; next live close enters\n",
                            cfg_.sym.c_str(), cfg_.thr * 100.0, cfg_.hi_window);
                std::fflush(stdout);
            }
        }
    }

    // LIVE feed: one CLOSED daily bar for this name.
    void on_daily_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic append (idempotent re-poll)
        // DATA-INTEGRITY / SELF-HEALING split guard (ladder-parity): reject a >50%
        // single-day move (torn read / x1000), but 3 consecutive at the new level =
        // a real level shift (split/adjusted-history seam) -> accept, void any open
        // position WITHOUT booking (clip math across a seam is fake), resume.
        if (!c_.empty()) {
            const double jump = std::fabs(close / c_.back() - 1.0);
            if (jump > 0.50) {
                if (++rej_streak_ >= 3) {
                    std::printf("[BC2PCT][RESYNC] %s accepts level %.4f after %d consecutive >50%% rejects vs %.4f — split/seam; position voided unbooked\n",
                                cfg_.sym.c_str(), close, rej_streak_, c_.back());
                    std::fflush(stdout);
                    rej_streak_ = 0;
                    in_pos_ = false; entry_pend_ = false; entry_px_ = 0; mfe_ = 0; held_ = 0; tok_.clear();
                    ingest_(ts_sec, close); append_dump_(ts_sec, close); save_live_state_();
                    return;
                }
                std::printf("[BC2PCT][REJECT] %s daily close %.4f vs prev %.4f (%.0f%% jump) — torn read/split? bar dropped (streak %d/3)\n",
                            cfg_.sym.c_str(), close, c_.back(), jump * 100.0, rej_streak_);
                std::fflush(stdout);
                return;
            }
            rej_streak_ = 0;
        }
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);
        live_step_(ts_sec);
    }

    // Reload persisted LIVE forward daily bars (books nothing; deploy-forward gate).
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
    std::string sym_json() const {
        const double notl = cfg_.notional;
        const double cur = c_.empty() ? 0.0 : c_.back();
        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();

        // OPEN position right now (empty = flat).
        std::ostringstream op; int nopen = 0;
        if (in_pos_ && entry_px_ > 0 && cur > 0) {
            const double u  = cur / entry_px_ - 1.0;
            const double ur = u - cfg_.rt_cost_bp / 1e4;
            op.precision(2);
            op << "{\"flavor\":\"" << cfg_.sym << "Imp2\",\"dir\":\"long\","
               << "\"entry\":" << entry_px_ << ",\"cur\":" << cur << ",\"held\":" << held_ << ",";
            op.precision(3); op << "\"mfe_pct\":" << mfe_
                                << ",\"upnl_pct\":" << (u * 100.0)
                                << ",\"upnl_pct_real\":" << (ur * 100.0) << ",";
            op.precision(0); op << "\"upnl_usd\":" << (u * notl)
                                << ",\"upnl_usd_real\":" << (ur * notl)
                                << ",\"entry_ts\":" << (long long)entry_ts_ << "}";
            nopen = 1;
        }

        // CLOSED forward clips log — most-recent first.
        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c = *it;
            if (ntr++) tr << ",";
            tr.precision(2); tr << std::fixed;
            tr << "{\"flavor\":\"" << cfg_.sym << "Imp2\",\"dir\":\"long\","
               << "\"entry\":" << c.entry << ",\"exit\":" << c.exit << ",";
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
        o << "\"pos\":{\"active\":" << (in_pos_ ? "true" : "false")
          << ",\"pending\":" << (entry_pend_ ? "true" : "false")
          << ",\"held\":" << held_ << "},";
        o << "\"clips\":" << fwd_.clips << ",\"wins\":" << fwd_.wins << ",";
        o.precision(3); o << "\"pct\":" << (fwd_.ret * 100.0)
                          << ",\"pct_real\":" << (fwd_.ret_real * 100.0) << ",";
        o.precision(0); o << "\"usd\":" << (fwd_.ret * notl)
                          << ",\"usd_real\":" << (fwd_.ret_real * notl) << ",";
        o << "\"open\":[" << op.str() << "],\"trades\":[" << tr.str() << "]}";
        return o.str();
    }

private:
    Config cfg_;
    std::vector<int64_t> ts_;
    std::vector<double>  c_;
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;
    int     rej_streak_ = 0;

    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;

    // ── single LONG position ─────────────────────────────────────────────
    bool    in_pos_     = false;
    bool    entry_pend_ = false;   // signal seen on the last seed close; enter at NEXT live close
    double  entry_px_   = 0.0;     // fill of the open position
    double  mfe_        = 0.0;     // best fav % since entry
    int     held_       = 0;       // daily bars held (entry close = 0)
    int64_t entry_ts_   = 0;
    std::string tok_;

    struct FwdBook { double ret = 0.0; int clips = 0; int wins = 0; double ret_real = 0.0; };
    FwdBook fwd_;

    struct Closed { double entry = 0, exit = 0, ret = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason;
                    double ret_real = 0, usd_real = 0; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;

    static constexpr const char* ENGINE_TAG_ = "BigCap2pctImpulse";

    // Signal on close at index `at` (negative = from back). Needs prev close + a
    // full hi_window of prior closes. +thr close-to-close AND close >= max of the
    // prior hi_window closes (a NEW N-day high). No regime gate (ungated).
    bool signal_at_(int at) const noexcept {
        const int N = (int)c_.size();
        const int i = (at < 0) ? (N + at) : at;
        if (i <= 0 || i >= N) return false;
        if (i - 1 - cfg_.hi_window < -1) return false;   // need hi_window prior closes below i
        const double prev = c_[i - 1];
        if (prev <= 0) return false;
        const double j = c_[i] / prev - 1.0;
        if (j < cfg_.thr) return false;
        // no-arm across a data gap (a "1-day" move that spans weeks)
        if ((ts_[i] - ts_[i - 1]) > (int64_t)86400 * 7) return false;
        double hi = 0.0;                                 // max of the prior hi_window closes [i-hi_window .. i-1]
        for (int k = i - cfg_.hi_window; k <= i - 1; ++k) hi = std::max(hi, c_[k]);
        return c_[i] >= hi;                              // today's close is a NEW hi_window-day high
    }

    // Book one clip: fill AT the observed daily close. gross = close/entry - 1;
    // real = gross - rt_cost_bp. (No floor, no wish — the only tradable mark.)
    void book_clip_(double fill, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        const double r      = (entry_px_ > 0) ? (fill / entry_px_ - 1.0) : 0.0;
        const double r_real = r - cfg_.rt_cost_bp / 1e4;
        if (fwd) {
            if (!tok_.empty() && close_fn_) close_fn_(cfg_.live_sym, true, cfg_.lot, fill, tok_);
            if (ledger_fn_ && !g_bc2_catchup.load(std::memory_order_relaxed))
                ledger_fn_(ENGINE_TAG_, cfg_.live_sym, true, entry_px_, fill, cfg_.lot, entry_ts_, ts_sec, reason);
            fwd_.ret += r; fwd_.ret_real += r_real; fwd_.clips += 1; fwd_.wins += (r_real > 1e-9 ? 1 : 0);
            save_fwd_book_();
            Closed rec{entry_px_, fill, r, r * cfg_.notional, entry_ts_, ts_sec, reason, r_real, r_real * cfg_.notional};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[BC2PCT][CLIP] %s entry=%.2f fill=%.2f ret=%.4f ret_real=%.4f (%s)\n",
                        cfg_.sym.c_str(), entry_px_, fill, r, r_real, reason);
            std::fflush(stdout);
        }
        tok_.clear();
    }

    void enter_(double px, int64_t ts_sec, bool fwd) noexcept {
        in_pos_ = true; entry_px_ = px; entry_ts_ = ts_sec; mfe_ = 0.0; held_ = 0; tok_.clear();
        if (fwd && open_fn_ && !g_bc2_catchup.load(std::memory_order_relaxed)) {
            const double tp_dist_pts = px * (cfg_.thr);   // nominal target distance for the cost gate
            if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) {
                tok_ = open_fn_(cfg_.live_sym, true, cfg_.lot, px);
                std::printf("[BC2PCT][OPEN] %s LONG @%.2f lot=%.2f tok=%s\n",
                            cfg_.sym.c_str(), px, cfg_.lot, tok_.c_str());
                std::fflush(stdout);
            } else {
                in_pos_ = false;   // gate blocked the entry -> stay flat (no phantom book)
            }
        }
    }

    // Incremental state machine on the NEWEST daily close.
    void live_step_(int64_t ts_sec) noexcept {
        const int N = (int)c_.size();
        if (N < 2) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];

        // 1) pending entry carried from the last seed close: enter at THIS close.
        if (entry_pend_) {
            entry_pend_ = false;
            enter_(cur, ts_sec, fwd);
            save_live_state_();
            return;   // first managed close is the NEXT bar
        }

        // 2) manage the open position on this close (loose 3-layer exit).
        if (in_pos_) {
            held_ += 1;
            const double fav = (cur / entry_px_ - 1.0) * 100.0;
            if (fav > mfe_ + 1e-9) mfe_ = fav;
            const char* reason = nullptr;
            // DRAWDOWN-CANCEL: -15% catastrophe hard floor (mimic never touches real trade -> free cut).
            //   Backtested lever; on daily bars a gap can leap past it (same coarse-fill caveat as
            //   bigcap_mimic_lc_sweep S-2026-07-11). Paired with gb90 trail + 60d max-hold below.
            if (fav <= -cfg_.catastrophe) {                              // -15% catastrophe hard floor
                reason = "CATASTROPHE_FLOOR";
            } else if (mfe_ > 1e-9 && fav <= mfe_ * (1.0 - cfg_.gb)) {   // gb90 peak-profit trail
                reason = "GB90_TRAIL";
            } else if (held_ >= cfg_.max_hold) {                        // 60-day max-hold cap
                reason = "MAX_HOLD";
            }
            if (reason) {
                book_clip_(cur, ts_sec, fwd, reason);
                in_pos_ = false; entry_px_ = 0; mfe_ = 0; held_ = 0;
            }
        }

        // 3) entry detector on THIS close (only when flat). LONG-only, ungated.
        if (!in_pos_ && signal_at_(N - 1)) {
            enter_(cur, ts_sec, fwd);
        }
        save_live_state_();
    }

    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        double ret = 0, rreal = 0; int clips = 0, wins = 0;
        std::string line;
        if (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%lf %d %d %lf", &ret, &clips, &wins, &rreal) >= 4) {
                fwd_.ret = ret; fwd_.clips = clips; fwd_.wins = wins; fwd_.ret_real = rreal;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << fwd_.ret << " " << fwd_.clips << " " << fwd_.wins << " " << fwd_.ret_real << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.entry << "," << r.exit << "," << r.ret << "," << r.usd << ","
          << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << ","
          << r.ret_real << "," << r.usd_real << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            long long ets = 0, xts = 0; double rreal = 0, ureal = 0;
            const int n = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n],%lf,%lf",
                                      &r.entry, &r.exit, &r.ret, &r.usd, &ets, &xts, reason, &rreal, &ureal);
            if (n >= 9) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                r.ret_real = rreal; r.usd_real = ureal;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

    void load_live_state_() noexcept {
        std::ifstream f(cfg_.live_path);
        if (!f.is_open()) return;
        int ip = 0, ep = 0; long long ets = 0; std::string tok;
        if (f >> ip >> ep >> entry_px_ >> mfe_ >> held_ >> ets >> tok) {
            in_pos_ = (ip != 0); entry_pend_ = (ep != 0);
            entry_ts_ = (int64_t)ets; tok_ = (tok == "-") ? std::string() : tok;
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << (in_pos_ ? 1 : 0) << " " << (entry_pend_ ? 1 : 0) << " " << entry_px_ << " "
            << mfe_ << " " << held_ << " " << (long long)entry_ts_ << " "
            << (tok_.empty() ? "-" : tok_) << "\n"; }
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

// ── registry: owns the names, seeds/polls the wide daily-close CSV, writes the
//   merged bigcap2pct_companion_state.json (served by /api/bigcap2pct_companion). ─
class BigCapImpulseBook {
public:
    ~BigCapImpulseBook() { stop_poller(); }

    void add(BigCapImpulseSym::Config c) {
        col_[c.sym] = syms_.size();
        syms_.emplace_back(std::move(c));
    }
    BigCapImpulseSym* find(const std::string& sym) {
        auto it = col_.find(sym);
        return (it == col_.end()) ? nullptr : &syms_[it->second];
    }
    void set_exec(BigCapImpulseSym::OpenFn o, BigCapImpulseSym::CloseFn c,
                  BigCapImpulseSym::GateFn g, BigCapImpulseSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }

    // warm-seed from the WIDE daily-close CSV (header ",NAME1,NAME2,...").
    size_t seed_from_wide_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[BC2PCT][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string header;
        if (!std::getline(f, header)) return 0;
        std::unordered_map<int, size_t> colsym;
        { std::stringstream hs(header); std::string tok; int ci = 0;
          while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = it->second; ++ci; } }
        int64_t watermark = 0;
        { std::ifstream wf(lastseen_path_); long long v = 0; if (wf.is_open() && (wf >> v)) watermark = v; }
        std::string line; size_t rows = 0, caught_up = 0;
        if (watermark > 0) g_bc2_catchup.store(true, std::memory_order_relaxed);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            const int64_t ts = parse_date_ts_(line);
            if (ts <= 0) continue;
            const bool live_replay = (watermark > 0 && ts > watermark);
            dispatch_row_(line, colsym, ts, /*live=*/live_replay);
            if (live_replay) ++caught_up;
            if (ts > last_seed_ts_) last_seed_ts_ = ts;
            ++rows;
        }
        g_bc2_catchup.store(false, std::memory_order_relaxed);
        save_lastseen_(last_seed_ts_);
        std::printf("[BC2PCT][SEED] wide-csv %s: %zu rows (%zu caught-up live, watermark=%lld), %zu/%zu names mapped, last=%lld\n",
                    path.c_str(), rows, caught_up, (long long)watermark,
                    colsym.size(), syms_.size(), (long long)last_seed_ts_);
        std::fflush(stdout);
        return rows;
    }

    void save_lastseen_(int64_t ts) const noexcept {
        if (ts <= 0) return;
        std::ofstream wf(lastseen_path_, std::ios::trunc);
        if (wf.is_open()) wf << (long long)ts << "\n";
    }

    size_t seed_dumps_all() { size_t n = 0; for (auto& s : syms_) n += s.seed_dump(); return n; }
    void   finalize_all() { for (auto& s : syms_) s.finalize_seed(); recompute_and_write(); }

    void on_daily_bar(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* s = find(sym)) { s->on_daily_bar(ts_sec, close); recompute_unlocked_(); }
    }

    void start_poller(const std::string& rel, int poll_ms = 900000 /*15min*/) {
        csv_rel_ = rel; poll_ms_ = poll_ms;
        last_seen_ts_ = last_seed_ts_;
        seed_missed_ = (last_seed_ts_ == 0);
        running_.store(true);
        thread_ = std::thread([this]{ poll_loop_(); });
        std::printf("[BC2PCT][POLL] watching %s (poll=%dms, resume-from=%lld)\n",
                    rel.c_str(), poll_ms_, (long long)last_seen_ts_.load());
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
    mutable std::mutex mu_;

    std::string state_json_unlocked_() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot_usd = 0.0, tot_usd_real = 0.0;
        for (const auto& s : syms_) { last_ts = std::max(last_ts, s.last_ts()); tot_usd += s.book_usd(); tot_usd_real += s.book_usd_real(); }
        o << "{\"engine\":\"bigcap-2pct-impulse\",\"shadow\":true,\"grade\":\"daily-close\",";
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

    std::vector<BigCapImpulseSym> syms_;
    std::unordered_map<std::string, size_t> col_;
    std::string state_path_ = "bigcap2pct_companion_state.json";
    int64_t last_seed_ts_ = 0;
    std::string lastseen_path_ = "bigcap2pct_companion_lastseen.txt";

    std::string      csv_rel_;
    int              poll_ms_ = 900000;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> last_seen_ts_{0};
    bool             seed_missed_ = false;
    std::thread      thread_;

    static int64_t parse_date_ts_(const std::string& line) noexcept {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(line.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
        return days_from_civil_(y, m, d) * 86400LL;
    }
    static int64_t days_from_civil_(int y, unsigned m, unsigned d) noexcept {
        y -= (m <= 2);
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097LL + (int64_t)doe - 719468LL;
    }

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
            const bool as_seed = seed_missed_;
            int64_t newest = seen; int fresh = 0;
            std::string line;
            {
                std::lock_guard<std::mutex> lk(mu_);
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    const int64_t ts = parse_date_ts_(line);
                    if (!as_seed && ts <= seen) continue;
                    dispatch_row_(line, colsym, ts, /*live=*/!as_seed);
                    if (ts > newest) newest = ts;
                    ++fresh;
                }
                if (fresh > 0 && as_seed) {
                    for (auto& s : syms_) s.finalize_seed();
                    last_seen_ts_.store(newest); seed_missed_ = false;
                    save_lastseen_(newest); recompute_unlocked_();
                } else if (fresh > 0) {
                    last_seen_ts_.store(newest);
                    save_lastseen_(newest); recompute_unlocked_();
                }
            }
            if (fresh > 0 && as_seed) {
                std::printf("[BC2PCT][POLL] first-load SEED %d rows (boot-seed missed), stamped deploy=%lld\n",
                            fresh, (long long)newest);
                std::fflush(stdout);
            } else if (fresh > 0) {
                std::printf("[BC2PCT][POLL] %d new daily row(s), newest=%lld\n", fresh, (long long)newest);
                std::fflush(stdout);
            }
        }
    }
};

// Singleton — accessor mirrors omega::stockmover_ladder_book().
inline BigCapImpulseBook& bigcap_impulse_book() noexcept {
    static BigCapImpulseBook inst;
    return inst;
}

} // namespace omega
