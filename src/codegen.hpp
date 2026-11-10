// codegen.hpp - compiled mode. Turns lowered processes into one C++ file that
// links against the same kernel (minisim.cpp), in the style of VCS and
// Verilator: translate the design, then let a C++ compiler optimize it.
//
// The interpreter (exec.inc) walks a Prog with a switch per instruction and
// evaluates each expression by walking a shared_ptr tree through a
// std::function reader. Here every process becomes its own coroutine:
//
//   Instr pc          -> a label `pP_pc:` and straight-line C++
//   Jmp / JmpIfNot    -> goto
//   expression tree   -> nested calls to the inline v_* operators in expr.hpp
//   signal read       -> S.get(sig) inlined at the use
//   Call / Ret        -> a return-id stack in the coroutine and a switch, so
//                        recursion and a #delay inside a task work as in exec.inc
//
// Each task or function body reachable from a process is emitted inside that
// process's coroutine. Semantics are the interpreter's: same operators, same
// kernel, same scheduling regions; the only thing removed is dispatch.
//
// $display / $strobe / $monitor arguments stay expression trees, rebuilt once
// at startup, because the kernel prints them later (postponed region) by
// evaluating the tree. They are not on the hot path.
//
// An instruction or operator this file does not know throws
// CodegenUnsupported, and msim falls back to the interpreter.
#pragma once
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "prog.hpp"
#include "sva.hpp"

// msim -compile: generated source for several files is one string, each file
// after a line starting with this marker and naming it.
inline constexpr const char* kFileMark = "//@@ msim file: ";

struct CodegenUnsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// What the generated main() needs to rebuild the kernel's signal table in the
// same order, so signal ids in the code match.
struct CodegenSig {
    std::string name;
    int w = 1;
    bool net = false;
    int drivers = 0;
};

struct CodegenProc {
    const Prog* prog = nullptr;
    std::string kind;
    // >= 0: a combinational process (prog.hpp: combShape) at this rank of the
    // static schedule. It is emitted as a plain function and registered with
    // Sim::addComb in rank order instead of being spawned.
    int combRank = -1;
    std::vector<int> combIns;
    int combLevel = 0;
    // A clocked process (prog.hpp: seqShape): emitted as a function that
    // returns its NBAs, registered with Sim::addSeq on these events.
    bool seq = false;
    std::vector<std::pair<int, Edge>> seqEv;
};

// Arguments of Sim::buildPartitions (by rank; fixed NBA targets).
struct CodegenParts {
    bool on = false;
    std::vector<std::vector<int>> outs, ins;
    std::vector<int> targets;
};

