#pragma once
// =============================================================================
// StockDayMoverLadderCompanion — per-NAME BIGCAP daily-mover MIMIC LADDER
// (long-only) companion books. NO FLOOR anywhere — this is the successor the
// StockDayMoverBeFloorCompanion retirement left open (BE-floor real-fill
// -$110.7k; the no-floor giveback LADDER is the only trail family that
// survives real fills per the S-2026-07-07 late-arm sweep).
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math   : backtest/bigcap_mimic_ladder_bt.py (S-2026-07-07 operator item 4)
//            — parent = +3% day close-to-close -> enter NEXT close (real fill),
//            exit on -3% day (flush at the close AFTER the -3% day), end-flush MTM.
//            Legs: TIGHT a0.5/s2/g0 (stall-only banker) + WIDE a8/s0/g50
//            (giveback runner) + self-funding ladder cap 5 (a net-positive clip
//            spawns a new WIDE leg at the clip close), reclip 5%, RT 8bp/clip.
//   result : 39-name pooled book n=4,981 clips net +7,044% of clip notional
//            PF 1.58, H1 +2,813 / H2 +4,231, bear +3,958; 2x-cost (16bp) PASS;
//            neighbors plateau; ex-semis +3,875% PF 1.41; FULL 565-name universe
//            +59,789% PF 1.29 (not list survivorship). Evidence
//            outputs/BIGCAP_MIMIC_LADDER_2026-07-07.md · vault BigCapMimicLadder.
//
// S-2026-07-13 REDO (operator: "the bigcap engine should be a engine that trades
//   on trigger with 4x mimic engines" — no immediate-entry legs beyond the ONE
//   parent; feedback-no-immediate-entry-mimic-only). Structure now:
//     PARENT (the engine, trades on trigger): ONE immediate leg at the window
//       activation close — the WIDE-trail cell (w_arm/w_gb, tier LadW book).
//     4x MIMIC legs (companion-at-BE): T (t_arm/t_stall, LadT), MIRROR
//       (m_arm/m_gb, LadL), Wm (w_arm/w_gb, LadL), W8 (w8_arm/w8_gb, LadL) spawn
//       PENDING and open ONLY at the first close >= trigger*(1+be_entry_pct%)
//       (cost/BE covered); cancel after pend_closes closes if BE never made —
//       no open-into-loss. Ladder respawns are BE-gated the same way off the
//       clip close ("re-enter after BE").
//   VALIDATED backtest/bigcap_parent4mimic_bt.py (ALL 45 wired names, RT 8bp,
//   lc15): PARENT a1/gb10 n=3,348 net +3,010% PF 1.48 all-6+2x PASS; 4x-MIMIC
//   standalone at wired cell be0.5/pend3 n=7,020 net +9,603% PF 1.64 worst
//   -24.1% all-6+2x PASS, ex-best-name (WDC) still PASS (+8,144% PF 1.57);
//   every be/pend cell in the 12-cell sweep passes (plateau, not a point);
//   each of the 4 legs passes standalone. be_entry_pct=0 = legacy behavior.
//
// ADVERSE-PROTECTION: backtested verdict (mandate + feedback-engine-loss-
//   protection-provision) — giveback trail after arm + LOSS_CUT 15% on the leg.
//   LOSS_CUT 15 is FREE: net +7,057% (== baseline) with worst clip -32.6% -> -28.1%
//   (daily-close fills: a gap through -15% books the observed close, hence the
//   worst clip can exceed the cut). Un-cut/un-armed legs flush MTM at parent exit
//   (never abandoned). Same harness, same data, same costs as the PASS above.
//   S-2026-07-10: the live-confirmation gate (Config.live_confirm) is an ENTRY filter only
//   (session + fresh-tick + rising); it does NOT alter the in-flight adverse-protection verdict
//   above (LOSS_CUT 15 + giveback trail unchanged). It strictly reduces exposure (fewer, live-
//   confirmed opens), so the backtested protection verdict remains valid / conservative.
//
// Books in RETURN units (not price-points): equities have no fixed $/pt, and the
// research measures each clip as a return on per-clip notional. USD = return *
// fixed notional/clip (default $10k, shadow-illustrative; operator rescales).
//
// FEED: NO single-stock intraday exists in /Users/jo/Tick, so the trail is
// DAILY-CLOSE grade (in-calibration: the harness IS daily closes). Live daily
// closes arrive from the external RDAgent refresh (tools/rdagent/
// refresh_close_ibkr.py, IBKR port 4002) appending to
// data/rdagent/sp500_long_close.csv; a background poller re-reads the wide CSV
// and dispatches each NEW date (deploy-forward: seed primes the detector only).
//
// COST GATE: ExecutionCostGuard has NO single-name equity cost rows (unknown
// tickers default to CFD-scaled values that mis-price stocks — the befloor
// lesson). The validated gate is the harness's own 8bp RT debit: the PASS is
// net-of-8bp and survives 2x (16bp); single-name IBKR RT is ~3-8bp. rt_cost_bp
// is debited from every clip's ret_real. TODO before any LIVE sizing: real
// equity cost row.
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-
// independent-engine). Observe-only, SHADOW: never opens/moves/shrinks/closes a
// real position, never read by any parent. Judged STANDALONE.
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
#include <ctime>   // gmtime_r / std::tm — US-session gate (live-confirmation, S-2026-07-10)
#include "SeedGuard.hpp"   // omega::resolve_seed_path (VPS cwd-robust warm-seed)

