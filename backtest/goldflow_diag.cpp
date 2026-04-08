// goldflow_diag.cpp
// GoldFlow diagnostic backtest — exit reason breakdown + per-trade CSV
// Build: g++ -O2 -std=c++17 -o goldflow_diag goldflow_diag.cpp
// Run:   ./goldflow_diag ticks.csv [trades_out.csv]
//
// VERSION HISTORY:
//   v27: London only → 26 trades +$6,136 65.4% WR
//   v29: Gate counter: 4,581 rejected by VWAP side check (66% of impulse ticks)
//   v30: Remove VWAP side check, session-anchor VWAP → still 26 trades
//        VWAP trend filter (0.004/30ticks) now killing 5,208 ticks — same bottleneck
//        75.1% of pb-zone candidates rejected by trend filter. 0.004 threshold is
//        too tight for session-anchored VWAP which barely moves in early London.
// v31: REMOVE VWAP TREND FILTER ENTIRELY
//      Direction signal = impulse direction + pullback zone only.
//      If price made an upward impulse and pulled back into the lower 50% of that
//      range, that IS the entry signal. No secondary trend confirmation needed.
//      This is the "ceiling" run — maximum trades, see raw edge quality.
//      Gate counter retained. VWAP kept for session anchor only (not as filter).
//      All other v27 params unchanged.

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
    if (tok.empty()||!isdigit((unsigned char)tok[0])) return false;
    try { t.ts=std::stoull(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.ask=std::stod(tok); } catch(...) { return false; }
    if (!getline(ss,tok,',')) return false;
    try { t.bid=std::stod(tok); } catch(...) { return false; }
    return (t.ask>0&&t.bid>0&&t.ask>=t.bid);
}

inline int      utc_hour(uint64_t ts)  { return (int)((ts/1000/3600)%24); }
inline uint64_t utc_day(uint64_t ts)   { return ts/1000/86400; }
inline bool     session_london(uint64_t ts) { int h=utc_hour(ts); return h>=7&&h<=10; }
inline uint64_t london_session_id(uint64_t ts) { return utc_day(ts)*100+7; }

// ── Parameters ────────────────────────────────────────────
static const int    WINDOW           = 600;
static const double IMPULSE_MIN      = 6.0;
static const double IMPULSE_MAX      = 15.0;
static const double TP_PTS           = 14.0;
static const double SL_PTS           = 7.0;
static const double PULLBACK_FRAC    = 0.50;
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
// VWAP_TREND_PTS removed — no trend filter in v31

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
    std::vector<double> price_buf;
    // Session-anchored VWAP (kept for future use, not used as filter in v31)
    double vwap=0, vwap_pv=0; uint64_t vwap_count=0, vwap_session=0;
    double hi=0, lo=0;
    bool in_pos=false, is_long=false, trail_active=false;
    double entry=0, tp=0, sl=0, mfe=0, mae=0, trail_sl=0;
    uint64_t entry_ts=0;
    int cooldown=0, pos_ticks=0;

    void update_vwap(double price, uint64_t ts) {
        uint64_t sess=london_session_id(ts);
        if (sess!=vwap_session){vwap_pv=0;vwap_count=0;vwap_session=sess;}
        vwap_pv+=price; vwap_count++;
        vwap=vwap_pv/(double)vwap_count;
    }
    void update_price(double price) {
        price_buf.push_back(price);
        if ((int)price_buf.size()>WINDOW+5) price_buf.erase(price_buf.begin());
    }
    bool detect_impulse() {
        if ((int)price_buf.size()<WINDOW) return false;
        int start=(int)price_buf.size()-WINDOW;
        hi=lo=price_buf[start];
        for (int i=start;i<(int)price_buf.size();i++){
            hi=std::max(hi,price_buf[i]); lo=std::min(lo,price_buf[i]);
        }
        return (hi-lo)>=IMPULSE_MIN;
    }
};

