// msim -i: interactive simulation control, the UCLI of msim (included by msim.cpp).
//
// Commands come from the standard input, or from -do FILE, and run on the
// tool line's Tcl interpreter (tcl.hpp): set, if, foreach, proc, expr,
// source and [command] substitution work around the commands below, as in
// VCS UCLI and the xrun Tcl shell. docs/DEBUG.md lists them all:
//   run [T [unit]] [-absolute] | run -posedge/-negedge/-change SIG | run -slot [N] | run -all
//   step, next                 one statement (any process / this process)
//   stop ...                   on a signal, a value, a line, a time or a condition (UCLI and xrun forms)
//   get, print/value, force, release, change/deposit, scope, show, search, signals, describe,
//   where/stack, listing, dump/probe, drivers/loads, call, save/restore/restart, senv, alias,
//   time, help, quit/exit/finish
// The kernel is driven one time slot at a time through Sim::run(end), so a
// normal run pays nothing for this mode. A stop at a line or a step pauses
// inside the statement hook (prog.hpp g_stmtHook) and reads commands there;
// the run goes on when one of them is run, step or next. Maintained by the
// debug tool thread.

namespace interactive {

struct Stop {
    int id = 0;
    int kind = 0;   // 0 change, 1 posedge, 2 negedge, 3 == value, 4 line, 5 time, 6 condition
    int sig = -1;
    std::string name;
    Val want, last;
    std::vector<uint32_t> files;
    uint32_t line = 0;
    uint64_t time = 0;
    std::string cond, cmd;
    bool once = false, cont = false, on = true, condWas = false;
};

struct Session;
inline Session* g_ses = nullptr;
inline void onStmt(const Instr& i);

struct Session {
    slang::ast::Compilation* comp = nullptr;
    const LowerResult* lr = nullptr;
    uint64_t maxTime = ~0ULL;
    bool started = false, prompt = false;
    std::unordered_map<std::string, int> byName;
    std::vector<Stop> stops;
    int nextId = 1;
    std::string scope;   // the current scope below the top ("" is the top itself)
    tcl::Interp tcl;
    std::istream* in = nullptr;
    // What the run in progress is waiting for. Commands given while paused
    // at a statement change them and the same run goes on.
    uint64_t limit = ~0ULL;
    long slots = -1;
    int stepMode = 0;   // 1 step: the next statement, 2 next: the next one of this process
    const Prog* stepProg = nullptr;
    std::unique_ptr<Stop> until;   // run -posedge SIG and friends
    std::string why;
    int nest = 0;
    bool resume = false, eof = false, ended = false, quitNow = false;
    const Prog* curProg = nullptr;
    // Every process and subroutine, for the statement hook: code ranges.
    struct Range { const Instr* b; const Instr* e; const Prog* p; };
    std::vector<Range> ranges;
    std::unordered_map<const Prog*, const LoweredProc*> procOf;
    bool hooked = false, allMarked = false;
    void (*prevHook)(const Instr&) = nullptr;
    std::map<uint32_t, std::vector<std::string>> srcCache;
    // save / restore: every command read so far, replayed by a restore.
    std::vector<std::string> history;
    std::string doPath;                     // -do FILE (msim.cpp), for a restore to go on reading it
    std::unique_ptr<std::istream> own;      // the input after a restore, or the replay
    std::istream* after = nullptr;          // the input that follows the replay
    bool replaying = false;
    std::string replayState;                // the checkpoint's digest
    std::vector<std::unique_ptr<Prog>> calls;   // the wrapper processes of call

    // ---- names ----
    void names() {
        if (!byName.empty()) return;
        for (size_t i = 0; i < S.sigs.size(); i++) {
            const std::string& nm = S.sigs[i].name;
            if (!nm.empty() && nm[0] != '$') byName.emplace(nm, (int)i);
        }
    }
    int findAbs(const std::string& n) {
        names();
        auto it = byName.find(n);
        if (it != byName.end()) return it->second;
        // The simulation top's own name in front is fine: tb.u.q for u.q.
        const std::string pre = S.vcdTop + ".";
        if (n.rfind(pre, 0) == 0)
            if (it = byName.find(n.substr(pre.size())); it != byName.end()) return it->second;
        return -1;
    }
    // A name in the current scope first, then from the top.
    int find(const std::string& n) {
        if (!scope.empty())
            if (int s = findAbs(scope + "." + n); s >= 0) return s;
        return findAbs(n);
    }
    std::string shown(int s) const { return S.vcdTop + "." + S.sigs[s].name; }
    std::string scopeShown() const { return scope.empty() ? S.vcdTop : S.vcdTop + "." + scope; }

    static std::string fmt(const Val& v) {
        if (v.real) { double d; std::memcpy(&d, &v.v, sizeof d); std::ostringstream o; o << d; return o.str(); }
        if (v.str) return "\"" + Sim::fmtVal('s', v, 0, false) + "\"";
        bool unk = v.x != 0;
        std::string h = Sim::fmtVal(unk ? 'b' : 'h', v, 0, true);
        return std::to_string(v.w) + (unk ? "'b" : "'h") + h;
    }
    static std::string radix(const Val& v, const std::string& r) {
        if (r.empty() || v.real || v.str) return fmt(v);
        char c = r == "bin" || r == "binary" || r == "b" ? 'b' : r == "oct" || r == "octal" || r == "o" ? 'o'
               : r == "dec" || r == "decimal" || r == "d" || r == "unsigned" ? 'd' : r == "sdec" || r == "signed" ? 's'
               : r == "hex" || r == "hexadecimal" || r == "h" ? 'h' : 0;
        if (!c) throw tcl::Error("-radix bin, oct, dec, sdec or hex");
        if (c == 's') {
            if (v.x || v.w > 64) return Sim::fmtVal('d', v, 0, true);
            int64_t n = (int64_t)v.v;
            if (v.w < 64 && (v.v >> (v.w - 1)) & 1) n = (int64_t)(v.v | (~0ull << v.w));
            return std::to_string(n);
        }
        return Sim::fmtVal(c, v, 0, c == 'd');
    }

    // 12, 8'hff, 'b1x0z, 4'd9: a value of the signal's width (up to 64 bits).
    static bool parse(const std::string& t, int w, Val& out) {
        if (w > 64) return false;
        size_t q = t.find('\'');
        int base = 10;
        std::string digits = t;
        if (q != std::string::npos) {
            size_t b = q + 1;
            if (b < t.size() && (t[b] == 's' || t[b] == 'S')) b++;
            if (b >= t.size()) return false;
            char c = (char)std::tolower((unsigned char)t[b]);
            base = c == 'h' ? 16 : c == 'b' ? 2 : c == 'o' ? 8 : c == 'd' ? 10 : 0;
            if (!base) return false;
            digits = t.substr(b + 1);
        }
        if (digits.empty()) return false;
        uint64_t v = 0, x = 0, z = 0;
        int sh = base == 16 ? 4 : base == 8 ? 3 : 1;
        for (char ch : digits) {
            if (ch == '_') continue;
            char c = (char)std::tolower((unsigned char)ch);
            if (base == 10) {
                if (c < '0' || c > '9') return false;
                v = v * 10 + (uint64_t)(c - '0');
                continue;
            }
            uint64_t m = (1ull << sh) - 1;
            v <<= sh; x <<= sh; z <<= sh;
            if (c == 'x') x |= m;
            else if (c == 'z' || c == '?') { x |= m; z |= m; }
            else {
                int d = c >= 'a' ? c - 'a' + 10 : c - '0';
                if (d < 0 || d >= base) return false;
                v |= (uint64_t)d;
            }
        }
        uint64_t mk = w >= 64 ? ~0ull : (1ull << w) - 1;
        out = Val{};
        out.w = w;
        out.v = v & mk & ~x; out.x = x & mk; out.z = z & mk;
        return true;
    }

    // 100, 100ns, "100 ns": time units of the simulation (ticks).
    static uint64_t ticks(const std::vector<std::string>& w, size_t& i) {
        if (i >= w.size()) throw tcl::Error("a time is missing");
        char* end = nullptr;
        double n = std::strtod(w[i].c_str(), &end);
        if (end == w[i].c_str()) throw tcl::Error("'" + w[i] + "' is not a time");
        static const std::map<std::string, int> ex = {{"s", 0}, {"ms", -3}, {"us", -6}, {"ns", -9}, {"ps", -12}, {"fs", -15}};
        std::string unit = end && *end ? std::string(end) : "";
        if (unit.empty() && i + 1 < w.size() && ex.count(w[i + 1])) unit = w[++i];
        auto u = ex.find(unit);
        if (!unit.empty() && u == ex.end()) throw tcl::Error("unknown time unit '" + unit + "'");
        double t = u == ex.end() ? n : n * std::pow(10.0, u->second - timeFmt().tickExp);
        return (uint64_t)std::llround(t);
    }

