#pragma once
// DualMomMimicBook.hpp — INDEPENDENT BE-floored mimic companion book riding the
// DualMomentumEngine's holdings (S-2026-07-23, operator: certified DualMom mimic).
//
// COMPANION-INDEPENDENCE (hard rule, feedback-companion-independent-engine):
//   This is a SEPARATE, INDEPENDENT engine. It reads the parent's position
//   snapshot ONE-WAY and NEVER closes, moves, or shrinks the parent position —
//   the parent rides its own certified exits regardless. Judged STANDALONE:
//   its own book net-positive after costs, WF both halves, 2022 — never
//   "vs riding the parent wide". Additive capital, operator-allocated.
//
// CERT (scratchpad dualmom_mimic_recert.py / dualmom_mimic_recert.json,
// S-2026-07-23; companion legs run off the TRAILED parent g10/arm5/STAY-OUT,
// honest daily-close fills incl. gap tails below BE, RT 20bp):
//   cell cf3/g10/rev5d|5%/oneclip: n=299 clips, +40192bp = +$8038 at $2k/leg,
//   PF 1.91, mDD $1312, 2022 +429bp (n22=4), WF H1 +10181 / H2 +30011,
//   2x-cost (40bp) net +32842bp PF 1.70 — PASS-ALL6.
//   nNeg=222, worst clip -1672bp (REAL booked gap tail — see MIMIC-FLOOR note).
//
// MECHANISM (replicates the cert companion() loop exactly):
//   FEED: wirer polls omega::dual_momentum_engine().collect_positions() and
//     passes the snapshot to on_parent_snapshot(snap, now_sec); daily closes
//     arrive via on_daily_close(sym, ts, close) at the same wide-csv poller
//     cadence as the parent. No tick path.
//   LEG LIFECYCLE, keyed (symbol, parent entry_ts):
//     parent position APPEARS  -> arm a mimic slot ANCHORED at the parent entry
//     px, leg 0 PENDING. A PENDING leg books/pays NOTHING (BE-ENTRY foundation,
//     feedback-befloor-on-open-foundation): it opens only when a daily close
//     >= anchor*(1+confirm_pct/100) (confirm 3% = 300bp >> 2x RT 40bp, the
//     mandate's confirm >= 2x round-trip floor) — the clip opens AT that close.
//     ONECLIP: one clip per leg; after the clip closes the leg is DONE until
//     the parent re-enters the name with a NEW entry_ts (new key, new slot).
//   STAGED MULTI-LEG (operator spec addition S-2026-07-23; default n_legs=1 =
//     the certified single-leg cell — the wirer flips to 2 only after the
//     staged cert lands): per parent slot, leg k (k>=1) is ARMED (appended
//     PENDING) only at the moment leg k-1 OPENS (BE-covered), and opens at a
//     close >= leg_{k-1}_fill*(1+stage_confirm_pct/100). Each leg carries its
//     OWN BE floor at its OWN fill, its own shares (notional_usd each), its own
//     oneclip lifecycle. Same exits; PARENT EXIT closes ALL open legs.
//   OPEN-clip exits (first hit wins, evaluated on each daily close):
//     (a) REV5D_EXIT  close < min(prior rev_low_days closes SINCE PARENT ENTRY
//         — the cert window is bounded by the parent entry index, so the slot
//         keeps its own close window, not the global history);
//     (b) DAYRET_CUT  day return <= -rev_day_pct% (vs the global prior close);
//     (c) FLOOR_STOP  close < fp-clamped floor = fill*(1+2*rt_cost_bp/1e4)
//         (= fill*1.004 at 20bp), clamped >= BE = fill*(1+rt_cost_bp/1e4).
//         On a gap through the floor the book records the ACTUAL close — the
//         REAL tail, never a clamp (S-17f honesty; cert nNeg=222, worst -1672bp);
//     (d) PARENT_EXIT the (symbol, entry_ts) key left the snapshot -> close all
//         open legs at the last known close (source-watch; the mimic NEVER
//         affects the parent). Pending legs are discarded unbooked (honest:
//         they never opened, so they book nothing).
//   G-TRAIL: NOT implemented. The cert proved it INERT on daily closes — at
//     cf3 every g in {10,15,20,30} books the IDENTICAL n299/+40192bp (see
//     dualmom_mimic_recert.json grid): the BE floor + rev5d always fires first
//     on daily grade. Adding one would be dead config (MSVC C4189 class).
//
// MIMIC-FLOOR / be_floor_on_open (prebe_loss_audit FILE marker — S-17f honest
//   framing): the BE floor is a DESIGN/config property — BE-ENTRY means no leg
//   ever books before favour >= confirm >= 2x cost, and the resting floor sits
//   >= BE from the moment of open. It is NOT an execution guarantee: an honest
//   fill on a gap through the floor books BELOW it (the cert's real -1672bp
//   worst). Never restate nNeg=0 as an execution truth.
//
// DRAWDOWN-CANCEL: DAYRET_CUT -5% day-return close (+ the fp-clamped BE floor as
//   backstop) IS the certified cancel — dualmom_mimic_recert.py S-2026-07-23 cert
//   grid: the rev5d|5% exit pair binds before any deeper lc on daily gap-through
//   bars (g/lc-style levers verified INERT at this grade: clip lists byte-identical
//   across g {10..30}); worst booked tail -1672bp is the honest gap-through floor.
// ADVERSE-PROTECTION: BE-floor + rev5d|5% reversal exit + parent-exit
//   source-watch, certified S-2026-07-23 (cell cf3/g10/rev5d|5% PF1.91,
//   2022 +429bp); g-trail inert on daily grade (all g cells identical at cf3);
//   REAL pre-BE gap tails booked (nNeg>0, worst -1672bp) — REDUCED tail, not
//   zero.
//
// COST GATE: ExecutionCostGuard::is_viable injected via gate_fn_ before every
//   live order (same wiring shape as DualMom/DayMover7; 2%-of-price TP proxy).
//
// EXEC REFUSAL HONESTY (DualMom S-23e class, ladder-style): an empty broker
//   token on a live open means the fill did NOT happen — the leg is NOT booked
//   (no phantom). The leg is SKIPPED with a printf; NO retry set is kept —
//   legs are parent-scoped and the confirm context is a one-shot level, so a
//   skipped leg goes DONE (and, staged, does NOT arm its successor: the
//   successor's trigger is "predecessor OPENED", which never happened).
//
// FEED WARMUP: wirer seeds per-name closes via seed_close() (rev/dayret prev-
//   close context only; slots start empty — deploy-forward). Heartbeat: NO
//   internal registration — the wirer registers AND pulses (DayMover7 contract).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "OpenPositionRegistry.hpp"

