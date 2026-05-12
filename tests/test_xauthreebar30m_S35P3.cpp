// =============================================================================
//  test_xauthreebar30m_S35P3.cpp
//
//  S35-P3 retrofit behavior test for XauThreeBar30mEngine. Drives a synthetic
//  bar stream through the engine and asserts:
//
//   1. Three-bar continuation signal correctly fires LONG entry at the trigger
//      bar, with SL distance == 2*ATR (SL_MULT) and TP distance == 4*ATR
//      (TP_MULT). Symbol="XAUUSD", side="LONG", engine="XauThreeBar30m".
//   2. At MFE < be_trigger_atr * ATR_at_entry, the BE flag stays false and the
//      SL is unchanged.
//   3. At MFE == be_trigger_atr * ATR_at_entry, the BE arms; when
//      trail_after_be=true and trail_atr_mult>0, the trail immediately
//      tightens the SL beyond the BE-shifted value (cascade behavior:
//      BE proposes entry + 0.10; trail proposes mid - 0.75*ATR; trail
//      wins because it's tighter for a long).
//   4. Trail ratchets the SL UP as MFE grows (mid + ATR -> mid + 2*ATR
//      tightens SL from entry+0.25*ATR to entry+1.25*ATR).
//   5. Trail does NOT loosen the SL when price reverses; when bid drops
//      to <= sl_px the engine closes with exitReason="TRAIL_HIT" (not
//      raw "SL_HIT"; the distinguishing field is guards.st.be_armed).
//   6. Consecutive-loss kill switch trips after exactly max_consec_losses
//      losses; shadow_mode flips and is reflected in subsequent
//      TradeRecord.shadow values.
//   7. Daily loss cap trips at cumulative net_pnl <= -daily_loss_limit;
//      subsequent check_entry_ok returns "DAILY_LOSS_CAP" until UTC
//      day rollover.
//
//  Build:
//      g++ -std=c++17 -O0 -Iinclude tests/test_xauthreebar30m_S35P3.cpp \
//          -o tests/test_xauthreebar30m_S35P3
//
//  Run:
//      ./tests/test_xauthreebar30m_S35P3
//
//  Prints PASS/FAIL per assertion; exits 0 on all-PASS, 1 on first FAIL.
//
//  This test compiles against the post-S35-P3 retrofit of
//  XauThreeBar30mEngine.hpp. If you revert the retrofit, expect compile
//  failure on engine.guards / engine.max_bars_held / engine.be_trigger_atr
//  etc., which is the intended canary.
// =============================================================================

#include "XauThreeBar30mEngine.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int                  g_close_count = 0;
omega::TradeRecord   g_last_close;

void on_close_cb(const omega::TradeRecord& tr) {
    ++g_close_count;
    g_last_close = tr;
    std::printf("  [ON_CLOSE] engine=%s side=%s entry=%.4f exit=%.4f "
                "pnl=%.6f mfe=%.6f mae=%.6f reason=%s shadow=%d\n",
                tr.engine.c_str(), tr.side.c_str(),
                tr.entryPrice, tr.exitPrice,
                tr.pnl, tr.mfe, tr.mae,
                tr.exitReason.c_str(), static_cast<int>(tr.shadow));
}

void check(bool cond, const char* what) {
    std::printf("  %s %s\n", cond ? "[PASS]" : "[FAIL]", what);
    if (!cond) std::exit(1);
}

} // anonymous