struct Stats {
    int count=0,wins=0;
    double total_pnl=0,total_mfe=0,total_mae=0,total_hold_ms=0,total_imp=0,total_pb=0;
    void add(const TradeRecord& tr){
        count++;total_pnl+=tr.net_pnl;total_mfe+=tr.mfe;total_mae+=tr.mae;
        total_hold_ms+=(double)tr.hold_ms;total_imp+=tr.impulse_sz;total_pb+=tr.pb_depth;
        if(tr.net_pnl>0)wins++;
    }
    void print(const char* label) const {
        if(!count){std::cout<<"  "<<label<<": 0 trades\n";return;}
        std::cout<<"  "<<std::left<<std::setw(16)<<label
            <<" n="<<std::setw(5)<<count
            <<" WR="<<std::fixed<<std::setprecision(1)<<std::setw(5)<<100.0*wins/count<<"%"
            <<" PnL="<<std::setw(9)<<std::setprecision(1)<<total_pnl*100<<" USD"
            <<" avg="<<std::setw(6)<<std::setprecision(2)<<total_pnl/count<<" pts"
            <<" MFE="<<std::setw(5)<<std::setprecision(1)<<total_mfe/count
            <<" MAE="<<std::setw(5)<<std::setprecision(1)<<total_mae/count
            <<" hold="<<std::setprecision(0)<<total_hold_ms/count/1000<<"s\n";
    }
};

inline bool trade_session_ok(uint64_t ts){return session_london(ts);}