namespace omega {

// S-2026-07-08c BOOT CATCH-UP order suppression: rows that arrived while the
// process was DOWN are replayed through the LIVE window logic at boot (so
// pending->active conversions and flushes scheduled for "the next close" are
// never silently eaten by a restart -- the bug that left AMD/TSLA/CRDO pendings
// latched over 07-07). During that replay broker orders + central-ledger writes
// are suppressed (fills at stale closes must not hit the live order path, and
// handle_closed_trade would PHANTOM-DROP pre-boot entries anyway); the book's
// own fwd_ totals + closed_ deque still record, so book accounting stays exact.
static std::atomic<bool> g_aulad_catchup{false};

// ── one stock name's long-only mimic-ladder book (returns-based) ────────────
class StockLadderSym {
public:
    struct Config {
        std::string sym      = "NVDA";    // ticker -> book name <sym>Lad + persist paths
        std::string live_sym = "NVDA";    // symbol the live legs trade (order path)
        double thr           = 0.03;      // parent day-mover threshold (+3% arm / -3% exit)
        // S-2026-07-08c AUTO-RETIREMENT (operator order): once this name's FORWARD
        // net-real book falls to <= retire_usd, NO new windows arm (existing legs
        // manage/flush normally, state stays visible with retired=1). Symmetric to
        // the n>=30 promote gate: a book that proves negative stops digging by
        // itself instead of waiting for a manual cull. Default -$600 = ~2x the
        // worst backtested clip (BIGCAP BT worst leg -28.1% of a $10k-notional
        // fractional leg, BIGCAP_MIMIC_LADDER_2026-07-07.md). 0 = disabled.
        // Un-retire = operator deletes the book file / raises the limit (loud act).
        double retire_usd    = -600.0;
        // S-2026-07-08c AGGRESSIVE RANKING (operator order; evidence
        // outputs/BIGCAP_AGGRESSIVE_RANKING_2026-07-08.md, validated cell per name
        // 2019-2026): notional is scaled at wiring (elite x2); ranked_out=true =
        // per-name book NET-NEGATIVE at the validated cell (TSLA PF0.56, COIN,
        // PLTR, MSTR, UBER, CRWV, SHOP, META, IONQ, QBTS) -> NO new windows arm
        // (existing legs manage/flush; detector history still maintained so a
        // future re-rank can re-enable with a warm detector).
        bool   ranked_out    = false;
        // TIGHT banker: arm 0.5% MFE, clip on 2-bar stall, no giveback leg
        double t_arm         = 0.5;       // % MFE to arm
        int    t_stall       = 2;         // daily bars without a new MFE high -> clip
        // WIDE runner: arm 8% MFE, clip on 50% giveback from peak
        double w_arm         = 8.0;       // % MFE to arm
        double w_gb          = 0.50;      // giveback fraction of MFE
        // S-2026-07-08c MIRROR tier (operator "obligatory mirrors"; standalone BT on the
        // active-book parents, 8bp: whole grid PF1.74-1.99 plateau; wired cell arm2/gb0.75
        // = n2034 +4660% bothH+ exbest+4425 2x-cost robust). Third BASE leg between
        // TIGHT and WIDE; cap is raised by 1 at wiring so ladder capacity is unchanged.
        bool   mirror_leg    = true;
        double m_arm         = 2.0;       // % MFE to arm the mirror
        double m_gb          = 0.75;      // giveback fraction of MFE
        // S-2026-07-13 PARENT + 4x BE-MIMIC redo (operator; validated
        // bigcap_parent4mimic_bt.py — see header). be_entry_pct > 0 switches the
        // book to: ONE immediate PARENT leg (w_arm/w_gb) + 4 PENDING mimic legs
        // (T, MIRROR, Wm, W8) that open only at the first close >= trigger*
        // (1+be%) and cancel after pend_closes closes; ladder respawns BE-gated
        // the same way. 0 = legacy all-immediate legs (backtest/parity default).
        double be_entry_pct  = 0.0;       // % above trigger a mimic leg must see on a CLOSE to open
        int    pend_closes   = 3;         // cancel a pending mimic leg after this many closes without BE
        // S-2026-07-16k MIMIC-ONLY (operator: "remove the mimic engines in bigcap and replace
        // with 2x mimic engines"; feedback-no-immediate-entry-mimic-only). When true (and
        // be_entry_pct > 0) the window opens NO immediate PARENT leg at all — ONLY the 4 PENDING
        // BE-mimic legs (T/MIRROR/Wm/W8), so the book NEVER trades into a loss on the jump close
        // (fresh-entry forbidden). The mimic legs already carry the full protection stack: BE-entry
        // gate (never open underwater), pre-arm LOSS_CUT/DRAWDOWN-CANCEL (loss_cut_pct), post-arm
        // giveback trail that floors ABOVE BE. VALIDATED STANDALONE bigcap_parent4mimic_bt.py
        // 4x-MIMIC be0.5/pend3: +9,603% PF1.64 worst -24.1% all-6 + 2x + ex-best PASS (the parent
        // was purely additive; removing it leaves this validated mimic book untouched).
        bool   mimic_only    = false;     // true = NO immediate parent leg, mimic legs only
        // S-2026-07-13 RIDE-HARDER (operator: "trade it hard and for as long as we can until
        // it reverses"): optional PARENT-ONLY trail override. p_arm > 0 replaces the parent
        // leg's w_arm/w_gb cell (mimic Wm + ladder respawns UNAFFECTED — they keep w_arm/w_gb).
        // p_arm = 1e9 = never arms -> NO trail: parent rides to the -thr% reversal flush;
        // loss_cut_pct (pre-arm) remains the catastrophe floor. VALIDATED
        // backtest/bigcap_ride_harder_bt.py A1 (45 names, RT 8bp): wired gb10 +3010%/PF1.48
        // -> gb25 +3703%/1.62 -> gb50 +4251%/1.74 -> gb75 +4610%/1.81 -> RIDE-TO-REVERSAL
        // +4658%/1.73 — monotone plateau, worst clip -28.1% UNCHANGED at every cell,
        // ex-best(WDC) +4199%/1.69 PASS. 0 = legacy parent (w_arm/w_gb).
        double p_arm         = 0.0;       // >0: parent-leg arm override (1e9 = ride to reversal)
        double p_gb          = 0.0;       // parent-leg giveback override (used when p_arm > 0)
        double w8_arm        = 8.0;       // 4th mimic cell: the old wide-runner (validated standalone)
        double w8_gb         = 0.50;
        double reclip        = 0.05;      // re-enter when fav > peak*(1+reclip)
        int    cap           = 5;         // max legs spawned per parent window (incl 2 base)
        double loss_cut_pct  = 15.0;      // ADVERSE-PROTECTION / DRAWDOWN-CANCEL: cut an open leg at -15% fav.
                                          // Mimic never touches the real trade -> cold cut is FREE. Sweep
                                          // bigcap_mimic_lc_sweep.py (S-2026-07-11): near-inert on daily bars
                                          // (losses gap-through, worst -33->-28%) but net flat/up + all-6 & 2x
                                          // pass at lc 8-20; lc=15 = free Pareto floor. Fires L642 (L.dead).
        double rt_cost_bp    = 8.0;       // REAL round-trip cost (bp of entry) debited per clip (validated gate)
        double notional      = 10000.0;   // $ per clip; USD = return * notional (name-agnostic sizing)
        double lot           = 1.0;       // order-path lot (shares/CFD units decided at flip)
        // S-2026-07-10 LIVE-CONFIRMATION GATE (operator: stop opening "fake" paper legs on
        // stale daily-close signals — 18 blind legs opened on names not actually trading).
        // When true, a pending +thr window does NOT open on the blind daily close; it opens
        // ONLY when a live L1 tick (fed via on_live_tick from the IBKR bridge) confirms ALL of:
        //   (1) US session open  — 13:30-20:00 UTC weekday (in_us_session_)
        //   (2) live tick fresh  — quote age < live_stale_ms (actively trading)
        //   (3) price rising     — live px > the daily-close entry reference (pend_ref_px_)
        // Any fail -> the window STAYS PENDING (re-checked each tick + each daily poll). Set true
        // ONLY in the live TU (engine_init). DEFAULT false keeps the backtest/parity path
        // (blind daily-close entry) byte-for-byte unchanged — the registry-mandated parity test
        // drives daily bars with no live ticks, so it MUST keep the daily-close conversion.
        bool    live_confirm  = false;
        int64_t live_stale_ms = 60000;    // a live quote older than this = not "actively trading"
        std::string deploy_path;          // per-name persisted deploy-forward anchor
        std::string bars_path;            // per-name persisted LIVE forward daily bars (survives restart)
        std::string book_path;            // per-name persisted REAL forward book (3 tier classes)
        std::string live_path;            // per-name persisted OPEN window+legs state (survives restart)
        std::string closed_path;          // per-name persisted CLOSED forward clips log
    };

    // ── LIVE EXECUTION WIRING — identical contract to the BeFloor companions. Set ONLY in the
    //   live main TU; null in a backtest TU -> live_step_ short-circuits (pure accounting).
    //   SHADOW today (send_live_order no-ops while mode!=LIVE), LIVE on flip. ──
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }
    // S-2026-07-10: turn the live-confirmation entry gate ON (live TU only; default OFF).
    void set_live_confirm(bool b) noexcept { cfg_.live_confirm = b; }

    explicit StockLadderSym(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "stockladder_companion_" + s + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "stockladder_companion_" + s + "_daily.csv";
        if (cfg_.book_path.empty())   cfg_.book_path   = "stockladder_companion_" + s + "_book.txt";
        if (cfg_.live_path.empty())   cfg_.live_path   = "stockladder_companion_" + s + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "stockladder_companion_" + s + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_fwd_book_();
        load_closed_();
        load_live_state_();
    }

    const std::string& sym() const { return cfg_.sym; }
    size_t bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_ret() const {        // GROSS total forward return (no cost) — display context only
        double r = 0.0; for (int ti = 0; ti < NT_; ++ti) r += fwd_[ti].ret; return r;
    }
    double  book_usd() const { return book_ret() * cfg_.notional; }
    double  book_ret_real() const {   // REAL total (close fills - rt cost) — the judgeable headline
        double r = 0.0; for (int ti = 0; ti < NT_; ++ti) r += fwd_[ti].ret_real; return r;
    }
    double  book_usd_real() const { return book_ret_real() * cfg_.notional; }

