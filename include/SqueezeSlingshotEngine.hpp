#pragma once
//  ADVERSE-PROTECTION: in-flight protection = fixed ATR stop/target bracket (ATR_STOP_MULT=2.0 / ATR_TARGET_MULT=4.0) + momo-rollover SIGNAL_EXIT + optional MAX_HOLD time-stop (default 0=off); no LOSS_CUT/BE ratchet. DEAD / culled -- CULL_LEDGER.tsv 2026-06-18 FAITHFUL (squeeze_xregime_nas.cpp real engine: all variants bear-neg, no gate edge); not wired in engine_init -- not live, protection moot, re-enable blocked. (backfill S-2026-06-24n)
// SqueezeSlingshotEngine.hpp -- CRTP wrapper around SqueezeSlingshotCore.
//
// Mirrors the AtrMeanRevGridEngine CRTP shape: template<class Traits>, per-symbol
// Traits structs (+ SWEEP_ variants for the harness grid), on_tick() aggregates mid
// bars at Traits::BAR_INTERVAL_MS and drives the lookahead-free signal core, and
// closed trades route through on_close_cb (set in engine_init -> write_shadow_csv).
//
// The CORE (SqueezeSlingshotCore.hpp) owns the math + signals; THIS owns fills, the
// single position, ATR stop/target, exit hints, and the TradeRecord. Signal on bar t
// fills at bar t+1 OPEN (the core's documented contract -> no lookahead). MFE/MAE are
// tracked through the hold (capture-ratio analyzable per the 2026-06-16 standard).

#include "SqueezeSlingshotCore.hpp"
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>

namespace omega {

// -----------------------------------------------------------------------------
// Base params (static constexpr). Per-symbol Traits inherit + override.
// Squeeze/EMA/ATR knobs mirror squeeze::Params; SYMBOL/BAR/LOT are harness wiring.
// -----------------------------------------------------------------------------
struct SqzBaseParams {
    static constexpr const char* SYMBOL = "XAUUSD";
    static constexpr std::int64_t BAR_INTERVAL_MS = 3600000LL;  // H1
    static constexpr double BASE_LOT = 0.01;
    static constexpr bool   VERBOSE_LOG = false;

    // squeeze core knobs
    static constexpr int    BB_LENGTH = 20;
    static constexpr double BB_MULT = 2.0;
    static constexpr int    KC_LENGTH = 20;
    static constexpr double KC_MULT_LOW = 2.0;
    static constexpr double KC_MULT_MID = 1.5;
    static constexpr double KC_MULT_HIGH = 1.0;
    static constexpr bool   KC_USE_TRUE_RANGE = true;
    static constexpr int    MOM_LENGTH = 20;
    static constexpr int    EMA_FAST = 8;
    static constexpr int    EMA_A = 21;
    static constexpr int    EMA_B = 34;
    static constexpr int    EMA_SLOW = 55;
    static constexpr bool   REQUIRE_SQUEEZE_ON = true;
    static constexpr int    MIN_TIER = 1;
    static constexpr bool   REQUIRE_MOMO_BELOW_ZERO = false;   // sweep flag (strict variant)
    static constexpr int    ATR_LENGTH = 14;
    static constexpr double ATR_STOP_MULT = 2.0;
    static constexpr double ATR_TARGET_MULT = 4.0;
    static constexpr bool   EXIT_ON_MOMO_ROLLOVER = true;
    static constexpr bool   EXIT_ON_SQUEEZE_FIRE = false;
    static constexpr int    MAX_HOLD_BARS = 0;                 // 0 disables
    static constexpr bool   LONG_ONLY = false;
};

// Production per-symbol traits (extend as needed).
struct SqzTraits_NAS100 : SqzBaseParams {
    static constexpr const char* SYMBOL = "NAS100";
    static constexpr std::int64_t BAR_INTERVAL_MS = 3600000LL;
};
struct SqzTraits_XAUUSD : SqzBaseParams {
    static constexpr const char* SYMBOL = "XAUUSD";
};

// Sweep traits (harness grid points -- pairwise X3; add rows as the grid expands).
struct SqzTraits_SWEEP_NAS100_A : SqzTraits_NAS100 {        // looser: tier1, momo-rising
    static constexpr int MIN_TIER = 1;
    static constexpr bool REQUIRE_MOMO_BELOW_ZERO = false;
    static constexpr double ATR_TARGET_MULT = 4.0;
};
struct SqzTraits_SWEEP_NAS100_B : SqzTraits_NAS100 {        // tighter compression
    static constexpr int MIN_TIER = 2;
    static constexpr bool REQUIRE_MOMO_BELOW_ZERO = false;
    static constexpr double ATR_TARGET_MULT = 6.0;
};
struct SqzTraits_SWEEP_NAS100_C : SqzTraits_NAS100 {        // strict before-the-fire variant
    static constexpr int MIN_TIER = 2;
    static constexpr bool REQUIRE_MOMO_BELOW_ZERO = true;
    static constexpr double ATR_TARGET_MULT = 0.0;          // trail/rollover only
};

template<class Traits>
class SqueezeSlingshotEngine {
public:
    using traits_t = Traits;

