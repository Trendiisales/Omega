// tse_sweep.cpp — CRTP TSE sweep, same logic as cfe2 but TSE-scaled params
// RSI slope EMA + sustained drift, warmup=60 ticks, shorter hold
// Build: clang++ -O3 -std=c++20 -o /tmp/tse_sweep /tmp/tse_sweep.cpp
// Run:   /tmp/tse_sweep ~/Downloads/l2_ticks_2026-04-16.csv

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Tick { int64_t ms; double bid,ask,drift,atr; };

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) { fprintf(stderr,"Cannot open %s\n",path); return out; }
    std::string line,tok;
    std::getline(f,line);
    if (!line.empty()&&line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,cr=-1,ci=0;
    { std::istringstream h(line);
      while(std::getline(h,tok,',')) {
        if(tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if(tok=="bid") cb=ci; if(tok=="ask") ca=ci;
        if(tok=="ewm_drift") cd=ci; if(tok=="atr") cr=ci; ++ci; } }
    if(cb<0||ca<0){fprintf(stderr,"No bid/ask\n");return out;}
    double pb=0,av=2.0; std::deque<double> aw;
    out.reserve(200000);
    while(std::getline(f,line)) {
        if(line.empty()) continue;
        if(line.back()=='\r') line.pop_back();
        static char buf[512];
        if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        std::vector<const char*> flds; flds.reserve(20); flds.push_back(buf);
        for(char* c=buf;*c;++c) if(*c==','){*c='\0';flds.push_back(c+1);}
        int nc=(int)flds.size();
        if(nc<=std::max({cm,cb,ca})) continue;
        try {
            Tick t;
            t.ms=(cm>=0)?(int64_t)std::stod(flds[cm]):0;
            t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
            t.drift=(cd>=0&&cd<nc)?std::stod(flds[cd]):0.0;
            t.atr=(cr>=0&&cr<nc)?std::stod(flds[cr]):0.0;
            if(t.bid<=0||t.ask<t.bid) continue;
            if(t.atr<=0.0){
                if(pb>0.0){double tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
                    aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                    if((int)aw.size()>=50){double s=0;for(double x:aw)s+=x;av=s/aw.size()*14.0;}}
                t.atr=av;}
            pb=t.bid; out.push_back(t);
        } catch(...) {}
    }
    return out;
}

struct P {
    int     rn;
    double  rt,rm,dm,st;
    int64_t sm,cd,mh;
    double  sl,tp,ta,td;
    bool    tr;
};

template<typename D>
struct EngBase {
    std::deque<double> rg,rl,mb;
    double rpm=0,rc=50,rp=50,rv=0; bool rw=false; double ra;
    int dsd=0; int64_t dss=0;
    bool pa=false,pl=false,pbe=false;
    double pe=0,psl=0,ptp=0,pm=0,ps=0.01;
    int64_t pts=0,cdu=0,lcm=0; int lcd=0,wt=0;
    bool ab=false; double lex=0; int lld=0;
    double tot=0,lp=0,sp=0; int nt=0,nw=0;

    explicit EngBase(double a):ra(a){}

    void ursi(double mid){
        if(!rpm){rpm=mid;return;}
        double c=mid-rpm;rpm=mid;
        rg.push_back(c>0?c:0.0); rl.push_back(c<0?-c:0.0);
        int n=static_cast<D*>(this)->rn();
        if((int)rg.size()>n){rg.pop_front();rl.pop_front();}
        if((int)rg.size()>=n){
            double ag=0,al=0;
            for(double x:rg)ag+=x; ag/=n;
            for(double x:rl)al+=x; al/=n;
            rp=rc; rc=al==0?100.0:100.0-100.0/(1.0+ag/al);
            double s=rc-rp;
            if(!rw){rv=s;rw=true;} else rv=s*ra+rv*(1-ra);
        }
    }

    void close(double ep,bool il,int64_t ms,const P& p,double en,double sz){
        double pp=il?(ep-en):(en-ep),pu=pp*sz*100.0;
        tot+=pu;++nt; if(pu>0)++nw;
        if(il)lp+=pu; else sp+=pu;
        lcd=il?1:-1; lcm=ms;
        if(pu<0){ab=true;lex=ep;lld=il?1:-1;cdu=ms+p.cd;}
        pa=false;
    }

    bool tick(int64_t ms,double bid,double ask,double drift,double atr,const P& p){
        double mid=(bid+ask)*0.5,s=ask-bid;
        ++wt; ursi(mid);
        mb.push_back(mid); if((int)mb.size()>10)mb.pop_front();

        if(drift>=p.st){if(dsd!=1){dsd=1;dss=ms;}}
        else if(drift<=-p.st){if(dsd!=-1){dsd=-1;dss=ms;}}
        else{dsd=0;dss=0;}
        int64_t dsms=dsd?(ms-dss):0;

        if(pa){
            double mv=pl?(mid-pe):(pe-mid); if(mv>pm)pm=mv;
            double eff=pl?bid:ask,td_=std::fabs(ptp-pe);
            if(!pbe&&td_>0&&mv>=td_*0.5){psl=pe;pbe=true;}
            if(p.tr&&mv>=p.ta){double tsl=pl?(mid-p.td):(mid+p.td);
                if(pl&&tsl>psl)psl=tsl; if(!pl&&tsl<psl)psl=tsl;}
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,pl,ms,p,pe,ps);return true;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pl,ms,p,pe,ps);return true;}
            if(ms-pts>p.mh){close(eff,pl,ms,p,pe,ps);return true;}
            return true;
        }

        if(wt<60||s>0.40||atr<1.0) return false;
        if(ms<cdu) return false;
        if(!rw) return false;

        int rd=0;
        if(rv>p.rt&&rv<p.rm) rd=1;
        else if(rv<-p.rt&&rv>-p.rm) rd=-1;
        if(!rd) return false;

        if(rd==1&&drift<0) return false;
        if(rd==-1&&drift>0) return false;
        if(std::fabs(drift)<p.dm) return false;
        if(dsms<p.sm) return false;

        if((int)mb.size()>=3){
            double n3=mb[mb.size()-3],mv3=mid-n3;
            if(rd==1&&mv3<-atr*0.3) return false;
            if(rd==-1&&mv3>atr*0.3) return false;
        }
        if(lcd&&(ms-lcm)<45000LL&&rd!=lcd) return false;
        if(ab&&lld){
            double dist=(lld==1)?(lex-mid):(mid-lex);
            bool same=(rd==1&&lld==1)||(rd==-1&&lld==-1);
            if(same&&dist>atr*0.4) return false; else ab=false;
        }

        bool il=(rd==1);
        double sl_=atr*p.sl,tp_=sl_*p.tp,e=il?ask:bid;
        pa=true;pl=il;pe=e;pm=0;pbe=false;
        psl=il?(e-sl_):(e+sl_); ptp=il?(e+tp_):(e-tp_); pts=ms;
        double raw=10.0/(sl_*100.0);
        ps=std::min(0.10,std::max(0.01,std::round(raw/0.01)*0.01));
        return true;
    }
    void fin(int64_t ms,const P& p){if(pa)close(pe,pl,ms,p,pe,ps);}
};

