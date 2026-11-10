// mhls middle end: the control/data-flow IR, lowering from the AST
// (inlining, if-conversion, loop unrolling, array scalarization), cleanup
// passes (liveness, dead code, bit-width narrowing) and a reference
// interpreter used to produce expected values for the generated testbench.
#pragma once
#include <algorithm>
#include <functional>
#include <tuple>

#include "hls_front.hpp"

namespace hls {

enum class OpK : uint8_t {
  Const, ReadVar, Add, Sub, Mul, UDiv, SDiv, URem, SRem, And, Or, Xor, Shl, LShr, AShr,
  Eq, Ne, ULt, ULe, SLt, SLe, Mux, ZExt, SExt, Trunc, Load, Store, RomRd
};

inline const char* opName(OpK k) {
  static const char* n[] = {"const", "read", "add", "sub", "mul", "udiv", "sdiv", "urem", "srem", "and",
                            "or", "xor", "shl", "lshr", "ashr", "eq", "ne", "ult", "ule", "slt",
                            "sle", "mux", "zext", "sext", "trunc", "load", "store", "rom"};
  return n[(int)k];
}

struct Op {
  OpK k = OpK::Const;
  int w = 32;
  int a = -1, b = -1, c = -1;   // Mux: a=cond b=then c=else. Store: a=index b=value c=predicate
  uint64_t imm = 0;
  int ref = -1;                 // variable (ReadVar) or array (Load/Store/RomRd)
  // Filled in by the scheduler and the binder.
  int st = 0;                   // state within the block (launch state for Load)
  int cyc = 1;                  // states occupied (multi-cycle operators)
  float tout = 0;               // arrival time of the result within its state, ns
  int hold = -1;                // temp register index, or -2 - var when a variable register holds it
  int lastUse = -1;
  int fu = -1;                  // shared functional unit
};

struct Var {
  std::string name;
  Type ty;
  bool isStatic = false;
  uint64_t init = 0;
  enum Port { None, In, Ref, Ret } port = None;
  bool portIn = false, portOut = false;   // decided after liveness
  bool reg = false;                       // has a register
  bool user = true;                       // named in the source
};

struct Arr {
  std::string name;
  Type ety;
  int size = 0;
  std::vector<int> dims;
  enum Mode { Reg, Mem, Rom } mode = Mem;
  bool isParam = false, isStatic = false;
  bool read = false, written = false;
  std::vector<uint64_t> init;
  std::vector<int> elems;   // Reg mode: one variable per element
  int aw() const {
    int n = 1;
    while ((1ll << n) < size) n++;
    return n;
  }
};

struct Term {
  enum K { Jump, Br, Ret } k = Ret;
  int cond = -1, t = -1, f = -1;
};

struct Block {
  std::vector<int> ops;
  std::vector<std::pair<int, int>> writes;   // variable <- value at the end of the block
  Term term;
  int len = 1, base = 0;                     // schedule: states, first global state
  std::vector<int> wst;                      // state of each write
};

struct Param {
  enum K { Scalar, Ref, Array } k = Scalar;
  int id = -1;
};

struct Function {
  std::string name;
  std::vector<Op> ops;
  std::vector<Block> blocks;
  std::vector<Var> vars;
  std::vector<Arr> arrs;
  std::vector<Param> params;
  int retVar = -1;
  bool pipeline = false;
  std::vector<std::string> warnings;
};

inline uint64_t maskW(uint64_t v, int w) { return w >= 64 ? v : (v & ((1ull << w) - 1)); }
inline int64_t sxW(uint64_t v, int w) {
  if (w >= 64) return (int64_t)v;
  v = maskW(v, w);
  if (v >> (w - 1)) v |= ~((1ull << w) - 1);
  return (int64_t)v;
}

// One definition of every operator, used for constant folding and by the
// interpreter. `wa` is the width of operand a. Division by zero is defined
// the way the generated hardware defines it: quotient all ones, remainder
// the dividend. Shifts by the width or more give 0 (or the sign).
inline uint64_t evalOp(OpK k, int w, int wa, uint64_t a, uint64_t b, uint64_t c) {
  uint64_t r = 0;
  switch (k) {
    case OpK::Add: r = a + b; break;
    case OpK::Sub: r = a - b; break;
    case OpK::Mul: r = a * b; break;
    case OpK::UDiv: r = maskW(b, w) == 0 ? ~0ull : maskW(a, w) / maskW(b, w); break;
    case OpK::URem: r = maskW(b, w) == 0 ? a : maskW(a, w) % maskW(b, w); break;
    case OpK::SDiv: {
      int64_t x = sxW(a, w), y = sxW(b, w);
      r = y == 0 ? ~0ull : y == -1 ? (uint64_t)0 - (uint64_t)x : (uint64_t)(x / y);
      break;
    }
    case OpK::SRem: {
      int64_t x = sxW(a, w), y = sxW(b, w);
      r = y == 0 ? a : y == -1 ? 0 : (uint64_t)(x % y);
      break;
    }
    case OpK::And: r = a & b; break;
    case OpK::Or: r = a | b; break;
    case OpK::Xor: r = a ^ b; break;
    case OpK::Shl: r = b >= (uint64_t)w ? 0 : a << b; break;
    case OpK::LShr: r = b >= (uint64_t)wa ? 0 : maskW(a, wa) >> b; break;
    case OpK::AShr: r = b >= (uint64_t)wa ? (sxW(a, wa) < 0 ? ~0ull : 0) : (uint64_t)(sxW(a, wa) >> b); break;
    case OpK::Eq: r = maskW(a, wa) == maskW(b, wa); break;
    case OpK::Ne: r = maskW(a, wa) != maskW(b, wa); break;
    case OpK::ULt: r = maskW(a, wa) < maskW(b, wa); break;
    case OpK::ULe: r = maskW(a, wa) <= maskW(b, wa); break;
    case OpK::SLt: r = sxW(a, wa) < sxW(b, wa); break;
    case OpK::SLe: r = sxW(a, wa) <= sxW(b, wa); break;
    case OpK::Mux: r = (a & 1) ? b : c; break;
    case OpK::ZExt: r = maskW(a, wa); break;
    case OpK::SExt: r = (uint64_t)sxW(a, wa); break;
    case OpK::Trunc: r = a; break;
    default: break;
  }
  return maskW(r, w);
}

struct Options {
  bool unrollAll = false;
  int unrollMax = 4096;      // most iterations a loop may unroll to
  int maxRegArray = 64;      // local arrays up to this many elements become registers
  bool pipeline = false;
};

struct NoConv {};   // thrown when a region cannot be if-converted or unrolled

class Lowerer {
 public:
  Lowerer(const Program& p, const Options& o) : P(p), opt(o) {}

  Function lowerTop(const Func& f) {
    S = State{};
    S.fn.name = f.name;
    S.fn.pipeline = f.pipeline || opt.pipeline;
    S.fn.blocks.emplace_back();
    S.scopes.emplace_back();
    for (auto& [n, v] : P.consts) {
      Sym s;
      s.k = Sym::Konst;
      s.kv = v;
      s.ty = P.constTypes.at(n);
      S.scopes[0][n] = s;
    }
    for (auto& d : P.globals) declare(d, true);
    S.scopes.emplace_back();
    for (auto& d : f.params) {
      Param pm;
      Sym s;
      if (d.name.empty()) fail(P.file, d.line, "parameters of the top function need names");
      if (d.isArray) {
        Arr a;
        a.name = d.name;
        a.ety = d.ty;
        a.size = 1;
        for (auto* e : d.dims) {
          int n = (int)cval(e);
          if (n <= 0) fail(P.file, d.line, "array '" + d.name + "' needs a positive size");
          a.dims.push_back(n);
          a.size *= n;
        }
        a.mode = Arr::Mem;
        a.isParam = true;
        S.fn.arrs.push_back(a);
        pm.k = Param::Array;
        pm.id = (int)S.fn.arrs.size() - 1;
        s.k = Sym::Array;
      } else {
        int v = newVar(d.name, d.ty, true);
        S.fn.vars[v].port = d.isRef ? Var::Ref : Var::In;
        pm.k = d.isRef ? Param::Ref : Param::Scalar;
        pm.id = v;
        s.k = Sym::Scalar;
      }
      s.id = pm.id;
      s.ty = d.ty;
      S.scopes.back()[d.name] = s;
      S.fn.params.push_back(pm);
    }
    CallCtx cc;
    cc.retTy = f.ret;
    if (f.ret.w) {
      cc.retVar = newVar("ap_return", f.ret, true);
      S.fn.vars[cc.retVar].port = Var::Ret;
      S.fn.retVar = cc.retVar;
    }
    cc.last = f.body->kids.empty() ? nullptr : f.body->kids.back();
    cc.loopBase = 0;
    if (earlyRet(f.body, cc.last)) {
      cc.flagVar = newVar("returned", boolT(), false);
      setVar(cc.flagVar, konst(0, 1));
    }
    S.calls.push_back(cc);
    S.callStack.push_back(f.name);
    try {
      lowerStmt(f.body);
    } catch (NoConv&) {
      fail(P.file, f.line, "internal: unhandled conversion failure");
    }
    Term t;
    t.k = Term::Ret;
    if (S.calls.back().retBlock >= 0) {
      Term j;
      j.k = Term::Jump;
      j.t = S.calls.back().retBlock;
      finish(j);
      start(j.t);
    }
    finish(t);
    return std::move(S.fn);
  }

