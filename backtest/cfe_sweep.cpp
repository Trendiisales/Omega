// cfe2.cpp — CRTP CFE sweep, exact Python cfe_sweep.py logic
// Columns: ts_ms(0) bid(1) ask(2) ... ewm_drift(15)  — no atr column
// Build: clang++ -O3 -std=c++20 -o /tmp/cfe2 /tmp/cfe2.cpp
// Run:   /tmp/cfe2 ~/Downloads/l2_ticks_2026-04-16.csv

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ─── Tick ────────────────────────────────────────────────────────────────────
struct Tick {
    int64_t ms;
    double  bid, ask, drift, atr;
};

static std::vector<Tick> load_csv(const char* path) {
    std::vector<Tick> out;
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return out; }

    // parse header — find column indices
    std::string line, tok;
    std::getline(f, line);
    // strip \r
    if (!line.empty() && line.back() == '\r') line.pop_back();

    int col_ms=-1, col_bid=-1, col_ask=-1, col_drift=-1, col_atr=-1, ci=0;
    {
        std::istringstream h(line);
        while (std::getline(h, tok, ',')) {
            if (tok=="ts_ms"||tok=="timestamp_ms") col_ms=ci;
            if (tok=="bid")       col_bid=ci;
            if (tok=="ask")       col_ask=ci;
            if (tok=="ewm_drift") col_drift=ci;
            if (tok=="atr")       col_atr=ci;
            ++ci;
        }
    }
    if (col_bid<0||col_ask<0) { fprintf(stderr,"No bid/ask cols\n"); return out; }

    // ATR fallback state (mirrors Python)
    double pb=0, av=2.0;
    std::deque<double> aw;

    out.reserve(200000);
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (!line.empty() && line.back()=='\r') line.pop_back();

        // fast split into fields
        std::vector<const char*> fields;
        fields.reserve(20);
        // tokenise in-place
        static char buf[512];
        if (line.size() >= sizeof(buf)) continue;
        memcpy(buf, line.c_str(), line.size()+1);
        char* p2 = buf;
        fields.push_back(p2);
        for (char* c2 = buf; *c2; ++c2) {
            if (*c2==',') { *c2='\0'; fields.push_back(c2+1); }
        }

        int nc = (int)fields.size();
        int need = std::max({col_ms, col_bid, col_ask});
        if (nc <= need) continue;

        try {
            Tick t;
            t.ms    = col_ms>=0   ? (int64_t)std::stod(fields[col_ms])   : 0;
            t.bid   = std::stod(fields[col_bid]);
            t.ask   = std::stod(fields[col_ask]);
            t.drift = (col_drift>=0 && col_drift<nc) ? std::stod(fields[col_drift]) : 0.0;
            t.atr   = (col_atr>=0  && col_atr<nc)   ? std::stod(fields[col_atr])   : 0.0;
            if (t.bid<=0 || t.ask<t.bid) continue;

            // ATR fallback — exact Python logic
            if (t.atr <= 0.0) {
                if (pb > 0.0) {
                    double tr = (t.ask - t.bid) + std::fabs(t.bid - pb);
                    aw.push_back(tr);
                    if ((int)aw.size() > 200) aw.pop_front();
                    if ((int)aw.size() >= 50) {
                        double s=0; for (double x:aw) s+=x;
                        av = s / aw.size() * 14.0;
                    }
                }
                t.atr = av;
            }
            pb = t.bid;
            out.push_back(t);
        } catch (...) {}
    }
    return out;
}

// ─── Params ──────────────────────────────────────────────────────────────────
struct P {
    int     rn;       // RSI period; EMA alpha = 2/(rn+1)
    double  rt;       // RSI trend threshold low
    double  rm;       // RSI trend threshold high (15.0)
    double  dm;       // drift abs min
    double  st;       // sustained drift threshold
    int64_t sm;       // sustained duration ms
    double  sl;       // SL = sl * ATR
    double  tp;       // TP = tp * SL
    int64_t cd;       // cooldown ms after loss
    int64_t mh;       // max hold ms
    bool    tr;       // trailing stop
    // fixed
    double  ta = 2.0; // trail arm pts
    double  td = 1.0; // trail dist pts
};

