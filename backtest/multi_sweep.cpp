// multi_sweep.cpp — multi-day CFE+TSE CRTP sweep using static arrays
// Reads all l2_ticks_*.csv from a directory, concatenates, sweeps both grids.
// Build: clang++ -O3 -std=c++20 -o /tmp/multi_sweep /tmp/multi_sweep.cpp
// Run:   /tmp/multi_sweep ~/Downloads
//
// CFE grid: sweep-validated params + neighbours
// TSE grid: new params (drift_min=0.50, sus_ms=15s) + neighbours

#include <algorithm>
#include <array>
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

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; float bid, ask, drift, atr; };

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line, tok;
    std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,cr=-1,ci=0;
    { std::istringstream h(line);
      while (std::getline(h,tok,',')) {
        if (tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if (tok=="bid")       cb=ci;
        if (tok=="ask")       ca=ci;
        if (tok=="ewm_drift") cd=ci;
        if (tok=="atr")       cr=ci;
        ++ci; } }
    if (cb<0||ca<0) return out;
    float pb=0,av=2.0f; std::deque<float> aw;
    out.reserve(200000);
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        if (line.back()=='\r') line.pop_back();
        static char buf[512];
        if (line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        // fast field split
        static const char* flds[32];
        int nf=0; flds[nf++]=buf;
        for (char* c=buf;*c;++c) if(*c==','){*c='\0';if(nf<32)flds[nf++]=c+1;}
        if (nf<=std::max({cm,cb,ca})) continue;
        Tick t;
        char* e;
        t.ms    = (cm>=0) ? (int64_t)strtod(flds[cm],&e) : 0;
        t.bid   = (float)strtod(flds[cb],&e);
        t.ask   = (float)strtod(flds[ca],&e);
        t.drift = (cd>=0&&cd<nf) ? (float)strtod(flds[cd],&e) : 0.f;
        t.atr   = (cr>=0&&cr<nf) ? (float)strtod(flds[cr],&e) : 0.f;
        if (t.bid<=0||t.ask<t.bid) continue;
        if (t.atr<=0.f) {
            if (pb>0.f) {
                float tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
                aw.push_back(tr); if((int)aw.size()>200)aw.pop_front();
                if((int)aw.size()>=50){float s=0;for(float x:aw)s+=x;av=s/aw.size()*14.f;}
            }
            t.atr=av;
        }
        pb=t.bid; out.push_back(t);
    }
    return out;
}

static std::vector<Tick> load_dir(const char* dir) {
    std::vector<std::string> paths;
    DIR* d = opendir(dir);
    if (!d) { fprintf(stderr,"Cannot open dir %s\n",dir); return {}; }
    struct dirent* ent;
    while ((ent=readdir(d))) {
        std::string n=ent->d_name;
        if (n.find("l2_ticks_")==0 && n.size()>4 && n.substr(n.size()-4)==".csv") {
            // skip part files and tiny files
            if (n.find("part")!=std::string::npos) continue;
            paths.push_back(std::string(dir)+"/"+n);
        }
    }
    closedir(d);
    std::sort(paths.begin(),paths.end());
    std::vector<Tick> all;
    for (auto& p:paths) {
        auto v=load_csv(p.c_str());
        if (v.size()<1000) { fprintf(stderr,"Skip %s (%zu ticks)\n",p.c_str(),v.size()); continue; }
        fprintf(stderr,"Loaded %s: %zu ticks\n",p.c_str(),v.size());
        all.insert(all.end(),v.begin(),v.end());
    }
    // sort by timestamp (files already sorted by name = date, but just in case)
    std::stable_sort(all.begin(),all.end(),[](const Tick&a,const Tick&b){return a.ms<b.ms;});
    return all;
}

// ─── Params ──────────────────────────────────────────────────────────────────
struct P {
    int     rn;
    float   rt,rm,dm,st;
    int64_t sm,cd,mh;
    float   sl,tp,ta,td;
    bool    tr;
    int     warmup;
};

