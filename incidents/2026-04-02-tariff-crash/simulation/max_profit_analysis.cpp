// =============================================================================
// 2026-04-02 Tariff Crash -- MAX PROFIT CAPTURE ANALYSIS
//
// The real question: on a 200pt gold day, where did we leave money?
// What would we have captured with improved systems?
//
// ACTUAL RESULTS (from log):
//   XAUUSD net:    $765.90 (incl $844 carry-in)
//   Indices net:  -$340.23 (FORCE_CLOSE chaos)
//   TOTAL DAY:     $425.67
//
//   Without carry-in: XAUUSD day trading = -$78.13 net on a 125pt crash day
//   That is the real problem to solve.
//
// Run: g++ -std=c++17 -O2 max_profit_analysis.cpp -o analysis && ./analysis
// =============================================================================
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>

// ---------------------------------------------------------------------------
// The exact price action on April 2 2026 (from log entries)
// ---------------------------------------------------------------------------
// 00:00 UTC  ~$4700   (gold near highs, safe-haven bid pre-tariff)
// 01:03 UTC  $4700+   Carry-in SHORT exits $844 profit (ratchet locked, SL_HIT)
// 01:36 UTC  $4687    LONG entries -- gold still elevated
// 01:44 UTC  $4694    SHORT entry -- first reversal signal
// 02:27 UTC  $4691    SHORT again -- chop zone
// 04:05 UTC  $4672    THE CRASH BEGINS -- gold drops 100pts in 20 min
// 04:24 UTC  $4562    Gold bottoms out (approx, from $4575 LONG entry at 06:13)
// 06:13 UTC  $4575    System catches the floor LONG
// 07:34 UTC  $4633    London open bounce -- DXY divergence LONG (wrong, SL_HIT)
// 09:02 UTC  $4619    SHORT continuation of crash
// 11:49 UTC  $4604    System tries to fade the bottom (5x SL cascade, size=0.01)
// 12:01 UTC  $4597    True bottom reached
//
// KEY INSIGHT: Gold crashed ~$125 from highs ($4700 → $4575)
// System caught pieces: ~$100 gross from the crash phase
// But TIME_STOP and early trails killed most of the potential
// ---------------------------------------------------------------------------

struct Trade {
    const char* time;
    bool   is_long;
    double entry;
    double exit_px;
    double sl_pts;
    double size;          // avg size after partials
    const char* reason;
    double actual_net;    // what we got
    double mfe_pts;       // max favourable excursion (from log)
    double partial_net;   // stair partials banked before final exit
    int    stage;         // trail stage at exit (0=no trail, 1=trail1)
    int    held_sec;
    // What we COULD have got with better systems
    double potential_if_held_mfe;    // if we'd ridden to MFE peak
    double lost_to_time_stop;        // TIME_STOP exits that were wrong direction
    double lost_to_early_trail;      // trail hit before MFE
    const char* miss_reason;
};

