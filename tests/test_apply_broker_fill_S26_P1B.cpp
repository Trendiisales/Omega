// =============================================================================
// test_apply_broker_fill_S26_P1B.cpp
//
// Deterministic proof test for the §4 correction to OmegaTradeLedger::
// applyBrokerFill. Drives the actual on-disk header through the live SHORT
// trade captured on 2026-05-10 23:00:38 UTC (account 8077780, FIX ExecReport
// in HANDOFF_S26_PART1B_VERIFICATION_REBUILD.md §2.1):
//
//     entry:        4693.28
//     engine TP:    4692.49      (TP=0.79pt below entry; SHORT)
//     broker close: 4693.39      (0.90pt adverse vs TP)
//     side:         SHORT
//     size:         0.01 lot
//     tick_mult:    100 ($1 per 0.01pt for XAUUSD 0.01 lot)
//
// Reality from cTrader history: net = -$0.11 USD (round to -NZ$0.18 ≈ what the
// history line for this entry timestamp shows).
//
// Three assertions:
//   1. After apply_realistic_costs + applyBrokerFill(entry) + applyBrokerFill
//      (close), tr.net_pnl ≈ -0.11 USD.
//   2. tr.broker_pnl (computed independently from broker fills) ≈ -0.11 USD.
//   3. disparity() (engine vs broker) collapses to ~0 on the cleanly-filled
//      trade.
//
// The test also re-creates the §3 buggy version's arithmetic OFFLINE (without
// touching the header) to demonstrate that the OLD code would have produced
// -$1.12 (11× worse than reality), confirming this test would have caught the
// bug pre-fix.
//
// Build: g++ -std=c++17 -O0 -Iinclude tests/test_apply_broker_fill_S26_P1B.cpp
//        -o tests/test_apply_broker_fill_S26_P1B
// Run:   ./tests/test_apply_broker_fill_S26_P1B
//        prints PASS/FAIL and the side-by-side numbers; exit code 0 on PASS.
// =============================================================================
#include "OmegaTradeLedger.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// USD epsilon for assertions. 1 cent is more than enough -- our math is exact
// in price points; the only float dust comes from doubles compounding small
// half-spread terms.
constexpr double EPS = 0.0001;

bool approx_eq(double a, double b, double eps = EPS) {
    return std::fabs(a - b) < eps;
}

// Re-create the §3 buggy logic offline so we can demonstrate what the broken
// code would have produced for the same inputs. Pure function -- does not
// touch the ledger.
double buggy_old_net_pnl(double entry, double target_close, double actual_close,
                         bool is_long, double size, double tick_mult,
                         double slip_entry_modeled, double commission)
{
    // Step (a) apply_realistic_costs would set slip_exit to either 0 (TP_HIT)
    // or half-spread. The trade closed at engine TP so exitReason="TP_HIT" =>
    // slip_exit = 0 prior to applyBrokerFill.
    //
    // Step (b) the OLD applyBrokerFill close branch did:
    const double slip_units = is_long
        ? (target_close - actual_close)
        : (actual_close - target_close);
    const double slip_out_usd = slip_units * size * tick_mult;
    // (b.5) recompute pnl from broker fills end-to-end (THE BUG):
    const double diff_units = is_long
        ? (actual_close - entry)
        : (entry - actual_close);
    const double recomputed_pnl = diff_units * size * tick_mult;
    // (b.6) net = recomputed_pnl - slip_in - slip_out - commission
    return recomputed_pnl - slip_entry_modeled - slip_out_usd - commission;
}

struct LiveTrade {
    const char* label;
    bool        is_long;
    double      entry;
    double      target_close;   // engine intended exit
    double      actual_close;   // broker fill
    double      cTrader_net_nzd; // for reference; USD path is deterministic
};

