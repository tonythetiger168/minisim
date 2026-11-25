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
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <chrono>
#include <fstream>

// MSIM_PHASES=1: time and memory after each step of elaboration and
// -compile, on stderr (for a large design that runs out of time or memory).
inline void phaseMark(const char* what) {
    static const bool on = std::getenv("MSIM_PHASES") != nullptr;
    if (!on) return;
    static const auto t0 = std::chrono::steady_clock::now();
    long rss = 0, hwm = 0;
    std::ifstream f("/proc/self/status");
    for (std::string k; f >> k;) {
        if (k == "VmRSS:") f >> rss;
        else if (k == "VmHWM:") f >> hwm;
    }
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "msim: phase %-22s %8.1f s  rss %6.2f GB  peak %6.2f GB\n", what, t, rss / 1048576.0, hwm / 1048576.0);
}
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
    // A narrow signal that is not all X before time 0 (an `always @*` arming
    // flag, msim.cpp rotateAtStar): its value, set right after S.sig.
    bool init = false;
    uint64_t v = 0, x = 0, z = 0;
    bool zeroWide = false;   // a wider one that is all 0 (-x-initial 0)
    // A string variable with a value before time 0 (a localparam struct's
    // string member, 6.20.2): its text, set right after S.sig.
    bool strInit = false;
    std::string strText;
    int mirror = -1;   // a copy msim dropped (substituteWires): the signal it reads, for waveforms
    // A register whose only write in the whole design is one whole-signal NBA
    // (msim.cpp soleNbaTargets): a clocked function skips that NBA when it
    // would not change the value (Sim::nbaKeep).
    bool soleNba = false;
    // A temporary of one process (an inlined function's automatic variable,
    // "_tf..."), which no other process reads or waits on (msim.cpp
    // quietTemps): its writes are stores, with no change event.
    bool quiet = false;
};

// msim -compile: the DPI-C libraries simv loads (the user's C files built
// into one, and -sv_lib), in msim's order (msim.cpp).
inline std::vector<std::string>& compileDpiLibs() { static std::vector<std::string> v; return v; }

struct CodegenProc {
    const Prog* prog = nullptr;
    std::string kind;
    // >= 0: a combinational process (prog.hpp: combShape) at this rank of the
    // static schedule. It is emitted as a plain function and registered with
    // Sim::addComb in rank order instead of being spawned.
    int combRank = -1;
    std::vector<int> combIns;
    int combLevel = 0;
    bool combOnCycle = false;   // on a cycle of comb ranks: Sim::Comb::onCycle (runDeferred)
    // A clocked process (prog.hpp: seqShape): emitted as a function that
    // returns its NBAs, registered with Sim::addSeq on these events.
    bool seq = false;
    std::vector<std::pair<int, Edge>> seqEv;
    bool loops = true;   // LoweredProc::loops (false: a constant continuous assign)
    // Mixed mode only: left to the interpreter whatever its code (a program's
    // process runs in the reactive region, 24.3).
    bool interp = false;
};

// Arguments of Sim::buildPartitions (by rank; fixed NBA targets).
struct CodegenParts {
    bool on = false;
    std::vector<std::vector<int>> outs, ins;
    std::vector<int> targets;
};

// Mixed mode's result (codegen(..., interp)): on when the output is the
// library msim loads, with the processes left to the interpreter and why.
struct CodegenMix {
    bool on = false;
    bool domain = false;   // the cycle domain may stay when nothing is interpreted
    std::vector<const Instr*> ins;   // g_mixIns: the instructions the library runs with msim's code
    std::vector<std::pair<int, std::string>> procs;
};

class Codegen {
public:
    std::string run(const std::vector<CodegenSig>& sigs, const std::vector<CodegenProc>& procs,
                    const CodegenParts& parts = {}) {
        for (auto& g : sigs) { quiet_.push_back(g.quiet); sigW_.push_back(g.w); sigNet_.push_back(g.net); soleNba_.push_back(g.soleNba); scratch_.push_back(g.name.rfind("_tf", 0) == 0); sigName_.push_back(g.name); }
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
        run_ = dom;
        orderDomain(procs, sigs, run_);   // the order they run in; dom keeps the code layout
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
            if (std::getenv("MSIM_WHYCYCLE")) {
                size_t nseq = 0;
                for (auto& p : procs) nseq += p.seq && p.combRank < 0;
                std::fprintf(stderr, "cycle domain: %zu clocked processes, %zu NBAs, %zu comb statements (%zu clocked functions%s)\n", dom.size(), nba, comb, nseq,
                             cycle ? "" : "; off: a comb cycle, -upf or -no-cycle");
            }
            if (comb > 64 && comb > 64 * nba && !getenv("MSIM_XCYC")) dom.clear();
            // Many clock groups (an SoC's gated and divided clocks, ticking at
            // different times): each group's commit evaluates every comb
            // statement. OpenTitan Earl Grey, 779 groups, ran 4.6x slower in
            // the domain than event-driven.
            if (domGroups_ > 64 && !getenv("MSIM_XCYC")) {
                if (std::getenv("MSIM_WHYCYCLE")) std::fprintf(stderr, "cycle domain off: %d clock groups\n", domGroups_);
                dom.clear();
            }
        }
        if (!dom.empty()) wakeEvents(procs, dom);
        armK_.clear(); armIns_.clear(); armSet_.clear();
        phaseMark(" cg: domain chosen");
        if (!dom.empty() && privates) privateSigs(sigs, procs, dom);
        phaseMark(" cg: private signals");
        std::vector<CodegenProc> wideProcs;
        if (!dom.empty() && privates && !std::getenv("MSIM_NOWIDE")) wideProcs = splitWide(procs, dom);
        std::vector<char> inDomV(procs.size(), 0);
        for (int i : dom) inDomV[(size_t)i] = 1;
        // Without the cycle domain each comb and clocked process is a function
        // of its own: one copy per module instead (shareEv). MSIM_NOSHARE=1: off.
        const bool evShare = dom.empty() && !cov && !std::getenv("MSIM_NOSHARE");
        std::vector<EvUnit> evUnits;
        evName_.assign(procs.size(), "");
        evPlain_.assign(procs.size(), 0);
        evAt_.assign(procs.size(), -1);
        evDT_.clear();
        for (size_t i = 0; i < procs.size(); i++) {
            const bool inDom = inDomV[i] != 0;
            if (upf && procs[i].prog->pd >= 0 && (procs[i].combRank >= 0 || procs[i].seq || inDom))
                throw CodegenUnsupported("a statically scheduled process of a power domain");
            if (procs[i].combRank >= 0 || inDom)
                for (auto& in : procs[i].prog->code) noFrames(in);
            if (procs[i].combRank >= 0) {
                if (dom.empty()) {   // else: inlined in combAll
                    std::ostringstream c;
                    emitComb(c, (int)i, *procs[i].prog, procs[i].kind);
                    if (evShare && !combAsserts(*procs[i].prog)) evUnits.push_back({(int)i, false, c.str()});
                    else chunks.push_back(c.str());
                }
                if ((size_t)procs[i].combRank >= byRank.size()) byRank.resize(procs[i].combRank + 1, -1);
                byRank[procs[i].combRank] = (int)i;
            } else if (inDom)
                continue;   // emitCycle
            else if (procs[i].seq && !mix) {
                std::ostringstream c;
                const bool fn = seqOrProc(c, procs, i);
                if (evShare && fn) evUnits.push_back({(int)i, true, c.str()});
                else if (evShare && !upf && evPlain(c.str(), (int)i)) evUnits.push_back({(int)i, false, plainText(c.str(), (int)i)});
                else chunks.push_back(c.str());
            }
            else if (!mix) {
                std::ostringstream c;
                emitProc(c, (int)i, *procs[i].prog, procs[i].kind);
                if (evShare && !upf && procs[i].kind != "init" && evPlain(c.str(), (int)i)) evUnits.push_back({(int)i, false, plainText(c.str(), (int)i)});
                else chunks.push_back(c.str());
            }
            else {   // mixed mode: a process codegen cannot compile runs in msim's interpreter
                std::ostringstream c;
                try {
                    if (procs[i].interp) throw CodegenUnsupported("a program process, or a gate whose delays change at run time");
                    const bool fn = procs[i].seq && seqOrProc(c, procs, i);
                    if (!procs[i].seq) emitProc(c, (int)i, *procs[i].prog, procs[i].kind);
                    if (evShare && fn) evUnits.push_back({(int)i, true, c.str()});
                    else if (evShare && !upf && procs[i].kind != "init" && evPlain(c.str(), (int)i)) evUnits.push_back({(int)i, false, plainText(c.str(), (int)i)});
                    else chunks.push_back(c.str());
                } catch (const CodegenUnsupported& ex) {
                    // A clocked one runs as an ordinary process there, as
                    // with msim -no-static-seq.
                    if (procs[i].seq) { mixSeq_.resize(procs.size()); mixSeq_[i] = 1; }
                    interp.push_back({(int)i, ex.what()});
                    chunks.push_back("// process " + std::to_string(i) + " (" + procs[i].kind + "): interpreted by msim\n"
                                     "static Task proc" + std::to_string(i) + "() {\n    return g_mixInterp(" + std::to_string(i) + ");\n}\n\n");
                }
            }
        }

        if (!evUnits.empty()) shareEv(evUnits, chunks);
        {   // the helpers are chunks of their own: they spread over the files
            std::vector<std::string> out;
            out.reserve(chunks.size());
            const char* om = std::getenv("MSIM_OUTLINEMIN");   // tests: outline small functions too
            const size_t outMin = om ? std::strtoull(om, nullptr, 10) : kOutlineMin;
            for (auto& c : chunks) {
                if (c.size() > outMin) for (auto& h : splitSeq(c)) out.push_back(std::move(h));
                if (c.size() > outMin) for (auto& h : outlineRuns(c)) out.push_back(std::move(h));
                if (!c.empty()) out.push_back(std::move(c));
            }
            chunks = std::move(out);
        }
        phaseMark(" cg: processes");
        if (!dom.empty()) { std::ostringstream c; emitCycle(c, wideProcs.empty() ? procs : wideProcs, dom, byRank); cyc = c.str(); }
        phaseMark(" cg: cycle domain");
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
        // A large design gets more files than cores: the compiler takes about
        // 0.25 GB per MB of generated code, so 1024 PicoRV32 cores in four
        // 35 MB files ran a 16 GB machine out of memory. At most 2 MB a file:
        // OpenTitan's 4 MB parts took 2.6 GB each at -O2, two at a time on
        // 16 GB beside msim; the header comes precompiled (design.hpp.gch).
        size_t want = std::max<size_t>(files, (chunkBytes + (2u << 20) - 1) / (2u << 20));
        int nf = files < 0 ? (int)std::min<size_t>(-files, nChunks)
               : files > 1 ? (int)std::min({want, chunkBytes / 65536, nChunks}) : 1;
        nf = std::max(nf, 1);
        if (nf > 1) {
            for (auto& c : cycFns) chunks.push_back(std::move(c));
            std::vector<std::string>().swap(cycFns);
            std::string().swap(cyc);
        }
        const char* SV = nf > 1 ? "inline " : "static ";

        std::ostringstream o;
        o << "// Generated by msim -compile. Do not edit.\n"
             "#define MINISIM_NO_MAIN\n"
          << (mix ? "#define MINISIM_EXTERN_S   // msim's own (mixed mode: a library msim loads)\n"
              : kernelObj ? "#define MINISIM_EXTERN_S   // defined in the kernel object\n#define MINISIM_CLIENT\n"
              : nf > 1 ? "#define MINISIM_EXTERN_S   // defined once, in design.cpp\n" : "") <<
             "#include \"minisim.cpp\"\n"
             "#include <cstdlib>\n"
             "#include <cstring>\n"
             "#include <thread>\n\n"
             "#pragma GCC diagnostic ignored \"-Wunused-label\"\n\n"
             "static inline Val R(int s, bool sg) { Val v = S.get(s); v.sgn = sg; return v; }\n"
             "#define MSIMI(k) (*static_cast<const Instr*>(g_mixIns[k]))   // mixed mode: msim's instruction k\n"
             "#define SIGN(n) (n)   // a signal number (shared code moves it: refScan)\n"
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
             "template <class... T> static inline Val CAT(const T&... p) {\n"
             "    const int tw = (0 + ... + p.w);\n"
             "    if (tw <= 64) { Val r{0, 0, 0}; ((r = cat1(r, p)), ...); return r; }\n"
             "    Val r = make(0, 0, tw);   // wide: each part copied into place once\n"
             "    int off = tw;\n"
             "    ((off -= p.w, p.w > 0 ? copyBits(r, off, p, 0, p.w) : void()), ...);\n"
             "    return r;\n"
             "}\n"
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
             "// A process that never waits, as a plain function (Codegen::evPlain).\n"
             "[[maybe_unused]] static Task evOnce(void (*f)(const void*), const void* x) { f(x); co_return; }\n"
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
             "// The same for an array whose elements are private (PA, from the first one's slot).\n"
             "static inline Val APRV(const ArrShape& a, std::initializer_list<Val> ix, const PSig* p0) {\n"
             "    int s = AOFF(a, ix);\n"
             "    if (s < 0) return a.twoState ? make(0, 0, a.elemW, 0, a.sgn) : xfill(a.elemW, a.sgn);\n"
             "    return PR(p0[s - a.base], a.elemW, a.sgn);\n"
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
        for (size_t i = 0; i < lays_.size(); i++) o << SV << "FrameLayout LAY" << i << ";\n";
        for (size_t i = 0; i < mems_.size(); i++) o << SV << "Instr MI" << i << ";\n";
        // Arrays, like PA: shared comb code reaches them as AIA[k + D7], CCA[n + D8].
        o << SV << "Instr AIA[" << std::max<size_t>(asrts_.size(), 1) << "];\n";
        for (size_t i = 0; i < asrts_.size(); i++) o << SV << "Instr& AI" << i << " = AIA[" << i << "];\n";
        o << SV << "std::shared_ptr<ProcCtl> CCA[" << ccMax_ + 1 << "];\n";
        o << SV << "uint8_t CIVA[" << ccMax_ + 1 << "];\n" << SV << "uint64_t* CIP[" << ccMax_ + 1 << "];\n";
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
            // A variable known before time 0 (CodegenSig::init: -x-initial 0, an
            // arming flag) starts with that value.
            o << "struct PInitRow { uint32_t s; uint64_t v, x, z; };\nstatic const PInitRow PINIT[] = {";
            size_t c = 0;
            for (size_t x = 0; x < priv_.size(); x++)
                if (priv_[x]) {
                    const bool k = x < sigs.size() && sigs[x].init && !sigNet_[x];
                    if (auto ci = constInit_.find((int)x); ci != constInit_.end()) {   // a constant continuous assign (privateSigs)
                        o << (c++ ? ", " : "") << "{" << x << ", " << ci->second << "ULL, 0, 0}";
                        continue;
                    }
                    o << (c++ ? ", " : "") << "{" << x << ", " << (k ? std::to_string(sigs[x].v) + "ULL" : "0") << ", "
                      << (k ? std::to_string(sigs[x].x) + "ULL" : M(sigW_[x])) << ", "
                      << (k ? std::to_string(sigs[x].z) + "ULL" : sigNet_[x] ? M(sigW_[x]) : "0") << "}";
                }
            if (!c) o << "{0, 0, 0, 0}";
            o << "};\n";
            if (c) privInit_ = "    for (auto& e : PINIT) { PA[e.s].v = e.v; PA[e.s].x = e.x; PA[e.s].z = e.z; }\n";
        }
        if (!armIns_.empty()) {   // armChk: the private inputs of each arming process, and their values when it began to wait
            std::string in, off = "0";
            size_t n = 0;
            for (auto& a : armIns_) {
                for (int x : a) in += (n++ ? ", " : "") + std::to_string(x);
                off += ", " + std::to_string(n);
            }
            o << SV << "const uint32_t ARMIN[] = {" << in << "};\n" << SV << "const uint32_t ARMOFF[] = {" << off << "};\n"
              << SV << "PSig ARMSNAP[" << n << "];\n" << SV << "uint8_t ARMST[" << armIns_.size() << "];   // 1 waiting, 2 done\n"
              << SV << "uint32_t ARMLEFT = " << armIns_.size() << ";\n";
        }
        // Startup statements (one per line). Split, they go to functions
        // spread over the files: in main() they made design.cpp the slowest
        // file to compile (Ibex: 1 MB, 42 s).
        std::string initText = privInit_, initCalls;
        for (size_t i = 0; i < sysArgs_.size(); i++) initText += "    A" + std::to_string(i) + " = {" + sysTrees[i] + "};\n";
        for (size_t i = 0; i < evals_.size(); i++) initText += "    ET" + std::to_string(i) + " = " + evTrees[i] + ";\n";
        for (size_t i = 0; i < lays_.size(); i++) {   // frame layouts (emitProc frames)
            const FrameLayout& l = *lays_[i];
            std::string w, v;
            for (size_t k = 0; k < l.w.size(); k++) { w += (k ? ", " : "") + std::to_string(l.w[k]); v += (k ? ", " : "") + val(l.init[k]); }
            initText += "    LAY" + std::to_string(i) + ".w = {" + w + "}; LAY" + std::to_string(i) + ".init = {" + v + "}; LAY" +
                        std::to_string(i) + ".name = " + quote(l.name) + ";\n";
        }
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
        std::unordered_map<std::string, std::string> rename;   // split with relocText: function -> its name after its text
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
            // Signal numbers in the parts: link-time symbols (relocText).
            std::vector<std::set<std::string>> csyms(chunks.size());
            const bool reloc = relocOn() && !mix && !std::getenv("MSIM_FULLHDR");
            std::vector<char> global(chunks.size(), 0);   // design.cpp: dispatch and tables, renumbered by any edit
            std::string globalCode;
            std::vector<std::string> kinds(chunks.size()), pkey(chunks.size());
            if (reloc) {
                buildSymOf();
                // Each chunk's function, by name; the code of one process or one
                // shared template stays in the parts, the rest (combAll, twoNow,
                // the calls of a template with an instance's distances, ...)
                // goes to design.cpp, which every edit rebuilds anyway.
                auto fnName = [](const std::string& c) {
                    size_t at = 0;
                    while (at < c.size() && c.compare(at, 2, "//") == 0) at = c.find('\n', at) + 1;
                    size_t nl = c.find('\n', at);
                    std::string ln = c.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                    std::string name;
                    for (size_t i = 0; i < ln.size();) {
                        if (std::isalpha((unsigned char)ln[i]) || ln[i] == '_') {
                            size_t j = i;
                            while (j < ln.size() && (std::isalnum((unsigned char)ln[j]) || ln[j] == '_')) j++;
                            std::string id = ln.substr(i, j - i);
                            if (j < ln.size() && ln[j] == '(' && id != "__attribute__" && id != "noinline" && id != "cold" && id != "used") { name = id; break; }
                            i = j;
                        } else i++;
                    }
                    return name;
                };
                auto localKind = [](const std::string& n) {
                    size_t a = 0;
                    while (a < n.size() && std::isalpha((unsigned char)n[a])) a++;
                    std::string pre = n.substr(0, a);
                    if (a == n.size() || !std::isdigit((unsigned char)n[a])) return std::string();
                    static const char* ok[] = {"proc", "comb", "combv", "combc", "seq", "procS", "combS", "combvS", "combcS", "seqS", "ds", "dsT", "cyA", "cyB", "cyAT", "cyBT", "combP", "combQ", "combC", "combK", "combD", "combPT", "combQT", "combCT", "commitS", "cgP", "cgQ", "cmC", "cmL"};
                    for (const char* k : ok) if (pre == k) return pre + (n.size() > 2 && n.compare(n.size() - 2, 2, "_2") == 0 ? "2" : "");
                    return std::string();
                };
                std::vector<std::string> names(chunks.size());
                for (size_t i = 0; i < chunks.size(); i++) {
                    names[i] = fnName(chunks[i]);
                    std::string k = localKind(names[i]);
                    kinds[i] = k;
                    if (k.empty() || chunks[i].find("{ dsT") != std::string::npos) global[i] = 1;
                }
                // A process's or template's function and its part are named
                // after a key that neither a logic edit nor renumbering
                // changes: its kind and the first signal it writes (an NBA
                // slot), else the first it names, by symbol (a hash of the
                // signal's name), with an ordinal for the same key twice.
                // So a logic edit changes only that function's text: its
                // part is rebuilt and design.cpp (which calls it) is not.
                // Only the parts' code goes by symbols; design.cpp keeps the
                // numbers (it is rebuilt on any edit, and has no extern decls).
                for (size_t i = 0; i < chunks.size(); i++)
                    if (!global[i]) chunks[i] = localNames(relocText(chunks[i], csyms[i]));
                auto mapIds = mapIdents;
                std::unordered_map<std::string, int> taken;
                for (size_t i = 0; i < chunks.size(); i++) {
                    if (global[i] || names[i].empty()) continue;
                    std::string k = kinds[i];   // a process's 2-state copy goes with it
                    if (!k.empty() && k.back() == '2') k.pop_back();
                    std::string m;
                    bool sl = false;
                    for (auto& y : csyms[i]) if (y.compare(0, 3, "SL_") == 0) { sl = true; break; }
                    for (auto& y : csyms[i]) {
                        if (sl && y.compare(0, 3, "SL_") != 0) continue;
                        std::string b = y.size() > 3 ? y.substr(3) : y;
                        if (m.empty() || b < m) m = b;
                    }
                    std::string key = k + ":" + m;
                    key += "#" + std::to_string(taken[kinds[i] + ":" + m]++);
                    pkey[i] = key;
                    uint64_t h = 1469598103934665603ULL;
                    for (char c : key) h = (h ^ (unsigned char)c) * 1099511628211ULL;
                    char hx[24];
                    std::snprintf(hx, sizeof hx, "%012llx", (unsigned long long)(h & 0xffffffffffffULL));
                    rename[names[i]] = kinds[i] + "_" + hx;
                }
                auto ren = [&](const std::string& id) { auto it = rename.find(id); return it == rename.end() ? id : it->second; };
                for (auto& c : chunks) c = mapIds(c, ren);
                protos = mapIds(protos, ren);
                cycHdr = mapIds(cycHdr, ren);
                std::string pre = mapIds(o.str(), ren);
                o.str("");
                o << pre;
                for (size_t i = 0; i < chunks.size(); i++) if (global[i]) globalCode += chunks[i];
            }
            // The header: what does not depend on the design's numbering
            // (declSplit); the rest goes to the parts that name it, and all of
            // it to design.cpp.
            std::string stable = o.str() + "\n" + protos + cycHdr;
            if (reloc)
                stable += "#define MSIM_RELOC 1   // parts and simv without PIE (msim.cpp runCompiled)\n"
                          "#define SGS(o) (*(Sig*)((char*)S.sigs.data() + (o)))\n"
                          "#define SNI(x) ((int)(intptr_t)(x))\n";
            std::vector<HdrItem> hitems;
            if (!std::getenv("MSIM_FULLHDR")) { std::string all = std::move(stable); declSplit(all, stable, hitems); }
            std::unordered_map<std::string, int> howner;
            for (size_t i = 0; i < hitems.size(); i++) for (auto& x : hitems[i].names) howner[x] = (int)i;
            outFiles.push_back(stable);
            // Each chunk goes to the file its key picks: the first line that
            // is not a comment (the function's signature) with the numbers
            // left out, and how many chunks before it had that same line. A
            // logic edit changes only the text of the chunks of what was
            // edited, and they stay in their files, so only those files are
            // rebuilt (a part is rebuilt when its text changes, msim.cpp
            // runCompiled); renumbering signals or processes moves nothing.
            // Only a process added or removed moves the later ones of its
            // kind. (A hash of the whole text moved an edited chunk to
            // another file: two files rebuilt for it.) Filling the smallest
            // file first balanced the files better (a full build of 4 Ibex
            // cores took 6% less) but moved chunks on any edit; so did a load cap.
            std::vector<std::vector<size_t>> mine(nf);
            std::vector<size_t> load(nf, 0);
            std::unordered_map<std::string, int> seen;
            for (size_t i = 0; i < chunks.size(); i++) {
                if (global[i]) continue;
                const std::string& c = chunks[i];
                size_t at = 0;
                while (at < c.size() && c.compare(at, 2, "//") == 0) { size_t nl = c.find('\n', at); at = nl == std::string::npos ? c.size() : nl + 1; }
                size_t nl = c.find('\n', at);
                std::string sig;
                for (size_t k = at; k < (nl == std::string::npos ? c.size() : nl); k++) {
                    if (c[k] >= '0' && c[k] <= '9') {   // a number: 12, 0x1fULL
                        if (c[k] == '0' && k + 1 < c.size() && c[k + 1] == 'x') k++;
                        while (k + 1 < c.size() && std::isxdigit((unsigned char)c[k + 1])) k++;
                        continue;
                    }
                    sig += c[k];
                }
                if (!pkey[i].empty()) sig = pkey[i];
                else sig += "#" + std::to_string(seen[sig]++);
                uint64_t h = 1469598103934665603ULL;
                for (char ch : sig) h = (h ^ (unsigned char)ch) * 1099511628211ULL;
                h ^= h >> 29;
                int f = (int)(h % (uint64_t)nf);
                load[f] += c.size();
                mine[f].push_back(i);
            }
            if (std::getenv("MSIM_WHYPARTS"))
                for (int f = 0; f < nf; f++) std::fprintf(stderr, "part%d: %zu chunks, %zu bytes\n", f, mine[f].size(), load[f]);
            for (int f = 0; f < nf; f++) {
                std::sort(mine[f].begin(), mine[f].end());   // process order within a file
                std::string body;
                std::set<std::string> syms;
                for (size_t i : mine[f]) { body += chunks[i]; syms.insert(csyms[i].begin(), csyms[i].end()); }
                outFiles.push_back("#include \"design.hpp\"\n\n" + relocDecls(syms) + partDecls(body, hitems, howner) + "\n" + body);
            }
            o.str("");
            o << (kernelObj || mix ? "#include \"design.hpp\"\n\n" : "#include \"design.hpp\"\n\nSim S;\n\n");
            // Macros for array elements (P<s> = PA[s]) only where design.cpp
            // names them (dropped at the end): a million of them for 1024
            // PicoRV32 cores were 30 MB of the file.
            o << kMacroMark << "{\n";
            for (auto& h : hitems) if (h.def.empty() && h.decl.compare(0, 8, "#define ") == 0) o << h.decl;
            o << kMacroMark << "}\n";
            for (auto& h : hitems) if (!(h.def.empty() && h.decl.compare(0, 8, "#define ") == 0)) o << h.decl;
            o << "\n";
            for (auto& h : hitems) o << h.def;
            o << "\n" << relocAsm() << "\n" << globalCode;
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
        if (!compileDpiLibs().empty()) o << "#include \"svdpi.h\"\nextern \"C\" {\n#include \"svdpi_bits.inc\"\n}\n\n";
        // Large startup tables are byte strings of LEB128 numbers (blob()):
        // the C++ compiler holds a string literal as one object, an
        // initializer as one per element.
        o << "struct MsimLeb {\n"
             "    const unsigned char* p;\n"
             "    uint64_t u() { uint64_t r = 0; for (int sh = 0;; sh += 7) { unsigned b = *p++; r |= (uint64_t)(b & 127) << sh; if (!(b & 128)) return r; } }\n"
             "    long long s() { uint64_t v = u(); return (long long)(v >> 1) ^ -(long long)(v & 1); }\n"
             "    std::vector<int> list() { std::vector<int> v(u()); for (auto& x : v) x = (int)s(); return v; }\n"
             "};\n\n";
        // Mixed mode: no main. msim has made the signals and set them up,
        // parsed the options and runs the simulation; the library registers
        // its processes and functions with msim's kernel (msim.cpp runCompiled).
        if (mix)
            o << "extern \"C\" int msim_mix_setup() {\n"
              << [] { std::string l; for (auto& p : compileDpiLibs()) l += "    cdpiLibs().push_back(" + quote(p) + ");\n"; return l; }();
        else
        o << "int main(int argc, char** argv) {\n"
             "    uint64_t maxTime = ~0ULL;\n"
             "    const char* covOut = nullptr;\n"
          << (upf ? "    const char* upfSnap = \"upf.snap\";\n" : "") <<
             "    S.vcdPath = \"\";\n"
             "    timeFmt().tickExp = timeFmt().units = " << precExp << ";\n"
             "    S.timescale = " << quote(timescale) << ";\n"
             "    S.threads = defaultThreads();\n"
          << [] { std::string l; for (auto& p : compileDpiLibs()) l += "    cdpiLibs().push_back(" + quote(p) + ");\n"; return l; }() <<
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
        // msim's strings in the same order: a string id inside a constant (a
        // struct parameter's string member, 6.20.2) names the same text.
        if (!mix && strPool().text.size() > 1) {
            o << "    static const char* const STRS[] = {";
            for (size_t k = 1; k < strPool().text.size(); k++) o << (k > 1 ? ", " : "") << quote(strPool().text[k]);
            o << "};\n    static const size_t STRN[] = {";
            for (size_t k = 1; k < strPool().text.size(); k++) o << (k > 1 ? ", " : "") << strPool().text[k].size();
            o << "};\n    for (size_t k = 0; k < sizeof STRN / sizeof STRN[0]; k++) strPool().of(std::string(STRS[k], STRN[k]));\n";
        }
        if (nSlots) o << "    XSL = SLT;\n";
        if (!arrs_.empty()) o << "    XAS = AST;\n";
        if (!sigs.empty() && !mix) {
            // Runs of array elements (m[0], m[1], ...) are one row: 1024
            // PicoRV32 cores have a quarter million memory words.
            auto elem = [&](size_t s, std::string& base, long long& ix) {
                const std::string& n = sigs[s].name;
                if (n.empty() || n.back() != ']') return false;
                size_t lb = n.rfind('[');
                if (lb == std::string::npos || lb + 2 > n.size() - 1) return false;
                for (size_t k = lb + 1; k + 1 < n.size(); k++) if (!std::isdigit((unsigned char)n[k])) return false;
                if (n.size() - lb - 2 > 9) return false;
                base = n.substr(0, lb);
                ix = std::stoll(n.substr(lb + 1, n.size() - lb - 2));
                return true;
            };
            // One byte string, not a table of rows: a million signals
            // (XiangShan) made a 120 MB initializer that the C++ compiler
            // could not hold in memory. Each row is the name as the length
            // it shares with the previous one plus the rest (front coding),
            // then width, net, drivers and, for a run of array elements,
            // count, first index and step, as LEB128 numbers.
            std::string blob, prev;
            auto vu = [&](uint64_t v) { leb(blob, v); };
            auto vs = [&](long long v) { lebS(blob, v); };
            auto row = [&](const std::string& n, size_t s, size_t cnt, long long lo, int step) {
                size_t k = 0;
                while (k < n.size() && k < prev.size() && n[k] == prev[k]) k++;
                vu(k);
                vu(n.size() - k);
                blob.append(n, k, std::string::npos);
                prev = n;
                vu((uint64_t)sigs[s].w);
                vu(sigs[s].net ? 1 : 0);
                vu((uint64_t)sigs[s].drivers);
                vu(cnt);
                if (cnt) { vs(lo); vs(step); }
            };
            for (size_t s = 0; s < sigs.size();) {
                std::string b0, b1;
                long long i0 = 0, i1 = 0;
                size_t e = s + 1;
                int step = 0;
                if (elem(s, b0, i0) && s + 1 < sigs.size() && elem(s + 1, b1, i1) && b1 == b0 && (i1 == i0 + 1 || i1 == i0 - 1)) {
                    step = (int)(i1 - i0);
                    while (e < sigs.size() && sigs[e].w == sigs[s].w && sigs[e].net == sigs[s].net && sigs[e].drivers == sigs[s].drivers &&
                           elem(e, b1, i1) && b1 == b0 && i1 == i0 + (long long)(e - s) * step)
                        e++;
                }
                if (e - s >= 4) row(b0, s, e - s, i0, step);
                else {
                    e = s + 1;
                    row(sigs[s].name, s, 0, 0, 0);
                }
                s = e;
            }
            o << "    static const char SIGB[] =\n" << blobText(blob) << ";\n"
                 "    {\n"
                 "        MsimLeb r{(const unsigned char*)SIGB};\n"
                 "        const unsigned char* end = r.p + sizeof SIGB - 1;\n"
                 "        auto vu = [&] { return r.u(); };\n"
                 "        auto vs = [&] { return r.s(); };\n"
                 "        std::string n;\n"
                 "        while (r.p < end) {\n"
                 "            size_t keep = vu(), add = vu();\n"
                 "            n.resize(keep);\n"
                 "            n.append((const char*)r.p, add);\n"
                 "            r.p += add;\n"
                 "            int w = (int)vu();\n"
                 "            bool net = vu() != 0;\n"
                 "            int drv = (int)vu(), cnt = (int)vu();\n"
                 "            long long lo = 0, step = 0;\n"
                 "            if (cnt) { lo = vs(); step = vs(); }\n"
                 "            for (int k = 0; k < std::max(cnt, 1); k++) {\n"
                 "                int s = cnt ? S.sig(n + \"[\" + std::to_string(lo + k * step) + \"]\", w, net) : S.sig(n, w, net);\n"
                 "                for (int d = 0; d < drv; d++) S.newDriver(s);\n"
                 "            }\n"
                 "        }\n"
                 "    }\n";
            // Initial values; runs of signals that start at 0 are one row
            // {first, count}.
            // Signal numbers as the distance from the previous row.
            std::string iv, iz;
            size_t ivAt = 0, izAt = 0;
            for (size_t s = 0; s < sigs.size();) {
                auto zero = [&](size_t k) { return sigs[k].init && !sigs[k].v && !sigs[k].x && !sigs[k].z; };
                if (zero(s)) {
                    size_t e = s + 1;
                    while (e < sigs.size() && zero(e)) e++;
                    leb(iz, s - izAt);
                    leb(iz, e - s);
                    izAt = e;
                    s = e;
                    continue;
                }
                if (sigs[s].init) {
                    leb(iv, s - ivAt);
                    leb(iv, sigs[s].v);
                    leb(iv, sigs[s].x);
                    leb(iv, sigs[s].z);
                    ivAt = s;
                }
                s++;
            }
            if (!iz.empty())
                o << "    {\n        static const char B[] =\n" << blobText(iz) << ";\n"
                     "        MsimLeb r{(const unsigned char*)B};\n"
                     "        for (size_t s = 0; r.p < (const unsigned char*)B + sizeof B - 1;) {\n"
                     "            s += r.u();\n"
                     "            for (size_t e = s + r.u(); s < e; s++) { Val& g = S.sigs[s].val; g.v = g.x = g.z = 0; }\n"
                     "        }\n    }\n";
            std::string zw;
            for (size_t s = 0, at = 0; s < sigs.size(); s++)
                if (sigs[s].zeroWide) { leb(zw, s - at); at = s; }
            if (!zw.empty())
                o << "    {\n        static const char B[] =\n" << blobText(zw) << ";\n"
                     "        MsimLeb r{(const unsigned char*)B};\n"
                     "        for (size_t s = 0; r.p < (const unsigned char*)B + sizeof B - 1;) {\n"
                     "            s += r.u();\n"
                     "            Val& g = S.sigs[s].val; g.v = g.x = g.z = 0; for (auto& h : g.hi) h.v = h.x = h.z = 0;\n"
                     "        }\n    }\n";
            if (!iv.empty())
                o << "    {\n        static const char B[] =\n" << blobText(iv) << ";\n"
                     "        MsimLeb r{(const unsigned char*)B};\n"
                     "        for (size_t s = 0; r.p < (const unsigned char*)B + sizeof B - 1;) {\n"
                     "            s += r.u();\n"
                     "            Val& g = S.sigs[s].val; g.v = r.u(); g.x = r.u(); g.z = r.u();\n"
                     "        }\n    }\n";
            for (size_t s = 0; s < sigs.size(); s++)
                if (sigs[s].strInit) o << "    S.sigs[" << s << "].val = mkstr(" << quote(sigs[s].strText) << ");\n";
            {
                std::string mr;
                for (size_t s = 0, at = 0; s < sigs.size(); s++)
                    if (sigs[s].mirror >= 0) { leb(mr, s - at); lebS(mr, (long long)sigs[s].mirror - (long long)s); at = s; }
                if (!mr.empty())
                    o << "    {\n        static const char B[] =\n" << blobText(mr) << ";\n"
                         "        MsimLeb r{(const unsigned char*)B};\n"
                         "        for (long long s = 0; r.p < (const unsigned char*)B + sizeof B - 1;) {\n"
                         "            s += (long long)r.u();\n"
                         "            S.addMirror((int)s, (int)(s + r.s()));\n"
                         "        }\n    }\n";
            }
            // A delayed driver is x until its first update arrives (10.3.3,
            // 28.16), with its own strengths, as msim sets it up.
            std::set<int> dlyNets;
            for (const auto& pr : procs)
                for (const Instr& i : pr.prog->code)
                    if ((!i.dly.empty() || !i.dlyE.empty()) && i.drv >= 0 && i.k == IK::Assign) {
                        o << "    { auto& dr = S.sigs[" << i.sig << "].drv[" << i.drv << "]; dr.v = ";
                        if (i.hi >= 0) o << "v_insert(allz(S.sigs[" << i.sig << "].w), allx(" << i.hi - i.lo + 1 << "), " << i.hi << ", " << i.lo << ")";
                        else o << "allx(S.sigs[" << i.sig << "].w)";
                        o << "; dr.s0 = " << (int)i.s0 << "; dr.s1 = " << (int)i.s1 << "; }\n";
                        dlyNets.insert(i.sig);
                    }
            for (int s : dlyNets)
                o << "    { auto& g = S.sigs[" << s << "]; g.val = resolveDrives(g.drv.data(), g.drv.size(), g.w); }\n";
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
        // With no comb units (every copy dropped, msim.cpp substituteWires) the
        // pass still runs after a testbench write to a private signal: syncP.
        if (!dom.empty()) {
            std::vector<int> ins;
            for (int i : byRank) ins.insert(ins.end(), procs[i].combIns.begin(), procs[i].combIns.end());
            std::sort(ins.begin(), ins.end());
            ins.erase(std::unique(ins.begin(), ins.end()), ins.end());
            if (!coneIns_.empty()) {   // the clock cone runs first, on the clocks alone
                std::vector<int> rest;
                for (int x : coneIns_) if (!coneK_ || !std::binary_search(coneClkIns_.begin(), coneClkIns_.end(), x)) rest.push_back(x);
                if (coneK_) {
                    o << "    S.addComb(combConeK, nullptr, {";
                    for (size_t j = 0; j < coneClkIns_.size(); j++) o << (j ? ", " : "") << coneClkIns_[j];
                    o << "});\n";
                }
                if (!rest.empty()) {
                    o << "    S.addComb(combCone, nullptr, {";
                    for (size_t j = 0; j < rest.size(); j++) o << (j ? ", " : "") << rest[j];
                    o << "});\n";
                }
                std::erase_if(ins, [&](int x) {
                    return std::binary_search(coneIns_.begin(), coneIns_.end(), x) || std::binary_search(coneOuts_.begin(), coneOuts_.end(), x);
                });
            }
            o << "    S.fastComb = (int)S.combs.size();\n    S.addComb(combAll, nullptr, {";
            for (size_t j = 0; j < ins.size(); j++) o << (j ? ", " : "") << ins[j];
            o << "});\n";
        }
        if (!evDT_.empty()) {   // shareEv: each process's distances
            std::vector<int> v(evDT_.begin(), evDT_.end());
            o << intTable("DT", v);
        }
        if (dom.empty() && !byRank.empty()) {   // the ranks: functions, wait lists in CINS
            std::string tab;
            std::vector<int> ins;
            size_t off = 0;
            for (int i : byRank) {
                ins.insert(ins.end(), procs[i].combIns.begin(), procs[i].combIns.end());
                bool one = oneAssign(*procs[i].prog);
                const std::string nm = evName_.size() > (size_t)i && !evName_[i].empty() ? evName_[i] : std::to_string(i);
                tab += "        {comb" + nm + ", " + (one ? "combv" + nm : "nullptr") + ", " +
                       (one ? "combc" + nm : "nullptr") + ", " + std::to_string(procs[i].combLevel) + ", " +
                       std::to_string(off) + ", " + std::to_string(procs[i].combIns.size()) + ", " + evCtx(i) + "},\n";
                off += procs[i].combIns.size();
            }
            std::string cyc;   // ranks on a cycle: only their final writes wake readers
            for (size_t k = 0; k < byRank.size(); k++) if (procs[byRank[k]].combOnCycle) cyc += std::to_string(k) + ",";
            ins.push_back(0);
            o << intTable("CINS", ins) <<
                 "    static const struct { void (*f)(const void*); Val (*v)(const void*); void (*c)(const void*, Val); int lv, at, n; const void* x; } COMBS[] = {\n"
              << tab << "    };\n"
                 "    const size_t combBase = S.combs.size();\n"
                 "    for (auto& c : COMBS) S.addComb(c.f, c.x, std::vector<int>(CINS + c.at, CINS + c.at + c.n), c.lv, c.v, c.c);\n";
            if (!cyc.empty())
                o << "    static const int CYC[] = {" << cyc << "};\n"
                     "    for (int k : CYC) S.combs[combBase + (size_t)k].onCycle = true;\n";
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
                if (procs[i].seq && !(i < mixSeq_.size() && mixSeq_[i])) {
                    for (auto& [sg, ed] : procs[i].seqEv) evs += "{" + std::to_string(sg) + ", " + edge(ed) + "},";
                    const std::string nm = evName_.size() > i && !evName_[i].empty() ? evName_[i] : std::to_string(i);
                    tab += "        {seq" + nm + ", nullptr, " + std::to_string(off) + ", " +
                           std::to_string(procs[i].seqEv.size()) + ", 0, " + evCtx((int)i) + ", nullptr},\n";
                    off += procs[i].seqEv.size();
                } else {
                    // Time 0 order as the interpreter's (msim.cpp): declaration
                    // order, except that constant continuous assigns before the
                    // first initial block start just before it (or last if there
                    // is none); each Task is still made in declaration order.
                    const int late = procs[i].kind == "assign" && !procs[i].loops ? 1 : procs[i].kind == "Initial" ? 2 : 0;
                    if (i < evPlain_.size() && evPlain_[i]) {
                        const std::string nm = !evName_[i].empty() ? evName_[i] : std::to_string(i);
                        tab += "        {nullptr, nullptr, 0, 0, " + std::to_string(late) + ", " + evCtx((int)i) + ", proc" + nm + "},\n";
                    } else {
                        int ck = -1;
                        uint64_t half = 0;
                        const bool fc = toggleClock(*procs[i].prog, ck, half);
                        tab += "        {nullptr, proc" + std::to_string(i) + ", 0, 0, " + std::to_string(late) + ", nullptr, nullptr" +
                               (fc ? ", " + std::to_string(ck + 1) + ", " + std::to_string(half) + "ULL" : std::string()) + "},\n";
                    }
                }
            }
            if (n) {
                o << "    static const std::pair<int, Edge> SEV[] = {" << evs << "{0, ANY}};\n"
                     "    static const struct { void (*s)(const void*, std::vector<Sim::NbaItem>&); Task (*p)(); int at, n, late; const void* x; void (*f)(const void*); int ck; unsigned long long half; } PROCS[] = {\n"
                  << tab << "    };\n"
                     "    std::vector<Task> held;\n    bool init1 = false;\n"
                     "    auto mk = [](const auto& p) { return p.p ? p.p() : evOnce(p.f, p.x); };\n"
                     "    for (auto& p : PROCS)\n"
                     "        if (p.s) S.addSeq(p.s, p.x, std::vector<std::pair<int, Edge>>(SEV + p.at, SEV + p.at + p.n));\n"
                     "        else if (p.late == 1 && !init1) held.push_back(mk(p));\n"
                     "        else if (p.late == 2 && !init1) {\n"
                     "            init1 = true;\n            Task t = mk(p);\n"
                     "            for (auto& h : held) S.spawn(h);\n            held.clear();\n            S.spawn(t);\n"
                     "        } else { Task t = mk(p); S.spawn(t); if (p.ck) S.addFastClock(t.h, p.ck - 1, p.half); }\n"
                     "    for (auto& h : held) S.spawn(h);\n";
            }
        }
        if (!dom.empty()) o << "    S.fastInit();\n";
        if (parts.on && dom.empty()) {
            std::string b;
            auto list = [&](const std::vector<int>& v) { leb(b, v.size()); for (int x : v) lebS(b, x); };
            leb(b, parts.outs.size());
            for (auto& v : parts.outs) list(v);
            leb(b, parts.ins.size());
            for (auto& v : parts.ins) list(v);
            list(parts.targets);
            o << "    {\n"
                 "        static const char PB[] =\n" << blobText(b) << ";\n"
                 "        MsimLeb r{(const unsigned char*)PB};\n"
                 "        std::vector<std::vector<int>> outs(r.u());\n"
                 "        for (auto& v : outs) v = r.list();\n"
                 "        std::vector<std::vector<int>> ins(r.u());\n"
                 "        for (auto& v : ins) v = r.list();\n"
                 "        S.buildPartitions(outs, ins, r.list());\n"
                 "    }\n";
        }
        if (cov) {
            o << "    g_condHits.assign(" << g_condHits.size() << ", 0);\n    S.fsmWatch({";
            for (size_t k = 0; k < fsmDefs().size(); k++) o << (k ? ", " : "") << fsmDefs()[k].sig;
            o << "}, {";
            for (size_t k = 0; k < fsmDefs().size(); k++) o << (k ? ", " : "") << (fsmDefs()[k].onehot ? 1 : 0);
            o << "});\n";
        }
        if (upf) o << "    upfrt::rt().start();\n";
        if (mix) o << "    return 0;\n}\n";
        else {
        o << "    S.run(maxTime);\n"
             "    std::cout.flush();\n";
        if (upf)
            o << "    if (!upfrt::rt().covPath.empty() && !upfrt::rt().writeCoverage()) {\n"
                 "        std::cerr << \"simv: cannot write \" << upfrt::rt().covPath << \"\\n\";\n"
                 "        return 1;\n"
                 "    }\n";
        if (cov) o << "    if (covOut && !writeCov(covOut)) { std::cerr << \"simv: cannot write \" << covOut << \"\\n\"; return 1; }\n";
        o << "    return S.exitCode;\n"
             "}\n";
        }
        if (outFiles.empty()) return o.str();
        // Files after one another, each after a marker line msim splits on.
        std::string dcpp = rename.empty() ? o.str() : mapIdents(o.str(), [&](const std::string& id) { auto it = rename.find(id); return it == rename.end() ? id : it->second; });
        o.str(std::string());
        if (size_t a = dcpp.find(std::string(kMacroMark) + "{\n"), b = dcpp.find(std::string(kMacroMark) + "}\n"); a != std::string::npos && b != std::string::npos && a < b) {
            size_t a2 = a + std::strlen(kMacroMark) + 2, b2 = b + std::strlen(kMacroMark) + 2;
            std::unordered_set<std::string> used;
            identsOf(dcpp.substr(0, a), [&](const std::string& x) { used.insert(x); });
            identsOf(dcpp.substr(b2), [&](const std::string& x) { used.insert(x); });
            std::string keep;
            for (size_t i = a2; i < b;) {
                size_t nl = dcpp.find('\n', i);
                if (nl == std::string::npos || nl > b) nl = b;
                size_t ne = dcpp.find(' ', i + 8);
                if (ne != std::string::npos && ne < nl && used.count(dcpp.substr(i + 8, ne - i - 8))) keep.append(dcpp, i, nl + 1 - i);
                i = nl + 1;
            }
            dcpp = dcpp.substr(0, a) + keep + dcpp.substr(b2);
        }
        // Each piece is freed as it is appended (a large design's C++ is
        // gigabytes).
        size_t total = dcpp.size() + 64 * (outFiles.size() + 1);
        for (auto& f : outFiles) total += f.size();
        std::string all;
        all.reserve(total);
        all += std::string(kFileMark) + "design.hpp\n";
        all += outFiles[0];
        std::string().swap(outFiles[0]);
        all += std::string(kFileMark) + "design.cpp\n";
        all += dcpp;
        std::string().swap(dcpp);
        for (size_t f = 1; f < outFiles.size(); f++) {
            all += std::string(kFileMark) + "part" + std::to_string(f - 1) + ".cpp\n";
            all += outFiles[f];
            std::string().swap(outFiles[f]);
        }
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
    // The kernel comes from an object built once (msim.cpp kernelObject):
    // every generated file only declares its large functions (MINISIM_CLIENT)
    // and the kernel object defines S.
    bool kernelObj = false;
    // Mixed mode (msim -compile of a design with processes codegen cannot
    // compile yet): such a process becomes a call to g_mixInterp, which msim
    // runs in its interpreter, listed in `interp` with the reason. The
    // output is a library msim loads (msim_mix_setup), not a program.
    bool mix = false;
    std::vector<std::pair<int, std::string>> interp;
    std::vector<const FrameLayout*> lays_;   // frame layouts emitProc's calls make (LAY<k>)
    bool needMsim_ = false;   // generated code needs msim's runtime (classes): mixed mode only
    std::vector<const Instr*> msimIns_;   // MSIMI(k): msim's instructions the library runs (g_mixIns)
    std::string msimIns(const Instr& i) {
        needMsim_ = true;
        if (!mix) return "MSIMI(0)";   // not built: codegen() goes on to mixed mode
        msimIns_.push_back(&i);
        return "MSIMI(" + std::to_string(msimIns_.size() - 1) + ")";
    }
    std::vector<char> mixSeq_;   // a clocked process left to the interpreter, or run as a coroutine
    // A clocked process as a clocked function, or else (a $display in it, say)
    // as an ordinary coroutine process, as with msim -no-static-seq.
    // Returns whether it is a clocked function.
    bool seqOrProc(std::ostream& o, const std::vector<CodegenProc>& procs, size_t i) {
        std::ostringstream c;
        try {
            emitSeq(c, (int)i, *procs[i].prog, procs[i].kind);
            o << c.str();
            return true;
        } catch (const CodegenUnsupported&) {
            if (std::getenv("MSIM_NOSEQPROC")) throw;
        }
        emitProc(o, (int)i, *procs[i].prog, procs[i].kind);
        mixSeq_.resize(procs.size());
        mixSeq_[i] = 1;
        return false;
    }
    // Event-driven comb and clocked functions shared between instances
    // (shareEv): the template whose function a process calls ("" its own)
    // and where its distances start in the table DT (-1: none).
    std::vector<std::string> evName_;
    std::vector<char> evPlain_;   // a process that never waits: a plain function, run by evOnce
    std::vector<int> evAt_;
    std::vector<int> evDT_;
    size_t evSaved_ = 0, evTotal_ = 0;
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
    // jmpIfNot with the condition split (split()): when none of its bits
    // is unknown, a plain 64-bit test instead of a Val and truth().
    // MSIM_NOSPLITJ=1: the Val form everywhere.
    std::string jmpIfNotSplit(const Instr& i, const std::string& no, const std::string& unk) {
        static const bool off = std::getenv("MSIM_NOSPLITJ") != nullptr;
        std::string g, u, sl;
        if (off || !split(i.e, g, u, sl, true)) return jmpIfNot(i, fast(i.e), no, unk);
        if (g.empty()) return " if (!(" + u + ")) " + no + "\n";
        // The unknown case out of line: a large clocked function (a CPU's
        // reorder buffer, thousands of conditions) would otherwise carry a
        // 4-state copy of every condition, and gcc's memory grows with it.
        const std::string cold = "[&]() __attribute__((noinline, cold)) -> Val { return " + sl + "; }()";
        return " if (__builtin_expect(" + g + ", 1)) { if (!(" + u + ")) " + no + " } else {" + jmpIfNot(i, cold, no, unk) + " }\n";
    }
    static std::string jmpIfNot(const Instr& i, const std::string& val, const std::string& no, const std::string& unk) {
        if (i.xt == Instr::NOX) return " if (truth(" + val + ") != 1) " + no + "\n";
        return " switch (truth(" + val + ")) { case 1: break; case 2: " + unk + " default: " + no + " }\n";
    }

