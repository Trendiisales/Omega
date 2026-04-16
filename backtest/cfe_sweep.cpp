// CFE Sweep -- C++ version, compiles on Mac
// clang++ -O3 -std=c++20 -o /tmp/cfe_sweep /tmp/cfe_sweep.cpp
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

// ── Tick RSI ─────────────────────────────────────────────────────────────────
struct TickRSI {
    std::deque<double> g,l;
    double pm=0,cur=50,prv=50,slope=0;
    bool warm=false;
    int period; double alpha;
    TickRSI(int n,int en):period(n),alpha(2.0/(en+1)){}
    void update(double mid){
        if(!pm){pm=mid;return;}
        double c=mid-pm;pm=mid;
        g.push_back(c>0?c:0);l.push_back(c<0?-c:0);
        if((int)g.size()>period){g.pop_front();l.pop_front();}
        if((int)g.size()>=period){
            double ag=0,al=0;
            for(auto x:g)ag+=x;ag/=period;
            for(auto x:l)al+=x;al/=period;
            prv=cur;cur=al==0?100:100-100/(1+ag/al);
            double s=std::max(-5.0,std::min(5.0,cur-prv));
            if(!warm){slope=s;warm=true;}
            else slope=s*alpha+slope*(1-alpha);
        }
    }
    int dir(double thresh,double maxv) const {
        if(!warm)return 0;
        if(slope>thresh&&slope<maxv)return 1;
        if(slope<-thresh&&slope>-maxv)return -1;
        return 0;
    }
};

struct Params {
    int   rsi_n=30,rsi_en=10;
    double rsi_t=4.0,rsi_max=15.0;
    double drift_min=0.5,sus_thresh=0.5;
    int   sus_ms=30000;
    double sl_atr=0.6,tp_rr=2.0;
    int   cool_ms=15000,hold_ms=300000;
    bool  trail=false;
    double trail_arm=2.0,trail_dist=1.0;
};

struct Trade{double pnl,mfe;bool is_long;int held_s;char reason[8];};

struct CFEEngine {
    Params p; TickRSI rsi;
    int ticks=0;
    bool pa=false,pl=false,pbe=false,ptrail=false;
    double pe=0,psl=0,ptp=0,pm=0,ps=0.01;
    int64_t pts=0,cd=0;
    // drift sustained
    int dsd=0; int64_t dss=0;
    // recent mid for price confirm
    std::deque<double> rm;
    // adverse block
    bool ab=false; double lex=0; int lld=0; double latr=0;
    // opp dir cooldown
    int lcd=0; int64_t lcm=0;
    std::vector<Trade> trades;

    CFEEngine(Params pp):p(pp),rsi(pp.rsi_n,pp.rsi_en){}

