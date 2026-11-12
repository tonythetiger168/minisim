#define MINISIM_NO_DEMO
#include "minisim.cpp"
#include "design.hpp"
#include <stdexcept>

GeneratedDesign design;
Task stimulus() {
    int clk = design.signals.at("clk"), rst = design.signals.at("rst");
    int count = design.signals.at("cnt");
    S.set(clk, mk(0));
    for (int cycle = 0; cycle < 96; ++cycle) {
        int reset = cycle == 0 || cycle == 37 || cycle == 38 || cycle == 58;
        Val reset_value = cycle == 20 ? allx(1) : cycle == 50 ? allz(1) : mk(reset);
        S.set(rst, reset_value);
        co_await Delay{1};
        S.set(clk, mk(1));
        co_await Delay{1};
        Val value = S.get(count);
        if (value.x) throw std::runtime_error("unexpected unknown counter after reset");
        std::cout << cycle << " " << value.v << '\n';
        S.set(clk, mk(0));
    }
    S.finished = true;
}
int main() {
    S.collect_coverage = true;
    design = build_generated_design();
    for (auto& program : design.programs) S.spawn(exec(&program));
    S.spawn(stimulus());
    S.run(300);
    std::ofstream line("line.json"); S.line_coverage.write_json(line);
    if (!line) throw std::runtime_error("cannot write line coverage");
}
