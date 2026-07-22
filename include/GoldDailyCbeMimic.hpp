#pragma once
// GoldDailyCbeMimic.hpp — x2 BE-floored mimic companion on the GoldDailyCbe
// parent (S-2026-07-22i follow-up, operator "go"). SEPARATE INDEPENDENT engine:
// never closes, moves, or shrinks the parent position — its own 2-leg book on
// the parent's entry/exit events, judged STANDALONE (never vs the parent ride).
//
// FOUNDATION (be_floor_on_open — BeFloorOnOpenFoundation, S-17c/17f honest form):
//   * legs spawn PENDING at parent entry (trig = parent entry px)
//   * BE-ENTRY: a leg OPENS only when the M1 close >= trig*(1+conf); conf 0.20%
//     ≈ 2.5x the spot round-trip cost; anchored le = epx = that close
//   * reclip = 0 (a clipped leg stays dead)
//   * pre-arm DRAWDOWN-CANCEL at -lc (free cut — mimic never touches the parent)
//   * post-arm BE-floor + giveback trail off mfe
//   * flush at parent exit (window-flush at the parent's exit mark)
//   HONEST BOOKING: fills at the live M1 close — a gap through the floor books
//   the REAL tail (REDUCED tail, not zero; never restate nNeg=0, S-17f).
//
// DRAWDOWN-CANCEL: lc = 1.0% of leg entry — BACKTESTED (gold_daily_cbe_bt.cpp
//   MIMIC=1 grid, GOLD_DAILY_CBE_FINDINGS_2026-07-22.md): lc=1.0 PF 3.29 all-3-
//   regimes+, WF 2.22/4.09, 2x-cost 3.08; lc=0.5 collapses to PF 1.31 with a
//   negative bear bucket — 1.0 is the certified setting, do not tighten.
//
// ADVERSE-PROTECTION: pre-arm -1% drawdown-cancel + post-arm BE-floor +
//   giveback clip (T gb0.5 / W gb0.75) — backtested per the grid above.
//
// COST GATE: ExecutionCostGuard::is_viable injected via gate_fn_ before any
//   live open (same wiring shape as the parent).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace omega {

class GoldDailyCbeMimic {
public:
    struct Config {
        bool        enabled    = false;
        bool        live_book  = false;
        std::string live_sym   = "XAUUSD.S";
        double      lot_oz     = 1.0;
        double      conf       = 0.002;    // BE-ENTRY confirm above trig (>=2x RT)
        double      t_arm      = 0.005;    double t_gb = 0.50;   // T leg
        double      w_arm      = 0.020;    double w_gb = 0.75;   // W leg
        double      lc         = 0.010;    // pre-arm drawdown-cancel (certified 1.0%)
        std::string state_path = "golddailycbemimic_live.txt";
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

    // ---- parent event hooks (parent never sees this book) ----
    void on_parent_open(double entry_px, int64_t ts_sec) {
        if (!cfg.enabled) return;
        legs_.clear();
        legs_.push_back(Leg{"GoldDailyCbeMimicT", cfg.t_arm, cfg.t_gb, entry_px});
        legs_.push_back(Leg{"GoldDailyCbeMimicW", cfg.w_arm, cfg.w_gb, entry_px});
        (void)ts_sec;
        std::printf("[GOLD-CBE-MIMIC] 2 legs PENDING trig=%.2f (conf %.2f%%)\n",
                    entry_px, cfg.conf * 100.0);
        std::fflush(stdout);
        save_();
    }

    void on_m1_close(double c, int64_t ts_sec) {
        if (!cfg.enabled || legs_.empty()) return;
        bool dirty = false;
        for (auto& L : legs_) {
            if (L.dead) continue;
            if (L.pending) {
                if (c >= L.trig * (1.0 + cfg.conf)) {
                    L.pending = false; L.open = true; L.le = c; L.mfe = 0; L.ets = ts_sec;
                    if (cfg.live_book && open_fn_) {
                        if (!gate_fn_ || gate_fn_(cfg.live_sym, L.le * L.arm, cfg.lot_oz))
                            L.token = open_fn_(cfg.live_sym, true, cfg.lot_oz, c);
                    }
                    std::printf("[GOLD-CBE-MIMIC][OPEN] %s @%.2f (BE-ENTRY) tok=%s\n",
                                L.tag, c, L.token.empty() ? "(book-only)" : L.token.c_str());
                    std::fflush(stdout);
                    dirty = true;
                }
                continue;
            }
            if (!L.open) continue;
            const double fav = (c - L.le) / L.le;
            if (fav > L.mfe) L.mfe = fav;
            const bool armed = L.mfe >= L.arm;
            if (!armed && fav <= -cfg.lc)              { clip_(L, c, ts_sec, "MIM_LC");   dirty = true; continue; }
            if (armed && fav <= 0.0)                   { clip_(L, c, ts_sec, "BE_FLOOR"); dirty = true; continue; }
            if (armed && fav <= L.mfe * (1.0 - L.gb))  { clip_(L, c, ts_sec, "MIM_GB");   dirty = true; }
        }
        if (dirty) save_();
    }