 private:
  const Program& P;
  Options opt;

  struct Sym {
    enum K { Scalar, Array, Konst } k = Scalar;
    int id = -1;
    uint64_t kv = 0;
    Type ty;
  };
  struct LoopCtx {
    int brk = -1, cont = -1;
    bool unrolled = false, isSwitch = false;
  };
  struct CallCtx {
    int retVar = -1, retBlock = -1;
    int flagVar = -1;        // 1 once a return under a condition has been taken
    bool predRet = false;    // such a return has been lowered
    const Stmt* last = nullptr;
    Type retTy;
    size_t loopBase = 0;
  };
  using Key = std::tuple<int, int, int, int, int, uint64_t, int>;
  struct State {
    Function fn;
    int cur = 0;
    std::map<int, int> env, entry;
    std::map<Key, int> cse;
    std::vector<std::map<std::string, Sym>> scopes;
    int pred = -1;
    int noPredRet = 0;       // returns under a condition must branch, not predicate
    std::vector<LoopCtx> loops;
    std::vector<CallCtx> calls;
    std::vector<std::string> callStack;
    std::map<const Decl*, Sym> statics;
    std::map<std::string, int> names;
  } S;

  struct TV {
    int v = -1;
    Type ty;
  };
  struct LV {
    int var = -1, arr = -1, idx = -1;
    Type ty;
  };

  // ---- IR building -------------------------------------------------------
  Op& op(int id) { return S.fn.ops[id]; }
  bool isC(int id) { return op(id).k == OpK::Const; }
  uint64_t cv(int id) { return op(id).imm; }

  int push(const Op& o) {
    S.fn.ops.push_back(o);
    int id = (int)S.fn.ops.size() - 1;
    S.fn.blocks[S.cur].ops.push_back(id);
    return id;
  }

  int konst(uint64_t v, int w) {
    Op o;
    o.k = OpK::Const;
    o.w = w;
    o.imm = maskW(v, w);
    return pure(o);
  }

  int pure(const Op& o) {
    Key key{(int)o.k, o.w, o.a, o.b, o.c, o.imm, o.ref};
    auto it = S.cse.find(key);
    if (it != S.cse.end()) return it->second;
    int id = push(o);
    S.cse[key] = id;
    return id;
  }

  static bool commutes(OpK k) {
    return k == OpK::Add || k == OpK::Mul || k == OpK::And || k == OpK::Or || k == OpK::Xor ||
           k == OpK::Eq || k == OpK::Ne;
  }

  int bin(OpK k, int a, int b) {
    bool cmp = k >= OpK::Eq && k <= OpK::SLe;
    int wa = op(a).w, w = cmp ? 1 : wa;
    if (isC(a) && isC(b)) return konst(evalOp(k, w, wa, cv(a), cv(b), 0), w);
    if (commutes(k) && isC(a)) std::swap(a, b);
    if (isC(b)) {
      uint64_t c = cv(b);
      bool shiftish = k == OpK::Shl || k == OpK::LShr || k == OpK::AShr;
      if (c == 0 && (k == OpK::Add || k == OpK::Sub || k == OpK::Or || k == OpK::Xor || shiftish)) return a;
      if (c == 0 && (k == OpK::Mul || k == OpK::And)) return konst(0, w);
      if (c == 1 && (k == OpK::Mul || k == OpK::UDiv)) return a;
      if (k == OpK::And && c == maskW(~0ull, w)) return a;
      if ((k == OpK::Mul || k == OpK::UDiv || k == OpK::URem) && c && (c & (c - 1)) == 0) {
        int sh = 0;
        while ((1ull << sh) != c) sh++;
        if (k == OpK::Mul) return bin(OpK::Shl, a, konst(sh, w));
        if (k == OpK::UDiv) return bin(OpK::LShr, a, konst(sh, w));
        return bin(OpK::And, a, konst(c - 1, w));
      }
    }
    if (a == b) {
      if (k == OpK::Eq || k == OpK::ULe || k == OpK::SLe) return konst(1, 1);
      if (k == OpK::Ne || k == OpK::ULt || k == OpK::SLt) return konst(0, 1);
      if (k == OpK::Sub || k == OpK::Xor) return konst(0, w);
      if (k == OpK::And || k == OpK::Or) return a;
    }
    // !!x on a single bit
    if (k == OpK::Xor && w == 1 && isC(b) && cv(b) == 1 && op(a).k == OpK::Xor && isC(op(a).b) &&
        cv(op(a).b) == 1)
      return op(a).a;
    Op o;
    o.k = k;
    o.w = w;
    o.a = a;
    o.b = b;
    return pure(o);
  }

  int mux(int c, int a, int b) {
    if (isC(c)) return cv(c) ? a : b;
    if (a == b) return a;
    Op o;
    o.k = OpK::Mux;
    o.w = op(a).w;
    o.a = c;
    o.b = a;
    o.c = b;
    return pure(o);
  }

  int ext(OpK k, int a, int w) {
    int wa = op(a).w;
    if (w == wa) return a;
    if (isC(a)) return konst(evalOp(k, w, wa, cv(a), 0, 0), w);
    if (k == OpK::Trunc) {
      OpK ak = op(a).k;
      if (ak == OpK::Trunc) return ext(OpK::Trunc, op(a).a, w);
      if (ak == OpK::ZExt || ak == OpK::SExt) {
        int inner = op(a).a;
        if (op(inner).w >= w) return ext(OpK::Trunc, inner, w);
        return ext(ak, inner, w);
      }
    }
    if (k == OpK::ZExt && op(a).k == OpK::ZExt) a = op(a).a;
    if (k == OpK::SExt && op(a).k == OpK::SExt) a = op(a).a;
    if (k == OpK::SExt && op(a).k == OpK::ZExt) { k = OpK::ZExt; a = op(a).a; }
    Op o;
    o.k = k;
    o.w = w;
    o.a = a;
    return pure(o);
  }

  int notB(int c) { return bin(OpK::Xor, c, konst(1, 1)); }

  // ---- variables ---------------------------------------------------------
  int newVar(const std::string& name, Type ty, bool user) {
    Var v;
    int& n = S.names[name];
    v.name = n ? name + "_" + std::to_string(n) : name;
    while (n && S.names.count(v.name)) v.name = name + "_" + std::to_string(++n);
    n++;
    if (v.name != name) S.names[v.name] = 1;
    v.ty = ty;
    v.user = user;
    S.fn.vars.push_back(v);
    return (int)S.fn.vars.size() - 1;
  }

  int entryVal(int v) {
    auto it = S.entry.find(v);
    if (it != S.entry.end()) return it->second;
    Op o;
    o.k = OpK::ReadVar;
    o.w = S.fn.vars[v].ty.w;
    o.ref = v;
    int id = push(o);
    S.entry[v] = id;
    return id;
  }
  int getVar(int v) {
    auto it = S.env.find(v);
    if (it != S.env.end()) return it->second;
    int e = entryVal(v);
    S.env[v] = e;
    return e;
  }
  void setVar(int v, int val) { S.env[v] = val; }

