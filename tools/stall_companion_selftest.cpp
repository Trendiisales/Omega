// =============================================================================
// stall_companion_selftest.cpp — sandbox FIRES-ON-TRIGGER proof for the LIVE
// C++ StallCompanion (include/StallCompanion.hpp), the in-binary protection
// that replaced the retired Mac-python stall_accountant.py at the 2026-07-06
// cutover.
//
// WHY (S-2026-07-20i effect-verification audit): protection_selftest.py check
// [3] still sandbox-fired the RETIRED python — a green [3] proved dead code
// clips, not the code in Omega.exe. This harness drives the ACTUAL shipped
// class through a synthetic peak->giveback and asserts the clip lands in
// companion_closed.csv, in BOTH live modes:
//   A) pct-mode REVERSAL_CLIP  (classic stall-accountant giveback trail)
//   B) be-mode  FLOOR_CLIP     (S-17t BE-ENTRY floored books — the
//                               profit-lock enforcers; anchor/floor path)
//
// Usage: stall_companion_selftest <empty-sandbox-dir>   (exit 0 = both fired)
// Compiled+run by protection_selftest.py check [3]; NOT part of Omega.exe.
// =============================================================================
#include "../include/StallCompanion.hpp"
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>

using omega::StallBook;
using omega::StallLiveRow;

static StallLiveRow row(double entry, double current, double upnl) {
    StallLiveRow r;
    r.book = "OMEGA"; r.eng = "SelfTest"; r.sym = "TESTSYM"; r.side = "LONG";
    r.entry = entry; r.current = current; r.upnl = upnl;
    return r;
}

// count closed.csv rows with the given reason; return that row's realized_pnl
static bool find_clip(const std::string& csv, const char* reason, double* pnl_out) {
    std::ifstream f(csv);
    if (!f.is_open()) return false;
    std::string ln; bool first = true;
    while (std::getline(f, ln)) {
        if (first) { first = false; continue; }
        std::vector<std::string> c; std::stringstream ss(ln); std::string t;
        while (std::getline(ss, t, ',')) c.push_back(t);
        if (c.size() < 10) continue;
        if (c[2] == reason) { *pnl_out = std::atof(c[7].c_str()); return true; }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <sandbox-dir>\n", argv[0]); return 2; }
    const std::string sb = argv[1];
    const int64_t now = (int64_t)std::time(nullptr);
    int fails = 0;

    // ── A) pct-mode: peak +8%%, give back to +1%% (<= 8*(1-0.40)=4.8) => REVERSAL_CLIP
    {
        StallBook::Config c;
        c.name = "selftest_pct"; c.dir = sb + "/pct";
        c.gate_pct = 2.0; c.rev_gb = 0.40; c.stall_bars = 99; c.retire_usd = 0.0;
        StallBook b(c);
        b.step({row(100.0, 108.0, 80.0)}, -1, -1, now);        // open + arm, mfe 8%
        b.step({row(100.0, 101.0, 10.0)}, -1, -1, now + 60);   // giveback -> clip
        double pnl = 0.0;
        const bool fired = find_clip(c.dir + "/companion_closed.csv", "REVERSAL_CLIP", &pnl);
        std::printf("[A pct REVERSAL_CLIP] fired=%d realized=%.2f (expect 10.00)\n", fired, pnl);
        if (!fired || pnl < 9.99 || pnl > 10.01) fails++;
    }

    // ── B) be-mode: flat below confirm, open at +60, fall to +20 (<= max(60-25, 0+2)=35)
    //      => FLOOR_CLIP banking upnl - anchor = 20
    {
        StallBook::Config c;
        c.name = "selftest_be"; c.dir = sb + "/be";
        c.confirm_usd = 25.0; c.trail_usd = 25.0; c.floor_cost_usd = 2.0;
        c.retrig_usd = 25.0; c.retire_usd = 0.0;
        StallBook b(c);
        b.step({row(100.0, 110.0, 10.0)}, -1, -1, now);        // fav 10 < confirm 25 -> stays FLAT
        {   // must NOT have opened pre-confirm (BE-ENTRY: no pre-BE exposure)
            std::ifstream f(c.dir + "/companion_positions.tsv");
            std::string ln; bool open_pre = false;
            while (std::getline(f, ln)) if (!ln.empty()) open_pre = true;
            std::printf("[B be pre-confirm flat] open_pre=%d (expect 0)\n", open_pre);
            if (open_pre) fails++;
        }
        b.step({row(100.0, 160.0, 60.0)}, -1, -1, now + 60);   // fav 60 >= 25 -> open anchored
        b.step({row(100.0, 120.0, 20.0)}, -1, -1, now + 120);  // 20 <= 35 -> FLOOR_CLIP
        double pnl = 0.0;
        const bool fired = find_clip(c.dir + "/companion_closed.csv", "FLOOR_CLIP", &pnl);
        std::printf("[B be FLOOR_CLIP] fired=%d realized=%.2f (expect 20.00)\n", fired, pnl);
        if (!fired || pnl < 19.99 || pnl > 20.01) fails++;
    }

    std::printf("STALL-SELFTEST %s (%d fail)\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : 1;
}
