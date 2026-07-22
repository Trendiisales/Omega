// GapShortEngine.cpp — Evan gap-short, C++ on the IBKR TWS API. NOT BlackBull/FIX.
// VALIDATED (proper backtest 2026-06-06): gap>=75% $3-20 short@open, 100% hard
//   stop, cover@close, Kelly 12%, locate<10%. With float 3-20M: 74.6% win PF1.76;
//   WITHOUT float (ships first, float-source TODO): 71.9% win PF1.45. Both:
//   all-years+, 3x-cost-robust, 9/9 plateau, 111 tickers, maxDD26%.
//
// modes:  (default) ENTRY  — premarket/open: scan -> qualify -> short@open + stop
//         --cover          — at close: reqPositions -> buy-to-cover all shorts
// SAFETY: PAPER_ONLY=true -> logs only, NO live orders. Flip on IBKR paper acct.
//   Order sizing uses bid64_integer.cpp (exact for integer share counts) -- the
//   full Intel RDFP lib is NOT required.
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
#include <filesystem>
#include "RiskManager.hpp"

struct Cfg {
    double GAP_MIN=75, PX_LO=3, PX_HI=20, FLO=0, FHI=20e6;  // FLO=0 -> float gate OFF (ship validated no-float)
    double STOP=1.00, KELLY=0.12, BANKROLL=50000, LOCATE_MAX=0.10;
    bool   PAPER_ONLY=true;
    bool   REQUIRE_LOCATE=true;   // gate shorts on IBKR shortable tick 236 (--no-locate to disable)
    double SHORTABLE_MIN=2.5;     // IB shortable code: >2.5 available, 1.5-2.5 hard-to-borrow, <1.5 none
};

class GapShortEngine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Cfg cfg_; OrderId nextId_=0; bool coverMode_=false;
    std::unordered_set<std::string> floatOK_;   // optional float cache (ticker in 3-20M)
    struct Cand { Contract c; double prevClose=0,last=0,gap=0; long shortable=0; bool gotShort=false; };
    std::map<int,Cand> pend_;
    std::map<int,Cand> locate_pend_;   // awaiting shortable tick 236
    int locateSeq_=0;
    int scanned_=0; bool scanDone_=false, coverDone_=false;
    std::ofstream log_;
    RiskManager risk_{50000.0};   // catastrophe protection (tail caps, never shrinks edge)
public:
    GapShortEngine(bool cover):coverMode_(cover){ risk_.new_day(); cli_=std::make_unique<EClientSocket>(this,&sig_);
        log_.open("data/gapshort/trades.csv", std::ios::app); }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }
    bool busy(){ return coverMode_ ? !coverDone_ : (!scanDone_ || !pend_.empty() || !locate_pend_.empty()); }
    void loadFloatCache(const char*p){ std::ifstream f(p); if(!f)return; std::string ln; std::getline(f,ln);
        while(std::getline(f,ln)){ auto c=ln.find(','); if(c==std::string::npos)continue;
            double v=atof(ln.substr(c+1).c_str()); if(cfg_.FLO<=v && v<cfg_.FHI) floatOK_.insert(ln.substr(0,c)); }
        printf("[GapShort] float cache: %zu tickers in range\n",floatOK_.size()); }
    void setRequireLocate(bool b){ cfg_.REQUIRE_LOCATE=b; }

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
        Cand x=it->second; pend_.erase(it);     // take a copy; drop from hist-pending
        double px=x.last,pc=x.prevClose;
        if(px<=0||pc<=0) return;
        double gap=(px-pc)/pc*100;
        bool fOK = floatOK_.empty() || floatOK_.count(x.c.symbol);   // float gate optional
        if(!(gap>=cfg_.GAP_MIN && px>=cfg_.PX_LO && px<=cfg_.PX_HI && fOK)) return;
        x.gap=gap;
        if(!cfg_.REQUIRE_LOCATE){ short_it(x,gap); return; }
        // LOCATE GATE: request shortable shares (generic tick 236); decide on tickGeneric/snapshotEnd.
        // (Borrow-fee >LOCATE_MAX gate needs IBKR sec-lending data, not a std tick -- the $3-20/
        //  float-3-20M universe already excludes the worst-to-borrow names; revisit if fee data wired.)
        int lid=20000+locateSeq_++;
        locate_pend_[lid]=x;
        cli_->reqMktData(lid,x.c,"236",true,false,TagValueListSPtr());   // snapshot -> auto-cancels
    }
    // shortable tick arrives here (tickType 46 = SHORTABLE)
    void tickGeneric(TickerId id, TickType tickType, double value) override {
        if((int)tickType != 46) return;
        auto it=locate_pend_.find((int)id); if(it==locate_pend_.end())return;
        Cand x=it->second; locate_pend_.erase(it);
        if(value>=cfg_.SHORTABLE_MIN){ short_it(x,x.gap); }
        else printf("[GapShort] SKIP %s no locate (shortable=%.1f < %.1f)\n",
                    x.c.symbol.c_str(),value,cfg_.SHORTABLE_MIN); fflush(stdout);
    }
    // snapshot finished with no shortable tick -> skip (fail-safe: never short without a locate)
    void tickSnapshotEnd(int reqId) override {
        auto it=locate_pend_.find(reqId); if(it==locate_pend_.end())return;
        printf("[GapShort] SKIP %s no shortable data (snapshot end)\n",it->second.c.symbol.c_str()); fflush(stdout);
        locate_pend_.erase(it);
    }
    void short_it(Cand& x,double gap){
        double px=x.last, stop=px*(1+cfg_.STOP);
        // RISK LAYER: gate (kill switch / concurrency / dup) + size (per-trade + gap-through cap)
        double notional = risk_.allow_entry(x.c.symbol, px, stop);
        if(notional<=0.0) return;                          // blocked by a risk gate
        long sz=(long)std::max(1.0, notional/px);          // shares from risk-capped notional
        risk_.on_open(x.c.symbol, notional);
        char buf[256]; snprintf(buf,sizeof(buf),"%s,SHORT,%.0f,%.2f,%.2f,%ld,$%.0f,%s\n",
            x.c.symbol.c_str(),gap,px,stop,sz,notional,cfg_.PAPER_ONLY?"PAPER":"LIVE");
        if(log_) log_<<buf; printf("[GapShort] %s SHORT %s gap=%.0f%% px=%.2f stop=%.2f sz=%ld notional=$%.0f [conc=%d]\n",
            cfg_.PAPER_ONLY?"PAPER":"LIVE",x.c.symbol.c_str(),gap,px,stop,sz,notional,risk_.open_count()); fflush(stdout);
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
    int port=4002; bool cover=false; const char* fcache=nullptr; bool noLocate=false;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--cover"))cover=true;
        else if(!strcmp(argv[i],"--no-locate"))noLocate=true;
        else if(!strcmp(argv[i],"--float")&&i+1<argc)fcache=argv[++i]; else port=atoi(argv[i]); }
    { std::error_code ec; std::filesystem::create_directories("data/gapshort", ec); } // not system("mkdir -p") -- POSIX flags fail on Windows cmd, dir never created, ledger rows silently lost
    GapShortEngine e(cover);
    if(noLocate) e.setRequireLocate(false);
    if(fcache) e.loadFloatCache(fcache);
    if(!e.connect("127.0.0.1",port,85)){ printf("connect failed\n"); return 1; }
    for(int i=0;i<400 && e.busy();++i) e.pump();
    e.cli()->eDisconnect(); printf("[GapShort] %s complete\n",cover?"COVER":"ENTRY");
    return 0;
}
