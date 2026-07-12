#pragma once
//  ADVERSE-PROTECTION: per-leg 50%-of-peak giveback clip (g50 rev-only, armed from tier arm%)
//  + whole-book reversal exit (opposite W-window move >= thr closes parent + every leg) --
//  backtested S-2026-07-12b/c: indices backtest/XS_BECASCADE_GOLD_INDEX_FINDINGS.md (all 48
//  cells PASS OOS-2023 gate, 2x-cost robust, random-entry control z 2.3-3.2); gold bracket
//  backtest/xau_bracket_becascade_bt.cpp (bear years 2013/2015 +39/+29 vs long-only -18/-19;
//  2022 chop ~flat). Intraday instances (M5/M10/M15, S-2026-07-12d) ADD the gold_regime
//  long_blocked entry gate (backtested: 2022 bear -12..-15% -> flat, bull PF 1.5, 2x+).
//  No cold-loss cut by design (crypto BE-cascade precedent: tiers arm from +0.2%, worst
//  path = reversal flush; tightening lowers net per the 2026-06-17 sweep).
// =============================================================================
// BeCascadeEngines.hpp -- the crypto up-jump BE-CASCADE mimic mechanism ported
// to Omega instruments (S-2026-07-12b/c). Two engines:
//
//  1. XsBeCascadeEngine (DAILY, LONG-ONLY) -- indices USTEC/US500/DJ30.
//     Parent: enter when close/close[W=10d]-1 >= thr; ride to reversal (j <= -thr).
//     Mimic cascade: parent covering BE (+be_bp on a daily close) spawns mimic 1;
//     each mimic covering ITS BE spawns the next (arms {0.2,2,3,4,6,8}%).
//     Per-leg g50 giveback clip once mfe >= arm. ALL legs exit on reversal.
//     Faithful source: Crypto/backtest/upjump_earlyarm_bt.cpp xsgrid (drove the
//     REAL UpJumpLadderCompanion header; this port re-implements because that
//     header is crypto-repo + long-only-tick-shaped, mechanism identical).
//
//  2. XauBracketCascadeEngine (H1, TWO-SIDED) -- gold. Operator spec 2026-07-12:
//     movement trigger |close/close[W=240h]-1| >= thr -> OCO BRACKET (buy-stop
//     +b / sell-stop -b); first tick through a level activates that side and
//     CANCELS the other; activated side = parent, rides to opposite reversal;
//     same BE-cascade mimic stack on the activated direction (short legs too).
//     WHY two-sided: long-only gold BE-cascade = bull beta (random-control z=1.1);
//     the bracket's short side turns 2013/2015 bear bleed into profit.
//
// Signals + cascade management evaluate on FINALIZED bar closes (the backtested
// convention); executions use the live mid at detection (next-open equivalent).
// Bracket fills are tick-level (real stop semantics, >= backtest fidelity).
//
// WARM-SEED SAFE: seed_from_csv fills the close window only (no entries -- entry
// paths run solely in on_tick with `enabled` checked). REBASE-ON-FIRST-TICK:
// seeded closes are scaled so the last seeded close == first live mid, killing
// the venue/level offset (cash-index seed vs .F CFD feed; MGC seed vs XAUUSD
// spot) that would otherwise fake a jump through the W-window. Seed age is
// printed loudly; the gold H1 seed is older (MGC to 2026-06-03) -- the book is
// honest after 240 live H1 bars (~2 weeks), SHADOW absorbs that.
// =============================================================================
#include <string>
#include <deque>
#include <vector>
#include <functional>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include "OmegaTradeLedger.hpp"
#include "OmegaCostGuard.hpp"
#include "OpenPositionRegistry.hpp"   // omega::PositionSnapshot (persistence shape)

namespace omega {

// ── shared cascade book (one direction at a time) ────────────────────────────
struct BeCascadeBook {
    struct Leg {
        bool   parent = false;
        double entry = 0.0, mfe_pct = 0.0, arm_pct = 0.0;
        bool   open = true;
        int64_t entry_ts = 0;
    };
    bool   active = false;
    int    dir = +1;                    // +1 long, -1 short
    std::vector<Leg> legs;              // legs[0] = parent
    size_t next_arm = 0;                // next mimic tier to spawn
    double par_mfe_bp = 0.0;

