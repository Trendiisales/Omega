#include <cstdio>
#include <cmath>
#include <vector>
#include <fstream>
#include <string>
#include <deque>
#include <algorithm>
struct Bar{double o,h,l,c;};
int main(int argc,char**argv){
    if(argc<4){printf("args\n");return 1;}
    std::ifstream f(argv[1]); if(!f){printf("open fail\n");return 1;}
    std::string ln; std::getline(f,ln); std::vector<Bar> b;
    while(std::getline(f,ln)){
        if(ln.empty())continue;
        double ts,o,h,l,c;
        if(sscanf(ln.c_str(),"%lf,%lf,%lf,%lf,%lf",&ts,&o,&h,&l,&c)!=5)continue;
        if(o>0&&h>0&&l>0&&c>0)b.push_back({o,h,l,c});
    }
    int N=(int)b.size();
    if(N<200){printf("few bars %d\n",N);return 1;}
    double COST=0.37,SLm=atof(argv[2]),TPm=atof(argv[3]);
    auto K=[](int n){return 2.0/(n+1);};
    double esa=0,dd=0,wt1=0,ef=b[0].c,es=b[0].c,atr=5,prevc=b[0].c; bool init=false;
    std::deque<double> w1h,trq; double pw1=0,pw2=0; bool havep=false;
    bool pos=false;int dir=0;double entry=0,sl=0,tp=0;
    double cum=0,peak=0,mdd=0,gw=0,gl=0;int nw=0,nl=0,ntr=0;
    auto close=[&](double px){double pnl=dir*(px-entry)-COST;cum+=pnl;if(cum>peak)peak=cum;double q=peak-cum;if(q>mdd)mdd=q;if(pnl>0){nw++;gw+=pnl;}else{nl++;gl+=-pnl;}ntr++;pos=false;};
    for(int i=1;i<N;i++){
        double ap=(b[i].h+b[i].l+b[i].c)/3.0;
        double tr=std::max({b[i].h-b[i].l,std::fabs(b[i].h-prevc),std::fabs(b[i].l-prevc)});
        if((int)trq.size()<14){trq.push_back(tr);double s=0;for(double v:trq)s+=v;atr=s/(int)trq.size();}else atr=(atr*13+tr)/14;
        prevc=b[i].c;
        if(!init){esa=ap;init=true;}
        esa+=K(10)*(ap-esa); dd+=K(10)*(std::fabs(ap-esa)-dd); double d=dd>1e-9?dd:1e-9;
        wt1+=K(21)*((ap-esa)/(0.015*d)-wt1);
        w1h.push_back(wt1); if((int)w1h.size()>4)w1h.pop_front();
        double s=0;for(double v:w1h)s+=v; double wt2=s/(int)w1h.size();
        ef+=K(21)*(b[i].c-ef); es+=K(55)*(b[i].c-es); bool up=ef>es;
        if(pos){ if(dir>0){if(b[i].l<=sl)close(sl);else if(b[i].h>=tp)close(tp);} else {if(b[i].h>=sl)close(sl);else if(b[i].l<=tp)close(tp);} }
        if(havep&&!pos&&i>60&&atr>0){
            bool cu=(wt1>wt2)&&(pw1<=pw2), cd=(wt1<wt2)&&(pw1>=pw2);
            if(cu&&up){dir=1;pos=true;entry=b[i].c;sl=entry-SLm*atr;tp=entry+TPm*atr;}
            else if(cd&&!up){dir=-1;pos=true;entry=b[i].c;sl=entry+SLm*atr;tp=entry-TPm*atr;}
        }
        pw1=wt1;pw2=wt2;havep=true;
    }
    double pf=gl>0?gw/gl:999;
    printf("WT-cross-dir SL%.1f TP%.1f | tr=%d net=%.0f PF=%.2f win=%.0f%% mdd=%.0f\n",
        SLm,TPm,ntr,cum,pf,ntr?100.0*nw/ntr:0,mdd);
    return 0;
}
