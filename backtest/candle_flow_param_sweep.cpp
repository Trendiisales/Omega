// candle_flow_param_sweep.cpp
// 2D sweep: stagnation_ms x dom_entry_min
// Also sweeps cost_mult to find the tightest filter that still generates enough trades
// Build: g++ -O2 -std=c++17 -o candle_flow_param_sweep candle_flow_param_sweep.cpp
// Run:   ./candle_flow_param_sweep l2_ticks_2026-04-09.csv l2_ticks_2026-04-10.csv

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
    int64_t ts_ms=0; double bid=0,ask=0,l2_imb=0.5;
    double l2_bid_vol=0,l2_ask_vol=0;
    int depth_bid=0,depth_ask=0;
    bool watchdog_dead=false;
};
bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v){if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v){if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    auto nb=[&](bool& v){if(!getline(ss,tok,','))return false;try{v=(bool)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp; if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid)||!nd(t.ask))return false;
    if(!nd(t.l2_imb)||!nd(t.l2_bid_vol)||!nd(t.l2_ask_vol))return false;
    if(!ni(t.depth_bid)||!ni(t.depth_ask))return false;
    if(!nd(tmp))return false;
    if(!nb(t.watchdog_dead))return false;
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid);
}
struct Bar{double open=0,high=0,low=0,close=0;int64_t ts_open=0;bool valid=false;};
struct M1B{
    Bar cur,last,prev; int64_t cm=0;
    void update(int64_t ts,double mid){
        int64_t bm=ts/60000;
        if(bm!=cm){if(cur.valid){prev=last;last=cur;}cur={mid,mid,mid,mid,ts,true};cm=bm;}
        else{if(mid>cur.high)cur.high=mid;if(mid<cur.low)cur.low=mid;cur.close=mid;}
    }
    bool has2()const{return last.valid&&prev.valid;}
};
struct ATR{std::deque<double>r;double val=0;
    void add(const Bar&b){r.push_back(b.high-b.low);if((int)r.size()>14)r.pop_front();double s=0;for(auto x:r)s+=x;val=s/r.size();}
};
struct DOM{
    int bc=0,ac=0,pbc=0,pac=0;double bv=0,av=0,pbv=0,pav=0;
    bool va=false,vb=false,wa=false,wb=false;
    void update(const L2Tick&t,const L2Tick&p){
        pbc=p.depth_bid;pac=p.depth_ask;pbv=p.l2_bid_vol;pav=p.l2_ask_vol;
        bc=t.depth_bid;ac=t.depth_ask;bv=t.l2_bid_vol;av=t.l2_ask_vol;
        va=(ac<=1);vb=(bc<=1);wa=(ac>=5&&bc<=2);wb=(bc>=5&&ac<=2);
    }
    int entry(bool il, int min_score)const{
        int s=0;
        if(il){if(bc>ac)s++;if(ac<pac&&pac>0)s++;if(va)s++;if(bc>pbc)s++;}
        else{if(ac>bc)s++;if(bc<pbc&&pbc>0)s++;if(vb)s++;if(ac>pac)s++;}
        return s;
    }
    int exit_score(bool il)const{int s=0;
        if(il){if(bv<pbv*0.7||av>pav*1.3)s++;if(wa)s++;if(bc<pbc&&ac>pac)s++;if(vb&&!va)s++;}
        else{if(av<pav*0.7||bv>pbv*1.3)s++;if(wb)s++;if(ac<pac&&bc>pbc)s++;if(va&&!vb)s++;}
        return s;}
};

struct Result{int n,w;double pnl,maxdd,avg_win,avg_loss;};

