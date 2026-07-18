// goldmimic_catchup_bt.cpp — OMEGA CERT for GoldTrendMimicBook restore-path BOUNDED
// CATCH-UP (S-2026-07-18; Config::catchup_max_age_secs / on_trend_restore / cu
// gap-through guard). Own cert — the crypto bounded-catch-up cert is never carried
// across engines.
//
// Background: the parent trend engines (XauTf4h / XauTfD1) re-fire on_trend_open from
// persist_restore on EVERY restart while holding — post-arm, unconditionally, with the
// ORIGINAL entry px and no age bound. With legs persisted that spawned DUPLICATE
// pending legs at a stale trigger (the closed.csv "time-travel fill" incident class).
// The port routes restore fires through on_trend_restore(), which spawns ONLY under
// the bounded-catch-up conditions (never-seen trigger + flat book + age bound + floored
// confirmed-entry family) and marks spawned legs cu so a BE level that was never traded
// inside the entry bar CANCELS instead of booking a backdated level fill.
//
// Drives the REAL GoldTrendMimicBook (no model reimplementation). Books = ALL THREE
// restore-fire-receiving books (never-half-symbols): XauTf4h (4 legs) + MgcTF4h
// (2 legs, rt5) on XAU H4, XauTfD1 (2 legs) on XAU D1 — engine_init config parity.
//
// Scenarios per trigger (each in its own fresh temp state dir):
//   A  always-on      : on_trend_open at T, feed every bar. The reference clips.
//   R  normal restart : on_trend_open at T, feed k bars, RESTART (new book, same state
//                       dir — legs reload), parent re-fires via on_trend_restore, feed
//                       the rest. GATE: clips == A exactly (the restore fire must DEDUP
//                       — the OLD unconditional refire spawned a 2nd leg set here).
//   L  state loss     : same but state files DELETED before restart (the only case a
//                       restore fire may legitimately spawn). If BE was NOT crossed
//                       during the k lost bars: GATE clips == A exactly (recovered legs
//                       book the same BE-level entries). If BE WAS crossed: the book
//                       cannot know (no history) — the cu gap-through guard cancels
//                       gap-over entries; what books instead is REPORTED and gated only
//                       against the floored envelope (worst clip >= -rt_cost, the
//                       BE_FLOOR bound — no new tail class).
//
// Build: g++ -std=c++17 -O2 -Iinclude backtest/goldmimic_catchup_bt.cpp -o /tmp/gmcu
// Data:  /Users/jo/Tick/XAUUSD_2022_2026.h1.csv (CERTIFIED CLEAN; .certified stamp) —
//        aggregated in-harness to H4 / D1 buckets.
// Env:   GMCU_MAXTRIG(200) GMCU_MOM(0.6) GMCU_K(1,2,4)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <memory>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>
#include "GoldTrendMimicLadder.hpp"

using omega::GoldTrendMimicBook;

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_h1(const std::string& path){
    std::vector<Bar> b; std::ifstream f(path); std::string ln;
    while (std::getline(f, ln)) {
        if (ln.empty() || !isdigit((unsigned char)ln[0])) continue;
        Bar x{}; if (std::sscanf(ln.c_str(), "%lld,%lf,%lf,%lf,%lf",
                     (long long*)&x.ts, &x.o, &x.h, &x.l, &x.c) == 5 && x.c > 0) b.push_back(x);
    }
    return b;
}
static std::vector<Bar> bucket(const std::vector<Bar>& h1, int64_t secs){
    std::vector<Bar> out;
    for (const Bar& x : h1) {
        const int64_t b0 = x.ts - (x.ts % secs);
        if (out.empty() || out.back().ts != b0) out.push_back({b0, x.o, x.h, x.l, x.c});
        else { Bar& y = out.back(); y.h = std::max(y.h, x.h); y.l = std::min(y.l, x.l); y.c = x.c; }
    }
    return out;
}

struct BookSpec { const char* tag; int nlegs; double arm, lc, be, rt; int cap, pend; int64_t bar_secs; };

struct Clip { std::string eng; int64_t ets, xts; double entry, exit; std::string reason; };

