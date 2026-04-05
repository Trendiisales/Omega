#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <map>
#include <string>
#include <algorithm>
#include <numeric>

struct Tick { long long ts; double bid, ask, mid; };

struct Stats {
    int n=0; double sum=0, sum_sq=0, wins=0;
    void add(double r) {
        n++; sum+=r; sum_sq+=r*r;
        if(r>0) wins++;
    }
    double mean()   const { return n?sum/n:0; }
    double wr()     const { return n?wins/n*100:0; }
    double stddev() const {
        if(n<2) return 0;
        double m=mean(); return std::sqrt(std::max(0.0,sum_sq/n-m*m));
    }
    double sharpe() const { double s=stddev(); return s>0?mean()/s:0; }
};

static bool parse(const std::string& ln, Tick& t) {
    const char* p=ln.c_str(); char* nx;
    if(p[0]<'0'||p[0]>'9') return false;
    t.ts=std::strtoll(p,&nx,10); if(*nx!=',') return false; p=nx+1;
    double a=std::strtod(p,&nx); if(*nx!=',') return false; p=nx+1;
    double b=std::strtod(p,&nx);
    if(a<=0||b<=0) return false;
    if(a>b){t.ask=a;t.bid=b;}else{t.ask=b;t.bid=a;}
    t.mid=(t.bid+t.ask)*0.5;
    return t.bid>0&&t.ask>t.bid;
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
    switch(s){
    case 0:return"DEAD"; case 1:return"LON_OPEN"; case 2:return"LON_CORE";
    case 3:return"OVERLAP"; case 4:return"NY"; case 5:return"NY_LATE"; case 6:return"ASIA";
    }return"?";
}