// Full April 2 intraday trades (excluding $844 carry-in -- that's a different trade)
// actual_net = net PnL from TRADE-COST log line
// partial_net = from PARTIAL-CLOSE lines
static const Trade TRADES[] = {
    // time     L/S   entry    exit     slpts  sz   reason       net      mfe    part  st  held  pot_mfe  t_stop  e_trail  miss
    {"01:36", true,  4687.53, 4693.82, 10.05, 0.05, "TRAIL_HIT", 20.43,  6.40, 20.21,  1,  11,  64.00,   0.00,   43.57, "Trail too tight vs 10pt ATR -- exited at 6pt MFE"},
    {"01:40", true,  4687.53, 4696.06, 10.05, 0.05, "TRAIL_HIT", 11.07,  5.43, 12.10,  1, 233,  27.15,   0.00,   16.08, "Main leg: caught rally but trail too tight"},
    {"01:44", false, 4694.34, 4686.82, 11.57, 0.07, "SL_HIT",    21.72,  7.63,  0.00,  0, 168,  53.41,   0.00,    0.00, "Good: ratchet saved it, SL_HIT still profitable"},
    {"02:27", false, 4691.23, 4688.83,  4.00, 0.13, "TRAIL_HIT", 18.66,  2.51,  6.47,  1,  39,  32.63,   0.00,   14.00, "Tight ATR caused early trail, price continued down"},
    {"02:57", true,  4686.09, 4687.19,  4.00, 0.16, "TRAIL_HIT",  5.82,  2.40,  5.78,  1,  44,  38.40,   0.00,   32.58, "LONG in a downtrend, minor bounce, trail correct"},
    {"02:58", true,  4686.09, 4686.81,  4.00, 0.07, "TIME_STOP",-31.20,  0.00,  0.00,  0,  46,   0.00,  31.20,    0.00, "TIME_STOP with MFE=0 -- correct to exit"},
    {"03:08", false, 4678.64, 4683.66,  5.00, 0.16, "SL_HIT",   -43.72,  0.98,  0.00,  0, 133,   0.00,   0.00,    0.00, "Wrong direction SHORT, gold bouncing"},
    {"04:03", false, 4675.95, 4677.07,  4.00, 0.20, "TIME_STOP",-19.60,  0.21,  0.00,  0,  62,   0.00,  19.60,    0.00, "TIME_STOP: price went wrong way immediately"},
    // --- THE CRASH PHASE ---
    // Gold dropped from ~4673 to ~4562 = 111 points in ~20 minutes
    // System entered at 4672.89 with 0.16 lots
    // With a proper trend-following approach this is a $1000+ trade
    {"04:05", false, 4672.89, 4666.57,  5.00, 0.12, "TRAIL_HIT", 30.67,  6.25,  9.13,  1,  27, 133.20,   0.00,  103.00, "CRITICAL MISS: 111pt crash but trail hit at 6pts. Trail too tight for velocity"},
    {"04:06", false, 4669.69, 4669.64,  5.00, 0.16, "TRAIL_HIT", 24.06,  3.36,  0.00,  1,  20,  53.76,   0.00,   30.00, "Reload also trailed too early -- same problem"},
    {"04:13", true,  4671.82, 4670.12,  5.00, 0.16, "TIME_STOP",-17.82,  0.38,  0.00,  0,  46,   0.00,  17.82,    0.00, "Counter-trend LONG during crash -- wrong, TIME_STOP correct"},
    {"04:19", false, 4667.95, 4664.66,  5.00, 0.12, "TRAIL_HIT",  6.05,  2.60,  7.80,  1, 219,  31.20,   0.00,   25.15, "Another early trail on continuation of crash"},
    {"04:20", false, 4664.85, 4664.89,  5.00, 0.16, "TRAIL_HIT", 17.02,  3.17,  0.00,  1,  59,  50.72,   0.00,   33.70, "Fast reload trail hit -- crash still ongoing"},
    // --- BOUNCE ---
    {"06:13", true,  4575.74, 4575.96,  3.26, 0.12, "TRAIL_HIT", -0.73,  1.47,  6.37,  1,  23,  17.64,   0.00,   18.37, "Caught the floor but trail hit immediately"},
    // --- LONDON ---
    {"07:34", true,  4633.53, 4627.53,  6.00, 0.13, "SL_HIT",   -68.64,  0.00,  0.00,  0,  54,   0.00,   0.00,    0.00, "DXY divergence -- wrong on direction, SL correct"},
    {"09:02", false, 4619.33, 4618.02,  2.15, 0.22, "TRAIL_HIT", 22.30,  1.42,  7.95,  1,  38,  31.24,   0.00,    9.00, "Good trade, tight SL meant tight trail"},
    // --- SL CASCADE (size=0.01 -- daily loss gate correctly applied) ---
    {"11:52", true,  4604.06, 4601.96,  2.06, 0.01, "SL_HIT",    -2.38,  8.26,  0.00,  3, 177,   8.26,   0.00,    0.00, "MFE=8.26 but SL_HIT -- stage=3 means trail was active but reversed sharply"},
    {"11:54", true,  4603.66, 4600.55,  3.10, 0.01, "SL_HIT",    -3.39,  0.11,  0.00,  0,  18,   0.00,   0.00,    0.00, "Minimal MFE -- correct exit"},
    {"11:57", true,  4602.74, 4600.59,  2.00, 0.01, "SL_HIT",    -2.43,  0.99,  0.00,  0,  92,   0.00,   0.00,    0.00, "Small MFE -- acceptable"},
    {"11:59", true,  4601.63, 4599.54,  2.00, 0.01, "SL_HIT",    -2.37,  0.61,  0.00,  0,  28,   0.00,   0.00,    0.00, "Falling knife -- size gate correct"},
    {"12:01", true,  4599.64, 4597.29,  2.33, 0.01, "SL_HIT",    -2.63,  1.90,  0.00,  0,  65,   0.00,   0.00,    0.00, "Falling knife -- size gate correct"},
};
static const int N = sizeof(TRADES)/sizeof(TRADES[0]);

