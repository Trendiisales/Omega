// qndx_sqf_ibkr.cpp -- the 4th Omega TU (after ibkr_exec.cpp + bigcap_momo_ibkr.cpp +
// nq_momo_ibkr.cpp) that pulls in the TWS API, for the in-process QNDX (Nasdaq-100 SQF)
// book folded from the ex-IBKRCrypto executor.
//
// Implements the thin omega::qndx_sqf_ibkr:: interface (QndxSqfIbkr.hpp) by wrapping a
// file-static EWrapper engine on its own reader/pump thread. TWS headers stay out of the
// giant main Omega TU (the std::atoll-pollution rule). Compiled into Omega.exe only
// (OMEGA_WITH_IBKR); degrades to safe no-ops elsewhere.
//
// The signal + sizing is a VERBATIM port of the ex-IBKRCrypto QndxStrat (QndxStrat.hpp):
// TWO orthogonal DAILY legs -- TSMom50 (trend) + RSIrev (mean-rev), both vol-targeted,
// long+short. Differences from the NqMomo sibling this file is modelled on:
//   * DAILY cadence, not 5m -- warm from a daily OHLC CSV (SQF has no IB HMDS history),
//     re-evaluate only on a NEW completed daily close.
//   * TWO legs netted into one QNDX contract position (idempotent MKT delta routing),
//     each leg shown as its own GUI row.
//   * LONG + SHORT (SQF can short), vs NqMomo LONG-only.
//   * Protection = vol-target sizing + EXIT-ON-TURN (leg flips/closes when its own signal
//     reverses). NO ATR trail / BE-ratchet / maxhold / cold-loss cut -- a per-trade stop
//     GUTS the trend edge (backtested verdict; see QndxSqfIbkr.hpp / QndxStrat.hpp header).
#include "QndxSqfIbkr.hpp"

#ifdef OMEGA_WITH_IBKR

#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Contract.h"
#include "Order.h"

#include "QndxStrat.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace omega {
namespace qndx_sqf_ibkr {
namespace {

using qndx::QndxStrat;
using qndx::StratParams;
using qndx::Mode;

// One validated QNDX leg: its rolling strategy + the position it currently intends to hold.
struct Leg {
    QndxStrat strat;
    const char* tag;       // engine sub-tag (GUI/ledger source, per-leg)
    int    pos    = 0;     // current target/held position: -1 / 0 / +1
    int    qty    = 0;     // sized whole contracts for this leg (base * size_mult, rounded)
    double entry  = 0;     // entry price of the current leg position
    double peak   = 0;     // MFE tracking (favourable extreme since entry)
    double trough = 0;     // MAE tracking
    int64_t entry_ts = 0;
    Leg(Mode m, const char* t) : strat(m), tag(t) {}
};

static long day_index(long epoch_sec){ return epoch_sec / 86400; }  // UTC day bucket for new-day detection

class Engine : public DefaultEWrapper {
    EReaderOSSignal sig_{2000};
    std::unique_ptr<EClientSocket> cli_;
    std::unique_ptr<EReader> rd_;
    Config cfg_;
    OrderId nextId_=0;
    static const int BAR_REQ=7200;                 // single QNDX daily bar feed reqId
    // two legs: TSMom50 trend + RSIrev mean-rev (the validated QNDX roster)
    Leg leg_trend_{Mode::TSMOM,  "QndxSqfTrend"};
    Leg leg_mr_   {Mode::RSIREV, "QndxSqfMeanRev"};
    Leg* legs_[2] = { &leg_trend_, &leg_mr_ };
    // forming daily bar (assembled from keepUpToDate updates)
    long   hb_day_=-1; double hb_o_=0, hb_h_=0, hb_l_=0, hb_c_=0;
    long   last_day_=-1;                            // last FINALIZED daily bar's day bucket
    bool   warm_done_=false, armed_=false;
    int    net_pos_=0;                             // last NET contracts routed to the broker
    double last_px_=0;                             // latest close (unrealized / marks)
    std::mutex book_mu_;
    std::function<void(const TradeRecord&)> on_trade_record_;
    int trade_id_=0;
    std::atomic<bool> handshake_done_{false};
    std::atomic<long long> last_activity_ms_{0};
    static long long now_ms(){ return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); }

    Contract contract() const {
        Contract c; c.symbol=cfg_.symbol; c.secType=cfg_.sec_type;
        c.exchange=cfg_.exchange; c.currency=cfg_.currency;
        if(!cfg_.last_trade_month.empty()) c.lastTradeDateOrContractMonth=cfg_.last_trade_month;
        return c;  // EMPTY month -> reqContractDetails/front resolution by the gateway (SQF product code)
    }

