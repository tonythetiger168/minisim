#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include <stdexcept>
void require(bool value,const char* msg) { if(!value) throw std::runtime_error(msg); }
Task latch(int clk,int q) { for(;;) { co_await Wait{clk,POS}; S.nba(q,mk(1)); } }
int main() {
    try {
        int clk=S.sig("clk",1),q=S.sig("q",1);
        S.set(clk,mk(0)); S.set(q,mk(0)); S.spawn(latch(clk,q)); S.advance_to(0);
        std::vector<std::string> order;
        int canceled=0;
        auto id=S.on_timer(1,[&]{++canceled;});
        require(S.cancel_callback(id) && !S.cancel_callback(id),"cancel lifecycle");
        S.on_timer(1,[&] {
            order.push_back("timer"); S.set(clk,mk(1));
        });
        S.on_edge(clk,POS,[&] {
            order.push_back("edge"); require(S.get(q).v==0,"edge ran after downstream NBA");
        });
        S.on_readwrite([&] {
            order.push_back("rw"); require(S.get(q).v==0,"early RW before timer");
            S.nba(q,mk(0));
        });
        S.advance_to(0); // RW belongs to registration time, not next timer
        S.on_timer(1,[&] {
            S.on_readwrite([&] {
                order.push_back("rw1"); require(S.get(q).v==1,"RW missed NBA");
                S.nba(q,mk(0));
            });
            S.on_readonly([&] {
                order.push_back("ro");require(S.get(q).v==0,"RO before RW NBA settled");
                bool blocked=false;
                try { S.set(q,mk(1)); } catch(const std::runtime_error&) {blocked=true;}
                require(blocked,"ReadOnly write accepted");
                bool reentrant=false;
                try {S.advance_to(1);} catch(const std::runtime_error&) {reentrant=true;}
                require(reentrant,"reentrant advance accepted");
            });
        });
        S.advance_to(1);
        require(order==std::vector<std::string>({"rw","timer","edge","rw1","ro"}),"callback order");
        require(canceled==0,"canceled timer fired");
        bool closed=false;
        try {S.nba(q,mk(1));} catch(const std::runtime_error&) {closed=true;}
        require(closed,"ReadOnly close did not persist");
        S.advance_to(2); S.set(clk,mk(0)); S.advance_to(2);
        S.set(clk,mk(1)); S.advance_to(2);
        require(S.get(q).v==1 && order.size()==5,"edge callback not one-shot");
        auto pending=S.on_edge(q,ANY,[&]{++canceled;});
        S.set(q,mk(0)); require(S.cancel_callback(pending),"queued cancellation"); S.advance_to(2);
        require(canceled==0,"queued canceled callback fired");
        bool zero=false;try{S.on_timer(0,[]{});}catch(const std::invalid_argument&){zero=true;}
        require(zero,"zero timer accepted");
        S.max_callbacks_per_slot=4;
        S.advance_to(3);
        std::function<void()> loop;loop=[&]{S.on_readwrite(loop);};S.on_readwrite(loop);
        bool bounded=false;
        try {S.advance_to(3);}catch(const std::runtime_error& error){bounded=std::string(error.what()).find("callback budget")!=std::string::npos;}
        require(bounded && S.finished,"callback loop not bounded");
        std::cout<<"callback phases/lifecycle/budget PASS\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
