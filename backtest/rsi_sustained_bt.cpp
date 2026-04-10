// rsi_sustained_bt.cpp
// Entry: RSI must have been falling/rising for N bars minimum, THEN turns
// This catches the genuine inflection points visible on the chart
// not every 1-tick noise wiggle
// Build: g++ -O2 -std=c++17 -o rsi_sustained_bt rsi_sustained_bt.cpp

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
    double bid=0,ask=0;
    int depth_bid=0,depth_ask=0;
    uint64_t depth_events=0;
    bool watchdog_dead=false;
    double ewm_drift=0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v)->bool{if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    auto nu=[&](uint64_t& v)->bool{if(!getline(ss,tok,','))return false;try{v=(uint64_t)std::stoull(tok);}catch(...){return false;}return true;};
    auto nb=[&](bool& v)->bool{if(!getline(ss,tok,','))return false;try{v=(bool)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp,skip;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid)||!nd(t.ask))return false;
    // skip l2_imb,l2_bid_vol,l2_ask_vol
    if(!nd(skip)||!nd(skip)||!nd(skip))return false;
    if(!ni(t.depth_bid)||!ni(t.depth_ask))return false;
    if(!nu(t.depth_events))return false;
    bool wd; if(!nb(wd))return false; t.watchdog_dead=wd;
    // skip vol_ratio,regime,vpin,has_pos,micro_edge
    for(int i=0;i<5;i++) getline(ss,tok,',');
    // ewm_drift is last column
    if(getline(ss,tok,','))try{t.ewm_drift=std::stod(tok);}catch(...){}
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid);
}

// RSI from tick mid prices
struct TickRSI {
    static constexpr int N=14;
    double avg_gain=0,avg_loss=0,prev=-1,val=50.0;
    int count=0; bool warmed=false;
    void update(double p){
        if(prev<0){prev=p;return;}
        double d=p-prev; prev=p;
        double g=d>0?d:0,l=d<0?-d:0;
        if(count<N){avg_gain+=g;avg_loss+=l;++count;
            if(count==N){avg_gain/=N;avg_loss/=N;warmed=true;}
        } else {
            avg_gain=(avg_gain*(N-1)+g)/N;
            avg_loss=(avg_loss*(N-1)+l)/N;
            val=avg_loss<1e-10?100.0:100.0-100.0/(1.0+avg_gain/avg_loss);
        }
    }
};

// Sustained move detector
// Tracks RSI over M1 bars (one RSI sample per bar close)
// Only signals when RSI has moved in one direction for >= MIN_BARS bars
// then reverses
struct SustainedRSI {
    // We sample RSI once per M1 bar (at bar close) to match chart timeframe
    std::deque<double> bar_rsi;  // RSI at each M1 bar close
    int64_t last_bar_min = -1;
    double  last_bar_rsi = 50.0;

    // How many consecutive bars RSI has been falling/rising
    int consec_falling = 0;
    int consec_rising  = 0;

    // Returns +1 (turn up after sustained fall), -1 (turn down after sustained rise), 0
    int update(double rsi_val, int64_t ts_ms, int min_bars) {
        int64_t bar_min = ts_ms / 60000;

        if (bar_min != last_bar_min) {
            // New bar -- record RSI at bar close
            if (last_bar_min >= 0 && !bar_rsi.empty()) {
                double prev_rsi = bar_rsi.back();
                double curr_rsi = last_bar_rsi;  // RSI at end of previous bar

                if (curr_rsi < prev_rsi - 0.3) {
                    // RSI fell this bar
                    consec_falling++;
                    consec_rising = 0;
                } else if (curr_rsi > prev_rsi + 0.3) {
                    // RSI rose this bar
                    consec_rising++;
                    consec_falling = 0;
                }
                // flat -- don't reset counters, just don't increment
            }
            bar_rsi.push_back(rsi_val);
            while ((int)bar_rsi.size() > 20) bar_rsi.pop_front();
            last_bar_min = bar_min;
        }
        last_bar_rsi = rsi_val;

        // Check for turn: was falling for min_bars, now rising for 1+ bar
        // "Rising" = RSI this bar higher than RSI last bar
        if (consec_falling >= min_bars) {
            int n = bar_rsi.size();
            if (n >= 2 && bar_rsi[n-1] > bar_rsi[n-2] + 0.5) {
                consec_falling = 0;  // reset
                return +1;  // turn up after sustained fall
            }
        }
        if (consec_rising >= min_bars) {
            int n = bar_rsi.size();
            if (n >= 2 && bar_rsi[n-1] < bar_rsi[n-2] - 0.5) {
                consec_rising = 0;  // reset
                return -1;  // turn down after sustained rise
            }
        }
        return 0;
    }

    double last_rsi() const { return last_bar_rsi; }
};

// ATR from M1 bars
struct ATR {
    std::deque<double> r; double val=0;
    void add(double x){
        r.push_back(x);
        if((int)r.size()>14)r.pop_front();
        double s=0;for(auto v:r)s+=v;val=s/r.size();
    }
};

struct M1B {
    double h=0,l=0; int64_t bm=-1; bool valid=false;
    double last_range=0;
    void update(int64_t ts,double mid){
        int64_t b=ts/60000;
        if(b!=bm){
            if(valid) last_range=h-l;
            h=l=mid;bm=b;valid=true;
        } else {if(mid>h)h=mid;if(mid<l)l=mid;}
    }
};