    // Warm both legs from the daily OHLC CSV. Establishes indicator buffers so entries are
    // armed on connect (warm-seed mandate; TSMom50 needs 51 daily bars). Sets last_day_ to
    // the last CSV bar so live gap-fill / finalize continues from there. NO acting here.
    void warm_from_csv(){
        std::ifstream f(cfg_.daily_csv);
        if(!f){ printf("[QndxSqf] WARN: daily CSV not found: %s -- legs cold, entries wait for live bars\n",
                       cfg_.daily_csv.c_str()); fflush(stdout); return; }
        std::string line; size_t nbars=0;
        while(std::getline(f,line)){
            if(line.empty()) continue;
            std::stringstream ss(line); std::string tok; double v[5]; int k=0; bool bad=false;
            while(std::getline(ss,tok,',') && k<5){
                char* end=nullptr; v[k]=std::strtod(tok.c_str(),&end);
                if(end==tok.c_str()){ bad=true; break; }   // header / non-numeric -> skip row
                ++k;
            }
            if(bad || k<5) continue;                        // need time,o,h,l,c
            qndx::Bar b{v[1],v[2],v[3],v[4]};
            leg_trend_.strat.on_daily_bar(b);
            leg_mr_.strat.on_daily_bar(b);
            last_day_ = day_index((long)v[0]);
            last_px_  = v[4];
            ++nbars;
        }
        warm_done_=true;
        printf("[QndxSqf] warm-seed: %zu daily bars from CSV, last_day=%ld last_px=%.2f "
               "(trend warm=%d, mr warm=%d)\n", nbars, last_day_, last_px_,
               (int)leg_trend_.strat.warm(), (int)leg_mr_.strat.warm()); fflush(stdout);
    }

    // Feed a completed daily bar to both legs, then reconcile each leg's held position to
    // its new target (EXIT-ON-TURN). `act` gates order routing + closed-trade records so
    // the CSV->today gap-fill only warms, while live finalize + the on-connect adopt act.
    void ingest_daily_bar(const qndx::Bar& b, bool act){
        last_px_ = b.c;
        for(Leg* lg : legs_){
            lg->strat.on_daily_bar(b);
            if(!act || !lg->strat.warm()) continue;
            int tgt = lg->strat.target();
            if(tgt == lg->pos){                         // no turn: just track extremes
                if(lg->pos!=0){ if(b.h>lg->peak) lg->peak=b.h; if(b.l<lg->trough||lg->trough<=0) lg->trough=b.l; }
                continue;
            }
            // EXIT-ON-TURN: close the current leg position (if any) at this close.
            if(lg->pos!=0) record_leg_close(*lg, b.c, "FLIP");
            // enter the new target (if non-flat), sized by vol-target.
            if(tgt!=0){
                int q = (int)std::lround((double)cfg_.base_contracts * lg->strat.size_mult());
                if(q<1) q=0;                            // vol-target sized this leg out -> stay flat
                lg->pos=tgt; lg->qty=q; lg->entry=b.c; lg->peak=b.c; lg->trough=b.c;
                lg->entry_ts=(int64_t)std::time(nullptr);
                if(q>0){ printf("[QndxSqf] %s %s %s @ %.2f qty=%d (vt=%.2f)\n",
                    cfg_.paper_only?"PAPER":"LIVE", lg->tag, tgt>0?"LONG":"SHORT", b.c, q,
                    lg->strat.size_mult()); fflush(stdout); }
            } else { lg->pos=0; lg->qty=0; }
        }
        if(act) rebalance_net();
    }

    // Idempotent net-delta MKT routing: sum the legs' signed contracts, route the delta to
    // move the broker QNDX position from net_pos_ to the new net. paper_only => log only.
    void rebalance_net(){
        int net=0; for(Leg* lg : legs_) net += lg->pos * lg->qty;
        int delta = net - net_pos_;
        if(delta==0) return;
        printf("[QndxSqf] %s REBALANCE net=%d (was %d) delta=%+d @ %.2f\n",
               cfg_.paper_only?"PAPER":"LIVE", net, net_pos_, delta, last_px_); fflush(stdout);
        if(!cfg_.paper_only){
            Order ord; ord.action = delta>0 ? "BUY" : "SELL"; ord.orderType="MKT";
            ord.totalQuantity = DecimalFunctions::doubleToDecimal((double)std::abs(delta));
            cli_->placeOrder(nextId_++, contract(), ord);
        }
        net_pos_ = net;
    }