int main(int argc, char** argv)
{
    if (argc<2){std::cout<<"usage: goldflow_diag ticks.csv [trades_out.csv]\n";return 0;}
    std::ifstream file(argv[1]);
    if (!file.is_open()){std::cout<<"cannot open: "<<argv[1]<<"\n";return 1;}

    std::ofstream csv_out;
    bool write_csv=(argc>=3);
    if (write_csv){
        csv_out.open(argv[2]);
        csv_out<<"id,is_long,entry_ts,entry,exit,tp,sl,spread_open,impulse_sz,pb_depth,"
               <<"mfe,mae,gross_pnl,net_pnl,hold_ticks,hold_ms,exit_why\n";
    }

    Engine e;
    std::vector<TradeRecord> trades;
    TradeRecord cur;
    uint64_t ticks_total=0,ticks_session=0;
    int trade_id=0;

    // Gate counters
    uint64_t cnt_impulse=0,cnt_rej_inpos=0,cnt_rej_cooldown=0;
    uint64_t cnt_rej_imax=0,cnt_rej_pbzone=0,cnt_rej_pbmin=0,cnt_entered=0;
    std::vector<double> pb_available,pb_entered;

    std::string line;
    auto t_start=std::chrono::high_resolution_clock::now();

    while(getline(file,line))
    {
        Tick t;
        if(!parse_tick(line,t)) continue;
        ticks_total++;
        double spread=t.ask-t.bid;
        double mid=(t.ask+t.bid)*0.5;

        e.update_vwap(mid,t.ts);
        e.update_price(mid);
        if(spread>MAX_SPREAD) continue;

        // Force-close at session end
        if(e.in_pos&&!trade_session_ok(t.ts)){
            cur.exit_price=e.is_long?t.bid:t.ask;
            cur.gross_pnl=e.is_long?cur.exit_price-e.entry:e.entry-cur.exit_price;
            cur.net_pnl=cur.gross_pnl-cur.spread_open-COMMISSION_PTS;
            cur.hold_ms=t.ts-e.entry_ts; cur.exit_why=ExitReason::TIME_STOP;
            cur.mfe=std::max(0.0,cur.mfe); cur.mae=std::max(0.0,cur.mae);
            trades.push_back(cur); e.in_pos=false; e.trail_active=false;
        }
        if(!trade_session_ok(t.ts)) continue;
        ticks_session++;
        if(e.cooldown>0) e.cooldown--;

        // ── manage open position ──────────────────────────
        if(e.in_pos){
            e.pos_ticks++; cur.hold_ticks++;
            double exc=e.is_long?mid-e.entry:e.entry-mid;
            if(exc>cur.mfe) cur.mfe=exc;
            if(exc<-cur.mae) cur.mae=-exc;
            if(TRAIL_ENABLED){
                if(!e.trail_active&&cur.mfe>=TRAIL_TRIGGER) e.trail_active=true;
                if(e.trail_active){
                    double locked=cur.mfe*TRAIL_LOCK;
                    double nt=e.is_long?e.entry+locked:e.entry-locked;
                    if(e.is_long) e.trail_sl=std::max(e.trail_sl,nt);
                    else          e.trail_sl=std::min(e.trail_sl,nt);
                }
            }
            bool adverse=(e.pos_ticks<=ADVERSE_WINDOW&&cur.mae>=ADVERSE_MIN_PTS);
            bool no_trail_to=(!e.trail_active&&(t.ts-e.entry_ts)>=NO_TRAIL_MS);
            ExitReason why=ExitReason::NONE; double exit_px=0;
            if(e.is_long){
                double px=t.bid;
                if      (px>=e.tp)                                      {why=ExitReason::TP_HIT;        exit_px=e.tp;}
                else if (adverse)                                        {why=ExitReason::ADVERSE_EARLY; exit_px=px;}
                else if (no_trail_to)                                    {why=ExitReason::TIME_STOP;     exit_px=px;}
                else if (px<=e.sl)                                      {why=ExitReason::SL_HIT;        exit_px=e.sl;}
                else if (TRAIL_ENABLED&&e.trail_active&&px<=e.trail_sl) {why=ExitReason::TRAIL_HIT;     exit_px=e.trail_sl;}
                else if (t.ts-e.entry_ts>=TIME_LIMIT_MS)                {why=ExitReason::TIME_STOP;     exit_px=px;}
            } else {
                double px=t.ask;
                if      (px<=e.tp)                                      {why=ExitReason::TP_HIT;        exit_px=e.tp;}
                else if (adverse)                                        {why=ExitReason::ADVERSE_EARLY; exit_px=px;}
                else if (no_trail_to)                                    {why=ExitReason::TIME_STOP;     exit_px=px;}
                else if (px>=e.sl)                                      {why=ExitReason::SL_HIT;        exit_px=e.sl;}
                else if (TRAIL_ENABLED&&e.trail_active&&px>=e.trail_sl) {why=ExitReason::TRAIL_HIT;     exit_px=e.trail_sl;}
                else if (t.ts-e.entry_ts>=TIME_LIMIT_MS)                {why=ExitReason::TIME_STOP;     exit_px=px;}
            }
            if(why!=ExitReason::NONE){
                cur.exit_price=exit_px;
                cur.gross_pnl=e.is_long?exit_px-e.entry:e.entry-exit_px;
                cur.net_pnl=cur.gross_pnl-cur.spread_open-COMMISSION_PTS;
                cur.hold_ms=t.ts-e.entry_ts; cur.exit_why=why;
                cur.mfe=std::max(0.0,cur.mfe); cur.mae=std::max(0.0,cur.mae);
                trades.push_back(cur);
                if(write_csv){
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
        if(!e.detect_impulse()) continue;
        cnt_impulse++;

        double impulse=e.hi-e.lo;
        if(e.in_pos)     {cnt_rej_inpos++;    continue;}
        if(e.cooldown>0) {cnt_rej_cooldown++;  continue;}
        if(impulse>IMPULSE_MAX){cnt_rej_imax++;continue;}

        double pb_long  = e.hi - PULLBACK_FRAC*impulse;
        double pb_short = e.lo + PULLBACK_FRAC*impulse;
        double pb_depth_long  = e.hi - mid;
        double pb_depth_short = mid  - e.lo;

        // Direction purely from pullback zone:
        // Long  = price in lower half of impulse range (pulled back from high)
        // Short = price in upper half of impulse range (pulled back from low)
        bool can_long  = (mid <= pb_long);
        bool can_short = (mid >= pb_short);

        if(!can_long && !can_short){cnt_rej_pbzone++;continue;}

        // If price is in both zones (very small impulse near midpoint), pick
        // the deeper pullback side
        if(can_long && can_short)
            can_long = (pb_depth_long >= pb_depth_short);

        double pb_depth_candidate = can_long ? pb_depth_long : pb_depth_short;
        pb_available.push_back(pb_depth_candidate);

        if(pb_depth_candidate < MIN_PB_DEPTH){cnt_rej_pbmin++;continue;}

        // ── ENTRY ──────────────────────────────────────────
        cnt_entered++;
        e.in_pos  = true;
        e.is_long = can_long;
        if(e.is_long){
            e.entry=t.ask; e.tp=e.entry+TP_PTS; e.sl=e.entry-SL_PTS; e.trail_sl=e.entry-SL_PTS;
        } else {
            e.entry=t.bid; e.tp=e.entry-TP_PTS; e.sl=e.entry+SL_PTS; e.trail_sl=e.entry+SL_PTS;
        }
        e.entry_ts=t.ts; e.trail_active=false; e.cooldown=COOLDOWN_TICKS;
        cur=TradeRecord{}; cur.id=++trade_id; cur.is_long=e.is_long;
        cur.entry=e.entry; cur.tp=e.tp; cur.sl=e.sl; cur.spread_open=spread;
        cur.impulse_sz=impulse; cur.pb_depth=pb_depth_candidate; cur.entry_ts=t.ts;
        e.pos_ticks=0; cur.mfe=0; cur.mae=0;
        pb_entered.push_back(pb_depth_candidate);
    }

    auto t_end=std::chrono::high_resolution_clock::now();
    double runtime=std::chrono::duration<double>(t_end-t_start).count();

    // ── Aggregate ─────────────────────────────────────────
    Stats s_total,s_tp,s_sl,s_adverse,s_time,s_trail;
    for(const auto& tr:trades){
        s_total.add(tr);
        switch(tr.exit_why){
            case ExitReason::TP_HIT:        s_tp.add(tr);      break;
            case ExitReason::SL_HIT:        s_sl.add(tr);      break;
            case ExitReason::ADVERSE_EARLY: s_adverse.add(tr); break;
            case ExitReason::TIME_STOP:     s_time.add(tr);    break;
            case ExitReason::TRAIL_HIT:     s_trail.add(tr);   break;
            default: break;
        }
    }

    auto pb_hist=[](const std::vector<double>& v,const char* title){
        if(v.empty()){std::cout<<"  "<<title<<": (none)\n";return;}
        std::vector<std::pair<std::string,std::pair<double,double>>> bkts={
            {"<1pt",{0,1}},{"1-2pt",{1,2}},{"2-3pt",{2,3}},{"3-4pt",{3,4}},
            {"4-5pt",{4,5}},{"5-6pt",{5,6}},{"6-7pt",{6,7}},{"7-9pt",{7,9}},
            {"9-12pt",{9,12}},{"12+pt",{12,999}},
        };
        std::cout<<"  "<<title<<" (n="<<v.size()<<"):\n";
        for(auto& [lbl,rng]:bkts){
            int n=0; for(double d:v) if(d>=rng.first&&d<rng.second) n++;
            if(n>0) std::cout<<"    "<<std::left<<std::setw(8)<<lbl<<" n="<<std::setw(6)<<n
                <<" ("<<std::fixed<<std::setprecision(1)<<100.0*n/v.size()<<"%)\n";
        }
    };

    // ── Report ─────────────────────────────────────────────
    std::cout<<"\n══════════════════════════════════════════════════════════════\n";
    std::cout<<"  GoldFlow Diagnostic Backtest  v31  [LONDON ONLY]\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  Ticks total   : "<<ticks_total<<"\n";
    std::cout<<"  Ticks session : "<<ticks_session<<"\n";
    std::cout<<"  Runtime       : "<<std::fixed<<std::setprecision(2)<<runtime<<" s\n\n";
    std::cout<<"  Parameters:\n";
    std::cout<<"    WINDOW="<<WINDOW<<" IMPULSE="<<IMPULSE_MIN<<"-"<<IMPULSE_MAX
             <<" TP="<<TP_PTS<<" SL="<<SL_PTS<<"\n";
    std::cout<<"    PULLBACK_FRAC="<<PULLBACK_FRAC<<" MIN_PB_DEPTH="<<MIN_PB_DEPTH<<"pts\n";
    std::cout<<"    NO VWAP TREND FILTER — direction from impulse+pullback zone only\n";
    std::cout<<"    ADVERSE_WINDOW="<<ADVERSE_WINDOW<<" ADVERSE_MIN="<<ADVERSE_MIN_PTS<<" pts\n";
    std::cout<<"    SESSION=LONDON(07-10) ONLY  force-close-at-end\n";
    std::cout<<"    TRAIL="<<(TRAIL_ENABLED?"ON":"OFF");
    if(TRAIL_ENABLED) std::cout<<" trigger="<<TRAIL_TRIGGER<<"pts lock="<<(int)(TRAIL_LOCK*100)<<"%";
    std::cout<<"\n\n";

    std::cout<<"── Entry Gate Funnel ─────────────────────────────────────────\n";
    std::cout<<"  Impulse detected   : "<<cnt_impulse<<"\n";
    std::cout<<"  Rej: in position   : "<<cnt_rej_inpos<<"\n";
    std::cout<<"  Rej: cooldown      : "<<cnt_rej_cooldown<<"\n";
    std::cout<<"  Rej: impulse>max   : "<<cnt_rej_imax<<"\n";
    std::cout<<"  Rej: not in pb zone: "<<cnt_rej_pbzone<<"\n";
    std::cout<<"  Rej: pb too shallow: "<<cnt_rej_pbmin<<"\n";
    std::cout<<"  ENTERED            : "<<cnt_entered<<"\n\n";

    if(cnt_impulse>0){
        std::cout<<"── Funnel % of impulse-detected ──────────────────────────────\n";
        std::cout<<"  Pass in-pos    : "<<std::fixed<<std::setprecision(1)
            <<100.0*(cnt_impulse-cnt_rej_inpos)/cnt_impulse<<"%\n";
        uint64_t a=cnt_impulse-cnt_rej_inpos-cnt_rej_cooldown;
        std::cout<<"  Pass cooldown  : "<<100.0*a/cnt_impulse<<"%\n";
        uint64_t b=a-cnt_rej_imax;
        std::cout<<"  Pass imax      : "<<100.0*b/cnt_impulse<<"%\n";
        uint64_t c=b-cnt_rej_pbzone;
        std::cout<<"  Pass pb zone   : "<<100.0*c/cnt_impulse<<"% ← "<<c<<" ticks\n";
        std::cout<<"  Pass pb min    : "<<100.0*cnt_entered/cnt_impulse<<"% ← "<<cnt_entered<<" entries\n\n";
    }

    pb_hist(pb_available,"pb_depth: passed all gates except pb_min");
    std::cout<<"\n";
    pb_hist(pb_entered,"pb_depth: actual entries");
    std::cout<<"\n";

    std::cout<<"── By Exit Reason ────────────────────────────────────────────\n";
    s_total.print("TOTAL");
    s_tp.print("TP_HIT");
    s_sl.print("SL_HIT");
    s_adverse.print("ADVERSE_EARLY");
    s_time.print("TIME_STOP");
    if(TRAIL_ENABLED) s_trail.print("TRAIL_HIT");
    std::cout<<"\n";

    if(s_sl.count>0){
        std::cout<<"── SL_HIT: MFE Groups ────────────────────────────────────────\n";
        for(auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
            {"MFE<1pts",{0,1}},{"MFE 1-3pts",{1,3}},{"MFE 3-6pts",{3,6}},{"MFE>6pts",{6,999}}}){
            int n=0; double pnl=0,imp=0,pb=0;
            for(const auto& tr:trades) if(tr.exit_why==ExitReason::SL_HIT&&tr.mfe>=rng.first&&tr.mfe<rng.second)
                {n++;pnl+=tr.net_pnl;imp+=tr.impulse_sz;pb+=tr.pb_depth;}
            if(n>0) std::cout<<"  "<<std::left<<std::setw(14)<<lbl
                <<" n="<<n<<" PnL="<<std::setprecision(1)<<pnl*100
                <<" avg_imp="<<std::setprecision(1)<<imp/n<<" avg_pb="<<std::setprecision(2)<<pb/n<<"\n";
        }
        std::cout<<"\n";
    }

    if(s_adverse.count>0){
        std::cout<<"── ADVERSE_EARLY: Impulse Buckets ────────────────────────────\n";
        for(auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
            {"<8pts",{0,8}},{"8-10pts",{8,10}},{"10-12pts",{10,12}},{"12-15pts",{12,15}}}){
            int n=0; double pnl=0;
            for(const auto& tr:trades) if(tr.exit_why==ExitReason::ADVERSE_EARLY&&tr.impulse_sz>=rng.first&&tr.impulse_sz<rng.second)
                {n++;pnl+=tr.net_pnl;}
            if(n>0) std::cout<<"  "<<std::left<<std::setw(12)<<lbl<<" n="<<n<<" PnL="<<std::setprecision(1)<<pnl*100<<"\n";
        }
        std::cout<<"\n";
    }

    if(s_time.count>0){
        std::cout<<"── TIME_STOP: MFE at timeout ─────────────────────────────────\n";
        for(auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
            {"MFE<2pts",{0,2}},{"MFE 2-5pts",{2,5}},{"MFE 5-8pts",{5,8}},{"MFE>8pts",{8,999}}}){
            int n=0; double pnl=0;
            for(const auto& tr:trades) if(tr.exit_why==ExitReason::TIME_STOP&&tr.mfe>=rng.first&&tr.mfe<rng.second)
                {n++;pnl+=tr.net_pnl;}
            if(n>0) std::cout<<"  "<<std::left<<std::setw(14)<<lbl<<" n="<<n<<" PnL="<<std::setprecision(1)<<pnl*100<<"\n";
        }
        std::cout<<"\n";
    }

    if(TRAIL_ENABLED&&s_trail.count>0){
        std::cout<<"── TRAIL_HIT: MFE at trail exit ──────────────────────────────\n";
        for(auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
            {"MFE 6-10pts",{6,10}},{"MFE 10-15pts",{10,15}},{"MFE 15-20pts",{15,20}},{"MFE>20pts",{20,999}}}){
            int n=0; double pnl=0;
            for(const auto& tr:trades) if(tr.exit_why==ExitReason::TRAIL_HIT&&tr.mfe>=rng.first&&tr.mfe<rng.second)
                {n++;pnl+=tr.net_pnl;}
            if(n>0) std::cout<<"  "<<std::left<<std::setw(14)<<lbl<<" n="<<n<<" PnL="<<std::setprecision(1)<<pnl*100<<"\n";
        }
        std::cout<<"\n";
    }

    std::cout<<"── Pullback Depth at Entry ───────────────────────────────────\n";
    for(auto& [lbl,rng]:std::vector<std::pair<std::string,std::pair<double,double>>>{
        {"pb 1-3pt",{1,3}},{"pb 3-5pt",{3,5}},{"pb 5-7pt",{5,7}},
        {"pb 7-10pt",{7,10}},{"pb>10pt",{10,999}}}){
        int n=0,w=0; double pnl=0;
        for(const auto& tr:trades) if(tr.pb_depth>=rng.first&&tr.pb_depth<rng.second)
            {n++;pnl+=tr.net_pnl;if(tr.net_pnl>0)w++;}
        if(n>0) std::cout<<"  "<<std::left<<std::setw(12)<<lbl
            <<" n="<<std::setw(5)<<n<<" WR="<<std::fixed<<std::setprecision(1)<<100.0*w/n
            <<"% PnL="<<std::setprecision(1)<<pnl*100<<" USD\n";
    }
    std::cout<<"\n";

    // Long vs Short breakdown
    {
        int nl=0,wl=0,ns=0,ws=0; double pl=0,ps=0;
        for(const auto& tr:trades){
            if(tr.is_long){nl++;pl+=tr.net_pnl;if(tr.net_pnl>0)wl++;}
            else           {ns++;ps+=tr.net_pnl;if(tr.net_pnl>0)ws++;}
        }
        std::cout<<"── Long vs Short ─────────────────────────────────────────────\n";
        if(nl>0) std::cout<<"  LONG  n="<<nl<<" WR="<<std::fixed<<std::setprecision(1)<<100.0*wl/nl<<"% PnL="<<std::setprecision(1)<<pl*100<<" USD\n";
        if(ns>0) std::cout<<"  SHORT n="<<ns<<" WR="<<std::fixed<<std::setprecision(1)<<100.0*ws/ns<<"% PnL="<<std::setprecision(1)<<ps*100<<" USD\n";
        std::cout<<"\n";
    }

    double total_usd=0; int total_wins=0;
    for(const auto& tr:trades){total_usd+=tr.net_pnl*100.0;if(tr.net_pnl>0)total_wins++;}
    int total_n=(int)trades.size();
    double wr=total_n>0?100.0*total_wins/total_n:0;
    std::cout<<"══════════════════════════════════════════════════════════════\n";
    std::cout<<"  RESULT: "<<total_n<<" trades | WR="<<std::fixed<<std::setprecision(1)<<wr
        <<"% | PnL="<<std::setprecision(0)<<total_usd<<" USD\n";
    std::cout<<"══════════════════════════════════════════════════════════════\n\n";
    if(write_csv) std::cout<<"  CSV: "<<argv[2]<<"\n\n";
    return 0;
}
