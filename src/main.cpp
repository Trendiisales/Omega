// ==============================================================================
// OMEGA -- Commodities & Indices Trading System
// Strategy: Compression Breakout (CRTP engine, zero virtual dispatch)
// Broker: BlackBull Markets -- identical FIX stack to ChimeraMetals
// Primary: MES · MNQ · MCL  |  Confirmation: ES NQ CL VIX DX ZN YM RTY
// GUI: HTTP :7779 / WebSocket :7780
// ==============================================================================

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <atomic>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <direct.h>   // _mkdir on Windows
#include <chrono>
#include <memory>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <deque>
#include <cmath>
#include <csignal>
#include <functional>
#include <cstdint>
#include <cstring>

// ── Omega headers (flat -- all files in same directory on VPS) ────────────────
#include "OmegaTelemetryWriter.hpp"
#include "OmegaTradeLedger.hpp"

// ── Build version -- injected by CMake from git hash + build timestamp ────────
// If not set by CMake (manual compile), falls back to "dev-build".
#ifndef OMEGA_GIT_HASH
#  define OMEGA_GIT_HASH "dev-build"
#endif
#ifndef OMEGA_GIT_DATE
#  define OMEGA_GIT_DATE "unknown"
#endif
#ifndef OMEGA_BUILD_TIME
#  define OMEGA_BUILD_TIME __DATE__ " " __TIME__
#endif
static constexpr const char* OMEGA_VERSION = OMEGA_GIT_HASH;
static constexpr const char* OMEGA_BUILT   = OMEGA_BUILD_TIME;
static constexpr const char* OMEGA_COMMIT  = OMEGA_GIT_DATE;
#include "BreakoutEngine.hpp"
#include "SymbolEngines.hpp"      // SpEngine, NqEngine, OilEngine, MacroContext (includes BreakoutEngine.hpp)
#include "MacroRegimeDetector.hpp"
#include "OmegaTelemetryServer.hpp"
#include "GoldEngineStack.hpp"    // Multi-engine gold stack (ported from ChimeraMetals)
#include "LatencyEdgeEngines.hpp" // Co-location speed advantage engines (LeadLag, SpreadDisloc, EventComp)

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE g_singleton_mutex = NULL;

// ─────────────────────────────────────────────────────────────────────────────
// Config
// ─────────────────────────────────────────────────────────────────────────────
struct OmegaConfig {
    // FIX -- identical to ChimeraMetals
    std::string host       = "live-uk-eqx-02.p.c-trader.com";
    int         port       = 5211;
    std::string sender     = "live.blackbull.8077780";
    std::string target     = "cServer";
    std::string username   = "8077780";
    std::string password   = "Bowen6feb";
    int         heartbeat  = 30;

    std::string mode       = "SHADOW";

    // Breakout params
    double vol_thresh_pct        = 0.050;
    double tp_pct                = 0.400;
    double sl_pct                = 2.000;
    int    compression_lookback  = 50;
    int    baseline_lookback     = 200;
    double compression_threshold = 0.80;
    int    max_hold_sec          = 1500;
    int    min_entry_gap_sec     = 180;
    double max_spread_pct        = 0.05;
    double max_latency_ms        = 60.0;   // paper default: realistic remote VPS RTT without starving all entries
    double momentum_thresh_pct   = 0.05;   // momentum gate threshold
    double min_breakout_pct      = 0.25;   // min breakout size from comp edge
    int    max_trades_per_min    = 2;       // rate limiter

    // Risk
    double daily_loss_limit  = 200.0;
    int    max_consec_losses = 3;
    int    loss_pause_sec    = 300;
    int    max_open_positions = 1;
    bool   independent_symbols = true;  // true: risk/position gating is per-symbol (recommended)
    bool   enable_shadow_signal_audit = true;
    int    auto_disable_after_trades  = 10;
    bool   shadow_ustec_pilot_only    = false;  // false: multi-symbol SHADOW research; true: restrict to GOLD + USTEC pilot
    bool   shadow_research_mode       = false;  // false: paper/live-like behavior; true: discovery mode with relaxed filters
    double ustec_pilot_size           = 0.35;   // reduced NQ pilot size vs default 1.0
    int    ustec_pilot_min_gap_sec    = 60;     // more opportunities than default 90s shadow NQ gap
    bool   ustec_pilot_require_session = true;  // keep USTEC pilot out of session-closed tape
    bool   ustec_pilot_require_latency = true;  // enforce latency gate in shadow for USTEC pilot
    bool   ustec_pilot_block_risk_off  = true;  // skip USTEC pilot in RISK_OFF regime
    bool   enable_extended_symbols    = true;   // subscribe + trade additional opportunity symbols
    int    ext_ger30_id               = 0;
    int    ext_uk100_id               = 0;
    int    ext_estx50_id              = 0;
    int    ext_xagusd_id              = 0;
    int    ext_eurusd_id              = 0;
    int    ext_ukbrent_id             = 0;

    // Session UTC
    int session_start_utc = 7;
    int session_end_utc   = 21;
    bool session_asia     = true;  // enable Asia/Tokyo window 22:00-05:00 UTC

    // Gold breakout params (XAU -- tighter than indices, price-regime aware)
    double gold_tp_pct   = 0.30;   // 0.30% TP -- ~$9 on $3000 gold
    double gold_sl_pct   = 0.15;   // 0.15% SL -- tight, gold moves are decisive
    double gold_vol_thresh_pct = 0.04; // lower threshold -- gold is less volatile than oil
    bool   gold_use_crtp_engine = false; // false: use GoldEngineStack as primary gold executor

    // SP (US500) -- liquid, tight compression, better TP:SL than generic default
    double sp_tp_pct          = 0.600;  // 0.60% TP: clean SP breaks extend 0.5-0.8%
    double sp_sl_pct          = 0.350;  // 0.35% SL: above noise, cut failed breaks fast
    double sp_vol_thresh_pct  = 0.040;  // 0.04%: tighter than default, SP compression is real
    int    sp_min_gap_sec     = 300;    // 5min gap between signals

    // NQ (USTEC) -- higher beta, wider TP
    double nq_tp_pct          = 0.700;  // 0.70% TP: NQ extends further than SP
    double nq_sl_pct          = 0.400;  // 0.40% SL: slightly more room for NQ noise
    double nq_vol_thresh_pct  = 0.050;  // 0.05%: NQ needs a full vol spike
    int    nq_min_gap_sec     = 240;    // 4min gap

    // Oil (USOIL) -- fundamentally different: 1-2% typical moves
    double oil_tp_pct         = 1.200;  // 1.20% TP: oil runs 1-2% on clean breaks
    double oil_sl_pct         = 0.600;  // 0.60% SL: oil noise is 0.3-0.5% intraday
    double oil_vol_thresh_pct = 0.080;  // 0.08%: oil needs a bigger initial signal
    int    oil_min_gap_sec    = 360;    // 6min gap: oil can multi-spike on news
    int    oil_max_hold_sec   = 1800;   // 30min: oil moves are slower than indices

    // GUI
    int         gui_port   = 7779;
    int         ws_port    = 7780;
    int         trade_port = 5212;   // FIX trade connection (orders)
    std::string shadow_csv = "omega_shadow.csv";
    std::string shadow_signal_csv = "omega_shadow_signals.csv";
    std::string log_file   = "";   // if set, tee all stdout+stderr here
};

static OmegaConfig         g_cfg;
static std::atomic<bool>   g_running(true);

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
static OmegaTelemetryWriter      g_telemetry;
omega::OmegaTradeLedger          g_omegaLedger;      // extern in TelemetryServer.cpp
static omega::MacroRegimeDetector g_macroDetector;

// CRTP breakout engines -- typed per symbol (instrument-specific params + regime gating)
static omega::SpEngine    g_eng_sp("US500.F");   // S&P 500 -- regime-gated, cross-symbol guard
static omega::NqEngine    g_eng_nq("USTEC.F");   // Nasdaq  -- regime-gated, cross-symbol guard
static omega::OilEngine   g_eng_cl("USOIL.F");   // WTI Oil -- inventory window blocked
static omega::GoldEngine  g_eng_xau("GOLD.F");   // Gold -- safe-haven, inverse VIX logic
static omega::Us30Engine  g_eng_us30("DJ30.F");  // Dow Jones -- macro-gated typed engine
static omega::Nas100Engine g_eng_nas100("NAS100"); // Nasdaq cash -- independent from USTEC.F
static omega::BreakoutEngine g_eng_ger30("GER30");   // DAX proxy
static omega::BreakoutEngine g_eng_uk100("UK100");   // FTSE
static omega::BreakoutEngine g_eng_estx50("ESTX50"); // EuroStoxx50
static omega::BreakoutEngine g_eng_xag("XAGUSD");    // Silver
static omega::BreakoutEngine g_eng_eurusd("EURUSD"); // FX major
static omega::BreakoutEngine g_eng_brent("UKBRENT"); // Brent crude

// Shared macro context -- updated each tick, read by SP/NQ shouldTrade()
static omega::MacroContext g_macro_ctx;

// Multi-engine gold stack -- CompressionBreakout + ImpulseContinuation +
// SessionMomentum + VWAPSnapback + LiquiditySweepPro + LiquiditySweepPressure
// Runs in parallel with g_eng_xau (GoldEngine) on every GOLD.F tick.
static omega::gold::GoldEngineStack g_gold_stack;

// Co-location latency edge engines -- GoldSilverLeadLag + GoldSpreadDislocation
// + GoldEventCompression. Fully independent from GoldEngineStack and CRTP engines.
// These exploit the 0.3-4ms RTT advantage of the co-located VPS.
static omega::latency::LatencyEdgeStack g_le_stack;

// Book
static std::mutex                              g_book_mtx;
static std::unordered_map<std::string,double>  g_bids;
static std::unordered_map<std::string,double>  g_asks;

// RTT
static double              g_rtt_last = 0.0, g_rtt_p50 = 0.0, g_rtt_p95 = 0.0;
static std::deque<double>  g_rtts;
static int64_t             g_rtt_pending_ts = 0;
static std::string         g_rtt_pending_id;

// Governor counters
static int     g_gov_spread  = 0;
static int     g_gov_lat     = 0;
static int     g_gov_pnl     = 0;
static int     g_gov_pos     = 0;
static int     g_gov_consec  = 0;

struct SymbolRiskState {
    double daily_pnl = 0.0;
    int    consec_losses = 0;
    int64_t pause_until = 0;
};
struct ShadowQualityState {
    int     fast_loss_streak = 0;
    int64_t pause_until      = 0;
};
static std::mutex g_sym_risk_mtx;
static std::unordered_map<std::string, SymbolRiskState> g_sym_risk;
static std::unordered_map<std::string, ShadowQualityState> g_shadow_quality;

// Latency governor -- blocks trades when FIX RTT exceeds configured hard cap
struct Governor {
    bool checkLatency(double latency_ms, double cfg_max_ms) const noexcept {
        if (cfg_max_ms > 0.0) return latency_ms <= cfg_max_ms;
        return latency_ms <= 2000.0;  // fallback: only block dead connections
    }
};
static Governor g_governor;
static int     g_last_ledger_utc_day = -1;

// Trade connection (port 5212) — separate SSL from quote (port 5211)
static SSL*               g_trade_ssl  = nullptr;
static int                g_trade_sock = -1;
static std::atomic<bool>  g_trade_ready{false};
static std::mutex         g_trade_mtx;
static int                g_trade_seq  = 1;
static std::atomic<bool>  g_quote_ready{false};
static std::atomic<bool>  g_ext_md_refresh_needed{false};

// Shadow CSV
static std::ofstream g_shadow_csv;
static std::ofstream g_shadow_signal_csv;
static std::ofstream g_shadow_signal_event_csv;
static std::ofstream g_trade_close_csv;
static std::mutex    g_trade_close_csv_mtx;

struct ShadowSignalPos {
    bool active = false;
    std::string symbol;
    bool is_long = true;
    double entry = 0.0;
    double tp = 0.0;
    double sl = 0.0;
    int64_t entry_ts = 0;
    std::string verdict; // BLOCKED / ELIGIBLE
    std::string reason;
};
static std::mutex g_shadow_signal_mtx;
static std::vector<ShadowSignalPos> g_shadow_signal_positions;

struct PerfStats {
    int live_trades = 0;
    int live_wins = 0;
    int live_losses = 0;
    double live_pnl = 0.0;
    int shadow_trades = 0;
    int shadow_wins = 0;
    int shadow_losses = 0;
    double shadow_pnl = 0.0;
    bool disabled = false;
};
static std::mutex g_perf_mtx;
static std::unordered_map<std::string, PerfStats> g_perf;
static bool g_disable_gold_stack = false;

static std::string perf_key_from_trade(const omega::TradeRecord& tr) {
    if (tr.symbol == "GOLD.F" && tr.engine != "BreakoutEngine") return "GOLD_STACK";
    if (tr.symbol == "GOLD.F") return "GOLD_CRTP";
    return tr.symbol;
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr);
static std::string build_shadow_signal_event_csv_row(const ShadowSignalPos& p,
                                                     int64_t event_ts);
static std::string build_shadow_signal_close_csv_row(const ShadowSignalPos& p,
                                                     int64_t exit_ts,
                                                     double exit_px,
                                                     const char* exit_reason,
                                                     double pnl);
static void write_shadow_signal_event(const ShadowSignalPos& p);
static void write_shadow_signal_close(const ShadowSignalPos& p, double exit_px,
                                      const char* exit_reason, double pnl);
static void manage_shadow_signals_on_tick(const std::string& sym, double bid, double ask);

