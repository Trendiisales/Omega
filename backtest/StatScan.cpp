// StatScan.cpp -- Statistical character of gold tick data
// Measures the ACTUAL properties before building any signal.
// Usage: StatScan ticks.csv

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <numeric>
#include <cstdio>
#include <cstring>

struct Tick { long long ts; double mid; };

static bool parse(const std::string& ln, Tick& t) {
    const char* p = ln.c_str();
    if (p[0]<'0'||p[0]>'9') return false;
    char* nx;
    t.ts = std::strtoll(p,&nx,10);
    if (*nx!=',') return false; p=nx+1;
    double a=std::strtod(p,&nx);
    if (*nx!=',') return false; p=nx+1;
    double b=std::strtod(p,&nx);
    if (a<=0||b<=0) return false;
    t.mid=(std::min(a,b)+std::max(a,b))*0.5;
    return t.mid>0;
}

static int sess(long long ms){
    int h=(int)((ms/1000/3600)%24);
    if(h>=21||h<1) return 0;
    if(h<7)  return 6;
    if(h<9)  return 1;
    if(h<13) return 2;
    if(h<17) return 3;
    if(h<19) return 4;
    return 5;
}
static const char* sname(int s){
    switch(s){case 0:return"DEAD";case 1:return"LON_OPEN";
    case 2:return"LON_CORE";case 3:return"OVERLAP";
    case 4:return"NY";case 5:return"NY_LATE";case 6:return"ASIA";}return"?";
}

