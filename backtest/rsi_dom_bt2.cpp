// rsi_dom_bt2.cpp  -- RSI slope + DOM using tick-computed RSI and ewm_drift
// RSI computed fresh from tick mid prices (same as RSIReversalEngine in Omega)
// ewm_drift from L2 CSV used as secondary confirmation
// Build: g++ -O2 -std=c++17 -o rsi_dom_bt2 rsi_dom_bt2.cpp

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

struct L2Tick {
    int64_t ts_ms=0;
    double bid=0,ask=0,l2_imb=0.5;
    double l2_bid_vol=0,l2_ask_vol=0;
    int depth_bid=0,depth_ask=0;
    uint64_t depth_events=0;
    bool watchdog_dead=false;
    double vol_ratio=1.0;
    int regime=0;
    double vpin=0;
    int has_pos=0;
    double micro_edge=0;
    double ewm_drift=0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v)->bool{if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    auto nu=[&](uint64_t& v)->bool{if(!getline(ss,tok,','))return false;try{v=(uint64_t)std::stoull(tok);}catch(...){return false;}return true;};
    auto nb=[&](bool& v)->bool{if(!getline(ss,tok,','))return false;try{v=(bool)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid)||!nd(t.ask))return false;
    if(!nd(t.l2_imb)||!nd(t.l2_bid_vol)||!nd(t.l2_ask_vol))return false;
    if(!ni(t.depth_bid)||!ni(t.depth_ask))return false;
    if(!nu(t.depth_events))return false;
    if(!nb(t.watchdog_dead))return false;
    if(!nd(t.vol_ratio))return false;
    if(!ni(t.regime))return false;
    if(getline(ss,tok,','))try{t.vpin=std::stod(tok);}catch(...){}
    if(getline(ss,tok,','))try{t.has_pos=(int)std::stoll(tok);}catch(...){}
    if(getline(ss,tok,','))try{t.micro_edge=std::stod(tok);}catch(...){}
    if(getline(ss,tok,','))try{t.ewm_drift=std::stod(tok);}catch(...){}
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid);
}

// RSI from tick prices
struct TickRSI {
    static constexpr int N=14;
    double avg_gain=0,avg_loss=0,prev=-1;
    int count=0;
    double val=50.0;
    bool warmed=false;

    void update(double price) {
        if(prev<0){prev=price;return;}
        double d=price-prev; prev=price;
        double g=d>0?d:0, l=d<0?-d:0;
        if(count<N){
            avg_gain+=g; avg_loss+=l; ++count;
            if(count==N){avg_gain/=N;avg_loss/=N;warmed=true;}
        } else {
            avg_gain=(avg_gain*(N-1)+g)/N;
            avg_loss=(avg_loss*(N-1)+l)/N;
            val=avg_loss<1e-10?100.0:100.0-100.0/(1.0+avg_gain/avg_loss);
        }
    }
};

// Slope detector on RSI
struct SlopeDetector {
    std::deque<double> hist;
    double ema=0; bool init=false;
    static constexpr double ALPHA=0.4; // faster response than 3-tick EMA

    // returns +1/-1/0
    int update(double v) {
        hist.push_back(v);
        if((int)hist.size()<2)return 0;
        while((int)hist.size()>4)hist.pop_front();
        int n=hist.size();
        double slope=hist[n-1]-hist[n-2];
        if(!init){ema=slope;init=true;return 0;}
        double prev_ema=ema;
        ema=ALPHA*slope+(1-ALPHA)*ema;
        if(prev_ema<=0&&ema>0) return +1;
        if(prev_ema>=0&&ema<0) return -1;
        return 0;
    }
    double slope()const{return ema;}
};

// ATR
struct ATR {
    std::deque<double>r; double val=0;
    void add(double x){r.push_back(x);if((int)r.size()>14)r.pop_front();double s=0;for(auto v:r)s+=v;val=s/r.size();}
};
// M1 bar
struct M1B{
    double o=0,h=0,l=0,c=0; int64_t bm=-1; bool valid=false;
    double last_h=0,last_l=0,last_range=0;
    void update(int64_t ts,double mid){
        int64_t b=ts/60000;
        if(b!=bm){
            if(valid){last_h=h;last_l=l;last_range=h-l;}
            o=h=l=c=mid;bm=b;valid=true;
        } else {if(mid>h)h=mid;if(mid<l)l=mid;c=mid;}
    }
};

int dom_score(bool il,const L2Tick&t,const L2Tick&p){
    int s=0;
    if(il){
        if(t.depth_bid>t.depth_ask)++s;
        if(t.depth_ask<p.depth_ask&&p.depth_ask>0)++s;
        if(t.depth_ask<=1)++s;
        if(t.depth_bid>p.depth_bid)++s;
    }else{
        if(t.depth_ask>t.depth_bid)++s;
        if(t.depth_bid<p.depth_bid&&p.depth_bid>0)++s;
        if(t.depth_bid<=1)++s;
        if(t.depth_ask>p.depth_ask)++s;
    }
    return s;
}

