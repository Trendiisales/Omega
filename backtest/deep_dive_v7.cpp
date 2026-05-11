// =====================================================================
// backtest/deep_dive_v7.cpp -- Pass 7 (S33j 2026-05-11)
// ---------------------------------------------------------------------
// Quant-playbook strategies not yet tested:
//
//   T1. Connors RSI(2) mean reversion (Larry Connors)
//       Long when RSI(2) < 5, short when RSI(2) > 95.
//
//   T2. Turtle breakout (Faith/Dennis) N=20 and N=55
//       Donchian-style entry at N-bar high, exit on N/2-bar opposite low.
//       (Classic Turtle exit rule.)
//
//   T3. Chandelier exit (Le Beau): exit at HH-3*ATR (long) / LL+3*ATR (short)
//       Trails behind highest-high since entry, not just MFE.
//
//   T4. CCI extreme fade (Lambert) at +200/-200
//       CCI > +200 fade short, CCI < -200 fade long.
//
//   T5. Aroon trend strength filter (>=80 = strong trend)
//       Apply Aroon as a gate to Donchian: only fire on aligned Aroon dir.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/deep_dive_v7.cpp -o backtest/deep_dive_v7
// =====================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#if __has_include(<glob.h>)
# include <glob.h>
# define HAVE_GLOB 1
#endif

namespace u {
static std::string trim(std::string s){size_t a=0;while(a<s.size()&&std::isspace((unsigned char)s[a]))++a;size_t b=s.size();while(b>a&&std::isspace((unsigned char)s[b-1]))--b;return s.substr(a,b-a);}
static std::string lower(std::string s){for(auto&c:s)c=(char)std::tolower((unsigned char)c);return s;}
static std::vector<std::string> split(const std::string& l){std::vector<std::string> o;std::string c;for(char ch:l){if(ch==','){o.push_back(c);c.clear();}else c+=ch;}o.push_back(c);for(auto& f:o)f=trim(f);return o;}
static bool pd(const std::string& s,double& v){if(s.empty())return false;try{size_t p=0;v=std::stod(s,&p);return p>0;}catch(...){return false;}}
static bool pl(const std::string& s,long long& v){if(s.empty())return false;try{size_t p=0;v=std::stoll(s,&p);return p>0;}catch(...){return false;}}
static std::vector<std::string> glob_expand(const std::string& pat){std::vector<std::string> o;
#ifdef HAVE_GLOB
    glob_t g{}; if(glob(pat.c_str(),GLOB_TILDE,nullptr,&g)==0)for(size_t i=0;i<g.gl_pathc;++i)o.emplace_back(g.gl_pathv[i]);
    globfree(&g);
#endif
    if(o.empty())o.push_back(pat); return o;}
}

struct Tick{long long ts=0;double bid=0,ask=0;};
struct Bar{long long ts_open=0;int tf_sec=0;double bid_o=0,bid_h=0,bid_l=0,bid_c=0,ask_o=0,ask_h=0,ask_l=0,ask_c=0;
    double mid_o()const{return 0.5*(bid_o+ask_o);} double mid_c()const{return 0.5*(bid_c+ask_c);}
    double mid_h()const{return 0.5*(bid_h+ask_h);} double mid_l()const{return 0.5*(bid_l+ask_l);}};

