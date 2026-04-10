// candle_rsi_l2_sweep.cpp
// Sweeps all params against real L2 data
// Build: g++ -O3 -std=c++17 -o candle_rsi_l2_sweep candle_rsi_l2_sweep.cpp

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

struct L2Tick {
    int64_t ts_ms=0;
    double bid=0,ask=0,l2_imb=0.5,l2_bid_vol=0,l2_ask_vol=0;
    int depth_bid=0,depth_ask=0;
    bool watchdog_dead=false;
    double vol_ratio=1.0;
    int regime=0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double&v)->bool{ if(!std::getline(ss,tok,','))return false; try{v=std::stod(tok);}catch(...){return false;} return true; };
    auto ni=[&](int&v)->bool{ if(!std::getline(ss,tok,','))return false; try{v=(int)std::stoll(tok);}catch(...){return false;} return true; };
    double tmp; int wd=0;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid))return false;
    if(!nd(t.ask))return false;
    if(!nd(t.l2_imb))return false;
    if(!nd(t.l2_bid_vol))return false;
    if(!nd(t.l2_ask_vol))return false;
    if(!ni(t.depth_bid))return false;
    if(!ni(t.depth_ask))return false;
    if(!nd(tmp))return false;
    if(!ni(wd))return false; t.watchdog_dead=(wd!=0);
    if(!nd(t.vol_ratio))return false;
    if(!ni(t.regime))return false;
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

struct Params {
    int   rsi_period;
    int   rsi_ema;
    double rsi_thresh;
    double imb_thresh;
    int   imb_ticks;
    double body_ratio;
    double cost_mult;
    int64_t stag_ms;
};

struct Result {
    Params p;
    int trades,wins;
    double wr,total_pnl,avg_win,avg_loss,rr,exp_pt,maxdd;
};

