#pragma once
// ==============================================================================
// OmegaFIX.hpp — BlackBull cTrader FIX 4.4 constants and symbol tables
//
// ── DEPTH UPGRADE (2026-03-24) ──────────────────────────────────────────────
// 264=5 (5-level depth) replaces 264=1 (top-of-book only).
// Evidence for compatibility:
//   • BlackBull cTrader GUI shows live DoM with 4+ levels AND real size data
//     (confirmed from BRENT screenshot: bid 5000/10000/85000/500 visible)
//   • cTrader FIX 4.4 spec supports 264=N for N levels on DMA accounts
//   • L2Book parser already handles up to 5 levels (bid_count<5 check present)
//   • 271 (MDEntrySize) parsing already in place
//
// FALLBACK SAFETY:
//   If BlackBull rejects 264=5 (MarketDataRequestReject, 35=Y), the handler
//   in dispatch_fix() sets g_md_depth_fallback=true and re-subscribes at 264=1.
//   This prevents the ghost session loop. The fallback is permanent for the
//   session — if 264=5 is rejected, the session stays at 264=1 forever.
//
// ORIGINAL CONSTRAINTS (still enforced where applicable):
//   265 (MDUpdateType)  : MUST be 0 (full refresh only).
//   267 (NoMDEntryTypes): MUST be 2 — bid(0) + ask(1) only.
//   269 (MDEntryType)   : 0=bid, 1=ask ONLY.
//   263 (SubReqType)    : 1=subscribe, 2=unsubscribe ONLY.
//
// FIX message builders live in main.cpp under "// ── IMMUTABLE FIX SECTION ──"
// Symbol IDs and tables live here.
// ==============================================================================
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>

struct SymbolDef    { int id; const char* name; };
struct ExtSymbolDef { int id; const char* name; };

static SymbolDef OMEGA_SYMS[] = {
    { 2642, "US500.F" }, { 2643, "USTEC.F" }, { 2632, "USOIL.F" },
    { 4462, "VIX.F"   }, { 2638, "DX.F"    }, { 2637, "DJ30.F"  },
    {  110, "NAS100"  }, { 2660, "GOLD.F"  }, { 2631, "NGAS.F"  },
};
static const int OMEGA_NSYMS = 9;

static std::vector<ExtSymbolDef> g_ext_syms = {
    {0,"GER40"},{0,"UK100"},{0,"ESTX50"},{0,"XAGUSD"},
    {0,"EURUSD"},{0,"BRENT"},{0,"GBPUSD"},{0,"AUDUSD"},
    {0,"NZDUSD"},{0,"USDJPY"}
};

// ── Depth capability flags ───────────────────────────────────────────────────
// g_md_depth_ok: starts true (we request 264=5). Set false if broker rejects.
// g_md_depth_fallback: set true by 35=Y handler — triggers re-sub at 264=1.
// Both are written only from the quote thread (dispatch_fix context).
static std::atomic<bool> g_md_depth_ok{true};
static std::atomic<bool> g_md_depth_fallback{false};

static std::mutex g_symbol_map_mtx;
static std::unordered_map<int,std::string> g_id_to_sym;

static int symbol_name_to_id(const std::string& name) noexcept {
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        if (name == OMEGA_SYMS[i].name) return OMEGA_SYMS[i].id;
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    for (const auto& e : g_ext_syms)
        if (name == e.name && e.id > 0) return e.id;
    return 0;
}

static void build_id_map() {
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    g_id_to_sym.clear();
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        g_id_to_sym[OMEGA_SYMS[i].id] = OMEGA_SYMS[i].name;
    for (const auto& e : g_ext_syms)
        if (e.id > 0) g_id_to_sym[e.id] = e.name;
}

// =============================================================================
// L2Book — up to 5 levels per symbol, updated from FIX depth feed (264=5)
// When broker only sends 264=1, bid_count=ask_count=1 and depth methods still
// work correctly (they don't block trades when size data is absent).
// =============================================================================
struct L2Level { double price = 0.0; double size = 0.0; };

struct L2Book {
    L2Level bids[5];
    L2Level asks[5];
    int     bid_count  = 0;
    int     ask_count  = 0;

    // ── Imbalance 0..1 ───────────────────────────────────────────────────────
    // bid_vol / (bid_vol + ask_vol) across top `levels` levels.
    // 0.5 = balanced | >0.65 = bid-heavy | <0.35 = ask-heavy
    double imbalance(int levels = 5) const noexcept {
        double bs = 0.0, as = 0.0;
        const int bn = std::min(bid_count, levels);
        const int an = std::min(ask_count, levels);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        for (int i = 0; i < an; ++i) as += asks[i].size;
        const double tot = bs + as;
        return (tot > 0.0) ? (bs / tot) : 0.5;
    }

    // ── 3-level bid/ask ratio ────────────────────────────────────────────────
    // ratio > 1.5 = strong bid (long-friendly)
    // ratio < 0.66 = strong ask pressure (short-friendly)
    double ratio3() const noexcept {
        double bs = 0.0, as = 0.0;
        const int bn = std::min(bid_count, 3);
        const int an = std::min(ask_count, 3);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        for (int i = 0; i < an; ++i) as += asks[i].size;
        if (as <= 0.0) return (bs > 0.0) ? 9.9 : 1.0;
        return bs / as;
    }

    // ── Best wall: largest single level on a side (0=bid, 1=ask) ────────────
    double wall_size(int side) const noexcept {
        double mx = 0.0;
        if (side == 0) { for (int i=0;i<bid_count&&i<5;++i) if(bids[i].size>mx) mx=bids[i].size; }
        else           { for (int i=0;i<ask_count&&i<5;++i) if(asks[i].size>mx) mx=asks[i].size; }
        return mx;
    }

    // ── Depth support: enough liquidity to absorb our position ───────────────
    // Returns true if top-3 bid/ask sum >= position_lots * cushion.
    // When sizes are 0 (no depth data), always returns true — never blocks.
    bool depth_supports_long(double position_lots, double cushion = 3.0) const noexcept {
        double bs = 0.0;
        const int bn = std::min(bid_count, 3);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        if (bs <= 0.0) return true;
        return bs >= (position_lots * cushion);
    }
    bool depth_supports_short(double position_lots, double cushion = 3.0) const noexcept {
        double as = 0.0;
        const int an = std::min(ask_count, 3);
        for (int i = 0; i < an; ++i) as += asks[i].size;
        if (as <= 0.0) return true;
        return as >= (position_lots * cushion);
    }

    // ── Sweep detection ──────────────────────────────────────────────────────
    // True when last tick volume exceeds level-1 size * 1.5 AND imbalance
    // confirms the direction. A sweep + imbalance = reliable short-term signal.
    // sweep_vol: tag-271 size of the triggering tick.
    bool is_sweep_long(double sweep_vol) const noexcept {
        if (sweep_vol <= 0.0) return false;
        const double l1_ask = (ask_count > 0) ? asks[0].size : 0.0;
        if (l1_ask <= 0.0) return false;
        return (sweep_vol > l1_ask * 1.5) && (imbalance() > 0.55);
    }
    bool is_sweep_short(double sweep_vol) const noexcept {
        if (sweep_vol <= 0.0) return false;
        const double l1_bid = (bid_count > 0) ? bids[0].size : 0.0;
        if (l1_bid <= 0.0) return false;
        return (sweep_vol > l1_bid * 1.5) && (imbalance() < 0.45);
    }

    int depth_levels() const noexcept { return std::max(bid_count, ask_count); }
};
