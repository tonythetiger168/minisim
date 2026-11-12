// prog.hpp - process IR. Each Verilog process (always/initial) is lowered to a
// flat instruction list; one coroutine (exec, in exec.inc) interprets it.
// Flat code avoids nested coroutines: if/else are jumps. A task or function
// is its own Prog. Call pushes a return address in that same coroutine, so a
// #delay inside the callee suspends the process and keeps the address.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "expr.hpp"

enum Edge { ANY, POS, NEG };

// IK::Dyn commands (7.5, 7.8, 7.10).
enum DynCmd { D_SET, D_NEW, D_DELETE, D_DELETE_AT, D_PUSH_BACK, D_PUSH_FRONT, D_INSERT, D_POP_FRONT,
              D_POP_BACK, D_FIRST, D_LAST, D_NEXT, D_PREV, D_COPY, D_APPEND, D_APPEND_SLICE, D_REVERSE,
              D_SORT, D_RSORT, D_SHUFFLE,
              D_SORT_BY, D_RSORT_BY,     // sort / rsort with (7.12.2): keys in dsrc
              D_UNPACK,                  // q = {>>{v}} (11.4.14.3): e cut into elements
              D_DEFAULT };               // '{default: v} of an associative array (7.9.11): a missing element reads as e
// IK::ProcOp operations (9.7).
enum ProcOpKind { PO_SELF, PO_STATUS, PO_KILL, PO_AWAIT, PO_SUSPEND, PO_RESUME };
enum class IK {
    Assign,     // sig = e            (blocking)
    Nba,        // sig <= e           (non-blocking)
    JmpIfNot,   // if (e is not true) pc = target
    Jmp,        // pc = target
    WaitEdge,   // @(edge sig)  or  @(e1 or e2 ...) when events is non-empty
    WaitDelay,  // #delay
    Finish,     // $finish
    Display,    // $display: print now, in the region the process is running in
    Strobe,     // $strobe: one print from the postponed region
    Monitor,    // $monitor: replace the watch; at most one postponed line per slot
    Call,       // push (this prog, pc+1) and transfer to callee
    Fork,       // start each of `branches` as a process; join 0 = all, 1 = any, 2 = none
    ReadMem,    // $readmemh / $readmemb into `arr` (fmt is the file name)
    WriteMem,   // $writememh / $writememb of `arr` (21.5), the same fields as ReadMem
    Force,      // force (hi = 1) or assign (hi = 0) sig = e until released (10.6)
    Release,    // release (hi = 1) or deassign (hi = 0) sig (10.6)
    Disable,    // disable <block> run by other processes: end every process whose root is block `sig`
    WaitFork,   // wait until every child process of this one has ended (9.6.1)
    DisableFork,// end every descendant process of this one (9.6.3)
    Dyn,        // a dynamic array / queue / associative array command: dcmd on handle `sig`
    Udp,        // a user-defined primitive (29): udp's table, inputs in args, state `sig`, last inputs `dres`
    Tran,       // bidirectional switch `sig` (28.8): its control is e (null: always on), active low if pdown
    Dump,       // a VCD task (21.7.1): dcmd is a DumpCmd; args / dsel hold the operands
    Dpi,        // call the C function of DPI import `dpi` (35); see DpiFunc
    Vpi,        // call the calltf of a VPI user system task or function (36-38); see VpiSite
    New,        // a new object of class hi (8.7), or a copy of object e (8.12); its handle to `sig`
    Randomize,  // sig = randomize the object e (null e: std::randomize), with rc (18.6, 18.12)
    RandMode,   // obj e: every rand variable (hi = 0) or constraint block (hi = 1) on or off by de (18.8, 18.9)
    // std::process (9.7): hi is the ProcOp, e the process object (its slot
    // `dkey` holds the process number), lo the class id, sig the result.
    ProcOp,
    // An immediate assertion (16.3, 16.4) of asserts()[sig] on condition e.
    // join 0, simple: pass falls through to the pass action, a fail jumps to
    // target, and an assertion that is off or whose action is off jumps to hi.
    // join 1 (#0) or 2 (final), deferred: queue a report and go on.
    Assert,
    // ##n (14.11): wait for n events of clockings()[dkey] (its event is
    // signal sig); ##0 waits only if that event has not happened in this step.
    CycleWait,
    // A synchronous drive (14.16): bits [hi:lo] of clockvar sig, output dres
    // of clockings()[dkey], take e at the clocking event `de` cycles on.
    CbDrive,
    AssertCtl,  // $assertcontrol (20.11): args are control, assertion and directive type, levels; fmt the targets
    Exit,       // $exit (24.7): end the calling thread's program
    Ret         // pop that address; an empty stack ends the process
};

struct Prog;

// A user-defined primitive's table (29.3.4). Each row has one field per
// input: a level ('0' '1' 'x' '?' 'b'), an edge symbol ('r' 'f' 'p' 'n' '*'),
// or an edge pair "(01)". `edge` is the input with the edge, or -1.
struct UdpRow { std::vector<std::string> in; char state = 0, out = 'x'; int edge = -1; };
struct UdpTable { int nin = 0; bool seq = false; bool edges = false; std::vector<UdpRow> rows; };
// IK::Dump commands, one per VCD task (21.7.1).
enum DumpCmd { DC_FILE, DC_VARS, DC_OFF, DC_ON, DC_ALL, DC_FLUSH, DC_LIMIT };