  int newBlock() {
    S.fn.blocks.emplace_back();
    return (int)S.fn.blocks.size() - 1;
  }
  void finish(Term t) {
    Block& b = S.fn.blocks[S.cur];
    for (auto& [v, val] : S.env) {
      auto it = S.entry.find(v);
      if (it == S.entry.end() || it->second != val) b.writes.push_back({v, val});
    }
    b.term = t;
    S.env.clear();
    S.entry.clear();
    S.cse.clear();
  }
  void start(int b) { S.cur = b; }
  void jumpTo(int b) {
    Term t;
    t.k = Term::Jump;
    t.t = b;
    finish(t);
  }
  void branch(int c, int t, int f) {
    Term tm;
    if (isC(c)) {
      tm.k = Term::Jump;
      tm.t = cv(c) ? t : f;
    } else {
      tm.k = Term::Br;
      tm.cond = c;
      tm.t = t;
      tm.f = f;
    }
    finish(tm);
  }

  // Run f with the predicate narrowed by c, then put the variable state back.
  template <class F>
  std::map<int, int> predicated(int c, F f) {
    std::map<int, int> env0 = S.env;
    int pred0 = S.pred;
    S.pred = pred0 < 0 ? c : bin(OpK::And, pred0, c);
    f();
    S.pred = pred0;
    std::map<int, int> r = std::move(S.env);
    S.env = std::move(env0);
    return r;
  }
  void merge(int c, const std::map<int, int>& envT, const std::map<int, int>& envE) {
    std::set<int> keys;
    for (auto& [v, x] : envT) keys.insert(v);
    for (auto& [v, x] : envE) keys.insert(v);
    for (int v : keys) {
      auto t = envT.find(v), e = envE.find(v);
      int a = t != envT.end() ? t->second : entryVal(v);
      int b = e != envE.end() ? e->second : entryVal(v);
      S.env[v] = mux(c, a, b);
    }
  }

  // ---- types -------------------------------------------------------------
  static Type boolT() { return {1, false, true}; }
  static Type prom(Type t) {
    if (!t.native && t.w < 64) return {64, true, true};
    if (t.w < 32) return {32, true, true};
    if (t.w > 32 && t.w < 64) return {64, true, true};
    return {t.w, t.s, true};
  }
  static Type usual(Type a, Type b) {
    a = prom(a);
    b = prom(b);
    if (a.w == b.w) return {a.w, a.s && b.s, true};
    return a.w > b.w ? a : b;
  }
  TV conv(TV x, Type to) {
    if (to.w == 0) return {x.v, to};
    if (to.w == 1 && !to.s && to.native) {
      if (x.ty.w == 1) return {x.v, to};
      return {bin(OpK::Ne, x.v, konst(0, op(x.v).w)), to};
    }
    int w = op(x.v).w;
    if (to.w < w) return {ext(OpK::Trunc, x.v, to.w), to};
    if (to.w > w) return {ext(x.ty.s ? OpK::SExt : OpK::ZExt, x.v, to.w), to};
    return {x.v, to};
  }
  int toBool(TV x) { return conv(x, boolT()).v; }

  // A call can end the current block (loops, early returns), and values do
  // not live across blocks. A value that is needed after such a call is
  // parked in a variable; without a block change this costs nothing.
  int spill(TV x) {
    int v = newVar("tmp", Type{op(x.v).w, x.ty.s, true}, false);
    setVar(v, x.v);
    return v;
  }

  // ---- symbols -----------------------------------------------------------
  Sym* lookup(const std::string& n) {
    for (size_t i = S.scopes.size(); i-- > 0;) {
      auto it = S.scopes[i].find(n);
      if (it != S.scopes[i].end()) return &it->second;
    }
    return nullptr;
  }

  uint64_t cval(Expr* e) {
    TV x = rv(e);
    if (!isC(x.v)) fail(P.file, e->line, "expected a constant expression");
    return (uint64_t)(x.ty.s ? sxW(cv(x.v), x.ty.w) : (int64_t)cv(x.v));
  }

  void declare(const Decl& d, bool global) {
    Sym s;
    s.ty = d.ty;
    if (d.isStatic && !global) {
      auto it = S.statics.find(&d);
      if (it != S.statics.end()) {
        S.scopes.back()[d.name] = it->second;
        return;
      }
    }
    if (d.isRef) fail(P.file, d.line, "local pointers and references are not supported");
    if (d.isArray) {
      Arr a;
      a.name = uniq(d.name);
      a.ety = d.ty;
      a.size = 1;
      a.isStatic = d.isStatic;
      for (auto* e : d.dims) {
        int64_t n = (int64_t)cval(e);
        if (n <= 0 || n > (1 << 24)) fail(P.file, d.line, "array '" + d.name + "' has a bad size");
        a.dims.push_back((int)n);
        a.size *= (int)n;
      }
      std::vector<TV> init;
      std::vector<int> parked;
      bool calls = false;
      for (auto* e : d.initList) calls = calls || hasCall(e);
      for (auto* e : d.initList) {
        init.push_back(conv(rv(e), d.ty));
        if (calls) parked.push_back(spill(init.back()));
      }
      for (size_t i = 0; i < parked.size(); i++) init[i].v = getVar(parked[i]);
      if ((int)init.size() > a.size) fail(P.file, d.line, "too many initializers for '" + d.name + "'");
      bool allConst = true;
      for (auto& x : init) allConst = allConst && isC(x.v);
      if (d.isConst && d.hasInitList && allConst) {
        a.mode = Arr::Rom;
        a.init.assign(a.size, 0);
        for (size_t i = 0; i < init.size(); i++) a.init[i] = cv(init[i].v);
      } else if (a.size <= opt.maxRegArray) {
        a.mode = Arr::Reg;
        if (d.isStatic && !allConst) fail(P.file, d.line, "static '" + d.name + "' needs constant initializers");
        for (int i = 0; i < a.size; i++) {
          int v = newVar(d.name + "_" + std::to_string(i), d.ty, false);
          a.elems.push_back(v);
          if (d.isStatic) {
            S.fn.vars[v].isStatic = true;
            S.fn.vars[v].init = i < (int)init.size() ? cv(init[i].v) : 0;
          } else if (d.hasInitList) {
            setVar(v, i < (int)init.size() ? init[i].v : konst(0, d.ty.w));
          }
        }
      } else {
        a.mode = Arr::Mem;
        if (d.isStatic && d.hasInitList) {
          for (auto& x : init)
            if (!isC(x.v) || cv(x.v) != 0)
              fail(P.file, d.line, "static array '" + d.name + "' is a RAM and can only start at zero; "
                                   "make it const for a ROM or raise -max-reg-array");
        }
      }
      S.fn.arrs.push_back(a);
      s.k = Sym::Array;
      s.id = (int)S.fn.arrs.size() - 1;
      if (a.mode == Arr::Mem && !d.isStatic && d.hasInitList) {
        for (int i = 0; i < a.size; i++) {
          LV lv;
          lv.arr = s.id;
          lv.idx = konst(i, a.aw());
          lv.ty = d.ty;
          write(lv, i < (int)init.size() ? init[i].v : konst(0, d.ty.w), d.line);
        }
      }
    } else if (d.isStatic) {
      int v = newVar(d.name, d.ty, true);
      S.fn.vars[v].isStatic = true;
      if (d.init) {
        TV x = conv(rv(d.init), d.ty);
        if (!isC(x.v)) fail(P.file, d.line, "static '" + d.name + "' needs a constant initializer");
        S.fn.vars[v].init = cv(x.v);
      }
      s.k = Sym::Scalar;
      s.id = v;
    } else {
      int v = newVar(d.name, d.ty, true);
      s.k = Sym::Scalar;
      s.id = v;
      if (d.init) {
        TV x = conv(rv(d.init), d.ty);
        setVar(v, x.v);
        if (d.isConst && isC(x.v)) {
          s.k = Sym::Konst;
          s.kv = cv(x.v);
        }
      }
    }
    if (d.isStatic && !global) S.statics[&d] = s;
    S.scopes.back()[d.name] = s;
  }

  std::string uniq(const std::string& name) {
    int& n = S.names["@" + name];
    std::string r = n ? name + "_" + std::to_string(n) : name;
    n++;
    return r;
  }