// ─── CRTP engine base ────────────────────────────────────────────────────────
template<typename Derived>
struct EngBase {
    // RSI state — mirrors Python ursi()
    std::deque<double> rg, rl;
    double rpm=0, rc=50, rp=50, rt_val=0;
    bool   rw=false;
    double ra;

    // recent mid buffer (last 10)
    std::deque<double> mid_buf;

    // drift sustained
    int     dsd=0;
    int64_t dss=0;

    // position
    bool    pa=false, pl=false, pbe=false;
    double  pe=0, psl=0, ptp=0, pm=0, ps=0.01;
    int64_t pts=0;

    // cooldown / adverse
    int64_t cd_until=0, lcm=0;
    int     lcd=0;
    bool    ab=false;
    double  lex=0;
    int     lld=0;

    int     wt=0;

    // results
    double  total_pnl=0;
    int     n_trades=0, n_wins=0;
    double  long_pnl=0, short_pnl=0;

    explicit EngBase(double alpha) : ra(alpha) {}

    void ursi(double mid) {
        if (rpm==0.0) { rpm=mid; return; }
        double c = mid-rpm; rpm=mid;
        rg.push_back(c>0?c:0.0);
        rl.push_back(c<0?-c:0.0);
        const int n = static_cast<Derived*>(this)->rn();
        if ((int)rg.size()>n) { rg.pop_front(); rl.pop_front(); }
        if ((int)rg.size()>=n) {
            double ag=0,al=0;
            for (double x:rg) ag+=x;
            ag/=n;
            for (double x:rl) al+=x;
            al/=n;
            rp=rc;
            rc = (al==0.0) ? 100.0 : 100.0-100.0/(1.0+ag/al);
            double s = rc-rp;
            if (!rw) { rt_val=s; rw=true; }
            else      rt_val = s*ra + rt_val*(1.0-ra);
        }
    }

    void close(double ep, bool is_long, int64_t ms,
               const P& p, double entry, double sz) {
        double pp = is_long ? (ep-entry) : (entry-ep);
        double pu = pp * sz * 100.0;
        total_pnl += pu;
        ++n_trades;
        if (pu > 0) ++n_wins;
        if (is_long) long_pnl+=pu; else short_pnl+=pu;
        lcd = is_long ? 1 : -1;
        lcm = ms;
        if (pu < 0) {
            ab=true; lex=ep; lld=is_long?1:-1;
            cd_until = ms + p.cd;
        }
        pa=false;
    }

