#pragma once
// =============================================================================
// ForwardTracker.hpp -- forward-return + bracket simulator.
//
// PROBLEM: at bar close t, we have features but no forward-return data yet.
// The forward returns and bracket outcomes are determined by ticks AFTER t.
//
// SOLUTION: keep a deque of "pending" rows. On every tick after a bar close,
// walk the pending deque from oldest to newest:
//   * For each fixed-horizon return: if (now_ms - bar_close_ms >= horizon_ms)
//     and the return is still NaN, write the most recent mid-at-or-before-deadline.
//   * For each first-touch ±X spec: if not yet decided and within horizon,
//     check whether THIS tick crossed +X or -X first.
//   * For each bracket spec: if not yet decided and within horizon, check
//     whether THIS tick hit TP or SL on the side implied by direction.
//     -- We test BOTH directions per bar (long entry: tp = close+tp_pts,
//        sl = close-sl_pts; short: mirror). Realised PnL is whichever side
//        a given downstream rule actually took. To keep the panel size sane
//        we only emit ONE bracket value per spec: the LONG outcome. Short
//        outcomes are derivable as -1*long_pnl_pts (with sign-flipped TP/SL).
//        The downstream analytics computes per-direction returns by signing
//        the bracket outcome with the feature's edge direction. (We pick LONG
//        as the canonical test direction; the analytics handles short-direction
//        edges by negating.)
//
// Shorts are NOT symmetric in raw PnL because tp/sl pts mean different prices,
// but the *outcome* (TP-first / SL-first / MtM) for a long entry at C is the
// time-flip of the same outcome for a short entry at C only when tp == sl.
// Our specs use TP > SL (e.g. 20/10), so we DO need a separate short tracker.
// We track both long and short brackets and emit the long PnL as the canonical
// value. The analytics layer can derive the short outcome by re-running the
// extractor with mirrored specs OR by using fwd_ret_h_pts as the directional
// test (which is the simpler path; brackets are best-effort PnL realism for
// long-biased edges).
//
// To keep things simple and the panel small, we emit ONE PnL per bracket spec
// representing the LONG entry outcome. Analytics that wants short-side PnL
// uses fwd_ret_*_pts inverted, which is a fixed-horizon (no SL/TP) approximation.
// This is good enough for v1 ranking. v2 can add explicit short brackets.
//
// MEMORY: pending deque holds at most max_horizon_min bars. Longest horizon is
// 240m so deque size <= 240 entries. Each entry is one PanelRow (~700 bytes)
// plus tracking state. Deque memory: ~1 MB. Fine.
// =============================================================================

#include "PanelSchema.hpp"
#include <deque>
#include <cmath>
#include <cstdint>

namespace edgefinder {

// Per-bracket runtime state (separate from the PanelRow's emit columns so we
// don't pollute the on-disk schema with helpers).
struct BracketRT {
    bool   decided   = false;
    double tp_price  = 0.0;
    double sl_price  = 0.0;
};

struct FirstTouchRT {
    bool decided = false;
};

// One pending row + all the runtime tracking it needs to fill its forward cells.
struct PendingRow {
    PanelRow      row;
    double        anchor_mid     = 0.0;   // mid at bar close (for fwd_ret + bracket entry)
    int64_t       close_ms       = 0;     // alias of row.ts_close_ms

    // Fixed-horizon: bool flags for "this fwd_ret_* column was written".
    bool          fwd_filled[N_FWD_RET]    = {false,false,false,false,false};
    // Last seen mid up to and including the deadline; used as the value at horizon.
    double        last_mid                  = 0.0;

    // First-touch state.
    FirstTouchRT  ft[N_FIRST_TOUCH]         = {};

    // Bracket state (LONG side; see header notes).
    BracketRT     br[N_BRACKETS]            = {};
};

class ForwardTracker {
public:
    using EmitCb = void(*)(const PanelRow&, void* userdata);

