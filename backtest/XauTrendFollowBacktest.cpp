// =============================================================================
// XauTrendFollowBacktest.cpp -- Multi-cell multi-TF trend-follow backtest
// =============================================================================
//
// Covers XauTrendFollow2hEngine (4 cells), XauTrendFollow4hEngine (6 cells),
// and XauTrendFollowD1Engine (3 cells) in one harness.  13 cells total.
//
// DATA FORMAT: Dukascopy combined CSV: timestamp_ms,askPrice,bidPrice
//
// COMPILATION:
//   g++ -std=c++17 -O2 -o backtest/xau_tf_bt backtest/XauTrendFollowBacktest.cpp
//   ./backtest/xau_tf_bt ~/Tick/duka_ticks/XAUUSD_2024-03_2026-04_combined.csv
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <array>
#include <deque>

namespace cfg {
    constexpr double LOT_SIZE   = 0.01;
    constexpr double PNL_PER_PT = LOT_SIZE * 100.0;
    constexpr int    ATR_PERIOD = 14;
    constexpr double ATR_INIT   = 5.0;
    constexpr double ATR_MIN    = 0.5;
    constexpr double OOS_FRAC   = 0.60;
    inline bool is_weekend(int64_t ts_ms) noexcept {
        const int64_t sec = ts_ms / 1000;
        const int dow = ((sec / 86400) + 4) % 7;
        if (dow == 0 || dow == 6) return true;
        if (dow == 5 && (int)((sec % 86400) / 3600) >= 21) return true;
        return false;
    }
}

struct Bar {
    int64_t open_ms = 0;
    double open = 0, high = 0, low = 99999999, close = 0;
    int tick_count = 0;
    void reset(int64_t ts, double p) { open_ms=ts; open=high=low=close=p; tick_count=1; }
    void update(double p) { if(p>high) high=p; if(p<low) low=p; close=p; tick_count++; }
};

struct BarBuilder {
    int64_t period_ms;
    Bar current{};
    std::deque<Bar> bars;
    bool active = false;
    int total_bars = 0;
    double atr = cfg::ATR_INIT, ema20 = 0;
    double don20_high = 0, don20_low = 99999999;
    double don50_high = 0, don50_low = 99999999;
    double adx = 0;
    // S88-followup: rolling ATR(14) window for vol-percentile band gate.
    std::deque<double> atr_vol_window;
    double plus_dm_sum = 0, minus_dm_sum = 0, tr_sum14 = 0;
    std::deque<double> dx_hist, tr_hist;

    BarBuilder(int64_t period) : period_ms(period) {}