Result run(const std::vector<L2Tick>& ticks, const Params& p) {
    M1Builder bars; ATR14 atr;
    bool in_t=false,is_long=false;
    double entry=0,sl=0,sz=0,cp=0,mfe=0;
    int64_t ets=0,cool=0,lbm=0;

    // RSI
    std::deque<double> gains,losses;
    double pm_rsi=0,rsi_cur=50,rsi_prev=50,rsi_trend=0;
    bool warmed=false;
    double ema_a=2.0/(p.rsi_ema+1);

    // DOM exit state
    int imb_against=0,prev_db=0,prev_da=0;
    double imb_ema=0; bool imb_first=true;

    int trades=0,wins=0;
    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;

    double pm=0;

    for (auto& t : ticks) {
        const double mid=(t.ask+t.bid)*0.5;
        const double sp=t.ask-t.bid;
        if(sp>0.30||sp<=0){pm=mid;continue;}

        // RSI
        if(pm_rsi!=0){
            double chg=mid-pm_rsi;
            gains.push_back(chg>0?chg:0); losses.push_back(chg<0?-chg:0);
            if((int)gains.size()>p.rsi_period){gains.pop_front();losses.pop_front();}
            if((int)gains.size()==p.rsi_period){
                double ag=0,al=0;
                for(auto x:gains)ag+=x; for(auto x:losses)al+=x;
                ag/=p.rsi_period; al/=p.rsi_period;
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

        if(t.ts_ms<cool){pm=mid;continue;}

        if(in_t){
            double move=is_long?(mid-entry):(entry-mid);
            if(move>mfe)mfe=move;

            // SL
            if(is_long?(t.bid<=sl):(t.ask>=sl)){
                double px=is_long?t.bid:t.ask;
                double pnl=(is_long?(px-entry):(entry-px))*sz*100.0;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;imb_against=0;imb_first=true;
                cool=t.ts_ms+10000; pm=mid; continue;
            }

            // DOM exit -- raw imbalance (no EMA), sustained N ticks
            double imb=(t.l2_imb-0.5)*2.0;
            bool against=is_long?(imb<-p.imb_thresh):(imb>p.imb_thresh);
            if(against) imb_against++;
            else        imb_against=0;

            // depth drop on support side
            bool depth_drop=is_long?(t.depth_bid<prev_db):(t.depth_ask<prev_da);
            bool dom_exit=(imb_against>=p.imb_ticks)||(imb_against>=2&&depth_drop);

            prev_db=t.depth_bid; prev_da=t.depth_ask;

            if(dom_exit){
                double px=is_long?t.bid:t.ask;
                double pnl=(is_long?(px-entry):(entry-px))*sz*100.0;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;imb_against=0;imb_first=true;
                cool=t.ts_ms+5000; pm=mid; continue;
            }

            // Stagnation
            int64_t held=t.ts_ms-ets;
            if(held>=p.stag_ms&&mfe<cp*1.0){
                double px=is_long?t.bid:t.ask;
                double pnl=(is_long?(px-entry):(entry-px))*sz*100.0;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;imb_against=0;imb_first=true;
                cool=t.ts_ms+10000; pm=mid; continue;
            }
            pm=mid; continue;
        }

        // Entry
        if(!bars.has2()||atr.val<=0||!warmed){pm=mid;continue;}
        int dir=(rsi_trend>p.rsi_thresh)?+1:(rsi_trend<-p.rsi_thresh)?-1:0;
        if(dir==0){pm=mid;continue;}

        const Bar&lb=bars.last; const Bar&pb=bars.prev;
        double range=lb.high-lb.low; if(range<=0){pm=mid;continue;}
        double bb=lb.close-lb.open,br=lb.open-lb.close;
        bool bull=(bb>0&&bb/range>=p.body_ratio&&lb.close>pb.high);
        bool bear=(br>0&&br/range>=p.body_ratio&&lb.close<pb.low);
        if(dir==+1&&!bull){pm=mid;continue;}
        if(dir==-1&&!bear){pm=mid;continue;}

        cp=sp+0.05*2+0.05*2;
        if(range<p.cost_mult*cp){pm=mid;continue;}

        is_long=(dir==+1);
        entry=is_long?t.ask:t.bid;
        double slp=atr.val>0?atr.val:1.0;
        sl=is_long?entry-slp:entry+slp;
        sz=std::max(0.01,std::min(0.50,30.0/(slp*100.0)));
        sz=std::floor(sz/0.001)*0.001;
        ets=t.ts_ms; mfe=0; in_t=true;
        imb_against=0; imb_first=true;
        prev_db=t.depth_bid; prev_da=t.depth_ask;
        pm=mid;
    }

    double wr=trades?100.0*wins/trades:0;
    double aw=wins?wpnl/wins:0;
    double al=(trades-wins)?lpnl/(trades-wins):0;
    double rr=al!=0?-aw/al:0;
    double exp_pt=trades?(wr/100.0*aw+(1.0-wr/100.0)*al):0;
    return {p,trades,wins,wr,total,aw,al,rr,exp_pt,maxdd};
}

int main(int argc,char* argv[]) {
    const char* fn=argc>1?argv[1]:"/mnt/user-data/uploads/1775794961248_l2_ticks_2026-04-10.csv";
    std::ifstream f(fn);
    if(!f){std::cerr<<"Cannot open: "<<fn<<"\n";return 1;}

    std::cerr<<"Loading...\n";
    std::vector<L2Tick> ticks;
    ticks.reserve(55000);
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        L2Tick t; if(parse_l2(line,t)&&!t.watchdog_dead)ticks.push_back(t);
    }
    std::cerr<<"Loaded "<<ticks.size()<<" ticks\n";

    // Sweep grid
    std::vector<int>    rsi_periods  = {5,10,20,30};
    std::vector<int>    rsi_emas     = {3,5,10};
    std::vector<double> rsi_threshs  = {0.5,1.0,2.0,4.0,6.0,8.0};
    std::vector<double> imb_threshs  = {0.03,0.05,0.07,0.10};
    std::vector<int>    imb_ticks_v  = {2,3,4,6};
    std::vector<double> body_ratios  = {0.50,0.60,0.70};
    std::vector<double> cost_mults   = {1.5,2.0,2.5};
    std::vector<int64_t> stag_ms_v   = {60000,120000};

    std::vector<Result> results;
    int total_runs=0;

    for(auto rp:rsi_periods)
    for(auto re:rsi_emas)
    for(auto rt:rsi_threshs)
    for(auto it:imb_threshs)
    for(auto iv:imb_ticks_v)
    for(auto br:body_ratios)
    for(auto cm:cost_mults)
    for(auto sm:stag_ms_v){
        Params p{rp,re,rt,it,iv,br,cm,sm};
        Result r=run(ticks,p);
        total_runs++;
        // Keep if: trades>=5 (sparse data), WR>=45%, positive expectancy
        if(r.trades>=5 && r.wr>=45.0 && r.exp_pt>0)
            results.push_back(r);
    }

    // Sort by expectancy desc
    std::sort(results.begin(),results.end(),[](const Result&a,const Result&b){
        return a.exp_pt>b.exp_pt;
    });

    std::cout<<"\n=== RSI+L2 DOM Sweep — trades>=5, WR>=45%, exp>0 ===\n";
    std::cout<<std::left
        <<std::setw(5)<<"RSI_P"<<std::setw(5)<<"EMA"<<std::setw(7)<<"RTHR"
        <<std::setw(6)<<"ITHR"<<std::setw(5)<<"ITK"<<std::setw(6)<<"BODY"
        <<std::setw(5)<<"CM"<<std::setw(7)<<"STAG"
        <<std::setw(5)<<"N"<<std::setw(7)<<"WR%"
        <<std::setw(10)<<"TOTAL"<<std::setw(7)<<"R:R"
        <<std::setw(9)<<"EXP/TR"<<std::setw(9)<<"MAXDD"<<"\n";
    std::cout<<std::string(100,'-')<<"\n";

    int shown=0;
    for(auto&r:results){
        if(shown++>=40)break;
        std::cout<<std::fixed
            <<std::setw(5)<<r.p.rsi_period
            <<std::setw(5)<<r.p.rsi_ema
            <<std::setw(7)<<std::setprecision(1)<<r.p.rsi_thresh
            <<std::setw(6)<<std::setprecision(2)<<r.p.imb_thresh
            <<std::setw(5)<<r.p.imb_ticks
            <<std::setw(6)<<std::setprecision(2)<<r.p.body_ratio
            <<std::setw(5)<<std::setprecision(1)<<r.p.cost_mult
            <<std::setw(7)<<(r.p.stag_ms/1000)<<"s"
            <<std::setw(5)<<r.trades
            <<std::setw(7)<<std::setprecision(1)<<r.wr
            <<std::setw(10)<<std::setprecision(2)<<r.total_pnl
            <<std::setw(7)<<std::setprecision(2)<<r.rr
            <<std::setw(9)<<std::setprecision(2)<<r.exp_pt
            <<std::setw(9)<<std::setprecision(2)<<r.maxdd<<"\n";
    }

    std::cout<<"\nTotal configs: "<<total_runs<<"\n";
    std::cout<<"Passing configs: "<<results.size()<<"\n";
    return 0;
}