class Codegen {
public:
    std::string run(const std::vector<CodegenSig>& sigs, const std::vector<CodegenProc>& procs,
                    const CodegenParts& parts = {}) {
        for (auto& g : sigs) { sigW_.push_back(g.w); sigNet_.push_back(g.net); }
        std::vector<std::string> chunks;   // one per process: the unit spread over files
        std::string cyc;                   // the cycle domain, kept with main()
        auto chunk = [&](auto&& emit) { std::ostringstream c; emit(c); chunks.push_back(c.str()); };
        std::vector<int> byRank;
        std::vector<int> dom = cycle && !cov ? cycleDomain(procs) : std::vector<int>{};
        {
            // The domain re-evaluates every static combinational statement on
            // each commit (and whenever any input changes). That pays when the
            // registers feed most of the logic every cycle (bench/: at most 6
            // statements per NBA), not for a CPU whose decoder and datapath far
            // outnumber the registers that change: Ibex has 1400 statements for
            // 62 NBAs, and ran 6x faster with only the changed ranks evaluated.
            size_t nba = 0, comb = 0;
            for (int i : dom)
                for (auto& in : procs[i].prog->code) nba += in.k == IK::Nba;
            for (auto& p : procs)
                if (p.combRank >= 0)
                    for (auto& in : p.prog->code) { if (in.k != IK::Assign) break; comb++; }
            if (comb > 64 && comb > 8 * nba) dom.clear();
        }
        for (auto& p : procs)   // the domain inlines straight-line comb statements only
            if (p.combRank >= 0 && combBranches(*p.prog)) dom.clear();
        if (!dom.empty() && privates) privateSigs(sigs, procs, dom);
        for (size_t i = 0; i < procs.size(); i++) {
            bool inDom = std::find(dom.begin(), dom.end(), (int)i) != dom.end();
            if (procs[i].combRank >= 0) {
                if (dom.empty()) chunk([&](std::ostream& c) { emitComb(c, (int)i, *procs[i].prog, procs[i].kind); });   // else: inlined in combAll
                if ((size_t)procs[i].combRank >= byRank.size()) byRank.resize(procs[i].combRank + 1, -1);
                byRank[procs[i].combRank] = (int)i;
            } else if (inDom)
                continue;   // emitCycle
            else if (procs[i].seq)
                chunk([&](std::ostream& c) { emitSeq(c, (int)i, *procs[i].prog, procs[i].kind); });
            else
                chunk([&](std::ostream& c) { emitProc(c, (int)i, *procs[i].prog, procs[i].kind); });
        }

        if (!dom.empty()) { std::ostringstream c; emitCycle(c, procs, dom, byRank); cyc = c.str(); }
        // Several files: the shared variables are inline (one object for
        // all files) and the process functions external.
        size_t chunkBytes = 0;
        for (auto& c : chunks) chunkBytes += c.size();
        // files < 0: exactly that many (msim -cfiles), else only for a large design.
        int nf = files < 0 ? (int)std::min<size_t>(-files, chunks.size()) : files > 1 ? (int)std::min<size_t>(files, chunkBytes / 65536) : 1;
        nf = std::max(nf, 1);
        const char* SV = nf > 1 ? "inline " : "static ";

        std::ostringstream o;
        o << "// Generated by msim -compile. Do not edit.\n"
             "#define MINISIM_NO_MAIN\n"
             "#include \"minisim.cpp\"\n"
             "#include <cstdlib>\n"
             "#include <cstring>\n"
             "#include <thread>\n\n"
             "#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n"
             "static inline Val R(int s, bool sg) { Val v = S.get(s); v.sgn = sg; return v; }\n"
             "static inline Val RS(Val v, int w, bool fill, bool sg) {\n"
             "    Val r = v_resize(v, w, fill && w > v.w); r.sgn = sg; return r;\n"
             "}\n"
             "struct PSig { uint64_t v, x, z; };\n"
             "static inline Val PR(const PSig& p, int w, bool sg) { return Val{p.v & ~p.x, p.x, w, p.z, sg}; }\n"
             "static inline void PW(PSig& p, int w, const Val& a) { uint64_t m = mask(w); p.v = a.v & m; p.x = a.x & m; p.z = a.z & m; }\n"
             "static inline Val cat1(Val r, Val p) { return v_concat({r, p}); }\n"
             "template <class... T> static inline Val CAT(T... p) { Val r{0, 0, 0}; ((r = cat1(r, p)), ...); return r; }\n"
             "static E X(Op op, Val k, int sig, int hi, int lo, bool sg, std::vector<E> kids) {\n"
             "    E e = node(op, std::move(kids)); e->k = k; e->sig = sig; e->hi = hi; e->lo = lo; e->sgn = sg; return e;\n"
             "}\n"
             "static E XF(E e, const char* f) { e->fmt = f; return e; }\n"
             "// Expression trees rebuilt at startup from text (Codegen::tree). As C++ calls\n"
             "// they made main() so large that building it took minutes.\n"
             "inline E* const* XSL = nullptr;            // assertion action slots SL<k>\n"
             "inline const ArrShape* const* XAS = nullptr;   // arrays AS<k>\n"
             "static E XP(const char*& p) {\n"
             "    while (*p == ' ') p++;\n"
             "    if (*p == 'N') { p++; return E{}; }\n"
             "    auto u = [&] { char* e; uint64_t v = std::strtoull(p, &e, 16); p = e; return v; };\n"
             "    auto i = [&] { char* e; long v = std::strtol(p, &e, 10); p = e; return (int)v; };\n"
             "    if (*p == 'S') { p++; return *XSL[i()]; }\n"
             "    Op op = Op(i()); Val k; k.v = u(); k.x = u(); k.w = i(); k.z = u(); k.sgn = i();\n"
             "    if (int nl = i()) { k.hi.resize(nl); for (int j = 0; j < nl; j++) { k.hi[j].v = u(); k.hi[j].x = u(); k.hi[j].z = u(); } }\n"
             "    int sig = i(), hi = i(), lo = i(); bool sg = i();\n"
             "    std::string f;\n"
             "    if (int fl = i()) { p++; f.assign(p, fl); p += fl; }\n"
             "    int ar = i(), nk = i();\n"
             "    std::vector<E> kids;\n"
             "    for (int j = 0; j < nk; j++) kids.push_back(XP(p));\n"
             "    E e = X(op, k, sig, hi, lo, sg, std::move(kids));\n"
             "    if (!f.empty()) e->fmt = f;\n"
             "    if (ar >= 0) e->arr = std::shared_ptr<const ArrShape>(std::shared_ptr<void>{}, XAS[ar]);\n"
             "    return e;\n"
             "}\n"
             "static E XT(const char* p) { return XP(p); }\n"
             "static E XA(E e, const ArrShape& a) { e->arr = std::shared_ptr<const ArrShape>(std::shared_ptr<void>{}, &a); return e; }\n"
             "// Unpacked arrays (eval() Op::ArrRead, target() in exec.inc): the element signal, or -1.\n"
             "static inline int AOFF(const ArrShape& a, std::initializer_list<Val> ix) {\n"
             "    int64_t off = 0; size_t d = 0;\n"
             "    for (const Val& v : ix) {\n"
             "        if (shiftUnknown(v)) return -1;\n"
             "        auto [l, r] = a.dims[d++];\n"
             "        int64_t n = (l > r ? l - r : r - l) + 1, i = v_to_i64(v), o = l <= r ? i - l : l - i;\n"
             "        if (o < 0 || o >= n) return -1;\n"
             "        off = off * n + o;\n"
             "    }\n"
             "    return a.base + (int)off;\n"
             "}\n"
             "static inline Val AREAD(const ArrShape& a, std::initializer_list<Val> ix) {\n"
             "    int s = AOFF(a, ix);\n"
             "    if (s < 0) return a.twoState ? make(0, 0, a.elemW, 0, a.sgn) : xfill(a.elemW, a.sgn);\n"
             "    Val r = S.get(s); r.sgn = a.sgn; return r;\n"
             "}\n"
             "static inline Val DSL(const Val& a, const Val& i, int w, bool down) {\n"
             "    if (shiftUnknown(i)) return allx(w);\n"
             "    int64_t s = v_to_i64(i);\n"
             "    return v_dynslice(a, down ? s - w + 1 : s, w);\n"
             "}\n"
             "static inline bool BIT(const Val& iv, int vlo, int pw, bool pdown, int w, int& hi, int& lo, Val& val) {\n"
             "    if (iv.x) return false;\n"
             "    int64_t b = v_to_i64(iv) - vlo;\n"
             "    if (!pw) { if (b < 0 || b >= w) return false; hi = lo = (int)b; return true; }\n"
             "    int64_t l = pdown ? b - pw + 1 : b, h = l + pw - 1;\n"
             "    int64_t cl = std::max<int64_t>(l, 0), ch = std::min<int64_t>(h, w - 1);\n"
             "    if (cl > ch) return false;\n"
             "    val = v_slice(val, (int)(ch - l), (int)(cl - l)); hi = (int)ch; lo = (int)cl; return true;\n"
             "}\n\n";
        // Trees rebuilt at startup, made now: they may name arrays (AS<k>).
        std::vector<std::string> sysTrees;
        for (auto& args : sysArgs_) {
            std::string t;
            for (size_t j = 0; j < args.size(); j++) t += (j ? ", " : "") + tree(args[j]);
            sysTrees.push_back(t);
        }
        std::vector<std::string> evTrees;
        for (auto& e : evals_) evTrees.push_back(tree(e));
        std::vector<std::string> memInit;
        for (size_t k = 0; k < mems_.size(); k++) {
            const Instr& i = *mems_[k];
            std::string t = "    MI" + std::to_string(k) + ".k = IK::" + (i.k == IK::ReadMem ? "ReadMem" : "WriteMem") +
                            "; MI" + std::to_string(k) + ".fmt = " + quote(i.fmt) + "; MI" + std::to_string(k) +
                            ".hex = " + (i.hex ? "true" : "false") + "; MI" + std::to_string(k) +
                            ".arr = std::shared_ptr<const ArrShape>(std::shared_ptr<void>{}, &" + arrName(i.arr.get()) + ");";
            std::string args;
            for (size_t j = 0; j < i.args.size(); j++) args += (j ? ", " : "") + tree(i.args[j]);
            t += " MI" + std::to_string(k) + ".args = {" + args + "};\n";
            memInit.push_back(t);
        }
        // The assertion table (sva.hpp), rebuilt at startup: what $assertcontrol
        // and the reports read, and the deferred actions as interpreted
        // processes whose captured arguments go into slots SL<k>.
        std::string asInit;
        int nSlots = 0, nAct = 0;
        if (!asserts().empty()) {
            asInit += "    asserts().resize(" + std::to_string(asserts().size()) + ");\n";
            auto action = [&](const Prog* p, const std::vector<E>& slots) -> std::string {
                if (!p) return "nullptr";
                std::string AP = "AP" + std::to_string(nAct++);
                for (auto& sl : slots) {
                    std::string n = "SL" + std::to_string(nSlots++);
                    asInit += "    " + n + " = " + tree(sl) + ";\n";
                    slotNames_[sl.get()] = n;
                }
                for (auto& i : p->code) {
                    if ((i.k != IK::Display && i.k != IK::Strobe) || i.lay || !i.init.empty())
                        throw CodegenUnsupported("an assertion action other than $display");
                    std::string args;
                    for (size_t j = 0; j < i.args.size(); j++) args += (j ? ", " : "") + tree(i.args[j]);
                    asInit += "    { Instr n; n.k = IK::" + std::string(i.k == IK::Display ? "Display" : "Strobe") + "; n.fmt = " +
                              quote(i.fmt) + "; n.nonl = " + (i.nonl ? "true" : "false") + "; n.args = {" + args + "}; " + AP +
                              ".code.push_back(n); }\n";
                }
                return "&" + AP;
            };
            auto list = [&](const std::vector<E>& v) {
                std::string t;
                for (size_t j = 0; j < v.size(); j++) t += (j ? ", " : "") + tree(v[j]);
                return "{" + t + "}";
            };
            for (size_t k = 0; k < asserts().size(); k++) {
                const AssertInfo& a = asserts()[k];
                std::string pass = action(a.pass, a.passSlot), fail = action(a.fail, a.failSlot);
                asInit += "    { auto& a = asserts()[" + std::to_string(k) + "]; a.name = " + quote(a.name) + "; a.inst = " +
                          quote(a.inst) + "; a.type = " + std::to_string(a.type) + "; a.dir = " + std::to_string(a.dir) +
                          "; a.hidden = " + (a.hidden ? "true" : "false") + "; a.pass = " + pass + "; a.fail = " + fail +
                          "; a.passCap = " + list(a.passCap) + "; a.failCap = " + list(a.failCap) + "; a.passSlot = " +
                          list(a.passSlot) + "; a.failSlot = " + list(a.failSlot) + "; }\n";
            }
        }
        for (size_t k = 0; k < asrts_.size(); k++) {
            const Instr& i = *asrts_[k];
            std::string A_ = "AI" + std::to_string(k);
            if (i.k == IK::AssertCtl) {
                std::string args;
                for (size_t j = 0; j < i.args.size(); j++) args += (j ? ", " : "") + tree(i.args[j]);
                asInit += "    " + A_ + ".k = IK::AssertCtl; " + A_ + ".fmt = " + quote(i.fmt) + "; " + A_ + ".args = {" + args + "};\n";
                continue;
            }
            asInit += "    " + A_ + ".k = IK::Assert; " + A_ + ".sig = " + std::to_string(i.sig) + "; " + A_ + ".join = " +
                      std::to_string(i.join) + "; " + A_ + ".target = 1; " + A_ + ".hi = 2; " + A_ + ".e = " + tree(i.e) + ";\n";
        }
        for (size_t k = 0; k < arrs_.size(); k++) {
            const ArrShape& a = *arrs_[k];
            o << SV << "const ArrShape AS" << k << "{" << a.base << ", " << a.count << ", " << a.elemW << ", "
              << (a.sgn ? "true" : "false") << ", " << (a.twoState ? "true" : "false") << ", {";
            for (size_t d = 0; d < a.dims.size(); d++)
                o << (d ? ", " : "") << "{" << a.dims[d].first << "LL, " << a.dims[d].second << "LL}";
            o << "}};\n";
        }
        for (size_t i = 0; i < sysArgs_.size(); i++) o << SV << "std::vector<E> A" << i << ";\n";
        for (size_t i = 0; i < evals_.size(); i++) o << SV << "E ET" << i << ";\n";
        for (size_t i = 0; i < mems_.size(); i++) o << SV << "Instr MI" << i << ";\n";
        for (size_t i = 0; i < asrts_.size(); i++) o << SV << "Instr AI" << i << ";\n";
        for (int i = 0; i < nAct; i++) o << SV << "Prog AP" << i << ";\n";
        for (int i = 0; i < nSlots; i++) o << SV << "E SL" << i << ";\n";
        if (nSlots) {
            o << SV << "E* const SLT[] = {";
            for (int i = 0; i < nSlots; i++) o << (i ? ", " : "") << "&SL" << i;
            o << "};\n";
        }
        if (!arrs_.empty()) {
            o << SV << "const ArrShape* const AST[] = {";
            for (size_t i = 0; i < arrs_.size(); i++) o << (i ? ", " : "") << "&AS" << i;
            o << "};\n";
        }
        for (size_t i = 0; i < events_.size(); i++) o << SV << "std::vector<std::pair<int, Edge>> EV" << i << ";\n";
        if (cov) o << (nf > 1 ? "inline " : "") << "uint64_t CV[" << std::max<size_t>(2, 2 * covSlots.size()) << "];\n";
        o << SV << "std::atomic<bool> pclean{false};   // every private signal is known (no X or Z bit)\n";
        // Private signals start as their S.sigs counterparts do: a variable X, a net Z.
        for (size_t s = 0; s < priv_.size(); s++)
            if (priv_[s]) {
                std::string m = M(sigW_[s]);
                o << SV << "PSig P" << s << "{0, " << m << ", " << (sigNet_[s] ? m : "0") << "};\n";
            }
        std::vector<std::string> outFiles;   // split: design.hpp, then each part (main() goes to design.cpp)
        if (nf > 1) {
            // Process functions become external, declared in the header.
            std::string protos;
            for (auto& c : chunks) {
                std::string out;
                for (size_t at = 0; at < c.size();) {
                    size_t nl = c.find('\n', at);
                    if (nl == std::string::npos) nl = c.size() - 1;
                    size_t h = c.compare(at, 7, "static ") == 0 ? c.find(") {", at) : std::string::npos;
                    if (h != std::string::npos && h < nl) {   // a function: "static <signature> {"
                        at += 7;
                        protos += c.substr(at, h + 1 - at) + ";\n";
                    }
                    out.append(c, at, nl + 1 - at);
                    at = nl + 1;
                }
                c = std::move(out);
            }
            outFiles.push_back(o.str() + "\n" + protos);
            // Largest first, each to the smallest file so far.
            std::vector<size_t> ord(chunks.size());
            for (size_t i = 0; i < ord.size(); i++) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) { return chunks[a].size() > chunks[b].size(); });
            std::vector<std::string> part(nf);
            std::vector<std::vector<size_t>> mine(nf);
            std::vector<size_t> load(nf, 0);
            for (size_t i : ord) {
                int f = (int)(std::min_element(load.begin(), load.end()) - load.begin());
                load[f] += chunks[i].size();
                mine[f].push_back(i);
            }
            for (int f = 0; f < nf; f++) {
                std::sort(mine[f].begin(), mine[f].end());   // process order within a file
                std::string t = "#include \"design.hpp\"\n\n";
                for (size_t i : mine[f]) t += chunks[i];
                outFiles.push_back(t);
            }
            o.str("");
            o << "#include \"design.hpp\"\n\n";
        } else
            for (auto& c : chunks) o << c;
        o << cyc;

        if (cov)
            o << "// Counters, then per signal its toggle masks (Sim::togRise / togFall), for msim.\n"
                 "static bool writeCov(const char* path) {\n"
                 "    FILE* f = std::fopen(path, \"wb\");\n"
                 "    if (!f) return false;\n"
                 "    uint64_t n = sizeof CV / sizeof CV[0], ns = S.sigs.size();\n"
                 "    std::fwrite(&n, 8, 1, f); std::fwrite(CV, 8, n, f); std::fwrite(&ns, 8, 1, f);\n"
                 "    for (size_t s = 0; s < ns; s++) {\n"
                 "        static const std::vector<uint64_t> none;\n"
                 "        const auto& R = s < S.togRise.size() ? S.togRise[s] : none;\n"
                 "        const auto& F = s < S.togFall.size() ? S.togFall[s] : none;\n"
                 "        uint64_t k = R.size();\n"
                 "        std::fwrite(&k, 8, 1, f); std::fwrite(R.data(), 8, k, f); std::fwrite(F.data(), 8, k, f);\n"
                 "    }\n"
                 "    { std::ofstream fc(std::string(path) + \".fcov\"); writeFuncCov(fc); }   // covergroups, assertions\n"
                 "    return std::fclose(f) == 0;\n"
                 "}\n\n";
        o << "int main(int argc, char** argv) {\n"
             "    uint64_t maxTime = ~0ULL;\n"
             "    const char* covOut = nullptr;\n"
             "    S.vcdPath = \"\";\n"
             "    S.threads = defaultThreads();\n"
             "    for (int i = 1; i < argc; i++) {\n"
             "        if (!std::strcmp(argv[i], \"-maxtime\") && i + 1 < argc) maxTime = std::strtoull(argv[++i], nullptr, 10);\n"
             "        else if (!std::strcmp(argv[i], \"-vcd\") && i + 1 < argc) S.vcdPath = argv[++i];\n"
             "        else if (!std::strcmp(argv[i], \"-vcdtop\") && i + 1 < argc) S.vcdTop = argv[++i];\n"
             "        else if (!std::strcmp(argv[i], \"-threads\") && i + 1 < argc) {\n"
             "            ++i;\n"
             "            S.threads = !std::strcmp(argv[i], \"all\") ? maxThreads() : std::max(1, std::atoi(argv[i]));\n"
             "        }\n"
             "        else if (!std::strcmp(argv[i], \"-no-pin\")) g_pin = 0;\n"
             "        else if (!std::strcmp(argv[i], \"-pin\")) g_pin = 1;\n"
             "        else if (!std::strcmp(argv[i], \"-cov\") && i + 1 < argc) { g_cov = true; covOut = argv[++i]; }\n"
             "        else if (!std::strcmp(argv[i], \"-seed\") && i + 1 < argc) {   // as msim -seed\n"
             "            uint64_t st = (std::strtoull(argv[++i], nullptr, 10) + 1) * 0x9E3779B97F4A7C15ull;\n"
             "            urandomState() = st ? st : 1;\n"
             "        }\n"
             "        else if (argv[i][0] == '+') plusArgs().push_back(argv[i] + 1);\n"
             "    }\n"
             "    S.threads = std::min(S.threads, maxThreads());\n";
        // Startup is data plus loops, not one statement per item: a main() of
        // tens of thousands of statements took the C++ compiler minutes.
        if (nSlots) o << "    XSL = SLT;\n";
        if (!arrs_.empty()) o << "    XAS = AST;\n";
        if (!sigs.empty()) {
            o << "    static const struct { const char* n; int w; bool net; int drv; } SIGS[] = {\n";
            for (size_t s = 0; s < sigs.size(); s++)
                o << "        {" << quote(sigs[s].name) << ", " << sigs[s].w << ", " << (sigs[s].net ? "true" : "false") << ", "
                  << sigs[s].drivers << "},\n";
            o << "    };\n"
                 "    for (int s = 0; s < (int)(sizeof SIGS / sizeof SIGS[0]); s++) {\n"
                 "        S.sig(SIGS[s].n, SIGS[s].w, SIGS[s].net);\n"
                 "        for (int d = 0; d < SIGS[s].drv; d++) S.newDriver(s);\n"
                 "    }\n";
        }
        for (size_t i = 0; i < sysArgs_.size(); i++) o << "    A" << i << " = {" << sysTrees[i] << "};\n";
        for (size_t i = 0; i < evals_.size(); i++) o << "    ET" << i << " = " << evTrees[i] << ";\n";
        for (auto& m : memInit) o << m;
        o << asInit;
        for (size_t i = 0; i < events_.size(); i++) {
            o << "    EV" << i << " = {";
            for (size_t j = 0; j < events_[i].size(); j++)
                o << (j ? ", " : "") << "{" << events_[i][j].first << ", " << edge(events_[i][j].second) << "}";
            o << "};\n";
        }
        for (int i : byRank)
            if (i < 0) throw CodegenUnsupported("gap in the combinational ranks");
        if (!dom.empty() && !byRank.empty()) {
            std::vector<int> ins;
            for (int i : byRank) ins.insert(ins.end(), procs[i].combIns.begin(), procs[i].combIns.end());
            std::sort(ins.begin(), ins.end());
            ins.erase(std::unique(ins.begin(), ins.end()), ins.end());
            o << "    S.fastComb = (int)S.combs.size();\n    S.addComb(combAll, nullptr, {";
            for (size_t j = 0; j < ins.size(); j++) o << (j ? ", " : "") << ins[j];
            o << "});\n";
        }
        if (dom.empty() && !byRank.empty()) {   // the ranks: functions, wait lists in CINS
            std::string ins, tab;
            size_t off = 0;
            for (int i : byRank) {
                for (int x : procs[i].combIns) ins += std::to_string(x) + ",";
                bool one = oneAssign(*procs[i].prog);
                tab += "        {comb" + std::to_string(i) + ", " + (one ? "combv" + std::to_string(i) : "nullptr") + ", " +
                       (one ? "combc" + std::to_string(i) : "nullptr") + ", " + std::to_string(procs[i].combLevel) + ", " +
                       std::to_string(off) + ", " + std::to_string(procs[i].combIns.size()) + "},\n";
                off += procs[i].combIns.size();
            }
            o << "    static const int CINS[] = {" << ins << "0};\n"
                 "    static const struct { void (*f)(const void*); Val (*v)(const void*); void (*c)(const void*, Val); int lv, at, n; } COMBS[] = {\n"
              << tab << "    };\n"
                 "    for (auto& c : COMBS) S.addComb(c.f, nullptr, std::vector<int>(CINS + c.at, CINS + c.at + c.n), c.lv, c.v, c.c);\n";
        }
        if (!dom.empty()) {
            o << "    S.fastSeqId = (int)S.seqs.size();\n    S.addSeq(dseq, nullptr, {";
            const auto& ev = procs[dom[0]].seqEv;
            for (size_t j = 0; j < ev.size(); j++)
                o << (j ? ", " : "") << "{" << ev[j].first << ", " << edge(ev[j].second) << "}";
            o << "});\n    S.fastCommit = fcommit;\n";
        }
        {   // clocked functions and coroutine processes, in process order
            std::string evs, tab;
            size_t off = 0, n = 0;
            for (size_t i = 0; i < procs.size(); i++) {
                if (procs[i].combRank >= 0) continue;
                if (std::find(dom.begin(), dom.end(), (int)i) != dom.end()) continue;
                n++;
                if (procs[i].seq) {
                    for (auto& [sg, ed] : procs[i].seqEv) evs += "{" + std::to_string(sg) + ", " + edge(ed) + "},";
                    tab += "        {seq" + std::to_string(i) + ", nullptr, " + std::to_string(off) + ", " +
                           std::to_string(procs[i].seqEv.size()) + "},\n";
                    off += procs[i].seqEv.size();
                } else
                    tab += "        {nullptr, proc" + std::to_string(i) + ", 0, 0},\n";
            }
            if (n) {
                o << "    static const std::pair<int, Edge> SEV[] = {" << evs << "{0, ANY}};\n"
                     "    static const struct { void (*s)(const void*, std::vector<Sim::NbaItem>&); Task (*p)(); int at, n; } PROCS[] = {\n"
                  << tab << "    };\n"
                     "    for (auto& p : PROCS)\n"
                     "        if (p.s) S.addSeq(p.s, nullptr, std::vector<std::pair<int, Edge>>(SEV + p.at, SEV + p.at + p.n));\n"
                     "        else S.spawn(p.p());\n";
            }
        }
        if (!dom.empty()) o << "    S.fastInit();\n";
        if (parts.on && dom.empty()) {
            auto list = [&](const std::vector<int>& v) {
                std::string r = "{";
                for (size_t j = 0; j < v.size(); j++) r += (j ? ", " : "") + std::to_string(v[j]);
                return r + "}";
            };
            o << "    S.buildPartitions({";
            for (size_t r = 0; r < parts.outs.size(); r++) o << (r ? ", " : "") << list(parts.outs[r]);
            o << "}, {";
            for (size_t r = 0; r < parts.ins.size(); r++) o << (r ? ", " : "") << list(parts.ins[r]);
            o << "}, " << list(parts.targets) << ");\n";
        }
        o << "    S.run(maxTime);\n"
             "    std::cout.flush();\n";
        if (cov) o << "    if (covOut && !writeCov(covOut)) { std::cerr << \"simv: cannot write \" << covOut << \"\\n\"; return 1; }\n";
        o << "    return 0;\n"
             "}\n";
        if (outFiles.empty()) return o.str();
        // Files after one another, each after a marker line msim splits on.
        std::string all = std::string(kFileMark) + "design.hpp\n" + outFiles[0];
        all += std::string(kFileMark) + "design.cpp\n" + o.str();
        for (size_t f = 1; f < outFiles.size(); f++) all += std::string(kFileMark) + "part" + std::to_string(f - 1) + ".cpp\n" + outFiles[f];
        return all;
    }

