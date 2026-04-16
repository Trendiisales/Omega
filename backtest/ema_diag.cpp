// ema_diag.cpp -- EMA 9/15 trade-level diagnostic
// Goal: understand WHY 46.5% WR. Find which subsets win and which bleed.
//
// Analysis axes:
//   1. Session slot (London 07-09, NY 09-13, NY core 13-17, NY late 17-20)
//   2. Direction (LONG vs SHORT)
//   3. Exit type (TP vs SL vs TIMEOUT) -- timeouts are the leak
//   4. ATR at entry (volatility context)
//   5. RSI at entry (momentum context)
//   6. EMA slope at entry (trend strength)
//   7. Time of day
//   8. Day of week
//   9. Drift direction alignment
//  10. Cross-to-entry lag (how long after cross did we fire)
//
// Build: clang++ -O3 -std=c++20 -o /tmp/ema_diag ema_diag.cpp
// Run:   /tmp/ema_diag ~/Downloads/l2_ticks_2026-04-09.csv ... (6 files)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift; };

static bool load_csv(const char* path, std::vector<Tick>& out) {
    std::ifstream f(path); if (!f) return false;
    std::string line, tok; std::getline(f, line);
    if (!line.empty() && line.back()=='\r') line.pop_back();
    int cm=-1,cb=-1,ca=-1,cd=-1,ci=0;
    { std::istringstream h(line); while(std::getline(h,tok,',')) {
        if(tok=="ts_ms")cm=ci; if(tok=="bid")cb=ci;
        if(tok=="ask")ca=ci; if(tok=="ewm_drift")cd=ci; ++ci; }}
    if(cb<0||ca<0) return false;
    size_t before=out.size();
    while(std::getline(f,line)){
        if(line.empty()) continue;
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        static char buf[512]; if(line.size()>=sizeof(buf)) continue;
        memcpy(buf,line.c_str(),line.size()+1);
        const char* flds[32]; int nf=0; flds[nf++]=buf;
        for(char* c=buf;*c&&nf<32;++c) if(*c==','){*c='\0';flds[nf++]=c+1;}
        int need=std::max({cm,cb,ca,cd}); if(nf<=need) continue;
        try { Tick t; t.ms=(int64_t)std::stod(flds[cm]);
              t.bid=std::stod(flds[cb]); t.ask=std::stod(flds[ca]);
              t.drift=(cd>=0&&nf>cd)?std::stod(flds[cd]):0.0;
              out.push_back(t); } catch(...) {}
    }
    fprintf(stderr,"Loaded %s: %zu\n",path,out.size()-before);
    return true;
}

static int utc_hour(int64_t ms) { return (int)(((ms/1000LL)%86400LL)/3600LL); }
static int utc_min(int64_t ms)  { return (int)(((ms/1000LL)%3600LL)/60LL); }
static int dow(int64_t ms)      { return (int)((ms/1000LL/86400LL + 4) % 7); } // 0=Sun

static int slot(int64_t ms) {
    int h=utc_hour(ms);
    if(h>=7&&h<9)  return 1; // London open
    if(h>=9&&h<13) return 2; // London core / pre-NY
    if(h>=13&&h<17)return 3; // NY core (best)
    if(h>=17&&h<20)return 4; // NY late
    return 0;
}

// ─── M1 bar tracker ──────────────────────────────────────────────────────────
struct M1Bar { double open,high,low,close; };
struct BarTracker {
    std::deque<M1Bar> bars;
    double atr=0, rsi=50;
    std::deque<double> rg, rl; double rp=0;
    M1Bar cur{}; int64_t cbms=0;

