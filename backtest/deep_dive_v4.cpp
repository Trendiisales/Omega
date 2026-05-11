// =====================================================================
// backtest/deep_dive_v4.cpp -- Pass 4 (S33f 2026-05-11)
// ---------------------------------------------------------------------
// Four tests on the proven winners:
//   1. R:R asymmetry sweep — TP multipliers {2, 3, 4, 5, 6} × SL=1.5*ATR
//   2. Trailing-stop variants — trail at {1.0, 1.5, 2.0} * ATR after BE
//   3. W1 (weekly) timeframe — full signal-family sweep
//   4. Cross-asset filter — XAU 4h gated by US500 4h direction (sign of
//      US500 EMA20 slope)
//
// Realistic bid/ask fills, $0.06/RT cost, ATR-scaled brackets.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/deep_dive_v4.cpp -o backtest/deep_dive_v4
// =====================================================================

#include <algorithm>
#include <array>
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
struct Bar{
    long long ts_open=0;int tf_sec=0;
    double bid_o=0,bid_h=0,bid_l=0,bid_c=0;
    double ask_o=0,ask_h=0,ask_l=0,ask_c=0;
    double mid_o()const{return 0.5*(bid_o+ask_o);}
    double mid_c()const{return 0.5*(bid_c+ask_c);}
    double mid_h()const{return 0.5*(bid_h+ask_h);}
    double mid_l()const{return 0.5*(bid_l+ask_l);}
};

static std::vector<Tick> load_ticks(const std::string& path){
    std::vector<Tick> out; std::ifstream fs(path); if(!fs)return out;
    std::string line; if(!std::getline(fs,line))return out;
    auto hdr=u::split(line); int c_ts=-1,c_bid=-1,c_ask=-1;bool ts_ms=false;
    bool first_num=!hdr.empty()&&!hdr[0].empty()&&(std::isdigit((unsigned char)hdr[0][0])||hdr[0][0]=='-');
    if(!first_num){
        for(size_t i=0;i<hdr.size();++i){std::string h=u::lower(hdr[i]);
            if(h=="ts_unix"||h=="ts"||h=="timestamp"||h=="time") c_ts=(int)i;
            if(h=="ts_ms"){c_ts=(int)i;ts_ms=true;}
            if(h=="bid") c_bid=(int)i; if(h=="ask") c_ask=(int)i;}
    } else { fs.clear();fs.seekg(0);c_ts=0;c_bid=1;c_ask=2;ts_ms=true;}
    if(c_ts<0||c_bid<0||c_ask<0)return out;
    while(std::getline(fs,line)){
        if(u::trim(line).empty()) continue; auto row=u::split(line);
        if((int)row.size()<=std::max({c_ts,c_bid,c_ask})) continue;
        Tick t;long long raw=0;
        if(!u::pl(row[c_ts],raw)) continue;
        t.ts=ts_ms?raw/1000:raw;
        if(!u::pd(row[c_bid],t.bid)||!u::pd(row[c_ask],t.ask)) continue;
        if(t.bid<=0||t.ask<=0) continue;
        out.push_back(t);
    }
    return out;
}