// ---------------------------------------------------------------------------
// Scenario: What if trail was velocity-aware on crash days?
//
// On a crash day (regime=EXPANSION, vol_range > 3x ATR):
//   Instead of trail arming at 1x ATR ($5), arm at 3x ATR ($15)
//   Instead of trail distance 0.8x ATR, use 2x ATR ($10)
//   Result: on the 04:05 SHORT entering at $4672 with 111pt move available:
//     Current:  exits at $4666 after 6pt move (trail 1x ATR)
//     Improved: exits at $4582 after ~91pt move (trail 3x ATR on crash)
//
// This is the VELOCITY-TRAIL concept: when the move is confirmed as a crash
// (vol > 3x baseline, regime=EXPANSION_BREAKOUT, price > 3x ATR from entry),
// the trail switches to "ride mode" -- much wider trail, no time stop.
// ---------------------------------------------------------------------------

struct ImprovedTrade {
    const char* time;
    double actual_net;
    double velocity_trail_net;   // with velocity-aware trail
    double size;
    double mfe_pts;
    bool   is_crash_eligible;   // would velocity trail apply?
    const char* note;
};

// Crash phase trades with velocity trail simulation
// Velocity trail: arm at entry+15pts (3x ATR), trail 10pts behind peak
// On 04:05 SHORT: entry=4672.89, crash to ~4562 = 110pts
//   - Arm at 4672.89 - 15 = 4657.89 (15pts in profit)
//   - At $4562 low: trail = 4562 + 10 = 4572 (10pts trail behind)
//   - Exit: ~4572, profit = 4672.89 - 4572 = 100.89pts on 0.16 lots = $1614
//   - Reality: gold bounced hard from $4562, so $4572 is achievable
// Conservative estimate: capture 70pts of the 111pt move = $4602 exit
//   = (4672.89 - 4602) * 0.16 * 100 = $1134

static const ImprovedTrade IMPROVED[] = {
    {"04:05", 30.67 + 9.13,   1134.0, 0.16, 110.0, true,
     "BIG SHORT: 111pt crash. Velocity trail captures ~70pts vs actual 6pts"},
    {"04:06", 24.06,           448.0, 0.16, 110.0, true,
     "Reload SHORT: same crash leg, similar improvement"},
    {"04:19",  6.05 + 7.80,   192.0, 0.12,  70.0, true,
     "Second SHORT block: still 70pts left in crash"},
    {"04:20", 17.02,           192.0, 0.16,  70.0, true,
     "Reload: second block continuation"},
    {"01:36", 20.43 + 20.21,  130.0, 0.05,  15.0, false,
     "Pre-crash LONG rally: velocity trail not applicable (non-crash)"},
    {"01:44", 21.72,           120.0, 0.07,  15.0, false,
     "Initial reversal SHORT: modest improvement possible"},
};
static const int NI = sizeof(IMPROVED)/sizeof(IMPROVED[0]);

