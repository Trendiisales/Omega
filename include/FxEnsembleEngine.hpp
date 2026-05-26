#pragma once
// =============================================================================
//  FxEnsembleEngine.hpp -- FX multi-pair multi-family ensemble (S37g 2026-05-26)
// =============================================================================
//
//  PROVENANCE -- /Users/jo/edge_research deep FX edge hunt 2026-05-26
//  (fx_deep_hunt.py). 7 signal families x 7 pairs x 4 TFs x SL/TP/MB grid.
//  Validation funnel (PF>=1.3 sweep -> 3-period intersect -> WF 3/4 folds ->
//  OOS hold-out PF>=0.7*IS):
//
//      Cell                         IS_PF  OOS_PF  WR    RF    Sharpe
//      ------------------------------------------------------------------
//      EURUSD H1 donchian_55  LONG   2.74  2.80   48%   4.0   1.75
//      GBPUSD H2 bb_rev_20    LONG   2.30  3.24   67%   5.0   1.41
//      AUDUSD H4 bb_rev_20    LONG   2.71  inf   100%   999   2.91  (thin n=5)
//      USDCAD H4 3bar_mom     SHORT  2.10  2.39   50%   6.0   1.76
//      USDJPY H2 donchian_20  LONG   1.61  1.67   25%   1.4   0.88
//
//  3 signal families, 5 pairs, mostly H1/H2/H4. All long except USDCAD short
//  (commodity FX = oil proxy, mean-revert short when oversold).
//
//  ARCHITECTURE -- single header, single namespace omega, M15 base TF.
//  Per-pair internal aggregator synthesizes H1/H2/H4 from input M15 bars.
//  Each cell has its own internal indicator state (Donchian, BB, 3barmom).
//  ProtectedEngineGuards bundle per-pair for BE/trail/spread/ATR floor.
//  State persistence: save/load deques + EMA/BB state per-pair to .dat.
//
//  USAGE
//      // globals.hpp
//      static omega::FxEnsembleEngine g_fx_ensemble_eurusd("EURUSD");
//      static omega::FxEnsembleEngine g_fx_ensemble_gbpusd("GBPUSD");
//      static omega::FxEnsembleEngine g_fx_ensemble_audusd("AUDUSD");
//      static omega::FxEnsembleEngine g_fx_ensemble_usdcad("USDCAD");
//      static omega::FxEnsembleEngine g_fx_ensemble_usdjpy("USDJPY");
//
//      // engine_init.hpp
//      g_fx_ensemble_eurusd.enable_cell(CellId::DONCHIAN_55_H1_LONG,3.0,3.0,24);
//      g_fx_ensemble_gbpusd.enable_cell(CellId::BB_REV_20_H2_LONG,3.0,5.0,96);
//      ...
//      g_fx_ensemble_<sym>.shadow_mode = true;
//      g_fx_ensemble_<sym>.lot = 0.01;
//      g_fx_ensemble_<sym>.init();
//      g_fx_ensemble_<sym>.load_or_seed_from_h1_csv("phase1/.../warmup_<SYM>_H1.csv");
//
//      // tick_fx.hpp -- per-tick + per-M15-bar-close
//      g_fx_ensemble_eurusd.on_tick(bid, ask, now_ms, ca_on_close);
//      // (M15 builder calls g_fx_ensemble_<sym>.on_15m_bar(bar, ...))
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <string>

#include "engine_protections.hpp"
#include "OmegaTradeLedger.hpp"

