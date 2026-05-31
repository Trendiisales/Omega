// =============================================================================
// fx_cross_reversion.cpp -- FX cross-pair mean-reversion edge scan (S43, FX-native)
//
// THESIS: crosses of macro-correlated majors (EURGBP, AUDNZD) carry no drift
// and mean-revert -- the two legs share drivers, so the cross ranges. This is
// an FX-native edge (no gold/index analogue). Tested long AND short (FX has no
// risk-premium drift, so long-only is wrong). Cost-gated on realistic retail
// cross spread.
//
// INSTRUMENTS:
//   EURGBP  -- real tradeable cross (direct H1 file), HS ~0.8bp -> ~1.0 retail
//   AUDNZD  -- synthesised = AUDUSD / NZDUSD (real file lands with Dukascopy);
//              pays BOTH legs' spread (0.8 + 1.0 bp) as the cost proxy.
//
// FIDELITY (mirrors xsym_edge_scan.cpp, the trusted harness):
//   cross-spread fills (enter@mid+/-HS, exit@mid-/+HS); cost 2*HS per RT;
//   ALL z-score state uses bars <= i (no lookahead); WF split at 50%;
//   6 equal-count blocks for regime consistency. n>=15 each half + blk+>=5/6.
//
// BUILD: c++ -std=c++17 -O2 backtest/fx_cross_reversion.cpp -o backtest/fx_cross_reversion
// RUN:   ./backtest/fx_cross_reversion
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

struct Bar { int64_t ts; double o,h,l,c; };

static std::vector<Bar> load_h1(const char* path){
    std::vector<Bar> v; FILE* f=fopen(path,"r"); if(!f){perror(path);return v;}
    char line[256]; bool first=true;
    while(fgets(line,sizeof line,f)){
        if(first){first=false; if(line[0]<'0'||line[0]>'9') continue;}
        Bar b{}; double ts;
        if(sscanf(line,"%lf,%lf,%lf,%lf,%lf",&ts,&b.o,&b.h,&b.l,&b.c)!=5) continue;
        if(b.c<=0) continue;
        b.ts=(ts>1e11)?(int64_t)(ts/1000.0):(int64_t)ts;   // Dukascopy ms OR legacy sec
        time_t t=(time_t)b.ts; struct tm g; gmtime_r(&t,&g);
        if(g.tm_wday==6) continue;                          // drop flat Saturday artifact (D1)
        v.push_back(b);
    }
    fclose(f); return v;
}

// Inner-join two H1 series on ts -> aligned close vectors.
static void align(const std::vector<Bar>&A,const std::vector<Bar>&B,
                  std::vector<int64_t>&ts,std::vector<double>&ca,std::vector<double>&cb){
    size_t i=0,j=0;
    while(i<A.size()&&j<B.size()){
        if(A[i].ts==B[j].ts){ ts.push_back(A[i].ts); ca.push_back(A[i].c); cb.push_back(B[j].c); ++i; ++j; }
        else if(A[i].ts<B[j].ts) ++i; else ++j;
    }
}

static int64_t SPLIT_TS=0, TS_MIN=0, TS_MAX=1;
static inline int blockOf(int64_t ts){ int b=(int)(6.0*(ts-TS_MIN)/(double)(TS_MAX-TS_MIN+1)); return b<0?0:(b>5?5:b); }

struct Stats {
    int n[3]={0,0,0}, win[3]={0,0,0};
    double gw[3]={0,0,0}, gl[3]={0,0,0}, g[3]={0,0,0}, peak=0, eq=0, mdd=0;
    double blk_g[6]={0}; int blk_n[6]={0};
    void add(double pnl,int64_t ts){
        int half=(ts<SPLIT_TS)?1:2;
        for(int k:{0,half}){ n[k]++; g[k]+=pnl; if(pnl>0){win[k]++;gw[k]+=pnl;} else gl[k]+=-pnl; }
        int b=blockOf(ts); blk_g[b]+=pnl; blk_n[b]++;
        eq+=pnl; if(eq>peak)peak=eq; if(peak-eq>mdd)mdd=peak-eq;
    }
    double pf(int k)const{ return gl[k]>0? gw[k]/gl[k] : (gw[k]>0?99:0); }
    double wr(int k)const{ return n[k]? 100.0*win[k]/n[k]:0; }
    int blocks_pos()const{ int p=0; for(int i=0;i<6;i++) if(blk_g[i]>0)p++; return p; }
    bool robust()const{ return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15&&blocks_pos()>=5; }
    bool wfpos()const{ return g[1]>0&&g[2]>0&&n[1]>=15&&n[2]>=15; }
};

static void report(const std::string& tag,const Stats& s){
    printf("  %-34s n=%-4d WR=%4.1f PF=%.2f g=%+8.1f mdd=%6.1f | H1 PF=%.2f | H2 PF=%.2f | blk+=%d/6 %s\n",
        tag.c_str(),s.n[0],s.wr(0),s.pf(0),s.g[0],s.mdd,s.pf(1),s.pf(2),s.blocks_pos(),
        s.robust()?"*** ROBUST":(s.wfpos()?"** WF+":""));
}

