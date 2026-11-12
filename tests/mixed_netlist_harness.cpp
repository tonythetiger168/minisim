#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "design.hpp"
GeneratedDesign design;
Task stimulus() {
    int clk=design.signals.at("clk"), rst=design.signals.at("rst"), cnt=design.signals.at("cnt");
    S.set(clk,mk(0));
    for (int cycle=0; cycle<40; ++cycle) {
        S.set(rst,mk(cycle==0 || cycle==19));
        co_await Delay{1}; S.set(clk,mk(1));
        co_await Delay{1};
        Val result=S.get(cnt);
        if (result.x) { std::cerr << "unknown mixed output\n"; S.finished=true; co_return; }
        std::cout << "TRACE " << cycle << ' ' << result.v << '\n';
        S.set(clk,mk(0));
    }
    S.finished=true;
}
int main() {
    S.collect_coverage=true;
    design=build_generated_design();
    for (auto& process:design.programs) S.spawn(exec(&process));
    S.spawn(stimulus()); S.run(100);
    std::ofstream out("line.json"); S.line_coverage.write_json(out);
}