static std::vector<Tick> load_ticks(const std::string& path){
    std::vector<Tick> out; std::ifstream fs(path); if(!fs)return out;
    std::string line; if(!std::getline(fs,line))return out;
    auto hdr=u::split(line); int c_ts=-1,c_bid=-1,c_ask=-1;bool ts_ms=false;
    bool first_num=!hdr.empty()&&!hdr[0].empty()&&(std::isdigit((unsigned char)hdr[0][0])||hdr[0][0]=='-');
    if(!first_num){for(size_t i=0;i<hdr.size();++i){std::string h=u::lower(hdr[i]);
        if(h=="ts_unix"||h=="ts"||h=="timestamp"||h=="time") c_ts=(int)i;
        if(h=="ts_ms"){c_ts=(int)i;ts_ms=true;} if(h=="bid") c_bid=(int)i; if(h=="ask") c_ask=(int)i;}}
    else { fs.clear();fs.seekg(0);c_ts=0;c_bid=1;c_ask=2;ts_ms=true;}
    if(c_ts<0||c_bid<0||c_ask<0)return out;
    while(std::getline(fs,line)){if(u::trim(line).empty()) continue; auto row=u::split(line);
        if((int)row.size()<=std::max({c_ts,c_bid,c_ask})) continue;
        Tick t;long long raw=0; if(!u::pl(row[c_ts],raw)) continue;
        t.ts=ts_ms?raw/1000:raw;
        if(!u::pd(row[c_bid],t.bid)||!u::pd(row[c_ask],t.ask)) continue;
        if(t.bid<=0||t.ask<=0) continue; out.push_back(t);}
    return out;
}
static std::vector<Bar> resample(const std::vector<Tick>& ts,int tf_sec){
    std::vector<Bar> out; if(ts.empty()||tf_sec<=0) return out;
    long long cur=(ts.front().ts/tf_sec)*tf_sec; Bar b; b.ts_open=cur;b.tf_sec=tf_sec; bool st=false;
    for(const auto& t:ts){long long buck=(t.ts/tf_sec)*tf_sec;
        if(buck!=cur){ if(st) out.push_back(b); cur=buck; b=Bar{}; b.ts_open=cur; b.tf_sec=tf_sec; st=false; }
        if(!st){b.bid_o=b.bid_h=b.bid_l=b.bid_c=t.bid; b.ask_o=b.ask_h=b.ask_l=b.ask_c=t.ask; st=true;}
        else{if(t.bid>b.bid_h)b.bid_h=t.bid; if(t.bid<b.bid_l)b.bid_l=t.bid; b.bid_c=t.bid;
             if(t.ask>b.ask_h)b.ask_h=t.ask; if(t.ask<b.ask_l)b.ask_l=t.ask; b.ask_c=t.ask;}}
    if(st) out.push_back(b); return out;
}
static std::vector<double> compute_atr(const std::vector<Bar>& bars,int n){
    std::vector<double> atr(bars.size(),0); if((int)bars.size()<=n) return atr;
    double s=0; for(int i=1;i<=n;++i){double cp=bars[i-1].mid_c();double tr=std::max(bars[i].mid_h()-bars[i].mid_l(),std::max(std::abs(bars[i].mid_h()-cp),std::abs(bars[i].mid_l()-cp)));s+=tr;}
    atr[n]=s/n; for(int i=n+1;i<(int)bars.size();++i){double cp=bars[i-1].mid_c();double tr=std::max(bars[i].mid_h()-bars[i].mid_l(),std::max(std::abs(bars[i].mid_h()-cp),std::abs(bars[i].mid_l()-cp)));atr[i]=(atr[i-1]*(n-1)+tr)/n;}
    return atr;
}
static std::vector<double> compute_sma(const std::vector<Bar>& bars,int n){
    std::vector<double> o(bars.size(),0); if((int)bars.size()<n)return o;
    double s=0;for(int i=0;i<n;++i)s+=bars[i].mid_c();
    o[n-1]=s/n; for(int i=n;i<(int)bars.size();++i){s+=bars[i].mid_c()-bars[i-n].mid_c();o[i]=s/n;}
    return o;
}

// Connors RSI(2) -- Wilder RSI with N=2
static std::vector<double> compute_rsi(const std::vector<Bar>& bars, int n){
    std::vector<double> rsi(bars.size(), 50.0);
    if((int)bars.size() <= n) return rsi;
    double g=0, l=0;
    for(int i=1;i<=n;++i){double d=bars[i].mid_c()-bars[i-1].mid_c(); if(d>0) g+=d; else l-=d;}
    g/=n; l/=n;
    for(int i=n;i<(int)bars.size();++i){
        if(i>n){double d=bars[i].mid_c()-bars[i-1].mid_c(); double gu=std::max(0.0,d),ld=std::max(0.0,-d); g=(g*(n-1)+gu)/n; l=(l*(n-1)+ld)/n;}
        if(l<1e-12) rsi[i]=100.0; else {double rs=g/l; rsi[i]=100.0-100.0/(1+rs);}
    }
    return rsi;
}

