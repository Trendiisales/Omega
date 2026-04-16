// fast_sweep.cpp — CFE logic, extended grid searching for fast-entry edge
// Same CRTP engine as cfe2. Grid extends sm down to 5s, mh down to 30s.
// Build: clang++ -O3 -std=c++20 -o /tmp/fast_sweep /tmp/fast_sweep.cpp
// Run:   /tmp/fast_sweep ~/Downloads/l2_ticks_2026-04-16.csv

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
      while (std::getline(h,tok,',')) {
        if (tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if (tok=="bid")       cb=ci;
        if (tok=="ask")       ca=ci;
        if (tok=="ewm_drift") cd=ci;
        if (tok=="atr")       cr=ci;
        ++ci; } }
    if (cb<0||ca<0) { fprintf(stderr,"No bid/ask\n"); return out; }
    double pb=0,av=2.0; std::deque<double> aw;
    out.reserve(200000);
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        std::vector<const char*> flds; flds.reserve(20); flds.push_back(buf);
        for (char* c=buf;*c;++c) if (*c==','){*c='\0';flds.push_back(c+1);}
        int nc=(int)flds.size();
        if (nc<=std::max({cm,cb,ca})) continue;
        try {
            Tick t;
            t.ms=(cm>=0)?(int64_t)std::stod(flds[cm]):0;
            t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
            t.drift=(cd>=0&&cd<nc)?std::stod(flds[cd]):0.0;
            t.atr  =(cr>=0&&cr<nc)?std::stod(flds[cr]):0.0;
            if (t.bid<=0||t.ask<t.bid) continue;
            if (t.atr<=0.0) {
                if (pb>0.0) {
                    double tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
                    aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                    if((int)aw.size()>=50){double s=0;for(double x:aw)s+=x;av=s/aw.size()*14.0;}
                }
                t.atr=av;
            }
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
    int     warmup;   // ticks before entry allowed
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
    double tot=0,lp_=0,sp_=0; int nt=0,nw=0;

    explicit EngBase(double a):ra(a){}

    void ursi(double mid) {
        if (!rpm){rpm=mid;return;}
        double c=mid-rpm; rpm=mid;
        rg.push_back(c>0?c:0.0); rl.push_back(c<0?-c:0.0);
        int n=static_cast<D*>(this)->rn();
        if ((int)rg.size()>n){rg.pop_front();rl.pop_front();}
        if ((int)rg.size()>=n) {
            double ag=0,al=0;
            for (double x:rg) ag+=x; ag/=n;
            for (double x:rl) al+=x; al/=n;
            rp=rc; rc=al==0?100.0:100.0-100.0/(1.0+ag/al);
            double s=rc-rp;
            if (!rw){rv=s;rw=true;} else rv=s*ra+rv*(1-ra);
        }
    }

    void close(double ep,bool il,int64_t ms,const P& p) {
        double pp=il?(ep-pe):(pe-ep),pu=pp*ps*100.0;
        tot+=pu; ++nt; if(pu>0)++nw;
        if(il)lp_+=pu; else sp_+=pu;
        lcd=il?1:-1; lcm=ms;
        if(pu<0){ab=true;lex=ep;lld=il?1:-1;cdu=ms+p.cd;}
        pa=false;
    }

    void tick(int64_t ms,double bid,double ask,double drift,double atr,const P& p) {
        double mid=(bid+ask)*0.5,sp=ask-bid;
        ++wt; ursi(mid);
        mb.push_back(mid); if((int)mb.size()>10)mb.pop_front();

        if (drift>=p.st){if(dsd!=1){dsd=1;dss=ms;}}
        else if(drift<=-p.st){if(dsd!=-1){dsd=-1;dss=ms;}}
        else{dsd=0;dss=0;}
        int64_t dsms=dsd?(ms-dss):0;

        if (pa) {
            double mv=pl?(mid-pe):(pe-mid); if(mv>pm)pm=mv;
            double eff=pl?bid:ask,td_=std::fabs(ptp-pe);
            if(!pbe&&td_>0&&mv>=td_*0.5){psl=pe;pbe=true;}
            if(p.tr&&mv>=p.ta){double tsl=pl?(mid-p.td):(mid+p.td);
                if(pl&&tsl>psl)psl=tsl; if(!pl&&tsl<psl)psl=tsl;}
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,pl,ms,p);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pl,ms,p);return;}
            if(ms-pts>p.mh){close(eff,pl,ms,p);return;}
            return;
        }

        if (wt<p.warmup||sp>0.40||atr<1.0) return;
        if (ms<cdu) return;
        if (!rw) return;

        int rd=0;
        if(rv>p.rt&&rv<p.rm) rd=1;
        else if(rv<-p.rt&&rv>-p.rm) rd=-1;
        if(!rd) return;

        if(rd==1&&drift<0) return;
        if(rd==-1&&drift>0) return;
        if(std::fabs(drift)<p.dm) return;
        if(dsms<p.sm) return;

        if((int)mb.size()>=3){
            double n3=mb[mb.size()-3],mv3=mid-n3;
            if(rd==1&&mv3<-atr*0.3) return;
            if(rd==-1&&mv3>atr*0.3) return;
        }
        if(lcd&&(ms-lcm)<45000LL&&rd!=lcd) return;
        if(ab&&lld){
            double dist=(lld==1)?(lex-mid):(mid-lex);
            bool same=(rd==1&&lld==1)||(rd==-1&&lld==-1);
            if(same&&dist>atr*0.4) return; else ab=false;
        }

        bool il=(rd==1);
        double sl_=atr*p.sl,tp_=sl_*p.tp,e=il?ask:bid;
        pa=true;pl=il;pe=e;pm=0;pbe=false;
        psl=il?(e-sl_):(e+sl_); ptp=il?(e+tp_):(e-tp_); pts=ms;
        double raw=10.0/(sl_*100.0);
        ps=std::min(0.10,std::max(0.01,std::round(raw/0.01)*0.01));
    }
    void fin(int64_t ms,const P& p){if(pa)close(pe,pl,ms,p);}
};

