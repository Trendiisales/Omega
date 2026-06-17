// pump_exit_unittest.cpp — faithful behavioural test of the REAL PumpScalpEngine
// exit logic. Answers the operator's question (2026-06-18): "it was set up to
// only win — how are we losing, and why did nothing close?" Builds the actual
// engine class (no mock), restores a position, drives on_price()/watchdog(), and
// asserts the booked PnL + exit reason for each scenario.
//
// Build (Mac/Linux, header-only deps — no winsock):
//   g++ -std=c++17 -Iinclude backtest/pump_exit_unittest.cpp -o /tmp/pumptest && /tmp/pumptest
#include "PumpScalpEngine.hpp"
#include <cstdio>
#include <string>
#include <vector>

using omega::PumpScalpEngine;
using omega::PositionSnapshot;
using omega::TradeRecord;

static int g_fail = 0;

// Build an engine wired to the LIVE manager defaults (PumpScalpManager configure()).
static PumpScalpEngine make_engine(std::vector<TradeRecord>& sink) {
    PumpScalpEngine e;
    e.symbol="TEST"; e.engine_name="PumpScalp_3m";
    e.TF_SEC=180; e.DAY_GATE_PCT=100.0; e.TRAIL_PCT=2.0; e.HARD_PCT=6.0;
    e.BE_ARM_PCT=2.0; e.BE_FLOOR_PCT=2.0; e.NOTIONAL_USD=1000.0; e.SLIP_PCT=1.0;
    e.MAXHOLD_SEC=5*180; e.shadow_mode=true;
    e.on_trade_record=[&sink](const TradeRecord& tr){ sink.push_back(tr); };
    e.init();
    return e;
}

// Inject a LONG position at `entry` (shares = $1000/entry, like NOTIONAL_USD).
static void open_long(PumpScalpEngine& e, double entry, int64_t entry_ts_sec) {
    PositionSnapshot ps; ps.symbol="TEST"; ps.side="LONG";
    ps.size=1000.0/entry; ps.entry=entry; ps.entry_ts=entry_ts_sec;
    e.persist_restore(ps);
}

static void check(const char* name, bool ok, const std::string& detail) {
    printf("  [%s] %s — %s\n", ok?"PASS":"FAIL", name, detail.c_str());
    if (!ok) ++g_fail;
}

