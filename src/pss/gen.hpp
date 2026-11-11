// gen.hpp - what mpss writes from a solved scenario: a listing, JSON, a C
// test function, and a UVM sequence class.
//
// The action tree becomes straight-line code. Every random field is a
// number by now, so an exec body's references to action fields are printed
// as literals; only the exec block's own local variables stay variables.
//   C     one function per scenario; a parallel block runs its branches one
//         after the other.
//   SV    one uvm_sequence class per scenario; a parallel block is a
//         fork ... join. Imported functions are tasks of an API class the
//         testbench implements (they take time on a bus); a call inside an
//         expression goes through a temporary.
#pragma once
#include "solve.hpp"

namespace pss {

inline std::string cstr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else o += c;
    }
    return o + "\"";
}
inline std::string hexStr(i128 v) {
    if (v == 0) return "0x0";
    std::string s;
    for (unsigned __int128 u = (unsigned __int128)v; u; u >>= 4) s.insert(s.begin(), "0123456789abcdef"[(int)(u & 15)]);
    return "0x" + s;
}
inline std::string ident(std::string s) {
    for (char& c : s)
        if (!isalnum((unsigned char)c)) c = '_';
    return s;
}

enum class Lang { C, SV };

inline std::string literal(i128 v, Lang l) {
    const i128 i32max = 2147483647, u32max = 4294967295LL;
    if (l == Lang::C) {
        if (v >= -i32max && v <= i32max) return i128str(v);
        if (v >= 0 && v <= u32max) return i128str(v) + "u";
        if (v >= 0) return i128str(v) + "ull";
        return "(" + i128str(v + 1) + "ll - 1)";
    }
    if (v >= -i32max && v <= i32max) return i128str(v);
    if (v >= 0) return "64'd" + i128str(v);
    return "-64'sd" + i128str(-v);
}

// A field's value: addresses and other wide unsigned fields in hex.
inline std::string literal(const Var& v, Lang l) {
    if (v.sgn || v.width < 32 || v.enumT || v.isBool) return literal(v.val, l);
    if (l == Lang::SV) return std::to_string(v.width) + "'h" + hexStr(v.val).substr(2);
    return hexStr(v.val) + (v.val > 4294967295LL ? "ull" : "u");
}

// What the model's functions are in the generated SystemVerilog.
struct FuncInfo {
    std::set<std::string> tasks;   // imported, or calls one that is
    bool usesPrint = false;

    static void calls(const Expr& e, std::vector<std::string>& out) {
        if (e.k == Expr::Call) out.push_back(e.s);
        for (auto& a : e.a) calls(*a, out);
        for (auto& p : e.path) if (p.index) calls(*p.index, out);
        for (auto& r : e.ranges) { calls(*r.first, out); if (r.second) calls(*r.second, out); }
    }
    static void calls(const std::vector<SP>& ss, std::vector<std::string>& out) {
        for (auto& s : ss) {
            if (s->e) calls(*s->e, out);
            if (s->lhs) calls(*s->lhs, out);
            calls(s->body, out);
            calls(s->els, out);
        }
    }
    explicit FuncInfo(const Model& m) {
        for (auto& f : m.funcs)
            if (!f.hasBody) tasks.insert(f.name);
        for (bool more = true; more;) {
            more = false;
            for (auto& f : m.funcs) {
                if (tasks.count(f.name)) continue;
                std::vector<std::string> c;
                calls(f.body, c);
                for (auto& n : c)
                    if (tasks.count(n)) { tasks.insert(f.name); more = true; break; }
            }
        }
        std::vector<std::string> c;
        for (auto& f : m.funcs) calls(f.body, c);
        for (TypeDecl* t : m.all)
            for (auto& x : t->execs) calls(x.stmts, c);
        for (auto& n : c) usesPrint |= n == "print";
    }
};

struct Emit {
    const Design& d;
    const FuncInfo& fi;
    Scenario* s;          // null inside a function body
    Lang lang;
    std::string name;     // the base name of the generated code
    Scope sc;
    std::set<std::string> locals;
    std::vector<std::string> decls;   // SV: declarations for the top of the block
    std::string retVar;               // SV task: where `return x` goes
    bool inFunction = false;
    int tmp = 0;