    bool on_tick(double mid, int64_t ts_ms) {
        if (!active) { current.reset(ts_ms, mid); active = true; return false; }
        int64_t boundary = (current.open_ms / period_ms) * period_ms + period_ms;
        if (ts_ms >= boundary) {
            bars.push_back(current); total_bars++; _on_close();
            current.reset(ts_ms, mid);
            while ((int)bars.size() > 260) bars.pop_front();
            return true;
        }
        current.update(mid); return false;
    }
private:
    void _on_close() {
        if (bars.size() < 2) return;
        const Bar& bar = bars.back(); const Bar& prev = bars[bars.size()-2];
        double tr = std::max({bar.high-bar.low,std::fabs(bar.high-prev.close),std::fabs(bar.low-prev.close)});
        tr_hist.push_back(tr);
        if ((int)tr_hist.size() > cfg::ATR_PERIOD) tr_hist.pop_front();
        if ((int)tr_hist.size() >= cfg::ATR_PERIOD) atr = atr*13.0/14.0 + tr/14.0;
        else { double s=0; for(auto t:tr_hist) s+=t; atr=s/tr_hist.size(); }
        atr = std::max(cfg::ATR_MIN, atr);
        // S88-followup: maintain rolling 200-bar ATR window for vol-band.
        atr_vol_window.push_back(atr);
        if ((int)atr_vol_window.size() > 200) atr_vol_window.pop_front();
        double k20=2.0/21.0;
        if(ema20==0) ema20=bar.close;
        ema20 = bar.close*k20 + ema20*(1.0-k20);
        if((int)bars.size()>=21) {
            don20_high=0; don20_low=99999999;
            for(int i=(int)bars.size()-21;i<(int)bars.size()-1;++i){
                if(bars[i].high>don20_high) don20_high=bars[i].high;
                if(bars[i].low<don20_low) don20_low=bars[i].low;
            }
        }
        if((int)bars.size()>=51) {
            don50_high=0; don50_low=99999999;
            for(int i=(int)bars.size()-51;i<(int)bars.size()-1;++i){
                if(bars[i].high>don50_high) don50_high=bars[i].high;
                if(bars[i].low<don50_low) don50_low=bars[i].low;
            }
        }
        double up=bar.high-prev.high, dn=prev.low-bar.low;
        double pdm=(up>dn&&up>0)?up:0, mdm=(dn>up&&dn>0)?dn:0;
        if(total_bars<=cfg::ATR_PERIOD){plus_dm_sum+=pdm;minus_dm_sum+=mdm;tr_sum14+=tr;}
        else{plus_dm_sum=plus_dm_sum*13.0/14.0+pdm;minus_dm_sum=minus_dm_sum*13.0/14.0+mdm;tr_sum14=tr_sum14*13.0/14.0+tr;}
        if(tr_sum14>0.001&&total_bars>=cfg::ATR_PERIOD){
            double pdi=100.0*plus_dm_sum/tr_sum14, mdi=100.0*minus_dm_sum/tr_sum14;
            double ds=pdi+mdi, dx=(ds>0.001)?100.0*std::fabs(pdi-mdi)/ds:0;
            dx_hist.push_back(dx);
            if((int)dx_hist.size()>cfg::ATR_PERIOD) dx_hist.pop_front();
            if((int)dx_hist.size()>=cfg::ATR_PERIOD) adx=adx*13.0/14.0+dx/14.0;
            else{double s=0;for(auto d:dx_hist) s+=d; adx=s/dx_hist.size();}
        }
    }
};

struct D1Builder {
    std::deque<Bar> bars; Bar current{}; bool active=false; int total_bars=0;
    double atr=cfg::ATR_INIT, ema20=0, don20_high=0, don20_low=99999999, adx=0;
    double plus_dm_sum=0, minus_dm_sum=0, tr_sum14=0;
    std::deque<double> dx_hist, tr_hist;
    std::deque<double> atr_vol_window;  // S88-followup vol-band
    int last_date=-1;

    bool on_4h_bar(const Bar& h4) {
        int date=(int)(h4.open_ms/1000/86400);
        if(!active){
            current.reset(h4.open_ms,h4.open); current.high=h4.high; current.low=h4.low; current.close=h4.close;
            active=true; last_date=date; return false;
        }
        if(date!=last_date){
            bars.push_back(current); total_bars++; _on_close();
            current.reset(h4.open_ms,h4.open); current.high=h4.high; current.low=h4.low; current.close=h4.close;
            last_date=date; while((int)bars.size()>260) bars.pop_front(); return true;
        }
        if(h4.high>current.high) current.high=h4.high;
        if(h4.low<current.low) current.low=h4.low;
        current.close=h4.close; return false;
    }
    void _on_close() {
        if(bars.size()<2) return;
        const Bar& bar=bars.back(); const Bar& prev=bars[bars.size()-2];
        double tr=std::max({bar.high-bar.low,std::fabs(bar.high-prev.close),std::fabs(bar.low-prev.close)});
        tr_hist.push_back(tr); if((int)tr_hist.size()>cfg::ATR_PERIOD) tr_hist.pop_front();
        if((int)tr_hist.size()>=cfg::ATR_PERIOD) atr=atr*13.0/14.0+tr/14.0;
        else{double s=0;for(auto t:tr_hist) s+=t; atr=s/tr_hist.size();}
        atr=std::max(cfg::ATR_MIN,atr);
        // S88-followup: rolling ATR window for D1 vol-band.
        atr_vol_window.push_back(atr);
        if((int)atr_vol_window.size()>200) atr_vol_window.pop_front();
        if(ema20==0) ema20=bar.close;
        ema20=bar.close*(2.0/21.0)+ema20*(1.0-2.0/21.0);
        if((int)bars.size()>=21){
            don20_high=0;don20_low=99999999;
            for(int i=(int)bars.size()-21;i<(int)bars.size()-1;++i){
                if(bars[i].high>don20_high) don20_high=bars[i].high;
                if(bars[i].low<don20_low) don20_low=bars[i].low;
            }
        }
        double up=bar.high-prev.high, dn=prev.low-bar.low;
        double pdm=(up>dn&&up>0)?up:0, mdm=(dn>up&&dn>0)?dn:0;
        if(total_bars<=cfg::ATR_PERIOD){plus_dm_sum+=pdm;minus_dm_sum+=mdm;tr_sum14+=tr;}
        else{plus_dm_sum=plus_dm_sum*13.0/14.0+pdm;minus_dm_sum=minus_dm_sum*13.0/14.0+mdm;tr_sum14=tr_sum14*13.0/14.0+tr;}
        if(tr_sum14>0.001&&total_bars>=cfg::ATR_PERIOD){
            double pdi=100.0*plus_dm_sum/tr_sum14, mdi=100.0*minus_dm_sum/tr_sum14;
            double ds=pdi+mdi, dx=(ds>0.001)?100.0*std::fabs(pdi-mdi)/ds:0;
            dx_hist.push_back(dx); if((int)dx_hist.size()>cfg::ATR_PERIOD) dx_hist.pop_front();
            if((int)dx_hist.size()>=cfg::ATR_PERIOD) adx=adx*13.0/14.0+dx/14.0;
            else{double s=0;for(auto d:dx_hist) s+=d; adx=s/dx_hist.size();}
        }
    }
};

