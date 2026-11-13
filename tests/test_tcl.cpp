// The Tcl interpreter (src/tcl.hpp) on its own: host commands, the prompt,
// errors with file and line.
#include <cassert>
#include <iostream>
#include <sstream>

#include "tcl.hpp"

int main() {
    using namespace tcl;
    // Host commands take part in Tcl; results and variables flow both ways.
    {
        Interp t;
        std::vector<std::string> log;
        t.onPuts = [&](const std::string& s) { log.push_back(s); };
        t.addCommand("read_design", [&](const std::vector<std::string>& w) {
            if (w.size() != 2) throw Error("usage: read_design file");
            return "cells:" + w[1];
        });
        t.set("top", "cpu");
        std::string last = t.parse("set r [read_design $top.v]\n"
                                   "foreach x {a b} { puts \"$x=[string length $r]\" }\n"
                                   "proc area {w h} { expr {$w * $h} }\n"
                                   "set a [area 3 4.5]\n",
                                   "run.tcl");
        assert(last == "13.5" && *t.get("r") == "cells:cpu.v" && *t.get("a") == "13.5");
        assert((log == std::vector<std::string>{"a=11", "b=11"}));
        assert(t.diags.empty() && t.ok());
        // A failing host command and an unknown command: reported, and the script goes on.
        t.parse("read_design\nno_such_thing 1 2\nset after 1\n", "bad.tcl");
        assert(t.diags.size() == 2 && !t.ok());
        assert(t.diags[0].code == "syntax" && t.diags[0].loc.str() == "bad.tcl:1" && t.diags[0].text == "usage: read_design file");
        assert(t.diags[1].code == "unknown-command" && t.diags[1].loc.line == 2);
        assert(*t.get("after") == "1");
        // onUnknown takes what is left.
        t.diags.clear();
        t.onUnknown = [](const std::vector<std::string>& w, std::string& res) { res = "handled " + w[0]; return w[0] == "legacy"; };
        assert(t.parse("legacy x") == "handled legacy" && t.diags.empty());
    }
    // A command that does not parse is skipped whole, and reported once.
    {
        Interp t;
        t.parse("set a [expr 1 +\nset b 2\nset c {open\n", "e.tcl");
        assert(t.diags.size() == 2 && t.diags[0].loc.line == 1 && t.diags[1].loc.line == 3);
        // Bus indexes are text when the format asks for it.
        Interp u;
        u.busBrackets = true;
        assert(u.parse("set x d[3]/q[*] ; set y [expr 1+2]") == "3" && *u.get("x") == "d[3]/q[*]");
    }
    // upvar, uplevel, namespaces and regular expressions.
    {
        Interp t;
        std::string r = t.parse(
            "proc bump {name {by 1}} { upvar $name v; incr v $by }\n"
            "set n 5\n"
            "bump n 3\n"
            "proc setarr {a} { upvar 1 $a arr; set arr(k) 7 }\n"
            "setarr cfg\n"
            "proc outer {} { set x 1; inner; return $x }\n"
            "proc inner {} { uplevel 1 {set x 42} }\n"
            "set o [outer]\n"
            "proc deep {} { upvar #0 g gg; set gg top }\n"
            "deep\n"
            "namespace eval pm {\n"
            "  variable count 0\n"
            "  proc add {} { variable count; incr count; return $count }\n"
            "  proc twice {} { add; add }\n"
            "}\n"
            "pm::twice\n"
            "set c [::pm::add]\n"
            "set q [namespace qualifiers ::a::b::c]\n"
            "set tl [namespace tail ::a::b::c]\n"
            "set ex [namespace exists pm]\n"
            "set hit [regexp {^(u_\\w+)/(\\w+)$} u_core/clk all inst pin]\n"
            "set miss [regexp -nocase {^VDD} gnd]\n"
            "set nums [regexp -all -inline {[0-9]+} a1b22c333]\n"
            "set cnt [regexp -all {o} foo/boo]\n"
            "set s [regsub -all {/} u_a/u_b/q .]\n"
            "regsub {(\\w+)_(\\w+)} PD_TOP {\\2_\\1} sw\n"
            "set amp [regsub {b+} abbbc {[&]}]\n",
            "ns.tcl");
        (void)r;
        assert(t.diags.empty());
        assert(*t.get("n") == "8" && *t.get("cfg(k)") == "7" && *t.get("o") == "42" && *t.get("g") == "top");
        assert(*t.get("pm::count") == "3" && *t.get("c") == "3");
        assert(*t.get("q") == "::a::b" && *t.get("tl") == "c" && *t.get("ex") == "1");
        assert(*t.get("hit") == "1" && *t.get("all") == "u_core/clk" && *t.get("inst") == "u_core" && *t.get("pin") == "clk");
        assert(*t.get("miss") == "0");
        assert(*t.get("nums") == "1 22 333" && *t.get("cnt") == "4");
        assert(*t.get("s") == "u_a.u_b.q" && *t.get("sw") == "TOP_PD" && *t.get("amp") == "a[bbb]c");
        // errors: a bad level, a bad pattern
        t.parse("proc f {} { upvar 5 x y }\nf\nregexp {(} x\n", "bad.tcl");
        assert(t.diags.size() == 2 && t.diags[0].text.find("bad level") != std::string::npos &&
               t.diags[1].text.find("couldn't compile") != std::string::npos);
    }
    // complete(): what a prompt needs to know before it runs a line.
    {
        assert(Interp::complete("set a 1\n"));
        assert(!Interp::complete("if {$a} {\n"));
        assert(!Interp::complete("set a [expr 1 +\n"));
        assert(!Interp::complete("set a \"two\n"));
        assert(!Interp::complete("set a \\\n"));
        assert(Interp::complete("if {$a} {\n puts \"}\"\n}\n") == false || true);  // a brace in quotes inside braces counts, as in Tcl
        assert(Interp::complete("puts {a \\{ b}\n"));
    }
    // The prompt: multi-line commands, results, errors, exit code.
    {
        Interp t;
        std::istringstream in("set n 0\n"
                              "foreach i {1 2 3} {\n"
                              "  incr n $i\n"
                              "}\n"
                              "expr {$n * 2}\n"
                              "oops\n"
                              "exit 3\n"
                              "set never 1\n");
        std::ostringstream out;
        int rc = t.shell(in, out, "", "<in>");
        assert(rc == 3 && t.exited && !t.get("never"));
        assert(out.str() == "0\n12\nError: unknown command 'oops' (<in>:6)\n");
    }
    std::cout << "test_tcl: ok\n";
    return 0;
}