int main() {
    const int64_t T0 = 1700000000;          // entry epoch sec
    const int64_t ms0 = T0*1000;             // entry epoch ms

    // ── A: ARMED then FADE — ran +3% (arms BE floor), faded to floor.
    //   This is the "win/flat" the BE-lock promises: a runner that turns can NOT
    //   become a real loss. Expect ~break-even (slightly neg from the slip model).
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(103.0, ms0+5000);         // +3% -> arms BE floor at 102
        e.on_price(102.0, ms0+10000);        // fades to floor -> TRAIL exit at 102
        bool ok = tr.size()==1 && tr[0].exitReason=="TRAIL" && tr[0].pnl > -0.5 && tr[0].pnl < 0.5;
        char d[160]; snprintf(d,sizeof d,"armed+faded -> pnl=$%.3f reason=%s (BE floor held it ~flat)",
            tr.empty()?0.0:tr[0].pnl, tr.empty()?"<none>":tr[0].exitReason.c_str());
        check("armed-fade ≈ break-even", ok, d);
    }

    // ── B: IMMEDIATE FADE, never reaches +2% — BE floor NEVER arms. The trade is
    //   unprotected and exits at the trailing/hard stop = a real ~4% LOSS.
    //   THIS is the loser bucket. It is BY DESIGN, not a bug: the BE-lock only
    //   protects trades that first run in your favour.
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(99.0, ms0+5000);          // never armed (peak=100, <102)
        e.on_price(98.0, ms0+10000);         // hits trail stop at 98 -> LOSS
        bool ok = tr.size()==1 && tr[0].exitReason=="TRAIL" && tr[0].pnl < -10.0;
        char d[180]; snprintf(d,sizeof d,"never +2%% -> pnl=$%.2f reason=%s (UNPROTECTED, by design, ~-4%%)",
            tr.empty()?0.0:tr[0].pnl, tr.empty()?"<none>":tr[0].exitReason.c_str());
        check("unarmed fade = loss by design", ok, d);
    }

    // ── C: FROZEN FEED (the 2026-06-18 bug). Position was WINNING (+5%, armed),
    //   then the feed dies — no more ticks. PRE-FIX: on_price never runs again ->
    //   position hangs open forever (reproduce). The watchdog (heartbeat-driven)
    //   then force-closes at last px -> BANKS the win instead of freezing.
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(105.0, ms0+5000);         // +5% armed, last good tick at +5s
        // ...feed dies. No further ticks. Reproduce the freeze:
        bool frozen = e.has_open_position() && tr.empty();
        check("freeze reproduced (no tick -> no exit)", frozen, "position still OPEN, 0 trades booked");
        // Heartbeat fires 121s later with no fresh tick -> STALE force-close at 105.
        e.watchdog(ms0+5000+121000, 120000);
        bool ok = !e.has_open_position() && tr.size()==1 && tr[0].exitReason=="STALE_WD" && tr[0].pnl>0;
        char d[180]; snprintf(d,sizeof d,"watchdog -> pnl=$%.2f reason=%s (dead feed no longer freezes; win locked)",
            tr.empty()?0.0:tr[0].pnl, tr.empty()?"<none>":tr[0].exitReason.c_str());
        check("watchdog STALE force-close banks the win", ok, d);
    }

    // ── D: MAXHOLD via watchdog — a quiet trade that never hit a stop must still
    //   flatten at the 15-min time-stop, EVEN IF the feed has also gone quiet
    //   (the time-stop used to be tick-gated too).
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(101.0, ms0+5000);         // drifts, never stops out
        e.watchdog(ms0+901000, 120000);      // 901s > MAXHOLD 900s -> TIME_WD
        bool ok = !e.has_open_position() && tr.size()==1 && tr[0].exitReason=="TIME_WD";
        char d[160]; snprintf(d,sizeof d,"held >15min -> reason=%s pnl=$%.2f (time-stop now feed-independent)",
            tr.empty()?"<none>":tr[0].exitReason.c_str(), tr.empty()?0.0:tr[0].pnl);
        check("MAXHOLD enforced by watchdog", ok, d);
    }

    // ── E: control — while ticks FLOW, the watchdog is a no-op (must not steal a
    //   live trade from the normal trail logic).
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(104.0, ms0+5000);
        e.watchdog(ms0+5000+1000, 120000);   // only 1s since last tick, < MAXHOLD
        bool ok = e.has_open_position() && tr.empty();
        check("watchdog no-op while feed is live", ok, "position untouched (fresh tick, under MAXHOLD)");
    }

    // ── F: LEDGER MARK — a frozen-feed position must report its REAL standing
    //   cost via persist_save() (what the GUI/live_trades panel reads), NOT +$0.
    //   A still-OPEN position's last seen px is ABOVE its stop (else it'd have
    //   closed) -> last px 99 (-1%) just under the run. current must be 99, the
    //   unrealized must show the real ~-$30 mark, not the fake +$0 / now=0.00.
    //   (CAVEAT: if the stock kept dropping AFTER the feed died, the TRUE loss is
    //   worse than this last-known mark — unknowable without a live feed, which is
    //   why the watchdog books fast at 120s to cap the unseen drift.)
    {
        std::vector<TradeRecord> tr; auto e=make_engine(tr); open_long(e,100.0,T0);
        e.on_price(99.0, ms0+5000);          // -1% (holds, stop is 98), then feed dies
        PositionSnapshot snap;
        bool saved = e.persist_save("PumpScalp_3m","TEST",snap);
        bool ok = saved && snap.current>98.5 && snap.current<99.5
                  && snap.unrealized_pnl < -20.0 && snap.unrealized_pnl > -40.0;
        char d[200]; snprintf(d,sizeof d,"frozen mark: current=%.2f unrealized=$%.2f (was a fake +$0 / now=0.00)",
            snap.current, snap.unrealized_pnl);
        check("ledger shows REAL cost on stale feed", ok, d);
    }

    printf("\n%s — %d failure(s)\n", g_fail==0?"ALL PASS":"FAILURES", g_fail);
    return g_fail==0 ? 0 : 1;
}