private:
    std::vector<int> sigW_;
    std::vector<std::string> sigName_;
    const char* dbgName(int s) const { return s >= 0 && (size_t)s < sigName_.size() ? sigName_[(size_t)s].c_str() : "?"; }
    // Temporaries of the lowering and of msim's call expansion (_tf...): each
    // read follows a write in the same pass, so an X left in one from a pass
    // that did not use it says nothing about 2-state mode.
    std::vector<char> scratch_;
    std::vector<char> quiet_;   // CodegenSig::quiet
    const bool noSliceOf_ = std::getenv("MSIM_NOSLICEOF") != nullptr;   // v_slice(R(..)) as before
    bool quiet(int s) const { return s >= 0 && (size_t)s < quiet_.size() && quiet_[(size_t)s]; }
    bool scratch(int s) const { return s >= 0 && (size_t)s < scratch_.size() && scratch_[(size_t)s]; }
    // Private signals of the cycle domain (see privateSigs): kept in plain
    // static variables P<s> instead of S.sigs, like Verilator's model members.
    std::vector<char> priv_, sigNet_, clock_, soleNba_;
    // `always @*` arming processes (msim.cpp rotateAtStar: wait once on the
    // block's inputs, then set its $atstar flag) that wait on private
    // signals: the waits on those never wake (a private signal changes in
    // PA, not S.sigs), so the domain compares them with their values at the
    // start of the wait instead (armChk). Per process: its arm number, or -1.
    std::vector<int> armK_;
    std::vector<int> run_;   // the domain's processes in the order they run (orderDomain)
    std::vector<std::vector<int>> armIns_;   // per arm: its private inputs
    std::vector<const Instr*> armSet_;       // per arm: the flag's Assign
    static bool armShape(const CodegenProc& p, const std::vector<CodegenSig>& sigs) {
        if (p.seq || p.combRank >= 0 || !p.prog) return false;
        const auto& c = p.prog->code;
        return c.size() == 2 && c[0].k == IK::WaitEdge && !c[0].dynWait && !c[0].events.empty() && c[1].k == IK::Assign &&
               c[1].sig >= 0 && (size_t)c[1].sig < sigs.size() && c[1].hi < 0 && !c[1].arr && !c[1].bidx && c[1].drv < 0 &&
               sigs[c[1].sig].name.rfind("$atstar", 0) == 0;
    }
    // A narrow variable no clocked process waits on: Sim::fastInit marks it
    // quiet, so a write is a store unless a process waits on it (or a VCD
    // or $monitor watches everything). `obs` is S.observed, read once per function.
    std::string regWrite(int s, const std::string& u) const {
        std::string S_ = std::to_string(s), m = M(sigW_[s]);
        if (sigW_[s] > 64 || sigNet_[s] || clock_[s]) return "S.fwv(" + S_ + ", " + u + ");";
        // Most writes of a comb pass store what is already there: compare
        // first (a known value, as fwSlow would find it), then the store.
        if (!noRegSame_)
            return "{ const uint64_t nv_ = (" + u + ") & " + m + "; Val& q = S.sigs[" + S_ + "].val; if (__builtin_expect(q.v != nv_ || (q.x | q.z) || q.real || q.str, 0)) { if (__builtin_expect(!obs && S.waiters[" +
                   S_ + "].empty(), 1)) { q.v = nv_; q.x = 0; q.z = 0; q.real = false; q.str = false; } else S.fwSlow(" + S_ + ", make(nv_, 0, " +
                   std::to_string(sigW_[s]) + ")); } }";
        return "{ if (__builtin_expect(!obs && S.waiters[" + S_ + "].empty(), 1)) { Val& q = S.sigs[" + S_ + "].val; q.v = (" +
               u + ") & " + m + "; q.x = 0; q.z = 0; q.real = false; q.str = false; } else S.fwSlow(" + S_ + ", make((" + u +
               ") & " + m + ", 0, " + std::to_string(sigW_[s]) + ")); }";
    }
    const bool noSetBit_ = std::getenv("MSIM_NOSETBIT") != nullptr;
    const bool noRegSame_ = std::getenv("MSIM_NOREGSAME") != nullptr;
    // A run of constant part writes of one private register (always_comb
    // loops: x[i] = y[N-1-i], x[i] = 1'b0, ...) as one word in the 2-state
    // copy: constant bits, bits moved by one distance, a bit reverse, the
    // rest one by one. ins[at..] are the candidates in order (the caller
    // stops them at a jump target); n: how many it took.
    const bool noLazyMz_ = std::getenv("MSIM_NOLAZYMEMO") != nullptr;   // the 2-state copy of an asserting comb process compares its inputs every pass
    std::string mzLazy_, mzVar_;   // the lazy compare of the process being emitted, and its flag
    const bool noBitRun_ = std::getenv("MSIM_NOBITRUN") != nullptr;
    bool bitRun2(const std::vector<const Instr*>& ins, size_t at, size_t& n, std::string& out) {
        n = 0;
        if (noBitRun_ || at >= ins.size()) return false;
        const Instr& a = *ins[at];
        const int sg = a.sig;
        auto ok = [&](const Instr& in) {
            return in.k == IK::Assign && in.sig == sg && in.hi >= 0 && in.lo >= 0 && in.hi < 64 && in.hi >= in.lo && !in.bidx && !in.arr &&
                   in.aidx.empty() && in.drv < 0 && isPriv(in.sig) && sigW_[in.sig] <= 64 && in.hi < sigW_[in.sig];
        };
        if (!ok(a)) return false;
        const std::string P_ = "P" + std::to_string(sg);
        auto readsSelf = [&](const std::string& u) {
            for (size_t f = u.find(P_); f != std::string::npos; f = u.find(P_, f + 1)) {
                const size_t e = f + P_.size();
                if ((f == 0 || !(std::isalnum((unsigned char)u[f - 1]) || u[f - 1] == '_')) && (e >= u.size() || !std::isdigit((unsigned char)u[e]))) return true;
            }
            return false;
        };
        struct It { int hi, lo; std::string u; };
        std::vector<It> its;
        uint64_t tm = 0;
        for (size_t q = at; q < ins.size(); q++) {
            const Instr& in = *ins[q];
            if (!ok(in)) break;
            std::string g, u, sl;
            if (!split(in.e, g, u, sl) || readsSelf(u)) break;
            const int w = in.hi - in.lo + 1;
            const uint64_t m = (w >= 64 ? ~0ULL : ((1ULL << w) - 1)) << in.lo;
            if (tm & m) break;   // a bit written twice: the order matters
            tm |= m;
            its.push_back({in.hi, in.lo, u});
        }
        if (its.size() < 3) return false;
        n = its.size();
        // Classify: constants, single bits of a source ((src >> k) & 0x1ULL), the rest.
        uint64_t cm = 0, cv = 0;
        std::map<std::string, std::vector<std::pair<int, int>>> bits;   // source -> (k, j)
        std::vector<std::string> terms;
        std::map<std::string, uint64_t> dec;   // "SRC\x01EN" -> its bits
        auto isSrc = [](const std::string& t) {
            if (t.size() < 3 || t.compare(t.size() - 2, 2, ".v") != 0) return false;
            for (size_t c = 0; c + 2 < t.size(); c++) if (!(std::isalnum((unsigned char)t[c]) || t[c] == '_')) return false;
            return true;
        };
        for (auto& it : its) {
            const int w = it.hi - it.lo + 1;
            const uint64_t m = (w >= 64 ? ~0ULL : ((1ULL << w) - 1)) << it.lo;
            const std::string& u = it.u;
            if (u.size() > 5 && u.rfind("0x", 0) == 0 && u.compare(u.size() - 3, 3, "ULL") == 0 && u.find_first_not_of("0123456789abcdefx", 0) == u.size() - 3) {
                cm |= m; cv |= (std::stoull(u.substr(2, u.size() - 5), nullptr, 16) << it.lo) & m;
                continue;
            }
            if (w == 1) {
                static const std::string tail = ") & 0x1ULL)";
                if (u.rfind("((", 0) == 0 && u.size() > tail.size() && u.compare(u.size() - tail.size(), tail.size(), tail) == 0) {
                    const std::string mid = u.substr(2, u.size() - 2 - tail.size());
                    const size_t sh = mid.rfind(" >> ");
                    if (sh != std::string::npos && isSrc(mid.substr(0, sh)) && mid.find_first_not_of("0123456789", sh + 4) == std::string::npos) {
                        bits[mid.substr(0, sh)].push_back({std::stoi(mid.substr(sh + 4)), it.lo});
                        continue;
                    }
                }
                if (isSrc(u)) { bits[u].push_back({0, it.lo}); continue; }
                // A decoder bit: ((uint64_t)(SRC == 0xJULL) != 0 ? EN : 0x0ULL) at bit J.
                static const std::string dp = "((uint64_t)(", dm = " == 0x", dq = "ULL) != 0 ? ", dt = " : 0x0ULL)";
                if (u.rfind(dp, 0) == 0 && u.size() > dt.size() && u.compare(u.size() - dt.size(), dt.size(), dt) == 0) {
                    const size_t a1 = u.find(dm), a2 = a1 == std::string::npos ? a1 : u.find(dq, a1);
                    if (a2 != std::string::npos) {
                        const std::string src = u.substr(dp.size(), a1 - dp.size()), num = u.substr(a1 + dm.size(), a2 - a1 - dm.size()),
                                          en = u.substr(a2 + dq.size(), u.size() - dt.size() - a2 - dq.size());
                        if (isSrc(src) && isSrc(en) && !num.empty() && num.size() <= 2 && num.find_first_not_of("0123456789abcdef") == std::string::npos &&
                            std::stoi(num, nullptr, 16) == it.lo) {
                            dec[src + "\x01" + en] |= 1ULL << it.lo;
                            continue;
                        }
                    }
                }
            }
            terms.push_back("(((uint64_t)(" + u + ") << " + std::to_string(it.lo) + ") & " + hex(m) + ")");
        }
        for (auto& [src, kj] : bits) {
            uint64_t gm = 0;
            for (auto& [k, j] : kj) gm |= 1ULL << j;
            bool sameD = true, sameC = true;
            for (auto& [k, j] : kj) { sameD &= j - k == kj[0].second - kj[0].first; sameC &= j + k == kj[0].second + kj[0].first; }
            if (kj.size() > 1 && sameD) {
                const int d = kj[0].second - kj[0].first;
                terms.push_back("((" + src + (d >= 0 ? " << " + std::to_string(d) : " >> " + std::to_string(-d)) + ") & " + hex(gm) + ")");
            } else if (kj.size() > 2 && sameC) {
                const int c = kj[0].second + kj[0].first;   // bit j = source bit c - j: rev64 puts source bit i at 63 - i
                terms.push_back("((msimRev64(" + src + ")" + (c <= 63 ? " >> " + std::to_string(63 - c) : " << " + std::to_string(c - 63)) + ") & " + hex(gm) + ")");
            } else
                for (auto& [k, j] : kj) terms.push_back("(((" + src + " >> " + std::to_string(k) + ") & 1ULL) << " + std::to_string(j) + ")");
        }
        for (auto& [k, gm] : dec) {   // bit J of the group: SRC == J and EN
            const size_t c = k.find('\x01');
            const std::string src = k.substr(0, c), en = k.substr(c + 1);
            terms.push_back("(" + src + " < 64 ? ((" + en + " & 1ULL) << " + src + ") & " + hex(gm) + " : 0ULL)");
        }
        const uint64_t full = M(sigW_[sg]) == "~0ULL" ? ~0ULL : (1ULL << sigW_[sg]) - 1;
        out = "    { uint64_t w_ = " + ((tm & full) == full ? std::string("0") : "(" + P_ + ".v & " + hex(~tm & full) + ")") + " | " + hex(cv) + ";";
        for (auto& t : terms) out += " w_ |= " + t + ";";
        out += " " + P_ + ".v = w_; }\n";
        return true;
    }
   // regWrite stores without comparing first   // a one-bit write goes through set()
    std::map<int, uint64_t> constInit_;   // privateSigs: private signals driven by a constant only, and the constant
    bool isPriv(int s) const { return s >= 0 && s < (int)priv_.size() && priv_[s]; }
    // Found while emitting: a private signal used where only S.sigs works (an
    // interpreted operator, an assertion). run() ends with CodegenRetry and
    // the next pass keeps these in S.sigs (noPriv).
public:
    std::set<int> noPriv, privHits_;
