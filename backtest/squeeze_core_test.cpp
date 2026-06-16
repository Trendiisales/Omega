// squeeze_core_test.cpp
#include "SqueezeSlingshotCore.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <vector>

using namespace squeeze;

static std::vector<Bar> random_walk(int n, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> step(0.0, 1.0);
    std::uniform_real_distribution<double> rngd(0.2, 1.4);
    std::vector<Bar> bars;
    double px = 2000.0;
    for (int i = 0; i < n; ++i) {
        const double drift = (i < n / 2) ? 0.6 : -0.4;
        px += drift + step(rng) * 3.0;
        if (px < 50.0) px = 50.0;
        const double rng_bar = rngd(rng) * 4.0;
        Bar b;
        b.open = px + step(rng) * rng_bar * 0.3;
        b.close = px + step(rng) * rng_bar * 0.3;
        b.high = std::max(b.open, b.close) + std::fabs(step(rng)) * rng_bar;
        b.low = std::min(b.open, b.close) - std::fabs(step(rng)) * rng_bar;
        b.volume = 1000.0 + i;
        bars.push_back(b);
    }
    return bars;
}

int main() {
    Params p;  // defaults

    // ---- 1) Dump indicators on a random walk for the NumPy cross-check ----
    auto bars = random_walk(400, 12345);
    {
        Evaluator ev(p);
        std::ofstream out("dump.csv");
        out << "i,high,low,close,warm,basis,upperBB,lowerBB,kc_mid,rangema,momentum,"
               "ema_fast,ema_a,ema_b,ema_slow,atr,tier,entry\n";
        out << std::fixed << std::setprecision(10);
        for (std::size_t i = 0; i < bars.size(); ++i) {
            BarSignal s = ev.update(bars[i]);
            out << i << "," << bars[i].high << "," << bars[i].low << "," << bars[i].close << ","
                << (s.warm ? 1 : 0) << "," << s.basis << "," << s.upperBB << "," << s.lowerBB << ","
                << s.kc_mid << "," << s.rangema << "," << s.momentum << "," << s.ema_fast << ","
                << s.ema_a << "," << s.ema_b << "," << s.ema_slow << "," << s.atr << ","
                << s.squeeze_tier << "," << static_cast<int>(s.entry) << "\n";
        }
    }

    // ---- 2) Lookahead-free check: signal at t must be identical whether or not t+1.. exist ----
    int checks = 0, mismatches = 0;
    {
        Evaluator full(p);
        std::vector<BarSignal> full_sig;
        for (const auto& b : bars) full_sig.push_back(full.update(b));

        for (int t : {60, 90, 150, 220, 300, 399}) {
            Evaluator prefix(p);
            BarSignal last;
            for (int i = 0; i <= t; ++i) last = prefix.update(bars[static_cast<std::size_t>(i)]);
            const BarSignal& f = full_sig[static_cast<std::size_t>(t)];
            const bool same =
                last.warm == f.warm && last.squeeze_tier == f.squeeze_tier &&
                last.entry == f.entry &&
                std::fabs(last.momentum - f.momentum) < 1e-9 &&
                std::fabs(last.basis - f.basis) < 1e-9 &&
                std::fabs(last.atr - f.atr) < 1e-9;
            ++checks;
            if (!same) ++mismatches;
        }
    }

    // ---- 3) Synthetic squeeze: low-vol compression then an uptrending expansion ----
    int entries_long = 0, max_tier = 0, squeeze_bars = 0;
    {
        std::vector<Bar> sq;
        double px = 1000.0;
        // 80 bars of very tight, slightly rising chop (forces BB inside KC)
        for (int i = 0; i < 80; ++i) {
            px += 0.05;
            Bar b;
            b.open = px - 0.05;
            b.close = px + 0.05;
            b.high = px + 0.15;
            b.low = px - 0.15;
            b.volume = 1000;
            sq.push_back(b);
        }
        // 40 bars of a clean rising expansion (squeeze should fire upward)
        for (int i = 0; i < 40; ++i) {
            px += 2.2;
            Bar b;
            b.open = px - 1.0;
            b.close = px + 1.2;
            b.high = px + 1.8;
            b.low = px - 1.2;
            b.volume = 1000;
            sq.push_back(b);
        }
        Params sp;  // tuned so the gate can fire within the synthetic budget
        sp.bb_length = 10; sp.kc_length = 10; sp.mom_length = 10;
        sp.ema_fast = 3; sp.ema_a = 5; sp.ema_b = 8; sp.ema_slow = 13;
        sp.require_momo_below_zero_for_long = false;
        Evaluator ev(sp);
        for (const auto& b : sq) {
            BarSignal s = ev.update(b);
            if (s.squeeze_on) ++squeeze_bars;
            max_tier = std::max(max_tier, s.squeeze_tier);
            if (s.entry == Signal::EnterLong) ++entries_long;
        }
    }

    std::cout << "lookahead checks=" << checks << " mismatches=" << mismatches << "\n";
    std::cout << "synthetic: squeeze_bars=" << squeeze_bars << " max_tier=" << max_tier
              << " long_entries=" << entries_long << "\n";

    const bool ok = (mismatches == 0) && (squeeze_bars > 0) && (max_tier >= 1) && (entries_long >= 1);
    std::cout << (ok ? "CORE SELF-TESTS PASS" : "CORE SELF-TESTS FAIL") << "\n";
    return ok ? 0 : 1;
}
