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

struct TD { uint64_t ts; double ask,bid; };
struct Bar { double open=0,high=0,low=0,close=0; uint64_t ts=0; bool valid=false; };
struct M1Builder {
    Bar cur,last,prev; uint64_t cur_min=0; bool new_bar=false;
    void update(uint64_t ts_ms, double mid) {
        new_bar=false; uint64_t bm=ts_ms/60000;
        if(bm!=cur_min){if(cur.valid){prev=last;last=cur;new_bar=true;}
            cur={mid,mid,mid,mid,ts_ms,true};cur_min=bm;}
        else{if(mid>cur.high)cur.high=mid;if(mid<cur.low)cur.low=mid;cur.close=mid;}
    }
    bool has2()const{return last.valid&&prev.valid;}
};
struct EMA {
    int period; double val=0; bool warm=false; int count=0; double alpha;
    explicit EMA(int p):period(p),alpha(2.0/(p+1)){}
    void update(double v){
        if(!warm){val=v;count++;if(count>=period)warm=true;}
        else val=v*alpha+val*(1.0-alpha);
    }
};
struct ATR14 {
    std::deque<double> r; double val=0;
    void add(const Bar&b){r.push_back(b.high-b.low);if((int)r.size()>14)r.pop_front();
        double s=0;for(auto x:r)s+=x;val=s/r.size();}
};
struct Params { int ef,es,to,ss,se; double bm,sm,tm,sep; };
struct Result { Params p; int trades,wins; double wr,total,aw,al,rr,exp_pt,maxdd; };

Result run(const std::vector<TD>& ticks, const Params& p) {
    M1Builder bars; EMA efa(p.ef),esa(p.es); ATR14 atr;
    bool in_t=false,is_long=false;
    double entry=0,sl=0,tp=0,sz=0,mfe=0;
    int bh=0; int64_t cool=0; uint64_t lbm=0;
    int trades=0,wins=0;
    double total=0,wpnl=0,lpnl=0,peak=0,eq=0,maxdd=0;
    for(auto& t:ticks){
        const double mid=(t.ask+t.bid)*0.5;
        const double sp=t.ask-t.bid;
        if(sp>0.40||sp<=0) continue;
        const int uh=(int)((t.ts/1000)%86400/3600);
        const bool in_sess=(uh>=p.ss&&uh<p.se);
        bars.update(t.ts,mid);
        const uint64_t bm=t.ts/60000;
        if(bm!=lbm&&bars.last.valid){efa.update(bars.last.close);esa.update(bars.last.close);atr.add(bars.last);lbm=bm;}
        if((int64_t)t.ts<cool) continue;
        if(in_t){
            const double move=is_long?(mid-entry):(entry-mid);
            if(move>mfe)mfe=move;
            if(is_long?(t.bid<=sl):(t.ask>=sl)){
                double pnl=(is_long?(t.bid-entry):(entry-t.ask))*sz*100;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;cool=(int64_t)t.ts+10000;continue;
            }
            if(is_long?(t.ask>=tp):(t.bid<=tp)){
                double pnl=(is_long?(t.ask-entry):(entry-t.bid))*sz*100;
                total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                trades++;in_t=false;cool=(int64_t)t.ts+10000;continue;
            }
            if(bars.new_bar&&bars.has2()){
                bh++;
                bool ca=(is_long&&efa.val<esa.val-p.sep)||(!is_long&&efa.val>esa.val+p.sep);
                if(ca||bh>=p.to){
                    double px=is_long?t.bid:t.ask;
                    double pnl=(is_long?(px-entry):(entry-px))*sz*100;
                    total+=pnl;eq+=pnl;if(eq>peak)peak=eq;maxdd=std::max(maxdd,peak-eq);
                    if(pnl>0){wins++;wpnl+=pnl;}else lpnl+=pnl;
                    trades++;in_t=false;cool=(int64_t)t.ts+10000;continue;
                }
            }
            continue;
        }
        if(!bars.new_bar||!bars.has2()||!efa.warm||!esa.warm||atr.val<=0||!in_sess) continue;
        double esep=efa.val-esa.val;
        bool up=(esep>p.sep),dn=(esep<-p.sep);
        if(!up&&!dn) continue;
        double brk=atr.val*p.bm;
        bool lb=up&&(bars.last.close>bars.prev.high+brk);
        bool sb=dn&&(bars.last.close<bars.prev.low-brk);
        if(!lb&&!sb) continue;
        is_long=lb; entry=is_long?t.ask:t.bid;
        double slp=atr.val*p.sm;
        sl=is_long?entry-slp:entry+slp;
        tp=is_long?entry+atr.val*p.tm:entry-atr.val*p.tm;
        sz=std::max(0.01,std::min(0.50,30.0/(slp*100)));
        sz=std::floor(sz/0.001)*0.001;
        mfe=0;bh=0;in_t=true;
    }
    double wr=trades?100.0*wins/trades:0;
    double aw=wins?wpnl/wins:0,al=(trades-wins)?lpnl/(trades-wins):0;
    double rr=al!=0?-aw/al:0;
    double ep=trades?(wr/100.0*aw+(1.0-wr/100.0)*al):0;
    return {p,trades,wins,wr,total,aw,al,rr,ep,maxdd};
}

