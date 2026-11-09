// UPF reader, model and expressions (src/upf.hpp), with no simulator.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>

#include "upf.hpp"

using namespace upf;

static Model load(const std::string& text) {
    std::string path = "test_upf_tmp.upf";
    std::ofstream(path) << text;
    Model m;
    m.load(path, "u_dut");
    std::remove(path.c_str());
    return m;
}

int main() {
    // Tcl: braces, quotes, continuation, comments, variables, ';'
    {
        Reader r;
        r.parse("set d PD_A ; # comment\n"
                "create_power_domain $d \\\n  -elements {u_a u_b}\n"
                "set_isolation iso -domain ${d} -isolation_signal \"$d/iso\" # not a comment here\n",
                "x.upf");
        assert(r.cmds.size() == 2);
        assert(r.cmds[0].w.size() == 4 && r.cmds[0].w[1] == "PD_A" && r.cmds[0].w[3] == "u_a u_b");
        assert(r.cmds[0].line == 2);
        assert(r.cmds[1].w[3] == "PD_A" && r.cmds[1].w[5] == "PD_A/iso");
        auto l = splitList("a {b c} \"d e\" {f {g h}}");
        assert(l.size() == 4 && l[1] == "b c" && l[2] == "d e" && l[3] == "f {g h}");
        bool threw = false;
        try { r.parse("puts $nope\n", "y.upf"); } catch (const Error& e) {
            threw = std::string(e.what()).find("y.upf:1") != std::string::npos;
        }
        assert(threw);
    }
    // Expressions: precedence, unknowns, supply comparisons
    {
        EvalCtx c;
        c.id = [](const std::string& id, EVal& v) {
            if (id == "a") { v.v = 1; return true; }
            if (id == "b") { v.v = 0; return true; }
            if (id == "x") { v.x = 1; return true; }
            if (id == "vdd") { v.k = EVal::Supply; v.sv = {S_FULL_ON, 0.9, true}; return true; }
            return false;
        };
        c.inState = [](const std::string& o, const std::string& s) { return o == "PD" && s == "RUN"; };
        auto t = [&](const char* e) { return truthOf(evalEx(parseExpr(e, "t"), c, "t")); };
        assert(t("a && !b") == 1);
        assert(t("a || x") == 1 && t("b && x") == 0 && t("a && x") == 2 && t("!x") == 2);
        assert(t("a == 1'b1 && b != 1") == 1);
        assert(t("vdd == FULL_ON") == 1 && t("vdd == {FULL_ON 0.9}") == 1 && t("vdd == {FULL_ON 1.0}") == 0);
        assert(t("vdd != OFF") == 1);
        assert(t("!(a & b) | b") == 1 && t("a ^ a") == 0);
    }
    // Model: scopes, handles, switches, strategies, power states, PSTs
    {
        Model m = load(
            "set_scope u_core\n"
            "create_power_domain PD_C -elements {.}\n"
            "set_scope ..\n"
            "create_power_domain PD_T -include_scope\n"
            "create_supply_port VDD\ncreate_supply_net VDD\ncreate_supply_net VC -domain PD_T\n"
            "connect_supply_net VDD -ports {VDD}\n"
            "create_supply_set SS -function {power VDD} -function {ground VSS}\n"
            "associate_supply_set SS -handle PD_T.primary\n"
            "create_supply_set u_core/PD_C.primary -update -function {power VC}\n"
            "create_power_switch SW -output_supply_port {o VC} -input_supply_port {i VDD} -control_port {c u_pmu/en} -on_state {on i {c}}\n"
            "set_isolation I -domain u_core/PD_C -applies_to outputs -clamp_value 1 -isolation_signal iso -isolation_sense low\n"
            "set_retention R -domain u_core/PD_C -save_signal {s high} -restore_signal {s low}\n"
            "add_power_state SS -state {HV -supply_expr {power == {FULL_ON 1.0}} -simstate NORMAL}\n"
            "add_port_state VDD -state {HV 1.0} -state {OFF off}\n"
            "create_pst P -supplies {VDD}\nadd_pst_state on -pst P -state {HV}\n");
        assert(m.domains.count("u_core/PD_C") && m.domains.at("u_core/PD_C").elements[0] == "u_core");
        assert(m.primaryOf("PD_T") == "SS" && m.fnNet("PD_T.primary.power") == "VDD");
        assert(m.fnNet("u_core/PD_C.primary.power") == "VC");
        assert(m.design("u_core/iso") == "u_dut.u_core.iso" && m.design(".") == "u_dut");
        assert(m.switches.at("SW").ctrls[0].second == "u_pmu/en");
        assert(m.isos.size() == 1 && m.isos[0].clamp == "1" && m.isos[0].senseLow[0] == 1 && m.isos[0].appliesTo == 1);
        assert(m.rets[0].save.net == "s" && m.rets[0].restore.sense == 1);
        assert(m.sets.at("SS").states[0].simstate == SS_NORMAL);
        assert(m.psts[0].rows[0].st[0] == "HV");
        DomainMap dm;
        dm.build(m);
        assert(dm.of("u_dut.u_core.x") == "u_core/PD_C" && dm.of("u_dut.u_corex") == "PD_T" && dm.of("tb") == "");
    }
    // Errors carry the file and line
    {
        bool threw = false;
        try { load("create_power_domain PD -elements {a}\nset_isolation I -domain PD -clamp_value 7\n"); }
        catch (const Error& e) { threw = std::string(e.what()).find(":2: -clamp_value 7") != std::string::npos; }
        assert(threw);
    }
    std::cout << "test_upf: ok\n";
    return 0;
}