namespace omega {

// ----------------------------------------------------------------------------
//  Cell catalogue -- one per validated (pair, family, TF, side) survivor
// ----------------------------------------------------------------------------
enum class FxCellId {
    DONCHIAN_55_H1_LONG,    // EURUSD H1, SL=3 TP=3 MB=24
    BB_REV_20_H2_LONG,      // GBPUSD H2, SL=3 TP=5 MB=96
    BB_REV_20_H4_LONG,      // AUDUSD H4, SL=3 TP=2 MB=24
    THREE_BAR_MOM_H4_SHORT, // USDCAD H4, SL=1.5 TP=5 MB=24
    DONCHIAN_20_H2_LONG,    // USDJPY H2, SL=1.5 TP=5 MB=96
};

enum class FxTf { M15, H1, H2, H4 };

struct FxEnsembleBar {
    int64_t bar_start_ms = 0;
    double  open = 0.0;
    double  high = 0.0;
    double  low  = 0.0;
    double  close = 0.0;
};

struct FxEnsemblePos {
    bool        active        = false;
    bool        is_long       = false;
    double      entry_px      = 0.0;
    double      sl_px         = 0.0;
    double      tp_px         = 0.0;
    double      atr_at_entry  = 0.0;
    int64_t     entry_ts_ms   = 0;
    int         bars_held     = 0;
    int         cooldown_bars = 0;
    int         max_bars_held = 96;
    FxCellId    cell_id       = FxCellId::DONCHIAN_55_H1_LONG;
    double      mfe_pts       = 0.0;
    double      mae_pts       = 0.0;
    bool        be_armed      = false;
};

struct FxCellConfig {
    FxCellId family;
    FxTf     tf;
    bool     is_long;
    double   sl_mult;   // ATR multiplier
    double   tp_mult;   // R multiplier (TP = sl_mult * tp_mult * ATR)
    int      max_bars_held;
    bool     enabled = false;
    const char* short_name = "";
};

// ----------------------------------------------------------------------------
//  Engine -- one per FX symbol. Wraps up to 5 cells; only cells flagged
//  enabled fire signals for that symbol.
// ----------------------------------------------------------------------------
class FxEnsembleEngine {
public:
    // Engine-owned knobs
    bool   shadow_mode      = true;
    bool   enabled          = false;
    double lot              = 0.01;
    double max_spread_price = 0.00030;  // 3 pips default, overridden per pair
    double min_atr_floor    = 0.00010;  // raw price, overridden per pair
    double be_trigger_atr   = 0.0;      // OFF default
    bool   trail_after_be   = false;
    double trail_atr_mult   = 0.0;

    using OnCloseFn = std::function<void(const omega::TradeRecord&)>;

    explicit FxEnsembleEngine(const char* symbol)
        : symbol_(symbol ? symbol : "UNKNOWN")
    {
        // Initialize default cell catalogue (all disabled by default;
        // engine_init.hpp must explicitly enable_cell() the ones validated
        // for this symbol).
        cells_[0] = { FxCellId::DONCHIAN_55_H1_LONG,    FxTf::H1, true,  3.0, 1.0, 24, false, "Donch55H1L" };
        cells_[1] = { FxCellId::BB_REV_20_H2_LONG,      FxTf::H2, true,  3.0, 1.67,96, false, "BBrev20H2L" };
        cells_[2] = { FxCellId::BB_REV_20_H4_LONG,      FxTf::H4, true,  3.0, 0.67,24, false, "BBrev20H4L" };
        cells_[3] = { FxCellId::THREE_BAR_MOM_H4_SHORT, FxTf::H4, false, 1.5, 3.33,24, false, "3BarMomH4S" };
        cells_[4] = { FxCellId::DONCHIAN_20_H2_LONG,    FxTf::H2, true,  1.5, 3.33,96, false, "Donch20H2L" };
    }

    const std::string& symbol() const { return symbol_; }

    void enable_cell(FxCellId id, double sl, double tp_r, int mb) {
        for (auto& c : cells_) {
            if (c.family == id) {
                c.enabled = true;
                c.sl_mult = sl;
                c.tp_mult = tp_r;
                c.max_bars_held = mb;
                return;
            }
        }
    }

