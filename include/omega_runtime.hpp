#pragma once
// omega_runtime.hpp -- extracted from main.cpp
// SINGLE-TRANSLATION-UNIT include -- only include from main.cpp

static void set_ctrader_tick_ms(const std::string& sym, int64_t ms) noexcept {
    auto* p = get_ctrader_tick_ms_ptr(sym);
    if (p) p->store(ms, std::memory_order_relaxed);
}
static bool ctrader_depth_is_live(const std::string& sym) noexcept {
    auto* p = get_ctrader_tick_ms_ptr(sym);
    if (!p) return false;
    const int64_t last = p->load(std::memory_order_relaxed);
    if (last == 0) return false;
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now_ms - last) < 500;  // fresh if cTrader event arrived within 500ms
}

// RTT
static double              g_rtt_last = 0.0, g_rtt_p50 = 0.0, g_rtt_p95 = 0.0;
// Live USDJPY mid -- updated every tick, used by tick_value_multiplier().
// Avoids the static 667 approximation (100000/150) drifting ?8% as rate moves.
// Initialised to 150.0 so the function is safe before the first USDJPY tick arrives.
static std::atomic<double> g_usdjpy_mid{150.0};
static std::deque<double>  g_rtts;
static int64_t             g_rtt_pending_ts = 0;
static std::string         g_rtt_pending_id;

// Governor counters
static int     g_gov_spread  = 0;
static int     g_gov_lat     = 0;
static int     g_gov_pnl     = 0;
// Last lot size computed by enter_directional -- written on success so callers
// can patch pos_.size for accurate shadow P&L simulation.
static double  g_last_directional_lot = 0.01;
static int     g_gov_pos     = 0;
static int     g_gov_consec  = 0;

struct SymbolRiskState {
    double daily_pnl = 0.0;
    int    consec_losses = 0;
    int64_t pause_until = 0;
};
struct ShadowQualityState {
    int     fast_loss_streak  = 0;
    int64_t pause_until       = 0;
    int     engine_consec_sl  = 0;   // consecutive SL_HIT per engine key
    bool    engine_culled     = false; // culled until session restart
};
static std::mutex g_sym_risk_mtx;
static std::unordered_map<std::string, SymbolRiskState> g_sym_risk;

// ?? Hourly P&L ring buffer -- rolling 2-hour loss throttle ????????????????????
// Records net_pnl of each closed trade with its close timestamp.
// On each symbol_gate call we sum trades from last 2h and block if > hourly_loss_limit.
struct HourlyPnlRecord { int64_t ts_sec; double net_pnl; };
static std::mutex                    g_hourly_pnl_mtx;
static std::deque<HourlyPnlRecord>   g_hourly_pnl_records;
static constexpr int64_t HOURLY_WINDOW_SEC = 7200; // 2-hour rolling window
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

// ?? Stale quote watchdog ?????????????????????????????????????????????????????
// Tracks the last time any tick was received per symbol.
// If last_tick_age > STALE_QUOTE_SEC while positions are open ? widen SLs and
// alert. A frozen feed with open positions is the most dangerous state.
static std::mutex                              g_last_tick_mtx;
static std::unordered_map<std::string,int64_t> g_last_tick_ts;   // symbol ? unix ms of last tick
static std::unordered_map<std::string,double>  g_last_tick_bid;  // symbol ? last bid price
static std::unordered_map<std::string,int>     g_frozen_count;   // symbol ? consecutive identical ticks
static constexpr int64_t STALE_QUOTE_SEC  = 30;   // 30s without tick = genuinely stale feed
static constexpr int     FROZEN_TICK_MAX  = 20;   // 20 consecutive identical bids = frozen feed
                                                   // At ~1 tick/10s: 20 ticks = 3.3 min of freeze
                                                   // Price-freeze detection catches brokers that
                                                   // repeat last-price with updated timestamps.