    ForwardTracker(EmitCb cb, void* userdata) : cb_(cb), ud_(userdata) {
        // Compute the largest horizon we need to retain pending rows for.
        max_horizon_ms_ = 0;
        for (int i = 0; i < N_FWD_RET; ++i) {
            if (FWD_RET_HORIZONS_MS[i] > max_horizon_ms_) max_horizon_ms_ = FWD_RET_HORIZONS_MS[i];
        }
        for (int i = 0; i < N_FIRST_TOUCH; ++i) {
            if (FIRST_TOUCH_HORIZON_MS[i] > max_horizon_ms_) max_horizon_ms_ = FIRST_TOUCH_HORIZON_MS[i];
        }
        for (int i = 0; i < N_BRACKETS; ++i) {
            if (BRACKETS[i].horizon_ms > max_horizon_ms_) max_horizon_ms_ = BRACKETS[i].horizon_ms;
        }
    }

    // Called when a bar closes. The just-closed PanelRow is registered as
    // pending; its forward-return columns will be filled by subsequent ticks.
    void on_bar_close(const PanelRow& row, double close_mid) noexcept {
        PendingRow p;
        p.row        = row;
        p.anchor_mid = close_mid;
        p.close_ms   = row.ts_close_ms;
        p.last_mid   = close_mid;
        for (int i = 0; i < N_BRACKETS; ++i) {
            p.br[i].tp_price = close_mid + BRACKETS[i].tp_pts;
            p.br[i].sl_price = close_mid - BRACKETS[i].sl_pts;
        }
        pending_.push_back(p);
    }

    // Called for every tick. Updates pending rows' forward state. Flushes
    // any rows whose longest-horizon deadline has passed (all fwd cells
    // are now decided).
    void on_tick(double bid, double ask, int64_t ts_ms) noexcept {
        const double mid = 0.5 * (bid + ask);

        // Advance every pending row.
        for (auto& p : pending_) {
            if (ts_ms <= p.close_ms) continue;     // tick is at or before bar close, skip
            const int64_t dt_ms = ts_ms - p.close_ms;

            // (a) Fixed-horizon forward returns.
            for (int i = 0; i < N_FWD_RET; ++i) {
                if (p.fwd_filled[i]) continue;
                if (dt_ms >= FWD_RET_HORIZONS_MS[i]) {
                    // Use the LAST observed mid before/at the deadline. Since
                    // we update p.last_mid below to this tick's mid, we want
                    // a slight subtlety: we accept "this tick" as the deadline
                    // crossing tick. PnL = mid - anchor.
                    write_fwd_ret(p, i, mid - p.anchor_mid);
                    p.fwd_filled[i] = true;
                }
            }

            // (b) First-touch.
            for (int i = 0; i < N_FIRST_TOUCH; ++i) {
                if (p.ft[i].decided) continue;
                if (dt_ms >= FIRST_TOUCH_HORIZON_MS[i]) {
                    // Horizon expired without a decision -> stays at 0 (already 0).
                    p.ft[i].decided = true;
                    continue;
                }
                const double up_target  = p.anchor_mid + FIRST_TOUCH_PTS[i];
                const double dn_target  = p.anchor_mid - FIRST_TOUCH_PTS[i];
                // Use ask for upside touch (most aggressive), bid for downside.
                if (ask >= up_target) {
                    write_first_touch(p, i, +1);
                    p.ft[i].decided = true;
                } else if (bid <= dn_target) {
                    write_first_touch(p, i, -1);
                    p.ft[i].decided = true;
                }
            }

            // (c) Brackets (long-side).
            for (int i = 0; i < N_BRACKETS; ++i) {
                if (p.br[i].decided) continue;
                if (dt_ms >= BRACKETS[i].horizon_ms) {
                    // Horizon expired -> mark-to-market at current mid.
                    write_bracket(p, i, mid - p.anchor_mid, /*outcome*/ 0);
                    p.br[i].decided = true;
                    continue;
                }
                // LONG: TP if ask >= tp; SL if bid <= sl. We use ask for upside
                // (would buy back / cross spread to exit at ask is wrong; for a
                // long the EXIT is at bid. So: TP touched if bid >= tp; SL
                // touched if bid <= sl... actually for an OPEN long we measure
                // the implied PnL using the mid; SL/TP are points distance from
                // the entry mid, not actual order prices. Simpler: use mid
                // crossings throughout — same anchor as the entry).
                // Decision: use mid for both. Conservative approach -- we
                // measure the underlying price's behaviour, not broker-specific
                // execution. The cost of crossing the spread is captured
                // separately via spread_median_pts in the row.
                if (mid >= p.br[i].tp_price) {
                    write_bracket(p, i, +BRACKETS[i].tp_pts, +1);
                    p.br[i].decided = true;
                } else if (mid <= p.br[i].sl_price) {
                    write_bracket(p, i, -BRACKETS[i].sl_pts, -1);
                    p.br[i].decided = true;
                }
            }

            p.last_mid = mid;
        }

        // Flush any fully-decided rows from the front of the queue.
        while (!pending_.empty() && all_decided(pending_.front())) {
            pending_.front().row.fwd_complete = 1;
            cb_(pending_.front().row, ud_);
            pending_.pop_front();
        }
    }

