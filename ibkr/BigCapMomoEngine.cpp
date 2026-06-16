// BigCapMomoEngine.cpp -- long big-cap momentum continuation, C++ on the IBKR TWS API.
// Long mirror of GapShortEngine. NOT BlackBull/FIX -- additive, isolated.
//
// VALIDATED (2026-06-16, pump/bigcap_scalp_sweep + _stress + _thorough; 508 NAS/SPX,
//   Yahoo 5m ~2-3mo): day-expansion gate + WIDE trail. Base config gate3%/trail4.0%:
//   PF 1.63, WR 51%, walk-forward H1 1.65 / H2 1.61 (stable), fat-tail-resilient
//   (PF 1.30 removing top-5 winners), slippage-robust to ~20-30bps. gate5%/trail4.0%
//   = PF 1.83 but tail-dependent/thinner. CAVEAT: one regime (60d) -> SHADOW first.
//   Edge is the trader's distilled: trade confirmed movers, ride a WIDE trail (the
//   give-back is the enemy; tight trails get chopped -- the "1min slippage trap").
//
// MODEL: scan TOP_PERC_GAIN -> per candidate stream 5s real-time bars, aggregate to
//   5m -> on each closed 5m bar, if flat: gate(day_up>=GATE%) + ignition(+IG% over
//   LB*5m) + vol surge(>=VOLX x 20-bar avg) -> LONG. If long: wide trail off peak +
//   max-hold backstop.
// SAFETY: PAPER_ONLY=true -> logs only, NO live orders. libbid stub -> real Intel RDFP
//   before LIVE order sizing (see ibkr/README).
//
// build (VPS MSVC, same as GapShortEngine target): see CMakeLists BigCapMomoEngine.
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "ScannerSubscription.h"
#include "Contract.h"
#include "Order.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <deque>
#include <memory>
#include "RiskManager.hpp"

struct Cfg {
    double GATE = 3.0;     // day-expansion gate: only trade names already +GATE% on session
    double TRAIL = 0.04;   // 4.0% wide trail off peak (the validated lever)
    double IG = 3.0;       // ignition: +IG% over LB 5m bars (continuation trigger)
    double VOLX = 3.0;     // volume surge vs 20-bar avg
    double PX_MIN = 10.0;  // big-cap / deep-liquidity floor
    int    LB = 6;         // ignition lookback (6*5m = 30min)
    int    MAXHOLD = 48;   // 48*5m = 4h backstop
    double KELLY = 0.12, BANKROLL = 50000;
    bool   PAPER_ONLY = true;
};

// per-symbol live state
struct Sym {
    Contract c;
    double day_open = 0.0;
    long   cur_bucket = -1;             // floor(epoch/300) of the building 5m bar
    double bo=0, bh=0, bl=0, bc=0; double bv=0;  // building 5m bar OHLCV
    std::deque<double> closes;          // finalized 5m closes (for ignition lookback)
    std::deque<double> vols;            // finalized 5m volumes (for surge avg)
    bool   inpos=false; double entry=0, peak=0; int hold=0; double notional=0;
};

class BigCapMomoEngine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Cfg cfg_; OrderId nextId_=0;
    std::map<int,Sym> subs_;            // reqId -> Sym (real-time-bar stream id)
    int next_rt_=20000; int scanned_=0; bool scanDone_=false;
    RiskManager risk_{50000.0};
