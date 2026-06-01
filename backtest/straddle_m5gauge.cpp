// =============================================================================
// straddle_m5gauge.cpp -- test using M5 as a "detector" to exit an M15/M30
// straddle EARLY when the breakout is failing (before the 3*ATR stop). Processes
// M5 natively; box/entry decided at the higher-TF boundary, management on M5.
//
// Detector modes (env DET):
//   none     baseline: hard SL(3*ATR) + 1R TP only
//   boxback  exit if an M5 bar closes back across the entry (breakout reclaimed)
//   mom      exit if M5 close moves K*m5ATR against entry (DETK, default 1.0)
//   slow     exit if M5 EMA(f)<EMA(s) flips against the position
//
//   g++ -std=c++17 -O2 -o backtest/straddle_m5gauge backtest/straddle_m5gauge.cpp
//   ./backtest/straddle_m5gauge <m5.csv> <tf_mult> <boxN> <stop_atr> <cost> [oos]
//     tf_mult = M5 bars per box-TF bar (3=M15, 6=M30)
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <algorithm>

struct Bar{int64_t ts;double o,h,l,c;};

int main(int argc,char**argv){
    if(argc<6){std::fprintf(stderr,"usage: %s <m5.csv> <tf_mult> <boxN> <stop_atr> <cost> [oos]\n",argv[0]);return 1;}
    const char* path=argv[1]; int tfm=atoi(argv[2]); int boxN=atoi(argv[3]);
    double stopm=atof(argv[4]), COST=atof(argv[5]); double oos=(argc>6)?atof(argv[6]):0.0;
    std::string DET=getenv("DET")?getenv("DET"):"none";
    double DETK=getenv("DETK")?atof(getenv("DETK")):1.0;
    double TPr=getenv("TP")?atof(getenv("TP")):1.0;
    int ef_p=getenv("EF")?atoi(getenv("EF")):9, es_p=getenv("ES")?atoi(getenv("ES")):21;

    // load m5
    std::vector<Bar> m5; { std::ifstream f(path); if(!f){std::fprintf(stderr,"open fail\n");return 1;}
        std::string ln; std::getline(f,ln);
        while(std::getline(f,ln)){ if(ln.empty())continue; const char* s=ln.c_str();char* e=nullptr;
            int64_t ts=strtoll(s,&e,10); if(*e!=',')continue; double o=strtod(e+1,&e);if(*e!=',')continue;
            double h=strtod(e+1,&e);if(*e!=',')continue; double l=strtod(e+1,&e);if(*e!=',')continue; double c=strtod(e+1,&e);
            if(o>0&&h>0&&l>0&&c>0) m5.push_back({ts,o,h,l,c}); } }
    if((int)m5.size()<2000){std::fprintf(stderr,"few m5\n");return 1;}
    int evalStart=(oos>0&&oos<1)?(int)(m5.size()*(1.0-oos)):0;

    // TF box state
    std::deque<double> bh_,bl_; double tfatr=0; int tfatr_n=0; double tfatr_sum=0; double tf_prevc=0;
    Bar curtf{0,0,0,0,0}; int tfcount=0; bool tfopen=false;
    double buy=0,sell=0; bool armed=false;
    // m5 atr + ema
    const int ATRP=14; double m5atr=5; std::deque<double> m5tr; double m5pc=0;
    double ef=0,es=0; bool einit=false; double kf=2.0/(ef_p+1),ks=2.0/(es_p+1);

    bool pos=false; int dir=0; double entry=0,sl=0,tp=0,sld=0; int barsHeld=0;
    double cum=0,peak=0,mdd=0; int nw=0,nl=0; double gw=0,gl=0; int ntr=0; std::vector<double> tp_;
    int detExits=0;

    auto close=[&](double px){ double pnl=(dir>0?(px-entry):(entry-px))-COST; cum+=pnl;
        if(cum>peak)peak=cum; double dd=peak-cum; if(dd>mdd)mdd=dd;
        if(pnl>0){nw++;gw+=pnl;}else if(pnl<0){nl++;gl+=-pnl;} tp_.push_back(pnl); ntr++; pos=false; };

    for(size_t i=0;i<m5.size();++i){
        const Bar& b=m5[i];
        // m5 ATR
        if(m5pc>0){ double tr=std::max({b.h-b.l,std::fabs(b.h-m5pc),std::fabs(b.l-m5pc)}); m5tr.push_back(tr); if((int)m5tr.size()>ATRP)m5tr.pop_front(); double s=0;for(double v:m5tr)s+=v; m5atr=s/m5tr.size(); }
        m5pc=b.c;
        // m5 EMA
        if(!einit){ef=es=b.c;einit=true;} else {ef=b.c*kf+ef*(1-kf);es=b.c*ks+es*(1-ks);}

        // ---- manage open pos on this m5 bar ----
        if(pos){
            barsHeld++;
            // hard SL / TP intrabar (stop priority)
            if(dir>0){ if(b.l<=sl){close(sl);} else if(b.h>=tp){close(tp);} }
            else     { if(b.h>=sl){close(sl);} else if(b.l<=tp){close(tp);} }
            // detector (only if still open + held >=1 bar)
            if(pos && barsHeld>=1){
                bool ex=false;
                if(DET=="boxback"){ if(dir>0 && b.c<entry) ex=true; if(dir<0 && b.c>entry) ex=true; }
                else if(DET=="mom"){ if(dir>0 && b.c < entry-DETK*m5atr) ex=true; if(dir<0 && b.c > entry+DETK*m5atr) ex=true; }
                else if(DET=="slow"){ if(dir>0 && ef<es) ex=true; if(dir<0 && ef>es) ex=true; }
                if(ex){ close(b.c); detExits++; }
            }
        }

        // ---- aggregate m5 -> TF bar ----
        if(!tfopen){ curtf={b.ts,b.o,b.h,b.l,b.c}; tfopen=true; tfcount=1; }
        else { if(b.h>curtf.h)curtf.h=b.h; if(b.l<curtf.l)curtf.l=b.l; curtf.c=b.c; tfcount++; }
        bool tfClosed=false;
        if(tfcount>=tfm){ // TF bar complete
            bh_.push_back(curtf.h); bl_.push_back(curtf.l);
            while((int)bh_.size()>boxN+2){bh_.pop_front();bl_.pop_front();}
            // TF ATR
            if(tf_prevc>0){ double tr=std::max({curtf.h-curtf.l,std::fabs(curtf.h-tf_prevc),std::fabs(curtf.l-tf_prevc)});
                if(tfatr_n<ATRP){tfatr_sum+=tr;tfatr_n++; if(tfatr_n==ATRP)tfatr=tfatr_sum/ATRP;} else tfatr=(tfatr*(ATRP-1)+tr)/ATRP; }
            tf_prevc=curtf.c; tfopen=false; tfClosed=true;
        }

        // ---- arm straddle at TF close ----
        if(tfClosed && !pos && (int)bh_.size()>=boxN+1 && tfatr>0){
            int sz=(int)bh_.size(); double H=bh_[sz-1-boxN],L=bl_[sz-1-boxN];
            for(int k=sz-boxN;k<sz-1;++k){ if(bh_[k]>H)H=bh_[k]; if(bl_[k]<L)L=bl_[k]; }
            buy=H; sell=L; armed=true;
        } else if(tfClosed && pos){ armed=false; }

        // ---- intrabar OCO fill on m5 ----
        if((int)i>=evalStart && !pos && armed && tfatr>0){
            int side=0; double fill=0;
            if(b.h>=buy && buy>0){side=1;fill=buy;} else if(b.l<=sell && sell>0){side=-1;fill=sell;}
            if(side!=0){ sld=stopm*tfatr; double tpd=TPr*sld;
                pos=true;dir=side;entry=fill; sl=fill-side*sld; tp=fill+side*tpd; barsHeld=0; armed=false; }
        }
    }
    if(pos) close(m5.back().c);
    double pf=(gl>0)?gw/gl:(gw>0?999:0), hit=(nw+nl>0)?100.0*nw/(nw+nl):0;
    double years; {int n=(int)m5.size();int s=std::max(evalStart,1); years=(m5[n-1].ts-m5[s].ts)/86400.0/365.25;}
    double sh=0; if(ntr>=2&&years>0){double m=0;for(double v:tp_)m+=v;m/=ntr;double s=0;for(double v:tp_)s+=(v-m)*(v-m);double sd=std::sqrt(s/(ntr-1));if(sd>0)sh=(m/sd)*std::sqrt((double)ntr/years);}
    std::printf("DET=%-7s tfm%d boxN%d | tr=%-4d net=%-7.0f PF=%.2f Sh=%+.2f win=%.0f%% mdd=%.0f detExit=%d avgLoss=%.2f\n",
        DET.c_str(),tfm,boxN,ntr,cum,pf,sh,hit,mdd,detExits, nl>0?-gl/nl:0.0);
    return 0;
}