// ─── CRTP engine using static arrays instead of deques ───────────────────────
template<typename D>
struct EngBase {
    // RSI state — static ring buffer, no allocation
    static constexpr int RSI_MAX = 32;
    float rg[RSI_MAX]={}, rl[RSI_MAX]={};
    int   rhead=0, rcount=0;
    float rpm=0,rc=50,rp=50,rv=0; bool rw=false; float ra;

    // mid buffer — last 10
    static constexpr int MID_MAX = 10;
    float mb[MID_MAX]={}; int mhead=0, mcount=0;

    // drift sustained
    int dsd=0; int64_t dss=0;

    // position
    bool pa=false,pl=false,pbe=false;
    float pe=0,psl=0,ptp=0,pm=0,ps=0.01f;
    int64_t pts=0,cdu=0,lcm=0; int lcd=0,wt=0;
    bool ab=false; float lex=0; int lld=0;

    // results
    float tot=0,lp_=0,sp_=0; int nt=0,nw=0;

    explicit EngBase(float a):ra(a){}

    // ring-buffer RSI (no allocation)
    void ursi(float mid) {
        if (!rpm){rpm=mid;return;}
        float c=mid-rpm; rpm=mid;
        int n=static_cast<D*>(this)->rn();
        rg[rhead]=c>0?c:0.f;
        rl[rhead]=c<0?-c:0.f;
        rhead=(rhead+1)%RSI_MAX;
        if(rcount<n) ++rcount;
        if(rcount>=n){
            float ag=0,al=0;
            for(int i=0;i<n;++i){
                int idx=(rhead-1-i+RSI_MAX)%RSI_MAX;
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
        mb[mhead%MID_MAX]=mid; ++mhead; if(mcount<MID_MAX)++mcount;

        if(drift>=p.st){if(dsd!=1){dsd=1;dss=ms;}}
        else if(drift<=-p.st){if(dsd!=-1){dsd=-1;dss=ms;}}
        else{dsd=0;dss=0;}
        int64_t dsms=dsd?(ms-dss):0;

        if(pa){
            float mv=pl?(mid-pe):(pe-mid);if(mv>pm)pm=mv;
            float eff=pl?bid:ask,td_=std::fabs(ptp-pe);
            if(!pbe&&td_>0&&mv>=td_*.5f){psl=pe;pbe=true;}
            if(p.tr&&mv>=p.ta){float tsl=pl?(mid-p.td):(mid+p.td);
                if(pl&&tsl>psl)psl=tsl;
                if(!pl&&tsl<psl)psl=tsl;}
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,pl,ms,p);return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pl,ms,p);return;}
            if(ms-pts>p.mh){close(eff,pl,ms,p);return;}
            return;
        }