// Record a tick receipt (called from on_tick per symbol)
// Also tracks consecutive identical bids for frozen-feed detection.
static inline void stale_watchdog_ping(const std::string& sym, double bid = 0.0) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_last_tick_mtx);
    g_last_tick_ts[sym] = now_ms;
    if (bid > 0.0) {
        auto it = g_last_tick_bid.find(sym);
        if (it != g_last_tick_bid.end() && std::fabs(it->second - bid) < 0.001) {
            g_frozen_count[sym]++;
        } else {
            g_frozen_count[sym] = 0;
            g_last_tick_bid[sym] = bid;
        }
    }
}

// Returns true if symbol has received a tick within STALE_QUOTE_SEC
static inline bool stale_watchdog_ok(const std::string& sym) {
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard<std::mutex> lk(g_last_tick_mtx);
    auto it = g_last_tick_ts.find(sym);
    if (it == g_last_tick_ts.end()) return false;  // never received -- treat as stale
    return (now_ms - it->second) < STALE_QUOTE_SEC * 1000;
}

// ?? Weekend gap sizing ???????????????????????????????????????????????????????
// Friday 21:00 UTC through Sunday 22:00 UTC -- markets closed, gap risk high.
// GOLD can gap 1.5-2% on Sunday open. Size is halved, SL widened.
// Returns a multiplier [0.5, 1.0] to apply to computed lot size.
// Always 1.0 during normal sessions.
static inline double weekend_gap_size_scale() {
    const int64_t now_sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    time_t t = static_cast<time_t>(now_sec);
    struct tm ti{};
#ifdef _WIN32
    gmtime_s(&ti, &t);
#else
    gmtime_r(&t, &ti);
#endif
    // tm_wday: 0=Sun 1=Mon 2=Tue 3=Wed 4=Thu 5=Fri 6=Sat
    const int wday = ti.tm_wday;
    const int hour = ti.tm_hour;
    // Friday >= 21:00 UTC or Saturday (all day) or Sunday < 22:00 UTC
    const bool in_gap_window =
        (wday == 5 && hour >= 21) ||   // Friday night
        (wday == 6) ||                 // All day Saturday
        (wday == 0 && hour < 22);      // Sunday before open
    if (in_gap_window) {
        static int64_t s_gap_log = 0;
        if (now_sec - s_gap_log > 3600) {  // log once per hour
            s_gap_log = now_sec;
            std::printf("[WEEKEND-GAP] Gap window active -- sizing 0.5x\n");
        }
        return 0.50;
    }
    return 1.00;
}

// Trade connection (port 5212) -- separate SSL from quote (port 5211)
static SSL*               g_trade_ssl  = nullptr;
static int                g_trade_sock = -1;
static std::atomic<bool>  g_trade_ready{false};
static std::mutex         g_trade_mtx;
static int                g_trade_seq  = 1;
static std::atomic<bool>  g_quote_ready{false};
static std::atomic<int64_t> g_connected_since{0};  // unix seconds of last successful logon
static std::atomic<bool>  g_ext_md_refresh_needed{false};
static std::atomic<bool>  g_md_subscribed{false};   // true once OMEGA-MD-ALL is active on this session

// Live unrealised P&L -- updated every tick, read by GUI and risk gates.
// Stored as int64 (cents) to allow lock-free atomic read/write.
// GUI daily_pnl = closed_pnl + g_open_unrealised_pnl_cents / 100.0
static std::atomic<int64_t> g_open_unrealised_cents{0};

// Portfolio open SL risk tracker -- sum of max_dollar_loss across all open positions.
// Incremented on entry (sl_pts * lot * tick_value), decremented on close.
// Stored in cents (int64) for lock-free atomic access on hot path.
// Checked in symbol_gate when max_portfolio_sl_risk_usd > 0.
static std::atomic<int64_t> g_open_sl_risk_cents{0};

