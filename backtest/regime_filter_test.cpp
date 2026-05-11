// =====================================================================
// backtest/regime_filter_test.cpp -- Pass 2 (S33d 2026-05-11)
// ---------------------------------------------------------------------
// Take the 10 strongest XAU 4h cells from edge_hunt + edge_hunt_v3 and
// apply regime filters one at a time. See whether ANY filter materially
// boosts net or stability (% months positive / longest neg streak).
//
// Filters tested (mutually exclusive; baseline = unfiltered):
//   1. D1 trend agreement: signal must agree with the daily EMA20 slope.
//      Implemented by tracking parallel D1 bars built from the same
//      tick stream; D1 EMA20 must be rising (long) or falling (short).
//   2. Vol regime: only fire when current ATR is in the top 50%-ile of
//      the trailing 100-bar ATR distribution (trending environment).
//   3. Time-of-day: 4h has 6 bar-starts per day (0,4,8,12,16,20 UTC).
//      Test which UTC hour-of-bar-start has the strongest signal.
//   4. Spread quartile: only fire when current bid/ask spread is below
//      the 50%-ile of the trailing 100-bar spread distribution.
//
// Build:
//   g++ -std=c++17 -O2 -Wall -Wextra \
//       backtest/regime_filter_test.cpp -o backtest/regime_filter_test
//
// Run (XAU, per-year chunks):
//   for Y in 2023 2024 2025; do
//     ./backtest/regime_filter_test \
//         --csv "outputs/duka_xauusd_daily/${Y}-*.csv" \
//         --year $Y --out backtest/regime_filter_${Y}.csv
//   done
// =====================================================================

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
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
    if(o.empty())o.push_back(pat);return o;}
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
    double spread()const{return ask_c-bid_c;}
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
    std::vector<double> atr(bars.size(),0.0); if((int)bars.size()<=n) return atr;
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

// Daily EMA-20 slope from H4 bars (every 6 bars = 1 day)
static std::vector<double> compute_d1_ema_slope(const std::vector<Bar>& bars,int ema_n=20){
    // Aggregate 4h bars into D1 by date (UTC)
    std::vector<Bar> d1;
    long long last_date = -1;
    Bar cur{};
    bool started=false;
    for(const auto& b : bars){
        std::time_t tt=(std::time_t)b.ts_open; std::tm tm{}; gmtime_r(&tt,&tm);
        long long date=tm.tm_year*10000LL+tm.tm_mon*100LL+tm.tm_mday;
        if(date!=last_date){
            if(started) d1.push_back(cur);
            cur=Bar{}; cur.ts_open=b.ts_open; cur.bid_o=b.bid_o; cur.bid_h=b.bid_h; cur.bid_l=b.bid_l; cur.bid_c=b.bid_c;
            cur.ask_o=b.ask_o; cur.ask_h=b.ask_h; cur.ask_l=b.ask_l; cur.ask_c=b.ask_c;
            started=true; last_date=date;
        } else {
            if(b.bid_h>cur.bid_h)cur.bid_h=b.bid_h; if(b.bid_l<cur.bid_l)cur.bid_l=b.bid_l; cur.bid_c=b.bid_c;
            if(b.ask_h>cur.ask_h)cur.ask_h=b.ask_h; if(b.ask_l<cur.ask_l)cur.ask_l=b.ask_l; cur.ask_c=b.ask_c;
        }
    }
    if(started) d1.push_back(cur);

    auto d1_ema = compute_ema(d1, ema_n);
    // Per-4h-bar D1 slope: map each H4 bar to the D1 EMA slope at that bar's date
    std::vector<double> slope(bars.size(),0.0);
    int d_idx = 0;
    for(size_t i=0;i<bars.size();++i){
        // advance d_idx to current day
        std::time_t tt=(std::time_t)bars[i].ts_open; std::tm tm{}; gmtime_r(&tt,&tm);
        long long date=tm.tm_year*10000LL+tm.tm_mon*100LL+tm.tm_mday;
        while(d_idx < (int)d1.size()){
            std::time_t dt=(std::time_t)d1[d_idx].ts_open; std::tm dtm{}; gmtime_r(&dt,&dtm);
            long long ddate=dtm.tm_year*10000LL+dtm.tm_mon*100LL+dtm.tm_mday;
            if(ddate >= date) break;
            ++d_idx;
        }
        if(d_idx >= 1 && d_idx < (int)d1.size()){
            slope[i] = d1_ema[d_idx] - d1_ema[d_idx-1];
        }
    }
    return slope;
}

// ---------- Bracket
struct Trade{long long entry_ts=0;int side=0;double pnl_gross=0;int bars=0;};
static const int MAX_HOLD_BARS = 50;
static const double LOT = 0.01;
static const double PT_SIZE = 0.01;