    void on_parent_close(double mark, int64_t ts_sec) {
        if (!cfg.enabled || legs_.empty()) return;
        for (auto& L : legs_)
            if (!L.dead && L.open) clip_(L, mark > 0 ? mark : L.le, ts_sec, "FLUSH_PARENT_EXIT");
        legs_.clear();
        save_();
    }

    // KILL-ALL panic closer (on_tick.hpp fan-out)
    int kill_all(double px, int64_t now_sec) {
        int n = 0;
        for (auto& L : legs_)
            if (!L.dead && L.open) { clip_(L, px > 0 ? px : L.le, now_sec, "MANUAL_KILL_ALL"); ++n; }
        legs_.clear();
        save_();
        return n;
    }

    void load_state() {
        std::ifstream f(cfg.state_path);
        if (!f.is_open()) return;
        std::string tag, tok; int pe, op, dd; double arm, gb, trig, le, mfe; long long ets;
        while (f >> tag >> pe >> op >> dd >> arm >> gb >> trig >> le >> mfe >> ets >> tok) {
            Leg L{nullptr, arm, gb, trig};
            L.tag = (tag == "GoldDailyCbeMimicT") ? "GoldDailyCbeMimicT" : "GoldDailyCbeMimicW";
            L.pending = pe; L.open = op; L.dead = dd; L.le = le; L.mfe = mfe;
            L.ets = (int64_t)ets; L.token = (tok == "-") ? std::string() : tok;
            if (!L.dead) legs_.push_back(L);
        }
        if (!legs_.empty()) {
            std::printf("[GOLD-CBE-MIMIC] %zu leg(s) restored\n", legs_.size());
            std::fflush(stdout);
        }
    }

private:
    struct Leg {
        const char* tag; double arm, gb, trig;
        bool pending = true, open = false, dead = false;
        double le = 0, mfe = 0; int64_t ets = 0; std::string token;
    };
    std::vector<Leg> legs_;
    OpenFn open_fn_; CloseFn close_fn_; GateFn gate_fn_; LedgerFn ledger_fn_;

    void clip_(Leg& L, double fill, int64_t ts_sec, const char* reason) {
        if (!L.token.empty() && close_fn_)
            close_fn_(cfg.live_sym, true, cfg.lot_oz, fill, L.token);
        if (ledger_fn_)
            ledger_fn_(L.tag, cfg.live_sym, true, L.le, fill, cfg.lot_oz, L.ets, ts_sec, reason);
        std::printf("[GOLD-CBE-MIMIC][CLIP] %s le=%.2f fill=%.2f (%s)\n", L.tag, L.le, fill, reason);
        std::fflush(stdout);
        L.dead = true; L.open = false; L.token.clear();
    }

    void save_() const {
        const std::string tmp = cfg.state_path + ".tmp";
        { std::ofstream f(tmp, std::ios::trunc); if (!f.is_open()) return;
          for (const auto& L : legs_) {
              if (L.dead) continue;
              f << L.tag << " " << (L.pending?1:0) << " " << (L.open?1:0) << " 0 "
                << L.arm << " " << L.gb << " " << L.trig << " " << L.le << " " << L.mfe
                << " " << (long long)L.ets << " " << (L.token.empty() ? "-" : L.token) << "\n";
          } }
#if defined(_WIN32)
        std::remove(cfg.state_path.c_str());
#endif
        std::rename(tmp.c_str(), cfg.state_path.c_str());
    }
};

inline GoldDailyCbeMimic& gold_daily_cbe_mimic() noexcept {
    static GoldDailyCbeMimic m;
    return m;
}

} // namespace omega
