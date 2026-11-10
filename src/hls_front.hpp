// mhls front end: a C/C++ subset lexer, a small preprocessor and a parser.
//
// The subset is what high-level synthesis accepts as untimed C: integer
// types (bool, char .. long long, intN_t, ap_int<N>/ap_uint<N>,
// sc_int<N>/sc_uint<N>, up to 64 bits), fixed-size arrays, functions,
// if/for/while/do, break/continue/return and the full C expression set.
// No pointers beyond scalar output parameters, no structs, no recursion,
// no floating point.
#pragma once
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hls {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};
[[noreturn]] inline void fail(const std::string& file, int line, const std::string& msg) {
  throw Error(file + ":" + std::to_string(line) + ": error: " + msg);
}

// An integer type. w == 0 is void. `native` is false for the ap_/sc_ types,
// which are widened to 64 bits in arithmetic so that a product or a sum of
// two of them does not lose its top bits before it is assigned.
struct Type {
  int w = 32;
  bool s = true;
  bool native = true;
  bool operator==(const Type& o) const { return w == o.w && s == o.s; }
};

struct Expr {
  enum K { Num, Var, Un, Post, Bin, Asg, Cond, Idx, Call, Cast } k = Num;
  std::string op, name;
  Expr *a = nullptr, *b = nullptr, *c = nullptr;
  std::vector<Expr*> args;
  uint64_t v = 0;
  Type ty;
  int line = 0;
};

struct Decl {
  std::string name;
  Type ty;
  bool isConst = false, isStatic = false, isRef = false, isPtr = false;
  bool isArray = false;
  std::vector<Expr*> dims;
  Expr* init = nullptr;
  std::vector<Expr*> initList;
  bool hasInitList = false;
  int line = 0;
};

struct Stmt {
  enum K { DeclS, ExprS, If, For, While, Do, Ret, Brk, Cont, Blk, Switch, Empty } k = Empty;
  struct Case {
    std::vector<Expr*> labels;
    bool isDefault = false;
    std::vector<Stmt*> body;
  };
  std::vector<Case> cases;
  std::vector<Decl> decls;
  Expr *e = nullptr, *cond = nullptr, *inc = nullptr;
  Stmt *init = nullptr, *body = nullptr, *els = nullptr;
  std::vector<Stmt*> kids;
  bool unroll = false;
  int line = 0;
};

struct Func {
  std::string name;
  Type ret;
  std::vector<Decl> params;
  Stmt* body = nullptr;
  bool pipeline = false, top = false;
  int line = 0;
};

struct Program {
  std::string file;
  std::vector<Func> funcs;
  std::vector<Decl> globals;
  std::map<std::string, uint64_t> consts;   // enum constants and const scalars
  std::map<std::string, Type> constTypes;
  std::vector<std::unique_ptr<Expr>> exprs;
  std::vector<std::unique_ptr<Stmt>> stmts;
  const Func* find(const std::string& n) const {
    for (auto& f : funcs)
      if (f.name == n && f.body) return &f;
    return nullptr;
  }
};

struct Tok {
  enum K { Id, Num, Punct, Pragma, End } k = End;
  std::string s;
  uint64_t v = 0;
  Type ty;
  int line = 0;
};

class Lexer {
 public:
  Lexer(std::string file, const std::string& text, std::map<std::string, std::string> defs)
      : file_(std::move(file)) {
    for (auto& [k, v] : defs) macros_[k] = lexLine(v, 0);
    run(text);
    Tok e;
    e.line = line_;
    out.push_back(e);
  }
  std::vector<Tok> out;

 private:
  std::string file_;
  int line_ = 1;
  std::map<std::string, std::vector<Tok>> macros_;

  static bool idStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
  static bool idChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