  // ---- lvalues -----------------------------------------------------------
  LV lvalue(Expr* e) {
    LV lv;
    if (e->k == Expr::Var) {
      Sym* s = lookup(e->name);
      if (!s) fail(P.file, e->line, "'" + e->name + "' is not declared");
      if (s->k != Sym::Scalar) fail(P.file, e->line, "'" + e->name + "' is not an assignable scalar");
      lv.var = s->id;
      lv.ty = s->ty;
      return lv;
    }
    if (e->k == Expr::Un && e->op == "*") return lvalue(e->a);
    if (e->k == Expr::Idx) {
      std::vector<Expr*> idx;
      Expr* base = e;
      while (base->k == Expr::Idx) {
        idx.push_back(base->b);
        base = base->a;
      }
      std::reverse(idx.begin(), idx.end());
      if (base->k != Expr::Var) fail(P.file, e->line, "only named arrays can be indexed");
      Sym* s = lookup(base->name);
      if (!s) fail(P.file, e->line, "'" + base->name + "' is not declared");
      if (s->k != Sym::Array) fail(P.file, e->line, "'" + base->name + "' is not an array");
      const Arr& a = S.fn.arrs[s->id];
      std::vector<int> dims = a.dims;
      Type ety = a.ety;
      int size = a.size, aw = a.aw();
      if (idx.size() != dims.size())
        fail(P.file, e->line, "'" + base->name + "' needs " + std::to_string(dims.size()) + " indices");
      int acc = -1;
      Type u32 = {32, false, true};
      for (size_t i = 0; i < idx.size(); i++) {
        int park = acc >= 0 && hasCall(idx[i]) ? spill({acc, u32}) : -1;
        int x = conv(rv(idx[i]), u32).v;
        if (park >= 0) acc = getVar(park);
        acc = acc < 0 ? x : bin(OpK::Add, bin(OpK::Mul, acc, konst(dims[i], 32)), x);
      }
      if (isC(acc) && cv(acc) >= (uint64_t)size)
        fail(P.file, e->line, "index " + std::to_string((int64_t)sxW(cv(acc), 32)) + " is outside '" +
                                  base->name + "[" + std::to_string(size) + "]'");
      lv.arr = s->id;
      lv.idx = ext(OpK::Trunc, acc, aw);
      if (aw > 32) lv.idx = ext(OpK::ZExt, acc, aw);
      lv.ty = ety;
      return lv;
    }
    fail(P.file, e->line, "not an assignable expression");
  }

  int read(const LV& lv, int line) {
    if (lv.var >= 0) return getVar(lv.var);
    Arr& a = S.fn.arrs[lv.arr];
    a.read = true;
    if (a.mode == Arr::Reg) {
      if (isC(lv.idx)) return getVar(a.elems[cv(lv.idx)]);
      std::vector<int> v;
      for (int e : a.elems) v.push_back(getVar(e));
      int bit = 0, aw = a.aw();
      while (v.size() > 1) {
        int sel = ext(OpK::Trunc, bin(OpK::LShr, lv.idx, konst(bit, aw)), 1);
        std::vector<int> n;
        for (size_t i = 0; i + 1 < v.size(); i += 2) n.push_back(mux(sel, v[i + 1], v[i]));
        if (v.size() & 1) n.push_back(v.back());
        v = n;
        bit++;
      }
      return v[0];
    }
    if (a.mode == Arr::Rom) {
      if (isC(lv.idx)) return konst(a.init[cv(lv.idx)], a.ety.w);
      Op o;
      o.k = OpK::RomRd;
      o.w = a.ety.w;
      o.a = lv.idx;
      o.ref = lv.arr;
      return pure(o);
    }
    (void)line;
    Op o;
    o.k = OpK::Load;
    o.w = a.ety.w;
    o.a = lv.idx;
    o.ref = lv.arr;
    return push(o);
  }

  void write(const LV& lv, int val, int line) {
    if (lv.var >= 0) {
      setVar(lv.var, val);
      return;
    }
    Arr& a = S.fn.arrs[lv.arr];
    if (a.mode == Arr::Rom) fail(P.file, line, "'" + a.name + "' is const");
    a.written = true;
    if (a.mode == Arr::Reg) {
      if (isC(lv.idx)) {
        setVar(a.elems[cv(lv.idx)], val);
        return;
      }
      std::vector<int> elems = a.elems;
      int aw = a.aw();
      for (size_t k = 0; k < elems.size(); k++) {
        int hit = bin(OpK::Eq, lv.idx, konst(k, aw));
        setVar(elems[k], mux(hit, val, getVar(elems[k])));
      }
      return;
    }
    Op o;
    o.k = OpK::Store;
    o.w = a.ety.w;
    o.a = lv.idx;
    o.b = val;
    o.c = S.pred;
    o.ref = lv.arr;
    push(o);
  }

  // ---- expressions -------------------------------------------------------
  TV arith(const std::string& o, TV a, TV b, int line) {
    if (o == "<<" || o == ">>") {
      Type t = prom(a.ty);
      a = conv(a, t);
      OpK k = o == "<<" ? OpK::Shl : t.s ? OpK::AShr : OpK::LShr;
      // The amount is compared against the width, so give it the same width.
      int amt = op(b.v).w > t.w ? ext(OpK::Trunc, b.v, t.w) : ext(OpK::ZExt, b.v, t.w);
      return {bin(k, a.v, amt), t};
    }
    Type t = usual(a.ty, b.ty);
    a = conv(a, t);
    b = conv(b, t);
    if (o == "+") return {bin(OpK::Add, a.v, b.v), t};
    if (o == "-") return {bin(OpK::Sub, a.v, b.v), t};
    if (o == "*") return {bin(OpK::Mul, a.v, b.v), t};
    if (o == "/") return {bin(t.s ? OpK::SDiv : OpK::UDiv, a.v, b.v), t};
    if (o == "%") return {bin(t.s ? OpK::SRem : OpK::URem, a.v, b.v), t};
    if (o == "&") return {bin(OpK::And, a.v, b.v), t};
    if (o == "|") return {bin(OpK::Or, a.v, b.v), t};
    if (o == "^") return {bin(OpK::Xor, a.v, b.v), t};
    if (o == "==") return {bin(OpK::Eq, a.v, b.v), boolT()};
    if (o == "!=") return {bin(OpK::Ne, a.v, b.v), boolT()};
    if (o == "<") return {bin(t.s ? OpK::SLt : OpK::ULt, a.v, b.v), boolT()};
    if (o == "<=") return {bin(t.s ? OpK::SLe : OpK::ULe, a.v, b.v), boolT()};
    if (o == ">") return {bin(t.s ? OpK::SLt : OpK::ULt, b.v, a.v), boolT()};
    if (o == ">=") return {bin(t.s ? OpK::SLe : OpK::ULe, b.v, a.v), boolT()};
    fail(P.file, line, "operator '" + o + "' is not supported");
  }

  // The C type of an expression, without evaluating it.
  Type typeOf(Expr* e) {
    switch (e->k) {
      case Expr::Num: case Expr::Cast: return e->ty;
      case Expr::Var: {
        Sym* s = lookup(e->name);
        if (!s) fail(P.file, e->line, "'" + e->name + "' is not declared");
        return s->ty;
      }
      case Expr::Idx: {
        Expr* b = e;
        while (b->k == Expr::Idx) b = b->a;
        Sym* s = b->k == Expr::Var ? lookup(b->name) : nullptr;
        if (!s || s->k != Sym::Array) fail(P.file, e->line, "only named arrays can be indexed");
        return S.fn.arrs[s->id].ety;
      }
      case Expr::Un:
        if (e->op == "!") return boolT();
        if (e->op == "*" || e->op == "++" || e->op == "--") return typeOf(e->a);
        return prom(typeOf(e->a));
      case Expr::Post: case Expr::Asg: return typeOf(e->a);
      case Expr::Cond: {
        Type b = typeOf(e->b), c = typeOf(e->c);
        return b == c ? b : usual(b, c);
      }
      case Expr::Bin: {
        const std::string& o = e->op;
        if (o == ",") return typeOf(e->b);
        if (o == "&&" || o == "||" || o == "==" || o == "!=" || o == "<" || o == ">" || o == "<=" || o == ">=")
          return boolT();
        if (o == "<<" || o == ">>") return prom(typeOf(e->a));
        return usual(typeOf(e->a), typeOf(e->b));
      }
      case Expr::Call: {
        const Func* f = P.find(e->name);
        if (!f) fail(P.file, e->line, "function '" + e->name + "' is not defined");
        return f->ret;
      }
    }
    return Type{};
  }

