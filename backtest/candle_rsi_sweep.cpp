// candle_rsi_sweep.cpp
// Sweeps RSI threshold, RSI period, slope EMA window, and DOM smoothing
// to find configs where win rate >= 45% and trade count >= 50
//
// Build: g++ -O2 -std=c++17 -o candle_rsi_sweep candle_rsi_sweep.cpp
// Run:   ./candle_rsi_sweep ~/tick/xauusd_merged_24months.csv

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

static constexpr double CFG_BODY_RATIO_MIN  = 0.60;
static constexpr double CFG_COST_SLIP       = 0.10;
static constexpr double CFG_COST_COMM       = 0.10;
static constexpr double CFG_COST_MULT       = 2.0;
static constexpr double CFG_MAX_SPREAD      = 0.50;
static constexpr int64_t CFG_STAGNATION_MS  = 60000;
static constexpr double CFG_STAGNATION_MULT = 1.5;
static constexpr double CFG_RISK_USD        = 30.0;
static constexpr double CFG_TICK_VALUE      = 100.0;
static constexpr int64_t CFG_COOLDOWN_MS    = 15000;
static constexpr int    CFG_DOM_EXIT_MIN    = 2;

struct Tick { uint64_t ts=0; double ask=0, bid=0; };
bool parse_tick(const std::string& line, Tick& t) {
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!std::getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false; try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!std::getline(ss,tok,',')) return false; try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

struct Bar { double open=0,high=0,low=0,close=0; uint64_t ts_open=0; bool valid=false; };
struct M1Builder {
    Bar cur,last,prev; uint64_t cur_min=0;
    void update(uint64_t ts, double mid) {
        const uint64_t bm=ts/60000;
        if (bm!=cur_min){if(cur.valid){prev=last;last=cur;}cur={mid,mid,mid,mid,ts,true};cur_min=bm;}
        else{if(mid>cur.high)cur.high=mid;if(mid<cur.low)cur.low=mid;cur.close=mid;}
    }
    bool has2() const { return last.valid&&prev.valid; }
};
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar& b){r.push_back(b.high-b.low);if((int)r.size()>14)r.pop_front();double s=0;for(auto x:r)s+=x;val=s/r.size();}
};

// Pre-load all ticks into memory for fast sweeping
struct TickData { uint64_t ts; double ask,bid; };

struct DOMSim {
    int bc=3,ac=3,pbc=3,pac=3;
    double bv=3,av=3,pbv=3,pav=3;
    bool vac_ask=false,vac_bid=false,wall_a=false,wall_b=false;
    struct ST{uint64_t ft=0;bool act=false;};
    ST s_bd,s_as,s_wa,s_wb,s_bs,s_vb,s_va;
    void update(double pm,double cm,double sp,uint64_t ts){
        pbc=bc;pac=ac;pbv=bv;pav=av;
        if(cm>pm+sp*0.3){bc=4;ac=1;bv=4;av=1;vac_ask=true;vac_bid=false;wall_a=false;wall_b=false;}
        else if(cm<pm-sp*0.3){bc=1;ac=4;bv=1;av=4;vac_bid=true;vac_ask=false;wall_a=false;wall_b=false;}
        else{bc=3;ac=3;bv=3;av=3;vac_ask=false;vac_bid=false;wall_a=false;wall_b=false;}
        auto tk=[&](ST&s,bool a){if(a){if(!s.act){s.ft=ts;s.act=true;}}else{s.act=false;s.ft=0;}};
        tk(s_bd,bv<pbv*0.7);tk(s_as,av>pav*1.3);tk(s_wa,wall_a);tk(s_wb,wall_b);
        tk(s_bs,bc<pbc&&ac>pac);tk(s_vb,vac_bid&&!vac_ask);tk(s_va,vac_ask&&!vac_bid);
    }
    bool sus(const ST&s,uint64_t now,double sm)const{return s.act&&(now-s.ft)>=(uint64_t)sm;}
    int exit_score(bool il,uint64_t now,double sm)const{
        int sc=0;
        if(il){if(sus(s_bd,now,sm)||sus(s_as,now,sm))sc++;if(sus(s_wa,now,sm))sc++;if(sus(s_bs,now,sm))sc++;if(sus(s_vb,now,sm))sc++;}
        else{if(sus(s_as,now,sm)||sus(s_bd,now,sm))sc++;if(sus(s_wb,now,sm))sc++;if(sus(s_va,now,sm))sc++;if(sus(s_va,now,sm))sc++;}
        return sc;
    }
};

