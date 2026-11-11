// mhls: high-level synthesis. Untimed C/C++ in, synthesizable SystemVerilog out.
//
//   mhls fir.c -top fir -o fir.sv                    FSM + datapath
//   mhls fir.c -top fir -pipeline -clock 5           II=1 pipeline
//   mhls fir.c -top fir -tb fir_tb.sv -vectors 100   self-checking testbench for msim
//   mhls fir.c -top fir -cref fir_ref.cpp            the same vectors against the host compiler
//   mhls fir.c -top fir -explore 2,5,10,20           latency/area over clock periods
//   mhls fir.cpp -clang -top fir4                    C++ classes and templates, read by clang
//
// Flow: parse (hls_front.hpp), lower to a control/data-flow graph with
// inlining, if-conversion, loop unrolling and array scalarization
// (hls_ir.hpp), then here: schedule each block into clock cycles with
// operator chaining under the clock period and resource limits, bind
// values to shared registers and multipliers/dividers to shared units,
// and emit an FSM with datapath or a feed-forward pipeline.
//
// Options
//   -top <f>[,<g>]    functions to synthesize (default: "#pragma hls top", else the last one)
//   -o <file|dir>     output (default <top>.sv; a directory when there are several tops)
//   -clock <ns>       clock period for scheduling (default 10)
//   -pipeline         pipeline the function, one new input per cycle (also "#pragma hls pipeline")
//   -unroll-all       unroll every loop with a constant trip count (also "#pragma hls unroll")
//   -pipeline-loops   start a new iteration every cycle in every loop that allows it
//                     (also "#pragma hls pipeline" first in a loop body, which insists)
//   -unroll-max <n>   most iterations a loop may unroll to (default 4096)
//   -mul <n>, -div <n>  at most n multipliers / dividers working in one cycle (default: no limit)
//   -no-share         one multiplier/divider per operation, one register per value
//   -handshake        also write <top>_hs: the same block behind ready/valid ports
//   -max-reg-array <n>  local arrays up to n elements become registers, larger ones RAMs (default 64)
//   -delay-scale <x>  scale the built-in operator delays (default 1.0, a generic 28 nm-class library)
//   -tb <file>        write a self-checking testbench with expected values from the reference run
//   -cref <file>      write a C++ program that checks the same vectors with the host compiler
//   -vectors <n>, -seed <n>   number of random calls (default 20) and the seed (default 1)
//   -explore <ns,...> schedule at each clock period and print a table instead of writing RTL
//   -report <file>    write the summary as JSON
//   -D<name>[=<v>]    define a macro
//   -threads <n>      worker threads, or all (default: every hardware thread)
//   -q                no summary on stdout
#include <cmath>
#include <filesystem>
#include <iostream>
#include <regex>

#include "parallel.hpp"
#include "hls_ir.hpp"
#include "hls_clang.hpp"

using namespace hls;

namespace {

struct Cfg {
  double clock = 10.0, scale = 1.0;
  int mulLimit = 0, divLimit = 0;
  bool share = true;
  bool handshake = false;   // also emit <top>_hs with ready/valid ports
};

// Threads come from the shared src/parallel.hpp. The wrapper only makes a
// failure deterministic: the first error in index order is the one reported.
unsigned g_threads = 1;
template <class F>
void parallelFor(size_t n, F fn) {
  std::vector<std::exception_ptr> errs(n);
  par::parallelFor(n, g_threads, [&](size_t i) {
    try {
      fn(i);
    } catch (...) {
      errs[i] = std::current_exception();
    }
  });
  for (auto& e : errs)
    if (e) std::rethrow_exception(e);
}

// ---- timing and area model ---------------------------------------------------
// Delays in ns for a generic 28 nm-class standard-cell library. They only
// decide where cycle boundaries fall; msynth and msta give the real numbers.
const double kTcq = 0.10, kTsu = 0.05, kMemSu = 0.20, kMemQ = 0.45;

bool isMulFu(const Function& f, const Op& o) {
  return o.k == OpK::Mul && f.ops[o.a].k != OpK::Const && f.ops[o.b].k != OpK::Const;
}
bool isDiv(OpK k) { return k == OpK::UDiv || k == OpK::SDiv || k == OpK::URem || k == OpK::SRem; }

double opDelay(const Function& f, const Op& o, double scale) {
  double lg = std::log2((double)std::max(o.w, 2));
  double d = 0;
  switch (o.k) {
    case OpK::Add: case OpK::Sub: d = 0.10 + 0.12 * lg; break;
    case OpK::ULt: case OpK::ULe: case OpK::SLt: case OpK::SLe:
      d = 0.10 + 0.12 * std::log2((double)std::max(f.ops[o.a].w, 2));
      break;
    case OpK::Eq: case OpK::Ne: d = 0.08 + 0.05 * std::log2((double)std::max(f.ops[o.a].w, 2)); break;
    case OpK::Mul: d = isMulFu(f, o) ? 0.30 + 0.35 * lg : 0.15 + 0.20 * lg; break;
    case OpK::UDiv: case OpK::SDiv: case OpK::URem: case OpK::SRem: d = 0.30 * o.w; break;
    case OpK::And: case OpK::Or: case OpK::Xor: d = f.ops[o.b].k == OpK::Const ? 0.03 : 0.06; break;
    case OpK::Mux: d = 0.10; break;
    case OpK::Shl: case OpK::LShr: case OpK::AShr: d = f.ops[o.b].k == OpK::Const ? 0 : 0.10 + 0.10 * lg; break;
    case OpK::RomRd: d = 0.20 + 0.06 * f.arrs[o.ref].aw(); break;
    default: d = 0; break;
  }
  return d * scale;
}

// Area in two-input NAND equivalents, for comparing schedules.
double opArea(const Function& f, const Op& o) {
  double w = o.w, lg = std::log2((double)std::max(o.w, 2));
  switch (o.k) {
    case OpK::Add: case OpK::Sub: return f.ops[o.b].k == OpK::Const ? 4 * w : 9 * w;
    case OpK::ULt: case OpK::ULe: case OpK::SLt: case OpK::SLe: return 5.0 * f.ops[o.a].w;
    case OpK::Eq: case OpK::Ne: return 2.5 * f.ops[o.a].w;
    case OpK::Mul: return isMulFu(f, o) ? 6.0 * w * w : 4.5 * w * lg;
    case OpK::UDiv: case OpK::SDiv: case OpK::URem: case OpK::SRem: return 11.0 * w * w;
    case OpK::And: case OpK::Or: return f.ops[o.b].k == OpK::Const ? 0 : 1.3 * w;
    case OpK::Xor: return f.ops[o.b].k == OpK::Const ? 0.7 * w : 2.5 * w;
    case OpK::Mux: return 2.3 * w;
    case OpK::Shl: case OpK::LShr: case OpK::AShr: return f.ops[o.b].k == OpK::Const ? 0 : 2.3 * w * lg;
    case OpK::RomRd: return 0.4 * w * f.arrs[o.ref].size;
    default: return 0;
  }
}

// ---- scheduling and binding ----------------------------------------------------

struct Fu {
  OpK k;
  int w = 0;
  std::vector<int> users;
};

struct Sched {
  std::vector<Fu> fus;
  std::map<int, int> temps;   // width -> shared temp registers
  int nStates = 1;
  int multicycle = 0;
  std::vector<bool> entryLive;
  long latMin = -1, latMax = -1;   // -1: depends on the data
  double area = 0;
  int regBits = 0, regs = 0, pipeRegBits = 0;
  int pipedLoops = 0;
  std::vector<std::string> loopNotes;   // what became of each loop that was to be pipelined
  std::map<std::string, int> opCount;
};

int availSt(const Op& o) {
  if (o.k == OpK::Const) return 0;
  if (o.k == OpK::ReadVar) return o.st;   // 0, but for a variable a pipelined loop updates
  if (o.k == OpK::Load) return o.st + 1;
  return o.st + o.cyc - 1;
}

// List scheduling in dependence order: every operator goes into the earliest
// state where its operands are available, chained after them when the
// combinational path still fits in the clock period.
//
// mode 0: an FSM block. mode 1: the one block of a pipelined function.
// mode 2: the body of a pipelined loop; readStage gives the stage in which
// each variable the loop updates is read (and so must be written).
void scheduleBlock(Function& f, Block& B, const Cfg& cfg, int mode, std::map<int, int>& temps,
                   const std::map<int, int>* readStage = nullptr) {
  const bool pipeline = mode != 0;
  const double budget = cfg.clock - kTsu * cfg.scale, tcq = kTcq * cfg.scale;
  std::map<int, int> lastAcc;                       // array -> state of its last access
  std::map<std::pair<int, int>, int> used;          // (class, state) -> operators
  auto cls = [&](const Op& o) { return isMulFu(f, o) ? 1 : isDiv(o.k) ? 2 : 0; };
  auto limit = [&](int c) { return pipeline ? 0 : c == 1 ? cfg.mulLimit : c == 2 ? cfg.divLimit : 0; };

  for (int id : B.ops) {
    Op& o = f.ops[id];
    o.cyc = 1;
    o.hold = -1;
    o.fu = -1;
    if (o.k == OpK::Const) { o.st = 0; o.tout = 0; continue; }
    if (o.k == OpK::ReadVar) {
      auto rs = readStage ? readStage->find(o.ref) : std::map<int, int>::const_iterator{};
      o.st = readStage && rs != readStage->end() ? rs->second : 0;
      o.tout = (float)tcq;
      continue;
    }
    int s = 0;
    double tin = 0;
    bool fromReg = false;
    std::vector<std::pair<int, double>> av;
    for (int x : {o.a, o.b, o.c}) {
      if (x < 0 || f.ops[x].k == OpK::Const) continue;
      const Op& p = f.ops[x];
      av.push_back({availSt(p), p.k == OpK::Load ? kMemQ * cfg.scale : (double)p.tout});
      s = std::max(s, av.back().first);
    }
    for (auto& [as, at] : av) {
      if (as == s) tin = std::max(tin, at);
      else fromReg = true;
    }
    if (fromReg) tin = std::max(tin, tcq);
    if (o.k == OpK::Load || o.k == OpK::Store) {
      if (tin + kMemSu * cfg.scale > budget && tin > tcq) { s++; tin = tcq; }
      // One transfer per state over all streams: a state waits for at most
      // one partner.
      int port = f.arrs[o.ref].mode == Arr::Stream ? -1 : o.ref;
      auto it = lastAcc.find(port);
      if (it != lastAcc.end()) s = std::max(s, it->second + 1);
      lastAcc[port] = s;
      o.st = s;
      o.tout = 0;
      continue;
    }
    double d = opDelay(f, o, cfg.scale);
    if (tin + d > budget) {
      if (tin > tcq) { s++; tin = tcq; }
      if (tin + d > budget) o.cyc = (int)std::ceil((tin + d) / budget);
    }
    int c = cls(o);
    if (limit(c)) {
      for (;;) {
        bool ok = true;
        for (int k = 0; k < o.cyc; k++) ok = ok && used[{c, s + k}] < limit(c);
        if (ok) break;
        s++;
        tin = tcq;
      }
      for (int k = 0; k < o.cyc; k++) used[{c, s + k}]++;
    }
    o.st = s;
    o.tout = (float)(o.cyc > 1 ? budget : tin + d);
  }

  // Wiring-only operators (extensions, truncations, constant shifts) move to
  // the state of their first reader when that lets the register hold the
  // narrower operand, or a variable that does not change in this block.
  {
    std::set<int> writtenVars;
    std::vector<bool> root(f.ops.size(), false);
    for (auto& [v, x] : B.writes) { writtenVars.insert(v); root[x] = true; }
    if (B.term.cond >= 0) root[B.term.cond] = true;
    std::map<int, int> firstUse;
    for (size_t i = B.ops.size(); i-- > 0;) {
      int id = B.ops[i];
      Op& o = f.ops[id];
      bool ext = o.k == OpK::ZExt || o.k == OpK::SExt;
      bool wiring = ext || o.k == OpK::Trunc ||
                    ((o.k == OpK::Shl || o.k == OpK::LShr || o.k == OpK::AShr) && f.ops[o.b].k == OpK::Const);
      auto fu = firstUse.find(id);
      if (wiring && !root[id] && fu != firstUse.end() && fu->second > o.st) {
        const Op& p = f.ops[o.a];
        bool stableVar = p.k == OpK::ReadVar && !writtenVars.count(p.ref);
        bool narrower = ext && p.k != OpK::Const && p.k != OpK::ReadVar && p.k != OpK::Load;
        if (stableVar || narrower) {
          o.st = fu->second;
          o.tout = (float)tcq;
        }
      }
      for (int x : {o.a, o.b, o.c})
        if (x >= 0) {
          auto it = firstUse.find(x);
          if (it == firstUse.end() || it->second > o.st) firstUse[x] = o.st;
        }
    }
  }

  int L = 1;
  for (int id : B.ops) {
    const Op& o = f.ops[id];
    L = std::max(L, (o.k == OpK::Store ? o.st : availSt(o)) + 1);
  }
  B.len = L;

  // Last state in which each value is looked at.
  for (int id : B.ops) f.ops[id].lastUse = -1;
  auto use = [&](int x, int s) {
    if (x >= 0) f.ops[x].lastUse = std::max(f.ops[x].lastUse, s);
  };
  // A multi-cycle operator needs its inputs steady until it is done, and so
  // does any wire it reads in its first state, back to the registers.
  std::map<int, int> wireEnd;
  for (size_t i = B.ops.size(); i-- > 0;) {
    int id = B.ops[i];
    const Op& o = f.ops[id];
    int s = (o.k == OpK::Load || o.k == OpK::Store) ? o.st : o.st + o.cyc - 1;
    auto we = wireEnd.find(id);
    if (we != wireEnd.end()) s = std::max(s, we->second);
    for (int x : {o.a, o.b, o.c}) {
      if (x < 0) continue;
      use(x, s);
      const Op& p = f.ops[x];
      if (p.k != OpK::Const && availSt(p) == o.st && s > o.st) {
        int& e = wireEnd[x];
        e = std::max(e, s);
      }
    }
  }
  if (B.term.cond >= 0) use(B.term.cond, mode == 2 ? availSt(f.ops[B.term.cond]) : L - 1);
  // A variable is written no earlier than the last read of its old value.
  std::map<int, int> entryRead;
  for (int id : B.ops)
    if (f.ops[id].k == OpK::ReadVar) entryRead[f.ops[id].ref] = id;
  B.wst.assign(B.writes.size(), 0);
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < B.writes.size(); i++) {
      auto [v, x] = B.writes[i];
      int ws = availSt(f.ops[x]);
      bool out = f.vars[v].port == Var::Ref || f.vars[v].port == Var::Ret;
      if (mode == 1 && !f.vars[v].isStatic && out) ws = L - 1;
      auto it = entryRead.find(v);
      if (it != entryRead.end() && mode != 2) ws = std::max(ws, f.ops[it->second].lastUse);
      ws = std::max(ws, B.wst[i]);
      if (ws != B.wst[i]) { B.wst[i] = ws; changed = true; }
      if (f.ops[x].lastUse < ws) { f.ops[x].lastUse = ws; changed = true; }
    }
  }
  if (pipeline) return;

  // Values that outlive their state need a register: the variable's own
  // register when the value is written there in the same state, otherwise a
  // temp register shared by the left-edge algorithm.
  struct Iv { int id, from, to; };
  std::map<int, std::vector<Iv>> byW;
  for (int id : B.ops) {
    Op& o = f.ops[id];
    if (o.k == OpK::Const || o.k == OpK::ReadVar || o.k == OpK::Store) continue;
    int a = availSt(o);
    if (o.lastUse <= a) continue;
    for (size_t i = 0; i < B.writes.size() && o.hold == -1; i++)
      if (B.writes[i].second == id && B.wst[i] == a) o.hold = -2 - B.writes[i].first;
    if (o.hold == -1) byW[o.w].push_back({id, a, o.lastUse});
  }
  for (auto& [w, ivs] : byW) {
    std::stable_sort(ivs.begin(), ivs.end(), [](const Iv& a, const Iv& b) { return a.from < b.from; });
    std::vector<int> freeAt;   // per register: the state after which it is free
    for (auto& iv : ivs) {
      int r = -1;
      if (cfg.share)
        for (size_t k = 0; k < freeAt.size() && r < 0; k++)
          if (freeAt[k] <= iv.from) r = (int)k;
      if (r < 0) {
        r = (int)freeAt.size();
        freeAt.push_back(0);
      }
      freeAt[r] = iv.to;
      f.ops[iv.id].hold = r;
    }
    temps[w] = (int)freeAt.size();
  }
}

