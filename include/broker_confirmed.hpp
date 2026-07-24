#pragma once
// =============================================================================
// broker_confirmed.hpp — BROKER-FILL GATE for the live-trades display (2026-07-24)
//
// WHY: the live-trades panel / TRADING neon / index tiles were driven by ENGINE
// INTENT — an engine sets a position "open" the moment it DECIDES to enter, and
// push_live_trade() rendered it whether or not the BROKER ever filled. Result: a
// desk full of "LIVE OPEN TRADES" while IBKR held ZERO positions (the phantom
// class: AAPL/INTC StockDip shown trading, broker flat). Operator, repeatedly and
// furiously: a position is REAL only with a broker execution report (execDetails /
// posted_exec>0), never on connection/intent (memory feedback-real-trade-broker-
// evidence). This gate enforces that rule at the DISPLAY chokepoint.
//
// WHAT: IbkrExecutionEngine::execDetails (the ONLY place a real broker fill is
// reported) records the signed net filled qty per symbol here. push_live_trade()
// then shows a position ONLY when the broker actually holds it. Zero real fills =>
// FLAT desk (the honest state). A real fill => that symbol shows. This is the
// display-side twin of the per-order SIZE cap: both make the GUI/exec tell the truth.
//
// SCOPE: IBKR instruments only (FUT/STK/CASH). Crypto positions reach the Omega
// desk via a SEPARATE live_mirror feed (real Binance fills), NOT live_trades, so
// they are unaffected by this gate. In-memory live_trades[] IS also read by the
// companion clip logic — gating to broker-confirmed makes clip-banking fire on
// REAL positions only, which is strictly more correct (phantom-clip banking was
// its own bug class, project-companion-phantom-realized-flap).
//
// ESCAPE HATCH: env OMEGA_SHOW_UNCONFIRMED=1 disables the gate (shows intent again)
// for debugging a symbol-mapping miss — without a rebuild.
// =============================================================================
#include <string>
#include <map>
#include <set>
#include <mutex>
#include <cmath>
#include <cctype>
#include <cstdlib>

namespace omega {

// Canonical symbol key: uppercase, strip venue suffixes, fold index aliases so the
// broker-fill record (exec omega_symbol, e.g. "NAS100") and the display sym (e.g.
// "USTEC.F") reconcile to ONE key. Stocks are identity (AAPL==AAPL) — exact match.
inline std::string canonical_sym(std::string s) {
    for (auto& c : s) c = (char)std::toupper((unsigned char)c);
    for (const char* suf : {".FUTURES", ".F", ".M"}) {
        const std::string sfx(suf);
        if (s.size() > sfx.size() &&
            s.compare(s.size() - sfx.size(), sfx.size(), sfx) == 0) {
            s.erase(s.size() - sfx.size());
            break;
        }
    }
    if (s == "NAS100" || s == "USTEC" || s == "MNQ" || s == "NQ")            return "NQ";
    if (s == "US500"  || s == "SPX"   || s == "SPX500" || s == "ES" || s == "MES") return "ES";
    if (s == "DJ30"   || s == "US30"  || s == "YM"  || s == "MYM")           return "YM";
    if (s == "XAUUSD" || s == "MGC"   || s == "GC")                          return "GOLD";
    return s;
}

struct BrokerConfirmedPositions {
    std::mutex m;
    std::map<std::string, double> net;   // canonical sym -> signed net filled qty (broker truth)

    // Called from IbkrExecutionEngine::execDetails on EVERY real broker fill.
    void on_fill(const std::string& omega_sym, const std::string& side, double qty) {
        const double signed_q = (side == "BOT") ? qty : -qty;
        std::lock_guard<std::mutex> lk(m);
        const std::string k = canonical_sym(omega_sym);
        net[k] += signed_q;
        if (std::fabs(net[k]) < 1e-9) net.erase(k);   // flat again -> drop
    }