public:
    bool twoState = true;  // emit the 2-state fast path (see fast())
    bool cycle = true;     // emit the cycle domain (see cycleDomain())
    bool privates = true;  // ...with its private signals in static variables (no VCD planned)
    // msim -compile -cov: count executions in CV[2k] and taken jumps in
    // CV[2k+1] for instruction covSlots[k], as exec.inc's covHit does. simv
    // writes the counters and toggle masks to its -cov file and msim turns
    // them into the coverage database. The cycle domain is off with it.
    bool cov = false;
    // Build in up to this many C++ files (in parallel): a large design as one
    // file took minutes. Small designs stay one file. Negative: -files files
    // whatever the size (msim -cfiles).
    int files = 1;
    std::vector<std::pair<const Prog*, size_t>> covSlots;
    std::map<std::pair<const Prog*, size_t>, size_t> covIdx_;
    size_t covSlot(const Prog* p, size_t pc) {
        auto [it, fresh] = covIdx_.try_emplace({p, pc}, covSlots.size());
        if (fresh) covSlots.push_back({p, pc});
        return it->second;
    }
    std::string CH(const Prog* p, size_t pc) {   // instruction pc of p runs
        return cov ? " ++CV[" + std::to_string(2 * covSlot(p, pc)) + "];" : std::string();
    }
    std::string CT(const Prog* p, size_t pc) {   // ...and its JmpIfNot jumps
        return cov ? " ++CV[" + std::to_string(2 * covSlot(p, pc) + 1) + "];" : std::string();
    }
    std::string jumpTo(const Prog* p, size_t pc, const std::string& label) {
        return cov ? "{" + CT(p, pc) + " goto " + label + "; }" : "goto " + label + ";";
    }

