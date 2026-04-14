// =============================================================================
// cfe_bt.cpp -- High-performance CandleFlowEngine backtest
//
// Build (Mac/Linux):
//   g++ -O3 -std=c++17 -march=native -o cfe_bt cfe_bt.cpp
//
// Run:
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv --diag        # hourly indicator samples
//   ./cfe_bt ~/Tick/2yr_XAUUSD_tick.csv --start 2024-01-01 --end 2024-07-01
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cinttypes>
#include <algorithm>
#include <vector>
#include <string>

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

// =============================================================================
// mmap
// =============================================================================
struct MMapFile {
    const char* data = nullptr;
    size_t      size = 0;
#ifdef _WIN32
    HANDLE hFile = INVALID_HANDLE_VALUE, hMap = nullptr;
#else
    int fd = -1;
#endif
    bool open(const char* path) noexcept {
#ifdef _WIN32
        hFile = CreateFileA(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (hFile==INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{}; GetFileSizeEx(hFile,&sz); size=(size_t)sz.QuadPart;
        hMap=CreateFileMappingA(hFile,nullptr,PAGE_READONLY,0,0,nullptr);
        if (!hMap){CloseHandle(hFile);return false;}
        data=(const char*)MapViewOfFile(hMap,FILE_MAP_READ,0,0,0);
        return data!=nullptr;
#else
        fd=::open(path,O_RDONLY); if(fd<0)return false;
        struct stat st{}; fstat(fd,&st); size=(size_t)st.st_size;
        data=(const char*)mmap(nullptr,size,PROT_READ,MAP_PRIVATE,fd,0);
        if(data==(const char*)-1){data=nullptr;::close(fd);return false;}
        madvise((void*)data,size,MADV_SEQUENTIAL); return true;
#endif
    }
    ~MMapFile() {
#ifdef _WIN32
        if(data)UnmapViewOfFile(data); if(hMap)CloseHandle(hMap);
        if(hFile!=INVALID_HANDLE_VALUE)CloseHandle(hFile);
#else
        if(data)munmap((void*)data,size); if(fd>=0)::close(fd);
#endif
    }
};

// =============================================================================
// Fast scalar parsers
// =============================================================================
static inline double fast_f(const char* s, const char** e) noexcept {
    while(*s==' ')++s;
    bool neg=(*s=='-'); if(neg)++s;
    double v=0;
    while((unsigned)(*s-'0')<10u) v=v*10.0+(*s++-'0');
    if(*s=='.'){ ++s; double f=0.1; while((unsigned)(*s-'0')<10u){v+=(*s++-'0')*f;f*=.1;} }
    if(e)*e=s; return neg?-v:v;
}
static inline int fast_int(const char* s, int n) noexcept {
    int v=0; for(int i=0;i<n;++i) v=v*10+(s[i]-'0'); return v;
}
// YYYYMMDD,HH:MM:SS -> epoch ms UTC
static int64_t ymdhms_ms(const char* d, const char* t) noexcept {
    int y=fast_int(d,4), mo=fast_int(d+4,2), dy=fast_int(d+6,2);
    int h=fast_int(t,2), mi=fast_int(t+3,2), se=fast_int(t+6,2);
    if(mo<=2){--y;mo+=12;}
    int64_t days=365LL*y+y/4-y/100+y/400+(153*mo+8)/5+dy-719469LL;
    return (days*86400LL+h*3600LL+mi*60LL+se)*1000LL;
}

struct Tick { int64_t ts_ms; double bid,ask; };

static std::vector<Tick> parse_csv(const MMapFile& f, int64_t s0, int64_t s1) {
    std::vector<Tick> v; v.reserve(120'000'000);
    const char* p=f.data, *end=p+f.size;
    if(f.size>=3&&(uint8_t)p[0]==0xEF&&(uint8_t)p[1]==0xBB&&(uint8_t)p[2]==0xBF)p+=3;
    // skip header if non-digit first char
    if(p<end&&!((unsigned)(*p-'0')<10u)){while(p<end&&*p!='\n')++p;if(p<end)++p;}
    while(p<end){
        while(p<end&&(*p=='\r'||*p=='\n'))++p; if(p>=end)break;
        if((size_t)(end-p)<17)break;
        const char* dp=p; while(p<end&&*p!=',')++p; if(p>=end)break; ++p;
        const char* tp=p; while(p<end&&*p!=',')++p; if(p>=end)break; ++p;
        const char* nx;
        double bid=fast_f(p,&nx); p=nx; if(p>=end||*p!=','){while(p<end&&*p!='\n')++p;continue;} ++p;
        double ask=fast_f(p,&nx); p=nx;
        while(p<end&&*p!='\n')++p;
        if(bid<=0||ask<=bid)continue;
        int64_t ts=ymdhms_ms(dp,tp); if(ts<=0)continue;
        if(s0>0&&ts<s0)continue;
        if(s1>0&&ts>=s1)break;
        v.push_back({ts,bid,ask});
    }
    printf("[CSV] Parsed %zu ticks\n",v.size()); return v;
}

// =============================================================================
// CFE constants -- exact mirror of CandleFlowEngine.hpp
// =============================================================================
static constexpr double  CFE_BODY_RATIO_MIN           = 0.60;
static constexpr double  CFE_COST_SLIPPAGE            = 0.10;
static constexpr double  CFE_COMMISSION_PTS           = 0.10;
static constexpr double  CFE_COST_MULT                = 2.0;
static constexpr int64_t CFE_STAGNATION_MS_ASIA       = 90'000;
static constexpr int64_t CFE_STAGNATION_MS_LONDON     = 180'000;
static constexpr double  CFE_STAGNATION_MULT          = 1.0;
static constexpr double  CFE_RISK_DOLLARS             = 30.0;
static constexpr double  CFE_MIN_LOT                  = 0.01;
static constexpr double  CFE_MAX_LOT                  = 0.20;
static constexpr int     CFE_RSI_PERIOD               = 30;
static constexpr double  CFE_RSI_THRESH               = 6.0;
static constexpr double  CFE_DFE_RSI_LEVEL_LONG_MIN   = 35.0;
static constexpr double  CFE_DFE_RSI_LEVEL_SHORT_MAX  = 65.0;
static constexpr int     CFE_DFE_DRIFT_PERSIST_TICKS  = 2;
static constexpr double  CFE_DFE_DRIFT_SUSTAINED_THRESH = 0.8;
static constexpr int64_t CFE_DFE_DRIFT_SUSTAINED_MS   = 45'000;
static constexpr int64_t CFE_BAR_TREND_BLOCK_MS       = 45'000;
static constexpr int     CFE_DFE_PRICE_CONFIRM_TICKS  = 3;
static constexpr double  CFE_DFE_PRICE_CONFIRM_MIN    = 0.05;
static constexpr double  CFE_DFE_DRIFT_THRESH         = 1.5;
static constexpr double  CFE_DFE_DRIFT_ACCEL          = 0.2;
static constexpr double  CFE_DFE_RSI_THRESH           = 3.0;
static constexpr double  CFE_DFE_RSI_TREND_MAX        = 12.0;
static constexpr double  CFE_DFE_SL_MULT              = 0.7;
static constexpr double  CFE_MAX_ATR_ENTRY            = 6.0;
static constexpr int64_t CFE_DFE_COOLDOWN_MS          = 120'000;
static constexpr double  CFE_DFE_MIN_SPREAD_MULT      = 1.5;
static constexpr int64_t CFE_OPPOSITE_DIR_COOLDOWN_MS = 60'000;
static constexpr int64_t CFE_WINNER_COOLDOWN_MS       = 30'000;
static constexpr double  CHOP_VOL_RATIO               = 1.2;
static constexpr double  CHOP_DRIFT_ABS               = 1.0;
static constexpr int64_t GAP_RESET_MS                 = 3'600'000;

// TIME-BASED EWM half-lives (ms) -- adapts to tick density
// Live engine: span=300 ticks @ ~5t/s = 60s window -> 30s halflife equivalent
static constexpr double EWM_DRIFT_HL_MS   = 30'000.0;  // 30s halflife for drift
static constexpr double RSI_SLOPE_HL_MS   = 10'000.0;  // 10s halflife for RSI slope EMA
static constexpr double VOL_SHORT_HL_MS   =  5'000.0;  // 5s  halflife for vol ratio short
static constexpr double VOL_LONG_HL_MS    = 60'000.0;  // 60s halflife for vol ratio long
static constexpr double ATR_ALPHA         = 2.0/(20.0+1.0);

static inline double tba(double dt, double hl) noexcept {
    // time-based EWM alpha: 1 - exp(-dt*ln2/hl)
    const double a=1.0-std::exp(-dt*0.693147/hl);
    return a<1.0?a:1.0;
}

// =============================================================================
// Trade record
// =============================================================================
struct TradeRec {
    int64_t entry_ms,exit_ms;
    double  entry,exit_px,sl,size,pnl_pts,pnl_usd,mfe;
    int     hold_s;
    bool    is_long;
    char    reason[24];
    char    etype[4];   // BAR DFE SUS
};

// =============================================================================
// Indicators -- flat arrays, no heap allocation
// =============================================================================
struct Ind {
    // RSI circular buffer
    double rg[CFE_RSI_PERIOD]={}, rl[CFE_RSI_PERIOD]={};
    int    ri=0,rc=0;
    double rsi_cur=50,rsi_prev=50,rsi_trend=0,rsi_pm=0;
    bool   rsi_warm=false;

    // EWM drift
    double ewm_mid=0,ewm_drift=0;
    bool   ewm_init=false;

    // ATR from M1 bars
    double atr=0; bool atr_init=false;

    // M1 bar builder
    double bo=0,bh=0,bl=0,bc=0; int64_t bmin=0; bool bhas=false;
    // completed bar
    double co=0,ch=0,cl=0,cc=0; bool cvalid=false;

    // VWAP
    double vpv=0,vvol=0; int vday=-1; double vwap=0;

    // Vol ratio
    double vs=0,vl=0;

    // Recent mid ring (for DFE price confirm)
    static constexpr int RN=CFE_DFE_PRICE_CONFIRM_TICKS+2;
    double rm[RN]={}; int ri2=0,rc2=0;

    // Timestamps
    int64_t last_ms=0,prev_ms=0;

    void reset(double mid) noexcept {
        ewm_mid=mid; ewm_drift=0; bhas=false; cvalid=false;
        rc2=0; ri2=0; vs=0; vl=0; prev_ms=0;
    }

    // Returns true when a new M1 bar completed this tick
    bool update(double bid, double ask, int64_t ts) noexcept {
        const double mid=(bid+ask)*0.5;
        cvalid=false;

        if(last_ms>0&&(ts-last_ms)>GAP_RESET_MS) reset(mid);
        const double dt=(prev_ms>0)?(double)(ts-prev_ms):100.0;
        prev_ms=last_ms=ts;

        // EWM drift (time-based)
        if(!ewm_init){ewm_mid=mid;ewm_init=true;}
        { const double a=tba(dt,EWM_DRIFT_HL_MS); ewm_mid=a*mid+(1-a)*ewm_mid; }
        ewm_drift=mid-ewm_mid;

        // RSI
        if(rsi_pm!=0){
            const double chg=mid-rsi_pm;
            rg[ri]=chg>0?chg:0; rl[ri]=chg<0?-chg:0;
            ri=(ri+1)%CFE_RSI_PERIOD;
            if(rc<CFE_RSI_PERIOD)++rc;
            if(rc>=CFE_RSI_PERIOD){
                double ag=0,al=0;
                for(int i=0;i<CFE_RSI_PERIOD;++i){ag+=rg[i];al+=rl[i];}
                ag/=CFE_RSI_PERIOD; al/=CFE_RSI_PERIOD;
                rsi_prev=rsi_cur;
                rsi_cur=(al==0)?100.0:100.0-100.0/(1.0+ag/al);
                const double slope=rsi_cur-rsi_prev;
                if(!rsi_warm){rsi_trend=slope;rsi_warm=true;}
                else{ const double sa=tba(dt,RSI_SLOPE_HL_MS); rsi_trend=sa*slope+(1-sa)*rsi_trend; }
            }
        }
        rsi_pm=mid;

        // Recent mid ring
        rm[ri2]=mid; ri2=(ri2+1)%RN; if(rc2<RN)++rc2;

        // VWAP daily reset
        { int day=(int)(ts/86'400'000LL);
          if(day!=vday){vpv=0;vvol=0;vday=day;}
          vpv+=mid;vvol+=1; vwap=vpv/vvol; }

        // Vol ratio (time-based)
        { const double am=std::fabs(ewm_drift);
          const double sa=tba(dt,VOL_SHORT_HL_MS), la=tba(dt,VOL_LONG_HL_MS);
          vs=sa*am+(1-sa)*vs; vl=la*am+(1-la)*vl; }

        // M1 bar
        const int64_t bm=ts/60'000LL;
        if(!bhas){bo=bh=bl=bc=mid;bmin=bm;bhas=true;}
        else if(bm!=bmin){
            co=bo;ch=bh;cl=bl;cc=bc; cvalid=true;
            const double rng=bh-bl;
            if(!atr_init){atr=rng;atr_init=true;}
            else atr=ATR_ALPHA*rng+(1-ATR_ALPHA)*atr;
            bo=bh=bl=bc=mid; bmin=bm;
        } else {
            if(mid>bh)bh=mid; if(mid<bl)bl=mid; bc=mid;
        }
        return cvalid;
    }

    int rsi_dir() const noexcept {
        if(!rsi_warm)return 0;
        return rsi_trend>CFE_RSI_THRESH?1:rsi_trend<-CFE_RSI_THRESH?-1:0;
    }
    double vol_ratio() const noexcept { return vl>1e-9?vs/vl:1.0; }
    double oldest_rm() const noexcept {
        if(rc2<RN)return rm[0];
        return rm[ri2]; // oldest slot
    }
};

// =============================================================================
// Position
// =============================================================================
struct Pos {
    double entry=0,sl=0,trail_sl=0,size=0,full_size=0,cost=0,atr=0,mfe=0;
    int64_t ets=0;
    bool is_long=false,trail=false,partial=false,active=false;
    char etype[4]="BAR";
};

// =============================================================================
// CRTP engine base
// =============================================================================
template<typename D>
struct Engine {
    Ind ind;
    Pos pos;
    int phase=0; // 0=IDLE 1=LIVE 2=COOL
    int64_t cool_start=0,cool_ms=15'000,dfe_cool=0;
    int last_dir=0; int64_t last_close_ms=0;
    bool dfe_warm=false; double prev_drift=0;
    double dfe_thresh=CFE_DFE_DRIFT_THRESH;
    int dfe_ptks=0,dfe_pdir=0;
    int64_t sus_start=0; int sus_dir=0;
    double adv_px=0,adv_atr=0; int adv_dir=0; bool adv_blk=false;
    bool gates=true;
    int trade_id=0;
    std::vector<TradeRec> trades;

    void on_tick(double bid, double ask, int64_t ts) noexcept {
        const bool bar=ind.update(bid,ask,ts);
        const double mid=(bid+ask)*0.5, spread=ask-bid;
        const double drift=ind.ewm_drift;
        const double atr=(ind.atr>0)?ind.atr:spread*3.0;
        const int utch=(int)((ts/1000%86400)/3600);
        const bool asia=(utch>=22||utch<7), postny=(utch>=19&&utch<22);

        _sus(drift,ts);
        const int64_t sus_ms=(sus_dir!=0&&sus_start>0)?(ts-sus_start):0;

        if(phase==1){ _manage(bid,ask,mid,ts,atr,asia); return; }
        if(phase==2){ if(ts-cool_start>=cool_ms)phase=0; else return; }

        // Adverse excursion block
        if(gates&&adv_blk&&adv_dir!=0){
            const double dist=(adv_dir==+1)?(adv_px-mid):(mid-adv_px);
            const bool same=(drift>0&&adv_dir==+1)||(drift<0&&adv_dir==-1);
            if(same&&dist>adv_atr*0.5)return;
            adv_blk=false;
        }

        // Opposite direction cooldown
        if(last_close_ms>0&&last_dir!=0&&(ts-last_close_ms)<CFE_OPPOSITE_DIR_COOLDOWN_MS){
            const int id=(drift>0)?+1:-1;
            if(id!=last_dir)return;
        }

        // DFE threshold
        dfe_thresh=asia?std::max(4.0,atr*0.40):std::max(CFE_DFE_DRIFT_THRESH,atr*0.30);

        // DFE persist
        if(std::fabs(drift)>=dfe_thresh){
            const int d=(drift>0)?1:-1;
            if(d==dfe_pdir)++dfe_ptks; else{dfe_ptks=1;dfe_pdir=d;}
        } else{dfe_ptks=0;dfe_pdir=0;}

        // ── DFE path ──────────────────────────────────────────────────────
        if(ind.rsi_warm&&std::fabs(drift)>=dfe_thresh){
            const double delta=drift-prev_drift;
            const bool accel=dfe_warm&&((drift>0&&delta>=CFE_DFE_DRIFT_ACCEL)||(drift<0&&delta<=-CFE_DFE_DRIFT_ACCEL));
            prev_drift=drift; dfe_warm=true;
            const bool dl=(drift>0);
            const bool rok=dl?(ind.rsi_trend>CFE_DFE_RSI_THRESH&&ind.rsi_trend<CFE_DFE_RSI_TREND_MAX)
                              :(ind.rsi_trend<-CFE_DFE_RSI_THRESH&&ind.rsi_trend>-CFE_DFE_RSI_TREND_MAX);
            const bool rlvl=dl?(ind.rsi_cur>=CFE_DFE_RSI_LEVEL_LONG_MIN):(ind.rsi_cur<=CFE_DFE_RSI_LEVEL_SHORT_MAX);
            const bool pers=(dfe_ptks>=CFE_DFE_DRIFT_PERSIST_TICKS);
            bool pconf=true;
            if(ind.rc2>=CFE_DFE_PRICE_CONFIRM_TICKS){
                const double net=mid-ind.oldest_rm();
                pconf=dl?(net>=CFE_DFE_PRICE_CONFIRM_MIN):(net<=-CFE_DFE_PRICE_CONFIRM_MIN);
            }
            const double cost=spread+CFE_COST_SLIPPAGE*2+CFE_COMMISSION_PTS*2;
            const bool spok=spread<cost*CFE_DFE_MIN_SPREAD_MULT;
            const bool cdok=ts>=dfe_cool;
            const bool atok=atr<=CFE_MAX_ATR_ENTRY;
            if(accel&&rok&&rlvl&&pers&&pconf&&spok&&cdok&&atok){
                _enter(dl,bid,ask,spread,atr,ts,"DFE"); return;
            }
        }

        // ── Sustained-drift path ──────────────────────────────────────────
        if(!asia&&sus_ms>=CFE_DFE_DRIFT_SUSTAINED_MS){
            const bool sl=(sus_dir==1);
            const bool r2=sl?(ind.rsi_trend>0):(ind.rsi_trend<0);
            const bool r3=sl?(ind.rsi_cur>=40):(ind.rsi_cur<=60);
            const double cost=spread+CFE_COST_SLIPPAGE*2+CFE_COMMISSION_PTS*2;
            const bool sp2=spread<cost*CFE_DFE_MIN_SPREAD_MULT;
            const bool cd2=ts>=dfe_cool, at2=atr<=CFE_MAX_ATR_ENTRY;
            if(r2&&r3&&sp2&&cd2&&at2){ sus_start=ts; _enter(sl,bid,ask,spread,atr,ts,"SUS"); return; }
        }

        // ── Bar-close path ────────────────────────────────────────────────
        if(!bar||!ind.rsi_warm)return;
        if(gates&&postny)return;
        if(gates&&std::fabs(drift)<0.3)return;

        // Trend context
        if(sus_ms>=CFE_BAR_TREND_BLOCK_MS){
            if(sus_dir==-1&&ind.cc>ind.co)return;
            if(sus_dir==+1&&ind.cc<ind.co)return;
        }

        const int rd=ind.rsi_dir(); if(rd==0)return;

        // RSI/drift agreement (gated)
        if(gates){
            if(rd==+1&&drift<0.0)return;
            if(rd==-1&&drift>0.0)return;
        }

        // Candle body
        const double body=ind.cc-ind.co, brange=ind.ch-ind.cl;
        if(brange==0)return;
        if(std::fabs(body)/brange<CFE_BODY_RATIO_MIN)return;
        if(rd==+1&&body<=0)return;
        if(rd==-1&&body>=0)return;

        // Cost coverage
        const double cost=spread+CFE_COST_SLIPPAGE*2+CFE_COMMISSION_PTS*2;
        if(brange<CFE_COST_MULT*cost)return;
        if(atr>CFE_MAX_ATR_ENTRY)return;

        // VWAP filter (gated)
        if(gates&&ind.vwap>0&&std::fabs(drift)<1.0){
            const bool il=(drift>0);
            if(il&&mid>ind.vwap)return;
            if(!il&&mid<ind.vwap)return;
        }

        // Chop gate (gated)
        if(gates&&ind.vol_ratio()>CHOP_VOL_RATIO&&std::fabs(drift)<CHOP_DRIFT_ABS)return;

        _enter(rd==+1,bid,ask,spread,atr,ts,"BAR");
    }

    void _enter(bool il, double bid, double ask, double spread, double atr,
                int64_t ts, const char* et) noexcept {
        const double ep=il?ask:bid;
        const double slpt=(atr>0)?atr:spread*5.0;
        const double slpx=il?(ep-slpt):(ep+slpt);
        double sz=CFE_RISK_DOLLARS/(slpt*100.0);
        sz=std::floor(sz/0.001)*0.001;
        sz=std::max(CFE_MIN_LOT,std::min(CFE_MAX_LOT,sz));
        const double cost=spread+CFE_COST_SLIPPAGE*2+CFE_COMMISSION_PTS*2;
        pos={ep,slpx,slpx,sz,sz,cost,atr,0,ts,il,false,false,true};
        strncpy(pos.etype,et,3); pos.etype[3]=0;
        ++trade_id; phase=1;
    }

    void _manage(double bid, double ask, double mid, int64_t ts, double atr, bool asia) noexcept {
        const double move=pos.is_long?(mid-pos.entry):(pos.entry-mid);
        if(move>pos.mfe)pos.mfe=move;
        const int64_t hms=ts-pos.ets;

        // Partial TP
        if(!pos.partial&&pos.mfe>=pos.cost*2.0){
            const double px=pos.is_long?bid:ask;
            const double pnl=(pos.is_long?(px-pos.entry):(pos.entry-px))*pos.full_size*0.5;
            _rec(px,"PARTIAL_TP",ts,pos.full_size*0.5,pnl,hms);
            pos.partial=true; pos.size=pos.full_size*0.5;
        }

        // Trail SL
        if(pos.mfe>=pos.atr*2.0){
            const double td=pos.atr*0.5;
            const double nt=pos.is_long?(mid-td):(mid+td);
            if(!pos.trail){
                if((pos.is_long&&nt>pos.sl)||(!pos.is_long&&nt<pos.sl)){pos.trail_sl=nt;pos.trail=true;}
            } else {
                if((pos.is_long&&nt>pos.trail_sl)||(!pos.is_long&&nt<pos.trail_sl))pos.trail_sl=nt;
            }
        }

        // SL hit
        const double esl=pos.trail?pos.trail_sl:pos.sl;
        if((pos.is_long&&bid<=esl)||(!pos.is_long&&ask>=esl)){
            const double px=pos.is_long?bid:ask;
            const double pnl=(pos.is_long?(px-pos.entry):(pos.entry-px))*pos.size;
            _close(px,pos.trail?"TRAIL_SL":"SL_HIT",ts,pnl,hms); return;
        }

        // Stagnation
        const int64_t stag=asia?CFE_STAGNATION_MS_ASIA:CFE_STAGNATION_MS_LONDON;
        if(hms>=stag&&pos.mfe<pos.cost*CFE_STAGNATION_MULT){
            const double px=pos.is_long?bid:ask;
            const double pnl=(pos.is_long?(px-pos.entry):(pos.entry-px))*pos.size;
            _close(px,"STAGNATION",ts,pnl,hms);
        }
    }

    void _rec(double xpx, const char* rsn, int64_t ts, double sz, double pnl, int64_t hms) noexcept {
        TradeRec t{};
        t.entry_ms=pos.ets; t.exit_ms=ts;
        t.entry=pos.entry; t.exit_px=xpx; t.sl=pos.sl; t.size=sz;
        t.pnl_pts=pos.is_long?(xpx-pos.entry):(pos.entry-xpx);
        t.pnl_usd=pnl*100.0; t.mfe=pos.mfe;
        t.hold_s=(int)(hms/1000); t.is_long=pos.is_long;
        strncpy(t.reason,rsn,23); t.reason[23]=0;
        strncpy(t.etype,pos.etype,3); t.etype[3]=0;
        trades.push_back(t);
    }

    void _close(double xpx, const char* rsn, int64_t ts, double pnl, int64_t hms) noexcept {
        _rec(xpx,rsn,ts,pos.size,pnl,hms);
        const double adv=pos.is_long?(pos.entry-xpx):(xpx-pos.entry);
        if(adv>0&&pos.atr>0&&adv>=pos.atr*0.5){
            adv_px=xpx; adv_dir=pos.is_long?+1:-1; adv_atr=pos.atr; adv_blk=true;
        }
        last_dir=pos.is_long?+1:-1; last_close_ms=ts;
        const bool win=(strcmp(rsn,"PARTIAL_TP")==0||strcmp(rsn,"TRAIL_SL")==0);
        cool_ms=strcmp(rsn,"STAGNATION")==0?60'000:strcmp(rsn,"FORCE_CLOSE")==0?300'000
               :strcmp(rsn,"IMB_EXIT")==0?30'000:win?30'000:15'000;
        if(strcmp(rsn,"FORCE_CLOSE")==0)     dfe_cool=ts+300'000;
        else if(strcmp(rsn,"SL_HIT")==0||strcmp(rsn,"TRAIL_SL")==0) dfe_cool=ts+CFE_DFE_COOLDOWN_MS;
        else if(win) dfe_cool=ts+CFE_WINNER_COOLDOWN_MS;
        pos=Pos{}; phase=2; cool_start=ts;
    }

    void _sus(double drift, int64_t ts) noexcept {
        const int nd=(drift>=CFE_DFE_DRIFT_SUSTAINED_THRESH)?1:(drift<=-CFE_DFE_DRIFT_SUSTAINED_THRESH)?-1:0;
        if(nd!=0&&nd==sus_dir)return;
        if(nd!=0){sus_dir=nd;sus_start=ts;} else{sus_dir=0;sus_start=0;}
    }

    void flush(int64_t ts) noexcept {
        if(phase!=1)return;
        const double px=pos.entry;
        _close(px,"FORCE_CLOSE",ts,0,ts-pos.ets);
    }
};

struct CFE_Gates   : Engine<CFE_Gates>   {};
struct CFE_NoGates : Engine<CFE_NoGates> {};

// =============================================================================
// Statistics
// =============================================================================
struct Stats {
    const char* name="";
    int64_t n=0,wins=0;
    double  pnl=0,peak=0,dd=0;
    int64_t hsum=0;
    // By hour
    int    bhn[24]={};  double bhp[24]={};
    // By reason
    int    brn[6]={};   double brp[6]={};
    static constexpr const char* REASONS[6]={"SL_HIT","TRAIL_SL","PARTIAL_TP","STAGNATION","FORCE_CLOSE","IMB_EXIT"};
    // By entry type
    int    etn[3]={};   double etp[3]={}; int etw[3]={};
    static constexpr const char* ETYPES[3]={"BAR","DFE","SUS"};
    // Monthly (approx 30 buckets)
    double monthly[30]={}; int64_t mo_base=0;

    void add(const TradeRec& t) {
        const bool part=(strcmp(t.reason,"PARTIAL_TP")==0);
        pnl+=t.pnl_usd;
        if(pnl>peak)peak=pnl;
        const double d=peak-pnl; if(d>dd)dd=d;
        if(part)return;
        ++n; if(t.pnl_usd>0)++wins; hsum+=t.hold_s;
        // hour
        const int h=(int)((t.entry_ms/1000%86400)/3600);
        if(h>=0&&h<24){bhn[h]++;bhp[h]+=t.pnl_usd;}
        // reason
        for(int i=0;i<6;++i) if(!strcmp(t.reason,REASONS[i])){brn[i]++;brp[i]+=t.pnl_usd;break;}
        // entry type
        for(int i=0;i<3;++i) if(!strcmp(t.etype,ETYPES[i])){etn[i]++;etp[i]+=t.pnl_usd;if(t.pnl_usd>0)etw[i]++;break;}
        // monthly (approx: seconds since 2023-09-27 / 2592000)
        const int64_t SEC0=1695772800LL; // 2023-09-27 UTC approx
        const int mo=(int)((t.exit_ms/1000-SEC0)/2592000);
        if(mo>=0&&mo<30)monthly[mo]+=t.pnl_usd;
    }

    void print(FILE* out=stdout) const {
        const double wr=n?100.0*wins/n:0;
        fprintf(out,"\n%s\n",std::string(70,'=').c_str());
        fprintf(out,"%-40s\n",name);
        fprintf(out,"%s\n",std::string(70,'=').c_str());
        fprintf(out,"Trades:       %lld\n",(long long)n);
        fprintf(out,"Net P&L:      $%.2f\n",pnl);
        fprintf(out,"Win rate:     %.1f%%\n",wr);
        fprintf(out,"Avg trade:    $%.2f\n",n?pnl/n:0.0);
        fprintf(out,"Avg hold:     %.0fs\n",n?(double)hsum/n:0.0);
        fprintf(out,"Max drawdown: $%.2f\n",dd);
        fprintf(out,"\n-- BY ENTRY TYPE --\n");
        for(int i=0;i<3;++i) if(etn[i]>0)
            fprintf(out,"  %-4s  n=%5d  pnl=$%+8.2f  wr=%.0f%%\n",
                    ETYPES[i],etn[i],etp[i],etn[i]?100.0*etw[i]/etn[i]:0.0);
        fprintf(out,"\n-- BY EXIT REASON --\n");
        for(int i=0;i<6;++i) if(brn[i]>0)
            fprintf(out,"  %-15s  n=%5d  pnl=$%+8.2f\n",REASONS[i],brn[i],brp[i]);
        fprintf(out,"\n-- BY UTC HOUR --\n");
        for(int h=0;h<24;++h){
            if(bhn[h]==0)continue;
            char bar[41]={};
            int b=std::min(40,(int)std::fabs(bhp[h])/50);
            memset(bar,bhp[h]>0?'#':'.',b); bar[b]=0;
            fprintf(out,"  %02d:00  n=%4d  pnl=$%+7.2f  %s\n",h,bhn[h],bhp[h],bar);
        }
        fprintf(out,"\n-- MONTHLY P&L (approx) --\n");
        double cum=0;
        for(int i=0;i<30;++i){
            if(monthly[i]==0.0)continue;
            cum+=monthly[i];
            // approx month label
            const int64_t sec=1695772800LL+(int64_t)i*2592000;
            const int yr=(int)(sec/31557600+1970);
            const int mo=(int)((sec%31557600)/2592000)+1;
            fprintf(out,"  %04d-%02d  %+8.2f  cum=%+9.2f\n",yr,mo,monthly[i],cum);
        }
        fprintf(out,"%s\n",std::string(70,'=').c_str());
    }
};

// =============================================================================
// Date arg
// =============================================================================
static int64_t parse_date(const char* s) {
    if(!s||strlen(s)<10)return 0;
    char buf[9]={s[0],s[1],s[2],s[3],s[5],s[6],s[8],s[9],0};
    return ymdhms_ms(buf,"00:00:00");
}

// =============================================================================
// main
// =============================================================================
int main(int argc, char** argv) {
    if(argc<2){
        fprintf(stderr,
            "Usage: cfe_bt <ticks.csv> [options]\n"
            "  --start YYYY-MM-DD\n"
            "  --end   YYYY-MM-DD\n"
            "  --trades  <file>   (default cfe_bt_trades.csv)\n"
            "  --report  <file>   (default cfe_bt_report.txt)\n"
            "  --equity  <file>   (default cfe_bt_equity.csv)\n"
            "  --diag             print hourly indicator samples\n");
        return 1;
    }

    const char* csv=argv[1];
    const char* trd="cfe_bt_trades.csv";
    const char* rep="cfe_bt_report.txt";
    const char* eq ="cfe_bt_equity.csv";
    int64_t s0=0,s1=0;
    bool diag=false;

    for(int i=2;i<argc;++i){
        if(!strcmp(argv[i],"--start")&&i+1<argc) s0=parse_date(argv[++i]);
        else if(!strcmp(argv[i],"--end")&&i+1<argc) s1=parse_date(argv[++i]);
        else if(!strcmp(argv[i],"--trades")&&i+1<argc) trd=argv[++i];
        else if(!strcmp(argv[i],"--report")&&i+1<argc) rep=argv[++i];
        else if(!strcmp(argv[i],"--equity")&&i+1<argc) eq=argv[++i];
        else if(!strcmp(argv[i],"--diag")) diag=true;
    }

    printf("[CFE_BT] Opening %s\n",csv);
    MMapFile f; if(!f.open(csv)){fprintf(stderr,"Cannot open %s\n",csv);return 1;}
    printf("[CFE_BT] File size: %.1f MB\n",f.size/1e6);

    auto ticks=parse_csv(f,s0,s1);
    if(ticks.empty()){fprintf(stderr,"No ticks\n");return 1;}

    printf("[CFE_BT] Running BOTH engines on %zu ticks...\n",ticks.size());

    CFE_Gates   eg; eg.gates=true;
    CFE_NoGates eb; eb.gates=false;

    int64_t diag_next=0;
    static constexpr int64_t DIAG_IV=3'600'000LL;

    const size_t N=ticks.size();
    for(size_t i=0;i<N;++i){
        const auto& tk=ticks[i];
        eg.on_tick(tk.bid,tk.ask,tk.ts_ms);
        eb.on_tick(tk.bid,tk.ask,tk.ts_ms);
        if(diag&&tk.ts_ms>=diag_next){
            diag_next=tk.ts_ms+DIAG_IV;
            const int h=(int)((tk.ts_ms/1000%86400)/3600);
            printf("  [DIAG] h=%02d bid=%.2f drift=%.3f rsi_cur=%.1f rsi_trend=%.3f atr=%.3f vwap=%.2f vol_r=%.2f\n",
                   h,tk.bid,eg.ind.ewm_drift,eg.ind.rsi_cur,eg.ind.rsi_trend,eg.ind.atr,eg.ind.vwap,eg.ind.vol_ratio());
        }
        if(i%10'000'000==0&&i>0)
            printf("  %zuM  gates_t=%zu  base_t=%zu\n",i/1'000'000,eg.trades.size(),eb.trades.size());
    }

    if(!ticks.empty()){const int64_t lt=ticks.back().ts_ms;eg.flush(lt);eb.flush(lt);}

    printf("[CFE_BT] Done. gates=%zu  baseline=%zu\n",eg.trades.size(),eb.trades.size());

    // Stats
    Stats sg,sb;
    sg.name="CFE_GATES (all gates ON)";
    sb.name="CFE_BASELINE (no gates)";
    for(const auto& t:eg.trades)sg.add(t);
    for(const auto& t:eb.trades)sb.add(t);

    auto print_both=[&](FILE* out){
        sg.print(out); sb.print(out);
        fprintf(out,"\n-- GATES vs BASELINE --\n");
        fprintf(out,"  trades: gates=%lld  base=%lld  diff=%lld\n",(long long)sg.n,(long long)sb.n,(long long)(sg.n-sb.n));
        fprintf(out,"  pnl:    gates=$%.2f  base=$%.2f  diff=$%.2f\n",sg.pnl,sb.pnl,sg.pnl-sb.pnl);
    };

    print_both(stdout);
    FILE* rp=fopen(rep,"w"); if(rp){print_both(rp);fclose(rp);}
    printf("[OUTPUT] Report -> %s\n",rep);

    // Trades CSV
    { FILE* ft=fopen(trd,"w");
      if(ft){
        fprintf(ft,"engine,side,etype,entry_ms,exit_ms,entry,exit,sl,size,pnl_pts,pnl_usd,hold_s,mfe,reason\n");
        auto wr=[&](const std::vector<TradeRec>& v,const char* lbl){
            for(const auto& t:v)
                fprintf(ft,"%s,%s,%s,%lld,%lld,%.3f,%.3f,%.3f,%.3f,%.4f,%.2f,%d,%.4f,%s\n",
                        lbl,t.is_long?"LONG":"SHORT",t.etype,
                        (long long)t.entry_ms,(long long)t.exit_ms,
                        t.entry,t.exit_px,t.sl,t.size,t.pnl_pts,t.pnl_usd,t.hold_s,t.mfe,t.reason);
        };
        wr(eg.trades,"CFE_GATES"); wr(eb.trades,"CFE_BASELINE");
        fclose(ft);
        printf("[OUTPUT] %zu+%zu trades -> %s\n",eg.trades.size(),eb.trades.size(),trd);
      }
    }

    // Equity CSV
    { FILE* fe=fopen(eq,"w");
      if(fe){
        fprintf(fe,"date_sec,daily_pnl,cumulative_pnl\n");
        double cum=0; int cd=-1; double dp=0;
        for(const auto& t:eg.trades){
            const int day=(int)(t.exit_ms/86'400'000LL);
            if(day!=cd){if(cd>=0){cum+=dp;fprintf(fe,"%lld,%.2f,%.2f\n",(long long)cd*86400,dp,cum);}cd=day;dp=0;}
            dp+=t.pnl_usd;
        }
        if(cd>=0){cum+=dp;fprintf(fe,"%lld,%.2f,%.2f\n",(long long)cd*86400,dp,cum);}
        fclose(fe); printf("[OUTPUT] Equity -> %s\n",eq);
      }
    }

    return 0;
}