    bool sv() const { return lang == Lang::SV; }

    std::string typeName(TypeRef t, Loc l) const {
        t = d.resolved(t);
        i128 w = t.width ? d.constEval(*t.width) : (t.k == TypeRef::Bit ? 1 : 32);
        switch (t.k) {
        case TypeRef::Void: return "void";
        case TypeRef::Bool: return sv() ? "bit" : "bool";
        case TypeRef::String: return sv() ? "string" : "const char*";
        case TypeRef::Bit:
            if (sv()) return w == 1 ? "bit" : "bit [" + i128str(w - 1) + ":0]";
            return w <= 8 ? "uint8_t" : w <= 16 ? "uint16_t" : w <= 32 ? "uint32_t" : "uint64_t";
        case TypeRef::Int:
            if (sv()) return w == 32 ? "int" : w == 64 ? "longint" : "bit signed [" + i128str(w - 1) + ":0]";
            return w <= 8 ? "int8_t" : w <= 16 ? "int16_t" : w <= 32 ? "int32_t" : "int64_t";
        case TypeRef::Named: {
            TypeDecl* td = d.lookup(t.name, nullptr);
            if (td && td->k == TypeDecl::Enum) return "int";
            d.m.fail(l, "type '" + t.name + "' cannot be used in an exec block or a function yet");
        }
        }
        return "int";
    }

    std::string call(const Expr& e, std::vector<std::string>& pre, bool asStmt) {
        if (!e.path.empty()) d.m.fail(e.loc, "'" + e.path[0].name + "." + e.s + "()': method calls are only supported in exec init blocks");
        if (e.s == "print" || e.s == "message") {
            if (!asStmt) d.m.fail(e.loc, e.s + "() has no value");
            size_t first = e.s == "message" ? 1 : 0;
            if (e.a.size() <= first || e.a[first]->k != Expr::Str) d.m.fail(e.loc, e.s + "() needs a format string");
            std::string fmt = e.a[first]->s, out;
            std::vector<char> kinds;
            for (size_t i = 0; i < fmt.size(); i++) {
                out += fmt[i];
                if (fmt[i] != '%') continue;
                if (i + 1 < fmt.size() && fmt[i + 1] == '%') { out += fmt[++i]; continue; }
                size_t j = i + 1;
                while (j < fmt.size() && !isalpha((unsigned char)fmt[j])) out += fmt[j++];
                if (j >= fmt.size()) break;
                char c = fmt[j];
                kinds.push_back(c);
                if (!sv() && c != 's' && c != 'c') out += "ll";
                // C prints a number at its own width; SystemVerilog needs %0d for that.
                if (sv() && j == i + 1 && c != 's' && c != 'c') out += "0";
                out += c;
                i = j;
            }
            std::string r = (sv() ? "$write(" : "printf(") + cstr(out);
            for (size_t i = first + 1; i < e.a.size(); i++) {
                std::string a = expr(*e.a[i], pre);
                size_t k = i - first - 1;
                bool str = k < kinds.size() && (kinds[k] == 's' || kinds[k] == 'c');
                bool uns = k < kinds.size() && (kinds[k] == 'x' || kinds[k] == 'X' || kinds[k] == 'u' || kinds[k] == 'o');
                r += ", " + (sv() || str ? a : std::string(uns ? "(unsigned long long)(" : "(long long)(") + a + ")");
            }
            return r + ")";
        }
        auto it = d.m.funcByName.find(e.s);
        if (it == d.m.funcByName.end()) d.m.fail(e.loc, "unknown function '" + e.s + "'");
        const Function& f = *it->second;
        if (e.a.size() != f.params.size())
            d.m.fail(e.loc, "'" + e.s + "' takes " + std::to_string(f.params.size()) + " arguments");
        std::string args;
        for (auto& a : e.a) args += (args.empty() ? "" : ", ") + expr(*a, pre);
        bool task = sv() && fi.tasks.count(f.name);
        std::string callee = (sv() && !f.hasBody ? "api." : "") + f.name;
        if (!task) return callee + "(" + args + ")";
        if (inFunction && !retVar.size() && !fi.tasks.count(curFn))
            d.m.fail(e.loc, "internal: a function calls a task");
        if (f.ret.k == TypeRef::Void) {
            if (!asStmt) d.m.fail(e.loc, "'" + e.s + "' returns nothing");
            return callee + "(" + args + ")";
        }
        std::string t = "mpss_t" + std::to_string(tmp++);
        decls.push_back(typeName(f.ret, e.loc) + " " + t + ";");
        std::string c = callee + "(" + args + (args.empty() ? "" : ", ") + t + ")";
        if (asStmt) return c;
        pre.push_back(c + ";");
        return t;
    }
    std::string curFn;