std::string pipeRate(int K) {
  return K == 1 ? "one iteration per cycle" : "one iteration every " + std::to_string(K) + " cycles";
}

// A loop body as a pipeline with a new iteration every K cycles. Returns why
// not when the loop does not allow it; the block is then left unscheduled.
std::string schedulePipeLoop(Function& f, Block& B, int bi, const Cfg& cfg, int K) {
  if (B.term.k != Term::Br || (B.term.t != bi && B.term.f != bi))
    return "its body is not straight-line code (break, continue, return, or an inner loop that is not unrolled)";
  std::map<int, int> none, rs;
  std::set<int> readVars;
  for (int id : B.ops)
    if (f.ops[id].k == OpK::ReadVar) readVars.insert(f.ops[id].ref);
  // A variable the loop updates from its old value is written less than K
  // stages after it is read, so that the next iteration, K cycles behind,
  // sees the new value.
  for (int round = 0;; round++) {
    scheduleBlock(f, B, cfg, 2, none, &rs);
    int moved = -1;
    for (size_t i = 0; i < B.writes.size(); i++) {
      int v = B.writes[i].first;
      if (readVars.count(v) && B.wst[i] > rs[v] + K - 1) {
        rs[v] = B.wst[i] - (K - 1);
        moved = v;
      }
    }
    if (moved < 0) break;
    if (round > 96 || rs[moved] > 64)
      return "'" + f.vars[moved].name + "' is updated from its own previous value through logic or a memory read "
             "that takes more than one clock period";
  }
  // The accesses to one memory share its port and must stay in program
  // order across iterations: all of them within K stages.
  std::map<int, std::pair<int, int>> acc;   // array -> first and last stage
  for (int id : B.ops) {
    const Op& o = f.ops[id];
    if (o.cyc > 1) return std::string("a ") + opName(o.k) + " takes more than one clock period (raise -clock)";
    if (o.k != OpK::Load && o.k != OpK::Store) continue;
    auto [it, fresh] = acc.insert({o.ref, {o.st, o.st}});
    if (fresh) continue;
    it->second.second = o.st;
    if (o.st - it->second.first >= K)
      return "array '" + f.arrs[o.ref].name + "' is accessed more than once per iteration, and a memory has one port";
  }
  if (availSt(f.ops[B.term.cond]) > K - 1)
    return "the loop condition is not known in the first stage (it depends on a memory read or on a long path)";
  return "";
}

Sched schedule(Function& f, const Cfg& cfg) {
  Sched sc;
  std::vector<std::map<int, int>> temps(f.blocks.size());
  std::vector<std::string> refused(f.blocks.size());
  parallelFor(f.blocks.size(), [&](size_t b) {
    Block& B = f.blocks[b];
    B.pipeL = 0;
    if (B.pipeLine && !f.pipeline) {
      // The fastest rate that works, as long as it beats running the
      // iterations one after the other.
      for (int K = 1; K <= 8; K++) {
        std::string why = schedulePipeLoop(f, B, (int)b, cfg, K);
        if (K == 1) refused[b] = why;
        if (why.empty() && K > 1 && K >= B.len)
          refused[b] += " (one iteration every " + std::to_string(K) + " cycles would work, but that is what the loop does unpipelined)";
        if (why.rfind("its body", 0) == 0 || (why.empty() && K >= B.len)) break;
        if (why.empty()) {
          refused[b].clear();
          B.pipeL = B.len;
          B.pipeII = K;
          B.len = 1;
          return;
        }
      }
    }
    scheduleBlock(f, B, cfg, f.pipeline ? 1 : 0, temps[b]);
  });
  for (size_t b = 0; b < f.blocks.size(); b++) {
    const Block& B = f.blocks[b];
    if (B.pipeL) sc.pipedLoops++;
    if (B.pipeLine && !f.pipeline) {
      std::string n = "loop at line " + std::to_string(B.pipeLine) + ": ";
      if (B.pipeL) n += "pipelined, " + std::to_string(B.pipeL) + " stages, " + pipeRate(B.pipeII);
      else if (refused[b].empty()) n += "one iteration per cycle without pipelining";
      else n += "not pipelined: " + refused[b];
      // With -pipeline-loops an outer loop is not news.
      if (B.pipeMust || B.pipeL || refused[b].rfind("its body", 0) != 0) sc.loopNotes.push_back(n);
    }
    if (!refused[b].empty() && f.blocks[b].pipeMust)
      throw Error("mhls: " + f.name + ": the loop at line " + std::to_string(f.blocks[b].pipeLine) +
                  " cannot be pipelined: " + refused[b]);
  }
  for (auto& t : temps)
    for (auto& [w, n] : t) sc.temps[w] = std::max(sc.temps[w], n);

  // A block that only returns costs no state: jumping to it is "done".
  int base = 1;
  for (size_t b = 0; b < f.blocks.size(); b++) {
    Block& B = f.blocks[b];
    bool nothing = B.term.k == Term::Ret && B.writes.empty();
    for (int id : B.ops) nothing = nothing && f.ops[id].k != OpK::Store && f.ops[id].k != OpK::Load;
    if (nothing && !f.pipeline) B.len = 0;
    B.base = base;
    base += B.len;
  }
  sc.nStates = base;

  // Shared multipliers and dividers: operators of the same kind and width
  // that are never busy in the same state use one unit.
  if (!f.pipeline && cfg.share) {
    std::map<std::pair<int, int>, std::vector<int>> unitsOf;   // (kind,width) -> fu ids
    for (auto& B : f.blocks) {
      if (B.pipeL) continue;   // every stage of a pipelined loop is busy every cycle
      std::map<std::pair<int, int>, std::vector<std::vector<bool>>> busy;
      for (int id : B.ops) {
        Op& o = f.ops[id];
        if (!isMulFu(f, o) && !isDiv(o.k)) continue;
        std::pair<int, int> key{(int)o.k, o.w};
        auto& bz = busy[key];
        size_t u = 0;
        for (;; u++) {
          if (u == bz.size()) bz.emplace_back(B.len, false);
          bool ok = true;
          for (int k = 0; k < o.cyc; k++) ok = ok && !bz[u][o.st + k];
          if (ok) break;
        }
        for (int k = 0; k < o.cyc; k++) bz[u][o.st + k] = true;
        auto& units = unitsOf[key];
        while (units.size() <= u) {
          units.push_back((int)sc.fus.size());
          sc.fus.push_back(Fu{o.k, o.w, {}});
        }
        o.fu = units[u];
        sc.fus[o.fu].users.push_back(id);
      }
    }
    // Chaining through two shared units in opposite orders in different
    // states would close a combinational loop through their input
    // multiplexers (a false path, but a real loop in the netlist). Find
    // such cycles between units and give one unit on each cycle back to
    // its operators.
    for (;;) {
      size_t nf = sc.fus.size();
      std::vector<std::set<int>> edge(nf);   // unit -> units whose inputs it can reach
      for (size_t u = 0; u < nf; u++)
        for (int id : sc.fus[u].users) {
          const Op& p = f.ops[id];
          std::vector<int> work{p.a, p.b};
          std::set<int> seen;
          while (!work.empty()) {
            int x = work.back();
            work.pop_back();
            if (x < 0 || !seen.insert(x).second) continue;
            const Op& q = f.ops[x];
            if (q.k == OpK::Const || q.k == OpK::ReadVar || availSt(q) != p.st) continue;
            if (q.fu >= 0) edge[q.fu].insert((int)u);
            if (q.cyc > 1 || q.k == OpK::Load) continue;
            for (int y : {q.a, q.b, q.c}) work.push_back(y);
          }
        }
      std::vector<int> color(nf, 0);
      int victim = -1;
      std::function<void(int)> dfs = [&](int u) {
        color[u] = 1;
        for (int v : edge[u]) {
          if (victim >= 0) return;
          if (color[v] == 1) victim = v;
          else if (color[v] == 0) dfs(v);
        }
        color[u] = 2;
      };
      for (size_t u = 0; u < nf && victim < 0; u++)
        if (color[u] == 0) dfs((int)u);
      if (victim < 0) break;
      for (int id : sc.fus[victim].users) f.ops[id].fu = -1;
      sc.fus[victim].users.clear();
    }
    for (auto& fu : sc.fus)
      if (fu.users.size() < 2) {
        for (int id : fu.users) f.ops[id].fu = -1;
        fu.users.clear();
      }
  }

  // Summary numbers.
  std::set<int> fuSeen;
  for (auto& B : f.blocks)
    for (int id : B.ops) {
      const Op& o = f.ops[id];
      if (o.cyc > 1) sc.multicycle++;
      if (o.k == OpK::Const || o.k == OpK::ReadVar || o.k == OpK::ZExt || o.k == OpK::SExt || o.k == OpK::Trunc)
        continue;
      if (o.fu >= 0) {
        sc.area += 2.3 * o.w * 2;   // input multiplexers of the shared unit
        if (!fuSeen.insert(o.fu).second) continue;
      }
      sc.opCount[opName(o.k)]++;
      sc.area += opArea(f, o);
    }
  // Latency: longest and shortest path when the control flow has no loops.
  {
    size_t nb = f.blocks.size();
    std::vector<int> color(nb, 0);
    std::vector<long> mn(nb, 0), mx(nb, 0);
    bool cyclic = false;
    std::function<void(int)> dfs = [&](int b) {
      color[b] = 1;
      const Term& t = f.blocks[b].term;
      long lo = 0, hi = 0;
      bool first = true;
      for (int s : {t.k != Term::Ret ? t.t : -1, t.k == Term::Br ? t.f : -1}) {
        if (s < 0) continue;
        if (color[s] == 1) { cyclic = true; continue; }
        if (color[s] == 0) dfs(s);
        lo = first ? mn[s] : std::min(lo, mn[s]);
        hi = first ? mx[s] : std::max(hi, mx[s]);
        first = false;
      }
      mn[b] = lo + f.blocks[b].len;
      mx[b] = hi + f.blocks[b].len;
      color[b] = 2;
    };
    dfs(0);
    if (!cyclic) {
      sc.latMin = mn[0] + (f.pipeline ? 0 : 1);
      sc.latMax = mx[0] + (f.pipeline ? 0 : 1);
    }
  }
  return sc;
}

// ---- SystemVerilog emission ------------------------------------------------------

std::string hexLit(uint64_t v, int w) {
  char buf[40];
  snprintf(buf, sizeof buf, "%d'h%llx", w, (unsigned long long)maskW(v, w));
  return buf;
}
std::string rng(int w) { return w > 1 ? "[" + std::to_string(w - 1) + ":0] " : ""; }

