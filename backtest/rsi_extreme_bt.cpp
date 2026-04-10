// rsi_extreme_bt.cpp
// Entry: RSI at extreme level (< LOW or > HIGH) AND RSI turning
// No DOM requirement -- data shows DOM adds noise not signal
// Exit: RSI crosses back through mid-level OR hard SL
// Build: g++ -O2 -std=c++17 -o rsi_extreme_bt rsi_extreme_bt.cpp

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
    bool watchdog_dead=false;
    double ewm_drift=0;
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v)->bool{if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp,skip; int iskip; bool b; uint64_t u;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid)||!nd(t.ask))return false;
    if(!nd(skip)||!nd(skip)||!nd(skip))return false;
    if(!ni(t.depth_bid)||!ni(t.depth_ask))return false;
    if(!getline(ss,tok,','))return false; // depth_events
    if(!getline(ss,tok,','))return false; try{t.watchdog_dead=(bool)std::stoll(tok);}catch(...){}
    for(int i=0;i<5;i++) getline(ss,tok,','); // vol_ratio,regime,vpin,has_pos,micro_edge
    if(getline(ss,tok,','))try{t.ewm_drift=std::stod(tok);}catch(...){}
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid&&(t.ask-t.bid)<0.5);
}

struct TickRSI {
    static constexpr int N=14;
    double ag=0,al=0,prev=-1,val=50.0; int c=0; bool w=false;
    void update(double p){
        if(prev<0){prev=p;return;}
        double d=p-prev; prev=p;
        double g=d>0?d:0,l=d<0?-d:0;
        if(c<N){ag+=g;al+=l;++c;if(c==N){ag/=N;al/=N;w=true;}}
        else{ag=(ag*(N-1)+g)/N;al=(al*(N-1)+l)/N;
             val=al<1e-10?100.0:100.0-100.0/(1.0+ag/al);}
    }
};

// Samples RSI at M1 bar boundaries, detects sustained move then turn
struct BarRSIDetector {
    std::deque<double> hist; // RSI at each bar close
    int64_t last_bm=-1;
    double  last_rsi=50;
    int consec_fall=0, consec_rise=0;

    // returns +1 (turn up), -1 (turn down), 0
    int update(double rsi, int64_t ts_ms, int min_bars) {
        int64_t bm=ts_ms/60000;
        if(bm!=last_bm && last_bm>=0) {
            if(!hist.empty()) {
                double prev=hist.back();
                if(last_rsi < prev-0.3){consec_fall++;consec_rise=0;}
                else if(last_rsi > prev+0.3){consec_rise++;consec_fall=0;}
            }
            hist.push_back(last_rsi);
            while((int)hist.size()>10) hist.pop_front();
            last_bm=bm;
        }
        if(last_bm<0) last_bm=bm;
        last_rsi=rsi;

        int n=hist.size();
        if(n<2) return 0;

        if(consec_fall>=min_bars && rsi>hist.back()+0.5) {
            consec_fall=0; return +1;
        }
        if(consec_rise>=min_bars && rsi<hist.back()-0.5) {
            consec_rise=0; return -1;
        }
        return 0;
    }
};

struct ATR {
    std::deque<double>r; double val=0;
    void add(double x){r.push_back(x);if((int)r.size()>14)r.pop_front();
        double s=0;for(auto v:r)s+=v;val=s/r.size();}
};
struct M1B {
    double h=0,l=0; int64_t bm=-1; bool valid=false; double lr=0;
    void update(int64_t ts,double m){
        int64_t b=ts/60000;
        if(b!=bm){if(valid)lr=h-l;h=l=m;bm=b;valid=true;}
        else{if(m>h)h=m;if(m<l)l=m;}
    }
};

struct Trade{bool il;double entry,exit_px,pnl;int held_s;std::string reason;double rsi_e;};
struct Result{int n,w;double pnl,maxdd,aw,al;};