static void print_perf_stats() {
    std::lock_guard<std::mutex> lk(g_perf_mtx);
    if (g_perf.empty()) return;
    for (const auto& kv : g_perf) {
        const auto& k = kv.first;
        const auto& s = kv.second;
        std::cout << "[OMEGA-PERF] " << k
                  << " liveT=" << s.live_trades
                  << " livePnL=" << s.live_pnl
                  << " WR=" << (s.live_trades > 0 ? (100.0 * s.live_wins / s.live_trades) : 0.0)
                  << "% shadowT=" << s.shadow_trades
                  << " shadowPnL=" << s.shadow_pnl
                  << " disabled=" << (s.disabled ? 1 : 0) << "\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingTeeBuffer — mirrors stdout to a daily rolling log file
// Rotates at UTC midnight. Keeps LOG_KEEP_DAYS files, deletes older ones.
// File naming: logs/omega_YYYY-MM-DD.log
// ─────────────────────────────────────────────────────────────────────────────
class RollingTeeBuffer : public std::streambuf {
public:
    static constexpr int LOG_KEEP_DAYS = 5;

    explicit RollingTeeBuffer(std::streambuf* orig, const std::string& log_dir)
        : orig_(orig), log_dir_(log_dir)
    {
        open_today();
    }

    int overflow(int c) override {
        if (c == EOF) return !EOF;
        check_rotate();
        orig_->sputc(static_cast<char>(c));
        if (file_buf_) file_buf_->sputc(static_cast<char>(c));
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        check_rotate();
        orig_->sputn(s, n);
        if (file_buf_) file_buf_->sputn(s, n);
        return n;
    }

    std::string current_path() const { return current_path_; }
    bool is_open() const { return file_.is_open(); }

    void force_rotate_check() { check_rotate(); }

    void flush_and_close() {
        if (file_.is_open()) { file_.flush(); file_.close(); }
        file_buf_ = nullptr;
    }

private:
    std::streambuf* orig_;
    std::string     log_dir_;
    std::ofstream   file_;
    std::streambuf* file_buf_ = nullptr;
    std::string     current_path_;
    int             current_day_ = -1;

    static std::string utc_date_str() {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
        gmtime_s(&ti, &t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
        return buf;
    }

    static int utc_day_of_year() {
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        struct tm ti{};
        gmtime_s(&ti, &t);
        return ti.tm_yday;
    }

    void open_today() {
        if (file_.is_open()) { file_.flush(); file_.close(); file_buf_ = nullptr; }
        _mkdir(log_dir_.c_str());
        current_path_ = log_dir_ + "/omega_" + utc_date_str() + ".log";
        file_.open(current_path_, std::ios::app);
        file_buf_    = file_.is_open() ? file_.rdbuf() : nullptr;
        current_day_ = utc_day_of_year();
        purge_old_logs();
    }

    void check_rotate() {
        if (utc_day_of_year() != current_day_)
            open_today();
    }

    void purge_old_logs() {
        // Enumerate logs/omega_*.log and delete files older than LOG_KEEP_DAYS
        WIN32_FIND_DATAA fd{};
        std::string pattern = log_dir_ + "/omega_*.log";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        // Collect all matching filenames
        std::vector<std::string> files;
        do {
            files.push_back(log_dir_ + "/" + fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        // Sort ascending — oldest first
        std::sort(files.begin(), files.end());

        // Delete everything beyond the keep window
        while (static_cast<int>(files.size()) > LOG_KEEP_DAYS) {
            DeleteFileA(files.front().c_str());
            files.erase(files.begin());
        }
    }
};

static std::string utc_date_for_ts(int64_t ts);

class RollingCsvLogger {
public:
    static constexpr int LOG_KEEP_DAYS = 5;

    RollingCsvLogger(std::string dir, std::string stem, std::string header)
        : dir_(std::move(dir)), stem_(std::move(stem)), header_(std::move(header)) {}

    void append_row(int64_t ts_utc, const std::string& row) {
        std::lock_guard<std::mutex> lk(mtx_);
        open_for_ts(ts_utc);
        if (!file_.is_open()) return;
        file_ << row << '\n';
        file_.flush();
    }

    std::string current_path() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return current_path_;
    }

    void close() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
        current_path_.clear();
        current_date_.clear();
    }

private:
    mutable std::mutex mtx_;
    std::string dir_;
    std::string stem_;
    std::string header_;
    std::ofstream file_;
    std::string current_path_;
    std::string current_date_;

    void open_for_ts(int64_t ts_utc) {
        const std::string date = utc_date_for_ts(ts_utc);
        if (date == current_date_ && file_.is_open()) return;

        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(dir_), ec);

        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }

        current_date_ = date;
        current_path_ = dir_ + "/" + stem_ + "_" + date + ".csv";
        file_.open(current_path_, std::ios::app);
        if (!file_.is_open()) return;

        file_.seekp(0, std::ios::end);
        if (file_.tellp() == std::streampos(0))
            file_ << header_ << '\n';
        purge_old_logs();
    }

    void purge_old_logs() {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<fs::path> files;
        for (const auto& entry : fs::directory_iterator(fs::path(dir_), ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto name = entry.path().filename().string();
            if (name.rfind(stem_ + "_", 0) != 0) continue;
            if (entry.path().extension() != ".csv") continue;
            files.push_back(entry.path());
        }
        std::sort(files.begin(), files.end());
        while (static_cast<int>(files.size()) > LOG_KEEP_DAYS) {
            fs::remove(files.front(), ec);
            files.erase(files.begin());
        }
    }
};

static RollingTeeBuffer* g_tee_buf   = nullptr;
static std::streambuf*   g_orig_cout = nullptr;
static std::unique_ptr<RollingCsvLogger> g_daily_trade_close_log;
static std::unique_ptr<RollingCsvLogger> g_daily_gold_trade_close_log;
static std::unique_ptr<RollingCsvLogger> g_daily_shadow_trade_log;
static std::unique_ptr<RollingCsvLogger> g_daily_shadow_signal_log;
static std::unique_ptr<RollingCsvLogger> g_daily_shadow_signal_event_log;

// FIX recv buffer
static std::string g_recv_buf;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static int64_t nowSec() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string timestamp() {
    const auto tp = std::chrono::system_clock::now();
    const auto t  = std::chrono::system_clock::to_time_t(tp);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        tp.time_since_epoch()) % 1000;
    struct tm ti; gmtime_s(&ti, &t);
    std::ostringstream o;
    o << std::put_time(&ti, "%Y%m%d-%H:%M:%S")
      << '.' << std::setw(3) << std::setfill('0') << ms.count();
    return o.str();
}

static void utc_tm_from_ts(int64_t ts, struct tm& ti) noexcept {
    const auto t = static_cast<time_t>(ts);
    gmtime_s(&ti, &t);
}

static std::string utc_iso8601(int64_t ts) {
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    std::ostringstream o;
    o << std::put_time(&ti, "%Y-%m-%dT%H:%M:%SZ");
    return o.str();
}

static std::string utc_date_for_ts(int64_t ts) {
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
    return buf;
}

static const char* utc_weekday_name(int64_t ts) noexcept {
    static const char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    struct tm ti{};
    utc_tm_from_ts(ts, ti);
    const int idx = (ti.tm_wday >= 0 && ti.tm_wday < 7) ? ti.tm_wday : 0;
    return DAYS[idx];
}

static std::string csv_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

static std::string log_root_dir() {
    if (!g_cfg.log_file.empty()) {
        const size_t slash = g_cfg.log_file.find_last_of("/\\");
        if (slash != std::string::npos) return g_cfg.log_file.substr(0, slash);
    }
    return "logs";
}

static std::string resolve_audit_log_path(const std::string& configured_path,
                                          const std::string& default_relative_path) {
    namespace fs = std::filesystem;
    if (configured_path.empty()) return log_root_dir() + "/" + default_relative_path;
    const fs::path p(configured_path);
    if (p.is_absolute() || p.has_parent_path()) return configured_path;
    const fs::path base = fs::path(log_root_dir()) / "shadow" / p;
    return base.generic_string();
}

static void ensure_parent_dir(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path p(path);
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path(), ec);
}

static std::string build_shadow_signal_event_csv_row(const ShadowSignalPos& p,
                                                     int64_t event_ts) {
    std::ostringstream o;
    o << event_ts
      << ',' << csv_quote(utc_iso8601(event_ts))
      << ',' << csv_quote(utc_weekday_name(event_ts))
      << ',' << csv_quote(p.symbol)
      << ',' << csv_quote(p.is_long ? "LONG" : "SHORT")
      << std::fixed << std::setprecision(4)
      << ',' << p.entry
      << ',' << p.tp
      << ',' << p.sl
      << ',' << csv_quote(p.verdict)
      << ',' << csv_quote(p.reason);
    return o.str();
}

static std::string build_shadow_signal_close_csv_row(const ShadowSignalPos& p,
                                                     int64_t exit_ts,
                                                     double exit_px,
                                                     const char* exit_reason,
                                                     double pnl) {
    const int64_t hold_sec = std::max<int64_t>(0, exit_ts - p.entry_ts);
    std::ostringstream o;
    o << p.entry_ts
      << ',' << csv_quote(utc_iso8601(p.entry_ts))
      << ',' << csv_quote(utc_weekday_name(p.entry_ts))
      << ',' << exit_ts
      << ',' << csv_quote(utc_iso8601(exit_ts))
      << ',' << csv_quote(utc_weekday_name(exit_ts))
      << ',' << csv_quote(p.symbol)
      << ',' << csv_quote(p.is_long ? "LONG" : "SHORT")
      << std::fixed << std::setprecision(4)
      << ',' << p.entry
      << ',' << exit_px
      << ',' << p.tp
      << ',' << p.sl
      << ',' << pnl
      << ',' << hold_sec
      << ',' << csv_quote(p.verdict)
      << ',' << csv_quote(p.reason)
      << ',' << csv_quote(exit_reason ? exit_reason : "");
    return o.str();
}

static void write_shadow_signal_event(const ShadowSignalPos& p) {
    const int64_t now = p.entry_ts > 0 ? p.entry_ts : nowSec();
    if (g_shadow_signal_event_csv.is_open()) {
        g_shadow_signal_event_csv
            << now
            << ',' << utc_iso8601(now)
            << ',' << utc_weekday_name(now)
            << ',' << p.symbol
            << ',' << (p.is_long ? "LONG" : "SHORT")
            << ',' << std::fixed << std::setprecision(4) << p.entry
            << ',' << p.tp
            << ',' << p.sl
            << ',' << p.verdict
            << ',' << p.reason << '\n';
        g_shadow_signal_event_csv.flush();
    }
    if (g_daily_shadow_signal_event_log) {
        g_daily_shadow_signal_event_log->append_row(
            now, build_shadow_signal_event_csv_row(p, now));
    }
}

static void write_shadow_signal_close(const ShadowSignalPos& p, double exit_px,
                                      const char* exit_reason, double pnl) {
    const int64_t now = nowSec();
    if (g_shadow_signal_csv.is_open()) {
        g_shadow_signal_csv
            << p.entry_ts << ',' << p.symbol << ',' << (p.is_long ? "LONG" : "SHORT")
            << ',' << p.entry << ',' << exit_px << ',' << p.tp << ',' << p.sl
            << ',' << pnl << ',' << (now - p.entry_ts)
            << ',' << p.verdict << ',' << p.reason << ',' << exit_reason << '\n';
        g_shadow_signal_csv.flush();
    }
    if (g_daily_shadow_signal_log) {
        g_daily_shadow_signal_log->append_row(
            now, build_shadow_signal_close_csv_row(p, now, exit_px, exit_reason, pnl));
    }
}

static void manage_shadow_signals_on_tick(const std::string& sym, double bid, double ask) {
    if (!g_cfg.enable_shadow_signal_audit) return;
    std::lock_guard<std::mutex> lk(g_shadow_signal_mtx);
    if (g_shadow_signal_positions.empty()) return;
    const double mid = (bid + ask) * 0.5;
    const int64_t now = nowSec();
    constexpr int SHADOW_MAX_HOLD_SEC = 600;
    for (auto& p : g_shadow_signal_positions) {
        if (!p.active || p.symbol != sym) continue;
        const bool tp_hit = p.is_long ? (mid >= p.tp) : (mid <= p.tp);
        const bool sl_hit = p.is_long ? (mid <= p.sl) : (mid >= p.sl);
        const bool to_hit = (now - p.entry_ts) >= SHADOW_MAX_HOLD_SEC;
        if (!tp_hit && !sl_hit && !to_hit) continue;
        const double exit_px = tp_hit ? p.tp : (sl_hit ? p.sl : mid);
        const double pnl = p.is_long ? (exit_px - p.entry) : (p.entry - exit_px);
        write_shadow_signal_close(p, exit_px, tp_hit ? "TP_HIT" : (sl_hit ? "SL_HIT" : "TIMEOUT"), pnl);
        {
            std::lock_guard<std::mutex> pk(g_perf_mtx);
            auto& ps = g_perf[p.symbol + "_SHADOW_" + p.verdict];
            ps.shadow_trades++;
            ps.shadow_pnl += pnl;
            if (pnl > 0) ps.shadow_wins++; else ps.shadow_losses++;
        }
        p.active = false;
    }
}

// Parse FIX SendingTime (tag 52) "YYYYMMDD-HH:MM:SS.mmm" -> microseconds since epoch
// Returns 0 on parse failure. Used for per-tick latency measurement.
static int64_t parse_fix_time_us(const std::string& ts) noexcept {
    if (ts.size() < 17) return 0;
    try {
        struct tm ti{};
        ti.tm_year = std::stoi(ts.substr(0,4))  - 1900;
        ti.tm_mon  = std::stoi(ts.substr(4,2))  - 1;
        ti.tm_mday = std::stoi(ts.substr(6,2));
        ti.tm_hour = std::stoi(ts.substr(9,2));
        ti.tm_min  = std::stoi(ts.substr(12,2));
        ti.tm_sec  = std::stoi(ts.substr(15,2));
        int ms = 0;
        if (ts.size() >= 21 && ts[17] == '.') ms = std::stoi(ts.substr(18,3));
#ifdef _WIN32
        const int64_t epoch_s = static_cast<int64_t>(_mkgmtime(&ti));
#else
        const int64_t epoch_s = static_cast<int64_t>(timegm(&ti));
#endif
        if (epoch_s < 0) return 0;
        return epoch_s * 1000000LL + static_cast<int64_t>(ms) * 1000LL;
    } catch (...) { return 0; }
}

static std::string extract_tag(const std::string& msg, const char* tag) {
    const std::string pat = std::string(tag) + '=';
    const size_t pos = msg.find(pat);
    if (pos == std::string::npos) return {};
    const size_t s = pos + pat.size();
    const size_t e = msg.find('\x01', s);
    if (e == std::string::npos) return {};
    return msg.substr(s, e - s);
}

static std::string compute_checksum(const std::string& body) {
    unsigned int sum = 0;
    for (unsigned char c : body) sum += c;
    sum %= 256u;
    char buf[4]; snprintf(buf, sizeof(buf), "%03u", sum);
    return buf;
}

static std::string wrap_fix(const std::string& body) {
    const std::string with_l = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    return with_l + "10=" + compute_checksum(with_l) + "\x01";
}

static int g_quote_seq = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Live Order Dispatch — 35=D NewOrderSingle
// ─────────────────────────────────────────────────────────────────────────────
// Design: SHADOW mode = zero orders sent, full paper audit trail.
//         LIVE mode   = real 35=D sent on trade session (port 5212).
//
// Safety architecture:
//   1. send_live_order() checks mode == "LIVE" before sending — impossible to
//      accidentally fire in SHADOW mode regardless of any other code path.
//   2. g_trade_ready atomic must be true (trade session logged in).
//   3. g_trade_ssl must be non-null and write must succeed.
//   4. Every order is logged to console AND the order log before sending.
//   5. Order tracking: g_live_orders maps clOrdId -> symbol+side for ACK matching.
//   6. FIX rejects (35=3, 35=j) on trade session are already logged in trade_loop.
//
// BlackBull/cTrader FIX 4.4 NewOrderSingle fields:
//   35=D, 11=clOrdId, 55=symbolId, 54=side(1=Buy/2=Sell),
//   38=qty, 40=ordType(1=Market), 59=timeInForce(3=IOC),
//   60=transactTime
//
// Position management (TP/SL/TIMEOUT) is handled entirely by the engines in
// software — we do NOT send bracket orders. This is correct for CFD/futures
// market makers like BlackBull where bracket orders are unreliable.
// The engine closes via another Market order when TP/SL triggers.
// ─────────────────────────────────────────────────────────────────────────────

struct LiveOrderRecord {
    std::string clOrdId;
    std::string symbol;
    std::string side;     // "LONG" / "SHORT"
    double      qty   = 0;
    double      price = 0;  // mid at time of order
    int64_t     ts    = 0;
    bool        acked = false;
    bool        rejected = false;
};

static std::mutex g_live_orders_mtx;
static std::unordered_map<std::string, LiveOrderRecord> g_live_orders;
static std::atomic<int> g_order_id_counter{1};

// Look up numeric symbol ID from name — defined after OMEGA_SYMS below
static int symbol_name_to_id(const std::string& name);

static std::string build_new_order_single(int seq, const std::string& clOrdId,
                                          int sym_id, bool is_long,
                                          double qty) {
    std::ostringstream b;
    b << "35=D\x01"
      << "49=" << g_cfg.sender << "\x01"
      << "56=" << g_cfg.target << "\x01"
      << "50=TRADE\x01" << "57=TRADE\x01"
      << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01"
      << "11=" << clOrdId << "\x01"           // ClOrdID
      << "55=" << sym_id  << "\x01"           // Symbol (numeric ID)
      << "54=" << (is_long ? "1" : "2") << "\x01"  // Side: 1=Buy 2=Sell
      << "38=" << std::fixed << std::setprecision(2) << qty << "\x01"  // OrderQty
      << "40=1\x01"                           // OrdType=Market
      << "59=3\x01"                           // TimeInForce=IOC
      << "60=" << timestamp() << "\x01";      // TransactTime
    return wrap_fix(b.str());
}