// Code coverage (msim -cov). The lowerer sets g_emitLoc to the statement it
// is lowering; Prog::emit stamps it on each instruction. When g_cov is on,
// the interpreter counts executions per instruction (and, for JmpIfNot, how
// often the jump was taken: the condition was false).
struct SrcLoc { uint32_t file = 0, line = 0; };
inline SrcLoc g_emitLoc;
inline SrcLoc g_runLoc;   // the statement running now, for run-time error messages
inline std::string runLocStr();
inline std::vector<std::string> g_covFiles{""};   // file id -> path; 0 = unknown
inline bool g_cov = false;
inline std::string runLocStr() {
    if (!g_runLoc.line || g_runLoc.file >= g_covFiles.size()) return "";
    return " (" + g_covFiles[g_runLoc.file] + ":" + std::to_string(g_runLoc.line) + ")";
}
inline uint32_t covFileId(const std::string& path) {
    static std::unordered_map<std::string, uint32_t> ids;
    auto [it, fresh] = ids.try_emplace(path, (uint32_t)g_covFiles.size());
    if (fresh) g_covFiles.push_back(path);
    return it->second;
}
// Condition coverage: one entry per instrumented condition (see
// Op::CondCov). Its counters are g_condHits[base .. base + terms.size()].
struct CondDef {
    uint32_t file = 0, line = 0, col = 0;
    bool isOr = false;
    bool lazy = false;                // the terms run in order until one decides (see condCovLazyEnd)
    std::string text;                 // the whole condition, as written
    std::vector<std::string> terms;   // each term, as written
    size_t base = 0;
};
inline std::vector<CondDef>& condDefs() { static std::vector<CondDef> d; return d; }
// FSM coverage: a state register found by the lowerer (fsm_extract.hpp), its
// states and the transitions its next-state logic can make. The kernel
// records the values the register takes and each change (Sim::covFsm).
struct FsmDef {
    std::string module, inst, name;   // definition, instance path, register
    uint32_t file = 0, line = 0;
    int sig = -1, w = 0;
    bool onehot = false;   // tested bit by bit: only values with one bit set are states
    std::vector<std::pair<uint64_t, std::string>> states;   // value, name
    struct Tr { uint64_t from, to; bool reset; };            // reset: to a state the block's reset branch assigns
    std::vector<Tr> trans;
};
inline std::vector<FsmDef>& fsmDefs() { static std::vector<FsmDef> d; return d; }
// Relaxed atomic add: a task body can run on two worker threads at once.
inline void covHit(uint64_t& c) { __atomic_fetch_add(&c, 1, __ATOMIC_RELAXED); }

// ---- DPI-C (IEEE 1800-2023 clause 35, annex H) ----
// How a value is passed to or from C (35.5.6, H.7). A packed type is passed
// by pointer in canonical form; a scalar bit or logic is one byte.
enum DpiType : uint8_t {
    DT_VOID, DT_BYTE, DT_SHORT, DT_INT, DT_LONG, DT_REAL, DT_SHORTREAL, DT_CHANDLE, DT_STRING,
    DT_BIT,      // svBit
    DT_LOGIC,    // svLogic
    DT_BITVEC,   // svBitVecVal[], 32 bits a word
    DT_LOGICVEC  // svLogicVecVal[], aval/bval pairs
};
struct DpiArg {
    DpiType t = DT_INT;
    int dir = 0;          // 0 input, 1 output, 2 inout
    int sig = -1;         // the formal's signal (not for an unpacked array)
    int w = 32;           // bits of the value, or of one element of an unpacked array
    bool sgn = false;
    // An unpacked array formal: its dimensions, leftmost first. An open
    // dimension `[]` (35.5.6.1) has open = true, and C gets an svOpenArrayHandle.
    bool uarr = false, open = false;
    std::vector<std::pair<int64_t, int64_t>> udims;
    std::vector<bool> uopen;   // per dimension: open