namespace omega {

class DualMomMimicBook {
public:
    struct Config {
        bool   enabled          = false;
        bool   live_book        = false;  // wirer flips after the operator live decision
        double confirm_pct      = 3.0;    // leg-0 BE-ENTRY confirm from the parent-entry anchor
                                          // (3% = 300bp >> 2x RT 40bp, mandate floor)
        int    rev_low_days     = 5;      // rev5d: close < min(prior 5 closes since entry)
        double rev_day_pct      = 5.0;    // single-day return cut (close/prev - 1 <= -5%)
        double notional_usd     = 2000.0; // equal-$ shares per leg (Bigcap3G4 idiom):
                                          // shares = max(1, round(notional/px)), persisted per leg
        double rt_cost_bp       = 20.0;   // measured RT at $2k/leg: 10bp + $1 min commission x2
        int    n_legs           = 1;      // staged legs per parent slot. DEFAULT 1 = the
                                          // certified cell; flip to 2 ONLY after the staged cert
        double stage_confirm_pct = 3.0;   // leg k>=1 confirm from leg_{k-1} fill
        std::string engine_tag  = "DualMomMimic";
        std::string state_path  = "dualmommimic_live.txt";
    };
    Config cfg;

    using OpenFn   = std::function<std::string(const std::string&, bool, double, double)>;
    using CloseFn  = std::function<void(const std::string&, bool, double, double, const std::string&)>;
    using GateFn   = std::function<bool(const std::string&, double, double)>;
    using LedgerFn = std::function<void(const std::string&, const std::string&, bool,
                                        double, double, double, int64_t, int64_t, const char*)>;
    void set_exec(OpenFn o, CloseFn c, GateFn g, LedgerFn l) {
        open_fn_ = std::move(o); close_fn_ = std::move(c);
        gate_fn_ = std::move(g); ledger_fn_ = std::move(l);
    }

    // history seed: closes only, no trading (rev/dayret prev-close context).
    void seed_close(const std::string& sym, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        push_hist_(sym, close);
    }
    void finalize_seed() {
        std::lock_guard<std::mutex> lk(mu_);
        std::printf("[DUALMOMMIMIC] seed done: %zu names with close history, deploy-forward flat\n",
                    hist_.size());
        std::fflush(stdout);
    }

