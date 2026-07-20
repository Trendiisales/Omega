// gold_ls_bt.cpp — SIMPLE long/short gold structure probe on honest fills.
// -----------------------------------------------------------------------------
// Uses the salvaged, edge-neutral plumbing in gold_ls_harness.hpp. Streams a
// tick/quote CSV once, aggregates to N-minute bars, and runs LONG and SHORT
// engines simultaneously & independently for ONE structure. Records raw
// (pre-cost) fill LEVELS per trade; cost is applied analytically afterwards so
// 1x and 2x are exact. Prints machine-readable RESULT lines for
// {long,short} x {1x,2x} x {all,first-half,second-half}.
//
// Structures (each tested LONG and SHORT separately):
//   DONCH : Donchian(N) breakout trend, ATR stop + opposite-Donchian trail.
//   EMA   : EMA(fast/slow) regime cross trend, ATR stop + chandelier + opp-cross.
//   MR    : ATR-band mean reversion, ATR stop + limit target at the mean.
//   ORB   : NY session opening-range breakout (London omitted for tractability).
//
// Causality: signal on COMPLETED bar b -> execute NEXT bar; management of an
// open position on bar b uses only indicator state through b-1; the protective
// stop is checked BEFORE the favourable excursion updates within the bar.
//
// Cost-viability entry filter (OmegaCostGuard analog): a trade is only taken if
// the structure's expected reward (in bp) >= viab_mult * base RT cost (5bp).
// -----------------------------------------------------------------------------
#include "gold_ls_harness.hpp"

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace goldls;

enum class Mode { DONCH, EMA, MR, ORB };

struct Params {
    Mode mode = Mode::DONCH;
    int   bar_min      = 5;
    int   atr_p        = 14;
    // DONCH
    int   don_n        = 20;
    int   don_exit_n   = 10;
    double sl_atr      = 2.0;
    double trail_atr   = 0.0;    // chandelier; 0 = off
    double tp_atr      = 0.0;    // 0 = no fixed TP
    int   time_bars    = 0;      // 0 = off
    // EMA
    int   ema_fast     = 20;
    int   ema_slow     = 50;
    // MR
    int   mean_p       = 50;
    double band_k      = 2.5;
    // ORB
    int   or_min       = 30;
    // cost / viability
    double base_cost_bp = 5.0;
    double viab_mult    = 1.5;
    double viab_reward_atr = 3.0; // expected reward (ATR mult) for the cost gate
    // bull-regime gate: only arm LONG when close > SMA(bull_sma) of bar closes,
    // only arm SHORT when close < SMA. 0 = gate off (ungated behaviour).
    int   bull_sma      = 0;
    // shipped-gate replica: block long in sustained-bear per omega::gold_regime().
    bool  regime_bear   = false;
    // read the input as an OHLC BAR csv (ts,o,h,l,c) instead of a tick/quote file.
    // Bar files must NOT go through MinuteCsvReader (it tick-collapses each row to
    // o=h=l=c=close, destroying the high/low the Donchian/stops need).
    bool  barcsv        = false;
};

// Rolling simple moving average of the last N values.
struct RollSMA {
    int n = 0; std::deque<double> q; double sum = 0;
    explicit RollSMA(int p) : n(p) {}
    double update(double x) { q.push_back(x); sum += x; if ((int)q.size() > n) { sum -= q.front(); q.pop_front(); } return value(); }
    [[nodiscard]] bool ready() const { return n > 0 && (int)q.size() >= n; }
    [[nodiscard]] double value() const { return q.empty() ? 0.0 : sum / (double)q.size(); }
};