    std::string expr(const Expr& e, std::vector<std::string>& pre) {
        switch (e.k) {
        case Expr::Num: return literal(e.num, lang);
        case Expr::Str: return cstr(e.s);
        case Expr::Call: return call(e, pre, false);
        case Expr::Ref: {
            if (e.scope.empty() && locals.count(e.path[0].name)) {
                std::string r;
                for (auto& p : e.path) {
                    r += (r.empty() ? "" : ".") + p.name;
                    if (p.index) r += "[" + expr(*p.index, pre) + "]";
                }
                return r;
            }
            if (!s) return literal(d.constEval(e), lang);
            Res r = s->resolveRef(e, sc);
            if (r.k == Res::Str) return cstr(r.s);
            if (r.k == Res::Const) return literal(r.v, lang);
            if (r.k == Res::VarRef) return literal(s->vars[r.var], lang);
            d.m.fail(e.loc, "'" + e.path.back().name + "' has no value here");
        }
        case Expr::Unary: return "(" + e.s + expr(*e.a[0], pre) + ")";
        case Expr::Binary: {
            if (e.s == "**" && !sv()) d.m.fail(e.loc, "** is not supported in C exec code");
            std::string a = expr(*e.a[0], pre), b = expr(*e.a[1], pre);
            return "(" + a + " " + e.s + " " + b + ")";
        }
        case Expr::Cond: {
            std::string c = expr(*e.a[0], pre), a = expr(*e.a[1], pre), b = expr(*e.a[2], pre);
            return "(" + c + " ? " + a + " : " + b + ")";
        }
        case Expr::In: {
            std::string x = expr(*e.a[0], pre), r;
            for (auto& rg : e.ranges) {
                std::string lo = expr(*rg.first, pre);
                std::string t = rg.second ? "(" + x + " >= " + lo + " && " + x + " <= " + expr(*rg.second, pre) + ")"
                                          : "(" + x + " == " + lo + ")";
                r += (r.empty() ? "" : " || ") + t;
            }
            return "(" + r + ")";
        }
        }
        return "0";
    }

