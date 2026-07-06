#pragma once
// =============================================================================
// JumpRiderEngine — the crypto UpJump pattern generalized to the Omega symbols
// (metals / oil / FX / indices). Operator ask 2026-07-07: "if upjump IS the
// solution, implement it on all the symbols where we are not using it".
//
// WHAT IT IS: the RIDER layer the non-crypto book was missing. The BE-floor
// companions HARVEST clips once a move covers cost, but nothing RIDES the body
// of the move on these symbols (crypto has UpJump2 for that). This engine is
// that parent: a +thr jump over W H1 bars opens LONG at the signal close and
// rides until the SYMMETRIC opposite jump (which flips to SHORT where allowed —
// CFD/futures venues, so both bull and bear are traded), with two validated
// loss-minimisers on top:
//   * BE-RATCHET : once the ride's peak return >= be_arm, it can never close
//     below breakeven — cuts green->red round-trips without clipping runners
//     (validated on the NDX TSMom leg: arm +1.5%/floor BE -> +95.6% PF1.78 vs
//     wide +87% PF1.72, better OOS; S-2026-06-30 NdxCompanionClip).
//   * HARD STOP  : catastrophe floor at -hard_stop (default 2x thr) for the
//     pre-arm cohort — the only window where a real loss beyond noise can grow.
//   After a BE-ratchet / hard-stop exit the same direction is BLOCKED until the
//   jump signal first releases (j back inside +/-thr) — no immediate re-entry
//   churn on a still-elevated jump.
//
// HONEST ACCOUNTING FROM DAY ONE (no legacy/model column): entries and exits
// book at OBSERVED H1 closes only, every round trip is debited rt_cost_bp
// (per-symbol real cost: spread+slip+comm), returns can be negative, the ledger
// receives the same real fill. Books in RETURN units; USD = ret * notional
// (shadow-illustrative $10k/symbol, operator rescales). Deploy-forward anchor:
// seeded history books NOTHING; the forward record starts at $0 on first boot.
//
// SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-engine): its OWN
// positions through the standard order path (SHADOW while mode!=LIVE, live on
// flip), its OWN state file (jumprider_state.json -> /api/jumprider), never
// reads or touches the BE-floor books it shares feeds with.
//
// ADVERSE-PROTECTION: BE-RATCHET + HARD-STOP + SYMMETRIC-FLIP — backtested
//   BASIS: the NDX engine-native BE-ratchet verdict (S-2026-06-30, arm/floor
//   figures above) and the crypto UpJump2 symmetric-flip harness
//   (ibkrcrypto_bt, costed 18-25bp RT). Per-symbol W/thr reuse each symbol's
//   validated BE-floor detector (gold/xag/usoil 2h +/-1%; FX/index 2h +/-0.30%);
//   be_arm=thr/2 and hard_stop=2x thr are PROVISIONAL family defaults — the
//   per-symbol faithful backtest on ~/Tick H1 data is OWED before any LIVE
//   flip. Ships SHADOW; judged on its forward real column only.
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
#include <unordered_map>
#include <cmath>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

// ── one symbol's jump rider (long/short, one position at a time) ─────────────
class JumpRiderSym {
public:
    struct Config {
        std::string sym      = "XAUUSD";  // tag -> persist paths + JSON name
        std::string live_sym = "XAUUSD";  // order-path symbol
        int    W          = 2;            // jump window (H1 bars)
        double thr        = 0.01;         // jump threshold (fraction)
        double be_arm     = 0.005;        // BE-ratchet arm (peak return fraction); default thr/2
        double hard_stop  = 0.02;         // catastrophe stop (adverse return fraction); default 2x thr
        double rt_cost_bp = 6.0;          // REAL round-trip cost (bp) debited from every trade's ret
        bool   allow_short = true;        // CFD/futures venue: ride down-jumps too
        double notional   = 10000.0;      // $ per ride; USD = ret * notional (shadow-illustrative)
        double lot        = 1.0;          // order-path lot
        std::string deploy_path, bars_path, book_path, live_path, closed_path;   // per-sym defaults in ctor
    };

    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit JumpRiderSym(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "jumprider_" + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "jumprider_" + s + "_h1.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = "jumprider_" + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = "jumprider_" + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "jumprider_" + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_book_();
        load_closed_();
        load_live_state_();
    }

    const std::string& sym() const { return cfg_.sym; }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_usd_real() const { return book_.ret_real * cfg_.notional; }

