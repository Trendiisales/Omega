// =============================================================================
// scripts/s61_parsecheck.cpp
// -----------------------------------------------------------------------------
// Mac-side syntax-only harness for the S61 range_history persistence +
// S59 fill-spread reject cohort patches.
//
// Forces the compiler to parse the *live* engine headers in include/ (not the
// frozen backtest variants in backtest/<sym>_bt/). Quote-includes here resolve
// to include/ because this file lives in scripts/ and there is no local
// EurusdLondonOpenEngine.hpp shadow.
//
// omega_types.hpp must precede the engine headers because the engines
// reference g_news_blackout at file scope (defined in omega_types.hpp:332).
//
// USAGE (from omega_repo root):
//   g++ -std=c++17 -I include -DOMEGA_BACKTEST -fsyntax-only \
//       scripts/s61_parsecheck.cpp
//
// Expected: clean compile, no diagnostics. -DOMEGA_BACKTEST makes the
// persistence I/O path no-op (per S61 design), so this harness validates the
// non-I/O scaffolding (ctor, member decls, helper signatures) only. The I/O
// path is exercised at runtime by the live VPS, not by this parse-check.
// =============================================================================

// omega_types.hpp transitively pulls Windows headers and the entire macro
// stack (HANDLE, CTraderDepthClient, SymBarState, etc.), which won't compile
// stand-alone on macOS. Provide a minimal stub for the only file-scope global
// the engines actually reference at parse time -- g_news_blackout. The real
// definition lives in include/omega_types.hpp:332 and is in scope during the
// live build.
#include "OmegaNewsBlackout.hpp"
static omega::news::NewsBlackout g_news_blackout;

#include "EurusdLondonOpenEngine.hpp"
#include "UsdjpyAsianOpenEngine.hpp"
#include "GbpusdLondonOpenEngine.hpp"
#include "AudusdSydneyOpenEngine.hpp"
#include "NzdusdAsianOpenEngine.hpp"
#include "GoldMidScalperEngine.hpp"

// Force instantiation so default-ctor + member layout are emitted.
namespace {
    omega::EurusdLondonOpenEngine  _eur;
    omega::UsdjpyAsianOpenEngine   _jpy;
    omega::GbpusdLondonOpenEngine  _gbp;
    omega::AudusdSydneyOpenEngine  _aud;
    omega::NzdusdAsianOpenEngine   _nzd;
    omega::GoldMidScalperEngine    _gold;
}

int main() {
    return 0;
}