static std::vector<Bar> resample(const std::vector<Tick>& ts,int tf_sec){
    std::vector<Bar> out; if(ts.empty()||tf_sec<=0) return out;
    long long cur=(ts.front().ts/tf_sec)*tf_sec; Bar b; b.ts_open=cur;b.tf_sec=tf_sec; bool st=false;
    for(const auto& t:ts){
        long long buck=(t.ts/tf_sec)*tf_sec;
        if(buck!=cur){ if(st) out.push_back(b); cur=buck; b=Bar{}; b.ts_open=cur; b.tf_sec=tf_sec; st=false; }
        if(!st){b.bid_o=b.bid_h=b.bid_l=b.bid_c=t.bid; b.ask_o=b.ask_h=b.ask_l=b.ask_c=t.ask; st=true;}
        else{if(t.bid>b.bid_h)b.bid_h=t.bid; if(t.bid<b.bid_l)b.bid_l=t.bid; b.bid_c=t.bid;
             if(t.ask>b.ask_h)b.ask_h=t.ask; if(t.ask<b.ask_l)b.ask_l=t.ask; b.ask_c=t.ask;}
    }
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

// ---- Standard fixed bracket (TP/SL ATR mult) ----
struct Trade{long long entry_ts=0;int side=0;double pnl_gross=0;int bars=0;};
static const int MAX_HOLD_BARS=50;
static const double LOT_XAU=0.01, PT_XAU=0.01;

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

// ---- Trailing-stop bracket ----
// Initial SL at sl_mult*ATR. Once MFE >= trail_arm_mult * ATR, SL trails
// behind at trail_mult * ATR. No fixed TP -- ride trend until trailed out.
static Trade bracket_trailing(const std::vector<Bar>& bars,size_t i,int side,double atr,
                              double sl_mult,double trail_arm_mult,double trail_mult){
    Trade r{}; if(i>=bars.size()||atr<=0) return r;
    double ep=(side>0)?bars[i].ask_c:bars[i].bid_c; if(ep<=0) return r;
    double sl=side>0?ep-sl_mult*atr:ep+sl_mult*atr;
    double mfe_px=ep;
    bool trail_armed=false;
    r.entry_ts=bars[i].ts_open; r.side=side;
    size_t end=std::min(bars.size(),i+1+(size_t)MAX_HOLD_BARS); double xp=ep;
    for(size_t k=i+1;k<end;++k){const auto& b=bars[k];
        if(side>0){
            // update MFE
            if(b.bid_h>mfe_px) mfe_px=b.bid_h;
            double mfe = mfe_px - ep;
            if(!trail_armed && mfe >= trail_arm_mult*atr){trail_armed=true;}
            if(trail_armed){double new_sl = mfe_px - trail_mult*atr; if(new_sl>sl) sl=new_sl;}
            if(b.bid_l<=sl){xp=sl; r.bars=(int)(k-i); break;}
            xp=b.bid_c;
        } else {
            if(b.ask_l<mfe_px||mfe_px==ep) mfe_px=b.ask_l;
            double mfe = ep - mfe_px;
            if(!trail_armed && mfe >= trail_arm_mult*atr){trail_armed=true;}
            if(trail_armed){double new_sl = mfe_px + trail_mult*atr; if(new_sl<sl) sl=new_sl;}
            if(b.ask_h>=sl){xp=sl; r.bars=(int)(k-i); break;}
            xp=b.ask_c;
        }
    }
    if(r.bars==0) r.bars=(int)(end-i-1);
    double pnl_pts=side>0?(xp-ep):(ep-xp);
    r.pnl_gross=(pnl_pts/PT_XAU)*1.0*LOT_XAU;
    return r;
}

// ---- Signal evaluators (lifted from winning cells) ----
static int sig_donchian(const std::vector<Bar>& bars, int i, int N=20){
    if(i<N+1) return 0;
    double hi=bars[i-N].mid_h(),lo=bars[i-N].mid_l();
    for(int k=i-N+1;k<=i-1;++k){if(bars[k].mid_h()>hi)hi=bars[k].mid_h(); if(bars[k].mid_l()<lo)lo=bars[k].mid_l();}
    if(bars[i].ask_c>hi) return +1;
    if(bars[i].bid_c<lo) return -1;
    return 0;
}
static int sig_inside_bar(const std::vector<Bar>& bars, int i){
    if(i<3) return 0;
    const auto& a=bars[i-2]; const auto& b=bars[i-1]; const auto& c=bars[i];
    if(!(b.mid_h()<a.mid_h() && b.mid_l()>a.mid_l())) return 0;
    if(c.ask_c>b.mid_h()) return +1;
    if(c.bid_c<b.mid_l()) return -1;
    return 0;
}
static int sig_momentum(const std::vector<Bar>& bars, int i, int N=20){
    if(i<N+2) return 0;
    if(bars[i].mid_c() > bars[i-N].mid_c()*1.001) return +1;
    if(bars[i].mid_c() < bars[i-N].mid_c()*0.999) return -1;
    return 0;
}
static int sig_er_trend(const std::vector<Bar>& bars, const std::vector<double>& er, int i, double thr=0.20, int N=20){
    if(i<N+2) return 0;
    if(er[i] < thr) return 0;
    if(bars[i].mid_c() > bars[i-N].mid_c()) return +1;
    if(bars[i].mid_c() < bars[i-N].mid_c()) return -1;
    return 0;
}
static int sig_keltner(const std::vector<Bar>& bars, const std::vector<double>& ema, const std::vector<double>& atr, int i){
    if(i<22||atr[i]<=0) return 0;
    double up=ema[i]+2.0*atr[i], lo=ema[i]-2.0*atr[i];
    if(bars[i].ask_c>up) return +1;
    if(bars[i].bid_c<lo) return -1;
    return 0;
}

// Generic runner: signal-fn -> bracket-fn -> trades, with cooldown
template <typename SigFn, typename BracketFn>
static std::vector<Trade> run_signal(const std::vector<Bar>& bars,
                                     const std::vector<double>& atr,
                                     SigFn sig, BracketFn br,
                                     int warmup){
    std::vector<Trade> out;
    int cd = 0;
    for(int i=warmup;i<(int)bars.size();++i){
        if(cd>0){--cd; continue;}
        if(atr[i]<=0) continue;
        int s = sig(i); if(s==0) continue;
        auto t = br(bars, (size_t)i, s, atr[i]);
        if(t.entry_ts==0) continue;
        out.push_back(t);
        cd = 1 + t.bars;
    }
    return out;
}

// ---- Summary ----
struct Summary { int n=0, wins=0; double gross=0; std::map<std::string,double> by_m; };
static std::string ym_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);char b[12];std::strftime(b,sizeof(b),"%Y-%m",&tm);return b;}
static const double COST = 0.06;

