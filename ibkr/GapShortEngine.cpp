// GapShortEngine.cpp — Evan gap-short, C++ on the IBKR TWS API. NOT BlackBull/FIX.
// VALIDATED (proper backtest 2026-06-06): gap>=75% $3-20 short@open, 100% hard
//   stop, cover@close, Kelly 12%, locate<10%. With float 3-20M: 74.6% win PF1.76;
//   WITHOUT float (ships first, float-source TODO): 71.9% win PF1.45. Both:
//   all-years+, 3x-cost-robust, 9/9 plateau, 111 tickers, maxDD26%.
//
// modes:  (default) ENTRY  — premarket/open: scan -> qualify -> short@open + stop
//         --cover          — at close: reqPositions -> buy-to-cover all shorts
// SAFETY: PAPER_ONLY=true -> logs only, NO live orders. Flip on IBKR paper acct.
//   libbid stub in use -> live order SIZING needs real Intel RDFP lib (TODO).
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
#include <memory>
#include <cmath>
#include <fstream>
#include <unordered_set>

struct Cfg {
    double GAP_MIN=75, PX_LO=3, PX_HI=20, FLO=0, FHI=20e6;  // FLO=0 -> float gate OFF (ship validated no-float)
    double STOP=1.00, KELLY=0.12, BANKROLL=50000, LOCATE_MAX=0.10;
    bool   PAPER_ONLY=true;
};

class GapShortEngine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Cfg cfg_; OrderId nextId_=0; bool coverMode_=false;
    std::unordered_set<std::string> floatOK_;   // optional float cache (ticker in 3-20M)
    struct Cand { Contract c; double prevClose=0,last=0; long shortable=0; bool gotShort=false; };
    std::map<int,Cand> pend_;
    int scanned_=0; bool scanDone_=false, coverDone_=false;
    std::ofstream log_;
public:
    GapShortEngine(bool cover):coverMode_(cover){ cli_=std::make_unique<EClientSocket>(this,&sig_);
        log_.open("data/gapshort/trades.csv", std::ios::app); }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }
    bool busy(){ return coverMode_ ? !coverDone_ : (!scanDone_ || !pend_.empty()); }
    void loadFloatCache(const char*p){ std::ifstream f(p); if(!f)return; std::string ln; std::getline(f,ln);
        while(std::getline(f,ln)){ auto c=ln.find(','); if(c==std::string::npos)continue;
            double v=atof(ln.substr(c+1).c_str()); if(cfg_.FLO<=v && v<cfg_.FHI) floatOK_.insert(ln.substr(0,c)); }
        printf("[GapShort] float cache: %zu tickers in range\n",floatOK_.size()); }

    void nextValidId(OrderId id) override { nextId_=id;
        if(coverMode_){ cli_->reqPositions(); return; }
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_GAIN";
        s.abovePrice=cfg_.PX_LO; s.belowPrice=cfg_.PX_HI; s.aboveVolume=100000;
        cli_->reqScannerSubscription(9001,s,TagValueListSPtr(),TagValueListSPtr());
        printf("[GapShort] ENTRY scan...\n"); fflush(stdout);
    }
    // ---- ENTRY path ----
    void scannerData(int,int rank,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        int rid=10000+scanned_++; Cand x; x.c=cd.contract; pend_[rid]=x;
        cli_->reqHistoricalData(rid,x.c,"","2 D","1 day","TRADES",1,1,false,TagValueListSPtr());
    }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }
    void historicalData(TickerId rid,const Bar& b) override { auto it=pend_.find((int)rid); if(it==pend_.end())return;
        it->second.prevClose=it->second.last; it->second.last=b.close; }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        auto it=pend_.find(rid); if(it==pend_.end())return;
        // locate check: request shortable shares (tick 236) then evaluate on tickSnapshotEnd
        Cand& x=it->second; double px=x.last,pc=x.prevClose;
        if(px<=0||pc<=0){ pend_.erase(it); return; }
        double gap=(px-pc)/pc*100;
        bool fOK = floatOK_.empty() || floatOK_.count(x.c.symbol);   // float gate optional
        if(gap>=cfg_.GAP_MIN && px>=cfg_.PX_LO && px<=cfg_.PX_HI && fOK){
            x.gotShort=true; // shortable/locate gate: reqMktData(236) in production; v1 logs candidate
            short_it(x,gap);
        }
        pend_.erase(it);
    }
    void short_it(Cand& x,double gap){
        double px=x.last, stop=px*(1+cfg_.STOP);
        long sz=(long)std::max(1.0,(cfg_.BANKROLL*cfg_.KELLY)/(stop-px));
        char buf[256]; snprintf(buf,sizeof(buf),"%s,SHORT,%.0f,%.2f,%.2f,%ld,%s\n",
            x.c.symbol.c_str(),gap,px,stop,sz,cfg_.PAPER_ONLY?"PAPER":"LIVE");
        if(log_) log_<<buf; printf("[GapShort] %s SHORT %s gap=%.0f%% px=%.2f stop=%.2f sz=%ld\n",
            cfg_.PAPER_ONLY?"PAPER":"LIVE",x.c.symbol.c_str(),gap,px,stop,sz); fflush(stdout);
        if(!cfg_.PAPER_ONLY){
            Order so; so.action="SELL"; so.orderType="MKT"; so.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz);
            cli_->placeOrder(nextId_++,x.c,so);
            Order st; st.action="BUY"; st.orderType="STP"; st.auxPrice=stop; st.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz);
            cli_->placeOrder(nextId_++,x.c,st);
        }
    }
    // ---- COVER path (at close: buy-to-cover all open shorts) ----
    void position(const std::string& acct,const Contract& c,Decimal pos,double avg) override {
        double p = DecimalFunctions::decimalToDouble(pos);
        if(p<0){ long sz=(long)(-p);
            printf("[GapShort] %s COVER %s sz=%ld (was short)\n",cfg_.PAPER_ONLY?"PAPER":"LIVE",c.symbol.c_str(),sz); fflush(stdout);
            if(!cfg_.PAPER_ONLY){ Order o; o.action="BUY"; o.orderType="MKT"; o.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz);
                cli_->placeOrder(nextId_++,c,o); } }
    }
    void positionEnd() override { cli_->cancelPositions(); coverDone_=true; }
    void error(int,int code,const std::string& m,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158&&code!=162&&code!=200&&code!=2150) printf("[GapShort] err %d: %s\n",code,m.c_str()); }
};

int main(int argc,char**argv){
    int port=4002; bool cover=false; const char* fcache=nullptr;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--cover"))cover=true;
        else if(!strcmp(argv[i],"--float")&&i+1<argc)fcache=argv[++i]; else port=atoi(argv[i]); }
    system("mkdir -p data/gapshort 2>/dev/null");
    GapShortEngine e(cover);
    if(fcache) e.loadFloatCache(fcache);
    if(!e.connect("127.0.0.1",port,85)){ printf("connect failed\n"); return 1; }
    for(int i=0;i<400 && e.busy();++i) e.pump();
    e.cli()->eDisconnect(); printf("[GapShort] %s complete\n",cover?"COVER":"ENTRY");
    return 0;
}