        if(wt<p.warmup||sp>0.40f||atr<1.f) return;
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
            float n3=mb[(mhead-3+MID_MAX*2)%MID_MAX];
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

// ─── Print top 20 ─────────────────────────────────────────────────────────────
static void print_top(const char* title, std::vector<Res>& results, int days) {
    std::sort(results.begin(),results.end(),[](const Res&a,const Res&b){return a.pnl>b.pnl;});
    printf("\n%s\n%s (%d days)\n%s\n",
           std::string(76,'=').c_str(), title, days, std::string(76,'=').c_str());
    printf("%-3s %-9s %-6s %-4s %-7s %-7s  rn  rt   dm   st  sm   sl   tp  mh   wu\n",
           "#","PnL","WR","N","L$","S$");
    int show=std::min((int)results.size(),20);
    for(int i=0;i<show;++i){
        auto& r=results[i];
        printf("%-3d $%+8.2f %5.1f%% %4d $%+6.2f $%+6.2f"
               "  %2d  %.1f  %.1f  %.1f  %2lds  %.2f  %.1f  %3lds  %d\n",
               i+1,r.pnl,r.wr*100,r.n,r.lp,r.sp,
               r.p.rn,(double)r.p.rt,(double)r.p.dm,(double)r.p.st,
               (long)(r.p.sm/1000),(double)r.p.sl,(double)r.p.tp,
               (long)(r.p.mh/1000),r.p.warmup);
    }
    if(!results.empty()){
        auto& b=results[0];
        printf("\nBEST: $%+.2f/day  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n",
               b.pnl/days,b.wr*100,b.n,b.lp,b.sp);
        printf("rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f tp=%.1f mh=%ldm warmup=%d trail=%s\n",
               b.p.rn,(double)b.p.rt,(double)b.p.dm,(double)b.p.st,
               (long)(b.p.sm/1000),(double)b.p.sl,(double)b.p.tp,
               (long)(b.p.mh/60000),b.p.warmup,b.p.tr?"Y":"N");
    }
}

int main(int argc,char* argv[]){
    if(argc<2){puts("Usage: multi_sweep file1.csv file2.csv ...");return 1;}

    auto t0=std::chrono::steady_clock::now();
    // Accept either a directory or individual files
    std::vector<Tick> ticks;
    for(int i=1;i<argc;++i){
        // check if it's a directory
        DIR* td=opendir(argv[i]);
        if(td){closedir(td);auto v=load_dir(argv[i]);ticks.insert(ticks.end(),v.begin(),v.end());}
        else{
            auto v=load_csv(argv[i]);
            if(v.size()<1000){fprintf(stderr,"Skip %s (%zu ticks)\n",argv[i],v.size());continue;}
            fprintf(stderr,"Loaded %s: %zu ticks\n",argv[i],v.size());
            ticks.insert(ticks.end(),v.begin(),v.end());
        }
    }
    if(ticks.empty()){fprintf(stderr,"No ticks loaded\n");return 1;}
    std::stable_sort(ticks.begin(),ticks.end(),[](const Tick&a,const Tick&b){return a.ms<b.ms;});

    // Count unique days
    int days=1; int64_t prev_day=ticks[0].ms/86400000LL;
    for(auto& t:ticks){int64_t d=t.ms/86400000LL;if(d!=prev_day){++days;prev_day=d;}}
    printf("Total: %zu ticks across ~%d days\n",ticks.size(),days);

    // ── CFE grid (around current live params + wider search) ─────────────────
    // Current live: rn=30 rt=2.0 dm=0.30 st=0.50 sm=30s sl=0.30 tp=3.0 mh=10m
    const int    cfe_rn[] ={20,30};
    const float  cfe_rt[] ={1.0f,2.0f,3.0f};
    const float  cfe_rm[] ={15.0f};
    const float  cfe_dm[] ={0.2f,0.3f,0.5f};
    const float  cfe_st[] ={0.3f,0.5f};
    const int64_t cfe_sm[]={15000,20000,30000};
    const float  cfe_sl[] ={0.25f,0.30f,0.40f};
    const float  cfe_tp[] ={2.0f,3.0f,4.0f};
    const int64_t cfe_cd[]={15000};
    const int64_t cfe_mh[]={300000,600000};
    const bool   cfe_tr[] ={false,true};
    const int    cfe_wu[] ={300};

    long cfe_total=2*3*1*3*2*3*3*3*1*2*2*1;
    printf("\nCFE: testing %ld combinations...\n",cfe_total);

    std::vector<Res> cfe_results; cfe_results.reserve(5000);
    long done=0;

    for(int rn:cfe_rn)
    for(float rt:cfe_rt)
    for(float rm_:cfe_rm)
    for(float dm:cfe_dm)
    for(float st:cfe_st)
    for(int64_t sm:cfe_sm)
    for(float sl:cfe_sl)
    for(float tp:cfe_tp)
    for(int64_t cd:cfe_cd)
    for(int64_t mh:cfe_mh)
    for(bool tr:cfe_tr)
    for(int wu:cfe_wu)
    {
        P pp{rn,rt,rm_,dm,st,sm,cd,mh,sl,tp,2.f,1.f,tr,wu};
        Eng eng(pp);
        for(auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr,pp);
        if(!ticks.empty()) eng.fin(ticks.back().ms,pp);
        if(eng.nt>=5){
            Res r; r.pnl=eng.tot; r.wr=(float)eng.nw/eng.nt;
            r.lp=eng.lp_; r.sp=eng.sp_; r.n=eng.nt; r.p=pp;
            cfe_results.push_back(r);
        }
        if(++done%500==0){
            float best=0; for(auto& r:cfe_results)if(r.pnl>best)best=r.pnl;
            printf("  CFE %ld/%ld  best=$%+.2f\n",done,cfe_total,best); fflush(stdout);
        }
    }

    // ── TSE grid (around new live params: dm=0.50, sm=15s) ────────────────────
    // Current live: rn=14(RSI-14 reused) rt=2.0 dm=0.50 st=0.50 sm=15s sl=0.30 tp=3.0
    const int    tse_rn[] ={14,20};
    const float  tse_rt[] ={1.0f,2.0f,3.0f};
    const float  tse_rm[] ={15.0f};
    const float  tse_dm[] ={0.3f,0.5f,0.8f};
    const float  tse_st[] ={0.3f,0.5f};
    const int64_t tse_sm[]={10000,15000,20000};
    const float  tse_sl[] ={0.25f,0.30f,0.40f};
    const float  tse_tp[] ={2.0f,3.0f,4.0f};
    const int64_t tse_cd[]={10000,15000};
    const int64_t tse_mh[]={60000,120000};
    const bool   tse_tr[] ={false,true};
    const int    tse_wu[] ={60};

    long tse_total=2*3*1*3*2*3*3*3*2*2*2*1;
    printf("\nTSE: testing %ld combinations...\n",tse_total);

    std::vector<Res> tse_results; tse_results.reserve(5000);
    done=0;

    for(int rn:tse_rn)
    for(float rt:tse_rt)
    for(float rm_:tse_rm)
    for(float dm:tse_dm)
    for(float st:tse_st)
    for(int64_t sm:tse_sm)
    for(float sl:tse_sl)
    for(float tp:tse_tp)
    for(int64_t cd:tse_cd)
    for(int64_t mh:tse_mh)
    for(bool tr:tse_tr)
    for(int wu:tse_wu)
    {
        P pp{rn,rt,rm_,dm,st,sm,cd,mh,sl,tp,2.f,1.f,tr,wu};
        Eng eng(pp);
        for(auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr,pp);
        if(!ticks.empty()) eng.fin(ticks.back().ms,pp);
        if(eng.nt>=5){
            Res r; r.pnl=eng.tot; r.wr=(float)eng.nw/eng.nt;
            r.lp=eng.lp_; r.sp=eng.sp_; r.n=eng.nt; r.p=pp;
            tse_results.push_back(r);
        }
        if(++done%500==0){
            float best=0; for(auto& r:tse_results)if(r.pnl>best)best=r.pnl;
            printf("  TSE %ld/%ld  best=$%+.2f\n",done,tse_total,best); fflush(stdout);
        }
    }

    auto t1=std::chrono::steady_clock::now();
    printf("\nDone in %.2fs\n",std::chrono::duration<double>(t1-t0).count());

    print_top("CFE RESULTS", cfe_results, days);
    print_top("TSE RESULTS", tse_results, days);

    // ── sm edge breakdown for both engines ────────────────────────────────────
    printf("\nCFE edge by sm:\n");
    for(int64_t sv:{15000,20000,30000}){
        float best=-1e9f; int bn=0; float bwr=0;
        for(auto& r:cfe_results)if(r.p.sm==sv&&r.pnl>best){best=r.pnl;bn=r.n;bwr=r.wr;}
        if(bn>0) printf("  sm=%2lds  $%+.2f/day  WR=%.1f%%  N=%d\n",(long)(sv/1000),best/days,bwr*100,bn);
    }
    printf("\nTSE edge by sm:\n");
    for(int64_t sv:{10000,15000,20000}){
        float best=-1e9f; int bn=0; float bwr=0;
        for(auto& r:tse_results)if(r.p.sm==sv&&r.pnl>best){best=r.pnl;bn=r.n;bwr=r.wr;}
        if(bn>0) printf("  sm=%2lds  $%+.2f/day  WR=%.1f%%  N=%d\n",(long)(sv/1000),best/days,bwr*100,bn);
    }

    return 0;
}