std::string svName(const std::string& n) {
  static const std::set<std::string> kw = {
      "input", "output", "inout", "reg", "wire", "logic", "bit", "byte", "module", "begin", "end",
      "always", "assign", "initial", "integer", "real", "time", "event", "task", "function", "case",
      "default", "do", "for", "if", "else", "while", "return", "int", "var", "type", "local", "new",
      "this", "super", "null", "string", "final", "ref", "buf", "not", "and", "or", "xor", "nand",
      "nor", "xnor", "table", "edge", "signed", "unsigned", "parameter", "small", "medium", "large",
      "weak", "strong", "force", "release", "wait", "repeat", "forever", "fork", "join", "disable",
      "generate", "genvar", "config", "design", "cell", "use", "library", "instance", "priority",
      "unique", "packed", "enum", "struct", "union", "typedef", "class", "package", "interface",
      "program", "property", "sequence", "assert", "assume", "cover", "expect", "bind", "alias",
      "tri", "wand", "wor", "supply0", "supply1", "vectored", "scalared", "shortint", "longint",
      "void", "static", "automatic", "const", "import", "export", "extern", "pure", "virtual",
      "rand", "randc", "constraint", "solve", "before", "inside", "dist", "with", "matches",
      "tagged", "foreach", "break", "continue", "iff", "chandle", "context", "clocking", "global",
      "let", "checker", "ap_clk", "ap_rst", "ap_start", "ap_done", "ap_idle", "ap_ce", "ap_vld_in",
      "ap_vld_out", "state", "in_valid", "in_ready", "out_valid", "out_ready"};
  return kw.count(n) ? n + "_" : n;
}

struct Emitter {
  const Function& f;
  const Sched& sc;
  std::ostringstream o;
  const Block* curB = nullptr;

  Emitter(const Function& fn, const Sched& s) : f(fn), sc(s) {}

  static std::string memName(const Arr& a) { return a.isParam ? svName(a.name) : a.name; }
  std::string vreg(int v) const { return "r_" + f.vars[v].name; }
  std::string wire(int id) const { return "n" + std::to_string(id); }
  static std::string slice(const std::string& n, int have, int want) {
    if (want >= have) return n;
    return n + (want == 1 ? "[0]" : "[" + std::to_string(want - 1) + ":0]");
  }

  // The signal carrying value x for a reader in local state s.
  std::function<std::string(int, int, int)> ref;

  std::string fsmRef(int x, int s, int want) const {
    const Op& p = f.ops[x];
    if (p.k == OpK::Const) return hexLit(p.imm, want);
    if (p.k == OpK::ReadVar) return slice(vreg(p.ref), p.w, want);
    if (s <= availSt(p)) return slice(wire(x), p.w, want);
    if (p.hold <= -2) return slice(vreg(-2 - p.hold), p.w, want);
    return slice("t" + std::to_string(p.w) + "_" + std::to_string(p.hold), p.w, want);
  }

  // Right-hand side for an operator, with operand names from R(value, width).
  // Signed operators get an assign of their own (pre), because a signed
  // expression inside ?: with an unsigned arm is evaluated unsigned.
  std::string expr(int id, const std::function<std::string(int, int)>& R, std::string& pre) const {
    const Op& p = f.ops[id];
    int w = p.w;
    auto full = [&](int x) { return R(x, f.ops[x].w); };
    std::string W = std::to_string(w);
    auto zero = hexLit(0, w);
    switch (p.k) {
      case OpK::Add: return R(p.a, w) + " + " + R(p.b, w);
      case OpK::Sub: return R(p.a, w) + " - " + R(p.b, w);
      case OpK::Mul: return R(p.a, w) + " * " + R(p.b, w);
      case OpK::And: return R(p.a, w) + " & " + R(p.b, w);
      case OpK::Or: return R(p.a, w) + " | " + R(p.b, w);
      case OpK::Xor: return R(p.a, w) + " ^ " + R(p.b, w);
      case OpK::UDiv: return "(" + R(p.b, w) + " == " + zero + ") ? {" + W + "{1'b1}} : " + R(p.a, w) + " / " + R(p.b, w);
      case OpK::URem: return "(" + R(p.b, w) + " == " + zero + ") ? " + R(p.a, w) + " : " + R(p.a, w) + " % " + R(p.b, w);
      case OpK::SDiv: case OpK::SRem: {
        std::string q = wire(id) + "_s";
        pre = "  logic " + rng(w) + q + ";\n  assign " + q + " = $signed(" + R(p.a, w) + ") " +
              (p.k == OpK::SDiv ? "/" : "%") + " $signed(" + R(p.b, w) + ");\n";
        return "(" + R(p.b, w) + " == " + zero + ") ? " + (p.k == OpK::SDiv ? "{" + W + "{1'b1}}" : R(p.a, w)) + " : " + q;
      }
      case OpK::Shl: return R(p.a, w) + " << " + full(p.b);
      case OpK::LShr: return full(p.a) + " >> " + full(p.b);
      case OpK::AShr: return "$signed(" + full(p.a) + ") >>> " + full(p.b);
      case OpK::Eq: return full(p.a) + " == " + full(p.b);
      case OpK::Ne: return full(p.a) + " != " + full(p.b);
      case OpK::ULt: return full(p.a) + " < " + full(p.b);
      case OpK::ULe: return full(p.a) + " <= " + full(p.b);
      case OpK::SLt: return "$signed(" + full(p.a) + ") < $signed(" + full(p.b) + ")";
      case OpK::SLe: return "$signed(" + full(p.a) + ") <= $signed(" + full(p.b) + ")";
      case OpK::Mux: return R(p.a, 1) + " ? " + R(p.b, w) + " : " + R(p.c, w);
      case OpK::Trunc: return R(p.a, w);
      case OpK::ZExt: {
        int wa = f.ops[p.a].w;
        return "{" + hexLit(0, w - wa) + ", " + full(p.a) + "}";
      }
      case OpK::SExt: {
        int wa = f.ops[p.a].w;
        std::string a = full(p.a);
        return "{{" + std::to_string(w - wa) + "{" + a + (wa > 1 ? "[" + std::to_string(wa - 1) + "]" : "") + "}}, " + a + "}";
      }
      case OpK::RomRd: return "rom_" + f.arrs[p.ref].name + "(" + full(p.a) + ")";
      case OpK::Load: return memName(f.arrs[p.ref]) + "_rdata";
      default: return "'0";
    }
  }

  // A ROM read is a case statement per reader. (A shared function would be
  // shorter, but per-reader logic is what every tool agrees on.)
  std::string romRead(int id, const std::string& idx) const {
    const Op& p = f.ops[id];
    const Arr& a = f.arrs[p.ref];
    std::ostringstream r;
    r << "  logic " << rng(p.w) << wire(id) << ";\n  always_comb begin\n    case (" << idx << ")\n";
    for (int i = 0; i < a.size; i++)
      r << "      " << hexLit(i, a.aw()) << ": " << wire(id) << " = " << hexLit(a.init[i], p.w) << ";\n";
    r << "      default: " << wire(id) << " = " << hexLit(0, p.w) << ";\n    endcase\n  end\n";
    return r.str();
  }

  // Port list shared by both architectures.
  void ports(std::vector<std::string>& p) {
    for (auto& pm : f.params) {
      if (pm.k == Param::Array) {
        const Arr& a = f.arrs[pm.id];
        std::string n = svName(a.name);
        if (!a.read && !a.written) continue;
        if (a.mode == Arr::Stream) {
          std::string in = a.read ? "input  logic " : "output logic ", back = a.read ? "output logic " : "input  logic ";
          p.push_back(in + rng(a.ety.w) + n + "_tdata");
          p.push_back(in + n + "_tvalid");
          p.push_back(back + n + "_tready");
          continue;
        }
        p.push_back("output logic " + rng(a.aw()) + n + "_addr");
        p.push_back("output logic " + n + "_ce");
        if (a.written) {
          p.push_back("output logic " + n + "_we");
          p.push_back("output logic " + rng(a.ety.w) + n + "_wdata");
        }
        if (a.read) p.push_back("input  logic " + rng(a.ety.w) + n + "_rdata");
      } else {
        const Var& v = f.vars[pm.id];
        std::string n = svName(v.name);
        bool both = v.portIn && v.portOut;
        if (pm.k == Param::Scalar || v.portIn) p.push_back("input  logic " + rng(v.ty.w) + n + (both ? "_i" : ""));
        if (v.portOut) p.push_back("output logic " + rng(v.ty.w) + n + (both ? "_o" : ""));
      }
    }
    if (f.retVar >= 0) p.push_back("output logic " + rng(f.vars[f.retVar].ty.w) + "ap_return");
  }
  std::string inPort(const Var& v) const { return svName(v.name) + (v.portIn && v.portOut ? "_i" : ""); }
  std::string outPort(const Var& v) const {
    if (v.port == Var::Ret) return "ap_return";
    return svName(v.name) + (v.portIn && v.portOut ? "_o" : "");
  }

  void header(const char* arch, const Cfg& cfg) {
    o << "// Generated by mhls from " << f.name << "(): " << arch << ", scheduled for a "
      << cfg.clock << " ns clock.\n";
  }

  // ---- FSM with datapath ----
  std::string go(int b) const {
    const Block& B = f.blocks[b];
    if (B.len == 0) return "state <= " + st(0) + "; ap_done <= 1'b1;";
    return "state <= " + st(B.base) + ";";
  }
  int sw() const {
    int n = 1;
    while ((1 << n) < sc.nStates) n++;
    return n;
  }
  std::string st(int s) const { return hexLit(s, sw()); }