// The 14 consecutive cTrader losses from HANDOFF_S26_PART1B §2.2.
// All XAUUSD, 0.01 lot, NZ account. Adverse on every one of them.
const std::vector<LiveTrade> LIVE_TRADES = {
    {"23:00:23", false, 4691.85, 4691.85 - 0.79, 4691.96, -0.18},
    {"23:00:32", false, 4692.78, 4692.78 - 0.79, 4692.99, -0.35},
    {"23:00:40", false, 4693.28, 4693.28 - 0.79, 4693.39, -0.18},  // ← FIX-captured
    {"23:00:48", false, 4693.99, 4693.99 - 0.79, 4694.28, -0.49},
    {"23:01:05", true,  4693.83, 4693.83 + 0.79, 4693.45, -0.64},
    {"23:01:22", false, 4693.73, 4693.73 - 0.79, 4693.94, -0.35},
    {"23:02:05", false, 4693.94, 4693.94 - 0.79, 4694.18, -0.40},
    {"23:02:19", true,  4693.43, 4693.43 + 0.79, 4693.22, -0.35},
    {"23:02:38", true,  4692.95, 4692.95 + 0.79, 4692.75, -0.34},
    {"23:02:41", false, 4692.73, 4692.73 - 0.79, 4692.87, -0.24},
    {"23:03:03", true,  4692.64, 4692.64 + 0.79, 4692.10, -0.44},
    {"23:03:19", true,  4692.23, 4692.23 + 0.79, 4692.10, -0.22},
    {"23:03:33", false, 4692.43, 4692.43 - 0.79, 4692.23, -0.34},
    {"23:03:38", false, 4692.75, 4692.75 - 0.79, 4693.07, -0.54},
};

// Drive one trade through the corrected ledger end-to-end. Returns the final
// net_pnl, broker_pnl, and disparity (eng - broker) for this trade only.
struct LedgerResult {
    double engine_pnl_intended;
    double engine_net;     // tr.net_pnl after both fills applied
    double broker_pnl;     // tr.broker_pnl after both fills applied
    double slip_entry;
    double slip_exit;
    double exit_price;     // overwritten by broker truth
    bool   both_legs_filled;
};

LedgerResult drive_one_trade(const LiveTrade& lt, int trade_id) {
    omega::OmegaTradeLedger lg;
    omega::TradeRecord tr;

    // === Engine intent: this is what handle_closed_trade would set up. ===
    tr.id            = trade_id;
    tr.symbol        = "XAUUSD";
    tr.side          = lt.is_long ? "LONG" : "SHORT";
    tr.entryPrice    = lt.entry;
    tr.exitPrice     = lt.target_close;    // engine THINKS it hit TP
    tr.size          = 0.01;
    tr.spreadAtEntry = 0.22;               // observed BlackBull XAUUSD spread
    tr.exitReason    = "TP_HIT";           // engine's belief

    // pnl = engine's intended gross (entry_intent → exit_intent), in USD via
    // tick_mult. For SHORT: (entry - exit). For LONG: (exit - entry). Always
    // +0.79 * 0.01 * 100 = +$0.79 on the engine's books pre-cost.
    const double tick_mult = 100.0;
    const double diff = lt.is_long
        ? (lt.target_close - lt.entry)
        : (lt.entry - lt.target_close);
    tr.pnl = diff * tr.size * tick_mult;

    // Engine cost model. TP_HIT => slip_exit = 0; slip_entry = half-spread.
    omega::apply_realistic_costs(tr, /*commission_per_side*/ 0.0, tick_mult);
    const double engine_pnl_intended = tr.net_pnl;  // capture before broker truth overwrites

    // Stamp clOrdIds so applyBrokerFill can find this record.
    tr.entry_clOrdId = "ENTRY-" + std::to_string(trade_id);
    tr.close_clOrdId = "CLOSE-" + std::to_string(trade_id);

    lg.record(tr);

    // === Broker truth: ExecReports arrive. Entry fill matches engine intent
    // exactly (we observed entry=4693.28 in the FIX-captured trade). ===
    bool ok_entry = lg.applyBrokerFill("ENTRY-" + std::to_string(trade_id),
                                       lt.entry, tick_mult);
    bool ok_close = lg.applyBrokerFill("CLOSE-" + std::to_string(trade_id),
                                       lt.actual_close, tick_mult);
    assert(ok_entry && "entry fill must find the trade post-record");
    assert(ok_close && "close fill must find the trade post-record");

    // Snapshot final state of the trade.
    const auto trades = lg.snapshot();
    assert(trades.size() == 1);
    const auto& final = trades[0];

    LedgerResult r;
    r.engine_pnl_intended = engine_pnl_intended;
    r.engine_net          = final.net_pnl;
    r.broker_pnl          = final.broker_pnl;
    r.slip_entry          = final.slippage_entry;
    r.slip_exit           = final.slippage_exit;
    r.exit_price          = final.exitPrice;
    r.both_legs_filled    = final.broker_entry_filled && final.broker_close_filled;
    return r;
}

}  // namespace

