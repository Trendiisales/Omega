#pragma once
// =============================================================================
// StallCompanion — native C++ port of the Mac-cron stall_accountant.py zoo.
//
// REPLACES ~/stall-accountant/stall_accountant.py (+ companion_aggregate.py) — the
// ~25 live Omega gold/index giveback-clip "stall-accountant" PROTECTION books that
// ran as separate cron lines on the Mac, HTTP-polling the VPS /api/telemetry. Ported
// into Omega.exe as ONE engine class parametrised by a Config, fed IN-MEMORY from the
// live_trades[] snapshot the telemetry writer already rebuilds every 250ms. No ssh, no
// HTTP self-poll — cleaner than the python and native on the box (operator: NO python
// in the live system). Mirrors the GoldBeFloorCompanion port precedent (C++ in the
// trading binary + own state file + own /api endpoint), but this is a PARENT-MIRROR
// STREAMING engine (tracks each real leg's live MFE), not a self-detecting batch book.
//
// HARD OPERATOR RULE — SEPARATE INDEPENDENT ENGINE (feedback-companion-independent-
// engine). Observe-only paper shadow: the real engine trade rides WIDE untouched; each
// StallBook opens a companion mirroring the same entry/side and BANKS it on an early
// exit into its OWN book. It NEVER opens / moves / shrinks / closes a real position and
// is NEVER read by any parent. Judge STANDALONE / additive — never vs riding WIDE.
//
// ADVERSE-PROTECTION: LOSS_CUT_CLIP — every book carries a cold-loss cut (COLD_LOSS_USD,
//   default OMEGA -$50) closing the companion when floating P&L drops below threshold,
//   independent of armed state. Backtested per-book verdicts live in the cron comments
//   the configs were ported from (see engine_init registry + [[GivebackLadderSweep]] /
//   [[ReclipRetrigSweep]] / [[DollarTrailCompanion]]). Faithful port of the validated
//   python; no NEW clip logic introduced.
//
// FAITHFULNESS: byte-exact port of stall_accountant.py main() per-instance state
//   machine + companion_aggregate.py merge. Parity is validated by REPLAYING a recorded
//   telemetry sequence through both python main() and this engine (a live mirror can
//   never be byte-identical to the python's own live poll — different observation
//   instants see different marks). companion_closed.csv is written byte-compatibly
//   (same header + QUOTE_MINIMAL rows) so ledgers migrate + continue across the cutover.
// =============================================================================
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <algorithm>