  std::vector<Tok> lexLine(const std::string& s, int line) {
    std::vector<Tok> r;
    size_t i = 0, n = s.size();
    while (i < n) {
      char c = s[i];
      if (isspace((unsigned char)c)) { i++; continue; }
      Tok t;
      t.line = line;
      if (idStart(c)) {
        size_t j = i;
        while (j < n && idChar(s[j])) j++;
        t.k = Tok::Id;
        t.s = s.substr(i, j - i);
        i = j;
      } else if (isdigit((unsigned char)c)) {
        size_t j = i;
        uint64_t v = 0;
        bool hexlike = false;
        if (c == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
          j += 2;
          hexlike = true;
          while (j < n && isxdigit((unsigned char)s[j])) {
            char d = s[j++];
            v = v * 16 + (isdigit((unsigned char)d) ? d - '0' : (tolower(d) - 'a' + 10));
          }
        } else if (c == '0' && j + 1 < n && (s[j + 1] == 'b' || s[j + 1] == 'B')) {
          j += 2;
          hexlike = true;
          while (j < n && (s[j] == '0' || s[j] == '1')) v = v * 2 + (s[j++] - '0');
        } else if (c == '0') {
          hexlike = true;
          while (j < n && s[j] >= '0' && s[j] <= '7') v = v * 8 + (s[j++] - '0');
        } else {
          while (j < n && isdigit((unsigned char)s[j])) v = v * 10 + (s[j++] - '0');
        }
        if (j < n && (s[j] == '.' || s[j] == 'e' || s[j] == 'E' || s[j] == 'f'))
          fail(file_, line, "floating-point literals are not synthesizable");
        bool u = false;
        int l = 0;
        while (j < n && (s[j] == 'u' || s[j] == 'U' || s[j] == 'l' || s[j] == 'L')) {
          if (s[j] == 'u' || s[j] == 'U') u = true;
          else l++;
          j++;
        }
        t.k = Tok::Num;
        t.v = v;
        // C rules: the first of int, (unsigned), long, unsigned long that fits.
        if (!u && !l && v <= 0x7fffffffull) t.ty = {32, true};
        else if (!l && v <= 0xffffffffull && (u || hexlike)) t.ty = {32, false};
        else if (!u && v <= 0x7fffffffffffffffull) t.ty = {64, true};
        else t.ty = {64, false};
        i = j;
      } else if (c == '\'') {
        size_t j = i + 1;
        uint64_t v = 0;
        if (j < n && s[j] == '\\') {
          j++;
          char e = j < n ? s[j] : 0;
          v = e == 'n' ? 10 : e == 't' ? 9 : e == '0' ? 0 : e == 'r' ? 13 : (unsigned char)e;
          j++;
        } else if (j < n) {
          v = (unsigned char)s[j++];
        }
        if (j >= n || s[j] != '\'') fail(file_, line, "bad character literal");
        t.k = Tok::Num;
        t.v = v;
        t.ty = {32, true};
        i = j + 1;
      } else if (c == '"') {
        fail(file_, line, "string literals are not synthesizable");
      } else {
        static const char* p3[] = {"<<=", ">>=", "..."};
        static const char* p2[] = {"++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||", "+=",
                                   "-=", "*=", "/=", "%=", "&=", "|=", "^=", "->", "::"};
        t.k = Tok::Punct;
        t.s = std::string(1, c);
        for (auto p : p3)
          if (s.compare(i, 3, p) == 0) t.s = p;
        if (t.s.size() == 1)
          for (auto p : p2)
            if (s.compare(i, 2, p) == 0) t.s = p;
        i += t.s.size();
      }
      r.push_back(t);
    }
    return r;
  }

  void expand(const std::vector<Tok>& in, std::vector<Tok>& dst, int depth, int line) {
    for (auto t : in) {
      t.line = line;
      if (t.k == Tok::Id && depth < 32) {
        auto it = macros_.find(t.s);
        if (it != macros_.end()) {
          expand(it->second, dst, depth + 1, line);
          continue;
        }
      }
      dst.push_back(t);
    }
  }