static Trade bracket_realistic(const std::vector<Bar>& bars,size_t i,int side,double atr,double sl_mult,double tp_mult){
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
    r.pnl_gross=(pnl_pts/PT_SIZE)*1.0*LOT;
    return r;
}

// ---------- Filter modes
enum class Filter { NONE, D1_TREND, VOL_HIGH, VOL_LOW, SPREAD_LOW };
static const char* filter_name(Filter f){
    switch(f){case Filter::NONE:return "none"; case Filter::D1_TREND:return "d1_trend";
              case Filter::VOL_HIGH:return "vol_high"; case Filter::VOL_LOW:return "vol_low";
              case Filter::SPREAD_LOW:return "spread_low";} return "?";
}

struct FilterCtx {
    std::vector<double> d1_slope;        // per-bar D1 EMA slope (sign-only)
    std::vector<double> atr;             // already computed
    std::vector<double> atr_pct;         // current-bar ATR percentile over last 100 bars
    std::vector<double> spread_pct;      // current spread percentile over last 100 bars
};

static FilterCtx build_filter_ctx(const std::vector<Bar>& bars, const std::vector<double>& atr){
    FilterCtx c;
    c.atr = atr;
    c.d1_slope = compute_d1_ema_slope(bars, 20);
    c.atr_pct.assign(bars.size(),0.5);
    c.spread_pct.assign(bars.size(),0.5);
    for(int i=100;i<(int)bars.size();++i){
        std::vector<double> a,s;
        for(int k=i-99;k<=i;++k){ if(atr[k]>0) a.push_back(atr[k]); s.push_back(bars[k].spread()); }
        std::sort(a.begin(),a.end()); std::sort(s.begin(),s.end());
        int cnt_a=0; for(auto v:a) if(v<atr[i]) ++cnt_a;
        int cnt_s=0; for(auto v:s) if(v<bars[i].spread()) ++cnt_s;
        c.atr_pct[i] = a.empty()?0.5:double(cnt_a)/a.size();
        c.spread_pct[i] = s.empty()?0.5:double(cnt_s)/s.size();
    }
    return c;
}

// Returns true if the filter permits firing this signal at bar i
static bool filter_allow(Filter f, int i, int side, const FilterCtx& ctx){
    switch(f){
        case Filter::NONE: return true;
        case Filter::D1_TREND: {
            if(side > 0) return ctx.d1_slope[i] > 0;
            if(side < 0) return ctx.d1_slope[i] < 0;
            return false;
        }
        case Filter::VOL_HIGH:   return ctx.atr_pct[i] >= 0.50;
        case Filter::VOL_LOW:    return ctx.atr_pct[i] <  0.50;
        case Filter::SPREAD_LOW: return ctx.spread_pct[i] <= 0.50;
    }
    return true;
}

// ---------- Signal definitions (the proven winners)
struct CellSpec {
    std::string name;
    std::string signal_type;  // "donchian20", "insidebar", "er020", "keltner", "adx_mom20", "momentum20"
    double sl_mult;
    double tp_mult;
};

static const std::vector<CellSpec> kCells = {
    { "Donchian_N20_sl1.5tp3.0", "donchian20", 1.5, 3.0 },
    { "InsideBar_sl2.0tp4.0",    "insidebar",  2.0, 4.0 },
    { "Momentum20_sl2.0tp4.0",   "momentum20", 2.0, 4.0 },
    { "ER0.20_sl1.5tp3.0",       "er020",      1.5, 3.0 },
    { "Keltner_sl1.5tp3.0",      "keltner",    1.5, 3.0 },
    { "Keltner_sl2.0tp4.0",      "keltner",    2.0, 4.0 },
    { "ADX_Mom_sl2.0tp4.0",      "adx_mom20",  2.0, 4.0 },
    { "ADX_Mom_sl1.5tp3.0",      "adx_mom20",  1.5, 3.0 },
};

