// arb_diag.cpp  — Asian Range Breakout diagnostic backtest
// XAUUSD tick data, London session execution
//
// STRATEGY:
//   1. Build Asian range: high and low of 00:00-06:59 UTC each day
//   2. At London open (07:00 UTC), range is locked and ready
//   3. Entry: first 5-minute bar that CLOSES outside the range
//      - Close above Asian high → LONG
//      - Close below Asian low  → SHORT
//   4. Stop loss: opposite side of Asian range (range acts as SL anchor)
//      Minimum SL = ATR-based floor (prevents entries on tiny ranges)
//   5. Trail: 75% lock once 6pt+ in profit (same mechanic proven in GoldFlow)
//   6. Force-close at 12:00 UTC (London close, before NY opens)
//      Optional: allow NY continuation if trailing
//   7. Range validity filter: Asian range must be >= MIN_RANGE_PTS
//      (too-narrow ranges = no real accumulation = fake breakout)
//   8. ATR filter: breakout candle body must be >= ATR_CONFIRM_FRAC * ATR14
//      (confirms momentum, not a wick fakeout)
//
// Build: g++ -O2 -std=c++17 -o arb_diag arb_diag.cpp
// Run:   ./arb_diag ticks.csv [trades_out.csv]

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <iomanip>
#include <deque>

// ─────────────────────────────────────────────
// Tick
// ─────────────────────────────────────────────
struct Tick { uint64_t ts=0; double ask=0, bid=0; };

bool parse_tick(const std::string& line, Tick& t)
{
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!getline(ss,tok,',')) return false;
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

// ─────────────────────────────────────────────
// Time helpers
// ─────────────────────────────────────────────
inline int      utc_hour(uint64_t ts) { return (int)((ts/1000/3600)%24); }
inline int      utc_min(uint64_t ts)  { return (int)((ts/1000/60)%60); }
inline uint64_t utc_day(uint64_t ts)  { return ts/1000/86400; }

// 5-minute bar index (0-based from midnight)
inline int bar5(uint64_t ts) {
    int h = utc_hour(ts);
    int m = utc_min(ts);
    return (h*60+m)/5;
}

inline bool is_asian(uint64_t ts) { int h=utc_hour(ts); return h>=0&&h<=6; }
inline bool is_london(uint64_t ts){ int h=utc_hour(ts); return h>=7&&h<=11; }
// Force-close at 12:00 UTC (configurable)

// ─────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────
static const double MIN_RANGE_PTS    = 5.0;    // Asian range must span >= 5pts or skip day
static const double MAX_RANGE_PTS    = 40.0;   // too-wide range = chaotic day, skip
static const double SL_BUFFER_PTS   = 1.0;    // SL placed this far beyond opposite range edge
static const double ATR_CONFIRM_FRAC = 0.3;    // breakout bar body >= 30% of ATR14 (daily)
static const int    ATR_PERIOD       = 14;     // daily ATR lookback
static const int    BAR_MINUTES      = 5;      // bar size for confirmation
static const double MAX_SPREAD       = 0.40;
static const double COMMISSION_PTS   = 0.0;
static const uint64_t FORCE_CLOSE_UTC_HOUR = 12; // close any open position at 12:00 UTC

// Trail (same mechanic proven in GoldFlow — 100% WR on TRAIL_HIT)
static const bool   TRAIL_ENABLED    = true;
static const double TRAIL_TRIGGER    = 6.0;    // activate trail after 6pt profit
static const double TRAIL_LOCK       = 0.75;   // lock 75% of MFE

// ─────────────────────────────────────────────
// Trade record
// ─────────────────────────────────────────────
enum class ExitReason { NONE, TP_NONE, SL_HIT, TRAIL_HIT, FORCE_CLOSE, TIME_STOP };
static const char* exit_name(ExitReason r) {
    switch(r) {
        case ExitReason::SL_HIT:     return "SL_HIT";
        case ExitReason::TRAIL_HIT:  return "TRAIL_HIT";
        case ExitReason::FORCE_CLOSE:return "FORCE_CLOSE";
        case ExitReason::TIME_STOP:  return "TIME_STOP";
        default:                     return "OTHER";
    }
}