int main(int argc,char* argv[]) {
    const char* fn=argc>1?argv[1]:"/Users/jo/tick/xauusd_merged_24months.csv";
    std::ifstream f(fn);
    if(!f){std::cerr<<"Cannot open "<<fn<<"\n";return 1;}
    std::cerr<<"Loading...\n";
    std::vector<TD> ticks; ticks.reserve(140000000);
    std::string line; std::getline(f,line);
    while(std::getline(f,line)){
        if(line.empty())continue;
        std::stringstream ss(line); std::string tok; TD td;
        if(!std::getline(ss,tok,','))continue;
        if(tok.empty()||!isdigit((unsigned char)tok[0]))continue;
        try{td.ts=std::stoull(tok);}catch(...){continue;}
        if(!std::getline(ss,tok,','))continue;try{td.ask=std::stod(tok);}catch(...){continue;}
        if(!std::getline(ss,tok,','))continue;try{td.bid=std::stod(tok);}catch(...){continue;}
        if(td.ask>0&&td.bid>0&&td.ask>=td.bid)ticks.push_back(td);
    }
    std::cerr<<"Loaded "<<ticks.size()<<" ticks. Sweeping...\n";

    int efs[]={10,20,50};
    int ess[]={50,100,200};
    double bms[]={0.5,1.0,1.5,2.0,3.0};
    double sms[]={0.8,1.2,2.0};
    double tms[]={1.5,2.5,4.0};
    double seps[]={0.05,0.20,0.50};
    int tos[]={10,20,40};
    int ss_arr[]={7,7,9};
    int se_arr[]={22,17,17};
    int n_sess=3;

    std::vector<Result> results;
    int total_runs=0;

    for(int ef:efs) {
    for(int es:ess) { if(ef>=es) continue;
    for(double bm:bms) {
    for(double sm:sms) {
    for(double tm:tms) { if(tm<=sm) continue;
    for(double sep:seps) {
    for(int to:tos) {
    for(int si=0;si<n_sess;si++) {
        Params p{ef,es,to,ss_arr[si],se_arr[si],bm,sm,tm,sep};
        Result res=run(ticks,p);
        total_runs++;
        if(res.trades>=100&&res.wr>=42.0&&res.exp_pt>0)
            results.push_back(res);
    }}}}}}}
    }

    std::sort(results.begin(),results.end(),[](const Result&a,const Result&b){
        return a.exp_pt>b.exp_pt;
    });

    std::cout<<"\n=== MomentumBreakout Sweep (trades>=100, WR>=42%, exp>0) ===\n";
    std::cout<<std::left
        <<std::setw(5)<<"EF"<<std::setw(5)<<"ES"<<std::setw(5)<<"BRK"
        <<std::setw(5)<<"SL"<<std::setw(5)<<"TP"<<std::setw(6)<<"SEP"
        <<std::setw(5)<<"TO"<<std::setw(6)<<"SESS"
        <<std::setw(6)<<"N"<<std::setw(7)<<"WR%"
        <<std::setw(10)<<"TOTAL"<<std::setw(6)<<"R:R"
        <<std::setw(9)<<"EXP/TR"<<std::setw(10)<<"MAXDD"<<"\n";
    std::cout<<std::string(88,'-')<<"\n";

    int shown=0;
    for(auto& res:results){
        if(shown++>=40) break;
        std::cout<<std::fixed
            <<std::setw(5)<<res.p.ef<<std::setw(5)<<res.p.es
            <<std::setw(5)<<std::setprecision(1)<<res.p.bm
            <<std::setw(5)<<std::setprecision(1)<<res.p.sm
            <<std::setw(5)<<std::setprecision(1)<<res.p.tm
            <<std::setw(6)<<std::setprecision(2)<<res.p.sep
            <<std::setw(5)<<res.p.to
            <<std::setw(3)<<res.p.ss<<"-"<<std::setw(2)<<res.p.se
            <<std::setw(6)<<res.trades
            <<std::setw(7)<<std::setprecision(1)<<res.wr
            <<std::setw(10)<<std::setprecision(0)<<res.total
            <<std::setw(6)<<std::setprecision(2)<<res.rr
            <<std::setw(9)<<std::setprecision(2)<<res.exp_pt
            <<std::setw(10)<<std::setprecision(0)<<res.maxdd<<"\n";
    }
    std::cout<<"\nTotal configs: "<<total_runs<<"\nPassing: "<<results.size()<<"\n";
    return 0;
}