struct Eng:EngBase<Eng>{
    int _rn;
    explicit Eng(const P& p):EngBase<Eng>(2.0/(p.rn+1)),_rn(p.rn){}
    int rn()const{return _rn;}
};

struct Res{double pnl,wr,lp,sp;int n;P p;};

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: fast_sweep <csv>");return 1;}
    auto t0=std::chrono::steady_clock::now();
    auto ticks=load_csv(argv[1]);
    if(ticks.empty()) return 1;
    printf("Loaded %zu ticks\n",ticks.size());

    // Full grid: CFE range + TSE-fast range combined
    // warmup: 60 (TSE-fast) and 300 (CFE)
    // sm: 5s through 30s — find where edge starts
    // mh: 30s through 10min
    const int    rn_v[] = {14, 20, 30};
    const double rt_v[] = {1.0, 2.0, 4.0};
    const double rm_v[] = {15.0};
    const double dm_v[] = {0.2, 0.3, 0.5, 0.8};
    const double st_v[] = {0.2, 0.3, 0.5};
    const int64_t sm_v[]= {5000,10000,15000,20000,30000};  // 5s→30s
    const double sl_v[] = {0.3, 0.4, 0.6};
    const double tp_v[] = {2.0, 3.0, 4.0};
    const int64_t cd_v[]= {15000};
    const int64_t mh_v[]= {60000,120000,300000,600000}; // 1min→10min
    const bool   tr_v[] = {false,true};
    const int    wu_v[] = {60};

    long TOTAL=3*3*1*4*3*5*3*3*1*4*2*1; // 19440
    printf("Testing %ld combinations...\n",TOTAL);

    std::vector<Res> results; results.reserve(30000);
    long done=0;

    for (int     rn : rn_v)
    for (double  rt : rt_v)
    for (double  rm_: rm_v)
    for (double  dm : dm_v)
    for (double  st : st_v)
    for (int64_t sm : sm_v)
    for (double  sl : sl_v)
    for (double  tp : tp_v)
    for (int64_t cd : cd_v)
    for (int64_t mh : mh_v)
    for (bool    tr : tr_v)
    for (int     wu __attribute__((unused)) : wu_v)
    {
        P pp{rn,rt,rm_,dm,st,sm,cd,mh,sl,tp,2.0,1.0,tr,wu};
        Eng eng(pp);
        for (auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr,pp);
        if (!ticks.empty()) eng.fin(ticks.back().ms,pp);
        if (eng.nt>=3){
            Res r; r.pnl=eng.tot; r.wr=(double)eng.nw/eng.nt;
            r.lp=eng.lp_; r.sp=eng.sp_; r.n=eng.nt; r.p=pp;
            results.push_back(r);
        }
        if (++done%3000==0){
            double best=0; for(auto& r:results)if(r.pnl>best)best=r.pnl;
            printf("  %ld/%ld  best=$%+.2f\n",done,TOTAL,best); fflush(stdout);
        }
    }

    std::sort(results.begin(),results.end(),[](const Res& a,const Res& b){return a.pnl>b.pnl;});
    auto t1=std::chrono::steady_clock::now();
    printf("\nDone in %.2fs\n",std::chrono::duration<double>(t1-t0).count());

    // Print top 20
    printf("\n%s\nTOP 20\n%s\n",std::string(80,'=').c_str(),std::string(80,'=').c_str());
    printf("%-3s %-9s %-6s %-4s %-7s %-7s  rn  rt   dm   st  sm   sl  tp  mh    wu  tr\n",
           "#","PnL","WR","N","L$","S$");
    int show=std::min((int)results.size(),20);
    for (int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+8.2f %5.1f%% %4d $%+6.2f $%+6.2f"
               "  %2d  %.1f  %.1f  %.1f  %2lds  %.2f  %.1f  %3lds  %3d  %s\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rn,r.p.rt,r.p.dm,r.p.st,
               (long)(r.p.sm/1000),r.p.sl,r.p.tp,
               (long)(r.p.mh/1000),r.p.warmup,r.p.tr?"Y":"N");
    }

    if (!results.empty()){
        auto& b=results[0];
        printf("\n%s\nBEST: $%+.2f  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n"
               "rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f tp=%.1f "
               "cd=%lds mh=%lds warmup=%d trail=%s\n",
               std::string(80,'=').c_str(),
               b.pnl,b.wr*100,b.n,b.lp,b.sp,
               b.p.rn,b.p.rt,b.p.dm,b.p.st,
               (long)(b.p.sm/1000),b.p.sl,b.p.tp,
               (long)(b.p.cd/1000),(long)(b.p.mh/1000),
               b.p.warmup,b.p.tr?"Y":"N");

        // sm breakdown: show best PnL per sm value so we can see where edge lives
        printf("\nEDGE BY sm (best PnL per sustained-drift window):\n");
        int64_t sm_vals[]={5000,10000,15000,20000,30000};
        for (int64_t sv:sm_vals){
            double best_pnl=-1e9; int best_n=0; double best_wr=0;
            for (auto& r:results)
                if(r.p.sm==sv && r.pnl>best_pnl){best_pnl=r.pnl;best_n=r.n;best_wr=r.wr;}
            if (best_n>0)
                printf("  sm=%2lds  best=$%+.2f  WR=%.1f%%  N=%d\n",
                       (long)(sv/1000),best_pnl,best_wr*100,best_n);
        }

        // warmup breakdown
        printf("\nEDGE BY warmup ticks:\n");
        int wu_vals[]={60,300};
        for (int wv:wu_vals){
            double best_pnl=-1e9; int best_n=0; double best_wr=0;
            for (auto& r:results)
                if(r.p.warmup==wv && r.pnl>best_pnl){best_pnl=r.pnl;best_n=r.n;best_wr=r.wr;}
            if (best_n>0)
                printf("  warmup=%3d  best=$%+.2f  WR=%.1f%%  N=%d\n",
                       wv,best_pnl,best_wr*100,best_n);
        }
    }
    return 0;
}