    // Emit a closed-trade record for a leg turning off its current position. book_mu_ HELD.
    void record_leg_close(Leg& lg, double px, const char* why){
        double pnl = (px - lg.entry) * cfg_.point_value * lg.pos * lg.qty;   // signed by pos
        printf("[QndxSqf] %s EXIT %s %s entry=%.2f exit=%.2f qty=%d pnl=%.2f (%s)\n",
               cfg_.paper_only?"PAPER":"LIVE", lg.tag, lg.pos>0?"LONG":"SHORT",
               lg.entry, px, lg.qty, pnl, why); fflush(stdout);
        if(on_trade_record_ && lg.qty>0){
            TradeRecord tr;
            tr.id         = ++trade_id_;
            tr.symbol     = cfg_.symbol;
            tr.side       = lg.pos>0 ? "LONG" : "SHORT";
            tr.entryPrice = lg.entry;
            tr.exitPrice  = px;
            tr.tp         = 0.0;                       // runner: no TP (exit-on-turn)
            tr.sl         = 0.0;                       // no stop by design
            tr.size       = (double)lg.qty;
            tr.pnl        = pnl;
            tr.mfe        = (lg.pos>0 ? (lg.peak-lg.entry) : (lg.entry-lg.trough)) * cfg_.point_value * lg.qty;
            tr.mae        = (lg.pos>0 ? (lg.entry-lg.trough) : (lg.peak-lg.entry)) * cfg_.point_value * lg.qty;
            tr.entryTs    = lg.entry_ts;
            tr.exitTs     = (int64_t)std::time(nullptr);
            tr.exitReason = "SIGNAL_FLIP";
            tr.engine     = lg.tag;
            tr.shadow     = cfg_.paper_only;
            on_trade_record_(tr);
        }
        lg.pos=0; lg.qty=0; lg.peak=0; lg.trough=0;
    }

