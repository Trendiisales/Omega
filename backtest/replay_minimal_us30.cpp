// =============================================================================
// replay_minimal_us30.cpp -- Replay test for MinimalH4US30Breakout engine.
//
// PURPOSE:
//   Drives the PRODUCTION engine (include/MinimalH4US30Breakout.hpp) through a
//   tick-by-tick replay of 2yr Tickstory DJ30.F data and verifies that the
//   live engine reproduces signal timing & trade count consistent with the
//   sweep harness htf_bt_multi.cpp.
//
//   The engine and the sweep harness implement the same strategy but use
//   DIFFERENT execution models:
//
//   Sweep harness (htf_bt_multi.cpp):
//     - Builds H4 bars from mid + tracks intrabar bid/ask high/low
//     - Entry at b.close +/- half_spread (NOT a live bid/ask)
//     - Exit at exact SL/TP price (NOT a live bid/ask)
//     - PnL = (exit - entry) * point_value * size
//
//   Production engine (MinimalH4US30Breakout.hpp):
//     - Builds H4 bars from mid using on_tick() callbacks
//     - Entry at live ask (long) / bid (short) on the tick that crosses
//       the H4 bar boundary
//     - Exit at live bid (long) / ask (short) when SL/TP crosses on tick
//     - PnL = (exit - entry) * size * dollars_per_point
//
//   These will produce nearby but not identical trade counts and PnL.
//   Engine entries fire on the FIRST TICK after bar boundary cross, harness
//   on bar close, so signals ~minutes apart but directionally identical.
//
// EXPECTED FROM SWEEP (htf_bt_US30_results.txt, D=10 SL=1.0 TP=4.0):
//   n=184 trades  WR=28.3%  PnL=+$637.03  DD=$-329.93  PF=1.54
//   Trade frequency: 0.25/day over 2yrs
//
// PASS THRESHOLDS for engine replay:
//   trade_count: 156 - 211      (sweep n=184, +/- 15%)
//   pnl_usd:     $318 - $955    (sweep $637, +/- 50% for execution realism)
//   pf:          >= 1.20        (sweep PF 1.54, allow some erosion)
//   max_dd:      <= $660        (sweep -$330, allow 2x slack)
//
// BUILD (Mac):
//   cd backtest
//   clang++ -O3 -std=c++17 -I../include -o replay_minimal_us30 replay_minimal_us30.cpp
//
// RUN:
//   ./replay_minimal_us30 "/Users/jo/Library/CloudStorage/GoogleDrive-kiwi18@gmail.com/My Drive/Tickstory/US30/dow30_2yr.csv"
//
// OUTPUT:
//   stdout: per-trade log + summary
//   replay_us30_trades.csv:  full trade list
//   replay_us30_summary.txt: summary stats + pass/fail vs thresholds
//   exit code 0 = ALL PASS, 2 = FAIL on any threshold
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <cmath>
#include <ctime>
#include <vector>
#include <string>
#include <chrono>

// Production engine + ledger types. OmegaTradeLedger.hpp is self-contained
// (only stdlib deps) so we can include it directly without dragging in the
// rest of the Omega codebase.
#include "MinimalH4US30Breakout.hpp"

// ---------------------------------------------------------------------------
// Tick parsing -- matches Tickstory CSV format used by htf_bt_multi.cpp
// Format: YYYYMMDD,HH:MM:SS,bid,ask[,extra...]
// ---------------------------------------------------------------------------
struct Tick {
    int64_t ts_ms;
    double  bid;
    double  ask;
};

static bool parse_line(const char* s, Tick& t) {
    if (strlen(s) < 18) return false;
    if (!isdigit((unsigned char)s[0])) return false;

    int year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[4]-'0')*10 + (s[5]-'0');
    int day   = (s[6]-'0')*10 + (s[7]-'0');
    if (s[8] != ',') return false;
    int H = (s[9]-'0')*10 + (s[10]-'0');
    int M = (s[12]-'0')*10 + (s[13]-'0');
    int S = (s[15]-'0')*10 + (s[16]-'0');
    if (s[17] != ',') return false;

    const char* p = s + 18;
    char* end;
    double bid = strtod(p, &end);
    if (end == p || *end != ',') return false;
    p = end + 1;
    double ask = strtod(p, &end);
    if (end == p) return false;

    // Howard Hinnant civil-to-epoch conversion
    int y = year;
    int m = month;
    if (m <= 2) { y -= 1; }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe/4 - yoe/100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
    int64_t sec  = days * 86400 + H * 3600 + M * 60 + S;

    t.ts_ms = sec * 1000LL;
    t.bid   = bid;
    t.ask   = ask;
    return true;
}

