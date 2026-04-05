// EdgeDiscovery v3 -- MFE/MAE structural edge scanner
// Measures maximum favourable/adverse excursion after structural events.
#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>

struct Tick { long long ts; double mid; };

struct MFEStats {
    std::vector<float> mfe, mae;
    void add(float mf, float ma) { mfe.push_back(mf); mae.push_back(ma); }
    int n() const { return (int)mfe.size(); }
    double pct(const std::vector<float>& v, double p) const {
        if (v.empty()) return 0;
        std::vector<float> s = v;
        std::sort(s.begin(), s.end());
        int idx = (int)(p * (s.size()-1));
        return s[idx];
    }
    double mfe_p50() const { return pct(mfe,0.50); }
    double mfe_p75() const { return pct(mfe,0.75); }
    double mfe_p90() const { return pct(mfe,0.90); }
    double mae_p50() const { return pct(mae,0.50); }
    // Expectancy: TP=target, SL=stop, cost applied
    double expect(double tgt, double stp, double cost) const {
        int w=0, l=0;
        for (int i=0; i<n(); i++) {
            if (mfe[i] >= (float)tgt) w++;
            else l++;
        }
        if (!n()) return 0;
        double wr=(double)w/n();
        return wr*(tgt-cost) - (1-wr)*(stp+cost);
    }
};

static bool parse(const std::string& ln, Tick& t) {
    const char* p=ln.c_str();
    if (p[0]<'0'||p[0]>'9') return false;
    char* nx;
    t.ts=std::strtoll(p,&nx,10);
    if (*nx!=',') return false; p=nx+1;
    double a=std::strtod(p,&nx);
    if (*nx!=',') return false; p=nx+1;
    double b=std::strtod(p,&nx);
    if (a<=0||b<=0) return false;
    double bid=std::min(a,b), ask=std::max(a,b);
    if (ask<=bid) return false;
    t.mid=(bid+ask)*0.5;
    return true;
}

static const char* sname(long long ms) {
    int h=(int)((ms/1000/3600)%24);
    if (h>=21||h<1) return "DEAD";
    if (h<7)  return "ASIA";
    if (h<9)  return "LON_OPEN";
    if (h<13) return "LON_CORE";
    if (h<17) return "OVERLAP";
    if (h<19) return "NY";
    return "NY_LATE";
}