inline void portfolio_sl_risk_add(double sl_pts, double lot, double tick_value) {
    if (sl_pts <= 0.0 || lot <= 0.0 || tick_value <= 0.0) return;
    const int64_t cents = static_cast<int64_t>((sl_pts * lot * tick_value) * 100.0);
    g_open_sl_risk_cents.fetch_add(cents, std::memory_order_relaxed);
}
inline void portfolio_sl_risk_sub(double sl_pts, double lot, double tick_value) {
    if (sl_pts <= 0.0 || lot <= 0.0 || tick_value <= 0.0) return;
    const int64_t cents = static_cast<int64_t>((sl_pts * lot * tick_value) * 100.0);
    // Never go below zero -- defensive against double-decrements
    const int64_t prev = g_open_sl_risk_cents.fetch_sub(cents, std::memory_order_relaxed);
    if (prev < cents) g_open_sl_risk_cents.store(0, std::memory_order_relaxed);
}

// Shadow CSV
static std::ofstream g_shadow_csv;
static std::ofstream g_trade_close_csv;
static std::ofstream g_trade_open_csv;   // entry-time log -- one row per position opened
static std::mutex    g_trade_close_csv_mtx;

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
    // Route gold trades to the specific engine that generated them,
    // not a single "GOLD_STACK" bucket. This prevents auto-disable
    // from misfiring when losses are from flow/bracket, not the stack.
    if (tr.symbol == "XAUUSD") {
        if (tr.engine.find("BRACKET") != std::string::npos)   return "XAUUSD_BRACKET";
        if (tr.engine.find("L2_FLOW") != std::string::npos ||
            tr.engine.find("GOLD_FLOW") != std::string::npos) return "XAUUSD_FLOW";
        if (tr.engine.find("LEAD_LAG") != std::string::npos)  return "XAUUSD_LATENCY";
        return "GOLD_STACK";  // CompressionBreakout / Impulse / SessionMom / VWAPSnap / SweepPro
    }
    return tr.symbol;
}

static std::string build_trade_close_csv_row(const omega::TradeRecord& tr);
static void write_trade_open_log(const std::string& symbol, const std::string& engine,
                                  const std::string& side, double entry_px, double tp,
                                  double sl, double size, double spread_at_entry,
                                  const std::string& regime, const std::string& reason);
static void print_perf_stats() {
    std::lock_guard<std::mutex> lk(g_perf_mtx);
    if (g_perf.empty()) return;
    for (const auto& kv : g_perf) {
        const auto& k = kv.first;
        const auto& s = kv.second;
        std::cout << "[OMEGA-PERF] " << k
                  << " liveT=" << s.live_trades
                  << " livePnL=" << std::fixed << std::setprecision(2) << s.live_pnl
                  << " WR=" << std::fixed << std::setprecision(1)
                  << (s.live_trades > 0 ? (100.0 * s.live_wins / s.live_trades) : 0.0)
                  << "% shadowT=" << s.shadow_trades
                  << " shadowPnL=" << std::fixed << std::setprecision(2) << s.shadow_pnl
                  << " disabled=" << (s.disabled ? 1 : 0) << "\n";
        std::cout.unsetf(std::ios::fixed);
        std::cout << std::setprecision(6); // restore default precision
    }
}

// ?????????????????????????????????????????????????????????????????????????????
// ?????????????????????????????????????????????????????????????????????????????
// RollingTeeBuffer -- mirrors stdout to a daily rolling log file
// Rotates at UTC midnight. Keeps LOG_KEEP_DAYS files, deletes older ones.
// File naming: logs/omega_YYYY-MM-DD.log
// Thread-safe: mtx_ serialises all streambuf operations.
// Without this mutex the FIX thread and cTrader depth thread both write to
// std::cout concurrently, corrupting at_line_start_ and the ofstream state
// -> undefined behaviour -> crash ~30s into startup during the book burst.
//
// LOG HEALTH GUARANTEES (never-stale contract):
//   1. latest.log TRUNCATED on every open_today() call -- can NEVER contain
//      content from a prior process run. std::ios::trunc overwrites from byte 0.
//   2. latest.log open FAILURE is immediately printed to console (NSSM stdout)
//      AND written into the dated log. [OMEGA-LOG-LATEST-FAIL] is always visible.
//   3. [LOG-HEALTH] heartbeat emitted every LOG_HEALTH_INTERVAL_SEC seconds.
//      Monitoring scripts grep this line and check its embedded timestamp.
//      Format: [LOG-HEALTH] dated=ok latest=<ok|FAIL> ts=HH:MM:SS
//      The first heartbeat fires at the moment of open_today() so the very
//      first line in both files confirms their status.
//   4. is_latest_open() / is_dated_open() public accessors for omega_main.hpp
//      health checks and GUI telemetry.
//   5. latest_path() public accessor so scripts know which file is live.
// ?????????????????????????????????????????????????????????????????????????????
class RollingTeeBuffer : public std::streambuf {
public:
    static constexpr int LOG_KEEP_DAYS           = 10;
    static constexpr int LOG_HEALTH_INTERVAL_SEC = 60;  // heartbeat cadence

