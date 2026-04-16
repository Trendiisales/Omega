// duka_sweep.cpp — 2-year CRTP CFE sweep on Dukascopy data
// Handles both formats:
//   Dukascopy: timestamp_ms,ask,bid,ask_vol,bid_vol  (computes drift internally)
//   l2_ticks:  ts_ms,bid,ask,...,ewm_drift           (uses pre-computed drift)
//
// Internal drift: EWM of mid price changes, alpha=0.1 (~10-tick halflife)
// This matches Omega's ewm_drift computation at tick level.
//
// Build: clang++ -O3 -std=c++20 -o /tmp/duka_sweep /tmp/duka_sweep.cpp
// Run:   /tmp/duka_sweep /Users/jo/Tick/yr1_2y.csv
//        /tmp/duka_sweep ~/Downloads/l2_ticks_2026-04-09.csv ~/Downloads/l2_ticks_2026-04-10.csv ...

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ─────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; float bid, ask, drift, atr; };

// ─── CSV loader — handles both formats ───────────────────────────────────────
static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return out; }

    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();

    int cm=-1, cb=-1, ca=-1, cd=-1, cr=-1, ci=0;
    { std::istringstream h(line);
      while (std::getline(h,tok,',')) {
        if (tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if (tok=="bid")       cb=ci;
        if (tok=="ask")       ca=ci;
        if (tok=="ewm_drift") cd=ci;
        if (tok=="atr")       cr=ci;
        ++ci; } }

    // Dukascopy: timestamp_ms,ask,bid -- ask before bid
    bool duka_format = (cb<0 && ca<0);
    if (duka_format) {
        ci=0; std::istringstream h2(line);
        while (std::getline(h2,tok,',')) {
            if (tok=="timestamp_ms") cm=ci;
            if (tok=="ask") ca=ci;
            if (tok=="bid") cb=ci;
            ++ci;
        }
    }
    if (cb<0||ca<0) { fprintf(stderr,"No bid/ask in %s\n",path); return out; }

    // ATR fallback state
    float pb=0, av=2.f;
    std::deque<float> aw;

    // Internal drift computation (EWM of mid changes, alpha=0.1)
    // Matches Omega's ewm_drift at tick granularity
    float drift_ema = 0.f;
    float prev_mid  = 0.f;
    static constexpr float DRIFT_ALPHA = 0.10f;

    out.reserve(500000);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);
        static const char* flds[32];
        int nf=0; flds[nf++]=buf;
        for (char* c=buf;*c;++c) if(*c==','){*c='\0';if(nf<32)flds[nf++]=c+1;}
        int need=cb>ca?cb:ca; if(cm>need)need=cm;
        if (nf<=need) continue;
        char* e;
        Tick t;
        t.ms    = (cm>=0) ? (int64_t)strtod(flds[cm],&e) : 0;
        t.bid   = (float)strtod(flds[cb],&e);
        t.ask   = (float)strtod(flds[ca],&e);
        t.drift = (cd>=0&&cd<nf) ? (float)strtod(flds[cd],&e) : 0.f;
        t.atr   = (cr>=0&&cr<nf) ? (float)strtod(flds[cr],&e) : 0.f;
        if (t.bid<=0||t.ask<t.bid) continue;

        float mid = (t.bid+t.ask)*0.5f;

        // Compute drift internally when not in file
        if (cd<0) {
            if (prev_mid>0.f) {
                float chg = mid - prev_mid;
                drift_ema = DRIFT_ALPHA*chg + (1.f-DRIFT_ALPHA)*drift_ema;
            }
            t.drift = drift_ema;
        }
        prev_mid = mid;

        // ATR fallback
        if (t.atr<=0.f) {
            if (pb>0.f) {
                float tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
                aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                if((int)aw.size()>=50){float s=0;for(float x:aw)s+=x;av=s/aw.size()*14.f;}
            }
            t.atr=av;
        }
        pb=t.bid;
        out.push_back(t);
    }
    return out;
}

