// SDC reader (src/sdc.hpp), with no simulator and no front end.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iostream>

#include "sdc.hpp"

using namespace sdc;

struct Toy : Design {
    std::vector<std::string> ports{"clk", "clk2", "rst_n", "d[0]", "d[1]", "d[2]", "q", "sel"};
    std::vector<std::string> pins{"u_a/clk", "u_a/q_reg/CK", "u_a/q_reg/D", "u_a/q_reg/Q", "u_b/r_reg[0]/CK", "u_div/q_reg/Q"};
    std::vector<std::string> cells{"u_a", "u_b", "u_div", "u_a/q_reg", "u_b/r_reg[0]", "u_div/q_reg"};
    std::vector<std::string> nets{"n1", "u_a/n2"};
    const std::vector<std::string>& names(Kind k) const override {
        return k == Kind::Port ? ports : k == Kind::Pin ? pins : k == Kind::Cell ? cells : nets;
    }
    int portDir(const std::string& p) const override { return p == "q" ? 1 : 0; }
    ObjList registers(const RegQuery& q) const override {
        ObjList o;
        if (q.cells) for (auto c : {"u_a/q_reg", "u_b/r_reg[0]", "u_div/q_reg"}) o.push_back({Kind::Cell, c, true});
        if (q.clockPins) o.push_back({Kind::Pin, "u_a/q_reg/CK", true});
        return o;
    }
    ObjList related(Kind want, const Obj& of) const override {
        ObjList o;
        if (want == Kind::Pin && of.kind == Kind::Cell)
            for (auto& p : pins)
                if (p.rfind(of.name + "/", 0) == 0 && p.find('/', of.name.size() + 1) == std::string::npos) o.push_back({Kind::Pin, p, true});
        return o;
    }
    std::optional<std::string> attribute(const Obj& o, const std::string& a) const override {
        if (a == "is_sequential") return o.name.find("_reg") != std::string::npos ? "true" : "false";
        return std::nullopt;
    }
};

static int count(const Reader& r, const char* code) {
    int n = 0;
    for (auto& d : r.diags) n += d.code == code;
    return n;
}

