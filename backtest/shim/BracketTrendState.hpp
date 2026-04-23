// HARNESS STUB — bracket_trend_bias returns 0 (no bias) unconditionally.
// Real implementation audits bracket state across sessions; harness assumes
// the gate is inert for the 2026-04-09..22 replay period. Flagged in audit.
#pragma once
#include <string>
inline int bracket_trend_bias(const std::string&) { return 0; }
