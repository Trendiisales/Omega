#pragma once
// ==============================================================================
// OmegaFIX.hpp -- BlackBull cTrader FIX 4.4 constants and symbol tables
//
// ── DEPTH UPGRADE (2026-03-24) ──────────────────────────────────────────────
// 264=5 (5-level depth) replaces 264=1 (top-of-book only).
// Evidence for compatibility:
//   ✓ BlackBull cTrader GUI shows live DoM with 4+ levels AND real size data
//     (confirmed from BRENT screenshot: bid 5000/10000/85000/500 visible)
//   ✓ cTrader FIX 4.4 spec supports 264=N for N levels on DMA accounts
//   ✓ L2Book parser already handles up to 5 levels (bid_count<5 check present)
//   ✓ 271 (MDEntrySize) parsing already in place
//
// FALLBACK SAFETY:
//   If BlackBull rejects 264=5 (MarketDataRequestReject, 35=Y), the handler
//   in dispatch_fix() sets g_md_depth_fallback=true and re-subscribes at 264=1.
//   This prevents the ghost session loop. The fallback is permanent for the
//   session -- if 264=5 is rejected, the session stays at 264=1 forever.
//
// ORIGINAL CONSTRAINTS (still enforced where applicable):
//   265 (MDUpdateType)  : MUST be 0 (full refresh only).
//   267 (NoMDEntryTypes): MUST be 2 -- bid(0) + ask(1) only.
//   269 (MDEntryType)   : 0=bid, 1=ask ONLY.
//   263 (SubReqType)    : 1=subscribe, 2=unsubscribe ONLY.
//
// FIX message builders live in main.cpp under "// ── IMMUTABLE FIX SECTION ──"
// Symbol IDs and tables live here.
// ==============================================================================
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>

struct SymbolDef    { int id; const char* name; };
struct ExtSymbolDef { int id; const char* name; };

static SymbolDef OMEGA_SYMS[] = {
    { 2642, "US500.F" }, { 2643, "USTEC.F" }, { 2632, "USOIL.F" },
    { 4462, "VIX.F"   }, { 2638, "DX.F"    }, { 2637, "DJ30.F"  },
    {  110, "NAS100"  },  // NAS100 cash -- id=110 confirmed from broker symbol list.
                          // NBM engine (NoiseBandMomentum) is active on this symbol.
                          // USTEC.F (id=2643) is separate futures CFD -- both needed.
    {   41, "XAUUSD"  }, { 2631, "NGAS.F"  },
    // FIX ID 41 = XAUUSD spot on BlackBull (confirmed seclist_raw.txt line 209).
    // USOIL.F (id=2632): BlackBull prices this at ~$102 -- may be Brent-priced.
    // BrentWtiSpreadEngine DISABLED until correct instrument confirmed.
};
static const int OMEGA_NSYMS = 9;

static std::vector<ExtSymbolDef> g_ext_syms = {
    {0,"GER40"},{0,"UK100"},{0,"ESTX50"},{0,"XAGUSD"},
    {0,"EURUSD"},{0,"BRENT"},{0,"GBPUSD"},{0,"AUDUSD"},
    {0,"NZDUSD"},{0,"USDJPY"}
};

// ── Depth capability flags ───────────────────────────────────────────────────
// g_md_depth_ok: starts true (we request 264=5). Set false if broker rejects.
// g_md_depth_fallback: set true by 35=Y handler -- triggers re-sub at 264=1.
// Both are written only from the quote thread (dispatch_fix context).
static std::atomic<bool> g_md_depth_ok{true};
static std::atomic<bool> g_md_depth_fallback{false};

static std::mutex g_symbol_map_mtx;
static std::unordered_map<int,std::string> g_id_to_sym;

static int symbol_name_to_id(const std::string& name) noexcept {
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        if (name == OMEGA_SYMS[i].name) return OMEGA_SYMS[i].id;
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    for (const auto& e : g_ext_syms)
        if (name == e.name && e.id > 0) return e.id;
    return 0;
}

static void build_id_map() {
    std::lock_guard<std::mutex> lk(g_symbol_map_mtx);
    g_id_to_sym.clear();
    for (int i = 0; i < OMEGA_NSYMS; ++i)
        g_id_to_sym[OMEGA_SYMS[i].id] = OMEGA_SYMS[i].name;
    for (const auto& e : g_ext_syms)
        if (e.id > 0) g_id_to_sym[e.id] = e.name;
}