Result run(const std::vector<std::string>& files,
           int64_t stag_ms, int dom_entry_min, double cost_mult) {
    M1B bars; ATR atr; DOM dom;
    bool in_trade=false,is_long=false;
    double entry=0,sl=0,size=0,cost_pts=0,mfe=0;
    int64_t entry_ts=0,last_bmin=0;
    int w=0,n=0; double pnl=0,peak=0,eq=0,maxdd=0;
    double wpnl=0,lpnl=0; int wn=0,ln=0;
    L2Tick prev;
    static constexpr double BODY=0.60,CSLIP=0.10,CCOM=0.10,MAXSP=0.50,RISK=30.0,TV=100.0;
    static constexpr double STAG_MULT=1.5;
    for(auto& fname:files){
        std::ifstream f(fname); if(!f)continue;
        std::string line; getline(f,line);
        while(getline(f,line)){
            L2Tick t; if(!parse_l2(line,t))continue;
            if(t.watchdog_dead){prev=t;continue;}
            double mid=(t.ask+t.bid)*0.5,sp=t.ask-t.bid;
            if(sp>MAXSP||sp<=0){prev=t;continue;}
            dom.update(t,prev); prev=t;
            bars.update(t.ts_ms,mid);
            int64_t bm=t.ts_ms/60000;
            if(bm!=last_bmin&&bars.last.valid){atr.add(bars.last);last_bmin=bm;}
            if(in_trade){
                double move=is_long?(mid-entry):(entry-mid);
                if(move>mfe)mfe=move;
                bool sl_hit = is_long?(t.bid<=sl):(t.ask>=sl);
                bool dom_rev = dom.exit_score(is_long)>=2;
                bool stag = (t.ts_ms-entry_ts)>=stag_ms && mfe<cost_pts*STAG_MULT;
                if(sl_hit||dom_rev||stag){
                    double px=is_long?t.bid:t.ask;
                    double p=(is_long?(px-entry):(entry-px))*size*TV;
                    pnl+=p;eq+=p;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                    if(p>0){w++;wn++;wpnl+=p;}else{ln++;lpnl+=p;}
                    n++;in_trade=false;
                }
                continue;
            }
            if(!bars.has2()||atr.val<=0)continue;
            const Bar&lb=bars.last;const Bar&pb=bars.prev;
            double range=lb.high-lb.low;if(range<=0)continue;
            double bb=lb.close-lb.open,bbar=lb.open-lb.close;
            bool bull=(bb>0&&bb/range>=BODY&&lb.close>pb.high);
            bool bear=(bbar>0&&bbar/range>=BODY&&lb.close<pb.low);
            if(!bull&&!bear)continue;
            cost_pts=sp+CSLIP*2+CCOM*2;
            if(range<cost_mult*cost_pts)continue;
            if(dom.entry(bull,dom_entry_min)<dom_entry_min)continue;
            is_long=bull;entry=is_long?t.ask:t.bid;
            double slp=atr.val>0?atr.val:5.0;
            sl=is_long?entry-slp:entry+slp;
            size=std::max(0.01,std::min(0.50,RISK/(slp*TV)));
            size=std::floor(size/0.001)*0.001;
            entry_ts=t.ts_ms;mfe=0;in_trade=true;
        }
    }
    double aw=wn?wpnl/wn:0, al=ln?lpnl/ln:0;
    return {n,w,pnl,maxdd,aw,al};
}

int main(int argc,char* argv[]){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: candle_flow_param_sweep files...\n";return 1;}

    // Section 1: stagnation x DOM entry min (cost_mult=2.0 fixed)
    std::cout << "\n=== Sweep: Stagnation(ms) x DOM_entry_min [cost_mult=2.0] ===\n";
    std::cout << std::setw(10)<<"Stag(ms)"<<std::setw(8)<<"DOM_min"
              <<std::setw(8)<<"Trades"<<std::setw(8)<<"WR%"
              <<std::setw(10)<<"PnL"<<std::setw(10)<<"MaxDD"
              <<std::setw(10)<<"AvgWin"<<std::setw(10)<<"AvgLoss"
              <<std::setw(8)<<"R:R\n";
    std::cout << std::string(82,'-') << "\n";
    std::cout << std::fixed << std::setprecision(1);

    for(int64_t ms : {30000,60000,90000,120000,180000}) {
        for(int dom : {1,2,3}) {
            auto r=run(files,ms,dom,2.0);
            double wr=r.n?100.0*r.w/r.n:0;
            double rr=r.avg_loss!=0?-r.avg_win/r.avg_loss:0;
            std::cout<<std::setw(10)<<ms<<std::setw(8)<<dom
                     <<std::setw(8)<<r.n<<std::setw(8)<<wr
                     <<std::setw(10)<<r.pnl<<std::setw(10)<<r.maxdd
                     <<std::setw(10)<<r.avg_win<<std::setw(10)<<r.avg_loss
                     <<std::setw(8)<<rr<<"\n";
        }
    }

    // Section 2: cost_mult sweep at best stagnation (60s, dom=2)
    std::cout << "\n=== Sweep: cost_mult [stag=60s, dom_min=2] ===\n";
    std::cout << std::setw(12)<<"CostMult"<<std::setw(8)<<"Trades"
              <<std::setw(8)<<"WR%"<<std::setw(10)<<"PnL"
              <<std::setw(10)<<"MaxDD"<<std::setw(8)<<"R:R\n";
    std::cout << std::string(56,'-') << "\n";

    for(double cm : {1.5,2.0,2.5,3.0,3.5,4.0}) {
        auto r=run(files,60000,2,cm);
        double wr=r.n?100.0*r.w/r.n:0;
        double rr=r.avg_loss!=0?-r.avg_win/r.avg_loss:0;
        std::cout<<std::setw(12)<<cm<<std::setw(8)<<r.n
                 <<std::setw(8)<<wr<<std::setw(10)<<r.pnl
                 <<std::setw(10)<<r.maxdd<<std::setw(8)<<rr<<"\n";
    }

    return 0;
}