    // ---- the design's scopes (slang) ----
    const slang::ast::Scope* topScope() const {
        if (!comp) return nullptr;
        for (auto* t : comp->getRoot().topInstances)
            if (t->name == S.vcdTop) return &t->body;
        auto tops = comp->getRoot().topInstances;
        return tops.empty() ? nullptr : &tops[0]->body;
    }
    static std::vector<std::string> split(const std::string& p) {
        std::vector<std::string> parts;
        std::istringstream is(p);
        for (std::string s; std::getline(is, s, '.');) if (!s.empty()) parts.push_back(s);
        return parts;
    }
    // A member of a scope by name, gen[1] being the block of a generate array.
    static const slang::ast::Symbol* member(const slang::ast::Scope& sc, const std::string& part) {
        if (const slang::ast::Symbol* m = sc.find(part)) return m;
        size_t b = part.find('[');
        if (b == std::string::npos) return nullptr;
        const slang::ast::Symbol* m = sc.find(part.substr(0, b));
        if (!m || m->kind != slang::ast::SymbolKind::GenerateBlockArray) return nullptr;
        int idx = std::atoi(part.c_str() + b + 1);
        for (auto* e : m->as<slang::ast::GenerateBlockArraySymbol>().entries)
            if (e->arrayIndex && e->arrayIndex->as<int>() == idx) return e;
        return nullptr;
    }
    // A scope path below the top (u1.g[0]) -> the slang scope, or null.
    const slang::ast::Scope* scopeAt(const std::string& rel) const {
        const slang::ast::Scope* sc = topScope();
        for (auto& part : split(rel)) {
            if (!sc) return nullptr;
            const slang::ast::Symbol* m = member(*sc, part);
            if (!m) return nullptr;
            if (m->kind == slang::ast::SymbolKind::Instance) sc = &m->as<slang::ast::InstanceSymbol>().body;
            else if (m->kind == slang::ast::SymbolKind::GenerateBlock) sc = &m->as<slang::ast::GenerateBlockSymbol>();
            else return nullptr;
        }
        return sc;
    }
    // A name, absolute or in the current scope, -> the scope path below the top, or nullopt.
    std::optional<std::string> scopeName(const std::string& n) const {
        std::string a = n;
        if (a.rfind(S.vcdTop + ".", 0) == 0) a = a.substr(S.vcdTop.size() + 1);
        else if (a == S.vcdTop) a.clear();
        if (!scope.empty() && scopeAt(scope + "." + a)) return scope + "." + a;
        if (scopeAt(a)) return a;
        return std::nullopt;
    }
    std::string where(slang::SourceLocation loc) const {
        const slang::SourceManager* sm = comp ? comp->getSourceManager() : nullptr;
        if (!sm || !loc.valid()) return "";
        auto ol = sm->getFullyOriginalLoc(loc);
        return std::string(std::filesystem::path(sm->getFileName(ol)).filename().string()) + ":" + std::to_string(sm->getLineNumber(ol));
    }

    // ---- class objects ----
    const slang::ast::Symbol* resolve(const std::vector<std::string>& parts, size_t& used) {
        const slang::ast::Scope* sc = nullptr;
        for (auto* t : comp->getRoot().topInstances)
            if (t->name == parts[0]) sc = &t->body;
        const slang::ast::Symbol* sym = nullptr;
        for (used = 1; sc && used < parts.size(); used++) {
            const slang::ast::Symbol* m = member(*sc, parts[used]);
            if (!m) return nullptr;
            sym = m;
            if (m->kind == slang::ast::SymbolKind::Instance) { sc = &m->as<slang::ast::InstanceSymbol>().body; sym = nullptr; }
            else if (m->isScope() && m->kind != slang::ast::SymbolKind::Variable) sc = &m->as<slang::ast::Scope>();
            else { used++; break; }
        }
        return sym;
    }
    static const ClassInfo* classOf(int cid) {
        for (auto& c : classStore()) if (c->id == cid) return c.get();
        return nullptr;
    }
    // The properties of an object, base class first: name -> (signal, 0 value / 1 handle / 2 string).
    static std::string propVal(int sig, int what) {
        Val v = S.get(sig);
        if (what == 2) v.str = true;
        return fmt(v);
    }
    void props(const ClassInfo* ci, int obj, std::vector<std::tuple<std::string, int, int>>& out) {
        if (!ci || !ci->ty) return;
        props(ci->base, obj, out);
        for (auto& p : ci->ty->membersOfType<slang::ast::ClassPropertySymbol>()) {
            auto it = propMap().find(&p);
            if (it == propMap().end()) continue;
            const auto& ty = p.getType().getCanonicalType();
            int what = ty.isClass() ? 1 : ty.isString() ? 2 : 0;
            out.emplace_back(std::string(p.name), it->second.kind == 0 ? obj + it->second.off : -1, what);
        }
    }
    static bool isNull(const Val& h) { return h.x != 0 || h.v == 0 || h.v >= S.sigs.size(); }
    void printObject(const std::string& name, const Val& h) {
        if (isNull(h)) { std::cout << name << " = null\n"; return; }
        int obj = (int)h.v;
        const ClassInfo* ci = classOf((int)S.sigs[obj].val.v);
        if (!ci) { std::cout << name << " = <object " << obj << ">\n"; return; }
        std::vector<std::tuple<std::string, int, int>> ps;
        props(ci, obj, ps);
        std::cout << name << " = " << classRt()[(size_t)ci->id].name << " @" << obj << " {";
        bool first = true;
        for (auto& [pn, sig, handle] : ps) {
            std::cout << (first ? " " : ", ") << pn << " = ";
            first = false;
            if (sig < 0) std::cout << "<array>";
            else if (handle == 1) {
                const Val v = S.get(sig);
                if (isNull(v)) std::cout << "null";
                else { const ClassInfo* k = classOf((int)S.sigs[(int)v.v].val.v); std::cout << (k ? classRt()[(size_t)k->id].name : "object") << " @" << v.v; }
            } else std::cout << propVal(sig, handle);
        }
        std::cout << " }\n";
    }
    // tb.h, tb.h.next, tb.h.next.id: a handle variable, then properties.
    bool printPath(const std::string& name0) {
        std::string name = name0;
        if (!comp) return false;
        if (name.rfind(S.vcdTop + ".", 0) != 0) {   // relative to the scope
            std::string a = S.vcdTop + "." + (scope.empty() ? "" : scope + ".") + name;
            std::vector<std::string> p = split(a);
            size_t u = 0;
            if (resolve(p, u)) name = a;
            else name = S.vcdTop + "." + name;
        }
        std::vector<std::string> parts = split(name);
        if (parts.size() < 2) return false;
        size_t used = 0;
        const slang::ast::Symbol* sym = resolve(parts, used);
        if (!sym || sym->kind != slang::ast::SymbolKind::Variable) return false;
        if (!sym->as<slang::ast::VariableSymbol>().getType().getCanonicalType().isClass()) return false;
        std::string base = parts[0];
        for (size_t i = 1; i < used; i++) base += "." + parts[i];
        int s = findAbs(base);
        if (s < 0) return false;
        Val h = S.get(s);
        for (size_t i = used; i < parts.size(); i++) {
            if (isNull(h)) { std::cout << name << ": " << base << " is null\n"; return true; }
            std::vector<std::tuple<std::string, int, int>> ps;
            props(classOf((int)S.sigs[(int)h.v].val.v), (int)h.v, ps);
            bool found = false;
            for (auto& [pn, sig, handle] : ps)
                if (pn == parts[i]) {
                    found = true;
                    base += "." + pn;
                    if (sig < 0) { std::cout << base << " = <array>\n"; return true; }
                    if (handle != 1) {
                        if (i + 1 != parts.size()) return false;
                        std::cout << base << " = " << propVal(sig, handle) << "\n";
                        return true;
                    }
                    h = S.get(sig);
                }
            if (!found) { std::cout << "no property '" << parts[i] << "' in " << base << "\n"; return true; }
        }
        printObject(base, h);
        return true;
    }

    // ---- source lines ----
    const std::vector<std::string>& lines(uint32_t f) {
        auto it = srcCache.find(f);
        if (it != srcCache.end()) return it->second;
        std::vector<std::string>& v = srcCache[f];
        if (f < g_covFiles.size()) {
            std::ifstream is(g_covFiles[f]);
            for (std::string l; std::getline(is, l);) v.push_back(l);
        }
        return v;
    }
    static std::string fileName(uint32_t f) {
        return f < g_covFiles.size() ? std::filesystem::path(g_covFiles[f]).filename().string() : "?";
    }
    std::string here(uint32_t f, uint32_t l) {
        std::string s = fileName(f) + ":" + std::to_string(l);
        const auto& v = lines(f);
        if (l >= 1 && l <= v.size()) {
            std::string t = v[l - 1];
            size_t a = t.find_first_not_of(" \t");
            s += "  " + (a == std::string::npos ? std::string() : t.substr(a));
        }
        return s;
    }
    // Source files a name means: the path, or a file name ending one.
    std::vector<uint32_t> filesNamed(const std::string& n) const {
        std::vector<uint32_t> out;
        for (uint32_t f = 1; f < g_covFiles.size(); f++) {
            const std::string& p = g_covFiles[f];
            if (p == n || (p.size() > n.size() && p.compare(p.size() - n.size(), n.size(), n) == 0 && p[p.size() - n.size() - 1] == '/'))
                out.push_back(f);
        }
        return out;
    }

