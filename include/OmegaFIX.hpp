#pragma once
// ==============================================================================
// OmegaFIX.hpp — BlackBull cTrader FIX 4.4 constants and symbol tables
//
// !! IMMUTABLE INFRASTRUCTURE — DO NOT MODIFY !!
//
// BlackBull FIX constraints (violations = ghost session + logon timeout loop):
//   264 (MarketDepth)   : MUST be 0 or 1. NEVER use 5 or any other value.
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

struct SymbolDef    { int id; const char* name; };
struct ExtSymbolDef { int id; const char* name; };

static SymbolDef OMEGA_SYMS[] = {
    { 2642, "US500.F" }, { 2643, "USTEC.F" }, { 2632, "USOIL.F" },
    { 4462, "VIX.F"   }, { 2638, "DX.F"    }, { 2637, "DJ30.F"  },
    {  110, "NAS100"  }, { 2660, "GOLD.F"  }, { 2631, "NGAS.F"  },
};
static const int OMEGA_NSYMS = 9;

static std::vector<ExtSymbolDef> g_ext_syms = {
    {0,"GER30"},{0,"UK100"},{0,"ESTX50"},{0,"XAGUSD"},
    {0,"EURUSD"},{0,"BRENT"},{0,"GBPUSD"},{0,"AUDUSD"},
    {0,"NZDUSD"},{0,"USDJPY"}
};

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