  std::string fsm(const Cfg& cfg) {
    header("FSM with datapath", cfg);
    std::vector<std::string> p = {"input  logic ap_clk", "input  logic ap_rst", "input  logic ap_start",
                                  "output logic ap_done", "output logic ap_idle"};
    ports(p);
    o << "module " << f.name << " (\n";
    for (size_t i = 0; i < p.size(); i++) o << "  " << p[i] << (i + 1 < p.size() ? ",\n" : "\n");
    o << ");\n";
    o << "  logic " << rng(sw()) << "state;\n";
    o << "  assign ap_idle = state == " << st(0) << ";\n";
    for (size_t v = 0; v < f.vars.size(); v++)
      if (f.vars[v].reg) o << "  logic " << rng(f.vars[v].ty.w) << vreg((int)v) << ";\n";
    for (auto& [w, n] : sc.temps)
      for (int i = 0; i < n; i++) o << "  logic " << rng(w) << "t" << w << "_" << i << ";\n";
    for (auto& v : f.vars)
      if (v.portOut || v.port == Var::Ret) o << "  assign " << outPort(v) << " = r_" << v.name << ";\n";

    // Streams. A transfer happens in a cycle where tvalid and tready are high.
    for (auto& a : f.arrs) {
      if (a.mode != Arr::Stream || (!a.read && !a.written)) continue;
      std::string n = svName(a.name);
      o << "  logic " << n << "_ce;\n  logic " << rng(a.ety.w) << n << (a.read ? "_rdata" : "_wdata") << ";\n";
      if (a.read)
        o << "  assign " << n << "_tready = " << n << "_ce;\n  always_ff @(posedge ap_clk) if (" << n << "_ce && " << n
          << "_tvalid) " << n << "_rdata <= " << n << "_tdata;\n";
      else
        o << "  assign " << n << "_tvalid = " << n << "_ce;\n  assign " << n << "_tdata = " << n << "_wdata;\n";
    }

    // Internal RAMs.
    for (auto& a : f.arrs) {
      if (a.mode != Arr::Mem || a.isParam || (!a.read && !a.written)) continue;
      std::string n = a.name;
      o << "  logic " << rng(a.ety.w) << n << "_mem [0:" << a.size - 1 << "];\n";
      o << "  logic " << rng(a.aw()) << n << "_addr;\n  logic " << n << "_ce, " << n << "_we;\n";
      o << "  logic " << rng(a.ety.w) << n << "_wdata, " << n << "_rdata;\n";
      o << "  always_ff @(posedge ap_clk)\n    if (" << n << "_ce) begin\n      if (" << n << "_we) " << n
        << "_mem[" << n << "_addr] <= " << n << "_wdata;\n      else " << n << "_rdata <= " << n << "_mem["
        << n << "_addr];\n    end\n";
      if (a.isStatic)
        o << "`ifndef SYNTHESIS\n  initial for (int i = 0; i < " << a.size << "; i++) " << n
          << "_mem[i] = '0;\n`endif\n";
    }

    // Datapath: one wire per operator.
    std::vector<std::vector<std::string>> act(sc.nStates);
    std::map<int, std::vector<std::string>> memAct;   // array -> case arms
    std::map<int, std::vector<std::string>> waits;    // state -> what it waits for
    for (size_t bi = 0; bi < f.blocks.size(); bi++) {
      const Block& B = f.blocks[bi];
      if (B.pipeL) {
        pipeLoop((int)bi, act, memAct, waits);
        continue;
      }
      for (int id : B.ops) {
        const Op& p = f.ops[id];
        if (p.k == OpK::Const || p.k == OpK::ReadVar) continue;
        auto R = [&](int x, int want) { return fsmRef(x, p.st, want); };
        if (p.k == OpK::Store || p.k == OpK::Load) {
          const Arr& a = f.arrs[p.ref];
          std::string n = a.isParam ? svName(a.name) : a.name;
          std::string arm = "      " + st(B.base + p.st) + ": begin " + n + "_addr = " + R(p.a, a.aw()) + "; ";
          // A memory access in a state that waits for a stream happens in
          // the state's last cycle only: a read repeated earlier would
          // replace data the waiting transfer still uses. The mark is
          // replaced once the waits of every state are known.
          std::string gate = "@W" + std::to_string(B.base + p.st) + "@";
          if (a.mode == Arr::Stream) {
            // The state is held until the element has been taken or sent.
            arm = "      " + st(B.base + p.st) + ": begin " + n + "_ce = " + (p.c >= 0 ? R(p.c, 1) : "1'b1") + "; " +
                  (p.k == OpK::Store ? n + "_wdata = " + R(p.b, a.ety.w) + "; " : "") + "end\n";
            waits[B.base + p.st].push_back("(!" + n + "_ce || " + n + (p.k == OpK::Load ? "_tvalid" : "_tready") + ")");
          } else if (p.k == OpK::Load) {
            arm += n + "_ce = " + gate + "1'b1; end\n";
          } else {
            std::string en = gate + (p.c >= 0 ? R(p.c, 1) : "1'b1");
            arm += n + "_ce = " + en + "; " + n + "_we = " + en + "; " + n + "_wdata = " + R(p.b, a.ety.w) + "; end\n";
          }
          memAct[p.ref].push_back(arm);
          if (p.k == OpK::Store) continue;
        }
        if (p.k == OpK::RomRd) {
          o << romRead(id, R(p.a, f.ops[p.a].w));
          if (p.hold >= 0)
            act[B.base + availSt(p)].push_back("t" + std::to_string(p.w) + "_" + std::to_string(p.hold) + " <= " + wire(id) + ";");
          continue;
        }
        std::string pre, e;
        if (p.fu >= 0) e = "fu" + std::to_string(p.fu) + "_y";
        else e = expr(id, R, pre);
        o << pre << "  logic " << rng(p.w) << wire(id) << ";\n  assign " << wire(id) << " = " << e << ";\n";
        if (p.hold >= 0)
          act[B.base + availSt(p)].push_back("t" + std::to_string(p.w) + "_" + std::to_string(p.hold) + " <= " + wire(id) + ";");
      }
      for (size_t i = 0; i < B.writes.size(); i++) {
        auto [v, x] = B.writes[i];
        act[B.base + B.wst[i]].push_back(vreg(v) + " <= " + fsmRef(x, B.wst[i], f.vars[v].ty.w) + ";");
      }
      if (B.len == 0) continue;
      for (int s = 0; s + 1 < B.len; s++) act[B.base + s].push_back("state <= " + st(B.base + s + 1) + ";");
      std::string& last = act[B.base + B.len - 1].emplace_back();
      const Term& t = B.term;
      if (t.k == Term::Ret) last = "state <= " + st(0) + "; ap_done <= 1'b1;";
      else if (t.k == Term::Jump) last = go(t.t);
      else last = "if (" + fsmRef(t.cond, B.len - 1, 1) + ") begin " + go(t.t) + " end else begin " + go(t.f) + " end";
    }

    // Shared multipliers and dividers.
    for (size_t u = 0; u < sc.fus.size(); u++) {
      const Fu& fu = sc.fus[u];
      if (fu.users.size() < 2) continue;
      std::string n = "fu" + std::to_string(u);
      o << "  logic " << rng(fu.w) << n << "_a, " << n << "_b, " << n << "_y;\n";
      o << "  always_comb begin\n    case (state)\n";
      for (size_t i = 0; i < fu.users.size(); i++) {
        int id = fu.users[i];
        const Op& p = f.ops[id];
        const Block* B = nullptr;
        for (auto& blk : f.blocks)
          if (std::find(blk.ops.begin(), blk.ops.end(), id) != blk.ops.end()) B = &blk;
        std::string states;
        for (int k = 0; k < p.cyc; k++) states += (k ? ", " : "") + st(B->base + p.st + k);
        std::string body = "begin " + n + "_a = " + fsmRef(p.a, p.st, fu.w) + "; " + n + "_b = " + fsmRef(p.b, p.st, fu.w) + "; end\n";
        if (i + 1 < fu.users.size()) o << "      " << states << ": " << body;
        else o << "      default: " << body;
      }
      o << "    endcase\n  end\n";
      // A copy of the first user, reading the unit's inputs.
      std::string a = n + "_a", b = n + "_b", zero = hexLit(0, fu.w), W = std::to_string(fu.w);
      switch (fu.k) {
        case OpK::Mul: o << "  assign " << n << "_y = " << a << " * " << b << ";\n"; break;
        case OpK::UDiv: o << "  assign " << n << "_y = (" << b << " == " << zero << ") ? {" << W << "{1'b1}} : " << a << " / " << b << ";\n"; break;
        case OpK::URem: o << "  assign " << n << "_y = (" << b << " == " << zero << ") ? " << a << " : " << a << " % " << b << ";\n"; break;
        default:
          o << "  logic " << rng(fu.w) << n << "_s;\n  assign " << n << "_s = $signed(" << a << ") "
            << (fu.k == OpK::SDiv ? "/" : "%") << " $signed(" << b << ");\n";
          o << "  assign " << n << "_y = (" << b << " == " << zero << ") ? "
            << (fu.k == OpK::SDiv ? "{" + W + "{1'b1}}" : a) << " : " << n << "_s;\n";
      }
    }

    // Memory ports.
    for (auto& [ai, arms] : memAct) {
      const Arr& a = f.arrs[ai];
      std::string n = a.isParam ? svName(a.name) : a.name;
      if (a.mode == Arr::Stream) {
        o << "  always_comb begin\n    " << n << "_ce = 1'b0;" << (a.read ? "" : " " + n + "_wdata = '0;");
      } else {
      o << "  always_comb begin\n    " << n << "_addr = '0; " << n << "_ce = 1'b0;";
      if (a.written || !a.isParam) o << " " << n << "_we = 1'b0; " << n << "_wdata = '0;";
      }
      o << "\n    case (state)\n";
      for (std::string s : arms) {
        for (size_t at; (at = s.find("@W")) != std::string::npos;) {
          size_t end = s.find('@', at + 2);
          std::string g;
          for (auto& w : waits[std::stoi(s.substr(at + 2, end - at - 2))]) g += w + " && ";
          s.replace(at, end - at + 1, g);
        }
        o << s;
      }
      o << "      default: ;\n    endcase\n  end\n";
    }

    // Control and registers.
    o << "  always_ff @(posedge ap_clk) begin\n    if (ap_rst) begin\n      state <= " << st(0)
      << ";\n      ap_done <= 1'b0;\n";
    for (auto& r : resets) o << "      " << r << "\n";
    for (size_t v = 0; v < f.vars.size(); v++) {
      const Var& x = f.vars[v];
      bool maybeUninit = x.port == Var::None && !x.isStatic && sc.entryLive[v];
      if (x.reg && (x.isStatic || x.portOut || x.port == Var::Ret || maybeUninit))
        o << "      " << vreg((int)v) << " <= " << hexLit(x.init, x.ty.w) << ";\n";
    }
    o << "    end else begin\n      ap_done <= 1'b0;\n      case (state)\n";
    o << "        " << st(0) << ": if (ap_start) begin\n";
    for (size_t v = 0; v < f.vars.size(); v++) {
      const Var& x = f.vars[v];
      if (x.reg && sc.entryLive[v] && (x.port == Var::In || (x.port == Var::Ref && x.portIn)))
        o << "          " << vreg((int)v) << " <= " << inPort(x) << ";\n";
    }
    o << "          " << go(0) << "\n        end\n";
    for (int s = 1; s < sc.nStates; s++) {
      o << "        " << st(s) << ": ";
      for (size_t i = 0; i < waits[s].size(); i++) o << (i ? " && " : "if (") << waits[s][i] << (i + 1 == waits[s].size() ? ") " : "");
      o << "begin\n";
      for (auto& a : act[s]) o << "          " << a << "\n";
      o << "        end\n";
    }
    o << "        default: state <= " << st(0) << ";\n      endcase\n    end\n  end\nendmodule\n";
    if (cfg.handshake) wrapper(p, false);
    return o.str();
  }

  // A pipelined loop: one FSM state. While the state is held, a new
  // iteration enters stage 0 every cycle until one says the loop is over;
  // the state is left when that last iteration has passed the last stage.
  std::vector<std::string> resets;
  void pipeLoop(int bi, std::vector<std::vector<std::string>>& act, std::map<int, std::vector<std::string>>& memAct,
                std::map<int, std::vector<std::string>>& waits) {
    const Block& B = f.blocks[bi];
    const int L = B.pipeL, K = B.pipeII;
    const int C = availSt(f.ops[B.term.cond]);   // the stage that knows whether the loop goes on
    const std::string P = "lp" + std::to_string(bi);
    std::set<int> written;
    for (auto& [v, x] : B.writes) written.insert(v);
    auto pname = [&](int x, int s) { return P + "_p" + std::to_string(s) + "_" + wire(x); };
    auto vld = [&](int s) {
      return s > 0 ? P + "_v" + std::to_string(s) : K == 1 ? "!" + P + "_stop" : "(!" + P + "_stop && " + P + "_ph == '0)";
    };
    auto ref2 = [&](int x, int s, int want) -> std::string {
      const Op& q = f.ops[x];
      if (q.k == OpK::Const) return hexLit(q.imm, want);
      if (q.k == OpK::ReadVar && (!written.count(q.ref) || s <= q.st)) return slice(vreg(q.ref), q.w, want);
      if (q.k != OpK::ReadVar && s <= availSt(q)) return slice(wire(x), q.w, want);
      return slice(pname(x, s), q.w, want);
    };
    o << "  // Loop at line " << B.pipeLine << ": " << L << " stages, " << pipeRate(K) << ".\n";
    o << "  logic " << P << "_stop;\n";
    resets.push_back(P + "_stop <= 1'b0;");
    int phw = 1;
    while ((1 << phw) < K) phw++;
    if (K > 1) {
      o << "  logic " << rng(phw) << P << "_ph;\n";
      resets.push_back(P + "_ph <= '0;");
    }
    for (int s = 1; s < L; s++) {
      o << "  logic " << P << "_v" << s << ";\n";
      resets.push_back(P + "_v" + std::to_string(s) + " <= 1'b0;");
      if (s <= C) continue;
      o << "  logic " << P << "_last" << s << ";\n";
      resets.push_back(P + "_last" + std::to_string(s) + " <= 1'b0;");
    }
    std::vector<std::string> regs;
    std::map<int, std::vector<std::pair<std::string, std::string>>> port;   // array -> (stage valid, what it drives)
    std::vector<std::string> xfer;   // per stream access: its stage has nothing to transfer, or the partner is ready
    for (int id : B.ops) {
      const Op& q = f.ops[id];
      if (q.k == OpK::Const) continue;
      if (q.k == OpK::ReadVar && !written.count(q.ref)) continue;   // steady for the whole loop
      auto R = [&](int x, int want) { return ref2(x, q.st, want); };
      if (q.k == OpK::Store || q.k == OpK::Load) {
        const Arr& a = f.arrs[q.ref];
        std::string n = a.isParam ? svName(a.name) : a.name;
        std::string drive = n + "_addr = " + R(q.a, a.aw()) + "; ";
        if (a.mode == Arr::Stream) {
          // "@O<k>@" becomes: every other stream is ready for its transfer.
          std::string en = q.c >= 0 ? R(q.c, 1) : "1'b1";
          drive = n + "_ce = @O" + std::to_string(xfer.size()) + "@" + en + "; " +
                  (q.k == OpK::Store ? n + "_wdata = " + R(q.b, a.ety.w) + ";" : "");
          xfer.push_back("(!(" + vld(q.st) + " && " + en + ") || " + n + (q.k == OpK::Load ? "_tvalid" : "_tready") + ")");
        } else if (q.k == OpK::Load) {
          drive += n + "_ce = @G@1'b1;";
        } else {
          std::string en = std::string("@G@") + (q.c >= 0 ? R(q.c, 1) : "1'b1");
          drive += n + "_ce = " + en + "; " + n + "_we = " + en + "; " + n + "_wdata = " + R(q.b, a.ety.w) + ";";
        }
        port[q.ref].push_back({vld(q.st), drive});
        if (q.k == OpK::Store) continue;
      }
      if (q.k == OpK::RomRd) {
        o << romRead(id, R(q.a, f.ops[q.a].w));
      } else if (q.k != OpK::ReadVar) {
        std::string pre;
        std::string e = expr(id, R, pre);
        o << pre << "  logic " << rng(q.w) << wire(id) << ";\n  assign " << wire(id) << " = " << e << ";\n";
      }
      for (int s = availSt(q) + 1; s <= q.lastUse; s++) {
        o << "  logic " << rng(q.w) << pname(id, s) << ";\n";
        regs.push_back(pname(id, s) + " <= " + ref2(id, s - 1, q.w) + ";");
      }
    }
    // With streams the whole pipeline moves only in a cycle where every
    // transfer its stages ask for can happen.
    if (!xfer.empty()) {
      o << "  logic " << P << "_go;\n  assign " << P << "_go = ";
      for (size_t i = 0; i < xfer.size(); i++) o << (i ? " && " : "") << xfer[i];
      o << ";\n";
      waits[B.base].push_back(P + "_go");
    }
    if (!regs.empty()) {
      o << "  always_ff @(posedge ap_clk) " << (xfer.empty() ? "" : "if (" + P + "_go) ") << "begin\n";
      for (auto& r : regs) o << "    " << r << "\n";
      o << "  end\n";
    }
    for (auto& [ai, uses] : port)
      for (auto& [stage, drive] : uses) {
        for (size_t at; (at = drive.find("@G@")) != std::string::npos;) drive.replace(at, 3, xfer.empty() ? "" : P + "_go && ");
        for (size_t at; (at = drive.find("@O")) != std::string::npos;) {
          size_t end = drive.find('@', at + 2);
          size_t k = std::stoul(drive.substr(at + 2, end - at - 2));
          std::string others;
          for (size_t i = 0; i < xfer.size(); i++)
            if (i != k) others += xfer[i] + " && ";
          drive.replace(at, end - at + 1, others);
        }
      }
    // The accesses to one memory are in different stages less than K
    // apart, so at most one of them holds an iteration in any cycle.
    for (auto& [ai, uses] : port) {
      std::string arm = "      " + st(B.base) + ": begin ";
      for (size_t i = 0; i < uses.size(); i++)
        arm += std::string(i ? "else " : "") + "if (" + uses[i].first + ") begin " + uses[i].second + " end ";
      memAct[ai].push_back(arm + "end\n");
    }
    auto& a = act[B.base];
    std::string c = ref2(B.term.cond, C, 1);
    std::string over = B.term.t == bi ? "!" + c : c;
    int next = B.term.t == bi ? B.term.f : B.term.t;
    for (int s = 1; s < L; s++) {
      a.push_back(P + "_v" + std::to_string(s) + " <= " + vld(s - 1) + ";");
      if (s <= C) continue;
      a.push_back(P + "_last" + std::to_string(s) + " <= " +
                  (s == C + 1 ? vld(C) + " && (" + over + ")" : P + "_last" + std::to_string(s - 1)) + ";");
    }
    a.push_back("if (" + vld(C) + " && (" + over + ")) " + P + "_stop <= 1'b1;");
    if (K > 1)
      a.push_back(P + "_ph <= " + P + "_ph == " + std::to_string(phw) + "'d" + std::to_string(K - 1) + " ? '0 : " + P +
                  "_ph + 1'b1;");
    for (size_t i = 0; i < B.writes.size(); i++) {
      auto [v, x] = B.writes[i];
      a.push_back("if (" + vld(B.wst[i]) + ") " + vreg(v) + " <= " + ref2(x, B.wst[i], f.vars[v].ty.w) + ";");
    }
    std::string l = std::to_string(L - 1);
    a.push_back("if (" + P + "_v" + l + " && " + P + "_last" + l + ") begin " + go(next) + " " + P + "_stop <= 1'b0;" +
                (K > 1 ? " " + P + "_ph <= '0;" : "") + " end");
  }