// Send a live market order. Does nothing in SHADOW mode.
// Returns clOrdId on success, empty string on failure/shadow.
static std::string send_live_order(const std::string& symbol, bool is_long,
                                   double qty, double mid_price) {
    // Hard SHADOW gate — never send in shadow regardless of anything else
    if (g_cfg.mode != "LIVE") return {};

    if (!g_trade_ready.load()) {
        std::cerr << "[ORDER] BLOCKED — trade session not ready\n";
        return {};
    }

    const int sym_id = symbol_name_to_id(symbol);
    if (sym_id <= 0) {
        std::cerr << "[ORDER] BLOCKED — no numeric ID for symbol " << symbol << "\n";
        return {};
    }

    const std::string clOrdId = "OM-" + std::to_string(nowSec())
                               + "-" + std::to_string(g_order_id_counter++);

    std::string msg;
    {
        std::lock_guard<std::mutex> lk(g_trade_mtx);
        if (!g_trade_ssl) {
            std::cerr << "[ORDER] BLOCKED — trade SSL null\n";
            return {};
        }
        msg = build_new_order_single(g_trade_seq++, clOrdId, sym_id, is_long, qty);
        const int w = SSL_write(g_trade_ssl, msg.c_str(), static_cast<int>(msg.size()));
        if (w <= 0) {
            std::cerr << "[ORDER] SSL_write failed for " << symbol << "\n";
            return {};
        }
    }

    // Record for ACK tracking
    {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        LiveOrderRecord rec;
        rec.clOrdId = clOrdId;
        rec.symbol  = symbol;
        rec.side    = is_long ? "LONG" : "SHORT";
        rec.qty     = qty;
        rec.price   = mid_price;
        rec.ts      = nowSec();
        g_live_orders[clOrdId] = rec;
    }

    std::cout << "\033[1;33m[ORDER-SENT] " << symbol
              << " " << (is_long ? "BUY" : "SELL")
              << " qty=" << qty
              << " mid=" << std::fixed << std::setprecision(4) << mid_price
              << " clOrdId=" << clOrdId
              << "\033[0m\n";
    std::cout.flush();

    return clOrdId;
}

// Handle ExecutionReport (35=8) from trade session
static void handle_execution_report(const std::string& msg) {
    const std::string clOrdId  = extract_tag(msg, "11");
    const std::string ordStatus= extract_tag(msg, "39"); // 0=New,1=PartFill,2=Fill,8=Rejected
    const std::string execType = extract_tag(msg, "150");
    const std::string text     = extract_tag(msg, "58");
    const std::string symbol   = extract_tag(msg, "55");
    const std::string side     = extract_tag(msg, "54");
    const std::string lastPx   = extract_tag(msg, "31");
    const std::string lastQty  = extract_tag(msg, "32");

    std::cout << "[ORDER-ACK] clOrdId=" << clOrdId
              << " status=" << ordStatus
              << " execType=" << execType
              << " sym=" << symbol
              << " side=" << side
              << " lastPx=" << lastPx
              << " lastQty=" << lastQty
              << (text.empty() ? "" : " text=" + text) << "\n";
    std::cout.flush();

    if (!clOrdId.empty()) {
        std::lock_guard<std::mutex> lk(g_live_orders_mtx);
        auto it = g_live_orders.find(clOrdId);
        if (it != g_live_orders.end()) {
            if (ordStatus == "8") {
                it->second.rejected = true;
                std::cerr << "[ORDER-REJECT] " << it->second.symbol
                          << " " << it->second.side
                          << " REJECTED text=" << text << "\n";
                std::cerr.flush();
            } else if (ordStatus == "0" || ordStatus == "1" || ordStatus == "2") {
                it->second.acked = true;
            }
        }
    }
}

static std::string build_logon(int seq, const char* subID) {
    std::ostringstream b;
    b << "35=A\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01" << "34=" << seq << "\x01"
      << "52=" << timestamp() << "\x01" << "98=0\x01" << "108=" << g_cfg.heartbeat << "\x01"
      << "141=Y\x01" << "553=" << g_cfg.username << "\x01" << "554=" << g_cfg.password << "\x01";
    return wrap_fix(b.str());
}

// ─────────────────────────────────────────────────────────────────────────────
// BlackBull symbol ID map.
// Primary symbols ship with bootstrap IDs; extended symbols can be learned at runtime from SecurityList.
// Primary trading: US500.F, USTEC.F, USOIL.F
// Confirmation:    VIX.F, DX.F, DJ30.F, NAS100, GOLD.F, NGAS.F, ES, DX
// ─────────────────────────────────────────────────────────────────────────────
struct SymbolDef { int id; const char* name; };
static SymbolDef OMEGA_SYMS[] = {
    // Primary -- traded
    { 2642, "US500.F"  },   // S&P 500 futures
    { 2643, "USTEC.F"  },   // Nasdaq futures
    { 2632, "USOIL.F"  },   // Oil futures
    // Confirmation -- regime/context only
    { 4462, "VIX.F"    },
    { 2638, "DX.F"     },
    { 2637, "DJ30.F"   },
    {  110, "NAS100"   },
    { 2660, "GOLD.F"   },
    { 2631, "NGAS.F"   },
    // ES (3225) and DX (3173) removed -- not valid on BlackBull, generated FIX rejects
};
static const int OMEGA_NSYMS = 9;
struct ExtSymbolDef { int id; const char* name; };
static std::vector<ExtSymbolDef> g_ext_syms = {
    {0, "GER30"}, {0, "UK100"}, {0, "ESTX50"}, {0, "XAGUSD"}, {0, "EURUSD"}, {0, "UKBRENT"}
};

// Runtime ID->name map built at startup from OMEGA_SYMS
static std::mutex g_symbol_map_mtx;
static std::unordered_map<int, std::string> g_id_to_sym;

// Look up numeric symbol ID from name (reverse of g_id_to_sym)
static int symbol_name_to_id(const std::string& name) {
    for (int i = 0; i < OMEGA_NSYMS; ++i) {
        if (name == OMEGA_SYMS[i].name) return OMEGA_SYMS[i].id;
    }
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    for (const auto& e : g_ext_syms) {
        if (name == e.name && e.id > 0) return e.id;
    }
    return 0;
}

static void build_id_map() {
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    g_id_to_sym.clear();
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        g_id_to_sym[OMEGA_SYMS[i].id] = OMEGA_SYMS[i].name;
    for (const auto& e : g_ext_syms) {
        if (e.id > 0) g_id_to_sym[e.id] = e.name;
    }
}

static std::string build_marketdata_req(int seq) {
    std::ostringstream b;
    b << "35=V\x01"
      << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-001\x01" << "263=1\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << OMEGA_NSYMS << "\x01";
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        b << "55=" << OMEGA_SYMS[i].id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}

static std::string build_marketdata_req_extended(int seq) {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
        ids.reserve(g_ext_syms.size());
        for (const auto& e : g_ext_syms) if (e.id > 0) ids.push_back(e.id);
    }
    if (ids.empty()) return {};

    std::ostringstream b;
    b << "35=V\x01"
      << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=QUOTE\x01" << "57=QUOTE\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "262=OMEGA-MD-EXT-" << seq << "\x01" << "263=1\x01" << "264=1\x01" << "265=0\x01"
      << "146=" << ids.size() << "\x01";
    for (int id : ids) b << "55=" << id << "\x01";
    b << "267=2\x01" << "269=0\x01" << "269=1\x01";
    return wrap_fix(b.str());
}

static std::string build_security_list_request(int seq, const std::string& req_id) {
    std::ostringstream b;
    b << "35=x\x01"
      << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=TRADE\x01" << "57=TRADE\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "320=" << req_id << "\x01"
      << "559=0\x01";  // optional 55 omitted => request full list (per cTrader FIX docs, inferred)
    return wrap_fix(b.str());
}

static std::vector<std::pair<int, std::string>> parse_security_list_entries(const std::string& msg) {
    std::vector<std::pair<int, std::string>> out;
    int current_id = 0;

    size_t pos = 0;
    while (pos < msg.size()) {
        const size_t eq = msg.find('=', pos);
        if (eq == std::string::npos) break;
        const size_t soh = msg.find('\x01', eq + 1);
        if (soh == std::string::npos) break;

        const std::string tag = msg.substr(pos, eq - pos);
        const std::string val = msg.substr(eq + 1, soh - (eq + 1));

        if (tag == "55") {
            try {
                current_id = std::stoi(val);
            } catch (...) {
                current_id = 0;
            }
        } else if (tag == "1007" && current_id > 0 && !val.empty()) {
            out.emplace_back(current_id, val);
            current_id = 0;
        }
        pos = soh + 1;
    }
    return out;
}

static bool apply_security_list_symbol_map(const std::vector<std::pair<int, std::string>>& entries) {
    bool ext_changed = false;
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    for (const auto& entry : entries) {
        const int id = entry.first;
        const std::string& name = entry.second;
        if (id <= 0 || name.empty()) continue;

        g_id_to_sym[id] = name;

        for (int i = 0; i < OMEGA_NSYMS; ++i) {
            if (name == OMEGA_SYMS[i].name && OMEGA_SYMS[i].id != id) {
                std::cout << "[OMEGA-SECURITY] primary id update " << name
                          << " " << OMEGA_SYMS[i].id << " -> " << id << "\n";
                OMEGA_SYMS[i].id = id;
            }
        }

        for (size_t i = 0; i < g_ext_syms.size(); ++i) {
            auto& ext = g_ext_syms[i];
            if (name != ext.name) continue;
            if (ext.id == id) break;

            std::cout << "[OMEGA-SECURITY] learned ext id " << name
                      << " -> " << id << "\n";
            ext.id = id;
            ext_changed = true;
            switch (i) {
                case 0: g_cfg.ext_ger30_id = id; break;
                case 1: g_cfg.ext_uk100_id = id; break;
                case 2: g_cfg.ext_estx50_id = id; break;
                case 3: g_cfg.ext_xagusd_id = id; break;
                case 4: g_cfg.ext_eurusd_id = id; break;
                case 5: g_cfg.ext_ukbrent_id = id; break;
                default: break;
            }
            break;
        }
    }
    return ext_changed;
}

static std::string build_heartbeat(int seq, const char* subID, const char* trid = nullptr) {
    std::ostringstream b;
    b << "35=0\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01";
    if (trid && *trid) b << "112=" << trid << "\x01";
    return wrap_fix(b.str());
}

static std::string build_test_request(int seq, const char* subID, const std::string& trid) {
    std::ostringstream b;
    b << "35=1\x01" << "49=" << g_cfg.sender << "\x01" << "56=" << g_cfg.target << "\x01"
      << "50=" << subID << "\x01" << "57=" << subID << "\x01"
      << "34=" << seq << "\x01" << "52=" << timestamp() << "\x01"
      << "112=" << trid << "\x01";
    return wrap_fix(b.str());
}

// ─────────────────────────────────────────────────────────────────────────────
// SSL connect (identical to ChimeraMetals -- untouched)
// ─────────────────────────────────────────────────────────────────────────────
static SSL* connect_ssl(const std::string& host, int port, int& sock_out) {
    struct addrinfo hints{}, *result = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result) != 0)
        return nullptr;
    SOCKET sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (sock == INVALID_SOCKET) { freeaddrinfo(result); return nullptr; }
    if (connect(sock, result->ai_addr, static_cast<int>(result->ai_addrlen)) != 0) {
        freeaddrinfo(result); closesocket(sock); return nullptr;
    }
    freeaddrinfo(result);
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE, reinterpret_cast<const char*>(&flag), sizeof(flag));
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { closesocket(sock); return nullptr; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); closesocket(sock); return nullptr; }
    SSL_set_fd(ssl, static_cast<int>(sock));
    if (SSL_connect(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(ssl); SSL_CTX_free(ctx); closesocket(sock); return nullptr;
    }
    sock_out = static_cast<int>(sock);
    return ssl;
}

// ─────────────────────────────────────────────────────────────────────────────
// RTT
// ─────────────────────────────────────────────────────────────────────────────
static void rtt_record(double ms) {
    g_rtt_last = ms;
    g_rtts.push_back(ms);
    if (g_rtts.size() > 200u) g_rtts.pop_front();
    std::vector<double> v(g_rtts.begin(), g_rtts.end());
    std::sort(v.begin(), v.end());
    g_rtt_p50 = v[static_cast<size_t>(v.size() * 0.50)];
    g_rtt_p95 = v[static_cast<size_t>(v.size() * 0.95)];
}

// ─────────────────────────────────────────────────────────────────────────────
// Shadow CSV
// ─────────────────────────────────────────────────────────────────────────────
static void write_shadow_csv(const omega::TradeRecord& tr) {
    if (g_shadow_csv.is_open()) {
        g_shadow_csv << tr.entryTs << ',' << tr.symbol << ',' << tr.side
                     << ',' << tr.entryPrice << ',' << tr.exitPrice
                     << ',' << tr.pnl << ',' << tr.mfe << ',' << tr.mae
                     << ',' << (tr.exitTs - tr.entryTs)
                     << ',' << tr.exitReason
                     << ',' << tr.spreadAtEntry
                     << ',' << tr.latencyMs
                     << ',' << tr.regime << '\n';
        g_shadow_csv.flush();
    }
    if (g_daily_shadow_trade_log) {
        const int64_t bucket_ts = tr.exitTs > 0 ? tr.exitTs : nowSec();
        g_daily_shadow_trade_log->append_row(bucket_ts, build_trade_close_csv_row(tr));
    }
}

static std::string trade_ref_from_record(const omega::TradeRecord& tr) {
    std::ostringstream o;
    o << tr.symbol << '|' << tr.entryTs << '|' << tr.id << '|' << tr.engine;
    return o.str();
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr) {
    const int64_t hold_sec = std::max<int64_t>(0, tr.exitTs - tr.entryTs);
    std::ostringstream o;
    o << tr.id
      << ',' << csv_quote(trade_ref_from_record(tr))
      << ',' << tr.entryTs
      << ',' << csv_quote(utc_iso8601(tr.entryTs))
      << ',' << csv_quote(utc_weekday_name(tr.entryTs))
      << ',' << tr.exitTs
      << ',' << csv_quote(utc_iso8601(tr.exitTs))
      << ',' << csv_quote(utc_weekday_name(tr.exitTs))
      << ',' << csv_quote(tr.symbol)
      << ',' << csv_quote(tr.engine)
      << ',' << csv_quote(tr.side)
      << std::fixed << std::setprecision(4)
      << ',' << tr.entryPrice
      << ',' << tr.exitPrice
      << ',' << tr.tp
      << ',' << tr.sl
      << ',' << tr.size
      << ',' << tr.pnl          // gross
      << ',' << tr.net_pnl      // net (after slippage + commission)
      << ',' << tr.slippage_entry
      << ',' << tr.slippage_exit
      << ',' << tr.commission
      << std::setprecision(6)
      << ',' << tr.slip_entry_pct
      << ',' << tr.slip_exit_pct
      << ',' << tr.comm_per_side
      << std::fixed << std::setprecision(4)
      << ',' << tr.mfe
      << ',' << tr.mae
      << ',' << hold_sec
      << ',' << tr.spreadAtEntry
      << ',' << tr.latencyMs
      << ',' << csv_quote(tr.regime)
      << ',' << csv_quote(tr.exitReason);
    return o.str();
}

static void write_trade_close_logs(const omega::TradeRecord& tr) {
    const std::string row = build_trade_close_csv_row(tr);
    {
        std::lock_guard<std::mutex> lk(g_trade_close_csv_mtx);
        if (g_trade_close_csv.is_open()) {
            g_trade_close_csv << row << '\n';
            g_trade_close_csv.flush();
        }
    }
    const int64_t bucket_ts = tr.exitTs > 0 ? tr.exitTs : nowSec();
    if (g_daily_trade_close_log) g_daily_trade_close_log->append_row(bucket_ts, row);
    if (tr.symbol == "GOLD.F" && g_daily_gold_trade_close_log)
        g_daily_gold_trade_close_log->append_row(bucket_ts, row);
}

