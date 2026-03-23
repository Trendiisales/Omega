#pragma once
// ==============================================================================
// SymbolConfig.hpp — per-symbol parameter store
//
// SINGLE SOURCE OF TRUTH for all per-symbol tuning.
// symbols.ini is loaded at startup. apply_bracket() in main.cpp applies these
// AFTER configure(), so symbols.ini always wins over any hardcoded value.
// Never add tunable values to configure() without also adding them here.
//
// symbols.ini format:
//   [SYMBOL_NAME]
//   MIN_RANGE=0.40
//   MIN_STRUCTURE_MS=20000
//   BREAKOUT_FAIL_MS=12000
//   MIN_HOLD_MS=8000
//   TP_MULT=1.6          ; = RR for bracket engines
//   MAX_SPREAD=0.08
//   SLIPPAGE_BUFFER=0.08  ; price-unit slippage estimate (not basis points)
//   COOLDOWN_MS=30000     ; post-trade cooldown in ms
//   BRACKET_RR=3.0        ; bracket-specific RR (overrides TP_MULT for brackets)
//   BRACKET_LOOKBACK=30   ; tick lookback for structural range
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
    int    max_hold_sec      = 0;

    // Risk/reward
    double tp_mult           = 1.5;
    double sl_mult           = 1.0;

    // Entry filter
    double max_spread        = 0.0;
    double min_edge_bp       = 0.0;
    double slippage_est_bp   = 0.0;  // basis-point slippage — for breakout engines
    double min_breakout_pct  = 0.0;

    // Bracket-specific overrides (symbols.ini owns these, configure() is fallback only)
    double slippage_buffer   = 0.0;   // SLIPPAGE_BUFFER: price-unit slip for bracket cost model (0=use configure default)
    int    cooldown_ms       = 0;     // COOLDOWN_MS: post-trade cooldown (0=use configure default)
    double bracket_rr        = 0.0;   // BRACKET_RR: bracket R:R ratio (0=use TP_MULT)
    int    bracket_lookback  = 0;     // BRACKET_LOOKBACK: tick lookback for structure (0=use configure default)

    // Supervisor config
    bool   allow_bracket           = true;
    bool   allow_breakout          = true;
    double min_regime_confidence   = 0.55;
    double min_engine_win_margin   = 0.10;
    double min_winner_score        = 0.25;
    double min_bracket_score       = 0.35;
    int    max_false_breaks        = 2;
    bool   bracket_in_quiet_comp   = true;
    bool   breakout_in_trend       = true;
    int    cooldown_fail_threshold = 20;
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
