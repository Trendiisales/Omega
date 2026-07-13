// gold_session_tp_bt.cpp — faithful backtest of the operator's "Gold Session
// Trend-Pullback Engine" spec (2026-07-14). M1-grade fills, 5m decisions,
// DST-correct London/NY sessions, full decision chain:
//   session -> regime(VWAP+EMA15 20/50+EMA60 slope+PDmid) -> 30m OR breakout
//   -> impulse qualifiers -> first controlled pullback (25-55%) -> micro-break
//   stop entry -> structural stop (pb low - 0.10*ATR5) -> cost/ATR stop gates
//   -> partial 25% @1.25R -> true-BE arm -> hybrid trail (wider of 2*ATR5 or
//   3-bar swing) -> VWAP-cross / slope-reversal / violent-impulse / session exits
//   -> 1 win/session, 2 initial losses/day, 3 entries/day.
// Costs: IBKR XAU real (1.5bp/side commission + $0.30 spread + slip), COST_MULT env.
// VWAP note: tape has no volume -> equal-weight M1 typical-price VWAP (limitation).
// Activity/tick-volume qualifier: not available on bar tape -> range qualifier carries it.
// ENV: BE_ARM(0.8|1.5) NEWS(0|1: NFP first-Fri block) LBMA(0|1) COST_MULT SESS(BOTH|LDN|NY)
//      IMPULSE(1|0) PULLBACK(1|0 immediate-break entry for ablation) REGIME(1|0)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>

struct M1 { long ts; double o,h,l,c; };
static std::vector<M1> tape;

// ---------- calendar helpers (all on UTC struct tm) ----------
static int wday_of(int y,int m,int d){ struct tm t{}; t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;t.tm_hour=12; time_t tt=timegm(&t); return gmtime(&tt)->tm_wday; }
static int nth_sunday(int y,int m,int nth){ int c=0; for(int d=1;d<=31;d++){ if(wday_of(y,m,d)==0){ c++; if(c==nth) return d; } } return -1; }
static int last_sunday(int y,int m){ int last=-1; for(int d=1;d<=31;d++){ struct tm t{}; t.tm_year=y-1900;t.tm_mon=m-1;t.tm_mday=d;t.tm_hour=12; time_t tt=timegm(&t); struct tm*g=gmtime(&tt); if(g->tm_mon==m-1 && g->tm_wday==0) last=d; } return last; }
// date key
static long dkey(int y,int m,int d){ return y*10000L+m*100+d; }

struct DstYear { long us_start, us_end, uk_start, uk_end; };
static DstYear dst(int y){
    return { dkey(y,3,nth_sunday(y,3,2)), dkey(y,11,nth_sunday(y,11,1)),
             dkey(y,3,last_sunday(y,3)),  dkey(y,10,last_sunday(y,10)) };
}
// offsets: ET = UTC + et_off (negative). London = UTC + ldn_off.
static void offsets(const struct tm& g, int& et_off, int& ldn_off){
    static int cy=-1; static DstYear D;
    int y=g.tm_year+1900;
    if(y!=cy){ D=dst(y); cy=y; }
    long k=dkey(y,g.tm_mon+1,g.tm_mday);
    et_off  = (k>=D.us_start && k<D.us_end) ? -4 : -5;
    ldn_off = (k>=D.uk_start && k<D.uk_end) ?  1 :  0;
}

// FOMC decision dates (14:00 ET) 2022-2026H1 — moot for entries (sessions end 11:30 ET) but kept for completeness.
static const long FOMC[] = {20220126,20220316,20220504,20220615,20220727,20220921,20221102,20221214,
 20230201,20230322,20230503,20230614,20230726,20230920,20231101,20231213,
 20240131,20240320,20240501,20240612,20240731,20240918,20241107,20241218,
 20250129,20250319,20250507,20250618,20250730,20250917,20251029,20251210,
 20260128,20260318,20260429,20260617};
