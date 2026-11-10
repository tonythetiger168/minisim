// mdft.cpp - design-for-test checks on the elaborated design, on the same
// slang front end as msim.
//
// Scan test needs every flop to be controllable and observable from the
// chains: in shift mode the tester drives one clock and holds every reset, so
// a clock the tester cannot reach, or a reset the tester cannot hold, costs
// coverage on everything behind it. These rules find that before synthesis.
//
// Rules
//   DFT001  a flop is clocked by a signal that is not a primary input and not
//           a clock the tester can reach: a gated or internally generated
//           clock, which needs a test bypass.
//   DFT002  a flop has an asynchronous set or reset driven by logic rather
//           than a primary input, so scan shift cannot hold it inactive.
//   DFT003  a latch (always_latch, or a combinational path that holds its
//           value): not scannable without transparency control.
//   DFT004  flops on the falling edge of a clock whose other flops use the
//           rising edge: the two halves need a lockup latch between chains.
//   DFT005  a combinational loop: no flop in the feedback path, so scan sees
//           an unknown value.
//   DFT006  a flop is clocked by a signal that another flop in the same
//           design drives (a ripple or divided clock).
//
// The summary lists the flop count per clock domain, which is what a chain
// plan is built from.
#include <functional>

#include "dft_mbist.hpp"
#include "dft_scan.hpp"
#include "lintir.hpp"
#include "version.hpp"

using namespace lintir;

namespace {

// A clock or reset the tester drives: a port of a top-level module.
bool primaryInput(const Symbol& s) {
    if (s.kind == SymbolKind::Port || s.kind == SymbolKind::ModportPort) return true;
    const Scope* sc = s.getParentScope();
    if (!sc) return false;
    const Symbol& owner = sc->asSymbol();
    if (owner.kind != SymbolKind::InstanceBody) return false;
    const auto& body = owner.as<InstanceBodySymbol>();
    // A net or variable that a port of this module names.
    for (const Symbol* p : body.getPortList()) {
        if (!p) continue;
        if (p == &s) return true;
        if (p->kind == SymbolKind::Port) {
            const auto& ps = p->as<PortSymbol>();
            if (ps.internalSymbol == &s) return true;
        }
    }
    return false;
}

// Is this instance body one of the design's top modules?
bool inTop(const Symbol& s) {
    const Scope* sc = s.getParentScope();
    while (sc) {
        const Symbol& owner = sc->asSymbol();
        if (owner.kind == SymbolKind::InstanceBody) {
            const auto& body = owner.as<InstanceBodySymbol>();
            const Symbol* parent = body.parentInstance;
            return parent && parent->as<InstanceSymbol>().isTopLevel();
        }
        sc = owner.getParentScope();
    }
    return false;
}

struct Dft {
    Design& d;
    Report& rep;
    // The flops of each domain, and whether each edge is used.
    struct DomainInfo {
        int flops = 0;
        std::vector<const Block*> pos, neg;
    };
    std::map<const Symbol*, DomainInfo> domains;

    Dft(Design& des, Report& r) : d(des), rep(r) {}

    const Symbol* dom(const Symbol* clk) { return clk ? d.join.find(clk) : nullptr; }

    // Every signal a flop drives, so a clock built from one can be found.
    std::unordered_set<const Symbol*> flopDriven() {
        std::unordered_set<const Symbol*> s;
        for (const Block& b : d.blocks) {
            if (!b.seq) continue;
            for (const Symbol* t : b.nba) s.insert(d.join.find(t));
            for (const Symbol* t : b.blocking)
                if (!b.locals.count(t)) s.insert(d.join.find(t));
        }
        return s;
    }

    // Every signal combinational logic drives, with what it is made from, for
    // the clock-gating and loop rules.
    // Logic, as opposed to a testbench: always_comb / always_latch, or an
    // always with only an event control (always @(*), always @(a or b)).
    // An initial block or `always #5 clk = ~clk` drives a pin the way a tester
    // does, so a clock or reset it makes is not "made by logic".
    static bool combLogic(const Block& b) {
        auto k = b.sym->procedureKind;
        if (k == ProceduralBlockKind::AlwaysComb || k == ProceduralBlockKind::AlwaysLatch) return true;
        if (k != ProceduralBlockKind::Always) return false;
        const Statement& body = b.sym->getBody();
        if (body.kind != StatementKind::Timed) return false;
        auto tk = body.as<TimedStatement>().timing.kind;
        return tk == TimingControlKind::SignalEvent || tk == TimingControlKind::EventList ||
               tk == TimingControlKind::ImplicitEvent;
    }

