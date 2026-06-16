// IbkrExecGlobal.hpp -- single global IBKR execution engine instance.
//
// Guarded by OMEGA_WITH_IBKR (CMake defines it for the Omega target only). Other
// targets that include order_exec.hpp / engine_init.hpp (OmegaBacktest, etc.) do
// NOT define it, so they never pull the TWS API headers in and are unaffected by
// the migration. C++17/20 `inline` variable => exactly one g_ibkr_exec across the
// translation unit set, no ODR issues.
#pragma once

#ifdef OMEGA_WITH_IBKR
#include "IbkrExecutionEngine.hpp"
namespace omega {
inline IbkrExecutionEngine g_ibkr_exec;
}
#endif