static std::string fmt_ts(int64_t ts_ms) {
    time_t s = (time_t)(ts_ms / 1000LL);
    struct tm tmv;
    gmtime_r(&s, &tmv);
    char buf[48];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Captured trade record -- populated by callback from engine
// ---------------------------------------------------------------------------
struct CapturedTrade {
    int         id;
    int64_t     entry_ts_ms;
    int64_t     exit_ts_ms;
    bool        is_long;
    double      entry;
    double      exit_px;
    double      sl;
    double      tp;
    double      size;
    double      pnl_usd;
    std::string exit_reason;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s <dj30_tickstory.csv>\n"
            "Replays 2yr DJ30.F tick data through MinimalH4US30Breakout engine\n"
            "and validates trade count/PnL against sweep harness expectation.\n",
            argv[0]);
        return 1;
    }
    const char* csv_path = argv[1];

    // ── Engine setup with production defaults ─────────────────────────────
    omega::MinimalH4US30Breakout eng;
    eng.p           = omega::make_minimal_h4_us30_params();
    eng.symbol      = "DJ30.F";
    eng.shadow_mode = true;
    eng.enabled     = true;

    printf("=== ENGINE REPLAY: MinimalH4US30Breakout ===\n");
    printf("Symbol:      %s\n", eng.symbol.c_str());
    printf("donchian:    %d\n", eng.p.donchian_bars);
    printf("sl_mult:     %.2f\n", eng.p.sl_mult);
    printf("tp_mult:     %.2f\n", eng.p.tp_mult);
    printf("risk:        $%.2f\n", eng.p.risk_dollars);
    printf("max_lot:     %.2f\n", eng.p.max_lot);
    printf("$/pt:        %.2f\n", eng.p.dollars_per_point);
    printf("max_spread:  %.2f\n", eng.p.max_spread);
    printf("atr_period:  %d\n", eng.p.atr_period);
    printf("\n");
    fflush(stdout);

    // ── Trade capture callback ─────────────────────────────────────────────
    std::vector<CapturedTrade> trades;
    auto on_close = [&trades](const omega::TradeRecord& tr) {
        CapturedTrade ct;
        ct.id          = tr.id;
        ct.entry_ts_ms = tr.entryTs * 1000LL;
        ct.exit_ts_ms  = tr.exitTs  * 1000LL;
        ct.is_long     = (tr.side == "LONG");
        ct.entry       = tr.entryPrice;
        ct.exit_px     = tr.exitPrice;
        ct.sl          = tr.sl;
        ct.tp          = tr.tp;
        ct.size        = tr.size;
        ct.pnl_usd     = tr.pnl;
        ct.exit_reason = tr.exitReason;
        trades.push_back(ct);
    };

    // ── Stream the CSV through the engine ─────────────────────────────────
    FILE* f = fopen(csv_path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", csv_path); return 1; }

    auto t_start = std::chrono::steady_clock::now();
    char line[256];
    uint64_t ticks_ok = 0, ticks_fail = 0;
    int64_t  first_ts = 0, last_ts = 0;
    int64_t  last_progress_ts = 0;
    uint64_t progress_ticks  = 0;

    while (fgets(line, sizeof(line), f)) {
        Tick tk;
        if (!parse_line(line, tk)) { ++ticks_fail; continue; }
        ++ticks_ok;
        if (first_ts == 0) first_ts = tk.ts_ms;
        last_ts = tk.ts_ms;

        eng.on_tick(tk.bid, tk.ask, tk.ts_ms, on_close);

        // Progress every 5M ticks (~1/4 of a 2yr file)
        ++progress_ticks;
        if (progress_ticks >= 5000000) {
            const auto t_now = std::chrono::steady_clock::now();
            const double el = std::chrono::duration_cast<std::chrono::milliseconds>(
                t_now - t_start).count() / 1000.0;
            printf("  ... %llu ticks processed @ %s (%.1fs elapsed, %d trades so far)\n",
                   (unsigned long long)ticks_ok, fmt_ts(tk.ts_ms).c_str(),
                   el, (int)trades.size());
            fflush(stdout);
            progress_ticks = 0;
            last_progress_ts = tk.ts_ms;
        }
    }
    fclose(f);
    (void)last_progress_ts;

    auto t_end = std::chrono::steady_clock::now();
    const double elapsed_s =
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count() / 1000.0;

    printf("\n=== TICK INGEST DONE ===\n");
    printf("Ticks OK:    %llu\n", (unsigned long long)ticks_ok);
    printf("Ticks fail:  %llu\n", (unsigned long long)ticks_fail);
    printf("Date range:  %s -> %s\n",
           fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
    printf("Replay time: %.1fs\n", elapsed_s);
    printf("\n");
    fflush(stdout);

    // ── Compute summary stats ─────────────────────────────────────────────
    double cum = 0.0, peak = 0.0, max_dd = 0.0;
    double gw = 0.0, gl = 0.0;
    int    nw = 0, nl = 0;
    int    n_long = 0, n_short = 0;
    int    n_tp = 0, n_sl = 0, n_other = 0;
    int    n_timeout = 0, n_weekend = 0, n_force = 0, n_unknown = 0;
    double pnl_tp = 0.0, pnl_sl = 0.0, pnl_timeout = 0.0, pnl_weekend = 0.0;
    for (const auto& t : trades) {
        cum += t.pnl_usd;
        if (cum > peak) peak = cum;
        const double dd = peak - cum;
        if (dd > max_dd) max_dd = dd;
        if (t.pnl_usd > 0.0) { ++nw; gw += t.pnl_usd; }
        else                 { ++nl; gl += -t.pnl_usd; }
        if (t.is_long) ++n_long; else ++n_short;
        if      (t.exit_reason == "TP_HIT")        { ++n_tp;       pnl_tp      += t.pnl_usd; }
        else if (t.exit_reason == "SL_HIT")        { ++n_sl;       pnl_sl      += t.pnl_usd; }
        else if (t.exit_reason == "TIMEOUT")       { ++n_timeout;  pnl_timeout += t.pnl_usd; ++n_other; }
        else if (t.exit_reason == "WEEKEND_CLOSE") { ++n_weekend;  pnl_weekend += t.pnl_usd; ++n_other; }
        else if (t.exit_reason == "FORCE_CLOSE")   { ++n_force;                                ++n_other; }
        else                                        { ++n_unknown;                              ++n_other; }
    }
    const int    n_trades   = (int)trades.size();
    const double win_rate   = n_trades > 0 ? (100.0 * nw / n_trades) : 0.0;
    const double avg_win    = nw > 0 ? gw / nw : 0.0;
    const double avg_loss   = nl > 0 ? gl / nl : 0.0;
    const double pf         = gl > 0.0 ? gw / gl : 0.0;
    const double expectancy = n_trades > 0 ? cum / n_trades : 0.0;

    printf("=== ENGINE REPLAY SUMMARY ===\n");
    printf("Trades:          %d  (%d LONG, %d SHORT)\n", n_trades, n_long, n_short);
    printf("Wins/Losses:     %d / %d\n", nw, nl);
    printf("Win rate:        %.1f%%\n", win_rate);
    printf("Total PnL:       $%+.2f\n", cum);
    printf("Avg win:         $%+.2f\n", avg_win);
    printf("Avg loss:        $%+.2f\n", -avg_loss);
    printf("Expectancy:      $%+.2f / trade\n", expectancy);
    printf("Max drawdown:    $%.2f\n", -max_dd);
    printf("Profit factor:   %.2f\n", pf);
    printf("Exit breakdown:  TP=%d  SL=%d  OTHER=%d\n", n_tp, n_sl, n_other);
    printf("                 (OTHER = TIMEOUT=%d WEEKEND=%d FORCE=%d UNK=%d)\n",
           n_timeout, n_weekend, n_force, n_unknown);
    printf("PnL by exit:     TP=$%+.2f  SL=$%+.2f  TIMEOUT=$%+.2f  WEEKEND=$%+.2f\n",
           pnl_tp, pnl_sl, pnl_timeout, pnl_weekend);
    if (n_tp + n_sl > 0) {
        const double avg_tp = n_tp > 0 ? pnl_tp / n_tp : 0.0;
        const double avg_sl = n_sl > 0 ? pnl_sl / n_sl : 0.0;
        const double avg_to = n_timeout > 0 ? pnl_timeout / n_timeout : 0.0;
        const double avg_wk = n_weekend > 0 ? pnl_weekend / n_weekend : 0.0;
        printf("Avg PnL/exit:    TP=$%+.2f  SL=$%+.2f  TIMEOUT=$%+.2f  WEEKEND=$%+.2f\n",
               avg_tp, avg_sl, avg_to, avg_wk);
    }
    printf("\n");

    // ── Compare to sweep expectation (D=10 SL=1.0 TP=4.0) ─────────────────
    const int    expect_n        = 184;
    const double expect_pnl      = 637.03;
    const double expect_pf       = 1.54;
    const double expect_dd       = 329.93;
    const double expect_wr       = 28.3;
    const int    n_lo            = (int)(expect_n * 0.85);   // 156
    const int    n_hi            = (int)(expect_n * 1.15);   // 211
    const double pnl_lo          = expect_pnl * 0.50;        // ~$318
    const double pnl_hi          = expect_pnl * 1.50;        // ~$955
    const double pf_floor        = 1.20;
    const double dd_ceiling      = expect_dd * 2.0;          // ~$660

    const bool pass_n   = (n_trades >= n_lo  && n_trades <= n_hi);
    const bool pass_pnl = (cum      >= pnl_lo && cum      <= pnl_hi);
    const bool pass_pf  = (pf       >= pf_floor);
    const bool pass_dd  = (max_dd   <= dd_ceiling);
    const bool pass_all = pass_n && pass_pnl && pass_pf && pass_dd;

    printf("=== VS SWEEP EXPECTATION (D=10 SL=1.0 TP=4.0) ===\n");
    printf("                   ENGINE     SWEEP    PASS RANGE         RESULT\n");
    printf("Trade count:       %-8d   %-8d [%d, %d]         %s\n",
           n_trades, expect_n, n_lo, n_hi, pass_n  ? "PASS" : "FAIL");
    printf("Total PnL:         $%+8.2f $%+8.2f [$%.0f, $%.0f]     %s\n",
           cum, expect_pnl, pnl_lo, pnl_hi, pass_pnl ? "PASS" : "FAIL");
    printf("Profit factor:     %-8.2f   %-8.2f >= %.2f             %s\n",
           pf, expect_pf, pf_floor, pass_pf ? "PASS" : "FAIL");
    printf("Max drawdown:      $%-8.2f $%-8.2f <= $%.0f             %s\n",
           max_dd, expect_dd, dd_ceiling, pass_dd ? "PASS" : "FAIL");
    printf("Win rate (info):   %-8.1f%%  %-8.1f%%\n",
           win_rate, expect_wr);
    printf("\n");
    printf("OVERALL: %s\n", pass_all
        ? "ALL PASS - engine faithfully reproduces strategy"
        : "FAIL - engine diverges from strategy, investigate");
    fflush(stdout);

    // ── Write trade CSV ────────────────────────────────────────────────────
    {
        FILE* tf = fopen("replay_us30_trades.csv", "w");
        if (tf) {
            fprintf(tf, "id,entry_ts_utc,exit_ts_utc,side,entry,exit,sl,tp,size,pnl_usd,exit_reason\n");
            for (const auto& t : trades) {
                fprintf(tf, "%d,%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%.3f,%.2f,%s\n",
                        t.id, fmt_ts(t.entry_ts_ms).c_str(), fmt_ts(t.exit_ts_ms).c_str(),
                        t.is_long ? "LONG" : "SHORT",
                        t.entry, t.exit_px, t.sl, t.tp, t.size,
                        t.pnl_usd, t.exit_reason.c_str());
            }
            fclose(tf);
            printf("\nWrote replay_us30_trades.csv (%d trades)\n", n_trades);
        }
    }
    {
        FILE* sf = fopen("replay_us30_summary.txt", "w");
        if (sf) {
            fprintf(sf, "MinimalH4US30Breakout engine replay summary\n");
            fprintf(sf, "Source CSV: %s\n", csv_path);
            fprintf(sf, "Date range: %s -> %s\n",
                    fmt_ts(first_ts).c_str(), fmt_ts(last_ts).c_str());
            fprintf(sf, "Ticks OK: %llu  fail: %llu\n",
                    (unsigned long long)ticks_ok, (unsigned long long)ticks_fail);
            fprintf(sf, "Replay time: %.1fs\n", elapsed_s);
            fprintf(sf, "\n");
            fprintf(sf, "Engine config: D=%d SL=%.1fx TP=%.1fx risk=$%.0f max_lot=%.2f $/pt=%.1f\n",
                    eng.p.donchian_bars, eng.p.sl_mult, eng.p.tp_mult,
                    eng.p.risk_dollars, eng.p.max_lot, eng.p.dollars_per_point);
            fprintf(sf, "\n");
            fprintf(sf, "Trades:         %d (%d LONG, %d SHORT)\n", n_trades, n_long, n_short);
            fprintf(sf, "Wins/Losses:    %d / %d\n", nw, nl);
            fprintf(sf, "Win rate:       %.1f%%\n", win_rate);
            fprintf(sf, "Total PnL:      $%+.2f\n", cum);
            fprintf(sf, "Avg win:        $%+.2f\n", avg_win);
            fprintf(sf, "Avg loss:       $%+.2f\n", -avg_loss);
            fprintf(sf, "Expectancy:     $%+.2f / trade\n", expectancy);
            fprintf(sf, "Max drawdown:   $%.2f\n", -max_dd);
            fprintf(sf, "Profit factor:  %.2f\n", pf);
            fprintf(sf, "Exit reasons:   TP=%d SL=%d OTHER=%d\n", n_tp, n_sl, n_other);
            fprintf(sf, "  OTHER detail: TIMEOUT=%d WEEKEND=%d FORCE=%d UNK=%d\n",
                    n_timeout, n_weekend, n_force, n_unknown);
            fprintf(sf, "PnL by exit:    TP=$%+.2f SL=$%+.2f TIMEOUT=$%+.2f WEEKEND=$%+.2f\n",
                    pnl_tp, pnl_sl, pnl_timeout, pnl_weekend);
            fprintf(sf, "\n");
            fprintf(sf, "Sweep expectation (D=10 SL=1.0 TP=4.0):\n");
            fprintf(sf, "  Trades=%d PnL=$%.2f PF=%.2f DD=$%.2f WR=%.1f%%\n",
                    expect_n, expect_pnl, expect_pf, expect_dd, expect_wr);
            fprintf(sf, "\n");
            fprintf(sf, "Pass thresholds:\n");
            fprintf(sf, "  trade_count [%d, %d]      %s (engine: %d)\n",
                    n_lo, n_hi, pass_n  ? "PASS" : "FAIL", n_trades);
            fprintf(sf, "  total_pnl   [$%.0f, $%.0f]   %s (engine: $%.2f)\n",
                    pnl_lo, pnl_hi, pass_pnl ? "PASS" : "FAIL", cum);
            fprintf(sf, "  pf >= %.2f             %s (engine: %.2f)\n",
                    pf_floor, pass_pf ? "PASS" : "FAIL", pf);
            fprintf(sf, "  max_dd <= $%.0f          %s (engine: $%.2f)\n",
                    dd_ceiling, pass_dd ? "PASS" : "FAIL", max_dd);
            fprintf(sf, "\n");
            fprintf(sf, "OVERALL: %s\n", pass_all ? "PASS" : "FAIL");
            fclose(sf);
            printf("Wrote replay_us30_summary.txt\n");
        }
    }

    return pass_all ? 0 : 2;
}
