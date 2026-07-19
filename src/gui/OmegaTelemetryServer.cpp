// ==============================================================================
// OmegaTelemetryServer.cpp
// HTTP :7779 (GUI + REST) + WebSocket :7780 (250ms push)
// All MSVC /W4 /WX issues resolved:
//   - WS frame bytes use static_cast<char> from uint8_t constants
//   - No truncation warnings
// ==============================================================================
#include "OmegaTelemetryServer.hpp"
#include "OmegaIndexHtml.hpp"   // HTML embedded at build time
#include "OmegaIndexHtmlLegacy.hpp"  // pre-2026-06-12 GUI, served at /legacy
#include "OmegaTradeLedger.hpp"
#include "OpenPositionRegistry.hpp"  // g_flatten_all_request (manual KILL-ALL panic flatten)
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

// ?????????????????????????????????????????????????????????????????????????????
// Utilities
// ?????????????????????????????????????????????????????????????????????????????

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

// S-2026-07-04: /api/companion torn-read guard (the REAL root of the desk
// TODAY 178<->120 flicker; client guard 61464b4 was symptom-only).
// companion_state.json is overwritten IN PLACE by the Mac stall-accountant push
// (scp = truncate + stream), so ~6% of reads (measured 16/250) land mid-write and
// return a valid PREFIX of the JSON -- structurally incomplete, missing the tail
// (by_book/open_detail). Folding that partial frame dropped ~$58 paper from the
// headline. Fix: only serve a STRUCTURALLY COMPLETE frame; on a torn read, replay
// the last complete one. Completeness = balanced braces/brackets outside strings
// (no reliance on key order or an OMEGA sentinel -- a truncated prefix always
// leaves depth>0 or an unterminated string). Single-threaded HTTP run loop
// (one accept() loop on thread_) -> plain static cache, no lock needed.
static bool jsonStructurallyComplete(const std::string& b)
{
    int depth = 0; bool inStr = false, esc = false, sawOpen = false;
    for (char ch : b) {
        if (inStr) {
            if (esc)             esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"')  inStr = false;
            continue;
        }
        if (ch == '"')                    inStr = true;
        else if (ch == '{' || ch == '[') { ++depth; sawOpen = true; }
        else if (ch == '}' || ch == ']') { if (--depth < 0) return false; }
    }
    return sawOpen && depth == 0 && !inStr;
}

