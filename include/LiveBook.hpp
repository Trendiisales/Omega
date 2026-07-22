#pragma once
// =============================================================================
// LiveBook.hpp -- the validated "live book" for cost-realistic shadow simulation.
//
// Goal (operator, 2026-06-24): run ONLY the validated engines in a live-like
// environment -- "trading" in shadow but with FULL costs/slippage/spread -- so the
// simulated P&L equals what real trading would produce. Two pieces:
//
//   1. livebook_is_validated(tag): the allowlist. An engine is "in the book" ONLY
//      if its AUDITED_CONFIGS.tsv verdict is EDGE (both-regime). Everything else is
//      RESEARCH (shadow sandbox) -- it still runs, but is NOT counted as the book.
//      This closes the structural hole where any enabled=true engine joined the book
//      with no validation check.
//
//   2. shadow_spread_floor(): guarantees a realistic bid/ask spread is charged even
//      when an engine left tr.spreadAtEntry==0 (turtles, equities did -> ZERO cost =
//      optimistic). Makes the shadow fills carry true execution cost.
//
//   3. livebook_record(): appends validated-engine closes to a clean, separate
//      ledger (omega_livebook_closes.csv) so the real number is legible and isn't
//      diluted by 80 research engines or masked by phantom fat-tails. Additive --
//      does not touch the existing journal pipeline.
//
// TAGS are the EXACT TradeRecord.engine strings the engines emit (verified in code):
//   XauTrendFollow4h_<cell>  (XauTrendFollow4hEngine)
//   CalendarTom_<sym>        (CalendarTomEngine)
//   NasTurtleD1_<sym>        (NasTurtleD1Engine; per-symbol tag added 2026-06-24)
// Keep this list tied to AUDITED_CONFIGS verdict=EDGE + cross-regime. Bull-only /
// SHADOW-CANDIDATE / DEAD engines do NOT belong here.
// =============================================================================
#include <string>
#include <fstream>
#include <mutex>
#include "OmegaTradeLedger.hpp"   // omega::TradeRecord

namespace omega {

inline bool livebook_tag_match(const std::string& tag, const std::string& base) {
    return tag == base ||
        (tag.size() > base.size() &&
         tag.compare(0, base.size(), base) == 0 && tag[base.size()] == '_');
}

// Validated both-regime EDGE engines (AUDITED_CONFIGS verdict=EDGE).
inline bool livebook_is_validated(const std::string& tag) {
    static const char* kEdge[] = {
        "XauTrendFollow4h",     // XauTf4h      EDGE bull1.58/bear1.13 cross-regime, NOT bull-beta
        // FxXRev_EURGBP tag REMOVED (S-2026-07-01 "no FX"): FxCrossRevEngine deleted.
        "CalendarTom",          // CalendarTom  EDGE turn-of-month, STRONGER in 2022 bear (index+XAU)
        "NasTurtleD1_US500.F",  // SpxTurtleD1  EDGE 10yr daily, 2022 bear +92, both WF halves+
        "NasTurtleD1_DJ30.F",   // Dj30TurtleD1 EDGE 10yr daily, 2022 bear +63, both WF halves+
        "ConnorsRSI2",          // ConnorsNas + ConnorsGer (prefix match)  EDGE PF4.17 NAS / PF1.38 GER both-WF+, 2022 bear positive both; SMA200 self-gate sits out bears
        // S-2026-07-22c live-only promotion (operator: certified-live, rest culled):
        "GoldTsmomD1V2",        // EDGE PF1.96-2.09 both-regime, 2022 short +117pt (never bull-gate its shorts)
        "GoldCampaignD1Anch",   // EDGE PF2.02@14bp z+3.22 pooled n=60 5-era, additivity proven
        "XAU_4h_DonchN20",      // SurvivorPortfolio cell  EDGE (survivor_gated_bt PF1.70, bear-2022 PF1.90+)
        "USTEC_4h_ZMR",         // SurvivorPortfolio cell  EDGE (same cert; freed by RSI_N7 cull n36->84 +$9,352)
    };
    for (auto e : kEdge) if (livebook_tag_match(tag, e)) return true;
    return false;
}

// Realistic full bid/ask spread (PRICE units) per instrument class. Used as a floor
// when an engine left tr.spreadAtEntry==0 so apply_realistic_costs never charges $0.
inline double shadow_spread_floor(const std::string& sym, double price) {
    if (price <= 0.0) return 0.0;
    double pct;  // full spread as % of price
    if (sym=="EURUSD"||sym=="GBPUSD"||sym=="AUDUSD"||sym=="NZDUSD"||
        sym=="USDJPY"||sym=="EURGBP"||sym=="AUDNZD")            pct = 0.003;  // FX major
    else if (sym=="MGC"||sym=="GC") return 0.10;  // S-2026-07-11: COMEX gold futures, 1 exchange tick full spread (the old fallback hit the 0.050% equity branch = ~2.2pt at $4400, ~22x the real 0.10pt)
    else if (sym=="XAUUSD"||sym=="XAGUSD")                      pct = 0.010;  // gold/silver
    else if (sym=="USOIL.F"||sym=="UKBRENT"||sym=="BRENT")      pct = 0.012;  // oil
    else if (sym=="US500.F"||sym=="USTEC.F"||sym=="DJ30.F"||sym=="NAS100"||
             sym=="US500"||sym=="GER40"||sym=="UK100"||sym=="ESTX50") pct = 0.006;  // index
    else                                                       pct = 0.050;  // EQUITY big-cap: spread+slip+comm embedded
    return price * pct / 100.0;
}

// Append a validated-engine close to the clean live-book ledger. No-op for research.
inline void livebook_record(const TradeRecord& tr, const std::string& trades_dir) {
    if (!livebook_is_validated(tr.engine)) return;
    static std::mutex mtx; std::lock_guard<std::mutex> lk(mtx);
    const std::string path = trades_dir + "/omega_livebook_closes.csv";
    bool newf;
    { std::ifstream chk(path);
      newf = !chk.good() || chk.peek() == std::ifstream::traits_type::eof(); }
    std::ofstream f(path, std::ios::app);
    if (!f.is_open()) return;
    if (newf) f << "exit_ts_unix,symbol,engine,side,entry_px,exit_px,size,"
                   "gross_pnl,net_pnl,slip_in,slip_out,commission,exit_reason\n";
    f << tr.exitTs << ',' << tr.symbol << ',' << tr.engine << ',' << tr.side << ','
      << tr.entryPrice << ',' << tr.exitPrice << ',' << tr.size << ','
      << tr.pnl << ',' << tr.net_pnl << ',' << tr.slippage_entry << ','
      << tr.slippage_exit << ',' << tr.commission << ',' << tr.exitReason << '\n';
}

} // namespace omega
