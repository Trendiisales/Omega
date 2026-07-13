#pragma once
// =============================================================================
// GoldTrendMimicLadder — INDEPENDENT shadow mimic ladders triggered by gold
// trend engines (operator S-2026-07-09).
//
// When a wired gold trend engine OPENS a position it fires a ONE-WAY notify
// omega::gold_trend_mimic().on_trend_open(tag, dir, entry_px, ts). This engine
// spawns its OWN independent legs at that entry and manages them entirely on the
// XAUUSD H1 bar stream — arm at a small MFE, per-leg PEAK-PROFIT giveback trail
// (tight vs wide), pre-arm LOSS_CUT, BE-floor, and an INDEPENDENT WINDOW-CAP
// flush (N bars). It NEVER reads, moves, shrinks or closes the trend engine's
// position and is never read back by it — additive, judged STANDALONE
// (feedback-companion-independent-engine). Books are SHADOW until their live flip;
// first live book: XAU_4h_DonchN20 (S-2026-07-14, resting-exec + H1-SMA200 gate,
// 1 MGC -- see the engine_init block + XAU_DONCH20_RESTING_REVALIDATION findings).
//
// VALIDATED (backtest/clip_path_*.cpp real-engine entries + independent window
// exit, cost-debited, standalone):
//   XauTF4h   4 legs (T gb8/10 + W gb20/25) arm0.25/lc1.5 cap12 : +110%/leg,
//             66% win, WF both halves + (H1+32/H2+78), bull+91/bear+20.
//   MgcFastDon30m 2 legs (T gb8 + W gb20) arm0.15/lc1.0 cap24  : +34/+35,
//             69% win, WF + , bull+26/bear+8.
//   XauTF-D1  2 legs (T gb8 + W gb20) arm0.25/lc2.0 cap8       : +24/+22,
//             70% win, WF + , bull+10/bear+14.
//   (close-grade first-pass; intrabar re-check owed before LIVE sizing.)
//
// ADVERSE-PROTECTION: per-leg backtested verdict — pre-arm LOSS_CUT (lc_pct of
//   entry) + post-arm peak-profit trail (keep 1-gb of the peak) + BE-floor
//   (armed leg stop >= entry) + window-cap flush. The PASS figures above are net
//   of the loss-cut. Books in RETURN units (USD = ret * notional/clip).
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <sstream>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace omega {

class GoldTrendMimicBook {
public:
    struct LegCfg { const char* tag; double gb; };   // tag suffix + giveback fraction of peak
    struct Config {
        std::string trigger_tag = "XauTf4h";  // the trend engine that fires on_trend_open
        std::string live_sym    = "XAUUSD";    // symbol the legs trade (order path)
        std::vector<LegCfg> legs;              // e.g. {{"T1",0.08},{"T2",0.10},{"W1",0.20},{"W2",0.25}}
        double arm_pct     = 0.25;   // % MFE that ARMS a leg (then the peak-profit trail governs)
        double lc_pct      = 1.5;    // DRAWDOWN-CANCEL: pre-arm LOSS_CUT, cut a leg at -lc_pct% before it
                                     //   arms (mimic never touches real trade -> free). Backtested: PASS
                                     //   figures in header are NET of this cut. Fires L152 (book_clip LOSS_CUT).
        // BE-ENTRY (operator S-2026-07-09b): a leg does NOT open at the trigger. It stays PENDING
        // until price clears +be_entry_pct in favor (costs covered / break-even made), then enters
        // THERE. A move that fades before covering cost never opens a leg -> no open-into-loss.
        // 0 = legacy enter-at-trigger. Wired to ~the round-trip cost so entry = the first bar the
        // trade is genuinely in the black. Backtest: +2-3% win-rate, zero faded-breakout reds, at
        // ~8-10% lower net (skips fades that would have recovered + gives up the first be of a move).
        double be_entry_pct = 0.0;   // >0 = wait for +be% before entering; 0 = enter at trigger
        int    pend_bars   = 6;      // cancel a PENDING leg if BE not made within this many bars
        int    cap_bars    = 12;     // INDEPENDENT window: flush an ENTERED leg after this many bars
        double rt_cost_bp  = 15.0;   // real round-trip cost (bp of entry) debited per clip
        double notional    = 10000.0;// $ per clip; USD = ret * notional
        double lot         = 0.01;   // order-path lot (decided at LIVE flip)
        bool   bull_only   = false;  // if true, only arm when the H1 close is above SMA200 (bull gate)
        // RESTING-EXEC (S-2026-07-14, operator wire order after XAU_DONCH20_RESTING_REVALIDATION):
        // market-at-close exec FAILS WF-H1 on the survivor cells; the decision-grade PASS model is
        // resting orders at the BE level with fine-grain trail. true = the book is driven by the
        // LIVE TICK STREAM (registry on_tick): a PENDING leg enters the moment price crosses the
        // BE level (order fired AT the cross, entry booked at the ACTUAL crossing price, not the
        // level -- no level-fill overstatement), and lc/trail exits fire the moment the synthetic
        // stop is crossed intra-bar. Native bars keep ONLY pend-cancel + window-cap + gate upkeep.
        // false = legacy close-grade management (all other books unchanged).
        bool   resting_exec = false;
        // REGIME GATE (operator proviso on the live flip): bull_only=true books receive the
        // registry-level XAU H1 SMA200 regime flag (xau_regime_h1_close feed, seeded from
        // warmup_XAUUSD_H1.csv at boot). Validated G-H1-SMA200 (bear-gate study, findings doc):
        // +13.9%/leg PF2.79 DD-2.6, calendar-2022 -1.9 -> +0.3, 2x-cost PF2.22, slip-robust.
        // Gate is trigger-time only (bear trigger spawns nothing); pending/open legs are NOT
        // cancelled on a regime flip (tested: delta -0.3, irrelevant). Fail-open until warm.
        bool   live_book    = false; // true after the operator live-flip: json shadow:false
        std::string state_path;      // per-book persisted OPEN legs + forward book
        std::string closed_path;     // per-book persisted CLOSED clips log
    };

    // LIVE EXECUTION WIRING — identical contract to the ladder companions. Null in a
    // backtest TU -> pure accounting. SHADOW today (send_live_order no-op until mode=LIVE).
    using OpenFn   = std::function<std::string(const std::string&, bool, double, double)>;
    using CloseFn  = std::function<void(const std::string&, bool, double, double, const std::string&)>;
    using GateFn   = std::function<bool(const std::string&, double, double)>;
    using LedgerFn = std::function<void(const std::string&, const std::string&, bool,
                                        double, double, double, int64_t, int64_t, const char*)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) {
        open_fn_ = std::move(o); close_fn_ = std::move(c); gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    explicit GoldTrendMimicBook(Config c) : cfg_(std::move(c)) {
        const std::string s = lower_(cfg_.trigger_tag);
        if (cfg_.state_path.empty())  cfg_.state_path  = "goldmimic_" + s + "_state.txt";
        if (cfg_.closed_path.empty()) cfg_.closed_path = "goldmimic_" + s + "_closed.csv";
        load_book_();
        load_open_();
    }

    const std::string& tag() const { return cfg_.trigger_tag; }
    const std::string& live_sym() const { return cfg_.live_sym; }
    bool wants_regime() const { return cfg_.bull_only; }
    void set_bull(bool b) { bull_ = b; ext_regime_ = true; }
    double book_usd_real() const { double r = 0; for (auto& b : books_) r += b.ret_real; return r * cfg_.notional; }

    // ── the ONE-WAY trigger: the trend engine opened -> spawn our own independent legs.
    void on_trend_open(int dir, double entry_px, int64_t ts_sec) noexcept {
        if (entry_px <= 0.0 || dir == 0) return;
        if (cfg_.bull_only && !bull_) return;   // bull-gate (SMA200) — quiet by policy, not fault
        const bool be = (cfg_.be_entry_pct > 0.0);   // BE-ENTRY: legs stay pending until cost covered
        for (size_t i = 0; i < cfg_.legs.size(); ++i) {
            Leg L; L.dir = dir; L.li = (int)i; L.entry_ts = ts_sec; L.peak = 0.0; L.armed = false;
            L.bars = 0; L.trig = entry_px; L.open = true;
            if (be) { L.pending = true; L.pbars = 0; }   // wait for +be_entry_pct before opening any order
            else {                                        // legacy: enter at the trigger close now
                L.pending = false; L.entry = entry_px;
                if (open_fn_ && gate_fn_) {
                    const double tp = entry_px * (cfg_.arm_pct / 100.0);
                    if (gate_fn_(cfg_.live_sym, tp, cfg_.lot)) L.token = open_fn_(cfg_.live_sym, dir > 0, cfg_.lot, entry_px);
                }
            }
            legs_.push_back(std::move(L));
        }
        std::printf("[GMIMIC][%s] %s %zu legs %s @%.2f%s\n", cfg_.trigger_tag.c_str(),
                    be ? "PENDING" : "spawn", cfg_.legs.size(), dir > 0 ? "LONG" : "SHORT", entry_px,
                    be ? " (waits for BE)" : "");
        std::fflush(stdout);
        save_open_();
    }

    // ── RESTING-EXEC tick path: fires the moment a level is crossed (registry-routed,
    //    armed books only). Entries/exits booked at the ACTUAL tick price -- the live
    //    analog of the validated REST-F/M1 model (level fills overstate 3-4x; real
    //    crossing fills survive the 1-2bp penetration + slip stress).
    void on_tick(double px, int64_t ts_sec) noexcept {
        if (!cfg_.resting_exec || px <= 0.0 || legs_.empty()) return;
        bool changed = false;
        std::vector<Leg> still; still.reserve(legs_.size());
        for (Leg& L : legs_) {
            if (!L.open) continue;
            if (L.pending) {   // synthetic resting STOP at the BE level: enter AT the cross
                const double lvl = L.trig * (1.0 + L.dir * cfg_.be_entry_pct / 100.0);
                if ((L.dir > 0 && px >= lvl) || (L.dir < 0 && px <= lvl)) {
                    L.entry = px; L.pending = false; L.bars = 0; L.peak = 0.0;
                    L.armed = false; L.entry_ts = ts_sec;
                    if (open_fn_ && gate_fn_) { const double tp = L.entry * (cfg_.arm_pct / 100.0);
                        if (gate_fn_(cfg_.live_sym, tp, cfg_.lot))
                            L.token = open_fn_(cfg_.live_sym, L.dir > 0, cfg_.lot, L.entry); }
                    std::printf("[GMIMIC][%s] BE-ENTER(tick) %s @%.2f (level %.2f trig %.2f)\n",
                                cfg_.trigger_tag.c_str(), leg_engine_(L.li).c_str(), L.entry, lvl, L.trig);
                    std::fflush(stdout);
                    changed = true;
                }
                still.push_back(std::move(L)); continue;
            }
            const double ret = L.dir * (px / L.entry - 1.0) * 100.0;
            if (ret > L.peak) L.peak = ret;
            const double gb = cfg_.legs[(L.li >= 0 && L.li < (int)cfg_.legs.size()) ? L.li : 0].gb;
            bool closed = false;
            if (!L.armed) {
                if (ret <= -cfg_.lc_pct) { book_clip_(L, ret, ts_sec, "LOSS_CUT"); closed = true; changed = true; }
                else if (L.peak >= cfg_.arm_pct) L.armed = true;
            } else {
                const double stop_ret = (1.0 - gb) * L.peak;   // keep (1-gb) of peak, BE-floored
                if (ret <= stop_ret) { book_clip_(L, ret, ts_sec, "TRAIL_STOP"); closed = true; changed = true; }
            }
            if (!closed) still.push_back(std::move(L));
        }
        legs_.swap(still);
        if (changed) save_open_();
    }

    // ── manage every open leg on the newest NATIVE bar (intrabar l->h->c, SL-first).
    //    manage=false (registry pre-arm / seed replay) = gate/SMA upkeep ONLY -- persisted
    //    legs are never counted, capped or clipped on replayed historical bars.
    void on_h1_bar(double h, double l, double c, int64_t ts_sec, bool bull, bool manage = true) noexcept {
        if (!ext_regime_) bull_ = bull;   // external XAU H1 regime feed (set_bull) wins once live
        if (!manage || legs_.empty()) return;
        std::vector<Leg> still; still.reserve(legs_.size());
        for (Leg& L : legs_) {
            if (!L.open) continue;
            // ── BE-ENTRY gate: a PENDING leg opens only once price covers cost (+be_entry_pct
            //    off the trigger). If BE is never made within pend_bars -> CANCEL (no book, no
            //    open-into-loss). This is the operator's "only trade once BE has been made". ──
            if (L.pending) {
                L.pbars += 1;
                const double fav  = L.dir > 0 ? h : l;                 // favorable extreme this bar
                const double fret = L.dir * (fav / L.trig - 1.0) * 100.0;
                if (fret >= cfg_.be_entry_pct) {                       // BE made -> ENTER at the BE level
                    L.entry = L.trig * (1.0 + L.dir * cfg_.be_entry_pct / 100.0);
                    L.pending = false; L.bars = 0; L.peak = 0.0; L.armed = false; L.entry_ts = ts_sec;
                    if (open_fn_ && gate_fn_) { const double tp = L.entry * (cfg_.arm_pct / 100.0);
                        if (gate_fn_(cfg_.live_sym, tp, cfg_.lot)) L.token = open_fn_(cfg_.live_sym, L.dir > 0, cfg_.lot, L.entry); }
                    std::printf("[GMIMIC][%s] BE-ENTER %s @%.2f (trig %.2f)\n",
                                cfg_.trigger_tag.c_str(), leg_engine_(L.li).c_str(), L.entry, L.trig);
                    std::fflush(stdout);
                    still.push_back(std::move(L)); continue;          // manage from the NEXT bar
                }
                if (L.pbars >= cfg_.pend_bars) { L.open = false; continue; }   // BE never made -> cancel
                still.push_back(std::move(L)); continue;              // still pending, no position yet
            }
            L.bars += 1;
            const double gb = cfg_.legs[(L.li >= 0 && L.li < (int)cfg_.legs.size()) ? L.li : 0].gb;
            const double seq[3] = { L.dir > 0 ? l : h, L.dir > 0 ? h : l, c };   // adverse extreme first
            bool closed = false;
            for (int k = 0; k < 3 && !closed; ++k) {
                const double ret = L.dir * (seq[k] / L.entry - 1.0) * 100.0;   // % return at this extreme
                if (ret > L.peak) L.peak = ret;
                if (!L.armed) {
                    if (ret <= -cfg_.lc_pct) { book_clip_(L, -cfg_.lc_pct, ts_sec, "LOSS_CUT"); closed = true; break; }
                    if (L.peak >= cfg_.arm_pct) L.armed = true;
                } else {
                    const double stop_ret = (1.0 - gb) * L.peak;   // keep (1-gb) of peak -> BE-floored (>=0)
                    if (ret <= stop_ret) { book_clip_(L, stop_ret, ts_sec, "TRAIL_STOP"); closed = true; break; }
                }
            }
            if (!closed && L.bars >= cfg_.cap_bars) {   // INDEPENDENT window flush at the close
                const double ret = L.dir * (c / L.entry - 1.0) * 100.0;
                book_clip_(L, ret, ts_sec, "WINDOW_CAP"); closed = true;
            }
            if (!closed) still.push_back(std::move(L));
        }
        legs_.swap(still);
        save_open_();
    }

    // desk JSON: REAL forward clips only ($0 until first live clip).
    std::string json() const {
        std::ostringstream o; o << std::fixed;
        double ret_real = 0; int clips = 0, wins = 0;
        for (size_t i = 0; i < books_.size(); ++i) { ret_real += books_[i].ret_real;
            clips += books_[i].clips; wins += books_[i].wins; }
        o << "{\"tag\":\"" << cfg_.trigger_tag << "\",\"sym\":\"" << cfg_.live_sym
          << "\",\"shadow\":" << (cfg_.live_book ? "false" : "true") << ",";
        o.precision(0); o << "\"notional\":" << cfg_.notional << ",\"open\":" << open_count_() << ",";
        o << "\"clips\":" << clips << ",\"wins\":" << wins << ",";
        o.precision(3); o << "\"pct_real\":" << (ret_real * 100.0) << ",";
        o.precision(0); o << "\"usd_real\":" << (ret_real * cfg_.notional) << ",\"legs\":[";
        for (size_t i = 0; i < books_.size(); ++i) { if (i) o << ",";
            o << "{\"leg\":\"" << cfg_.legs[i].tag << "\",\"gb\":"; o.precision(2); o << cfg_.legs[i].gb;
            o.precision(0); o << ",\"clips\":" << books_[i].clips << ",\"wins\":" << books_[i].wins;
            o.precision(0); o << ",\"usd_real\":" << (books_[i].ret_real * cfg_.notional) << "}"; }
        o << "]}";
        return o.str();
    }

private:
    Config cfg_;
    bool   bull_ = true;
    bool   ext_regime_ = false;   // true once the registry regime feed has spoken
    struct Leg { int dir = 0, li = 0, bars = 0, pbars = 0; double entry = 0, peak = 0, trig = 0;
                 bool armed = false, open = false, pending = false;
                 int64_t entry_ts = 0; std::string token; };
    std::vector<Leg> legs_;
    struct Book { double ret = 0, ret_real = 0; int clips = 0, wins = 0; };
    std::vector<Book> books_;   // one per leg config index
    struct Closed { int li; double entry, exit, ret_real; int64_t ets, xts; std::string reason; };

    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    int open_count_() const { int n = 0; for (auto& L : legs_) if (L.open) ++n; return n; }
    std::string leg_engine_(int li) const {
        return cfg_.trigger_tag + "Mimic" + (li >= 0 && li < (int)cfg_.legs.size() ? cfg_.legs[li].tag : "?");
    }

    // book one clip at a RETURN level (fill = entry*(1 + dir*ret/100)); real = ret - cost.
    void book_clip_(Leg& L, double ret_pct, int64_t ts_sec, const char* reason) noexcept {
        const double fill = L.entry * (1.0 + L.dir * ret_pct / 100.0);
        const double r      = ret_pct / 100.0;
        const double r_real = r - cfg_.rt_cost_bp / 1e4;
        if (L.li >= 0 && L.li < (int)books_.size()) {
            books_[L.li].ret += r; books_[L.li].ret_real += r_real; books_[L.li].clips += 1;
            books_[L.li].wins += (r_real > 1e-9 ? 1 : 0);
        }
        if (!L.token.empty() && close_fn_) close_fn_(cfg_.live_sym, L.dir > 0, cfg_.lot, fill, L.token);
        if (ledger_fn_) ledger_fn_(leg_engine_(L.li), cfg_.live_sym, L.dir > 0, L.entry, fill, cfg_.lot,
                                   L.entry_ts, ts_sec, reason);
        L.open = false; L.token.clear();
        append_closed_(Closed{L.li, L.entry, fill, r_real, L.entry_ts, ts_sec, reason});
        save_book_();
        std::printf("[GMIMIC][%s] CLIP %s entry=%.2f fill=%.2f ret_real=%.4f (%s)\n",
                    cfg_.trigger_tag.c_str(), leg_engine_(L.li).c_str(), L.entry, fill, r_real, reason);
        std::fflush(stdout);
    }

    void ensure_books_() { if ((int)books_.size() < (int)cfg_.legs.size()) books_.resize(cfg_.legs.size()); }
    void load_book_() { ensure_books_();
        std::ifstream f(cfg_.state_path); if (!f.is_open()) return;
        std::string kind;
        while (f >> kind) { if (kind == "book") { int li; double rt, rr; int cl, wn;
            if (f >> li >> rt >> rr >> cl >> wn && li >= 0 && li < (int)books_.size())
                books_[li] = Book{rt, rr, cl, wn}; }
            else { std::string rest; std::getline(f, rest); } }
    }
    void save_book_() const {
        const std::string tmp = cfg_.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (size_t i = 0; i < books_.size(); ++i)
              f << "book " << i << " " << books_[i].ret << " " << books_[i].ret_real << " "
                << books_[i].clips << " " << books_[i].wins << "\n"; }
        std::rename(tmp.c_str(), cfg_.state_path.c_str());
    }
    void load_open_() {  // open legs persisted alongside the book (survive restart)
        std::ifstream f(cfg_.state_path + ".open"); if (!f.is_open()) return;
        std::string kind;
        while (f >> kind) { if (kind == "leg") { Leg L; int op, ar, pend; long long ets; std::string tok;
            if (f >> L.dir >> L.li >> L.bars >> L.entry >> L.peak >> ar >> op >> ets >> pend >> L.trig >> L.pbars >> tok) {
                L.armed = ar; L.open = op; L.pending = pend; L.entry_ts = ets; L.token = (tok == "-") ? "" : tok;
                if (L.open) legs_.push_back(std::move(L)); } } }
    }
    void save_open_() const {
        const std::string p = cfg_.state_path + ".open", tmp = p + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (const Leg& L : legs_) if (L.open)
              f << "leg " << L.dir << " " << L.li << " " << L.bars << " " << L.entry << " " << L.peak
                << " " << (L.armed ? 1 : 0) << " " << (L.open ? 1 : 0) << " " << (long long)L.entry_ts
                << " " << (L.pending ? 1 : 0) << " " << L.trig << " " << L.pbars
                << " " << (L.token.empty() ? "-" : L.token) << "\n"; }
        std::rename(tmp.c_str(), p.c_str());
    }
    void append_closed_(const Closed& c) const {
        std::ofstream f(cfg_.closed_path, std::ios::app); if (!f.is_open()) return;
        f << c.li << "," << c.entry << "," << c.exit << "," << c.ret_real << ","
          << (long long)c.ets << "," << (long long)c.xts << "," << c.reason << "\n";
    }
    static std::string lower_(std::string s) { for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch); return s; }
};

