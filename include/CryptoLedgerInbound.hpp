// CryptoLedgerInbound.hpp -- routes IBKRCrypto shadow-book CLOSES into the Omega ledger
// (S-2026-06-24, operator: "when a crypto trade exits, add that pnl to Omega + show the
// engine in the Omega GUI"). The crypto book is a SEPARATE shadow system (Python
// refresh_shadow.py, its own state.json). It writes each realized flip-close to
// crypto_inbound.csv (scp'd into C:\Omega\logs\trades\). This consumer reads new rows
// and injects them via g_omegaLedger.record() DIRECTLY -- NOT handle_closed_trade --
// so they roll into the Omega P&L total + appear by engine tag "IBKRCrypto:<sym>:<strat>"
// WITHOUT re-applying costs (crypto pnl is already net) or touching Omega's risk gates
// (consec-loss pauses etc. are in handle_closed_trade; crypto shadow losses must NOT
// pause the real gold/index engines). Restart-safe dedup via a .seen sidecar.
// Opt-in: OMEGA_CRYPTO_INBOUND=1. Off => never reads the file.
#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <cstdlib>
#include <cstdio>
#include "OmegaTradeLedger.hpp"

namespace omega {

// crypto_inbound.csv row: id,entry_ts,exit_ts,sym,strat,side,entry,exit,net_usd
inline void ingest_crypto_inbound(OmegaTradeLedger& ledger) {
    static std::unordered_set<std::string> seen;
    static bool loaded = false;
    static int  next_id = 900000;          // crypto id range -- away from engine ids
    const char* envf = std::getenv("OMEGA_CRYPTO_INBOUND_FILE");
    const std::string fp     = envf ? envf : "C:\\Omega\\logs\\trades\\crypto_inbound.csv";
    const std::string seenfp = fp + ".seen";
    if (!loaded) {                          // restart-safe: reload already-ingested ids
        std::ifstream sf(seenfp); std::string l;
        while (std::getline(sf, l)) if (!l.empty()) seen.insert(l);
        loaded = true;
    }
    std::ifstream f(fp);
    if (!f) return;
    std::ofstream sfa(seenfp, std::ios::app);
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("id,", 0) == 0)     continue;     // header
        std::stringstream ss(line); std::string tok; std::vector<std::string> c;
        while (std::getline(ss, tok, ',')) c.push_back(tok);
        if (c.size() < 9) continue;
        const std::string uid = c[0];
        if (seen.count(uid)) continue;
        seen.insert(uid); sfa << uid << "\n"; sfa.flush();

        TradeRecord tr;
        tr.id         = next_id++;
        tr.entryTs    = std::atoll(c[1].c_str());
        tr.exitTs     = std::atoll(c[2].c_str());
        tr.symbol     = c[3];
        tr.side       = c[5];
        tr.entryPrice = std::atof(c[6].c_str());
        tr.exitPrice  = std::atof(c[7].c_str());
        const double net = std::atof(c[8].c_str());
        tr.pnl        = net;                 // crypto pnl is already net (shadow, cost-adjusted)
        tr.net_pnl    = net;
        tr.size       = 1.0;
        tr.exitReason = "FLIP";              // crypto exits on signal-turn
        tr.regime     = "CRYPTO";
        tr.engine     = "IBKRCrypto:" + c[3] + ":" + c[4];
        ledger.record(tr);                   // -> Omega P&L total + GUI; NO risk-gate side effects
        std::printf("[CRYPTO-LEDGER] %s %s net=$%.2f -> Omega ledger\n",
                    tr.engine.c_str(), tr.side.c_str(), net);
        std::fflush(stdout);
    }
}

} // namespace omega
