#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OmegaBeCascadeBook — Omega (namespace omega) wrapper over the vendored crypto-
// validated BE-CASCADE engine (chimera::UpJumpLadderCompanion, frozen in
// include/BeCascadeCompanionEngine.hpp). ONE book holds every wired cell (7 non-
// stock + 39 bigcap/rdagent stocks = 46), keyed by live symbol; each is fed from
// its asset-class dispatch site (tick_fx / tick_indices / tick_gold H1; stock-daily
// close loop). Config is byte-identical to backtest/omega_becascade_bt.cpp run():
// mimic_floor + mimic_stagger + stagger_mode=1 BE_CASCADE + stagger_be_bp=20 +
// reclip=0 + confirm=0 + loss_cut_bp (per cell). Per-cell RT bp = the REAL round
// trip cost (NOT the crypto 28bp proxy). Judged STANDALONE (own book, own cost) —
// SEPARATE INDEPENDENT SHADOW book, never touches a parent (feedback-companion-
// independent-engine). Emits ClipRecords → shadow ledger via the injected clip_fn_.
//
// ADVERSE-PROTECTION: inherited from the engine banner — floored cascade, cold-loss
// cut (lc150) + post-arm BE floor; worst clip ~-155bp across all cells (validated on
// the original 47-cell wire; 46 cells since one non-stock cell was dropped).
//
// COST-GATE: the RT cost is BAKED IN (round_trip_bp debited on every clip's net_bp_real
// at fire time) — this is the entry-cost gate for a shadow clip book. It does NOT route
// through OmegaCostGuard/ExecutionCostGuard because it never places a live order; it is
// a paper/shadow ledger. Documented in scripts/ungated_engine_allowlist.txt.
// ─────────────────────────────────────────────────────────────────────────────
#include "BeCascadeCompanionEngine.hpp"   // chimera::UpJumpLadderCompanion (vendored, frozen)
#include <unordered_map>
#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <chrono>

namespace omega {

class BeCascadeBook {
public:
    // clip sink: (live_sym, net_bp_real, entry_px, exit_px, mfe_pct, entry_ts_ms, exit_ts_ms, reason)
    using ClipFn = std::function<void(const std::string&, double, double, double, double,
                                      int64_t, int64_t, const std::string&)>;

    void set_clip_fn(ClipFn f) { clip_fn_ = std::move(f); }

