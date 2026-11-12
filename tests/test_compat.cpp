// Unit test for src/compat.hpp (vendor command-line compatibility layer).
#include "compat.hpp"
#include <cassert>
#include <iostream>

using namespace compat;
using V = std::vector<std::string>;

static const Alias kAliases[] = {{"xrun", Vendor::Cadence}, {"qrun", Vendor::Siemens}, {"vcs", Vendor::Synopsys}};
static const Rule kRules[] = {
    {Vendor::Cadence, "-access", Act::Drop, {}, true, false, "all signals are visible"},
    {Vendor::Cadence, "-svseed", Act::Rename, "-seed", true},
    {Vendor::Cadence, "-input", Act::Rename, "-i -do", true},
    {Vendor::Cadence, "-define", Act::Rename, "-D", true},
    {Vendor::Siemens, "-sv_seed", Act::Rename, "-seed", true},
    {Vendor::Siemens, "-voptargs=", Act::Drop, {}, false, true},
    {Vendor::Synopsys, "+ntb_random_seed=", Act::Rename, "-seed=", false, true},
};

int main() {
    // --compat wins over argv0 and is removed from args.
    V a{"top.sv", "--compat=siemens", "-c"};
    assert(detect("/usr/bin/xrun", a, kAliases) == Vendor::Siemens);
    assert((a == V{"top.sv", "-c"}));
    V b{"-compat", "Cadence", "x.sv"};
    assert(detect("msim", b, kAliases) == Vendor::Cadence);
    assert((b == V{"x.sv"}));
    // argv0 alias, with directory and .exe stripped.
    V c{"x.sv"};
    assert(detect("C:\\tools\\qrun.exe", c, kAliases) == Vendor::Siemens);
    assert(detect("./vcs", c, kAliases) == Vendor::Synopsys);
    assert(detect("msim", c, kAliases) == Vendor::Native);
    // Unknown vendor name is left for the tool to reject.
    V d{"--compat=foo"};
    assert(detect("msim", d, kAliases) == Vendor::Native && d.size() == 1);
    assert(parseVendor("mentor") == Vendor::Siemens && parseVendor("cdns") == Vendor::Cadence);

    // Cadence: case-insensitive, rename with value, multi-word target, drops.
    Result r = translate(Vendor::Cadence,
        {"-SV", "-ACCESS", "+rwc", "-svseed", "7", "-input", "run.tcl", "-64bit", "-define", "A=1", "top.sv"}, kRules);
    assert((r.args == V{"-SV", "-seed", "7", "-i", "-do", "run.tcl", "-D", "A=1", "top.sv"}));
    assert(r.notes.size() == 2);
    assert(r.notes[0] == "ignored cadence option -ACCESS +rwc (all signals are visible)");
    // Siemens: case-sensitive, prefix drop.
    r = translate(Vendor::Siemens, {"-voptargs=+acc", "-sv_seed", "3", "-64", "-SV_SEED"}, kRules);
    assert((r.args == V{"-seed", "3", "-SV_SEED"}));
    assert(r.notes.size() == 2);
    // Synopsys: prefix rename keeps the suffix.
    r = translate(Vendor::Synopsys, {"+ntb_random_seed=42", "-full64"}, kRules);
    assert((r.args == V{"-seed=42"}) && r.notes.size() == 1);
    // Native: untouched, even if a rule would match another vendor.
    r = translate(Vendor::Native, {"-svseed", "1", "-64bit"}, kRules);
    assert((r.args == V{"-svseed", "1", "-64bit"}) && r.notes.empty());
    // Option with a missing value at the end does not read past args.
    r = translate(Vendor::Cadence, {"-svseed"}, kRules);
    assert((r.args == V{"-seed"}));

    Msg m{Severity::Error, "xmvlog", "EXPSMC", "top.sv", 12, 5, "expecting a semicolon"};
    assert(format(Vendor::Cadence, m) == "xmvlog: *E,EXPSMC (top.sv,12|5): expecting a semicolon");
    assert(format(Vendor::Siemens, m) == "** Error: (xmvlog-EXPSMC) top.sv(12): expecting a semicolon");
    assert(format(Vendor::Synopsys, m) == "Error-[EXPSMC] expecting a semicolon\n  \"top.sv\", 12");
    assert(format(Vendor::Native, m) == "top.sv:12:5: error: expecting a semicolon [EXPSMC]");
    Msg w{Severity::Warning, "msim", "W1", "", 0, 0, "no file"};
    assert(format(Vendor::Cadence, w) == "msim: *W,W1: no file");
    assert(format(Vendor::Native, w) == "warning: no file [W1]");
    std::cout << "test_compat: OK\n";
}