    // ---- statements: the hook ----
    void index() {
        if (!ranges.empty() || !lr) return;
        std::unordered_set<const Prog*> seen;
        std::function<void(const Prog*)> walk = [&](const Prog* p) {
            if (!p || !seen.insert(p).second) return;
            if (!p->code.empty()) ranges.push_back({p->code.data(), p->code.data() + p->code.size(), p});
            for (auto& i : p->code) {
                if (i.callee) walk(i.callee);
                for (auto* b : i.branches) walk(b);
            }
        };
        for (auto& p : lr->procs) { walk(&p.prog); procOf[&p.prog] = &p; }
        for (auto& p : lr->subs) walk(p.get());
    }
    const Prog* progOf(const Instr* i) const {
        for (auto& r : ranges) if (i >= r.b && i < r.e) return r.p;
        return nullptr;
    }
    void hook() {
        index();
        if (!hooked) { prevHook = g_stmtHook; g_stmtHook = onStmt; g_hook = true; hooked = true; }
    }
    // Marks the first instruction of each statement on `line` (every line when line is 0).
    bool mark(const std::vector<uint32_t>& files, uint32_t line) {
        hook();
        bool any = false;
        for (auto& r : ranges)
            for (const Instr* i = r.b; i < r.e; i++) {
                if (!i->line || (line && i->line != line)) continue;
                if (line && std::find(files.begin(), files.end(), i->file) == files.end()) continue;
                if (i == r.b || (i - 1)->file != i->file || (i - 1)->line != i->line) i->stmtCb = true;
                any = true;
            }
        return any;
    }
    void stepAll() {
        if (!allMarked) { mark({}, 0); allMarked = true; }
    }

    void stmt(const Instr& i) {
        if (nest || eof || S.finished) return;
        const Prog* p = progOf(&i);
        std::string hit;
        std::vector<std::string> cmds;
        if (stepMode == 1 || (stepMode == 2 && (!stepProg || p == stepProg))) hit = stepMode == 1 ? "step" : "next";
        for (size_t k = 0; k < stops.size(); k++) {
            Stop& st = stops[k];
            if (!st.on || st.kind != 4 || st.line != i.line) continue;
            if (std::find(st.files.begin(), st.files.end(), i.file) == st.files.end()) continue;
            if (!st.cond.empty() && !cond(st.cond)) continue;
            std::string text = "stop " + std::to_string(st.id);
            if (!st.cmd.empty()) cmds.push_back(st.cmd);
            bool cont = st.cont;
            if (st.once) { stops.erase(stops.begin() + (long)k); k--; }
            if (cont) { std::cout << "time " << S.now << ": " << text << ": " << here(i.file, i.line) << "\n"; continue; }
            if (hit.empty() || hit == "step" || hit == "next") hit = text;
        }
        if (!hit.empty()) std::cout << "time " << S.now << ": " << hit << ": " << here(i.file, i.line) << "\n";
        for (auto& c : cmds) script(c);
        if (hit.empty()) return;
        stepMode = 0;
        pause(p);
    }
    // Paused at a statement: commands until one goes on.
    void pause(const Prog* p) {
        curProg = p;
        nest++;
        resume = false;
        std::string buf;
        while (!resume && !S.finished && !quitNow) {
            if (!read(buf)) { rest(); break; }
            run(buf);
        }
        nest--;
        resume = false;
    }
    // The commands are over: the rest of the run, without stops.
    void rest() {
        eof = true;
        stops.clear();
        until.reset();
        stepMode = 0;
        limit = ~0ULL;
        slots = -1;
    }

    // A condition in Verilog or Tcl form: tb.cnt == 3 && en. Signals become
    // their values; one with x or z bits makes the condition false.
    bool cond(const std::string& c) {
        std::string out;
        bool unknown = false;   // still evaluated, so that a bad condition is an error
        for (size_t i = 0; i < c.size();) {
            char ch = c[i];
            if (std::isalpha((unsigned char)ch) || ch == '_') {
                size_t j = i;
                while (j < c.size() && (std::isalnum((unsigned char)c[j]) || c[j] == '_' || c[j] == '.' || c[j] == '$')) j++;
                if (j < c.size() && c[j] == '[') { size_t e = c.find(']', j); if (e != std::string::npos) j = e + 1; }
                std::string n = c.substr(i, j - i);
                int s = find(n);
                if (s < 0) throw tcl::Error("no signal '" + n + "' in the condition");
                Val v = S.get(s);
                if (v.x || v.real || v.str) { unknown = true; out += "0"; }
                else out += Sim::fmtVal('d', v, 0, true);
                i = j;
            } else if (std::isdigit((unsigned char)ch) || ch == '\'') {
                size_t j = i;
                while (j < c.size() && (std::isalnum((unsigned char)c[j]) || c[j] == '\'' || c[j] == '_')) j++;
                std::string lit = c.substr(i, j - i);
                Val v;
                if (lit.find('\'') != std::string::npos) {
                    if (!parse(lit, 64, v)) throw tcl::Error("bad number '" + lit + "' in the condition");
                    if (v.x) unknown = true;
                    out += std::to_string(v.v);
                } else out += lit;
                i = j;
            } else { out += ch; i++; }
        }
        size_t before = tcl.diags.size();
        std::string r = tcl.parse("expr {" + out + "}", "<condition>");
        if (tcl.diags.size() > before) {
            std::string e = tcl.diags.back().text;
            tcl.diags.resize(before);
            throw tcl::Error("bad condition '" + c + "': " + e);
        }
        return !unknown && r != "0" && !r.empty();
    }
    void script(const std::string& s) { run(s); }

    // ---- running ----
    static std::string stopText(const Stop& st) {
        switch (st.kind) {
            case 1: return st.name + " posedge";
            case 2: return st.name + " negedge";
            case 3: return st.name + " == " + fmt(st.want);
            case 4: return "line " + fileName(st.files.empty() ? 0 : st.files[0]) + ":" + std::to_string(st.line);
            case 5: return "time " + std::to_string(st.time);
            case 6: return "condition {" + st.cond + "}";
            default: return st.name + " (any change)";
        }
    }
    bool sigHit(Stop& st) {
        Val cur = S.get(st.sig);
        bool hit = false;
        if (!(cur == st.last)) {
            int o = Sim::bit0(st.last), n = Sim::bit0(cur);
            hit = st.kind == 0 || (st.kind == 1 && n == 1 && o != 1) || (st.kind == 2 && n == 0 && o != 0) || (st.kind == 3 && cur == st.want);
        }
        st.last = cur;
        return hit && (st.cond.empty() || cond(st.cond));
    }
    // One time slot at a time, checking the stops after each. Returns false
    // when the simulation is over; otherwise `why` says where it stopped.
    bool advance() {
        why.clear();
        for (Stop& st : stops) if (st.sig >= 0) st.last = S.get(st.sig);
        for (Stop& st : stops) if (st.kind == 6) st.condWas = cond(st.cond);
        if (until) until->last = S.get(until->sig);
        while (!S.finished) {
            if (!started) { started = true; S.run(S.now, true); }
            else {
                uint64_t lim = limit;
                const Stop* at = nullptr;
                for (const Stop& st : stops)
                    if (st.on && st.kind == 5 && st.time > S.now && st.time <= lim) { lim = st.time; at = &st; }
                bool empty = S.wheel.empty();
                uint64_t t = empty ? ~0ULL : S.wheel.begin()->first;
                if ((empty || t > maxTime) && !at) return false;
                if (t > lim) {
                    if (lim > S.now) { S.now = lim; g_simTime = lim; }
                    if (at) {
                        int id = at->id;
                        std::string cmd = at->cmd;
                        bool cont = at->cont;
                        if (at->once) std::erase_if(stops, [&](const Stop& s) { return s.id == id; });
                        else for (Stop& s : stops) if (s.id == id) s.on = false;   // a time passes once
                        std::string text = "stop " + std::to_string(id) + ": time " + std::to_string(S.now);
                        if (cont || !cmd.empty()) {
                            std::cout << "time " << S.now << ": " << text << "\n";
                            if (!cmd.empty()) script(cmd);
                            if (cont) continue;
                            why = "-";
                        } else why = text;
                        return true;
                    }
                    why = "ran to time " + std::to_string(lim) + (empty ? std::string(" (no more events)") : " (next event at " + std::to_string(t) + ")");
                    limit = ~0ULL;
                    return true;
                }
                S.run(t, false);
            }
            if (S.finished || quitNow) return false;
            if (until && sigHit(*until)) { why = until->name + " = " + fmt(S.get(until->sig)); until.reset(); }
            for (size_t k = 0; k < stops.size(); k++) {
                Stop& st = stops[k];
                bool hit = false;
                if (!st.on) continue;
                if (st.sig >= 0) hit = sigHit(st);
                else if (st.kind == 6) { bool now = cond(st.cond); hit = now && !st.condWas; st.condWas = now; }
                if (!hit) continue;
                std::string text = "stop " + std::to_string(st.id) + ": " + (st.sig >= 0 ? st.name + " = " + fmt(S.get(st.sig)) : "{" + st.cond + "}");
                std::string cmd = st.cmd;
                bool cont = st.cont;
                if (st.once) { stops.erase(stops.begin() + (long)k); k--; }
                if (cont || !cmd.empty()) {
                    if (cont) std::cout << "time " << S.now << ": " << text << "\n";
                    else if (why.empty()) { std::cout << "time " << S.now << ": " << text << "\n"; why = "-"; }
                    if (!cmd.empty()) script(cmd);
                    continue;
                }
                if (why.empty()) why = text;
            }
            if (!why.empty()) return true;
            if (slots > 0 && --slots == 0) { why = "slot"; slots = -1; return true; }
        }
        return false;
    }
    // Starts the run asked for, or lets a paused one go on.
    void go() {
        if (nest) { resume = true; return; }
        if (!advance()) { ended = true; std::cout << "simulation ended at time " << S.now << "\n"; return; }
        if (why != "-") std::cout << "time " << S.now << ": " << why << "\n";
    }
    // A value written from here takes effect in a slot at the current time.
    void settle() {
        if (!started || nest) return;
        S.wheel[S.now];
        S.run(S.now, false);
    }

