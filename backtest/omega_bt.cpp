// =============================================================================
// omega_bt.cpp -- Multi-engine Omega backtest
// Tests ALL gold engines independently on 2yr tick data.
//
// Engines tested:
//   1. CandleFlowEngine  (CFE) -- bar-close + RSI + drift
//   2. GoldFlowEngine    (GFE) -- L2 imbalance + drift persistence
//   3. MacroCrashEngine  (MCE) -- high-ATR expansion moves
//   4. RSIReversalEngine (RRE) -- RSI extreme reversal
//   5. MicroMomentumEngine (MME) -- RSI delta momentum
//   6. DomPersistEngine  (DPE) -- DOM persistence
//   7. GoldHybridBracketEngine (HBE) -- structure breakout bracket
//
// Build:
//   g++ -O3 -std=c++17 -march=native \
//       -I../include \
//       -o omega_bt omega_bt.cpp
//   (run from backtest/ directory inside Omega repo)
//
// Run:
//   ./omega_bt ~/Tick/2yr_XAUUSD_tick.csv
//   ./omega_bt ~/Tick/2yr_XAUUSD_tick.csv --start 2024-01-01 --end 2025-01-01
//   ./omega_bt ~/Tick/2yr_XAUUSD_tick.csv --engine cfe,gfe,mce
//   ./omega_bt ~/Tick/2yr_XAUUSD_tick.csv --diag
// =============================================================================

// Time shim MUST be first
#include "OmegaTimeShim.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// Engine headers -- order matters (shim must be before all)
#include "../include/OmegaTradeLedger.hpp"
#include "../include/CandleFlowEngine.hpp"
#include "../include/GoldFlowEngine.hpp"
#include "../include/MacroCrashEngine.hpp"
#include "../include/RSIReversalEngine.hpp"
#include "../include/MicroMomentumEngine.hpp"
#include "../include/DomPersistEngine.hpp"
#include "../include/GoldHybridBracketEngine.hpp"

// =============================================================================
// mmap
// =============================================================================
struct MMapFile {
    const char* data=nullptr; size_t size=0;
#ifdef _WIN32
    HANDLE hFile=INVALID_HANDLE_VALUE,hMap=nullptr;
#else
    int fd=-1;
#endif
    bool open(const char* path) noexcept {
#ifdef _WIN32
        hFile=CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(hFile==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER sz{};GetFileSizeEx(hFile,&sz);size=(size_t)sz.QuadPart;
        hMap=CreateFileMappingA(hFile,nullptr,PAGE_READONLY,0,0,nullptr);
        if(!hMap){CloseHandle(hFile);return false;}
        data=(const char*)MapViewOfFile(hMap,FILE_MAP_READ,0,0,0);return data!=nullptr;
#else
        fd=::open(path,O_RDONLY);if(fd<0)return false;
        struct stat st{};fstat(fd,&st);size=(size_t)st.st_size;
        data=(const char*)mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0);
        if(data==(const char*)-1){data=nullptr;::close(fd);return false;}
        madvise((void*)data,size,MADV_SEQUENTIAL);return true;
#endif
    }
    ~MMapFile(){
#ifdef _WIN32
        if(data)UnmapViewOfFile(data);if(hMap)CloseHandle(hMap);
        if(hFile!=INVALID_HANDLE_VALUE)CloseHandle(hFile);
#else
        if(data)munmap((void*)data,size);if(fd>=0)::close(fd);
#endif
    }
};

