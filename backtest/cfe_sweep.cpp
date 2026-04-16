// cfe_sweep.cpp — exact C++ translation of cfe_sweep.py
// Grid and logic match Python 1:1. No extra params.
#include <algorithm>
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

// ── Params (mirrors Python dict) ─────────────────────────────────────────────
struct P {
    int    rn;          // RSI period (also controls EMA alpha = 2/(rn+1))
    double rt;          // RSI trend threshold (low)
    double rm;          // RSI trend threshold (max)
    double dm;          // drift min
    double st;          // sustained drift threshold
    int64_t sm;         // sustained duration (ms)
    double sl;          // SL = sl * ATR
    double tp;          // TP = tp * SL
    int64_t cd;         // cooldown (ms)
    int64_t mh;         // max hold (ms)
    bool   tr;          // trail enabled
    double ta;          // trail arm (pts)
    double td;          // trail dist (pts)
};

// ── Trade record ─────────────────────────────────────────────────────────────
struct Trade {
    double pnl, mfe;
    bool   il;          // long
    int    h;           // held seconds
    char   r[8];        // reason
};

// ── CFE engine (mirrors Python CFE class exactly) ─────────────────────────────
struct Eng {
    P p;

    // RSI state (mirrors ursi())
    std::deque<double> rg, rl;
    double rpm = 0.0;
    double rc = 50.0, rp = 50.0, rt_val = 0.0;
    bool   rw = false;
    double ra;           // EMA alpha = 2/(rn+1)
    std::deque<double> rm_buf; // recent mid (last 10)

    // drift sustained state
    int    dsd = 0;      // direction: 1, -1, 0
    int64_t dss = 0;     // ms when direction was set

    // position state
    bool   pa = false;   // position active
    bool   pl = false;   // long
    bool   pbe = false;  // breakeven hit
    double pe = 0;       // entry price
    double psl = 0;      // stop loss
    double ptp = 0;      // take profit
    int64_t pts = 0;     // position open ts
    double pm = 0;       // max MFE
    double ps = 0.01;    // size

    // cooldown / adverse block state
    int64_t cd_until = 0;
    int    lcd = 0;      // last close direction
    int64_t lcm = 0;     // last close ms
    bool   ab = false;   // adverse block active
    double lex = 0;      // last exit price
    int    lld = 0;      // last loss direction

    int    wt = 0;       // tick count

    std::vector<Trade> trades;

    explicit Eng(const P& pp) : p(pp), ra(2.0 / (pp.rn + 1)) {}

    // mirrors ursi()
    void ursi(double mid) {
        if (rpm == 0.0) { rpm = mid; return; }
        double c = mid - rpm; rpm = mid;
        rg.push_back(c > 0 ? c : 0.0);
        rl.push_back(c < 0 ? -c : 0.0);
        int n = p.rn;
        if ((int)rg.size() > n) { rg.pop_front(); rl.pop_front(); }
        if ((int)rg.size() >= n) {
            double ag = 0, al = 0;
            for (double x : rg) ag += x;
            ag /= n;
            for (double x : rl) al += x;
            al /= n;
            rp = rc;
            rc = (al == 0.0) ? 100.0 : 100.0 - 100.0 / (1.0 + ag / al);
            double s = rc - rp;
            if (!rw) { rt_val = s; rw = true; }
            else rt_val = s * ra + rt_val * (1.0 - ra);
        }
    }