    // ONE-WAY parent watch: the wirer passes dual_momentum_engine().collect_positions().
    // READ-ONLY on the parent — this book never issues anything against it.
    void on_parent_snapshot(const std::vector<PositionSnapshot>& snap, int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        std::set<std::string> present;
        bool changed = false;
        for (const auto& ps : snap) {
            if (ps.side != "LONG" || ps.entry <= 0.0) continue;   // parent is long-only
            const std::string k = key_(ps.symbol, ps.entry_ts);
            present.insert(k);
            if (slots_.count(k)) continue;                        // known (incl. restart-restored)
            Slot sl; sl.sym = ps.symbol; sl.parent_ets = ps.entry_ts; sl.anchor = ps.entry;
            Leg L0; L0.stage = 0; L0.st = LEG_PENDING; L0.base = ps.entry;
            sl.legs.push_back(L0);
            slots_.emplace(k, std::move(sl));
            std::printf("[DUALMOMMIMIC] ARM %s parent_ets=%lld anchor=%.4f (PENDING, opens at close>=%.4f)\n",
                        ps.symbol.c_str(), (long long)ps.entry_ts, ps.entry,
                        ps.entry * (1.0 + cfg.confirm_pct / 100.0));
            std::fflush(stdout);
            changed = true;
        }
        // (d) PARENT EXIT: key gone from the snapshot -> close ALL open legs at the
        // last known close (cert: books at the parent-exit close); pending legs
        // are dropped unbooked. Slot removed — a parent re-entry has a new entry_ts.
        for (auto it = slots_.begin(); it != slots_.end();) {
            if (present.count(it->first)) { ++it; continue; }
            Slot& sl = it->second;
            for (auto& L : sl.legs) {
                if (L.st != LEG_OPEN) continue;
                double px = last_px_(sl.sym);
                if (px <= 0.0) px = L.fill;   // no mark: book flat-of-cost, never invent a px
                book_clip_(sl.sym, L, px, now_sec, "PARENT_EXIT");
            }
            std::printf("[DUALMOMMIMIC] parent %s ets=%lld gone -> slot closed\n",
                        sl.sym.c_str(), (long long)sl.parent_ets);
            std::fflush(stdout);
            it = slots_.erase(it);
            changed = true;
        }
        if (changed) save_();
    }

    // one daily close for one name (same poller cadence as the parent).
    void on_daily_close(const std::string& sym, int64_t ts_sec, double close) {
        std::lock_guard<std::mutex> lk(mu_);
        if (close <= 0.0) return;
        const double prev = last_px_(sym);       // global prior close (cert prev1 = F[s][j-1])
        push_hist_(sym, close);
        bool changed = false;
        for (auto& kv : slots_) {
            Slot& sl = kv.second;
            if (sl.sym != sym) continue;
            // exit context, computed once per slot-bar (cert companion() loop):
            // rev5d window = closes since PARENT ENTRY only (slot-local, cert
            // prevs = F[s][max(ei, j-revN)..j)), dayret vs the global prior close.
            const bool rev = !sl.closes.empty() &&
                close < *std::min_element(sl.closes.begin(), sl.closes.end());
            const bool dayret = prev > 0.0 &&
                (close / prev - 1.0) <= -cfg.rev_day_pct / 100.0;
            // (a)/(b)/(c) on OPEN legs — first hit wins, per leg.
            for (auto& L : sl.legs) {
                if (L.st != LEG_OPEN) continue;
                const char* why = nullptr;
                if (rev)         why = "REV5D_EXIT";
                else if (dayret) why = "DAYRET_CUT";
                else {
                    // be_floor_on_open: resting floor anchored at THIS leg's own fill,
                    // fp-clamped >= BE (mandate: 2x-RT floor, clamp kills the IEEE-754
                    // ~-1e-7bp residue). Books the ACTUAL close on a breach — the real
                    // (possibly sub-floor gap) tail, never the level (S-17f).
                    const double flr = std::max(L.fill * (1.0 + 2.0 * cfg.rt_cost_bp / 1e4),
                                                L.fill * (1.0 + cfg.rt_cost_bp / 1e4));
                    if (close < flr) why = "FLOOR_STOP";
                }
                if (why) { book_clip_(sym, L, close, ts_sec, why); changed = true; }
            }
            // BE-ENTRY confirms on PENDING legs — AFTER exits, so a leg opening on
            // this close is never exit-checked against the same bar (cert: a leg
            // cannot open and close on the same j). Index loop: open_leg_ success
            // may append the next staged leg (vector may reallocate).
            for (size_t i = 0; i < sl.legs.size(); ++i) {
                if (sl.legs[i].st != LEG_PENDING) continue;
                const double cf = (sl.legs[i].stage == 0) ? cfg.confirm_pct
                                                          : cfg.stage_confirm_pct;
                if (close >= sl.legs[i].base * (1.0 + cf / 100.0)) {
                    if (open_leg_(sym, sl, i, close, ts_sec)) changed = true;
                }
            }
            sl.closes.push_back(close);
            while ((int)sl.closes.size() > cfg.rev_low_days) sl.closes.pop_front();
        }
        if (changed) save_();
    }