// =============================================================================
// L2Book -- up to 5 levels per symbol
// Fed from cTrader Open API ProtoOADepthEvent (real multi-level) when active,
// falls back to FIX 264=1 single-level when cTrader feed is not connected.
// All methods degrade gracefully when size data is absent (no false blocks).
//
// ── MICROSTRUCTURE SIGNAL SUITE ─────────────────────────────────────────────
// Stateless signals (only current snapshot needed):
//   imbalance()          -- bid_vol / (bid+ask) vol, 0=ask-heavy, 1=bid-heavy
//   imbalance_level()    -- bid_count / (bid+ask count): level-count imbalance.
//                          PRIMARY signal for BlackBull (size_raw=0 always).
//                          bid_count=3, ask_count=2 → 0.60 (mild bid pressure).
//                          More levels active on a side = stronger conviction.
//   ratio3()             -- bid/ask ratio top-3 levels: >1.5 strong bid
//   microprice()         -- (bid×ask_sz + ask×bid_sz) / (bid_sz+ask_sz)
//                          predicts next tick direction vs mid
//   book_slope()         -- weighted directional bias: +ve=buy pressure
//   wall_above(mid)      -- largest ask level > 4× avg → resistance ceiling
//   wall_below(mid)      -- largest bid level > 4× avg → support floor
//   liquidity_vacuum_ask()-- top-3 ask vol thin → upward impulse likely
//   liquidity_vacuum_bid()-- top-3 bid vol thin → downward impulse likely
//   depth_supports_long/short() -- enough liquidity to fill our size
//   is_sweep_long/short()       -- tick volume exceeds L1 size (FIX only)
//
// Stateful signals (need previous snapshot -- in CTDepthBook, CTraderDepthClient.hpp):
//   queue_pull_up()      -- ask L1 shrank >50% → sellers pulled → up impulse
//   queue_pull_down()    -- bid L1 shrank >50% → buyers pulled → down impulse
//   pull_ratio()         -- total book volume shrank >35% → thin book alert
// =============================================================================
struct L2Level { double price = 0.0; double size = 0.0; };

struct L2Book {
    L2Level bids[5];
    L2Level asks[5];
    int     bid_count  = 0;
    int     ask_count  = 0;

    // ── Imbalance 0..1 (volume-based) ───────────────────────────────────────
    // bid_vol / (bid_vol + ask_vol) across top `levels` levels.
    // 0.5 = balanced | >0.65 = bid-heavy | <0.35 = ask-heavy
    // NOTE: BlackBull sends size_raw=0 for XAUUSD depth quotes. The parse path
    // substitutes 1 lot per level, making bid_total == ask_total == 0.500 always.
    // Use imbalance_level() instead for BlackBull feeds. This method remains
    // correct for brokers that send real size data.
    double imbalance(int levels = 5) const noexcept {
        double bs = 0.0, as = 0.0;
        const int bn = std::min(bid_count, levels);
        const int an = std::min(ask_count, levels);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        for (int i = 0; i < an; ++i) as += asks[i].size;
        const double tot = bs + as;
        return (tot > 0.0) ? (bs / tot) : 0.5;
    }

    // ── Imbalance 0..1 (level-count-based) ──────────────────────────────────
    // bid_count / (bid_count + ask_count) across up to `levels` levels.
    //
    // This is the PRIMARY imbalance signal for BlackBull Markets, which sends
    // size_raw=0 on all XAUUSD depth quotes. Since we cannot rely on volume,
    // we instead count how many price levels are active on each side. More
    // active bid levels than ask levels indicates buy-side DOM pressure, and
    // vice versa. The signal is weaker than true volume imbalance but is the
    // best available signal from this broker.
    //
    // Examples (max 5 levels per side):
    //   bid=4, ask=1 → 4/5 = 0.80  (strong bid pressure → long signal)
    //   bid=1, ask=4 → 1/5 = 0.20  (strong ask pressure → short signal)
    //   bid=3, ask=2 → 3/5 = 0.60  (mild bid pressure)
    //   bid=2, ask=3 → 2/5 = 0.40  (mild ask pressure)
    //   bid=3, ask=3 → 3/6 = 0.50  (neutral)
    //
    // GFE_LONG_THRESHOLD=0.75, GFE_SHORT_THRESHOLD=0.25 (in GoldFlowEngine.hpp).
    // For a LONG signal: need bid_count > ask_count by at least 4:1 (e.g. 4 bid, 1 ask).
    // For a SHORT signal: need ask_count > bid_count by at least 4:1 (e.g. 1 bid, 4 ask).
    // Neutral (0.40–0.60) = no trade from GFE alone; drift must override.
    double imbalance_level(int levels = 5) const noexcept {
        const int bn = std::min(bid_count, levels);
        const int an = std::min(ask_count, levels);
        const int tot = bn + an;
        if (tot == 0) return 0.5;
        return static_cast<double>(bn) / static_cast<double>(tot);
    }

