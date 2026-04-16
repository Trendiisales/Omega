#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct RSI {
    std::deque<double> g,l;
    double pm=0,cur=50,prv=50,trend=0;
    bool w=false; int n; double a;
    RSI(int n_,int en):n(n_),a(2.0/(en+1)){}
    void upd(double m){
        if(!pm){pm=m;return;}
        double c=m-pm;pm=m;
        g.push_back(c>0?c:0);l.push_back(c<0?-c:0);
        if((int)g.size()>n){g.pop_front();l.pop_front();}
        if((int)g.size()>=n){
            double ag=0,al=0;
            for(auto x:g)ag+=x;ag/=n;
            for(auto x:l)al+=x;al/=n;
            prv=cur;cur=al==0?100:100-100/(1+ag/al);
            double s=std::max(-5.0,std::min(5.0,cur-prv));
            if(!w){trend=s;w=true;}else trend=s*a+trend*(1-a);
        }
    }
};

struct P {
    int rn,ren; double rt,rm,dm,st;
    int sm; double sl,tp; int cd,mh;
    bool tr; double ta,td;
};

struct T {double pnl,mfe;bool il;int h;char r[8];};

struct Eng {
    P p; RSI rsi;
    std::deque<double> rm; // recent mid
    int ticks=0;
    bool pa=false,pl=false,pbe=false,ptr_=false;
    double pe=0,psl=0,ptp=0,pm=0,ps=0.01;
    int64_t pts=0,cd=0;
    double ds=0; int64_t dss=0; int dd=0; // drift sustained
    bool ab=false; double lex=0; int ld=0;
    int lcd=0; int64_t lcm=0;
    std::vector<T> trades;

    Eng(P pp):p(pp),rsi(pp.rn,pp.ren){}

    void tick(int64_t ms,double bid,double ask,double drift,double atr){
        ++ticks; double sp=ask-bid,mid=(bid+ask)/2;
        rsi.upd(mid);
        rm.push_back(mid); if((int)rm.size()>10)rm.pop_front();

        // drift sustained
        if(drift>=p.st){if(dd!=1){dd=1;dss=ms;}}
        else if(drift<=-p.st){if(dd!=-1){dd=-1;dss=ms;}}
        else{dd=0;dss=0;}
        int64_t dsms=dd?ms-dss:0;

        if(pa){
            double mv=pl?(mid-pe):(pe-mid);
            if(mv>pm)pm=mv;
            double eff=pl?bid:ask,td_=fabs(ptp-pe);
            if(!pbe&&td_>0&&mv>=td_*0.5){psl=pe;pbe=true;}
            if(p.tr&&mv>=p.ta){
                double tsl=pl?(mid-p.td):(mid+p.td);
                if(pl&&tsl>psl)psl=tsl;
                if(!pl&&tsl<psl)psl=tsl;
                ptr_=true;
            }
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){cl(eff,"TP",ms);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){cl(eff,pbe?"BE":"SL",ms);return;}
            if((ms-pts)/1000>p.mh){cl(eff,"TO",ms);return;}
            return;
        }

        if(ticks<200||sp>0.40||atr<1.0||ms<cd||!rsi.w)return;

        // RSI dir
        int rd=0;
        if(rsi.trend>p.rt&&rsi.trend<p.rm)rd=1;
        else if(rsi.trend<-p.rt&&rsi.trend>-p.rm)rd=-1;
        if(!rd)return;

        // drift agrees
        if(rd==1&&drift<0)return;
        if(rd==-1&&drift>0)return;
        if(fabs(drift)<p.dm)return;

        // sustained
        if(dsms<(int64_t)p.sm*1000)return;

        // price confirm (last 3 ticks)
        if((int)rm.size()>=3){
            double o=rm[rm.size()-3],mv_=mid-o;
            if(rd==1&&mv_<-atr*0.3)return;
            if(rd==-1&&mv_>atr*0.3)return;
        }

        // counter-spike
        if((int)rm.size()>=3){
            double o=rm[rm.size()-3],mv_=mid-o;
            double thr=atr*0.4;
            if(rd==1&&mv_<=-thr)return;
            if(rd==-1&&mv_>=thr)return;
        }