    // ---- reading commands ----
    bool read(std::string& buf) {
        buf.clear();
        std::string line;
        for (;;) {
            if (prompt) std::cout << (buf.empty() ? (nest ? "msim(stopped)> " : "msim> ") : "...   ") << std::flush;
            if (!std::getline(*in, line)) {
                if (replaying && after) { endReplay(); continue; }
                if (buf.empty()) return false;
                history.push_back(buf);
                return true;
            }
            buf += line + "\n";
            if (tcl::Interp::complete(buf)) { history.push_back(buf); return true; }
        }
    }
    void run(const std::string& text) {
        size_t before = tcl.diags.size();
        std::string res = tcl.parse(text, "<command>");
        for (size_t i = before; i < tcl.diags.size(); i++) std::cout << "Error: " << tcl.diags[i].text << "\n";
        tcl.diags.resize(before);
        if (!res.empty()) std::cout << res << "\n";
        if (tcl.exited) quitNow = true;
    }

    // ---- commands ----
    using W = std::vector<std::string>;
    int sigArg(const W& a, size_t i) {
        if (i >= a.size()) throw tcl::Error(a[0] + " needs a signal");
        int s = find(a[i]);
        if (s < 0) throw tcl::Error("no signal '" + a[i] + "'");
        return s;
    }
    static bool flag(const std::string& s, std::initializer_list<const char*> names) {
        for (const char* n : names) if (s == n) return true;
        return false;
    }

    std::string cmdRun(const W& a) {
        if (a[0] == "step" || a[0] == "next" || (a.size() > 1 && flag(a[1], {"-step", "-next"}))) {
            stepAll();
            bool nx = a[0] == "next" || (a.size() > 1 && a[1] == "-next");
            stepMode = nx ? 2 : 1;
            stepProg = nx && nest ? curProg : nullptr;
            limit = ~0ULL;
            go();
            return "";
        }
        limit = ~0ULL;
        slots = -1;
        bool absolute = false;
        std::optional<uint64_t> t;
        for (size_t i = 1; i < a.size(); i++) {
            const std::string& o = a[i];
            if (o == "-all") continue;
            if (o == "-absolute") absolute = true;
            else if (o == "-relative") absolute = false;
            else if (flag(o, {"-posedge", "-negedge", "-change"})) {
                int s = sigArg(a, ++i);
                until = std::make_unique<Stop>();
                until->sig = s;
                until->name = shown(s);
                until->kind = o == "-posedge" ? 1 : o == "-negedge" ? 2 : 0;
            } else if (o == "-slot" || o == "-slots" || o == "-timepoint") {
                slots = i + 1 < a.size() && std::isdigit((unsigned char)a[i + 1][0]) ? std::max(1L, std::atol(a[++i].c_str())) : 1;
            } else t = ticks(a, i);
        }
        if (t) limit = absolute ? *t : S.now + *t;
        if (t && absolute && *t <= S.now && started) throw tcl::Error("time " + std::to_string(*t) + " is not after now (" + std::to_string(S.now) + ")");
        go();
        return "";
    }

    std::string cmdStop(const W& a) {
        if (a.size() == 1 || (a.size() == 2 && flag(a[1], {"-show", "-list"}))) {
            for (auto& st : stops)
                std::cout << st.id << ": " << stopText(st) << (st.cond.empty() || st.kind == 6 ? "" : " if {" + st.cond + "}")
                          << (st.on ? "" : " (disabled)") << (st.once ? " (once)" : "") << "\n";
            if (stops.empty()) std::cout << "no stops\n";
            return "";
        }
        if (flag(a[1], {"-delete", "-disable", "-enable", "-clear"})) {
            if (a.size() < 3) throw tcl::Error("stop " + a[1] + " N ... | all");
            for (size_t i = 2; i < a.size(); i++) {
                bool all = a[i] == "all" || a[i] == "*";
                int n = all ? 0 : std::atoi(a[i].c_str());
                bool any = false;
                for (size_t k = 0; k < stops.size(); k++) {
                    if (!all && stops[k].id != n) continue;
                    any = true;
                    if (a[1] == "-delete" || a[1] == "-clear") { stops.erase(stops.begin() + (long)k); k--; }
                    else stops[k].on = a[1] == "-enable";
                }
                if (!any && !all) throw tcl::Error("no stop " + a[i]);
            }
            return "";
        }
        Stop st;
        std::string file;
        bool lineGiven = false;
        std::vector<std::string> pos;
        for (size_t i = 1; i < a.size(); i++) {
            const std::string& o = a[i];
            auto arg = [&]() -> const std::string& { if (i + 1 >= a.size()) throw tcl::Error("stop " + o + " needs a value"); return a[++i]; };
            if (o == "-create") continue;
            if (o == "-object" || o == "-change" || o == "-event") { st.kind = 0; pos.insert(pos.begin(), arg()); }
            else if (o == "-posedge") { st.kind = 1; pos.insert(pos.begin(), arg()); }
            else if (o == "-negedge") { st.kind = 2; pos.insert(pos.begin(), arg()); }
            else if (o == "-line") { st.kind = 4; st.line = (uint32_t)std::atol(arg().c_str()); lineGiven = true; }
            else if (o == "-file") file = arg();
            else if (o == "-absolute" || o == "-time") { st.kind = 5; ++i; st.time = ticks(a, i); }
            else if (o == "-relative" || o == "-delay") { st.kind = 5; ++i; st.time = S.now + ticks(a, i); }
            else if (o == "-condition") st.cond = arg();
            else if (o == "-command" || o == "-execute") st.cmd = arg();
            else if (o == "-continue") st.cont = true;
            else if (o == "-once") st.once = true;
            else if (o == "-name" || o == "-instance" || o == "-thread") arg();   // accepted, not used
            else if (!o.empty() && o[0] == '-' && !std::isdigit((unsigned char)o[1])) throw tcl::Error("unknown option " + o + " (stop -help is in help)");
            else pos.push_back(o);
        }
        if (lineGiven) {
            if (file.empty() && g_runLoc.file) file = g_covFiles[g_runLoc.file];
            if (file.empty()) {
                std::vector<uint32_t> all;
                for (uint32_t f = 1; f < g_covFiles.size(); f++) all.push_back(f);
                if (all.size() != 1) throw tcl::Error("stop -line N -file FILE: which file?");
                st.files = all;
            } else st.files = filesNamed(file);
            if (st.files.empty()) throw tcl::Error("no source file '" + file + "'");
            if (!mark(st.files, st.line)) throw tcl::Error("no statement at " + fileName(st.files[0]) + ":" + std::to_string(st.line));
        } else if (st.kind == 5) {
            if (started && st.time <= S.now) throw tcl::Error("time " + std::to_string(st.time) + " is not after now");
        } else if (pos.empty()) {
            if (st.cond.empty()) throw tcl::Error("stop: a signal, -line, -absolute or -condition");
            st.kind = 6;
        } else {
            int s = find(pos[0]);
            if (s < 0) throw tcl::Error("no signal '" + pos[0] + "'");
            st.sig = s;
            st.name = shown(s);
            st.last = S.get(s);
            if (pos.size() > 1) {
                if (pos[1] == "posedge") st.kind = 1;
                else if (pos[1] == "negedge") st.kind = 2;
                else if (pos[1] == "==" && pos.size() > 2 && parse(pos[2], S.sigs[s].w, st.want)) st.kind = 3;
                else throw tcl::Error("stop SIG [posedge|negedge|== VALUE]");
            }
        }
        if (!st.cond.empty()) cond(st.cond);   // a bad condition fails now, not at the first check
        if (st.kind == 6) st.condWas = cond(st.cond);
        st.id = nextId++;
        stops.push_back(st);
        std::cout << "stop " << st.id << " set on " << (st.sig >= 0 ? st.name : stopText(st)) << "\n";
        return "";
    }

    std::string cmdGet(const W& a) {
        std::string r;
        std::vector<int> sigs;
        for (size_t i = 1; i < a.size(); i++) {
            if (a[i] == "-radix" || a[i] == "-format") { if (++i >= a.size()) throw tcl::Error("-radix needs a value"); r = a[i]; continue; }
            if (a[i].size() > 1 && a[i][0] == '-') { r = a[i].substr(1); continue; }   // xrun value %d style is below
            if (a[i][0] == '%') { r = a[i][1] == 'd' ? "dec" : a[i][1] == 'b' ? "bin" : a[i][1] == 'o' ? "oct" : "hex"; continue; }
            sigs.push_back(sigArg(a, i));
        }
        if (sigs.empty()) throw tcl::Error(a[0] + " needs a signal");
        std::string out;
        for (int s : sigs) out += (out.empty() ? "" : " ") + radix(S.get(s), r);
        return out;
    }

