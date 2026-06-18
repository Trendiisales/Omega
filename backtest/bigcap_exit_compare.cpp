// bigcap_exit_compare.cpp — "how SHOULD it have traded?" OLD vs NEW BigCap exit,
// SAME entries (gate4 long-only me2), paired per-trade. Drives the REAL PumpScalpEngine.
//   OLD exit: trail 5% + 240-min TIME cap, no BE-ratchet, no ride-in-profit (pre-fix LIVE).
//   NEW exit: ATR-trail(30x4) + BE-ratchet(arm3/floor2) + ride-in-profit + 8h backstop.
// Pairs trades by (sym,day,entryTs) — identical because entry config is identical.
// build: clang++ -O2 -std=c++17 -Iinclude -o /tmp/bcx backtest/bigcap_exit_compare.cpp
// run:   /tmp/bcx backtest/data/bigcap_5m.csv [slip=0.15]
#include "PumpScalpEngine.hpp"
#include <cstdio>
#include <cstdint>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <fstream>
#include <algorithm>
using omega::PumpScalpEngine; using omega::TradeRecord;
struct Bar{ int64_t ts; double o,h,l,c,v; };
struct NameDay{ std::string sym; int day; std::vector<Bar> bars; };

static PumpScalpEngine build(bool neu, double slip){
    PumpScalpEngine e;
    // SAME ENTRY both configs
    e.TF_SEC=300; e.DAY_GATE_PCT=4.0; e.LB=3; e.IG_PCT=3.0; e.VOLX=0.0; e.STRENGTH=0.60;
    e.ENTRY_MAX_EXT_PCT=0.0; e.ALLOW_SHORT=false; e.MAX_ENTRIES_PER_DAY=2;
    e.RUNUP_PCT=20.0; e.EXT_PCT=5.0; e.NEWHOD_M=8; e.VOL_REG_FILTER=true; e.REG_LB=12; e.SLOPE_MIN=0.0;
    e.NOTIONAL_USD=1000.0; e.MIN_DVOL_USD=0.0; e.PRICE_MIN=0.0; e.PYR_ADDS=0; e.HARD_PCT=6.0;
    e.SLIP_PCT=slip; e.shadow_mode=true;
    if(!neu){ // OLD exit (pre-fix live)
        e.TRAIL_PCT=5.0; e.ATR_LEN=0; e.ATR_MULT=0.0; e.BE_ARM_PCT=0.0; e.BE_FLOOR_PCT=0.0;
        e.GIVEBACK_FRAC=0.0; e.MAXHOLD_SKIP_IF_PROFIT=false; e.MAXHOLD_SEC=48*300;
    } else {  // NEW exit (deployed fix)
        e.TRAIL_PCT=0.0; e.ATR_LEN=30; e.ATR_MULT=4.0; e.BE_ARM_PCT=3.0; e.BE_FLOOR_PCT=2.0;
        e.GIVEBACK_FRAC=0.0; e.MAXHOLD_SKIP_IF_PROFIT=true; e.MAXHOLD_SEC=96*300;
    }
    e.init(); return e;
}
static void replay(const NameDay& nd, bool neu, double slip, std::vector<TradeRecord>& out){
    PumpScalpEngine e=build(neu,slip);
    e.on_trade_record=[&out](const TradeRecord& t){ out.push_back(t); };
    e.symbol=nd.sym; e.engine_name="BigCapMomo";
    for(const Bar& b:nd.bars){ int64_t t0=b.ts; int dir=e.has_open_position()?e.pos.dir:1;
        if(dir>0){e.on_price(b.o,t0);e.on_price(b.l,t0+75000);e.on_price(b.h,t0+150000);e.on_price(b.c,t0+225000);}
        else     {e.on_price(b.o,t0);e.on_price(b.h,t0+75000);e.on_price(b.l,t0+150000);e.on_price(b.c,t0+225000);}
        e.on_entry_bar(b.o,b.h,b.l,b.c,b.v,t0+290000,false); }
    if(!nd.bars.empty()&&e.has_open_position()) e.force_close(nd.bars.back().c,nd.bars.back().ts+300000);
}