struct Result {
    int rsi_p, ema_n; double thresh, dom_sm;
    int trades,wins; double total_pnl,wr,rr,exp_pt,maxdd;
};

struct RunParams { int rsi_p, ema_n; double thresh, dom_sm; };

Result run_backtest(const std::vector<TickData>& ticks, const RunParams& p) {
    M1Builder bars; ATR14 atr; DOMSim dom;
    bool in_t=false,is_long=false;
    double entry=0,sl=0,sz=0,cp=0,mfe=0;
    int64_t ets=0,cool=0;
    uint64_t lbm=0;
    double pm=0;

    // RSI state
    std::deque<double> gains,losses;
    double prev_mid2=0,rsi_cur=50,rsi_prev=50,rsi_trend=0;
    bool warmed=false; int tc2=0;
    double ema_a=2.0/(p.ema_n+1);

    int trades=0,wins=0; double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;

    for (auto& tk : ticks) {
        const double mid=(tk.ask+tk.bid)*0.5;
        const double sp=tk.ask-tk.bid;
        if(sp>CFG_MAX_SPREAD||sp<=0){pm=mid;continue;}

        // RSI update
        if(prev_mid2!=0){
            const double chg=mid-prev_mid2;
            const double g=chg>0?chg:0,l=chg<0?-chg:0;
            gains.push_back(g);losses.push_back(l);
            if((int)gains.size()>p.rsi_p){gains.pop_front();losses.pop_front();}
            if((int)gains.size()==p.rsi_p){
                double ag=0,al=0;for(auto x:gains)ag+=x;for(auto x:losses)al+=x;
                ag/=p.rsi_p;al/=p.rsi_p;
                rsi_prev=rsi_cur;
                rsi_cur=(al==0)?100.0:100.0-100.0/(1.0+ag/al);
                double slope=rsi_cur-rsi_prev;
                if(!warmed){rsi_trend=slope;warmed=true;}
                else rsi_trend=slope*ema_a+rsi_trend*(1.0-ema_a);
                tc2++;
            }
        }
        prev_mid2=mid;

        // DOM update
        dom.update(pm,mid,sp,tk.ts); pm=mid;

        // Bar update
        bars.update(tk.ts,mid);
        const uint64_t bm=tk.ts/60000;
        if(bm!=lbm&&bars.last.valid){atr.add(bars.last);lbm=bm;}

        if((int64_t)tk.ts<cool) continue;

        if(in_t){
            const double move=is_long?(mid-entry):(entry-mid);
            if(move>mfe)mfe=move;
            if(is_long?(tk.bid<=sl):(tk.ask>=sl)){
                const double px=is_long?tk.bid:tk.ask;
                const double pnl=(is_long?(px-entry):(entry-px))*sz*CFG_TICK_VALUE;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;cool=(int64_t)tk.ts+CFG_COOLDOWN_MS;continue;
            }
            if(dom.exit_score(is_long,tk.ts,p.dom_sm)>=CFG_DOM_EXIT_MIN){
                const double px=is_long?tk.bid:tk.ask;
                const double pnl=(is_long?(px-entry):(entry-px))*sz*CFG_TICK_VALUE;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;cool=(int64_t)tk.ts+5000;continue;
            }
            const int64_t held=(int64_t)(tk.ts-ets);
            if(held>=CFG_STAGNATION_MS&&mfe<cp*CFG_STAGNATION_MULT){
                const double px=is_long?tk.bid:tk.ask;
                const double pnl=(is_long?(px-entry):(entry-px))*sz*CFG_TICK_VALUE;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;cool=(int64_t)tk.ts+CFG_COOLDOWN_MS;continue;
            }
            continue;
        }

        if(!bars.has2()||atr.val<=0||!warmed)continue;
        const int dir=(rsi_trend>p.thresh)?+1:(rsi_trend<-p.thresh)?-1:0;
        if(dir==0)continue;

        const Bar&lb=bars.last;const Bar&pb=bars.prev;
        const double range=lb.high-lb.low;if(range<=0)continue;
        const double bb=lb.close-lb.open,br=lb.open-lb.close;
        const bool bull=(bb>0&&bb/range>=CFG_BODY_RATIO_MIN&&lb.close>pb.high);
        const bool bear=(br>0&&br/range>=CFG_BODY_RATIO_MIN&&lb.close<pb.low);
        if(dir==+1&&!bull)continue;if(dir==-1&&!bear)continue;
        cp=sp+CFG_COST_SLIP*2.0+CFG_COST_COMM*2.0;
        if(range<CFG_COST_MULT*cp)continue;

        is_long=(dir==+1);
        entry=is_long?tk.ask:tk.bid;
        const double slp=atr.val>0?atr.val:5.0;
        sl=is_long?entry-slp:entry+slp;
        sz=std::max(0.01,std::min(0.50,CFG_RISK_USD/(slp*CFG_TICK_VALUE)));
        sz=std::floor(sz/0.001)*0.001;
        ets=(int64_t)tk.ts;mfe=0;in_t=true;
    }

    double wr=trades?100.0*wins/trades:0;
    double aw=wins?wpnl/wins:0,al=(trades-wins)?lpnl/(trades-wins):0;
    double rr=al!=0?-aw/al:0;
    double exp_pt=trades?(wr/100.0*aw+(1.0-wr/100.0)*al):0;
    return {p.rsi_p,p.ema_n,p.thresh,p.dom_sm,trades,wins,total,wr,rr,exp_pt,maxdd};
}