    std::string cmdWrite(const W& a) {
        bool force = a[0] == "force";
        std::vector<std::string> w;
        for (size_t i = 1; i < a.size(); i++) {
            if (a[i] == "-deposit") force = false;
            else if (a[i] == "-freeze" || a[i] == "-keepvalue") force = true;
            else if (!a[i].empty() && a[i][0] == '-' && a[i].size() > 1 && !std::isdigit((unsigned char)a[i][1])) continue;   // -release, -cancel: accepted
            else w.push_back(a[i]);
        }
        if (w.size() == 3 && w[1] == "=") w.erase(w.begin() + 1);   // xrun deposit sig = value
        W b{a[0]};
        b.insert(b.end(), w.begin(), w.end());
        int s = sigArg(b, 1);
        Val v;
        if (w.size() < 2 || !parse(w[1], S.sigs[s].w, v)) throw tcl::Error(a[0] + " SIG VALUE (a number such as 5, 8'hff or 'b1x; up to 64 bits)");
        if (force) S.holdOn(s, konst(v), true);
        else S.set(s, v);
        settle();
        std::cout << shown(s) << " = " << fmt(S.get(s)) << (force ? "  (forced)" : "") << "\n";
        return "";
    }

    std::string cmdScope(const W& a) {
        if (a.size() == 1 || (a.size() == 2 && flag(a[1], {"-show", "-get"}))) return scopeShown();
        std::string to = a.back();
        if (a[1] == "-up" || to == "..") {
            size_t d = scope.rfind('.');
            scope = d == std::string::npos ? "" : scope.substr(0, d);
            return scopeShown();
        }
        if (a[1] == "-top" || to == "/") { scope.clear(); return scopeShown(); }
        auto sc = scopeName(to);
        if (!sc) throw tcl::Error("no scope '" + to + "'");
        scope = *sc;
        return scopeShown();
    }

    std::string cmdShow(const W& a) {
        bool sigs = true, scopes = true, ports = false, values = false;
        std::string target;
        for (size_t i = 1; i < a.size(); i++) {
            const std::string& o = a[i];
            if (o == "-scopes" || o == "-instances") { scopes = true; sigs = false; }
            else if (o == "-signals" || o == "-variables" || o == "-nets") { sigs = true; scopes = false; }
            else if (o == "-ports") { ports = true; scopes = false; }
            else if (o == "-value" || o == "-values") values = true;
            else if (!o.empty() && o[0] == '-') throw tcl::Error("show [-scopes|-signals|-ports] [-value] [SCOPE]");
            else target = o;
        }
        std::string rel = scope;
        if (!target.empty()) {
            auto sc = scopeName(target);
            if (!sc) throw tcl::Error("no scope '" + target + "'");
            rel = *sc;
        }
        const slang::ast::Scope* sc = scopeAt(rel);
        if (!sc) throw tcl::Error("no scope");
        std::string pre = rel.empty() ? "" : rel + ".";
        for (const auto& m : sc->members()) {
            using K = slang::ast::SymbolKind;
            if (m.kind == K::Instance) {
                if (scopes) std::cout << m.name << " (instance of " << m.as<slang::ast::InstanceSymbol>().getDefinition().name << ")\n";
            } else if (m.kind == K::GenerateBlock) {
                if (scopes && !m.as<slang::ast::GenerateBlockSymbol>().isUninstantiated && !m.name.empty()) std::cout << m.name << " (generate block)\n";
            } else if (m.kind == K::GenerateBlockArray) {
                if (scopes)
                    for (auto* e : m.as<slang::ast::GenerateBlockArraySymbol>().entries)
                        if (!e->isUninstantiated && e->arrayIndex) std::cout << m.name << "[" << e->arrayIndex->toString() << "] (generate block)\n";
            } else if (m.kind == K::Port) {
                if (ports) {
                    auto d = m.as<slang::ast::PortSymbol>().direction;
                    const char* dn = d == slang::ast::ArgumentDirection::In ? "input" : d == slang::ast::ArgumentDirection::Out ? "output" : "inout";
                    std::cout << m.name << " (" << dn << ")";
                    if (values) if (int s = findAbs(pre + std::string(m.name)); s >= 0) std::cout << " = " << fmt(S.get(s));
                    std::cout << "\n";
                }
            } else if ((m.kind == K::Variable || m.kind == K::Net) && sigs && !ports) {
                int s = findAbs(pre + std::string(m.name));
                if (s < 0) continue;
                std::cout << m.name;
                if (values) std::cout << " = " << fmt(S.get(s));
                std::cout << "\n";
            }
        }
        return "";
    }

    std::string cmdDescribe(const W& a) {
        if (a.size() < 2) throw tcl::Error("describe NAME");
        std::string out;
        for (size_t i = 1; i < a.size(); i++) {
            try { out += describe(a[i]); }
            catch (const tcl::Error& e) { out += e.what(); }
            if (i + 1 < a.size()) out += "\n";
        }
        return out;
    }
    std::string describe(const std::string& n) {
        std::string out;
        {
            if (auto sc = scopeName(n)) {
                const slang::ast::Scope* s = scopeAt(*sc);
                const slang::ast::Symbol& sym = s->asSymbol();
                std::string full = sc->empty() ? S.vcdTop : S.vcdTop + "." + *sc;
                if (sym.kind == slang::ast::SymbolKind::InstanceBody) {
                    auto& body = sym.as<slang::ast::InstanceBodySymbol>();
                    out += full + ": instance of " + std::string(body.getDefinition().getKindString()) + " " + std::string(body.getDefinition().name) +
                           ", declared at " + where(body.getDefinition().location);
                } else out += full + ": generate block, at " + where(sym.location);
            } else {
                std::string full = n.rfind(S.vcdTop + ".", 0) == 0 ? n : S.vcdTop + "." + (scope.empty() ? "" : scope + ".") + n;
                std::vector<std::string> parts = split(full);
                size_t used = 0;
                const slang::ast::Symbol* sym = parts.size() > 1 ? resolve(parts, used) : nullptr;
                if ((!sym || used != parts.size()) && !scope.empty()) {
                    full = S.vcdTop + "." + n;
                    parts = split(full);
                    sym = parts.size() > 1 ? resolve(parts, used) : nullptr;
                }
                if (!sym || used != parts.size()) throw tcl::Error("no object '" + n + "'");
                std::string kind = sym->kind == slang::ast::SymbolKind::Net ? "net" : sym->kind == slang::ast::SymbolKind::Variable ? "variable"
                                 : sym->kind == slang::ast::SymbolKind::Parameter ? "parameter" : std::string(slang::ast::toString(sym->kind));
                std::string ty;
                if (auto* v = sym->as_if<slang::ast::ValueSymbol>()) ty = v->getType().toString();
                out += full + ": " + kind + (ty.empty() ? "" : " " + ty);
                if (auto* p = sym->as_if<slang::ast::ParameterSymbol>()) out += " = " + p->getValue().toString();
                int s = findAbs(full);
                if (s >= 0) out += ", " + std::to_string(S.sigs[s].w) + " bits";
                out += ", declared at " + where(sym->location);
            }
        }
        return out;
    }

    std::string cmdWhere(const W&) {
        if (!g_runLoc.line) return "time " + std::to_string(S.now) + ", no statement has run yet";
        std::string s = "time " + std::to_string(S.now) + ", " + (nest ? "stopped at " : "last statement ") + here(g_runLoc.file, g_runLoc.line);
        if (nest && curProg)
            if (auto it = procOf.find(curProg); it != procOf.end()) {
                const LoweredProc* lp = it->second;
                static const std::map<std::string, std::string> kinds = {
                    {"Always", "always"}, {"AlwaysFF", "always_ff"}, {"AlwaysComb", "always_comb"}, {"AlwaysLatch", "always_latch"},
                    {"Initial", "initial"}, {"Final", "final"}, {"init", "initializer"}, {"assign", "assign"}};
                auto k = kinds.find(lp->kind);
                s += "\n  in " + (k == kinds.end() ? lp->kind : k->second) + " of " + (lp->scope.empty() ? S.vcdTop : S.vcdTop + "." + lp->scope);
            }
        return s;
    }

    std::string cmdListing(const W& a) {
        uint32_t f = g_runLoc.file, l = g_runLoc.line;
        int n = 5;
        for (size_t i = 1; i < a.size(); i++) {
            if (a[i] == "-file" && i + 1 < a.size()) {
                auto fs = filesNamed(a[++i]);
                if (fs.empty()) throw tcl::Error("no source file '" + a[i] + "'");
                f = fs[0];
                if (!l) l = 1;
            } else if (a[i] == "-line" && i + 1 < a.size()) l = (uint32_t)std::atol(a[++i].c_str());
            else n = std::max(1, std::atoi(a[i].c_str()));
        }
        if (!f || !l) throw tcl::Error("no statement has run yet: listing -file F -line N");
        const auto& v = lines(f);
        if (v.empty()) throw tcl::Error("cannot read " + g_covFiles[f]);
        uint32_t lo = l > (uint32_t)n ? l - (uint32_t)n : 1, hi = std::min<uint32_t>((uint32_t)v.size(), l + (uint32_t)n);
        std::ostringstream o;
        o << fileName(f) << ":\n";
        for (uint32_t k = lo; k <= hi; k++) o << (k == g_runLoc.line && f == g_runLoc.file ? "-> " : "   ") << std::setw(4) << k << "  " << v[k - 1] << "\n";
        std::string s = o.str();
        s.pop_back();
        return s;
    }

