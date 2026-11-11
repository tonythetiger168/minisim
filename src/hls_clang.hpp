// mhls: C++ through Clang.
//
// `mhls -clang` lets the installed clang (an external program) read the
// source and takes its syntax tree as JSON. What comes back has every
// template instantiated, every overload resolved and every implicit
// conversion spelled out, so classes, member functions, constructors,
// operators, templates, namespaces and scoped enums reduce to what the
// mhls front end already reads: plain functions over structs. This file
// writes that reduction as C text; hls_front.hpp parses it as usual, and
// `-emit-c` shows it.
//
//   class with members         ->  typedef struct, one function per member
//                                   function with the object as "self"
//   template instantiation     ->  one struct / function per instantiation
//   constructor                ->  a function called after the declaration
//   operator, method call      ->  a call
//   implicit conversion        ->  a cast
#pragma once

#include <cstdio>
#include <sys/wait.h>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "hls_front.hpp"

namespace hls {

// ---- a small JSON reader --------------------------------------------------
struct Json {
  enum K { Null, Bool, Num, Str, Arr, Obj } k = Null;
  bool b = false;
  std::string s;   // string value, or the text of a number
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  int line = 0, bline = 0, eline = 0;   // source lines of a syntax node, filled in by ClangFront

  const Json* get(const char* key) const {
    for (auto& [k2, v] : o)
      if (k2 == key) return &v;
    return nullptr;
  }
  Json* get(const char* key) { return const_cast<Json*>(static_cast<const Json*>(this)->get(key)); }
  std::string str(const char* key) const {
    const Json* v = get(key);
    return v && (v->k == Str || v->k == Num) ? v->s : std::string();
  }
  bool flag(const char* key) const {
    const Json* v = get(key);
    return v && v->k == Bool && v->b;
  }
  const std::vector<Json>& inner() const {
    static const std::vector<Json> none;
    const Json* v = get("inner");
    return v ? v->a : none;
  }
  std::string kind() const { return str("kind"); }
};

class JsonReader {
 public:
  explicit JsonReader(const std::string& t) : t_(t) {}
  Json read() {
    Json j = value();
    ws();
    return j;
  }

 private:
  const std::string& t_;
  size_t i_ = 0;
  void ws() {
    while (i_ < t_.size() && (t_[i_] == ' ' || t_[i_] == '\n' || t_[i_] == '\t' || t_[i_] == '\r')) i_++;
  }
  [[noreturn]] void bad() { throw Error("mhls: clang's syntax tree is not valid JSON at byte " + std::to_string(i_)); }
  std::string string() {
    std::string r;
    i_++;
    while (i_ < t_.size() && t_[i_] != '"') {
      if (t_[i_] == '\\' && i_ + 1 < t_.size()) {
        char c = t_[++i_];
        if (c == 'n') r += '\n';
        else if (c == 't') r += '\t';
        else if (c == 'u') { r += '?'; i_ += 4; }
        else r += c;
        i_++;
      } else {
        r += t_[i_++];
      }
    }
    i_++;
    return r;
  }
  Json value() {
    ws();
    if (i_ >= t_.size()) bad();
    Json j;
    char c = t_[i_];
    if (c == '{') {
      j.k = Json::Obj;
      i_++;
      ws();
      if (t_[i_] == '}') { i_++; return j; }
      for (;;) {
        ws();
        if (t_[i_] != '"') bad();
        std::string key = string();
        ws();
        if (t_[i_++] != ':') bad();
        j.o.emplace_back(std::move(key), value());
        ws();
        if (t_[i_] == ',') { i_++; continue; }
        if (t_[i_] == '}') { i_++; return j; }
        bad();
      }
    }
    if (c == '[') {
      j.k = Json::Arr;
      i_++;
      ws();
      if (t_[i_] == ']') { i_++; return j; }
      for (;;) {
        j.a.push_back(value());
        ws();
        if (t_[i_] == ',') { i_++; continue; }
        if (t_[i_] == ']') { i_++; return j; }
        bad();
      }
    }
    if (c == '"') {
      j.k = Json::Str;
      j.s = string();
      return j;
    }
    if (t_.compare(i_, 4, "true") == 0) { j.k = Json::Bool; j.b = true; i_ += 4; return j; }
    if (t_.compare(i_, 5, "false") == 0) { j.k = Json::Bool; i_ += 5; return j; }
    if (t_.compare(i_, 4, "null") == 0) { i_ += 4; return j; }
    size_t b = i_;
    while (i_ < t_.size() && (isdigit((unsigned char)t_[i_]) || strchr("+-.eE", t_[i_]))) i_++;
    if (b == i_) bad();
    j.k = Json::Num;
    j.s = t_.substr(b, i_ - b);
    return j;
  }
};

// ---- the reduction --------------------------------------------------------
class ClangFront {
 public:
  // Returns the C text for `path`. `args` are extra clang arguments (-I, -D).
  // `tops` names the functions to keep; empty keeps every function of the file.
  std::string run(const std::string& path, const std::vector<std::string>& args, const std::vector<std::string>& tops) {
    path_ = path;
    const char* env = getenv("MHLS_CLANG");
    std::string cmd = quote(env && *env ? env : "clang") + " -x c++ -std=c++17 -fsyntax-only -w -Wno-c++11-narrowing -D__MHLS__";
    for (auto& a : args) cmd += " " + quote(a);
    // Two views of the file, read at the same time: the tree, and clang's
    // printed form (for the sizes of array parameters).
    std::thread printer([&] {
      try {
        print_ = capture(cmd + " -Xclang -ast-print " + quote(path) + " 2>/dev/null");
      } catch (Error&) {
        print_.clear();   // the tree's run reports what is wrong
      }
    });
    std::string tree;
    try {
      tree = capture(cmd + " -Xclang -ast-dump=json " + quote(path));
    } catch (...) {
      printer.join();
      throw;
    }
    printer.join();
    Json root = JsonReader(tree).read();
    {
      std::ifstream in(path);
      std::string ln;
      for (int n = 1; std::getline(in, ln); n++) {
        size_t p = ln.find_first_not_of(" \t");
        if (p != std::string::npos && ln.compare(p, 7, "#pragma") == 0) {
          std::istringstream ls(ln.substr(p + 7));
          std::string w;
          ls >> w;
          if (w == "hls" || w == "mhls" || w == "HLS") pragmas_[n] = ln.substr(p);
        }
      }
    }
    locs(root);
    if (Json* in = root.get("inner"))
      for (auto& d : in->a) index(d, "");
    // What the file itself defines is the design; anything else (a header's
    // classes, templates and inline functions) comes in when it is used.
    for (const Json* d : mainVars_) globals_ << varDecl(*d) << "\n";
    for (const Json* d : mainFns_)
      if (tops.empty() || std::find(tops.begin(), tops.end(), d->str("name")) != tops.end()) need(d->str("id"));
    std::ostringstream body;
    for (size_t i = 0; i < funcs_.size(); i++) {
      Fn& f = funcs_[i];
      cur_ = &f;
      if (f.main) {
        // Pragmas on the lines right above the function belong to it.
        std::string pre;
        for (int l = f.decl->bline - 1; pragmas_.count(l); l--) pre = pragmas_[l] + "\n" + pre;
        body << "#line " << f.decl->bline << "\n" << pre;
      }
      body << f.proto << " {\n";
      if (f.ctor) {
        for (auto& c : f.decl->inner()) {
          if (c.kind() != "CXXCtorInitializer") continue;
          const Json* m = c.get("anyInit");
          if (!m || c.inner().empty()) unsupported(*f.decl, "a constructor that initializes a base class");
          std::string line = initTo("self." + m->str("name"), c.inner()[0]);
          for (auto& l : pre_) body << "  " << l << "\n";
          pre_.clear();
          if (!line.empty()) body << "  " << line << "\n";
          for (auto& l : post_) body << "  " << l << "\n";
          post_.clear();
        }
      }
      for (auto& c : f.decl->inner())
        if (c.kind() == "CompoundStmt") block(c, 1, body, false);
      body << "}\n";
    }
    cur_ = nullptr;
    std::ostringstream out;
    out << "// Written by mhls -clang from " << path << ": the C++ source reduced to plain functions and structs.\n";
    for (auto& r : records_) out << r;
    out << globals_.str();
    for (auto& f : funcs_) out << f.proto << ";\n";
    out << body.str();
    return out.str();
  }