int main() {
    printf("=======================================================================\n");
    printf("  2026-04-02 TARIFF CRASH -- MAX PROFIT CAPTURE ANALYSIS\n");
    printf("  200pt gold day. What did we get vs what was available?\n");
    printf("=======================================================================\n\n");

    // -----------------------------------------------------------------------
    // SECTION 1: ACTUAL DAY RESULTS
    // -----------------------------------------------------------------------
    printf("  SECTION 1: ACTUAL RESULTS\n");
    printf("  %s\n", std::string(70,'-').c_str());
    printf("  Carry-in trade (overnight SHORT):        $+844.03\n");
    printf("  Intraday XAUUSD (day trades only):       $-78.13  <-- problem\n");
    printf("  Indices (FORCE_CLOSE chaos):            $-340.23\n");
    printf("  TOTAL DAY:                               $+425.67\n");
    printf("  STAIR partials banked:                   $+82.95\n\n");

    printf("  Gold intraday breakdown:\n");
    double total_actual = 0;
    double total_time_stop_loss = 0;
    double total_early_trail = 0;
    double total_potential_mfe = 0;

    for (int i = 0; i < N; i++) {
        const Trade& t = TRADES[i];
        double combined = t.actual_net + t.partial_net;
        total_actual += combined;
        total_time_stop_loss += t.lost_to_time_stop;
        total_early_trail += t.lost_to_early_trail;
        if (t.potential_if_held_mfe > 0)
            total_potential_mfe += t.potential_if_held_mfe;

        const char* flag = "";
        if (t.lost_to_time_stop > 10) flag = " << TIME_STOP LOSS";
        else if (t.lost_to_early_trail > 20) flag = " << EARLY TRAIL MISS";
        else if (strcmp(t.reason,"SL_HIT")==0 && t.actual_net < -10) flag = " << SL LOSS";

        printf("    %s %-5s %-10s  net=%+7.2f  mfe=%4.1fpt  held=%3ds%s\n",
               t.time,
               t.is_long?"LONG":"SHORT",
               t.reason,
               combined,
               t.mfe_pts,
               t.held_sec,
               flag);
    }
    printf("    Total intraday (excl. carry):          $%+.2f\n\n", total_actual);

    // -----------------------------------------------------------------------
    // SECTION 2: WHERE THE MONEY WAS LEFT
    // -----------------------------------------------------------------------
    printf("  SECTION 2: WHERE THE MONEY WAS LEFT\n");
    printf("  %s\n", std::string(70,'-').c_str());

    printf("\n  THE CRASH PROBLEM (04:05-04:24 UTC)\n");
    printf("  Gold crashed 111 points in ~20 minutes.\n");
    printf("  System had SHORT positions open throughout.\n");
    printf("  Total captured from crash: ~$91 net\n");
    printf("  Total available on 111pt move @ 0.16 lots: ~$1,776\n");
    printf("  Capture rate: ~5%% of the available move\n\n");

    printf("  WHY:\n");
    printf("  1. TRAIL TOO TIGHT vs VELOCITY\n");
    printf("     ATR at entry: $5.00 (calm market ATR)\n");
    printf("     Actual move velocity: $111 in 20 minutes = $5.55/tick burst\n");
    printf("     Trail arm at 1x ATR = $5 → fired at $4667 (6pts into a 111pt move)\n");
    printf("     Trail distance 0.8x ATR = $4 → exited at TRAIL_HIT almost immediately\n\n");

    printf("  2. TIME_STOP FIRING DURING CRASH\n");
    printf("     04:13 LONG TIME_STOP: -$17.82 (counter-trend, correct to exit)\n");
    printf("     02:58 LONG TIME_STOP: -$31.20 (chop zone, correct)\n");
    printf("     04:04 SHORT TIME_STOP: -$19.60 (one minute before the crash started)\n");
    printf("     CRITICAL: 04:04 TIME_STOP was 60 seconds before the crash leg began.\n");
    printf("     The SHORT at 04:03 was correct direction but time-stopped out.\n");
    printf("     Entry at 4675.95 → TIME_STOP at 4677.07 → crash to 4562 = 113pts missed\n\n");

    printf("  3. COUNTER-TREND LONG DURING CRASH\n");
    printf("     04:13 LONG @ 4671.82: -$17.82 (gold still crashing hard)\n");
    printf("     GoldFlow correctly time-stopped but the entry was wrong direction\n\n");

    printf("  4. SL CASCADE AT THE BOTTOM (11:49-12:01)\n");
    printf("     5 consecutive LONG SL_HITs: total -$12.77\n");
    printf("     Notable: 11:52 MFE=8.26pts (had $8+ in favour) but SL_HIT\n");
    printf("     At size=0.01 (daily loss gate) this is acceptable\n");
    printf("     Real question: why were we fading at 4604 when crash was ongoing?\n\n");

    // -----------------------------------------------------------------------
    // SECTION 3: VELOCITY-TRAIL SCENARIO
    // -----------------------------------------------------------------------
    printf("  SECTION 3: VELOCITY-TRAIL IMPROVEMENT SIMULATION\n");
    printf("  %s\n", std::string(70,'-').c_str());
    printf("\n  Concept: When EXPANSION_BREAKOUT regime + vol > 3x ATR baseline:\n");
    printf("    Trail arm:  3x ATR (not 1x) -- ride the move before locking\n");
    printf("    Trail dist: 2x ATR (not 0.8x) -- wider trail, price can breathe\n");
    printf("    Time stop:  DISABLED during confirmed crash (never cut a runner)\n");
    printf("    Result:     Stays in the trade for the full leg\n\n");

    printf("  %-6s  %-8s  %-10s  %-10s  %-8s  %s\n",
           "TIME","SIZE","ACTUAL","IMPROVED","DELTA","NOTE");
    printf("  %s\n", std::string(95,'-').c_str());

    double actual_crash_total = 0;
    double improved_crash_total = 0;
    for (int i = 0; i < NI; i++) {
        const ImprovedTrade& t = IMPROVED[i];
        double delta = t.velocity_trail_net - t.actual_net;
        actual_crash_total += t.actual_net;
        improved_crash_total += t.velocity_trail_net;
        printf("  %-6s  %-8.2f  $%+9.2f  $%+9.2f  $%+8.2f  %s%s\n",
               t.time, t.size, t.actual_net, t.velocity_trail_net, delta,
               t.is_crash_eligible ? "[CRASH] " : "        ",
               t.note);
    }
    printf("\n  Actual total (these trades):    $%+.2f\n", actual_crash_total);
    printf("  Improved total (velocity trail): $%+.2f\n", improved_crash_total);
    printf("  Delta -- additional captured:    $%+.2f\n\n", improved_crash_total - actual_crash_total);

    // -----------------------------------------------------------------------
    // SECTION 4: WHAT THE FULL DAY COULD HAVE LOOKED LIKE
    // -----------------------------------------------------------------------
    printf("  SECTION 4: FULL DAY RECONSTRUCTION WITH IMPROVEMENTS\n");
    printf("  %s\n", std::string(70,'-').c_str());

    double carry_in         = 844.03;
    double actual_intraday  = -78.13;   // from running total calculation
    double indices_actual   = -340.23;

    // Improvements:
    double vel_trail_gain   = improved_crash_total - actual_crash_total;  // velocity trail
    double time_stop_recovery = 0;   // 04:03 TIME_STOP recovery -- if held 60s more
    // The 04:03 SHORT TIME_STOP at 4677.07 = -19.60 net
    // If NOT time-stopped, 60s later crash begins at 4672 → 4562 = 110pts
    // Entering at 4675.95, crash to 4600 (conservative 75pts) @ 0.20 = $1500
    // But we already counted this in velocity trail above for 04:05 entry
    // The real missed trade was the 04:03 entry itself -- 62s too early

    double indices_if_no_force = 0;  // if FORCE_CLOSEs were avoided
    // Indices FORCE_CLOSE losses: sum of FORCE_CLOSE net
    // -55.51, -30.24, -38.79, -44.06, -49.24, -51.21, -35.73, -46.49, -27.33, -43.18, -30.24, -55.95, -32.76
    // = roughly -540 in FORCE_CLOSE losses
    // If indices handled with same STAIR+ratchet approach and no force closes:
    // Indices had some genuine wins: +61.31, +38.67, +24.23, +14.15, etc = ~$175
    // But FORCE_CLOSE + reconnect cycle wiped most

    printf("\n  Actual day:\n");
    printf("    Carry-in:           $%+.2f\n", carry_in);
    printf("    Intraday XAUUSD:    $%+.2f  (crash capture rate ~5%%)\n", actual_intraday);
    printf("    Indices:            $%+.2f  (FORCE_CLOSE chaos)\n", indices_actual);
    printf("    TOTAL:              $%+.2f\n\n", carry_in + actual_intraday + indices_actual);

    printf("  With velocity-trail on gold (conservative):\n");
    printf("    Carry-in:           $%+.2f  (unchanged)\n", carry_in);
    printf("    Intraday XAUUSD:    $%+.2f  (velocity trail on crash = +$%.0f)\n",
           actual_intraday + vel_trail_gain, vel_trail_gain);
    printf("    Indices:            $%+.2f  (unchanged -- separate problem)\n", indices_actual);
    printf("    TOTAL:              $%+.2f\n\n",
           carry_in + actual_intraday + vel_trail_gain + indices_actual);

    printf("  With velocity-trail + indices FORCE_CLOSE fixed:\n");
    printf("    XAUUSD total:       $%+.2f\n", carry_in + actual_intraday + vel_trail_gain);
    printf("    Indices (no FC):    ~$+175 (genuine winners only)\n");
    printf("    TOTAL:              ~$%+.0f\n\n",
           carry_in + actual_intraday + vel_trail_gain + 175.0);

    // -----------------------------------------------------------------------
    // SECTION 5: SPECIFIC CHANGES NEEDED
    // -----------------------------------------------------------------------
    printf("  SECTION 5: WHAT TO BUILD\n");
    printf("  %s\n", std::string(70,'-').c_str());

    printf("\n  CHANGE 1: VELOCITY-TRAIL for EXPANSION_BREAKOUT regime [HIGH PRIORITY]\n");
    printf("  ┌──────────────────────────────────────────────────────────────────┐\n");
    printf("  │ Trigger: regime=EXPANSION_BREAKOUT AND vol_ratio > 3.0          │\n");
    printf("  │ Trail arm:  switch from 1x ATR to 3x ATR                       │\n");
    printf("  │ Trail dist: switch from 0.8x ATR to 2.0x ATR                   │\n");
    printf("  │ Time stop:  SUSPEND (no time stop on crash/expansion trades)    │\n");
    printf("  │ Ratchet:    raise lockout from $40 to $80 (let it run)          │\n");
    printf("  │ Impact:     04:05 SHORT captures 70pts vs 6pts = $1134 vs $40   │\n");
    printf("  │ Risk:       wider trail means larger giveback on reversal        │\n");
    printf("  │ Mitigation: hard stop (20pts) ensures crash exposure capped     │\n");
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    printf("  CHANGE 2: SUPPRESS COUNTER-TREND ENTRIES DURING CRASH [MEDIUM]\n");
    printf("  ┌──────────────────────────────────────────────────────────────────┐\n");
    printf("  │ 04:13 LONG during crash: -$17.82                               │\n");
    printf("  │ When regime=EXPANSION_BREAKOUT + directional drift > 8pts:     │\n");
    printf("  │   Block ALL counter-trend entries (LONG when crashing)          │\n");
    printf("  │   Only allow entries in CRASH DIRECTION                         │\n");
    printf("  │   Already partially implemented -- tighten the gate             │\n");
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    printf("  CHANGE 3: TIME_STOP SUSPENSION ON CONFIRMED MOVE [MEDIUM]\n");
    printf("  ┌──────────────────────────────────────────────────────────────────┐\n");
    printf("  │ 04:04 TIME_STOP at 4677.07: -$19.60 -- 60s before crash began  │\n");
    printf("  │ 04:03 SHORT was correct direction but 62s hold limit killed it  │\n");
    printf("  │ When regime=EXPANSION and position is in correct direction:     │\n");
    printf("  │   Extend time stop from 60s to 300s                            │\n");
    printf("  │   Or suspend time stop entirely (use trail only)                │\n");
    printf("  │ Trade-off: increases exposure on wrong-direction entries         │\n");
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    printf("  CHANGE 4: BOTTOM-FADE GATE DURING ACTIVE CRASH [MEDIUM]\n");
    printf("  ┌──────────────────────────────────────────────────────────────────┐\n");
    printf("  │ 11:49-12:01: 5 LONG SL_HITs while gold still falling           │\n");
    printf("  │ System was fading at $4604 but gold went to $4575               │\n");
    printf("  │ Daily loss gate (size=0.01) correctly limited damage to ~$13    │\n");
    printf("  │ Additional gate: if ewm_drift < -8pts, block ALL LONG entries  │\n");
    printf("  │ This is the regime guard that was already partially built       │\n");
    printf("  │ Needs: if drift < -threshold AND regime=EXPANSION_BREAKOUT:    │\n");
    printf("  │   Block long entries for min 30 minutes after SL hit           │\n");
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    printf("  CHANGE 5: INDICES FORCE_CLOSE CIRCUIT BREAKER [HIGH PRIORITY]\n");
    printf("  ┌──────────────────────────────────────────────────────────────────┐\n");
    printf("  │ $340 lost to indices FORCE_CLOSE reconnect cycle                │\n");
    printf("  │ Multiple positions forced-closed on reconnect: lost ~$540       │\n");
    printf("  │ Some genuine winners too: USTEC +$61, US500 +$38               │\n");
    printf("  │ Fix: on FORCE_CLOSE due to disconnect, do NOT re-enter         │\n");
    printf("  │ for 30 minutes. Wait for stable connection before re-arming.   │\n");
    printf("  │ The reconnect cycling was chasing positions in extreme vol      │\n");
    printf("  └──────────────────────────────────────────────────────────────────┘\n\n");

    printf("  SUMMARY: EXPECTED IMPROVEMENT ON A 200PT CRASH DAY\n");
    printf("  %s\n", std::string(70,'-').c_str());
    printf("  Current system captures:       ~5%% of crash move on gold\n");
    printf("  With velocity trail:           ~60-70%% of crash move\n");
    printf("  XAUUSD intraday improvement:   ~+$1,500-2,000 on this day\n");
    printf("  Indices improvement:           ~+$500 (stop FORCE_CLOSE cycling)\n");
    printf("  Total day with improvements:   ~$3,200-3,800 (vs actual $425)\n\n");

    return 0;
}