// ─────────────────────────────────────────────────────────────────────────────
// session_tradeable() — UTC hour gate for all engine entries
//
// TWO WINDOWS are evaluated. Either window active = trading allowed.
//
// PRIMARY WINDOW  (config: session_start_utc → session_end_utc)
//   Default: 07:00–22:00 UTC  (London open → NY close + 1hr buffer)
//   Covers:  London 07:00–16:00, NY 13:00–22:00
//   Supports wrap-through-midnight if start > end (e.g. 22→5)
//   Set start == end to run 24h (not recommended)
//
// ASIA WINDOW  (config: session_asia=true)
//   Fixed:  22:00–05:00 UTC  (Tokyo gold market + NZ/AU morning)
//   Active for gold, silver, oil — all trade during Tokyo hours
//   Hardcoded range — not affected by primary window values
//
// DEAD ZONE (intentional gap):
//   05:00–07:00 UTC — Sydney close to London open
//   Genuinely thin liquidity, wide spreads, no engine runs here
//
// COVERAGE MAP:
//   00:00–05:00  Asia window active      ✓ trading
//   05:00–07:00  DEAD ZONE               ✗ blocked
//   07:00–22:00  Primary window active   ✓ trading
//   22:00–24:00  Asia window active      ✓ trading
//
// BUG HISTORY:
//   session_end_utc was 21 — created a silent 21:00–22:00 dead zone each night.
//   Gold, oil, silver, forex all blocked for 1hr despite being open markets.
//   Fixed: session_end_utc raised to 22 — primary window now hands off directly
//   to Asia window with no gap.
// ─────────────────────────────────────────────────────────────────────────────
static bool session_tradeable() noexcept {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti; gmtime_s(&ti, &t);
    const int h = ti.tm_hour;

    // PRIMARY WINDOW — London + NY (07:00–22:00 UTC by default)
    // Supports wrap-through-midnight: if start > end, range crosses 00:00
    // e.g. start=22 end=5 → active from 22:00 through to 05:00
    bool in_primary = false;
    if (g_cfg.session_start_utc == g_cfg.session_end_utc) {
        in_primary = true;                                              // 24h explicit mode
    } else if (g_cfg.session_start_utc < g_cfg.session_end_utc) {
        in_primary = (h >= g_cfg.session_start_utc && h < g_cfg.session_end_utc); // normal range
    } else {
        in_primary = (h >= g_cfg.session_start_utc || h < g_cfg.session_end_utc); // wraps midnight
    }

    // ASIA WINDOW — Tokyo gold market (22:00–05:00 UTC, wraps midnight)
    // Hardcoded range — independent of primary window config.
    // When session_end_utc=22, primary hands off to Asia with zero gap.
    const bool in_asia = g_cfg.session_asia && (h >= 22 || h < 5);

    return in_primary || in_asia;
}

// ─────────────────────────────────────────────────────────────────────────────
// Apply config to engines -- per-symbol typed overloads
// ─────────────────────────────────────────────────────────────────────────────
// Generic fallback (used for GOLD BreakoutEngine)
// SP -- uses [sp] config section, links macro context
static void apply_engine_config(omega::SpEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.sp_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.sp_tp_pct;
    eng.SL_PCT                = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC           = std::max(g_cfg.sp_min_gap_sec, g_cfg.min_entry_gap_sec);
    // Do NOT override MOMENTUM_THRESH_PCT / MIN_BREAKOUT_PCT — constructor has
    // correct per-instrument values (0.012% / 0.12%) tuned for SP price level.
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.macro                 = &g_macro_ctx;
}
// NQ -- uses [nq] config section, links macro context
static void apply_engine_config(omega::NqEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.nq_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.nq_tp_pct;
    eng.SL_PCT                = g_cfg.nq_sl_pct;
    eng.MIN_GAP_SEC           = std::max(g_cfg.nq_min_gap_sec, g_cfg.min_entry_gap_sec);
    // Do NOT override MOMENTUM_THRESH_PCT / MIN_BREAKOUT_PCT — constructor has
    // correct per-instrument values (0.010% / 0.08%) tuned for NQ price level.
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.macro                 = &g_macro_ctx;
}
// Oil -- uses [oil] config section, inventory window block built into engine
static void apply_engine_config(omega::OilEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.oil_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.oil_tp_pct;
    eng.SL_PCT                = g_cfg.oil_sl_pct;
    eng.MIN_GAP_SEC           = std::max(g_cfg.oil_min_gap_sec, g_cfg.min_entry_gap_sec);
    // Do NOT override MOMENTUM_THRESH_PCT / MIN_BREAKOUT_PCT — constructor has
    // correct per-instrument values (0.015% / 0.10%) tuned for Oil price level.
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = std::max(g_cfg.oil_max_hold_sec, 1800);
    eng.macro                 = &g_macro_ctx;
}

static void apply_generic_index_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = std::min(0.06, std::max(0.02, g_cfg.sp_vol_thresh_pct));
    eng.TP_PCT                = g_cfg.sp_tp_pct;
    eng.SL_PCT                = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC           = std::max(30, g_cfg.sp_min_gap_sec);
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    // European indices (GER30~22000, UK100~8300, ESTX50~5700) need
    // instrument-appropriate thresholds, not the global 0.025%/0.12%.
    // 0.025% on UK100 at 8300 = $2.08 — fine.
    // 0.025% on GER30 at 22000 = $5.50 — too tight for early London.
    // Use 0.010% momentum (absolute ~$1-2 for all EU indices) and
    // 0.06% min_breakout (absolute ~$5-13 depending on index level).
    eng.MOMENTUM_THRESH_PCT   = 0.010;
    eng.MIN_BREAKOUT_PCT      = 0.06;
}

// Us30 (DJ30.F) -- typed engine, links macro context
static void apply_engine_config(omega::Us30Engine& eng) noexcept {
    eng.VOL_THRESH_PCT        = std::min(0.05, std::max(0.02, g_cfg.sp_vol_thresh_pct));
    eng.TP_PCT                = g_cfg.sp_tp_pct;
    eng.SL_PCT                = g_cfg.sp_sl_pct;
    eng.MIN_GAP_SEC           = std::max(g_cfg.sp_min_gap_sec, g_cfg.min_entry_gap_sec);
    // Do NOT override MOMENTUM_THRESH_PCT / MIN_BREAKOUT_PCT — constructor has
    // correct per-instrument values (0.006% / 0.04%) tuned for DJ30 price level
    // (46700+ points — global 0.025%/0.12% translates to absurd absolute values).
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.macro                 = &g_macro_ctx;
}

// Nas100 -- typed engine, links macro context
static void apply_engine_config(omega::Nas100Engine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.nq_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.nq_tp_pct;
    eng.SL_PCT                = g_cfg.nq_sl_pct;
    eng.MIN_GAP_SEC           = std::max(g_cfg.nq_min_gap_sec, g_cfg.min_entry_gap_sec);
    // Do NOT override MOMENTUM_THRESH_PCT / MIN_BREAKOUT_PCT — constructor has
    // correct per-instrument values (0.010% / 0.08%) tuned for NAS100 price level.
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.macro                 = &g_macro_ctx;
}

static void apply_generic_fx_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = 0.010;
    eng.TP_PCT                = 0.080;
    eng.SL_PCT                = 0.040;
    eng.MIN_GAP_SEC           = 45;
    eng.MAX_SPREAD_PCT        = 0.010;
    eng.MOMENTUM_THRESH_PCT   = std::min(0.015, std::max(0.004, g_cfg.momentum_thresh_pct));
    eng.MIN_BREAKOUT_PCT      = std::min(0.080, std::max(0.020, g_cfg.min_breakout_pct));
    eng.MAX_TRADES_PER_MIN    = std::max(4, g_cfg.max_trades_per_min);
    eng.MAX_HOLD_SEC          = std::min(240, std::max(45, g_cfg.max_hold_sec));
}

static void apply_generic_silver_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = 0.060;
    eng.TP_PCT                = 0.800;
    eng.SL_PCT                = 0.400;
    // Silver is churn-prone in early London (07-09 UTC) due to thin liquidity.
    // 180s min gap prevents the engine re-entering after every SL hit.
    // Was 60s — too short, caused back-to-back losses in choppy range.
    eng.MIN_GAP_SEC           = 180;
    eng.MAX_SPREAD_PCT        = 0.08;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    eng.MOMENTUM_THRESH_PCT   = 0.012;
    eng.MIN_BREAKOUT_PCT      = 0.08;
}

static void apply_generic_brent_config(omega::BreakoutEngine& eng) noexcept {
    eng.VOL_THRESH_PCT        = g_cfg.oil_vol_thresh_pct;
    eng.TP_PCT                = g_cfg.oil_tp_pct;
    eng.SL_PCT                = g_cfg.oil_sl_pct;
    eng.MIN_GAP_SEC           = std::max(60, g_cfg.oil_min_gap_sec);
    eng.MAX_SPREAD_PCT        = 0.12;
    eng.MOMENTUM_THRESH_PCT   = g_cfg.momentum_thresh_pct;
    eng.MIN_BREAKOUT_PCT      = g_cfg.min_breakout_pct;
    eng.MAX_TRADES_PER_MIN    = g_cfg.max_trades_per_min;
    eng.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
}

// ─────────────────────────────────────────────────────────────────────────────
// Config loader
// ─────────────────────────────────────────────────────────────────────────────
static void load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) { std::cout << "[CONFIG] Using defaults\n"; return; }
    std::string line, section;
    auto trim = [](std::string& s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
        while (!s.empty() && s.front() == ' ') s = s.substr(1);
    };
    while (std::getline(f, line)) {
        trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') { section = line.substr(1, line.find(']') - 1); continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        trim(k); trim(v);
        const auto cm = v.find('#');
        if (cm != std::string::npos) v = v.substr(0, cm);
        trim(v);
        if (v.empty()) continue;

        if (section == "fix") {
            if (k=="host")               g_cfg.host      = v;
            if (k=="port")               g_cfg.port      = std::stoi(v);
            if (k=="trade_port")        g_cfg.trade_port = std::stoi(v);
            if (k=="sender_comp_id")     g_cfg.sender    = v;
            if (k=="target_comp_id")     g_cfg.target    = v;
            if (k=="username")           g_cfg.username  = v;
            if (k=="password")           g_cfg.password  = v;
            if (k=="heartbeat_interval") g_cfg.heartbeat = std::stoi(v);
        }
        if (section == "mode"     && k=="mode")         g_cfg.mode           = v;
        if (section == "breakout") {
            if (k=="vol_thresh_pct")        g_cfg.vol_thresh_pct        = std::stod(v);
            if (k=="tp_pct")                g_cfg.tp_pct                = std::stod(v);
            if (k=="sl_pct")                g_cfg.sl_pct                = std::stod(v);
            if (k=="compression_lookback")  g_cfg.compression_lookback  = std::stoi(v);
            if (k=="baseline_lookback")     g_cfg.baseline_lookback     = std::stoi(v);
            if (k=="compression_threshold") g_cfg.compression_threshold = std::stod(v);
            if (k=="max_hold_sec")          g_cfg.max_hold_sec          = std::stoi(v);
            if (k=="min_entry_gap_sec")     g_cfg.min_entry_gap_sec     = std::stoi(v);
            if (k=="max_spread_entry_pct")  g_cfg.max_spread_pct        = std::stod(v);
            if (k=="momentum_threshold")    g_cfg.momentum_thresh_pct  = std::stod(v);
            if (k=="min_breakout_move_pct") g_cfg.min_breakout_pct     = std::stod(v);
            if (k=="max_trades_per_minute") g_cfg.max_trades_per_min   = std::stoi(v);
        }
        if (section == "risk") {
            if (k=="max_positions")        g_cfg.max_open_positions = std::stoi(v);
            if (k=="daily_loss_limit")     g_cfg.daily_loss_limit  = std::stod(v);
            if (k=="max_consec_losses")    g_cfg.max_consec_losses = std::stoi(v);
            if (k=="loss_pause_sec")       g_cfg.loss_pause_sec    = std::stoi(v);
            if (k=="independent_symbols")  g_cfg.independent_symbols = (v == "true" || v == "1");
            if (k=="enable_shadow_signal_audit") g_cfg.enable_shadow_signal_audit = (v == "true" || v == "1");
            if (k=="auto_disable_after_trades")  g_cfg.auto_disable_after_trades = std::stoi(v);
            if (k=="shadow_ustec_pilot_only")    g_cfg.shadow_ustec_pilot_only = (v == "true" || v == "1");
            if (k=="shadow_research_mode")       g_cfg.shadow_research_mode = (v == "true" || v == "1");
            if (k=="ustec_pilot_size")           g_cfg.ustec_pilot_size = std::stod(v);
            if (k=="ustec_pilot_min_gap_sec")    g_cfg.ustec_pilot_min_gap_sec = std::stoi(v);
            if (k=="ustec_pilot_require_session") g_cfg.ustec_pilot_require_session = (v == "true" || v == "1");
            if (k=="ustec_pilot_require_latency") g_cfg.ustec_pilot_require_latency = (v == "true" || v == "1");
            if (k=="ustec_pilot_block_risk_off")  g_cfg.ustec_pilot_block_risk_off = (v == "true" || v == "1");
            if (k=="enable_extended_symbols")    g_cfg.enable_extended_symbols = (v == "true" || v == "1");
            if (k=="min_entry_gap_sec")    g_cfg.min_entry_gap_sec = std::stoi(v);
            if (k=="max_spread_entry_pct") g_cfg.max_spread_pct    = std::stod(v);
            if (k=="max_latency_ms")       g_cfg.max_latency_ms    = std::stod(v);
            // Backward-compat: older configs place breakout keys under [risk].
            // Parse them here too so tuned values are not silently ignored.
            if (k=="momentum_threshold")    g_cfg.momentum_thresh_pct = std::stod(v);
            if (k=="min_breakout_move_pct") g_cfg.min_breakout_pct    = std::stod(v);
            if (k=="max_trades_per_minute") g_cfg.max_trades_per_min  = std::stoi(v);
        }
        if (section == "session") {
            if (k=="session_start_utc") g_cfg.session_start_utc = std::stoi(v);
            if (k=="session_end_utc")   g_cfg.session_end_utc   = std::stoi(v);
            if (k=="session_asia")      g_cfg.session_asia      = (v == "true" || v == "1");
        }
        if (section == "telemetry") {
            if (k=="gui_port")   g_cfg.gui_port   = std::stoi(v);
            if (k=="ws_port")    g_cfg.ws_port     = std::stoi(v);
            if (k=="shadow_csv") g_cfg.shadow_csv  = v;
            if (k=="shadow_signal_csv") g_cfg.shadow_signal_csv = v;
            if (k=="log_file")   g_cfg.log_file    = v;
        }
        if (section == "gold") {
            if (k=="gold_tp_pct")        g_cfg.gold_tp_pct        = std::stod(v);
            if (k=="gold_sl_pct")        g_cfg.gold_sl_pct        = std::stod(v);
            if (k=="gold_vol_thresh_pct") g_cfg.gold_vol_thresh_pct = std::stod(v);
            if (k=="use_crtp_engine")    g_cfg.gold_use_crtp_engine = (v == "true" || v == "1");
        }
        if (section == "extended_ids") {
            if (k=="ger30_id")   g_cfg.ext_ger30_id   = std::stoi(v);
            if (k=="uk100_id")   g_cfg.ext_uk100_id   = std::stoi(v);
            if (k=="estx50_id")  g_cfg.ext_estx50_id  = std::stoi(v);
            if (k=="xagusd_id")  g_cfg.ext_xagusd_id  = std::stoi(v);
            if (k=="eurusd_id")  g_cfg.ext_eurusd_id  = std::stoi(v);
            if (k=="ukbrent_id") g_cfg.ext_ukbrent_id = std::stoi(v);
        }
        if (section == "sp") {
            if (k=="tp_pct")         g_cfg.sp_tp_pct         = std::stod(v);
            if (k=="sl_pct")         g_cfg.sp_sl_pct         = std::stod(v);
            if (k=="vol_thresh_pct") g_cfg.sp_vol_thresh_pct = std::stod(v);
            if (k=="min_gap_sec")    g_cfg.sp_min_gap_sec    = std::stoi(v);
        }
        if (section == "nq") {
            if (k=="tp_pct")         g_cfg.nq_tp_pct         = std::stod(v);
            if (k=="sl_pct")         g_cfg.nq_sl_pct         = std::stod(v);
            if (k=="vol_thresh_pct") g_cfg.nq_vol_thresh_pct = std::stod(v);
            if (k=="min_gap_sec")    g_cfg.nq_min_gap_sec    = std::stoi(v);
        }
        if (section == "oil") {
            if (k=="tp_pct")         g_cfg.oil_tp_pct         = std::stod(v);
            if (k=="sl_pct")         g_cfg.oil_sl_pct         = std::stod(v);
            if (k=="vol_thresh_pct") g_cfg.oil_vol_thresh_pct = std::stod(v);
            if (k=="min_gap_sec")    g_cfg.oil_min_gap_sec    = std::stoi(v);
            if (k=="max_hold_sec")   g_cfg.oil_max_hold_sec   = std::stoi(v);
        }
    }
    std::cout << "[CONFIG] mode=" << g_cfg.mode
              << " vol=" << g_cfg.vol_thresh_pct
              << "% tp=" << g_cfg.tp_pct
              << "% sl=" << g_cfg.sl_pct
              << "% maxhold=" << g_cfg.max_hold_sec << "s\n"
              << "[CONFIG] SP   tp=" << g_cfg.sp_tp_pct   << "% sl=" << g_cfg.sp_sl_pct   << "% vol=" << g_cfg.sp_vol_thresh_pct  << "%\n"
              << "[CONFIG] NQ   tp=" << g_cfg.nq_tp_pct   << "% sl=" << g_cfg.nq_sl_pct   << "% vol=" << g_cfg.nq_vol_thresh_pct  << "%\n"
              << "[CONFIG] OIL  tp=" << g_cfg.oil_tp_pct  << "% sl=" << g_cfg.oil_sl_pct  << "% vol=" << g_cfg.oil_vol_thresh_pct << "%\n"
              << "[CONFIG] GOLD tp=" << g_cfg.gold_tp_pct << "% sl=" << g_cfg.gold_sl_pct << "% vol=" << g_cfg.gold_vol_thresh_pct << "%\n"
              << "[CONFIG] USTEC pilot only=" << (g_cfg.shadow_ustec_pilot_only ? "true" : "false")
              << " shadow_research=" << (g_cfg.shadow_research_mode ? "true" : "false")
              << " size=" << g_cfg.ustec_pilot_size
              << " min_gap=" << g_cfg.ustec_pilot_min_gap_sec << "s\n"
              << "[CONFIG] latency_cap=" << g_cfg.max_latency_ms << "ms spread_cap=" << g_cfg.max_spread_pct << "%\n";
}

