#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>

// ==============================================================================
// OmegaTelemetrySnapshot — shared memory block written by main loop,
// read by OmegaTelemetryServer (GUI). Uses a different named mapping
// ("Global\\OmegaTelemetrySharedMemory") so it does not conflict with
// ChimeraMetals which uses "Global\\ChimeraTelemetrySharedMemory".
// ==============================================================================

struct OmegaTelemetrySnapshot
{
    std::atomic<uint64_t> sequence;

    // --- Primary symbol prices (MES, MNQ, MCL) ---
    double sp_bid;
    double sp_ask;
    double nq_bid;
    double nq_ask;
    double cl_bid;
    double cl_ask;

    // --- Confirmation symbol prices ---
    double vix_bid;    double vix_ask;
    double dx_bid;     double dx_ask;
    double dj_bid;     double dj_ask;
    double nas_bid;    double nas_ask;
    double gold_bid;   double gold_ask;
    double ngas_bid;   double ngas_ask;
    double es_bid;     double es_ask;
    double dxcash_bid; double dxcash_ask;
    double ger30_bid;  double ger30_ask;
    double uk100_bid;  double uk100_ask;
    double estx50_bid; double estx50_ask;
    double xag_bid;    double xag_ask;
    double eurusd_bid; double eurusd_ask;
    double gbpusd_bid; double gbpusd_ask;
    double audusd_bid; double audusd_ask;
    double nzdusd_bid; double nzdusd_ask;
    double usdjpy_bid; double usdjpy_ask;
    double brent_bid;  double brent_ask;

    // --- P&L ---
    double daily_pnl;           // net P&L after slippage (closed + floating combined)
    double gross_daily_pnl;     // gross P&L before slippage
    double closed_pnl;          // closed trades only — no floating
    double open_unrealised_pnl; // floating P&L on open positions only
    double max_drawdown;

    // --- Latency ---
    double fix_rtt_last;
    double fix_rtt_p50;
    double fix_rtt_p95;

    // --- Trade stats ---
    int total_trades;
    int wins;
    int losses;
    double win_rate;
    double avg_win;
    double avg_loss;
    int total_orders;
    int total_fills;

    // --- FIX session ---
    char fix_quote_status[16];
    char fix_trade_status[16];
    int  quote_msg_rate;
    int  sequence_gaps;

    // --- Mode ---
    char mode[8];        // "SHADOW" or "LIVE"
    char build_version[32]; // git hash, e.g. "f60cebb"
    char build_time[48];    // build timestamp

    // --- Session ---
    char session_name[32];
    int  session_tradeable;

    // --- Breakout engine state per primary symbol ---
    // US500.F
    int  sp_phase;             // 0=FLAT 1=COMPRESSION 2=BREAKOUT
    double sp_comp_high;
    double sp_comp_low;
    double sp_recent_vol_pct;
    double sp_baseline_vol_pct;
    int  sp_signals;

    // USTEC.F
    int  nq_phase;
    double nq_comp_high;
    double nq_comp_low;
    double nq_recent_vol_pct;
    double nq_baseline_vol_pct;
    int  nq_signals;

    // USOIL.F
    int  cl_phase;
    double cl_comp_high;
    double cl_comp_low;
    double cl_recent_vol_pct;
    double cl_baseline_vol_pct;
    int  cl_signals;

    // GOLD.F
    int  xau_phase;
    double xau_comp_high;
    double xau_comp_low;
    double xau_recent_vol_pct;
    double xau_baseline_vol_pct;
    int  xau_signals;

    // --- Signal history ring buffer (5 most recent signals) ---
    static constexpr int MAX_SIGNAL_HISTORY = 50;

    // --- Per-symbol bracket engine state ---
    // phase: 0=IDLE, 1=ARMED, 2=PENDING, 3=LIVE, 4=COOLDOWN
    // hi/lo: locked bracket levels (0 when IDLE/COOLDOWN)
    struct BracketState { int phase=0; double hi=0; double lo=0; };
    BracketState bkt_sp;      // US500.F
    BracketState bkt_nq;      // USTEC.F
    BracketState bkt_us30;    // DJ30.F
    BracketState bkt_nas;     // NAS100
    BracketState bkt_ger;     // GER40
    BracketState bkt_uk;      // UK100
    BracketState bkt_estx;    // ESTX50
    BracketState bkt_xag;     // XAGUSD
    BracketState bkt_gold;    // GOLD.F
    BracketState bkt_eur;     // EURUSD
    BracketState bkt_gbp;     // GBPUSD
    BracketState bkt_brent;   // UKBRENT

    // UKBRENT engine state
    int    brent_phase           = 0;
    double brent_comp_high       = 0.0;
    double brent_comp_low        = 0.0;
    double brent_recent_vol_pct  = 0.0;
    double brent_baseline_vol_pct= 0.0;
    int    brent_signals         = 0;

    // XAGUSD breakout engine state
    int    xag_phase             = 0;
    double xag_comp_high         = 0.0;
    double xag_comp_low          = 0.0;
    double xag_recent_vol_pct    = 0.0;
    double xag_baseline_vol_pct  = 0.0;
    int    xag_signals           = 0;

    // EURUSD breakout engine state
    int    eurusd_phase          = 0;
    double eurusd_comp_high      = 0.0;
    double eurusd_comp_low       = 0.0;
    double eurusd_recent_vol_pct = 0.0;
    double eurusd_baseline_vol_pct = 0.0;
    int    eurusd_signals        = 0;

