// =============================================================================
// gold_cost_unlock_sweep.cpp -- quantify which long-only gold trend/breakout
// cells the IBKR cost cut (0.60 -> 0.37 RT pts) unlocks.
//
// Reads an M5 OHLC CSV (ts_sec,o,h,l,c), aggregates to a target timeframe by
// timestamp bucket (handles weekend gaps), runs ONE long-only cell, applies a
// per-RT-trade cost of COST * |units|, and prints one result row.
//
// Strategy kinds (NO mean reversion; gold shorts dead -> long-only):
//   don  : Donchian-N breakout long; exit close < N-low; ATRxSTOP stop
//   ema  : EMA(fast,slow) cross long + slow-rising; exit fast<slow; ATRxSTOP
//   kelt : Keltner breakout long (close > EMA_n + k*ATR); exit close < EMA_n
//   tsmom: time-series momentum (close > close[N] & slow-rising); exit flip
//
// Sizing: vol-target 10% ann (same as GoldTrendEnsembleBacktest), 1% risk x
// scalar, capped 50% notional. Cost model identical to the ensemble harness so
// numbers are comparable.
//
//   g++ -std=c++17 -O2 -o backtest/gold_cost_unlock_sweep \
//       backtest/gold_cost_unlock_sweep.cpp
//   ./backtest/gold_cost_unlock_sweep <m5.csv> <kind> <tf_min> <p1> <p2> \
//       <atr_stop> <kelt_k> <cost> [oos_frac]
//     p1/p2 = (fast,slow) for ema; (N,0) for don/tsmom; (ema_n,0) for kelt
//     oos_frac: if >0, only evaluate last oos_frac of the series (e.g. 0.30)
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

static std::vector<Bar> load_and_aggregate(const char* path, int tf_min) {
    std::ifstream f(path);
    std::vector<Bar> out;
    if (!f) { std::fprintf(stderr,"cannot open %s\n",path); return out; }
    std::string line; std::getline(f,line); // header
    int64_t period = (int64_t)tf_min * 60;
    Bar cur; int64_t cur_bucket = -1; bool active=false;
    while (std::getline(f,line)) {
        if (line.empty()) continue;
        const char* s=line.c_str(); char* e=nullptr;
        int64_t ts=strtoll(s,&e,10); if(*e!=',')continue;
        double o=strtod(e+1,&e); if(*e!=',')continue;
        double h=strtod(e+1,&e); if(*e!=',')continue;
        double l=strtod(e+1,&e); if(*e!=',')continue;
        double c=strtod(e+1,&e);
        if (o<=0||h<=0||l<=0||c<=0) continue;
        int64_t bucket = (ts/period)*period;
        if (!active) { cur={bucket,o,h,l,c}; cur_bucket=bucket; active=true; continue; }
        if (bucket != cur_bucket) {
            out.push_back(cur);
            cur={bucket,o,h,l,c}; cur_bucket=bucket;
        } else {
            if (h>cur.h) cur.h=h;
            if (l<cur.l) cur.l=l;
            cur.c=c;
        }
    }
    if (active) out.push_back(cur);
    return out;
}