// z-score mean-reversion on a cross series. PnL in bps of price (cross-symbol
// comparable). cost_bps = total round-trip cost in bps (2*HS already folded by
// caller -> we pass per-RT bps directly). require_hook: only enter once z has
// started reverting (|z| this bar < |z| prev bar) -- no falling-knife.
static Stats zmr(const std::vector<int64_t>&ts,const std::vector<double>&px,
                 int win,double zin,double zout,double zstop,int hold,
                 double cost_bps,bool require_hook){
    Stats s; const int N=(int)px.size();
    bool in=false,is_long=false; double e=0; int ei=0,bars=0; double zprev=0;
    std::vector<double> lp(N); for(int i=0;i<N;i++) lp[i]=std::log(px[i]);
    for(int i=win;i<N;i++){
        // rolling mean/sd over [i-win, i-1] (no lookahead)
        double sum=0; for(int k=i-win;k<i;k++) sum+=lp[k]; double mean=sum/win;
        double var=0; for(int k=i-win;k<i;k++){ double d=lp[k]-mean; var+=d*d; } var/=(win-1);
        double sd=std::sqrt(var); if(sd<=0) continue;
        double z=(lp[i]-mean)/sd;
        if(in){
            ++bars;
            bool ex=false;
            if(is_long && z>=-zout) ex=true;
            if(!is_long && z<= zout) ex=true;
            if(is_long && z<=-zstop) ex=true;
            if(!is_long && z>= zstop) ex=true;
            if(bars>=hold) ex=true;
            if(ex){
                double x=px[i];
                double mv=is_long ? (x-e)/e : (e-x)/e;   // fractional move in trade direction
                double pnl=mv*10000.0 - cost_bps;        // bps minus round-trip cost
                s.add(pnl,ts[i]); in=false;
            }
        } else {
            bool reverting = std::fabs(z) < std::fabs(zprev);
            if(z> zin && (!require_hook||reverting)){ in=true; is_long=false; e=px[i]; ei=i; bars=0; }
            else if(z<-zin && (!require_hook||reverting)){ in=true; is_long=true;  e=px[i]; ei=i; bars=0; }
        }
        zprev=z;
        (void)ei;
    }
    return s;
}

static void scan_cross(const std::string& name,const std::vector<int64_t>&ts,
                       const std::vector<double>&px,double cost_bps,bool d1=false){
    if(px.size()<200){ printf("[%s] too few aligned bars (%zu)\n",name.c_str(),px.size()); return; }
    TS_MIN=ts.front(); TS_MAX=ts.back(); SPLIT_TS=ts[ts.size()/2];
    printf("\n=== %s  bars=%zu  span=%ld..%ld  cost_rt=%.1fbp ===\n",
           name.c_str(),px.size(),(long)ts.front(),(long)ts.back(),cost_bps);
    std::vector<int> wins  = d1 ? std::vector<int>{20,40,60,120} : std::vector<int>{60,120,240,480};
    std::vector<int> holds = d1 ? std::vector<int>{10,20,40}     : std::vector<int>{48,96,168};
    double zins[]={2.0,2.5,3.0};
    double zouts[]={0.3,0.5};
    for(int w:wins) for(double zi:zins) for(double zo:zouts) for(int h:holds){
        for(int hook=0;hook<2;hook++){
            Stats s=zmr(ts,px,w,zi,zo,3.5,h,cost_bps,hook);
            if(s.n[0]<20) continue;
            if(s.wfpos()||s.robust()){ // only print survivors to cut noise
                char tag[96]; snprintf(tag,sizeof tag,"w%d zin%.1f zout%.1f h%d %s",
                                       w,zi,zo,h,hook?"HOOK":"raw");
                report(tag,s);
            }
        }
    }
}

int main(){
    printf("FX CROSS-PAIR MEAN-REVERSION SCAN (S43)  --  long+short, cost-gated, WF+6block\n");

    // --- EURGBP: real cross, direct file. retail RT ~ 2*0.8bp = 1.6bp ---
    {
        auto E=load_h1("/Users/jo/Tick/EURGBP_merged.h1.csv");
        std::vector<int64_t> ts; std::vector<double> px;
        for(auto&b:E){ ts.push_back(b.ts); px.push_back(b.c); }
        scan_cross("EURGBP (real cross)", ts, px, 1.6);
    }

    // --- AUDNZD: synth = AUDUSD/NZDUSD. pays both legs: RT ~ 2*(0.8+1.0)=3.6bp ---
    {
        auto A=load_h1("/Users/jo/Tick/AUDUSD_merged.h1.csv");
        auto NZ=load_h1("/Users/jo/Tick/NZDUSD_merged.h1.csv");
        std::vector<int64_t> ts; std::vector<double> ca,cb;
        align(A,NZ,ts,ca,cb);
        std::vector<double> px(ts.size());
        for(size_t i=0;i<ts.size();i++) px[i]=ca[i]/cb[i];   // AUD/NZD
        scan_cross("AUDNZD (synth AUDUSD/NZDUSD)", ts, px, 3.6);
    }

    // === REAL D1 crosses (Dukascopy direct, true retail cost) ===
    {
        auto E=load_h1("/Users/jo/Omega/download/eurgbp-d1-bid-2019-01-01-2026-05-31.csv");
        std::vector<int64_t> ts; std::vector<double> px;
        for(auto&b:E){ ts.push_back(b.ts); px.push_back(b.c); }
        scan_cross("EURGBP (REAL D1)", ts, px, 1.6, true);
    }
    {
        auto A=load_h1("/Users/jo/Omega/download/audnzd-d1-bid-2019-01-01-2026-05-31.csv");
        std::vector<int64_t> ts; std::vector<double> px;
        for(auto&b:A){ ts.push_back(b.ts); px.push_back(b.c); }
        scan_cross("AUDNZD (REAL D1, RT 3.0bp)", ts, px, 3.0, true);
    }

    printf("\n(blank section = NO survivor at that cost. *** ROBUST = both halves + 5/6 blocks.)\n");
    return 0;
}