    // GBPUSD breakout engine state
    int    gbpusd_phase          = 0;
    double gbpusd_comp_high      = 0.0;
    double gbpusd_comp_low       = 0.0;
    double gbpusd_recent_vol_pct = 0.0;
    double gbpusd_baseline_vol_pct = 0.0;
    int    gbpusd_signals        = 0;

    // AUDUSD breakout engine state
    int    audusd_phase          = 0;
    double audusd_comp_high      = 0.0;
    double audusd_comp_low       = 0.0;
    double audusd_recent_vol_pct = 0.0;
    double audusd_baseline_vol_pct = 0.0;
    int    audusd_signals        = 0;

    // NZDUSD breakout engine state
    int    nzdusd_phase          = 0;
    double nzdusd_comp_high      = 0.0;
    double nzdusd_comp_low       = 0.0;
    double nzdusd_recent_vol_pct = 0.0;
    double nzdusd_baseline_vol_pct = 0.0;
    int    nzdusd_signals        = 0;

    // USDJPY breakout engine state
    int    usdjpy_phase          = 0;
    double usdjpy_comp_high      = 0.0;
    double usdjpy_comp_low       = 0.0;
    double usdjpy_recent_vol_pct = 0.0;
    double usdjpy_baseline_vol_pct = 0.0;
    int    usdjpy_signals        = 0;
    char   sig_symbol    [MAX_SIGNAL_HISTORY][16];
    char   sig_side      [MAX_SIGNAL_HISTORY][8];   // "LONG" / "SHORT"
    double sig_price     [MAX_SIGNAL_HISTORY];
    double sig_tp        [MAX_SIGNAL_HISTORY];      // take-profit price
    double sig_sl        [MAX_SIGNAL_HISTORY];      // stop-loss price
    char   sig_reason    [MAX_SIGNAL_HISTORY][64];
    char   sig_sup_regime[MAX_SIGNAL_HISTORY][32];  // supervisor regime e.g. "EXPANSION_BREAKOUT"
    char   sig_macro     [MAX_SIGNAL_HISTORY][16];  // macro regime e.g. "RISK_ON"
    char   sig_engine    [MAX_SIGNAL_HISTORY][16];  // engine type e.g. "BREAKOUT" / "BRACKET"
    int    sig_head;   // index of most recent signal (0-based, wraps)
    int    sig_count;  // how many valid entries (0–5)

    // --- Regime indicators ---
    double vix_level;
    char   macro_regime[32];    // "RISK_ON" / "RISK_OFF" / "NEUTRAL"
    double es_nq_divergence;    // ES vs NQ relative strength

    // --- Governor blocks ---
    int gov_spread;
    int gov_latency;
    int gov_pnl;
    int gov_positions;
    int gov_consec_loss;

    // --- SL cooldown state (per symbol) ---
    static constexpr int MAX_COOLDOWN_SYMBOLS = 16;
    char    sl_cooldown_symbols[MAX_COOLDOWN_SYMBOLS][16]; // symbols currently in SL cooldown
    int32_t sl_cooldown_secs_remaining[MAX_COOLDOWN_SYMBOLS]; // seconds left on each
    int     sl_cooldown_count;          // how many symbols are in cooldown right now

    // --- L2 data quality ---
    int     ctrader_l2_live;  // 1 = cTrader depth client received at least 1 event
    int     gold_l2_real;     // 1 = GOLD.F book has non-zero size data (vs FIX-only 0.5 fallback)

    // --- Asia FX gate ---
    int     asia_fx_gate_open;          // 1 = trading allowed (gate open), 0 = session-blocked

    // --- Config snapshot ---
    int     cfg_max_trades_per_cycle;   // current value of max_trades_per_cycle
    int     cfg_max_open_positions;     // current value of max_open_positions

    // --- Uptime ---
    int64_t uptime_sec;    // seconds since process start — written each tick by main loop
    int64_t start_time;    // unix timestamp of process start — set once at init

    // --- Cross-asset engine live state (Engines 1–8 + ORB instances) ---
    // One slot per named engine instance. Written each tick by main.cpp.
    // active=1 means position is currently open.
    static constexpr int MAX_CA_ENGINES = 20;
    struct CrossAssetEngineState {
        char   name[24];       // e.g. "ORB_US", "VWAP_GER40", "TREND_GOLD"
        char   symbol[12];     // e.g. "US500.F"
        int    active;         // 1 = position open, 0 = idle
        int    is_long;        // direction of open position (valid when active=1)
        double entry;          // entry price (valid when active=1)
        double tp;             // TP price
        double sl;             // SL price
        double ref_price;      // VWAP / EMA50 / ORB range mid — context reference
        int    cost_blocked;   // cumulative count of COST-GUARD blocks this session
        int    signals_today;  // signals fired today
    };
    CrossAssetEngineState ca_engines[MAX_CA_ENGINES];
    int ca_engine_count;   // how many slots are populated

    // ── Live open trades — per-trade real-time P&L ────────────────────────────
    // Updated every 250ms by the unrealised P&L push in main.cpp.
    // GUI uses this to show per-trade floating P&L in real time.
    static constexpr int MAX_LIVE_TRADES = 16;
    struct LiveTrade {
        char   symbol[12];    // "GOLD.F", "XAGUSD", etc.
        char   engine[24];    // "GoldFlow", "GoldStack/CompBreakout", etc.
        char   side[6];       // "LONG" or "SHORT"
        int    is_long;       // 1=LONG 0=SHORT — used for dist_to_sl/tp calc
        double entry;         // fill price
        double current;       // current bid (LONG) or ask (SHORT) for P&L calc
        double tp;            // take profit price
        double sl;            // stop loss price
        double size;          // lots
        double live_pnl;      // current floating P&L in USD (updated every 250ms)
        double tick_value;    // USD per point per lot (for display)
        int64_t entry_ts;     // epoch seconds of entry
    };
    LiveTrade live_trades[MAX_LIVE_TRADES];
    int live_trade_count = 0;

