// Hard Stop Architecture v2 -- fixes the two real issues found in simulation:
//
// FIX 1: Position registry keyed by (symbol + engine + entry_ts_ms), not counter.
//   Concurrent main+reload positions each get their own hard stop entry.
//   The close callback carries entry_ts which uniquely identifies which leg closed.
//
// FIX 2: Tombstone pattern for filled stops.
//   reconcile_on_reconnect() sets filled=true but does NOT erase.
//   cancel_before_close() checks filled flag BEFORE deciding to proceed.
//   Tombstones are only cleared after the session ends / position confirmed flat.

#include <cstdio>
#include <string>
#include <unordered_map>
#include <cmath>
#include <cassert>

static int failures = 0;
void check(bool cond, const char* msg) {
    if (!cond) { printf("  !! FAIL: %s\n", msg); failures++; }
    else         printf("  OK: %s\n", msg);
}

// ---------------------------------------------------------------------------
// HardStopManager v2
// ---------------------------------------------------------------------------
struct HardStop {
    std::string clOrdId;
    std::string pos_id;
    bool   is_long_close;  // direction to close (opposite of trade)
    double stop_price;
    double size;
    bool   active  = false;
    bool   filled  = false;  // tombstone: hard stop fired at broker
    bool   cancelled = false;
};

class HardStopManager {
public:
    static constexpr double HARD_STOP_MULT = 3.0;
    static constexpr double HARD_STOP_MIN  = 20.0;
    static constexpr double HARD_STOP_MAX  = 60.0;

    // pos_id = symbol + ":" + engine + ":" + entry_ts_ms (unique per leg)
    std::unordered_map<std::string, HardStop> stops;
    int order_counter = 2000;

    std::string place(const std::string& pos_id, bool trade_is_long,
                      double entry, double sl_pts, double size) {
        double hard_dist = std::max(HARD_STOP_MIN,
                           std::min(HARD_STOP_MAX, sl_pts * HARD_STOP_MULT));
        HardStop hs;
        hs.clOrdId        = "HS-" + std::to_string(order_counter++);
        hs.pos_id         = pos_id;
        hs.is_long_close  = !trade_is_long;
        hs.stop_price     = trade_is_long ? entry - hard_dist : entry + hard_dist;
        hs.size           = size;
        hs.active         = true;
        hs.filled         = false;
        hs.cancelled      = false;
        stops[pos_id]     = hs;
        printf("  [HS-PLACE] %s %s @ %.2f hard_stop=%.2f dist=%.1f sz=%.2f id=%s\n",
               pos_id.c_str(), trade_is_long?"LONG":"SHORT",
               entry, hs.stop_price, hard_dist, size, hs.clOrdId.c_str());
        return hs.clOrdId;
    }

    // Update size after STAIR_PARTIAL (size decreases)
    void update_size(const std::string& pos_id, double remaining) {
        auto it = stops.find(pos_id);
        if (it == stops.end() || !it->second.active) return;
        printf("  [HS-SIZE-UPDATE] %s %.2f -> %.2f lots\n",
               pos_id.c_str(), it->second.size, remaining);
        it->second.size = remaining;
    }

    // Cancel + send market close. Returns false if hard stop already filled (skip market order)
    bool cancel_for_close(const std::string& pos_id) {
        auto it = stops.find(pos_id);
        if (it == stops.end()) {
            // Not found -- check if it was a tombstoned filled stop
            printf("  [HS-CANCEL] %s not found (already cleared)\n", pos_id.c_str());
            return true;
        }
        HardStop& hs = it->second;
        // FIX 2: check tombstone BEFORE anything else
        if (hs.filled) {
            printf("  [HS-GUARD] %s FILLED tombstone -- abort market close (double-fill prevention)\n",
                   pos_id.c_str());
            return false;  // broker already closed it
        }
        if (hs.cancelled) {
            printf("  [HS-GUARD] %s already cancelled\n", pos_id.c_str());
            return true;
        }
        hs.active    = false;
        hs.cancelled = true;
        printf("  [HS-CANCEL] %s id=%s cancelled OK\n", pos_id.c_str(), hs.clOrdId.c_str());
        return true;  // safe to send market close
    }

