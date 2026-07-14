// =============================================================================
// mgc_tf1h_port_bt.cpp -- XauTrendFollow1h ported to MGC futures cost basis.
//
// S-2026-07-14ay. Candidate #1 from the 2026-07-14 gold scoping: the 1h family
// member PASSED the Phase-1b spot 8bp kill tier (+$7,325 PF1.40 both halves +
// 2x -- outputs/GOLD_PHASE1B_2026-07-11.md) but only the 4h/2h were ever ported
// to MGC (S-2026-07-07w, registry section 7). This harness drives the REAL
// include/XauTrendFollow1hEngine.hpp (same convention as
// backtest/XauTrendFollow1hBacktest.cpp + the MGC=1 cost mode of
// backtest/XauTrendFollow4h2hBacktest.cpp) with PRODUCTION engine_init.hpp
// config mirrored 1:1, on MGC futures execution costs.
//
// FIDELITY:
//   - H1 OHLC CSV: ts,o,h,l,c (ts seconds). Bars integrity-gated upstream.
//   - cross-spread fills: (bid,ask) = (level, level+SPREAD); engine enters
//     long@ask, exits long@bid.
//   - SL-first intrabar path: each bar fed low -> high -> close before the
//     bar-close signal evaluation (conservative adverse-first).
//   - entries fire in on_h1_bar() at bar close; managed from next bar onward.
//   - engine computes its own Wilder ATR14 (atr14_external=0).
//
// COST MODEL (certified, per 1 MGC = 10oz = $10/pt):
//   RT = $2.08 commission (0.208pt) + 0.10pt spread (crossed mechanically via
//   bid/ask) + 0.10pt slip ~= 0.41pt RT. COSTX=2 doubles all three (0.82pt).
//   Sizing basis: engine lot 0.01 == 1 MGC contract ($10/pt). usd = tr.pnl *
//   1000 (pnl = pts*size; size 0.01 -> pts*10 $). comm+slip debited per closed
//   record scaled by contracts (tr.size/0.01) -- pyramid adds each carry their
//   own contract's friction. (Adds fill at bar-close mid in the engine, so
//   their half-spread is inside the slip allowance, as in the 4h/2h port.)
//
// PRODUCTION CONFIG MIRRORED (engine_init.hpp S118 block, 2026-07-14 HEAD):
//   cell_enable_mask=0x0F, lot=0.01, max_spread=1.0, min_impulse_atr=0.5,
//   er_gate_min=0.40, er_gate_n=20, LOSS_CUT_PCT=0.5 (sweep: LC=0 per the
//   2h-port trap -- spot LC kills MGC variants), use_vol_target=true
//   unit=0.10, pyramid_max_adds=2 step=1.0 sl=3.0.
//   VT=0 runs the fixed-1-contract futures variant (vol-target's unit/ATR
//   fractional-oz sizing has no MGC equivalent below 1 contract; the 4h/2h
//   port wired fixed 1 micro) -- everything else identical.
//
// ENV: LC (LOSS_CUT_PCT, default 0.5) | COSTX (1|2, default 1) | VT (1|0,
//      default 1 = production vol-target+pyramid) |
//      M30=1 (input is a 30m ts,o,h,l,c[,v] file; drives the PRODUCTION
//      MgcFastDonchianFeed poll path -- 30m on_tick l/h/c + H1 bucket
//      aggregation -- the registry section-7 feed-path parity layer; finer
//      intrabar fills than the H1 l->h->c path, halves gap-fill optimism) |
//      SLICE_START/SLICE_END (unix sec; extra reporting window)
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/mgc_tf1h_port_bt.cpp \
//        -o /tmp/mgc_tf1h_port_bt
// RUN:   /tmp/mgc_tf1h_port_bt <h1_csv>
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <map>

#include "XauTrendFollow1hEngine.hpp"

