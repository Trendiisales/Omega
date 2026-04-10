// candle_rsi_l2_bt_final.cpp
// Backtest of CandleFlowEngine with sweep-optimised settings
// RSI_P=30 EMA=10 THRESH=6.0 IMB=0.05 IMB_TICKS=2 BODY=0.60 COST_MULT=2.5 STAG=60s
//
// Build: g++ -O2 -std=c++17 -o candle_rsi_l2_bt_final candle_rsi_l2_bt_final.cpp
// Run:   ./candle_rsi_l2_bt_final l2_ticks_2026-04-10.csv

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <iomanip>
#include <map>

static constexpr double  CFE_BODY_RATIO_MIN  = 0.60;
static constexpr double  CFE_COST_SLIP       = 0.10;
static constexpr double  CFE_COST_COMM       = 0.10;
static constexpr double  CFE_COST_MULT       = 2.5;
static constexpr int64_t CFE_STAGNATION_MS   = 60000;
static constexpr double  CFE_STAGNATION_MULT = 1.0;
static constexpr double  CFE_RISK_USD        = 30.0;
static constexpr double  CFE_TICK_VALUE      = 100.0;
static constexpr int64_t CFE_COOLDOWN_MS     = 10000;
static constexpr double  CFE_MAX_SPREAD      = 0.30;
static constexpr int     CFE_RSI_PERIOD      = 30;
static constexpr int     CFE_RSI_EMA_N       = 10;
static constexpr double  CFE_RSI_THRESH      = 6.0;
static constexpr double  CFE_IMB_THRESH      = 0.05;
static constexpr int     CFE_IMB_TICKS       = 2;

struct L2Tick {
    int64_t ts_ms=0; double bid=0,ask=0,l2_imb=0.5; int depth_bid=0,depth_ask=0;
    bool watchdog_dead=false;
};
bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double&v)->bool{ if(!std::getline(ss,tok,','))return false; try{v=std::stod(tok);}catch(...){return false;} return true; };
    auto ni=[&](int&v)->bool{ if(!std::getline(ss,tok,','))return false; try{v=(int)std::stoll(tok);}catch(...){return false;} return true; };
    double tmp; int wd=0;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid))return false; if(!nd(t.ask))return false;
    if(!nd(t.l2_imb))return false;
    double bv,av; if(!nd(bv))return false; if(!nd(av))return false;
    if(!ni(t.depth_bid))return false; if(!ni(t.depth_ask))return false;
    if(!nd(tmp))return false; if(!ni(wd))return false; t.watchdog_dead=(wd!=0);
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid);
}

struct Bar { double open=0,high=0,low=0,close=0; int64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur,last,prev; int64_t cur_min=0;
    void update(int64_t ts,double mid){
        int64_t bm=ts/60000;
        if(bm!=cur_min){if(cur.valid){prev=last;last=cur;}cur={mid,mid,mid,mid,ts,true};cur_min=bm;}
        else{if(mid>cur.high)cur.high=mid;if(mid<cur.low)cur.low=mid;cur.close=mid;}
    }
    bool has2()const{return last.valid&&prev.valid;}
};
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar&b){r.push_back(b.high-b.low);if((int)r.size()>14)r.pop_front();double s=0;for(auto x:r)s+=x;val=s/r.size();}
};

struct Trade { bool is_long; double entry,exit_px,pnl_usd; int held_s; std::string reason; };