    static std::string pad(int n) { return std::string((size_t)n * 2, ' '); }
    void block(const std::vector<SP>& ss, std::string& out, int ind) {
        out += sv() ? " begin\n" : " {\n";
        stmts(ss, out, ind + 1);
        out += pad(ind) + (sv() ? "end" : "}");
    }
    void stmts(const std::vector<SP>& ss, std::string& out, int ind) {
        for (auto& st : ss) stmt(*st, out, ind);
    }
    void flush(std::vector<std::string>& pre, std::string& out, int ind) {
        for (auto& p : pre) out += pad(ind) + p + "\n";
        pre.clear();
    }
    void stmt(const Stmt& st, std::string& out, int ind) {
        std::vector<std::string> pre;
        switch (st.k) {
        case Stmt::Expr_: {
            std::string x = st.e->k == Expr::Call ? call(*st.e, pre, true) : expr(*st.e, pre);
            flush(pre, out, ind);
            out += pad(ind) + x + ";\n";
            break;
        }
        case Stmt::Assign: {
            if (st.lhs->k != Expr::Ref || !locals.count(st.lhs->path[0].name))
                d.m.fail(st.loc, "an exec block can assign only to its own variables");
            std::string r = expr(*st.e, pre), l = expr(*st.lhs, pre);
            flush(pre, out, ind);
            out += pad(ind) + l + " " + st.op + " " + r + ";\n";
            break;
        }
        case Stmt::Var: {
            std::string t = typeName(st.type, st.loc);
            std::string init = st.e ? expr(*st.e, pre) : "";
            flush(pre, out, ind);
            locals.insert(st.name);
            if (sv()) {
                decls.push_back(t + " " + st.name + ";");
                if (st.e) out += pad(ind) + st.name + " = " + init + ";\n";
            } else {
                out += pad(ind) + t + " " + st.name + (st.e ? " = " + init : "") + ";\n";
            }
            break;
        }
        case Stmt::If: {
            std::string c = expr(*st.e, pre);
            flush(pre, out, ind);
            out += pad(ind) + "if (" + c + ")";
            block(st.body, out, ind);
            if (!st.els.empty()) {
                out += " else";
                block(st.els, out, ind);
            }
            out += "\n";
            break;
        }
        case Stmt::Repeat: {
            std::string n = expr(*st.e, pre);
            flush(pre, out, ind);
            std::string i = st.name.empty() ? "mpss_i" + std::to_string(tmp++) : st.name;
            bool added = locals.insert(i).second;
            out += pad(ind) + "for (int " + i + " = 0; " + i + " < " + n + "; " + i + "++)";
            block(st.body, out, ind);
            out += "\n";
            if (added) locals.erase(i);
            break;
        }
        case Stmt::While: {
            std::string c = expr(*st.e, pre);
            if (pre.empty()) {
                out += pad(ind) + "while (" + c + ")";
                block(st.body, out, ind);
                out += "\n";
            } else {
                // The condition calls a task: evaluate it at the top of each round.
                out += pad(ind) + "forever begin\n";
                flush(pre, out, ind + 1);
                out += pad(ind + 1) + "if (!(" + c + ")) break;\n";
                stmts(st.body, out, ind + 1);
                out += pad(ind) + "end\n";
            }
            break;
        }
        case Stmt::Return: {
            if (!inFunction) d.m.fail(st.loc, "return is not supported in an exec body");
            std::string v = st.e ? expr(*st.e, pre) : "";
            flush(pre, out, ind);
            if (!retVar.empty() && st.e) out += pad(ind) + retVar + " = " + v + ";\n" + pad(ind) + "return;\n";
            else out += pad(ind) + "return" + (st.e ? " " + v : "") + ";\n";
            break;
        }
        case Stmt::Block:
            out += pad(ind) + (sv() ? "begin\n" : "{\n");
            stmts(st.body, out, ind + 1);
            out += pad(ind) + (sv() ? "end\n" : "}\n");
            break;
        case Stmt::Break: out += pad(ind) + "break;\n"; break;
        case Stmt::Continue: out += pad(ind) + "continue;\n"; break;
        }
    }

    // {{expression}} in a target template -> its value.
    std::string subst(const Exec& x) {
        std::string out;
        const std::string& t = x.text;
        size_t i = 0;
        while (i < t.size()) {
            size_t a = t.find("{{", i);
            if (a == std::string::npos) break;
            size_t b = t.find("}}", a);
            if (b == std::string::npos) d.m.fail(x.loc, "'{{' without '}}' in the target code");
            out += t.substr(i, a - i);
            std::string src = t.substr(a + 2, b - a - 2);
            EP e;
            try { e = Parser(d.m, lex(src, "{{" + src + "}}"), x.loc.file).parseExpr(); }
            catch (Error& er) { d.m.fail(x.loc, "in {{" + src + "}}: " + er.what()); }
            if (e->k == Expr::Ref) {
                Res r = s->resolveRef(*e, sc);
                if (r.k == Res::Str) out += r.s;
                else if (r.k == Res::VarRef) out += i128str(s->vars[r.var].val);
                else if (r.k == Res::Const) out += i128str(r.v);
                else d.m.fail(x.loc, "{{" + src + "}} has no value here");
            } else out += i128str(s->compileConst(*e, sc));
            i = b + 2;
        }
        out += t.substr(i);
        return out;
    }
};