private:
    // An array whose elements are private (privateSigs: read in place):
    // anything that reads it through S.sigs trips them all into S.sigs.
    bool arrPriv(const ArrShape* a) const {
        if (!a) return false;
        for (int64_t k = 0; k < a->count; k++) if (isPriv((int)(a->base + k))) return true;
        return false;
    }
    void hitArr(const ArrShape* a) {
        if (a) for (int64_t k = 0; k < a->count; k++) if (isPriv((int)(a->base + k))) privHits_.insert((int)(a->base + k));
    }
    void hitPriv(const E& e) {
        if (!e) return;
        if (e->op == Op::Ref && isPriv(e->sig)) privHits_.insert(e->sig);
        if (e->op == Op::ArrRead) hitArr(e->arr.get());
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
    // Every element of a is private, in signal order (PA is indexed by signal).
    bool privArr(const ArrShape* a) const {
        if (!a || a->count < 1) return false;
        for (int64_t k = 0; k < a->count; k++) if (!isPriv((int)(a->base + k))) return false;
        return true;
    }
    bool readsPriv(const E& e) const {
        if (!e) return false;
        if (e->op == Op::Ref && isPriv(e->sig)) return true;
        if (e->op == Op::ArrRead && arrPriv(e->arr.get())) return true;
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
        // A frame slot (emitProc frames): its signal now, from the running frame.
        const std::string sg = isRel(i.sig) && !i.arr ? "R(" + std::to_string(i.sig) + ")" : std::to_string(i.sig);
        o << " { Val v = " << val << "; int s = " << sg << ", hi = " << i.hi << ", lo = " << i.lo << ";";
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

    static void leb(std::string& b, uint64_t v) {
        do { b += (char)((v & 127) | (v > 127 ? 128 : 0)); v >>= 7; } while (v);
    }
    static void lebS(std::string& b, long long v) { leb(b, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
    // static int NAME[n], filled from a byte string at startup (each number
    // as the signed distance from the one before it): its address is a
    // constant for the tables that point into it.
    static std::string intTable(const char* name, const std::vector<int>& v) {
        std::string b;
        long long at = 0;
        for (int x : v) { lebS(b, (long long)x - at); at = x; }
        return std::string("    static int ") + name + "[" + std::to_string(std::max<size_t>(v.size(), 1)) + "];\n"
               "    {\n        static const char B[] =\n" + blobText(b) + ";\n"
               "        MsimLeb r{(const unsigned char*)B};\n"
               "        long long at = 0;\n"
               "        for (size_t k = 0; k < " + std::to_string(v.size()) + "; k++) " + name + "[k] = (int)(at += r.s());\n"
               "    }\n";
    }
    // A byte string as string literals of 4 KB each (adjacent literals are one).
    static std::string blobText(const std::string& b) {
        std::string r;
        for (size_t k = 0; k < b.size(); k += 4096) r += "        " + quote(b.substr(k, 4096)) + "\n";
        if (b.empty()) r = "        \"\"\n";
        return r;
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
        if (ar) hitArr(e->arr.get());   // the interpreter reads it from S.sigs
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
                if (e->at) break;   // h.x: eval()
                if (isRel(e->sig))   // a frame slot (emitProc frames): resolved now
                    return "R(R(" + std::to_string(e->sig) + "), " + (e->sgn ? "true" : "false") + ")";
                if (isPriv(e->sig))
                    return "PR(P" + std::to_string(e->sig) + ", " + std::to_string(sigW_[e->sig]) + ", " + (e->sgn ? "true" : "false") + ")";
                return "R(" + std::to_string(e->sig) + ", " + (e->sgn ? "true" : "false") + ")";
            case Op::DynSlice:   // a[i +: w] / a[i -: w] read (11.5.1)
                return "DSL(" + k(0) + ", " + k(1) + ", " + std::to_string(e->hi) + ", " + (e->lo ? "true" : "false") + ")";
            case Op::ArrRead: {
                std::string ix;
                for (size_t i = 0; i < e->kids.size(); i++) ix += (i ? ", " : "") + k((int)i);
                if (privArr(e->arr.get()))   // every element private: read in place
                    return "APRV(" + arrName(e->arr.get()) + ", {" + ix + "}, &P" + std::to_string(e->arr->base) + ")";
                hitArr(e->arr.get());
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
            case Op::Slice: {
                const E& a = e->kids.at(0);
                // A field of a wide stored signal: read in place (sliceOf).
                if (a->op == Op::Ref && !a->at && !isRel(a->sig) && !isPriv(a->sig) && a->sig >= 0 && (size_t)a->sig < sigW_.size() &&
                    sigW_[a->sig] > 64 && e->lo >= 0 && e->hi >= e->lo && e->hi < sigW_[a->sig] && e->hi - e->lo < 64 && !noSliceOf_)
                    return "sliceOf(S.sigs[" + std::to_string(a->sig) + "].val, " + std::to_string(e->hi) + ", " + std::to_string(e->lo) + ")";
                return "v_slice(" + k(0) + ", " + std::to_string(e->hi) + ", " + std::to_string(e->lo) + ")";
            }
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
            // Side-effect-free operators ({32{a & b[31]}} lowers to 32 copies of one And).
            case Op::Not: case Op::And: case Op::Or: case Op::Xor: case Op::Eq: case Op::Ne:
            case Op::LogNot: case Op::LogAnd: case Op::LogOr: case Op::RedAnd: case Op::RedOr: case Op::RedXor:
            case Op::Mux: case Op::Resize:
                if (a->hi != b->hi || a->lo != b->lo || a->sgn != b->sgn) return false;
                break;
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
                if (e->at || e->sig < 0 || e->sig >= (int)sigW_.size() || sigW_[e->sig] > 64 || sigW_[e->sig] < 1) return false;
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
                int w = e->hi - e->lo + 1;
                const E& r = e->kids.empty() ? e : e->kids[0];
                if (r && r->op == Op::Ref && r->sig >= 0 && r->sig < (int)sigW_.size() && sigW_[r->sig] > 64 && !isPriv(r->sig) &&
                    e->lo >= 0 && w >= 1 && w <= 64 && e->hi < sigW_[r->sig]) {
                    // Up to 64 bits of a wider signal in S.sigs: from its limbs
                    // (one or two), known when those limbs are.
                    int l0 = e->lo / 64, l1 = e->hi / 64, sh = e->lo % 64;
                    std::string val = "S.sigs[" + std::to_string(r->sig) + "].val";
                    auto lv = [&](int l) { return "limbV(" + val + ", " + std::to_string(l) + ")"; };
                    auto lx = [&](int l) { return "limbX(" + val + ", " + std::to_string(l) + ")"; };
                    std::string a = "(" + lv(l0) + " >> " + std::to_string(sh) + ")";
                    if (l1 != l0) a = "(" + a + " | (" + lv(l1) + " << " + std::to_string(64 - sh) + "))";
                    out = "(" + a + " & " + M(w) + ")";
                    fxXg_.push_back(l1 != l0 ? lx(l0) + " | " + lx(l1) : lx(l0));
                    fi = {w, false};
                    return true;
                }
                if (!kid(0, A, fa)) return false;
                if (e->lo < 0 || e->lo >= 64 || w < 1 || w > 64 || e->hi >= fa.w) return false;   // bits past the width read x: v_slice
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
                const bool pa = privArr(a);   // every element private: (&P<base>)[i], shared chunks move P<base>
                for (int k = 0; k < a->count; k++)
                    if ((!pa && isPriv(a->base + k)) || a->base + k >= (int)sigW_.size() || sigW_[a->base + k] != a->elemW) return false;
                if (!kid(0, A, fa)) return false;
                auto [l, r] = a->dims[0];
                std::string off = l <= r ? "((uint64_t)(" + A + ") - (uint64_t)" + std::to_string(l) + "LL)"
                                         : "((uint64_t)" + std::to_string(l) + "LL - (uint64_t)(" + A + "))";
                std::string at = pa ? "(&P" + std::to_string(a->base) + ")[" + off + " < " + std::to_string(a->count) + " ? " + off + " : 0]"
                                    : "S.sigs[" + std::to_string(a->base) + " + (" + off + " < " + std::to_string(a->count) + " ? " + off + " : 0)].val";
                out = "(" + at + ".v)";
                fxOor_.push_back("(" + off + " >= " + std::to_string(a->count) + " || " + at + ".x)");
                fi = {a->elemW, e->sgn};
                return true;
            }
            case Op::DynSlice: {   // a[i +: w]: X when any of it is outside a
                if (e->lo || e->kids.size() != 2) return false;
                {
                    // Up to 64 bits of a wide signal in S.sigs at a run-time
                    // offset, through nested up-selects (v[i*320 +: 320][j*64 +: 64][k]):
                    // read in place, not as copies of the whole vector.
                    const Expr* c = e.get();
                    int cw = e->hi;
                    std::string off = "0";
                    std::vector<std::string> oor;
                    bool ok = cw >= 1 && cw <= 64;
                    while (ok && c->op == Op::DynSlice && !c->lo && c->kids.size() == 2) {
                        std::string I;
                        FastInfo fi_{};
                        if (!fx(c->kids[1], I, fi_, refs)) { ok = false; break; }
                        const Expr* b = c->kids[0].get();
                        int bw = b->op == Op::DynSlice && !b->lo ? b->hi
                               : b->op == Op::Ref && !b->at && b->sig >= 0 && b->sig < (int)sigW_.size() ? sigW_[b->sig] : -1;
                        if (bw < cw) { ok = false; break; }
                        oor.push_back("(uint64_t)(" + I + ") > " + std::to_string(bw - cw));
                        off = "(" + off + " + (uint64_t)(" + I + "))";
                        cw = bw;
                        c = b;
                    }
                    if (ok && c->op == Op::Ref && !c->at && !isRel(c->sig) && !isPriv(c->sig) && sigW_[c->sig] > 64 && !std::getenv("MSIM_NOWIDEBITS")) {
                        const std::string val = "S.sigs[" + std::to_string(c->sig) + "].val";
                        for (auto& q : oor) fxOor_.push_back(q);
                        fxOor_.push_back("bitsXZ(" + val + ", " + off + ", " + std::to_string(e->hi) + ")");
                        out = "bitsV(" + val + ", " + off + ", " + std::to_string(e->hi) + ")";
                        fi = {e->hi, false};
                        return true;
                    }
                }
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
    // ---- 4-state path in two words ----
    // When some input holds X (PicoRV32 drives 'bx onto its datapath every
    // cycle), the Val helpers cost a call and a by-value copy per operator.
    // For the same operators as fx(), up to 64 bits, this computes the value
    // word and the X word inline (as expr.hpp does: a value bit is 0 where
    // its X bit is set). It applies while no input holds Z (zGuard); the
    // result is then free of Z too. Each operator becomes one temporary in
    // `pre` (names _v<n> / _x<n>), so shared operands are not repeated.
    std::string zRef(int s) const { return isPriv(s) ? "P" + std::to_string(s) + ".z" : "S.sigs[" + std::to_string(s) + "].val.z"; }
    bool fx4(const E& e, std::string& V, std::string& X, FastInfo& fi, std::vector<int>& refs, std::string& pre, int& tn) {
        if (!e) return false;
        auto kid = [&](int i, std::string& v, std::string& x, FastInfo& f) {
            return i < (int)e->kids.size() && fx4(e->kids[i], v, x, f, refs, pre, tn);
        };
        auto tmp = [&](const std::string& v, const std::string& x) {
            std::string n = std::to_string(tn++);
            pre += "const uint64_t _v" + n + " = " + v + ", _x" + n + " = " + x + "; ";
            V = "_v" + n;
            X = "_x" + n;
        };
        std::string A, AX, B, BX, C, CX;
        FastInfo fa{}, fb{}, fc{};
        switch (e->op) {
            case Op::Const:
                if (e->k.w > 64 || e->k.w < 1 || e->k.real || e->k.str || !e->k.hi.empty() || e->k.z) return false;
                V = hex(e->k.v & ~e->k.x & mask(e->k.w));
                X = hex(e->k.x & mask(e->k.w));
                fi = {e->k.w, e->k.sgn};
                return true;
            case Op::Ref:
                if (e->sig < 0 || e->sig >= (int)sigW_.size() || sigW_[e->sig] > 64 || sigW_[e->sig] < 1) return false;
                refs.push_back(e->sig);
                tmp("(" + vRef(e->sig) + " & ~" + xRef(e->sig) + ")", xRef(e->sig));
                fi = {sigW_[e->sig], e->sgn};
                return true;
            case Op::Not:
                if (!kid(0, A, AX, fa)) return false;
                tmp("(~" + A + " & ~" + AX + " & " + M(fa.w) + ")", AX);
                fi = fa;
                return true;
            case Op::And: case Op::Or: case Op::Xor: {
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                int w = std::max(fa.w, fb.w);
                if (e->op == Op::And)   // a known 0 on either side gives 0
                    tmp("(" + A + " & " + B + ")", "((" + AX + " | " + BX + ") & ((" + A + " | " + AX + ") & (" + B + " | " + BX + ")))");
                else if (e->op == Op::Or)   // a known 1 on either side gives 1
                    tmp("(" + A + " | " + B + ")", "((" + AX + " | " + BX + ") & ~(" + A + " | " + B + "))");
                else
                    tmp("((" + A + " ^ " + B + ") & ~(" + AX + " | " + BX + "))", "(" + AX + " | " + BX + ")");
                fi = {w, fa.sgn && fb.sgn};
                return true;
            }
            case Op::Add: case Op::Sub: case Op::Mul: {   // any X: all X
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                int w = std::max(fa.w, fb.w);
                bool sg = fa.sgn && fb.sgn;
                const char* o = e->op == Op::Add ? " + " : e->op == Op::Sub ? " - " : " * ";
                std::string q = "(" + AX + " | " + BX + ")";
                tmp("(" + q + " ? 0ULL : ((" + ext(A, fa.w, w, sg) + o + ext(B, fb.w, w, sg) + ") & " + M(w) + "))",
                    "(" + q + " ? " + M(w) + " : 0ULL)");
                fi = {w, sg};
                return true;
            }
            case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge: {
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                int w = std::max(fa.w, fb.w);
                bool sg = fa.sgn && fb.sgn;
                std::string a = ext(A, fa.w, w, sg), b = ext(B, fb.w, w, sg), ax = ext(AX, fa.w, w, sg), bx = ext(BX, fb.w, w, sg);
                std::string anyx = "((" + AX + " | " + BX + ") != 0)";
                if (e->op == Op::Eq || e->op == Op::Ne) {   // a known bit that differs decides; else any X gives X
                    std::string d = "(((" + a + " ^ " + b + ") & ~" + ax + " & ~" + bx + ") != 0)";
                    std::string val = e->op == Op::Eq ? "!" + d : d;
                    tmp("(uint64_t)(!(!" + d + " && " + anyx + ") && " + val + ")", "(uint64_t)(!" + d + " && " + anyx + ")");
                } else {
                    if (sg) { a = "(int64_t)" + sx(a, w); b = "(int64_t)" + sx(b, w); }
                    const char* o = e->op == Op::Lt ? " < " : e->op == Op::Le ? " <= " : e->op == Op::Gt ? " > " : " >= ";
                    tmp("(uint64_t)(!" + anyx + " && (" + a + o + b + "))", "(uint64_t)" + anyx);
                }
                fi = {1, false};
                return true;
            }
            case Op::CaseEq: case Op::CaseZEq:   // no Z (zGuard): every bit, X included, compared as is
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                tmp("(uint64_t)(" + A + " == " + B + " && " + AX + " == " + BX + ")", "0ULL");
                fi = {1, false};
                return true;
            case Op::CaseXEq:     // X bits on either side match anything
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                tmp("(uint64_t)(((" + A + " ^ " + B + ") & ~(" + AX + " | " + BX + ")) == 0)", "0ULL");
                fi = {1, false};
                return true;
            case Op::Shl: case Op::Shr: {   // an unknown amount: all X
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                std::string u = "(" + BX + " != 0)", big = "(" + B + " >= 64)";
                if (e->op == Op::Shl)
                    tmp("(" + u + " || " + big + " ? 0ULL : ((" + A + " << " + B + ") & " + M(fa.w) + "))",
                        "(" + u + " ? " + M(fa.w) + " : " + big + " ? 0ULL : ((" + AX + " << " + B + ") & " + M(fa.w) + "))");
                else
                    tmp("(" + u + " || " + big + " ? 0ULL : (" + A + " >> " + B + "))",
                        "(" + u + " ? " + M(fa.w) + " : " + big + " ? 0ULL : (" + AX + " >> " + B + "))");
                fi = fa;
                return true;
            }
            case Op::SysFn:
                if (e->hi != SF_TO_2STATE || !kid(0, A, AX, fi)) return false;
                V = A;
                X = "0ULL";
                return true;
            case Op::Resize: {
                if (!kid(0, A, AX, fa)) return false;
                int w = e->hi;
                if (w < 1 || w > 64) return false;
                bool fill = e->lo && w > fa.w;
                tmp("(" + (fill ? sx(A, fa.w) : A) + " & " + M(w) + ")", "(" + (fill ? sx(AX, fa.w) : AX) + " & " + M(w) + ")");
                fi = {w, e->sgn};
                return true;
            }
            case Op::LogNot:
                if (!kid(0, A, AX, fa)) return false;
                tmp("(uint64_t)(" + A + " == 0 && " + AX + " == 0)", "(uint64_t)(" + A + " == 0 && " + AX + " != 0)");
                fi = {1, false};
                return true;
            case Op::LogAnd: case Op::LogOr: {
                if (!kid(0, A, AX, fa) || !kid(1, B, BX, fb)) return false;
                std::string a1 = "(" + A + " != 0)", b1 = "(" + B + " != 0)";
                std::string a0 = "(" + A + " == 0 && " + AX + " == 0)", b0 = "(" + B + " == 0 && " + BX + " == 0)";
                if (e->op == Op::LogAnd)
                    tmp("(uint64_t)(" + a1 + " && " + b1 + ")", "(uint64_t)(!(" + a0 + " || " + b0 + ") && !(" + a1 + " && " + b1 + "))");
                else
                    tmp("(uint64_t)(" + a1 + " || " + b1 + ")", "(uint64_t)(!(" + a1 + " || " + b1 + ") && !(" + a0 + " && " + b0 + "))");
                fi = {1, false};
                return true;
            }
            case Op::RedAnd: case Op::RedOr: case Op::RedXor: {
                if (!kid(0, A, AX, fa)) return false;
                if (e->op == Op::RedAnd) {
                    std::string k0 = "((" + M(fa.w) + " & ~" + AX + " & ~" + A + ") != 0)";
                    tmp("(uint64_t)(!" + k0 + " && " + AX + " == 0)", "(uint64_t)(!" + k0 + " && " + AX + " != 0)");
                } else if (e->op == Op::RedOr)
                    tmp("(uint64_t)(" + A + " != 0)", "(uint64_t)(" + A + " == 0 && " + AX + " != 0)");
                else
                    tmp("(uint64_t)(" + AX + " == 0 && __builtin_parityll(" + A + "))", "(uint64_t)(" + AX + " != 0)");
                fi = {1, false};
                return true;
            }
            case Op::Slice: {
                if (!kid(0, A, AX, fa)) return false;
                int w = e->hi - e->lo + 1;
                if (e->lo < 0 || e->lo >= 64 || w < 1 || w > 64 || e->hi >= fa.w) return false;   // bits past the width read x: v_slice
                std::string l = std::to_string(e->lo);
                tmp("((" + A + " >> " + l + ") & " + M(w) + ")", "((" + AX + " >> " + l + ") & " + M(w) + ")");
                fi = {w, false};
                return true;
            }
            case Op::Concat: {
                if (e->kids.empty()) return false;
                std::string rv, rx;
                int w = 0;
                for (size_t i = 0; i < e->kids.size();) {
                    std::string P, PX;
                    FastInfo fp{};
                    if (!fx4(e->kids[i], P, PX, fp, refs, pre, tn)) return false;
                    size_t j = i + 1;
                    if (fp.w == 1) while (j < e->kids.size() && pureEq(e->kids[i], e->kids[j])) j++;
                    int pw = fp.w * (int)(j - i);
                    if (j - i > 1) {
                        P = "(((uint64_t)0 - " + P + ") & " + M(pw) + ")";
                        PX = "(((uint64_t)0 - " + PX + ") & " + M(pw) + ")";
                    }
                    w += pw;
                    if (w > 64) return false;
                    std::string sh = std::to_string(pw);
                    rv = i ? "((" + rv + " << " + sh + ") | " + P + ")" : P;
                    rx = i ? "((" + rx + " << " + sh + ") | " + PX + ")" : PX;
                    i = j;
                }
                tmp(rv, rx);
                fi = {w, false};
                return true;
            }
            case Op::Mux: {   // an unknown condition: the bits both arms agree on, X elsewhere
                if (!kid(0, C, CX, fc) || !kid(1, A, AX, fa) || !kid(2, B, BX, fb)) return false;
                if (fa.w != fb.w || fa.sgn != fb.sgn) return false;
                std::string t1 = "(" + C + " != 0)", tx = "(" + CX + " != 0)";
                std::string same = "(~" + AX + " & ~" + BX + " & ~(" + A + " ^ " + B + ") & " + M(fa.w) + ")";
                tmp("(" + t1 + " ? " + A + " : " + tx + " ? (" + A + " & " + same + ") : " + B + ")",
                    "(" + t1 + " ? " + AX + " : " + tx + " ? (" + M(fa.w) + " & ~" + same + ") : " + BX + ")");
                fi = fa;
                return true;
            }
            default:
                return false;
        }
    }
    // `slow` (the Val code of e) with the two-word path in front of it when
    // no input holds Z. Used where some input holds X: the cold branches.
    std::string slow4(const E& e, const std::string& slow) {
        if (!twoState || std::getenv("MSIM_NOFX4")) return slow;
        std::string V, X, pre;
        FastInfo fi{};
        std::vector<int> refs;
        int tn = 0;
        if (!fx4(e, V, X, fi, refs, pre, tn)) return slow;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string zg;
        for (int r : refs) zg += (zg.empty() ? "" : " | ") + zRef(r);
        std::string val = "({ " + pre + "Val{" + V + ", " + X + ", " + std::to_string(fi.w) + ", 0, " + (fi.sgn ? "true" : "false") + "}; })";
        if (zg.empty()) return val;
        return "((" + zg + ") == 0 ? " + val + " : " + slow + ")";
    }
    // The expression as a Val, with the 2-state fast path when it applies.
    // The 2-state path in parts: `guard` true (or empty: always) means `u64`
    // is the value, else `slow`. False when there is no 2-state path.
    // plainSlow: `slow` stays ex(e), without slow4()'s inline 4-state form.
    bool split(const E& e, std::string& guard, std::string& u64, std::string& slow, bool plainSlow = false) {
        slow = ex(e);
        if (!twoState) return false;
        if (!plainSlow) slow = slow4(e, slow);
        FastInfo fi{};
        std::vector<int> refs;
        fxOor_.clear();
        fxXg_.clear();
        if (!fx(e, u64, fi, refs)) return false;
        // A run-time select that may fall outside its vector (the result is
        // then X): only where the caller tests the guard (dynOk_).
        if (!fxOor_.empty() && !dynOk_) return false;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        for (auto& x : fxXg_) g += (g.empty() ? "" : " | ") + x;
        guard = g.empty() ? "" : "(" + g + ") == 0";
        for (auto& c : fxOor_) guard += (guard.empty() ? "" : " && ") + ("!(" + c + ")");
        return true;
    }
    std::vector<std::string> fxOor_;   // fx: conditions under which the 2-state value does not hold
    std::vector<std::string> fxXg_;    // fx: X words to test besides those of the refs (limbs of wide signals)
    bool dynOk_ = false;
    std::string fast(const E& e) {
        std::string slow = ex(e);
        if (!twoState) return slow;
        std::string f;
        FastInfo fi{};
        std::vector<int> refs;
        fxOor_.clear();
        fxXg_.clear();
        if (!fx(e, f, fi, refs)) return slow;
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
        std::string g;
        for (int r : refs) g += (g.empty() ? "" : " | ") + xRef(r);
        for (auto& x : fxXg_) g += (g.empty() ? "" : " | ") + x;
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
        if (isRel(i.sig)) {   // a frame slot (emitProc frames, framesOK): resolved now
            const std::string t = "R(" + std::to_string(i.sig) + ")";
            if (i.tmp || i.xq) o << " S.poke(" << t << ", " << val << ", " << i.hi << ", " << i.lo << ");\n";
            else if (i.hi >= 0) o << " { int t = " << t << "; S.set(t, v_insert(S.get(t), " << val << ", " << i.hi << ", " << i.lo << ")); }\n";
            else o << " S.set(" << t << ", " << val << ");\n";
            return o.str();
        }
        if ((i.tmp || i.xq || (quiet(i.sig) && !i.bidx && i.drv < 0)) && !isPriv(i.sig)) {   // a temporary of the process, or a quiet -xprop write: no change event
            o << (i.tmp || i.xq ? " S.poke(" : " S.pokeQ(") << i.sig << ", " << val << ", " << i.hi << ", " << i.lo << ");\n";
            return o.str();
        }
        if (isPriv(i.sig)) {   // a testbench write to a private reg: the comb logic may read it
            if (i.drv >= 0 && i.hi < 0 && constInit_.count(i.sig) && i.e && i.e->op == Op::Const) return " ;\n";   // its PINIT value
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
        else if (i.hi >= 0 && sigW_[i.sig] > 64 && i.hi - i.lo < 64 && !std::getenv("MSIM_NOWIDEBITS"))   // in place (Sim::setBitsV)
            o << " S.setBitsV(" << i.sig << ", " << val << ", " << i.hi << ", " << i.lo << ");\n";
        else if (i.hi >= 0)
            o << " S.setPart(" << i.sig << ", " << val << ", " << i.hi << ", " << i.lo << ");\n";
        else if (sigW_[i.sig] == 1 && !sigNet_[i.sig] && !noSetBit_)
            o << " S.setOneBit(" << i.sig << ", " << val << ");\n";
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
    // Split builds (several C++ files): the header keeps only what does not
    // depend on the design's numbering (the kernel, helper functions, types).
    // Every variable becomes an extern declaration, its definition goes to
    // design.cpp, a reference to an array element (P<s>, F<k>, AI<k>) becomes
    // a macro, and each part gets just the declarations it names. A part
    // whose own text did not change then compiles to the same object even
    // when signals were added or removed elsewhere: it is not rebuilt
    // (msim.cpp runCompiled). Anything not understood stays in the header,
    // which only costs that (MSIM_FULLHDR=1 keeps the whole header).
    // ---- link-time signal numbers (split builds) ----
    // A part names signals by their place in the signal table (S.sigs[N],
    // P<N>), so adding or removing a signal anywhere renumbered the rest and
    // changed every part. relocText writes those numbers as symbols named
    // after the signal (SO_<name>: its byte offset in S.sigs, SN_<name>: its
    // number, PV_<name>: its private copy), defined once in design.cpp
    // (relocAsm) and resolved by the linker. A part then compiles to the same
    // object until its own signals change. The code is the same as with the
    // numbers: an absolute displacement or immediate (the parts and simv are
    // built without PIE for this; msim.cpp runCompiled, MSIM_RELOC).
    // x86-64 Linux only; MSIM_NORELOC=1 keeps the numbers.
    std::vector<std::string> symOf_;          // signal -> symbol stem ("" when not relocated)
    std::vector<std::array<int, 3>> slotSig_;   // NBA slot -> its target (sig, hi, lo), for slot symbols
    std::vector<std::string> slotSym_;          // NBA slot -> its symbol ("" when not relocated)
    std::map<std::string, std::string> relocSyms_;   // symbol -> its .set expression
    static bool relocOn() {
#if defined(__linux__) && defined(__x86_64__)
        return !std::getenv("MSIM_NORELOC");
#else
        return false;
#endif
    }
    void buildSymOf() {
        symOf_.assign(sigName_.size(), "");
        std::unordered_map<std::string, int> used;
        for (size_t s = 0; s < sigName_.size(); s++) {
            const std::string& n = sigName_[s];
            if (n.empty()) continue;
            uint64_t h = 1469598103934665603ULL;
            for (char c : n) h = (h ^ (unsigned char)c) * 1099511628211ULL;
            std::string m;
            for (size_t i = n.size() > 48 ? n.size() - 48 : 0; i < n.size(); i++)
                m += std::isalnum((unsigned char)n[i]) ? n[i] : '_';
            char hx[24];
            std::snprintf(hx, sizeof hx, "%012llx", (unsigned long long)(h & 0xffffffffffffULL));
            std::string stem = std::string(hx) + "_" + m;
            int& k = used[stem];
            if (k++) stem += "_" + std::to_string(k - 1);   // the same name twice: in table order
            symOf_[s] = stem;
        }
        // An NBA slot: its target's name, bits and how many slots before it
        // have that same target.
        slotSym_.assign(slotSig_.size(), "");
        std::map<std::array<int, 3>, int> nth;
        for (size_t k = 0; k < slotSig_.size(); k++) {
            auto [sg, hi, lo] = slotSig_[k];
            if (sg < 0 || (size_t)sg >= symOf_.size() || symOf_[(size_t)sg].empty()) continue;
            int i = nth[slotSig_[k]]++;
            slotSym_[k] = "SL_" + symOf_[(size_t)sg] + (hi >= 0 ? "_" + std::to_string(hi) + "_" + std::to_string(lo) : std::string()) +
                          (i ? "_" + std::to_string(i) : std::string());
        }
    }
    std::string relocText(const std::string& t, std::set<std::string>& syms) {
        std::string r;
        r.reserve(t.size() + t.size() / 4);
        size_t last = 0, skipTo = 0;
        auto stem = [&](long long n) -> const std::string* {
            if (n < 0 || (size_t)n >= symOf_.size() || symOf_[(size_t)n].empty()) return nullptr;
            return &symOf_[(size_t)n];
        };
        auto use = [&](const std::string& sym, const std::string& def) { syms.insert(sym); relocSyms_.emplace(sym, def); };
        refScan(t, [&](size_t start, size_t at, size_t e, long long n, int kind) {
            if (start < skipTo) return;
            if (kind == 1) {   // P<N>: the private copy
                const std::string* m = stem(n);
                if (!m || !priv_.size() || (size_t)n >= priv_.size() || !priv_[(size_t)n]) return;
                r.append(t, last, start - last);
                std::string sym = "PV_" + *m;
                use(sym, "PA+" + std::to_string(n) + "*%c1");
                r += sym;
                last = e;
                return;
            }
            if (kind != 0) return;
            const std::string* m = stem(n);
            if (!m) return;
            if (t.compare(start, 7, "S.sigs[") == 0) {
                size_t close = t.find(']', e);
                if (close == std::string::npos) return;
                std::string rest = t.substr(e, close - e);
                if (rest.find_first_of("[(") != std::string::npos) return;
                std::string sym = "SO_" + *m;
                use(sym, std::to_string(n) + "*%c0");
                r.append(t, last, start - last);
                r += "SGS((intptr_t)" + sym + (rest.empty() ? "" : " + (0" + rest + ") * (intptr_t)sizeof(Sig)") + ")";
                last = close + 1;
                skipTo = last;
                return;
            }
            std::string sym = "SN_" + *m;
            use(sym, std::to_string(n));
            r.append(t, last, at - last);
            r += "SNI(" + sym + ")";
            last = e;
        });
        r.append(t, last, std::string::npos);
        // PA[N + D<c>] (shared comb code, refParam): the private copy of the
        // first instance, moved by D.
        std::string o;
        o.reserve(r.size());
        for (size_t i = 0; i < r.size();) {
            if (r.compare(i, 3, "PA[") == 0 && (i == 0 || !(std::isalnum((unsigned char)r[i - 1]) || r[i - 1] == '_')) && i + 3 < r.size() &&
                std::isdigit((unsigned char)r[i + 3])) {
                size_t e = i + 3;
                while (e < r.size() && std::isdigit((unsigned char)r[e])) e++;
                long long n = std::strtoll(r.c_str() + i + 3, nullptr, 10);
                const std::string* m = stem(n);
                size_t close = r.find(']', e);
                if (m && (size_t)n < priv_.size() && priv_[(size_t)n] && close != std::string::npos &&
                    r.substr(e, close - e).find_first_of("[(") == std::string::npos) {
                    std::string sym = "PV_" + *m;
                    use(sym, "PA+" + std::to_string(n) + "*%c1");
                    o += "(&" + sym + ")[0" + r.substr(e, close - e) + "]";
                    i = close + 1;
                    continue;
                }
            }
            o += r[i++];
        }
        // NBA slots: VA[k], HA[k], FA[k], CLP(k), with k + D<c> in shared code.
        r.clear();
        for (size_t i = 0; i < o.size();) {
            size_t n = 0;
            for (const char* p : {"VA[", "HA[", "FA[", "CLP("}) {
                size_t l = std::strlen(p);
                if (o.compare(i, l, p) == 0 && (i == 0 || !(std::isalnum((unsigned char)o[i - 1]) || o[i - 1] == '_')) && i + l < o.size() &&
                    std::isdigit((unsigned char)o[i + l])) { n = l; break; }
            }
            if (n) {
                size_t e = i + n;
                while (e < o.size() && std::isdigit((unsigned char)o[e])) e++;
                long long k = std::strtoll(o.c_str() + i + n, nullptr, 10);
                if (k >= 0 && (size_t)k < slotSym_.size() && !slotSym_[(size_t)k].empty()) {
                    const std::string& sym = slotSym_[(size_t)k];
                    use(sym, std::to_string(k));
                    r.append(o, i, n);
                    r += "(intptr_t)" + sym;
                    i = e;
                    continue;
                }
            }
            r += o[i++];
        }
        return r;
    }
    // Text that names nothing by number: labels (p<proc>_<pc>, cb<proc>_<k>,
    // cq<proc>_<k>) and cold counters numbered within the chunk, the process
    // number left out of "// process N (...)" comments. Labels are local to
    // a function, so numbering them in order keeps them distinct.
    static std::string localNames(const std::string& t) {
        std::string r;
        r.reserve(t.size());
        std::unordered_map<std::string, int> lab;
        int cold = 0;
        auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        for (size_t i = 0; i < t.size();) {
            if (t.compare(i, 11, "// process ") == 0 && (i == 0 || t[i - 1] == '\n')) {
                size_t e = i + 11;
                while (e < t.size() && std::isdigit((unsigned char)t[e])) e++;
                r += "// process";
                i = e;
                continue;
            }
            if (t.compare(i, 5, "COLD(") == 0 && (i == 0 || !idc(t[i - 1]))) {
                size_t e = i + 5;
                while (e < t.size() && std::isdigit((unsigned char)t[e])) e++;
                if (e > i + 5) { r += "COLD(" + std::to_string(cold++); i = e; continue; }
            }
            if ((t[i] == 'p' || t.compare(i, 2, "cb") == 0 || t.compare(i, 2, "cq") == 0) && (i == 0 || !idc(t[i - 1]))) {
                size_t a = i + (t[i] == 'p' ? 1 : 2), e = a;
                while (e < t.size() && std::isdigit((unsigned char)t[e])) e++;
                if (e > a && e < t.size() && t[e] == '_') {
                    size_t f = e + 1;
                    while (f < t.size() && std::isdigit((unsigned char)t[f])) f++;
                    if (f > e + 1 && (f >= t.size() || !idc(t[f]))) {
                        std::string tok = t.substr(i, f - i);
                        auto [it, fresh] = lab.emplace(tok, (int)lab.size());
                        r += "L" + std::to_string(it->second) + "_";
                        i = f;
                        continue;
                    }
                }
            }
            r += t[i++];
        }
        return r;
    }
    // design.cpp: the symbols, in one asm block (sizes from the compiler).
    std::string relocAsm() const {
        if (relocSyms_.empty()) return "";
        std::string a = "__attribute__((used)) static void msimRelocSyms() {\n    asm volatile(\n";
        for (auto& [sym, def] : relocSyms_) a += "        \".globl " + sym + "\\n.set " + sym + ", " + def + "\\n\"\n";
        a += "        :: \"i\"(sizeof(Sig)), \"i\"(sizeof(PSig)));\n}\n";
        return a;
    }
    // Each identifier of t through f.
    static std::string mapIdents(const std::string& t, const std::function<std::string(const std::string&)>& f) {
        std::string r;
        r.reserve(t.size());
        for (size_t i = 0; i < t.size();) {
            if ((std::isalpha((unsigned char)t[i]) || t[i] == '_') && (i == 0 || !(std::isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_'))) {
                size_t j = i;
                while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
                r += f(t.substr(i, j - i));
                i = j;
            } else r += t[i++];
        }
        return r;
    }
    static std::string relocDecls(const std::set<std::string>& syms) {
        std::string d, so, pv;
        for (auto& x : syms) (x[0] == 'P' ? pv : so) += (x[0] == 'P' ? (pv.empty() ? "" : ", ") : (so.empty() ? "" : ", ")) + x + (x[0] == 'P' ? "" : "[]");
        if (!so.empty()) d += "extern \"C\" char " + so + ";\n";
        if (!pv.empty()) d += "extern \"C\" PSig " + pv + ";\n";
        return d;
    }

    struct HdrItem {
        std::vector<std::string> names;   // what it declares
        std::string decl;                 // into each part that names one of them, and design.cpp
        std::string def;                  // design.cpp only
        std::vector<int> deps;            // other items its declaration names
    };
    static constexpr const char* kMacroMark = "// @macros";
    static void identsOf(const std::string& t, const std::function<void(const std::string&)>& f) {
        for (size_t i = 0; i < t.size();) {
            unsigned char c = (unsigned char)t[i];
            if (std::isalpha(c) || c == '_') {
                size_t j = i + 1;
                while (j < t.size() && (std::isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
                f(t.substr(i, j - i));
                i = j;
            } else if (std::isdigit(c)) {
                while (i < t.size() && (std::isalnum((unsigned char)t[i]) || t[i] == '_')) i++;
            } else i++;
        }
    }
    // The top-level items of generated C++: preprocessor lines, declarations
    // and definitions, each with the comment lines before it.
    static std::vector<std::string> topItems(const std::string& t) {
        std::vector<std::string> out;
        size_t start = 0, i = 0, n = t.size();
        int depth = 0;
        bool eq = false;
        while (i < n) {
            char c = t[i];
            if (c == '/' && i + 1 < n && t[i + 1] == '/') { size_t nl = t.find('\n', i); i = nl == std::string::npos ? n : nl; continue; }
            if (c == '#' && depth == 0 && (i == 0 || t[i - 1] == '\n') && t.compare(i, 3, "#if") == 0) {
                // A conditional block stays one piece (of the header).
                int lv = 0;
                size_t q = i;
                while (q < n) {
                    size_t nl = t.find('\n', q);
                    std::string ln = t.substr(q, (nl == std::string::npos ? n : nl) - q);
                    if (ln.compare(0, 3, "#if") == 0) lv++;
                    else if (ln.compare(0, 6, "#endif") == 0 && --lv == 0) { q = nl == std::string::npos ? n : nl + 1; break; }
                    q = nl == std::string::npos ? n : nl + 1;
                }
                i = q;
                out.push_back(t.substr(start, i - start));
                start = i;
                continue;
            }
            if (c == '#' && depth == 0 && (i == 0 || t[i - 1] == '\n')) {
                size_t nl = i;
                for (;;) {   // a line continued with a backslash
                    nl = t.find('\n', nl);
                    if (nl == std::string::npos || nl == 0 || t[nl - 1] != '\\') break;
                    nl++;
                }
                i = nl == std::string::npos ? n : nl + 1;
                out.push_back(t.substr(start, i - start));
                start = i;
                continue;
            }
            if (c == '"' || c == '\'') {
                for (i++; i < n && t[i] != c; i++) if (t[i] == '\\') i++;
                i++;
                continue;
            }
            if (c == '{') depth++;
            else if (c == '}') depth--;
            else if (c == '=' && depth == 0) eq = true;
            i++;
            if (depth == 0 && (c == ';' || (c == '}' && !eq))) {
                while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
                if (i < n && t[i] == ';') i++;
                while (i < n && t[i] != '\n' && (t[i] == ' ' || t[i] == '\t' || t[i] == '/')) {   // a trailing comment
                    if (t[i] == '/') { size_t nl = t.find('\n', i); i = nl == std::string::npos ? n : nl; break; }
                    i++;
                }
                if (i < n && t[i] == '\n') i++;
                out.push_back(t.substr(start, i - start));
                start = i;
                eq = false;
            }
        }
        if (start < n) out.push_back(t.substr(start));
        return out;
    }
    // A variable definition "inline|static <head> [= init | {init}];": its
    // declared name and the head without the initializer. False when it is
    // not one (or not one this understands).
    static bool varParts(const std::string& body, std::string& head, std::string& init, std::string& name) {
        std::string b = body;
        if (b.compare(0, 7, "inline ") == 0) b = b.substr(7);
        else if (b.compare(0, 7, "static ") == 0) b = b.substr(7);
        else return false;
        if (b.compare(0, 7, "inline ") == 0) b = b.substr(7);
        if (b.compare(0, 9, "template ") == 0 || b.compare(0, 7, "struct ") == 0 || b.compare(0, 6, "using ") == 0) return false;
        int par = 0, br = 0, ang = 0;
        size_t cut = std::string::npos;
        for (size_t i = 0; i < b.size(); i++) {
            char c = b[i];
            if (c == '(') par++;
            else if (c == ')') par--;
            else if (c == '[') br++;
            else if (c == ']') br--;
            else if (c == '<') ang++;
            else if (c == '>') ang--;
            else if (par == 0 && br == 0 && ang == 0) {
                if (c == ',') return false;   // several declarators
                if (c == '=' || c == '{' || c == ';') { cut = i; break; }
            }
        }
        if (cut == std::string::npos) return false;
        head = b.substr(0, cut);
        init = b.substr(cut);
        while (!head.empty() && head.back() == ' ') head.pop_back();
        if (head.find("struct {") != std::string::npos || head.find('(') != std::string::npos && head.find("(*") == std::string::npos) return false;
        if (b[cut] == '{') {   // a function body, not a brace initializer?
            if (!head.empty() && head.back() == ')') return false;
        }
        // The name: the first identifier followed by '[', else the first
        // followed by ')' (a function pointer), else the last one.
        name.clear();
        std::string br1, pa1, last;
        for (size_t i = 0; i < head.size();) {
            if (std::isalpha((unsigned char)head[i]) || head[i] == '_') {
                size_t j = i + 1;
                while (j < head.size() && (std::isalnum((unsigned char)head[j]) || head[j] == '_')) j++;
                size_t k = j;
                while (k < head.size() && head[k] == ' ') k++;
                std::string id = head.substr(i, j - i);
                if (k < head.size() && head[k] == '[' && br1.empty()) br1 = id;
                if (k < head.size() && head[k] == ')' && pa1.empty()) pa1 = id;
                if (k == head.size()) last = id;
                i = j;
            } else i++;
        }
        name = !br1.empty() ? br1 : !pa1.empty() ? pa1 : last;
        return !name.empty() && name != "const";
    }
    static void declSplit(const std::string& hdr, std::string& stable, std::vector<HdrItem>& items) {
        std::vector<std::string> raw = topItems(hdr);
        // Per raw item: an item (variable, function declaration), a candidate
        // (a macro, function or type: an item only if it names an item), or
        // stable text.
        std::vector<HdrItem> it(raw.size());
        std::vector<int> kind(raw.size(), 0);   // 0 stable, 1 item, 2 candidate
        for (size_t r = 0; r < raw.size(); r++) {
            const std::string& t = raw[r];
            size_t at = t.find_first_not_of(" \t\n");
            while (at != std::string::npos && t.compare(at, 2, "//") == 0) {
                size_t nl = t.find('\n', at);
                at = nl == std::string::npos ? std::string::npos : t.find_first_not_of(" \t\n", nl + 1);
            }
            if (at == std::string::npos) continue;
            std::string body = t.substr(at);
            while (!body.empty() && (body.back() == '\n' || body.back() == ' ')) body.pop_back();
            {   // a comment after the last ';'
                size_t sc = body.rfind(';');
                if (sc != std::string::npos && body.find('\n', sc) == std::string::npos && body.find("//", sc) != std::string::npos) body.erase(sc + 1);
            }
            HdrItem& h = it[r];
            std::string head, init, name;
            if (body[0] != '#' && body.find('{') == std::string::npos && body.back() == ';' && body.find('=') == std::string::npos &&
                body.compare(0, 7, "inline ") != 0 && body.compare(0, 7, "static ") != 0 && body.compare(0, 6, "using ") != 0 &&
                body.compare(0, 7, "struct ") != 0 && body.compare(0, 9, "template ") != 0 && body.find('(') != std::string::npos) {
                // A function declaration: "Task proc12();", "void ds3(const long D0, ...);"
                std::string nm, sig = body;
                for (size_t q; (q = sig.find("__attribute__((")) != std::string::npos;) {
                    size_t e = sig.find("))", q);
                    if (e == std::string::npos) break;
                    sig.erase(q, e + 2 - q);
                }
                identsOf(sig.substr(0, sig.find('(')), [&](const std::string& x) { nm = x; });
                if (nm.empty()) continue;
                h.names = {nm};
                h.decl = body + "\n";
                kind[r] = 1;
            } else if (body[0] != '#' && varParts(body, head, init, name)) {
                std::string ext = head;
                size_t nb = std::string::npos;
                for (size_t p = ext.find(name); p != std::string::npos; p = ext.find(name, p + 1)) {
                    auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
                    if ((p == 0 || !idc(ext[p - 1])) && (p + name.size() == ext.size() || !idc(ext[p + name.size()]))) nb = p;
                }
                if (nb == std::string::npos) continue;
                size_t af = nb + name.size();
                if (af + 1 < ext.size() && ext[af] == '[' && ext[af + 1] != ']') {
                    // A stated bound grows with the design (PA, FA): the parts
                    // do not need it, so they do not change with it.
                    size_t e = ext.find(']', af);
                    if (e == std::string::npos) continue;
                    if (ext.find('[', e) == std::string::npos) ext.erase(af + 1, e - af - 1);
                } else if (af + 1 < ext.size() && ext[af] == '[' && ext[af + 1] == ']') {
                    // The bound from the initializer: a declaration with it keeps
                    // sizeof and range for working in the parts.
                    size_t ob = init.find('{');
                    if (ob == std::string::npos) continue;
                    int dep = 0;
                    size_t cnt = 0;
                    bool any = false;
                    for (size_t q = ob; q < init.size(); q++) {
                        char c = init[q];
                        if (c == '"' || c == '\'') { char e = c; for (q++; q < init.size() && init[q] != e; q++) if (init[q] == '\\') q++; any = true; continue; }
                        if (c == '{' || c == '(' || c == '[') { if (dep == 1) any = true; dep++; }
                        else if (c == '}' || c == ')' || c == ']') { dep--; if (dep == 0) break; }
                        else if (c == ',' && dep == 1) { cnt++; any = false; }
                        else if (dep == 1 && c != ' ' && c != '\n') any = true;
                    }
                    if (any) cnt++;
                    ext.insert(af + 1, std::to_string(std::max<size_t>(cnt, 1)));
                }
                h.names = {name};
                kind[r] = 1;
                // A reference to an array element (P<s> = PA[s]): a macro.
                if (head.find('&') != std::string::npos && init.compare(0, 2, "= ") == 0) {
                    std::string ix = init.substr(2);
                    while (!ix.empty() && (ix.back() == ';' || ix.back() == ' ')) ix.pop_back();
                    if (!ix.empty() && ix.back() == ']' && ix.find('[') != std::string::npos && ix.find_first_of("(=,") == std::string::npos) {
                        h.decl = "#define " + name + " " + ix + "\n";
                        continue;
                    }
                }
                h.decl = "extern " + ext + ";\n";
                h.def = head + init;
                if (h.def.back() != ';') h.def += ";";
                h.def += "\n";
            } else if (body.compare(0, 8, "#define ") == 0) {
                size_t e = 8;
                while (e < body.size() && (std::isalnum((unsigned char)body[e]) || body[e] == '_')) e++;
                h.names = {body.substr(8, e - 8)};
                h.decl = t;
                kind[r] = 2;
            } else if (body[0] != '#' && body.compare(0, 6, "using ") != 0) {
                // a function, type or template: named by its identifier before '(' or after struct
                std::string nm;
                size_t p = body.find('('), sb = body.find("struct ");
                if (sb != std::string::npos && (p == std::string::npos || sb < p))
                    identsOf(body.substr(sb + 7, 80), [&](const std::string& x) { if (nm.empty()) nm = x; });
                else if (p != std::string::npos) {
                    std::string sig = body;
                    for (size_t q; (q = sig.find("__attribute__((")) != std::string::npos;) {
                        size_t e = sig.find("))", q);
                        if (e == std::string::npos) break;
                        sig.erase(q, e + 2 - q);
                    }
                    identsOf(sig.substr(0, sig.find('(')), [&](const std::string& x) { nm = x; });
                }
                h.names = {nm.empty() ? "@" + std::to_string(r) : nm};
                h.decl = t;
                kind[r] = 2;
            }
        }
        // A candidate that names an item is an item (the assertion helpers
        // reading XSL, CLP reading CL); repeat until nothing changes.
        std::unordered_set<std::string> named;
        for (size_t r = 0; r < raw.size(); r++) if (kind[r] == 1) named.insert(it[r].names[0]);
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t r = 0; r < raw.size(); r++) {
                if (kind[r] != 2) continue;
                bool hit = false;
                identsOf(it[r].decl, [&](const std::string& x) { if (x != it[r].names[0] && named.count(x)) hit = true; });
                if (!hit) continue;
                kind[r] = 1;
                named.insert(it[r].names[0]);
                grew = true;
            }
        }
        stable.clear();
        items.clear();
        for (size_t r = 0; r < raw.size(); r++) {
            if (kind[r] == 1) items.push_back(std::move(it[r]));
            else stable += raw[r];
        }
        std::unordered_map<std::string, int> owner;
        for (size_t i = 0; i < items.size(); i++) for (auto& x : items[i].names) owner[x] = (int)i;
        for (size_t i = 0; i < items.size(); i++) {
            std::set<int> d;
            identsOf(items[i].decl, [&](const std::string& x) {
                auto f = owner.find(x);
                if (f != owner.end() && f->second != (int)i) d.insert(f->second);
            });
            items[i].deps.assign(d.begin(), d.end());
        }
    }
    // The declarations a part needs: the items it names, and theirs.
    static std::string partDecls(const std::string& text, const std::vector<HdrItem>& items, const std::unordered_map<std::string, int>& owner) {
        std::vector<char> need(items.size(), 0);
        std::vector<int> work;
        identsOf(text, [&](const std::string& x) { auto f = owner.find(x); if (f != owner.end() && !need[f->second]) { need[f->second] = 1; work.push_back(f->second); } });
        while (!work.empty()) {
            int i = work.back();
            work.pop_back();
            for (int d : items[i].deps) if (!need[d]) { need[d] = 1; work.push_back(d); }
        }
        std::string out;
        for (size_t i = 0; i < items.size(); i++) if (need[i]) out += items[i].decl;
        return out;
    }
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
    // A case statement as the 2-state code sees it: a chain of tests
    // "if (!(sel == K1 || sel == K2)) goto next", each failing to the next
    // test, on one selector of at most 64 bits and constant keys. The tests
    // have no effect, so the first one that holds is found by one switch on
    // the selector; the chain stays for the 4-state code. caseDispatch
    // returns that switch for the chain starting at pc (empty: no chain of
    // at least 4 keys), and marks the later tests of the chain in `in`.
    // MSIM_NOCASESWITCH: off. MSIM_WHYCASE=1: the first tests that are not a chain.
    // A selector: signals, constant selects of them and concatenations (at most 64 bits).
    bool caseSel(const E& e) const {
        if (!e || e->at || e->arr) return false;
        switch (e->op) {
            case Op::Ref: return e->sig >= 0 && (size_t)e->sig < sigW_.size() && sigW_[e->sig] <= 64;
            case Op::Slice: return e->kids.size() == 1 && caseSel(e->kids[0]);
            case Op::Resize: return !e->lo && e->kids.size() == 1 && caseSel(e->kids[0]);
            case Op::Concat: for (auto& k : e->kids) if (!caseSel(k)) return false; return !e->kids.empty();
            default: return false;
        }
    }
    static bool sameExpr(const E& a, const E& b) {
        if (a == b) return true;
        if (!a || !b || a->op != b->op || a->sig != b->sig || a->hi != b->hi || a->lo != b->lo || a->kids.size() != b->kids.size() || a->at || b->at) return false;
        if (a->op == Op::Const && !(a->k == b->k)) return false;
        for (size_t j = 0; j < a->kids.size(); j++) if (!sameExpr(a->kids[j], b->kids[j])) return false;
        return true;
    }
    bool caseKeys(const E& e, E& sel, std::vector<uint64_t>& keys) const {
        if (!e || e->at) return false;
        if ((e->op == Op::Or || e->op == Op::LogOr) && e->kids.size() == 2)
            return caseKeys(e->kids[0], sel, keys) && caseKeys(e->kids[1], sel, keys);
        // ==, ===, casez / casex and ==? (inside) all mean == for a key without x or z bits.
        bool wild = e->op == Op::CaseZEq || e->op == Op::CaseXEq || e->op == Op::WildEq;
        if ((e->op != Op::Eq && e->op != Op::CaseEq && !wild) || e->kids.size() != 2) return false;
        E a = e->kids[0], b = e->kids[1];
        if (a && a->op == Op::Const && e->op != Op::WildEq) std::swap(a, b);   // ==? : the pattern is the right one
        if (!a || !b || b->op != Op::Const || b->k.x || b->k.z || !b->k.hi.empty() || b->k.real || b->k.str) return false;
        if (a->op == Op::Resize && !a->lo && a->kids.size() == 1) a = a->kids[0];
        int w = widthOf(a);
        if (w < 1 || w > 64 || !caseSel(a)) return false;
        if (sel && !sameExpr(sel, a)) return false;
        sel = a;
        uint64_t k = b->k.v;
        if (w < 64 && (k >> w)) return true;   // wider than the selector: never matches
        keys.push_back(k);
        return true;
    }
    template <class Lab>
    std::string caseDispatch(const Prog& cp, size_t pc, std::vector<char>& in, const Lab& lab) {
        if (std::getenv("MSIM_NOCASESWITCH")) return "";
        E sel;
        std::vector<std::pair<uint64_t, size_t>> arms;   // key, the body it selects
        std::set<uint64_t> seen;
        std::vector<size_t> tests;
        size_t k = pc, nTests = 0;
        while (k < cp.code.size() && cp.code[k].k == IK::JmpIfNot && cp.code[k].xt == Instr::NOX) {
            std::vector<uint64_t> keys;
            E s = sel;
            if (!caseKeys(cp.code[k].e, s, keys)) {
                if (std::getenv("MSIM_WHYCASE") && k == pc) {
                    std::function<std::string(const E&)> pr = [&](const E& e) -> std::string {
                        if (!e) return "null";
                        std::string r = "op" + std::to_string((int)e->op);
                        if (e->op == Op::Ref) r += "[s" + std::to_string(e->sig) + "]";
                        if (e->op == Op::Const) r += "[w" + std::to_string(e->k.w) + "]";
                        if (e->op == Op::Resize || e->op == Op::Slice) r += "[" + std::to_string(e->hi) + "," + std::to_string(e->lo) + "]";
                        if (e->kids.empty()) return r;
                        r += "(";
                        for (size_t j = 0; j < e->kids.size() && j < 3; j++) r += (j ? "," : "") + pr(e->kids[j]);
                        return r + ")";
                    };
                    std::fprintf(stderr, "case? %s\n", pr(cp.code[k].e).substr(0, 300).c_str());
                }
                break;
            }
            sel = s;
            for (uint64_t x : keys) if (seen.insert(x).second) arms.push_back({x, k + 1});   // the first test that holds wins
            tests.push_back(k);
            nTests++;
            if (cp.code[k].target <= k) { k = cp.code[k].target; break; }
            k = cp.code[k].target;
        }
        if (!sel || nTests < 1 || arms.size() < 4 || k > cp.code.size()) return "";
        std::string g, u, sl;
        if (!split(sel, g, u, sl)) return "";
        for (size_t t : tests) if (t != pc) in[t] = 1;
        std::string r = " switch ((uint64_t)(" + u + ") & " + M(widthOf(sel)) + ") {";
        for (auto& [key, body] : arms) r += " case " + std::to_string(key) + "ULL: goto " + lab(body) + ";";
        return r + " default: goto " + lab(k) + "; }\n";
    }
    // A testbench clock: `always #h clk = ~clk` (or an initial block that
    // ends in `forever #h clk = ~clk`) on a one-bit signal. The kernel
    // flips it itself when it is all that happens at a time step
    // (Sim::addFastClock), without resuming the coroutine.
    bool toggleClock(const Prog& p, int& sig, uint64_t& half) const {
        static const bool off = std::getenv("MSIM_NOFASTCLK") != nullptr;
        const auto& c = p.code;
        if (off || c.size() < 3) return false;
        const size_t k = c.size() - 3;
        const Instr &d = c[k], &a = c[k + 1], &j = c[k + 2];
        if (d.k != IK::WaitDelay || d.de || d.delay == 0) return false;
        if (j.k != IK::Jmp || j.target != k) return false;
        if (a.k != IK::Assign || a.hi >= 0 || a.bidx || a.arr || !a.aidx.empty() || a.tmp || a.drv >= 0 || a.de || !a.dlyE.empty()) return false;
        if (a.sig < 0 || (size_t)a.sig >= sigW_.size() || sigW_[a.sig] != 1) return false;
        const E& e = a.e;
        if (!e || (e->op != Op::Not && e->op != Op::LogNot) || e->kids.size() != 1) return false;
        const E& r = e->kids[0];
        if (!r || r->op != Op::Ref || r->sig != a.sig || r->at) return false;
        for (size_t q = 0; q < k; q++)   // the part before the loop runs once, at time 0, without waiting
            if (c[q].k != IK::Assign) return false;
        sig = a.sig;
        half = d.delay;
        return true;
    }
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
                reachAll(pr.prog, progs);
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
        // Fixed NBA targets of every clocked process: the process that writes
        // it (-2: more than one).
        std::map<int, int> fixedNba;
        viaK_.clear();
        for (size_t i = 0; i < procs.size(); i++)
            if (procs[i].seq && procs[i].combRank < 0)
                for (auto& in : procs[i].prog->code)
                    if (in.k == IK::Nba && !in.arr && in.aidx.empty() && !in.bidx) {
                        auto [it, fresh] = fixedNba.emplace(in.sig, (int)i);
                        if (!fresh && it->second != (int)i) it->second = -2;
                    }
        // An element (or vector) the same process also writes at a run-time
        // index: its fixed NBAs go through the kernel's list too, in program
        // order with the others (viaK_). Another process's fixed NBA to it: no.
        auto clashAt = [&](int sg, size_t i, std::vector<int>& via) {
            auto it = fixedNba.find(sg);
            if (it == fixedNba.end()) return false;
            if (it->second != (int)i || std::getenv("MSIM_NOVIAK")) return true;
            via.push_back(sg);
            return false;
        };
        for (size_t i = 0; i < procs.size(); i++) {
            if (!procs[i].seq || procs[i].combRank >= 0) continue;
            bool forward = true;
            const char* why = "";
            int whyLine = 0;
            const auto& code = procs[i].prog->code;
            std::vector<int> via;
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
                if (code[pc].k == IK::Nba && code[pc].arr && code[pc].drv < 0 && code[pc].dyn < 0 && !code[pc].nbaLater &&
                    !code[pc].de && !code[pc].delay && !std::getenv("MSIM_NODOMARR")) {
                    bool clash = false;
                    for (int64_t k = 0; k < code[pc].arr->count; k++) clash |= clashAt((int)(code[pc].arr->base + k), i, via);
                    if (clash) { forward = false; why = "array also written at a fixed index by another process"; whyLine = code[pc].line; }
                } else if (code[pc].k == IK::Nba && code[pc].bidx && code[pc].drv < 0 && code[pc].dyn < 0 && !code[pc].nbaLater &&
                           !code[pc].de && !code[pc].delay && !std::getenv("MSIM_NODOMARR")) {
                    // Bits of a vector chosen at run time (q[i][k] <= ... of a packed array): the
                    // same way, the vector staying in S.sigs (the private signal choice leaves it there).
                    if (clashAt(code[pc].sig, i, via)) { forward = false; why = "vector also written at fixed bits by another process"; whyLine = code[pc].line; }
                } else if (!tmpw && (code[pc].arr || code[pc].bidx)) { forward = false; why = "run-time index"; whyLine = code[pc].line; }   // no fixed slot: an ordinary clocked function
            }
            if (forward)   // automatic variables (a frame): an ordinary clocked function or coroutine
                for (auto& in : code)
                    try { noFrames(in); } catch (const CodegenUnsupported&) { forward = false; why = "automatic variables"; whyLine = in.line; break; }
            if (forward) viaK_.insert(via.begin(), via.end());
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
                reachAll(procs[i].prog, progs);
                for (const Prog* p : progs)
                    for (auto& in : p->code)
                        if (in.k == IK::Nba && std::binary_search(targets.begin(), targets.end(), in.sig)) {
                            if (std::getenv("MSIM_WHYCYCLE"))
                                std::fprintf(stderr, "domain clash: %s (line %d) also writes %s\n", procs[i].kind.c_str(), in.line, sigs[in.sig].name.c_str());
                            return true;
                        }
            }
            return false;
        };
        // Every group (each wakes on its own events, see dseq), when there
        // are few; else, or when that clashes, the largest one alone.
        std::vector<int> all;
        // (A multi-core SoC has a gated clock per core: a few groups per core.)
        const bool bestAlone = std::getenv("MSIM_BESTALONE") && groups.size() > 64;
        if (groups.size() > 1 && groups.size() <= 1024 && !std::getenv("MSIM_ONEGROUP") && !bestAlone) {
            for (auto& g : groups) all.insert(all.end(), g.begin(), g.end());
            std::sort(all.begin(), all.end());
            if (clash(all) && !std::getenv("MSIM_NOPRUNE")) {
                // Leave out the processes whose NBA targets another process
                // outside the domain also writes (each run that way is an
                // ordinary clocked function), until nothing clashes.
                std::vector<char> in(procs.size(), 0);
                for (int i : all) in[i] = 1;
                std::vector<int> outW(sigs.size(), 0);   // NBA writes from outside the domain
                auto addOut = [&](size_t i) {
                    std::vector<const Prog*> progs;
                    reachAll(procs[i].prog, progs);
                    for (const Prog* q : progs)
                        for (auto& x : q->code)
                            if (x.k == IK::Nba && x.sig >= 0 && (size_t)x.sig < outW.size()) outW[x.sig]++;
                };
                for (size_t i = 0; i < procs.size(); i++) if (!in[i]) addOut(i);
                for (bool more = true; more;) {
                    more = false;
                    for (int i : all) {
                        if (!in[i]) continue;
                        bool hit = false;
                        for (auto& x : procs[i].prog->code) hit |= x.k == IK::Nba && x.sig >= 0 && (size_t)x.sig < outW.size() && outW[x.sig] > 0;
                        if (!hit) continue;
                        in[i] = 0;
                        addOut((size_t)i);
                        more = true;
                        if (std::getenv("MSIM_WHYCYCLE")) std::fprintf(stderr, "left out of the domain (a target written outside it): %s line %d\n", procs[i].kind.c_str(), procs[i].prog->code.empty() ? 0 : procs[i].prog->code[0].line);
                    }
                }
                std::erase_if(all, [&](int i) { return !in[i]; });
                for (auto& g : groups) std::erase_if(g, [&](int i) { return !in[i]; });
                std::erase_if(groups, [](auto& g) { return g.empty(); });
            }
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

    // The order the domain's processes run in, within each clock group. A
    // register one process writes (NBA) and another reads can be stored at
    // once when the reader runs first: it still sees the old value, and the
    // commit list is skipped (see direct). Readers go before writers as far
    // as the read graph allows (picorv32's decoder and memory interface
    // read the main state machine's registers). Greedy feedback-arc order
    // (Eades, Lin, Smyth 1993), weighted by the writes each order saves;
    // ties keep the original order. MSIM_NOORDER: as found.
    // The greedy order above can put a process that many others read
    // before one of its readers. Within each set of processes joined by
    // read edges (a core's always blocks), when it is small, take the order
    // that leaves the least weight read after its writer (DP over subsets),
    // in the places the set had.
    static void exactOrder(std::vector<int>& ord, const std::map<std::pair<int, int>, long>& w) {
        std::map<int, int> pos, par;
        for (size_t i = 0; i < ord.size(); i++) { pos[ord[i]] = (int)i; par[ord[i]] = ord[i]; }
        std::function<int(int)> root = [&](int x) { return par[x] == x ? x : par[x] = root(par[x]); };
        for (auto& [e, x] : w)
            if (pos.count(e.first) && pos.count(e.second)) par[root(e.first)] = root(e.second);
        std::map<int, std::vector<int>> comp;
        for (int v : ord) comp[root(v)].push_back(v);
        for (auto& [r, mem] : comp) {
            const size_t n = mem.size();
            if (n < 3 || n > 16) continue;
            std::vector<std::vector<long>> W(n, std::vector<long>(n, 0));   // W[a][b]: a reads b
            for (size_t a = 0; a < n; a++)
                for (size_t b = 0; b < n; b++) {
                    auto it = w.find({mem[a], mem[b]});
                    if (it != w.end()) W[a][b] = it->second;
                }
            const size_t full = (size_t)1 << n;
            std::vector<long> f(full, LONG_MAX);
            std::vector<int8_t> last(full, -1);
            f[0] = 0;
            for (size_t S = 0; S < full; S++) {
                if (f[S] == LONG_MAX) continue;
                for (size_t v = 0; v < n; v++) {
                    if (S >> v & 1) continue;
                    long c = f[S];
                    for (size_t u = 0; u < n; u++) if (S >> u & 1) c += W[v][u];   // v reads u, which ran first
                    const size_t T = S | (size_t)1 << v;
                    if (c < f[T]) { f[T] = c; last[T] = (int8_t)v; }
                }
            }
            long cur = 0;   // the greedy order's cost (mem is in that order)
            for (size_t a = 0; a < n; a++) for (size_t b = 0; b < a; b++) cur += W[a][b];
            if (f[full - 1] >= cur) continue;
            std::vector<int> best;
            for (size_t S = full - 1; S; S &= ~((size_t)1 << last[S])) best.push_back(mem[(size_t)last[S]]);
            std::reverse(best.begin(), best.end());
            std::vector<int> slots;
            for (int v : mem) slots.push_back(pos[v]);
            std::sort(slots.begin(), slots.end());
            for (size_t k = 0; k < n; k++) ord[(size_t)slots[k]] = best[k];
        }
    }

    void orderDomain(const std::vector<CodegenProc>& procs, const std::vector<CodegenSig>& sigs, std::vector<int>& dom) {
        if (dom.size() < 2 || std::getenv("MSIM_NOORDER")) return;
        const size_t ns = sigs.size();
        std::vector<int> wproc(ns, -1), nsite(ns, 0);
        auto okS = [&](int x) { return x >= 0 && (size_t)x < ns; };
        for (int p : dom)
            for (auto& in : procs[p].prog->code)
                if (in.k == IK::Nba && okS(in.sig)) {
                    nsite[in.sig]++;
                    wproc[in.sig] = wproc[in.sig] == -1 || wproc[in.sig] == p ? p : -2;
                }
        // reads: proc -> signals
        std::map<std::pair<int, int>, long> w;   // (reader, writer) -> weight: reader first saves it
        std::function<void(const E&, std::set<int>&)> refs = [&](const E& e, std::set<int>& out) {
            if (!e) return;
            if (okS(e->sig) && e->op != Op::Const) out.insert(e->sig);
            for (auto& k : e->kids) refs(k, out);
            if (e->at) refs(e->at, out);
        };
        for (int q : dom) {
            std::set<int> rd;
            for (auto& in : procs[q].prog->code) {
                refs(in.e, rd); refs(in.de, rd); refs(in.bidx, rd); refs(in.fd, rd);
                for (auto& a : in.aidx) refs(a, rd);
                for (auto& a : in.args) refs(a, rd);
            }
            for (int sg : rd)
                if (wproc[sg] >= 0 && wproc[sg] != q && domGroup_[q] == domGroup_[wproc[sg]]) w[{q, wproc[sg]}] += nsite[sg];
        }
        if (w.empty()) return;
        static const bool noExact = std::getenv("MSIM_NOEXACTORDER") != nullptr;
        std::map<int, std::vector<int>> byG;   // clock group -> its processes, in order
        std::vector<int> gOrder;
        for (int p : dom) { int g = domGroup_[p]; if (!byG.count(g)) gOrder.push_back(g); byG[g].push_back(p); }
        std::map<int, std::vector<int>> sorted;
        for (auto& [g, ps] : byG) {
            std::set<int> left(ps.begin(), ps.end());
            std::vector<int> head, tail;
            std::map<int, long> outW, inW;
            for (auto& [e, x] : w)
                if (left.count(e.first) && left.count(e.second)) { outW[e.first] += x; inW[e.second] += x; }
            auto drop = [&](int v) {
                left.erase(v);
                for (auto& [e, x] : w) {
                    if (e.first == v && left.count(e.second)) inW[e.second] -= x;
                    if (e.second == v && left.count(e.first)) outW[e.first] -= x;
                }
            };
            std::vector<int> idx(ps);   // original order for ties
            while (!left.empty()) {
                bool moved = false;
                for (int v : idx) if (left.count(v) && outW[v] == 0 && inW[v] != 0) { tail.push_back(v); drop(v); moved = true; }   // sinks: last
                for (int v : idx) if (left.count(v) && inW[v] == 0 && outW[v] != 0) { head.push_back(v); drop(v); moved = true; }   // sources: first
                if (moved) continue;
                int best = -1; long bd = 0;
                for (int v : idx) if (left.count(v) && (best < 0 || outW[v] - inW[v] > bd)) { best = v; bd = outW[v] - inW[v]; }
                if (outW[best] == 0 && inW[best] == 0) {   // no edges left: the rest as found
                    for (int v : idx) if (left.count(v)) head.push_back(v);
                    left.clear();
                    break;
                }
                head.push_back(best);
                drop(best);
            }
            head.insert(head.end(), tail.rbegin(), tail.rend());
            if (!noExact) exactOrder(head, w);
            sorted[g] = head;
        }
        // Each group's processes back into the places that group had.
        std::map<int, size_t> at;
        for (int& p : dom) { int g = domGroup_[p]; p = sorted[g][at[g]++]; }
    }

    // ---- shared comb code (one copy per module, like Verilator) ----
    // The instances of one module get their signals at a constant distance
    // (c1.cpu.x = c0.cpu.x + D): the comb statements of one instance are those
    // of another with every signal number moved by D. refScan finds the
    // signal numbers in generated text (S.sigs[N], R(N, ...), P<N>, ...) and
    // calls f(at, len, N) for each.
    template <class F> static void refScan(const std::string& t, F&& f) {
        static const char* pre[] = {"S.sigs[", "S.waiters[", "S.fwSlow(", "S.fwdv(", "S.fwv(", "S.fw(", "S.drive(",
                                    "S.get(", "S.set(", "S.setOneBit(", "S.setBitsV(", "S.setPart(", "S.poke(", "S.pokeQ(", "S.nbaKeep(", "R(", "int s = ",   // dynTarget
                                    "SIGN(",   // a commit list entry's signal (C1E)
                                    "nb.push_back({"};   // an NBA of a clocked function (emitSeq)
        auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        for (size_t i = 0; i < t.size(); i++) {
            char c = t[i];
            if (c != 'S' && c != 'R' && c != 'P' && c != 'A' && c != 'V' && c != 'H' && c != 'F' && c != 'C' && c != 'i' && c != 'n') continue;
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
    // The instance a signal belongs to, for sharing one copy of the code
    // between instances of a module: the first name component (u3 of
    // u3.cpu.pc), or, when the scope has generate or instance array indexes
    // (soc.row[1].col[2].t.pc), the path down to the last of them, so that
    // the tiles of a mesh are instances of their own. MSIM_SHAREKEY0: always
    // the first component.
    static std::string instKey(const std::string& m) {
        static const bool first = std::getenv("MSIM_SHAREKEY0") != nullptr;
        const size_t d = m.find('.');
        if (d == std::string::npos) return "";
        const size_t last = m.rfind('.');
        size_t end = std::string::npos;
        for (size_t at = 0; !first && at < last;) {
            size_t nx = m.find('.', at);
            if (nx == std::string::npos || nx > last) nx = last;
            if (m.find('[', at) < nx) end = nx;
            at = nx + 1;
        }
        return end == std::string::npos ? m.substr(0, d) : m.substr(0, end);
    }
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
        auto key = [&](size_t j) -> std::string {   // the instance (instKey)
            int sg = ksig(j);
            if (sg < 0) return "";
            return instKey(sigName_[sg]);
        };
        auto rest = [&](size_t j) {
            const std::string& n = sigName_[ksig(j)];
            return n.substr(instKey(n).size() + 1);
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
                // A unit that reads or writes a signal outside its instance (a
                // port bound to sum[i] of the top, say) moves by another
                // distance than the rest: it goes to a group of its own, so
                // that it does not keep the instance's other units from
                // sharing one copy.
                auto inst = [](const std::string& k) { return k.substr(0, k.find('\x01')); };
                auto outside = [&](size_t j, const std::string& k) {
                    for (long long n : refToks(text[j]).num) if (refCls(n, k) == -1) return true;
                    return false;
                };
                for (size_t x = 0; x < L.size(); x++) {
                    std::string k = off || asserts(L[x]) ? "" : key(L[x]);
                    if (!k.empty() && !asserts0(L[x]) && outside(L[x], k)) k += "\x01o";
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
                        for (size_t x = 0; ok && x < g.size(); x++) { ok = refMatch(text[t[x]], text[g[x]], inst(tk0), inst(k), D.data(), set); bad = x; }
                        static const char* dbgS = std::getenv("MSIM_WHYSHARE");
                        if (!ok && dbgS && *dbgS == '2' && g.size() == t.size()) {
                            {
                                std::array<long long, kRefCls> D0{};
                                bool s0[kRefCls] = {};
                                for (size_t x = 0; x <= bad; x++) {
                                    std::array<long long, kRefCls> Dx{};
                                    bool sx[kRefCls] = {};
                                    if (!refMatch(text[t[x]], text[g[x]], inst(tk0), inst(k), Dx.data(), sx)) { std::fprintf(stderr, "   unit %zu does not match alone\n", x); bad = x; break; }
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
                                std::fprintf(stderr, "   %lld (%s, cls %d) vs %lld (%s, cls %d)\n", ta.num[q], ta.num[q] < (long long)sigName_.size() ? sigName_[ta.num[q]].c_str() : "?", refCls(ta.num[q], inst(tk0)),
                                             tb.num[q], tb.num[q] < (long long)sigName_.size() ? sigName_[tb.num[q]].c_str() : "?", refCls(tb.num[q], inst(k)));
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
                            for (size_t x = c; x < tg.size() && x < c + (size_t)kChunk; x++) o << refParam(text[tg[x]], inst(tk));
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
                    if (d != std::string::npos && m[0] != '_' && m[0] != '$') return instKey(m);
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
        std::vector<int> arms;
        std::vector<int> whyL(ns, 0), whyP(ns, -1);   // MSIM_WHYPRIV: the line that kept each candidate in S.sigs
        std::vector<int> owner(ns, -1);   // the branching comb process that writes a candidate
        std::vector<int> tmpOwner(ns, -1);   // the domain process whose temporary a candidate is
        std::vector<char> mine(procs.size(), 0);
        for (int i : dom) mine[i] = 1;
        for (size_t i = 0; i < procs.size(); i++) if (procs[i].combRank >= 0) mine[i] = 1;
        // The elements of arrays a comb process reads at a run-time index
        // (read in place, below): a constant driver may make one private.
        std::vector<char> arrElem(ns, 0);
        if (!std::getenv("MSIM_NOPRIVARR")) {
            std::function<void(const E&)> ae = [&](const E& e) {
                if (!e) return;
                if (e->op == Op::ArrRead && e->arr)
                    for (int64_t k = 0; k < e->arr->count; k++)
                        if (e->arr->base + k >= 0 && (size_t)(e->arr->base + k) < ns) arrElem[(size_t)(e->arr->base + k)] = 1;
                for (auto& k : e->kids) ae(k);
            };
            for (auto& pr : procs)
                if (pr.combRank >= 0 && pr.prog)
                    for (auto& in : pr.prog->code) { ae(in.e); ae(in.bidx); ae(in.de); for (auto& a : in.aidx) ae(a); }
        }
        int refP = -1;
        std::function<void(const E&)> refs = [&](const E& e) {
            if (!e) return;
            if (e->op == Op::Ref && e->sig >= 0 && (size_t)e->sig < ns) (bad[e->sig] = 1, whyL[e->sig] = __LINE__, whyP[e->sig] = refP);
            for (auto& k : e->kids) refs(k);
        };
        for (size_t i = 0; i < procs.size(); i++) {
            // A clock or reset stays in S.sigs when a process waits on it: the
            // domain waits on its wake signals only (wakeEvents).
            for (auto& [sg, ed] : procs[i].seqEv) if ((size_t)sg < ns && (!mine[i] || procs[i].combRank >= 0 || wakeSig(sg))) (bad[sg] = 1, whyL[sg] = __LINE__, whyP[sg] = (int)i);
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
                    } else (bad[in.sig] = 1, whyL[in.sig] = __LINE__);
                }
                continue;
            }
            if (mine[i]) {
                for (auto& in : procs[i].prog->code)
                    if (in.k == IK::Nba && in.sig >= 0 && (size_t)in.sig < ns) {
                        // A part of a narrow one: the commit merges it (commitStmts).
                        if (in.bidx && !in.arr) (bad[in.sig] = 1, whyL[in.sig] = __LINE__);   // bits chosen at run time: through the kernel
                        else if (in.hi < 0 || sigs[in.sig].w > 64 || !std::getenv("MSIM_NOPRIVPART")) cand[in.sig] = 1; else (bad[in.sig] = 1, whyL[in.sig] = __LINE__);
                    } else if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < ns) {
                        // A temporary of this process (a decoder's locals): private
                        // when it is written whole, by this process only. Two
                        // processes would race on it when the domain runs in parallel.
                        // A constant part of one (v[3] = 1) too (MSIM_NOPRIVTMPPART: not).
                        bool whole = in.tmp && (in.hi < 0 || (in.lo >= 0 && in.hi < sigs[in.sig].w && !std::getenv("MSIM_NOPRIVTMPPART"))) &&
                                     in.drv < 0 && !in.arr && in.aidx.empty() && !in.bidx &&
                                     sigs[in.sig].w <= 64 && !std::getenv("MSIM_NOPRIVTMP");
                        if (whole && (tmpOwner[in.sig] < 0 || tmpOwner[in.sig] == (int)i)) { tmpOwner[in.sig] = (int)i; cand[in.sig] = 1; }
                        else bad[in.sig] = 1, whyL[in.sig] = __LINE__;
                    }
                continue;
            }
            if (armShape(procs[i], sigs)) { arms.push_back((int)i); continue; }   // its wait: armChk
            // A constant continuous assign (assign r[0] = '0): the one driver
            // runs once at time 0. An element of an array read in place may
            // be private, starting with the constant (PINIT). Not any other
            // signal: an @* block reading it would be armed through armChk,
            // which then checks a value that never changes on every pass.
            if (!procs[i].seq && procs[i].prog && procs[i].prog->code.size() == 1) {
                const Instr& in = procs[i].prog->code[0];
                if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < ns && arrElem[(size_t)in.sig] && in.hi < 0 && !in.arr && in.aidx.empty() && !in.bidx &&
                    in.drv >= 0 && sigs[in.sig].drivers == 1 && in.s0 != STR_HIZ && in.s1 != STR_HIZ && in.e && in.e->op == Op::Const &&
                    in.e->k.w == sigs[in.sig].w && in.e->k.w <= 64 && in.e->k.hi.empty() && !in.e->k.real && !in.e->k.str &&
                    !in.e->k.x && !in.e->k.z && !std::getenv("MSIM_NOPRIVARR")) {
                    cand[in.sig] = 1;
                    constInit_[in.sig] = in.e->k.v;
                    continue;
                }
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
                    if (in.sig >= 0 && (size_t)in.sig < ns && !plainWrite) (bad[in.sig] = 1, whyL[in.sig] = __LINE__, whyP[in.sig] = (int)i);
                    for (auto& [sg, ed] : in.events) if ((size_t)sg < ns) (bad[sg] = 1, whyL[sg] = __LINE__, whyP[sg] = (int)i);
                    bool compiled = in.k == IK::Assign || in.k == IK::JmpIfNot || in.k == IK::Nba || in.k == IK::WaitDelay;
                    if (!compiled) { refP = (int)i; refs(in.e); refs(in.bidx); refs(in.de); for (auto& a : in.aidx) refs(a); }
                    refP = (int)i;
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
                    if (a->base + k >= 0 && (size_t)(a->base + k) < ns) (bad[(size_t)(a->base + k)] = 1, whyL[(size_t)(a->base + k)] = __LINE__);
            };
            std::function<void(const E&)> ea = [&](const E& e) {
                if (!e) return;
                range(e->arr.get());
                for (auto& k : e->kids) ea(k);
            };
            // A read a[i] in a compiled statement of a comb process
            // reads the private elements in place ((&P<base>)[i], APRV), so
            // such an array (Ibex's register file mirror rf_reg) may be
            // private: the kernel never reaches it. Each element still has to
            // qualify on its own (cand), and an element the generated code
            // cannot read in place trips privHits_ and stays in S.sigs.
            // MSIM_NOPRIVARR=1: every array named stays in S.sigs.
            std::function<void(const E&)> er = [&](const E& e) {
                if (!e) return;
                if (!(e->op == Op::ArrRead && e->arr && e->arr->dims.size() == 1 && !e->arr->dynElem && !e->arr->twoState &&
                      e->arr->elemW >= 1 && e->arr->elemW <= 64))
                    range(e->arr.get());
                for (auto& k : e->kids) er(k);
            };
            const bool privArr = !std::getenv("MSIM_NOPRIVARR");
            std::unordered_set<const Prog*> seen;
            std::function<void(const Prog*, bool)> walk = [&](const Prog* p, bool own) {
                if (!p || !seen.insert(p).second) return;
                for (auto& in : p->code) {
                    range(in.arr.get());
                    const bool rd = own && privArr && (in.k == IK::Assign || in.k == IK::JmpIfNot || in.k == IK::Nba);
                    if (rd) { er(in.e); er(in.bidx); er(in.de); for (auto& a : in.aidx) er(a); }
                    else { ea(in.e); ea(in.bidx); ea(in.de); for (auto& a : in.aidx) ea(a); }
                    for (auto& a : in.args) ea(a);
                    walk(in.callee, false);
                    for (auto* b : in.branches) walk(b, false);
                }
            };
            // Comb processes only: a clocked process of the domain reads a
            // register another one writes from its snapshot (snap_, LS_), and
            // an element read in place would see the new value.
            for (size_t i = 0; i < procs.size(); i++) walk(procs[i].prog, mine[i] && procs[i].combRank >= 0);
        }
        for (int s : noPriv) if (s >= 0 && (size_t)s < ns) bad[(size_t)s] = 1;
        priv_.assign(ns, 0);
        // An element of an unpacked array or a signal in a generate loop
        // (a name with '[') is private too, unless a run-time index reaches
        // it (above). MSIM_NOPRIVBR: the old rule, never private.
        const char* notPriv = std::getenv("MSIM_NOPRIVBR") ? "[$" : "$";
        for (size_t s = 0; s < ns; s++)
            priv_[s] = cand[s] && !bad[s] && sigs[s].w >= 1 && sigs[s].w <= 64 &&
                       sigs[s].name.find_first_of(notPriv) == std::string::npos;
        armK_.assign(procs.size(), -1);
        armIns_.clear();
        armSet_.clear();
        for (int i : arms) {
            std::vector<int> in;
            for (auto& [sg, ed] : procs[i].prog->code[0].events)
                if (sg >= 0 && (size_t)sg < ns && priv_[sg]) in.push_back(sg);
            if (in.empty()) continue;
            armK_[i] = (int)armIns_.size();
            armIns_.push_back(std::move(in));
            armSet_.push_back(&procs[i].prog->code[1]);
        }
        if (std::getenv("MSIM_WHYPRIV"))
            for (size_t s = 0; s < ns; s++)
                if (cand[s] && !priv_[s])
                    std::fprintf(stderr, "not private: %s (%d bits), codegen.hpp line %d%s%s\n", sigs[s].name.c_str(), sigs[s].w, whyL[s],
                                 whyP[s] >= 0 ? ", by a process: " : "",
                                 whyP[s] >= 0 ? (procs[whyP[s]].kind + " line " + std::to_string([&] { for (auto& in : procs[whyP[s]].prog->code) if (in.line) return (int)in.line; return 0; }()) +
                                                 " (process " + std::to_string(whyP[s]) + ")").c_str() : "");
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
    // MSIM_NOTWOTMP=1: a process with temporaries runs its 4-state body in
    // 2-state mode too, and twoNow does not check its temporaries.
    const bool twoTmpOff_ = std::getenv("MSIM_NOTWOTMP") != nullptr;
    const bool c0On_ = !std::getenv("MSIM_NOC0");   // MSIM_NOC0=1: 2-state private NBAs through their slots
    // scanP's back-off while a private signal holds X: at most this many
    // passes without a look. A 4-state comb pass costs far more than a scan,
    // so a long wait after the last X goes kept the design off its 2-state
    // path for up to 255 passes (PicoRV32 x16: 4% of the run). MSIM_SCANCAP=<n>.
    const int scanCap_ = std::getenv("MSIM_SCANCAP") ? std::atoi(std::getenv("MSIM_SCANCAP")) : 7;
    std::string privInit_;       // run(): sets the private signals' first X / Z (PINIT)
    std::vector<int> coneIns_;   // emitCycle: the outside signals of the clock cone (combCone), sorted
    std::map<int, std::vector<int>> snap_;   // emitCycle: per domain process, the registers it reads from a snapshot (sorted)
    const bool noCySplit_ = std::getenv("MSIM_NOCYSPLIT") != nullptr;   // a merged comb cycle in one function
    const bool noShareCy_ = std::getenv("MSIM_NOSHARE") != nullptr;   // the members of a merged cycle: one copy per module
    int cySerial_ = 0;
    std::map<std::string, std::vector<std::tuple<int, std::string, std::string>>> cyTmpl_;   // shape -> (id, instance, text)
    std::set<int> viaK_;         // domain NBA targets also written at a run-time index: all through DNB (see the domain choice)
    bool dAhead_ = false;        // emitCycle: a direct NBA some comb process reads (DAHEAD, see directNba)
    // Of those, the clocks themselves, and their own smaller cone (combConeK):
    // a clock edge runs only that. The others (a debug request, an enable
    // feeding a clock gate) run the whole cone, rarely.
    std::vector<int> coneClkIns_;
    bool coneK_ = false;
    std::string cold(const std::string& stmt) {
        return "[&]() __attribute__((noinline, cold)) { COLD(" + std::to_string(colds_++) + ") " + stmt + " }();";
    }
    // Writes to a private signal: a known value, or any Val.
    std::string pwv(int s, const std::string& u) const {
        return "{ P" + std::to_string(s) + ".v = (" + u + ") & " + M(sigW_[s]) + "; P" + std::to_string(s) + ".x = 0; P" +
               std::to_string(s) + ".z = 0; }";
    }
    // The 2-state copy of a write: twoNow ran only with every scanned private
    // signal known (pclean), and 2-state code writes no unknown bit, so the
    // x and z words are already 0. Scratch and dead signals are not scanned:
    // those keep the full write. MSIM_NOPWV2 turns this off.
    const bool noPwv2_ = std::getenv("MSIM_NOPWV2") != nullptr;
    std::string pwv2(int s, const std::string& u, bool scanned) const {
        if (!scanned || noPwv2_) return pwv(s, u);
        return "{ P" + std::to_string(s) + ".v = (" + u + ") & " + M(sigW_[s]) + "; }";
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
            if (i.hi - i.lo < 64 && i.lo >= 0 && split(i.e, g, u, sl)) {
                if (!noRegSame_ && sigW_[i.sig] <= 64 && i.hi < sigW_[i.sig] && !sigNet_[i.sig] && !clock_[i.sig]) {
                    // A narrow one with no x or z bits: the new word in place, as regWrite.
                    const std::string S_ = std::to_string(i.sig), mp = "(" + M(i.hi - i.lo + 1) + " << " + std::to_string(i.lo) + ")";
                    o << " { Val& q = S.sigs[" << S_ << "].val; if (__builtin_expect(" << (g.empty() ? "true" : g) << " && !(q.x | q.z) && !q.real && !q.str, 1)) {"
                      << " const uint64_t w_ = (q.v & ~" << mp << ") | (((uint64_t)(" << u << ") << " << i.lo << ") & " << mp << ");"
                      << " if (__builtin_expect(q.v != w_, 0)) S.fwv(" << S_ << ", w_); } else " << slowW << " }\n";
                } else
                o << " if (!(" << (g.empty() ? "true" : g) << " && S.samePart(" << i.sig << ", " << i.hi << ", " << i.lo << ", " << u << "))) "
                  << slowW << "\n";
            }
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
        std::unordered_set<const Prog*> seen;
        std::function<void(const Prog*)> outside = [&](const Prog* p) {
            if (!p || !seen.insert(p).second) return;
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
        dAhead_ = false;
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
        std::set<const Instr*> deadIn, deadSeq;
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
                if (e->op == Op::ArrRead && e->arr)   // a[i]: any element (read in place when private)
                    for (int64_t k = 0; k < e->arr->count; k++) { Expr x; x.op = Op::Ref; x.sig = (int)(e->arr->base + k); f(x); }
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
            std::unordered_set<const Prog*> seen;
            std::function<void(const Prog*, bool)> walk = [&](const Prog* p, bool outside) {
                if (!p || !seen.insert(p).second) return;
                for (auto& in : p->code) {
                    if ((in.k == IK::Assign || in.k == IK::Nba) && ok(in.sig)) nw[in.sig]++;
                    if (outside) touchAll(in, live);
                    walk(in.callee, outside);
                    for (auto* b : in.branches) walk(b, outside);
                }
            };
            // An @* arming process (armK_) waits on its block's inputs: those
            // are the block's reads, live only if the block is (its unit).
            // Only when that block's unit reads every one of them (below); one
            // the kernel runs keeps its inputs through this process (clkgate.sv).
            std::vector<char> armUnit(procs.size(), 0);
            if (!std::getenv("MSIM_NODEADSEQ"))
                for (size_t i = 0; i < procs.size() && i < armK_.size(); i++) armUnit[i] = armK_[i] >= 0;
            for (size_t i = 0; i < procs.size(); i++) walk(procs[i].prog, !mine[i] && !armUnit[i]);
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
            // A private wire one straight comb assign writes (a = E) and one
            // comb statement reads, once: the reader computes E in its place
            // and the write goes to combDead below (nothing live reads it),
            // which runs only while a waveform or $monitor watches. Verilator
            // inlines these (Ibex: about 190 of them). Not across a merged
            // comb cycle, not into an asserting process (its memo compares
            // its inputs). MSIM_NOINLINEWIRE=1: never.
            size_t nInline = 0;
            if (!std::getenv("MSIM_NOINLINEWIRE")) {
                std::vector<int> nref(ns, 0);
                std::vector<std::pair<Expr*, int>> where(ns, {nullptr, -1});   // the one read, and its process
                std::function<void(const E&, int)> cnt = [&](const E& e, int pi) {
                    if (!e) return;
                    if (e->op == Op::Ref && ok(e->sig)) { nref[e->sig]++; where[e->sig] = {e.get(), pi}; if (e->at) nref[e->sig] += 2; }
                    if (e->arr) for (int64_t k = 0; k < e->arr->count; k++) if (ok((int)(e->arr->base + k))) nref[(size_t)(e->arr->base + k)] += 2;
                    if (e->at) cnt(e->at, -1);
                    for (auto& k : e->kids) cnt(k, pi);
                };
                std::unordered_set<const Prog*> seenC;
                std::function<void(const Prog*, int)> walkC = [&](const Prog* p, int pi) {
                    if (!p || !seenC.insert(p).second) return;
                    for (auto& in : p->code) {
                        const bool rd = pi >= 0 && (in.k == IK::Assign || in.k == IK::JmpIfNot);
                        cnt(in.e, rd ? pi : -1);
                        cnt(in.bidx, -1); cnt(in.de, -1); cnt(in.fd, -1);
                        for (auto& a : in.args) cnt(a, -1);
                        for (auto& a : in.aidx) cnt(a, -1);
                        // A comb process of the domain runs on every pass: its wait list is unused.
                        if (pi < 0) {
                            for (auto& [sg, ed] : in.events) if (ok(sg)) nref[sg] += 2;
                            if (in.k != IK::Assign && in.k != IK::Nba && ok(in.sig)) nref[in.sig] += 2;
                        } else if (in.k != IK::Assign && in.k != IK::Nba && in.k != IK::WaitEdge && in.k != IK::Jmp && in.k != IK::JmpIfNot && ok(in.sig))
                            nref[in.sig] += 2;
                        if (in.arr) for (int64_t k = 0; k < in.arr->count; k++) if (ok((int)(in.arr->base + k))) nref[(size_t)(in.arr->base + k)] += 2;
                        walkC(in.callee, -1);
                        for (auto* b : in.branches) walkC(b, -1);
                    }
                };
                // Readers: straight or branching comb processes, not asserting, not on a merged cycle.
                std::vector<char> rdOk(procs.size(), 0);
                for (int i : byRank) if (i >= 0 && procs[i].prog->segs.empty() && !combAsserts(*procs[i].prog)) rdOk[(size_t)i] = 1;
                for (size_t i = 0; i < procs.size(); i++) walkC(procs[i].prog, rdOk[i] ? (int)i : -1);
                std::function<bool(const E&, int)> small = [&](const E& e, int d) {
                    if (!e || e->at || d > 24) return false;
                    if (e->op == Op::ArrRead || e->op == Op::SysFn || e->op == Op::CondCov || !e->fmt.empty()) return false;
                    for (auto& k : e->kids) if (!small(k, d + 1)) return false;
                    return true;
                };
                for (int i : byRank) {
                    if (i < 0 || combBranches(*procs[i].prog) || !procs[i].prog->segs.empty() || combAsserts(*procs[i].prog)) continue;
                    for (auto& in : procs[i].prog->code) {
                        if (in.k != IK::Assign) break;
                        const int a = in.sig;
                        if (!ok(a) || !isPriv(a) || keep[a] || nw[a] != 1 || alias[a] >= 0 || nref[a] != 1 || in.hi >= 0 || in.bidx || in.arr ||
                            !in.aidx.empty() || !in.e || in.tmp || in.delay || !in.dly.empty() || sigW_[a] > 64 ||
                            (in.drv >= 0 && (in.s0 == STR_HIZ || in.s1 == STR_HIZ)) || !small(in.e, 0))
                            continue;
                        auto [x, pr] = where[a];
                        if (!x || pr < 0 || pr == i || x->sig != a) continue;
                        if (in.e->op == Op::Ref && in.e->sig == a) continue;
                        // The read becomes E, cut to a's width (an assign keeps the low bits).
                        const bool sg = x->sgn;
                        Expr r;
                        r.op = Op::Resize;
                        r.hi = sigW_[a];
                        r.lo = 0;
                        r.sgn = sg;
                        r.kids = {in.e};
                        *x = std::move(r);
                        nInline++;
                    }
                }
            }
            if (std::getenv("MSIM_WHYCYCLE")) std::fprintf(stderr, "inlined %zu single-use wires\n", nInline);
            // Units: a process with if / case, or one straight assign.
            struct Unit { int proc; const Instr* one; std::vector<int> wr, rd; bool live; bool seq = false; };
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
            {
                for (size_t i = 0; i < procs.size(); i++) {
                    if (!armUnit[i]) continue;
                    const auto& code = procs[i].prog->code;
                    const int flag = code.size() > 1 ? code[1].sig : -1;
                    bool all = ok(flag);
                    if (all) {
                        std::vector<char> rd(ns, 0);
                        bool found = false;
                        for (auto& u : us)
                            if (std::find(u.rd.begin(), u.rd.end(), flag) != u.rd.end()) { found = true; for (int r : u.rd) rd[r] = 1; }
                        all = found;
                        for (auto& in : code)
                            for (auto& [sg, ed] : in.events) all &= !ok(sg) || rd[sg];
                    }
                    if (!all) for (auto& in : code) touchAll(in, live);
                }
            }
            // A clocked process's clock edge is a copy's root (the domain
            // waits on that): a port's private clk_i copy nobody else reads
            // is dead (1125 of them in Ibex, two passes a cycle).
            // A clocked process's write of a private register that nothing
            // live reads (picorv32's dbg_ascii_instr, cached_ascii_instr) is a
            // unit too; a dead one runs only while S.observed (deadSeq).
            // Not in a design with $dumpvars: a dump that starts later would
            // see such a register's stale value (MSIM_NODEADSEQ: never).
            static const std::set<Op> pureOp = {Op::Const, Op::Ref, Op::Not, Op::And, Op::Or, Op::Xor, Op::Add, Op::Sub, Op::Mul, Op::Div, Op::Mod,
                                                Op::Eq, Op::Ne, Op::CaseEq, Op::CaseZEq, Op::CaseXEq, Op::Lt, Op::Le, Op::Gt, Op::Ge, Op::Shl, Op::Shr,
                                                Op::Asr, Op::Resize, Op::LogNot, Op::LogAnd, Op::LogOr, Op::RedAnd, Op::RedOr, Op::RedXor, Op::Slice,
                                                Op::Index, Op::Concat, Op::Mux, Op::WildEq, Op::DynSlice};
            std::function<bool(const E&)> pureE = [&](const E& e) {
                if (!e) return true;
                if (e->at || !pureOp.count(e->op)) return false;
                for (auto& k : e->kids) if (!pureE(k)) return false;
                return true;
            };
            bool seqDead = !std::getenv("MSIM_NODEADSEQ");
            {
                std::unordered_set<const Prog*> seenD;
                std::function<void(const Prog*)> dumps = [&](const Prog* p) {
                    if (!p || !seqDead || !seenD.insert(p).second) return;
                    for (auto& in : p->code) {
                        if (in.k == IK::Dump || in.k == IK::Vpi) { seqDead = false; return; }
                        dumps(in.callee);
                        for (auto* b : in.branches) dumps(b);
                    }
                };
                for (auto& pr : procs) dumps(pr.prog);
            }
            for (int n : dom)
                for (auto& in : procs[n].prog->code) {
                    if (seqDead && (in.k == IK::Assign || in.k == IK::Nba) && ok(in.sig) && isPriv(in.sig) && !keep[in.sig] && !in.arr &&
                        in.aidx.empty() && in.args.empty() && in.drv < 0 && !in.delay && in.dly.empty() && !in.nbaLater && in.gate < 0 &&
                        in.dyn < 0 && !in.udp && pureE(in.e) && pureE(in.bidx) && pureE(in.de)) {
                        us.push_back({n, &in, {in.sig}, {}, false, true});
                        exprs(in, [&](Expr& x) { if (ok(x.sig)) us.back().rd.push_back(x.sig); });
                        continue;
                    }
                    if (in.k == IK::WaitEdge && !std::getenv("MSIM_NOCLKALIAS")) {
                        exprs(in, [&](Expr& x) { if (ok(x.sig)) live[x.sig] = 1; });
                        if (ok(in.sig)) live[alias[in.sig] >= 0 && !wakeSig(in.sig) ? root(in.sig) : in.sig] = 1;
                        for (auto& [sg, ed] : in.events) if (ok(sg)) live[alias[sg] >= 0 && !wakeSig(sg) ? root(sg) : sg] = 1;
                    } else touchAll(in, live);
                }
            for (bool ch = true; ch;) {
                ch = false;
                for (auto& u : us) {
                    if (!u.live) for (int w : u.wr) if (live[w]) { u.live = true; break; }
                    if (u.live) for (int r : u.rd) if (!live[r]) { live[r] = 1; ch = true; }
                }
            }
            size_t nDead = 0, nDeadSeq = 0;
            for (auto& u : us) {
                if (u.live) continue;
                if (u.seq) { deadSeq.insert(u.one); nDeadSeq++; continue; }
                nDead++;
                if (const char* wc = std::getenv("MSIM_WHYCYCLE"); wc && *wc == '2')
                    for (int w : u.wr) std::fprintf(stderr, "dead comb unit (process %d) writes %s\n", u.proc, sigName_[w].c_str());
                if (u.one) deadIn.insert(u.one); else deadProc[u.proc] = 1;
                for (int w : u.wr) deadSig[w] = 1;
            }
            for (int n : dom)
                for (auto& in : procs[n].prog->code) if (in.k == IK::Nba && ok(in.sig)) deadSig[in.sig] = 0;
            if (std::getenv("MSIM_WHYCYCLE"))
                std::fprintf(stderr, "cycle domain: %zu copies read at their source, %zu of %zu comb units have no reader, %zu clocked writes nothing reads\n",
                             nAlias, nDead, us.size(), nDeadSeq);
        }
        struct Slot { int sig, hi, lo; };
        std::vector<Slot> slots;
        bool dynArrNba = false;   // a domain process writes an array element chosen at run time
        std::vector<int> slotProc;   // the domain process of each slot
        std::vector<char> slotListed;   // its 2-state value may go onto C0L / C1L
        std::ostringstream fns, fns2;
        // 2-state mode: when no signal the domain reads holds X or Z, every
        // statement takes its 2-state path, so a second copy without the
        // checks runs instead (dsN2, combQ). Only when every statement has one.
        bool two = twoState;
        std::set<int> ext;   // signals in S.sigs that the domain reads
        // Temporaries of a domain process: only it reads them. twoNow checks
        // them like any other input, since a read before the first write of
        // a pass sees the last pass's value (MSIM_NOTWOTMP: the old way, its
        // 4-state body in both modes and the temporaries left out).
        std::set<int> tmpSig;
        for (int n : dom)
            for (auto& in : procs[n].prog->code) if (in.k == IK::Assign && in.tmp) tmpSig.insert(in.sig);
        auto noteRefs = [&](const E& e) {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->op == Op::Ref && !isPriv(x->sig) && (!twoTmpOff_ || !tmpSig.count(x->sig))) ext.insert(x->sig);
                for (auto& k : x->kids) f(k);
            };
            f(e);
        };
        // Direct NBAs: a narrow private register that only NBAs of one domain
        // process write, that no other process reads, and that its writer
        // does not read after the first write, is stored at the NBA itself:
        // nobody can see the store before the commit would have made it.
        // A comb process of the domain may read it too: combAll runs after
        // fcommit, and a pass that would run between the clocked processes
        // and fcommit (an outside input changed in the same time step) waits
        // for the one after it (DAHEAD), as it would see the stores early.
        // Not a register that feeds a clock: combCone runs at any time.
        std::vector<char> direct(sigW_.size(), 0);
        // partNba: a register the commit lists (C0L, C1L) must not take, as
        // they are stored before the slots: some NBA to it writes an element,
        // a part of a private or wide one, has no 2-state value, or is in
        // another process.
        std::vector<char> partNba(sigW_.size(), 0);
        {
            std::vector<int> by(sigW_.size(), -1);
            for (int n : dom)
                for (auto& in : procs[n].prog->code) {
                    if (in.k != IK::Nba || in.sig < 0 || (size_t)in.sig >= partNba.size()) continue;
                    std::string g, u, sl;
                    const bool partOk = in.hi < 0 || (sigW_[in.sig] <= 64 && !sigNet_[in.sig]);   // C0L and C1L take a mask
                    if (!partOk || in.arr || in.bidx || !in.aidx.empty() || (by[in.sig] >= 0 && by[in.sig] != n) || !split(in.e, g, u, sl))
                        partNba[in.sig] = 1;
                    by[in.sig] = n;
                }
        }
        if (!std::getenv("MSIM_NODIRECT")) {
            const size_t ns = sigW_.size();
            std::vector<int> wproc(ns, -1), firstW(ns, -1);
            std::vector<char> bad(ns, 0), inDom(procs.size(), 0), inByRank(procs.size(), 0), selfRd(ns, 0);
            for (int n : dom) inDom[n] = 1;
            for (int r : byRank) if (r >= 0) inByRank[r] = 1;
            const char* dbgW0 = std::getenv("MSIM_WHYDIRECT"); const bool dbgW = dbgW0 && *dbgW0 == '2';
            for (size_t p = 0; p < procs.size(); p++) {
                if (!procs[p].prog) continue;
                const auto& code = procs[p].prog->code;
                for (size_t pc = 0; pc < code.size(); pc++) {
                    const Instr& i = code[pc];
                    if (i.sig < 0 || (size_t)i.sig >= ns) continue;
                    if (i.k == IK::WaitEdge) continue;   // a read (its edge signal): below
                    const bool ok = inDom[p] && i.k == IK::Nba && i.hi < 0 && !i.arr && i.aidx.empty() && !i.bidx && !i.de &&
                                    !i.delay && i.dly.empty();
                    // A declaration initializer (logic a = 0) runs before any clock.
                    if (!ok && procs[p].kind == "init" && procs[p].combRank < 0 && !procs[p].seq && i.k == IK::Assign) continue;
                    if (!ok) { if (dbgW && !bad[i.sig]) std::fprintf(stderr, "WD write %s k=%d proc=%zu %s dom=%d\n", dbgName(i.sig), (int)i.k, p, procs[p].kind.c_str(), (int)inDom[p]); bad[i.sig] = 1; continue; }
                    if (wproc[i.sig] < 0) { wproc[i.sig] = (int)p; firstW[i.sig] = (int)pc; }
                    else if (wproc[i.sig] != (int)p) bad[i.sig] = 1;
                }
            }
            // Reads. In the writer: bad only where some write can come before
            // the read (the code is a DAG of forward jumps: one pass in pc
            // order, the set of candidates written so far on some path).
            const char* dbgD0 = std::getenv("MSIM_WHYDIRECT"); const bool dbgD = dbgD0 && *dbgD0 == '2';
            // The signals some comb process turns into a clock, at any depth.
            std::vector<char> feedsClk(ns, 0), combRead(ns, 0);
            for (size_t x = 0; x < ns && x < clock_.size(); x++) feedsClk[x] = clock_[x];
            for (bool again = true; again;) {
                again = false;
                for (int r : byRank) {
                    if (r < 0) continue;
                    bool feeds = false;
                    for (auto& in : procs[r].prog->code) {
                        if (in.k == IK::WaitEdge) break;
                        if (in.k == IK::Assign && in.sig >= 0 && (size_t)in.sig < ns) feeds |= feedsClk[in.sig] != 0;
                    }
                    if (!feeds) continue;
                    for (int x : procs[r].combIns)
                        if (x >= 0 && (size_t)x < ns && !feedsClk[x]) { feedsClk[x] = 1; again = true; }
                }
            }
            auto cand = [&](int sg) { return sg >= 0 && (size_t)sg < ns && wproc[sg] >= 0 && !bad[sg]; };
            std::vector<int> domPos(procs.size(), -1);
            for (size_t j = 0; j < run_.size(); j++) domPos[run_[j]] = (int)j;
            static const bool noEarly = std::getenv("MSIM_NOORDER") != nullptr;
            std::vector<int> cidx(ns, -1);
            for (size_t p = 0; p < procs.size(); p++) {
                if (!procs[p].prog) continue;
                const auto& code = procs[p].prog->code;
                std::vector<int> mine;   // candidates this process writes
                for (auto& i : code)
                    if (i.k == IK::Nba && cand(i.sig) && wproc[i.sig] == (int)p && cidx[i.sig] < 0) { cidx[i.sig] = (int)mine.size(); mine.push_back(i.sig); }
                const size_t nw = (mine.size() + 63) / 64;
                bool fwd = true;
                for (size_t pc = 0; pc < code.size(); pc++)
                    if ((code[pc].k == IK::Jmp || code[pc].k == IK::JmpIfNot) && code[pc].target <= pc &&
                        !(code[pc].k == IK::Jmp && pc + 1 == code.size()))   // the jump back to the wait ends an activation
                        fwd = false;
                std::vector<std::vector<uint64_t>> win(code.size() + 1, std::vector<uint64_t>(nw, 0));
                for (size_t pc = 0; pc < code.size(); pc++) {
                    const Instr& i = code[pc];
                    auto rd = [&](int sg) {
                        if (!cand(sg)) return;
                        if (wproc[sg] != (int)p && procs[p].combRank >= 0 && inByRank[p] && !feedsClk[sg]) { combRead[sg] = 1; return; }
                        // An @* arming process (armChk compares the inputs; it arms
                        // its block once, which runs in combAll after fcommit either way).
                        if (wproc[sg] != (int)p && p < armK_.size() && armK_[p] >= 0) return;
                        // Another process of the domain that runs before the writer, in
                        // the same clock group (orderDomain), sees the old value too.
                        if (wproc[sg] != (int)p && inDom[p] && domPos[p] >= 0 && domPos[p] < domPos[wproc[sg]] &&
                            domGroup_[p] == domGroup_[wproc[sg]] && !noEarly)
                            return;
                        if (wproc[sg] != (int)p) { if (!bad[sg] && dbgD) std::fprintf(stderr, "WD other %s w=%d r=%zu %s rank=%d seq=%d dom=%d line=%d\n", dbgName(sg), wproc[sg], p, procs[p].kind.c_str(), procs[p].combRank, (int)procs[p].seq, (int)inDom[p], procs[p].prog->code.empty() ? 0 : procs[p].prog->code[0].line); bad[sg] = 1; return; }
                        const int c = cidx[sg];
                        if (!fwd || (win[pc][(size_t)c / 64] >> (c % 64) & 1)) {
                            if (!bad[sg] && !selfRd[sg] && dbgD) std::fprintf(stderr, "WD self %s fwd=%d\n", dbgName(sg), (int)fwd);
                            selfRd[sg] = 1;   // read after a write of its own: a snapshot (below), or the commit list
                        }
                    };
                    std::function<void(const E&)> f = [&](const E& x) {
                        if (!x) return;
                        if (x->op == Op::Ref) rd(x->sig);
                        if (x->sig >= 0 && x->op != Op::Ref && x->op != Op::Const) rd(x->sig);
                        if (x->op == Op::ArrRead && x->arr)   // a[i]: any element
                            for (int64_t k = 0; k < x->arr->count; k++) rd((int)(x->arr->base + k));
                        for (auto& k : x->kids) f(k);
                        if (x->at) f(x->at);
                    };
                    f(i.e); f(i.de); f(i.bidx); f(i.fd);
                    for (auto& a : i.aidx) f(a);
                    for (auto& a : i.args) f(a);
                    for (auto& ev : i.events) rd(ev.first);
                    if (i.k == IK::WaitEdge) rd(i.sig);
                    if (nw == 0 || !fwd) continue;
                    std::vector<uint64_t> out = win[pc];
                    if (i.k == IK::Nba && cand(i.sig) && cidx[i.sig] >= 0 && wproc[i.sig] == (int)p)
                        out[(size_t)cidx[i.sig] / 64] |= 1ULL << (cidx[i.sig] % 64);
                    auto join = [&](size_t t) { if (t < win.size()) for (size_t w = 0; w < nw; w++) win[t][w] |= out[w]; };
                    if (i.k == IK::Jmp) { if (i.target > pc) join(i.target); }
                    else {
                        if (i.k == IK::JmpIfNot) { join(i.target); if (i.xt != Instr::NOX) join(i.xt); }
                        join(pc + 1);
                    }
                }
                for (int sg : mine) cidx[sg] = -1;
            }
            // A register its writer reads after writing it is stored at the
            // NBA too, when every read of it in that process can take a copy
            // made as the process starts (snap_): the process sees the old
            // value, as it would through the commit. Only in a process whose
            // reads are all in its generated text (NBAs, jumps, temporaries).
            static const bool noSnap = std::getenv("MSIM_NOSNAP") != nullptr;
            std::vector<char> plain(procs.size(), 0);
            for (int n : dom) {
                bool ok = true;
                for (auto& in : procs[n].prog->code)
                    ok &= in.k == IK::Nba || in.k == IK::Jmp || in.k == IK::JmpIfNot || in.k == IK::WaitEdge || (in.k == IK::Assign && in.tmp);
                plain[n] = ok;
            }
            size_t nd = 0, nc = 0, nsnap = 0;
            for (size_t sg = 0; sg < ns; sg++)
                if (wproc[sg] >= 0 && !bad[sg] && isPriv((int)sg) && sigW_[sg] <= 64 && !clock_[sg] && !sigNet_[sg] &&
                    (!selfRd[sg] || (!noSnap && plain[wproc[sg]]))) {
                    direct[sg] = 1;
                    nd++;
                    if (selfRd[sg]) { snap_[wproc[sg]].push_back((int)sg); nsnap++; }
                    if (combRead[sg]) { dAhead_ = true; nc++; }
                }
            if (std::getenv("MSIM_WHYDIRECT")) std::fprintf(stderr, "direct NBAs: %zu signals, %zu read by comb, %zu through a snapshot\n", nd, nc, nsnap);
        }
        std::vector<std::string> dsB4(procs.size()), dsB2(procs.size());
        std::vector<char> dsTmp(procs.size());
        for (int n : dom) {
            const Prog& p = *procs[n].prog;
            size_t end = p.code.size() - 1;
            auto L = [](size_t pc) { return "d_" + std::to_string(pc); };
            std::ostringstream allF, allF2;   // the bodies; shared or emitted below
            bool hasTmp = false;
            // Snapshots (snap_): the process reads LS_<k>, a copy of the
            // register made as it starts; its NBAs store the register itself.
            const std::vector<int>* sn = snap_.count(n) ? &snap_[n] : nullptr;
            auto snapRen = [&](const std::string& t) {
                if (!sn) return t;
                std::string o;
                size_t last = 0;
                refScan(t, [&](size_t i0, size_t, size_t e, long long v, int kind) {
                    if (kind != 1) return;
                    auto it = std::lower_bound(sn->begin(), sn->end(), (int)v);
                    if (it == sn->end() || *it != (int)v) return;
                    o.append(t, last, i0 - last);
                    o += "LS_" + std::to_string(it - sn->begin());
                    last = e;
                });
                o.append(t, last, std::string::npos);
                return o;
            };
            if (sn)
                for (size_t k = 0; k < sn->size(); k++) {
                    const std::string ks = std::to_string(k), ps = "P" + std::to_string((*sn)[k]);
                    allF << " const auto LS_" << ks << " = " << ps << ";";
                    allF2 << " const PSig LS_" << ks << "{" << ps << ".v, 0, 0};";   // the 2-state body: known bits
                }
            if (sn) { allF << "\n"; allF2 << "\n"; }
            std::vector<char> inChainD(p.code.size() + 1, 0);
            for (size_t pc = 1; pc < end; pc++) {
                const Instr& i = p.code[pc];
                std::ostringstream fns, fns2;
                bool renamed = false;   // the text below already reads the snapshots
                fns << L(pc) << ":";
                fns2 << L(pc) << ":";
                noteRefs(i.e);
                switch (i.k) {
                    case IK::Nba: {
                        if (i.arr || i.bidx) {   // an element or bits chosen at run time: through the kernel (see above)
                            for (auto& a : i.aidx) noteRefs(a);
                            std::string t = dynTarget(i, fast(i.e), " DNB->push_back({s, v, hi, lo});");
                            fns << t;
                            fns2 << t;
                            dynArrNba = true;
                            break;
                        }
                        if (i.arr || !i.aidx.empty() || i.bidx) throw CodegenUnsupported("array or variable-index write");
                        if (viaK_.count(i.sig)) {   // also written at a run-time index: the same list, in order
                            std::string t = " DNB->push_back({" + std::to_string(i.sig) + ", " + fast(i.e) + ", " + std::to_string(i.hi) + ", " + std::to_string(i.lo) + "});\n";
                            fns << t;
                            fns2 << t;
                            break;
                        }
                        if (i.hi < 0 && direct[i.sig]) {   // stored here: nobody reads it before the commit (above)
                            std::string g, u, sl;
                            const std::string slow = pw(i.sig, "@V@") + " if (P" + std::to_string(i.sig) + ".x | P" + std::to_string(i.sig) + ".z) pclean = false;";
                            auto at = [&](const std::string& v) { std::string t = slow; t.replace(t.find("@V@"), 3, snapRen(v)); return t; };
                            renamed = sn != nullptr;
                            if (i.hi < 0 && split(i.e, g, u, sl)) {
                                g = snapRen(g);
                                u = snapRen(u);
                                fns2 << " " << pwv2(i.sig, u, !scratch(i.sig) && !deadSig[i.sig]) << "\n";
                                if (g.empty()) fns << " " << pwv(i.sig, u) << "\n";
                                else fns << " if (__builtin_expect(" << g << ", 1)) " << pwv(i.sig, u) << " else " << cold(at(sl)) << "\n";
                            } else {
                                fns << " " << at(fast(i.e)) << "\n";
                                fns2 << " " << at(fast(i.e)) << "\n";
                            }
                            break;
                        }
                        int k = (int)slots.size();
                        slots.push_back({i.sig, i.hi, i.lo});
                        slotProc.push_back(n);
                        std::string g, u, sl;
                        bool sp = (i.hi < 0 || (sigW_[i.sig] <= 64 && !sigNet_[i.sig])) && split(i.e, g, u, sl);   // a narrow part: see the commit
                        // Through the arrays (VA[k], not V<k>): shared code moves k.
                        const std::string K = std::to_string(k), V = " VA[" + K + "]", H = " HA[" + K + "]", F = " FA[" + K + "]";
                        fns << " CLP(" << k << ");";
                        // A private register's 2-state value: onto the C0 list (its
                        // address and value), which fcommit stores in order (see C0L).
                        // A register's 2-state value onto a commit list instead (its
                        // address and value, stored in order by fcommit; see C0L):
                        // a private one (C0), or a narrow S.sigs one (C1). Not for a
                        // register some NBA writes a part of: the order would change.
                        const bool c0 = sp && (i.hi < 0 || sigW_[i.sig] <= 64) && c0On_ && !partNba[i.sig] && isPriv(i.sig);
                        const bool c1 = sp && c0On_ && !partNba[i.sig] && !isPriv(i.sig) && sigW_[i.sig] <= 64 &&
                                        !sigNet_[i.sig] && !clock_[i.sig];
                        // C1L: the bits written (all, or the part's) and their value there.
                        const std::string c1m = i.hi < 0 ? M(sigW_[i.sig]) : "(" + M(i.hi - i.lo + 1) + " << " + std::to_string(i.lo) + ")";
                        const std::string c1v = i.hi < 0 ? "(" + u + ")" : "((uint64_t)(" + u + ") << " + std::to_string(i.lo) + ")";
                        slotListed.push_back(c0 || c1);
                        if (c0)
                            fns2 << " if (C0OK) { C0L[C0N].p = &P" << i.sig << "; C0L[C0N].m = " << (i.hi < 0 ? M(sigW_[i.sig]) : c1m) << "; C0L[C0N++].v = " << c1v << " & " << (i.hi < 0 ? M(sigW_[i.sig]) : c1m)
                                 << "; } else { COVF = true; CLP(" << k << ");" << V << " = " << u << ";" << H << " = 1; }\n";
                        else if (c1)
                            fns2 << " if (C1OK) { C1L[C1N].s = SIGN(" << i.sig << "); C1L[C1N].m = " << c1m << "; C1L[C1N++].v = " << c1v << " & " << c1m
                                 << "; } else { COVF = true; CLP(" << k << ");" << V << " = " << u << ";" << H << " = 1; }\n";
                        else fns2 << " CLP(" << k << ");";
                        if (c0 || c1) {}
                        else if (sp) fns2 << V << " = " << u << ";" << H << " = 1;\n";
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
                        if (!inChainD[pc]) fns2 << caseDispatch(p, pc, inChainD, L);   // a case: one switch (see caseDispatch)
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
                        if (!i.tmp) throw CodegenUnsupported("blocking assign in a clocked process (line " + std::to_string(i.line) + ")");
                        for (auto& a : i.aidx) noteRefs(a);
                        // The 2-state copy writes the value alone: twoNow checks the
                        // temporaries too (a read before the first write of a pass
                        // sees the last pass's value), so every input is known.
                        if (i.bidx) {
                            noteRefs(i.bidx);
                            std::string t = dynTarget(i, fast(i.e), " S.poke(s, v, hi, lo);", true);
                            fns << t;
                            fns2 << t;
                        } else if (isPriv(i.sig) && i.hi >= 0) {   // a constant part of a private temporary
                            const std::string P_ = "P" + std::to_string(i.sig), W_ = std::to_string(sigW_[i.sig]);
                            const std::string H_ = std::to_string(i.hi), L_ = std::to_string(i.lo);
                            std::string g, u, sl;
                            std::string slow = " PW(" + P_ + ", " + W_ + ", v_insert(PR(" + P_ + ", " + W_ + ", false), " + fast(i.e) + ", " + H_ + ", " + L_ + "));\n";
                            if (split(i.e, g, u, sl)) {
                                std::string fastW = " { const uint64_t m = " + M(i.hi - i.lo + 1) + " << " + L_ + "; " + P_ + ".v = (" + P_ + ".v & ~m) | (((uint64_t)(" + u + ") << " + L_ + ") & m); }";
                                // 4-state: known value and known bits there; otherwise the general insert
                                if (g.empty()) fns << " { const uint64_t m = " << M(i.hi - i.lo + 1) << " << " << L_ << "; " << P_ << ".v = (" << P_ << ".v & ~m) | (((uint64_t)(" << u << ") << " << L_ << ") & m); " << P_ << ".x &= ~m; " << P_ << ".z &= ~m; }\n";
                                else fns << " if (__builtin_expect(" << g << ", 1)) { const uint64_t m = " << M(i.hi - i.lo + 1) << " << " << L_ << "; " << P_ << ".v = (" << P_ << ".v & ~m) | (((uint64_t)(" << u << ") << " << L_ << ") & m); " << P_ << ".x &= ~m; " << P_ << ".z &= ~m; } else " << cold(slow) << "\n";
                                fns2 << fastW << "\n";
                            } else {
                                fns << slow;
                                fns2 << slow;
                            }
                        } else if (isPriv(i.sig) && i.hi < 0) {   // a private temporary (privateSigs): no write through the kernel
                            std::string g, u, sl;
                            if (!split(i.e, g, u, sl)) {
                                fns << " " << pw(i.sig, sl) << "\n";
                                fns2 << " " << pw(i.sig, sl) << "\n";
                            } else {
                                if (g.empty()) fns << " " << pwv(i.sig, u) << "\n";
                                else fns << " if (__builtin_expect(" << g << ", 1)) " << pwv(i.sig, u) << " else " << cold(pw(i.sig, sl)) << "\n";
                                fns2 << " " << pwv2(i.sig, u, !scratch(i.sig) && !deadSig[i.sig]) << "\n";
                            }
                        } else {
                            std::string t = assign(i);
                            fns << t;
                            fns2 << t;
                        }
                        hasTmp = twoTmpOff_;
                        break;
                    case IK::Display: {   // a testbench's clocked block (seqShape sys): printed where it runs
                        sysArgs_.push_back(i.args);
                        std::vector<int> rs;
                        std::function<void(const E&)> col = [&](const E& e) {
                            if (!e) return;
                            if (e->op == Op::Ref && isPriv(e->sig)) rs.push_back(e->sig);
                            for (auto& k : e->kids) col(k);
                        };
                        for (auto& a : i.args) col(a);
                        std::string t;
                        for (int r : rs) t += " S.sigs[" + std::to_string(r) + "].val = PR(P" + std::to_string(r) + ", " + std::to_string(sigW_[r]) + ", false);";
                        t += " S.printSys(" + quote(i.fmt) + ", A" + std::to_string(sysArgs_.size() - 1) + (i.nonl ? ", false" : "") + ");\n";
                        fns << t;
                        fns2 << t;
                        break;
                    }
                    case IK::Finish: {   // $fatal: its exit status
                        std::string t = " S.finished = true;" + (i.hi > 0 ? " if (S.exitCode < " + std::to_string(i.hi) + ") S.exitCode = " + std::to_string(i.hi) + ";" : std::string()) +
                                        " goto " + L(end) + ";\n";
                        fns << t;
                        fns2 << t;
                        break;
                    }
                    default: throw CodegenUnsupported("instruction " + std::to_string((int)i.k) + " in a clocked process (line " + std::to_string(i.line) + ")");
                }
                std::string t4 = fns.str(), t2 = fns2.str();
                if (deadSeq.count(&i)) {   // nothing live reads it: only for a waveform or $monitor
                    const std::string lb = L(pc) + ":";
                    auto guard = [&](std::string& t) {
                        if (t.compare(0, lb.size(), lb) != 0) return;
                        while (!t.empty() && t.back() == '\n') t.pop_back();
                        t = lb + " if (__builtin_expect(S.observed, 0)) {" + t.substr(lb.size()) + "\n }\n";
                    };
                    guard(t4);
                    guard(t2);
                }
                if (renamed || !sn) { allF << t4; allF2 << t2; }
                else { allF << snapRen(t4); allF2 << snapRen(t2); }
            }
            allF << L(end) << ": return;\n";
            allF2 << L(end) << ": return;\n";
            dsB4[n] = allF.str();
            dsB2[n] = allF2.str();
            dsTmp[n] = hasTmp;
        }
        emitDomainFns(fns, fns2, dom, procs, dsB4, dsB2, dsTmp);
        slotSig_.clear();
        for (auto& sl : slots) slotSig_.push_back({sl.sig, sl.hi, sl.lo});
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
        o << "static uint32_t CL[" << std::max<size_t>(slots.size(), 1) << "]; static const uint32_t CLCAP = " << std::max<size_t>(slots.size(), 1)
          << "; static uint32_t CN; static bool CLOVF; static bool CLON = true;\n"
             "static bool DAHEAD;   // the clocked processes ran, fcommit has not (see directNba)\n"
             "#define CLP(k) do { if (CLON) { if (CN < CLCAP) CL[CN++] = (k); else CLOVF = true; } } while (0)\n";
        // The 2-state NBAs to private registers, in the order they ran: one
        // store each in fcommit, no slot (PicoRV32 x16 commits ~1300 a cycle).
        // Off for the parallel path (CLON) and when full: the slot instead.
        // (No sizes in the macros: they go to every part, and a part must not
        // change when a process elsewhere adds slots.)
        o << "struct C0E { PSig* p; uint64_t v, m; };\nstruct C1E { int s; uint64_t v, m; };   // by number: a frame may move S.sigs\n"
             // Thread-local: in the parallel path each group fills its own
             // lists (gBegin), committed by the group's cmG.
             "static C0E C0B[" << std::max<size_t>(2 * slots.size(), 1) << "]; static C1E C1B[" << std::max<size_t>(2 * slots.size(), 1)
          << "];\nstatic const uint32_t C0CAPG = " << std::max<size_t>(2 * slots.size(), 1) << ";\n"
             "static __thread C0E* C0L = C0B;\nstatic __thread C1E* C1L = C1B;\n"
             "static __thread uint32_t C0N;\nstatic __thread uint32_t C1N;\nstatic __thread uint32_t C0CAP = C0CAPG;\nstatic __thread bool COVF;\n"
             "#define C0OK (__builtin_expect(C0N < C0CAP, 1))\n"
             "#define C1OK (__builtin_expect(C1N < C0CAP, 1))\n"
             // pclean: every scanned private signal is known, and the list's
             // targets (clocked NBA targets, all scanned) with it. pclean only
             // turns true in scanP, after the pass, so the commit stores the
             // value word alone. MSIM_NOPWV2 keeps the full commit.
          << (noPwv2_ ? "#define C0TWO false\n" : "#define C0TWO (pclean.load(std::memory_order_relaxed))\n");
        std::vector<int> privs;
        for (size_t s = 0; s < priv_.size(); s++) if (priv_[s]) privs.push_back((int)s);
        // Only when something watches every change (a VCD opened at run time).
        // One table and a loop: unrolled, the copies were 450 KB of code
        // and the slowest function to compile on Ibex.
        o << "struct SynRow { PSig* p; int s, w; };\nstatic const SynRow SYN[] = {";
        size_t nsyn = 0;
        for (int s : privs)
            if (origOf(s) == s) o << (nsyn++ ? ", " : "") << "{&P" << s << ", " << s << ", " << sigW_[s] << "}";
        if (!nsyn) o << "{nullptr, 0, 0}";
        o << "};\n";
        std::string loop = nsyn ? "    for (auto& e : SYN) " : "    if (false) for (auto& e : SYN) ";
        std::string syncPriv_ = loop + "S.fw(e.s, Val{e.p->v, e.p->x, e.w, e.p->z, false});\n";
        for (auto& [sg, ps] : pieces_) syncPriv_ += "    S.fw(" + std::to_string(sg) + ", " + ex(piecesOf(sg, sigW_[sg] - 1, 0)) + ");\n";
        o << "static void combDead();\nstatic void syncP() {\n    combDead();\n" << syncPriv_ << "}\n";
        // An arming process whose private inputs changed since it began to
        // wait sets its flag, as it would have on waking (armShape).
        if (!armIns_.empty()) {
            o << "static void armSet(uint32_t k) {\n    switch (k) {\n";
            for (size_t k = 0; k < armSet_.size(); k++) o << "    case " << k << ":" << assign(*armSet_[k]) << "    break;\n";
            o << "    }\n    S.markFast();   // the block's comb pass runs again, now armed\n}\n";
            o << "static void armChk() {\n"
                 "    for (uint32_t k = 0; k < " << armIns_.size() << "; k++) {\n"
                 "        if (ARMST[k] != 1) continue;\n"
                 "        for (uint32_t j = ARMOFF[k]; j < ARMOFF[k + 1]; j++) {\n"
                 "            const PSig& a = PA[ARMIN[j]]; const PSig& b = ARMSNAP[j];\n"
                 "            if (a.v != b.v || a.x != b.x || a.z != b.z) { ARMST[k] = 2; ARMLEFT--; armSet(k); break; }\n"
                 "        }\n    }\n}\n";
        }
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
                // MSIM_WHYBAIL=1 at -compile: simv names the first statements that abandon a 2-state pass.
                std::string why = std::getenv("MSIM_WHYBAIL") && (size_t)in.sig < sigName_.size()
                                      ? "{ static int n = 0; if (n++ < 2) std::fprintf(stderr, \"2-state comb pass abandoned: %s\\n\", " + quote(sigName_[in.sig]) + "); } "
                                      : "";
                return "    {" + fassign(in) + "    if (__builtin_expect(" + unk + " != 0, 0)) { " + why + "@BAIL@ } }\n";
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
                size_t lbase = 0;   // a merged block's member: its labels count from its start
                auto L4 = [&](size_t pc) { return "cb" + std::to_string(i) + "_" + std::to_string(pc - lbase); };
                auto L2 = [&](size_t pc) { return "cq" + std::to_string(i) + "_" + std::to_string(pc - lbase); };
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
                // The 2-state copy compares the inputs only where an assertion
                // fails (lazyMz): see below. Not with the blocks of a merged
                // cycle in functions of their own (their text is shared).
                const bool lazyMz = combAsserts(cp) && !noLazyMz_ && (cp.segs.empty() || noCySplit_);
                std::string mzEnd;
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
                    // CIVA: 1 seen once, 2 every x / z word below is 0. In
                    // 2-state mode (every input known) only the values compare.
                    std::string I_ = std::to_string(i), chk = "    { bool ch = !CIVA[" + I_ + "]; CIVA[" + I_ + "] = 1;\n";
                    std::string chk2 = "    { bool ch = !CIVA[" + I_ + "];\n", xz2;
                    size_t nw = 0;
                    auto CI = [&](size_t k) { return "CIP[" + I_ + "][" + std::to_string(k) + "]"; };
                    for (int sg : ins) {
                        std::string G_ = std::to_string(sg);
                        if (isPriv(sg) || sigW_[sg] <= 64) {
                            const std::string src = isPriv(sg) ? "P" + G_ : "S.sigs[" + G_ + "].val";
                            for (const char* f : {".v", ".x", ".z"})
                                chk += "      ch |= UPD(" + CI(nw++) + ", " + src + f + ");\n";
                            chk2 += "      ch |= UPD(" + CI(nw - 3) + ", " + src + ".v);\n";
                            xz2 += " o |= " + CI(nw - 2) + " | " + CI(nw - 1) + "; " + CI(nw - 2) + " = 0; " + CI(nw - 1) + " = 0;";
                        } else {
                            o << "static Val CW" << I_ << "_" << G_ << ";\n";
                            chk += "      ch |= UPDV(CW" + I_ + "_" + G_ + ", S.sigs[" + G_ + "].val);\n";
                            chk2 += "      ch |= UPDV(CW" + I_ + "_" + G_ + ", S.sigs[" + G_ + "].val);\n";
                        }
                        if (!isPriv(sg)) ext.insert(sg);   // a private one is in P, and S.sigs keeps its first X
                    }
                    o << "static uint64_t CI" << I_ << "[" << std::max<size_t>(nw, 1) << "]; static const bool CIR" << I_ << " = (CIP[" << I_
                      << "] = CI" << I_ << ", true);\n";
                    chk += "      if (!ch) goto @END@; }\n    CC" + I_ + "->epoch++;\n";
                    chk2 += "      if (__builtin_expect(CIVA[" + I_ + "] != 3, 0)) { uint64_t o = 0;" + xz2 + " ch |= o != 0; CIVA[" + I_ + "] = 3; }\n"
                            "      if (!ch) goto @END@; }\n    CC" + I_ + "->epoch++;\n";
                    auto withEnd = [&](std::string c, const std::string& lab) {
                        c.replace(c.find("@END@"), 5, lab);
                        return c;
                    };
                    c4 += withEnd(chk, L4(cp.code.size() - 2));
                    if (lazyMz) {
                        // The body is a function of the inputs, so running it
                        // again changes nothing; only a report may not repeat
                        // while they are the same. An assertion that fails
                        // compares them with those of the last report (and
                        // takes them); a pass that reaches none forgets them.
                        // That is the eager compare's outcome: a failure with
                        // the inputs of the last pass that ran fails again.
                        std::string lz = chk;
                        lz.replace(lz.find("    { "), 6, "[&]() __attribute__((noinline, cold)) -> bool { ");
                        lz.replace(lz.find("      if (!ch) goto @END@; }\n    CC" + I_ + "->epoch++;\n"), std::string("      if (!ch) goto @END@; }\n    CC" + I_ + "->epoch++;\n").size(),
                                   "      if (ch) CC" + I_ + "->epoch++; return ch; }()");
                        mzLazy_ = lz;
                        mzVar_ = "mz" + I_ + "_";
                        c2 += "    int8_t " + mzVar_ + " = -1;\n";
                        mzEnd = " if (" + mzVar_ + " < 0 && CIVA[" + I_ + "]) { CIVA[" + I_ + "] = 0; CC" + I_ + "->epoch++; }";
                    } else c2 += withEnd(chk2, L2(cp.code.size() - 2));
                    serialOnly = true;
                }
                unitIns.emplace_back();
                unitProc.push_back(i);
                unitConds.emplace_back();
                size_t pc = 0;
                std::vector<char> inChain(cp.code.size() + 1, 0);
                // bitRun2 in a block: a run never starts or ends inside another
                // statement's jump target or a segment's edge.
                std::vector<char> tgtB(cp.code.size() + 2, 0), runDone(cp.code.size() + 1, 0);
                for (auto& x : cp.code)
                    if (x.k == IK::Jmp || x.k == IK::JmpIfNot) { if (x.target < tgtB.size()) tgtB[x.target] = 1; if (x.xt != Instr::NOX && x.xt < tgtB.size()) tgtB[x.xt] = 1; }
                for (auto& sgm : cp.segs) { if (sgm.first < tgtB.size()) tgtB[sgm.first] = 1; if (sgm.second < tgtB.size()) tgtB[sgm.second] = 1; }
                // One instruction of the block into c4 / c2; false at its end (the wait).
                auto one = [&](size_t pc, std::string& c4, std::string& c2) {
                    const Instr& in = cp.code[pc];
                    if (in.k == IK::JmpIfNot) {
                        // A case chain: one switch, then its tests as they were (not reached).
                        const std::string pre2 = L2(pc) + ":" + (inChain[pc] ? "" : caseDispatch(cp, pc, inChain, L2));
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
                        if (sp) c2 += pre2 + " if (!(" + u + ")) goto " + L2(in.target) + ";\n";
                        else c2 += pre2 + jmpIfNot(in, sl, "goto " + L2(in.target) + ";", in.xt == Instr::NOX ? "" : "goto " + L2(in.xt) + ";");
                        return true;
                    }
                    if (in.k == IK::Jmp) {
                        c4 += L4(pc) + ": goto " + L4(in.target) + ";\n";
                        c2 += L2(pc) + ": goto " + L2(in.target) + ";\n";
                        return true;
                    }
                    if (in.k == IK::Assert) {
                        std::string a = combAssert(i, in);
                        c4 += L4(pc) + ":" + a;
                        if (lazyMz) c2 += L2(pc) + ": if (" + mzVar_ + " < 0) " + mzVar_ + " = " + mzLazy_ + "; if (" + mzVar_ + ")" + a;
                        else c2 += L2(pc) + ":" + a;
                        return true;
                    }
                    if (in.k != IK::Assign) return false;
                    noteRefs(in.e);
                    noteRefs(in.bidx);
                    combIns.push_back(&in);
                    unitIns.back().push_back(&in);
                    c4 += L4(pc) + ":" + fassign(in);
                    if (runDone[pc]) { c2 += L2(pc) + ": ;\n"; return true; }   // written by its run's first statement
                    if (!noBitRun_) {
                        std::vector<const Instr*> cand{&in};
                        for (size_t q = pc + 1; q < cp.code.size() && !tgtB[q] && cp.code[q].k == IK::Assign && cp.code[q].sig == in.sig; q++) cand.push_back(&cp.code[q]);
                        std::string run;
                        size_t n = 0;
                        if (cand.size() >= 3 && bitRun2(cand, 0, n, run)) {
                            for (size_t q = 1; q < n; q++) runDone[pc + q] = 1;
                            c2 += L2(pc) + ":" + run;
                            return true;
                        }
                    }
                    c2 += L2(pc) + ":" + stmt2(in);
                    return true;
                };
                // A merged cycle of blocks (mergeCombCycles): each block in a
                // function of its own, so that the compiler never sees one
                // function of thousands of statements. The 2-state copy says
                // when it gives up (@BAIL@ of the caller).
                size_t sg = 0;
                for (; pc < cp.code.size(); pc++) {
                    while (sg < cp.segs.size() && cp.segs[sg].first < pc) sg++;
                    if (sg < cp.segs.size() && cp.segs[sg].first == pc && cp.segs[sg].second > pc && !noCySplit_) {
                        const size_t e = cp.segs[sg].second;
                        std::string h4, h2;
                        lbase = pc;
                        for (size_t q = pc; q < e; q++) one(q, h4, h2);
                        h4 += L4(e) + ": ;\n";
                        h2 += L2(e) + ": return false;\n";
                        lbase = 0;
                        for (size_t at; (at = h2.find("@BAIL@")) != std::string::npos;) h2.replace(at, 6, "return true;");
                        // Members of other instances of one module share one copy
                        // (the same text but for signal numbers moved by D, as shareChunks).
                        std::string key;
                        for (size_t q = pc; q < e && key.empty(); q++) {
                            const Instr& in = cp.code[q];
                            if (in.k == IK::Assign && !in.tmp && in.sig >= 0 && (size_t)in.sig < sigName_.size()) {
                                const std::string& m = sigName_[in.sig];
                                if (!m.empty() && m[0] != '_' && m[0] != '$') key = instKey(m);
                            }
                        }
                        const std::string both = h4 + "\x01" + h2;
                        int id = -1;
                        std::array<long long, kRefCls> D{};
                        if (!key.empty() && !noShareCy_) {
                            RefToks rt = refToks(both);
                            std::string shape;
                            for (auto& pcs : rt.piece) shape += pcs + "\x02";
                            auto& lst = cyTmpl_[shape];
                            for (auto& [tid, tk, tt] : lst) {
                                std::array<long long, kRefCls> Dx{};
                                bool set[kRefCls] = {};
                                if (refMatch(tt, both, tk, key, Dx.data(), set)) { id = tid; D = Dx; break; }
                            }
                            if (id < 0) {
                                id = cySerial_++;
                                lst.push_back({id, key, both});
                                o << "static void cyAT" << id << "(" << dParams() << ") {\n    const bool obs = S.observed; (void)obs;" << dUnused() << "\n"
                                  << refParam(h4, key) << "}\n";
                                o << "static bool cyBT" << id << "(" << dParams() << ") {\n    const bool obs = S.observed; (void)obs;" << dUnused() << "\n"
                                  << refParam(h2, key) << "}\n";
                            } else shareSaved_ += h4.size();
                        }
                        std::string f;
                        if (id >= 0) {
                            std::string args;
                            for (int c = 0; c < kRefCls; c++) args += (c ? ", " : "") + std::to_string(D[c]);
                            c4 += L4(pc) + ": cyAT" + std::to_string(id) + "(" + args + "); goto " + L4(e) + ";\n";
                            c2 += L2(pc) + ": if (cyBT" + std::to_string(id) + "(" + args + ")) { @BAIL@ } goto " + L2(e) + ";\n";
                        } else {
                            f = std::to_string(i) + "_" + std::to_string(sg);
                            o << "static void cyA" << f << "() {\n    const bool obs = S.observed; (void)obs;\n" << h4 << "}\n";
                            o << "static bool cyB" << f << "() {\n    const bool obs = S.observed; (void)obs;\n" << h2 << "}\n";
                            c4 += L4(pc) + ": cyA" + f + "(); goto " + L4(e) + ";\n";
                            c2 += L2(pc) + ": if (cyB" + f + "()) { @BAIL@ } goto " + L2(e) + ";\n";
                        }
                        shareTotal_ += h4.size();
                        pc = e - 1;
                        continue;
                    }
                    if (!one(pc, c4, c2)) break;
                }
                combStmts.push_back(c4 + L4(pc) + ": ;\n    }\n");
                combStmts2.push_back(c2 + L2(pc) + ": ;" + mzEnd + "\n    }\n");
                continue;
            }
            std::vector<const Instr*> line;   // the live assigns, in order (bitRun2)
            for (const Instr& in : cp.code) {
                if (in.k != IK::Assign) break;
                if (!deadIn.count(&in)) line.push_back(&in);
            }
            size_t runLeft = 0;
            for (const Instr& in : cp.code) {
                if (in.k != IK::Assign) break;
                if (deadIn.count(&in)) { deadStmts.push_back("   " + fassign(in)); continue; }
                combStmts.push_back("   " + fassign(in));
                combIns.push_back(&in);
                unitIns.push_back({&in});
                unitProc.push_back(i);
                unitConds.emplace_back();
                noteRefs(in.e);
                if (runLeft) { runLeft--; combStmts2.push_back("    ;\n"); continue; }   // written by the run's first unit
                std::string run;
                size_t n = 0;
                const size_t at = (size_t)(std::find(line.begin(), line.end(), &in) - line.begin());
                if (bitRun2(line, at, n, run)) { runLeft = n - 1; combStmts2.push_back(run); continue; }
                combStmts2.push_back(stmt2(in));
            }
        }
        // Generate loops (assign y[k] = x[N-1-k]) are one comb process per
        // bit: consecutive single-assign units writing parts of one private
        // register are one word too (bitRun2), written by the run's first unit.
        if (!noBitRun_) {
            for (size_t u = 0; u < combStmts2.size();) {
                auto single = [&](size_t k) {
                    return k < unitIns.size() && unitIns[k].size() == 1 && unitConds[k].empty() && combStmts2[k] == stmt2(*unitIns[k][0]);
                };
                if (!single(u)) { u++; continue; }
                std::vector<const Instr*> run;
                for (size_t k = u; k < combStmts2.size() && single(k) && unitIns[k][0]->sig == unitIns[u][0]->sig; k++) run.push_back(unitIns[k][0]);
                std::string txt;
                size_t n = 0;
                if (run.size() >= 3 && bitRun2(run, 0, n, txt)) {
                    combStmts2[u] = txt;
                    for (size_t k = 1; k < n; k++) combStmts2[u + k] = "    ;\n";
                    u += n;
                } else u++;
            }
        }
        o << "static __thread std::vector<Sim::NbaItem>* DNB;\n";
        o << fns.str();
        if (two) o << fns2.str();
        // 2-state mode holds when every private signal is known (pclean, kept
        // by the writes) and so is every S.sigs signal the domain reads.
        // A table and a loop: one expression of 4000 terms (16 Ibex cores)
        // took the compiler 12 GB and 80 s.
        o << "static bool twoNow() {\n";
        if (two) {
            // The x words of the narrow signals by address (taken again whenever S.sigs moves:
            // a frame makes signals), then the wide ones limb by limb:
            // a loop over Val and its limbs cost about 14 instructions a
            // signal, 30% of Ibex's run time once it was all 2-state.
            o << "    static const int T[] = {-1";
            for (int e : ext) if (!scratch(e) && sigW_[e] <= 64) o << ", " << e;
            o << "};\n    static const int TW[] = {-1";
            for (int e : ext) if (!scratch(e) && sigW_[e] > 64) o << ", " << e;
            o << "};\n    constexpr size_t NT = sizeof T / sizeof T[0] - 1;\n"
                 "    static const uint64_t* TX[NT ? NT : 1];\n"
                 "    static const Sig* at = nullptr;   // S.sigs moves when a frame makes signals\n"
                 "    if (at != S.sigs.data()) { for (size_t k = 0; k < NT; k++) TX[k] = &S.sigs[T[k + 1]].val.x; at = S.sigs.data(); }\n"
                 "    if (!pclean) return false;\n    uint64_t x = 0;\n";
            // Up to a few thousand narrow signals: one load each at a constant
            // offset from S.sigs.data(), unrolled (the pointer table took two
            // loads and the loop: 7% of PicoRV32 x16). More: the table loop.
            size_t nNarrow = 0;
            for (int e : ext) if (!scratch(e) && sigW_[e] <= 64) nNarrow++;
            if (nNarrow <= 4096) {
                o << "    const Sig* const B = S.sigs.data();\n";
                size_t c = 0;
                for (int e : ext)
                    if (!scratch(e) && sigW_[e] <= 64) o << (c++ % 8 ? " " : "    ") << "x |= B[" << e << "].val.x;" << (c % 8 ? "" : "\n");
                if (c % 8) o << "\n";
            } else
                o << "    for (size_t k = 0; k < NT; k++) x |= *TX[k];\n";
            o <<
                 "    for (size_t k = 1; k < sizeof TW / sizeof TW[0]; k++) { const Val& v = S.sigs[TW[k]].val; x |= v.x; for (auto& h : v.hi) x |= h.x; }\n"
                 "    return x == 0;\n";   // every limb: fx reads up to 64 bits of a wider signal
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
        o << "    pclean = x == 0;\n    skip = x ? (skip < " << scanCap_ << " ? skip * 2 + 1 : " << scanCap_ << ") : 0;\n    wait = skip;\n}\n";
        // In run order (orderDomain): readers of another process's registers first.
        o << "static void (*const DS[])() = {";
        for (size_t j = 0; j < run_.size(); j++) o << (j ? ", " : "") << "ds" << run_[j];
        o << "};\n";
        if (two) {
            o << "static void (*const DS2[])() = {";
            for (size_t j = 0; j < run_.size(); j++) o << (j ? ", " : "") << "ds" << run_[j] << "_2";
            o << "};\n";
        }
        // Several clock groups: dseq is registered once per group and runs
        // that group's processes; one marker NBA per slot commits them all.
        const bool multi = domGroups_ > 1;
        if (multi) {
            // The processes of each group in run order, as direct calls in one
            // function per group: no table loop and no indirect call per
            // process (Ibex: 79 processes a cycle).
            std::vector<std::vector<size_t>> byG(domGroups_);
            for (size_t j = 0; j < run_.size(); j++) byG[domGroup_[run_[j]]].push_back(j);
            o << "static bool dpend = false;\n";
            for (int g = 0; g < domGroups_; g++) {
                o << "static void dsQ" << g << "() {";
                for (size_t j : byG[g]) o << " ds" << run_[j] << "();";
                o << " }\n";
                if (two) {
                    o << "static void dsQ2_" << g << "() {";
                    for (size_t j : byG[g]) o << " ds" << run_[j] << "_2();";
                    o << " }\n";
                }
            }
            o << "static void (*const DSQ[])() = {";
            for (int g = 0; g < domGroups_; g++) o << (g ? ", " : "") << "dsQ" << g;
            o << "};\n";
            if (two) {
                o << "static void (*const DSQ2[])() = {";
                for (int g = 0; g < domGroups_; g++) o << (g ? ", " : "") << "dsQ2_" << g;
                o << "};\n";
            }
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
        const bool whyPar = std::getenv("MSIM_WHYPAR") != nullptr;
        int whyN = 0;
        auto noPar = [&](const char* why, int sg) {
            allPriv = false;
            if (whyPar && whyN++ < 8) std::fprintf(stderr, "   %s %s\n", why, sg >= 0 && (size_t)sg < sigName_.size() ? sigName_[sg].c_str() : "");
        };
        if (whyPar && !two) std::fprintf(stderr, "   not 2-state\n");
        // A public register the domain writes: allowed when a commit can be
        // a plain store (at most 64 bits, not a net or a clock). Whether the
        // store is safe on a worker thread is decided at run time (locFast):
        // otherwise it waits for the end of the pass (DEF).
        const bool noLocal = std::getenv("MSIM_NOPARPUB") != nullptr;
        auto localOk = [&](int sg) { return !noLocal && sigW_[sg] <= 64 && !sigNet_[sg] && !((size_t)sg < clock_.size() && clock_[sg]); };
        for (auto& sl : slots) { written[sl.sig] = 1; if (!isPriv(sl.sig) && !localOk(sl.sig)) noPar("public slot (wide, net or clock)", sl.sig); }
        // NBAs to array elements chosen at run time: every element is a target.
        std::map<int, std::vector<std::pair<int, int>>> dynArr;   // process -> (first element, count)
        for (int n : dom)
            for (auto& in : procs[n].prog->code)
                if (in.k == IK::Nba && (in.arr || in.bidx)) {
                    const int b0 = in.arr ? (int)in.arr->base : in.sig, cnt = in.arr ? (int)in.arr->count : 1;
                    dynArr[n].push_back({b0, cnt});
                    for (int64_t k = 0; k < cnt; k++) {
                        int sg = (int)(b0 + k);
                        if ((size_t)sg >= written.size()) continue;
                        written[sg] = 1;
                        if (isPriv(sg) || !localOk(sg)) noPar("array NBA target", sg);
                    }
                }
        // A copy of a clock down the hierarchy (a port) stays in S.sigs and
        // is shared by every island: the clock cone (combCone) computes it
        // before the domain runs, so the parallel groups leave its unit out.
        auto clkCopy = [&](const Instr* in) { return !isPriv(in->sig) && (size_t)in->sig < clock_.size() && clock_[in->sig]; };
        std::vector<char> skipUnit(unitIns.size(), 0);
        for (size_t j = 0; j < unitIns.size(); j++) {
            size_t c = 0;
            for (auto* in : unitIns[j]) c += clkCopy(in);
            if (c && c == unitIns[j].size()) skipUnit[j] = 1;
            else if (c) noPar("unit writes a clock copy and more", unitIns[j][0]->sig);
        }
        // A unit whose outputs are public but read by nothing in the domain
        // (a port bound to the testbench's sum[i]) runs after the parallel
        // pass, on one thread (cmPub): S.sigs writes with waiters are not
        // thread-safe.
        std::vector<char> domRead(sigW_.size(), 0);
        {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->sig >= 0 && (size_t)x->sig < domRead.size()) domRead[x->sig] = 1;
                if (x->arr) for (int64_t k = 0; k < x->arr->count; k++) if ((size_t)(x->arr->base + k) < domRead.size()) domRead[x->arr->base + k] = 1;
                for (auto& k : x->kids) f(k);
                if (x->at) f(x->at);
            };
            for (int n : dom) for (auto& in : procs[n].prog->code) { f(in.e); f(in.bidx); for (auto& a : in.aidx) f(a); for (auto& a : in.args) f(a); }
            for (size_t j = 0; j < unitIns.size(); j++) {
                for (auto* in : unitIns[j]) { f(in->e); f(in->bidx); for (auto& a : in->aidx) f(a); }
                for (auto& e : unitConds[j]) f(e);
            }
        }
        std::vector<char> pubUnit(unitIns.size(), 0);
        for (size_t j = 0; j < unitIns.size(); j++) {
            if (skipUnit[j] || unitIns[j].empty()) continue;
            bool pub = false, ok = true;
            for (auto* in : unitIns[j]) if (!isPriv(in->sig)) { pub = true; ok &= !domRead[in->sig]; }
            if (pub && ok) pubUnit[j] = 1;
        }
        for (size_t j = 0; j < unitIns.size(); j++)
            for (auto* in : unitIns[j]) {
                if (skipUnit[j]) continue;
                written[in->sig] = 1;
                if (!isPriv(in->sig) && !pubUnit[j]) noPar("public comb", in->sig);
            }
        for (auto* in : combIns) if (!clkCopy(in)) written[in->sig] = 1;
        auto refsOf = [&](const E& e, std::vector<int>& out) {
            std::function<void(const E&)> f = [&](const E& x) {
                if (!x) return;
                if (x->op == Op::Ref && x->sig >= 0 && (size_t)x->sig < written.size() && written[x->sig]) out.push_back(x->sig);
                if (x->arr)
                    for (int64_t k = 0; k < x->arr->count; k++) {
                        size_t sg = (size_t)(x->arr->base + k);
                        if (sg < written.size() && written[sg]) out.push_back((int)sg);
                    }
                for (auto& k : x->kids) f(k);
                if (x->at) f(x->at);
            };
            f(e);
        };
        std::map<int, int> procSig;    // domain process -> one of its targets
        for (size_t k = 0; k < slots.size(); k++) {
            int n = slotProc[k];
            if (!procSig.count(n)) procSig[n] = slots[k].sig;
            unite(procSig[n], slots[k].sig);
        }
        // Direct NBAs (stored at the NBA, no slot) are the process's targets too.
        for (int n : dom)
            for (auto& in : procs[n].prog->code)
                if (in.k == IK::Nba && !in.arr && in.sig >= 0 && (size_t)in.sig < written.size()) {
                    written[in.sig] = 1;
                    if (!procSig.count(n)) procSig[n] = in.sig;
                    unite(procSig[n], in.sig);
                }
        for (auto& [n, v] : dynArr)
            for (auto [b, c] : v)
                for (int k = 0; k < c; k++) {
                    if (!procSig.count(n)) procSig[n] = b;
                    unite(procSig[n], b + k);
                }
        for (int n : dom) {
            bool writes = false;   // only jumps: nothing to run (an always block with empty branches)
            for (auto& in : procs[n].prog->code) writes |= !(in.k == IK::Jmp || in.k == IK::JmpIfNot || in.k == IK::WaitEdge);
            if (!procSig.count(n) && !writes) continue;
            if (!procSig.count(n)) {
                if (whyPar) { std::string k; for (auto& in : procs[n].prog->code) k += std::to_string((int)in.k) + (in.arr ? "a" : "") + (in.sig >= 0 && (size_t)in.sig < sigName_.size() ? ":" + sigName_[in.sig] : "") + " "; std::fprintf(stderr, "   proc %d: %s\n", n, k.substr(0, 300).c_str()); }
                noPar("process with no target", -1); continue;
            }
            std::vector<int> rs;
            for (auto& in : procs[n].prog->code) { refsOf(in.e, rs); refsOf(in.bidx, rs); for (auto& a : in.aidx) refsOf(a, rs); for (auto& a : in.args) refsOf(a, rs); }
            for (int r : rs) unite(procSig[n], r);
        }
        for (size_t j = 0; j < unitIns.size(); j++) {
            int s0 = unitIns[j].empty() ? -1 : unitIns[j][0]->sig;
            if (s0 < 0 || skipUnit[j] || pubUnit[j]) continue;
            std::vector<int> rs;
            for (auto* in : unitIns[j]) { refsOf(in->e, rs); refsOf(in->bidx, rs); for (auto& a : in->aidx) refsOf(a, rs); rs.push_back(in->sig); }
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
        for (auto* in : combIns) if (!clkCopy(in)) islandOf(in->sig);
        // Number the islands by their lowest signal (an island's root):
        // neighbours in the design, so a group's signals share cache lines
        // with its own and not with another thread's.
        { int k = 0; for (auto& [r, id] : islandId) id = k++; }
        int nIsl = (int)islandId.size();
        const int kGroups = 64;
        // NBAs to array elements chosen at run time go to a list per group
        // (GS[g].gnb), handed to the kernel in group order after the parallel pass.
        bool sysTask = false;   // $display / $finish in a clocked process: their order is the process order
        for (int n : dom) for (auto& in : procs[n].prog->code) sysTask |= in.k == IK::Display || in.k == IK::Finish;
        bool par = allPriv && nIsl >= 32 && !serialOnly && !multi && !sysTask;
        if (std::getenv("MSIM_WHYPAR")) {
            std::fprintf(stderr, "parallel domain: %s (all private %d, islands %d, serial %d, groups %d, dynamic array NBA %d, system task %d)\n",
                         par ? "yes" : "no", (int)allPriv, nIsl, (int)serialOnly, (int)multi, (int)dynArrNba, (int)sysTask);
        }
        int G = par ? std::min(nIsl, kGroups) : 0;
        // Islands in order cut into G groups of about equal work (code size),
        // so each thread's share of the groups is the same every pass and
        // stays in its cache (the pool's static split).
        std::vector<int> islGroup(nIsl, 0);
        if (G > 0) {
            std::vector<double> wt(nIsl, 0);
            for (int n : dom) if (procSig.count(n)) wt[islandOf(procSig[n])] += (double)procs[n].prog->code.size();
            for (size_t j = 0; j < unitIns.size(); j++)
                if (!unitIns[j].empty() && !skipUnit[j] && !pubUnit[j]) wt[islandOf(unitIns[j][0]->sig)] += (double)unitIns[j].size();
            double tot = 0;
            for (double w : wt) tot += w;
            double acc = 0;
            for (int i = 0; i < nIsl; i++) {
                islGroup[i] = std::min(G - 1, tot > 0 ? (int)(acc * G / tot) : (int)((int64_t)i * G / nIsl));
                acc += wt[i];
            }
            // An island heavier than 1/G of the work leaves the groups it
            // spans empty (16 PicoRV32 cores: 48 of 64), each still a call
            // and a commit per pass: number the used ones only.
            std::vector<int> used(G, -1);
            int k = 0;
            for (int i = 0; i < nIsl; i++) {
                if (used[islGroup[i]] < 0) used[islGroup[i]] = k++;
                islGroup[i] = used[islGroup[i]];
            }
            G = std::max(k, 1);
        }
        auto groupOf = [&](int sig) { return islGroup[islandOf(sig)]; };
        if (par) {
            std::vector<std::vector<int>> gProcs(G);
            for (int n : run_) if (procSig.count(n)) gProcs[groupOf(procSig[n])].push_back(n);
            for (int g = 0; g < G; g++) {
                o << "static void dsG" << g << "(bool two) {\n    if (two) {";
                for (int n : gProcs[g]) o << " ds" << n << "_2();";
                o << " } else {";
                for (int n : gProcs[g]) o << " ds" << n << "();";
                o << " }\n}\n";
            }
            o << "static void cmPub(bool);\n";
            for (int g = 0; g < G; g++) o << "static void cmG" << g << "(bool);\n";
            o << "static void (*const CMG[])(bool) = {";
            for (int g = 0; g < G; g++) o << (g ? ", " : "") << "cmG" << g;
            o << "};\n";
            o << "static void (*const DSG[])(bool) = {";
            for (int g = 0; g < G; g++) o << (g ? ", " : "") << "dsG" << g;
            o << "};\n";
        }
        if (par) {
            // Per group, one cache line apart: the groups' threads write these.
            o << "struct alignas(64) GSt { std::vector<Sim::NbaItem> gnb, def; std::vector<C0E> c0; std::vector<C1E> c1; uint32_t c0n = 0, c1n = 0; bool ovf = false; };\n"
                 "static GSt GS[" << G << "];   // gnb: the group's NBAs to array elements\n"
                 // A public signal the pass writes: a plain store on a worker
                 // thread only when nothing but the domain reacts to it (fw's
                 // fast path). Otherwise its write waits in the group's DEF
                 // list for the end of the pass, which then settles combAll.

                 "static __thread std::vector<Sim::NbaItem>* DEF;\n"
                 "inline bool locFast(int s) { return S.quiet[s] && S.waiters[s].empty(); }\n"
                 "inline void LFW(int s, const Val& x) { if (!DEF || locFast(s)) S.fw(s, x); else DEF->push_back({s, x, -1, 0}); }\n"
                 // The group's run-time array NBAs, in order, before its commits.
                 "static void gnbApply(std::vector<Sim::NbaItem>& v) {\n"
                 "    size_t k = 0;\n"
                 "    for (; k < v.size(); k++) {\n"
                 "        auto& n = v[k];\n"
                 "        if (DEF && !locFast(n.s)) break;\n"
                 "        S.fw(n.s, n.hi >= 0 ? v_insert(S.get(n.s), n.v, n.hi, n.lo) : n.v);\n"
                 "    }\n"
                 "    for (; k < v.size(); k++) DEF->push_back(v[k]);\n"
                 "    v.clear();\n"
                 "}\n"
                 "static void combAll(const void*);\n";
            // Each group's commit lists (C0L, C1L while its processes run).
            std::vector<size_t> gcap(G, 1);
            for (size_t k = 0; k < slots.size(); k++) gcap[groupOf(slots[k].sig)] += 2;
            o << "static const uint32_t GCAP[] = {";
            for (int g = 0; g < G; g++) o << (g ? ", " : "") << gcap[g];
            o << "};\n"
                 "static inline void gBegin(size_t g) {\n"
                 "    if (GS[g].c0.empty()) { GS[g].c0.resize(GCAP[g]); GS[g].c1.resize(GCAP[g]); }\n"
                 "    C0L = GS[g].c0.data(); C1L = GS[g].c1.data(); C0N = C1N = 0; C0CAP = GCAP[g]; COVF = false;\n"
                 "}\n"
                 // A 4-state pass or a full list leaves values in the slots (H).
                 "static inline void gEnd(size_t g, bool t) {\n"
                 "    GS[g].c0n = C0N; GS[g].c1n = C1N; GS[g].ovf = COVF || !t;\n"
                 "    C0L = C0B; C1L = C1B; C0N = C1N = 0; C0CAP = C0CAPG; COVF = false;\n"
                 "}\n"
                 "static void c1Apply(const C1E* L, uint32_t n) {\n"
                 "    const bool obs = S.observed;\n"
                 "    for (uint32_t j = 0; j < n; j++) {\n"
                 "        const int s = L[j].s; Sig& g = S.sigs[s]; const uint64_t m = L[j].m; Val& q = g.val;\n"
                 "        if (DEF && !locFast(s)) { const int lo = __builtin_ctzll(m), hi = 63 - __builtin_clzll(m); DEF->push_back({s, make(L[j].v >> lo, 0, hi - lo + 1), hi, lo}); continue; }\n"
                 "        if (__builtin_expect(!obs && S.waiters[s].empty(), 1)) { q.v = (q.v & ~m) | L[j].v; q.x &= ~m; q.z &= ~m; q.real = false; q.str = false; }\n"
                 "        else S.fwSlow(s, Val{(q.v & ~m) | L[j].v, q.x & ~m, g.w, q.z & ~m});\n"
                 "    }\n"
                 "}\n"
                 "static bool defApply() {\n"
                 "    bool any = false;\n"
                 "    for (auto& gs : GS) { auto& v = gs.def; for (auto& n : v) S.fw(n.s, n.hi >= 0 ? v_insert(S.get(n.s), n.v, n.hi, n.lo) : n.v); any |= !v.empty(); v.clear(); }\n"
                 "    return any;\n"
                 "}\n";
        }
        o << "static void dseq(const void* ctx, std::vector<Sim::NbaItem>& nb) {\n    (void)ctx;\n    DNB = &nb;\n";
        if (dAhead_) o << "    DAHEAD = true;\n";
        if (par) o << "    CLON = false;\n";
        if (par)
            // Nothing else runs before the NBA region (S.fuseOk()): each group
            // computes, commits and settles its islands in one parallel pass.
            o << "    if (S.fuseOk()) {\n"
                 "        const bool t = twoNow();\n"
                 // A waveform or coverage sees every change in order: one thread.
                 "        const bool tune = !S.observed && !g_cov;\n"
                 "        const int use = tune ? S.domainTune[0].pick(S.threads) : 1;\n"
                 "        static const bool pst = std::getenv(\"MSIM_PARSTAT\") != nullptr; static std::atomic<uint64_t> pIn{0}; static uint64_t pWall = 0, pN = 0;\n"
                 "        if (pst && pN == 0) std::atexit([] { std::fprintf(stderr, \"parstat: %llu passes, wall %.3f s, in groups %.3f s\\n\", (unsigned long long)pN, pWall / 1e9, pIn.load() / 1e9); });\n"
                 "        const auto pt0 = pst ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
                 "        S.parallelFor(" << G << ", [t](size_t g) { const auto g0 = pst ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}; DNB = &GS[g].gnb; DEF = Sim::tlInPar ? &GS[g].def : nullptr; gBegin(g); DSG[g](t); gEnd(g, t); gnbApply(GS[g].gnb); CMG[g](t); if (pst) pIn += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g0).count(); }, use);\n"
                 "        if (pst) { pN++; pWall += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pt0).count(); }\n"
                 "        if (tune) S.domainTune[0].done(S.threads);\n"
                 "        DNB = &nb; DEF = nullptr;\n"
                 "        const bool late = defApply();\n"
                 "        cmPub(t);\n"
                 "        if (late) { DAHEAD = false; combAll(nullptr); }\n"
                 "        if (!t || cdirty.exchange(false)) scanP();\n"
                 "        DAHEAD = false;\n"
                 // As fcommit: the @* blocks whose inputs the pass changed run now.
              << (armIns_.empty() ? "" : "        if (ARMLEFT) armChk();\n") <<
                 "        if (S.observed) syncP();\n"
                 "        return;\n    }\n"
                 "    { const bool t = twoNow(); const bool tune = !S.observed && !g_cov; const int use = tune ? S.domainTune[1].pick(S.threads) : 1;\n"
                 "      S.parallelFor(" << G << ", [t](size_t g) { DNB = &GS[g].gnb; gBegin(g); DSG[g](t); gEnd(g, t); }, use); if (tune) S.domainTune[1].done(S.threads);\n"
                 "      DNB = &nb;\n      for (auto& gs : GS) if (auto& v = gs.gnb; !v.empty()) { nb.insert(nb.end(), v.begin(), v.end()); v.clear(); } }\n";
        else if (multi) {
            o << "    const int grp = (int)(intptr_t)ctx;\n";
            // The groups of one runSeqs run back to back and write no signal
            // in between (NBAs only): one twoNow() for all of them.
            if (two) o << "    static uint64_t tAt = ~0ULL; static bool tNow = false;\n"
                          "    if (S.seqRun != tAt) { tAt = S.seqRun; tNow = twoNow(); }\n"
                          "    if (tNow) DSQ2[grp](); else\n";
            o << "    DSQ[grp]();\n"
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
            if (sl.hi >= 0 && isPriv(sl.sig) && sigW_[sl.sig] <= 64) {   // a part of a private register
                const std::string m = "(" + M(sl.hi - sl.lo + 1) + " << " + std::to_string(sl.lo) + ")", P = "P" + sg;
                w = "    if (H" + K + " == 1) { const uint64_t m = " + m + "; " + P + ".v = (" + P + ".v & ~m) | (((uint64_t)V" + K + " << " +
                    std::to_string(sl.lo) + ") & m); " + P + ".x &= ~m; " + P + ".z &= ~m; if (" + P + ".x | " + P + ".z) pclean = false; }\n"
                    "    else if (H" + K + ") { " + pw(sl.sig, "v_insert(PR(" + P + ", " + std::to_string(sigW_[sl.sig]) + ", false), F" + K + ", " +
                    std::to_string(sl.hi) + ", " + std::to_string(sl.lo) + ")") + " if (" + P + ".x | " + P + ".z) pclean = false; }\n"
                    "    H" + K + " = 0;\n";
            } else if (sl.hi >= 0 && sigW_[sl.sig] <= 64 && !sigNet_[sl.sig]) {   // H 1: the part's 2-state value in V
                const std::string m = "(" + M(sl.hi - sl.lo + 1) + " << " + std::to_string(sl.lo) + ")";
                w = "    if (H" + K + " == 1) { H" + K + " = 0; const Val& c = S.sigs[" + sg + "].val; const uint64_t m = " + m +
                    "; S.fw(" + sg + ", Val{(c.v & ~m) | (((uint64_t)V" + K + " << " + std::to_string(sl.lo) + ") & m), c.x & ~m, " +
                    std::to_string(sigW_[sl.sig]) + ", c.z & ~m}); }\n"
                    "    else if (H" + K + ") { H" + K + " = 0; S.fw(" + sg + ", v_insert(S.get(" + sg + "), F" + K + ", " +
                    std::to_string(sl.hi) + ", " + std::to_string(sl.lo) + ")); }\n";
            } else if (sl.hi >= 0)
                w = "    if (H" + K + ") { H" + K + " = 0; S.fw(" + sg + ", v_insert(S.get(" + sg + "), F" + K + ", " +
                    std::to_string(sl.hi) + ", " + std::to_string(sl.lo) + ")); }\n";
            else if (isPriv(sl.sig))
                w = "    if (H" + K + " == 1) " + pwv(sl.sig, "V" + K) + " else if (H" + K + ") { " + pw(sl.sig, "F" + K) +
                    " if (P" + std::to_string(sl.sig) + ".x | P" + std::to_string(sl.sig) + ".z) pclean = false; } H" + K + " = 0;\n";
            else
                w = "    if (H" + K + " == 1) " + regWrite(sl.sig, "V" + K) + " else if (H" + K + ") S.fw(" + sg + ", F" + K +
                    "); H" + K + " = 0;\n";
            if (par && !isPriv(sl.sig)) {   // a public register: LFW (see DEF)
                if (sl.hi >= 0) {   // a deferred part keeps its bits: two parts of one register both land
                    const std::string H = std::to_string(sl.hi), Lo = std::to_string(sl.lo), pwid = std::to_string(sl.hi - sl.lo + 1);
                    w = "    if (H" + K + ") { const Val pv = H" + K + " == 1 ? make((uint64_t)V" + K + " & " + M(sl.hi - sl.lo + 1) + ", 0, " + pwid + ") : F" + K + "; H" + K +
                        " = 0; if (!DEF || locFast(" + sg + ")) S.fw(" + sg + ", v_insert(S.get(" + sg + "), pv, " + H + ", " + Lo + ")); else DEF->push_back({" + sg +
                        ", pv, " + H + ", " + Lo + "}); }\n";
                }
                else
                    w = "    if (H" + K + " == 1) LFW(" + sg + ", make((uint64_t)V" + K + " & " + M(sigW_[sl.sig]) + ", 0, " + std::to_string(sigW_[sl.sig]) +
                        ")); else if (H" + K + ") LFW(" + sg + ", F" + K + "); H" + K + " = 0;\n";
            }
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
            size_t nCone = 0, nClk = 0;
            std::vector<char> inClk(procs.size(), 0);
            coneClkIns_.clear();
            coneK_ = false;
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
                bool clk = clock_[c];   // a clock, or the source its copies down the hierarchy come from
                for (size_t x = 0; x < clock_.size() && !clk; x++) clk = clock_[x] && x < domRoot_.size() && domRoot_[x] == c;
                if (clk) {
                    coneClkIns_.push_back(c);
                    for (int i : byRank)
                        if (in1[i] && !inClk[i]) { inClk[i] = 1; nClk++; }
                }
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
                // After a cone pass: when every private signal was known before
                // it, only the ones the cone writes can have changed that (a
                // full scanP on every clock edge was 7% of PicoRV32 x16).
                auto rescan = [&](const std::vector<size_t>& units) {
                    std::set<int> w;
                    for (size_t j : units)
                        for (auto* in : unitIns[j])
                            if (isPriv(in->sig) && !scratch(in->sig) && !deadSig[in->sig]) w.insert(in->sig);
                    std::string r = "    if (pclean) { uint64_t x = 0;";
                    for (int sg : w) r += " x |= P" + std::to_string(sg) + ".x;";
                    return r + " if (x) pclean = false; } else scanP();\n";
                };
                std::vector<std::string> callsC = shareChunks(o, "combC", coneUnits, combStmts, unitProc, unitIns, procs, kChunk);
                o << "static void combCone(const void*) {\n" << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n");
                for (auto& c : callsC) o << "    " << c << ";\n";
                if (privW) o << rescan(coneUnits);
                o << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n") << "    if (S.observed) syncP();\n}\n";
                // The clocks' own cone: when it is smaller, a clock edge runs
                // only it (Ibex: the clock gate and the clock copies, not the
                // controller logic behind the gate's enable).
                std::sort(coneClkIns_.begin(), coneClkIns_.end());
                if (!coneClkIns_.empty() && nClk < nCone && !std::getenv("MSIM_NOCONEK")) {
                    coneK_ = true;
                    std::vector<size_t> kUnits;
                    bool kPriv = false;
                    for (size_t j : coneUnits)
                        if (inClk[unitProc[j]]) {
                            kUnits.push_back(j);
                            for (auto* in : unitIns[j]) kPriv |= isPriv(in->sig);
                        }
                    std::vector<std::string> callsK = shareChunks(o, "combK", kUnits, combStmts, unitProc, unitIns, procs, kChunk);
                    o << "static void combConeK(const void*) {\n" << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n");
                    for (auto& c : callsK) o << "    " << c << ";\n";
                    if (kPriv) o << rescan(kUnits);
                    o << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n") << "    if (S.observed) syncP();\n}\n";
                } else coneClkIns_.clear();
            }
        }
        // armChk before the pass (a testbench write to a private input) and
        // after it (a private comb output read by a later @* block).
        o << "static void combAll(const void*) {\n" << (dAhead_ ? "    if (DAHEAD) return;   // fcommit queues the pass after it\n" : "")
          << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n");
        if (two) {
            o << "    if (twoNow()) {\n        cdirty = false;\n";
            for (auto& c : callsQ) o << "        " << c << "; if (cdirty) goto slow;\n";
            o << "        goto done;\n    }\n    slow: ;\n";
            for (auto& c : callsP) o << "    " << c << ";\n";
            o << "    scanP(); why4();\n    done: ;\n";
        } else
            for (auto& c : callsP) o << "    " << c << ";\n";
        o << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n") << "    if (S.observed) syncP();\n}\n";
        if (par) {
            // Per group: its slots' commits, then its comb statements in rank order.
            // Per group: its slots' commits, then its comb units in rank order,
            // through the shared per-module chunks (shareChunks) like combAll.
            std::vector<std::vector<std::string>> gCommit(G), gListed(G);
            std::vector<std::vector<size_t>> gUnits(G);
            std::string pubComb, pubComb2;
            for (size_t k = 0; k < slots.size(); k++) (slotListed[k] ? gListed : gCommit)[groupOf(slots[k].sig)].push_back(commitStmts[k]);
            for (size_t j = 0; j < unitIns.size(); j++) {
                if (unitIns[j].empty() || skipUnit[j]) continue;
                if (pubUnit[j]) { pubComb += combStmts[j]; pubComb2 += combStmts2[j]; continue; }
                gUnits[groupOf(unitIns[j][0]->sig)].push_back(j);
            }
            o << "static void cmPub(bool two) {\n    const bool obs = S.observed; (void)obs;\n"
              << "    if (two) {\n" << bail(pubComb2, "{ cdirty = true; goto slow; }") << "        return;\n    }\n    slow: ;\n" << pubComb << "}\n";
            for (int g = 0; g < G; g++) {
                const std::string G_ = std::to_string(g);
                std::vector<std::string> cP = shareChunks(o, "cgP" + G_ + "_", gUnits[g], combStmts, unitProc, unitIns, procs, kChunk);
                std::vector<std::string> cQ;
                if (two) cQ = shareChunks(o, "cgQ" + G_ + "_", gUnits[g], combStmtsQ, unitProc, unitIns, procs, kChunk);
                int nc = chunks("cmC" + G_ + "_", gCommit[g]), nl = chunks("cmL" + G_ + "_", gListed[g]);
                o << "static void cmG" << g << "(bool two) {\n"
                  << "    { const C0E* L = GS[" << g << "].c0.data(); const uint32_t n = GS[" << g << "].c0n; if (C0TWO) { for (uint32_t j = 0; j < n; j++) { PSig& p = *L[j].p; p.v = (p.v & ~L[j].m) | L[j].v; } } else for (uint32_t j = 0; j < n; j++) { PSig& p = *L[j].p; const uint64_t m = L[j].m; p.v = (p.v & ~m) | L[j].v; p.x &= ~m; p.z &= ~m; } GS[" << g << "].c0n = 0; }\n"
                  << "    if (GS[" << g << "].c1n) { c1Apply(GS[" << g << "].c1.data(), GS[" << g << "].c1n); GS[" << g << "].c1n = 0; }\n"
                  << "    if (GS[" << g << "].ovf) {\n        GS[" << g << "].ovf = false;\n";
                for (int k = 0; k < nl; k++) o << "        cmL" << g << "_" << k << "();\n";
                o << "    }\n";
                for (int k = 0; k < nc; k++) o << "    cmC" << g << "_" << k << "();\n";
                if (two) {
                    o << "    if (two) {\n";
                    for (auto& c : cQ) o << "        " << c << "; if (cdirty) goto slow;\n";
                    o << "        return;\n    }\n    slow: ;\n";
                } else o << "    (void)two;\n";
                for (auto& c : cP) o << "    " << c << ";\n";
                o << "}\n";
            }
            // twoNow() before the commits: in 2-state mode every slot holds a
            // known value, so the commits keep every private signal known.
            o << "static void fcommit() {\n"
                 "    DAHEAD = false;\n"
                 "    const bool t = twoNow();\n"
                 "    if (C0TWO) { for (uint32_t j = 0; j < C0N; j++) { PSig& p = *C0L[j].p; p.v = (p.v & ~C0L[j].m) | C0L[j].v; } } else for (uint32_t j = 0; j < C0N; j++) { PSig& p = *C0L[j].p; const uint64_t m = C0L[j].m; p.v = (p.v & ~m) | C0L[j].v; p.x &= ~m; p.z &= ~m; }\n    C0N = 0;\n"                 "    { const bool obs = S.observed;" " for (uint32_t j = 0; j < C1N; j++) { const int s = C1L[j].s; Sig& g = S.sigs[s];"
             " const uint64_t m = C1L[j].m; Val& q = g.val;"
             " if (__builtin_expect(!obs && S.waiters[s].empty(), 1)) { q.v = (q.v & ~m) | C1L[j].v; q.x &= ~m; q.z &= ~m; q.real = false; q.str = false; }"
             " else S.fwSlow(s, Val{(q.v & ~m) | C1L[j].v, q.x & ~m, g.w, q.z & ~m}); } C1N = 0;" " }\n"
                 "    const bool tune = !S.observed && !g_cov;\n"
                 "    const int use = tune ? S.domainTune[2].pick(S.threads) : 1;\n"
                 "    S.parallelFor(" << G << ", [t](size_t g) { DEF = Sim::tlInPar ? &GS[g].def : nullptr; CMG[g](t); }, use);\n"
                 "    if (tune) S.domainTune[2].done(S.threads);\n"
                 "    DEF = nullptr;\n"
                 "    const bool late = defApply();\n"
                 "    cmPub(t);\n"
                 "    if (late) combAll(nullptr);\n"
                 "    if (!t || cdirty.exchange(false)) scanP();\n"
              << (armIns_.empty() ? "" : "    if (ARMLEFT) armChk();\n") << "    if (S.observed) syncP();\n}\n\n";
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
             "        if (h == 1) { t.p->v = VA[k] & t.m; t.p->x = 0; t.p->z = 0; } else { PW(*t.p, t.w, FA[k]); if (t.p->x | t.p->z) pclean = false; }\n"
             "    } else if (t.kind == 1) {\n"
             "        HA[k] = 0;\n"
             "        if (h != 1) S.fw(t.sig, FA[k]);\n"
             "        else if (__builtin_expect(!obs && S.waiters[t.sig].empty(), 1)) {\n"
             "            Val& q = S.sigs[t.sig].val; q.v = VA[k] & t.m; q.x = 0; q.z = 0; q.real = false; q.str = false;\n"
             "        } else S.fwSlow(t.sig, make(VA[k] & t.m, 0, t.w));\n"
             "    } else CMS[k / " << kChunk << "](k);\n"
             "}\n";
        o << "static void fcommit() { DAHEAD = false;" << (multi ? " dpend = false;" : "");
        o << " const bool obs = S.observed;"
             // The common slot inline: a private signal given a known value.
             " if (C0TWO) { for (uint32_t j = 0; j < C0N; j++) { PSig& p = *C0L[j].p; p.v = (p.v & ~C0L[j].m) | C0L[j].v; } } else for (uint32_t j = 0; j < C0N; j++) { PSig& p = *C0L[j].p; const uint64_t m = C0L[j].m; p.v = (p.v & ~m) | C0L[j].v; p.x &= ~m; p.z &= ~m; } C0N = 0;"
             " for (uint32_t j = 0; j < C1N; j++) { const int s = C1L[j].s; Sig& g = S.sigs[s];"
             " const uint64_t m = C1L[j].m; Val& q = g.val;"
             " if (__builtin_expect(!obs && S.waiters[s].empty(), 1)) { q.v = (q.v & ~m) | C1L[j].v; q.x &= ~m; q.z &= ~m; q.real = false; q.str = false; }"
             " else S.fwSlow(s, Val{(q.v & ~m) | C1L[j].v, q.x & ~m, g.w, q.z & ~m}); } C1N = 0;"
             " if (!CLOVF) { for (uint32_t j = 0; j < CN; j++) { const uint32_t k = CL[j]; const uint8_t h = HA[k];"
             " if (h == 1) { const CTRow& t = CTAB[k]; if (t.kind == 0) { HA[k] = 0; PSig& p = *t.p; p.v = VA[k] & t.m; p.x = 0; p.z = 0; continue; } }"
             " if (h) commitOne(k, obs); } }"
             " else for (uint32_t k = 0; k < " << slots.size() << "; k++) commitOne(k, obs);"
             " CN = 0; CLOVF = false;" << (armIns_.empty() ? "" : " if (ARMLEFT) armChk();");
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
        // A virtual interface's member (25.9) is a relative id as a frame's
        // variable is: R() resolves it in the interpreter only.
        for (auto& i : p.code) noFrames(i);
        // Each NBA runs at most once per call (no jump goes back): one that
        // would not change its register can be left out (Sim::nbaKeep).
        bool fwd = !std::getenv("MSIM_NONBASKIP");
        for (size_t pc = 1; pc < end && fwd; pc++) {
            const Instr& i = p.code[pc];
            if ((i.k == IK::Jmp || i.k == IK::JmpIfNot) && (i.target <= pc || (i.xt != Instr::NOX && i.xt <= pc))) fwd = false;
        }
        for (size_t pc = 1; pc < end; pc++) {
            const Instr& i = p.code[pc];
            o << L(pc) << ":" << CH(&p, pc);
            switch (i.k) {
                case IK::Nba:
                    if (i.drv >= 0 || i.dyn >= 0 || i.nbaLater || isRel(i.sig)) throw CodegenUnsupported("NBA kind in a clocked process");
                    if (i.arr || i.bidx) o << dynTarget(i, fast(i.e), " nb.push_back({s, v, hi, lo});");
                    else if (fwd && i.hi < 0 && i.sig >= 0 && (size_t)i.sig < soleNba_.size() && soleNba_[(size_t)i.sig])
                        o << " { Val v_ = " << fast(i.e) << "; if (S.nbaKeep(" << i.sig << ", v_)) nb.push_back({" << i.sig << ", std::move(v_), -1, 0}); }\n";
                    else o << " nb.push_back({" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << "});\n";
                    break;
                case IK::JmpIfNot: o << jmpIfNotSplit(i, jumpTo(&p, pc, L(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, L(i.xt))); break;
                case IK::Jmp: o << " goto " << L(i.target) << ";\n"; break;
                case IK::Assign:   // a temporary of its own (Instr::tmp)
                    o << " S.poke(" << i.sig << ", " << fast(i.e) << ", " << i.hi << ", " << i.lo << ");\n";
                    break;
                default: throw CodegenUnsupported("instruction " + std::to_string((int)i.k) + " in a clocked process (line " + std::to_string(i.line) + ")");
            }
        }
        o << L(end) << ": return;\n}\n\n";
    }

    // ---- shared event-driven functions (no cycle domain) ----
    // Each comb and clocked process outside the cycle domain is a function of
    // its own, with its signal numbers in the text: OpenTitan has 400,000 of
    // them, 300 MB of C++, mostly the same few thousand functions once per
    // instance. A process whose text is another's with the numbers of each
    // scope (instance) moved by one distance calls that one's function with
    // its distances (ctx: a row of the table DT). The text decides: anything
    // but a signal number that differs (a constant, a width, a string) keeps
    // the process's own copy.
    struct EvUnit { int i; bool seq; std::string text; };
    // A function of megabytes (OpenTitan's sampled-value process: 46,000
    // statements; a comb process of 30,000) took the C++ compiler longer
    // than the rest of its file and held the build up: its straight runs of
    // signal writes go to helper functions of kOutlineRun statements each.
    // A label something jumps to starts a new run (the call keeps it).
    static constexpr size_t kOutlineMin = 256u << 10, kOutlineRun = 1000;
    // A clocked function (emitSeq) whose jumps all go forward splits where no
    // jump crosses: each piece is a function of its own, a jump to the end of
    // its piece returns, and the function calls the pieces in order.
    // XiangShan's TileLink fixers are blocks of 24,000 "if (c) r <= e;":
    // conditions between the NBAs leave no straight run for outlineRuns, and
    // one function of 25 MB took gcc over ten minutes and 9 GB.
    static constexpr size_t kSplitPiece = 128u << 10;
    static std::vector<std::string> splitSeq(std::string& t) {
        std::vector<std::string> helpers;
        static const std::string pre = "static void ", sfx = "(const void*, std::vector<Sim::NbaItem>& nb) {";
        size_t sigAt = 0;
        for (;;) {   // the signature: after the comment lines
            if (t.compare(sigAt, pre.size(), pre) == 0) break;
            if (t.compare(sigAt, 2, "//") != 0) return helpers;
            sigAt = t.find('\n', sigAt);
            if (sigAt == std::string::npos) return helpers;
            sigAt++;
        }
        size_t sigEnd = t.find('\n', sigAt);
        if (sigEnd == std::string::npos || sigEnd < sigAt + pre.size() + sfx.size() || t.compare(sigEnd - sfx.size(), sfx.size(), sfx) != 0)
            return helpers;
        const std::string name = t.substr(sigAt + pre.size(), sigEnd - sfx.size() - sigAt - pre.size());
        for (char c : name) if (!(std::isalnum((unsigned char)c) || c == '_')) return helpers;
        // The body: one "s_<n>: ..." line per instruction, labels rising,
        // the last "s_<end>: return;", then "}".
        struct Ln { size_t a, e; long long n; };
        std::vector<Ln> ls;
        size_t at = sigEnd + 1;
        for (;;) {
            size_t nl = t.find('\n', at);
            if (nl == std::string::npos) return helpers;
            if (nl == at + 1 && t[at] == '}') { at = nl + 1; break; }
            if (t.compare(at, 2, "s_") != 0) return helpers;
            char* e = nullptr;
            long long n = std::strtoll(t.c_str() + at + 2, &e, 10);
            if (!e || *e != ':' || (!ls.empty() && n <= ls.back().n)) return helpers;
            ls.push_back({at, nl, n});
            at = nl + 1;
        }
        if (ls.size() < 3 || t.compare(ls.back().a, ls.back().e - ls.back().a, "s_" + std::to_string(ls.back().n) + ": return;") != 0)
            return helpers;
        const std::string rest = t.substr(at);   // whatever follows the function
        const char* pe = std::getenv("MSIM_SPLITPIECE");   // tests: small pieces
        const size_t piece = pe ? std::strtoull(pe, nullptr, 10) : kSplitPiece;
        // A piece starts where the size says, and where a jump from an
        // earlier piece lands. A jump out of a piece returns its code, and the
        // function goes on there.
        std::unordered_map<long long, size_t> lineOf;
        for (size_t k = 0; k < ls.size(); k++) lineOf[ls[k].n] = k;
        std::vector<std::vector<std::pair<size_t, long long>>> jumps(ls.size());   // (offset of "goto", label)
        for (size_t k = 0; k + 1 < ls.size(); k++) {
            std::string_view ln(t.data() + ls[k].a, ls[k].e - ls[k].a);
            for (const char* bad : {"co_", "return", " D_", "x_)", "x_,"})
                if (ln.find(bad) != std::string_view::npos) return helpers;
            for (size_t g = ln.find("goto s_"); g != std::string_view::npos; g = ln.find("goto s_", g + 7)) {
                long long n = std::strtoll(std::string(ln.substr(g + 7, 20)).c_str(), nullptr, 10);
                if (n <= ls[k].n || !lineOf.count(n)) return helpers;   // a loop
                jumps[k].push_back({g, n});
            }
        }
        std::vector<size_t> cuts{0};
        std::set<long long> pend, need;
        size_t bytes = 0;
        for (size_t k = 0; k + 1 < ls.size(); k++) {
            const long long n = ls[k].n;
            if (k > cuts.back() && (need.count(n) || bytes >= piece)) {
                for (long long x : pend) if (x > n) need.insert(x);
                pend.clear();
                cuts.push_back(k);
                bytes = 0;
            }
            for (auto& j : jumps[k]) pend.insert(j.second);
            bytes += ls[k].e - ls[k].a + 1;
        }
        if (cuts.size() < 2) return helpers;
        const long long endN = ls.back().n;
        std::string body = t.substr(0, sigEnd + 1);
        for (size_t c = 0; c < cuts.size(); c++) {
            size_t from = cuts[c], to = c + 1 < cuts.size() ? cuts[c + 1] : ls.size() - 1;
            std::map<long long, int> exits;
            std::string h = name + "_p" + std::to_string(c);
            std::string f = "static int " + h + "(std::vector<Sim::NbaItem>& nb) {\n";
            for (size_t k = from; k < to; k++) {
                size_t last = ls[k].a;
                for (auto& [g, n] : jumps[k]) {
                    if (n < ls[to].n) continue;   // inside the piece
                    auto it = exits.emplace(n, (int)exits.size() + 1).first;
                    size_t ga = ls[k].a + g, ge = ga + 7 + std::to_string(n).size();
                    f.append(t, last, ga - last);
                    f += "return " + std::to_string(it->second);
                    last = ge;
                }
                f.append(t, last, ls[k].e + 1 - last);
            }
            f += "    return 0;\n}\n\n";
            helpers.push_back(std::move(f));
            body += "s_" + std::to_string(ls[from].n) + ":";
            if (exits.empty()) body += " " + h + "(nb);\n";
            else {
                body += " switch (" + h + "(nb)) {";
                for (auto& [n, code] : exits)
                    body += " case " + std::to_string(code) + ": " + (n == endN ? std::string("return;") : "goto s_" + std::to_string(n) + ";");
                body += " default: break; }\n";
            }
        }
        body.append(t, ls.back().a, ls.back().e - ls.back().a + 1);
        body += "}\n" + rest;
        t = std::move(body);
        return helpers;
    }
    // Returns the helpers, one per element, and leaves t calling them.
    static std::vector<std::string> outlineRuns(std::string& t) {
        std::vector<std::string> helpers;
        if (t.find(" D_ = ") != std::string::npos) return helpers;   // shareEv's templates: the distances are locals
        auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        std::unordered_set<std::string> tgt;
        for (size_t a = t.find("goto "); a != std::string::npos; a = t.find("goto ", a + 5)) {
            size_t e = a + 5;
            while (e < t.size() && idc(t[e])) e++;
            tgt.insert(t.substr(a + 5, e - a - 5));
        }
        // The function: its signature is the first line that starts with
        // "static " or "Task " and ends with "{".
        size_t sigAt = std::string::npos;
        std::string name;
        for (size_t at = 0; at < t.size();) {
            size_t nl = t.find('\n', at);
            if (nl == std::string::npos) break;
            if ((t.compare(at, 7, "static ") == 0 || t.compare(at, 5, "Task ") == 0) && nl > at && t[nl - 1] == '{') {
                size_t par = t.find('(', at);
                if (par == std::string::npos || par > nl) break;
                size_t b = par;
                while (b > at && idc(t[b - 1])) b--;
                name = t.substr(b, par - b);
                sigAt = at;
                break;
            }
            at = nl + 1;
        }
        if (sigAt == std::string::npos || name.empty()) return helpers;
        // A clocked process's NBAs go to its list nb: helpers take it along
        // (XiangShan: one block of 24,000 NBAs, 38 MB of C++ in one function).
        const bool nbOk = [&] {
            static const std::string sfx = "std::vector<Sim::NbaItem>& nb) {";
            size_t nl = t.find('\n', sigAt);
            return nl != std::string::npos && nl >= sigAt + sfx.size() && t.compare(nl - sfx.size(), sfx.size(), sfx) == 0;
        }();
        // A line that may move: "<label>: S.set(...);" and friends, naming
        // nothing local to the function.
        auto movable = [&](size_t a, size_t nl, size_t& stmt) {
            size_t c = a;
            while (c < nl && idc(t[c])) c++;
            if (c == a || c >= nl || t[c] != ':') return false;
            stmt = c + 1;
            size_t b = stmt;
            while (b < nl && t[b] == ' ') b++;
            bool ok = false;
            for (const char* p : {"S.set(", "S.setOneBit(", "S.setPart(", "S.poke(", "S.pokeQ(", "S.drive("})
                if (t.compare(b, std::strlen(p), p) == 0) { ok = true; break; }
            bool nbLine = false;
            if (!ok && nbOk)
                for (const char* p : {"nb.push_back({", "{ Val v_ = "})
                    if (t.compare(b, std::strlen(p), p) == 0) { ok = nbLine = true; break; }
            if (!ok || !(t[nl - 1] == ';' || (nbLine && t[nl - 1] == '}'))) return false;
            std::string_view ln(t.data() + b, nl - b);
            for (const char* bad : {"goto", "co_", "rs.", "ctl", "nb.", "return", "x_", "frs", "fr->", "lr"})
                if (ln.find(bad) != std::string_view::npos && !(nbLine && !std::strcmp(bad, "nb."))) return false;
            if (nbLine) {   // its own nb only: no other name that ends in nb
                for (size_t q = ln.find("nb."); q != std::string_view::npos; q = ln.find("nb.", q + 3))
                    if (q && idc(ln[q - 1])) return false;
                for (const char* bad : {"v_ =", "v_)"})
                    if (ln.find(bad) != std::string_view::npos && ln.substr(0, 11) != "{ Val v_ = ") return false;
            }
            return true;
        };
        std::string body;
        int nh = 0;
        std::vector<std::pair<std::string, std::string>> run;   // label, statement
        auto flush = [&] {
            if (run.size() < 64) {
                for (auto& [l, st] : run) body += l + ":" + st + "\n";
            } else
                for (size_t k = 0; k < run.size(); k += kOutlineRun) {
                    std::string h = name + "_o" + std::to_string(nh++);
                    std::string f = "static void " + h + (nbOk ? "(std::vector<Sim::NbaItem>& nb) {\n" : "() {\n");
                    for (size_t j = k; j < std::min(run.size(), k + kOutlineRun); j++) f += run[j].second + "\n";
                    f += "}\n\n";
                    helpers.push_back(std::move(f));
                    body += run[k].first + ": " + h + (nbOk ? "(nb);\n" : "();\n");
                }
            run.clear();
        };
        for (size_t at = sigAt; at < t.size();) {
            size_t nl = t.find('\n', at);
            if (nl == std::string::npos) nl = t.size();
            size_t stmt = 0;
            if (movable(at, nl, stmt)) {
                std::string lab = t.substr(at, stmt - 1 - at);
                if (tgt.count(lab)) flush();
                run.push_back({lab, t.substr(stmt, nl - stmt)});
            } else {
                flush();
                body.append(t, at, nl - at);
                if (nl < t.size()) body += '\n';
            }
            at = nl + 1;
        }
        flush();
        if (nh) t = t.substr(0, sigAt) + body;
        return helpers;
    }
    // A process that never waits (a constant continuous assign, an initial
    // block without delays: OpenTitan has 46,000): a plain function that a
    // coroutine of the header (evOnce) calls when the process starts, not a
    // coroutine of its own, which costs the C++ compiler far more.
    bool evPlain(const std::string& t, int n) {
        const std::string sig = "static Task proc" + std::to_string(n) + "() {\n";
        size_t a = t.find(sig);
        if (a == std::string::npos || t.find("Task", a + sig.size()) != std::string::npos || t.find("co_await") != std::string::npos ||
            t.find("co_yield") != std::string::npos)
            return false;
        size_t r = t.find("co_return;");
        if (r == std::string::npos || t.find("co_return", r + 1) != std::string::npos) return false;
        evPlain_[(size_t)n] = 1;
        return true;
    }
    static std::string plainText(const std::string& t, int n) {
        const std::string sig = "static Task proc" + std::to_string(n) + "() {\n";
        std::string r = t;
        size_t a = r.find(sig);
        r.replace(a, sig.size(), "static void proc" + std::to_string(n) + "(const void*) {\n");
        size_t c = r.find("co_return;");
        r.replace(c, 10, "return;");
        return r;
    }
    std::string evCtx(int i) const {
        if (i < 0 || (size_t)i >= evAt_.size() || evAt_[(size_t)i] < 0) return "nullptr";
        return "DT + " + std::to_string(evAt_[(size_t)i]);
    }
    // The text with the process number in its names and labels as @@.
    static std::string evNorm(const std::string& t, int n) {
        const std::string id = std::to_string(n);
        auto idc = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        std::string r;
        r.reserve(t.size());
        for (size_t i = 0; i < t.size();) {
            bool hit = false;
            if (i == 0 || !idc(t[i - 1]))
                for (const char* pre : {"combv", "combc", "comb", "seq", "proc", "c"}) {
                    size_t n0 = std::strlen(pre);
                    if (t.compare(i, n0, pre) != 0 || t.compare(i + n0, id.size(), id) != 0) continue;
                    size_t e = i + n0 + id.size();
                    char nx = e < t.size() ? t[e] : 0;
                    if (pre[1] == 0 ? nx != '_' : nx != '(') continue;
                    r += pre;
                    r += "@@";
                    i = e;
                    hit = true;
                    break;
                }
            if (!hit && t.compare(i, 11, "// process ") == 0 && t.compare(i + 11, id.size(), id) == 0 && i + 11 + id.size() < t.size() &&
                t[i + 11 + id.size()] == ' ') {
                r += "// process @@";
                i += 11 + id.size();
                hit = true;
            }
            if (!hit) r += t[i++];
        }
        return r;
    }
    void shareEv(std::vector<EvUnit>& us, std::vector<std::string>& chunks) {
        // Each distinct signal of a template is a slot a member moves by its
        // own distance (the same signal twice: the same distance). Slots
        // that move together in every member then share one distance, and a
        // slot no member moves is left as it is.
        struct Tpl { size_t u; std::vector<long long> num; std::vector<int> slot; int nslot = 0; std::vector<std::pair<size_t, std::vector<long long>>> mem; };
        std::vector<Tpl> tpls;
        std::unordered_map<std::string, std::vector<size_t>> byShape;
        std::vector<std::string> norm(us.size());
        constexpr int kTries = 16;
        for (size_t u = 0; u < us.size(); u++) {
            norm[u] = evNorm(us[u].text, us[u].i);
            std::string shape;
            std::vector<long long> num;
            bool ok = true;
            size_t last = 0;
            refScan(norm[u], [&](size_t, size_t at, size_t e, long long n, int kind) {
                if (kind != 0) ok = false;
                shape += refPiece(norm[u].substr(last, at - last));
                shape += '\x01';
                num.push_back(n);
                last = e;
            });
            shape += refPiece(norm[u].substr(last));
            evTotal_ += us[u].text.size();
            if (!ok || num.empty()) { chunks.push_back(std::move(us[u].text)); norm[u].clear(); continue; }
            auto& cand = byShape[shape];
            bool placed = false;
            std::vector<long long> D;
            std::vector<char> set;
            for (size_t c = 0; c < cand.size() && c < (size_t)kTries && !placed; c++) {
                Tpl& t = tpls[cand[c]];
                D.assign((size_t)t.nslot, 0);
                set.assign((size_t)t.nslot, 0);
                bool good = true;
                for (size_t k = 0; k < num.size() && good; k++) {
                    int sl = t.slot[k];
                    long long d = num[k] - t.num[k];
                    if (!set[(size_t)sl]) { set[(size_t)sl] = 1; D[(size_t)sl] = d; }
                    else good = D[(size_t)sl] == d;
                }
                if (!good) continue;
                t.mem.push_back({u, D});
                evSaved_ += us[u].text.size();
                std::string().swap(us[u].text);
                std::string().swap(norm[u]);
                placed = true;
            }
            if (placed) continue;
            if (cand.size() >= (size_t)kTries) { chunks.push_back(std::move(us[u].text)); norm[u].clear(); continue; }
            Tpl t;
            t.u = u;
            t.num = num;
            std::unordered_map<long long, int> slotOf;
            for (long long n : num) {
                auto [it, fresh] = slotOf.try_emplace(n, (int)slotOf.size());
                t.slot.push_back(it->second);
            }
            t.nslot = (int)slotOf.size();
            cand.push_back(tpls.size());
            tpls.push_back(std::move(t));
        }
        for (auto& t : tpls) {
            EvUnit& tu = us[t.u];
            if (t.mem.empty()) { chunks.push_back(std::move(tu.text)); continue; }
            // Slots whose distances are the same in every member share one;
            // all zero: none.
            std::vector<int> merged((size_t)t.nslot, -1);
            std::vector<int> rep;   // merged slot -> a slot of it
            {
                std::map<std::vector<long long>, int> cols;
                for (int sl = 0; sl < t.nslot; sl++) {
                    std::vector<long long> col;
                    bool zero = true;
                    for (auto& [u, d] : t.mem) { col.push_back(d[(size_t)sl]); zero &= d[(size_t)sl] == 0; }
                    if (zero) continue;
                    auto [it, fresh] = cols.try_emplace(col, (int)rep.size());
                    if (fresh) rep.push_back(sl);
                    merged[(size_t)sl] = it->second;
                }
            }
            const int nd = (int)rep.size();
            // The template: its numbers as N + D<slot>, its distances from ctx.
            const std::string nm = "S" + std::to_string(tu.i);
            std::string decl = " const int* D_ = (const int*)x_; (void)D_;";
            for (int k = 0; k < nd; k++) decl += (k ? ", D" : " const int D") + std::to_string(k) + " = D_[" + std::to_string(k) + "]";
            if (nd) decl += ";";
            const std::string& nt = norm[t.u];
            std::string r;
            r.reserve(nt.size() + nt.size() / 4);
            size_t last = 0, k = 0;
            refScan(nt, [&](size_t, size_t, size_t e, long long, int) {
                r.append(nt, last, e - last);
                if (int m = merged[(size_t)t.slot[k]]; m >= 0) r += " + D" + std::to_string(m);
                k++;
                last = e;
            });
            r.append(nt, last, std::string::npos);
            std::string out;
            out.reserve(r.size() + 4096);
            for (size_t i = 0; i < r.size();) {
                if (r.compare(i, 2, "@@") == 0) { out += nm; i += 2; continue; }
                bool hit = false;
                for (const char* sg : {"(const void*) {", "(const void*, Val v) {", "(const void*, std::vector<Sim::NbaItem>& nb) {"}) {
                    size_t n0 = std::strlen(sg);
                    if (r.compare(i, n0, sg) != 0) continue;
                    out += "(const void* x_";
                    out.append(sg + 12, n0 - 12);
                    out += decl;
                    i += n0;
                    hit = true;
                    break;
                }
                if (!hit) out += r[i++];
            }
            chunks.push_back(std::move(out));
            auto add = [&](size_t u, const std::vector<long long>* d) {
                evName_[(size_t)us[u].i] = nm;
                if (!nd) return;   // the same signals in every copy
                evAt_[(size_t)us[u].i] = (int)evDT_.size();
                for (int m = 0; m < nd; m++) evDT_.push_back(d ? (int)(*d)[(size_t)rep[(size_t)m]] : 0);
            };
            add(t.u, nullptr);
            for (auto& [u, d] : t.mem) add(u, &d);
        }
        if (std::getenv("MSIM_WHYSHARE"))
            std::fprintf(stderr, "shared event code: %zu of %zu bytes are calls to another instance's copy\n", evSaved_, evTotal_);
    }

    // A combinational process: its leading Assign instructions as a function.
    void emitComb(std::ostream& o, int n, const Prog& p, const std::string& kind) {
        o << "// process " << n << " (" << kind << ", combinational)\n";
        const bool as = combAsserts(p);
        // One assign: the value and the write (the kernel's batches call
        // them apart), and the process itself as the two (once, not three
        // copies of the expression for the C++ compiler).
        if (oneAssign(p) && !as && !p.code[0].bidx && !p.code[0].arr) {
            o << "static Val combv" << n << "(const void*) {" << CH(&p, 0) << " return " << fast(p.code[0].e) << "; }\n";
            o << "static void combc" << n << "(const void*, Val v) {" << assign(p.code[0], "v") << "}\n";
            o << "static void comb" << n << "(const void* x) { combc" << n << "(x, combv" << n << "(x)); }\n\n";
            return;
        }
        if (as) o << combCtlDecl(n);
        o << "static void comb" << n << "(const void*) {\n";
        if (as) o << "    CC" << n << "->epoch++;\n";
        auto L = [&](size_t pc) { return "c" + std::to_string(n) + "_" + std::to_string(pc); };
        size_t pc = 0;
        for (; pc < p.code.size(); pc++) {
            const Instr& i = p.code[pc];
            if (i.k == IK::JmpIfNot) {
                o << L(pc) << ":" << CH(&p, pc) << jmpIfNotSplit(i, jumpTo(&p, pc, L(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, L(i.xt)));
                continue;
            }
            if (i.k == IK::Jmp) { o << L(pc) << ":" << CH(&p, pc) << " goto " << L(i.target) << ";\n"; continue; }
            if (i.k == IK::Assert) { o << L(pc) << ":" << CH(&p, pc) << combAssert(n, i); continue; }
            if (i.k != IK::Assign) break;
            if (i.arr) throw CodegenUnsupported("array write in a static combinational process");
            if (i.bidx) {
                o << L(pc) << ":" << CH(&p, pc) << dynTarget(i, fast(i.e), " S.setBitsV(s, v, hi, lo);");
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
    // A DPI-C import function call (35.5): the C function itself, found in
    // the user's libraries when simv starts (Sim cdpiSym), with the
    // arguments in their C layout (dpiconv.hpp) and the outputs and result
    // written back to the formals' signals, as the interpreter does
    // (dpi.inc invoke). Import tasks, context imports (svGetScope and the
    // other scope functions are msim's own), and array or struct arguments
    // are not compiled yet.
    std::string dpiCall(const Instr& i) {
        const DpiFunc& f = *i.dpi;
        if (f.task) throw CodegenUnsupported("a DPI-C import task");
        if (f.context) throw CodegenUnsupported("a DPI-C context import");
        auto ctype = [](DpiType t) -> std::string {
            switch (t) {
                case DT_VOID: return "void";
                case DT_BYTE: return "int8_t";
                case DT_SHORT: return "int16_t";
                case DT_INT: return "int32_t";
                case DT_LONG: return "int64_t";
                case DT_REAL: return "double";
                case DT_SHORTREAL: return "float";
                case DT_CHANDLE: return "void*";
                case DT_STRING: return "const char*";
                case DT_BIT: case DT_LOGIC: return "uint8_t";
                case DT_BITVEC: return "uint32_t";
                case DT_LOGICVEC: return "dpi::LogicVec";
                default: throw CodegenUnsupported("a DPI-C argument of this type");
            }
        };
        std::string sig = ctype(f.ret) + " (*)(", pre, call, post;
        for (size_t k = 0; k < f.args.size(); k++) {
            const DpiArg& a = f.args[k];
            if (a.uarr || a.t == DT_STRUCT) throw CodegenUnsupported("a DPI-C array or struct argument");
            if (a.sig < 0 || isPriv(a.sig)) throw CodegenUnsupported("a DPI-C argument without a signal");
            const bool vec = a.t == DT_BITVEC || a.t == DT_LOGICVEC;
            const std::string ct = ctype(a.t), K = std::to_string(k), sg = std::to_string(a.sig);
            sig += std::string(k ? ", " : "") + (vec ? (a.dir == 0 ? "const " : "") + ct + "*" : a.dir == 0 ? ct : ct + "*");
            pre += vec ? " " + ct + " a" + K + "[" + std::to_string((a.w + 31) / 32) + "] = {};"
                       : " " + ct + " a" + K + "{};";
            if (a.dir != 1) pre += " dpi::toC(" + std::string(vec ? "a" : "&a") + K + ", (DpiType)" + std::to_string((int)a.t) + ", " +
                                   std::to_string(a.w) + ", S.sigs[" + sg + "].val, ds);";
            call += std::string(k ? ", " : "") + (vec || a.dir == 0 ? "a" : "&a") + K;
            if (a.dir != 0)
                post += " S.set(" + sg + ", dpi::fromC(" + std::string(vec ? "a" : "&a") + K + ", (DpiType)" + std::to_string((int)a.t) + ", " +
                        std::to_string(a.w) + ", " + (a.sgn ? "true" : "false") + "));";
        }
        sig += ")";
        std::string o = " { std::deque<std::string> ds; using F = " + sig + "; static const F fn = (F)cdpiSym(" + quote(f.cName) + ");";
        o += pre;
        if (f.ret != DT_VOID && f.retSig >= 0) {
            o += " " + ctype(f.ret) + " r = fn(" + call + ");" + post;
            o += " S.set(" + std::to_string(f.retSig) + ", dpi::fromC(&r, (DpiType)" + std::to_string((int)f.ret) + ", " + std::to_string(f.retW) +
                 ", " + (f.retSgn ? "true" : "false") + "));";
        } else o += " fn(" + call + ");" + post;
        o += " if (S.finished) co_return; }\n";
        return o;
    }
    // Every subroutine a process may call, with no checks (analysis only).
    static void reachAll(const Prog* p, std::vector<const Prog*>& out) {
        for (const Prog* q : out)
            if (q == p) return;
        out.push_back(p);
        for (auto& i : p->code)
            if (i.k == IK::Call && i.callee) reachAll(i.callee, out);
    }
    static void reach(const Prog* p, std::vector<const Prog*>& out, bool frames = false) {
        for (const Prog* q : out)
            if (q == p) return;
        out.push_back(p);
        for (auto& i : p->code) frames ? framesOK(i) : noFrames(i);
        for (auto& i : p->code)
            if (i.k == IK::Call) {
                if (!i.callee) throw CodegenUnsupported("call without a callee");
                reach(i.callee, out, frames);
            }
    }
    // A coroutine process (emitProc) may use frames of its own: an automatic
    // task or function of a module (a new frame per call, its arguments set
    // in it), its slots (relative mode 0) and the slots of the frame the
    // last call returned from (kRelRet). Fork frames, `this`, handles and
    // containers stay in the interpreter.
    // Every mode but an enclosing frame (1-7, fork branches) and an
    // indirect one up the frame chain: R() resolves them from g_fr / g_ret.
    static bool relOK(int s) {
        if (!isRel(s)) return true;
        const int m = relMode(s);
        // `this` and the indirect modes (objects: randomize, handles) stay in
        // the interpreter: compiled, the random tests of clause 18 differed.
        static const bool obj = std::getenv("MSIM_CGOBJFRAMES") != nullptr;
        if (obj) return m == 0 || m == kRelThis || m == kRelRet || m == kRelIndSig || m == kRelIndFrame;
        return m == 0 || m == kRelRet;
    }
    static bool exprRelOK(const E& e) {
        if (!e) return true;
        if ((e->arr && isRel(e->arr->base)) || !relOK(e->sig)) return false;
        if (e->at && (hasRel(e->at) || !exprRelOK(e->at))) return false;   // h.x through a handle in a frame: the interpreter
        for (auto& k : e->kids) if (!exprRelOK(k)) return false;
        return true;
    }
    static void framesOK(const Instr& i) {
        auto no = [](const char* w) { throw CodegenUnsupported(std::string("automatic variables (frames): ") + w); };
        if (i.dynWait) no("a wait on automatic variables");
        if (!i.dinit.empty() || (i.lay && !i.lay->dyn.empty())) no("a container in a frame");
        if (i.lay && i.k != IK::Call) no("a fork block's frame");
        if (i.lay && i.vslot != -1) no("a virtual method call");
        if (!exprRelOK(i.self)) no("a method call through a handle or this");
        if (!i.init.empty() && !i.lay) no("arguments without a frame");
        for (auto& [off, e] : i.init) if (!exprRelOK(e)) no("an argument through a handle or this");
        for (auto* b : i.blays) if (b) no("a fork branch's frame");
        if (isRel(i.sig) && !relOK(i.sig)) no("a write through a handle or this");
        if (isRel(i.sig) && i.k != IK::New && i.k != IK::Dyn && i.k != IK::Randomize && (i.k != IK::Assign || i.drv >= 0 || i.arr)) throw CodegenUnsupported("automatic variables (frames): a frame slot written by instruction " + std::to_string((int)i.k) +
                                     (i.arr || i.bidx ? " (an element)" : i.drv >= 0 ? " (a driver)" : "") + ", line " + std::to_string(i.line));
        if (!exprRelOK(i.e) || !exprRelOK(i.bidx) || !exprRelOK(i.de)) no("a read through a handle or this");
        for (auto& a : i.args) if (!exprRelOK(a)) no("a read through a handle or this");
        for (auto& a : i.aidx) if (!exprRelOK(a)) no("an index through a handle or this");
        for (auto& [s, e] : i.events) if (isRel(s)) no("a wait on automatic variables");
        if (i.arr && isRel(i.arr->base)) no("an array in a frame");
    }
    static bool usesFrames(const Instr& i) {
        if (i.lay || isRel(i.sig)) return true;
        if (hasRel(i.e) || hasRel(i.bidx) || hasRel(i.de)) return true;
        for (auto& a : i.args) if (hasRel(a)) return true;
        for (auto& a : i.aidx) if (hasRel(a)) return true;
        return false;
    }
    int layIdx(const FrameLayout* l) {
        for (size_t k = 0; k < lays_.size(); k++) if (lays_[k] == l) return (int)k;
        lays_.push_back(l);
        return (int)lays_.size() - 1;
    }

    void emitProc(std::ostream& o, int n, const Prog& entry, const std::string& kind) {
        std::vector<const Prog*> progs;
        reach(&entry, progs, !std::getenv("MSIM_NOCGFRAMES"));
        bool frames = false;
        for (const Prog* q : progs)
            for (auto& i : q->code) frames |= usesFrames(i);
        // Frames with objects (new, randomize, rand_mode, a method or member
        // through a handle) stay in the interpreter: compiled, clauses 7 and
        // 18 printed other values. MSIM_CGOBJFRAMES=1 compiles them anyway.
        if (frames && !std::getenv("MSIM_CGOBJFRAMES")) {
            std::function<bool(const E&)> hasAt = [&](const E& e) {
                if (!e) return false;
                if (e->at) return true;
                for (auto& k : e->kids) if (hasAt(k)) return true;
                return false;
            };
            for (const Prog* q : progs)
                for (auto& i : q->code) {
                    bool obj = i.k == IK::New || i.k == IK::Randomize || i.k == IK::RandMode || i.k == IK::Dyn || i.self || i.vslot != -1 ||
                               hasAt(i.e) || hasAt(i.bidx) || hasAt(i.de);
                    for (auto& a : i.args) obj |= hasAt(a);
                    for (auto& [off, e] : i.init) obj |= hasAt(e);
                    if (obj) throw CodegenUnsupported("automatic variables (frames) with objects");
                }
        }
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
        // Frames, as exec.inc: the running one, the one the last call
        // returned from, and the caller's at each call. g_fr and g_ret are
        // set at every instruction (another process may have run).
        if (frames) o << "    FrameP fr, lr;\n    std::vector<FrameP> frs;\n";
        const std::string FG = frames ? " g_fr = fr.get(); g_ret = lr.get();" : "";
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
                o << L(P, pc) << ":" << CH(&p, pc) << FG;
                auto tgt = [&](size_t t) { return L(P, t > p.code.size() ? p.code.size() : t); };
                // A write chosen at run time (a[i] = ..., v[i] = ...): Assign and Nba only.
                bool dynW = (i.k == IK::Assign || i.k == IK::Nba) && (i.arr || i.bidx);
                if (dynW && ((i.k != IK::Assign && i.k != IK::Nba) || i.drv >= 0))
                    throw CodegenUnsupported(std::string("array or variable-index ") + (i.drv >= 0 ? "continuous assign" : "write (instruction " + std::to_string((int)i.k) + ")"));
                if (i.nbaLater && i.k != IK::Nba) throw CodegenUnsupported("intra-assignment delay");
                if (i.de && i.k != IK::Nba && i.k != IK::WaitDelay) throw CodegenUnsupported("delay expression");
                if ((i.dyn >= 0 || i.dsrc >= 0 || i.dres >= 0 || i.dkey >= 0) && !(mix && i.k == IK::Dyn)) throw CodegenUnsupported("dynamic array");
                if (!p.roots.empty() || !p.spans.empty()) throw CodegenUnsupported("disable of a named block (line " + std::to_string(i.line) + ")");
                if (i.fd) throw CodegenUnsupported("$fstrobe / $fmonitor");
                if ((!i.dly.empty() || !i.dlyE.empty()) && (i.k != IK::Assign || i.drv < 0 || i.tmp || i.xq || isPriv(i.sig)))   // (SDF timing keeps the interpreter)
                    throw CodegenUnsupported("delayed continuous assignment");
                if (!i.dly.empty() || !i.dlyE.empty()) {   // assign #d (10.3.3): the kernel's pending-driver queue, as the interpreter does
                    std::string v = W(i, fast(i.e));
                    if (i.hi >= 0)
                        v = "v_insert(allz(S.sigs[" + std::to_string(i.sig) + "].w), " + v + ", " + std::to_string(i.hi) + ", " +
                            std::to_string(i.lo) + ")";
                    std::string dl;
                    if (!i.dlyE.empty()) {   // assign #(expr): the delay as it is now
                        dl = "std::vector<uint64_t>{";
                        for (size_t k = 0; k < i.dlyE.size(); k++)
                            dl += (k ? ", " : "") + std::string("[](const Val& d) { int64_t t = d.x ? 0 : v_to_i64(d); return t < 0 ? 0ULL : (uint64_t)t; }(") + fast(i.dlyE[k]) + ")";
                        dl += "}";
                    } else {
                        dl = "DL";
                        o << " { static const std::vector<uint64_t> DL{";
                        for (size_t k = 0; k < i.dly.size(); k++) o << (k ? ", " : "") << i.dly[k] << "ULL";
                        o << "};";
                    }
                    if (i.dlyE.empty()) o << " S.driveLater(" << i.sig << ", " << i.drv << ", " << v << ", " << (int)i.s0 << ", " << (int)i.s1 << ", " << dl << "); }\n";
                    else o << " S.driveLater(" << i.sig << ", " << i.drv << ", " << v << ", " << (int)i.s0 << ", " << (int)i.s1 << ", " << dl << ");\n";
                    continue;
                }
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
                                                                      : " S.setBitsV(s, v, hi, lo);");
                    continue;
                }
                switch (i.k) {
                    case IK::Assign: o << (pd >= 0 ? assign(i, W(i, fast(i.e))) : assign(i)); break;
                    case IK::Nba:
                        if (i.nbaLater)   // x <= #d v: the update d ticks later (9.4.5)
                            o << " S.nbaAt(S.now + " << (i.de ? "v_time(" + fast(i.de) + ")" : std::to_string(i.delay) + "ULL") << ", " << i.sig
                              << ", " << W(i, fast(i.e)) << ", " << i.hi << ", " << i.lo << ");\n";
                        else
                            o << " S.nba(" << i.sig << ", " << W(i, fast(i.e)) << ", " << i.hi << ", " << i.lo << ");\n";
                        break;
                    case IK::JmpIfNot:
                        o << jmpIfNot(i, fast(i.e), jumpTo(&p, pc, tgt(i.target)), i.xt == Instr::NOX ? "" : jumpTo(&p, pc, tgt(i.xt)));
                        break;
                    case IK::Jmp: o << " goto " << tgt(i.target) << ";\n"; break;
                    case IK::WaitEdge: {
                        const int ak = n >= 0 && (size_t)n < armK_.size() ? armK_[n] : -1;
                        if (ak >= 0)
                            o << " for (uint32_t j = ARMOFF[" << ak << "]; j < ARMOFF[" << ak + 1 << "]; j++) ARMSNAP[j] = PA[ARMIN[j]]; ARMST[" << ak
                              << "] = 1;\n";
                        std::string wait;
                        if (i.events.size() > 1) {
                            events_.push_back(i.events);
                            wait = " co_await WaitAny{&EV" + std::to_string(events_.size() - 1) + "};";
                        } else
                            wait = " co_await Wait{" + std::to_string(i.sig) + ", " + edge(i.edge) + "};";
                        if (pi == 0 && (int)pc == entry.settleWait && ak < 0) {
                            // always_comb (9.2.2.2), as exec.inc: a change another
                            // process has already undone does not run it again.
                            std::vector<int> ws;
                            if (i.events.size() > 1) for (auto& [sg, ed] : i.events) ws.push_back(sg);
                            else ws.push_back(i.sig);
                            o << " { Val sv[" << ws.size() << "] = {";
                            for (size_t k = 0; k < ws.size(); k++) o << (k ? ", " : "") << "S.sigs[" << ws[k] << "].val";
                            o << "}; for (;;) {" << wait << " if (!(";
                            for (size_t k = 0; k < ws.size(); k++) o << (k ? " && " : "") << "S.sigs[" << ws[k] << "].val == sv[" << k << "]";
                            o << ")) break; } }";
                        } else
                            o << wait;
                        if (ak >= 0) o << " if (ARMST[" << ak << "] == 2) co_return; ARMST[" << ak << "] = 2; ARMLEFT--;";
                        o << (asserting ? " ctl->epoch++;\n" : "\n");   // a deferred assertion flush point (16.4.2)
                        o << gate;
                        break;
                    }
                    case IK::WaitDelay:
                        o << " co_await Delay{" << (i.de ? "v_time(" + fast(i.de) + ")" : std::to_string(i.delay) + "ULL") << "};\n" << gate;
                        break;
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
                    case IK::Finish:   // $fatal: its exit status (the interpreter: exec.inc)
                        o << " S.finished = true;" << (i.hi > 0 ? " if (S.exitCode < " + std::to_string(i.hi) + ") S.exitCode = " + std::to_string(i.hi) + ";" : "")
                          << " co_return;\n";
                        break;
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
                        if (frames) {   // exec.inc IK::Call: the caller's frame kept; an automatic subroutine gets a new one
                            o << " frs.push_back(fr);";
                            if (i.lay) {
                                std::string self = i.noSelf ? "0" : "fr ? fr->self : 0";
                                if (!i.noSelf && i.self)   // a method: the object the handle names
                                    self = "[&] { Val h = " + ex(i.self) + "; if (h.x || !h.v) nullAccess(); return (int)h.v; }()";
                                o << " { FrameP nf = newFrame(&LAY" << layIdx(i.lay) << ", nullptr, " << self << ");";
                                for (auto& [off, e] : i.init) o << " S.set(nf->base + " << off << ", " << ex(e) << ");";
                                o << " fr = std::move(nf); }";
                            }
                        }
                        o << " rs.push_back(" << retLabels.size() - 1 << "); goto " << L(idx(i.callee), 0) << ";\n";
                        break;
                    case IK::Ret: o << " goto ret;\n"; break;
                    case IK::Dpi: o << dpiCall(i); break;
                    // Mixed mode: msim's own instruction (MSIMI), run as exec.inc
                    // runs it; class tables and containers are msim's.
                    case IK::New:
                        o << " newObject(" << msimIns(i) << ");\n";
                        break;
                    case IK::Dyn:
                        if (!mix) throw CodegenUnsupported("dynamic array");
                        o << " dynExec(" << msimIns(i) << ");\n";
                        break;
                    case IK::Randomize:   // 18.6, 18.12
                        o << " { const Instr& i = " << msimIns(i) << "; int obj = 0;"
                             " if (i.e) { Val h = eval(i.e, rd); if (h.x || !h.v) nullAccess(); obj = (int)h.v;"
                             " if (i.rc && i.rc->objTmp >= 0) S.set(R(i.rc->objTmp), make((uint64_t)obj, 0, 64)); }"
                             " int r = doRandomize(obj, i.rc.get());" << FG <<
                             " if (i.sig >= 0) S.set(R(i.sig), make((uint64_t)r, 0, 32, 0, true)); }\n";
                        break;
                    case IK::RandMode:   // 18.8, 18.9
                        o << " { const Instr& i = " << msimIns(i) << "; Val h = eval(i.e, rd); if (h.x || !h.v) nullAccess();"
                             " randModeAll((int)h.v, i.hi != 0, truth(eval(i.de, rd)) == 1); }\n";
                        break;
                    default: throw CodegenUnsupported("instruction " + std::to_string((int)i.k));
                }
            }
            // Falling off the end is a return (an empty stack ends the process).
            o << L(P, p.code.size()) << ": goto ret;\n";
        }
        o << "ret:\n";
        if (calls) {
            o << "    if (rs.empty()) co_return;\n";
            if (frames)   // exec.inc ret(): back to the caller's frame, the callee's kept for kRelRet
                o << "    { FrameP f = std::move(frs.back()); frs.pop_back(); if (f != fr) { lr = std::move(fr); fr = std::move(f); } }\n";
            o << "    { int r = rs.back(); rs.pop_back();\n"
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
                           int precExp = -9, const std::string& timescale = "1ns", bool upf = false,
                           bool kernelObj = false, CodegenMix* interp = nullptr) {
    // interp: mixed mode allowed. A design that compiles whole is built as
    // before; otherwise once more with the processes that cannot be compiled
    // left to the interpreter (Codegen::mix), every signal in S.sigs.
    if (interp) {
        try {
            if (std::any_of(procs.begin(), procs.end(), [](const CodegenProc& p) { return p.interp; })) interp->on = true;
            if (!interp->on)   // on already: the library whatever compiles (a final block, say)
                return codegen(sigs, procs, parts, twoState, cycle, privates, covSlots, split, precExp, timescale, upf, kernelObj);
            throw CodegenUnsupported("mixed mode");
        } catch (const CodegenUnsupported& ex) {
            if (std::getenv("MSIM_WHYMIX")) std::fprintf(stderr, "msim: -compile: mixed mode: %s\n", ex.what());
            // Interpreted processes make signals at run time (frames,
            // objects: S.sigs grows and moves) and wait on signals no static
            // analysis sees (virtual interfaces): no cycle domain, which
            // keeps addresses in S.sigs and skips the kernel's wake-ups,
            // and no partitions. Only when nothing is interpreted (a final
            // block alone) does the domain stay.
            auto pass = [&](bool cyc) {
                Codegen g;
                g.files = split;
                g.precExp = precExp;
                g.timescale = timescale;
                g.twoState = twoState;
                g.cycle = cyc;
                g.privates = false;
                g.mix = true;
                std::string src = g.run(sigs, procs, cyc ? parts : CodegenParts{});
                interp->procs = std::move(g.interp);
                interp->ins = std::move(g.msimIns_);
                return src;
            };
            const bool forced = interp->on;
            interp->on = true;
            if (forced && cycle && interp->domain) {
                try {
                    std::string src = pass(true);
                    if (interp->procs.empty()) return src;
                } catch (const CodegenUnsupported& ex) {   // the domain's own limits: without it
                    if (std::getenv("MSIM_WHYMIX")) std::fprintf(stderr, "msim: -compile: no cycle domain: %s\n", ex.what());
                }
            }
            return pass(false);
        }
    }
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
        g.kernelObj = kernelObj;
        std::string src = g.run(sigs, procs, parts);
        if (g.needMsim_) throw CodegenUnsupported("classes");   // mixed mode, with msim's class tables
        if (!g.privHits_.empty()) {
            if (!g.privates) throw CodegenUnsupported("a private signal");
            for (int h : g.privHits_) noPriv.insert(g.origOf(h));
            continue;
        }
        if (covSlots) *covSlots = std::move(g.covSlots);
        return src;
    }
}