    explicit RollingTeeBuffer(std::streambuf* orig, const std::string& log_dir)
        : orig_(orig), log_dir_(log_dir)
    {
        open_today();
    }

    int overflow(int c) override {
        if (c == EOF) return !EOF;
        std::lock_guard<std::mutex> lk(mtx_);
        check_rotate();
        maybe_emit_health_heartbeat();
        orig_->sputc(static_cast<char>(c));
        if (file_buf_) {
            if (at_line_start_ && c != '\n') {
                write_ts_prefix();
                at_line_start_ = false;
            }
            file_buf_->sputc(static_cast<char>(c));
            if (latest_buf_) latest_buf_->sputc(static_cast<char>(c));
            if (c == '\n') {
                file_.flush();
                if (latest_buf_) latest_.flush();
                at_line_start_ = true;
            }
        }
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lk(mtx_);
        check_rotate();
        maybe_emit_health_heartbeat();
        orig_->sputn(s, n);
        if (file_buf_) {
            const char* p   = s;
            const char* end = s + n;
            while (p < end) {
                if (at_line_start_) {
                    write_ts_prefix();
                    at_line_start_ = false;
                }
                const char* nl = static_cast<const char*>(
                    std::memchr(p, '\n', static_cast<size_t>(end - p)));
                if (nl) {
                    file_buf_->sputn(p, (nl - p) + 1);
                    if (latest_buf_) latest_buf_->sputn(p, (nl - p) + 1);
                    at_line_start_ = true;
                    p = nl + 1;
                } else {
                    file_buf_->sputn(p, end - p);
                    if (latest_buf_) latest_buf_->sputn(p, end - p);
                    break;
                }
            }
            file_.flush();
            if (latest_buf_) latest_.flush();
        }
        return n;
    }

    std::string current_path() const { std::lock_guard<std::mutex> lk(mtx_); return current_path_; }
    std::string latest_path()  const { std::lock_guard<std::mutex> lk(mtx_); return latest_path_; }
    bool is_open()             const { std::lock_guard<std::mutex> lk(mtx_); return file_.is_open(); }
    bool is_dated_open()       const { std::lock_guard<std::mutex> lk(mtx_); return file_.is_open(); }
    bool is_latest_open()      const { std::lock_guard<std::mutex> lk(mtx_); return latest_.is_open(); }

    void force_rotate_check() {
        std::lock_guard<std::mutex> lk(mtx_);
        const std::string before = current_path_;
        check_rotate();
        if (current_path_ != before && file_buf_) {
            const std::string hdr = "[OMEGA-LOG] Daily rotation -- new log: " + current_path_ + "\n";
            file_buf_->sputn(hdr.c_str(), (std::streamsize)hdr.size());
            file_.flush();
        }
    }

    void flush_and_close() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (file_.is_open()) { file_.flush(); file_.close(); }
        file_buf_ = nullptr;
        if (latest_.is_open()) { latest_.flush(); latest_.close(); }
        latest_buf_ = nullptr;
    }