    std::string name;
};
// One DPI import or export. An import is called by IK::Dpi; an export is an
// SV task or function that C calls through a generated stub.
struct DpiFunc {
    std::string svName, cName;
    std::string scope;    // hierarchical name of the module instance or package that declares it
    bool task = false, context = false, pure = false, isExport = false;
    DpiType ret = DT_VOID;
    int retSig = -1, retW = 0;
    bool retSgn = false;
    std::vector<DpiArg> args;
    const Prog* prog = nullptr;   // export: the SV body
    void* rt = nullptr;           // run-time data (resolved C function)
};
// A user system task or function registered with vpi_register_systf
// (38.37). The names are known before elaboration, so slang accepts calls.
struct VpiSystfInfo {
    int idx = 0;          // into vpi::st().systfs
    bool func = false;
    int ftype = 1;        // vpiIntFunc, vpiRealFunc, vpiTimeFunc, vpiSizedFunc, vpiSizedSignedFunc
    int size = 32;        // result width (sizetf for a sized function)
};
inline std::unordered_map<std::string, VpiSystfInfo>& vpiSystfNames() {
    static std::unordered_map<std::string, VpiSystfInfo> m;
    return m;
}
// One argument of a call of a user system task or function, as VPI sees it.
struct VpiArg {
    enum Kind { Empty, Signal, Value, String, Scope } kind = Empty;
    int sig = -1;               // Signal: the variable or net; Value: a temporary holding the value
    int w = 0;
    bool sgn = false, real = false, str = false;
    std::string text;           // String: the literal
    const void* sym = nullptr;  // Signal: its slang symbol; Scope: the instance
    int vtype = 0;              // Value / String: the object type VPI reports (vpiConstant, vpiOperation, ...)
};
// One call site of a user system task or function (an IK::Vpi).
struct VpiSite {
    int systf = 0;
    bool func = false;
    std::vector<VpiArg> args;
    int ret = -1, retW = 32;    // function: the result signal
    bool retSgn = false, retReal = false;
    std::string file;
    int line = 0;
    const void* scope = nullptr;   // the instance the call is in (slang InstanceSymbol)
    bool prot = false;             // the call is in protected IP (37.3.6, IEEE 1735 9.7.1)
    mutable void* userdata = nullptr;   // vpi_put_userdata (38.40)
};
// An unpacked array actual of a DPI call: a fixed array or a container.
struct DpiActual {
    std::shared_ptr<const ArrShape> arr;
    int dyn = -1;
};

struct Instr {
    IK k = IK::Jmp;
    int sig = -1;
    E e;
    Edge edge = ANY;
    size_t target = 0;
    // JmpIfNot: where to go when the condition is unknown (x), if not `target` (msim -xprop).
    static constexpr size_t NOX = (size_t)-1;
    size_t xt = NOX;
    uint64_t delay = 0;
    int drv = -1;         // Assign from a continuous driver: driver id on a resolved net
    int gate = -1;        // a gate or UDP output driver: its gateRecs() entry (SDF DEVICE, 32.4.1)
    uint8_t s0 = STR_STRONG, s1 = STR_STRONG;  // that driver's 0-strength and 1-strength
    int hi = -1, lo = 0;  // Assign/Nba: hi >= 0 means partial write sig[hi:lo]
    std::vector<std::pair<int, Edge>> events;  // WaitEdge with 2+ events: @(a or b ...)
    // Display/Strobe/Monitor. `args` are read when the line is printed, so a
    // $strobe/$monitor sees values after NBA. %b, %d, %%, and other text.
    std::string fmt;
    bool nonl = false;    // Display from $write: no newline at the end
    E fd;                 // Strobe / Monitor from $fstrobe / $fmonitor: the descriptor (21.3.2)
    std::vector<E> args;
    const Prog* callee = nullptr; // Call
    std::vector<const Prog*> branches;  // Fork
    int join = 0;                       // Fork: 0 join, 1 join_any, 2 join_none
    // Assign/Nba to an array element chosen at run time: the target is
    // arr->base + offset(aidx). An invalid index skips the write (7.4.6).
    // ReadMem: arr is the memory, args hold [start [, finish]], hex selects $readmemh.
    std::shared_ptr<const ArrShape> arr;
    std::vector<E> aidx;
    // Assign/Nba to a bit chosen at run time (v[i] = ...): hi = lo = bidx.
    // With pw > 0 it is v[bidx +: pw] (or -: when pdown).
    E bidx;
    // WaitDelay: `de`, when set, is the delay in ticks, computed at run time.
    // Nba with nbaLater: the update happens `delay` / `de` ticks later (9.4.5).
    E de;
    bool nbaLater = false;
    // Dyn: the command, its operands in args, the value in e, another
    // container dsrc, a result signal dres, and the key variable dkey.
    // Assign with dyn >= 0 writes element args[0] of container dyn.
    int dcmd = 0, dsrc = -1, dres = -1, dkey = -1, dyn = -1;
    // A driver (drv >= 0) with a delay (10.3.3, 28.16): 1 to 3 values in
    // ticks, rise / fall / turn-off. Updates are inertial.
    std::vector<uint64_t> dly;
    int pw = 0;
    bool pdown = false;
    bool hex = false;
    // Assign to a temporary only this process uses (msim -xprop): nothing
    // watches it, so the value is stored at once (Sim::poke).
    bool tmp = false;
    const UdpTable* udp = nullptr;   // Udp
    // Dump with DC_VARS: the scopes and variables named by $dumpvars, as
    // signal-name prefixes relative to the top ("" is the top, "u1." an
    // instance); `second` is true for a variable (an exact name).
    std::vector<std::pair<std::string, bool>> dsel;
    // Frames (expr.hpp). Call: when `lay` is set the callee is automatic and
    // runs in a new frame of that shape; `init` gives each argument slot its
    // value, evaluated in the caller before the switch. Fork: `blays` is the
    // frame shape of each branch, and `lay` with `init` a frame for the fork
    // block's own automatic variables, shared by its branches (9.3.2).
    const FrameLayout* lay = nullptr;
    std::vector<std::pair<int, E>> init;
    std::vector<const FrameLayout*> blays;
    // Call: containers copied into the new frame (7.7): its slot, the
    // caller's container signal.
    std::vector<std::pair<int, int>> dinit;
    // WaitEdge with dynWait: wait for a change of any signal `e` reads,
    // resolved when the wait starts (wait (cond) on automatic variables).
    bool dynWait = false;
    uint32_t file = 0, line = 0;            // source of the statement (coverage)
    mutable uint64_t hits = 0, taken = 0;   // -cov counters
    // Dpi: the import, one entry per argument (arrays only), and the call's
    // source location for svGetCallerInfo (file in fmt).
    const DpiFunc* dpi = nullptr;
    const VpiSite* vpi = nullptr;   // Vpi: the call site
    std::vector<DpiActual> dact;
    int dline = 0;
    // Call of a class method (8): `self` is the object (null: the caller's
    // own `this`; noSelf: a static method). vslot >= 0 picks the body from
    // the object's class (a virtual method, 8.20).
    E self;
    bool noSelf = false;
    int vslot = -1;
    std::shared_ptr<const struct RandCall> rc;   // Randomize
};

