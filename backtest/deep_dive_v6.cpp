// =====================================================================
// backtest/deep_dive_v6.cpp -- Pass 6 (S33h 2026-05-11)
// ---------------------------------------------------------------------
// Six more regime tests, focused on areas not yet covered:
//
//   1. SL-multiplier sweep: Pass-4 fixed SL=1.5, swept TP. Now sweep SL too.
//      Test SL ∈ {0.75, 1.0, 1.5, 2.0, 2.5, 3.0} × ATR for each winner cell.
//   2. Mean-reversion gated by ADX<20 (chop regime).  Bollinger/RSI/Stoch
//      cells were dead unfiltered; do they fire profitably ONLY in chop?
//   3. ATR-period variation: indicators using ATR(7), ATR(14), ATR(21), ATR(28).
//      14 is the default, but 7 (faster) or 21 (slower) may track price better.
//   4. Pyramid (scale-in): on initial Donchian fire, place 1st unit;
//      add a 2nd unit if a continuation signal fires while first is open.
//      Closes when SL or TP hit.  Tests if winning trades can be amplified.
//   5. Volatility-conditional R:R: in HIGH vol (ATR > 70th pct of last 100),
//      use TP=6*ATR (4:1 R:R); in LOW vol (ATR < 30th pct), use TP=3*ATR.
//      Tests if adaptive R:R captures runs in trending env without giving
//      back in chop.
//   6. Day-of-week analysis on Donchian winning trades.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/deep_dive_v6.cpp -o backtest/deep_dive_v6
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
static std::vector<double> compute_ema(const std::vector<Bar>& bars,int n){
    std::vector<double> o(bars.size(),0); if(bars.empty()) return o;
    double a=2.0/(n+1); o[0]=bars[0].mid_c();
    for(int i=1;i<(int)bars.size();++i) o[i]=a*bars[i].mid_c()+(1-a)*o[i-1];
    return o;
}
static std::vector<double> compute_kaufman_er(const std::vector<Bar>& bars,int n){
    std::vector<double> er(bars.size(),0); if((int)bars.size()<=n) return er;
    for(int i=n;i<(int)bars.size();++i){double num=std::abs(bars[i].mid_c()-bars[i-n].mid_c()); double den=0; for(int k=i-n+1;k<=i;++k)den+=std::abs(bars[k].mid_c()-bars[k-1].mid_c()); er[i]=(den>1e-12)?num/den:0.0;}
    return er;
}
static std::vector<double> compute_sma(const std::vector<Bar>& bars,int n){
    std::vector<double> o(bars.size(),0); if((int)bars.size()<n)return o;
    double s=0;for(int i=0;i<n;++i)s+=bars[i].mid_c();
    o[n-1]=s/n; for(int i=n;i<(int)bars.size();++i){s+=bars[i].mid_c()-bars[i-n].mid_c();o[i]=s/n;}
    return o;
}
// Wilder ADX
static std::vector<double> compute_adx(const std::vector<Bar>& bars,int n=14){
    std::vector<double> adx(bars.size(),0); if((int)bars.size()<=2*n) return adx;
    std::vector<double> pdm(bars.size(),0),mdm(bars.size(),0),tr(bars.size(),0);
    for(int i=1;i<(int)bars.size();++i){double up=bars[i].mid_h()-bars[i-1].mid_h(); double dn=bars[i-1].mid_l()-bars[i].mid_l();
        pdm[i]=(up>dn&&up>0)?up:0; mdm[i]=(dn>up&&dn>0)?dn:0;
        double cp=bars[i-1].mid_c();
        tr[i]=std::max(bars[i].mid_h()-bars[i].mid_l(),std::max(std::abs(bars[i].mid_h()-cp),std::abs(bars[i].mid_l()-cp)));}
    double atr_s=0,pdi_s=0,mdi_s=0;
    for(int i=1;i<=n;++i){atr_s+=tr[i];pdi_s+=pdm[i];mdi_s+=mdm[i];}
    std::vector<double> dx(bars.size(),0);
    if(atr_s>1e-12){double pdi=100*pdi_s/atr_s,mdi=100*mdi_s/atr_s; double sum=pdi+mdi; dx[n]=sum>1e-12?100*std::abs(pdi-mdi)/sum:0;}
    for(int i=n+1;i<(int)bars.size();++i){atr_s=atr_s-atr_s/n+tr[i];pdi_s=pdi_s-pdi_s/n+pdm[i];mdi_s=mdi_s-mdi_s/n+mdm[i];
        double pdi=atr_s>1e-12?100*pdi_s/atr_s:0; double mdi=atr_s>1e-12?100*mdi_s/atr_s:0; double sum=pdi+mdi;
        dx[i]=sum>1e-12?100*std::abs(pdi-mdi)/sum:0;}
    double sum_dx=0;for(int i=n+1;i<=2*n;++i) sum_dx+=dx[i];
    if(2*n<(int)bars.size()) adx[2*n]=sum_dx/n;
    for(int i=2*n+1;i<(int)bars.size();++i) adx[i]=(adx[i-1]*(n-1)+dx[i])/n;
    return adx;
}
// Bollinger bands at SMA(W) ± k*sd
struct BB { std::vector<double> mid, up, lo; };
static BB compute_bb(const std::vector<Bar>& bars, int W, double k){
    BB b; b.mid.assign(bars.size(),0); b.up.assign(bars.size(),0); b.lo.assign(bars.size(),0);
    if((int)bars.size()<W) return b;
    double s=0,s2=0; for(int i=0;i<W;++i){s+=bars[i].mid_c(); s2+=bars[i].mid_c()*bars[i].mid_c();}
    for(int i=W-1;i<(int)bars.size();++i){
        double mean=s/W; double var=std::max(0.0,s2/W-mean*mean); double sd=std::sqrt(var);
        b.mid[i]=mean; b.up[i]=mean+k*sd; b.lo[i]=mean-k*sd;
        if(i+1<(int)bars.size()){s+=bars[i+1].mid_c()-bars[i+1-W].mid_c(); s2+=bars[i+1].mid_c()*bars[i+1].mid_c()-bars[i+1-W].mid_c()*bars[i+1-W].mid_c();}
    }
    return b;
}