// =============================================================================
// Fast scalar parsers
// =============================================================================
static inline double fast_f(const char* s,const char** e) noexcept {
    while(*s==' ')++s;bool neg=*s=='-';if(neg)++s;
    double v=0;while((unsigned)(*s-'0')<10u)v=v*10+(*s++-'0');
    if(*s=='.'){++s;double f=.1;while((unsigned)(*s-'0')<10u){v+=(*s++-'0')*f;f*=.1;}}
    if(e)*e=s;return neg?-v:v;
}
static inline int fast_int(const char* s,int n) noexcept {
    int v=0;for(int i=0;i<n;++i)v=v*10+(s[i]-'0');return v;
}
static int64_t ymdhms_ms(const char* d,const char* t) noexcept {
    int y=fast_int(d,4),mo=fast_int(d+4,2),dy=fast_int(d+6,2);
    int h=fast_int(t,2),mi=fast_int(t+3,2),se=fast_int(t+6,2);
    if(mo<=2){--y;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return(days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

struct Tick{int64_t ts_ms;double bid,ask;};

static std::vector<Tick> parse_csv(const MMapFile& f,int64_t s0,int64_t s1){
    std::vector<Tick> v;v.reserve(120'000'000);
    const char* p=f.data,*end=p+f.size;
    if(f.size>=3&&(uint8_t)p[0]==0xEF&&(uint8_t)p[1]==0xBB&&(uint8_t)p[2]==0xBF)p+=3;
    if(p<end&&!((unsigned)(*p-'0')<10u)){while(p<end&&*p!='\n')++p;if(p<end)++p;}
    while(p<end){
        while(p<end&&(*p=='\r'||*p=='\n'))++p;if(p>=end)break;
        if((size_t)(end-p)<17)break;
        const char* dp=p;while(p<end&&*p!=',')++p;if(p>=end)break;++p;
        const char* tp=p;while(p<end&&*p!=',')++p;if(p>=end)break;++p;
        const char* nx;
        double bid=fast_f(p,&nx);p=nx;if(p>=end||*p!=','){while(p<end&&*p!='\n')++p;continue;}++p;
        double ask=fast_f(p,&nx);p=nx;
        while(p<end&&*p!='\n')++p;
        if(bid<=0||ask<=bid)continue;
        int64_t ts=ymdhms_ms(dp,tp);if(ts<=0)continue;
        if(s0>0&&ts<s0)continue;
        if(s1>0&&ts>=s1)break;
        v.push_back({ts,bid,ask});
    }
    printf("[CSV] %zu ticks\n",v.size());return v;
}

// =============================================================================
// Shared indicator bundle -- computed once per tick, shared across all engines
// TIME-BASED alphas throughout
// =============================================================================
struct SharedInd {
    // EWM drift (30s halflife)
    double ewm_mid=0,ewm_drift=0;bool ewm_init=false;
    // ATR (M1 bar EMA-20)
    double atr=0;bool atr_init=false;
    // M1 bar
    double bo=0,bh=0,bl=0,bc=0;int64_t bmin=0;bool bhas=false;
    bool bar_closed=false;
    double cb_o=0,cb_h=0,cb_l=0,cb_c=0;
    // VWAP daily reset
    double vpv=0,vvol=0;int vday=-1;double vwap=0;
    // Vol ratio
    double vs=0,vl=0,vol_ratio=1.0;
    // RSI-14 on 1s pseudo bars
    static constexpr int RP=14;
    double rg[RP]={},rl[RP]={};int ri=0,rc=0;
    double rsi=50.0;int64_t rsi_bar_ts=0;double rsi_bar_open=0;
    // EMA trend
    double ema_f=0,ema_s=0;bool ema_init=false;
    int trend_state=0; // +1/-1/0
    // Session slot
    int session_slot=0;
    // PDH/PDL daily range
    double pdh=0,pdl=0;        // previous day high/low
    double today_hi=0,today_lo=1e9;
    int    pdhl_day=-1;
    bool   inside_range=true;  // price inside PDH/PDL +/-1pt
    // Timestamps
    int64_t last_ms=0,prev_ms=0;
    // Gap tracking
    static constexpr int64_t GAP_MS=3'600'000;

    void reset(double mid) noexcept {
        ewm_mid=mid;ewm_drift=0;
        bhas=false;bar_closed=false;
        vs=vl=0;vol_ratio=1;
        ema_f=ema_s=mid;
        prev_ms=0;
    }

    static double tba(double dt,double hl) noexcept {
        const double a=1.0-std::exp(-dt*0.693147/hl);
        return a<1.0?a:1.0;
    }

    void update(double bid,double ask,int64_t ts) noexcept {
        const double mid=(bid+ask)*0.5;
        bar_closed=false;

        if(last_ms>0&&(ts-last_ms)>GAP_MS)reset(mid);
        const double dt=(prev_ms>0)?(double)(ts-prev_ms):100.0;
        prev_ms=last_ms=ts;

        // Set simulated time for OmegaTimeShim
        omega::bt::set_sim_time(ts);

        // EWM drift 30s halflife
        if(!ewm_init){ewm_mid=mid;ewm_init=true;}
        {const double a=tba(dt,30000.0);ewm_mid=a*mid+(1-a)*ewm_mid;}
        ewm_drift=mid-ewm_mid;

        // VWAP
        {int day=(int)(ts/86'400'000LL);
         if(day!=vday){vpv=vvol=0;vday=day;}
         vpv+=mid;vvol+=1;vwap=vpv/vvol;}

        // Vol ratio 5s/60s halflife
        {const double am=std::fabs(ewm_drift);
         const double sa=tba(dt,5000.0),la=tba(dt,60000.0);
         vs=sa*am+(1-sa)*vs;vl=la*am+(1-la)*vl;
         vol_ratio=vl>1e-9?vs/vl:1.0;}

        // EMA trend fast=5s slow=60s
        if(!ema_init){ema_f=ema_s=mid;ema_init=true;}
        {const double af=tba(dt,5000.0),as=tba(dt,60000.0);
         ema_f=af*mid+(1-af)*ema_f;ema_s=as*mid+(1-as)*ema_s;}
        const double gap=ema_f-ema_s,thresh=atr>0.5?atr*0.3:0.3;
        trend_state=gap>thresh?1:gap<-thresh?-1:0;

        // RSI-14 on 1s bars
        if(rsi_bar_ts==0){rsi_bar_ts=ts;rsi_bar_open=mid;}
        if(ts-rsi_bar_ts>=1000){
            const double mv=mid-rsi_bar_open;
            rg[ri]=mv>0?mv:0;rl[ri]=mv<0?-mv:0;
            ri=(ri+1)%RP;if(rc<RP)++rc;
            if(rc>=RP){double ag=0,al=0;
                for(int i=0;i<RP;++i){ag+=rg[i];al+=rl[i];}
                ag/=RP;al/=RP;rsi=al<1e-9?100.0:100.0-100.0/(1+ag/al);}
            rsi_bar_ts=ts;rsi_bar_open=mid;
        }

        // M1 bar
        const int64_t bm=ts/60000LL;
        if(!bhas){bo=bh=bl=bc=mid;bmin=bm;bhas=true;}
        else if(bm!=bmin){
            cb_o=bo;cb_h=bh;cb_l=bl;cb_c=bc;bar_closed=true;
            const double rng=bh-bl;
            if(!atr_init){atr=rng;atr_init=true;}
            else{const double a=2.0/(20.0+1.0);atr=a*rng+(1-a)*atr;}
            bo=bh=bl=bc=mid;bmin=bm;
        } else{if(mid>bh)bh=mid;if(mid<bl)bl=mid;bc=mid;}

        // Session slot
        const int utch=(int)((ts/1000%86400)/3600);
        const int hhmm=utch*100+((int)((ts/1000%3600)/60));
        if     (hhmm>=600 &&hhmm<800 )session_slot=1;
        else if(hhmm>=800 &&hhmm<1000)session_slot=2;
        else if(hhmm>=1200&&hhmm<1630)session_slot=3;
        else if(hhmm>=1630&&hhmm<1900)session_slot=4;
        else if(hhmm>=1900&&hhmm<2100)session_slot=5;
        else if(hhmm>=100 &&hhmm<600 )session_slot=6;
        else session_slot=0;

        // PDH/PDL daily range tracking
        const int day=(int)(ts/86'400'000LL);
        if(day!=pdhl_day){
            if(pdhl_day>=0){ pdh=today_hi; pdl=(today_lo<1e8?today_lo:0.0); }
            today_hi=mid; today_lo=mid; pdhl_day=day;
        } else {
            if(mid>today_hi)today_hi=mid;
            if(mid<today_lo)today_lo=mid;
        }
        inside_range=(pdh>0&&pdl>0)
            ?(mid<=pdh+1.0&&mid>=pdl-1.0)
            :true;
    }

    bool expansion_regime() const noexcept {
        return vol_ratio>2.0&&std::fabs(ewm_drift)>2.0;
    }
};

// =============================================================================
// Stats tracker
// =============================================================================
struct Stats {
    std::string name_str;
    const char* name="";
    int64_t n=0,wins=0;double pnl=0,peak=0,dd=0;int64_t hsum=0;
    int bhn[24]={};double bhp[24]={};
    int brn[6]={};double brp[6]={};
    static constexpr const char* RN[6]={"SL_HIT","TRAIL_SL","PARTIAL_TP","STAGNATION","FORCE_CLOSE","other"};
    double monthly[32]={};
    static constexpr int64_t MO_BASE=1695772800LL; // 2023-09-27

    void add(const omega::TradeRecord& t) {
        const double p=t.pnl*100.0;
        pnl+=p;if(pnl>peak)peak=pnl;
        const double d=peak-pnl;if(d>dd)dd=d;
        if(strcmp(t.exitReason.c_str(),"PARTIAL_TP")==0)return;
        ++n;if(p>0)++wins;hsum+=(t.exitTs-t.entryTs);
        const int h=(int)(t.entryTs%86400/3600);
        if(h>=0&&h<24){bhn[h]++;bhp[h]+=p;}
        bool matched=false;
        for(int i=0;i<5;++i)if(t.exitReason==RN[i]){brn[i]++;brp[i]+=p;matched=true;break;}
        if(!matched){brn[5]++;brp[5]+=p;}
        const int mo=(int)((t.exitTs-MO_BASE)/2592000);
        if(mo>=0&&mo<32)monthly[mo]+=p;
    }

    void print(FILE* out=stdout,bool full=true) const {
        const double wr=n?100.0*wins/n:0;
        fprintf(out,"\n%s\n",std::string(70,'=').c_str());
        fprintf(out,"%-40s\n",name);
        fprintf(out,"%s\n",std::string(70,'=').c_str());
        fprintf(out,"Trades: %lld  WR: %.1f%%  Net: $%.2f  Avg: $%.2f  Hold: %.0fs  DD: $%.2f\n",
                (long long)n,wr,pnl,n?pnl/n:0.0,n?(double)hsum/n:0.0,dd);
        if(!full)return;
        fprintf(out,"\n  Exit reasons:\n");
        for(int i=0;i<6;++i)if(brn[i])
            fprintf(out,"    %-15s n=%5d  $%+8.2f\n",RN[i],brn[i],brp[i]);
        fprintf(out,"\n  By UTC hour:\n");
        for(int h=0;h<24;++h){if(!bhn[h])continue;
            char bar[41]={};int b=std::min(40,(int)(std::fabs(bhp[h])/100));
            memset(bar,bhp[h]>0?'#':'.',b);
            fprintf(out,"    %02d:00  n=%4d  $%+7.2f  %s\n",h,bhn[h],bhp[h],bar);}
        fprintf(out,"\n  Monthly:\n");
        double cum=0;
        for(int i=0;i<32;++i){if(monthly[i]==0)continue;cum+=monthly[i];
            const int64_t sec=MO_BASE+(int64_t)i*2592000;
            const int yr=(int)(sec/31557600+1970);
            const int mo=(int)((sec%31557600)/2592000+1);
            char bar[41]={};int b=std::min(40,(int)(std::fabs(monthly[i])/100));
            memset(bar,monthly[i]>0?'#':'.',b);
            fprintf(out,"    %04d-%02d  %+8.2f  cum=%+9.2f  %s\n",yr,mo,monthly[i],cum,bar);}
        fprintf(out,"%s\n",std::string(70,'=').c_str());
    }
};

// =============================================================================
// Global trade store (same pattern as OmegaBacktest.cpp)
// =============================================================================
static std::vector<omega::TradeRecord> g_trades;
static std::unordered_map<std::string,Stats> g_stats;
static int64_t g_warmup_ts=0;

static auto make_cb(const char* engine_override=nullptr) {
    return [engine_override](const omega::TradeRecord& t){
        if(t.entryTs < g_warmup_ts/1000) return;
        omega::TradeRecord r=t;
        if(engine_override) r.engine=engine_override;
        g_trades.push_back(r);
        auto& s=g_stats[r.engine];
        if(s.name_str.empty()){ s.name_str=r.engine; s.name=s.name_str.c_str(); }
        s.add(r);
    };
}

// =============================================================================
// Engine runner wrappers
// =============================================================================

struct CFERunner {
    omega::CandleFlowEngine eng;
    CFERunner(){ eng.shadow_mode=false; }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        omega::CandleFlowEngine::BarSnap bar{};
        if(ind.bar_closed){
            bar.open=ind.cb_o;bar.high=ind.cb_h;bar.low=ind.cb_l;bar.close=ind.cb_c;
            bar.valid=true;
        }
        omega::CandleFlowEngine::DOMSnap dom{};
        dom.l2_imb=0.5;dom.bid_count=5;dom.ask_count=5;
        dom.prev_bid_count=5;dom.prev_ask_count=5;
        auto cb=make_cb("CFE");
        eng.on_tick(bid,ask,bar,dom,ts,ind.atr>0?ind.atr:1.0,cb,ind.ewm_drift);
    }
};

struct GFERunner {
    GoldFlowEngine eng;
    GFERunner(){ eng.addon_shadow_mode=true; } // no addon, use trade record cb
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("GFE");
        eng.on_tick(bid,ask,0.5,ind.ewm_drift,ts,cb,ind.session_slot,false);
    }
};

struct MCERunner {
    omega::MacroCrashEngine eng;
    MCERunner(){
        eng.shadow_mode=false;
        eng.pyramid_shadow=false;
        eng.on_trade_record=make_cb("MCE");
    }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        eng.on_tick(bid,ask,ind.atr,ind.vol_ratio,ind.ewm_drift,
                    ind.expansion_regime(),ts,
                    0.0,false,false,0.0,ind.rsi,ind.session_slot);
    }
};