    void reset() { active = false; legs.clear(); next_arm = 0; par_mfe_bp = 0.0; }
};

// ── 1. XsBeCascadeEngine: DAILY long-only (indices) ──────────────────────────
struct XsBeCascadeEngine {
    bool shadow_mode = true;
    bool enabled     = false;

    int    W       = 10;      // detect + reversal window, trading days
    double thr     = 0.02;    // jump / reversal threshold
    double be_bp   = 20.0;    // BE coverage that spawns the next mimic
    double gb      = 0.50;    // per-leg giveback of peak
    double loss_cut_bp = 0.0; // HARD REVERSAL STOP (S-2026-07-13, operator): cut ANY open leg at
                               // loss_cut_bp below its entry, PER-TICK — same fix proven on the crypto
                               // book (worst clip -1800->-70bp, net preserved, PF 2.5->4.5). Long-only:
                               // below entry the up-move is over. 0 = off.
    std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
    double lot     = 1.0;

    std::string symbol      = "USTEC.F";
    std::string engine_name = "XsBeCascade";
    std::string tag         = "XSBEC";
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // daily aggregation
    int64_t cur_day_ = -1; double day_close_ = 0.0;
    std::deque<double> closes_;         // finalized daily closes (keep W+1)
    bool rebased_ = false;

    BeCascadeBook book_;
    bool has_open_position() const noexcept { return book_.active; }

    static int64_t utc_day(int64_t ms) noexcept { return (ms / 1000LL) / 86400LL; }