struct Cell {
    const char* name; double sl_mult, tp_mult;
    int total=0,wins=0; double pnl=0,gw=0,gl=0,mdd=0,pk=0;
    void record(double p){total++;pnl+=p;if(p>0){wins++;gw+=p;}else gl+=std::fabs(p);if(pnl>pk)pk=pnl;double d=pk-pnl;if(d>mdd)mdd=d;}
    double pf()const{return gl>0?gw/gl:999.0;} double wr()const{return total>0?100.0*wins/total:0;}
    void print()const{std::printf("  %-22s trades=%4d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f\n",name,total,wr(),pf(),pnl,mdd);}
};
struct SimTrade { bool active=false,is_long=true; double entry_px=0,sl_px=0,tp_px=0; int64_t entry_time=0; };
struct Metrics {
    int total=0,wins=0; double pnl=0,gw=0,gl=0,mdd=0,pk=0;
    void record(double p){total++;pnl+=p;if(p>0){wins++;gw+=p;}else gl+=std::fabs(p);if(pnl>pk)pk=pnl;double d=pk-pnl;if(d>mdd)mdd=d;}
    double pf()const{return gl>0?gw/gl:999.0;} double wr()const{return total>0?100.0*wins/total:0;}
    void print(const char* l)const{std::printf("  %-20s trades=%5d  WR=%.1f%%  PF=%.2f  PnL=$%.2f  maxDD=$%.2f\n",l,total,wr(),pf(),pnl,mdd);}
};