        // opp dir cooldown
        if(lcd&&ms-lcm<45000&&rd!=lcd)return;

        // adverse block
        if(ab&&ld){
            double dist=ld==1?(lex-mid):(mid-lex);
            bool same=(rd==1&&ld==1)||(rd==-1&&ld==-1);
            if(same&&dist>atr*0.4)return;
            else ab=false;
        }

        bool il=rd==1;
        double sl_=atr*p.sl,tp_=sl_*p.tp;
        double e=il?ask:bid;
        pa=true;pl=il;pe=e;
        psl=il?e-sl_:e+sl_;ptp=il?e+tp_:e-tp_;
        pts=ms;pm=0;pbe=false;ptr_=false;
        ps=std::min(0.10,std::max(0.01,std::round(10.0/(sl_*100)/0.01)*0.01));
    }

    void cl(double ep,const char* r,int64_t ms){
        double pp=pl?(ep-pe):(pe-ep),pu=pp*ps*100;
        lcd=pl?1:-1;lcm=ms;
        if(pu<0){ab=true;lex=ep;ld=pl?1:-1;cd=ms+(int64_t)p.cd*1000;}
        T t;t.pnl=pu;t.mfe=pm;t.il=pl;t.h=(int)((ms-pts)/1000);
        strncpy(t.r,r,7);t.r[7]=0;trades.push_back(t);pa=false;
    }
    void fin(int64_t ms){if(pa)cl(pe,"END",ms);}
    double tot()const{double s=0;for(auto&t:trades)s+=t.pnl;return s;}
    double lp()const{double s=0;for(auto&t:trades)s+=t.il?t.pnl:0;return s;}
    double sp()const{double s=0;for(auto&t:trades)s+=t.il?0:t.pnl;return s;}
    int wins()const{int n=0;for(auto&t:trades)n+=(t.pnl>0);return n;}
    int n()const{return(int)trades.size();}
};

struct Tick{int64_t ms;double bid,ask,drift,atr;};

std::vector<Tick> load(const char* path){
    std::vector<Tick> v;std::ifstream f(path);
    if(!f){fprintf(stderr,"Cannot open %s\n",path);return v;}
    std::string line,tok;std::getline(f,line);
    int tc=-1,bc=-1,ac=-1,dc=-1,rc=-1,ci=0;
    std::istringstream hss(line);
    while(std::getline(hss,tok,',')){
        if(tok=="ts_ms"||tok=="timestamp_ms")tc=ci;
        if(tok=="bid")bc=ci;if(tok=="ask")ac=ci;
        if(tok=="ewm_drift")dc=ci;if(tok=="atr")rc=ci;++ci;
    }
    double pb=0,av=2;std::deque<double> aw;
    v.reserve(200000);
    while(std::getline(f,line)){
        std::vector<std::string> c;std::istringstream ss(line);
        while(std::getline(ss,tok,','))c.push_back(tok);
        if((int)c.size()<=std::max({tc,bc,ac}))continue;
        try{
            Tick t;
            t.ms=tc>=0?(int64_t)std::stod(c[tc]):0;
            t.bid=std::stod(c[bc]);t.ask=std::stod(c[ac]);
            t.drift=dc>=0?std::stod(c[dc]):0;
            t.atr=rc>=0?std::stod(c[rc]):0;
            if(t.bid<=0||t.ask<t.bid)continue;
            if(t.atr<=0){
                if(pb>0){
                    double tr=t.ask-t.bid+fabs(t.bid-pb);
                    aw.push_back(tr);if((int)aw.size()>200)aw.pop_front();
                    if((int)aw.size()>=50){double s=0;for(auto x:aw)s+=x;av=s/aw.size()*14;}
                }
                t.atr=av;
            }
            pb=t.bid;v.push_back(t);
        }catch(...){}
    }
    return v;
}