    // --- ExecutionCostGuard session stats ---
    int64_t cost_guard_blocked_total;
    int64_t cost_guard_passed_total;

    // L2 imbalance per symbol (0.0=ask-heavy 0.5=neutral 1.0=bid-heavy)
    double l2_sp, l2_nq, l2_dj, l2_nas, l2_cl, l2_brent;
    double l2_gold, l2_xag, l2_ger, l2_uk, l2_estx;
    double l2_eur, l2_gbp, l2_aud, l2_nzd, l2_jpy;
    int    l2_active;  // 1 = cTrader depth feed live

    // L2 book depth levels for panel display (top 5 bid/ask for key symbols)
    struct L2Level { double price; double size; };
    static constexpr int L2_DEPTH = 5;
    L2Level l2_book_gold_bid[L2_DEPTH]{}, l2_book_gold_ask[L2_DEPTH]{};
    L2Level l2_book_sp_bid[L2_DEPTH]{},   l2_book_sp_ask[L2_DEPTH]{};
    L2Level l2_book_eur_bid[L2_DEPTH]{},  l2_book_eur_ask[L2_DEPTH]{};
    L2Level l2_book_xag_bid[L2_DEPTH]{},  l2_book_xag_ask[L2_DEPTH]{};
    int l2_book_gold_bids = 0, l2_book_gold_asks = 0;
    int l2_book_sp_bids   = 0, l2_book_sp_asks   = 0;
    int l2_book_eur_bids  = 0, l2_book_eur_asks  = 0;
    int l2_book_xag_bids  = 0, l2_book_xag_asks  = 0;

    // --- Per-engine live session P&L (closed trades, USD) ---
    // Updated on each trade close from handle_closed_trade.
    // Allows GUI to show which engines are contributing vs dragging.
    double eng_pnl_breakout      = 0.0;  // all CRTP BreakoutEngine instances
    double eng_pnl_bracket       = 0.0;  // all BracketEngine instances
    double eng_pnl_gold_stack    = 0.0;  // GoldEngineStack (gold engines excl. MeanReversion)
    double eng_pnl_gold_flow     = 0.0;  // GoldFlowEngine
    double eng_pnl_cross         = 0.0;  // all CrossAsset engines (ORB/VWAP/TrendPB/etc)
    double eng_pnl_latency       = 0.0;  // LatencyEdgeStack
    double eng_pnl_mean_rev      = 0.0;  // MeanReversionEngine (GoldEngineStack)
    int    eng_trades_breakout   = 0;
    int    eng_trades_bracket    = 0;
    int    eng_trades_gold_stack = 0;
    int    eng_trades_gold_flow  = 0;
    int    eng_trades_cross      = 0;
    int    eng_trades_latency    = 0;
    int    eng_trades_mean_rev   = 0;

    // --- Real-time dollar exposure per correlation cluster ---
    // Each value = sum of (lot * tick_value_multiplier) for all OPEN positions in cluster.
    // Positive = net long exposure in USD notional, negative = net short.
    double exposure_us_equity;  // US500, USTEC, DJ30, NAS100
    double exposure_eu_equity;  // GER40, UK100, ESTX50
    double exposure_oil;        // USOIL.F, BRENT
    double exposure_metals;     // GOLD.F, XAGUSD
    double exposure_jpy_risk;   // USDJPY, AUDUSD, NZDUSD
    double exposure_eur_gbp;    // EURUSD, GBPUSD
    double exposure_total;      // sum of |cluster| across all clusters

    // --- Multi-day drawdown throttle state ---
    int    multiday_consec_loss_days;  // consecutive losing sessions
    double multiday_scale;             // current size multiplier (0.5 or 1.0)
    int    multiday_throttle_active;   // 1 = throttle currently firing
};

// ==============================================================================
// OmegaTelemetryWriter — writes to the shared memory snapshot
// ==============================================================================
class OmegaTelemetryWriter
{
private:
    HANDLE              m_map;
    OmegaTelemetrySnapshot* m_snap;
public:
    OmegaTelemetrySnapshot* snap() const { return m_snap; }
private:

    // Last valid prices (prevents zero-price bug)
    double lv_sp_bid=0,     lv_sp_ask=0;
    double lv_nq_bid=0,     lv_nq_ask=0;
    double lv_cl_bid=0,     lv_cl_ask=0;
    double lv_vix_bid=0,    lv_vix_ask=0;
    double lv_dx_bid=0,     lv_dx_ask=0;
    double lv_dj_bid=0,     lv_dj_ask=0;
    double lv_nas_bid=0,    lv_nas_ask=0;
    double lv_gold_bid=0,   lv_gold_ask=0;
    double lv_ngas_bid=0,   lv_ngas_ask=0;
    double lv_es_bid=0,     lv_es_ask=0;
    double lv_dxcash_bid=0, lv_dxcash_ask=0;
    double lv_ger30_bid=0,  lv_ger30_ask=0;
    double lv_uk100_bid=0,  lv_uk100_ask=0;
    double lv_estx50_bid=0, lv_estx50_ask=0;
    double lv_xag_bid=0,    lv_xag_ask=0;
    double lv_eurusd_bid=0, lv_eurusd_ask=0;
    double lv_gbpusd_bid=0, lv_gbpusd_ask=0;
    double lv_audusd_bid=0, lv_audusd_ask=0;
    double lv_nzdusd_bid=0, lv_nzdusd_ask=0;
    double lv_usdjpy_bid=0, lv_usdjpy_ask=0;
    double lv_brent_bid=0,  lv_brent_ask=0;

public:
    OmegaTelemetryWriter() : m_map(nullptr), m_snap(nullptr) {}