static double SPREAD    = 0.10;    // MGC typical top-of-book (pt)
static double COMM_PT   = 0.208;   // $2.08 RT / $10 per pt, per contract
static double SLIP_PT   = 0.10;    // per RT, per contract
static const double USD_PER_PT_MGC = 10.0;   // 1 MGC micro = 10oz

struct BarCSV { int64_t ts; double o,h,l,c; };

static std::vector<BarCSV> load_csv(const char* path){
    std::vector<BarCSV> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); return v; }
    std::string line; bool first=true;
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(first){ first=false; if(line[0]<'0'||line[0]>'9') continue; }
        BarCSV b{}; double ts;
        if(std::sscanf(line.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        b.ts=(int64_t)ts; v.push_back(b);
    }
    return v;
}

struct Stat {
    int n=0, wins=0; double pnl=0, gw=0, gl=0, pk=0, eq=0, mdd=0, worst=0;
    void rec(double usd){
        ++n; pnl+=usd; if(usd>0){++wins; gw+=usd;} else gl+=std::fabs(usd);
        if(usd<worst) worst=usd;
        eq+=usd; if(eq>pk) pk=eq; if(pk-eq>mdd) mdd=pk-eq;
    }
    double pf()const{ return gl>0? gw/gl : (gw>0?999.0:0.0); }
    double wr()const{ return n? 100.0*wins/n : 0.0; }
};