    // On-connect adopt: after warm + gap-fill, take each leg's current target as the held
    // position and route to net (no idle wait -- warm-seed mandate).
    void adopt_on_connect(){
        for(Leg* lg : legs_){
            if(!lg->strat.warm()) continue;
            int tgt=lg->strat.target();
            if(tgt==0){ lg->pos=0; lg->qty=0; continue; }
            int q=(int)std::lround((double)cfg_.base_contracts * lg->strat.size_mult());
            if(q<1){ lg->pos=0; lg->qty=0; continue; }
            lg->pos=tgt; lg->qty=q; lg->entry=last_px_; lg->peak=last_px_; lg->trough=last_px_;
            lg->entry_ts=(int64_t)std::time(nullptr);
            printf("[QndxSqf] adopt %s %s @ %.2f qty=%d (vt=%.2f)\n", lg->tag,
                   tgt>0?"LONG":"SHORT", last_px_, q, lg->strat.size_mult()); fflush(stdout);
        }
        armed_=true;
        rebalance_net();
    }

public:
    Engine(){ cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void set_config(const Config& c){ cfg_=c; }
    void set_client_id(int id){ cfg_.client_id = id; }
    bool handshake_done() const { return handshake_done_.load(std::memory_order_relaxed); }
    long long last_activity_ms() const { return last_activity_ms_.load(std::memory_order_relaxed); }
    void set_sink(std::function<void(const TradeRecord&)> cb){ on_trade_record_=std::move(cb); }

    void log_heartbeat(){
        long long la=last_activity_ms_.load(std::memory_order_relaxed);
        double ago = la>0 ? (now_ms()-la)/1000.0 : -1.0;
        int tp, mp; { std::lock_guard<std::mutex> lk(book_mu_); tp=leg_trend_.pos; mp=leg_mr_.pos; }
        printf("[QndxSqf] ALIVE %s trend=%+d mr=%+d net=%d last_px=%.2f last_data=%s\n",
               cfg_.symbol.c_str(), tp, mp, net_pos_, last_px_,
               ago<0?"NEVER":(std::to_string((long)ago)+"s ago").c_str()); fflush(stdout);
    }

    std::vector<PositionSnapshot> collect_positions(){
        std::vector<PositionSnapshot> v;
        std::lock_guard<std::mutex> lk(book_mu_);
        for(Leg* lg : legs_){
            if(lg->pos==0 || lg->qty==0) continue;
            PositionSnapshot ps;
            ps.symbol         = cfg_.symbol;
            ps.side           = lg->pos>0 ? "LONG" : "SHORT";
            ps.size           = (double)lg->qty;
            ps.entry          = lg->entry;
            ps.current        = last_px_;
            ps.unrealized_pnl = (last_px_ - lg->entry) * cfg_.point_value * lg->pos * lg->qty;
            ps.mfe            = (lg->pos>0 ? (lg->peak-lg->entry) : (lg->entry-lg->trough)) * cfg_.point_value * lg->qty;
            ps.mae            = (lg->pos>0 ? (lg->entry-lg->trough) : (lg->peak-lg->entry)) * cfg_.point_value * lg->qty;
            ps.engine         = lg->tag;
            ps.entry_ts       = lg->entry_ts;
            ps.tp             = 0.0;
            ps.sl             = 0.0;
            v.push_back(ps);
        }
        return v;
    }

    void nextValidId(OrderId id) override {
        nextId_=id;
        handshake_done_.store(true, std::memory_order_relaxed);
        cli_->reqMarketDataType(cfg_.market_data_type);
        // Daily bars via reqHistoricalData(keepUpToDate). Legs are already warmed from CSV;
        // the seed rows here only gap-fill CSV->today, and the forming (today) bar streams
        // via historicalDataUpdate. formatDate=2 -> bar.time epoch sec; useRTH=1 (index).
        Contract c=contract();
        cli_->reqHistoricalData(BAR_REQ, c, "", "30 D", "1 day", "TRADES", 1, 2, true, TagValueListSPtr());
        printf("[QndxSqf] subscribe %s %s%s DAILY (TSMom50 + RSIrev, vol-target 2%%/day, exit-on-turn) %s\n",
               cfg_.symbol.c_str(), c.secType.c_str(),
               cfg_.last_trade_month.empty()?"(front)":(" "+cfg_.last_trade_month).c_str(),
               cfg_.paper_only?"PAPER":"LIVE"); fflush(stdout);
    }

    // seed rows: completed daily bars. Gap-fill any bar AFTER the last CSV day (warm only).
    void historicalData(TickerId rid, const Bar& b) override {
        if((int)rid!=BAR_REQ) return;
        std::lock_guard<std::mutex> lk(book_mu_);
        long d=day_index(atol(b.time.c_str()));
        if(warm_done_ && d>last_day_){ ingest_daily_bar({b.open,b.high,b.low,b.close}, /*act=*/false); last_day_=d; }
        else { last_px_=b.close; }
    }
    void historicalDataEnd(int rid,const std::string&,const std::string&) override {
        if(rid!=BAR_REQ) return;
        std::lock_guard<std::mutex> lk(book_mu_);
        if(!armed_){ adopt_on_connect();
            printf("[QndxSqf] seeded+armed: last_day=%ld last_px=%.2f -- live daily finalize armed\n",
                   last_day_, last_px_); fflush(stdout); }
    }
    void historicalDataUpdate(TickerId rid, const Bar& b) override {
        if((int)rid!=BAR_REQ) return;
        last_activity_ms_.store(now_ms(), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(book_mu_);
        long d=day_index(atol(b.time.c_str()));
        last_px_=b.close;
        if(hb_day_<0){ hb_day_=d; hb_o_=b.open; hb_h_=b.high; hb_l_=b.low; hb_c_=b.close; return; }
        if(d==hb_day_){ hb_o_=b.open; hb_h_=b.high; hb_l_=b.low; hb_c_=b.close; return; }
        // day advanced -> the forming bar (hb_day_) is now a COMPLETED daily bar -> finalize.
        if(armed_ && hb_day_>last_day_){ ingest_daily_bar({hb_o_,hb_h_,hb_l_,hb_c_}, /*act=*/true); last_day_=hb_day_; }
        hb_day_=d; hb_o_=b.open; hb_h_=b.high; hb_l_=b.low; hb_c_=b.close;
    }

    bool connect(){
        handshake_done_.store(false, std::memory_order_relaxed);
        if(!cli_->eConnect(cfg_.host.c_str(),cfg_.port,cfg_.client_id,false)) return false;
        rd_=std::make_unique<EReader>(cli_.get(),&sig_); rd_->start();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while(std::chrono::steady_clock::now() < deadline){
            rd_->processMsgs();
            if(handshake_done_.load(std::memory_order_relaxed)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    }
    void hard_disconnect(){ if(cli_) cli_->eDisconnect(); rd_.reset();
                            cli_=std::make_unique<EClientSocket>(this,&sig_); }
    void pump(){ sig_.waitForSignal(); rd_->processMsgs(); }
    void disconnect(){ if(cli_) cli_->eDisconnect(); }
    void do_warm(){ std::lock_guard<std::mutex> lk(book_mu_); warm_from_csv(); }

    void error(int,int code,const std::string& m,const std::string&) override {
        if(code!=2104&&code!=2106&&code!=2158&&code!=162&&code!=200&&code!=2150&&code!=10090)
            printf("[QndxSqf] err %d: %s\n",code,m.c_str()); }
};

// ── file-static singleton + thread lifecycle (mirrors nq_momo_ibkr) ──
static Engine&            eng()        { static Engine e; return e; }
static Config             g_cfg{};
static std::atomic<bool>  g_enabled{false};
static std::atomic<bool>  g_running{false};
static std::atomic<bool>  g_stop{false};
static std::thread        g_thread;

} // anonymous namespace

void configure(const Config& cfg){ g_cfg=cfg; eng().set_config(cfg); }
void set_enabled(bool on){ g_enabled.store(on); }
bool is_enabled(){ return g_enabled.load(); }
long long last_activity_ms(){ return g_running.load() ? eng().last_activity_ms() : 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)> cb){ eng().set_sink(std::move(cb)); }

std::vector<PositionSnapshot> collect_positions(){
    if(!g_enabled.load() || !g_running.load()) return {};
    return eng().collect_positions();
}

// paper/live port-vs-routing guard (mirrors nq_momo_ibkr::paper_port_guard)
static bool paper_port_guard(const Config& c){
    const bool live_port  = (c.port==7496 || c.port==4001);
    const bool paper_port = (c.port==7497 || c.port==4002);
    if(!c.paper_only){
        if(paper_port){
            printf("[QndxSqf] ABORT: paper_only=false (LIVE order routing armed) but port %d is a "
                   "PAPER gateway -- inconsistent config. Refusing to start.\n", c.port); fflush(stdout);
            return false;
        }
        printf("[QndxSqf] *** LIVE ORDER ROUTING ARMED *** paper_only=false on %s:%d -- REAL orders "
               "WILL be sent.\n", c.host.c_str(), c.port); fflush(stdout);
    } else if(live_port){
        printf("[QndxSqf] shadow OK: paper_only=true on live gateway %d -- logging only, NO orders.\n",
               c.port); fflush(stdout);
    }
    return true;
}

bool start(){
    if(!g_enabled.load()) return false;
    if(!paper_port_guard(g_cfg)){ g_running.store(false); return false; }
    if(g_running.exchange(true)) return true;
    eng().set_config(g_cfg);
    eng().do_warm();                                   // warm legs from CSV before connect
    const int base_id = g_cfg.client_id;
    bool live=false; int win_id=base_id;
    for(int attempt=0; attempt<3 && !live; ++attempt){
        const int id=base_id+attempt;
        eng().set_client_id(id);
        if(eng().connect()){
            win_id=id; live=true;
            printf("[QndxSqf] API handshake OK (nextValidId) -- book live %s:%d id=%d\n",
                   g_cfg.host.c_str(), g_cfg.port, id); fflush(stdout);
            break;
        }
        printf("[QndxSqf] connect/handshake FAILED %s:%d id=%d (attempt %d/3) -- %s\n",
               g_cfg.host.c_str(), g_cfg.port, id, attempt+1,
               attempt<2 ? "TCP up but no nextValidId; rotating clientId" : "giving up"); fflush(stdout);
        eng().hard_disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if(!live){
        printf("[QndxSqf] DOWN -- no API handshake after 3 clientId attempts (%d-%d).\n",
               base_id, base_id+2); fflush(stdout);
        g_running.store(false);
        return false;
    }
    g_stop.store(false);
    g_thread = std::thread([win_id]{
        printf("[QndxSqf] in-process IBKR book thread up (%s:%d id=%d)\n",
               g_cfg.host.c_str(), g_cfg.port, win_id); fflush(stdout);
        auto last_hb = std::chrono::steady_clock::now();
        while(!g_stop.load()){
            eng().pump();
            auto now = std::chrono::steady_clock::now();
            if(now - last_hb >= std::chrono::seconds(60)){ eng().log_heartbeat(); last_hb = now; }
        }
        eng().disconnect();
        printf("[QndxSqf] book thread stopped\n"); fflush(stdout);
    });
    return true;
}

void stop(){
    if(!g_running.load()) return;
    g_stop.store(true);
    if(g_thread.joinable()) g_thread.join();
    g_running.store(false);
}

} // namespace qndx_sqf_ibkr
} // namespace omega

#else  // !OMEGA_WITH_IBKR -- safe no-op stubs

namespace omega {
namespace qndx_sqf_ibkr {
void configure(const Config&) {}
void set_enabled(bool) {}
bool is_enabled() { return false; }
long long last_activity_ms() { return 0; }
void set_on_trade_record(std::function<void(const TradeRecord&)>) {}
std::vector<PositionSnapshot> collect_positions() { return {}; }
bool start() { return false; }
void stop() {}
} // namespace qndx_sqf_ibkr
} // namespace omega

#endif