    void init() noexcept {
        bars_m15_.clear(); bars_h1_.clear();
        bars_h2_.clear();  bars_h4_.clear();
        atr14_m15_ = atr14_h1_ = atr14_h2_ = atr14_h4_ = 0.0;
        atr_warmup_m15_ = atr_warmup_h1_ = atr_warmup_h2_ = atr_warmup_h4_ = 0;
        cur_h1_ = cur_h2_ = cur_h4_ = {};
        h1_open_ = h2_open_ = h4_open_ = false;
        cur_h1_n_ = cur_h2_n_ = cur_h4_n_ = 0;
        for (auto& p : pos_) p = {};
        bb_mid_h2_ = bb_std_h2_ = 0.0;
        bb_mid_h4_ = bb_std_h4_ = 0.0;
        donch_55_hi_h1_ = donch_55_lo_h1_ = 0.0;
        donch_20_hi_h2_ = donch_20_lo_h2_ = 0.0;
    }

    bool has_open_position() const noexcept {
        for (const auto& p : pos_) if (p.active) return true;
        return false;
    }
    int open_count() const noexcept {
        int n = 0; for (const auto& p : pos_) if (p.active) ++n; return n;
    }

    // -------------------------------------------------------------------------
    //  on_15m_bar -- called by tick_fx.hpp at every M15 close.
    //  Synthesizes H1/H2/H4 internally; evaluates each enabled cell on its TF.
    // -------------------------------------------------------------------------
    void on_15m_bar(const FxEnsembleBar& bar,
                    double bid, double ask, int64_t now_ms,
                    OnCloseFn on_close) noexcept
    {
        if (!enabled) return;

        _push_bar(bars_m15_, bar, kHistM15);
        _update_atr(bars_m15_, atr14_m15_, atr_warmup_m15_);

        bool h1_closed = _ingest_into_synth(cur_h1_, h1_open_, cur_h1_n_, bar, kM15PerH1);
        if (h1_closed) {
            _push_bar(bars_h1_, cur_h1_, kHistH1);
            _update_atr(bars_h1_, atr14_h1_, atr_warmup_h1_);
            _update_donchian(bars_h1_, 55, donch_55_hi_h1_, donch_55_lo_h1_);
            cur_h1_n_ = 0; h1_open_ = false;
        }
        bool h2_closed = _ingest_into_synth(cur_h2_, h2_open_, cur_h2_n_, bar, kM15PerH2);
        if (h2_closed) {
            _push_bar(bars_h2_, cur_h2_, kHistH2);
            _update_atr(bars_h2_, atr14_h2_, atr_warmup_h2_);
            _update_bb20(bars_h2_, bb_mid_h2_, bb_std_h2_);
            _update_donchian(bars_h2_, 20, donch_20_hi_h2_, donch_20_lo_h2_);
            cur_h2_n_ = 0; h2_open_ = false;
        }
        bool h4_closed = _ingest_into_synth(cur_h4_, h4_open_, cur_h4_n_, bar, kM15PerH4);
        if (h4_closed) {
            _push_bar(bars_h4_, cur_h4_, kHistH4);
            _update_atr(bars_h4_, atr14_h4_, atr_warmup_h4_);
            _update_bb20(bars_h4_, bb_mid_h4_, bb_std_h4_);
            cur_h4_n_ = 0; h4_open_ = false;
        }

        // bars_held tick per cell on cell's own TF close
        for (int ci = 0; ci < kNCells; ++ci) {
            auto& p = pos_[ci];
            if (p.cooldown_bars > 0) --p.cooldown_bars;
            if (!p.active) continue;
            const FxTf tf = cells_[ci].tf;
            if ((tf == FxTf::M15) ||
                (tf == FxTf::H1 && h1_closed) ||
                (tf == FxTf::H2 && h2_closed) ||
                (tf == FxTf::H4 && h4_closed)) {
                ++p.bars_held;
            }
        }

        // spread guard at engine level
        if ((ask - bid) > max_spread_price) return;

        // Try each enabled cell whose TF closed this M15
        for (int ci = 0; ci < kNCells; ++ci) {
            const auto& cfg = cells_[ci];
            if (!cfg.enabled) continue;
            if (pos_[ci].active) continue;
            if (pos_[ci].cooldown_bars > 0) continue;

            const bool tf_ready =
                (cfg.tf == FxTf::H1 && h1_closed) ||
                (cfg.tf == FxTf::H2 && h2_closed) ||
                (cfg.tf == FxTf::H4 && h4_closed);
            if (!tf_ready) continue;

            const int side = _evaluate_signal(ci);
            if (side == 0) continue;
            _fire_entry(ci, side, bid, ask, now_ms);
        }
        (void)on_close;
    }