    // seed one historical H1 close (primes the jump window; books nothing — deploy-forward gate).
    void seed_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ingest_(norm_ts_(ts_sec), close);
    }
    // seed from a CSV (ts,o,h,l,c OR ts,c); returns rows ingested.
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) return 0;
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            const int got = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c);
            const double close = (got == 5) ? c : (got == 2 ? o : 0.0);
            if (got >= 2 && close > 0.0) { ingest_(norm_ts_((int64_t)ts), close); ++n; }
        }
        return n;
    }
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
        }
    }

    // LIVE feed: one CLOSED H1 bar (same sink cadence as the BE-floor sibling).
    void on_h1_bar(int64_t ts_sec, double close) noexcept {
        if (!enabled || close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);   // self-persist -> detector + book continuity across restarts
        live_step_(ts_sec);
    }

    bool enabled = true;

    std::string sym_json() const {
        const double notl = cfg_.notional;
        const double cur = c_.empty() ? 0.0 : c_.back();
        std::ostringstream o; o << std::fixed;
        o << "{\"sym\":\"" << cfg_.sym << "\",\"live_sym\":\"" << cfg_.live_sym << "\",";
        o.precision(4); o << "\"thr\":" << cfg_.thr << ",\"be_arm\":" << cfg_.be_arm
                          << ",\"hard_stop\":" << cfg_.hard_stop << ",";
        o.precision(1); o << "\"rt_cost_bp\":" << cfg_.rt_cost_bp << ",";
        o << "\"W\":" << cfg_.W << ",\"allow_short\":" << (cfg_.allow_short ? "true" : "false")
          << ",\"bars\":" << ts_.size() << ",\"deploy_ts\":" << (long long)deploy_ts_
          << ",\"ts\":" << (long long)last_ts() << ",";
        o.precision(0); o << "\"notional\":" << notl << ",";
        // forward REAL book (the only column — no model figures in this engine)
        o << "\"trades\":" << book_.trades << ",\"wins\":" << book_.wins << ",";
        o.precision(3); o << "\"pct_real\":" << (book_.ret_real * 100.0) << ",";
        o.precision(0); o << "\"usd_real\":" << (book_.ret_real * notl) << ",";
        // open position
        o << "\"open\":";
        if (pos_ != 0 && entry_ > 0) {
            const double u  = pos_ * (cur - entry_) / entry_;
            const double ur = u - cfg_.rt_cost_bp / 1e4;
            o << "{\"dir\":\"" << (pos_ > 0 ? "LONG" : "SHORT") << "\",";
            o.precision(4); o << "\"entry\":" << entry_ << ",\"cur\":" << cur
                              << ",\"peak_ret\":" << peak_ << ",";
            o.precision(3); o << "\"upnl_pct_real\":" << (ur * 100.0) << ",";
            o.precision(0); o << "\"upnl_usd_real\":" << (ur * notl)
                              << ",\"entry_ts\":" << (long long)entry_ts_
                              << ",\"be_armed\":" << (peak_ >= cfg_.be_arm ? "true" : "false") << "}";
        } else o << "null";
        // closed rides — most-recent first
        o << ",\"rides\":[";
        int n = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& r = *it;
            if (n++) o << ",";
            o << "{\"dir\":\"" << (r.dir > 0 ? "LONG" : "SHORT") << "\",";
            o.precision(4); o << "\"entry\":" << r.entry << ",\"exit\":" << r.exit << ",";
            o.precision(3); o << "\"pct_real\":" << (r.ret_real * 100.0) << ",";
            o.precision(0); o << "\"usd_real\":" << r.usd_real
                              << ",\"reason\":\"" << r.reason << "\",\"entry_ts\":" << (long long)r.ets
                              << ",\"exit_ts\":" << (long long)r.xts << "}";
        }
        o << "]}";
        return o.str();
    }