static void sanitize_config() noexcept {
    auto clampd = [](double v, double lo, double hi, double fallback) {
        if (!std::isfinite(v)) return fallback;
        return std::max(lo, std::min(v, hi));
    };
    auto clampi = [](int v, int lo, int hi, int fallback) {
        if (v < lo || v > hi) return fallback;
        return v;
    };

    g_cfg.max_open_positions = clampi(g_cfg.max_open_positions, 1, 8, 1);
    g_cfg.max_consec_losses  = clampi(g_cfg.max_consec_losses, 1, 20, 3);
    g_cfg.loss_pause_sec     = clampi(g_cfg.loss_pause_sec, 10, 3600, 300);
    g_cfg.auto_disable_after_trades = clampi(g_cfg.auto_disable_after_trades, 5, 200, 10);
    g_cfg.ustec_pilot_min_gap_sec   = clampi(g_cfg.ustec_pilot_min_gap_sec, 15, 900, 60);

    g_cfg.max_latency_ms     = clampd(g_cfg.max_latency_ms, 0.0, 5000.0, 60.0);
    g_cfg.daily_loss_limit   = clampd(g_cfg.daily_loss_limit, 1.0, 1000000.0, 200.0);
    g_cfg.momentum_thresh_pct = clampd(g_cfg.momentum_thresh_pct, 0.0, 10.0, 0.05);
    g_cfg.min_breakout_pct    = clampd(g_cfg.min_breakout_pct, 0.0, 10.0, 0.25);
    g_cfg.ustec_pilot_size    = clampd(g_cfg.ustec_pilot_size, 0.05, 2.0, 0.35);

    g_cfg.sp_vol_thresh_pct   = clampd(g_cfg.sp_vol_thresh_pct, 0.0, 10.0, 0.04);
    g_cfg.nq_vol_thresh_pct   = clampd(g_cfg.nq_vol_thresh_pct, 0.0, 10.0, 0.05);
    g_cfg.oil_vol_thresh_pct  = clampd(g_cfg.oil_vol_thresh_pct, 0.0, 10.0, 0.08);
    g_cfg.gold_vol_thresh_pct = clampd(g_cfg.gold_vol_thresh_pct, 0.0, 10.0, 0.04);

    g_cfg.session_start_utc = clampi(g_cfg.session_start_utc, 0, 23, 7);
    g_cfg.session_end_utc   = clampi(g_cfg.session_end_utc,   0, 23, 21);

    g_cfg.ext_ger30_id   = std::max(0, g_cfg.ext_ger30_id);
    g_cfg.ext_uk100_id   = std::max(0, g_cfg.ext_uk100_id);
    g_cfg.ext_estx50_id  = std::max(0, g_cfg.ext_estx50_id);
    g_cfg.ext_xagusd_id  = std::max(0, g_cfg.ext_xagusd_id);
    g_cfg.ext_eurusd_id  = std::max(0, g_cfg.ext_eurusd_id);
    g_cfg.ext_ukbrent_id = std::max(0, g_cfg.ext_ukbrent_id);

    g_ext_syms[0].id = g_cfg.ext_ger30_id;
    g_ext_syms[1].id = g_cfg.ext_uk100_id;
    g_ext_syms[2].id = g_cfg.ext_estx50_id;
    g_ext_syms[3].id = g_cfg.ext_xagusd_id;
    g_ext_syms[4].id = g_cfg.ext_eurusd_id;
    g_ext_syms[5].id = g_cfg.ext_ukbrent_id;

    std::cout << "[CONFIG] risk max_positions=" << g_cfg.max_open_positions
              << " max_consec_losses=" << g_cfg.max_consec_losses
              << " loss_pause_sec=" << g_cfg.loss_pause_sec << "\n";
}

static void apply_shadow_research_profile() noexcept {
    if (g_cfg.mode != "SHADOW" || !g_cfg.shadow_research_mode) return;

    // SHADOW is research mode: remove session dead-zones and loosen entry throttles.
    g_cfg.session_start_utc = 0;
    g_cfg.session_end_utc   = 0;   // equal start/end => 24h tradeable window
    g_cfg.session_asia      = true;

    g_cfg.max_latency_ms    = std::max(g_cfg.max_latency_ms, 25.0);
    // SHADOW quality profile: still active, but avoid low-edge overtrading.
    g_cfg.max_hold_sec        = std::min(g_cfg.max_hold_sec, 120);
    g_cfg.momentum_thresh_pct = std::max(g_cfg.momentum_thresh_pct, 0.045);
    g_cfg.min_breakout_pct    = std::max(g_cfg.min_breakout_pct, 0.12);
    g_cfg.max_trades_per_min  = std::min(g_cfg.max_trades_per_min, 3);

    g_cfg.sp_min_gap_sec  = std::max(g_cfg.sp_min_gap_sec, 90);
    g_cfg.nq_min_gap_sec  = std::max(g_cfg.nq_min_gap_sec, 90);
    g_cfg.oil_min_gap_sec = std::max(g_cfg.oil_min_gap_sec, 180);

    g_cfg.sp_vol_thresh_pct   = std::max(g_cfg.sp_vol_thresh_pct, 0.040);
    g_cfg.nq_vol_thresh_pct   = std::max(g_cfg.nq_vol_thresh_pct, 0.050);
    g_cfg.oil_vol_thresh_pct  = std::max(g_cfg.oil_vol_thresh_pct, 0.080);
    g_cfg.gold_vol_thresh_pct = std::max(g_cfg.gold_vol_thresh_pct, 0.045);

    // Small-win profile with better quality than pure scalping.
    g_cfg.sp_tp_pct = std::min(g_cfg.sp_tp_pct, 0.11);
    g_cfg.sp_sl_pct = std::min(g_cfg.sp_sl_pct, 0.08);
    g_cfg.nq_tp_pct = std::min(g_cfg.nq_tp_pct, 0.12);
    g_cfg.nq_sl_pct = std::min(g_cfg.nq_sl_pct, 0.09);
    g_cfg.oil_tp_pct = std::min(g_cfg.oil_tp_pct, 0.24);
    g_cfg.oil_sl_pct = std::min(g_cfg.oil_sl_pct, 0.18);
    g_cfg.gold_tp_pct = std::min(g_cfg.gold_tp_pct, 0.10);
    g_cfg.gold_sl_pct = std::min(g_cfg.gold_sl_pct, 0.08);

    std::cout << "[CONFIG] SHADOW quality profile enabled: 24h session, conservative quality tuning\n";
}

static void maybe_reset_daily_ledger() {
    const auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    struct tm ti{};
    gmtime_s(&ti, &t);
    if (g_last_ledger_utc_day < 0) {
        g_last_ledger_utc_day = ti.tm_yday;
        return;
    }
    if (ti.tm_yday == g_last_ledger_utc_day) return;
    g_last_ledger_utc_day = ti.tm_yday;

    g_omegaLedger.resetDaily();
    {
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        g_sym_risk.clear();
        g_shadow_quality.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_shadow_signal_mtx);
        g_shadow_signal_positions.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_perf_mtx);
        g_perf.clear();
    }
    g_disable_gold_stack = false;
    g_gov_spread = g_gov_lat = g_gov_pnl = g_gov_pos = g_gov_consec = 0;
    std::cout << "[OMEGA-RISK] UTC day rollover — per-symbol risk state reset\n";
}

