#pragma once
// =============================================================================
// IndexBeFloorCompanion — per-symbol <SYM>Pos (long) + <SYM>Neg (short) BE-floor
// books for US500 / NAS100 / DJ30 / GER40 index CFDs.
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math : backtest/index_befloor_ls.py  (parent() 2h-jump detector + leg_book()
//          BE-floor walk) — itself the gold/FX mechanism (gold_befloor_ls.py /
//          fx_befloor_ls.py) index-tuned.
// Multi-symbol like FxBeFloorCompanion (own aggregate state file + /api endpoint),
// but books in PRICE POINTS -> USD via a PER-SYMBOL point-value (gold convention,
// NOT FX percent-of-notional) because index point-values differ hugely:
//   US500.F $50/pt · USTEC/NAS100 $1/pt · DJ30.F $5/pt · GER40 ~$1.10/pt (sizing.hpp).
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-
// engine). Observe-only, shadow: NEVER opens / moves / shrinks / closes a real
// position, NEVER read by any parent. Each symbol self-detects its OWN 2h up/down
// move windows (W=2 H1, +/-0.30%) from that symbol's live H1 close stream, and runs
// x3 BE-floor tiers per direction (20bp banker / 150bp runner / 400bp wide). Judge STANDALONE
// (net>0, both WF halves) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: BE-FLOOR — a leg stays FLAT until price clears +be_bp from its
//   ref (covers RT cost), opens THERE, and its stop sits at-or-above entry (long) /
//   at-or-below entry (short) and only trails favourably. Therefore exit >= entry
//   (long) / <= entry (short) ALWAYS => net_pts >= 0 on EVERY clip BY CONSTRUCTION.
//   A cold-loss cut is structurally impossible and unnecessary. Backtested vs
//   backtest/index_befloor_ls.py: neg=0 every symbol/config, both WF halves + on all
//   4 (US500/NAS100/DJ30/GER40, thr=0.30% be=6 r20/r150). Verdict = BE-floor (no cut),
//   not skipped (feedback-engine-loss-protection-provision).
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

// ── one index symbol's Pos/Neg BE-floor book ─────────────────────────────────
class IndexBeFloorSym {
public:
    struct Config {
        std::string sym      = "US500";   // short tag -> flavor names <sym>Pos/<sym>Neg + persist paths
        std::string live_sym = "US500.F"; // symbol the live legs trade (order path / tick_value)
        int    W             = 2;         // detector window (H1 bars) -> "2h"
        double thr           = 0.003;     // 2h jump arm threshold (0.30%; research best both WF halves +)
        double be_bp         = 6.0;       // RT cost floor (bp) to open a leg (research be=6)
        double dpp_per_lot   = 50.0;      // $/point for 1.0 lot (per-symbol point-value)
        double lot           = 1.0;
        std::string deploy_path;          // per-symbol persisted deploy-forward anchor
        std::string bars_path;            // per-symbol persisted LIVE forward H1 bars (survives restart)
        std::string book_path;            // per-symbol persisted REAL forward book (2 runners x 2 dirs)
        std::string live_path;            // per-symbol persisted OPEN window+leg arm-state (survives restart)
        std::string closed_path;          // per-symbol persisted CLOSED forward trades log (the "trades log")
    };

    // ── LIVE EXECUTION WIRING — makes <SYM>Pos/<SYM>Neg REAL trading engines (2 runners each).
    //   Identical contract to Gold/FxBeFloorCompanion: set ONLY in the live main TU; null in the
    //   backtest TU -> live_step_ short-circuits (pure accounting, canary unaffected). Each runner
    //   opens its OWN position via the order path (SHADOW today: send_live_order no-ops while
    //   mode!=LIVE; LIVE on flip) and records EACH close to the shadow ledger -> ENGINE LEDGER +
    //   headline PnL. ──
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit IndexBeFloorSym(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "index_companion_" + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "index_companion_" + s + "_h1.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = "index_companion_" + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = "index_companion_" + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "index_companion_" + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();       // CLOSED forward trades log -> survives restart
        load_live_state_();   // RESUME open window+legs across restart (fix "resets every deploy")
    }

    const std::string& sym() const { return cfg_.sym; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }

    // warm-seed from an H1 CSV (ts,o,h,l,c OR ts,c). Primes the detector + rebuilds the
    // book across restarts; books nothing new by itself (deploy-forward gate). Dedup+sort.
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[AUIDX][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue; // skip header
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            const int got = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c);
            const double close = (got == 5) ? c : (got == 2 ? o : 0.0);
            if (got >= 2 && close > 0.0) { ingest_(norm_ts_((int64_t)ts), close); ++n; }
        }
        std::printf("[AUIDX][SEED] %s: %zu H1 bars from %s (bars=%zu)\n", cfg_.sym.c_str(), n, path.c_str(), ts_.size());
        std::fflush(stdout);
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

    // LIVE feed: one CLOSED H1 bar for this symbol (close = mid at H1 close).
    void on_h1_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);   // PERSIST the forward bar so the book survives restart
        live_step_(ts_sec);            // fire real (shadow/live-gated) orders on live BE-floor transitions
    }

    // Reload persisted LIVE forward bars (written by on_h1_bar) from the cwd dump CSV.
    // Books nothing new by itself (deploy-forward gate); the recompute replays these forward
    // bars so the book is NON-VOLATILE across restarts (mirrors gold/FX persisted forward book).
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

    // Emit this symbol's desk JSON object. REAL TRADES ONLY — no backtest/replay number (operator rule:
    // if it is not a forward trade it does not belong in the live GUI). "usd"/"pts" = the REAL forward
    // book ($0 until the first live clip closes). Per-flavor "runners" = both r20 banker + r150 runner
    // (forward-only). "open" = live legs open right now. "trades" = closed forward trades (recent first).
    std::string sym_json() const {
        const double dpp = cfg_.dpp_per_lot * cfg_.lot;   // price-point -> USD
        const char* TIER_TAG[NT_] = { "banker", "runner", "wide", "r50", "r100" };
        struct Flavor { const char* suffix; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"Pos", "long", true}, {"Neg", "short", false} };
        const double cur = c_.empty() ? 0.0 : c_.back();

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double sym_pts = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            double book_pts = 0.0; int fwd_clips = 0, fwd_wins = 0;
            std::ostringstream runs;
            for (int ti = 0; ti < NT_; ++ti) {
                const FwdBook& b = fwd_[fi][ti];
                book_pts += b.pts; fwd_clips += b.clips; fwd_wins += b.wins;
                if (ti) runs << ",";
                runs.precision(0); runs << std::fixed;
                runs << "{\"tier\":\"" << TIER_TAG[ti] << "\",\"gb_bp\":" << (long)LIVE_GB_[ti]
                     << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                runs.precision(2); runs << "\"pts\":" << b.pts << ",";
                runs.precision(0); runs << "\"usd\":" << (b.pts * dpp) << "}";
            }
            sym_pts += book_pts;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << cfg_.sym << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"clips\":" << fwd_clips << ",\"wins\":" << fwd_wins << ",";
            fl.precision(2); fl << "\"book_pts\":" << book_pts << ",";
            fl.precision(0); fl << "\"book_usd\":" << (book_pts * dpp)
               << ",\"runners\":[" << runs.str() << "]}";
        }

        // OPEN legs right now (empty = idle/reset).
        std::ostringstream op; int nopen = 0;
        for (int fi = 0; fi < 2; ++fi) for (int ti = 0; ti < NT_; ++ti) {
            const LiveLeg& L = live_[fi][ti];
            if (!L.has_entry) continue;
            const bool up = (fi == 0);
            const double u = up ? (cur - L.entry) : (L.entry - cur);
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << cfg_.sym << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"tier\":\"" << TIER_TAG[ti] << "\",";
            op.precision(2);
            op << "\"entry\":" << L.entry << ",\"wm\":" << L.wm << ",\"cur\":" << cur
               << ",\"upnl_pts\":" << u << ",";
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
            tr << "{\"flavor\":\"" << cfg_.sym << flavors[cfi].suffix << "\",\"dir\":\""
               << flavors[cfi].dir << "\",\"tier\":\"" << TIER_TAG[(c.ti >= 0 && c.ti < NT_) ? c.ti : 0] << "\",";
            tr.precision(2);
            tr << "\"entry\":" << c.entry << ",\"exit\":" << c.exit << ",\"pts\":" << c.pts << ",";
            tr.precision(0);
            tr << "\"usd\":" << c.usd << ",\"reason\":\"" << c.reason << "\",\"entry_ts\":" << (long long)c.ets
               << ",\"exit_ts\":" << (long long)c.xts << "}";
        }

        o << "{\"sym\":\"" << cfg_.sym << "\",\"live_sym\":\"" << cfg_.live_sym << "\",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_ << ",\"ts\":" << (long long)last_ts << ",";
        o.precision(2); o << "\"dpp\":" << dpp << ",\"pts\":" << sym_pts << ",";
        o.precision(0); o << "\"usd\":" << (sym_pts * dpp) << ",";
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

    // ── live execution — TWO runner legs per direction (operator spec: 2 runners), BE-floored
    //   (arm only once price covers cost be_bp; trail stop pinned >= entry long / <= entry short ->
    //   exit at stall/reversal keeping profit; neg=0 by construction). r20 tight + r150 wide. Each
    //   runner is its OWN position -> OWN ledger row -> shows in PnL, exactly like every main engine. ──
    static constexpr int    NT_ = 5;   // r20 banker / r150 runner / r400 wide / r50 / r100
    static constexpr double LIVE_GB_[NT_] = { 20.0, 150.0, 400.0, 50.0, 100.0 };  // 50/100 APPENDED at end (persistence keys by index -> never reorder)
    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    bool     win_[2] = { false, false };                 // parent 2h window per direction (0=Pos/long,1=Neg/short)
    struct LiveLeg { bool has_entry = false; double entry = 0, wm = 0, ref = 0; int64_t entry_ts = 0; std::string token; };
    LiveLeg live_[2][NT_];
    struct FwdBook { double pts = 0.0; int clips = 0; int wins = 0; };
    FwdBook  fwd_[2][NT_];

    // CLOSED forward trades — the "trades log". .pts = trade price-pts (neg=0 by construction);
    // .usd = pts * dpp at close time.
    struct Closed { int fi = 0, ti = 0; double entry = 0, exit = 0, pts = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;
    std::string leg_engine_(int fi, int ti) const {
        return cfg_.sym + (fi == 0 ? "Pos" : "Neg") + ("_r" + std::to_string((long)LIVE_GB_[ti]));
    }

    // Incremental live BE-floor state machine on the NEWEST bar (mirrors Gold/FxBeFloorCompanion).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                               // backtest TU / not wired: pure accounting
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N <= W) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];
        const double j   = c_[N - 1] / c_[N - 1 - W] - 1.0;
        const double thr = cfg_.thr, be = cfg_.be_bp;
        for (int fi = 0; fi < 2; ++fi) {
            const bool up = (fi == 0);
            const bool enter = up ? (j >=  thr) : (j <= -thr);
            const bool exit  = up ? (j <= -thr) : (j >=  thr);
            if (!win_[fi] && enter) {
                win_[fi] = true;
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; L.has_entry = false; L.wm = 0; L.ref = cur; }
            }
            for (int ti = 0; ti < NT_ && win_[fi]; ++ti) {
                LiveLeg& L = live_[fi][ti];
                const double gb = LIVE_GB_[ti];
                if (!L.has_entry) {
                    const bool cond = up ? ((cur / L.ref - 1.0) * 1e4 >= be) : ((1.0 - cur / L.ref) * 1e4 >= be);
                    if (cond) {
                        const double tp_dist_pts = cur * (gb / 1e4);
                        const bool viable = !gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot);
                        if (viable) {
                            L.entry = cur; L.wm = cur; L.has_entry = true; L.entry_ts = ts_sec;
                            if (fwd) {
                                L.token = open_fn_(cfg_.live_sym, up, cfg_.lot, cur);
                                std::printf("[AUIDX][OPEN] %s %s @%.2f lot=%.2f tok=%s\n",
                                            leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", cur, cfg_.lot, L.token.c_str());
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
            if (win_[fi] && exit) {
                for (int ti = 0; ti < NT_; ++ti) { LiveLeg& L = live_[fi][ti]; if (L.has_entry) close_leg_(fi, ti, up, cur, ts_sec, fwd, "WINDOW_EXIT"); }
                win_[fi] = false;
            }
        }
        save_live_state_();   // snapshot window+leg arm-state every live bar -> restart RESUMES, never re-zeroes
    }

    void close_leg_(int fi, int ti, bool up, double px, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        LiveLeg& L = live_[fi][ti];
        if (fwd) {
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.live_sym, up, cfg_.lot, px, L.token);
            if (ledger_fn_) ledger_fn_(leg_engine_(fi, ti), cfg_.live_sym, up, L.entry, px, cfg_.lot, L.entry_ts, ts_sec, reason);
            const double p = up ? (px - L.entry) : (L.entry - px);   // raw price-pts; BE-floor -> p>=0 (neg=0)
            fwd_[fi][ti].pts += p; fwd_[fi][ti].clips += 1; fwd_[fi][ti].wins += (p > 1e-6 ? 1 : 0);
            save_fwd_book_();
            // completed trade -> push to the trades list, persist, then reset leg below.
            const double dpp = cfg_.dpp_per_lot * cfg_.lot;
            Closed rec{fi, ti, L.entry, px, p, p * dpp, L.entry_ts, ts_sec, reason};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[AUIDX][CLOSE] %s %s entry=%.2f exit=%.2f pts=%.2f (%s)\n",
                        leg_engine_(fi, ti).c_str(), up ? "LONG" : "SHORT", L.entry, px, p, reason);
            std::fflush(stdout);
        }
        L.has_entry = false; L.wm = 0; L.token.clear(); L.entry_ts = 0;
    }

    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int fi = -1, ti = -1; double pts = 0; int clips = 0, wins = 0;
        std::string line;
        while (std::getline(f, line)) {
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

    // ── CLOSED forward trades log: append one CSV row per completed clip; reload last N at ctor. ──
    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.fi << "," << r.ti << "," << r.entry << "," << r.exit << "," << r.pts << ","
          << r.usd << "," << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            if (std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^\n]",
                            &r.fi, &r.ti, &r.entry, &r.exit, &r.pts, &r.usd,
                            (long long*)&r.ets, (long long*)&r.xts, reason) >= 8
                && r.fi >= 0 && r.fi < 2 && r.ti >= 0 && r.ti < NT_) {
                r.reason = reason;
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

    // ── persist / restore OPEN arm-state (window flags + open legs) — same fix as Gold/FxBeFloorCompanion:
    //   win_/live_ were RAM-only, so every VPS restart wiped an in-progress 2h window/legs and the symbol
    //   re-zeroed to idle. Snapshot on every live bar (atomic tmp+rename), reload at construction. ──
    void load_live_state_() noexcept {
        std::ifstream f(cfg_.live_path);
        if (!f.is_open()) return;
        std::string kind;
        while (f >> kind) {
            if (kind == "win") { int w0 = 0, w1 = 0; f >> w0 >> w1; win_[0] = (w0 != 0); win_[1] = (w1 != 0); }
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

// ── registry: owns the symbols, writes the merged aggregate index_companion_state.json ──
//   (served by /api/index_companion -> desk INDEX COMPANIONS panel). Mirrors the FX companion
//   book's single-file publish. Atomic remove-then-rename (Windows-safe).
class IndexBeFloorBook {
public:
    void add(IndexBeFloorSym::Config c) { syms_.emplace_back(std::move(c)); }

    IndexBeFloorSym* find(const std::string& sym) {
        for (auto& s : syms_) if (s.sym() == sym) return &s;
        return nullptr;
    }

    size_t seed_sym(const std::string& sym, const std::string& csv) {
        if (auto* s = find(sym)) return s->seed_from_h1_csv(csv);
        return 0;
    }
    // Reload every symbol's persisted LIVE forward bars (call AFTER warmup seed, BEFORE finalize_all).
    size_t seed_dumps_all() { size_t n = 0; for (auto& s : syms_) n += s.seed_dump(); return n; }
    // Wire the live order path into EVERY symbol (call after add(), before live).
    void set_exec(IndexBeFloorSym::OpenFn o, IndexBeFloorSym::CloseFn c,
                  IndexBeFloorSym::GateFn g, IndexBeFloorSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }
    void finalize_all() { for (auto& s : syms_) s.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed H1 bar for `sym`. Rewrites the aggregate.
    void on_h1_bar(const std::string& sym, int64_t ts_sec, double close) {
        if (auto* s = find(sym)) { s->on_h1_bar(ts_sec, close); recompute_and_write(); }
    }

    std::string state_json() const {
        std::ostringstream o;
        int64_t last_ts = 0;
        for (const auto& s : syms_) last_ts = std::max(last_ts, s.last_ts());
        o << "{\"engine\":\"index-befloor-pos-neg\",\"shadow\":true,\"syms\":[";
        for (size_t i = 0; i < syms_.size(); ++i) {
            if (i) o << ",";
            o << syms_[i].sym_json();
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
    std::vector<IndexBeFloorSym> syms_;
    std::string state_path_ = "index_companion_state.json";   // served by /api/index_companion (cwd = C:\Omega)
};

// Singleton — accessor mirrors omega::fx_befloor_book().
inline IndexBeFloorBook& index_befloor_book() noexcept {
    static IndexBeFloorBook inst;
    return inst;
}

} // namespace omega