private:
    Config cfg_;
    std::vector<int64_t> ts_;
    std::vector<double>  c_;
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;

    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    // position state (one ride at a time)
    int     pos_ = 0;              // -1/0/+1
    double  entry_ = 0, peak_ = 0; // entry px; peak favourable return (fraction)
    int64_t entry_ts_ = 0;
    std::string token_;
    bool    block_up_ = false, block_dn_ = false;   // post-BE/HS re-entry guard until signal releases

    struct Book { double ret_real = 0.0; int trades = 0; int wins = 0; };
    Book book_;
    struct Closed { int dir = 0; double entry = 0, exit = 0, ret_real = 0, usd_real = 0;
                    int64_t ets = 0, xts = 0; std::string reason; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;

    std::string engine_name_() const { return "JumpRider" + cfg_.sym; }

    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                                 // backtest TU: accounting only
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N <= W) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];
        const double j   = c_[N - 1] / c_[N - 1 - W] - 1.0;
        // GAP GUARD: index-based W-bar jump spans days across a weekend/outage — never OPEN into a
        // gap regime the calibration never contained; exits below stay honoured.
        const bool contig = (ts_[N - 1] - ts_[N - 1 - W]) <= (int64_t)W * 3600 * 2;
        const bool up = (j >=  cfg_.thr);
        const bool dn = (j <= -cfg_.thr);
        if (!up) block_up_ = false;                            // signal released -> same-dir re-entry unblocked
        if (!dn) block_dn_ = false;

        if (pos_ != 0 && entry_ > 0) {
            const double ret = pos_ * (cur - entry_) / entry_;
            if (ret > peak_) peak_ = ret;
            const bool flip_sig = (pos_ > 0) ? dn : up;
            if (flip_sig) {                                    // symmetric jump against the ride -> exit
                close_ride_(cur, ts_sec, fwd, "SYM_FLIP");
                // flip: the opposite jump IS a fresh signal; open the other way (gap-guarded)
                if (contig && cfg_.allow_short && ((dn && !block_dn_) || (up && !block_up_)))
                    open_ride_(dn ? -1 : +1, cur, ts_sec, fwd);
            } else if (peak_ >= cfg_.be_arm && ret <= 0.0) {   // BE-RATCHET: armed winner can't go red
                close_ride_(cur, ts_sec, fwd, "BE_RATCHET");
                if (pos_ == 0) { if (j >= cfg_.thr) block_up_ = true; if (j <= -cfg_.thr) block_dn_ = true; }
            } else if (ret <= -cfg_.hard_stop) {               // catastrophe floor (pre-arm cohort)
                close_ride_(cur, ts_sec, fwd, "HARD_STOP");
                if (pos_ == 0) { if (j >= cfg_.thr) block_up_ = true; if (j <= -cfg_.thr) block_dn_ = true; }
            }
        } else if (contig) {
            if (up && !block_up_)                       open_ride_(+1, cur, ts_sec, fwd);
            else if (dn && cfg_.allow_short && !block_dn_) open_ride_(-1, cur, ts_sec, fwd);
        }
        save_live_state_();
    }

    void open_ride_(int dir, double cur, int64_t ts_sec, bool fwd) noexcept {
        // cost gate: the ride must be able to clear its own RT cost on a thr-sized move
        const double tp_dist_pts = cur * cfg_.thr;
        if (gate_fn_ && !gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) return;
        pos_ = dir; entry_ = cur; peak_ = 0.0; entry_ts_ = ts_sec; token_.clear();
        if (fwd) {
            token_ = open_fn_(cfg_.live_sym, dir > 0, cfg_.lot, cur);
            std::printf("[JMPRDR][OPEN] %s %s @%.4f lot=%.2f tok=%s\n",
                        engine_name_().c_str(), dir > 0 ? "LONG" : "SHORT", cur, cfg_.lot, token_.c_str());
            std::fflush(stdout);
        }
    }

    void close_ride_(double px_obs, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        const int dir = pos_;
        if (fwd && dir != 0 && entry_ > 0) {
            if (!token_.empty() && close_fn_) close_fn_(cfg_.live_sym, dir > 0, cfg_.lot, px_obs, token_);
            if (ledger_fn_) ledger_fn_(engine_name_(), cfg_.live_sym, dir > 0, entry_, px_obs, cfg_.lot,
                                       entry_ts_, ts_sec, reason);
            const double r_real = dir * (px_obs - entry_) / entry_ - cfg_.rt_cost_bp / 1e4;   // REAL only
            book_.ret_real += r_real; book_.trades += 1; book_.wins += (r_real > 1e-9 ? 1 : 0);
            save_book_();
            Closed rec{dir, entry_, px_obs, r_real, r_real * cfg_.notional, entry_ts_, ts_sec, reason};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[JMPRDR][CLOSE] %s %s entry=%.4f exit=%.4f ret_real=%.4f (%s)\n",
                        engine_name_().c_str(), dir > 0 ? "LONG" : "SHORT", entry_, px_obs, r_real, reason);
            std::fflush(stdout);
        }
        pos_ = 0; entry_ = 0; peak_ = 0; entry_ts_ = 0; token_.clear();
        save_live_state_();   // persist the flat state NOW (crash double-book guard, befloor precedent)
    }

    // ── persistence ──
    void load_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        double r = 0; int t = 0, w = 0;
        if (f >> r >> t >> w) { book_.ret_real = r; book_.trades = t; book_.wins = w; }
    }
    void save_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << book_.ret_real << " " << book_.trades << " " << book_.wins << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }
    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.dir << "," << r.entry << "," << r.exit << "," << r.ret_real << "," << r.usd_real << ","
          << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            long long ets = 0, xts = 0;
            if (std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^\n]",
                            &r.dir, &r.entry, &r.exit, &r.ret_real, &r.usd_real, &ets, &xts, reason) >= 7) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }
    void load_live_state_() noexcept {
        std::ifstream f(cfg_.live_path);
        if (!f.is_open()) return;
        int p = 0, bu = 0, bd = 0; double e = 0, pk = 0; long long ets = 0; std::string tok;
        if (f >> p >> e >> pk >> ets >> tok >> bu >> bd) {
            pos_ = p; entry_ = e; peak_ = pk; entry_ts_ = (int64_t)ets;
            token_ = (tok == "-") ? std::string() : tok;
            block_up_ = (bu != 0); block_dn_ = (bd != 0);
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << pos_ << " " << entry_ << " " << peak_ << " " << (long long)entry_ts_ << " "
            << (token_.empty() ? "-" : token_) << " " << (block_up_ ? 1 : 0) << " " << (block_dn_ ? 1 : 0) << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.live_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.live_path.c_str());
    }

    void ingest_(int64_t ts, double close) noexcept { ts_.push_back(ts); c_.push_back(close); }
    void append_dump_(int64_t ts, double close) const noexcept {
        std::ofstream f(cfg_.bars_path, std::ios::app);
        if (f.is_open()) f << (long long)ts << "," << close << "\n";
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
            if (k + 1 < n && ts_[idx[k + 1]] == ts_[i]) continue;
            nts.push_back(ts_[i]); nc.push_back(c_[i]);
        }
        ts_.swap(nts); c_.swap(nc);
    }
};