  // The block behind ready/valid ports: a transfer happens in a cycle where
  // valid and ready are both high. p holds the port declarations of the
  // block, the five (FSM: clock, reset, start, done, idle; pipeline: clock,
  // reset, enable, valid in, valid out) control ports first.
  void wrapper(const std::vector<std::string>& p, bool pipeline) {
    auto name = [](const std::string& d) { return d.substr(d.rfind(' ') + 1); };
    o << "\n// " << f.name << " with ready/valid handshakes: inputs are taken when in_valid and\n"
         "// in_ready are high, the result waits with out_valid high until out_ready.\n";
    o << "module " << f.name << "_hs (\n  input  logic ap_clk,\n  input  logic ap_rst,\n  input  logic in_valid,\n"
         "  output logic in_ready,\n  output logic out_valid,\n  input  logic out_ready";
    for (size_t i = 5; i < p.size(); i++) o << ",\n  " << p[i];
    o << "\n);\n";
    std::string conn = ".ap_clk(ap_clk), .ap_rst(ap_rst)";
    if (pipeline) {
      // A result nobody takes stalls every stage behind it.
      o << "  logic ap_ce;\n  assign ap_ce = !out_valid || out_ready;\n  assign in_ready = ap_ce;\n";
      conn += ", .ap_ce(ap_ce), .ap_vld_in(in_valid), .ap_vld_out(out_valid)";
    } else {
      // The outputs hold after ap_done; the next call starts when the
      // result has been taken.
      o << "  logic ap_start, ap_done, ap_idle, hold;\n  assign out_valid = ap_done || hold;\n"
           "  assign in_ready = ap_idle && !out_valid;\n  assign ap_start = in_valid && in_ready;\n"
           "  always_ff @(posedge ap_clk) hold <= !ap_rst && out_valid && !out_ready;\n";
      conn += ", .ap_start(ap_start), .ap_done(ap_done), .ap_idle(ap_idle)";
    }
    for (size_t i = 5; i < p.size(); i++) conn += ", ." + name(p[i]) + "(" + name(p[i]) + ")";
    o << "  " << f.name << " core (" << conn << ");\nendmodule\n";
  }

  // ---- pipeline ----
  std::string pipe(const Cfg& cfg) {
    const Block& B = f.blocks[0];
    int L = B.len;
    header("pipeline, one input per cycle", cfg);
    o << "// Latency " << L << " cycle" << (L == 1 ? "" : "s") << " from ap_vld_in to ap_vld_out.\n";
    std::vector<std::string> p = {"input  logic ap_clk", "input  logic ap_rst", "input  logic ap_ce",
                                  "input  logic ap_vld_in", "output logic ap_vld_out"};
    ports(p);
    o << "module " << f.name << " (\n";
    for (size_t i = 0; i < p.size(); i++) o << "  " << p[i] << (i + 1 < p.size() ? ",\n" : "\n");
    o << ");\n";
    auto pname = [&](int x, int s) { return "p" + std::to_string(s) + "_" + wire(x); };
    auto ref2 = [&](int x, int s, int want) -> std::string {
      const Op& q = f.ops[x];
      if (q.k == OpK::Const) return hexLit(q.imm, want);
      if (q.k == OpK::ReadVar) {
        const Var& v = f.vars[q.ref];
        if (v.isStatic) return slice(vreg(q.ref), q.w, want);
        if (v.port == Var::None) return hexLit(0, want);
        if (s == 0) return slice(inPort(v), q.w, want);
        return slice(pname(x, s), q.w, want);
      }
      if (s <= availSt(q)) return slice(wire(x), q.w, want);
      return slice(pname(x, s), q.w, want);
    };
    std::vector<bool> preg(f.vars.size(), false);
    for (size_t v = 0; v < f.vars.size(); v++)
      preg[v] = f.vars[v].reg && (f.vars[v].isStatic || f.vars[v].port == Var::Ret || f.vars[v].portOut);
    for (size_t v = 0; v < f.vars.size(); v++)
      if (preg[v]) o << "  logic " << rng(f.vars[v].ty.w) << vreg((int)v) << ";\n";
    for (auto& v : f.vars)
      if (v.portOut || v.port == Var::Ret) o << "  assign " << outPort(v) << " = r_" << v.name << ";\n";
    for (int s = 1; s < L; s++) o << "  logic vld" << s << ";\n";
    auto vld = [&](int s) { return s == 0 ? std::string("ap_vld_in") : "vld" + std::to_string(s); };

    std::vector<std::vector<std::string>> stage(L);   // registers clocked at the end of stage s
    for (int id : B.ops) {
      const Op& q = f.ops[id];
      if (q.k == OpK::Const) continue;
      bool port = q.k == OpK::ReadVar;
      if (port && (f.vars[q.ref].isStatic || f.vars[q.ref].port == Var::None)) continue;
      if (q.k == OpK::RomRd) {
        o << romRead(id, ref2(q.a, q.st, f.ops[q.a].w));
      } else if (!port) {
        std::string pre;
        auto R = [&](int x, int want) { return ref2(x, q.st, want); };
        std::string e = expr(id, R, pre);
        o << pre << "  logic " << rng(q.w) << wire(id) << ";\n  assign " << wire(id) << " = " << e << ";\n";
      }
      for (int s = availSt(q) + 1; s <= q.lastUse; s++) {
        o << "  logic " << rng(q.w) << pname(id, s) << ";\n";
        stage[s - 1].push_back(pname(id, s) + " <= " + ref2(id, s - 1, q.w) + ";");
      }
    }
    o << "  always_ff @(posedge ap_clk) begin\n    if (ap_rst) begin\n      ap_vld_out <= 1'b0;\n";
    for (int s = 1; s < L; s++) o << "      vld" << s << " <= 1'b0;\n";
    for (size_t v = 0; v < f.vars.size(); v++)
      if (preg[v]) o << "      " << vreg((int)v) << " <= " << hexLit(f.vars[v].init, f.vars[v].ty.w) << ";\n";
    o << "    end else if (ap_ce) begin\n";
    for (int s = 1; s < L; s++) o << "      vld" << s << " <= " << vld(s - 1) << ";\n";
    o << "      ap_vld_out <= " << vld(L - 1) << ";\n";
    for (size_t i = 0; i < B.writes.size(); i++) {
      auto [v, x] = B.writes[i];
      o << "      if (" << vld(B.wst[i]) << ") " << vreg(v) << " <= " << ref2(x, B.wst[i], f.vars[v].ty.w) << ";\n";
    }
    o << "    end\n  end\n";
    bool any = false;
    for (auto& s : stage) any = any || !s.empty();
    if (any) {
      o << "  always_ff @(posedge ap_clk) begin\n    if (ap_ce) begin\n";
      for (auto& s : stage)
        for (auto& a : s) o << "      " << a << "\n";
      o << "    end\n  end\n";
    }
    o << "endmodule\n";
    if (cfg.handshake) wrapper(p, true);
    return o.str();
  }
};

// Pipelining needs a straight-line function; says why when it is not.
void checkPipeline(const Function& f, const Program& P, const Func& src) {
  auto no = [&](const std::string& why) { fail(P.file, src.line, "'" + f.name + "' cannot be pipelined: " + why); };
  if (f.blocks.size() != 1)
    no("it still has branches or loops after if-conversion (unroll the loops with -unroll-all or "
       "\"#pragma hls unroll\", and avoid break/continue/early return)");
  const Block& B = f.blocks[0];
  for (int id : B.ops) {
    const Op& o = f.ops[id];
    if ((o.k == OpK::Load || o.k == OpK::Store) && f.arrs[o.ref].mode == Arr::Stream)
      no("it reads or writes stream '" + f.arrs[o.ref].name + "' (use the FSM architecture)");
    if (o.k == OpK::Load || o.k == OpK::Store) no("array '" + f.arrs[o.ref].name + "' is a memory (raise -max-reg-array, or use the FSM architecture)");
    if (o.cyc > 1) no(std::string("a ") + opName(o.k) + " takes more than one clock period (raise -clock)");
  }
  // A static read and its update must share a stage, or the next input
  // would read a stale value.
  for (size_t i = 0; i < B.writes.size(); i++) {
    int v = B.writes[i].first;
    if (!f.vars[v].isStatic) continue;
    for (size_t k = 0; k < B.writes.size(); k++) {
      const Op& x = f.ops[B.writes[k].second];
      if (x.k == OpK::ReadVar && x.ref == v && B.wst[k] != B.wst[i])
        no("static '" + f.vars[v].name + "' is read and updated in different stages (raise -clock)");
    }
    for (int id : B.ops) {
      const Op& o = f.ops[id];
      for (int x : {o.a, o.b, o.c})
        if (x >= 0 && f.ops[x].k == OpK::ReadVar && f.ops[x].ref == v && o.st != B.wst[i] && o.k != OpK::Const)
          no("static '" + f.vars[v].name + "' is read in stage " + std::to_string(o.st) + " and updated in stage " +
             std::to_string(B.wst[i]) + ", so one input per cycle is not possible at this clock (raise -clock)");
    }
  }
}

// ---- vectors, testbench and C reference -------------------------------------------

struct Rng {
  uint64_t s;
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  uint64_t value(int w) {
    uint64_t r = next();
    switch (r % 8) {
      case 0: return maskW(next() % 16, w);
      case 1: return maskW(0 - (next() % 16), w);
      case 2: return (next() & 1) ? maskW(~0ull, w) : 0;
      default: return maskW(next(), w);
    }
  }
};

struct Vec {
  std::map<int, uint64_t> in;     // variable -> input value
  std::map<int, uint64_t> out;    // variable -> expected output
  std::vector<std::tuple<int, int, uint64_t>> memChg;   // array, index, new value
  std::map<int, std::vector<uint64_t>> sIn, sOut;        // stream -> the elements taken / sent by this call
  long lat = 0;
};
struct Vectors {
  std::map<int, std::vector<uint64_t>> memInit;
  std::vector<Vec> v;
  std::map<int, std::vector<uint64_t>> memFinal;
};

// What the source of a stream offers in the reference run and in the
// testbench: this many elements, over and over.
const int kStreamSrc = 64;

Vectors makeVectors(const Function& f, int n, uint64_t seed) {
  Vectors V;
  Rng r{seed * 0x9E3779B97F4A7C15ull + 0x1234567ull};
  Machine m;
  m.reset(f);
  for (auto& pm : f.params)
    if (pm.k == Param::Array) {
      const Arr& a = f.arrs[pm.id];
      if (a.mode == Arr::Stream) m.mems[pm.id].assign(a.read ? kStreamSrc : 0, 0);
      for (auto& x : m.mems[pm.id]) x = r.value(a.ety.w);
      V.memInit[pm.id] = m.mems[pm.id];
    }
  for (int i = 0; i < n; i++) {
    Vec vec;
    for (auto& pm : f.params) {
      if (pm.k == Param::Array) continue;
      const Var& v = f.vars[pm.id];
      if (pm.k == Param::Scalar || v.portIn) {
        uint64_t x = r.value(v.ty.w);
        vec.in[pm.id] = x;
        m.vars[pm.id] = x;
      }
    }
    auto before = m.mems;
    auto pos = m.spos;
    vec.lat = interpret(f, m) + (f.pipeline ? 0 : 1);
    for (auto& pm : f.params) {
      if (pm.k != Param::Array || f.arrs[pm.id].mode != Arr::Stream) continue;
      for (size_t k = pos[pm.id]; k < m.spos[pm.id]; k++) vec.sIn[pm.id].push_back(m.mems[pm.id][k % kStreamSrc]);
      vec.sOut[pm.id] = std::move(m.sout[pm.id]);
      m.sout[pm.id].clear();
    }
    for (size_t v = 0; v < f.vars.size(); v++)
      if (f.vars[v].portOut || f.vars[v].port == Var::Ret) vec.out[(int)v] = m.vars[v];
    for (auto& pm : f.params)
      if (pm.k == Param::Array)
        for (size_t k = 0; k < m.mems[pm.id].size(); k++)
          if (m.mems[pm.id][k] != before[pm.id][k]) vec.memChg.push_back({pm.id, (int)k, m.mems[pm.id][k]});
    V.v.push_back(vec);
  }
  for (auto& pm : f.params)
    if (pm.k == Param::Array && f.arrs[pm.id].mode != Arr::Stream) V.memFinal[pm.id] = m.mems[pm.id];
  return V;
}