    void _record(const BeCascadeBook::Leg& lg, double exit_px, const char* reason, int64_t now_ms) noexcept {
        const double pnl = (exit_px - lg.entry) * lot;           // long-only
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = "LONG"; tr.engine = engine_name; tr.exitReason = reason;
        tr.entryPrice = lg.entry; tr.exitPrice = exit_px; tr.size = lot; tr.pnl = pnl;
        tr.entryTs = lg.entry_ts / 1000LL; tr.exitTs = now_ms / 1000LL;
        tr.mfe = lg.mfe_pct / 100.0 * lg.entry; tr.shadow = shadow_mode;
        std::printf("[%s] CLOSE %s %s @ %.2f entry=%.2f pnl=%.2f %s%s\n", tag.c_str(), symbol.c_str(),
                    lg.parent ? "PARENT" : "MIMIC", exit_px, lg.entry, pnl, reason,
                    shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (on_trade_record) on_trade_record(tr);
    }

    void _close_all(double px, const char* reason, int64_t now_ms) noexcept {
        for (auto& lg : book_.legs) if (lg.open) { lg.open = false; _record(lg, px, reason, now_ms); }
        book_.reset();
    }

    // HARD REVERSAL STOP (S-2026-07-13, operator): per-TICK — the moment ANY open leg is
    // loss_cut_bp below its entry, cut the WHOLE book (parent + every mimic) at that leg's stop.
    // Long-only: below entry the up-move is over. Bounds every loss (no -146/-1402 tail) and is
    // edge-neutral (a leg still above entry never trips it — winners exit via giveback/reversal).
    void _intrabar_stop(double mid, int64_t now_ms) noexcept {
        if (!book_.active || loss_cut_bp <= 0.0 || mid <= 0.0) return;
        for (const auto& lg : book_.legs) {
            if (!lg.open || lg.entry <= 0.0) continue;
            if ((mid / lg.entry - 1.0) * 1e4 <= -loss_cut_bp) {   // long-only: below entry by cut
                _close_all(mid, "REVERSAL_CUT", now_ms);
                return;
            }
        }
    }

    // cascade management on a FINALIZED daily close; executions at `mid` (new-day open)
    void _manage_on_close(double fin_close, double mid, int64_t now_ms) noexcept {
        if (!book_.active) return;
        // parent mfe
        double fav_par_bp = (fin_close / book_.legs[0].entry - 1.0) * 1e4;
        if (fav_par_bp > book_.par_mfe_bp) book_.par_mfe_bp = fav_par_bp;
        // spawn chain: parent covers BE -> mimic 1; last mimic covers BE -> next
        bool spawn = false;
        if (book_.next_arm == 0) spawn = (book_.par_mfe_bp >= be_bp);
        else if (book_.next_arm < arms.size()) {
            const auto& prev = book_.legs.back();
            spawn = prev.open && (fin_close / prev.entry - 1.0) * 1e4 >= be_bp;
        }
        if (spawn && book_.next_arm < arms.size()) {
            BeCascadeBook::Leg lg; lg.entry = mid; lg.arm_pct = arms[book_.next_arm++];
            lg.entry_ts = now_ms;
            book_.legs.push_back(lg);
            std::printf("[%s] SPAWN %s mimic#%zu arm=%.1f%% @ %.2f%s\n", tag.c_str(), symbol.c_str(),
                        book_.next_arm, lg.arm_pct, mid, shadow_mode ? " [SHADOW]" : "");
            std::fflush(stdout);
        }
        // per-leg g50 clip (mimics only; parent rides to reversal)
        for (size_t k = 1; k < book_.legs.size(); k++) {
            auto& lg = book_.legs[k];
            if (!lg.open) continue;
            const double fav = (fin_close / lg.entry - 1.0) * 100.0;
            if (fav > lg.mfe_pct) lg.mfe_pct = fav;
            if (lg.mfe_pct >= lg.arm_pct && fav <= lg.mfe_pct * (1.0 - gb)) {
                lg.open = false; _record(lg, mid, "G50_CLIP", now_ms);
            }
        }
        // reversal
        if ((int)closes_.size() >= W + 1) {
            const double j = closes_.back() / closes_.front() - 1.0;
            if (j <= -thr) _close_all(mid, "REV_EXIT", now_ms);
        }
    }

    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        if (!rebased_ && !closes_.empty()) {                      // kill seed-venue level offset
            const double k = mid / closes_.back();
            for (auto& c : closes_) c *= k;
            rebased_ = true;
            std::printf("[%s] REBASE %s seed x%.5f (last seed close -> live mid %.2f)\n",
                        tag.c_str(), symbol.c_str(), k, mid);
            std::fflush(stdout);
        }
        _intrabar_stop(mid, now_ms);   // S-2026-07-13: per-tick hard reversal stop (before aggregation)
        const int64_t day = utc_day(now_ms);
        if (cur_day_ < 0) { cur_day_ = day; day_close_ = mid; return; }
        if (day != cur_day_) {
            const double fin = day_close_;
            closes_.push_back(fin);
            while ((int)closes_.size() > W + 1) closes_.pop_front();
            cur_day_ = day; day_close_ = mid;
            _manage_on_close(fin, mid, now_ms);
            // entry: flat + window full + jump
            if (enabled && !book_.active && (int)closes_.size() >= W + 1) {
                const double j = closes_.back() / closes_.front() - 1.0;
                if (j >= thr
                    && ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid, mid * 0.02, lot, 1.5)) {
                    book_.active = true; book_.dir = +1;
                    BeCascadeBook::Leg par; par.parent = true; par.entry = mid; par.entry_ts = now_ms;
                    book_.legs.push_back(par);
                    std::printf("[%s] ENTRY %s PARENT LONG @ %.2f (j=%.2f%% >= %.1f%%)%s\n", tag.c_str(),
                                symbol.c_str(), mid, j * 100.0, thr * 100.0, shadow_mode ? " [SHADOW]" : "");
                    std::fflush(stdout);
                }
            }
            return;
        }
        day_close_ = mid;
    }

