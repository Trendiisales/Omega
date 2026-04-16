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

struct TickRSI {
    std::deque<double> gains,losses;
    double prev_mid=0,cur=50,prev=50,slope_ema=0;
    bool warmed=false;
    int period; double alpha;
    TickRSI(int n,int en):period(n),alpha(2.0/(en+1)){}
    void update(double bid){
        if(!prev_mid){prev_mid=bid;return;}
        double c=bid-prev_mid;prev_mid=bid;
        gains.push_back(c>0?c:0);losses.push_back(c<0?-c:0);
        if((int)gains.size()>period){gains.pop_front();losses.pop_front();}
        if((int)gains.size()>=period){
            double ag=0,al=0;
            for(auto x:gains)ag+=x;ag/=period;
            for(auto x:losses)al+=x;al/=period;
            prev=cur;cur=al==0?100.0:100.0-100.0/(1.0+ag/al);
            double s=std::max(-5.0,std::min(5.0,cur-prev));
            if(!warmed){slope_ema=s;warmed=true;}
            else slope_ema=s*alpha+slope_ema*(1-alpha);
        }
    }
};

struct Params{
    int rsi_n=14; double rsi_thresh=0.5,drift_min=0.3;
    double sl_pts=0.8,tp_rr=2.5,trail_arm=1.5,trail_dist=0.8;
    int max_hold_s=300,cooldown_s=15;
};

struct Trade{double pnl,mfe;bool is_long;int held_s;char reason[8];};

