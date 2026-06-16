// squeeze_engine_test.cpp -- drive the CRTP wrapper end-to-end: synthetic squeeze ->
// pending-entry fill at next-bar open -> ATR stop/target + exit -> TradeRecord emitted
// with MFE/MAE populated. Verifies the wrapper, not just the core.
#include "SqueezeSlingshotEngine.hpp"
#include <cstdio>
#include <vector>

// Test traits: small periods so the gate can fire within a synthetic budget.
struct SqzTraits_TEST : omega::SqzBaseParams {
    static constexpr const char* SYMBOL = "TEST";
    static constexpr int BB_LENGTH = 10, KC_LENGTH = 10, MOM_LENGTH = 10;
    static constexpr int EMA_FAST = 3, EMA_A = 5, EMA_B = 8, EMA_SLOW = 13;
    static constexpr bool REQUIRE_MOMO_BELOW_ZERO = false;
    static constexpr double ATR_TARGET_MULT = 4.0;
    static constexpr bool VERBOSE_LOG = true;
};

int main() {
    omega::SqueezeSlingshotEngine<SqzTraits_TEST> eng;
    eng.shadow_mode = true;
    int trades = 0; int with_mfe = 0; double last_pnl = 0;
    eng.on_close_cb = [&](const omega::TradeRecord& tr) {
        ++trades; last_pnl = tr.pnl;
        if (tr.mfe != 0.0 || tr.mae != 0.0) ++with_mfe;
        std::printf("  TRADE %s pnl=%.4f mfe=%.4f mae=%.4f reason=%s\n",
                    tr.side.c_str(), tr.pnl, tr.mfe, tr.mae, tr.exitReason.c_str());
    };

    // 80 bars tight compression (BB inside KC), then 40 bars rising expansion.
    std::vector<std::array<double,5>> bars;  // o,h,l,c,ts
    double px = 1000.0; long ts = 0; const long BAR = 3600000LL;
    for (int i = 0; i < 80; ++i) { px += 0.05; bars.push_back({px-0.05,px+0.15,px-0.15,px+0.05,(double)(ts+=BAR)}); }
    for (int i = 0; i < 40; ++i) { px += 2.2;  bars.push_back({px-1.0, px+1.8, px-1.2, px+1.2,(double)(ts+=BAR)}); }

    for (auto& b : bars) eng.on_bar(b[0], b[1], b[2], b[3], (long long)b[4]);

    std::printf("trades=%d with_mfe_or_mae=%d last_pnl=%.4f\n", trades, with_mfe, last_pnl);
    const bool ok = (trades >= 1) && (with_mfe >= 1);
    std::printf("%s\n", ok ? "ENGINE WRAPPER TEST PASS" : "ENGINE WRAPPER TEST FAIL");
    return ok ? 0 : 1;
}