    // warm-seed: daily bars ts[,ms|s],o,h,l,c -- fills the close window; NO entries.
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[%s] SEED FAIL '%s'\n", tag.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        int fed = 0; int64_t last_ts = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == 't' || line[0] == 'b') continue;
            std::stringstream ss(line); std::string a, o, h, l, c;
            std::getline(ss, a, ','); std::getline(ss, o, ','); std::getline(ss, h, ',');
            std::getline(ss, l, ','); std::getline(ss, c, ',');
            if (c.empty()) continue;
            const double cl = std::strtod(c.c_str(), nullptr);
            if (cl <= 0) continue;
            int64_t ms = std::strtoll(a.c_str(), nullptr, 10); if (ms < 100000000000LL) ms *= 1000LL;
            closes_.push_back(cl);
            while ((int)closes_.size() > W + 1) closes_.pop_front();
            last_ts = ms; ++fed;
        }
        std::printf("[SEED] [%s] %s fed=%d closes=%zu last_bar_ts=%lld\n", tag.c_str(), symbol.c_str(),
                    fed, closes_.size(), (long long)(last_ts / 1000LL));
        std::fflush(stdout);
        return fed;
    }

    // ---- persistence (multicell shape: one snapshot per open leg, "<base>#<k>") ----
    // sl field carries mfe (parent: par_mfe_bp; mimic: mfe_pct), tp carries arm_pct —
    // repurposed transport slots (this book has no SL/TP), documented here.
    void persist_save_all(const char* base, const char* sym, std::vector<omega::PositionSnapshot>& out) const {
        if (!book_.active) return;
        for (size_t k = 0; k < book_.legs.size(); k++) {
            const auto& lg = book_.legs[k];
            if (!lg.open) continue;
            omega::PositionSnapshot ps;
            ps.engine = std::string(base) + "#" + std::to_string(k);
            ps.symbol = sym; ps.side = "LONG"; ps.size = lot;
            ps.entry = lg.entry;
            ps.sl = lg.parent ? book_.par_mfe_bp : lg.mfe_pct;
            ps.tp = lg.arm_pct;
            ps.entry_ts = lg.entry_ts / 1000LL;
            out.push_back(ps);
        }
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        const auto h = ps.engine.find('#');
        if (h == std::string::npos) return false;
        const size_t k = (size_t)std::strtoul(ps.engine.c_str() + h + 1, nullptr, 10);
        BeCascadeBook::Leg lg;
        lg.parent = (k == 0); lg.entry = ps.entry; lg.arm_pct = ps.tp;
        lg.mfe_pct = lg.parent ? 0.0 : ps.sl; lg.entry_ts = ps.entry_ts * 1000LL;
        book_.active = true; book_.dir = +1;
        if (lg.parent) book_.par_mfe_bp = ps.sl;
        book_.legs.push_back(lg);
        if (!lg.parent && k > book_.next_arm) book_.next_arm = k;   // k mimics restored -> next tier = k
        return true;
    }
    bool force_close_all_at(double bid, double ask, const char* reason) {
        if (!book_.active || bid <= 0 || ask <= 0) return false;
        _close_all((bid + ask) * 0.5, reason, (int64_t)std::time(nullptr) * 1000LL);
        return true;
    }
};

// ── 2. XauBracketCascadeEngine: H1 two-sided OCO bracket (gold) ──────────────
struct XauBracketCascadeEngine {
    bool shadow_mode = true;
    bool enabled     = false;

    int    tf_secs = 3600;    // bar size (3600=H1 flagship; 300/600/900 = the S-2026-07-12d
                              // bull-gated intraday instances, W scaled to a ~12h window)
    int    W       = 240;     // bars in the movement/reversal window
    double thr     = 0.02;    // movement + reversal threshold
    double boff    = 0.003;   // bracket offset from trigger close
    int    ttl     = 48;      // bracket pending TTL, bars
    // Optional entry gate (S-2026-07-12d): when set and returning TRUE, NO new bracket is
    // placed and a pending one will not fill. Wired to omega::gold_regime().long_blocked()
    // on the intraday instances — backtested gate: 2022 bear bleed -12..-15% -> flat
    // (-2.5..+0.6), bull keeps 75-92% of net, PF improves. The H1 flagship stays UNGATED
    // (two-sided bracket IS its bear protection at the 10-day window).
    std::function<bool()> entry_blocked;
    double be_bp   = 20.0;
    double gb      = 0.50;
    double loss_cut_bp = 0.0; // HARD REVERSAL STOP (S-2026-07-13, operator): per-tick cut at
                               // loss_cut_bp below a leg's entry (bracket long OR short side). 0=off.
    std::vector<double> arms = {0.2, 2, 3, 4, 6, 8};
    double lot     = 1.0;