    int kill_all(double, int64_t now_sec) {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& kv : slots_) {
            Slot& sl = kv.second;
            for (auto& L : sl.legs) {
                if (L.st != LEG_OPEN) continue;
                double px = last_px_(sl.sym);
                if (px <= 0.0) px = L.fill;
                book_clip_(sl.sym, L, px, now_sec, "MANUAL_KILL_ALL");   // honest mark, never floored
                ++n;
            }
        }
        slots_.clear();
        save_();
        return n;
    }

    std::vector<PositionSnapshot> collect_positions() {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<PositionSnapshot> v;
        for (auto& kv : slots_) {
            Slot& sl = kv.second;
            for (auto& L : sl.legs) {
                if (L.st != LEG_OPEN) continue;
                PositionSnapshot o;
                o.engine = cfg.engine_tag; o.symbol = sl.sym; o.side = "LONG";
                o.size = L.shares; o.entry = L.fill;
                o.current = last_px_(sl.sym);
                o.unrealized_pnl = (o.current > 0.0 ? (o.current - L.fill) : 0.0) * L.shares;
                o.entry_ts = L.open_ts;      // seconds-native (poller now_s, epoch s)
                o.token = L.token;
                v.push_back(o);
            }
        }
        return v;
    }

    // migration-tolerant line format: unknown keywords are skipped, a slot is
    // whatever rows follow its SLOT line. DONE legs persist too (oneclip memory).
    void load_state() {
        std::ifstream f(cfg.state_path);
        if (!f.is_open()) return;
        std::string line;
        Slot* cur = nullptr;
        int nslot = 0, nleg = 0;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string kw;
            if (!(ss >> kw)) continue;
            if (kw == "SLOT") {
                std::string sym; long long ets = 0; double anchor = 0;
                if (!(ss >> sym >> ets >> anchor)) { cur = nullptr; continue; }
                Slot sl; sl.sym = sym; sl.parent_ets = (int64_t)ets; sl.anchor = anchor;
                cur = &slots_.emplace(key_(sym, (int64_t)ets), std::move(sl)).first->second;
                ++nslot;
            } else if (kw == "LEG" && cur) {
                Leg L; long long ots = 0; std::string tok;
                if (!(ss >> L.stage >> L.st >> L.base >> L.fill >> L.shares >> ots >> tok))
                    continue;
                L.open_ts = (int64_t)ots;
                L.token = (tok == "-") ? std::string() : tok;
                cur->legs.push_back(L);
                ++nleg;
            } else if (kw == "CLS" && cur) {
                double c;
                while (ss >> c) {
                    cur->closes.push_back(c);
                    while ((int)cur->closes.size() > cfg.rev_low_days) cur->closes.pop_front();
                }
            }
            // any other keyword: forward/backward-format row, skip.
        }
        if (nslot)
            std::printf("[DUALMOMMIMIC] restored %d slot(s) / %d leg(s)\n", nslot, nleg);
    }