private:
    mutable std::mutex mtx_;  // serialises all writes -- FIX + cTrader threads both use std::cout
    std::streambuf* orig_;
    std::string     log_dir_;
    std::ofstream   file_;
    std::streambuf* file_buf_  = nullptr;
    std::ofstream   latest_;        // always points to logs/latest.log (truncated on each open_today)
    std::streambuf* latest_buf_ = nullptr;
    std::string     current_path_;
    std::string     latest_path_;   // stored for is_latest_open() / latest_path() accessors
    int             current_day_  = -1;
    bool            at_line_start_ = true;  // true when next char starts a new line
    int64_t         last_health_ts_ = 0;    // unix seconds of last [LOG-HEALTH] emission

    // -------------------------------------------------------------------------
    // maybe_emit_health_heartbeat
    // Called under mtx_ from overflow() and xsputn().
    // Emits a [LOG-HEALTH] line to both dated and latest files at most once per
    // LOG_HEALTH_INTERVAL_SEC seconds.  The first call (last_health_ts_==0) fires
    // immediately so the opening lines of both files confirm their own status.
    //
    // Format:  [LOG-HEALTH] dated=ok latest=<ok|FAIL> ts=HH:MM:SS
    //
    // Monitoring scripts:
    //   - Grep for "[LOG-HEALTH]" and check embedded ts= against wall clock.
    //   - If latest=FAIL all scripts MUST fall back to dated log.
    //   - Gap between consecutive [LOG-HEALTH] ts= values > 120s means
    //     the process is frozen or dead.
    // -------------------------------------------------------------------------
    void maybe_emit_health_heartbeat() {
        auto now_tp  = std::chrono::system_clock::now();
        auto now_t   = std::chrono::system_clock::to_time_t(now_tp);
        const int64_t now_sec = static_cast<int64_t>(now_t);
        if (now_sec - last_health_ts_ < LOG_HEALTH_INTERVAL_SEC) return;
        last_health_ts_ = now_sec;

        struct tm ti{};
        gmtime_s(&ti, &now_t);
        char ts_buf[16];
        std::snprintf(ts_buf, sizeof(ts_buf), "%02d:%02d:%02d",
                      ti.tm_hour, ti.tm_min, ti.tm_sec);

        const char* latest_status = latest_.is_open() ? "ok" : "FAIL";
        // Build line without injecting via the normal path (avoids re-entrancy on at_line_start_)
        char hb[128];
        std::snprintf(hb, sizeof(hb),
                      "%02d:%02d:%02d [LOG-HEALTH] dated=ok latest=%s ts=%s\n",
                      ti.tm_hour, ti.tm_min, ti.tm_sec,
                      latest_status, ts_buf);
        const std::streamsize hb_len = static_cast<std::streamsize>(std::strlen(hb));

        if (file_buf_)   { file_buf_->sputn(hb, hb_len);   file_.flush(); }
        if (latest_buf_) { latest_buf_->sputn(hb, hb_len); latest_.flush(); }
        // Console (NSSM stdout) -- strip the timestamp prefix for readability
        if (orig_) {
            const char* msg_start = std::strchr(hb, '[');
            if (msg_start) orig_->sputn(msg_start, (std::streamsize)std::strlen(msg_start));
        }
        // at_line_start_ remains true: heartbeat always ends with \n
        at_line_start_ = true;
    }

    void write_ts_prefix() {
        // UTC HH:MM:SS prefix injected into file only (not console)
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        struct tm ti{};
        gmtime_s(&ti, &t);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d ",
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
        file_buf_->sputn(buf, 9);
        if (latest_buf_) latest_buf_->sputn(buf, 9);
    }

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
        // Use filesystem::create_directories -- _mkdir returns -1 if dir exists
        // which was silently ignored, but more importantly create_directories
        // handles nested paths and Unicode correctly on Windows.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(fs::path(log_dir_), ec);
        current_path_ = log_dir_ + "/omega_" + utc_date_str() + ".log";
        file_.open(current_path_, std::ios::app);
        file_buf_         = file_.is_open() ? file_.rdbuf() : nullptr;
        current_day_      = utc_day_of_year();
        current_date_str_ = utc_date_str();
        if (!file_.is_open()) {
            // Print direct to orig_ (console) -- tee not usable if file failed
            const std::string msg = "[OMEGA-LOG-FAIL] Cannot open dated log: " + current_path_ + "\n";
            if (orig_) orig_->sputn(msg.c_str(), (std::streamsize)msg.size());
        }

        // ── latest.log ──────────────────────────────────────────────────────
        // GUARANTEE: std::ios::trunc overwrites from byte 0 on every open_today()
        // call. latest.log can NEVER contain output from a prior process run.
        // If open fails we emit [OMEGA-LOG-LATEST-FAIL] to console AND to the
        // dated log so the failure is always visible in at least one place.
        if (latest_.is_open()) { latest_.flush(); latest_.close(); latest_buf_ = nullptr; }
        latest_path_ = log_dir_ + "/latest.log";
        latest_.open(latest_path_, std::ios::trunc);
        if (latest_.is_open()) {
            latest_buf_ = latest_.rdbuf();
        } else {
            latest_buf_ = nullptr;
            // ── EXPLICIT FAILURE NOTIFICATION ─────────────────────────────
            // 1. Console (captured by NSSM stdout log)
            const std::string fail_msg =
                "[OMEGA-LOG-LATEST-FAIL] Cannot open latest.log: " + latest_path_
                + " -- monitoring scripts MUST use dated log: " + current_path_ + "\n";
            if (orig_) orig_->sputn(fail_msg.c_str(), (std::streamsize)fail_msg.size());
            // 2. Dated log (permanent record)
            if (file_buf_) {
                file_buf_->sputn(fail_msg.c_str(), (std::streamsize)fail_msg.size());
                file_.flush();
            }
        }

        // Emit the very first [LOG-HEALTH] heartbeat immediately.
        // This is the first line written into both new files and confirms their
        // open status before any other engine output arrives.
        last_health_ts_ = 0;
        maybe_emit_health_heartbeat();

        purge_old_logs();
    }

    void check_rotate() {
        // Compare full date string not just day-of-year -- day-of-year wraps
        // at year boundary and can miss the Jan 1 rotation.
        if (utc_date_str() != current_date_str_)
            open_today();
    }

    std::string current_date_str_; // set in open_today via utc_date_str()
    void purge_old_logs() {
        // Enumerate logs/omega_*.log:
        //   - Files older than LOG_ARCHIVE_AFTER_DAYS: compress to logs/archive/omega_YYYY-MM-DD.zip
        //     and delete the original. Keeps full history without eating disk.
        //   - Files older than LOG_KEEP_DAYS total: delete even the zip (hard cap).
        // This means: 2 days of uncompressed logs (fast grep), 8 more days as zips.
        // Archive dir is created automatically.
        static constexpr int LOG_ARCHIVE_AFTER_DAYS = 2;

        // Ensure archive directory exists
        const std::string archive_dir = log_dir_ + "/archive";
        {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(fs::path(archive_dir), ec);
        }

        WIN32_FIND_DATAA fd{};
        std::string pattern = log_dir_ + "/omega_*.log";
        HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        // Collect all matching filenames
        std::vector<std::string> files;
        do {
            // Skip today's file — never archive the live log
            const std::string fname = log_dir_ + "/" + fd.cFileName;
            if (fname != current_path_)
                files.push_back(fname);
        } while (FindNextFileA(h, &fd));
        FindClose(h);

        // Sort ascending -- oldest first
        std::sort(files.begin(), files.end());

        // Archive files older than LOG_ARCHIVE_AFTER_DAYS using PowerShell Compress-Archive
        // Files beyond LOG_KEEP_DAYS total are hard-deleted (including their zips).
        const int total = static_cast<int>(files.size());
        for (int i = 0; i < total; ++i) {
            const std::string& fpath = files[i];
            // Extract date from filename: omega_YYYY-MM-DD.log
            const std::string fname  = fpath.substr(fpath.find_last_of("/\\") + 1);
            const std::string zip_path = archive_dir + "/" + fname.substr(0, fname.size() - 4) + ".zip";

            const bool should_archive = (total - i) > LOG_ARCHIVE_AFTER_DAYS;
            const bool should_delete  = (total - i) > LOG_KEEP_DAYS;

            if (should_delete) {
                // Hard cap exceeded — delete log and zip
                DeleteFileA(fpath.c_str());
                DeleteFileA(zip_path.c_str());
                continue;
            }

            if (should_archive) {
                // Check if zip already exists — don't re-compress
                WIN32_FIND_DATAA zfd{};
                const HANDLE zh = FindFirstFileA(zip_path.c_str(), &zfd);
                const bool zip_exists = (zh != INVALID_HANDLE_VALUE);
                if (zh != INVALID_HANDLE_VALUE) FindClose(zh);

                if (!zip_exists) {
                    // Use PowerShell Compress-Archive (available Windows 5+)
                    // Run hidden, fire-and-forget — don't block the tick loop
                    const std::string cmd = "powershell -WindowStyle Hidden -Command "
                        "\"Compress-Archive -Path '" + fpath + "' "
                        "-DestinationPath '" + zip_path + "' -Force\" & exit";
                    // Build the zip check path as a named std::string BEFORE the lambda.
                    // The old code built a temporary string expression inside the lambda
                    // and immediately called .c_str() on it -- the temporary was destroyed
                    // before FindFirstFileA read the pointer (dangling pointer UB).
                    // Capturing the fully-formed std::string by value guarantees lifetime.
                    const std::string zip_check_path =
                        (fpath.substr(0, fpath.rfind('/') + 1) + "archive/" +
                         fpath.substr(fpath.rfind('/') + 1, fpath.rfind('.') - fpath.rfind('/') - 1) + ".zip");
                    std::thread([cmd, fpath, zip_check_path]() {
                        // Small delay so Omega doesn't hammer disk on startup
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        std::system(cmd.c_str());
                        // Delete original after successful zip
                        WIN32_FIND_DATAA cfd{};
                        const HANDLE ch = FindFirstFileA(zip_check_path.c_str(), &cfd);
                        if (ch != INVALID_HANDLE_VALUE) {
                            FindClose(ch);
                            DeleteFileA(fpath.c_str());
                        }
                    }).detach();
                } else {
                    // Zip already exists — safe to delete original
                    DeleteFileA(fpath.c_str());
                }
            }
        }
    }
};