namespace omega {

// One live open trade as seen in the VPS live_trades[] snapshot (== python poll_omega row).
struct StallLiveRow {
    std::string book;      // "OMEGA" (VPS) — crypto legs handled by the separate crypto binary
    std::string eng;       // engine tag
    std::string sym;       // symbol
    std::string side;      // "LONG"/"SHORT"
    double entry   = 0.0;
    double current = 0.0;
    double upnl    = 0.0;
};

// ── roll-up aggregates (shared by StallBook + the aggregate merge) ────────────
struct StallEngAgg { int open = 0, closed = 0; double realized = 0.0; };
struct StallLegAgg { std::string engine, entry, book; int open = 0, closed = 0; double realized = 0.0; };
struct StallOpenDet {
    std::string book, eng, sym, side;
    double entry = 0.0, mfe_pct = 0.0, mfe_usd = 0.0, upnl = 0.0;
    int    stall = 0;
    bool   eligible = false;
};
struct StallRollUp {
    std::string name, gauge, updated;
    double arm_usd = 0.0, trail_usd = 0.0, retrig_usd = 0.0, gate_pct = 0.0, rev_gb = 0.0;
    int    stall_bars = 0, open_companions = 0;
    double realized_total = 0, realized_today = 0, realized_7d = 0, realized_30d = 0;
    double crypto_realized = 0;
    std::map<std::string, double>      by_reason;
    std::map<std::string, double>      crypto_by_reason;
    std::map<std::string, StallEngAgg> per_engine;
    std::map<std::string, StallLegAgg> per_leg;
    std::vector<StallOpenDet>          open_detail;
};

namespace stall_detail {
    inline double round4(double v) { return std::floor(v * 1e4 + 0.5) / 1e4; }
    inline double round2(double v) { return std::floor(v * 1e2 + 0.5) / 1e2; }
    inline void jstr(std::ostringstream& o, const std::string& s) {
        o << '"';
        for (char ch : s) { if (ch == '"' || ch == '\\') o << '\\'; o << ch; }
        o << '"';
    }
    // Format like python str(round(v, prec)): fixed to `prec` places then strip trailing
    // zeros, KEEPING one digit after the point (python 3300.0 -> "3300.0", not "3300").
    // Byte-compatible with the python csv.writer rows so migrated ledgers stay consistent.
    inline std::string pyfloat(double v, int prec) {
        char b[40]; std::snprintf(b, sizeof(b), "%.*f", prec, v);
        std::string s = b;
        const size_t dot = s.find('.');
        if (dot == std::string::npos) return s;
        size_t last = s.find_last_not_of('0');
        if (last == dot) ++last;                     // keep exactly one zero after '.'
        s.erase(last + 1);
        return s;
    }
    inline std::string fmtnum(double v) { return pyfloat(v, 4); }   // entry price (python round(entry,4))
    inline std::string updated_utc(int64_t now) {
        std::time_t t = (std::time_t)now; std::tm tmv{};
#if defined(_WIN32)
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        char b[40]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d UTC",
            tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
        return b;
    }
    // CSV parse honouring QUOTE_MINIMAL (a quoted symbol field may contain commas).
    inline std::vector<std::string> csvparse(const std::string& line) {
        std::vector<std::string> out; std::string cur; bool q = false;
        for (size_t i = 0; i < line.size(); ++i) {
            char ch = line[i];
            if (q) {
                if (ch == '"') { if (i + 1 < line.size() && line[i+1] == '"') { cur += '"'; ++i; } else q = false; }
                else cur += ch;
            } else {
                if (ch == '"') q = true;
                else if (ch == ',') { out.push_back(cur); cur.clear(); }
                else if (ch == '\r') {}
                else cur += ch;
            }
        }
        out.push_back(cur);
        return out;
    }
}

// ── one cron instance (one companion book) ───────────────────────────────────
class StallBook {
public:
    struct Config {
        std::string name = "main";                 // book dir basename ("main" for the shared root book)
        std::vector<std::string> include;          // COMPANION_INCLUDE (empty = keep all non-excluded)
        std::vector<std::string> exclude;          // COMPANION_EXCLUDE
        int    stall_bars   = 2;                   // N_STALL
        double gate_pct     = 1.5;                 // GATE_PCT
        int    tf_sec       = 4 * 3600;            // STALL_TF_HOURS * 3600
        double rev_gb       = 0.05;                // REVERSAL_GIVEBACK (fraction of peak)
        double rev_gb_pts   = 0.0;                 // REVERSAL_GIVEBACK_PTS (absolute pct-points)
        double retrig_pct   = 0.05;                // COMPANION_RETRIG_PCT
        double arm_usd      = 0.0;                 // STALL_GATE_USD (>0 => USD gauge)
        double trail_usd    = 0.0;                 // TRAIL_USD
        double retrig_usd   = 0.0;                 // COMPANION_RETRIG_USD
        bool   bull_only    = false;               // COMPANION_BULL_ONLY
        double cold_loss_omega  = -50.0;           // COLD_LOSS_USD_OMEGA
        double cold_loss_crypto = -15.0;           // COLD_LOSS_USD_CRYPTO
        std::string dir;                           // working dir (relative, e.g. "stall/xau_tf4h_usd_a")
    };

    explicit StallBook(Config c) : cfg_(std::move(c)) {
        usd_mode_ = cfg_.arm_usd > 0.0;
        { std::error_code ec; std::filesystem::create_directories(cfg_.dir, ec); }   // never silent-die on a missing state dir
        closed_path_  = cfg_.dir + "/companion_closed.csv";
        state_path_   = cfg_.dir + "/companion_state.json";
        pos_path_     = cfg_.dir + "/companion_positions.tsv";     // C++-owned persistence (not python JSON)
        clipped_path_ = cfg_.dir + "/companion_clipped.tsv";
        load_pos_();
    }

    const Config& config() const { return cfg_; }
    const std::string& name() const { return cfg_.name; }
    const StallRollUp& rollup() const { return roll_; }

