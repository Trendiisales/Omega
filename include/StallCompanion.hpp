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
// A single banked clip row (from companion_closed.csv) surfaced to the desk so a
// COMP-BANK increase is always explained by visible rows (operator 2026-07-08:
// "something traded and increased the total but not displaying why").
struct StallClosedDet {
    int64_t     ts = 0;
    std::string book, reason, eng, sym, side;
    double      entry = 0.0, pnl = 0.0;
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
    std::vector<StallClosedDet>        closed_detail;   // tail of companion_closed.csv (newest last)
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
// S-2026-07-08c AUTO-RETIREMENT (operator order): a companion book whose banked
// forward net proves <= retire_usd stops ARMING new companions/mirrors (open ones
// still manage/bank normally; state files keep publishing). Symmetric to the
// n>=30 promote gate -- a proven-negative book retires itself instead of waiting
// for a manual cull. Un-retire = operator raises the limit / clears the closed
// csv (loud, deliberate). Banked net restored at boot by summing the book's own
// companion_closed.csv (realized_pnl column, 3rd-from-last -- same parser as the
// registry rollup).
inline double stall_closed_net_(const std::string& closed_csv) {
    std::ifstream f(closed_csv);
    if (!f.is_open()) return 0.0;
    std::string ln; double net = 0.0; bool first = true;
    while (std::getline(f, ln)) {
        if (first) { first = false; continue; }
        std::vector<std::string> c; std::stringstream ss(ln); std::string t;
        while (std::getline(ss, t, ',')) c.push_back(t);
        if (c.size() < 4) continue;
        net += std::atof(c[c.size() - 3].c_str());
    }
    return net;
}

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
        // ── S-2026-07-17t BE-ENTRY FLOORED mimic mode (BeFloorOnOpenFoundation) ──
        // confirm_usd > 0 switches the book to the mandated floored-on-open shape:
        // the companion stays FLAT (books NOTHING, mirrors no parent loser) until the
        // leg's fav_usd >= confirm_usd; it then opens ANCHORED (le=epx: banks measure
        // from the parent entry / reclip level, never the crossing-bar overshoot) with
        // its clip level FLOORED at anchor + floor_cost_usd (>= BE by construction —
        // a config property, NOT an execution guarantee: an H1 gap through the floor
        // books the real fill / real tail, per the S-17f honesty rule). Reclip is
        // LEVEL-anchored GAP-25 (S-17u): reopen when fav_usd >= prior_peak + retrig_usd,
        // anchor = max(0, prior_peak + retrig_usd − confirm_usd) — anchor = reopen −
        // confirm, identical BE-entry room to the initial open (restart-safe: the only
        // reclip state is the persisted peak). trail_usd doubles as the giveback trail; the pct paths,
        // stall clip and cold-loss cut are all bypassed (a flat-until-confirm book has
        // no pre-BE exposure to cut). Certified per-cell before wiring any book
        // (backtest/stall_clip_sweep_idx_bearshort.py; feedback-profit-lock-mandatory).
        // NOTE: anchor is carried in fav_usd units and subtracted from the banked
        // r.upnl — valid while the parent leg's size x tick-mult == 1 ($1/pt index
        // CFD legs); certify before reusing on a scaled symbol.
        double confirm_usd    = 0.0;               // >0 => floored BE-ENTRY mode
        double floor_cost_usd = 0.0;               // BE floor offset above anchor (companion RT cost)
        bool   bull_only    = false;               // COMPANION_BULL_ONLY
        double retire_usd   = -300.0;              // S-2026-07-08c auto-retirement: banked net <= this
                                                   //   => no NEW companions (0 = disabled; ~2x a worst
                                                   //   validated clip-book drawdown leg)
        // DRAWDOWN-CANCEL: USD cold-loss cut per book (mimic never touches real trade -> free).
        //   Backtested clip-book drawdown leg; crypto/gold run intraday (fine bars -> clean cut,
        //   no daily gap-through). Paired with retire_usd latch above.
        double cold_loss_omega  = -50.0;           // COLD_LOSS_USD_OMEGA
        double cold_loss_crypto = -15.0;           // COLD_LOSS_USD_CRYPTO
        double cold_loss_bear   = 0.0;             // S-2026-07-08 loss-bound study: <0 => TIGHTER cold
                                                   //   LOSS_CUT for GOLD legs while gold is PRICE-BEAR
                                                   //   (gold_regime().is_bear() core, passed per drive).
                                                   //   0 = disabled. Evidence (companion_lossbound_sweep.py,
                                                   //   2022-2026 certified H1, intrabar adverse-first):
                                                   //   flat tightening fails (-35 keeps 66% of 4h econ net);
                                                   //   bear-only -35 keeps 96% econ / 98.5% ledger net,
                                                   //   worst leg -71 -> -52, all-6 PASS, smooth plateau.
                                                   //   Fail-safe: unknown regime (-1) => baseline cut.
        std::string dir;                           // working dir (relative, e.g. "stall/xau_tf4h_usd_a")
    };

    explicit StallBook(Config c) : cfg_(std::move(c)) {
        be_mode_  = cfg_.confirm_usd > 0.0;
        usd_mode_ = cfg_.arm_usd > 0.0 || be_mode_;   // be-mode gauges/peaks in fav_usd
        { std::error_code ec; std::filesystem::create_directories(cfg_.dir, ec); }   // never silent-die on a missing state dir
        closed_path_  = cfg_.dir + "/companion_closed.csv";
        banked_net_   = stall_closed_net_(closed_path_);   // S-2026-07-08c retirement watermark
        state_path_   = cfg_.dir + "/companion_state.json";
        pos_path_     = cfg_.dir + "/companion_positions.tsv";     // C++-owned persistence (not python JSON)
        clipped_path_ = cfg_.dir + "/companion_clipped.tsv";
        load_pos_();
    }

    const Config& config() const { return cfg_; }
    const std::string& name() const { return cfg_.name; }
    const StallRollUp& rollup() const { return roll_; }

    // [PROFIT-LOCK-GATE] (S-2026-07-17u, H5): would this book adopt a leg with this
    // engine tag + symbol? Exposes the EXACT step-filter semantics (excluded_/included_)
    // so the registry's runtime coverage sweep uses the matching code itself, not a
    // config-text re-derivation.
    bool claims(const std::string& eng, const std::string& sym) const {
        return !excluded_(eng) && included_(eng, sym);
    }

    // ── one cycle. rows_all = ALL live_trades (unfiltered), for the omega_empty guard;
    //    filtering is done inside. gold_bull: 1 up / 0 down / -1 unknown (computed once
    //    per drive cycle by the registry). gold_bear: 1 price-bear / 0 not / -1 unknown
    //    (gold_regime().is_bear() core, for cold_loss_bear). now = epoch seconds. ──
    void step(const std::vector<StallLiveRow>& rows_all, int gold_bull, int gold_bear, int64_t now) {
        using namespace stall_detail;
        // EMPTY-OMEGA GUARD (faithful to python): a totally flat VPS book (0 live trades)
        // is indistinguishable from a restart blip — SKIP the harvest entirely (no bank,
        // no ENGINE_EXIT prune, pos+clipped preserved), then still write state (heartbeat).
        const bool omega_empty = rows_all.empty();
        if (!omega_empty) {
            const int64_t bar = now / cfg_.tf_sec;
            std::map<std::string, double> live;   // key -> last upnl

            // ── KEY-MIGRATION pre-pass (S-2026-07-17r flap fix) ──────────────────
            // A live row whose key drifted — the parent's REPORTED entry changed
            // representation across a restart/manual-rebook (NAS100 28987.4 <->
            // 28987.375) — is the SAME leg, not an exit + a new leg. Re-key the open
            // companion (and any clip memory) IN PLACE. Without this the old key
            // banked ENGINE_EXIT and the new key reopened at the parent entry,
            // RE-REALIZING the full from-entry upnl on every drift (2026-07-17:
            // +$980.90 phantom over 5 spurious banks on ONE IndexBearShort ride).
            // Runs BEFORE the main row loop so a drifted row can never open a
            // duplicate companion first. Trade-off: a genuine close + instant
            // re-entry within kKeyDriftTol of the old entry merges into the old leg
            // (no bank) — conservative direction, never double-counts.
            {
                std::map<std::string, const StallLiveRow*> in;   // filtered incoming rows by key
                for (const auto& r : rows_all) {
                    if (excluded_(r.eng) || !included_(r.eng, r.sym)) continue;
                    in[mkkey_(r.book, r.eng, r.sym, r.entry)] = &r;
                }
                struct ReKey { std::string oldk, newk; double entry; };
                std::vector<ReKey> mv;
                for (const auto& kv : pos_) {
                    if (in.count(kv.first)) continue;            // key still live, nothing to do
                    const Pos& p = kv.second;
                    for (const auto& ik : in) {
                        if (pos_.count(ik.first) || clipped_.count(ik.first)) continue;   // target already tracked
                        bool taken = false;
                        for (const auto& m : mv) if (m.newk == ik.first) { taken = true; break; }
                        if (taken) continue;
                        const StallLiveRow& r = *ik.second;
                        if (r.book != p.book || r.eng != p.eng || r.sym != p.sym || r.side != p.side) continue;
                        if (std::fabs(r.entry - p.entry) > kKeyDriftTol * std::max(std::fabs(p.entry), 1.0)) continue;
                        mv.push_back(ReKey{kv.first, ik.first, r.entry});
                        break;
                    }
                }
                for (const auto& m : mv) {
                    Pos p = pos_[m.oldk]; pos_.erase(m.oldk);
                    p.entry = round4(m.entry);
                    pos_[m.newk] = p;
                    absent_.erase(m.oldk);
                    std::printf("[STALL][REKEY] book=%s %s -> %s (parent entry drift, same leg -- no ENGINE_EXIT bank)\n",
                                cfg_.name.c_str(), m.oldk.c_str(), m.newk.c_str());
                    std::fflush(stdout);
                }
                // Clip memories drift the same way — re-key so the clip survives (a
                // flap-orphaned clip memory would let the returning row REOPEN and
                // re-clip a ride the book already banked). Side is not in the key.
                std::vector<ReKey> mvc;
                for (const auto& kv : clipped_) {
                    if (in.count(kv.first)) continue;
                    std::string b, e, s; double en = 0.0;
                    if (!keyfields_(kv.first, b, e, s, en)) continue;
                    for (const auto& ik : in) {
                        if (pos_.count(ik.first) || clipped_.count(ik.first)) continue;
                        bool taken = false;
                        for (const auto& m : mvc) if (m.newk == ik.first) { taken = true; break; }
                        if (taken) continue;
                        const StallLiveRow& r = *ik.second;
                        if (r.book != b || r.eng != e || r.sym != s) continue;
                        if (std::fabs(r.entry - en) > kKeyDriftTol * std::max(std::fabs(en), 1.0)) continue;
                        mvc.push_back(ReKey{kv.first, ik.first, r.entry});
                        break;
                    }
                }
                for (const auto& m : mvc) {
                    const double pk = clipped_[m.oldk]; clipped_.erase(m.oldk);
                    clipped_[m.newk] = pk;
                    absent_.erase(m.oldk);
                }
            }

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
                double reopen_anchor = 0.0; bool reopened = false;   // be-mode LEVEL-anchored reclip
                if (cit != clipped_.end()) {
                    const double prior_peak = cit->second;
                    bool retrig = false;
                    if (be_mode_) {
                        // GAP-25 reclip (S-2026-07-17u, operator: "capture every 25 instead of 50"):
                        // reopen once the leg clears peak + retrig; anchor = reopen − confirm so the
                        // reclip leg opens exactly confirm above its anchor (same BE-entry room as the
                        // initial open) and banks its own segment only. With retrig==confirm the anchor
                        // sits AT the prior peak — the never-banked strip between clips shrinks from
                        // retrig+confirm to retrig. No overlap with the prior banked segment (prior
                        // bank level = peak − trail <= peak <= anchor). Certified BOTH symbols, both
                        // reclip variants, cost x1/x2 (sweep_gap25 over stall_clip_sweep_idx_bearshort
                        // paths: NAS +12,110 PF7.9 worst −190; US500 +2,307 PF12.2 worst −50; 12/12
                        // grid PASS, WF halves + — beats the shipped gap-50 on net AND worst-leg).
                        retrig = (cfg_.retrig_usd > 0.0 && prior_peak > 0.0
                                  && fav_usd >= prior_peak + cfg_.retrig_usd);
                        if (retrig) {
                            reopen_anchor = std::max(0.0, prior_peak + cfg_.retrig_usd - cfg_.confirm_usd);
                            reopened = true;
                        }
                    }
                    else if (usd_mode_) retrig = (cfg_.retrig_usd > 0.0 && prior_peak > 0.0 && fav_usd > prior_peak + cfg_.retrig_usd);
                    else                retrig = (cfg_.retrig_pct > 0.0 && prior_peak > 0.0 && fav     > prior_peak * (1.0 + cfg_.retrig_pct));
                    if (retrig) clipped_.erase(cit);
                    else        continue;
                }

                if (pos_.find(key) == pos_.end()) {
                    // BE-ENTRY (be-mode): stay FLAT until the leg has covered confirm_usd —
                    // a parent loser is never mirrored, so the book has no pre-BE exposure.
                    if (be_mode_ && !reopened && fav_usd < cfg_.confirm_usd) continue;
                    // BULL-REGIME GATE: only OPEN a new gold companion while the 4h trend is up.
                    // Never blocks an already-open companion. FAIL-SAFE: unknown regime -> skip open.
                    if (cfg_.bull_only && is_gold_(r.sym) && gold_bull != 1) continue;
                    // S-2026-07-08c auto-retirement: proven-negative book arms nothing new.
                    if (cfg_.retire_usd < 0.0 && banked_net_ <= cfg_.retire_usd) {
                        if (!retired_logged_) { retired_logged_ = true;
                            std::printf("[STALL][RETIRED] book=%s banked_net=$%.0f <= $%.0f -- no new companions (auto-retirement)\n",
                                        cfg_.name.c_str(), banked_net_, cfg_.retire_usd); std::fflush(stdout); }
                        continue;
                    }
                    Pos p;
                    p.book = r.book; p.eng = r.eng; p.sym = r.sym; p.side = r.side;
                    p.entry = round4(r.entry);
                    p.open_bar = bar; p.mfe_pct = fav; p.mfe_usd = fav_usd; p.ext_bar = bar;
                    p.last_upnl = r.upnl;
                    p.anchor = reopen_anchor;   // be-mode: 0 on a fresh open, peak+retrig on a reclip
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

                // ── be-mode: FLOORED giveback trail (opened at confirm => armed by construction).
                // Clip LEVEL never below the BE floor (anchor + floor_cost_usd); the BANK is the
                // real mark at detection (honest fill — a gap through the floor books its tail).
                // No stall/cold-loss paths: a flat-until-confirm book has no pre-BE exposure.
                if (be_mode_) {
                    const double stop_lvl = std::max(p.mfe_usd - cfg_.trail_usd,
                                                     p.anchor + cfg_.floor_cost_usd);
                    if (fav_usd <= stop_lvl) {
                        close_(key, "FLOOR_CLIP", r.upnl - p.anchor, bar);
                        clipped_[key] = peak_store;
                    }
                    continue;
                }

                const bool armed = usd_mode_ ? (p.mfe_usd >= cfg_.arm_usd) : (p.mfe_pct >= cfg_.gate_pct);

                if (armed && p.stall >= cfg_.stall_bars) { close_(key, "STALL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                if (usd_mode_) {
                    if (armed && cfg_.trail_usd > 0.0 && fav_usd <= p.mfe_usd - cfg_.trail_usd) { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                } else {
                    if (armed && fav <= p.mfe_pct * (1.0 - cfg_.rev_gb))                     { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                    if (armed && cfg_.rev_gb_pts > 0.0 && fav <= p.mfe_pct - cfg_.rev_gb_pts) { close_(key, "REVERSAL_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
                }
                double cold = (r.book == "CRYPTO") ? cfg_.cold_loss_crypto : cfg_.cold_loss_omega;
                // S-2026-07-08 loss-bound: tighter cut on GOLD legs while gold is price-bear only.
                // Guarded so a misconfig can never LOOSEN the baseline floor; -1 (unknown) = baseline.
                if (cfg_.cold_loss_bear < 0.0 && gold_bear == 1 && r.book != "CRYPTO"
                    && is_gold_(r.sym) && cfg_.cold_loss_bear > cold) cold = cfg_.cold_loss_bear;
                if (r.upnl <= cold) { close_(key, "LOSS_CUT_CLIP", r.upnl, bar); clipped_[key] = peak_store; continue; }
            }
            // real trade closed first -> ENGINE_EXIT any open companion whose key left `live`.
            // ── ABSENCE DEBOUNCE (S-2026-07-17r flap fix): a telemetry frame can transiently
            // DROP one live row while others remain (partial-frame flap — the GUI already
            // ignores such frames, omega_desk.html "COMP-BANK 54<->29"). Banking on FIRST
            // absence re-realized the same open ride 4x intraday on 2026-07-17. Bank only
            // after the key is absent kExitMissCycles CONSECUTIVE drive cycles (60s cadence
            // -> ~2 min confirmation; a genuine parent close banks the SAME carried
            // last_upnl one cycle later, so nothing real is lost).
            std::vector<std::string> gone;
            for (const auto& kv : pos_) if (live.find(kv.first) == live.end()) gone.push_back(kv.first);
            for (const auto& key : gone) {
                const int miss = ++absent_[key];
                if (miss < kExitMissCycles) {
                    std::printf("[STALL][FLAP-HOLD] book=%s key=%s absent %d/%d cycles -- ENGINE_EXIT deferred\n",
                                cfg_.name.c_str(), key.c_str(), miss, kExitMissCycles);
                    std::fflush(stdout);
                    continue;
                }
                absent_.erase(key);
                // be-mode: an open floored leg banks its own segment only (anchored)
                close_(key, "ENGINE_EXIT", pos_[key].last_upnl - (be_mode_ ? pos_[key].anchor : 0.0), bar);
            }
            // a clip stays clipped only while its real trade is live; drop once the engine
            // actually closes it — same debounce (a flap-pruned clip memory would let the
            // returning row REOPEN + re-clip a ride the book already banked).
            for (auto it = clipped_.begin(); it != clipped_.end();) {
                if (live.find(it->first) == live.end() && ++absent_[it->first] >= kExitMissCycles) {
                    absent_.erase(it->first);
                    it = clipped_.erase(it);
                } else ++it;
            }
            // absence counters reset the moment the key is seen live again (or is untracked)
            for (auto it = absent_.begin(); it != absent_.end();) {
                const bool tracked = pos_.count(it->first) || clipped_.count(it->first);
                if (!tracked || live.count(it->first)) it = absent_.erase(it); else ++it;
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
        double anchor = 0.0;   // be-mode reclip anchor (fav_usd level banks measure from)
    };

    // ── S-2026-07-17r flap-fix knobs (see step() KEY-MIGRATION + ABSENCE DEBOUNCE) ──
    static constexpr int    kExitMissCycles = 2;    // consecutive absent drive cycles before ENGINE_EXIT banks / clip memory prunes
    static constexpr double kKeyDriftTol    = 1e-4; // rel entry tolerance separating same-leg key drift from a genuine new entry

    Config cfg_;
    bool usd_mode_ = false;
    bool be_mode_  = false;            // S-2026-07-17t: confirm_usd > 0 => BE-ENTRY FLOORED mimic mode
    double banked_net_ = 0.0;          // S-2026-07-08c auto-retirement running net
    bool   retired_logged_ = false;
    std::string closed_path_, state_path_, pos_path_, clipped_path_;
    std::unordered_map<std::string, Pos>    pos_;
    std::unordered_map<std::string, double> clipped_;
    std::unordered_map<std::string, int>    absent_;   // key -> consecutive drive cycles absent from live (transient, not persisted)
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
        banked_net_ += pnl;   // S-2026-07-08c retirement watermark
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
        if (f.is_open()) {
            // S-2026-07-17r: default ostream precision (6 sig figs) truncated the entry
            // (28987.375 -> "28987.4"), so load_pos_ rebuilt a DIFFERENT key than the live
            // row after every restart -> guaranteed spurious ENGINE_EXIT bank + reopen.
            f << std::setprecision(12);
            for (const auto& kv : pos_) {
                const Pos& p = kv.second;
                f << kv.first << '\t' << p.book << '\t' << p.eng << '\t' << p.sym << '\t' << p.side
                  << '\t' << p.entry << '\t' << p.open_bar << '\t' << p.ext_bar << '\t' << p.mfe_pct
                  << '\t' << p.mfe_usd << '\t' << p.stall << '\t' << p.last_upnl
                  << '\t' << p.anchor << '\n';
            }
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
            p.anchor = t.size() > 12 ? std::atof(t[12].c_str()) : 0.0;   // pre-anchor rows load as 0
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
    // Parse a companion key back into its fields ("book|eng|sym|entry"); used by the
    // clip-memory KEY-MIGRATION path (clipped_ stores no Pos fields, only key -> peak).
    static bool keyfields_(const std::string& key, std::string& book, std::string& eng,
                           std::string& sym, double& entry) {
        std::vector<std::string> t = split_(key, '|');
        if (t.size() != 4) return false;
        book = t[0]; eng = t[1]; sym = t[2];
        char* end = nullptr; entry = std::strtod(t[3].c_str(), &end);
        return end != t[3].c_str() && *end == '\0';
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
            StallClosedDet cd;
            cd.ts = ts; cd.book = book; cd.reason = reason; cd.eng = engine;
            cd.sym = c[4]; cd.side = c[5];
            cd.entry = std::strtod(entry.c_str(), nullptr); cd.pnl = round2(pnl);
            R.closed_detail.push_back(cd);
        }
        if (R.closed_detail.size() > 30)
            R.closed_detail.erase(R.closed_detail.begin(), R.closed_detail.end() - 30);
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

// ── MirrorBook — x2 add-on mirror companion (S-2026-07-07s ConnorsMirror) ─────
// DIFFERENT mechanism from StallBook: not a giveback clip of the parent's own
// gain. While a parent leg is live, watch H1 CLOSES; when price gains >= arm_pct
// over the PARENT entry, open an x2-size mirror AT THAT CLOSE (its own entry),
// trail it with a tight gb_pct giveback from its own peak (H1-close eval,
// worse-of fills), re-arm only on +retrig_pct above the prior clip peak, and
// force-flat when the parent closes. SEPARATE INDEPENDENT book (operator rule
// feedback-companion-independent-engine): never touches the parent, judged
// STANDALONE. Faithful port of backtest/connors_mirror_bt.py sim(mode="close")
// — the validated config (AUDITED ConnorsMirror_NAS100, commit 059918cd):
// arm 2.0% / gb 0.75% / retrig 2% / x2: n=15/4.3yr +897pt PF2.66 WR67
// both-halves+ bear22 +410 2x-cost +809 ex-best +496. SHADOW, promote n>=30.
//
// ADVERSE-PROTECTION: TRAIL_FROM_ENTRY — the trail is live from the mirror's
//   own entry (peak starts at entry), so max adverse ≈ gb_pct of entry before
//   the clip fires; backtested worst trade −155pt (evidence
//   outputs/CONNORS_MIRROR_REALFILL_2026-07-07.txt). No separate cold cut —
//   adding one deviates from the validated config.
//
// Live-vs-sim deviations (documented, conservative):
//   * sim armed only from the day AFTER parent entry (anti-lookahead device for
//     D1-close parents); live arms from the real entry moment — honest, no
//     lookahead exists live.
//   * H1 "close" = last 60s-drive mark seen inside the hour (<=60s before the
//     boundary); a weekend/multi-bar gap evaluates once with that stored mark.
class MirrorBook {
public:
    struct Config {
        std::string name;                        // book dir basename
        std::vector<std::string> legs;           // exact "ENGINE|SYMBOL" parent legs
        double arm_pct    = 2.0;                 // arm at parent gain >= this % over parent entry
        double gb_pct     = 0.75;                // trail giveback % from mirror peak (close-eval)
        double retrig_pct = 2.0;                 // re-arm needs close beyond clip peak by this %
        double size_mult  = 2.0;                 // mirror size = mult x parent size
        double rt_bp      = 3.0;                 // round-trip cost, bp of mirror entry px
        int    tf_sec     = 3600;                // close-eval timeframe (H1)
        double retire_usd = -300.0;              // S-2026-07-08c auto-retirement: banked net <= this
                                                 //   => no NEW mirror arms (0 = disabled; ~2x the worst
                                                 //   validated mirror trade, e.g. Connors -155pt)
        bool   long_only  = false;               // skip SHORT parents (mirror sweep's SHORT sim
                                                 // is invalid — negation breaks % thresholds; only
                                                 // LONG-side mirrors are validated for XauTF fams)
        std::string dir;                         // working dir, e.g. "stall/connors_mirror_x2"
    };

    explicit MirrorBook(Config c) : cfg_(std::move(c)) {
        { std::error_code ec; std::filesystem::create_directories(cfg_.dir, ec); }
        closed_path_ = cfg_.dir + "/companion_closed.csv";
        banked_net_  = stall_closed_net_(closed_path_);   // S-2026-07-08c retirement watermark
        state_path_  = cfg_.dir + "/companion_state.json";
        pos_path_    = cfg_.dir + "/mirror_watch.tsv";
        load_();
    }

    const Config& config() const { return cfg_; }
    const std::string& name() const { return cfg_.name; }
    const StallRollUp& rollup() const { return roll_; }
    int open_mirrors() const {
        int n = 0; for (const auto& kv : watch_) if (kv.second.active) ++n; return n;
    }
    size_t watching() const { return watch_.size(); }

    void step(const std::vector<StallLiveRow>& rows_all, int64_t now) {
        using namespace stall_detail;
        const bool omega_empty = rows_all.empty();   // restart-blip guard (same as StallBook)
        if (!omega_empty) {
            const int64_t bar = now / cfg_.tf_sec;
            std::map<std::string, const StallLiveRow*> live;
            for (const auto& r : rows_all) {
                if (!leg_match_(r.eng, r.sym)) continue;
                if (cfg_.long_only && (r.side.empty() || r.side[0] == 'S' || r.side[0] == 's'))
                    continue;                                       // unvalidated SHORT parent
                if (r.current <= 0.0 || r.entry <= 0.0) continue;   // no live mark yet
                const std::string key = r.book + "|" + r.eng + "|" + r.sym + "|"
                                      + pyfloat(round4(r.entry), 4);
                live[key] = &r;
                auto it = watch_.find(key);
                if (it == watch_.end()) {
                    Watch w;
                    w.book = r.book; w.eng = r.eng; w.sym = r.sym; w.side = r.side;
                    w.p_entry = round4(r.entry);
                    w.cur_bar = bar; w.last_close = r.current;
                    it = watch_.emplace(key, std::move(w)).first;
                }
                Watch& w = it->second;
                const int dir = (!r.side.empty() && (r.side[0] == 'S' || r.side[0] == 's')) ? -1 : 1;
                // $-per-point from the parent row (upnl / favorable points): scales the
                // mirror book into honest USD at size_mult x parent size.
                {
                    const double fpts = (r.current - r.entry) * dir;
                    if (std::fabs(fpts) > r.entry * 1e-5 && r.upnl != 0.0)
                        w.usd_per_pt = std::fabs(r.upnl / fpts);
                }
                if (bar > w.cur_bar) {
                    // H1 boundary crossed -> w.last_close is the finished bar's close
                    eval_close_(key, w, dir, w.last_close, bar);
                    w.cur_bar = bar;
                }
                w.last_close = r.current;
            }
            // parent closed -> force-flat any open mirror at the last seen mark
            std::vector<std::string> gone;
            for (const auto& kv : watch_) if (live.find(kv.first) == live.end()) gone.push_back(kv.first);
            for (const auto& key : gone) {
                Watch& w = watch_[key];
                if (w.active) {
                    const int dir = (!w.side.empty() && (w.side[0] == 'S' || w.side[0] == 's')) ? -1 : 1;
                    bank_(key, w, dir, w.last_close, "PARENT_EXIT", bar);
                }
                watch_.erase(key);
            }
        }
        save_();
        roll_ = compute_rollup_(now);
        write_state_();
    }

private:
    struct Watch {
        std::string book, eng, sym, side;
        double  p_entry = 0.0;                   // parent entry px
        int64_t cur_bar = 0;
        double  last_close = 0.0;                // last mark seen in current bar
        double  usd_per_pt = 0.0;
        bool    active = false;                  // mirror open?
        double  m_entry = 0.0, m_peak = 0.0;     // mirror entry / favorable extreme px
        int64_t m_bar = 0;                       // mirror entry bar
        bool    clipped = false;                 // banked once, awaiting retrig
        double  clip_ref = 0.0;                  // peak px at clip (retrig reference)
    };

    Config cfg_;
    std::string closed_path_, state_path_, pos_path_;
    std::unordered_map<std::string, Watch> watch_;
    double banked_net_ = 0.0;          // S-2026-07-08c auto-retirement running net
    bool   retired_logged_ = false;
    StallRollUp roll_;

    bool leg_match_(const std::string& eng, const std::string& sym) const {
        const std::string leg = eng + "|" + sym;
        for (const auto& l : cfg_.legs) if (l == leg) return true;
        return false;
    }

    // one finished-bar close: sim(mode="close") state machine, worse-of fills
    void eval_close_(const std::string& key, Watch& w, int dir, double close, int64_t bar) {
        if (close <= 0.0) return;
        if (w.active) {
            const double stop = w.m_peak * (1.0 - dir * cfg_.gb_pct / 100.0);
            const bool hit = dir > 0 ? (close <= stop) : (close >= stop);
            if (hit) {
                const double fill = dir > 0 ? std::min(close, stop) : std::max(close, stop);
                w.clip_ref = w.m_peak;
                bank_(key, w, dir, fill, "TRAIL_CLIP", bar);
                w.clipped = true;
                return;
            }
            if (dir > 0 ? (close > w.m_peak) : (close < w.m_peak)) w.m_peak = close;
            return;
        }
        double trig;
        if (w.clipped) {
            if (cfg_.retrig_pct <= 0.0 || w.clip_ref <= 0.0) return;   // one mirror per parent trade
            trig = w.clip_ref * (1.0 + dir * cfg_.retrig_pct / 100.0);
        } else {
            trig = w.p_entry * (1.0 + dir * cfg_.arm_pct / 100.0);
        }
        if (dir > 0 ? (close >= trig) : (close <= trig)) {
            // S-2026-07-08c auto-retirement: proven-negative mirror book arms nothing new.
            if (cfg_.retire_usd < 0.0 && banked_net_ <= cfg_.retire_usd) {
                if (!retired_logged_) { retired_logged_ = true;
                    std::printf("[MIRROR][RETIRED] book=%s banked_net=$%.0f <= $%.0f -- no new mirrors (auto-retirement)\n",
                                cfg_.name.c_str(), banked_net_, cfg_.retire_usd); std::fflush(stdout); }
                return;
            }
            w.active = true; w.m_entry = close; w.m_peak = close; w.m_bar = bar;
            if (w.clipped) { w.clipped = false; w.clip_ref = 0.0; }
        }
    }

    void bank_(const std::string& key, Watch& w, int dir, double fill, const char* reason, int64_t bar) {
        using namespace stall_detail;
        (void)key;
        const double upp  = w.usd_per_pt > 0.0 ? w.usd_per_pt : 1.0;
        const double pts  = (fill - w.m_entry) * dir;
        const double cost = w.m_entry * cfg_.rt_bp * 1e-4;
        const double usd  = (pts - cost) * upp * cfg_.size_mult;
        const double peak_pct = w.m_entry > 0.0 ? ((w.m_peak - w.m_entry) * dir / w.m_entry) * 100.0 : 0.0;
        const bool need_header = !file_nonempty_(closed_path_);
        std::ofstream f(closed_path_, std::ios::app);
        if (f.is_open()) {
            if (need_header) f << "ts,book,reason,engine,symbol,side,entry,realized_pnl,mfe_peak_pct,bars_held\n";
            f << (long long)std::time(nullptr) << ',' << w.book << ',' << reason << ','
              << csvq_("MirrorX2:" + w.eng) << ',' << csvq_(w.sym) << ',' << w.side << ','
              << fmtnum(w.m_entry) << ',' << pyfloat(round2(usd), 2) << ','
              << pyfloat(round2(peak_pct), 2) << ',' << (bar - w.m_bar) << '\n';
        }
        banked_net_ += usd;   // S-2026-07-08c retirement watermark
        w.active = false; w.m_entry = 0.0; w.m_peak = 0.0; w.m_bar = 0;
    }

    static bool file_nonempty_(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        return f.is_open() && f.tellg() > 0;
    }
    static std::string csvq_(const std::string& s) {
        if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
        std::string out = "\"";
        for (char ch : s) { if (ch == '"') out += '"'; out += ch; }
        out += '"';
        return out;
    }

    void save_() const {
        std::ofstream f(pos_path_, std::ios::trunc);
        if (!f.is_open()) return;
        for (const auto& kv : watch_) {
            const Watch& w = kv.second;
            f << kv.first << '\t' << w.book << '\t' << w.eng << '\t' << w.sym << '\t' << w.side
              << '\t' << w.p_entry << '\t' << w.cur_bar << '\t' << w.last_close << '\t' << w.usd_per_pt
              << '\t' << (w.active ? 1 : 0) << '\t' << w.m_entry << '\t' << w.m_peak << '\t' << w.m_bar
              << '\t' << (w.clipped ? 1 : 0) << '\t' << w.clip_ref << '\n';
        }
    }
    void load_() {
        std::ifstream f(pos_path_);
        std::string line;
        while (std::getline(f, line)) {
            std::vector<std::string> t;
            { std::string cur; for (char ch : line) { if (ch == '\t') { t.push_back(cur); cur.clear(); } else cur += ch; } t.push_back(cur); }
            if (t.size() < 15) continue;
            Watch w;
            w.book = t[1]; w.eng = t[2]; w.sym = t[3]; w.side = t[4];
            w.p_entry = std::atof(t[5].c_str()); w.cur_bar = std::atoll(t[6].c_str());
            w.last_close = std::atof(t[7].c_str()); w.usd_per_pt = std::atof(t[8].c_str());
            w.active = std::atoi(t[9].c_str()) != 0;
            w.m_entry = std::atof(t[10].c_str()); w.m_peak = std::atof(t[11].c_str());
            w.m_bar = std::atoll(t[12].c_str());
            w.clipped = std::atoi(t[13].c_str()) != 0; w.clip_ref = std::atof(t[14].c_str());
            watch_[t[0]] = std::move(w);
        }
    }

    StallRollUp compute_rollup_(int64_t now) const {
        using namespace stall_detail;
        StallRollUp R;
        R.name = cfg_.name; R.gauge = "MIRROR";
        R.gate_pct = cfg_.arm_pct; R.rev_gb = cfg_.gb_pct; R.stall_bars = 0;
        R.updated = updated_utc(now);
        R.open_companions = open_mirrors();

        const int64_t cut_today = (now / 86400) * 86400;
        std::ifstream f(closed_path_);
        std::string line; bool first = true;
        while (std::getline(f, line)) {
            if (first) { first = false; continue; }
            if (line.empty()) continue;
            std::vector<std::string> c = csvparse(line);
            if (c.size() < 10) continue;
            const std::string& book = c[1]; const std::string& reason = c[2]; const std::string& engine = c[3];
            const std::string& entry = c[6];
            const std::string& pnls = c[c.size() - 3];
            char* e = nullptr; double pnl = std::strtod(pnls.c_str(), &e);
            if (e == pnls.c_str()) continue;
            StallEngAgg& ea = R.per_engine[engine]; ea.closed++; ea.realized += pnl; R.realized_total += pnl;
            StallLegAgg& la = R.per_leg[engine + "|" + entry];
            la.engine = engine; la.entry = entry; la.book = book; la.closed++; la.realized = round2(la.realized + pnl);
            R.by_reason[reason] = round2(R.by_reason[reason] + pnl);
            const int64_t ts = std::atoll(c[0].c_str());
            if (ts >= cut_today)        R.realized_today += pnl;
            if (ts >= now - 7  * 86400) R.realized_7d    += pnl;
            if (ts >= now - 30 * 86400) R.realized_30d   += pnl;
            StallClosedDet cd;
            cd.ts = ts; cd.book = book; cd.reason = reason; cd.eng = "MirrorX2:" + engine;
            cd.sym = c[4]; cd.side = c[5];
            cd.entry = std::strtod(entry.c_str(), nullptr); cd.pnl = round2(pnl);
            R.closed_detail.push_back(cd);
        }
        if (R.closed_detail.size() > 30)
            R.closed_detail.erase(R.closed_detail.begin(), R.closed_detail.end() - 30);
        for (auto& kv : R.per_engine) kv.second.realized = round2(kv.second.realized);
        R.realized_total = round2(R.realized_total);
        R.realized_today = round2(R.realized_today);
        R.realized_7d    = round2(R.realized_7d);
        R.realized_30d   = round2(R.realized_30d);
        for (const auto& kv : watch_) {
            const Watch& w = kv.second;
            if (!w.active) continue;
            const std::string tag = "MirrorX2:" + w.eng;
            R.per_engine[tag].open++;
            StallLegAgg& la = R.per_leg[tag + "|" + fmtnum(w.m_entry)];
            if (la.engine.empty()) { la.engine = tag; la.entry = fmtnum(w.m_entry); la.book = w.book; }
            la.open++;
            const int dir = (!w.side.empty() && (w.side[0] == 'S' || w.side[0] == 's')) ? -1 : 1;
            StallOpenDet od;
            od.book = w.book; od.eng = tag; od.sym = w.sym; od.side = w.side; od.entry = w.m_entry;
            od.mfe_pct = w.m_entry > 0.0 ? round2(((w.m_peak - w.m_entry) * dir / w.m_entry) * 100.0) : 0.0;
            od.mfe_usd = round2((w.m_peak - w.m_entry) * dir * (w.usd_per_pt > 0.0 ? w.usd_per_pt : 1.0) * cfg_.size_mult);
            od.stall = 0;
            od.upnl = round2((w.last_close - w.m_entry) * dir * (w.usd_per_pt > 0.0 ? w.usd_per_pt : 1.0) * cfg_.size_mult);
            od.eligible = true;
            R.open_detail.push_back(od);
        }
        return R;
    }

    void write_state_() const {
        using namespace stall_detail;
        const StallRollUp& R = roll_;
        std::ostringstream o; o << std::fixed << std::setprecision(2);
        o << "{\"updated\":"; jstr(o, R.updated);
        o << ",\"gauge\":\"MIRROR\",\"arm_pct\":" << cfg_.arm_pct << ",\"gb_pct\":" << cfg_.gb_pct
          << ",\"retrig_pct\":" << cfg_.retrig_pct << ",\"size_mult\":" << cfg_.size_mult
          << ",\"open_companions\":" << R.open_companions
          << ",\"realized_total\":" << R.realized_total << ",\"realized_today\":" << R.realized_today
          << ",\"realized_7d\":" << R.realized_7d << ",\"realized_30d\":" << R.realized_30d;
        o << ",\"by_reason\":{"; { bool fr = true; for (auto& kv : R.by_reason) { if (!fr) o << ','; fr = false; jstr(o, kv.first); o << ':' << kv.second; } } o << "}}";
        const std::string js = o.str();
        const std::string tmp = state_path_ + ".tmp";
        { std::ofstream sf(tmp, std::ios::trunc); if (!sf.is_open()) return; sf << js; }
#if defined(_WIN32)
        std::remove(state_path_.c_str());
#endif
        std::rename(tmp.c_str(), state_path_.c_str());
    }
};

// ── registry: all book instances + 60s throttle + gold-bull compute + aggregate ──
class StallCompanionRegistry {
public:
    bool enabled = true;

    StallBook& add(StallBook::Config c) { books_.emplace_back(std::move(c)); return books_.back(); }
    MirrorBook& add_mirror(MirrorBook::Config c) { mirrors_.emplace_back(std::move(c)); return mirrors_.back(); }
    size_t size() const { return books_.size(); }
    size_t mirror_count() const { return mirrors_.size(); }
    const std::vector<MirrorBook>& mirrors() const { return mirrors_; }

    // Cheap pre-check so the caller can skip building the live-row vector 240x/min:
    // true only when the 60s harvest is actually due (maybe_drive re-checks authoritatively).
    bool due(int64_t now) const { return enabled && (!books_.empty() || !mirrors_.empty()) && (now - last_drive_ >= 60); }

    // Drive all books off the current live_trades snapshot. Throttled to 60s to match
    // the retired cron cadence (the snapshot rebuilds every 250ms; we only harvest 1/min).
    // rows_all = ALL live_trades (unfiltered). Writes each book's state + the merged
    // aggregate companion_state.json (served by /api/companion).
    void maybe_drive(const std::vector<StallLiveRow>& rows_all, int64_t now,
                     const std::string& gold_trend_h4_csv, int gold_price_bear = -1) {
        if (!enabled || (books_.empty() && mirrors_.empty())) return;
        if (now - last_drive_ < 60) return;
        last_drive_ = now;
        const int gold_bull = gold_4h_bull_(gold_trend_h4_csv);
        for (auto& b : books_) b.step(rows_all, gold_bull, gold_price_bear, now);
        for (auto& m : mirrors_) m.step(rows_all, now);
        write_aggregate_(now);
        profit_lock_gate_(rows_all, now);
    }

private:
    std::vector<StallBook> books_;
    std::vector<MirrorBook> mirrors_;
    int64_t last_drive_ = 0;
    std::unordered_map<std::string, bool> plg_seen_;   // eng|sym -> covered (report once per boot)
    bool plg_announced_ = false;

    // ── [PROFIT-LOCK-GATE] runtime coverage sweep (S-2026-07-17u, pre-live hole H5) ──
    // Commit-time proof (companion_coverage_audit.sh parsing config text) is NOT
    // runtime proof — the IBS incident arm sat dead ABOVE the peak in a book that
    // config-text said covered it, and Rider4h was invisible to the audit universe
    // entirely. This sweep asks the RUNNING zoo, every drive cycle, using the books'
    // own matching code: "does ANY book/mirror claim this live leg?" A leg claimed by
    // ZERO books can ride profit back down with no giveback cover — the exact
    // profit-lock-mandatory violation class. RED line + logs/profit_lock_uncovered.log
    // (surfaced by tools/protection_selftest.py check [9]); reported once per tag per
    // boot so a persistent hole cannot spam the tape.
    void profit_lock_gate_(const std::vector<StallLiveRow>& rows_all, int64_t now) {
        if (!plg_announced_) {
            printf("[PROFIT-LOCK-GATE] runtime coverage sweep armed: %zu books + %zu mirrors watch every live leg\n",
                   books_.size(), mirrors_.size());
            fflush(stdout);
            plg_announced_ = true;
        }
        for (const auto& r : rows_all) {
            if (r.book != "OMEGA") continue;          // crypto legs: the crypto binary's own floor gate
            const std::string tag = r.eng + "|" + r.sym;
            if (plg_seen_.count(tag)) continue;
            bool covered = false;
            for (const auto& b : books_) if (b.claims(r.eng, r.sym)) { covered = true; break; }
            if (!covered) {
                const bool is_short = !r.side.empty() && (r.side[0] == 'S' || r.side[0] == 's');
                for (const auto& m : mirrors_) {
                    const auto& mc = m.config();
                    if (mc.long_only && is_short) continue;
                    for (const auto& l : mc.legs)
                        if (l == r.eng + "|" + r.sym) { covered = true; break; }
                    if (covered) break;
                }
            }
            plg_seen_[tag] = covered;
            if (!covered) {
                printf("[PROFIT-LOCK-GATE] *** UNCOVERED LIVE LEG %s %s entry=%.5f -- NO giveback "
                       "book/mirror claims it (profit-lock-mandatory VIOLATION; wire cover or exclude) ***\n",
                       r.eng.c_str(), r.sym.c_str(), r.entry);
                fflush(stdout);
                std::ofstream f("logs/profit_lock_uncovered.log", std::ios::app);
                if (f.is_open())
                    f << now << "," << r.eng << "," << r.sym << ","
                      << stall_detail::pyfloat(stall_detail::round4(r.entry), 4) << "\n";
            }
        }
    }

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
        std::vector<const StallClosedDet*> closed_detail;
        int open_total = 0;
        double rt = 0, rtd = 0, r7 = 0, r30 = 0, crypto_r = 0;
        std::map<std::string, double> crypto_by_reason;

        std::vector<const StallRollUp*> rolls;
        rolls.reserve(books_.size() + mirrors_.size());
        for (const auto& b : books_)   rolls.push_back(&b.rollup());
        for (const auto& m : mirrors_) rolls.push_back(&m.rollup());
        for (const auto* rp : rolls) {
            const StallRollUp& R = *rp;
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
            for (const auto& d : R.closed_detail) closed_detail.push_back(&d);
            open_total += R.open_companions;
            rt += R.realized_total; rtd += R.realized_today; r7 += R.realized_7d; r30 += R.realized_30d;
            crypto_r += R.crypto_realized;
            for (const auto& kv : R.crypto_by_reason) crypto_by_reason[kv.first] = round2(crypto_by_reason[kv.first] + kv.second);
        }
        for (auto& kv : per_engine) kv.second.realized = round2(kv.second.realized);
        rt = round2(rt); rtd = round2(rtd); r7 = round2(r7); r30 = round2(r30);

        std::ostringstream o; o << std::fixed << std::setprecision(2);
        o << "{\"updated\":"; jstr(o, updated_utc(now));
        o << ",\"source\":\"stall_companion_cpp\",\"n_books\":" << (books_.size() + mirrors_.size())
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
        o << ",\"open_detail\":["; { bool fo = true; for (const auto* d : open_detail) { if (!fo) o << ','; fo = false; o << "{\"book\":"; jstr(o, d->book); o << ",\"eng\":"; jstr(o, d->eng); o << ",\"sym\":"; jstr(o, d->sym); o << ",\"side\":"; jstr(o, d->side); o << ",\"entry\":" << d->entry << ",\"mfe_pct\":" << d->mfe_pct << ",\"mfe_usd\":" << d->mfe_usd << ",\"stall\":" << d->stall << ",\"upnl\":" << d->upnl << ",\"eligible\":" << (d->eligible ? "true" : "false") << "}"; } } o << "]";
        // Recent banked clips, newest first, capped 30 — the desk renders these under the
        // ENGINE LEDGER so a COMP-BANK / ALL-TIME increase is always explained by rows.
        std::sort(closed_detail.begin(), closed_detail.end(),
                  [](const StallClosedDet* a, const StallClosedDet* b) { return a->ts > b->ts; });
        if (closed_detail.size() > 30) closed_detail.resize(30);
        o << ",\"closed_detail\":["; { bool fc = true; for (const auto* d : closed_detail) { if (!fc) o << ','; fc = false; o << "{\"ts\":" << d->ts << ",\"book\":"; jstr(o, d->book); o << ",\"reason\":"; jstr(o, d->reason); o << ",\"eng\":"; jstr(o, d->eng); o << ",\"sym\":"; jstr(o, d->sym); o << ",\"side\":"; jstr(o, d->side); o << ",\"entry\":" << d->entry << ",\"pnl\":" << d->pnl << "}"; } } o << "]}";

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