    bool enabled     = true;
    bool shadow_mode = true;
    std::function<void(const TradeRecord&)> on_close_cb = nullptr;

    SqueezeSlingshotEngine() : eval_(make_params()) {}

    static squeeze::Params make_params() {
        squeeze::Params p;
        p.bb_length = Traits::BB_LENGTH; p.bb_mult = Traits::BB_MULT;
        p.kc_length = Traits::KC_LENGTH; p.kc_mult_low = Traits::KC_MULT_LOW;
        p.kc_mult_mid = Traits::KC_MULT_MID; p.kc_mult_high = Traits::KC_MULT_HIGH;
        p.kc_use_true_range = Traits::KC_USE_TRUE_RANGE;
        p.mom_length = Traits::MOM_LENGTH;
        p.ema_fast = Traits::EMA_FAST; p.ema_a = Traits::EMA_A;
        p.ema_b = Traits::EMA_B; p.ema_slow = Traits::EMA_SLOW;
        p.require_squeeze_on = Traits::REQUIRE_SQUEEZE_ON; p.min_tier = Traits::MIN_TIER;
        p.require_momo_below_zero_for_long = Traits::REQUIRE_MOMO_BELOW_ZERO;
        p.atr_length = Traits::ATR_LENGTH; p.atr_stop_mult = Traits::ATR_STOP_MULT;
        p.atr_target_mult = Traits::ATR_TARGET_MULT;
        p.exit_on_momo_rollover = Traits::EXIT_ON_MOMO_ROLLOVER;
        p.exit_on_squeeze_fire = Traits::EXIT_ON_SQUEEZE_FIRE;
        p.max_hold_bars = Traits::MAX_HOLD_BARS;
        return p;
    }

    bool has_open_position() const noexcept { return pos_.active; }

    // Production entry point: tick -> internal bar aggregation -> on_bar() on close.
    void on_tick(double bid, double ask, std::int64_t ts_ms) {
        if (bid <= 0.0 || ask <= 0.0) return;
        last_bid_ = bid; last_ask_ = ask;
        const double mid = (bid + ask) * 0.5;
        constexpr std::int64_t BAR_MS = Traits::BAR_INTERVAL_MS;
        const std::int64_t bucket = (ts_ms / BAR_MS) * BAR_MS;
        if (!bar_active_) {
            bar_start_ = bucket; bo_ = bh_ = bl_ = bc_ = mid; bar_active_ = true;
        } else if (bucket != bar_start_) {
            on_bar(bo_, bh_, bl_, bc_, bar_start_ + BAR_MS);   // close stamp = bar end
            bar_start_ = bucket; bo_ = bh_ = bl_ = bc_ = mid;
        } else {
            if (mid > bh_) bh_ = mid; if (mid < bl_) bl_ = mid; bc_ = mid;
        }
    }

    // Closed-bar handler: drive the core, manage the position, fill pending entries.
    void on_bar(double o, double h, double l, double c, std::int64_t ts_ms) {
        squeeze::Bar bar; bar.open = o; bar.high = h; bar.low = l; bar.close = c;
        const squeeze::BarSignal sig = eval_.update(bar);

        // 1) Manage an EXISTING position with this bar (h/l for stop/target + excursion).
        if (pos_.active) {
            ++pos_.bars_held;
            const double fav = pos_.is_long ? (h - pos_.entry_px) : (pos_.entry_px - l);
            const double adv = pos_.is_long ? (pos_.entry_px - l) : (h - pos_.entry_px);
            if (fav > pos_.mfe) pos_.mfe = fav;
            if (adv > pos_.mae) pos_.mae = adv;

            const char* why = nullptr; double exit_px = c;
            if (pos_.is_long) {
                if (l <= pos_.stop)                              { why = "SL_HIT";  exit_px = pos_.stop; }
                else if (pos_.target > 0.0 && h >= pos_.target)  { why = "TP_HIT";  exit_px = pos_.target; }
            } else {
                if (h >= pos_.stop)                              { why = "SL_HIT";  exit_px = pos_.stop; }
                else if (pos_.target > 0.0 && l <= pos_.target)  { why = "TP_HIT";  exit_px = pos_.target; }
            }
            if (!why && sig.warm) {
                const bool ex = pos_.is_long ? squeeze::slingshot_long_exit(prev_sig_, sig, make_params())
                                             : squeeze::slingshot_short_exit(prev_sig_, sig, make_params());
                if (ex) { why = "SIGNAL_EXIT"; exit_px = c; }
            }
            if (!why && Traits::MAX_HOLD_BARS > 0 && pos_.bars_held >= Traits::MAX_HOLD_BARS) {
                why = "MAX_HOLD"; exit_px = c;
            }
            if (why) close(exit_px, ts_ms, why);
        }

        // 2) Fill a pending entry at THIS bar's OPEN (signal came on the prior bar).
        if (pending_ && !pos_.active && enabled) {
            open(pending_long_, o, pending_atr_, ts_ms);
            pending_ = false;
        }

        // 3) Arm a new pending entry from this bar's signal (fills next bar's open).
        if (enabled && !pos_.active && !pending_ && sig.warm && sig.entry != squeeze::Signal::None) {
            const bool is_long = (sig.entry == squeeze::Signal::EnterLong);
            if (!(Traits::LONG_ONLY && !is_long) && sig.atr > 0.0) {
                pending_ = true; pending_long_ = is_long; pending_atr_ = sig.atr;
            }
        }
        prev_sig_ = sig;
    }