struct Res{double pnl,wr,lp,sp;int n;P p;};

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: cfe_sweep <csv>");return 1;}
    auto t0=std::chrono::steady_clock::now();
    auto ticks=load(argv[1]);
    printf("Loaded %zu ticks\n",ticks.size());

    // Grid matching Python cfe_sweep
    int    rn_v[]  ={20,30};
    int    ren_v[] ={8,12};
    double rt_v[]  ={2.0,4.0,6.0};
    double dm_v[]  ={0.3,0.5,0.8};
    double st_v[]  ={0.3,0.5};
    int    sm_v[]  ={10,20,30};    // seconds
    double sl_v[]  ={0.4,0.6,0.8};
    double tp_v[]  ={1.5,2.0,2.5};
    int    cd_v[]  ={15,30};
    int    mh_v[]  ={180,300};
    bool   tr_v[]  ={false,true};

    long total=2*2*3*3*2*3*3*3*2*2*2;
    printf("Testing %ld combinations...\n",total);

    std::vector<Res> results; results.reserve(50000);
    long done=0;

    for(int rn:rn_v)for(int ren:ren_v)for(double rt:rt_v)
    for(double dm:dm_v)for(double st:st_v)for(int sm:sm_v)
    for(double sl:sl_v)for(double tp:tp_v)for(int cd:cd_v)
    for(int mh:mh_v)for(bool tr:tr_v){
        P p;p.rn=rn;p.ren=ren;p.rt=rt;p.rm=15;
        p.dm=dm;p.st=st;p.sm=sm;p.sl=sl;p.tp=tp;
        p.cd=cd;p.mh=mh;p.tr=tr;p.ta=2;p.td=1;

        Eng eng(p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        if(!ticks.empty())eng.fin(ticks.back().ms);

        if(eng.n()>=3){
            Res r;r.pnl=eng.tot();r.wr=(double)eng.wins()/eng.n();
            r.lp=eng.lp();r.sp=eng.sp();r.n=eng.n();r.p=p;
            results.push_back(r);
        }
        if(++done%1000==0){
            double best=0;for(auto& r:results)if(r.pnl>best)best=r.pnl;
            printf("  %ld/%ld best=$%+.2f\n",done,total,best);fflush(stdout);
        }
    }

    std::sort(results.begin(),results.end(),[](const Res&a,const Res&b){return a.pnl>b.pnl;});
    auto t1=std::chrono::steady_clock::now();
    printf("Done in %.2fs\n",std::chrono::duration<double>(t1-t0).count());

    puts("\n================================================================");
    puts("TOP 20");
    puts("================================================================");
    printf("%-3s %-8s %-6s %-4s %-7s %-7s %-5s %-5s %-5s %-4s %-4s %-3s %-4s\n",
           "#","PnL","WR","N","L$","S$","rsi_t","drift","sl","tp","sm","tr","hold");
    int show=std::min((int)results.size(),20);
    for(int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+7.2f %5.1f%% %4d $%+6.2f $%+6.2f %5.1f %5.2f %5.2f %4.1f %4d %3s %4d\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rt,r.p.dm,r.p.sl,r.p.tp,r.p.sm,
               r.p.tr?"Y":"N",r.p.mh);
    }

    if(!results.empty()){
        auto& b=results[0];
        printf("\nBEST: $%+.2f WR=%.1f%% N=%d L=$%+.2f S=$%+.2f\n",
               b.pnl,b.wr*100,b.n,b.lp,b.sp);
        printf("rn=%d ren=%d rt=%.1f dm=%.2f st=%.2f sm=%ds sl=%.2f tp=%.1f cd=%d mh=%d trail=%s\n",
               b.p.rn,b.p.ren,b.p.rt,b.p.dm,b.p.st,b.p.sm,
               b.p.sl,b.p.tp,b.p.cd,b.p.mh,b.p.tr?"Y":"N");
        Eng eng(b.p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        if(!ticks.empty())eng.fin(ticks.back().ms);
        puts("\nAll trades:");
        printf("  %-5s $%-7s %-5s %-4s %s\n","Side","PnL","MFE","R","Held");
        for(auto& t:eng.trades)
            printf("  %-5s $%+6.2f %5.2f %-4s %ds\n",
                   t.il?"LONG":"SHORT",t.pnl,t.mfe,t.r,t.h);
    }
    return 0;
}