 private:
  struct Fn {
    const Json* decl = nullptr;
    std::string name, proto, rec;
    bool ctor = false, main = false, selfRet = false;
  };
  struct LazyFn {
    const Json* decl = nullptr;
    std::string suffix, recId;   // the name after the class's, and the class
  };
  struct LazyRec {
    const Json* decl = nullptr;
    std::string qual;
  };
  std::map<std::string, LazyFn> lazyFn_;         // function id -> a definition not yet written
  std::map<std::string, std::string> fnAlias_;   // id of another declaration of a function -> its definition's
  std::map<std::string, std::string> prevOf_;
  std::map<std::string, LazyRec> lazyRec_;       // a spelling of a class type -> its definition
  std::map<std::string, std::string> recQual_;   // class id -> qualified name
  std::map<std::string, std::string> recId_;     // class C name -> id
  std::map<std::string, std::string> lazyCtor_;  // class id | constructor type -> constructor id
  std::map<std::string, const Json*> lazyVar_;   // a header's global -> its declaration
  std::vector<const Json*> mainVars_, mainFns_;
  std::map<std::string, const Json*> anon_;      // unnamed struct id -> its definition, until a typedef names it
  std::map<std::string, std::string> anonEnum_;
  std::string path_, print_, curFile_;
  int lastLine_ = 0;
  std::map<int, std::string> pragmas_;
  std::vector<std::string> records_;
  std::ostringstream globals_;
  std::deque<Fn> funcs_;
  Fn* cur_ = nullptr;
  std::map<std::string, std::string> fnName_;    // declaration id -> C name
  std::map<std::string, std::vector<int>> refKinds_;   // function id -> per parameter: 0 value, 1 const reference, 2 reference
  std::map<std::string, const Json*> fnDecl_;    // function id -> its declaration
  std::map<std::string, std::string> ctorId_;    // constructor C name -> declaration id
  std::map<std::string, std::string> varName_;   // declaration id of a static member -> C name
  std::map<std::string, std::string> ctors_;     // record C name | constructor type -> C name
  std::map<std::string, std::string> enumVal_;   // enumerator id -> value
  std::map<std::string, std::string> typedefs_;  // name -> what it stands for
  std::map<std::string, std::string> enums_;     // enum type name -> the integer type under it
  std::map<std::string, std::string> recAlias_;  // a spelling of a class type -> its C name
  std::set<std::string> recNames_, taken_;
  // Clang spells a type with or without its namespaces depending on where
  // it is used: every tail of the qualified name finds it.
  template <class M>
  static void alias(M& m, const std::string& qual, const std::string& v) {
    m[qual] = v;
    int depth = 0;
    for (size_t i = 0; i + 1 < qual.size(); i++) {
      if (qual[i] == '<') depth++;
      else if (qual[i] == '>') depth--;
      else if (!depth && qual[i] == ':' && qual[i + 1] == ':') m.emplace(qual.substr(i + 2), v);
    }
  }