int dom_score(bool il, const L2Tick& t, const L2Tick& p) {
    int s=0;
    if(il){
        if(t.depth_bid>t.depth_ask)++s;
        if(t.depth_ask<p.depth_ask&&p.depth_ask>0)++s;
        if(t.depth_ask<=1)++s;
        if(t.depth_bid>p.depth_bid)++s;
    } else {
        if(t.depth_ask>t.depth_bid)++s;
        if(t.depth_bid<p.depth_bid&&p.depth_bid>0)++s;
        if(t.depth_bid<=1)++s;
        if(t.depth_ask>p.depth_ask)++s;
    }
    return s;
}

struct Trade{bool il;double entry,exit_px,pnl;int held_s;std::string reason;double rsi_e;};

struct Result{int n,w;double pnl,maxdd,aw,al;};

Result run(const std::vector<std::string>& files,
           int min_bars, int dom_min, double sl_atr) {
    TickRSI rsi; SustainedRSI srsi; ATR atr; M1B bars;
    std::vector<Trade> trades;
    bool in_trade=false,il=false;
    double entry=0,sl=0,size=0;
    int64_t entry_ts=0,last_bm=0;
    int signal_cooldown=0;
    L2Tick prev;
    static constexpr double RISK=30,TV=100;

    for(auto& fname:files){
        std::ifstream f(fname); if(!f)continue;
        std::string line; getline(f,line);
        while(getline(f,line)){
            L2Tick t; if(!parse_l2(line,t))continue;
            if(t.watchdog_dead){prev=t;continue;}
            double mid=(t.bid+t.ask)*0.5;
            double spread=t.ask-t.bid;
            if(spread>0.5||spread<=0){prev=t;continue;}

            bars.update(t.ts_ms,mid);
            int64_t bm=t.ts_ms/60000;
            if(bm!=last_bm&&bars.valid&&bars.last_range>0){
                atr.add(bars.last_range);last_bm=bm;
            }

            rsi.update(mid);
            if(!rsi.warmed){prev=t;continue;}

            int signal = srsi.update(rsi.val, t.ts_ms, min_bars);
            if(signal_cooldown>0) { --signal_cooldown; signal=0; }

            if(in_trade){
                double move=il?(mid-entry):(entry-mid);
                // SL
                if(il?(t.bid<=sl):(t.ask>=sl)){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL",rsi.val});
                    in_trade=false; signal_cooldown=5; prev=t; continue;
                }
                // Exit: RSI turns against position (sustained reversal)
                if((il&&signal==-1)||(!il&&signal==+1)){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"RSI_TURN",rsi.val});
                    in_trade=false; signal_cooldown=3; prev=t; continue;
                }
                prev=t; continue;
            }

            if(!signal||!atr.val||atr.val<0.5){prev=t;continue;}
            bool sig_long=(signal==+1);

            // DOM confirmation
            if(dom_score(sig_long,t,prev)<dom_min){prev=t;continue;}

            // Enter
            il=sig_long;
            entry=il?t.ask:t.bid;
            double sl_pts=atr.val*sl_atr;
            if(sl_pts<0.5)sl_pts=0.5;
            sl=il?entry-sl_pts:entry+sl_pts;
            size=std::max(0.01,std::min(0.50,RISK/(sl_pts*TV)));
            size=std::floor(size/0.001)*0.001;
            entry_ts=t.ts_ms; in_trade=true;
            prev=t; continue;
        }
    }

    if(trades.empty())return{0,0,0,0,0,0};
    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for(auto& tr:trades){
        total+=tr.pnl; eq+=tr.pnl;
        if(eq>peak)peak=eq; maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl>0){w++;wpnl+=tr.pnl;}else{l++;lpnl+=tr.pnl;}
    }
    return{(int)trades.size(),w,total,maxdd,w?wpnl/w:0,l?lpnl/l:0};
}

int main(int argc, char* argv[]){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: rsi_sustained_bt files...\n";return 1;}

    std::cout<<std::fixed<<std::setprecision(1);
    std::cout<<"\n=== Sustained RSI Turn + DOM Backtest ===\n";
    std::cout<<std::setw(10)<<"MinBars"<<std::setw(6)<<"DOM"<<std::setw(7)<<"SL"
             <<std::setw(8)<<"Trades"<<std::setw(7)<<"WR%"
             <<std::setw(10)<<"PnL"<<std::setw(10)<<"MaxDD"
             <<std::setw(7)<<"R:R"<<std::setw(9)<<"AvgW"<<std::setw(9)<<"AvgL\n";
    std::cout<<std::string(81,'-')<<"\n";

    for(int mb:{2,3,4,5,6,8})
    for(int dom:{1,2})
    for(double sl:{0.5,1.0,1.5,2.0}){
        auto r=run(files,mb,dom,sl);
        if(r.n==0){
            std::cout<<std::setw(10)<<mb<<std::setw(6)<<dom<<std::setw(7)<<sl
                     <<" -- no trades\n"; continue;
        }
        double wr=100.0*r.w/r.n;
        double rr=r.al!=0?-r.aw/r.al:0;
        std::cout<<std::setw(10)<<mb<<std::setw(6)<<dom<<std::setw(7)<<sl
                 <<std::setw(8)<<r.n<<std::setw(7)<<wr
                 <<std::setw(10)<<r.pnl<<std::setw(10)<<r.maxdd
                 <<std::setw(7)<<rr<<std::setw(9)<<r.aw<<std::setw(9)<<r.al<<"\n";
    }
    return 0;
}