int main(int argc, char** argv){
    if(argc<2){puts("usage: StatScan ticks.csv");return 0;}
    std::ifstream f(argv[1]);
    if(!f){puts("cannot open");return 1;}

    // Load sample -- every 10th tick to fit in memory
    std::vector<Tick> T;
    T.reserve(15000000);
    std::string line; Tick t;
    int skip=0;
    while(std::getline(f,line)){
        if(!parse(line,t)) continue;
        if(++skip%10!=0) continue;
        T.push_back(t);
    }
    const int N=(int)T.size();
    printf("Loaded %d ticks (sampled 1:10)\n\n",N);

    // ── 1. AUTOCORRELATION OF RETURNS ────────────────────────────────────
    printf("=== 1. AUTOCORRELATION OF RETURNS ===\n");
    printf("Positive = momentum (trend), Negative = mean reversion\n\n");
    {
        std::vector<double> r(N-1);
        for(int i=0;i<N-1;i++) r[i]=T[i+1].mid-T[i].mid;
        double mean_r=0;
        for(double x:r) mean_r+=x; mean_r/=r.size();
        double var_r=0;
        for(double x:r) var_r+=(x-mean_r)*(x-mean_r); var_r/=r.size();

        printf("  %-8s  %10s  %s\n","Lag","AutoCorr","Interpretation");
        printf("  %s\n",std::string(50,'-').c_str());
        for(int lag : {1,2,5,10,20,50,100,200,500}){
            if(lag>=(int)r.size()) break;
            double cov=0;
            int cnt=0;
            for(int i=0;i<(int)r.size()-lag;i+=5){ // stride for speed
                cov+=(r[i]-mean_r)*(r[i+lag]-mean_r);
                cnt++;
            }
            double ac = (var_r>0 && cnt>0) ? (cov/cnt)/var_r : 0;
            const char* interp = ac>0.02?"MOMENTUM":ac<-0.02?"MEAN_REVERT":"RANDOM";
            printf("  %-8d  %+10.4f  %s\n",lag,ac,interp);
        }
    }

    // ── 2. CONDITIONAL CONTINUATION vs REVERSAL ──────────────────────────
    printf("\n=== 2. CONDITIONAL CONTINUATION vs REVERSAL ===\n");
    printf("After a move of X pts in T ticks, does price continue or reverse?\n\n");
    {
        // Measure in 100-tick windows (sampled = ~1000 real ticks = ~100s)
        const int WINDOW=100, FWD=50;
        struct Outcome { int cont=0, rev=0, flat=0; };
        std::map<std::string,Outcome> res;

        for(int i=WINDOW;i<N-FWD;i++){
            double move = T[i].mid - T[i-WINDOW].mid;
            double fwd  = T[i+FWD].mid - T[i].mid;

            // Classify by move size
            double atr=0;
            for(int k=i-WINDOW;k<i;k++) atr+=std::fabs(T[k+1].mid-T[k].mid);
            atr/=WINDOW; if(atr<0.001) continue;

            double move_atr = move/atr;
            std::string bucket;
            if(std::fabs(move_atr)<1) continue; // skip small moves
            else if(std::fabs(move_atr)<2) bucket="move_1-2xATR";
            else if(std::fabs(move_atr)<3) bucket="move_2-3xATR";
            else if(std::fabs(move_atr)<5) bucket="move_3-5xATR";
            else                           bucket="move_5xATR+";

            double fwd_threshold = atr * 0.5; // need 0.5 ATR to count as directional
            bool continued = (move>0 && fwd>fwd_threshold) || (move<0 && fwd<-fwd_threshold);
            bool reversed  = (move>0 && fwd<-fwd_threshold) || (move<0 && fwd>fwd_threshold);

            if(continued) res[bucket].cont++;
            else if(reversed) res[bucket].rev++;
            else res[bucket].flat++;
        }

        printf("  %-18s  %6s  %6s  %6s  %8s\n","Move","Cont%","Rev%","Flat%","N");
        printf("  %s\n",std::string(52,'-').c_str());
        for(auto& kv : res){
            auto& o=kv.second;
            int tot=o.cont+o.rev+o.flat;
            if(tot<100) continue;
            printf("  %-18s  %5.1f%%  %5.1f%%  %5.1f%%  %8d\n",
                   kv.first.c_str(),
                   100.0*o.cont/tot, 100.0*o.rev/tot, 100.0*o.flat/tot, tot);
        }
        printf("\n  If Cont%% > 55%%: build MOMENTUM engine for this size\n");
        printf("  If Rev%%  > 55%%: build FADE engine for this size\n");
    }

    // ── 3. SESSION VOLATILITY AND RETURN PROFILE ─────────────────────────
    printf("\n=== 3. SESSION PROFILE ===\n");
    printf("Volatility and directional persistence by session\n\n");
    {
        struct SessStats {
            double sum_move=0, sum_abs=0, sum_sq=0;
            int n=0, pos=0, neg=0;
            std::vector<double> moves;
        };
        SessStats ss[7];

        const int LOOK=200; // ~20min sampled
        for(int i=LOOK;i<N;i++){
            int s=sess(T[i].ts);
            double move = T[i].mid - T[i-LOOK].mid;
            double vol  = 0;
            for(int k=i-LOOK;k<i;k++) vol+=std::fabs(T[k+1].mid-T[k].mid);
            vol/=LOOK;
            ss[s].sum_move += move;
            ss[s].sum_abs  += std::fabs(move);
            ss[s].sum_sq   += move*move;
            ss[s].n++;
            if(move>0) ss[s].pos++;
            else if(move<0) ss[s].neg++;
            if(ss[s].moves.size()<50000) ss[s].moves.push_back(move);
        }

        printf("  %-12s  %7s  %8s  %7s  %8s  %s\n",
               "Session","Vol/tick","MeanMove","PosRate","Skew","Interpretation");
        printf("  %s\n",std::string(72,'-').c_str());
        for(int s=0;s<7;s++){
            auto& st=ss[s];
            if(st.n<100) continue;
            double vol  = st.sum_abs/st.n;
            double mean = st.sum_move/st.n;
            double var  = st.sum_sq/st.n - mean*mean;
            double std_ = std::sqrt(std::max(0.0,var));
            double pos_rate = (double)st.pos/(st.pos+st.neg)*100;
            // Skew: mean/std ratio shows if directional
            double skew = std_>0 ? mean/std_ : 0;
            const char* interp =
                pos_rate>55 ? "BULLISH BIAS" :
                pos_rate<45 ? "BEARISH BIAS" :
                vol>0.05    ? "HIGH VOL/NEUTRAL" : "QUIET/NEUTRAL";
            printf("  %-12s  %7.4f  %+8.4f  %6.1f%%  %+8.4f  %s\n",
                   sname(s), vol, mean, pos_rate, skew, interp);
        }
    }

    // ── 4. TIME-OF-DAY MOVE DISTRIBUTION ─────────────────────────────────
    printf("\n=== 4. LARGE MOVE FREQUENCY BY HOUR ===\n");
    printf("When do 2pt+ moves (in 100 sampled ticks ~100s) occur?\n\n");
    {
        std::map<int,std::pair<int,int>> hourly; // hour -> {count, large_count}
        const int LOOK=100;
        for(int i=LOOK;i<N;i++){
            int h=(int)((T[i].ts/1000/3600)%24);
            double move=std::fabs(T[i].mid-T[i-LOOK].mid);
            hourly[h].first++;
            if(move>=2.0) hourly[h].second++;
        }
        printf("  Hour(UTC)  TotalSamples  Large2pt+  Rate\n");
        printf("  %s\n",std::string(45,'-').c_str());
        for(auto& kv:hourly){
            int h=kv.first;
            int tot=kv.second.first, large=kv.second.second;
            if(tot<100) continue;
            printf("  %02d:00 UTC  %12d  %9d  %4.1f%%\n",
                   h,tot,large,100.0*large/tot);
        }
    }

    // ── 5. MOMENTUM DECAY ────────────────────────────────────────────────
    printf("\n=== 5. MOMENTUM PERSISTENCE (after confirmed move) ===\n");
    printf("After price moves 2pt in 60 sampled ticks, what is P(continue 1pt) vs P(reverse 1pt)?\n\n");
    {
        const int TRIGGER_TICKS=60;
        const double TRIGGER_MOVE=2.0;
        const double TARGET=1.0;
        int cont=0, rev=0, timeout=0;
        const int MAX_WAIT=200;

        for(int i=TRIGGER_TICKS;i<N-MAX_WAIT;i++){
            double move=T[i].mid-T[i-TRIGGER_TICKS].mid;
            if(std::fabs(move)<TRIGGER_MOVE) continue;

            bool is_up=(move>0);
            double entry=T[i].mid;
            bool found=false;

            for(int k=1;k<=MAX_WAIT;k++){
                double dm=T[i+k].mid-entry;
                if(is_up && dm>=TARGET) { cont++; found=true; break; }
                if(is_up && dm<=-TARGET){ rev++;  found=true; break; }
                if(!is_up && dm<=-TARGET){cont++; found=true; break; }
                if(!is_up && dm>=TARGET){ rev++;  found=true; break; }
            }
            if(!found) timeout++;
        }

        int total=cont+rev+timeout;
        if(total>0){
            printf("  After 2pt move in 60 ticks:\n");
            printf("  Continue +1pt:  %d (%.1f%%)\n",cont,100.0*cont/total);
            printf("  Reverse  -1pt:  %d (%.1f%%)\n",rev,100.0*rev/total);
            printf("  Timeout (200t): %d (%.1f%%)\n",timeout,100.0*timeout/total);
            printf("\n");
            if(100.0*cont/total>55)
                printf("  >> MOMENTUM SIGNAL: continuation > 55%% -- trend engine valid\n");
            else if(100.0*rev/total>55)
                printf("  >> MEAN REVERSION: reversal > 55%% -- fade engine valid\n");
            else
                printf("  >> NO EDGE: continuation %.1f%% -- random walk territory\n",
                       100.0*cont/total);
        }
    }

    return 0;
}
