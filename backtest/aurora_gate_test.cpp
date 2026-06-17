// aurora_gate_test.cpp -- unit test for include/AuroraGate.hpp (fail-open safety).
// Build: clang++ -O2 -std=c++17 -I include backtest/aurora_gate_test.cpp -o /tmp/agt
#include "AuroraGate.hpp"
#include <cstdio>
#include <fstream>
#include <cstdlib>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++fails; } \
                              else std::printf("ok  : %s\n", msg); } while(0)

static void write_tsv(const std::string& p, const std::string& body) {
    std::ofstream f(p); f << body; f.close();
}

int main() {
    const std::string p = "/tmp/aurora_gate_test.tsv";
    const int64_t NOW = 1781700000000LL;            // fixed "now" (epoch ms)
    const int64_t FRESH = NOW - 60000;              // 1 min old
    const int64_t STALE = NOW - 60LL*60*1000;       // 1 hr old (> 15 min)

    // 1. Fresh: XAU long ok, short blocked; NAS both ok
    write_tsv(p, "# sym al as bias rl rs stamp\n"
                 "XAUUSD\t1\t0\tlong\t2.10\t0.40\t" + std::to_string(FRESH) + "\n"
                 "NAS100\t1\t1\tneutral\t3.0\t3.0\t" + std::to_string(FRESH) + "\n");
    {
        omega::AuroraGate g; g.set_path(p);
        CHECK(g.allow("XAUUSD", true,  NOW) == true,  "fresh XAU long allowed");
        CHECK(g.allow("XAUUSD", false, NOW) == false, "fresh XAU short BLOCKED");
        CHECK(g.allow("NAS100", true,  NOW) == true,  "fresh NAS long allowed");
        CHECK(g.bias("XAUUSD", NOW) == 1,             "XAU bias=long(+1)");
        CHECK(g.allow("EURUSD", true,  NOW) == true,  "uncovered symbol -> allow");
    }
    // 2. Stale stamp -> fail open (allow everything incl the blocked short)
    write_tsv(p, "XAUUSD\t1\t0\tlong\t2.1\t0.4\t" + std::to_string(STALE) + "\n");
    {
        omega::AuroraGate g; g.set_path(p);
        CHECK(g.allow("XAUUSD", false, NOW) == true,  "STALE feed -> short allowed (fail-open)");
        CHECK(g.bias("XAUUSD", NOW) == 0,             "STALE -> bias neutral");
    }
    // 3. Missing file -> allow
    {
        omega::AuroraGate g; g.set_path("/tmp/does_not_exist_aurora.tsv");
        CHECK(g.allow("XAUUSD", false, NOW) == true,  "missing file -> allow (fail-open)");
    }
    // 4. Master switch off -> allow even a fresh block
    write_tsv(p, "XAUUSD\t0\t0\tshort\t0.1\t0.1\t" + std::to_string(FRESH) + "\n");
    {
        omega::AuroraGate g; g.set_path(p); g.set_enabled(false);
        CHECK(g.allow("XAUUSD", true,  NOW) == true,  "gate disabled -> allow");
    }
    // 5. Fresh hard block both sides honored when enabled
    {
        omega::AuroraGate g; g.set_path(p);
        CHECK(g.allow("XAUUSD", true,  NOW) == false, "fresh XAU long BLOCKED honored");
        CHECK(g.allow("XAUUSD", false, NOW) == false, "fresh XAU short BLOCKED honored");
    }
    std::printf(fails ? "\n=== %d FAILURES ===\n" : "\n=== ALL PASS ===\n", fails);
    return fails ? 1 : 0;
}