int main() {
    Toy toy;
    // Tcl: variables, expr, continuation, comments, lists, control flow, procs
    {
        Reader r;
        r.design = &toy;
        std::vector<std::string> out;
        r.onPuts = [&](const std::string& s) { out.push_back(s); };
        r.parse("set p 10.0 ; # a comment\n"
                "set half [expr {$p / 2}]\n"
                "puts \"$half [expr 7/2] [expr {7 % 3}] [expr {2**10}] [expr {1 ? \"a\" : \"b\"}]\"\n"
                "proc twice {x {k 2}} { return [expr {$x * $k}] }\n"
                "puts [twice 4] ; puts [twice 4 3]\n"
                "set acc {}\n"
                "foreach {a b} {1 2 3 4} { lappend acc [expr {$a + $b}] }\n"
                "for {set i 0} {$i < 3} {incr i} { if {$i == 1} continue ; lappend acc $i }\n"
                "puts $acc\n"
                "puts [llength {a {b c} d}] ; puts [lindex {a {b c} d} 1] ; puts [join {a b c} ,]\n"
                "puts [format \"%.2f %d %s\" 1.005 42 x] ; puts [string toupper ab] ; puts [string match c* clk]\n"
                "if {$p > 20} { puts big } elseif {$p > 5} { puts mid } else { puts small }\n"
                "set arr(k) 5 ; puts $arr(k) ; puts ${p}ns ; puts [info exists nope]\n"
                "puts [catch {nonsense_command}]\n",
                "t.tcl");
        std::vector<std::string> want{"5.0 3 1 1024 a", "8", "12", "3 7 0 2", "3", "b c", "a,b,c", "1.00 42 x", "AB", "1",
                                      "mid", "5", "10.0ns", "0", "1"};
        if (out != want) {
            for (auto& s : out) std::cerr << "[" << s << "]\n";
            for (auto& d : r.diags) std::cerr << d.loc.str() << " " << d.text << "\n";
        }
        assert(out == want);
        assert(r.diags.empty());
    }
    // Clocks, generated clocks, I/O delays
    {
        Reader r;
        r.design = &toy;
        r.parse("set_units -time ns\n"
                "create_clock -name clk -period 10 [get_ports clk]\n"
                "create_clock -period 8 -waveform {1 5} [get_ports clk2]\n"
                "create_clock -name vclk -period 20\n"
                "create_generated_clock -name div2 -source [get_ports clk] \\\n"
                "    -divide_by 2 [get_pins u_div/q_reg/Q]\n"
                "set_input_delay -clock clk -max 2.5 [get_ports d*]\n"
                "set_input_delay -clock clk -min -0.5 [get_ports {d[0] d[1]}]\n"
                "set_output_delay -clock [get_clocks vclk] 3 [all_outputs] -clock_fall -add_delay\n",
                "c.sdc");
        for (auto& d : r.diags) std::cerr << d.loc.str() << " " << d.text << "\n";
        assert(r.diags.empty());
        auto& s = r.sdc;
        assert(s.units["time"] == "ns");
        assert(s.clocks.size() == 4);
        assert(s.clocks[0].name == "clk" && s.clocks[0].period == 10 && s.clocks[0].waveform[1] == 5);
        assert(s.clocks[0].sources.size() == 1 && s.clocks[0].sources[0].kind == Kind::Port);
        assert(s.clocks[1].name == "clk2" && s.clocks[1].hasWaveform && s.clocks[1].waveform[0] == 1);
        assert(s.clocks[2].isVirtual());
        assert(s.clocks[3].generated && s.clocks[3].divideBy == 2 && s.clocks[3].loc.line == 5);
        assert(s.clocks[3].masterPins[0].name == "clk" && s.clocks[3].sources[0].kind == Kind::Pin);
        assert(s.ioDelays.size() == 3);
        assert(s.ioDelays[0].ports.size() == 3 && s.ioDelays[0].max && s.ioDelays[0].clock == "clk" && s.ioDelays[0].loc.line == 7);
        assert(s.ioDelays[1].value == -0.5 && s.ioDelays[1].min && s.ioDelays[1].ports.size() == 2);
        assert(!s.ioDelays[2].input && s.ioDelays[2].clock == "vclk" && s.ioDelays[2].clockFall && s.ioDelays[2].add);
        assert(s.ioDelays[2].ports.size() == 1 && s.ioDelays[2].ports[0].name == "q");
        assert(s.clock("div2") && !s.clock("nope"));
    }
    // Exceptions, groups, uncertainty, the generic command list
    {
        Reader r;
        r.design = &toy;
        r.parse("create_clock -name a -period 10 [get_ports clk]\n"
                "create_clock -name b -period 7 [get_ports clk2]\n"
                "set_clock_groups -asynchronous -group {a} -group [get_clocks b] -name g1\n"
                "set_false_path -from [get_clocks a] -through [get_pins u_a/q_reg/Q] -through [get_nets n1] -to [get_ports q]\n"
                "set_multicycle_path 2 -setup -from [get_cells u_a/q_reg] -to [get_cells {u_b/r_reg[0]}]\n"
                "set_multicycle_path 1 -hold -end -from [get_cells u_a/q_reg] -to [get_cells {u_b/r_reg[0]}]\n"
                "set_max_delay 3.5 -from rst_n -to [all_registers] -ignore_clock_latency\n"
                "set_clock_uncertainty -setup 0.2 [get_clocks a]\n"
                "set_clock_uncertainty 0.5 -from a -to b\n"
                "set_clock_latency -source -max 1.2 [get_clocks a]\n"
                "set_case_analysis 0 [get_ports sel]\n"
                "set_propagated_clock [all_clocks]\n"
                "set_load -pin_load 0.05 [all_outputs]\n"
                "set_driving_cell -lib_cell BUFX2 -pin Y [all_inputs]\n"
                "set_max_transition 0.4 [current_design]\n",
                "e.sdc");
        for (auto& d : r.diags) std::cerr << d.loc.str() << " " << d.text << "\n";
        assert(r.diags.empty());
        auto& s = r.sdc;
        assert(s.clockGroups.size() == 1 && s.clockGroups[0].type == ClockGroups::Asynchronous);
        assert(s.clockGroups[0].groups.size() == 2 && s.clockGroups[0].groups[1][0] == "b" && s.clockGroups[0].name == "g1");
        assert(s.exceptions.size() == 4);
        auto& fp = s.exceptions[0];
        assert(fp.type == Exception::FalsePath && fp.path.from[0].kind == Kind::Clock && fp.path.through.size() == 2);
        assert(fp.path.through[1][0].kind == Kind::Net && fp.path.to[0].name == "q");
        assert(s.exceptions[1].type == Exception::Multicycle && s.exceptions[1].value == 2 && s.exceptions[1].setup);
        assert(s.exceptions[1].path.to[0].name == "u_b/r_reg[0]");
        assert(s.exceptions[2].hold && s.exceptions[2].end);
        assert(s.exceptions[3].type == Exception::MaxDelay && s.exceptions[3].value == 3.5 && s.exceptions[3].datapathOnly);
        assert(s.exceptions[3].path.from[0].kind == Kind::Port && s.exceptions[3].path.to.size() == 3);
        assert(s.uncertainties.size() == 2 && s.uncertainties[0].setup && s.uncertainties[1].from[0] == "a" && s.uncertainties[1].to[0] == "b");
        assert(s.latencies.size() == 1 && s.latencies[0].source && s.latencies[0].max && s.latencies[0].value == 1.2);
        assert(s.caseAnalysis.size() == 1 && s.caseAnalysis[0].value == "0");
        assert(s.propagated.size() == 2);
        assert(s.of("set_load").size() == 1 && s.of("set_load")[0]->has("-pin_load") && s.of("set_load")[0]->num("value") == 0.05);
        auto* dc = s.of("set_driving_cell")[0];
        assert(dc->str("-lib_cell") == "BUFX2" && dc->objs("objects").size() == 7 && dc->loc.line == 14);
    }
    // Object queries: globs, buses, -hierarchical, -filter, -of_objects, collections
    {
        Reader r;
        r.design = &toy;
        std::vector<std::string> out;
        r.onPuts = [&](const std::string& s) { out.push_back(s); };
        r.parse("puts [sizeof_collection [get_ports d]]\n"
                "puts [sizeof_collection [get_ports {d[1]}]]\n"
                "puts [sizeof_collection [get_ports clk*]]\n"
                "puts [sizeof_collection [get_cells *]]\n"            // one level only
                "puts [sizeof_collection [get_cells -hierarchical *_reg*]]\n"
                "puts [get_object_name [get_cells -hier -filter {is_sequential == true && full_name =~ u_a*} *]]\n"
                "puts [get_object_name [get_pins -of_objects [get_cells u_a/q_reg]]]\n"
                "puts [sizeof_collection [get_pins u_a/*/CK]]\n"
                "puts [sizeof_collection [get_pins -regexp {u_.*/CK}]]\n"
                "set c [add_to_collection [get_ports clk] [get_ports rst_n]]\n"
                "set n 0 ; foreach_in_collection x $c { incr n }\n"
                "puts \"$n [sizeof_collection [remove_from_collection $c [get_ports clk]]]\"\n"
                "puts [sizeof_collection [get_ports -quiet nothing*]]\n"
                "puts [sizeof_collection [all_inputs]]\n",
                "q.sdc");
        std::vector<std::string> want{"3", "1", "2", "3", "3", "u_a/q_reg", "u_a/q_reg/CK u_a/q_reg/D u_a/q_reg/Q", "1", "2", "2 1", "0", "7"};
        if (out != want) for (auto& s : out) std::cerr << "[" << s << "]\n";
        assert(out == want);
        assert(r.diags.empty());
    }
    // Diagnostics: each keeps its file and line, and the reader carries on
    {
        Reader r;
        r.design = &toy;
        r.parse("create_clock -name a -period 10 [get_ports nope]\n"
                "set_input_delay -clock missing 1 [get_ports d]\n"
                "create_clock -period ten [get_ports clk]\n"
                "set_false_path -frm [get_ports d]\n"
                "report_timing\n"
                "set_input_delay -clock a\n"
                "set x $undefined\n"
                "create_clock -name ok -period 4 [get_ports clk2]\n",
                "bad.sdc");
        assert(count(r, "no-match") == 1 && count(r, "no-clock") == 1 && count(r, "bad-value") == 2);
        assert(count(r, "unknown-option") == 1 && count(r, "unknown-command") == 1 && count(r, "missing-value") == 2);
        assert(count(r, "syntax") == 1);
        for (auto& d : r.diags)
            if (d.code == "unknown-command") assert(d.loc.file == "bad.sdc" && d.loc.line == 5);
        assert(!r.ok());
        assert(r.sdc.clock("ok") && r.sdc.ioDelays[0].clock == "missing");
    }
    // No design: patterns are kept unresolved; host commands; option prefixes
    {
        Reader r;
        std::string seen;
        r.addCommand("report_timing", [&](const std::vector<std::string>& w) {
            seen = w.size() > 2 ? w[2] : "";
            return std::string("slack 1.5");
        });
        r.extra["set_dont_touch"] = "| objects=o:cn value=s?";
        r.parse("create_clock -p 5 -n c [get_ports clk]\n"
                "set_false_path -from [get_pins a/b/*] -to [get_ports o*]\n"
                "set r [report_timing -to $r_missing_is_fine_inside_braces_not_here]\n",
                "n.sdc");
        assert(count(r, "syntax") == 1);  // $r_missing...: an error with a line, and the reader goes on
        r.diags.clear();
        r.parse("set r [report_timing -to x]\nset_dont_touch [get_cells u1] true\nif {$r eq \"slack 1.5\"} { create_clock -name h -period 1 }\n", "n2.sdc");
        assert(r.diags.empty() && seen == "x" && r.sdc.clock("h"));
        assert(r.sdc.clocks[0].period == 5 && r.sdc.clocks[0].name == "c");
        assert(!r.sdc.clocks[0].sources[0].resolved && r.sdc.clocks[0].sources[0].name == "clk");
        assert(r.sdc.exceptions[0].path.from[0].kind == Kind::Pin && r.sdc.exceptions[0].path.from[0].name == "a/b/*");
        assert(r.sdc.of("set_dont_touch").size() == 1);
    }
    // source, relative to the including file
    {
        std::ofstream("test_sdc_inc.sdc") << "create_clock -name inc -period 3\n";
        std::ofstream("test_sdc_top.sdc") << "source test_sdc_inc.sdc\nsource -echo ./test_sdc_inc.sdc\n";
        Reader r;
        r.load("test_sdc_top.sdc");
        assert(r.diags.empty() && r.sdc.clocks.size() == 2 && r.sdc.clocks[0].loc.file.find("test_sdc_inc.sdc") != std::string::npos);
        std::remove("test_sdc_inc.sdc");
        std::remove("test_sdc_top.sdc");
    }
    std::cout << "test_sdc: ok\n";
    return 0;
}