// ---------- constrained randomization (18) ----------
// One range of a dist (18.5.3): [lo, hi] (hi null: the single value lo),
// with its weight, := per value or :/ shared by the range.
struct RandDist { E lo, hi, weight; bool perRange = false; };
// One constraint, a truth value; or, when isDist, `e dist {...}`.
struct RandItem {
    E e;
    bool soft = false;           // 18.5.13
    bool disableSoft = false;    // disable soft e (18.5.13.2): drops the earlier soft constraints on e
    bool isDist = false;
    E guard;                     // dist under an implication: holds when guard is false
    std::vector<RandDist> dist;
    E distDefault;               // default :/ w (18.5.3): w spread over the values no item names
    // foreach over a dynamic array or queue (18.5.8.1): body once per
    // element at solve time, with signal loopVar read as the index.
    int loopCont = -1, loopVar = -1;
    std::vector<RandItem> body;
    // Function calls in the constraint (18.5.12): each program computes one
    // call's result into the temporary that e reads. They run at solve time
    // once the rand variables they read have values.
    std::vector<std::shared_ptr<Prog>> fns;
    // m[i] of a container of containers (7.5, 18.4): the indirect id the
    // constraint uses for it, and the DynElemRef that names it, resolved at
    // solve time with the loop indices of that moment.
    std::vector<std::pair<int, E>> refs;
    // unique {q, x} with a dynamic array, queue or associative array in it
    // (18.5.5): every element of those containers and every uniqElems value
    // differ, pairwise, made at solve time when the sizes are known.
    std::vector<int> uniqConts;
    std::vector<E> uniqElems;
};
// A constraint block (18.5): its constraint_mode slot in the object.
struct RandBlock {
    std::string name;
    int mode = -1;
    bool staticMode = false;   // a static block (18.5.10): mode is a signal holding "off" as 0 (x or 1: on)
    std::vector<RandItem> items;
    std::vector<std::pair<std::vector<E>, std::vector<E>>> before;   // solve a before b (18.5.9)
};
// A rand / randc property (18.4): `count` slots from `off`, each w bits;
// `mode` is its rand_mode slot. obj: a rand class handle, whose object is
// randomized with this one.
struct RandVar {
    std::string name;
    int off = 0, count = 1, w = 1, mode = -1;
    bool sgn = false, randc = false, obj = false;
    bool dyn = false;   // a dynamic array or queue: off is its container slot, w / sgn its elements'
    int depth = 1;      // containers of containers (int m[][]): how many levels
};
struct RandSpec {
    std::vector<RandVar> vars;
    std::vector<std::shared_ptr<const RandBlock>> blocks;   // own and inherited; an override by name replaces the base block
    const Prog* pre = nullptr; const FrameLayout* preLay = nullptr;     // pre_randomize (18.6.2)
    const Prog* post = nullptr; const FrameLayout* postLay = nullptr;   // post_randomize
};
// A randomize() call: with {...} constraints, read in the caller's frame
// (the object's members through objTmp), and randomize(a, b) arguments.
struct RandCall {
    std::vector<RandItem> items;
    std::vector<std::pair<std::vector<E>, std::vector<E>>> before;
    std::vector<E> args;        // the variables to randomize, as Refs
    bool argsGiven = false;     // randomize(...) with arguments (18.11): only those
    int objTmp = -1;
    // Container arguments (randomize(q), 18.11): the container, its
    // innermost element width and sign, and its levels (int m[][] is 2).
    struct ContArg { int sig; int w; bool sgn; int depth; E addr; };   // addr: an ArrAddr giving id + 1, read in the object
    std::vector<ContArg> contArgs;
};
struct Prog;
// A class at run time (8): its object shape (slot 0 holds the class id),
// its virtual method table, and its base class.
struct ClassRt {
    std::string name;
    const FrameLayout* lay = nullptr;
    int base = -1;
    std::vector<std::pair<const Prog*, const FrameLayout*>> vt;
    // Interface classes (8.26) this class implements, and for each
    // interface method k (global numbering) the vt slot of its
    // implementation (-1: none).
    std::vector<int> ifs;
    std::vector<int> imap;
    std::shared_ptr<RandSpec> rand;   // 18
};
inline std::vector<ClassRt>& classRt() { static std::vector<ClassRt> v; return v; }
inline bool classIsA(int cls, int want) {
    for (; cls >= 0; cls = classRt()[(size_t)cls].base) {
        if (cls == want) return true;
        for (int k : classRt()[(size_t)cls].ifs)
            if (k == want) return true;
    }
    return false;
}