private:
    enum LegState { LEG_PENDING = 0, LEG_OPEN = 1, LEG_DONE = 2 };
    struct Leg {
        int     stage = 0;        // staged index k (persisted)
        int     st    = LEG_PENDING;
        double  base  = 0.0;      // confirm base: slot anchor (k=0) / leg_{k-1} fill (k>=1)
        double  fill  = 0.0;      // open fill = the confirming close
        double  shares = 0.0;     // equal-$: max(1, round(notional_usd/fill)), fixed at open
        int64_t open_ts = 0;
        std::string token;        // broker clOrdId; "" = book-only
    };
    struct Slot {
        std::string sym;
        int64_t parent_ets = 0;
        double  anchor = 0.0;     // parent entry px (leg-0 confirm base)
        std::deque<double> closes;   // closes SINCE parent appeared (rev5d window bound)
        std::vector<Leg> legs;
    };
    std::map<std::string, Slot> slots_;               // key = sym|parent_ets
    std::map<std::string, std::deque<double>> hist_;  // global closes (prev-close / marks)
    std::mutex mu_;
    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    static std::string key_(const std::string& sym, int64_t ets) {
        return sym + "|" + std::to_string((long long)ets);
    }
    void push_hist_(const std::string& sym, double close) {
        auto& h = hist_[sym];
        h.push_back(close); if (h.size() > 60) h.pop_front();
    }
    double last_px_(const std::string& s) const {
        auto it = hist_.find(s);
        return (it != hist_.end() && !it->second.empty()) ? it->second.back() : 0.0;
    }

    // BE-ENTRY open at the confirming close. Refusal honesty: empty token on a
    // live open = NOT booked, leg SKIPPED (DONE, fill=0), no retry, successor
    // never arms. Success: arm the next staged leg (base = this fill).
    bool open_leg_(const std::string& sym, Slot& sl, size_t idx, double px, int64_t ts) {
        const double shares = std::max(1.0, std::round(cfg.notional_usd / px));
        std::string tok;
        if (cfg.live_book && open_fn_) {
            if (gate_fn_ && !gate_fn_(sym, px * 0.02, shares))
                return false;   // cost-gate veto: stay PENDING (gate is entry-time, re-eval next close)
            tok = open_fn_(sym, true, shares, px);
            if (tok.empty()) {
                Leg& Lr = sl.legs[idx];
                std::printf("[DUALMOMMIMIC] BUY %s leg%d @%.4f REFUSED by exec -- NOT booked, leg skipped (no retry)\n",
                            sym.c_str(), Lr.stage, px);
                std::fflush(stdout);
                Lr.st = LEG_DONE;   // fill stays 0 = skipped marker
                return true;
            }
        }
        Leg& L = sl.legs[idx];
        L.st = LEG_OPEN; L.fill = px; L.shares = shares; L.open_ts = ts; L.token = tok;
        std::printf("[DUALMOMMIMIC] OPEN %s leg%d @%.4f x%.0f (confirm from base %.4f) tok=%s\n",
                    sym.c_str(), L.stage, px, shares, L.base,
                    tok.empty() ? "(book-only)" : tok.c_str());
        std::fflush(stdout);
        const int next = L.stage + 1;
        if (next < cfg.n_legs) {
            Leg Ln; Ln.stage = next; Ln.st = LEG_PENDING; Ln.base = px;   // staged: arm on open
            sl.legs.push_back(Ln);                                        // (invalidates L)
            std::printf("[DUALMOMMIMIC] STAGE %s leg%d armed PENDING (opens at close>=%.4f)\n",
                        sym.c_str(), next, px * (1.0 + cfg.stage_confirm_pct / 100.0));
            std::fflush(stdout);
        }
        return true;
    }

    // books the ACTUAL exit px (honest fill — a gap through the floor books the
    // real sub-floor tail, S-17f). ONECLIP: leg goes DONE, never re-arms; a new
    // clip on this name requires a new parent entry_ts.
    void book_clip_(const std::string& sym, Leg& L, double px, int64_t ts, const char* why) {
        if (cfg.live_book && !L.token.empty() && close_fn_)
            close_fn_(sym, true, L.shares, px, L.token);
        if (ledger_fn_)
            ledger_fn_(cfg.engine_tag, sym, true, L.fill, px, L.shares, L.open_ts, ts, why);
        const double netbp = (px / L.fill - 1.0) * 1e4 - cfg.rt_cost_bp;
        std::printf("[DUALMOMMIMIC] CLIP %s leg%d fill=%.4f exit=%.4f x%.0f net=%+.0fbp (%s)\n",
                    sym.c_str(), L.stage, L.fill, px, L.shares, netbp, why);
        std::fflush(stdout);
        L.st = LEG_DONE;
    }

    void save_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            if (!f.is_open()) {
                std::printf("[DUALMOMMIMIC][FATAL-STATE] cannot open %s for write\n", tmp.c_str());
                std::fflush(stdout);
                return;
            }
            f.precision(10);
            for (const auto& kv : slots_) {
                const Slot& sl = kv.second;
                f << "SLOT " << sl.sym << " " << (long long)sl.parent_ets << " " << sl.anchor << "\n";
                if (!sl.closes.empty()) {
                    f << "CLS";
                    for (double c : sl.closes) f << " " << c;
                    f << "\n";
                }
                for (const auto& L : sl.legs)
                    f << "LEG " << L.stage << " " << L.st << " " << L.base << " " << L.fill
                      << " " << L.shares << " " << (long long)L.open_ts << " "
                      << (L.token.empty() ? "-" : L.token) << "\n";
            }
        }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
};

inline DualMomMimicBook& dualmom_mimic_book() noexcept {
    static DualMomMimicBook b;
    return b;
}

} // namespace omega
