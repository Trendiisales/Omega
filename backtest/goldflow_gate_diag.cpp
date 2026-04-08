// goldflow_gate_diag.cpp
// Diagnoses exactly which gate is blocking entries
// Build: g++ -O2 -std=c++17 -o goldflow_gate_diag goldflow_gate_diag.cpp
// Run:   ./goldflow_gate_diag ticks.csv

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <iomanip>

struct Tick { uint64_t ts=0; double ask=0, bid=0; };

bool parse_tick(const std::string& line, Tick& t)
{
    if (line.empty()) return false;
    std::stringstream ss(line);
    std::string tok;
    if (!getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

inline int      utc_hour(uint64_t ts) { return (int)((ts/1000/3600)%24); }
inline uint64_t utc_day (uint64_t ts) { return ts/1000/86400; }
inline bool session_ok  (uint64_t ts) { int h=utc_hour(ts); return (h>=7&&h<=10)||(h>=12&&h<=16); }

static const int    WINDOW          = 60;
static const double IMPULSE_MIN     = 7.0;
static const double PULLBACK_FRAC   = 0.55;
static const double VWAP_TREND_PTS  = 0.5;
static const int    VWAP_TREND_LOOK = 30;
static const double MAX_SPREAD      = 0.40;
static const int    COOLDOWN_TICKS  = 300;

int main(int argc, char** argv)
{
    if (argc < 2) { std::cout << "usage: goldflow_gate_diag ticks.csv\n"; return 0; }

    std::ifstream file(argv[1]);
    if (!file.is_open()) { std::cout << "cannot open: " << argv[1] << "\n"; return 1; }

    // gate counters
    uint64_t ticks_total      = 0;
    uint64_t ticks_in_session = 0;
    uint64_t blocked_spread   = 0;
    uint64_t blocked_cooldown = 0;
    uint64_t blocked_no_impulse = 0;
    uint64_t impulse_fired    = 0;
    uint64_t blocked_no_pullback = 0;  // impulse OK but price not in pullback zone
    uint64_t blocked_vwap_pos = 0;     // pullback OK but wrong side of VWAP
    uint64_t blocked_vwap_trend = 0;   // vwap side OK but trend delta too small
    uint64_t entries          = 0;

    // sample some actual vwap deltas so we can see what values look like
    std::vector<double> vwap_deltas;
    vwap_deltas.reserve(10000);

    // state
    std::vector<double> price_buf, vwap_buf;
    double vwap=0, vwap_pv=0;
    uint64_t vwap_count=0, vwap_day=0;
    double hi=0, lo=0;
    int cooldown=0;
    bool in_pos=false;
    uint64_t entry_ts=0;
    static const uint64_t TIME_LIMIT_MS=1800000;

    // also track: of sessions where impulse fired, what fraction had VWAP trend?
    uint64_t impulse_with_vwap_data = 0;   // impulse fired AND vwap_buf big enough
    uint64_t impulse_vwap_trend_ok  = 0;   // of those, trend delta was sufficient

    std::string line;
    int sample_count = 0;

    while (getline(file, line))
    {
        Tick t;
        if (!parse_tick(line,t)) continue;
        ticks_total++;

        double spread = t.ask - t.bid;
        double mid    = (t.ask + t.bid) * 0.5;

        // VWAP update (always)
        {
            uint64_t day = utc_day(t.ts);
            if (day != vwap_day) { vwap_pv=0; vwap_count=0; vwap_day=day; }
            vwap_pv += mid; vwap_count++;
            vwap = vwap_pv / (double)vwap_count;
        }
        vwap_buf.push_back(vwap);
        if ((int)vwap_buf.size() > VWAP_TREND_LOOK + 5)
            vwap_buf.erase(vwap_buf.begin());

        price_buf.push_back(mid);
        if ((int)price_buf.size() > WINDOW + 5)
            price_buf.erase(price_buf.begin());

        if (spread > MAX_SPREAD) { blocked_spread++; continue; }
        if (!session_ok(t.ts)) continue;
        ticks_in_session++;

        // manage fake position (just for cooldown tracking)
        if (in_pos) {
            if (t.ts - entry_ts >= TIME_LIMIT_MS) {
                in_pos = false;
                cooldown = COOLDOWN_TICKS;
            }
        }

        if (cooldown > 0) { cooldown--; }

        if (in_pos || cooldown > 0) {
            if (cooldown > 0 && !in_pos) blocked_cooldown++;
            continue;
        }

        // impulse check
        if ((int)price_buf.size() < WINDOW) { blocked_no_impulse++; continue; }
        int start = (int)price_buf.size() - WINDOW;
        hi = price_buf[start]; lo = price_buf[start];
        for (int i=start;i<(int)price_buf.size();i++) {
            hi=std::max(hi,price_buf[i]); lo=std::min(lo,price_buf[i]);
        }
        if (hi - lo < IMPULSE_MIN) { blocked_no_impulse++; continue; }

        impulse_fired++;

        double impulse  = hi - lo;
        double pb_long  = hi - PULLBACK_FRAC * impulse;
        double pb_short = lo + PULLBACK_FRAC * impulse;

        bool in_pb_long  = (mid <= pb_long);
        bool in_pb_short = (mid >= pb_short);

        if (!in_pb_long && !in_pb_short) { blocked_no_pullback++; continue; }

        // VWAP side check
        bool vwap_side_ok = (in_pb_long && mid > vwap) || (in_pb_short && mid < vwap);
        if (!vwap_side_ok) { blocked_vwap_pos++; continue; }

        // VWAP trend check
        if ((int)vwap_buf.size() >= VWAP_TREND_LOOK) {
            impulse_with_vwap_data++;
            int n = (int)vwap_buf.size();
            double delta = vwap_buf[n-1] - vwap_buf[n - VWAP_TREND_LOOK];

            // sample deltas for distribution
            if (sample_count < 50000) {
                vwap_deltas.push_back(std::abs(delta));
                sample_count++;
            }

            bool trend_ok = (in_pb_long && delta > VWAP_TREND_PTS) ||
                            (in_pb_short && delta < -VWAP_TREND_PTS);
            if (!trend_ok) { blocked_vwap_trend++; continue; }
            impulse_vwap_trend_ok++;
        } else {
            blocked_vwap_trend++;
            continue;
        }

        // ENTRY
        entries++;
        in_pos    = true;
        entry_ts  = t.ts;
        cooldown  = COOLDOWN_TICKS;
    }

    // ── VWAP delta percentiles ───────────────────────────────
    std::sort(vwap_deltas.begin(), vwap_deltas.end());
    auto pct = [&](double p) -> double {
        if (vwap_deltas.empty()) return 0;
        size_t idx = (size_t)(p * (vwap_deltas.size()-1));
        return vwap_deltas[idx];
    };

    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  GoldFlow Gate Diagnostics\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";
    std::cout << "  Total ticks      : " << ticks_total << "\n";
    std::cout << "  In-session ticks : " << ticks_in_session << "\n";
    std::cout << "  Blocked spread   : " << blocked_spread << "\n";
    std::cout << "\n";
    std::cout << "  Of in-session ticks (not in position, not in cooldown):\n";
    std::cout << "    Blocked no impulse     : " << blocked_no_impulse << "\n";
    std::cout << "    Impulse fired          : " << impulse_fired << "\n";
    std::cout << "    Blocked no pullback    : " << blocked_no_pullback << "\n";
    std::cout << "    Blocked wrong VWAP side: " << blocked_vwap_pos << "\n";
    std::cout << "    Blocked VWAP trend     : " << blocked_vwap_trend << "\n";
    std::cout << "    ENTRIES                : " << entries << "\n";
    std::cout << "\n";
    std::cout << "  VWAP trend gate detail:\n";
    std::cout << "    Impulse ticks w/ vwap data  : " << impulse_with_vwap_data << "\n";
    std::cout << "    Of those: trend delta OK     : " << impulse_vwap_trend_ok << "\n";
    std::cout << "    Gate pass rate               : ";
    if (impulse_with_vwap_data > 0)
        std::cout << std::fixed << std::setprecision(2)
                  << 100.0 * impulse_vwap_trend_ok / impulse_with_vwap_data << "%\n";
    else std::cout << "n/a\n";
    std::cout << "\n";
    std::cout << "  VWAP delta distribution (|delta| over " << VWAP_TREND_LOOK << " ticks)\n";
    std::cout << "  (sampled from ticks that passed impulse+pullback+vwap-side)\n";
    std::cout << "    p10  = " << std::fixed << std::setprecision(4) << pct(0.10) << " pts\n";
    std::cout << "    p25  = " << pct(0.25) << " pts\n";
    std::cout << "    p50  = " << pct(0.50) << " pts\n";
    std::cout << "    p75  = " << pct(0.75) << " pts\n";
    std::cout << "    p90  = " << pct(0.90) << " pts\n";
    std::cout << "    p95  = " << pct(0.95) << " pts\n";
    std::cout << "    p99  = " << pct(0.99) << " pts\n";
    std::cout << "    max  = " << pct(1.00) << " pts\n";
    std::cout << "  threshold = " << VWAP_TREND_PTS << " pts\n";
    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════════════════\n";

    return 0;
}
