// fx_diagnostic_d1: D1-specific variant pointing at /tmp/d1_* csvs.
// Adds: 200-day Donchian breakout (slow), and long-only-trend tests.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

struct Bar { long long ts = 0; double o=0,h=0,l=0,c=0; };
static std::vector<Bar> load_bars(const std::string& path) {
    std::vector<Bar> out;
    std::ifstream f(path); if (!f) return out;
    std::string line; bool first = true;
    while (std::getline(f, line)) {
        if (first) { first = false; if (!line.empty() && (line[0]<'0'||line[0]>'9') && line[0]!='-') continue; }
        std::stringstream ss(line); std::string t; std::vector<std::string> tok;
        while (std::getline(ss,t,',')) tok.push_back(t);
        if (tok.size() < 5) continue;
        Bar b; b.ts = std::atoll(tok[0].c_str());
        b.o = std::atof(tok[1].c_str()); b.h = std::atof(tok[2].c_str());
        b.l = std::atof(tok[3].c_str()); b.c = std::atof(tok[4].c_str());
        if (b.h > 0) out.push_back(b);
    }
    return out;
}

struct Stats { double mean=0, std=0; int n=0; };
static Stats statsv(const std::vector<double>& r) {
    Stats s; if (r.empty()) return s; s.n=(int)r.size();
    for (double x : r) s.mean += x; s.mean /= s.n;
    double m2=0; for (double x : r) { double d=x-s.mean; m2+=d*d; } m2/=s.n;
    s.std = std::sqrt(m2); return s;
}
static double sharpe(const std::vector<double>& r) {
    Stats s = statsv(r);
    return (s.std > 1e-30) ? (s.mean/s.std)*std::sqrt((double)s.n) : 0;
}

// Donchian N + ATR bracket (sl_m, tp_m) at cost
static std::vector<double> donch_pnls(const std::vector<Bar>& bars, int N,
                                      double sl_m, double tp_m, int max_hold,
                                      double pt, double vpp, double lot,
                                      double hs, double cost) {
    std::vector<double> pnls;
    if ((int)bars.size() < N + 20) return pnls;
    const int ATR_N = 14;
    std::vector<double> atr(bars.size(), 0), tr(bars.size(), 0);
    for (size_t i=1;i<bars.size();++i) {
        double h=bars[i].h,l=bars[i].l,pc=bars[i-1].c;
        tr[i]=std::max({h-l,std::abs(h-pc),std::abs(l-pc)});
    }
    double sum=0; for (int i=1;i<=ATR_N;++i) sum+=tr[i]; atr[ATR_N]=sum/ATR_N;
    for (size_t i=ATR_N+1;i<bars.size();++i) atr[i]=(atr[i-1]*(ATR_N-1)+tr[i])/ATR_N;
    std::vector<double> dh(bars.size(),0), dl(bars.size(),0);
    for (int i=N;i<(int)bars.size();++i) {
        double hh=bars[i-1].h, ll=bars[i-1].l;
        for (int k=2;k<=N;++k) { if(bars[i-k].h>hh) hh=bars[i-k].h; if(bars[i-k].l<ll) ll=bars[i-k].l; }
        dh[i]=hh; dl[i]=ll;
    }
    int last=-1;
    for (int i=N+1;i<(int)bars.size();++i) {
        if (atr[i]<=0) continue;
        if (last>=0 && (i-last)<=1) continue;
        int side=0;
        if (bars[i].c > dh[i-1]) side=+1;
        else if (bars[i].c < dl[i-1]) side=-1;
        if (!side) continue;
        double e=bars[i].c, sd=sl_m*atr[i], td=tp_m*atr[i];
        double tp = side>0 ? e+td : e-td;
        double sl = side>0 ? e-sd : e+sd;
        int end = std::min((int)bars.size(), i+max_hold+1);
        double ex=e;
        for (int j=i+1; j<end; ++j) {
            const auto& b = bars[j];
            if (side>0) {
                if (b.l<=sl) { ex=sl-hs; break; }
                if (b.h>=tp) { ex=tp-hs; break; }
                ex=b.c;
            } else {
                if (b.h>=sl) { ex=sl+hs; break; }
                if (b.l<=tp) { ex=tp+hs; break; }
                ex=b.c;
            }
        }
        double pp = side>0 ? (ex-e) : (e-ex);
        double gross = (pp/pt)*vpp*lot;
        pnls.push_back(gross - cost);
        last=i;
    }
    return pnls;
}