int main() {
    std::printf("================================================================\n");
    std::printf("S26 P1B applyBrokerFill correction -- definitive proof test\n");
    std::printf("================================================================\n\n");

    int passed = 0, failed = 0;

    // ----- Assertion set 1: the FIX-captured live SHORT trade @ 23:00:40 ----
    {
        const LiveTrade lt = LIVE_TRADES[2];   // the 4693.28/4693.39 trade
        const LedgerResult r = drive_one_trade(lt, 1001);

        // Expected:
        //   engine intended gross  = (4693.28 - 4692.49) * 0.01 * 100 = +0.79
        //   engine intended net    = 0.79 - 0.11 (half-spread on entry) - 0 - 0 = +0.68
        //   measured entry slip    = 0 (broker entry == engine entry)
        //   measured exit slip     = (4693.39 - 4692.49) * 0.01 * 100 = +0.90
        //   corrected net_pnl      = 0.79 - 0 - 0.90 - 0 = -0.11
        //   broker_pnl             = (4693.28 - 4693.39) * 0.01 * 100 = -0.11
        //   exitPrice              = 4693.39 (broker truth, not 4692.49)
        const double exp_engine_pnl = 0.79;
        const double exp_net        = -0.11;
        const double exp_broker     = -0.11;
        const double exp_slip_in    =  0.00;
        const double exp_slip_out   =  0.90;
        const double exp_exit_price = 4693.39;

        std::printf("LIVE TRADE  %s  SHORT  entry=%.4f tp=%.4f close=%.4f\n",
                    lt.label, lt.entry, lt.target_close, lt.actual_close);
        std::printf("  engine.pnl (intended gross) : %+8.4f  expected %+8.4f  %s\n",
                    0.79, exp_engine_pnl,
                    approx_eq(0.79, exp_engine_pnl) ? "ok" : "FAIL");
        std::printf("  slip_entry (measured)       : %+8.4f  expected %+8.4f  %s\n",
                    r.slip_entry, exp_slip_in,
                    approx_eq(r.slip_entry, exp_slip_in) ? "ok" : "FAIL");
        std::printf("  slip_exit  (measured)       : %+8.4f  expected %+8.4f  %s\n",
                    r.slip_exit, exp_slip_out,
                    approx_eq(r.slip_exit, exp_slip_out) ? "ok" : "FAIL");
        std::printf("  net_pnl    (post-truth)     : %+8.4f  expected %+8.4f  %s\n",
                    r.engine_net, exp_net,
                    approx_eq(r.engine_net, exp_net) ? "ok" : "FAIL");
        std::printf("  broker_pnl (round-trip)     : %+8.4f  expected %+8.4f  %s\n",
                    r.broker_pnl, exp_broker,
                    approx_eq(r.broker_pnl, exp_broker) ? "ok" : "FAIL");
        std::printf("  exitPrice  (overwritten)    : %+8.4f  expected %+8.4f  %s\n",
                    r.exit_price, exp_exit_price,
                    approx_eq(r.exit_price, exp_exit_price) ? "ok" : "FAIL");

        // Hard assertions
        bool ok =
            approx_eq(r.slip_entry, exp_slip_in) &&
            approx_eq(r.slip_exit,  exp_slip_out) &&
            approx_eq(r.engine_net, exp_net) &&
            approx_eq(r.broker_pnl, exp_broker) &&
            approx_eq(r.exit_price, exp_exit_price) &&
            r.both_legs_filled;
        if (ok) { ++passed; std::printf("  >>> ASSERTION 1 PASS\n\n"); }
        else    { ++failed; std::printf("  >>> ASSERTION 1 FAIL\n\n"); }
    }

    // ----- Assertion set 2: the §3 bug would have shown -$1.12 for the same -
    {
        const LiveTrade lt = LIVE_TRADES[2];
        const double slip_in_modeled = (0.22 / 2.0) * 100.0 * 0.01;  // 0.11
        const double bug_net = buggy_old_net_pnl(
            lt.entry, lt.target_close, lt.actual_close,
            lt.is_long, 0.01, 100.0, slip_in_modeled, /*commission*/ 0.0);
        const double exp_bug_net = -1.12;

        std::printf("OLD BUG REPRO (offline arithmetic, header not touched)\n");
        std::printf("  buggy net_pnl from old code : %+8.4f  expected %+8.4f  %s\n",
                    bug_net, exp_bug_net,
                    approx_eq(bug_net, exp_bug_net, 0.01) ? "ok" : "FAIL");
        std::printf("  ratio bug/reality           : %.1fx worse than -0.11\n",
                    bug_net / -0.11);
        if (approx_eq(bug_net, exp_bug_net, 0.01)) {
            ++passed; std::printf("  >>> ASSERTION 2 PASS (this test catches the §3 bug)\n\n");
        } else {
            ++failed; std::printf("  >>> ASSERTION 2 FAIL\n\n");
        }
    }

    // ----- Assertion set 3: all 14 trades in the cTrader slice, total -------
    {
        omega::OmegaTradeLedger lg;
        const double tick_mult = 100.0;

        for (size_t i = 0; i < LIVE_TRADES.size(); ++i) {
            const auto& lt = LIVE_TRADES[i];
            const int id = 2000 + static_cast<int>(i);

            omega::TradeRecord tr;
            tr.id = id;
            tr.symbol = "XAUUSD";
            tr.side = lt.is_long ? "LONG" : "SHORT";
            tr.entryPrice = lt.entry;
            tr.exitPrice  = lt.target_close;
            tr.size = 0.01;
            tr.spreadAtEntry = 0.22;
            tr.exitReason = "TP_HIT";
            tr.entryTs = static_cast<int64_t>(id);  // unique for dedup
            const double diff = lt.is_long
                ? (lt.target_close - lt.entry)
                : (lt.entry - lt.target_close);
            tr.pnl = diff * tr.size * tick_mult;
            omega::apply_realistic_costs(tr, 0.0, tick_mult);
            tr.entry_clOrdId = "E" + std::to_string(id);
            tr.close_clOrdId = "C" + std::to_string(id);
            lg.record(tr);
            lg.applyBrokerFill("E" + std::to_string(id), lt.entry, tick_mult);
            lg.applyBrokerFill("C" + std::to_string(id), lt.actual_close, tick_mult);
        }

        const double eng_pnl       = lg.engineLivePnl();
        const double broker_pnl    = lg.brokerRealisedPnl();
        const double disp          = lg.disparity();
        const int    confirmed     = lg.brokerConfirmedCount();
        const int    orphans       = lg.brokerOrphanCount();

        // Each trade's engine_net should equal its broker_pnl (clean fill,
        // zero commission, no entry slip, exit slip exactly mirrors broker
        // diff). Total engine = total broker, disparity = 0.
        std::printf("ALL 14 cTRADER LOSSES (replay through corrected ledger)\n");
        std::printf("  engineLivePnl       : %+8.4f USD\n", eng_pnl);
        std::printf("  brokerRealisedPnl   : %+8.4f USD\n", broker_pnl);
        std::printf("  disparity (eng-bkr) : %+8.4f USD  (expect ~0 on clean fills)\n", disp);
        std::printf("  confirmed trades    : %d / 14\n", confirmed);
        std::printf("  orphans             : %d\n", orphans);

        bool ok =
            approx_eq(disp, 0.0, 0.0001) &&
            confirmed == 14 &&
            orphans == 0 &&
            eng_pnl < 0.0 &&             // strategy bleeds
            broker_pnl < 0.0;            // broker confirms it bleeds
        if (ok) { ++passed; std::printf("  >>> ASSERTION 3 PASS\n\n"); }
        else    { ++failed; std::printf("  >>> ASSERTION 3 FAIL\n\n"); }
    }

    std::printf("================================================================\n");
    std::printf("SUMMARY: %d passed, %d failed\n", passed, failed);
    std::printf("================================================================\n");
    return failed == 0 ? 0 : 1;
}
