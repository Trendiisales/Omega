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
// tiers per direction (20bp banker / 150bp runner). Judge STANDALONE (net>0, both WF
// halves) — NEVER vs a parent / vs riding WIDE.
//
// ADVERSE-PROTECTION: BE-FLOOR — a leg stays FLAT until price clears +be_bp from its
//   ref (covers RT cost), opens THERE, and its stop sits at-or-above entry (long) /
//   at-or-below entry (short) and only trails favourably. Therefore exit >= entry
//   (long) / <= entry (short) ALWAYS => net_bp >= 0 on EVERY clip BY CONSTRUCTION.
//   A cold-loss cut is structurally impossible and unnecessary. Backtested vs
//   backtest/fx_befloor_ls.py: neg=0 every pair/config, both WF halves +. GBPUSD
//   2022-H2 (Truss/GBP-crisis, independent regime) Neg +52% both halves. Verdict =
//   BE-floor (no cut), not skipped (feedback-engine-loss-protection-provision).
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
        double notional     = 100000.0;   // std-lot notional -> 1% == $1000 (USD-quote major)
        double lot          = 1.0;
        std::string deploy_path;          // per-pair persisted deploy-forward anchor
    };
    struct Tier { double gb_bp; const char* tag; };

    explicit FxBeFloorPair(Config c) : cfg_(std::move(c)) {
        if (cfg_.deploy_path.empty())
            cfg_.deploy_path = "fx_companion_" + lower_(cfg_.pair) + "_deploy_ts.txt";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
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
    }

    // Emit this pair's desk JSON object: {"pair":..,"bars":..,"deploy_ts":..,"pct":..,"usd":..,"flavors":[..]}
    // Flavors named <PAIR>Pos / <PAIR>Neg (research convention, distinct from gold AUPOS/AUNEG).
    std::string pair_json() const {
        const double usd_per_pct = cfg_.notional / 100.0 * cfg_.lot;   // %-point -> USD
        const Tier tiers[2] = { {20.0, "banker"}, {150.0, "runner"} };
        struct Flavor { const char* suffix; const char* dir; bool is_long; };
        const Flavor flavors[2] = { {"Pos", "long", true}, {"Neg", "short", false} };

        std::ostringstream o; o << std::fixed;
        const int64_t last_ts = ts_.empty() ? 0 : ts_.back();
        double pair_pct = 0.0;
        std::ostringstream fl;
        for (int fi = 0; fi < 2; ++fi) {
            const auto trades = detect_(flavors[fi].is_long);
            double book_pct = 0.0;
            std::ostringstream comps;
            for (int ti = 0; ti < 2; ++ti) {
                const BookRes b = book_(trades, flavors[fi].is_long, tiers[ti].gb_bp);
                book_pct += b.pct;
                if (ti) comps << ",";
                comps.precision(0); comps << std::fixed;
                comps << "{\"tier\":\"" << tiers[ti].tag << "\",\"gb_bp\":" << tiers[ti].gb_bp
                      << ",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
                comps.precision(3); comps << "\"pct\":" << b.pct << ",";
                comps.precision(0); comps << "\"usd\":" << (b.pct * usd_per_pct) << "}";
            }
            pair_pct += book_pct;
            if (fi) fl << ",";
            fl.precision(0); fl << std::fixed;
            fl << "{\"name\":\"" << cfg_.pair << flavors[fi].suffix << "\",\"dir\":\"" << flavors[fi].dir
               << "\",\"events\":" << trades.size() << ",";
            fl.precision(3); fl << "\"book_pct\":" << book_pct << ",";
            fl.precision(0); fl << "\"book_usd\":" << (book_pct * usd_per_pct) << ",";
            fl << "\"companions\":[" << comps.str() << "]}";
        }
        o << "{\"pair\":\"" << cfg_.pair << "\",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_ << ",\"ts\":" << (long long)last_ts << ",";
        o.precision(3); o << "\"pct\":" << pair_pct << ",";
        o.precision(0); o << "\"usd\":" << (pair_pct * usd_per_pct) << ",";
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

    void ingest_(int64_t ts, double close) noexcept { ts_.push_back(ts); c_.push_back(close); }
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