    // Broker ExecutionReport: hard stop filled (process was dead)
    void on_broker_fill(const std::string& hs_clOrdId, double fill_px) {
        for (auto& [pos_id, hs] : stops) {
            if (hs.clOrdId == hs_clOrdId) {
                hs.filled  = true;
                hs.active  = false;
                printf("  [HS-BROKER-FILL] %s filled @ %.2f -- tombstone set\n",
                       pos_id.c_str(), fill_px);
                return;
            }
        }
        printf("  [HS-BROKER-FILL-WARN] id=%s not found (already cancelled -- normal)\n",
               hs_clOrdId.c_str());
    }

    // On reconnect: check if position was closed by hard stop while offline
    // FIX 2: does NOT erase -- leaves tombstone so cancel_for_close still sees filled=true
    bool reconcile(const std::string& pos_id) {
        auto it = stops.find(pos_id);
        if (it == stops.end()) return false;
        if (it->second.filled) {
            printf("  [HS-RECONCILE] %s hard stop fired offline -- position flat\n",
                   pos_id.c_str());
            // Do NOT erase -- tombstone must survive for cancel_for_close guard
            return true;
        }
        return false;
    }

    // Clean tombstones after session end / confirmed flat
    void clear_tombstone(const std::string& pos_id) {
        stops.erase(pos_id);
    }

    int active_count() const {
        int n = 0;
        for (auto& [k,v] : stops) if (v.active) n++;
        return n;
    }
    int filled_tombstones() const {
        int n = 0;
        for (auto& [k,v] : stops) if (v.filled) n++;
        return n;
    }
};

// ---------------------------------------------------------------------------
// SCENARIO A v2: Full April 2 sequence with proper position ID keying
// pos_id = symbol:engine:entry_ts -- each leg uniquely identified
// ---------------------------------------------------------------------------
void scenario_a_v2() {
    printf("\n=== SCENARIO A v2: Full April 2 -- proper pos_id keying ===\n");
    HardStopManager hsm;

    // Simulate the actual trade sequence using realistic pos_ids
    // (symbol:engine:timestamp -- unique per leg)

    // 01:36 LONG block
    hsm.place("XAUUSD:GoldFlow:013626",  true,  4687.53, 10.05, 0.08);
    hsm.place("XAUUSD:GoldFlow:013636",  true,  4693.52, 10.87, 0.07);  // reload
    // Reload exits first (TRAIL_HIT @ 4693.82)
    bool ok = hsm.cancel_for_close("XAUUSD:GoldFlow:013636");
    check(ok, "A: Reload LONG exits cleanly");
    // Main exits (TRAIL_HIT @ 4696.06)
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:013626");
    check(ok, "A: Main LONG exits cleanly");

    // 01:44 SHORT
    hsm.place("XAUUSD:GoldFlow:014426", false, 4694.34, 11.57, 0.07);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:014426");
    check(ok, "A: SHORT @ 4694 exits (SL_HIT profitable trail)");

    // 02:27 SHORT
    hsm.place("XAUUSD:GoldFlow:022703", false, 4691.23, 4.00, 0.20);
    // STAIR partial: size 0.20 -> 0.16
    hsm.update_size("XAUUSD:GoldFlow:022703", 0.16);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:022703");
    check(ok, "A: SHORT @ 4691 exits TRAIL");

    // 04:05 BIG SHORT block
    hsm.place("XAUUSD:GoldFlow:040559", false, 4672.89, 5.00, 0.16);
    hsm.place("XAUUSD:GoldFlow:040610", false, 4669.69, 5.00, 0.16);  // reload
    // STAIR on main
    hsm.update_size("XAUUSD:GoldFlow:040559", 0.12);
    // Reload exits first (TRAIL @ 4669.64)
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:040610");
    check(ok, "A: Big short reload exits TRAIL");
    // STAIR on main again, then main exits (TRAIL @ 4666.57)
    hsm.update_size("XAUUSD:GoldFlow:040559", 0.11);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:040559");
    check(ok, "A: Big short main exits TRAIL");

    // 04:13 LONG reversal
    hsm.place("XAUUSD:GoldFlow:041328", true, 4671.82, 5.00, 0.16);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:041328");
    check(ok, "A: LONG reversal exits TIME_STOP");

    // 04:19 SHORT block
    hsm.place("XAUUSD:GoldFlow:041940", false, 4667.95, 5.00, 0.16);
    hsm.place("XAUUSD:GoldFlow:042037", false, 4664.85, 5.00, 0.16);  // reload
    hsm.update_size("XAUUSD:GoldFlow:041940", 0.13);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:042037");
    check(ok, "A: Second short reload exits TRAIL");
    hsm.update_size("XAUUSD:GoldFlow:041940", 0.11);
    ok = hsm.cancel_for_close("XAUUSD:GoldFlow:041940");
    check(ok, "A: Second short main exits TRAIL");

    // 11:49 SL cascade (5 LONGs, all size=0.01)
    const char* cascade_ids[] = {
        "XAUUSD:GoldFlow:114934",
        "XAUUSD:GoldFlow:115416",
        "XAUUSD:GoldFlow:115538",
        "XAUUSD:GoldFlow:115849",
        "XAUUSD:GoldFlow:120054"
    };
    double entries[] = {4604.06, 4603.66, 4602.74, 4601.63, 4599.64};
    double sl_pts[]  = {2.06, 3.10, 2.00, 2.00, 2.33};
    for (int i = 0; i < 5; i++) {
        hsm.place(cascade_ids[i], true, entries[i], sl_pts[i], 0.01);
        ok = hsm.cancel_for_close(cascade_ids[i]);
        check(ok, "A: Cascade LONG SL_HIT exits cleanly");
    }

    check(hsm.active_count() == 0,
          "A: All hard stops cancelled -- zero active leftover");
    printf("  Active: %d  Filled-tombstones: %d\n",
           hsm.active_count(), hsm.filled_tombstones());
}