public:
    BigCapMomoEngine(){ risk_.new_day(); cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void set(double gate,double trail,bool live){ cfg_.GATE=gate; cfg_.TRAIL=trail; cfg_.PAPER_ONLY=!live; }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }

    void nextValidId(OrderId id) override { nextId_=id;
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_GAIN";
        s.abovePrice=cfg_.PX_MIN; s.aboveVolume=100000;
        cli_->reqScannerSubscription(9100,s,TagValueListSPtr(),TagValueListSPtr());
        printf("[BigCapMomo] scan TOP_PERC_GAIN (gate>=%.0f%% trail=%.0f%% IG=%.0f%% volx=%.0f) %s\n",
               cfg_.GATE,cfg_.TRAIL*100,cfg_.IG,cfg_.VOLX,cfg_.PAPER_ONLY?"PAPER":"LIVE"); fflush(stdout);
    }
    void scannerData(int,int rank,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        if(rank>=20) return;                         // top-20 movers only
        int rid=next_rt_++; Sym x; x.c=cd.contract; subs_[rid]=x;
        // 5-second real-time bars; we aggregate to 5m. useRTH=true, TRADES.
        cli_->reqRealTimeBars(rid, x.c, 5, "TRADES", true, TagValueListSPtr());
    }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }

    // 5-second bar -> aggregate into the current 5m bucket; finalize on rollover.
    void realtimeBar(TickerId rid,long t,double o,double h,double l,double c,Decimal vol,Decimal,int) override {
        auto it=subs_.find((int)rid); if(it==subs_.end()) return; Sym& s=it->second;
        long bucket = t/300;                          // 5-minute bucket
        double v = DecimalFunctions::decimalToDouble(vol);
        if(s.cur_bucket<0){ s.cur_bucket=bucket; s.bo=o; s.bh=h; s.bl=l; s.bc=c; s.bv=v; return; }
        if(bucket==s.cur_bucket){ if(h>s.bh)s.bh=h; if(l<s.bl)s.bl=l; s.bc=c; s.bv+=v; return; }
        // bucket rolled -> finalize the completed 5m bar, then start the new one
        on_5m_bar(s, s.bo, s.bh, s.bl, s.bc, s.bv, t);
        s.cur_bucket=bucket; s.bo=o; s.bh=h; s.bl=l; s.bc=c; s.bv=v;
    }

    void on_5m_bar(Sym& s, double o, double h, double l, double c, double v, long t) {
        // session open = first 5m bar's open of the day (UTC date change resets via day_open=0)
        if(s.day_open<=0) s.day_open=o;
        // ---- manage open position first ----
        if(s.inpos){
            s.hold++;
            if(h>s.peak) s.peak=h;
            double tstop=s.peak*(1-cfg_.TRAIL);
            if(l<=tstop){ close_pos(s,tstop,"TRAIL"); return; }
            if(s.hold>=cfg_.MAXHOLD){ close_pos(s,c,"MAXHOLD"); return; }
            return;
        }
        // ---- entry gates (mirror the validated backtest exactly) ----
        bool fire=false;
        if(c>=cfg_.PX_MIN && s.day_open>0){
            double day_up=(c/s.day_open-1)*100;
            if(day_up>=cfg_.GATE && (int)s.vols.size()>=20 && (int)s.closes.size()>=cfg_.LB){
                double avgv=0; for(int k=0;k<20;k++) avgv+=s.vols[s.vols.size()-1-k]; avgv/=20.0;
                double base=s.closes[s.closes.size()-cfg_.LB];
                if(avgv>0 && v>=cfg_.VOLX*avgv && base>0 && (c/base-1)*100>=cfg_.IG) fire=true;
            }
        }
        if(fire){
            double notional=risk_.allow_entry(s.c.symbol,c,c*(1-cfg_.TRAIL)); // trail as soft stop for sizing
            if(notional>0){
                long sz=(long)std::max(1.0,notional/c); risk_.on_open(s.c.symbol,notional);
                s.inpos=true; s.entry=c; s.peak=h; s.hold=0; s.notional=notional;
                printf("[BigCapMomo] %s LONG %s c=%.2f day_up=%.0f%% sz=%ld notional=$%.0f [conc=%d]\n",
                    cfg_.PAPER_ONLY?"PAPER":"LIVE",s.c.symbol.c_str(),c,(c/s.day_open-1)*100,sz,notional,risk_.open_count());
                fflush(stdout);
                if(!cfg_.PAPER_ONLY){ Order ord; ord.action="BUY"; ord.orderType="MKT";
                    ord.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,s.c,ord); }
            }
        }
        // push finalized bar into history (after entry eval, so lookback excludes current)
        s.closes.push_back(c); s.vols.push_back(v);
        if((int)s.closes.size()>200) s.closes.pop_front();
        if((int)s.vols.size()>200)   s.vols.pop_front();
    }

    void close_pos(Sym& s,double px,const char*why){
        double ret=(px*(1-0.0008))/s.entry-1.0;       // 8bps exit slip for the log
        risk_.on_close(s.c.symbol, ret*s.notional);
        printf("[BigCapMomo] %s EXIT %s %s entry=%.2f exit=%.2f ret=%.1f%% held=%d*5m\n",
            cfg_.PAPER_ONLY?"PAPER":"LIVE",s.c.symbol.c_str(),why,s.entry,px,ret*100,s.hold); fflush(stdout);
        if(!cfg_.PAPER_ONLY){ Order ord; ord.action="SELL"; ord.orderType="MKT";
            ord.totalQuantity=DecimalFunctions::doubleToDecimal(1.0); cli_->placeOrder(nextId_++,s.c,ord); }
        s.inpos=false; s.hold=0;
    }
    void error(int,int code,const std::string& m,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158&&code!=162&&code!=200&&code!=2150&&code!=10090) printf("[BigCapMomo] err %d: %s\n",code,m.c_str()); }
};

int main(int argc,char**argv){
    int port=4002; double gate=3.0, trail=0.04; bool live=false;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--live"))live=true;
        else if(!strcmp(argv[i],"--gate")&&i+1<argc)gate=atof(argv[++i]);
        else if(!strcmp(argv[i],"--trail")&&i+1<argc)trail=atof(argv[++i]);
        else port=atoi(argv[i]); }
    system("mkdir -p data/bigcapmomo 2>/dev/null");
    BigCapMomoEngine e;
    e.set(gate,trail,live);
    if(!e.connect("127.0.0.1",port,86)){ printf("connect failed\n"); return 1; }
    for(int i=0;i<100000;++i) e.pump();              // long-running intraday stream
    e.cli()->eDisconnect(); printf("[BigCapMomo] session complete\n");
    return 0;
}