int main(int argc, char* argv[]) {
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: candle_rsi_l2_bt_final l2_ticks_YYYY-MM-DD.csv ...\n";return 1;}

    M1Builder bars; ATR14 atr;
    std::vector<Trade> trades;
    std::map<std::string,int> reasons;

    bool in_t=false,is_long=false;
    double entry=0,sl=0,sz=0,cp=0,mfe=0;
    int64_t ets=0,cool=0,lbm=0,tc=0;
    int imb_against=0,prev_db=0,prev_da=0;

    // RSI state
    std::deque<double> gains,losses;
    double pm_rsi=0,rsi_cur=50,rsi_prev=50,rsi_trend=0;
    bool warmed=false;
    const double ema_a=2.0/(CFE_RSI_EMA_N+1);

    for(auto& fname:files){
        std::ifstream f(fname);
        if(!f){std::cerr<<"Cannot open: "<<fname<<"\n";continue;}
        std::string line; std::getline(f,line);
        while(std::getline(f,line)){
            if(!line.empty()&&line.back()=='\r')line.pop_back();
            L2Tick t; if(!parse_l2(line,t))continue;
            if(t.watchdog_dead)continue;
            const double mid=(t.ask+t.bid)*0.5;
            const double sp=t.ask-t.bid;
            if(sp>CFE_MAX_SPREAD||sp<=0)continue;
            tc++;

            // RSI update
            if(pm_rsi!=0){
                double chg=mid-pm_rsi;
                gains.push_back(chg>0?chg:0); losses.push_back(chg<0?-chg:0);
                if((int)gains.size()>CFE_RSI_PERIOD){gains.pop_front();losses.pop_front();}
                if((int)gains.size()==CFE_RSI_PERIOD){
                    double ag=0,al=0;
                    for(auto x:gains)ag+=x; for(auto x:losses)al+=x;
                    ag/=CFE_RSI_PERIOD; al/=CFE_RSI_PERIOD;
                    rsi_prev=rsi_cur;
                    rsi_cur=(al==0)?100.0:100.0-100.0/(1.0+ag/al);
                    double slope=rsi_cur-rsi_prev;
                    if(!warmed){rsi_trend=slope;warmed=true;}
                    else rsi_trend=slope*ema_a+rsi_trend*(1.0-ema_a);
                }
            }
            pm_rsi=mid;

            bars.update(t.ts_ms,mid);
            int64_t bm=t.ts_ms/60000;
            if(bm!=lbm&&bars.last.valid){atr.add(bars.last);lbm=bm;}

            if(t.ts_ms<cool)continue;

            if(in_t){
                double move=is_long?(mid-entry):(entry-mid);
                if(move>mfe)mfe=move;

                // Hard SL
                if(is_long?(t.bid<=sl):(t.ask>=sl)){
                    double px=is_long?t.bid:t.ask;
                    double pnl=(is_long?(px-entry):(entry-px))*sz*CFE_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-ets)/1000),"SL_HIT"});
                    reasons["SL_HIT"]++; in_t=false; imb_against=0;
                    cool=t.ts_ms+CFE_COOLDOWN_MS; continue;
                }

                // L2 imbalance exit
                double imb=(t.l2_imb-0.5)*2.0;
                bool against=is_long?(imb<-CFE_IMB_THRESH):(imb>CFE_IMB_THRESH);
                if(against) imb_against++;
                else        imb_against=0;
                bool depth_drop=is_long?(t.depth_bid<prev_db):(t.depth_ask<prev_da);
                prev_db=t.depth_bid; prev_da=t.depth_ask;
                bool imb_exit=(imb_against>=CFE_IMB_TICKS)||(imb_against>=1&&depth_drop);
                if(imb_exit){
                    double px=is_long?t.bid:t.ask;
                    double pnl=(is_long?(px-entry):(entry-px))*sz*CFE_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)((t.ts_ms-ets)/1000),"IMB_EXIT"});
                    reasons["IMB_EXIT"]++; in_t=false; imb_against=0;
                    cool=t.ts_ms+5000; continue;
                }

                // Stagnation
                int64_t held=t.ts_ms-ets;
                if(held>=CFE_STAGNATION_MS&&mfe<cp*CFE_STAGNATION_MULT){
                    double px=is_long?t.bid:t.ask;
                    double pnl=(is_long?(px-entry):(entry-px))*sz*CFE_TICK_VALUE;
                    trades.push_back({is_long,entry,px,pnl,(int)(held/1000),"STAGNATION"});
                    reasons["STAGNATION"]++; in_t=false; imb_against=0;
                    cool=t.ts_ms+CFE_COOLDOWN_MS; continue;
                }
                continue;
            }

            // Entry
            if(!bars.has2()||atr.val<=0||!warmed)continue;
            int dir=(rsi_trend>CFE_RSI_THRESH)?+1:(rsi_trend<-CFE_RSI_THRESH)?-1:0;
            if(dir==0)continue;

            const Bar&lb=bars.last; const Bar&pb=bars.prev;
            double range=lb.high-lb.low; if(range<=0)continue;
            double bb=lb.close-lb.open,br=lb.open-lb.close;
            bool bull=(bb>0&&bb/range>=CFE_BODY_RATIO_MIN&&lb.close>pb.high);
            bool bear=(br>0&&br/range>=CFE_BODY_RATIO_MIN&&lb.close<pb.low);
            if(dir==+1&&!bull)continue;
            if(dir==-1&&!bear)continue;

            cp=sp+CFE_COST_SLIP*2.0+CFE_COST_COMM*2.0;
            if(range<CFE_COST_MULT*cp)continue;

            is_long=(dir==+1);
            entry=is_long?t.ask:t.bid;
            double slp=atr.val>0?atr.val:1.0;
            sl=is_long?entry-slp:entry+slp;
            sz=std::max(0.01,std::min(0.50,CFE_RISK_USD/(slp*CFE_TICK_VALUE)));
            sz=std::floor(sz/0.001)*0.001;
            ets=t.ts_ms; mfe=0; in_t=true;
            imb_against=0; prev_db=t.depth_bid; prev_da=t.depth_ask;
            reasons["ENTRY"]++;
            std::cout<<"[ENTRY] "<<(is_long?"LONG":"SHORT")
                     <<" @ "<<std::fixed<<std::setprecision(2)<<entry
                     <<" sl="<<sl<<" rsi_trend="<<std::setprecision(2)<<rsi_trend
                     <<" imb="<<std::setprecision(4)<<(t.l2_imb-0.5)*2.0<<"\n";
        }
    }

    std::cout<<"\n=== CandleFlowEngine — RSI Entry + L2 IMB Exit (FINAL) ===\n";
    std::cout<<"Config: RSI_P="<<CFE_RSI_PERIOD<<" EMA="<<CFE_RSI_EMA_N
             <<" THRESH="<<CFE_RSI_THRESH<<" IMB="<<CFE_IMB_THRESH
             <<" IMB_TICKS="<<CFE_IMB_TICKS<<" BODY="<<CFE_BODY_RATIO_MIN
             <<" COST_MULT="<<CFE_COST_MULT<<" STAG="<<CFE_STAGNATION_MS/1000<<"s\n";
    std::cout<<std::string(55,'-')<<"\n";
    std::cout<<"Ticks     : "<<tc<<"\n";

    if(trades.empty()){std::cout<<"No trades.\n";return 0;}

    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for(auto&tr:trades){
        total+=tr.pnl_usd; eq+=tr.pnl_usd;
        if(eq>peak)peak=eq; maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl_usd>0){w++;wpnl+=tr.pnl_usd;}else{l++;lpnl+=tr.pnl_usd;}
    }
    int n=(int)trades.size();
    double wr=100.0*w/n;
    double aw=w?wpnl/w:0,al=l?lpnl/l:0,rr=al!=0?-aw/al:0;
    double exp_pt=(wr/100.0)*aw+(1.0-wr/100.0)*al;

    std::cout<<"Trades    : "<<n<<"\n";
    std::cout<<"Win rate  : "<<std::fixed<<std::setprecision(1)<<wr<<"%\n";
    std::cout<<"Total PnL : $"<<std::setprecision(2)<<total<<"\n";
    std::cout<<"Avg win   : $"<<aw<<"\n";
    std::cout<<"Avg loss  : $"<<al<<"\n";
    std::cout<<"R:R       : "<<std::setprecision(2)<<rr<<"\n";
    std::cout<<"Exp/trade : $"<<exp_pt<<"\n";
    std::cout<<"Max DD    : $"<<maxdd<<"\n";
    std::cout<<"Exits:\n";
    for(auto&kv:reasons) if(kv.first!="ENTRY") std::cout<<"  "<<std::left<<std::setw(14)<<kv.first<<kv.second<<"\n";
    return 0;
}
