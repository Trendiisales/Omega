// JoDisplayInbound.hpp -- surfaces the Mac-side Jo paper engine's OPEN positions in the
// Omega live_trades GUI panel (S-2026-06-26, operator: "show the 8 open jo engine trades in
// the omega gui"). Jo is a SEPARATE Mac swing-paper engine (jo_engine.py, launchd
// com.jo.engine -- long-only tight-trail Luke companion). It writes its open book to
// jo_inbound_open.csv which jo_inbound_sync.py scp's into C:\Omega\logs\trades\. This DISPLAY
// source reads that file and returns PositionSnapshots so the legs appear by tag "Jo:<sym>"
// in the GUI. DISPLAY ONLY -- no execution, no ledger record, no risk gates (mirrors the
// crypto-inbound posture: a foreign shadow book must never pause the real engines). Re-parses
// only when the file mtime changes (cheap under the 250ms snapshot rebuild).
// Opt-in: OMEGA_JO_INBOUND=1. Off => source never registered (see engine_init.hpp).
#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>
#include "OpenPositionRegistry.hpp"

namespace omega {

// jo_inbound_open.csv row: symbol,side,size,entry,current,sl,entry_ts  (header line skipped).
// current==entry today (Jo is a daily engine, no intraday mark) -> unrealized 0; the panel
// still shows symbol / side / size / entry, which is the ask.
inline std::vector<PositionSnapshot> read_jo_inbound() {
    static std::vector<PositionSnapshot> cache;
    static long long cached_mtime = -1;
    const char* envf = std::getenv("OMEGA_JO_INBOUND_FILE");
    const std::string fp = envf ? envf : "C:\\Omega\\logs\\trades\\jo_inbound_open.csv";

    struct stat st;
    if (stat(fp.c_str(), &st) != 0) { cache.clear(); cached_mtime = -1; return cache; }
    if (static_cast<long long>(st.st_mtime) == cached_mtime) return cache;  // unchanged -> cache

    std::vector<PositionSnapshot> out;
    std::ifstream f(fp);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("symbol,", 0) == 0)  continue;   // header
        std::stringstream ss(line); std::string tok; std::vector<std::string> c;
        while (std::getline(ss, tok, ',')) c.push_back(tok);
        if (c.size() < 7) continue;
        PositionSnapshot ps;
        ps.symbol         = c[0];
        ps.side           = c[1].empty() ? "LONG" : c[1];
        ps.size           = std::atof(c[2].c_str());
        ps.entry          = std::atof(c[3].c_str());
        ps.current        = std::atof(c[4].c_str());
        ps.sl             = std::atof(c[5].c_str());
        ps.entry_ts       = std::atoll(c[6].c_str());
        ps.unrealized_pnl = (ps.current > 0.0 && ps.entry > 0.0)
                              ? (ps.current - ps.entry) * ps.size : 0.0;
        ps.engine         = "Jo:" + c[0];
        out.push_back(ps);
    }
    cache = out; cached_mtime = static_cast<long long>(st.st_mtime);
    return cache;
}

} // namespace omega
