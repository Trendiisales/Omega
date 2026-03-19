#pragma once
// ==============================================================================
// SymbolConfig.hpp — per-symbol parameter store
//
// Every symbol is independently configurable from symbols.ini.
// No shared TP/SL inheritance. No generic fallback leakage.
// The manager loads symbols.ini at startup and provides O(1) lookup by name.
//
// symbols.ini format:
//   [SYMBOL_NAME]
//   MIN_RANGE=0.40
//   CONFIRM_OFFSET=0.05
//   MIN_STRUCTURE_MS=30000
//   BREAKOUT_FAIL_MS=5000
//   MIN_HOLD_MS=12000
//   TP_MULT=1.6
//   SL_MULT=1.0
//   MAX_SPREAD=2.0
// ==============================================================================
#include <string>
#include <unordered_map>

struct SymbolConfig
{
    // Bracket/breakout trigger geometry
    double min_range         = 0.0;
    double confirm_offset    = 0.0;
    int    min_structure_ms  = 0;

    // Position management
    int    breakout_fail_ms  = 0;
    int    min_hold_ms       = 0;
    int    max_hold_sec      = 0;    // per-symbol max hold — overrides global max_hold_sec

    // Risk/reward
    double tp_mult           = 1.5;
    double sl_mult           = 1.0;

    // Entry filter
    double max_spread        = 0.0;
    double min_edge_bp       = 0.0;
    double slippage_est_bp   = 0.0;

    // Supervisor config — controls which engine is permitted per regime
    bool   allow_bracket           = true;
    bool   allow_breakout          = true;
    double min_regime_confidence   = 0.55;
    double min_engine_win_margin   = 0.10;
    double min_winner_score        = 0.25;  // absolute floor: blocks low 0.12 vs 0.09 noise
    double min_bracket_score       = 0.35;  // bracket-specific floor (higher: places 2 orders)
    int    max_false_breaks        = 2;
    bool   bracket_in_quiet_comp   = true;
    bool   breakout_in_trend       = true;
    int    cooldown_fail_threshold = 20;  // raised from 3 — 3 fired on every normal fluctuation
    int    cooldown_duration_ms    = 120000;
};

// ==============================================================================
// SymbolConfigManager — loads symbols.ini, provides per-symbol lookup
// ==============================================================================
class SymbolConfigManager
{
public:
    // Load symbols.ini. Returns true on success.
    // Can be called again to hot-reload without restarting.
    bool load(const std::string& path);

    // Returns the config for the given symbol.
    // If symbol not found, returns default_config_ (all zeros / safe defaults).
    const SymbolConfig& get(const std::string& symbol) const;

    // True if the given symbol was found in the ini file.
    bool has(const std::string& symbol) const;

    // Number of symbols loaded
    size_t size() const { return configs_.size(); }

private:
    std::unordered_map<std::string, SymbolConfig> configs_;
    SymbolConfig default_config_;  // returned when symbol not found
};