    std::string symbol      = "XAUUSD";
    std::string engine_name = "XauBracketCascade";
    std::string tag         = "XBRC";
    using TradeRecordCallback = std::function<void(const omega::TradeRecord&)>;
    TradeRecordCallback on_trade_record;

    // H1 aggregation
    int64_t cur_hour_ = -1; double hour_close_ = 0.0;
    std::deque<double> closes_;         // finalized H1 closes (keep W+1)
    bool rebased_ = false;

    // pending OCO bracket
    bool   brk_pending_ = false;
    double brk_buy_ = 0.0, brk_sell_ = 0.0;
    int    brk_ttl_left_ = 0;

    BeCascadeBook book_;
    bool has_open_position() const noexcept { return book_.active; }

    int64_t bar_of(int64_t ms) const noexcept { return (ms / 1000LL) / (int64_t)tf_secs; }

    void _record(const BeCascadeBook::Leg& lg, double exit_px, const char* reason, int64_t now_ms) noexcept {
        const double pnl = book_.dir * (exit_px - lg.entry) * lot;
        omega::TradeRecord tr{};
        tr.symbol = symbol; tr.side = book_.dir > 0 ? "LONG" : "SHORT";
        tr.engine = engine_name; tr.exitReason = reason;
        tr.entryPrice = lg.entry; tr.exitPrice = exit_px; tr.size = lot; tr.pnl = pnl;
        tr.entryTs = lg.entry_ts / 1000LL; tr.exitTs = now_ms / 1000LL;
        tr.mfe = lg.mfe_pct / 100.0 * lg.entry; tr.shadow = shadow_mode;
        std::printf("[%s] CLOSE %s %s %s @ %.2f entry=%.2f pnl=%.2f %s%s\n", tag.c_str(), symbol.c_str(),
                    lg.parent ? "PARENT" : "MIMIC", book_.dir > 0 ? "LONG" : "SHORT",
                    exit_px, lg.entry, pnl, reason, shadow_mode ? " [SHADOW]" : "");
        std::fflush(stdout);
        if (on_trade_record) on_trade_record(tr);
    }

    void _close_all(double px, const char* reason, int64_t now_ms) noexcept {
        for (auto& lg : book_.legs) if (lg.open) { lg.open = false; _record(lg, px, reason, now_ms); }
        book_.reset();
    }

    // HARD REVERSAL STOP (S-2026-07-13, operator): per-TICK. Two-sided — favour is measured in
    // the ACTIVATED direction (book_.dir). The moment any open leg is loss_cut_bp adverse, cut the
    // whole book (parent + mimics) at that stop. Bounds every loss; edge-neutral (a leg still in
    // profit vs entry never trips it).
    void _intrabar_stop(double mid, int64_t now_ms) noexcept {
        if (!book_.active || loss_cut_bp <= 0.0 || mid <= 0.0) return;
        const int d = book_.dir;
        for (const auto& lg : book_.legs) {
            if (!lg.open || lg.entry <= 0.0) continue;
            if (d * (mid / lg.entry - 1.0) * 1e4 <= -loss_cut_bp) {
                _close_all(mid, "REVERSAL_CUT", now_ms);
                return;
            }
        }
    }

    void _manage_on_close(double fin_close, double mid, int64_t now_ms) noexcept {
        if (!book_.active) return;
        const int d = book_.dir;
        double fav_par_bp = d * (fin_close / book_.legs[0].entry - 1.0) * 1e4;
        if (fav_par_bp > book_.par_mfe_bp) book_.par_mfe_bp = fav_par_bp;
        bool spawn = false;
        if (book_.next_arm == 0) spawn = (book_.par_mfe_bp >= be_bp);
        else if (book_.next_arm < arms.size()) {
            const auto& prev = book_.legs.back();
            spawn = prev.open && d * (fin_close / prev.entry - 1.0) * 1e4 >= be_bp;
        }
        if (spawn && book_.next_arm < arms.size()) {
            BeCascadeBook::Leg lg; lg.entry = mid; lg.arm_pct = arms[book_.next_arm++];
            lg.entry_ts = now_ms;
            book_.legs.push_back(lg);
            std::printf("[%s] SPAWN %s mimic#%zu %s arm=%.1f%% @ %.2f%s\n", tag.c_str(), symbol.c_str(),
                        book_.next_arm, d > 0 ? "LONG" : "SHORT", lg.arm_pct, mid,
                        shadow_mode ? " [SHADOW]" : "");
            std::fflush(stdout);
        }
        for (size_t k = 1; k < book_.legs.size(); k++) {
            auto& lg = book_.legs[k];
            if (!lg.open) continue;
            const double fav = d * (fin_close / lg.entry - 1.0) * 100.0;
            if (fav > lg.mfe_pct) lg.mfe_pct = fav;
            if (lg.mfe_pct >= lg.arm_pct && fav <= lg.mfe_pct * (1.0 - gb)) {
                lg.open = false; _record(lg, mid, "G50_CLIP", now_ms);
            }
        }
        if ((int)closes_.size() >= W + 1) {
            const double j = closes_.back() / closes_.front() - 1.0;
            if ((d > 0 && j <= -thr) || (d < 0 && j >= thr)) _close_all(mid, "REV_EXIT", now_ms);
        }
    }