  static std::string quote(const std::string& s) {
    std::string r = "'";
    for (char c : s) r += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return r + "'";
  }
  std::string capture(const std::string& cmd) {
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) throw Error("mhls: cannot run clang (" + cmd + ")");
    std::string r;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) r.append(buf, n);
    int rc = pclose(p);
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 127)
      throw Error("mhls: -clang needs clang: put it on the PATH or set MHLS_CLANG to it");
    if (rc != 0) throw Error("mhls: clang did not accept " + path_ + " (its messages are above)");
    return r;
  }
  [[noreturn]] void unsupported(const Json& n, const std::string& what) {
    throw Error(path_ + ":" + std::to_string(n.bline ? n.bline : n.line) + ": error: mhls -clang does not support " + what);
  }

  // Clang writes a file name or a line only where it differs from the one
  // before, in document order.
  int touch1(const Json& l) {
    std::string f = l.str("file");
    if (!f.empty()) curFile_ = f;
    std::string ln = l.str("line");
    if (!ln.empty()) lastLine_ = atoi(ln.c_str());
    return lastLine_;
  }
  int touch(const Json& l) {
    if (const Json* sp = l.get("spellingLoc")) {
      touch1(*sp);
      if (const Json* e = l.get("expansionLoc")) return touch1(*e);
    }
    return touch1(l);
  }
  std::set<const Json*> inMain_;
  void locs(Json& n) {
    if (n.k != Json::Obj) return;
    if (Json* l = n.get("loc")) n.line = touch(*l);
    if (curFile_ == path_) inMain_.insert(&n);
    if (Json* r = n.get("range")) {
      if (Json* b = r->get("begin")) n.bline = touch(*b);
      if (Json* e = r->get("end")) n.eline = touch(*e);
    }
    if (Json* in = n.get("inner"))
      for (auto& c : in->a) locs(c);
  }

  // ---- types ----
  static std::string trim(std::string s) {
    size_t b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
  }
  static std::string mangle(const std::string& s) {
    std::string r;
    for (char c : s) {
      if (isalnum((unsigned char)c)) r += c;
      else if (!r.empty() && r.back() != '_') r += '_';
    }
    while (!r.empty() && r.back() == '_') r.pop_back();
    return r;
  }
  struct CT {
    std::string base, dims;
    bool ref = false, ptr = false, isConst = false;
  };
  CT ctype(const Json& n, const Json* typeNode = nullptr) {
    const Json* t = typeNode ? typeNode : n.get("type");
    if (!t) unsupported(n, "a declaration without a type");
    std::string q = t->str("desugaredQualType");
    if (q.empty()) q = t->str("qualType");
    return ctypeStr(q, n);
  }
  CT ctypeStr(std::string q, const Json& at) {
    CT r;
    q = trim(q);
    if (!q.empty() && q.back() == '&') { r.ref = true; q = trim(q.substr(0, q.size() - 1)); }
    if (!q.empty() && q.back() == '&') unsupported(at, "rvalue references");
    while (!q.empty() && q.back() == ']') {
      size_t b = q.rfind('[');
      r.dims = q.substr(b) + r.dims;
      q = trim(q.substr(0, b));
    }
    if (!q.empty() && q.back() == '*') { r.ptr = true; q = trim(q.substr(0, q.size() - 1)); }
    if (q.size() > 6 && q.compare(q.size() - 6, 6, " const") == 0) { r.isConst = true; q = trim(q.substr(0, q.size() - 6)); }
    if (q.rfind("const ", 0) == 0) { r.isConst = true; q = trim(q.substr(6)); }
    if (q.rfind("volatile ", 0) == 0) q = trim(q.substr(9));
    for (const char* p : {"struct ", "class ", "enum "})
      if (q.rfind(p, 0) == 0) q = trim(q.substr(strlen(p)));
    static const std::map<std::string, std::string> builtin = {
        {"bool", "bool"}, {"_Bool", "bool"}, {"char", "int8_t"}, {"signed char", "int8_t"}, {"unsigned char", "uint8_t"},
        {"short", "int16_t"}, {"unsigned short", "uint16_t"}, {"int", "int32_t"}, {"unsigned int", "uint32_t"},
        {"long", "int64_t"}, {"unsigned long", "uint64_t"}, {"long long", "int64_t"}, {"unsigned long long", "uint64_t"},
        {"void", "void"}, {"char16_t", "uint16_t"}, {"char32_t", "uint32_t"}, {"wchar_t", "int32_t"}};
    auto it = builtin.find(q);
    if (it != builtin.end()) {
      r.base = it->second;
    } else if (q.rfind("hls::stream<", 0) == 0 && q.back() == '>') {
      r.base = "hls::stream<" + ctypeStr(q.substr(12, q.size() - 13), at).base + " >";
    } else if (enums_.count(q)) {
      r.base = enums_[q];
    } else if (!recordOf(q, at).empty()) {
      r.base = recordOf(q, at);
    } else if (typedefs_.count(q) && typedefs_[q] != q) {
      CT u = ctypeStr(typedefs_[q], at);
      u.ref = u.ref || r.ref;
      u.ptr = u.ptr || r.ptr;
      u.isConst = u.isConst || r.isConst;
      u.dims = r.dims + u.dims;
      return u;
    } else if (q == "float" || q == "double" || q == "long double") {
      unsupported(at, "floating point ('" + q + "')");
    } else {
      unsupported(at, "the type '" + q + "'");
    }
    return r;
  }

  // ---- declarations ----
  static bool hasBody(const Json& d) {
    for (auto& c : d.inner())
      if (c.kind() == "CompoundStmt") return true;
    return false;
  }
  static std::string templateArgs(const Json& d) {
    std::string a;
    for (auto& c : d.inner()) {
      if (c.kind() != "TemplateArgument") continue;
      std::string v = c.str("value");
      if (v.empty())
        if (const Json* t = c.get("type")) v = t->str("qualType");
      a += (a.empty() ? "" : ", ") + v;
    }
    return a;
  }
  std::string unique(std::string n) {
    std::string r = n;
    for (int k = 2; !taken_.insert(r).second; k++) r = n + "_" + std::to_string(k);
    return r;
  }
  static std::string opWord(const std::string& name) {
    static const std::map<std::string, std::string> w = {
        {"+", "add"}, {"-", "sub"}, {"*", "mul"}, {"/", "div"}, {"%", "rem"}, {"==", "eq"}, {"!=", "ne"}, {"<", "lt"},
        {"<=", "le"}, {">", "gt"}, {">=", "ge"}, {"+=", "add_assign"}, {"-=", "sub_assign"}, {"*=", "mul_assign"},
        {"&", "and"}, {"|", "or"}, {"^", "xor"}, {"<<", "shl"}, {">>", "shr"}, {"[]", "index"}, {"()", "call"},
        {"!", "not"}, {"~", "inv"}, {"++", "inc"}, {"--", "dec"}, {"=", "assign"}, {"&=", "and_assign"},
        {"|=", "or_assign"}, {"^=", "xor_assign"}, {"<<=", "shl_assign"}, {">>=", "shr_assign"}, {"&&", "land"}, {"||", "lor"}};
    if (name.rfind("operator", 0) != 0) return name;
    auto it = w.find(name.substr(8));
    return "op_" + (it != w.end() ? it->second : mangle(name.substr(8)));
  }

  static bool defined(const Json& d) { return d.flag("completeDefinition") || d.get("definitionData"); }
  // Notes every definition in the tree; nothing is written yet.
  void index(const Json& d, const std::string& ns) {
    std::string k = d.kind();
    if (k == "NamespaceDecl" || k == "LinkageSpecDecl") {
      std::string in = k == "NamespaceDecl" && !d.str("name").empty() && !d.flag("isInline") ? ns + d.str("name") + "::" : ns;
      for (auto& c : d.inner()) index(c, in);
    } else if (k == "TypedefDecl" || k == "TypeAliasDecl") {
      // "typedef struct { ... } name;" names the struct.
      for (auto& c : d.inner())
        if (const Json* own = c.get("ownedTagDecl")) {
          auto an = anon_.find(own->str("id"));
          if (an != anon_.end()) { indexRecord(*an->second, ns + d.str("name")); return; }
          auto ae = anonEnum_.find(own->str("id"));
          if (ae != anonEnum_.end()) { alias(enums_, ns + d.str("name"), ae->second); return; }
        }
      if (const Json* t = d.get("type")) {
        std::string q = t->str("desugaredQualType");
        alias(typedefs_, ns + d.str("name"), q.empty() ? t->str("qualType") : q);
      }
    } else if (k == "EnumDecl") {
      std::string under = "int32_t";
      if (const Json* u = d.get("fixedUnderlyingType")) {
        std::string q = u->str("desugaredQualType");
        if (q.empty()) q = u->str("qualType");
        auto td = typedefs_.find(q);
        if (td != typedefs_.end()) q = td->second;
        static const std::map<std::string, std::string> w = {
            {"bool", "bool"}, {"char", "int8_t"}, {"signed char", "int8_t"}, {"unsigned char", "uint8_t"}, {"short", "int16_t"},
            {"unsigned short", "uint16_t"}, {"int", "int32_t"}, {"unsigned int", "uint32_t"}, {"long", "int64_t"},
            {"unsigned long", "uint64_t"}, {"long long", "int64_t"}, {"unsigned long long", "uint64_t"}};
        auto it = w.find(q);
        if (it != w.end()) under = it->second;
      }
      if (!d.str("name").empty()) alias(enums_, ns + d.str("name"), under);
      else anonEnum_[d.str("id")] = under;
      long long next = 0;
      for (auto& c : d.inner()) {
        if (c.kind() != "EnumConstantDecl") continue;
        std::string v = firstValue(c);
        if (!v.empty()) next = atoll(v.c_str());
        enumVal_[c.str("id")] = std::to_string(next++);
      }
    } else if (k == "CXXRecordDecl") {
      if (defined(d) && d.str("name").empty()) anon_[d.str("id")] = &d;
      else if (defined(d)) indexRecord(d, ns + d.str("name"));
    } else if (k == "ClassTemplateDecl") {
      for (auto& c : d.inner())
        if (c.kind() == "ClassTemplateSpecializationDecl" && defined(c)) indexRecord(c, ns + c.str("name") + "<" + templateArgs(c) + ">");
    } else if (k == "FunctionDecl" || k == "CXXMethodDecl" || k == "CXXConstructorDecl") {
      // A member function defined outside its class arrives here too.
      indexFn(d, k == "CXXConstructorDecl" ? "ctor" : opWord(d.str("name")), k == "FunctionDecl" ? "" : d.str("parentDeclContextId"));
      if (k == "FunctionDecl" && hasBody(d) && inMain_.count(&d)) mainFns_.push_back(&d);
    } else if (k == "FunctionTemplateDecl") {
      for (auto& c : d.inner())
        if (c.kind() == "FunctionDecl" && !templateArgs(c).empty()) indexFn(c, c.str("name") + "_" + mangle(templateArgs(c)), "");
    } else if (k == "VarDecl") {
      if (inMain_.count(&d)) mainVars_.push_back(&d);
      else lazyVar_[d.str("id")] = &d;
    }
  }
  void indexFn(const Json& d, const std::string& suffix, const std::string& recId) {
    std::string id = d.str("id"), prev = d.str("previousDecl");
    if (!prev.empty()) prevOf_[id] = prev;
    if (!hasBody(d)) {
      // A later declaration of a function already defined.
      if (!prev.empty()) {
        if (lazyFn_.count(prev)) fnAlias_[id] = prev;
        else if (fnAlias_.count(prev)) fnAlias_[id] = fnAlias_[prev];
      }
      return;
    }
    if (d.flag("isImplicit")) return;
    lazyFn_[id] = {&d, suffix, recId};
    for (std::string p = prev; !p.empty(); p = prevOf_.count(p) ? prevOf_[p] : "") fnAlias_[p] = id;
    if (d.kind() == "CXXConstructorDecl" && d.get("type")) lazyCtor_[recId + "|" + d.get("type")->str("qualType")] = id;
  }
  void indexRecord(const Json& d, const std::string& qual) {
    std::string id = d.str("id");
    recQual_[id] = qual;
    LazyRec lr{&d, qual};
    lazyRec_[qual] = lr;
    int depth = 0;
    for (size_t i = 0; i + 1 < qual.size(); i++) {
      if (qual[i] == '<') depth++;
      else if (qual[i] == '>') depth--;
      else if (!depth && qual[i] == ':' && qual[i + 1] == ':') lazyRec_.emplace(qual.substr(i + 2), lr);
    }
    for (auto& c : d.inner()) {
      std::string k = c.kind();
      if (k == "CXXRecordDecl" && !c.flag("isImplicit")) {
        if (defined(c) && !c.str("name").empty()) indexRecord(c, qual + "::" + c.str("name"));
      } else if (k == "EnumDecl" || k == "TypedefDecl" || k == "TypeAliasDecl") {
        index(c, qual + "::");
      } else if (k == "CXXMethodDecl" || k == "CXXConstructorDecl") {
        indexFn(c, k == "CXXConstructorDecl" ? "ctor" : opWord(c.str("name")), id);
      } else if (k == "FunctionTemplateDecl") {
        for (auto& f : c.inner())
          if ((f.kind() == "CXXMethodDecl" || f.kind() == "CXXConstructorDecl") && !templateArgs(f).empty())
            indexFn(f, opWord(f.str("name")) + "_" + mangle(templateArgs(f)), id);
      }
    }
  }
  // Writes the function with this id if it has not been yet; returns the
  // id of its definition, "" when there is none.
  std::string need(std::string id) {
    auto al = fnAlias_.find(id);
    if (al != fnAlias_.end()) id = al->second;
    if (fnName_.count(id)) return id;
    auto it = lazyFn_.find(id);
    if (it == lazyFn_.end()) return "";
    LazyFn lf = it->second;
    std::string cn;
    if (!lf.recId.empty()) {
      auto q = recQual_.find(lf.recId);
      if (q == recQual_.end()) unsupported(*lf.decl, "this member function ('" + lf.decl->str("name") + "')");
      cn = recordOf(q->second, *lf.decl);
    }
    function(*lf.decl, cn.empty() ? lf.suffix : cn + "_" + lf.suffix, cn);
    return id;
  }
  static std::string firstValue(const Json& n) {
    for (auto& c : n.inner()) {
      std::string v = c.str("value");
      if (!v.empty()) return v;
      v = firstValue(c);
      if (!v.empty()) return v;
    }
    return "";
  }


  // The C name of the class spelled `q`, writing its struct on first use; "" if `q` is no class.
  std::string recordOf(const std::string& q, const Json&) {
    auto a = recAlias_.find(q);
    if (a != recAlias_.end()) return a->second;
    auto it = lazyRec_.find(q);
    if (it == lazyRec_.end()) return "";
    const Json& d = *it->second.decl;
    const std::string qual = it->second.qual;
    a = recAlias_.find(qual);
    if (a != recAlias_.end()) return a->second;
    std::string cn = unique(mangle(qual));
    alias(recAlias_, qual, cn);
    recId_[cn] = d.str("id");
    if (const Json* bases = d.get("bases"))
      if (!bases->a.empty()) unsupported(d, "base classes ('" + qual + "')");
    std::ostringstream o;
    o << "typedef struct {\n";
    int fields = 0;
    for (auto& c : d.inner()) {
      if (c.kind() != "FieldDecl") continue;
      CT t = ctype(c);
      if (t.ref || t.ptr) unsupported(c, "pointer or reference members");
      o << "  " << t.base << " " << c.str("name") << t.dims;
      if (c.flag("isBitfield")) o << " : " << firstValue(c);
      o << ";\n";
      fields++;
    }
    if (!fields) unsupported(d, "a class without data members ('" + qual + "')");
    o << "} " << cn << ";\n";
    records_.push_back(o.str());
    for (auto& c : d.inner()) {
      if (c.kind() != "VarDecl") continue;   // a static member is a global under the class's name
      varName_[c.str("id")] = unique(cn + "_" + c.str("name"));
      Fn* was = cur_;
      cur_ = nullptr;
      std::string v = varDecl(c);
      cur_ = was;
      if (v.rfind("static ", 0) == 0) v = v.substr(7);
      size_t at = v.find(" " + c.str("name"));
      globals_ << v.substr(0, at + 1) << varName_[c.str("id")] << v.substr(at + 1 + c.str("name").size()) << "\n";
    }
    return cn;
  }

  // The sizes of an array parameter, which clang's tree gives as a
  // pointer: its printed form of the function has them, with constants
  // already evaluated.
  std::string paramDims(const std::string& fn, const std::string& pn) {
    for (size_t at = 0; (at = print_.find(fn + "(", at)) != std::string::npos; at += fn.size()) {
      if (at && (isalnum((unsigned char)print_[at - 1]) || print_[at - 1] == '_')) continue;
      size_t e = at + fn.size(), depth = 0;
      for (; e < print_.size(); e++) {
        if (print_[e] == '(') depth++;
        else if (print_[e] == ')' && --depth == 0) break;
      }
      std::string sig = print_.substr(at, e - at);
      for (size_t p = 0; (p = sig.find(pn + "[", p)) != std::string::npos; p += pn.size()) {
        if (isalnum((unsigned char)sig[p - 1]) || sig[p - 1] == '_') continue;
        size_t q = p + pn.size();
        std::string dims;
        while (q < sig.size() && sig[q] == '[') {
          size_t c = sig.find(']', q);
          dims += sig.substr(q, c - q + 1);
          q = c + 1;
        }
        if (dims.find_first_not_of("[]0123456789") == std::string::npos && dims.find("[]") == std::string::npos) return dims;
      }
    }
    return "";
  }

  void function(const Json& d, const std::string& base, const std::string& rec) {
    Fn f;
    f.decl = &d;
    f.rec = rec;
    f.ctor = d.kind() == "CXXConstructorDecl";
    f.main = inMain_.count(&d) > 0;
    f.name = unique(base);
    fnName_[d.str("id")] = f.name;
    fnDecl_[d.str("id")] = &d;
    std::string ty = d.get("type") ? d.get("type")->str("qualType") : "";
    if (f.ctor) ctors_[rec + "|" + ty] = f.name, ctorId_[f.name] = d.str("id");
    std::string ret = "void";
    bool constMethod = false;
    if (!f.ctor) {
      size_t paren = ty.find('(');
      std::string q = trim(ty.substr(0, paren));
      constMethod = ty.size() > 6 && ty.compare(ty.size() - 6, 6, " const") == 0;
      // The declared return type, with typedefs resolved where clang says so.
      if (const Json* t = d.get("type"))
        if (!t->str("desugaredQualType").empty()) q = trim(t->str("desugaredQualType").substr(0, t->str("desugaredQualType").find('(')));
      CT r = ctypeStr(q, d);
      if (r.ref && !r.isConst && !r.ptr && !rec.empty() && r.base == rec && returnsThis(d)) {
        f.selfRet = true;
        selfRet_.insert(d.str("id"));
        r.ref = false;
        r.base = "void";
      }
      // A reference to a constant is as good as the value.
      if (r.ptr || (r.ref && !r.isConst)) unsupported(d, "functions that return a pointer or reference ('" + d.str("name") + "')");
      ret = r.base;
    }
    std::string ps;
    bool method = !rec.empty() && d.str("storageClass") != "static";
    if (method) ps = (constMethod ? "const " : "") + rec + "& self";
    std::vector<int>& kinds = refKinds_[d.str("id")];
    if (method) kinds.push_back(constMethod ? 1 : 2);
    for (auto& c : d.inner()) {
      if (c.kind() != "ParmVarDecl") continue;
      CT t = ctype(c);
      // A constant reference to a number is the number: callers pass values that are not variables.
      if (t.ref && t.isConst && t.dims.empty() && !recId_.count(t.base)) t.ref = t.isConst = false;
      std::string n = c.str("name");
      if (n.empty()) n = "mhls_unnamed" + std::to_string(ps.size());
      std::string one;
      if (t.base.rfind("hls::stream<", 0) == 0) one = t.base + "& " + n;
      else if (t.ptr) {
        std::string dims = paramDims(d.str("name"), n);
        one = dims.empty() ? t.base + "* " + n : std::string(t.isConst ? "const " : "") + t.base + " " + n + dims;
      } else one = std::string(t.isConst && t.ref ? "const " : "") + t.base + (t.ref ? "& " : " ") + n + t.dims;
      kinds.push_back(t.ref && !t.ptr && t.base.rfind("hls::stream<", 0) != 0 ? (t.isConst ? 1 : 2) : 0);
      ps += (ps.empty() ? "" : ", ") + one;
    }
    f.proto = ret + " " + f.name + "(" + ps + ")";
    funcs_.push_back(f);
  }

  std::string varDecl(const Json& d) {
    CT t = ctype(d);
    if (t.ref || t.ptr) unsupported(d, "pointer or reference variables ('" + d.str("name") + "')");
    if (t.base.rfind("hls::stream<", 0) == 0) unsupported(d, "a stream that is not a parameter");
    std::string s;
    if (d.str("storageClass") == "static") s += "static ";
    if (t.isConst || d.flag("constexpr")) s += "const ";
    s += t.base + " " + d.str("name") + t.dims;
    const Json* init = nullptr;
    for (auto& c : d.inner())
      if (c.k == Json::Obj && !c.kind().empty()) init = &c;
    if (!init) return s + ";";
    const Json* e = strip(*init);
    if (e->kind() == "CXXConstructExpr" && !(ctorOf(*e).empty() && e->inner().size() == 1)) {
      if (!cur_) {
        if (!ctorOf(*e).empty()) unsupported(d, "a global object with a constructor ('" + d.str("name") + "')");
        return s + ";";
      }
      std::string run = initTo(d.str("name"), *e);
      return s + ";" + (run.empty() ? "" : " " + run);
    }
    return s + " = " + ex(*init) + ";";
  }
  // Statements that initialize `target` from the initializer `init0`:
  // an assignment, or constructor calls (one per element for an array).
  int loops_ = 0;
  std::string initTo(const std::string& target, const Json& init0) {
    const Json* e = strip(init0);
    std::string k = e->kind();
    if ((k != "CXXConstructExpr" && k != "CXXTemporaryObjectExpr") || (ctorOf(*e).empty() && e->inner().size() == 1))
      return target + " = " + ex(init0) + ";";
    std::string ctor = ctorOf(*e);
    if (ctor.empty()) {
      if (!e->inner().empty()) unsupported(*e, "this constructor");
      return e->flag("zeroing") ? target + " = {0};" : "";
    }
    CT t = ctype(*e);
    std::string head, idx, tail;
    for (size_t p = 0; p < t.dims.size();) {
      size_t c = t.dims.find(']', p);
      std::string v = "mhls_i" + std::to_string(++loops_);
      head += "for (int32_t " + v + " = 0; " + v + " < " + t.dims.substr(p + 1, c - p - 1) + "; " + v + "++) { ";
      idx += "[" + v + "]";
      tail += " }";
      p = c + 1;
    }
    std::string obj = target, back;
    if (!idx.empty()) {
      obj = "mhls_t" + std::to_string(++temps_);
      head += t.base + " " + obj + " = " + target + idx + "; ";
      back = " " + target + idx + " = " + obj + ";";
    }
    std::string call = ctor + "(" + obj;
    const std::vector<int>& kinds = refKinds_[ctorId_[ctor]];
    size_t i = 1;
    for (auto& a : e->inner()) call += ", " + refArg(a, i < kinds.size() ? kinds[i] : 0), i++;
    return head + call + ");" + back + tail;
  }
  // The user constructor a construction runs, "" for a trivial one.
  std::string ctorOf(const Json& e) {
    const Json* t = e.get("type");
    const Json* c = e.get("ctorType");
    if (!t || !c) return "";
    std::string q = t->str("desugaredQualType");
    if (q.empty()) q = t->str("qualType");
    CT r = ctypeStr(q, e);
    auto lz = lazyCtor_.find(recId_[r.base] + "|" + c->str("qualType"));
    if (lz != lazyCtor_.end()) need(lz->second);
    auto it = ctors_.find(r.base + "|" + c->str("qualType"));
    return it == ctors_.end() ? "" : it->second;
  }
  static const Json* strip(const Json& n) {
    const Json* e = &n;
    for (;;) {
      std::string k = e->kind();
      if ((k == "ExprWithCleanups" || k == "MaterializeTemporaryExpr" || k == "CXXBindTemporaryExpr" || k == "ConstantExpr" ||
           k == "CXXFunctionalCastExpr" || (k == "ImplicitCastExpr" && (e->str("castKind") == "NoOp" || e->str("castKind") == "ConstructorConversion"))) &&
          e->inner().size() == 1)
        e = &e->inner()[0];
      else
        return e;
    }
  }

  // ---- statements ----
  // Clang's name for a construct, in words.
  static std::string word(const std::string& k) {
    static const std::map<std::string, std::string> w = {
        {"CXXForRangeStmt", "range-based for loops (index the array)"}, {"LambdaExpr", "lambdas"}, {"CXXNewExpr", "new"},
        {"CXXDeleteExpr", "delete"}, {"CXXThrowExpr", "exceptions"}, {"CXXTryStmt", "exceptions"}, {"GotoStmt", "goto"},
        {"LabelStmt", "labels"}, {"StringLiteral", "string literals"}, {"CXXDynamicCastExpr", "dynamic_cast"},
        {"CXXReinterpretCastExpr", "reinterpret_cast"}, {"CXXTypeidExpr", "typeid"}, {"CXXStdInitializerListExpr", "std::initializer_list"},
        {"CXXPseudoDestructorExpr", "destructor calls"}, {"CompoundLiteralExpr", "compound literals"}, {"StmtExpr", "statement expressions"},
        {"CXXInheritedCtorInitExpr", "inherited constructors"}, {"PackExpansionExpr", "parameter packs"}, {"CXXFoldExpr", "fold expressions"}};
    auto it = w.find(k);
    return it != w.end() ? it->second : "this construct (clang calls it " + k + ")";
  }
  static std::string pad(int n) { return std::string(2 * n, ' '); }
  void block(const Json& n, int ind, std::ostringstream& o, bool braces) {
    if (braces) o << pad(ind - 1) << "{\n";
    int lo = n.bline;
    for (auto& c : n.inner()) {
      if (c.k != Json::Obj || c.kind().empty()) continue;
      // Pragmas between the statement before and this one.
      if (cur_ && cur_->main)
        for (auto& [l, text] : pragmas_)
          if (l >= lo && l < c.bline && l <= n.eline) o << text << "\n";
      lo = std::max(c.eline, c.bline) + 1;
      if (cur_ && cur_->main) o << "#line " << c.bline << "\n";
      stmt(c, ind, o);
    }
    if (braces) o << pad(ind - 1) << "}\n";
  }
  void body(const Json& n, int ind, std::ostringstream& o) {
    if (n.kind() == "CompoundStmt") block(n, ind + 1, o, true);
    else {
      o << pad(ind) << "{\n";
      stmt(n, ind + 1, o);
      o << pad(ind) << "}\n";
    }
  }
  static bool present(const Json& n) { return n.k == Json::Obj && !n.kind().empty(); }
  // A temporary object becomes a variable declared just before the
  // statement that uses it.
  std::vector<std::string> pre_, post_;
  int temps_ = 0;
  void stmt(const Json& n, int ind, std::ostringstream& o) {
    std::vector<std::string> outer = std::move(pre_), outerPost = std::move(post_);
    pre_.clear();
    post_.clear();
    std::ostringstream s;
    stmt1(n, ind, s);
    std::string k = n.kind();
    if (!post_.empty() && k.size() > 4 && k.compare(k.size() - 4, 4, "Stmt") == 0 && k != "DeclStmt")
      unsupported(n, "an array element passed by reference here (do the call in a statement of its own)");
    if (!pre_.empty() && (k == "ForStmt" || k == "WhileStmt" || k == "DoStmt"))
      unsupported(n, "a temporary object in a loop's condition or step (give it a name inside the loop)");
    for (auto& l : pre_) o << pad(ind) << l << "\n";
    o << s.str();
    for (auto& l : post_) o << pad(ind) << l << "\n";
    pre_ = std::move(outer);
    post_ = std::move(outerPost);
  }
  void stmt1(const Json& n, int ind, std::ostringstream& o) {
    std::string k = n.kind();
    const auto& in = n.inner();
    if (k == "CompoundStmt") {
      block(n, ind + 1, o, true);
    } else if (k == "DeclStmt") {
      for (auto& d : in) {
        if (d.kind() == "VarDecl") o << pad(ind) << varDecl(d) << "\n";
        else if (d.kind() != "TypedefDecl" && d.kind() != "TypeAliasDecl" && d.kind() != "StaticAssertDecl")
          unsupported(d, "a local " + d.kind());
      }
    } else if (k == "IfStmt") {
      if (n.flag("hasInit") || n.flag("hasVar")) unsupported(n, "a declaration inside an if condition");
      o << pad(ind) << "if (" << ex(in[0]) << ")\n";
      body(in[1], ind, o);
      if (in.size() > 2 && present(in[2])) {
        o << pad(ind) << "else\n";
        body(in[2], ind, o);
      }
    } else if (k == "ForStmt") {
      if (in.size() != 5 || present(in[1])) unsupported(n, "this form of for loop");
      o << pad(ind) << "for (";
      if (present(in[0])) {
        if (in[0].kind() == "DeclStmt") {
          if (in[0].inner().size() != 1) unsupported(n, "several declarations in a for loop");
          o << varDecl(in[0].inner()[0]);
        } else o << ex(in[0]) << ";";
      } else o << ";";
      o << " " << (present(in[2]) ? ex(in[2]) : "") << "; " << (present(in[3]) ? ex(in[3]) : "") << ")\n";
      body(in[4], ind, o);
    } else if (k == "WhileStmt") {
      if (n.flag("hasVar")) unsupported(n, "a declaration inside a while condition");
      o << pad(ind) << "while (" << ex(in[0]) << ")\n";
      body(in[1], ind, o);
    } else if (k == "DoStmt") {
      o << pad(ind) << "do\n";
      body(in[0], ind, o);
      o << pad(ind) << "while (" << ex(in[1]) << ");\n";
    } else if (k == "ReturnStmt") {
      if (cur_ && cur_->selfRet) {
        if (!in.empty()) ex(in[0]);   // a call on this object runs first
        o << pad(ind) << "return;\n";
      }
      else o << pad(ind) << "return" << (in.empty() ? "" : " " + ex(in[0])) << ";\n";
    } else if (k == "BreakStmt") {
      o << pad(ind) << "break;\n";
    } else if (k == "ContinueStmt") {
      o << pad(ind) << "continue;\n";
    } else if (k == "NullStmt") {
      o << pad(ind) << ";\n";
    } else if (k == "SwitchStmt") {
      if (n.flag("hasInit") || n.flag("hasVar")) unsupported(n, "a declaration inside a switch condition");
      o << pad(ind) << "switch (" << ex(in[0]) << ")\n";
      body(in[1], ind, o);
    } else if (k == "CaseStmt") {
      o << pad(ind) << "case " << ex(in[0]) << ":\n";
      stmt(in.back(), ind + 1, o);
    } else if (k == "DefaultStmt") {
      o << pad(ind) << "default:\n";
      stmt(in.back(), ind + 1, o);
    } else if (k == "AttributedStmt") {
      stmt(in.back(), ind, o);
    } else if (k.size() > 4 && k.compare(k.size() - 4, 4, "Stmt") == 0) {
      unsupported(n, word(k));
    } else {
      selfNode_ = nullptr;
      std::string e = ex(n);
      if (selfNode_ != strip(n)) o << pad(ind) << e << ";\n";   // else the call, already written, was the whole statement
    }
  }

  // ---- expressions ----
  bool isStream(const Json& e) {
    const Json* t = e.get("type");
    if (!t) return false;
    std::string q = t->str("desugaredQualType");
    if (q.empty()) q = t->str("qualType");
    return q.rfind("hls::stream<", 0) == 0 || q.rfind("const hls::stream<", 0) == 0;
  }
  bool isRecordType(const Json& e) {
    const Json* t = e.get("type");
    if (!t) return false;
    std::string q = t->str("desugaredQualType");
    if (q.empty()) q = t->str("qualType");
    if (q.rfind("const ", 0) == 0) q = q.substr(6);
    auto td = typedefs_.find(q);
    if (td != typedefs_.end()) q = td->second;
    return recAlias_.count(q) || lazyRec_.count(q);
  }
  static bool isThis(const Json& e) {
    const Json* x = &e;
    while (x->kind() == "ImplicitCastExpr" || x->kind() == "ParenExpr") x = &x->inner()[0];
    return x->kind() == "CXXThisExpr";
  }
  // An argument for a reference parameter (how: 1 const, 2 not). The mhls
  // front end binds a reference to a variable or a member of one; an array
  // element is copied to a variable first and, when the callee may change
  // it, copied back after the statement.
  std::string refArg(const Json& a, int how) {
    const Json* e = &a;
    bool elem = false;
    for (;;) {
      std::string k = e->kind();
      if (k == "ArraySubscriptExpr") { elem = true; break; }
      if ((k == "MemberExpr" || k == "ParenExpr" || k == "ImplicitCastExpr" || k == "MaterializeTemporaryExpr") && !e->inner().empty() && !isThis(*e))
        e = &e->inner()[0];
      else
        break;
    }
    std::string v = ex(a);
    if (!how || !elem || !cur_) return v;
    std::string t = "mhls_t" + std::to_string(++temps_);
    pre_.push_back(ctype(a).base + " " + t + " = " + v + ";");
    if (how == 2) post_.push_back(v + " = " + t + ";");
    return t;
  }
  // The id of the definition a callee expression names, written if need be.
  std::string idOf(const Json& c) {
    const Json* x = &c;
    while ((x->kind() == "ImplicitCastExpr" || x->kind() == "ParenExpr") && !x->inner().empty()) x = &x->inner()[0];
    const Json* ref = x->get("referencedDecl");
    return need(ref ? ref->str("id") : x->str("referencedMemberDecl"));
  }
  const std::vector<int>& kindsOf(const Json& c) {
    static const std::vector<int> none;
    auto it = refKinds_.find(idOf(c));
    return it == refKinds_.end() ? none : it->second;
  }
  // The value the callee declares for its parameter number `p`.
  std::string defaultArg(const Json& c, size_t p, const Json& at) {
    auto it = fnDecl_.find(idOf(c));
    if (it != fnDecl_.end()) {
      size_t i = 0;
      for (auto& d : it->second->inner()) {
        if (d.kind() != "ParmVarDecl") continue;
        if (i++ != p) continue;
        for (auto& v : d.inner())
          if (present(v)) return ex(v);
      }
    }
    unsupported(at, "this default argument (pass the value)");
  }
  std::string callee(const Json& c, const Json& at) {
    auto it = fnName_.find(idOf(c));
    if (it == fnName_.end()) {
      const Json* x = &c;
      while ((x->kind() == "ImplicitCastExpr" || x->kind() == "ParenExpr") && !x->inner().empty()) x = &x->inner()[0];
      const Json* ref = x->get("referencedDecl");
      unsupported(at, "a call to '" + (ref ? ref->str("name") : x->str("name")) + "', which clang shows no body for");
    }
    return it->second;
  }
  // A member function that returns *this by reference (operator+=, a
  // setter that chains) is written as a void function: the call runs
  // before the statement and its value is the object it was called on.
  std::set<std::string> selfRet_;
  const Json* selfNode_ = nullptr;
  std::string selfCall(const Json& n, const std::string& id, const std::string& call, const std::string& obj) {
    if (!selfRet_.count(id)) return call;
    pre_.push_back(call + ";");
    selfNode_ = &n;
    return obj;
  }
  static bool returnsThis(const Json& n) {
    if (n.kind() == "ReturnStmt") {
      if (n.inner().empty()) return false;
      const Json* e = strip(n.inner()[0]);
      while (e->kind() == "ParenExpr") e = strip(e->inner()[0]);
      auto self = [](const Json* x) {
        x = strip(*x);
        while (x->kind() == "ParenExpr") x = strip(x->inner()[0]);
        return x->kind() == "UnaryOperator" && x->str("opcode") == "*" && isThis(x->inner()[0]);
      };
      if (self(e)) return true;
      // "return *this += 1;": another such member function, called on this object.
      if (e->kind() == "CXXOperatorCallExpr" && e->inner().size() > 1) return self(&e->inner()[1]);
      if (e->kind() == "CXXMemberCallExpr" && !e->inner().empty() && !e->inner()[0].inner().empty()) {
        const Json& obj = e->inner()[0].inner()[0];
        return isThis(obj) || self(&obj);
      }
      return false;
    }
    for (auto& c : n.inner())
      if (c.k == Json::Obj && c.kind() != "LambdaExpr" && !returnsThisOk(c)) return false;
    return true;
  }
  static bool returnsThisOk(const Json& c) { return returnsThis(c); }
  // The mhls front end takes an object returned by a call only as the
  // whole right side of an assignment to a variable.
  std::string ex(const Json& n) {
    std::string k = n.kind();
    if ((k == "CallExpr" || k == "CXXMemberCallExpr" || k == "CXXOperatorCallExpr") && cur_ && isRecordType(n)) {
      selfNode_ = nullptr;
      std::string v = ex1(n);
      if (selfNode_ == &n) return v;
      if (v.empty() || v[0] == '(') return v;   // a plain copy written as an assignment
      std::string t = "mhls_t" + std::to_string(++temps_);
      pre_.push_back(ctype(n).base + " " + t + "; " + t + " = " + v + ";");
      return t;
    }
    return ex1(n);
  }
  std::string ex1(const Json& n) {
    std::string k = n.kind();
    const auto& in = n.inner();
    if (k == "IntegerLiteral") {
      std::string v = n.str("value"), q = n.get("type") ? n.get("type")->str("qualType") : "int";
      if (q.find("long") != std::string::npos) return "((" + ctypeStr(q, n).base + ")" + v + (q.find("unsigned") != std::string::npos ? "u" : "") + ")";
      return v + (q.find("unsigned") != std::string::npos ? "u" : "");
    }
    if (k == "CXXBoolLiteralExpr") return n.flag("value") ? "1" : "0";
    if (k == "CharacterLiteral") return n.str("value");
    if (k == "ParenExpr") return "(" + ex(in[0]) + ")";
    if (k == "ConstantExpr" || k == "ExprWithCleanups" || k == "MaterializeTemporaryExpr" || k == "CXXBindTemporaryExpr" ||
        k == "SubstNonTypeTemplateParmExpr")
      return ex(in.back());
    if (k == "ImplicitValueInitExpr" || k == "CXXScalarValueInitExpr" || k == "CXXNullPtrLiteralExpr") {
      const Json* t = n.get("type");
      return t && (t->str("qualType").find('[') != std::string::npos || isRecordType(n)) ? "{0}" : "0";
    }
    if (k == "DeclRefExpr") {
      const Json* r = n.get("referencedDecl");
      if (!r) unsupported(n, "this reference");
      std::string rk = r->str("kind");
      if (rk == "EnumConstantDecl") {
        auto it = enumVal_.find(r->str("id"));
        if (it == enumVal_.end()) unsupported(n, "an enumerator declared outside this file ('" + r->str("name") + "')");
        return it->second;
      }
      if (rk == "FunctionDecl" || rk == "CXXMethodDecl") return callee(n, n);
      auto lv = lazyVar_.find(r->str("id"));
      if (lv != lazyVar_.end()) {
        const Json* d = lv->second;
        lazyVar_.erase(lv);
        Fn* was = cur_;
        cur_ = nullptr;
        globals_ << varDecl(*d) << "\n";
        cur_ = was;
      }
      auto v = varName_.find(r->str("id"));
      return v != varName_.end() ? v->second : r->str("name");
    }
    if (k == "ImplicitCastExpr" || k == "CStyleCastExpr" || k == "CXXStaticCastExpr" || k == "CXXFunctionalCastExpr" ||
        k == "CXXConstCastExpr") {
      std::string ck = n.str("castKind");
      if (ck == "IntegralToBoolean") return "((" + ex(in[0]) + ") != 0)";
      if (ck == "IntegralCast" || ck == "BooleanToSignedIntegral") return "((" + ctype(n).base + ")(" + ex(in[0]) + "))";
      if (ck == "IntegralToFloating" || ck == "FloatingToIntegral" || ck == "FloatingCast") unsupported(n, "floating point");
      if (ck == "DerivedToBase" || ck == "UncheckedDerivedToBase") unsupported(n, "base classes");
      return ex(in[0]);
    }
    if (k == "UnaryOperator") {
      std::string op = n.str("opcode");
      if ((op == "*" || op == "&") && isThis(in[0])) return "self";
      if (op == "&") return ex(in[0]);   // an object handed to a pointer parameter
      if (n.flag("isPostfix")) return "(" + ex(in[0]) + op + ")";
      return "(" + op + ex(in[0]) + ")";
    }
    if (k == "BinaryOperator" || k == "CompoundAssignOperator") return "(" + ex(in[0]) + " " + n.str("opcode") + " " + ex(in[1]) + ")";
    if (k == "ConditionalOperator" && isRecordType(n) && cur_) {
      // Choosing between two objects: a variable set by an if.
      std::string t = "mhls_t" + std::to_string(++temps_), c = ex(in[0]), a = ex(in[1]), b = ex(in[2]);
      pre_.push_back(ctype(n).base + " " + t + "; if (" + c + ") " + t + " = " + a + "; else " + t + " = " + b + ";");
      return t;
    }
    if (k == "ConditionalOperator") return "(" + ex(in[0]) + " ? " + ex(in[1]) + " : " + ex(in[2]) + ")";
    if (k == "ArraySubscriptExpr") return ex(in[0]) + "[" + ex(in[1]) + "]";
    if (k == "CXXThisExpr") return "self";
    if (k == "MemberExpr") {
      if (isThis(in[0])) return "self." + n.str("name");
      return ex(in[0]) + (n.flag("isArrow") ? "->" : ".") + n.str("name");
    }
    if (k == "InitListExpr") {
      // A list shorter than its array comes as the filler value followed by
      // the elements that were written.
      std::string s = "{";
      const Json* fill = n.get("array_filler");
      const std::vector<Json>& items = fill ? fill->a : in;
      size_t count = 0;
      for (size_t i = fill ? 1 : 0; i < items.size(); i++, count++) s += (count ? ", " : "") + ex(items[i]);
      if (fill && !fill->a.empty()) {
        std::string q = n.get("type") ? n.get("type")->str("qualType") : "";
        size_t b = q.find('[');
        size_t want = b == std::string::npos ? count : (size_t)atoll(q.c_str() + b + 1);
        std::string z = ex(fill->a[0]);
        for (; count < want && want <= 65536; count++) s += (count ? ", " : "") + z;
      }
      return s + "}";
    }
    if (k == "CXXMemberCallExpr") {
      const Json& m = in[0];
      const Json& obj = m.inner()[0];
      if (isStream(obj)) {
        std::string s = ex(obj) + "." + m.str("name") + "(";
        for (size_t i = 1; i < in.size(); i++) s += (i > 1 ? ", " : "") + ex(in[i]);
        return s + ")";
      }
      const std::vector<int>& kinds = kindsOf(m);
      auto kind = [&](size_t i) { return i < kinds.size() ? kinds[i] : 0; };
      std::string first = isThis(obj) ? "self" : m.flag("isArrow") ? "(*" + ex(obj) + ")" : refArg(obj, kind(0));
      std::string s = callee(m, n) + "(" + first;
      for (size_t i = 1; i < in.size(); i++) {
        if (in[i].kind() == "CXXDefaultArgExpr") { s += ", " + defaultArg(m, i - 1, n); continue; }
        s += ", " + refArg(in[i], kind(i));
      }
      return selfCall(n, idOf(m), s + ")", first);
    }
    if (k == "CXXOperatorCallExpr" || k == "CallExpr") {
      if (k == "CXXOperatorCallExpr" && in.size() == 3) {
        // A copy assignment the class does not define is a plain copy.
        const Json* x = &in[0];
        while (x->kind() == "ImplicitCastExpr" && !x->inner().empty()) x = &x->inner()[0];
        const Json* ref = x->get("referencedDecl");
        if (ref && ref->str("name") == "operator=" && need(ref->str("id")).empty()) return "(" + ex(in[1]) + " = " + ex(in[2]) + ")";
      }
      const std::vector<int>& kinds = kindsOf(in[0]);
      std::string s = callee(in[0], n) + "(", first;
      for (size_t i = 1; i < in.size(); i++) {
        if (in[i].kind() == "CXXDefaultArgExpr") {
          bool member = !kinds.empty() && k == "CXXOperatorCallExpr" && fnDecl_.count(idOf(in[0])) && fnDecl_[idOf(in[0])]->kind() != "FunctionDecl";
          s += (i > 1 ? ", " : "") + defaultArg(in[0], member ? i - 2 : i - 1, n);
          continue;
        }
        std::string a = refArg(in[i], i - 1 < kinds.size() ? kinds[i - 1] : 0);
        if (i == 1) first = a;
        s += (i > 1 ? ", " : "") + a;
      }
      return selfCall(n, idOf(in[0]), s + ")", first);
    }
    if (k == "CXXConstructExpr" || k == "CXXTemporaryObjectExpr") {
      std::string ctor = ctorOf(n);
      if (ctor.empty() && in.size() == 1) return ex(in[0]);   // a copy
      if (!cur_) unsupported(n, "a constructor call outside a function");
      std::string t = "mhls_t" + std::to_string(++temps_), d = ctype(n).base + " " + t;
      if (ctor.empty()) {
        if (!in.empty()) unsupported(n, "this constructor");
        d += " = {0};";
      } else {
        d += "; " + ctor + "(" + t;
        const std::vector<int>& kinds = refKinds_[ctorId_[ctor]];
        size_t i = 1;
        for (auto& a : in) d += ", " + refArg(a, i < kinds.size() ? kinds[i] : 0), i++;
        d += ");";
      }
      pre_.push_back(d);
      return t;
    }
    if (k == "UnaryExprOrTypeTraitExpr") unsupported(n, n.str("name"));
    if (k == "FloatingLiteral") unsupported(n, "floating point");
    unsupported(n, k.empty() ? "this construct" : word(k));
  }
};

}   // namespace hls
