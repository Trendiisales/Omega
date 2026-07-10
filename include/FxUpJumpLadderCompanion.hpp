#pragma once
// =============================================================================
// FxUpJumpLadderCompanion — per-pair FX H1 JUMP LADDER companion books
// (long UP-JUMP: EURUSD / GBPUSD / NZDUSD / AUDUSD; short DOWN-JUMP mirror:
// USDCAD, S-2026-07-08c). The FX member of the no-floor ladder family (BIGCAP
// daily = StockDayMoverLadderCompanion; this is the H1 port the S-2026-07-07x
// instrument sweep validated; the short mirror is the S-2026-07-08 both-ways
// sweep's first genuine FX short-side pass).
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math   : backtest/omega_upjump_ladder_bt.py run() (S-2026-07-07x, operator
//            order 3) — detector: close rises >= thr% off the min LOW of the
//            last W H1 bars -> window of W bars. At the trigger close spawn
//            TIGHT (arm 0.17thr, abs trail 0.67thr) + WIDE (arm 2.7thr, g50)
//            + STACKED arms {0.67,1.33,2.0}thr each g50; reclip a further WIDE
//            leg on +1.67thr extension, cap 5 batches/window; LOSS_CUT 5thr
//            per leg pre-arm; window end flushes remaining legs at the close.
//            Manage is INTRABAR via the H1 bar's l -> h -> c (SL-first).
//   result : EURUSD W48 thr0.5 +39.7% PF1.47 n507 (all-9-cells plateau);
//            GBPUSD W48 thr1.0 +37.4% PF2.20 n240 (random control ZERO);
//            NZDUSD W24 thr1.5 +41.2% PF4.35 n100 (thr1.5 plateau). All WF
//            both halves + 2x-cost PASS + beat the 5-seed RANDOM-WINDOW
//            control. AUDUSD NOT wired (marginal 5/9 cells, no robustness
//            pass); USDJPY/USDCAD dead; XAU bull-beta (random captures it);
//            GER40 bull-only (index axis, separate wire). Evidence
//            outputs/OMEGA_UPJUMP_LADDER_2026-07-07.md · vault FxUpJumpLadder.
//   short  : SIGN-MIRROR of the same mechanics (cfg.short_downjump=true), the
//            exact dir=-1 parameterization of backtest/fx_bothways_sweep.py
//            ladder(): window arms when close <= -thr% UNDER the max HIGH of
//            the last W H1 bars; legs SHORT at the trigger close; arms/trails
//            measured from the trough (favorable extreme), clip on bounce-back
//            (abs trail or 50%-of-MFE giveback above the trough); LOSS_CUT 5thr
//            adverse RALLY pre-arm; reclip on a further -1.67thr extension;
//            window end flushes at the close. Intrabar order mirrored h->l->c
//            (adverse extreme first). Validated: USDCAD DNJUMP W96/thr0.5
//            +2241bp PF1.58 n230, WF +1493/+748, ex-best +2007, 2x-cost +1781,
//            over-random +2137, plateau ok (neigh +541; W96/0.75, W72/0.75,
//            W24/0.5, W48/0.5 also pass) — outputs/FX_BOTHWAYS_SWEEP_2026-07-08.md
//            row 5. SINGLE-REGIME caveat (2025 = CAD-strength year, no 2022
//            data) -> wired at HALF the standard notional.
//
// ADVERSE-PROTECTION: backtested verdict (mandate + feedback-engine-loss-
//   protection-provision) — LOSS_CUT at 5*thr adverse per leg PRE-ARM is part
//   of the validated mechanism (the PASS figures are net of its cuts), armed
//   legs are trail-stopped (TIGHT abs; WIDE/STACKED/LADDER a peak-profit giveback
//   trail — S-2026-07-09 tightened from 50%-of-MFE to a wire-configurable
//   wide_gb_frac, wired 0.10 = "keep ~90% of the peak gain", engaged at
//   wide_arm_pct MFE; LADDER_WIDE_TRAIL_TIGHTEN_2026-07-09.md), and the window exit
//   flushes everything at the close (no leg is ever abandoned). SHORT side
//   (S-2026-07-08c): identical protections sign-mirrored — LOSS_CUT 5*thr fires
//   on an adverse RALLY pre-arm, trails clip on bounce-back off the trough,
//   window flush unchanged; the USDCAD +2241bp PF1.58 verdict is net of these
//   cuts (FX_BOTHWAYS_SWEEP_2026-07-08.md; single-regime caveat -> half size).
//   AUTO-RETIREMENT (StockDayMoverLadderCompanion retire_usd latch, same
//   S-2026-07-08c pattern): once a pair's FORWARD real book falls to
//   <= cfg.retire_usd (<0 = enabled), NO new windows arm; open legs still
//   manage/flush. USDCAD short wired at -$580 = 2x worst BT drawdown (maxDD
//   -581bp on the W96/0.5 equity curve, $5k notional -> -$291; 2x ~ -$580).
//
// FILL CONVENTION (in-calibration): trail/LC exits book AT the stop level when
//   an H1 low pierces it (resting-stop convention, same as the research); the
//   window flush books the observed close. Costs: per-clip round-trip
//   rt_cost_bp debited from EVERY clip — single honest column from day one
//   (no model/real split; the PASS survived 2x cost).
//
// FEED: tick_fx.hpp fx_feed_bars() H1 roll -> on_h1_bar(pair, ts, h, l, c).
//   Warm-seed from phase1/signal_discovery/warmup_<PAIR>_H1.csv + own persisted
//   forward dump. Seeding primes the detector only (deploy-forward gate).
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-
// independent-engine). Observe-only, SHADOW: never opens/moves/shrinks/closes
// a real position, never read by any parent. Judged STANDALONE.
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
#include <cmath>
#include <atomic>
#include <chrono>
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

// ── one pair's long-only upjump-ladder book (percent-of-notional based) ──────
class FxLadderPair {
public:
    struct Config {
        std::string pair     = "EURUSD";
        std::string live_sym = "EURUSD";
        int    W             = 48;        // detector lookback + window length (H1 bars)
        double thr           = 0.5;       // upjump threshold, % off the W-bar min low
        double rt_cost_bp    = 2.0;       // REAL round-trip cost (bp of entry) debited per clip
        double notional      = 10000.0;   // $ per clip; USD = pct/100 * notional
        double lot           = 1.0;       // order-path lot (units decided at LIVE flip)
        int    cap           = 5;         // max clip batches per window (1 base + 4 reclips)
        // S-2026-07-09 WIDE-runner PEAK-PROFIT TRAIL retune (operator: keep WIDE a runner but
        //   replace the 50%-of-MFE giveback with a "keep ~90% of the peak gain" trail so a real
        //   winner exits ON THE TURN instead of round-tripping / window-flushing at breakeven).
        //   Applies to the RUNNER tiers WIDE(1)/STACKED(2)/LADDER(3); the TIGHT(0) abs-trail
        //   banker is unchanged. Evidence outputs/LADDER_WIDE_TRAIL_TIGHTEN_2026-07-09.md:
        //   NAS100 arm1/gb10 +7.4% (arm0/gb10 +34.4%), US500 +24%, GER40 +4.9%, NZD +5.6%,
        //   GBP +5.4% — all WF+ both halves, RT 0.20%->0.04%, runner-trail capture = 90.0%.
        //     wide_gb_frac : giveback fraction of the gain -> stop = entry + (1-frac)*(peak-entry).
        //                    <=0 = legacy g50 (0.50). Wired 0.10 (keep 90% of the peak gain).
        //     wide_arm_pct : MFE %-of-entry that ENGAGES the WIDE/LADDER trail. <0 = legacy 2.7*thr.
        //                    Wired +1.0 (confirmed low arm, net-positive every index+FX cell). The
        //                    jump-gated ENTRY already pre-filters chop, so a low arm is safe; arm 0
        //                    (engage-from-entry) is +34% on NAS100 but -5.5% GER40/-6..-13% FX, so
        //                    +1.0 is the robust uniform default (per-symbol arm0 available for NAS).
        //                    STACKED keeps its staggered arm; only its giveback shares wide_gb_frac.
        double wide_gb_frac  = 0.0;       // 0 = legacy g50 (0.50)
        double wide_arm_pct  = -1.0;      // <0 = legacy 2.7*thr engage
        // BE-ENTRY (operator S-2026-07-09b): base/spawn legs stay PENDING at the trigger and open
        // only once price clears +be_entry_pct in favor (cost/break-even covered), then enter THERE.
        // A breakout that fades before covering cost never opens -> no open-into-loss. Cancel a
        // pending leg if BE is not made within pend_bars. 0 = legacy enter-at-trigger.
        double be_entry_pct  = 0.0;       // >0 = wait for +be% before opening
        int    pend_bars     = 4;         // cancel a pending leg if BE not made within this many bars
        // S-2026-07-08c DIRECTION flag: false = long UP-JUMP (original mechanics);
        // true = short DOWN-JUMP sign-mirror (USDCAD; fx_bothways_sweep.py dir=-1).
        bool   short_downjump = false;
        // S-2026-07-08c AUTO-RETIREMENT latch (StockDayMoverLadderCompanion pattern):
        // forward real book <= retire_usd (<0 = enabled) -> no NEW windows arm
        // (open legs manage/flush normally). 0 = disabled.
        double retire_usd     = 0.0;
        std::string file_prefix = "fxladder_companion_";   // persistence-file family (index book overrides)
        // Optional REGIME BLOCK for NEW windows (GER40 bull-gate: bear file 24/24 cells
        // negative -> wire ONLY behind the index risk-off gate, feedback-companion-bull-
        // gate-not-reject). Open legs still manage/flush; only new triggers are blocked.
        std::function<bool()> block_new_windows_fn;
        std::string deploy_path;          // per-pair persisted deploy-forward anchor
        std::string bars_path;            // per-pair persisted LIVE forward H1 bars (ts,h,l,c)
        std::string book_path;            // per-pair persisted REAL forward book (4 tiers)
        std::string live_path;            // per-pair persisted OPEN window+legs state
        std::string closed_path;          // per-pair persisted CLOSED forward clips log
    };

    // Research-locked mechanism ratios (thr-scaled; BIGCAP-derived, swept per-pair):
    static constexpr double T_ARM_M   = 0.17;   // TIGHT arm  = 0.17*thr
    static constexpr double T_TRAIL_M = 0.67;   // TIGHT trail (abs % of entry) = 0.67*thr
    static constexpr double W_ARM_M   = 2.7;    // WIDE arm   = 2.7*thr (g50 trail)
    static constexpr double S_ARM_M[3] = { 0.67, 1.33, 2.0 };   // STACKED arms (thr units)
    static constexpr double RECLIP_M  = 1.67;   // reclip on +1.67*thr extension
    static constexpr double LC_M      = 5.0;    // LOSS_CUT   = 5*thr pre-arm

    // ── LIVE EXECUTION WIRING — identical contract to the ladder/BeFloor family. Set ONLY
    //   in the live main TU; null in a backtest TU -> live_step_ short-circuits. SHADOW
    //   today (send_live_order no-ops while mode!=LIVE), LIVE on flip. ──
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit FxLadderPair(Config c) : cfg_(std::move(c)) {
        d_ = cfg_.short_downjump ? -1.0 : 1.0;
        const std::string s = lower_(cfg_.pair);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = cfg_.file_prefix + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = cfg_.file_prefix + s + "_h1.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = cfg_.file_prefix + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = cfg_.file_prefix + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = cfg_.file_prefix + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();
        load_live_state_();
    }

    const std::string& pair() const { return cfg_.pair; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_pct() const {   // REAL total forward %, net of per-clip cost (the judged column)
        double r = 0.0; for (int ti = 0; ti < NT_; ++ti) r += fwd_[ti].pct; return r;
    }
    double  book_usd() const { return book_pct() / 100.0 * cfg_.notional; }
    // MTM of currently-open legs at the last close, net of cost (parity/end-flush accounting).
    double  open_mtm_pct() const {
        if (c_.empty()) return 0.0;
        const double cur = c_.back(); double r = 0.0;
        for (const Leg& L : legs_)
            if (!L.pending && L.entry > 0.0)
                r += d_ * (cur / L.entry - 1.0) * 100.0 - cfg_.rt_cost_bp / 100.0;
        return r;
    }
    int total_clips() const { int n = 0; for (int ti = 0; ti < NT_; ++ti) n += fwd_[ti].clips; return n; }

    // seed one historical H1 bar (primes detector history; books nothing — deploy-forward gate).
    void seed_bar(int64_t ts_sec, double h, double l, double c) noexcept {
        if (c <= 0.0 || h <= 0.0 || l <= 0.0) return;
        ingest_(norm_ts_(ts_sec), h, l, c);
    }

    // warm-seed from an H1 CSV: ts,o,h,l,c (5-col) or ts,c (2-col; close-only -> h=l=c).
    size_t seed_from_h1_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) return 0;
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue;
            double ts = 0, o = 0, h = 0, l = 0, c = 0;
            const int got = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf", &ts, &o, &h, &l, &c);
            if (got == 5 && c > 0.0)      { ingest_(norm_ts_((int64_t)ts), h, l, c); ++n; }
            else if (got == 2 && o > 0.0) { ingest_(norm_ts_((int64_t)ts), o, o, o); ++n; }
        }
        return n;
    }

    // Reload persisted LIVE forward bars (ts,h,l,c written by on_h1_bar). Books nothing new.
    size_t seed_dump() noexcept {
        std::ifstream f(cfg_.bars_path);
        if (!f.is_open()) return 0;
        std::string line; size_t n = 0;
        while (std::getline(f, line)) {
            if (line.empty() || !(std::isdigit((unsigned char)line[0]) || line[0]=='-')) continue;
            double ts = 0, h = 0, l = 0, c = 0;
            if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf", &ts, &h, &l, &c) == 4 && c > 0.0) {
                ingest_(norm_ts_((int64_t)ts), h, l, c); ++n;
            }
        }
        return n;
    }

    // Call ONCE after all seeding: stamp+persist deploy_ts on first-ever boot; sort history.
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
        }
    }

    // S-2026-07-08d: live display mark (per-tick), so the desk shows the CURRENT
    // price between H1 closes instead of the last H1 close (which, right after a
    // mid-hour restart, is the warmup file's final bar on a different feed-scale
    // than the live quote -- the 29391-vs-29210 gap the operator saw). Display
    // only: clip/arm logic stays H1-close-cadence.
    void set_disp_mid(double mid) noexcept { if (mid > 0.0) disp_mid_ = mid; }

    // LIVE feed: one CLOSED H1 bar (from the tick_fx.hpp aggregator).
    void on_h1_bar(int64_t ts_sec, double h, double l, double c) noexcept {
        if (c <= 0.0 || h <= 0.0 || l <= 0.0 || h < l) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        ingest_(ts_sec, h, l, c);
        append_dump_(ts_sec, h, l, c);
        live_step_(ts_sec);
    }

    // Emit this pair's desk JSON object. REAL FORWARD CLIPS ONLY ($0 until first live clip).
    std::string pair_json() const {
        const double notl = cfg_.notional;
        // S-2026-07-08d: display against the LIVE mid when we have one (tracks between
        // H1 closes + heals the post-restart warmup-scale seam); fall back to last H1 close.
        const double cur = disp_mid_ > 0.0 ? disp_mid_ : (c_.empty() ? 0.0 : c_.back());
        std::ostringstream o; o << std::fixed;
        const int64_t lts = ts_.empty() ? 0 : ts_.back();

        double spct = 0.0; int sclips = 0, swins = 0;
        std::ostringstream runs;
        for (int ti = 0; ti < NT_; ++ti) {
            const FwdBook& b = fwd_[ti];
            spct += b.pct; sclips += b.clips; swins += b.wins;
            if (ti) runs << ",";
            runs.precision(0); runs << std::fixed;
            runs << "{\"tier\":\"" << TIER_TAG_[ti] << "\",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
            runs.precision(3); runs << "\"pct\":" << b.pct << ",";
            runs.precision(0); runs << "\"usd\":" << (b.pct / 100.0 * notl) << "}";
        }

        const char* dstr = cfg_.short_downjump ? "short" : "long";
        std::ostringstream op; int nopen = 0;
        for (const Leg& L : legs_) {
            if (L.pending) continue;   // BE-entry: not yet opened -> not shown as an open position
            const double u = (cur > 0 && L.entry > 0)
                             ? d_ * (cur / L.entry - 1.0) * 100.0 - cfg_.rt_cost_bp / 100.0 : 0.0;
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << cfg_.pair << "Lad\",\"dir\":\"" << dstr << "\",\"tier\":\"" << TIER_TAG_[L.ti] << "\",";
            op.precision(5); op << "\"entry\":" << L.entry << ",\"peak\":" << L.peak << ",\"cur\":" << cur << ",";
            op << "\"armed\":" << (L.armed ? "true" : "false") << ",";
            op.precision(3); op << "\"upnl_pct\":" << u << ",";
            op.precision(0); op << "\"upnl_usd\":" << (u / 100.0 * notl)
                                << ",\"entry_ts\":" << (long long)L.entry_ts << "}";
        }

        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c2 = *it;
            if (ntr++) tr << ",";
            tr.precision(0); tr << std::fixed;
            tr << "{\"flavor\":\"" << cfg_.pair << "Lad\",\"dir\":\"" << dstr << "\",\"tier\":\""
               << TIER_TAG_[(c2.ti >= 0 && c2.ti < NT_) ? c2.ti : 0] << "\",";
            tr.precision(5); tr << "\"entry\":" << c2.entry << ",\"exit\":" << c2.exit << ",";
            tr.precision(3); tr << "\"pct\":" << c2.pct << ",";
            tr.precision(0); tr << "\"usd\":" << c2.usd
               << ",\"reason\":\"" << c2.reason << "\",\"entry_ts\":" << (long long)c2.ets
               << ",\"exit_ts\":" << (long long)c2.xts << "}";
        }

        o << "{\"pair\":\"" << cfg_.pair << "\",\"live_sym\":\"" << cfg_.live_sym
          << "\",\"dir\":\"" << dstr << "\",\"retired\":" << (retired_ ? "true" : "false")
          << ",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_ << ",\"ts\":" << (long long)lts << ",";
        o << "\"W\":" << cfg_.W << ",";
        o.precision(2); o << "\"thr\":" << cfg_.thr << ",\"rt_cost_bp\":" << cfg_.rt_cost_bp << ",";
        o.precision(0); o << "\"notional\":" << notl << ",";
        o << "\"win\":{\"active\":" << (win_ ? "true" : "false")
          << ",\"age\":" << age_ << ",\"arms\":" << nclips_
          << ",\"nclips\":" << nclips_ << "},";   // S-2026-07-08d: 'arms'=batches (honest); 'nclips' kept for back-compat, GUI now reads clips from tiers/trades
        o << "\"clips\":" << sclips << ",\"wins\":" << swins << ",";
        o.precision(3); o << "\"pct\":" << spct << ",";
        o.precision(0); o << "\"usd\":" << (spct / 100.0 * notl) << ",";
        o << "\"open\":[" << op.str() << "],\"trades\":[" << tr.str() << "],";
        o << "\"tiers\":[" << runs.str() << "]}";
        return o.str();
    }