  void run(const std::string& text) {
    // Strip comments, keep newlines, join continuation lines.
    std::string s;
    s.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
      if (text.compare(i, 2, "//") == 0) {
        while (i < text.size() && text[i] != '\n') i++;
      } else if (text.compare(i, 2, "/*") == 0) {
        i += 2;
        while (i < text.size() && text.compare(i, 2, "*/") != 0) {
          if (text[i] == '\n') s += '\n';
          i++;
        }
        i += 2;
        s += ' ';
      } else {
        s += text[i++];
      }
    }
    std::vector<bool> active;   // #if stack
    auto on = [&] {
      for (bool b : active)
        if (!b) return false;
      return true;
    };
    std::istringstream is(s);
    std::string ln;
    while (std::getline(is, ln)) {
      int startLine = line_;
      while (!ln.empty() && (ln.back() == '\r')) ln.pop_back();
      while (!ln.empty() && ln.back() == '\\') {
        ln.pop_back();
        std::string more;
        if (!std::getline(is, more)) break;
        line_++;
        ln += " " + more;
      }
      line_++;
      size_t p = ln.find_first_not_of(" \t");
      if (p != std::string::npos && ln[p] == '#') {
        std::istringstream ls(ln.substr(p + 1));
        std::string d;
        ls >> d;
        if (d == "ifdef" || d == "ifndef") {
          std::string n;
          ls >> n;
          active.push_back(macros_.count(n) ? d == "ifdef" : d == "ifndef");
        } else if (d == "if") {
          std::string rest;
          std::getline(ls, rest);
          auto toks = lexLine(rest, startLine);
          std::vector<Tok> ex;
          expand(toks, ex, 0, startLine);
          active.push_back(ex.size() == 1 && ex[0].k == Tok::Num && ex[0].v != 0);
        } else if (d == "else") {
          if (active.empty()) fail(file_, startLine, "#else without #if");
          active.back() = !active.back();
        } else if (d == "endif") {
          if (active.empty()) fail(file_, startLine, "#endif without #if");
          active.pop_back();
        } else if (!on()) {
        } else if (d == "define") {
          std::string n, rest;
          ls >> n;
          std::getline(ls, rest);
          if (n.find('(') != std::string::npos)
            fail(file_, startLine, "function-like macros are not supported");
          macros_[n] = lexLine(rest, startLine);
        } else if (d == "undef") {
          std::string n;
          ls >> n;
          macros_.erase(n);
        } else if (d == "pragma") {
          std::string w, rest;
          ls >> w;
          for (auto& ch : w) ch = (char)tolower(ch);
          if (w == "hls" || w == "mhls") {
            std::getline(ls, rest);
            for (auto& ch : rest) ch = (char)tolower(ch);
            Tok t;
            t.k = Tok::Pragma;
            t.s = rest;
            t.line = startLine;
            out.push_back(t);
          }
        }
        continue;   // #include and everything else is ignored
      }
      if (!on()) continue;
      expand(lexLine(ln, startLine), out, 0, startLine);
    }
  }
};

class Parser {
 public:
  Parser(Program& p, std::vector<Tok> toks) : P(p), t_(std::move(toks)) {}

  void parseProgram() {
    while (cur().k != Tok::End) topLevel();
  }

 private:
  Program& P;
  std::vector<Tok> t_;
  size_t i_ = 0;
  std::map<std::string, Type> typedefs_;
  bool pendUnroll_ = false, pendPipeline_ = false, pendTop_ = false;

  const Tok& cur() const { return t_[i_]; }
  const Tok& peek(int n = 1) const { return t_[std::min(i_ + n, t_.size() - 1)]; }
  [[noreturn]] void err(const std::string& m) const { fail(P.file, cur().line, m); }
  bool isP(const char* s) const { return cur().k == Tok::Punct && cur().s == s; }
  bool isId(const char* s) const { return cur().k == Tok::Id && cur().s == s; }
  bool accept(const char* s) {
    if (isP(s)) { i_++; return true; }
    return false;
  }
  bool acceptId(const char* s) {
    if (isId(s)) { i_++; return true; }
    return false;
  }
  void expect(const char* s) {
    if (!accept(s)) err(std::string("expected '") + s + "' before '" + cur().s + "'");
  }
  std::string ident() {
    if (cur().k != Tok::Id) err("expected an identifier before '" + cur().s + "'");
    return t_[i_++].s;
  }

