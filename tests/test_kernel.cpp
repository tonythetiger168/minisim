// Exercise the actual kernel and awaiters; suppress only the demo main.
#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include <sstream>
#include <stdexcept>

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::vector<int> observed;
int a, b;
Task zero_time_loop() { for (;;) co_await Delay{0}; }
Task delayed_error() { co_await Delay{1}; S.nba(a, mk(1)); throw std::runtime_error("bad process"); }
Task boundary_delay(uint64_t delay) { co_await Delay{delay}; observed.push_back(1); }
Task advancing_loop() {
    for (int i=0; i<20; ++i) { observed.push_back(i); co_await Delay{1}; }
}

Task nba_writer() {
    S.set(a, mk(0, 4));
    S.nba(a, mk(3, 2), 1, 0);
    S.nba(a, mk(2, 2), 3, 2);
    observed.push_back(S.get(a).v);
    co_await Delay{0};
    observed.push_back(S.get(a).v);  // #0 precedes NBA
    co_await Delay{1};
    observed.push_back(S.get(a).v);
}
Task multi_waiter() {
    co_await WaitAny{{{a, POS}, {b, POS}}};
    observed.push_back(1);
    co_await Delay{2};
    observed.push_back(2);
}
Task multi_writer() {
    S.set(a, mk(0)); S.set(b, mk(0));
    co_await Delay{1};
    S.set(a, mk(1)); S.set(b, mk(1));
}
Task edge_waiter() {
    for (int i = 0; i < 3; ++i) {
        co_await Wait{a, POS};
        observed.push_back(S.now);
    }
}
Task edge_writer() {
    S.set(a, mk(0));
    co_await Delay{1}; S.set(a, allx(1)); // 0 -> X is posedge
    co_await Delay{1}; S.set(a, allz(1)); // X -> Z is not posedge
    co_await Delay{1}; S.set(a, mk(1));   // Z -> 1 is posedge
    co_await Delay{1}; S.set(a, mk(0));
    co_await Delay{1}; S.set(a, mk(1));
}
Task finish_now() { S.finished = true; co_return; }
Task should_not_run() { observed.push_back(99); co_return; }
Task finish_with_nba() { S.nba(a, mk(1)); S.finished = true; co_return; }

int main(int argc, char** argv) {
    try {
        require(argc == 2, "scenario required");
        std::string scenario = argv[1];
        a = S.sig("a", scenario == "nba" ? 4 : 1);
        b = S.sig("b", 1);
        if (scenario == "process_exception") {
            S.set(a, mk(0)); S.spawn(delayed_error()); bool caught = false;
            try { S.run(2); } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "process error at time 1: bad process";
            }
            require(caught && S.finished && S.get(a).v == 0, "process error not propagated before NBA");
        } else if (scenario == "delay_overflow") {
            S.now = std::numeric_limits<uint64_t>::max()-1;
            S.spawn(boundary_delay(2)); bool caught = false;
            try { S.run(std::numeric_limits<uint64_t>::max()); }
            catch (const std::runtime_error& error) {
                caught = std::string(error.what()).find("delay overflow") != std::string::npos;
            }
            require(caught && S.finished && S.wheel.empty() && observed.empty(), "overflow scheduled past event");
        } else if (scenario == "delay_boundary") {
            S.now = std::numeric_limits<uint64_t>::max()-1;
            S.spawn(boundary_delay(1)); S.run(std::numeric_limits<uint64_t>::max());
            require(S.now == std::numeric_limits<uint64_t>::max() && observed.size() == 1 && !S.finished,
                    "legal maximum timestamp rejected");
        } else if (scenario == "ir_budget") {
            S.max_ir_instructions_per_resume = 8;
            Prog p; Instr jump; jump.k = IK::Jmp; jump.target = 0; p.emit(jump);
            S.spawn(exec(&p)); bool caught = false;
            try { S.run(1); } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "IR instruction budget exceeded at time 0 pc 0";
            }
            require(caught && S.finished, "non-yielding IR loop was not bounded");
        } else if (scenario == "ir_budget_wait") {
            S.max_ir_instructions_per_resume = 2;
            Prog p; Instr wait; wait.k = IK::WaitDelay; wait.delay = 1; p.emit(wait);
            Instr jump; jump.k = IK::Jmp; jump.target = 0; p.emit(jump);
            S.spawn(exec(&p)); S.run(20);
            require(S.now == 20 && !S.finished && S.execution_error.empty(), "IR budget did not reset at waits");
        } else if (scenario == "resume_budget") {
            S.max_resumes_per_slot = 16;
            S.spawn(zero_time_loop());
            bool caught = false;
            try { S.run(1); } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "process resume budget exceeded at time 0";
            }
            require(caught && S.finished && S.now == 0, "zero-time process was not bounded");
        } else if (scenario == "resume_budget_time") {
            S.max_resumes_per_slot = 1;
            S.spawn(advancing_loop()); S.run(20);
            require(observed.size() == 20 && !S.finished, "budget did not reset per time slot");
        } else if (scenario == "nba") {
            S.spawn(nba_writer()); S.run(2);
            require(observed == std::vector<int>({0, 0, 11}), "NBA/#0/partial write ordering");
        } else if (scenario == "multi_wait") {
            S.spawn(multi_waiter()); S.spawn(multi_writer()); S.run(5);
            require(observed == std::vector<int>({1, 2}), "multi-event wait resumed twice");
        } else if (scenario == "edges") {
            S.spawn(edge_waiter()); S.spawn(edge_writer()); S.run(6);
            require(observed == std::vector<int>({1, 3, 5}), "four-state edge detection");
        } else if (scenario == "finish_active") {
            S.spawn(finish_now()); S.spawn(should_not_run()); S.run(1);
            require(observed.empty(), "active process ran after finish");
        } else if (scenario == "finish_nba") {
            S.set(a, mk(0)); S.spawn(finish_with_nba()); S.run(1);
            require(S.get(a).v == 0, "NBA applied after finish");
        } else if (scenario == "finish_ir") {
            Prog p;
            Instr finish; finish.k = IK::Finish; p.emit(finish);
            Instr assign; assign.k = IK::Assign; assign.sig = a;
            assign.e = konst(mk(1)); p.emit(assign);
            S.spawn(exec(&p)); S.run(1);
            require(S.get(a).x == 1, "IR continued after finish");
        } else if (scenario == "vcd_initial") {
            S.run(0); S.vcd.flush();
            std::ifstream input("dump.vcd");
            std::stringstream text; text << input.rdbuf();
            require(text.str().find("#0\nx!\nx\"\n") != std::string::npos,
                    "VCD missing initial values for unchanged signals");
        } else throw std::runtime_error("unknown scenario");
        std::cout << scenario << " PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