    // Build one cell. W=det window (bars), thr=up-jump fraction, rt=RT cost bp,
    // tf_secs=3600 H1 / 86400 daily, legs=cap (8), g=uniform giveback (0.5),
    // confirm_bp=BE-ENTRY threshold in bp (0 => auto 3x rt). rank_out=true => detector
    // stays warm (windows tracked) but the cell REFUSES to arm new legs (books nothing).
    // Used for stock names that pass on OHLC backtest but fail/0-trade on the live
    // close-only feed — kept warm for a future re-rank, mirroring omega::stockmover_
    // ladder_book()'s ranked-out handling.
    //
    // NEVER-PRE-BE-LOSS (S-2026-07-17, feedback-no-prebe-loss-ever): every cell is a
    // BE-ENTRY floored mimic. A leg stays FLAT (books nothing, no cost) until fav>=confirm_bp
    // (>= rt = the BE cost); it then opens with confirm_anchor_epx=true, i.e. le stays = epx
    // (the window entry) so hwm=cur >= le*(1+rt) => the leg is floored ON OPEN at BE and its
    // worst clip is net>=0. loss_cut_bp=0: there is NO pre-arm cut because a leg never opens
    // below BE. This REPLACES the prior confirm_bp=0 + loss_cut_bp=150 config, whose PREBE_CUT
    // path could book a ~-155bp clip before break-even (the forbidden immediate-entry+pre-BE-loss
    // pattern). Validated worst-clip>=0 (nNeg=0) + standalone net-positive (WF both halves,
    // omit-2022, base+2x cost) across all 23 ACTIVE cells: backtest/omega_becascade_prebe_bt.cpp.
    void add_cell(const std::string& live_sym, int W, double thr, double rt,
                  int tf_secs, int legs = 8, double g = 0.5, double confirm_bp = 0.0,
                  bool rank_out = false) {
        chimera::UpJumpLadderCompanion::Config c;
        const double conf = (confirm_bp > 0.0) ? confirm_bp : rt * 3.0;   // BE-ENTRY threshold (>rt); 3x rt = crypto-validated margin
        c.parent_tag   = "SELF";               // self-triggered (no external parent leg)
        c.tag          = "OMEGA-BC-" + live_sym;
        c.symbol       = live_sym;
        c.det_w        = W;
        c.det_thr      = thr;
        c.tf_secs      = tf_secs;
        c.round_trip_bp= rt;
        c.mimic_floor  = true;                 // post-arm BE floor (never book negative after arming)
        c.mimic_stagger= true;
        c.stagger_mode = 1;                    // BE_CASCADE: release next leg only once every open leg is BE
        c.stagger_be_bp= 20.0;
        c.reclip_pct   = 0.0;                  // no self-funding ladder beyond the staggered set
        c.loss_cut_bp  = 0.0;                  // NEVER-PRE-BE-LOSS: no pre-arm cut (leg never opens below BE)
        c.confirm_bp   = conf;                 // BE-ENTRY: leg opens only once fav>=conf (>=rt)
        c.confirm_anchor_epx = true;           // le=epx -> floored ON OPEN at BE => worst clip net>=0
        c.be_floor     = false;                // mimic_floor path (not the be_floor trail mode)
        c.mimic_giveback = g;
        c.tight = {0.2, 0, 0.0, 0, 0.0};       // Tier{arm,stall,gb,trail_bp,confirm}
        c.wide  = {0.2, 0, 0.0, 0, 0.0};
        for (int k = 2; k < legs; ++k) c.extra_base.push_back({0.2, 0, 0.0, 0, 0.0});
        c.cap   = legs;

        auto cell = std::make_unique<chimera::UpJumpLadderCompanion>(c);
        if (rank_out) cell->set_rank_out(true);   // warm detector, refuse-to-arm
        const std::string sym = live_sym;
        cell->set_on_clip([this, sym](const chimera::UpJumpLadderCompanion::ClipRecord& r) {
            // seeding_ = replaying history to prime the detector: prime, don't book.
            if (seeding_.load()) return;
            if (clip_fn_) clip_fn_(sym, r.net_bp_real, r.entry_px, r.exit_px, r.mfe_pct,
                                   r.entry_ts_ms, r.exit_ts_ms, r.reason);
        });
        cells_[live_sym] = std::move(cell);
    }

    // Per-bar drive (H1 or daily — the cell's tf_secs governs bar bucketing).
    // ts_sec = bar-close epoch seconds; the engine wants ms.
    void on_bar(const std::string& live_sym, int64_t ts_sec, double /*h*/, double l, double c) noexcept {
        auto it = cells_.find(live_sym);
        if (it == cells_.end() || !it->second) return;
        const int64_t ts_ms = ts_sec * 1000;
        it->second->stop_check_only(l, ts_ms);      // intrabar low → cold-loss / floor stop
        it->second->observe(true, 0.0, c, ts_ms);   // self-triggered detector + leg management
    }

    bool has(const std::string& live_sym) const { return cells_.count(live_sym) != 0; }
    size_t size() const { return cells_.size(); }

    std::vector<std::string> symbols() const {
        std::vector<std::string> v; v.reserve(cells_.size());
        for (auto& kv : cells_) v.push_back(kv.first);
        return v;
    }

