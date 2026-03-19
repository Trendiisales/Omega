#include "SymbolConfig.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

// ── helpers ───────────────────────────────────────────────────────────────────
static std::string trim(const std::string& s)
{
    size_t l = s.find_first_not_of(" \t\r\n");
    size_t r = s.find_last_not_of(" \t\r\n");
    return (l == std::string::npos) ? "" : s.substr(l, r - l + 1);
}

static double get_double(const std::unordered_map<std::string,std::string>& kv,
                         const std::string& key, double def)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stod(it->second); } catch(...) { return def; }
}

static int get_int(const std::unordered_map<std::string,std::string>& kv,
                   const std::string& key, int def)
{
    auto it = kv.find(key);
    if (it == kv.end()) return def;
    try { return std::stoi(it->second); } catch(...) { return def; }
}

// ── SymbolConfigManager::load ─────────────────────────────────────────────────
bool SymbolConfigManager::load(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[SYMCFG] Failed to open " << path << "\n";
        return false;
    }

    std::string line;
    std::string current_section;
    std::unordered_map<std::string,std::string> kv;

    auto flush = [&]() {
        if (current_section.empty()) return;

        SymbolConfig cfg;
        cfg.min_range        = get_double(kv, "MIN_RANGE",        0.0);
        cfg.confirm_offset   = get_double(kv, "CONFIRM_OFFSET",   0.0);
        cfg.min_structure_ms = get_int   (kv, "MIN_STRUCTURE_MS", 0);
        cfg.breakout_fail_ms = get_int   (kv, "BREAKOUT_FAIL_MS", 0);
        cfg.min_hold_ms      = get_int   (kv, "MIN_HOLD_MS",      0);
        cfg.max_hold_sec     = get_int   (kv, "MAX_HOLD_SEC",     0);
        cfg.tp_mult          = get_double(kv, "TP_MULT",          1.5);
        cfg.sl_mult          = get_double(kv, "SL_MULT",          1.0);
        cfg.max_spread       = get_double(kv, "MAX_SPREAD",       0.0);
        cfg.min_edge_bp      = get_double(kv, "MIN_EDGE_BP",      0.0);
        cfg.slippage_est_bp  = get_double(kv, "SLIPPAGE_EST_BP",  0.0);

        // Supervisor fields
        cfg.allow_bracket          = get_int   (kv, "ALLOW_BRACKET",          1) != 0;
        cfg.allow_breakout         = get_int   (kv, "ALLOW_BREAKOUT",         1) != 0;
        cfg.min_regime_confidence  = get_double(kv, "MIN_REGIME_CONFIDENCE",  0.55);
        cfg.min_engine_win_margin  = get_double(kv, "MIN_ENGINE_WIN_MARGIN",  0.10);
        cfg.min_winner_score       = get_double(kv, "MIN_WINNER_SCORE",       0.25);
        cfg.min_bracket_score      = get_double(kv, "MIN_BRACKET_SCORE",      0.35);
        cfg.max_false_breaks       = get_int   (kv, "MAX_FALSE_BREAKS",       2);
        cfg.bracket_in_quiet_comp  = get_int   (kv, "BRACKET_IN_QUIET_COMP",  1) != 0;
        cfg.breakout_in_trend      = get_int   (kv, "BREAKOUT_IN_TREND",      1) != 0;
        cfg.cooldown_fail_threshold= get_int   (kv, "COOLDOWN_FAIL_THRESHOLD",3);
        cfg.cooldown_duration_ms   = get_int   (kv, "COOLDOWN_DURATION_MS",   120000);

        configs_[current_section] = cfg;
        std::cout << "[SYMCFG] Loaded " << current_section
                  << " MIN_RANGE="    << cfg.min_range
                  << " TP_MULT="      << cfg.tp_mult
                  << " MAX_HOLD_SEC=" << cfg.max_hold_sec
                  << " MIN_EDGE_BP="  << cfg.min_edge_bp
                  << " MAX_SPREAD="   << cfg.max_spread << "\n";

        bool cfg_warn = false;
        if (cfg.max_spread <= 0.0) { std::cerr << "[SYMCFG] WARN " << current_section << ": MAX_SPREAD=0 — spread gate disabled\n";                cfg_warn = true; }
        if (cfg.tp_mult    <= 0.0) { std::cerr << "[SYMCFG] WARN " << current_section << ": TP_MULT=0 — TP will equal entry, trade closes flat\n";  cfg_warn = true; }
        if (cfg.sl_mult    <= 0.0) { std::cerr << "[SYMCFG] WARN " << current_section << ": SL_MULT=0 — SL will equal entry, trade closes flat\n";  cfg_warn = true; }
        if (!cfg_warn) std::cout << "[SYMCFG] " << current_section << " OK\n";

        kv.clear();
    };

    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line[0] == '[') {
            flush();
            current_section = trim(line.substr(1, line.size() - 2));
            continue;
        }

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, pos));
        std::string val = trim(line.substr(pos + 1));
        kv[key] = val;
    }

    flush();  // flush final section

    std::cout << "[SYMCFG] Loaded " << configs_.size() << " symbol configs from " << path << "\n";
    return !configs_.empty();
}

// ── SymbolConfigManager::get ──────────────────────────────────────────────────
const SymbolConfig& SymbolConfigManager::get(const std::string& symbol) const
{
    auto it = configs_.find(symbol);
    if (it != configs_.end()) return it->second;
    std::cerr << "[SYMCFG] WARNING: no config for '" << symbol << "' — using defaults\n";
    return default_config_;
}

bool SymbolConfigManager::has(const std::string& symbol) const
{
    return configs_.find(symbol) != configs_.end();
}
