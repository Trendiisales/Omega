// ConnorsRSI2 enhancement sweep (faithful daily replica) — find gains before wiring.
// Levers: RSI_IN (entry oversold), EXIT_MODE (0=hold N, 1=RSI>=exit, 2=close>SMA(short)),
// EXIT_RSI, SHORT_SMA, TREND_SMA, MAXHOLD cap, SCALEIN (add a 2nd unit if RSI deepens).
// RUN: connors_opt <daily.csv ts,o,h,l,c> <label> [cost_pts=4]
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <ctime>
struct Bar{int64_t ts;double o,h,l,c;};
static std::vector<Bar> load(const char*p){std::vector<Bar>v;std::ifstream f(p);std::string l;
 while(std::getline(f,l)){if(l.empty()||(l[0]<'0'||l[0]>'9'))continue;Bar b{};double t;
  if(std::sscanf(l.c_str(),"%lf,%lf,%lf,%lf,%lf",&t,&b.o,&b.h,&b.l,&b.c)!=5)continue;b.ts=(int64_t)t;v.push_back(b);}return v;}
static int yr(int64_t s){std::time_t t=s;std::tm g{};gmtime_r(&t,&g);return g.tm_year+1900;}
struct Cfg{double rsi_in;int exit_mode;double exit_rsi;int short_sma;int trend;int maxhold;bool scalein;double cost;};
struct Res{int n;double pf,net,h1,h2;int wr;double worst;int y22;};
static double rsi2(const std::vector<Bar>&B,int i){ if(i<2)return 50; double g=0,l=0;
 for(int k=i-1;k<=i;k++){double ch=B[k].c-B[k-1].c; if(ch>0)g+=ch; else l+=-ch;} return l==0?100.0:100.0-100.0/(1.0+g/l);}
static double sma(const std::vector<Bar>&B,int i,int n){double s=0;for(int k=i-n+1;k<=i;k++)s+=B[k].c;return s/n;}
static Res run(const std::vector<Bar>&B,Cfg c){
 struct T{int64_t ts;double pnl;}; std::vector<T> trs;
 bool act=false; double entry=0; int64_t ets=0; int held=0; double units=0;
 int start=c.trend>c.short_sma?c.trend:c.short_sma; start+=3;
 for(int i=start;i<(int)B.size();++i){
   if(act){
     held++;
     bool exit=false;
     if(c.exit_mode==0){ if(held>=c.maxhold)exit=true; }
     else if(c.exit_mode==1){ if(rsi2(B,i)>=c.exit_rsi||held>=c.maxhold)exit=true; }
     else { if(B[i].c>sma(B,i,c.short_sma)||held>=c.maxhold)exit=true; }
     // scale-in: add a unit if still oversold + not yet doubled (Connors cumulative)
     if(!exit && c.scalein && units<2 && rsi2(B,i)<c.rsi_in && B[i].c>sma(B,i,c.trend)){
       entry=(entry*units+B[i].c)/(units+1); units+=1; // avg-in
     }
     if(exit){ double pnl=(B[i].c-entry)*units - c.cost*units; trs.push_back({ets,pnl}); act=false; }
   }
   if(!act){
     if(B[i].c>sma(B,i,c.trend) && rsi2(B,i)<c.rsi_in){ act=true; entry=B[i].c; ets=B[i].ts; held=0; units=1; }
   }
 }
 Res r{}; double gw=0,gl=0; int w=0; r.worst=0;
 for(auto&t:trs){r.net+=t.pnl; if(t.pnl>0){gw+=t.pnl;++w;}else gl+=-t.pnl; if(t.pnl<r.worst)r.worst=t.pnl; if(yr(t.ts)==2022)r.y22+=t.pnl;}
 r.n=trs.size(); r.pf=gl>0?gw/gl:(gw>0?99:0); r.wr=trs.empty()?0:100*w/trs.size();
 for(size_t i=0;i<trs.size();++i)(i<trs.size()/2?r.h1:r.h2)+=trs[i].pnl;
 return r;}
int main(int argc,char**argv){
 const char*path=argv[1]; std::string lbl=argv[2]; double cost=argc>3?atof(argv[3]):4.0;
 auto B=load(path); if(B.size()<260){fprintf(stderr,"few\n");return 1;}
 printf("=== %s ConnorsRSI2 enhancement sweep (cost=%.0fpt, 10yr) ===\n",lbl.c_str(),cost);
 printf("%-42s %4s %3s %6s %8s %7s %7s %6s\n","config","n","WR","PF","net","H1","H2","2022");
 struct N{const char*name;Cfg c;};
 N cfgs[]={
  {"BASE rsi10 hold1 sma200",        {10,0,0,5,200,1,false,cost}},
  {"rsi10 RSIexit>50 cap10",          {10,1,50,5,200,10,false,cost}},
  {"rsi10 RSIexit>70 cap10",          {10,1,70,5,200,10,false,cost}},
  {"rsi10 close>sma5 cap10",          {10,2,0,5,200,10,false,cost}},
  {"rsi5  RSIexit>50 cap10",          {5,1,50,5,200,10,false,cost}},
  {"rsi5  close>sma5 cap10",          {5,2,0,5,200,10,false,cost}},
  {"rsi15 RSIexit>50 cap10",          {15,1,50,5,200,10,false,cost}},
  {"rsi10 RSIexit>50 sma100 cap10",   {10,1,50,5,100,10,false,cost}},
  {"rsi10 RSIexit>50 SCALEIN cap10",  {10,1,50,5,200,10,true,cost}},
  {"rsi5  RSIexit>50 SCALEIN cap10",  {5,1,50,5,200,10,true,cost}},
 };
 for(auto&n:cfgs){Res r=run(B,n.c);
  printf("%-42s %4d %2d%% %6.2f %+8.0f %+7.0f %+7.0f %+6.0f %s\n",
   n.name,r.n,r.wr,r.pf,r.net,r.h1,r.h2,(double)r.y22,(r.h1>0&&r.h2>0)?"BOTH+":"");
 }
 return 0;}