struct TradeRecord {
    int id=0; bool is_long=false;
    double entry=0, exit_price=0, sl=0;
    double spread_open=0, range_hi=0, range_lo=0, range_sz=0;
    double atr_day=0, bar_body=0;
    double mfe=0, mae=0, gross_pnl=0, net_pnl=0;
    int hold_ticks=0; uint64_t hold_ms=0, entry_ts=0;
    ExitReason exit_why=ExitReason::NONE;
};

struct Stats {
    int count=0,wins=0;
    double total_pnl=0,total_mfe=0,total_mae=0,total_hold_ms=0;
    double total_range=0, total_atr=0;
    void add(const TradeRecord& tr){
        count++;total_pnl+=tr.net_pnl;total_mfe+=tr.mfe;total_mae+=tr.mae;
        total_hold_ms+=(double)tr.hold_ms;
        total_range+=tr.range_sz; total_atr+=tr.atr_day;
        if(tr.net_pnl>0)wins++;
    }
    void print(const char* label) const {
        if(!count){std::cout<<"  "<<label<<": 0 trades\n";return;}
        double wr=100.0*wins/count;
        std::cout<<"  "<<std::left<<std::setw(16)<<label
            <<" n="<<std::setw(5)<<count
            <<" WR="<<std::fixed<<std::setprecision(1)<<std::setw(5)<<wr<<"%"
            <<" PnL="<<std::setw(9)<<std::setprecision(1)<<total_pnl*100<<" USD"
            <<" avg="<<std::setw(6)<<std::setprecision(2)<<total_pnl/count<<" pts"
            <<" MFE="<<std::setw(5)<<std::setprecision(1)<<total_mfe/count
            <<" MAE="<<std::setw(5)<<std::setprecision(1)<<total_mae/count
            <<" rng="<<std::setw(5)<<std::setprecision(1)<<total_range/count
            <<" hold="<<std::setprecision(0)<<total_hold_ms/count/1000<<"s\n";
    }
};

// ─────────────────────────────────────────────
// 5-minute bar builder
// ─────────────────────────────────────────────
struct Bar5 {
    uint64_t ts_open=0;
    double open=0, high=0, low=0, close=0;
    bool complete=false;
    int bar_idx=-1;
};

// ─────────────────────────────────────────────
// Daily ATR tracker (based on daily high-low)
// ─────────────────────────────────────────────
struct DailyATR {
    std::deque<double> ranges;
    double day_hi=0, day_lo=0;
    uint64_t cur_day=0;
    double atr=10.0; // default starting ATR

    void update(double mid, uint64_t ts) {
        uint64_t day=utc_day(ts);
        if (day!=cur_day) {
            if (cur_day>0 && day_hi>0) {
                ranges.push_back(day_hi-day_lo);
                if ((int)ranges.size()>ATR_PERIOD) ranges.pop_front();
                double sum=0; for(double r:ranges) sum+=r;
                atr=sum/ranges.size();
            }
            cur_day=day; day_hi=mid; day_lo=mid;
        }
        day_hi=std::max(day_hi,mid);
        day_lo=std::min(day_lo,mid);
    }
};