    // ── one cycle. rows_all = ALL live_trades (unfiltered), for the omega_empty guard;
    //    filtering is done inside. gold_bull: 1 up / 0 down / -1 unknown (computed once
    //    per drive cycle by the registry). now = epoch seconds. ──
    void step(const std::vector<StallLiveRow>& rows_all, int gold_bull, int64_t now) {
        using namespace stall_detail;
        // EMPTY-OMEGA GUARD (faithful to python): a totally flat VPS book (0 live trades)
        // is indistinguishable from a restart blip — SKIP the harvest entirely (no bank,
        // no ENGINE_EXIT prune, pos+clipped preserved), then still write state (heartbeat).
        const bool omega_empty = rows_all.empty();
        if (!omega_empty) {
            const int64_t bar = now / cfg_.tf_sec;
            std::map<std::string, double> live;   // key -> last upnl
            for (const auto& r : rows_all) {
                if (excluded_(r.eng) || !included_(r.eng, r.sym)) continue;
                const std::string key = mkkey_(r.book, r.eng, r.sym, r.entry);
                if (r.current <= 0.0 || r.entry <= 0.0) {          // no live mark yet -> no garbage MFE
                    auto it = pos_.find(key);
                    if (it != pos_.end()) live[key] = it->second.last_upnl;
                    continue;
                }
                live[key] = r.upnl;
                const bool is_long = (!r.side.empty() && (r.side[0] == 'L' || r.side[0] == 'l'));
                const double fav     = ((is_long ? (r.current - r.entry) : (r.entry - r.current)) / r.entry) * 100.0;
                const double fav_usd =   is_long ? (r.current - r.entry) : (r.entry - r.current);

                auto cit = clipped_.find(key);
                if (cit != clipped_.end()) {
                    const double prior_peak = cit->second;
                    bool retrig = false;
                    if (usd_mode_) retrig = (cfg_.retrig_usd > 0.0 && prior_peak > 0.0 && fav_usd > prior_peak + cfg_.retrig_usd);
                    else           retrig = (cfg_.retrig_pct > 0.0 && prior_peak > 0.0 && fav     > prior_peak * (1.0 + cfg_.retrig_pct));
                    if (retrig) clipped_.erase(cit);
                    else        continue;
                }

                if (pos_.find(key) == pos_.end()) {
                    // BULL-REGIME GATE: only OPEN a new gold companion while the 4h trend is up.
                    // Never blocks an already-open companion. FAIL-SAFE: unknown regime -> skip open.
                    if (cfg_.bull_only && is_gold_(r.sym) && gold_bull != 1) continue;
                    Pos p;
                    p.book = r.book; p.eng = r.eng; p.sym = r.sym; p.side = r.side;
                    p.entry = round4(r.entry);
                    p.open_bar = bar; p.mfe_pct = fav; p.mfe_usd = fav_usd; p.ext_bar = bar;
                    p.last_upnl = r.upnl;
                    pos_[key] = p;
                }
                Pos& p = pos_[key];
                const bool new_extreme = usd_mode_ ? (fav_usd > p.mfe_usd + 1e-9) : (fav > p.mfe_pct + 1e-9);
                if (fav     > p.mfe_pct + 1e-9) p.mfe_pct = fav;
                if (fav_usd > p.mfe_usd + 1e-9) p.mfe_usd = fav_usd;
                if (new_extreme) p.ext_bar = bar;
                p.stall     = (int)(bar - p.ext_bar);
                p.last_upnl = r.upnl;
                const double peak_store = usd_mode_ ? p.mfe_usd : p.mfe_pct;
                const bool armed = usd_mode_ ? (p.mfe_usd >= cfg_.arm_usd) : (p.mfe_pct >= cfg_.gate_pct);

                if (armed && p.stall >= cfg_.stall_bars) { close_(key, "STALL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                if (usd_mode_) {
                    if (armed && cfg_.trail_usd > 0.0 && fav_usd <= p.mfe_usd - cfg_.trail_usd) { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                } else {
                    if (armed && fav <= p.mfe_pct * (1.0 - cfg_.rev_gb))                     { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                    if (armed && cfg_.rev_gb_pts > 0.0 && fav <= p.mfe_pct - cfg_.rev_gb_pts) { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                }
                const double cold = (r.book == "CRYPTO") ? cfg_.cold_loss_crypto : cfg_.cold_loss_omega;
                if (r.upnl <= cold) { close_(key, "LOSS_CUT_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
            }
            // real trade closed first -> ENGINE_EXIT any open companion whose key left `live`
            std::vector<std::string> gone;
            for (const auto& kv : pos_) if (live.find(kv.first) == live.end()) gone.push_back(kv.first);
            for (const auto& key : gone) close_(key, "ENGINE_EXIT", pos_[key].last_upnl, bar);
            // a clip stays clipped only while its real trade is live; drop once the engine actually closes it
            for (auto it = clipped_.begin(); it != clipped_.end();) {
                if (live.find(it->first) == live.end()) it = clipped_.erase(it); else ++it;
            }
        }
        save_pos_();
        roll_ = compute_rollup_(now);
        write_state_();
    }

private:
    struct Pos {
        std::string book, eng, sym, side;
        double entry = 0.0;
        int64_t open_bar = 0, ext_bar = 0;
        double mfe_pct = 0.0, mfe_usd = 0.0;
        int    stall = 0;
        double last_upnl = 0.0;
    };

    Config cfg_;
    bool usd_mode_ = false;
    std::string closed_path_, state_path_, pos_path_, clipped_path_;
    std::unordered_map<std::string, Pos>    pos_;
    std::unordered_map<std::string, double> clipped_;
    StallRollUp roll_;

    std::string mkkey_(const std::string& book, const std::string& eng,
                       const std::string& sym, double entry) const {
        // Faithful to python `f"{book}|{eng}|{sym}|{round(entry,4)}"`: str(round(x,4)) STRIPS
        // trailing zeros (73.43 -> "73.43", NOT "%.4f"->"73.4300"). The %.4f form orphaned
        // seed-migrated python positions on the crypto cutover (key never matched the live
        // harvest -> spurious ENGINE_EXIT + reopen); backported from the crypto copy. Old
        // %.4f-keyed .tsv state migrates transparently: load_pos_ rebuilds keys from fields.
        return book + "|" + eng + "|" + sym + "|" + stall_detail::pyfloat(stall_detail::round4(entry), 4);
    }
    static bool icontains_(const std::string& hay, const std::string& needle) {
        if (needle.empty()) return false;
        std::string h = hay, n = needle;
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return h.find(n) != std::string::npos;
    }
    bool excluded_(const std::string& eng) const {
        for (const auto& x : cfg_.exclude) if (icontains_(eng, x)) return true;
        return false;
    }
    bool included_(const std::string& eng, const std::string& sym) const {
        if (cfg_.include.empty()) return true;
        const std::string hay = eng + " " + sym;
        for (const auto& x : cfg_.include) if (icontains_(hay, x)) return true;
        return false;
    }
    static bool is_gold_(const std::string& sym) {
        std::string s = sym; std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return s.find("XAU") != std::string::npos || s.find("GOLD") != std::string::npos
            || s.find("MGC") != std::string::npos || s.find("GC") != std::string::npos;
    }

    // ── append a bank row to companion_closed.csv (byte-compatible w/ python csv.writer) ──
    void close_(const std::string& key, const char* reason, double pnl, int64_t bar) {
        using namespace stall_detail;
        auto it = pos_.find(key);
        if (it == pos_.end()) return;
        const Pos& p = it->second;
        const bool need_header = !file_nonempty_(closed_path_);
        std::ofstream f(closed_path_, std::ios::app);
        if (f.is_open()) {
            if (need_header) f << "ts,book,reason,engine,symbol,side,entry,realized_pnl,mfe_peak_pct,bars_held\n";
            f << (long long)std::time(nullptr) << ',' << p.book << ',' << reason << ','
              << csvq_(p.eng) << ',' << csvq_(p.sym) << ',' << p.side << ',' << fmtnum(p.entry)
              << ',' << pyfloat(round2(pnl), 2) << ',' << pyfloat(round2(p.mfe_pct), 2) << ','
              << (bar - p.open_bar) << '\n';
        }
        pos_.erase(it);
    }
    static bool file_nonempty_(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        return f.is_open() && f.tellg() > 0;
    }
    static std::string csvq_(const std::string& s) {   // QUOTE_MINIMAL
        if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
        std::string out = "\"";
        for (char ch : s) { if (ch == '"') out += '"'; out += ch; }
        out += '"';
        return out;
    }

    // ── persistence (C++-owned .tsv; survives binary restart, not python-compatible) ──
    void save_pos_() const {
        std::ofstream f(pos_path_, std::ios::trunc);
        if (f.is_open())
            for (const auto& kv : pos_) {
                const Pos& p = kv.second;
                f << kv.first << '\t' << p.book << '\t' << p.eng << '\t' << p.sym << '\t' << p.side
                  << '\t' << p.entry << '\t' << p.open_bar << '\t' << p.ext_bar << '\t' << p.mfe_pct
                  << '\t' << p.mfe_usd << '\t' << p.stall << '\t' << p.last_upnl << '\n';
            }
        std::ofstream g(clipped_path_, std::ios::trunc);
        if (g.is_open()) for (const auto& kv : clipped_) g << kv.first << '\t' << kv.second << '\n';
    }
    void load_pos_() {
        std::ifstream f(pos_path_);
        std::string line;
        while (std::getline(f, line)) {
            std::vector<std::string> t = split_(line, '\t');
            if (t.size() < 12) continue;
            Pos p;
            p.book = t[1]; p.eng = t[2]; p.sym = t[3]; p.side = t[4];
            p.entry = std::atof(t[5].c_str()); p.open_bar = std::atoll(t[6].c_str());
            p.ext_bar = std::atoll(t[7].c_str()); p.mfe_pct = std::atof(t[8].c_str());
            p.mfe_usd = std::atof(t[9].c_str()); p.stall = std::atoi(t[10].c_str());
            p.last_upnl = std::atof(t[11].c_str());
            // Rebuild the key from the row's own fields instead of trusting t[0]: migrates old
            // "%.4f"-keyed rows to the pyfloat key format in place (no spurious ENGINE_EXIT/reopen
            // churn on the first post-upgrade harvest).
            pos_[mkkey_(p.book, p.eng, p.sym, p.entry)] = p;
        }
        std::ifstream g(clipped_path_);
        while (std::getline(g, line)) {
            std::vector<std::string> t = split_(line, '\t');
            if (t.size() < 2) continue;
            clipped_[normalize_key_(t[0])] = std::atof(t[1].c_str());
        }
    }
    // Reformat an old "%.4f"-entry key ("...|73.4300") to the pyfloat form ("...|73.43").
    // Keys already in pyfloat form pass through unchanged.
    std::string normalize_key_(const std::string& key) const {
        const size_t bar = key.rfind('|');
        if (bar == std::string::npos || bar + 1 >= key.size()) return key;
        const std::string es = key.substr(bar + 1);
        char* end = nullptr; const double entry = std::strtod(es.c_str(), &end);
        if (end == es.c_str() || *end != '\0') return key;   // non-numeric tail -> leave untouched
        return key.substr(0, bar + 1) + stall_detail::pyfloat(stall_detail::round4(entry), 4);
    }
    static std::vector<std::string> split_(const std::string& s, char d) {
        std::vector<std::string> out; std::string cur;
        for (char ch : s) { if (ch == d) { out.push_back(cur); cur.clear(); } else cur += ch; }
        out.push_back(cur);
        return out;
    }

    // ── roll-up from companion_closed.csv + open pos (faithful to python build_state) ──
    StallRollUp compute_rollup_(int64_t now) const {
        using namespace stall_detail;
        StallRollUp R;
        R.name = cfg_.name; R.gauge = usd_mode_ ? "USD" : "PCT";
        R.arm_usd = cfg_.arm_usd; R.trail_usd = cfg_.trail_usd; R.retrig_usd = cfg_.retrig_usd;
        R.gate_pct = cfg_.gate_pct; R.rev_gb = cfg_.rev_gb; R.stall_bars = cfg_.stall_bars;
        R.updated = updated_utc(now);
        R.open_companions = (int)pos_.size();

        const int64_t cut_today = (now / 86400) * 86400;
        std::ifstream f(closed_path_);
        std::string line; bool first = true;
        while (std::getline(f, line)) {
            if (first) { first = false; continue; }            // header
            if (line.empty()) continue;
            std::vector<std::string> c = csvparse(line);
            if (c.size() < 10) continue;
            const std::string& book = c[1]; const std::string& reason = c[2]; const std::string& engine = c[3];
            const std::string& entry = c[6];
            const std::string& pnls = c[c.size() - 3];         // realized_pnl always 3rd-from-last
            char* e = nullptr; double pnl = std::strtod(pnls.c_str(), &e);
            if (e == pnls.c_str()) continue;
            StallEngAgg& ea = R.per_engine[engine]; ea.closed++; ea.realized += pnl; R.realized_total += pnl;
            StallLegAgg& la = R.per_leg[engine + "|" + entry];
            la.engine = engine; la.entry = entry; la.book = book; la.closed++; la.realized = round2(la.realized + pnl);
            R.by_reason[reason] = round2(R.by_reason[reason] + pnl);
            if (book == "CRYPTO") { R.crypto_realized += pnl; R.crypto_by_reason[reason] = round2(R.crypto_by_reason[reason] + pnl); }
            const int64_t ts = std::atoll(c[0].c_str());
            if (ts >= cut_today)        R.realized_today += pnl;
            if (ts >= now - 7  * 86400) R.realized_7d    += pnl;
            if (ts >= now - 30 * 86400) R.realized_30d   += pnl;
        }
        for (auto& kv : R.per_engine) kv.second.realized = round2(kv.second.realized);
        R.realized_total = round2(R.realized_total);
        R.realized_today = round2(R.realized_today);
        R.realized_7d    = round2(R.realized_7d);
        R.realized_30d   = round2(R.realized_30d);
        for (const auto& kv : pos_) {
            const Pos& p = kv.second;
            R.per_engine[p.eng].open++;
            StallLegAgg& la = R.per_leg[p.eng + "|" + fmtnum(p.entry)];
            if (la.engine.empty()) { la.engine = p.eng; la.book = p.book; }
            la.open++;
            StallOpenDet od;
            od.book = p.book; od.eng = p.eng; od.sym = p.sym; od.side = p.side; od.entry = p.entry;
            od.mfe_pct = round2(p.mfe_pct); od.mfe_usd = round2(p.mfe_usd); od.stall = p.stall;
            od.upnl = round2(p.last_upnl);
            od.eligible = usd_mode_ ? (p.mfe_usd >= cfg_.arm_usd) : (p.mfe_pct >= cfg_.gate_pct);
            R.open_detail.push_back(od);
        }
        return R;
    }

    // ── serialise a single book's companion_state.json (debug/parity; the DESK reads the
    //    merged aggregate, not this). Written atomically (Windows rename can't overwrite). ──
    void write_state_() const {
        const std::string js = serialize_book_(roll_);
        const std::string tmp = state_path_ + ".tmp";
        { std::ofstream sf(tmp, std::ios::trunc); if (!sf.is_open()) return; sf << js; }
#if defined(_WIN32)
        std::remove(state_path_.c_str());
#endif
        std::rename(tmp.c_str(), state_path_.c_str());
    }

    static std::string serialize_book_(const StallRollUp& R) {
        using namespace stall_detail;
        std::ostringstream o; o << std::fixed << std::setprecision(2);
        o << "{\"updated\":"; jstr(o, R.updated);
        o << ",\"stall_bars\":" << R.stall_bars << ",\"gate_pct\":" << R.gate_pct
          << ",\"reversal_giveback\":" << R.rev_gb << ",\"gauge\":"; jstr(o, R.gauge);
        o << ",\"arm_usd\":" << R.arm_usd << ",\"trail_usd\":" << R.trail_usd
          << ",\"retrig_usd\":" << R.retrig_usd << ",\"open_companions\":" << R.open_companions
          << ",\"realized_total\":" << R.realized_total << ",\"realized_today\":" << R.realized_today
          << ",\"realized_7d\":" << R.realized_7d << ",\"realized_30d\":" << R.realized_30d;
        o << ",\"by_reason\":{"; { bool fr = true; for (auto& kv : R.by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}";
        o << ",\"by_book\":{\"OMEGA\":{\"realized\":" << R.realized_total << ",\"by_reason\":{";
        { bool fr = true; for (auto& kv : R.by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}}}";
        o << ",\"per_engine\":{"; { bool fe = true; for (auto& kv : R.per_engine) { if (!fe) o << ','; fe = false; jstr(o, kv.first); o << ":{\"open\":" << kv.second.open << ",\"closed\":" << kv.second.closed << ",\"realized\":" << kv.second.realized << "}"; } } o << "}";
        o << ",\"per_leg\":{"; { bool fl = true; for (auto& kv : R.per_leg) { if (!fl) o << ','; fl = false; jstr(o, kv.first); o << ":{\"engine\":"; jstr(o, kv.second.engine); o << ",\"entry\":"; jstr(o, kv.second.entry); o << ",\"book\":"; jstr(o, kv.second.book); o << ",\"open\":" << kv.second.open << ",\"closed\":" << kv.second.closed << ",\"realized\":" << kv.second.realized << "}"; } } o << "}";
        o << ",\"open_detail\":["; { bool fo = true; for (const auto& d : R.open_detail) { if (!fo) o << ','; fo = false; o << "{\"book\":"; jstr(o, d.book); o << ",\"eng\":"; jstr(o, d.eng); o << ",\"sym\":"; jstr(o, d.sym); o << ",\"side\":"; jstr(o, d.side); o << ",\"entry\":" << d.entry << ",\"mfe_pct\":" << d.mfe_pct << ",\"mfe_usd\":" << d.mfe_usd << ",\"stall\":" << d.stall << ",\"upnl\":" << d.upnl << ",\"eligible\":" << (d.eligible ? "true" : "false") << "}"; } } o << "]}";
        return o.str();
    }
};

// ── registry: all book instances + 60s throttle + gold-bull compute + aggregate ──
class StallCompanionRegistry {
public:
    bool enabled = true;

    StallBook& add(StallBook::Config c) { books_.emplace_back(std::move(c)); return books_.back(); }
    size_t size() const { return books_.size(); }

    // Cheap pre-check so the caller can skip building the live-row vector 240x/min:
    // true only when the 60s harvest is actually due (maybe_drive re-checks authoritatively).
    bool due(int64_t now) const { return enabled && !books_.empty() && (now - last_drive_ >= 60); }

    // Drive all books off the current live_trades snapshot. Throttled to 60s to match
    // the retired cron cadence (the snapshot rebuilds every 250ms; we only harvest 1/min).
    // rows_all = ALL live_trades (unfiltered). Writes each book's state + the merged
    // aggregate companion_state.json (served by /api/companion).
    void maybe_drive(const std::vector<StallLiveRow>& rows_all, int64_t now,
                     const std::string& gold_trend_h4_csv) {
        if (!enabled || books_.empty()) return;
        if (now - last_drive_ < 60) return;
        last_drive_ = now;
        const int gold_bull = gold_4h_bull_(gold_trend_h4_csv);
        for (auto& b : books_) b.step(rows_all, gold_bull, now);
        write_aggregate_(now);
    }

private:
    std::vector<StallBook> books_;
    int64_t last_drive_ = 0;

    // Gold 4h trend from gold_d1_trend_h4.csv (ts_ms,o,h,l,c) tail 60. Fast SMA(10) vs
    // slow SMA(30) of h4 closes: up when fast>=slow. Faithful to python gold_4h_bull().
    // Returns 1 (up) / 0 (down) / -1 (unknown: feed unreadable / too few bars).
    static int gold_4h_bull_(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return -1;
        std::vector<double> closes; std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || !std::isdigit((unsigned char)line[0])) continue;   // skip header
            std::vector<std::string> p = stall_detail::csvparse(line);
            if (p.size() < 5) continue;
            char* e = nullptr; double c = std::strtod(p[4].c_str(), &e);
            if (e != p[4].c_str() && c > 0.0) closes.push_back(c);
        }
        if (closes.size() > 60) closes.erase(closes.begin(), closes.end() - 60);   // tail 60
        const int FAST = 10, SLOW = 30;
        if ((int)closes.size() < SLOW) {
            if (closes.size() < 3) return -1;
            double sum = 0; for (double v : closes) sum += v;
            return closes.back() >= (sum / closes.size()) ? 1 : 0;
        }
        double fs = 0, ss = 0;
        for (size_t i = closes.size() - FAST; i < closes.size(); ++i) fs += closes[i];
        for (size_t i = closes.size() - SLOW; i < closes.size(); ++i) ss += closes[i];
        return (fs / FAST) >= (ss / SLOW) ? 1 : 0;
    }

    // ── merge every book's roll-up -> companion_state.json (port of companion_aggregate.py) ──
    void write_aggregate_(int64_t now) const {
        using namespace stall_detail;
        std::map<std::string, StallEngAgg> per_engine;
        std::map<std::string, StallLegAgg> per_leg;
        std::map<std::string, double> by_reason;
        std::vector<const StallOpenDet*> open_detail;
        int open_total = 0;
        double rt = 0, rtd = 0, r7 = 0, r30 = 0, crypto_r = 0;
        std::map<std::string, double> crypto_by_reason;

        for (const auto& b : books_) {
            const StallRollUp& R = b.rollup();
            for (const auto& kv : R.per_engine) {
                StallEngAgg& a = per_engine[kv.first];
                a.open += kv.second.open; a.closed += kv.second.closed; a.realized += kv.second.realized;
            }
            for (const auto& kv : R.per_leg) {
                StallLegAgg& a = per_leg[kv.first];
                if (a.engine.empty()) { a.engine = kv.second.engine; a.entry = kv.second.entry; a.book = kv.second.book; }
                a.open += kv.second.open; a.closed += kv.second.closed; a.realized = round2(a.realized + kv.second.realized);
            }
            for (const auto& kv : R.by_reason) by_reason[kv.first] = round2(by_reason[kv.first] + kv.second);
            for (const auto& d : R.open_detail) if (d.book.empty() || d.book == "OMEGA") open_detail.push_back(&d);
            open_total += R.open_companions;
            rt += R.realized_total; rtd += R.realized_today; r7 += R.realized_7d; r30 += R.realized_30d;
            crypto_r += R.crypto_realized;
            for (const auto& kv : R.crypto_by_reason) crypto_by_reason[kv.first] = round2(crypto_by_reason[kv.first] + kv.second);
        }
        for (auto& kv : per_engine) kv.second.realized = round2(kv.second.realized);
        rt = round2(rt); rtd = round2(rtd); r7 = round2(r7); r30 = round2(r30);

        std::ostringstream o; o << std::fixed << std::setprecision(2);
        o << "{\"updated\":"; jstr(o, updated_utc(now));
        o << ",\"source\":\"stall_companion_cpp\",\"n_books\":" << books_.size()
          << ",\"open_companions\":" << open_total
          << ",\"realized_total\":" << rt << ",\"realized_today\":" << rtd
          << ",\"realized_7d\":" << r7 << ",\"realized_30d\":" << r30;
        o << ",\"by_reason\":{"; { bool fr = true; for (auto& kv : by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}";
        // by_book.OMEGA is LOAD-BEARING (desk pollComp guard rejects a frame missing it).
        o << ",\"by_book\":{\"OMEGA\":{\"realized\":" << rt << ",\"realized_today\":" << rtd
          << ",\"realized_7d\":" << r7 << ",\"realized_30d\":" << r30 << ",\"by_reason\":{";
        { bool fr = true; for (auto& kv : by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}}";
        o << ",\"CRYPTO\":{\"realized\":" << round2(crypto_r) << ",\"by_reason\":{";
        { bool fr = true; for (auto& kv : crypto_by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}}}";
        o << ",\"per_engine\":{"; { bool fe = true; for (auto& kv : per_engine) { if (!fe) o << ','; fe = false; jstr(o, kv.first); o << ":{\"open\":" << kv.second.open << ",\"closed\":" << kv.second.closed << ",\"realized\":" << kv.second.realized << "}"; } } o << "}";
        o << ",\"per_leg\":{"; { bool fl = true; for (auto& kv : per_leg) { if (!fl) o << ','; fl = false; jstr(o, kv.first); o << ":{\"engine\":"; jstr(o, kv.second.engine); o << ",\"entry\":"; jstr(o, kv.second.entry); o << ",\"book\":"; jstr(o, kv.second.book); o << ",\"open\":" << kv.second.open << ",\"closed\":" << kv.second.closed << ",\"realized\":" << kv.second.realized << "}"; } } o << "}";
        o << ",\"open_detail\":["; { bool fo = true; for (const auto* d : open_detail) { if (!fo) o << ','; fo = false; o << "{\"book\":"; jstr(o, d->book); o << ",\"eng\":"; jstr(o, d->eng); o << ",\"sym\":"; jstr(o, d->sym); o << ",\"side\":"; jstr(o, d->side); o << ",\"entry\":" << d->entry << ",\"mfe_pct\":" << d->mfe_pct << ",\"mfe_usd\":" << d->mfe_usd << ",\"stall\":" << d->stall << ",\"upnl\":" << d->upnl << ",\"eligible\":" << (d->eligible ? "true" : "false") << "}"; } } o << "]}";

        const std::string js = o.str();
        const std::string dest = "companion_state.json";   // served by /api/companion (cwd = C:\Omega)
        const std::string tmp = dest + ".tmp";
        { std::ofstream sf(tmp, std::ios::trunc); if (!sf.is_open()) return; sf << js; }
#if defined(_WIN32)
        std::remove(dest.c_str());
#endif
        std::rename(tmp.c_str(), dest.c_str());
    }
};

inline StallCompanionRegistry& stall_companions() noexcept {
    static StallCompanionRegistry inst;
    return inst;
}

} // namespace omega