// CCI (Commodity Channel Index): (price - SMA) / (0.015 * mean deviation)
static std::vector<double> compute_cci(const std::vector<Bar>& bars, int n){
    std::vector<double> cci(bars.size(),0); if((int)bars.size()<n) return cci;
    std::vector<double> tp(bars.size(),0);
    for(int i=0;i<(int)bars.size();++i) tp[i] = (bars[i].mid_h()+bars[i].mid_l()+bars[i].mid_c())/3.0;
    for(int i=n-1;i<(int)bars.size();++i){
        double s=0; for(int k=i-n+1;k<=i;++k) s += tp[k]; double m = s/n;
        double md=0; for(int k=i-n+1;k<=i;++k) md += std::abs(tp[k]-m); md/=n;
        cci[i] = (md>1e-12) ? (tp[i]-m)/(0.015*md) : 0.0;
    }
    return cci;
}

// Aroon Up/Down: time since highest/lowest in last n bars (scaled 0-100)
struct Aroon { std::vector<double> up, dn; };
static Aroon compute_aroon(const std::vector<Bar>& bars, int n){
    Aroon a; a.up.assign(bars.size(),50); a.dn.assign(bars.size(),50);
    if((int)bars.size()<=n) return a;
    for(int i=n;i<(int)bars.size();++i){
        int idx_hi=i-n, idx_lo=i-n;
        for(int k=i-n+1;k<=i;++k){
            if(bars[k].mid_h() >= bars[idx_hi].mid_h()) idx_hi=k;
            if(bars[k].mid_l() <= bars[idx_lo].mid_l()) idx_lo=k;
        }
        a.up[i] = 100.0 * (n - (i - idx_hi)) / n;
        a.dn[i] = 100.0 * (n - (i - idx_lo)) / n;
    }
    return a;
}

struct Trade{long long entry_ts=0;int side=0;double pnl_gross=0;int bars=0;};
static const int MAX_HOLD_BARS=50;
static const double LOT_XAU=0.01, PT_XAU=0.01, COST=0.06;

// Fixed bracket
static Trade bracket_fixed(const std::vector<Bar>& bars,size_t i,int side,double atr,double sl_mult,double tp_mult){
    Trade r{}; if(i>=bars.size()||atr<=0) return r;
    double ep=(side>0)?bars[i].ask_c:bars[i].bid_c; if(ep<=0) return r;
    double sl_d=sl_mult*atr, tp_d=tp_mult*atr;
    double tp=side>0?ep+tp_d:ep-tp_d; double sl=side>0?ep-sl_d:ep+sl_d;
    r.entry_ts=bars[i].ts_open; r.side=side;
    size_t end=std::min(bars.size(),i+1+(size_t)MAX_HOLD_BARS); double xp=ep;
    for(size_t k=i+1;k<end;++k){const auto& b=bars[k];
        if(side>0){if(b.bid_l<=sl){xp=sl;r.bars=(int)(k-i);break;} if(b.bid_h>=tp){xp=tp;r.bars=(int)(k-i);break;} xp=b.bid_c;}
        else{if(b.ask_h>=sl){xp=sl;r.bars=(int)(k-i);break;} if(b.ask_l<=tp){xp=tp;r.bars=(int)(k-i);break;} xp=b.ask_c;}
    }
    if(r.bars==0) r.bars=(int)(end-i-1);
    double pnl_pts=side>0?(xp-ep):(ep-xp);
    r.pnl_gross=(pnl_pts/PT_XAU)*1.0*LOT_XAU;
    return r;
}

// Chandelier exit: SL = HH - K*ATR (long), or LL + K*ATR (short).
// Updated each bar based on highest-high (lowest-low) since entry.
static Trade bracket_chandelier(const std::vector<Bar>& bars,size_t i,int side,double atr,
                                double init_sl_mult, double chandelier_K){
    Trade r{}; if(i>=bars.size()||atr<=0) return r;
    double ep=(side>0)?bars[i].ask_c:bars[i].bid_c; if(ep<=0) return r;
    double sl = side>0 ? ep - init_sl_mult*atr : ep + init_sl_mult*atr;
    double hh = ep, ll = ep;
    r.entry_ts=bars[i].ts_open; r.side=side;
    size_t end=std::min(bars.size(),i+1+(size_t)MAX_HOLD_BARS); double xp=ep;
    for(size_t k=i+1;k<end;++k){const auto& b=bars[k];
        if(side>0){
            if(b.bid_h > hh) hh = b.bid_h;
            double new_sl = hh - chandelier_K * atr;
            if(new_sl > sl) sl = new_sl;
            if(b.bid_l <= sl){xp=sl; r.bars=(int)(k-i); break;}
            xp = b.bid_c;
        } else {
            if(b.ask_l < ll) ll = b.ask_l;
            double new_sl = ll + chandelier_K * atr;
            if(new_sl < sl) sl = new_sl;
            if(b.ask_h >= sl){xp=sl; r.bars=(int)(k-i); break;}
            xp = b.ask_c;
        }
    }
    if(r.bars==0) r.bars=(int)(end-i-1);
    double pnl_pts=side>0?(xp-ep):(ep-xp);
    r.pnl_gross=(pnl_pts/PT_XAU)*1.0*LOT_XAU;
    return r;
}