    void tick(int64_t ms,double bid,double ask,double drift,double atr){
        ++ticks; double sp=ask-bid, mid=(bid+ask)/2;
        rsi.update(mid);
        rm.push_back(mid); if((int)rm.size()>10)rm.pop_front();
        // drift sustained
        if(drift>=p.sus_thresh){ if(dsd!=1){dsd=1;dss=ms;} }
        else if(drift<=-p.sus_thresh){ if(dsd!=-1){dsd=-1;dss=ms;} }
        else{ dsd=0;dss=0; }
        int64_t dsms=(dsd!=0)?(ms-dss):0;

        if(pa){
            double mv=pl?(mid-pe):(pe-mid); if(mv>pm)pm=mv;
            double eff=pl?bid:ask, td=std::fabs(ptp-pe);
            if(!pbe&&td>0&&mv>=td*0.5){psl=pe;pbe=true;}
            if(p.trail&&mv>=p.trail_arm){
                double tsl=pl?(mid-p.trail_dist):(mid+p.trail_dist);
                if(pl&&tsl>psl)psl=tsl; if(!pl&&tsl<psl)psl=tsl;
                ptrail=true;
            }
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,"TP",ms);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){
                close(eff,ptrail?"TRAIL":(pbe?"BE":"SL"),ms);return;
            }
            if(ms-pts>(int64_t)p.hold_ms){close(eff,"TO",ms);return;}
            return;
        }

        if(ticks<300||sp>0.40||atr<1.0||ms<cd)return;
        int rd=rsi.dir(p.rsi_t,p.rsi_max);
        if(!rd)return;
        if(rd==1&&drift<0)return;
        if(rd==-1&&drift>0)return;
        if(std::fabs(drift)<p.drift_min)return;
        if(dsms<(int64_t)p.sus_ms)return;
        // price confirm: last 3 ticks not counter-spike
        if((int)rm.size()>=3){
            double oldest=*( rm.end()-3);
            double mv3=mid-oldest;
            if(rd==1&&mv3<-atr*0.3)return;
            if(rd==-1&&mv3>atr*0.3)return;
        }
        // opp dir cooldown
        if(lcd!=0&&ms-lcm<45000&&rd!=lcd)return;
        // adverse block
        if(ab&&lld!=0){
            double dist=lld==1?(lex-mid):(mid-lex);
            bool same=(rd==1&&lld==1)||(rd==-1&&lld==-1);
            if(same&&dist>latr*0.4)return;
            else ab=false;
        }
        // enter
        bool il=rd==1;
        double sl=atr*p.sl_atr, tp=sl*p.tp_rr, e=il?ask:bid;
        pa=true;pl=il;pe=e;psl=il?e-sl:e+sl;ptp=il?e+tp:e-tp;
        pts=ms;pm=0;pbe=false;ptrail=false;
        ps=std::min(0.10,std::max(0.01,std::round(10.0/(sl*100)/0.01)*0.01));
    }

    void close(double ep,const char* r,int64_t ms){
        double pp=pl?(ep-pe):(pe-ep), pu=pp*ps*100;
        lcd=pl?1:-1; lcm=ms;
        if(pu<0){ab=true;lex=ep;lld=pl?1:-1;latr=1.5;cd=ms+p.cool_ms;}
        Trade t;t.pnl=pu;t.mfe=pm;t.is_long=pl;
        t.held_s=(int)((ms-pts)/1000);
        strncpy(t.reason,r,7);t.reason[7]=0;trades.push_back(t);pa=false;
    }
    void fin(int64_t ms){if(pa)close(pa?pe:pe,"END",ms);}
    double totpnl()const{double s=0;for(auto&t:trades)s+=t.pnl;return s;}
    double lpnl()const{double s=0;for(auto&t:trades)s+=t.is_long?t.pnl:0;return s;}
    double spnl()const{double s=0;for(auto&t:trades)s+=t.is_long?0:t.pnl;return s;}
    int wins()const{int n=0;for(auto&t:trades)n+=(t.pnl>0);return n;}
    int n()const{return(int)trades.size();}
};

struct Tick{int64_t ms;double bid,ask,drift,atr;};

std::vector<Tick> load(const char* path){
    std::vector<Tick> v; std::ifstream f(path);
    if(!f){fprintf(stderr,"Cannot open %s\n",path);return v;}
    std::string line,tok; std::getline(f,line);
    int tc=-1,bc=-1,ac=-1,dc=-1,rc=-1,ci=0;
    std::istringstream hss(line);
    while(std::getline(hss,tok,',')){
        if(tok=="ts_ms"||tok=="timestamp_ms")tc=ci;
        if(tok=="bid")bc=ci; if(tok=="ask")ac=ci;
        if(tok=="ewm_drift")dc=ci; if(tok=="atr")rc=ci; ++ci;
    }
    if(bc<0||ac<0){fprintf(stderr,"Bad header\n");return v;}
    double pb=0,av=2; std::deque<double> aw;
    v.reserve(300000);
    while(std::getline(f,line)){
        std::vector<std::string> c; std::istringstream ss(line);
        while(std::getline(ss,tok,','))c.push_back(tok);
        if((int)c.size()<=std::max({tc,bc,ac}))continue;
        try{
            Tick t;
            t.ms=tc>=0?(int64_t)std::stod(c[tc]):0;
            t.bid=std::stod(c[bc]); t.ask=std::stod(c[ac]);
            t.drift=dc>=0?std::stod(c[dc]):0;
            t.atr=rc>=0?std::stod(c[rc]):0;
            if(t.bid<=0||t.ask<t.bid)continue;
            if(t.atr<=0){
                if(pb>0){
                    double tr=t.ask-t.bid+std::fabs(t.bid-pb);
                    aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                    if((int)aw.size()>=50){double s=0;for(auto x:aw)s+=x;av=s/aw.size()*14;}
                }
                t.atr=av;
            }
            pb=t.bid; v.push_back(t);
        }catch(...){}
    }
    return v;
}