    void on_tick(double bid, double ask, int64_t now_ms) noexcept {
        if (bid <= 0.0 || ask <= 0.0) return;
        const double mid = (bid + ask) * 0.5;
        if (!rebased_ && !closes_.empty()) {
            const double k = mid / closes_.back();
            for (auto& c : closes_) c *= k;
            rebased_ = true;
            std::printf("[%s] REBASE %s seed x%.5f (last seed close -> live mid %.2f)\n",
                        tag.c_str(), symbol.c_str(), k, mid);
            std::fflush(stdout);
        }
        _intrabar_stop(mid, now_ms);   // S-2026-07-13: per-tick hard reversal stop
        const int64_t hr = bar_of(now_ms);
        if (cur_hour_ < 0) { cur_hour_ = hr; hour_close_ = mid; return; }

        // tick-level bracket fill (real stop semantics); regime-gated instances also
        // refuse the FILL if the regime flipped bear while the bracket was pending
        if (brk_pending_ && enabled && !book_.active) {
            if (entry_blocked && entry_blocked()) {
                brk_pending_ = false;
                std::printf("[%s] BRACKET GATE-CANCEL %s (regime long_blocked)\n", tag.c_str(), symbol.c_str());
                std::fflush(stdout);
            }
        }
        if (brk_pending_ && enabled && !book_.active) {
            int d = 0;
            if (mid >= brk_buy_) d = +1; else if (mid <= brk_sell_) d = -1;
            if (d != 0) {
                brk_pending_ = false;   // fill one side, cancel the other
                if (ExecutionCostGuard::is_viable(symbol.c_str(), ask - bid, mid * 0.02, lot, 1.5)) {
                    book_.active = true; book_.dir = d;
                    BeCascadeBook::Leg par; par.parent = true; par.entry = mid; par.entry_ts = now_ms;
                    book_.legs.push_back(par);
                    std::printf("[%s] BRACKET FILL %s PARENT %s @ %.2f (buy=%.2f sell=%.2f)%s\n",
                                tag.c_str(), symbol.c_str(), d > 0 ? "LONG" : "SHORT", mid,
                                brk_buy_, brk_sell_, shadow_mode ? " [SHADOW]" : "");
                    std::fflush(stdout);
                }
            }
        }

        if (hr != cur_hour_) {
            const double fin = hour_close_;
            closes_.push_back(fin);
            while ((int)closes_.size() > W + 1) closes_.pop_front();
            cur_hour_ = hr; hour_close_ = mid;
            _manage_on_close(fin, mid, now_ms);
            if (brk_pending_ && --brk_ttl_left_ <= 0) {
                brk_pending_ = false;
                std::printf("[%s] BRACKET TTL-CANCEL %s\n", tag.c_str(), symbol.c_str());
                std::fflush(stdout);
            }
            // movement trigger -> place OCO bracket (regime-gated instances: no NEW
            // bracket while long_blocked — the S-2026-07-12d backtested bull gate)
            if (enabled && !book_.active && !brk_pending_ && (int)closes_.size() >= W + 1
                && !(entry_blocked && entry_blocked())) {
                const double j = closes_.back() / closes_.front() - 1.0;
                if (std::fabs(j) >= thr) {
                    brk_pending_ = true; brk_ttl_left_ = ttl;
                    brk_buy_ = fin * (1.0 + boff); brk_sell_ = fin * (1.0 - boff);
                    std::printf("[%s] BRACKET PLACE %s buy=%.2f sell=%.2f (j=%.2f%%) ttl=%d%s\n",
                                tag.c_str(), symbol.c_str(), brk_buy_, brk_sell_, j * 100.0, ttl,
                                shadow_mode ? " [SHADOW]" : "");
                    std::fflush(stdout);
                }
            }
            return;
        }
        hour_close_ = mid;
    }