static Summary summarise(const std::vector<Trade>& trades){
    Summary s;
    for(const auto& t:trades){
        ++s.n;
        s.gross += t.pnl_gross;
        if(t.pnl_gross - COST > 0) ++s.wins;
        s.by_m[ym_of(t.entry_ts)] += t.pnl_gross - COST;
    }
    return s;
}

static int months_pos(const Summary& s){int p=0; for(auto& m:s.by_m) if(m.second>0) ++p; return p;}

// =====================================================================
//  Main
// =====================================================================
int main(int argc, char** argv){
    std::vector<std::string> xau_pat, us500_pat;
    std::string out_path;
    auto need=[&](int&i,const char*f)->const char*{if(i+1>=argc)std::exit(2);return argv[++i];};
    for(int i=1;i<argc;++i){std::string a=argv[i];
        if(a=="--xau") xau_pat.push_back(need(i,"--xau"));
        else if(a=="--us500") us500_pat.push_back(need(i,"--us500"));
        else if(a=="--out") out_path = need(i,"--out");
    }
    if(xau_pat.empty()){std::cerr<<"need --xau\n";return 2;}

    // Load XAU
    std::vector<Tick> xt;
    for(auto& p:xau_pat) for(auto& f:u::glob_expand(p)){auto v=load_ticks(f); xt.insert(xt.end(),v.begin(),v.end());}
    std::sort(xt.begin(),xt.end(),[](const Tick& a,const Tick& b){return a.ts<b.ts;});
    if(xt.empty()){std::cerr<<"no XAU ticks\n";return 1;}
    std::cerr<<"[load] XAU ticks="<<xt.size()<<"\n";

    auto bars_h4 = resample(xt, 14400);
    auto bars_w1 = resample(xt, 7*86400);
    auto atr_h4  = compute_atr(bars_h4, 14);
    auto atr_w1  = compute_atr(bars_w1, 14);
    auto ema_h4  = compute_ema(bars_h4, 20);
    auto ema_w1  = compute_ema(bars_w1, 20);
    auto er_h4   = compute_kaufman_er(bars_h4, 20);
    std::cerr<<"[load] H4 bars="<<bars_h4.size()<<"  W1 bars="<<bars_w1.size()<<"\n";

    // Optionally load US500 H4 for cross-asset filter
    std::vector<Bar> us500_h4; std::vector<double> us500_ema_h4;
    if(!us500_pat.empty()){
        std::vector<Tick> ut;
        for(auto& p:us500_pat) for(auto& f:u::glob_expand(p)){auto v=load_ticks(f); ut.insert(ut.end(),v.begin(),v.end());}
        std::sort(ut.begin(),ut.end(),[](const Tick& a,const Tick& b){return a.ts<b.ts;});
        us500_h4 = resample(ut, 14400);
        us500_ema_h4 = compute_ema(us500_h4, 20);
        std::cerr<<"[load] US500 ticks="<<ut.size()<<"  H4 bars="<<us500_h4.size()<<"\n";
    }

    xt.clear(); xt.shrink_to_fit();

    std::ofstream out;
    if(!out_path.empty()){
        out.open(out_path);
        out << "test,cell,n,wins,wr,gross,net_at_006,months_total,months_pos,be_cost\n";
        out << std::fixed << std::setprecision(4);
    }

    auto report = [&](const std::string& test, const std::string& cell, const Summary& s){
        if(s.n < 5) return;
        double net = s.gross - COST * s.n;
        double be = s.n>0 ? s.gross/s.n : 0;
        std::cout << std::left << std::setw(18) << test
                  << std::setw(38) << cell
                  << std::right << std::setw(5) << s.n
                  << std::setw(7) << (100.0*s.wins/std::max(1,s.n))
                  << std::setw(11) << s.gross
                  << std::setw(11) << net
                  << std::setw(6) << s.by_m.size()
                  << std::setw(6) << months_pos(s)
                  << std::setw(8) << be
                  << "\n";
        if(out) out << test << ",\"" << cell << "\"," << s.n << "," << s.wins
                    << "," << (s.n? double(s.wins)/s.n:0) << "," << s.gross << "," << net
                    << "," << s.by_m.size() << "," << months_pos(s) << "," << be << "\n";
    };

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n=== HEADER ===\n";
    std::cout << std::left << std::setw(18) << "test"
              << std::setw(38) << "cell"
              << std::right << std::setw(5) << "n"
              << std::setw(7) << "wr%"
              << std::setw(11) << "gross$"
              << std::setw(11) << "net$"
              << std::setw(6) << "Mtot"
              << std::setw(6) << "M+"
              << std::setw(8) << "BE$"
              << "\n";

    // ==== TEST 1: R:R ASYMMETRY SWEEP on top XAU 4h cells ====
    // Original cells were sl=1.5*ATR, tp=3.0*ATR (2:1). Test 1:1 through 6:1.
    std::cout << "\n--- TEST 1: R:R SWEEP (XAU 4h, top cells, SL fixed at 1.5*ATR) ---\n";
    struct RR { double tp_mult; const char* lbl; };
    RR rrs[] = { {1.5,"1:1"}, {3.0,"2:1"}, {4.5,"3:1"}, {6.0,"4:1"}, {7.5,"5:1"}, {9.0,"6:1"} };
    for(auto& rr : rrs){
        auto sl_mult = 1.5;
        auto br = [&](const std::vector<Bar>& bb, size_t i, int s, double a){
            return bracket_fixed(bb, i, s, a, sl_mult, rr.tp_mult);
        };
        std::ostringstream nm;
        nm << "Donchian_RR" << rr.lbl;
        report("RR_sweep", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_donchian(bars_h4,i,20);}, br, 22)));
        nm.str(""); nm << "InsideBar_RR" << rr.lbl;
        report("RR_sweep", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_inside_bar(bars_h4,i);}, br, 16)));
        nm.str(""); nm << "ER020_RR" << rr.lbl;
        report("RR_sweep", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_er_trend(bars_h4,er_h4,i,0.20,20);}, br, 22)));
        nm.str(""); nm << "Momentum_RR" << rr.lbl;
        report("RR_sweep", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_momentum(bars_h4,i,20);}, br, 22)));
        nm.str(""); nm << "Keltner_RR" << rr.lbl;
        report("RR_sweep", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4,i);}, br, 22)));
    }

    // ==== TEST 2: TRAILING-STOP VARIANTS ====
    std::cout << "\n--- TEST 2: TRAILING STOP (XAU 4h, top cells) ---\n";
    struct TS { double arm; double trail; const char* lbl; };
    TS tses[] = { {1.5, 1.0, "arm1.5_trail1.0"}, {2.0, 1.5, "arm2.0_trail1.5"},
                  {3.0, 2.0, "arm3.0_trail2.0"} };
    for(auto& ts : tses){
        auto br = [&](const std::vector<Bar>& bb, size_t i, int s, double a){
            return bracket_trailing(bb, i, s, a, 1.5, ts.arm, ts.trail);
        };
        std::ostringstream nm;
        nm << "Donchian_TS_" << ts.lbl;
        report("TrailStop", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_donchian(bars_h4,i,20);}, br, 22)));
        nm.str(""); nm << "InsideBar_TS_" << ts.lbl;
        report("TrailStop", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_inside_bar(bars_h4,i);}, br, 16)));
        nm.str(""); nm << "Momentum_TS_" << ts.lbl;
        report("TrailStop", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_momentum(bars_h4,i,20);}, br, 22)));
        nm.str(""); nm << "Keltner_TS_" << ts.lbl;
        report("TrailStop", nm.str(),
               summarise(run_signal(bars_h4, atr_h4,
                                   [&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4,i);}, br, 22)));
    }

    // ==== TEST 3: W1 (weekly) timeframe ====
    std::cout << "\n--- TEST 3: W1 weekly timeframe (full family sweep) ---\n";
    auto br_w1_fixed = [&](double slm, double tpm){
        return [&,slm,tpm](const std::vector<Bar>& bb, size_t i, int s, double a){
            return bracket_fixed(bb, i, s, a, slm, tpm);
        };
    };
    auto er_w1 = compute_kaufman_er(bars_w1, 8);  // shorter ER on W1
    for(auto sl_tp : std::vector<std::pair<double,double>>{{1.5,3.0},{2.0,4.0}}){
        std::ostringstream slt; slt << "sl" << sl_tp.first << "_tp" << sl_tp.second;
        report("W1", "Donchian_N10_" + slt.str(),
               summarise(run_signal(bars_w1, atr_w1,
                                   [&](int i){return sig_donchian(bars_w1,i,10);}, br_w1_fixed(sl_tp.first,sl_tp.second), 12)));
        report("W1", "Momentum_N10_" + slt.str(),
               summarise(run_signal(bars_w1, atr_w1,
                                   [&](int i){return sig_momentum(bars_w1,i,10);}, br_w1_fixed(sl_tp.first,sl_tp.second), 12)));
        report("W1", "Keltner_" + slt.str(),
               summarise(run_signal(bars_w1, atr_w1,
                                   [&](int i){return sig_keltner(bars_w1,ema_w1,atr_w1,i);}, br_w1_fixed(sl_tp.first,sl_tp.second), 22)));
        report("W1", "ER020_N8_" + slt.str(),
               summarise(run_signal(bars_w1, atr_w1,
                                   [&](int i){return sig_er_trend(bars_w1,er_w1,i,0.20,8);}, br_w1_fixed(sl_tp.first,sl_tp.second), 12)));
        report("W1", "InsideBar_" + slt.str(),
               summarise(run_signal(bars_w1, atr_w1,
                                   [&](int i){return sig_inside_bar(bars_w1,i);}, br_w1_fixed(sl_tp.first,sl_tp.second), 8)));
    }

    // ==== TEST 4: CROSS-ASSET FILTER (XAU 4h gated by US500 EMA slope) ====
    if(!us500_h4.empty()){
        std::cout << "\n--- TEST 4: XAU 4h gated by US500 direction (EMA20 slope) ---\n";
        // Build per-XAU-bar US500 EMA slope sign by timestamp alignment.
        std::vector<int> us_slope_sign(bars_h4.size(), 0);
        size_t ui = 0;
        for(size_t i=0;i<bars_h4.size();++i){
            while(ui+1 < us500_h4.size() && us500_h4[ui+1].ts_open <= bars_h4[i].ts_open) ++ui;
            if(ui >= 1){
                double s = us500_ema_h4[ui] - us500_ema_h4[ui-1];
                us_slope_sign[i] = s>0 ? +1 : (s<0 ? -1 : 0);
            }
        }
        // Filter modes: "same_dir" = take long only if us_slope > 0; short only if us_slope < 0
        //               "opp_dir"  = inverse (XAU/US500 inverse correlation hypothesis)
        struct Mode { const char* lbl; int sign_mult; };
        Mode modes[] = { {"same_dir", +1}, {"opp_dir", -1} };
        for(auto& mode : modes){
            auto br = [&](const std::vector<Bar>& bb, size_t i, int s, double a){
                return bracket_fixed(bb, i, s, a, 1.5, 3.0);
            };
            auto gated = [&](auto raw_sig)->std::vector<Trade>{
                std::vector<Trade> out; int cd=0;
                for(int i=22;i<(int)bars_h4.size();++i){
                    if(cd>0){--cd; continue;}
                    if(atr_h4[i]<=0) continue;
                    int s = raw_sig(i); if(s==0) continue;
                    int us = us_slope_sign[i];
                    if(us == 0) continue;
                    // For same_dir: require s and us same sign.
                    // For opp_dir:  require s and us opposite signs.
                    if(mode.sign_mult > 0 && s != us) continue;
                    if(mode.sign_mult < 0 && s == us) continue;
                    auto t = br(bars_h4, (size_t)i, s, atr_h4[i]);
                    if(t.entry_ts == 0) continue;
                    out.push_back(t);
                    cd = 1 + t.bars;
                }
                return out;
            };
            std::string suf = std::string("_") + mode.lbl;
            report("CrossAsset", "Donchian"+suf,    summarise(gated([&](int i){return sig_donchian(bars_h4,i,20);})));
            report("CrossAsset", "InsideBar"+suf,   summarise(gated([&](int i){return sig_inside_bar(bars_h4,i);})));
            report("CrossAsset", "Momentum"+suf,    summarise(gated([&](int i){return sig_momentum(bars_h4,i,20);})));
            report("CrossAsset", "Keltner"+suf,     summarise(gated([&](int i){return sig_keltner(bars_h4,ema_h4,atr_h4,i);})));
            report("CrossAsset", "ER020"+suf,       summarise(gated([&](int i){return sig_er_trend(bars_h4,er_h4,i,0.20,20);})));
        }
    } else {
        std::cout << "\n--- TEST 4: cross-asset filter skipped (no --us500 data) ---\n";
    }

    return 0;
}
