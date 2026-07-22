// =============================================================================
// XauTrendFollow4h2hBacktest.cpp -- REAL-CLASS faithful BT of the 4h + 2h
// production engines (closes the audit sliver: 2h/4h previously had only the
// XauTrendFollowBacktest.cpp PORT which re-implements the cells).
//
// Like XauTrendFollow1hBacktest.cpp, this #includes the REAL engine headers
// and drives them bar-by-bar, so every result reflects the actual cell logic.
//
// FIDELITY (mirrors the 1h harness):
//   - bar CSV: ts,o,h,l,c (ts seconds).
//   - cross-spread fills: bid=level, ask=level+SPREAD; long enters@ask exits@bid.
//   - SL-first intrabar: each bar fed low -> high -> close before the close call,
//     so an adverse stop is hit before a favourable extreme (conservative).
//   - entries fire at bar close; managed from the NEXT bar onward (no look-ahead).
//   - engine computes its own Wilder ATR14 (atr14_external=0 for 4h).
//   - PnL: tr.pnl = points*lot; XAU 0.01 lot = $1/pt -> USD = tr.pnl*100.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/XauTrendFollow4h2hBacktest.cpp \
//             -o backtest/xau_tf_4h2h_bt
// RUN:   ./backtest/xau_tf_4h2h_bt <H4_csv> <H1_csv>
//        (4h driven on the H4 file; 2h driven on the H1 file -- it builds 2h
//         buckets from H1 internally.)
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>
#include <fstream>
#include <functional>

#include "XauTrendFollow4hEngine.hpp"
#include "XauTrendFollow2hEngine.hpp"

static double SPREAD = 0.20;          // env SPREAD override (IBKR XAU measured ~0.22-0.50/oz)
static double IMP     = 0.0;          // env IMP: min_impulse_atr on the 4h engine (0=off)
static double ADXF    = 0.0;          // env ADX: min_adx_entry chop-gate on the 4h engine (0=off)
static long   MASK4   = -1;           // env MASK: cell_enable_mask on the 4h engine (-1=engine default)
static int    VB      = 0;            // env VB=1: mirror production vol-band gate (mask 0x8, 0.30-0.85)
// IBKR XAU commission: 1.5bps/side, price-proportional. At 0.01 lot (1 oz),
// RT comm in USD = 2*0.00015*entryPrice. Applied per closed trade in cb.
// MGC=1 (env): MGC micro-future cost model instead — comm ~$1.04/side per 10oz
// contract -> $0.208/oz RT, fixed not price-proportional (pair with SPREAD=0.10,
// one exchange tick). Total ~0.31pt RT vs spot ~1.4pt at 4000.
static int MGC = 0;
static double LC4  = 0.0;         // env LC: LOSS_CUT_PCT on the 4h engine (prod 1.5)
static double COSTX = 1.0;        // env COSTX: multiply commission (2x-cost stress)
static inline double ibkr_comm_usd(double entry_px){
    return COSTX * (MGC ? 0.208 : 2.0 * 0.00015 * entry_px);
}

struct BarCSV { int64_t ts; double o,h,l,c; };