    // ── 3-level bid/ask ratio ────────────────────────────────────────────────
    // >1.5 = strong bid pressure (long-friendly)
    // <0.67 = strong ask pressure (short-friendly)
    double ratio3() const noexcept {
        double bs = 0.0, as = 0.0;
        const int bn = std::min(bid_count, 3);
        const int an = std::min(ask_count, 3);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        for (int i = 0; i < an; ++i) as += asks[i].size;
        if (as <= 0.0) return (bs > 0.0) ? 9.9 : 1.0;
        return bs / as;
    }

    // ── Microprice ───────────────────────────────────────────────────────────
    // Weighted midpoint that accounts for L1 queue sizes.
    // microprice > mid → upward pressure (buyers have smaller queue to exhaust)
    // microprice < mid → downward pressure
    // Returns mid (best_bid+best_ask)*0.5 when sizes are 0 (graceful fallback).
    double microprice() const noexcept {
        if (bid_count == 0 || ask_count == 0) return 0.0;
        const double bp = bids[0].price, bs = bids[0].size;
        const double ap = asks[0].price, as_ = asks[0].size;
        const double tot = bs + as_;
        if (tot <= 0.0) return (bp + ap) * 0.5;
        return (bp * as_ + ap * bs) / tot;
    }

    // Microprice - mid: positive = upward pressure, negative = downward
    double microprice_bias() const noexcept {
        if (bid_count == 0 || ask_count == 0) return 0.0;
        const double mid = (bids[0].price + asks[0].price) * 0.5;
        return microprice() - mid;
    }

    // ── Book slope ───────────────────────────────────────────────────────────
    // Weighted sum across all levels: bid pressure - ask pressure.
    // Each level weighted by 1/(1+distance_from_mid) so near levels count more.
    // Positive = buy pressure building; negative = sell pressure building.
    // Range is roughly -1..+1; use >0.15 as meaningful directional signal.
    double book_slope() const noexcept {
        if (bid_count == 0 || ask_count == 0) return 0.0;
        const double mid = (bids[0].price + asks[0].price) * 0.5;
        if (mid <= 0.0) return 0.0;
        double bid_wt = 0.0, ask_wt = 0.0, total = 0.0;
        for (int i = 0; i < bid_count; ++i) {
            const double dist = std::fabs(mid - bids[i].price) / mid;
            const double w = 1.0 / (1.0 + dist * 100.0);  // decay by % distance
            bid_wt += bids[i].size * w;
            total  += bids[i].size * w;
        }
        for (int i = 0; i < ask_count; ++i) {
            const double dist = std::fabs(asks[i].price - mid) / mid;
            const double w = 1.0 / (1.0 + dist * 100.0);
            ask_wt += asks[i].size * w;
            total  += asks[i].size * w;
        }
        if (total <= 0.0) return 0.0;
        return (bid_wt - ask_wt) / total;  // -1..+1
    }

    // ── Liquidity vacuum ─────────────────────────────────────────────────────
    // True when top-3 levels on a side are very thin -- price can move fast.
    // threshold: fraction of average total book volume per level.
    // Default 0.15 = top-3 ask is <15% of average → vacuum on ask side → up impulse.
    // When sizes are 0 (no data), always returns false (never triggers falsely).
    bool liquidity_vacuum_ask(double threshold = 0.15) const noexcept {
        if (ask_count == 0) return false;
        double ask3 = 0.0;
        const int an = std::min(ask_count, 3);
        for (int i = 0; i < an; ++i) ask3 += asks[i].size;
        if (ask3 <= 0.0) return false;
        double total = 0.0;
        for (int i = 0; i < bid_count; ++i) total += bids[i].size;
        for (int i = 0; i < ask_count; ++i) total += asks[i].size;
        const int levels = bid_count + ask_count;
        if (levels == 0 || total <= 0.0) return false;
        const double avg_per_level = total / levels;
        return (ask3 / 3.0) < (avg_per_level * threshold);
    }
    bool liquidity_vacuum_bid(double threshold = 0.15) const noexcept {
        if (bid_count == 0) return false;
        double bid3 = 0.0;
        const int bn = std::min(bid_count, 3);
        for (int i = 0; i < bn; ++i) bid3 += bids[i].size;
        if (bid3 <= 0.0) return false;
        double total = 0.0;
        for (int i = 0; i < bid_count; ++i) total += bids[i].size;
        for (int i = 0; i < ask_count; ++i) total += asks[i].size;
        const int levels = bid_count + ask_count;
        if (levels == 0 || total <= 0.0) return false;
        const double avg_per_level = total / levels;
        return (bid3 / 3.0) < (avg_per_level * threshold);
    }