    void on_tick(double bid, double ask, int64_t now_ms,
                 OnCloseFn on_close = nullptr) noexcept
    {
        if (!enabled) return;
        // Internal M15 aggregator -- builds bars from ticks; calls on_15m_bar
        // on bar close. Caller only needs on_tick() -- same pattern as AMR.
        const int64_t b15 = (now_ms / 900000LL) * 900000LL;
        const double mid = (bid + ask) * 0.5;
        if (m15_start_ == 0) {
            m15_cur_ = { b15, mid, mid, mid, mid };
            m15_start_ = b15;
        } else if (b15 != m15_start_) {
            on_15m_bar(m15_cur_, bid, ask, now_ms, on_close);
            m15_cur_ = { b15, mid, mid, mid, mid };
            m15_start_ = b15;
        } else {
            if (mid > m15_cur_.high) m15_cur_.high = mid;
            if (mid < m15_cur_.low)  m15_cur_.low  = mid;
            m15_cur_.close = mid;
        }
        // Manage open positions intra-bar
        for (int ci = 0; ci < kNCells; ++ci) {
            if (!pos_[ci].active) continue;
            _manage_open(ci, bid, ask, now_ms, on_close);
        }
    }

    void force_close(double bid, double ask, int64_t now_ms,
                     OnCloseFn on_close, const char* reason) noexcept
    {
        for (int ci = 0; ci < kNCells; ++ci) {
            if (!pos_[ci].active) continue;
            const double xp = pos_[ci].is_long ? bid : ask;
            _close(ci, xp, reason ? reason : "FORCE_CLOSE", now_ms, on_close);
        }
    }

    // -------------------------------------------------------------------------
    //  State persistence (S37e pattern) -- save/load bar deques to .dat
    // -------------------------------------------------------------------------
    int save_state(const std::string& path) const noexcept {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return -1;
        const std::uint32_t magic = 0x53584646; // 'FFXS'
        const std::uint32_t ver = 1u;
        std::fwrite(&magic, 4, 1, f);
        std::fwrite(&ver, 4, 1, f);
        auto write_deque = [&](const std::deque<FxEnsembleBar>& dq) {
            std::uint32_t n = static_cast<std::uint32_t>(dq.size());
            std::fwrite(&n, 4, 1, f);
            for (const auto& b : dq) {
                std::fwrite(&b.bar_start_ms, sizeof(int64_t), 1, f);
                std::fwrite(&b.open,  sizeof(double), 1, f);
                std::fwrite(&b.high,  sizeof(double), 1, f);
                std::fwrite(&b.low,   sizeof(double), 1, f);
                std::fwrite(&b.close, sizeof(double), 1, f);
            }
        };
        write_deque(bars_m15_);
        write_deque(bars_h1_);
        write_deque(bars_h2_);
        write_deque(bars_h4_);
        std::fclose(f);
        return 0;
    }