  Expr* mk(Expr::K k, int line) {
    P.exprs.push_back(std::make_unique<Expr>());
    Expr* e = P.exprs.back().get();
    e->k = k;
    e->line = line;
    return e;
  }
  Stmt* mkS(Stmt::K k) {
    P.stmts.push_back(std::make_unique<Stmt>());
    Stmt* s = P.stmts.back().get();
    s->k = k;
    s->line = cur().line;
    return s;
  }

  void pragmas() {
    while (cur().k == Tok::Pragma) {
      std::istringstream is(cur().s);
      std::string w;
      is >> w;
      if (w == "unroll") pendUnroll_ = true;
      else if (w == "pipeline") pendPipeline_ = true;
      else if (w == "top") pendTop_ = true;
      i_++;
    }
  }

  // ---- types -------------------------------------------------------------
  bool typeStart() const {
    if (cur().k != Tok::Id) return false;
    static const std::set<std::string> kw = {
        "const", "static", "volatile", "unsigned", "signed", "char", "short", "int", "long",
        "bool", "void", "_Bool", "inline", "register", "extern", "constexpr",
        "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
        "size_t", "ap_int", "ap_uint", "sc_int", "sc_uint", "sc_bigint", "sc_biguint"};
    return kw.count(cur().s) || typedefs_.count(cur().s);
  }

  struct Spec {
    Type ty;
    bool isConst = false, isStatic = false;
  };

  Spec typeSpec() {
    Spec sp;
    bool sawUns = false, sawSig = false, sawBase = false;
    int longs = 0;
    std::string base;
    for (;;) {
      if (cur().k != Tok::Id) break;
      const std::string& s = cur().s;
      if (s == "const" || s == "constexpr") { sp.isConst = true; i_++; }
      else if (s == "static") { sp.isStatic = true; i_++; }
      else if (s == "volatile" || s == "inline" || s == "register" || s == "extern") i_++;
      else if (s == "unsigned") { sawUns = true; i_++; }
      else if (s == "signed") { sawSig = true; i_++; }
      else if (s == "long") { longs++; i_++; }
      else if (s == "char" || s == "short" || s == "int" || s == "bool" || s == "_Bool" || s == "void") {
        if (base.empty() || s != "int") base = base.empty() ? s : base;
        sawBase = true;
        i_++;
      } else if (!sawBase && !longs && !sawUns && !sawSig &&
                 (s == "ap_int" || s == "ap_uint" || s == "sc_int" || s == "sc_uint" ||
                  s == "sc_bigint" || s == "sc_biguint")) {
        bool sg = s == "ap_int" || s == "sc_int" || s == "sc_bigint";
        i_++;
        expect("<");
        Expr* n = binary(11);   // above the relational operators, so '>' ends it
        expect(">");
        uint64_t w = constEval(n);
        if (w < 1 || w > 64) err("bit widths from 1 to 64 are supported, got " + std::to_string(w));
        sp.ty = {(int)w, sg, false};
        sawBase = true;
        base = "ap";
      } else if (!sawBase && !longs && !sawUns && !sawSig && typedefs_.count(s)) {
        sp.ty = typedefs_[s];
        sawBase = true;
        base = "td";
        i_++;
      } else if (!sawBase && !longs && !sawUns && !sawSig) {
        static const std::map<std::string, Type> fixed = {
            {"int8_t", {8, true}},    {"int16_t", {16, true}},  {"int32_t", {32, true}},
            {"int64_t", {64, true}},  {"uint8_t", {8, false}},  {"uint16_t", {16, false}},
            {"uint32_t", {32, false}}, {"uint64_t", {64, false}}, {"size_t", {64, false}}};
        auto it = fixed.find(s);
        if (it == fixed.end()) break;
        sp.ty = it->second;
        sawBase = true;
        base = "td";
        i_++;
      } else {
        break;
      }
    }
    if (base == "ap" || base == "td") return sp;
    if (!sawBase && !longs && !sawUns && !sawSig) err("expected a type before '" + cur().s + "'");
    if (base == "void") sp.ty = {0, false};
    else if (base == "bool" || base == "_Bool") sp.ty = {1, false};
    else if (base == "char") sp.ty = {8, !sawUns};
    else if (base == "short") sp.ty = {16, !sawUns};
    else if (longs) sp.ty = {64, !sawUns};
    else sp.ty = {32, !sawUns};
    return sp;
  }

