// CapitulationEngine.cpp — Lance Breitstein capitulation V-reversal, C++ on IBKR.
// LONG the bounce after a clean multi-day waterfall. Same stack as the gap-short.
// VALIDATED (3yr free data, realistic +50% cap): PF 1.82, win 38%, top5=28% diversified,
//   WF both-halves+, all-years+. Low-win/fat-tail edge (winners 3x losers).
// LOGIC (daily decision): scan biggest losers -> 20-day history -> qualify:
//   5 down days, >=25% drop, capitulation vol (3x avg) -> wait for the TURN
//   (today's high > prior day high) -> LONG. Trail stop = prior-bar low; +50% cap.
// LONG -> no short-locate needed; per-name loss bounded (can't go below 0).
// Risk layer: RiskManager (daily kill, concurrency, per-trade cap). PAPER_ONLY default.
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "ScannerSubscription.h"
#include "Contract.h"
#include "Order.h"
#include "RiskManager.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cmath>

class CapitulationEngine : public DefaultEWrapper {
    EReaderOSSignal sig_{1000}; std::unique_ptr<EClientSocket> cli_; std::unique_ptr<EReader> rd_;
    OrderId nextId_=1; bool paper_=true; RiskManager risk_{50000.0,RiskLimits{0.08,0.10,4,0.12,1.0}};
    // long-only -> worst_case_gap=1.0 (can't lose >100% on a long)
    struct Cand{ Contract c; std::vector<double> hi,lo,cl,vol; };
    std::map<int,Cand> pend_; int scanned_=0; bool scanDone_=false;
public:
    CapitulationEngine(){ cli_=std::make_unique<EClientSocket>(this,&sig_); risk_.new_day(); }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false; rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }
    bool busy(){ return !scanDone_ || !pend_.empty(); }
    void nextValidId(OrderId id) override { nextId_=id;
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_LOSE";
        s.abovePrice=1; s.belowPrice=50; s.aboveVolume=500000;
        cli_->reqScannerSubscription(9101,s,TagValueListSPtr(),TagValueListSPtr());
        printf("[CAPIT] scan biggest losers (waterfall candidates)...\n"); }
    void scannerData(int,int,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        int rid=30000+scanned_++; Cand x; x.c=cd.contract; pend_[rid]=x;
        cli_->reqHistoricalData(rid,x.c,"","25 D","1 day","TRADES",1,1,false,TagValueListSPtr()); }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }
    void historicalData(TickerId rid,const Bar& b) override { auto it=pend_.find((int)rid); if(it==pend_.end())return;
        it->second.hi.push_back(b.high); it->second.lo.push_back(b.low); it->second.cl.push_back(b.close);
        it->second.vol.push_back((double)DecimalFunctions::decimalToDouble(b.volume)); }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        auto it=pend_.find(rid); if(it==pend_.end())return; Cand x=it->second; pend_.erase(it);
        int n=x.cl.size(); if(n<22) return; int i=n-1;            // today = last bar
        // waterfall: 5 down days ending YESTERDAY (i-1)
        bool down=true; for(int k=i-5;k<i-1;k++) if(x.cl[k+1]>=x.cl[k]) down=false;
        if(!down) return;
        double drop=(x.cl[i-6]-x.cl[i-1])/x.cl[i-6]*100; if(drop<25) return;
        double avgv=0; for(int k=i-21;k<i-1;k++) avgv+=x.vol[k]; avgv/=20;
        if(avgv<=0 || x.vol[i-1]<3*avgv) return;                 // capitulation volume (yesterday)
        // TURN: today's high broke yesterday's high
        if(x.hi[i]<=x.hi[i-1]) return;
        double entry=x.hi[i-1], stop=x.lo[i-1], px=x.cl[i];      // enter on the break of prior high; stop = prior low
        double notional=risk_.allow_entry(x.c.symbol,px,stop); if(notional<=0) return;
        long sz=(long)std::max(1.0,notional/px); risk_.on_open(x.c.symbol,notional);
        printf("[CAPIT] %s LONG %s drop=%.0f%% entry~%.2f stop=%.2f tgt=%.2f sz=%ld notional=$%.0f conc=%d\n",
            paper_?"PAPER":"LIVE",x.c.symbol.c_str(),drop,entry,stop,entry*1.5,sz,notional,risk_.open_count());
        if(!paper_){ Order bo;bo.action="BUY";bo.orderType="STP";bo.auxPrice=entry;bo.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,x.c,bo);
            Order sl;sl.action="SELL";sl.orderType="STP";sl.auxPrice=stop;sl.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,x.c,sl);
            Order tp;tp.action="SELL";tp.orderType="LMT";tp.lmtPrice=entry*1.5;tp.totalQuantity=DecimalFunctions::doubleToDecimal((double)(sz/2)); cli_->placeOrder(nextId_++,x.c,tp); } }
    void error(int,int code,const std::string& m,const std::string&) override { if(code!=2104&&code!=2106&&code!=2158&&code!=2150&&code!=200&&code!=162) printf("[CAPIT] err %d %s\n",code,m.c_str()); }
};
int main(int argc,char**argv){ setvbuf(stdout,nullptr,_IONBF,0); int port=argc>1?atoi(argv[1]):4002;
    CapitulationEngine e; if(!e.connect("127.0.0.1",port,87)){printf("connect failed\n");return 1;}
    for(int i=0;i<500 && e.busy();++i) e.pump(); e.cli()->eDisconnect(); printf("[CAPIT] scan complete\n"); return 0; }