    // ── Liquidity wall detection ─────────────────────────────────────────────
    // True when a single level holds > wall_mult × average level size.
    // wall_above(mid): resistance ceiling above current price (ask side)
    // wall_below(mid): support floor below current price (bid side)
    // Default wall_mult=4.0 per microstructure literature.
    bool wall_above(double mid, double wall_mult = 4.0) const noexcept {
        if (ask_count == 0) return false;
        double total = 0.0;
        for (int i = 0; i < ask_count; ++i) total += asks[i].size;
        if (total <= 0.0) return false;
        const double avg = total / ask_count;
        for (int i = 0; i < ask_count; ++i)
            if (asks[i].price > mid && asks[i].size > avg * wall_mult) return true;
        return false;
    }
    bool wall_below(double mid, double wall_mult = 4.0) const noexcept {
        if (bid_count == 0) return false;
        double total = 0.0;
        for (int i = 0; i < bid_count; ++i) total += bids[i].size;
        if (total <= 0.0) return false;
        const double avg = total / bid_count;
        for (int i = 0; i < bid_count; ++i)
            if (bids[i].price < mid && bids[i].size > avg * wall_mult) return true;
        return false;
    }

    // ── Best wall: largest single level on a side (0=bid, 1=ask) ────────────
    double wall_size(int side) const noexcept {
        double mx = 0.0;
        if (side == 0) { for (int i=0;i<bid_count&&i<5;++i) if(bids[i].size>mx) mx=bids[i].size; }
        else           { for (int i=0;i<ask_count&&i<5;++i) if(asks[i].size>mx) mx=asks[i].size; }
        return mx;
    }

    // ── Depth support: enough liquidity to absorb our position ───────────────
    // Returns true if top-3 bid/ask sum >= position_lots * cushion.
    // When sizes are 0 (no depth data), always returns true -- never blocks.
    bool depth_supports_long(double position_lots, double cushion = 3.0) const noexcept {
        double bs = 0.0;
        const int bn = std::min(bid_count, 3);
        for (int i = 0; i < bn; ++i) bs += bids[i].size;
        if (bs <= 0.0) return true;
        return bs >= (position_lots * cushion);
    }
    bool depth_supports_short(double position_lots, double cushion = 3.0) const noexcept {
        double as = 0.0;
        const int an = std::min(ask_count, 3);
        for (int i = 0; i < an; ++i) as += asks[i].size;
        if (as <= 0.0) return true;
        return as >= (position_lots * cushion);
    }

    // ── Sweep detection ──────────────────────────────────────────────────────
    // True when last tick volume exceeds level-1 size * 1.5 AND imbalance
    // confirms the direction. Used with FIX tag-271 tick volume.
    bool is_sweep_long(double sweep_vol) const noexcept {
        if (sweep_vol <= 0.0) return false;
        const double l1_ask = (ask_count > 0) ? asks[0].size : 0.0;
        if (l1_ask <= 0.0) return false;
        return (sweep_vol > l1_ask * 1.5) && (imbalance() > 0.55);
    }
    bool is_sweep_short(double sweep_vol) const noexcept {
        if (sweep_vol <= 0.0) return false;
        const double l1_bid = (bid_count > 0) ? bids[0].size : 0.0;
        if (l1_bid <= 0.0) return false;
        return (sweep_vol > l1_bid * 1.5) && (imbalance() < 0.45);
    }

    int depth_levels() const noexcept { return std::max(bid_count, ask_count); }
    bool has_data()    const noexcept { return bid_count > 0 && ask_count > 0 &&
                                               bids[0].size > 0.0 && asks[0].size > 0.0; }
};