    // Returns true if a trade was opened or managed; false = pass
    bool tick(int64_t ms, double bid, double ask, double drift, double atr,
              const P& p) {
        double mid = (bid+ask)*0.5;
        double sp  = ask-bid;
        ++wt;
        ursi(mid);
        mid_buf.push_back(mid);
        if ((int)mid_buf.size()>10) mid_buf.pop_front();

        // drift sustained tracking — exact Python
        if (drift >= p.st) {
            if (dsd!=1) { dsd=1; dss=ms; }
        } else if (drift <= -p.st) {
            if (dsd!=-1) { dsd=-1; dss=ms; }
        } else {
            dsd=0; dss=0;
        }
        int64_t dsms = dsd ? (ms-dss) : 0;

        // ── manage open position ──────────────────────────────────────────
        if (pa) {
            double mv = pl ? (mid-pe) : (pe-mid);
            if (mv>pm) pm=mv;
            double eff = pl?bid:ask;
            double td_ = std::fabs(ptp-pe);
            if (!pbe && td_>0 && mv>=td_*0.5) { psl=pe; pbe=true; }
            if (p.tr && mv>=p.ta) {
                double tsl = pl ? (mid-p.td) : (mid+p.td);
                if ( pl && tsl>psl) psl=tsl;
                if (!pl && tsl<psl) psl=tsl;
            }
            if (( pl && bid>=ptp) || (!pl && ask<=ptp)) { close(eff,pl,ms,p,pe,ps); return true; }
            if (( pl && bid<=psl) || (!pl && ask>=psl)) { close(eff,pl,ms,p,pe,ps); return true; }
            if (ms-pts > p.mh)                          { close(eff,pl,ms,p,pe,ps); return true; }
            return true;
        }

        // ── entry gates — exact Python order ────────────────────────────
        if (wt<300 || sp>0.40 || atr<1.0) return false;
        if (ms < cd_until)                 return false;
        if (!rw)                           return false;

        // RSI trend direction
        int rd=0;
        if      (rt_val >  p.rt && rt_val <  p.rm) rd=1;
        else if (rt_val < -p.rt && rt_val > -p.rm) rd=-1;
        if (!rd) return false;

        if (rd==1  && drift<0.0)  return false;
        if (rd==-1 && drift>0.0)  return false;
        if (std::fabs(drift) < p.dm) return false;
        if (dsms < p.sm)          return false;

        // price confirm — last 3 ticks
        if ((int)mid_buf.size()>=3) {
            double n3  = mid_buf[mid_buf.size()-3];
            double mv3 = mid-n3;
            if (rd==1  && mv3 < -atr*0.3) return false;
            if (rd==-1 && mv3 >  atr*0.3) return false;
        }

        // opposite-direction cooldown 45 s
        if (lcd!=0 && (ms-lcm)<45000LL && rd!=lcd) return false;

        // adverse block
        if (ab && lld!=0) {
            double dist = (lld==1) ? (lex-mid) : (mid-lex);
            bool same = (rd==1&&lld==1)||(rd==-1&&lld==-1);
            if (same && dist>atr*0.4) return false;
            else ab=false;
        }

        // ── open ─────────────────────────────────────────────────────────
        bool il = (rd==1);
        double sl_ = atr*p.sl, tp_ = sl_*p.tp;
        double e   = il?ask:bid;
        pa=true; pl=il; pe=e; pm=0; pbe=false;
        psl = il ? (e-sl_) : (e+sl_);
        ptp = il ? (e+tp_) : (e-tp_);
        pts = ms;
        // size: exact Python formula
        double raw = 10.0/(sl_*100.0);
        ps = std::min(0.10, std::max(0.01, std::round(raw/0.01)*0.01));
        return true;
    }

    void fin(int64_t ms, const P& p) {
        if (pa) close(pe, pl, ms, p, pe, ps);
    }
};

// ─── Concrete engine (CRTP leaf) ─────────────────────────────────────────────
struct Eng : EngBase<Eng> {
    int _rn;
    explicit Eng(const P& p) : EngBase<Eng>(2.0/(p.rn+1)), _rn(p.rn) {}
    int rn() const { return _rn; }
};

// ─── Result ──────────────────────────────────────────────────────────────────
struct Res {
    double pnl, wr, lp, sp;
    int    n;
    P      p;
};

// ─── Grid — exact Python cfe_sweep.py ────────────────────────────────────────
// rn:[20,30] rt:[2,4] rm:[15] dm:[0.3,0.5,0.8] st:[0.3,0.5]
// sm:[10000,20000,30000]ms sl:[0.4,0.6,0.8] tp:[2,3,4]
// cd:[15000,30000]ms mh:[300000,600000]ms tr:[F,T]
// total = 2*2*3*2*3*3*3*2*2*2 = 5184