    std::map<const Symbol*, std::vector<const Symbol*>> combFrom() {
        std::map<const Symbol*, std::vector<const Symbol*>> m;
        for (const Block& b : d.blocks) {
            if (b.seq || !combLogic(b)) continue;
            // A value the block assigns on every path before using it (y = a + b;
            // z = y == 0;, or sum = 0; sum = sum ^ x;) is not feedback: within one
            // evaluation the read sees this pass's write. Its own inputs are
            // already among the block's reads.
            // A block's own target read back (bit by bit in a loop, or one part
            // select after another) is not counted either: a self-edge was most
            // of the false loops on OpenTitan.
            for (const Symbol* t : b.blocking) {
                if (b.locals.count(t)) continue;
                const Symbol* ct = d.join.find(t);
                auto& v = m[ct];
                for (const Symbol* r : b.reads)
                    if (!b.always.count(r) && d.join.find(r) != ct) v.push_back(d.join.find(r));
            }
        }
        return m;
    }

    void run() {
        auto flopped = flopDriven();
        auto comb = combFrom();

        for (const Block& b : d.blocks) {
            const ProceduralBlockSymbol& p = *b.sym;

            // DFT003: a latch.
            if (p.procedureKind == ProceduralBlockKind::AlwaysLatch) {
                rep.add("DFT003", p.location,
                        "an always_latch is not scannable; a latch needs a transparency control "
                        "(a test-mode enable) or it blocks the chain behind it");
                continue;
            }
            if (!b.seq) continue;

            const Symbol* c = dom(b.clk);
            if (!c) {
                rep.add("DFT001", p.location,
                        "the clock of this block is an expression, not a signal, so the tester "
                        "cannot reach it");
                continue;
            }
            auto& info = domains[c];
            info.flops++;

            // Which edge, for DFT004.
            const Statement& body = p.getBody();
            if (body.kind == StatementKind::Timed) {
                std::vector<const SignalEventControl*> evs;
                clocksOf(body.as<TimedStatement>().timing, evs);
                for (const SignalEventControl* e : evs) {
                    const Symbol* s = oneName(e->expr);
                    if (!s || d.join.find(s) != c) continue;
                    if (e->edge == EdgeKind::NegEdge) info.neg.push_back(&b);
                    else if (e->edge == EdgeKind::PosEdge) info.pos.push_back(&b);
                }
            }

            // DFT006 / DFT001: where does the clock come from?
            if (flopped.count(c)) {
                rep.add("DFT006", p.location,
                        "'" + path(*b.clk) +
                            "' is driven by a flop (a ripple or divided clock); scan needs it "
                            "muxed to the test clock in test mode");
            } else if (comb.count(c)) {
                rep.add("DFT001", p.location,
                        "'" + path(*b.clk) +
                            "' is made by combinational logic (a gated clock); add a test bypass "
                            "so the tester drives this flop in shift mode");
            } else if (!primaryInput(*b.clk) && !inTop(*b.clk)) {
                // A clock that neither a port nor any process drives: a tie or
                // an unconnected signal.
                rep.add("DFT001", p.location,
                        "'" + path(*b.clk) + "' has no driver the tester can reach");
            }

            // DFT002: an asynchronous set or reset built from logic.
            if (b.rst) {
                const Symbol* r = d.join.find(b.rst);
                if (flopped.count(r) || comb.count(r))
                    rep.add("DFT002", p.location,
                            "the asynchronous reset '" + path(*b.rst) +
                                "' is driven by logic, so scan shift cannot hold it inactive; "
                                "drive it from a primary input in test mode");
            }
        }

        // DFT004: both edges of one clock.
        for (auto& [c, info] : domains) {
            if (info.pos.empty() || info.neg.empty()) continue;
            rep.add("DFT004", info.neg.front()->sym->location,
                    "this block uses the falling edge of '" + path(*c) +
                        "' while " + std::to_string(info.pos.size()) +
                        " other block(s) use the rising edge; the two halves need separate chains "
                        "with a lockup latch between them");
        }

        // DFT005: a combinational loop.
        findLoops(comb);
    }

