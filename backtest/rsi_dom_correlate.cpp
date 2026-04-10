// rsi_dom_correlate.cpp
// Find every RSI turn (sustained fall then first uptick, or sustained rise then first downtick)
// Print what DOM was doing in the 10 ticks BEFORE and 10 ticks AFTER the turn
// This shows us what DOM signal PRECEDES or FOLLOWS the RSI turn
// Build: g++ -O2 -std=c++17 -o rsi_dom_correlate rsi_dom_correlate.cpp

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>

struct L2Tick {
    int64_t ts_ms=0;
    double bid=0,ask=0,mid=0,spread=0;
    int depth_bid=0,depth_ask=0;
    bool watchdog_dead=false;
    double ewm_drift=0;
    double rsi=0; // computed
    int dom_long=0, dom_short=0; // dom scores
};

bool parse_l2(const std::string& line, L2Tick& t) {
    if (line.empty()||line[0]=='t') return false;
    std::stringstream ss(line); std::string tok;
    auto nd=[&](double& v)->bool{if(!getline(ss,tok,','))return false;try{v=std::stod(tok);}catch(...){return false;}return true;};
    auto ni=[&](int& v)->bool{if(!getline(ss,tok,','))return false;try{v=(int)std::stoll(tok);}catch(...){return false;}return true;};
    double tmp; uint64_t u; bool b;
    if(!nd(tmp))return false; t.ts_ms=(int64_t)tmp;
    if(!nd(t.bid)||!nd(t.ask))return false;
    t.mid=(t.bid+t.ask)*0.5; t.spread=t.ask-t.bid;
    double skip; int iskip;
    if(!nd(skip)||!nd(skip)||!nd(skip))return false; // l2_imb,bid_vol,ask_vol
    if(!ni(t.depth_bid)||!ni(t.depth_ask))return false;
    // depth_events
    if(!getline(ss,tok,','))return false;
    // watchdog_dead
    if(!getline(ss,tok,','))return false; try{b=(bool)std::stoll(tok);t.watchdog_dead=b;}catch(...){}
    // vol_ratio, regime, vpin, has_pos, micro_edge
    for(int i=0;i<5;i++) getline(ss,tok,',');
    // ewm_drift
    if(getline(ss,tok,','))try{t.ewm_drift=std::stod(tok);}catch(...){}
    return (t.bid>0&&t.ask>0&&t.ask>=t.bid&&t.spread<0.5);
}

struct TickRSI {
    static constexpr int N=14;
    double ag=0,al=0,prev=-1,val=50.0; int c=0; bool w=false;
    void update(double p){
        if(prev<0){prev=p;return;}
        double d=p-prev; prev=p;
        double g=d>0?d:0,l=d<0?-d:0;
        if(c<N){ag+=g;al+=l;++c;if(c==N){ag/=N;al/=N;w=true;}}
        else{ag=(ag*(N-1)+g)/N;al=(al*(N-1)+l)/N;
             val=al<1e-10?100.0:100.0-100.0/(1.0+ag/al);}
    }
};

int dom_long_score(const L2Tick& t, const L2Tick& p){
    int s=0;
    if(t.depth_bid>t.depth_ask)++s;
    if(t.depth_ask<p.depth_ask&&p.depth_ask>0)++s;
    if(t.depth_ask<=1)++s;
    if(t.depth_bid>p.depth_bid)++s;
    return s;
}
int dom_short_score(const L2Tick& t, const L2Tick& p){
    int s=0;
    if(t.depth_ask>t.depth_bid)++s;
    if(t.depth_bid<p.depth_bid&&p.depth_bid>0)++s;
    if(t.depth_bid<=1)++s;
    if(t.depth_ask>p.depth_ask)++s;
    return s;
}