static GoldTrendMimicBook::Config make_cfg(const BookSpec& bs, const std::string& dir, bool catchup){
    GoldTrendMimicBook::Config c;
    c.trigger_tag = bs.tag; c.live_sym = "XAUUSD";
    static const char* TAGS[4] = {"T1","T2","W1","W2"};
    for (int i = 0; i < bs.nlegs; ++i) c.legs.push_back({TAGS[i], i < bs.nlegs/2 ? 0.08 : 0.20});
    c.arm_pct = bs.arm; c.lc_pct = bs.lc; c.cap_bars = bs.cap; c.rt_cost_bp = bs.rt;
    c.be_entry_pct = bs.be; c.no_prebe_loss = true; c.pend_bars = bs.pend;
    c.catchup_max_age_secs = catchup ? 86400 : 0;
    c.catchup_bar_secs = bs.bar_secs;
    c.state_path = dir + "/state.txt"; c.closed_path = dir + "/closed.csv";
    return c;
}

struct Run { std::vector<Clip> clips; };

static std::unique_ptr<GoldTrendMimicBook> mk_book(const BookSpec& bs, const std::string& dir,
                                                   bool catchup, Run& r){
    auto b = std::make_unique<GoldTrendMimicBook>(make_cfg(bs, dir, catchup));
    b->set_exec(
        [](const std::string&, bool, double, double)->std::string{ return "t"; },
        [](const std::string&, bool, double, double, const std::string&){},
        [](const std::string&, double, double)->bool{ return true; },
        [&r](const std::string& eng, const std::string&, bool, double entry, double exit,
             double, int64_t ets, int64_t xts, const char* reason, double, double){
            r.clips.push_back({eng, ets, xts, entry, exit, reason}); });
    return b;
}