int main(int argc, char** argv){
    if (argc < 9){ std::fprintf(stderr,"usage: %s <m5.csv> <kind> <tf_min> <p1> <p2> <atr_stop> <kelt_k> <cost> [oos_frac]\n",argv[0]); return 1; }
    const char* path=argv[1];
    std::string kind=argv[2];
    int tf_min=atoi(argv[3]);
    int p1=atoi(argv[4]), p2=atoi(argv[5]);
    double atr_stop=atof(argv[6]);
    double kelt_k=atof(argv[7]);
    double COST=atof(argv[8]);
    double oos_frac = (argc>9)?atof(argv[9]):0.0;
    const char* eq_path = (argc>10)?argv[10]:nullptr;  // daily MTM equity out
    std::vector<std::pair<int64_t,double>> daily; int64_t cur_day=-1; double day_mtm=0;

    std::vector<Bar> bars = load_and_aggregate(path, tf_min);
    if ((int)bars.size() < 200){ std::fprintf(stderr,"too few bars (%zu)\n",bars.size()); return 1; }

    const double START=100000.0, VOL_TARGET=0.10, ATR_MIN=0.5, MAXFRAC=0.50;
    const int ATR_P=14, VOL_LB=60, SLOW_RISE=3;
    // bars per year for this TF
    double bpy = 252.0 * (24.0*60.0/ tf_min);

    int eval_start = 0;
    if (oos_frac>0.0 && oos_frac<1.0) eval_start = (int)(bars.size()*(1.0-oos_frac));

    // --- regime gate (env-driven): suppress entries in chop ---
    const char* g = getenv("GATE"); std::string gate = g?g:"none"; // none/adx/er/both
    double ADX_THR = getenv("ADX_THR")?atof(getenv("ADX_THR")):20.0;
    double ER_THR  = getenv("ER_THR") ?atof(getenv("ER_THR")) :0.30;
    int    ER_N    = getenv("ER_N")   ?atoi(getenv("ER_N"))   :14;
    int    ADX_P   = 14;
    int    COOLDOWN = getenv("COOLDOWN")?atoi(getenv("COOLDOWN")):0;   // bars to block after a stop-loss
    double BREAK_BUF= getenv("BREAK_BUF")?atof(getenv("BREAK_BUF")):0.0; // require close > chan + buf*ATR
    int    cooldown_until = -1;
    // ADX(Wilder) state
    double sm_pdm=0, sm_mdm=0, sm_tr=0, adx=0; bool adx_ready=false; int adx_cnt=0;
    std::deque<double> close_hist; // for ER

    // indicator state
    double atr=5.0; std::deque<double> tr;
    double ema_f=0, ema_s=0; bool ema_init=false;
    std::deque<double> ema_s_hist;        // slow-rising
    std::deque<double> logret;            // vol sizing
    double ema_n=0; bool emn_init=false;  // keltner EMA
    double kf = 2.0/(p1+1), ks = 2.0/(p2+1);
    double kn = 2.0/(p1+1);               // keltner ema period = p1

    bool pos=false; double entry=0, stop=0, units=0; int entry_i=0;
    double cum=0, peak=0, mdd=0;
    int nw=0,nl=0; double gw=0,gl=0;
    int ntr=0; std::vector<double> trade_pnl;

    auto close_pos=[&](double px,int i){
        double pnl=(px-entry)*units - COST*std::fabs(units);
        cum+=pnl; if(cum>peak)peak=cum; double dd=peak-cum; if(dd>mdd)mdd=dd;
        if(pnl>0){nw++;gw+=pnl;} else if(pnl<0){nl++;gl+=-pnl;}
        trade_pnl.push_back(pnl); ntr++;
        pos=false;
    };

    for (int i=1;i<(int)bars.size();++i){
        const Bar& b=bars[i]; const Bar& pv=bars[i-1];
        // ATR
        double t=std::max({b.h-b.l,std::fabs(b.h-pv.c),std::fabs(b.l-pv.c)});
        tr.push_back(t); if((int)tr.size()>ATR_P)tr.pop_front();
        if((int)tr.size()>=ATR_P) atr=atr*13.0/14.0+t/14.0;
        else { double sm=0; for(double v:tr)sm+=v; atr=sm/tr.size(); }
        atr=std::max(ATR_MIN,atr);
        // EMAs
        if(!ema_init){ema_f=ema_s=b.c;ema_init=true;}
        else{ema_f=b.c*kf+ema_f*(1-kf);ema_s=b.c*ks+ema_s*(1-ks);}
        ema_s_hist.push_back(ema_s); while((int)ema_s_hist.size()>16)ema_s_hist.pop_front();
        if(!emn_init){ema_n=b.c;emn_init=true;} else ema_n=b.c*kn+ema_n*(1-kn);
        // log ret
        if(pv.c>0&&b.c>0){ logret.push_back(std::log(b.c/pv.c)); while((int)logret.size()>VOL_LB)logret.pop_front(); }

        // --- ADX(14) Wilder ---
        double up_move=b.h-pv.h, dn_move=pv.l-b.l;
        double pdm=(up_move>dn_move && up_move>0)?up_move:0.0;
        double mdm=(dn_move>up_move && dn_move>0)?dn_move:0.0;
        if(adx_cnt<ADX_P){ sm_pdm+=pdm; sm_mdm+=mdm; sm_tr+=t; adx_cnt++; }
        else { sm_pdm=sm_pdm-(sm_pdm/ADX_P)+pdm; sm_mdm=sm_mdm-(sm_mdm/ADX_P)+mdm; sm_tr=sm_tr-(sm_tr/ADX_P)+t; }
        double pdi=(sm_tr>0)?100.0*sm_pdm/sm_tr:0, mdi=(sm_tr>0)?100.0*sm_mdm/sm_tr:0;
        double dx=(pdi+mdi>0)?100.0*std::fabs(pdi-mdi)/(pdi+mdi):0;
        if(adx_cnt>=ADX_P){ if(!adx_ready){adx=dx;adx_ready=true;} else adx=(adx*(ADX_P-1)+dx)/ADX_P; }
        // --- Kaufman Efficiency Ratio ---
        close_hist.push_back(b.c); while((int)close_hist.size()>ER_N+1) close_hist.pop_front();
        double er=0;
        if((int)close_hist.size()==ER_N+1){
            double net=std::fabs(close_hist.back()-close_hist.front());
            double sum=0; for(int k=1;k<(int)close_hist.size();++k) sum+=std::fabs(close_hist[k]-close_hist[k-1]);
            er=(sum>0)?net/sum:0;
        }

        // intra-bar stop (long) -> arm cooldown (whipsaw guard)
        if(pos && b.l<=stop){ close_pos(stop,i); if(COOLDOWN>0) cooldown_until=i+COOLDOWN; }

        int warm = std::max({p1,p2,VOL_LB}) + 5;
        if(i<warm) continue;
        if(i<eval_start){ // still maintain state but no trading in IS portion
            // allow positions opened pre-eval to be ignored: skip signals
            continue;
        }

        bool slow_rising = ((int)ema_s_hist.size()>SLOW_RISE) &&
                           (ema_s_hist.back()>ema_s_hist[ema_s_hist.size()-1-SLOW_RISE]);

        // Donchian channel over prior N bars (exclude current)
        double dh=0, dl=1e18;
        if(kind=="don"||kind=="tsmom"){
            int N=p1; int a=i-N, z=i;
            if(a>=0){ for(int k=a;k<z;++k){ if(bars[k].h>dh)dh=bars[k].h; if(bars[k].l<dl)dl=bars[k].l; } }
        }

        bool sig_long=false, sig_exit=false;
        if(kind=="don"){
            if(!pos && dh>0 && b.c>dh+BREAK_BUF*atr) sig_long=true;
            if(pos && b.c<dl) sig_exit=true;
        } else if(kind=="ema"){
            bool fa = ema_f>ema_s;
            sig_long = fa && slow_rising;
            sig_exit = !fa;
        } else if(kind=="kelt"){
            double up = ema_n + kelt_k*atr;
            if(!pos && b.c>up) sig_long=true;
            if(pos && b.c<ema_n) sig_exit=true;
        } else if(kind=="tsmom"){
            int N=p1;
            double past = bars[i-N].c;
            bool mom = b.c>past;
            sig_long = mom && slow_rising;
            sig_exit = !mom;
        }

        // regime gate: block NEW entries in chop (never forces exits)
        bool trend_ok=true;
        if(gate=="adx")  trend_ok = adx_ready && adx>=ADX_THR;
        else if(gate=="er")   trend_ok = er>=ER_THR;
        else if(gate=="both") trend_ok = (adx_ready && adx>=ADX_THR) && (er>=ER_THR);
        if(!trend_ok) sig_long=false;
        if(COOLDOWN>0 && i<cooldown_until) sig_long=false;  // whipsaw cooldown

        if(pos && sig_exit){ close_pos(b.c,i); }
        if(!pos && sig_long){
            double sd=0; { int n=(int)logret.size(); if(n>=10){ double m=0;for(double v:logret)m+=v;m/=n; double s=0;for(double v:logret)s+=(v-m)*(v-m); sd=std::sqrt(s/(n-1)); } }
            if(sd<=0) continue;
            double ann=sd*std::sqrt(bpy); if(ann<=0) continue;
            double scalar=std::min(4.0, VOL_TARGET/ann);
            double stop_dist=atr_stop*atr; if(stop_dist<=0) continue;
            double eq=START+cum; double risk=eq*0.01*scalar;
            double u=risk/stop_dist; double cap=(eq*MAXFRAC)/b.c; u=std::min(u,cap);
            if(u<=0) continue;
            pos=true; entry=b.c; stop=b.c-stop_dist; units=u; entry_i=i;
        }

        // daily mark-to-market (realized cum + unrealized), last value per day
        if(eq_path){
            double mtm = cum + (pos ? (b.c-entry)*units : 0.0);
            int64_t day = b.ts/86400;
            if(day!=cur_day){ if(cur_day>=0) daily.push_back({cur_day,day_mtm}); cur_day=day; }
            day_mtm=mtm;
        }
    }
    if(pos) close_pos(bars.back().c,(int)bars.size()-1);
    if(eq_path){
        if(cur_day>=0) daily.push_back({cur_day,day_mtm});
        std::ofstream ef(eq_path);
        ef<<"day,mtm\n";
        for(auto&p:daily) ef<<p.first<<','<<p.second<<'\n';
    }

    double pf = (gl>0)?gw/gl:(gw>0?999.0:0.0);
    double hit = (nw+nl>0)?100.0*nw/(nw+nl):0.0;
    // trade-based annualized Sharpe
    double years_eval; { int n=(int)bars.size(); int s=std::max(eval_start,1);
        years_eval=(bars[n-1].ts-bars[s].ts)/86400.0/365.25; }
    double sharpe=0;
    if(ntr>=2 && years_eval>0){ double m=0;for(double v:trade_pnl)m+=v;m/=ntr; double s=0;for(double v:trade_pnl)s+=(v-m)*(v-m); double sd=std::sqrt(s/(ntr-1)); if(sd>0) sharpe=(m/sd)*std::sqrt((double)ntr/years_eval); }

    // CSV row: kind,tf,p1,p2,atrstop,keltk,cost,trades,pnl,pf,sharpe,win,mdd
    std::printf("%s,%d,%d,%d,%.1f,%.1f,%.2f,%d,%.0f,%.2f,%.2f,%.1f,%.0f\n",
        kind.c_str(),tf_min,p1,p2,atr_stop,kelt_k,COST,ntr,cum,pf,sharpe,hit,mdd);
    return 0;
}