  TV rv(Expr* e) {
    switch (e->k) {
      case Expr::Num: return {konst(e->v, e->ty.w), e->ty};
      case Expr::Var: {
        Sym* s = lookup(e->name);
        if (!s) fail(P.file, e->line, "'" + e->name + "' is not declared");
        if (s->k == Sym::Konst) return {konst(s->kv, s->ty.w), s->ty};
        if (s->k == Sym::Array) fail(P.file, e->line, "array '" + e->name + "' used as a value");
        return {getVar(s->id), s->ty};
      }
      case Expr::Idx: {
        LV lv = lvalue(e);
        return {read(lv, e->line), lv.ty};
      }
      case Expr::Cast: return conv(rv(e->a), e->ty);
      case Expr::Un: {
        if (e->op == "*") {
          LV lv = lvalue(e->a);
          return {read(lv, e->line), lv.ty};
        }
        if (e->op == "&") fail(P.file, e->line, "'&' is only supported on call arguments");
        if (e->op == "++" || e->op == "--") {
          LV lv = lvalue(e->a);
          TV old{read(lv, e->line), lv.ty};
          TV one{konst(1, 32), Type{32, true}};
          TV n = conv(arith(e->op == "++" ? "+" : "-", old, one, e->line), lv.ty);
          write(lv, n.v, e->line);
          return n;
        }
        TV x = rv(e->a);
        if (e->op == "!") return {notB(toBool(x)), boolT()};
        Type t = prom(x.ty);
        x = conv(x, t);
        if (e->op == "+") return x;
        if (e->op == "-") return {bin(OpK::Sub, konst(0, t.w), x.v), t};
        if (e->op == "~") return {bin(OpK::Xor, x.v, konst(~0ull, t.w)), t};
        fail(P.file, e->line, "unary '" + e->op + "' is not supported");
      }
      case Expr::Post: {
        LV lv = lvalue(e->a);
        TV old{read(lv, e->line), lv.ty};
        TV one{konst(1, 32), Type{32, true}};
        TV n = conv(arith(e->op == "++" ? "+" : "-", old, one, e->line), lv.ty);
        write(lv, n.v, e->line);
        return old;
      }
      case Expr::Asg: {
        // Right operand first, as C++17 orders it.
        TV rhs = rv(e->b);
        int park = hasCall(e->a) ? spill(rhs) : -1;
        LV lv = lvalue(e->a);
        if (park >= 0) rhs.v = getVar(park);
        TV r;
        if (e->op == "=") {
          r = conv(rhs, lv.ty);
        } else {
          TV old{read(lv, e->line), lv.ty};
          r = conv(arith(e->op.substr(0, e->op.size() - 1), old, rhs, e->line), lv.ty);
        }
        write(lv, r.v, e->line);
        return r;
      }
      case Expr::Cond: {
        int c = toBool(rv(e->a));
        if (isC(c)) {
          // The result type still comes from both arms.
          Type tb = typeOf(e->b), tc = typeOf(e->c);
          return conv(rv(cv(c) ? e->b : e->c), tb == tc ? tb : usual(tb, tc));
        }
        TV x, y;
        bool risky = hasCall(e->b) || hasCall(e->c);
        std::unique_ptr<State> snap;
        if (risky) snap = std::make_unique<State>(S);
        try {
          auto envT = predicated(c, [&] { x = rv(e->b); });
          auto envE = predicated(notB(c), [&] { y = rv(e->c); });
          merge(c, envT, envE);
        } catch (NoConv&) {
          if (!snap) throw;
          S = std::move(*snap);
          if (S.pred >= 0) throw;
          // Branch instead: each arm leaves its value in a variable.
          int vc = spill({c, boolT()});
          int T = newBlock(), E = newBlock(), J = newBlock();
          branch(c, T, E);
          start(T);
          x = rv(e->b);
          int vx = spill(x);
          jumpTo(J);
          start(E);
          y = rv(e->c);
          int vy = spill(y);
          jumpTo(J);
          start(J);
          c = getVar(vc);
          x.v = getVar(vx);
          y.v = getVar(vy);
        }
        Type t = usual(x.ty, y.ty);
        if (x.ty == y.ty) t = x.ty;
        return {mux(c, conv(x, t).v, conv(y, t).v), t};
      }
      case Expr::Bin: {
        if (e->op == ",") {
          rv(e->a);
          return rv(e->b);
        }
        if (e->op == "&&" || e->op == "||") {
          bool isAnd = e->op == "&&";
          int a = toBool(rv(e->a));
          if (isC(a)) {
            if ((cv(a) != 0) != isAnd) return {konst(!isAnd, 1), boolT()};
            return {toBool(rv(e->b)), boolT()};
          }
          int b = -1;
          int guard = isAnd ? a : notB(a);
          std::unique_ptr<State> snap;
          if (hasCall(e->b)) snap = std::make_unique<State>(S);
          try {
            std::map<int, int> env0 = S.env;
            auto envT = predicated(guard, [&] { b = toBool(rv(e->b)); });
            merge(guard, envT, env0);
          } catch (NoConv&) {
            if (!snap) throw;
            S = std::move(*snap);
            if (S.pred >= 0) throw;
            // Branch instead: the right operand only runs when it decides the result.
            int res = spill({konst(!isAnd, 1), boolT()});
            int T = newBlock(), J = newBlock();
            branch(guard, T, J);
            start(T);
            setVar(res, toBool(rv(e->b)));
            jumpTo(J);
            start(J);
            return {getVar(res), boolT()};
          }
          return {bin(isAnd ? OpK::And : OpK::Or, a, b), boolT()};
        }
        TV a = rv(e->a);
        int park = hasCall(e->b) ? spill(a) : -1;
        TV b = rv(e->b);
        if (park >= 0) a.v = getVar(park);
        return arith(e->op, a, b, e->line);
      }
      case Expr::Call: return call(e);
    }
    fail(P.file, e->line, "unsupported expression");
  }

  TV call(Expr* e) {
    const Func* f = P.find(e->name);
    if (!f) fail(P.file, e->line, "function '" + e->name + "' is not defined");
    if (std::find(S.callStack.begin(), S.callStack.end(), f->name) != S.callStack.end())
      fail(P.file, e->line, "recursion ('" + f->name + "') is not synthesizable");
    if (e->args.size() != f->params.size())
      fail(P.file, e->line, "'" + f->name + "' takes " + std::to_string(f->params.size()) + " arguments");
    std::map<std::string, Sym> ps;
    for (size_t i = 0; i < f->params.size(); i++) {
      const Decl& d = f->params[i];
      Expr* a = e->args[i];
      Sym s;
      s.ty = d.ty;
      if (d.isArray) {
        Sym* as = a->k == Expr::Var ? lookup(a->name) : nullptr;
        if (!as || as->k != Sym::Array) fail(P.file, a->line, "argument " + std::to_string(i + 1) + " of '" + f->name + "' must be an array");
        s = *as;
      } else if (d.isRef) {
        Expr* t = a;
        if (t->k == Expr::Un && t->op == "&") t = t->a;
        LV lv = lvalue(t);
        if (lv.var < 0) fail(P.file, a->line, "an array element cannot be passed by reference");
        s.k = Sym::Scalar;
        s.id = lv.var;
        s.ty = lv.ty;
      } else {
        TV x = conv(rv(a), d.ty);
        int v = newVar(d.name, d.ty, false);
        setVar(v, x.v);
        s.k = Sym::Scalar;
        s.id = v;
      }
      ps[d.name] = s;
    }
    auto saved = std::move(S.scopes);
    S.scopes.clear();
    S.scopes.push_back(saved[0]);
    S.scopes.push_back(std::move(ps));
    CallCtx cc;
    cc.retTy = f->ret;
    if (f->ret.w) cc.retVar = newVar(f->name + "_ret", f->ret, false);
    cc.last = f->body->kids.empty() ? nullptr : f->body->kids.back();
    cc.loopBase = S.loops.size();
    if (earlyRet(f->body, cc.last)) {
      cc.flagVar = newVar("returned", boolT(), false);
      setVar(cc.flagVar, konst(0, 1));
    }
    S.calls.push_back(cc);
    S.callStack.push_back(f->name);
    lowerStmt(f->body);
    cc = S.calls.back();
    if (cc.retBlock >= 0) {
      jumpTo(cc.retBlock);
      start(cc.retBlock);
    }
    S.calls.pop_back();
    S.callStack.pop_back();
    // Statics declared in the callee stay known through S.statics.
    saved[0] = S.scopes[0];
    S.scopes = std::move(saved);
    if (cc.retVar < 0) return {konst(0, 1), Type{0, false}};
    return {getVar(cc.retVar), f->ret};
  }