// Power-aware simulation (IEEE 1801-2024 9.5): per power domain, the
// simstate its primary supply set applies (upf::Simstate: 0 NORMAL ... 5
// CORRUPT), and a signal that changes when the domain powers up. A process
// with Prog::pd >= 0 is in that domain (upf_rt.inc).
inline bool g_upf = false;
inline std::vector<int> g_pdMode, g_pdSig;

struct Prog {
    int pd = -1;           // UPF power domain, -1 for none (initial blocks are never gated, 9.2.1)
    bool pstate = false;   // UPF: this process models state elements (an edge-triggered block or a latch)
    std::vector<Instr> code;
    // msim -xprop, a clocked block with a clean copy: code[1] tests for an
    // ambiguous clock edge and otherwise jumps here. A clocked function
    // starts here at once in a time step where no signal has changed from
    // or to x (Sim::xAt). 0: none.
    size_t xpFast = 0;
    // Named blocks this process runs as a whole (an initial's named body, a
    // fork branch that is a named block, every branch of a named fork).
    // `disable` of one of them from another process ends the process.
    std::vector<int> roots;
    // Named blocks and tasks some `disable` names (9.6.2): code [s, e) is
    // block id, and a process there resumes at e when it is disabled. A
    // task's whole body is one span with e = SIZE_MAX: its caller resumes
    // after the call.
    struct Span { int id; size_t s, e; };
    std::vector<Span> spans;
    // A static combinational process with deferred assertions (a unique
    // case's violation check): the ProcCtl its reports are queued under.
    // Each evaluation is a flush point (16.4.2), as the wait of the process is.
    mutable std::shared_ptr<void> combCtl;
    size_t emit(Instr i) {
        if (!i.line) { i.file = g_emitLoc.file; i.line = g_emitLoc.line; }
        code.push_back(i);
        return code.size() - 1;
    }
    size_t here() const { return code.size(); }
};

// Some Prog has spans: processes are tracked for disable (9.6.2).
inline bool& anySpans() { static bool b = false; return b; }

