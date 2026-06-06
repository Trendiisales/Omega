// CapitulationDaily.cpp — production daily-loop for the Lance capitulation swing.
// Capitulation is a SWING (holds overnight, trails prior-bar-low), NOT intraday like
// the gap-short. Model: IBKR holds GTC bracket orders server-side overnight; each
// weekday morning this process relaunches and does TWO passes:
//   A) REPROTECT — cancel stale working orders, pull open longs, re-place a fresh GTC
//      protective stop at the NEW prior-bar low (the daily trailing-stop ratchet).
//   B) ENTRY     — scan biggest losers -> qualify waterfall+turn -> GTC bracket
//      (STP entry @ prior-high, STP loss @ prior-low, LMT TP @ +50%), risk-gated.
// Then it monitors the daily-PnL kill switch for a few hours and exits; GTC orders +
// positions persist to the next morning. Same RiskManager as the gap-short.
//   --orders  = submit to the IB **paper** account (DU) = live rehearsal. default log-only.
// Build: cmake --build build --target CapitulationDaily
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
#include <ctime>
#include <cmath>

static int et_hhmm(){ time_t t=time(nullptr); struct tm* g=gmtime(&t); int h=(g->tm_hour+24-4)%24; return h*100+g->tm_min; } // EDT approx

class CapitulationDaily : public DefaultEWrapper {
    EReaderOSSignal sig_{1000}; std::unique_ptr<EClientSocket> cli_; std::unique_ptr<EReader> rd_;
    OrderId nextId_=1; std::string acct_; bool paper_=true; bool killed_=false;
    RiskManager risk_{50000.0,RiskLimits{0.08,0.10,4,0.12,1.0}};   // long -> worst_case_gap=1.0
    struct Cand{ Contract c; std::vector<double> hi,lo,cl,vol; };
    std::map<int,Cand> pend_;                 // 30000+ entry-scan hist
    struct Held{ Contract c; long qty=0; };
    std::map<int,Held> reprot_;               // 40000+ reprotect hist (reqId -> held long)
    std::vector<Held> longs_;                 // open longs collected from reqPositions
    int scanned_=0; bool scanDone_=false;
    bool didReprotect_=false, didEntry_=false;
public:
    CapitulationDaily(){ cli_=std::make_unique<EClientSocket>(this,&sig_); }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false; rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }
    void set_paper_only(bool p){ paper_=p; }
    bool killed() const { return killed_; }
    bool entered() const { return didEntry_; }

    void managedAccounts(const std::string& a) override { if(acct_.empty()){ acct_=a.substr(0,a.find(',')); cli_->reqPnL(7778,acct_,""); risk_.new_day(); printf("[CAPIT] account=%s, PnL kill monitor on\n",acct_.c_str()); } }
    void nextValidId(OrderId id) override { nextId_=id; }

    // --- daily kill switch ---
    void pnl(int,double dailyPnL,double,double) override {
        if(killed_) return; static double last=0; double d=dailyPnL-last; last=dailyPnL;
        if(risk_.on_close("__day__",d)){ killed_=true; flatten_all("KILL"); } }
    void flatten_all(const char* why){ printf("[CAPIT] FLATTEN ALL (%s) @ %d ET\n",why,et_hhmm()); cli_->reqGlobalCancel(); cli_->reqPositions(); /*positions->market sell below*/ killFlatten_=true; }
    bool killFlatten_=false;

    // ===== PASS A: REPROTECT (cancel working orders, re-trail stops on open longs) =====
    void run_reprotect(){ didReprotect_=true; longs_.clear();
        printf("[CAPIT] REPROTECT: clearing working orders + re-trailing stops @ %d ET\n",et_hhmm());
        cli_->reqGlobalCancel();                 // clears stale GTC brackets (NOT filled positions)
        cli_->reqPositions(); }
    void position(const std::string&,const Contract& c,Decimal pos,double) override {
        double p=DecimalFunctions::decimalToDouble(pos);
        if(killFlatten_){ if(p>0){ printf("[CAPIT] %s SELL-FLATTEN %s qty=%.0f\n",paper_?"PAPER":"LIVE",c.symbol.c_str(),p);
            if(!paper_){ Order o;o.action="SELL";o.orderType="MKT";o.totalQuantity=DecimalFunctions::doubleToDecimal(p); cli_->placeOrder(nextId_++,c,o);} } return; }
        if(p>0){ Held h; h.c=c; h.qty=(long)p; longs_.push_back(h); }   // collect open longs
    }
    void positionEnd() override { cli_->cancelPositions();
        if(killFlatten_) return;
        // for each open long, pull 2D daily -> new prior-low -> fresh GTC protective stop
        int rid=40000; for(auto& h: longs_){ reprot_[rid]=h; cli_->reqHistoricalData(rid,h.c,"","2 D","1 day","TRADES",1,1,false,TagValueListSPtr()); rid++; }
        if(longs_.empty()) printf("[CAPIT] REPROTECT: no open longs\n");
    }

    // ===== PASS B: ENTRY (scan -> qualify -> GTC bracket) =====
    void run_entry(){ didEntry_=true;
        ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_LOSE";
        s.abovePrice=1; s.belowPrice=50; s.aboveVolume=500000;
        cli_->reqScannerSubscription(9101,s,TagValueListSPtr(),TagValueListSPtr());
        printf("[CAPIT] ENTRY scan (waterfall losers) @ %d ET\n",et_hhmm()); }
    void scannerData(int,int,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        int rid=30000+scanned_++; Cand x; x.c=cd.contract; pend_[rid]=x;
        cli_->reqHistoricalData(rid,x.c,"","25 D","1 day","TRADES",1,1,false,TagValueListSPtr()); }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }

    void historicalData(TickerId rid,const Bar& b) override {
        int r=(int)rid;
        if(r>=40000){ auto it=reprot_.find(r); if(it!=reprot_.end()){ rp_hi_[r].push_back(b.high); rp_lo_[r].push_back(b.low);} return; }
        auto it=pend_.find(r); if(it==pend_.end())return;
        it->second.hi.push_back(b.high); it->second.lo.push_back(b.low); it->second.cl.push_back(b.close);
        it->second.vol.push_back((double)DecimalFunctions::decimalToDouble(b.volume)); }
    std::map<int,std::vector<double>> rp_hi_, rp_lo_;

    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        if(rid>=40000){ auto it=reprot_.find(rid); if(it==reprot_.end())return; Held h=it->second; reprot_.erase(it);
            auto& lo=rp_lo_[rid]; if(lo.size()<2){ printf("[CAPIT] reprotect %s: no prior-low\n",h.c.symbol.c_str()); return; }
            double newstop=lo[lo.size()-2];                       // yesterday's low = trailing stop
            printf("[CAPIT] %s RE-TRAIL stop -> %.2f (qty=%ld)\n",h.c.symbol.c_str(),newstop,h.qty);
            if(!paper_){ Order st;st.action="SELL";st.orderType="STP";st.auxPrice=newstop;st.tif="GTC";st.totalQuantity=DecimalFunctions::doubleToDecimal((double)h.qty); cli_->placeOrder(nextId_++,h.c,st); }
            return; }
        auto it=pend_.find(rid); if(it==pend_.end())return; Cand x=it->second; pend_.erase(it);
        int n=x.cl.size(); if(n<22) return; int i=n-1;
        bool down=true; for(int k=i-5;k<i-1;k++) if(x.cl[k+1]>=x.cl[k]) down=false;
        if(!down) return;
        double drop=(x.cl[i-6]-x.cl[i-1])/x.cl[i-6]*100; if(drop<25) return;
        double avgv=0; for(int k=i-21;k<i-1;k++) avgv+=x.vol[k]; avgv/=20;
        if(avgv<=0 || x.vol[i-1]<3*avgv) return;
        if(x.hi[i]<=x.hi[i-1]) return;                            // TURN: today broke prior high
        double entry=x.hi[i-1], stop=x.lo[i-1], px=x.cl[i];
        double notional=risk_.allow_entry(x.c.symbol,px,stop); if(notional<=0) return;
        long sz=(long)std::max(1.0,notional/px); risk_.on_open(x.c.symbol,notional);
        printf("[CAPIT] %s LONG %s drop=%.0f%% entry~%.2f stop=%.2f tgt=%.2f sz=%ld notional=$%.0f conc=%d\n",
            paper_?"PAPER":"LIVE",x.c.symbol.c_str(),drop,entry,stop,entry*1.5,sz,notional,risk_.open_count());
        if(!paper_){
            Order bo;bo.action="BUY";bo.orderType="STP";bo.auxPrice=entry;bo.tif="GTC";bo.transmit=false;bo.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); OrderId eId=nextId_++; cli_->placeOrder(eId,x.c,bo);
            Order sl;sl.action="SELL";sl.orderType="STP";sl.auxPrice=stop;sl.tif="GTC";sl.parentId=eId;sl.transmit=false;sl.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,x.c,sl);
            Order tp;tp.action="SELL";tp.orderType="LMT";tp.lmtPrice=entry*1.5;tp.tif="GTC";tp.parentId=eId;tp.transmit=true;tp.totalQuantity=DecimalFunctions::doubleToDecimal((double)(sz/2)); cli_->placeOrder(nextId_++,x.c,tp); }
    }
    void error(int,int code,const std::string& m,const std::string&) override { if(code!=2104&&code!=2106&&code!=2158&&code!=2150&&code!=200&&code!=162) printf("[CAPIT] err %d %s\n",code,m.c_str()); }
};