    // ── AUTHORITATIVE broker snapshot (reqPositions -> position()* -> positionEnd()).
    //    Rebuild pattern: begin_snapshot() clears staging, stage() adds each reported
    //    position, commit_snapshot() atomically REPLACES net with the staged truth.
    //    Full-replace is required — reqPositions does NOT report a now-flat symbol, so
    //    a per-symbol overwrite would leave a stale phantom; replacing the whole map
    //    drops any symbol the broker no longer holds. Between snapshots on_fill() keeps
    //    net fresh incrementally; the next snapshot corrects any drift (survives
    //    restart / missed fills). signed_qty>0 long, <0 short.
    std::map<std::string, double> staging;
    std::map<std::string, int>    absent_streak_;   // canonical sym -> consecutive snapshots MISSING
    static constexpr int          DROP_AFTER = 3;    // survive an incomplete reqPositions before dropping
    void begin_snapshot() { std::lock_guard<std::mutex> lk(m); staging.clear(); }
    void stage(const std::string& omega_sym, double signed_qty) {
        if (std::fabs(signed_qty) < 1e-9) return;
        std::lock_guard<std::mutex> lk(m);
        staging[canonical_sym(omega_sym)] = signed_qty;
    }
    // Commit the reqPositions snapshot, but DEBOUNCE drops: a symbol held in the previous net
    // that this snapshot did NOT report is CARRIED FORWARD for up to DROP_AFTER consecutive
    // snapshots before being dropped. reqPositions can return an INCOMPLETE set (the BMY
    // incident: broker-holds bounced 8->7->8, vanishing a real held position from the desk AND
    // cancelling its native stop via positionEnd cancel-on-flat). One incomplete snapshot must
    // not vaporize a held position; a genuine close (absent DROP_AFTER times) still drops.
    void commit_snapshot() {
        std::lock_guard<std::mutex> lk(m);
        std::set<std::string> reported;                 // what reqPositions ACTUALLY reported
        for (auto& kv : staging) reported.insert(kv.first);
        for (auto& kv : net) {
            if (reported.count(kv.first)) { absent_streak_[kv.first] = 0; continue; }  // reported -> fresh
            if (++absent_streak_[kv.first] < DROP_AFTER) staging[kv.first] = kv.second; // carry forward
            // else: absent >= DROP_AFTER consecutive snapshots -> let it drop (genuine close)
        }
        for (const auto& s : reported) absent_streak_[s] = 0;   // newly/again reported -> reset
        net.swap(staging); staging.clear();
        for (auto it = absent_streak_.begin(); it != absent_streak_.end();)  // prune dead streaks
            (net.count(it->first) ? ++it : it = absent_streak_.erase(it));
    }

    // True only when the broker actually holds a net position in this symbol.
    bool holds(const std::string& sym) {
        std::lock_guard<std::mutex> lk(m);
        auto it = net.find(canonical_sym(sym));
        return it != net.end() && std::fabs(it->second) > 1e-9;
    }

    // Signed net broker qty for a symbol (>0 long, <0 short, 0 flat). For a safe
    // targeted close: SELL/BUY exactly what the broker holds (never naked).
    double net_qty(const std::string& sym) {
        std::lock_guard<std::mutex> lk(m);
        auto it = net.find(canonical_sym(sym));
        return it == net.end() ? 0.0 : it->second;
    }

    // Distinct broker-confirmed open symbols (book-wide exposure count, gap 4).
    size_t count() {
        std::lock_guard<std::mutex> lk(m);
        return net.size();
    }

    // Copy of the current broker-held positions (canonical sym -> signed qty). Used to
    // place a native disaster stop on every held position.
    std::map<std::string, double> all() {
        std::lock_guard<std::mutex> lk(m);
        return net;
    }
};

inline BrokerConfirmedPositions g_broker_confirmed;

// Display gate: when true (default), the live-trades panel shows ONLY broker-
// confirmed positions — never engine intent. Ends the phantom display. Env
// OMEGA_SHOW_UNCONFIRMED=1 flips it OFF (debug a mapping miss without a rebuild).
inline bool display_broker_confirmed_only() {
    static const bool v = (std::getenv("OMEGA_SHOW_UNCONFIRMED") == nullptr);
    return v;
}

}  // namespace omega