    // seed one historical daily close (primes detector history; books nothing — deploy-forward gate).
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
            // S-2026-07-09 CARRY-FORWARD PENDING ARM. Deploy-forward suppresses booking on
            // every historical seed row, but the validated harness rule is "+thr day close
            // -> enter the NEXT close". If the LAST seed day is itself a +thr close-to-close
            // move, the faithful next action is a live entry at the next close -- NOT silence
            // until a fresh +thr prints. Without this, a first deploy landing AFTER a +thr
            // close (NVDA/AVGO +3% on the 07-08 close, ladder first shipped that evening)
            // absorbs the mover into history and stays blind to it across every restart
            // (the S-2026-07-08c catch-up watermark only replays rows that arrived while
            // DOWN, not the last seed row of the first-ever deploy). Books NOTHING now:
            // deploy_ts is already stamped at this same last bar, so when the next LIVE close
            // converts win_pend_ the entry ts > deploy_ts => fwd=true real fill. Restart-safe:
            // this branch runs ONLY on the first-ever boot (deploy_loaded_ was false); on a
            // restart the pending window is restored from live_path instead, never re-armed.
            const int n = (int)c_.size();
            if (n >= 2 && !cfg_.ranked_out) {
                const double j      = c_[n - 1] / c_[n - 2] - 1.0;
                const bool   contig = (ts_[n - 1] - ts_[n - 2]) <= (int64_t)86400 * 7;   // no arm across a data-gap
                double net_real_usd = 0.0;
                for (int ti = 0; ti < NT_; ++ti) net_real_usd += fwd_[ti].ret_real * cfg_.notional;
                const bool retired = (cfg_.retire_usd < 0.0 && net_real_usd <= cfg_.retire_usd);
                if (contig && !retired && j >= cfg_.thr) {
                    win_pend_ = true;            // base legs enter at the NEXT live close (or live-confirm)
                    pend_ref_px_ = c_[n - 1]; pend_ref_ts_ = ts_[n - 1];   // rising-test reference
                    save_live_state_();
                    std::printf("[AULAD][SEED-ARM] %s last seed day %+.2f%% (>= thr %.2f%%) -> pending arm carried; next live close enters\n",
                                cfg_.sym.c_str(), j * 100.0, cfg_.thr * 100.0);
                    std::fflush(stdout);
                }
            }
        }
    }

    // LIVE feed: one CLOSED daily bar for this name (close = official daily close).
    void on_daily_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only (idempotent re-poll)
        // DATA-INTEGRITY GUARD (live path only): a torn read of the externally-written wide CSV
        // parses as a valid >0 close and would arm a phantom window. Reject any single-day move
        // > 50% (CLAUDE.md x1000 class). SELF-HEALING (parity test found the befloor-era guard
        // sticky: CRWD's 492->124 split/vendor seam bricked the name FOREVER — every later bar
        // rejected vs the stale prev): a torn read is transient, but 3 CONSECUTIVE daily bars at
        // the new level = a real level shift (split / adjusted-history seam). Accept the level,
        // VOID any open window WITHOUT booking (clip math across the seam is fake ±%), resume.
        if (!c_.empty()) {
            const double jump = std::fabs(close / c_.back() - 1.0);
            if (jump > 0.50) {
                if (++rej_streak_ >= 3) {
                    std::printf("[AULAD][RESYNC] %s accepts level %.4f after %d consecutive >50%% rejects vs %.4f — split/seam; window voided unbooked\n",
                                cfg_.sym.c_str(), close, rej_streak_, c_.back());
                    std::fflush(stdout);
                    rej_streak_ = 0;
                    legs_.clear(); win_ = false; win_pend_ = false; exit_pend_ = false;
                    spawned_ = 0; bar_ = 0; win_entry_ = 0;   // no books: seam PnL is not real
                    ingest_(ts_sec, close);
                    append_dump_(ts_sec, close);
                    save_live_state_();
                    return;   // no live_step_: the seam bar's day-move vs stale prev is fake
                }
                std::printf("[AULAD][REJECT] %s daily close %.4f vs prev %.4f (%.0f%% jump) — torn read/split? bar dropped (streak %d/3)\n",
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

    // LIVE L1 tick for this name (from the IBKR bridge, via the book's on_live_tick).
    // Stores the latest quote; when the live-confirmation gate is armed and a +thr window is
    // PENDING, opens the base legs the instant the gate passes (session+fresh+rising). Returns
    // true iff a window was opened this tick (book state changed -> caller re-publishes). Cheap
    // no-op whenever the gate is off / not live-wired / nothing pending. NOT a backtest path.
    bool on_live_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (!(bid > 0.0 && ask > 0.0 && ask >= bid)) return false;
        lq_.bid = bid; lq_.ask = ask; lq_.px = 0.5 * (bid + ask); lq_.ts_ms = now_ms;
        if (!cfg_.live_confirm || !open_fn_) return false;   // gate off / backtest TU -> just store
        if (!win_pend_ || cfg_.ranked_out) return false;     // nothing to confirm
        // retirement guard — mirror the daily-close arm gate so a proven-negative book never
        // opens even a live-confirmed window.
        if (cfg_.retire_usd < 0.0) {
            double net = 0.0; for (int ti = 0; ti < NT_; ++ti) net += fwd_[ti].ret_real * cfg_.notional;
            if (net <= cfg_.retire_usd) return false;
        }
        if (!live_confirm_ok_(now_ms)) return false;         // session + fresh + rising (all 3)
        const int64_t ts_sec = now_ms / 1000;
        const bool    fwd     = ts_sec > deploy_ts_;
        const double  ref     = pend_ref_px_;
        activate_window_(lq_.px, ts_sec, fwd);               // OPEN base legs at the live rising price
        pend_ref_px_ = 0.0; pend_ref_ts_ = 0;
        std::printf("[AULAD][LIVE-CONFIRM] %s pending +%.1f%% window CONFIRMED -> base legs OPEN @live %.2f (ref close %.2f; session+fresh+rising)\n",
                    cfg_.sym.c_str(), cfg_.thr * 100.0, lq_.px, ref);
        std::fflush(stdout);
        save_live_state_();
        return true;
    }

    // STEP 3 (S-2026-07-10): clear the currently-open UNCONFIRMED legs/window — the pre-gate
    // blind daily-close opens that never met the live gate. No clip is booked (so nothing hits
    // the closed ledger); any live token is flattened (SHADOW no-op today). Detector history +
    // the closed forward book stay intact (deploy-forward safe: a real signal re-arms + must now
    // pass the live gate). Returns the count of open legs cleared.
    int flush_unconfirmed(const char* reason) noexcept {
        int cleared = 0;
        for (Leg& L : legs_) {
            if (L.open && !L.dead) ++cleared;
            if (!L.token.empty() && close_fn_)
                close_fn_(cfg_.live_sym, true, cfg_.lot, (c_.empty() ? L.le : c_.back()), L.token);
            L.token.clear();
        }
        const bool had_state = cleared || win_ || win_pend_ || exit_pend_ || !legs_.empty();
        legs_.clear();
        win_ = false; win_pend_ = false; exit_pend_ = false;
        spawned_ = 0; bar_ = 0; win_entry_ = 0.0; pend_ref_px_ = 0.0; pend_ref_ts_ = 0;
        if (had_state) {
            save_live_state_();
            std::printf("[AULAD][FLUSH] %s cleared %d unconfirmed open leg(s) + window (%s)\n",
                        cfg_.sym.c_str(), cleared, reason);
            std::fflush(stdout);
        }
        return cleared;
    }

    // ── MANUAL KILL-ALL (S-2026-07-20): desk panic flatten. This book has NO
    //    register_source, so the on_tick registry flatten can never see its legs —
    //    this is its own closer. OPEN legs are force-closed through book_clip_
    //    (fires the real broker close via close_fn_ where a live token exists +
    //    books honestly at the best available mark: live L1 mid, else last daily
    //    close, else leg entry). Stray tokens on non-open legs are flattened
    //    unbooked (flush_unconfirmed convention). PENDING legs disarmed, window
    //    state cleared so nothing respawns. Returns legs cleared.
    int kill_all(int64_t now_sec) noexcept {
        const double mark = lq_.px > 0.0 ? lq_.px : (c_.empty() ? 0.0 : c_.back());
        int n = 0;
        for (Leg& L : legs_) {
            if (L.open && !L.clipped && !L.dead) {
                ++n;
                book_clip_(L, mark > 0.0 ? mark : L.le, now_sec, /*fwd=*/true, "MANUAL_KILL_ALL");
            } else if (!L.token.empty() && close_fn_) {
                close_fn_(cfg_.live_sym, true, cfg_.lot, mark > 0.0 ? mark : L.le, L.token);
                L.token.clear();
            }
            if (L.pending) ++n;                            // no position yet: disarm only
        }
        const bool had_state = n || win_ || win_pend_ || exit_pend_ || !legs_.empty();
        legs_.clear();
        win_ = false; win_pend_ = false; exit_pend_ = false;
        spawned_ = 0; bar_ = 0; win_entry_ = 0.0; pend_ref_px_ = 0.0; pend_ref_ts_ = 0;
        if (had_state) save_live_state_();
        return n;
    }

    // S-2026-07-10 IN-BINARY DAILY-CLOSE WRITER support: latest live L1 mid + its ms-timestamp for
    // this name (from on_live_tick / the IBKR bridge). Returns false when the name has never ticked
    // (ts<=0) or has no valid px. The writer additionally checks the ts falls on the current UTC day
    // so a stale (bridge-down) quote is never snapshotted as "today's close".
    bool last_live_px(double& px, int64_t& ts_ms) const noexcept {
        if (lq_.ts_ms <= 0 || lq_.px <= 0.0) return false;
        px = lq_.px; ts_ms = lq_.ts_ms; return true;
    }

    // Reload persisted LIVE forward daily bars (written by on_daily_bar). Books nothing new
    // (deploy-forward gate); replay keeps the price history contiguous across restart.
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

        // tier books
        double sret = 0.0, sret_real = 0.0; int sclips = 0, swins = 0;
        std::ostringstream runs;
        for (int ti = 0; ti < NT_; ++ti) {
            const FwdBook& b = fwd_[ti];
            sret += b.ret; sret_real += b.ret_real; sclips += b.clips; swins += b.wins;
            if (ti) runs << ",";
            runs.precision(0); runs << std::fixed;
            runs << "{\"tier\":\"" << TIER_TAG_[ti] << "\",\"clips\":" << b.clips << ",\"wins\":" << b.wins << ",";
            runs.precision(3); runs << "\"pct\":" << (b.ret * 100.0)
                                    << ",\"pct_real\":" << (b.ret_real * 100.0) << ",";
            runs.precision(0); runs << "\"usd\":" << (b.ret * notl)
                                    << ",\"usd_real\":" << (b.ret_real * notl) << "}";
        }

        // OPEN legs right now (empty = idle / no window).
        std::ostringstream op; int nopen = 0;
        for (const Leg& L : legs_) {
            if (!L.open || L.clipped || L.dead) continue;
            const double u  = (cur > 0 && L.le > 0) ? (cur / L.le - 1.0) : 0.0;
            const double ur = u - cfg_.rt_cost_bp / 1e4;
            if (nopen++) op << ",";
            op.precision(0); op << std::fixed;
            op << "{\"flavor\":\"" << cfg_.sym << "Lad\",\"dir\":\"long\",\"tier\":\"" << TIER_TAG_[L.ti] << "\",";
            op.precision(2);
            op << "\"entry\":" << L.le << ",\"wm\":" << (L.epx * (1.0 + L.mfe / 100.0)) << ",\"cur\":" << cur << ",";
            op.precision(3); op << "\"upnl_pct\":" << (u * 100.0)
                                << ",\"upnl_pct_real\":" << (ur * 100.0) << ",";
            op.precision(0); op << "\"upnl_usd\":" << (u * notl)
                                << ",\"upnl_usd_real\":" << (ur * notl)
                                << ",\"entry_ts\":" << (long long)L.entry_ts << "}";
        }

        // CLOSED forward clips log — most-recent first.
        std::ostringstream tr; int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& c = *it;
            if (ntr++) tr << ",";
            tr.precision(0); tr << std::fixed;
            tr << "{\"flavor\":\"" << cfg_.sym << "Lad\",\"dir\":\"long\",\"tier\":\""
               << TIER_TAG_[(c.ti >= 0 && c.ti < NT_) ? c.ti : 0] << "\",";
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
        o << "\"win\":{\"active\":" << (win_ ? "true" : "false")
          << ",\"pending\":" << (win_pend_ ? "true" : "false")
          << ",\"exit_pending\":" << (exit_pend_ ? "true" : "false")
          << ",\"spawned\":" << spawned_ << ",\"bar\":" << bar_ << "},";
        o << "\"clips\":" << sclips << ",\"wins\":" << swins << ",";
        o.precision(3); o << "\"pct\":" << (sret * 100.0)
                          << ",\"pct_real\":" << (sret_real * 100.0) << ",";
        o.precision(0); o << "\"usd\":" << (sret * notl)
                          << ",\"usd_real\":" << (sret_real * notl) << ",";
        o << "\"open\":[" << op.str() << "],\"trades\":[" << tr.str() << "],";
        o << "\"tiers\":[" << runs.str() << "]}";
        return o.str();
    }

private:
    Config cfg_;
    std::vector<int64_t> ts_;
    std::vector<double>  c_;
    int64_t deploy_ts_ = 0;
    bool    deploy_loaded_ = false;
    int     rej_streak_ = 0;   // consecutive >50%-jump rejects (3 = real level shift -> resync)
    bool    retired_logged_ = false;   // S-2026-07-08c: one-shot auto-retirement log

    static constexpr int NT_ = 3;   // 0 tight banker / 1 wide runner / 2 ladder spawn (wide params)
    static constexpr const char* TIER_TAG_[NT_] = { "tight", "wide", "ladder" };

    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;

    // ── parent window + legs (harness Leg/run_trade, incremental on daily closes) ──
    bool win_       = false;   // window active (legs live)
    bool win_pend_  = false;   // +thr day seen; legs enter at the NEXT close
    bool exit_pend_ = false;   // -thr day seen; flush at the NEXT close
    int  bar_       = 0;       // daily bars since window activation (activation close = 0)
    int  spawned_   = 0;       // legs created this window (base 2, cap cfg_.cap)
    double win_entry_ = 0.0;   // activation close (base legs' epx)
    int64_t win_ets_  = 0;
    // S-2026-07-10 LIVE-CONFIRMATION GATE state
    double  pend_ref_px_ = 0.0;   // daily-close entry reference for the "rising" test (arming close)
    int64_t pend_ref_ts_ = 0;     // ts (sec) of the arming close -> pending-expiry clock
    struct LiveQ { double bid = 0, ask = 0, px = 0; int64_t ts_ms = 0; } lq_;   // latest live L1 quote

    struct Leg {
        int    ti = 0;            // tier class (0 tight / 1 wide / 2 ladder)
        double epx = 0;           // leg anchor entry (fav/mfe reference — never changes)
        double le  = 0;           // last entry px (changes on reclip; clip PnL measured from here)
        double arm = 0, gb = 0;   // arm % MFE, giveback fraction (0 = stall-only)
        int    sb  = 0;           // stall bars (0 = giveback-only)
        bool   open = false, clipped = false, dead = false;
        double pk  = 0;           // MFE % at last clip (reclip reference)
        double mfe = 0;           // best fav % since leg entry (from epx)
        int    ext = 0;           // bar index of last MFE extension
        int64_t entry_ts = 0;
        // S-2026-07-13 BE-ENTRY mimic state: a pending leg has NO position (open=false);
        // it opens at the first close >= trig*(1+be%) or dies after pend_closes closes.
        bool   pending = false;
        double trig = 0;          // window trigger px the BE gate measures from
        int    pbars = 0;         // pending closes remaining before cancel
        std::string token;
    };
    std::vector<Leg> legs_;

    struct FwdBook { double ret = 0.0; int clips = 0; int wins = 0; double ret_real = 0.0; };
    FwdBook fwd_[NT_];   // .ret = gross clip return (no cost); .ret_real = net of rt_cost_bp

    struct Closed { int ti = 0; double entry = 0, exit = 0, ret = 0, usd = 0;
                    int64_t ets = 0, xts = 0; std::string reason;
                    double ret_real = 0, usd_real = 0; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;
    std::string leg_engine_(int ti) const {
        static const char* SUF[NT_] = { "LadT", "LadW", "LadL" };
        return cfg_.sym + SUF[(ti >= 0 && ti < NT_) ? ti : 0];
    }

    // Book one clip: fill AT the observed daily close (the only tradable mark — no floor,
    // no wish). gross = close/le - 1; real = gross - rt_cost_bp. Returns net bp (real).
    double book_clip_(Leg& L, double fill, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        const double r      = (L.le > 0) ? (fill / L.le - 1.0) : 0.0;
        const double r_real = r - cfg_.rt_cost_bp / 1e4;
        if (fwd) {
            if (!L.token.empty() && close_fn_) close_fn_(cfg_.live_sym, true, cfg_.lot, fill, L.token);
            if (ledger_fn_ && !g_aulad_catchup.load(std::memory_order_relaxed))
                ledger_fn_(leg_engine_(L.ti), cfg_.live_sym, true, L.le, fill, cfg_.lot, L.entry_ts, ts_sec, reason);
            fwd_[L.ti].ret += r; fwd_[L.ti].ret_real += r_real;
            fwd_[L.ti].clips += 1; fwd_[L.ti].wins += (r_real > 1e-9 ? 1 : 0);
            save_fwd_book_();
            Closed rec{L.ti, L.le, fill, r, r * cfg_.notional, L.entry_ts, ts_sec, reason,
                       r_real, r_real * cfg_.notional};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[AULAD][CLIP] %s entry=%.2f fill=%.2f ret=%.4f ret_real=%.4f (%s)\n",
                        leg_engine_(L.ti).c_str(), L.le, fill, r, r_real, reason);
            std::fflush(stdout);
        }
        L.token.clear();
        return r_real * 1e4;
    }

    Leg make_leg_(int ti, double px, int64_t ts_sec, bool fwd) noexcept {
        Leg L; L.ti = ti; L.epx = px; L.le = px;
        if (ti == 0) { L.arm = cfg_.t_arm; L.sb = cfg_.t_stall; L.gb = 0.0; }
        else         { L.arm = cfg_.w_arm; L.sb = 0;            L.gb = cfg_.w_gb; }
        L.open = true; L.mfe = 0.0; L.ext = bar_; L.entry_ts = ts_sec;
        if (fwd && open_fn_ && !g_aulad_catchup.load(std::memory_order_relaxed)) {
            const double tp_dist_pts = px * (L.arm / 100.0);
            if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) {
                L.token = open_fn_(cfg_.live_sym, true, cfg_.lot, px);
                std::printf("[AULAD][OPEN] %s LONG @%.2f lot=%.2f tok=%s\n",
                            leg_engine_(ti).c_str(), px, cfg_.lot, L.token.c_str());
                std::fflush(stdout);
            }
        }
        return L;
    }

    // S-2026-07-13: a PENDING BE-mimic leg — no order, no book, until the BE gate opens it.
    Leg make_pending_leg_(int ti, double arm, int sb, double gb, double trig_px, int64_t ts_sec) noexcept {
        Leg L; L.ti = ti; L.epx = trig_px; L.le = trig_px;
        L.arm = arm; L.sb = sb; L.gb = gb;
        L.open = false; L.pending = true; L.trig = trig_px; L.pbars = cfg_.pend_closes;
        L.mfe = 0.0; L.ext = bar_; L.entry_ts = ts_sec;
        return L;
    }

    // Open the legs of a fresh window at `entry_px` (factored out so BOTH the daily-close
    // path (backtest/parity) and the live-confirmation path (on_live_tick) build an identical
    // window).
    //   be_entry_pct > 0 (S-2026-07-13 redo): ONE immediate PARENT leg (the engine, trades on
    //     trigger — WIDE-trail cell, LadW book) + 4 PENDING BE-mimic legs (T/MIRROR/Wm/W8).
    //   be_entry_pct == 0 (legacy/parity): TIGHT + WIDE + optional MIRROR, all immediate.
    void activate_window_(double entry_px, int64_t ts_sec, bool fwd) noexcept {
        win_pend_ = false; win_ = true; bar_ = 0;
        win_entry_ = entry_px; win_ets_ = ts_sec;
        legs_.clear();
        if (cfg_.be_entry_pct > 0.0) {
            // S-2026-07-16k MIMIC-ONLY: skip the immediate PARENT leg entirely — only the 4 PENDING
            // BE-mimic legs (never open into a loss on the jump close). mimic_only=false keeps the
            // legacy PARENT + 4 mimic structure (backtest parity / prior behaviour).
            if (!cfg_.mimic_only) {
                spawned_ = 5;
                {   // PARENT (immediate, LadW). p_arm>0 = ride-harder trail override (parent ONLY;
                    // 1e9 never arms -> rides to the -thr flush, loss_cut_pct still guards pre-arm).
                    Leg P = make_leg_(1, entry_px, ts_sec, fwd);
                    if (cfg_.p_arm > 0.0) { P.arm = cfg_.p_arm; P.gb = cfg_.p_gb; }
                    legs_.push_back(std::move(P));
                }
            } else {
                spawned_ = 4;   // no parent leg; 4 mimic legs only
            }
            legs_.push_back(make_pending_leg_(0, cfg_.t_arm,  cfg_.t_stall, 0.0,       entry_px, ts_sec)); // T
            legs_.push_back(make_pending_leg_(2, cfg_.m_arm,  0,            cfg_.m_gb, entry_px, ts_sec)); // MIRROR
            legs_.push_back(make_pending_leg_(2, cfg_.w_arm,  0,            cfg_.w_gb, entry_px, ts_sec)); // Wm
            legs_.push_back(make_pending_leg_(2, cfg_.w8_arm, 0,            cfg_.w8_gb, entry_px, ts_sec)); // W8
            std::printf("[AULAD][MIMIC-PEND] %s %s + 4 mimic legs PENDING (be %.2f%%, %d closes)\n",
                        cfg_.sym.c_str(), cfg_.mimic_only ? "MIMIC-ONLY (no parent)" : "parent OPEN",
                        cfg_.be_entry_pct, cfg_.pend_closes);
            std::fflush(stdout);
            return;
        }
        spawned_ = 2;
        legs_.push_back(make_leg_(0, entry_px, ts_sec, fwd));   // TIGHT banker
        legs_.push_back(make_leg_(1, entry_px, ts_sec, fwd));   // WIDE runner
        if (cfg_.mirror_leg) {                                  // MIRROR tier -> ladder bucket
            Leg M = make_leg_(2, entry_px, ts_sec, fwd);
            M.arm = cfg_.m_arm; M.gb = cfg_.m_gb; M.sb = 0;
            legs_.push_back(std::move(M)); spawned_ += 1;
        }
    }

    // US cash session gate: 13:30-20:00 UTC on a weekday (operator spec; = 09:30-16:00 ET during
    // US daylight time — the current DST offset. Standard time shifts this to 14:30-21:00 UTC; the
    // operator fixed the window at 13:30-20:00, accepted for the DST-dominant trading calendar).
    static bool in_us_session_(int64_t now_ms) noexcept {
        const std::time_t t = (std::time_t)(now_ms / 1000);
        std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        if (g.tm_wday == 0 || g.tm_wday == 6) return false;        // Sun / Sat
        const int mod = g.tm_hour * 60 + g.tm_min;                 // minute-of-day UTC
        return mod >= (13 * 60 + 30) && mod < (20 * 60);           // 13:30-20:00 UTC
    }

    // The three-part live-confirmation predicate (STEP 2): session open + fresh tick + rising.
    bool live_confirm_ok_(int64_t now_ms) const noexcept {
        if (!in_us_session_(now_ms))                 return false; // (1) actively-trading session
        if (lq_.ts_ms <= 0)                          return false; // no live quote ever
        if (now_ms - lq_.ts_ms > cfg_.live_stale_ms) return false; // (2) fresh (actively trading)
        if (pend_ref_px_ > 0.0 && lq_.px <= pend_ref_px_) return false; // (3) rising above the ref close
        return true;
    }

    // Incremental ladder state machine on the NEWEST daily close (harness run_trade order:
    // flush close never steps legs; activation close steps at fav=0; -thr detected AFTER the
    // day's stepping; a clip's spawn joins the leg list after the day's step loop).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                               // backtest TU / not wired: pure accounting
        const int N = (int)c_.size();
        if (N < 2) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];
        const double j   = c_[N - 1] / c_[N - 2] - 1.0;
        // GAP GUARD: block NEW windows across a multi-week data outage (a "1-day move" that
        // actually spans weeks); exits/flushes stay honoured.
        const bool contig = (ts_[N - 1] - ts_[N - 2]) <= (int64_t)86400 * 7;

        // 1) pending flush from yesterday's -thr day: flush ALL open legs at THIS close.
        if (exit_pend_) {
            for (Leg& L : legs_) {
                if (L.open && !L.clipped && !L.dead) book_clip_(L, cur, ts_sec, fwd, "WINDOW_EXIT");
            }
            legs_.clear(); win_ = false; exit_pend_ = false; spawned_ = 0; bar_ = 0; win_entry_ = 0;
        }

        // 1b) LIVE-CONFIRM pending management (poll-side re-check — operator "re-checked each
        //     tick/poll"). A pending window whose signal has REVERSED (gave back the whole +thr
        //     move) or which has sat unconfirmed for > 5 days is DROPPED, so a stale daily-close
        //     signal never latches forever waiting for a live confirm that isn't coming.
        if (cfg_.live_confirm && win_pend_) {
            const bool reversed = (pend_ref_px_ > 0.0 && cur <= pend_ref_px_ * (1.0 - cfg_.thr));
            const bool expired  = (pend_ref_ts_ > 0 && (ts_[N - 1] - pend_ref_ts_) > (int64_t)86400 * 5);
            if (reversed || expired) {
                std::printf("[AULAD][PEND-DROP] %s pending +%.1f%% window dropped (%s) — never live-confirmed\n",
                            cfg_.sym.c_str(), cfg_.thr * 100.0, reversed ? "reversed below arm" : "expired >5d");
                std::fflush(stdout);
                win_pend_ = false; pend_ref_px_ = 0.0; pend_ref_ts_ = 0;
            }
        }

        // 2) pending entry from yesterday's +thr day: base legs enter at THIS close.
        if (win_pend_) {
            if (cfg_.live_confirm) {
                // LIVE-CONFIRMATION GATE (STEP 2): do NOT open on the blind daily close. The
                // window stays pending; on_live_tick() opens it the instant a live tick confirms
                // session+fresh+rising. (Backtest/parity keeps live_confirm=false -> daily-close
                // entry below, byte-for-byte unchanged.)
            } else {
                activate_window_(cur, ts_sec, fwd);           // daily-close entry (backtest/parity)
            }
        }
        // 3) step legs on this close (activation close included, fav=0 — harness range(ei, xi)).
        else if (win_) {
            bar_ += 1;
            std::vector<Leg> born;
            for (Leg& L : legs_) {
                if (L.dead) continue;
                // S-2026-07-13 BE-ENTRY gate: a PENDING mimic leg opens ONLY at the first close
                // that covers +be_entry_pct off the window trigger (cost/BE made). If BE is never
                // made within pend_closes closes the leg dies unbooked — no open-into-loss
                // (feedback-no-immediate-entry-mimic-only). Managed from the NEXT close.
                if (L.pending) {
                    if ((cur / L.trig - 1.0) * 100.0 >= cfg_.be_entry_pct) {
                        L.pending = false; L.epx = cur; L.le = cur;
                        L.open = true; L.mfe = 0.0; L.ext = bar_; L.entry_ts = ts_sec;
                        if (fwd && open_fn_ && !g_aulad_catchup.load(std::memory_order_relaxed)) {
                            const double tp_dist_pts = cur * (L.arm / 100.0);
                            if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist_pts, cfg_.lot)) {
                                L.token = open_fn_(cfg_.live_sym, true, cfg_.lot, cur);
                                std::printf("[AULAD][BE-ENTER] %s LONG @%.2f (trig %.2f +%.2f%%)\n",
                                            leg_engine_(L.ti).c_str(), cur, L.trig, cfg_.be_entry_pct);
                                std::fflush(stdout);
                            }
                        }
                    } else if (--L.pbars <= 0) {
                        L.dead = true;   // BE never made -> cancel, nothing booked
                    }
                    continue;
                }
                const double fav = (cur / L.epx - 1.0) * 100.0;
                if (L.clipped) {   // reclip on trend resume
                    if (cfg_.reclip > 0 && L.pk > 0 && fav > L.pk * (1.0 + cfg_.reclip)) {
                        L.clipped = false; L.le = cur; L.entry_ts = ts_sec;
                        if (fwd && open_fn_ && !g_aulad_catchup.load(std::memory_order_relaxed)) {
                            L.token = open_fn_(cfg_.live_sym, true, cfg_.lot, cur);
                            std::printf("[AULAD][RECLIP] %s LONG @%.2f\n", leg_engine_(L.ti).c_str(), cur);
                            std::fflush(stdout);
                        }
                    }
                    continue;
                }
                if (fav > L.mfe + 1e-9) { L.mfe = fav; L.ext = bar_; }
                // ADVERSE-PROTECTION LOSS_CUT: cut the leg at -loss_cut% fav (backtested FREE;
                // fill at the observed close — a gap through the cut books the gap).
                if (cfg_.loss_cut_pct > 0 && fav <= -cfg_.loss_cut_pct) {
                    book_clip_(L, cur, ts_sec, fwd, "LOSS_CUT");
                    L.dead = true;
                    continue;
                }
                const bool armed = (L.mfe >= L.arm);
                const int  stall = bar_ - L.ext;
                double net_bp = 0.0; bool clipped_now = false;
                if (armed && L.sb > 0 && stall >= L.sb) {                       // TIGHT stall clip
                    L.pk = L.mfe; L.clipped = true; clipped_now = true;
                    net_bp = book_clip_(L, cur, ts_sec, fwd, "STALL_CLIP");
                } else if (armed && L.gb > 0 && fav <= L.mfe * (1.0 - L.gb)) {  // WIDE giveback clip
                    L.pk = L.mfe; L.clipped = true; clipped_now = true;
                    net_bp = book_clip_(L, cur, ts_sec, fwd, "GIVEBACK_CLIP");
                }
                // self-funding ladder: a net-positive clip spawns a new WIDE leg at this close.
                // S-2026-07-13: BE-gated when the mimic redo is on — the respawn goes PENDING
                // off the clip close ("re-enter after BE"), never an immediate open.
                if (clipped_now && net_bp > 0 && spawned_ < cfg_.cap) {
                    if (cfg_.be_entry_pct > 0.0)
                        born.push_back(make_pending_leg_(2, cfg_.w_arm, 0, cfg_.w_gb, cur, ts_sec));
                    else
                        born.push_back(make_leg_(2, cur, ts_sec, fwd));
                    spawned_ += 1;
                }
            }
            for (Leg& b : born) legs_.push_back(std::move(b));
        }

        // 4) parent detector on today's close-to-close move (one signal per day, harness elif).
        if (win_ && j <= -cfg_.thr) {
            exit_pend_ = true;                              // flush at the NEXT close
        } else if (!win_ && !win_pend_ && contig && j >= cfg_.thr) {
            // S-2026-07-08c AUTO-RETIREMENT gate: a proven-negative forward book
            // stops arming NEW windows (existing legs above still managed/flushed).
            double net_real_usd = 0.0;
            for (int ti = 0; ti < NT_; ++ti) net_real_usd += fwd_[ti].ret_real * cfg_.notional;
            if (cfg_.ranked_out) {
                // ranked-out names never arm (quiet by policy, not by fault)
            } else if (cfg_.retire_usd < 0.0 && net_real_usd <= cfg_.retire_usd) {
                if (!retired_logged_) {
                    retired_logged_ = true;
                    std::printf("[AULAD][RETIRED] %s forward net_real $%.0f <= $%.0f -- no new windows (auto-retirement, S-2026-07-08c)\n",
                                cfg_.sym.c_str(), net_real_usd, cfg_.retire_usd);
                    std::fflush(stdout);
                }
            } else {
                win_pend_ = true;                           // legs enter at the NEXT close (or live-confirm)
                pend_ref_px_ = cur; pend_ref_ts_ = ts_sec;  // daily-close entry reference for the rising test
            }
        }
        save_live_state_();
    }

    void load_fwd_book_() noexcept {
        std::ifstream f(cfg_.book_path);
        if (!f.is_open()) return;
        int ti = -1; double ret = 0; int clips = 0, wins = 0; double rreal = 0;
        std::string line;
        while (std::getline(f, line)) {
            const int n = std::sscanf(line.c_str(), "%d %lf %d %d %lf", &ti, &ret, &clips, &wins, &rreal);
            if (n >= 5 && ti >= 0 && ti < NT_) {
                fwd_[ti].ret = ret; fwd_[ti].clips = clips; fwd_[ti].wins = wins; fwd_[ti].ret_real = rreal;
            }
        }
    }
    void save_fwd_book_() const noexcept {
        const std::string tmp = cfg_.book_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (int ti = 0; ti < NT_; ++ti)
              f << ti << " " << fwd_[ti].ret << " " << fwd_[ti].clips << " "
                << fwd_[ti].wins << " " << fwd_[ti].ret_real << "\n"; }