    ~OmegaTelemetryWriter()
    {
        if (m_snap) UnmapViewOfFile(m_snap);
        if (m_map)  CloseHandle(m_map);
    }

    bool Init()
    {
        m_map = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(OmegaTelemetrySnapshot),
            "Global\\OmegaTelemetrySharedMemory"
        );
        if (!m_map) return false;
        m_snap = (OmegaTelemetrySnapshot*)MapViewOfFile(
            m_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OmegaTelemetrySnapshot));
        if (m_snap) {
            memset(m_snap, 0, sizeof(OmegaTelemetrySnapshot));
            m_snap->sequence.store(0, std::memory_order_release);
            strcpy_s(m_snap->fix_quote_status, "DISCONNECTED");
            strcpy_s(m_snap->fix_trade_status, "DISCONNECTED");
            strcpy_s(m_snap->mode, "SHADOW");
            strcpy_s(m_snap->macro_regime, "NEUTRAL");
            m_snap->sig_head  = 0;
            m_snap->sig_count = 0;
            for (int i = 0; i < OmegaTelemetrySnapshot::MAX_SIGNAL_HISTORY; ++i) {
                m_snap->sig_symbol[i][0]     = '\0';
                strcpy_s(m_snap->sig_side[i], "NONE");
                m_snap->sig_price[i]         = 0.0;
                m_snap->sig_tp[i]            = 0.0;
                m_snap->sig_sl[i]            = 0.0;
                m_snap->sig_reason[i][0]     = '\0';
                m_snap->sig_sup_regime[i][0] = '\0';
                m_snap->sig_macro[i][0]      = '\0';
                m_snap->sig_engine[i][0]     = '\0';
            }
        }
        return m_snap != nullptr;
    }

    OmegaTelemetrySnapshot* snap() { return m_snap; }

    void UpdatePrice(const char* sym, double bid, double ask)
    {
        if (!m_snap || bid <= 0 || ask <= 0) return;
        uint64_t seq = m_snap->sequence.load(std::memory_order_relaxed);
        // Route to correct field
        if      (!strcmp(sym,"US500.F")) { lv_sp_bid=bid;     lv_sp_ask=ask;     m_snap->sp_bid=bid;     m_snap->sp_ask=ask; }
        else if (!strcmp(sym,"USTEC.F")) { lv_nq_bid=bid;     lv_nq_ask=ask;     m_snap->nq_bid=bid;     m_snap->nq_ask=ask; }
        else if (!strcmp(sym,"USOIL.F")) { lv_cl_bid=bid;     lv_cl_ask=ask;     m_snap->cl_bid=bid;     m_snap->cl_ask=ask; }
        else if (!strcmp(sym,"VIX.F"))   { lv_vix_bid=bid;    lv_vix_ask=ask;    m_snap->vix_bid=bid;    m_snap->vix_ask=ask; }
        else if (!strcmp(sym,"DX.F"))    { lv_dx_bid=bid;     lv_dx_ask=ask;     m_snap->dx_bid=bid;     m_snap->dx_ask=ask; }
        else if (!strcmp(sym,"DJ30.F"))  { lv_dj_bid=bid;     lv_dj_ask=ask;     m_snap->dj_bid=bid;     m_snap->dj_ask=ask; }
        else if (!strcmp(sym,"NAS100"))  { lv_nas_bid=bid;    lv_nas_ask=ask;    m_snap->nas_bid=bid;    m_snap->nas_ask=ask; }
        else if (!strcmp(sym,"GOLD.F"))  { lv_gold_bid=bid;   lv_gold_ask=ask;   m_snap->gold_bid=bid;   m_snap->gold_ask=ask; }
        else if (!strcmp(sym,"NGAS.F"))  { lv_ngas_bid=bid;   lv_ngas_ask=ask;   m_snap->ngas_bid=bid;   m_snap->ngas_ask=ask; }
        else if (!strcmp(sym,"ES"))      { lv_es_bid=bid;     lv_es_ask=ask;     m_snap->es_bid=bid;     m_snap->es_ask=ask; }
        else if (!strcmp(sym,"DX"))      { lv_dxcash_bid=bid; lv_dxcash_ask=ask; m_snap->dxcash_bid=bid; m_snap->dxcash_ask=ask; }
        else if (!strcmp(sym,"GER40"))   { lv_ger30_bid=bid;  lv_ger30_ask=ask;  m_snap->ger30_bid=bid;  m_snap->ger30_ask=ask; }
        else if (!strcmp(sym,"UK100"))   { lv_uk100_bid=bid;  lv_uk100_ask=ask;  m_snap->uk100_bid=bid;  m_snap->uk100_ask=ask; }
        else if (!strcmp(sym,"ESTX50"))  { lv_estx50_bid=bid; lv_estx50_ask=ask; m_snap->estx50_bid=bid; m_snap->estx50_ask=ask; }
        else if (!strcmp(sym,"XAGUSD"))  { lv_xag_bid=bid;    lv_xag_ask=ask;    m_snap->xag_bid=bid;    m_snap->xag_ask=ask; }
        else if (!strcmp(sym,"EURUSD"))  { lv_eurusd_bid=bid; lv_eurusd_ask=ask; m_snap->eurusd_bid=bid; m_snap->eurusd_ask=ask; }
        else if (!strcmp(sym,"GBPUSD"))  { lv_gbpusd_bid=bid; lv_gbpusd_ask=ask; m_snap->gbpusd_bid=bid; m_snap->gbpusd_ask=ask; }
        else if (!strcmp(sym,"AUDUSD"))  { lv_audusd_bid=bid; lv_audusd_ask=ask; m_snap->audusd_bid=bid; m_snap->audusd_ask=ask; }
        else if (!strcmp(sym,"NZDUSD"))  { lv_nzdusd_bid=bid; lv_nzdusd_ask=ask; m_snap->nzdusd_bid=bid; m_snap->nzdusd_ask=ask; }
        else if (!strcmp(sym,"USDJPY"))  { lv_usdjpy_bid=bid; lv_usdjpy_ask=ask; m_snap->usdjpy_bid=bid; m_snap->usdjpy_ask=ask; }
        else if (!strcmp(sym,"BRENT")) { lv_brent_bid=bid;  lv_brent_ask=ask;  m_snap->brent_bid=bid;  m_snap->brent_ask=ask; }
        m_snap->sequence.store(seq + 1, std::memory_order_release);
    }

    void UpdateStats(double pnl, double gross_pnl, double dd, int trades, int w, int l,
                     double wr, double avg_w, double avg_l, int orders, int fills,
                     double closed_pnl = 0.0, double open_unrealised = 0.0)
    {
        if (!m_snap) return;
        m_snap->daily_pnl           = pnl;
        m_snap->gross_daily_pnl     = gross_pnl;
        m_snap->closed_pnl          = closed_pnl;
        m_snap->open_unrealised_pnl = open_unrealised;
        m_snap->max_drawdown        = dd;
        m_snap->total_trades        = trades;
        m_snap->wins                = w;
        m_snap->losses              = l;
        m_snap->win_rate            = wr;
        m_snap->avg_win             = avg_w;
        m_snap->avg_loss            = avg_l;
        m_snap->total_orders        = orders;
        m_snap->total_fills         = fills;
    }

    void UpdateLatency(double last, double p50, double p95)
    {
        if (!m_snap) return;
        m_snap->fix_rtt_last = last;
        m_snap->fix_rtt_p50  = p50;
        m_snap->fix_rtt_p95  = p95;
    }

    void UpdateFixStatus(const char* q, const char* t, int qrate, int gaps)
    {
        if (!m_snap) return;
        strcpy_s(m_snap->fix_quote_status, q);
        strcpy_s(m_snap->fix_trade_status, t);
        m_snap->quote_msg_rate = qrate;
        m_snap->sequence_gaps  = gaps;
    }

    void UpdateBuildVersion(const char* version, const char* built) {
        strncpy_s(m_snap->build_version, version, 31);
        strncpy_s(m_snap->build_time,    built,   47);
    }
    void UpdateSession(const char* name, int tradeable)
    {
        if (!m_snap) return;
        strcpy_s(m_snap->session_name, name);
        m_snap->session_tradeable = tradeable;
    }

    void UpdateEngineState(const char* sym, int phase,
                           double comp_high, double comp_low,
                           double recent_vol_pct, double baseline_vol_pct, int signals)
    {
        if (!m_snap) return;
        if (!strcmp(sym,"US500.F")) {
            m_snap->sp_phase=phase; m_snap->sp_comp_high=comp_high;
            m_snap->sp_comp_low=comp_low; m_snap->sp_recent_vol_pct=recent_vol_pct;
            m_snap->sp_baseline_vol_pct=baseline_vol_pct; m_snap->sp_signals=signals;
        } else if (!strcmp(sym,"USTEC.F")) {
            m_snap->nq_phase=phase; m_snap->nq_comp_high=comp_high;
            m_snap->nq_comp_low=comp_low; m_snap->nq_recent_vol_pct=recent_vol_pct;
            m_snap->nq_baseline_vol_pct=baseline_vol_pct; m_snap->nq_signals=signals;
        } else if (!strcmp(sym,"USOIL.F")) {
            m_snap->cl_phase=phase; m_snap->cl_comp_high=comp_high;
            m_snap->cl_comp_low=comp_low; m_snap->cl_recent_vol_pct=recent_vol_pct;
            m_snap->cl_baseline_vol_pct=baseline_vol_pct; m_snap->cl_signals=signals;
        } else if (!strcmp(sym,"GOLD.F")) {
            m_snap->xau_phase=phase; m_snap->xau_comp_high=comp_high;
            m_snap->xau_comp_low=comp_low; m_snap->xau_recent_vol_pct=recent_vol_pct;
            m_snap->xau_baseline_vol_pct=baseline_vol_pct; m_snap->xau_signals=signals;
        } else if (!strcmp(sym,"BRENT")) {
            m_snap->brent_phase=phase; m_snap->brent_comp_high=comp_high;
            m_snap->brent_comp_low=comp_low; m_snap->brent_recent_vol_pct=recent_vol_pct;
            m_snap->brent_baseline_vol_pct=baseline_vol_pct; m_snap->brent_signals=signals;
        } else if (!strcmp(sym,"XAGUSD")) {
            m_snap->xag_phase=phase; m_snap->xag_comp_high=comp_high;
            m_snap->xag_comp_low=comp_low; m_snap->xag_recent_vol_pct=recent_vol_pct;
            m_snap->xag_baseline_vol_pct=baseline_vol_pct; m_snap->xag_signals=signals;
        } else if (!strcmp(sym,"EURUSD")) {
            m_snap->eurusd_phase=phase; m_snap->eurusd_comp_high=comp_high;
            m_snap->eurusd_comp_low=comp_low; m_snap->eurusd_recent_vol_pct=recent_vol_pct;
            m_snap->eurusd_baseline_vol_pct=baseline_vol_pct; m_snap->eurusd_signals=signals;
        } else if (!strcmp(sym,"GBPUSD")) {
            m_snap->gbpusd_phase=phase; m_snap->gbpusd_comp_high=comp_high;
            m_snap->gbpusd_comp_low=comp_low; m_snap->gbpusd_recent_vol_pct=recent_vol_pct;
            m_snap->gbpusd_baseline_vol_pct=baseline_vol_pct; m_snap->gbpusd_signals=signals;
        } else if (!strcmp(sym,"AUDUSD")) {
            m_snap->audusd_phase=phase; m_snap->audusd_comp_high=comp_high;
            m_snap->audusd_comp_low=comp_low; m_snap->audusd_recent_vol_pct=recent_vol_pct;
            m_snap->audusd_baseline_vol_pct=baseline_vol_pct; m_snap->audusd_signals=signals;
        } else if (!strcmp(sym,"NZDUSD")) {
            m_snap->nzdusd_phase=phase; m_snap->nzdusd_comp_high=comp_high;
            m_snap->nzdusd_comp_low=comp_low; m_snap->nzdusd_recent_vol_pct=recent_vol_pct;
            m_snap->nzdusd_baseline_vol_pct=baseline_vol_pct; m_snap->nzdusd_signals=signals;
        } else if (!strcmp(sym,"USDJPY")) {
            m_snap->usdjpy_phase=phase; m_snap->usdjpy_comp_high=comp_high;
            m_snap->usdjpy_comp_low=comp_low; m_snap->usdjpy_recent_vol_pct=recent_vol_pct;
            m_snap->usdjpy_baseline_vol_pct=baseline_vol_pct; m_snap->usdjpy_signals=signals;
        }
    }

    // Update the live open trades array — called every 250ms by the unrealised push.
    // Clears and rebuilds from scratch each call so stale entries never persist.
    void ClearLiveTrades() {
        if (!m_snap) return;
        m_snap->live_trade_count = 0;
    }
    void AddLiveTrade(const char* symbol, const char* engine, const char* side,
                      double entry, double current, double tp, double sl,
                      double size, double live_pnl, double tick_value, int64_t entry_ts)
    {
        if (!m_snap) return;
        int idx = m_snap->live_trade_count;
        if (idx >= OmegaTelemetrySnapshot::MAX_LIVE_TRADES) return;
        auto& lt = m_snap->live_trades[idx];
        strncpy_s(lt.symbol,  symbol,  11);
        strncpy_s(lt.engine,  engine,  23);
        strncpy_s(lt.side,    side,     5);
        lt.is_long    = (side[0] == 'L') ? 1 : 0;
        lt.entry      = entry;
        lt.current    = current;
        lt.tp         = tp;
        lt.sl         = sl;
        lt.size       = size;
        lt.live_pnl   = live_pnl;
        lt.tick_value = tick_value;
        lt.entry_ts   = entry_ts;
        ++m_snap->live_trade_count;
    }

    void UpdateBracketState(const char* sym, int phase, double hi, double lo)
    {
        if (!m_snap) return;
        auto set = [&](OmegaTelemetrySnapshot::BracketState& b){
            b.phase = phase; b.hi = hi; b.lo = lo;
        };
        if      (!strcmp(sym,"US500.F")) set(m_snap->bkt_sp);
        else if (!strcmp(sym,"USTEC.F")) set(m_snap->bkt_nq);
        else if (!strcmp(sym,"DJ30.F"))  set(m_snap->bkt_us30);
        else if (!strcmp(sym,"NAS100"))  set(m_snap->bkt_nas);
        else if (!strcmp(sym,"GER40"))   set(m_snap->bkt_ger);
        else if (!strcmp(sym,"UK100"))   set(m_snap->bkt_uk);
        else if (!strcmp(sym,"ESTX50"))  set(m_snap->bkt_estx);
        else if (!strcmp(sym,"XAGUSD"))  set(m_snap->bkt_xag);
        else if (!strcmp(sym,"GOLD.F"))  set(m_snap->bkt_gold);
        else if (!strcmp(sym,"EURUSD"))  set(m_snap->bkt_eur);
        else if (!strcmp(sym,"GBPUSD"))  set(m_snap->bkt_gbp);
        else if (!strcmp(sym,"BRENT")) set(m_snap->bkt_brent);
    }

    void UpdateLastSignal(const char* sym, const char* side, double price, const char* reason,
                          const char* sup_regime = "", const char* macro_regime = "", const char* engine_type = "BREAKOUT",
                          double tp = 0.0, double sl = 0.0)
    {
        if (!m_snap) return;
        // Push new signal into ring buffer (newest at index sig_head)
        const int idx = m_snap->sig_head;
        strcpy_s(m_snap->sig_symbol[idx],     sym);
        strcpy_s(m_snap->sig_side[idx],       side);
        m_snap->sig_price[idx]              = price;
        m_snap->sig_tp[idx]                 = tp;
        m_snap->sig_sl[idx]                 = sl;
        strcpy_s(m_snap->sig_reason[idx],     reason);
        strcpy_s(m_snap->sig_sup_regime[idx], sup_regime    ? sup_regime    : "");
        strcpy_s(m_snap->sig_macro[idx],      macro_regime  ? macro_regime  : "");
        strcpy_s(m_snap->sig_engine[idx],     engine_type   ? engine_type   : "BREAKOUT");
        m_snap->sig_head  = (idx + 1) % OmegaTelemetrySnapshot::MAX_SIGNAL_HISTORY;
        if (m_snap->sig_count < OmegaTelemetrySnapshot::MAX_SIGNAL_HISTORY)
            ++m_snap->sig_count;
    }

    void UpdateMacroRegime(double vix, const char* regime, double es_nq_div)
    {
        if (!m_snap) return;
        m_snap->vix_level       = vix;
        m_snap->es_nq_divergence = es_nq_div;
        strcpy_s(m_snap->macro_regime, regime);
    }

    void UpdateL2(double sp, double nq, double dj, double nas,
                  double cl, double brent, double gold, double xag,
                  double ger, double uk, double estx,
                  double eur, double gbp, double aud, double nzd, double jpy,
                  int active)
    {
        if (!m_snap) return;
        m_snap->l2_sp=sp; m_snap->l2_nq=nq; m_snap->l2_dj=dj; m_snap->l2_nas=nas;
        m_snap->l2_cl=cl; m_snap->l2_brent=brent; m_snap->l2_gold=gold; m_snap->l2_xag=xag;
        m_snap->l2_ger=ger; m_snap->l2_uk=uk; m_snap->l2_estx=estx;
        m_snap->l2_eur=eur; m_snap->l2_gbp=gbp; m_snap->l2_aud=aud;
        m_snap->l2_nzd=nzd; m_snap->l2_jpy=jpy;
        m_snap->l2_active=active;
    }

    // Update L2 book depth levels for a symbol (called from main.cpp with real L2Book data)
    // Takes raw arrays to avoid including OmegaFIX.hpp in this header.
    void UpdateL2Book(const char* sym,
                      const double* bid_prices, const double* bid_sizes, int nb,
                      const double* ask_prices, const double* ask_sizes, int na) {
        if (!m_snap) return;
        auto copy = [&](OmegaTelemetrySnapshot::L2Level* bid_out, int& out_nb,
                        OmegaTelemetrySnapshot::L2Level* ask_out, int& out_na) {
            out_nb = nb < OmegaTelemetrySnapshot::L2_DEPTH ? nb : OmegaTelemetrySnapshot::L2_DEPTH;
            out_na = na < OmegaTelemetrySnapshot::L2_DEPTH ? na : OmegaTelemetrySnapshot::L2_DEPTH;
            for (int i = 0; i < out_nb; ++i) { bid_out[i].price = bid_prices[i]; bid_out[i].size = bid_sizes[i]; }
            for (int i = 0; i < out_na; ++i) { ask_out[i].price = ask_prices[i]; ask_out[i].size = ask_sizes[i]; }
        };
        const std::string s(sym);
        if      (s == "GOLD.F")  copy(m_snap->l2_book_gold_bid, m_snap->l2_book_gold_bids, m_snap->l2_book_gold_ask, m_snap->l2_book_gold_asks);
        else if (s == "US500.F") copy(m_snap->l2_book_sp_bid,   m_snap->l2_book_sp_bids,   m_snap->l2_book_sp_ask,   m_snap->l2_book_sp_asks);
        else if (s == "EURUSD")  copy(m_snap->l2_book_eur_bid,  m_snap->l2_book_eur_bids,  m_snap->l2_book_eur_ask,  m_snap->l2_book_eur_asks);
        else if (s == "XAGUSD")  copy(m_snap->l2_book_xag_bid,  m_snap->l2_book_xag_bids,  m_snap->l2_book_xag_ask,  m_snap->l2_book_xag_asks);
    }

    // Accumulate per-engine session P&L from closed trades.
    // engine_type: "BREAKOUT", "BRACKET", "GOLD_STACK", "GOLD_FLOW", "CROSS", "LATENCY"
    void AccumEnginePnl(const char* engine_type, double net_pnl)
    {
        if (!m_snap) return;
        const auto classify = [&]() {
            if (!engine_type) return 0;
            if (strstr(engine_type, "BRACKET"))    return 1;
            if (strstr(engine_type, "MEAN_REV") || strstr(engine_type, "MeanReversion")
             || strstr(engine_type, "MEAN-REV"))   return 6;
            if (strstr(engine_type, "GOLD_STACK") || strstr(engine_type, "GOLD-STACK")
             || strstr(engine_type, "GoldStack")
             || strstr(engine_type, "IntradaySeasonality")
             || strstr(engine_type, "WickRejection")
             || strstr(engine_type, "DonchianBreakout")
             || strstr(engine_type, "NR3Breakout")
             || strstr(engine_type, "SpikeFade")
             || strstr(engine_type, "AsianRange"))  return 2;
            if (strstr(engine_type, "GOLD_FLOW")  || strstr(engine_type, "L2_FLOW"))  return 3;
            if (strstr(engine_type, "ORB")   || strstr(engine_type, "VWAP")
             || strstr(engine_type, "TREND") || strstr(engine_type, "CROSS")
             || strstr(engine_type, "CARRY") || strstr(engine_type, "EsNq")
             || strstr(engine_type, "OIL")   || strstr(engine_type, "BRENT_WTI")
             || strstr(engine_type, "FX_CASCADE") || strstr(engine_type, "LONDON_FIX")) return 4;
            if (strstr(engine_type, "LATENCY") || strstr(engine_type, "LEAD_LAG")
             || strstr(engine_type, "SPREAD_DISLOC"))  return 5;
            return 0;  // default: BREAKOUT
        };
        switch (classify()) {
            case 0: m_snap->eng_pnl_breakout   += net_pnl; ++m_snap->eng_trades_breakout;   break;
            case 1: m_snap->eng_pnl_bracket    += net_pnl; ++m_snap->eng_trades_bracket;    break;
            case 2: m_snap->eng_pnl_gold_stack += net_pnl; ++m_snap->eng_trades_gold_stack; break;
            case 3: m_snap->eng_pnl_gold_flow  += net_pnl; ++m_snap->eng_trades_gold_flow;  break;
            case 4: m_snap->eng_pnl_cross      += net_pnl; ++m_snap->eng_trades_cross;      break;
            case 5: m_snap->eng_pnl_latency    += net_pnl; ++m_snap->eng_trades_latency;    break;
            case 6: m_snap->eng_pnl_mean_rev   += net_pnl; ++m_snap->eng_trades_mean_rev;   break;
        }
    }

    // Update real-time cluster dollar exposure.
    // Each value is the net USD notional (lot * tick_value_mult * direction) for open positions.
    void UpdateExposure(double us_equity, double eu_equity, double oil,
                        double metals,   double jpy_risk,  double eur_gbp)
    {
        if (!m_snap) return;
        m_snap->exposure_us_equity = us_equity;
        m_snap->exposure_eu_equity = eu_equity;
        m_snap->exposure_oil       = oil;
        m_snap->exposure_metals    = metals;
        m_snap->exposure_jpy_risk  = jpy_risk;
        m_snap->exposure_eur_gbp   = eur_gbp;
        m_snap->exposure_total     = std::fabs(us_equity) + std::fabs(eu_equity)
                                   + std::fabs(oil)       + std::fabs(metals)
                                   + std::fabs(jpy_risk)  + std::fabs(eur_gbp);
    }

    // Update multi-day throttle state for GUI display.
    void UpdateMultiDayThrottle(int consec_loss_days, double scale, int active)
    {
        if (!m_snap) return;
        m_snap->multiday_consec_loss_days = consec_loss_days;
        m_snap->multiday_scale            = scale;
        m_snap->multiday_throttle_active  = active;
    }

    void UpdateGovernor(int spread, int lat, int pnl, int pos, int consec)
    {
        if (!m_snap) return;
        m_snap->gov_spread    = spread;
        m_snap->gov_latency   = lat;
        m_snap->gov_pnl       = pnl;
        m_snap->gov_positions = pos;
        m_snap->gov_consec_loss = consec;
    }

    // Update which symbols are in SL cooldown and how long remains
    void UpdateSLCooldown(const std::vector<std::pair<std::string,int>>& active_cooldowns)
    {
        if (!m_snap) return;
        const int n = std::min(static_cast<int>(active_cooldowns.size()),
                               OmegaTelemetrySnapshot::MAX_COOLDOWN_SYMBOLS);
        m_snap->sl_cooldown_count = n;
        for (int i = 0; i < n; ++i) {
            strncpy_s(m_snap->sl_cooldown_symbols[i], 16,
                      active_cooldowns[i].first.c_str(), _TRUNCATE);
            m_snap->sl_cooldown_secs_remaining[i] = active_cooldowns[i].second;
        }
        // Zero unused slots
        for (int i = n; i < OmegaTelemetrySnapshot::MAX_COOLDOWN_SYMBOLS; ++i) {
            m_snap->sl_cooldown_symbols[i][0] = '\0';
            m_snap->sl_cooldown_secs_remaining[i] = 0;
        }
    }

    // Update Asia FX gate state and config snapshot
    void UpdateAsiaCfg(int asia_gate_open, int max_trades_per_cycle, int max_open_pos)
    {
        if (!m_snap) return;
        m_snap->asia_fx_gate_open       = asia_gate_open;
        m_snap->cfg_max_trades_per_cycle = max_trades_per_cycle;
        m_snap->cfg_max_open_positions   = max_open_pos;
    }

    void SetMode(const char* m) { if (m_snap) strcpy_s(m_snap->mode, m); }

    // Update a single cross-asset engine slot by name (upsert by name match)
    void UpdateCrossAsset(const char* name, const char* symbol,
                          int active, int is_long,
                          double entry, double tp, double sl, double ref_price,
                          int signals_today)
    {
        if (!m_snap) return;
        // Find existing slot or claim a new one
        int slot = -1;
        for (int i = 0; i < m_snap->ca_engine_count; ++i) {
            if (strncmp(m_snap->ca_engines[i].name, name, 23) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (m_snap->ca_engine_count >= OmegaTelemetrySnapshot::MAX_CA_ENGINES) return;
            slot = m_snap->ca_engine_count++;
        }
        auto& e = m_snap->ca_engines[slot];
        strncpy_s(e.name,   name,   23);
        strncpy_s(e.symbol, symbol, 11);
        e.active       = active;
        e.is_long      = is_long;
        e.entry        = entry;
        e.tp           = tp;
        e.sl           = sl;
        e.ref_price    = ref_price;
        e.signals_today = signals_today;
        // cost_blocked is incremented separately via IncrCostBlocked
    }

    void IncrCostBlocked() {
        if (!m_snap) return;
        ++m_snap->cost_guard_blocked_total;
    }
    void IncrCostPassed() {
        if (!m_snap) return;
        ++m_snap->cost_guard_passed_total;
    }
};
