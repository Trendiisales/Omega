#pragma once
// =============================================================================
// StockDayMoverLadderCompanion — per-NAME BIGCAP daily-mover UP-JUMP LADDER
// (long-only) companion books. NO FLOOR anywhere — this is the successor the
// StockDayMoverBeFloorCompanion retirement left open (BE-floor real-fill
// -$110.7k; the no-floor giveback LADDER is the only trail family that
// survives real fills per the S-2026-07-07 late-arm sweep).
//
// C++ in-binary engine, faithful port of the VALIDATED research:
//   math   : backtest/bigcap_upjump_ladder_bt.py (S-2026-07-07 operator item 4)
//            — parent = +3% day close-to-close -> enter NEXT close (real fill),
//            exit on -3% day (flush at the close AFTER the -3% day), end-flush MTM.
//            Legs: TIGHT a0.5/s2/g0 (stall-only banker) + WIDE a8/s0/g50
//            (giveback runner) + self-funding ladder cap 5 (a net-positive clip
//            spawns a new WIDE leg at the clip close), reclip 5%, RT 8bp/clip.
//   result : 39-name pooled book n=4,981 clips net +7,044% of clip notional
//            PF 1.58, H1 +2,813 / H2 +4,231, bear +3,958; 2x-cost (16bp) PASS;
//            neighbors plateau; ex-semis +3,875% PF 1.41; FULL 565-name universe
//            +59,789% PF 1.29 (not list survivorship). Evidence
//            outputs/BIGCAP_UPJUMP_LADDER_2026-07-07.md · vault BigCapUpJumpLadder.
//
// ADVERSE-PROTECTION: backtested verdict (mandate + feedback-engine-loss-
//   protection-provision) — giveback trail after arm + LOSS_CUT 15% on the leg.
//   LOSS_CUT 15 is FREE: net +7,057% (== baseline) with worst clip -32.6% -> -28.1%
//   (daily-close fills: a gap through -15% books the observed close, hence the
//   worst clip can exceed the cut). Un-cut/un-armed legs flush MTM at parent exit
//   (never abandoned). Same harness, same data, same costs as the PASS above.
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

// ── one stock name's long-only upjump-ladder book (returns-based) ────────────
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
        // fractional leg, BIGCAP_UPJUMP_LADDER_2026-07-07.md). 0 = disabled.
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
        double reclip        = 0.05;      // re-enter when fav > peak*(1+reclip)
        int    cap           = 5;         // max legs spawned per parent window (incl 2 base)
        double loss_cut_pct  = 15.0;      // ADVERSE-PROTECTION: cut an open leg at -15% fav (FREE, backtested)
        double rt_cost_bp    = 8.0;       // REAL round-trip cost (bp of entry) debited per clip (validated gate)
        double notional      = 10000.0;   // $ per clip; USD = return * notional (name-agnostic sizing)
        double lot           = 1.0;       // order-path lot (shares/CFD units decided at flip)
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

        // 2) pending entry from yesterday's +thr day: base legs enter at THIS close.
        if (win_pend_) {
            win_pend_ = false; win_ = true; bar_ = 0; spawned_ = 2;
            win_entry_ = cur; win_ets_ = ts_sec;
            legs_.clear();
            legs_.push_back(make_leg_(0, cur, ts_sec, fwd));   // TIGHT banker
            legs_.push_back(make_leg_(1, cur, ts_sec, fwd));   // WIDE runner
        }
        // 3) step legs on this close (activation close included, fav=0 — harness range(ei, xi)).
        else if (win_) {
            bar_ += 1;
            std::vector<Leg> born;
            for (Leg& L : legs_) {
                if (L.dead) continue;
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
                // self-funding ladder: a net-positive clip spawns a new WIDE leg at this close
                if (clipped_now && net_bp > 0 && spawned_ < cfg_.cap) {
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
                win_pend_ = true;                           // legs enter at the NEXT close
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
            } else if (kind == "leg") {
                Leg L; int op = 0, cl = 0, dd = 0; long long ets = 0; std::string tok;
                f >> L.ti >> op >> cl >> dd >> L.epx >> L.le >> L.arm >> L.gb >> L.sb
                  >> L.pk >> L.mfe >> L.ext >> ets >> tok;
                if (L.ti >= 0 && L.ti < NT_) {
                    L.open = (op != 0); L.clipped = (cl != 0); L.dead = (dd != 0);
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
          for (const Leg& L : legs_)
              f << "leg " << L.ti << " " << (L.open ? 1 : 0) << " " << (L.clipped ? 1 : 0) << " "
                << (L.dead ? 1 : 0) << " " << L.epx << " " << L.le << " " << L.arm << " " << L.gb << " "
                << L.sb << " " << L.pk << " " << L.mfe << " " << L.ext << " "
                << (long long)L.entry_ts << " " << (L.token.empty() ? "-" : L.token) << "\n"; }
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
        o << "{\"engine\":\"stockmover-upjump-ladder\",\"shadow\":true,\"grade\":\"daily-close\",";
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