// ---------------------------------------------------------------------------
// SCENARIO B v2: Process crash + reconnect with tombstone guard
// ---------------------------------------------------------------------------
void scenario_b_v2() {
    printf("\n=== SCENARIO B v2: Crash + reconnect tombstone guard ===\n");
    HardStopManager hsm;

    std::string pos_id  = "XAUUSD:GoldFlow:040559";
    std::string hs_id   = hsm.place(pos_id, false, 4672.89, 5.00, 0.16);

    printf("\n  ** CRASH -- hard stop fires @ 4692.89 **\n");
    hsm.on_broker_fill(hs_id, 4692.89);

    printf("\n  ** RECONNECT -- reconcile **\n");
    bool already_flat = hsm.reconcile(pos_id);
    check(already_flat, "B: Reconcile detects hard stop fired");

    printf("\n  ** Reconnect logic tries to send market close (bug path) **\n");
    bool safe = hsm.cancel_for_close(pos_id);
    check(!safe, "B: Tombstone blocks market close after hard stop fill (no double-fill)");

    printf("\n  ** Session end: clear tombstone **\n");
    hsm.clear_tombstone(pos_id);
    check(hsm.filled_tombstones() == 0, "B: Tombstone cleared at session end");
}

// ---------------------------------------------------------------------------
// SCENARIO C v2: Concurrent reload -- verify independent stops, size tracking
// ---------------------------------------------------------------------------
void scenario_c_v2() {
    printf("\n=== SCENARIO C v2: Concurrent main + reload, STAIR size tracking ===\n");
    HardStopManager hsm;

    // Main entry
    hsm.place("XAUUSD:GoldFlow:040559", false, 4672.89, 5.00, 0.16);
    // Reload 11s later
    hsm.place("XAUUSD:GoldFlow:040610", false, 4669.69, 5.00, 0.16);

    check(hsm.active_count() == 2, "C: Two independent hard stops");

    // Stair on MAIN
    hsm.update_size("XAUUSD:GoldFlow:040559", 0.12);

    // Reload exits (TRAIL)
    bool ok = hsm.cancel_for_close("XAUUSD:GoldFlow:040610");
    check(ok && hsm.active_count() == 1, "C: Reload exits, main still protected");

    // Now process crashes while main is still open
    std::string main_hs = hsm.stops["XAUUSD:GoldFlow:040559"].clOrdId;
    printf("\n  ** CRASH while main still open (size=0.12 after stair) **\n");
    hsm.on_broker_fill(main_hs, 4692.89);

    double exposure = 0.12 * 20.0 * 100;  // 0.12 lots * 20pts * $100/pt
    printf("  Exposure capped: $%.2f (0.12 lots x 20pts) -- not original 0.16\n", exposure);
    check(hsm.stops["XAUUSD:GoldFlow:040559"].size == 0.12,
          "C: Hard stop size correctly updated to remaining after STAIR");

    // Reconnect
    bool flat = hsm.reconcile("XAUUSD:GoldFlow:040559");
    check(flat, "C: Reconnect detects correct position was closed");

    // Tombstone blocks spurious re-close
    bool safe = hsm.cancel_for_close("XAUUSD:GoldFlow:040559");
    check(!safe, "C: Tombstone blocks any spurious re-close on reconnect");
}