    bool update(double bid, double ask, int64_t ms) {
        double mid=(bid+ask)*0.5;
        int64_t bms=(ms/60000LL)*60000LL;
        if(rp>0){ double chg=mid-rp;
            rg.push_back(chg>0?chg:0); rl.push_back(chg<0?-chg:0);
            if((int)rg.size()>14){rg.pop_front();rl.pop_front();}
            if((int)rg.size()==14){ double ag=0,al=0;
                for(auto x:rg)ag+=x; for(auto x:rl)al+=x; ag/=14;al/=14;
                rsi=(al==0)?100.0:100.0-100.0/(1.0+ag/al); }}
        rp=mid;
        bool nb=false;
        if(cbms==0){cur={mid,mid,mid,mid};cbms=bms;}
        else if(bms!=cbms){
            bars.push_back(cur); if((int)bars.size()>50) bars.pop_front();
            if((int)bars.size()>=2){
                double sum=0; int n=std::min(14,(int)bars.size()-1);
                for(int i=(int)bars.size()-1;i>=(int)bars.size()-n;--i){
                    auto& b=bars[i]; auto& pb=bars[i-1];
                    sum+=std::max({b.high-b.low,std::fabs(b.high-pb.close),std::fabs(b.low-pb.close)});}
                atr=sum/n;}
            cur={mid,mid,mid,mid}; cbms=bms; nb=true;
        } else { if(mid>cur.high)cur.high=mid; if(mid<cur.low)cur.low=mid; cur.close=mid; }
        return nb;
    }
};

// ─── EMA ─────────────────────────────────────────────────────────────────────
struct EMA {
    int period; double val=0,alpha; bool warmed=false; int count=0;
    EMA(int p): period(p), alpha(2.0/(p+1.0)) {}
    void update(double p) {
        if(!warmed){val=(val*count+p)/(count+1);++count;if(count>=period)warmed=true;}
        else val=p*alpha+val*(1.0-alpha);
    }
};

// ─── Trade record ─────────────────────────────────────────────────────────────
struct Trade {
    bool is_long;
    double entry, exit_px, pnl, mfe;
    double entry_atr, entry_rsi;
    double entry_fast, entry_slow;
    double entry_slope;   // fast EMA slope = fast - fast_prev (momentum)
    double entry_drift;
    int    held_s, entry_slot, entry_dow, entry_hour, entry_min;
    int    cross_lag_s;   // seconds from cross detection to entry
    std::string reason;
};

// ─── Engine (best config: 9/15 rsi_lo=40 rsi_hi=50 sl=1.5 rr=1.0) ──────────
struct Engine {
    EMA fast{9}, slow{15};
    double prev_fast=0, prev_slow=0;
    double atr=0, rsi=50;
    int cross_dir=0; int64_t cross_ms=0;
    double fast_prev2=0; // for slope

    struct Pos {
        bool active=false,is_long=false,be_done=false;
        double entry=0,sl=0,tp=0,size=0,mfe=0,atr=0;
        int64_t ets=0;
        double entry_rsi,entry_fast,entry_slow,entry_slope,entry_drift;
        int slot,dow,hour,min,cross_lag_s;
    } pos;

    int64_t last_exit=0, startup=0;
    double dpnl=0; int64_t dday=0;

    using CB = std::function<void(const Trade&)>;

    void on_bar(double close, double bar_atr, double bar_rsi, int64_t ms) {
        if(bar_atr>0.5&&bar_atr<50) atr=bar_atr;
        rsi=bar_rsi;
        double pf=fast.val, ps=slow.val;
        fast_prev2=pf;
        fast.update(close); slow.update(close);
        if(!fast.warmed||!slow.warmed) return;
        bool lx=(fast.val>slow.val)&&(pf<=ps);
        bool sx=(fast.val<slow.val)&&(pf>=ps);
        if(lx){cross_dir=1;cross_ms=ms;}
        if(sx){cross_dir=-1;cross_ms=ms;}
        prev_fast=pf; prev_slow=ps;
    }

