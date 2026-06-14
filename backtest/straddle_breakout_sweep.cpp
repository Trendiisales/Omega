// =============================================================================
// straddle_breakout_sweep.cpp -- single-shot OCO straddle breakout (the Quantum
// Dark Gold entry, minus the grid). At bar granularity an OCO Buy-Stop/Sell-Stop
// straddle == a SYMMETRIC breakout: break above box high+buf -> long, below box
// low-buf -> short, one position only, ATR stop, optional RR TP. NO grid.
//
// Studies "death by a thousand cuts" = false breakouts + counter-trend leg.
// Each anti-cut filter is a toggle (env), so we can see what converts bleed->edge:
//   BIAS    long|short|both   only arm the leg aligned with EMA(fast)>EMA(slow)
//                             (both = symmetric; long = with-trend on a bull)
//   COMPRESS k  arm only when box range < k*ATR (real breakouts come from coils)
//   SESS h0 h1  arm only when UTC hour in [h0,h1) (session breakouts)
//   COOLDOWN n  block re-arm for n bars after a stop-out (kills whipsaw repeats)
//   TP r        fixed TP = r*stop (0 = no-TP runner, exit on box-opposite or stop)
//
//   g++ -std=c++17 -O2 -o backtest/straddle_breakout_sweep backtest/straddle_breakout_sweep.cpp
//   ./backtest/straddle_breakout_sweep <m5.csv> <tf_min> <boxN> <buf_atr> <stop_atr> <cost> [oos_frac]
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <algorithm>

struct Bar { int64_t ts=0; double o=0,h=0,l=0,c=0; };

static std::vector<Bar> load_agg(const char* path, int tf_min){
    std::ifstream f(path); std::vector<Bar> out;
    if(!f){ std::fprintf(stderr,"open fail %s\n",path); return out; }
    std::string line; std::getline(f,line);
    int64_t per=(int64_t)tf_min*60; Bar cur; int64_t cb=-1; bool act=false;
    while(std::getline(f,line)){
        if(line.empty())continue; const char* s=line.c_str(); char* e=nullptr;
        int64_t ts=strtoll(s,&e,10); if(*e!=',')continue;
        double o=strtod(e+1,&e); if(*e!=',')continue;
        double h=strtod(e+1,&e); if(*e!=',')continue;
        double l=strtod(e+1,&e); if(*e!=',')continue;
        double c=strtod(e+1,&e);
        if(o<=0||h<=0||l<=0||c<=0)continue;
        int64_t b=(ts/per)*per;
        if(!act){cur={b,o,h,l,c};cb=b;act=true;continue;}
        if(b!=cb){out.push_back(cur);cur={b,o,h,l,c};cb=b;}
        else{if(h>cur.h)cur.h=h;if(l<cur.l)cur.l=l;cur.c=c;}
    }
    if(act)out.push_back(cur);
    return out;
}