int main(int argc, char* argv[]) {
    if (argc<2) { puts("Usage: cfe2 <csv>"); return 1; }

    auto t0 = std::chrono::steady_clock::now();

    auto ticks = load_csv(argv[1]);
    if (ticks.empty()) { fprintf(stderr,"No ticks loaded\n"); return 1; }
    printf("Loaded %zu ticks\n", ticks.size());

    // Verify ATR is non-zero for a sample tick past warmup
    if (ticks.size()>300 && ticks[300].atr<=0) {
        fprintf(stderr,"ATR is 0 at tick 300 — fallback broken\n"); return 1;
    }

    const int    rn_v[]  = {20,30};
    const double rt_v[]  = {2.0,4.0};
    const double rm_v[]  = {15.0};
    const double dm_v[]  = {0.3,0.5,0.8};
    const double st_v[]  = {0.3,0.5};
    const int64_t sm_v[] = {10000,20000,30000};
    const double sl_v[]  = {0.4,0.6,0.8};
    const double tp_v[]  = {2.0,3.0,4.0};
    const int64_t cd_v[] = {15000,30000};
    const int64_t mh_v[] = {300000,600000};
    const bool   tr_v[]  = {false,true};

    constexpr long TOTAL = 2*2*1*3*2*3*3*3*2*2*2; // 5184
    printf("Testing %ld combinations...\n", TOTAL);

    std::vector<Res> results;
    results.reserve(8000);
    long done=0;

    for (int    rn  : rn_v)
    for (double rt  : rt_v)
    for (double rm_ : rm_v)
    for (double dm  : dm_v)
    for (double st  : st_v)
    for (int64_t sm : sm_v)
    for (double sl  : sl_v)
    for (double tp  : tp_v)
    for (int64_t cd : cd_v)
    for (int64_t mh : mh_v)
    for (bool   tr  : tr_v)
    {
        P pp{rn,rt,rm_,dm,st,sm,sl,tp,cd,mh,tr};

        Eng eng(pp);
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr, pp);
        if (!ticks.empty()) eng.fin(ticks.back().ms, pp);

        if (eng.n_trades >= 3) {
            Res r;
            r.pnl = eng.total_pnl;
            r.wr  = (double)eng.n_wins / eng.n_trades;
            r.lp  = eng.long_pnl;
            r.sp  = eng.short_pnl;
            r.n   = eng.n_trades;
            r.p   = pp;
            results.push_back(r);
        }

        if (++done % 1000 == 0) {
            double best=0;
            for (auto& r:results) if(r.pnl>best) best=r.pnl;
            printf("  %ld/%ld  best=$%+.2f\n", done, TOTAL, best);
            fflush(stdout);
        }
    }

    std::sort(results.begin(), results.end(),
              [](const Res& a, const Res& b){ return a.pnl>b.pnl; });

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1-t0).count();

    printf("\nDone in %.2fs\n", elapsed);
    printf("\n%s\n", std::string(72,'=').c_str());
    printf("TOP 20\n");
    printf("%s\n", std::string(72,'=').c_str());
    printf("%-3s %-9s %-6s %-4s %-7s %-7s  dm    st   sm    sl   tp   cd   mh  tr\n",
           "#","PnL","WR","N","L$","S$");

    int show = std::min((int)results.size(), 20);
    for (int i=0; i<show; ++i) {
        auto& r = results[i];
        printf("%-3d $%+8.2f %5.1f%% %4d $%+6.2f $%+6.2f"
               "  %.1f  %.1f  %2lds  %.2f  %.1f  %2lds  %3ldm  %s\n",
               i+1, r.pnl, r.wr*100, r.n, r.lp, r.sp,
               r.p.dm, r.p.st,
               (long)(r.p.sm/1000),
               r.p.sl, r.p.tp,
               (long)(r.p.cd/1000),
               (long)(r.p.mh/60000),
               r.p.tr?"Y":"N");
    }

    if (!results.empty()) {
        auto& b = results[0];
        printf("\n%s\n", std::string(72,'=').c_str());
        printf("BEST: $%+.2f  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n",
               b.pnl, b.wr*100, b.n, b.lp, b.sp);
        printf("rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f"
               " tp=%.1f cd=%lds mh=%ldm trail=%s\n",
               b.p.rn, b.p.rt, b.p.dm, b.p.st,
               (long)(b.p.sm/1000), b.p.sl, b.p.tp,
               (long)(b.p.cd/1000), (long)(b.p.mh/60000),
               b.p.tr?"Y":"N");

        // Rerun best — print all trades
        Eng eng(b.p);
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr, b.p);
        eng.fin(ticks.back().ms, b.p);
        // (trade detail requires storing trades — skipped for sweep perf)
    }

    return 0;
}