    // mirrors tick()
    void tick(int64_t ms, double bid, double ask, double drift, double atr) {
        double mid = (bid + ask) / 2.0;
        double sp  = ask - bid;
        ++wt;
        ursi(mid);
        rm_buf.push_back(mid);
        if ((int)rm_buf.size() > 10) rm_buf.pop_front();

        // drift sustained tracking
        if (drift >= p.st) {
            if (dsd != 1) { dsd = 1; dss = ms; }
        } else if (drift <= -p.st) {
            if (dsd != -1) { dsd = -1; dss = ms; }
        } else {
            dsd = 0; dss = 0;
        }
        int64_t dsms = (dsd != 0) ? (ms - dss) : 0;

        // ── manage open position ──────────────────────────────────────────
        if (pa) {
            double mv   = pl ? (mid - pe) : (pe - mid);
            if (mv > pm) pm = mv;
            double eff  = pl ? bid : ask;
            double td_  = std::fabs(ptp - pe);
            if (!pbe && td_ > 0 && mv >= td_ * 0.5) { psl = pe; pbe = true; }
            if (p.tr && mv >= p.ta) {
                double tsl = pl ? (mid - p.td) : (mid + p.td);
                if (pl  && tsl > psl) psl = tsl;
                if (!pl && tsl < psl) psl = tsl;
            }
            if ((pl && bid >= ptp) || (!pl && ask <= ptp)) { close(eff, "TP", ms); return; }
            if ((pl && bid <= psl) || (!pl && ask >= psl)) { close(eff, pbe ? "BE" : "SL", ms); return; }
            if (ms - pts > p.mh) { close(eff, "TO", ms); return; }
            return;
        }

        // ── entry gates ───────────────────────────────────────────────────
        if (wt < 300 || sp > 0.40 || atr < 1.0) return;
        if (ms < cd_until) return;
        if (!rw) return;

        // RSI direction
        int rd = 0;
        if      (rt_val >  p.rt && rt_val <  p.rm) rd =  1;
        else if (rt_val < -p.rt && rt_val > -p.rm) rd = -1;
        if (!rd) return;

        // drift agrees with RSI direction
        if (rd ==  1 && drift <  0.0) return;
        if (rd == -1 && drift >  0.0) return;
        if (std::fabs(drift) < p.dm) return;

        // sustained duration
        if (dsms < p.sm) return;

        // price confirm (last 3 ticks moving with trend)
        if ((int)rm_buf.size() >= 3) {
            double n3  = rm_buf[rm_buf.size() - 3];
            double mv3 = mid - n3;
            if (rd ==  1 && mv3 < -atr * 0.3) return;
            if (rd == -1 && mv3 >  atr * 0.3) return;
        }

        // opposite-direction cooldown (45 s)
        if (lcd != 0 && (ms - lcm) < 45000LL) {
            if (rd != lcd) return;
        }

        // adverse block
        if (ab && lld != 0) {
            double dist = (lld == 1) ? (lex - mid) : (mid - lex);
            bool same = (rd == 1 && lld == 1) || (rd == -1 && lld == -1);
            if (same && dist > atr * 0.4) return;
            else ab = false;
        }

        // ── open position ─────────────────────────────────────────────────
        bool il  = (rd == 1);
        double sl_ = atr * p.sl;
        double tp_ = sl_ * p.tp;
        double e   = il ? ask : bid;
        pa = true; pl = il; pe = e;
        psl = il ? (e - sl_) : (e + sl_);
        ptp = il ? (e + tp_) : (e - tp_);
        pts = ms; pm = 0.0; pbe = false;
        // size: matches Python ps=min(0.10,max(0.01,round(10/(sl*100)/0.01)*0.01))
        double raw = 10.0 / (sl_ * 100.0);
        ps = std::min(0.10, std::max(0.01, std::round(raw / 0.01) * 0.01));
    }

    void close(double ep, const char* reason, int64_t ms) {
        double pp = pl ? (ep - pe) : (pe - ep);
        double pu = pp * ps * 100.0;
        lcd = pl ? 1 : -1;
        lcm = ms;
        if (pu < 0) {
            ab  = true;
            lex = ep;
            lld = pl ? 1 : -1;
            cd_until = ms + p.cd;
        }
        Trade t;
        t.pnl = pu; t.mfe = pm; t.il = pl;
        t.h   = (int)((ms - pts) / 1000);
        std::strncpy(t.r, reason, 7); t.r[7] = '\0';
        trades.push_back(t);
        pa = false;
    }

    void fin(int64_t ms) { if (pa) close(pe, "END", ms); }

    double tot() const { double s = 0; for (auto& t : trades) s += t.pnl; return s; }
    double lp()  const { double s = 0; for (auto& t : trades) s += t.il ? t.pnl : 0; return s; }
    double sp2() const { double s = 0; for (auto& t : trades) s += t.il ? 0 : t.pnl; return s; }
    int    wins()const { int n = 0; for (auto& t : trades) n += (t.pnl > 0); return n; }
    int    n()   const { return (int)trades.size(); }
};