// ── registry: owns the per-trend-engine books, routes on_trend_open + the XAU H1 feed. ──
class GoldTrendMimicRegistry {
public:
    void add(GoldTrendMimicBook::Config c) {
        std::lock_guard<std::mutex> lk(mu_);
        idx_[c.trigger_tag] = books_.size();
        books_.emplace_back(std::move(c));
    }
    void set_exec(GoldTrendMimicBook::OpenFn o, GoldTrendMimicBook::CloseFn c,
                  GoldTrendMimicBook::GateFn g, GoldTrendMimicBook::LedgerFn l) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& b : books_) b.set_exec(o, c, g, l);
    }
    // DEPLOY-FORWARD arm: engine_init calls arm() AFTER the trend engines finish warm-seeding,
    // so any historical open replayed during seed (belt-and-braces; seed already suppresses
    // entries via enabled=false) can NOT spawn phantom legs. Live opens (post-arm) spawn.
    void arm() { std::lock_guard<std::mutex> lk(mu_); armed_ = true; }

    // fired by a trend engine's OPEN path (one-way; unknown tag / pre-arm = no-op).
    void on_trend_open(const std::string& tag, int dir, double px, int64_t ts_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!armed_) return;
        auto it = idx_.find(tag); if (it == idx_.end()) return;
        books_[it->second].on_trend_open(dir, px, ts_sec);
    }
    // SPECIFIC FEED: the trigger engine feeds its OWN book on its NATIVE bar (turtle=D1,
    // XauTF=H4, MgcFast=M30) so leg management matches the cadence it was backtested on --
    // NOT a shared H1 stream (which would clip on intraday noise + mis-time the window cap).
    // Keyed by tag: each engine calls on_bar(its_tag, h,l,c,ts) once per native bar.
    void on_bar(const std::string& tag, double h, double l, double c, int64_t ts_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = idx_.find(tag); if (it == idx_.end()) return;
        // manage=armed_: pre-arm (warm-seed replay) bars feed ONLY the regime-gate SMA --
        // persisted legs are never counted/capped/clipped on replayed historical bars.
        books_[it->second].on_h1_bar(h, l, c, ts_sec, true, armed_);
    }
    // RESTING-EXEC tick route (S-2026-07-14): the trigger engine's tick driver calls this
    // per tick; no-op for unknown tags, non-resting books, empty books, or pre-arm.
    void on_tick(const std::string& tag, double px, int64_t ts_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!armed_) return;
        auto it = idx_.find(tag); if (it == idx_.end()) return;
        books_[it->second].on_tick(px, ts_sec);
    }
    // XAU H1 REGIME GATE feed (S-2026-07-14 bear-gate study, XAU_DONCH20_RESTING_REVALIDATION
    // BEAR-GATE section): SMA200 of XAU H1 closes; bull_only books refuse triggers while
    // close < SMA. Called from the tick_gold H1-close path; seeded at boot from
    // warmup_XAUUSD_H1.csv so the gate is warm from the first live bar (fail-open until warm).
    void xau_regime_h1_close(double c) {
        std::lock_guard<std::mutex> lk(mu_);
        regime_close_(c);
    }
    void seed_xau_regime_h1_csv(const std::string& path) {
        std::lock_guard<std::mutex> lk(mu_);
        std::ifstream f(path);
        if (!f.is_open()) {
            std::printf("[OMEGA-INIT][SEED] GoldTrendMimic XAU-H1 regime gate: CSV MISSING %s "
                        "(gate fail-open until 200 live H1 bars)\n", path.c_str());
            std::fflush(stdout); return;
        }
        std::string line; int n = 0;
        while (std::getline(f, line)) {           // H1 CSV convention: ts,o,h,l,c
            const size_t p = line.rfind(',');
            if (p == std::string::npos) continue;
            const double c = std::atof(line.c_str() + p + 1);
            if (c > 0.0) { regime_close_(c); ++n; }
        }
        std::printf("[OMEGA-INIT][SEED] GoldTrendMimic XAU-H1 regime gate seeded: %d bars, "
                    "SMA200 %s -> %s\n", n, (int)reg_closes_.size() >= 200 ? "WARM" : "COLD",
                    reg_bull_ ? "BULL" : "BEAR");
        std::fflush(stdout);
    }
    std::string state_json() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream o; o << "{\"engine\":\"gold-trend-mimic\",\"shadow\":true,\"books\":[";
        for (size_t i = 0; i < books_.size(); ++i) { if (i) o << ","; o << books_[i].json(); }
        o << "]}"; return o.str();
    }
    double total_usd_real() const {
        std::lock_guard<std::mutex> lk(mu_);
        double t = 0; for (auto& b : books_) t += b.book_usd_real(); return t;
    }
private:
    mutable std::mutex mu_;
    bool armed_ = false;
    std::vector<GoldTrendMimicBook> books_;
    std::unordered_map<std::string, size_t> idx_;
    std::deque<double> reg_closes_;   // XAU H1 closes for the SMA200 regime gate
    double reg_sum_  = 0.0;
    bool   reg_bull_ = true;          // fail-open until 200 bars seen
    void regime_close_(double c) {    // callers hold mu_
        reg_closes_.push_back(c); reg_sum_ += c;
        if ((int)reg_closes_.size() > 200) { reg_sum_ -= reg_closes_.front(); reg_closes_.pop_front(); }
        reg_bull_ = ((int)reg_closes_.size() < 200) ? true : (c >= reg_sum_ / 200.0);
        for (auto& b : books_) if (b.wants_regime()) b.set_bull(reg_bull_);
    }
};

inline GoldTrendMimicRegistry& gold_trend_mimic() noexcept {
    static GoldTrendMimicRegistry inst;
    return inst;
}

} // namespace omega