 public:
  // Constant expressions: array sizes, template widths, enum values.
  uint64_t constEval(Expr* e) {
    auto sx = [](uint64_t v, Type t) -> int64_t {
      if (t.w >= 64) return (int64_t)v;
      uint64_t m = (1ull << t.w) - 1;
      v &= m;
      if (t.s && (v >> (t.w - 1))) v |= ~m;
      return (int64_t)v;
    };
    switch (e->k) {
      case Expr::Num: return e->v;
      case Expr::Var: {
        auto it = P.consts.find(e->name);
        if (it == P.consts.end()) fail(P.file, e->line, "'" + e->name + "' is not a constant");
        return it->second;
      }
      case Expr::Cast: return (uint64_t)sx(constEval(e->a), e->ty);
      case Expr::Un: {
        uint64_t a = constEval(e->a);
        if (e->op == "-") return 0 - a;
        if (e->op == "~") return ~a;
        if (e->op == "!") return !a;
        if (e->op == "+") return a;
        break;
      }
      case Expr::Cond: return constEval(e->a) ? constEval(e->b) : constEval(e->c);
      case Expr::Bin: {
        int64_t a = (int64_t)constEval(e->a), b = (int64_t)constEval(e->b);
        const std::string& o = e->op;
        if (o == "+") return a + b;
        if (o == "-") return a - b;
        if (o == "*") return a * b;
        if (o == "/" || o == "%") {
          if (b == 0) fail(P.file, e->line, "division by zero in a constant expression");
          return o == "/" ? a / b : a % b;
        }
        if (o == "<<") return (uint64_t)a << (b & 63);
        if (o == ">>") return a >> (b & 63);
        if (o == "&") return a & b;
        if (o == "|") return a | b;
        if (o == "^") return a ^ b;
        if (o == "<") return a < b;
        if (o == ">") return a > b;
        if (o == "<=") return a <= b;
        if (o == ">=") return a >= b;
        if (o == "==") return a == b;
        if (o == "!=") return a != b;
        if (o == "&&") return a && b;
        if (o == "||") return a || b;
        break;
      }
      default: break;
    }
    fail(P.file, e->line, "not a constant expression");
  }

 private:
  // ---- expressions -------------------------------------------------------
  Expr* primary() {
    int line = cur().line;
    if (cur().k == Tok::Num) {
      Expr* e = mk(Expr::Num, line);
      e->v = cur().v;
      e->ty = cur().ty;
      i_++;
      return e;
    }
    if (accept("(")) {
      if (typeStart()) {
        Spec sp = typeSpec();
        expect(")");
        Expr* e = mk(Expr::Cast, line);
        e->ty = sp.ty;
        e->a = unary();
        return e;
      }
      Expr* e = expr();
      expect(")");
      return e;
    }
    if (cur().k == Tok::Id) {
      if (isId("true") || isId("false")) {
        Expr* e = mk(Expr::Num, line);
        e->v = cur().s == "true";
        e->ty = {1, false};
        i_++;
        return e;
      }
      if (isId("sizeof")) err("sizeof is not supported");
      if (typeStart()) {   // functional cast: int(x), ap_uint<4>(x)
        Spec sp = typeSpec();
        expect("(");
        Expr* e = mk(Expr::Cast, line);
        e->ty = sp.ty;
        e->a = expr();
        expect(")");
        return e;
      }
      std::string n = ident();
      if (accept("(")) {
        Expr* e = mk(Expr::Call, line);
        e->name = n;
        if (!isP(")"))
          do e->args.push_back(assign());
          while (accept(","));
        expect(")");
        return e;
      }
      Expr* e = mk(Expr::Var, line);
      e->name = n;
      return e;
    }
    err("unexpected '" + cur().s + "' in an expression");
  }