    bool load_state(const std::string& path) noexcept {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::uint32_t magic = 0, ver = 0;
        if (std::fread(&magic, 4, 1, f) != 1 || magic != 0x53584646) { std::fclose(f); return false; }
        if (std::fread(&ver, 4, 1, f) != 1   || ver != 1u)            { std::fclose(f); return false; }
        auto read_deque = [&](std::deque<FxEnsembleBar>& dq) -> bool {
            std::uint32_t n = 0;
            if (std::fread(&n, 4, 1, f) != 1 || n > 10000) return false;
            dq.clear();
            for (std::uint32_t i = 0; i < n; ++i) {
                FxEnsembleBar b{};
                if (std::fread(&b.bar_start_ms, sizeof(int64_t), 1, f) != 1) return false;
                if (std::fread(&b.open,  sizeof(double), 1, f) != 1) return false;
                if (std::fread(&b.high,  sizeof(double), 1, f) != 1) return false;
                if (std::fread(&b.low,   sizeof(double), 1, f) != 1) return false;
                if (std::fread(&b.close, sizeof(double), 1, f) != 1) return false;
                dq.push_back(b);
            }
            return true;
        };
        bool ok = read_deque(bars_m15_) && read_deque(bars_h1_)
                  && read_deque(bars_h2_) && read_deque(bars_h4_);
        std::fclose(f);
        if (!ok) {
            bars_m15_.clear(); bars_h1_.clear();
            bars_h2_.clear();  bars_h4_.clear();
            return false;
        }
        // Recompute derived indicators from loaded bars
        atr14_m15_ = _recompute_atr(bars_m15_, atr_warmup_m15_);
        atr14_h1_  = _recompute_atr(bars_h1_,  atr_warmup_h1_);
        atr14_h2_  = _recompute_atr(bars_h2_,  atr_warmup_h2_);
        atr14_h4_  = _recompute_atr(bars_h4_,  atr_warmup_h4_);
        _update_bb20(bars_h2_, bb_mid_h2_, bb_std_h2_);
        _update_bb20(bars_h4_, bb_mid_h4_, bb_std_h4_);
        _update_donchian(bars_h1_, 55, donch_55_hi_h1_, donch_55_lo_h1_);
        _update_donchian(bars_h2_, 20, donch_20_hi_h2_, donch_20_lo_h2_);
        std::printf("[FX-STATE-LOAD] %s loaded m15=%zu h1=%zu h2=%zu h4=%zu (no warmup)\n",
                    symbol_.c_str(), bars_m15_.size(), bars_h1_.size(),
                    bars_h2_.size(), bars_h4_.size());
        return true;
    }

    // Warmup from H1 CSV (fallback when .dat missing). Format: ts,o,h,l,c (ts secs).
    int seed_from_h1_csv(const std::string& path) noexcept {
        std::FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return 0;
        char line[512];
        int n = 0;
        while (std::fgets(line, sizeof(line), f)) {
            if (line[0] == 't' || line[0] == '#') continue;
            int64_t ts = 0; double o=0,h=0,l=0,c=0;
            char* p = line;
            ts = std::strtoll(p, &p, 10); if (*p==',') ++p;
            o = std::strtod(p, &p);       if (*p==',') ++p;
            h = std::strtod(p, &p);       if (*p==',') ++p;
            l = std::strtod(p, &p);       if (*p==',') ++p;
            c = std::strtod(p, &p);
            FxEnsembleBar b{ts*1000, o, h, l, c};
            _push_bar(bars_h1_, b, kHistH1);
            ++n;
        }
        std::fclose(f);
        atr14_h1_ = _recompute_atr(bars_h1_, atr_warmup_h1_);
        _update_donchian(bars_h1_, 55, donch_55_hi_h1_, donch_55_lo_h1_);
        std::printf("[FX-SEED] %s loaded %d H1 bars from %s\n", symbol_.c_str(), n, path.c_str());
        return n;
    }

    bool load_or_seed_from_h1_csv(const std::string& state_path, const std::string& csv_path) noexcept {
        if (load_state(state_path)) return true;
        return seed_from_h1_csv(csv_path) > 0;
    }

private:
    // --- bar synth + indicator helpers ---
    static constexpr int kHistM15 = 64;
    static constexpr int kHistH1  = 200;   // need 55 for Donchian + 20 for BB
    static constexpr int kHistH2  = 128;
    static constexpr int kHistH4  = 80;
    static constexpr int kAtrPeriod = 14;
    static constexpr int kM15PerH1 = 4;
    static constexpr int kM15PerH2 = 8;
    static constexpr int kM15PerH4 = 16;
    static constexpr int kNCells   = 5;

    std::string symbol_;
    std::array<FxCellConfig, kNCells> cells_{};
    std::array<FxEnsemblePos, kNCells> pos_{};

    std::deque<FxEnsembleBar> bars_m15_;
    std::deque<FxEnsembleBar> bars_h1_;
    std::deque<FxEnsembleBar> bars_h2_;
    std::deque<FxEnsembleBar> bars_h4_;

