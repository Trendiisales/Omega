// =====================================================================
// backtest/deep_dive_v5.cpp -- Pass 5 (S33g 2026-05-11)
// ---------------------------------------------------------------------
// Five more regime tests on the merged 30-month corpus:
//
//   1. R:R asymmetry on D1 cells (same approach as Pass 4 but on daily)
//   2. Multi-TF confluence: 4h Donchian/InsideBar/etc fire only when
//      D1 close > D1 EMA20 (long) or < (short)
//   3. Session decomposition: which UTC hour-of-bar drives the XAU 4h
//      winning trades?  Bar starts are at 0/4/8/12/16/20 UTC.
//   4. W1 with wider bracket (SL=2.0*ATR_W1, TP up to 8.0*ATR)
//   5. Range expansion entries: bars whose TR > 2*ATR, trade direction
//      of the bar (continuation)
//   6. Counter-trend at extremes: fade after price moves >2.5*ATR from
//      EMA20 in one bar (mean reversion entry)
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/deep_dive_v5.cpp -o backtest/deep_dive_v5
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

// Aggregate H4 bars into D1 by UTC date
static std::vector<Bar> aggregate_to_d1(const std::vector<Bar>& h4){
    std::vector<Bar> d1;
    long long last_date = -1; Bar cur{}; bool st = false;
    for(const auto& b : h4){
        std::time_t tt=(std::time_t)b.ts_open; std::tm tm{}; gmtime_r(&tt,&tm);
        long long date=tm.tm_year*10000LL+tm.tm_mon*100LL+tm.tm_mday;
        if(date != last_date){
            if(st) d1.push_back(cur);
            cur = b; st = true; last_date = date;
        } else {
            if(b.bid_h>cur.bid_h)cur.bid_h=b.bid_h; if(b.bid_l<cur.bid_l)cur.bid_l=b.bid_l; cur.bid_c=b.bid_c;
            if(b.ask_h>cur.ask_h)cur.ask_h=b.ask_h; if(b.ask_l<cur.ask_l)cur.ask_l=b.ask_l; cur.ask_c=b.ask_c;
        }
    }
    if(st) d1.push_back(cur);
    return d1;
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

struct Trade{long long entry_ts=0;int side=0;double pnl_gross=0;int bars=0;};
static const int MAX_HOLD_BARS=50;
static const double LOT_XAU=0.01, PT_XAU=0.01;
static const double COST = 0.06;

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

// Signals
static int sig_donchian(const std::vector<Bar>& bars,int i,int N=20){
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
    if(bars[i].mid_c() > bars[i-N].mid_c()*1.001) return +1;
    if(bars[i].mid_c() < bars[i-N].mid_c()*0.999) return -1; return 0;
}
static int sig_keltner(const std::vector<Bar>& bars,const std::vector<double>& ema,const std::vector<double>& atr,int i){
    if(i<22||atr[i]<=0) return 0;
    double up=ema[i]+2.0*atr[i], lo=ema[i]-2.0*atr[i];
    if(bars[i].ask_c>up) return +1; if(bars[i].bid_c<lo) return -1; return 0;
}
// Range-expansion bar entry: TR > K*ATR, direction = sign of (close-open)
static int sig_range_expansion(const std::vector<Bar>& bars,const std::vector<double>& atr,int i,double K){
    if(i<20||atr[i]<=0) return 0;
    double tr = bars[i].mid_h() - bars[i].mid_l();
    if(tr < K * atr[i]) return 0;
    if(bars[i].mid_c() > bars[i].mid_o()) return +1;
    if(bars[i].mid_c() < bars[i].mid_o()) return -1;
    return 0;
}
// Counter-trend extreme: close is > Z*ATR away from EMA20.  Fade.
static int sig_extreme_fade(const std::vector<Bar>& bars,const std::vector<double>& ema,const std::vector<double>& atr,int i,double Z){
    if(i<22||atr[i]<=0) return 0;
    double dist = bars[i].mid_c() - ema[i];
    if(dist >  Z*atr[i]) return -1;
    if(dist < -Z*atr[i]) return +1;
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
static int hour_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);return tm.tm_hour;}
static Sum summarise(const std::vector<Trade>& ts){Sum s; for(const auto&t:ts){++s.n;s.gross+=t.pnl_gross;if(t.pnl_gross-COST>0)++s.wins; s.by_m[ym_of(t.entry_ts)]+=t.pnl_gross-COST;} return s;}
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
    auto bars_w1 = resample(xt, 7*86400);
    auto bars_d1 = aggregate_to_d1(bars_h4);
    xt.clear(); xt.shrink_to_fit();
    auto atr_h4 = compute_atr(bars_h4,14);
    auto atr_d1 = compute_atr(bars_d1,14);
    auto atr_w1 = compute_atr(bars_w1,14);
    auto ema_h4 = compute_ema(bars_h4,20);
    auto ema_d1 = compute_ema(bars_d1,20);
    auto ema_w1 = compute_ema(bars_w1,20);
    std::cerr<<"[bars] H4="<<bars_h4.size()<<" D1="<<bars_d1.size()<<" W1="<<bars_w1.size()<<"\n";

    std::ofstream out;
    if(!out_path.empty()){out.open(out_path); out<<"test,cell,n,wins,wr,gross,net,months_pos,be\n"; out<<std::fixed<<std::setprecision(4);}
    auto report=[&](const std::string& test, const std::string& cell, const Sum& s){
        if(s.n<5) return;
        double net=s.gross-COST*s.n;
        double be=s.n>0?s.gross/s.n:0;
        std::cout<<std::left<<std::setw(14)<<test<<std::setw(40)<<cell
                 <<std::right<<std::setw(5)<<s.n<<std::setw(7)<<(100.0*s.wins/std::max(1,s.n))
                 <<std::setw(10)<<s.gross<<std::setw(10)<<net
                 <<std::setw(5)<<s.by_m.size()<<std::setw(5)<<months_pos(s)
                 <<std::setw(8)<<be<<"\n";
        if(out)out<<test<<",\""<<cell<<"\","<<s.n<<","<<s.wins<<","<<(s.n?double(s.wins)/s.n:0)
                  <<","<<s.gross<<","<<net<<","<<months_pos(s)<<","<<be<<"\n";
    };

    std::cout<<std::fixed<<std::setprecision(2);
    std::cout<<"\n"<<std::left<<std::setw(14)<<"test"<<std::setw(40)<<"cell"
             <<std::right<<std::setw(5)<<"n"<<std::setw(7)<<"wr%"<<std::setw(10)<<"gross$"
             <<std::setw(10)<<"net$"<<std::setw(5)<<"Mtot"<<std::setw(5)<<"M+"<<std::setw(8)<<"BE$"<<"\n";

    // TEST 1: D1 R:R sweep
    std::cout<<"\n--- TEST 1: D1 R:R SWEEP (SL=2.0*ATR) ---\n";
    for(double tp : {2.0, 4.0, 6.0, 8.0, 10.0}){
        auto br = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,2.0,tp);};
        std::ostringstream nm; nm<<"D1_Momentum_lb20_tp"<<tp;
        report("D1_RR", nm.str(), summarise(run_signal(bars_d1, atr_d1,
            [&](int i){return sig_momentum(bars_d1,i,20);}, br, 22)));
        nm.str(""); nm<<"D1_Keltner_K2_tp"<<tp;
        report("D1_RR", nm.str(), summarise(run_signal(bars_d1, atr_d1,
            [&](int i){return sig_keltner(bars_d1,ema_d1,atr_d1,i);}, br, 22)));
        nm.str(""); nm<<"D1_Donchian_N10_tp"<<tp;
        report("D1_RR", nm.str(), summarise(run_signal(bars_d1, atr_d1,
            [&](int i){return sig_donchian(bars_d1,i,10);}, br, 12)));
    }

    // TEST 2: Multi-TF confluence (4h signal AND D1 EMA20 slope agrees)
    std::cout<<"\n--- TEST 2: 4h GATED BY D1 EMA20 SLOPE AGREEMENT ---\n";
    // build per-H4-bar D1 EMA slope sign
    std::vector<int> d1_slope_sign(bars_h4.size(),0);
    {
        size_t di = 0;
        for(size_t i=0;i<bars_h4.size();++i){
            std::time_t tt=(std::time_t)bars_h4[i].ts_open; std::tm tm{}; gmtime_r(&tt,&tm);
            long long date=tm.tm_year*10000LL+tm.tm_mon*100LL+tm.tm_mday;
            while(di+1 < bars_d1.size()){
                std::time_t dt=(std::time_t)bars_d1[di+1].ts_open; std::tm dtm{}; gmtime_r(&dt,&dtm);
                long long ddate=dtm.tm_year*10000LL+dtm.tm_mon*100LL+dtm.tm_mday;
                if(ddate > date) break;
                ++di;
            }
            if(di >= 1){
                double s = ema_d1[di] - ema_d1[di-1];
                d1_slope_sign[i] = s>0?+1:(s<0?-1:0);
            }
        }
    }
    auto br_44 = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,1.5,6.0);};  // 4:1 R:R
    auto gated_4h = [&](auto raw_sig){
        std::vector<Trade> out; int cd=0;
        for(int i=22;i<(int)bars_h4.size();++i){
            if(cd>0){--cd; continue;} if(atr_h4[i]<=0) continue;
            int s=raw_sig(i); if(s==0) continue;
            int ds=d1_slope_sign[i]; if(ds==0||s!=ds) continue;
            auto t=br_44(bars_h4,(size_t)i,s,atr_h4[i]); if(t.entry_ts==0) continue;
            out.push_back(t); cd=1+t.bars;
        }
        return out;
    };
    report("MTF_4h", "Donchian_4h_d1agree_RR4to1", summarise(gated_4h([&](int i){return sig_donchian(bars_h4,i,20);})));
    report("MTF_4h", "InsideBar_4h_d1agree_RR4to1", summarise(gated_4h([&](int i){return sig_inside_bar(bars_h4,i);})));
    report("MTF_4h", "Momentum_4h_d1agree_RR4to1", summarise(gated_4h([&](int i){return sig_momentum(bars_h4,i,20);})));
    report("MTF_4h", "Keltner_4h_d1agree_RR4to1", summarise(gated_4h([&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4,i);})));

    // TEST 3: Session decomposition for XAU 4h winners
    std::cout<<"\n--- TEST 3: XAU 4h SESSION DECOMPOSITION (Donchian R:R=4:1) ---\n";
    // Run Donchian at sl1.5tp6.0, then bucket trades by entry hour-of-bar
    auto br_4to1 = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,1.5,6.0);};
    auto donch_trades = run_signal(bars_h4, atr_h4, [&](int i){return sig_donchian(bars_h4,i,20);}, br_4to1, 22);
    std::map<int, Sum> by_hour;
    for(const auto& t : donch_trades){
        int h = hour_of(t.entry_ts);
        auto& s = by_hour[h];
        ++s.n; s.gross += t.pnl_gross; if(t.pnl_gross-COST>0) ++s.wins;
        s.by_m[ym_of(t.entry_ts)] += t.pnl_gross-COST;
    }
    for(auto& kv : by_hour){
        std::ostringstream nm; nm<<"Donch_4h_RR4to1_hour"<<kv.first<<"UTC";
        report("Session", nm.str(), kv.second);
    }

    // TEST 4: W1 with wider brackets
    std::cout<<"\n--- TEST 4: W1 (weekly) WITH WIDE BRACKETS ---\n";
    for(double sl : {1.5, 2.0, 3.0}){
        for(double tp : {3.0, 5.0, 8.0}){
            auto br_w = [&,sl,tp](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,sl,tp);};
            std::ostringstream nm; nm<<"W1_Momentum_lb10_sl"<<sl<<"_tp"<<tp;
            report("W1", nm.str(), summarise(run_signal(bars_w1, atr_w1,
                [&](int i){return sig_momentum(bars_w1,i,10);}, br_w, 12)));
            nm.str(""); nm<<"W1_Keltner_sl"<<sl<<"_tp"<<tp;
            report("W1", nm.str(), summarise(run_signal(bars_w1, atr_w1,
                [&](int i){return sig_keltner(bars_w1,ema_w1,atr_w1,i);}, br_w, 22)));
        }
    }

    // TEST 5: Range-expansion entries (4h)
    std::cout<<"\n--- TEST 5: RANGE-EXPANSION ENTRIES (4h, K*ATR) ---\n";
    for(double K : {1.5, 2.0, 2.5, 3.0}){
        for(auto sl_tp : std::vector<std::pair<double,double>>{{1.5,3.0},{1.5,6.0},{2.0,4.0}}){
            auto br_re = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,sl_tp.first,sl_tp.second);};
            std::ostringstream nm; nm<<"RangeExpand_K"<<K<<"_sl"<<sl_tp.first<<"tp"<<sl_tp.second;
            report("RangeExp", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&](int i){return sig_range_expansion(bars_h4,atr_h4,i,K);}, br_re, 22)));
        }
    }

    // TEST 6: Counter-trend fade at extremes (4h)
    std::cout<<"\n--- TEST 6: COUNTER-TREND FADE AT EXTREMES (4h, Z*ATR from EMA20) ---\n";
    for(double Z : {2.0, 2.5, 3.0}){
        for(auto sl_tp : std::vector<std::pair<double,double>>{{1.5,3.0},{1.0,2.0},{2.0,3.0}}){
            auto br_ef = [&](const std::vector<Bar>& bb,size_t i,int s,double a){return bracket_fixed(bb,i,s,a,sl_tp.first,sl_tp.second);};
            std::ostringstream nm; nm<<"ExtremeFade_Z"<<Z<<"_sl"<<sl_tp.first<<"tp"<<sl_tp.second;
            report("ExtremeFade", nm.str(), summarise(run_signal(bars_h4, atr_h4,
                [&](int i){return sig_extreme_fade(bars_h4,ema_h4,atr_h4,i,Z);}, br_ef, 22)));
        }
    }

    return 0;
}
