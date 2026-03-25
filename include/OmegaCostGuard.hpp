#pragma once
// ==============================================================================
// OmegaCostGuard.hpp
// ExecutionCostGuard — extracted from CrossAssetEngines.hpp so it can be
// included early in main.cpp before any templated lambdas reference it.
//
// MSVC requires identifiers used inside templated lambdas to be visible at the
// point of template definition, not just instantiation. Because dispatch() and
// dispatch_bracket() are generic lambdas (auto parameters), MSVC cannot find
// ExecutionCostGuard if it is only defined later via CrossAssetEngines.hpp.
//
// Solution: include this header before the lambdas are defined.
// CrossAssetEngines.hpp includes this header and skips re-defining the struct.
// ==============================================================================

#include <string>
#include <cstdio>

struct ExecutionCostGuard {
    // Conservative (high-end) cost floors per lot, in instrument price points.
    // Indices and oil have no per-lot commission (included in spread by broker).
    // FX/metals carry $6/lot round-trip commission.

    // Returns total estimated cost in USD for the given instrument and lot size.
    // spread_pts: current live bid-ask spread in price points (ask - bid).
    // lot: position size in lots.
    static double estimated_cost_usd(const char* sym, double spread_pts, double lot) noexcept {
        if (lot <= 0.0) return 9999.0;
        double commission_per_lot = 0.0;
        double slippage_pts       = 0.0;
        double tick_usd_per_lot   = 1.0;  // fallback

        const std::string s(sym);

        if (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.0002;
            tick_usd_per_lot   = 100000.0;
        } else if (s == "USDJPY") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.020;
            tick_usd_per_lot   = 667.0;
        } else if (s == "GOLD.F") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.30;
            tick_usd_per_lot   = 100.0;
        } else if (s == "XAGUSD") {
            commission_per_lot = 6.0;
            slippage_pts       = 0.02;
            tick_usd_per_lot   = 5000.0;
        } else if (s == "GER40") {
            commission_per_lot = 0.0;
            slippage_pts       = 0.80;
            tick_usd_per_lot   = 1.10;
        } else if (s == "UK100") {
            commission_per_lot = 0.0;
            slippage_pts       = 0.60;
            tick_usd_per_lot   = 1.27;
        } else if (s == "ESTX50") {
            commission_per_lot = 0.0;
            slippage_pts       = 0.70;
            tick_usd_per_lot   = 1.10;
        } else if (s == "US500.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 1.50;
            tick_usd_per_lot   = 50.0;
        } else if (s == "USTEC.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 2.00;
            tick_usd_per_lot   = 20.0;
        } else if (s == "NAS100") {
            commission_per_lot = 0.0;
            slippage_pts       = 2.50;
            tick_usd_per_lot   = 1.0;
        } else if (s == "DJ30.F") {
            commission_per_lot = 0.0;
            slippage_pts       = 3.00;
            tick_usd_per_lot   = 5.0;
        } else if (s == "USOIL.F" || s == "BRENT") {
            commission_per_lot = 0.0;
            slippage_pts       = 0.02;
            tick_usd_per_lot   = 1000.0;
        }

        const double spread_cost = spread_pts * tick_usd_per_lot * lot;
        const double slip_cost   = slippage_pts * tick_usd_per_lot * lot;
        const double comm_cost   = commission_per_lot * lot;
        return spread_cost + slip_cost + comm_cost;
    }

    // Returns expected gross profit in USD if TP is hit.
    static double expected_gross_usd(const char* sym, double tp_dist_pts, double lot) noexcept {
        if (lot <= 0.0 || tp_dist_pts <= 0.0) return 0.0;
        double tick_usd_per_lot = 1.0;
        const std::string s(sym);
        if      (s == "EURUSD" || s == "GBPUSD" || s == "AUDUSD" || s == "NZDUSD") tick_usd_per_lot = 100000.0;
        else if (s == "USDJPY")    tick_usd_per_lot = 667.0;
        else if (s == "GOLD.F")    tick_usd_per_lot = 100.0;
        else if (s == "XAGUSD")    tick_usd_per_lot = 5000.0;
        else if (s == "GER40")     tick_usd_per_lot = 1.10;
        else if (s == "UK100")     tick_usd_per_lot = 1.27;
        else if (s == "ESTX50")    tick_usd_per_lot = 1.10;
        else if (s == "US500.F")   tick_usd_per_lot = 50.0;
        else if (s == "USTEC.F")   tick_usd_per_lot = 20.0;
        else if (s == "NAS100")    tick_usd_per_lot = 1.0;
        else if (s == "DJ30.F")    tick_usd_per_lot = 5.0;
        else if (s == "USOIL.F" || s == "BRENT") tick_usd_per_lot = 1000.0;
        return tp_dist_pts * tick_usd_per_lot * lot;
    }

    // Gate: returns true if the trade is viable (expected gross > total cost × ratio).
    static bool is_viable(const char* sym, double spread_pts, double tp_dist_pts,
                           double lot, double cost_ratio_min = 1.5) noexcept {
        const double cost  = estimated_cost_usd(sym, spread_pts, lot);
        const double gross = expected_gross_usd(sym, tp_dist_pts, lot);
        if (gross < cost * cost_ratio_min) {
            printf("[COST-GUARD] BLOCKED %s spread=%.5f tp_dist=%.5f lot=%.2f"
                   " cost=$%.2f gross=$%.2f ratio=%.2f < %.1fx\n",
                   sym, spread_pts, tp_dist_pts, lot, cost, gross,
                   cost > 0 ? gross/cost : 0.0, cost_ratio_min);
            fflush(stdout);
            return false;
        }
        return true;
    }
};