int main(int argc, char* argv[]){
    std::vector<std::string> files;
    for(int i=1;i<argc;i++) files.push_back(argv[i]);
    if(files.empty()){std::cerr<<"Usage: rsi_dom_correlate files...\n";return 1;}

    // Load all ticks with RSI and DOM computed
    std::vector<L2Tick> ticks;
    TickRSI rsi;
    L2Tick prev{};

    for(auto& fname:files){
        std::ifstream f(fname); if(!f)continue;
        std::string line; getline(f,line);
        while(getline(f,line)){
            L2Tick t; if(!parse_l2(line,t))continue;
            if(t.watchdog_dead)continue;
            rsi.update(t.mid);
            if(!rsi.w){prev=t;continue;}
            t.rsi=rsi.val;
            t.dom_long=dom_long_score(t,prev);
            t.dom_short=dom_short_score(t,prev);
            ticks.push_back(t);
            prev=t;
        }
    }

    printf("Loaded %d ticks\n\n", (int)ticks.size());

    // Find RSI turns using M1 bar RSI samples
    // Sample RSI at each M1 bar boundary
    struct BarRSI { double rsi; int64_t ts; int tick_idx; };
    std::vector<BarRSI> bar_rsis;
    int64_t last_bm=-1;
    for(int i=0;i<(int)ticks.size();i++){
        int64_t bm=ticks[i].ts_ms/60000;
        if(bm!=last_bm){
            if(last_bm>=0) bar_rsis.push_back({ticks[i-1].rsi, ticks[i-1].ts_ms, i-1});
            last_bm=bm;
        }
    }

    printf("=== RSI TURN ANALYSIS: DOM state around each RSI inflection ===\n");
    printf("Showing turns where RSI fell/rose for 2+ bars then reversed\n\n");

    // Stats: for each of -30 to +30 ticks around the turn, what is avg DOM score
    static const int WINDOW=30;
    std::vector<double> sum_dom_long_before(WINDOW,0), sum_dom_long_after(WINDOW,0);
    std::vector<double> sum_dom_short_before(WINDOW,0), sum_dom_short_after(WINDOW,0);
    int n_long_turns=0, n_short_turns=0;

    // Also track: what is avg DOM score at various offsets before the turn
    // offset -N = N ticks before turn, +N = N ticks after
    struct TurnEvent {
        int tick_idx; bool is_long_turn;
        double rsi_at_turn, drift_at_turn;
        double price_change_after_10s; // price move in 10 ticks after entry
    };
    std::vector<TurnEvent> turns;

    for(int b=2;b<(int)bar_rsis.size()-1;b++){
        double r0=bar_rsis[b-2].rsi;
        double r1=bar_rsis[b-1].rsi;
        double r2=bar_rsis[b].rsi;
        double r3=bar_rsis[b+1<(int)bar_rsis.size()?b+1:b].rsi;

        // Sustained fall then turn up: r0>r1>r2 then r3>r2
        bool long_turn = (r1 < r0-0.5) && (r2 < r1-0.5) && (r3 > r2+0.5);
        // Sustained rise then turn down: r0<r1<r2 then r3<r2
        bool short_turn = (r1 > r0+0.5) && (r2 > r1+0.5) && (r3 < r2-0.5);

        if(!long_turn && !short_turn) continue;

        int ti = bar_rsis[b].tick_idx;
        if(ti<WINDOW || ti+WINDOW>=(int)ticks.size()) continue;

        double drift = ticks[ti].ewm_drift;
        double price_now = ticks[ti].mid;
        double price_later = ticks[std::min(ti+20,(int)ticks.size()-1)].mid;
        double price_change = long_turn ? (price_later-price_now) : (price_now-price_later);

        turns.push_back({ti, long_turn, r2, drift, price_change});

        // Accumulate DOM stats
        for(int off=0;off<WINDOW;off++){
            if(long_turn){
                sum_dom_long_before[off] += ticks[ti-WINDOW+off].dom_long;
                sum_dom_long_after[off]  += ticks[ti+off].dom_long;
                n_long_turns++;
            } else {
                sum_dom_short_before[off] += ticks[ti-WINDOW+off].dom_short;
                sum_dom_short_after[off]  += ticks[ti+off].dom_short;
                n_short_turns++;
            }
        }
        if(n_long_turns>0) n_long_turns--; // fix double count
        if(n_short_turns>0) n_short_turns--;
    }

    // Fix count (we incremented in loop)
    n_long_turns=0; n_short_turns=0;
    for(auto& t:turns){ if(t.is_long_turn)n_long_turns++; else n_short_turns++; }

    printf("Found %d LONG turns, %d SHORT turns\n\n", n_long_turns, n_short_turns);

    // Print each turn with DOM context
    printf("%-6s %-8s %-6s %-8s %-8s %-8s %-12s\n",
           "Turn#","Type","RSI","Drift","DOM-5","DOM0","Price+20ticks");
    printf("%s\n", std::string(58,'-').c_str());

    for(int i=0;i<(int)turns.size();i++){
        auto& t=turns[i];
        int ti=t.tick_idx;
        // DOM average in 5 ticks before turn
        double dom_before=0;
        for(int j=5;j>=1;j--) dom_before+=t.is_long_turn?ticks[ti-j].dom_long:ticks[ti-j].dom_short;
        dom_before/=5;
        // DOM at turn tick
        double dom_at=t.is_long_turn?ticks[ti].dom_long:ticks[ti].dom_short;

        printf("%-6d %-8s %-6.1f %-8.2f %-8.2f %-8.0f $%-12.2f\n",
               i+1, t.is_long_turn?"LONG":"SHORT",
               t.rsi_at_turn, t.drift_at_turn,
               dom_before, dom_at, t.price_change_after_10s*100.0);
    }

    // Summary: avg DOM score at each offset relative to turn
    printf("\n=== AVG DOM SCORE RELATIVE TO RSI TURN ===\n");
    printf("Positive = DOM confirming trade direction\n");
    printf("Offset = ticks before(-) / after(+) the RSI turn bar\n\n");

    if(n_long_turns>0){
        printf("LONG TURNS (%d total):\n", n_long_turns);
        printf("  Before turn: ");
        for(int i=WINDOW-5;i<WINDOW;i++)
            printf("[-%d]=%.2f ", WINDOW-i, sum_dom_long_before[i]/n_long_turns);
        printf("\n  After turn:  ");
        for(int i=0;i<5;i++)
            printf("[+%d]=%.2f ", i+1, sum_dom_long_after[i]/n_long_turns);
        printf("\n\n");
    }
    if(n_short_turns>0){
        printf("SHORT TURNS (%d total):\n", n_short_turns);
        printf("  Before turn: ");
        for(int i=WINDOW-5;i<WINDOW;i++)
            printf("[-%d]=%.2f ", WINDOW-i, sum_dom_short_before[i]/n_short_turns);
        printf("\n  After turn:  ");
        for(int i=0;i<5;i++)
            printf("[+%d]=%.2f ", i+1, sum_dom_short_after[i]/n_short_turns);
        printf("\n\n");
    }

    return 0;
}