  // ---- statements --------------------------------------------------------
  bool wantUnroll(const Stmt* s) const { return s->unroll || opt.unrollAll; }

  // True when the statement can be turned into predicated assignments.
  bool simple(const Stmt* s) const {
    if (!s) return true;
    switch (s->k) {
      case Stmt::Brk: case Stmt::Cont: case Stmt::Do: return false;
      case Stmt::Ret: return true;   // a return under a condition sets a flag; lowering decides
      case Stmt::While: case Stmt::For: return wantUnroll(s) && simple(s->body) && !hasRet(s->body);
      case Stmt::If: return simple(s->body) && simple(s->els);
      case Stmt::Switch: {
        for (size_t i = 0; i < s->cases.size(); i++)
          for (auto* k : caseBody(s, i))
            if (!simple(k)) return false;
        return true;
      }
      case Stmt::Blk:
        for (auto* k : s->kids)
          if (!simple(k)) return false;
        return true;
      default: return true;
    }
  }
  static bool hasCall(const Expr* e) {
    if (!e) return false;
    if (e->k == Expr::Call) return true;
    for (auto* a : e->args)
      if (hasCall(a)) return true;
    return hasCall(e->a) || hasCall(e->b) || hasCall(e->c);
  }
  // The statements case i runs: its own and, falling through, the next
  // cases', up to the first break or return at the top level.
  static std::vector<const Stmt*> caseBody(const Stmt* sw, size_t i) {
    std::vector<const Stmt*> r;
    for (size_t j = i; j < sw->cases.size(); j++)
      for (auto* k : sw->cases[j].body) {
        if (k->k == Stmt::Brk) return r;
        r.push_back(k);
        if (k->k == Stmt::Ret) return r;
      }
    return r;
  }
  static bool hasRet(const Stmt* s) {
    if (!s) return false;
    if (s->k == Stmt::Ret) return true;
    for (auto* k : s->kids)
      if (hasRet(k)) return true;
    for (auto& c : s->cases)
      for (auto* k : c.body)
        if (hasRet(k)) return true;
    return hasRet(s->init) || hasRet(s->body) || hasRet(s->els);
  }
  // A return other than the function's last statement.
  static bool earlyRet(const Stmt* s, const Stmt* last) {
    if (!s) return false;
    if (s->k == Stmt::Ret) return s != last;
    for (auto* k : s->kids)
      if (earlyRet(k, last)) return true;
    for (auto& c : s->cases)
      for (auto* k : c.body)
        if (earlyRet(k, last)) return true;
    return earlyRet(s->init, last) || earlyRet(s->body, last) || earlyRet(s->els, last);
  }
  static bool mayFail(const Stmt* s) {
    if (!s) return false;
    if (s->k == Stmt::For || s->k == Stmt::While || s->k == Stmt::Ret) return true;
    for (auto& c : s->cases) {
      for (auto* l : c.labels)
        if (hasCall(l)) return true;
      for (auto* k : c.body)
        if (mayFail(k)) return true;
    }
    if (hasCall(s->e) || hasCall(s->cond) || hasCall(s->inc)) return true;
    for (auto& d : s->decls) {
      if (hasCall(d.init)) return true;
      for (auto* x : d.initList)
        if (hasCall(x)) return true;
    }
    for (auto* k : s->kids)
      if (mayFail(k)) return true;
    return mayFail(s->init) || mayFail(s->body) || mayFail(s->els);
  }

  void scoped(const Stmt* s) {
    if (!s) return;
    S.scopes.emplace_back();
    lowerStmt(s);
    S.scopes.pop_back();
  }

  // Inside a loop of the function being lowered (a switch is not a loop).
  bool inLoop() const {
    for (size_t i = S.calls.back().loopBase; i < S.loops.size(); i++)
      if (!S.loops[i].isSwitch) return true;
    return false;
  }
  bool inUnrolled() const {
    for (size_t i = S.calls.back().loopBase; i < S.loops.size(); i++)
      if (S.loops[i].unrolled) return true;
    return false;
  }

  // if (c) T else E: multiplexers when both sides can be predicated,
  // otherwise a branch in the control flow.
  void lowerBranch(int c, const std::function<void()>& T, const std::function<void()>& E, bool convert, bool risky) {
    if (isC(c)) {
      (cv(c) ? T : E)();
      return;
    }
    if (convert) {
      std::unique_ptr<State> snap;
      if (risky) snap = std::make_unique<State>(S);
      try {
        auto envT = predicated(c, T);
        auto envE = predicated(notB(c), E);
        merge(c, envT, envE);
        return;
      } catch (NoConv&) {
        if (!snap) throw;
        S = std::move(*snap);
      }
    }
    if (S.pred >= 0) throw NoConv{};
    int tb = newBlock(), eb = newBlock(), jb = newBlock();
    branch(c, tb, eb);
    start(tb);
    T();
    jumpTo(jb);
    start(eb);
    E();
    jumpTo(jb);
    start(jb);
  }

  void lowerIf(const Stmt* s) {
    int c = toBool(rv(s->cond));
    lowerBranch(c, [&] { scoped(s->body); }, [&] { scoped(s->els); }, simple(s->body) && simple(s->els),
                mayFail(s->body) || mayFail(s->els));
  }

  void lowerSwitch(const Stmt* s) {
    TV sel = rv(s->cond);
    int selVar = spill(sel);
    bool convert = simple(s), risky = mayFail(s);
    LoopCtx lc;
    lc.isSwitch = true;
    S.loops.push_back(lc);
    size_t depth = S.loops.size();
    std::vector<size_t> order;   // cases with labels in source order, the default last
    for (size_t i = 0; i < s->cases.size(); i++)
      if (!s->cases[i].labels.empty()) order.push_back(i);
    int dflt = -1;
    for (size_t i = 0; i < s->cases.size(); i++)
      if (s->cases[i].isDefault) dflt = (int)i;
    auto body = [&](size_t i) {
      S.scopes.emplace_back();
      lowerSeq(caseBody(s, i), 0);
      S.scopes.pop_back();
    };
    std::function<void(size_t)> chain = [&](size_t n) {
      if (n == order.size()) {
        if (dflt >= 0) body((size_t)dflt);
        return;
      }
      size_t i = order[n];
      int c = konst(0, 1);
      for (auto* l : s->cases[i].labels) {
        TV lab = rv(l);
        c = bin(OpK::Or, c, arith("==", TV{getVar(selVar), sel.ty}, lab, l->line).v);
      }
      // A default that sits on this case's fall-through path is part of body(i) already.
      lowerBranch(c, [&] { body(i); }, [&] { chain(n + 1); }, convert, risky);
    };
    chain(0);
    lc = S.loops[depth - 1];
    S.loops.resize(depth - 1);
    if (lc.brk >= 0) {
      jumpTo(lc.brk);
      start(lc.brk);
    }
  }

  void lowerLoop(const Stmt* s) {
    S.scopes.emplace_back();
    if (s->init) lowerStmt(s->init);
    bool isDo = s->k == Stmt::Do;
    if (!isDo && wantUnroll(s)) {
      State snap = S;
      try {
        LoopCtx lc;
        lc.unrolled = true;
        S.loops.push_back(lc);
        for (int n = 0;; n++) {
          int c = s->cond ? toBool(rv(s->cond)) : konst(1, 1);
          if (!isC(c)) throw NoConv{};
          if (!cv(c)) break;
          if (n >= opt.unrollMax) throw NoConv{};
          scoped(s->body);
          if (s->inc) rv(s->inc);
        }
        S.loops.pop_back();
        S.scopes.pop_back();
        return;
      } catch (NoConv&) {
        S = std::move(snap);
        if (s->unroll)
          S.fn.warnings.push_back(P.file + ":" + std::to_string(s->line) +
                                  ": warning: loop not unrolled (the trip count is not a constant, or the "
                                  "body has break/continue/return)");
      }
    }
    if (S.pred >= 0) throw NoConv{};
    int body = newBlock(), exit = newBlock();
    if (isDo) {
      jumpTo(body);
    } else {
      int c = s->cond ? toBool(rv(s->cond)) : konst(1, 1);
      branch(c, body, exit);
    }
    start(body);
    LoopCtx lc;
    lc.brk = exit;
    S.loops.push_back(lc);
    scoped(s->body);
    lc = S.loops.back();
    S.loops.pop_back();
    if (lc.cont >= 0) {
      jumpTo(lc.cont);
      start(lc.cont);
    }
    if (s->inc) rv(s->inc);
    int c = s->cond ? toBool(rv(s->cond)) : konst(1, 1);
    branch(c, body, exit);
    start(exit);
    S.scopes.pop_back();
  }