// ---- helpers shared by lowering and tests ----
// Signals an expression reads, each once, in first-read order. A variable
// array index reads every element, so a wait on it wakes on any of them.
inline void collectRefsInto(const E& e, std::vector<int>& out, std::unordered_set<int>& seen) {
    if (!e) return;
    // A relative id (an automatic variable) has no fixed signal: it is left
    // out here, and a wait on one resolves it at run time (Instr::dynWait).
    auto add = [&](int s) { if (!isRel(s) && seen.insert(s).second) out.push_back(s); };
    if (e->op == Op::Ref && !e->at) add(e->sig);   // h.x has no fixed signal either
    collectRefsInto(e->at, out, seen);
    if (e->op == Op::ArrRead)
        for (int i = 0; i < e->arr->count; i++) add(e->arr->base + i);
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce || e->op == Op::DynBits || e->op == Op::DynHas || e->op == Op::DynElemRef) add(e->sig);
    if (e->op == Op::SysFn && e->sig >= 0) add(e->sig);   // a query of a container (SF_ASSOC_BOUND)
    for (auto& k : e->kids) collectRefsInto(k, out, seen);
}
// Does e read an automatic variable (a relative id)?
inline bool hasRel(const E& e) {
    if (!e) return false;
    if (isRel(e->sig) && e->op != Op::Const) return true;
    if (e->at || e->op == Op::This || e->op == Op::Addr || e->op == Op::AddrOf || e->op == Op::VifSig || e->op == Op::DynElemRef) return true;   // objects (8), ref (13.5.2), virtual interfaces (25.9)
    if (e->op == Op::ArrRead && isRel(e->arr->base)) return true;
    for (auto& k : e->kids) if (hasRel(k)) return true;
    return false;
}
inline void collectRefs(const E& e, std::vector<int>& out) {
    std::unordered_set<int> seen(out.begin(), out.end());
    collectRefsInto(e, out, seen);
}
// Signals read by this prog and by any prog it calls. @* / always_comb must see
// a function body, or a call would hide the signals that should wake it.
inline void collectProgRefsInto(const Prog& p, std::vector<int>& sigs, std::unordered_set<int>& have,
                                std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        collectRefsInto(i.e, sigs, have);
        for (auto& a : i.args) collectRefsInto(a, sigs, have);  // $display / $strobe / $monitor
        for (auto& a : i.aidx) collectRefsInto(a, sigs, have);  // a[i] = ... reads i
        if (i.k == IK::Dyn || i.dyn >= 0)
            for (auto& a : i.args) collectRefsInto(a, sigs, have);
        collectRefsInto(i.bidx, sigs, have);
        for (auto& a : i.init) collectRefsInto(a.second, sigs, have);
        if (i.k == IK::Call && i.callee) collectProgRefsInto(*i.callee, sigs, have, seen);
        for (const Prog* b : i.branches) collectProgRefsInto(*b, sigs, have, seen);
    }
}
inline void collectProgRefs(const Prog& p, std::vector<int>& sigs, std::vector<const Prog*>& seen) {
    std::unordered_set<int> have(sigs.begin(), sigs.end());
    collectProgRefsInto(p, sigs, have, seen);
}
inline std::vector<std::pair<int, Edge>> readSet(const Prog& p) {
    std::vector<int> sigs;
    std::vector<const Prog*> seen;
    collectProgRefs(p, sigs, seen);
    std::vector<std::pair<int, Edge>> ev;
    for (int s : sigs) ev.push_back({s, ANY});
    return ev;
}
// Signals a process (or a subroutine it calls) writes whole: an assignment
// without a part select, or any element of an array written at a run-time
// index. always_comb leaves these out of its sensitivity (9.2.2.2.1 b).
inline void collectProgWrites(const Prog& p, std::unordered_set<int>& out, std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        if ((i.k == IK::Assign || i.k == IK::Nba) && i.sig >= 0 && i.drv < 0 && i.dyn < 0) {
            if (i.arr) for (int k = 0; k < i.arr->count; k++) out.insert(i.arr->base + k);
            else if (i.hi < 0 && !i.bidx) out.insert(i.sig);
        }
        if (i.k == IK::Call && i.callee) collectProgWrites(*i.callee, out, seen);
        for (const Prog* b : i.branches) collectProgWrites(*b, out, seen);
    }
}
// Bits of a signal: ranges [lo, hi], or every bit.
struct BitSet {
    bool all = false;
    std::vector<std::pair<int, int>> r;
    bool covers(int lo, int hi) const {   // is every bit of [lo, hi] in r?
        if (all) return true;
        for (int b = lo; b <= hi; b++) {
            bool in = false;
            for (auto& [l, h] : r) if (b >= l && b <= h) { in = true; break; }
            if (!in) return false;
        }
        return true;
    }
};
// The bits of each signal that e reads: a constant part select of a signal
// reads only those bits, any other reference reads all of it.
inline void collectReadBits(const E& e, std::unordered_map<int, BitSet>& m) {
    if (!e) return;
    if (e->op == Op::Slice && e->kids.size() == 1 && e->kids[0] && e->kids[0]->op == Op::Ref &&
        !e->kids[0]->at && !isRel(e->kids[0]->sig)) {
        m[e->kids[0]->sig].r.push_back({e->lo, e->hi});
        return;
    }
    if (e->op == Op::Ref && !e->at && !isRel(e->sig)) m[e->sig].all = true;
    if (e->op == Op::ArrRead)
        for (int i = 0; i < e->arr->count; i++) m[e->arr->base + i].all = true;
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce ||
        e->op == Op::DynBits || e->op == Op::DynHas || e->op == Op::DynElemRef)
        m[e->sig].all = true;
    collectReadBits(e->at, m);
    for (auto& k : e->kids) collectReadBits(k, m);
}
inline void collectProgReadBits(const Prog& p, std::unordered_map<int, BitSet>& m, std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        collectReadBits(i.e, m);
        for (auto& a : i.args) collectReadBits(a, m);
        for (auto& a : i.aidx) collectReadBits(a, m);
        collectReadBits(i.bidx, m);
        for (auto& a : i.init) collectReadBits(a.second, m);
        if (i.k == IK::Call && i.callee) collectProgReadBits(*i.callee, m, seen);
        for (const Prog* b : i.branches) collectProgReadBits(*b, m, seen);
    }
}
// Bits written at a constant part select (v[3] = ..., v[7:4] = ...).
inline void collectProgPartWrites(const Prog& p, std::unordered_map<int, BitSet>& out, std::vector<const Prog*>& seen) {
    for (const Prog* s : seen)
        if (s == &p) return;
    seen.push_back(&p);
    for (auto& i : p.code) {
        if ((i.k == IK::Assign || i.k == IK::Nba) && i.sig >= 0 && i.drv < 0 && i.dyn < 0 && !i.arr &&
            i.hi >= 0 && !i.bidx)
            out[i.sig].r.push_back({i.lo, i.hi});
        if (i.k == IK::Call && i.callee) collectProgPartWrites(*i.callee, out, seen);
        for (const Prog* b : i.branches) collectProgPartWrites(*b, out, seen);
    }
}
// always_comb / always_latch sensitivity (9.2.2.2.1): what the block reads,
// less what it also writes. A signal written only in part stays in the list
// unless every read of it is of bits the block writes, so two blocks that
// each own a bit of one vector do not wake each other.
inline std::vector<std::pair<int, Edge>> combReadSet(const Prog& p) {
    auto rs = readSet(p);
    std::unordered_set<int> wr;
    std::vector<const Prog*> seen;
    collectProgWrites(p, wr, seen);
    std::erase_if(rs, [&](const std::pair<int, Edge>& e) { return wr.count(e.first) > 0; });
    std::unordered_map<int, BitSet> pw;
    seen.clear();
    collectProgPartWrites(p, pw, seen);
    if (pw.empty()) return rs;
    std::unordered_map<int, BitSet> rd;
    seen.clear();
    collectProgReadBits(p, rd, seen);
    std::erase_if(rs, [&](const std::pair<int, Edge>& e) {
        auto w = pw.find(e.first);
        if (w == pw.end()) return false;
        auto r = rd.find(e.first);
        if (r == rd.end() || r->second.all) return false;
        for (auto& [l, h] : r->second.r) if (!w->second.covers(l, h)) return false;
        return true;
    });
    return rs;
}
// A WaitEdge on one or more events (ev must be non-empty).
inline Instr waitInstr(const std::vector<std::pair<int, Edge>>& ev) {
    Instr i;
    i.k = IK::WaitEdge;
    i.events = ev;
    i.sig = ev[0].first;
    i.edge = ev[0].second;
    return i;
}