    double atr14_m15_ = 0.0, atr14_h1_ = 0.0, atr14_h2_ = 0.0, atr14_h4_ = 0.0;
    int    atr_warmup_m15_ = 0, atr_warmup_h1_ = 0, atr_warmup_h2_ = 0, atr_warmup_h4_ = 0;

    FxEnsembleBar cur_h1_{}, cur_h2_{}, cur_h4_{};
    bool h1_open_ = false, h2_open_ = false, h4_open_ = false;
    int  cur_h1_n_ = 0, cur_h2_n_ = 0, cur_h4_n_ = 0;
    // Internal M15 aggregator (for on_tick auto-build)
    FxEnsembleBar m15_cur_{};
    int64_t m15_start_ = 0;

    // Indicator caches (recomputed on bar close)
    double bb_mid_h2_ = 0.0, bb_std_h2_ = 0.0;
    double bb_mid_h4_ = 0.0, bb_std_h4_ = 0.0;
    double donch_55_hi_h1_ = 0.0, donch_55_lo_h1_ = 0.0;
    double donch_20_hi_h2_ = 0.0, donch_20_lo_h2_ = 0.0;

    void _push_bar(std::deque<FxEnsembleBar>& dq, const FxEnsembleBar& b, int maxN) noexcept {
        dq.push_back(b);
        while ((int)dq.size() > maxN) dq.pop_front();
    }

    void _update_atr(const std::deque<FxEnsembleBar>& dq, double& atr, int& warm) noexcept {
        if ((int)dq.size() < 2) { atr = 0.0; return; }
        const auto& c = dq.back(); const auto& p = dq[dq.size()-2];
        double tr = std::max(c.high - c.low,
                             std::max(std::abs(c.high - p.close),
                                      std::abs(c.low  - p.close)));
        if (warm < kAtrPeriod) {
            atr = (atr * warm + tr) / (warm + 1);
            ++warm;
        } else {
            atr = (atr * (kAtrPeriod - 1) + tr) / kAtrPeriod;
        }
    }

    double _recompute_atr(const std::deque<FxEnsembleBar>& dq, int& warm) noexcept {
        warm = 0; double atr = 0.0;
        for (size_t i = 1; i < dq.size(); ++i) {
            const auto& c = dq[i]; const auto& p = dq[i-1];
            double tr = std::max(c.high - c.low,
                                 std::max(std::abs(c.high - p.close),
                                          std::abs(c.low  - p.close)));
            if (warm < kAtrPeriod) { atr = (atr * warm + tr) / (warm + 1); ++warm; }
            else { atr = (atr * (kAtrPeriod - 1) + tr) / kAtrPeriod; }
        }
        return atr;
    }

    void _update_bb20(const std::deque<FxEnsembleBar>& dq, double& mid, double& sd) noexcept {
        const int N = 20;
        if ((int)dq.size() < N) { mid = 0.0; sd = 0.0; return; }
        double sum = 0.0;
        for (int i = (int)dq.size() - N; i < (int)dq.size(); ++i) sum += dq[i].close;
        mid = sum / N;
        double var = 0.0;
        for (int i = (int)dq.size() - N; i < (int)dq.size(); ++i) {
            double d = dq[i].close - mid; var += d*d;
        }
        sd = std::sqrt(var / N);
    }

    void _update_donchian(const std::deque<FxEnsembleBar>& dq, int n,
                          double& hi, double& lo) noexcept {
        if ((int)dq.size() < n) { hi = lo = 0.0; return; }
        hi = -1e18; lo = 1e18;
        for (int i = (int)dq.size() - n; i < (int)dq.size(); ++i) {
            if (dq[i].high > hi) hi = dq[i].high;
            if (dq[i].low  < lo) lo = dq[i].low;
        }
    }

