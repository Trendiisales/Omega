// GapShortDaily.cpp — production daily-loop for the gap-short. ONE stateful process:
// enter at open (locate-gated, risk-gated) -> monitor (kill switch on daily PnL) ->
// cover at close. Makes the RiskManager kill switch + concurrency real across the day.
// PAPER_ONLY default. Build: cmake --build build --target GapShortDaily
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
#include <map>
#include <memory>
#include <ctime>
#include <cmath>
#include <fstream>

struct Cfg { double GAP_MIN=75,PX_LO=3,PX_HI=20,STOP=1.0; bool PAPER_ONLY=true; };
static std::string iso_utc(){ time_t t=time(nullptr); struct tm* g=gmtime(&t); char b[24]; strftime(b,sizeof(b),"%Y-%m-%dT%H:%M:%SZ",g); return std::string(b); }
static int et_hhmm(){ time_t t=time(nullptr); struct tm* g=gmtime(&t); int h=(g->tm_hour+24-4)%24; return h*100+g->tm_min; } // EDT approx (portable)

class GapShortDaily : public DefaultEWrapper {
    EReaderOSSignal sig_{1000}; std::unique_ptr<EClientSocket> cli_; std::unique_ptr<EReader> rd_;
    Cfg cfg_; OrderId nextId_=1; std::string acct_; RiskManager risk_{50000.0};
    struct Cand{ Contract c; double prevClose=0,last=0; double shortable=-1; bool gap_ok=false; };
    std::map<int,Cand> hist_; std::map<int,Cand> loc_;   // reqId -> candidate (hist then locate)
    std::map<long,std::pair<Contract,long>> openpos_;     // permId-ish -> (contract, shares)
    bool scanDone_=false; int scanned_=0; bool entered_=false; bool killed_=false;
    std::ofstream led_;                                   // forward ledger (entry/cover/kill)
    // forward-record ledger: every entry/cover/flatten/kill -> data/gapshort/daily_ledger.csv
    void ledger(const char* ev,const std::string& sym,const char* side,double gap,double px,double stop,long sz,double notional,const char* note){
        if(!led_) return;
        char b[320]; snprintf(b,sizeof(b),"%s,%s,%s,%s,%.1f,%.4f,%.4f,%ld,%.0f,%s,%s\n",
            iso_utc().c_str(),ev,sym.c_str(),side,gap,px,stop,sz,notional,cfg_.PAPER_ONLY?"PAPER":"LIVE",note);
        led_<<b; led_.flush();
    }
public:
    GapShortDaily(){ cli_=std::make_unique<EClientSocket>(this,&sig_);
        system("mkdir -p data/gapshort 2>/dev/null");
        bool isnew = !std::ifstream("data/gapshort/daily_ledger.csv").good();
        led_.open("data/gapshort/daily_ledger.csv", std::ios::app);
        if(isnew && led_) led_<<"ts_utc,event,symbol,side,gap_pct,price,stop,shares,notional,mode,note\n";
    }
    bool connect(const char*h,int p,int id){ if(!cli_->eConnect(h,p,id,false))return false; rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start(); return true; }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    EClientSocket* cli(){ return cli_.get(); }
    void managedAccounts(const std::string& a) override { if(acct_.empty()){ acct_=a.substr(0,a.find(',')); cli_->reqPnL(7777,acct_,""); printf("[DAILY] account=%s, PnL monitor on\n",acct_.c_str()); risk_.new_day(); } }
    // --- live daily PnL -> kill switch ---
    void pnl(int,double dailyPnL,double,double) override {
        if(killed_) return;
        // feed delta into RiskManager via a synthetic close (tracks cumulative day pnl)
        static double last=0; double d=dailyPnL-last; last=dailyPnL;
        if(risk_.on_close("__day__",d)){ killed_=true; flatten_all("KILL"); }
    }
    // --- ENTRY pipeline: scan -> hist(gap) -> locate(236) -> risk -> short ---
    void run_entry(){ entered_=true; ScannerSubscription s; s.instrument="STK"; s.locationCode="STK.US.MAJOR"; s.scanCode="TOP_PERC_GAIN";
        s.abovePrice=cfg_.PX_LO; s.belowPrice=cfg_.PX_HI; s.aboveVolume=100000; cli_->reqScannerSubscription(9001,s,TagValueListSPtr(),TagValueListSPtr());
        printf("[DAILY] ENTRY scan @ %d ET\n",et_hhmm()); }
    void scannerData(int,int,const ContractDetails& cd,const std::string&,const std::string&,const std::string&,const std::string&) override {
        int rid=10000+scanned_++; Cand x; x.c=cd.contract; hist_[rid]=x;
        cli_->reqHistoricalData(rid,x.c,"","2 D","1 day","TRADES",1,1,false,TagValueListSPtr()); }
    void scannerDataEnd(int rid) override { cli_->cancelScannerSubscription(rid); scanDone_=true; }
    void historicalData(TickerId rid,const Bar& b) override { auto it=hist_.find((int)rid); if(it==hist_.end())return; it->second.prevClose=it->second.last; it->second.last=b.close; }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        auto it=hist_.find(rid); if(it==hist_.end())return; Cand x=it->second; hist_.erase(it);
        double px=x.last,pc=x.prevClose; if(px<=0||pc<=0)return; double gap=(px-pc)/pc*100;
        if(gap<cfg_.GAP_MIN||px<cfg_.PX_LO||px>cfg_.PX_HI)return;
        x.gap_ok=true; int lid=20000+rid; loc_[lid]=x;                 // LOCATE check next
        cli_->reqMktData(lid,x.c,"236",true,false,TagValueListSPtr()); }
    void tickGeneric(TickerId id,TickType tt,double val) override {   // 46 = SHORTABLE
        auto it=loc_.find((int)id); if(it==loc_.end()||tt!=46)return; it->second.shortable=val; }
    void tickSnapshotEnd(int id) override {
        auto it=loc_.find(id); if(it==loc_.end())return; Cand x=it->second; loc_.erase(it);
        double sh=x.shortable;          // <1.5 = not shortable, >=2.5 = easy borrow
        if(sh<1.5){ printf("[DAILY] SKIP %s: no locate (shortable=%.1f)\n",x.c.symbol.c_str(),sh); return; }
        double px=x.last, stop=px*(1+cfg_.STOP), gap=(px-x.prevClose)/x.prevClose*100;
        double notional=risk_.allow_entry(x.c.symbol,px,stop); if(notional<=0)return;
        long sz=(long)std::max(1.0,notional/px); risk_.on_open(x.c.symbol,notional);
        printf("[DAILY] %s SHORT %s gap=%.0f%% px=%.2f stop=%.2f sz=%ld notional=$%.0f conc=%d\n",
            cfg_.PAPER_ONLY?"PAPER":"LIVE",x.c.symbol.c_str(),gap,px,stop,sz,notional,risk_.open_count());
        ledger("ENTRY",x.c.symbol,"SHORT",gap,px,stop,sz,notional,"");
        if(!cfg_.PAPER_ONLY){ Order so;so.action="SELL";so.orderType="MKT";so.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,x.c,so);
            Order st;st.action="BUY";st.orderType="STP";st.auxPrice=stop;st.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,x.c,st);
            openpos_[nextId_]={x.c,sz}; } }
    // --- COVER / FLATTEN ---
    void flatten_all(const char* why){ printf("[DAILY] FLATTEN ALL (%s) @ %d ET\n",why,et_hhmm()); ledger("FLATTEN","*","-",0,0,0,0,0,why); cli_->reqGlobalCancel(); cli_->reqPositions(); }
    void position(const std::string&,const Contract& c,Decimal pos,double) override {
        double p=DecimalFunctions::decimalToDouble(pos);
        if(p<0){ long sz=(long)(-p); printf("[DAILY] %s COVER %s sz=%ld\n",cfg_.PAPER_ONLY?"PAPER":"LIVE",c.symbol.c_str(),sz);
            ledger("COVER",c.symbol,"BUY",0,0,0,sz,0,"flatten");
            if(!cfg_.PAPER_ONLY){ Order o;o.action="BUY";o.orderType="MKT";o.totalQuantity=DecimalFunctions::doubleToDecimal((double)sz); cli_->placeOrder(nextId_++,c,o);} } }
    void positionEnd() override { cli_->cancelPositions(); }
    void error(int,int code,const std::string& m,const std::string&) override { if(code!=2104&&code!=2106&&code!=2158&&code!=2150&&code!=200&&code!=162) printf("[DAILY] err %d %s\n",code,m.c_str()); }
    void set_paper_only(bool p){ cfg_.PAPER_ONLY=p; }
    // S-2026-07-23a proving-size: operator minimum-first rule — the hardcoded
    // $50k RiskManager sized $4k/trade with a 100% stop. --equity=N rescales
    // every cap (risk 8%, name cap 10%, daily kill 12%) to the chosen base.
    void set_equity(double v){ if(v>0) risk_ = RiskManager(v); }
    bool entered() const { return entered_; }
    bool killed()  const { return killed_; }
};