private:
    std::vector<int> sigW_;
    // Private signals of the cycle domain (see privateSigs): kept in plain
    // static variables P<s> instead of S.sigs, like Verilator's model members.
    std::vector<char> priv_, sigNet_, clock_;
    // A narrow variable no clocked process waits on: Sim::fastInit marks it
    // quiet, so a write is a store unless a process waits on it (or a VCD
    // or $monitor watches everything). `obs` is S.observed, read once per function.
    std::string regWrite(int s, const std::string& u) const {
        std::string S_ = std::to_string(s), m = M(sigW_[s]);
        if (sigW_[s] > 64 || sigNet_[s] || clock_[s]) return "S.fwv(" + S_ + ", " + u + ");";
        return "{ if (__builtin_expect(!obs && S.waiters[" + S_ + "].empty(), 1)) { Val& q = S.sigs[" + S_ + "].val; q.v = (" +
               u + ") & " + m + "; q.x = 0; q.z = 0; q.real = false; q.str = false; } else S.fwSlow(" + S_ + ", make((" + u +
               ") & " + m + ", 0, " + std::to_string(sigW_[s]) + ")); }";
    }
    bool isPriv(int s) const { return s >= 0 && s < (int)priv_.size() && priv_[s]; }
    std::string vRef(int s) const { return isPriv(s) ? "P" + std::to_string(s) + ".v" : "S.sigs[" + std::to_string(s) + "].val.v"; }
    std::string xRef(int s) const { return isPriv(s) ? "P" + std::to_string(s) + ".x" : "S.sigs[" + std::to_string(s) + "].val.x"; }
    std::vector<std::vector<E>> sysArgs_;
    std::vector<std::vector<std::pair<int, Edge>>> events_;
    std::vector<const ArrShape*> arrs_;   // unpacked arrays, emitted as AS<k>
    std::vector<E> evals_;                // operators without C++ yet: interpreted (ET<k>)
    std::vector<const Instr*> mems_;      // $readmem / $writemem instructions, rebuilt as MI<k>
    std::vector<const Instr*> asrts_;     // immediate assertions and $assertcontrol, rebuilt as AI<k>
    std::map<const Expr*, std::string> slotNames_;   // assertion action slots, rebuilt as SL<k>
    bool readsPriv(const E& e) const {
        if (!e) return false;
        if (e->op == Op::Ref && isPriv(e->sig)) return true;
        for (auto& k : e->kids) if (readsPriv(k)) return true;
        return false;
    }
    std::string arrName(const ArrShape* a) {
        for (size_t k = 0; k < arrs_.size(); k++)
            if (arrs_[k] == a || (arrs_[k]->base == a->base && arrs_[k]->count == a->count && arrs_[k]->dims == a->dims &&
                                  arrs_[k]->elemW == a->elemW && arrs_[k]->sgn == a->sgn && arrs_[k]->twoState == a->twoState))
                return "AS" + std::to_string(k);
        arrs_.push_back(a);
        return "AS" + std::to_string(arrs_.size() - 1);
    }
    // An element write chosen at run time (a[i] = v, v[i] = v, v[i +: w] = v):
    // the code that sets s, hi, lo and v, then runs `then` unless the index is
    // invalid (7.4.6, 11.5.1). Mirrors target() in exec.inc.
    std::string dynTarget(const Instr& i, const std::string& val, const std::string& then) {
        if (isPriv(i.sig)) throw CodegenUnsupported("variable-index write to a private signal");
        std::ostringstream o;
        o << " { Val v = " << val << "; int s = " << i.sig << ", hi = " << i.hi << ", lo = " << i.lo << ";";
        if (i.arr) {
            o << " s = AOFF(" << arrName(i.arr.get()) << ", {";
            for (size_t k = 0; k < i.aidx.size(); k++) o << (k ? ", " : "") << fast(i.aidx[k]);
            o << "}); if (s >= 0)";
        }
        if (i.bidx)
            o << " if (BIT(" << fast(i.bidx) << ", " << i.lo << ", " << i.pw << ", " << (i.pdown ? "true" : "false")
              << ", S.sigs[s].w, hi, lo, v))";
        o << " {" << then << "} }\n";
        return o.str();
    }

    static std::string quote(const std::string& s) {
        std::string r = "\"";
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') { r += '\\'; r += (char)c; }
            else if (c == '\n') r += "\\n";
            else if (c == '\t') r += "\\t";
            else if (c < 0x20 || c >= 0x7f) {
                char b[8];
                std::snprintf(b, sizeof b, "\\%03o", c);
                r += b;
            } else r += (char)c;
        }
        return r + "\"";
    }
    static std::string hex(uint64_t v) {
        char b[24];
        std::snprintf(b, sizeof b, "0x%llxULL", (unsigned long long)v);
        return b;
    }
    static std::string val(const Val& k) {
        if (k.real || k.str) throw CodegenUnsupported("real or string constant");
        std::string base = "Val{" + hex(k.v) + ", " + hex(k.x) + ", " + std::to_string(k.w) + ", " + hex(k.z) +
                           ", " + (k.sgn ? "true" : "false") + "}";
        if (k.hi.empty()) return base;
        std::string s = "([&]{ Val v = " + base + "; v.hi = {";
        for (size_t i = 0; i < k.hi.size(); i++) {
            if (i) s += ", ";
            s += "Limb{" + hex(k.hi[i].v) + ", " + hex(k.hi[i].x) + ", " + hex(k.hi[i].z) + "}";
        }
        return s + "}; return v; }())";
    }
    static const char* edge(Edge e) { return e == POS ? "POS" : e == NEG ? "NEG" : "ANY"; }

    // The tree itself, rebuilt at startup (for the printing system tasks).
    std::string tree(const E& e) {
        if (!e) return "E{}";
        std::string t;
        treeText(e, t);
        return "XT(" + quote(t) + ")";
    }
    // The text XP() reads back: per node op, value (v x w z sgn, limbs), sig,
    // hi, lo, sgn, format (length, then the bytes), array, kids.
    void treeText(const E& e, std::string& t) {
        if (!t.empty()) t += ' ';
        if (!e) { t += 'N'; return; }
        if (auto it = slotNames_.find(e.get()); it != slotNames_.end()) { t += "S" + it->second.substr(2); return; }
        bool ar = e->op == Op::ArrRead;   // only its kids and array count
        const Val k = ar ? Val{} : e->k;
        if (k.real || k.str) throw CodegenUnsupported("real or string constant");
        char b[96];
        std::snprintf(b, sizeof b, "%d %llx %llx %d %llx %d %zu", (int)e->op, (unsigned long long)k.v, (unsigned long long)k.x,
                      k.w, (unsigned long long)k.z, (int)k.sgn, k.hi.size());
        t += b;
        for (size_t i = 0; i < k.hi.size(); i++) {
            std::snprintf(b, sizeof b, " %llx %llx %llx", (unsigned long long)k.hi[i].v, (unsigned long long)k.hi[i].x,
                          (unsigned long long)k.hi[i].z);
            t += b;
        }
        if (ar) t += " -1 0 0 0";
        else t += " " + std::to_string(e->sig) + " " + std::to_string(e->hi) + " " + std::to_string(e->lo) + " " + (e->sgn ? "1" : "0");
        t += " " + std::to_string(e->fmt.size());
        if (!e->fmt.empty()) t += " " + e->fmt;
        t += " " + (ar ? arrName(e->arr.get()).substr(2) : std::string("-1")) + " " + std::to_string(e->kids.size());
        for (auto& kid : e->kids) treeText(kid, t);
    }

    // The expression as C++. Mirrors eval() in expr.hpp case by case.
    std::string ex(const E& e) {
        if (!e) throw CodegenUnsupported("empty expression");
        auto k = [&](int i) { return ex(e->kids.at(i)); };
        auto f1 = [&](const char* fn) { return std::string(fn) + "(" + k(0) + ")"; };
        auto f2 = [&](const char* fn) { return std::string(fn) + "(" + k(0) + ", " + k(1) + ")"; };
        switch (e->op) {
            case Op::Const: return val(e->k);
            case Op::Ref:
                if (isPriv(e->sig))
                    return "PR(P" + std::to_string(e->sig) + ", " + std::to_string(sigW_[e->sig]) + ", " + (e->sgn ? "true" : "false") + ")";
                return "R(" + std::to_string(e->sig) + ", " + (e->sgn ? "true" : "false") + ")";
            case Op::DynSlice:   // a[i +: w] / a[i -: w] read (11.5.1)
                return "DSL(" + k(0) + ", " + k(1) + ", " + std::to_string(e->hi) + ", " + (e->lo ? "true" : "false") + ")";
            case Op::ArrRead: {
                std::string ix;
                for (size_t i = 0; i < e->kids.size(); i++) ix += (i ? ", " : "") + k((int)i);
                return "AREAD(" + arrName(e->arr.get()) + ", {" + ix + "})";
            }
            case Op::Not: return f1("v_not");
            case Op::And: return f2("v_and");
            case Op::Or: return f2("v_or");
            case Op::Xor: return f2("v_xor");
            case Op::Add: return f2("v_add");
            case Op::Sub: return f2("v_sub");
            case Op::Mul: return f2("v_mul");
            case Op::Div: return f2("v_div");
            case Op::Mod: return f2("v_mod");
            case Op::Eq: return f2("v_eq");
            case Op::Ne: return f2("v_ne");
            case Op::CaseEq: return f2("v_case_eq");
            case Op::CaseZEq: return f2("v_casez_eq");
            case Op::CaseXEq: return f2("v_casex_eq");
            case Op::Lt: return f2("v_lt");
            case Op::Le: return f2("v_le");
            case Op::Gt: return f2("v_gt");
            case Op::Ge: return f2("v_ge");
            case Op::Shl: return f2("v_shl");
            case Op::Shr: return f2("v_shr");
            case Op::Asr: return f2("v_asr");
            case Op::Resize:
                return "RS(" + k(0) + ", " + std::to_string(e->hi) + ", " + (e->lo ? "true" : "false") + ", " +
                       (e->sgn ? "true" : "false") + ")";
            case Op::LogNot: return f1("v_lnot");
            case Op::LogAnd: return f2("v_land");
            case Op::LogOr: return f2("v_lor");
            case Op::RedAnd: return f1("v_red_and");
            case Op::RedOr: return f1("v_red_or");
            case Op::RedXor: return f1("v_red_xor");
            case Op::Slice: return "v_slice(" + k(0) + ", " + std::to_string(e->hi) + ", " + std::to_string(e->lo) + ")";
            case Op::Index: return f2("v_index");
            case Op::Concat: {
                if (e->kids.empty()) return "Val{0, 0, 0}";
                std::string r = "CAT(";
                for (size_t i = 0; i < e->kids.size(); i++) r += (i ? ", " : "") + ex(e->kids[i]);
                return r + ")";
            }
            case Op::Mux: return "v_mux(" + k(0) + ", " + k(1) + ", " + k(2) + ")";
            default: break;
        }
        // Any other operator (==?, real math, strings, system functions, ...):
        // the interpreter's eval() on the subtree, rebuilt at startup.
        if (readsPriv(e)) throw CodegenUnsupported("operator " + std::to_string((int)e->op) + " on a private signal");
        val(e->k);   // throws for a real or string constant
        evals_.push_back(e);
        return "eval(ET" + std::to_string(evals_.size() - 1) + ", rd)";
    }

    // ---- 2-state fast path ----
    // Most of the time no signal holds X or Z. For an expression made of
    // operators whose 0/1 result is plain integer arithmetic, emit
    //     (every signal it reads is known) ? Val{uint64 arithmetic} : <4-state code>
    // The integer form mirrors expr.hpp operator by operator (widths, sign
    // extension, masks), so both arms give the same bits. Operators that can
    // make X from known inputs (/, %, a[i] out of range, **) or that have no
    // fixed width keep the 4-state code.
    struct FastInfo { int w; bool sgn; };
    static std::string M(int w) { return w >= 64 ? "~0ULL" : hex((1ULL << w) - 1); }
    // Sign-extend a w-bit value to 64 bits (as a uint64_t expression).
    static std::string sx(const std::string& a, int w) {
        if (w >= 64) return a;
        return "(uint64_t)((int64_t)((" + a + ") << " + std::to_string(64 - w) + ") >> " + std::to_string(64 - w) + ")";
    }
    // Extend a (wa bits) to w bits: sign-fill when `s`, else it is already zero-extended.
    static std::string ext(const std::string& a, int wa, int w, bool s) {
        if (!s || wa >= w) return a;
        return "(" + sx(a, wa) + " & " + M(w) + ")";
    }
    bool fx(const E& e, std::string& out, FastInfo& fi, std::vector<int>& refs) {
        if (!e) return false;
        auto kid = [&](int i, std::string& o, FastInfo& f) { return i < (int)e->kids.size() && fx(e->kids[i], o, f, refs); };
        std::string A, B, C;
        FastInfo fa{}, fb{}, fc{};
        switch (e->op) {
            case Op::Const:
                if (e->k.x || e->k.w > 64 || e->k.w < 1 || e->k.real || e->k.str) return false;
                out = hex(e->k.v);
                fi = {e->k.w, e->k.sgn};
                return true;
            case Op::Ref:
                if (e->sig < 0 || e->sig >= (int)sigW_.size() || sigW_[e->sig] > 64 || sigW_[e->sig] < 1) return false;
                refs.push_back(e->sig);
                out = vRef(e->sig);
                fi = {sigW_[e->sig], e->sgn};
                return true;
            case Op::Not:
                if (!kid(0, A, fa)) return false;
                out = "(~" + A + " & " + M(fa.w) + ")";
                fi = fa;
                return true;
            case Op::And: case Op::Or: case Op::Xor: {
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                const char* o = e->op == Op::And ? " & " : e->op == Op::Or ? " | " : " ^ ";
                out = "(" + A + o + B + ")";
                fi = {std::max(fa.w, fb.w), fa.sgn && fb.sgn};
                return true;
            }
            case Op::Add: case Op::Sub: case Op::Mul: {
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                int w = std::max(fa.w, fb.w);
                bool sg = fa.sgn && fb.sgn;
                const char* o = e->op == Op::Add ? " + " : e->op == Op::Sub ? " - " : " * ";
                out = "((" + ext(A, fa.w, w, sg) + o + ext(B, fb.w, w, sg) + ") & " + M(w) + ")";
                fi = {w, sg};
                return true;
            }
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                int w = std::max(fa.w, fb.w);
                bool sg = fa.sgn && fb.sgn;
                std::string a = ext(A, fa.w, w, sg), b = ext(B, fb.w, w, sg);
                if (sg && e->op != Op::Eq && e->op != Op::Ne) { a = "(int64_t)" + sx(a, w); b = "(int64_t)" + sx(b, w); }
                const char* o = e->op == Op::Eq ? " == " : e->op == Op::Ne ? " != " : e->op == Op::Lt ? " < "
                              : e->op == Op::Le ? " <= " : e->op == Op::Gt ? " > " : " >= ";
                out = "(uint64_t)(" + a + o + b + ")";
                fi = {1, false};
                return true;
            }
            case Op::CaseEq: case Op::CaseZEq: case Op::CaseXEq:
                // No X or Z: every bit is cared about and compared as is (no extension).
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                out = "(uint64_t)(" + A + " == " + B + ")";
                fi = {1, false};
                return true;
            case Op::Shl: case Op::Shr: {
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                std::string n = B;
                if (e->op == Op::Shl) out = "(" + n + " >= 64 ? 0ULL : ((" + A + " << " + n + ") & " + M(fa.w) + "))";
                else out = "(" + n + " >= 64 ? 0ULL : (" + A + " >> " + n + "))";
                fi = fa;
                return true;
            }
            case Op::Asr: {
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                if (!fa.sgn) out = "(" + B + " >= 64 ? 0ULL : (" + A + " >> " + B + "))";
                else
                    out = "(" + B + " >= " + std::to_string(fa.w) + " ? (" + sx(A, fa.w) + " >> 63 ? " + M(fa.w) +
                          " : 0ULL) : ((uint64_t)((int64_t)" + sx(A, fa.w) + " >> " + B + ") & " + M(fa.w) + "))";
                fi = fa;
                return true;
            }
            case Op::Resize: {
                if (!kid(0, A, fa)) return false;
                int w = e->hi;
                if (w < 1 || w > 64) return false;
                bool fill = e->lo && w > fa.w;
                out = "(" + (fill ? sx(A, fa.w) : A) + " & " + M(w) + ")";
                fi = {w, e->sgn};
                return true;
            }
            case Op::LogNot:
                if (!kid(0, A, fa)) return false;
                out = "(uint64_t)(" + A + " == 0)";
                fi = {1, false};
                return true;
            case Op::LogAnd: case Op::LogOr:
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                out = "(uint64_t)((" + A + " != 0)" + (e->op == Op::LogAnd ? " & " : " | ") + "(" + B + " != 0))";
                fi = {1, false};
                return true;
            case Op::RedAnd:
                if (!kid(0, A, fa)) return false;
                out = "(uint64_t)(" + A + " == " + M(fa.w) + ")";
                fi = {1, false};
                return true;
            case Op::RedOr:
                if (!kid(0, A, fa)) return false;
                out = "(uint64_t)(" + A + " != 0)";
                fi = {1, false};
                return true;
            case Op::RedXor:
                if (!kid(0, A, fa)) return false;
                out = "(uint64_t)__builtin_parityll(" + A + ")";
                fi = {1, false};
                return true;
            case Op::Slice: {
                if (!kid(0, A, fa)) return false;
                int w = e->hi - e->lo + 1;
                if (e->lo < 0 || e->lo >= 64 || w < 1 || w > 64) return false;
                out = "((" + A + " >> " + std::to_string(e->lo) + ") & " + M(w) + ")";
                fi = {w, false};
                return true;
            }
            case Op::Concat: {
                if (e->kids.empty()) return false;
                std::string r;
                int w = 0;
                for (size_t i = 0; i < e->kids.size(); i++) {
                    std::string P;
                    FastInfo fp{};
                    if (!fx(e->kids[i], P, fp, refs)) return false;
                    w += fp.w;
                    if (w > 64) return false;
                    r = i ? "((" + r + " << " + std::to_string(fp.w) + ") | " + P + ")" : P;
                }
                out = r;
                fi = {w, false};
                return true;
            }
            case Op::Mux:
                if (!kid(0, C, fc) || !kid(1, A, fa) || !kid(2, B, fb)) return false;
                // v_mux returns the chosen arm as is: its own width and signedness.
                if (fa.w != fb.w || fa.sgn != fb.sgn) return false;
                out = "(" + C + " != 0 ? " + A + " : " + B + ")";
                fi = fa;
                return true;
            default:
                return false;
        }
    }
    // The expression as a Val, with the 2-state fast path when it applies.
    // The 2-state path in parts: `guard` true (or empty: always) means `u64`
    // is the value, else `slow`. False when there is no 2-state path.
    bool split(const E& e, std::string& guard, std::string& u64, std::string& slow) {
        slow = ex(e);
        if (!twoState) return false;
        FastInfo fi{};
        std::vector<int> refs;
        if (!fx(e, u64, fi, refs)) return false;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        guard = g.empty() ? "" : "(" + g + ") == 0";
        return true;
    }
    std::string fast(const E& e) {
        std::string slow = ex(e);
        if (!twoState) return slow;
        std::string f;
        FastInfo fi{};
        std::vector<int> refs;
        if (!fx(e, f, fi, refs)) return slow;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        std::string val = "Val{" + f + ", 0, " + std::to_string(fi.w) + ", 0, " + (fi.sgn ? "true" : "false") + "}";
        if (g.empty()) return val;
        return "((" + g + ") == 0 ? " + val + " : " + slow + ")";
    }

    // The write of an Assign, with `val` as the value (default: its expression).
    std::string assign(const Instr& i, std::string val = "") {
        if (val.empty()) val = fast(i.e);
        std::ostringstream o;
        if (isPriv(i.sig)) {   // a testbench write to a private reg: the comb logic may read it
            if (i.drv >= 0 || i.hi >= 0) throw CodegenUnsupported("partial or driven write to a private signal");
            o << " " << pw(i.sig, val) << " pclean = false; S.markFast();\n";
            return o.str();
        }
        if (i.drv >= 0 && i.hi >= 0)  // a driver of part of a net drives Z on the other bits
            o << " S.drive(" << i.sig << ", " << i.drv << ", v_insert(allz(S.sigs[" << i.sig << "].w), " << val
              << ", " << i.hi << ", " << i.lo << "), " << (int)i.s0 << ", " << (int)i.s1 << ");\n";
        else if (i.drv >= 0)
            o << " S.drive(" << i.sig << ", " << i.drv << ", " << val << ", " << (int)i.s0 << ", " << (int)i.s1
              << ");\n";
        else if (i.hi >= 0)
            o << " S.set(" << i.sig << ", v_insert(S.get(" << i.sig << "), " << val << ", " << i.hi << ", " << i.lo
              << "));\n";
        else
            o << " S.set(" << i.sig << ", " << val << ");\n";
        return o.str();
    }
    static bool oneAssign(const Prog& p) {
        return p.code.size() >= 2 && p.code[0].k == IK::Assign && p.code[1].k == IK::WaitEdge;
    }
    // A static combinational process with if / case (prog.hpp: combShape).
    static bool combBranches(const Prog& p) {
        for (auto& i : p.code) {
            if (i.k == IK::WaitEdge) return false;
            if (i.k == IK::JmpIfNot || i.k == IK::Jmp) return true;
        }
        return false;
    }

    // The cycle domain: the largest group of clocked processes that wait on
    // the same events, if no other process makes an NBA to one of their NBA
    // targets (a blocking write, as a testbench's reset, is fine: it is not
    // ordered against the NBA queue) and every jump in them goes forward (so each NBA runs at most
    // once and can own a fixed slot). Empty: no domain.
    static std::vector<int> cycleDomain(const std::vector<CodegenProc>& procs) {
        std::vector<std::vector<int>> groups;
        for (size_t i = 0; i < procs.size(); i++) {
            if (!procs[i].seq || procs[i].combRank >= 0) continue;
            bool forward = true;
            const auto& code = procs[i].prog->code;
            for (size_t pc = 1; pc + 1 < code.size(); pc++) {
                if ((code[pc].k == IK::Jmp || code[pc].k == IK::JmpIfNot) && code[pc].target <= pc) forward = false;
                if (code[pc].arr || code[pc].bidx) forward = false;   // no fixed slot: an ordinary clocked function
            }
            if (!forward) continue;
            bool placed = false;
            for (auto& g : groups)
                if (procs[g[0]].seqEv == procs[i].seqEv) { g.push_back((int)i); placed = true; break; }
            if (!placed) groups.push_back({(int)i});
        }
        std::vector<int> best;
        for (auto& g : groups) if (g.size() > best.size()) best = g;
        if (best.empty()) return best;
        std::vector<char> mine(procs.size(), 0);
        for (int i : best) mine[i] = 1;
        std::vector<int> targets;
        for (int i : best)
            for (auto& in : procs[i].prog->code) if (in.k == IK::Nba) targets.push_back(in.sig);
        for (size_t i = 0; i < procs.size(); i++) {
            if (mine[i]) continue;
            std::vector<const Prog*> progs;
            reach(procs[i].prog, progs);
            for (const Prog* p : progs)
                for (auto& in : p->code)
                    if (in.k == IK::Nba &&
                        std::find(targets.begin(), targets.end(), in.sig) != targets.end())
                        return {};
        }
        return best;
    }

    // A signal is private to the cycle domain when only the domain uses it
    // in ways that need S.sigs: a full-width NBA target of a domain process or the output of a
    // single-driver continuous assign in the static comb schedule, 64 bits or
    // less, not a clock, not an array element, and not named by any other
    // process (no read, write, wait, $display or $monitor of it). Nothing
    // outside can then see where its value is kept.
    void privateSigs(const std::vector<CodegenSig>& sigs, const std::vector<CodegenProc>& procs,
                     const std::vector<int>& dom) {
        size_t ns = sigs.size();
        bool anyArr = false;
        std::vector<char> cand(ns, 0), bad(ns, 0);
        std::vector<char> mine(procs.size(), 0);
        for (int i : dom) mine[i] = 1;
        for (size_t i = 0; i < procs.size(); i++) if (procs[i].combRank >= 0) mine[i] = 1;
        std::function<void(const E&)> refs = [&](const E& e) {
            if (!e) return;
            if (e->op == Op::Ref && e->sig >= 0 && (size_t)e->sig < ns) bad[e->sig] = 1;
            for (auto& k : e->kids) refs(k);
        };
        for (size_t i = 0; i < procs.size(); i++) {
            for (auto& [sg, ed] : procs[i].seqEv) if ((size_t)sg < ns) bad[sg] = 1;
            if (procs[i].combRank >= 0) {
                for (auto& in : procs[i].prog->code) {
                    if (in.k != IK::Assign) break;
                    if (in.sig < 0 || (size_t)in.sig >= ns) continue;
                    if (in.drv >= 0 && in.hi < 0 && sigs[in.sig].drivers == 1 && in.s0 != STR_HIZ && in.s1 != STR_HIZ)
                        cand[in.sig] = 1;
                    else bad[in.sig] = 1;
                }
                continue;
            }
            if (mine[i]) {
                for (auto& in : procs[i].prog->code)
                    if (in.k == IK::Nba && in.sig >= 0 && (size_t)in.sig < ns) {
                        if (in.hi < 0) cand[in.sig] = 1; else bad[in.sig] = 1;
                    }
                continue;
            }
            // Another process may still write one with a plain blocking
            // assign, read it in compiled code, or $display it (the value is
            // copied into S.sigs first). Anything else keeps it in S.sigs.
            std::vector<const Prog*> progs;
            std::function<void(const Prog*)> walk = [&](const Prog* p) {
                if (!p || std::find(progs.begin(), progs.end(), p) != progs.end()) return;
                progs.push_back(p);
                for (auto& in : p->code) { walk(in.callee); for (auto* b : in.branches) walk(b); }
            };
            walk(procs[i].prog);
            std::function<void(const E&)> arr = [&](const E& e) {
                if (!e) return;
                if (e->arr) anyArr = true;
                for (auto& k : e->kids) arr(k);
            };
            for (const Prog* p : progs)
                for (auto& in : p->code) {
                    bool plainWrite = in.k == IK::Assign && in.hi < 0 && in.drv < 0 && !in.arr && in.aidx.empty() && !in.bidx;
                    if (in.sig >= 0 && (size_t)in.sig < ns && !plainWrite) bad[in.sig] = 1;
                    for (auto& [sg, ed] : in.events) if ((size_t)sg < ns) bad[sg] = 1;
                    bool compiled = in.k == IK::Assign || in.k == IK::JmpIfNot || in.k == IK::Nba || in.k == IK::WaitDelay;
                    if (!compiled) { refs(in.e); refs(in.bidx); refs(in.de); for (auto& a : in.aidx) refs(a); }
                    if (in.k != IK::Display) for (auto& a : in.args) refs(a);
                    arr(in.e);
                    for (auto& a : in.args) arr(a);
                    if (in.arr) anyArr = true;
                }
            if (anyArr) { priv_.assign(ns, 0); return; }   // run-time array access: keep everything in S.sigs
        }
        priv_.assign(ns, 0);
        for (size_t s = 0; s < ns; s++)
            priv_[s] = cand[s] && !bad[s] && sigs[s].w >= 1 && sigs[s].w <= 64 &&
                       sigs[s].name.find_first_of("[$") == std::string::npos;
    }

    // The 4-state branch, out of the hot code: a domain of thousands of
    // statements otherwise misses the instruction cache all the time.
    static std::string cold(const std::string& stmt) {
        return "[&]() __attribute__((noinline, cold)) { " + stmt + " }();";
    }
    // Writes to a private signal: a known value, or any Val.
    std::string pwv(int s, const std::string& u) const {
        return "{ P" + std::to_string(s) + ".v = (" + u + ") & " + M(sigW_[s]) + "; P" + std::to_string(s) + ".x = 0; P" +
               std::to_string(s) + ".z = 0; }";
    }
    std::string pw(int s, const std::string& val) const {
        return "PW(P" + std::to_string(s) + ", " + std::to_string(sigW_[s]) + ", " + val + ");";
    }

    // A comb write that bypasses set() when nothing else watches the signal.
    std::string fassign(const Instr& i) {
        std::ostringstream o;
        std::string g, u, sl;
        if (isPriv(i.sig)) {
            if (split(i.e, g, u, sl)) {
                if (g.empty()) return " " + pwv(i.sig, u) + "\n";
                return " if (__builtin_expect(" + g + ", 1)) " + pwv(i.sig, u) + " else " + cold(pw(i.sig, sl)) + "\n";
            }
            return " " + pw(i.sig, sl) + "\n";
        }
        if (i.hi < 0 && split(i.e, g, u, sl)) {
            std::string fastW = i.drv >= 0
                ? "S.fwdv(" + std::to_string(i.sig) + ", " + std::to_string(i.drv) + ", " + u + ", " +
                  std::to_string((int)i.s0) + ", " + std::to_string((int)i.s1) + ");"
                : regWrite(i.sig, u);
            std::string slowW = i.drv >= 0
                ? "S.fwd(" + std::to_string(i.sig) + ", " + std::to_string(i.drv) + ", " + sl + ", " +
                  std::to_string((int)i.s0) + ", " + std::to_string((int)i.s1) + ");"
                : "S.fw(" + std::to_string(i.sig) + ", " + sl + ");";
            if (g.empty()) return " " + fastW + "\n";
            return " if (__builtin_expect(" + g + ", 1)) " + fastW + " else " + cold(slowW) + "\n";
        }
        std::string val = fast(i.e);
        if (i.drv >= 0 && i.hi >= 0) return assign(i, val);
        if (i.drv >= 0)
            o << " S.fwd(" << i.sig << ", " << i.drv << ", " << val << ", " << (int)i.s0 << ", " << (int)i.s1 << ");\n";
        else if (i.hi >= 0)
            o << " S.fw(" << i.sig << ", v_insert(S.get(" << i.sig << "), " << val << ", " << i.hi << ", " << i.lo << "));\n";
        else
            o << " S.fw(" << i.sig << ", " << val << ");\n";
        return o.str();
    }

    // dsN: one domain process, its NBAs into slots F<k>/H<k>. dseq runs them
    // all (in parallel) and leaves one marker NBA; fcommit writes the slots in
    // queue order and re-evaluates every static comb process in rank order.
    void emitCycle(std::ostream& o, const std::vector<CodegenProc>& procs, const std::vector<int>& dom,
                   const std::vector<int>& byRank) {
        clock_.assign(sigW_.size(), 0);
        for (auto& pr : procs)
            for (auto& [sg, ed] : pr.seqEv) if (sg >= 0 && (size_t)sg < clock_.size()) clock_[sg] = 1;
        struct Slot { int sig, hi, lo; };
        std::vector<Slot> slots;
        std::vector<int> slotProc;   // the domain process of each slot
        std::ostringstream fns, fns2;
        // 2-state mode: when no signal the domain reads holds X or Z, every
        // statement takes its 2-state path, so a second copy without the
        // checks runs instead (dsN2, combQ). Only when every statement has one.
        bool two = twoState;
        std::set<int> ext;   // signals in S.sigs that the domain reads
        auto noteRefs = [&](const E& e) {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->op == Op::Ref && !isPriv(x->sig)) ext.insert(x->sig);
                for (auto& k : x->kids) f(k);
            };
            f(e);
        };
        for (int n : dom) {
            const Prog& p = *procs[n].prog;
            size_t end = p.code.size() - 1;
            auto L = [](size_t pc) { return "d_" + std::to_string(pc); };
            fns << "// process " << n << " (" << procs[n].kind << ", cycle domain)\n";
            fns << "static void ds" << n << "() {\n";
            fns2 << "static void ds" << n << "_2() {\n";
            for (size_t pc = 1; pc < end; pc++) {
                const Instr& i = p.code[pc];
                fns << L(pc) << ":";
                fns2 << L(pc) << ":";
                noteRefs(i.e);
                switch (i.k) {
                    case IK::Nba: {
                        if (i.arr || !i.aidx.empty() || i.bidx) throw CodegenUnsupported("array or variable-index write");
                        int k = (int)slots.size();
                        slots.push_back({i.sig, i.hi, i.lo});
                        slotProc.push_back(n);
                        std::string g, u, sl;
                        bool sp = i.hi < 0 && split(i.e, g, u, sl);
                        if (sp) fns2 << " V" << k << " = " << u << "; H" << k << " = 1;\n";
                        else two = false;
                        // H: 1 = the 2-state value in V<k>, 2 = the Val in F<k>.
                        if (sp) {
                            if (g.empty()) fns << " V" << k << " = " << u << "; H" << k << " = 1;\n";
                            else fns << " if (__builtin_expect(" << g << ", 1)) { V" << k << " = " << u << "; H" << k
                                     << " = 1; } else " << cold("F" + std::to_string(k) + " = " + sl + "; H" + std::to_string(k) + " = 2;") << "\n";
                        } else
                            fns << " F" << k << " = " << fast(i.e) << "; H" << k << " = 2;\n";
                        break;
                    }
                    case IK::JmpIfNot: {
                        std::string g, u, sl;
                        if (split(i.e, g, u, sl)) {
                            fns2 << " if (!(" << u << ")) goto " << L(i.target) << ";\n";
                            std::string slowT = "[&]() __attribute__((noinline, cold)) { return truth(" + sl + ") != 1; }()";
                            fns << " if (" << (g.empty() ? "!(" + u + ")" : "(__builtin_expect(" + g + ", 1) ? !(" + u + ") : " + slowT + ")")
                                << ") goto " << L(i.target) << ";\n";
                        } else {
                            fns << " if (truth(" << sl << ") != 1) goto " << L(i.target) << ";\n";
                            two = false;
                        }
                        break;
                    }
                    case IK::Jmp: fns << " goto " << L(i.target) << ";\n"; fns2 << " goto " << L(i.target) << ";\n"; break;
                    default: throw CodegenUnsupported("instruction in a clocked process");
                }
            }
            fns << L(end) << ": return;\n}\n";
            fns2 << L(end) << ": return;\n}\n";
        }
        for (size_t k = 0; k < slots.size(); k++)
            o << "static Val F" << k << "; static uint64_t V" << k << "; static uint8_t H" << k << ";\n";
        std::vector<int> privs;
        for (size_t s = 0; s < priv_.size(); s++) if (priv_[s]) privs.push_back((int)s);
        // Only when something watches every change (a VCD opened at run time).
        o << "static void syncP() {\n";
        for (int s : privs)
            o << "    S.fw(" << s << ", Val{P" << s << ".v, P" << s << ".x, " << sigW_[s] << ", P" << s << ".z, false});\n";
        o << "}\n";
        // Comb statements first: their reads are part of `ext`, which twoNow checks.
        std::vector<std::string> combStmts, combStmts2, commitStmts;
        std::vector<const Instr*> combIns;
        for (int i : byRank)
            for (const Instr& in : procs[i].prog->code) {
                if (in.k != IK::Assign) break;
                combStmts.push_back("   " + fassign(in));
                combIns.push_back(&in);
                noteRefs(in.e);
                std::string g, u, sl;
                if (in.hi >= 0 || !split(in.e, g, u, sl)) { two = false; continue; }
                std::string S_ = std::to_string(in.sig);
                if (isPriv(in.sig))   // x and z are already 0 in 2-state mode
                    combStmts2.push_back("    P" + S_ + ".v = (" + u + ") & " + M(sigW_[in.sig]) + ";\n");
                else if (in.drv >= 0)
                    combStmts2.push_back("    S.fwdv(" + S_ + ", " + std::to_string(in.drv) + ", " + u + ", " +
                                         std::to_string((int)in.s0) + ", " + std::to_string((int)in.s1) + ");\n");
                else
                    combStmts2.push_back("    " + regWrite(in.sig, u) + "\n");
            }
        o << fns.str();
        if (two) o << fns2.str();
        // 2-state mode holds when every private signal is known (pclean, kept
        // by the writes) and so is every S.sigs signal the domain reads.
        o << "static bool twoNow() {\n    return " << (two ? "pclean && (0" : "false");
        if (two) {
            for (int e : ext) o << " | S.sigs[" << e << "].val.x";
            o << ") == 0";
        }
        o << ";\n}\n";
        o << "static void scanP() {\n    uint64_t x = 0;\n";
        for (int s : privs) o << "    x |= P" << s << ".x;\n";
        o << "    pclean = x == 0;\n}\n";
        o << "static void (*const DS[])() = {";
        for (size_t j = 0; j < dom.size(); j++) o << (j ? ", " : "") << "ds" << dom[j];
        o << "};\n";
        if (two) {
            o << "static void (*const DS2[])() = {";
            for (size_t j = 0; j < dom.size(); j++) o << (j ? ", " : "") << "ds" << dom[j] << "_2";
            o << "};\n";
        }
        // Islands (like the kernel's partitions): processes and comb statements
        // that share no signal the domain writes. When every domain target is
        // private, each group of islands runs its NBAs, commit and comb logic
        // on one thread with no locks: the groups write disjoint variables.
        std::vector<int> isl;          // per signal: union-find parent
        isl.resize(sigW_.size());
        for (size_t x = 0; x < isl.size(); x++) isl[x] = (int)x;
        std::function<int(int)> find = [&](int x) { while (isl[x] != x) x = isl[x] = isl[isl[x]]; return x; };
        auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) isl[std::max(a, b)] = std::min(a, b); };
        std::vector<char> written(sigW_.size(), 0);
        bool allPriv = two;
        for (auto& sl : slots) { written[sl.sig] = 1; if (!isPriv(sl.sig)) allPriv = false; }
        for (auto* in : combIns) { written[in->sig] = 1; if (!isPriv(in->sig)) allPriv = false; }
        auto refsOf = [&](const E& e, std::vector<int>& out) {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->op == Op::Ref && x->sig >= 0 && (size_t)x->sig < written.size() && written[x->sig]) out.push_back(x->sig);
                for (auto& k : x->kids) f(k);
            };
            f(e);
        };
        std::map<int, int> procSig;    // domain process -> one of its targets
        for (size_t k = 0; k < slots.size(); k++) {
            int n = slotProc[k];
            if (!procSig.count(n)) procSig[n] = slots[k].sig;
            unite(procSig[n], slots[k].sig);
        }
        for (int n : dom) {
            if (!procSig.count(n)) { allPriv = false; continue; }
            std::vector<int> rs;
            for (auto& in : procs[n].prog->code) refsOf(in.e, rs);
            for (int r : rs) unite(procSig[n], r);
        }
        for (auto* in : combIns) {
            std::vector<int> rs;
            refsOf(in->e, rs);
            for (int r : rs) unite(in->sig, r);
        }
        std::map<int, int> islandId;
        auto islandOf = [&](int sig) {
            int r = find(sig);
            auto it = islandId.find(r);
            if (it != islandId.end()) return it->second;
            int id = (int)islandId.size();
            islandId[r] = id;
            return id;
        };
        for (int n : dom) if (procSig.count(n)) islandOf(procSig[n]);
        for (auto* in : combIns) islandOf(in->sig);
        int nIsl = (int)islandId.size();
        const int kGroups = 64;
        bool par = allPriv && nIsl >= 32;
        int G = par ? std::min(nIsl, kGroups) : 0;
        auto groupOf = [&](int sig) { return (int)((int64_t)islandOf(sig) * G / nIsl); };
        if (par) {
            std::vector<std::vector<int>> gProcs(G);
            for (int n : dom) gProcs[groupOf(procSig[n])].push_back(n);
            for (int g = 0; g < G; g++) {
                o << "static void dsG" << g << "(bool two) {\n    if (two) {";
                for (int n : gProcs[g]) o << " ds" << n << "_2();";
                o << " } else {";
                for (int n : gProcs[g]) o << " ds" << n << "();";
                o << " }\n}\n";
            }
            for (int g = 0; g < G; g++) o << "static void cmG" << g << "(bool);\n";
            o << "static void (*const CMG[])(bool) = {";
            for (int g = 0; g < G; g++) o << (g ? ", " : "") << "cmG" << g;
            o << "};\n";
            o << "static void (*const DSG[])(bool) = {";
            for (int g = 0; g < G; g++) o << (g ? ", " : "") << "dsG" << g;
            o << "};\n";
        }
        o << "static void dseq(const void*, std::vector<Sim::NbaItem>& nb) {\n";
        if (par)
            // Nothing else runs before the NBA region (S.fuseOk()): each group
            // computes, commits and settles its islands in one parallel pass.
            o << "    if (S.fuseOk()) {\n"
                 "        const bool t = twoNow();\n"
                 "        const int use = S.domainTune[0].pick(S.threads);\n"
                 "        S.parallelFor(" << G << ", [t](size_t g) { DSG[g](t); CMG[g](t); }, use);\n"
                 "        S.domainTune[0].done(S.threads);\n"
                 "        if (!t) scanP();\n"
                 "        if (S.observed) syncP();\n"
                 "        return;\n    }\n"
                 "    { const bool t = twoNow(); const int use = S.domainTune[1].pick(S.threads);\n"
                 "      S.parallelFor(" << G << ", [t](size_t g) { DSG[g](t); }, use); S.domainTune[1].done(S.threads); }\n";
        else if (two) o << "    if (twoNow()) { for (auto f : DS2) f(); } else\n";
        if (!par) o << "    for (auto f : DS) f();\n";
        o << "    nb.push_back({-1, Val{}, -1, 0});\n}\n";
        // Both are split into functions of at most kChunk statements: one huge
        // function takes the C++ compiler minutes and is no faster.
        const int kChunk = 64;
        for (size_t k = 0; k < slots.size(); k++) {
            const Slot& sl = slots[k];
            std::string K = std::to_string(k), sg = std::to_string(sl.sig);
            std::string w;
            if (sl.hi >= 0)
                w = "    if (H" + K + ") { H" + K + " = 0; S.fw(" + sg + ", v_insert(S.get(" + sg + "), F" + K + ", " +
                    std::to_string(sl.hi) + ", " + std::to_string(sl.lo) + ")); }\n";
            else if (isPriv(sl.sig))
                w = "    if (H" + K + " == 1) " + pwv(sl.sig, "V" + K) + " else if (H" + K + ") { " + pw(sl.sig, "F" + K) +
                    " pclean = false; } H" + K + " = 0;\n";
            else
                w = "    if (H" + K + " == 1) " + regWrite(sl.sig, "V" + K) + " else if (H" + K + ") S.fw(" + sg + ", F" + K +
                    "); H" + K + " = 0;\n";
            commitStmts.push_back(w);
        }
        auto chunks = [&](const std::string& name, const std::vector<std::string>& st) {
            int n = 0;
            for (size_t a = 0; a < st.size(); a += kChunk, n++) {
                o << "static void " << name << n << "() {\n    const bool obs = S.observed; (void)obs;\n";
                for (size_t b = a; b < st.size() && b < a + kChunk; b++) o << st[b];
                o << "}\n";
            }
            return n;
        };
        int nc = chunks("combP", combStmts), nm = chunks("commitP", commitStmts);
        int nq = two ? chunks("combQ", combStmts2) : 0;
        o << "static void combAll(const void*) {\n";
        if (two) {
            o << "    if (twoNow()) {";
            for (int j = 0; j < nq; j++) o << " combQ" << j << "();";
            o << " } else {";
            for (int j = 0; j < nc; j++) o << " combP" << j << "();";
            o << " scanP(); }\n";
        } else
            for (int j = 0; j < nc; j++) o << "    combP" << j << "();\n";
        o << "    if (S.observed) syncP();\n}\n";
        if (par) {
            // Per group: its slots' commits, then its comb statements in rank order.
            std::vector<std::string> gCommit(G), gComb(G), gComb2(G);
            for (size_t k = 0; k < slots.size(); k++) gCommit[groupOf(slots[k].sig)] += commitStmts[k];
            for (size_t j = 0; j < combIns.size(); j++) {
                int g = groupOf(combIns[j]->sig);
                gComb[g] += combStmts[j];
                gComb2[g] += combStmts2[j];
            }
            for (int g = 0; g < G; g++)
                o << "static void cmG" << g << "(bool two) {\n    const bool obs = false; (void)obs;\n" << gCommit[g]
                  << "    if (two) {\n" << gComb2[g] << "    } else {\n" << gComb[g] << "    }\n}\n";
            // twoNow() before the commits: in 2-state mode every slot holds a
            // known value, so the commits keep every private signal known.
            o << "static void fcommit() {\n"
                 "    const bool t = twoNow();\n"
                 "    const int use = S.domainTune[2].pick(S.threads);\n"
                 "    S.parallelFor(" << G << ", [t](size_t g) { CMG[g](t); }, use);\n"
                 "    S.domainTune[2].done(S.threads);\n"
                 "    if (!t) scanP();\n"
                 "    if (S.observed) syncP();\n}\n\n";
            return;
        }
        o << "static void fcommit() {";
        for (int j = 0; j < nm; j++) o << " commitP" << j << "();";
        o << " S.fastCombs(combAll); if (S.observed) syncP(); }\n\n";
    }

    // A clocked process: the body after its wait, NBAs into `nb`.
    void emitSeq(std::ostream& o, int n, const Prog& p, const std::string& kind) {
        size_t end = p.code.size() - 1;
        auto L = [](size_t pc) { return "s_" + std::to_string(pc); };
        o << "// process " << n << " (" << kind << ", clocked)\n";
        o << "static void seq" << n << "(const void*, std::vector<Sim::NbaItem>& nb) {" << CH(&p, 0) << "\n";
        for (size_t pc = 1; pc < end; pc++) {
            const Instr& i = p.code[pc];
            o << L(pc) << ":" << CH(&p, pc);
            switch (i.k) {
                case IK::Nba:
                    if (i.drv >= 0 || i.dyn >= 0 || i.nbaLater) throw CodegenUnsupported("NBA kind in a clocked process");
                    if (i.arr || i.bidx) o << dynTarget(i, fast(i.e), " nb.push_back({s, v, hi, lo});");
                    else o << " nb.push_back({" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << "});\n";
                    break;
                case IK::JmpIfNot: o << " if (truth(" << fast(i.e) << ") != 1) " << jumpTo(&p, pc, L(i.target)) << "\n"; break;
                case IK::Jmp: o << " goto " << L(i.target) << ";\n"; break;
                default: throw CodegenUnsupported("instruction in a clocked process");
            }
        }
        o << L(end) << ": return;\n}\n\n";
    }

    // A combinational process: its leading Assign instructions as a function.
    void emitComb(std::ostream& o, int n, const Prog& p, const std::string& kind) {
        o << "// process " << n << " (" << kind << ", combinational)\n";
        o << "static void comb" << n << "(const void*) {\n";
        auto L = [&](size_t pc) { return "c" + std::to_string(n) + "_" + std::to_string(pc); };
        size_t pc = 0;
        for (; pc < p.code.size(); pc++) {
            const Instr& i = p.code[pc];
            if (i.k == IK::JmpIfNot) {
                o << L(pc) << ":" << CH(&p, pc) << " if (truth(" << fast(i.e) << ") != 1) " << jumpTo(&p, pc, L(i.target)) << "\n";
                continue;
            }
            if (i.k == IK::Jmp) { o << L(pc) << ":" << CH(&p, pc) << " goto " << L(i.target) << ";\n"; continue; }
            if (i.k != IK::Assign) break;
            if (i.arr || i.bidx) throw CodegenUnsupported("variable-index write in a static combinational process");
            o << L(pc) << ":" << CH(&p, pc) << " " << assign(i);
        }
        o << L(pc) << ": ;\n}\n";
        if (oneAssign(p)) {
            o << "static Val combv" << n << "(const void*) {" << CH(&p, 0) << " return " << fast(p.code[0].e) << "; }\n";
            o << "static void combc" << n << "(const void*, Val v) {" << assign(p.code[0], "v") << "}\n";
        }
        o << "\n";
    }

    // Every Prog this process can run: the entry, then callees, depth first.
    // Frames (automatic variables, expr.hpp) are interpreter-only.
    static void noFrames(const Instr& i) {
        bool rel = i.lay || !i.init.empty() || i.dynWait || isRel(i.sig) || hasRel(i.e) || hasRel(i.bidx) || hasRel(i.de);
        for (auto* b : i.blays) rel |= b != nullptr;
        for (auto& a : i.args) rel |= hasRel(a);
        for (auto& a : i.aidx) rel |= hasRel(a);
        for (auto& [s, e] : i.events) rel |= isRel(s);
        if (i.arr) rel |= isRel(i.arr->base);
        if (rel) throw CodegenUnsupported("automatic variables (frames)");
    }
    static void reach(const Prog* p, std::vector<const Prog*>& out) {
        for (const Prog* q : out)
            if (q == p) return;
        out.push_back(p);
        for (auto& i : p->code) noFrames(i);
        for (auto& i : p->code)
            if (i.k == IK::Call) {
                if (!i.callee) throw CodegenUnsupported("call without a callee");
                reach(i.callee, out);
            }
    }

    void emitProc(std::ostream& o, int n, const Prog& entry, const std::string& kind) {
        std::vector<const Prog*> progs;
        reach(&entry, progs);
        auto idx = [&](const Prog* p) {
            for (size_t i = 0; i < progs.size(); i++)
                if (progs[i] == p) return (int)i;
            return -1;
        };
        auto L = [](int p, size_t pc) { return "p" + std::to_string(p) + "_" + std::to_string(pc); };
        std::vector<std::string> retLabels;  // return id -> label to resume at
        bool calls = progs.size() > 1;

        o << "// process " << n << " (" << kind << ")\n";
        o << "static Task proc" << n << "() {\n";
        if (calls) o << "    std::vector<int> rs;\n";
        bool asserting = false;
        for (const Prog* q : progs)
            for (auto& i : q->code) asserting |= i.k == IK::Assert;
        if (asserting) o << "    auto ctl = std::make_shared<ProcCtl>();\n";
        for (size_t pi = 0; pi < progs.size(); pi++) {
            const Prog& p = *progs[pi];
            int P = (int)pi;
            if (pi) o << "    // subroutine body\n";
            for (size_t pc = 0; pc < p.code.size(); pc++) {
                const Instr& i = p.code[pc];
                o << L(P, pc) << ":" << CH(&p, pc);
                auto tgt = [&](size_t t) { return L(P, t > p.code.size() ? p.code.size() : t); };
                // A write chosen at run time (a[i] = ..., v[i] = ...): Assign and Nba only.
                bool dynW = (i.k == IK::Assign || i.k == IK::Nba) && (i.arr || i.bidx);
                if (dynW && ((i.k != IK::Assign && i.k != IK::Nba) || i.drv >= 0))
                    throw CodegenUnsupported(std::string("array or variable-index ") + (i.drv >= 0 ? "continuous assign" : "write (instruction " + std::to_string((int)i.k) + ")"));
                if (i.de || i.nbaLater) throw CodegenUnsupported("delay expression or intra-assignment delay");
                if (i.dyn >= 0 || i.dsrc >= 0 || i.dres >= 0 || i.dkey >= 0) throw CodegenUnsupported("dynamic array");
                if (!p.roots.empty()) throw CodegenUnsupported("disable of a named block");
                if (i.k == IK::Finish && i.hi > 0) throw CodegenUnsupported("$fatal exit status");
                if (i.fd) throw CodegenUnsupported("$fstrobe / $fmonitor");
                if (!i.dly.empty()) throw CodegenUnsupported("delayed continuous assignment");
                if (i.k == IK::ReadMem || i.k == IK::WriteMem) {   // $readmem / $writemem (21.4, 21.5)
                    if (!i.arr || readsPriv(i.e)) throw CodegenUnsupported("$readmem form");
                    for (auto& a : i.args) if (readsPriv(a)) throw CodegenUnsupported("$readmem form");
                    mems_.push_back(&i);
                    std::string M_ = "MI" + std::to_string(mems_.size() - 1);
                    arrName(i.arr.get());
                    if (i.e) {   // a file name known only at run time
                        evals_.push_back(i.e);
                        o << " { Instr n = " << M_ << "; n.fmt = v_str(eval(ET" << evals_.size() - 1 << ", rd)); "
                          << (i.k == IK::ReadMem ? "readMem" : "writeMem") << "(n); }\n";
                    } else
                        o << " " << (i.k == IK::ReadMem ? "readMem" : "writeMem") << "(" << M_ << ");\n";
                    continue;
                }
                if (dynW) {
                    o << dynTarget(i, fast(i.e), i.k == IK::Nba ? " S.nba(s, v, hi, lo);"
                                                                : " S.set(s, hi >= 0 ? v_insert(S.get(s), v, hi, lo) : v);");
                    continue;
                }
                switch (i.k) {
                    case IK::Assign: o << assign(i); break;
                    case IK::Nba:
                        o << " S.nba(" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << ");\n";
                        break;
                    case IK::JmpIfNot:
                        o << " if (truth(" << fast(i.e) << ") != 1) " << jumpTo(&p, pc, tgt(i.target)) << "\n";
                        break;
                    case IK::Jmp: o << " goto " << tgt(i.target) << ";\n"; break;
                    case IK::WaitEdge:
                        if (i.events.size() > 1) {
                            events_.push_back(i.events);
                            o << " co_await WaitAny{&EV" << events_.size() - 1 << "};";
                        } else
                            o << " co_await Wait{" << i.sig << ", " << edge(i.edge) << "};";
                        o << (asserting ? " ctl->epoch++;\n" : "\n");   // a deferred assertion flush point (16.4.2)
                        break;
                    case IK::WaitDelay: o << " co_await Delay{" << i.delay << "ULL};\n"; break;
                    case IK::Display:
                    case IK::Strobe:
                    case IK::Monitor: {
                        sysArgs_.push_back(i.args);
                        const char* fn = i.k == IK::Display ? "printSys" : i.k == IK::Strobe ? "strobe" : "armMon";
                        // The kernel prints from S.sigs: copy in the private signals it reads.
                        if (i.k == IK::Display) {
                            std::vector<int> rs;
                            std::function<void(const E&)> col = [&](const E& e) {
                                if (!e) return;
                                if (e->op == Op::Ref && isPriv(e->sig)) rs.push_back(e->sig);
                                for (auto& k : e->kids) col(k);
                            };
                            for (auto& a : i.args) col(a);
                            for (int r : rs)
                                o << " S.sigs[" << r << "].val = PR(P" << r << ", " << sigW_[r] << ", false);";
                        }
                        o << " S." << fn << "(" << quote(i.fmt) << ", A" << sysArgs_.size() - 1;
                        if (i.k == IK::Display && i.nonl) o << ", false";  // $write: no newline
                        o << "); if (S.finished) co_return;\n";
                        break;
                    }
                    case IK::Finish: o << " S.finished = true; co_return;\n"; break;
                    case IK::Assert: {   // an immediate assertion (16.3, 16.4), through the kernel's runAssert
                        if (i.join > 2) throw CodegenUnsupported("a procedural concurrent assertion or expect");
                        const AssertInfo& a = asserts().at(i.sig);
                        bool priv = readsPriv(i.e);
                        for (auto* cs : {&a.passCap, &a.failCap})
                            for (auto& c : *cs) priv |= readsPriv(c);
                        if (priv) throw CodegenUnsupported("an assertion on a private signal");
                        asrts_.push_back(&i);
                        std::string A_ = "AI" + std::to_string(asrts_.size() - 1);
                        if (i.join)   // deferred: queues its report and goes on
                            o << " runAssert(" << A_ << ", ctl, 0);\n";
                        else   // simple: 11 runs the pass action, 1 the fail action, 2 neither
                            o << " { size_t r = runAssert(" << A_ << ", ctl, 10); if (r == 1) goto " << tgt(i.target)
                              << "; if (r == 2) goto " << tgt((size_t)i.hi) << "; }\n";
                        break;
                    }
                    case IK::AssertCtl:   // $assertcontrol and friends (20.11)
                        for (auto& a : i.args)
                            if (readsPriv(a)) throw CodegenUnsupported("$assertcontrol on a private signal");
                        asrts_.push_back(&i);
                        o << " assertCtl(AI" << asrts_.size() - 1 << ");\n";
                        break;
                    case IK::Call:
                        retLabels.push_back(L(P, pc + 1));
                        o << " rs.push_back(" << retLabels.size() - 1 << "); goto " << L(idx(i.callee), 0) << ";\n";
                        break;
                    case IK::Ret: o << " goto ret;\n"; break;
                    default: throw CodegenUnsupported("instruction " + std::to_string((int)i.k));
                }
            }
            // Falling off the end is a return (an empty stack ends the process).
            o << L(P, p.code.size()) << ": goto ret;\n";
        }
        o << "ret:\n";
        if (calls) {
            o << "    if (rs.empty()) co_return;\n"
                 "    { int r = rs.back(); rs.pop_back();\n"
                 "      switch (r) {\n";
            for (size_t r = 0; r < retLabels.size(); r++) o << "        case " << r << ": goto " << retLabels[r] << ";\n";
            o << "      }\n    }\n";
        }
        o << "    co_return;\n}\n\n";
    }
};

inline std::string codegen(const std::vector<CodegenSig>& sigs, const std::vector<CodegenProc>& procs,
                           const CodegenParts& parts = {}, bool twoState = true, bool cycle = true,
                           bool privates = true,
                           std::vector<std::pair<const Prog*, size_t>>* covSlots = nullptr, int split = 1) {
    Codegen g;
    g.files = split;
    g.twoState = twoState;
    g.cycle = cycle;
    g.privates = privates;
    g.cov = covSlots != nullptr;
    std::string src = g.run(sigs, procs, parts);
    if (covSlots) *covSlots = std::move(g.covSlots);
    return src;
}
