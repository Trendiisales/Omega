#pragma once
// =============================================================================
// StockDipTurtleEngine — TWO per-name daily-close LONG-only book families on US
// single stocks (S-2026-07-08c, AUDITED_CONFIGS StockConnorsDip_RESEARCH PF1.60
// + StockTurtleD1_RESEARCH PF2.13; evidence outputs/STOCK_OTHER_ENGINES_
// 2026-07-08.txt + outputs/STOCK_TURTLE_PERNAME_2026-07-08.txt):
//
//   StockDip    (ConnorsRSI2 archetype): enter LONG at the daily close when
//               close > SMA200(daily) AND RSI2 < 10 (Cutler RSI, the house
//               ConnorsRSI2Engine::_rsi convention); exit at the first close >
//               SMA5 (incl. today) OR after 10 trading days. Wired names =
//               the individual all-6 passers of the audited per-name split:
//               MU,NVDA,AVGO,DELL,CRDO,STX,INTC,AMD,AAPL,TPR,MSFT.
//   StockTurtle (Donchian archetype): enter LONG at the daily close when
//               close > max(prior 20 closes); exit at the first close <
//               min(prior 10 closes). Wired names = the all-6 passers of the
//               S-2026-07-08c rerun split (outputs/STOCK_TURTLE_PERNAME_
//               2026-07-08.txt): NVDA,AVGO,STX,DD,AMD,AAPL,TPR,BMY,SWKS,MSFT,QCOM.
//
// ADVERSE-PROTECTION: (S-2026-07-08c, backtested verdicts — one per family)
//   StockDip    = trend-filter-gated MEAN REVERTER: the close>SMA200 gate + the
//     10-trading-day time-stop IS the protection (the rejected-cold-cut class,
//     same verdict as ConnorsRSI2Engine.hpp: "a cold loss-cut on a mean-reverter
//     would cut exactly the dip it is paid to buy"). Faithful 7yr BT on the 11
//     wired names, 8bp RT: worst trade -23.0% (AMD), worst per-name banked-curve
//     DD -47.6% (MU); pooled 18-name PF 1.60 both-halves+ WITH this exit set and
//     NO cold cut. Auto-retirement (below) is the book-level backstop.
//   StockTurtle = channel trend-follower: the 10-day-low CLOSE channel exit IS
//     the trailing protection (trail-only by design — the 2026-06-17 sweep class).
//     Faithful BT on the 11 wired names, 8bp RT: worst trade -19.5% (STX), worst
//     per-name banked-curve DD -41.3% (TPR); pooled PF 2.13 with exactly this
//     exit and no additional cut. Auto-retirement is the book-level backstop.
//
// AUTO-RETIREMENT (per name, per family): once a name's FORWARD banked net_real
// falls to <= retire_usd, NO new entries (open position still managed/exits
// normally); loud one-shot log. Defaults = ~2x the worst BT per-name banked-curve
// DD episode on $10k notional: StockDip -$9,500 (2x -$4,756 MU), StockTurtle
// -$8,500 (2x -$4,130 TPR).
//
// Sizing/costs: notional-based ($10k/position default), USD = return * notional.
// rt_cost_bp (8bp RT, the validated gate — both PASSes are net-of-8bp) is debited
// from every trade's ret_real; entry additionally calls ExecutionCostGuard::
// is_viable via the wired gate_fn (the S-2026-07-08c US-equity cost row).
//
// FEED: the wide daily-close CSV data/rdagent/sp500_long_close.csv (RDAgent
// refresh_close_ibkr.py, IBKR 4002); own 15-min poller thread; monotonic
// per-name ingest; >50% single-day-move reject with 3-consecutive resync
// (split/seam healing) — the StockDayMoverLadderCompanion house pattern.
//
// Deploy-forward: seed primes indicators ONLY; per-name persisted deploy_ts
// anchor; forward accounting starts at $0. BOOT CATCH-UP watermark (the
// S-2026-07-08c bug class): the book persists the last LIVE-processed ts; at
// boot, rows <= watermark seed silently, rows beyond replay through LIVE logic
// with broker/ledger calls suppressed (g_sdt_catchup) so restarts never eat an
// entry/exit NOR double-book. Wire exec callbacks BEFORE seeding (cutover-#9:
// live logic hard-returns if exec unset). Open position + banked net survive
// restart (banked net restored by summing the name's own closed csv).
//
// SHADOW only, judged STANDALONE, long-only. Ledger tags StockDip_<SYM> /
// StockTurtle_<SYM>, shadow=true.
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