struct RRERunner {
    omega::RSIReversalEngine eng;
    RRERunner(){ eng.shadow_mode=false; }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("RRE");
        eng.on_tick(bid,ask,ind.session_slot,ts,
                    0.5,false,false,false,false,false,cb,ind.ewm_drift);
    }
};

struct MMERunner {
    omega::MicroMomentumEngine eng;
    MMERunner(){ eng.shadow_mode=false; }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("MME");
        eng.on_tick(bid,ask,ind.session_slot,ts,
                    0.5,0.0,false,false,false,false,false,cb,ind.ewm_drift);
    }
};

struct DPERunner {
    DomPersistEngine eng;
    DPERunner(){ eng.shadow_mode=false; }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("DPE");
        eng.on_tick(bid,ask,0.5,false,ts,ind.session_slot,cb);
    }
};

struct HBERunner {
    omega::GoldHybridBracketEngine eng;
    // HBE has no shadow_mode member -- enable via can_enter flag
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("HBE");
        eng.on_tick(bid,ask,ts,true,false,false,0,cb);
    }
};

struct PDHLRunner {
    omega::PDHLReversionEngine eng;
    PDHLRunner(){ eng.shadow_mode=false; }
    void tick(const SharedInd& ind,double bid,double ask,int64_t ts){
        auto cb=make_cb("PDHL");
        eng.on_tick(bid,ask,ts,
                    ind.pdh,ind.pdl,
                    ind.atr,
                    0.5,   // l2_imbalance: no real L2 in CSV backtest
                    0,0,   // depth_bid, depth_ask
                    false, // l2_real=false: use drift proxy
                    ind.ewm_drift,
                    ind.session_slot,
                    cb);
    }
};

