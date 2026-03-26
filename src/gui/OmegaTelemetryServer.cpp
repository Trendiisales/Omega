// ==============================================================================
// OmegaTelemetryServer.cpp
// HTTP :7779 (GUI + REST) + WebSocket :7780 (250ms push)
// All MSVC /W4 /WX issues resolved:
//   - WS frame bytes use static_cast<char> from uint8_t constants
//   - No truncation warnings
// ==============================================================================
#include "OmegaTelemetryServer.hpp"
#include "OmegaIndexHtml.hpp"   // HTML embedded at build time
#include "OmegaTradeLedger.hpp"
#include <unordered_map>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>

extern omega::OmegaTradeLedger g_omegaLedger;

namespace omega {

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

static std::string loadFile(const std::string& p)
{
    static const char* bases[] = {
        "", "C:\\Omega\\", "..\\", ".\\"
    };
    for (const char* base : bases) {
        std::string full = std::string(base) + p;
        std::ifstream f(full, std::ios::binary);
        if (f.is_open()) {
            std::ostringstream ss; ss << f.rdbuf();
            return ss.str();
        }
    }
    return "";
}

static std::string base64Encode(const unsigned char* data, size_t len)
{
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO* mem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, mem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr; BIO_get_mem_ptr(b64, &bptr);
    std::string r(bptr->data, bptr->length);
    BIO_free_all(b64);
    return r;
}

static std::string extractHeader(const std::string& req, const std::string& name)
{
    std::string lr = req, ln = name;
    std::transform(lr.begin(), lr.end(), lr.begin(), ::tolower);
    std::transform(ln.begin(), ln.end(), ln.begin(), ::tolower);
    size_t pos = lr.find(ln + ":");
    if (pos == std::string::npos) return "";
    pos += ln.size() + 1;
    while (pos < req.size() && (req[pos] == ' ' || req[pos] == '\t')) pos++;
    size_t end = req.find("\r\n", pos);
    if (end == std::string::npos) end = req.find('\n', pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON builders
// ─────────────────────────────────────────────────────────────────────────────

static std::string buildTelemetryJson(const OmegaTelemetrySnapshot* s)
{
    if (!s) return "{}";
    const int    trades = s->total_trades;
    const int    wins   = s->wins;
    const double wr     = (trades > 0) ? (100.0 * wins / trades) : 0.0;
    char buf[8192];
    snprintf(buf, sizeof(buf),
        "{"
        "\"sp_bid\":%.4f,\"sp_ask\":%.4f,"
        "\"nq_bid\":%.4f,\"nq_ask\":%.4f,"
        "\"cl_bid\":%.4f,\"cl_ask\":%.4f,"
        "\"vix_bid\":%.4f,\"vix_ask\":%.4f,"
        "\"dx_bid\":%.4f,\"dx_ask\":%.4f,"
        "\"dj_bid\":%.4f,\"dj_ask\":%.4f,"
        "\"nas_bid\":%.4f,\"nas_ask\":%.4f,"
        "\"gold_bid\":%.4f,\"gold_ask\":%.4f,"
        "\"ngas_bid\":%.4f,\"ngas_ask\":%.4f,"
        "\"es_bid\":%.4f,\"es_ask\":%.4f,"
        "\"dxcash_bid\":%.4f,\"dxcash_ask\":%.4f,"
        "\"ger30_bid\":%.4f,\"ger30_ask\":%.4f,"
        "\"uk100_bid\":%.4f,\"uk100_ask\":%.4f,"
        "\"estx50_bid\":%.4f,\"estx50_ask\":%.4f,"
        "\"xag_bid\":%.4f,\"xag_ask\":%.4f,"
        "\"eurusd_bid\":%.4f,\"eurusd_ask\":%.4f,"
        "\"gbpusd_bid\":%.4f,\"gbpusd_ask\":%.4f,"
        "\"audusd_bid\":%.4f,\"audusd_ask\":%.4f,"
        "\"nzdusd_bid\":%.4f,\"nzdusd_ask\":%.4f,"
        "\"usdjpy_bid\":%.4f,\"usdjpy_ask\":%.4f,"
        "\"brent_bid\":%.4f,\"brent_ask\":%.4f,"
        "\"daily_pnl\":%.2f,\"gross_daily_pnl\":%.2f,\"max_drawdown\":%.2f,"
        "\"closed_pnl\":%.2f,\"open_unrealised_pnl\":%.2f,"
        "\"fix_rtt_last\":%.2f,\"fix_rtt_p50\":%.2f,\"fix_rtt_p95\":%.2f,"
        "\"total_trades\":%d,\"wins\":%d,\"losses\":%d,\"win_rate\":%.1f,"
        "\"avg_win\":%.2f,\"avg_loss\":%.2f,"
        "\"total_orders\":%d,\"total_fills\":%d,"
        "\"fix_quote_status\":\"%s\",\"fix_trade_status\":\"%s\","
        "\"quote_msg_rate\":%d,\"sequence_gaps\":%d,"
        "\"mode\":\"%s\","
        "\"session_name\":\"%s\",\"session_tradeable\":%d,"
        "\"sp_phase\":%d,\"sp_comp_high\":%.4f,\"sp_comp_low\":%.4f,"
        "\"sp_recent_vol_pct\":%.4f,\"sp_baseline_vol_pct\":%.4f,\"sp_signals\":%d,"
        "\"nq_phase\":%d,\"nq_comp_high\":%.4f,\"nq_comp_low\":%.4f,"
        "\"nq_recent_vol_pct\":%.4f,\"nq_baseline_vol_pct\":%.4f,\"nq_signals\":%d,"
        "\"cl_phase\":%d,\"cl_comp_high\":%.4f,\"cl_comp_low\":%.4f,"
        "\"cl_recent_vol_pct\":%.4f,\"cl_baseline_vol_pct\":%.4f,\"cl_signals\":%d,"
        "\"vix_level\":%.2f,\"macro_regime\":\"%s\",\"es_nq_divergence\":%.6f,"
        "\"gov_spread\":%d,\"gov_latency\":%d,\"gov_pnl\":%d,"
        "\"gov_positions\":%d,\"gov_consec_loss\":%d,"
        "\"asia_fx_gate_open\":%d,"
        "\"cfg_max_trades_per_cycle\":%d,\"cfg_max_open_positions\":%d,"
        "\"sl_cooldown_count\":%d,"
        "\"ctrader_l2_live\":%d,\"gold_l2_real\":%d,"
        "\"xau_phase\":%d,\"xau_comp_high\":%.4f,\"xau_comp_low\":%.4f,"
        "\"xau_recent_vol_pct\":%.4f,\"xau_baseline_vol_pct\":%.4f,\"xau_signals\":%d,"
        "\"brent_phase\":%d,\"brent_comp_high\":%.4f,\"brent_comp_low\":%.4f,"
        "\"brent_recent_vol_pct\":%.4f,\"brent_baseline_vol_pct\":%.4f,\"brent_signals\":%d,"
        "\"xag_phase\":%d,\"xag_comp_high\":%.4f,\"xag_comp_low\":%.4f,"
        "\"xag_recent_vol_pct\":%.4f,\"xag_baseline_vol_pct\":%.4f,\"xag_signals\":%d,"
        "\"eurusd_phase\":%d,\"eurusd_comp_high\":%.4f,\"eurusd_comp_low\":%.4f,"
        "\"eurusd_recent_vol_pct\":%.4f,\"eurusd_baseline_vol_pct\":%.4f,\"eurusd_signals\":%d,"
        "\"gbpusd_phase\":%d,\"gbpusd_comp_high\":%.4f,\"gbpusd_comp_low\":%.4f,"
        "\"gbpusd_recent_vol_pct\":%.4f,\"gbpusd_baseline_vol_pct\":%.4f,\"gbpusd_signals\":%d,"
        "\"audusd_phase\":%d,\"audusd_comp_high\":%.4f,\"audusd_comp_low\":%.4f,"
        "\"audusd_recent_vol_pct\":%.4f,\"audusd_baseline_vol_pct\":%.4f,\"audusd_signals\":%d,"
        "\"nzdusd_phase\":%d,\"nzdusd_comp_high\":%.4f,\"nzdusd_comp_low\":%.4f,"
        "\"nzdusd_recent_vol_pct\":%.4f,\"nzdusd_baseline_vol_pct\":%.4f,\"nzdusd_signals\":%d,"
        "\"usdjpy_phase\":%d,\"usdjpy_comp_high\":%.4f,\"usdjpy_comp_low\":%.4f,"
        "\"usdjpy_recent_vol_pct\":%.4f,\"usdjpy_baseline_vol_pct\":%.4f,\"usdjpy_signals\":%d,"
        "\"build_version\":\"%s\",\"build_time\":\"%s\","
        "\"uptime_sec\":%lld",
        s->sp_bid,     s->sp_ask,     s->nq_bid,  s->nq_ask,
        s->cl_bid,     s->cl_ask,     s->vix_bid, s->vix_ask,
        s->dx_bid,     s->dx_ask,     s->dj_bid,  s->dj_ask,
        s->nas_bid,    s->nas_ask,    s->gold_bid, s->gold_ask,
        s->ngas_bid,   s->ngas_ask,   s->es_bid,  s->es_ask,
        s->dxcash_bid, s->dxcash_ask,
        s->ger30_bid,  s->ger30_ask,  s->uk100_bid, s->uk100_ask,
        s->estx50_bid, s->estx50_ask, s->xag_bid,   s->xag_ask,
        s->eurusd_bid, s->eurusd_ask,
        s->gbpusd_bid, s->gbpusd_ask,
        s->audusd_bid, s->audusd_ask, s->nzdusd_bid, s->nzdusd_ask,
        s->usdjpy_bid, s->usdjpy_ask, s->brent_bid,  s->brent_ask,
        s->daily_pnl, s->gross_daily_pnl, s->max_drawdown,
        s->closed_pnl, s->open_unrealised_pnl,
        s->fix_rtt_last, s->fix_rtt_p50, s->fix_rtt_p95,
        trades, wins, s->losses, wr,
        s->avg_win, s->avg_loss,
        s->total_orders, s->total_fills,
        s->fix_quote_status, s->fix_trade_status,
        s->quote_msg_rate, s->sequence_gaps,
        s->mode,
        s->session_name, s->session_tradeable,
        s->sp_phase, s->sp_comp_high, s->sp_comp_low,
        s->sp_recent_vol_pct, s->sp_baseline_vol_pct, s->sp_signals,
        s->nq_phase, s->nq_comp_high, s->nq_comp_low,
        s->nq_recent_vol_pct, s->nq_baseline_vol_pct, s->nq_signals,
        s->cl_phase, s->cl_comp_high, s->cl_comp_low,
        s->cl_recent_vol_pct, s->cl_baseline_vol_pct, s->cl_signals,
        s->vix_level, s->macro_regime, s->es_nq_divergence,
        s->gov_spread, s->gov_latency, s->gov_pnl,
        s->gov_positions, s->gov_consec_loss,
        s->asia_fx_gate_open,
        s->cfg_max_trades_per_cycle, s->cfg_max_open_positions,
        s->sl_cooldown_count,
        s->ctrader_l2_live, s->gold_l2_real,
        s->xau_phase, s->xau_comp_high, s->xau_comp_low,
        s->xau_recent_vol_pct, s->xau_baseline_vol_pct, s->xau_signals,
        s->brent_phase, s->brent_comp_high, s->brent_comp_low,
        s->brent_recent_vol_pct, s->brent_baseline_vol_pct, s->brent_signals,
        s->xag_phase, s->xag_comp_high, s->xag_comp_low,
        s->xag_recent_vol_pct, s->xag_baseline_vol_pct, s->xag_signals,
        s->eurusd_phase, s->eurusd_comp_high, s->eurusd_comp_low,
        s->eurusd_recent_vol_pct, s->eurusd_baseline_vol_pct, s->eurusd_signals,
        s->gbpusd_phase, s->gbpusd_comp_high, s->gbpusd_comp_low,
        s->gbpusd_recent_vol_pct, s->gbpusd_baseline_vol_pct, s->gbpusd_signals,
        s->audusd_phase, s->audusd_comp_high, s->audusd_comp_low,
        s->audusd_recent_vol_pct, s->audusd_baseline_vol_pct, s->audusd_signals,
        s->nzdusd_phase, s->nzdusd_comp_high, s->nzdusd_comp_low,
        s->nzdusd_recent_vol_pct, s->nzdusd_baseline_vol_pct, s->nzdusd_signals,
        s->usdjpy_phase, s->usdjpy_comp_high, s->usdjpy_comp_low,
        s->usdjpy_recent_vol_pct, s->usdjpy_baseline_vol_pct, s->usdjpy_signals,
        s->build_version, s->build_time,
        (long long)s->uptime_sec
    );

    // Append signal_history ring buffer as JSON array (newest first)
    std::string result(buf);
    result += ",\"signal_history\":[";
    const int count = s->sig_count;
    const int head  = s->sig_head;  // next-write index, so newest = head-1 (wraps)
    for (int i = 0; i < count; ++i) {
        // Walk backwards from head-1
        const int idx = (head - 1 - i + OmegaTelemetrySnapshot::MAX_SIGNAL_HISTORY) % OmegaTelemetrySnapshot::MAX_SIGNAL_HISTORY;
        if (i > 0) result += ',';
        char entry[384];
        snprintf(entry, sizeof(entry),
            "{\"symbol\":\"%s\",\"side\":\"%s\",\"price\":%.4f,\"tp\":%.4f,\"sl\":%.4f,\"reason\":\"%s\","
            "\"sup_regime\":\"%s\",\"macro\":\"%s\",\"engine\":\"%s\"}",
            s->sig_symbol[idx], s->sig_side[idx],
            s->sig_price[idx],  s->sig_tp[idx],  s->sig_sl[idx],
            s->sig_reason[idx],
            s->sig_sup_regime[idx], s->sig_macro[idx], s->sig_engine[idx]);
        result += entry;
    }
    result += "]";  // close signal_history array — outer object closed after brackets below

    // Append SL cooldown array
    result += ",\"sl_cooldowns\":[";
    for (int i = 0; i < s->sl_cooldown_count; ++i) {
        if (i > 0) result += ',';
        char cd[64];
        snprintf(cd, sizeof(cd),
            "{\"symbol\":\"%s\",\"secs_remaining\":%d}",
            s->sl_cooldown_symbols[i], s->sl_cooldown_secs_remaining[i]);
        result += cd;
    }
    result += "]";

    // Append per-symbol bracket state
    result += ",\"brackets\":{";
    auto bktJson = [&](const char* key, const OmegaTelemetrySnapshot::BracketState& b) {
        char tmp[96];
        snprintf(tmp, sizeof(tmp),
            "\"%s\":{\"phase\":%d,\"hi\":%.4f,\"lo\":%.4f}",
            key, b.phase, b.hi, b.lo);
        result += tmp;
    };
    bktJson("sp",   s->bkt_sp);   result += ',';
    bktJson("nq",   s->bkt_nq);   result += ',';
    bktJson("us30", s->bkt_us30); result += ',';
    bktJson("nas",  s->bkt_nas);  result += ',';
    bktJson("ger",  s->bkt_ger);  result += ',';
    bktJson("uk",   s->bkt_uk);   result += ',';
    bktJson("estx", s->bkt_estx); result += ',';
    bktJson("xag",  s->bkt_xag);  result += ',';
    bktJson("gold", s->bkt_gold); result += ',';
    bktJson("eur",  s->bkt_eur);  result += ',';
    bktJson("gbp",  s->bkt_gbp);  result += ',';
    bktJson("brent",s->bkt_brent);
    result += "}";  // close brackets{} only — root {} closed by final "}" after ca_engines

    // Append cross-asset engine live state array
    result += ",\"ca_engines\":[";
    const int nca = s->ca_engine_count;
    for (int i = 0; i < nca; ++i) {
        const auto& e = s->ca_engines[i];
        if (i > 0) result += ',';
        char ce[256];
        snprintf(ce, sizeof(ce),
            "{\"name\":\"%s\",\"symbol\":\"%s\","
            "\"active\":%d,\"is_long\":%d,"
            "\"entry\":%.4f,\"tp\":%.4f,\"sl\":%.4f,"
            "\"ref_price\":%.4f,\"signals_today\":%d,\"cost_blocked\":%d}",
            e.name, e.symbol,
            e.active, e.is_long,
            e.entry, e.tp, e.sl,
            e.ref_price, e.signals_today, e.cost_blocked);
        result += ce;
    }
    result += "]";

    // Append cost guard session totals
    {
        char cg[96];
        snprintf(cg, sizeof(cg),
            ",\"cost_guard_blocked\":%lld,\"cost_guard_passed\":%lld",
            (long long)s->cost_guard_blocked_total,
            (long long)s->cost_guard_passed_total);
        result += cg;
    }

    // Per-engine session P&L attribution
    {
        char ep[512];
        snprintf(ep, sizeof(ep),
            ",\"eng_pnl\":{\"breakout\":%.2f,\"bracket\":%.2f,\"gold_stack\":%.2f,\"gold_flow\":%.2f,\"cross\":%.2f,\"latency\":%.2f}"
            ",\"eng_trades\":{\"breakout\":%d,\"bracket\":%d,\"gold_stack\":%d,\"gold_flow\":%d,\"cross\":%d,\"latency\":%d}",
            s->eng_pnl_breakout,   s->eng_pnl_bracket,
            s->eng_pnl_gold_stack, s->eng_pnl_gold_flow,
            s->eng_pnl_cross,      s->eng_pnl_latency,
            s->eng_trades_breakout,   s->eng_trades_bracket,
            s->eng_trades_gold_stack, s->eng_trades_gold_flow,
            s->eng_trades_cross,      s->eng_trades_latency);
        result += ep;
    }

    // L2 imbalance per symbol
    {
        char l2[512];
        snprintf(l2, sizeof(l2),
            ",\"l2_sp\":%.3f,\"l2_nq\":%.3f,\"l2_dj\":%.3f,\"l2_nas\":%.3f"
            ",\"l2_cl\":%.3f,\"l2_brent\":%.3f,\"l2_gold\":%.3f,\"l2_xag\":%.3f"
            ",\"l2_ger\":%.3f,\"l2_uk\":%.3f,\"l2_estx\":%.3f"
            ",\"l2_eur\":%.3f,\"l2_gbp\":%.3f,\"l2_aud\":%.3f"
            ",\"l2_nzd\":%.3f,\"l2_jpy\":%.3f,\"l2_active\":%d",
            s->l2_sp, s->l2_nq, s->l2_dj, s->l2_nas,
            s->l2_cl, s->l2_brent, s->l2_gold, s->l2_xag,
            s->l2_ger, s->l2_uk, s->l2_estx,
            s->l2_eur, s->l2_gbp, s->l2_aud,
            s->l2_nzd, s->l2_jpy, s->l2_active);
        result += l2;

        // L2 book depth levels for panel display
        auto appendBook = [&](const char* sym,
                               const OmegaTelemetrySnapshot::L2Level* bids, int nb,
                               const OmegaTelemetrySnapshot::L2Level* asks, int na) {
            char buf[1024]; int pos = 0;
            pos += snprintf(buf+pos, sizeof(buf)-pos, ",\"%s_bids\":[", sym);
            for (int i = 0; i < nb && i < OmegaTelemetrySnapshot::L2_DEPTH; ++i)
                pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"p\":%.2f,\"s\":%.2f}", i?",":"", bids[i].price, bids[i].size);
            pos += snprintf(buf+pos, sizeof(buf)-pos, "],\"%s_asks\":[", sym);
            for (int i = 0; i < na && i < OmegaTelemetrySnapshot::L2_DEPTH; ++i)
                pos += snprintf(buf+pos, sizeof(buf)-pos, "%s{\"p\":%.2f,\"s\":%.2f}", i?",":"", asks[i].price, asks[i].size);
            pos += snprintf(buf+pos, sizeof(buf)-pos, "]");
            result += buf;
        };
        appendBook("gold",  s->l2_book_gold_bid, s->l2_book_gold_bids, s->l2_book_gold_ask, s->l2_book_gold_asks);
        appendBook("sp",    s->l2_book_sp_bid,   s->l2_book_sp_bids,   s->l2_book_sp_ask,   s->l2_book_sp_asks);
        appendBook("eur",   s->l2_book_eur_bid,  s->l2_book_eur_bids,  s->l2_book_eur_ask,  s->l2_book_eur_asks);
        appendBook("xag",   s->l2_book_xag_bid,  s->l2_book_xag_bids,  s->l2_book_xag_ask,  s->l2_book_xag_asks);
    }

    // Real-time cluster dollar exposure
    {
        char ex[256];
        snprintf(ex, sizeof(ex),
            ",\"exposure_us_equity\":%.2f,\"exposure_eu_equity\":%.2f"
            ",\"exposure_oil\":%.2f,\"exposure_metals\":%.2f"
            ",\"exposure_jpy_risk\":%.2f,\"exposure_eur_gbp\":%.2f"
            ",\"exposure_total\":%.2f",
            s->exposure_us_equity, s->exposure_eu_equity,
            s->exposure_oil,       s->exposure_metals,
            s->exposure_jpy_risk,  s->exposure_eur_gbp,
            s->exposure_total);
        result += ex;
    }

    // Multi-day throttle state
    {
        char md[128];
        snprintf(md, sizeof(md),
            ",\"multiday_consec_loss_days\":%d,\"multiday_scale\":%.2f,\"multiday_throttle_active\":%d",
            s->multiday_consec_loss_days,
            s->multiday_scale,
            s->multiday_throttle_active);
        result += md;
    }

    // open_positions array — drives green "LIVE ●" highlight in engine cells.
    // Derived from snapshot phase fields (IN_TRADE=3 for breakout, LIVE=3 for bracket,
    // active=1 for cross-asset). No extra snapshot fields needed.
    {
        result += ",\"open_positions\":[";
        bool first_pos = true;
        auto addPos = [&](const char* sym) {
            if (!first_pos) result += ',';
            result += "{\"symbol\":\"";
            result += sym;
            result += "\"}";
            first_pos = false;
        };
        // Breakout engines: phase==3 means IN_TRADE
        if (s->sp_phase    == 3) addPos("US500.F");
        if (s->nq_phase    == 3) addPos("USTEC.F");
        if (s->cl_phase    == 3) addPos("USOIL.F");
        if (s->xau_phase   == 3) addPos("GOLD.F");
        if (s->brent_phase == 3) addPos("BRENT");
        if (s->xag_phase   == 3) addPos("XAGUSD");
        if (s->eurusd_phase  == 3) addPos("EURUSD");
        if (s->gbpusd_phase  == 3) addPos("GBPUSD");
        if (s->audusd_phase  == 3) addPos("AUDUSD");
        if (s->nzdusd_phase  == 3) addPos("NZDUSD");
        if (s->usdjpy_phase  == 3) addPos("USDJPY");
        // Bracket engines: bkt phase==3 means LIVE
        if (s->bkt_sp.phase   == 3) addPos("US500.F");
        if (s->bkt_nq.phase   == 3) addPos("USTEC.F");
        if (s->bkt_us30.phase == 3) addPos("DJ30.F");
        if (s->bkt_nas.phase  == 3) addPos("NAS100");
        if (s->bkt_ger.phase  == 3) addPos("GER40");
        if (s->bkt_uk.phase   == 3) addPos("UK100");
        if (s->bkt_estx.phase == 3) addPos("ESTX50");
        if (s->bkt_xag.phase  == 3) addPos("XAGUSD");
        if (s->bkt_gold.phase == 3) addPos("GOLD.F");
        if (s->bkt_eur.phase  == 3) addPos("EURUSD");
        if (s->bkt_gbp.phase  == 3) addPos("GBPUSD");
        if (s->bkt_brent.phase== 3) addPos("BRENT");
        // Cross-asset engines: active==1
        for (int i = 0; i < s->ca_engine_count; ++i) {
            if (s->ca_engines[i].active) addPos(s->ca_engines[i].symbol);
        }
        result += "]";
    }

    // live_trades array -- per-trade real-time P&L, updated every 250ms
    {
        result += ",\"live_trades\":[";
        bool first_lt = true;
        for (int i = 0; i < s->live_trade_count; ++i) {
            const auto& lt = s->live_trades[i];
            if (!first_lt) result += ',';
            first_lt = false;
            char row[512];
            const double dist_to_sl = lt.is_long
                ? (lt.current - lt.sl) : (lt.sl - lt.current);
            const double dist_to_tp = (lt.tp > 0.0)
                ? (lt.is_long ? (lt.tp - lt.current) : (lt.current - lt.tp)) : 0.0;
            const int64_t held_sec = static_cast<int64_t>(std::time(nullptr)) - lt.entry_ts;
            snprintf(row, sizeof(row),
                "{\"symbol\":\"%s\",\"engine\":\"%s\","
                "\"side\":\"%s\","
                "\"entry\":%.4f,\"current\":%.4f,"
                "\"tp\":%.4f,\"sl\":%.4f,"
                "\"size\":%.4f,\"live_pnl\":%.2f,"
                "\"tick_value\":%.1f,"
                "\"held_sec\":%lld,"
                "\"dist_sl\":%.2f,\"dist_tp\":%.2f}",
                lt.symbol, lt.engine,
                lt.side,
                lt.entry, lt.current,
                lt.tp, lt.sl,
                lt.size, lt.live_pnl,
                lt.tick_value,
                (long long)held_sec,
                dist_to_sl, dist_to_tp);
            result += row;
        }
        result += "]";
    }

    result += "}";  // close root object
    return result;
}

static std::string buildTradesJson()
{
    const auto trades = g_omegaLedger.snapshot();
    std::string out;
    out.reserve(4096);
    out += '[';
    bool first = true;
    for (auto it = trades.crbegin(); it != trades.crend(); ++it) {
        const TradeRecord& t = *it;
        if (!first) out += ',';
        first = false;
        char row[768];
        snprintf(row, sizeof(row),
            "{\"id\":%d,\"symbol\":\"%s\",\"side\":\"%s\","
            "\"price\":%.4f,\"exitPrice\":%.4f,\"tp\":%.4f,\"sl\":%.4f,\"size\":%.4f,"
            "\"pnl\":%.2f,\"net_pnl\":%.2f,\"slippage_entry\":%.2f,\"slippage_exit\":%.2f,"
            "\"mfe\":%.2f,\"mae\":%.2f,"
            "\"entryTs\":%lld,\"exitTs\":%lld,"
            "\"exitReason\":\"%s\",\"engine\":\"%s\",\"regime\":\"%s\","
            "\"bracket_hi\":%.4f,\"bracket_lo\":%.4f}",
            t.id, t.symbol.c_str(), t.side.c_str(),
            t.entryPrice, t.exitPrice, t.tp, t.sl, t.size,
            t.pnl, t.net_pnl, t.slippage_entry, t.slippage_exit,
            t.mfe, t.mae,
            static_cast<long long>(t.entryTs), static_cast<long long>(t.exitTs),
            t.exitReason.c_str(), t.engine.c_str(), t.regime.c_str(),
            t.bracket_hi, t.bracket_lo);
        out += row;
    }
    out += ']';
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildHistoryJson — reads omega_trade_closes.csv from disk and returns JSON.
// Survives restarts. Shows all trades from today's file (daily-rotating log).
// Used by /api/history endpoint so the dashboard always has complete session data.
// Falls back to in-memory snapshot when CSV is unavailable.
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildHistoryJson()
{
    // Determine path: same logic as main.cpp log_root_dir() + /trades/omega_trade_closes.csv
    // Try common locations in order
    static const char* PATHS[] = {
        "logs/trades/omega_trade_closes.csv",
        "C:\\Omega\\logs\\trades\\omega_trade_closes.csv",
        "C:\\Omega\\build\\Release\\logs\\trades\\omega_trade_closes.csv",
        "../logs/trades/omega_trade_closes.csv",
        "..\\..\\logs\\trades\\omega_trade_closes.csv",
    };

    FILE* f = nullptr;
    for (auto p : PATHS) {
        f = fopen(p, "r");
        if (f) break;
    }

    if (!f) {
        // Fall back to in-memory snapshot
        return buildTradesJson();
    }

    std::string out;
    out.reserve(65536);
    out += '[';
    bool first = true;

    char line[2048];
    bool header_skipped = false;
    while (fgets(line, sizeof(line), f)) {
        // Skip header row
        if (!header_skipped) {
            header_skipped = true;
            if (line[0] == 't' || line[0] == '"') continue; // starts with "trade_id" or "ts_unix"
        }

        // Parse CSV row — fields:
        // trade_id,trade_ref,entry_ts_unix,entry_ts_utc,entry_utc_weekday,
        // exit_ts_unix,exit_ts_utc,exit_utc_weekday,symbol,engine,side,
        // entry_px,exit_px,tp,sl,size,gross_pnl,net_pnl,
        // slippage_entry,slippage_exit,commission,
        // slip_entry_pct,slip_exit_pct,comm_per_side,
        // mfe,mae,hold_sec,spread_at_entry,
        // latency_ms,regime,exit_reason
        int  trade_id = 0;
        char trade_ref[64]={}, entry_ts_utc[32]={}, entry_wd[16]={};
        char exit_ts_utc[32]={}, exit_wd[16]={};
        char symbol[32]={}, engine[64]={}, side[8]={};
        double entry_px=0, exit_px=0, tp=0, sl=0, size=0;
        double gross_pnl=0, net_pnl=0, slip_e=0, slip_x=0, comm=0;
        double slip_epct=0, slip_xpct=0, comm_side=0;
        double mfe=0, mae=0, hold_sec=0, spread=0, lat=0;
        char regime[32]={}, exit_reason[64]={};
        long long entry_ts=0, exit_ts=0;

        // Remove trailing newline
        const size_t len = strlen(line);
        if (len > 0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[len-1]='\0';
        if (len > 1 && (line[len-2]=='\r')) line[len-2]='\0';

        // Simple CSV tokeniser (handles quoted fields)
        char* fields[32]; int nf = 0;
        char buf[2048]; strncpy(buf, line, sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        char* p2 = buf;
        while (nf < 31 && *p2) {
            if (*p2 == '"') {
                ++p2;
                fields[nf++] = p2;
                while (*p2 && *p2 != '"') ++p2;
                if (*p2) { *p2++='\0'; }
                if (*p2==',') ++p2;
            } else {
                fields[nf++] = p2;
                while (*p2 && *p2 != ',') ++p2;
                if (*p2) { *p2++='\0'; }
            }
        }
        if (nf < 20) continue;

        trade_id  = atoi(fields[0]);
        strncpy(trade_ref, fields[1], sizeof(trade_ref)-1);
        entry_ts  = atoll(fields[2]);
        strncpy(entry_ts_utc, fields[3], sizeof(entry_ts_utc)-1);
        strncpy(entry_wd,     fields[4], sizeof(entry_wd)-1);
        exit_ts   = atoll(fields[5]);
        strncpy(exit_ts_utc,  fields[6], sizeof(exit_ts_utc)-1);
        strncpy(exit_wd,      fields[7], sizeof(exit_wd)-1);
        strncpy(symbol,  fields[8],  sizeof(symbol)-1);
        strncpy(engine,  fields[9],  sizeof(engine)-1);
        strncpy(side,    fields[10], sizeof(side)-1);
        entry_px  = atof(fields[11]); exit_px = atof(fields[12]);
        tp        = atof(fields[13]); sl      = atof(fields[14]);
        size      = atof(fields[15]);
        gross_pnl = atof(fields[16]); net_pnl = atof(fields[17]);
        slip_e    = atof(fields[18]); slip_x  = atof(fields[19]);
        comm      = nf>20 ? atof(fields[20]) : 0;
        slip_epct = nf>21 ? atof(fields[21]) : 0;
        slip_xpct = nf>22 ? atof(fields[22]) : 0;
        comm_side = nf>23 ? atof(fields[23]) : 0;
        mfe       = nf>24 ? atof(fields[24]) : 0;
        mae       = nf>25 ? atof(fields[25]) : 0;
        hold_sec  = nf>26 ? atof(fields[26]) : 0;
        spread    = nf>27 ? atof(fields[27]) : 0;
        lat       = nf>28 ? atof(fields[28]) : 0;
        if (nf>29) strncpy(regime,      fields[29], sizeof(regime)-1);
        if (nf>30) strncpy(exit_reason, fields[30], sizeof(exit_reason)-1);

        if (trade_id == 0 && entry_ts == 0) continue;  // skip malformed rows

        if (!first) out += ',';
        first = false;
        char row[1024];
        snprintf(row, sizeof(row),
            "{\"id\":%d,\"trade_ref\":\"%s\","
            "\"symbol\":\"%s\",\"side\":\"%s\",\"engine\":\"%s\","
            "\"price\":%.4f,\"exitPrice\":%.4f,\"tp\":%.4f,\"sl\":%.4f,\"size\":%.4f,"
            "\"pnl\":%.2f,\"net_pnl\":%.2f,"
            "\"slippage_entry\":%.2f,\"slippage_exit\":%.2f,\"commission\":%.2f,"
            "\"mfe\":%.2f,\"mae\":%.2f,\"hold_sec\":%.0f,"
            "\"spread_at_entry\":%.5f,\"latency_ms\":%.1f,"
            "\"entryTs\":%lld,\"exitTs\":%lld,"
            "\"entry_ts_utc\":\"%s\",\"exit_ts_utc\":\"%s\","
            "\"regime\":\"%s\",\"exitReason\":\"%s\"}",
            trade_id, trade_ref,
            symbol, side, engine,
            entry_px, exit_px, tp, sl, size,
            gross_pnl, net_pnl,
            slip_e, slip_x, comm,
            mfe, mae, hold_sec,
            spread, lat,
            entry_ts, exit_ts,
            entry_ts_utc, exit_ts_utc,
            regime, exit_reason);
        out += row;
    }
    fclose(f);
    out += ']';
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// buildDailySummaryJson — aggregates today's trades from CSV into a summary.
// Returns net P&L, trade count, win rate, by-engine breakdown.
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildDailySummaryJson()
{
    // Get today's UTC date prefix for filtering
    time_t now_t = time(nullptr);
    struct tm ti{}; gmtime_s(&ti, &now_t);
    char today[16];
    snprintf(today, sizeof(today), "%04d-%02d-%02d",
             ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);

    // Try today's daily rotating file first
    char daily_path[256];
    static const char* DAILY_DIRS[] = {
        "logs/trades",
        "C:\\Omega\\logs\\trades",
        "C:\\Omega\\build\\Release\\logs\\trades",
        "../logs/trades",
        "..\\..\\logs\\trades",
    };

    FILE* f = nullptr;
    for (auto d : DAILY_DIRS) {
        snprintf(daily_path, sizeof(daily_path), "%s/omega_trade_closes_%s.csv", d, today);
        f = fopen(daily_path, "r");
        if (f) break;
    }

    // Fallback: full cumulative CSV
    if (!f) {
        static const char* FULL_PATHS[] = {
            "logs/trades/omega_trade_closes.csv",
            "C:\\Omega\\logs\\trades\\omega_trade_closes.csv",
            "C:\\Omega\\build\\Release\\logs\\trades\\omega_trade_closes.csv",
            "../logs/trades/omega_trade_closes.csv",
        };
        for (auto p : FULL_PATHS) { f = fopen(p, "r"); if (f) break; }
    }

    // If still no file, use in-memory ledger summary
    if (!f) {
        const auto trades = g_omegaLedger.snapshot();
        int n=0, wins=0; double total=0, gross=0;
        struct EngStat { int n=0; int w=0; double pnl=0; };
        std::unordered_map<std::string,EngStat> by_eng;
        for (auto& t : trades) {
            ++n; total += t.net_pnl; gross += t.pnl;
            if (t.net_pnl > 0) ++wins;
            auto& e = by_eng[t.engine]; ++e.n; e.pnl += t.net_pnl;
            if (t.net_pnl > 0) ++e.w;
        }
        char out[2048];
        snprintf(out, sizeof(out),
            "{\"date\":\"%s\",\"source\":\"memory\","
            "\"total_trades\":%d,\"wins\":%d,\"losses\":%d,"
            "\"win_rate\":%.1f,\"net_pnl\":%.2f,\"gross_pnl\":%.2f}",
            today, n, wins, n-wins,
            n>0 ? wins*100.0/n : 0.0, total, gross);
        return out;
    }

    int n=0, wins=0; double total=0, gross=0;
    // Simple map for engine breakdown
    struct EngStat { int n=0; int w=0; double pnl=0; };
    std::unordered_map<std::string,EngStat> by_eng;

    char line[2048];
    bool header_skipped = false;
    while (fgets(line, sizeof(line), f)) {
        if (!header_skipped) { header_skipped=true; continue; }
        // Tokenise quickly
        char buf[2048]; strncpy(buf,line,sizeof(buf)-1);
        char* fields[32]; int nf=0; char* p2=buf;
        while(nf<31&&*p2){
            if(*p2=='"'){++p2;fields[nf++]=p2;while(*p2&&*p2!='"')++p2;if(*p2){*p2++='\0';}if(*p2==',')++p2;}
            else{fields[nf++]=p2;while(*p2&&*p2!=',')++p2;if(*p2){*p2++='\0';}}
        }
        if(nf<20) continue;
        const double net = atof(fields[17]);
        const double grs = atof(fields[16]);
        char eng[64]={}; strncpy(eng,fields[9],sizeof(eng)-1);
        ++n; total+=net; gross+=grs;
        if(net>0) ++wins;
        auto& e=by_eng[eng]; ++e.n; e.pnl+=net; if(net>0)++e.w;
    }
    fclose(f);

    std::string out;
    out.reserve(1024);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "{\"date\":\"%s\",\"source\":\"csv\","
        "\"total_trades\":%d,\"wins\":%d,\"losses\":%d,"
        "\"win_rate\":%.1f,\"net_pnl\":%.2f,\"gross_pnl\":%.2f,"
        "\"by_engine\":[",
        today, n, wins, n-wins,
        n>0?wins*100.0/n:0.0, total, gross);
    out += hdr;
    bool first=true;
    for(auto& kv : by_eng) {
        if(!first) out+=','; first=false;
        char eng_row[256];
        snprintf(eng_row, sizeof(eng_row),
            "{\"engine\":\"%s\",\"n\":%d,\"wins\":%d,\"net_pnl\":%.2f}",
            kv.first.c_str(), kv.second.n, kv.second.w, kv.second.pnl);
        out+=eng_row;
    }
    out+="]}";
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmegaTelemetryServer ctor / dtor / start / stop
// ─────────────────────────────────────────────────────────────────────────────

OmegaTelemetryServer::OmegaTelemetryServer()
    : running_(false)
    , server_fd_(INVALID_SOCKET)
    , ws_fd_(INVALID_SOCKET)
    , ws_port_(7780)
    , hMap_(NULL)
    , snap_(nullptr)
{}

OmegaTelemetryServer::~OmegaTelemetryServer() { stop(); }

void OmegaTelemetryServer::start(int http_port, int ws_port, OmegaTelemetrySnapshot* snap)
{
    if (running_.load()) return;
    ws_port_ = ws_port;

    if (snap) {
        // Direct pointer — same process, no shared memory roundtrip needed
        snap_ = snap;
        std::cout << "[OmegaHTTP] Direct snapshot pointer OK\n";
    } else {
        hMap_ = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\OmegaTelemetrySharedMemory");
        if (hMap_)
            snap_ = static_cast<OmegaTelemetrySnapshot*>(
                        MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, sizeof(OmegaTelemetrySnapshot)));
        if (!snap_)
            std::cerr << "[OmegaHTTP] WARNING: snapshot null — no data will show\n";
    }
    running_.store(true);
    thread_    = std::thread(&OmegaTelemetryServer::run,             this, http_port);
    ws_thread_ = std::thread(&OmegaTelemetryServer::wsBroadcastLoop, this);
}

void OmegaTelemetryServer::stop()
{
    if (!running_.load()) return;
    running_.store(false);
    if (server_fd_ != INVALID_SOCKET) closesocket(server_fd_);
    if (ws_fd_     != INVALID_SOCKET) closesocket(ws_fd_);
    if (thread_.joinable())    thread_.join();
    if (ws_thread_.joinable()) ws_thread_.join();
    if (snap_ && hMap_) UnmapViewOfFile(snap_);  // only unmap if we opened it
    if (hMap_) CloseHandle(hMap_);
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket helpers
// ─────────────────────────────────────────────────────────────────────────────

std::string OmegaTelemetryServer::wsHandshakeResponse(const std::string& request)
{
    const std::string key = extractHeader(request, "Sec-WebSocket-Key");
    if (key.empty()) return "";
    const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha1[20];
    SHA1(reinterpret_cast<const unsigned char*>(magic.c_str()), magic.size(), sha1);
    const std::string accept = base64Encode(sha1, 20);
    return "HTTP/1.1 101 Switching Protocols\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
}

std::string OmegaTelemetryServer::wsBuildFrame(const std::string& payload)
{
    // Use uint8_t constants to avoid MSVC C4309 truncation warning,
    // then narrow to char via static_cast for std::string::push_back.
    std::string frame;
    frame.reserve(payload.size() + 10);

    // FIN=1, opcode=1 (text) => 0x81
    frame.push_back(static_cast<char>(static_cast<uint8_t>(0x81u)));

    const size_t len = payload.size();
    if (len < 126u) {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(len)));
    } else if (len < 65536u) {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(126u)));
        frame.push_back(static_cast<char>(static_cast<uint8_t>((len >> 8u) & 0xFFu)));
        frame.push_back(static_cast<char>(static_cast<uint8_t>( len        & 0xFFu)));
    } else {
        frame.push_back(static_cast<char>(static_cast<uint8_t>(127u)));
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<char>(static_cast<uint8_t>((len >> (8u * static_cast<unsigned>(i))) & 0xFFu)));
    }
    frame += payload;
    return frame;
}

bool OmegaTelemetryServer::wsSendFrame(SOCKET s, const std::string& payload)
{
    const std::string frame = wsBuildFrame(payload);
    return send(s, frame.c_str(), static_cast<int>(frame.size()), 0) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// WebSocket broadcast loop — 250ms push
// ─────────────────────────────────────────────────────────────────────────────

void OmegaTelemetryServer::wsBroadcastLoop()
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    ws_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(ws_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // binds all interfaces — GUI accessible from browser
    addr.sin_port        = htons(static_cast<u_short>(ws_port_));

    if (bind(ws_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ws_fd_, 8) == SOCKET_ERROR) {
        std::cerr << "[OmegaWS] bind/listen failed\n"; return;
    }

    u_long nb = 1;
    ioctlsocket(ws_fd_, FIONBIO, &nb);

    std::cout << "[OmegaWS] WebSocket port " << ws_port_ << "\n" << std::flush;

    auto last_broadcast = std::chrono::steady_clock::now();

    while (running_.load()) {
        sockaddr_in ca{}; int cl = sizeof(ca);
        SOCKET c = accept(ws_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c != INVALID_SOCKET) {
            char buf[1024];
            const int n = recv(c, buf, static_cast<int>(sizeof(buf)) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "Upgrade: websocket") || strstr(buf, "Upgrade: WebSocket")) {
                    const std::string resp = wsHandshakeResponse(std::string(buf, static_cast<size_t>(n)));
                    if (!resp.empty()) {
                        send(c, resp.c_str(), static_cast<int>(resp.size()), 0);
                        std::lock_guard<std::mutex> lk(ws_mutex_);
                        ws_clients_.push_back(c);
                    } else { closesocket(c); }
                } else { closesocket(c); }
            } else { closesocket(c); }
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= 250) {
            last_broadcast = now;
            // Update uptime every broadcast — independent of FIX tick rate
            if (snap_ && snap_->start_time > 0)
                snap_->uptime_sec = static_cast<int64_t>(std::time(nullptr)) - snap_->start_time;
            std::lock_guard<std::mutex> lk(ws_mutex_);
            if (!ws_clients_.empty()) {
                const std::string payload = buildTelemetryJson(snap_);
                std::vector<SOCKET> alive;
                alive.reserve(ws_clients_.size());
                for (SOCKET s : ws_clients_) {
                    if (wsSendFrame(s, payload)) alive.push_back(s);
                    else closesocket(s);
                }
                ws_clients_ = std::move(alive);
            }
        }
        Sleep(10);
    }
    closesocket(ws_fd_);
    ws_fd_ = INVALID_SOCKET;
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP handler
// ─────────────────────────────────────────────────────────────────────────────

void OmegaTelemetryServer::run(int port)
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // binds all interfaces — GUI accessible from browser
    addr.sin_port        = htons(static_cast<u_short>(port));

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(server_fd_, 16) == SOCKET_ERROR) {
        std::cerr << "[OmegaHTTP] bind/listen failed\n"; return;
    }

    std::cout << "[OmegaHTTP] port " << port << "\n" << std::flush;

    while (running_.load()) {
        sockaddr_in ca{}; int cl = sizeof(ca);
        SOCKET c = accept(server_fd_, reinterpret_cast<sockaddr*>(&ca), &cl);
        if (c == INVALID_SOCKET) { if (!running_.load()) break; Sleep(5); continue; }

        char buf[2048];
        const int n = recv(c, buf, static_cast<int>(sizeof(buf)) - 1, 0);
        if (n <= 0) { closesocket(c); continue; }
        buf[n] = '\0';

        std::string body, ct = "text/html";
        int status = 200;

        if (strstr(buf, "GET /api/telemetry"))    { ct = "application/json"; body = buildTelemetryJson(snap_); }
        else if (strstr(buf, "GET /api/trades"))  { ct = "application/json"; body = buildTradesJson(); }
        else if (strstr(buf, "GET /api/history")) { ct = "application/json"; body = buildHistoryJson(); }
        else if (strstr(buf, "GET /api/daily"))   { ct = "application/json"; body = buildDailySummaryJson(); }
        else if (strstr(buf, "GET /version"))     {
            ct = "application/json";
            char vbuf[256];
            if (snap_) snprintf(vbuf, sizeof(vbuf),
                "{\"version\":\"%s\",\"built\":\"%s\"}",
                snap_->build_version, snap_->build_time);
            else snprintf(vbuf, sizeof(vbuf), "{\"version\":\"starting\",\"built\":\"unknown\"}");
            body = vbuf;
        }
        else if (strstr(buf, "GET /chimera_logo.png")) { ct = "image/png"; body = loadFile("chimera_logo.png"); }
        else if (strstr(buf, "GET / ") || strstr(buf, "GET /index.html")) {
            ct = "text/html";
            body = omega_gui::INDEX_HTML;
            if (body.empty()) { body = "<h1>Omega GUI not found</h1>"; status = 404; }
        }
        else { status = 404; body = "Not Found"; }

        char hdr[256];
        snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\nX-Content-Type-Options: nosniff\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\n"
            "Connection: close\r\n\r\n",
            status, ct.c_str(), body.size());
        send(c, hdr,        static_cast<int>(strlen(hdr)),     0);
        send(c, body.c_str(), static_cast<int>(body.size()),   0);
        closesocket(c);
    }
    closesocket(server_fd_);
    server_fd_ = INVALID_SOCKET;
}

} // namespace omega