// BOOT CATCH-UP order suppression (see header): rows that arrived while the
// process was DOWN replay through the LIVE logic at boot with broker orders +
// central-ledger writes suppressed; the book's own totals still record.
static std::atomic<bool> g_sdt_catchup{false};

// ── one (name, family) long-only daily-close book ────────────────────────────
class StockDipTurtleSym {
public:
    enum Family { DIP = 0, TURTLE = 1 };

    struct Config {
        std::string sym      = "NVDA";   // ticker (wide-CSV column + persist key)
        std::string live_sym = "NVDA";   // symbol the live order path trades
        int         family   = DIP;      // DIP or TURTLE
        // DIP params (audited cell): SMA200 trend gate, RSI2<10 entry, SMA5-bounce
        // or 10-trading-day exit. TURTLE params: 20-close high entry / 10-close low exit.
        int    dip_trend_sma = 200;
        int    dip_rsi_len   = 2;
        double dip_rsi_in    = 10.0;
        int    dip_exit_sma  = 5;
        int    dip_max_hold  = 10;
        int    tur_hi_n      = 20;
        int    tur_lo_n      = 10;
        double retire_usd    = -9500.0;  // AUTO-RETIREMENT bar (family default set at wiring)
        double rt_cost_bp    = 8.0;      // validated RT cost debit (bp of entry)
        double notional      = 10000.0;  // $ per position; USD = return * notional
        double lot           = 1.0;      // order-path lot
        std::string deploy_path;         // persisted deploy-forward anchor
        std::string bars_path;           // persisted LIVE forward daily bars
        std::string live_path;           // persisted OPEN position state
        std::string closed_path;         // persisted CLOSED forward trades (banked net source)
    };