int main(){
    auto envs=[&](const char* k, const char* d){ const char* v=getenv(k); return std::string(v?v:d); };
    const int maxtrig = atoi(envs("GMCU_MAXTRIG","200").c_str());
    const double momthr = atof(envs("GMCU_MOM","0.6").c_str());
    std::vector<int> Ks;
    { std::stringstream ss(envs("GMCU_K","1,2,4")); std::string t; while(std::getline(ss,t,',')) Ks.push_back(atoi(t.c_str())); }

    auto h1 = load_h1("/Users/jo/Tick/XAUUSD_2022_2026.h1.csv");
    if (h1.size() < 1000) { std::fprintf(stderr, "no XAU H1 data\n"); return 2; }
    auto h4 = bucket(h1, 14400);
    auto d1 = bucket(h1, 86400);

    static const BookSpec BOOKS[] = {   // engine_init parity
        {"XauTf4h", 4, 0.25, 1.5, 0.15, 15.0, 12, 6, 14400},
        {"MgcTF4h", 2, 0.25, 1.5, 0.15,  5.0, 12, 6, 14400},
        {"XauTfD1", 2, 0.25, 2.0, 0.15, 15.0,  8, 4, 86400},
    };

    fflush(stdout); int saved = dup(1); { int dn=open("/dev/null",O_WRONLY); dup2(dn,1); close(dn); }
    auto say=[&](const char* fmt, ...){ va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); };

    // sorted pairwise compare with RELATIVE 1e-8 price tolerance — a quantized key
    // (llround(px*1e4)) is boundary-fragile: a p15 file round-trip can shift the last
    // ulp across a .5 boundary and split identical clips into different keys (caught
    // on XauTf4h 2625.95515). p15 round-trip error ~1e-12 rel; real divergence >=1e-5.
    auto same=[&](const Run& x, const Run& y){
        if (x.clips.size() != y.clips.size()) return false;
        auto ord=[](const Clip& a, const Clip& b){
            return std::tie(a.eng,a.ets,a.xts,a.reason,a.entry) < std::tie(b.eng,b.ets,b.xts,b.reason,b.entry); };
        auto xs = x.clips, ys = y.clips;
        std::sort(xs.begin(), xs.end(), ord); std::sort(ys.begin(), ys.end(), ord);
        for (size_t i = 0; i < xs.size(); ++i) {
            const Clip& a = xs[i]; const Clip& b = ys[i];
            if (a.eng != b.eng || a.ets != b.ets || a.xts != b.xts || a.reason != b.reason) return false;
            if (std::fabs(a.entry-b.entry) > std::max(1e-9, std::fabs(a.entry)*1e-8)) return false;
            if (std::fabs(a.exit -b.exit ) > std::max(1e-9, std::fabs(a.exit )*1e-8)) return false;
        }
        return true;
    };

    int g_fail = 0;
    for (const auto& bs : BOOKS) {
        const auto& bars = (bs.bar_secs == 14400) ? h4 : d1;
        // triggers: |mom over 6 bars| >= momthr%, isolated (no overlap with the sim horizon)
        const int horizon = bs.cap + bs.pend + 4;
        std::vector<int> trig; std::vector<int> tdir; int last = -1000000;
        for (int i = 8; i + horizon + 2 < (int)bars.size(); ++i) {
            const double mom = (bars[i].c / bars[i-6].c - 1.0) * 100.0;
            if (std::fabs(mom) >= momthr && i - last > horizon + 2 && (int)trig.size() < maxtrig) {
                trig.push_back(i); tdir.push_back(mom > 0 ? 1 : -1); last = i;
            }
        }
        int nR_fail = 0, nL_exact = 0, nL_exact_fail = 0, nL_crossed = 0, nL_env_fail = 0, nA_clip = 0;
        int nL_bounded = 0, nL_bounded_fail = 0, nL_conserv = 0;
        double L_worst = 0.0, L_dnet = 0.0;
        for (size_t t = 0; t < trig.size(); ++t) {
            const int T = trig[t]; const int dir = tdir[t];
            const double px = bars[T].c; const int64_t ts = bars[T].ts;
            const int end = std::min((int)bars.size(), T + horizon);
            auto feed=[&](GoldTrendMimicBook& b, int from, int to){
                for (int i = from; i < to; ++i) b.on_h1_bar(bars[i].h, bars[i].l, bars[i].c, bars[i].ts, true, true); };
            auto net_of=[&](const Run& r){ double n=0; for (const auto& c : r.clips)
                n += dir*( c.exit / c.entry - 1.0)*100.0 - bs.rt/100.0; return n; };
            // A: always-on
            Run rA; char dA[] = "/tmp/gmcu.XXXXXX"; std::string da = mkdtemp(dA);
            { auto b = mk_book(bs, da, true, rA); b->on_trend_open(dir, px, ts); feed(*b, T+1, end); }
            nA_clip += (int)rA.clips.size();
            // was BE crossed within the first k bars? (per k below)
            for (int k : Ks) {
                if (T + 1 + k >= end) continue;
                bool crossed = false;
                const double lvl = px * (1.0 + dir * bs.be / 100.0);
                for (int i = T + 1; i < T + 1 + k; ++i)
                    if (dir * ((dir > 0 ? bars[i].h : bars[i].l) - lvl) >= 0
                        && dir * ((dir > 0 ? bars[i].h : bars[i].l) / px - 1.0) * 100.0 >= bs.be) { crossed = true; break; }
                // R: normal restart (state persists) — restore fire must DEDUP
                Run rR; char dR[] = "/tmp/gmcu.XXXXXX"; std::string drr = mkdtemp(dR);
                { auto b = mk_book(bs, drr, true, rR); b->on_trend_open(dir, px, ts); feed(*b, T+1, T+1+k); }
                { auto b = mk_book(bs, drr, true, rR);            // restart: reload state
                  b->on_trend_restore(dir, px, ts, bars[T+k].ts); // parent still holds -> re-fire
                  feed(*b, T+1+k, end); }
                if (!same(rA, rR)) {
                    nR_fail++;
                    if (getenv("GMCU_DUMP")) {
                        say("DUMP %s T=%d k=%d dir=%d px=%.2f\n", bs.tag, T, k, dir, px);
                        for (const auto& c : rA.clips) say("  A %s ets=%lld xts=%lld e=%.10f x=%.10f %s\n",
                            c.eng.c_str(), (long long)c.ets, (long long)c.xts, c.entry, c.exit, c.reason.c_str());
                        for (const auto& c : rR.clips) say("  R %s ets=%lld xts=%lld e=%.10f x=%.10f %s\n",
                            c.eng.c_str(), (long long)c.ets, (long long)c.xts, c.entry, c.exit, c.reason.c_str());
                        unsetenv("GMCU_DUMP");
                    }
                }
                std::string c1 = "rm -rf '" + drr + "'"; (void)!system(c1.c_str());
                // L: state loss — the legitimate catch-up spawn case
                Run rL; char dL[] = "/tmp/gmcu.XXXXXX"; std::string dll = mkdtemp(dL);
                { Run tmp; auto b = mk_book(bs, dll, true, tmp); b->on_trend_open(dir, px, ts); feed(*b, T+1, T+1+k); }
                { std::string c2 = "rm -f '" + dll + "/state.txt' '" + dll + "/state.txt.open' '" + dll + "/closed.csv'";
                  (void)!system(c2.c_str()); }
                { auto b = mk_book(bs, dll, true, rL);
                  b->on_trend_restore(dir, px, ts, bars[T+k].ts);
                  feed(*b, T+1+k, end); }
                // is the catch-up spawn expected at all? (age bound 24h + pend-alive rule —
                // for the D1 book k>=2 exceeds the 24h bound: the certified BOUNDED skip)
                const int64_t age = bars[T+k].ts - ts;
                const bool expected_spawn = age <= 86400 && (age / bs.bar_secs) < bs.pend;
                if (!expected_spawn) {
                    nL_bounded++;
                    if (!rL.clips.empty()) nL_bounded_fail++;   // bounded skip must book NOTHING
                } else if (!crossed) {
                    nL_exact++;
                    // Accepted CONSERVATIVE CANCEL: when the first bar the recovered leg sees
                    // is itself a gap bar over the BE level (weekend reopen), the book cannot
                    // distinguish "crossed during the outage" (forbidden backfill) from "gap-
                    // fill at the reopen" (A books at level) — it resolves to NO TRADE (never
                    // a worse/backdated fill). Verify the gap really exists on A's entry bar.
                    bool conservative_ok = false;
                    if (!same(rA, rL) && rL.clips.empty() && !rA.clips.empty()) {
                        const int64_t aets = rA.clips.front().ets;
                        for (int i = T + 1; i < end; ++i) if (bars[i].ts == aets) {
                            const double adv = dir > 0 ? bars[i].l : bars[i].h;
                            if (dir * (adv - lvl) > 0.0) conservative_ok = true;
                            break;
                        }
                        if (conservative_ok) nL_conserv++;
                    }
                    if (!same(rA, rL) && !conservative_ok) {
                        nL_exact_fail++;
                        if (getenv("GMCU_DUMPL")) {
                            say("DUMPL %s T=%d k=%d dir=%d px=%.4f trig_ts=%lld now=%lld lvl=%.6f\n",
                                bs.tag, T, k, dir, px, (long long)ts, (long long)bars[T+k].ts, lvl);
                            for (const auto& c : rL.clips) say("  L %s ets=%lld xts=%lld e=%.10f x=%.10f %s\n",
                                c.eng.c_str(), (long long)c.ets, (long long)c.xts, c.entry, c.exit, c.reason.c_str());
                            for (const auto& c : rA.clips) say("  A %s ets=%lld xts=%lld e=%.10f x=%.10f %s\n",
                                c.eng.c_str(), (long long)c.ets, (long long)c.xts, c.entry, c.exit, c.reason.c_str());
                            unsetenv("GMCU_DUMPL");
                        }
                    }
                } else {
                    nL_crossed++;
                    for (const auto& c : rL.clips) {   // floored-envelope gate: no new tail class
                        const double net = dir*(c.exit/c.entry - 1.0)*100.0 - bs.rt/100.0;
                        if (net < L_worst) L_worst = net;
                        if (net < -(bs.rt/100.0) - 1e-6) nL_env_fail++;
                    }
                    L_dnet += net_of(rL) - net_of(rA);
                }
                std::string c3 = "rm -rf '" + dll + "'"; (void)!system(c3.c_str());
            }
            std::string c0 = "rm -rf '" + da + "'"; (void)!system(c0.c_str());
        }
        const bool fail = nR_fail > 0 || nL_exact_fail > 0 || nL_env_fail > 0 || nL_bounded_fail > 0;
        if (fail) g_fail++;
        say("%-8s triggers=%zu A_clips=%d | R(dedup) fails=%d | L(exact) n=%d fails=%d conserv=%d | "
            "L(bounded-skip) n=%d fails=%d | L(crossed SKIP-class) n=%d env_fails=%d worst=%.3f%% dnet_sum=%+.2f%% => %s\n",
            bs.tag, trig.size(), nA_clip, nR_fail, nL_exact, nL_exact_fail, nL_conserv,
            nL_bounded, nL_bounded_fail, nL_crossed, nL_env_fail, L_worst, L_dnet, fail ? "FAIL" : "OK");
    }
    fflush(stdout); dup2(saved,1); close(saved);
    std::printf("GOLDMIMIC CATCHUP CERT => %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
