// Lower a flat module with slang and run it on the kernel.
//
// counter.sv's always_ff is the same six instructions as the hand-built demo,
// and cnt+1 is 4 bits (15 wraps to 0), not the unsized literal's 32.
// checks.sv is also run on Icarus Verilog 13: an 8-bit context makes
// 4'hF+4'h1 equal 00010000, the 4-bit context wraps to 0000, and
// b=0; b<=1; $display prints 0 while $strobe / $monitor print 1.
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "lower.hpp"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/syntax/SyntaxTree.h"

#define MINISIM_NO_MAIN
#include "minisim.cpp"

static int fail(const std::string& msg) {
    std::cerr << "FAIL " << msg << "\n";
    return 1;
}

static std::string srcPath(const char* rel) {
    return std::string(MINISIM_SOURCE_DIR) + "/" + rel;
}

struct Compiled {
    LowerResult result;
    std::unordered_map<std::string, int> id;
};

static Compiled compileSv(const std::string& path, bool intoSim) {
    auto tree = slang::syntax::SyntaxTree::fromFile(path);
    if (!tree) throw std::runtime_error("cannot read " + path);

    slang::ast::Compilation comp;
    comp.addSyntaxTree(tree.value());

    slang::DiagnosticEngine engine(*comp.getSourceManager());
    auto client = std::make_shared<slang::TextDiagnosticClient>();
    engine.addClient(client);
    bool errors = false;
    for (auto& d : comp.getAllDiagnostics()) {
        engine.issue(d);
        if (d.isError()) errors = true;
    }
    std::string msgs = client->getString();
    if (errors) throw std::runtime_error(msgs.empty() ? "slang error in " + path : msgs);

    Compiled out;
    int next = 0;
    out.result = lower(
        comp,
        [&](const std::string& n, int w) {
            int id = intoSim ? S.sig(n, w) : next++;
            out.id[n] = id;
            return id;
        },
        [&](int s) { return intoSim ? S.newDriver(s) : 0; });
    return out;
}

static std::string describe(const E& e) {
    if (!e) return "null";
    if (e->op == Op::Const)
        return "const w=" + std::to_string(e->k.w) + " v=" + std::to_string(e->k.v);
    if (e->op == Op::Ref) return "ref " + std::to_string(e->sig);
    if (e->op == Op::Add) return "add(" + describe(e->kids[0]) + ", " + describe(e->kids[1]) + ")";
    if (e->op == Op::Concat) return "concat(" + std::to_string(e->kids.size()) + ")";
    return "op";
}

static const char* ikName(IK k) {
    switch (k) {
        case IK::Assign: return "Assign";
        case IK::Nba: return "Nba";
        case IK::JmpIfNot: return "JmpIfNot";
        case IK::Jmp: return "Jmp";
        case IK::WaitEdge: return "WaitEdge";
        case IK::WaitDelay: return "WaitDelay";
        case IK::Finish: return "Finish";
        case IK::Display: return "Display";
        case IK::Strobe: return "Strobe";
        case IK::Monitor: return "Monitor";
    }
    return "?";
}

static int testHier() {
    bool threw = false;
    std::string msg;
    try {
        compileSv(srcPath("examples/hier.sv"), false);
    } catch (const std::exception& ex) {
        threw = true;
        msg = ex.what();
    }
    if (!threw) return fail("hier.sv did not throw");
    if (msg.find("not supported") == std::string::npos ||
        msg.find("port connections") == std::string::npos)
        return fail("hier.sv threw an unexpected error: " + msg);
    return 0;
}

static int testCounter() {
    Compiled c = compileSv(srcPath("examples/counter.sv"), false);
    if (c.result.procs.size() != 1) return fail("counter proc count");
    if (!c.result.procs[0].loops) return fail("always_ff should loop");
    const Prog& p = c.result.procs[0].prog;
    if (p.code.size() != 6) {
        std::string s = "counter instr count " + std::to_string(p.code.size()) + ":";
        for (auto& i : p.code) s += std::string(" ") + ikName(i.k);
        return fail(s);
    }
    int clk = c.id.at("clk"), rst = c.id.at("rst"), cnt = c.id.at("cnt");
    const auto& code = p.code;
    if (code[0].k != IK::WaitEdge || code[0].sig != clk || code[0].edge != POS)
        return fail("counter[0] posedge clk");
    if (code[1].k != IK::JmpIfNot || code[1].target != 4 || !code[1].e ||
        code[1].e->op != Op::Ref || code[1].e->sig != rst)
        return fail("counter[1] if (rst)");
    if (code[2].k != IK::Nba || code[2].sig != cnt || !code[2].e || code[2].e->op != Op::Const ||
        code[2].e->k.w != 4 || code[2].e->k.v != 0)
        return fail("counter[2] NBA 0 should be 4-bit, got " + describe(code[2].e));
    if (code[3].k != IK::Jmp || code[3].target != 5) return fail("counter[3] else jump");
    if (code[4].k != IK::Nba || code[4].sig != cnt || !code[4].e || code[4].e->op != Op::Add)
        return fail("counter[4] NBA cnt+1, got " + describe(code[4].e));
    const E& add = code[4].e;
    if (add->kids.size() != 2 || add->kids[0]->op != Op::Ref || add->kids[0]->sig != cnt ||
        add->kids[1]->op != Op::Const || add->kids[1]->k.w != 4 || add->kids[1]->k.v != 1)
        return fail("counter cnt+1 should be 4-bit, got " + describe(add));
    if (code[5].k != IK::Jmp || code[5].target != 0) return fail("counter[5] loop");

    // 15+1 in that 4-bit add wraps to 0. A 32-bit unsized +1 would be 16.
    Val wrapped = eval(add, [&](int s) { return s == cnt ? mk(15, 4) : mk(0, 1); });
    if (wrapped.w != 4 || wrapped.v != 0 || wrapped.x != 0)
        return fail("4-bit 15+1 wrapped to w=" + std::to_string(wrapped.w) +
                    " v=" + std::to_string(wrapped.v));
    return 0;
}

