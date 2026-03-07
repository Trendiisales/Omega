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
    double mes_bid;
    double mes_ask;
    double mnq_bid;
    double mnq_ask;
    double mcl_bid;
    double mcl_ask;

    // --- Confirmation symbol prices ---
    double es_bid;  double es_ask;
    double nq_bid;  double nq_ask;
    double cl_bid;  double cl_ask;
    double vix_bid; double vix_ask;
    double dx_bid;  double dx_ask;
    double zn_bid;  double zn_ask;
    double ym_bid;  double ym_ask;
    double rty_bid; double rty_ask;

    // --- P&L ---
    double daily_pnl;
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
    char mode[8];   // "SHADOW" or "LIVE"

    // --- Session ---
    char session_name[32];
    int  session_tradeable;

    // --- Breakout engine state per primary symbol ---
    // MES
    int  mes_phase;            // 0=FLAT 1=COMPRESSION 2=BREAKOUT
    double mes_comp_high;
    double mes_comp_low;
    double mes_recent_vol_pct;
    double mes_baseline_vol_pct;
    int  mes_signals;

    // MNQ
    int  mnq_phase;
    double mnq_comp_high;
    double mnq_comp_low;
    double mnq_recent_vol_pct;
    double mnq_baseline_vol_pct;
    int  mnq_signals;

    // MCL
    int  mcl_phase;
    double mcl_comp_high;
    double mcl_comp_low;
    double mcl_recent_vol_pct;
    double mcl_baseline_vol_pct;
    int  mcl_signals;

    // --- Last signal ---
    char last_signal_symbol[8];
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
    double lv_mes_bid=0, lv_mes_ask=0;
    double lv_mnq_bid=0, lv_mnq_ask=0;
    double lv_mcl_bid=0, lv_mcl_ask=0;
    double lv_es_bid=0,  lv_es_ask=0;
    double lv_nq_bid=0,  lv_nq_ask=0;
    double lv_cl_bid=0,  lv_cl_ask=0;
    double lv_vix_bid=0, lv_vix_ask=0;
    double lv_dx_bid=0,  lv_dx_ask=0;
    double lv_zn_bid=0,  lv_zn_ask=0;
    double lv_ym_bid=0,  lv_ym_ask=0;
    double lv_rty_bid=0, lv_rty_ask=0;

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
        if      (!strcmp(sym,"MES"))  { lv_mes_bid=bid; lv_mes_ask=ask; m_snap->mes_bid=bid; m_snap->mes_ask=ask; }
        else if (!strcmp(sym,"MNQ"))  { lv_mnq_bid=bid; lv_mnq_ask=ask; m_snap->mnq_bid=bid; m_snap->mnq_ask=ask; }
        else if (!strcmp(sym,"MCL"))  { lv_mcl_bid=bid; lv_mcl_ask=ask; m_snap->mcl_bid=bid; m_snap->mcl_ask=ask; }
        else if (!strcmp(sym,"ES"))   { lv_es_bid=bid;  lv_es_ask=ask;  m_snap->es_bid=bid;  m_snap->es_ask=ask; }
        else if (!strcmp(sym,"NQ"))   { lv_nq_bid=bid;  lv_nq_ask=ask;  m_snap->nq_bid=bid;  m_snap->nq_ask=ask; }
        else if (!strcmp(sym,"CL"))   { lv_cl_bid=bid;  lv_cl_ask=ask;  m_snap->cl_bid=bid;  m_snap->cl_ask=ask; }
        else if (!strcmp(sym,"VIX"))  { lv_vix_bid=bid; lv_vix_ask=ask; m_snap->vix_bid=bid; m_snap->vix_ask=ask; }
        else if (!strcmp(sym,"DX"))   { lv_dx_bid=bid;  lv_dx_ask=ask;  m_snap->dx_bid=bid;  m_snap->dx_ask=ask; }
        else if (!strcmp(sym,"ZN"))   { lv_zn_bid=bid;  lv_zn_ask=ask;  m_snap->zn_bid=bid;  m_snap->zn_ask=ask; }
        else if (!strcmp(sym,"YM"))   { lv_ym_bid=bid;  lv_ym_ask=ask;  m_snap->ym_bid=bid;  m_snap->ym_ask=ask; }
        else if (!strcmp(sym,"RTY"))  { lv_rty_bid=bid; lv_rty_ask=ask; m_snap->rty_bid=bid; m_snap->rty_ask=ask; }
        m_snap->sequence.store(seq + 1, std::memory_order_release);
    }

    void UpdateStats(double pnl, double dd, int trades, int w, int l,
                     double wr, double avg_w, double avg_l, int orders, int fills)
    {
        if (!m_snap) return;
        m_snap->daily_pnl    = pnl;
        m_snap->max_drawdown = dd;
        m_snap->total_trades = trades;
        m_snap->wins         = w;
        m_snap->losses       = l;
        m_snap->win_rate     = wr;
        m_snap->avg_win      = avg_w;
        m_snap->avg_loss     = avg_l;
        m_snap->total_orders = orders;
        m_snap->total_fills  = fills;
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
        if (!strcmp(sym,"MES")) {
            m_snap->mes_phase=phase; m_snap->mes_comp_high=comp_high;
            m_snap->mes_comp_low=comp_low; m_snap->mes_recent_vol_pct=recent_vol_pct;
            m_snap->mes_baseline_vol_pct=baseline_vol_pct; m_snap->mes_signals=signals;
        } else if (!strcmp(sym,"MNQ")) {
            m_snap->mnq_phase=phase; m_snap->mnq_comp_high=comp_high;
            m_snap->mnq_comp_low=comp_low; m_snap->mnq_recent_vol_pct=recent_vol_pct;
            m_snap->mnq_baseline_vol_pct=baseline_vol_pct; m_snap->mnq_signals=signals;
        } else if (!strcmp(sym,"MCL")) {
            m_snap->mcl_phase=phase; m_snap->mcl_comp_high=comp_high;
            m_snap->mcl_comp_low=comp_low; m_snap->mcl_recent_vol_pct=recent_vol_pct;
            m_snap->mcl_baseline_vol_pct=baseline_vol_pct; m_snap->mcl_signals=signals;
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