static std::vector<Tick> load_files(int argc, char* argv[]) {
    std::vector<Tick> all;
    for (int i=1;i<argc;++i) {
        // directory?
        DIR* td=opendir(argv[i]);
        if (td) {
            closedir(td);
            struct dirent* ent;
            DIR* d=opendir(argv[i]);
            std::vector<std::string> paths;
            while ((ent=readdir(d))) {
                std::string n=ent->d_name;
                if ((n.find("l2_ticks_")==0||n.find("XAUUSD_")==0) &&
                    n.size()>4 && n.substr(n.size()-4)==".csv" &&
                    n.find("part")==std::string::npos)
                    paths.push_back(std::string(argv[i])+"/"+n);
            }
            closedir(d);
            std::sort(paths.begin(),paths.end());
            for (auto& p:paths) {
                auto v=load_csv(p.c_str());
                if (v.size()<1000){fprintf(stderr,"Skip %s (%zu)\n",p.c_str(),v.size());continue;}
                fprintf(stderr,"Loaded %s: %zu ticks\n",p.c_str(),v.size());
                all.insert(all.end(),v.begin(),v.end());
            }
        } else {
            auto v=load_csv(argv[i]);
            if (v.size()<1000){fprintf(stderr,"Skip %s (%zu)\n",argv[i],v.size());continue;}
            fprintf(stderr,"Loaded %s: %zu ticks\n",argv[i],v.size());
            all.insert(all.end(),v.begin(),v.end());
        }
    }
    std::stable_sort(all.begin(),all.end(),[](const Tick&a,const Tick&b){return a.ms<b.ms;});
    return all;
}

// ─── Params ───────────────────────────────────────────────────────────────────
struct P {
    int rn; float rt,rm,dm,st; int64_t sm,cd,mh;
    float sl,tp,ta,td; bool tr; int warmup;
};

// ─── CRTP engine with static ring buffers ─────────────────────────────────────
template<typename D>
struct EngBase {
    static constexpr int RMAX=32, MMAX=10;
    float rg[RMAX]={},rl[RMAX]={};
    int rhead=0,rcount=0;
    float rpm=0,rc=50,rp=50,rv=0; bool rw=false; float ra;
    float mb[MMAX]={}; int mhead=0,mcount=0;
    int dsd=0; int64_t dss=0;
    bool pa=false,pl=false,pbe=false;
    float pe=0,psl=0,ptp=0,pm=0,ps=0.01f;
    int64_t pts=0,cdu=0,lcm=0; int lcd=0,wt=0;
    bool ab=false; float lex=0; int lld=0;
    float tot=0,lp_=0,sp_=0; int nt=0,nw=0;

    explicit EngBase(float a):ra(a){}

    void ursi(float mid) {
        if(!rpm){rpm=mid;return;}
        float c=mid-rpm; rpm=mid;
        int n=static_cast<D*>(this)->rn();
        rg[rhead]=c>0?c:0.f; rl[rhead]=c<0?-c:0.f;
        rhead=(rhead+1)%RMAX;
        if(rcount<n)++rcount;
        if(rcount>=n){
            float ag=0,al=0;
            for(int i=0;i<n;++i){
                int idx=(rhead-1-i+RMAX)%RMAX;
                ag+=rg[idx]; al+=rl[idx];
            }
            ag/=n; al/=n;
            rp=rc; rc=al==0?100.f:100.f-100.f/(1.f+ag/al);
            float s=rc-rp;
            if(!rw){rv=s;rw=true;} else rv=s*ra+rv*(1-ra);
        }
    }

    void close(float ep,bool il,int64_t ms,const P& p){
        float pp=il?(ep-pe):(pe-ep),pu=pp*ps*100.f;
        tot+=pu;++nt;if(pu>0)++nw;
        if(il)lp_+=pu;else sp_+=pu;
        lcd=il?1:-1;lcm=ms;
        if(pu<0){ab=true;lex=ep;lld=il?1:-1;cdu=ms+p.cd;}
        pa=false;
    }