// Faithful replica of the SHIPPED omega::gold_regime() sustained-bear gate
// (include/RegimeState.hpp). Fed H1 closes; blocks longs when
//   close < EMA200 AND EMA200 < EMA200[PERSIST bars ago] AND EMA50 < EMA200.
// EMA_SLOW=200, EMA_FAST=50, PERSIST=100 = the IBS-validated live defaults.
// Self-buckets 1m bars into H1 (BAR_SECS=3600), same convention as the engine.
struct H1RegimeGate {
    int EMA_SLOW=200, EMA_FAST=50, PERSIST=100; int64_t BAR_SECS=3600;
    std::deque<double> emaS_hist; double emaS=0, emaF=0; bool have=false; int bars=0;
    double last_close=0; bool bear=false, bull=false;
    int64_t cur_bucket=-1; double acc_close=0; int acc_n=0;
    void on_h1_close(double c) {
        const double kS=2.0/(EMA_SLOW+1), kF=2.0/(EMA_FAST+1);
        if (!have) { emaS=c; emaF=c; have=true; } else { emaS+=kS*(c-emaS); emaF+=kF*(c-emaF); }
        emaS_hist.push_back(emaS); while ((int)emaS_hist.size()>PERSIST+1) emaS_hist.pop_front();
        last_close=c; ++bars; bear=bull=false;
        if (bars>=EMA_SLOW+PERSIST && (int)emaS_hist.size()>=PERSIST+1) {
            const double old=emaS_hist.front();
            if (c<emaS && emaS<old && emaF<emaS) bear=true;
            else if (c>emaS && emaS>old && emaF>emaS) bull=true;
        }
    }
    // feed a 1m bar; completes H1 buckets internally.
    void on_minute(const MinuteBar& m) {
        const int64_t b = m.t / BAR_SECS;
        if (acc_n==0) { cur_bucket=b; acc_close=m.close; acc_n=1; }
        else if (b==cur_bucket) { acc_close=m.close; ++acc_n; }
        else { on_h1_close(acc_close); cur_bucket=b; acc_close=m.close; acc_n=1; }
    }
    [[nodiscard]] bool long_blocked() const { return bear; }
    [[nodiscard]] bool short_blocked() const { return bull; }
    [[nodiscard]] bool warm() const { return bars>=EMA_SLOW+PERSIST; }
};

struct Trade {
    bool   is_long = true;
    double entry_raw = 0;  // pre-cost fill level
    double exit_raw  = 0;  // pre-cost exit level
    double risk_px   = 0;  // stop distance at entry (for R)
    int64_t entry_ts = 0;
};

// ---- one-directional position engine ----
struct DirEngine {
    bool is_long = true;
    const Params* p = nullptr;
    std::vector<Trade>* out = nullptr;

    bool in_pos = false;
    bool just_entered = false;
    double entry_raw=0, stop_level=0, tp_level=0, risk_px=0;
    double hh_since=0, ll_since=0; // extremes since entry
    int    bars_held=0;
    int64_t entry_ts=0;

    // pending entry armed from previous bar
    bool   pend = false;
    bool   pend_stop = false;   // true = stop-order at level; false = market-on-open
    double pend_level = 0;      // trigger level (stop-order)
    double pend_tp_ref = 0;     // for MR: fixed target reference (mean)

    void close(double exit_raw) {
        out->push_back(Trade{is_long, entry_raw, exit_raw, risk_px, entry_ts});
        in_pos = false;
    }

    // Manage an open position on bar b, using ctx levels through b-1.
    // Returns true if closed this bar.
    void manage(const Bar& b, double atr, double ex_trail /*opp donch trail level or NAN*/) {
        ++bars_held;
        // 1) protective stop (checked before favourable excursion)
        double sl = stop_level;
        if (std::isfinite(ex_trail)) sl = is_long ? std::max(sl, ex_trail) : std::min(sl, ex_trail);
        if (p->trail_atr > 0.0 && atr > 0.0) {
            const double ch = is_long ? hh_since - p->trail_atr*atr : ll_since + p->trail_atr*atr;
            sl = is_long ? std::max(sl, ch) : std::min(sl, ch);
        }
        stop_level = sl;
        if (is_long ? (b.low <= sl) : (b.high >= sl)) {
            // gap-through booked at the REAL price: min(open,stop) long / max short
            const double raw = is_long ? std::min(b.open, sl) : std::max(b.open, sl);
            close(raw); return;
        }
        // 2) limit target (MR / fixed TP)
        if (tp_level > 0.0) {
            if (is_long ? (b.high >= tp_level) : (b.low <= tp_level)) { close(tp_level); return; }
        }
        // 3) time stop -> market at close
        if (p->time_bars > 0 && bars_held >= p->time_bars) { close(b.close); return; }
        // 4) favourable excursion update (AFTER stop check)
        hh_since = std::max(hh_since, b.high);
        ll_since = std::min(ll_since, b.low);
    }