private:
    Config cfg_;
    double d_ = 1.0;                  // direction multiplier: +1 long upjump, -1 short downjump
    bool   retired_ = false;          // S-2026-07-08c auto-retirement latch state (new windows blocked)
    bool   retired_logged_ = false;   // one-shot retirement log
    std::vector<int64_t> ts_;
    std::vector<double>  h_, l_, c_;
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;

    static constexpr int NT_ = 4;   // 0 tight / 1 wide / 2 stacked / 3 ladder(reclip)
    static constexpr const char* TIER_TAG_[NT_] = { "tight", "wide", "stacked", "ladder" };

    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;

    // ── parent window + legs (harness run(), incremental on H1 bars) ──
    bool   win_        = false;   // window active
    int    age_        = 0;       // bars since trigger (flush when age_ >= W)
    int    nclips_     = 0;       // ARM-BATCHES spawned this window (base 1, cap cfg_.cap) -- NOT banked clips
    double disp_mid_   = 0.0;    // S-2026-07-08d live tick mid for display marking
    double last_reclip_px_ = 0.0; // reclip reference close

    struct Leg {
        int    ti = 0;              // tier class (0 tight / 1 wide / 2 stacked / 3 ladder)
        double entry = 0;           // entry close
        double peak  = 0;           // favorable extreme since entry (peak long / trough short; MFE reference)
        double arm_px = 0;          // arm level (px)
        double trail_abs = 0;       // abs trail distance in px (TIGHT; 0 = g50 trail)
        bool   armed = false;
        int64_t entry_ts = 0;
        std::string token;
        // BE-ENTRY (S-2026-07-09b): a leg stays PENDING at the trigger and opens only once price
        // clears +be_entry_pct (cost covered). arm_off/trail_mult are stored so arm_px + trail_abs
        // recompute off the DELAYED entry price. pending/trig/pbars drive the gate.
        bool   pending = false;
        double trig = 0;            // trigger close (BE measured off this)
        int    pbars = 0;           // bars spent pending
        double arm_off = 0;         // arm offset % (recompute arm_px = entry*(1+d*arm_off/100))
        double trail_mult = 0;      // trail_abs_mult (recompute trail_abs = entry*trail_mult*thr/100)
    };
    std::vector<Leg> legs_;

    struct FwdBook { double pct = 0.0; int clips = 0; int wins = 0; };   // net of rt_cost_bp
    FwdBook fwd_[NT_];

    struct Closed { int ti = 0; double entry = 0, exit = 0, pct = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;
    std::string leg_engine_(int ti) const {
        static const char* SUF[NT_] = { "Lad_T", "Lad_W", "Lad_S", "Lad_L" };
        return cfg_.pair + SUF[(ti >= 0 && ti < NT_) ? ti : 0];
    }

    // Book one clip at `fill` (stop level for trail/LC — resting-stop convention, in-calibration
    // with the research; observed close for window flush). pct is net of rt_cost_bp.
    void book_clip_(const Leg& L, double fill, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        const double pct = (L.entry > 0)
                           ? d_ * (fill / L.entry - 1.0) * 100.0 - cfg_.rt_cost_bp / 100.0 : 0.0;
        if (!fwd) return;
        if (!L.token.empty() && close_fn_) close_fn_(cfg_.live_sym, d_ > 0, cfg_.lot, fill, L.token);
        if (ledger_fn_) ledger_fn_(leg_engine_(L.ti), cfg_.live_sym, d_ > 0, L.entry, fill, cfg_.lot, L.entry_ts, ts_sec, reason);
        fwd_[L.ti].pct += pct; fwd_[L.ti].clips += 1; fwd_[L.ti].wins += (pct > 1e-9 ? 1 : 0);
        save_fwd_book_();
        Closed rec{L.ti, L.entry, fill, pct, pct / 100.0 * cfg_.notional, L.entry_ts, ts_sec, reason};
        closed_.push_back(rec);
        while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
        append_closed_(rec);
        std::printf("[FXLAD][CLIP] %s entry=%.5f fill=%.5f pct=%.4f (%s)\n",
                    leg_engine_(L.ti).c_str(), L.entry, fill, pct, reason);
        std::fflush(stdout);
    }

    Leg make_leg_(int ti, double arm_mult, double trail_abs_mult, double px, int64_t ts_sec, bool fwd,
                  double arm_abs = -1.0) noexcept {
        Leg L; L.ti = ti; L.armed = false; L.entry_ts = ts_sec;
        // arm_abs >= 0 -> engage at an ABSOLUTE MFE% (S-2026-07-09 WIDE peak-profit trail);
        // arm_abs < 0  -> legacy thr-scaled engage (arm_mult * thr).
        const double arm_off = (arm_abs >= 0.0) ? arm_abs : arm_mult * cfg_.thr;   // arm % offset from entry
        L.arm_off = arm_off; L.trail_mult = trail_abs_mult;
        if (cfg_.be_entry_pct > 0.0) {   // BE-ENTRY: stay PENDING at the trigger; open on BE (below)
            L.pending = true; L.trig = px; L.entry = 0.0; L.peak = 0.0; L.pbars = 0;
            return L;                    // no arm_px/trail/broker order yet -- set when BE is made
        }
        L.entry = px; L.peak = px;       // legacy: enter at the trigger close now
        L.arm_px = px * (1.0 + d_ * arm_off / 100.0);   // arm level in the favorable direction
        L.trail_abs = (trail_abs_mult > 0.0) ? px * (trail_abs_mult * cfg_.thr / 100.0) : 0.0;
        if (fwd && open_fn_) {
            const double tp_dist_pts = std::fabs(L.arm_px - px);
            if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) {
                L.token = open_fn_(cfg_.live_sym, d_ > 0, cfg_.lot, px);
                std::printf("[FXLAD][OPEN] %s %s @%.5f lot=%.2f tok=%s\n",
                            leg_engine_(ti).c_str(), d_ > 0 ? "LONG" : "SHORT",
                            px, cfg_.lot, L.token.c_str());
                std::fflush(stdout);
            }
        }
        return L;
    }

    // Incremental ladder state machine on the NEWEST H1 bar. Faithful harness bar order:
    // (1) manage open legs intrabar, ADVERSE extreme first (long: l->h->c, short: h->l->c);
    // (2) window-end flush at close; (3) detector on the W bars BEFORE this one;
    // (4) base-batch entry at the trigger close / reclip. Direction-parameterized via d_
    // (sign-mirror, exactly fx_bothways_sweep.py ladder() dir=+-1, S-2026-07-08c).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                               // backtest TU / not wired
        const int N = (int)c_.size(); const int W = cfg_.W;
        if (N < W + 1) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double hh = h_[N-1], ll = l_[N-1], cc = c_[N-1];
        // DRAWDOWN-CANCEL: ATR/thr-scaled LC stop (mimic never touches real trade -> free cut).
        //   Backtested: maxDD -581bp on the W96/0.5 equity curve (header FILL CONVENTION);
        //   H1 bars -> finer than daily, cut bites cleanly (no daily gap-through). Exit L484.
        const double lc_lvl_mult = 1.0 - d_ * LC_M * cfg_.thr / 100.0;

        // 1) manage open legs intrabar: adverse extreme first (SL-first), then favorable, then c.
        {
            const double seq[3] = { d_ > 0 ? ll : hh, d_ > 0 ? hh : ll, cc };
            std::vector<Leg> still; still.reserve(legs_.size());
            for (Leg& L : legs_) {
                // BE-ENTRY gate: a PENDING leg opens only once price clears +be_entry_pct off the
                // trigger (cost covered). A breakout that fades before covering cost never opens ->
                // no open-into-loss (the GER40 faded-breakout the operator flagged). Cancel if BE
                // is not made within pend_bars. arm_px + trail_abs recompute off the delayed entry.
                if (L.pending) {
                    L.pbars += 1;
                    const double fav = d_ > 0 ? hh : ll;
                    if (d_ * (fav / L.trig - 1.0) * 100.0 >= cfg_.be_entry_pct) {   // BE made -> ENTER
                        const double e = L.trig * (1.0 + d_ * cfg_.be_entry_pct / 100.0);
                        L.entry = e; L.peak = e; L.pending = false; L.entry_ts = ts_sec;
                        L.arm_px = e * (1.0 + d_ * L.arm_off / 100.0);
                        L.trail_abs = (L.trail_mult > 0.0) ? e * (L.trail_mult * cfg_.thr / 100.0) : 0.0;
                        if (fwd && open_fn_) {
                            const double tp_dist_pts = std::fabs(L.arm_px - e);
                            if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) {
                                L.token = open_fn_(cfg_.live_sym, d_ > 0, cfg_.lot, e);
                                std::printf("[FXLAD][BE-ENTER] %s %s @%.5f (trig %.5f)\n",
                                            leg_engine_(L.ti).c_str(), d_ > 0 ? "LONG" : "SHORT", e, L.trig);
                                std::fflush(stdout);
                            }
                        }
                        still.push_back(std::move(L)); continue;   // manage from the NEXT bar
                    }
                    if (L.pbars >= cfg_.pend_bars) continue;       // BE never made -> cancel, no book
                    still.push_back(std::move(L)); continue;       // still pending
                }
                bool closed = false;
                for (int k = 0; k < 3 && !closed; ++k) {
                    const double px = seq[k];
                    if (!L.armed) {
                        const double cut = L.entry * lc_lvl_mult;   // long: below entry; short: rally above
                        if (d_ * (px - cut) <= 0) { book_clip_(L, cut, ts_sec, fwd, "LOSS_CUT"); closed = true; break; }
                        if (d_ * (px - L.arm_px) >= 0) { L.armed = true; if (d_ * (px - L.peak) > 0) L.peak = px; }
                    } else {
                        if (d_ * (px - L.peak) > 0) L.peak = px;    // favorable extreme (peak/trough)
                        // RUNNER tiers WIDE(1)/STACKED(2)/LADDER(3) share the tightened peak-profit
                        // giveback (wide_gb_frac); TIGHT(0) uses its abs trail. gbf 0.5 = legacy g50.
                        const double gbf = ((L.ti == 1 || L.ti == 2 || L.ti == 3) && cfg_.wide_gb_frac > 0.0)
                                           ? cfg_.wide_gb_frac : 0.5;
                        const double stop = (L.trail_abs > 0.0)
                                            ? (L.peak - d_ * L.trail_abs)
                                            : (L.entry + (1.0 - gbf) * (L.peak - L.entry));   // peak-profit trail (sign-neutral)
                        if (d_ * (px - stop) <= 0) { book_clip_(L, stop, ts_sec, fwd, "TRAIL_STOP"); closed = true; break; }
                    }
                }
                if (!closed) still.push_back(std::move(L));
            }
            legs_.swap(still);
        }

        // 2) window end: flush remaining legs at this close.
        if (win_ && ++age_ >= W) {
            for (Leg& L : legs_) if (!L.pending) book_clip_(L, cc, ts_sec, fwd, "WINDOW_EXIT");  // pending never opened -> drop, no book
            legs_.clear(); win_ = false; age_ = 0; nclips_ = 0; last_reclip_px_ = 0.0;
        }

        // 3) detector — long: close >= thr% above the min LOW of the W bars BEFORE this one;
        //    short: close <= -thr% under the max HIGH of the W bars BEFORE this one.
        //    GAP GUARD (live-only deviation, documented): block NEW windows when the W-bar
        //    span exceeds W hours + 4 days (weekend/holiday slack) — a multi-day feed outage
        //    makes the "W-bar move" span weeks, outside calibration. Exits stay honoured.
        if (!win_ && !(cfg_.block_new_windows_fn && cfg_.block_new_windows_fn())) {
            double ref = (d_ > 0) ? l_[N-1-W] : h_[N-1-W];
            if (d_ > 0) { for (int i = N - W; i <= N - 2; ++i) if (l_[i] < ref) ref = l_[i]; }
            else        { for (int i = N - W; i <= N - 2; ++i) if (h_[i] > ref) ref = h_[i]; }
            const bool contig = (ts_[N-1] - ts_[N-1-W]) <= (int64_t)W * 3600 + 4 * 86400;
            const double jump = (ref > 0) ? d_ * (cc - ref) / ref * 100.0 : 0.0;
            if (ref > 0 && contig && jump >= cfg_.thr) {
                // S-2026-07-08c AUTO-RETIREMENT gate: a proven-negative forward book stops
                // arming NEW windows by itself (open legs above still managed/flushed).
                if (cfg_.retire_usd < 0.0 && book_usd() <= cfg_.retire_usd) {
                    retired_ = true;
                    if (!retired_logged_) {
                        retired_logged_ = true;
                        std::printf("[FXLAD][RETIRED] %s forward book $%.0f <= $%.0f -- no new windows (auto-retirement, S-2026-07-08c)\n",
                                    cfg_.pair.c_str(), book_usd(), cfg_.retire_usd);
                        std::fflush(stdout);
                    }
                } else {
                    retired_ = false;   // book recovered above the latch (legs can still flush green)
                    win_ = true; age_ = 0; nclips_ = 0; last_reclip_px_ = cc;
                }
            }
        }

        // 4) entries: base batch at the trigger close; reclip a WIDE leg on a further
        //    +1.67thr favorable extension (short: -1.67thr).
        if (win_ && nclips_ < cfg_.cap) {
            const double warm = cfg_.wide_arm_pct;   // >=0 absolute MFE% engage; <0 legacy 2.7*thr
            if (nclips_ == 0) {
                legs_.push_back(make_leg_(0, T_ARM_M, T_TRAIL_M, cc, ts_sec, fwd));         // TIGHT
                legs_.push_back(make_leg_(1, W_ARM_M, 0.0,       cc, ts_sec, fwd, warm));   // WIDE peak-profit
                for (int k = 0; k < 3; ++k)
                    legs_.push_back(make_leg_(2, S_ARM_M[k], 0.0, cc, ts_sec, fwd));        // STACKED (staggered arm, shared gb)
                nclips_ = 1; last_reclip_px_ = cc;
            } else if (d_ * (cc - last_reclip_px_ * (1.0 + d_ * RECLIP_M * cfg_.thr / 100.0)) >= 0) {
                legs_.push_back(make_leg_(3, W_ARM_M, 0.0, cc, ts_sec, fwd, warm));         // LADDER (wide params)
                nclips_ += 1; last_reclip_px_ = cc;
            }
        }
        save_live_state_();
    }

    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int ti = -1; double pct = 0; int clips = 0, wins = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (std::sscanf(line.c_str(), "%d %lf %d %d", &ti, &pct, &clips, &wins) == 4
                && ti >= 0 && ti < NT_) {
                fwd_[ti].pct = pct; fwd_[ti].clips = clips; fwd_[ti].wins = wins;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (int ti = 0; ti < NT_; ++ti)
              f << ti << " " << fwd_[ti].pct << " " << fwd_[ti].clips << " " << fwd_[ti].wins << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.ti << "," << r.entry << "," << r.exit << "," << r.pct << ","
          << r.usd << "," << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << "\n";
    }
    void load_closed_() noexcept {
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0};
            long long ets = 0, xts = 0;
            const int n = std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n]",
                                      &r.ti, &r.entry, &r.exit, &r.pct, &r.usd, &ets, &xts, reason);
            if (n >= 8 && r.ti >= 0 && r.ti < NT_) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
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
            if (kind == "win") {
                int a = 0; f >> a >> age_ >> nclips_ >> last_reclip_px_;
                win_ = (a != 0);
            } else if (kind == "leg") {
                Leg L; int armed = 0, pend = 0; long long ets = 0; std::string tok;
                // new format adds: pending trig pbars arm_off trail_mult (before token). Old-format
                // state (pre-BE-entry) fails this parse -> the leg is skipped (one-time seam drop).
                if (f >> L.ti >> L.entry >> L.peak >> L.arm_px >> L.trail_abs >> armed >> ets
                      >> pend >> L.trig >> L.pbars >> L.arm_off >> L.trail_mult >> tok) {
                    if (L.ti >= 0 && L.ti < NT_ && (L.entry > 0.0 || pend)) {
                        L.armed = (armed != 0); L.pending = (pend != 0); L.entry_ts = (int64_t)ets;
                        L.token = (tok == "-") ? std::string() : tok;
                        legs_.push_back(std::move(L));
                    }
                }
            } else { std::string rest; std::getline(f, rest); }
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << "win " << (win_ ? 1 : 0) << " " << age_ << " " << nclips_ << " " << last_reclip_px_ << "\n";
          for (const Leg& L : legs_)
              f << "leg " << L.ti << " " << L.entry << " " << L.peak << " " << L.arm_px << " "
                << L.trail_abs << " " << (L.armed ? 1 : 0) << " " << (long long)L.entry_ts << " "
                << (L.pending ? 1 : 0) << " " << L.trig << " " << L.pbars << " "
                << L.arm_off << " " << L.trail_mult << " "
                << (L.token.empty() ? "-" : L.token) << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.live_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.live_path.c_str());
    }

    void ingest_(int64_t ts, double h, double l, double c) noexcept {
        ts_.push_back(ts); h_.push_back(h); l_.push_back(l); c_.push_back(c);
    }
    void append_dump_(int64_t ts_sec, double h, double l, double c) const noexcept {
        std::ofstream f(cfg_.bars_path, std::ios::app);
        if (f.is_open()) f << (long long)ts_sec << "," << h << "," << l << "," << c << "\n";
    }
    static int64_t norm_ts_(int64_t ts) noexcept { return ts >= 100000000000LL ? ts / 1000 : ts; }
    static std::string lower_(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }

    void dedup_sort_() noexcept {
        const size_t n = ts_.size();
        if (n < 2) return;
        std::vector<size_t> idx(n);
        for (size_t i = 0; i < n; ++i) idx[i] = i;
        std::stable_sort(idx.begin(), idx.end(), [this](size_t a, size_t b){ return ts_[a] < ts_[b]; });
        std::vector<int64_t> nts; std::vector<double> nh, nl, nc;
        nts.reserve(n); nh.reserve(n); nl.reserve(n); nc.reserve(n);
        for (size_t k = 0; k < n; ++k) {
            const size_t i = idx[k];
            if (k + 1 < n && ts_[idx[k + 1]] == ts_[i]) continue;   // keep last of an equal-ts run
            nts.push_back(ts_[i]); nh.push_back(h_[i]); nl.push_back(l_[i]); nc.push_back(c_[i]);
        }
        ts_.swap(nts); h_.swap(nh); l_.swap(nl); c_.swap(nc);
    }
};

