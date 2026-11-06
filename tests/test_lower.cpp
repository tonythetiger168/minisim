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
        [&](const std::string& n, int w, bool net) {
            int id = intoSim ? S.sig(n, w, net) : next++;
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
        case IK::Call: return "Call";
        case IK::Ret: return "Ret";
    }
    return "?";
}

static int testHier() {
    Compiled c = compileSv(srcPath("examples/hier.sv"), false);
    if (c.result.procs.empty()) return fail("hier.sv produced no processes");
    if (!c.id.count("x") || !c.id.count("y")) return fail("hier parent signals");
    if (!c.id.count("u.a") || !c.id.count("u.b")) return fail("hier child signals");
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

// examples/compound.sv. a += b matches a = a + b, including a wider
// operand (NDIV is 0, not X from a 4-bit divide by the low bits of 16)
// and a signed operand widened into an unsigned add (MIX is 9, not -7).
// Icarus 13's compound /= and %= truncate a wider operand to the left-hand
// width (NDIV xxxx, N20 0001, NMOD 0011) while a = a / b stays wide
// (0000, 0000, 0111). minisim follows the plain assignment.
static int testCompound() {
    S.finished = false;
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/compound.sv"), true);
    if (c.result.procs.size() != 1) return fail("compound proc count");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-compound-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "U_ADD 00101100", "U_ADD_EQ 00101100",
        "U_SUB 11100010", "U_SUB_EQ 11100010",
        "U_MUL 00000100", "U_MUL_EQ 00000100",
        "U_DIV 00011100", "U_DIV_EQ 00011100",
        "U_MOD 00000100", "U_MOD_EQ 00000100",
        "S_ADD 10111010", "S_ADD_EQ 10111010",
        "S_SUB 11001110", "S_SUB_EQ 11001110",
        "S_MUL 11101000", "S_MUL_EQ 11101000",
        "S_DIV 11111011", "S_DIV_EQ 11111011",
        "S_MOD 11111011", "S_MOD_EQ 11111011",
        "AND 10001000", "AND_EQ 10001000",
        "OR 11101110", "XOR 01100110",
        "SHL 00110100", "SHL_EQ 00110100",
        "SHR 00000011",
        "ASR 11111100", "ASR_EQ 11111100",
        "ASR_U 00111100",
        "X_ADD xxxx", "X_ADD_EQ xxxx",
        "Z_AND 1x00",
        "NAR 0000", "NAR_EQ 0000", "NAR_W 00010000",
        "NDIV 0000", "NDIV_EQ 0000",
        "N20 0000", "N20_EQ 0000",
        "NMOD 0111", "NMOD_EQ 0111",
        "WADD 11111001", "MIX 00001001",
        "S4_DIV 1110", "S4_DIV_EQ 1110",
        "S4_MOD 1110",
        "S4_ASR 1100", "S4_ASR_EQ 1100",
        "S4_MUL 0000",
        "PART 10110000",
        "FOR 0110",
        "DIV0 xxxx",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL compound stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    return 0;
}

// examples/breakcont.sv. break leaves the innermost loop. continue in a for
// still runs the step, so FOR_CONT_I is 4 and FOR_CONT_G is 4. The guard ends
// that loop if the step is skipped, and those counts then disagree.
static int testBreak() {
    S.finished = false;
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/breakcont.sv"), true);
    if (c.result.procs.size() != 1) return fail("breakcont proc count");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-break-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "FOR_BRK 0000", "FOR_BRK 0001", "FOR_BRK_I 0010", "FOR_BRK_G 0011",
        "FOR_CONT 0000", "FOR_CONT 0010", "FOR_CONT 0011",
        "FOR_CONT_I 0100", "FOR_CONT_G 0100", "FOR_CONT_S 0011",
        "WH_BRK 0000", "WH_BRK 0001", "WH_BRK_I 0010",
        "WH_CONT 0001", "WH_CONT 0011", "WH_CONT 0100", "WH_CONT_I 0100",
        "NEST_BRK 0000 0000", "NEST_BRK 0001 0000", "NEST_BRK 0010 0000",
        "NEST_CONT 0000 0000", "NEST_CONT 0000 0010",
        "NEST_CONT 0001 0000", "NEST_CONT 0001 0010",
        "CASE 0000", "CASE_I 0010", "CASE_G 0011", "CASE_S 0001",
        "DO_BRK 0000", "DO_BRK 0001", "DO_BRK_I 0010",
        "DO_CONT 0000", "DO_CONT_I 1010",
        "FV_BRK 0000", "FV_BRK 0001", "FV_BRK 0010", "FV_BRK_I 0011", "FV_BRK_G 0100",
        "FV_CONT 0001", "FV_CONT 0011", "FV_CONT 0100", "FV_CONT_I 0100", "FV_CONT_G 0100",
        "RP_BRK 0000", "RP_BRK 0001", "RP_BRK_I 0001",
        "RP_CONT 0001", "RP_CONT 0011", "RP_CONT 0100", "RP_CONT_I 0100",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL breakcont stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    return 0;
}

// examples/tasks.sv. A wrong return value, a dropped output, or a rejected
// call fails these lines. Icarus Verilog 13 is the source of the text.
static int testTasks() {
    S.finished = false;
    S.now = 0;
    S.wheel.clear();
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/tasks.sv"), true);
    if (c.result.procs.empty()) return fail("tasks proc count");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-tasks-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "FUNC 00010000", "NAME 00000111", "SUM 00011000", "DISP 00010000", "IF 1",
        "NEST 00000111", "SEXT 11111000", "ZEXT 00001000", "SLT 1", "SLT0 0",
        "XZ 10xz", "ADDX xxxx", "SEXTZ zzzzz000",
        "STICKY 00000101", "STICKY2 00000101",
        "STEP 00000010", "STEP2 00000011",
        "ASTEP 00000010", "ASTEP2 00000010",
        "TASK 00010001", "INOUT 00000101", "NAMED 00000011", "PASSZ z10x",
        "COUNT 00000100", "EARLY 00000001",
        "D0 0000", "IN", "MID", "OUT", "D5 1010",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL tasks stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    if (S.now < 5) return fail("delayed task did not reach #5");
    return 0;
}

// examples/hierarchy.sv. A dropped input, a dropped output, or a rejected
// instance fails these lines. Icarus Verilog 13 is the source of the text.
static int testHierarchy() {
    S.finished = false;
    S.now = 0;
    S.wheel.clear();
    S.mon.on = false;
    S.mon.queued = false;
    S.vcd.close();
    S.active.clear();
    S.inactive.clear();
    S.postq.clear();
    S.nbaq.clear();

    Compiled c = compileSv(srcPath("examples/hierarchy.sv"), true);
    if (c.result.procs.empty()) return fail("hierarchy proc count");
    for (auto& pr : c.result.procs) S.spawn(exec(&pr.prog));

    auto saved = std::filesystem::current_path();
    auto tmp = std::filesystem::temp_directory_path() / "minisim-hier-test";
    std::filesystem::create_directories(tmp);
    std::filesystem::current_path(tmp);
    std::stringstream buf;
    std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
    S.run(1000);
    std::cout.rdbuf(old);
    std::filesystem::current_path(saved);

    const std::vector<std::string> want = {
        "PROC0 0",
        "HIDE 1x0z",
        "NAMED 0",
        "OTHER 1",
        "POS 0",
        "EXPR 1",
        "GRAND 1100",
        "XZ 1x0z",
        "PROC1 1",
        "W8 10110001",
        "W4 0110",
        "INOUT 1",
    };
    auto got = linesOf(buf.str());
    if (got != want) {
        std::cerr << "FAIL hierarchy stdout\n--- got ---\n";
        for (auto& s : got) std::cerr << s << "\n";
        std::cerr << "--- want ---\n";
        for (auto& s : want) std::cerr << s << "\n";
        return 1;
    }
    if (S.now < 2) return fail("hierarchy did not reach #2");
    return 0;
}

static int testRecur() {
    bool threw = false;
    std::string msg;
    try {
        compileSv(srcPath("examples/recur.sv"), false);
    } catch (const std::exception& ex) {
        threw = true;
        msg = ex.what();
    }
    if (!threw) return fail("recur.sv did not throw");
    if (msg.find("recursive") == std::string::npos)
        return fail("recur.sv threw an unexpected error: " + msg);
    return 0;
}

int main() {
    if (int rc = testHier()) return rc;
    if (int rc = testCounter()) return rc;
    if (int rc = testChecks()) return rc;
    if (int rc = testSigned()) return rc;
    if (int rc = testDiv()) return rc;
    if (int rc = testCompound()) return rc;
    if (int rc = testBreak()) return rc;
    if (int rc = testTasks()) return rc;
    if (int rc = testRecur()) return rc;
    if (int rc = testHierarchy()) return rc;
    std::cout << "ok\n";
    return 0;
}
