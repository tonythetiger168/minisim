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
        for (auto& g : sigs) { sigW_.push_back(g.w); sigNet_.push_back(g.net); scratch_.push_back(g.name.rfind("_tf", 0) == 0); sigName_.push_back(g.name); }
        {
            // Array shapes numbered by base signal: the instances of a module
            // then have theirs at a constant distance, as shared code needs.
            std::vector<const ArrShape*> as;
            std::function<void(const E&)> walk = [&](const E& e) {
                if (!e) return;
                if (e->arr) as.push_back(e->arr.get());
                for (auto& k : e->kids) walk(k);
            };
            for (auto& p : procs)
                for (auto& in : p.prog->code) {
                    if (in.arr) as.push_back(in.arr.get());
                    walk(in.e);
                    walk(in.bidx);
                    for (auto& x : in.aidx) walk(x);
                    for (auto& x : in.args) walk(x);
                }
            std::stable_sort(as.begin(), as.end(), [](const ArrShape* x, const ArrShape* y) { return x->base < y->base; });
            for (auto* a : as) arrName(a);
        }
        std::vector<std::string> chunks;   // one per process: the unit spread over files
        std::string cyc;                   // the cycle domain, kept with main()
        auto chunk = [&](auto&& emit) { std::ostringstream c; emit(c); chunks.push_back(c.str()); };
        std::vector<int> byRank;
        std::vector<int> dom = cycle && !cov ? cycleDomain(procs, sigs) : std::vector<int>{};
        {
            // The domain re-evaluates every static combinational statement on
            // each commit (and whenever an input outside the clock cone
            // changes). That pays when registers feed the logic every cycle:
            // bench/ has at most 6 statements per NBA, Ibex 16 (3200 for 199)
            // and runs 1.5x faster in the domain than with only the changed
            // ranks evaluated. A design that is nearly all comb logic stays
            // out. MSIM_XCYC=1 forces the domain, MSIM_WHYCYCLE=1 prints the counts.
            size_t nba = 0, comb = 0;
            for (int i : dom)
                for (auto& in : procs[i].prog->code) nba += in.k == IK::Nba;
            for (auto& p : procs)
                if (p.combRank >= 0)
                    for (auto& in : p.prog->code) { if (in.k == IK::WaitEdge) break; comb += in.k == IK::Assign; }
            if (std::getenv("MSIM_WHYCYCLE")) std::fprintf(stderr, "cycle domain: %zu clocked processes, %zu NBAs, %zu comb statements\n", dom.size(), nba, comb);
            if (comb > 64 && comb > 64 * nba && !getenv("MSIM_XCYC")) dom.clear();
        }
        if (!dom.empty()) wakeEvents(procs, dom);
        if (!dom.empty() && privates) privateSigs(sigs, procs, dom);
        std::vector<CodegenProc> wideProcs;
        if (!dom.empty() && privates && !std::getenv("MSIM_NOWIDE")) wideProcs = splitWide(procs, dom);
        for (size_t i = 0; i < procs.size(); i++) {
            bool inDom = std::find(dom.begin(), dom.end(), (int)i) != dom.end();
            if (upf && procs[i].prog->pd >= 0 && (procs[i].combRank >= 0 || procs[i].seq || inDom))
                throw CodegenUnsupported("a statically scheduled process of a power domain");
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

        if (!dom.empty()) { std::ostringstream c; emitCycle(c, wideProcs.empty() ? procs : wideProcs, dom, byRank); cyc = c.str(); }
        // Several files: the shared variables are inline (one object for
        // all files) and the process functions external.
        // The cycle domain splits too: its functions (ds<n>, combP<n>, ...)
        // spread over the files like the processes, its variables go to the
        // header.
        std::vector<std::string> cycFns;
        std::string cycHdr;
        splitTop(cyc, cycFns, cycHdr);
        size_t chunkBytes = 0;
        for (auto& c : chunks) chunkBytes += c.size();
        for (auto& c : cycFns) chunkBytes += c.size();
        size_t nChunks = chunks.size() + cycFns.size();
        // files < 0: exactly that many (msim -cfiles), else only for a large design.
        int nf = files < 0 ? (int)std::min<size_t>(-files, nChunks) : files > 1 ? (int)std::min<size_t>(files, chunkBytes / 65536) : 1;
        nf = std::max(nf, 1);
        if (nf > 1) {
            for (auto& c : cycFns) chunks.push_back(std::move(c));
            cyc.clear();
        }
        const char* SV = nf > 1 ? "inline " : "static ";

        std::ostringstream o;
        o << "// Generated by msim -compile. Do not edit.\n"
             "#define MINISIM_NO_MAIN\n"
          << (nf > 1 ? "#define MINISIM_EXTERN_S   // defined once, in design.cpp\n" : "") <<
             "#include \"minisim.cpp\"\n"
             "#include <cstdlib>\n"
             "#include <cstring>\n"
             "#include <thread>\n\n"
             "#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n"
             "static inline Val R(int s, bool sg) { Val v = S.get(s); v.sgn = sg; return v; }\n"
             "static inline Val RS(Val v, int w, bool fill, bool sg) {\n"
             "    Val r = v_resize(v, w, fill && w > v.w); r.sgn = sg; return r;\n"
             "}\n"
             "#ifdef MSIM_COLDSTAT\n"
             "inline long coldN[1 << 16];\n"
             "struct ColdDump { ~ColdDump() { for (int r = 0; r < 40; r++) { int b = 0; for (int k = 1; k < (1 << 16); k++) if (coldN[k] > coldN[b]) b = k;\n"
             "    if (!coldN[b]) break; std::fprintf(stderr, \"COLD(%d) %ld\\n\", b, coldN[b]); coldN[b] = 0; } } };\n"
             "inline ColdDump coldDump;\n"
             "#define COLD(k) coldN[k]++;\n"
             "#else\n"
             "#define COLD(k)\n"
             "#endif\n"
             "static inline bool UPD(uint64_t& a, uint64_t b) { bool c = a != b; a = b; return c; }\n"
             "static inline bool UPDV(Val& a, const Val& b) { if (a == b && a.w == b.w) return false; a = b; return true; }\n"
             "struct PSig { uint64_t v, x, z; };\n"
             "static inline Val PR(const PSig& p, int w, bool sg) { return Val{p.v & ~p.x, p.x, w, p.z, sg}; }\n"
             "static inline void PW(PSig& p, int w, const Val& a) { uint64_t m = mask(w); p.v = a.v & m; p.x = a.x & m; p.z = a.z & m; }\n"
             "static inline Val cat1(const Val& r, const Val& p) {\n"
             "    if (r.w + p.w <= 64) {\n"
             "        int s = p.w; bool o = s >= 64;\n"
             "        return make((o ? 0 : r.v << s) | p.v, (o ? 0 : r.x << s) | p.x, r.w + s, (o ? 0 : r.z << s) | p.z);\n"
             "    }\n"
             "    return v_concat({r, p});\n"
             "}\n"
             "template <class... T> static inline Val CAT(const T&... p) { Val r{0, 0, 0}; ((r = cat1(r, p)), ...); return r; }\n"
             "static inline Val REP(const Val& a, int n) {   // {n{a}}\n"
             "    if (a.w > 0 && n > 1 && a.w * n <= 64) {\n"
             "        uint64_t v = 0, x = 0, z = 0;\n"
             "        for (int i = 0; i < n; i++) { v = (v << a.w) | a.v; x = (x << a.w) | a.x; z = (z << a.w) | a.z; }\n"
             "        return make(v, x, a.w * n, z);\n"
             "    }\n"
             "    Val r{0, 0, 0}; for (int i = 0; i < n; i++) r = cat1(r, a); return r;\n"
             "}\n"
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
             "    Op op = Op(i()); Val k; k.v = u(); k.x = u(); k.w = i(); k.z = u(); int fl = i(); k.sgn = fl & 1; k.real = fl & 2;\n"
             "    if (int nl = i()) { k.hi.resize(nl); for (int j = 0; j < nl; j++) { k.hi[j].v = u(); k.hi[j].x = u(); k.hi[j].z = u(); } }\n"
             "    if (fl & 4) { int n = i(); p++; Val s = mkstr(std::string(p, n)); p += n; k.v = s.v; k.str = true; }\n"
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
        if (upf)   // msim -upf: a write by a process of a power domain (exec.inc upfWrite)
            o << "static inline Val UPFW(int pd, bool ps, int s, int hi, int lo, Val v) {\n"
                 "    return __builtin_expect(g_pdMode[(size_t)pd] > 0, 0) ? upfWriteM(pd, ps, s, hi, lo, v) : v;\n"
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
        // Arrays, like PA: shared comb code reaches them as AIA[k + D7], CCA[n + D8].
        o << SV << "Instr AIA[" << std::max<size_t>(asrts_.size(), 1) << "];\n";
        for (size_t i = 0; i < asrts_.size(); i++) o << SV << "Instr& AI" << i << " = AIA[" << i << "];\n";
        o << SV << "std::shared_ptr<ProcCtl> CCA[" << ccMax_ + 1 << "];\n";
        o << SV << "bool CIVA[" << ccMax_ + 1 << "];\n" << SV << "uint64_t* CIP[" << ccMax_ + 1 << "];\n";
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
        o << SV << "std::atomic<bool> pclean{false};   // every private signal is known (no X or Z bit)\n";        o << SV << "std::atomic<bool> cdirty{false};   // a 2-state comb pass met an unknown value: run the 4-state one\n";

        // Private signals start as their S.sigs counterparts do: a variable X, a net Z.
        // One array, indexed by signal: shared comb code (shareChunks) reaches
        // the private signals of another instance at a constant distance.
        {
            size_t np = 0;
            for (size_t x = 0; x < priv_.size(); x++) if (priv_[x]) np = x + 1;
            o << SV << "PSig PA[" << std::max<size_t>(np, 1) << "];\n";
            for (size_t x = 0; x < priv_.size(); x++)
                if (priv_[x]) o << SV << "PSig& P" << x << " = PA[" << x << "];\n";
            o << "static const struct { uint32_t s; uint64_t m; bool net; } PINIT[] = {";
            size_t c = 0;
            for (size_t x = 0; x < priv_.size(); x++)
                if (priv_[x]) o << (c++ ? ", " : "") << "{" << x << ", " << M(sigW_[x]) << ", " << (sigNet_[x] ? "true" : "false") << "}";
            if (!c) o << "{0, 0, false}";
            o << "};\n";
            if (c) privInit_ = "    for (auto& e : PINIT) { PA[e.s].x = e.m; PA[e.s].z = e.net ? e.m : 0; }\n";
        }
        // Startup statements (one per line). Split, they go to functions
        // spread over the files: in main() they made design.cpp the slowest
        // file to compile (Ibex: 1 MB, 42 s).
        std::string initText = privInit_, initCalls;
        for (size_t i = 0; i < sysArgs_.size(); i++) initText += "    A" + std::to_string(i) + " = {" + sysTrees[i] + "};\n";
        for (size_t i = 0; i < evals_.size(); i++) initText += "    ET" + std::to_string(i) + " = " + evTrees[i] + ";\n";
        for (auto& m : memInit) initText += m;
        initText += asInit;
        if (nf > 1 && !initText.empty()) {
            int k = 0;
            for (size_t at = 0; at < initText.size(); k++) {
                size_t end = std::min(initText.size(), at + 32768);
                if (end < initText.size()) end = initText.find('\n', end) + 1;
                if (end == 0) end = initText.size();
                chunks.push_back("static void startup" + std::to_string(k) + "() {\n" + initText.substr(at, end - at) + "}\n");
                initCalls += "    startup" + std::to_string(k) + "();\n";
                at = end;
            }
            initText.clear();
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
            outFiles.push_back(o.str() + "\n" + protos + cycHdr);
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
            o << "#include \"design.hpp\"\n\nSim S;\n\n";
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
                 "    // condition counters, then per FSM its states and transitions\n"
                 "    uint64_t nc = g_condHits.size(), nfsm = S.fsmSigs.size();\n"
                 "    std::fwrite(&nc, 8, 1, f); std::fwrite(g_condHits.data(), 8, nc, f);\n"
                 "    S.fsmFinish();\n"
                 "    std::fwrite(&nfsm, 8, 1, f);\n"
                 "    for (size_t k = 0; k < nfsm; k++) {\n"
                 "        uint64_t a = S.fsmSeen[k].size(), b = S.fsmTrans[k].size();\n"
                 "        std::fwrite(&a, 8, 1, f);\n"
                 "        for (auto& [v, c] : S.fsmSeen[k]) { uint64_t r[2] = {v, c}; std::fwrite(r, 8, 2, f); }\n"
                 "        std::fwrite(&b, 8, 1, f);\n"
                 "        for (auto& [t, c] : S.fsmTrans[k]) { uint64_t r[3] = {t.first, t.second, c}; std::fwrite(r, 8, 3, f); }\n"
                 "    }\n"
                 "    { std::ofstream fc(std::string(path) + \".fcov\"); writeFuncCov(fc); }   // covergroups, assertions\n"
                 "    return std::fclose(f) == 0;\n"
                 "}\n\n";
        if (upf) o << "#define UPF_RT_NO_DESIGN\n#include \"upf_rt.inc\"\n\n";   // msim -upf: the power-aware runtime
        o << "int main(int argc, char** argv) {\n"
             "    uint64_t maxTime = ~0ULL;\n"
             "    const char* covOut = nullptr;\n"
          << (upf ? "    const char* upfSnap = \"upf.snap\";\n" : "") <<
             "    S.vcdPath = \"\";\n"
             "    timeFmt().tickExp = timeFmt().units = " << precExp << ";\n"
             "    S.timescale = " << quote(timescale) << ";\n"
             "    S.threads = defaultThreads();\n"
             "    for (int i = 1; i < argc; i++) {\n"
             "        if (!std::strcmp(argv[i], \"-maxtime\") && i + 1 < argc) maxTime = std::strtoull(argv[++i], nullptr, 10);\n"
             "        else if (!std::strcmp(argv[i], \"-vcd\") && i + 1 < argc) S.vcdPath = argv[++i];\n"
             "        else if (!std::strcmp(argv[i], \"-vcdtop\") && i + 1 < argc) S.vcdTop = argv[++i];\n"
             "        else if (!std::strcmp(argv[i], \"-vcd-arrays\") && i + 1 < argc) S.dumpArrays = std::strtoull(argv[++i], nullptr, 10);\n"
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
          << (upf ? "        else if (!std::strcmp(argv[i], \"-upf_snap\") && i + 1 < argc) upfSnap = argv[++i];\n" : "") <<
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
        if (upf) {   // the power intent msim bound to the design (upf_rt.inc Rt::writeSnapshot)
            o << "    static Task (*const UPFPROC[])() = {";   // sim_replay_control runs these again
            for (size_t i = 0; i < procs.size(); i++)
                o << (i ? ", " : "") << (procs[i].combRank < 0 && !procs[i].seq && std::find(dom.begin(), dom.end(), (int)i) == dom.end() ? "proc" + std::to_string(i) : std::string("nullptr"));
            o << "};\n    upfrt::rt().procFns = UPFPROC;\n";
        }
        if (upf)
            o << "    try {\n"
                 "        if (!upfrt::rt().readSnapshot(upfSnap)) { std::cerr << \"simv: cannot read \" << upfSnap << \"\\n\"; return 1; }\n"
                 "    } catch (const std::exception& ex) { std::cerr << \"simv: UPF: \" << ex.what() << \"\\n\"; return 1; }\n";
        o << initText << initCalls;
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
            if (!coneIns_.empty()) {   // the clock cone runs first, on the clocks alone
                o << "    S.addComb(combCone, nullptr, {";
                for (size_t j = 0; j < coneIns_.size(); j++) o << (j ? ", " : "") << coneIns_[j];
                o << "});\n";
                std::erase_if(ins, [&](int x) {
                    return std::binary_search(coneIns_.begin(), coneIns_.end(), x) || std::binary_search(coneOuts_.begin(), coneOuts_.end(), x);
                });
            }
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
        // Declaration initializers take effect before anything waits (6.8):
        // logic clk = 0 is no negedge for a clocked process registered below.
        auto isInit = [&](size_t i) { return procs[i].combRank < 0 && !procs[i].seq && procs[i].kind == "init"; };
        for (size_t i = 0; i < procs.size(); i++)
            if (isInit(i)) o << "    { Task t = proc" << i << "(); S.all.push_back(t.h); t.h.resume(); }\n";
        // A waveform opened at run time starts with the initialized values.
        if (!dom.empty()) o << "    if (!S.vcdPath.empty()) syncInit();\n";
        if (!dom.empty()) {
            o << "    S.fastSeqId = (int)S.seqs.size();\n    S.addSeq(dseq, nullptr, {";
            // Every event of every domain process: they wait on copies of the
            // same clocks (see cycleDomain), and one pending mark runs dseq once.
            for (int g = 0; g < (int)domWake_.size(); g++) {
                const auto& ev = domWake_[g];
                if (g) o << "    S.addSeq(dseq, (const void*)(intptr_t)" << g << ", {";
                for (size_t j = 0; j < ev.size(); j++)
                    o << (j ? ", " : "") << "{" << ev[j].first << ", " << edge(ev[j].second) << "}";
                o << "});\n";
            }
            o << "    S.fastCommit = fcommit;\n";
        }
        {   // clocked functions and coroutine processes, in process order
            std::string evs, tab;
            size_t off = 0, n = 0;
            for (size_t i = 0; i < procs.size(); i++) {
                if (procs[i].combRank >= 0) continue;
                if (std::find(dom.begin(), dom.end(), (int)i) != dom.end() || isInit(i)) continue;
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
        if (cov) {
            o << "    g_condHits.assign(" << g_condHits.size() << ", 0);\n    S.fsmWatch({";
            for (size_t k = 0; k < fsmDefs().size(); k++) o << (k ? ", " : "") << fsmDefs()[k].sig;
            o << "}, {";
            for (size_t k = 0; k < fsmDefs().size(); k++) o << (k ? ", " : "") << (fsmDefs()[k].onehot ? 1 : 0);
            o << "});\n";
        }
        if (upf) o << "    upfrt::rt().start();\n";
        o << "    S.run(maxTime);\n"
             "    std::cout.flush();\n";
        if (upf)
            o << "    if (!upfrt::rt().covPath.empty() && !upfrt::rt().writeCoverage()) {\n"
                 "        std::cerr << \"simv: cannot write \" << upfrt::rt().covPath << \"\\n\";\n"
                 "        return 1;\n"
                 "    }\n";
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
    // msim -upf: simv runs the power-aware runtime (upf_rt.inc) from msim's
    // snapshot; a process of a power domain (Prog::pd >= 0) sleeps while the
    // domain is off and corrupts its writes as its simstate says. Off, the
    // generated code is the same as without this option.
    bool upf = false;
    // Build in up to this many C++ files (in parallel): a large design as one
    // file took minutes. Small designs stay one file. Negative: -files files
    // whatever the size (msim -cfiles).
    int files = 1;
    // One kernel tick = 10^precExp s (the finest `timescale precision).
    // Delays are already in ticks; simv needs it for %t, $timeformat and
    // the waveform header.
    int precExp = -9;
    std::string timescale = "1ns";
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
    // JmpIfNot on the Val expression `val`: `no` is the statement that jumps
    // when it is not true, `unk` the one for an unknown value (Instr::xt).
    static std::string jmpIfNot(const Instr& i, const std::string& val, const std::string& no, const std::string& unk) {
        if (i.xt == Instr::NOX) return " if (truth(" + val + ") != 1) " + no + "\n";
        return " switch (truth(" + val + ")) { case 1: break; case 2: " + unk + " default: " + no + " }\n";
    }

private:
    std::vector<int> sigW_;
    std::vector<std::string> sigName_;
    // Temporaries of the lowering and of msim's call expansion (_tf...): each
    // read follows a write in the same pass, so an X left in one from a pass
    // that did not use it says nothing about 2-state mode.
    std::vector<char> scratch_;
    bool scratch(int s) const { return s >= 0 && (size_t)s < scratch_.size() && scratch_[(size_t)s]; }
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
    // Found while emitting: a private signal used where only S.sigs works (an
    // interpreted operator, an assertion). run() ends with CodegenRetry and
    // the next pass keeps these in S.sigs (noPriv).
public:
    std::set<int> noPriv, privHits_;
private:
    void hitPriv(const E& e) {
        if (!e) return;
        if (e->op == Op::Ref && isPriv(e->sig)) privHits_.insert(e->sig);
        for (auto& k : e->kids) hitPriv(k);
    }
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
    std::string dynTarget(const Instr& i, const std::string& val, const std::string& then, bool privOk = false) {
        if (isPriv(i.sig) && !privOk) privHits_.insert(i.sig);
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
        // A real keeps its double in v. A string is an id into strPool, which
        // simv fills in its own order: the text is looked up once, at startup.
        if (k.str)
            return "([]{ static const Val v = [] { Val s = mkstr(" + quote(v_str(k)) + "); s.w = " + std::to_string(k.w) +
                   "; s.sgn = " + (k.sgn ? "true" : "false") + "; return s; }(); return v; }())";
        if (k.real)
            return "([]{ Val v{" + hex(k.v) + ", " + hex(k.x) + ", " + std::to_string(k.w) + ", " + hex(k.z) + ", " +
                   (k.sgn ? "true" : "false") + "}; v.real = true; return v; }())";
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
        char b[96];
        // flags: 1 signed, 2 real (the double in v), 4 string (the text follows the limbs)
        std::snprintf(b, sizeof b, "%d %llx %llx %d %llx %d %zu", (int)e->op, (unsigned long long)k.v, (unsigned long long)k.x,
                      k.w, (unsigned long long)k.z, (int)k.sgn | (k.real ? 2 : 0) | (k.str ? 4 : 0), k.hi.size());
        t += b;
        if (k.str) { const std::string& tx = v_str(k); t += " " + std::to_string(tx.size()) + " " + tx; }
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
            case Op::CondCov: {   // condition coverage: every term, then the counters (expr.hpp)
                if (e->lo & 2) {   // lazy: each term a callable, run in order
                    std::string r = "condCovLazy(" + std::to_string(e->hi) + ", " + (e->lo & 1 ? "true" : "false");
                    for (size_t i = 0; i < e->kids.size(); i++) r += ", [&]() -> Val { return " + ex(e->kids[i]) + "; }";
                    return r + ")";
                }
                std::string r = "condCovVal(" + std::to_string(e->hi) + ", " + std::to_string(e->lo & 5) + ", {";
                for (size_t i = 0; i < e->kids.size(); i++) r += (i ? ", " : "") + ex(e->kids[i]);
                return r + "})";
            }
            case Op::RedAnd: return f1("v_red_and");
            case Op::RedOr: return f1("v_red_or");
            case Op::RedXor: return f1("v_red_xor");
            case Op::Slice: return "v_slice(" + k(0) + ", " + std::to_string(e->hi) + ", " + std::to_string(e->lo) + ")";
            case Op::Index: return f2("v_index");
            case Op::Concat: {
                if (e->kids.empty()) return "Val{0, 0, 0}";
                // A run of the same operand ({32{a[0]}} lowered bit by bit) is computed once.
                std::string r = "CAT(";
                for (size_t i = 0; i < e->kids.size();) {
                    size_t j = i + 1;
                    while (j < e->kids.size() && pureEq(e->kids[i], e->kids[j])) j++;
                    std::string t = ex(e->kids[i]);
                    r += (i ? ", " : "") + (j - i > 1 ? "REP(" + t + ", " + std::to_string(j - i) + ")" : t);
                    i = j;
                }
                return r + ")";
            }
            case Op::Mux: return "v_mux(" + k(0) + ", " + k(1) + ", " + k(2) + ")";
            case Op::WildEq: return f2("v_wild_eq");
            case Op::SysFn:
                if (e->hi == SF_TO_2STATE) return "v_to_2state(" + k(0) + ")";
                break;
            default: break;
        }
        // Any other operator (==?, real math, strings, system functions, ...):
        // the interpreter's eval() on the subtree, rebuilt at startup.
        hitPriv(e);
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
    // The same side-effect-free expression twice (signals, constants, selects, concatenations).
    static bool pureEq(const E& a, const E& b) {
        if (!a || !b) return false;
        if (a->op != b->op || a->kids.size() != b->kids.size()) return false;
        switch (a->op) {
            case Op::Ref: if (a->sig != b->sig || a->sgn != b->sgn || a->at || b->at) return false; break;
            case Op::Const: if (!(a->k == b->k) || a->k.w != b->k.w || a->k.sgn != b->k.sgn || a->k.real || a->k.str) return false; break;
            case Op::Slice: if (a->hi != b->hi || a->lo != b->lo) return false; break;
            case Op::Concat: break;
            default: return false;
        }
        for (size_t i = 0; i < a->kids.size(); i++) if (!pureEq(a->kids[i], b->kids[i])) return false;
        return true;
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
            case Op::WildEq: {   // a ==? pattern: the pattern's X and Z bits match anything
                if (e->kids.size() != 2 || !e->kids[1] || e->kids[1]->op != Op::Const) return false;
                const Val& pk = e->kids[1]->k;
                if (pk.w > 64 || pk.w < 1 || pk.real || pk.str || !pk.hi.empty()) return false;
                if (!kid(0, A, fa) || fa.w != pk.w) return false;
                out = "(uint64_t)(((" + A + " ^ " + hex(pk.v) + ") & " + hex(~pk.x & mask(pk.w)) + ") == 0)";
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
            case Op::SysFn:   // a 2-state conversion: these values have no x or z bits
                if (e->hi != SF_TO_2STATE || !kid(0, out, fi)) return false;
                return true;
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
            case Op::CondCov: {
                if (e->lo & 2) return false;   // lazy: the Val path
                std::string r = "condCovFast(" + std::to_string(e->hi) + ", " + std::to_string(e->lo & 5) + ", {";
                for (size_t i = 0; i < e->kids.size(); i++) {
                    std::string P;
                    FastInfo fp{};
                    if (!fx(e->kids[i], P, fp, refs)) return false;
                    r += (i ? ", " : "") + ("(int)(" + P + " != 0)");
                }
                out = r + "})";
                fi = {1, false};
                return true;
            }
            case Op::RedAnd: case Op::RedOr: case Op::RedXor: {
                // Over a concatenation (often wider than 64 bits: a parity
                // over several registers) the reduction goes part by part.
                const E& c = e->kids.empty() ? e : e->kids[0];
                if (c && c->op == Op::Concat && c->kids.size() > 1) {
                    std::string r;
                    bool ok = true;
                    for (auto& ck : c->kids) {
                        std::string P;
                        FastInfo fp{};
                        if (!fx(ck, P, fp, refs)) { ok = false; break; }
                        std::string t = e->op == Op::RedAnd ? "(" + P + " == " + M(fp.w) + ")" : e->op == Op::RedOr ? "(" + P + " != 0)" : P;
                        r += (r.empty() ? "" : e->op == Op::RedAnd ? " && " : e->op == Op::RedOr ? " || " : " ^ ") + t;
                    }
                    if (ok) {
                        out = e->op == Op::RedXor ? "(uint64_t)__builtin_parityll(" + r + ")" : "(uint64_t)(" + r + ")";
                        fi = {1, false};
                        return true;
                    }
                }
                if (!kid(0, A, fa)) return false;
                out = e->op == Op::RedAnd ? "(uint64_t)(" + A + " == " + M(fa.w) + ")"
                    : e->op == Op::RedOr ? "(uint64_t)(" + A + " != 0)" : "(uint64_t)__builtin_parityll(" + A + ")";
                fi = {1, false};
                return true;
            }
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
                for (size_t i = 0; i < e->kids.size();) {
                    std::string P;
                    FastInfo fp{};
                    if (!fx(e->kids[i], P, fp, refs)) return false;
                    size_t j = i + 1;   // a run of the same one-bit operand: all ones or all zeros
                    if (fp.w == 1) while (j < e->kids.size() && pureEq(e->kids[i], e->kids[j])) j++;
                    int pw = fp.w * (int)(j - i);
                    if (j - i > 1) P = "(((uint64_t)0 - (" + P + ")) & " + M(pw) + ")";
                    w += pw;
                    if (w > 64) return false;
                    r = i ? "((" + r + " << " + std::to_string(pw) + ") | " + P + ")" : P;
                    i = j;
                }
                out = r;
                fi = {w, false};
                return true;
            }
            case Op::ArrRead: {   // a[i] of a one-dimensional array in S.sigs: X when i is outside it or the element is unknown
                const ArrShape* a = e->arr.get();
                if (!a || a->base < 0 || a->dynElem || a->dims.size() != 1 || e->kids.size() != 1 || a->elemW < 1 || a->elemW > 64 ||
                    a->count < 1 || a->twoState)
                    return false;
                for (int k = 0; k < a->count; k++)
                    if (isPriv(a->base + k) || a->base + k >= (int)sigW_.size() || sigW_[a->base + k] != a->elemW) return false;
                if (!kid(0, A, fa)) return false;
                auto [l, r] = a->dims[0];
                std::string off = l <= r ? "((uint64_t)(" + A + ") - (uint64_t)" + std::to_string(l) + "LL)"
                                         : "((uint64_t)" + std::to_string(l) + "LL - (uint64_t)(" + A + "))";
                std::string at = "S.sigs[" + std::to_string(a->base) + " + (" + off + " < " + std::to_string(a->count) + " ? " + off + " : 0)].val";
                out = "(" + at + ".v)";
                fxOor_.push_back("(" + off + " >= " + std::to_string(a->count) + " || " + at + ".x)");
                fi = {a->elemW, e->sgn};
                return true;
            }
            case Op::DynSlice: {   // a[i +: w]: X when any of it is outside a
                if (e->lo || e->kids.size() != 2) return false;
                if (!kid(0, A, fa) || !kid(1, B, fb)) return false;
                int w = e->hi;
                if (w < 1 || w > fa.w) return false;
                out = "((" + A + " >> ((" + B + ") & 63)) & " + M(w) + ")";
                fxOor_.push_back("(uint64_t)(" + B + ") > " + std::to_string(fa.w - w));
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
        fxOor_.clear();
        if (!fx(e, u64, fi, refs)) return false;
        // A run-time select that may fall outside its vector (the result is
        // then X): only where the caller tests the guard (dynOk_).
        if (!fxOor_.empty() && !dynOk_) return false;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        guard = g.empty() ? "" : "(" + g + ") == 0";
        for (auto& c : fxOor_) guard += (guard.empty() ? "" : " && ") + ("!(" + c + ")");
        return true;
    }
    std::vector<std::string> fxOor_;   // fx: conditions under which the 2-state value does not hold
    bool dynOk_ = false;
    std::string fast(const E& e) {
        std::string slow = ex(e);
        if (!twoState) return slow;
        std::string f;
        FastInfo fi{};
        std::vector<int> refs;
        fxOor_.clear();
        if (!fx(e, f, fi, refs)) return slow;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        if (!g.empty()) g = "(" + g + ") == 0";
        for (auto& c : fxOor_) g += (g.empty() ? "" : " && ") + ("!(" + c + ")");
        std::string val = "Val{" + f + ", 0, " + std::to_string(fi.w) + ", 0, " + (fi.sgn ? "true" : "false") + "}";
        if (g.empty()) return val;
        return "(" + g + " ? " + val + " : " + slow + ")";
    }

    // The write of an Assign, with `val` as the value (default: its expression).
    std::string assign(const Instr& i, std::string val = "") {
        if (val.empty()) val = fast(i.e);
        std::ostringstream o;
        if (i.tmp) {   // a temporary of the process: nothing watches it
            o << " S.poke(" << i.sig << ", " << val << ", " << i.hi << ", " << i.lo << ");\n";
            return o.str();
        }
        if (isPriv(i.sig)) {   // a testbench write to a private reg: the comb logic may read it
            if (i.drv >= 0 || i.hi >= 0) { privHits_.insert(i.sig); return " ;\n"; }
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
    // A deferred assertion inside a static combinational process: its
    // report is queued under the process's own ProcCtl (CC<n>).
    std::string combAssert(int n, const Instr& i) {
        const AssertInfo& a = asserts().at(i.sig);
        hitPriv(i.e);
        for (auto* cs : {&a.passCap, &a.failCap})
            for (auto& c : *cs) hitPriv(c);
        asrts_.push_back(&i);
        return " runAssert(AI" + std::to_string(asrts_.size() - 1) + ", CC" + std::to_string(n) + ", 0);\n";
    }
    static bool combAsserts(const Prog& p) {
        for (auto& i : p.code) {
            if (i.k == IK::WaitEdge) return false;
            if (i.k == IK::Assert) return true;
        }
        return false;
    }
    // Splits generated code into its top-level items: function definitions
    // (to `fns`, one each, still "static") and the rest (to `hdr`, with each
    // "static" variable made "inline" so that every file shares it). Forward
    // declarations of functions are dropped: the split writes prototypes.
    static void splitTop(const std::string& t, std::vector<std::string>& fns, std::string& hdr) {
        size_t start = 0, i = 0, n = t.size();
        int depth = 0;
        size_t brace = std::string::npos;   // the first '{' at depth 0 of the item
        bool eq = false;                    // an '=' at depth 0 before it
        auto item = [&](size_t end) {       // [start, end) ends a top-level item
            size_t s0 = start;
            std::string it = t.substr(start, end - start);
            start = end;
            size_t b = it.find_first_not_of(" \t\n");
            if (b == std::string::npos) { hdr += it; return; }
            std::string lead = it.substr(0, b);
            std::string body = it.substr(b);
            // Comment lines before the item stay with it.
            size_t at = 0;
            while (body.compare(at, 2, "//") == 0) { size_t nl = body.find('\n', at); if (nl == std::string::npos) break; at = nl + 1; }
            std::string com = body.substr(0, at), rest = body.substr(at);
            bool isStatic = rest.compare(0, 7, "static ") == 0;
            size_t rb = brace == std::string::npos ? std::string::npos : brace - s0 - b - at;
            if (isStatic && rb != std::string::npos && !eq) {
                size_t p = rest.find_last_not_of(" \t\n", rb - 1);
                if (p != std::string::npos && rest[p] == ')') { fns.push_back(lead + com + rest); return; }
            }
            if (isStatic && rb == std::string::npos && !eq) {
                size_t p = rest.find_last_not_of(" \t\n;");
                if (p != std::string::npos && rest[p] == ')') { hdr += lead + com; return; }   // a declaration
            }
            if (isStatic) rest.replace(0, 7, "inline ");
            hdr += lead + com + rest;
        };
        while (i < n) {
            char c = t[i];
            if (c == '/' && i + 1 < n && t[i + 1] == '/') { size_t nl = t.find('\n', i); i = nl == std::string::npos ? n : nl; continue; }
            if (c == '#' && depth == 0 && (i == 0 || t[i - 1] == '\n')) {   // a preprocessor line
                size_t nl = t.find('\n', i);
                i = nl == std::string::npos ? n : nl + 1;
                std::string pre = t.substr(start, i - start);
                hdr += pre; start = i;
                continue;
            }
            if (c == '"' || c == '\'') {
                for (i++; i < n && t[i] != c; i++) if (t[i] == '\\') i++;
                i++;
                continue;
            }
            if (c == '{') { if (depth == 0 && brace == std::string::npos) brace = i; depth++; }
            else if (c == '}') depth--;
            else if (c == '=' && depth == 0 && brace == std::string::npos) eq = true;
            i++;
            if (depth == 0 && (c == ';' || (c == '}' && !eq))) {
                // A function ends with its '}'; a variable with ';' after its '}'.
                while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
                if (i < n && t[i] == ';') i++;
                if (i < n && t[i] == '\n') i++;
                item(i);
                brace = std::string::npos;
                eq = false;
            }
        }
        if (start < n) hdr += t.substr(start);
    }
    int ccMax_ = 0;
    std::string combCtlDecl(int n) {
        ccMax_ = std::max(ccMax_, n);
        return "static const std::shared_ptr<ProcCtl>& CC" + std::to_string(n) + " = (CCA[" + std::to_string(n) + "] = std::make_shared<ProcCtl>());\n";
    }
    static bool oneAssign(const Prog& p) {
        return p.code.size() >= 2 && p.code[0].k == IK::Assign && p.code[1].k == IK::WaitEdge;
    }
    // A static combinational process with if / case (prog.hpp: combShape).
    static bool combBranches(const Prog& p) {
        for (auto& i : p.code) {
            if (i.k == IK::WaitEdge) return false;
            if (i.k == IK::JmpIfNot || i.k == IK::Jmp || i.k == IK::Assert) return true;
        }
        return false;
    }

    // The cycle domain: the largest group of clocked processes that wait on
    // the same events, if no other process makes an NBA to one of their NBA
    // targets (a blocking write, as a testbench's reset, is fine: it is not
    // ordered against the NBA queue) and every jump in them goes forward (so each NBA runs at most
    // once and can own a fixed slot). Empty: no domain.
    std::vector<int> cycleDomain(const std::vector<CodegenProc>& procs, const std::vector<CodegenSig>& sigs) {
        // The clock a process waits on is often a copy of the top clock made
        // by the port connections on the way down (assign u.clk_i = clk).
        // Copies change in the same comb pass as their source, so processes
        // that wait on copies of the same signals belong to one domain.
        std::vector<int> root(sigs.size());
        for (size_t x = 0; x < root.size(); x++) root[x] = (int)x;
        {
            std::vector<int> writers(sigs.size(), 0);
            for (auto& pr : procs) {
                std::vector<const Prog*> progs;
                reach(pr.prog, progs);
                for (const Prog* p : progs)
                    for (auto& in : p->code)
                        if ((in.k == IK::Assign || in.k == IK::Nba || in.k == IK::Force) && in.sig >= 0 && (size_t)in.sig < writers.size())
                            writers[in.sig] += in.k == IK::Force ? 2 : 1;
            }
            for (auto& pr : procs) {
                if (pr.combRank < 0 || !oneAssign(*pr.prog)) continue;
                const Instr& a = pr.prog->code[0];
                if (a.k != IK::Assign || a.hi >= 0 || a.arr || a.bidx || !a.aidx.empty() || !a.e || a.e->op != Op::Ref || a.e->at) continue;
                int d = a.sig, src = a.e->sig;
                if (d < 0 || src < 0 || (size_t)d >= sigs.size() || (size_t)src >= sigs.size()) continue;
                if (writers[d] != 1 || sigs[d].w != 1 || sigs[src].w != 1 || sigs[d].drivers > 1) continue;
                root[d] = src;
            }
        }
        auto find = [&](int x) {
            for (int n = 0; x >= 0 && (size_t)x < root.size() && root[x] != x && n < 1000; n++) x = root[x];
            return x;
        };
        auto canon = [&](const std::vector<std::pair<int, Edge>>& ev) {
            std::vector<std::pair<int, Edge>> c;
            for (auto& [sg, ed] : ev) c.push_back({find(sg), ed});
            std::sort(c.begin(), c.end());
            return c;
        };
        auto raw = [&](const std::vector<std::pair<int, Edge>>& ev) {
            for (auto& [sg, ed] : ev) if (find(sg) != sg) return false;
            return true;
        };
        domRoot_.resize(sigs.size());
        for (size_t x = 0; x < sigs.size(); x++) domRoot_[x] = find((int)x);
        std::vector<std::vector<int>> groups;
        std::set<int> fixedNba;   // fixed NBA targets of every clocked process
        for (auto& pr : procs)
            if (pr.seq && pr.combRank < 0)
                for (auto& in : pr.prog->code) if (in.k == IK::Nba && !in.arr && in.aidx.empty()) fixedNba.insert(in.sig);
        for (size_t i = 0; i < procs.size(); i++) {
            if (!procs[i].seq || procs[i].combRank >= 0) continue;
            bool forward = true;
            const char* why = "";
            int whyLine = 0;
            const auto& code = procs[i].prog->code;
            for (size_t pc = 1; pc + 1 < code.size(); pc++) {
                if ((code[pc].k == IK::Jmp || code[pc].k == IK::JmpIfNot) && code[pc].target <= pc) { forward = false; why = "loop"; whyLine = code[pc].line; }
                if (code[pc].k == IK::JmpIfNot && code[pc].xt != Instr::NOX) { forward = false; why = "three-way jump"; whyLine = code[pc].line; }   // a three-way jump: an ordinary clocked function
                // A temporary of the process (msim.cpp: localizeSeqBlocking) is a plain store.
                const bool tmpw = code[pc].k == IK::Assign && code[pc].tmp && code[pc].drv < 0 && !code[pc].arr && code[pc].aidx.empty();
                if (code[pc].k == IK::Assign && !tmpw) {
                    forward = false; why = "blocking assign"; whyLine = code[pc].line;
                }
                // An array element chosen at run time: its write goes to the
                // kernel's NBA list (dseq's nb, before the slots commit), when
                // no fixed slot of the domain writes an element of that array.
                if (code[pc].k == IK::Nba && code[pc].arr && !code[pc].bidx && code[pc].drv < 0 && code[pc].dyn < 0 && !code[pc].nbaLater &&
                    !code[pc].de && !code[pc].delay && !std::getenv("MSIM_NODOMARR")) {
                    bool clash = false;
                    for (int64_t k = 0; k < code[pc].arr->count; k++) clash |= fixedNba.count((int)(code[pc].arr->base + k)) > 0;
                    if (clash) { forward = false; why = "array also written at a fixed index"; whyLine = code[pc].line; }
                } else if (!tmpw && (code[pc].arr || code[pc].bidx)) { forward = false; why = "run-time index"; whyLine = code[pc].line; }   // no fixed slot: an ordinary clocked function
            }
            if (!forward) {
                if (std::getenv("MSIM_WHYCYCLE")) std::fprintf(stderr, "not in the domain (%s, line %d): %s line %d\n", why, whyLine, procs[i].kind.c_str(), code.empty() ? 0 : code[0].line);
                continue;
            }
            // A process that waits on the source signals themselves runs when
            // they change; one that waits on copies runs after the comb pass
            // that brings the copies (and anything else computed from the
            // sources, as a reset it reads) up to date. Two groups, so each
            // still runs at its own point of the time step.
            bool placed = false;
            for (auto& g : groups)
                if (canon(procs[g[0]].seqEv) == canon(procs[i].seqEv) && raw(procs[g[0]].seqEv) == raw(procs[i].seqEv)) { g.push_back((int)i); placed = true; break; }
            if (!placed) groups.push_back({(int)i});
        }
        std::vector<int> best;
        for (auto& g : groups) if (g.size() > best.size()) best = g;
        if (std::getenv("MSIM_WHYCYCLE"))
            for (auto& g : groups) {
                std::string ev;
                for (auto& [sg, ed] : canon(procs[g[0]].seqEv)) ev += " " + sigs[sg].name + "/" + std::to_string((int)ed);
                std::fprintf(stderr, "clock group of %zu:%s%s\n", g.size(), ev.c_str(), raw(procs[g[0]].seqEv) ? "" : " (through copies)");
            }
        domGroup_.assign(procs.size(), -1);
        domGroups_ = 0;
        if (best.empty()) return best;
        // No other process may make an NBA to a target of the domain.
        auto clash = [&](const std::vector<int>& d) {
            std::vector<char> mine(procs.size(), 0);
            for (int i : d) mine[i] = 1;
            std::vector<int> targets;
            for (int i : d)
                for (auto& in : procs[i].prog->code) if (in.k == IK::Nba) targets.push_back(in.sig);
            std::sort(targets.begin(), targets.end());
            for (size_t i = 0; i < procs.size(); i++) {
                if (mine[i]) continue;
                std::vector<const Prog*> progs;
                reach(procs[i].prog, progs);
                for (const Prog* p : progs)
                    for (auto& in : p->code)
                        if (in.k == IK::Nba && std::binary_search(targets.begin(), targets.end(), in.sig)) return true;
            }
            return false;
        };
        // Every group (each wakes on its own events, see dseq), when there
        // are few; else, or when that clashes, the largest one alone.
        std::vector<int> all;
        // (A multi-core SoC has a gated clock per core: a few groups per core.)
        if (groups.size() > 1 && groups.size() <= 1024 && !std::getenv("MSIM_ONEGROUP")) {
            for (auto& g : groups) all.insert(all.end(), g.begin(), g.end());
            std::sort(all.begin(), all.end());
            if (clash(all)) all.clear();
        }
        if (!all.empty()) {
            for (size_t g = 0; g < groups.size(); g++)
                for (int i : groups[g]) domGroup_[i] = (int)g;
            domGroups_ = (int)groups.size();
            return all;
        }
        if (clash(best)) return {};
        for (int i : best) domGroup_[i] = 0;
        domGroups_ = 1;
        return best;
    }

    // ---- shared comb code (one copy per module, like Verilator) ----
    // The instances of one module get their signals at a constant distance
    // (c1.cpu.x = c0.cpu.x + D): the comb statements of one instance are those
    // of another with every signal number moved by D. refScan finds the
    // signal numbers in generated text (S.sigs[N], R(N, ...), P<N>, ...) and
    // calls f(at, len, N) for each.
    template <class F> static void refScan(const std::string& t, F&& f) {
        static const char* pre[] = {"S.sigs[", "S.waiters[", "S.fwSlow(", "S.fwdv(", "S.fwv(", "S.fw(", "S.drive(",
                                    "S.get(", "S.set(", "S.poke(", "R(", "int s = "};   // the last: dynTarget
        auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        for (size_t i = 0; i < t.size(); i++) {
            char c = t[i];
            if (c != 'S' && c != 'R' && c != 'P' && c != 'A' && c != 'V' && c != 'H' && c != 'F' && c != 'C' && c != 'i') continue;
            if (i > 0 && (idc(t[i - 1]) || t[i - 1] == '.')) continue;
            size_t at = std::string::npos;
            int kind = 0;   // 0 a signal number, 1 a private signal P<N>, 2 an array shape AS<N>, 3 an NBA slot
            if (c == 'C' && t.compare(i, 2, "CC") == 0 && i + 2 < t.size() && std::isdigit((unsigned char)t[i + 2])) {
                at = i + 2, kind = 5;
            } else if (c == 'V' || c == 'H' || c == 'F' || c == 'C') {
                for (const char* p : {"VA[", "HA[", "FA[", "CLP(", "CIVA[", "CIP["}) {
                    size_t n = std::strlen(p);
                    if (t.compare(i, n, p) == 0 && i + n < t.size() && std::isdigit((unsigned char)t[i + n])) {
                        at = i + n, kind = p[1] == 'I' ? 7 : 3;
                        break;
                    }
                }
            } else if (c == 'P') {
                if (i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1])) at = i + 1, kind = 1;
            } else if (c == 'A') {
                if (t.compare(i, 2, "AS") == 0 && i + 2 < t.size() && std::isdigit((unsigned char)t[i + 2])) at = i + 2, kind = 2;
                else if (t.compare(i, 2, "AI") == 0 && i + 2 < t.size() && std::isdigit((unsigned char)t[i + 2])) at = i + 2, kind = 4;
            } else
                for (const char* p : pre) {
                    size_t n = std::strlen(p);
                    if (t.compare(i, n, p) == 0 && i + n < t.size() && std::isdigit((unsigned char)t[i + n])) { at = i + n; break; }
                }
            if (at == std::string::npos) continue;
            size_t e = at;
            while (e < t.size() && std::isdigit((unsigned char)t[e])) e++;
            if ((kind == 1 || kind == 2 || kind == 4 || kind == 5) && e < t.size() && idc(t[e])) continue;
            f(i, at, e, std::strtoll(t.c_str() + at, nullptr, 10), kind);
            i = e - 1;
        }
    }
    // Signals made after elaboration come in ranges of their own, each with
    // its own distance between instances: function copies (_tfc), @* arming
    // flags ($atstar), other function scratch (_tf) and the pieces of wide
    // signals (numbers past the signal table).
    // 6: an NBA slot of the cycle domain (refToks moves them past kSlotRef).
    // 7, 8: an assertion (AI<k>) and its process (CC<n>, CIVA[n], CIP[n]).
    static constexpr int kRefCls = 9;
    static constexpr long long kSlotRef = 1LL << 40, kAIRef = 2LL << 40, kCCRef = 3LL << 40;
    // -1: a signal outside the instance `key` (a top-level clock, say): the
    // same number in every copy. 5: an array shape (refToks makes them negative).
    int refCls(long long n, const std::string& key) const {
        if (n < 0) return 5;
        if (n >= kCCRef) return 8;
        if (n >= kAIRef) return 7;
        if (n >= kSlotRef) return 6;
        if ((size_t)n >= sigName_.size()) return 4;
        const std::string& m = sigName_[(size_t)n];
        if (m.rfind("_tfc", 0) == 0) return 1;
        if (m.rfind("$atstar", 0) == 0) return 2;
        if (m.rfind("_tf", 0) == 0) return 3;
        if (m.size() <= key.size() || m.compare(0, key.size(), key) != 0 || m[key.size()] != '.') return -1;
        return 0;
    }
    // Text between the signal numbers, with labels (cb<proc>_, cq<proc>_)
    // and cold counters made neutral.
    static std::string refPiece(const std::string& r) {
        std::string o;
        o.reserve(r.size());
        for (size_t i = 0; i < r.size(); i++) {
            if ((r.compare(i, 2, "cb") == 0 || r.compare(i, 2, "cq") == 0) && (i == 0 || !std::isalnum((unsigned char)r[i - 1])) &&
                i + 2 < r.size() && std::isdigit((unsigned char)r[i + 2])) {
                size_t e = i + 2;
                while (e < r.size() && std::isdigit((unsigned char)r[e])) e++;
                if (e < r.size() && r[e] == '_') { o += r.substr(i, 2) + "#"; i = e - 1; continue; }
            }
            if (r.compare(i, 5, "COLD(") == 0) {
                size_t e = i + 5;
                while (e < r.size() && std::isdigit((unsigned char)r[e])) e++;
                o += "COLD(#"; i = e - 1; continue;
            }
            o += r[i];
        }
        return o;
    }
    struct RefToks { std::vector<std::string> piece; std::vector<long long> num; };
    static RefToks refToks(const std::string& t) {
        RefToks r;
        size_t last = 0;
        refScan(t, [&](size_t, size_t at, size_t e, long long n, int kind) {
            r.piece.push_back(refPiece(t.substr(last, at - last)));
            r.num.push_back(kind == 2 ? -1 - n : kind == 3 ? kSlotRef + n : kind == 4 ? kAIRef + n : kind == 5 || kind == 7 ? kCCRef + n : n);   // array shapes, slots: classes of their own
            last = e;
        });
        r.piece.push_back(refPiece(t.substr(last)));
        return r;
    }
    // Is g the template t with every signal number moved by D[its class]?
    // Fills the classes not set yet.
    bool refMatch(const std::string& t, const std::string& g, const std::string& tk, const std::string& gk, long long* D, bool* set) const {
        RefToks a = refToks(t), b = refToks(g);
        if (a.num.size() != b.num.size() || a.piece != b.piece) return false;
        for (size_t k = 0; k < a.num.size(); k++) {
            int c = refCls(a.num[k], tk);
            if (c != refCls(b.num[k], gk)) return false;
            long long d = c == 5 ? a.num[k] - b.num[k] : b.num[k] - a.num[k];   // shapes are stored as -1 - n
            if (c < 0) { if (d) return false; continue; }
            if (!set[c]) { set[c] = true; D[c] = d; }
            else if (D[c] != d) return false;
        }
        return true;
    }
    // The text with every signal number N written as (N + D<class>).
    std::string refParam(const std::string& t, const std::string& key) const {
        std::string r;
        size_t last = 0;
        refScan(t, [&](size_t start, size_t at, size_t e, long long n, int kind) {
            if (kind == 2) { r.append(t, last, start - last); r += "(*AST[" + std::to_string(n) + " + D5])"; last = e; return; }
            if (kind == 4 || kind == 5) {
                r.append(t, last, start - last);
                r += (kind == 4 ? "AIA[" : "CCA[") + std::to_string(n) + (kind == 4 ? " + D7]" : " + D8]");
                last = e;
                return;
            }
            int c = kind == 3 ? 6 : kind == 7 ? 8 : refCls(n, key);
            if (c < 0) return;   // the same in every copy: left as it is
            std::string d = "D" + std::to_string(c);
            if (kind == 1) { r.append(t, last, start - last); r += "PA[" + std::to_string(n) + " + " + d + "]"; }
            else { r.append(t, last, at - last); r += std::to_string(n) + " + " + d; }
            last = e;
        });
        r.append(t, last, std::string::npos);
        return r;
    }
    // Emits the statements js (indices into text, in rank order) as functions
    // and returns the calls that run them in order. Within one level the
    // statements of each instance are kept together; an instance whose
    // statements equal another's moved by D calls that one's functions with
    // D. MSIM_NOSHARE=1: plain chunks only.
    int shareSerial_ = 0;
    size_t shareSaved_ = 0, shareTotal_ = 0;
    size_t shareWhy_[4] = {};   // not shared: in a level with asserts, outside any instance, a template, no match
    static std::string dParams() {
        std::string r;
        for (int c = 0; c < kRefCls; c++) r += (c ? ", const long D" : "const long D") + std::to_string(c);
        return r;
    }
    static std::string dUnused() {
        std::string r;
        for (int c = 0; c < kRefCls; c++) r += " (void)D" + std::to_string(c) + ";";
        return r;
    }
    std::vector<std::string> shareChunks(std::ostream& o, const std::string& name, const std::vector<size_t>& js,
                                         const std::vector<std::string>& text, const std::vector<int>& unitProc,
                                         const std::vector<std::vector<const Instr*>>& unitIns,
                                         const std::vector<CodegenProc>& procs, int kChunk) {
        std::vector<std::string> calls;
        std::string buf;
        int nb = 0, nplain = 0;
        auto flush = [&] {
            if (!nb) return;
            std::string f = name + std::to_string(nplain++);
            o << "static void " << f << "() {\n    const bool obs = S.observed; (void)obs;\n" << buf << "}\n";
            calls.push_back(f + "()");
            buf.clear();
            nb = 0;
        };
        auto plain = [&](size_t j) {
            buf += text[j];
            if (++nb >= kChunk) flush();
        };
        static const bool off = [] { const char* e = std::getenv("MSIM_NOSHARE"); return e && std::strcmp(e, "ds") != 0; }();   // "ds": the clocked code only
        // The signal that names a unit's instance: the first it writes with a
        // name of the design (not _tf scratch, $atstar, a wide piece), else
        // the first such signal its text reads.
        auto good = [&](long long sg) {
            if (sg < 0 || (size_t)sg >= sigName_.size()) return false;
            const std::string& n = sigName_[(size_t)sg];
            return !n.empty() && n[0] != '_' && n[0] != '$' && n.find('.') != std::string::npos;
        };
        std::vector<int> keySig(text.size(), -2);
        auto ksig = [&](size_t j) {
            if (keySig[j] != -2) return keySig[j];
            int r = -1;
            for (auto* in : unitIns[j]) if (good(in->sig)) { r = in->sig; break; }
            if (r < 0) refScan(text[j], [&](size_t, size_t, size_t, long long n, int kind) { if (r < 0 && kind <= 1 && good(n)) r = (int)n; });
            return keySig[j] = r;
        };
        auto key = [&](size_t j) -> std::string {   // the instance: the first name component
            int sg = ksig(j);
            if (sg < 0) return "";
            const std::string& n = sigName_[sg];
            return n.substr(0, n.find('.'));
        };
        auto rest = [&](size_t j) {
            const std::string& n = sigName_[ksig(j)];
            return n.substr(n.find('.') + 1);
        };
        for (size_t a = 0; a < js.size();) {
            size_t b = a;
            int lv = procs[unitProc[js[a]]].combLevel;
            while (b < js.size() && procs[unitProc[js[b]]].combLevel == lv) b++;
            // Segments of the level: a new one where an instance's assertion
            // units would come a second time (module A in every instance, then
            // module B), so that grouping by instance keeps the reports in
            // order. The units without assertions go with the first.
            std::vector<std::vector<size_t>> segs(1);
            {
                std::set<std::string> seen;
                std::string run;
                bool any = false;
                for (size_t x = a; x < b; x++) {
                    if (off || !combAsserts(*procs[unitProc[js[x]]].prog)) { segs[0].push_back(js[x]); continue; }
                    std::string k = key(js[x]);
                    if (!any || k != run) {
                        if (seen.count(k)) { segs.emplace_back(); seen.clear(); }
                        seen.insert(k);
                        run = k;
                        any = true;
                    }
                    segs.back().push_back(js[x]);
                }
            }
            for (auto& L : segs) {
                // Groups by instance, in order of first appearance.
                std::vector<std::string> keys;
                std::map<std::string, std::vector<size_t>> grp;
                // Units with assertions stay out of the groups, in their own order
                // (their reports keep the rank order); the others of one level do
                // not depend on each other, so any order computes the same.
                // When the assertion units of each instance come in one run (one
                // instance after another), the groups are emitted in the order of
                // those runs, after the groups without assertions, and every unit
                // is grouped: the reports keep their order.
                auto asserts0 = [&](size_t j) { return combAsserts(*procs[unitProc[j]].prog); };
                bool keepAll = !off;
                std::vector<std::string> runOrder;
                {
                    std::set<std::string> seen;
                    for (size_t x = 0; x < L.size() && keepAll; x++) {
                        if (!asserts0(L[x])) continue;
                        std::string k = key(L[x]);
                        if (!runOrder.empty() && runOrder.back() == k) continue;
                        if (!seen.insert(k).second) keepAll = false;
                        runOrder.push_back(k);
                    }
                    if (const char* d3 = std::getenv("MSIM_WHYSHARE"); !keepAll && d3 && *d3 == '3') {
                        std::fprintf(stderr, "assertion order:");
                        for (size_t x = 0; x < L.size(); x++) if (asserts0(L[x])) std::fprintf(stderr, " %s", key(L[x]).c_str());
                        std::fprintf(stderr, "\n");
                    }
                }
                auto asserts = [&](size_t j) { return !keepAll && asserts0(j); };
                for (size_t x = 0; x < L.size(); x++) {
                    std::string k = off || asserts(L[x]) ? "" : key(L[x]);
                    if (!grp.count(k)) keys.push_back(k);
                    grp[k].push_back(L[x]);
                }
                if (keepAll && !runOrder.empty()) {
                    std::set<std::string> late(runOrder.begin(), runOrder.end());
                    std::vector<std::string> ord;
                    for (auto& k : keys) if (!late.count(k)) ord.push_back(k);
                    for (auto& k : runOrder) ord.push_back(k);
                    keys = ord;
                }
                // Template per shape (the names below the instance, in order).
                std::map<std::string, std::vector<std::string>> tmplOf;   // shape -> keys of the groups that are templates
                std::map<std::string, std::array<long long, kRefCls>> dOf;   // key -> its distances when it shares
                std::map<std::string, std::vector<std::string>> users;
                for (auto& k : keys) {
                    if (k.empty()) continue;
                    auto& g = grp[k];
                    std::string shape;
                    for (size_t j : g) shape += rest(j) + "|";
                    // Against each template of this shape in turn; a new one if none fits
                    // (an instance whose port copy is public, say).
                    bool ok = false;
                    for (const std::string& tk0 : tmplOf[shape]) {
                        auto& t = grp[tk0];
                        std::array<long long, kRefCls> D{};
                        bool set[kRefCls] = {};
                        ok = g.size() == t.size();
                        size_t bad = 0;
                        for (size_t x = 0; ok && x < g.size(); x++) { ok = refMatch(text[t[x]], text[g[x]], tk0, k, D.data(), set); bad = x; }
                        static const char* dbgS = std::getenv("MSIM_WHYSHARE");
                        if (!ok && dbgS && *dbgS == '2' && g.size() == t.size()) {
                            {
                                std::array<long long, kRefCls> D0{};
                                bool s0[kRefCls] = {};
                                for (size_t x = 0; x <= bad; x++) {
                                    std::array<long long, kRefCls> Dx{};
                                    bool sx[kRefCls] = {};
                                    if (!refMatch(text[t[x]], text[g[x]], tk0, k, Dx.data(), sx)) { std::fprintf(stderr, "   unit %zu does not match alone\n", x); bad = x; break; }
                                    for (int c = 0; c < kRefCls; c++)
                                        if (sx[c]) {
                                            if (!s0[c]) { s0[c] = true; D0[c] = Dx[c]; }
                                            else if (D0[c] != Dx[c]) std::fprintf(stderr, "   unit %zu (%s): class %d moves %lld, before %lld\n", x, sigName_[std::max(0, ksig(g[x]))].c_str(), c, Dx[c], D0[c]);
                                        }
                                }
                            }
                            RefToks ta = refToks(text[t[bad]]), tb = refToks(text[g[bad]]);
                            std::fprintf(stderr, "no share: %s vs %s (%zu/%zu refs)\n", sigName_[std::max(0, ksig(t[bad]))].c_str(), sigName_[std::max(0, ksig(g[bad]))].c_str(), ta.num.size(), tb.num.size());
                            for (size_t q = 0; q < std::min(ta.num.size(), tb.num.size()); q++)
                                std::fprintf(stderr, "   %lld (%s, cls %d) vs %lld (%s, cls %d)\n", ta.num[q], ta.num[q] < (long long)sigName_.size() ? sigName_[ta.num[q]].c_str() : "?", refCls(ta.num[q], tk0),
                                             tb.num[q], tb.num[q] < (long long)sigName_.size() ? sigName_[tb.num[q]].c_str() : "?", refCls(tb.num[q], k));
                            if (ta.piece != tb.piece) for (size_t q = 0; q < std::min(ta.piece.size(), tb.piece.size()); q++) if (ta.piece[q] != tb.piece[q]) {
                                size_t o = 0;
                                while (o < ta.piece[q].size() && o < tb.piece[q].size() && ta.piece[q][o] == tb.piece[q][o]) o++;
                                o = o > 80 ? o - 80 : 0;
                                std::fprintf(stderr, "   piece: [%s] vs [%s]\n", ta.piece[q].substr(o, 200).c_str(), tb.piece[q].substr(o, 200).c_str());
                                break;
                            }
                        }
                        if (ok) { dOf[k] = D; users[tk0].push_back(k); break; }
                    }
                    if (!ok) tmplOf[shape].push_back(k);
                }
                std::map<std::string, std::vector<std::string>> fnsOf;   // template key -> its functions
                for (auto& k : keys) {
                    auto& g = grp[k];
                    bool isT = users.count(k) > 0;
                    if (!isT && !dOf.count(k)) { for (size_t j : g) plain(j); continue; }
                    flush();
                    const std::string& tk = isT ? k : [&]() -> const std::string& {
                        for (auto& [t, us] : users) if (std::find(us.begin(), us.end(), k) != us.end()) return t;
                        return k;
                    }();
                    if (!fnsOf.count(tk)) {
                        auto& tg = grp[tk];
                        int id = shareSerial_++;
                        for (size_t c = 0; c < tg.size(); c += kChunk) {
                            std::string f = name + "T" + std::to_string(id) + "_" + std::to_string(c / kChunk);
                            o << "static void " << f << "(" << dParams() << ") {\n    const bool obs = S.observed; (void)obs;" << dUnused() << "\n";
                            for (size_t x = c; x < tg.size() && x < c + (size_t)kChunk; x++) o << refParam(text[tg[x]], tk);
                            o << "}\n";
                            fnsOf[tk].push_back(f);
                        }
                    }
                    std::array<long long, kRefCls> D{};
                    if (!isT) D = dOf[k];
                    std::string args;
                    for (int c = 0; c < kRefCls; c++) args += (c ? ", " : "") + std::to_string(D[c]);
                    for (auto& f : fnsOf[tk]) calls.push_back(f + "(" + args + ")");
                    if (!isT) for (size_t j : g) shareSaved_ += text[j].size();
                }
                for (size_t x = 0; x < L.size(); x++) {
                    shareTotal_ += text[L[x]].size();
                    const bool as = asserts(L[x]);
                    const std::string k = as ? "" : key(L[x]);
                    int why = as ? 0 : k.empty() ? 1 : dOf.count(k) ? -1 : users.count(k) ? 2 : 3;
                    if (why >= 0) shareWhy_[why] += text[L[x]].size();
                }
            }
            a = b;
        }
        flush();
        return calls;
    }
    // The clocked functions of the cycle domain (ds<n>, ds<n>_2) from their
    // bodies. Like the comb code, the processes of instances of one module
    // share one copy (dsT<n>, moved by D); MSIM_NOSHARE=1 turns that off.
    void emitDomainFns(std::ostream& o4, std::ostream& o2, const std::vector<int>& dom, const std::vector<CodegenProc>& procs,
                       const std::vector<std::string>& b4, const std::vector<std::string>& b2, const std::vector<char>& tmp) {
        static const bool off = [] { const char* e = std::getenv("MSIM_NOSHARE"); return e && std::strcmp(e, "comb") != 0; }();   // "comb": the comb code only
        auto keyOf = [&](int n) -> std::string {
            for (auto& in : procs[n].prog->code)
                if ((in.k == IK::Nba || in.k == IK::Assign) && in.sig >= 0 && (size_t)in.sig < sigName_.size()) {
                    const std::string& m = sigName_[in.sig];
                    size_t d = m.find('.');
                    if (d != std::string::npos && m[0] != '_' && m[0] != '$') return m.substr(0, d);
                }
            return "";
        };
        std::vector<int> useT(procs.size(), -1);
        std::vector<char> isT(procs.size());
        std::vector<std::string> key(procs.size());
        std::vector<std::array<long long, kRefCls>> useD(procs.size());
        std::map<std::string, std::vector<int>> tmplOf;   // shape -> template processes
        for (int n : dom) {
            if (off) break;
            key[n] = keyOf(n);
            if (key[n].empty()) continue;
            std::string all = b4[n] + "\x01" + b2[n];
            RefToks rt = refToks(all);
            std::string shape = tmp[n] ? "T" : "N";
            for (auto& pc : rt.piece) shape += pc + "\x02";
            auto& lst = tmplOf[shape];
            for (int t : lst) {
                std::array<long long, kRefCls> D{};
                bool set[kRefCls] = {};
                if (refMatch(b4[t] + "\x01" + b2[t], all, key[t], key[n], D.data(), set)) { useT[n] = t; useD[n] = D; isT[t] = 1; break; }
                static const char* dbg = std::getenv("MSIM_WHYSHARE");
                if (dbg && *dbg == '2') {
                    RefToks ta = refToks(b4[t] + "\x01" + b2[t]);
                    for (size_t q = 0; q < ta.num.size(); q++) {
                        int ca = refCls(ta.num[q], key[t]), cb = refCls(rt.num[q], key[n]);
                        if (ca != cb) { std::fprintf(stderr, "no share ds%d/ds%d (%s/%s): %lld cls %d vs %lld cls %d\n", t, n, key[t].c_str(), key[n].c_str(), ta.num[q], ca, rt.num[q], cb); break; }
                    }
                }
            }
            if (useT[n] < 0) lst.push_back(n);
            if (const char* d3 = std::getenv("MSIM_WHYSHARE"); d3 && *d3 == '3')
                std::fprintf(stderr, "ds%d key %s shape %zu bytes, %zu templates, uses %d\n", n, key[n].c_str(), shape.size(), lst.size(), useT[n]);
        }
        auto args = [&](const std::array<long long, kRefCls>& D) {
            std::string r;
            for (int c = 0; c < kRefCls; c++) r += (c ? ", " : "") + std::to_string(D[c]);
            return r;
        };
        size_t saved = 0, total = 0;
        for (int n : dom) {
            total += b4[n].size() + b2[n].size();
            o4 << "// process " << n << " (" << procs[n].kind << ", cycle domain)\n";
            if (isT[n]) {
                o4 << "static void __attribute__((noinline)) dsT" << n << "(" << dParams() << ") {" << dUnused() << "\n"
                   << refParam(b4[n], key[n]) << "}\n";
                if (!tmp[n])
                    o2 << "static void __attribute__((noinline)) dsT" << n << "_2(" << dParams() << ") {" << dUnused() << "\n"
                       << refParam(b2[n], key[n]) << "}\n";
            }
            int t = isT[n] ? n : useT[n];
            if (t >= 0) {
                std::string a = args(isT[n] ? std::array<long long, kRefCls>{} : useD[n]);
                o4 << "static void ds" << n << "() { dsT" << t << "(" << a << "); }\n";
                if (tmp[n]) o2 << "static void ds" << n << "_2() { ds" << n << "(); }\n";
                else o2 << "static void ds" << n << "_2() { dsT" << t << "_2(" << a << "); }\n";
                if (!isT[n]) saved += b4[n].size() + b2[n].size();
                continue;
            }
            o4 << "static void ds" << n << "() {\n" << b4[n] << "}\n";
            if (tmp[n]) o2 << "static void ds" << n << "_2() { ds" << n << "(); }\n";
            else o2 << "static void ds" << n << "_2() {\n" << b2[n] << "}\n";
        }
        if (std::getenv("MSIM_WHYSHARE"))
            std::fprintf(stderr, "shared clocked code: %zu of %zu bytes are calls to another instance's copy\n", saved, total);
    }
    std::vector<int> domRoot_;    // cycleDomain: per signal, the signal it is a copy of (itself if none)
    std::vector<int> domGroup_;   // cycleDomain: per process, its clock group in the domain (or -1)
    // The events each clock group of the domain is registered on. One signal
    // per source and edge: the copies of a source all change in the same comb
    // pass, so one of them is enough to wake the group. A copy rather than
    // the source, when there is one (the group then runs after that pass).
    // The other copies are ordinary signals, and private ones when nothing
    // else waits on them.
    std::vector<std::vector<std::pair<int, Edge>>> domWake_;
    void wakeEvents(const std::vector<CodegenProc>& procs, const std::vector<int>& dom) {
        domWake_.clear();
        for (int g = 0; g < std::max(domGroups_, 1); g++) {
            std::vector<std::pair<int, Edge>> ev, key;
            for (int d : dom)
                if (domGroups_ <= 1 || domGroup_[d] == g)
                    for (auto& e : procs[d].seqEv) {
                        int rt = e.first >= 0 && (size_t)e.first < domRoot_.size() ? domRoot_[e.first] : e.first;
                        std::pair<int, Edge> kk{rt, e.second};
                        auto it = std::find(key.begin(), key.end(), kk);
                        if (it == key.end()) { key.push_back(kk); ev.push_back(e); }
                        else if (ev[(size_t)(it - key.begin())].first == rt && e.first != rt) ev[(size_t)(it - key.begin())] = e;
                    }
            domWake_.push_back(std::move(ev));
        }
    }
    bool wakeSig(int s) const {
        for (auto& g : domWake_) for (auto& e : g) if (e.first == s) return true;
        return false;
    }
    int domGroups_ = 0;

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
        std::vector<int> owner(ns, -1);   // the branching comb process that writes a candidate
        std::vector<char> mine(procs.size(), 0);
        for (int i : dom) mine[i] = 1;
        for (size_t i = 0; i < procs.size(); i++) if (procs[i].combRank >= 0) mine[i] = 1;
        std::function<void(const E&)> refs = [&](const E& e) {
            if (!e) return;
            if (e->op == Op::Ref && e->sig >= 0 && (size_t)e->sig < ns) bad[e->sig] = 1;
            for (auto& k : e->kids) refs(k);
        };
        for (size_t i = 0; i < procs.size(); i++) {
            // A clock or reset stays in S.sigs when a process waits on it: the
            // domain waits on its wake signals only (wakeEvents).
            for (auto& [sg, ed] : procs[i].seqEv) if ((size_t)sg < ns && (!mine[i] || procs[i].combRank >= 0 || wakeSig(sg))) bad[sg] = 1;
            if (procs[i].combRank >= 0) {
                for (auto& in : procs[i].prog->code) {
                    if (in.k == IK::WaitEdge) break;
                    if (in.k != IK::Assign) continue;   // a jump or an assertion of an if / case body
                    if (in.sig < 0 || (size_t)in.sig >= ns) continue;
                    // The one driver of a net, or (drv < 0) the continuous assign msim
                    // merged into a plain write (mergePartialDrivers).
                    bool whole = in.hi < 0 && !in.arr && in.aidx.empty() && !in.bidx;
                    if (whole && in.drv >= 0 && sigs[in.sig].drivers == 1 && in.s0 != STR_HIZ && in.s1 != STR_HIZ)
                        cand[in.sig] = 1;
                    else if (whole && in.drv < 0 && sigs[in.sig].drivers == 0 && oneAssign(*procs[i].prog) && !cand[in.sig])
                        cand[in.sig] = 1;
                    // The outputs of one always_comb with if / case: any
                    // number of whole or part writes, all from this process.
                    else if (in.drv < 0 && !in.tmp && sigs[in.sig].drivers == 0 && !in.arr && in.aidx.empty() &&
                             (!cand[in.sig] || owner[in.sig] == (int)i || sigs[in.sig].w > 64 || (in.hi >= 0 && owner[in.sig] >= 0))) {
                        // wide: see splitWide. Part writes from several processes: the
                        // assigns of single bits or fields that msim merged (mergePartialDrivers).
                        cand[in.sig] = 1;
                        owner[in.sig] = (int)i;
                    } else bad[in.sig] = 1;
                }
                continue;
            }
            if (mine[i]) {
                for (auto& in : procs[i].prog->code)
                    if (in.k == IK::Nba && in.sig >= 0 && (size_t)in.sig < ns) {
                        if (in.hi < 0 || sigs[in.sig].w > 64) cand[in.sig] = 1; else bad[in.sig] = 1;
                    } else if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < ns) bad[in.sig] = 1;   // a temporary: kept in S.sigs
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
        }
        // Run-time array access reads and writes S.sigs[base + index]: the
        // elements of every array any process names stay there.
        (void)anyArr;
        {
            auto range = [&](const ArrShape* a) {
                if (!a) return;
                for (int64_t k = 0; k < a->count; k++)
                    if (a->base + k >= 0 && (size_t)(a->base + k) < ns) bad[(size_t)(a->base + k)] = 1;
            };
            std::function<void(const E&)> ea = [&](const E& e) {
                if (!e) return;
                range(e->arr.get());
                for (auto& k : e->kids) ea(k);
            };
            std::vector<const Prog*> seen;
            std::function<void(const Prog*)> walk = [&](const Prog* p) {
                if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return;
                seen.push_back(p);
                for (auto& in : p->code) {
                    range(in.arr.get());
                    ea(in.e); ea(in.bidx); ea(in.de);
                    for (auto& a : in.args) ea(a);
                    for (auto& a : in.aidx) ea(a);
                    walk(in.callee);
                    for (auto* b : in.branches) walk(b);
                }
            };
            for (auto& pr : procs) walk(pr.prog);
        }
        for (int s : noPriv) if (s >= 0 && (size_t)s < ns) bad[(size_t)s] = 1;
        priv_.assign(ns, 0);
        for (size_t s = 0; s < ns; s++)
            priv_[s] = cand[s] && !bad[s] && sigs[s].w >= 1 && sigs[s].w <= 64 &&
                       sigs[s].name.find_first_of("[$") == std::string::npos;
        wide_.assign(ns, 0);
        for (size_t s = 0; s < ns; s++)
            wide_[s] = cand[s] && !bad[s] && sigs[s].w > 64 && !noPriv.count((int)s) &&
                       sigs[s].name.find_first_of("[$") == std::string::npos;
    }

    // The 4-state branch, out of the hot code: a domain of thousands of
    // statements otherwise misses the instruction cache all the time.
    // -DMSIM_COLDSTAT counts each one and prints the busiest at exit (COLD in
    // the preamble): the statements that keep running the 4-state way.
    int colds_ = 0;
    std::vector<int> coneOuts_;
    std::string privInit_;       // run(): sets the private signals' first X / Z (PINIT)
    std::vector<int> coneIns_;   // emitCycle: the outside signals of the clock cone (combCone), sorted
    std::string cold(const std::string& stmt) {
        return "[&]() __attribute__((noinline, cold)) { COLD(" + std::to_string(colds_++) + ") " + stmt + " }();";
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
        struct Dyn { bool& f; Dyn(bool& x) : f(x) { f = true; } ~Dyn() { f = false; } } dyn(dynOk_);   // every split below is guarded
        if (i.tmp) return assign(i);   // an -xprop temporary: poked, as everywhere else
        if (i.bidx) {   // v[i] = ...: the bits are chosen at run time
            bool pv = isPriv(i.sig);
            int W = sigW_[i.sig];
            std::string P_ = "P" + std::to_string(i.sig), S_ = std::to_string(i.sig), W_ = std::to_string(W);
            std::string slowW = pv
                ? dynTarget(i, ex(i.e), " PW(" + P_ + ", " + W_ + ", hi >= 0 ? v_insert(PR(" + P_ + ", " + W_ + ", false), v, hi, lo) : v);", true)
                : dynTarget(i, pv ? ex(i.e) : fast(i.e), " S.fw(s, hi >= 0 ? v_insert(S.get(s), v, hi, lo) : v);");
            std::string gb, ub, sb;
            // One bit, index and value known: set or clear it in place.
            // v[i] or v[i +: n], index and value known: the bits change in place.
            // A select that is partly outside the vector takes the general path.
            int n = i.pw ? i.pw : 1;
            if (!i.pdown && !i.arr && W <= 64 && n <= W && i.lo >= 0 && split(i.bidx, gb, ub, sb) && split(i.e, g, u, sl)) {
                std::string cond = gb.empty() ? g : g.empty() ? gb : gb + " && " + g;
                std::string wr = pv
                    ? P_ + ".v = (" + P_ + ".v & ~m) | (((uint64_t)(" + u + ") << b) & m); " + P_ + ".x &= ~m; " + P_ + ".z &= ~m;"
                    : "const Val& c = S.sigs[" + S_ + "].val; S.fw(" + S_ + ", Val{(c.v & ~m) | (((uint64_t)(" + u + ") << b) & m), c.x & ~m, " +
                      W_ + ", c.z & ~m, false});";
                std::string fastW = "{ const uint64_t b = (uint64_t)(" + ub + ") - " + std::to_string(i.lo) + "; if (b <= " +
                                    std::to_string(W - n) + ") { const uint64_t m = " + M(n) + " << b; " + wr + " } else " + cold(slowW) + " }";
                if (cond.empty()) return " " + fastW + "\n";
                return " if (__builtin_expect(" + cond + ", 1)) " + fastW + " else " + cold(slowW) + "\n";
            }
            return slowW;
        }
        if (isPriv(i.sig) && i.hi >= 0) {   // some bits of a private signal
            std::string P_ = "P" + std::to_string(i.sig), m = M(i.hi - i.lo + 1) + " << " + std::to_string(i.lo);
            std::string slowW = "PW(" + P_ + ", " + std::to_string(sigW_[i.sig]) + ", v_insert(PR(" + P_ + ", " +
                                std::to_string(sigW_[i.sig]) + ", false), " + ex(i.e) + ", " + std::to_string(i.hi) + ", " +
                                std::to_string(i.lo) + "));";
            if (!split(i.e, g, u, sl)) return " " + slowW + "\n";
            std::string fastW = "{ const uint64_t m = " + m + "; " + P_ + ".v = (" + P_ + ".v & ~m) | (((uint64_t)(" + u + ") << " +
                                std::to_string(i.lo) + ") & m); " + P_ + ".x &= ~m; " + P_ + ".z &= ~m; }";
            if (g.empty()) return " " + fastW + "\n";
            return " if (__builtin_expect(" + g + ", 1)) " + fastW + " else " + cold(slowW) + "\n";
        }
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
        else if (i.hi >= 0) {
            // Some bits of a signal in S.sigs (it may be wider than 64 bits):
            // most passes write what is already there, which samePart sees
            // without building the whole value.
            std::string slowW = "S.fw(" + std::to_string(i.sig) + ", v_insert(S.get(" + std::to_string(i.sig) + "), " + val + ", " +
                                std::to_string(i.hi) + ", " + std::to_string(i.lo) + "));";
            if (i.hi - i.lo < 64 && i.lo >= 0 && split(i.e, g, u, sl))
                o << " if (!(" << (g.empty() ? "true" : g) << " && S.samePart(" << i.sig << ", " << i.hi << ", " << i.lo << ", " << u << "))) "
                  << slowW << "\n";
            else
                o << " " << slowW << "\n";
        } else
            o << " S.fw(" << i.sig << ", " << val << ");\n";
        return o.str();
    }

    // dsN: one domain process, its NBAs into slots F<k>/H<k>. dseq runs them
    // all (in parallel) and leaves one marker NBA; fcommit writes the slots in
    // queue order and re-evaluates every static comb process in rank order.
    // Wide signals in pieces. A signal of more than 64 bits that only the
    // domain touches is cut where its part selects and part writes cut it
    // (the fields of a packed struct), and at 64 bits at most. Each piece is
    // a private signal of its own (ids from sigs.size() on, no S.sigs entry):
    // a field read is a plain variable and a field write a store, with no
    // Val to allocate. The processes that name such a signal are rewritten
    // in copies (ownProgs_); syncP puts the pieces together for a waveform.
    struct Piece { int id, hi, lo; };
    std::vector<char> wide_;
    std::map<int, std::vector<Piece>> pieces_;   // per split signal, from bit 0 up
    std::vector<int> pieceOf_;                   // per piece id - sigs.size(): its signal
    std::deque<Prog> ownProgs_;
    size_t realSigs_ = 0;
public:
    int origOf(int s) const { return s >= 0 && (size_t)s >= realSigs_ && (size_t)s - realSigs_ < pieceOf_.size() ? pieceOf_[(size_t)s - realSigs_] : s; }
private:
    int widthOf(const E& e) const {
        if (!e || e->at) return -1;
        switch (e->op) {
            case Op::Const: return e->k.w;
            case Op::Ref: return e->sig >= 0 && (size_t)e->sig < sigW_.size() ? sigW_[e->sig] : -1;
            case Op::Slice: return e->hi - e->lo + 1;
            case Op::Resize: return e->hi;
            case Op::Concat: { int w = 0; for (auto& k : e->kids) { int x = widthOf(k); if (x < 0) return -1; w += x; } return w; }
            case Op::Mux: { int a = widthOf(e->kids[1]), b = widthOf(e->kids[2]); return a == b ? a : -1; }
            case Op::Not: return widthOf(e->kids[0]);
            case Op::And: case Op::Or: case Op::Xor: { int a = widthOf(e->kids[0]), b = widthOf(e->kids[1]); return a == b ? a : -1; }
            default: return -1;
        }
    }
    static E mkSlice(E a, int hi, int lo) {
        E e = node(Op::Slice, {std::move(a)});
        e->hi = hi; e->lo = lo;
        return e;
    }
    // Bits [hi:lo] of a split signal, as its pieces.
    E piecesOf(int s, int hi, int lo) const {
        std::vector<E> parts;   // the top one first
        const auto& ps = pieces_.at(s);
        for (size_t k = ps.size(); k-- > 0;) {
            const Piece& p = ps[k];
            int a = std::max(lo, p.lo), b = std::min(hi, p.hi);
            if (a > b) continue;
            parts.push_back(a == p.lo && b == p.hi ? ref(p.id) : mkSlice(ref(p.id), b - p.lo, a - p.lo));
        }
        if (parts.size() == 1) return parts[0];
        return node(Op::Concat, std::move(parts));
    }
    // Bits [hi:lo] of e, with the select pushed toward the leaves.
    E sliceOf(const E& e, int hi, int lo) const {
        int w = widthOf(e);
        if (w > 0 && lo == 0 && hi == w - 1) return e;
        if (w > 0 && hi < w && !e->at) switch (e->op) {
            case Op::Const: return konst(v_slice(e->k, hi, lo));
            case Op::Slice: return sliceOf(e->kids[0], e->lo + hi, e->lo + lo);
            case Op::Resize: { int kw = widthOf(e->kids[0]); if (kw > hi) return sliceOf(e->kids[0], hi, lo); break; }
            case Op::Mux: { E m = node(Op::Mux, {e->kids[0], sliceOf(e->kids[1], hi, lo), sliceOf(e->kids[2], hi, lo)}); return m; }
            case Op::Not: return node(Op::Not, {sliceOf(e->kids[0], hi, lo)});
            case Op::And: case Op::Or: case Op::Xor: return node(e->op, {sliceOf(e->kids[0], hi, lo), sliceOf(e->kids[1], hi, lo)});
            case Op::Concat: {
                std::vector<E> parts;
                int top = w;
                for (auto& k : e->kids) {
                    int kw = widthOf(k), klo = top - kw, khi = top - 1;
                    top = klo;
                    int a = std::max(lo, klo), b = std::min(hi, khi);
                    if (a > b) continue;
                    parts.push_back(sliceOf(k, b - klo, a - klo));
                }
                if (parts.size() == 1) return parts[0];
                return node(Op::Concat, std::move(parts));
            }
            default: break;
        }
        return mkSlice(e, hi, lo);
    }
    std::vector<CodegenProc> splitWide(const std::vector<CodegenProc>& procs, const std::vector<int>& dom) {
        std::vector<CodegenProc> out = procs;
        size_t ns = sigW_.size();
        realSigs_ = ns;
        std::vector<char> W = wide_;
        W.resize(ns, 0);
        if (std::find(W.begin(), W.end(), 1) == W.end()) return out;
        auto isW = [&](int x) { return x >= 0 && (size_t)x < ns && W[x]; };
        std::vector<char> mine(procs.size(), 0);
        for (int i : dom) mine[i] = 1;
        for (size_t i = 0; i < procs.size(); i++) if (procs[i].combRank >= 0) mine[i] = 1;
        std::function<void(const E&, const std::function<void(const Expr&, bool)>&)> each =
            [&](const E& e, const std::function<void(const Expr&, bool)>& f) {
                if (!e) return;
                if (e->op == Op::Ref) f(*e, false);
                if (e->op == Op::Slice && e->kids.size() == 1 && e->kids[0] && e->kids[0]->op == Op::Ref && !e->kids[0]->at) { f(*e, true); return; }
                each(e->at, f);
                for (auto& k : e->kids) each(k, f);
            };
        auto exprs = [&](const Instr& in, const std::function<void(const Expr&, bool)>& f) {
            each(in.e, f); each(in.bidx, f); each(in.de, f); each(in.fd, f);
            for (auto& a : in.args) each(a, f);
            for (auto& a : in.aidx) each(a, f);
        };
        auto drop = [&](int x) { if (isW(x)) W[x] = 0; };
        auto dropAll = [&](const Instr& in) {
            exprs(in, [&](const Expr& x, bool sl) { drop(sl ? x.kids[0]->sig : x.sig); });
            drop(in.sig); drop(in.dres); drop(in.dkey); drop(in.dsrc); drop(in.dyn);
            for (auto& [sg, ed] : in.events) drop(sg);
        };
        // Anything outside the domain that names one keeps it whole.
        std::vector<const Prog*> seen;
        std::function<void(const Prog*)> outside = [&](const Prog* p) {
            if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return;
            seen.push_back(p);
            for (auto& in : p->code) { dropAll(in); outside(in.callee); for (auto* b : in.branches) outside(b); }
        };
        for (size_t i = 0; i < procs.size(); i++) if (!mine[i]) outside(procs[i].prog);
        static const std::set<Op> pure = {Op::Const, Op::Ref, Op::Not, Op::And, Op::Or, Op::Xor, Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod,
                                          Op::Eq, Op::Ne, Op::CaseEq, Op::CaseZEq, Op::CaseXEq, Op::Lt, Op::Le, Op::Gt, Op::Ge, Op::Shl, Op::Shr,
                                          Op::Asr, Op::Resize, Op::LogNot, Op::LogAnd, Op::LogOr, Op::RedAnd, Op::RedOr, Op::RedXor, Op::Slice,
                                          Op::Index, Op::Concat, Op::Mux, Op::WildEq};
        std::function<bool(const E&)> isPure = [&](const E& e) {
            if (!e) return true;
            if (e->at || !pure.count(e->op)) return false;
            for (auto& k : e->kids) if (!isPure(k)) return false;
            return true;
        };
        std::map<int, std::set<int>> cuts;
        for (size_t i = 0; i < procs.size(); i++) {
            if (!mine[i]) continue;
            const Prog& p = *procs[i].prog;
            bool comb = procs[i].combRank >= 0, asserts = comb && combAsserts(p);
            for (auto& in : p.code) {
                outside(in.callee);
                for (auto* b : in.branches) outside(b);
                bool wr = in.k == IK::Assign || in.k == IK::Nba;
                if (asserts || !(wr || in.k == IK::Jmp || in.k == IK::JmpIfNot || in.k == IK::WaitEdge)) { dropAll(in); continue; }
                if (in.k == IK::WaitEdge) {
                    if (in.edge != ANY) drop(in.sig);
                    for (auto& [sg, ed] : in.events) if (ed != ANY) drop(sg);
                }
                if (wr && isW(in.sig)) {
                    bool self = false;
                    each(in.e, [&](const Expr& x, bool sl) { self |= (sl ? x.kids[0]->sig : x.sig) == in.sig; });
                    int w = sigW_[in.sig];
                    if (in.bidx || in.arr || !in.aidx.empty() || in.tmp || in.dyn >= 0 || in.nbaLater || in.de || !in.dly.empty() ||
                        in.gate >= 0 || in.udp || (comb ? in.k != IK::Assign : in.k != IK::Nba) || (comb && self) || !isPure(in.e) ||
                        (in.hi >= 0 && (in.lo < 0 || in.hi >= w || in.lo > in.hi)))
                        drop(in.sig);
                    else if (in.hi >= 0) { cuts[in.sig].insert(in.lo); cuts[in.sig].insert(in.hi + 1); }
                }
                exprs(in, [&](const Expr& x, bool sl) {
                    if (!sl) return;
                    int sg = x.kids[0]->sig;
                    if (!isW(sg)) return;
                    if (x.lo < 0 || x.hi >= sigW_[sg] || x.lo > x.hi) { drop(sg); return; }
                    cuts[sg].insert(x.lo); cuts[sg].insert(x.hi + 1);
                });
            }
        }
        for (size_t x = 0; x < ns; x++) {
            if (!W[x]) continue;
            std::set<int>& c = cuts[(int)x];
            c.insert(0); c.insert(sigW_[x]);
            std::vector<Piece> ps;
            for (auto it = c.begin(); std::next(it) != c.end(); ++it)
                for (int lo = *it, end = *std::next(it); lo < end; lo += 64) ps.push_back({0, std::min(lo + 63, end - 1), lo});
            if (ps.size() > 64) { W[x] = 0; continue; }
            for (auto& pc : ps) {
                pc.id = (int)sigW_.size();
                sigW_.push_back(pc.hi - pc.lo + 1);
                sigNet_.push_back(sigNet_[x]);
                scratch_.push_back(scratch_[x]);
                priv_.push_back(1);
                pieceOf_.push_back((int)x);
            }
            pieces_[(int)x] = std::move(ps);
        }
        if (std::getenv("MSIM_WHYCYCLE")) {
            size_t np = 0;
            for (auto& [sg, ps] : pieces_) np += ps.size();
            std::fprintf(stderr, "cycle domain: %zu wide signals kept as %zu pieces\n", pieces_.size(), np);
        }
        if (pieces_.empty()) return out;
        auto split = [&](int x) { return x >= 0 && (size_t)x < ns && W[x]; };
        std::function<E(const E&)> rw = [&](const E& e) -> E {
            if (!e) return e;
            if (e->op == Op::Slice && e->kids.size() == 1 && e->kids[0] && e->kids[0]->op == Op::Ref && !e->kids[0]->at && split(e->kids[0]->sig))
                return piecesOf(e->kids[0]->sig, e->hi, e->lo);
            if (e->op == Op::Ref && !e->at && split(e->sig)) {
                E c = piecesOf(e->sig, sigW_[e->sig] - 1, 0);
                return e->sgn ? resize(c, sigW_[e->sig], false, true) : c;
            }
            E at = rw(e->at);
            std::vector<E> kids;
            bool ch = at != e->at;
            for (auto& k : e->kids) { kids.push_back(rw(k)); ch |= kids.back() != k; }
            if (!ch) return e;
            auto n = std::make_shared<Expr>(*e);
            n->kids = std::move(kids);
            n->at = at;
            return n;
        };
        for (size_t i = 0; i < procs.size(); i++) {
            if (!mine[i]) continue;
            const Prog& p = *procs[i].prog;
            bool touch = false;
            for (auto& in : p.code) {
                exprs(in, [&](const Expr& x, bool sl) { touch |= split(sl ? x.kids[0]->sig : x.sig); });
                touch |= split(in.sig);
                for (auto& [sg, ed] : in.events) touch |= split(sg);
            }
            if (!touch) continue;
            ownProgs_.push_back(p);
            Prog& q = ownProgs_.back();
            q.code.clear();
            std::vector<size_t> at(p.code.size() + 1, 0);
            for (size_t pc = 0; pc < p.code.size(); pc++) {
                at[pc] = q.code.size();
                Instr in = p.code[pc];
                in.e = rw(in.e); in.bidx = rw(in.bidx); in.de = rw(in.de);
                for (auto& a : in.args) a = rw(a);
                for (auto& a : in.aidx) a = rw(a);
                if (in.k == IK::WaitEdge) {
                    std::vector<std::pair<int, Edge>> ev = in.events;
                    if (ev.empty() && in.sig >= 0) ev.push_back({in.sig, in.edge});
                    bool any = false;
                    for (auto& [sg, ed] : ev) any |= split(sg);
                    if (any) {
                        std::vector<std::pair<int, Edge>> ne;
                        for (auto& [sg, ed] : ev) {
                            if (!split(sg)) { ne.push_back({sg, ed}); continue; }
                            for (auto& pc2 : pieces_[sg]) ne.push_back({pc2.id, ANY});
                        }
                        in.events = ne;
                        in.sig = ne[0].first;
                        in.edge = ne.size() == 1 ? ne[0].second : in.edge;
                        if (ne.size() == 1) in.events.clear();
                    }
                    q.code.push_back(std::move(in));
                    continue;
                }
                if ((in.k == IK::Assign || in.k == IK::Nba) && split(in.sig)) {
                    int L = in.hi >= 0 ? in.lo : 0, H = in.hi >= 0 ? in.hi : sigW_[in.sig] - 1;
                    E val = in.e;
                    for (auto& pc2 : pieces_[in.sig]) {
                        if (pc2.hi < L || pc2.lo > H) continue;
                        Instr w = in;
                        w.sig = pc2.id; w.hi = -1; w.lo = 0; w.drv = -1; w.s0 = w.s1 = STR_STRONG;
                        w.e = pc2.lo == L && pc2.hi == H ? val : sliceOf(val, pc2.hi - L, pc2.lo - L);
                        q.code.push_back(std::move(w));
                    }
                    continue;
                }
                q.code.push_back(std::move(in));
            }
            at[p.code.size()] = q.code.size();
            for (auto& in : q.code)
                if (in.k == IK::Jmp || in.k == IK::JmpIfNot) {
                    in.target = at[std::min(in.target, p.code.size())];
                    if (in.xt != Instr::NOX) in.xt = at[std::min(in.xt, p.code.size())];
                }
            out[i].prog = &q;
            std::vector<int> ins;
            for (int x : out[i].combIns) {
                if (!split(x)) { ins.push_back(x); continue; }
                for (auto& pc2 : pieces_[x]) ins.push_back(pc2.id);
            }
            out[i].combIns = std::move(ins);
        }
        return out;
    }
    void emitCycle(std::ostream& o, const std::vector<CodegenProc>& procs, const std::vector<int>& dom,
                   const std::vector<int>& byRank) {
        clock_.assign(sigW_.size(), 0);
        for (size_t i = 0; i < procs.size(); i++)
            for (auto& [sg, ed] : procs[i].seqEv)
                if (sg >= 0 && (size_t)sg < clock_.size() && (wakeSig(sg) || std::find(dom.begin(), dom.end(), (int)i) == dom.end())) clock_[sg] = 1;
        // Copies and dead logic. A private signal whose one write is a
        // whole copy of another private signal (a port connection) is that
        // signal under another name: the domain reads the source instead.
        // Then a comb statement that only writes private signals nothing
        // reads has no effect anyone sees. Those go to combDead, which runs
        // only when a VCD or $monitor watches every signal (syncP).
        std::set<const Instr*> deadIn;
        std::vector<char> deadProc(procs.size(), 0), deadSig(sigW_.size(), 0);
        std::vector<std::string> deadStmts;
        if (!std::getenv("MSIM_NODEAD")) {
            size_t ns = sigW_.size();
            auto ok = [&](int x) { return x >= 0 && (size_t)x < ns; };
            std::vector<char> mine(procs.size(), 0), live(ns, 0), keep(ns, 0);
            for (int i : dom) mine[i] = 1;
            for (int i : byRank) if (i >= 0) mine[i] = 1;
            std::vector<int> nw(ns, 0);
            std::function<void(const E&, const std::function<void(Expr&)>&)> each = [&](const E& e, const std::function<void(Expr&)>& f) {
                if (!e) return;
                if (e->op == Op::Ref && !e->at) f(*e);
                for (auto& k : e->kids) each(k, f);
            };
            auto exprs = [&](const Instr& in, const std::function<void(Expr&)>& f) {
                each(in.e, f); each(in.bidx, f); each(in.de, f);
                for (auto& a : in.args) each(a, f);
                for (auto& a : in.aidx) each(a, f);
            };
            auto touchAll = [&](const Instr& in, std::vector<char>& out) {
                exprs(in, [&](Expr& x) { if (ok(x.sig)) out[x.sig] = 1; });
                if (ok(in.sig)) out[in.sig] = 1;
                for (auto& [sg, ed] : in.events) if (ok(sg)) out[sg] = 1;
            };
            std::vector<const Prog*> seen;
            std::function<void(const Prog*, bool)> walk = [&](const Prog* p, bool outside) {
                if (!p || std::find(seen.begin(), seen.end(), p) != seen.end()) return;
                seen.push_back(p);
                for (auto& in : p->code) {
                    if ((in.k == IK::Assign || in.k == IK::Nba) && ok(in.sig)) nw[in.sig]++;
                    if (outside) touchAll(in, live);
                    walk(in.callee, outside);
                    for (auto* b : in.branches) walk(b, outside);
                }
            };
            for (size_t i = 0; i < procs.size(); i++) walk(procs[i].prog, !mine[i]);
            // An assertion's process keeps every signal it names as it is.
            for (int i : byRank)
                if (i >= 0 && combAsserts(*procs[i].prog))
                    for (auto& in : procs[i].prog->code) { touchAll(in, keep); touchAll(in, live); }
            std::vector<int> alias(ns, -1);
            for (int i : byRank) {
                if (i < 0 || combBranches(*procs[i].prog)) continue;
                for (auto& in : procs[i].prog->code) {
                    if (in.k != IK::Assign) break;
                    if (in.hi >= 0 || in.bidx || in.arr || !in.aidx.empty() || !in.e || in.e->op != Op::Ref) continue;
                    int a = in.sig, b = in.e->sig;
                    if (!isPriv(a) || !isPriv(b) || a == b || sigW_[a] != sigW_[b] || nw[a] != 1 || keep[a]) continue;
                    alias[a] = b;
                }
            }
            auto root = [&](int a) {
                int r = a;
                for (size_t n = 0; alias[r] >= 0; n++) { r = alias[r]; if (n > ns) return -1; }   // a loop of copies
                return r;
            };
            for (size_t a = 0; a < ns; a++) if (alias[a] >= 0 && root((int)a) < 0) alias[a] = -1;
            size_t nAlias = 0;
            for (size_t a = 0; a < ns; a++) nAlias += alias[a] >= 0;
            auto rw = [&](Expr& x) { if (ok(x.sig) && alias[x.sig] >= 0) x.sig = root(x.sig); };
            for (size_t i = 0; i < procs.size(); i++)
                if (mine[i]) for (auto& in : procs[i].prog->code) exprs(in, rw);
            // Units: a process with if / case, or one straight assign.
            struct Unit { int proc; const Instr* one; std::vector<int> wr, rd; bool live; };
            std::vector<Unit> us;
            for (int i : byRank) {
                if (i < 0) continue;
                const Prog& cp = *procs[i].prog;
                bool br = combBranches(cp);
                if (br) us.push_back({i, nullptr, {}, {}, combAsserts(cp)});
                for (auto& in : cp.code) {
                    if (in.k == IK::WaitEdge) break;
                    if (!br && in.k != IK::Assign) break;
                    if (!br) us.push_back({i, &in, {}, {}, false});
                    Unit& u = us.back();
                    exprs(in, [&](Expr& x) { if (ok(x.sig)) u.rd.push_back(x.sig); });
                    if (in.k == IK::Assign && ok(in.sig)) { u.wr.push_back(in.sig); if (!isPriv(in.sig)) u.live = true; }
                    else if (in.k != IK::Assign && in.k != IK::Jmp && in.k != IK::JmpIfNot) u.live = true;
                }
            }
            for (int n : dom)
                for (auto& in : procs[n].prog->code) touchAll(in, live);
            for (bool ch = true; ch;) {
                ch = false;
                for (auto& u : us) {
                    if (!u.live) for (int w : u.wr) if (live[w]) { u.live = true; break; }
                    if (u.live) for (int r : u.rd) if (!live[r]) { live[r] = 1; ch = true; }
                }
            }
            size_t nDead = 0;
            for (auto& u : us) {
                if (u.live) continue;
                nDead++;
                if (u.one) deadIn.insert(u.one); else deadProc[u.proc] = 1;
                for (int w : u.wr) deadSig[w] = 1;
            }
            for (int n : dom)
                for (auto& in : procs[n].prog->code) if (in.k == IK::Nba && ok(in.sig)) deadSig[in.sig] = 0;
            if (std::getenv("MSIM_WHYCYCLE"))
                std::fprintf(stderr, "cycle domain: %zu copies read at their source, %zu of %zu comb units have no reader\n", nAlias, nDead, us.size());
        }
        struct Slot { int sig, hi, lo; };
        std::vector<Slot> slots;
        bool dynArrNba = false;   // a domain process writes an array element chosen at run time
        std::vector<int> slotProc;   // the domain process of each slot
        std::ostringstream fns, fns2;
        // 2-state mode: when no signal the domain reads holds X or Z, every
        // statement takes its 2-state path, so a second copy without the
        // checks runs instead (dsN2, combQ). Only when every statement has one.
        bool two = twoState;
        std::set<int> ext;   // signals in S.sigs that the domain reads
        // Temporaries of a domain process: only it reads them, and it runs
        // its 4-state body (its 2-state copy calls that one), so an X they
        // hold does not keep the rest of the domain off its 2-state path.
        std::set<int> tmpSig;
        for (int n : dom)
            for (auto& in : procs[n].prog->code) if (in.k == IK::Assign && in.tmp) tmpSig.insert(in.sig);
        auto noteRefs = [&](const E& e) {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->op == Op::Ref && !isPriv(x->sig) && !tmpSig.count(x->sig)) ext.insert(x->sig);
                for (auto& k : x->kids) f(k);
            };
            f(e);
        };
        std::vector<std::string> dsB4(procs.size()), dsB2(procs.size());
        std::vector<char> dsTmp(procs.size());
        for (int n : dom) {
            const Prog& p = *procs[n].prog;
            size_t end = p.code.size() - 1;
            auto L = [](size_t pc) { return "d_" + std::to_string(pc); };
            std::ostringstream fns, fns2;   // the bodies; shared or emitted below
            bool hasTmp = false;
            for (size_t pc = 1; pc < end; pc++) {
                const Instr& i = p.code[pc];
                fns << L(pc) << ":";
                fns2 << L(pc) << ":";
                noteRefs(i.e);
                switch (i.k) {
                    case IK::Nba: {
                        if (i.arr && !i.bidx) {   // an element chosen at run time: through the kernel (see above)
                            for (auto& a : i.aidx) noteRefs(a);
                            std::string t = dynTarget(i, fast(i.e), " DNB->push_back({s, v, hi, lo});");
                            fns << t;
                            fns2 << t;
                            dynArrNba = true;
                            break;
                        }
                        if (i.arr || !i.aidx.empty() || i.bidx) throw CodegenUnsupported("array or variable-index write");
                        int k = (int)slots.size();
                        slots.push_back({i.sig, i.hi, i.lo});
                        slotProc.push_back(n);
                        fns << " CLP(" << k << ");";
                        fns2 << " CLP(" << k << ");";
                        std::string g, u, sl;
                        bool sp = i.hi < 0 && split(i.e, g, u, sl);
                        // Through the arrays (VA[k], not V<k>): shared code moves k.
                        const std::string K = std::to_string(k), V = " VA[" + K + "]", H = " HA[" + K + "]", F = " FA[" + K + "]";
                        if (sp) fns2 << V << " = " << u << ";" << H << " = 1;\n";
                        else fns2 << F << " = " << fast(i.e) << ";" << H << " = 2;\n";   // no 2-state path: as it is
                        // H: 1 = the 2-state value in V<k>, 2 = the Val in F<k>.
                        if (sp) {
                            if (g.empty()) fns << V << " = " << u << ";" << H << " = 1;\n";
                            else fns << " if (__builtin_expect(" << g << ", 1)) {" << V << " = " << u << ";" << H
                                     << " = 1; } else " << cold(F + " = " + sl + ";" + H + " = 2;") << "\n";
                        } else
                            fns << F << " = " << fast(i.e) << ";" << H << " = 2;\n";
                        break;
                    }
                    case IK::JmpIfNot: {
                        if (i.xt != Instr::NOX) throw CodegenUnsupported("three-way jump in the cycle domain");
                        std::string g, u, sl;
                        if (split(i.e, g, u, sl)) {
                            fns2 << " if (!(" << u << ")) goto " << L(i.target) << ";\n";
                            std::string slowT = "[&]() __attribute__((noinline, cold)) { return truth(" + sl + ") != 1; }()";
                            fns << " if (" << (g.empty() ? "!(" + u + ")" : "(__builtin_expect(" + g + ", 1) ? !(" + u + ") : " + slowT + ")")
                                << ") goto " << L(i.target) << ";\n";
                        } else {
                            fns << " if (truth(" << sl << ") != 1) goto " << L(i.target) << ";\n";
                            fns2 << " if (truth(" << sl << ") != 1) goto " << L(i.target) << ";\n";
                        }
                        break;
                    }
                    case IK::Jmp: fns << " goto " << L(i.target) << ";\n"; fns2 << " goto " << L(i.target) << ";\n"; break;
                    case IK::Assign:
                        if (!i.tmp) throw CodegenUnsupported("instruction in a clocked process");
                        for (auto& a : i.aidx) noteRefs(a);
                        if (i.bidx) { noteRefs(i.bidx); fns << dynTarget(i, fast(i.e), " S.poke(s, v, hi, lo);", true); }
                        else fns << assign(i);
                        hasTmp = true;
                        break;
                    default: throw CodegenUnsupported("instruction in a clocked process");
                }
            }
            fns << L(end) << ": return;\n";
            fns2 << L(end) << ": return;\n";
            dsB4[n] = fns.str();
            dsB2[n] = fns2.str();
            dsTmp[n] = hasTmp;
        }
        emitDomainFns(fns, fns2, dom, procs, dsB4, dsB2, dsTmp);
        // Dense arrays: the hot V and H of neighbouring slots share cache
        // lines (a Val is 56 bytes; interleaved, each slot took its own line).
        o << "static Val FA[" << std::max<size_t>(slots.size(), 1) << "]; static uint64_t VA[" << std::max<size_t>(slots.size(), 1)
          << "]; static uint8_t HA[" << std::max<size_t>(slots.size(), 1) << "];\n";
        for (size_t k = 0; k < slots.size(); k++)
            o << "static Val& F" << k << " = FA[" << k << "]; static uint64_t& V" << k << " = VA[" << k << "]; static uint8_t& H" << k
              << " = HA[" << k << "];\n";
        // The slots written in this time step, in order: the commit visits
        // only those (a large design has tens of thousands of slots, most
        // idle in any one cycle). CLON is off for the parallel path.
        o << "static uint32_t CL[" << std::max<size_t>(slots.size(), 1) << "]; static uint32_t CN; static bool CLOVF, CLON = true;\n"
             "#define CLP(k) do { if (CLON) { if (CN < sizeof CL / sizeof CL[0]) CL[CN++] = (k); else CLOVF = true; } } while (0)\n";
        std::vector<int> privs;
        for (size_t s = 0; s < priv_.size(); s++) if (priv_[s]) privs.push_back((int)s);
        // Only when something watches every change (a VCD opened at run time).
        // One table and a loop: unrolled, the copies were 450 KB of code
        // and the slowest function to compile on Ibex.
        o << "static const struct { PSig* p; int s, w; } SYN[] = {";
        size_t nsyn = 0;
        for (int s : privs)
            if (origOf(s) == s) o << (nsyn++ ? ", " : "") << "{&P" << s << ", " << s << ", " << sigW_[s] << "}";
        if (!nsyn) o << "{nullptr, 0, 0}";
        o << "};\n";
        std::string loop = nsyn ? "    for (auto& e : SYN) " : "    if (false) for (auto& e : SYN) ";
        std::string syncPriv_ = loop + "S.fw(e.s, Val{e.p->v, e.p->x, e.w, e.p->z, false});\n";
        for (auto& [sg, ps] : pieces_) syncPriv_ += "    S.fw(" + std::to_string(sg) + ", " + ex(piecesOf(sg, sigW_[sg] - 1, 0)) + ");\n";
        o << "static void combDead();\nstatic void syncP() {\n    combDead();\n" << syncPriv_ << "}\n";
        // Before the kernel starts (no waiters yet): the values only.
        o << "static void syncInit() {\n" << loop << "{ Val& g = S.sigs[e.s].val; g.v = e.p->v; g.x = e.p->x; g.z = e.p->z; }\n";
        for (auto& [sg, ps] : pieces_)
            o << "    { Val v = " << ex(piecesOf(sg, sigW_[sg] - 1, 0)) << "; Val& g = S.sigs[" << sg << "].val; g.v = v.v; g.x = v.x; g.z = v.z; g.hi = v.hi; }\n";
        o << "}\n";
        // Comb statements first: their reads are part of `ext`, which twoNow checks.
        // One unit per straight-line assign, or per process with if / case
        // (its jumps stay inside the unit). unitIns: the unit's writes, then
        // unitRefs: the signals its conditions read.
        std::vector<std::string> combStmts, combStmts2, commitStmts;
        std::vector<const Instr*> combIns;
        bool serialOnly = false;
        std::vector<std::vector<const Instr*>> unitIns;
        std::vector<int> unitProc;   // the comb process of each unit
        std::vector<std::vector<E>> unitConds;
        // In the 2-state copy a statement without a 2-state path runs as it
        // is; if what it wrote is not known, the pass is abandoned (@BAIL@)
        // and everything runs again the 4-state way: the statements are pure,
        // so the ones already done compute the same values.
        auto stmt2 = [&](const Instr& in) -> std::string {
            std::string g, u, sl;
            if (in.hi >= 0 && !in.bidx && isPriv(in.sig) && split(in.e, g, u, sl)) {   // x and z are already 0
                std::string P_ = "P" + std::to_string(in.sig), L_ = std::to_string(in.lo);
                return "    { const uint64_t m = " + M(in.hi - in.lo + 1) + " << " + L_ + "; " + P_ + ".v = (" + P_ +
                       ".v & ~m) | (((uint64_t)(" + u + ") << " + L_ + ") & m); }\n";
            }
            if (in.hi >= 0 || in.bidx || !split(in.e, g, u, sl)) {
                std::string S_ = std::to_string(in.sig);
                std::string unk = isPriv(in.sig) ? "P" + S_ + ".x"
                                : sigW_[in.sig] <= 64 ? "S.sigs[" + S_ + "].val.x" : "anyX(S.sigs[" + S_ + "].val)";
                return "    {" + fassign(in) + "    if (__builtin_expect(" + unk + " != 0, 0)) @BAIL@ }\n";
            }
            std::string S_ = std::to_string(in.sig);
            if (isPriv(in.sig))   // x and z are already 0 in 2-state mode
                return "    P" + S_ + ".v = (" + u + ") & " + M(sigW_[in.sig]) + ";\n";
            if (in.drv >= 0)
                return "    S.fwdv(" + S_ + ", " + std::to_string(in.drv) + ", " + u + ", " + std::to_string((int)in.s0) +
                       ", " + std::to_string((int)in.s1) + ");\n";
            return "    " + regWrite(in.sig, u) + "\n";
        };
        for (int i : byRank) {
            const Prog& cp = *procs[i].prog;
            if (combBranches(cp)) {
                std::string c4 = "    {\n", c2 = "    {\n";
                auto L4 = [&](size_t pc) { return "cb" + std::to_string(i) + "_" + std::to_string(pc); };
                auto L2 = [&](size_t pc) { return "cq" + std::to_string(i) + "_" + std::to_string(pc); };
                if (deadProc[i]) {
                    size_t pc = 0;
                    for (; pc < cp.code.size(); pc++) {
                        const Instr& in = cp.code[pc];
                        if (in.k == IK::JmpIfNot)
                            c4 += L4(pc) + ":" + jmpIfNot(in, fast(in.e), "goto " + L4(in.target) + ";", in.xt == Instr::NOX ? "" : "goto " + L4(in.xt) + ";");
                        else if (in.k == IK::Jmp) c4 += L4(pc) + ": goto " + L4(in.target) + ";\n";
                        else if (in.k == IK::Assign) c4 += L4(pc) + ":" + fassign(in);
                        else break;
                    }
                    deadStmts.push_back(c4 + L4(pc) + ": ;\n    }\n");
                    continue;
                }
                if (combAsserts(cp)) {   // reports go through the kernel's queues: one thread
                    // The domain evaluates every unit on each pass, but the
                    // process only runs (and reports again) when a signal it
                    // waits on changed: skip the unit otherwise.
                    o << combCtlDecl(i);
                    const Instr& w = cp.code[cp.code.size() - 2];
                    std::vector<int> ins;
                    if (w.events.size() > 1) for (auto& [sg, ed] : w.events) ins.push_back(sg);
                    else ins.push_back(w.sig);
                    // Reached through tables by process (CIVA, CIP), so that shared code moves them.
                    std::string I_ = std::to_string(i), chk = "    { bool ch = !CIVA[" + I_ + "]; CIVA[" + I_ + "] = true;\n";
                    size_t nw = 0;
                    auto CI = [&](size_t k) { return "CIP[" + I_ + "][" + std::to_string(k) + "]"; };
                    for (int sg : ins) {
                        std::string G_ = std::to_string(sg);
                        if (isPriv(sg)) {
                            for (const char* f : {".v", ".x", ".z"})
                                chk += "      ch |= UPD(" + CI(nw++) + ", P" + G_ + f + ");\n";
                        } else if (sigW_[sg] <= 64) {
                            for (const char* f : {".v", ".x", ".z"})
                                chk += "      ch |= UPD(" + CI(nw++) + ", S.sigs[" + G_ + "].val" + f + ");\n";
                        } else {
                            o << "static Val CW" << I_ << "_" << G_ << ";\n";
                            chk += "      ch |= UPDV(CW" + I_ + "_" + G_ + ", S.sigs[" + G_ + "].val);\n";
                        }
                        if (!isPriv(sg)) ext.insert(sg);   // a private one is in P, and S.sigs keeps its first X
                    }
                    o << "static uint64_t CI" << I_ << "[" << std::max<size_t>(nw, 1) << "]; static const bool CIR" << I_ << " = (CIP[" << I_
                      << "] = CI" << I_ << ", true);\n";
                    chk += "      if (!ch) goto @END@; }\n    CC" + I_ + "->epoch++;\n";
                    auto withEnd = [&](std::string c, const std::string& lab) {
                        c.replace(c.find("@END@"), 5, lab);
                        return c;
                    };
                    c4 += withEnd(chk, L4(cp.code.size() - 2));
                    c2 += withEnd(chk, L2(cp.code.size() - 2));
                    serialOnly = true;
                }
                unitIns.emplace_back();
                unitProc.push_back(i);
                unitConds.emplace_back();
                size_t pc = 0;
                for (; pc < cp.code.size(); pc++) {
                    const Instr& in = cp.code[pc];
                    if (in.k == IK::JmpIfNot) {
                        noteRefs(in.e);
                        unitConds.back().push_back(in.e);
                        std::string g, u, sl;
                        // A known condition is a plain test; the Val and truth() only when a bit is unknown.
                        bool sp = split(in.e, g, u, sl);
                        std::string slow4 = jmpIfNot(in, sp ? sl : fast(in.e), "goto " + L4(in.target) + ";", in.xt == Instr::NOX ? "" : "goto " + L4(in.xt) + ";");
                        if (sp && g.empty()) c4 += L4(pc) + ": if (!(" + u + ")) goto " + L4(in.target) + ";\n";
                        else if (sp) c4 += L4(pc) + ": if (__builtin_expect(" + g + ", 1)) { if (!(" + u + ")) goto " + L4(in.target) + "; } else {" + slow4 + " }\n";
                        else c4 += L4(pc) + ":" + slow4;
                        // Every signal read is known here (twoNow, and no bail so far).
                        if (sp) c2 += L2(pc) + ": if (!(" + u + ")) goto " + L2(in.target) + ";\n";
                        else c2 += L2(pc) + ":" + jmpIfNot(in, sl, "goto " + L2(in.target) + ";", in.xt == Instr::NOX ? "" : "goto " + L2(in.xt) + ";");
                        continue;
                    }
                    if (in.k == IK::Jmp) {
                        c4 += L4(pc) + ": goto " + L4(in.target) + ";\n";
                        c2 += L2(pc) + ": goto " + L2(in.target) + ";\n";
                        continue;
                    }
                    if (in.k == IK::Assert) {
                        std::string a = combAssert(i, in);
                        c4 += L4(pc) + ":" + a;
                        c2 += L2(pc) + ":" + a;
                        continue;
                    }
                    if (in.k != IK::Assign) break;
                    noteRefs(in.e);
                    noteRefs(in.bidx);
                    combIns.push_back(&in);
                    unitIns.back().push_back(&in);
                    c4 += L4(pc) + ":" + fassign(in);
                    c2 += L2(pc) + ":" + stmt2(in);
                }
                combStmts.push_back(c4 + L4(pc) + ": ;\n    }\n");
                combStmts2.push_back(c2 + L2(pc) + ": ;\n    }\n");
                continue;
            }
            for (const Instr& in : cp.code) {
                if (in.k != IK::Assign) break;
                if (deadIn.count(&in)) { deadStmts.push_back("   " + fassign(in)); continue; }
                combStmts.push_back("   " + fassign(in));
                combIns.push_back(&in);
                unitIns.push_back({&in});
                unitProc.push_back(i);
                unitConds.emplace_back();
                noteRefs(in.e);
                combStmts2.push_back(stmt2(in));
            }
        }
        o << "static std::vector<Sim::NbaItem>* DNB;\n";
        o << fns.str();
        if (two) o << fns2.str();
        // 2-state mode holds when every private signal is known (pclean, kept
        // by the writes) and so is every S.sigs signal the domain reads.
        // A table and a loop: one expression of 4000 terms (16 Ibex cores)
        // took the compiler 12 GB and 80 s.
        o << "static bool twoNow() {\n";
        if (two) {
            o << "    static const int T[] = {-1";
            for (int e : ext) if (!scratch(e)) o << ", " << e;
            o << "};\n    if (!pclean) return false;\n    uint64_t x = 0;\n"
                 "    for (size_t k = 1; k < sizeof T / sizeof T[0]; k++) x |= S.sigs[T[k]].val.x;\n    return x == 0;\n";
        } else
            o << "    return false;\n";
        o << "}\n";
        // MSIM_WHY4=<n>: on the n-th 4-state comb pass, name the signals that
        // keep the domain out of 2-state mode.
        o << "static void why4() {\n    static long n = 0; static const char* w = std::getenv(\"MSIM_WHY4\");\n"
             "    if (!w || ++n != std::atol(w)) return;\n";
        // Tables and loops: one line per signal was 1.5 MB of code on 16 Ibex cores.
        o << "    static const int E[] = {-1";
        if (two)
            for (int e : ext) if (!scratch(e)) o << ", " << e;
        o << "};\n    for (int e : E) if (e >= 0 && S.sigs[e].val.x) std::fprintf(stderr, \"4-state: %s\\n\", S.sigs[e].name.c_str());\n";
        o << "    static const int Q[][2] = {{-1, 0}";
        for (int s : privs) if (!scratch(s) && !deadSig[s]) o << ", {" << s << ", " << origOf(s) << "}";
        o << "};\n    for (auto& q : Q) if (q[0] >= 0 && PA[q[0]].x) std::fprintf(stderr, \"4-state (private): %s\\n\", S.sigs[q[1]].name.c_str());\n";
        o << "}\n";
        // While some register keeps an unknown value (one without a reset),
        // the scan finds it every time: look again after 1, 3, 7 ... 255
        // passes only. pclean = false is always safe (the 4-state pass runs).
        o << "static void scanP() {\n    static unsigned skip = 0, wait = 0;\n"
             "    if (wait) { wait--; pclean = false; return; }\n    uint64_t x = 0;\n";
        for (int s : privs) if (!scratch(s) && !deadSig[s]) o << "    x |= P" << s << ".x;\n";
        o << "    pclean = x == 0;\n    skip = x ? (skip < 255 ? skip * 2 + 1 : 255) : 0;\n    wait = skip;\n}\n";
        o << "static void (*const DS[])() = {";
        for (size_t j = 0; j < dom.size(); j++) o << (j ? ", " : "") << "ds" << dom[j];
        o << "};\n";
        if (two) {
            o << "static void (*const DS2[])() = {";
            for (size_t j = 0; j < dom.size(); j++) o << (j ? ", " : "") << "ds" << dom[j] << "_2";
            o << "};\n";
        }
        // Several clock groups: dseq is registered once per group and runs
        // that group's processes; one marker NBA per slot commits them all.
        const bool multi = domGroups_ > 1;
        if (multi) {
            // The processes of each group: DSO[DOFF[g] .. DOFF[g + 1]), indices into DS.
            std::vector<std::vector<size_t>> byG(domGroups_);
            for (size_t j = 0; j < dom.size(); j++) byG[domGroup_[dom[j]]].push_back(j);
            o << "static const uint32_t DSO[] = {";
            size_t c = 0;
            for (auto& v : byG) for (size_t j : v) o << (c++ ? ", " : "") << j;
            o << "};\nstatic const uint32_t DOFF[] = {0";
            c = 0;
            for (auto& v : byG) { c += v.size(); o << ", " << c; }
            o << "};\nstatic bool dpend = false;\n";
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
        for (size_t j = 0; j < unitIns.size(); j++) {
            int s0 = unitIns[j].empty() ? -1 : unitIns[j][0]->sig;
            if (s0 < 0) continue;
            std::vector<int> rs;
            for (auto* in : unitIns[j]) { refsOf(in->e, rs); rs.push_back(in->sig); }
            for (auto& e : unitConds[j]) refsOf(e, rs);
            for (int r : rs) unite(s0, r);
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
        bool par = allPriv && nIsl >= 32 && !serialOnly && !multi && !dynArrNba;
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
        o << "static void dseq(const void* ctx, std::vector<Sim::NbaItem>& nb) {\n    (void)ctx;\n    DNB = &nb;\n";
        if (par) o << "    CLON = false;\n";
        if (par)
            // Nothing else runs before the NBA region (S.fuseOk()): each group
            // computes, commits and settles its islands in one parallel pass.
            o << "    if (S.fuseOk()) {\n"
                 "        const bool t = twoNow();\n"
                 "        const int use = S.domainTune[0].pick(S.threads);\n"
                 "        S.parallelFor(" << G << ", [t](size_t g) { DSG[g](t); CMG[g](t); }, use);\n"
                 "        S.domainTune[0].done(S.threads);\n"
                 "        if (!t || cdirty.exchange(false)) scanP();\n"
                 "        if (S.observed) syncP();\n"
                 "        return;\n    }\n"
                 "    { const bool t = twoNow(); const int use = S.domainTune[1].pick(S.threads);\n"
                 "      S.parallelFor(" << G << ", [t](size_t g) { DSG[g](t); }, use); S.domainTune[1].done(S.threads); }\n";
        else if (multi) {
            o << "    const int grp = (int)(intptr_t)ctx;\n";
            if (two) o << "    if (twoNow()) { for (uint32_t k = DOFF[grp]; k < DOFF[grp + 1]; k++) DS2[DSO[k]](); } else\n";
            o << "    for (uint32_t k = DOFF[grp]; k < DOFF[grp + 1]; k++) DS[DSO[k]]();\n"
                 "    if (dpend) return;\n    dpend = true;\n";
        } else if (two) o << "    if (twoNow()) { for (auto f : DS2) f(); } else\n";
        if (!par && !multi) o << "    for (auto f : DS) f();\n";
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
        auto bail = [](std::string st, const std::string& with) {
            for (size_t at; (at = st.find("@BAIL@")) != std::string::npos;) st.replace(at, 6, with);
            return st;
        };
        std::vector<std::string> combStmtsQ;
        for (auto& st : combStmts2) combStmtsQ.push_back(bail(st, "{ cdirty = true; return; }"));
        std::vector<size_t> allUnits(unitProc.size());
        for (size_t j = 0; j < allUnits.size(); j++) allUnits[j] = j;
        std::vector<std::string> callsP = shareChunks(o, "combP", allUnits, combStmts, unitProc, unitIns, procs, kChunk);
        {
            int nd = chunks("combD", deadStmts);
            o << "static void combDead() {\n";
            for (int j = 0; j < nd; j++) o << "    combD" << j << "();\n";
            o << "}\n";
        }
        std::vector<std::string> callsQ;
        if (two) callsQ = shareChunks(o, "combQ", allUnits, combStmtsQ, unitProc, unitIns, procs, kChunk);
        if (std::getenv("MSIM_WHYSHARE"))
            std::fprintf(stderr, "shared comb code: %zu of %zu bytes are calls to another instance's copy\n", shareSaved_, shareTotal_);
        if (std::getenv("MSIM_WHYSHARE"))
            std::fprintf(stderr, "comb code not shared: %zu bytes in levels with assertions, %zu outside any instance, %zu templates, %zu no match\n",
                         shareWhy_[0], shareWhy_[1], shareWhy_[2], shareWhy_[3]);
        // The clock cone: the comb processes downstream of the outside signals
        // that reach a clock or reset of a clocked process (the top clock and
        // its copies down the hierarchy, gated clocks). When only such a
        // signal changed, which is every clock edge, combCone evaluates that
        // cone instead of the whole domain. Nothing outside the cone reads
        // what the cone writes: the cone is closed downstream.
        coneIns_.clear();
        {
            std::vector<char> wr(sigW_.size(), 0), inCone(procs.size(), 0), coneSig(sigW_.size(), 0);
            for (int i : byRank)
                for (auto& in : procs[i].prog->code) {
                    if (in.k == IK::WaitEdge) break;
                    if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < wr.size()) wr[in.sig] = 1;
                }
            for (auto& sl : slots) wr[sl.sig] = 1;
            auto outs = [&](int i, auto&& f) {
                for (auto& in : procs[i].prog->code) {
                    if (in.k == IK::WaitEdge) break;
                    if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < wr.size()) f(in.sig);
                }
            };
            // Backward from the clocks: the outside signals that feed them.
            std::vector<char> need(sigW_.size(), 0);
            for (size_t x = 0; x < need.size(); x++) need[x] = clock_[x];
            for (bool again = true; again;) {
                again = false;
                for (int i : byRank) {
                    bool feeds = false;
                    outs(i, [&](int sg) { feeds |= need[sg] != 0; });
                    if (!feeds) continue;
                    for (int x : procs[i].combIns)
                        if (x >= 0 && (size_t)x < need.size() && !need[x]) { need[x] = 1; again = true; }
                }
            }
            for (int i : byRank)
                for (int x : procs[i].combIns)
                    if (x >= 0 && (size_t)x < need.size() && need[x] && !wr[x] && !coneSig[x]) { coneSig[x] = 1; coneIns_.push_back(x); }
            // Forward from each of those: everything that depends on it. Only
            // a signal with a small cone is worth a pass of its own (a reset
            // that the data logic reads is not).
            std::vector<int> cands;
            cands.swap(coneIns_);
            std::fill(coneSig.begin(), coneSig.end(), 0);
            size_t nCone = 0;
            for (int c : cands) {
                std::vector<char> in1(procs.size(), 0), sg1(sigW_.size(), 0);
                sg1[c] = 1;
                size_t n1 = 0;
                bool asserts = false;
                for (bool again = true; again;) {
                    again = false;
                    for (int i : byRank) {
                        if (in1[i]) continue;
                        bool reads = false;
                        for (int x : procs[i].combIns) reads |= x >= 0 && (size_t)x < sg1.size() && sg1[x];
                        if (!reads) continue;
                        in1[i] = 1;
                        n1++;
                        again = true;
                        asserts |= combAsserts(*procs[i].prog);
                        outs(i, [&](int sg) { sg1[sg] = 1; });
                    }
                }
                if (std::getenv("MSIM_WHYCONE"))
                    std::fprintf(stderr, "cone of signal %d: %zu of %zu processes%s\n", c, n1, byRank.size(), asserts ? ", asserts" : "");
                if (!n1 || asserts || n1 * 8 > byRank.size() + 512) continue;
                coneIns_.push_back(c);
                for (int i : byRank)
                    if (in1[i] && !inCone[i]) { inCone[i] = 1; nCone++; }
            }
            if (nCone == 0) coneIns_.clear();
            // What the cone writes, only the cone reads: those signals do not wake combAll either.
            coneOuts_.clear();
            if (!coneIns_.empty())
                for (int i : byRank)
                    if (inCone[i]) outs(i, [&](int sg) { coneOuts_.push_back(sg); });
            std::sort(coneOuts_.begin(), coneOuts_.end());
            if (!coneIns_.empty()) {
                std::sort(coneIns_.begin(), coneIns_.end());
                bool privW = false;
                std::vector<size_t> coneUnits;
                for (size_t j = 0; j < unitIns.size(); j++) {
                    if (!inCone[unitProc[j]]) continue;
                    coneUnits.push_back(j);
                    for (auto* in : unitIns[j]) privW |= isPriv(in->sig);
                }
                std::vector<std::string> callsC = shareChunks(o, "combC", coneUnits, combStmts, unitProc, unitIns, procs, kChunk);
                o << "static void combCone(const void*) {\n";
                for (auto& c : callsC) o << "    " << c << ";\n";
                if (privW) o << "    scanP();\n";
                o << "    if (S.observed) syncP();\n}\n";
            }
        }
        o << "static void combAll(const void*) {\n";
        if (two) {
            o << "    if (twoNow()) {\n        cdirty = false;\n";
            for (auto& c : callsQ) o << "        " << c << "; if (cdirty) goto slow;\n";
            o << "        goto done;\n    }\n    slow: ;\n";
            for (auto& c : callsP) o << "    " << c << ";\n";
            o << "    scanP(); why4();\n    done: ;\n";
        } else
            for (auto& c : callsP) o << "    " << c << ";\n";
        o << "    if (S.observed) syncP();\n}\n";
        if (par) {
            // Per group: its slots' commits, then its comb statements in rank order.
            std::vector<std::string> gCommit(G), gComb(G), gComb2(G);
            for (size_t k = 0; k < slots.size(); k++) gCommit[groupOf(slots[k].sig)] += commitStmts[k];
            for (size_t j = 0; j < unitIns.size(); j++) {
                if (unitIns[j].empty()) continue;
                int g = groupOf(unitIns[j][0]->sig);
                gComb[g] += combStmts[j];
                gComb2[g] += combStmts2[j];
            }
            for (int g = 0; g < G; g++)
                o << "static void cmG" << g << "(bool two) {\n    const bool obs = false; (void)obs;\n" << gCommit[g]
                  << "    if (two) {\n" << bail(gComb2[g], "{ cdirty = true; goto slow; }") << "        return;\n    }\n    slow: ;\n"
                  << gComb[g] << "}\n";
            // twoNow() before the commits: in 2-state mode every slot holds a
            // known value, so the commits keep every private signal known.
            o << "static void fcommit() {\n"
                 "    const bool t = twoNow();\n"
                 "    const int use = S.domainTune[2].pick(S.threads);\n"
                 "    S.parallelFor(" << G << ", [t](size_t g) { CMG[g](t); }, use);\n"
                 "    S.domainTune[2].done(S.threads);\n"
                 "    if (!t || cdirty.exchange(false)) scanP();\n"
                 "    if (S.observed) syncP();\n}\n\n";
            return;
        }
        // The commit is data: one table row per slot and one small loop
        // over the slots written (generated code per slot was 300 bytes
        // each, 16 MB on bench/gen_soc.py 512, and the run waited on the
        // instruction cache). Part writes, nets, clocks and wide signals keep
        // their generated statement (kind 2).
        std::vector<int> kind(slots.size(), 2);
        for (size_t k = 0; k < slots.size(); k++) {
            const Slot& sl = slots[k];
            if (sl.hi >= 0) continue;
            if (isPriv(sl.sig)) kind[k] = 0;
            else if (!(sigW_[sl.sig] > 64 || sigNet_[sl.sig] || clock_[sl.sig])) kind[k] = 1;
        }
        int nm = 0;
        std::string cms;
        for (size_t a = 0; a < slots.size(); a += kChunk, nm++) {
            bool any = false;
            for (size_t b = a; b < slots.size() && b < a + kChunk; b++) any |= kind[b] == 2;
            if (!any) { cms += std::string(nm ? ", " : "") + "nullptr"; continue; }
            o << "static void commitS" << nm << "(uint32_t k) {\n    const bool obs = S.observed; (void)obs;\n    switch (k) {\n";
            for (size_t b = a; b < slots.size() && b < a + kChunk; b++)
                if (kind[b] == 2) o << "    case " << b << ":" << commitStmts[b] << "    break;\n";
            o << "    }\n}\n";
            cms += std::string(nm ? ", " : "") + "commitS" + std::to_string(nm);
        }
        o << "static void (*const CMS[])(uint32_t) = {" << (nm ? cms : "nullptr") << "};\n";
        o << "struct CTRow { PSig* p; uint64_t m; int32_t sig, w; uint8_t kind; };\n";
        o << "static const CTRow CTAB[] = {";
        for (size_t k = 0; k < slots.size(); k++) {
            int sg = slots[k].sig;
            o << (k ? ",\n" : "\n") << "{" << (kind[k] == 0 ? "&P" + std::to_string(sg) : std::string("nullptr")) << ", " << M(sigW_[sg])
              << ", " << sg << ", " << sigW_[sg] << ", " << kind[k] << "}";
        }
        if (slots.empty()) o << "{nullptr, 0, 0, 0, 2}";
        o << "};\n";
        o << "static void commitOne(uint32_t k, bool obs) {\n"
             "    const uint8_t h = HA[k];\n"
             "    if (!h) return;\n"
             "    const CTRow& t = CTAB[k];\n"
             "    if (t.kind == 0) {\n"
             "        HA[k] = 0;\n"
             "        if (h == 1) { t.p->v = VA[k] & t.m; t.p->x = 0; t.p->z = 0; } else { PW(*t.p, t.w, FA[k]); pclean = false; }\n"
             "    } else if (t.kind == 1) {\n"
             "        HA[k] = 0;\n"
             "        if (h != 1) S.fw(t.sig, FA[k]);\n"
             "        else if (__builtin_expect(!obs && S.waiters[t.sig].empty(), 1)) {\n"
             "            Val& q = S.sigs[t.sig].val; q.v = VA[k] & t.m; q.x = 0; q.z = 0; q.real = false; q.str = false;\n"
             "        } else S.fwSlow(t.sig, make(VA[k] & t.m, 0, t.w));\n"
             "    } else CMS[k / " << kChunk << "](k);\n"
             "}\n";
        o << "static void fcommit() {" << (multi ? " dpend = false;" : "");
        o << " const bool obs = S.observed;"
             " if (!CLOVF) { for (uint32_t j = 0; j < CN; j++) commitOne(CL[j], obs); }"
             " else for (uint32_t k = 0; k < " << slots.size() << "; k++) commitOne(k, obs);"
             " CN = 0; CLOVF = false;";
        // The comb pass is queued, not run here: other NBAs of this time
        // step (a testbench driving the inputs) may still follow in the
        // queue, and one pass after all of them is enough.
        o << " S.markFast(); if (S.observed) syncP(); }\n\n";
    }

    // A clocked process: the body after its wait, NBAs into `nb`.
    void emitSeq(std::ostream& o, int n, const Prog& p, const std::string& kind) {
        size_t end = p.code.size() - 1;
        auto L = [](size_t pc) { return "s_" + std::to_string(pc); };
        o << "// process " << n << " (" << kind << ", clocked)\n";
        o << "static void seq" << n << "(const void*, std::vector<Sim::NbaItem>& nb) {" << CH(&p, 0) << "\n";
        if (p.xpFast) o << " if (S.xAt != S.now) goto " << L(p.xpFast) << ";\n";   // msim -xprop: no edge can be ambiguous
        for (size_t pc = 1; pc < end; pc++) {
            const Instr& i = p.code[pc];
            o << L(pc) << ":" << CH(&p, pc);
            switch (i.k) {
                case IK::Nba:
                    if (i.drv >= 0 || i.dyn >= 0 || i.nbaLater) throw CodegenUnsupported("NBA kind in a clocked process");
                    if (i.arr || i.bidx) o << dynTarget(i, fast(i.e), " nb.push_back({s, v, hi, lo});");
                    else o << " nb.push_back({" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << "});\n";
                    break;
                case IK::JmpIfNot: o << jmpIfNot(i, fast(i.e), jumpTo(&p, pc, L(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, L(i.xt))); break;
                case IK::Jmp: o << " goto " << L(i.target) << ";\n"; break;
                case IK::Assign:   // a temporary of its own (Instr::tmp)
                    o << " S.poke(" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << ");\n";
                    break;
                default: throw CodegenUnsupported("instruction in a clocked process");
            }
        }
        o << L(end) << ": return;\n}\n\n";
    }

    // A combinational process: its leading Assign instructions as a function.
    void emitComb(std::ostream& o, int n, const Prog& p, const std::string& kind) {
        o << "// process " << n << " (" << kind << ", combinational)\n";
        const bool as = combAsserts(p);
        if (as) o << combCtlDecl(n);
        o << "static void comb" << n << "(const void*) {\n";
        if (as) o << "    CC" << n << "->epoch++;\n";
        auto L = [&](size_t pc) { return "c" + std::to_string(n) + "_" + std::to_string(pc); };
        size_t pc = 0;
        for (; pc < p.code.size(); pc++) {
            const Instr& i = p.code[pc];
            if (i.k == IK::JmpIfNot) {
                o << L(pc) << ":" << CH(&p, pc) << jmpIfNot(i, fast(i.e), jumpTo(&p, pc, L(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, L(i.xt)));
                continue;
            }
            if (i.k == IK::Jmp) { o << L(pc) << ":" << CH(&p, pc) << " goto " << L(i.target) << ";\n"; continue; }
            if (i.k == IK::Assert) { o << L(pc) << ":" << CH(&p, pc) << combAssert(n, i); continue; }
            if (i.k != IK::Assign) break;
            if (i.arr) throw CodegenUnsupported("array write in a static combinational process");
            if (i.bidx) {
                o << L(pc) << ":" << CH(&p, pc) << dynTarget(i, fast(i.e), " S.set(s, hi >= 0 ? v_insert(S.get(s), v, hi, lo) : v);");
                continue;
            }
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
        // msim -upf: a process of a domain that is off sleeps until it powers
        // up, then starts over from its top (exec.inc, 1801 9.5.3).
        const int pd = upf ? entry.pd : -1;
        std::string gate;
        if (pd >= 0) {
            std::string m = "g_pdMode[" + std::to_string(pd) + "] == 5";
            gate = " if (__builtin_expect(" + m + ", 0)) { while (" + m + ") co_await Wait{g_pdSig[" + std::to_string(pd) + "], ANY};" +
                   (calls ? " rs.clear();" : "") + " goto " + L(0, 0) + "; }\n";
            o << gate;
        }
        // ...and stores corrupt values while its simstate says so (upfWrite).
        auto W = [&](const Instr& i, const std::string& v) {
            if (pd < 0 || i.tmp || i.sig < 0 || isRel(i.sig) || i.arr) return v;
            return "UPFW(" + std::to_string(pd) + ", " + (entry.pstate ? "true" : "false") + ", " + std::to_string(i.sig) + ", " +
                   std::to_string(i.hi) + ", " + std::to_string(i.lo) + ", " + v + ")";
        };
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
                if (!p.roots.empty() || !p.spans.empty()) throw CodegenUnsupported("disable of a named block");
                if (i.k == IK::Finish && i.hi > 0) throw CodegenUnsupported("$fatal exit status");
                if (i.fd) throw CodegenUnsupported("$fstrobe / $fmonitor");
                if (!i.dly.empty()) throw CodegenUnsupported("delayed continuous assignment");
                if (i.k == IK::ReadMem || i.k == IK::WriteMem) {   // $readmem / $writemem (21.4, 21.5)
                    if (!i.arr || readsPriv(i.e) || i.pw == 2 || !i.aidx.empty()) throw CodegenUnsupported("$readmem form");
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
                    o << dynTarget(i, W(i, fast(i.e)), i.k == IK::Nba ? " S.nba(s, v, hi, lo);"
                                                                      : " S.set(s, hi >= 0 ? v_insert(S.get(s), v, hi, lo) : v);");
                    continue;
                }
                switch (i.k) {
                    case IK::Assign: o << (pd >= 0 ? assign(i, W(i, fast(i.e))) : assign(i)); break;
                    case IK::Nba:
                        o << " S.nba(" << i.sig << ", " << W(i, fast(i.e)) << ", " << i.hi << ", " << i.lo << ");\n";
                        break;
                    case IK::JmpIfNot:
                        o << jmpIfNot(i, fast(i.e), jumpTo(&p, pc, tgt(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, tgt(i.xt)));
                        break;
                    case IK::Jmp: o << " goto " << tgt(i.target) << ";\n"; break;
                    case IK::WaitEdge:
                        if (i.events.size() > 1) {
                            events_.push_back(i.events);
                            o << " co_await WaitAny{&EV" << events_.size() - 1 << "};";
                        } else
                            o << " co_await Wait{" << i.sig << ", " << edge(i.edge) << "};";
                        o << (asserting ? " ctl->epoch++;\n" : "\n");   // a deferred assertion flush point (16.4.2)
                        o << gate;
                        break;
                    case IK::WaitDelay: o << " co_await Delay{" << i.delay << "ULL};\n" << gate; break;
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
                        if (priv) { hitPriv(i.e); for (auto* cs : {&a.passCap, &a.failCap}) for (auto& c : *cs) hitPriv(c); }
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
                            hitPriv(a);
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
                           std::vector<std::pair<const Prog*, size_t>>* covSlots = nullptr, int split = 1,
                           int precExp = -9, const std::string& timescale = "1ns", bool upf = false) {
    std::set<int> noPriv;
    for (int pass = 0;; pass++) {
        Codegen g;
        g.files = split;
        g.precExp = precExp;
        g.timescale = timescale;
        g.twoState = twoState;
        g.cycle = cycle;
        g.privates = privates && pass < 8;   // each pass keeps the signals the last one tripped on in S.sigs
        g.noPriv = noPriv;
        g.cov = covSlots != nullptr;
        g.upf = upf;
        std::string src = g.run(sigs, procs, parts);
        if (!g.privHits_.empty()) {
            if (!g.privates) throw CodegenUnsupported("a private signal");
            for (int h : g.privHits_) noPriv.insert(g.origOf(h));
            continue;
        }
        if (covSlots) *covSlots = std::move(g.covSlots);
        return src;
    }
}