    // warm-seed: H1 bars ts[,ms|s],o,h,l,c -- fills the close window; NO entries.
    int seed_from_csv(const std::string& path) noexcept {
        std::ifstream f(path);
        if (!f.is_open()) { std::printf("[%s] SEED FAIL '%s'\n", tag.c_str(), path.c_str()); return 0; }
        std::string line; std::getline(f, line);
        int fed = 0; int64_t last_ts = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == 't' || line[0] == 'b') continue;
            std::stringstream ss(line); std::string a, o, h, l, c;
            std::getline(ss, a, ','); std::getline(ss, o, ','); std::getline(ss, h, ',');
            std::getline(ss, l, ','); std::getline(ss, c, ',');
            if (c.empty()) continue;
            const double cl = std::strtod(c.c_str(), nullptr);
            if (cl <= 0) continue;
            int64_t ms = std::strtoll(a.c_str(), nullptr, 10); if (ms < 100000000000LL) ms *= 1000LL;
            closes_.push_back(cl);
            while ((int)closes_.size() > W + 1) closes_.pop_front();
            last_ts = ms; ++fed;
        }
        std::printf("[SEED] [%s] %s fed=%d closes=%zu last_bar_ts=%lld (gold seed may lag -- honest after %d live H1 bars)\n",
                    tag.c_str(), symbol.c_str(), fed, closes_.size(), (long long)(last_ts / 1000LL), W);
        std::fflush(stdout);
        return fed;
    }

    // ---- persistence (multicell shape; sl=mfe transport, tp=arm transport; side carries dir) ----
    void persist_save_all(const char* base, const char* sym, std::vector<omega::PositionSnapshot>& out) const {
        if (!book_.active) return;
        for (size_t k = 0; k < book_.legs.size(); k++) {
            const auto& lg = book_.legs[k];
            if (!lg.open) continue;
            omega::PositionSnapshot ps;
            ps.engine = std::string(base) + "#" + std::to_string(k);
            ps.symbol = sym; ps.side = book_.dir > 0 ? "LONG" : "SHORT"; ps.size = lot;
            ps.entry = lg.entry;
            ps.sl = lg.parent ? book_.par_mfe_bp : lg.mfe_pct;
            ps.tp = lg.arm_pct;
            ps.entry_ts = lg.entry_ts / 1000LL;
            out.push_back(ps);
        }
    }
    bool persist_restore(const omega::PositionSnapshot& ps) {
        const auto h = ps.engine.find('#');
        if (h == std::string::npos) return false;
        const size_t k = (size_t)std::strtoul(ps.engine.c_str() + h + 1, nullptr, 10);
        BeCascadeBook::Leg lg;
        lg.parent = (k == 0); lg.entry = ps.entry; lg.arm_pct = ps.tp;
        lg.mfe_pct = lg.parent ? 0.0 : ps.sl; lg.entry_ts = ps.entry_ts * 1000LL;
        book_.active = true; book_.dir = (ps.side == "SHORT") ? -1 : +1;
        if (lg.parent) book_.par_mfe_bp = ps.sl;
        book_.legs.push_back(lg);
        if (!lg.parent && k > book_.next_arm) book_.next_arm = k;
        return true;
    }
    bool force_close_all_at(double bid, double ask, const char* reason) {
        if (!book_.active || bid <= 0 || ask <= 0) return false;
        _close_all((bid + ask) * 0.5, reason, (int64_t)std::time(nullptr) * 1000LL);
        return true;
    }
};

} // namespace omega