// Turtle exit: close on N/2-bar opposite extreme
static Trade bracket_turtle(const std::vector<Bar>& bars,size_t i,int side,double atr,
                            double init_sl_mult, int turtle_exit_n){
    Trade r{}; if(i>=bars.size()||atr<=0) return r;
    double ep=(side>0)?bars[i].ask_c:bars[i].bid_c; if(ep<=0) return r;
    double sl = side>0 ? ep - init_sl_mult*atr : ep + init_sl_mult*atr;
    r.entry_ts=bars[i].ts_open; r.side=side;
    size_t end=std::min(bars.size(),i+1+(size_t)MAX_HOLD_BARS); double xp=ep;
    for(size_t k=i+1;k<end;++k){const auto& b=bars[k];
        if(side>0){
            // Exit when bar.low < lowest low of previous turtle_exit_n bars
            if((int)k < turtle_exit_n) continue;
            double exit_lo = bars[k-turtle_exit_n].mid_l();
            for(int kk=(int)k-turtle_exit_n+1; kk<(int)k; ++kk) if(bars[kk].mid_l() < exit_lo) exit_lo = bars[kk].mid_l();
            if(b.bid_l <= sl){xp=sl; r.bars=(int)(k-i); break;}
            if(b.bid_l <= exit_lo){xp = exit_lo; r.bars=(int)(k-i); break;}
            xp = b.bid_c;
        } else {
            if((int)k < turtle_exit_n) continue;
            double exit_hi = bars[k-turtle_exit_n].mid_h();
            for(int kk=(int)k-turtle_exit_n+1; kk<(int)k; ++kk) if(bars[kk].mid_h() > exit_hi) exit_hi = bars[kk].mid_h();
            if(b.ask_h >= sl){xp=sl; r.bars=(int)(k-i); break;}
            if(b.ask_h >= exit_hi){xp = exit_hi; r.bars=(int)(k-i); break;}
            xp = b.ask_c;
        }
    }
    if(r.bars==0) r.bars=(int)(end-i-1);
    double pnl_pts=side>0?(xp-ep):(ep-xp);
    r.pnl_gross=(pnl_pts/PT_XAU)*1.0*LOT_XAU;
    return r;
}

// Signals
static int sig_donchian(const std::vector<Bar>& bars,int i,int N){
    if(i<N+1) return 0;
    double hi=bars[i-N].mid_h(),lo=bars[i-N].mid_l();
    for(int k=i-N+1;k<=i-1;++k){if(bars[k].mid_h()>hi)hi=bars[k].mid_h(); if(bars[k].mid_l()<lo)lo=bars[k].mid_l();}
    if(bars[i].ask_c>hi) return +1; if(bars[i].bid_c<lo) return -1; return 0;
}
// Connors RSI(2) mean rev: long when RSI<lo, short when RSI>hi
static int sig_connors_rsi2(const std::vector<Bar>& bars,const std::vector<double>& rsi2,int i,double lo,double hi){
    if(i<5) return 0;
    if(rsi2[i] < lo) return +1;
    if(rsi2[i] > hi) return -1;
    return 0;
}
// CCI fade at +200/-200
static int sig_cci_fade(const std::vector<double>& cci,int i,double thr){
    if(cci[i] < -thr) return +1;
    if(cci[i] >  thr) return -1;
    return 0;
}
// Donchian with Aroon trend agreement: only fire long if Aroon Up > Aroon Dn AND Up > 70
static int sig_donchian_aroon(const std::vector<Bar>& bars,const Aroon& a,int i,int N,double thr=70.0){
    int raw = sig_donchian(bars,i,N);
    if(raw == 0) return 0;
    if(raw > 0 && a.up[i] > a.dn[i] && a.up[i] >= thr) return +1;
    if(raw < 0 && a.dn[i] > a.up[i] && a.dn[i] >= thr) return -1;
    return 0;
}