int main(int argc,char**argv){ setvbuf(stdout,nullptr,_IONBF,0);
    int port=4002; bool send_orders=false; double equity=50000.0;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--orders")) send_orders=true;
        else if(!strncmp(argv[i],"--equity=",9)) equity=atof(argv[i]+9);
        else port=atoi(argv[i]);
    }
    // --orders: actually submit. default = log-only. --equity=N = proving-size base
    // (S-23a operator minimum-first: 2500 -> ~$200/trade, worst stop-out ~-$200).
    GapShortDaily e; e.set_paper_only(!send_orders); e.set_equity(equity);
    printf("[DAILY] orders=%s (account gate is IB paper DU; --orders flips submission)\n", send_orders?"ON":"LOG-ONLY");
    if(!e.connect("127.0.0.1",port,86)){printf("connect failed\n");return 1;}
    printf("[DAILY] up. open=0935 close=1555 ET (current %d)\n",et_hhmm());
    bool covered=false;
    for(int tick=0;;++tick){ e.pump();
        int et=et_hhmm();
        if(!e.entered() && et>=935 && et<1555 && !e.killed()) e.run_entry();
        if(!covered && et>=1555){ e.flatten_all("EOD"); covered=true; }
        if(covered && tick>4000) break;
        // (test mode: if invoked off-hours, one scan pass then exit after ~50 pumps)
        if(et<935 && tick>60) { e.run_entry(); }
        if(tick>2000 && (et<935||et>=1600)) break;
    }
    e.cli()->eDisconnect(); printf("[DAILY] session end\n"); return 0; }