    bool _ingest_into_synth(FxEnsembleBar& cur, bool& open, int& n,
                             const FxEnsembleBar& m15, int per) noexcept {
        if (!open) { cur = m15; open = true; n = 1; }
        else {
            if (m15.high > cur.high) cur.high = m15.high;
            if (m15.low  < cur.low ) cur.low  = m15.low;
            cur.close = m15.close;
            ++n;
        }
        return (n >= per);
    }

    // --- signal evals per cell ---
    int _evaluate_signal(int ci) const noexcept {
        const auto& cfg = cells_[ci];
        switch (cfg.family) {
            case FxCellId::DONCHIAN_55_H1_LONG:    return _sig_donchian_long(bars_h1_, donch_55_hi_h1_);
            case FxCellId::BB_REV_20_H2_LONG:      return _sig_bb_rev_long(bars_h2_, bb_mid_h2_, bb_std_h2_);
            case FxCellId::BB_REV_20_H4_LONG:      return _sig_bb_rev_long(bars_h4_, bb_mid_h4_, bb_std_h4_);
            case FxCellId::THREE_BAR_MOM_H4_SHORT: return _sig_3bar_mom_short(bars_h4_);
            case FxCellId::DONCHIAN_20_H2_LONG:    return _sig_donchian_long(bars_h2_, donch_20_hi_h2_);
        }
        return 0;
    }

    int _sig_donchian_long(const std::deque<FxEnsembleBar>& dq, double hi_n) const noexcept {
        if (dq.size() < 2 || hi_n <= 0) return 0;
        // Long when current close > N-bar high (Turtle break-out long-only)
        if (dq.back().close > hi_n - 1e-12) return +1;
        return 0;
    }

    int _sig_bb_rev_long(const std::deque<FxEnsembleBar>& dq, double mid, double sd) const noexcept {
        if (dq.size() < 20 || sd <= 0) return 0;
        const double lower = mid - 2.0 * sd;
        // RSI<30 + close below lower band
        // Compute quick 14-period RSI from closes
        const int R = 14;
        if (dq.size() < R + 1) return 0;
        double up = 0, dn = 0;
        for (int i = (int)dq.size() - R; i < (int)dq.size(); ++i) {
            double d = dq[i].close - dq[i-1].close;
            if (d > 0) up += d; else dn -= d;
        }
        if (up + dn == 0) return 0;
        double rs = (dn > 0) ? up / dn : 100.0;
        double rsi = 100.0 - 100.0 / (1.0 + rs);
        if (dq.back().close < lower && rsi < 30.0) return +1;
        return 0;
    }

    int _sig_3bar_mom_short(const std::deque<FxEnsembleBar>& dq) const noexcept {
        if (dq.size() < 4) return 0;
        const auto& c0 = dq.back();
        const auto& c1 = dq[dq.size()-2];
        const auto& c2 = dq[dq.size()-3];
        const auto& c3 = dq[dq.size()-4];
        if (c0.close < c1.close && c1.close < c2.close && c2.close < c3.close) return -1;
        return 0;
    }

    double _atr_for_cell(int ci) const noexcept {
        switch (cells_[ci].tf) {
            case FxTf::M15: return atr14_m15_;
            case FxTf::H1:  return atr14_h1_;
            case FxTf::H2:  return atr14_h2_;
            case FxTf::H4:  return atr14_h4_;
        }
        return 0.0;
    }

    void _fire_entry(int ci, int side, double bid, double ask, int64_t now_ms) noexcept {
        const auto& cfg = cells_[ci];
        const double atr = _atr_for_cell(ci);
        if (atr < min_atr_floor) return;
        const double entry = (side > 0) ? ask : bid;
        if (entry <= 0.0) return;

        const double sl_dist = cfg.sl_mult * atr;
        const double tp_dist = cfg.tp_mult * sl_dist;

        auto& p = pos_[ci];
        p.active        = true;
        p.is_long       = (side > 0);
        p.entry_px      = entry;
        p.sl_px         = (side > 0) ? entry - sl_dist : entry + sl_dist;
        p.tp_px         = (side > 0) ? entry + tp_dist : entry - tp_dist;
        p.atr_at_entry  = atr;
        p.entry_ts_ms   = now_ms;
        p.bars_held     = 0;
        p.cooldown_bars = 0;
        p.max_bars_held = cfg.max_bars_held;
        p.cell_id       = cfg.family;
        p.mfe_pts       = 0.0;
        p.mae_pts       = 0.0;
        p.be_armed      = false;
    }