  Expr* postfix() {
    Expr* e = primary();
    for (;;) {
      int line = cur().line;
      if (accept("[")) {
        Expr* x = mk(Expr::Idx, line);
        x->a = e;
        x->b = expr();
        expect("]");
        e = x;
      } else if (isP("++") || isP("--")) {
        Expr* x = mk(Expr::Post, line);
        x->op = cur().s;
        x->a = e;
        i_++;
        e = x;
      } else if (isP(".") || isP("->")) {
        err("structs and member access are not supported");
      } else {
        return e;
      }
    }
  }

  Expr* unary() {
    int line = cur().line;
    for (const char* o : {"-", "+", "!", "~", "*", "&", "++", "--"})
      if (isP(o)) {
        i_++;
        Expr* e = mk(Expr::Un, line);
        e->op = o;
        e->a = unary();
        return e;
      }
    return postfix();
  }

  static int prec(const std::string& o) {
    if (o == "*" || o == "/" || o == "%") return 13;
    if (o == "+" || o == "-") return 12;
    if (o == "<<" || o == ">>") return 11;
    if (o == "<" || o == ">" || o == "<=" || o == ">=") return 10;
    if (o == "==" || o == "!=") return 9;
    if (o == "&") return 8;
    if (o == "^") return 7;
    if (o == "|") return 6;
    if (o == "&&") return 5;
    if (o == "||") return 4;
    return -1;
  }

  Expr* binary(int minPrec) {
    Expr* l = unary();
    for (;;) {
      if (cur().k != Tok::Punct) return l;
      int p = prec(cur().s);
      if (p < minPrec) return l;
      int line = cur().line;
      std::string o = cur().s;
      i_++;
      Expr* r = binary(p + 1);
      Expr* e = mk(Expr::Bin, line);
      e->op = o;
      e->a = l;
      e->b = r;
      l = e;
    }
  }

  Expr* ternary() {
    Expr* c = binary(4);
    int line = cur().line;
    if (!accept("?")) return c;
    Expr* e = mk(Expr::Cond, line);
    e->a = c;
    e->b = expr();
    expect(":");
    e->c = assign();
    return e;
  }

