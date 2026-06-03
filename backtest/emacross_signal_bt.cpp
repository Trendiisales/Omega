// EMA9xEMA21 + RSI>50 long entry (the dashboard BUY signal) as a real trade.
#include <cstdio>
#include <cmath>
#include <vector>
#include <fstream>
#include <sstream>
#include <deque>
struct Bar{double o,h,l,c;};
int main(int argc,char**argv){
    std::ifstream f(argv[1]); std::string ln; std::getline(f,ln);
    std::vector<Bar> b;
    while(std::getline(f,ln)){const char*s=ln.c_str();char*e;long ts=strtol(s,&e,10);if(*e!=',')continue;
        double o=strtod(e+1,&e);if(*e!=',')continue;double h=strtod(e+1,&e);if(*e!=',')continue;
        double l=strtod(e+1,&e);if(*e!=',')continue;double c=strtod(e+1,&e);
        if(o>0&&h>0&&l>0&&c>0)b.push_back({o,h,l,c});}
    double COST=0.37, SLm=atof(argv[2]), TPm=atof(argv[3]);
    int N=b.size();
    // EMAs + RSI(14) + ATR(14)
    double e9=b[0].c,e21=b[0].c,atr=5,prevc=b[0].c; std::deque<double> trq;
    double rg=0,rl=0; int rn=0; // rsi wilder
    bool pos=false; double entry=0,sl=0,tp=0; double e9p=e9,e21p=e21;
    double cum=0,peak=0,mdd=0,gw=0,gl=0; int nw=0,nl=0,ntr=0;
    auto close=[&](double px){double pnl=(px-entry)-COST;cum+=pnl;if(cum>peak)peak=cum;double dd=peak-cum;if(dd>mdd)mdd=dd;if(pnl>0){nw++;gw+=pnl;}else{nl++;gl+=-pnl;}ntr++;pos=false;};
    for(int i=1;i<N;i++){
        double tr=std::max({b[i].h-b[i].l,std::fabs(b[i].h-prevc),std::fabs(b[i].l-prevc)});
        if((int)trq.size()<14){trq.push_back(tr);double s=0;for(double v:trq)s+=v;atr=s/trq.size();}else atr=(atr*13+tr)/14;
        double d=b[i].c-b[i-1].c; double up=d>0?d:0, dn=d<0?-d:0;
        if(rn<14){rg+=up;rl+=dn;rn++;}else{rg=(rg*13+up)/14;rl=(rl*13+dn)/14;}
        double rsi=rl<1e-9?100:100-100/(1+rg/rl);
        prevc=b[i].c; e9p=e9;e21p=e21; e9=b[i].c*2.0/10+e9*8.0/10; e21=b[i].c*2.0/22+e21*20.0/22;
        if(pos){ if(b[i].l<=sl)close(sl); else if(b[i].h>=tp)close(tp); }
        if(!pos && i>30){
            bool crossup=(e9p<=e21p)&&(e9>e21);
            if(crossup && rsi>50 && atr>0){pos=true;entry=b[i].c;sl=entry-SLm*atr;tp=entry+TPm*atr;}
        }
    }
    double pf=gl>0?gw/gl:999;
    std::printf("EMA9xEMA21+RSI50 long  SL%.1f TP%.1f | tr=%d net=%.0f PF=%.2f win=%.0f%% mdd=%.0f\n",
        SLm,TPm,ntr,cum,pf,ntr?100.0*nw/ntr:0,mdd);
    return 0;
}