// =============================================================================
// Date parser
// =============================================================================
static int64_t parse_date(const char* s){
    if(!s||strlen(s)<10)return 0;
    char buf[9]={s[0],s[1],s[2],s[3],s[5],s[6],s[8],s[9],0};
    return ymdhms_ms(buf,"00:00:00");
}

// =============================================================================
// Output
// =============================================================================
static void write_trades(const char* path){
    FILE* f=fopen(path,"w");if(!f){fprintf(stderr,"[WARN] cannot write %s\n",path);return;}
    fprintf(f,"engine,side,entry_ts,exit_ts,entry,exit,sl,size,pnl_pts,pnl_usd,hold_s,mfe,mae,reason,regime\n");
    for(const auto& t:g_trades)
        fprintf(f,"%s,%s,%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%lld,%.4f,%.4f,%s,%s\n",
                t.engine.c_str(),t.side.c_str(),
                (long long)t.entryTs,(long long)t.exitTs,
                t.entryPrice,t.exitPrice,t.sl,t.size,
                t.pnl,t.pnl*100.0,
                (long long)(t.exitTs-t.entryTs),
                t.mfe,t.mae,
                t.exitReason.c_str(),t.regime.c_str());
    fclose(f);
    printf("[OUTPUT] %zu trades -> %s\n",g_trades.size(),path);
}

