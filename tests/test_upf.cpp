// UPF reader, model and expressions (src/upf.hpp), with no simulator.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

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
    // Value conversion tables (6.18, 6.28, Annex B)
    {
        auto& pre = predefinedVcts();
        assert(pre.size() == 12);
        auto h2u = [&](const char* t, char v) { return pre.at(t).toUpf(v); };
        auto u2h = [&](const char* t, int st) { return pre.at(t).toHdl(st); };
        assert(h2u("SV_LOGIC2UPF", '0') == S_OFF && h2u("SV_LOGIC2UPF", '1') == S_FULL_ON);
        assert(h2u("SV_LOGIC2UPF", 'x') == S_UNDETERMINED && h2u("SV_LOGIC2UPF", 'Z') == S_UNDETERMINED);
        assert(h2u("SV_LOGIC2UPF_GNDZERO", '0') == S_FULL_ON && h2u("SV_LOGIC2UPF_GNDZERO", '1') == S_OFF);
        assert(u2h("UPF2SV_LOGIC", S_OFF) == '0' && u2h("UPF2SV_LOGIC", S_FULL_ON) == '1');
        assert(u2h("UPF2SV_LOGIC", S_PARTIAL_ON) == 'X' && u2h("UPF2SV_LOGIC", S_UNDETERMINED) == 'X');
        assert(u2h("UPF_GNDZERO2SV_LOGIC", S_OFF) == '1' && u2h("UPF_GNDZERO2SV_LOGIC", S_FULL_ON) == '0');
        assert(h2u("VHDL_SL2UPF", 'H') == S_FULL_ON && h2u("VHDL_SL2UPF", 'L') == S_OFF && h2u("VHDL_SL2UPF", 'U') == S_UNDETERMINED);
        assert(h2u("VHDL_SL2UPF", 'W') == S_UNDETERMINED && h2u("VHDL_SL2UPF", '-') == S_UNDETERMINED);
        assert(h2u("VHDL_SL2UPF_GNDZERO", 'L') == S_FULL_ON && h2u("VHDL_SL2UPF_GNDZERO", '1') == S_OFF);
        assert(u2h("UPF2VHDL_SL", S_FULL_ON) == '1' && u2h("UPF_GNDZERO2VHDL_SL", S_FULL_ON) == '0');
        // the TIED tables are upf2hdl (1801-2024 Annex B): the tie value only while FULL_ON
        assert(!pre.at("SV_TIED_HI").hdl2upf && !pre.at("VHDL_TIED_LO").hdl2upf);
        assert(u2h("SV_TIED_HI", S_FULL_ON) == '1' && u2h("SV_TIED_LO", S_FULL_ON) == '0');
        for (int st : {S_OFF, S_PARTIAL_ON, S_UNDETERMINED}) assert(u2h("SV_TIED_HI", st) == 'X' && u2h("SV_TIED_LO", st) == 'X');
        assert(u2h("VHDL_TIED_HI", S_FULL_ON) == '1' && u2h("VHDL_TIED_HI", S_OFF) == 'X');
        assert(u2h("VHDL_TIED_LO", S_FULL_ON) == '0' && u2h("VHDL_TIED_LO", S_PARTIAL_ON) == '0' && u2h("VHDL_TIED_LO", S_OFF) == 'X');
        Model m = load("create_supply_net VR\n"
                       "create_hdl2upf_vct H -hdl_type {vhdl std_logic} -table {{'1' FULL_ON} {'H' PARTIAL_ON} {'0' off}}\n"
                       "create_upf2hdl_vct U -hdl_type {sv} -table {{FULL_ON 1} {OFF z}}\n"
                       "connect_supply_net VR -ports {u_a/p u_b/q} -vct U\n"
                       "connect_supply_net VR -ports {u_c/r}\n");
        const Vct* h = m.vct("H");
        assert(h && h->hdl2upf && h->lang == "vhdl" && h->hdlType == "std_logic");
        assert(h->toUpf('1') == S_FULL_ON && h->toUpf('h') == S_PARTIAL_ON && h->toUpf('0') == S_OFF && h->toUpf('Z') == S_UNDETERMINED);
        const Vct* u = m.vct("U");
        assert(u && !u->hdl2upf && u->toHdl(S_FULL_ON) == '1' && u->toHdl(S_OFF) == 'Z' && u->toHdl(S_PARTIAL_ON) == 'X');
        assert(m.vct("SV_TIED_HI") && !m.vct("NOPE"));
        assert(m.connectVct.at({"VR", "u_a/p"}) == "U" && m.connectVct.at({"VR", "u_b/q"}) == "U" && !m.connectVct.count({"VR", "u_c/r"}));
        for (const char* bad : {"create_hdl2upf_vct H2 -table {{1 OFF}}\n",                       // no -hdl_type
                                "create_hdl2upf_vct H2 -hdl_type {sc} -table {{1 OFF}}\n",       // not vhdl or sv
                                "create_hdl2upf_vct H2 -hdl_type {sv} -table {{1 ON}}\n",        // not a supply state
                                "create_hdl2upf_vct H2 -hdl_type {sv} -table {{U OFF}}\n",       // U is not SystemVerilog
                                "create_hdl2upf_vct H2 -hdl_type {sv} -table {{1 OFF} {1 FULL_ON}}\n",
                                "create_upf2hdl_vct U2 -hdl_type {sv} -table {{OFF 0 1}}\n",
                                "create_upf2hdl_vct SV_TIED_HI -hdl_type {sv} -table {{OFF 0}}\n",
                                "connect_supply_net VR -ports {p} -vct NOPE\n"}) {
            bool threw = false;
            try { load(std::string("create_supply_net VR\n") + bad); } catch (const Error& e) {
                threw = std::string(e.what()).find("test_upf_tmp.upf:2") != std::string::npos;
            }
            assert(threw);
        }
    }
    // find_objects (6.33) against a design view
    {
        DesignView dv;
        dv.instances = {"u_dut", "u_dut.u_a", "u_dut.u_a.u_leaf", "u_dut.g[0]", "u_dut.g[0].u_b", "u_dut.u_c"};
        dv.model = {{"", "tb"}, {"u_dut", "dut"}, {"u_dut.u_a", "blk"}, {"u_dut.u_a.u_leaf", "cell"},
                    {"u_dut.g[0].u_b", "blk"}, {"u_dut.u_c", "cell"}};
        dv.portsOf["u_dut.u_a"] = {{"clk", 0}, {"q", 1}, {"io", 2}};
        dv.signals = {"u_dut.n1", "u_dut.u_a.n1", "u_dut.u_a.q"};
        std::string path = "test_upf_find.upf";
        auto run = [&](const std::string& text) {
            std::ofstream(path) << "set_scope u_a\nset_scope ..\nset r [" << text << "]\nputs $r\n";
            Model m;
            m.dv = &dv;
            std::ostringstream out;
            m.out = &out;
            m.load(path, "u_dut");
            std::string r = out.str();
            if (!r.empty()) r.pop_back();
            return r;
        };
        assert(run("find_objects . -pattern u_*") == "u_a u_c");
        assert(run("find_objects . -pattern u_* -transitive") == "u_a u_c {g[0]/u_b} u_a/u_leaf");
        assert(run("find_objects . -pattern blk -object_type model -transitive") == "u_a {g[0]/u_b}");
        assert(run("find_objects . -pattern * -leaf_only") == "u_c");
        assert(run("find_objects . -pattern * -non_leaf") == "u_a");
        assert(run("find_objects u_a -pattern * -object_type port -direction out") == "u_a/q");
        assert(run("find_objects u_a -pattern * -object_type port") == "u_a/clk u_a/q u_a/io");
        assert(run("find_objects . -pattern n1 -object_type net -transitive") == "n1 u_a/n1");
        assert(run("find_objects . -pattern {^u_[ac]$} -regexp") == "u_a u_c");
        assert(run("find_objects . -pattern U_A -exact -ignore_case") == "u_a");
        assert(run("find_objects . -pattern U_A -exact") == "");
        for (const char* bad : {"find_objects . -pattern * -object_type wire", "find_objects nope -pattern *",
                                "find_objects . -pattern * -direction in", "find_objects .",
                                "find_objects . -pattern * -bogus", "find_objects . -pattern * -regexp -exact",
                                "find_objects . -pattern * -object_type process"}) {
            bool threw = false;
            try { run(bad); } catch (const Error& e) { threw = std::string(e.what()).find(path + ":3: find_objects") != std::string::npos; }
            assert(threw);
        }
        // without a design, find_objects is an error; supply objects need none
        std::ofstream(path) << "create_supply_net VDD\ncreate_supply_port P -direction out\n"
                               "puts [find_objects . -pattern * -object_type supply_net]\n"
                               "puts [find_objects . -pattern * -object_type supply_port -direction out]\n";
        Model m;
        std::ostringstream out;
        m.out = &out;
        m.load(path, "");
        assert(out.str() == "VDD\nP\n");
        std::remove(path.c_str());
    }
    // The information model (clause 10) and its queries (11.1)
    {
        DesignView dv;
        dv.instances = {"u_dut", "u_dut.u_a", "u_dut.u_a.u_leaf", "u_dut.u_c"};
        dv.model = {{"", "tb"}, {"u_dut", "dut"}, {"u_dut.u_a", "blk"}, {"u_dut.u_a.u_leaf", "cell"}, {"u_dut.u_c", "cell"}};
        dv.portsOf["u_dut.u_a"] = {{"clk", 0}, {"q", 1}};
        dv.signals = {"u_dut.iso_en", "u_dut.sw_en", "u_dut.u_a.q", "u_dut.u_a.clk", "u_dut.save"};
        dv.width["u_dut.u_a.q"] = 8;
        std::string path = "test_upf_query.upf";
        const std::string intent =
            "create_supply_port VDD\ncreate_supply_net VDD\ncreate_supply_net VDD_A\n"
            "connect_supply_net VDD -ports VDD\n"
            "create_supply_set SS -function {power VDD} -function {ground VSS}\n"
            "create_power_domain PD_TOP -include_scope -supply {primary SS}\n"
            "create_power_domain PD_A -elements {u_a}\n"
            "create_power_switch sw -domain PD_TOP -input_supply_port {vin VDD} -output_supply_port {vout VDD_A} "
            "-control_port {en sw_en} -on_state {on vin {en}} -off_state {off {!en}}\n"
            "set_isolation iso -domain PD_A -isolation_signal iso_en -clamp_value 1 -applies_to outputs -isolation_supply SS\n"
            "set_retention ret -domain PD_A -save_signal {save posedge} -restore_signal {save negedge}\n"
            "add_port_state VDD -state {on 0.9} -state {off off}\n"
            "add_power_state PD_A -state {RUN -logic_expr {iso_en == 0}}\n";
        auto run = [&](const std::string& text) {
            std::ofstream(path) << intent << "set r [" << text << "]\nputs $r\n";
            Model m;
            m.dv = &dv;
            std::ostringstream out;
            m.out = &out;
            m.load(path, "u_dut");
            std::string r = out.str();
            if (!r.empty()) r.pop_back();
            return r;
        };
        assert(run("upf_query_object_type PD_A") == "upfPowerDomainT");
        assert(run("upf_query_object_type /u_dut/PD_A.iso") == "upfIsolationStrategyT");
        assert(run("upf_query_object_type VDD") == "upfSupplyPortT");
        assert(run("upf_query_object_type VDD@upfSupplyNetT") == "upfSupplyNetT");
        assert(run("upf_query_object_type sw/en") == "upfLogicPortT");
        assert(run("upf_query_object_type sw.on") == "upfPowerSwitchStateT");
        assert(run("upf_query_object_type PD_A.RUN") == "upfPowerStateT");
        assert(run("upf_query_object_type u_a/q") == "upfHdlPortMultiBitT");
        assert(run("upf_query_object_type .") == "upfHdlScopeT");
        assert(run("upf_query_object_type #UPFEXTENT1#") == "upfExtentT");
        assert(run("upf_object_in_class PD_A.iso -class upfBoundaryStrategyT") == "1");
        assert(run("upf_object_in_class PD_A -class upfStrategyT") == "0");
        assert(run("upf_query_object_properties PD_A -property upf_isolation_strategies") == "/u_dut/PD_A.iso");
        assert(run("upf_query_object_properties PD_A -property upf_pd_states") == "/u_dut/PD_A.RUN");
        assert(run("upf_query_object_properties PD_A -property upf_repeater_strategies") == "");
        assert(run("upf_query_object_properties PD_A.iso -property upf_clamp_values") == "1");
        assert(run("upf_query_object_properties PD_A.iso -property upf_applies_to") == "UPF_FILTER_OUTPUTS");
        assert(run("upf_query_object_properties PD_A.iso.isolation_signal -property upf_ref_object") == "/u_dut/iso_en");
        assert(run("upf_query_object_properties PD_A.ret -property upf_retention_parameters") == "RET_SUP_COR SAV_RES_COR");
        assert(run("upf_query_object_properties VDD -property upf_loconn") == "/u_dut/VDD@upfSupplyNetT");
        assert(run("upf_query_object_properties VDD_A -property upf_root_driver") == "/u_dut/sw/vout");
        assert(run("upf_query_object_properties VDD.on -property upf_volt_nom") == "0.9");
        assert(run("upf_query_object_properties sw/en -property upf_hiconn") == "/u_dut/sw_en");
        assert(run("upf_query_object_properties u_a/q -property upf_hdl_width") == "8");
        assert(run("upf_query_object_properties #UPFSIGSENSE1#") ==
               "{upf_signal_sensitivity UPF_SENSE_HIGH} {upf_control_signal /u_dut/iso_en}");
        assert(run("upf_query_object_properties [upf_query_object_properties PD_A.ret -property upf_save_signal]"
                   " -property upf_signal_sensitivity") == "UPF_SENSE_POSEDGE");
        assert(run("upf_query_object_properties [upf_query_object_properties sw.on -property upf_switch_expr]"
                   " -property upf_expr_string") == "en");
        assert(run("upf_query_object_pathname PD_A.iso -relative_to /u_dut") == "PD_A.iso");
        assert(run("upf_query_object_pathname u_a/q -relative_to u_a") == "q");
        assert(run("upf_query_object_pathname u_a -relative_to u_c") == "");
        assert(run("upf_query_object_pathname #UPFEXTENT1#") == "");
        assert(run("query_upf . -pattern * -object_type inst -transitive") == "u_a u_c u_a/u_leaf");
        assert(run("query_upf PD_A -pattern *") == "u_a u_a/u_leaf u_a/clk u_a/q PD_A.primary");
        assert(run("query_upf . -pattern VDD* -object_type supply_net") == "VDD VDD_A");
        assert(run("query_upf . -pattern S* -object_type supply_set") == "SS");
        for (const char* bad : {"upf_query_object_type NOPE", "upf_query_object_properties PD_A -property upf_port_dir",
                                "upf_query_object_properties PD_A -property upf_bogus",
                                "upf_query_object_properties PD_A -property upf_current_state",
                                "upf_object_in_class PD_A -class upfBogusT", "upf_query_object_type #bad",
                                "query_upf ../x -pattern *", "query_upf . -pattern * -object_type wire",
                                "query_upf . -pattern * -inst_type nand"}) {
            bool threw = false;
            try { run(bad); } catch (const Error& e) { threw = std::string(e.what()).find(path + ":13: ") != std::string::npos; }
            assert(threw);
        }
        // phase 5 (11.1.1): after a query only set_scope, bind_checker, find_objects and load_upf
        bool threw = false;
        try { run("upf_query_object_type PD_A]\nset_scope u_a\nset_scope ..\nset r [find_objects . -pattern u_a]\ncreate_supply_net LATE\nputs [list"); }
        catch (const Error& e) { threw = std::string(e.what()).find(":17: create_supply_net after a UPF query") != std::string::npos; }
        assert(threw);
        std::remove(path.c_str());
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