static std::vector<BarCSV> load_csv(const char* path){
    std::vector<BarCSV> v; std::ifstream f(path);
    if(!f.is_open()){ std::fprintf(stderr,"cannot open %s\n",path); return v; }
    std::string line; bool first=true;
    while(std::getline(f,line)){
        if(first){ first=false; if(line.find_first_of("0123456789")!=0 || line.find(',')==std::string::npos){ if(!isdigit((unsigned char)line[0])) continue; } }
        BarCSV b; double o,h,l,c; long long ts;
        if(std::sscanf(line.c_str(),"%lld,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)==5){
            b.ts=ts; b.o=o; b.h=h; b.l=l; b.c=c; v.push_back(b);
        }
    }
    return v;
}

struct Stat {
    int n=0; double pnl=0, gw=0, gl=0, peak=0, mdd=0, cur=0;
    void rec(double usd){ n++; pnl+=usd; if(usd>=0)gw+=usd; else gl+=-usd;
        cur+=usd; if(cur>peak)peak=cur; if(peak-cur>mdd)mdd=peak-cur; }
    double pf()const{ return gl>0? gw/gl : (gw>0?99.9:0); }
    double wrx(double wins)const{ return n? 100.0*wins/n : 0; }
};

// Run the 4h engine on H4 bars. Returns full-span + split halves.
template<class Eng, bool IS_4H>
static void run(const std::vector<BarCSV>& bars, const char* label){
    Eng eng; eng.shadow_mode=true; eng.enabled=true; eng.lot=0.01; eng.max_spread=1.0;
    if constexpr (IS_4H) { eng.min_impulse_atr = IMP; eng.min_adx_entry = ADXF; if(MASK4>=0) eng.cell_enable_mask=(uint32_t)MASK4;
        eng.LOSS_CUT_PCT = LC4;   // prod g_xau_tf_4h.LOSS_CUT_PCT = 1.5
        if(VB){ eng.use_vol_band_gate=true; eng.vol_band_low_pct=0.30; eng.vol_band_high_pct=0.85; eng.cell_vol_band_mask=0x8; } }   // 4h-only fields
    eng.init();
    Stat full, h1half, h2half; int wins=0; const int N=(int)bars.size(); const int mid=N/2;
    int idx=0;
    auto cb=[&](const omega::TradeRecord& tr){
        double usd = tr.pnl * 100.0 - ibkr_comm_usd(tr.entryPrice);  // IBKR price-proportional comm
        full.rec(usd); if(usd>=0) wins++;
        if(idx < mid) h1half.rec(usd); else h2half.rec(usd);
    };
    for(int i=0;i<N;++i){
        idx=i; const auto& b=bars[i]; const int64_t ts_ms=(int64_t)b.ts*1000;
        if(i>0){
            eng.on_tick(b.l, b.l+SPREAD, ts_ms, cb);
            eng.on_tick(b.h, b.h+SPREAD, ts_ms, cb);
            eng.on_tick(b.c, b.c+SPREAD, ts_ms, cb);
        }
        if constexpr (IS_4H){
            omega::XauTfBar bar{}; bar.bar_start_ms=ts_ms; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
            eng.on_h4_bar(bar, b.c, b.c+SPREAD, 0.0, ts_ms, cb);
        } else {
            omega::XauTf2hBar bar{}; bar.bar_start_ms=ts_ms; bar.open=b.o; bar.high=b.h; bar.low=b.l; bar.close=b.c;
            eng.on_h1_bar(bar, b.c, b.c+SPREAD, ts_ms, cb);
        }
    }
    const auto& last=bars.back();
    eng.force_close(last.c, last.c+SPREAD, (int64_t)last.ts*1000, cb, "EOD_FLAT");
    std::printf("  %-22s n=%-4d WR=%4.1f%%  net=$%+9.1f  PF=%5.2f  maxDD=$%-6.0f | H1 PF=%.2f($%+.0f) H2 PF=%.2f($%+.0f) %s\n",
        label, full.n, full.wrx(wins), full.pnl, full.pf(), full.mdd,
        h1half.pf(), h1half.pnl, h2half.pf(), h2half.pnl,
        (full.pnl>0 && h1half.pnl>0 && h2half.pnl>0)?"both-halves+ ✓":"both-halves FAIL ✗");
}

int main(int argc, char** argv){
    if(argc<3){ std::fprintf(stderr,"usage: %s <H4_csv> <H1_csv>\n",argv[0]); return 1; }
    if(getenv("SPREAD")) SPREAD=atof(getenv("SPREAD"));
    if(getenv("IMP"))    IMP=atof(getenv("IMP"));
    if(getenv("ADX"))    ADXF=atof(getenv("ADX"));
    if(getenv("MASK"))   MASK4=strtol(getenv("MASK"),nullptr,0);
    if(getenv("VB"))     VB=atoi(getenv("VB"));
    if(getenv("MGC"))    MGC=atoi(getenv("MGC"));
    if(getenv("LC"))     LC4=atof(getenv("LC"));
    if(getenv("COSTX"))  COSTX=atof(getenv("COSTX"));
    auto h4=load_csv(argv[1]); auto h1=load_csv(argv[2]);
    std::printf("[XTF4h2h-BT] real-class  H4 bars=%zu  H1 bars=%zu  spread=%.2f  IMP(4h)=%.2f  ADX(4h)=%.1f  LC(4h)=%.2f  COSTX=%.1f  +IBKR_comm(1.5bps/side)\n", h4.size(), h1.size(), SPREAD, IMP, ADXF, LC4, COSTX);
    std::printf("===== 4h engine (on_h4_bar, 6-cell mask 0x3F) =====\n");
    if((int)h4.size()>60) run<omega::XauTrendFollow4hEngine,true>(h4, "XauTrendFollow4h");
    else std::printf("  (insufficient H4 bars)\n");
    std::printf("===== 2h engine (on_h1_bar -> internal 2h) =====\n");
    if((int)h1.size()>120) run<omega::XauTrendFollow2hEngine,false>(h1, "XauTrendFollow2h");
    else std::printf("  (insufficient H1 bars)\n");
    return 0;
}