    std::string cmdSearch(const W& a) {
        std::string pat = a.size() > 1 ? a.back() : "*";
        bool glob = pat.find_first_of("*?") != std::string::npos || a[0] == "search";
        std::regex re;
        if (glob) {
            std::string r;
            for (char ch : pat) r += ch == '*' ? std::string(".*") : ch == '?' ? std::string(".") : std::strchr(".[]()+^$|\\{}", ch) ? std::string("\\") + ch : std::string(1, ch);
            re = std::regex(r);
        }
        names();
        std::vector<std::string> out;
        for (auto& [n, s] : byName) {
            if (S.sigs[s].hide) continue;
            std::string full = S.vcdTop + "." + n;
            bool hit = glob ? std::regex_search(full, re) && (std::regex_match(full, re) || std::regex_match(n, re) ||
                                                              std::regex_match(full.substr(full.rfind('.') + 1), re))
                            : a.size() < 2 || full.find(pat) != std::string::npos;
            if (hit) out.push_back(full);
        }
        std::sort(out.begin(), out.end());
        for (size_t k = 0; k < out.size() && k < 200; k++) std::cout << out[k] << "\n";
        if (out.size() > 200) std::cout << "... " << out.size() - 200 << " more\n";
        if (out.empty() && a[0] == "search") std::cout << "nothing matches " << pat << "\n";
        return "";
    }

    std::string cmdDump(const W& a) {
        if (a.size() == 1) throw tcl::Error("dump -file F | -add NAMES [-depth N] | -off | -on | -flush | -close");
        std::vector<std::pair<std::string, bool>> sel;
        int64_t depth = 0;
        bool add = a[0] == "probe";
        for (size_t i = 1; i < a.size(); i++) {
            const std::string& o = a[i];
            auto arg = [&]() -> const std::string& { if (i + 1 >= a.size()) throw tcl::Error(o + " needs a value"); return a[++i]; };
            if (o == "-file") { S.dumpTask(DC_FILE, arg(), 0, {}); std::cout << "dump file " << S.dumpFile << "\n"; }
            else if (o == "-type" || o == "-shm" || o == "-database" || o == "-name" || o == "-create" || o == "-fid") { if (o == "-type" || o == "-database" || o == "-name" || o == "-fid") arg(); }
            else if (o == "-add") add = true;
            else if (o == "-depth") { const std::string& d = arg(); depth = d == "all" ? 0 : std::atoll(d.c_str()); }
            else if (o == "-all") depth = 0;
            else if (o == "-off") S.dumpTask(DC_OFF, "", 0, {});
            else if (o == "-on") S.dumpTask(DC_ON, "", 0, {});
            else if (o == "-flush") S.dumpTask(DC_FLUSH, "", 0, {});
            else if (o == "-close") { S.dumpTask(DC_FLUSH, "", 0, {}); S.dumpTask(DC_OFF, "", 0, {}); }
            else if (!o.empty() && o[0] == '-') throw tcl::Error("unknown option " + o);
            else {
                // A list of names: scopes (everything below, -depth levels) or signals.
                std::istringstream is(o);
                for (std::string n; is >> n;) {
                    if (auto sc = scopeName(n)) sel.push_back({sc->empty() ? "" : *sc + ".", false});
                    else if (int s = find(n); s >= 0) sel.push_back({S.sigs[s].name, true});
                    else throw tcl::Error("no scope or signal '" + n + "'");
                }
            }
        }
        if (add) {
            if (S.waving()) throw tcl::Error("the dump has started: signals can only be added before it (21.7.1.2)");
            if (sel.empty()) sel.push_back({scope.empty() ? "" : scope + ".", false});
            S.dumpTask(DC_VARS, "", depth, sel);
            std::cout << "dumping to " << S.dumpFile << " from the next time slot\n";
        }
        return "";
    }

    // Verdi's UCLI commands (the FSDB PLI adds them to VCS and Xcelium):
    // fsdbDumpfile NAME, fsdbDumpvars [DEPTH] [SCOPE ...] [+OPTION ...],
    // fsdbDumpon / off / flush / Finish, fsdbDumpMDA. As the system tasks,
    // they write FST (vendor_tasks.hpp).
    std::string cmdFsdb(const W& a) {
        const std::string& c = a[0];
        if (c == "fsdbDumpfile") {
            if (a.size() < 2) throw tcl::Error("fsdbDumpfile NAME");
            S.dumpTask(DC_VFILE, a[1], 0, {});
            std::cout << "dump file " << S.dumpFile << "\n";
        } else if (c == "fsdbDumpon") S.dumpTask(DC_ON, "", 0, {});
        else if (c == "fsdbDumpoff") S.dumpTask(DC_OFF, "", 0, {});
        else if (c == "fsdbDumpflush") S.dumpTask(DC_FLUSH, "", 0, {});
        else if (c == "fsdbDumpFinish") { S.dumpTask(DC_FLUSH, "", 0, {}); S.dumpTask(DC_OFF, "", 0, {}); }
        else if (c == "fsdbDumpMDA") S.dumpTask(DC_ARRAYS, "", 0, {});
        else {   // fsdbDumpvars
            std::vector<std::pair<std::string, bool>> sel;
            int64_t depth = 0;
            bool arrays = false;
            for (size_t i = 1; i < a.size(); i++) {
                const std::string& o = a[i];
                if (o.rfind("+fsdbfile+", 0) == 0) { S.dumpTask(DC_VFILE, o.substr(10), 0, {}); continue; }
                if (o == "+mda" || o == "+all") { arrays = true; continue; }
                if (!o.empty() && o[0] == '+') continue;
                if (i == 1 && !o.empty() && std::isdigit((unsigned char)o[0])) { depth = std::atoll(o.c_str()); continue; }
                if (auto sc = scopeName(o)) sel.push_back({sc->empty() ? "" : *sc + ".", false});
                else if (int s = find(o); s >= 0) sel.push_back({S.sigs[s].name, true});
                else throw tcl::Error("no scope or signal '" + o + "'");
            }
            if (S.waving()) throw tcl::Error("the dump has started: signals can only be added before it (21.7.1.2)");
            S.dumpTask(DC_VFILE, "?novas.fsdb", 0, {});
            if (arrays) S.dumpTask(DC_ARRAYS, "", 0, {});
            if (sel.empty()) sel.push_back({scope.empty() ? "" : scope + ".", false});
            S.dumpTask(DC_VARS, "", depth, sel);
            std::cout << "dumping to " << S.dumpFile << " from the next time slot\n";
        }
        return "";
    }

    std::string cmdSenv(const W& a) {
        std::map<std::string, std::string> e = {
            {"time", std::to_string(S.now)}, {"scope", scopeShown()}, {"top", S.vcdTop},
            {"timescale", "1e" + std::to_string(timeFmt().tickExp) + " s"},
            {"file", g_runLoc.file ? g_covFiles[g_runLoc.file] : ""}, {"line", std::to_string(g_runLoc.line)},
            {"stopped", nest ? "1" : "0"}};
        if (a.size() > 1) {
            auto it = e.find(a[1]);
            if (it == e.end()) throw tcl::Error("senv: time, scope, top, timescale, file, line or stopped");
            return it->second;
        }
        std::string s;
        for (auto& [k, v] : e) s += (s.empty() ? "" : "\n") + k + " " + v;
        return s;
    }


    // ---- drivers and loads: the statements of the simulation that write or read a signal ----
    std::string who(const Prog* p) {
        if (auto it = procOf.find(p); it != procOf.end()) {
            const LoweredProc* lp = it->second;
            std::string sc = lp->scope.empty() ? S.vcdTop : S.vcdTop + "." + lp->scope;
            if (!lp->port.empty()) return "port " + S.vcdTop + "." + lp->port;
            static const std::map<std::string, std::string> kinds = {
                {"Always", "always"}, {"AlwaysFF", "always_ff"}, {"AlwaysComb", "always_comb"}, {"AlwaysLatch", "always_latch"},
                {"Initial", "initial"}, {"Final", "final"}, {"init", "initializer"}, {"assign", "assign"}};
            auto k = kinds.find(lp->kind);
            return (k == kinds.end() ? lp->kind : k->second) + " in " + sc;
        }
        if (p && p->sub) return std::string("subroutine ") + std::string(static_cast<const slang::ast::SubroutineSymbol*>(p->sub)->name);
        return "process";
    }
    static bool reads(const E& e, int s) {
        if (!e) return false;
        if ((e->op == Op::Ref && e->sig == s) || (e->arr && s >= e->arr->base && s < e->arr->base + e->arr->count)) return true;
        if (reads(e->at, s)) return true;
        for (auto& k : e->kids) if (reads(k, s)) return true;
        return false;
    }
    std::string cmdDrivers(const W& a) {
        bool load = a[0] == "loads";
        if (a.size() < 2) throw tcl::Error(a[0] + " SIG ...");
        index();
        std::string out;
        for (size_t n = 1; n < a.size(); n++) {
            if (a[n][0] == '-') continue;   // -full and the like: accepted
            int s = sigArg(a, n);
            std::vector<std::string> rows;
            for (auto& r : ranges)
                for (const Instr* i = r.b; i < r.e; i++) {
                    bool hit;
                    if (load) {
                        hit = reads(i->e, s) || (i->k == IK::WaitEdge && i->sig == s);
                        for (auto& x : i->args) hit = hit || reads(x, s);
                        for (auto& [sg, ed] : i->events) hit = hit || sg == s;
                        for (auto& [off, x] : i->init) hit = hit || reads(x, s);
                    } else hit = (i->k == IK::Assign || i->k == IK::Nba || i->k == IK::Force) && i->sig == s;
                    if (!hit) continue;
                    std::string what = who(r.p) + (i->k == IK::Force ? ", force" : "");
                    std::string row = i->line ? "  " + here(i->file, i->line) + "  (" + what + ")" : "  (" + what + ")";
                    if (std::find(rows.begin(), rows.end(), row) == rows.end()) rows.push_back(row);
                }
            if (!load)
                if (auto h = S.holds.find(s); h != S.holds.end() && (h->second.fe || !h->second.parts.empty())) rows.push_back("  (forced now)");
            out += (out.empty() ? "" : "\n") + shown(s) + ": " + std::to_string(rows.size()) + (load ? " load" : " driver") + (rows.size() == 1 ? "" : "s");
            for (auto& r : rows) out += "\n" + r;
        }
        return out;
    }