int main(int argc, char* argv[]) {
    const char* infile=argc>1?argv[1]:"/Users/jo/tick/xauusd_merged_24months.csv";
    std::ifstream f(infile);
    if(!f){std::cerr<<"Cannot open: "<<infile<<"\n";return 1;}

    std::cerr<<"Loading ticks...\n";
    std::vector<TickData> ticks;
    ticks.reserve(140000000);
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){
        if(line.empty())continue;
        std::stringstream ss(line);std::string tok;
        TickData td;
        if(!std::getline(ss,tok,','))continue;
        if(tok.empty()||!isdigit((unsigned char)tok[0]))continue;
        try{td.ts=std::stoull(tok);}catch(...){continue;}
        if(!std::getline(ss,tok,','))continue;try{td.ask=std::stod(tok);}catch(...){continue;}
        if(!std::getline(ss,tok,','))continue;try{td.bid=std::stod(tok);}catch(...){continue;}
        if(td.ask>0&&td.bid>0&&td.ask>=td.bid)ticks.push_back(td);
    }
    std::cerr<<"Loaded "<<ticks.size()<<" ticks. Running sweep...\n";

    // Sweep grid
    std::vector<int>    rsi_periods   = {5, 10, 20, 50};
    std::vector<int>    ema_windows   = {3, 5, 10};
    std::vector<double> thresholds    = {0.1, 0.3, 0.5, 1.0, 2.0};
    std::vector<double> dom_smooths   = {100.0, 300.0, 500.0};

    std::vector<Result> results;
    int total_runs=0;
    for(auto rp:rsi_periods)
    for(auto en:ema_windows)
    for(auto th:thresholds)
    for(auto ds:dom_smooths){
        RunParams p{rp,en,th,ds};
        Result r=run_backtest(ticks,p);
        total_runs++;
        if(r.trades>=50 && r.wr>=40.0)
            results.push_back(r);
    }

    // Sort by expectancy descending
    std::sort(results.begin(),results.end(),[](const Result&a,const Result&b){
        return a.exp_pt>b.exp_pt;
    });

    std::cout<<"\n=== RSI+DOM Sweep Results (trades>=50, WR>=40%) ===\n";
    std::cout<<std::left
             <<std::setw(6)<<"RSI_P"<<std::setw(6)<<"EMA_N"<<std::setw(8)<<"THRESH"
             <<std::setw(8)<<"DOM_SM"<<std::setw(8)<<"TRADES"<<std::setw(8)<<"WR%"
             <<std::setw(12)<<"TOTAL_PNL"<<std::setw(8)<<"R:R"<<std::setw(10)<<"EXP/TR"
             <<std::setw(10)<<"MAX_DD"<<"\n";
    std::cout<<std::string(84,'-')<<"\n";

    int shown=0;
    for(auto&r:results){
        if(shown++>=30)break;
        std::cout<<std::fixed
                 <<std::setw(6)<<r.rsi_p
                 <<std::setw(6)<<r.ema_n
                 <<std::setw(8)<<std::setprecision(2)<<r.thresh
                 <<std::setw(8)<<std::setprecision(0)<<r.dom_sm
                 <<std::setw(8)<<r.trades
                 <<std::setw(8)<<std::setprecision(1)<<r.wr
                 <<std::setw(12)<<std::setprecision(2)<<r.total_pnl
                 <<std::setw(8)<<std::setprecision(2)<<r.rr
                 <<std::setw(10)<<std::setprecision(2)<<r.exp_pt
                 <<std::setw(10)<<std::setprecision(2)<<r.maxdd<<"\n";
    }

    std::cout<<"\nTotal configs run: "<<total_runs<<"\n";
    std::cout<<"Configs meeting criteria: "<<results.size()<<"\n";
    return 0;
}