// Compute Wilder ADX (same as edge_hunt)
static std::vector<double> compute_adx(const std::vector<Bar>& bars,int n=14){
    std::vector<double> adx(bars.size(),0); if((int)bars.size()<=n*2) return adx;
    std::vector<double> pdm(bars.size(),0),mdm(bars.size(),0),tr(bars.size(),0);
    for(int i=1;i<(int)bars.size();++i){
        double up=bars[i].mid_h()-bars[i-1].mid_h(); double dn=bars[i-1].mid_l()-bars[i].mid_l();
        pdm[i]=(up>dn&&up>0)?up:0; mdm[i]=(dn>up&&dn>0)?dn:0;
        double cp=bars[i-1].mid_c();
        tr[i]=std::max(bars[i].mid_h()-bars[i].mid_l(),std::max(std::abs(bars[i].mid_h()-cp),std::abs(bars[i].mid_l()-cp)));
    }
    double atr_s=0,pdi_s=0,mdi_s=0;
    for(int i=1;i<=n;++i){atr_s+=tr[i];pdi_s+=pdm[i];mdi_s+=mdm[i];}
    std::vector<double> dx(bars.size(),0);
    if(atr_s>1e-12){double pdi=100*pdi_s/atr_s, mdi=100*mdi_s/atr_s; double sum=pdi+mdi; dx[n]=sum>1e-12?100*std::abs(pdi-mdi)/sum:0;}
    for(int i=n+1;i<(int)bars.size();++i){atr_s=atr_s-atr_s/n+tr[i]; pdi_s=pdi_s-pdi_s/n+pdm[i]; mdi_s=mdi_s-mdi_s/n+mdm[i];
        double pdi=atr_s>1e-12?100*pdi_s/atr_s:0; double mdi=atr_s>1e-12?100*mdi_s/atr_s:0; double sum=pdi+mdi;
        dx[i]=sum>1e-12?100*std::abs(pdi-mdi)/sum:0;}
    double sum_dx=0; for(int i=n+1;i<=2*n;++i) sum_dx+=dx[i];
    if(2*n<(int)bars.size()) adx[2*n]=sum_dx/n;
    for(int i=2*n+1;i<(int)bars.size();++i) adx[i]=(adx[i-1]*(n-1)+dx[i])/n;
    return adx;
}

static int eval_signal(const std::string& type, const std::vector<Bar>& bars, int i,
                      const std::vector<double>& ema20, const std::vector<double>& er,
                      const std::vector<double>& adx){
    if(type == "donchian20"){
        int N=20; if(i<N+1) return 0;
        double hi=bars[i-N].mid_h(),lo=bars[i-N].mid_l();
        for(int k=i-N+1;k<=i-1;++k){if(bars[k].mid_h()>hi)hi=bars[k].mid_h(); if(bars[k].mid_l()<lo)lo=bars[k].mid_l();}
        if(bars[i].ask_c>hi) return +1; if(bars[i].bid_c<lo) return -1; return 0;
    }
    if(type == "insidebar"){
        if(i<3) return 0;
        const auto& a=bars[i-2]; const auto& b=bars[i-1]; const auto& c=bars[i];
        if(!(b.mid_h()<a.mid_h() && b.mid_l()>a.mid_l())) return 0;
        if(c.ask_c>b.mid_h()) return +1; if(c.bid_c<b.mid_l()) return -1; return 0;
    }
    if(type == "momentum20"){
        if(i<22) return 0;
        if(bars[i].mid_c() > bars[i-20].mid_c()*1.001) return +1;
        if(bars[i].mid_c() < bars[i-20].mid_c()*0.999) return -1;
        return 0;
    }
    if(type == "er020"){
        if(i<22) return 0;
        if(er[i] < 0.20) return 0;
        if(bars[i].mid_c() > bars[i-20].mid_c()) return +1;
        if(bars[i].mid_c() < bars[i-20].mid_c()) return -1;
        return 0;
    }
    if(type == "keltner"){
        if(i<22) return 0;
        // computed via passed ema + atr externally
        double atr = i < (int)adx.size() ? bars[i].mid_h()-bars[i].mid_l() : 0; // unused here; use ema-band approach
        double up = ema20[i] + 2.0*0; // we need atr -- handled by closure outside
        (void)up; (void)atr;
        return 0;  // we handle keltner below with proper context
    }
    if(type == "adx_mom20"){
        if(i<32) return 0;
        if(adx[i] < 25.0) return 0;
        if(bars[i].mid_c() > bars[i-20].mid_c()*1.001) return +1;
        if(bars[i].mid_c() < bars[i-20].mid_c()*0.999) return -1;
        return 0;
    }
    return 0;
}

// Specialised keltner eval (needs ema + atr)
static int eval_keltner(const std::vector<Bar>& bars, int i,
                        const std::vector<double>& ema, const std::vector<double>& atr){
    if(i<22||atr[i]<=0) return 0;
    double up = ema[i] + 2.0 * atr[i];
    double lo = ema[i] - 2.0 * atr[i];
    if(bars[i].ask_c > up) return +1;
    if(bars[i].bid_c < lo) return -1;
    return 0;
}

struct RunResult {
    std::string cell, filter;
    int n=0, wins=0;
    double net=0;
    std::map<std::string, double> by_month;
};

static std::string ym_of(long long ts){std::time_t tt=(std::time_t)ts;std::tm tm{};gmtime_r(&tt,&tm);char b[12];std::strftime(b,sizeof(b),"%Y-%m",&tm);return b;}
static const double COST = 0.06;