static bool is_fomc(long k){ for(long f:FOMC) if(f==k) return true; return false; }
// NFP: first Friday of month (computed).
static bool is_first_friday(int y,int m,int d){ if(wday_of(y,m,d)!=5) return false; return d<=7; }

// ---------- indicators ----------
struct Ema { double v=0; bool init=false; double k; Ema(int n):k(2.0/(n+1)){} void push(double x){ if(!init){v=x;init=true;} else v+=k*(x-v);} };
struct Atr { std::deque<double> tr; double prev_c=0; bool has=false; int n;
    Atr(int n):n(n){}
    void push(double h,double l,double c){ double t=h-l; if(has){ t=std::max(t,std::max(std::fabs(h-prev_c),std::fabs(l-prev_c))); } tr.push_back(t); if((int)tr.size()>n) tr.pop_front(); prev_c=c; has=true; }
    bool ready() const { return (int)tr.size()==n; }
    double v() const { double s=0; for(double x:tr) s+=x; return s/tr.size(); } };
struct Bar { long ts=0; double o=0,h=0,l=0,c=0; bool live=false;
    void start(long t,double px){ ts=t;o=h=l=c=px;live=true; }
    void upd(double hh,double ll,double cc){ h=std::max(h,hh); l=std::min(l,ll); c=cc; } };