int main(int argc,char**argv){ setvbuf(stdout,nullptr,_IONBF,0);
    int port=4002; bool send_orders=false;
    for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"--orders")) send_orders=true; else port=atoi(argv[i]); }
    CapitulationDaily e; e.set_paper_only(!send_orders);
    printf("[CAPIT] up. orders=%s reprotect=0945 entry=1000 monitor->1200 ET (current %d)\n", send_orders?"ON":"LOG-ONLY", et_hhmm());
    if(!e.connect("127.0.0.1",port,87)){printf("connect failed\n");return 1;}
    bool offhours = (et_hhmm()<930 || et_hhmm()>=1600);
    bool e_reprotect_done=false;
    for(int tick=0;;++tick){ e.pump(); int et=et_hhmm();
        if(!e.killed()){
            if((et>=945 || offhours) && tick>40 && !e_reprotect_done){ e.run_reprotect(); e_reprotect_done=true; }
            if((et>=1000 || offhours) && tick>120 && e_reprotect_done && !e.entered()){ e.run_entry(); }
        }
        if(e.entered() && tick>300 && (et>=1200 || offhours)) break;   // GTC orders persist; exit
        if(tick>4000) break;
    }
    e.cli()->eDisconnect(); printf("[CAPIT] session end\n"); return 0; }