    // LIVE EXECUTION WIRING — identical contract to StockLadderSym. Set ONLY in the
    // live main TU; null in a backtest TU -> live_step_ short-circuits.
    using OpenFn   = std::function<std::string(const std::string& sym, bool is_long, double lots, double px)>;
    using CloseFn  = std::function<void(const std::string& sym, bool orig_is_long, double lots, double px, const std::string& token)>;
    using GateFn   = std::function<bool(const std::string& sym, double tp_dist_pts, double lots)>;
    using LedgerFn = std::function<void(const std::string& engine, const std::string& sym, bool is_long,
                                        double entry_px, double exit_px, double lots,
                                        int64_t entry_ts, int64_t exit_ts, const char* reason)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) noexcept {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit StockDipTurtleSym(Config c) : cfg_(std::move(c)) {
        const std::string key = std::string(cfg_.family == TURTLE ? "turtle_" : "dip_") + lower_(cfg_.sym);
        if (cfg_.deploy_path.empty()) cfg_.deploy_path = "stockdipturtle_" + key + "_deploy_ts.txt";
        if (cfg_.bars_path.empty())   cfg_.bars_path   = "stockdipturtle_" + key + "_daily.csv";
        if (cfg_.live_path.empty())   cfg_.live_path   = "stockdipturtle_" + key + "_live.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "stockdipturtle_" + key + "_closed.csv";
        std::ifstream f(cfg_.deploy_path);
        if (f.is_open()) { long long v = 0; if (f >> v) { deploy_ts_ = v; deploy_loaded_ = true; } }
        load_closed_();      // restores banked net by summing the closed csv
        load_live_state_();  // restores an open position across restart
    }

    const std::string& sym() const { return cfg_.sym; }
    int         family() const { return cfg_.family; }
    std::string engine_tag() const {
        return std::string(cfg_.family == TURTLE ? "StockTurtle_" : "StockDip_") + cfg_.sym;
    }
    size_t  bars() const { return ts_.size(); }
    int64_t last_ts() const { return ts_.empty() ? 0 : ts_.back(); }
    double  book_usd() const { return banked_ret_ * cfg_.notional; }
    double  book_usd_real() const { return banked_ret_real_ * cfg_.notional; }
    int     clips() const { return clips_; }
    bool    in_pos() const { return pos_.active; }
    // S-2026-07-15: entry_ts of the currently-OPEN position (0 if flat). The
    // registry collects these at boot so the central phantom-drop guard
    // (trade_lifecycle.hpp) can EXEMPT them: a StockDip position genuinely
    // restored across a restart has a real entry that predates boot, and its
    // eventual SMA5_BOUNCE/TIME_STOP close must BOOK its (often multi-day) PnL
    // into g_omegaLedger, not be silently dropped as a warm-seed phantom.
    // History: DELL +$712 / MU +$492 (07-11->12, +7.1%/+4.9%) were phantom-
    // dropped because a reboot fell between entry and exit -> the real dip
    // winners never reached the desk headline.
    int64_t open_entry_ts() const noexcept { return pos_.active ? pos_.entry_ts : 0; }

    // S-2026-07-16l STOCKDIP BE-MIMIC hooks (operator: bigcap up-jump engines killed,
    // replaced by 2x BE-mimic cells layered on the ONE bigcap book that fires — StockDip).
    // The mimic runs its OWN independent book (feedback-companion-independent-engine): it
    // never touches this real position. open_cb fires one-way the instant a DIP entry opens
    // (LONG, at the entry close); bar_cb feeds the mimic that name's ACCEPTED daily close each
    // live bar for leg management. Both are no-ops until engine_init arms the mimic registry
    // (deploy-forward). DIP family only (StockTurtle is a separate character, not mimicked).
    using MimicOpenCb = std::function<void(const std::string& sym, int dir, double px, int64_t ts)>;
    using MimicBarCb  = std::function<void(const std::string& sym, double close, int64_t ts)>;
    void set_mimic_cbs(MimicOpenCb o, MimicBarCb b) noexcept {
        mimic_open_cb_ = std::move(o); mimic_bar_cb_ = std::move(b);
    }
    bool is_dip() const noexcept { return cfg_.family == DIP; }

    // seed one historical daily close (primes indicators only — deploy-forward gate).
    void seed_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ingest_(norm_ts_(ts_sec), close);
    }

    // Call ONCE after all seeding: stamp+persist deploy_ts on first-ever boot; load on restart.
    void finalize_seed() noexcept {
        dedup_sort_();
        if (!deploy_loaded_ && !ts_.empty()) {
            deploy_ts_ = ts_.back(); deploy_loaded_ = true;
            std::ofstream f(cfg_.deploy_path, std::ios::trunc);
            if (f.is_open()) f << (long long)deploy_ts_ << "\n";
        }
    }

    // LIVE feed: one CLOSED daily bar (close = official daily close).
    void on_daily_bar(int64_t ts_sec, double close) noexcept {
        if (close <= 0.0) return;
        ts_sec = norm_ts_(ts_sec);
        if (!ts_.empty() && ts_sec <= ts_.back()) return;   // monotonic live append only
        // DATA-INTEGRITY GUARD + 3-consecutive SELF-HEALING resync (the ladder house
        // pattern): reject any >50% single-day move (torn read / x1000 class); 3
        // consecutive rejects at the new level = a real split/seam -> accept the
        // level and VOID any open position WITHOUT booking (seam PnL is fake).
        if (!c_.empty()) {
            const double jump = std::fabs(close / c_.back() - 1.0);
            if (jump > 0.50) {
                if (++rej_streak_ >= 3) {
                    std::printf("[SDT][RESYNC] %s accepts level %.4f after %d consecutive >50%% rejects vs %.4f — split/seam; position voided unbooked\n",
                                engine_tag().c_str(), close, rej_streak_, c_.back());
                    std::fflush(stdout);
                    rej_streak_ = 0;
                    pos_ = Pos();          // void, no book: seam PnL is not real
                    ingest_(ts_sec, close);
                    append_dump_(ts_sec, close);
                    save_live_state_();
                    return;                // no live_step_: seam day-move is fake
                }
                std::printf("[SDT][REJECT] %s daily close %.4f vs prev %.4f (%.0f%% jump) — torn read/split? bar dropped (streak %d/3)\n",
                            engine_tag().c_str(), close, c_.back(), jump * 100.0, rej_streak_);
                std::fflush(stdout);
                return;
            }
            rej_streak_ = 0;
        }
        ingest_(ts_sec, close);
        append_dump_(ts_sec, close);
        live_step_(ts_sec);
        // S-2026-07-16l: feed the StockDip BE-mimic this name's ACCEPTED daily close (torn/
        // resync bars returned early above, so the mimic never sees a fake ×1000 seam). No-op
        // until the mimic registry is armed (deploy-forward) and for non-DIP names.
        if (mimic_bar_cb_) mimic_bar_cb_(cfg_.sym, close, ts_sec);
    }

    // Reload persisted LIVE forward daily bars (price history continuity across restart).
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

    // Emit this book's desk JSON object. REAL FORWARD TRADES ONLY ($0 until first live close).
    std::string sym_json() const {
        const double notl = cfg_.notional;
        const double cur = c_.empty() ? 0.0 : c_.back();
        std::ostringstream o; o << std::fixed;
        o << "{\"engine\":\"" << engine_tag() << "\",\"sym\":\"" << cfg_.sym
          << "\",\"family\":\"" << (cfg_.family == TURTLE ? "turtle" : "dip")
          << "\",\"bars\":" << ts_.size()
          << ",\"deploy_ts\":" << (long long)deploy_ts_
          << ",\"ts\":" << (long long)last_ts() << ",";
        o.precision(0); o << "\"notional\":" << notl << ",";
        o << "\"clips\":" << clips_ << ",\"wins\":" << wins_ << ",";
        o.precision(3); o << "\"pct\":" << (banked_ret_ * 100.0)
                          << ",\"pct_real\":" << (banked_ret_real_ * 100.0) << ",";
        o.precision(0); o << "\"usd\":" << (banked_ret_ * notl)
                          << ",\"usd_real\":" << (banked_ret_real_ * notl) << ",";
        o << "\"retired\":" << (retired_() ? "true" : "false") << ",\"open\":";
        if (pos_.active && pos_.epx > 0) {
            const double u  = (cur > 0) ? (cur / pos_.epx - 1.0) : 0.0;
            const double ur = u - cfg_.rt_cost_bp / 1e4;
            o << "{\"entry\":"; o.precision(2); o << pos_.epx << ",\"cur\":" << cur << ",";
            o.precision(3); o << "\"upnl_pct\":" << (u * 100.0) << ",\"upnl_pct_real\":" << (ur * 100.0) << ",";
            o.precision(0); o << "\"upnl_usd\":" << (u * notl) << ",\"upnl_usd_real\":" << (ur * notl)
                              << ",\"held\":" << pos_.held << ",\"entry_ts\":" << (long long)pos_.entry_ts << "}";
        } else o << "null";
        o << ",\"trades\":[";
        int ntr = 0;
        for (auto it = closed_.rbegin(); it != closed_.rend(); ++it) {
            const Closed& t = *it;
            if (ntr++) o << ",";
            o.precision(2); o << "{\"entry\":" << t.entry << ",\"exit\":" << t.exit << ",";
            o.precision(3); o << "\"pct\":" << (t.ret * 100.0) << ",\"pct_real\":" << (t.ret_real * 100.0) << ",";
            o.precision(0); o << "\"usd_real\":" << (t.ret_real * notl)
                              << ",\"reason\":\"" << t.reason << "\",\"entry_ts\":" << (long long)t.ets
                              << ",\"exit_ts\":" << (long long)t.xts << "}";
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
    int     rej_streak_ = 0;
    bool    retired_logged_ = false;   // one-shot auto-retirement log

    OpenFn   open_fn_;
    CloseFn  close_fn_;
    GateFn   gate_fn_;
    LedgerFn ledger_fn_;
    MimicOpenCb mimic_open_cb_;   // S-2026-07-16l: one-way DIP-open trigger -> StockDip BE-mimic
    MimicBarCb  mimic_bar_cb_;    // S-2026-07-16l: per-name daily-close feed -> mimic leg mgmt

    struct Pos {
        bool    active = false;
        double  epx = 0;         // entry close
        int     held = 0;        // trading days since entry (entry close = 0)
        int64_t entry_ts = 0;
        std::string token;
    };
    Pos pos_;

    // banked forward book — restored by summing closed csv at boot
    double banked_ret_ = 0.0, banked_ret_real_ = 0.0;
    int    clips_ = 0, wins_ = 0;

    struct Closed { double entry = 0, exit = 0, ret = 0, ret_real = 0;
                    int64_t ets = 0, xts = 0; std::string reason; };
    static constexpr size_t MAX_CLOSED_ = 60;
    std::deque<Closed> closed_;

    bool retired_() const noexcept {
        return cfg_.retire_usd < 0.0 && banked_ret_real_ * cfg_.notional <= cfg_.retire_usd;
    }

    // ── indicators on the PRIOR history (c_[0..N-2]) + today's close c_[N-1],
    //   exact parity with the audited python sim (STOCK_TURTLE_PERNAME notes) ──
    double sma_prior_(int n) const noexcept {   // mean of the n closes BEFORE today
        const int N = (int)c_.size();
        if (N - 1 < n) return 0.0;
        double s = 0.0; for (int k = N - 1 - n; k < N - 1; ++k) s += c_[k];
        return s / n;
    }
    double sma_incl_(int n) const noexcept {    // mean of the last n closes INCLUDING today
        const int N = (int)c_.size();
        if (N < n) return 0.0;
        double s = 0.0; for (int k = N - n; k < N; ++k) s += c_[k];
        return s / n;
    }
    double rsi_incl_(int n) const noexcept {    // Cutler RSI over the last n changes incl today
        const int N = (int)c_.size();           // (house ConnorsRSI2Engine::_rsi convention)
        if (N < n + 1) return 50.0;
        double g = 0, l = 0;
        for (int k = N - n; k < N; ++k) { const double ch = c_[k] - c_[k - 1]; if (ch > 0) g += ch; else l += -ch; }
        return l == 0 ? 100.0 : 100.0 - 100.0 / (1.0 + g / l);
    }
    double hi_prior_(int n) const noexcept {    // max of the n closes BEFORE today
        const int N = (int)c_.size();
        if (N - 1 < n) return 0.0;
        double h = c_[N - 1 - n]; for (int k = N - n; k < N - 1; ++k) h = std::max(h, c_[k]);
        return h;
    }
    double lo_prior_(int n) const noexcept {    // min of the n closes BEFORE today
        const int N = (int)c_.size();
        if (N - 1 < n) return 0.0;
        double l = c_[N - 1 - n]; for (int k = N - n; k < N - 1; ++k) l = std::min(l, c_[k]);
        return l;
    }

    void book_exit_(double fill, int64_t ts_sec, bool fwd, const char* reason) noexcept {
        const double r      = (pos_.epx > 0) ? (fill / pos_.epx - 1.0) : 0.0;
        const double r_real = r - cfg_.rt_cost_bp / 1e4;
        if (fwd) {
            if (!pos_.token.empty() && close_fn_) close_fn_(cfg_.live_sym, true, cfg_.lot, fill, pos_.token);
            if (ledger_fn_ && !g_sdt_catchup.load(std::memory_order_relaxed)) {
                // ledger size = SHARES (notional/entry) so the central-ledger USD
                // ((exit-entry)*size) equals the book's gross return*notional.
                const double sh = (pos_.epx > 0) ? cfg_.notional / pos_.epx : 0.0;
                ledger_fn_(engine_tag(), cfg_.live_sym, true, pos_.epx, fill, sh, pos_.entry_ts, ts_sec, reason);
            }
            banked_ret_ += r; banked_ret_real_ += r_real;
            clips_ += 1; wins_ += (r_real > 1e-9 ? 1 : 0);
            Closed rec{pos_.epx, fill, r, r_real, pos_.entry_ts, ts_sec, reason};
            closed_.push_back(rec);
            while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            append_closed_(rec);
            std::printf("[SDT][CLOSE] %s entry=%.2f fill=%.2f ret=%.4f ret_real=%.4f held=%d (%s)\n",
                        engine_tag().c_str(), pos_.epx, fill, r, r_real, pos_.held, reason);
            std::fflush(stdout);
        }
        pos_ = Pos();
    }

    // Daily state machine on the NEWEST close (sim order: in-position -> exit check
    // ONLY; else entry check. No same-day exit->re-entry).
    void live_step_(int64_t ts_sec) noexcept {
        if (!open_fn_) return;                    // backtest TU / not wired: no live logic
        const int N = (int)c_.size();
        if (N < 2) return;
        const bool   fwd = ts_sec > deploy_ts_;
        const double cur = c_[N - 1];
        // GAP GUARD: block NEW entries across a multi-week data outage; exits stay honoured.
        const bool contig = (ts_[N - 1] - ts_[N - 2]) <= (int64_t)86400 * 7;

        if (pos_.active) {
            pos_.held += 1;
            bool do_exit = false; const char* reason = "";
            if (cfg_.family == DIP) {
                const double s5 = sma_incl_(cfg_.dip_exit_sma);
                if (s5 > 0 && cur > s5)               { do_exit = true; reason = "SMA5_BOUNCE"; }
                else if (pos_.held >= cfg_.dip_max_hold) { do_exit = true; reason = "TIME_STOP"; }
            } else {
                const double lo = lo_prior_(cfg_.tur_lo_n);
                if (lo > 0 && cur < lo)               { do_exit = true; reason = "CH10_LOW"; }
            }
            if (do_exit) book_exit_(cur, ts_sec, fwd, reason);
        } else if (contig) {
            bool sig = false;
            if (cfg_.family == DIP) {
                const double s200 = sma_prior_(cfg_.dip_trend_sma);
                sig = (s200 > 0 && cur > s200 && rsi_incl_(cfg_.dip_rsi_len) < cfg_.dip_rsi_in);
            } else {
                const double hi = hi_prior_(cfg_.tur_hi_n);
                sig = (hi > 0 && cur > hi);
            }
            if (sig) {
                // AUTO-RETIREMENT gate: a proven-negative forward book stops entering.
                if (retired_()) {
                    if (!retired_logged_) {
                        retired_logged_ = true;
                        std::printf("[SDT][RETIRED] %s forward net_real $%.0f <= $%.0f -- no new entries (auto-retirement, S-2026-07-08c)\n",
                                    engine_tag().c_str(), banked_ret_real_ * cfg_.notional, cfg_.retire_usd);
                        std::fflush(stdout);
                    }
                } else {
                    // COST GATE: ExecutionCostGuard::is_viable via the wired gate_fn
                    // (US-equity row; lots = SHARES for equities, tp_dist ~ the class's
                    // typical favorable move ~2%); the 8bp RT debit in ret_real is the
                    // validated book-level gate on top.
                    const double shares  = (cur > 0) ? cfg_.notional / cur : 0.0;
                    const double tp_dist = cur * 0.02;
                    if (!gate_fn_ || gate_fn_(cfg_.live_sym, tp_dist, shares)) {
                        pos_.active = true; pos_.epx = cur; pos_.held = 0; pos_.entry_ts = ts_sec;
                        pos_.token.clear();
                        // S-2026-07-16l: one-way fire to the BE-mimic (LONG, at the entry
                        // close). Independent book, never touches this real position
                        // (feedback-companion-independent-engine). S-2026-07-16p: fire for
                        // whichever family has a cb installed -- DIP syms carry the DIP-cell
                        // fan (set_mimic_cbs), TURTLE syms carry the 4 TURTLE-cell fan
                        // (set_turtle_mimic_cbs); disjoint, family-routed by the setter.
                        if (mimic_open_cb_) mimic_open_cb_(cfg_.sym, +1, cur, ts_sec);
                        if (fwd && !g_sdt_catchup.load(std::memory_order_relaxed)) {
                            pos_.token = open_fn_(cfg_.live_sym, true, cfg_.lot, cur);
                            std::printf("[SDT][OPEN] %s LONG @%.2f lot=%.2f tok=%s\n",
                                        engine_tag().c_str(), cur, cfg_.lot, pos_.token.c_str());
                            std::fflush(stdout);
                        }
                    }
                }
            }
        }
        save_live_state_();
    }

    void append_closed_(const Closed& r) const noexcept {
        std::ofstream f(cfg_.closed_path, std::ios::app);
        if (!f.is_open()) return;
        f << r.entry << "," << r.exit << "," << r.ret << "," << r.ret_real << ","
          << (long long)r.ets << "," << (long long)r.xts << "," << r.reason << "\n";
    }
    void load_closed_() noexcept {   // banked net = SUM of the name's own closed csv
        std::ifstream f(cfg_.closed_path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            Closed r; char reason[32] = {0}; long long ets = 0, xts = 0;
            const int n = std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lld,%lld,%31[^,\n]",
                                      &r.entry, &r.exit, &r.ret, &r.ret_real, &ets, &xts, reason);
            if (n >= 7) {
                r.ets = (int64_t)ets; r.xts = (int64_t)xts; r.reason = reason;
                banked_ret_ += r.ret; banked_ret_real_ += r.ret_real;
                clips_ += 1; wins_ += (r.ret_real > 1e-9 ? 1 : 0);
                closed_.push_back(r);
                while (closed_.size() > MAX_CLOSED_) closed_.pop_front();
            }
        }
    }

    void load_live_state_() noexcept {
        std::ifstream f(cfg_.live_path);
        if (!f.is_open()) return;
        int act = 0; double epx = 0; int held = 0; long long ets = 0; std::string tok;
        if (f >> act >> epx >> held >> ets >> tok) {
            pos_.active = (act != 0); pos_.epx = epx; pos_.held = held;
            pos_.entry_ts = (int64_t)ets; pos_.token = (tok == "-") ? std::string() : tok;
        }
    }
    void save_live_state_() const noexcept {
        const std::string tmp = cfg_.live_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          f << (pos_.active ? 1 : 0) << " " << pos_.epx << " " << pos_.held << " "
            << (long long)pos_.entry_ts << " " << (pos_.token.empty() ? "-" : pos_.token) << "\n"; }
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

// ── registry: owns both families' books, seeds/polls the wide daily-close CSV,
//   writes the merged stockdipturtle_state.json. One name may appear in BOTH
//   families (e.g. NVDA dip + NVDA turtle) -> column maps to a LIST of books. ──
class StockDipTurtleBook {
public:
    ~StockDipTurtleBook() { stop_poller(); }

    void add(StockDipTurtleSym::Config c) {
        col_[c.sym].push_back(syms_.size());
        syms_.emplace_back(std::move(c));
    }

    StockDipTurtleSym* find(const std::string& sym, int family) {
        auto it = col_.find(sym);
        if (it == col_.end()) return nullptr;
        for (size_t i : it->second) if (syms_[i].family() == family) return &syms_[i];
        return nullptr;
    }

    void set_exec(StockDipTurtleSym::OpenFn o, StockDipTurtleSym::CloseFn c,
                  StockDipTurtleSym::GateFn g, StockDipTurtleSym::LedgerFn l) {
        for (auto& s : syms_) s.set_exec(o, c, g, l);
    }

    // warm-seed from the WIDE daily-close CSV (header ",NAME1,NAME2,...", rows
    // "YYYY-MM-DD,c1,c2,..."). BOOT CATCH-UP watermark semantics as documented
    // in the header: rows <= watermark seed; rows beyond replay through LIVE
    // logic with orders/ledger suppressed (g_sdt_catchup).
    size_t seed_from_wide_csv(const std::string& rel) noexcept {
        const std::string path = omega::resolve_seed_path(rel);
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[SDT][SEED] MISS %s\n", path.c_str()); std::fflush(stdout); return 0; }
        std::string header;
        if (!std::getline(f, header)) return 0;
        std::unordered_map<int, const std::vector<size_t>*> colsym;
        { std::stringstream hs(header); std::string tok; int ci = 0;
          while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = &it->second; ++ci; } }
        int64_t watermark = 0;
        { std::ifstream wf(lastseen_path_); long long v = 0; if (wf.is_open() && (wf >> v)) watermark = v; }
        std::string line; size_t rows = 0, caught_up = 0;
        if (watermark > 0) g_sdt_catchup.store(true, std::memory_order_relaxed);
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
        g_sdt_catchup.store(false, std::memory_order_relaxed);
        save_lastseen_(last_seed_ts_);
        std::printf("[SDT][SEED] wide-csv %s: %zu rows (%zu caught-up live, watermark=%lld), %zu cols -> %zu books, last=%lld\n",
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

    // LIVE: one closed daily bar for `sym` (both families). Poller uses the row variant.
    void on_daily_bar(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = col_.find(sym);
        if (it == col_.end()) return;
        for (size_t i : it->second) syms_[i].on_daily_bar(ts_sec, close);
        recompute_unlocked_();
    }

    void start_poller(const std::string& rel, int poll_ms = 900000 /*15min*/) {
        csv_rel_ = rel; poll_ms_ = poll_ms;
        last_seen_ts_ = last_seed_ts_;   // resume forward from the last seeded date
        seed_missed_ = (last_seed_ts_ == 0);   // boot seed found NO file -> first load SEEDS
        running_.store(true);
        thread_ = std::thread([this]{ poll_loop_(); });
        std::printf("[SDT][POLL] watching %s (poll=%dms, resume-from=%lld)\n",
                    rel.c_str(), poll_ms_, (long long)last_seen_ts_);
        std::fflush(stdout);
    }
    void stop_poller() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    std::string state_json() const { std::lock_guard<std::mutex> lk(mu_); return state_json_unlocked_(); }
    void recompute_and_write() const { std::lock_guard<std::mutex> lk(mu_); recompute_unlocked_(); }
    double total_usd_real() const {
        std::lock_guard<std::mutex> lk(mu_);
        double t = 0.0; for (const auto& s : syms_) t += s.book_usd_real(); return t;
    }
    size_t count(int family) const {
        size_t n = 0; for (const auto& s : syms_) if (s.family() == family) ++n; return n;
    }
    // S-2026-07-15: entry_ts of every currently-OPEN position across all names.
    // engine_init inserts these into g_restored_entry_ts after finalize_all() so
    // the central phantom-drop guard exempts genuine restored positions whose
    // close spans a reboot (see StockDipTurtleSym::open_entry_ts()).
    std::vector<int64_t> restored_open_entry_ts() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<int64_t> v;
        for (const auto& s : syms_) { const int64_t e = s.open_entry_ts(); if (e > 0) v.push_back(e); }
        return v;
    }

    // S-2026-07-16l: install the StockDip BE-mimic hooks on every DIP-family sym (StockTurtle
    // is not mimicked). engine_init sets these to fan-out lambdas that fire the per-name T + W
    // mimic cells in stockdip_trend_mimic(). Independent SHADOW book (never touches the real
    // trade). Call AFTER add()-ing all syms, once.
    void set_mimic_cbs(StockDipTurtleSym::MimicOpenCb o, StockDipTurtleSym::MimicBarCb b) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : syms_) if (s.is_dip()) s.set_mimic_cbs(o, b);
    }

    // S-2026-07-16p: install the TURTLE BE-mimic hooks on every TURTLE-family sym. engine_init
    // sets these to fan-out lambdas that fire the per-name 4 TURTLE mimic cells (A/B/C/D arm
    // ladder) in stockdip_trend_mimic(). Independent SHADOW book (never touches the real trade);
    // validated STANDALONE all-6 PASS ungated (TURTLE_MIMIC_FINDINGS_2026-07-16). Disjoint from
    // the DIP setter (is_dip vs !is_dip). Call AFTER add()-ing all syms, once.
    void set_turtle_mimic_cbs(StockDipTurtleSym::MimicOpenCb o, StockDipTurtleSym::MimicBarCb b) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : syms_) if (!s.is_dip()) s.set_mimic_cbs(o, b);
    }