static void handle_closed_trade(const omega::TradeRecord& tr_in) {
    // Apply realistic shadow costs before any recording/stats.
    // commission_per_side=0.0 for BlackBull CFD model (cost embedded in spread).
    omega::TradeRecord tr = tr_in;
    omega::apply_realistic_costs(tr, 0.0);

    std::cout << "[TRADE-COST] " << tr.symbol
              << " gross=" << std::fixed << std::setprecision(4) << tr.pnl
              << " slip_in=" << tr.slippage_entry
              << " slip_out=" << tr.slippage_exit
              << " net=" << tr.net_pnl
              << " exit=" << tr.exitReason << "\n";
    std::cout.flush();

    g_omegaLedger.record(tr);
    write_shadow_csv(tr);
    write_trade_close_logs(tr);

    const std::string perf_key = perf_key_from_trade(tr);
    const bool shadow_research = (g_cfg.mode == "SHADOW" && g_cfg.shadow_research_mode);
    {
        const std::string risk_key = g_cfg.independent_symbols ? tr.symbol : "GLOBAL";
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        auto& st = g_sym_risk[risk_key];
        st.daily_pnl += tr.net_pnl;   // track net (after costs) for risk gates
        if (tr.net_pnl <= 0.0) {
            const int loss_limit = shadow_research ? 2 : g_cfg.max_consec_losses;
            const int pause_sec  = shadow_research ? 300 : g_cfg.loss_pause_sec;
            if (++st.consec_losses >= loss_limit) {
                st.pause_until = nowSec() + pause_sec;
                std::cout << "[OMEGA-RISK] " << risk_key << " "
                          << loss_limit << " consecutive losses -- pause "
                          << pause_sec << "s\n";
            }
        } else {
            st.consec_losses = 0;
        }
        if (shadow_research) {
            auto& qs = g_shadow_quality[tr.symbol];
            const int64_t held = std::max<int64_t>(0, tr.exitTs - tr.entryTs);
            const bool fast_bad_loss =
                (tr.net_pnl <= 0.0) &&
                (held <= 120) &&
                (tr.exitReason == "SL_HIT" || tr.exitReason == "SCRATCH" || tr.exitReason == "TIMEOUT");
            if (fast_bad_loss) {
                if (++qs.fast_loss_streak >= 2) {
                    qs.pause_until = nowSec() + 180;
                    std::cout << "[OMEGA-QUALITY] " << tr.symbol
                              << " fast-loss streak=" << qs.fast_loss_streak
                              << " pause=180s\n";
                }
            } else if (tr.net_pnl > 0.0) {
                qs.fast_loss_streak = 0;
            } else if (qs.fast_loss_streak > 0) {
                --qs.fast_loss_streak;
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_perf_mtx);
        auto& ps = g_perf[perf_key];
        ps.live_trades++;
        ps.live_pnl += tr.net_pnl;   // net pnl for performance tracking
        if (tr.net_pnl > 0) ps.live_wins++; else ps.live_losses++;
        if (!ps.disabled &&
            g_cfg.mode != "SHADOW" &&
            ps.live_trades >= g_cfg.auto_disable_after_trades &&
            ps.live_pnl < 0.0) {
            ps.disabled = true;
            if (perf_key == "GOLD_STACK") g_disable_gold_stack = true;
            std::cout << "[OMEGA-AUTO-DISABLE] " << perf_key
                      << " live_trades=" << ps.live_trades
                      << " pnl=" << ps.live_pnl << "\n";
        }
    }
    g_telemetry.UpdateStats(
        g_omegaLedger.dailyPnl(), g_omegaLedger.maxDD(),
        g_omegaLedger.total(), g_omegaLedger.wins(), g_omegaLedger.losses(),
        g_omegaLedger.winRate(), g_omegaLedger.avgWin(), g_omegaLedger.avgLoss(), 0, 0);
    g_telemetry.UpdateLastSignal(tr.symbol.c_str(), "CLOSED", tr.exitPrice, tr.exitReason.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
// Signal handler
// ─────────────────────────────────────────────────────────────────────────────
static void sig_handler(int) noexcept { g_running.store(false); }

// ─────────────────────────────────────────────────────────────────────────────
// Tick handler -- called for every bid/ask update
// ─────────────────────────────────────────────────────────────────────────────
static void on_tick(const std::string& sym, double bid, double ask) {
    { std::lock_guard<std::mutex> lk(g_book_mtx); g_bids[sym] = bid; g_asks[sym] = ask; }
    manage_shadow_signals_on_tick(sym, bid, ask);

    // Rate-limit tick logging — max 1 line per symbol per 30s to keep logs readable.
    // Previously logged every tick: thousands of lines/minute drowning signal output.
    {
        static std::mutex s_tick_log_mtx;
        static std::unordered_map<std::string, int64_t> s_last_tick_log;
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::lock_guard<std::mutex> lk(s_tick_log_mtx);
        auto& last = s_last_tick_log[sym];
        if (now_ms - last >= 30000) {
            last = now_ms;
            std::cout << "[TICK] " << sym << " " << bid << "/" << ask << "\n";
            std::cout.flush();
        }
    }

    maybe_reset_daily_ledger();

    const double mid = (bid + ask) * 0.5;
    if (sym == "VIX.F")   g_macroDetector.updateVIX(mid);
    if (sym == "US500.F") g_macroDetector.updateES(mid);   // use traded futures, not cash ES
    if (sym == "USTEC.F") g_macroDetector.updateNQ(mid);   // use traded futures, not cash NAS100

    g_telemetry.UpdatePrice(sym.c_str(), bid, ask);

    const std::string regime = g_macroDetector.regime();
    g_telemetry.UpdateMacroRegime(
        g_macroDetector.vixLevel(), regime.c_str(), g_macroDetector.esNqDivergence());

    // Update shared MacroContext -- read by SP/NQ shouldTrade() overrides
    g_macro_ctx.regime     = regime;
    g_macro_ctx.vix        = g_macroDetector.vixLevel();
    g_macro_ctx.es_nq_div  = g_macroDetector.esNqDivergence();
    g_macro_ctx.sp_open    = g_eng_sp.pos.active;
    g_macro_ctx.nq_open    = g_eng_nq.pos.active;
    g_macro_ctx.oil_open   = g_eng_cl.pos.active;

    const bool tradeable = session_tradeable();
    g_telemetry.UpdateSession(tradeable ? "ACTIVE" : "CLOSED", tradeable ? 1 : 0);

    // Base gate flags -- passed into dispatch, checked before entry (not before warmup)
    // Use p95 RTT (not last) -- a single spike in g_rtt_last was permanently blocking
    // entries until the next 5s ping. p95 over 200 samples is stable and representative.
    const double rtt_check = (g_rtt_p95 > 0.0) ? g_rtt_p95 : g_rtt_last;
    const bool lat_ok = (rtt_check <= 0.0 || g_governor.checkLatency(rtt_check, g_cfg.max_latency_ms));
    if (!lat_ok) ++g_gov_lat;

    auto symbol_risk_blocked = [&](const std::string& symbol) -> bool {
        std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
        auto& st = g_sym_risk[symbol];
        const bool shadow_research = (g_cfg.mode == "SHADOW" && g_cfg.shadow_research_mode);
        const double daily_limit = shadow_research ? 8.0 : g_cfg.daily_loss_limit;
        if (st.daily_pnl < -daily_limit) {
            ++g_gov_pnl;
            if (shadow_research && st.pause_until < nowSec() + 300) {
                st.pause_until = nowSec() + 300; // 5 min symbol cooldown in shadow
            }
            return true;
        }
        const int64_t now = nowSec();
        if (st.pause_until > now) {
            ++g_gov_consec;
            return true;
        }
        if (st.pause_until != 0 && st.pause_until <= now) {
            st.pause_until = 0;
            st.consec_losses = 0;
            std::cout << "[OMEGA-RISK] " << symbol << " loss pause cleared\n";
        }
        return false;
    };

    auto symbol_gate = [&](const std::string& symbol, bool symbol_has_open_position) -> bool {
        const bool shadow_mode = (g_cfg.mode == "SHADOW");
        const bool shadow_research = (shadow_mode && g_cfg.shadow_research_mode);
        if (symbol == "GOLD.F" && g_disable_gold_stack) return false;
        {
            std::lock_guard<std::mutex> lk(g_perf_mtx);
            auto it = g_perf.find(symbol);
            if (it != g_perf.end() && it->second.disabled) return false;
        }
        if ((!shadow_mode || !shadow_research) && !tradeable) return false;
        if ((!shadow_mode || !shadow_research) && !lat_ok) return false;
        if (shadow_mode) {
            // Optional shadow pilot mode: keep GOLD stack live and run USTEC pilot only.
            if (g_cfg.shadow_ustec_pilot_only &&
                symbol != "GOLD.F" && symbol != "USTEC.F") {
                return false;
            }
            if (g_cfg.shadow_ustec_pilot_only && symbol == "USTEC.F") {
                if (g_cfg.ustec_pilot_require_session && !tradeable) return false;
                if (g_cfg.ustec_pilot_require_latency && !lat_ok) return false;
                if (g_cfg.ustec_pilot_block_risk_off && regime == "RISK_OFF") return false;
            }
            if (shadow_research) {
                std::lock_guard<std::mutex> lk(g_sym_risk_mtx);
                auto& qs = g_shadow_quality[symbol];
                const int64_t now = nowSec();
                if (qs.pause_until > now) {
                    ++g_gov_consec;
                    return false;
                }
                if (qs.pause_until != 0 && qs.pause_until <= now) {
                    qs.pause_until = 0;
                    qs.fast_loss_streak = 0;
                }
            }
        }
        if (symbol_has_open_position) {
            ++g_gov_pos;
            return false;
        }
        if (g_cfg.independent_symbols) {
            return !symbol_risk_blocked(symbol);
        }
        // Legacy global portfolio mode.
        if (symbol_risk_blocked("GLOBAL")) return false;
        const int open_positions =
            static_cast<int>(g_eng_sp.pos.active) +
            static_cast<int>(g_eng_nq.pos.active) +
            static_cast<int>(g_eng_cl.pos.active) +
            static_cast<int>(g_eng_us30.pos.active) +
            static_cast<int>(g_eng_nas100.pos.active) +
            static_cast<int>(g_eng_ger30.pos.active) +
            static_cast<int>(g_eng_uk100.pos.active) +
            static_cast<int>(g_eng_estx50.pos.active) +
            static_cast<int>(g_eng_xag.pos.active) +
            static_cast<int>(g_eng_eurusd.pos.active) +
            static_cast<int>(g_eng_brent.pos.active) +
            static_cast<int>(g_eng_xau.pos.active) +
            static_cast<int>(g_gold_stack.has_open_position());
        const bool pos_budget_ok = open_positions < g_cfg.max_open_positions;
        if (!pos_budget_ok) ++g_gov_pos;
        return pos_budget_ok;
    };

    // ── Route to engine -- typed dispatch (CRTP has no virtual base) ──────────
    // Each branch calls the same logical sequence on the correct typed engine.
    // on_close lambda is defined once and reused across all branches.
    auto on_close = [&](const omega::TradeRecord& tr) {
        handle_closed_trade(tr);
    };

    // Helper lambda -- always feeds ticks to engine (warmup + position management).
    // can_enter=false gates new entries only; TP/SL/timeout always run.
    auto dispatch = [&](auto& eng, bool can_enter_for_symbol) {
        const auto sig = eng.update(bid, ask, rtt_check, regime.c_str(), on_close, can_enter_for_symbol);
        g_telemetry.UpdateEngineState(sym.c_str(),
            static_cast<int>(eng.phase), eng.comp_high, eng.comp_low,
            eng.recent_vol_pct, eng.base_vol_pct, eng.signal_count);
        if (sig.valid) {
            g_telemetry.UpdateLastSignal(sym.c_str(),
                sig.is_long ? "LONG" : "SHORT", sig.entry, sig.reason);
            std::cout << "\033[1;" << (sig.is_long ? "32" : "31") << "m"
                      << "[OMEGA] " << sym << " " << (sig.is_long ? "LONG" : "SHORT")
                      << " entry=" << sig.entry << " tp=" << sig.tp << " sl=" << sig.sl
                      << " regime=" << regime << "\033[0m\n";
            // ── Live order dispatch (no-op in SHADOW mode) ──────────────────
            send_live_order(sym, sig.is_long, eng.ENTRY_SIZE, sig.entry);
        }
    };

    if      (sym == "US500.F") {
        dispatch(g_eng_sp, symbol_gate("US500.F", g_eng_sp.pos.active));
    }
    else if (sym == "USTEC.F") {
        dispatch(g_eng_nq, symbol_gate("USTEC.F", g_eng_nq.pos.active));
    }
    else if (sym == "USOIL.F") {
        dispatch(g_eng_cl, symbol_gate("USOIL.F", g_eng_cl.pos.active));
    }
    else if (sym == "DJ30.F") {
        dispatch(g_eng_us30, symbol_gate("DJ30.F", g_eng_us30.pos.active));
    }
    else if (sym == "GER30") {
        dispatch(g_eng_ger30, symbol_gate("GER30", g_eng_ger30.pos.active));
    }
    else if (sym == "UK100") {
        dispatch(g_eng_uk100, symbol_gate("UK100", g_eng_uk100.pos.active));
    }
    else if (sym == "ESTX50") {
        dispatch(g_eng_estx50, symbol_gate("ESTX50", g_eng_estx50.pos.active));
    }
    else if (sym == "XAGUSD") {
        // Standard breakout engine for silver
        dispatch(g_eng_xag, symbol_gate("XAGUSD", g_eng_xag.pos.active));
        // Lead-lag engine: enter silver when gold has moved but silver hasn't yet
        {
            const auto ll_sig = g_le_stack.on_tick_silver(bid, ask, rtt_check, on_close);
            if (ll_sig.valid) {
                g_telemetry.UpdateLastSignal("XAGUSD",
                    ll_sig.is_long ? "LONG" : "SHORT", ll_sig.entry, ll_sig.reason);
                send_live_order("XAGUSD", ll_sig.is_long, ll_sig.size, ll_sig.entry);
            }
        }
    }
    else if (sym == "EURUSD") {
        dispatch(g_eng_eurusd, symbol_gate("EURUSD", g_eng_eurusd.pos.active));
    }
    else if (sym == "UKBRENT") {
        dispatch(g_eng_brent, symbol_gate("UKBRENT", g_eng_brent.pos.active));
    }
    else if (sym == "NAS100") {
        dispatch(g_eng_nas100, symbol_gate("NAS100", g_eng_nas100.pos.active));
    }
    else if (sym == "GOLD.F")  {
        const bool gold_symbol_open =
            g_gold_stack.has_open_position() || (g_cfg.gold_use_crtp_engine && g_eng_xau.pos.active);
        const bool gold_can_enter = symbol_gate("GOLD.F", gold_symbol_open);

        // Keep CRTP gold warmed for telemetry; default execution path is stack-only.
        dispatch(g_eng_xau, g_cfg.gold_use_crtp_engine ? gold_can_enter : false);

        // ── GoldEngineStack: dedicated gold executor ──────────────────────────
        const auto gsig = g_gold_stack.on_tick(bid, ask, rtt_check, on_close, gold_can_enter);
        if (gsig.valid) {
            g_telemetry.UpdateLastSignal("GOLD.F",
                gsig.is_long ? "LONG" : "SHORT", gsig.entry, gsig.reason);
            std::cout << "\033[1;" << (gsig.is_long ? "32" : "31") << "m"
                      << "[GOLD-STACK-ENTRY] " << (gsig.is_long ? "LONG" : "SHORT")
                      << " entry=" << gsig.entry
                      << " tp="    << gsig.tp_ticks << "ticks"
                      << " sl="    << gsig.sl_ticks << "ticks"
                      << " conf="  << gsig.confidence
                      << " eng="   << gsig.engine
                      << " reason=" << gsig.reason
                      << " regime=" << g_gold_stack.regime_name()
                      << " vwap="  << g_gold_stack.vwap()
                      << "\033[0m\n";
            send_live_order("GOLD.F", gsig.is_long, 1.0, gsig.entry);
        }

        // ── Latency Edge Stack: co-location speed engines ─────────────────────
        // SpreadDislocation and EventCompression run on every GOLD.F tick.
        // LeadLag arms here, fires on next XAGUSD tick (see XAGUSD dispatch block).
        // These engines are fully independent — separate positions and P&L.
        {
            const auto le_sig = g_le_stack.on_tick_gold(bid, ask, rtt_check, on_close);
            if (le_sig.valid) {
                g_telemetry.UpdateLastSignal("GOLD.F",
                    le_sig.is_long ? "LONG" : "SHORT", le_sig.entry, le_sig.reason);
                send_live_order("GOLD.F", le_sig.is_long, le_sig.size, le_sig.entry);
            }
        }
    }
    else {
        // Confirmation-only symbol (VIX, ES, NAS100, DX etc) -- no engine dispatch
        g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);
        return;
    }

    g_telemetry.UpdateGovernor(g_gov_spread, g_gov_lat, g_gov_pnl, g_gov_pos, g_gov_consec);
}  // ← on_tick
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::string> extract_messages(const char* data, int n) {
    g_recv_buf.append(data, static_cast<size_t>(n));
    std::vector<std::string> msgs;
    while (true) {
        const size_t bs = g_recv_buf.find("8=FIX");
        if (bs == std::string::npos) { g_recv_buf.clear(); break; }
        if (bs > 0u) g_recv_buf = g_recv_buf.substr(bs);
        const size_t bl_pos = g_recv_buf.find("\x01" "9=");
        if (bl_pos == std::string::npos) break;
        const size_t bl_start = bl_pos + 3u;
        const size_t bl_end   = g_recv_buf.find('\x01', bl_start);
        if (bl_end == std::string::npos) break;
        const int    body_len = std::stoi(g_recv_buf.substr(bl_start, bl_end - bl_start));
        const size_t hdr_end  = bl_end + 1u;
        const size_t msg_end  = hdr_end + static_cast<size_t>(body_len) + 7u;
        if (msg_end > g_recv_buf.size()) break;
        msgs.push_back(g_recv_buf.substr(0u, msg_end));
        g_recv_buf = g_recv_buf.substr(msg_end);
    }
    return msgs;
}

// ─────────────────────────────────────────────────────────────────────────────
// FIX dispatch
// ─────────────────────────────────────────────────────────────────────────────
static void dispatch_fix(const std::string& msg, SSL* ssl) {
    const std::string type = extract_tag(msg, "35");

    if (type == "A") {
        std::cout << "[OMEGA] LOGON ACCEPTED\n";
        g_quote_ready.store(true);
        g_telemetry.UpdateFixStatus("CONNECTED", "CONNECTED", 0, 0);
        const std::string md = build_marketdata_req(g_quote_seq++);
        SSL_write(ssl, md.c_str(), static_cast<int>(md.size()));
        std::cout << "[OMEGA] Subscribed: US500.F USTEC.F USOIL.F + 8 confirmation\n";
        if (g_cfg.enable_extended_symbols) {
            const std::string ext = build_marketdata_req_extended(g_quote_seq++);
            if (!ext.empty()) {
                SSL_write(ssl, ext.c_str(), static_cast<int>(ext.size()));
                std::cout << "[OMEGA] Subscribed EXT (numeric IDs from config)\n";
            } else {
                std::cout << "[OMEGA] EXT subscription deferred (waiting for SecurityList or configured IDs)\n";
            }
        }
        return;
    }

    if (type == "0") {
        const std::string trid = extract_tag(msg, "112");
        if (!trid.empty() && trid == g_rtt_pending_id && g_rtt_pending_ts > 0) {
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            rtt_record(static_cast<double>(now_us - g_rtt_pending_ts) / 1000.0);
            g_rtt_pending_ts = 0;
            g_telemetry.UpdateLatency(g_rtt_last, g_rtt_p50, g_rtt_p95);
        }
        return;
    }

    if (type == "1") {
        const std::string trid = extract_tag(msg, "112");
        const std::string hb   = build_heartbeat(g_quote_seq++, "QUOTE", trid.c_str());
        SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size()));
        return;
    }

    // ── Unknown / unexpected message types -- log everything for diagnostics ──
    if (type != "W" && type != "X" && type != "A" && type != "0" && type != "1" && type != "3" && type != "j") {
        std::string readable = msg.substr(0, std::min(msg.size(), size_t(300)));
        for (char& c : readable) if (c == '\x01') c = '|';
        std::cerr << "[OMEGA-RAW] type=" << type << " msg=" << readable << "\n";
        std::cerr.flush();
    }

    // ── Market data ───────────────────────────────────────────────────────────
    if (type == "W" || type == "X") {
        const std::string sym_raw = extract_tag(msg, "55");
        if (sym_raw.empty()) {
            std::cerr << "[OMEGA-MD] W/X msg missing tag 55 -- raw: ";
            std::string r = msg.substr(0, 200); for (char& c : r) if (c=='\x01') c='|';
            std::cerr << r << "\n"; std::cerr.flush();
            return;
        }
        std::string sym;
        // Try numeric ID first (normal case), then string name fallback
        try {
            const int id = std::stoi(sym_raw);
            std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
            const auto it = g_id_to_sym.find(id);
            if (it == g_id_to_sym.end()) {
                std::cerr << "[OMEGA-MD] Unknown numeric ID " << id << " in tag55\n";
                std::cerr.flush();
                return;
            }
            sym = it->second;
        } catch (...) {
            // Broker sent string name in 55= (e.g. "GOLD.F") -- look up directly
            for (int i = 0; i < OMEGA_NSYMS; ++i) {
                if (sym_raw == OMEGA_SYMS[i].name) { sym = OMEGA_SYMS[i].name; break; }
            }
            if (sym.empty() && g_cfg.enable_extended_symbols) {
                std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
                for (const auto& e : g_ext_syms) {
                    if (sym_raw == e.name) { sym = e.name; break; }
                }
            }
            if (sym.empty()) {
                std::cerr << "[OMEGA-MD] Unknown string symbol '" << sym_raw << "' in tag55\n";
                std::cerr.flush();
                return;
            }
        }
        double bid = 0.0, ask = 0.0;
        size_t pos = 0u;
        while ((pos = msg.find("269=", pos)) != std::string::npos) {
            const char et = msg[pos + 4u];
            const size_t next_soh = msg.find('\x01', pos);
            if (next_soh == std::string::npos) break;
            const size_t px = msg.find("270=", pos);
            if (px == std::string::npos) { pos = next_soh; continue; }
            const size_t pxe = msg.find('\x01', px + 4u);
            if (pxe == std::string::npos) break;
            const double price = std::stod(msg.substr(px + 4u, pxe - (px + 4u)));
            if (et == '0') bid = price;
            else if (et == '1') ask = price;
            pos = pxe;
        }
        // Measure latency from broker tag 52 (SendingTime) on every quote
        // Provides sub-second RTT samples vs 5s heartbeat ping
        const std::string send_ts = extract_tag(msg, "52");
        if (!send_ts.empty()) {
            const auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const int64_t sent_us = parse_fix_time_us(send_ts);
            if (sent_us > 0 && now_us > sent_us) {
                const double tick_lat_ms = static_cast<double>(now_us - sent_us) / 1000.0;
                if (tick_lat_ms > 0.0 && tick_lat_ms < 5000.0) {
                    // Do NOT feed tag52 delta into rtt_record — broker clock vs our clock
                    // may differ by 10-20ms even on co-located hardware (NTP drift).
                    // rtt_record() feeds the lat_ok gate — only use true TestRequest RTT.
                    // tag52 delta is displayed separately as feed latency indicator only.
                    static int64_t s_last_lat_push_us = 0;
                    if (now_us - s_last_lat_push_us >= 200000LL) {
                        s_last_lat_push_us = now_us;
                        g_telemetry.UpdateLatency(tick_lat_ms, g_rtt_p50, g_rtt_p95);
                    }
                }
            }
        }
        // Merge incremental update with cached book.
        // BlackBull type=X sends only ONE side (bid OR ask).
        // Fill missing side from last known book so on_tick always gets valid bid+ask.
        if (bid <= 0.0 || ask <= 0.0) {
            std::lock_guard<std::mutex> lk(g_book_mtx);
            if (bid <= 0.0) { const auto it = g_bids.find(sym); if (it != g_bids.end()) bid = it->second; }
            if (ask <= 0.0) { const auto it = g_asks.find(sym); if (it != g_asks.end()) ask = it->second; }
        }
        if (bid > 0.0 && ask > 0.0) {
            on_tick(sym, bid, ask);
        }
        // else: book not yet seeded for this symbol, drop silently
        return;
    }

    if (type == "3" || type == "j") {
        std::string r = msg.substr(0, 400); for (char& c : r) if (c=='\x01') c='|';
        std::cerr << "[OMEGA] FIX REJECT type=" << type
                  << " text=" << extract_tag(msg, "58")
                  << " refMsgType=" << extract_tag(msg, "372")
                  << " full=" << r << "\n";
        std::cerr.flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Quote loop
// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// trade_loop  — FIX session on port 5212 (order management)
// Runs in its own thread. In SHADOW mode: connects, logs on, keeps alive.
// In LIVE mode: NewOrderSingle messages are sent via g_trade_ssl.
// ─────────────────────────────────────────────────────────────────────────────
static void trade_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA-TRADE] Connecting " << g_cfg.host << ":" << g_cfg.trade_port << "\n";

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.trade_port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA-TRADE] Connect failed -- retry " << backoff_ms << "ms\n";
            Sleep(static_cast<DWORD>(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        g_trade_seq = 1;

        // Send trade logon
        const std::string logon = build_logon(g_trade_seq++, "TRADE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA-TRADE] Logon sent\n";

        // Store globally for order submission
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = ssl;
            g_trade_sock = sock;
        }

        // Read loop — heartbeats + logon ACK only on trade session
        std::string trade_recv_buf;
        auto last_ping = std::chrono::steady_clock::now();

        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // Heartbeat every 30s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= g_cfg.heartbeat) {
                last_ping = now;
                const std::string hb = build_heartbeat(g_trade_seq++, "TRADE");
                std::lock_guard<std::mutex> lk(g_trade_mtx);
                if (SSL_write(ssl, hb.c_str(), static_cast<int>(hb.size())) <= 0) break;
            }

            char buf[4096];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                std::cerr << "[OMEGA-TRADE] SSL error " << err << " -- reconnecting\n";
                break;
            }
            trade_recv_buf.append(buf, static_cast<size_t>(n));

            // Parse messages from trade session
            while (true) {
                const size_t bs = trade_recv_buf.find("8=FIX");
                if (bs == std::string::npos) { trade_recv_buf.clear(); break; }
                if (bs > 0) trade_recv_buf = trade_recv_buf.substr(bs);
                const size_t bl_pos = trade_recv_buf.find("\x01" "9=");
                if (bl_pos == std::string::npos) break;
                const size_t bl_start = bl_pos + 3u;
                const size_t bl_end   = trade_recv_buf.find('\x01', bl_start);
                if (bl_end == std::string::npos) break;
                const int body_len = std::stoi(trade_recv_buf.substr(bl_start, bl_end - bl_start));
                const size_t hdr_end = bl_end + 1u;
                const size_t msg_end = hdr_end + static_cast<size_t>(body_len) + 7u;
                if (msg_end > trade_recv_buf.size()) break;
                const std::string tmsg = trade_recv_buf.substr(0u, msg_end);
                trade_recv_buf = trade_recv_buf.substr(msg_end);

                const std::string ttype = extract_tag(tmsg, "35");
                if (ttype == "A") {
                    g_trade_ready.store(true);
                    std::cout << "[OMEGA-TRADE] LOGON ACCEPTED\n";
                    const std::string req_id = "omega-sec-" + std::to_string(nowSec());
                    const std::string sec_req = build_security_list_request(g_trade_seq++, req_id);
                    SSL_write(ssl, sec_req.c_str(), static_cast<int>(sec_req.size()));
                    std::cout << "[OMEGA-TRADE] SecurityListRequest sent req_id=" << req_id << "\n";
                } else if (ttype == "8") {
                    // ExecutionReport — order ACK / fill / reject
                    handle_execution_report(tmsg);
                } else if (ttype == "y") {
                    const auto entries = parse_security_list_entries(tmsg);
                    if (!entries.empty()) {
                        const bool ext_changed = apply_security_list_symbol_map(entries);
                        const std::string req_id = extract_tag(tmsg, "320");
                        std::cout << "[OMEGA-TRADE] SecurityList received req_id="
                                  << (req_id.empty() ? "?" : req_id)
                                  << " entries=" << entries.size() << "\n";
                        if (ext_changed) g_ext_md_refresh_needed.store(true);
                    }
                } else if (ttype == "5") {
                    std::cout << "[OMEGA-TRADE] Logout received\n";
                    break;
                } else if (ttype == "3" || ttype == "j") {
                    std::string r = tmsg.substr(0, 300);
                    for (char& c : r) if (c == '\x01') c = '|';
                    std::cerr << "[OMEGA-TRADE] REJECT type=" << ttype
                              << " text=" << extract_tag(tmsg, "58") << "\n";
                }
                // Heartbeats (type=0) and TestRequests (type=1) silently absorbed
            }
        }

        // Tear down
        g_trade_ready.store(false);
        {
            std::lock_guard<std::mutex> lk(g_trade_mtx);
            g_trade_ssl  = nullptr;
            g_trade_sock = -1;
        }
        SSL_shutdown(ssl); SSL_free(ssl);
        if (sock >= 0) closesocket(static_cast<SOCKET>(sock));
        std::cerr << "[OMEGA-TRADE] Disconnected -- reconnecting\n";
        Sleep(2000);
    }
}