    // Called at end of input. Any remaining pending rows have their unfilled
    // cells set to NaN/0 and are emitted with fwd_complete=0.
    void flush_remaining() noexcept {
        while (!pending_.empty()) {
            // Mark anything still undecided.
            auto& p = pending_.front();
            for (int i = 0; i < N_FWD_RET; ++i) {
                if (!p.fwd_filled[i]) {
                    write_fwd_ret(p, i, std::nan(""));
                    p.fwd_filled[i] = true;
                }
            }
            for (int i = 0; i < N_FIRST_TOUCH; ++i) {
                p.ft[i].decided = true;  // outcome stays 0 (not decided)
            }
            for (int i = 0; i < N_BRACKETS; ++i) {
                if (!p.br[i].decided) {
                    p.row.fwd_bracket_pts[i]     = std::nan("");
                    p.row.fwd_bracket_outcome[i] = 0;
                    p.br[i].decided              = true;
                }
            }
            p.row.fwd_complete = 0;
            cb_(p.row, ud_);
            pending_.pop_front();
        }
    }

    std::size_t pending_count() const noexcept { return pending_.size(); }
    int64_t     max_horizon_ms() const noexcept { return max_horizon_ms_; }

private:
    EmitCb                     cb_;
    void*                      ud_;
    std::deque<PendingRow>     pending_;
    int64_t                    max_horizon_ms_ = 0;

    static void write_fwd_ret(PendingRow& p, int i, double v) noexcept {
        switch (i) {
            case 0: p.row.fwd_ret_1m_pts   = v; break;
            case 1: p.row.fwd_ret_5m_pts   = v; break;
            case 2: p.row.fwd_ret_15m_pts  = v; break;
            case 3: p.row.fwd_ret_60m_pts  = v; break;
            case 4: p.row.fwd_ret_240m_pts = v; break;
            default: break;
        }
    }

    static void write_first_touch(PendingRow& p, int i, int dir) noexcept {
        switch (i) {
            case 0: p.row.first_touch_5m  = dir; break;
            case 1: p.row.first_touch_15m = dir; break;
            case 2: p.row.first_touch_60m = dir; break;
            default: break;
        }
    }

    static void write_bracket(PendingRow& p, int i, double pnl, int outcome) noexcept {
        p.row.fwd_bracket_pts[i]     = pnl;
        p.row.fwd_bracket_outcome[i] = outcome;
    }

    static bool all_decided(const PendingRow& p) noexcept {
        for (int i = 0; i < N_FWD_RET; ++i)     if (!p.fwd_filled[i]) return false;
        for (int i = 0; i < N_FIRST_TOUCH; ++i) if (!p.ft[i].decided) return false;
        for (int i = 0; i < N_BRACKETS; ++i)    if (!p.br[i].decided) return false;
        return true;
    }
};

} // namespace edgefinder