template <typename SigFn, typename BracketFn>
static std::vector<Trade> run_signal(const std::vector<Bar>& bars,const std::vector<double>& atr,
                                     SigFn sig, BracketFn br, int warmup){
    std::vector<Trade> out; int cd=0;
    for(int i=warmup;i<(int)bars.size();++i){
        if(cd>0){--cd; continue;} if(atr[i]<=0) continue;
        int s=sig(i); if(s==0) continue;
        auto t=br(bars,(size_t)i,s,atr[i]); if(t.entry_ts==0) continue;
        out.push_back(t); cd=1+t.bars;
    }
    return out;
}

struct Sum{int n=0,wins=0;double gross=0;std::map<std::string,double> by_m;};
static std::string ym_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);char b[12];std::strftime(b,sizeof(b),"%Y-%m",&tm);return b;}
static Sum summarise(const std::vector<Trade>& ts){Sum s;for(const auto&t:ts){++s.n;s.gross+=t.pnl_gross;if(t.pnl_gross-COST>0)++s.wins;s.by_m[ym_of(t.entry_ts)]+=t.pnl_gross-COST;}return s;}
static int months_pos(const Sum& s){int p=0;for(auto& m:s.by_m)if(m.second>0)++p;return p;}

int main(int argc, char** argv){
    std::vector<std::string> xau_pat;
    std::string out_path;
    auto need=[&](int&i,const char*f)->const char*{if(i+1>=argc)std::exit(2);return argv[++i];};
    for(int i=1;i<argc;++i){std::string a=argv[i];
        if(a=="--xau") xau_pat.push_back(need(i,"--xau"));
        else if(a=="--out") out_path=need(i,"--out");
    }
    if(xau_pat.empty()){std::cerr<<"need --xau\n";return 2;}

    std::vector<Tick> xt;
    for(auto& p:xau_pat) for(auto& f:u::glob_expand(p)){auto v=load_ticks(f); xt.insert(xt.end(),v.begin(),v.end());}
    std::sort(xt.begin(),xt.end(),[](const Tick& a,const Tick& b){return a.ts<b.ts;});
    if(xt.empty()){std::cerr<<"no ticks\n";return 1;}
    std::cerr<<"[load] ticks="<<xt.size()<<"\n";

    auto bars_h4 = resample(xt, 14400);
    xt.clear(); xt.shrink_to_fit();
    auto atr_h4 = compute_atr(bars_h4,14);
    auto rsi2 = compute_rsi(bars_h4, 2);
    auto cci20 = compute_cci(bars_h4, 20);
    auto aroon25 = compute_aroon(bars_h4, 25);
    std::cerr<<"[bars] H4="<<bars_h4.size()<<"\n";

    std::ofstream out;
    if(!out_path.empty()){out.open(out_path); out<<"test,cell,n,wins,wr,gross,net,months_pos,be\n"; out<<std::fixed<<std::setprecision(4);}
    auto report=[&](const std::string& test, const std::string& cell, const Sum& s){
        if(s.n<5) return;
        double net=s.gross-COST*s.n;
        double be=s.n>0?s.gross/s.n:0;
        std::cout<<std::left<<std::setw(14)<<test<<std::setw(46)<<cell
                 <<std::right<<std::setw(5)<<s.n<<std::setw(7)<<(100.0*s.wins/std::max(1,s.n))
                 <<std::setw(10)<<s.gross<<std::setw(10)<<net
                 <<std::setw(5)<<s.by_m.size()<<std::setw(5)<<months_pos(s)
                 <<std::setw(8)<<be<<"\n";
        if(out)out<<test<<",\""<<cell<<"\","<<s.n<<","<<s.wins<<","<<(s.n?double(s.wins)/s.n:0)
                  <<","<<s.gross<<","<<net<<","<<months_pos(s)<<","<<be<<"\n";
    };

    std::cout<<std::fixed<<std::setprecision(2);
    std::cout<<"\n"<<std::left<<std::setw(14)<<"test"<<std::setw(46)<<"cell"
             <<std::right<<std::setw(5)<<"n"<<std::setw(7)<<"wr%"<<std::setw(10)<<"gross$"
             <<std::setw(10)<<"net$"<<std::setw(5)<<"Mtot"<<std::setw(5)<<"M+"<<std::setw(8)<<"BE$"<<"\n";

    // T1: Connors RSI(2) mean reversion -- try several thresholds & brackets
    std::cout<<"\n--- T1: CONNORS RSI(2) MEAN REVERSION (XAU 4h) ---\n";
    for(auto thr : std::vector<std::pair<double,double>>{{5,95}, {10,90}, {15,85}, {20,80}}){
        for(auto sl_tp : std::vector<std::pair<double,double>>{{1.0,2.0},{1.5,3.0},{1.5,2.0},{2.0,3.0}}){
            auto br = [&](const std::vector<Bar>& b,size_t i,int s,double a){return bracket_fixed(b,i,s,a,sl_tp.first,sl_tp.second);};
            std::ostringstream nm;
            nm<<"ConnorsRSI2_lo"<<thr.first<<"_hi"<<thr.second<<"_sl"<<sl_tp.first<<"tp"<<sl_tp.second;
            report("ConnorsRSI2", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&,thr](int i){return sig_connors_rsi2(bars_h4,rsi2,i,thr.first,thr.second);}, br, 10)));
        }
    }

    // T2: Turtle breakout system (N=20/55, exit at N/2-bar opposite)
    std::cout<<"\n--- T2: TURTLE BREAKOUT N=20/55 ---\n";
    for(int N : {20, 55}){
        int exit_n = N/2;
        for(double init_sl : {2.0, 3.0}){
            auto br = [&](const std::vector<Bar>& b,size_t i,int s,double a){return bracket_turtle(b,i,s,a,init_sl,exit_n);};
            std::ostringstream nm;
            nm<<"Turtle_N"<<N<<"_exitN"<<exit_n<<"_initsl"<<init_sl<<"ATR";
            report("Turtle", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&,N](int i){return sig_donchian(bars_h4,i,N);}, br, N+1)));
        }
    }

    // T3: Chandelier exit on top winners (Donchian/InsideBar/Keltner)
    std::cout<<"\n--- T3: CHANDELIER EXIT (K = 2.5 / 3.0 / 4.0) ---\n";
    for(double K : {2.5, 3.0, 4.0}){
        auto br = [&,K](const std::vector<Bar>& b,size_t i,int s,double a){return bracket_chandelier(b,i,s,a,1.5,K);};
        std::ostringstream nm;
        nm<<"Donchian_chandelier_K"<<K;
        report("Chandelier", nm.str(), summarise(run_signal(bars_h4, atr_h4,
            [&](int i){return sig_donchian(bars_h4,i,20);}, br, 22)));
    }

    // T4: CCI extreme fade at +200/-200 / +150/-150 / +250/-250
    std::cout<<"\n--- T4: CCI EXTREME FADE (+/-thr) ---\n";
    for(double thr : {100.0, 150.0, 200.0, 250.0}){
        for(auto sl_tp : std::vector<std::pair<double,double>>{{1.0,2.0},{1.5,3.0},{1.0,1.5}}){
            auto br = [&](const std::vector<Bar>& b,size_t i,int s,double a){return bracket_fixed(b,i,s,a,sl_tp.first,sl_tp.second);};
            std::ostringstream nm;
            nm<<"CCI_fade_thr"<<thr<<"_sl"<<sl_tp.first<<"tp"<<sl_tp.second;
            report("CCI_fade", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&,thr](int i){return sig_cci_fade(cci20,i,thr);}, br, 22)));
        }
    }

    // T5: Aroon trend filter on Donchian
    std::cout<<"\n--- T5: DONCHIAN GATED BY AROON >= thr ---\n";
    for(double aroon_thr : {60.0, 70.0, 80.0}){
        for(auto sl_tp : std::vector<std::pair<double,double>>{{1.5,3.0},{1.5,6.0},{2.0,4.0}}){
            auto br = [&](const std::vector<Bar>& b,size_t i,int s,double a){return bracket_fixed(b,i,s,a,sl_tp.first,sl_tp.second);};
            std::ostringstream nm;
            nm<<"Donch_aroon"<<aroon_thr<<"_sl"<<sl_tp.first<<"tp"<<sl_tp.second;
            report("Aroon", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&,aroon_thr](int i){return sig_donchian_aroon(bars_h4,aroon25,i,20,aroon_thr);}, br, 27)));
        }
    }

    return 0;
}