    void tick(int64_t ms,float bid,float ask,float drift,float atr,const P& p){
        float mid=(bid+ask)*.5f,sp=ask-bid;
        ++wt; ursi(mid);
        mb[mhead%MMAX]=mid;++mhead;if(mcount<MMAX)++mcount;

        if(drift>=p.st){if(dsd!=1){dsd=1;dss=ms;}}
        else if(drift<=-p.st){if(dsd!=-1){dsd=-1;dss=ms;}}
        else{dsd=0;dss=0;}
        int64_t dsms=dsd?(ms-dss):0;

        if(pa){
            float mv=pl?(mid-pe):(pe-mid);if(mv>pm)pm=mv;
            float eff=pl?bid:ask,td_=std::fabs(ptp-pe);
            if(!pbe&&td_>0&&mv>=td_*.5f){psl=pe;pbe=true;}
            if(p.tr&&mv>=p.ta){
                float tsl=pl?(mid-p.td):(mid+p.td);
                if(pl&&tsl>psl)psl=tsl;
                if(!pl&&tsl<psl)psl=tsl;
            }
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,pl,ms,p);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pl,ms,p);return;}
            if(ms-pts>p.mh){close(eff,pl,ms,p);return;}
            return;
        }

        if(wt<p.warmup||sp>0.40f||atr<0.5f) return;
        if(ms<cdu) return;
        if(!rw) return;

        int rd=0;
        if(rv>p.rt&&rv<p.rm) rd=1;
        else if(rv<-p.rt&&rv>-p.rm) rd=-1;
        if(!rd) return;
        if(rd==1&&drift<0) return;
        if(rd==-1&&drift>0) return;
        if(std::fabs(drift)<p.dm) return;
        if(dsms<p.sm) return;

        if(mcount>=3){
            float n3=mb[(mhead-3+MMAX*2)%MMAX];
            float mv3=mid-n3;
            if(rd==1&&mv3<-atr*.3f) return;
            if(rd==-1&&mv3>atr*.3f) return;
        }
        if(lcd&&(ms-lcm)<45000LL&&rd!=lcd) return;
        if(ab&&lld){
            float dist=(lld==1)?(lex-mid):(mid-lex);
            bool same=(rd==1&&lld==1)||(rd==-1&&lld==-1);
            if(same&&dist>atr*.4f) return; else ab=false;
        }

        bool il=(rd==1);
        float sl_=atr*p.sl,tp_=sl_*p.tp,e=il?ask:bid;
        pa=true;pl=il;pe=e;pm=0;pbe=false;
        psl=il?(e-sl_):(e+sl_);ptp=il?(e+tp_):(e-tp_);pts=ms;
        float raw=10.f/(sl_*100.f);
        ps=std::min(.10f,std::max(.01f,std::round(raw/.01f)*.01f));
    }
    void fin(int64_t ms,const P& p){if(pa)close(pe,pl,ms,p);}
};

struct Eng:EngBase<Eng>{
    int _rn;
    explicit Eng(const P& p):EngBase<Eng>(2.f/(p.rn+1)),_rn(p.rn){}
    int rn()const{return _rn;}
};

struct Res{float pnl,wr,lp,sp;int n;P p;};

