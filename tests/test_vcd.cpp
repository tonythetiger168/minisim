// Demo VCD must checkpoint every signal at time 0, before the first event.
// The hand-built counter (./minisim) starts cnt at X and does not change it
// until #5, so a change-only writer omits cnt from #0. $dumpvars is that sample.
//
// Expected $dumpvars values are the pre-event state: clk/rst/cnt are variables,
// so they are X. The #0 change records (clk=0, rst=1) and everything after them
// are the trace this file had before the checkpoint was added.
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static int fail(const std::string& msg) {
    std::cerr << "FAIL " << msg << "\n";
    return 1;
}

// A timestamp is a line '#<digits>'. The cnt id is also '#', so a raw find is not enough.
static bool has_timestamp(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        if (s[i] == '#' && i + 1 < e && std::isdigit(static_cast<unsigned char>(s[i + 1]))) return true;
        i = e + (e < s.size() ? 1 : 0);
        if (e == s.size()) break;
    }
    return false;
}

// Value changes from the first timestamp on. Must stay identical.
static const char kTrace[] = R"vcd(#0
0!
1"
#5
1!
b0000 #
#10
0!
#15
1!
#20
0!
#22
0"
#25
1!
b0001 #
#30
0!
#35
1!
b0010 #
#40
0!
#45
1!
b0011 #
#50
0!
#55
1!
b0100 #
#60
0!
#65
1!
b0101 #
#70
0!
#75
1!
b0110 #
#80
0!
#85
1!
b0111 #
#90
0!
#95
1!
b1000 #
#100
0!
#105
1!
b1001 #
#110
0!
#115
1!
b1010 #
#120
0!
)vcd";

// clk = !, rst = ", cnt = #. Variables, full-width, before any event.
static const char kDumpvars[] =
    "x!\n"
    "x\"\n"
    "bxxxx #\n";

int main(int argc, char** argv) {
    if (argc != 2) return fail("usage: test_vcd <minisim>");

    fs::path dir = fs::temp_directory_path() / "minisim-vcd-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    if (ec) return fail("could not create " + dir.string());

    std::string cmd = "cd " + dir.string() + " && " + argv[1] + " >stdout.txt 2>stderr.txt";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return fail("demo exited " + std::to_string(rc));

    std::ifstream in(dir / "dump.vcd");
    if (!in) return fail("demo wrote no dump.vcd");
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string vcd = ss.str();

    const std::string decls =
        "$var reg 1 ! clk $end\n"
        "$var reg 1 \" rst $end\n"
        "$var reg 4 # cnt $end\n";
    if (vcd.find(decls) == std::string::npos) {
        std::cerr << "FAIL $var declarations\n" << vcd << "\n";
        return 1;
    }

    const std::string block = std::string("$dumpvars\n") + kDumpvars + "$end\n";
    auto at = vcd.find(block);
    if (at == std::string::npos) {
        std::cerr << "FAIL $dumpvars block at #0\n--- file ---\n" << vcd << "\n";
        return 1;
    }
    // Time 0 is implicit until the first #. A timestamp before the checkpoint
    // would mean the sample is not the pre-event state.
    if (has_timestamp(vcd.substr(0, at))) return fail("$dumpvars is not at time 0");
    if (vcd.find("$dumpvars", at + 1) != std::string::npos) return fail("more than one $dumpvars");

    std::string tail = vcd.substr(at + block.size());
    if (tail != kTrace) {
        std::cerr << "FAIL value trace after $dumpvars changed\n--- got ---\n" << tail
                  << "--- want ---\n" << kTrace;
        return 1;
    }

    std::ifstream out(dir / "stdout.txt");
    std::string stdout((std::istreambuf_iterator<char>(out)), std::istreambuf_iterator<char>());
    if (stdout.find("t=5 cnt=0\n") == std::string::npos) return fail("demo stdout missing cnt at t=5");
    if (stdout.find("$finish at t=122\n") == std::string::npos) return fail("demo did not $finish at t=122");

    std::cout << "ok\n";
    return 0;
}
