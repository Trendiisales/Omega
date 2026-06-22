// Generalized REAL-engine driver for IndexBearShortEngine. Drives the ACTUAL class
// (never a port) on real data. Two input modes:
//   --ticks <dir>   HISTDATA tick CSVs (YYYYMMDD HHMMSSmmm,bid,ask,vol), EST -> +5h UTC
//   --h1 <csv>      H1 bar CSV (ts_sec,o,h,l,c) fed as 4 ticks/bar (O,H,L,C)
// Engine param overrides (for faithful gate sweeps): --persist --don --emaslow --emafast
//   --slatr --tpr --maxhold --cooldown --atrn --cost --symbol --label
// Reports total + WF halves + per-month/year + PF, all from the engine's on_close_cb.
//
// BUILD: c++ -std=c++17 -O2 -Iinclude backtest/ibs_real_engine.cpp -o /tmp/ibs_eng
// USE: see backtest/data_manifest.json for vendor-name -> file mapping.
#include "IndexBearShortEngine.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>
#include <dirent.h>
#include <algorithm>
#include <ctime>

static int64_t hist_ms(const char* d, const char* t){ // "YYYYMMDD","HHMMSSmmm" EST->UTC+5h
    int Y=(d[0]-'0')*1000+(d[1]-'0')*100+(d[2]-'0')*10+(d[3]-'0');
    int M=(d[4]-'0')*10+(d[5]-'0'), D=(d[6]-'0')*10+(d[7]-'0');
    int hh=(t[0]-'0')*10+(t[1]-'0'), mm=(t[2]-'0')*10+(t[3]-'0'), ss=(t[4]-'0')*10+(t[5]-'0');
    int ms=(t[6]-'0')*100+(t[7]-'0')*10+(t[8]-'0');
    struct tm tmv{}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=hh; tmv.tm_min=mm; tmv.tm_sec=ss;
    return (int64_t)timegm(&tmv)*1000 + ms + 5LL*3600LL*1000LL;
}

struct Cls { int64_t ts; double pnl; };

int main(int argc,char**argv){
    std::string ticks, h1, symbol="NAS100", label="run";
    double cost=2.0;
    omega::IndexBearShortEngine eng;
    for(int i=1;i<argc;i++){
        std::string a=argv[i]; auto nv=[&](const char*f){return std::string(argv[i+1]);};
        if(a=="--ticks") ticks=argv[++i];
        else if(a=="--h1") h1=argv[++i];
        else if(a=="--symbol") symbol=argv[++i];
        else if(a=="--label") label=argv[++i];
        else if(a=="--cost") cost=atof(argv[++i]);
        else if(a=="--persist") eng.PERSIST=atoi(argv[++i]);
        else if(a=="--don") eng.DON=atoi(argv[++i]);
        else if(a=="--emaslow") eng.EMA_SLOW=atoi(argv[++i]);
        else if(a=="--emafast") eng.EMA_FAST=atoi(argv[++i]);
        else if(a=="--slatr") eng.SL_ATR=atof(argv[++i]);
        else if(a=="--tpr") eng.TP_R=atof(argv[++i]);
        else if(a=="--maxhold") eng.MAX_HOLD=atoi(argv[++i]);
        else if(a=="--cooldown") eng.COOLDOWN=atoi(argv[++i]);
        else if(a=="--atrn") eng.ATR_N=atoi(argv[++i]);
    }
    eng.symbol=symbol; eng.engine_name="IndexBearShort"; eng.shadow_mode=true;
    eng.enabled=true; eng.COST_PTS=cost; eng.lot=1.0; eng.USE_RISKOFF_GATE=false;
    std::vector<Cls> closes; int64_t cur=0;
    eng.on_close_cb=[&](const omega::TradeRecord& tr){ closes.push_back({cur, tr.pnl}); };

    size_t nin=0;
    if(!ticks.empty()){
        std::vector<std::string> files; DIR* dp=opendir(ticks.c_str()); dirent* de;
        while(dp&&(de=readdir(dp))){ std::string n=de->d_name; if(n.size()>4&&n.substr(n.size()-4)==".csv") files.push_back(ticks+"/"+n);} if(dp)closedir(dp);
        std::sort(files.begin(),files.end());
        char ln[160];
        for(auto&f:files){ FILE*cf=fopen(f.c_str(),"r"); if(!cf)continue;
            while(fgets(ln,sizeof ln,cf)){ if(ln[8]!=' ')continue; char db[9]; memcpy(db,ln,8); db[8]=0;
                const char*c1=strchr(ln,','); if(!c1)continue; double bid=atof(c1+1);
                const char*c2=strchr(c1+1,','); if(!c2)continue; double ask=atof(c2+1);
                if(bid<=0||ask<=0)continue; cur=hist_ms(db,ln+9); eng.on_tick(bid,ask,cur); ++nin; }
            fclose(cf); }
    } else if(!h1.empty()){
        FILE*cf=fopen(h1.c_str(),"r"); if(!cf){perror(h1.c_str());return 1;} char ln[256]; bool first=true;
        while(fgets(ln,sizeof ln,cf)){ if(first){first=false; if(ln[0]<'0'||ln[0]>'9')continue;}
            double ts,o,h,l,c; if(sscanf(ln,"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;
            int64_t base=(int64_t)ts*1000; double seq[4]={o,h,l,c};
            for(int k=0;k<4;k++){ cur=base+k*900000LL; eng.on_tick(seq[k],seq[k],cur); } ++nin; }
        fclose(cf);
    } else { fprintf(stderr,"need --ticks <dir> or --h1 <csv>\n"); return 2; }

    auto pf=[](const std::vector<Cls>&v){ double gp=0,gl=0;int w=0;
        for(auto&c:v){ if(c.pnl>0){gp+=c.pnl;w++;} else gl+=-c.pnl; }
        printf("n=%-3zu WR=%2.0f%% PF=%4.2f net=%+7.0fpt\n",v.size(),v.empty()?0:100.0*w/v.size(),gl>0?gp/gl:9.9,gp-gl); };
    printf("# REAL IndexBearShortEngine [%s] sym=%s persist=%d don=%d emaSlow=%d cost=%.2f -> %zu inputs, %zu closes\n",
           label.c_str(),symbol.c_str(),eng.PERSIST,eng.DON,eng.EMA_SLOW,cost,nin,closes.size());
    printf("ALL : "); pf(closes);
    size_t h=closes.size()/2;
    printf("H1  : "); pf(std::vector<Cls>(closes.begin(),closes.begin()+h));
    printf("H2  : "); pf(std::vector<Cls>(closes.begin()+h,closes.end()));
    // per-year-month
    int lastk=-1; std::vector<Cls> bk;
    auto flush=[&](int k){ if(!bk.empty()){ time_t e=bk[0].ts/1000; struct tm tmv{}; gmtime_r(&e,&tmv);
        printf("  %04d-%02d : ",tmv.tm_year+1900,tmv.tm_mon+1); pf(bk); bk.clear(); } };
    for(auto&c:closes){ time_t e=c.ts/1000; struct tm tmv{}; gmtime_r(&e,&tmv); int k=(tmv.tm_year+1900)*12+tmv.tm_mon;
        if(k!=lastk&&lastk>=0)flush(lastk); lastk=k; bk.push_back(c);} flush(lastk);
    return 0;
}