std::string testbench(const Function& f, const Sched& sc, const Vectors& V, const Cfg& cfg) {
  Emitter em(f, sc);
  std::ostringstream o;
  char half[32];
  snprintf(half, sizeof half, "%.3f", cfg.clock / 2);
  o << "// Generated by mhls: self-checking testbench for " << f.name << ", " << V.v.size()
    << " calls with expected values from the C reference run.\n`timescale 1ns/1ps\nmodule " << f.name << "_tb;\n";
  o << "  logic ap_clk = 0, ap_rst = 1;\n  integer tb_errors = 0, tb_lat = 0, tb_latmin = 1 << 30, tb_latmax = 0, tb_n = 0;\n";
  std::string conn = ".ap_clk(ap_clk), .ap_rst(ap_rst)";
  int nStreams = 0;
  const bool hs = cfg.handshake;
  if (hs) {
    o << "  logic in_valid = 0, out_ready = " << (f.pipeline ? 1 : 0) << ";\n  wire in_ready, out_valid;\n";
    conn += ", .in_valid(in_valid), .in_ready(in_ready), .out_valid(out_valid), .out_ready(out_ready)";
  } else if (f.pipeline) {
    o << "  logic ap_ce = 1, ap_vld_in = 0;\n  wire ap_vld_out;\n";
    conn += ", .ap_ce(ap_ce), .ap_vld_in(ap_vld_in), .ap_vld_out(ap_vld_out)";
  } else {
    o << "  logic ap_start = 0;\n  wire ap_done, ap_idle;\n";
    conn += ", .ap_start(ap_start), .ap_done(ap_done), .ap_idle(ap_idle)";
  }
  for (auto& pm : f.params) {
    if (pm.k == Param::Array) {
      const Arr& a = f.arrs[pm.id];
      if (!a.read && !a.written) continue;
      if (a.mode == Arr::Stream) {
        // A source that offers its elements in order and a sink that checks
        // what arrives; while tb_stall is set both hesitate now and then.
        std::string n = svName(a.name), k = std::to_string(nStreams++);
        if (nStreams == 1)
          o << "  logic tb_stall = 0;\n  integer tb_t = 0;\n  always @(posedge ap_clk) tb_t <= tb_t + 1;\n";
        conn += ", ." + n + "_tdata(" + n + "_tdata), ." + n + "_tvalid(" + n + "_tvalid), ." + n + "_tready(" + n + "_tready)";
        if (a.read) {
          o << "  logic " << rng(a.ety.w) << n << "_src [0:" << kStreamSrc - 1 << "];\n  integer " << n << "_pos = 0;\n  logic " << n
            << "_tvalid = 0, " << n << "_took = 0;\n  wire " << n << "_tready;\n  wire " << rng(a.ety.w) << n << "_tdata = " << n
            << "_src[" << n << "_pos % " << kStreamSrc << "];\n";
          o << "  always @(posedge ap_clk) begin\n    " << n << "_took <= " << n << "_tvalid && " << n << "_tready;\n    if (" << n
            << "_tvalid && " << n << "_tready) " << n << "_pos <= " << n << "_pos + 1;\n  end\n";
          o << "  always @(negedge ap_clk) if (!" << n << "_tvalid || " << n << "_took) " << n << "_tvalid = !tb_stall || (tb_t + "
            << k << ") % 5 < 2;\n";
        } else {
          o << "  wire " << rng(a.ety.w) << n << "_tdata;\n  wire " << n << "_tvalid;\n  logic " << n << "_tready = 1;\n  integer " << n
            << "_cnt = 0;\n";
          o << "  always @(negedge ap_clk) " << n << "_tready = !tb_stall || (tb_t * 5 + " << k << ") % 7 < 4;\n";
          o << "  always @(posedge ap_clk) if (!ap_rst && " << n << "_tvalid && " << n << "_tready) begin\n    case (" << n << "_cnt)\n";
          size_t c = 0;
          for (auto& vec : V.v) {
            auto it = vec.sOut.find(pm.id);
            if (it == vec.sOut.end()) continue;
            for (auto x : it->second)
              o << "      " << c++ << ": if (" << n << "_tdata !== " << hexLit(x, a.ety.w) << ") begin $display(\"FAIL: " << n
                << " element " << c - 1 << " = %h, expected " << hexLit(x, a.ety.w) << "\", " << n
                << "_tdata); tb_errors = tb_errors + 1; end\n";
          }
          o << "      default: begin $display(\"FAIL: " << n << " sent more than " << c
            << " elements\"); tb_errors = tb_errors + 1; end\n    endcase\n    " << n << "_cnt = " << n << "_cnt + 1;\n  end\n";
        }
        continue;
      }
      std::string n = svName(a.name);
      o << "  logic " << rng(a.ety.w) << n << "_m [0:" << a.size - 1 << "];\n  wire " << rng(a.aw()) << n
        << "_addr;\n  wire " << n << "_ce;\n";
      conn += ", ." + n + "_addr(" + n + "_addr), ." + n + "_ce(" + n + "_ce)";
      if (a.written) {
        o << "  wire " << n << "_we;\n  wire " << rng(a.ety.w) << n << "_wdata;\n";
        conn += ", ." + n + "_we(" + n + "_we), ." + n + "_wdata(" + n + "_wdata)";
      }
      if (a.read) {
        o << "  logic " << rng(a.ety.w) << n << "_rdata;\n";
        conn += ", ." + n + "_rdata(" + n + "_rdata)";
      }
      o << "  always @(posedge ap_clk) if (" << n << "_ce) begin\n";
      if (a.written) o << "    if (" << n << "_we) " << n << "_m[" << n << "_addr] <= " << n << "_wdata;\n";
      if (a.read) o << "    " << (a.written ? "else " : "") << n << "_rdata <= " << n << "_m[" << n << "_addr];\n";
      o << "  end\n";
    } else {
      const Var& v = f.vars[pm.id];
      if (pm.k == Param::Scalar || v.portIn) {
        o << "  logic " << rng(v.ty.w) << em.inPort(v) << " = 0;\n";
        conn += ", ." + em.inPort(v) + "(" + em.inPort(v) + ")";
      }
      if (v.portOut) {
        o << "  wire " << rng(v.ty.w) << em.outPort(v) << ";\n";
        conn += ", ." + em.outPort(v) + "(" + em.outPort(v) + ")";
      }
    }
  }
  if (f.retVar >= 0) {
    o << "  wire " << rng(f.vars[f.retVar].ty.w) << "ap_return;\n";
    conn += ", .ap_return(ap_return)";
  }
  o << "  " << f.name << (hs ? "_hs" : "") << " dut (" << conn << ");\n  always #" << half << " ap_clk = ~ap_clk;\n";
  auto setIn = [&](const Vec& vec) {
    std::string s;
    for (auto& [v, x] : vec.in) s += em.inPort(f.vars[v]) + " = " + hexLit(x, f.vars[v].ty.w) + "; ";
    return s;
  };
  auto check = [&](const std::string& sig, uint64_t x, int w, size_t i) {
    return "if (" + sig + " !== " + hexLit(x, w) + ") begin $display(\"FAIL: call " + std::to_string(i) + ": " + sig +
           " = %h, expected " + hexLit(x, w) + "\", " + sig + "); tb_errors = tb_errors + 1; end";
  };
  if (!f.pipeline) {
    long limit = 0;
    for (auto& vec : V.v) limit = std::max(limit, vec.lat);
    limit = (limit * 2 + 100) * (nStreams ? 4 : 1);
    // With handshakes: the result is left waiting for a few cycles, which
    // must not change it, and is then taken.
    o << "  task call;\n    begin\n"
      << (hs ? "      @(negedge ap_clk); in_valid = 1;\n      while (!in_ready) @(negedge ap_clk);\n"
               "      @(negedge ap_clk); in_valid = 0; tb_lat = 1;\n      while (!out_valid) begin\n"
             : "      @(negedge ap_clk); ap_start = 1;\n      @(negedge ap_clk); ap_start = 0; tb_lat = 1;\n"
               "      while (!ap_done) begin\n")
      << "        @(negedge ap_clk); tb_lat = tb_lat + 1;\n        if (tb_lat > " << limit
      << ") begin $display(\"FAIL: no ap_done after %0d cycles\", tb_lat); $finish; end\n      end\n"
         "      if (tb_lat < tb_latmin) tb_latmin = tb_lat;\n      if (tb_lat > tb_latmax) tb_latmax = tb_lat;\n"
      << (hs ? "      repeat (tb_n % 3) @(negedge ap_clk);\n      if (!out_valid || in_ready) begin $display(\"FAIL: the "
               "result did not wait for out_ready\"); tb_errors = tb_errors + 1; end\n      out_ready = 1;\n"
               "      @(negedge ap_clk); out_ready = 0; tb_n = tb_n + 1;\n"
             : "")
      << "    end\n  endtask\n";
    o << "  initial begin\n";
    for (auto& [ai, init] : V.memInit) {
      const Arr& a = f.arrs[ai];
      if (!a.read && !a.written) continue;
      for (size_t k = 0; k < init.size(); k++)
        o << "    " << svName(a.name) << (a.mode == Arr::Stream ? "_src[" : "_m[") << k << "] = " << hexLit(init[k], a.ety.w) << ";\n";
    }
    o << "    repeat (3) @(negedge ap_clk);\n    ap_rst = 0;\n";
    std::map<int, size_t> taken, sent;
    for (size_t i = 0; i < V.v.size(); i++) {
      const Vec& vec = V.v[i];
      // Every other call runs against a hesitant source and sink: the same
      // elements in the same order, in at least as many cycles.
      const bool stall = nStreams && i % 2;
      if (nStreams) o << "    tb_stall = " << stall << ";\n";
      o << "    " << setIn(vec) << "call;\n";
      for (auto& [ai, xs] : vec.sIn) {
        std::string n = svName(f.arrs[ai].name);
        taken[ai] += xs.size();
        o << "    if (" << n << "_pos != " << taken[ai] << ") begin $display(\"FAIL: call " << i << ": %0d elements taken from " << n
          << ", expected " << taken[ai] << "\", " << n << "_pos); tb_errors = tb_errors + 1; end\n";
      }
      for (auto& [ai, xs] : vec.sOut) {
        if (!f.arrs[ai].written) continue;
        std::string n = svName(f.arrs[ai].name);
        sent[ai] += xs.size();
        o << "    if (" << n << "_cnt != " << sent[ai] << ") begin $display(\"FAIL: call " << i << ": %0d elements sent to " << n
          << ", expected " << sent[ai] << "\", " << n << "_cnt); tb_errors = tb_errors + 1; end\n";
      }
      for (auto& [v, x] : vec.out) o << "    " << check(em.outPort(f.vars[v]), x, f.vars[v].ty.w, i) << "\n";
      for (auto& [ai, k, x] : vec.memChg)
        o << "    " << check(svName(f.arrs[ai].name) + "_m[" + std::to_string(k) + "]", x, f.arrs[ai].ety.w, i) << "\n";
      o << "    if (tb_lat " << (stall ? "<" : "!=") << " " << vec.lat << ") begin $display(\"FAIL: call " << i << ": %0d cycles, expected "
        << vec.lat << "\", tb_lat); tb_errors = tb_errors + 1; end\n";
    }
    for (auto& [ai, fin] : V.memFinal) {
      const Arr& a = f.arrs[ai];
      if (!a.written) continue;
      for (size_t k = 0; k < fin.size(); k++)
        o << "    " << check(svName(a.name) + "_m[" + std::to_string(k) + "]", fin[k], a.ety.w, V.v.size()) << "\n";
    }
    o << "    if (tb_errors == 0) $display(\"PASS: " << f.name << " " << V.v.size()
      << " calls, latency %0d..%0d cycles\", tb_latmin, tb_latmax);\n    else $display(\"FAIL: " << f.name
      << " %0d errors\", tb_errors);\n    $finish;\n  end\n";
  } else {
    int L = f.blocks[0].len;
    o << "  integer tb_cyc = 0, tb_first_in = -1, tb_first_out = -1;\n  always @(posedge ap_clk) tb_cyc <= tb_cyc + 1;\n";
    if (hs) {
      // The sink refuses now and then; the source waits for each transfer.
      o << "  logic tb_acc = 0;\n  always @(posedge ap_clk) tb_acc <= in_valid && in_ready;\n"
           "  always @(negedge ap_clk) out_ready = (tb_cyc % 7 != 3) && (tb_cyc % 11 < 8);\n";
      o << "  initial begin\n    repeat (3) @(negedge ap_clk);\n    ap_rst = 0;\n    @(negedge ap_clk);\n";
      for (size_t i = 0; i < V.v.size(); i++) {
        o << "    in_valid = 1; " << setIn(V.v[i]) << "\n    @(negedge ap_clk); while (!tb_acc) @(negedge ap_clk);\n";
        if (i % 5 == 3) o << "    in_valid = 0; @(negedge ap_clk);\n";
      }
      o << "    in_valid = 0;\n    repeat (" << 3 * L + 40 << ") @(negedge ap_clk);\n";
    } else {
    o << "  initial begin\n    repeat (3) @(negedge ap_clk);\n    ap_rst = 0;\n";
    for (size_t i = 0; i < V.v.size(); i++) {
      // A bubble every few inputs, and one stalled cycle, to exercise valid and ap_ce.
      if (i % 5 == 3) o << "    @(negedge ap_clk); ap_vld_in = 0;\n";
      if (i % 7 == 5 && (int)i > L + 2) o << "    @(negedge ap_clk); ap_ce = 0; ap_vld_in = 0;\n    @(negedge ap_clk); ap_ce = 1;\n";
      o << "    @(negedge ap_clk); ap_vld_in = 1; " << setIn(V.v[i]) << (i == 0 ? "tb_first_in = tb_cyc;" : "") << "\n";
    }
    o << "    @(negedge ap_clk); ap_vld_in = 0;\n    repeat (" << L + 4 << ") @(negedge ap_clk);\n";
    }
    o << "    if (tb_n != " << V.v.size() << ") begin $display(\"FAIL: %0d results, expected " << V.v.size()
      << "\", tb_n); tb_errors = tb_errors + 1; end\n";
    if (!hs)
      o << "    if (tb_first_out - tb_first_in != " << L << ") begin $display(\"FAIL: latency %0d, expected " << L
      << "\", tb_first_out - tb_first_in); tb_errors = tb_errors + 1; end\n";
    o << "    if (tb_errors == 0) $display(\"PASS: " << f.name << " " << V.v.size() << " inputs, pipeline latency "
      << L << " cycles\");\n    else $display(\"FAIL: " << f.name << " %0d errors\", tb_errors);\n    $finish;\n  end\n";
    o << "  always @(posedge ap_clk) if (!ap_rst && " << (hs ? "out_valid && out_ready" : "ap_ce && ap_vld_out") << ") begin\n    if (tb_first_out < 0) tb_first_out = tb_cyc;\n    case (tb_n)\n";
    for (size_t i = 0; i < V.v.size(); i++) {
      o << "      " << i << ": begin\n";
      for (auto& [v, x] : V.v[i].out) o << "        " << check(em.outPort(f.vars[v]), x, f.vars[v].ty.w, i) << "\n";
      o << "      end\n";
    }
    o << "      default: ;\n    endcase\n    tb_n = tb_n + 1;\n  end\n";
  }
  o << "endmodule\n";
  return o.str();
}