// Long-only buy at break above N-day high, exit at trail stop trail*ATR
static std::vector<double> longonly_donch(const std::vector<Bar>& bars, int N, double trail_m,
                                          double pt, double vpp, double lot, double hs, double cost) {
    std::vector<double> pnls;
    if ((int)bars.size() < N + 20) return pnls;
    const int ATR_N = 14;
    std::vector<double> atr(bars.size(),0), tr(bars.size(),0);
    for (size_t i=1;i<bars.size();++i) {
        double h=bars[i].h,l=bars[i].l,pc=bars[i-1].c;
        tr[i]=std::max({h-l,std::abs(h-pc),std::abs(l-pc)});
    }
    double sum=0; for (int i=1;i<=ATR_N;++i) sum+=tr[i]; atr[ATR_N]=sum/ATR_N;
    for (size_t i=ATR_N+1;i<bars.size();++i) atr[i]=(atr[i-1]*(ATR_N-1)+tr[i])/ATR_N;
    std::vector<double> dh(bars.size(),0);
    for (int i=N;i<(int)bars.size();++i) {
        double hh=bars[i-1].h;
        for (int k=2;k<=N;++k) if(bars[i-k].h>hh) hh=bars[i-k].h;
        dh[i]=hh;
    }
    int last=-1;
    for (int i=N+1;i<(int)bars.size();++i) {
        if (atr[i]<=0) continue;
        if (last>=0 && (i-last)<=1) continue;
        if (bars[i].c <= dh[i-1]) continue;
        double e=bars[i].c, peak=e, exit=0;
        for (int j=i+1; j<(int)bars.size(); ++j) {
            const auto& b=bars[j];
            peak = std::max(peak, b.h);
            double trail = peak - trail_m*atr[i];
            if (b.l <= trail) { exit = trail - hs; break; }
            exit = b.c;
        }
        double pp = exit - e;
        double gross = (pp/pt)*vpp*lot;
        pnls.push_back(gross - cost);
        last=i;
        if ((int)pnls.size() > 1000) break;
    }
    return pnls;
}

struct PS { std::string sym; std::string csv; double pt, vpp, lot, hs, cost; };
int main() {
    std::vector<PS> p = {
        {"EURUSD","/tmp/d1_EURUSD.csv", 0.0001,1.0,0.01,0.00005,0.36},
        {"GBPUSD","/tmp/d1_GBPUSD.csv", 0.0001,1.0,0.01,0.00005,0.36},
        {"USDJPY","/tmp/d1_USDJPY.csv", 0.01,  1.0,0.01,0.005,  0.22},
        {"AUDUSD","/tmp/d1_AUDUSD.csv", 0.0001,1.0,0.01,0.00005,0.36},
        {"NZDUSD","/tmp/d1_NZDUSD.csv", 0.0001,1.0,0.01,0.00005,0.36},
        {"USDCAD","/tmp/d1_USDCAD.csv", 0.0001,1.0,0.01,0.00005,0.36},
        {"EURGBP","/tmp/d1_EURGBP.csv", 0.0001,1.0,0.01,0.00005,0.36},
    };
    printf("%-8s %5s | %-22s | %-22s | %-22s | %-22s\n",
           "sym","bars","D20 n/Sh/net(c=1)","D55 n/Sh/net(c=1)","D200 n/Sh/net(c=1)","Long-only D55 trail=2");
    printf("%-8s %5s | %-22s | %-22s | %-22s | %-22s\n",
           "---","----","-----------------","-----------------","------------------","-----------------------");
    for (auto& x : p) {
        auto bars = load_bars(x.csv);
        if (bars.empty()) { printf("%-8s missing\n", x.sym.c_str()); continue; }
        struct R { int n; double sh; double net; };
        auto pack = [&](const std::vector<double>& v){
            R r; r.n=(int)v.size(); r.sh=sharpe(v); r.net=0; for (auto z:v) r.net+=z; return r;
        };
        auto d20  = pack(donch_pnls(bars, 20,  1.5, 3.0, 30, x.pt, x.vpp, x.lot, x.hs, x.cost));
        auto d55  = pack(donch_pnls(bars, 55,  1.5, 4.0, 60, x.pt, x.vpp, x.lot, x.hs, x.cost));
        auto d200 = pack(donch_pnls(bars, 200, 2.0, 5.0,120, x.pt, x.vpp, x.lot, x.hs, x.cost));
        auto lo   = pack(longonly_donch(bars, 55, 2.0,           x.pt, x.vpp, x.lot, x.hs, x.cost));
        printf("%-8s %5d | n=%-3d Sh=%5.2f $%6.2f | n=%-3d Sh=%5.2f $%6.2f | n=%-3d Sh=%5.2f $%6.2f | n=%-3d Sh=%5.2f $%6.2f\n",
               x.sym.c_str(), (int)bars.size(),
               d20.n, d20.sh, d20.net, d55.n, d55.sh, d55.net, d200.n, d200.sh, d200.net,
               lo.n, lo.sh, lo.net);
    }
    return 0;
}