struct Engine{
    Params p; TickRSI rsi;
    int ticks=0; bool pa=false,pl=false,pbe=false;
    double pe=0,psl=0,ptp=0,pm=0,ps=0.01,prev_drift=0;
    int64_t pts=0,cd=0;
    std::vector<Trade> trades;
    Engine(Params pp):p(pp),rsi(pp.rsi_n,5){}
    void tick(int64_t ms,double bid,double ask,double drift,double atr){
        ++ticks;double sp=ask-bid,mid=(bid+ask)/2;rsi.update(bid);
        if(pa){
            double mv=pl?(mid-pe):(pe-mid);if(mv>pm)pm=mv;
            double eff=pl?bid:ask,td=std::fabs(ptp-pe);
            if(!pbe&&td>0&&pm>=td*0.5){psl=pe;pbe=true;}
            if(mv>=p.trail_arm){
                double tsl=pl?(mid-p.trail_dist):(mid+p.trail_dist);
                if(pl&&tsl>psl)psl=tsl;if(!pl&&tsl<psl)psl=tsl;
            }
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,"TP",ms);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pbe?"BE":"SL",ms);return;}
            if((ms-pts)/1000>p.max_hold_s){close(eff,"TO",ms);return;}
            prev_drift=drift;return;
        }
        if(ticks<300||sp>0.40||atr<1.0||ms<cd||!rsi.warmed){prev_drift=drift;return;}
        bool rl=rsi.slope_ema>p.rsi_thresh,rs=rsi.slope_ema<-p.rsi_thresh;
        bool dl=drift>=p.drift_min,ds=drift<=-p.drift_min;
        bool gl=rl&&dl,gs=rs&&ds;
        if(!gl&&!gs){prev_drift=drift;return;}
        if(gl&&drift<prev_drift-0.10){prev_drift=drift;return;}
        if(gs&&drift>prev_drift+0.10){prev_drift=drift;return;}
        bool il=gl;double sl=p.sl_pts,tp=sl*p.tp_rr,e=il?ask:bid;
        pa=true;pl=il;pe=e;psl=il?e-sl:e+sl;ptp=il?e+tp:e-tp;
        pts=ms;pm=0;pbe=false;
        ps=std::min(0.10,std::max(0.01,std::round(10.0/(sl*100)/0.01)*0.01));
        prev_drift=drift;
    }
    void close(double ep,const char* r,int64_t ms){
        double pp=pl?(ep-pe):(pe-ep),pu=pp*ps*100;
        if(pu<0)cd=ms+(int64_t)p.cooldown_s*1000;
        Trade t;t.pnl=pu;t.mfe=pm;t.is_long=pl;
        t.held_s=(int)((ms-pts)/1000);
        strncpy(t.reason,r,7);t.reason[7]=0;trades.push_back(t);pa=false;
    }
    void fin(int64_t ms){if(pa)close(pe,"END",ms);}
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
    std::string line,tok;std::getline(f,line);
    int tc=-1,bc=-1,ac=-1,dc=-1,rc=-1,ci=0;
    std::istringstream hss(line);
    while(std::getline(hss,tok,',')){
        if(tok=="ts_ms"||tok=="timestamp_ms")tc=ci;
        if(tok=="bid")bc=ci;if(tok=="ask")ac=ci;
        if(tok=="ewm_drift")dc=ci;if(tok=="atr")rc=ci;++ci;
    }
    if(bc<0||ac<0){fprintf(stderr,"Bad header\n");return v;}
    double pb=0,av=2;std::deque<double> aw;
    v.reserve(300000);
    while(std::getline(f,line)){
        std::vector<std::string> c;std::istringstream ss(line);
        while(std::getline(ss,tok,','))c.push_back(tok);
        int mx=std::max({tc,bc,ac});if((int)c.size()<=mx)continue;
        try{
            Tick t;
            t.ms=tc>=0?(int64_t)std::stod(c[tc]):0;
            t.bid=std::stod(c[bc]);t.ask=std::stod(c[ac]);
            t.drift=dc>=0?std::stod(c[dc]):0;
            t.atr=rc>=0?std::stod(c[rc]):0;
            if(t.bid<=0||t.ask<t.bid)continue;
            if(t.atr<=0){
                if(pb>0){
                    double tr=t.ask-t.bid+std::fabs(t.bid-pb);
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

struct Res{double pnl,wr,lp,sp;int n;Params p;};

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: momentum_sweep <csv>");return 1;}
    auto t0=std::chrono::steady_clock::now();
    auto ticks=load(argv[1]);
    printf("Loaded %zu ticks\n",ticks.size());

    double rsi_t[]={0.3,0.5,0.8,1.2};
    double drft[]={0.2,0.3,0.5,0.8};
    double sl[]={0.6,0.8,1.0,1.5};
    double tp[]={2.0,2.5,3.0,4.0};
    double arm[]={1.0,1.5,2.0};
    double dst[]={0.5,0.8,1.0};
    int    mh[]={180,300,600};
    int    cd[]={10,20};
    int    rn[]={10,14,20};
    int total=(int)(sizeof(rsi_t)/8*sizeof(drft)/8*sizeof(sl)/8*sizeof(tp)/8*
                    sizeof(arm)/8*sizeof(dst)/8*sizeof(mh)/4*sizeof(cd)/4*sizeof(rn)/4);
    printf("Testing %d combinations...\n",total);

    std::vector<Res> results;results.reserve(total);
    int done=0;
    for(int ri:rn)for(double rt:rsi_t)for(double dm:drft)
    for(double s:sl)for(double t:tp)for(double a:arm)
    for(double d:dst)for(int m:mh)for(int c:cd){
        Params p;p.rsi_n=ri;p.rsi_thresh=rt;p.drift_min=dm;
        p.sl_pts=s;p.tp_rr=t;p.trail_arm=a;p.trail_dist=d;
        p.max_hold_s=m;p.cooldown_s=c;
        Engine eng(p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        if(!ticks.empty())eng.fin(ticks.back().ms);
        if(eng.n()>=5){
            Res r;r.pnl=eng.totpnl();r.wr=(double)eng.wins()/eng.n();
            r.lp=eng.lpnl();r.sp=eng.spnl();r.n=eng.n();r.p=p;
            results.push_back(r);
        }
        if(++done%5000==0){
            double best=0;for(auto& r:results)if(r.pnl>best)best=r.pnl;
            printf("  %d/%d best=$%+.2f\n",done,total,best);fflush(stdout);
        }
    }
    std::sort(results.begin(),results.end(),[](const Res&a,const Res&b){return a.pnl>b.pnl;});
    auto t1=std::chrono::steady_clock::now();
    printf("Done in %.2fs\n",std::chrono::duration<double>(t1-t0).count());

    puts("\n================================================================");
    puts("TOP 20");
    puts("================================================================");
    printf("%-3s %-8s %-6s %-4s %-7s %-7s %-5s %-5s %-5s %-4s %-4s %-4s\n",
           "#","PnL","WR","N","L$","S$","rsi_t","drift","sl","tp","arm","hold");
    int show=std::min((int)results.size(),20);
    for(int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+7.2f %5.1f%% %4d $%+6.2f $%+6.2f %5.2f %5.2f %5.2f %4.1f %4.1f %4d\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rsi_thresh,r.p.drift_min,r.p.sl_pts,r.p.tp_rr,r.p.trail_arm,r.p.max_hold_s);
    }
    if(!results.empty()){
        auto& b=results[0];
        printf("\nBEST: $%+.2f WR=%.1f%% N=%d L=$%+.2f S=$%+.2f\n",
               b.pnl,b.wr*100,b.n,b.lp,b.sp);
        printf("rsi_n=%d thresh=%.2f drift=%.2f sl=%.2f tp=%.1f arm=%.1f dist=%.1f hold=%d cd=%d\n",
               b.p.rsi_n,b.p.rsi_thresh,b.p.drift_min,b.p.sl_pts,b.p.tp_rr,
               b.p.trail_arm,b.p.trail_dist,b.p.max_hold_s,b.p.cooldown_s);
        // Rerun best with trade detail
        Engine eng(b.p);
        for(auto& tk:ticks)eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);
        if(!ticks.empty())eng.fin(ticks.back().ms);
        puts("\nAll trades:");
        for(auto& t:eng.trades)
            printf("  %-5s $%+6.2f mfe=%5.2f %-4s %ds\n",
                   t.is_long?"LONG":"SHORT",t.pnl,t.mfe,t.reason,t.held_s);
    }
    return 0;
}