// A C++ program that includes the source and checks the same vectors with
// the host compiler, so the front end is checked against a real C compiler.
std::string cref(const Function& f, const Func& src, const Vectors& V, const std::string& path) {
  std::ostringstream o;
  auto lit = [](uint64_t x) {
    char b[32];
    snprintf(b, sizeof b, "0x%llxULL", (unsigned long long)x);
    return std::string(b);
  };
  for (auto& d : src.params)
    if (!d.ty.native && d.cpath.empty())
      throw Error("mhls: -cref needs standard C types; parameter '" + d.name + "' of '" + f.name + "' is not one");
  o << "// Generated by mhls: checks " << f.name << "() with the host compiler against the reference run.\n"
       "#include <cstdint>\n#include <cstdio>\n#include <tuple>\n#include <type_traits>\n#include \""
    << std::filesystem::absolute(path).string() << "\"\n";
  o << "template <int mhls_n, class mhls_f> struct mhls_arg;\ntemplate <int mhls_n, class mhls_r, class... mhls_a> struct mhls_arg<mhls_n, mhls_r (*)(mhls_a...)> {\n"
       "  using type = std::remove_const_t<std::remove_pointer_t<std::remove_reference_t<std::tuple_element_t<mhls_n, std::tuple<mhls_a...>>>>>;\n};\n"
       "template <class mhls_t> static uint64_t mhls_bits(mhls_t v, int w) { return (uint64_t)v & (w >= 64 ? ~0ull : ((1ull << w) - 1)); }\n";
  o << "int main() {\n  int mhls_errors = 0;\n";
  // The type of C parameter j, and the C expression for (element k of) the
  // flattened parameter i. A struct parameter j lives in mhls_s<j>, the
  // returned struct in mhls_r; anything else in mhls_v<i>.
  auto ctype = [&](int j) { return "mhls_arg<" + std::to_string(j) + ", decltype(&" + f.name + ")>::type"; };
  auto isStruct = [&](size_t i) { return !src.params[i].cpath.empty(); };
  auto isRet = [&](size_t i) { return src.params[i].origParam == -2; };
  auto cexpr = [&](size_t i, long k) {
    const Decl& d = src.params[i];
    if (!isStruct(i)) return "mhls_v" + std::to_string(i) + (k >= 0 ? "[" + std::to_string(k) + "]" : "");
    std::string e = isRet(i) ? "mhls_r" : "mhls_s" + std::to_string(d.origParam);
    std::vector<int> dims;
    if (f.params[i].k == Param::Array) dims = f.arrs[f.params[i].id].dims;
    long div = 1;
    for (int x : dims) div *= x;
    size_t di = 0;
    for (size_t c = 0; c < d.cpath.size(); c++) {
      if (d.cpath.compare(c, 2, "[]") == 0) {
        div /= dims[di];
        e += "[" + std::to_string((k / div) % dims[di]) + "]";
        di++;
        c++;
      } else {
        e += d.cpath[c];
      }
    }
    return e;
  };
  std::set<int> declared;
  for (size_t i = 0; i < f.params.size(); i++) {
    const Param& pm = f.params[i];
    const Decl& d = src.params[i];
    if (isStruct(i)) {
      if (isRet(i) || !declared.insert(d.origParam).second) continue;
      o << "  static " << ctype(d.origParam) << " mhls_s" << d.origParam;
      if (d.outer) o << "[" << f.arrs[pm.id].dims[0] << "]";
      o << "{};\n";
      continue;
    }
    std::string T = ctype(d.origParam);
    if (pm.k == Param::Array && f.arrs[pm.id].mode == Arr::Stream) {
      o << "  static " << T << " mhls_v" << i << ";\n";
    } else if (pm.k == Param::Array) {
      const Arr& a = f.arrs[pm.id];
      o << "  static " << T << " mhls_v" << i << "[" << a.size << "] = {";
      for (auto x : V.memInit.at(pm.id)) o << "(" << T << ")" << lit(x) << ", ";
      o << "};\n";
    } else if (pm.k == Param::Ref) {
      o << "  static " << T << " mhls_v" << i << " = 0;\n";
    }
  }
  for (size_t i = 0; i < f.params.size(); i++) {
    if (!isStruct(i) || isRet(i) || f.params[i].k != Param::Array) continue;
    if (f.arrs[f.params[i].id].mode == Arr::Stream) continue;
    const auto& init = V.memInit.at(f.params[i].id);
    for (size_t k = 0; k < init.size(); k++) o << "  " << cexpr(i, (long)k) << " = " << lit(init[k]) << ";\n";
  }
  for (size_t n = 0; n < V.v.size(); n++) {
    const Vec& vec = V.v[n];
    o << "  {\n";
    std::vector<std::string> arg(src.origKind.size());
    for (size_t i = 0; i < f.params.size(); i++) {
      const Param& pm = f.params[i];
      const Decl& d = src.params[i];
      if (isRet(i)) continue;
      if (isStruct(i)) {
        if (pm.k != Param::Array && vec.in.count(pm.id)) o << "    " << cexpr(i, -1) << " = " << lit(vec.in.at(pm.id)) << ";\n";
        arg[d.origParam] = (src.origKind[d.origParam] == 2 ? "&mhls_s" : "mhls_s") + std::to_string(d.origParam);
        continue;
      }
      std::string T = ctype(d.origParam);
      std::string a = "mhls_v" + std::to_string(i);
      if (pm.k == Param::Scalar) a = "(" + T + ")" + lit(vec.in.at(pm.id));
      else if (pm.k == Param::Ref) {
        if (vec.in.count(pm.id)) o << "    mhls_v" << i << " = (" << T << ")" << lit(vec.in.at(pm.id)) << ";\n";
        if (d.isPtr) a = "&" + a;
      }
      arg[d.origParam] = a;
    }
    // A stream holds exactly what the reference run took from it.
    auto streamOf = [&](int ai) {
      for (size_t i = 0; i < f.params.size(); i++)
        if (f.params[i].k == Param::Array && f.params[i].id == ai) return "mhls_v" + std::to_string(i);
      return std::string();
    };
    for (auto& [ai, xs] : vec.sIn)
      for (auto x : xs) o << "    " << streamOf(ai) << ".write((decltype(" << streamOf(ai) << ".read()))" << lit(x) << ");\n";
    std::string args;
    for (size_t j = 0; j < arg.size(); j++) args += (j ? ", " : "") + arg[j];
    if (f.retVar >= 0 || src.retStruct) o << "    auto mhls_r = " << f.name << "(" << args << ");\n";
    else o << "    " << f.name << "(" << args << ");\n";
    auto chk = [&](const std::string& what, const std::string& e, uint64_t x, int w) {
      o << "    if (mhls_bits(" << e << ", " << w << ") != " << lit(x) << ") { printf(\"FAIL: call " << n << ": " << what
        << " = %llx, expected %llx\\n\", (unsigned long long)mhls_bits(" << e << ", " << w << "), (unsigned long long)" << lit(x)
        << "); mhls_errors++; }\n";
    };
    for (auto& [v, x] : vec.out) {
      if (f.vars[v].port == Var::Ret) chk("return", "mhls_r", x, f.vars[v].ty.w);
      else
        for (size_t i = 0; i < f.params.size(); i++)
          if (f.params[i].k == Param::Ref && f.params[i].id == v) chk(f.vars[v].name, cexpr(i, -1), x, f.vars[v].ty.w);
    }
    for (auto& [ai, xs] : vec.sIn)
      o << "    if (!" << streamOf(ai) << ".empty()) { printf(\"FAIL: call " << n << ": " << f.arrs[ai].name
        << " was not read as often as expected\\n\"); mhls_errors++; while (!" << streamOf(ai) << ".empty()) " << streamOf(ai)
        << ".read(); }\n";
    for (auto& [ai, xs] : vec.sOut) {
      if (!f.arrs[ai].written) continue;
      o << "    if (" << streamOf(ai) << ".size() != " << xs.size() << ") { printf(\"FAIL: call " << n << ": " << f.arrs[ai].name
        << " got %u elements, expected " << xs.size() << "\\n\", " << streamOf(ai) << ".size()); mhls_errors++; }\n";
      for (size_t k = 0; k < xs.size(); k++)
        chk(f.arrs[ai].name + " element " + std::to_string(k), streamOf(ai) + ".read()", xs[k], f.arrs[ai].ety.w);
      o << "    while (!" << streamOf(ai) << ".empty()) " << streamOf(ai) << ".read();\n";
    }
    for (auto& [ai, k, x] : vec.memChg)
      for (size_t i = 0; i < f.params.size(); i++)
        if (f.params[i].k == Param::Array && f.params[i].id == ai)
          chk(f.arrs[ai].name + "[" + std::to_string(k) + "]", cexpr(i, k), x, f.arrs[ai].ety.w);
    o << "  }\n";
  }
  for (size_t i = 0; i < f.params.size(); i++) {
    if (f.params[i].k != Param::Array || isRet(i)) continue;
    const Arr& a = f.arrs[f.params[i].id];
    if (a.mode == Arr::Stream) continue;
    const auto& fin = V.memFinal.at(f.params[i].id);
    if (isStruct(i)) {
      for (size_t k = 0; k < fin.size(); k++)
        o << "  if (mhls_bits(" << cexpr(i, (long)k) << ", " << a.ety.w << ") != " << lit(fin[k]) << ") { printf(\"FAIL: final "
          << a.name << "[" << k << "]\\n\"); mhls_errors++; }\n";
      continue;
    }
    o << "  {\n    static const uint64_t e[] = {";
    for (auto x : fin) o << lit(x) << ", ";
    o << "};\n    for (int k = 0; k < " << a.size << "; k++) if (mhls_bits(mhls_v" << i << "[k], " << a.ety.w
      << ") != e[k]) { printf(\"FAIL: final " << a.name << "[%d]\\n\", k); mhls_errors++; }\n  }\n";
  }
  o << "  if (mhls_errors == 0) printf(\"PASS: " << f.name << " " << V.v.size() << " calls match the host compiler\\n\");\n"
       "  return mhls_errors != 0;\n}\n";
  return o.str();
}

// ---- driver --------------------------------------------------------------------

struct Job {
  const Func* src = nullptr;
  Function fn;
  Sched sc;
  std::string sv, summary, json;
};

Function lowerAndClean(const Program& P, const Func& src, const Options& opt, std::vector<bool>& entryLive) {
  Lowerer lw(P, opt);
  Function f = lw.lowerTop(src);
  pruneBlocks(f);
  entryLive = deadCode(f);
  pruneBlocks(f);
  entryLive = deadCode(f);
  narrow(f);
  balance(f);
  // Ports and registers.
  std::vector<bool> written(f.vars.size(), false), read(f.vars.size(), false);
  for (auto& B : f.blocks) {
    for (auto& [v, x] : B.writes) written[v] = true;
    for (int id : B.ops)
      if (f.ops[id].k == OpK::ReadVar) read[f.ops[id].ref] = true;
  }
  for (size_t v = 0; v < f.vars.size(); v++) {
    Var& x = f.vars[v];
    if (x.port == Var::Ref) {
      x.portIn = entryLive[v] && read[v];
      x.portOut = written[v];
      if (!x.portIn && !x.portOut) x.portIn = true;   // unused: keep it as an input
    }
    x.reg = written[v] || read[v] || x.port == Var::Ret;
    if (x.port == Var::None && !x.isStatic && entryLive[v] && read[v] && x.user)
      f.warnings.push_back(P.file + ": warning: '" + x.name + "' may be used before it is set in '" + f.name + "' (reads 0 after reset)");
  }
  return f;
}

// -dump-ir: the scheduled control/data-flow graph, for debugging mhls itself.
void dumpIr(const Function& f, std::ostream& o) {
  for (size_t b = 0; b < f.blocks.size(); b++) {
    const Block& B = f.blocks[b];
    o << "block " << b << " (states " << B.base << ".." << B.base + B.len - 1 << ")\n";
    for (int id : B.ops) {
      const Op& p = f.ops[id];
      o << "  n" << id << ":" << p.w << " = " << opName(p.k);
      if (p.k == OpK::Const) o << " " << p.imm;
      if (p.k == OpK::ReadVar) o << " " << f.vars[p.ref].name;
      if (p.k == OpK::Load || p.k == OpK::Store || p.k == OpK::RomRd) o << " " << f.arrs[p.ref].name;
      for (int x : {p.a, p.b, p.c})
        if (x >= 0) o << " n" << x;
      o << "   @" << p.st << (p.cyc > 1 ? "+" + std::to_string(p.cyc - 1) : "") << " t=" << p.tout << "\n";
    }
    for (size_t i = 0; i < B.writes.size(); i++)
      o << "  " << f.vars[B.writes[i].first].name << " <- n" << B.writes[i].second << "   @" << B.wst[i] << "\n";
    const Term& t = B.term;
    if (t.k == Term::Ret) o << "  ret\n";
    else if (t.k == Term::Jump) o << "  jump " << t.t << "\n";
    else o << "  br n" << t.cond << " ? " << t.t << " : " << t.f << "\n";
  }
}