static const Instr* findAssign(const Prog& p, int sig) {
    for (auto& i : p.code)
        if ((i.k == IK::Assign || i.k == IK::Nba) && i.sig == sig) return &i;
    return nullptr;
}

static int testChecks() {
    Compiled c = compileSv(srcPath("examples/checks.sv"), true);
    if (c.result.procs.size() != 1) return fail("checks proc count");
    if (c.result.procs[0].loops) return fail("initial should not loop");
    const Prog& p = c.result.procs[0].prog;

    const IK wantK[] = {IK::Assign, IK::Display, IK::Assign, IK::Display, IK::Assign,
                        IK::Nba,    IK::Display, IK::Strobe, IK::Monitor, IK::Finish};
    if (p.code.size() != 10) {
        std::string s = "checks instr count " + std::to_string(p.code.size()) + ":";
        for (auto& i : p.code) s += std::string(" ") + ikName(i.k);
        return fail(s);
    }
    for (int i = 0; i < 10; i++)
        if (p.code[i].k != wantK[i])
            return fail(std::string("checks instr ") + std::to_string(i) + " is " + ikName(p.code[i].k));

    if (p.code[1].fmt != "W %b" || p.code[3].fmt != "N %b" || p.code[6].fmt != "D %b")
        return fail("display formats: [" + p.code[1].fmt + "] [" + p.code[3].fmt + "] [" +
                    p.code[6].fmt + "]");
    if (p.code[7].fmt != "S %b" || p.code[7].k != IK::Strobe) return fail("strobe format");
    if (p.code[8].fmt != "M %b" || p.code[8].k != IK::Monitor) return fail("monitor format");
    if (p.code[9].k != IK::Finish) return fail("finish");

    int wide = c.id.at("wide"), narrow = c.id.at("narrow"), b = c.id.at("b");
    const Instr* wAsg = findAssign(p, wide);
    const Instr* nAsg = findAssign(p, narrow);
    if (!wAsg || !wAsg->e || wAsg->e->op != Op::Add) return fail("wide rhs " + describe(wAsg ? wAsg->e : E{}));
    if (!nAsg || !nAsg->e || nAsg->e->op != Op::Add) return fail("narrow rhs " + describe(nAsg ? nAsg->e : E{}));
    auto litOk = [](const E& e, int w, uint64_t v) {
        return e && e->op == Op::Const && e->k.w == w && e->k.v == v && e->k.x == 0;
    };
    if (!litOk(wAsg->e->kids[0], 8, 15) || !litOk(wAsg->e->kids[1], 8, 1))
        return fail("wide add should be 8-bit 15+1, got " + describe(wAsg->e));
    if (!litOk(nAsg->e->kids[0], 4, 15) || !litOk(nAsg->e->kids[1], 4, 1))
        return fail("narrow add should be 4-bit 15+1, got " + describe(nAsg->e));

    Val wideV = eval(wAsg->e, [](int) { return mk(0, 1); });
    Val narrowV = eval(nAsg->e, [](int) { return mk(0, 1); });
    if (wideV.w != 8 || wideV.v != 16) return fail("wide eval");
    if (narrowV.w != 4 || narrowV.v != 0) return fail("narrow eval");

    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-lower-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {"W 00010000", "N 0000", "D 0", "S 1", "M 1"};
    std::vector<std::string> got;
    std::string cur;
    for (char ch : buf.str()) {
        if (ch == '\n') { got.push_back(cur); cur.clear(); }
        else cur.push_back(ch);
    }
    if (!cur.empty()) got.push_back(cur);
    if (got != want) {
        std::cerr << "FAIL checks stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    Val gv = S.get(wide), nv = S.get(narrow), bv = S.get(b);
    if (gv.w != 8 || gv.v != 16 || gv.x) return fail("wide signal after run");
    if (nv.w != 4 || nv.v != 0 || nv.x) return fail("narrow signal after run");
    if (bv.w != 1 || bv.v != 1 || bv.x) return fail("b after NBA");
    if (!S.finished || S.now != 0) return fail("$finish did not stop at time 0");
    return 0;
}

static std::vector<std::string> linesOf(const std::string& text) {
    std::vector<std::string> got;
    std::string cur;
    for (char ch : text) {
        if (ch == '\n') {
            got.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    if (!cur.empty()) got.push_back(cur);
    return got;
}

static bool hasOp(const E& e, Op op) {
    if (!e) return false;
    if (e->op == op) return true;
    for (const auto& k : e->kids)
        if (hasOp(k, op)) return true;
    return false;
}

static bool progHas(const Prog& p, Op op) {
    for (const auto& i : p.code) {
        if (hasOp(i.e, op)) return true;
        for (const auto& a : i.args)
            if (hasOp(a, op)) return true;
    }
    return false;
}

// examples/div.sv, checked against Icarus Verilog 13 (v13_0, dfeee909).
// An unsigned-only divide prints S_N_DIV 0010 (9/3) instead of 1110.
// Ignoring X prints X_N_DIV 0100. Skipping the width push prints WIDE_MIN 11111000.
static int testDiv() {
    S.finished = false;
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/div.sv"), true);
    if (c.result.procs.size() != 1) return fail("div proc count");
    if (!progHas(c.result.procs[0].prog, Op::Div) || !progHas(c.result.procs[0].prog, Op::Mod))
        return fail("div.sv did not lower / and %");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-div-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "U_DIV 0010", "U_MOD 0001", "U_DIV15 0011", "U_MOD15 0011",
        "U_0DIV1 0000", "U_0MOD1 0000", "U_HI_DIV 0111", "U_HI_MOD 0001",
        "S_N_DIV 1110", "S_N_MOD 1111", "S_D_DIV 1110", "S_D_MOD 0001",
        "S_ND_DIV 0010", "S_ND_MOD 1111", "S_M1_DIV 0000", "S_M1_MOD 1111",
        "S_MIN2_DIV 1100", "S_MIN2_MOD 0000", "S_MIN1_DIV 1000", "S_MIN1_MOD 0000",
        "U_DIV0 xxxx", "U_MOD0 xxxx", "S_DIV0 xxxx", "S_MOD0 xxxx",
        "U_0DIV0 xxxx", "U_0MOD0 xxxx",
        "X_N_DIV xxxx", "X_N_MOD xxxx", "X_D_DIV xxxx", "X_D_MOD xxxx",
        "Z_N_DIV xxxx", "Z_N_MOD xxxx", "Z_D_DIV xxxx", "Z_D_MOD xxxx",
        "X_S_DIV xxxx", "X_S_MOD xxxx",
        "MIX_DIV 0100", "MIX_MOD 0000", "MIX_UD_DIV 0000", "MIX_UD_MOD 0111",
        "CAST_U_DIV 0100", "CAST_U_MOD 0000", "CAST_S_DIV 1100", "CAST_S_MOD 0000",
        "NARROW_MIN 1000", "NARROW_MIN_MOD 0000",
        "WIDE_MIN 00001000", "WIDE_MIN_U 00001000", "WIDE_MIX 00000010",
        "WIDE_S3 11111110", "WIDE_S3_MOD 11111110",
        "S64_MIN1_DIV 1000000000000000000000000000000000000000000000000000000000000000",
        "S64_MIN1_MOD 0000000000000000000000000000000000000000000000000000000000000000",
        "S64_MIN3_DIV 1101010101010101010101010101010101010101010101010101010101010110",
        "S64_MIN3_MOD 1111111111111111111111111111111111111111111111111111111111111110",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL div stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    return 0;
}

// examples/signed.sv, checked against Icarus Verilog 13 (v13_0, dfeee909).
// Dropping Val::sgn makes CMP_S print 0 and EXT_S print 00001000.
static int testSigned() {
    S.finished = false;
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/signed.sv"), true);
    if (c.result.procs.size() != 1) return fail("signed proc count");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-signed-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "CMP_S 1",     "CMP_U 0",      "CMP_SU 0",    "CMP_SS 1",
        "CMP_LE 1",    "CMP_GT 0",     "EQ_S 1",      "EQ_M 0",
        "EXT_S 11111000", "EXT_US 11111000", "EXT_SU 00001000",
        "ADD_SS 11110001", "ADD_SS_U 11110001",
        "ADD_MIX 00010000", "ADD_MIX_S 00010000",
        "SUB 11110111", "MUL 11110000",
        "ASR 11111100", "ASR4 1100", "LSR 01111100", "ASR_U 00000100",
        "SHL 11110000", "SHL_U 00010000",
        "CAST_S 1", "CAST_U 0", "CAST_EXT 11111000", "CAST_UEXT 00001000",
        "LT_X x", "ADD_X xxxxxxxx", "ASR_X xxxxxx00", "ASR_Z zzzzzz00",
        "LT_Z x", "ASR_GE 11111111", "ASR_XC xxxxxxxx",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL signed stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    return 0;
}

int main() {
    if (int rc = testHier()) return rc;
    if (int rc = testCounter()) return rc;
    if (int rc = testChecks()) return rc;
    if (int rc = testSigned()) return rc;
    if (int rc = testDiv()) return rc;
    std::cout << "ok\n";
    return 0;
}