int main(int argc, char** argv){
    const char* path = argc>1 ? argv[1] : "/Users/jo/Tick/mgc_2024_2026.h1.csv";
    const double LC     = getenv("LC")    ? atof(getenv("LC"))    : 0.5;
    const int    COSTX  = getenv("COSTX") ? atoi(getenv("COSTX")) : 1;
    const int    VT     = getenv("VT")    ? atoi(getenv("VT"))    : 1;
    const int64_t SL_A  = getenv("SLICE_START") ? atoll(getenv("SLICE_START")) : 0;
    const int64_t SL_B  = getenv("SLICE_END")   ? atoll(getenv("SLICE_END"))   : 0;
    SPREAD  *= COSTX; COMM_PT *= COSTX; SLIP_PT *= COSTX;

    auto bars = load_csv(path);
    if((int)bars.size() < 300){ std::fprintf(stderr,"not enough bars (%zu)\n",bars.size()); return 1; }

    omega::XauTrendFollow1hEngine eng;
    eng.shadow_mode      = true;
    eng.enabled          = true;
    // -- production engine_init.hpp mirror --
    eng.cell_enable_mask = 0x0F;
    eng.lot              = 0.01;          // == 1 MGC contract on the $10/pt basis
    eng.max_spread       = 1.0;
    eng.min_impulse_atr  = 0.5;
    eng.er_gate_min      = 0.40;
    eng.er_gate_n        = 20;
    eng.LOSS_CUT_PCT     = LC;
    eng.use_vol_target   = (VT != 0);     // prod=true; VT=0 = fixed 1-contract futures variant
    eng.vol_target_unit  = 0.10;
    eng.pyramid_max_adds = 2;
    eng.pyramid_step_atr = 1.0;
    eng.pyramid_sl_atr   = 3.0;
    eng.init();

    Stat full, h1st, h2nd, slice;
    double weeks_slice_pnl_dummy = 0; (void)weeks_slice_pnl_dummy;
    std::map<std::string,Stat> cells;
    const int N=(int)bars.size(); const int mid=N/2;
    int idx=0;
    auto cb=[&](const omega::TradeRecord& tr){
        const double contracts = tr.size / 0.01;              // 0.01 lot == 1 MGC
        const double usd = tr.pnl*1000.0                      // pts*size -> $ at $10/pt/contract
                         - (COMM_PT + SLIP_PT) * contracts * USD_PER_PT_MGC;
        full.rec(usd);
        if(idx<mid) h1st.rec(usd); else h2nd.rec(usd);
        if(SL_A && tr.exitTs>=SL_A && tr.exitTs<SL_B) slice.rec(usd);
        cells[tr.engine].rec(usd);
        cells[std::string("reason_")+tr.exitReason].rec(usd);
    };
    const int M30 = getenv("M30") ? atoi(getenv("M30")) : 0;
    if(!M30){
        for(int i=0;i<N;++i){
            idx=i; const auto& b=bars[i]; const int64_t ts_ms=b.ts*1000;
            if(i>0){
                eng.on_tick(b.l, b.l+SPREAD, ts_ms, cb);
                eng.on_tick(b.h, b.h+SPREAD, ts_ms, cb);
                eng.on_tick(b.c, b.c+SPREAD, ts_ms, cb);
            }
            omega::XauTfBar1h bar{};
            bar.bar_start_ms=ts_ms; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
            eng.on_h1_bar(bar, b.c, b.c+SPREAD, 0.0, ts_ms, cb);
        }
    } else {
        // Production MgcFastDonchianFeed-style poll path: each 30m row manages
        // open positions via on_tick l->h->c; completed UTC H1 buckets fire
        // on_h1_bar at the roll, BEFORE the new hour's first ticks.
        BarCSV h1{}; bool have=false; int64_t bkt=-1;
        for(int i=0;i<N;++i){
            idx=i; const auto& b=bars[i];
            const int64_t hb = b.ts/3600*3600;
            if(have && hb!=bkt){
                omega::XauTfBar1h bar{};
                bar.bar_start_ms=bkt*1000; bar.open=h1.o; bar.high=h1.h; bar.low=h1.l; bar.close=h1.c;
                eng.on_h1_bar(bar, h1.c, h1.c+SPREAD, 0.0, hb*1000, cb);
                have=false;
            }
            const int64_t ts_ms=b.ts*1000;
            eng.on_tick(b.l, b.l+SPREAD, ts_ms, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts_ms, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts_ms, cb);
            if(!have){ h1={hb,b.o,b.h,b.l,b.c}; have=true; bkt=hb; }
            else { h1.h=std::max(h1.h,b.h); h1.l=std::min(h1.l,b.l); h1.c=b.c; }
        }
    }
    const auto& last=bars.back();
    eng.force_close(last.c, last.c+SPREAD, last.ts*1000, cb, "EOD_FLAT");

    const double span_d = (double)(bars.back().ts - bars.front().ts)/86400.0;
    const double tpw    = full.n / (span_d/7.0);
    std::printf("[MGC-TF1h] %s  bars=%d M30=%d  px %.0f->%.0f  LC=%.2f COSTX=%d VT=%d  rt=%.3fpt(+%.2f spread)\n",
                path, N, M30, bars.front().c, bars.back().c, LC, COSTX, VT, COMM_PT+SLIP_PT, SPREAD);
    std::printf("  FULL  n=%-4d WR=%4.1f%% net=$%+9.1f PF=%4.2f worst=$%+7.1f maxDD=$%-7.0f t/wk=%.1f | H1 $%+8.1f PF%.2f  H2 $%+8.1f PF%.2f  %s\n",
                full.n, full.wr(), full.pnl, full.pf(), full.worst, full.mdd, tpw,
                h1st.pnl, h1st.pf(), h2nd.pnl, h2nd.pf(),
                (full.pnl>0&&h1st.pnl>0&&h2nd.pnl>0)?"both-halves+":"halves-FAIL");
    if(SL_A) std::printf("  SLICE n=%-4d net=$%+9.1f PF=%4.2f worst=$%+7.1f maxDD=$%-7.0f\n",
                slice.n, slice.pnl, slice.pf(), slice.worst, slice.mdd);
    for(auto& kv:cells)
        std::printf("    %-34s n=%-4d net=$%+9.1f PF=%4.2f worst=$%+7.1f\n",
                    kv.first.c_str(), kv.second.n, kv.second.pnl, kv.second.pf(), kv.second.worst);
    return 0;
}