  // A statement list. After a statement that may have returned under a
  // condition, the rest runs under "not returned"; when the rest cannot be
  // predicated (it has a loop), that statement is lowered again with real
  // branches instead.
  template <class Vec>
  void lowerSeq(const Vec& kids, size_t from) {
    for (size_t i = from; i < kids.size(); i++) {
      bool watch = i + 1 < kids.size() && hasRet(kids[i]) && kids[i]->k != Stmt::Ret &&
                   S.calls.back().flagVar >= 0 && !S.noPredRet;
      if (!watch) {
        lowerStmt(kids[i]);
        continue;
      }
      State snap = S;
      lowerStmt(kids[i]);
      if (!S.calls.back().predRet) continue;
      int fl = getVar(S.calls.back().flagVar);
      if (isC(fl) && !cv(fl)) continue;
      try {
        std::map<int, int> env0 = S.env;
        int go = notB(fl);
        auto envT = predicated(go, [&] { lowerSeq(kids, i + 1); });
        merge(go, envT, env0);
        return;
      } catch (NoConv&) {
        S = std::move(snap);
        S.noPredRet++;
        lowerStmt(kids[i]);
        S.noPredRet--;
      }
    }
  }

  void lowerStmt(const Stmt* s) {
    if (!s) return;
    switch (s->k) {
      case Stmt::Empty: break;
      case Stmt::DeclS:
        for (auto& d : s->decls) declare(d, false);
        break;
      case Stmt::ExprS: rv(s->e); break;
      case Stmt::Blk:
        S.scopes.emplace_back();
        lowerSeq(s->kids, 0);
        S.scopes.pop_back();
        break;
      case Stmt::If: lowerIf(s); break;
      case Stmt::Switch: lowerSwitch(s); break;
      case Stmt::For: case Stmt::While: case Stmt::Do: lowerLoop(s); break;
      case Stmt::Brk: case Stmt::Cont: {
        // break leaves the innermost loop or switch, continue the innermost loop.
        size_t i = S.loops.size(), base = S.calls.back().loopBase;
        while (i > base && s->k == Stmt::Cont && S.loops[i - 1].isSwitch) i--;
        if (i <= base) fail(P.file, s->line, "break/continue outside a loop");
        i--;
        if (S.pred >= 0 || S.loops[i].unrolled) throw NoConv{};
        if (s->k == Stmt::Brk) {
          if (S.loops[i].brk < 0) {
            int b = newBlock();
            S.loops[i].brk = b;
          }
          jumpTo(S.loops[i].brk);
        } else {
          if (S.loops[i].cont < 0) {
            int b = newBlock();
            S.loops[i].cont = b;
          }
          jumpTo(S.loops[i].cont);
        }
        start(newBlock());
        break;
      }
      case Stmt::Ret: {
        TV x;
        if (s->e) x = rv(s->e);   // may call, which grows S.calls
        CallCtx cc = S.calls.back();
        if (s->e) {
          if (cc.retVar < 0) fail(P.file, s->line, "a void function returns a value");
          setVar(cc.retVar, conv(x, cc.retTy).v);
        } else if (cc.retVar >= 0) {
          fail(P.file, s->line, "return needs a value");
        }
        if (s == cc.last) break;
        if (S.pred >= 0) {
          // Under a condition: note that the function has returned, and
          // lowerSeq runs what follows only when it has not.
          if (S.noPredRet || inLoop() || cc.flagVar < 0) throw NoConv{};
          setVar(cc.flagVar, konst(1, 1));
          S.calls.back().predRet = true;
          break;
        }
        if (inUnrolled()) throw NoConv{};
        if (cc.retBlock < 0) {
          int rb = newBlock();
          S.calls.back().retBlock = rb;
        }
        jumpTo(S.calls.back().retBlock);
        start(newBlock());
        break;
      }
    }
  }
};

// ---- cleanup passes --------------------------------------------------------

// Drop unreachable blocks, skip blocks that do nothing, renumber.
inline void pruneBlocks(Function& f) {
  auto empty = [&](int b) {
    const Block& B = f.blocks[b];
    if (!B.writes.empty() || B.term.k != Term::Jump) return false;
    for (int o : B.ops)
      if (f.ops[o].k == OpK::Store) return false;
    return true;
  };
  auto resolve = [&](int b) {
    for (int n = 0; n < (int)f.blocks.size() && empty(b) && f.blocks[b].term.t != b; n++) b = f.blocks[b].term.t;
    return b;
  };
  for (auto& B : f.blocks) {
    if (B.term.k == Term::Ret) continue;
    B.term.t = resolve(B.term.t);
    if (B.term.k == Term::Br) {
      B.term.f = resolve(B.term.f);
      if (B.term.t == B.term.f) {
        B.term.k = Term::Jump;
        B.term.cond = -1;
      }
    }
  }
  int entry = resolve(0);
  std::vector<int> id(f.blocks.size(), -1), order, work{entry};
  while (!work.empty()) {
    int b = work.back();
    work.pop_back();
    if (id[b] >= 0) continue;
    id[b] = 1;
    order.push_back(b);
    const Term& t = f.blocks[b].term;
    if (t.k == Term::Br) work.push_back(t.f);
    if (t.k != Term::Ret) work.push_back(t.t);
  }
  std::sort(order.begin(), order.end());
  // The entry block must come first.
  order.erase(std::find(order.begin(), order.end(), entry));
  order.insert(order.begin(), entry);
  for (size_t i = 0; i < order.size(); i++) id[order[i]] = (int)i;
  std::vector<Block> nb;
  for (int b : order) {
    Block B = std::move(f.blocks[b]);
    if (B.term.k != Term::Ret) B.term.t = id[B.term.t];
    if (B.term.k == Term::Br) B.term.f = id[B.term.f];
    nb.push_back(std::move(B));
  }
  f.blocks = std::move(nb);
}

// Liveness of variables across blocks; removes dead writes and dead operators.
// Returns the variables live on entry.
inline std::vector<bool> deadCode(Function& f) {
  size_t nv = f.vars.size(), nb = f.blocks.size();
  std::vector<bool> exitLive(nv, false);
  for (size_t v = 0; v < nv; v++)
    exitLive[v] = f.vars[v].isStatic || f.vars[v].port == Var::Ref || f.vars[v].port == Var::Ret;
  std::vector<std::vector<bool>> in(nb, std::vector<bool>(nv, false));
  for (;;) {
    // Mark operators reachable from the roots of each block.
    std::vector<bool> liveOp(f.ops.size(), false);
    bool removed = false;
    for (auto& B : f.blocks) {
      auto mark = [&](int v) {
        if (v >= 0) liveOp[v] = true;
      };
      for (auto& [v, val] : B.writes) mark(val);
      mark(B.term.cond);
      for (size_t i = B.ops.size(); i-- > 0;) {
        Op& o = f.ops[B.ops[i]];
        if (o.k == OpK::Store) liveOp[B.ops[i]] = true;
        if (!liveOp[B.ops[i]]) continue;
        mark(o.a);
        mark(o.b);
        mark(o.c);
      }
      size_t n = B.ops.size();
      B.ops.erase(std::remove_if(B.ops.begin(), B.ops.end(), [&](int o) { return !liveOp[o]; }), B.ops.end());
      removed = removed || B.ops.size() != n;
    }
    // Backward dataflow.
    for (auto& v : in) std::fill(v.begin(), v.end(), false);
    std::vector<std::vector<bool>> out(nb, std::vector<bool>(nv, false));
    for (bool changed = true; changed;) {
      changed = false;
      for (size_t bi = nb; bi-- > 0;) {
        Block& B = f.blocks[bi];
        std::vector<bool> o(nv, false);
        if (B.term.k == Term::Ret) o = exitLive;
        else {
          o = in[B.term.t];
          if (B.term.k == Term::Br)
            for (size_t v = 0; v < nv; v++) o[v] = o[v] || in[B.term.f][v];
        }
        std::vector<bool> i = o;
        for (auto& [v, val] : B.writes) i[v] = false;
        for (int op : B.ops)
          if (f.ops[op].k == OpK::ReadVar) i[f.ops[op].ref] = true;
        if (i != in[bi] || o != out[bi]) {
          in[bi] = i;
          out[bi] = o;
          changed = true;
        }
      }
    }
    for (size_t bi = 0; bi < nb; bi++) {
      Block& B = f.blocks[bi];
      size_t n = B.writes.size();
      B.writes.erase(std::remove_if(B.writes.begin(), B.writes.end(),
                                    [&](auto& w) { return !out[bi][w.first]; }),
                     B.writes.end());
      removed = removed || B.writes.size() != n;
    }
    if (!removed) break;
  }
  return in[0];
}

// Demanded-bits narrowing: an operator whose users only look at its low n
// bits is built n bits wide. C promotes everything to int, so this is what
// turns "uint8_t c = a + b" back into an 8-bit adder.
inline void narrow(Function& f) {
  std::vector<int> need(f.ops.size(), 0);
  auto full = [&](int v) {
    if (v >= 0) need[v] = std::max(need[v], f.ops[v].w);
  };
  auto dem = [&](int v, int w) {
    if (v >= 0) need[v] = std::max(need[v], std::min(w, f.ops[v].w));
  };
  for (auto& B : f.blocks) {
    for (auto& [v, val] : B.writes) full(val);
    full(B.term.cond);
    for (size_t i = B.ops.size(); i-- > 0;) {
      int id = B.ops[i];
      Op& o = f.ops[id];
      int n = need[id] ? need[id] : o.w;
      switch (o.k) {
        case OpK::Const:
          o.w = n;
          o.imm = maskW(o.imm, n);
          break;
        case OpK::Add: case OpK::Sub: case OpK::Mul: case OpK::And: case OpK::Or: case OpK::Xor:
          o.w = n;
          dem(o.a, n);
          dem(o.b, n);
          break;
        case OpK::Shl:
          o.w = n;
          dem(o.a, n);
          full(o.b);
          break;
        case OpK::Mux:
          o.w = n;
          full(o.a);
          dem(o.b, n);
          dem(o.c, n);
          break;
        case OpK::Trunc:
          o.w = n;
          dem(o.a, n);
          break;
        case OpK::ZExt: case OpK::SExt:
          o.w = n;
          if (n <= f.ops[o.a].w) {
            o.k = OpK::Trunc;
            dem(o.a, n);
          } else {
            full(o.a);
          }
          break;
        default:
          full(o.a);
          full(o.b);
          full(o.c);
          break;
      }
    }
  }
}

// Tree-height reduction: a chain a+b+c+d... whose partial sums are used
// nowhere else becomes a balanced tree, so a 64-term sum is 6 adders deep
// instead of 63. The same for *, &, | and ^.
inline void balance(Function& f) {
  for (auto& B : f.blocks) {
    std::map<int, int> uses;
    for (int id : B.ops)
      for (int x : {f.ops[id].a, f.ops[id].b, f.ops[id].c})
        if (x >= 0) uses[x]++;
    for (auto& [v, x] : B.writes) uses[x] += 2;   // a written value is a root
    if (B.term.cond >= 0) uses[B.term.cond] += 2;
    std::set<int> gone;
    std::map<int, std::vector<int>> before;       // root -> new operators placed just before it
    for (size_t i = B.ops.size(); i-- > 0;) {
      int root = B.ops[i];
      if (gone.count(root)) continue;
      OpK k = f.ops[root].k;
      int w = f.ops[root].w;
      if (k != OpK::Add && k != OpK::Mul && k != OpK::And && k != OpK::Or && k != OpK::Xor) continue;
      std::vector<int> leaves, inner;
      std::function<void(int)> collect = [&](int x) {
        const Op& o = f.ops[x];
        if (x != root && (o.k != k || o.w != w || uses[x] != 1)) {
          leaves.push_back(x);
          return;
        }
        if (x != root) inner.push_back(x);
        collect(o.a);
        collect(o.b);
      };
      collect(root);
      if (leaves.size() < 4) continue;
      for (int x : inner) gone.insert(x);
      std::vector<int> level = leaves;
      while (level.size() > 2) {
        std::vector<int> next;
        for (size_t j = 0; j + 1 < level.size(); j += 2) {
          Op n;
          n.k = k;
          n.w = w;
          n.a = level[j];
          n.b = level[j + 1];
          f.ops.push_back(n);
          next.push_back((int)f.ops.size() - 1);
          before[root].push_back(next.back());
        }
        if (level.size() & 1) next.push_back(level.back());
        level = next;
      }
      f.ops[root].a = level[0];
      f.ops[root].b = level[1];
    }
    if (gone.empty()) continue;
    std::vector<int> ops;
    for (int id : B.ops) {
      if (gone.count(id)) continue;
      auto it = before.find(id);
      if (it != before.end()) ops.insert(ops.end(), it->second.begin(), it->second.end());
      ops.push_back(id);
    }
    B.ops = std::move(ops);
  }
}

// ---- reference interpreter -------------------------------------------------

struct Machine {
  std::vector<uint64_t> vars;
  std::vector<std::vector<uint64_t>> mems;
  void reset(const Function& f) {
    vars.assign(f.vars.size(), 0);
    for (size_t v = 0; v < f.vars.size(); v++) vars[v] = f.vars[v].init;
    mems.resize(f.arrs.size());
    for (size_t a = 0; a < f.arrs.size(); a++)
      if (f.arrs[a].mode == Arr::Mem) mems[a].assign(f.arrs[a].size, 0);
  }
};

// Runs one call. Returns the number of FSM states visited (the latency in
// cycles between start and done once the blocks have been scheduled).
inline long interpret(const Function& f, Machine& m, long maxSteps = 50'000'000) {
  std::vector<uint64_t> val(f.ops.size(), 0);
  int b = 0;
  long states = 0, steps = 0;
  for (;;) {
    const Block& B = f.blocks[b];
    states += B.len;
    for (int id : B.ops) {
      const Op& o = f.ops[id];
      switch (o.k) {
        case OpK::Const: val[id] = o.imm; break;
        case OpK::ReadVar: val[id] = maskW(m.vars[o.ref], o.w); break;
        case OpK::Load: {
          uint64_t i = maskW(val[o.a], f.ops[o.a].w);
          val[id] = i < m.mems[o.ref].size() ? m.mems[o.ref][i] : 0;
          break;
        }
        case OpK::RomRd: {
          uint64_t i = maskW(val[o.a], f.ops[o.a].w);
          val[id] = i < f.arrs[o.ref].init.size() ? f.arrs[o.ref].init[i] : 0;
          break;
        }
        case OpK::Store: {
          if (o.c >= 0 && !(val[o.c] & 1)) break;
          uint64_t i = maskW(val[o.a], f.ops[o.a].w);
          if (i < m.mems[o.ref].size()) m.mems[o.ref][i] = maskW(val[o.b], o.w);
          break;
        }
        default: {
          int wa = o.a >= 0 ? f.ops[o.a].w : o.w;
          uint64_t a = o.a >= 0 ? val[o.a] : 0, bb = o.b >= 0 ? val[o.b] : 0, c = o.c >= 0 ? val[o.c] : 0;
          // Narrowed operators only look at the low bits of wider operands.
          if (o.k == OpK::Shl) bb = maskW(bb, f.ops[o.b].w);
          if (o.k == OpK::LShr || o.k == OpK::AShr) bb = maskW(bb, f.ops[o.b].w);
          val[id] = evalOp(o.k, o.w, wa, a, bb, c);
        }
      }
    }
    for (auto& [v, x] : B.writes) val[x] = maskW(val[x], f.ops[x].w);
    std::vector<std::pair<int, uint64_t>> wr;
    for (auto& [v, x] : B.writes) wr.push_back({v, maskW(val[x], f.vars[v].ty.w)});
    for (auto& [v, x] : wr) m.vars[v] = x;
    if (B.term.k == Term::Ret) return states;
    if (B.term.k == Term::Jump) b = B.term.t;
    else b = (val[B.term.cond] & 1) ? B.term.t : B.term.f;
    if (++steps > maxSteps) throw Error("mhls: the reference run of '" + f.name + "' does not terminate");
  }
}

}   // namespace hls
