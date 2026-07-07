// =============================================================================
// mgc_tf_feed_parity.cpp -- registry §6 PARITY TEST for the S-2026-07-07 MGC
// TrendFollow port. Drives the PRODUCTION feed path (MgcFastDonchianFeed.hpp
// poll_mgc_feed: 30m rows -> on_tick l/h/c + H1/H4 bucket aggregation) over
// the same MGC history the faithful harness used, and compares net/n against
// XauTrendFollow4h2hBacktest MGC=1 (which drives real engines on pre-built
// H4/H1 bars). Deviations should be granularity-only (30m intrabar manage is
// finer than the harness's per-bar l/h/c).
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/mgc_tf_feed_parity.cpp \
//            -o backtest/mgc_tf_feed_parity
// RUN:   ./backtest/mgc_tf_feed_parity /Users/jo/Tick/mgc_30m_hist.csv
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <atomic>

#include "OmegaTradeLedger.hpp"
#include "MgcFastDonchianFeed.hpp"

int main(int argc, char** argv){
    if(argc<2){ std::fprintf(stderr,"usage: %s <mgc_30m_csv>\n",argv[0]); return 1; }
    // production config mirror (omega_main.hpp MGC port block), warmup skipped:
    // whole file replays through the live path with floor=0.
    g_mgc_fastdon.enabled = true;   // poll_mgc_feed early-outs if fastdon disabled
    g_mgc_fastdon.shadow_mode = true;
    g_mgc_volbrk.enabled = false;
    g_mgc_tf_4h.enabled      = true;  g_mgc_tf_4h.shadow_mode = true;
    g_mgc_tf_4h.lot          = 1.0;   g_mgc_tf_4h.max_spread  = 1.50;
    g_mgc_tf_4h.LOSS_CUT_PCT = getenv("LC4")?atof(getenv("LC4")):1.5;
    g_mgc_tf_4h.cell_enable_mask = 0xC9;
    g_mgc_tf_4h.use_vol_band_gate = true;
    g_mgc_tf_4h.vol_band_low_pct = 0.30; g_mgc_tf_4h.vol_band_high_pct = 0.85;
    g_mgc_tf_4h.cell_vol_band_mask = 0x8;
    g_mgc_tf_4h.min_impulse_atr = 0.5; g_mgc_tf_4h.min_adx_entry = 15.0;
    g_mgc_tf_4h.ledger_prefix = "MgcTF4h_"; g_mgc_tf_4h.ledger_symbol = "MGC";
    g_mgc_tf_4h.init();
    g_mgc_tf_2h.enabled      = true;  g_mgc_tf_2h.shadow_mode = true;
    g_mgc_tf_2h.lot          = 1.0;   g_mgc_tf_2h.max_spread  = 1.50;
    g_mgc_tf_2h.LOSS_CUT_PCT = getenv("LC2")?atof(getenv("LC2")):0.5;
    g_mgc_tf_2h.use_adx_gate = true;  g_mgc_tf_2h.adx_min = 25.0;
    g_mgc_tf_2h.cell_adx_mask = 0xB;
    g_mgc_tf_2h.use_vol_band_gate = true;
    g_mgc_tf_2h.vol_band_low_pct = 0.30; g_mgc_tf_2h.vol_band_high_pct = 0.85;
    g_mgc_tf_2h.cell_vol_band_mask = 0x4;
    g_mgc_tf_2h.ledger_prefix = "MgcTF2h_"; g_mgc_tf_2h.ledger_symbol = "MGC";
    g_mgc_tf_2h.init();
    g_mgc_tf_floor_ts = 0;

    struct S { double net=0,gw=0,gl=0,cur=0,peak=0,mdd=0; int n=0;
        void rec(double u){ n++; net+=u; if(u>=0)gw+=u; else gl-=u;
            cur+=u; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur; }
        double pf()const{ return gl>0?gw/gl:99.9; } };
    static S s4, s2;
    auto cb=[](const omega::TradeRecord& tr){
        // tr.pnl = pts*lot(1.0). Comm: $0.208/oz RT = 0.208 pts at $1/pt-oz scale
        // (harness ibkr_comm_usd MGC branch), spread already crossed in fills.
        const double usd = tr.pnl - 0.208;
        if(tr.engine.rfind("MgcTF4h_",0)==0) s4.rec(usd);
        else if(tr.engine.rfind("MgcTF2h_",0)==0) s2.rec(usd);
    };
    poll_mgc_feed(argv[1], "/nonexistent.json", cb);
    std::printf("[PARITY] feed-path replay of %s  LC4=%.2f LC2=%.2f\n", argv[1],
                g_mgc_tf_4h.LOSS_CUT_PCT, g_mgc_tf_2h.LOSS_CUT_PCT);
    std::printf("  MgcTF4h  n=%d  net=$%+.1f  PF=%.2f  maxDD=$%.0f   (harness ref: n=285 net=+$4404 PF1.54 DD$1331)\n",
                s4.n, s4.net, s4.pf(), s4.mdd);
    std::printf("  MgcTF2h  n=%d  net=$%+.1f  PF=%.2f  maxDD=$%.0f   (harness ref: n=801 net=+$4281 PF1.21 DD$3266)\n",
                s2.n, s2.net, s2.pf(), s2.mdd);
    return 0;
}
