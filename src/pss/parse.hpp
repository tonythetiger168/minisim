// parse.hpp - lexer and recursive-descent parser for the PSS DSL subset of
// mpss (docs/MPSS.md).
#pragma once
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>

#include "ast.hpp"

namespace pss {

struct Tok {
    enum K { End, Id, Num, Str, Punct } k = End;
    std::string s;
    i128 num = 0;
    int line = 0;
};

inline std::vector<Tok> lex(const std::string& src, const std::string& fname) {
    std::vector<Tok> out;
    size_t i = 0, n = src.size();
    int line = 1;
    auto err = [&](const std::string& m) { throw Error(fname + ":" + std::to_string(line) + ": " + m); };
    auto digit = [](char c, int base) -> int {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
        return d < base ? d : -1;
    };
    while (i < n) {
        char c = src[i];
        if (c == '\n') { line++; i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '/') { while (i < n && src[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) { if (src[i] == '\n') line++; i++; }
            if (i + 1 >= n) err("unterminated comment");
            i += 2;
            continue;
        }
        Tok t;
        t.line = line;
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < n && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
            t.k = Tok::Id;
            t.s = src.substr(i, j - i);
            i = j;
        } else if (isdigit((unsigned char)c) || (c == '\'' && i + 1 < n && isalpha((unsigned char)src[i + 1]))) {
            // 123, 0x1f, 0b101, 0o17, and Verilog-style 8'hff / 'd5.
            int base = 10;
            i128 v = 0;
            auto digits = [&]() {
                bool any = false;
                while (i < n && (digit(src[i], base) >= 0 || src[i] == '_')) {
                    if (src[i] != '_') { v = v * base + digit(src[i], base); any = true; }
                    i++;
                }
                if (!any) err("bad number");
            };
            if (c == '0' && i + 1 < n && (src[i + 1] == 'x' || src[i + 1] == 'X')) { base = 16; i += 2; digits(); }
            else if (c == '0' && i + 1 < n && (src[i + 1] == 'b' || src[i + 1] == 'B')) { base = 2; i += 2; digits(); }
            else if (c == '0' && i + 1 < n && (src[i + 1] == 'o' || src[i + 1] == 'O')) { base = 8; i += 2; digits(); }
            else {
                if (c != '\'') digits();
                if (i + 1 < n && src[i] == '\'' && isalpha((unsigned char)src[i + 1])) {
                    i++;
                    if (src[i] == 's' || src[i] == 'S') i++;
                    char b = (char)tolower((unsigned char)src[i++]);
                    base = b == 'h' ? 16 : b == 'b' ? 2 : b == 'o' ? 8 : b == 'd' ? 10 : 0;
                    if (!base) err("bad number base");
                    v = 0;
                    digits();
                }
            }
            t.k = Tok::Num;
            t.num = v;
        } else if (c == '"') {
            t.k = Tok::Str;
            if (src.compare(i, 3, "\"\"\"") == 0) {
                size_t e = src.find("\"\"\"", i + 3);
                if (e == std::string::npos) err("unterminated \"\"\" string");
                t.s = src.substr(i + 3, e - i - 3);
                for (char ch : t.s) if (ch == '\n') line++;
                i = e + 3;
            } else {
                i++;
                while (i < n && src[i] != '"') {
                    if (src[i] == '\n') err("unterminated string");
                    if (src[i] == '\\' && i + 1 < n) {
                        char e = src[++i];
                        t.s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    } else t.s += src[i];
                    i++;
                }
                if (i >= n) err("unterminated string");
                i++;
            }
        } else {
            static const char* three[] = {"<<=", ">>=", "..."};
            static const char* two[] = {"::", "->", "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "..", "**",
                                        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
            t.k = Tok::Punct;
            for (auto p : three) if (t.s.empty() && src.compare(i, 3, p) == 0) t.s = p;
            for (auto p : two) if (t.s.empty() && src.compare(i, 2, p) == 0) t.s = p;
            if (t.s.empty()) t.s = std::string(1, c);
            i += t.s.size();
        }
        out.push_back(std::move(t));
    }
    Tok e;
    e.line = line;
    out.push_back(e);
    return out;
}

class Parser {
public:
    Parser(Model& m, std::vector<Tok> toks, int file) : m_(m), t_(std::move(toks)), file_(file) {}

    void parseFile() {
        while (!at(Tok::End)) item(nullptr, "");
    }
    // One expression and nothing after it (a {{...}} of a target template).
    EP parseExpr() {
        EP e = expr();
        if (!at(Tok::End)) fail("unexpected text after the expression");
        return e;
    }

private:
    Model& m_;
    std::vector<Tok> t_;
    size_t p_ = 0;
    int file_;

    const Tok& cur() const { return t_[p_]; }
    const Tok& peek(int k = 1) const { return t_[std::min(p_ + k, t_.size() - 1)]; }
    bool at(Tok::K k) const { return cur().k == k; }
    bool isP(const char* s) const { return cur().k == Tok::Punct && cur().s == s; }
    bool isK(const char* s) const { return cur().k == Tok::Id && cur().s == s; }
    Loc loc() const { return {file_, cur().line}; }
    [[noreturn]] void fail(const std::string& msg) const {
        std::string got = at(Tok::End) ? "end of file" : "'" + (at(Tok::Num) ? i128str(cur().num) : cur().s) + "'";
        m_.fail(loc(), msg + " (at " + got + ")");
    }
    bool acceptP(const char* s) { if (isP(s)) { p_++; return true; } return false; }
    bool acceptK(const char* s) { if (isK(s)) { p_++; return true; } return false; }
    void expectP(const char* s) { if (!acceptP(s)) fail(std::string("expected '") + s + "'"); }
    std::string ident(const char* what = "a name") {
        if (!at(Tok::Id)) fail(std::string("expected ") + what);
        return t_[p_++].s;
    }
    std::string qname() {
        std::string s = ident();
        while (isP("::") && peek().k == Tok::Id) { p_++; s += "::" + ident(); }
        return s;
    }

    // ------------------------------------------------------------ expressions
    EP mk(Expr::K k, Loc l) { auto e = std::make_shared<Expr>(); e->k = k; e->loc = l; return e; }
    EP num(i128 v, Loc l) { auto e = mk(Expr::Num, l); e->num = v; return e; }

    EP primary() {
        Loc l = loc();
        if (at(Tok::Num)) return num(t_[p_++].num, l);
        if (at(Tok::Str)) { auto e = mk(Expr::Str, l); e->s = t_[p_++].s; return e; }
        if (acceptP("(")) { EP e = expr(); expectP(")"); return e; }
        if (isK("true") || isK("false")) return num(t_[p_++].s == "true", l);
        if (!at(Tok::Id)) fail("expected an expression");
        std::string first = ident();
        if (isP("(")) {
            auto e = mk(Expr::Call, l);
            e->s = first;
            p_++;
            if (!isP(")")) do e->a.push_back(expr()); while (acceptP(","));
            expectP(")");
            return e;
        }
        auto e = mk(Expr::Ref, l);
        std::string name = first;
        while (isP("::") && peek().k == Tok::Id) { p_++; e->scope.push_back(name); name = ident(); }
        for (;;) {
            PathElem pe;
            pe.name = name;
            if (acceptP("[")) { pe.index = expr(); expectP("]"); }
            e->path.push_back(pe);
            if (isP(".") && peek().k == Tok::Id) { p_++; name = ident(); continue; }
            break;
        }
        return e;
    }
    EP unary() {
        Loc l = loc();
        for (const char* op : {"!", "-", "~", "+"})
            if (isP(op)) {
                p_++;
                EP x = unary();
                if (*op == '+') return x;
                auto e = mk(Expr::Unary, l);
                e->s = op;
                e->a = {x};
                return e;
            }
        return primary();
    }
    static int prec(const std::string& s) {
        static const std::map<std::string, int> t = {
            {"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6}, {"<", 7}, {"<=", 7},
            {">", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}, {"**", 11}};
        auto it = t.find(s);
        return it == t.end() ? 0 : it->second;
    }
    std::vector<std::pair<EP, EP>> rangeList() {
        std::vector<std::pair<EP, EP>> r;
        expectP("[");
        do {
            EP lo = expr(), hi;
            if (acceptP("..")) hi = expr();
            r.push_back({lo, hi});
        } while (acceptP(","));
        expectP("]");
        return r;
    }
    EP binary(int minPrec) {
        EP lhs = unary();
        for (;;) {
            Loc l = loc();
            if (isK("in") && minPrec <= 7) {
                p_++;
                auto e = mk(Expr::In, l);
                e->a = {lhs};
                e->ranges = rangeList();
                lhs = e;
                continue;
            }
            if (!at(Tok::Punct)) break;
            int pr = prec(cur().s);
            if (!pr || pr < minPrec) break;
            auto e = mk(Expr::Binary, l);
            e->s = t_[p_++].s;
            e->a = {lhs, binary(pr + 1)};
            lhs = e;
        }
        return lhs;
    }
    EP expr() {
        EP c = binary(1);
        if (isP("?")) {
            auto e = mk(Expr::Cond, loc());
            p_++;
            EP a = expr();
            expectP(":");
            EP b = expr();
            e->a = {c, a, b};
            return e;
        }
        return c;
    }

    // ------------------------------------------------------------------ types
    bool atType() const {
        if (!at(Tok::Id)) return false;
        const std::string& s = cur().s;
        return s == "bit" || s == "int" || s == "bool" || s == "string" || s == "chandle" || s == "array";
    }
    // "array<T, N>": the size comes back through dim.
    TypeRef type(EP* dim = nullptr) {
        TypeRef t;
        if (acceptK("array")) {
            expectP("<");
            t = type();
            expectP(",");
            EP d = binary(8);
            expectP(">");
            if (!dim) fail("array<> is not allowed here");
            *dim = d;
            return t;
        }
        if (isK("bit") || isK("int")) {
            t.k = cur().s == "bit" ? TypeRef::Bit : TypeRef::Int;
            p_++;
            if (acceptP("[")) {
                EP hi = expr();
                if (acceptP(":")) {
                    EP lo = expr();
                    auto w = mk(Expr::Binary, hi->loc);
                    w->s = "-";
                    w->a = {hi, lo};
                    auto w1 = mk(Expr::Binary, hi->loc);
                    w1->s = "+";
                    w1->a = {w, num(1, hi->loc)};
                    t.width = w1;
                } else t.width = hi;
                expectP("]");
            }
            if (isK("in") && peek().k == Tok::Punct && peek().s == "[") { p_++; t.dom = rangeList(); }
            return t;
        }
        if (acceptK("bool")) { t.k = TypeRef::Bool; return t; }
        if (acceptK("string")) { t.k = TypeRef::String; return t; }
        if (acceptK("chandle")) { t.k = TypeRef::Int; t.width = num(64, loc()); return t; }
        if (acceptK("void")) return t;
        t.k = TypeRef::Named;
        t.name = qname();
        return t;
    }

    // ------------------------------------------------------------ constraints
    std::vector<CP> consSet() {
        std::vector<CP> r;
        if (acceptP("{")) {
            while (!acceptP("}")) consItem(r);
        } else consItem(r);
        return r;
    }
    void foreachHead(std::string& it, std::string& idx, EP& coll) {
        expectP("(");
        if (at(Tok::Id) && peek().k == Tok::Punct && peek().s == ":") { it = ident(); p_++; }
        coll = primary();
        if (coll->k != Expr::Ref) fail("foreach needs an array");
        auto& last = coll->path.back();
        if (last.index) {
            if (last.index->k != Expr::Ref || last.index->path.size() != 1 || last.index->path[0].index)
                fail("the foreach index must be a name");
            idx = last.index->path[0].name;
            last.index = nullptr;
        }
        expectP(")");
    }
    void consItem(std::vector<CP>& out) {
        auto c = std::make_shared<Cons>();
        c->loc = loc();
        if (acceptP(";")) return;
        if (acceptK("if")) {
            c->k = Cons::If;
            expectP("(");
            c->e = expr();
            expectP(")");
            c->body = consSet();
            if (acceptK("else")) c->els = consSet();
        } else if (acceptK("foreach")) {
            c->k = Cons::Foreach;
            foreachHead(c->it, c->idx, c->e);
            c->body = consSet();
        } else if (acceptK("unique")) {
            c->k = Cons::Unique;
            expectP("{");
            do c->list.push_back(expr()); while (acceptP(","));
            expectP("}");
            acceptP(";");
        } else if (isK("forall") || isK("default") || isK("disable")) {
            fail("'" + cur().s + "' constraints are not supported yet");
        } else {
            EP e = expr();
            if (acceptP("->")) {
                c->k = Cons::If;
                c->e = e;
                c->body = consSet();
            } else {
                c->e = e;
                expectP(";");
            }
        }
        out.push_back(c);
    }
    // After the "constraint" keyword.
    void constraintDecl(std::vector<CP>& out) {
        if (at(Tok::Id) && !isK("unique") && peek().k == Tok::Punct && peek().s == "{") p_++;   // a named block
        if (acceptP("{")) {
            while (!acceptP("}")) consItem(out);
        } else consItem(out);
    }

    // --------------------------------------------------------------- activity
    AP mkA(Act::K k) { auto a = std::make_shared<Act>(); a->k = k; a->loc = loc(); return a; }
    void actBlock(std::vector<AP>& out) {
        expectP("{");
        while (!acceptP("}")) if (AP a = actStmt()) out.push_back(a);
    }
    void withClause(Act& a) {
        if (!acceptK("with")) return;
        if (acceptP("{")) { while (!acceptP("}")) consItem(a.with); }
        else { consItem(a.with); return; }
    }
    AP actStmt() {
        if (acceptP(";")) return nullptr;
        if (at(Tok::Id) && peek().k == Tok::Punct && peek().s == ":") p_ += 2;   // label
        if (isP("{") || isK("sequence") || isK("atomic")) {
            if (at(Tok::Id)) p_++;
            AP a = mkA(Act::Seq);
            actBlock(a->body);
            return a;
        }
        if (acceptK("parallel")) {
            AP a = mkA(Act::Par);
            if (acceptP("(")) { while (!acceptP(")")) p_++; }   // join specification: all branches joined
            actBlock(a->body);
            return a;
        }
        if (acceptK("schedule")) {
            AP a = mkA(Act::Sched);
            actBlock(a->body);
            return a;
        }
        if (acceptK("select")) {
            AP a = mkA(Act::Select);
            expectP("{");
            while (!acceptP("}")) {
                EP guard, weight;
                if (isP("(")) { p_++; guard = expr(); expectP(")"); }
                if (isP("[")) { p_++; weight = expr(); expectP("]"); }
                if (guard || weight) expectP(":");
                AP b = actStmt();
                if (!b) continue;
                a->body.push_back(b);
                a->guards.push_back(guard);
                a->weights.push_back(weight);
            }
            return a;
        }
        if (isK("repeat") || isK("replicate")) {
            AP a = mkA(cur().s == "repeat" ? Act::Repeat : Act::Replicate);
            p_++;
            expectP("(");
            if (at(Tok::Id) && peek().k == Tok::Punct && peek().s == ":") { a->idx = ident(); p_++; }
            a->e = expr();
            expectP(")");
            if (AP b = actStmt()) a->body.push_back(b);
            return a;
        }
        if (acceptK("if")) {
            AP a = mkA(Act::If);
            expectP("(");
            a->e = expr();
            expectP(")");
            if (AP b = actStmt()) a->body.push_back(b);
            if (acceptK("else")) if (AP b = actStmt()) a->els.push_back(b);
            return a;
        }
        if (acceptK("constraint")) {
            AP a = mkA(Act::Constraint);
            constraintDecl(a->with);
            return a;
        }
        if (acceptK("bind")) {
            AP a = mkA(Act::Bind);
            do a->binds.push_back(primary()); while (!isP(";") && (acceptP(",") || true));
            expectP(";");
            return a;
        }
        if (acceptK("do")) {
            AP a = mkA(Act::Do);
            a->type = qname();
            withClause(*a);
            acceptP(";");
            return a;
        }
        if (isK("foreach") || isK("match") || isK("while")) fail("'" + cur().s + "' is not supported in an activity yet");
        AP a = mkA(Act::Traverse);
        a->handle.name = ident("an activity statement");
        if (acceptP("[")) { a->handle.index = expr(); expectP("]"); }
        withClause(*a);
        acceptP(";");
        return a;
    }

    // ------------------------------------------------------------- procedural
    SP mkS(Stmt::K k) { auto s = std::make_shared<Stmt>(); s->k = k; s->loc = loc(); return s; }
    std::vector<SP> stmtOrBlock() {
        std::vector<SP> r;
        if (acceptP("{")) { while (!acceptP("}")) stmt(r); }
        else stmt(r);
        return r;
    }
    void stmt(std::vector<SP>& out) {
        if (acceptP(";")) return;
        if (isP("{")) {
            SP s = mkS(Stmt::Block);
            s->body = stmtOrBlock();
            out.push_back(s);
            return;
        }
        if (acceptK("if")) {
            SP s = mkS(Stmt::If);
            expectP("(");
            s->e = expr();
            expectP(")");
            s->body = stmtOrBlock();
            if (acceptK("else")) s->els = stmtOrBlock();
            out.push_back(s);
            return;
        }
        if (acceptK("repeat")) {
            SP s = mkS(Stmt::Repeat);
            expectP("(");
            if (at(Tok::Id) && peek().k == Tok::Punct && peek().s == ":") { s->name = ident(); p_++; }
            s->e = expr();
            expectP(")");
            s->body = stmtOrBlock();
            out.push_back(s);
            return;
        }
        if (acceptK("while")) {
            SP s = mkS(Stmt::While);
            expectP("(");
            s->e = expr();
            expectP(")");
            s->body = stmtOrBlock();
            out.push_back(s);
            return;
        }
        if (acceptK("return")) {
            SP s = mkS(Stmt::Return);
            if (!isP(";")) s->e = expr();
            expectP(";");
            out.push_back(s);
            return;
        }
        if (isK("break") || isK("continue")) {
            out.push_back(mkS(cur().s == "break" ? Stmt::Break : Stmt::Continue));
            p_++;
            expectP(";");
            return;
        }
        if (isK("foreach") || isK("match")) fail("'" + cur().s + "' is not supported in an exec block yet");
        if (atType() || (at(Tok::Id) && peek().k == Tok::Id)) {
            TypeRef t = type();
            do {
                SP s = mkS(Stmt::Var);
                s->type = t;
                s->name = ident();
                if (acceptP("=")) s->e = expr();
                out.push_back(s);
            } while (acceptP(","));
            expectP(";");
            return;
        }
        EP e = expr();
        static const std::set<std::string> ops = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="};
        if (at(Tok::Punct) && ops.count(cur().s)) {
            SP s = mkS(Stmt::Assign);
            s->lhs = e;
            s->op = t_[p_++].s;
            s->e = expr();
            expectP(";");
            out.push_back(s);
            return;
        }
        SP s = mkS(Stmt::Expr_);
        s->e = e;
        expectP(";");
        out.push_back(s);
    }

    void exec(std::vector<Exec>& out) {
        Exec x;
        x.loc = loc();
        x.kind = ident("an exec block kind");
        if (isP("{")) {
            x.stmts = stmtOrBlock();
        } else {
            x.lang = ident("a target language");
            if (x.lang == "file") fail("exec file blocks are not supported yet");
            expectP("=");
            if (!at(Tok::Str)) fail("expected the target code string");
            x.text = t_[p_++].s;
            acceptP(";");
        }
        out.push_back(std::move(x));
    }

    void function(TypeRef ret) {
        Function f;
        f.loc = loc();
        f.ret = ret;
        f.name = ident("a function name");
        expectP("(");
        if (!isP(")")) do {
            for (const char* d : {"input", "output", "inout"}) acceptK(d);
            Param p;
            p.type = type();
            p.name = ident("a parameter name");
            if (acceptP("=")) expr();
            f.params.push_back(p);
        } while (acceptP(","));
        expectP(")");
        if (isP("{")) { f.hasBody = true; f.body = stmtOrBlock(); }
        else expectP(";");
        for (auto& g : m_.funcs)
            if (g.name == f.name) {
                if (f.hasBody && !g.hasBody) g = f;
                return;
            }
        m_.funcs.push_back(std::move(f));
    }

    // ------------------------------------------------------------ declarations
    TypeDecl* newType(TypeDecl* owner, const std::string& pkg, TypeDecl::K k, bool extend) {
        Loc l = loc();
        std::string name = extend ? qname() : ident("a type name");
        std::string q = owner ? owner->qname + "::" + name : pkg.empty() ? name : pkg + "::" + name;
        if (extend) {
            TypeDecl* t = nullptr;
            for (TypeDecl* c : m_.all)
                if (c->qname == name || c->qname == q || c->name == name) { t = c; break; }
            if (!t) m_.fail(l, "extend: no type '" + name + "'");
            return t;
        }
        auto up = std::make_unique<TypeDecl>();
        TypeDecl* t = up.get();
        t->k = k;
        t->name = name;
        t->qname = q;
        t->owner = owner;
        t->loc = l;
        t->id = (int)m_.all.size();
        for (TypeDecl* c : m_.all)
            if (c->qname == q) m_.fail(l, "'" + q + "' is declared twice (first at " + m_.where(c->loc) + ")");
        m_.all.push_back(t);
        (owner ? owner->nested : m_.types).push_back(std::move(up));
        return t;
    }

    void typeBody(TypeDecl* t, const std::string& pkg) {
        if (acceptP(":")) t->super = qname();
        expectP("{");
        while (!acceptP("}")) member(t, pkg);
        acceptP(";");
    }

    void enumBody(TypeDecl* t) {
        expectP("{");
        while (!acceptP("}")) {
            std::string n = ident("an enum item");
            EP v;
            if (acceptP("=")) v = expr();
            t->items.push_back({n, v});
            if (!acceptP(",") && !isP("}")) fail("expected ',' or '}'");
        }
        acceptP(";");
    }

    // A declaration that may appear at file, package, or component level.
    // Returns false if the tokens are not one.
    bool item(TypeDecl* owner, const std::string& pkg) {
        if (acceptP(";")) return true;
        if (!owner && acceptK("package")) {
            std::string name = qname();
            expectP("{");
            while (!acceptP("}")) item(nullptr, pkg.empty() ? name : pkg + "::" + name);
            return true;
        }
        if (isK("import") && !(peek().k == Tok::Id && (peek().s == "target" || peek().s == "solve" || peek().s == "function" ||
                                                        peek(2).s == "function"))) {
            p_++;
            qname();
            if (acceptP("::")) expectP("*");
            expectP(";");
            return true;
        }
        if (isK("import") || isK("target") || isK("solve") || isK("pure") || isK("function")) {
            size_t save = p_;
            bool imp = acceptK("import");
            while (isK("target") || isK("solve") || isK("pure")) p_++;
            if (imp && at(Tok::Id) && !isK("function") && peek().k == Tok::Id && peek().s == "function") p_++;   // language
            if (isK("component") && p_ > save) { p_ = save + 1; }   // "pure component"
            else if (acceptK("function")) {
                if (imp && at(Tok::Id) && peek().k == Tok::Punct && peek().s == ";") { p_ += 2; return true; }
                TypeRef ret = type();
                function(ret);
                return true;
            } else fail("expected 'function'");
        }
        bool abstract = acceptK("abstract");
        bool extend = acceptK("extend");
        static const std::map<std::string, TypeDecl::K> kinds = {
            {"struct", TypeDecl::Struct}, {"buffer", TypeDecl::Buffer}, {"stream", TypeDecl::Stream},
            {"state", TypeDecl::State}, {"resource", TypeDecl::Resource}, {"action", TypeDecl::Action},
            {"component", TypeDecl::Component}, {"enum", TypeDecl::Enum}};
        if (at(Tok::Id)) {
            auto it = kinds.find(cur().s);
            if (it != kinds.end() && peek().k == Tok::Id) {
                p_++;
                TypeDecl* t = newType(it->second == TypeDecl::Action ? owner : nullptr,
                                      owner && it->second != TypeDecl::Action ? owner->qname : pkg, it->second, extend);
                t->isAbstract |= abstract;
                if (t->k == TypeDecl::Enum) enumBody(t);
                else typeBody(t, pkg);
                return true;
            }
        }
        if (abstract || extend) fail("expected a type declaration");
        if (acceptK("typedef")) {
            TypeRef t = type();
            std::string n = ident();
            expectP(";");
            m_.typedefs[n] = t;
            return true;
        }
        if (!owner) {
            // Package-level constant: [static] const type name = value;
            acceptK("static");
            if (acceptK("const")) {
                type();
                std::string n = ident();
                expectP("=");
                m_.consts[n] = expr();
                expectP(";");
                return true;
            }
            fail("expected a declaration");
        }
        return false;
    }

    void member(TypeDecl* t, const std::string& pkg) {
        if (t->k == TypeDecl::Component && item(t, pkg)) return;
        if (t->k != TypeDecl::Component && acceptP(";")) return;
        for (const char* q : {"public", "private", "protected", "static", "const"}) acceptK(q);
        Loc l = loc();
        if (acceptK("constraint")) { constraintDecl(t->cons); return; }
        if (acceptK("dynamic")) fail("dynamic constraints are not supported yet");
        if (acceptK("activity")) {
            AP a = mkA(Act::Seq);
            actBlock(a->body);
            t->activity.push_back(a);
            return;
        }
        if (acceptK("exec")) { exec(t->execs); return; }
        if (t->k == TypeDecl::Component && acceptK("pool")) {
            Field f;
            f.k = Field::Pool;
            f.loc = l;
            if (acceptP("[")) { f.poolSize = expr(); expectP("]"); }
            f.type = type();
            f.name = ident();
            expectP(";");
            t->fields.push_back(f);
            return;
        }
        if (t->k == TypeDecl::Component && acceptK("bind")) {
            // Every pool is visible to the whole subtree of its component.
            while (!acceptP(";")) {
                if (at(Tok::End)) fail("expected ';'");
                p_++;
            }
            return;
        }
        Field proto;
        proto.loc = l;
        if (acceptK("input")) proto.k = Field::Input;
        else if (acceptK("output")) proto.k = Field::Output;
        else if (acceptK("lock")) proto.k = Field::Lock;
        else if (acceptK("share")) proto.k = Field::Share;
        proto.rand = acceptK("rand");
        EP dim;
        proto.type = type(&dim);
        do {
            Field f = proto;
            f.loc = loc();
            f.name = ident("a field name");
            f.dim = dim;
            if (acceptP("[")) { f.dim = expr(); expectP("]"); }
            if (acceptP("=")) f.init = expr();
            if (t->field(f.name)) m_.fail(f.loc, "'" + f.name + "' is declared twice in " + t->qname);
            t->fields.push_back(f);
        } while (acceptP(","));
        expectP(";");
    }
};

inline void parseSource(Model& m, const std::string& text, const std::string& fname) {
    int file = (int)m.files.size();
    m.files.push_back(fname);
    Parser(m, lex(text, fname), file).parseFile();
}

inline void parseFile(Model& m, const std::string& fname) {
    std::ifstream in(fname, std::ios::binary);
    if (!in) throw Error("cannot read " + fname);
    std::stringstream ss;
    ss << in.rdbuf();
    parseSource(m, ss.str(), fname);
}

} // namespace pss