private:
    mutable std::mutex mu_;   // poller thread dispatches bars while main thread reads state

    std::string state_json_unlocked_() const {
        std::ostringstream o;
        int64_t last_ts = 0; double tot = 0.0, tot_real = 0.0;
        for (const auto& s : syms_) { last_ts = std::max(last_ts, s.last_ts()); tot += s.book_usd(); tot_real += s.book_usd_real(); }
        o << "{\"engine\":\"stock-dip-turtle\",\"shadow\":true,\"grade\":\"daily-close\",";
        o.precision(0); o << std::fixed << "\"total_usd\":" << tot
                          << ",\"total_usd_real\":" << tot_real << ",\"books\":[";
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

    std::vector<StockDipTurtleSym> syms_;
    std::unordered_map<std::string, std::vector<size_t>> col_;   // name -> book indices (both families)
    std::string state_path_ = "stockdipturtle_state.json";
    int64_t last_seed_ts_ = 0;
    std::string lastseen_path_ = "stockdipturtle_lastseen.txt";  // live watermark (cwd C:\Omega)

    // poller
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

    void dispatch_row_(const std::string& line,
                       const std::unordered_map<int, const std::vector<size_t>*>& colsym,
                       int64_t ts, bool live) {
        std::stringstream ls(line); std::string tok; int ci = 0;
        while (std::getline(ls, tok, ',')) {
            auto it = colsym.find(ci);
            if (it != colsym.end() && !tok.empty()) {
                char* end = nullptr; const double v = std::strtod(tok.c_str(), &end);
                if (end != tok.c_str() && v > 0.0) {
                    for (size_t si : *it->second) {
                        if (live) syms_[si].on_daily_bar(ts, v);
                        else      syms_[si].seed_bar(ts, v);
                    }
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
            std::unordered_map<int, const std::vector<size_t>*> colsym;
            { std::stringstream hs(header); std::string tok; int ci = 0;
              while (std::getline(hs, tok, ',')) { auto it = col_.find(tok); if (it != col_.end()) colsym[ci] = &it->second; ++ci; } }
            const int64_t seen = last_seen_ts_.load();
            const bool as_seed = seed_missed_;   // first load after a missed boot-seed = SEED, not live
            int64_t newest = seen; int fresh = 0;
            std::string line;
            {
                std::lock_guard<std::mutex> lk(mu_);
                while (std::getline(f, line)) {
                    if (line.empty()) continue;
                    const int64_t ts = parse_date_ts_(line);
                    if (!as_seed && ts <= seen) continue;     // live: already processed
                    dispatch_row_(line, colsym, ts, /*live=*/!as_seed);
                    if (ts > newest) newest = ts;
                    ++fresh;
                }
                if (fresh > 0) {
                    if (as_seed) { for (auto& s : syms_) s.finalize_seed(); seed_missed_ = false; }
                    last_seen_ts_.store(newest);
                    save_lastseen_(newest);      // advance the watermark
                    recompute_unlocked_();
                }
            }
            if (fresh > 0) {
                std::printf("[SDT][POLL] %s%d daily row(s), newest=%lld\n",
                            as_seed ? "first-load SEED " : "", fresh, (long long)newest);
                std::fflush(stdout);
            }
        }
    }
};

// Singleton — accessor mirrors omega::stockmover_ladder_book().
inline StockDipTurtleBook& stock_dipturtle_book() noexcept {
    static StockDipTurtleBook inst;
    return inst;
}

} // namespace omega