struct Trade{long long entry_ts=0;int side=0;double pnl_gross=0;int bars=0;};
static const int MAX_HOLD_BARS=50;
static const double LOT_XAU=0.01, PT_XAU=0.01;
static const double COST=0.06;

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

// Signal evaluators
static int sig_donchian(const std::vector<Bar>& bars,int i,int N){
    if(i<N+1) return 0;
    double hi=bars[i-N].mid_h(),lo=bars[i-N].mid_l();
    for(int k=i-N+1;k<=i-1;++k){if(bars[k].mid_h()>hi)hi=bars[k].mid_h(); if(bars[k].mid_l()<lo)lo=bars[k].mid_l();}
    if(bars[i].ask_c>hi) return +1; if(bars[i].bid_c<lo) return -1; return 0;
}
static int sig_inside_bar(const std::vector<Bar>& bars,int i){
    if(i<3) return 0;
    const auto& a=bars[i-2]; const auto& b=bars[i-1]; const auto& c=bars[i];
    if(!(b.mid_h()<a.mid_h() && b.mid_l()>a.mid_l())) return 0;
    if(c.ask_c>b.mid_h()) return +1; if(c.bid_c<b.mid_l()) return -1; return 0;
}
static int sig_momentum(const std::vector<Bar>& bars,int i,int N){
    if(i<N+2) return 0;
    if(bars[i].mid_c()>bars[i-N].mid_c()*1.001) return +1;
    if(bars[i].mid_c()<bars[i-N].mid_c()*0.999) return -1; return 0;
}
static int sig_keltner(const std::vector<Bar>& bars,const std::vector<double>& ema,const std::vector<double>& atr,int i){
    if(i<22||atr[i]<=0) return 0;
    double up=ema[i]+2.0*atr[i], lo=ema[i]-2.0*atr[i];
    if(bars[i].ask_c>up) return +1; if(bars[i].bid_c<lo) return -1; return 0;
}
static int sig_er_trend(const std::vector<Bar>& bars,const std::vector<double>& er,int i,double thr=0.20,int N=20){
    if(i<N+2) return 0; if(er[i]<thr) return 0;
    if(bars[i].mid_c()>bars[i-N].mid_c()) return +1;
    if(bars[i].mid_c()<bars[i-N].mid_c()) return -1; return 0;
}
static int sig_range_expansion(const std::vector<Bar>& bars,const std::vector<double>& atr,int i,double K){
    if(i<20||atr[i]<=0) return 0;
    double tr=bars[i].mid_h()-bars[i].mid_l();
    if(tr<K*atr[i]) return 0;
    if(bars[i].mid_c()>bars[i].mid_o()) return +1;
    if(bars[i].mid_c()<bars[i].mid_o()) return -1;
    return 0;
}
// Mean-rev signals
static int sig_bb_revert(const std::vector<Bar>& bars,const BB& bb,int i){
    if(i<22) return 0;
    // Long when close pierces lower band, short upper
    if(bars[i].ask_c < bb.lo[i]) return +1;
    if(bars[i].bid_c > bb.up[i]) return -1;
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
static int dow_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);return tm.tm_wday;}
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
    auto atr_h4_14 = compute_atr(bars_h4,14);
    auto atr_h4_7  = compute_atr(bars_h4,7);
    auto atr_h4_21 = compute_atr(bars_h4,21);
    auto atr_h4_28 = compute_atr(bars_h4,28);
    auto ema_h4 = compute_ema(bars_h4,20);
    auto er_h4  = compute_kaufman_er(bars_h4,20);
    auto adx_h4 = compute_adx(bars_h4,14);
    auto bb_h4_20_2 = compute_bb(bars_h4,20,2.0);
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

    // TEST 1: SL-multiplier sweep on top cells (TP held at 6*ATR for fair comparison vs current best)
    std::cout<<"\n--- TEST 1: SL SWEEP (XAU 4h, TP=6*ATR) ---\n";
    for(double sl : {0.75, 1.0, 1.5, 2.0, 2.5}){
        auto br = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,sl,6.0);};
        std::ostringstream nm;
        nm<<"Donchian_sl"<<sl<<"_tp6";
        report("SL_sweep", nm.str(), summarise(run_signal(bars_h4,atr_h4_14,[&](int i){return sig_donchian(bars_h4,i,20);},br,22)));
        nm.str(""); nm<<"InsideBar_sl"<<sl<<"_tp6";
        report("SL_sweep", nm.str(), summarise(run_signal(bars_h4,atr_h4_14,[&](int i){return sig_inside_bar(bars_h4,i);},br,16)));
        nm.str(""); nm<<"ER020_sl"<<sl<<"_tp6";
        report("SL_sweep", nm.str(), summarise(run_signal(bars_h4,atr_h4_14,[&](int i){return sig_er_trend(bars_h4,er_h4,i,0.20,20);},br,22)));
        nm.str(""); nm<<"Keltner_sl"<<sl<<"_tp6";
        report("SL_sweep", nm.str(), summarise(run_signal(bars_h4,atr_h4_14,[&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4_14,i);},br,22)));
        nm.str(""); nm<<"RangeExp_sl"<<sl<<"_tp6";
        report("SL_sweep", nm.str(), summarise(run_signal(bars_h4,atr_h4_14,[&](int i){return sig_range_expansion(bars_h4,atr_h4_14,i,1.5);},br,22)));
    }

    // TEST 2: Mean-reversion gated by ADX < 20 (chop regime)
    std::cout<<"\n--- TEST 2: BB MEAN-REV GATED BY ADX<20 ---\n";
    auto gated_chop = [&](auto raw_sig){
        std::vector<Trade> out; int cd=0;
        auto br = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,1.0,2.0);};
        for(int i=22;i<(int)bars_h4.size();++i){
            if(cd>0){--cd; continue;}
            if(atr_h4_14[i]<=0) continue;
            if(adx_h4[i] >= 20.0) continue;  // require chop
            int s=raw_sig(i); if(s==0) continue;
            auto t=br(bars_h4,(size_t)i,s,atr_h4_14[i]); if(t.entry_ts==0) continue;
            out.push_back(t); cd=1+t.bars;
        }
        return out;
    };
    report("MR_chop", "BB_revert_chopOnly_sl1tp2",
        summarise(gated_chop([&](int i){return sig_bb_revert(bars_h4,bb_h4_20_2,i);})));

    // TEST 3: ATR period variation on existing winners (all sl1.5 tp6.0)
    std::cout<<"\n--- TEST 3: ATR PERIOD VARIATION (sl1.5_tp6.0) ---\n";
    struct AP { int n; const std::vector<double>* atr; const char* lbl; };
    AP aps[] = { {7, &atr_h4_7, "ATR7"}, {14, &atr_h4_14, "ATR14"}, {21, &atr_h4_21, "ATR21"}, {28, &atr_h4_28, "ATR28"} };
    for(auto& ap : aps){
        auto br = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,1.5,6.0);};
        // Use ap.atr for both gating & bracket
        const auto& ATR = *ap.atr;
        auto run_with_atr = [&](auto sig, int warmup){
            std::vector<Trade> out; int cd=0;
            for(int i=warmup;i<(int)bars_h4.size();++i){
                if(cd>0){--cd; continue;} if(ATR[i]<=0) continue;
                int s=sig(i); if(s==0) continue;
                auto t=br(bars_h4,(size_t)i,s,ATR[i]); if(t.entry_ts==0) continue;
                out.push_back(t); cd=1+t.bars;
            }
            return out;
        };
        std::ostringstream nm;
        nm<<"Donchian_"<<ap.lbl;
        report("ATR_period", nm.str(), summarise(run_with_atr([&](int i){return sig_donchian(bars_h4,i,20);}, 22)));
        nm.str(""); nm<<"InsideBar_"<<ap.lbl;
        report("ATR_period", nm.str(), summarise(run_with_atr([&](int i){return sig_inside_bar(bars_h4,i);}, 16)));
        nm.str(""); nm<<"Keltner_"<<ap.lbl;
        report("ATR_period", nm.str(), summarise(run_with_atr([&](int i){return sig_keltner(bars_h4,ema_h4,ATR,i);}, 22)));
        nm.str(""); nm<<"RangeExp_"<<ap.lbl;
        report("ATR_period", nm.str(), summarise(run_with_atr([&](int i){return sig_range_expansion(bars_h4,ATR,i,1.5);}, 22)));
    }

    // TEST 5: Vol-conditional R:R: high vol -> tp6; low vol -> tp3
    std::cout<<"\n--- TEST 5: VOL-CONDITIONAL R:R (high vol -> wider TP) ---\n";
    auto vol_conditional = [&](auto raw_sig){
        std::vector<Trade> out; int cd=0;
        for(int i=120;i<(int)bars_h4.size();++i){
            if(cd>0){--cd; continue;}
            if(atr_h4_14[i]<=0) continue;
            int s=raw_sig(i); if(s==0) continue;
            // Rank current ATR vs last 100 bars
            std::vector<double> recent; recent.reserve(100);
            for(int k=i-99;k<=i;++k) if(atr_h4_14[k]>0) recent.push_back(atr_h4_14[k]);
            if(recent.size()<50){cd=0; continue;}
            std::sort(recent.begin(),recent.end());
            int rank = 0; for(double v : recent) if(v < atr_h4_14[i]) ++rank;
            double pct = double(rank)/recent.size();
            double tp = (pct >= 0.7) ? 6.0 : (pct <= 0.3 ? 3.0 : 4.5);
            auto br = [&](const std::vector<Bar>& bb,size_t i2,int s2,double a2){return bracket_fixed(bb,i2,s2,a2,1.5,tp);};
            auto t = br(bars_h4,(size_t)i,s,atr_h4_14[i]); if(t.entry_ts==0) continue;
            out.push_back(t); cd=1+t.bars;
        }
        return out;
    };
    report("VolCondRR", "Donchian_VC", summarise(vol_conditional([&](int i){return sig_donchian(bars_h4,i,20);})));
    report("VolCondRR", "InsideBar_VC", summarise(vol_conditional([&](int i){return sig_inside_bar(bars_h4,i);})));
    report("VolCondRR", "Keltner_VC", summarise(vol_conditional([&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4_14,i);})));

    // TEST 6: Day-of-week analysis on Donchian 4h winning trades (sl1.5tp3)
    std::cout<<"\n--- TEST 6: DAY-OF-WEEK BREAKDOWN (Donchian 4h sl1.5tp3) ---\n";
    auto donch = run_signal(bars_h4,atr_h4_14,[&](int i){return sig_donchian(bars_h4,i,20);},
                            [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,1.5,3.0);},22);
    std::map<int, Sum> by_dow;
    const char* dow_lbl[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    for(const auto& t : donch){
        int d = dow_of(t.entry_ts);
        auto& s = by_dow[d];
        ++s.n; s.gross += t.pnl_gross; if(t.pnl_gross-COST>0) ++s.wins;
        s.by_m[ym_of(t.entry_ts)] += t.pnl_gross-COST;
    }
    for(auto& kv : by_dow){
        std::ostringstream nm; nm<<"Donch_dow_"<<dow_lbl[kv.first];
        report("DayOfWeek", nm.str(), kv.second);
    }

    return 0;
}