int main(int argc, char** argv)
{
    if (argc<2){std::cout<<"usage: arb_diag ticks.csv [trades_out.csv]\n";return 0;}
    std::ifstream file(argv[1]);
    if (!file.is_open()){std::cout<<"cannot open: "<<argv[1]<<"\n";return 1;}

    std::ofstream csv_out;
    bool write_csv=(argc>=3);
    if (write_csv){
        csv_out.open(argv[2]);
        csv_out<<"id,is_long,entry_ts,entry,exit,sl,spread_open,"
               <<"range_hi,range_lo,range_sz,atr_day,bar_body,"
               <<"mfe,mae,gross_pnl,net_pnl,hold_ticks,hold_ms,exit_why\n";
    }

    // ── State ──────────────────────────────────────────────
    // Asian range tracking
    double asian_hi=0, asian_lo=1e9;
    bool asian_range_valid=false;
    uint64_t current_day=0;
    bool london_entry_taken=false; // one trade per day

    // Current 5-min bar
    Bar5 cur_bar;
    int last_bar_idx=-1;

    // Position
    bool in_pos=false; bool is_long=false;
    double entry=0, sl=0, trail_sl=0, mfe=0, mae=0;
    bool trail_active=false;
    uint64_t entry_ts=0;
    int pos_ticks=0;
    double spread_open_stored=0;
    double range_hi_stored=0, range_lo_stored=0, range_sz_stored=0;
    double atr_stored=0, bar_body_stored=0;

    DailyATR daily_atr;
    std::vector<TradeRecord> trades;
    TradeRecord cur;
    int trade_id=0;
    uint64_t ticks_total=0, ticks_london=0;

    // Gate counters
    uint64_t cnt_days=0, cnt_days_range_too_small=0, cnt_days_range_too_large=0;
    uint64_t cnt_breakouts_seen=0, cnt_rej_confirm=0, cnt_entered=0;
    uint64_t cnt_already_traded=0;

    std::string line;
    auto t_start=std::chrono::high_resolution_clock::now();

    while(getline(file,line))
    {
        Tick t;
        if(!parse_tick(line,t)) continue;
        ticks_total++;
        double spread=t.ask-t.bid;
        double mid=(t.ask+t.bid)*0.5;

        if(spread>MAX_SPREAD) continue;

        daily_atr.update(mid,t.ts);

        uint64_t day=utc_day(t.ts);

        // ── New day reset ──────────────────────────────────
        if (day!=current_day) {
            // Close any leftover position at day boundary
            if (in_pos) {
                cur.exit_price=is_long?t.bid:t.ask;
                cur.gross_pnl=is_long?cur.exit_price-entry:entry-cur.exit_price;
                cur.net_pnl=cur.gross_pnl-spread_open_stored-COMMISSION_PTS;
                cur.hold_ms=t.ts-entry_ts; cur.exit_why=ExitReason::FORCE_CLOSE;
                cur.mfe=std::max(0.0,mfe); cur.mae=std::max(0.0,mae);
                trades.push_back(cur); in_pos=false; trail_active=false;
            }
            current_day=day;
            asian_hi=0; asian_lo=1e9; asian_range_valid=false;
            london_entry_taken=false;
            cur_bar=Bar5{}; last_bar_idx=-1;
            cnt_days++;
        }

        // ── Build Asian range ──────────────────────────────
        if (is_asian(t.ts)) {
            if (asian_hi==0) asian_hi=mid;
            asian_hi=std::max(asian_hi,mid);
            asian_lo=std::min(asian_lo,mid);
        }

        // ── At London open: validate range ─────────────────
        if (utc_hour(t.ts)==7 && utc_min(t.ts)==0 && !asian_range_valid) {
            double rng=asian_hi-asian_lo;
            if (rng<MIN_RANGE_PTS) { cnt_days_range_too_small++; }
            else if (rng>MAX_RANGE_PTS) { cnt_days_range_too_large++; }
            else { asian_range_valid=true; }
        }

        if (!is_london(t.ts)) {
            // Force-close at FORCE_CLOSE_UTC_HOUR
            if (in_pos && utc_hour(t.ts)==(int)FORCE_CLOSE_UTC_HOUR && utc_min(t.ts)==0) {
                cur.exit_price=is_long?t.bid:t.ask;
                cur.gross_pnl=is_long?cur.exit_price-entry:entry-cur.exit_price;
                cur.net_pnl=cur.gross_pnl-spread_open_stored-COMMISSION_PTS;
                cur.hold_ms=t.ts-entry_ts; cur.exit_why=ExitReason::FORCE_CLOSE;
                cur.mfe=std::max(0.0,mfe); cur.mae=std::max(0.0,mae);
                trades.push_back(cur); in_pos=false; trail_active=false;
            }
            continue;
        }
        ticks_london++;

        // ── Build 5-minute bars ────────────────────────────
        int bidx=bar5(t.ts);
        if (bidx!=last_bar_idx) {
            // bar just completed — process it
            if (cur_bar.complete && asian_range_valid && !london_entry_taken && !in_pos) {
                double body=std::abs(cur_bar.close-cur_bar.open);
                bool broke_hi=(cur_bar.close > asian_hi);
                bool broke_lo=(cur_bar.close < asian_lo);

                if (broke_hi || broke_lo) {
                    cnt_breakouts_seen++;
                    double atr_now=daily_atr.atr;
                    double confirm_threshold=ATR_CONFIRM_FRAC*atr_now;

                    if (body < confirm_threshold) {
                        cnt_rej_confirm++;
                    } else {
                        // ENTER
                        cnt_entered++;
                        london_entry_taken=true;
                        in_pos=true;
                        is_long=broke_hi;
                        entry=is_long?t.ask:t.bid;
                        double range_sz=asian_hi-asian_lo;
                        sl=is_long?(asian_lo-SL_BUFFER_PTS):(asian_hi+SL_BUFFER_PTS);
                        trail_sl=sl;
                        mfe=0; mae=0; trail_active=false;
                        entry_ts=t.ts; pos_ticks=0;
                        spread_open_stored=spread;
                        range_hi_stored=asian_hi; range_lo_stored=asian_lo;
                        range_sz_stored=range_sz;
                        atr_stored=atr_now; bar_body_stored=body;

                        cur=TradeRecord{}; cur.id=++trade_id; cur.is_long=is_long;
                        cur.entry=entry; cur.sl=sl; cur.spread_open=spread;
                        cur.range_hi=asian_hi; cur.range_lo=asian_lo;
                        cur.range_sz=range_sz; cur.atr_day=atr_now;
                        cur.bar_body=body; cur.entry_ts=t.ts;
                        cur.mfe=0; cur.mae=0;
                    }
                }
            }
            // Start new bar
            cur_bar.ts_open=t.ts; cur_bar.open=mid; cur_bar.high=mid;
            cur_bar.low=mid; cur_bar.close=mid; cur_bar.complete=false;
            cur_bar.bar_idx=bidx; last_bar_idx=bidx;
        } else {
            // Update current bar
            cur_bar.high=std::max(cur_bar.high,mid);
            cur_bar.low=std::min(cur_bar.low,mid);
            cur_bar.close=mid;
            cur_bar.complete=true;
        }

        // ── Manage open position ───────────────────────────
        if (!in_pos) continue;

        pos_ticks++; cur.hold_ticks++;
        double exc=is_long?mid-entry:entry-mid;
        if (exc>mfe) mfe=exc;
        if (exc<-mae) mae=-exc;
        cur.mfe=mfe; cur.mae=mae;

        if (TRAIL_ENABLED) {
            if (!trail_active && mfe>=TRAIL_TRIGGER) trail_active=true;
            if (trail_active) {
                double locked=mfe*TRAIL_LOCK;
                double nt=is_long?entry+locked:entry-locked;
                if (is_long) trail_sl=std::max(trail_sl,nt);
                else         trail_sl=std::min(trail_sl,nt);
            }
        }

        ExitReason why=ExitReason::NONE; double exit_px=0;
        if (is_long) {
            double px=t.bid;
            if      (px<=sl)                                   {why=ExitReason::SL_HIT;    exit_px=sl;}
            else if (TRAIL_ENABLED&&trail_active&&px<=trail_sl){why=ExitReason::TRAIL_HIT; exit_px=trail_sl;}
        } else {
            double px=t.ask;
            if      (px>=sl)                                   {why=ExitReason::SL_HIT;    exit_px=sl;}
            else if (TRAIL_ENABLED&&trail_active&&px>=trail_sl){why=ExitReason::TRAIL_HIT; exit_px=trail_sl;}
        }

        if (why!=ExitReason::NONE) {
            cur.exit_price=exit_px;
            cur.gross_pnl=is_long?exit_px-entry:entry-exit_px;
            cur.net_pnl=cur.gross_pnl-spread_open_stored-COMMISSION_PTS;
            cur.hold_ms=t.ts-entry_ts; cur.exit_why=why;
            cur.mfe=std::max(0.0,mfe); cur.mae=std::max(0.0,mae);
            trades.push_back(cur);
            if (write_csv) {
                csv_out<<cur.id<<","<<(cur.is_long?1:0)<<","<<cur.entry_ts<<","
                    <<std::fixed<<std::setprecision(2)
                    <<cur.entry<<","<<cur.exit_price<<","<<cur.sl<<","<<cur.spread_open<<","
                    <<cur.range_hi<<","<<cur.range_lo<<","<<cur.range_sz<<","
                    <<cur.atr_day<<","<<cur.bar_body<<","
                    <<cur.mfe<<","<<cur.mae<<","
                    <<cur.gross_pnl<<","<<cur.net_pnl<<","
                    <<cur.hold_ticks<<","<<cur.hold_ms<<","<<exit_name(why)<<"\n";
            }
            in_pos=false; trail_active=false;
        }
    }

    // Close any remaining position
    if (in_pos) {
        cur.exit_price=is_long?0:0; // mark as unclosed
        cur.exit_why=ExitReason::FORCE_CLOSE;
        cur.mfe=std::max(0.0,mfe); cur.mae=std::max(0.0,mae);
        trades.push_back(cur);
    }

    auto t_end=std::chrono::high_resolution_clock::now();
    double runtime=std::chrono::duration<double>(t_end-t_start).count();

    // ── Aggregate ──────────────────────────────────────────
    Stats s_total, s_sl, s_trail, s_force;
    for (const auto& tr:trades) {
        s_total.add(tr);
        switch(tr.exit_why) {
            case ExitReason::SL_HIT:     s_sl.add(tr);    break;
            case ExitReason::TRAIL_HIT:  s_trail.add(tr); break;
            case ExitReason::FORCE_CLOSE:s_force.add(tr); break;
            default: break;
        }
    }

    // Long vs Short
    Stats s_long, s_short;
    for (const auto& tr:trades) {
        if (tr.is_long) s_long.add(tr);
        else            s_short.add(tr);
    }

    // Range size buckets
    auto range_bucket = [&](double lo, double hi, const char* lbl) {
        int n=0,w=0; double pnl=0;
        for (const auto& tr:trades)
            if (tr.range_sz>=lo&&tr.range_sz<hi){n++;pnl+=tr.net_pnl;if(tr.net_pnl>0)w++;}
        if (n>0) std::cout<<"  "<<std::left<<std::setw(14)<<lbl
            <<" n="<<std::setw(5)<<n<<" WR="<<std::fixed<<std::setprecision(1)<<100.0*w/n
            <<"% PnL="<<std::setprecision(1)<<pnl*100<<" USD\n";
    };

    // MFE distribution
    auto mfe_bucket = [&](double lo, double hi, const char* lbl) {
        int n=0,w=0; double pnl=0;
        for (const auto& tr:trades)
            if (tr.mfe>=lo&&tr.mfe<hi){n++;pnl+=tr.net_pnl;if(tr.net_pnl>0)w++;}
        if (n>0) std::cout<<"  "<<std::left<<std::setw(12)<<lbl
            <<" n="<<std::setw(5)<<n<<" WR="<<std::fixed<<std::setprecision(1)<<100.0*w/n
            <<"% PnL="<<std::setprecision(1)<<pnl*100<<" USD\n";
    };

    // ── Report ─────────────────────────────────────────────
    std::cout<<"\n══════════════════════════════════════════════════════════════\n";
    std::cout<<"  Asian Range Breakout (ARB)  v1  [LONDON EXECUTION]\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  Ticks total   : "<<ticks_total<<"\n";
    std::cout<<"  Ticks London  : "<<ticks_london<<"\n";
    std::cout<<"  Runtime       : "<<std::fixed<<std::setprecision(2)<<runtime<<" s\n\n";
    std::cout<<"  Parameters:\n";
    std::cout<<"    Asian range:  00:00-06:59 UTC  MIN="<<MIN_RANGE_PTS<<"pts  MAX="<<MAX_RANGE_PTS<<"pts\n";
    std::cout<<"    Entry:        5-min bar CLOSE outside range\n";
    std::cout<<"    Confirm:      bar body >= "<<ATR_CONFIRM_FRAC<<"x ATR14\n";
    std::cout<<"    SL:           opposite range edge + "<<SL_BUFFER_PTS<<"pt buffer\n";
    std::cout<<"    Force-close:  "<<FORCE_CLOSE_UTC_HOUR<<":00 UTC\n";
    std::cout<<"    TRAIL:        "<<(TRAIL_ENABLED?"ON":"OFF");
    if (TRAIL_ENABLED) std::cout<<" trigger="<<TRAIL_TRIGGER<<"pts lock="<<(int)(TRAIL_LOCK*100)<<"%";
    std::cout<<"\n\n";

    std::cout<<"── Day/Entry Funnel ──────────────────────────────────────────\n";
    std::cout<<"  Trading days seen      : "<<cnt_days<<"\n";
    std::cout<<"  Range too small (<"<<MIN_RANGE_PTS<<"pt): "<<cnt_days_range_too_small<<"\n";
    std::cout<<"  Range too large (>"<<MAX_RANGE_PTS<<"pt): "<<cnt_days_range_too_large<<"\n";
    uint64_t valid_days=cnt_days-cnt_days_range_too_small-cnt_days_range_too_large;
    std::cout<<"  Valid range days       : "<<valid_days<<"\n";
    std::cout<<"  Breakout bars seen     : "<<cnt_breakouts_seen<<"\n";
    std::cout<<"  Rej: confirm too weak  : "<<cnt_rej_confirm<<"\n";
    std::cout<<"  ENTRIES                : "<<cnt_entered<<"\n";
    std::cout<<"  Trades/year (approx)   : "<<std::fixed<<std::setprecision(0)<<cnt_entered/2.0<<"\n\n";

    std::cout<<"── By Exit Reason ────────────────────────────────────────────\n";
    s_total.print("TOTAL");
    s_sl.print("SL_HIT");
    s_trail.print("TRAIL_HIT");
    s_force.print("FORCE_CLOSE");
    std::cout<<"\n";

    std::cout<<"── Long vs Short ─────────────────────────────────────────────\n";
    s_long.print("LONG");
    s_short.print("SHORT");
    std::cout<<"\n";

    std::cout<<"── Asian Range Size at Entry ─────────────────────────────────\n";
    range_bucket(5,8,"5-8pt");
    range_bucket(8,12,"8-12pt");
    range_bucket(12,16,"12-16pt");
    range_bucket(16,20,"16-20pt");
    range_bucket(20,40,"20-40pt");
    std::cout<<"\n";

    std::cout<<"── MFE Distribution ──────────────────────────────────────────\n";
    mfe_bucket(0,2,"MFE<2pts");
    mfe_bucket(2,4,"MFE 2-4pts");
    mfe_bucket(4,6,"MFE 4-6pts");
    mfe_bucket(6,10,"MFE 6-10pts");
    mfe_bucket(10,20,"MFE 10-20pts");
    mfe_bucket(20,999,"MFE>20pts");
    std::cout<<"\n";

    // SL_HIT breakdown
    if (s_sl.count>0) {
        std::cout<<"── SL_HIT: MFE Groups ────────────────────────────────────────\n";
        for (auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
            {"MFE<1pt",{0,1}},{"MFE 1-3pt",{1,3}},{"MFE 3-6pt",{3,6}},{"MFE>6pt",{6,999}}}) {
            int n=0; double pnl=0, rng_sum=0;
            for (const auto& tr:trades)
                if (tr.exit_why==ExitReason::SL_HIT&&tr.mfe>=rng.first&&tr.mfe<rng.second)
                    {n++;pnl+=tr.net_pnl;rng_sum+=tr.range_sz;}
            if (n>0) std::cout<<"  "<<std::left<<std::setw(14)<<lbl
                <<" n="<<n<<" PnL="<<std::setprecision(1)<<pnl*100
                <<" avg_range="<<std::setprecision(1)<<rng_sum/n<<"\n";
        }
        std::cout<<"\n";
    }

    // FORCE_CLOSE: what MFE did we leave behind?
    if (s_force.count>0) {
        std::cout<<"── FORCE_CLOSE: PnL distribution ────────────────────────────\n";
        int pos=0,neg=0,flat=0;
        double pos_pnl=0,neg_pnl=0;
        for (const auto& tr:trades) {
            if (tr.exit_why!=ExitReason::FORCE_CLOSE) continue;
            if (tr.net_pnl>0.01){pos++;pos_pnl+=tr.net_pnl*100;}
            else if (tr.net_pnl<-0.01){neg++;neg_pnl+=tr.net_pnl*100;}
            else flat++;
        }
        std::cout<<"  Profitable at close: n="<<pos<<" total=$"<<std::setprecision(1)<<pos_pnl<<"\n";
        std::cout<<"  Loss at close:       n="<<neg<<" total=$"<<neg_pnl<<"\n";
        std::cout<<"  Flat at close:       n="<<flat<<"\n\n";
    }

    double total_usd=0; int total_wins=0;
    for (const auto& tr:trades){total_usd+=tr.net_pnl*100.0;if(tr.net_pnl>0)total_wins++;}
    int total_n=(int)trades.size();
    double wr=total_n>0?100.0*total_wins/total_n:0;
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  RESULT: "<<total_n<<" trades | WR="<<std::fixed<<std::setprecision(1)<<wr
        <<"% | PnL="<<std::setprecision(0)<<total_usd<<" USD\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n\n";
    if (write_csv) std::cout<<"  CSV: "<<argv[2]<<"\n\n";
    return 0;
}