    // Try to execute a pending entry on bar b. Returns true if entered.
    // reward_bp = expected reward for the cost-viability gate.
    void execute(const Bar& b, double atr, double reward_bp) {
        if (!pend) return;
        // cost-viability gate (base cost, 1x) — refuse trades that can't cover cost
        if (reward_bp < p->viab_mult * p->base_cost_bp) { pend=false; return; }
        double raw;
        if (pend_stop) {
            const bool trig = is_long ? (b.high >= pend_level) : (b.low <= pend_level);
            if (!trig) { pend=false; return; }
            raw = is_long ? std::max(b.open, pend_level) : std::min(b.open, pend_level);
        } else {
            raw = b.open; // market-on-open
        }
        entry_raw = raw;
        risk_px   = p->sl_atr * atr;
        stop_level = is_long ? raw - risk_px : raw + risk_px;
        tp_level   = 0.0;
        if (p->mode == Mode::MR) {
            tp_level = pend_tp_ref; // revert to the mean
        } else if (p->tp_atr > 0.0) {
            tp_level = is_long ? raw + p->tp_atr*atr : raw - p->tp_atr*atr;
        }
        hh_since = b.high; ll_since = b.low;
        bars_held = 0;
        entry_ts = b.start;
        in_pos = true; just_entered = true;
        pend = false;
    }
};

struct Ctx {
    double atr=0;
    double don_hi=0, don_lo=0, ex_hi=0, ex_lo=0;   // DONCH
    double ema_f=0, ema_s=0; bool cross_up=false, cross_down=false; // EMA
    double mean=0; bool below_lo=false, above_hi=false; // MR
    // ORB
    bool orb_armed=false, orb_flat=false; double orb_hi=0, orb_lo=0;
};