int main(int argc, char** argv){
    if(argc<2){puts("usage: EdgeDiscovery ticks.csv");return 0;}
    std::ifstream f(argv[1]);
    if(!f){puts("cannot open");return 1;}

    std::vector<Tick> ticks;
    ticks.reserve(140000000);
    std::string line;
    Tick t;
    while(std::getline(f,line))
        if(parse(line,t)) ticks.push_back(t);

    const int N=(int)ticks.size();
    printf("Loaded %d ticks\n",N);

    // Forward windows to test (in ticks)
    // At ~5-15 ticks/sec on gold:
    // 100t ~ 10s, 500t ~ 50s, 2000t ~ 3min, 5000t ~ 8min, 10000t ~ 17min
    const int FWD[] = {100, 500, 2000, 5000, 10000};
    const int NFWD  = 5;
    const int LOOK  = 200;
    const int MAXFWD= 10000;

    // Stats[signal][fwd_idx]
    std::map<std::string, Stats[5]> stats;

    for(int i=LOOK; i<N-MAXFWD; i++){
        auto& tk = ticks[i];
        double p = tk.mid;
        int ss   = sess(tk.ts);

        // ATR: mean abs 1-tick move over last 200 ticks
        double atr=0;
        for(int k=i-LOOK;k<i;k++) atr+=std::fabs(ticks[k+1].mid-ticks[k].mid);
        atr/=LOOK;
        if(atr<0.0005) continue;

        // Drift persistence: fraction of last 50 ticks in one direction
        int up=0,dn=0;
        for(int k=i-50;k<i;k++){
            if(ticks[k+1].mid>ticks[k].mid) up++;
            else if(ticks[k+1].mid<ticks[k].mid) dn++;
        }
        double pers=(double)std::max(up,dn)/50.0;
        bool bull_pers = pers>=0.70 && up>dn;
        bool bear_pers = pers>=0.70 && dn>up;

        // Volatility ratio
        double vol10=std::fabs(ticks[i].mid-ticks[i-10].mid);
        double vol50=0;
        for(int k=i-50;k<i;k++) vol50+=std::fabs(ticks[k+1].mid-ticks[k].mid);
        vol50/=50;
        double vratio=(vol50>0.0001)?vol10/vol50:1.0;

        // Range compression
        double lo=p,hi=p;
        for(int k=i-50;k<i;k++){
            if(ticks[k].mid<lo) lo=ticks[k].mid;
            if(ticks[k].mid>hi) hi=ticks[k].mid;
        }
        bool compressed=(hi-lo)<atr*2.0;

        // Overextension (50-tick move vs ATR)
        double move50=ticks[i].mid-ticks[i-50].mid;
        bool overext_up = move50 >  atr*3.0;
        bool overext_dn = move50 < -atr*3.0;

        // Momentum (last 10 ticks)
        double mom10=ticks[i].mid-ticks[i-10].mid;
        bool bull_mom = mom10 >  atr*0.5;
        bool bear_mom = mom10 < -atr*0.5;

        std::string ss_str = sname(ss);

        // Compute forward returns at each horizon
        for(int fi=0;fi<NFWD;fi++){
            int fwd=FWD[fi];
            double ret=ticks[i+fwd].mid - p;
            char key[64];

            // GoldFlow archetype: persistence in each session
            if(bull_pers){
                snprintf(key,64,"PERSIST_BULL_%s_fwd%d",ss_str,fwd);
                stats[key][fi].add(ret);
                snprintf(key,64,"PERSIST_BULL_ALL_fwd%d",fwd);
                stats[key][fi].add(ret);
            }
            if(bear_pers){
                snprintf(key,64,"PERSIST_BEAR_%s_fwd%d",ss_str,fwd);
                stats[key][fi].add(-ret); // flip: bear is right when ret<0
                snprintf(key,64,"PERSIST_BEAR_ALL_fwd%d",fwd);
                stats[key][fi].add(-ret);
            }

            // Compression+expansion breakout
            if(compressed && vratio>1.5){
                if(bull_mom){
                    snprintf(key,64,"COMP_BREAK_BULL_%s_fwd%d",ss_str,fwd);
                    stats[key][fi].add(ret);
                }
                if(bear_mom){
                    snprintf(key,64,"COMP_BREAK_BEAR_%s_fwd%d",ss_str,fwd);
                    stats[key][fi].add(-ret);
                }
            }

            // Fade overextension
            if(overext_up){
                snprintf(key,64,"FADE_SHORT_%s_fwd%d",ss_str,fwd);
                stats[key][fi].add(-ret); // fade up = short = win when ret<0
            }
            if(overext_dn){
                snprintf(key,64,"FADE_LONG_%s_fwd%d",ss_str,fwd);
                stats[key][fi].add(ret);
            }

            // Combined: persist + compressed + expanding (GoldFlow's full signal)
            if((bull_pers||bear_pers) && compressed && vratio>1.5){
                double dir_ret = bull_pers ? ret : -ret;
                snprintf(key,64,"GF_FULL_%s_fwd%d",ss_str,fwd);
                stats[key][fi].add(dir_ret);
                snprintf(key,64,"GF_FULL_ALL_fwd%d",fwd);
                stats[key][fi].add(dir_ret);
            }
        }
    }

    // Print results -- for each unique signal base, show all forward windows
    // Group by stripping the _fwdN suffix
    std::map<std::string,std::map<int,Stats>> grouped;
    for(auto& kv:stats){
        const std::string& nm=kv.first;
        // find last _fwd
        size_t pos=nm.rfind("_fwd");
        if(pos==std::string::npos) continue;
        std::string base=nm.substr(0,pos);
        int fwd=std::stoi(nm.substr(pos+4));
        grouped[base][fwd]=kv.second[0]; // [0] is placeholder, use map directly
    }
    // Actually simpler: just print flat, sorted by signal then fwd
    printf("\n=== EDGE RESULTS BY FORWARD WINDOW ===\n");
    printf("Cost = 0.35pt (spread+slip). * = mean > cost.\n\n");
    printf("  %-42s  %5s  %7s  %6s  %6s\n","Signal","Count","Mean","WinRate","Sharpe");
    printf("  %s\n",std::string(75,'-').c_str());

    // Collect and sort
    std::vector<std::pair<std::string,Stats>> flat;
    for(auto& kv:stats) flat.push_back({kv.first,kv.second[0]});
    std::sort(flat.begin(),flat.end(),[](auto&a,auto&b){
        return a.second.mean()>b.second.mean();
    });

    int shown=0;
    for(auto& kv:flat){
        auto& s=kv.second;
        if(s.n<10000) continue;
        if(shown>80) break;
        char flag = s.mean()>0.35?'*':' ';
        printf("  %-42s  %5d  %+7.3f  %5.1f%%  %6.2f %c\n",
               kv.first.c_str(),s.n,s.mean(),s.wr(),s.sharpe(),flag);
        shown++;
    }

    // Summary: what's the best signal at each FWD horizon?
    printf("\n=== BEST SIGNAL PER FORWARD WINDOW ===\n\n");
    for(int fwd : {100,500,2000,5000,10000}){
        std::string best_k; double best_m=-99;
        for(auto& kv:flat){
            if(kv.first.find("fwd"+std::to_string(fwd))==std::string::npos) continue;
            if(kv.second.n<10000) continue;
            if(kv.second.mean()>best_m){best_m=kv.second.mean();best_k=kv.first;}
        }
        if(!best_k.empty())
            printf("  fwd=%-6d  best=%-40s  mean=%+.3f  %s\n",
                   fwd,best_k.c_str(),best_m,best_m>0.35?"*** PROFITABLE ***":"below cost");
    }
    return 0;
}