struct Res{double pnl,wr,lp,sp;int n;Params p;};

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: cfe_sweep <csv>");return 1;}
    auto t0=std::chrono::steady_clock::now();
    auto ticks=load(argv[1]);
    printf("Loaded %zu ticks\n",ticks.size());
    if(ticks.empty())return 1;

    // Grid matching Python cfe_sweep.py
    int    rsi_n[]  ={20,30};
    double rsi_t[]  ={2.0,4.0,6.0};
    double dm[]     ={0.3,0.5,0.8};
    double st[]     ={0.3,0.5};
    int    sm[]     ={10000,20000,30000};
    double sl[]     ={0.4,0.6,0.8};
    double tp[]     ={1.5,2.0,2.5};
    int    cd[]     ={15000,30000};
    int    mh[]     ={180000,300000};
    bool   tr[]     ={false,true};

    int total=2*3*3*2*3*3*3*2*2*2;
    printf("Testing %d combinations...\n",total);

    std::vector<Res> results; results.reserve(total);
    int done=0;
    for(int rn:rsi_n)
    for(double rt:rsi_t)
    for(double d:dm)
    for(double s2:st)
    for(int s3:sm)
    for(double s4:sl)
    for(double t2:tp)
    for(int c:cd)
    for(int m:mh)
    for(bool t3:tr){
        Params p; p.rsi_n=rn; p.rsi_t=rt; p.drift_min=d;
        p.sus_thresh=s2; p.sus_ms=s3; p.sl_atr=s4; p.tp_rr=t2;
        p.cool_ms=c; p.hold_ms=m; p.trail=t3;
        CFEEngine eng(p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        eng.fin(ticks.back().ms);
        if(eng.n()>=3){
            Res r; r.pnl=eng.totpnl(); r.wr=(double)eng.wins()/eng.n();
            r.lp=eng.lpnl(); r.sp=eng.spnl(); r.n=eng.n(); r.p=p;
            results.push_back(r);
        }
        if(++done%500==0){
            double best=0; for(auto& r:results)if(r.pnl>best)best=r.pnl;
            printf("  %d/%d best=$%+.2f\n",done,total,best); fflush(stdout);
        }
    }
    std::sort(results.begin(),results.end(),[](const Res&a,const Res&b){return a.pnl>b.pnl;});
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
    printf("Done in %.2fs\n",elapsed);

    puts("\n================================================================");
    puts("TOP 20 CFE CONFIGURATIONS");
    puts("================================================================");
    printf("%-3s %-8s %-6s %-4s %-7s %-7s %-5s %-5s %-5s %-5s %-5s %-5s %-5s\n",
           "#","PnL","WR","N","L$","S$","rsi_t","drift","sus_s","sl","tp","hold","tr");
    int show=std::min((int)results.size(),20);
    for(int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+7.2f %5.1f%% %4d $%+6.2f $%+6.2f %5.1f %5.2f %5d %5.2f %4.1f %5d %3s\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rsi_t,r.p.drift_min,r.p.sus_ms/1000,
               r.p.sl_atr,r.p.tp_rr,r.p.hold_ms/1000,r.p.trail?"Y":"N");
    }
    if(!results.empty()){
        auto& b=results[0];
        printf("\nBEST: $%+.2f WR=%.1f%% N=%d L=$%+.2f S=$%+.2f\n",
               b.pnl,b.wr*100,b.n,b.lp,b.sp);
        printf("rsi_n=%d rsi_thresh=%.1f drift=%.2f sus=%ds sl=%.2fxATR tp=%.1f hold=%ds trail=%s\n",
               b.p.rsi_n,b.p.rsi_t,b.p.drift_min,b.p.sus_ms/1000,
               b.p.sl_atr,b.p.tp_rr,b.p.hold_ms/1000,b.p.trail?"Y":"N");
        CFEEngine eng(b.p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        eng.fin(ticks.back().ms);
        puts("\nAll trades:");
        for(auto& t:eng.trades)
            printf("  %-5s $%+6.2f mfe=%5.2f %-6s %ds\n",
                   t.is_long?"LONG":"SHORT",t.pnl,t.mfe,t.reason,t.held_s);
    }
    return 0;
}