int main(int argc,char**argv){
    const char* path=argc>1?argv[1]:"backtest/data/bigcap_5m.csv";
    const double slip=argc>2?atof(argv[2]):0.15;
    std::ifstream f(path); if(!f){fprintf(stderr,"open fail %s\n",path);return 2;}
    std::map<std::string,NameDay> book; std::string line; std::getline(f,line);
    while(std::getline(f,line)){ if(line.empty())continue; std::stringstream ss(line); std::string cl; std::vector<std::string> col;
        while(std::getline(ss,cl,','))col.push_back(cl); if(col.size()<8)continue;
        try{ NameDay&nd=book[col[0]+":"+col[1]]; nd.sym=col[0]; nd.day=std::stoi(col[1]);
            nd.bars.push_back({std::stoll(col[2]),std::stod(col[3]),std::stod(col[4]),std::stod(col[5]),std::stod(col[6]),std::stod(col[7])}); }catch(...){continue;} }

    struct Row{ std::string sym; int day; double entry,mfe,oldp,newp; std::string oldr,newr; bool haveOld=false,haveNew=false; };
    std::map<long long,Row> rows;  // key entryTs
    double oldNet=0,newNet=0; long oldN=0,newN=0; double oldGw=0,oldGl=0,newGw=0,newGl=0;
    std::map<std::string,int> oldRC, newRC;
    for(auto&kv:book){
        std::vector<TradeRecord> ot,nt;
        replay(kv.second,false,slip,ot); replay(kv.second,true,slip,nt);
        for(auto&t:ot){ auto&r=rows[t.entryTs]; r.sym=t.symbol;r.day=kv.second.day;r.entry=t.entryPrice;r.mfe=t.mfe;r.oldp=t.pnl;r.oldr=t.exitReason;r.haveOld=true;
            oldNet+=t.pnl;++oldN; if(t.pnl>0)oldGw+=t.pnl;else oldGl+=-t.pnl; oldRC[t.exitReason]++; }
        for(auto&t:nt){ auto&r=rows[t.entryTs]; r.sym=t.symbol;r.day=kv.second.day;r.entry=t.entryPrice;if(t.mfe>r.mfe)r.mfe=t.mfe;r.newp=t.pnl;r.newr=t.exitReason;r.haveNew=true;
            newNet+=t.pnl;++newN; if(t.pnl>0)newGw+=t.pnl;else newGl+=-t.pnl; newRC[t.exitReason]++; }
    }
    std::vector<Row> v; for(auto&kv:rows) if(kv.second.haveOld||kv.second.haveNew) v.push_back(kv.second);
    std::sort(v.begin(),v.end(),[](const Row&a,const Row&b){return a.mfe>b.mfe;});

    printf("\n=== BigCapMomo OLD vs NEW EXIT — same entries, real engine, slip=%.2f%% ===\n",slip);
    printf("TOP 16 MOVERS BY PEAK (MFE $): how each exit handled the run\n");
    printf("%-7s %8s %8s | %8s %-10s | %8s %-10s\n","sym","entry","peakMFE","OLD pnl","OLD exit","NEW pnl","NEW exit");
    printf("%s\n",std::string(74,'-').c_str());
    int shown=0; for(auto&r:v){ if(shown++>=16)break;
        printf("%-7s %8.2f %8.1f | %8.1f %-10s | %8.1f %-10s\n",
            r.sym.c_str(),r.entry,r.mfe, r.haveOld?r.oldp:0,r.haveOld?r.oldr.c_str():"-", r.haveNew?r.newp:0,r.haveNew?r.newr.c_str():"-"); }
    auto pf=[](double w,double l){return l>1e-9?w/l:(w>0?999:0);};
    printf("\nAGGREGATE:\n");
    printf("  OLD exit: n=%ld net=%+.0f PF=%.2f  exits=",oldN,oldNet,pf(oldGw,oldGl));
    for(auto&k:oldRC)printf("%s:%d ",k.first.c_str(),k.second); printf("\n");
    printf("  NEW exit: n=%ld net=%+.0f PF=%.2f  exits=",newN,newNet,pf(newGw,newGl));
    for(auto&k:newRC)printf("%s:%d ",k.first.c_str(),k.second); printf("\n");
    printf("  DELTA net: %+.0f  (%.0f%% improvement)\n", newNet-oldNet, oldNet!=0?100.0*(newNet-oldNet)/std::fabs(oldNet):0);
    return 0;
}