// ── Tick data ─────────────────────────────────────────────────────────────────
struct Tick { int64_t ms; double bid, ask, drift, atr; };

std::vector<Tick> load(const char* path) {
    std::vector<Tick> v;
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return v; }

    std::string line, tok;
    std::getline(f, line);

    // parse header
    int tc=-1, bc=-1, ac=-1, dc=-1, rc=-1, ci=0;
    std::istringstream hss(line);
    while (std::getline(hss, tok, ',')) {
        if (tok == "ts_ms" || tok == "timestamp_ms") tc = ci;
        if (tok == "bid")       bc = ci;
        if (tok == "ask")       ac = ci;
        if (tok == "ewm_drift") dc = ci;
        if (tok == "atr")       rc = ci;
        ++ci;
    }
    if (bc < 0 || ac < 0) { fprintf(stderr, "CSV missing bid/ask columns\n"); return v; }

    double pb = 0, av = 2.0;
    std::deque<double> aw;
    v.reserve(200000);

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> cols;
        std::istringstream ss(line);
        while (std::getline(ss, tok, ',')) cols.push_back(tok);
        int need = std::max({tc, bc, ac});
        if ((int)cols.size() <= need) continue;
        try {
            Tick t;
            t.ms    = (tc >= 0) ? (int64_t)std::stod(cols[tc]) : 0;
            t.bid   = std::stod(cols[bc]);
            t.ask   = std::stod(cols[ac]);
            t.drift = (dc >= 0 && dc < (int)cols.size()) ? std::stod(cols[dc]) : 0.0;
            t.atr   = (rc >= 0 && rc < (int)cols.size()) ? std::stod(cols[rc]) : 0.0;
            if (t.bid <= 0 || t.ask < t.bid) continue;
            // ATR fallback (mirrors Python)
            if (t.atr <= 0) {
                if (pb > 0) {
                    double tr = (t.ask - t.bid) + std::fabs(t.bid - pb);
                    aw.push_back(tr);
                    if ((int)aw.size() > 200) aw.pop_front();
                    if ((int)aw.size() >= 50) {
                        double s = 0; for (double x : aw) s += x;
                        av = s / aw.size() * 14.0;
                    }
                }
                t.atr = av;
            }
            pb = t.bid;
            v.push_back(t);
        } catch (...) {}
    }
    fprintf(stderr, "Loaded %zu ticks\n", v.size());
    return v;
}

// ── Grid (exact match to Python cfe_sweep.py) ─────────────────────────────────
// Python grid:
//   rn:  [20,30]
//   rt:  [2.0,4.0]
//   rm:  [15.0]
//   dm:  [0.3,0.5,0.8]
//   st:  [0.3,0.5]
//   sm:  [10000,20000,30000]   (ms)
//   sl:  [0.4,0.6,0.8]
//   tp:  [2.0,3.0,4.0]
//   cd:  [15000,30000]         (ms)
//   mh:  [300000,600000]       (ms)
//   tr:  [false,true]
//   ta:  [2.0]
//   td:  [1.0]
// total = 2*2*3*2*3*3*3*2*2*2 = 5184