int main(int argc, char** argv) {
    Params p;
    std::string file;
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        auto val = [&](const char* def)->std::string{ return (i+1<argc)? argv[++i] : def; };
        if      (a=="--file")  file = val("");
        else if (a=="--mode")  { std::string m=val("DONCH");
            p.mode = (m=="EMA")?Mode::EMA : (m=="MR")?Mode::MR : (m=="ORB")?Mode::ORB : Mode::DONCH; }
        else if (a=="--bar")   p.bar_min = std::atoi(val("5").c_str());
        else if (a=="--atr")   p.atr_p = std::atoi(val("14").c_str());
        else if (a=="--don_n") p.don_n = std::atoi(val("20").c_str());
        else if (a=="--don_exit_n") p.don_exit_n = std::atoi(val("10").c_str());
        else if (a=="--sl_atr")p.sl_atr = std::atof(val("2.0").c_str());
        else if (a=="--trail_atr") p.trail_atr = std::atof(val("0").c_str());
        else if (a=="--tp_atr")p.tp_atr = std::atof(val("0").c_str());
        else if (a=="--time_bars") p.time_bars = std::atoi(val("0").c_str());
        else if (a=="--ema_fast") p.ema_fast = std::atoi(val("20").c_str());
        else if (a=="--ema_slow") p.ema_slow = std::atoi(val("50").c_str());
        else if (a=="--mean_p") p.mean_p = std::atoi(val("50").c_str());
        else if (a=="--band_k") p.band_k = std::atof(val("2.5").c_str());
        else if (a=="--or_min") p.or_min = std::atoi(val("30").c_str());
        else if (a=="--viab_mult") p.viab_mult = std::atof(val("1.5").c_str());
        else if (a=="--viab_reward_atr") p.viab_reward_atr = std::atof(val("3.0").c_str());
        else if (a=="--bull_sma") p.bull_sma = std::atoi(val("0").c_str());
        else if (a=="--regime_bear") p.regime_bear = (std::atoi(val("1").c_str())!=0);
        else if (a=="--barcsv") p.barcsv = true;
    }
    if (file.empty()) { std::fprintf(stderr, "need --file\n"); return 2; }

    BarAggregator agg(p.bar_min);
    ATR atr(p.atr_p);
    EMA ema_f(p.ema_fast), ema_s(p.ema_slow), ema_mean(p.mean_p);
    RollSMA sma_ind(std::max(1, p.bull_sma));
    H1RegimeGate regime;

    std::deque<double> hi, lo;   // completed-bar highs/lows for Donchian
    const int maxwin = std::max({p.don_n, p.don_exit_n, 1});

    std::vector<Trade> trades; // long+short interleaved (is_long flag distinguishes)
    DirEngine eng_l, eng_s;
    eng_l.is_long=true;  eng_l.p=&p; eng_l.out=&trades;
    eng_s.is_long=false; eng_s.p=&p; eng_s.out=&trades;

    // prev-snapshot indicator state (through b-1) for causal management/entry
    double prev_atr=0, prev_don_hi=0, prev_don_lo=0, prev_ex_hi=0, prev_ex_lo=0;
    double prev_ema_f=0, prev_ema_s=0, prev_mean=0;
    bool   have_prev_ema=false; double last_ema_f=0, last_ema_s=0; bool ema_prev_up=false, have_ema_dir=false;

    // ORB session state (NY only)
    int64_t orb_day = -1; double orb_hi=0, orb_lo=0; bool orb_have=false, orb_window_done=false;
    int orb_entries_l=0, orb_entries_s=0;

    int64_t first_ts=0, last_ts=0; bool have_first=false;
    uint64_t nbars=0;

    MinuteBar m;
    auto process_bar = [&](const Bar& b) {
        if (!b.valid()) return;
        ++nbars;
        if (!have_first){ first_ts=b.start; have_first=true; }
        last_ts = b.start;

        // ---- ctx snapshot as of b-1 (before folding b) ----
        Ctx ctx;
        ctx.atr = prev_atr;
        ctx.don_hi=prev_don_hi; ctx.don_lo=prev_don_lo; ctx.ex_hi=prev_ex_hi; ctx.ex_lo=prev_ex_lo;
        ctx.ema_f=prev_ema_f; ctx.ema_s=prev_ema_s; ctx.mean=prev_mean;

        const bool ind_ready = atr.ready() && (int)hi.size() >= p.don_n;

        // ORB session context (uses bar b's clock; window build folds b so handled below)
        LocalClock nyc = new_york_clock(b.start);
        const int ny_open = 8*60;             // 08:00 local
        const int ny_or_end = ny_open + p.or_min;
        const int ny_close = 16*60;           // 16:00 local flatten
        const bool ny_weekday = (nyc.weekday>=1 && nyc.weekday<=5);
        int64_t day_idx = b.start / kSecPerDay + (new_york_dst(b.start)? -1:-1); // rough day key; use nyc-based below
        // day key from NY civil day:
        {
            CivilDateTime cc = utc_to_civil(b.start + (new_york_dst(b.start)? -4*3600 : -5*3600));
            day_idx = days_from_civil(cc.year, cc.month, cc.day);
        }
        if (p.mode==Mode::ORB) {
            if (day_idx != orb_day) { orb_day=day_idx; orb_have=false; orb_window_done=false; orb_hi=0; orb_lo=0; orb_entries_l=0; orb_entries_s=0; }
        }

        // ---------- MANAGE / EXECUTE on bar b (using ctx through b-1) ----------
        // opposite-Donchian trailing level for DONCH exits:
        double trail_l = std::numeric_limits<double>::quiet_NaN();
        double trail_s = std::numeric_limits<double>::quiet_NaN();
        if (p.mode==Mode::DONCH && p.don_exit_n>0 && (int)lo.size()>=p.don_exit_n) {
            trail_l = ctx.ex_lo; // long trails on lowest-low(exitN)
            trail_s = ctx.ex_hi;
        }

        auto step_dir = [&](DirEngine& e, double trail){
            e.just_entered = false;
            if (e.in_pos) { e.manage(b, ctx.atr, trail); }
            else if (e.pend) {
                double reward_bp = 0.0;
                if (ctx.atr>0 && b.open>0) {
                    if (p.mode==Mode::MR)
                        reward_bp = std::fabs(b.open - e.pend_tp_ref)/b.open*kBp;
                    else
                        reward_bp = p.viab_reward_atr*ctx.atr / b.open * kBp;
                }
                e.execute(b, ctx.atr, reward_bp);
            }
        };
        // ORB forced flatten at session close
        if (p.mode==Mode::ORB && ny_weekday && nyc.minute_of_day >= ny_close) {
            if (eng_l.in_pos) eng_l.close(b.open);
            if (eng_s.in_pos) eng_s.close(b.open);
            eng_l.pend=false; eng_s.pend=false;
        } else {
            step_dir(eng_l, trail_l);
            step_dir(eng_s, trail_s);
        }

        // ---------- fold bar b into indicators ----------
        double a = atr.update(b);
        double ef = ema_f.update(b.close);
        double es = ema_s.update(b.close);
        double mn = ema_mean.update(b.close);
        double sm = sma_ind.update(b.close);
        (void)sm;
        hi.push_back(b.high); lo.push_back(b.low);
        while ((int)hi.size() > maxwin) hi.pop_front();
        while ((int)lo.size() > maxwin) lo.pop_front();

        // recompute donchian over the freshest window (through b)
        double dhi=0,dlo=1e18,ehi=0,elo=1e18;
        {
            int nH = (int)hi.size();
            for (int k = std::max(0,nH-p.don_n); k<nH; ++k){ dhi=std::max(dhi,hi[k]); dlo=std::min(dlo,lo[k]); }
            for (int k = std::max(0,nH-p.don_exit_n); k<nH; ++k){ ehi=std::max(ehi,hi[k]); elo=std::min(elo,lo[k]); }
        }

        // ORB window build (fold b)
        if (p.mode==Mode::ORB && ny_weekday) {
            const int mod = nyc.minute_of_day;
            if (mod>=ny_open && mod<ny_or_end) {
                if (!orb_have){ orb_hi=b.high; orb_lo=b.low; orb_have=true; }
                else { orb_hi=std::max(orb_hi,b.high); orb_lo=std::min(orb_lo,b.low); }
            } else if (mod>=ny_or_end && orb_have) {
                orb_window_done=true;
            }
        }

        // ---------- ARM pending for NEXT bar from COMPLETED bar b ----------
        auto arm = [&](DirEngine& e){
            if (e.in_pos || e.just_entered) { e.pend=false; return; }
            e.pend=false;
            if (!ind_ready) return;
            // bull-regime gate: long only when close>SMA, short only when close<SMA.
            if (p.bull_sma > 0) {
                if (!sma_ind.ready()) return;               // gate cold -> no entry
                const bool bull = b.close > sm;
                if (e.is_long && !bull) return;
                if (!e.is_long && bull) return;
            }
            if (p.regime_bear) {
                if (e.is_long && regime.long_blocked()) return;   // sustained-bear: no new long
                if (!e.is_long && regime.short_blocked()) return; // sustained-bull: no new short
            }
            if (p.mode==Mode::DONCH) {
                e.pend=true; e.pend_stop=true;
                e.pend_level = e.is_long ? dhi : dlo;
            } else if (p.mode==Mode::EMA) {
                // cross detection needs prior relationship
                if (!have_ema_dir) return;
                const bool up_now = ef>es;
                const bool cross_up   =  up_now && !ema_prev_up;
                const bool cross_down = !up_now &&  ema_prev_up;
                if (e.is_long && cross_up)  { e.pend=true; e.pend_stop=false; }
                if (!e.is_long && cross_down){ e.pend=true; e.pend_stop=false; }
                // opposite-cross exit for an open position handled below
            } else if (p.mode==Mode::MR) {
                if (!ema_mean.ready() || a<=0) return;
                const double lower = mn - p.band_k*a;
                const double upper = mn + p.band_k*a;
                if (e.is_long && b.close < lower) { e.pend=true; e.pend_stop=false; e.pend_tp_ref=mn; }
                if (!e.is_long && b.close > upper){ e.pend=true; e.pend_stop=false; e.pend_tp_ref=mn; }
            } else if (p.mode==Mode::ORB) {
                if (ny_weekday && orb_window_done && nyc.minute_of_day < ny_close) {
                    if (e.is_long && orb_entries_l==0){ e.pend=true; e.pend_stop=true; e.pend_level=orb_hi; }
                    if (!e.is_long && orb_entries_s==0){ e.pend=true; e.pend_stop=true; e.pend_level=orb_lo; }
                }
            }
        };
        // EMA opposite-cross exit: mark for market close next bar (simple: close at this bar's close)
        if (p.mode==Mode::EMA && have_ema_dir) {
            const bool up_now = ef>es;
            if (eng_l.in_pos && !up_now && ema_prev_up) eng_l.close(b.close);
            if (eng_s.in_pos &&  up_now && !ema_prev_up) eng_s.close(b.close);
        }
        arm(eng_l); arm(eng_s);
        // track ORB entries taken (so one/session)
        if (p.mode==Mode::ORB) {
            if (eng_l.just_entered) orb_entries_l++;
            if (eng_s.just_entered) orb_entries_s++;
        }

        // advance prev snapshots to include b
        prev_atr=a; prev_ema_f=ef; prev_ema_s=es; prev_mean=mn;
        prev_don_hi=dhi; prev_don_lo=(dlo>=1e17?0:dlo); prev_ex_hi=ehi; prev_ex_lo=(elo>=1e17?0:elo);
        if (ema_f.ready() && ema_s.ready()) { ema_prev_up = ef>es; have_ema_dir=true; }
        (void)have_prev_ema; (void)last_ema_f; (void)last_ema_s; (void)ny_or_end;
    };

    if (p.barcsv) {
        MinuteBarCsvReader reader(file);   // ts,o,h,l,c bar rows -> MinuteBars (OHLC preserved)
        while (reader.next(m)) { regime.on_minute(m); auto done = agg.update(m); if (done) process_bar(*done); }
    } else {
        MinuteCsvReader reader(file);      // tick/quote -> 1m aggregation
        while (reader.next(m)) { regime.on_minute(m); auto done = agg.update(m); if (done) process_bar(*done); }
    }
    if (auto f = agg.flush()) process_bar(*f);

    // ---------------- STATS ----------------
    const int64_t mid_ts = have_first ? (first_ts + (last_ts-first_ts)/2) : 0;
    HonestBook book;

    auto emit = [&](bool is_long, double cost_mult, int split /*0 all,1 h1,2 h2*/){
        book.cost_bp = p.base_cost_bp * cost_mult;
        int n=0, win=0; double net=0, gw=0, gl=0, sumR=0; double eq=0, peak=0, dd=0;
        for (const auto& t : trades) {
            if (t.is_long != is_long) continue;
            if (split==1 && t.entry_ts >= mid_ts) continue;
            if (split==2 && t.entry_ts <  mid_ts) continue;
            const int sign = t.is_long?1:-1;
            const double ef = t.is_long ? t.entry_raw + book.half_px(t.entry_raw)
                                        : t.entry_raw - book.half_px(t.entry_raw);
            const double xf = t.is_long ? t.exit_raw  - book.half_px(t.exit_raw)
                                        : t.exit_raw  + book.half_px(t.exit_raw);
            const double ret_bp = sign * (xf - ef) / ef * kBp;
            ++n; net += ret_bp;
            if (ret_bp>0){ ++win; gw+=ret_bp; } else gl += -ret_bp;
            if (t.risk_px>0) sumR += (sign*(xf-ef)) / t.risk_px;
            eq += ret_bp; peak=std::max(peak,eq); dd=std::min(dd, eq-peak);
        }
        const double pf = gl>1e-9 ? gw/gl : (gw>0?999.0:0.0);
        const double avgR = n>0 ? sumR/n : 0.0;
        std::printf("RESULT|mode=%s|dir=%s|cost=%gx|split=%s|n=%d|win=%d|net_bp=%.1f|pf=%.2f|avgR=%.2f|dd_bp=%.1f|gw=%.1f|gl=%.1f\n",
            (p.mode==Mode::DONCH?"DONCH":p.mode==Mode::EMA?"EMA":p.mode==Mode::MR?"MR":"ORB"),
            is_long?"long":"short",
            cost_mult, (split==0?"all":split==1?"h1":"h2"),
            n, win, net, pf, avgR, dd, gw, gl);
    };
    for (bool L : {true,false})
        for (double c : {1.0,2.0})
            for (int s : {0,1,2})
                emit(L,c,s);

    std::fprintf(stderr, "[gold_ls_bt] file=%s mode=%s bars=%llu span=%lld..%lld trades=%zu\n",
        file.c_str(),
        (p.mode==Mode::DONCH?"DONCH":p.mode==Mode::EMA?"EMA":p.mode==Mode::MR?"MR":"ORB"),
        (unsigned long long)nbars, (long long)first_ts, (long long)last_ts, trades.size());
    return 0;
}