struct Trade{bool il;double entry,exit_px,pnl;int held_s;std::string reason;double rsi_entry;};

struct Result{int n,w;double pnl,maxdd,aw,al;};

Result run(const std::vector<std::string>& files,
           double slope_min, int dom_min, double sl_atr, bool use_drift_filter) {
    TickRSI rsi; SlopeDetector sd; ATR atr; M1B bars;
    std::vector<Trade> trades;
    bool in_trade=false,il=false;
    double entry=0,sl=0,size=0,mfe=0;
    int64_t entry_ts=0,last_bm=0;
    L2Tick prev;
    static constexpr double RISK=30,TV=100,COST=0.8; // typical cost

    for(auto& fname:files){
        std::ifstream f(fname);if(!f)continue;
        std::string line;getline(f,line);
        while(getline(f,line)){
            L2Tick t;if(!parse_l2(line,t))continue;
            if(t.watchdog_dead){prev=t;continue;}
            double mid=(t.bid+t.ask)*0.5;
            double spread=t.ask-t.bid;
            if(spread>0.5||spread<=0){prev=t;continue;}

            bars.update(t.ts_ms,mid);
            int64_t bm=t.ts_ms/60000;
            if(bm!=last_bm&&bars.valid&&bars.last_range>0){atr.add(bars.last_range);last_bm=bm;}

            rsi.update(mid);
            if(!rsi.warmed){prev=t;continue;}
            int infl=sd.update(rsi.val);

            if(in_trade){
                double move=il?(mid-entry):(entry-mid);
                if(move>mfe)mfe=move;
                if(il?(t.bid<=sl):(t.ask>=sl)){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL_HIT",rsi.val});
                    in_trade=false;prev=t;continue;
                }
                // Exit on RSI slope reversal
                if((il&&infl==-1)||(!il&&infl==+1)){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"RSI_FLIP",rsi.val});
                    in_trade=false;prev=t;continue;
                }
                prev=t;continue;
            }

            if(!infl||!atr.val||atr.val<COST*1.5){prev=t;continue;}
            bool sig_long=(infl==+1);

            // Slope magnitude check
            if(std::fabs(sd.slope())<slope_min){prev=t;continue;}

            // DOM check
            if(dom_score(sig_long,t,prev)<dom_min){prev=t;continue;}

            // Optional: ewm_drift alignment
            if(use_drift_filter){
                if(sig_long&&t.ewm_drift<-0.5){prev=t;continue;}  // drift against long
                if(!sig_long&&t.ewm_drift>0.5){prev=t;continue;}  // drift against short
            }

            // Enter
            il=sig_long;
            entry=il?t.ask:t.bid;
            double sl_pts=atr.val*sl_atr;
            sl=il?entry-sl_pts:entry+sl_pts;
            size=std::max(0.01,std::min(0.50,RISK/(sl_pts*TV)));
            size=std::floor(size/0.001)*0.001;
            entry_ts=t.ts_ms;mfe=0;in_trade=true;
            prev=t;continue;
        }
    }

    if(trades.empty())return{0,0,0,0,0,0};
    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for(auto&tr:trades){
        total+=tr.pnl;eq+=tr.pnl;
        if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl>0){w++;wpnl+=tr.pnl;}else{l++;lpnl+=tr.pnl;}
    }
    int n=trades.size();
    return{n,w,total,maxdd,w?wpnl/w:0,l?lpnl/l:0};
}

int main(int argc,char* argv[]){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++)files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: rsi_dom_bt2 files...\n";return 1;}

    std::cout<<std::fixed<<std::setprecision(1);
    std::cout<<"\n=== RSI Slope Turn + DOM Entry/Exit Backtest ===\n";
    std::cout<<std::setw(8)<<"Slope"<<std::setw(6)<<"DOM"<<std::setw(7)<<"SL"
             <<std::setw(7)<<"Drift"<<std::setw(8)<<"Trades"<<std::setw(7)<<"WR%"
             <<std::setw(10)<<"PnL"<<std::setw(10)<<"MaxDD"<<std::setw(7)<<"R:R"
             <<std::setw(9)<<"AvgW"<<std::setw(9)<<"AvgL\n";
    std::cout<<std::string(81,'-')<<"\n";

    for(double slope:{0.05,0.10,0.20,0.30})
    for(int dom:{1,2})
    for(double sl:{0.5,1.0,1.5})
    for(bool drift:{false,true}){
        auto r=run(files,slope,dom,sl,drift);
        if(r.n==0)continue;
        double wr=100.0*r.w/r.n;
        double rr=r.al!=0?-r.aw/r.al:0;
        std::cout<<std::setw(8)<<slope<<std::setw(6)<<dom<<std::setw(7)<<sl
                 <<std::setw(7)<<(drift?"Y":"N")<<std::setw(8)<<r.n
                 <<std::setw(7)<<wr<<std::setw(10)<<r.pnl
                 <<std::setw(10)<<r.maxdd<<std::setw(7)<<rr
                 <<std::setw(9)<<r.aw<<std::setw(9)<<r.al<<"\n";
    }
    return 0;
}