    // ---- call: a task or function of the design, from the prompt ----
    const Subr* subrNamed(const std::string& n) {
        if (!lr) return nullptr;
        std::string scopePart, name = n;
        if (size_t d = n.rfind('.'); d != std::string::npos) { scopePart = n.substr(0, d); name = n.substr(d + 1); }
        std::vector<std::string> tries;
        if (scopePart.empty()) { tries.push_back(scope); if (!scope.empty()) tries.push_back(""); }
        else if (auto sc = scopeName(scopePart)) tries.push_back(*sc);
        for (auto& t : tries) {
            const slang::ast::Scope* sc = scopeAt(t);
            if (!sc) continue;
            const slang::ast::Symbol* m = sc->find(name);
            if (!m || m->kind != slang::ast::SymbolKind::Subroutine) continue;
            for (auto& sb : lr->subrs) if (sb->sym == &m->as<slang::ast::SubroutineSymbol>()) return sb.get();
        }
        return nullptr;
    }
    std::string cmdCall(const W& a0) {
        // call name arg ...   call {name(arg, ...)}   call name(arg, ...)
        std::string text;
        for (size_t i = 1; i < a0.size(); i++) text += (i > 1 ? " " : "") + a0[i];
        std::string name = text;
        std::vector<std::string> args;
        if (size_t p = text.find('('); p != std::string::npos) {
            name = text.substr(0, p);
            size_t e = text.rfind(')');
            std::string in = text.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
            std::stringstream ss(in);
            for (std::string x; std::getline(ss, x, ',');) {
                size_t b = x.find_first_not_of(" \t"), f = x.find_last_not_of(" \t");
                if (b != std::string::npos) args.push_back(x.substr(b, f - b + 1));
            }
        } else {
            std::istringstream ss(text);
            ss >> name;
            for (std::string x; ss >> x;) args.push_back(x);
        }
        while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
        if (name.empty()) throw tcl::Error("call TASK(ARGS) or call TASK ARG ...");
        const Subr* sb = subrNamed(name);
        if (!sb || !sb->prog) throw tcl::Error("no task or function '" + name + "' here (scope " + scopeShown() + ")");
        if (sb->cls) throw tcl::Error(name + " is a class method: call it from the design");
        auto w = std::make_unique<Prog>();
        Instr c;
        c.k = IK::Call;
        c.callee = sb->prog;
        c.lay = sb->lay;
        c.noSelf = true;
        size_t k = 0;
        std::vector<std::pair<std::string, const Subr::Arg*>> outs;
        const auto& formals = sb->sym->getArguments();
        for (size_t n = 0; n < sb->args.size(); n++) {
            const Subr::Arg& f = sb->args[n];
            std::string fname = n < formals.size() ? std::string(formals[n]->name) : "arg" + std::to_string(n);
            if (f.arr) throw tcl::Error(name + ": array argument '" + fname + "' cannot be given from the prompt");
            if (f.dir == slang::ast::ArgumentDirection::Out) { outs.push_back({fname, &f}); continue; }
            if (f.dir == slang::ast::ArgumentDirection::Ref) throw tcl::Error(name + ": ref argument '" + fname + "' cannot be given from the prompt");
            Val v;
            if (k < args.size()) {
                if (!parse(args[k], std::min(f.w, 64), v)) throw tcl::Error("bad value '" + args[k] + "' for " + fname);
                k++;
            } else if (n < formals.size() && formals[n]->getDefaultValue()) {
                throw tcl::Error(name + ": give " + fname + " (default arguments are not read from the prompt)");
            } else throw tcl::Error(name + " needs " + fname);
            v = v_resize(v, f.w, false);
            if (f.dir == slang::ast::ArgumentDirection::InOut) outs.push_back({fname, &f});
            if (sb->lay) c.init.push_back({f.sig, konst(v)});
            else {
                Instr as;
                as.k = IK::Assign;
                as.sig = f.sig;
                as.e = konst(v);
                w->code.push_back(as);
            }
        }
        if (k < args.size()) throw tcl::Error(name + ": too many arguments");
        w->code.push_back(c);
        // What it gives back, printed when it returns.
        Instr d;
        d.k = IK::Display;
        std::string fmt = name + " returned";
        auto val = [&](int sig) { return ref(sb->lay ? relSig(kRelRet, sig) : sig); };
        bool any = false;
        if (sb->retSig >= 0 && !sb->retDyn) { fmt += " %0d"; d.args.push_back(val(sb->retSig)); any = true; }
        for (auto& [fn, f] : outs) { fmt += std::string(any ? "," : "") + " " + fn + " = %0d"; d.args.push_back(val(f->sig)); any = true; }
        if (!any) fmt = name + " done";
        d.fmt = fmt;
        w->code.push_back(d);
        const Prog* wp = w.get();
        calls.push_back(std::move(w));
        index();
        ranges.push_back({wp->code.data(), wp->code.data() + wp->code.size(), wp});
        if (!started) { started = true; S.run(S.now, true); }
        S.spawn(process(wp));
        if (nest) return "the call runs when the simulation goes on";
        settle();
        return "";
    }