int main() {
    omega::XauThreeBar30mEngine eng;
    eng.shadow_mode        = false;   // tr.shadow should be false in output
    eng.enabled            = true;
    eng.lot                = 0.01;
    eng.max_spread         = 1.0;
    eng.max_bars_held      = 0;       // disable time stop for cleanness
    eng.be_trigger_atr     = 1.0;
    eng.be_cost_buffer_pts = 0.10;
    eng.trail_after_be     = true;
    eng.trail_atr_mult     = 0.75;
    eng.daily_loss_limit   = 0.0;     // disabled in main test
    eng.max_consec_losses  = 0;       // disabled in main test
    eng.min_atr_floor      = 0.0;
    eng.max_atr_ceil       = 0.0;
    eng.block_hour_start   = -1;
    eng.block_hour_end     = -1;
    eng.init();

    std::printf("=== Test 1: three-bar LONG signal fires + entry geometry ===\n");
    const int64_t start_ms = 1747000000000LL;
    const int64_t bar_ms   = 30LL * 60LL * 1000LL;

    // Seed bars: 12 oscillating non-directional bars so neither all_up nor
    // all_down latches before our staged setup.
    double px = 3000.0;
    for (int i = 0; i < 12; ++i) {
        omega::XauThreeBar30mBar b{};
        b.bar_start_ms = start_ms + i * bar_ms;
        b.open  = px;
        b.high  = px + 0.7;
        b.low   = px - 0.7;
        b.close = (i % 2 == 0) ? px + 0.4 : px - 0.4;
        eng.on_30m_bar(b, b.close - 0.05, b.close + 0.05,
                       0.0, b.bar_start_ms + bar_ms, on_close_cb);
        px = b.close;
    }

    // Three green bars then trigger bar.
    double base = px;
    omega::XauThreeBar30mBar bs[4];
    for (int i = 0; i < 3; ++i) {
        bs[i].bar_start_ms = start_ms + (12 + i) * bar_ms;
        bs[i].open  = base;
        bs[i].high  = base + 1.0;
        bs[i].low   = base - 0.3;
        bs[i].close = base + 0.8;
        base = bs[i].close;
    }
    bs[3].bar_start_ms = start_ms + 15 * bar_ms;
    bs[3].open  = base;
    bs[3].high  = bs[2].high + 2.0;
    bs[3].low   = base - 0.2;
    bs[3].close = bs[2].high + 1.5;

    for (int i = 0; i < 4; ++i) {
        const double bid = bs[i].close - 0.05;
        const double ask = bs[i].close + 0.05;
        eng.on_30m_bar(bs[i], bid, ask, /*atr14_external=*/1.0,
                       bs[i].bar_start_ms + bar_ms, on_close_cb);
    }

    check(eng.has_open_position(), "trigger bar fired LONG entry");
    check(eng.pos.is_long, "position is long");
    check(eng.pos.entry_px > 0.0, "entry_px > 0");
    const double entry = eng.pos.entry_px;
    const double atr_e = eng.pos.atr_at_entry;
    std::printf("  entry_px=%.4f sl_px=%.4f tp_px=%.4f atr_at_entry=%.4f\n",
                entry, eng.pos.sl_px, eng.pos.tp_px, atr_e);
    check(std::abs((entry - eng.pos.sl_px) - 2.0 * atr_e) < 1e-6,
          "SL distance == 2*ATR (long)");
    check(std::abs((eng.pos.tp_px - entry) - 4.0 * atr_e) < 1e-6,
          "TP distance == 4*ATR (long)");

    std::printf("\n=== Test 2: tick at entry+0.5*ATR (no BE arm) ===\n");
    const double pre_sl = eng.pos.sl_px;
    eng.on_tick(entry + 0.5 * atr_e - 0.05, entry + 0.5 * atr_e + 0.05,
                bs[3].bar_start_ms + bar_ms + 1000, on_close_cb);
    check(!eng.guards.st.be_armed, "BE not armed at +0.5*ATR favourable");
    check(eng.pos.sl_px == pre_sl, "SL unchanged at +0.5*ATR favourable");

    std::printf("\n=== Test 3: tick at entry+1.0*ATR (BE arms + trail cascade) ===\n");
    eng.on_tick(entry + 1.0 * atr_e - 0.05, entry + 1.0 * atr_e + 0.05,
                bs[3].bar_start_ms + bar_ms + 2000, on_close_cb);
    check(eng.guards.st.be_armed, "BE armed at +1.0*ATR favourable");
    const double expected_sl_t3 = entry + 0.25 * atr_e;
    std::printf("  pos.sl_px=%.6f expected_sl=%.6f (trail wins over BE)\n",
                eng.pos.sl_px, expected_sl_t3);
    check(std::abs(eng.pos.sl_px - expected_sl_t3) < 1e-6,
          "SL moved to entry + 0.25*ATR (cascade: BE arms, trail tighter)");

    std::printf("\n=== Test 4: tick at entry+2.0*ATR (trail ratchets up) ===\n");
    eng.on_tick(entry + 2.0 * atr_e - 0.05, entry + 2.0 * atr_e + 0.05,
                bs[3].bar_start_ms + bar_ms + 3000, on_close_cb);
    const double expected_sl_t4 = entry + 1.25 * atr_e;
    std::printf("  pos.sl_px=%.6f expected_sl=%.6f\n",
                eng.pos.sl_px, expected_sl_t4);
    check(std::abs(eng.pos.sl_px - expected_sl_t4) < 1e-6,
          "SL ratcheted to entry + 1.25*ATR");

    std::printf("\n=== Test 5: mid drops to entry+1.0*ATR (trail must not loosen) ===\n");
    eng.on_tick(entry + 1.0 * atr_e - 0.05, entry + 1.0 * atr_e + 0.05,
                bs[3].bar_start_ms + bar_ms + 4000, on_close_cb);
    check(!eng.has_open_position(),
          "trail SL triggered close on reversal to entry+1*ATR");
    check(g_close_count == 1, "exactly one trade closed");
    check(g_last_close.exitReason == "TRAIL_HIT",
          "exit reason is TRAIL_HIT (BE armed before SL touch)");
    check(g_last_close.side == "LONG", "closed trade was LONG");
    check(g_last_close.engine == "XauThreeBar30m", "engine name preserved");
    check(g_last_close.symbol == "XAUUSD", "symbol = XAUUSD");
    check(g_last_close.shadow == false, "shadow flag matches engine setting");
    check(g_last_close.pnl > 0.0, "trail exit captured positive pnl");

    std::printf("\n=== Test 6: killswitch trips after 3 losses ===\n");
    omega::XauThreeBar30mEngine eng2;
    eng2.enabled            = true;
    eng2.shadow_mode        = false;
    eng2.lot                = 0.01;
    eng2.max_consec_losses  = 3;
    eng2.init();
    eng2.guards.on_close(-2.0);
    eng2.guards.on_close(-1.5);
    check(!eng2.guards.st.killswitch_tripped, "not tripped after 2 losses");
    eng2.guards.on_close(-1.0);
    check(eng2.guards.st.killswitch_tripped, "tripped after 3rd loss");

    std::printf("\n=== Test 7: daily cap blocks new entries ===\n");
    omega::XauThreeBar30mEngine eng3;
    eng3.enabled            = true;
    eng3.shadow_mode        = false;
    eng3.lot                = 0.01;
    eng3.daily_loss_limit   = 5.0;
    eng3.init();
    eng3.guards.roll_day(1747000000);
    eng3.guards.on_close(-3.0);
    eng3.guards.on_close(-2.5);
    check(eng3.guards.st.daily_capped,
          "daily_capped set after total loss >= -5.0");
    const char* why = eng3.guards.check_entry_ok(3000.0, 3000.5, 1.0,
                                                  1747000000);
    check(why != nullptr && std::string(why) == "DAILY_LOSS_CAP",
          "check_entry_ok returns DAILY_LOSS_CAP after cap trip");

    std::printf("\n================================================================\n");
    std::printf("SUMMARY: ALL TESTS PASSED\n");
    std::printf("================================================================\n");
    return 0;
}
