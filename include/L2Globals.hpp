// =============================================================================
// L2Globals.hpp -- AtomicL2 type + per-symbol L2 globals
// -----------------------------------------------------------------------------
// Extracted from globals.hpp 2026-05-30 so engine headers can include this
// without picking up the cyclic IndexFlowEngine/etc. dependencies via
// globals.hpp. globals.hpp still includes this header (single source of truth).
//
// Uses `inline` (C++17) so each TU shares the same instance through linker
// merging -- avoids the prior `static` per-TU-instance pattern that left
// engine TUs unable to access the writer's L2 state.
// =============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// WHY NOT atomic<L2Book>: L2Book=168 bytes. atomic<T> over 16 bytes is NOT
// lock-free on x86-64 -- MSVC falls back to a hidden internal mutex, which is
// strictly worse. atomic<double>=8 bytes, aligned = genuine lock-free MOV.
struct AtomicL2 {
    std::atomic<double>   imbalance{0.5};       // bid_size/(bid_size+ask_size), FIX-fed since S8
    std::atomic<double>   microprice_bias{0.0}; // microprice - mid, signed
    std::atomic<bool>     has_data{false};      // true when book has non-zero sizes
    std::atomic<int64_t>  last_update_ms{0};    // epoch-ms of last L2 update

    bool fresh(int64_t now_ms, int64_t max_age_ms = 5000) const noexcept {
        const int64_t t = last_update_ms.load(std::memory_order_relaxed);
        return (t > 0) && ((now_ms - t) <= max_age_ms);
    }
};

inline AtomicL2 g_l2_gold;    // XAUUSD
inline AtomicL2 g_l2_sp;      // US500.F
inline AtomicL2 g_l2_nq;      // USTEC.F
inline AtomicL2 g_l2_cl;      // USOIL.F
inline AtomicL2 g_l2_eur;     // EURUSD
inline AtomicL2 g_l2_gbp;     // GBPUSD
inline AtomicL2 g_l2_aud;     // AUDUSD
inline AtomicL2 g_l2_nzd;     // NZDUSD
inline AtomicL2 g_l2_jpy;     // USDJPY
inline AtomicL2 g_l2_ger40;   // GER40
inline AtomicL2 g_l2_uk100;   // UK100
inline AtomicL2 g_l2_estx50;  // ESTX50
inline AtomicL2 g_l2_brent;   // BRENT
inline AtomicL2 g_l2_nas;     // NAS100
inline AtomicL2 g_l2_us30;    // DJ30.F

inline AtomicL2* get_atomic_l2(const std::string& sym) noexcept {
    if (sym=="XAUUSD") return &g_l2_gold;
    if (sym=="US500.F")  return &g_l2_sp;
    if (sym=="USTEC.F")  return &g_l2_nq;
    if (sym=="USOIL.F")  return &g_l2_cl;
    if (sym=="EURUSD")   return &g_l2_eur;
    if (sym=="GBPUSD")   return &g_l2_gbp;
    if (sym=="AUDUSD")   return &g_l2_aud;
    if (sym=="NZDUSD")   return &g_l2_nzd;
    if (sym=="USDJPY")   return &g_l2_jpy;
    if (sym=="GER40")    return &g_l2_ger40;
    if (sym=="UK100")    return &g_l2_uk100;
    if (sym=="ESTX50")   return &g_l2_estx50;
    if (sym=="BRENT")    return &g_l2_brent;
    if (sym=="NAS100")   return &g_l2_nas;
    if (sym=="DJ30.F")   return &g_l2_us30;
    return nullptr;
}