// ---- static combinational schedule ----
// A process with this shape is pure combinational logic:
//     Assign ... Assign ; WaitEdge (every event ANY) ; Jmp 0
// i.e. a continuous assign, a gate, or an always_comb / always @* whose body
// is assignments and jumps within it (if, case, for).
// Such a process can run as a plain function, levelized like Verilator and
// VCS do: the kernel keeps the functions in one topological order and, when a
// signal changes, runs the dependent ones in that order once each, without a
// coroutine, a waiter list, or an event per process. `ins` is the wait list
// (exactly what the process would have waited on); `outs` are the signals it
// assigns.
struct CombShape {
    std::vector<int> ins, outs;
    size_t body = 0;  // instructions before the wait: Assigns, and jumps within them when `branches`
    bool branches = false;
    bool asserts = false;   // deferred immediate assertions in the body
};
inline bool combShape(const Prog& p, CombShape& c) {
    size_t n = p.code.size();
    if (n < 3) return false;
    const Instr& w = p.code[n - 2];
    const Instr& j = p.code[n - 1];
    if (j.k != IK::Jmp || j.target != 0 || w.k != IK::WaitEdge) return false;
    if (!p.roots.empty() || !p.spans.empty()) return false;  // another process can `disable` it
    c = CombShape{};
    for (size_t i = 0; i + 2 < n; i++) {
        // An always_comb with if / case / loops: jumps that stay in the body.
        if (p.code[i].k == IK::JmpIfNot || p.code[i].k == IK::Jmp) {
            if (p.code[i].target > n - 2) return false;
            if (p.code[i].xt != Instr::NOX && p.code[i].xt > n - 2) return false;
            c.branches = true;
            continue;
        }
        // A deferred immediate assertion (assert #0 / final, a unique or
        // priority case's check): queues a report and goes on.
        if (p.code[i].k == IK::Assert && (p.code[i].join == 1 || p.code[i].join == 2)) {
            c.asserts = c.branches = true;
            continue;
        }
        if (p.code[i].k != IK::Assign) return false;
        // A target chosen at run time (a[i] = ..., v[i] = ...) has no fixed output signal.
        // A bit or part select chosen at run time (v[i] = ..., v[i +: 4] = ...)
        // still writes one known signal; an array element (a[i] = ...) does not.
        if (p.code[i].arr || !p.code[i].aidx.empty()) return false;
        if (p.code[i].bidx && p.code[i].drv >= 0) return false;
        if (p.code[i].de || p.code[i].delay || !p.code[i].dly.empty()) return false;
        if (p.code[i].tmp) continue;   // a temporary of its own: not an output
        bool seen = false;
        for (int o : c.outs) seen |= o == p.code[i].sig;
        if (!seen) c.outs.push_back(p.code[i].sig);
    }
    c.body = n - 2;
    if (w.events.size() > 1) {
        for (auto& [s, e] : w.events) {
            if (e != ANY) return false;
            c.ins.push_back(s);
        }
    } else {
        if (w.edge != ANY) return false;
        c.ins.push_back(w.sig);
    }
    return true;
}
// Topological order of the given combinational processes (indices into cs).
// A process is never woken by its own writes (it is running, not waiting),
// so a self-dependency is not an edge. Processes on a cycle are kept
// together (see the components below; *cyclic says there are some).
// The order is sorted by level (longest path from an input that no process
// in the set drives), stable within a level, so ranks of one level never
// feed each other and can be computed in parallel. `levels` (optional)
// receives the level of each entry of the returned order.
inline std::vector<int>* g_combCycles = nullptr;   // out: per process, its cycle (component) or -1
inline std::vector<size_t> combSchedule(const std::vector<CombShape>& cs, std::vector<int>* levels = nullptr, bool* cyclic = nullptr) {
    size_t n = cs.size();
    std::vector<std::vector<size_t>> succ(n);
    std::vector<size_t> indeg(n, 0);
    std::unordered_map<int, std::vector<size_t>> readers;  // signal -> processes waiting on it
    for (size_t b = 0; b < n; b++)
        for (int i : cs[b].ins) readers[i].push_back(b);
    for (size_t a = 0; a < n; a++) {
        std::vector<size_t> to;
        for (int o : cs[a].outs) {
            auto it = readers.find(o);
            if (it == readers.end()) continue;
            for (size_t b : it->second)
                if (b != a) to.push_back(b);
        }
        std::sort(to.begin(), to.end());
        to.erase(std::unique(to.begin(), to.end()), to.end());
        for (size_t b : to) { succ[a].push_back(b); indeg[b]++; }
    }
    // Strongly connected components (Tarjan, without recursion). Processes
    // on a cycle (block A reads what block B writes and the other way round,
    // usually through different bits or branches) are scheduled as one unit:
    // consecutive ranks, each on a level of its own. A write marks the ranks
    // that read the signal, lower ones too, and the kernel always runs the
    // lowest pending rank, so the unit is evaluated until it is stable, as
    // the event-driven processes would be.
    std::vector<int> idx(n, -1), low(n, 0), comp(n, -1);
    std::vector<size_t> rep(n, 0);
    std::vector<std::vector<size_t>> members;
    {
        std::vector<char> on(n, 0);
        std::vector<size_t> st;
        std::vector<std::pair<size_t, size_t>> call;   // node, next successor
        int next = 0;
        for (size_t r = 0; r < n; r++) {
            if (idx[r] >= 0) continue;
            call.push_back({r, 0});
            while (!call.empty()) {
                size_t v = call.back().first;
                if (call.back().second == 0) { idx[v] = low[v] = next++; st.push_back(v); on[v] = 1; }
                if (call.back().second < succ[v].size()) {
                    size_t w = succ[v][call.back().second++];
                    if (idx[w] < 0) call.push_back({w, 0});
                    else if (on[w]) low[v] = std::min(low[v], idx[w]);
                    continue;
                }
                if (low[v] == idx[v]) {
                    members.emplace_back();
                    for (;;) {
                        size_t w = st.back(); st.pop_back(); on[w] = 0;
                        comp[w] = (int)members.size() - 1;
                        members.back().push_back(w);
                        if (w == v) break;
                    }
                    std::sort(members.back().begin(), members.back().end());
                }
                call.pop_back();
                if (!call.empty()) low[call.back().first] = std::min(low[call.back().first], low[v]);
            }
        }
    }
    for (size_t a = 0; a < n; a++) rep[a] = members[(size_t)comp[a]][0];
    if (g_combCycles) {   // msim's MSIM_WHYCYCLE report
        g_combCycles->assign(n, -1);
        for (size_t a = 0; a < n; a++) if (members[(size_t)comp[a]].size() > 1) (*g_combCycles)[a] = comp[a];
    }
    if (cyclic) {
        *cyclic = false;
        for (auto& m : members) *cyclic |= m.size() > 1;
    }
    // Kahn's order over the components; the smallest first index among the
    // ready ones keeps the order stable (and, without cycles, what it was).
    std::vector<size_t> cdeg(members.size(), 0);
    for (size_t a = 0; a < n; a++)
        for (size_t b : succ[a])
            if (comp[a] != comp[b]) cdeg[(size_t)comp[b]]++;
    std::vector<size_t> order;
    std::vector<int> lv(n, 0);
    std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
    for (size_t c = 0; c < members.size(); c++)
        if (!cdeg[c]) ready.push(members[c][0]);
    while (!ready.empty()) {
        size_t r = ready.top();
        ready.pop();
        const auto& m = members[(size_t)comp[r]];
        int base = 0;
        for (size_t a : m) base = std::max(base, lv[a]);
        for (size_t k = 0; k < m.size(); k++) { lv[m[k]] = base + (int)k; order.push_back(m[k]); }
        for (size_t a : m)
            for (size_t b : succ[a]) {
                if (comp[a] == comp[b]) continue;
                if (lv[b] < lv[a] + 1) lv[b] = lv[a] + 1;
                if (--cdeg[(size_t)comp[b]] == 0) ready.push(rep[b]);
            }
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return lv[a] < lv[b]; });
    if (levels) {
        levels->clear();
        for (size_t a : order) levels->push_back(lv[a]);
    }
    return order;
}