int main(int argc, char** argv) {
    if (argc<2) { puts("usage: EdgeDiscovery ticks.csv"); return 0; }
    std::ifstream f(argv[1]);
    if (!f) { puts("cannot open"); return 1; }
    std::vector<Tick> ticks;
    ticks.reserve(140000000);
    std::string line; Tick t;
    while (std::getline(f,line))
        if (parse(line,t)) ticks.push_back(t);
    const int N=(int)ticks.size();
    printf("Loaded %d ticks\n\n",N);

    const int LOOK=100, WINDOW=2000, STEP=5;
    std::map<std::string,MFEStats> stats;

    for (int i=LOOK; i<N-WINDOW; i+=STEP) {
        double p=ticks[i].mid;
        const char* ss=sname(ticks[i].ts);
        if (strcmp(ss,"DEAD")==0) continue;

        // ATR
        double atr=0;
        for (int k=i-LOOK;k<i;k++) atr+=std::fabs(ticks[k+1].mid-ticks[k].mid);
        atr/=LOOK;
        if (atr<0.0005) continue;

        // Range
        double lo=p,hi=p;
        for (int k=i-LOOK;k<i;k++){
            if(ticks[k].mid<lo)lo=ticks[k].mid;
            if(ticks[k].mid>hi)hi=ticks[k].mid;
        }
        bool compressed=(hi-lo)<atr*1.5;

        // Drift persistence
        int up=0,dn=0;
        for (int k=i-50;k<i;k++){
            if(ticks[k+1].mid>ticks[k].mid)up++;
            else if(ticks[k+1].mid<ticks[k].mid)dn++;
        }
        double pers=(double)std::max(up,dn)/50.0;
        bool bull_pers=pers>=0.70&&up>dn;
        bool bear_pers=pers>=0.70&&dn>up;

        // Volatility expansion
        double vr10=std::fabs(ticks[i].mid-ticks[i-10].mid)/(atr*10+0.0001);
        bool expanding=vr10>1.5;

        // Overextension
        double m100=ticks[i].mid-ticks[i-LOOK].mid;
        bool ovx_up=m100>atr*3.0, ovx_dn=m100<-atr*3.0;

        // Compute MFE/MAE
        auto mfemae=[&](bool is_long)->std::pair<float,float>{
            float mf=0,ma=0;
            for(int k=i+1;k<=i+WINDOW;k++){
                float mv=is_long?(float)(ticks[k].mid-p):(float)(p-ticks[k].mid);
                if(mv>mf)mf=mv;
                if(-mv>ma)ma=-mv;
            }
            return{mf,ma};
        };
        auto abs_mfe=[&]()->float{
            float mx=0;
            for(int k=i+1;k<=i+WINDOW;k++){
                float mv=(float)std::fabs(ticks[k].mid-p);
                if(mv>mx)mx=mv;
            }
            return mx;
        };

        char key[80];

        // Compression breakout
        if (compressed&&expanding) {
            stats["COMP_BREAK_ABS"].add(abs_mfe(),0);
            snprintf(key,80,"COMP_BREAK_%s",ss);
            stats[key].add(abs_mfe(),0);
        }
        if (compressed&&expanding&&bull_pers){
            auto[mf,ma]=mfemae(true);
            stats["COMP_BREAK_BULL"].add(mf,ma);
            snprintf(key,80,"COMP_BREAK_BULL_%s",ss); stats[key].add(mf,ma);
        }
        if (compressed&&expanding&&bear_pers){
            auto[mf,ma]=mfemae(false);
            stats["COMP_BREAK_BEAR"].add(mf,ma);
            snprintf(key,80,"COMP_BREAK_BEAR_%s",ss); stats[key].add(mf,ma);
        }

        // Persistence (GoldFlow archetype)
        if (bull_pers){
            auto[mf,ma]=mfemae(true);
            stats["PERSIST_BULL"].add(mf,ma);
            snprintf(key,80,"PERSIST_BULL_%s",ss); stats[key].add(mf,ma);
        }
        if (bear_pers){
            auto[mf,ma]=mfemae(false);
            stats["PERSIST_BEAR"].add(mf,ma);
            snprintf(key,80,"PERSIST_BEAR_%s",ss); stats[key].add(mf,ma);
        }

        // Fade overextension (bleed-flip candidate)
        if (ovx_up){
            auto[mf,ma]=mfemae(false);
            stats["FADE_SHORT"].add(mf,ma);
            snprintf(key,80,"FADE_SHORT_%s",ss); stats[key].add(mf,ma);
        }
        if (ovx_dn){
            auto[mf,ma]=mfemae(true);
            stats["FADE_LONG"].add(mf,ma);
            snprintf(key,80,"FADE_LONG_%s",ss); stats[key].add(mf,ma);
        }

        // GoldFlow full archetype
        if (bull_pers&&compressed&&expanding){
            auto[mf,ma]=mfemae(true);
            stats["GF_ARCH_BULL"].add(mf,ma);
            snprintf(key,80,"GF_ARCH_BULL_%s",ss); stats[key].add(mf,ma);
        }
        if (bear_pers&&compressed&&expanding){
            auto[mf,ma]=mfemae(false);
            stats["GF_ARCH_BEAR"].add(mf,ma);
            snprintf(key,80,"GF_ARCH_BEAR_%s",ss); stats[key].add(mf,ma);
        }
    }

    const double TGT=1.2, STP=0.6, COST=0.35;
    printf("=== MFE/MAE STRUCTURAL EDGE SCAN ===\n");
    printf("Window=%d ticks (~%.0fs)  Target=%.1fpt Stop=%.1fpt Cost=%.2fpt\n\n",
           WINDOW,WINDOW/10.0,TGT,STP,COST);
    printf("  %-32s  %6s  %6s  %6s  %6s  %7s\n",
           "Signal","Count","MFEp50","MFEp75","MAEp50","Expect");
    printf("  %s\n",std::string(72,'-').c_str());

    std::vector<std::pair<std::string,MFEStats*>> sv;
    for (auto& kv:stats) sv.push_back({kv.first,&kv.second});
    std::sort(sv.begin(),sv.end(),[&](auto& a,auto& b){
        return a.second->expect(TGT,STP,COST)>b.second->expect(TGT,STP,COST);
    });

    int profitable=0;
    for (auto& [k,s]:sv){
        if (s->n()<200) continue;
        double ex=s->expect(TGT,STP,COST);
        char flag=ex>0?'*':' ';
        if(ex>0) profitable++;
        printf("  %-32s  %6d  %6.2f  %6.2f  %6.2f  %+7.3f %c\n",
               k.c_str(),s->n(),
               s->mfe_p50(),s->mfe_p75(),s->mae_p50(),ex,flag);
    }
    printf("\n  * = positive expectancy. Profitable: %d/%d\n",
           profitable,(int)sv.size());
    return 0;
}