    void _manage_open(int ci, double bid, double ask, int64_t now_ms,
                      OnCloseFn on_close) noexcept {
        auto& p = pos_[ci];
        const double mid = (bid + ask) * 0.5;
        if (mid > 0.0 && p.entry_px > 0.0) {
            const double favourable = p.is_long ? (mid - p.entry_px) : (p.entry_px - mid);
            if (favourable > p.mfe_pts) p.mfe_pts = favourable;
            if (favourable < p.mae_pts) p.mae_pts = favourable;
        }

        // Optional BE arm + trail (off by default, validated cells don't need)
        if (be_trigger_atr > 0 && !p.be_armed
            && p.mfe_pts >= be_trigger_atr * p.atr_at_entry) {
            const double new_sl = p.is_long ? p.entry_px : p.entry_px;
            if (p.is_long  && new_sl > p.sl_px) p.sl_px = new_sl;
            if (!p.is_long && new_sl < p.sl_px) p.sl_px = new_sl;
            p.be_armed = true;
        }
        if (p.be_armed && trail_after_be && trail_atr_mult > 0) {
            if (p.is_long) {
                const double trail = (p.entry_px + p.mfe_pts) - trail_atr_mult * p.atr_at_entry;
                if (trail > p.sl_px) p.sl_px = trail;
            } else {
                const double trail = (p.entry_px - p.mfe_pts) + trail_atr_mult * p.atr_at_entry;
                if (trail < p.sl_px) p.sl_px = trail;
            }
        }

        // max_bars_held timeout
        if (p.max_bars_held > 0 && p.bars_held >= p.max_bars_held) {
            const double xp = p.is_long ? bid : ask;
            _close(ci, xp, "MAX_BARS", now_ms, on_close);
            return;
        }

        bool hit_tp = false, hit_sl = false;
        double xp = 0.0;
        if (p.is_long) {
            if (bid <= p.sl_px)      { xp = p.sl_px; hit_sl = true; }
            else if (bid >= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        } else {
            if (ask >= p.sl_px)      { xp = p.sl_px; hit_sl = true; }
            else if (ask <= p.tp_px) { xp = p.tp_px; hit_tp = true; }
        }
        if (!hit_tp && !hit_sl) return;

        const char* reason = hit_tp ? "TP_HIT"
                            : (p.be_armed ? "TRAIL_HIT" : "SL_HIT");
        _close(ci, xp, reason, now_ms, on_close);
    }

    void _close(int ci, double exit_px, const char* reason,
                int64_t now_ms, OnCloseFn on_close) noexcept {
        auto& p = pos_[ci];
        if (!p.active) return;
        const double pts_move = p.is_long ? (exit_px - p.entry_px) : (p.entry_px - exit_px);

        omega::TradeRecord tr;
        tr.symbol     = symbol_;
        tr.engine     = std::string("FxEnsemble_") + cells_[ci].short_name;
        tr.side       = p.is_long ? "LONG" : "SHORT";
        tr.entryPrice = p.entry_px;
        tr.exitPrice  = exit_px;
        tr.tp         = p.tp_px;
        tr.sl         = p.sl_px;
        tr.size       = lot;
        tr.pnl        = pts_move * lot;
        tr.net_pnl    = tr.pnl;
        tr.mfe        = p.mfe_pts * lot;
        tr.mae        = p.mae_pts * lot;
        tr.entryTs    = p.entry_ts_ms / 1000;
        tr.exitTs     = now_ms / 1000;
        tr.exitReason = reason;
        tr.regime     = cells_[ci].short_name;
        tr.shadow     = shadow_mode;
        if (on_close) on_close(tr);

        p.active = false;
        p.cooldown_bars = 1;
    }
};

} // namespace omega