static void write_equity(const char* path){
    // Per-engine daily equity
    std::unordered_map<std::string,std::unordered_map<int,double>> daily;
    for(const auto& t:g_trades){
        if(t.exitReason=="PARTIAL_TP")continue;
        const int day=(int)(t.exitTs/86400);
        daily[t.engine][day]+=t.pnl*100.0;
    }
    FILE* f=fopen(path,"w");if(!f)return;
    fprintf(f,"date_sec");
    std::vector<std::string> engines;
    for(auto& kv:daily)engines.push_back(kv.first);
    std::sort(engines.begin(),engines.end());
    for(const auto& e:engines)fprintf(f,",%s",e.c_str());
    fprintf(f,"\n");
    // Collect all days
    std::vector<int> days;
    for(auto& kv:daily)for(auto& dv:kv.second)days.push_back(dv.first);
    std::sort(days.begin(),days.end());
    days.erase(std::unique(days.begin(),days.end()),days.end());
    std::unordered_map<std::string,double> cum;
    for(int d:days){
        fprintf(f,"%lld",(long long)d*86400);
        for(const auto& e:engines){
            const double p=daily.count(e)&&daily[e].count(d)?daily[e][d]:0;
            cum[e]+=p;
            fprintf(f,",%.2f",cum[e]);
        }
        fprintf(f,"\n");
    }
    fclose(f);
    printf("[OUTPUT] Equity -> %s\n",path);
}