static RunResult run_filtered(const std::vector<Bar>& bars,
                              const std::vector<double>& atr,
                              const std::vector<double>& ema20,
                              const std::vector<double>& er,
                              const std::vector<double>& adx,
                              const FilterCtx& fctx,
                              const CellSpec& cell, Filter filter){
    RunResult r; r.cell = cell.name; r.filter = filter_name(filter);
    int cd=0;
    for(int i=22; i<(int)bars.size(); ++i){
        if(cd>0){--cd; continue;}
        if(atr[i]<=0) continue;
        int s;
        if(cell.signal_type == "keltner") s = eval_keltner(bars, i, ema20, atr);
        else s = eval_signal(cell.signal_type, bars, i, ema20, er, adx);
        if(s==0) continue;
        if(!filter_allow(filter, i, s, fctx)) continue;
        auto t = bracket_realistic(bars, i, s, atr[i], cell.sl_mult, cell.tp_mult);
        if(t.entry_ts==0) continue;
        double net = t.pnl_gross - COST;
        ++r.n; r.net += net; if(net>0) ++r.wins;
        r.by_month[ym_of(t.entry_ts)] += net;
        cd = 1 + t.bars;
    }
    return r;
}

int main(int argc, char** argv){
    std::vector<std::string> patterns;
    std::string out_path, year_tag;
    auto need=[&](int& i,const char* f)->const char*{if(i+1>=argc)std::exit(2);return argv[++i];};
    for(int i=1;i<argc;++i){std::string a=argv[i];
        if(a=="--csv") patterns.push_back(need(i,"--csv"));
        else if(a=="--year") year_tag = need(i,"--year");
        else if(a=="--out") out_path = need(i,"--out");
    }
    if(patterns.empty()) return 2;

    std::vector<Tick> merged;
    for(auto& p:patterns) for(auto& f:u::glob_expand(p)){
        auto v = load_ticks(f); merged.insert(merged.end(),v.begin(),v.end());
    }
    std::sort(merged.begin(),merged.end(),[](const Tick& a,const Tick& b){return a.ts<b.ts;});
    if(merged.empty()) return 1;

    auto bars = resample(merged, 14400);  // 4h
    merged.clear(); merged.shrink_to_fit();
    if((int)bars.size() < 50) return 1;

    auto atr = compute_atr(bars, 14);
    auto ema20 = compute_ema(bars, 20);
    auto er = compute_kaufman_er(bars, 20);
    auto adx = compute_adx(bars, 14);
    auto fctx = build_filter_ctx(bars, atr);

    std::vector<RunResult> results;
    Filter filters[] = { Filter::NONE, Filter::D1_TREND, Filter::VOL_HIGH, Filter::VOL_LOW, Filter::SPREAD_LOW };
    for(const auto& cell : kCells){
        for(auto f : filters){
            results.push_back(run_filtered(bars, atr, ema20, er, adx, fctx, cell, f));
        }
    }

    // Write CSV
    std::ofstream out;
    if(!out_path.empty()){
        out.open(out_path);
        out << "year,cell,filter,n_trades,wins,wr,net,months_pos,months_neg,longest_neg_streak,best_month,worst_month\n";
        out << std::fixed << std::setprecision(4);
    }
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "year=" << year_tag << "  bars=" << bars.size() << "\n";
    std::cout << std::left << std::setw(28) << "cell" << std::setw(12) << "filter"
              << std::right << std::setw(5) << "n" << std::setw(7) << "wr%"
              << std::setw(10) << "net$" << std::setw(7) << "M+" << std::setw(7) << "M-"
              << std::setw(8) << "max_DD" << "\n";
    for(const auto& r : results){
        int m_pos=0, m_neg=0, streak=0, longest=0;
        double best=0, worst=0;
        for(auto& m : r.by_month){
            if(m.second > 0) { ++m_pos; streak = 0; if(m.second>best) best=m.second; }
            else { ++m_neg; ++streak; if(streak>longest) longest=streak; if(m.second<worst) worst=m.second; }
        }
        std::cout << std::left << std::setw(28) << r.cell << std::setw(12) << r.filter
                  << std::right << std::setw(5) << r.n
                  << std::setw(7) << (r.n? 100.0*r.wins/r.n : 0)
                  << std::setw(10) << r.net
                  << std::setw(7) << m_pos << std::setw(7) << m_neg
                  << std::setw(8) << longest << "\n";
        if(out){
            out << year_tag << "," << r.cell << "," << r.filter << ","
                << r.n << "," << r.wins << "," << (r.n? double(r.wins)/r.n : 0) << ","
                << r.net << "," << m_pos << "," << m_neg << "," << longest << ","
                << best << "," << worst << "\n";
        }
    }
    return 0;
}