  Expr* assign() {
    Expr* l = ternary();
    int line = cur().line;
    for (const char* o : {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="})
      if (isP(o)) {
        i_++;
        Expr* e = mk(Expr::Asg, line);
        e->op = o;
        e->a = l;
        e->b = assign();
        return e;
      }
    return l;
  }

  Expr* expr() {
    Expr* e = assign();
    while (isP(",")) {
      int line = cur().line;
      i_++;
      Expr* x = mk(Expr::Bin, line);
      x->op = ",";
      x->a = e;
      x->b = assign();
      e = x;
    }
    return e;
  }

  // ---- declarations ------------------------------------------------------
  void initList(Decl& d) {
    // Nested braces are flattened: int m[2][2] = {{1,2},{3,4}}.
    expect("{");
    while (!isP("}")) {
      if (isP("{")) initList(d);
      else d.initList.push_back(assign());
      if (!accept(",")) break;
    }
    expect("}");
  }

  Decl declarator(const Spec& sp, bool param) {
    Decl d;
    d.ty = sp.ty;
    d.isConst = sp.isConst;
    d.isStatic = sp.isStatic;
    d.line = cur().line;
    if (accept("*")) d.isRef = d.isPtr = true;
    else if (accept("&")) d.isRef = true;
    acceptId("const");
    if (isP("*")) err("pointers to pointers are not supported");
    if (cur().k == Tok::Id) d.name = ident();
    else if (!param) err("expected a name before '" + cur().s + "'");
    while (accept("[")) {
      d.isArray = true;
      if (isP("]")) err("array '" + d.name + "' needs a constant size");
      d.dims.push_back(ternary());
      expect("]");
    }
    if (d.isArray && d.isRef) err("arrays of pointers are not supported");
    if (!param && accept("=")) {
      if (isP("{")) {
        d.hasInitList = true;
        initList(d);
      } else {
        d.init = assign();
      }
    }
    if (d.ty.w == 0 && !param) err("variable '" + d.name + "' declared void");
    return d;
  }

  Stmt* declStmt() {
    Stmt* s = mkS(Stmt::DeclS);
    Spec sp = typeSpec();
    do s->decls.push_back(declarator(sp, false));
    while (accept(","));
    expect(";");
    return s;
  }

  Stmt* block() {
    Stmt* s = mkS(Stmt::Blk);
    expect("{");
    while (!isP("}")) {
      if (cur().k == Tok::End) err("missing '}'");
      s->kids.push_back(stmt());
    }
    expect("}");
    return s;
  }

  Stmt* stmt() {
    pragmas();
    if (pendPipeline_ && curFunc_) {
      curFunc_->pipeline = true;
      pendPipeline_ = false;
    }
    if (isP("{")) return block();
    if (accept(";")) return mkS(Stmt::Empty);
    if (isId("if")) {
      Stmt* s = mkS(Stmt::If);
      i_++;
      expect("(");
      s->cond = expr();
      expect(")");
      s->body = stmt();
      if (acceptId("else")) s->els = stmt();
      return s;
    }
    if (isId("for")) {
      Stmt* s = mkS(Stmt::For);
      s->unroll = pendUnroll_;
      pendUnroll_ = false;
      i_++;
      expect("(");
      if (accept(";")) {
      } else if (typeStart()) {
        s->init = declStmt();
      } else {
        s->init = mkS(Stmt::ExprS);
        s->init->e = expr();
        expect(";");
      }
      if (!isP(";")) s->cond = expr();
      expect(";");
      if (!isP(")")) s->inc = expr();
      expect(")");
      innerUnroll(s);
      return s;
    }
    if (isId("while")) {
      Stmt* s = mkS(Stmt::While);
      s->unroll = pendUnroll_;
      pendUnroll_ = false;
      i_++;
      expect("(");
      s->cond = expr();
      expect(")");
      innerUnroll(s);
      return s;
    }
    if (isId("do")) {
      Stmt* s = mkS(Stmt::Do);
      i_++;
      s->body = stmt();
      if (!acceptId("while")) err("expected 'while' after the do body");
      expect("(");
      s->cond = expr();
      expect(")");
      expect(";");
      return s;
    }
    if (isId("return")) {
      Stmt* s = mkS(Stmt::Ret);
      i_++;
      if (!isP(";")) s->e = expr();
      expect(";");
      return s;
    }
    if (isId("break") || isId("continue")) {
      Stmt* s = mkS(cur().s == "break" ? Stmt::Brk : Stmt::Cont);
      i_++;
      expect(";");
      return s;
    }
    if (isId("switch")) {
      Stmt* s = mkS(Stmt::Switch);
      i_++;
      expect("(");
      s->cond = expr();
      expect(")");
      expect("{");
      while (!isP("}")) {
        if (cur().k == Tok::End) err("missing '}'");
        if (isId("case") || isId("default")) {
          if (s->cases.empty() || !s->cases.back().body.empty()) s->cases.emplace_back();
          if (acceptId("default")) {
            s->cases.back().isDefault = true;
          } else {
            i_++;
            s->cases.back().labels.push_back(ternary());
          }
          expect(":");
        } else {
          if (s->cases.empty()) err("statement before the first case label");
          s->cases.back().body.push_back(stmt());
        }
      }
      expect("}");
      return s;
    }
    for (const char* k : {"goto", "struct", "union", "class", "try", "throw", "new", "delete"})
      if (isId(k)) err(std::string("'") + k + "' is not supported");
    if (typeStart()) return declStmt();
    Stmt* s = mkS(Stmt::ExprS);
    s->e = expr();
    expect(";");
    return s;
  }

  // "#pragma hls unroll" may also be the first thing inside the loop body.
  void innerUnroll(Stmt* loop) {
    if (isP("{") && peek().k == Tok::Pragma) {
      size_t save = i_;
      i_++;
      pragmas();
      if (pendUnroll_) {
        loop->unroll = true;
        pendUnroll_ = false;
      }
      Stmt* b = mkS(Stmt::Blk);
      while (!isP("}")) {
        if (cur().k == Tok::End) err("missing '}'");
        b->kids.push_back(stmt());
      }
      expect("}");
      loop->body = b;
      (void)save;
    } else {
      loop->body = stmt();
    }
  }

  Func* curFunc_ = nullptr;

  void topLevel() {
    pragmas();
    if (cur().k == Tok::End) return;
    if (accept(";")) return;
    if (isId("using") || isId("namespace")) {
      if (isId("namespace")) err("namespaces are not supported");
      while (!isP(";") && cur().k != Tok::End) i_++;
      expect(";");
      return;
    }
    if (acceptId("typedef")) {
      Spec sp = typeSpec();
      std::string n = ident();
      expect(";");
      typedefs_[n] = sp.ty;
      return;
    }
    if (isId("enum")) {
      i_++;
      if (cur().k == Tok::Id) i_++;
      expect("{");
      uint64_t next = 0;
      while (!isP("}")) {
        std::string n = ident();
        if (accept("=")) next = constEval(ternary());
        P.consts[n] = next++;
        P.constTypes[n] = {32, true};
        if (!accept(",")) break;
      }
      expect("}");
      expect(";");
      return;
    }
    for (const char* k : {"struct", "union", "class", "template"})
      if (isId(k)) err(std::string("'") + k + "' is not supported");
    Spec sp = typeSpec();
    bool ref = false;
    size_t save = i_;
    if (isP("*") || isP("&")) { ref = true; i_++; }
    std::string n = ident();
    if (isP("(")) {
      if (ref) err("functions returning pointers are not supported");
      i_++;
      Func f;
      f.name = n;
      f.ret = sp.ty;
      f.line = cur().line;
      f.top = pendTop_;
      f.pipeline = pendPipeline_;
      pendTop_ = pendPipeline_ = false;
      if (isId("void") && peek().k == Tok::Punct && peek().s == ")") i_++;
      if (!isP(")"))
        do {
          Spec ps = typeSpec();
          Decl d = declarator(ps, true);
          if (d.ty.w == 0) err("parameter '" + d.name + "' declared void");
          f.params.push_back(d);
        } while (accept(","));
      expect(")");
      if (accept(";")) return;   // prototype
      P.funcs.push_back(f);
      curFunc_ = &P.funcs.back();
      Stmt* body = block();
      P.funcs.back().body = body;
      curFunc_ = nullptr;
      pendUnroll_ = false;
      return;
    }
    i_ = save;
    do {
      Decl d = declarator(sp, false);
      if (d.isRef) err("global pointers are not supported");
      if (d.isConst && !d.isArray) {
        if (!d.init) err("const '" + d.name + "' needs a value");
        uint64_t v = constEval(d.init);
        if (d.ty.w < 64) {
          uint64_t m = (1ull << d.ty.w) - 1;
          v &= m;
          if (d.ty.s && (v >> (d.ty.w - 1))) v |= ~m;
        }
        P.consts[d.name] = v;
        P.constTypes[d.name] = d.ty;
      } else {
        d.isStatic = true;
        P.globals.push_back(d);
      }
    } while (accept(","));
    expect(";");
  }
};

inline void parseFile(Program& P, const std::string& path, const std::map<std::string, std::string>& defs) {
  std::ifstream in(path);
  if (!in) throw Error("mhls: cannot open " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  P.file = path;
  Lexer lx(path, ss.str(), defs);
  Parser ps(P, std::move(lx.out));
  ps.parseProgram();
}

}   // namespace hls