inline bool langIs(const std::string& l, Lang want) {
    std::string u;
    for (char c : l) u += (char)toupper((unsigned char)c);
    return want == Lang::C ? (u == "C" || u == "CPP" || u == "C_PLUS_PLUS") : (u == "SV" || u == "SYSTEMVERILOG");
}

inline std::string reindent(const std::string& text, int ind) {
    // A template keeps its own relative indentation; shift it to `ind`.
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string l;
    while (std::getline(ss, l)) lines.push_back(l);
    size_t common = std::string::npos;
    for (auto& x : lines) {
        size_t p = x.find_first_not_of(" \t");
        if (p != std::string::npos) common = std::min(common, p);
    }
    std::string out;
    for (auto& x : lines) {
        size_t p = x.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        size_t e = x.find_last_not_of(" \t\r");
        out += Emit::pad(ind) + x.substr(common, e + 1 - common) + "\n";
    }
    return out;
}

// --------------------------------------------------------------- the listing
inline std::string valueText(const Var& v) {
    if (v.isStr) return cstr(v.str);
    if (v.isBool) return v.val ? "true" : "false";
    if (v.enumT) {
        for (auto& it : v.enumT->values)
            if (it.second == v.val) return it.first;
        return i128str(v.val);
    }
    if (!v.sgn && v.width >= 32) return hexStr(v.val);
    return i128str(v.val);
}
inline std::string fieldsText(const Scenario& s, const std::map<std::string, int>& vm) {
    std::string r;
    for (auto& kv : vm) r += " " + kv.first + "=" + valueText(s.vars[kv.second]);
    return r;
}
inline std::string actionLine(const Scenario& s, int ai) {
    const AI& a = s.ais[ai];
    std::string r = a.type->qname + " @" + a.comp->path + (a.inferred ? " (inferred)" : "") + ":" + fieldsText(s, a.vars);
    for (auto& rf : a.refs) {
        if (rf.second < 0) continue;
        const Obj& o = s.objs[rf.second];
        if (o.idVar >= 0)
            r += " " + rf.first + "=" + s.d.pools[o.pool].name + "[" + i128str(s.vars[o.idVar].val) + "]";
        else r += " " + rf.first + "=#" + std::to_string(rf.second);
    }
    return r;
}
// Nodes worth showing: an unnamed sequence inside a sequence is its items.
inline void shown(const Scenario& s, int n, bool inSeq, std::vector<int>& out) {
    const Node& x = s.nodes[n];
    if (x.k != Node::Leaf && x.ai < 0 && (x.kids.size() == 1 || (x.k == Node::Seq && inSeq) || x.kids.empty())) {
        for (int k : x.kids) shown(s, k, inSeq, out);
        return;
    }
    out.push_back(n);
}
inline void listNode(const Scenario& s, int n, int ind, std::string& out) {
    const Node& x = s.nodes[n];
    std::string p((size_t)ind * 2, ' ');
    if (x.k == Node::Leaf) { out += p + actionLine(s, x.ai) + "\n"; return; }
    if (x.ai >= 0) out += p + "sequence " + actionLine(s, x.ai) + "\n";
    else out += p + (x.k == Node::Par ? "parallel\n" : "sequence\n");
    std::vector<int> kids;
    for (int k : x.kids) shown(s, k, x.k == Node::Seq, kids);
    for (int k : kids) listNode(s, k, ind + 1, out);
}
inline std::string listing(const Scenario& s) {
    std::string out = "scenario " + std::to_string(s.index) + " (seed " + std::to_string(s.seed) + ")\n";
    std::vector<int> top;
    shown(s, s.root, false, top);
    for (int n : top) listNode(s, n, 1, out);
    for (size_t o = 0; o < s.objs.size(); o++) {
        const Obj& ob = s.objs[o];
        if (ob.idVar >= 0 || ob.producer < 0) continue;
        out += "  #" + std::to_string(o) + " " + ob.type->qname + " from " + s.ais[ob.producer].name + ":" + fieldsText(s, ob.vars) + "\n";
    }
    return out;
}