static void quote_loop() {
    int backoff_ms = 1000;
    const int max_backoff = 30000;

    while (g_running.load()) {
        std::cout << "[OMEGA] Connecting " << g_cfg.host << ":" << g_cfg.port << "\n";
        g_telemetry.UpdateFixStatus("CONNECTING", "CONNECTING", 0, 0);

        int sock = -1;
        SSL* ssl = connect_ssl(g_cfg.host, g_cfg.port, sock);
        if (!ssl) {
            std::cerr << "[OMEGA] Connect failed -- retry " << backoff_ms << "ms\n";
            Sleep(static_cast<DWORD>(backoff_ms));
            backoff_ms = std::min(backoff_ms * 2, max_backoff);
            continue;
        }

        backoff_ms = 1000;
        g_recv_buf.clear();
        g_quote_seq      = 1;
        g_rtt_pending_ts = 0;

        const std::string logon = build_logon(g_quote_seq++, "QUOTE");
        SSL_write(ssl, logon.c_str(), static_cast<int>(logon.size()));
        std::cout << "[OMEGA] Logon sent\n";

        auto last_ping = std::chrono::steady_clock::now();
        auto last_diag = std::chrono::steady_clock::now();

        while (g_running.load()) {
            const auto now = std::chrono::steady_clock::now();

            // RTT ping every 5s
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count() >= 5) {
                last_ping = now;
                if (g_rtt_pending_ts == 0) {
                    g_rtt_pending_id = "omega-" + std::to_string(nowSec());
                    g_rtt_pending_ts = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    const std::string tr = build_test_request(g_quote_seq++, "QUOTE", g_rtt_pending_id);
                    SSL_write(ssl, tr.c_str(), static_cast<int>(tr.size()));
                }
            }

            // Diagnostic every 60s -- visibility into engine phase + vol state
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_diag).count() >= 60) {
                last_diag = now;
                if (g_tee_buf) g_tee_buf->force_rotate_check();  // ensure daily log rolls at UTC midnight even if stdout is quiet
                std::cout << "[OMEGA-DIAG] PnL=" << g_omegaLedger.dailyPnl()
                          << " T=" << g_omegaLedger.total()
                          << " WR=" << g_omegaLedger.winRate() << "%"
                          << " RTTp95=" << g_rtt_p95 << "ms"
                          << " cap=" << g_cfg.max_latency_ms << "ms"
                          << " lat_ok=" << (g_governor.checkLatency((g_rtt_p95 > 0.0 ? g_rtt_p95 : g_rtt_last), g_cfg.max_latency_ms) ? 1 : 0)
                          << " session=" << (session_tradeable() ? "ACTIVE" : "CLOSED") << "\n"
                          << "[OMEGA-DIAG] SP phase=" << static_cast<int>(g_eng_sp.phase)
                          << " recent=" << g_eng_sp.recent_vol_pct << "% base=" << g_eng_sp.base_vol_pct << "%"
                          << " ratio=" << (g_eng_sp.base_vol_pct>0 ? g_eng_sp.recent_vol_pct/g_eng_sp.base_vol_pct : 0) << "\n"
                          << "[OMEGA-DIAG] NQ phase=" << static_cast<int>(g_eng_nq.phase)
                          << " recent=" << g_eng_nq.recent_vol_pct << "% base=" << g_eng_nq.base_vol_pct << "%"
                          << " ratio=" << (g_eng_nq.base_vol_pct>0 ? g_eng_nq.recent_vol_pct/g_eng_nq.base_vol_pct : 0) << "\n"
                          << "[OMEGA-DIAG] CL phase=" << static_cast<int>(g_eng_cl.phase)
                          << " recent=" << g_eng_cl.recent_vol_pct << "% base=" << g_eng_cl.base_vol_pct << "%"
                          << " ratio=" << (g_eng_cl.base_vol_pct>0 ? g_eng_cl.recent_vol_pct/g_eng_cl.base_vol_pct : 0) << "\n"
                          << "[OMEGA-DIAG] XAU phase=" << static_cast<int>(g_eng_xau.phase)
                          << " recent=" << g_eng_xau.recent_vol_pct << "% base=" << g_eng_xau.base_vol_pct << "%"
                          << " ratio=" << (g_eng_xau.base_vol_pct>0 ? g_eng_xau.recent_vol_pct/g_eng_xau.base_vol_pct : 0) << "\n";
                // Gold multi-engine stack stats
                g_gold_stack.print_stats();
                std::cout << "[GOLD-DIAG] regime=" << g_gold_stack.regime_name()
                          << " vwap=" << g_gold_stack.vwap()
                          << " vol_range=" << g_gold_stack.vol_range() << "\n";
                // Latency edge engines stats
                g_le_stack.print_stats();
                print_perf_stats();
            }

            if (g_quote_ready.load() && g_cfg.enable_extended_symbols &&
                g_ext_md_refresh_needed.exchange(false)) {
                const std::string ext = build_marketdata_req_extended(g_quote_seq++);
                if (!ext.empty()) {
                    SSL_write(ssl, ext.c_str(), static_cast<int>(ext.size()));
                    std::cout << "[OMEGA] Refreshed EXT subscription from SecurityList\n";
                }
            }

            char buf[8192];
            const int n = SSL_read(ssl, buf, static_cast<int>(sizeof(buf)) - 1);
            if (n <= 0) {
                const int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    Sleep(1); continue;
                }
                std::cerr << "[OMEGA] SSL error " << err << " -- reconnecting\n";
                break;
            }
            for (const auto& m : extract_messages(buf, n)) dispatch_fix(m, ssl);
        }

        g_quote_ready.store(false);
        // Force-close on disconnect -- auto& template lambda works for all typed engines
        auto fc = [](auto& eng, const char* sym) {
            if (!eng.pos.active) return;
            double bid = 0.0, ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find(sym); if (bi != g_bids.end()) bid = bi->second;
              const auto ai = g_asks.find(sym); if (ai != g_asks.end()) ask = ai->second; }
            if (bid > 0.0 && ask > 0.0)
                eng.forceClose(bid, ask, "DISCONNECT", g_rtt_last,
                    g_macroDetector.regime().c_str(),
                    [](const omega::TradeRecord& tr) {
                        handle_closed_trade(tr);
                    });
        };
        fc(g_eng_sp, "US500.F"); fc(g_eng_nq, "USTEC.F"); fc(g_eng_cl, "USOIL.F");
        fc(g_eng_us30, "DJ30.F"); fc(g_eng_nas100, "NAS100");
        fc(g_eng_ger30, "GER30"); fc(g_eng_uk100, "UK100");
        fc(g_eng_estx50, "ESTX50"); fc(g_eng_xag, "XAGUSD"); fc(g_eng_eurusd, "EURUSD");
        fc(g_eng_brent, "UKBRENT"); fc(g_eng_xau, "GOLD.F");
        // Force-close GoldEngineStack
        {
            double g_bid = 0.0, g_ask = 0.0, s_bid = 0.0, s_ask = 0.0;
            { std::lock_guard<std::mutex> lk(g_book_mtx);
              const auto bi = g_bids.find("GOLD.F"); if (bi != g_bids.end()) g_bid = bi->second;
              const auto ai = g_asks.find("GOLD.F"); if (ai != g_asks.end()) g_ask = ai->second;
              const auto sbi = g_bids.find("XAGUSD"); if (sbi != g_bids.end()) s_bid = sbi->second;
              const auto sai = g_asks.find("XAGUSD"); if (sai != g_asks.end()) s_ask = sai->second; }
            if (g_bid > 0.0 && g_ask > 0.0) {
                omega::gold::GoldEngineStack::CloseCallback gold_fc_cb =
                    [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
                g_gold_stack.force_close(g_bid, g_ask, g_rtt_last, gold_fc_cb);
                // Force-close latency edge engines
                omega::latency::LatencyEdgeStack::CloseCb le_cb =
                    [](const omega::TradeRecord& tr) { handle_closed_trade(tr); };
                g_le_stack.force_close_all(g_bid, g_ask,
                    s_bid > 0.0 ? s_bid : g_bid * 0.0185,  // fallback silver price
                    s_ask > 0.0 ? s_ask : g_ask * 0.0185,
                    g_rtt_last, le_cb);
            }
        }

        SSL_shutdown(ssl); SSL_free(ssl); closesocket(static_cast<SOCKET>(sock));
        g_telemetry.UpdateFixStatus("DISCONNECTED", "DISCONNECTED", 0, 0);
        Sleep(static_cast<DWORD>(backoff_ms));
        backoff_ms = std::min(backoff_ms * 2, max_backoff);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    g_singleton_mutex = CreateMutexA(NULL, TRUE, "Global\\Omega_Breakout_System");
    if (!g_singleton_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "[OMEGA] Already running\n"; return 1;
    }

    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0; GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    std::cout << "\033[1;36m"
              << "=======================================================\n"
              << "  OMEGA  |  Commodities & Indices  |  Breakout System  \n"
              << "=======================================================\n"
              << "  Build:   " << OMEGA_VERSION << "  (" << OMEGA_BUILT << ")\n"
              << "  Commit:  " << OMEGA_COMMIT  << "\n"
              << "=======================================================\n"
              << "\033[0m";
    // Also print to stderr so it's visible even if stdout is redirected
    std::fprintf(stderr, "[OMEGA] version=%s built=%s\n", OMEGA_VERSION, OMEGA_BUILT);

    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    const std::string cfg_path = (argc > 1) ? argv[1] : "omega_config.ini";
    load_config(cfg_path);
    sanitize_config();
    apply_shadow_research_profile();
    // Per-symbol typed overloads -- each applies instrument-specific params + macro context ptr
    apply_engine_config(g_eng_sp);   // [sp] section: tp=0.60%, sl=0.35%, vol=0.04%, regime-gated
    apply_engine_config(g_eng_nq);   // [nq] section: tp=0.70%, sl=0.40%, vol=0.05%, regime-gated
    apply_engine_config(g_eng_cl);   // [oil] section: tp=1.20%, sl=0.60%, vol=0.08%, inventory-blocked
    apply_engine_config(g_eng_us30); // typed Us30Engine: macro-gated like SP/NQ
    apply_engine_config(g_eng_nas100); // typed Nas100Engine: macro-gated, independent from USTEC.F
    apply_generic_index_config(g_eng_ger30);
    apply_generic_index_config(g_eng_uk100);
    apply_generic_index_config(g_eng_estx50);
    apply_generic_silver_config(g_eng_xag);
    apply_generic_fx_config(g_eng_eurusd);
    apply_generic_brent_config(g_eng_brent);
    // Gold: generic breakout engine, overridden with gold-specific pct params
    // Gold: dedicated config -- do not use generic breakout defaults
    g_eng_xau.macro                 = &g_macro_ctx;  // gold uses inverse regime logic
    g_eng_xau.TP_PCT                = g_cfg.gold_tp_pct;
    g_eng_xau.SL_PCT                = g_cfg.gold_sl_pct;
    g_eng_xau.VOL_THRESH_PCT        = g_cfg.gold_vol_thresh_pct;
    g_eng_xau.COMPRESSION_LOOKBACK  = 60;   // gold compresses slower than indices
    g_eng_xau.BASELINE_LOOKBACK     = 250;  // longer baseline -- gold trends persist
    // g_eng_xau.COMPRESSION_THRESHOLD set to 0.85 in GoldEngine constructor -- do not override
    g_eng_xau.MAX_HOLD_SEC          = g_cfg.max_hold_sec;
    g_eng_xau.MIN_GAP_SEC           = 180;  // 3min gap between signals
    g_eng_xau.MAX_SPREAD_PCT        = 0.06; // gold spreads slightly wider than indices

    // ── Startup parameter validation — logged on every start ─────────────────
    // This block documents the exact live values every engine will use.
    // Any mismatch between config intent and actual values is visible immediately.
    std::cout << "\n[OMEGA-PARAMS] ═══════════════════════════════════════════\n"
              << "[OMEGA-PARAMS] ENGINE PARAMETER AUDIT (live values after all config overrides)\n"
              << "[OMEGA-PARAMS] ───────────────────────────────────────────\n"
              << "[OMEGA-PARAMS] US500.F  TP=" << g_eng_sp.TP_PCT  << "% SL=" << g_eng_sp.SL_PCT
              << "% vol=" << g_eng_sp.VOL_THRESH_PCT << "% mom=" << g_eng_sp.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_sp.MIN_BREAKOUT_PCT << "% gap=" << g_eng_sp.MIN_GAP_SEC
              << "s hold=" << g_eng_sp.MAX_HOLD_SEC << "s spread=" << g_eng_sp.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USTEC.F  TP=" << g_eng_nq.TP_PCT  << "% SL=" << g_eng_nq.SL_PCT
              << "% vol=" << g_eng_nq.VOL_THRESH_PCT << "% mom=" << g_eng_nq.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nq.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nq.MIN_GAP_SEC
              << "s hold=" << g_eng_nq.MAX_HOLD_SEC << "s spread=" << g_eng_nq.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] USOIL.F  TP=" << g_eng_cl.TP_PCT  << "% SL=" << g_eng_cl.SL_PCT
              << "% vol=" << g_eng_cl.VOL_THRESH_PCT << "% mom=" << g_eng_cl.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_cl.MIN_BREAKOUT_PCT << "% gap=" << g_eng_cl.MIN_GAP_SEC
              << "s hold=" << g_eng_cl.MAX_HOLD_SEC << "s spread=" << g_eng_cl.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] DJ30.F   TP=" << g_eng_us30.TP_PCT << "% SL=" << g_eng_us30.SL_PCT
              << "% vol=" << g_eng_us30.VOL_THRESH_PCT << "% mom=" << g_eng_us30.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_us30.MIN_BREAKOUT_PCT << "% gap=" << g_eng_us30.MIN_GAP_SEC
              << "s hold=" << g_eng_us30.MAX_HOLD_SEC << "s spread=" << g_eng_us30.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] NAS100   TP=" << g_eng_nas100.TP_PCT << "% SL=" << g_eng_nas100.SL_PCT
              << "% vol=" << g_eng_nas100.VOL_THRESH_PCT << "% mom=" << g_eng_nas100.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_nas100.MIN_BREAKOUT_PCT << "% gap=" << g_eng_nas100.MIN_GAP_SEC
              << "s hold=" << g_eng_nas100.MAX_HOLD_SEC << "s spread=" << g_eng_nas100.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] XAGUSD   TP=" << g_eng_xag.TP_PCT  << "% SL=" << g_eng_xag.SL_PCT
              << "% vol=" << g_eng_xag.VOL_THRESH_PCT << "% mom=" << g_eng_xag.MOMENTUM_THRESH_PCT
              << "% brk=" << g_eng_xag.MIN_BREAKOUT_PCT << "% gap=" << g_eng_xag.MIN_GAP_SEC
              << "s hold=" << g_eng_xag.MAX_HOLD_SEC << "s spread=" << g_eng_xag.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] GOLD.F   TP=" << g_eng_xau.TP_PCT  << "% SL=" << g_eng_xau.SL_PCT
              << "% vol=" << g_eng_xau.VOL_THRESH_PCT << "% gap=" << g_eng_xau.MIN_GAP_SEC
              << "s hold=" << g_eng_xau.MAX_HOLD_SEC << "s spread=" << g_eng_xau.MAX_SPREAD_PCT << "%\n"
              << "[OMEGA-PARAMS] GoldStack MIN_ENTRY_GAP=30s MAX_HOLD=600s REGIME_FLIP_MIN=60s\n"
              << "[OMEGA-PARAMS] LeadLag=DISABLED SpreadDisloc_cooldown=60s EventComp_max_daily=4\n"
              << "[OMEGA-PARAMS] ═══════════════════════════════════════════\n\n";
    std::cout.flush();

    if (g_cfg.mode == "SHADOW") {
        const bool shadow_research = g_cfg.shadow_research_mode;
        g_eng_sp.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_nq.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_cl.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_us30.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_nas100.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_ger30.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_uk100.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_estx50.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_xag.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_eurusd.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_brent.AGGRESSIVE_SHADOW = shadow_research;
        g_eng_xau.AGGRESSIVE_SHADOW = shadow_research;

        if (g_cfg.shadow_ustec_pilot_only) {
            g_eng_nq.ENTRY_SIZE = g_cfg.ustec_pilot_size;
            g_eng_nq.MIN_GAP_SEC = g_cfg.ustec_pilot_min_gap_sec;
            g_eng_nq.MAX_TRADES_PER_MIN = std::min(g_eng_nq.MAX_TRADES_PER_MIN, 2);
            std::cout << "[OMEGA-PILOT] USTEC shadow pilot enabled | size=" << g_eng_nq.ENTRY_SIZE
                      << " min_gap=" << g_eng_nq.MIN_GAP_SEC
                      << " max_trades_per_min=" << g_eng_nq.MAX_TRADES_PER_MIN << "\n";
        } else {
            std::cout << "[OMEGA-PILOT] Multi-symbol shadow enabled | all configured engines may trade\n";
        }
        std::cout << "[OMEGA-MODE] SHADOW "
                  << (shadow_research ? "research/discovery" : "paper/live-like")
                  << " execution profile active\n";
    }

    auto bind_shadow_cb = [](auto& eng) {
        eng.shadow_signal_cb =
            [](const char* symbol, bool is_long, double entry, double tp, double sl,
               const char* verdict, const char* reason) {
                if (!g_cfg.enable_shadow_signal_audit) return;
                ShadowSignalPos p;
                p.active  = true;
                p.symbol  = symbol ? symbol : "";
                p.is_long = is_long;
                p.entry   = entry;
                p.tp      = tp;
                p.sl      = sl;
                p.entry_ts = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                p.verdict = verdict ? verdict : "";
                p.reason  = reason ? reason : "";
                write_shadow_signal_event(p);
                std::lock_guard<std::mutex> lk(g_shadow_signal_mtx);
                g_shadow_signal_positions.push_back(std::move(p));
            };
    };
    bind_shadow_cb(g_eng_sp);
    bind_shadow_cb(g_eng_nq);
    bind_shadow_cb(g_eng_cl);
    bind_shadow_cb(g_eng_us30);
    bind_shadow_cb(g_eng_nas100);
    bind_shadow_cb(g_eng_ger30);
    bind_shadow_cb(g_eng_uk100);
    bind_shadow_cb(g_eng_estx50);
    bind_shadow_cb(g_eng_xag);
    bind_shadow_cb(g_eng_eurusd);
    bind_shadow_cb(g_eng_brent);
    bind_shadow_cb(g_eng_xau);
    build_id_map();

    // Open log file and tee stdout into it
    // Rolling log: logs/omega_YYYY-MM-DD.log, UTC daily rotation, 5-file retention
    {
        const std::string log_dir = log_root_dir();
        g_orig_cout = std::cout.rdbuf();
        g_tee_buf   = new RollingTeeBuffer(g_orig_cout, log_dir);
        if (!g_tee_buf->is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open rolling log under " << log_dir << "\n";
            delete g_tee_buf;
            g_tee_buf = nullptr;
            return 1;
        }
        std::cout.rdbuf(g_tee_buf);
        std::cerr.rdbuf(g_tee_buf);  // tee stderr too — nothing gets lost
        std::cout << "[OMEGA] Rolling log: " << g_tee_buf->current_path()
                  << " (UTC daily rotation, 5-file retention)\n";
    }

    {
        const std::string trade_dir = log_root_dir() + "/trades";
        const std::string gold_dir  = log_root_dir() + "/gold";
        const std::string shadow_trade_dir = log_root_dir() + "/shadow/trades";
        const std::string shadow_signal_dir = log_root_dir() + "/shadow/signals";
        const std::string shadow_signal_event_dir = log_root_dir() + "/shadow/events";
        const std::string header =
            "trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,"
            "exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,"
            "entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,"
            "slippage_entry,slippage_exit,commission,"
            "slip_entry_pct,slip_exit_pct,comm_per_side,"
            "mfe,mae,hold_sec,spread_at_entry,"
            "latency_ms,regime,exit_reason";
        const std::string shadow_signal_event_header =
            "event_ts_unix,event_ts_utc,event_utc_weekday,symbol,side,entry_px,tp,sl,verdict,reason";
        const std::string shadow_signal_header =
            "entry_ts_unix,entry_ts_utc,entry_utc_weekday,exit_ts_unix,exit_ts_utc,"
            "exit_utc_weekday,symbol,side,entry_px,exit_px,tp,sl,pnl,hold_sec,"
            "verdict,reason,exit_reason";

        const std::string trade_csv_path = trade_dir + "/omega_trade_closes.csv";
        ensure_parent_dir(trade_csv_path);
        // Always truncate and rewrite header on startup — ensures column schema
        // matches current build. Old header with missing columns (e.g. gross_pnl,
        // net_pnl added in a later build) would cause blank fields in PowerShell.
        g_trade_close_csv.open(trade_csv_path, std::ios::trunc);
        if (!g_trade_close_csv.is_open()) {
            std::cerr << "[OMEGA-FATAL] Failed to open full trade CSV: " << trade_csv_path << "\n";
            return 1;
        }
        g_trade_close_csv << header << '\n';
        std::cout << "[OMEGA] Full Trade CSV: " << trade_csv_path << "\n";

        g_daily_trade_close_log = std::make_unique<RollingCsvLogger>(
            trade_dir, "omega_trade_closes", header);
        g_daily_gold_trade_close_log = std::make_unique<RollingCsvLogger>(
            gold_dir, "omega_gold_trade_closes", header);
        g_daily_shadow_trade_log = std::make_unique<RollingCsvLogger>(
            shadow_trade_dir, "omega_shadow_trades", header);
        g_daily_shadow_signal_log = std::make_unique<RollingCsvLogger>(
            shadow_signal_dir, "omega_shadow_signals", shadow_signal_header);
        g_daily_shadow_signal_event_log = std::make_unique<RollingCsvLogger>(
            shadow_signal_event_dir, "omega_shadow_signal_events", shadow_signal_event_header);
        std::cout << "[OMEGA] Daily Trade Logs: " << trade_dir
                  << "/omega_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Gold Logs: " << gold_dir
                  << "/omega_gold_trade_closes_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Shadow Trade Logs: " << shadow_trade_dir
                  << "/omega_shadow_trades_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Shadow Signal Logs: " << shadow_signal_dir
                  << "/omega_shadow_signals_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
        std::cout << "[OMEGA] Daily Shadow Signal Event Logs: " << shadow_signal_event_dir
                  << "/omega_shadow_signal_events_YYYY-MM-DD.csv (UTC, 5-file retention)\n";
    }

    const std::string shadow_csv_path =
        resolve_audit_log_path(g_cfg.shadow_csv, "shadow/omega_shadow.csv");
    ensure_parent_dir(shadow_csv_path);
    // Truncate on startup — ensures header always matches current schema.
    g_shadow_csv.open(shadow_csv_path, std::ios::trunc);
    if (!g_shadow_csv.is_open()) {
        std::cerr << "[OMEGA-FATAL] Failed to open shadow trade CSV: " << shadow_csv_path << "\n";
        return 1;
    }
    g_shadow_csv << "ts_unix,symbol,side,entry_px,exit_px,pnl,mfe,mae,"
                    "hold_sec,reason,spread_at_entry,latency_ms,regime\n";
    std::cout << "[OMEGA] Shadow CSV: " << shadow_csv_path << "\n";

    const std::string shadow_signal_csv_path =
        resolve_audit_log_path(g_cfg.shadow_signal_csv, "shadow/omega_shadow_signals.csv");
    ensure_parent_dir(shadow_signal_csv_path);
    // Truncate on startup.
    g_shadow_signal_csv.open(shadow_signal_csv_path, std::ios::trunc);
    if (!g_shadow_signal_csv.is_open()) {
        std::cerr << "[OMEGA-FATAL] Failed to open shadow signal CSV: " << shadow_signal_csv_path << "\n";
        return 1;
    }
    g_shadow_signal_csv << "ts_unix,symbol,side,entry_px,exit_px,tp,sl,pnl,hold_sec,verdict,reason,exit_reason\n";
    std::cout << "[OMEGA] Shadow Signal CSV: " << shadow_signal_csv_path << "\n";

    const std::string shadow_signal_event_csv_path =
        resolve_audit_log_path("logs/shadow/omega_shadow_signal_events.csv",
                               "shadow/omega_shadow_signal_events.csv");
    ensure_parent_dir(shadow_signal_event_csv_path);
    // Truncate on startup.
    g_shadow_signal_event_csv.open(shadow_signal_event_csv_path, std::ios::trunc);
    if (!g_shadow_signal_event_csv.is_open()) {
        std::cerr << "[OMEGA-FATAL] Failed to open shadow signal event CSV: "
                  << shadow_signal_event_csv_path << "\n";
        return 1;
    }
    g_shadow_signal_event_csv
        << "event_ts_unix,event_ts_utc,event_utc_weekday,symbol,side,entry_px,tp,sl,verdict,reason\n";
    std::cout << "[OMEGA] Shadow Signal Event CSV: " << shadow_signal_event_csv_path << "\n";

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[OMEGA] WSAStartup failed\n"; return 1;
    }
    SSL_library_init(); SSL_load_error_strings(); OpenSSL_add_all_algorithms();

    if (!g_telemetry.Init()) std::cerr << "[OMEGA] Telemetry init failed\n";
    g_telemetry.SetMode(g_cfg.mode.c_str());
    g_telemetry.UpdateBuildVersion(OMEGA_VERSION, OMEGA_BUILT);

    omega::OmegaTelemetryServer gui_server;
    gui_server.start(g_cfg.gui_port, g_cfg.ws_port, g_telemetry.snap());
    std::cout << "[OMEGA] GUI http://localhost:" << g_cfg.gui_port
              << "  WS:" << g_cfg.ws_port << "\n";

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    std::cout << "[OMEGA] FIX loop starting -- " << g_cfg.mode << " mode\n";
    // Launch trade connection in background thread
    std::thread trade_thread(trade_loop);
    trade_thread.detach();
    Sleep(500);  // Give trade connection 500ms head start before quote loop
    quote_loop();

    std::cout << "[OMEGA] Shutdown\n";
    gui_server.stop();
    if (g_daily_trade_close_log) g_daily_trade_close_log->close();
    if (g_daily_gold_trade_close_log) g_daily_gold_trade_close_log->close();
    if (g_daily_shadow_trade_log) g_daily_shadow_trade_log->close();
    if (g_daily_shadow_signal_log) g_daily_shadow_signal_log->close();
    if (g_daily_shadow_signal_event_log) g_daily_shadow_signal_event_log->close();
    g_trade_close_csv.close();
    g_shadow_csv.close();
    g_shadow_signal_csv.close();
    g_shadow_signal_event_csv.close();
    if (g_tee_buf)   { g_tee_buf->flush_and_close(); std::cout.rdbuf(g_orig_cout); }
    WSACleanup();
    ReleaseMutex(g_singleton_mutex);
    CloseHandle(g_singleton_mutex);
    return 0;
}