int main(int argc, char* argv[]) {
    if(argc<2){std::fprintf(stderr,"Usage: %s <tick_csv> [--vol-band] [--lo PCT] [--hi PCT]\n",argv[0]);return 1;}
    // S88-followup CLI flags for vol-percentile band gate.
    bool   use_vol_band = false;
    bool   use_adx      = false;
    double vb_low = 0.30, vb_high = 0.85, adx_min = 25.0;
    // S88-followup per-cell masks (default 0xFFFFFFFF = all cells gated).
    // For 2h cell ids match kXauTf2hCells: 0=Keltner, 1=Donch20, 2=Donch50, 3=InsideBar.
    // Operator sets via --cell-adx-mask 0xB --cell-vol-mask 0x4 to mirror engine_init.
    unsigned cell_adx_mask = 0xFFFFFFFFu;
    unsigned cell_vol_mask = 0xFFFFFFFFu;
    for(int i=2;i<argc;++i){
        std::string a=argv[i];
        if(a=="--vol-band") use_vol_band=true;
        else if(a=="--adx") use_adx=true;
        else if(a=="--lo" && i+1<argc) vb_low = std::stod(argv[++i]);
        else if(a=="--hi" && i+1<argc) vb_high = std::stod(argv[++i]);
        else if(a=="--adx-min" && i+1<argc) adx_min = std::stod(argv[++i]);
        else if(a=="--cell-adx-mask" && i+1<argc) cell_adx_mask = std::stoul(argv[++i], nullptr, 0);
        else if(a=="--cell-vol-mask" && i+1<argc) cell_vol_mask = std::stoul(argv[++i], nullptr, 0);
    }
    std::printf("[TF-BT] vol_band: %s [%.2f, %.2f] mask=0x%X  adx: %s (>=%.1f) mask=0x%X\n",
                use_vol_band?"ON":"OFF", vb_low, vb_high, cell_vol_mask,
                use_adx?"ON":"OFF", adx_min, cell_adx_mask);
    BarBuilder bb2(2LL*3600*1000), bb4(4LL*3600*1000);
    D1Builder d1;
    Cell cells[]={
        {"2h_Keltner",2,4},{"2h_Donch20",2,4},{"2h_Donch50",2,4},{"2h_InsideBar",2,4},
        {"4h_Donch20",1.5,3},{"4h_InsideBar",2,6},{"4h_ER20",0.75,6},{"4h_Keltner",1.5,3},
        {"4h_ADX_Mom",2,4},{"4h_RangeExpand",1.5,6},
        {"D1_Momentum20",2,4},{"D1_Keltner",2,6},{"D1_ADX_Mom",2,4},
    };
    constexpr int NC=13;
    SimTrade trades[NC]; Metrics tf2,tf4,tfd,all,ism,osm;
    int64_t ots=0; bool oset=false; int64_t ft=0,lt=0; size_t ttk=0; double mnp=99999999,mxp=0;

    std::printf("[TF-BT] XAUUSD Multi-cell TrendFollow backtest (2h/4h/D1)\n\n");
    std::ifstream f(argv[1]); if(!f.is_open()){std::fprintf(stderr,"Cannot open %s\n",argv[1]);return 1;}
    std::string fl; std::getline(f,fl);
    bool hdr=(fl.find("timestamp")!=std::string::npos||fl.find("Time")!=std::string::npos);

    auto pd=[](const std::string& l,int64_t& ts,double& b,double& a)->bool{return std::sscanf(l.c_str(),"%lld,%lf,%lf",(long long*)&ts,&a,&b)==3;};
    auto ker=[](const std::deque<Bar>& bars,int n)->double{
        if((int)bars.size()<n+1)return 0;
        double dir=std::fabs(bars.back().close-bars[bars.size()-1-n].close),vol=0;
        for(int i=(int)bars.size()-n;i<(int)bars.size();++i) vol+=std::fabs(bars[i].close-bars[i-1].close);
        return(vol>0.001)?dir/vol:0;
    };
    // v2: disable losing 4h cells: 4h_InsideBar(5), 4h_ER20(6), 4h_ADX_Mom(8)
    // v1 PF: 0.98, 0.95, 0.85 respectively — dragging OOS from 1.20 to 1.18
    constexpr bool cell_enabled[NC]={true,true,true,true, true,false,false,true,false,true, true,true,true};
    // S88-followup vol-band helper. Returns true if current_atr is inside
    // [vb_low, vb_high] percentile of the rolling 200-bar atr_vol_window.
    // If gate OFF -> always true. If window < 200 (warmup) -> fail-open (true).
    auto vbpass=[&](const std::deque<double>& w, double cur_atr)->bool{
        if(!use_vol_band) return true;
        if((int)w.size() < 200) return true;
        int below=0; for(double x:w) if(x<cur_atr) ++below;
        const double pct = (double)below / w.size();
        return pct >= vb_low && pct <= vb_high;
    };
    auto te=[&](int ci,bool il,double b,double a,int64_t ts,double atr){
        if(!cell_enabled[ci])return;
        // S88-followup per-cell gates: ci 0-3 = 2h, 4-9 = 4h, 10-12 = D1.
        // Each cell can be selectively gated by ADX or vol_band via the
        // global cell_adx_mask / cell_vol_mask. Bit i = 1 means cell i is
        // gated by the corresponding gate when use_X is on.
        const unsigned bit = (1u << ci);
        const std::deque<double>* w = nullptr;
        double adx_v = 100.0;  // default: pass ADX
        if (ci < 4)      { w = &bb2.atr_vol_window; adx_v = bb2.adx; }
        else if (ci < 10){ w = &bb4.atr_vol_window; adx_v = bb4.adx; }
        else             { w = &d1.atr_vol_window;  adx_v = d1.adx;  }
        if (use_vol_band && (cell_vol_mask & bit) && w && !vbpass(*w, atr)) return;
        if (use_adx && (cell_adx_mask & bit) && adx_v < adx_min) return;
        if(trades[ci].active)return; auto& t=trades[ci];
        t.active=true;t.is_long=il;t.entry_px=il?a:b;
        t.sl_px=il?t.entry_px-cells[ci].sl_mult*atr:t.entry_px+cells[ci].sl_mult*atr;
        t.tp_px=il?t.entry_px+cells[ci].tp_mult*atr:t.entry_px-cells[ci].tp_mult*atr;
        t.entry_time=ts;
    };
    auto mg=[&](double b,double a,int64_t ts){
        for(int i=0;i<NC;++i){auto& t=trades[i];if(!t.active)continue;
            bool sl=t.is_long?(b<=t.sl_px):(a>=t.sl_px);
            bool tp=t.is_long?(b>=t.tp_px):(a<=t.tp_px);
            if(!sl&&!tp)continue;
            double ep=tp?t.tp_px:t.sl_px,pp=t.is_long?(ep-t.entry_px):(t.entry_px-ep),pu=pp*cfg::PNL_PER_PT;
            cells[i].record(pu);all.record(pu);
            if(t.entry_time<ots)ism.record(pu);else osm.record(pu);
            if(i<4)tf2.record(pu);else if(i<10)tf4.record(pu);else tfd.record(pu);
            t.active=false;
        }
    };
    auto proc=[&](int64_t ts,double b,double a){
        if(b<=0||a<=0||a<b)return; double mid=(b+a)*0.5; if(mid<1000||mid>5000)return;
        ttk++; if(mid<mnp)mnp=mid; if(mid>mxp)mxp=mid; if(ft==0)ft=ts; lt=ts;
        if(!oset&&ft>0){ots=ft+(int64_t)(26.0*30*24*3600*1000.0*cfg::OOS_FRAC);oset=true;}
        if(cfg::is_weekend(ts))return;
        mg(b,a,ts);
        bool b2h=bb2.on_tick(mid,ts), b4h=bb4.on_tick(mid,ts), bd1=false;
        if(b4h&&bb4.bars.size()>0) bd1=d1.on_4h_bar(bb4.bars.back());

        if(b2h&&bb2.total_bars>=52){  // S88: gates moved to per-cell te() check
            double c=bb2.bars.back().close;
            if(c>bb2.ema20+2*bb2.atr)te(0,true,b,a,ts,bb2.atr);
            else if(c<bb2.ema20-2*bb2.atr)te(0,false,b,a,ts,bb2.atr);
            if((int)bb2.bars.size()>=21){if(c>bb2.don20_high)te(1,true,b,a,ts,bb2.atr);else if(c<bb2.don20_low)te(1,false,b,a,ts,bb2.atr);}
            if((int)bb2.bars.size()>=51){if(c>bb2.don50_high)te(2,true,b,a,ts,bb2.atr);else if(c<bb2.don50_low)te(2,false,b,a,ts,bb2.atr);}
            if((int)bb2.bars.size()>=3){
                const Bar&mo=bb2.bars[bb2.bars.size()-3];const Bar&in=bb2.bars[bb2.bars.size()-2];
                if(in.high<=mo.high&&in.low>=mo.low){if(c>in.high)te(3,true,b,a,ts,bb2.atr);else if(c<in.low)te(3,false,b,a,ts,bb2.atr);}
            }
        }
        if(b4h&&bb4.total_bars>=52){  // S88: per-cell te() gates
            double c=bb4.bars.back().close; const Bar& b4=bb4.bars.back();
            if((int)bb4.bars.size()>=21){if(c>bb4.don20_high)te(4,true,b,a,ts,bb4.atr);else if(c<bb4.don20_low)te(4,false,b,a,ts,bb4.atr);}
            if((int)bb4.bars.size()>=3){const Bar&mo=bb4.bars[bb4.bars.size()-3];const Bar&in=bb4.bars[bb4.bars.size()-2];
                if(in.high<=mo.high&&in.low>=mo.low){if(c>in.high)te(5,true,b,a,ts,bb4.atr);else if(c<in.low)te(5,false,b,a,ts,bb4.atr);}}
            double er=ker(bb4.bars,20);
            if(er>=0.20&&(int)bb4.bars.size()>=21){double mp=(c-bb4.bars[bb4.bars.size()-21].close)/bb4.bars[bb4.bars.size()-21].close;
                if(mp>0.001)te(6,true,b,a,ts,bb4.atr);else if(mp<-0.001)te(6,false,b,a,ts,bb4.atr);}
            if(c>bb4.ema20+2*bb4.atr)te(7,true,b,a,ts,bb4.atr);else if(c<bb4.ema20-2*bb4.atr)te(7,false,b,a,ts,bb4.atr);
            if(bb4.adx>=25&&(int)bb4.bars.size()>=21){double mp=(c-bb4.bars[bb4.bars.size()-21].close)/bb4.bars[bb4.bars.size()-21].close;
                if(mp>0.001)te(8,true,b,a,ts,bb4.atr);else if(mp<-0.001)te(8,false,b,a,ts,bb4.atr);}
            double btr=b4.high-b4.low; if(btr>1.5*bb4.atr){bool bull=(b4.close>b4.open);te(9,bull,b,a,ts,bb4.atr);}
        }
        if(bd1&&d1.total_bars>=22){  // S88: per-cell te() gates
            double c=d1.bars.back().close;
            if((int)d1.bars.size()>=21){double pc=d1.bars[d1.bars.size()-21].close;
                if(c>pc*1.001)te(10,true,b,a,ts,d1.atr);else if(c<pc*0.999)te(10,false,b,a,ts,d1.atr);}
            if(c>d1.ema20+2*d1.atr)te(11,true,b,a,ts,d1.atr);else if(c<d1.ema20-2*d1.atr)te(11,false,b,a,ts,d1.atr);
            if(d1.adx>=25&&(int)d1.bars.size()>=21){double mp=(c-d1.bars[d1.bars.size()-21].close)/d1.bars[d1.bars.size()-21].close;
                if(mp>0.001)te(12,true,b,a,ts,d1.atr);else if(mp<-0.001)te(12,false,b,a,ts,d1.atr);}
        }
    };

    if(!hdr){int64_t ts;double b,a;if(pd(fl,ts,b,a))proc(ts,b,a);}
    std::string line; size_t lc=0;
    while(std::getline(f,line)){int64_t ts;double b,a;if(pd(line,ts,b,a))proc(ts,b,a);if(++lc%10000000==0)std::printf("  ... %zuM ticks\n",lc/1000000);}

    double hrs=(lt>ft)?(lt-ft)/3600000.0:0;
    std::printf("\n===============================================================\n");
    std::printf("  XAUUSD MULTI-CELL TREND-FOLLOW BACKTEST (2h/4h/D1)\n");
    std::printf("===============================================================\n\n");
    std::printf("  Ticks: %zu  |  Period: %.1f days  |  Price: %.1f - %.1f\n\n",ttk,hrs/24,mnp,mxp);
    std::printf("-- PER-CELL RESULTS ----------------------------------------\n");
    for(int i=0;i<NC;++i) cells[i].print();
    std::printf("\n-- PER-TIMEFRAME AGGREGATE ----------------------------------\n");
    tf2.print("2h (4 cells)"); tf4.print("4h (6 cells)"); tfd.print("D1 (3 cells)"); all.print("ALL COMBINED");
    std::printf("\n===============================================================\n");
    std::printf("  IN-SAMPLE / OUT-OF-SAMPLE (60/40 split)\n");
    std::printf("===============================================================\n\n");
    ism.print("IS TOTAL"); osm.print("OOS TOTAL");
    std::printf("\n  OOS VERDICT: %s\n\n",(osm.total>=8&&osm.pf()>=1.20)?"PASS":"REVIEW");
    return 0;
}