// --------------------------------------------------------------------- JSON
inline std::string jsonFields(const Scenario& s, const std::map<std::string, int>& vm) {
    std::string r = "{";
    for (auto& kv : vm) {
        const Var& v = s.vars[kv.second];
        r += (r.size() > 1 ? "," : "") + cstr(kv.first) + ":" + (v.isStr ? cstr(v.str) : i128str(v.val));
    }
    return r + "}";
}
inline void jsonNode(const Scenario& s, int n, bool inSeq, std::string& out) {
    std::vector<int> ns;
    shown(s, n, inSeq, ns);
    for (int id : ns) {
        const Node& x = s.nodes[id];
        if (out.back() != '[') out += ",";
        out += "{\"kind\":";
        out += x.k == Node::Leaf ? "\"action\"" : x.k == Node::Par ? "\"parallel\"" : "\"sequence\"";
        if (x.ai >= 0) {
            const AI& a = s.ais[x.ai];
            out += ",\"action\":" + cstr(a.type->qname) + ",\"name\":" + cstr(a.name) + ",\"component\":" + cstr(a.comp->path);
            if (a.inferred) out += ",\"inferred\":true";
            out += ",\"fields\":" + jsonFields(s, a.vars) + ",\"refs\":{";
            bool first = true;
            for (auto& rf : a.refs) {
                if (rf.second < 0) continue;
                out += (first ? "" : ",") + cstr(rf.first) + ":" + std::to_string(rf.second);
                first = false;
            }
            out += "}";
        }
        if (x.k != Node::Leaf) {
            out += ",\"children\":[";
            for (int k : x.kids) jsonNode(s, k, x.k == Node::Seq, out);
            out += "]";
        }
        out += "}";
    }
}
inline std::string json(const Scenario& s) {
    std::string out = "{\"index\":" + std::to_string(s.index) + ",\"seed\":" + std::to_string(s.seed) + ",\"tree\":[";
    jsonNode(s, s.root, false, out);
    out += "],\"objects\":[";
    for (size_t o = 0; o < s.objs.size(); o++) {
        const Obj& ob = s.objs[o];
        out += (o ? "," : "") + std::string("{\"id\":") + std::to_string(o) + ",\"type\":" + cstr(ob.type->qname) +
               ",\"pool\":" + cstr(s.d.pools[ob.pool].owner->path + "." + s.d.pools[ob.pool].name);
        if (ob.idVar >= 0) out += ",\"instance_id\":" + i128str(s.vars[ob.idVar].val);
        out += ",\"fields\":" + jsonFields(s, ob.vars) + "}";
    }
    return out + "]}";
}

// --------------------------------------------------------------------- code
inline void codeNode(Scenario& s, const FuncInfo& fi, Lang lang, const std::string& name, int n, int ind, std::string& out) {
    const Node& x = s.nodes[n];
    std::string p = Emit::pad(ind);
    bool sv = lang == Lang::SV;
    auto note = [&](const std::string& t) { return sv ? "// " + t + "\n" : "/* " + t + " */\n"; };
    if (x.k == Node::Leaf) {
        const AI& a = s.ais[x.ai];
        out += p + note(a.name + ": " + actionLine(s, x.ai));
        Emit em{s.d, fi, &s, lang, name, {}, {}, {}, {}, false, 0, {}};
        em.sc.ai = x.ai;
        std::string body;
        for (auto& ex : a.type->execs) {
            if (ex.kind != "body") continue;
            if (ex.lang.empty()) em.stmts(ex.stmts, body, ind + 1);
            else if (langIs(ex.lang, lang)) body += reindent(em.subst(ex), ind + 1);
        }
        if (sv) {
            out += p + "begin\n";
            for (auto& dcl : em.decls) out += Emit::pad(ind + 1) + dcl + "\n";
            out += Emit::pad(ind + 1) + "`uvm_info(\"MPSS\", " + cstr(a.name + ": " + a.type->qname) + ", UVM_HIGH)\n";
            out += body + p + "end\n";
        } else if (!body.empty()) {
            out += p + "{\n" + body + p + "}\n";
        }
        return;
    }
    std::vector<int> kids;
    for (int k : x.kids) shown(s, k, x.k == Node::Seq, kids);
    if (x.ai >= 0) out += p + note("begin " + s.ais[x.ai].name + ": " + actionLine(s, x.ai));
    if (x.k == Node::Par && kids.size() > 1) {
        if (sv) {
            out += p + "fork\n";
            for (int k : kids) {
                out += Emit::pad(ind + 1) + "begin\n";
                codeNode(s, fi, lang, name, k, ind + 2, out);
                out += Emit::pad(ind + 1) + "end\n";
            }
            out += p + "join\n";
        } else {
            out += p + note("parallel: the branches run one after the other here");
            for (int k : kids) codeNode(s, fi, lang, name, k, ind, out);
            out += p + note("end parallel");
        }
    } else {
        for (int k : kids) codeNode(s, fi, lang, name, k, ind, out);
    }
    if (x.ai >= 0) out += p + note("end " + s.ais[x.ai].name);
}

