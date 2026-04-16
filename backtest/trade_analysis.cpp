// trade_analysis.cpp — run best CFE config, record every trade, find WR filters
// Best config from 6-day sweep: rn=30 rt=3.0 dm=0.2 st=0.2 sm=60s sl=0.40 tp=4.0 mh=600s
// Analyses: time-of-day, direction, MFE distribution, drift at entry, hold time
//
// Build: clang++ -O3 -std=c++20 -o /tmp/trade_analysis /tmp/trade_analysis.cpp
// Run:   /tmp/trade_analysis ~/Downloads/l2_ticks_2026-04-*.csv

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

struct Tick { int64_t ms; float bid,ask,drift,atr; };

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path); if(!f) return out;
    std::string line,tok; std::getline(f,line);
    if(!line.empty()&&line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,ci=0;
    { std::istringstream h(line);
      while(std::getline(h,tok,',')) {
        if(tok=="ts_ms"||tok=="timestamp_ms") cm=ci;
        if(tok=="bid") cb=ci; if(tok=="ask") ca=ci;
        if(tok=="ewm_drift") cd=ci; ++ci; } }
    if(cb<0||ca<0) return out;
    float pb=0,av=2.f; std::deque<float> aw;
    float drift_ema=0,prev_mid=0;
    out.reserve(300000);
    while(std::getline(f,line)) {
        if(line.empty()) continue;
        if(line.back()=='\r') line.pop_back();
        static char buf[512]; if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        static const char* flds[32]; int nf=0; flds[nf++]=buf;
        for(char* c=buf;*c;++c) if(*c==','){*c='\0';if(nf<32)flds[nf++]=c+1;}
        int need=cb>ca?cb:ca; if(cm>need)need=cm; if(nf<=need) continue;
        char* e; Tick t;
        t.ms=(cm>=0)?(int64_t)strtod(flds[cm],&e):0;
        t.bid=(float)strtod(flds[cb],&e); t.ask=(float)strtod(flds[ca],&e);
        t.drift=(cd>=0&&cd<nf)?(float)strtod(flds[cd],&e):0.f; t.atr=0.f;
        if(t.bid<=0||t.ask<t.bid) continue;
        float mid=(t.bid+t.ask)*.5f;
        if(cd<0) { if(prev_mid>0) drift_ema=0.1f*(mid-prev_mid)+0.9f*drift_ema; t.drift=drift_ema; }
        prev_mid=mid;
        if(pb>0){float tr=(t.ask-t.bid)+std::fabs(t.bid-pb);
            aw.push_back(tr);if((int)aw.size()>200)aw.pop_front();
            if((int)aw.size()>=50){float s=0;for(float x:aw)s+=x;av=s/aw.size()*14.f;}}
        t.atr=av; pb=t.bid; out.push_back(t);
    }
    return out;
}

struct Trade {
    int64_t entry_ms, exit_ms;
    float   entry_px, exit_px, mfe, pnl, drift_at_entry, atr_at_entry;
    int64_t dsms_at_entry;   // how long drift was sustained at entry
    float   rv_at_entry;     // RSI slope EMA at entry
    bool    is_long;
    char    exit_reason;     // 'T'=TP 'S'=SL 'O'=timeout
};

static constexpr int RMAX=32, MMAX=10;

struct Eng {
    // params (best config from 6-day sweep rn=30 cluster)
    int   rn=30; float rt=3.0f,rm=15.f,dm=0.2f,st=0.2f;
    int64_t sm=60000,cd=15000,mh=600000;
    float sl=0.40f,tp=4.0f;
    int   warmup=300;

    float rg[RMAX]={},rl[RMAX]={};
    int rhead=0,rcount=0;
    float rpm=0,rc=50,rp=50,rv=0; bool rw=false;
    float ra=2.f/31.f;
    float mb[MMAX]={}; int mhead=0,mcount=0;
    int dsd=0; int64_t dss=0;
    bool pa=false,pl=false,pbe=false;
    float pe=0,psl=0,ptp=0,pm=0,ps=0.01f;
    int64_t pts=0,cdu=0,lcm=0; int lcd=0,wt=0;
    bool ab=false; float lex=0; int lld=0;
    float entry_drift=0,entry_rv=0,entry_atr=0; int64_t entry_dsms=0;