int main(int argc,char**argv){
    if(argc<7){std::fprintf(stderr,"usage: %s <m5.csv> <tf_min> <boxN> <buf_atr> <stop_atr> <cost> [oos]\n",argv[0]);return 1;}
    const char* path=argv[1]; int tf=atoi(argv[2]); int boxN=atoi(argv[3]);
    double buf=atof(argv[4]), stopm=atof(argv[5]), COST=atof(argv[6]);
    double oos=(argc>7)?atof(argv[7]):0.0;

    std::string BIAS = getenv("BIAS")?getenv("BIAS"):"both";
    double COMPRESS  = getenv("COMPRESS")?atof(getenv("COMPRESS")):0.0;   // 0=off
    int    SESS0     = getenv("SESS0")?atoi(getenv("SESS0")):-1;
    int    SESS1     = getenv("SESS1")?atoi(getenv("SESS1")):-1;
    int    COOLDOWN  = getenv("COOLDOWN")?atoi(getenv("COOLDOWN")):0;
    double TPr       = getenv("TP")?atof(getenv("TP")):0.0;               // 0=runner
    int    biasFast  = getenv("BFAST")?atoi(getenv("BFAST")):20;
    int    biasSlow  = getenv("BSLOW")?atoi(getenv("BSLOW")):80;
    // profit-preservation knobs (2026-06-03): BE = move stop to entry+buffer once
    // favorable excursion >= BE_ARM (in R); TRAIL = trail stop TRAIL*ATR behind.
    double BE_ARM    = getenv("BE_ARM")?atof(getenv("BE_ARM")):0.0;       // R to arm breakeven (0=off)
    double BE_BUF    = getenv("BE_BUF")?atof(getenv("BE_BUF")):0.0;       // lock buffer in R above entry
    double TRAIL     = getenv("TRAIL")?atof(getenv("TRAIL")):0.0;         // ATR mult trail (0=off)
    int    HOLD      = getenv("HOLD")?atoi(getenv("HOLD")):0;             // max bars in trade (0=off) -- time-stop test
    // weekend-flat (2026-06-13): force-close any open position at/after Friday
    // FRIDAY_FLAT minutes-of-day UTC (e.g. 1245 = 20:45), and block new entries
    // from then until the weekend gap. 0 = off (hold across weekend, baseline).
    int    FRIDAY_FLAT = getenv("FRIDAY_FLAT")?atoi(getenv("FRIDAY_FLAT")):0;

    std::vector<Bar> b=load_agg(path,tf);
    if((int)b.size()<200){std::fprintf(stderr,"few bars\n");return 1;}
    int evalStart = (oos>0&&oos<1)?(int)(b.size()*(1.0-oos)):0;

    const int ATR_P=14; double atr=5.0; std::deque<double> tr;
    double ef=0,es=0; bool einit=false; double kf=2.0/(biasFast+1),ks=2.0/(biasSlow+1);

    // pyramid (env): add units on favorable advance, trail stop. PYMAX>0 forces
    // runner mode (no fixed TP -- pyramid needs room to run).
    int    PYMAX = getenv("PYMAX")?atoi(getenv("PYMAX")):0;
    double PYSTEP= getenv("PYSTEP")?atof(getenv("PYSTEP")):1.0;   // atr step between adds
    double PYSL  = getenv("PYSL")?atof(getenv("PYSL")):3.0;       // trail stop atr below last add

    // partial scale-out (env): bank PARTIAL fraction at +PARTIAL_R, run the rest
    // to TP/SL. One combined trade for stats. PARTIAL=0 -> off (full size to TP/SL).
    double PARTIAL   = getenv("PARTIAL")?atof(getenv("PARTIAL")):0.0;   // fraction 0..1
    double PARTIAL_R = getenv("PARTIAL_R")?atof(getenv("PARTIAL_R")):0.5;

    bool pos=false; int dir=0; double entry=0,stop=0,tp=0; int cooldown_until=-1; int entry_bar=-1;
    long long holdsum=0;
    int units=0; double entry_sum=0, last_add=0;
    double pend_bank=0.0, pos_frac=1.0; bool part_taken=false;   // partial state
    double cum=0,peak=0,mdd=0; int nw=0,nl=0; double gw=0,gl=0; int ntr=0;
    int totAdds=0; int nFF=0;
    // leg attribution
    int longN=0,shortN=0; double longNet=0,shortNet=0;
    std::vector<double> tpnl;
    std::vector<int64_t> tts;   // PORT_DUMP: exit-bar ts per trade

    auto close=[&](double px,int i){
        double pnl=pend_bank + (dir*(units*px - entry_sum) - COST*units)*pos_frac;
        pend_bank=0.0; pos_frac=1.0; part_taken=false;
        cum+=pnl; if(cum>peak)peak=cum; double dd=peak-cum; if(dd>mdd)mdd=dd;
        if(pnl>0){nw++;gw+=pnl;}else if(pnl<0){nl++;gl+=-pnl;}
        if(dir>0){longN++;longNet+=pnl;}else{shortN++;shortNet+=pnl;}
        tpnl.push_back(pnl); tts.push_back(b[i].ts); ntr++; pos=false;
    };

    for(int i=1;i<(int)b.size();++i){
        const Bar& bar=b[i]; const Bar& pv=b[i-1];
        double t=std::max({bar.h-bar.l,std::fabs(bar.h-pv.c),std::fabs(bar.l-pv.c)});
        tr.push_back(t); if((int)tr.size()>ATR_P)tr.pop_front();
        if((int)tr.size()>=ATR_P) atr=atr*13.0/14.0+t/14.0; else {double sm=0;for(double v:tr)sm+=v;atr=sm/tr.size();}
        atr=std::max(0.5,atr);
        if(!einit){ef=es=bar.c;einit=true;} else {ef=bar.c*kf+ef*(1-kf);es=bar.c*ks+es*(1-ks);}

        // pyramid: add units on favorable advance + trail stop (before exit check)
        if(pos && PYMAX>0 && units < 1+PYMAX){
            if(dir>0 && bar.h >= last_add + PYSTEP*atr){
                double add=last_add+PYSTEP*atr; entry_sum+=add; units++; last_add=add; totAdds++;
                double ns=last_add - PYSL*atr; if(ns>stop) stop=ns;
            } else if(dir<0 && bar.l <= last_add - PYSTEP*atr){
                double add=last_add-PYSTEP*atr; entry_sum+=add; units++; last_add=add; totAdds++;
                double ns=last_add + PYSL*atr; if(ns<stop) stop=ns;
            }
        }
        // profit preservation: BE ratchet + ATR trail (before stop/tp check)
        if(pos && (BE_ARM>0 || TRAIL>0)){
            const double sd = stopm*atr;               // 1R distance
            const double fav = (dir>0)? (bar.h-entry) : (entry-bar.l);
            const double favR = sd>0 ? fav/sd : 0.0;
            if(BE_ARM>0 && favR>=BE_ARM){
                double be = entry + dir*BE_BUF*sd;
                if(dir>0){ if(be>stop) stop=be; } else { if(be<stop) stop=be; }
            }
            if(TRAIL>0 && favR>0){
                double ts = (dir>0)? bar.h-TRAIL*atr : bar.l+TRAIL*atr;
                if(dir>0){ if(ts>stop) stop=ts; } else { if(ts<stop) stop=ts; }
            }
        }
        // partial scale-out: bank PARTIAL fraction at +PARTIAL_R, run the rest
        if(pos && PARTIAL>0 && !part_taken){
            const double sd=stopm*atr;
            const double favR=sd>0?((dir>0)?(bar.h-entry):(entry-bar.l))/sd:0.0;
            if(favR>=PARTIAL_R){
                double ppx=entry+dir*PARTIAL_R*sd;
                pend_bank += (dir*(ppx-entry)-COST)*PARTIAL;   // banked fraction (its cost share)
                pos_frac = 1.0-PARTIAL; part_taken=true;
            }
        }
        // manage open pos intrabar (stop priority, then tp)
        if(pos){
            if(dir>0){ if(bar.l<=stop) close(stop,i); else if(tp>0&&bar.h>=tp) close(tp,i); }
            else     { if(bar.h>=stop) close(stop,i); else if(tp>0&&bar.l<=tp) close(tp,i); }
        }
        if(pos && HOLD>0 && entry_bar>=0 && (i-entry_bar)>=HOLD){ holdsum+=(i-entry_bar); close(bar.c,i); }
        // weekend-flat: dow via (days+4)%7 -> Sun=0..Fri=5 (epoch 1970-01-01 = Thursday=4)
        bool ffWindow=false;
        if(FRIDAY_FLAT>0){
            int dow=(int)((bar.ts/86400+4)%7), mod=(int)((bar.ts%86400)/60);
            ffWindow=(dow==5 && mod>=FRIDAY_FLAT);
            if(pos && ffWindow){ nFF++; close(bar.c,i); }
        }
        if(pos && dir>0 && tp<=0){ /* runner: exit on close below box low (computed below) */ }

        int warm=std::max({boxN,biasSlow,ATR_P})+3;
        if(i<warm) continue;

        // box over prior boxN bars (exclude current)
        double bh=0,bl=1e18; for(int k=i-boxN;k<i;++k){if(b[k].h>bh)bh=b[k].h;if(b[k].l<bl)bl=b[k].l;}
        double boxrange=bh-bl;

        // runner exit: opposite box edge on close
        if(pos && tp<=0){
            if(dir>0 && bar.c<bl) close(bar.c,i);
            else if(dir<0 && bar.c>bh) close(bar.c,i);
        }

        if(i<evalStart) continue;
        if(pos) continue;
        if(ffWindow) continue;   // no fresh entries into the weekend-flat window
        if(COOLDOWN>0 && i<cooldown_until) continue;

        // filters
        bool armL=true, armS=true;
        if(BIAS=="long"){armS=false;} else if(BIAS=="short"){armL=false;}
        else { // both, but optionally bias-gate each leg by EMA trend
            // pure symmetric = both true; trend-gate handled via BIAS=trend
        }
        if(BIAS=="trend"){ bool up=ef>es; armL=up; armS=!up; }
        if(COMPRESS>0.0 && boxrange > COMPRESS*atr){ armL=armS=false; }
        if(SESS0>=0){ int hr=(int)((bar.ts%86400)/3600); bool in=(SESS0<=SESS1)?(hr>=SESS0&&hr<SESS1):(hr>=SESS0||hr<SESS1); if(!in){armL=armS=false;} }

        double buyStop=bh+buf*atr, sellStop=bl-buf*atr;
        // intrabar OCO: which triggers this bar? prefer the one nearer the open
        bool hitL = armL && bar.h>=buyStop;
        bool hitS = armS && bar.l<=sellStop;
        if(hitL && hitS){ // both: pick by open proximity (whichever the bar likely hit first)
            if(std::fabs(bar.o-buyStop) <= std::fabs(bar.o-sellStop)){ hitS=false; } else { hitL=false; }
        }
        double effTP = (PYMAX>0)?0.0:TPr;   // pyramid -> runner (no fixed TP)
        if(hitL){ pos=true;dir=1;entry=buyStop;stop=buyStop-stopm*atr; tp=effTP>0?buyStop+effTP*stopm*atr:0; units=1; entry_sum=entry; last_add=entry; pend_bank=0;pos_frac=1.0;part_taken=false; entry_bar=i; }
        else if(hitS){ pos=true;dir=-1;entry=sellStop;stop=sellStop+stopm*atr; tp=effTP>0?sellStop-effTP*stopm*atr:0; units=1; entry_sum=entry; last_add=entry; pend_bank=0;pos_frac=1.0;part_taken=false; entry_bar=i; }
    }
    if(pos) close(b.back().c,(int)b.size()-1);

    if(getenv("PORT_DUMP")){FILE*pd=fopen(getenv("PORT_DUMP"),"w");for(size_t k=0;k<tpnl.size();++k)fprintf(pd,"%lld,%.4f\n",(long long)tts[k],tpnl[k]);fclose(pd);}

    double pf=(gl>0)?gw/gl:(gw>0?999:0); double hit=(nw+nl>0)?100.0*nw/(nw+nl):0;
    double years; {int n=(int)b.size();int s=std::max(evalStart,1);years=(b[n-1].ts-b[s].ts)/86400.0/365.25;}
    double sh=0; if(ntr>=2&&years>0){double m=0;for(double v:tpnl)m+=v;m/=ntr;double s=0;for(double v:tpnl)s+=(v-m)*(v-m);double sd=std::sqrt(s/(ntr-1));if(sd>0)sh=(m/sd)*std::sqrt((double)ntr/years);}
    std::printf("TP%.1f PARTIAL%.2f@%.2fR BE_arm%.2f TRAIL%.1f FF%d | tr=%-4d net=%-8.0f PF=%.2f Sh=%+.2f win=%.0f%% mdd=%.0f ffClosed=%d\n",
        TPr,PARTIAL,PARTIAL_R,BE_ARM,TRAIL,FRIDAY_FLAT,ntr,cum,pf,sh,hit,mdd,nFF); (void)totAdds;(void)biasFast;(void)longN;(void)shortN;(void)longNet;(void)shortNet;
    return 0;
}