    // Replay a bar CSV (ts,o,h,l,c[,v]) -- warm-seed without firing live entries.
    std::size_t seed_from_csv(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) { std::fprintf(stderr, "[SEED-FAIL] Sqz-%s: %s\n", Traits::SYMBOL, path.c_str()); return 0; }
        const bool was = enabled; enabled = false;
        std::string ln; std::getline(f, ln); std::size_t n = 0;
        while (std::getline(f, ln)) {
            double ts=0,o=0,h=0,l=0,c=0,v=0;
            if (std::sscanf(ln.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf", &ts,&o,&h,&l,&c,&v) < 5) continue;
            if (c <= 0.0) continue;
            const std::int64_t ts_ms = (ts > 1e11) ? (std::int64_t)ts : (std::int64_t)(ts*1000.0);
            on_bar(o,h,l,c,ts_ms); ++n;
        }
        enabled = was;
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[SEED][Sqz-%s] %zu bars replayed\n", Traits::SYMBOL, n);
        return n;
    }

private:
    struct Pos {
        bool   active = false; bool is_long = false;
        double entry_px = 0.0, lot = 0.0, stop = 0.0, target = 0.0;
        std::int64_t entry_ts = 0; int bars_held = 0;
        double mfe = 0.0, mae = 0.0;
    } pos_;

    void open(bool is_long, double fill, double atr, std::int64_t ts_ms) {
        const squeeze::Params p = make_params();
        pos_ = Pos{};
        pos_.active = true; pos_.is_long = is_long;
        pos_.entry_px = fill; pos_.lot = Traits::BASE_LOT; pos_.entry_ts = ts_ms;
        pos_.stop   = is_long ? squeeze::long_stop_price(fill, atr, p)   : squeeze::short_stop_price(fill, atr, p);
        pos_.target = is_long ? squeeze::long_target_price(fill, atr, p) : squeeze::short_target_price(fill, atr, p);
        if (std::isnan(pos_.target)) pos_.target = 0.0;   // 0 disables
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[Sqz-%s] ENTRY %s @ %.5f stop=%.5f tgt=%.5f%s\n",
                        Traits::SYMBOL, is_long?"LONG":"SHORT", fill, pos_.stop, pos_.target,
                        shadow_mode?" [SHADOW]":"");
    }

    void close(double exit_px, std::int64_t ts_ms, const char* why) {
        if (!pos_.active) return;
        const double diff = pos_.is_long ? (exit_px - pos_.entry_px) : (pos_.entry_px - exit_px);
        const double pnl  = diff * pos_.lot;            // RAW pts*lot; ledger applies tick_value
        TradeRecord tr{};
        tr.symbol      = Traits::SYMBOL;
        tr.side        = pos_.is_long ? "LONG" : "SHORT";
        tr.engine      = "SqueezeSlingshot";
        tr.entryPrice  = pos_.entry_px; tr.exitPrice = exit_px;
        tr.pnl         = pnl; tr.net_pnl = pnl; tr.size = pos_.lot;
        tr.sl          = pos_.stop; tr.tp = pos_.target;
        tr.mfe         = pos_.mfe; tr.mae = pos_.mae;
        tr.entryTs     = pos_.entry_ts / 1000; tr.exitTs = ts_ms / 1000;
        tr.exitReason  = why; tr.regime = "SQUEEZE"; tr.shadow = shadow_mode;
        if constexpr (Traits::VERBOSE_LOG)
            std::printf("[Sqz-%s] EXIT %s %s pnl=%.5f held=%d%s\n",
                        Traits::SYMBOL, pos_.is_long?"LONG":"SHORT", why, pnl, pos_.bars_held,
                        shadow_mode?" [SHADOW]":"");
        if (on_close_cb) on_close_cb(tr);
        pos_ = Pos{};
    }

    squeeze::Evaluator eval_;
    squeeze::BarSignal prev_sig_{};

    // bar aggregator
    bool bar_active_ = false; std::int64_t bar_start_ = 0;
    double bo_=0, bh_=0, bl_=0, bc_=0;
    double last_bid_ = 0.0, last_ask_ = 0.0;

    // pending entry (fills next bar open)
    bool pending_ = false; bool pending_long_ = false; double pending_atr_ = 0.0;
};

// Seed helper (matches engine_seed_helpers naming).
template<class Traits>
inline void seed_squeeze(SqueezeSlingshotEngine<Traits>& eng, const std::string& csv_path) {
    eng.seed_from_csv(csv_path);
}

}  // namespace omega