inline std::string scenarioName(const std::string& base, int index) { return base + "_" + std::to_string(index); }

inline std::string cScenario(Scenario& s, const FuncInfo& fi, const std::string& base) {
    std::string out = "\n/* scenario " + std::to_string(s.index) + ", seed " + std::to_string(s.seed) + " */\n";
    out += "void " + scenarioName(base, s.index) + "(void)\n{\n";
    std::vector<int> top;
    shown(s, s.root, false, top);
    for (int n : top) codeNode(s, fi, Lang::C, base, n, 1, out);
    return out + "}\n";
}
inline std::string svScenario(Scenario& s, const FuncInfo& fi, const std::string& base) {
    std::string cn = scenarioName(base, s.index) + "_seq";
    std::string out = "\n  // scenario " + std::to_string(s.index) + ", seed " + std::to_string(s.seed) + "\n";
    out += "  class " + cn + " extends " + base + "_base_seq;\n";
    out += "    `uvm_object_utils(" + cn + ")\n";
    out += "    function new(string name = " + cstr(cn) + ");\n      super.new(name);\n    endfunction\n";
    out += "    virtual task body();\n      mpss_init();\n";
    std::vector<int> top;
    shown(s, s.root, false, top);
    for (int n : top) codeNode(s, fi, Lang::SV, base, n, 3, out);
    out += "    endtask\n  endclass\n";
    return out;
}

inline std::string templates(const Design& d, const char* kind, Lang lang, int ind) {
    std::string out;
    for (TypeDecl* t : d.m.all)
        for (auto& x : t->execs)
            if (x.kind == kind && !x.lang.empty() && langIs(x.lang, lang)) out += reindent(x.text, ind);
    return out;
}

inline std::string params(Emit& em, const Function& f, bool svTask) {
    std::string r;
    for (auto& p : f.params)
        r += (r.empty() ? "" : ", ") + std::string(em.sv() ? "input " : "") + em.typeName(p.type, f.loc) + " " + p.name;
    if (svTask && f.ret.k != TypeRef::Void)
        r += (r.empty() ? "" : ", ") + std::string("output ") + em.typeName(f.ret, f.loc) + " mpss_ret";
    if (r.empty() && !em.sv()) r = "void";
    return r;
}

inline std::string cPrelude(const Design& d, const FuncInfo& fi, const std::string& base, const std::string& banner) {
    std::string out = "/* " + banner + " */\n#include <stdbool.h>\n#include <stdint.h>\n";
    if (fi.usesPrint) out += "#include <stdio.h>\n";
    out += "#if defined(__GNUC__)\n#define MPSS_UNUSED __attribute__((unused))\n#else\n#define MPSS_UNUSED\n#endif\n";
    out += templates(d, "header", Lang::C, 0);
    Emit em{d, fi, nullptr, Lang::C, base, {}, {}, {}, {}, true, 0, {}};
    bool any = false;
    for (auto& f : d.m.funcs) {
        if (!any) out += "\n/* The functions the model calls: the test platform provides the ones marked extern. */\n";
        any = true;
        out += std::string(f.hasBody ? "MPSS_UNUSED static " : "extern ") + em.typeName(f.ret, f.loc) + " " + f.name + "(" + params(em, f, false) + ");\n";
    }
    for (auto& f : d.m.funcs) {
        if (!f.hasBody) continue;
        Emit fe{d, fi, nullptr, Lang::C, base, {}, {}, {}, {}, true, 0, f.name};
        for (auto& p : f.params) fe.locals.insert(p.name);
        std::string body;
        fe.stmts(f.body, body, 1);
        out += "\nstatic " + fe.typeName(f.ret, f.loc) + " " + f.name + "(" + params(fe, f, false) + ")\n{\n" + body + "}\n";
    }
    out += templates(d, "declaration", Lang::C, 0);
    return out;
}
inline std::string cPostlude(const std::string& base, int n) {
    std::string out = "\n/* Every scenario, for a loop over them. */\nvoid (*const " + base + "_tests[])(void) = {";
    for (int i = 0; i < n; i++) out += (i ? ", " : "") + scenarioName(base, i);
    out += "};\nconst unsigned " + base + "_count = " + std::to_string(n) + ";\n";
    out += "\n#ifdef MPSS_MAIN\nint main(void)\n{\n  for (unsigned i = 0; i < " + base + "_count; i++) " + base + "_tests[i]();\n  return 0;\n}\n#endif\n";
    return out;
}