    // ---- save / restore / restart: by replay, as $save and msim -restore (annexd.inc) ----
    std::string cmdSave(const W& a) {
        std::string f;
        for (size_t i = 1; i < a.size(); i++) if (a[i][0] != '-') f = a[i];
        if (f.empty()) throw tcl::Error("save FILE");
        std::ofstream o(f);
        if (!o) throw tcl::Error("cannot write " + f);
        char cwd[4096];
        o << "msim checkpoint\nsave 0\ntime " << S.now << "\nstate " << annexd::stateDigest() << "\n";
        if (getcwd(cwd, sizeof cwd)) o << "cwd " << cwd << "\n";
        for (auto& x : annexd::argv0()) o << "arg " << x << "\n";
        // The commands up to this one: a restore runs them again.
        for (size_t i = 0; i + 1 < history.size(); i++) {
            std::istringstream is(history[i]);
            for (std::string ln; std::getline(is, ln);) o << "ucli " << ln << "\n";
        }
        if (!o) throw tcl::Error("cannot write " + f);
        return "saved the simulation at time " + std::to_string(S.now) + " in " + f;
    }
    // Runs msim again with the checkpoint's command line; the new run replays
    // its commands quietly and goes on reading where this one is now.
    [[noreturn]] void again(const std::string& ckpt, bool fromZero) {
        if (!fromZero) setenv("MSIM_UCLI_RESTORE", ckpt.c_str(), 1);
        else setenv("MSIM_UCLI_RESTART", "1", 1);
        if (!doPath.empty() && in != &std::cin) {
            setenv("MSIM_UCLI_DO", std::filesystem::absolute(doPath).string().c_str(), 1);
            setenv("MSIM_UCLI_AT", std::to_string((long long)in->tellg()).c_str(), 1);
        } else {   // standard input: a file goes on at the next command; a terminal anyway
            long pos = ftell(stdin);
            if (pos >= 0) lseek(0, pos, SEEK_SET);
        }
        annexd::again();
    }
    std::string cmdRestore(const W& a) {
        bool restart = a[0] == "restart";
        std::string f;
        for (size_t i = 1; i < a.size(); i++) if (a[i][0] != '-') f = a[i];
        if (restart && f.empty()) again("", true);   // UCLI restart: from time 0
        if (f.empty()) throw tcl::Error("restore FILE");
        std::ifstream c(f);
        std::string magic, ln;
        std::getline(c, magic);
        std::vector<std::string> args;
        std::string cwd;
        bool ucli = false;
        while (std::getline(c, ln)) {
            if (ln.rfind("arg ", 0) == 0) args.push_back(ln.substr(4));
            else if (ln.rfind("cwd ", 0) == 0) cwd = ln.substr(4);
            else if (ln == "save 0") ucli = true;
        }
        if (magic != "msim checkpoint" || args.empty()) throw tcl::Error(f + " is not a checkpoint");
        if (!ucli) throw tcl::Error(f + " was written by $save: use $restart or msim -restore");
        std::string abs = std::filesystem::absolute(f).string();
        if (!doPath.empty()) doPath = std::filesystem::absolute(doPath).string();
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) throw tcl::Error("cannot go to " + cwd);
        annexd::argv0() = args;
        again(abs, false);
    }
    // At the start: a restore's replay, or a restart's input.
    void startReplay() {
        const char* r = getenv("MSIM_UCLI_RESTORE");
        const char* z = getenv("MSIM_UCLI_RESTART");
        if (!r && !z) return;
        std::string ck = r ? r : "";
        const char* dp = getenv("MSIM_UCLI_DO");
        const char* at = getenv("MSIM_UCLI_AT");
        std::istream* next = &std::cin;
        if (dp) {
            auto f = std::make_unique<std::ifstream>(dp);
            if (at) f->seekg(std::atoll(at));
            doPath = dp;
            own = std::move(f);
            next = own.get();
        }
        prompt = !dp && isatty(0);
        unsetenv("MSIM_UCLI_RESTORE"); unsetenv("MSIM_UCLI_RESTART"); unsetenv("MSIM_UCLI_DO"); unsetenv("MSIM_UCLI_AT");
        if (z) { in = next; std::cout << "restarted at time 0\n"; return; }
        std::ifstream c(ck);
        std::string text, ln;
        uint64_t t = 0;
        while (std::getline(c, ln)) {
            if (ln.rfind("ucli ", 0) == 0) text += ln.substr(5) + "\n";
            else if (ln.rfind("state ", 0) == 0) replayState = ln.substr(6);
            else if (ln.rfind("time ", 0) == 0) t = std::strtoull(ln.c_str() + 5, nullptr, 10);
        }
        (void)t;
        replay = std::make_unique<std::istringstream>(text);
        after = next;
        in = replay.get();
        replaying = true;
        savedPrompt = prompt;
        prompt = false;
        std::cout.flush();
        fflush(nullptr);
        annexd::savedOut = dup(1);
        annexd::savedErr = dup(2);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); close(nul); }
    }
    std::unique_ptr<std::istringstream> replay;
    bool savedPrompt = false;
    void endReplay() {
        replaying = false;
        annexd::unmute();
        in = after;
        after = nullptr;
        prompt = savedPrompt;
        if (!replayState.empty() && annexd::stateDigest() != replayState)
            std::cerr << "msim: warning: the state at the checkpoint differs from the one saved; the run is "
                         "not repeatable (it reads the clock, the environment or a DPI model?)\n";
        std::cout << "restored the checkpoint at time " << S.now << "\n";
    }

    void help() {
        std::cout <<
            "run [T [unit]] [-absolute] | run -posedge/-negedge/-change SIG | run -slot [N] | step | next\n"
            "stop SIG [posedge|negedge|== VALUE] | stop -line N [-file F] | stop -absolute/-relative T |\n"
            "  stop -condition {EXPR} | ... [-condition E] [-command {CMDS}] [-continue] [-once] |\n"
            "  stop [-show] | stop -delete/-disable/-enable N|all | delete N\n"
            "get SIG [-radix hex|dec|sdec|bin|oct] | print/value NAME ... | describe NAME |\n"
            "force SIG VALUE | release SIG | deposit/change SIG VALUE\n"
            "scope [PATH|-up] | show [-scopes|-signals|-ports] [-value] [SCOPE] | search PATTERN | signals [TEXT]\n"
            "where/stack | listing [N] | dump -file F | dump -add NAMES [-depth N] | probe NAMES | dump -off/-on/-flush/-close\n"
            "fsdbDumpfile F | fsdbDumpvars [DEPTH] [SCOPES] [+all] | fsdbDumpon/off/flush/Finish (written as FST)\n"
            "drivers SIG | loads SIG | call TASK(ARGS) | save FILE | restore FILE | restart\n"
            "senv [NAME] | alias NAME COMMAND | time | help | quit/exit/finish\n"
            "Tcl: set, if, foreach, while, proc, expr, source, [command] ...\n";
    }

    void commands() {
        tcl.busBrackets = true;
        tcl.onPuts = [](const std::string& s) { std::cout << s << "\n"; };
        tcl.onUnknown = [](const W& w, std::string&) {
            std::cout << "unknown command '" << w[0] << "' (help lists them)\n";
            return true;
        };
        auto quit = [this](const W&) {
            S.finished = true;
            quitNow = true;
            std::cout << "quit at time " << S.now << "\n";
            return std::string();
        };
        for (const char* q : {"quit", "exit", "finish"}) tcl.addCommand(q, quit);
        tcl.addCommand("help", [this](const W&) { help(); return std::string(); });
        tcl.addCommand("time", [](const W& a) {
            if (a.size() > 1) return std::to_string(S.now);   // time -value: just the number, for scripts
            std::cout << "time " << S.now << "\n";
            return std::string();
        });
        for (const char* r : {"run", "step", "next", "continue", "cont"}) tcl.addCommand(r, [this](const W& a) { return cmdRun(a); });
        tcl.addCommand("stop", [this](const W& a) { return cmdStop(a); });
        tcl.addCommand("delete", [this](const W& a) {
            if (a.size() < 2) throw tcl::Error("delete N ...");
            for (size_t i = 1; i < a.size(); i++) {
                size_t n = stops.size();
                if (a[i] == "all") stops.clear();
                else std::erase_if(stops, [&](const Stop& s) { return std::to_string(s.id) == a[i]; });
                std::cout << (stops.size() < n ? "deleted\n" : "no stop " + a[i] + "\n");
            }
            return std::string();
        });
        tcl.addCommand("get", [this](const W& a) { return cmdGet(a); });
        for (const char* d : {"database", "simvision", "dumptcf", "assertion"})   // xrun -input commands with no counterpart here
            tcl.addCommand(d, [](const W& a) {
                std::cerr << "msim: the command '" << a[0] << "' is not supported; ignored\n";
                return std::string();
            });
        for (const char* pr : {"print", "value"}) tcl.addCommand(pr, [this](const W& a) {
            if (a.size() < 2) std::cout << "print needs a name\n";
            for (size_t i = 1; i < a.size(); i++) {
                if (printPath(a[i])) continue;
                int s = find(a[i]);
                if (s < 0) std::cout << "no signal '" << a[i] << "'\n";
                else std::cout << shown(s) << " = " << fmt(S.get(s)) << "\n";
            }
            return std::string();
        });
        for (const char* f : {"force", "deposit", "change"}) tcl.addCommand(f, [this](const W& a) { return cmdWrite(a); });
        tcl.addCommand("release", [this](const W& a) {
            int s = sigArg(a, a.size() > 2 && a[1][0] == '-' ? 2 : 1);
            S.holdOff(s, true);
            settle();
            std::cout << shown(s) << " released\n";
            return std::string();
        });
        tcl.addCommand("scope", [this](const W& a) { return cmdScope(a); });
        tcl.addCommand("show", [this](const W& a) { return cmdShow(a); });
        tcl.addCommand("describe", [this](const W& a) { return cmdDescribe(a); });
        for (const char* w : {"where", "stack"}) tcl.addCommand(w, [this](const W& a) { return cmdWhere(a); });
        tcl.addCommand("listing", [this](const W& a) { return cmdListing(a); });
        for (const char* s : {"search", "signals"}) tcl.addCommand(s, [this](const W& a) { return cmdSearch(a); });
        for (const char* d : {"dump", "probe"}) tcl.addCommand(d, [this](const W& a) { return cmdDump(a); });
        for (const char* d : {"fsdbDumpfile", "fsdbDumpvars", "fsdbDumpon", "fsdbDumpoff", "fsdbDumpflush", "fsdbDumpFinish", "fsdbDumpMDA"})
            tcl.addCommand(d, [this](const W& a) { return cmdFsdb(a); });
        tcl.addCommand("senv", [this](const W& a) { return cmdSenv(a); });
        for (const char* d : {"drivers", "loads"}) tcl.addCommand(d, [this](const W& a) { return cmdDrivers(a); });
        tcl.addCommand("call", [this](const W& a) { return cmdCall(a); });
        for (const char* sv : {"save", "checkpoint"}) tcl.addCommand(sv, [this](const W& a) { return cmdSave(a); });
        for (const char* r : {"restore", "restart"}) tcl.addCommand(r, [this](const W& a) { return cmdRestore(a); });
        tcl.addCommand("alias", [this](const W& a) {
            if (a.size() < 3) throw tcl::Error("alias NAME COMMAND ...");
            std::string body;
            for (size_t i = 2; i < a.size(); i++) body += (i > 2 ? " " : "") + a[i];
            tcl.addCommand(a[1], [this, body](const W& w) {
                std::string line = body;
                for (size_t i = 1; i < w.size(); i++) line += " {" + w[i] + "}";
                size_t before = tcl.diags.size();
                std::string r = tcl.parse(line, "<alias>");
                if (tcl.diags.size() > before) {
                    std::string e = tcl.diags.back().text;
                    tcl.diags.resize(before);
                    throw tcl::Error(e);
                }
                return r;
            });
            return std::string();
        });
    }

    // Returns when the simulation is over or the commands are.
    void loop(std::istream& is) {
        in = &is;
        g_ses = this;
        commands();
        startReplay();
        std::string buf;
        while (!ended && !quitNow && !S.finished) {
            if (!read(buf)) break;
            run(buf);
        }
        if (quitNow || ended) return;
        // No more commands: the rest of the run.
        rest();
        while (advance()) {}
    }
};

inline void onStmt(const Instr& i) {
    Session* s = g_ses;
    if (s->prevHook) s->prevHook(i);
    s->stmt(i);
}

}   // namespace interactive