    std::vector<Trade> trades;

    void ursi(float mid) {
        if(!rpm){rpm=mid;return;}
        float c=mid-rpm; rpm=mid;
        rg[rhead]=c>0?c:0.f; rl[rhead]=c<0?-c:0.f;
        rhead=(rhead+1)%RMAX;
        if(rcount<rn)++rcount;
        if(rcount>=rn){
            float ag=0,al=0;
            for(int i=0;i<rn;++i){int idx=(rhead-1-i+RMAX)%RMAX;ag+=rg[idx];al+=rl[idx];}
            ag/=rn;al/=rn;
            rp=rc;rc=al==0?100.f:100.f-100.f/(1.f+ag/al);
            float s=rc-rp;
            if(!rw){rv=s;rw=true;}else rv=s*ra+rv*(1-ra);
        }
    }

    void close(float ep,bool il,int64_t ms,char reason) {
        float pp=il?(ep-pe):(pe-ep),pu=pp*ps*100.f;
        Trade t;
        t.entry_ms=pts; t.exit_ms=ms;
        t.entry_px=pe; t.exit_px=ep;
        t.mfe=pm; t.pnl=pu; t.is_long=il;
        t.drift_at_entry=entry_drift;
        t.rv_at_entry=entry_rv;
        t.atr_at_entry=entry_atr;
        t.dsms_at_entry=entry_dsms;
        t.exit_reason=reason;
        trades.push_back(t);
        lcd=il?1:-1;lcm=ms;
        if(pu<0){ab=true;lex=ep;lld=il?1:-1;cdu=ms+cd;}
        pa=false;
    }

    void tick(int64_t ms,float bid,float ask,float drift,float atr) {
        float mid=(bid+ask)*.5f,sp=ask-bid;
        ++wt; ursi(mid);
        mb[mhead%MMAX]=mid;++mhead;if(mcount<MMAX)++mcount;
        if(drift>=st){if(dsd!=1){dsd=1;dss=ms;}}
        else if(drift<=-st){if(dsd!=-1){dsd=-1;dss=ms;}}
        else{dsd=0;dss=0;}
        int64_t dsms=dsd?(ms-dss):0;

        if(pa){
            float mv=pl?(mid-pe):(pe-mid);if(mv>pm)pm=mv;
            float eff=pl?bid:ask;
            if(!pbe&&std::fabs(ptp-pe)>0&&mv>=std::fabs(ptp-pe)*.5f){psl=pe;pbe=true;}
            if((pl&&bid>=ptp)||(!pl&&ask<=ptp)){close(eff,pl,ms,'T');return;}
            if((pl&&bid<=psl)||(!pl&&ask>=psl)){close(eff,pl,ms,'S');return;}
            if(ms-pts>mh){close(eff,pl,ms,'O');return;}
            return;
        }

        if(wt<warmup||sp>0.40f||atr<0.5f) return;
        if(ms<cdu||!rw) return;
        int rd=0;
        if(rv>rt&&rv<rm) rd=1; else if(rv<-rt&&rv>-rm) rd=-1;
        if(!rd) return;
        if(rd==1&&drift<0) return; if(rd==-1&&drift>0) return;
        if(std::fabs(drift)<dm) return; if(dsms<sm) return;
        if(mcount>=3){float n3=mb[(mhead-3+MMAX*2)%MMAX],mv3=mid-n3;
            if(rd==1&&mv3<-atr*.3f) return; if(rd==-1&&mv3>atr*.3f) return;}
        if(lcd&&(ms-lcm)<45000LL&&rd!=lcd) return;
        if(ab&&lld){float dist=(lld==1)?(lex-mid):(mid-lex);
            bool same=(rd==1&&lld==1)||(rd==-1&&lld==-1);
            if(same&&dist>atr*.4f) return; else ab=false;}

        bool il=(rd==1);
        float sl_=atr*sl,tp_=sl_*tp,e=il?ask:bid;
        pa=true;pl=il;pe=e;pm=0;pbe=false;
        psl=il?(e-sl_):(e+sl_);ptp=il?(e+tp_):(e-tp_);pts=ms;
        float raw=10.f/(sl_*100.f);
        ps=std::min(.10f,std::max(.01f,std::round(raw/.01f)*.01f));
        entry_drift=drift; entry_rv=rv; entry_atr=atr; entry_dsms=dsms;
    }
};

