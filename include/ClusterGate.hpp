#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClusterGate — cross-engine same-direction concurrency cap per asset cluster.
//
//   Why (2026-06-11): FvgCont30m, PeachyOrb and NasOrbRetrace all went LONG
//   NAS100 in the same session and lost together (-265 net) — three engines,
//   one correlated bet. Each engine is individually validated; nothing capped
//   the STACK. Same failure class as ChimeraCrypto's 31-May XLM stack
//   (CLUSTER_MAX_PER_SYMBOL fix): N same-direction positions on one cluster =
//   pure N-x leverage, zero diversification.
//
//   What it does: an engine calls ClusterGate::allow_entry(symbol, is_long)
//   right before opening. The gate counts OPEN same-direction positions whose
//   symbol is in the same cluster (via g_open_positions.snapshot_all()) and
//   blocks the entry when the count is already >= MAX_SAME_DIR (default 2).
//   Pure entry filter — never touches exits, never flattens. Blocks are
//   logged [CLUSTER-GATE] so the block rate is auditable from the journal.
//
//   Risk-control only: it does not alter any engine's signal logic. Note the
//   shadow-stat caveat: a blocked entry is unmeasured for that engine, so
//   per-engine shadow PF mixes in the gate's effect. Accepted by operator
//   2026-06-11 ("fix all the correlation/concurrency issues now").
//
//   Snapshot cost: snapshot_all() is mutex-guarded and walks every source
//   lambda — fine at entry frequency (a few calls per session), do NOT call
//   it per tick.
// ─────────────────────────────────────────────────────────────────────────────
#include "OpenPositionRegistry.hpp"
#include <cstdio>
#include <cstring>
#include <string>

namespace omega {

class ClusterGate {
public:
    static constexpr int MAX_SAME_DIR = 2;   // max concurrent same-direction positions per cluster

    // nullptr = symbol not in any capped cluster -> always allowed.
    static const char* cluster_of(const char* sym) {
        if (!sym) return nullptr;
        // US equity indices (the 2026-06-11 stack). Both broker + display names.
        static const char* us_eq[] = {"NAS100","USTEC","USTEC.F","US500","US500.F",
                                      "SPX500","DJ30","DJ30.F","US30","US30.F", nullptr};
        // EU equity indices — same beta, same session.
        static const char* eu_eq[] = {"GER40","DE40","UK100","ESTX50","EUSTX50", nullptr};
        for (int i = 0; us_eq[i]; ++i) if (!std::strcmp(sym, us_eq[i])) return "US_EQUITY";
        for (int i = 0; eu_eq[i]; ++i) if (!std::strcmp(sym, eu_eq[i])) return "EU_EQUITY";
        return nullptr;
    }

    // Count open same-direction positions in `sym`'s cluster; allow if < cap.
    static bool allow_entry(const char* sym, bool is_long, const char* engine_name) {
        const char* cl = cluster_of(sym);
        if (!cl) return true;
        int same_dir = 0;
        for (const auto& p : g_open_positions.snapshot_all()) {
            const char* pcl = cluster_of(p.symbol.c_str());
            if (!pcl || std::strcmp(pcl, cl)) continue;
            const bool p_long = (p.side == "LONG");
            if (p_long == is_long) ++same_dir;
        }
        if (same_dir >= MAX_SAME_DIR) {
            std::printf("[CLUSTER-GATE] BLOCK %s %s %s: %d same-direction open in %s (cap %d)\n",
                        engine_name ? engine_name : "?", sym, is_long ? "LONG" : "SHORT",
                        same_dir, cl, MAX_SAME_DIR);
            std::fflush(stdout);
            return false;
        }
        return true;
    }
};

} // namespace omega