    // ── STOCK daily-close poller ─────────────────────────────────────────────
    // Reads the wide close-only matrix (data/rdagent/sp500_long_close.csv: date rows,
    // name columns) every poll_ms, dispatches each present stock cell's daily close as
    // a close-only bar (h=l=c=close — the live feed carries no intraday H/L, matching
    // BigCap2pctImpulse). First load replays all history under seeding_ (primes the
    // det window, books nothing); only genuinely-new rows thereafter book live. Mirrors
    // BigCapImpulseBook::start_poller(). Non-stock cells (FX/index/gold) are fed inline
    // from their tick dispatchers instead — this poller only touches cells it finds columns for.
    ~BeCascadeBook() { stop_poller(); }
    void start_poller(const std::string& rel, int poll_ms = 900000 /*15min*/) {
        if (running_.exchange(true)) return;
        path_ = rel; poll_ms_ = poll_ms;
        thread_ = std::thread([this]{ poll_loop_(); });
    }
    void stop_poller() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

private:
    static int64_t days_from_civil_(int y, unsigned m, unsigned d) noexcept {
        y -= (m <= 2);
        const int64_t era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = (unsigned)(y - era * 400);
        const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097LL + (int64_t)doe - 719468LL;
    }
    static int64_t parse_date_ts_(const std::string& s) noexcept {
        int y = 0, m = 0, d = 0;
        if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 0;
        if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
        return days_from_civil_(y, m, d) * 86400LL;
    }
    void poll_once_() {
        std::ifstream f(path_);
        if (!f) return;
        std::string ln;
        if (!std::getline(f, ln)) return;               // header
        std::vector<size_t> colcell;                    // csv col -> wired stock symbol (or empty)
        std::vector<std::string> colname;
        { std::stringstream hs(ln); std::string tok; while (std::getline(hs, tok, ','))
              colname.push_back(tok); }
        // rows
        while (std::getline(f, ln)) {
            const size_t comma = ln.find(',');
            if (comma == std::string::npos) continue;
            const int64_t ts = parse_date_ts_(ln.substr(0, comma));
            if (ts == 0 || ts <= last_ts_) continue;     // already processed
            const bool live = (last_ts_ != 0) || is_last_row_(ts);  // first load: only newest books
            seeding_.store(!live);
            std::stringstream ls(ln); std::string tok; int ci = 0;
            while (std::getline(ls, tok, ',')) {
                if (ci > 0 && ci < (int)colname.size() && !tok.empty()) {
                    auto it = cells_.find(colname[ci]);
                    if (it != cells_.end() && it->second) {
                        char* end = nullptr; const double v = std::strtod(tok.c_str(), &end);
                        if (end != tok.c_str() && v > 0.0)
                            on_bar(colname[ci], ts, v, v, v);   // close-only
                    }
                }
                ++ci;
            }
            newest_ts_ = ts;
        }
        seeding_.store(false);
        last_ts_ = newest_ts_;
    }
    // On the FIRST load last_ts_==0: we must replay all-but-newest as seed. We don't know
    // the newest ts until the file is read, so a two-pass is cleanest — but a single pass
    // with a pre-scan of the max ts keeps it simple.
    bool is_last_row_(int64_t ts) const { return ts == file_max_ts_; }
    void prescan_max_ts_() {
        std::ifstream f(path_); if (!f) return; std::string ln;
        if (!std::getline(f, ln)) return;
        int64_t mx = 0;
        while (std::getline(f, ln)) {
            const size_t c = ln.find(','); if (c == std::string::npos) continue;
            const int64_t ts = parse_date_ts_(ln.substr(0, c)); if (ts > mx) mx = ts;
        }
        file_max_ts_ = mx;
    }
    void poll_loop_() {
        prescan_max_ts_();
        while (running_.load()) {
            poll_once_();
            for (int slept = 0; slept < poll_ms_ && running_.load(); slept += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::unordered_map<std::string, std::unique_ptr<chimera::UpJumpLadderCompanion>> cells_;
    ClipFn clip_fn_;
    std::atomic<bool> seeding_{false};
    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::string       path_;
    int               poll_ms_    = 900000;
    int64_t           last_ts_    = 0;
    int64_t           newest_ts_  = 0;
    int64_t           file_max_ts_= 0;
};

// Singleton — accessor mirrors omega::fx_upjump_ladder_book().
inline BeCascadeBook& be_cascade_book() noexcept {
    static BeCascadeBook b;
    return b;
}

} // namespace omega