    void on_tick(double bid, double ask, int64_t ms, double drift, CB cb) {
        if(startup==0)startup=ms;
        if(ms-startup<120000LL)return;
        int64_t day=(ms/1000LL)/86400LL;
        if(day!=dday){dpnl=0;dday=day;}
        if(dpnl<=-200.0)return;
        double mid=(bid+ask)*0.5;
        double spread=ask-bid;

        if(pos.active){
            double mv=pos.is_long?(mid-pos.entry):(pos.entry-mid);
            if(mv>pos.mfe)pos.mfe=mv;
            double td=std::fabs(pos.tp-pos.entry);
            if(!pos.be_done&&td>0&&pos.mfe>=td*0.50){pos.sl=pos.entry;pos.be_done=true;}
            double ep=pos.is_long?bid:ask;
            if(pos.is_long?(bid>=pos.tp):(ask<=pos.tp)){_close(ep,"TP",ms,cb);return;}
            if(pos.is_long?(bid<=pos.sl):(ask>=pos.sl)){
                _close(ep,pos.be_done?"BE":"SL",ms,cb);return;}
            if(ms-pos.ets>180000LL){_close(ep,"TIMEOUT",ms,cb);return;}
            if(pos.mfe>0&&((pos.is_long&&fast.val<slow.val)||(!pos.is_long&&fast.val>slow.val))){
                _close(ep,"CROSS_EXIT",ms,cb);return;}
            return;
        }

        if(!fast.warmed||!slow.warmed||cross_dir==0||atr<=0) return;
        if(ms/1000-last_exit<20) return;
        if(spread>atr*0.30) return;
        if(ms-cross_ms>180000LL){cross_dir=0;return;}

        bool isl=(cross_dir==1);
        if(isl&&(rsi<40.0||rsi>75.0))return;
        if(!isl&&(rsi>50.0||rsi<25.0))return;

        double sl_dist=atr*1.5, tp_dist=sl_dist*1.0;
        double cost=spread+0.20;
        if(tp_dist<=cost)return;

        double sz=std::max(0.01,std::min(0.10,std::floor(30.0/(sl_dist*100.0)/0.001)*0.001));
        double ep=isl?ask:bid;
        double slope=fast.val-fast_prev2;

        pos.active=true; pos.is_long=isl; pos.entry=ep;
        pos.sl=isl?(ep-sl_dist):(ep+sl_dist);
        pos.tp=isl?(ep+tp_dist):(ep-tp_dist);
        pos.size=sz; pos.mfe=0; pos.be_done=false; pos.atr=atr; pos.ets=ms;
        pos.entry_rsi=rsi; pos.entry_fast=fast.val; pos.entry_slow=slow.val;
        pos.entry_slope=slope; pos.entry_drift=drift;
        pos.slot=slot(ms); pos.dow=dow(ms); pos.hour=utc_hour(ms); pos.min=utc_min(ms);
        pos.cross_lag_s=(int)((ms-cross_ms)/1000);
        cross_dir=0;
    }

    void fc(double bid,double ask,int64_t ms,CB cb){
        if(!pos.active)return;_close(pos.is_long?bid:ask,"FC",ms,cb);}

    void _close(double ep,const char* r,int64_t ms,CB cb){
        double pnl=(pos.is_long?(ep-pos.entry):(pos.entry-ep))*pos.size*100.0;
        dpnl+=pnl; last_exit=ms/1000;
        Trade t; t.is_long=pos.is_long; t.entry=pos.entry; t.exit_px=ep;
        t.pnl=pnl; t.mfe=pos.mfe;
        t.entry_atr=pos.atr; t.entry_rsi=pos.entry_rsi;
        t.entry_fast=pos.entry_fast; t.entry_slow=pos.entry_slow;
        t.entry_slope=pos.entry_slope; t.entry_drift=pos.entry_drift;
        t.held_s=(int)((ms-pos.ets)/1000);
        t.entry_slot=pos.slot; t.entry_dow=pos.dow;
        t.entry_hour=pos.hour; t.entry_min=pos.min;
        t.cross_lag_s=pos.cross_lag_s; t.reason=r;
        cb(t); pos=Pos{};
    }
};

// ─── Bucket stats ─────────────────────────────────────────────────────────────
struct Bucket {
    int T=0,W=0; double pnl=0;
    void add(const Trade& t){++T;if(t.pnl>0)++W;pnl+=t.pnl;}
    double wr()const{return T>0?100.0*W/T:0;}
    double avg()const{return T>0?pnl/T:0;}
    void print(const char* label) const {
        if(T<3)return;
        printf("  %-32s T=%3d WR=%5.1f%% PnL=%8.2f Avg=%6.2f\n",
               label,T,wr(),pnl,avg());
    }
};

