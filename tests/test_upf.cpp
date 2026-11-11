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
    // Tcl (5.1): command substitution, foreach, proc, if, expr, lists; a bus
    // index stays text; a UPF command's result is the value of [command]
    {
        Reader r;
        r.onCmd = [&](const Cmd& c) {
            r.cmds.push_back(c);
            return c.w[0] == "find_objects" ? std::string("u_a u_b") : std::string();
        };
        r.parse("proc mkdom {name args} {\n"
                "  create_power_domain PD_$name -elements $args\n"
                "}\n"
                "foreach i [find_objects . -pattern u_*] { mkdom [string toupper $i] $i }\n"
                "set n [expr {[llength [find_objects . -pattern *]] * 2}]\n"
                "if {$n == 4} { set_isolation iso$n -domain PD_U_A -elements {d[3] q[7:0]} } else { bad }\n"
                "for {set k 0} {$k < 2} {incr k} { create_supply_net VDD$k }\n"
                "set_level_shifter ls -domain PD_U_B -elements [list gen[0]/u x]\n"
                "catch { nope_cmd } msg\n",
                "t.upf");
        std::vector<std::string> got;
        for (auto& c : r.cmds) if (c.w[0] != "find_objects") got.push_back(tcl::joinList(c.w));
        assert(got.size() == 7);
        assert(got[0] == "create_power_domain PD_U_A -elements u_a" && r.cmds[1].line == 2);
        assert(got[1] == "create_power_domain PD_U_B -elements u_b");
        assert(got[2] == "set_isolation iso4 -domain PD_U_A -elements {d[3] q[7:0]}");
        assert(got[3] == "create_supply_net VDD0" && got[4] == "create_supply_net VDD1");
        auto els = splitList(r.cmds[7].w[5]);
        assert(r.cmds[7].w[0] == "set_level_shifter" && els.size() == 2 && els[0] == "gen[0]/u" && els[1] == "x");
        assert(got[6] == "nope_cmd");
        // the first error of a script is thrown; a UPF command's error keeps its own place
        r.onCmd = [](const Cmd& c) -> std::string { throw Error(c.where() + ": unknown UPF command " + c.w[0]); };
        std::string msg;
        try { r.parse("set a 1\nforeach x {1 2} {\n  zap $x\n}\n", "e.upf"); } catch (const Error& e) { msg = e.what(); }
        assert(msg == "e.upf:3: unknown UPF command zap");
        msg.clear();
        try { r.parse("set a [expr {1 +}]\n", "e.upf"); } catch (const Error& e) { msg = e.what(); }
        assert(msg.rfind("e.upf:1: ", 0) == 0);
        // catch sees a UPF error and the read goes on
        r.parse("if {[catch {zap 1} m]} { set seen $m }\n", "e.upf");
        assert(r.get("seen") && r.get("seen")->find("unknown UPF command zap") != std::string::npos);
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