int main(int argc,char**argv){
    const char* path = argc>1?argv[1]:"XAUUSD_2022_2026.m1.csv";
    double BE_ARM   = atof(getenv("BE_ARM")?getenv("BE_ARM"):"0.8");
    int    NEWS     = atoi(getenv("NEWS")?getenv("NEWS"):"1");
    int    LBMA     = atoi(getenv("LBMA")?getenv("LBMA"):"1");
    double COSTM    = atof(getenv("COST_MULT")?getenv("COST_MULT"):"1.0");
    const char* SESS= getenv("SESS")?getenv("SESS"):"BOTH";
    int    USE_IMP  = atoi(getenv("IMPULSE")?getenv("IMPULSE"):"1");
    int    USE_PB   = atoi(getenv("PULLBACK")?getenv("PULLBACK"):"1");
    int    USE_REG  = atoi(getenv("REGIME")?getenv("REGIME"):"1");
    long   TS_FROM  = atol(getenv("TS_FROM")?getenv("TS_FROM"):"0");
    long   TS_TO    = atol(getenv("TS_TO")?getenv("TS_TO"):"9999999999");
    // COST_EXTRA: multiplies the cost debited in FILLS (accounting) without touching
    // the 2.5x-cost stop-floor SELECTION gate — honest 2x-cost stress (selection at 1x).
    double COSTX    = atof(getenv("COST_EXTRA")?getenv("COST_EXTRA"):"1.0");

    // load
    FILE* f=fopen(path,"r"); if(!f){ fprintf(stderr,"no file %s\n",path); return 1; }
    char line[256]; fgets(line,sizeof line,f);
    while(fgets(line,sizeof line,f)){ M1 b; if(sscanf(line,"%ld,%lf,%lf,%lf,%lf",&b.ts,&b.o,&b.h,&b.l,&b.c)==5) tape.push_back(b); }
    fclose(f);

    // state: bars
    Bar b5, b15, b60; Bar bday; // day = COMEX roll day
    Ema e15_20(20), e15_50(50), e60_20(20), e15_slope_ref(20);
    Atr atr15(14), atr5(14);
    double ema60_prev=0; bool ema60_has=false; double ema60_slope=0;
    double ema15_prev=0; bool ema15_has=false; double ema15_slope=0;
    std::deque<double> rng5; // last 100 5m ranges
    auto med5=[&](){ if(rng5.size()<30) return -1.0; std::vector<double> v(rng5.begin(),rng5.end()); size_t k=v.size()/2; std::nth_element(v.begin(),v.begin()+k,v.end()); return v[k]; };
    // daily
    long cur_day=-1; double day_hi=0, day_lo=0; double pd_hi=0, pd_lo=0, pd_mid=0; bool pd_has=false;
    double vwap_num=0; long vwap_n=0; double vwap=0;
    // 5m history for swing lows (trail): last 3 completed 5m lows
    std::deque<double> last5lo;
    std::vector<double> day5close; // not needed

    // session state machine
    enum Phase { NONE, ORB, HUNT, PULL, ARMED };
    struct Sess {
        bool active=false; Phase ph=NONE; int dir=0; // dir set at impulse
        double or_hi=0, or_lo=0; bool or_done=false;
        long or_end_ts=0, end_ts=0; int id=0; // 0=LDN 1=NY
        double imp_origin=0, imp_ext=0; long imp_ts=0;
        double pb_low=0, pb_high=0; double trigger=0; bool have_pb_extreme=false;
        bool traded_win=false; int entries=0;
    } S;
    // position
    struct Pos {
        bool open=false; int dir=0; double entry=0, stop=0, r=0; double size=1.0; // size in units of "risk fraction"
        bool part=false; bool be=false; double mfe=0; long ts0=0; int sess=0;
        double realized=0; // R banked from partial
        double true_be=0;
    } P;
    int day_losses=0, day_entries=0; bool day_dead=false;
    long day_of_limits=-1;

    struct Trade { long ts; int dir; double rmult; int sess; double px_in; long dur; const char* reason; };
    std::vector<Trade> trades;

    auto cost_rt=[&](double px){ return COSTM*(0.30 + 2*1.5e-4*px + 2*0.03); }; // spread + comm rt + slip rt, $/oz
    auto half_spread=[&](){ return COSTM*COSTX*0.15; };
    auto slip=[&](){ return COSTM*COSTX*0.03; };
    auto comm=[&](double px){ return COSTM*COSTX*1.5e-4*px; };

    auto close_pos=[&](double px_mid, long ts, const char* why){
        // exit remaining size at mid -> receive bid (long) etc.
        double px = P.dir>0 ? px_mid - half_spread() - slip() : px_mid + half_spread() + slip();
        double pnl = P.dir*(px - P.entry) - comm(P.entry) - comm(px); // $/oz for remaining fraction
        double rm = P.realized + (pnl / P.r) * (P.part ? 0.75 : 1.0);
        trades.push_back({ts,P.dir,rm,P.sess,P.entry,ts-P.ts0,why});
        if(rm < -0.05 && !P.part && !P.be) day_losses++;           // "initial loss"
        if(rm > 0.02) S.traded_win=true;
        P.open=false;
    };

    // main loop
    for(size_t i=0;i<tape.size();++i){
        const M1& m=tape[i];
        if(m.ts<TS_FROM||m.ts>TS_TO) continue;
        time_t tt=m.ts; struct tm g=*gmtime(&tt);
        int et_off, ldn_off; offsets(g,et_off,ldn_off);
        int y=g.tm_year+1900, mo=g.tm_mon+1, dd=g.tm_mday;
        // local minutes
        long utc_min = (m.ts/60)%1440;
        long et_min  = ((utc_min + et_off*60)%1440+1440)%1440;
        long ldn_min = ((utc_min + ldn_off*60)%1440+1440)%1440;
        // local date keys (approx: session hours never cross local midnight)
        // COMEX day roll: 17:00 ET
        long comex_day = (m.ts + et_off*3600 - 17*3600) / 86400;

        // ---- daily roll ----
        if(comex_day!=cur_day){
            if(cur_day>=0){ pd_hi=day_hi; pd_lo=day_lo; pd_mid=0.5*(pd_hi+pd_lo); pd_has=true; }
            cur_day=comex_day; day_hi=m.h; day_lo=m.l;
            vwap_num=0; vwap_n=0;
            // day limit counters key on ET calendar date of the SESSION, use comex day
        } else { day_hi=std::max(day_hi,m.h); day_lo=std::min(day_lo,m.l); }
        vwap_num += (m.h+m.l+m.c)/3.0; vwap_n++; vwap = vwap_num/vwap_n;

        // day-limits reset on ET calendar date
        long et_date = (m.ts + et_off*3600)/86400;
        if(et_date!=day_of_limits){ day_of_limits=et_date; day_losses=0; day_entries=0; day_dead=false; }
        if(day_losses>=2) day_dead=true;

        // ---- session windows (local, DST-correct) ----
        bool ldn_ok = strcmp(SESS,"NY")!=0, ny_ok = strcmp(SESS,"LDN")!=0;
        bool in_ldn = ldn_ok && g.tm_wday>=1 && g.tm_wday<=5 && ldn_min>=8*60 && ldn_min<11*60;
        bool in_ny  = ny_ok  && g.tm_wday>=1 && g.tm_wday<=5 && et_min>=8*60+20 && et_min<11*60+30;
        int  sess_id = in_ldn?0 : in_ny?1 : -1;

        // session open detection
        if(sess_id>=0 && !S.active){
            S=Sess{}; S.active=true; S.id=sess_id; S.ph=ORB;
            long start_min = sess_id==0 ? 8*60 : 8*60+20;
            long cur_local = sess_id==0 ? ldn_min : et_min;
            long into = cur_local-start_min;
            S.or_end_ts = m.ts + (30-into)*60;
            S.or_hi=m.h; S.or_lo=m.l;
            long end_local = sess_id==0 ? 11*60 : 11*60+30;
            S.end_ts = m.ts + (end_local-cur_local)*60;
        }
        if(S.active){
            if(sess_id<0){
                // session ended: flatten
                if(P.open) close_pos(m.o,m.ts,"SESSION_END");
                S.active=false;
            }
        }

        // ---- position management on M1 (fills) ----
        if(P.open){
            double lvl_part = P.entry + P.dir*1.25*P.r;
            if(!P.part && ((P.dir>0&&m.h>=lvl_part)||(P.dir<0&&m.l<=lvl_part))){
                double px = lvl_part - P.dir*(half_spread()+slip());
                double pnl = P.dir*(px-P.entry) - comm(P.entry) - comm(px);
                P.realized += 0.25*pnl/P.r; P.part=true;
            }
            P.mfe = std::max(P.mfe, P.dir>0 ? (m.h-P.entry) : (P.entry-m.l));
            if(!P.be && P.mfe >= BE_ARM*P.r){
                P.be=true;
                double c = half_spread()+slip()+comm(P.entry)+comm(P.entry)+0.05;
                P.true_be = P.entry + P.dir*c;
                if(P.dir>0) P.stop=std::max(P.stop,P.true_be); else P.stop=std::min(P.stop,P.true_be);
            }
            if((P.dir>0&&m.l<=P.stop)||(P.dir<0&&m.h>=P.stop)){
                double px = P.stop - P.dir*(half_spread()+slip());
                double pnl = P.dir*(px-P.entry) - comm(P.entry) - comm(px);
                double rm = P.realized + (pnl/P.r)*(P.part?0.75:1.0);
                trades.push_back({m.ts,P.dir,rm,P.sess,P.entry,m.ts-P.ts0,P.be?"TRAIL/BE":"STOP"});
                if(rm < -0.05 && !P.be) day_losses++;
                if(rm > 0.02) S.traded_win=true;
                P.open=false;
            }
        }
        // pending stop-entry fill on M1
        if(!P.open && S.active && S.ph==ARMED && m.ts<S.end_ts){
            bool hit = S.dir>0 ? m.h>=S.trigger : m.l<=S.trigger;
            if(hit && !day_dead && day_entries<3 && !S.traded_win){
                double entry_mid=S.trigger;
                double entry = entry_mid + S.dir*(half_spread()+slip());
                double stop  = S.dir>0 ? S.pb_low - 0.10*atr5.v() : S.pb_high + 0.10*atr5.v();
                double rdist = S.dir*(entry-stop);
                double crt=cost_rt(entry);
                bool ok = rdist>=2.5*crt && rdist<=0.80*atr15.v() && atr15.ready() && atr5.ready();
                // news blocks
                if(NEWS){
                    if(is_first_friday(y,mo,dd)){ long blk0=8*60+20, blk1=8*60+45; if(et_min>=blk0&&et_min<blk1) ok=false; }
                    if(is_fomc(dkey(y,mo,dd))){ long blk0=13*60+50, blk1=14*60+15; if(et_min>=blk0&&et_min<blk1) ok=false; }
                }
                if(LBMA){ if((ldn_min>=10*60+20&&ldn_min<10*60+45)||(ldn_min>=14*60+50&&ldn_min<15*60+15)) ok=false; }
                if(ok){
                    P=Pos{}; P.open=true; P.dir=S.dir; P.entry=entry; P.stop=stop; P.r=rdist; P.ts0=m.ts; P.sess=S.id;
                    day_entries++;
                }
                S.ph=HUNT; // either way, need fresh structure for another go
            }
        }

        // ---- 5m bar aggregation + decisions at 5m closes ----
        long t5 = m.ts - (m.ts%300);
        if(!b5.live) b5.start(t5,m.o);
        if(t5!=b5.ts){
            // 5m bar CLOSED: b5
            double r=b5.h-b5.l;
            // trail update on close (after partial or BE armed, per spec remaining 75%)
            if(P.open && (P.part||P.be)){
                if(atr5.ready()&&last5lo.size()>=3){
                    if(P.dir>0){
                        double sw=*std::min_element(last5lo.begin(),last5lo.end());
                        double cand=std::min(sw, b5.c-2.0*atr5.v());
                        P.stop=std::max(P.stop,cand);
                    } else {
                        // maintain 3-bar highs symmetric via last5lo storing -high for shorts: simpler recompute below
                    }
                }
            }
            // (shorts trail handled with lows deque of highs: keep both)
            static std::deque<double> last5hi;
            if(P.open && (P.part||P.be) && P.dir<0 && atr5.ready() && last5hi.size()>=3){
                double sw=*std::max_element(last5hi.begin(),last5hi.end());
                double cand=std::max(sw, b5.c+2.0*atr5.v());
                P.stop=std::min(P.stop,cand);
            }
            // management exits at 5m close
            if(P.open){
                bool exit=false; const char* why="";
                if(P.dir>0 && b5.c<vwap){ exit=true; why="VWAP_CROSS"; }
                if(P.dir<0 && b5.c>vwap){ exit=true; why="VWAP_CROSS"; }
                if(!exit && ema15_has && atr15.ready()){
                    if(P.dir>0 && ema15_slope < -0.02*atr15.v()) { exit=true; why="SLOPE_REV"; }
                    if(P.dir<0 && ema15_slope >  0.02*atr15.v()) { exit=true; why="SLOPE_REV"; }
                }
                double md=med5();
                if(!exit && md>0 && r>=2.0*md){
                    bool violent_dn = (b5.c-b5.l)/(r+1e-9)<0.25 && b5.c<b5.o;
                    bool violent_up = (b5.h-b5.c)/(r+1e-9)<0.25 && b5.c>b5.o;
                    if(P.dir>0&&violent_dn){exit=true;why="VIOLENT_OPP";}
                    if(P.dir<0&&violent_up){exit=true;why="VIOLENT_OPP";}
                }
                if(exit) close_pos(b5.c,m.ts,why);
            }
            // session logic at 5m close
            if(S.active && m.ts<S.end_ts){
                if(S.ph==ORB){
                    if(m.ts>=S.or_end_ts){ S.or_done=true; S.ph=HUNT; }
                    else { S.or_hi=std::max(S.or_hi,b5.h); S.or_lo=std::min(S.or_lo,b5.l); }
                }
                if(S.ph==HUNT && S.or_done && !P.open && !S.traded_win && !day_dead && day_entries<3 && atr15.ready() && atr5.ready() && pd_has){
                    // regime
                    int reg=0;
                    bool L = b5.c>vwap && e15_20.v>e15_50.v && ema60_slope>0 && b5.c>pd_mid;
                    bool Sh= b5.c<vwap && e15_20.v<e15_50.v && ema60_slope<0 && b5.c<pd_mid;
                    if(!USE_REG){ L = b5.c>vwap; Sh = b5.c<vwap; }
                    reg = L?1:(Sh?-1:0);
                    double md=med5();
                    if(reg!=0 && md>0){
                        bool brk = reg>0 ? b5.c>S.or_hi : b5.c<S.or_lo;
                        double dist = reg>0 ? b5.c-S.or_hi : S.or_lo-b5.c;
                        bool imp = !USE_IMP || (dist>=0.15*atr15.v() && r>=1.25*md);
                        if(brk&&imp){
                            S.dir=reg; S.imp_origin = reg>0? b5.o : b5.o; // impulse origin = breakout bar open
                            // extend origin down to bar low for longs (base of thrust)
                            S.imp_origin = reg>0 ? std::min(b5.o,S.or_hi) : std::max(b5.o,S.or_lo);
                            S.imp_ext = reg>0? b5.h : b5.l; S.imp_ts=m.ts;
                            if(!USE_PB){ S.trigger = reg>0? b5.c+0.02 : b5.c-0.02; S.pb_low=b5.l; S.pb_high=b5.h; S.ph=ARMED; }
                            else S.ph=PULL;
                        }
                    }
                }
                else if(S.ph==PULL && !P.open){
                    // track impulse extension & pullback
                    if(S.dir>0){
                        if(b5.h>S.imp_ext && !S.have_pb_extreme){ S.imp_ext=b5.h; }
                        // pullback: bar that fails to extend
                        double leg=S.imp_ext-S.imp_origin;
                        if(leg>0){
                            if(!S.have_pb_extreme){
                                if(b5.h<S.imp_ext){ S.have_pb_extreme=true; S.pb_low=b5.l; S.trigger=b5.h+0.02; }
                            } else {
                                if(b5.l<S.pb_low){ S.pb_low=b5.l; S.trigger=b5.h+0.02; }
                                double f=(S.imp_ext-S.pb_low)/leg;
                                bool valid = f>=0.25&&f<=0.55 && S.pb_low>S.or_hi-0.10*atr5.v() && b5.c>vwap && b5.c>S.imp_origin;
                                if(b5.c<S.imp_origin || f>0.80 || b5.c<vwap) { S.ph=HUNT; S.have_pb_extreme=false; }
                                else if(valid && b5.c>b5.o){ S.ph=ARMED; }
                            }
                        }
                    } else {
                        if(b5.l<S.imp_ext && !S.have_pb_extreme){ S.imp_ext=b5.l; }
                        double leg=S.imp_origin-S.imp_ext;
                        if(leg>0){
                            if(!S.have_pb_extreme){
                                if(b5.l>S.imp_ext){ S.have_pb_extreme=true; S.pb_high=b5.h; S.trigger=b5.l-0.02; }
                            } else {
                                if(b5.h>S.pb_high){ S.pb_high=b5.h; S.trigger=b5.l-0.02; }
                                double f=(S.pb_high-S.imp_ext)/leg;
                                bool valid = f>=0.25&&f<=0.55 && S.pb_high<S.or_lo+0.10*atr5.v() && b5.c<vwap && b5.c<S.imp_origin;
                                if(b5.c>S.imp_origin || f>0.80 || b5.c>vwap) { S.ph=HUNT; S.have_pb_extreme=false; }
                                else if(valid && b5.c<b5.o){ S.ph=ARMED; }
                            }
                        }
                    }
                }
            }
            // indicator updates on closed 5m
            atr5.push(b5.h,b5.l,b5.c);
            rng5.push_back(r); if(rng5.size()>100) rng5.pop_front();
            last5lo.push_back(b5.l); if(last5lo.size()>3) last5lo.pop_front();
            last5hi.push_back(b5.h); if(last5hi.size()>3) last5hi.pop_front();
            b5.start(t5,m.o);
        }
        b5.upd(m.h,m.l,m.c);

        long t15 = m.ts-(m.ts%900);
        if(!b15.live) b15.start(t15,m.o);
        if(t15!=b15.ts){
            atr15.push(b15.h,b15.l,b15.c);
            double prev=e15_20.v; bool had=e15_20.init;
            e15_20.push(b15.c); e15_50.push(b15.c);
            if(had){ ema15_slope=e15_20.v-prev; ema15_has=true; }
            b15.start(t15,m.o);
        }
        b15.upd(m.h,m.l,m.c);

        long t60=m.ts-(m.ts%3600);
        if(!b60.live) b60.start(t60,m.o);
        if(t60!=b60.ts){
            double prev=e60_20.v; bool had=e60_20.init;
            e60_20.push(b60.c);
            if(had){ ema60_slope=e60_20.v-prev; ema60_has=true; }
            b60.start(t60,m.o);
        }
        b60.upd(m.h,m.l,m.c);
    }
    if(P.open){ const M1&m=tape.back(); close_pos(m.c,m.ts,"EOD_TAPE"); }

    // ---- report ----
    auto stats=[&](auto pred, const char* name){
        double net=0,gp=0,gl=0,peak=0,dd=0,eq=0; int n=0,w=0;
        for(auto&t:trades){ if(!pred(t)) continue; n++; net+=t.rmult; if(t.rmult>0){w++;gp+=t.rmult;} else gl-=t.rmult; eq+=t.rmult; peak=std::max(peak,eq); dd=std::min(dd,eq-peak); }
        printf("%-28s n=%4d  netR=%8.2f  avgR=%6.3f  PF=%5.2f  WR=%4.1f%%  maxDD_R=%6.2f\n",
               name,n,net,n?net/n:0.0,gl>0?gp/gl:(gp>0?99.0:0.0),n?100.0*w/n:0.0,dd);
    };
    printf("== gold_session_tp_bt  BE_ARM=%.2f NEWS=%d LBMA=%d COST_MULT=%.1f SESS=%s IMP=%d PB=%d REG=%d ==\n",
           BE_ARM,NEWS,LBMA,COSTM,SESS,USE_IMP,USE_PB,USE_REG);
    stats([](const Trade&){return true;},"ALL");
    stats([](const Trade&t){return t.dir>0;},"LONG");
    stats([](const Trade&t){return t.dir<0;},"SHORT");
    stats([](const Trade&t){return t.sess==0;},"LDN");
    stats([](const Trade&t){return t.sess==1;},"NY");
    long mid=1734000000; // ~2024-12-12 tape midpoint by time
    stats([&](const Trade&t){return t.ts<mid;},"WF-H1(22.01-24.12)");
    stats([&](const Trade&t){return t.ts>=mid;},"WF-H2(24.12-26.06)");
    stats([](const Trade&t){return t.ts<1672531200;},"REGIME-2022bear");
    stats([](const Trade&t){return t.ts>=1704067200;},"REGIME-24-26bull");
    // exit reason breakdown
    printf("-- exits: ");
    const char* reasons[]={"STOP","TRAIL/BE","VWAP_CROSS","SLOPE_REV","VIOLENT_OPP","SESSION_END","EOD_TAPE"};
    for(auto rz:reasons){ int c=0; double s=0; for(auto&t:trades){ if(!strcmp(t.reason,rz)){c++;s+=t.rmult;} } if(c) printf("%s n=%d net=%.1f  ",rz,c,s); }
    printf("\n");
    return 0;
}
