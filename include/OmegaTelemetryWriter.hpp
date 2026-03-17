#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>

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
    double audusd_bid; double audusd_ask;
    double nzdusd_bid; double nzdusd_ask;
    double usdjpy_bid; double usdjpy_ask;
    double brent_bid;  double brent_ask;

    // --- P&L ---
    double daily_pnl;       // net P&L after slippage (what you actually made)
    double gross_daily_pnl; // gross P&L before slippage (for transparency display)
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

    // --- Last signal ---
    char last_signal_symbol[16];
    char last_signal_side[8];   // "LONG" / "SHORT" / "NONE"
    double last_signal_price;
    char last_signal_reason[64];

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

    // --- Uptime ---
    int64_t uptime_sec;    // seconds since process start — written each tick by main loop
    int64_t start_time;    // unix timestamp of process start — set once at init
};

// ==============================================================================
// OmegaTelemetryWriter — writes to the shared memory snapshot
// ==============================================================================
class OmegaTelemetryWriter
{
private:
    HANDLE              m_map;
    OmegaTelemetrySnapshot* m_snap;

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
            strcpy_s(m_snap->last_signal_side, "NONE");
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
        else if (!strcmp(sym,"GER30"))   { lv_ger30_bid=bid;  lv_ger30_ask=ask;  m_snap->ger30_bid=bid;  m_snap->ger30_ask=ask; }
        else if (!strcmp(sym,"UK100"))   { lv_uk100_bid=bid;  lv_uk100_ask=ask;  m_snap->uk100_bid=bid;  m_snap->uk100_ask=ask; }
        else if (!strcmp(sym,"ESTX50"))  { lv_estx50_bid=bid; lv_estx50_ask=ask; m_snap->estx50_bid=bid; m_snap->estx50_ask=ask; }
        else if (!strcmp(sym,"XAGUSD"))  { lv_xag_bid=bid;    lv_xag_ask=ask;    m_snap->xag_bid=bid;    m_snap->xag_ask=ask; }
        else if (!strcmp(sym,"EURUSD"))  { lv_eurusd_bid=bid; lv_eurusd_ask=ask; m_snap->eurusd_bid=bid; m_snap->eurusd_ask=ask; }
        else if (!strcmp(sym,"AUDUSD"))  { lv_audusd_bid=bid; lv_audusd_ask=ask; m_snap->audusd_bid=bid; m_snap->audusd_ask=ask; }
        else if (!strcmp(sym,"NZDUSD"))  { lv_nzdusd_bid=bid; lv_nzdusd_ask=ask; m_snap->nzdusd_bid=bid; m_snap->nzdusd_ask=ask; }
        else if (!strcmp(sym,"USDJPY"))  { lv_usdjpy_bid=bid; lv_usdjpy_ask=ask; m_snap->usdjpy_bid=bid; m_snap->usdjpy_ask=ask; }
        else if (!strcmp(sym,"UKBRENT")) { lv_brent_bid=bid;  lv_brent_ask=ask;  m_snap->brent_bid=bid;  m_snap->brent_ask=ask; }
        m_snap->sequence.store(seq + 1, std::memory_order_release);
    }

    void UpdateStats(double pnl, double gross_pnl, double dd, int trades, int w, int l,
                     double wr, double avg_w, double avg_l, int orders, int fills)
    {
        if (!m_snap) return;
        m_snap->daily_pnl       = pnl;
        m_snap->gross_daily_pnl = gross_pnl;
        m_snap->max_drawdown    = dd;
        m_snap->total_trades    = trades;
        m_snap->wins            = w;
        m_snap->losses          = l;
        m_snap->win_rate        = wr;
        m_snap->avg_win         = avg_w;
        m_snap->avg_loss        = avg_l;
        m_snap->total_orders    = orders;
        m_snap->total_fills     = fills;
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
        }
    }

    void UpdateLastSignal(const char* sym, const char* side, double price, const char* reason)
    {
        if (!m_snap) return;
        strcpy_s(m_snap->last_signal_symbol, sym);
        strcpy_s(m_snap->last_signal_side, side);
        m_snap->last_signal_price = price;
        strcpy_s(m_snap->last_signal_reason, reason);
    }

    void UpdateMacroRegime(double vix, const char* regime, double es_nq_div)
    {
        if (!m_snap) return;
        m_snap->vix_level       = vix;
        m_snap->es_nq_divergence = es_nq_div;
        strcpy_s(m_snap->macro_regime, regime);
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

    void SetMode(const char* m) { if (m_snap) strcpy_s(m_snap->mode, m); }
};