// ── registry: owns the pairs, writes the merged fxladder_companion_state.json
//   (served by /api/fxladder_companion). ──
class FxLadderBook {
public:
    void add(FxLadderPair::Config c) { pairs_.emplace_back(std::move(c)); }

    FxLadderPair* find(const std::string& pair) {
        for (auto& p : pairs_) if (p.pair() == pair) return &p;
        return nullptr;
    }

    size_t seed_pair(const std::string& pair, const std::string& csv) {
        if (auto* p = find(pair)) return p->seed_from_h1_csv(csv);
        return 0;
    }
    size_t seed_dumps_all() { size_t n = 0; for (auto& p : pairs_) n += p.seed_dump(); return n; }
    void set_exec(FxLadderPair::OpenFn o, FxLadderPair::CloseFn c,
                  FxLadderPair::GateFn g, FxLadderPair::LedgerFn l) {
        for (auto& p : pairs_) p.set_exec(o, c, g, l);
    }
    void finalize_all() { for (auto& p : pairs_) p.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed H1 bar for `pair` (no-op when the pair isn't wired).
    void on_h1_bar(const std::string& pair, int64_t ts_sec, double h, double l, double c) {
        if (auto* p = find(pair)) { p->on_h1_bar(ts_sec, h, l, c); recompute_and_write(); }
    }
    // S-2026-07-08d: per-tick live display mark (display only, no trading side effect).
    // S-2026-07-09b: recompute_and_write() otherwise only fires on_h1_bar (hourly), so the
    // served /api JSON froze between H1 bars -- the live disp_mid never reached the desk
    // (operator: "price in the index ladder is not updating in real time"). Throttle a
    // display refresh to ~1Hz here so the served leg cur/uPnL tracks the live mark without
    // hammering file I/O. Trading logic still H1-close-cadence; this only republishes.
    void set_disp_mid(const std::string& pair, double mid) {
        auto* p = find(pair);
        if (!p) return;
        p->set_disp_mid(mid);
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t last = last_disp_write_ms_.load(std::memory_order_relaxed);
        if (now_ms - last >= 1000 &&
            last_disp_write_ms_.compare_exchange_strong(last, now_ms, std::memory_order_relaxed)) {
            recompute_and_write();
        }
    }

    // Index book publishes under its own engine name + state file (same class/mechanism).
    void set_identity(std::string engine_name, std::string state_path) {
        engine_name_ = std::move(engine_name); state_path_ = std::move(state_path);
    }

    std::string state_json() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot_usd = 0.0;
        for (const auto& p : pairs_) { last_ts = std::max(last_ts, p.last_ts()); tot_usd += p.book_usd(); }
        o << "{\"engine\":\"" << engine_name_ << "\",\"shadow\":true,\"grade\":\"h1-intrabar\",";
        o.precision(0); o << std::fixed << "\"total_usd\":" << tot_usd << ",\"pairs\":[";
        for (size_t i = 0; i < pairs_.size(); ++i) { if (i) o << ","; o << pairs_[i].pair_json(); }
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
    std::vector<FxLadderPair> pairs_;
    std::string engine_name_ = "fx-upjump-ladder";
    std::string state_path_ = "fxladder_companion_state.json";   // cwd = C:\Omega
    mutable std::atomic<int64_t> last_disp_write_ms_{0};   // S-2026-07-09b 1Hz display-refresh throttle
};

// Singleton — accessor mirrors omega::stockmover_ladder_book().
inline FxLadderBook& fx_upjump_ladder_book() noexcept {
    static FxLadderBook inst;
    return inst;
}

// INDEX instance of the same validated ladder mechanism (US500/NAS100/GER40-bull-gated;
// research backtest/index_upjump_ladder_sweep.py, outputs/INDEX_UPJUMP_LADDER_2026-07-07.txt).
// Own persistence prefix + state file + /api/idxladder_companion.
inline FxLadderBook& index_upjump_ladder_book() noexcept {
    static FxLadderBook inst;
    static bool once = [] {
        inst.set_identity("index-upjump-ladder", "idxladder_companion_state.json");
        return true;
    }();
    (void)once;
    return inst;
}

} // namespace omega