static void print_top(const char* title,std::vector<Res>& R,int days){
    std::sort(R.begin(),R.end(),[](const Res&a,const Res&b){return a.pnl>b.pnl;});
    printf("\n%s\n%s (%d days)\n%s\n",
           std::string(76,'=').c_str(),title,days,std::string(76,'=').c_str());
    printf("%-3s %-9s %-7s %-5s %-7s %-7s  rn  rt   dm   st  sm   sl   tp  mh\n",
           "#","PnL","PnL/day","N","L$","S$");
    int show=std::min((int)R.size(),20);
    for(int i=0;i<show;++i){
        auto& r=R[i];
        printf("%-3d $%+8.2f $%+6.2f %5d $%+6.2f $%+6.2f"
               "  %2d  %.1f  %.1f  %.1f  %2lds  %.2f  %.1f  %3lds\n",
               i+1,r.pnl,(float)r.pnl/days,r.n,r.lp,r.sp,
               r.p.rn,(double)r.p.rt,(double)r.p.dm,(double)r.p.st,
               (long)(r.p.sm/1000),(double)r.p.sl,(double)r.p.tp,
               (long)(r.p.mh/1000));
    }
    if(!R.empty()){
        auto& b=R[0];
        printf("\nBEST/DAY: $%+.2f  WR=%.1f%%  N=%d total (%d/day avg)\n",
               (float)b.pnl/days,b.wr*100,b.n,b.n/days);
        printf("rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f tp=%.1f mh=%lds trail=%s\n",
               b.p.rn,(double)b.p.rt,(double)b.p.dm,(double)b.p.st,
               (long)(b.p.sm/1000),(double)b.p.sl,(double)b.p.tp,
               (long)(b.p.mh/1000),b.p.tr?"Y":"N");
    }
    // sm breakdown
    printf("\nEdge by sm:\n");
    for(int64_t sv:{15000,20000,30000,60000}){
        float best=-1e9f;int bn=0;float bwr=0;
        for(auto& r:R)if(r.p.sm==sv&&r.pnl>best){best=r.pnl;bn=r.n;bwr=r.wr;}
        if(bn>0)printf("  sm=%2lds  $%+.2f/day  WR=%.1f%%  N=%d\n",
                        (long)(sv/1000),(float)best/days,bwr*100,bn);
    }
}

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: duka_sweep <csv_or_dir> [more_files...]");return 1;}

    auto t0=std::chrono::steady_clock::now();
    auto ticks=load_files(argc,argv);
    if(ticks.empty()){fprintf(stderr,"No ticks\n");return 1;}

    int days=1; int64_t pd=ticks[0].ms/86400000LL;
    for(auto& t:ticks){int64_t d=t.ms/86400000LL;if(d!=pd){++days;pd=d;}}
    printf("Total: %zu ticks across ~%d days\n",ticks.size(),days);

    // Check drift availability
    int with_drift=0;
    for(int i=0;i<std::min((int)ticks.size(),1000);++i)
        if(ticks[i].drift!=0.f)++with_drift;
    printf("Drift source: %s\n", with_drift>10?"pre-computed (l2_ticks)":"computed internally (Dukascopy)");

    // ── CFE grid ─────────────────────────────────────────────────────────────
    // Wider search: include longer sm values for 2-year data
    const int    rn_v[] ={20,30};
    const float  rt_v[] ={1.0f,2.0f,3.0f,4.0f};
    const float  rm_v[] ={15.0f};
    const float  dm_v[] ={0.1f,0.2f,0.3f,0.5f};
    const float  st_v[] ={0.2f,0.3f,0.5f};
    const int64_t sm_v[]={15000,30000,60000,120000}; // 15s to 2min
    const float  sl_v[] ={0.30f,0.40f,0.50f};
    const float  tp_v[] ={2.0f,3.0f,4.0f};
    const int64_t cd_v[]={15000};
    const int64_t mh_v[]={300000,600000};
    const bool   tr_v[] ={false,true};
    const int    wu_v[] ={300};

    long TOTAL=2*4*1*4*3*4*3*3*1*2*2*1; // 10368
    printf("\nCFE sweep: %ld combinations...\n",TOTAL);

    std::vector<Res> cfe_res; cfe_res.reserve(15000);
    long done=0;

    for(int rn:rn_v)
    for(float rt:rt_v)
    for(float rm_:rm_v)
    for(float dm:dm_v)
    for(float st:st_v)
    for(int64_t sm:sm_v)
    for(float sl:sl_v)
    for(float tp:tp_v)
    for(int64_t cd:cd_v)
    for(int64_t mh:mh_v)
    for(bool tr:tr_v)
    for(int wu:wu_v)
    {
        P pp{rn,rt,rm_,dm,st,sm,cd,mh,sl,tp,2.f,1.f,tr,wu};
        Eng eng(pp);
        for(auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr,pp);
        if(!ticks.empty()) eng.fin(ticks.back().ms,pp);
        if(eng.nt>=10){
            Res r; r.pnl=eng.tot; r.wr=(float)eng.nw/eng.nt;
            r.lp=eng.lp_; r.sp=eng.sp_; r.n=eng.nt; r.p=pp;
            cfe_res.push_back(r);
        }
        if(++done%2000==0){
            float best=0;for(auto& r:cfe_res)if(r.pnl>best)best=r.pnl;
            printf("  CFE %ld/%ld  best=$%+.2f\n",done,TOTAL,best);fflush(stdout);
        }
    }

    auto t1=std::chrono::steady_clock::now();
    printf("Done in %.1fs\n",std::chrono::duration<double>(t1-t0).count());

    print_top("CFE RESULTS",cfe_res,days);
    return 0;
}