// =============================================================================
// Summary table
// =============================================================================
static void print_summary(FILE* out=stdout){
    fprintf(out,"\n%s\n",std::string(100,'=').c_str());
    fprintf(out,"%-6s  %6s  %5s  %10s  %8s  %8s  %6s  %s\n",
            "ENGINE","TRADES","WR%","NET_PNL","AVG","MAX_DD","HOLD","MONTHLY_AVG");
    fprintf(out,"%s\n",std::string(100,'-').c_str());

    std::vector<const Stats*> sv;
    for(auto& kv:g_stats)sv.push_back(&kv.second);
    std::sort(sv.begin(),sv.end(),[](const Stats*a,const Stats*b){return a->pnl>b->pnl;});

    for(const auto* s:sv){
        // count non-zero months
        int months=0;double mo_sum=0;
        for(int i=0;i<32;++i)if(s->monthly[i]!=0){++months;mo_sum+=s->monthly[i];}
        const double mo_avg=months?mo_sum/months:0;
        fprintf(out,"%-6s  %6lld  %5.1f  %+10.2f  %+8.2f  %8.2f  %6.0f  %+.2f/mo\n",
                s->name,(long long)s->n,s->n?100.0*s->wins/s->n:0,
                s->pnl,s->n?s->pnl/s->n:0,s->dd,
                s->n?(double)s->hsum/s->n:0,mo_avg);
    }
    fprintf(out,"%s\n",std::string(100,'=').c_str());
}

// =============================================================================
// main
// =============================================================================
int main(int argc,char** argv){
    if(argc<2){
        fprintf(stderr,"Usage: omega_bt <ticks.csv> [--start YYYY-MM-DD] [--end YYYY-MM-DD]\n"
                       "               [--engine cfe,gfe,mce,rre,mme,dpe,hbe] [--trades f]\n"
                       "               [--report f] [--equity f] [--diag]\n");
        return 1;
    }

    const char* csv=argv[1];
    const char* trd="omega_bt_trades.csv";
    const char* rep="omega_bt_report.txt";
    const char* eq ="omega_bt_equity.csv";
    int64_t s0=0,s1=0;
    bool diag=false;
    // Engine enable flags
    bool en_cfe=true,en_gfe=true,en_mce=true,en_rre=true,en_mme=true,en_dpe=true,en_hbe=true,en_pdhl=true;

    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--start")&&i+1<argc) s0=parse_date(argv[++i]);
        else if(!strcmp(argv[i],"--end")&&i+1<argc) s1=parse_date(argv[++i]);
        else if(!strcmp(argv[i],"--trades")&&i+1<argc) trd=argv[++i];
        else if(!strcmp(argv[i],"--report")&&i+1<argc) rep=argv[++i];
        else if(!strcmp(argv[i],"--equity")&&i+1<argc) eq=argv[++i];
        else if(!strcmp(argv[i],"--diag")) diag=true;
        else if(!strcmp(argv[i],"--engine")&&i+1<argc){
            const char* e=argv[++i];
            en_cfe=!!strstr(e,"cfe");en_gfe=!!strstr(e,"gfe");
            en_mce=!!strstr(e,"mce");en_rre=!!strstr(e,"rre");
            en_mme=!!strstr(e,"mme");en_dpe=!!strstr(e,"dpe");
            en_hbe=!!strstr(e,"hbe");en_pdhl=!!strstr(e,"pdhl");
            if(!!strstr(e,"all")){en_cfe=en_gfe=en_mce=en_rre=en_mme=en_dpe=en_hbe=en_pdhl=true;}
        }
    }

    printf("[OMEGA_BT] Opening %s\n",csv);
    MMapFile f;if(!f.open(csv)){fprintf(stderr,"Cannot open\n");return 1;}
    printf("[OMEGA_BT] %.1f MB\n",f.size/1e6);

    auto ticks=parse_csv(f,s0,s1);
    if(ticks.empty()){fprintf(stderr,"No ticks\n");return 1;}

    // Warmup: skip first 5000 ticks worth of time for indicator warmup
    g_warmup_ts=ticks.size()>5000?ticks[5000].ts_ms:0;

    printf("[OMEGA_BT] Engines: %s%s%s%s%s%s%s%s\n",
           en_cfe?"CFE ":"",en_gfe?"GFE ":"",en_mce?"MCE ":"",
           en_rre?"RRE ":"",en_mme?"MME ":"",en_dpe?"DPE ":"",en_hbe?"HBE ":"",
           en_pdhl?"PDHL ":"");
    printf("[OMEGA_BT] Running %zu ticks...\n",ticks.size());

    // Suppress engine diagnostic stdout during tick loop
    // Engines print massive amounts of per-tick diagnostics -- we only want our summary
    int saved_stdout = -1;
    FILE* devnull = nullptr;
