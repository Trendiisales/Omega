#pragma once
// =============================================================================
// OmegaEVGuard.hpp -- Expected-value pre-fire gate (universal)
// =============================================================================
//
// 2026-05-12 (Path B pilot, Claude / Jo): Universal expected-value gate
//   intended to sit IN FRONT OF the per-engine FIRE decision. The thesis
//   under test (MidScalperGold pilot): only fire when the conditional
//   expected gross > expected cost + safety margin.
//
//   At decision time (engine has produced a candidate FIRE with side,
//   sl_dist, tp_dist, current spread, current hour, current ATR regime),
//   the guard looks up the empirical (win_rate, mean_winner, mean_loser)
//   for the matching feature bucket and decides fire/skip based on:
//
//     ev = wr * mean_winner + (1 - wr) * mean_loser - expected_cost
//     fire if ev > safety_margin   (default $0.20 USD)
//
//   The bucket index is (spread_bucket, hour_bucket). Lookup falls back
//   to (spread_bucket) marginal, then global mean, if the joint bucket
//   has fewer than MIN_BUCKET_N=30 historical samples. This keeps the
//   estimator stable in long-tail cells while still differentiating where
//   we have signal.
//
//   The stats table is supplied externally -- the guard does no learning
//   itself. In the pilot harness the table is computed from a baseline
//   backtest pass, then re-applied on a second pass. For live deployment
//   the table can be refreshed periodically from rolling trade history.
//
//   Cost model (XAUUSD per 0.01 lot): $0.66 fixed + 2 * spread_pt.
//   For other instruments, supply cost_per_lot_floor and
//   tick_usd_per_lot via the cost params.
//
// USAGE
//   omega::EVGuard guard;
//   guard.load_stats("ev_stats.csv");        // bucket means + win-rates
//   guard.set_cost_xauusd_01lot();           // sets cost params for XAU 0.01
//   if (!guard.should_fire(spread_pt, hour_utc)) return;  // pre-fire skip
//
//   At end of a trade:
//   guard.record_trade(side, gross_usd, spread_at_entry_pt, hour_utc);
//
//   guard.save_stats("ev_stats.csv");        // persist for next session
//
// =============================================================================

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace omega {

class EVGuard {
public:
    // Bucket layout: 6 spread buckets x 24 hours = 144 cells.
    //   Spread buckets (pt):
    //     0: 0.0 - 0.5
    //     1: 0.5 - 1.0
    //     2: 1.0 - 1.5
    //     3: 1.5 - 2.0
    //     4: 2.0 - 2.5
    //     5: 2.5+
    static constexpr int N_SPREAD = 6;
    static constexpr int N_HOUR   = 24;
    static constexpr int MIN_BUCKET_N = 15;  // min samples for joint bucket
    static constexpr int MIN_SPREAD_MARGINAL_N = 25;
    static constexpr double DEFAULT_SAFETY_MARGIN_USD = 0.20;

    struct BucketStats {
        int    n          = 0;
        int    n_wins     = 0;
        double sum_winner = 0.0;   // gross USD over winners
        double sum_loser  = 0.0;   // gross USD over losers (negative)
        // Derived (computed on freeze())
        double win_rate    = 0.0;
        double mean_winner = 0.0;
        double mean_loser  = 0.0;
    };

    // Cost model coefficients: cost_usd = fixed + per_spread_pt * spread_pt
    double cost_fixed_usd      = 0.66;   // XAUUSD 0.01 lot default
    double cost_per_spread_pt  = 2.00;   // 2 * spread * $1/pt
    double safety_margin_usd   = DEFAULT_SAFETY_MARGIN_USD;

    // Joint, marginal, global stats. Index helpers below.
    std::array<BucketStats, N_SPREAD * N_HOUR> joint{};
    std::array<BucketStats, N_SPREAD>          spread_marg{};
    BucketStats                                global{};

    bool stats_frozen = false;

    static inline int spread_bucket(double spread_pt) noexcept {
        if (spread_pt < 0.5) return 0;
        if (spread_pt < 1.0) return 1;
        if (spread_pt < 1.5) return 2;
        if (spread_pt < 2.0) return 3;
        if (spread_pt < 2.5) return 4;
        return 5;
    }
    static inline int hour_bucket(int hour_utc) noexcept {
        if (hour_utc < 0) return 0;
        if (hour_utc > 23) return 23;
        return hour_utc;
    }
    static inline int joint_ix(int sb, int hb) noexcept {
        return sb * N_HOUR + hb;
    }

    void set_cost_xauusd_01lot() noexcept {
        // commission $0.06 + slippage $0.60 = $0.66 fixed
        // spread cost = 2 * spread_pt * $1/pt for 0.01 lot (USD_PER_PT = $100, 0.01 lot)
        cost_fixed_usd     = 0.66;
        cost_per_spread_pt = 2.00;
    }

    double expected_cost_usd(double spread_pt) const noexcept {
        if (spread_pt < 0.0) spread_pt = 0.0;
        return cost_fixed_usd + cost_per_spread_pt * spread_pt;
    }

    void reset_stats() noexcept {
        joint.fill(BucketStats{});
        spread_marg.fill(BucketStats{});
        global = BucketStats{};
        stats_frozen = false;
    }

    // record_trade -- training-time only. gross_usd is the COST-FREE gross PnL
    // for the trade (sign retained: positive = winner, negative = loser).
    void record_trade(double gross_usd, double spread_at_entry_pt, int hour_utc) noexcept {
        const int sb = spread_bucket(spread_at_entry_pt);
        const int hb = hour_bucket(hour_utc);
        const int ix = joint_ix(sb, hb);
        auto record = [gross_usd](BucketStats& bs){
            bs.n += 1;
            if (gross_usd > 0) { bs.n_wins += 1; bs.sum_winner += gross_usd; }
            else               { bs.sum_loser += gross_usd; }
        };
        record(joint[ix]);
        record(spread_marg[sb]);
        record(global);
        stats_frozen = false;
    }

    // freeze() -- compute derived stats from raw counters.
    void freeze() noexcept {
        auto _finalize = [](BucketStats& bs) {
            if (bs.n <= 0) {
                bs.win_rate = 0.0; bs.mean_winner = 0.0; bs.mean_loser = 0.0; return;
            }
            bs.win_rate    = (double)bs.n_wins / bs.n;
            const int n_losers = bs.n - bs.n_wins;
            bs.mean_winner = (bs.n_wins > 0) ? bs.sum_winner / bs.n_wins : 0.0;
            bs.mean_loser  = (n_losers > 0) ? bs.sum_loser / n_losers : 0.0;
        };
        for (auto& bs : joint)       _finalize(bs);
        for (auto& bs : spread_marg) _finalize(bs);
        _finalize(global);
        stats_frozen = true;
    }

    // Resolve which stats to use for a (sb, hb) cell with fallbacks.
    const BucketStats& resolve(int sb, int hb) const noexcept {
        const int ix = joint_ix(sb, hb);
        if (joint[ix].n >= MIN_BUCKET_N)      return joint[ix];
        if (spread_marg[sb].n >= MIN_SPREAD_MARGINAL_N) return spread_marg[sb];
        return global;
    }

    struct EVResult {
        double ev_usd         = 0.0;
        double expected_cost  = 0.0;
        double expected_gross = 0.0;
        double win_rate       = 0.0;
        double mean_winner    = 0.0;
        double mean_loser     = 0.0;
        int    bucket_n       = 0;
        int    fallback_level = 0;  // 0=joint, 1=spread_marg, 2=global
        bool   fire           = false;
    };

    EVResult evaluate(double spread_pt, int hour_utc) const noexcept {
        EVResult r{};
        const int sb = spread_bucket(spread_pt);
        const int hb = hour_bucket(hour_utc);
        const int ix = joint_ix(sb, hb);
        const BucketStats* bs = nullptr;
        if (joint[ix].n >= MIN_BUCKET_N) {
            bs = &joint[ix]; r.fallback_level = 0;
        } else if (spread_marg[sb].n >= MIN_SPREAD_MARGINAL_N) {
            bs = &spread_marg[sb]; r.fallback_level = 1;
        } else {
            bs = &global; r.fallback_level = 2;
        }
        r.bucket_n      = bs->n;
        r.win_rate      = bs->win_rate;
        r.mean_winner   = bs->mean_winner;
        r.mean_loser    = bs->mean_loser;
        r.expected_gross = bs->win_rate * bs->mean_winner
                         + (1.0 - bs->win_rate) * bs->mean_loser;
        r.expected_cost  = expected_cost_usd(spread_pt);
        r.ev_usd         = r.expected_gross - r.expected_cost;
        r.fire           = (r.ev_usd > safety_margin_usd);
        // If global itself has no data (very early or empty), refuse to fire
        // rather than blindly admit -- the guard is a SAFETY layer.
        if (bs->n == 0) r.fire = false;
        return r;
    }

    bool should_fire(double spread_pt, int hour_utc) const noexcept {
        return evaluate(spread_pt, hour_utc).fire;
    }

    // ------ Persistence -----------------------------------------------------
    // CSV: kind,sb,hb,n,n_wins,sum_winner,sum_loser
    //   kind = J (joint), S (spread marginal), G (global)
    bool save_stats(const char* path) const {
        std::ofstream f(path);
        if (!f.is_open()) return false;
        f << "kind,sb,hb,n,n_wins,sum_winner,sum_loser\n";
        for (int sb = 0; sb < N_SPREAD; ++sb) {
            for (int hb = 0; hb < N_HOUR; ++hb) {
                const auto& bs = joint[joint_ix(sb, hb)];
                f << "J," << sb << "," << hb << "," << bs.n << "," << bs.n_wins
                  << "," << bs.sum_winner << "," << bs.sum_loser << "\n";
            }
        }
        for (int sb = 0; sb < N_SPREAD; ++sb) {
            const auto& bs = spread_marg[sb];
            f << "S," << sb << ",-1," << bs.n << "," << bs.n_wins
              << "," << bs.sum_winner << "," << bs.sum_loser << "\n";
        }
        f << "G,-1,-1," << global.n << "," << global.n_wins
          << "," << global.sum_winner << "," << global.sum_loser << "\n";
        return true;
    }

    bool load_stats(const char* path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string line;
        if (!std::getline(f, line)) return false;  // header
        reset_stats();
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string kind, tok;
            int sb = -1, hb = -1, n = 0, nw = 0;
            double sw = 0.0, sl = 0.0;
            if (!std::getline(ss, kind, ',')) continue;
            std::getline(ss, tok, ','); sb = std::atoi(tok.c_str());
            std::getline(ss, tok, ','); hb = std::atoi(tok.c_str());
            std::getline(ss, tok, ','); n  = std::atoi(tok.c_str());
            std::getline(ss, tok, ','); nw = std::atoi(tok.c_str());
            std::getline(ss, tok, ','); sw = std::atof(tok.c_str());
            std::getline(ss, tok, ','); sl = std::atof(tok.c_str());
            BucketStats bs{}; bs.n = n; bs.n_wins = nw;
            bs.sum_winner = sw; bs.sum_loser = sl;
            if (kind == "J" && sb >= 0 && sb < N_SPREAD && hb >= 0 && hb < N_HOUR) {
                joint[joint_ix(sb, hb)] = bs;
            } else if (kind == "S" && sb >= 0 && sb < N_SPREAD) {
                spread_marg[sb] = bs;
            } else if (kind == "G") {
                global = bs;
            }
        }
        freeze();
        return true;
    }

    void dump_buckets(FILE* out) const {
        std::fprintf(out, "kind,sb,hb,n,n_wins,win_rate,mean_winner,mean_loser,exp_gross,exp_cost,ev\n");
        for (int sb = 0; sb < N_SPREAD; ++sb) {
            for (int hb = 0; hb < N_HOUR; ++hb) {
                const auto& bs = joint[joint_ix(sb, hb)];
                if (bs.n == 0) continue;
                const double exp_gross = bs.win_rate * bs.mean_winner
                                       + (1.0 - bs.win_rate) * bs.mean_loser;
                const double sp_mid = (sb < N_SPREAD - 1) ? (0.5 * sb + 0.25)
                                                          : 2.75;
                const double exp_cost = expected_cost_usd(sp_mid);
                std::fprintf(out, "J,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
                             sb, hb, bs.n, bs.n_wins, bs.win_rate,
                             bs.mean_winner, bs.mean_loser,
                             exp_gross, exp_cost, exp_gross - exp_cost);
            }
        }
        for (int sb = 0; sb < N_SPREAD; ++sb) {
            const auto& bs = spread_marg[sb];
            if (bs.n == 0) continue;
            const double exp_gross = bs.win_rate * bs.mean_winner
                                   + (1.0 - bs.win_rate) * bs.mean_loser;
            std::fprintf(out, "S,%d,-1,%d,%d,%.4f,%.4f,%.4f,%.4f,,%.4f\n",
                         sb, bs.n, bs.n_wins, bs.win_rate,
                         bs.mean_winner, bs.mean_loser,
                         exp_gross, exp_gross);
        }
        if (global.n > 0) {
            const double exp_gross = global.win_rate * global.mean_winner
                                   + (1.0 - global.win_rate) * global.mean_loser;
            std::fprintf(out, "G,-1,-1,%d,%d,%.4f,%.4f,%.4f,%.4f,,%.4f\n",
                         global.n, global.n_wins, global.win_rate,
                         global.mean_winner, global.mean_loser,
                         exp_gross, exp_gross);
        }
    }
};

} // namespace omega