inline std::string svPrelude(const Design& d, const FuncInfo& fi, const std::string& base, const std::string& banner) {
    std::string out = "// " + banner + "\n`include \"uvm_macros.svh\"\npackage " + base + "_pkg;\n  import uvm_pkg::*;\n";
    out += templates(d, "header", Lang::SV, 1);
    Emit em{d, fi, nullptr, Lang::SV, base, {}, {}, {}, {}, true, 0, {}};
    bool imports = false;
    for (auto& f : d.m.funcs) imports |= !f.hasBody;
    if (imports) {
        out += "\n  // The functions the model imports. The testbench extends this class, drives\n"
               "  // its agents in each task, and hands an object to the sequences:\n"
               "  //   uvm_config_db#(" + base + "_api)::set(null, \"*\", \"mpss_api\", my_api);\n";
        out += "  virtual class " + base + "_api extends uvm_object;\n";
        out += "    function new(string name = " + cstr(base + "_api") + ");\n      super.new(name);\n    endfunction\n";
        for (auto& f : d.m.funcs)
            if (!f.hasBody) out += "    pure virtual task " + f.name + "(" + params(em, f, true) + ");\n";
        out += "  endclass\n";
    }
    out += "\n  // What every scenario shares: the API handle and the model's own functions.\n";
    out += "  class " + base + "_base_seq extends uvm_sequence #(uvm_sequence_item);\n";
    if (imports) out += "    " + base + "_api api;\n";
    out += "    function new(string name = " + cstr(base + "_base_seq") + ");\n      super.new(name);\n    endfunction\n";
    out += "    virtual task mpss_init();\n";
    if (imports)
        out += "      if (api == null && !uvm_config_db#(" + base + "_api)::get(null, \"\", \"mpss_api\", api))\n"
               "        `uvm_fatal(\"MPSS\", \"no " + base + "_api object: set \\\"mpss_api\\\" in uvm_config_db or assign seq.api\")\n";
    out += "    endtask\n";
    for (auto& f : d.m.funcs) {
        if (!f.hasBody) continue;
        bool task = fi.tasks.count(f.name);
        Emit fe{d, fi, nullptr, Lang::SV, base, {}, {}, {}, {}, true, 0, f.name};
        if (task && f.ret.k != TypeRef::Void) fe.retVar = "mpss_ret";
        for (auto& p : f.params) fe.locals.insert(p.name);
        std::string body;
        fe.stmts(f.body, body, 3);
        if (task) out += "    virtual task " + f.name + "(" + params(fe, f, true) + ");\n";
        else out += "    virtual function " + fe.typeName(f.ret, f.loc) + " " + f.name + "(" + params(fe, f, false) + ");\n";
        for (auto& dcl : fe.decls) out += "      " + dcl + "\n";
        out += body + (task ? "    endtask\n" : "    endfunction\n");
    }
    out += templates(d, "declaration", Lang::SV, 2);
    out += "  endclass\n";
    return out;
}
inline std::string svPostlude(const std::string& base) { return "endpackage : " + base + "_pkg\n"; }

} // namespace pss