struct Eng:EngBase<Eng>{
    int _rn;
    explicit Eng(const P& p):EngBase<Eng>(2.0/(p.rn+1)),_rn(p.rn){}
    int rn()const{return _rn;}
};

struct Res{double pnl,wr,lp,sp;int n;P p;};

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: tse_sweep <csv>");return 1;}
    auto t0=std::chrono::steady_clock::now();
    auto ticks=load_csv(argv[1]);
    if(ticks.empty()) return 1;
    printf("Loaded %zu ticks\n",ticks.size());

    // TSE grid: faster RSI, shorter sustained, tighter SL, shorter hold
    const int    rn_v[] ={8,14};
    const double rt_v[] ={1.0,2.0,3.0};
    const double rm_v[] ={15.0};
    const double dm_v[] ={0.2,0.3,0.5};
    const double st_v[] ={0.2,0.3};
    const int64_t sm_v[]={5000,10000,15000};   // 5s/10s/15s
    const double sl_v[] ={0.3,0.4,0.5};
    const double tp_v[] ={1.5,2.0,2.5};
    const int64_t cd_v[]={10000,15000};
    const int64_t mh_v[]={60000,120000};       // 1min/2min
    const bool   tr_v[] ={false,true};

    constexpr long TOTAL=2*3*1*3*2*3*3*3*2*2*2; // 3888
    printf("Testing %ld combinations...\n",TOTAL);

    std::vector<Res> results; results.reserve(5000);
    long done=0;

    for(int rn:rn_v)
    for(double rt:rt_v)
    for(double rm_:rm_v)
    for(double dm:dm_v)
    for(double st:st_v)
    for(int64_t sm:sm_v)
    for(double sl:sl_v)
    for(double tp:tp_v)
    for(int64_t cd:cd_v)
    for(int64_t mh:mh_v)
    for(bool tr:tr_v)
    {
        P pp{rn,rt,rm_,dm,st,sm,cd,mh,sl,tp,2.0,1.0,tr};
        Eng eng(pp);
        for(auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr,pp);
        if(!ticks.empty()) eng.fin(ticks.back().ms,pp);
        if(eng.nt>=3){
            Res r; r.pnl=eng.tot; r.wr=(double)eng.nw/eng.nt;
            r.lp=eng.lp; r.sp=eng.sp; r.n=eng.nt; r.p=pp;
            results.push_back(r);
        }
        if(++done%1000==0){
            double best=0; for(auto& r:results)if(r.pnl>best)best=r.pnl;
            printf("  %ld/%ld  best=$%+.2f\n",done,TOTAL,best); fflush(stdout);
        }
    }

    std::sort(results.begin(),results.end(),[](const Res& a,const Res& b){return a.pnl>b.pnl;});
    auto t1=std::chrono::steady_clock::now();
    printf("\nDone in %.2fs\n",std::chrono::duration<double>(t1-t0).count());

    printf("\n%s\nTOP 20 TSE\n%s\n",
           std::string(72,'=').c_str(),std::string(72,'=').c_str());
    printf("%-3s %-9s %-6s %-4s %-7s %-7s  rn  rt   dm   st  sm   sl   tp  mh   tr\n",
           "#","PnL","WR","N","L$","S$");
    int show=std::min((int)results.size(),20);
    for(int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+8.2f %5.1f%% %4d $%+6.2f $%+6.2f"
               "  %2d  %.1f  %.1f  %.1f  %2lds  %.2f  %.1f  %3lds  %s\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rn,r.p.rt,r.p.dm,r.p.st,
               (long)(r.p.sm/1000),r.p.sl,r.p.tp,
               (long)(r.p.mh/1000),r.p.tr?"Y":"N");
    }
    if(!results.empty()){
        auto& b=results[0];
        printf("\n%s\nBEST: $%+.2f  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n"
               "rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f tp=%.1f cd=%lds mh=%lds trail=%s\n",
               std::string(72,'=').c_str(),
               b.pnl,b.wr*100,b.n,b.lp,b.sp,
               b.p.rn,b.p.rt,b.p.dm,b.p.st,
               (long)(b.p.sm/1000),b.p.sl,b.p.tp,
               (long)(b.p.cd/1000),(long)(b.p.mh/1000),b.p.tr?"Y":"N");
    }
    return 0;
}