std::string fmtLat(const Sched& sc) {
  if (sc.latMin < 0) return "data-dependent";
  if (sc.latMin == sc.latMax) return std::to_string(sc.latMin);
  return std::to_string(sc.latMin) + ".." + std::to_string(sc.latMax);
}

void summarize(Job& j, const Cfg& cfg) {
  const Function& f = j.fn;
  Sched& sc = j.sc;
  for (auto& v : f.vars)
    if (v.reg) { sc.regs++; sc.regBits += v.ty.w; }
  for (auto& [w, n] : sc.temps) { sc.regs += n; sc.regBits += w * n; }
  for (auto& B : f.blocks) {
    if (!B.pipeL) continue;
    std::set<int> written;
    for (auto& [v, x] : B.writes) written.insert(v);
    for (int id : B.ops) {
      const Op& o = f.ops[id];
      if (o.k == OpK::Const || (o.k == OpK::ReadVar && !written.count(o.ref))) continue;
      int n = std::max(0, o.lastUse - availSt(o));
      sc.regs += n;
      sc.regBits += n * o.w;
    }
    sc.regs += 2 * B.pipeL - 1;
    sc.regBits += 2 * B.pipeL - 1;
    if (B.pipeII > 1) {
      sc.regs++;
      for (int k = 1; k < B.pipeII; k *= 2) sc.regBits++;
    }
  }
  if (f.pipeline) {
    sc.regs = 0;
    sc.regBits = 0;
    for (auto& v : f.vars)
      if (v.reg) { sc.regs++; sc.regBits += v.ty.w; }
    for (int id : f.blocks[0].ops) {
      const Op& o = f.ops[id];
      if (o.k == OpK::Const) continue;
      if (o.k == OpK::ReadVar && (f.vars[o.ref].isStatic || f.vars[o.ref].port == Var::None)) continue;
      int n = std::max(0, o.lastUse - availSt(o));
      sc.regs += n;
      sc.regBits += n * o.w;
    }
  }
  sc.area += 6.0 * sc.regBits;
  std::ostringstream s;
  int shared = 0;
  for (auto& fu : sc.fus) shared += fu.users.size() > 1;
  s << "mhls: " << f.name << ": " << (f.pipeline ? "pipeline" : "FSM") << ", clock " << cfg.clock << " ns, ";
  if (f.pipeline) s << f.blocks[0].len << " stage" << (f.blocks[0].len == 1 ? "" : "s");
  else s << sc.nStates << " states, latency " << fmtLat(sc) << " cycles";
  s << "\n  registers: " << sc.regs << " (" << sc.regBits << " bits)\n  operators:";
  if (sc.opCount.empty()) s << " none";
  for (auto& [k, n] : sc.opCount) s << " " << n << " " << k;
  if (shared) s << " (" << shared << " shared unit" << (shared == 1 ? "" : "s") << ")";
  if (sc.multicycle) s << "\n  multi-cycle operators: " << sc.multicycle << " (constrain them as multicycle paths)";
  for (auto& n : sc.loopNotes) s << "\n  " << n;
  s << "\n  estimated area: " << (long)std::llround(sc.area) << " gate equivalents\n";
  j.summary = s.str();
  std::ostringstream js;
  js << "{\"top\": \"" << f.name << "\", \"architecture\": \"" << (f.pipeline ? "pipeline" : "fsm") << "\", \"clock_ns\": "
     << cfg.clock << ", \"states\": " << (f.pipeline ? f.blocks[0].len : sc.nStates) << ", \"latency_min\": " << sc.latMin
     << ", \"latency_max\": " << sc.latMax << ", \"registers\": " << sc.regs << ", \"register_bits\": " << sc.regBits
     << ", \"shared_units\": " << shared << ", \"multicycle\": " << sc.multicycle << ", \"pipelined_loops\": " << sc.pipedLoops << ", \"area_ge\": "
     << (long)std::llround(sc.area) << ", \"operators\": {";
  bool first = true;
  for (auto& [k, n] : sc.opCount) {
    js << (first ? "" : ", ") << "\"" << k << "\": " << n;
    first = false;
  }
  js << "}}";
  j.json = js.str();
}

int usage() {
  std::cerr << "usage: mhls <file.c>... [-top <f>[,<g>]] [-o <out.sv|dir>] [-clock <ns>] [-pipeline]\n"
               "            [-unroll-all] [-unroll-max <n>] [-pipeline-loops] [-mul <n>] [-div <n>] [-no-share] [-handshake]\n"
               "            [-max-reg-array <n>] [-delay-scale <x>] [-tb <file>] [-cref <file>]\n"
               "            [-vectors <n>] [-seed <n>] [-explore <ns,...>] [-report <file>]\n"
               "            [-clang] [-emit-c <file>] [-I<dir>] [-D<name>[=<v>]] [-threads <n>|all] [-q]\n";
  return 2;
}

}   // namespace

int main(int argc, char** argv) {
  std::vector<std::string> files, tops;
  std::string out, tb, crefOut, report, emitC;
  std::vector<std::string> clangArgs;
  bool useClang = false;
  std::vector<double> explore;
  std::map<std::string, std::string> defs;
  Options opt;
  Cfg cfg;
  int nvec = 20;
  uint64_t seed = 1;
  bool quiet = false, dump = false;
  g_threads = par::defaultThreads();
  auto split = [](const std::string& s) {
    std::vector<std::string> r;
    std::stringstream ss(s);
    for (std::string t; std::getline(ss, t, ',');)
      if (!t.empty()) r.push_back(t);
    return r;
  };
  try {
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      auto val = [&]() -> std::string {
        if (i + 1 >= argc) throw Error("mhls: " + a + " needs a value");
        return argv[++i];
      };
      if (a == "-top") { for (auto& t : split(val())) tops.push_back(t); }
      else if (a == "-o") out = val();
      else if (a == "-clock") cfg.clock = std::stod(val());
      else if (a == "-pipeline") opt.pipeline = true;
      else if (a == "-unroll-all") opt.unrollAll = true;
      else if (a == "-pipeline-loops") opt.pipelineLoops = true;
      else if (a == "-unroll-max") opt.unrollMax = std::stoi(val());
      else if (a == "-mul") cfg.mulLimit = std::stoi(val());
      else if (a == "-div") cfg.divLimit = std::stoi(val());
      else if (a == "-no-share") cfg.share = false;
      else if (a == "-handshake") cfg.handshake = true;
      else if (a == "-max-reg-array") opt.maxRegArray = std::stoi(val());
      else if (a == "-delay-scale") cfg.scale = std::stod(val());
      else if (a == "-tb") tb = val();
      else if (a == "-cref") crefOut = val();
      else if (a == "-clang") useClang = true;
      else if (a == "-emit-c") { emitC = val(); useClang = true; }
      else if (a.rfind("-I", 0) == 0) clangArgs.push_back(a.size() > 2 ? a : "-I" + val());
      else if (a == "-vectors") nvec = std::stoi(val());
      else if (a == "-seed") seed = std::stoull(val());
      else if (a == "-explore") { for (auto& t : split(val())) explore.push_back(std::stod(t)); }
      else if (a == "-report") report = val();
      else if (a == "-threads") {
        std::string n = val();
        int k = n == "all" ? (int)par::defaultThreads() : std::stoi(n);
        if (k < 1) throw Error("mhls: -threads must be at least 1");
        g_threads = (unsigned)k;
      }
      else if (a == "-q") quiet = true;
      else if (a == "-dump-ir") dump = true;
      else if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
      else if (a.rfind("-D", 0) == 0 && a.size() > 2) {
        auto eq = a.find('=');
        defs[a.substr(2, eq == std::string::npos ? eq : eq - 2)] = eq == std::string::npos ? "1" : a.substr(eq + 1);
        clangArgs.push_back(a);
      }
      else if (a[0] == '-') { std::cerr << "mhls: unknown option " << a << "\n"; return usage(); }
      else files.push_back(a);
    }
    if (files.empty()) return usage();
    if (cfg.clock <= kTcq * cfg.scale + kTsu * cfg.scale) throw Error("mhls: -clock is shorter than a register's own delay");

    Program P;
    for (auto& fn : files) {
      if (!useClang) { parseFile(P, fn, defs); continue; }
      std::string c = ClangFront().run(fn, clangArgs, tops);
      if (!emitC.empty()) {
        std::ofstream os(emitC, &fn == &files[0] ? std::ios::out : std::ios::app);
        if (!os) throw Error("mhls: cannot write " + emitC);
        os << c;
      }
      parseText(P, fn, c, {});
    }
    if (tops.empty()) {
      for (auto& f : P.funcs)
        if (f.top) tops.push_back(f.name);
      if (tops.empty() && !P.funcs.empty()) tops.push_back(P.funcs.back().name);
    }
    if (tops.empty()) throw Error("mhls: no function to synthesize");
    std::vector<Job> jobs(tops.size());
    for (size_t i = 0; i < tops.size(); i++) {
      jobs[i].src = P.find(tops[i]);
      if (!jobs[i].src) throw Error("mhls: top function '" + tops[i] + "' is not defined");
    }
    if ((!tb.empty() || !crefOut.empty()) && tops.size() != 1) throw Error("mhls: -tb and -cref take one top function");

    // Design-space exploration: every clock period is an independent schedule.
    if (!explore.empty()) {
      const Func& src = *jobs[0].src;
      std::vector<std::string> rows(explore.size());
      parallelFor(explore.size(), [&](size_t i) {
        Cfg c = cfg;
        c.clock = explore[i];
        Job j;
        j.src = &src;
        std::vector<bool> live;
        j.fn = lowerAndClean(P, src, opt, live);
        j.sc = schedule(j.fn, c);
        j.sc.entryLive = live;
        char buf[200];
        try {
          if (j.fn.pipeline) checkPipeline(j.fn, P, src);
          summarize(j, c);
          snprintf(buf, sizeof buf, "%9.2f  %7d  %14s  %9d  %9ld  %10d", explore[i],
                   j.fn.pipeline ? j.fn.blocks[0].len : j.sc.nStates, fmtLat(j.sc).c_str(), j.sc.regBits,
                   (long)std::llround(j.sc.area), j.sc.multicycle);
        } catch (Error&) {
          snprintf(buf, sizeof buf, "%9.2f  not possible at this clock", explore[i]);
        }
        rows[i] = buf;
      });
      std::cout << "mhls: " << src.name << ": clock period sweep\n clock ns  " << (opt.pipeline || src.pipeline ? " stages" : " states")
                << "  latency cycles   reg bits    area GE  multicycle\n";
      for (auto& r : rows) std::cout << r << "\n";
      return 0;
    }

    parallelFor(jobs.size(), [&](size_t i) {
      Job& j = jobs[i];
      std::vector<bool> live;
      j.fn = lowerAndClean(P, *j.src, opt, live);
      j.sc = schedule(j.fn, cfg);
      j.sc.entryLive = live;
      if (j.fn.pipeline) checkPipeline(j.fn, P, *j.src);
      Emitter em(j.fn, j.sc);
      j.sv = j.fn.pipeline ? em.pipe(cfg) : em.fsm(cfg);
      summarize(j, cfg);
    });

    namespace fs = std::filesystem;
    bool dir = tops.size() > 1;
    if (dir && !out.empty()) fs::create_directories(out);
    for (auto& j : jobs) {
      for (auto& w : j.fn.warnings) std::cerr << w << "\n";
      if (dump) dumpIr(j.fn, std::cout);
      std::string path = dir ? (out.empty() ? "" : out + "/") + j.fn.name + ".sv" : out.empty() ? j.fn.name + ".sv" : out;
      std::ofstream os(path);
      if (!os) throw Error("mhls: cannot write " + path);
      os << j.sv;
      if (!quiet) std::cout << j.summary << "  wrote " << path << "\n";
    }
    if (!tb.empty() || !crefOut.empty()) {
      Job& j = jobs[0];
      Vectors V = makeVectors(j.fn, nvec, seed);
      if (!tb.empty()) {
        std::ofstream os(tb);
        if (!os) throw Error("mhls: cannot write " + tb);
        os << testbench(j.fn, j.sc, V, cfg);
        if (!quiet) std::cout << "  wrote " << tb << " (" << nvec << " calls)\n";
      }
      if (!crefOut.empty()) {
        std::ofstream os(crefOut);
        if (!os) throw Error("mhls: cannot write " + crefOut);
        // A base class is a member in what -clang hands over, not in the C++ the host compiles.
        os << std::regex_replace(cref(j.fn, *j.src, V, P.file), std::regex("mhls_base_[A-Za-z0-9_]+\\."), "");
        if (!quiet) std::cout << "  wrote " << crefOut << "\n";
      }
    }
    if (!report.empty()) {
      std::ofstream os(report);
      if (!os) throw Error("mhls: cannot write " + report);
      os << "[";
      for (size_t i = 0; i < jobs.size(); i++) os << (i ? ",\n " : "") << jobs[i].json;
      os << "]\n";
    }
  } catch (Error& e) {
    std::cerr << e.what() << "\n";
    return 1;
  } catch (std::exception& e) {
    std::cerr << "mhls: " << e.what() << "\n";
    return 2;
  }
  return 0;
}
