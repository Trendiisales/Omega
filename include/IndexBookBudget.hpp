#pragma once
// =============================================================================
//  IndexBookBudget.hpp -- global concurrent-exposure cap for the D1 INDEX BOOK
//  (IndexSeasonalEngine + CalendarTomEngine + CrossSectionalIndexEngine).
//
//  WHY THIS EXISTS: those D1 engines fire into handle_closed_trade directly and
//  NEVER consult PortfolioGovernor (which caps only the directional gold/bracket
//  zoo, max_total_campaigns=2). In shadow that is harmless. But on a live flip
//  there is no hard cap on CONCURRENT INDEX EXPOSURE: a Turn-of-Month window
//  (5 calendar-correlated TOM legs all LONG) overlapping the Tuesday/Friday
//  seasonal legs can stack a large simultaneous long-index beta the freq/DD
//  frontier's "equal-weight diversified" DD numbers do NOT account for (they
//  assume statistical diversification, not a worst-case concurrent stack).
//  Ref: memory omega-freq-dd-frontier (equal-weight orthogonal legs ~halve DD;
//  concurrent-cap=2 -> 1.8% DD in mr_breadth_book.py) + STEP3 architecture doc.
//
//  WHAT IT CAPS:
//    * max_net_long_legs  -- cap on (open long legs - open short legs). This is
//      the directional cluster that drives book drawdown. The XS market-neutral
//      L/S legs (long one index funded by a short another) net ~0 here by design
//      -> they are NOT throttled, preserving the orthogonal diversifier.
//    * max_concurrent_legs -- a total-legs backstop (long+short).
//
//  SHADOW vs LIVE (observe_only):
//    * observe_only=true (default, SHADOW): NEVER blocks -- only logs an
//      [IDX-BUDGET] would-block line. Keeps the live shadow ledger COMPLETE so
//      the >=30-trade G2 promotion gate accumulates on the true uncapped book,
//      while forward-validating exactly what the cap WOULD have throttled.
//      (Mirrors the L2-confirmation-gate pattern: inert in shadow/BT, measured.)
//    * observe_only=false (LIVE): enforces -- reserve() returns false when a new
//      long leg would breach the net-long cap, so the engine skips that entry.
//  Flip observe_only=false in the same change that sets shadow_mode=false on the
//  index book (live promotion). Backtests never touch the singleton -> inert ->
//  faithful BT figures reproduce unchanged.
//
//  THREAD-MODEL: the engine on_tick path is the single dispatch consumer
//  (one [ENG-DISPATCH] thread). Counters are std::atomic anyway for safety and
//  to match the codebase's AtomicL2 style. Header-only, zero deps.
// =============================================================================
#include <atomic>
#include <cstdio>

namespace omega {

enum class IdxDir : int { LONG = +1, SHORT = -1 };

class IndexBookBudget {
public:
    // --- config (set once at init; defaults sized to the frontier) ----------
    bool observe_only       = true;   // SHADOW default: measure, never block
    int  max_net_long_legs  = 6;      // cap on (long - short) open legs
    int  max_concurrent_legs= 12;     // backstop on total open legs

    static IndexBookBudget& g() noexcept { static IndexBookBudget b; return b; }

    // Try to reserve a slot for a new leg in direction `dir` opened by engine
    // `who` on `sym`. Returns true if the leg may open. In observe_only mode
    // always returns true (but logs a would-block when a cap is breached).
    bool reserve(IdxDir dir, const char* who, const char* sym) noexcept {
        const int add_long  = (dir == IdxDir::LONG)  ? 1 : 0;
        const int add_short = (dir == IdxDir::SHORT) ? 1 : 0;
        const int net_long  = (long_.load() + add_long) - (short_.load() + add_short);
        const int total     = long_.load() + short_.load() + 1;
        const bool breach = (net_long > max_net_long_legs) || (total > max_concurrent_legs);
        if (breach) {
            std::printf("[IDX-BUDGET] %s-block %s %s net_long=%d/%d total=%d/%d%s\n",
                        observe_only ? "would" : "HARD", who, sym,
                        net_long, max_net_long_legs, total, max_concurrent_legs,
                        observe_only ? " [observe]" : "");
            std::fflush(stdout);
            if (!observe_only) return false;     // LIVE: actually skip the entry
        }
        if (dir == IdxDir::LONG) long_.fetch_add(1); else short_.fetch_add(1);
        return true;
    }

    void release(IdxDir dir) noexcept {
        if (dir == IdxDir::LONG) { if (long_.load()  > 0) long_.fetch_sub(1); }
        else                     { if (short_.load() > 0) short_.fetch_sub(1); }
    }

    int open_long()  const noexcept { return long_.load(); }
    int open_short() const noexcept { return short_.load(); }
    int net_long()   const noexcept { return long_.load() - short_.load(); }

private:
    IndexBookBudget() = default;
    std::atomic<int> long_{0};
    std::atomic<int> short_{0};
};

} // namespace omega