Result run(const std::vector<std::string>& files,
           int min_bars, double rsi_low, double rsi_high,
           double exit_rsi, double sl_atr) {
    TickRSI rsi; BarRSIDetector det; ATR atr; M1B bars;
    bool in_trade=false,il=false;
    double entry=0,sl=0,size=0;
    int64_t entry_ts=0,last_bm=0;
    L2Tick prev{};
    std::vector<Trade> trades;
    static constexpr double RISK=30,TV=100;

    for(auto& fname:files){
        std::ifstream f(fname);if(!f)continue;
        std::string line;getline(f,line);
        while(getline(f,line)){
            L2Tick t;if(!parse_l2(line,t))continue;
            if(t.watchdog_dead){prev=t;continue;}
            double mid=(t.bid+t.ask)*0.5;
            double spread=t.ask-t.bid;

            bars.update(t.ts_ms,mid);
            int64_t bm=t.ts_ms/60000;
            if(bm!=last_bm&&bars.valid&&bars.lr>0){atr.add(bars.lr);last_bm=bm;}

            rsi.update(mid);
            if(!rsi.w){prev=t;continue;}

            int signal=det.update(rsi.val,t.ts_ms,min_bars);

            if(in_trade){
                double move=il?(mid-entry):(entry-mid);
                // Hard SL
                if(il?(t.bid<=sl):(t.ask>=sl)){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"SL",rsi.val});
                    in_trade=false;prev=t;continue;
                }
                // Exit: RSI crosses exit level
                bool rsi_exit = il ? (rsi.val >= exit_rsi) : (rsi.val <= (100.0-exit_rsi));
                if(rsi_exit){
                    double px=il?t.bid:t.ask;
                    double pnl=(il?(px-entry):(entry-px))*size*TV;
                    trades.push_back({il,entry,px,pnl,(int)((t.ts_ms-entry_ts)/1000),"RSI_EXIT",rsi.val});
                    in_trade=false;prev=t;continue;
                }
                prev=t;continue;
            }

            if(!signal||!atr.val){prev=t;continue;}
            bool sig_long=(signal==+1);

            // RSI must be at extreme when turn fires
            bool rsi_extreme = sig_long  ? (rsi.val < rsi_low)
                                         : (rsi.val > rsi_high);
            if(!rsi_extreme){prev=t;continue;}

            // Cost gate: ATR must cover spread+cost
            double cost=spread+0.4;
            if(atr.val<cost*1.5){prev=t;continue;}

            il=sig_long;
            entry=il?t.ask:t.bid;
            double sl_pts=atr.val*sl_atr;
            if(sl_pts<0.5)sl_pts=0.5;
            sl=il?entry-sl_pts:entry+sl_pts;
            size=std::max(0.01,std::min(0.50,RISK/(sl_pts*TV)));
            size=std::floor(size/0.001)*0.001;
            entry_ts=t.ts_ms;in_trade=true;
            prev=t;continue;
        }
    }
    if(trades.empty())return{0,0,0,0,0,0};
    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    int w=0,l=0;
    for(auto& tr:trades){
        total+=tr.pnl;eq+=tr.pnl;
        if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
        if(tr.pnl>0){w++;wpnl+=tr.pnl;}else{l++;lpnl+=tr.pnl;}
    }
    return{(int)trades.size(),w,total,maxdd,w?wpnl/w:0,l?lpnl/l:0};
}

int main(int argc,char* argv[]){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: rsi_extreme_bt files...\n";return 1;}

    std::cout<<std::fixed<<std::setprecision(1);
    std::cout<<"\n=== RSI Extreme + Turn Backtest (no DOM) ===\n";
    std::cout<<std::setw(8)<<"MinBrs"<<std::setw(7)<<"Low"<<std::setw(7)<<"High"
             <<std::setw(8)<<"Exit"<<std::setw(7)<<"SL"
             <<std::setw(8)<<"Trades"<<std::setw(7)<<"WR%"
             <<std::setw(10)<<"PnL"<<std::setw(10)<<"MaxDD"
             <<std::setw(7)<<"R:R"<<std::setw(9)<<"AvgW"<<std::setw(9)<<"AvgL\n";
    std::cout<<std::string(90,'-')<<"\n";

    for(int mb:{2,3})
    for(double lo:{20,25,30})
    for(double hi:{70,75,80})
    for(double ex:{45,50,55})
    for(double sl:{0.5,1.0,1.5}){
        auto r=run(files,mb,lo,hi,ex,sl);
        if(r.n<3)continue;
        double wr=100.0*r.w/r.n;
        double rr=r.al!=0?-r.aw/r.al:0;
        std::cout<<std::setw(8)<<mb<<std::setw(7)<<lo<<std::setw(7)<<hi
                 <<std::setw(8)<<ex<<std::setw(7)<<sl
                 <<std::setw(8)<<r.n<<std::setw(7)<<wr
                 <<std::setw(10)<<r.pnl<<std::setw(10)<<r.maxdd
                 <<std::setw(7)<<rr<<std::setw(9)<<r.aw<<std::setw(9)<<r.al<<"\n";
    }
    return 0;
}
