// goldflow_diag.cpp
// GoldFlow GATE COUNTER — instruments every entry rejection to diagnose low trade frequency
// Build: g++ -O2 -std=c++17 -o goldflow_diag goldflow_diag.cpp
// Run:   ./goldflow_diag ticks.csv
//
// This version counts every tick that passes impulse detection and then
// tallies which gate kills the entry. Output shows the rejection funnel.
// Use this to find which single gate is most restrictive, then relax it.
//
// Base: v27 params (London only, best clean result)
// No pb dead zone — v28 proved that filter is unstable.

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

struct Tick { uint64_t ts=0; double ask=0, bid=0; };

bool parse_tick(const std::string& line, Tick& t)
{
    if (line.empty()) return false;
    std::stringstream ss(line); std::string tok;
    if (!getline(ss,tok,',')) return false;
    if (tok.empty() || !isdigit((unsigned char)tok[0])) return false;
    try { t.ts = std::stoull(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.ask = std::stod(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.bid = std::stod(tok); } catch(...) { return false; }
    return (t.ask>0 && t.bid>0 && t.ask>=t.bid);
}

inline int      utc_hour(uint64_t ts) { return (int)((ts/1000/3600)%24); }
inline uint64_t utc_day(uint64_t ts)  { return ts/1000/86400; }
inline bool     session_london(uint64_t ts) { int h=utc_hour(ts); return h>=7&&h<=10; }

// ── Parameters (v27 base) ──────────────────────────────────
static const int    WINDOW           = 600;
static const double IMPULSE_MIN      = 6.0;
static const double IMPULSE_MAX      = 15.0;
static const double TP_PTS           = 14.0;
static const double SL_PTS           = 7.0;
static const double PULLBACK_FRAC    = 0.50;
static const double VWAP_TREND_PTS   = 0.004;
static const int    VWAP_TREND_LOOK  = 30;
static const double MAX_SPREAD       = 0.40;
static const int    COOLDOWN_TICKS   = 300;
static const uint64_t TIME_LIMIT_MS  = 7200000;
static const uint64_t NO_TRAIL_MS    = 3600000;
static const int    ADVERSE_WINDOW   = 10;
static const double ADVERSE_MIN_PTS  = 4.0;
static const double MIN_PB_DEPTH     = 1.5;
static const bool   TRAIL_ENABLED    = true;
static const double TRAIL_TRIGGER    = 6.0;
static const double TRAIL_LOCK       = 0.75;
static const double COMMISSION_PTS   = 0.0;

enum class ExitReason { NONE, TP_HIT, SL_HIT, ADVERSE_EARLY, TIME_STOP, TRAIL_HIT };
static const char* exit_name(ExitReason r) {
    switch(r) {
        case ExitReason::TP_HIT:        return "TP_HIT";
        case ExitReason::SL_HIT:        return "SL_HIT";
        case ExitReason::ADVERSE_EARLY: return "ADVERSE_EARLY";
        case ExitReason::TIME_STOP:     return "TIME_STOP";
        case ExitReason::TRAIL_HIT:     return "TRAIL_HIT";
        default:                        return "NONE";
    }
}

struct TradeRecord {
    int id=0; bool is_long=false;
    double entry=0, exit_price=0, tp=0, sl=0;
    double spread_open=0, impulse_sz=0, pb_depth=0;
    double mfe=0, mae=0, gross_pnl=0, net_pnl=0;
    int hold_ticks=0; uint64_t hold_ms=0, entry_ts=0;
    ExitReason exit_why=ExitReason::NONE;
};

struct Engine {
    std::vector<double> price_buf, vwap_buf;
    double vwap=0, vwap_pv=0; uint64_t vwap_count=0, vwap_day=0;
    double hi=0, lo=0;
    bool in_pos=false, is_long=false, trail_active=false;
    double entry=0, tp=0, sl=0, mfe=0, mae=0, trail_sl=0;
    uint64_t entry_ts=0;
    int cooldown=0, pos_ticks=0;

    void update_vwap(double price, uint64_t ts) {
        uint64_t day = utc_day(ts);
        if (day != vwap_day) { vwap_pv=0; vwap_count=0; vwap_day=day; }
        vwap_pv += price; vwap_count++;
        vwap = vwap_pv / (double)vwap_count;
        vwap_buf.push_back(vwap);
        if ((int)vwap_buf.size() > std::max(WINDOW,VWAP_TREND_LOOK)+10)
            vwap_buf.erase(vwap_buf.begin());
    }
    void update_price(double price) {
        price_buf.push_back(price);
        if ((int)price_buf.size() > WINDOW+5) price_buf.erase(price_buf.begin());
    }
    bool detect_impulse() {
        if ((int)price_buf.size() < WINDOW) return false;
        int start=(int)price_buf.size()-WINDOW;
        hi=lo=price_buf[start];
        for (int i=start;i<(int)price_buf.size();i++) {
            hi=std::max(hi,price_buf[i]); lo=std::min(lo,price_buf[i]);
        }
        return (hi-lo)>=IMPULSE_MIN;
    }
    bool vwap_trend_up() {
        if ((int)vwap_buf.size()<VWAP_TREND_LOOK) return false;
        int n=(int)vwap_buf.size();
        return (vwap_buf[n-1]-vwap_buf[n-VWAP_TREND_LOOK])>VWAP_TREND_PTS;
    }
    bool vwap_trend_down() {
        if ((int)vwap_buf.size()<VWAP_TREND_LOOK) return false;
        int n=(int)vwap_buf.size();
        return (vwap_buf[n-1]-vwap_buf[n-VWAP_TREND_LOOK])<-VWAP_TREND_PTS;
    }
};

struct Stats {
    int count=0, wins=0; double total_pnl=0, total_mfe=0, total_mae=0, total_hold_ms=0;
    void add(const TradeRecord& tr) {
        count++; total_pnl+=tr.net_pnl; total_mfe+=tr.mfe; total_mae+=tr.mae;
        total_hold_ms+=(double)tr.hold_ms; if(tr.net_pnl>0) wins++;
    }
    void print(const char* label) const {
        if (!count) { std::cout<<"  "<<label<<": 0 trades\n"; return; }
        std::cout<<"  "<<std::left<<std::setw(16)<<label
            <<" n="<<std::setw(5)<<count
            <<" WR="<<std::fixed<<std::setprecision(1)<<std::setw(5)<<100.0*wins/count<<"%"
            <<" PnL="<<std::setw(9)<<std::setprecision(1)<<total_pnl*100<<" USD"
            <<" MFE="<<std::setw(5)<<std::setprecision(1)<<total_mfe/count
            <<" MAE="<<std::setw(5)<<std::setprecision(1)<<total_mae/count
            <<" hold="<<std::setprecision(0)<<total_hold_ms/count/1000<<"s\n";
    }
};

int main(int argc, char** argv)
{
    if (argc < 2) { std::cout<<"usage: goldflow_diag ticks.csv [trades_out.csv]\n"; return 0; }
    std::ifstream file(argv[1]);
    if (!file.is_open()) { std::cout<<"cannot open: "<<argv[1]<<"\n"; return 1; }

    std::ofstream csv_out;
    bool write_csv = (argc >= 3);
    if (write_csv) {
        csv_out.open(argv[2]);
        csv_out<<"id,is_long,entry_ts,entry,exit,tp,sl,spread_open,impulse_sz,pb_depth,"
               <<"mfe,mae,gross_pnl,net_pnl,hold_ticks,hold_ms,exit_why\n";
    }

    Engine e;
    std::vector<TradeRecord> trades;
    TradeRecord cur;

    uint64_t ticks_total=0, ticks_session=0;
    int trade_id=0;

    // ── Gate rejection counters ────────────────────────────
    // Each counter tracks how many ticks had impulse detected but were
    // rejected by each gate. A tick can be rejected by multiple gates
    // (tallied at first failing gate — funnel order).
    uint64_t cnt_impulse_detected  = 0;  // passed impulse >= IMPULSE_MIN
    uint64_t cnt_rej_in_pos        = 0;  // already in position
    uint64_t cnt_rej_cooldown      = 0;  // cooldown active
    uint64_t cnt_rej_impulse_max   = 0;  // impulse > IMPULSE_MAX (exhaustion)
    uint64_t cnt_rej_pb_frac       = 0;  // price not in pullback zone (mid > pb_long / mid < pb_short)
    uint64_t cnt_rej_vwap_side     = 0;  // price on wrong side of VWAP
    uint64_t cnt_rej_vwap_trend    = 0;  // VWAP trend not confirmed
    uint64_t cnt_rej_pb_min        = 0;  // pullback depth < MIN_PB_DEPTH
    uint64_t cnt_entered           = 0;  // actually entered

    // Also track: what would the pb_depth distribution look like for
    // ticks that passed ALL gates except pb_min — so we can see what
    // pb depths are available
    std::vector<double> pb_depth_available;  // pb depth at ticks that passed all gates except pb_min
    std::vector<double> pb_depth_entered;    // pb depth at actual entries

    std::string line;
    auto t_start = std::chrono::high_resolution_clock::now();

    while (getline(file, line))
    {
        Tick t;
        if (!parse_tick(line, t)) continue;
        ticks_total++;

        double spread = t.ask - t.bid;
        double mid    = (t.ask + t.bid) * 0.5;

        e.update_vwap(mid, t.ts);
        e.update_price(mid);

        if (spread > MAX_SPREAD) continue;

        // Force-close at session end
        if (e.in_pos && !session_london(t.ts)) {
            cur.exit_price = e.is_long ? t.bid : t.ask;
            cur.gross_pnl  = e.is_long ? cur.exit_price-e.entry : e.entry-cur.exit_price;
            cur.net_pnl    = cur.gross_pnl - cur.spread_open - COMMISSION_PTS;
            cur.hold_ms    = t.ts - e.entry_ts;
            cur.exit_why   = ExitReason::TIME_STOP;
            cur.mfe = std::max(0.0, cur.mfe); cur.mae = std::max(0.0, cur.mae);
            trades.push_back(cur);
            e.in_pos=false; e.trail_active=false;
        }

        if (!session_london(t.ts)) continue;
        ticks_session++;
        if (e.cooldown > 0) e.cooldown--;

        // ── manage open position ──────────────────────────
        if (e.in_pos) {
            e.pos_ticks++; cur.hold_ticks++;
            double exc = e.is_long ? mid-e.entry : e.entry-mid;
            if (exc > cur.mfe) cur.mfe=exc;
            if (exc < -cur.mae) cur.mae=-exc;
            if (TRAIL_ENABLED) {
                if (!e.trail_active && cur.mfe>=TRAIL_TRIGGER) e.trail_active=true;
                if (e.trail_active) {
                    double locked = cur.mfe*TRAIL_LOCK;
                    double nt = e.is_long ? e.entry+locked : e.entry-locked;
                    if (e.is_long) e.trail_sl=std::max(e.trail_sl,nt);
                    else           e.trail_sl=std::min(e.trail_sl,nt);
                }
            }
            bool adverse_early    = (e.pos_ticks<=ADVERSE_WINDOW && cur.mae>=ADVERSE_MIN_PTS);
            bool no_trail_timeout = (!e.trail_active && (t.ts-e.entry_ts)>=NO_TRAIL_MS);
            ExitReason why=ExitReason::NONE; double exit_px=0;
            if (e.is_long) {
                double px=t.bid;
                if      (px>=e.tp)                                         { why=ExitReason::TP_HIT;        exit_px=e.tp; }
                else if (adverse_early)                                     { why=ExitReason::ADVERSE_EARLY; exit_px=px; }
                else if (no_trail_timeout)                                  { why=ExitReason::TIME_STOP;     exit_px=px; }
                else if (px<=e.sl)                                         { why=ExitReason::SL_HIT;        exit_px=e.sl; }
                else if (TRAIL_ENABLED&&e.trail_active&&px<=e.trail_sl)    { why=ExitReason::TRAIL_HIT;     exit_px=e.trail_sl; }
                else if (t.ts-e.entry_ts>=TIME_LIMIT_MS)                   { why=ExitReason::TIME_STOP;     exit_px=px; }
            } else {
                double px=t.ask;
                if      (px<=e.tp)                                         { why=ExitReason::TP_HIT;        exit_px=e.tp; }
                else if (adverse_early)                                     { why=ExitReason::ADVERSE_EARLY; exit_px=px; }
                else if (no_trail_timeout)                                  { why=ExitReason::TIME_STOP;     exit_px=px; }
                else if (px>=e.sl)                                         { why=ExitReason::SL_HIT;        exit_px=e.sl; }
                else if (TRAIL_ENABLED&&e.trail_active&&px>=e.trail_sl)    { why=ExitReason::TRAIL_HIT;     exit_px=e.trail_sl; }
                else if (t.ts-e.entry_ts>=TIME_LIMIT_MS)                   { why=ExitReason::TIME_STOP;     exit_px=px; }
            }
            if (why!=ExitReason::NONE) {
                cur.exit_price=exit_px;
                cur.gross_pnl=e.is_long?exit_px-e.entry:e.entry-exit_px;
                cur.net_pnl=cur.gross_pnl-cur.spread_open-COMMISSION_PTS;
                cur.hold_ms=t.ts-e.entry_ts; cur.exit_why=why;
                cur.mfe=std::max(0.0,cur.mfe); cur.mae=std::max(0.0,cur.mae);
                trades.push_back(cur);
                if (write_csv) {
                    csv_out<<cur.id<<","<<(cur.is_long?1:0)<<","<<cur.entry_ts<<","
                        <<std::fixed<<std::setprecision(2)
                        <<cur.entry<<","<<cur.exit_price<<","<<cur.tp<<","<<cur.sl<<","
                        <<cur.spread_open<<","<<cur.impulse_sz<<","<<cur.pb_depth<<","
                        <<cur.mfe<<","<<cur.mae<<","<<cur.gross_pnl<<","<<cur.net_pnl<<","
                        <<cur.hold_ticks<<","<<cur.hold_ms<<","<<exit_name(why)<<"\n";
                }
                e.in_pos=false; e.trail_active=false;
            }
        }

        // ── entry gate funnel ─────────────────────────────
        if (!e.detect_impulse()) continue;
        cnt_impulse_detected++;

        double impulse = e.hi - e.lo;

        if (e.in_pos)    { cnt_rej_in_pos++;     continue; }
        if (e.cooldown>0){ cnt_rej_cooldown++;    continue; }
        if (impulse > IMPULSE_MAX) { cnt_rej_impulse_max++; continue; }

        double pb_long  = e.hi - PULLBACK_FRAC * impulse;
        double pb_short = e.lo + PULLBACK_FRAC * impulse;

        double pb_depth_long  = e.hi - mid;
        double pb_depth_short = mid - e.lo;

        bool in_pb_long  = (mid <= pb_long);
        bool in_pb_short = (mid >= pb_short);

        if (!in_pb_long && !in_pb_short) { cnt_rej_pb_frac++; continue; }

        bool above_vwap = (mid > e.vwap);
        bool below_vwap = (mid < e.vwap);
        bool vwap_side_ok = (in_pb_long && above_vwap) || (in_pb_short && below_vwap);

        if (!vwap_side_ok) { cnt_rej_vwap_side++; continue; }

        bool trend_up   = e.vwap_trend_up();
        bool trend_down = e.vwap_trend_down();
        bool trend_ok   = (in_pb_long && above_vwap && trend_up) ||
                          (in_pb_short && below_vwap && trend_down);

        if (!trend_ok) { cnt_rej_vwap_trend++; continue; }

        // At this point: passed impulse, not-in-pos, no cooldown, impulse size ok,
        // in pullback zone, correct vwap side, trend confirmed.
        // Only pb_min_depth remains.
        double pb_depth_candidate = in_pb_long ? pb_depth_long : pb_depth_short;
        pb_depth_available.push_back(pb_depth_candidate);

        if (pb_depth_candidate < MIN_PB_DEPTH) { cnt_rej_pb_min++; continue; }

        // ── ENTRY ──────────────────────────────────────────
        cnt_entered++;
        bool can_long  = (in_pb_long  && above_vwap && trend_up);
        bool can_short = (in_pb_short && below_vwap && trend_down);
        // If somehow both (shouldn't happen), prefer the side with deeper pullback
        if (can_long && can_short)
            can_long = (pb_depth_long >= pb_depth_short);

        e.in_pos  = true;
        e.is_long = can_long;
        if (e.is_long) {
            e.entry=t.ask; e.tp=e.entry+TP_PTS; e.sl=e.entry-SL_PTS; e.trail_sl=e.entry-SL_PTS;
        } else {
            e.entry=t.bid; e.tp=e.entry-TP_PTS; e.sl=e.entry+SL_PTS; e.trail_sl=e.entry+SL_PTS;
        }
        e.entry_ts=t.ts; e.trail_active=false; e.cooldown=COOLDOWN_TICKS;
        cur=TradeRecord{}; cur.id=++trade_id; cur.is_long=e.is_long;
        cur.entry=e.entry; cur.tp=e.tp; cur.sl=e.sl; cur.spread_open=spread;
        cur.impulse_sz=impulse; cur.pb_depth=pb_depth_candidate; cur.entry_ts=t.ts;
        e.pos_ticks=0; cur.mfe=0; cur.mae=0;
        pb_depth_entered.push_back(pb_depth_candidate);
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    double runtime = std::chrono::duration<double>(t_end-t_start).count();

    // ── Trade stats ────────────────────────────────────────
    Stats s_total, s_tp, s_sl, s_adverse, s_time, s_trail;
    for (const auto& tr : trades) {
        s_total.add(tr);
        switch(tr.exit_why) {
            case ExitReason::TP_HIT:        s_tp.add(tr);      break;
            case ExitReason::SL_HIT:        s_sl.add(tr);      break;
            case ExitReason::ADVERSE_EARLY: s_adverse.add(tr); break;
            case ExitReason::TIME_STOP:     s_time.add(tr);    break;
            case ExitReason::TRAIL_HIT:     s_trail.add(tr);   break;
            default: break;
        }
    }

    // ── pb_depth histogram helper ──────────────────────────
    auto pb_hist = [](const std::vector<double>& v, const char* title) {
        if (v.empty()) { std::cout<<"  "<<title<<": (none)\n"; return; }
        std::vector<std::pair<std::string,std::pair<double,double>>> bkts = {
            {"<1pt",  {0,1}},{"1-2pt",{1,2}},{"2-3pt",{2,3}},{"3-4pt",{3,4}},
            {"4-5pt", {4,5}},{"5-6pt",{5,6}},{"6-7pt",{6,7}},{"7-9pt",{7,9}},
            {"9-12pt",{9,12}},{"12+pt",{12,999}},
        };
        std::cout<<"  "<<title<<" (n="<<v.size()<<"):\n";
        for (auto& [lbl,rng] : bkts) {
            int n=0; for (double d:v) if(d>=rng.first&&d<rng.second) n++;
            if (n>0)
                std::cout<<"    "<<std::left<<std::setw(8)<<lbl<<" n="<<std::setw(6)<<n
                    <<" ("<<std::fixed<<std::setprecision(1)<<100.0*n/v.size()<<"%)\n";
        }
    };

    // ── Report ─────────────────────────────────────────────
    std::cout<<"\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  GoldFlow Gate Counter  [LONDON ONLY, v27 params]\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  Ticks total   : "<<ticks_total<<"\n";
    std::cout<<"  Ticks session : "<<ticks_session<<"\n";
    std::cout<<"  Runtime       : "<<std::fixed<<std::setprecision(2)<<runtime<<" s\n\n";

    std::cout<<"── Entry Gate Funnel ─────────────────────────────────────────\n";
    std::cout<<"  Impulse detected   : "<<cnt_impulse_detected<<"\n";
    std::cout<<"  Rej: in position   : "<<cnt_rej_in_pos<<"\n";
    std::cout<<"  Rej: cooldown      : "<<cnt_rej_cooldown<<"\n";
    std::cout<<"  Rej: impulse>max   : "<<cnt_rej_impulse_max<<"\n";
    std::cout<<"  Rej: not in pb zone: "<<cnt_rej_pb_frac<<"\n";
    std::cout<<"  Rej: wrong vwap    : "<<cnt_rej_vwap_side<<"\n";
    std::cout<<"  Rej: no vwap trend : "<<cnt_rej_vwap_trend<<" ← VWAP_TREND_PTS="<<VWAP_TREND_PTS<<"\n";
    std::cout<<"  Rej: pb too shallow: "<<cnt_rej_pb_min<<" ← MIN_PB_DEPTH="<<MIN_PB_DEPTH<<"\n";
    std::cout<<"  ENTERED            : "<<cnt_entered<<"\n\n";

    // Percentage of impulse-detected ticks that reach each stage
    if (cnt_impulse_detected > 0) {
        auto pct = [&](uint64_t n) {
            return 100.0 * n / cnt_impulse_detected;
        };
        std::cout<<"── Funnel as % of impulse-detected ──────────────────────────\n";
        std::cout<<"  Pass in-pos check  : "<<std::fixed<<std::setprecision(1)
            <<pct(cnt_impulse_detected - cnt_rej_in_pos)<<"%\n";
        uint64_t after_cooldown = cnt_impulse_detected - cnt_rej_in_pos - cnt_rej_cooldown;
        std::cout<<"  Pass cooldown      : "<<pct(after_cooldown)<<"%\n";
        uint64_t after_imax = after_cooldown - cnt_rej_impulse_max;
        std::cout<<"  Pass impulse max   : "<<pct(after_imax)<<"%\n";
        uint64_t after_pb = after_imax - cnt_rej_pb_frac;
        std::cout<<"  Pass pb zone       : "<<pct(after_pb)<<"%\n";
        uint64_t after_vwap_side = after_pb - cnt_rej_vwap_side;
        std::cout<<"  Pass vwap side     : "<<pct(after_vwap_side)<<"%\n";
        uint64_t after_trend = after_vwap_side - cnt_rej_vwap_trend;
        std::cout<<"  Pass vwap trend    : "<<pct(after_trend)<<"% ← "<<after_trend<<" ticks\n";
        std::cout<<"  Pass pb min depth  : "<<pct(cnt_entered)<<"% ← "<<cnt_entered<<" entries\n\n";
    }

    std::cout<<"── pb_depth at ticks that passed ALL gates except pb_min ─────\n";
    pb_hist(pb_depth_available, "Available pb depths");
    std::cout<<"\n";
    std::cout<<"── pb_depth at actual entries ────────────────────────────────\n";
    pb_hist(pb_depth_entered, "Entered pb depths");
    std::cout<<"\n";

    std::cout<<"── Trade Results (v27 params) ────────────────────────────────\n";
    s_total.print("TOTAL");
    s_tp.print("TP_HIT");
    s_sl.print("SL_HIT");
    s_adverse.print("ADVERSE_EARLY");
    s_time.print("TIME_STOP");
    s_trail.print("TRAIL_HIT");
    std::cout<<"\n";

    double total_usd=0; int total_wins=0;
    for (const auto& tr : trades) { total_usd+=tr.net_pnl*100.0; if(tr.net_pnl>0) total_wins++; }
    int total_n=(int)trades.size();
    double wr = total_n>0 ? 100.0*total_wins/total_n : 0;

    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  RESULT: "<<total_n<<" trades | WR="<<std::fixed<<std::setprecision(1)<<wr
        <<"% | PnL="<<std::setprecision(0)<<total_usd<<" USD\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n\n";
    if (write_csv) std::cout<<"  CSV: "<<argv[2]<<"\n\n";
    return 0;
}