static std::string loadCompanionStateAtomic()
{
    std::string raw = loadFile("companion_state.json");
    static std::string s_lastGood;
    if (jsonStructurallyComplete(raw)) { s_lastGood = raw; return raw; }
    if (!s_lastGood.empty()) return s_lastGood;   // torn read -> replay last complete frame
    return raw;                                    // no complete frame seen yet -> passthrough
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

// ?????????????????????????????????????????????????????????????????????????????
// JSON builders
// ?????????????????????????????????????????????????????????????????????????????

static std::string buildTelemetryJson(const OmegaTelemetrySnapshot* s)
{
    if (!s) return "{}";
    const int    trades = s->total_trades;
    const int    wins   = s->wins;
    const double wr     = (trades > 0) ? (100.0 * wins / trades) : 0.0;
    char buf[16384];  // 8192 -> 12288 (S37c PDH/PDL) -> 16384 (S37l CurH/CurL)
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
        // FX majors at %.5f (was %.4f): EUR/GBP/AUD/NZD trade ~1.1/1.3/0.69/0.57 so
        // %.4f = 1-pip resolution -- an IDEALPRO sub-pip spread (0.00001-0.00002)
        // collapses to bid==ask, rendering the desk header spread as "s 0.00000" and
        // masking sub-pip price moves (looked "frozen" though the feed was live).
        // 5dp shows the real bid/ask + spread. (S-2026-07-06 FX-venue follow-up.)
        "\"eurusd_bid\":%.5f,\"eurusd_ask\":%.5f,"
        "\"gbpusd_bid\":%.5f,\"gbpusd_ask\":%.5f,"
        "\"audusd_bid\":%.5f,\"audusd_ask\":%.5f,"
        "\"nzdusd_bid\":%.5f,\"nzdusd_ask\":%.5f,"
        "\"usdcad_bid\":%.5f,\"usdcad_ask\":%.5f,"
        "\"usdjpy_bid\":%.5f,\"usdjpy_ask\":%.5f,"
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
        "\"gold_regime\":\"%s\",\"gold_long_blocked\":%d,\"gold_warm\":%d,"
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
        "\"uptime_sec\":%lld,"
        "\"last_entry_ts\":%lld,\"last_signal_ts\":%lld,"
        "\"gf_trail_stage\":%d,\"gf_profit_usd\":%.2f,\"gf_stack_unlocked\":%d,\"gf_atr_at_entry\":%.4f,"
        "\"sp_pdh\":%.4f,\"sp_pdl\":%.4f,\"nq_pdh\":%.4f,\"nq_pdl\":%.4f,"
        "\"dj_pdh\":%.4f,\"dj_pdl\":%.4f,\"nas_pdh\":%.4f,\"nas_pdl\":%.4f,"
        "\"xau_pdh\":%.4f,\"xau_pdl\":%.4f,\"cl_pdh\":%.4f,\"cl_pdl\":%.4f,"
        "\"ger40_pdh\":%.4f,\"ger40_pdl\":%.4f,\"uk100_pdh\":%.4f,\"uk100_pdl\":%.4f,"
        "\"estx50_pdh\":%.4f,\"estx50_pdl\":%.4f,\"brent_pdh\":%.4f,\"brent_pdl\":%.4f,"
        "\"xag_pdh\":%.4f,\"xag_pdl\":%.4f,\"eurusd_pdh\":%.4f,\"eurusd_pdl\":%.4f,"
        "\"gbpusd_pdh\":%.4f,\"gbpusd_pdl\":%.4f,\"audusd_pdh\":%.4f,\"audusd_pdl\":%.4f,"
        "\"nzdusd_pdh\":%.4f,\"nzdusd_pdl\":%.4f,\"usdjpy_pdh\":%.4f,\"usdjpy_pdl\":%.4f,"
        "\"sp_curh\":%.4f,\"sp_curl\":%.4f,\"nq_curh\":%.4f,\"nq_curl\":%.4f,"
        "\"dj_curh\":%.4f,\"dj_curl\":%.4f,\"nas_curh\":%.4f,\"nas_curl\":%.4f,"
        "\"xau_curh\":%.4f,\"xau_curl\":%.4f,\"cl_curh\":%.4f,\"cl_curl\":%.4f,"
        "\"ger40_curh\":%.4f,\"ger40_curl\":%.4f,\"uk100_curh\":%.4f,\"uk100_curl\":%.4f,"
        "\"estx50_curh\":%.4f,\"estx50_curl\":%.4f,\"brent_curh\":%.4f,\"brent_curl\":%.4f,"
        "\"xag_curh\":%.4f,\"xag_curl\":%.4f,\"eurusd_curh\":%.4f,\"eurusd_curl\":%.4f,"
        "\"gbpusd_curh\":%.4f,\"gbpusd_curl\":%.4f,\"audusd_curh\":%.4f,\"audusd_curl\":%.4f,"
        "\"nzdusd_curh\":%.4f,\"nzdusd_curl\":%.4f,\"usdjpy_curh\":%.4f,\"usdjpy_curl\":%.4f,"
        "\"usdcad_curh\":%.4f,\"usdcad_curl\":%.4f",
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
        s->usdcad_bid, s->usdcad_ask,
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
        s->gold_regime, s->gold_long_blocked, s->gold_warm,
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
        (long long)s->uptime_sec,
        (long long)s->last_entry_ts, (long long)s->last_signal_ts,
        s->gf_trail_stage, s->gf_profit_usd, s->gf_stack_unlocked, s->gf_atr_at_entry,
        s->sp_pdh, s->sp_pdl, s->nq_pdh, s->nq_pdl,
        s->dj_pdh, s->dj_pdl, s->nas_pdh, s->nas_pdl,
        s->xau_pdh, s->xau_pdl, s->cl_pdh, s->cl_pdl,
        s->ger40_pdh, s->ger40_pdl, s->uk100_pdh, s->uk100_pdl,
        s->estx50_pdh, s->estx50_pdl, s->brent_pdh, s->brent_pdl,
        s->xag_pdh, s->xag_pdl, s->eurusd_pdh, s->eurusd_pdl,
        s->gbpusd_pdh, s->gbpusd_pdl, s->audusd_pdh, s->audusd_pdl,
        s->nzdusd_pdh, s->nzdusd_pdl, s->usdjpy_pdh, s->usdjpy_pdl,
        s->sp_curh, s->sp_curl, s->nq_curh, s->nq_curl,
        s->dj_curh, s->dj_curl, s->nas_curh, s->nas_curl,
        s->xau_curh, s->xau_curl, s->cl_curh, s->cl_curl,
        s->ger40_curh, s->ger40_curl, s->uk100_curh, s->uk100_curl,
        s->estx50_curh, s->estx50_curl, s->brent_curh, s->brent_curl,
        s->xag_curh, s->xag_curl, s->eurusd_curh, s->eurusd_curl,
        s->gbpusd_curh, s->gbpusd_curl, s->audusd_curh, s->audusd_curl,
        s->nzdusd_curh, s->nzdusd_curl, s->usdjpy_curh, s->usdjpy_curl,
        s->usdcad_curh, s->usdcad_curl
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
    result += "]";  // close signal_history array -- outer object closed after brackets below

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
    result += "}";  // close brackets{} only -- root {} closed by final "}" after ca_engines

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
        char ep[768];
        snprintf(ep, sizeof(ep),
            ",\"eng_pnl\":{\"breakout\":%.2f,\"bracket\":%.2f,\"gold_stack\":%.2f,\"gold_flow\":%.2f,\"cross\":%.2f,\"latency\":%.2f,\"mean_rev\":%.2f,\"h1_swing\":%.2f,\"h4_regime\":%.2f}"
            ",\"eng_trades\":{\"breakout\":%d,\"bracket\":%d,\"gold_stack\":%d,\"gold_flow\":%d,\"cross\":%d,\"latency\":%d,\"mean_rev\":%d,\"h1_swing\":%d,\"h4_regime\":%d}"
            ",\"htf\":{\"h1_open\":%d,\"h4_open\":%d,\"h1_pnl\":%.2f,\"h4_pnl\":%.2f,\"h1_shadow\":%d,\"h4_shadow\":%d,\"h1_adx\":%.1f,\"h4_adx\":%.1f,\"h4_trend\":%d}",
            s->eng_pnl_breakout,   s->eng_pnl_bracket,
            s->eng_pnl_gold_stack, s->eng_pnl_gold_flow,
            s->eng_pnl_cross,      s->eng_pnl_latency,
            s->eng_pnl_mean_rev,   s->eng_pnl_h1_swing, s->eng_pnl_h4_regime,
            s->eng_trades_breakout,   s->eng_trades_bracket,
            s->eng_trades_gold_stack, s->eng_trades_gold_flow,
            s->eng_trades_cross,      s->eng_trades_latency,
            s->eng_trades_mean_rev,   s->eng_trades_h1_swing, s->eng_trades_h4_regime,
            s->h1_swing_open,  s->h4_regime_open,
            (double)s->h1_swing_daily_pnl, (double)s->h4_regime_daily_pnl,
            s->h1_swing_shadow, s->h4_regime_shadow,
            (double)s->h1_adx, (double)s->h4_adx, s->h4_trend_state);
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
        // Gold book: XAUUSD spot data from cTrader (pinned by ID then name).
        appendBook("gold", s->l2_book_gold_bid, s->l2_book_gold_bids,
                           s->l2_book_gold_ask, s->l2_book_gold_asks);
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

    // open_positions array -- drives green "LIVE ?" highlight in engine cells.
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
        if (s->xau_phase   == 3) addPos("XAUUSD");
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
        if (s->bkt_gold.phase == 3) addPos("XAUUSD");
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

    // 2026-05-09 BROKER RECONCILIATION: append truth-of-state metrics
    // computed from g_omegaLedger. These let the dashboard distinguish
    // "engine paper" from "broker realised" so today's NZ$300 disparity
    // failure mode is visible the moment it starts.
    {
        const double engine_pnl     = g_omegaLedger.engineLivePnl();
        const double broker_pnl     = g_omegaLedger.brokerRealisedPnl();
        const double disparity      = engine_pnl - broker_pnl;
        const int    orphan_count   = g_omegaLedger.brokerOrphanCount();
        const int    reject_count   = g_omegaLedger.brokerRejectCount();
        const int    confirmed_cnt  = g_omegaLedger.brokerConfirmedCount();
        char br[512];
        snprintf(br, sizeof(br),
            ",\"broker\":{"
            "\"engine_pnl\":%.2f,"
            "\"realised_pnl\":%.2f,"
            "\"disparity\":%.2f,"
            "\"orphan_count\":%d,"
            "\"reject_count\":%d,"
            "\"confirmed_count\":%d"
            "}",
            engine_pnl, broker_pnl, disparity,
            orphan_count, reject_count, confirmed_cnt);
        // Insert before the closing brace of the root object.
        // result currently does NOT have the closing "}" yet -- it's added
        // on the next line.
        result += br;
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

// ?????????????????????????????????????????????????????????????????????????????
// buildHistoryJson -- reads omega_trade_closes.csv from disk and returns JSON.
// Survives restarts. Shows all trades from today's file (daily-rotating log).
// Used by /api/history endpoint so the dashboard always has complete session data.
// Falls back to in-memory snapshot when CSV is unavailable.
// ?????????????????????????????????????????????????????????????????????????????
static std::string buildHistoryJson(bool all_time = false)
{
    // Determine path: same logic as main.cpp log_root_dir() + /trades/omega_trade_closes.csv
    // Default: read TODAY's daily file -- matches the stats header exactly.
    // all_time (GET /api/history?all=1): read the cumulative rolling file so the
    // dashboard can show full trade history across restarts/clears/day-rollover.
    // The old code read omega_trade_closes.csv (cumulative all-time rolling file)
    // which caused the trade table to show trades from previous days, making
    // the PnL in the table disagree with the daily PnL in the header.
    char today[16] = {};
    {
        time_t t = time(nullptr);
        struct tm ti{};
#ifdef _WIN32
        gmtime_s(&ti, &t);
#else
        gmtime_r(&t, &ti);
#endif
        snprintf(today, sizeof(today), "%04d-%02d-%02d",
                 ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday);
    }
    static const char* DAILY_DIRS[] = {
        "logs/trades",
        "C:\\Omega\\logs\\trades",
        "C:\\Omega\\build\\Release\\logs\\trades",
        "../logs/trades",
        "..\\..\\logs\\trades",
    };
    char daily_path[512];
    FILE* f = nullptr;
    for (auto d : DAILY_DIRS) {
        if (all_time)
            snprintf(daily_path, sizeof(daily_path), "%s/omega_trade_closes.csv", d);
        else
            snprintf(daily_path, sizeof(daily_path), "%s/omega_trade_closes_%s.csv", d, today);
        f = fopen(daily_path, "r");
        if (f) break;
    }
    // Default (daily) view: do NOT fall back to cumulative -- if today's file
    // doesn't exist, no trades happened today, and the cumulative all-time CSV
    // would show stale data that disagrees with the daily PnL header.
    // all_time view intentionally reads the cumulative file and may be empty.
    if (!f) {
        return "[]";
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

        // Parse CSV row -- fields:
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
// buildCryptoTradesJson (S-2026-07-10; +chimera S-2026-07-12) -- reads the
// crypto books' closed trades and returns them in the same shape as
// buildTradesJson (+ book tag) so the unified "LAST TRADES" panel can merge
// them with the Omega engine ledger. Three sources (all REAL FORWARD trades,
// never backtest rows), each with an absolute VPS path + a relative dev
// fallback tried FIRST-OPEN-WINS per source (so the same file is never read
// twice — the old flat list could double-read crypto_inbound.csv when cwd was
// C:\Omega):
//   crypto_inbound.csv          book="crypto"   Mac ibkrcrypto daily book
//   crypto_intraday_inbound.csv book="crypto"   Mac ibkrcrypto intraday book
//   chimera_inbound.csv         book="chimera"  josgp1 chimera SHADOW closes
//     (slot engines/MIMIC parents/companion clips/XSec rebalances; written by
//     chimera export_desk_trade, relayed by tools/gui/refresh_crypto_companion.sh
//     every 120s). DISPLAY-ONLY — never folded into ALL-TIME PnL (the fold
//     path, CryptoLedgerInbound, reads crypto_inbound.csv only).
// CSV: id,entry_ts,exit_ts,sym,strat,side,entry_px,exit_px,pnl[,reason]
// (chimera rows carry the optional 10th reason column: SL/TRAIL/TIME/REBAL…)
// All sources merge (no break): the GUI sorts the union by exitTs, newest 15.
// ─────────────────────────────────────────────────────────────────────────────
static std::string buildCryptoTradesJson()
{
    struct CryptoSrc { const char* abs_path; const char* rel_path; const char* book; };
    static const CryptoSrc SRCS[] = {
        // S-2026-07-12 CONSOLIDATION: the Mac ibkrcrypto book was folded onto the ONE
        // Chimera system (josgp1) and its crons retired -> crypto_inbound.csv +
        // crypto_intraday_inbound.csv are FROZEN. Dropped from the desk so LAST-15 shows
        // only the live Chimera book. (Re-add if the Mac book is ever revived.)
        { "C:\\Omega\\logs\\trades\\chimera_inbound.csv",         "logs/trades/chimera_inbound.csv",         "chimera" },
    };
    std::string out = "[";
    bool first = true;
    for (const auto& src : SRCS) {
        FILE* f = fopen(src.abs_path, "r");
        if (!f) f = fopen(src.rel_path, "r");
        if (!f) continue;
        std::vector<std::string> rows;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (line[0]=='i' && line[1]=='d') continue;   // header
            rows.push_back(line);
        }
        fclose(f);
        // newest last in file -> emit last 20 reversed (newest first)
        int start = (int)rows.size() > 20 ? (int)rows.size()-20 : 0;
        for (int i = (int)rows.size()-1; i >= start; --i) {
            char buf[1024]; strncpy(buf, rows[i].c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
            size_t l = strlen(buf); while (l && (buf[l-1]=='\n'||buf[l-1]=='\r')) buf[--l]='\0';
            char* fld[16]; int nf=0; char* p=buf;
            while (nf<16 && *p) { fld[nf++]=p; while (*p && *p!=',') ++p; if (*p) *p++='\0'; }
            if (nf < 9) continue;
            long long ex_ts = atoll(fld[2]);
            char ut[32]={}; time_t tt=(time_t)ex_ts; struct tm* g=gmtime(&tt);
            if (g) strftime(ut, sizeof(ut), "%Y-%m-%d %H:%M", g);
            if (!first) out += ',';
            first = false;
            char row[512];
            snprintf(row, sizeof(row),
                "{\"book\":\"%s\",\"symbol\":\"%s\",\"engine\":\"%s\",\"side\":\"%s\","
                "\"price\":%.4f,\"exitPrice\":%.4f,\"net_pnl\":%.2f,\"pnl\":%.2f,"
                "\"exitTs\":%lld,\"exit_ts_utc\":\"%s UTC\",\"exitReason\":\"%s\"}",
                src.book, fld[3], fld[4], fld[5], atof(fld[6]), atof(fld[7]), atof(fld[8]), atof(fld[8]),
                ex_ts, ut, nf >= 10 ? fld[9] : "");
            out += row;
        }
    }
    out += ']';
    return out;
}

// ?????????????????????????????????????????????????????????????????????????????
// buildShadowTradesJson -- reads omega_shadow.csv and returns last 60 trades
// as JSON (matching buildTradesJson shape with shadow:true flag added so the
// dashboard can render them in a tinted "SHADOW (audit)" table alongside the
// real broker trades. Shadow trades never hit the broker -- they're written by
// engines running with shadow_mode=true (AtrMeanRevGrid, etc).
//
// CSV header (per omega_main.hpp): ts_unix,symbol,side,engine,entry_px,exit_px,
//                                  pnl,mfe,mae,hold_sec,exit_reason,
//                                  spread_at_entry,latency_ms,regime
// ?????????????????????????????????????????????????????????????????????????????
static std::string buildShadowTradesJson()
{
    static const char* PATHS[] = {
        "C:\\Omega\\logs\\shadow\\omega_shadow.csv",
        "logs/shadow/omega_shadow.csv",
        "../logs/shadow/omega_shadow.csv",
        "../../logs/shadow/omega_shadow.csv",
        nullptr
    };
    FILE* f = nullptr;
    for (int i = 0; PATHS[i]; ++i) { f = fopen(PATHS[i], "r"); if (f) break; }
    if (!f) return "[]";

    // Read whole file (bounded -- AMR is sparse, shadow file stays small).
    std::vector<std::string> lines;
    char line[2048];
    bool header_skipped = false;
    while (fgets(line, sizeof(line), f)) {
        if (!header_skipped) { header_skipped = true; if (line[0]=='t') continue; }
        lines.push_back(line);
    }
    fclose(f);

    // Keep last 60.
    const size_t want = 60;
    size_t start = lines.size() > want ? lines.size() - want : 0;

    std::string out;
    out.reserve(8192);
    out += '[';
    bool first = true;
    for (size_t i = start; i < lines.size(); ++i) {
        // tokenise CSV
        char buf[2048];
        strncpy(buf, lines[i].c_str(), sizeof(buf)-1); buf[sizeof(buf)-1]='\0';
        // strip newline
        size_t L = strlen(buf);
        if (L > 0 && (buf[L-1]=='\n'||buf[L-1]=='\r')) buf[L-1]='\0';
        if (L > 1 && buf[L-2]=='\r') buf[L-2]='\0';

        char* fields[32]; int nf = 0; char* p = buf;
        while (nf < 31 && *p) {
            fields[nf++] = p;
            while (*p && *p != ',') ++p;
            if (*p) { *p++ = '\0'; }
        }
        if (nf < 11) continue;
        long long entry_ts = atoll(fields[0]);
        const char* symbol = fields[1];
        const char* side   = fields[2];
        const char* engine = fields[3];
        double entry_px = atof(fields[4]);
        double exit_px  = atof(fields[5]);
        double pnl      = atof(fields[6]);
        double mfe      = atof(fields[7]);
        double mae      = atof(fields[8]);
        long long hold_sec = atoll(fields[9]);
        const char* exit_reason = fields[10];
        double spread = nf > 11 ? atof(fields[11]) : 0.0;
        double lat    = nf > 12 ? atof(fields[12]) : 0.0;
        const char* regime = nf > 13 ? fields[13] : "";
        long long exit_ts = entry_ts + hold_sec;

        if (!first) out += ',';
        first = false;
        char row[1024];
        snprintf(row, sizeof(row),
            "{\"shadow\":true,\"symbol\":\"%s\",\"side\":\"%s\",\"engine\":\"%s\","
            "\"price\":%.5f,\"exitPrice\":%.5f,\"pnl\":%.4f,\"net_pnl\":%.4f,"
            "\"mfe\":%.4f,\"mae\":%.4f,"
            "\"entryTs\":%lld,\"exitTs\":%lld,"
            "\"exitReason\":\"%s\",\"regime\":\"%s\","
            "\"spread_at_entry\":%.5f,\"latency_ms\":%.1f}",
            symbol, side, engine,
            entry_px, exit_px, pnl, pnl,
            mfe, mae,
            entry_ts, exit_ts,
            exit_reason, regime,
            spread, lat);
        out += row;
    }
    out += ']';
    return out;
}

// ?????????????????????????????????????????????????????????????????????????????
// buildDailySummaryJson -- aggregates today's trades from CSV into a summary.
// Returns net P&L, trade count, win rate, by-engine breakdown.
// ?????????????????????????????????????????????????????????????????????????????
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

    // No file for today = no trades yet today. Return zero summary.
    // Do NOT fall back to cumulative CSV -- that shows all historical trades
    // in the header as "(FROM DISK)" after UTC midnight rollover.
    if (!f) {
        char out[256];
        snprintf(out, sizeof(out),
            "{\"date\":\"%s\",\"source\":\"empty\","
            "\"total_trades\":0,\"wins\":0,\"losses\":0,"
            "\"win_rate\":0.0,\"net_pnl\":0.00,\"gross_pnl\":0.00}",
            today);
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
        char exit_rsn[64]={}; if(nf>30) strncpy(exit_rsn,fields[30],sizeof(exit_rsn)-1);
        // TOTALS FIX: exclude PARTIAL_1R/PARTIAL_2R from trade count and W/L.
        // Dollars still accumulated (real money banked) but count/wins/losses not.
        const bool is_partial = (strncmp(exit_rsn,"PARTIAL_1R",10)==0 ||
                                  strncmp(exit_rsn,"PARTIAL_2R",10)==0);
        total+=net; gross+=grs;
        if(!is_partial){
            ++n;
            if(net>0) ++wins;
            auto& e=by_eng[eng]; ++e.n; e.pnl+=net; if(net>0)++e.w;
        }
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

// ?????????????????????????????????????????????????????????????????????????????
// OmegaTelemetryServer ctor / dtor / start / stop
// ?????????????????????????????????????????????????????????????????????????????

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
        // Direct pointer -- same process, no shared memory roundtrip needed
        snap_ = snap;
        std::cout << "[OmegaHTTP] Direct snapshot pointer OK\n";
    } else {
        hMap_ = OpenFileMappingA(FILE_MAP_READ, FALSE, "Global\\OmegaTelemetrySharedMemory");
        if (hMap_)
            snap_ = static_cast<OmegaTelemetrySnapshot*>(
                        MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, sizeof(OmegaTelemetrySnapshot)));
        if (!snap_)
            std::cerr << "[OmegaHTTP] WARNING: snapshot null -- no data will show\n";
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

// ?????????????????????????????????????????????????????????????????????????????
// WebSocket helpers
// ?????????????????????????????????????????????????????????????????????????????

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

// ?????????????????????????????????????????????????????????????????????????????
// WebSocket broadcast loop -- 1000ms push
// ?????????????????????????????????????????????????????????????????????????????

void OmegaTelemetryServer::wsBroadcastLoop()
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    ws_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(ws_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // binds all interfaces -- GUI accessible from browser
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
                        ws_clients_.push_back(c);
                    } else { closesocket(c); }
                } else { closesocket(c); }
            } else { closesocket(c); }
        }

        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= 1000) {
            last_broadcast = now;
            // Update uptime every broadcast -- independent of FIX tick rate
            if (snap_ && snap_->start_time > 0)
                snap_->uptime_sec = static_cast<int64_t>(std::time(nullptr)) - snap_->start_time;
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

// ?????????????????????????????????????????????????????????????????????????????
// HTTP handler
// ?????????????????????????????????????????????????????????????????????????????

void OmegaTelemetryServer::run(int port)
{
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    // Timeout on accept() so the run loop can check running_ and exit cleanly on shutdown.
    // Without this, closesocket(server_fd_) in stop() may not wake a blocked accept() on
    // all Windows versions -- the thread hangs in join() forever and Ctrl+C appears to freeze.
    DWORD accept_timeout_ms = 200;
    setsockopt(server_fd_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&accept_timeout_ms), sizeof(accept_timeout_ms));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;  // binds all interfaces -- GUI accessible from browser
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

        if (strstr(buf, "GET /api/symbols")) {
            // Serve the resolved symbol map written by main process on connect
            ct = "application/json";
            const char* paths[] = {
                "C:\\Omega\\logs\\symbols_resolved.json",
                "logs/symbols_resolved.json",
                "../logs/symbols_resolved.json",
                nullptr
            };
            for (int pi = 0; paths[pi]; ++pi) {
                FILE* fp = fopen(paths[pi], "r");
                if (!fp) continue;
                fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
                if (sz > 0 && sz < 1024*1024) {
                    body.resize(sz);
                    fread(&body[0], 1, sz, fp);
                }
                fclose(fp);
                break;
            }
            if (body.empty()) body = "{\"error\":\"symbols_resolved.json not found - connect cTrader first\"}";
        }
        else if (strstr(buf, "GET /api/telemetry"))        { ct = "application/json"; body = buildTelemetryJson(snap_); }
        else if (strstr(buf, "GET /api/trades"))      { ct = "application/json"; body = buildTradesJson(); }
        else if (strstr(buf, "GET /api/history"))     { ct = "application/json"; body = buildHistoryJson(strstr(buf, "all=1") != nullptr); }
        else if (strstr(buf, "GET /api/shadow_trades")) { ct = "application/json"; body = buildShadowTradesJson(); }
        else if (strstr(buf, "GET /api/crypto_trades")) { ct = "application/json"; body = buildCryptoTradesJson(); }
        else if (strstr(buf, "GET /api/predictive_ranges")) {
            // S-2026-06-12c: stepped Predictive Ranges snapshot, written every 60s
            // by the on_tick [BAR-SAVE] block (logs/predictive_ranges.json).
            ct = "application/json";
            body = loadFile("logs/predictive_ranges.json");
            if (body.empty()) body = "{\"updated\":0,\"datasets\":{}}";
        }
        else if (strstr(buf, "GET /api/daily"))       { ct = "application/json"; body = buildDailySummaryJson(); }
        else if (strstr(buf, "GET /api/companion"))   { ct = "application/json"; body = loadCompanionStateAtomic(); if (body.empty()) body = "{\"open_companions\":0,\"open_detail\":[]}"; }
        else if (strstr(buf, "GET /api/crypto_companion")) { ct = "application/json"; body = loadFile("crypto_companion_state.json"); if (body.empty()) body = "{\"ts\":0,\"legs\":[]}"; }
        // S-2026-07-18r LIVE REALIZED cash truth: real Binance fills' realized PnL (fees
        // included), written on josgp1 (data/live_realized.json), relayed by
        // refresh_crypto_companion.sh HOP 3. GUI folds total_usd into ALL-TIME (operator
        // order: the correct live loss number in the PnL — real forward fills, never shadow).
        else if (strstr(buf, "GET /api/crypto_live_pnl")) { ct = "application/json"; body = loadFile("live_realized.json"); if (body.empty()) body = "{\"ts\":0,\"total_usd\":0,\"trades\":[]}"; }
        // S-2026-07-18af CHIMERA HEALTH truth chip (operator: "see IMMEDIATELY that the
        // correct software is loaded" — banners missed during the 07-18 93-restart loop).
        // Mac chimera_executor_watch.sh probes josgp1 every 1min (build==repo HEAD, mode
        // LIVE, restart-loop, config-conflict), writes /tmp/chimera_health.json, relayed
        // by refresh_crypto_companion.sh HOP 4. GUI header CC chip renders it; a stale ts
        // (dead watch/relay) reads RED = NOT-VERIFIED, never silently green.
        else if (strstr(buf, "GET /api/chimera_health")) { ct = "application/json"; body = loadFile("chimera_health.json"); if (body.empty()) body = "{\"ts\":0}"; }
        // rdagent stock basket (S-2026-07-11): pushed from the Mac so the META-type picks show ON
        // the desk instead of the Mac-only :7799 page. book = paper P&L/orders; rank = model ranking.
        else if (strstr(buf, "GET /api/rdagent_book")) { ct = "application/json"; body = loadFile("data/rdagent/rda_basket.json"); if (body.empty()) body = "{\"mode\":\"none\",\"n_names\":0,\"orders\":[]}"; }
        else if (strstr(buf, "GET /api/rdagent_rank")) { ct = "application/json"; body = loadFile("data/rdagent/latest.json"); if (body.empty()) body = "{\"signal\":{\"basket\":[]}}"; }
        else if (strstr(buf, "GET /api/gold_companion")) { ct = "application/json"; body = loadFile("gold_companion_state.json"); if (body.empty()) body = "{\"ts\":0,\"flavors\":[]}"; }
        else if (strstr(buf, "GET /api/xag_companion"))  { ct = "application/json"; body = loadFile("xag_companion_state.json"); if (body.empty()) body = "{\"ts\":0,\"flavors\":[]}"; }
        else if (strstr(buf, "GET /api/usoil_companion")){ ct = "application/json"; body = loadFile("usoil_companion_state.json"); if (body.empty()) body = "{\"ts\":0,\"flavors\":[]}"; }
        else if (strstr(buf, "GET /api/fx_companion"))   { ct = "application/json"; body = loadFile("fx_companion_state.json"); if (body.empty()) body = "{\"engine\":\"fx-befloor-pos-neg\",\"pairs\":[]}"; }
        else if (strstr(buf, "GET /api/index_companion")){ ct = "application/json"; body = loadFile("index_companion_state.json"); if (body.empty()) body = "{\"engine\":\"index-befloor-pos-neg\",\"syms\":[]}"; }
        else if (strstr(buf, "GET /api/stockmover_companion")){ ct = "application/json"; body = loadFile("stockmover_companion_state.json"); if (body.empty()) body = "{\"engine\":\"stockmover-befloor-pos-neg\",\"names\":[]}"; }
        else if (strstr(buf, "GET /api/stockladder_companion")){ ct = "application/json"; body = loadFile("stockladder_companion_state.json"); if (body.empty()) body = "{\"engine\":\"stockmover-mimic-ladder\",\"names\":[]}"; }
        else if (strstr(buf, "GET /api/stockdipturtle")){ ct = "application/json"; body = loadFile("stockdipturtle_state.json"); if (body.empty()) body = "{\"engine\":\"stock-dip-turtle\",\"books\":[]}"; }
        // BigCapHi52 (S-2026-07-17m): SHADOW deploy-forward 52wk-high portfolio paper book
        // (rdagent-basket class). Engine writes hi52_state.json atomically on every daily row.
        else if (strstr(buf, "GET /api/hi52")){ ct = "application/json"; body = loadFile("hi52_state.json"); if (body.empty()) body = "{\"engine\":\"BigCapHi52\",\"shadow\":true,\"pnl_usd\":0,\"eq_fwd\":1.0,\"members\":[]}"; }
        else if (strstr(buf, "GET /api/bigcap2pct_companion")){ ct = "application/json"; body = loadFile("bigcap2pct_companion_state.json"); if (body.empty()) body = "{\"engine\":\"bigcap-2pct-impulse\",\"names\":[]}"; }
        // (GET /api/jumprider endpoint REMOVED — JumpRider engine culled/tombstoned S-2026-07-10)
        else if (strstr(buf, "GET /api/fxladder_companion")){ ct = "application/json"; body = loadFile("fxladder_companion_state.json"); if (body.empty()) body = "{\"engine\":\"fx-mimic-ladder\",\"pairs\":[]}"; }
        else if (strstr(buf, "GET /api/idxladder_companion")){ ct = "application/json"; body = loadFile("idxladder_companion_state.json"); if (body.empty()) body = "{\"engine\":\"index-mimic-ladder\",\"pairs\":[]}"; }
        else if (strstr(buf, "POST /api/clear_ledger") || strstr(buf, "GET /api/clear_ledger")) {
            // Clear in-memory ledger + rename today's CSV so it won't be re-read.
            //
            // S113 2026-05-19: path list must match buildHistoryJson's DAILY_DIRS
            // exactly, OR /api/history will still find the CSV in a directory the
            // clear endpoint never touched (e.g. C:\Omega\build\Release\logs\trades).
            // Also iterate ALL paths (don't break on first rename) -- the CSV may
            // exist in multiple directories during a deploy transition.
            ct = "application/json";
            g_omegaLedger.resetDaily();
            char today[16] = {};
            { time_t t = time(nullptr); struct tm ti{};
#ifdef _WIN32
              gmtime_s(&ti, &t);
#else
              gmtime_r(&t, &ti);
#endif
              snprintf(today, sizeof(today), "%04d-%02d-%02d",
                       ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday); }
            // Match buildHistoryJson DAILY_DIRS list verbatim
            static const char* DIRS[] = {
                "logs/trades",
                "C:\\Omega\\logs\\trades",
                "C:\\Omega\\build\\Release\\logs\\trades",
                "../logs/trades",
                "..\\..\\logs\\trades",
                nullptr };
            int n_cleared = 0;
            for (int pi = 0; DIRS[pi]; ++pi) {
                char src[512], dst[640];
                snprintf(src, sizeof(src), "%s/omega_trade_closes_%s.csv", DIRS[pi], today);
                // Append unix-ts suffix so repeated clears don't collide on dst
                snprintf(dst, sizeof(dst), "%s/omega_trade_closes_%s.cleared_%lld.csv",
                         DIRS[pi], today, (long long)time(nullptr));
                if (rename(src, dst) == 0) {
                    ++n_cleared;
                    printf("[LEDGER-CLEAR] renamed %s -> %s\n", src, dst);
                }
            }
            char msg[256];
            snprintf(msg, sizeof(msg),
                "{\"ok\":true,\"msg\":\"Ledger cleared\",\"csvs_renamed\":%d}",
                n_cleared);
            body = msg;
            printf("[LEDGER-CLEAR] Session ledger cleared via API (csvs_renamed=%d)\n",
                   n_cleared);
            fflush(stdout);
        }
        else if (strstr(buf, "POST /api/flatten")) {
            // ── MANUAL KILL-ALL panic flatten (S-2026-06-30) ──────────────
            // Operator-triggered "close ALL open positions NOW" from the desk
            // panic button. POST-only (never GET) so a crawler / prefetch can
            // never fire it. We DO NOT close here -- sending broker orders from
            // this HTTP thread would race the trading thread mid-tick. Instead
            // we raise an atomic flag that on_tick consumes exactly once on the
            // trading thread (real opposing MKT order per position via
            // send_live_order, itself hard SHADOW-gated, + close_matching to
            // clear the engine slot). Per-book by construction: this process
            // only holds the Omega book.
            ct = "application/json";
            g_flatten_all_request.store(true);
            printf("[KILL-ALL] panic flatten REQUESTED via desk button -- "
                   "trading thread will flatten all open positions on next tick\n");
            // S-2026-07-18t KILL-ALL fan-out (LiveMirrorIncident20260718 follow-up):
            // this button only ever flattened THIS box -- the crypto system (josgp1)
            // kept its armed companion legs and, when live, the LiveMimicMirror's
            // REAL Binance holdings (operator hit KILL ALL 2026-07-18 and the crypto
            // panel kept trading). Fan out to chimera's full /api/kill (sleeves +
            // 132 companions + mirror flatten_all) through the nginx control path.
            // Auth file (NOT in git): C:\Omega\config\chimera_kill.auth, line 1 =
            // nginx basic-auth user:pass, line 2 = chimera control token. Missing
            // file = loud warn, local flatten still proceeds. Detached curl.exe so
            // this HTTP thread never blocks on the WAN.
            {
                FILE* af = fopen("C:\\Omega\\config\\chimera_kill.auth", "rb");
                if (af) {
                    char up[128] = {}, tok[128] = {};
                    if (fgets(up, sizeof(up), af)) fgets(tok, sizeof(tok), af);
                    fclose(af);
                    char* trims[2] = { up, tok };
                    for (int ti = 0; ti < 2; ++ti) {
                        char* s = trims[ti];
                        size_t sl = strlen(s);   // not 'n': hides the L1268 local -> MSVC /WX C4456
                        while (sl && (s[sl-1]=='\n' || s[sl-1]=='\r' || s[sl-1]==' ')) s[--sl] = 0;
                    }
                    if (up[0] && tok[0]) {
                        char cmd[640];
                        snprintf(cmd, sizeof(cmd),
                            "start /b curl.exe -k -s -m 10 -X POST -u %s "
                            "-H \"X-Auth-Token: %s\" "
                            "https://143.198.89.54:9443/api/kill "
                            ">> C:\\Omega\\logs\\chimera_kill_fanout.log 2>&1",
                            up, tok);
                        system(cmd);
                        printf("[KILL-ALL] fan-out to chimera /api/kill dispatched "
                               "(result -> logs\\chimera_kill_fanout.log)\n");
                    } else {
                        printf("[KILL-ALL] WARN chimera_kill.auth malformed -- "
                               "crypto box NOT killed by this button\n");
                    }
                } else {
                    printf("[KILL-ALL] WARN no C:\\Omega\\config\\chimera_kill.auth -- "
                           "crypto box NOT killed by this button\n");
                }
            }
            fflush(stdout);
            body = "{\"ok\":true,\"msg\":\"flatten queued -- closing all open positions "
                   "(this box + chimera fan-out)\"}";
        }
        else if (strstr(buf, "GET /api/shadow_csv"))  {
            // Serve the full shadow CSV for remote analysis -- tries all known paths
            ct = "text/csv";
            const char* paths[] = {
                "C:\\Omega\\logs\\trades\\omega_trade_closes.csv",
                "C:\\Omega\\logs\\shadow\\omega_shadow.csv",
                "logs/trades/omega_trade_closes.csv",
                "logs/shadow/omega_shadow.csv",
                "../logs/trades/omega_trade_closes.csv",
                nullptr
            };
            for (int pi = 0; paths[pi]; ++pi) {
                FILE* f = fopen(paths[pi], "r");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    body.resize(static_cast<size_t>(sz));
                    fread(&body[0], 1, static_cast<size_t>(sz), f);
                }
                fclose(f);
                if (!body.empty()) break;
            }
            if (body.empty()) { status = 404; body = "no shadow data yet"; ct = "text/plain"; }
        }
        else if (strstr(buf, "GET /api/health")) {
            // Serve the latest healthcheck snapshot from tools/healthcheck.ps1.
            // Returns the raw JSON; the GUI parses overall + checks. If the
            // snapshot is missing or stale (>10m), we synthesize a FAIL so the
            // operator is notified rather than seeing a silently-stale "OK".
            ct = "application/json";
            const char* paths[] = {
                "C:\\Omega\\logs\\health\\status.json",
                "logs/health/status.json",
                "../logs/health/status.json",
                nullptr
            };
            for (int pi = 0; paths[pi]; ++pi) {
                FILE* f = fopen(paths[pi], "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0) {
                    body.resize(static_cast<size_t>(sz));
                    fread(&body[0], 1, static_cast<size_t>(sz), f);
                }
                fclose(f);
                if (!body.empty()) break;
            }
            if (body.empty()) {
                body = "{\"overall\":\"FAIL\",\"fail_count\":1,\"warn_count\":0,"
                       "\"checks\":[{\"name\":\"healthcheck.snapshot_present\","
                       "\"severity\":\"FAIL\",\"status\":\"missing\","
                       "\"detail\":\"logs/health/status.json not found. Schedule "
                       "tools/healthcheck.ps1 to run every 2 min as SYSTEM.\"}]}";
            }
        }
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
        else if (strstr(buf, "GET /legacy")) {
            ct = "text/html";
            body = omega_gui_legacy::INDEX_HTML;
        }
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