#if defined(_WIN32)
        std::remove(cfg_.book_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg_.book_path.c_str());
    }

    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.ti << "," << r.entry << "," << r.exit << "," << r.ret << ","
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
            long long ets = 0, xts = 0; double rreal = 0, ureal = 0;
            const int n = std::sscanf(line.c_str(), "%d,%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n],%lf,%lf",
                                      &r.ti, &r.entry, &r.exit, &r.ret, &r.usd,
                                      &ets, &xts, reason, &rreal, &ureal);
            if (n >= 10 && r.ti >= 0 && r.ti < NT_) {
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
        std::string kind;
        while (f >> kind) {
            if (kind == "win") {
                int a = 0, p = 0, x = 0; long long ets = 0;
                f >> a >> p >> x >> bar_ >> spawned_ >> win_entry_ >> ets;
                win_ = (a != 0); win_pend_ = (p != 0); exit_pend_ = (x != 0); win_ets_ = (int64_t)ets;
            } else if (kind == "pend") {
                long long pts = 0; f >> pend_ref_px_ >> pts; pend_ref_ts_ = (int64_t)pts;
            } else if (kind == "leg") {
                Leg L; int op = 0, cl = 0, dd = 0; long long ets = 0; std::string tok;
                f >> L.ti >> op >> cl >> dd >> L.epx >> L.le >> L.arm >> L.gb >> L.sb
                  >> L.pk >> L.mfe >> L.ext >> ets >> tok;
                if (L.ti >= 0 && L.ti < NT_) {
                    L.open = (op != 0); L.clipped = (cl != 0); L.dead = (dd != 0);
                    L.entry_ts = (int64_t)ets; L.token = (tok == "-") ? std::string() : tok;
                    legs_.push_back(std::move(L));
                }
            } else if (kind == "leg2") {   // S-2026-07-13: adds pending/trig/pbars (BE-mimic)
                Leg L; int op = 0, cl = 0, dd = 0, pend = 0; long long ets = 0; std::string tok;
                f >> L.ti >> op >> cl >> dd >> L.epx >> L.le >> L.arm >> L.gb >> L.sb
                  >> L.pk >> L.mfe >> L.ext >> ets >> pend >> L.trig >> L.pbars >> tok;
                if (L.ti >= 0 && L.ti < NT_) {
                    L.open = (op != 0); L.clipped = (cl != 0); L.dead = (dd != 0); L.pending = (pend != 0);
                    L.entry_ts = (int64_t)ets; L.token = (tok == "-") ? std::string() : tok;
                    legs_.push_back(std::move(L));
                }
            } else { std::string rest; std::getline(f, rest); }
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << "win " << (win_ ? 1 : 0) << " " << (win_pend_ ? 1 : 0) << " " << (exit_pend_ ? 1 : 0)
            << " " << bar_ << " " << spawned_ << " " << win_entry_ << " " << (long long)win_ets_ << "\n";
          // S-2026-07-10: live-confirmation pending reference (own record line -> backward-compatible;
          // old state files simply lack it and pend_ref_ stays 0).
          f << "pend " << pend_ref_px_ << " " << (long long)pend_ref_ts_ << "\n";
          for (const Leg& L : legs_)   // leg2 (S-2026-07-13): pending/trig/pbars before token
              f << "leg2 " << L.ti << " " << (L.open ? 1 : 0) << " " << (L.clipped ? 1 : 0) << " "
                << (L.dead ? 1 : 0) << " " << L.epx << " " << L.le << " " << L.arm << " " << L.gb << " "
                << L.sb << " " << L.pk << " " << L.mfe << " " << L.ext << " "
                << (long long)L.entry_ts << " " << (L.pending ? 1 : 0) << " " << L.trig << " "
                << L.pbars << " " << (L.token.empty() ? "-" : L.token) << "\n"; }
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
//   stockladder_companion_state.json (served by /api/stockladder_companion). ──
class StockLadderBook {
public:
    ~StockLadderBook() { stop_poller(); }

    void add(StockLadderSym::Config c) {
        col_[c.sym] = syms_.size();   // name -> index (also the wide-CSV column lookup key)
        syms_.emplace_back(std::move(c));
    }

    StockLadderSym* find(const std::string& sym) {
        auto it = col_.find(sym);
        return (it == col_.end()) ? nullptr : &syms_[it->second];
    }

    void set_exec(StockLadderSym::OpenFn o, StockLadderSym::CloseFn c,
                  StockLadderSym::GateFn g, StockLadderSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }

    // S-2026-07-10: arm the live-confirmation entry gate on every name (live TU only).
    void set_live_confirm(bool b) { for (auto& s : syms_) s.set_live_confirm(b); }

    // S-2026-07-10: ENABLE the in-binary daily-close writer (live TU only; default OFF so no
    // backtest/parity TU that instantiates this book ever appends to the research CSV). When on,
    // the poll loop appends one WIDE-format daily-close row per US-cash-close (20:00 UTC weekday),
    // sourcing each subscribed name's close from its last live IBKR L1 mid — this REPLACES the
    // external yfinance producer (tools/rdagent/vps_stockmover_feed.py, task OmegaStockMoverFeed).
    // min_cov = minimum fresh-name coverage required to write a row (bridge-down/thin day -> skip).
    void enable_daily_close_writer(bool b, int min_cov = 10) noexcept { dc_writer_ = b; dc_min_cov_ = min_cov; }

    // LIVE L1 tick for `sym` (the ticker, from the IBKR bridge -> omega_main on_book). Routes to
    // that name's live quote store + confirmation gate. col_ is built once in add() (before the
    // poller starts) and never mutated after, so the membership check is lockless; the mutex is
    // taken only for an actual ladder name. Re-publishes state ONLY when a window opened.
    void on_live_tick(const std::string& sym, double bid, double ask, int64_t now_ms) {
        auto it = col_.find(sym);
        if (it == col_.end()) return;                 // not a ladder name -> ignore, no lock taken
        std::lock_guard<std::mutex> lk(mu_);
        if (syms_[it->second].on_live_tick(bid, ask, now_ms))
            recompute_unlocked_();
    }

    // STEP 3 one-shot (S-2026-07-10): on the FIRST boot after the live-confirmation gate ships,
    // clear every open UNCONFIRMED leg (the pre-gate blind daily-close opens — the 18 "fake"
    // legs). Guarded by a sentinel file so a later restart NEVER nukes legitimately live-confirmed
    // legs. cwd path (C:\Omega), same convention as the per-name state files.
    size_t flush_all_unconfirmed_once(const char* reason) {
        { std::ifstream sf(flush_sentinel_path_); if (sf.is_open()) return 0; }   // already migrated
        std::lock_guard<std::mutex> lk(mu_);
        size_t cleared = 0;
        for (auto& s : syms_) cleared += (size_t)s.flush_unconfirmed(reason);
        { std::ofstream sf(flush_sentinel_path_, std::ios::trunc);
          if (sf.is_open()) sf << "flushed " << reason << "\n"; }
        recompute_unlocked_();
        std::printf("[AULAD][FLUSH] one-shot live-confirm migration: %zu open leg(s) cleared across %zu names (sentinel %s)\n",
                    cleared, syms_.size(), flush_sentinel_path_.c_str());
        std::fflush(stdout);
        return cleared;
    }

    // warm-seed from the WIDE daily-close CSV (header ",NAME1,NAME2,...", rows "YYYY-MM-DD,c1,c2,...")
    size_t seed_from_wide_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[AULAD][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string header;
        if (!std::getline(f, header)) return 0;
        std::unordered_map<int, size_t> colsym;
        { std::stringstream hs(header); std::string tok; int ci = 0;
          while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = it->second; ++ci; } }
        // S-2026-07-08c BOOT CATCH-UP: rows the LIVE path already processed before the
        // last shutdown (persisted watermark) are history -> seed (no window logic).
        // Rows NEWER than the watermark arrived while we were DOWN -> replay them
        // through the LIVE window logic (orders/ledger suppressed via g_aulad_catchup)
        // so pending->active conversions and flushes land on their correct closes
        // instead of latching. First-ever deploy (no watermark file) seeds everything
        // (deploy-forward semantics unchanged).
        int64_t watermark = 0;
        { std::ifstream wf(lastseen_path_); long long v = 0; if (wf.is_open() && (wf >> v)) watermark = v; }
        std::string line; size_t rows = 0, caught_up = 0;
        if (watermark > 0) g_aulad_catchup.store(true, std::memory_order_relaxed);
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
        g_aulad_catchup.store(false, std::memory_order_relaxed);
        save_lastseen_(last_seed_ts_);
        std::printf("[AULAD][SEED] wide-csv %s: %zu rows (%zu caught-up live, watermark=%lld), %zu/%zu names mapped, last=%lld\n",
                    path.c_str(), rows, caught_up, (long long)watermark,
                    colsym.size(), syms_.size(), (long long)last_seed_ts_);
        std::fflush(stdout);
        return rows;
    }

    // S-2026-07-08c: persisted live watermark (last ts processed through LIVE logic).
    void save_lastseen_(int64_t ts) const noexcept {
        if (ts <= 0) return;
        std::ofstream wf(lastseen_path_, std::ios::trunc);
        if (wf.is_open()) wf << (long long)ts << "\n";
    }

    size_t seed_dumps_all() { size_t n = 0; for (auto& s : syms_) n += s.seed_dump(); return n; }
    void   finalize_all() { for (auto& s : syms_) s.finalize_seed(); recompute_and_write(); }

    // LIVE: one closed daily bar for `sym`. (Poller calls the row variant; this is for tests.)
    void on_daily_bar(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        if (auto* s = find(sym)) { s->on_daily_bar(ts_sec, close); recompute_unlocked_(); }
    }

    void start_poller(const std::string& rel, int poll_ms = 900000 /*15min*/) {
        csv_rel_ = rel; poll_ms_ = poll_ms;
        last_seen_ts_ = last_seed_ts_;   // resume forward from the last seeded date
        // GUARD: boot seed found NO file -> first poller load must SEED (history only), not book
        // years of history as fake forward PnL (deploy_ts never stamped).
        seed_missed_ = (last_seed_ts_ == 0);
        running_.store(true);
        thread_ = std::thread([this]{ poll_loop_(); });
        std::printf("[AULAD][POLL] watching %s (poll=%dms, resume-from=%lld)\n",
                    rel.c_str(), poll_ms_, (long long)last_seen_ts_);
        std::fflush(stdout);
    }
    void stop_poller() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    // MANUAL KILL-ALL fan-out (S-2026-07-20): on_tick g_flatten_all_request routes
    // here because these books have no register_source. Returns legs cleared.
    int kill_all(int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& s : syms_) n += s.kill_all(now_sec);
        if (n) recompute_unlocked_();
        return n;
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
    // all mutation/read of syms_ after start_poller() goes through mu_ (poller thread dispatches
    // bars + rewrites state while the main thread queries totals/state). Exec callbacks are
    // invoked FROM the poller thread — shadow no-ops are thread-safe; re-check before LIVE flip.
    mutable std::mutex mu_;

    std::string state_json_unlocked_() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot_usd = 0.0, tot_usd_real = 0.0;
        for (const auto& s : syms_) { last_ts = std::max(last_ts, s.last_ts()); tot_usd += s.book_usd(); tot_usd_real += s.book_usd_real(); }
        o << "{\"engine\":\"stockmover-mimic-ladder\",\"shadow\":true,\"grade\":\"daily-close\",";
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

    std::vector<StockLadderSym> syms_;
    std::unordered_map<std::string, size_t> col_;     // name -> sym index
    std::string state_path_ = "stockladder_companion_state.json";
    int64_t last_seed_ts_ = 0;
    // S-2026-07-08c: live watermark file (cwd C:\Omega, same convention as the
    // per-name deploy/book/live files).
    std::string lastseen_path_ = "stockladder_companion_lastseen.txt";
    // S-2026-07-10: one-shot live-confirmation flush sentinel (STEP 3).
    std::string flush_sentinel_path_ = "stockladder_companion_liveconfirm_flush.done";

    // poller
    std::string      csv_rel_;
    int              poll_ms_ = 900000;
    std::atomic<bool> running_{false};
    std::atomic<int64_t> last_seen_ts_{0};
    bool             seed_missed_ = false;   // boot-seed found no CSV -> first poll load seeds, not books
    std::thread      thread_;

    // S-2026-07-10 IN-BINARY DAILY-CLOSE WRITER state (replaces the yfinance producer).
    bool        dc_writer_    = false;   // OFF unless engine_init enables it (live TU only)
    int         dc_min_cov_   = 10;      // min fresh-name coverage to append a row
    std::string dc_done_path_ = "stockladder_companion_dailyclose.done";   // persisted last-written YMD (idempotency)

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

    // ── IN-BINARY DAILY-CLOSE WRITER (S-2026-07-10) ─────────────────────────────────────────
    // Format YYYY-MM-DD from a UTC day-index (Howard Hinnant civil_from_days, inverse of
    // days_from_civil_ above).
    static std::string ymd_from_day_(int64_t z) noexcept {
        z += 719468;
        const int64_t  era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = (unsigned)(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const int64_t  y   = (int64_t)yoe + era * 400;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp  = (5 * doy + 2) / 153;
        const unsigned d   = doy - (153 * mp + 2) / 5 + 1;
        const unsigned m   = mp + (mp < 10 ? 3 : (unsigned)-9);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%04lld-%02u-%02u", (long long)(y + (m <= 2)), m, d);
        return std::string(buf);
    }
    // Price -> string: %.4f with trailing zeros + dot trimmed (matches the file's "517.4"/"166.58"
    // daily-close style while keeping sub-dollar precision, e.g. "0.0064").
    static std::string fmt_px_(double v) noexcept {
        char buf[32]; std::snprintf(buf, sizeof buf, "%.4f", v);
        std::string s(buf);
        const size_t dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last == dot) --last;                 // "5.0000" -> "5"
            s.erase(last + 1);
        }
        return s;
    }
    void save_dc_done_(const std::string& d) const noexcept {
        std::ofstream f(dc_done_path_, std::ios::trunc);
        if (f.is_open()) f << d << "\n";
    }

    // Once per US-cash-close (>= 20:00 UTC on a weekday), snapshot each subscribed name's last live
    // IBKR L1 mid and APPEND a new dated row to the WIDE daily-close CSV, byte-for-byte in the
    // header's column order (blank for any header name that is not a ladder name or has no fresh
    // tick today). Idempotent: guarded by both a persisted last-written YMD and the CSV's own last
    // row date. Called at the top of each poll iteration; the SAME poll cycle then re-reads and
    // dispatches the newly-written row through the live window logic.
    void maybe_write_daily_close_(const std::string& path) {
        if (!dc_writer_) return;
        const int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // TIME GATE: weekday, at/after the 20:00 UTC US cash close.
        const std::time_t t = (std::time_t)(now_ms / 1000);
        std::tm g{};
#if defined(_WIN32)
        gmtime_s(&g, &t);
#else
        gmtime_r(&t, &g);
#endif
        if (g.tm_wday == 0 || g.tm_wday == 6) return;   // Sun / Sat: US market closed
        if (g.tm_hour < 20)                    return;   // before the 20:00 UTC cash close
        const int64_t     today    = (now_ms / 1000) / 86400;   // UTC civil day index
        const std::string date_str = ymd_from_day_(today);
        // GUARD 1: already written today (persisted watermark).
        { std::ifstream df(dc_done_path_); std::string d;
          if (df.is_open() && (df >> d) && d == date_str) return; }
        // Read the current header (column order) + the last row's date from the file.
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string header;
        if (!std::getline(f, header)) return;
        { std::string line, last;
          while (std::getline(f, line)) if (!line.empty()) last.swap(line);
          int y = 0, m = 0, dd = 0;
          if (!last.empty() && std::sscanf(last.c_str(), "%d-%d-%d", &y, &m, &dd) == 3) {
              char lb[16]; std::snprintf(lb, sizeof lb, "%04d-%02d-%02d", y, m, dd);
              if (date_str == lb) { save_dc_done_(date_str); return; }   // GUARD 2: row already present
          } }
        // Header tokens -> column order (token 0 = the date/index column, "").
        std::vector<std::string> cols;
        { std::stringstream hs(header); std::string tok; while (std::getline(hs, tok, ',')) cols.push_back(tok); }
        const int nf = (int)cols.size();
        // Snapshot each header column's value under the lock (blank unless a ladder name with a
        // fresh live mid dated to TODAY).
        std::vector<std::string> vals(nf);
        int cov = 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int ci = 1; ci < nf; ++ci) {
                auto it = col_.find(cols[ci]);
                if (it == col_.end()) continue;             // not a ladder name -> blank
                double px = 0.0; int64_t tms = 0;
                if (syms_[it->second].last_live_px(px, tms) && (tms / 1000) / 86400 == today) {
                    vals[ci] = fmt_px_(px); ++cov;
                }
            }
        }
        if (cov < dc_min_cov_) {
            std::printf("[AULAD][DCWRITE] %s SKIP -- only %d/%d names fresh (< min_cov %d); no row appended (bridge down/thin)\n",
                        date_str.c_str(), cov, nf - 1, dc_min_cov_);
            std::fflush(stdout);
            return;
        }
        std::ostringstream row; row << date_str;
        for (int ci = 1; ci < nf; ++ci) row << "," << vals[ci];
        row << "\n";
        { std::ofstream of(path, std::ios::app); if (!of.is_open()) return; of << row.str(); }
        save_dc_done_(date_str);
        std::printf("[AULAD][DCWRITE] %s appended daily-close row: %d/%d names from live IBKR L1 -> %s (in-binary writer; yfinance OmegaStockMoverFeed retired)\n",
                    date_str.c_str(), cov, nf - 1, path.c_str());
        std::fflush(stdout);
    }

    void poll_loop_() {
        while (running_.load()) {
            for (int slept = 0; slept < poll_ms_ && running_.load(); slept += 200)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (!running_.load()) break;
            const std::string path = omega::resolve_seed_path(csv_rel_);
            maybe_write_daily_close_(path);   // S-2026-07-10: append today's close (~20:00 UTC) BEFORE dispatch
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
                    for (auto& s : syms_) s.finalize_seed();
                    last_seen_ts_.store(newest); seed_missed_ = false;
                    save_lastseen_(newest);      // S-2026-07-08c: advance the watermark
                    recompute_unlocked_();
                } else if (fresh > 0) {
                    last_seen_ts_.store(newest);
                    save_lastseen_(newest);      // S-2026-07-08c: advance the watermark
                    recompute_unlocked_();
                }
            }
            if (fresh > 0 && as_seed) {
                std::printf("[AULAD][POLL] first-load SEED %d rows (boot-seed missed), stamped deploy=%lld\n",
                            fresh, (long long)newest);
                std::fflush(stdout);
            } else if (fresh > 0) {
                std::printf("[AULAD][POLL] %d new daily row(s), newest=%lld\n", fresh, (long long)newest);
                std::fflush(stdout);
            }
        }
    }
};

// Singleton — accessor mirrors omega::stockmover_befloor_book().
inline StockLadderBook& stockmover_ladder_book() noexcept {
    static StockLadderBook inst;
    return inst;
}

} // namespace omega