// ── registry: owns the symbols, merges jumprider_state.json ─────────────────
class JumpRiderBook {
public:
    void add(JumpRiderSym::Config c) {
        idx_[c.sym] = syms_.size();
        syms_.emplace_back(std::move(c));
    }
    JumpRiderSym* find(const std::string& sym) {
        auto it = idx_.find(sym);
        return (it == idx_.end()) ? nullptr : &syms_[it->second];
    }
    void set_exec(JumpRiderSym::OpenFn o, JumpRiderSym::CloseFn c,
                  JumpRiderSym::GateFn g, JumpRiderSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }
    size_t seed_sym(const std::string& sym, const std::string& csv) {
        auto* s = find(sym);
        return s ? s->seed_from_h1_csv(csv) : 0;
    }
    size_t seed_dumps_all() {
        size_t n = 0;
        for (auto& s : syms_) n += s.seed_from_h1_csv("jumprider_" + lower__(s.sym()) + "_h1.csv");
        return n;
    }
    void finalize_all() { for (auto& s : syms_) s.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed H1 bar for `sym` (call right next to the BE-floor sibling's feed).
    void on_h1_bar(const std::string& sym, int64_t ts_sec, double close) {
        if (auto* s = find(sym)) { s->on_h1_bar(ts_sec, close); recompute_and_write(); }
    }

    std::string state_json() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot = 0.0;
        for (const auto& s : syms_) { last_ts = std::max(last_ts, s.last_ts()); tot += s.book_usd_real(); }
        o << "{\"engine\":\"jump-rider\",\"shadow\":true,\"grade\":\"h1-close\",";
        o.precision(0); o << std::fixed << "\"total_usd_real\":" << tot << ",\"symbols\":[";
        for (size_t i = 0; i < syms_.size(); ++i) { if (i) o << ","; o << syms_[i].sym_json(); }
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
    static std::string lower__(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
    std::vector<JumpRiderSym> syms_;
    std::unordered_map<std::string, size_t> idx_;
    std::string state_path_ = "jumprider_state.json";
};

inline JumpRiderBook& jump_rider_book() noexcept {
    static JumpRiderBook inst;
    return inst;
}

} // namespace omega
