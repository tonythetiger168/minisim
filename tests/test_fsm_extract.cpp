// fsm_extract.hpp alone, with only slang: the state machines of tests/cov/fsm.sv
// and tests/cov/fsm_onehot.sv as a lint rule or a debug view would read them.
#include <iostream>
#include <string>

#include "fsm_extract.hpp"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

static int failed = 0;
static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL " << what << "\n";
        failed++;
    }
}

static void run(const char* rel, bool print) {
    auto tree = slang::syntax::SyntaxTree::fromFile(std::string(MINISIM_SOURCE_DIR) + "/" + rel);
    if (!tree) {
        check(false, std::string("cannot read ") + rel);
        return;
    }
    slang::ast::Compilation comp;
    comp.addSyntaxTree(*tree);
    comp.getAllDiagnostics();
    auto fsms = extractFsms(comp.getRoot());
    check(!fsms.empty(), std::string(rel) + ": no FSM found");
    const slang::SourceManager* sm = comp.getSourceManager();
    for (const ExtractedFsm& f : fsms) {
        std::string id = f.inst + "." + f.name;
        check(f.var != nullptr, id + ": state register symbol");
        check(f.states.size() >= 2, id + ": states");
        check(!f.trans.empty(), id + ": transitions");
        check(!f.module.empty() && !f.inst.empty(), id + ": module and instance");
        for (size_t i = 1; i < f.states.size(); i++) check(f.states[i - 1].first < f.states[i].first, id + ": states in value order");
        auto isState = [&](uint64_t v) {
            for (auto& s : f.states)
                if (s.first == v) return true;
            return false;
        };
        for (const auto& t : f.trans) {
            check(isState(t.from) && isState(t.to) && t.from != t.to, id + ": transition between two states");
            check(t.loc.valid(), id + ": transition source location");
        }
        for (uint64_t r : f.resetStates) check(isState(r), id + ": reset state is a state");
        if (f.onehot)
            for (auto& s : f.states) check(s.first && !(s.first & (s.first - 1)), id + ": one-hot state value");
        if (print) {
            std::cout << f.module << " " << id << " w=" << f.w << (f.onehot ? " onehot" : "") << (f.caseOnState ? " case" : "")
                      << (f.caseDefault ? " default" : "") << (f.caseFull ? " full" : "") << (f.next ? " next=" + std::string(f.next->name) : "") << "\n  states:";
            for (auto& s : f.states) std::cout << " " << s.second << "=" << s.first;
            std::cout << "\n  reset:";
            for (uint64_t r : f.resetStates) std::cout << " " << r;
            std::cout << "\n  trans:";
            for (const auto& t : f.trans)
                std::cout << " " << t.from << ">" << t.to << (t.reset ? "r" : "") << "@" << sm->getLineNumber(sm->getFullyOriginalLoc(t.loc));
            std::cout << "\n";
        }
    }
}

int main(int argc, char** argv) {
    bool print = argc > 1;
    // unique case, priority case and (* full_case *) with no default are full; a plain one is not.
    {
        auto tree = slang::syntax::SyntaxTree::fromText(R"(
module m(input logic clk, rst, go);
  logic [1:0] a, b, c, d;
  always_ff @(posedge clk) if (rst) a <= 0; else unique case (a) 0: if (go) a <= 1; 1: a <= 2; 2: a <= 0; endcase
  always_ff @(posedge clk) if (rst) b <= 0; else priority case (b) 0: if (go) b <= 1; 1: b <= 2; 2: b <= 0; endcase
  always_ff @(posedge clk) if (rst) c <= 0; else (* full_case *) case (c) 0: if (go) c <= 1; 1: c <= 2; 2: c <= 0; endcase
  always_ff @(posedge clk) if (rst) d <= 0; else case (d) 0: if (go) d <= 1; 1: d <= 2; 2: d <= 0; endcase
endmodule
)");
        slang::ast::Compilation comp;
        comp.addSyntaxTree(tree);
        comp.getAllDiagnostics();
        auto fsms = extractFsms(comp.getRoot());
        check(fsms.size() == 4, "caseFull: four FSMs");
        for (const ExtractedFsm& f : fsms) {
            check(f.caseOnState && !f.caseDefault, f.name + ": case without default");
            check(f.caseFull == (f.name != "d"), f.name + ": caseFull");
        }
    }
    run("tests/cov/fsm.sv", print);
    run("tests/cov/fsm_onehot.sv", print);
    if (failed) return 1;
    std::cout << "PASS\n";
    return 0;
}
