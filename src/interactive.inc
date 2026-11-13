// msim -i: interactive simulation control (included by msim.cpp).
//
// Commands come from the standard input, or from -do FILE:
//   run [T]            run until a stop hits or the simulation ends; with T, for T time units
//   step [N]           run N time slots (default 1)
//   stop SIG [posedge|negedge|== VALUE]   stop when SIG changes / rises / falls / becomes VALUE
//   stop               list the stops;  delete N  removes one
//   print NAME ...     values now: signals, and class objects through their handles (tb.h, tb.h.next.id)
//   force SIG VALUE    hold SIG at VALUE until release SIG;  deposit SIG VALUE  writes it once
//   signals [TEXT]     the signals whose name contains TEXT
//   time, help, quit   (quit ends the simulation now; final blocks still run)
// The kernel is driven one time slot at a time through Sim::run(end), so a
// normal run pays nothing for this mode. Maintained by the debug tool thread.

namespace interactive {

struct Stop { int sig; std::string name; int kind; Val want, last; };   // kind 0 change, 1 posedge, 2 negedge, 3 == value

struct Session {
    slang::ast::Compilation* comp = nullptr;
    uint64_t maxTime = ~0ULL;
    bool started = false, prompt = false;
    std::unordered_map<std::string, int> byName;
    std::vector<Stop> stops;

    int find(const std::string& n) {
        if (byName.empty())
            for (size_t i = 0; i < S.sigs.size(); i++) {
                const std::string& nm = S.sigs[i].name;
                if (!nm.empty() && nm[0] != '$') byName.emplace(nm, (int)i);
            }
        auto it = byName.find(n);
        if (it != byName.end()) return it->second;
        // The simulation top's own name in front is fine: tb.u.q for u.q.
        const std::string pre = S.vcdTop + ".";
        if (n.rfind(pre, 0) == 0)
            if (it = byName.find(n.substr(pre.size())); it != byName.end()) return it->second;
        return -1;
    }
    std::string shown(int s) const { return S.vcdTop + "." + S.sigs[s].name; }