int main(int argc, char** argv) {
    if(argc<2){fprintf(stderr,"Usage: ema_diag file1.csv ...\n");return 1;}
    std::vector<Tick> ticks; ticks.reserve(2000000);
    for(int i=1;i<argc;++i)load_csv(argv[i],ticks);
    fprintf(stderr,"Total: %zu ticks\n\n",ticks.size());

    Engine eng; BarTracker bt;
    std::vector<Trade> trades;
    auto cb=[&](const Trade& t){trades.push_back(t);};
    for(auto& t:ticks){
        bool nb=bt.update(t.bid,t.ask,t.ms);
        if(nb)(void)bt.bars;
        if(nb && !bt.bars.empty()){
            eng.on_bar(bt.bars.back().close, bt.atr>0?bt.atr:1.0, bt.rsi, t.ms);
        }
        eng.on_tick(t.bid,t.ask,t.ms,t.drift,cb);
    }
    if(eng.pos.active&&!ticks.empty())
        eng.fc(ticks.back().bid,ticks.back().ask,ticks.back().ms,cb);

    printf("Total trades: %zu\n\n",(size_t)trades.size());

    // ── By direction ─────────────────────────────────────────────────────────
    printf("=== DIRECTION ===\n");
    Bucket bl,bs;
    for(auto& t:trades){if(t.is_long)bl.add(t);else bs.add(t);}
    bl.print("LONG"); bs.print("SHORT");

    // ── By session slot ───────────────────────────────────────────────────────
    printf("\n=== SESSION SLOT ===\n");
    const char* snames[]={"DEAD(00-07/20+)","London(07-09)","PreNY(09-13)","NYcore(13-17)","NYlate(17-20)"};
    Bucket bslot[5];
    for(auto& t:trades) bslot[t.entry_slot].add(t);
    for(int i=0;i<5;++i) bslot[i].print(snames[i]);

    // ── By day of week ────────────────────────────────────────────────────────
    printf("\n=== DAY OF WEEK ===\n");
    const char* dnames[]={"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    Bucket bdow[7];
    for(auto& t:trades) bdow[t.entry_dow].add(t);
    for(int i=0;i<7;++i) bdow[i].print(dnames[i]);

    // ── By exit type ──────────────────────────────────────────────────────────
    printf("\n=== EXIT TYPE ===\n");
    std::map<std::string,Bucket> bex;
    for(auto& t:trades) bex[t.reason].add(t);
    for(auto& [k,v]:bex){ char buf[64]; snprintf(buf,sizeof(buf),"%s",k.c_str()); v.print(buf); }

    // ── By RSI at entry (buckets of 5) ────────────────────────────────────────
    printf("\n=== RSI AT ENTRY ===\n");
    Bucket brsi[20]; // 0-100 in buckets of 5
    for(auto& t:trades){
        int b=std::min(19,(int)(t.entry_rsi/5));
        brsi[b].add(t);}
    for(int i=0;i<20;++i){
        char buf[32]; snprintf(buf,sizeof(buf),"RSI %d-%d",i*5,i*5+5);
        brsi[i].print(buf);}

    // ── By ATR at entry ───────────────────────────────────────────────────────
    printf("\n=== ATR AT ENTRY ===\n");
    // Buckets: <1.0, 1-2, 2-3, 3-5, 5+
    const double atr_edges[]={0,1.0,2.0,3.0,5.0,999};
    const char* atr_names[]={"ATR<1.0","ATR 1-2","ATR 2-3","ATR 3-5","ATR 5+"};
    Bucket batr[5];
    for(auto& t:trades){
        int b=0; for(int i=0;i<5;++i) if(t.entry_atr>=atr_edges[i]&&t.entry_atr<atr_edges[i+1])b=i;
        batr[b].add(t);}
    for(int i=0;i<5;++i) batr[i].print(atr_names[i]);

    // ── By EMA slope (fast - fast_prev) ──────────────────────────────────────
    printf("\n=== EMA SLOPE AT ENTRY (fast momentum) ===\n");
    // Slope buckets: <-0.3, -0.3 to -0.1, -0.1 to 0, 0 to 0.1, 0.1 to 0.3, >0.3
    Bucket bslope[6];
    const char* slope_names[]={"slope<-0.3","slope-0.3:-0.1","slope-0.1:0","slope 0:0.1","slope 0.1:0.3","slope>0.3"};
    for(auto& t:trades){
        int b;
        if(t.entry_slope<-0.3)b=0;
        else if(t.entry_slope<-0.1)b=1;
        else if(t.entry_slope<0)b=2;
        else if(t.entry_slope<0.1)b=3;
        else if(t.entry_slope<0.3)b=4;
        else b=5;
        bslope[b].add(t);}
    for(int i=0;i<6;++i) bslope[i].print(slope_names[i]);

    // ── By drift alignment ───────────────────────────────────────────────────
    printf("\n=== DRIFT ALIGNMENT ===\n");
    Bucket bd_aligned, bd_against, bd_neutral;
    for(auto& t:trades){
        bool aligned=(t.is_long&&t.entry_drift>0.5)||((!t.is_long)&&t.entry_drift<-0.5);
        bool against=(t.is_long&&t.entry_drift<-0.5)||((!t.is_long)&&t.entry_drift>0.5);
        if(aligned)bd_aligned.add(t);
        else if(against)bd_against.add(t);
        else bd_neutral.add(t);}
    bd_aligned.print("drift_aligned(>0.5)");
    bd_neutral.print("drift_neutral(-0.5:0.5)");
    bd_against.print("drift_against(<-0.5)");

    // ── By cross-to-entry lag ─────────────────────────────────────────────────
    printf("\n=== CROSS LAG (s from cross to entry) ===\n");
    Bucket blag[6]; // 0-30, 30-60, 60-90, 90-120, 120-150, 150-180
    const char* lag_names[]={"lag 0-30s","lag 30-60s","lag 60-90s","lag 90-120s","lag 120-150s","lag 150-180s"};
    for(auto& t:trades){
        int b=std::min(5,(int)(t.cross_lag_s/30));
        blag[b].add(t);}
    for(int i=0;i<6;++i) blag[i].print(lag_names[i]);

    // ── By hour of day ────────────────────────────────────────────────────────
    printf("\n=== HOUR OF DAY (UTC) ===\n");
    Bucket bhour[24];
    for(auto& t:trades) bhour[t.entry_hour].add(t);
    for(int i=0;i<24;++i){
        char buf[16]; snprintf(buf,sizeof(buf),"UTC %02d:00",i);
        bhour[i].print(buf);}

    // ── TIMEOUT deep dive -- what was price doing ────────────────────────────
    printf("\n=== TIMEOUT TRADES: MFE distribution ===\n");
    std::vector<double> tmfes;
    for(auto& t:trades) if(t.reason=="TIMEOUT"||t.reason=="BE") tmfes.push_back(t.mfe);
    if(!tmfes.empty()){
        std::sort(tmfes.begin(),tmfes.end());
        printf("  Count=%zu MFE p25=%.3f p50=%.3f p75=%.3f p90=%.3f max=%.3f\n",
               tmfes.size(),
               tmfes[tmfes.size()*25/100],tmfes[tmfes.size()*50/100],
               tmfes[tmfes.size()*75/100],tmfes[tmfes.size()*90/100],
               tmfes.back());}

    // ── LONG vs SHORT by slot ─────────────────────────────────────────────────
    printf("\n=== LONG vs SHORT BY SLOT ===\n");
    for(int s=0;s<5;++s){
        Bucket ll,ss;
        for(auto& t:trades){if(t.entry_slot!=s)continue; if(t.is_long)ll.add(t);else ss.add(t);}
        if(ll.T>0||ss.T>0){
            char lb[48],sb[48];
            snprintf(lb,sizeof(lb),"%s LONG",snames[s]);
            snprintf(sb,sizeof(sb),"%s SHORT",snames[s]);
            ll.print(lb); ss.print(sb);
        }
    }

    // ── EMA spread (fast-slow gap) at entry ──────────────────────────────────
    printf("\n=== EMA SPREAD AT ENTRY (fast-slow gap) ===\n");
    // Small gap = fresh cross, large gap = late entry
    Bucket bgap[5]; // <0.05, 0.05-0.15, 0.15-0.30, 0.30-0.50, >0.50
    const char* gap_names[]={"gap<0.05","gap 0.05-0.15","gap 0.15-0.30","gap 0.30-0.50","gap>0.50"};
    const double gap_edges[]={0,0.05,0.15,0.30,0.50,999};
    for(auto& t:trades){
        double gap=std::fabs(t.entry_fast-t.entry_slow);
        int b=0; for(int i=0;i<5;++i) if(gap>=gap_edges[i]&&gap<gap_edges[i+1])b=i;
        bgap[b].add(t);}
    for(int i=0;i<5;++i) bgap[i].print(gap_names[i]);

    return 0;
}