static void bucket(const char* label, const std::vector<Trade>& trades,
                   std::function<bool(const Trade&)> filter) {
    int n=0,wins=0; float pnl=0,mfe_sum=0;
    for(auto& t:trades) if(filter(t)){++n;pnl+=t.pnl;mfe_sum+=t.mfe;if(t.pnl>0)++wins;}
    if(n<3) return;
    printf("  %-35s N=%3d  WR=%5.1f%%  PnL=$%+7.2f  AvgMFE=%.3f\n",
           label,n,100.f*wins/n,pnl,mfe_sum/n);
}

int main(int argc,char* argv[]) {
    if(argc<2){puts("Usage: trade_analysis <csv> [...]");return 1;}

    std::vector<Tick> ticks;
    for(int i=1;i<argc;++i) {
        auto v=load_csv(argv[i]);
        if(v.size()<1000){fprintf(stderr,"Skip %s\n",argv[i]);continue;}
        fprintf(stderr,"Loaded %s: %zu\n",argv[i],v.size());
        ticks.insert(ticks.end(),v.begin(),v.end());
    }
    std::stable_sort(ticks.begin(),ticks.end(),[](const Tick&a,const Tick&b){return a.ms<b.ms;});
    printf("Total: %zu ticks\n",ticks.size());

    Eng eng;
    for(auto& tk:ticks) eng.tick(tk.ms,tk.bid,tk.ask,tk.drift,tk.atr);

    auto& T=eng.trades;
    int n=(int)T.size(),wins=0; float tot=0;
    for(auto& t:T){tot+=t.pnl;if(t.pnl>0)++wins;}
    printf("\nTotal trades: %d  WR=%.1f%%  PnL=$%.2f  AvgPnL=$%.2f\n\n",
           n,n?100.f*wins/n:0,tot,n?tot/n:0);

    // ── Direction ─────────────────────────────────────────────────────────────
    printf("=== DIRECTION ===\n");
    bucket("LONG",  T,[](const Trade& t){return  t.is_long;});
    bucket("SHORT", T,[](const Trade& t){return !t.is_long;});

    // ── Exit reason ───────────────────────────────────────────────────────────
    printf("\n=== EXIT REASON ===\n");
    bucket("TP hit",  T,[](const Trade& t){return t.exit_reason=='T';});
    bucket("SL hit",  T,[](const Trade& t){return t.exit_reason=='S';});
    bucket("Timeout", T,[](const Trade& t){return t.exit_reason=='O';});

    // ── Time of day (UTC hour at entry) ───────────────────────────────────────
    printf("\n=== TIME OF DAY (UTC hour at entry) ===\n");
    for(int h=0;h<24;h+=2) {
        char label[32]; snprintf(label,sizeof(label),"%02d:00-%02d:00 UTC",h,h+2);
        bucket(label,T,[h](const Trade& t){
            int eh=(int)((t.entry_ms/1000)%86400/3600);
            return eh>=h&&eh<h+2;});
    }

    // ── Drift at entry ────────────────────────────────────────────────────────
    printf("\n=== DRIFT AT ENTRY (abs) ===\n");
    bucket("|drift| 0.2-0.5", T,[](const Trade& t){float d=std::fabs(t.drift_at_entry);return d>=0.2f&&d<0.5f;});
    bucket("|drift| 0.5-1.0", T,[](const Trade& t){float d=std::fabs(t.drift_at_entry);return d>=0.5f&&d<1.0f;});
    bucket("|drift| 1.0-2.0", T,[](const Trade& t){float d=std::fabs(t.drift_at_entry);return d>=1.0f&&d<2.0f;});
    bucket("|drift| 2.0+",    T,[](const Trade& t){return std::fabs(t.drift_at_entry)>=2.0f;});

    // ── RSI slope at entry ────────────────────────────────────────────────────
    printf("\n=== RSI SLOPE AT ENTRY (abs) ===\n");
    bucket("|rv| 3-5",  T,[](const Trade& t){float r=std::fabs(t.rv_at_entry);return r>=3.f&&r<5.f;});
    bucket("|rv| 5-8",  T,[](const Trade& t){float r=std::fabs(t.rv_at_entry);return r>=5.f&&r<8.f;});
    bucket("|rv| 8-12", T,[](const Trade& t){float r=std::fabs(t.rv_at_entry);return r>=8.f&&r<12.f;});

    // ── Sustained duration at entry ───────────────────────────────────────────
    printf("\n=== SUSTAINED DURATION AT ENTRY ===\n");
    bucket("dsms 60-90s",  T,[](const Trade& t){return t.dsms_at_entry>=60000&&t.dsms_at_entry<90000;});
    bucket("dsms 90-120s", T,[](const Trade& t){return t.dsms_at_entry>=90000&&t.dsms_at_entry<120000;});
    bucket("dsms 120-180s",T,[](const Trade& t){return t.dsms_at_entry>=120000&&t.dsms_at_entry<180000;});
    bucket("dsms 180s+",   T,[](const Trade& t){return t.dsms_at_entry>=180000;});

    // ── MFE distribution ─────────────────────────────────────────────────────
    printf("\n=== MFE AT LOSS (what profit did losers show before reversing) ===\n");
    printf("  (if losers show high MFE -> need BE stop, if low -> wrong direction entry)\n");
    int loss_no_mfe=0,loss_some_mfe=0,loss_big_mfe=0;
    float sl_pts_avg=0;
    for(auto& t:T) {
        if(t.pnl>=0) continue;
        float sl_pts=std::fabs(t.exit_px-t.entry_px);
        sl_pts_avg+=sl_pts;
        if(t.mfe<0.10f)       ++loss_no_mfe;
        else if(t.mfe<sl_pts*0.5f) ++loss_some_mfe;
        else                  ++loss_big_mfe;
    }
    int losses=n-wins;
    if(losses>0) {
        sl_pts_avg/=losses;
        printf("  Losers with MFE<0.10pt (wrong dir entry): %d (%.0f%%)\n",
               loss_no_mfe,100.f*loss_no_mfe/losses);
        printf("  Losers with MFE<0.5xSL (small pullback):  %d (%.0f%%)\n",
               loss_some_mfe,100.f*loss_some_mfe/losses);
        printf("  Losers with MFE>0.5xSL (reversed from profit): %d (%.0f%%)\n",
               loss_big_mfe,100.f*loss_big_mfe/losses);
        printf("  Avg loss SL pts: %.3f\n",sl_pts_avg);
    }

    // ── ATR at entry ──────────────────────────────────────────────────────────
    printf("\n=== ATR AT ENTRY ===\n");
    bucket("ATR 1.0-2.0", T,[](const Trade& t){return t.atr_at_entry>=1.f&&t.atr_at_entry<2.f;});
    bucket("ATR 2.0-3.0", T,[](const Trade& t){return t.atr_at_entry>=2.f&&t.atr_at_entry<3.f;});
    bucket("ATR 3.0-4.0", T,[](const Trade& t){return t.atr_at_entry>=3.f&&t.atr_at_entry<4.f;});
    bucket("ATR 4.0+",    T,[](const Trade& t){return t.atr_at_entry>=4.f;});

    // ── Combined: best time+direction filter ──────────────────────────────────
    printf("\n=== COMBINED: London/NY + direction (07-20 UTC) ===\n");
    bucket("London/NY LONG  (07-20 UTC)", T,[](const Trade& t){
        int h=(int)((t.entry_ms/1000)%86400/3600);
        return t.is_long&&h>=7&&h<20;});
    bucket("London/NY SHORT (07-20 UTC)", T,[](const Trade& t){
        int h=(int)((t.entry_ms/1000)%86400/3600);
        return !t.is_long&&h>=7&&h<20;});
    bucket("Asia LONG       (20-07 UTC)", T,[](const Trade& t){
        int h=(int)((t.entry_ms/1000)%86400/3600);
        return t.is_long&&(h>=20||h<7);});
    bucket("Asia SHORT      (20-07 UTC)", T,[](const Trade& t){
        int h=(int)((t.entry_ms/1000)%86400/3600);
        return !t.is_long&&(h>=20||h<7);});

    return 0;
}