    static std::string fmt(const Val& v) {
        if (v.real) { double d; std::memcpy(&d, &v.v, sizeof d); std::ostringstream o; o << d; return o.str(); }
        if (v.str) return "\"" + Sim::fmtVal('s', v, 0, false) + "\"";
        bool unk = v.x != 0;
        for (auto l : v.hi) (void)l;
        std::string h = Sim::fmtVal(unk ? 'b' : 'h', v, 0, true);
        return std::to_string(v.w) + (unk ? "'b" : "'h") + h;
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

    // ---- class objects ----
    const slang::ast::Symbol* resolve(const std::vector<std::string>& parts, size_t& used) {
        const slang::ast::Scope* sc = nullptr;
        for (auto* t : comp->getRoot().topInstances)
            if (t->name == parts[0]) sc = &t->body;
        const slang::ast::Symbol* sym = nullptr;
        for (used = 1; sc && used < parts.size(); used++) {
            const slang::ast::Symbol* m = sc->find(parts[used]);
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
    bool printPath(const std::string& name) {
        std::vector<std::string> parts;
        std::istringstream is(name);
        for (std::string p; std::getline(is, p, '.');) parts.push_back(p);
        if (parts.size() < 2 || !comp) return false;
        size_t used = 0;
        const slang::ast::Symbol* sym = resolve(parts, used);
        if (!sym || sym->kind != slang::ast::SymbolKind::Variable) return false;
        if (!sym->as<slang::ast::VariableSymbol>().getType().getCanonicalType().isClass()) return false;
        std::string base = parts[0];
        for (size_t i = 1; i < used; i++) base += "." + parts[i];
        int s = find(base);
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

    // ---- running ----
    // One time slot at a time, checking the stops after each. slots < 0: no limit.
    // Returns false when the simulation is over.
    bool advance(uint64_t limit, long slots, std::string& why) {
        for (Stop& st : stops) st.last = S.get(st.sig);
        while (!S.finished) {
            if (!started) { started = true; S.run(S.now, true); }
            else {
                if (S.wheel.empty()) return false;
                uint64_t t = S.wheel.begin()->first;
                if (t > maxTime) return false;
                if (t > limit) { why = "ran to time " + std::to_string(limit) + " (next event at " + std::to_string(t) + ")"; return true; }
                S.run(t, false);
            }
            if (S.finished) return false;
            for (size_t k = 0; k < stops.size(); k++) {
                Stop& st = stops[k];
                Val cur = S.get(st.sig);
                bool hit = false;
                if (!(cur == st.last)) {
                    int o = Sim::bit0(st.last), n = Sim::bit0(cur);
                    hit = st.kind == 0 || (st.kind == 1 && n == 1 && o != 1) || (st.kind == 2 && n == 0 && o != 0) || (st.kind == 3 && cur == st.want);
                }
                st.last = cur;
                if (hit && why.empty()) why = "stop " + std::to_string(k + 1) + ": " + st.name + " = " + fmt(cur);
            }
            if (!why.empty()) return true;
            if (slots > 0 && --slots == 0) { why = "step"; return true; }
        }
        return false;
    }
    // A value written from here takes effect in a slot at the current time.
    void settle() {
        if (!started) return;
        S.wheel[S.now];
        S.run(S.now, false);
    }

    void help() {
        std::cout << "run [T] | step [N] | stop SIG [posedge|negedge|== VALUE] | stop | delete N | print NAME ... |\n"
                     "force SIG VALUE | release SIG | deposit SIG VALUE | signals [TEXT] | time | quit\n";
    }

    // Returns when the simulation is over or the commands are.
    void loop(std::istream& in) {
        std::string ln;
        for (;;) {
            if (prompt) std::cout << "msim> " << std::flush;
            if (!std::getline(in, ln)) break;
            std::istringstream is(ln);
            std::vector<std::string> a;
            for (std::string t; is >> t;) a.push_back(t);
            if (a.empty() || a[0][0] == '#') continue;
            const std::string& c = a[0];
            auto sigArg = [&](size_t i) {
                int s = i < a.size() ? find(a[i]) : -1;
                if (s < 0) std::cout << (i < a.size() ? "no signal '" + a[i] + "'" : c + " needs a signal") << "\n";
                return s;
            };
            if (c == "quit" || c == "exit") { S.finished = true; std::cout << "quit at time " << S.now << "\n"; return; }
            if (c == "help") { help(); continue; }
            if (c == "time") { std::cout << "time " << S.now << "\n"; continue; }
            if (c == "run" || c == "step") {
                uint64_t limit = ~0ULL;
                long slots = -1;
                if (c == "step") slots = a.size() > 1 ? std::max(1L, std::atol(a[1].c_str())) : 1;
                else if (a.size() > 1 && a[1] != "-all") {
                    // run N, or with a unit as xrun scripts write it: run 100 ns, run 100ns
                    char* end = nullptr;
                    double n = std::strtod(a[1].c_str(), &end);
                    std::string unit = end && *end ? std::string(end) : a.size() > 2 ? a[2] : "";
                    static const std::map<std::string, int> ex = {{"s", 0}, {"ms", -3}, {"us", -6}, {"ns", -9}, {"ps", -12}, {"fs", -15}};
                    auto u = ex.find(unit);
                    double ticks = u == ex.end() ? n : n * std::pow(10.0, u->second - timeFmt().tickExp);
                    limit = S.now + (uint64_t)std::llround(ticks);
                }
                std::string why;
                if (!advance(limit, slots, why)) { std::cout << "simulation ended at time " << S.now << "\n"; return; }
                std::cout << "time " << S.now << ": " << why << "\n";
                continue;
            }
            if (c == "stop") {
                if (a.size() == 1) {
                    for (size_t k = 0; k < stops.size(); k++)
                        std::cout << k + 1 << ": " << stops[k].name
                                  << (stops[k].kind == 1 ? " posedge" : stops[k].kind == 2 ? " negedge" : stops[k].kind == 3 ? " == " + fmt(stops[k].want) : " (any change)") << "\n";
                    if (stops.empty()) std::cout << "no stops\n";
                    continue;
                }
                int s = sigArg(1);
                if (s < 0) continue;
                Stop st{s, shown(s), 0, {}, {}};
                if (a.size() > 2) {
                    if (a[2] == "posedge") st.kind = 1;
                    else if (a[2] == "negedge") st.kind = 2;
                    else if (a[2] == "==" && a.size() > 3 && parse(a[3], S.sigs[s].w, st.want)) st.kind = 3;
                    else { std::cout << "stop SIG [posedge|negedge|== VALUE]\n"; continue; }
                }
                stops.push_back(st);
                std::cout << "stop " << stops.size() << " set on " << st.name << "\n";
                continue;
            }
            if (c == "delete") {
                size_t n = a.size() > 1 ? (size_t)std::atol(a[1].c_str()) : 0;
                if (n < 1 || n > stops.size()) std::cout << "no stop " << (a.size() > 1 ? a[1] : "") << "\n";
                else { stops.erase(stops.begin() + (long)(n - 1)); std::cout << "deleted\n"; }
                continue;
            }
            if (c == "print") {
                if (a.size() < 2) std::cout << "print needs a name\n";
                for (size_t i = 1; i < a.size(); i++) {
                    if (printPath(a[i])) continue;
                    int s = find(a[i]);
                    if (s < 0) std::cout << "no signal '" << a[i] << "'\n";
                    else std::cout << shown(s) << " = " << fmt(S.get(s)) << "\n";
                }
                continue;
            }
            if (c == "force" || c == "deposit") {
                int s = sigArg(1);
                if (s < 0) continue;
                Val v;
                if (a.size() < 3 || !parse(a[2], S.sigs[s].w, v)) { std::cout << c << " SIG VALUE (a number such as 5, 8'hff or 'b1x; up to 64 bits)\n"; continue; }
                if (c == "force") S.holdOn(s, konst(v), true);
                else S.set(s, v);
                settle();
                std::cout << shown(s) << " = " << fmt(S.get(s)) << (c == "force" ? "  (forced)" : "") << "\n";
                continue;
            }
            if (c == "release") {
                int s = sigArg(1);
                if (s < 0) continue;
                S.holdOff(s, true);
                settle();
                std::cout << shown(s) << " released\n";
                continue;
            }
            if (c == "signals") {
                find("");
                std::vector<std::string> names;
                for (auto& [n, s] : byName)
                    if (!S.sigs[s].hide && (a.size() < 2 || (S.vcdTop + "." + n).find(a[1]) != std::string::npos)) names.push_back(S.vcdTop + "." + n);
                std::sort(names.begin(), names.end());
                for (size_t k = 0; k < names.size() && k < 200; k++) std::cout << names[k] << "\n";
                if (names.size() > 200) std::cout << "... " << names.size() - 200 << " more\n";
                continue;
            }
            std::cout << "unknown command '" << c << "' (help lists them)\n";
        }
        // No more commands: the rest of the run.
        std::string why;
        stops.clear();
        while (advance(~0ULL, -1, why)) why.clear();
    }
};

}   // namespace interactive