    // A cycle through combinational logic only: depth-first search over
    // "this signal is made from these signals".
    void findLoops(const std::map<const Symbol*, std::vector<const Symbol*>>& comb) {
        std::unordered_set<const Symbol*> done, stack;
        std::vector<const Symbol*> pathv;
        std::function<bool(const Symbol*)> walk = [&](const Symbol* s) -> bool {
            if (stack.count(s)) {
                std::string cyc;
                bool seen = false;
                for (const Symbol* x : pathv) {
                    if (!seen && x != s) continue;
                    seen = true;
                    cyc += (cyc.empty() ? "" : " -> ") + path(*x);
                }
                cyc += " -> " + path(*s);
                const Symbol* where = s;
                rep.add("DFT005", where->location,
                        "combinational loop " + cyc +
                            ": no flop in the feedback path, so its value is unknown in scan and "
                            "static timing cannot cut it");
                return true;
            }
            if (done.count(s)) return false;
            stack.insert(s);
            pathv.push_back(s);
            auto it = comb.find(s);
            if (it != comb.end())
                for (const Symbol* in : it->second)
                    if (walk(in)) break;
            pathv.pop_back();
            stack.erase(s);
            done.insert(s);
            return false;
        };
        for (auto& [s, _] : comb) walk(s);
    }

    // The chain plan: the flops per domain, which is what scan insertion needs.
    void summary(std::ostream& os) {
        int total = 0;
        for (auto& [c, info] : domains) total += info.flops;
        os << "mdft: " << total << (total == 1 ? " flop in " : " flops in ") << domains.size()
           << (domains.size() == 1 ? " clock domain" : " clock domains") << "\n";
        for (auto& [c, info] : domains) {
            os << "  " << path(*c) << ": " << info.flops << (info.flops == 1 ? " flop" : " flops");
            if (!info.pos.empty() && !info.neg.empty())
                os << " (" << info.pos.size() << " rising, " << info.neg.size() << " falling)";
            os << "\n";
        }
    }
};

void usage() {
    std::cout << "usage: mdft [options] file.sv ...\n"
                 "  design-for-test checks and a scan chain plan\n"
                 "       mdft -insert_scan netlist.v -o scanned.v ...   (see mdft -insert_scan -help)\n"
                 "       mdft -mbist -name N -words W -width B ...      (see mdft -mbist -help)\n\n"
                 "  -disable RULE  turn one rule off (repeatable)\n"
                 "  -list          list the rules\n"
                 "  -Werror        exit non-zero on any finding\n"
                 "  -quiet         print findings only, no summary\n"
                 "  -top M         the top module    -f <file>  an argument file\n"
              << protfront::Options::help() <<
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version       print the version\n"
                 "  --help-slang   list every front-end option\n";
}

void listRules() {
    std::cout << "DFT001  a gated or unreachable clock: the tester cannot drive the flop\n"
                 "DFT002  an asynchronous reset built from logic: scan shift cannot hold it\n"
                 "DFT003  a latch: not scannable without a transparency control\n"
                 "DFT004  both edges of one clock: the chains need a lockup latch\n"
                 "DFT005  a combinational loop\n"
                 "DFT006  a ripple or divided clock (a flop drives the clock)\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Scan insertion and memory BIST work on a gate netlist or on numbers,
    // not on the elaborated design: they have their own option sets.
    if (argc > 1 && (std::string(argv[1]) == "-insert_scan" || std::string(argv[1]) == "-mbist")) {
        std::vector<std::string> rest(argv + 2, argv + argc);
        try {
            return std::string(argv[1]) == "-mbist" ? mbist::main(rest) : scan::main(rest);
        } catch (const std::exception& e) {
            std::cerr << "mdft: " << e.what() << "\n";
            return 2;
        }
    }
    FrontEnd fe;
    std::vector<std::string> fwd{argv[0]};
    Rules rules;
    bool werror = false, quiet = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mdft: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-list" || a == "--list") { listRules(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mdft " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-disable") { rules.disabled.insert(next("a rule name")); continue; }
        if (a == "-Werror") { werror = true; continue; }
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (fe.prot.take(argc, argv, i)) continue;
        fwd.push_back(a);
    }

    fe.tool = "mdft";
    if (int rc = fe.run(fwd, "mdft front end")) return rc < 0 ? 0 : rc;

    Report rep;
    rep.sm = fe.comp->getSourceManager();
    if (fe.pc.any()) { rep.pc = &fe.pc; namePC() = &fe.pc; }
    rep.rules = rules;
    Design d;
    d.gather(*fe.comp, rep);

    Dft dft(d, rep);
    dft.run();
    rep.print(std::cout, /*quiet*/ true, "mdft");
    if (!quiet) {
        std::map<std::string, int> per;
        for (const Finding& f : rep.out) per[f.rule]++;
        std::cout << "mdft: " << rep.out.size() << (rep.out.size() == 1 ? " finding" : " findings");
        bool first = true;
        for (auto& [r, n] : per) {
            std::cout << (first ? " (" : ", ") << r << " " << n;
            first = false;
        }
        std::cout << (first ? "" : ")") << "\n";
        dft.summary(std::cout);
    }
    return werror && !rep.out.empty() ? 1 : 0;
}