static std::string utc_date_for_ts(int64_t ts);

class RollingCsvLogger {
public:
    static constexpr int LOG_KEEP_DAYS = 10;

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
static std::unique_ptr<RollingCsvLogger> g_daily_trade_open_log;   // entry-time rolling log

// FIX recv buffer -- owned by extract_messages() as a static local

// ?????????????????????????????????????????????????????????????????????????????
// Helpers
// ?????????????????????????????????????????????????????????????????????????????

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
    // Always absolute path -- exe runs from C:\Omega\build\Release
    // Relative "logs" fallback silently lost all logs to build dir. Removed.
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string abs = "C:\\Omega\\logs";
    fs::create_directories(fs::path(abs), ec);
    if (ec) {
        fprintf(stderr, "[OMEGA-FATAL] Cannot create log dir %s: %s\n",
                abs.c_str(), ec.message().c_str());
        fflush(stderr);
    }
    return abs;  // always absolute path -- no relative fallback
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


// ?????????????????????????????????????????????????????????????????????????????
// Tick value normalisation -- converts raw price-point PnL to USD equivalent.
// BlackBull CFD lot sizes (standard 1-lot). Adjust if trading mini/micro lots.
// Used by: daily_loss_limit check, shadow PnL accumulation, GUI display.
// ?????????????????????????????????????????????????????????????????????????????
#include "sizing.hpp"
#include "fix_builders.hpp"
// Does nothing in SHADOW mode. Returns clOrdId on success, empty on failure/shadow.
#include "order_exec.hpp"
// ?????????????????????????????????????????????????????????????????????????????
#include "logging.hpp"
// ?????????????????????????????????????????????????????????????????????????????
#include "engine_config.hpp"
#include "config.hpp"
#include "trade_lifecycle.hpp"