int main(int argc, char* argv[]) {
    if (argc < 2) { puts("Usage: cfe_sweep <csv>"); return 1; }

    auto t0 = std::chrono::steady_clock::now();
    auto ticks = load(argv[1]);
    printf("Loaded %zu ticks\n", ticks.size());
    if (ticks.empty()) return 1;

    const int    rn_v[]  = {20, 30};
    const double rt_v[]  = {2.0, 4.0};
    const double rm_v[]  = {15.0};
    const double dm_v[]  = {0.3, 0.5, 0.8};
    const double st_v[]  = {0.3, 0.5};
    const int64_t sm_v[] = {10000, 20000, 30000};  // ms — matches Python
    const double sl_v[]  = {0.4, 0.6, 0.8};
    const double tp_v[]  = {2.0, 3.0, 4.0};
    const int64_t cd_v[] = {15000, 30000};          // ms — matches Python
    const int64_t mh_v[] = {300000, 600000};         // ms — matches Python
    const bool   tr_v[]  = {false, true};

    long total = 2*2*1*3*2*3*3*3*2*2*2;
    printf("Testing %ld combinations...\n", total);

    struct Res { double pnl, wr, lp, sp; int n; P p; };
    std::vector<Res> results;
    results.reserve(10000);
    long done = 0;

    for (int rn : rn_v)
    for (double rt : rt_v)
    for (double rm_ : rm_v)
    for (double dm : dm_v)
    for (double st : st_v)
    for (int64_t sm : sm_v)
    for (double sl : sl_v)
    for (double tp : tp_v)
    for (int64_t cd : cd_v)
    for (int64_t mh : mh_v)
    for (bool tr : tr_v)
    {
        P pp;
        pp.rn = rn; pp.rt = rt; pp.rm = rm_;
        pp.dm = dm; pp.st = st; pp.sm = sm;
        pp.sl = sl; pp.tp = tp; pp.cd = cd; pp.mh = mh;
        pp.tr = tr; pp.ta = 2.0; pp.td = 1.0;

        Eng eng(pp);
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr);
        eng.fin(ticks.back().ms);

        if (eng.n() >= 3) {
            Res r;
            r.pnl = eng.tot();
            r.wr  = (double)eng.wins() / eng.n();
            r.lp  = eng.lp();
            r.sp  = eng.sp2();
            r.n   = eng.n();
            r.p   = pp;
            results.push_back(r);
        }

        if (++done % 1000 == 0) {
            double best = 0;
            for (auto& r : results) if (r.pnl > best) best = r.pnl;
            printf("  %ld/%ld best=$%+.2f\n", done, total, best);
            fflush(stdout);
        }
    }

    std::sort(results.begin(), results.end(),
              [](const Res& a, const Res& b){ return a.pnl > b.pnl; });

    auto t1 = std::chrono::steady_clock::now();
    printf("\nDone in %.2fs\n",
           std::chrono::duration<double>(t1 - t0).count());

    puts("\n================================================================");
    puts("TOP 20");
    puts("================================================================");
    printf("%-3s %-8s %-6s %-4s %-7s %-7s %-5s %-5s %-5s %-4s %-5s %-3s %-5s\n",
           "#","PnL","WR","N","L$","S$","rsi_t","drift","sl","tp","sm_s","tr","hold_m");

    int show = std::min((int)results.size(), 20);
    for (int i = 0; i < show; ++i) {
        auto& r = results[i];
        printf("%-3d $%+7.2f %5.1f%% %4d $%+6.2f $%+6.2f "
               "%5.1f  %4.2f  %4.2f %4.1f  %4lds  %3s  %4ldm\n",
               i+1, r.pnl, r.wr*100, r.n, r.lp, r.sp,
               r.p.rt, r.p.dm, r.p.sl, r.p.tp,
               (long)(r.p.sm / 1000),
               r.p.tr ? "Y" : "N",
               (long)(r.p.mh / 60000));
    }

    if (!results.empty()) {
        auto& b = results[0];
        printf("\nBEST: $%+.2f  WR=%.1f%%  N=%d  L=$%+.2f  S=$%+.2f\n",
               b.pnl, b.wr*100, b.n, b.lp, b.sp);
        printf("rn=%d rt=%.1f dm=%.2f st=%.2f sm=%lds sl=%.2f tp=%.1f "
               "cd=%ldms mh=%ldms trail=%s\n",
               b.p.rn, b.p.rt, b.p.dm, b.p.st,
               (long)(b.p.sm/1000), b.p.sl, b.p.tp,
               (long)b.p.cd, (long)b.p.mh,
               b.p.tr ? "Y" : "N");

        // rerun best to show trades
        Eng eng(b.p);
        for (auto& tk : ticks)
            eng.tick(tk.ms, tk.bid, tk.ask, tk.drift, tk.atr);
        eng.fin(ticks.back().ms);

        puts("\nAll trades (best config):");
        printf("  %-5s  %-5s  %-8s  %-6s  %-4s  %s\n",
               "Side","Held","PnL","MFE","R","");
        for (auto& t : eng.trades) {
            printf("  %-5s  %4ds  $%+6.2f  %6.3f  %-4s\n",
                   t.il ? "LONG" : "SHORT",
                   t.h, t.pnl, t.mfe, t.r);
        }
    }

    return 0;
}