// ---------------------------------------------------------------------------
// SCENARIO D v2: Hard stop on the March 30 0.50-lot position
// Validates HARD_STOP_MAX caps exposure on large-size trades
// ---------------------------------------------------------------------------
void scenario_d_v2() {
    printf("\n=== SCENARIO D v2: Large size (0.50 lots) -- MAX cap validation ===\n");
    HardStopManager hsm;

    // sl_pts=0.66, size=0.50
    // Dist = max(20, min(60, 0.66*3=1.98)) = 20
    hsm.place("XAUUSD:GoldFlow:MAR30A", true, 4527.93, 0.66, 0.50);
    double hs_px = 4527.93 - 20.0;

    printf("  Hard stop at %.2f -- worst-case loss = $%.0f\n",
           hs_px, 20.0 * 0.50 * 100.0);
    printf("  Compare: uncapped 50pt gap = $%.0f\n", 50.0 * 0.50 * 100.0);

    check(hsm.stops["XAUUSD:GoldFlow:MAR30A"].stop_price == hs_px,
          "D: Hard stop at correct price (entry - HARD_STOP_MIN)");

    // Another large trade where sl_pts*3 > HARD_STOP_MIN but < MAX
    // sl_pts=10, size=0.16 -> dist = max(20, min(60, 30)) = 30
    hsm.place("XAUUSD:GoldFlow:BIG10", false, 4694.34, 11.57, 0.07);
    double expected_dist = std::max(20.0, std::min(60.0, 11.57 * 3.0));  // = 34.7
    printf("  sl_pts=11.57 -> dist=max(20, min(60, 34.7))=%.1f\n", expected_dist);
    check(std::fabs(hsm.stops["XAUUSD:GoldFlow:BIG10"].stop_price
                    - (4694.34 + expected_dist)) < 0.1,
          "D: Large SL correctly uses 3x multiplier (not clamped to MIN)");

    // Normal closes
    hsm.cancel_for_close("XAUUSD:GoldFlow:MAR30A");
    hsm.cancel_for_close("XAUUSD:GoldFlow:BIG10");
    check(hsm.active_count() == 0, "D: All cleared cleanly");
}

int main() {
    scenario_a_v2();
    scenario_b_v2();
    scenario_c_v2();
    scenario_d_v2();

    printf("\n========================================================\n");
    if (failures == 0)
        printf("ALL SCENARIOS PASS -- architecture is sound\n");
    else
        printf("RESULTS: %d failure(s)\n", failures);
    printf("========================================================\n");
    return failures;
}