// A clocked process that only schedules nonblocking assignments:
//     WaitEdge (fixed events) ; { Nba | JmpIfNot | Jmp } ... ; Jmp 0
// with every jump inside the body. It may also assign temporaries of its
// own (Instr::tmp, msim -xprop). Typical always @(posedge clk) RTL. It
// reads signals and writes nothing until the NBA region, so all such
// processes triggered in one slot can run at the same time.
inline bool seqShape(const Prog& p, std::vector<std::pair<int, Edge>>& ev) {
    size_t n = p.code.size();
    if (n < 3) return false;
    const Instr& w = p.code[0];
    const Instr& j = p.code[n - 1];
    if (w.k != IK::WaitEdge || j.k != IK::Jmp || j.target != 0) return false;
    if (!p.roots.empty() || !p.spans.empty()) return false;  // another process can `disable` it
    for (size_t i = 1; i + 1 < n; i++) {
        const Instr& x = p.code[i];
        if (x.k == IK::Nba) {
            if (x.drv >= 0 || x.nbaLater || x.de || x.delay) return false;  // a <= #d b goes through the kernel
        } else if (x.k == IK::JmpIfNot || x.k == IK::Jmp) {
            if (x.target < 1 || x.target > n - 1) return false;
            if (x.xt != Instr::NOX && (x.xt < 1 || x.xt > n - 1)) return false;
        } else if (x.k == IK::Assign && x.tmp) {
        } else
            return false;
    }
    ev.clear();
    if (w.events.size() > 1) ev = w.events;
    else ev.push_back({w.sig, w.edge});
    return true;
}

// The NBA targets of a seqShape process that are fixed at compile time.
inline void seqTargets(const Prog& p, std::vector<int>& out) {
    for (const Instr& i : p.code)
        if (i.k == IK::Nba && !i.arr && i.aidx.empty() && !i.bidx) out.push_back(i.sig);
}