#ifndef _WIN32
    saved_stdout = dup(STDOUT_FILENO);
    devnull = fopen("/dev/null","w");
    if(devnull){ fflush(stdout); dup2(fileno(devnull),STDOUT_FILENO); }
#endif

    // Instantiate engines
    CFERunner cfe; GFERunner gfe; MCERunner mce;
    RRERunner rre; MMERunner mme; DPERunner dpe; HBERunner hbe;
    PDHLRunner pdhl;

    SharedInd ind;
    int64_t diag_next=0;
    static constexpr int64_t DIAG_IV=3'600'000LL;

    const size_t N=ticks.size();
    for(size_t i=0;i<N;++i){
        const auto& tk=ticks[i];
        ind.update(tk.bid,tk.ask,tk.ts_ms);

        if(en_cfe) cfe.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_gfe) gfe.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_mce) mce.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_rre) rre.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_mme) mme.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_dpe) dpe.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_hbe) hbe.tick(ind,tk.bid,tk.ask,tk.ts_ms);
        if(en_pdhl) pdhl.tick(ind,tk.bid,tk.ask,tk.ts_ms);

        if(diag&&tk.ts_ms>=diag_next){
            diag_next=tk.ts_ms+DIAG_IV;
            const int h=(int)((tk.ts_ms/1000%86400)/3600);
            printf("  [DIAG] h=%02d bid=%.2f drift=%.3f rsi=%.1f atr=%.3f vwap=%.2f vol_r=%.2f slot=%d\n",
                   h,tk.bid,ind.ewm_drift,ind.rsi,ind.atr,ind.vwap,ind.vol_ratio,ind.session_slot);
        }

        if(i%10'000'000==0&&i>0)
            printf("  %zuM  trades=%zu\n",i/1'000'000,g_trades.size());
    }

    // Restore stdout
#ifndef _WIN32
    if(saved_stdout>=0){ fflush(stdout); dup2(saved_stdout,STDOUT_FILENO); close(saved_stdout); }
    if(devnull) fclose(devnull);
#endif
    printf("[OMEGA_BT] Done. %zu trade records.\n",g_trades.size());

    // Summary table to console
    print_summary(stdout);

    // Full per-engine detail
    std::vector<const Stats*> sv;
    for(auto& kv:g_stats)sv.push_back(&kv.second);
    std::sort(sv.begin(),sv.end(),[](const Stats*a,const Stats*b){return a->pnl>b->pnl;});
    for(const auto* s:sv)s->print(stdout);

    // Write report
    FILE* rp=fopen(rep,"w");
    if(rp){
        print_summary(rp);
        for(const auto* s:sv)s->print(rp);
        fclose(rp);
        printf("[OUTPUT] Report -> %s\n",rep);
    }

    write_trades(trd);
    write_equity(eq);

    return 0;
}
