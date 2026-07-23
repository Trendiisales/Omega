#pragma once
// Dma200GateFeed — single-source daily-close 200DMA regime gate for the index-ladder
// bull gates (S-2026-07-23; gate-source-robustness fix after the US500 own-H1 vs
// external-daily all-6 flip).
//
// Contract:
//   * Reads a "ts_sec,close" daily file (data/ndx_close_hist.csv / spx_close_hist.csv),
//     full-rewritten nightly by the OmegaMacroRegime VPS task (tools/fetch_macro_regime.py)
//     — ONE consistent source, no scale seam by construction (whole-file rewrite).
//   * FAIL-CLOSED: file missing, unreadable, < 210 rows, or last row older than
//     stale_days -> bull() returns false (blocks NEW ladder windows; open legs unaffected
//     — same graceful semantics as the BigCapHi52 spy gate).
//   * HYSTERESIS (the robustness lever): gate turns ON only above ma*on_mult and OFF only
//     below ma*off_mult — kills the source-sensitivity of days hugging the MA line.
//     on_mult=off_mult=1.0 degrades to the plain prior-day close>MA200 rule.
//   * mtime-cached: re-parses only when the file changes (gate is called per H1 bar).
//   * PRIOR-DAY discipline: the newest row is used as-is; the nightly task writes only
//     completed sessions, so the newest row IS the prior close at any intraday call time
//     (no look-ahead).
#include <cstdio>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace omega {

class Dma200GateFeed {
public:
    Dma200GateFeed(std::string path, double on_mult = 1.0, double off_mult = 1.0,
                   int stale_days = 6)
        : path_(std::move(path)), on_mult_(on_mult), off_mult_(off_mult),
          stale_days_(stale_days) {}

    // Thread-safe; callable from any engine/ladder thread.
    bool bull(int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        refresh_();
        if (closes_.size() < 210) { state_ = false; return false; }          // fail-CLOSED
        if (now_sec > 0 && last_ts_ > 0 &&
            now_sec - last_ts_ > (int64_t)stale_days_ * 86400) {              // stale feed
            state_ = false; return false;
        }
        double s = 0; const size_t n = closes_.size();
        for (size_t i = n - 200; i < n; ++i) s += closes_[i];
        const double ma = s / 200.0, c = closes_.back();
        if (!state_ && c > ma * on_mult_)  state_ = true;                     // hysteresis ON
        if ( state_ && c < ma * off_mult_) state_ = false;                    // hysteresis OFF
        return state_;
    }

    // BEAR-side latch (S-2026-07-23, NAS100 short twin): its OWN state machine —
    // NOT !bull(): the hysteresis dead-zone means both latches can be off, and a
    // stale feed must fail CLOSED for the short book too (bear=false -> windows
    // blocked). ON when close < ma*off_mult (0.99 at the certified band), OFF when
    // close > ma*on_mult (1.01) — the exact state-machine inverse the cert ran.
    bool bear(int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        refresh_();
        if (closes_.size() < 210) { bear_state_ = false; return false; }     // fail-CLOSED
        if (now_sec > 0 && last_ts_ > 0 &&
            now_sec - last_ts_ > (int64_t)stale_days_ * 86400) {
            bear_state_ = false; return false;
        }
        double s = 0; const size_t n = closes_.size();
        for (size_t i = n - 200; i < n; ++i) s += closes_[i];
        const double ma = s / 200.0, c = closes_.back();
        if (!bear_state_ && c < ma * off_mult_) bear_state_ = true;          // bear ON
        if ( bear_state_ && c > ma * on_mult_)  bear_state_ = false;         // bear OFF
        return bear_state_;
    }

private:
    void refresh_() {
        struct stat st{};
        if (stat(path_.c_str(), &st) != 0) { closes_.clear(); last_ts_ = 0; return; }
        if ((int64_t)st.st_mtime == mtime_) return;                          // unchanged
        mtime_ = (int64_t)st.st_mtime;
        closes_.clear(); last_ts_ = 0;
        FILE* f = std::fopen(path_.c_str(), "r");
        if (!f) return;
        char line[128];
        while (std::fgets(line, sizeof line, f)) {
            long long ts = 0; double c = 0;
            if (std::sscanf(line, "%lld,%lf", &ts, &c) == 2 && c > 0) {
                closes_.push_back(c); last_ts_ = (int64_t)ts;
                if (closes_.size() > 420) closes_.pop_front();
            }
        }
        std::fclose(f);
    }

    std::string path_;
    double on_mult_, off_mult_;
    int stale_days_;
    std::mutex mu_;
    std::deque<double> closes_;
    int64_t last_ts_ = 0, mtime_ = -1;
    bool state_ = false;        // bull hysteresis latch (starts risk-off = fail-closed)
    bool bear_state_ = false;   // bear latch (starts blocked = fail-closed for the short book)
};

} // namespace omega
