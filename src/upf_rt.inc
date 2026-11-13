// upf_rt.inc - power-aware simulation for msim (IEEE 1801-2024 clause 9),
// included after the kernel (minisim.cpp) by msim.cpp.
//
// What it models:
//   * supply ports and nets, their connections, and power switches (9.2):
//     each supply subnet has a value {state, voltage}, set from the
//     testbench with supply_on / supply_off / supply_partial_on /
//     set_supply_state (package UPF, 11.2) or driven by a switch;
//   * supply set power states and simstates (9.3, 9.5); predefined ON / OFF
//     when a set has no states of its own;
//   * the primary supply's simstate applied to every process in a domain
//     (9.5, 9.6): CORRUPT puts the domain's processes to sleep and holds what
//     they drive at X; on power-up the holds are released, combinational
//     logic re-evaluates and flops keep X until written; the corrupt-on-
//     activity / on-change simstates corrupt writes (exec.inc upfWrite);
//   * isolation (9.8) and level shifters (9.9) at domain boundaries, as a
//     cell on the port connection between a parent and a child instance;
//   * retention (9.7): save and restore of the domain's flops;
//   * domain power states, -illegal states, and legacy PSTs, reported as
//     errors when the simulation reaches a state that is not legal.
#include <iomanip>
#include <type_traits>
#include "upf.hpp"

#ifdef UPF_RT_NO_DESIGN
namespace slang::ast { class Type; class Symbol; }
#endif

namespace upfrt {
using namespace upf;

// A binary archive of plain data for the msim -compile snapshot (Rt::snapshot):
// a(x, y, ...) writes or reads each argument; seq(v, f) a vector of structs.
namespace snap {
template <class D> struct Ar {
    template <class... T> void operator()(T&... x) { (io(x), ...); }
    template <class T, class F> void seq(std::vector<T>& v, F f) {
        size_t n = v.size();
        io(n);
        if (!D::out) v.assign(n, T{});
        for (auto& e : v) f(e);
    }
    D& d() { return static_cast<D&>(*this); }
    template <class T> requires std::is_arithmetic_v<T> || std::is_enum_v<T> void io(T& x) { d().raw(&x, sizeof x); }
    void io(std::string& s) {
        size_t n = s.size();
        io(n);
        if (!D::out) s.resize(n);
        d().raw(s.data(), n);
    }
    void io(Val& v) {
        io(v.v); io(v.x); io(v.w); io(v.z); io(v.sgn); io(v.real); io(v.str);
        size_t n = v.hi.size();
        io(n);
        if (!D::out) v.hi.resize(n);
        for (size_t k = 0; k < n; k++) { io(v.hi[k].v); io(v.hi[k].x); io(v.hi[k].z); }
    }
    void io(SupplyVal& v) { io(v.st); io(v.v); io(v.hasV); }
    template <class A, class B> void io(std::pair<A, B>& p) { io(p.first); io(p.second); }
    template <class T> void io(std::vector<T>& v) {
        size_t n = v.size();
        io(n);
        if (!D::out) v.assign(n, T{});
        for (auto& e : v) io(e);
    }
    template <class C> void assoc(C& c) {   // map, set, unordered_map
        constexpr bool kv = requires { typename C::mapped_type; };
        size_t n = c.size();
        io(n);
        if (D::out) {
            for (auto& e : c) {
                if constexpr (kv) {
                    std::pair<typename C::key_type, typename C::mapped_type> x(e.first, e.second);
                    io(x);
                } else {
                    typename C::key_type x = e;
                    io(x);
                }
            }
            return;
        }
        c.clear();
        for (size_t k = 0; k < n; k++) {
            if constexpr (kv) {
                std::pair<typename C::key_type, typename C::mapped_type> x;
                io(x);
                c.emplace(std::move(x.first), std::move(x.second));
            } else {
                typename C::key_type x;
                io(x);
                c.insert(std::move(x));
            }
        }
    }
    template <class K, class V, class Cm> void io(std::map<K, V, Cm>& c) { assoc(c); }
    template <class K, class V, class H> void io(std::unordered_map<K, V, H>& c) { assoc(c); }
    template <class K, class Cm> void io(std::set<K, Cm>& c) { assoc(c); }
};
struct Out : Ar<Out> {
    static constexpr bool out = true;
    std::ostream& o;
    explicit Out(std::ostream& s) : o(s) {}
    void raw(const void* p, size_t n) { o.write((const char*)p, (std::streamsize)n); }
};
struct In : Ar<In> {
    static constexpr bool out = false;
    std::istream& i;
    explicit In(std::istream& s) : i(s) {}
    void raw(void* p, size_t n) { i.read((char*)p, (std::streamsize)n); }
};
}   // namespace snap

struct SetRt {
    std::string state = "OFF";
    int sim = SS_CORRUPT;
    int sig = -1;          // toggles when the simstate changes (wakes strategy cells)
    bool known = false;
    std::string forced;    // a deferred state made active by set_power_state
};
struct DomRt {
    std::string name, set;
    int mode = SS_CORRUPT;
    bool applied = false;
    bool alwaysOn = false;
    bool region = false;   // elements explicitly powered by a supply set (6.15), not a domain
    std::vector<int> owned;
    std::string state;     // current power state of the domain (add_power_state -domain)
    std::string forced;
};
struct GroupRt { std::string name, state = "UNDEFINED", forced; };
struct Cell {               // an isolation or level-shifter cell on a port connection
    int kind = 0;           // 0 isolation, 1 level shifter, 2 repeater or a port corrupted with its supply
    std::string clamp;      // isolation: the clamp value
    const Isolation* iso = nullptr;
    std::vector<std::string> supplies;   // all must be NORMAL
    std::vector<int> senseLow;
    Val latch;
    bool latched = false;
    std::string port;
    int model = -1;         // isolation: use_interface_cell -force_function model (Rt::ifcEls), or -1
    long enables = 0;       // isolation: times it began to isolate (msim -upf_cov)
    bool isolating = false;
};
struct RetEl { int sig; Val saved; bool have = false; };
struct RetRt {
    const Retention* r = nullptr;
    int dom = -1;
    int saveSig = -1, restoreSig = -1;
    int lastSave = 2, lastRestore = 2;
    bool single = false, inRet = false, restoring = false;
    std::vector<RetEl> els;
};

struct Rt {
    Model m;
    DomainMap dm;
    SupplyGraph g;
    bool on = false, verbose = false;
    std::unordered_map<std::string, int> sigByName;
    std::map<std::string, SupplyVal> drv, val;                  // subnet root -> root driver value / current value
    std::map<std::string, std::vector<std::string>> swOf;       // subnet root -> switches driving it
    std::map<std::string, std::string> swState;
    std::map<std::string, SetRt> sets;
    std::vector<DomRt> doms;
    std::map<std::string, int> domIdx;
    std::vector<Cell> cells;
    std::vector<GroupRt> groups;
    std::map<std::string, int> groupIdx;
    std::map<std::string, std::string> lastState;   // object -> its last power state that is not UNDEFINED (9.4)
    std::vector<RetRt> rets;
    std::vector<std::pair<int, Edge>> watch;
    std::map<int, int> override;   // signal -> 0 X held, 1 retained value held
    std::set<std::string> reported;
    int chkSig = -1;
    uint64_t errors = 0;

    // ---------------------------------------------------------------- utils
    std::string tstr() const { return std::to_string(S.now); }
    void note(const std::string& s) {
        if (verbose) std::cout << "UPF: " << tstr() << ": " << s << "\n";
    }
    void error(const std::string& s) {
        errors++;
        std::cout << "UPF error: " << tstr() << ": " << s << "\n";
    }
    int sigOf(const std::string& upfName) const {
        auto it = sigByName.find(m.design(upfName));
        return it == sigByName.end() ? -1 : it->second;
    }
    static std::string scopeOfName(const std::string& n) {
        size_t s = n.rfind('/');
        return s == std::string::npos ? "" : n.substr(0, s);
    }
    std::string root(const std::string& n) { return g.find(n); }
    bool isSupply(const std::string& n) const {
        if (m.ports.count(n) || m.nets.count(n)) return true;
        size_t s = n.rfind('/');
        if (s != std::string::npos) {
            auto it = m.switches.find(n.substr(0, s));
            if (it != m.switches.end()) {
                std::string p = n.substr(s + 1);
                if (p == it->second.outPort) return true;
                for (auto& [ip, net] : it->second.ins) if (ip == p) return true;
            }
        }
        return false;
    }
    // A supply object named by the testbench: "VDD", "/VDD", "tb/u_dut/VDD",
    // "PD.primary.power". Returns the supply net or port, or "".
    std::string supplyByName(std::string n) const {
        for (char& c : n) if (c == '\\') c = '/';
        while (!n.empty() && n[0] == '/') n.erase(0, 1);
        for (;;) {
            if (isSupply(n)) return n;
            std::string f = m.fnNet(n);
            if (!f.empty()) return f;
            size_t s = n.find_first_of("/.");
            if (s == std::string::npos) return "";
            n = n.substr(s + 1);
        }
    }
    EVal logicOf(int s) const {
        EVal r;
        Val v = S.get(s);
        r.v = v.v & ~(v.x | v.z);
        r.x = v.x | v.z;
        return r;
    }
    EVal supplyOf(const std::string& net) {
        EVal r;
        r.k = EVal::Supply;
        auto it = val.find(root(net));
        if (it != val.end()) r.sv = it->second;
        // set_partial_on_translation (6.50): before any expression or PST sees it
        if (r.sv.st == S_PARTIAL_ON && !m.partialOn.empty()) r.sv.st = m.partialOn == "OFF" ? S_OFF : S_FULL_ON;
        return r;
    }
    // Names in power state expressions: a function of the set itself, a
    // supply net or port, a supply set function, a domain or supply set (a
    // power state comparison), or a logic net.
    EvalCtx ctx(const std::string& scope, const SupplySet* self) {
        EvalCtx c;
        c.id = [this, scope, self](const std::string& id, EVal& out) {
            if (self) {
                auto f = self->fn.find(id);
                if (f != self->fn.end()) { out = supplyOf(f->second); return true; }
            }
            std::string full = Model::join(scope, id);
            for (const std::string& n : {full, id}) {
                if (isSupply(n)) { out = supplyOf(n); return true; }
                std::string f = m.fnNet(n);
                if (!f.empty()) { out = supplyOf(f); return true; }
                if (m.domains.count(n) || m.groups.count(n) || m.sets.count(m.setOf(n))) {
                    out.k = EVal::Obj;
                    out.sym = m.domains.count(n) || m.groups.count(n) ? n : m.setOf(n);
                    return true;
                }
                int s = sigOf(n);
                if (s >= 0) { out = logicOf(s); return true; }
            }
            return false;
        };
        c.inState = [this](const std::string& obj, const std::string& st) {
            auto d = domIdx.find(obj);
            if (d != domIdx.end()) return doms[(size_t)d->second].state == st;
            auto gi = groupIdx.find(obj);
            if (gi != groupIdx.end()) return groups[(size_t)gi->second].state == st;
            auto it = sets.find(obj);
            return it != sets.end() && it->second.state == st;
        };
        return c;
    }

    // --------------------------------------------------- overrides of signals
    // Hold s at X (corrupt), at a retained value, or release it.
    void hold(int s, const Val& v, int kind) {
        override[s] = kind;
        S.holdOn(s, konst(v), true);
    }
    void corrupt(int s) { hold(s, allx(S.sigs[(size_t)s].w), 0); }
    void release(int s) {
        auto it = override.find(s);
        if (it == override.end()) return;
        override.erase(it);
        S.holdOff(s, true);
    }

    // ------------------------------------------- models, regions, behavior
    // The module of an instance (simulator path); "" for a generate block.
    std::string modelOf(const std::string& path) const {
        std::string full = S.vcdTop.empty() ? path : path.empty() ? S.vcdTop : S.vcdTop + "." + path;
        auto it = instCells().find(full);
        return it == instCells().end() ? std::string() : it->second;
    }
    // The supply set an instance is explicitly connected to (6.15), or
    // null: the closest element above it wins, and the later command.
    const SupplyConn* explicitConn(const std::string& path) const {
        const SupplyConn* best = nullptr;
        size_t len = 0;
        for (auto& sc : m.supplyConns) {
            bool out = false;
            for (auto& x : sc.exclude) if (DomainMap::within(path, m.design(x))) out = true;
            if (out) continue;
            for (auto& e : sc.elements) {
                std::string d = m.design(e);
                bool in = sc.transitive ? DomainMap::within(path, d) : path == d;
                if (in && (!best || d.size() >= len)) { best = &sc; len = d.size(); }
            }
        }
        return best;
    }
    std::map<std::string, int> regionIdx;
    int regionOf(const std::string& set) {
        auto it = regionIdx.find(set);
        if (it != regionIdx.end()) return it->second;
        DomRt d;
        d.name = "supply set " + set;
        d.set = m.setOf(set);
        d.region = true;
        auto st = m.sets.find(d.set);
        d.alwaysOn = st == m.sets.end() || st->second.fn.empty();
        d.mode = d.alwaysOn ? SS_NORMAL : SS_CORRUPT;
        int idx = (int)doms.size();
        regionIdx[set] = idx;
        doms.push_back(d);
        g_pdMode.push_back(d.mode);
        g_pdSig.push_back(S.sig("$upf.ss_region." + set, 1));
        return idx;
    }
    // The supply set that powers an instance: an explicit connection, else
    // its domain's primary supply; "" outside every domain.
    std::string supplySetOf(const std::string& path) const {
        if (const SupplyConn* sc = explicitConn(path)) return m.setOf(sc->set);
        std::string dn = dm.of(path);
        return dn.empty() ? std::string() : m.primaryOf(dn);
    }
    // The simstate behavior of an instance (6.56): ENABLE, SIMSTATE_ONLY,
    // PORT_CORR_ONLY or DISABLE, and the instance the rule was given for
    // (the macro whose ports are corrupted).
    std::pair<std::string, std::string> behaviorOf(const std::string& path) const {
        std::string mode, at;
        int best = -1;
        auto take = [&](int rank, const std::string& md, const std::string& inst) {
            if (rank >= best) { best = rank; mode = md; at = inst; }
        };
        // every instance from the top down to path
        std::vector<std::string> chain{""};
        for (size_t i = 0; i <= path.size(); i++)
            if (i == path.size() || path[i] == '.') { if (i) chain.push_back(path.substr(0, i)); }
        for (auto& a : chain) {
            std::string mod = modelOf(a);
            if (mod.empty() && !a.empty()) continue;
            // attributes: the lowest precedence
            std::string av = m.designAttr(a, mod, "UPF_simstate_behavior");
            if (!av.empty()) take(1, av, a);
            else if (m.designAttr(a, mod, "UPF_is_power_aware_model") == "TRUE") take(0, "DISABLE", a);
        }
        for (auto& sb : m.simBehaviors) {
            if (!sb.models.empty() || !sb.lib.empty()) {
                for (auto& a : chain) {
                    std::string mod = modelOf(a);
                    if (mod.empty()) continue;
                    if (!sb.models.empty() && !Model::modelIn(sb.models, mod)) continue;
                    bool in = sb.elements.empty();
                    auto rel = [&](const std::string& e) {
                        std::string d = e;
                        for (char& ch : d) if (ch == '/') ch = '.';
                        return a.empty() ? d : a + "." + d;
                    };
                    for (auto& e : sb.elements) if (DomainMap::within(path, rel(e))) in = true;
                    for (auto& e : sb.exclude) if (DomainMap::within(path, rel(e))) in = false;
                    if (in) take(sb.elements.empty() ? 2 : 3, sb.mode, a);
                }
            } else {
                bool in = false;
                std::string at2;
                for (auto& e : sb.elements) if (DomainMap::within(path, m.design(e))) { in = true; at2 = m.design(e); }
                for (auto& e : sb.exclude) if (DomainMap::within(path, m.design(e))) in = false;
                if (in) take(3, sb.mode, at2);
            }
        }
        if (mode == "PORT_CORR_ONLY" && m.designAttr(at, modelOf(at), "UPF_is_hard_macro") != "TRUE")
            throw Error("set_simstate_behavior PORT_CORR_ONLY for " + (at.empty() ? std::string("the top") : at) +
                        ", which is not a hard macro (UPF_is_hard_macro, 6.56)");
        return {mode.empty() ? std::string("ENABLE") : mode, at};
    }

    // ----------------------------------------------------------- bind
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bind(LowerResult& lw) {
        on = true;
        g_upf = true;
        dm.build(m);
        expandPgConnects();
        g.build(m);
        for (size_t i = 0; i < S.sigs.size(); i++) sigByName.emplace(S.sigs[i].name, (int)i);
        for (auto& w : m.warnings) std::cerr << "msim: UPF: " << w << "\n";
        for (auto& e : ifcEls) {   // use_interface_cell -force_function (6.62)
            auto a = sigByName.find(e.d), b = sigByName.find(e.y);
            if (a == sigByName.end() || b == sigByName.end())
                throw Error("use_interface_cell: no signal " + (a == sigByName.end() ? e.d : e.y) + " for the isolation model");
            e.dSig = a->second;
            e.ySig = b->second;
        }
        for (auto& e : retModelEls) {   // map_retention_cell -lib_model_name (6.39)
            auto a = sigByName.find(e.el), b = sigByName.find(e.q);
            if (a == sigByName.end() || b == sigByName.end())
                throw Error("map_retention_cell: no signal " + (a == sigByName.end() ? e.el : e.q) + " for the retention model");
            e.elSig = a->second;
            e.qSig = b->second;
        }
        // supply subnets: every supply port, net and switch port; switches
        // driving a subnet
        std::set<std::string> all(m.ports.begin(), m.ports.end());
        all.insert(m.nets.begin(), m.nets.end());
        for (auto& [n, sw] : m.switches) {
            if (!sw.outPort.empty()) {
                all.insert(n + "/" + sw.outPort);
                swOf[root(n + "/" + sw.outPort)].push_back(n);
            }
            for (auto& [p, net] : sw.ins) all.insert(n + "/" + p);
            swState[n] = "";
        }
        for (auto& n : all) val[root(n)] = SupplyVal{};
        for (auto& [r, v] : swOf)
            if (v.size() > 1) {
                std::string res;
                for (auto& n : all) if (root(n) == r && m.resolve.count(n)) res = m.resolve.at(n);
                if (res.empty() || res == "unresolved")
                    throw Error("supply net driven by " + std::to_string(v.size()) + " switches (" + v[0] + ", " + v[1] +
                                ") is not resolved; use create_supply_net -resolve parallel (9.1 b)");
            }
        for (auto& [n, s] : m.sets) sets[n];
        for (auto& [n, s] : sets) s.sig = S.sig("$upf.ss." + n, 1);
        // domains
        for (auto& dn : m.domainOrder) {
            DomRt d;
            d.name = dn;
            d.set = m.primaryOf(dn);
            auto it = m.sets.find(d.set);
            d.alwaysOn = it == m.sets.end() || it->second.fn.empty();
            d.mode = d.alwaysOn ? SS_NORMAL : SS_CORRUPT;
            domIdx[dn] = (int)doms.size();
            doms.push_back(d);
            g_pdMode.push_back(d.mode);
            g_pdSig.push_back(S.sig("$upf.pd." + dn, 1));
        }
        for (auto& e : m.domainOrder)
            for (auto& el : m.domains.at(e).elements) {
                std::string p = m.design(el);
                bool found = p.empty();
                for (auto& [n, id] : sigByName) {
                    (void)id;
                    if (DomainMap::within(n, p) && n.size() > p.size()) { found = true; break; }
                }
                if (!found) std::cerr << "msim: UPF: " << m.domains.at(e).where << ": power domain " << e
                                      << ": no instance '" << p << "' in the design\n";
            }
        for (auto& gn : m.groupOrder) {
            groupIdx[gn] = (int)groups.size();
            groups.push_back({gn, "UNDEFINED", ""});
        }
        // every state a transition names is a power state of its object (6.7)
        for (auto& [obj, ts] : m.transitions) {
            std::set<std::string> names;
            const std::vector<PowerState>* st = ts.kind == 'g' ? &m.groups.at(obj).states
                                              : ts.kind == 'd' ? &m.domains.at(obj).states : &m.sets.at(obj).states;
            for (auto& ps : *st) names.insert(ps.name);
            if (ts.kind == 's') { names.insert("ON"); names.insert("OFF"); }
            for (auto& t : ts.tr) {
                auto chk = [&](const std::string& sn) {
                    if (!names.count(sn)) throw Error(t.where + ": transition " + t.name + ": " + sn + " is not a power state of " + obj + " (6.7)");
                };
                for (auto& a : t.from) chk(a);
                for (auto& b : t.to) chk(b);
                for (auto& [a, b] : t.paired) { chk(a); chk(b); }
            }
        }
        for (auto& e : intentErrors(m, dm, [this](const std::string& p) { return modelOf(p); },
                                        [this](const std::string& n) { return sigOf(n) >= 0; }))
            throw Error(e.where + ": " + e.text);
        for (auto& sb : m.simBehaviors)
            if (sb.loaded)
                for (auto& mm : sb.models)
                    if (instancesOf(mm.substr(mm.find('.') + 1)).empty())
                        throw Error(sb.where + ": load_simstate_behavior: model " + mm + " is not in the design (6.34)");
        bindLogic(lw);
        bindTypes(lw);
        // processes: domain, state or not; what each domain drives
        std::vector<std::set<int>> owned(doms.size());
        stateSigs.assign(doms.size(), {});
        for (auto& lp : lw.procs) {
            if (!lp.port.empty() || lp.kind == "upf_wire") continue;   // a port connection is a wire, not logic of either side
            // slang names procedure kinds "Initial", "AlwaysFF", ...; lowering adds "assign", "init", "sampled"
            const std::string& k = lp.kind;
            if (k == "Initial" || k == "Final" || k == "init" || k == "sampled") continue;
            if (inRetModel(lp.scope)) continue;   // a -lib_model_name or -force_function model is not corrupted (6.39, 6.62)
            std::string dn = dm.of(lp.scope);
            // set_simstate_behavior (6.56): DISABLE and PORT_CORR_ONLY leave the logic alone
            auto [beh, behAt] = behaviorOf(lp.scope);
            if (beh == "DISABLE" || beh == "PORT_CORR_ONLY") continue;
            int pd;
            if (const SupplyConn* sc = explicitConn(lp.scope)) pd = regionOf(sc->set);   // connect_supply_set (6.15)
            else if (!dn.empty()) pd = domIdx.at(dn);
            else continue;
            if (owned.size() < doms.size()) { owned.resize(doms.size()); stateSigs.resize(doms.size()); }
            // sim_corruption_control -type process (6.59): the process is not corrupted
            bool keepProc = false;
            for (auto& cc : m.corrCtls)
                if (cc.type == "process" && selected(cc.sel, lp.scope, true)) keepProc = true;
            if (keepProc) continue;
            bool st = k == "AlwaysFF" || k == "AlwaysLatch";
            if (k == "Always")
                for (auto& in : lp.prog.code)
                    if (in.k == IK::WaitEdge) {
                        if (in.events.size() > 1) { for (auto& [s, e] : in.events) if (e != ANY) st = true; }
                        else if (in.edge != ANY) st = true;
                    }
            std::set<int> wr;
            std::vector<const Prog*> seen;
            std::function<void(const Prog&)> writes = [&](const Prog& p) {
                for (const Prog* q : seen) if (q == &p) return;
                seen.push_back(&p);
                for (auto& in : p.code) {
                    if ((in.k == IK::Assign || in.k == IK::Nba) && in.sig >= 0 && !isRel(in.sig)) {
                        if (in.arr) for (int k = 0; k < in.arr->count; k++) wr.insert(in.arr->base + k);
                        else wr.insert(in.sig);
                    }
                    if (in.k == IK::Call && in.callee) writes(*in.callee);
                    for (const Prog* b : in.branches) writes(*b);
                }
            };
            writes(lp.prog);
            // sim_corruption_control (6.59): what the process drives that is
            // not to be corrupted; a process that drives nothing else runs on
            size_t kept = 0;
            for (int s : wr)
                if (noCorruption(s, st)) { noCorr.insert(s); kept++; }
            if (!wr.empty() && kept == wr.size()) continue;
            lp.prog.pd = pd;
            lp.prog.pstate = st;
            // Level-sensitive logic re-evaluates at power-up (9.5.3) even when
            // no input changed while the domain was off: it also waits on the
            // domain's power-up signal.
            if (!st)
                for (auto& in : lp.prog.code)
                    if (in.k == IK::WaitEdge) {
                        std::vector<std::pair<int, Edge>> ev = in.events.size() > 1 ? in.events : std::vector<std::pair<int, Edge>>{{in.sig, in.edge}};
                        bool level = std::all_of(ev.begin(), ev.end(), [](auto& x) { return x.second == ANY; });
                        if (!level || in.sig < 0 && in.events.empty()) continue;
                        ev.push_back({g_pdSig[(size_t)pd], ANY});
                        in = waitInstr(ev);
                    }
            for (int s : wr) owned[(size_t)pd].insert(s);
            // latches (set_retention -applies_to latch, 6.54): always_latch,
            // and a level-sensitive always that leaves a variable unassigned
            // on some path from one wait to the next (an inferred latch)
            std::set<int> lat;
            if (k == "AlwaysLatch")
                for (auto& in : lp.prog.code)
                    if ((in.k == IK::Nba || in.k == IK::Assign) && in.sig >= 0 && !isRel(in.sig) && !in.arr) lat.insert(in.sig);
            if (k == "Always" && !st) lat = inferredLatches(lp.prog);
            if (!lat.empty()) {
                for (int s : lat) {
                    stateSigs[(size_t)pd].insert(s);
                    latchSigs.insert(s);
                    if (doms[(size_t)pd].region && !dn.empty()) stateSigs[(size_t)domIdx.at(dn)].insert(s);
                }
            }
            if (st && k != "AlwaysLatch") {
                std::set<int> w;
                for (auto& in : lp.prog.code)
                    if ((in.k == IK::Nba || in.k == IK::Assign) && in.sig >= 0 && !isRel(in.sig) && !in.arr) w.insert(in.sig);
                for (int s : w) stateSigs[(size_t)pd].insert(s);
                // a domain's retention strategies also reach the elements on another supply
                if (doms[(size_t)pd].region && !dn.empty())
                    for (int s : w) stateSigs[(size_t)domIdx.at(dn)].insert(s);
            }
        }
        for (auto& e : retModelEls)
            for (auto& o : owned) o.erase(e.elSig);
        for (auto& st2 : stateSigs)
            for (auto& e : retModelEls) st2.erase(e.elSig);
        for (size_t d = 0; d < doms.size(); d++) {
            for (int s : owned[d])
                if (S.sigs[(size_t)s].name.find('$') == std::string::npos && !noCorr.count(s)) doms[d].owned.push_back(s);
        }
        bindCells(lw);
        bindRetention();
        // a retained register is corrupted after all (6.59)
        for (auto& r : rets)
            for (auto& e : r.els)
                if (noCorr.count(e.sig)) {
                    std::cerr << "msim: UPF: sim_corruption_control: " << S.sigs[(size_t)e.sig].name << " is retained by "
                              << r.r->name << "; its corruption is not turned off (6.59)\n";
                    noCorr.erase(e.sig);
                    if (r.dom >= 0) doms[(size_t)r.dom].owned.push_back(e.sig);
                }
        bindAsserts();
        bindReplays(lw);
        // logic signals the runtime reacts to
        std::set<int> w;
        auto addIds = [&](const ExP& e, const std::string& scope) {
            std::vector<std::string> ids;
            exprIds(e, ids);
            for (auto& id : ids) {
                int s = sigOf(Model::join(scope, id));
                if (s < 0) s = sigOf(id);
                if (s >= 0) w.insert(s);
            }
        };
        for (auto& [n, sw] : m.switches)
            for (auto& [p, net] : sw.ctrls) {
                if (net.empty()) continue;
                int s = sigOf(net);
                if (s < 0) throw Error(sw.where + ": power switch " + n + ": no control signal '" + m.design(net) + "' in the design");
                w.insert(s);
            }
        for (auto& [n, s] : m.sets)
            for (auto& ps : s.states) addIds(ps.logicExpr, scopeOfName(n));
        for (auto& dn : m.domainOrder)
            for (auto& ps : m.domains.at(dn).states) addIds(ps.logicExpr, m.domains.at(dn).scope);
        for (auto& gn : m.groupOrder)
            for (auto& ps : m.groups.at(gn).states) addIds(ps.logicExpr, m.groups.at(gn).scope);
        for (auto& r : rets) {
            if (r.saveSig >= 0) w.insert(r.saveSig);
            if (r.restoreSig >= 0) w.insert(r.restoreSig);
            addIds(r.r->saveCond, scopeOfName(r.r->domain));
            addIds(r.r->restoreCond, scopeOfName(r.r->domain));
            addIds(r.r->restorePeriodCond, scopeOfName(r.r->domain));
            addIds(r.r->powerdownCond, scopeOfName(r.r->domain));
        }
        for (auto& ac : m.assertCtls) addIds(ac.ctlExpr, ac.sel.scope);
        bindHdlPorts(lw);
        for (auto& h : hdlPorts) {
            if (h.hdl2upf) w.insert(h.sig);
            if (h.copyFrom >= 0) w.insert(h.copyFrom);
        }
        for (auto& e : retModelEls) w.insert(e.qSig);
        for (int s : w) watch.push_back({s, ANY});
        chkSig = S.sig("$upf.check", 1);
        g_upfHook = [](int op, const std::vector<Val>& a) { return rt().hook(op, a); };
    }
#endif
    // Low-power coverage (msim -upf_cov): power states and named transitions
    // reached, isolation cells enabled, retention saves and restores.
    std::string covPath;
    std::map<std::string, std::map<std::string, long>> stateHits;   // object -> state -> times entered
    std::map<std::string, std::map<std::string, long>> transHits;   // object -> transition name -> times
    std::map<std::string, std::pair<long, long>> retHits;           // strategy -> {saves, restores}
    bool writeCoverage() const {
        std::ofstream f(covPath);
        if (!f) return false;
        int bins = 0, hit = 0;
        std::ostringstream body;
        auto row = [&](const std::string& what, long n) {
            bins++;
            if (n > 0) hit++;
            body << "  " << std::left << std::setw(56) << what << " " << std::right << std::setw(6) << n << (n ? "" : "  <- not hit") << "\n";
        };
        auto states = [&](const char* kind, const std::string& obj, const std::vector<PowerState>& ps, bool onOff) {
            std::vector<std::string> names;
            if (onOff) names = {"ON", "OFF"};
            for (auto& p : ps)
                if (!p.illegal && std::find(names.begin(), names.end(), p.name) == names.end()) names.push_back(p.name);
            auto it = stateHits.find(obj);
            for (auto& n : names) row(std::string(kind) + " " + obj + " " + n, it == stateHits.end() || !it->second.count(n) ? 0 : it->second.at(n));
        };
        body << "power states\n";
        for (auto& [n, ss] : m.sets) if (!ss.states.empty() || m.transitions.count(n)) states("supply set", n, ss.states, true);
        for (auto& dn : m.domainOrder) states("power domain", dn, m.domains.at(dn).states, false);
        for (auto& [n, g] : m.groups) states("power state group", n, g.states, false);
        body << "state transitions\n";
        for (auto& [obj, ts] : m.transitions)
            for (auto& t : ts.tr) {
                if (t.illegal) continue;
                auto it = transHits.find(obj);
                row(obj + " " + t.name, it == transHits.end() || !it->second.count(t.name) ? 0 : it->second.at(t.name));
            }
        body << "isolation enables\n";
        for (auto& c : cells) if (c.kind == 0) row(c.iso->name + " " + c.port, c.enables);
        body << "retention\n";
        for (auto& r : rets) {
            auto it = retHits.find(r.r->name);
            row(r.r->name + " save", it == retHits.end() ? 0 : it->second.first);
            row(r.r->name + " restore", it == retHits.end() ? 0 : it->second.second);
        }
        f << "UPF low-power coverage: " << hit << " of " << bins << " bins";
        if (bins) f << " (" << std::fixed << std::setprecision(1) << 100.0 * hit / bins << "%)";
        f << "\n" << body.str();
        return true;
    }
    std::vector<std::set<int>> stateSigs;
    std::set<int> latchSigs;   // the state elements that are latches
    // The variables a level-sensitive process does not assign on every path
    // from one event wait to the next: they hold their value, as a latch.
    std::set<int> inferredLatches(const Prog& p) {
        size_t N = p.code.size();
        std::set<int> wr, out;
        for (auto& in : p.code)
            if ((in.k == IK::Assign || in.k == IK::Nba) && in.sig >= 0 && !isRel(in.sig) && !in.arr && in.drv < 0 && in.gate < 0)
                wr.insert(in.sig);
        if (wr.empty()) return out;
        for (auto& in : p.code)   // only plain code: a call, fork or delay is not followed
            if (in.k == IK::Call || in.k == IK::Fork || in.k == IK::WaitDelay || in.k == IK::Force) return out;
        std::vector<std::optional<std::set<int>>> st(N + 1);
        std::vector<size_t> work;
        auto flow = [&](size_t j, const std::set<int>& v) {
            if (j > N) return;
            if (!st[j]) { st[j] = v; work.push_back(j); return; }
            std::set<int> x;
            std::set_intersection(st[j]->begin(), st[j]->end(), v.begin(), v.end(), std::inserter(x, x.begin()));
            if (x.size() != st[j]->size()) { st[j] = x; work.push_back(j); }
        };
        for (size_t i = 0; i < N; i++)   // from each wait (not from the start: nothing ran before the first wait)
            if (p.code[i].k == IK::WaitEdge) flow(i + 1, {});
        while (!work.empty()) {
            size_t i = work.back();
            work.pop_back();
            std::set<int> v = *st[i];
            if (i == N) continue;
            const Instr& in = p.code[i];
            switch (in.k) {
                case IK::WaitEdge:
                    for (int s : wr) if (!v.count(s)) out.insert(s);
                    break;
                case IK::Jmp: flow(in.target, v); break;
                case IK::JmpIfNot:
                    flow(in.target, v);
                    if (in.xt != Instr::NOX) flow(in.xt, v);
                    flow(i + 1, v);
                    break;
                case IK::Finish: break;
                case IK::Assign:
                case IK::Nba:
                    if (in.sig >= 0 && !in.arr) v.insert(in.sig);
                    flow(i + 1, v);
                    break;
                default: flow(i + 1, v);
            }
        }
        return out;
    }

    // ------------------------------------- simulation controls (6.58, 6.59)
    std::set<int> noCorr;   // signals sim_corruption_control keeps from corruption
    std::set<int> realSigs, intSigs;   // variables of type real / shortreal, and of a predefined integer type
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bindTypes(LowerResult& lw) {
        if (m.corrCtls.empty()) return;
        for (auto& [sym, sid] : lw.sigOf) {
            if (!sym || !slang::ast::ValueSymbol::isKind(sym->kind)) continue;
            const auto& t = sym->as<slang::ast::ValueSymbol>().getType().getCanonicalType();
            if (t.isFloating()) realSigs.insert(sid);
            else if (t.kind == slang::ast::SymbolKind::PredefinedIntegerType) intSigs.insert(sid);
        }
    }
#endif
    // Instances of a model (5.8), as design paths.
    std::vector<std::string> instancesOf(const std::string& model) const {
        std::vector<std::string> r;
        std::string pre = S.vcdTop.empty() ? "" : S.vcdTop + ".";
        for (auto& [full, mod] : instCells()) {
            if (mod != model) continue;
            if (!pre.empty() && full.compare(0, pre.size(), pre) != 0) continue;
            r.push_back(full.substr(pre.size()));
        }
        return r;
    }
    // Whether a control selects a design element: an instance (inst) or a
    // signal or assertion in one.
    bool selected(const ElemSel& sel, const std::string& path, bool inst) const {
        std::string container = inst ? path : parentOf(path);
        if (!sel.domain.empty() && dm.of(container) != sel.domain) return false;
        auto hit = [&](const std::string& e) {
            if (path == e) return true;
            return sel.transitive ? DomainMap::within(path, e) : container == e;
        };
        std::vector<std::string> bases;
        if (sel.model.empty()) bases.push_back("");
        else bases = instancesOf(sel.model);
        for (auto& b : bases) {
            auto name = [&](const std::string& e) {
                if (sel.model.empty()) return m.design(e);
                std::string d = e;
                for (char& ch : d) if (ch == '/') ch = '.';
                return b.empty() ? d : b + "." + d;
            };
            bool in = false;
            if (!sel.elements.empty()) {
                for (auto& e : sel.elements) if (hit(name(e))) in = true;
            } else if (!sel.model.empty()) in = hit(b);
            else if (!sel.domain.empty()) in = true;
            else in = hit(m.design(sel.scope));
            if (!in) continue;
            bool out = false;
            for (auto& e : sel.exclude) if (hit(name(e))) out = true;
            if (!out) return true;
        }
        return false;
    }
    // Whether sim_corruption_control keeps signal s (written by a state
    // process when st) from corruption.
    bool noCorruption(int s, bool st) const {
        if (m.corrCtls.empty()) return false;
        const auto& sg = S.sigs[(size_t)s];
        bool isPort = false;
        if (!S.vcdTop.empty()) isPort = portRecs().count(S.vcdTop + "." + sg.name) != 0;
        for (auto& cc : m.corrCtls) {
            const std::string& t = cc.type;
            bool kind = t == "real" ? realSigs.count(s) != 0
                      : t == "integer" ? intSigs.count(s) != 0
                      : t == "seq" ? st : t == "comb" ? !st : t == "port" ? isPort : false;
            if (kind && selected(cc.sel, sg.name, false)) return true;
        }
        return false;
    }
    // sim_assertion_control: each assertion it selects, and what controls it.
    struct AsrtCtlRt {
        const AssertCtl* r;
        int id;
        int dom = -1;          // the controlling domain; -1: the expression
        bool active = false, dead = false;
    };
    std::vector<AsrtCtlRt> asrtCtls;
    std::string designOf(const std::string& simPath) const {
        std::string pre = S.vcdTop.empty() ? "" : S.vcdTop + ".";
        if (!pre.empty() && simPath.compare(0, pre.size(), pre) == 0) return simPath.substr(pre.size());
        return simPath;
    }
    void bindAsserts() {
        auto& as = asserts();
        for (auto& ac : m.assertCtls)
            for (size_t i = 0; i < as.size(); i++) {
                const AssertInfo& a = as[i];
                if (a.hidden) continue;
                std::string inst = designOf(a.inst), name = designOf(a.name);
                if (!selected(ac.sel, name, false) && !selected(ac.sel, inst, true)) continue;
                AsrtCtlRt x;
                x.r = &ac;
                x.id = (int)i;
                if (!ac.ctlExpr) {
                    if (!ac.ctlDomain.empty()) x.dom = domIdx.at(ac.ctlDomain);
                    else if (const SupplyConn* sc = explicitConn(inst)) x.dom = regionOf(sc->set);
                    else {
                        std::string dn = dm.of(inst);
                        if (dn.empty()) continue;   // in no domain: never off
                        x.dom = domIdx.at(dn);
                    }
                }
                note("sim_assertion_control -type " + ac.type + " on " + name);
                asrtCtls.push_back(x);
            }
    }
    // sim_replay_control (6.60): the initial blocks to run again, by the
    // domain whose power-up replays them.
    std::vector<std::pair<const Prog*, int>> replays;
    // msim -compile: the replayed processes by number (Rt::snapshot), and
    // simv's compiled processes, proc<n> (codegen.hpp).
    std::vector<int> replayProc;
    Task (*const* procFns)() = nullptr;
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bindReplays(LowerResult& lw) {
        std::map<std::string, const ReplayCtl*> byModel;   // instances a -model command covers
        for (auto& rc : m.replayCtls)
            if (!rc.sel.model.empty())
                for (auto& i : instancesOf(rc.sel.model)) byModel.emplace(i, &rc);
        for (auto& rc : m.replayCtls)
            if (rc.sel.model.empty())
                for (auto& e : rc.sel.elements) {
                    auto it = byModel.find(m.design(e));
                    if (it != byModel.end())
                        throw Error(rc.where + ": sim_replay_control: " + m.design(e) + " is already covered by sim_replay_control -model " +
                                    it->second->sel.model + " (" + it->second->where + ", 6.60)");
                }
        for (auto& lp : lw.procs) {
            if (lp.kind != "Initial") continue;
            for (auto& rc : m.replayCtls) {
                if (!selected(rc.sel, lp.scope, true)) continue;
                int d = -1;
                if (!rc.ctlDomain.empty()) d = domIdx.at(rc.ctlDomain);
                else if (const SupplyConn* sc = explicitConn(lp.scope)) d = regionOf(sc->set);
                else if (std::string dn = dm.of(lp.scope); !dn.empty()) d = domIdx.at(dn);
                if (d < 0) continue;
                replays.push_back({&lp.prog, d});
                note("sim_replay_control: an initial block of " + lp.scope + " replays when " + doms[(size_t)d].name + " powers up");
                break;
            }
        }
    }
#endif
    void applyAsserts() {
        for (auto& x : asrtCtls) {
            if (x.dead) continue;
            bool act = x.dom >= 0 ? g_pdMode[(size_t)x.dom] != SS_NORMAL
                                  : truthOf(evalEx(x.r->ctlExpr, ctx(x.r->sel.scope, nullptr), "sim_assertion_control -control_expr")) == 1;
            if (act == x.active) continue;
            x.active = act;
            AssertInfo& a = asserts()[(size_t)x.id];
            const std::string& t = x.r->type;
            if (act) {
                a.on = false;
                if (t != "suspend") a.killGen++;   // reset and kill end the attempts in progress
                if (t == "kill") x.dead = true;
            } else a.on = true;
            note("assertion " + designOf(a.name) + (act ? (t == "kill" ? " killed" : t == "reset" ? " reset and suspended" : " suspended")
                                                        : " enabled again"));
        }
    }

    // Logic ports and nets made by UPF and their connections (6.13, 6.19,
    // 6.20): each connection is one more continuous assignment, from the
    // source side to the sink side. A connection to a port of a descendant
    // instance is a port connection like those of the HDL, so the interface
    // cell strategies see it (4.5.5).
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bindLogic(LowerResult& lw) {
        auto make1 = [&](const std::string& upfName) {
            std::string dn = m.design(upfName);
            auto it = sigByName.find(dn);
            if (it != sigByName.end()) return it->second;
            int s = S.sig(dn, 1);
            sigByName.emplace(dn, s);
            return s;
        };
        for (auto& n : m.logicNets) make1(n);
        for (auto& [n, d] : m.logicPorts) make1(n);
        std::string top = S.vcdTop;
        for (auto& lc : m.logicConns) {
            int net = sigOf(lc.net);
            if (net < 0)
                throw Error(lc.where + ": connect_logic_net: no logic net '" + m.design(lc.net) + "' in the design or made by create_logic_net (6.13 a)");
            std::string dn = m.design(lc.port);
            int port = sigOf(lc.port);
            if (port < 0)
                throw Error(lc.where + ": connect_logic_net: no port '" + dn + "' in the design or made by create_logic_port");
            int dir = -1;
            auto lp = m.logicPorts.find(lc.port);
            if (lp != m.logicPorts.end()) dir = lp->second == "in" ? 0 : lp->second == "out" ? 1 : 2;
            else {
                auto pr = portRecs().find(top.empty() ? dn : top + "." + dn);
                if (pr == portRecs().end())
                    throw Error(lc.where + ": connect_logic_net: '" + dn + "' is not a port (6.13)");
                dir = pr->second.dir;
            }
            if (S.sigs[(size_t)port].w != 1 || S.sigs[(size_t)net].w != 1)
                throw Error(lc.where + ": connect_logic_net: " + dn + " and " + m.design(lc.net) + " must both be one bit of logic (6.13)");
            if (dir == 2) throw Error(lc.where + ": connect_logic_net: inout port " + dn + " is not supported");
            // the port is on the interface of the net's scope (LowConn) or
            // on an instance below it (HighConn)
            size_t sl = lc.port.rfind('/');
            std::string owner = sl == std::string::npos ? "" : lc.port.substr(0, sl);
            bool low = owner == lc.scope;
            // LowConn of an input and HighConn of an output are sources (6.20)
            bool portDrives = low ? dir == 0 : dir == 1;
            if (!low) {
                // what the HDL already connects to the HighConn
                for (auto& old : lw.procs) {
                    if (old.port != dn) continue;
                    bool same = false, constant = true;
                    for (auto& in : old.prog.code) {
                        if (in.k == IK::WaitEdge) {
                            constant = false;
                            if (in.events.size() <= 1 ? in.sig == net : false) same = true;
                        }
                        if (in.k == IK::Assign && in.sig == net) same = true;
                    }
                    if (same) continue;
                    if (dir == 0 && constant) {
                        if (!lc.reconnect)
                            throw Error(lc.where + ": connect_logic_net: input port " + dn + " is driven by a constant; use -reconnect (6.13 c)");
                        old.prog.code.clear();
                        old.port.clear();
                        old.kind = "upf_wire";
                        continue;
                    }
                    throw Error(lc.where + ": connect_logic_net: port " + dn + " is already connected to another net (6.13 b)");
                }
            }
            int src = portDrives ? port : net, dst = portDrives ? net : port;
            LoweredProc np;
            Instr a; a.k = IK::Assign; a.sig = dst; a.e = ref(src);
            np.prog.emit(a);
            np.prog.emit(waitInstr({{src, ANY}}));
            Instr j; j.k = IK::Jmp; j.target = 0;
            np.prog.emit(j);
            np.loops = true;
            np.kind = low ? "upf_wire" : "assign";
            np.scope = m.design(lc.scope);
            if (!low) { np.port = dn; np.portDir = dir; }
            lw.procs.push_back(std::move(np));
            note("logic net " + m.design(lc.net) + (portDrives ? " <- " : " -> ") + dn);
        }
    }
#endif

    // ------------------------------------------- HDL ports on supply nets
    // connect_supply_net to a port of the design that is not a UPF supply
    // port (4.10): a value conversion table turns the port's logic value
    // into the net's supply state (hdl2upf, the port drives the net) or the
    // net's state into the port's value (upf2hdl, the net drives the port).
    // Without -vct an output port drives the net through SV_LOGIC2UPF and
    // an input port is driven through UPF2SV_LOGIC (Annex B).
    // With -vcm (6.14, 6.30) a VCM converts other HDL types too: real,
    // integer, enum and multi-bit ports by table, the VCM picked out of a
    // list by the port's type and direction (4.10.2). A port of the package
    // type upfSupplyTypeT (supply_net_type) connects without conversion
    // (4.10.1).
    struct HdlPort {
        int sig = -1;
        std::string net, port;
        const Vct* vct = nullptr;
        const Vcm* vcm = nullptr;    // a table VCM of type r, i or e
        bool supply = false;         // a upfSupplyTypeT port
        bool hdl2upf = true;
        char cat = 'b';
        bool sgn = false;
        int w = 1;
        std::map<int64_t, std::string> labelOf;   // enum value -> name
        std::map<std::string, int64_t> valueOf;   // enum name -> value
        int last = -1;               // upf2hdl through a Vct: the value held on the port
        Val lastVal;
        bool held = false;
        double lastV = 0;            // hdl2upf: the voltage last driven (rows without a voltage keep it)
        bool hasV = false;
        bool warned = false;
        int fOff = -1;               // -field: the field's lowest bit in the port (its width is w)
        int copyFrom = -1, target = -1;   // a function or model VCM (upf2hdl): its result, held on the port
        Val lastCopy;
        bool copied = false;
    };
    std::vector<HdlPort> hdlPorts;
    // Supply ports found by their pg_type (UPF_pg_type, 6.51): the
    // connections of connect_supply_net -pg_type (6.14), of
    // connect_supply_set -connect (6.15), and the automatic ones of each
    // domain's primary supply set (4.5.4.6.2), in that order of precedence;
    // a port already connected keeps its connection.
    void expandPgConnects() {
        std::set<std::string> taken;
        for (auto& [net, ps] : m.connects) taken.insert(ps.begin(), ps.end());
        std::string pre = S.vcdTop.empty() ? "" : S.vcdTop + ".";
        std::string topPre = m.top.empty() ? "" : m.top + ".";
        struct Inst { std::string design, upf, model; };
        std::vector<Inst> insts;
        for (auto& [full, mod] : instCells()) {
            if (!pre.empty() && full.compare(0, pre.size(), pre) != 0) continue;
            std::string d = full.substr(pre.size());
            if (!topPre.empty() && d.compare(0, topPre.size(), topPre) != 0) continue;
            std::string u = d.substr(topPre.size());
            for (char& c : u) if (c == '.') c = '/';
            insts.push_back({d, u, mod});
        }
        auto under = [](const std::string& u, const std::string& scope) {
            return scope.empty() || u == scope || (u.size() > scope.size() && u.compare(0, scope.size(), scope) == 0 && u[scope.size()] == '/');
        };
        // the ports of an instance with a pg_type in `types`
        auto pgPorts = [&](const Inst& in, const std::vector<std::string>& types, std::vector<std::pair<std::string, std::string>>& out) {
            std::string full = pre + in.design + ".";
            for (auto it = portRecs().lower_bound(full); it != portRecs().end() && it->first.compare(0, full.size(), full) == 0; ++it) {
                std::string port = it->first.substr(full.size());
                if (port.find('.') != std::string::npos) continue;
                std::string pg = m.portAttr(in.design, in.model, port, it->second.dir, "UPF_pg_type");
                if (!pg.empty() && std::find(types.begin(), types.end(), pg) != types.end()) out.push_back({in.upf + "/" + port, pg});
            }
        };
        auto add = [&](const std::string& net, const std::string& port, const Vcm* vcm, const std::string& where, const std::string& how) {
            if (!taken.insert(port).second) return;
            m.connects[net].push_back(port);
            if (vcm) m.connectVcm[{net, port}] = vcm;
            m.connectWhere[{net, port}] = where;
            note("supply " + net + " connected to " + m.design(port) + " (" + how + ")");
        };
        for (auto& pc : m.pgConnects) {   // 6.14
            std::string base = pc.domain.empty() ? pc.scope : m.domains.at(pc.domain).scope;
            int hits = 0;
            for (auto& in : insts) {
                if (!under(in.upf, base) || in.upf == base) continue;
                if (!pc.domain.empty() && dm.of(in.design) != pc.domain) continue;
                if (!pc.cells.empty() && std::find(pc.cells.begin(), pc.cells.end(), in.model) == pc.cells.end()) continue;
                if (!pc.elements.empty() && std::find(pc.elements.begin(), pc.elements.end(), in.upf) == pc.elements.end()) continue;
                std::vector<std::pair<std::string, std::string>> ps;
                pgPorts(in, pc.pgTypes, ps);
                for (auto& [port, pg] : ps) { add(pc.net, port, pc.vcm, pc.where, "-pg_type " + pg); hits++; }
            }
            for (auto& c : pc.cells)
                if (std::none_of(insts.begin(), insts.end(), [&](const Inst& in) { return in.model == c; }))
                    throw Error(pc.where + ": connect_supply_net -cells: no instance of " + c + " in the design (6.14)");
            for (auto& e : pc.elements)
                if (std::none_of(insts.begin(), insts.end(), [&](const Inst& in) { return in.upf == e; }))
                    throw Error(pc.where + ": connect_supply_net -elements: no instance " + m.design(e) + " in the design (6.14)");
            if (!hits) std::cerr << "msim: UPF: " << pc.where << ": connect_supply_net " << pc.net << " -pg_type: no port with that pg_type\n";
        }
        // the supply net of a supply set function
        auto fnNet = [&](const std::string& set, const std::string& fn) -> std::string {
            auto it = m.sets.find(m.setOf(set));
            if (it == m.sets.end()) return "";
            auto f = it->second.fn.find(fn);
            return f == it->second.fn.end() ? "" : f->second;
        };
        for (auto& sc : m.supplyConns) {   // 6.15 -connect
            if (sc.connect.empty()) continue;
            for (auto& in : insts) {
                bool hit = false;
                for (auto& e : sc.elements)
                    if (in.upf == e || (sc.transitive && under(in.upf, e))) hit = true;
                for (auto& e : sc.exclude)
                    if (in.upf == e || under(in.upf, e)) hit = false;
                if (!hit) continue;
                for (auto& [fn, pg] : sc.connect) {
                    std::string net = fnNet(sc.set, fn);
                    if (net.empty()) throw Error(sc.where + ": connect_supply_set " + sc.set + " -connect: the set has no " + fn + " function (6.15)");
                    std::vector<std::pair<std::string, std::string>> ps;
                    pgPorts(in, {pg}, ps);
                    for (auto& pp : ps) add(net, pp.first, nullptr, sc.where, "connect_supply_set " + sc.set + "." + fn);
                }
            }
        }
        // 4.5.4.6.2: each function of a domain's primary supply set to the
        // ports of its default pg_type, on instances in the domain's extent
        static const std::vector<std::pair<std::string, std::string>> dflt = {
            {"power", "primary_power"}, {"ground", "primary_ground"}, {"pwell", "pwell"},
            {"nwell", "nwell"}, {"deeppwell", "deeppwell"}, {"deepnwell", "deepnwell"}};
        for (auto& in : insts) {
            std::string dn = dm.of(in.design);
            if (dn.empty()) continue;
            auto d = m.domains.find(dn);
            if (d == m.domains.end()) continue;
            auto h = d->second.handles.find("primary");
            if (h == d->second.handles.end()) continue;
            for (auto& [fn, pg] : dflt) {
                std::string net = fnNet(h->second, fn);
                if (net.empty()) continue;
                std::vector<std::pair<std::string, std::string>> ps;
                pgPorts(in, {pg}, ps);
                for (auto& pp : ps) add(net, pp.first, nullptr, d->second.where, "primary supply of " + dn);
            }
        }
    }
    // Function and model VCMs (6.30.8, 6.30.9) placed in the design before
    // it is elaborated again: the instance that runs the conversion.
    struct VcmBind {
        const Vcm* vcm = nullptr;
        bool hdl2upf = true;
        std::string inst;   // under the simulation top
    };
    std::map<std::pair<std::string, std::string>, VcmBind> vcmBinds;
    // What an HDL port is, for choosing a VCM: its category, type name, enum values.
    struct PortType {
        char cat = 'x';              // b one bit, r real, i integer or vector, e enum, s upfSupplyTypeT
        std::string name, keyword;   // the type's own name (an alias or enum), the predefined keyword
        bool sgn = false;
        std::map<int64_t, std::string> labels;
        const slang::ast::Type* type = nullptr;   // the port's declared type
    };
    static std::string stateName(int st) {
        return st == S_OFF ? "OFF" : st == S_FULL_ON ? "FULL_ON" : st == S_PARTIAL_ON ? "PARTIAL_ON" : "UNDETERMINED";
    }
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    static PortType portTypeOf(const slang::ast::Symbol* sym) {
        using namespace slang::ast;
        if (!sym || !ValueSymbol::isKind(sym->kind)) return {};
        return typeInfo(sym->as<ValueSymbol>().getType());
    }
    static PortType typeInfo(const slang::ast::Type& t) {
        using namespace slang::ast;
        PortType pt;
        pt.type = &t;
        const Type& ct = t.getCanonicalType();
        if (t.isAlias()) pt.name = std::string(t.name);
        if (ct.kind == SymbolKind::PackedStructType || ct.kind == SymbolKind::UnpackedStructType) {
            std::set<std::string> f;
            for (auto& fs : ct.as<Scope>().membersOfType<FieldSymbol>()) f.insert(std::string(fs.name));
            if (ct.kind == SymbolKind::PackedStructType && f == std::set<std::string>{"state", "voltage"}) pt.cat = 's';
            else pt.cat = 'u';   // a struct: a VCM converts one of its fields (-field)
            if (pt.name.empty()) pt.name = std::string(ct.name);
            return pt;
        }
        if (ct.isFloating()) { pt.cat = 'r'; pt.keyword = ct.toString(); return pt; }
        if (ct.isEnum()) {
            pt.cat = 'e';
            if (pt.name.empty()) pt.name = std::string(ct.name);
            for (auto& ev : ct.as<EnumType>().values()) {
                auto cv = ev.getValue();
                if (cv.isInteger()) {
                    auto iv = cv.integer().as<int64_t>();
                    if (iv) pt.labels[*iv] = std::string(ev.name);
                }
            }
            return pt;
        }
        if (ct.isIntegral()) {
            pt.sgn = ct.isSigned();
            if (ct.kind == SymbolKind::PredefinedIntegerType) { pt.cat = 'i'; pt.keyword = ct.toString(); }
            else pt.cat = ct.getBitWidth() == 1 ? 'b' : 'i';
        }
        return pt;
    }
#endif
    // A field of a struct port (-field of a table VCM, 6.30): its bits in the
    // port's value, counted from bit 0, and its type. "a.b" names a field of
    // a nested struct.
    struct FieldLoc { bool ok = false; int off = 0, w = 0; PortType pt; std::string why; };
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    static FieldLoc fieldOf(const slang::ast::Type& t0, const std::string& path) {
        using namespace slang::ast;
        FieldLoc r;
        const Type* cur = &t0;
        size_t b = 0;
        while (true) {
            size_t e = path.find('.', b);
            std::string nm = path.substr(b, e == std::string::npos ? std::string::npos : e - b);
            const Type& ct = cur->getCanonicalType();
            if (ct.kind != SymbolKind::PackedStructType && ct.kind != SymbolKind::UnpackedStructType) {
                r.why = "it is not a struct where " + nm + " is looked up";
                return r;
            }
            const FieldSymbol* f = nullptr;
            for (auto& fs : ct.as<Scope>().membersOfType<FieldSymbol>())
                if (fs.name == nm) f = &fs;
            if (!f) {
                r.why = "it has no field " + nm;
                return r;
            }
            if (ct.kind == SymbolKind::PackedStructType) r.off += (int)f->bitOffset;
            else r.off += uField(ct, *f).first;
            cur = &f->getType();
            if (e == std::string::npos) break;
            b = e + 1;
        }
        const Type& ft = cur->getCanonicalType();
        r.w = ft.isFloating() ? 64 : ft.isIntegral() ? (int)ft.getBitWidth() : uWidth(ft);
        r.pt = typeInfo(*cur);
        if (r.pt.cat == 'b') r.pt.cat = 'i';   // a one-bit field: an integer table (0 and 1)
        if (r.pt.cat == 'x' || r.pt.cat == 'u' || r.pt.cat == 's') {
            r.why = "field " + path + " is not of a type a table converts (4.10.1)";
            return r;
        }
        r.ok = true;
        return r;
    }
#endif
    // Whether a table VCM converts this port: its type, or with -field the
    // named field of a struct of the VCM's -hdl_type.
#ifndef UPF_RT_NO_DESIGN
    static bool tableFits(const Vcm& v, const PortType& pt) {
        if (v.field.empty()) return v.cat == 'b' ? pt.cat == 'b' && (v.lang == "sv" || v.bits.lang == "sv") : vcmFits(v, pt);
        if (pt.cat != 'u' || !pt.type || v.lang != "sv") return false;
        std::string last = v.hdlType.substr(v.hdlType.rfind(':') == std::string::npos ? 0 : v.hdlType.rfind(':') + 1);
        if (last != pt.name) return false;
        FieldLoc fl = fieldOf(*pt.type, v.field);
        if (!fl.ok) return false;
        return fl.pt.cat == v.cat;   // the table's values say which kind of field it converts
    }
#endif
    // Whether a table VCM converts this port's type (6.14).
    static bool vcmFits(const Vcm& v, const PortType& pt) {
        if (v.lang != "sv") return false;
        if (v.cat != pt.cat) return false;
        static const std::set<std::string> builtin = {"bit", "logic", "reg", "real", "shortreal", "realtime", "int", "integer",
                                                      "shortint", "longint", "byte"};
        std::string ty = v.hdlType;
        for (char& c : ty) c = (char)std::tolower((unsigned char)c);
        if (!builtin.count(ty)) {   // a user type: by its name, with or without the package
            std::string last = v.hdlType.substr(v.hdlType.rfind(':') == std::string::npos ? 0 : v.hdlType.rfind(':') + 1);
            return last == pt.name;
        }
        if (pt.cat == 'i' && !pt.keyword.empty()) return ty == pt.keyword;
        if (pt.cat == 'i') return ty == "integer" || ty == "int" || ty == "longint";   // a vector
        return true;
    }
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bindHdlPorts(LowerResult& lw) {
        std::map<std::string, int> dirOf;   // instance port (simulator name) -> 0 input, 1 output, 2 inout
        for (auto& lp : lw.procs)
            if (!lp.port.empty()) dirOf[lp.port] = lp.portDir;
        std::map<int, const slang::ast::Symbol*> symOf;
        auto typeOf = [&](int s) {
            if (symOf.empty())
                for (auto& [sym, sid] : lw.sigOf)
                    if (sym && slang::ast::ValueSymbol::isKind(sym->kind)) symOf.emplace(sid, sym);
            auto it = symOf.find(s);
            return portTypeOf(it == symOf.end() ? nullptr : it->second);
        };
        for (auto& [net, ps] : m.connects)
            for (auto& p : ps) {
                if (isSupply(p)) continue;
                auto key = std::make_pair(net, p);
                std::string where = m.connectWhere.count(key) ? m.connectWhere.at(key) : "";
                auto named = m.connectVct.find(key);
                auto nvcm = m.connectVcm.find(key);
                bool explicitConv = named != m.connectVct.end() || nvcm != m.connectVcm.end();
                int s = sigOf(p);
                if (s < 0) {
                    if (explicitConv)
                        throw Error(where + ": connect_supply_net " + net + ": no port '" + m.design(p) + "' in the design");
                    continue;
                }
                HdlPort h;
                h.sig = s;
                h.net = net;
                h.port = p;
                h.w = S.sigs[(size_t)s].w;
                auto d = dirOf.find(S.sigs[(size_t)s].name);
                int dir = d == dirOf.end() ? -1 : d->second;
                PortType pt = typeOf(s);
                auto fail = [&](const std::string& msg) { throw Error(where + ": connect_supply_net " + net + ": " + m.design(p) + msg); };
                if (pt.cat == 's') {   // upfSupplyTypeT: no conversion; -vct and -vcm do not apply (6.14)
                    if (dir != 0 && dir != 1) fail(" is a upfSupplyTypeT port that is neither an input nor an output");
                    h.supply = true;
                    h.hdl2upf = dir == 1;
                    note(std::string("supply net ") + net + (h.hdl2upf ? " is driven by " : " drives ") + m.design(p) + " (upfSupplyTypeT)");
                    hdlPorts.push_back(h);
                    continue;
                }
                if (auto vb = vcmBinds.find(key); vb != vcmBinds.end()) {   // a function or model VCM
                    auto sigNamed = [&](const std::string& n) {
                        auto it = sigByName.find(n);
                        if (it == sigByName.end()) fail(": VCM " + vb->second.vcm->name + ": no signal " + n);
                        return it->second;
                    };
                    h.supply = true;
                    h.hdl2upf = vb->second.hdl2upf;
                    if (h.hdl2upf) h.sig = sigNamed(vb->second.inst + ".upf_vcm_out");
                    else {
                        h.sig = sigNamed(vb->second.inst + ".upf_vcm_in");
                        h.copyFrom = sigNamed(vb->second.inst + ".upf_vcm_val");
                        h.target = s;
                    }
                    note(std::string("supply net ") + net + (h.hdl2upf ? " is driven by " : " drives ") + m.design(p) + " through " +
                         vb->second.vcm->name + " (" + vb->second.inst + ")");
                    hdlPorts.push_back(h);
                    continue;
                }
                if (nvcm != m.connectVcm.end()) {
                    std::vector<const Vcm*> all, fit;
                    nvcm->second->flatten(all);
                    for (auto* v : all) {
                        if (v->kind == Vcm::Function || v->kind == Vcm::Model) continue;   // none fit this port (vcmSource)
                        bool dirOk = dir == 2 || (dir == 0 && !v->hdl2upf) || (dir == 1 && v->hdl2upf);
                        if (!dirOk) continue;
                        if (tableFits(*v, pt)) fit.push_back(v);
                    }
                    std::string tn = pt.cat == 'b' ? "a one-bit port" : pt.cat == 'r' ? "a real port" : pt.cat == 'e' ? "an enum port (" + pt.name + ")"
                                   : pt.cat == 'i' ? "an integer port" : pt.cat == 'u' ? "a struct port (" + pt.name + ")"
                                   : "a port of a type no VCM converts";
                    std::string dn = dir == 0 ? "input" : dir == 1 ? "output" : "inout";
                    if (fit.empty() && pt.cat == 'u')   // say why no field fits (6.30)
                        for (auto* v : all) {
                            if (v->kind != Vcm::Table) continue;
                            std::string last = v->hdlType.substr(v->hdlType.rfind(':') == std::string::npos ? 0 : v->hdlType.rfind(':') + 1);
                            if (last != pt.name) continue;
                            if (v->field.empty())
                                fail(" is " + tn + ", and VCM " + v->name + " converts the whole struct " + v->hdlType +
                                     "; a struct with several fields needs -field (6.30)");
                            FieldLoc fl = fieldOf(*pt.type, v->field);
                            if (!fl.ok) fail(": VCM " + v->name + " -field " + v->field + ": " + fl.why + " (6.30)");
                        }
                    if (fit.empty())
                        fail(" is " + tn + " (" + dn + "), and VCM " + nvcm->second->name + " has no " +
                             (dir == 0 ? "upf2hdl " : dir == 1 ? "hdl2upf " : "") + "conversion for it (6.14, 4.10.2)");
                    // Fields of a struct input: one upf2hdl VCM for each field (6.30).
                    bool perField = fit.size() > 1 && pt.cat == 'u';
                    std::set<std::string> fieldsSeen;
                    for (auto* v : fit) {
                        if (!perField) break;
                        if (v->hdl2upf || !fieldsSeen.insert(v->field).second) perField = false;
                    }
                    if (fit.size() > 1 && !perField)
                        fail(" is " + tn + " (" + dn + "), and VCMs " + fit[0]->name + " and " + fit[1]->name + " both fit it" +
                             (dir == 2 ? "; a list for an inout port holds one direction per type (4.10.2)"
                              : pt.cat == 'u' ? "; only a struct input takes one upf2hdl VCM for each of its fields (6.30)" : " (6.14)"));
                    for (const Vcm* v : fit) {
                        HdlPort hf = h;
                        PortType ft = pt;
                        if (!v->field.empty()) {
                            FieldLoc fl = fieldOf(*pt.type, v->field);
                            hf.fOff = fl.off;
                            hf.w = fl.w;
                            ft = fl.pt;
                        }
                        hf.hdl2upf = v->hdl2upf;
                        if (v->cat == 'b') hf.vct = &v->bits;
                        else {
                            hf.vcm = v;
                            hf.cat = ft.cat;
                            hf.sgn = ft.sgn;
                            for (auto& [val, nm] : ft.labels) { hf.labelOf[val] = nm; hf.valueOf[nm] = val; }
                            if (ft.cat == 'e')
                                for (auto& r : v->rows)
                                    if (r.h == Vcm::Row::Label && !hf.valueOf.count(r.label))
                                        fail(": VCM " + v->name + " names " + r.label + ", which is not a value of " + ft.name + " (6.30.6)");
                        }
                        note(std::string("supply net ") + net + (hf.hdl2upf ? " is driven by " : " drives ") + m.design(p) +
                             (v->field.empty() ? "" : "." + v->field) + " through " + v->name);
                        hdlPorts.push_back(hf);
                    }
                    continue;
                }
                if (named != m.connectVct.end()) {
                    h.vct = m.vct(named->second);
                    if ((dir == 0 && h.vct->hdl2upf) || (dir == 1 && !h.vct->hdl2upf))
                        throw Error(where + ": connect_supply_net " + net + ": " + m.design(p) + " is an " +
                                    (dir == 0 ? "input" : "output") + " port, but " + h.vct->name + " converts " +
                                    (h.vct->hdl2upf ? "an HDL value to a supply state" : "a supply state to an HDL value"));
                } else if (dir == 0 || dir == 1) {
                    if (pt.cat != 'b' && pt.cat != 'x')
                        fail(" is not a one-bit port; connect it with -vcm and a VCM for its type (6.14, 9.1)");
                    h.vct = m.vct(dir == 1 ? "SV_LOGIC2UPF" : "UPF2SV_LOGIC");
                } else {
                    throw Error(where + ": connect_supply_net " + net + ": " + m.design(p) +
                                " is not an input or output port of an instance; name a value conversion table with -vct");
                }
                h.hdl2upf = h.vct->hdl2upf;
                if (S.sigs[(size_t)s].w != 1)
                    throw Error(where + ": connect_supply_net " + net + ": " + m.design(p) + " is " +
                                std::to_string(S.sigs[(size_t)s].w) + " bits wide; a value conversion table converts one bit");
                note(std::string("supply net ") + net + (h.vct->hdl2upf ? " is driven by " : " drives ") + m.design(p) +
                     " through " + h.vct->name);
                hdlPorts.push_back(h);
            }
        std::map<std::string, std::string> drivers;
        for (auto& h : hdlPorts) {
            if (!h.hdl2upf) continue;
            std::string r = root(h.net);
            if (swOf.count(r))
                throw Error("supply net " + h.net + " is driven by the HDL port " + m.design(h.port) + " and by the power switch " +
                            swOf[r][0]);
            if (!drivers.emplace(r, h.port).second)
                throw Error("supply net " + h.net + " is driven by two HDL ports (" + m.design(drivers[r]) + ", " + m.design(h.port) + ")");
        }
    }
#endif
    // A function or model VCM's result, held on its port (upf2hdl).
    void copyVcmResults() {
        for (auto& h : hdlPorts) {
            if (h.copyFrom < 0) continue;
            Val v = S.get(h.copyFrom);
            if (h.copied && v == h.lastCopy) continue;
            h.copied = true;
            h.lastCopy = v;
            S.holdOn(h.target, konst(v), true);
        }
    }
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    static bool supplyType(const slang::ast::Type& t) {
        using namespace slang::ast;
        const Type& ct = t.getCanonicalType();
        if (ct.kind != SymbolKind::PackedStructType) return false;
        std::set<std::string> f;
        for (auto& fs : ct.as<Scope>().membersOfType<FieldSymbol>()) f.insert(std::string(fs.name));
        return f == std::set<std::string>{"state", "voltage"};
    }
#endif
    // SystemVerilog for the function and model VCMs of connect_supply_net
    // -vcm (6.30.8, 6.30.9): one module per connection, bound into the
    // simulation top, that calls the function or instantiates the model on
    // the port (a hierarchical reference) and a upfSupplyTypeT variable the
    // runtime reads or writes. Table VCMs need none. simTop is the
    // simulation's top instance; comp is the design elaborated without them.
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    std::string vcmSource(slang::ast::Compilation& comp, const std::string& simTop) {
        using namespace slang::ast;
        std::string out;
        int k = 0;
        for (auto& [key, vcm] : m.connectVcm) {
            std::vector<const Vcm*> all;
            vcm->flatten(all);
            if (std::none_of(all.begin(), all.end(), [](const Vcm* v) { return v->kind == Vcm::Function || v->kind == Vcm::Model; }))
                continue;
            const std::string& p = key.second;
            std::string where = m.connectWhere.count(key) ? m.connectWhere.at(key) : "";
            std::string full = simTop + "." + m.design(p);
            auto fail = [&](const std::string& msg) -> void { throw Error(where + ": connect_supply_net " + key.first + ": " + msg); };
            // walk the instance tree: the instance, then the port's own variable or net
            size_t dot = full.rfind('.');
            const Symbol* instSym = nullptr;
            {
                std::vector<std::string> parts;
                for (size_t a = 0, b2; a <= dot; a = b2 + 1) {
                    b2 = full.find('.', a);
                    if (b2 == std::string::npos || b2 > dot) b2 = dot;
                    parts.push_back(full.substr(a, b2 - a));
                    if (b2 == dot) break;
                }
                for (auto* t : comp.getRoot().topInstances)
                    if (!parts.empty() && t->name == parts[0]) instSym = t;
                for (size_t i = 1; instSym && i < parts.size(); i++) {
                    const Symbol* c2 = instSym->as<InstanceSymbol>().body.find(parts[i]);
                    instSym = c2 && c2->kind == SymbolKind::Instance ? c2 : nullptr;
                }
            }
            const Symbol* sym = instSym ? instSym->as<InstanceSymbol>().body.find(full.substr(dot + 1)) : nullptr;
            if (!sym || !instSym || instSym->kind != SymbolKind::Instance) fail("no port '" + m.design(p) + "' in the design");
            const Symbol* ps = instSym->as<InstanceSymbol>().body.findPort(full.substr(dot + 1));
            if (!ps || ps->kind != SymbolKind::Port) fail(m.design(p) + " is not a port");
            ArgumentDirection pd = ps->as<PortSymbol>().direction;
            int dir = pd == ArgumentDirection::In ? 0 : pd == ArgumentDirection::Out ? 1 : 2;
            if (!ValueSymbol::isKind(sym->kind)) fail(m.design(p) + " is not a variable or net");
            const Type& pt = sym->as<ValueSymbol>().getType();
            PortType ptc = portTypeOf(sym);
            if (ptc.cat == 's') continue;   // no conversion (6.14)
            struct Fit { const Vcm* v; bool h2u; std::string inPort, outPort; };
            std::vector<Fit> fits;
            for (const Vcm* v : all) {
                bool h2u = false;
                std::string inP, outP;
                if (v->kind == Vcm::Table) {
                    bool dirOk = dir == 2 || (dir == 0 && !v->hdl2upf) || (dir == 1 && v->hdl2upf);
                    if (dirOk && tableFits(*v, ptc)) fits.push_back({v, v->hdl2upf, "", ""});
                    continue;
                }
                if (v->kind == Vcm::Function) {   // 6.30.8
                    std::string pkg = v->function.substr(0, v->function.find("::")), fn = v->function.substr(v->function.find("::") + 2);
                    const PackageSymbol* pk = comp.getPackage(pkg);
                    if (!pk) fail("VCM " + v->name + ": no package " + pkg + " for -function " + v->function + " (6.30.8)");
                    const Symbol* f = pk->find(fn);
                    if (!f || f->kind != SymbolKind::Subroutine) fail("VCM " + v->name + ": no function " + v->function + " (6.30.8)");
                    auto& sub = f->as<SubroutineSymbol>();
                    auto args = sub.getArguments();
                    if (args.size() != 1 || args[0]->direction != ArgumentDirection::In)
                        fail("VCM " + v->name + ": " + v->function + " needs exactly one input argument (6.30.8)");
                    bool argS = supplyType(args[0]->getType()), retS = supplyType(sub.getReturnType());
                    if (argS == retS)
                        fail("VCM " + v->name + ": exactly one of the argument and the return type of " + v->function +
                             " is upfSupplyTypeT (6.30.8)");
                    h2u = retS;
                    const Type& hdlT = h2u ? args[0]->getType() : sub.getReturnType();
                    if (!hdlT.isMatching(pt) && !hdlT.isEquivalent(pt)) continue;
                } else {   // 6.30.9
                    std::string mod = v->model.substr(v->model.rfind('.') == std::string::npos ? 0 : v->model.rfind('.') + 1);
                    auto def = comp.tryGetDefinition(mod, comp.getRoot());
                    if (!def.definition || def.definition->kind != SymbolKind::Definition)
                        fail("VCM " + v->name + ": no module " + v->model + " for -model (6.30.9)");
                    auto& inst = InstanceSymbol::createDefault(comp, def.definition->as<DefinitionSymbol>());
                    std::vector<const PortSymbol*> ins, outs;
                    for (const Symbol* q : inst.body.getPortList()) {
                        if (q->kind != SymbolKind::Port) fail("VCM " + v->name + ": " + v->model + " has a port that is not a plain port (6.30.9)");
                        auto& qp = q->as<PortSymbol>();
                        if (qp.direction == ArgumentDirection::In) ins.push_back(&qp);
                        else if (qp.direction == ArgumentDirection::Out) outs.push_back(&qp);
                        else fail("VCM " + v->name + ": " + v->model + " has an inout port; bidirectional model VCMs are not supported (6.30.9)");
                    }
                    if (ins.size() != 1 || outs.size() != 1)
                        fail("VCM " + v->name + ": " + v->model + " needs one input and one output port (6.30.9)");
                    bool inS = supplyType(ins[0]->getType()), outS = supplyType(outs[0]->getType());
                    if (inS == outS)
                        fail("VCM " + v->name + ": exactly one port of " + v->model + " is upfSupplyTypeT (6.30.9)");
                    h2u = outS;
                    const Type& hdlT = h2u ? ins[0]->getType() : outs[0]->getType();
                    if (!hdlT.isMatching(pt) && !hdlT.isEquivalent(pt)) continue;
                    inP = std::string(ins[0]->name);
                    outP = std::string(outs[0]->name);
                }
                bool dirOk = dir == 2 || (dir == 0 && !h2u) || (dir == 1 && h2u);
                if (dirOk) fits.push_back({v, h2u, inP, outP});
            }
            if (fits.size() > 1)
                fail(m.design(p) + ": VCMs " + fits[0].v->name + " and " + fits[1].v->name + " both fit it (6.14)");
            if (fits.empty() || fits[0].v->kind == Vcm::Table) continue;   // bindHdlPorts reports it, or converts by table
            const Fit& f = fits[0];
            std::string modName = "upf_vcm_mod_" + std::to_string(k), inst = "upf_vcm_" + std::to_string(k);
            k++;
            std::string body;
            if (f.v->kind == Vcm::Function) {
                if (f.h2u) body = "  UPF::upfSupplyTypeT upf_vcm_out;\n  assign upf_vcm_out = " + f.v->function + "(" + full + ");\n";
                else
                    body = "  UPF::upfSupplyTypeT upf_vcm_in = '0;\n  var type(" + f.v->function + "(upf_vcm_in)) upf_vcm_val;\n" +
                           "  assign upf_vcm_val = " + f.v->function + "(upf_vcm_in);\n";
            } else {
                std::string mod = f.v->model.substr(f.v->model.rfind('.') == std::string::npos ? 0 : f.v->model.rfind('.') + 1);
                std::string params;
                for (size_t i = 0; i < f.v->params.size(); i++) {
                    static const std::regex num(R"(^[-+]?([0-9][0-9_]*(\.[0-9_]+)?([eE][-+]?[0-9]+)?)?('[sS]?[bBoOdDhH]?[0-9a-fA-FxXzZ_?]+)?$)");
                    std::string val = f.v->params[i].second;
                    if (val.empty() || !std::regex_match(val, num)) val = "\"" + val + "\"";
                    params += std::string(i ? ", ." : ".") + f.v->params[i].first + "(" + val + ")";
                }
                std::string inst2 = mod + (params.empty() ? "" : " #(" + params + ")") + " u_model(";
                if (f.h2u) body = "  UPF::upfSupplyTypeT upf_vcm_out;\n  " + inst2 + "." + f.inPort + "(" + full + "), ." + f.outPort + "(upf_vcm_out));\n";
                else
                    body = "  UPF::upfSupplyTypeT upf_vcm_in = '0;\n  " + pt.toString() + " upf_vcm_val;\n  " + inst2 + "." + f.inPort +
                           "(upf_vcm_in), ." + f.outPort + "(upf_vcm_val));\n";
            }
            out += "// connect_supply_net " + key.first + " -ports " + p + " -vcm " + f.v->name + " (IEEE 1801-2024 6.30)\nmodule " + modName +
                   ";\n" + body + "endmodule\nbind " + simTop + " " + modName + " " + inst + "();\n";
            vcmBinds[key] = {f.v, f.h2u, inst};
        }
        return out;
    }
#endif
    // map_retention_cell -lib_model_name (6.39): each mapped register is
    // simulated by the model. A wrapper module bound into the register's
    // instance takes the register's canonical interface, inferred from the
    // always block that assigns it (UPF_GENERIC_CLOCK, _DATA,
    // _ASYNC_SET_RESET / _ASYNC_LOAD, _OUTPUT), and instantiates the model
    // with -port_map; supply ports of the model are connected like
    // connect_supply_net. The register then follows the model's output, and
    // neither corruption nor the strategy's retention applies to it.
    struct RetModelEl { std::string el, q, inst, strategy; int elSig = -1, qSig = -1; bool copied = false; Val last; };
    std::vector<RetModelEl> retModelEls;
    std::vector<std::string> retModelInsts;   // design paths of the wrapper instances
    bool inRetModel(const std::string& scope) const {
        for (auto& i : retModelInsts) if (DomainMap::within(scope, i)) return true;
        return false;
    }
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    std::string retModelSource(slang::ast::Compilation& comp, const std::string& simTop) {
        using namespace slang::ast;
        std::string out;
        int k = 0;
        auto trim = [](std::string t) {
            size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
        };
        auto text = [&](const Expression& e) { return e.syntax ? trim(e.syntax->toString()) : std::string(); };
        auto instAt = [&](const std::string& full) -> const InstanceSymbol* {
            const Symbol* cur = nullptr;
            size_t a = 0;
            while (true) {
                size_t b = full.find('.', a);
                std::string part = full.substr(a, b == std::string::npos ? std::string::npos : b - a);
                if (!cur) {
                    for (auto* t : comp.getRoot().topInstances) if (t->name == part) cur = t;
                } else {
                    const Symbol* c2 = cur->as<InstanceSymbol>().body.find(part);
                    cur = c2 && c2->kind == SymbolKind::Instance ? c2 : nullptr;
                }
                if (!cur) return nullptr;
                if (b == std::string::npos) return &cur->as<InstanceSymbol>();
                a = b + 1;
            }
        };
        for (auto& rm : m.retModels) {
            auto fail = [&](const std::string& msg) -> void { throw Error(rm.where + ": map_retention_cell -lib_model_name " + rm.model + ": " + msg + " (6.39)"); };
            std::string mod = rm.model.substr(rm.model.rfind('.') == std::string::npos ? 0 : rm.model.rfind('.') + 1);
            auto def = comp.tryGetDefinition(mod, comp.getRoot());
            if (!def.definition || def.definition->kind != SymbolKind::Definition) fail("no module " + rm.model);
            auto& minst = InstanceSymbol::createDefault(comp, def.definition->as<DefinitionSymbol>());
            struct MP { std::string name; int dir; bool supply; int w; };
            std::vector<MP> mports;
            for (const Symbol* q : minst.body.getPortList()) {
                if (q->kind != SymbolKind::Port) fail(rm.model + " has a port that is not a plain port");
                auto& qp = q->as<PortSymbol>();
                int dir = qp.direction == ArgumentDirection::In ? 0 : qp.direction == ArgumentDirection::Out ? 1 : 2;
                if (dir == 2) fail(rm.model + " has an inout port " + std::string(qp.name));
                mports.push_back({std::string(qp.name), dir, supplyType(qp.getType()), (int)qp.getType().getBitWidth()});
            }
            for (auto& [port, ref] : rm.portMap)
                if (std::none_of(mports.begin(), mports.end(), [&](const MP& x) { return x.name == port; }))
                    fail(rm.model + " has no port " + port);
            // the registers: -elements, or each strategy's elements
            std::vector<std::pair<const Retention*, std::string>> regs;   // strategy, rooted element
            for (auto& sn : rm.strategies)
                for (auto& r : m.rets)
                    if (r.name == sn && r.domain == rm.domain) {
                        const auto& els = rm.elements.empty() ? r.elements : rm.elements;
                        if (els.empty()) fail("strategy " + sn + " names no elements; give -elements");
                        for (auto& e : els) regs.push_back({&r, e});
                    }
            for (auto& [r, e] : regs) {
                std::string d = m.design(e);
                std::string full = simTop.empty() ? d : simTop + "." + d;
                // an instance: every register its clocked blocks assign; else one variable
                std::vector<std::pair<const InstanceSymbol*, const Symbol*>> vars;
                if (const InstanceSymbol* is = instAt(full)) {
                    std::function<void(const InstanceSymbol&)> walk = [&](const InstanceSymbol& in) {
                        for (auto& pb : in.body.membersOfType<ProceduralBlockSymbol>()) {
                            if (pb.procedureKind != ProceduralBlockKind::AlwaysFF && pb.procedureKind != ProceduralBlockKind::Always) continue;
                            std::function<void(const Statement&)> sv = [&](const Statement& st) {
                                if (st.kind == StatementKind::Timed) sv(st.as<TimedStatement>().stmt);
                                else if (st.kind == StatementKind::Block) sv(st.as<BlockStatement>().body);
                                else if (st.kind == StatementKind::List) for (auto* x : st.as<StatementList>().list) sv(*x);
                                else if (st.kind == StatementKind::Conditional) {
                                    auto& c = st.as<ConditionalStatement>();
                                    sv(c.ifTrue);
                                    if (c.ifFalse) sv(*c.ifFalse);
                                } else if (st.kind == StatementKind::ExpressionStatement) {
                                    auto& x = st.as<ExpressionStatement>().expr;
                                    if (x.kind == ExpressionKind::Assignment) {
                                        auto& l = x.as<AssignmentExpression>().left();
                                        if (l.kind == ExpressionKind::NamedValue) {
                                            const Symbol* v = &l.as<NamedValueExpression>().symbol;
                                            if (std::none_of(vars.begin(), vars.end(), [&](auto& pv) { return pv.second == v; })) vars.push_back({&in, v});
                                        }
                                    }
                                }
                            };
                            if (pb.getBody().kind == StatementKind::Timed) sv(pb.getBody());
                        }
                        for (auto& ch : in.body.membersOfType<InstanceSymbol>()) walk(ch);
                    };
                    walk(*is);
                    if (vars.empty()) fail("instance " + e + " has no register");
                } else {
                    size_t dot = full.rfind('.');
                    const InstanceSymbol* is2 = dot == std::string::npos ? nullptr : instAt(full.substr(0, dot));
                    const Symbol* v = is2 ? is2->body.find(full.substr(dot + 1)) : nullptr;
                    if (!v || !ValueSymbol::isKind(v->kind)) fail("no register " + e + " in the design");
                    vars.push_back({is2, v});
                }
                for (auto& [is, var] : vars) {
                    // the always block that assigns it
                    const ProceduralBlockSymbol* blk = nullptr;
                    std::function<bool(const Statement&)> assigns = [&](const Statement& st) -> bool {
                        switch (st.kind) {
                            case StatementKind::Timed: return assigns(st.as<TimedStatement>().stmt);
                            case StatementKind::Block: return assigns(st.as<BlockStatement>().body);
                            case StatementKind::List:
                                for (auto* x : st.as<StatementList>().list) if (assigns(*x)) return true;
                                return false;
                            case StatementKind::Conditional: {
                                auto& c = st.as<ConditionalStatement>();
                                return assigns(c.ifTrue) || (c.ifFalse && assigns(*c.ifFalse));
                            }
                            case StatementKind::ExpressionStatement: {
                                auto& x = st.as<ExpressionStatement>().expr;
                                if (x.kind != ExpressionKind::Assignment) return false;
                                auto& l = x.as<AssignmentExpression>().left();
                                return l.kind == ExpressionKind::NamedValue && &l.as<NamedValueExpression>().symbol == var;
                            }
                            default: return false;
                        }
                    };
                    for (auto& pb : is->body.membersOfType<ProceduralBlockSymbol>())
                        if (pb.getBody().kind == StatementKind::Timed && assigns(pb.getBody())) blk = &pb;
                    std::string vname(var->name);
                    if (!blk) fail("no clocked always block assigns " + vname);
                    auto& ts = blk->getBody().as<TimedStatement>();
                    struct Ev { std::string sig; int edge; };   // 1 posedge, 2 negedge
                    std::vector<Ev> evs;
                    auto addEv = [&](const TimingControl& t) {
                        if (t.kind != TimingControlKind::SignalEvent) fail("the block that assigns " + vname + " is not edge triggered");
                        auto& se = t.as<SignalEventControl>();
                        if (se.edge != EdgeKind::PosEdge && se.edge != EdgeKind::NegEdge) fail("the block that assigns " + vname + " is not edge triggered");
                        evs.push_back({text(se.expr), se.edge == EdgeKind::PosEdge ? 1 : 2});
                    };
                    if (ts.timing.kind == TimingControlKind::EventList)
                        for (auto* t : ts.timing.as<EventListControl>().events) addEv(*t);
                    else addEv(ts.timing);
                    // the next state of the clocked branch, and the async events tested by the if chain
                    const Statement* body = &ts.stmt;
                    while (body->kind == StatementKind::Block) body = &body->as<BlockStatement>().body;
                    auto mentions = [](const std::string& t, const std::string& id) {
                        std::regex re("(^|[^A-Za-z0-9_$.])" + std::regex_replace(id, std::regex(R"([.^$|()\[\]{}*+?\\])"), R"(\$&)") + "($|[^A-Za-z0-9_$])");
                        return std::regex_search(t, re);
                    };
                    std::vector<bool> isAsync(evs.size(), false);
                    if (evs.size() > 1) {
                        const Statement* b = body;
                        while (b->kind == StatementKind::Conditional) {
                            auto& c = b->as<ConditionalStatement>();
                            std::string ct = text(*c.conditions[0].expr);
                            bool any = false;
                            for (size_t i = 0; i < evs.size(); i++)
                                if (mentions(ct, evs[i].sig)) { isAsync[i] = true; any = true; }
                            if (!any || !c.ifFalse) break;
                            b = c.ifFalse;
                            while (b->kind == StatementKind::Block) b = &b->as<BlockStatement>().body;
                        }
                        body = b;
                    }
                    int clk = -1;
                    for (size_t i = 0; i < evs.size(); i++)
                        if (!isAsync[i]) {
                            if (clk >= 0) fail("the block that assigns " + vname + " has two clocks");
                            clk = (int)i;
                        }
                    if (clk < 0) fail("the block that assigns " + vname + " has no clock");
                    std::function<std::string(const Statement&, const std::string&)> next = [&](const Statement& st, const std::string& cur) -> std::string {
                        switch (st.kind) {
                            case StatementKind::Block: return next(st.as<BlockStatement>().body, cur);
                            case StatementKind::List: {
                                std::string c2 = cur;
                                for (auto* x : st.as<StatementList>().list) c2 = next(*x, c2);
                                return c2;
                            }
                            case StatementKind::Conditional: {
                                auto& c = st.as<ConditionalStatement>();
                                std::string t = next(c.ifTrue, cur), f = c.ifFalse ? next(*c.ifFalse, cur) : cur;
                                if (t == f) return t;
                                return "((" + text(*c.conditions[0].expr) + ") ? " + t + " : " + f + ")";
                            }
                            case StatementKind::ExpressionStatement: {
                                auto& x = st.as<ExpressionStatement>().expr;
                                if (x.kind != ExpressionKind::Assignment) return cur;
                                auto& a = x.as<AssignmentExpression>();
                                if (a.left().kind == ExpressionKind::NamedValue && &a.left().as<NamedValueExpression>().symbol == var)
                                    return "(" + text(a.right()) + ")";
                                return cur;
                            }
                            case StatementKind::Empty: return cur;
                            default: fail("the block that assigns " + vname + " has a statement the canonical interface is not inferred from");
                        }
                        return cur;
                    };
                    std::string data = next(*body, vname);
                    std::string clkT = (evs[(size_t)clk].edge == 1 ? "" : "!") + std::string("(") + evs[(size_t)clk].sig + ")";
                    std::string async;
                    for (size_t i = 0; i < evs.size(); i++)
                        if (isAsync[i]) async += std::string(async.empty() ? "" : " || ") + (evs[i].edge == 1 ? "(" : "!(") + evs[i].sig + ")";
                    if (async.empty()) async = "1'b0";
                    const Type& vt = var->as<ValueSymbol>().getType();
                    int w = (int)vt.getBitWidth();
                    if (w <= 0) fail(vname + " is not a vector of bits");
                    std::string instPath = std::string(is->getHierarchicalPath());   // slang's dotted path from the root
                    std::string relInst = instPath.substr(simTop.size() + (instPath.size() > simTop.size() ? 1 : 0));
                    std::string id = std::to_string(k++);
                    std::string modName = "upf_retm_mod_" + id, inst = "upf_retm_" + id;
                    std::string root = simTop.empty() ? "" : simTop + ".";
                    auto hier = [&](const std::string& upfName) { return root + m.design(upfName); };
                    // the model's ports
                    std::string ports = "input logic upf_clk, input logic [" + std::to_string(w - 1) + ":0] upf_data, input logic upf_async, "
                                        "input logic upf_save, input logic upf_restore";
                    std::string conns;
                    std::string relInstUpf = relInst;
                    {
                        std::string topPre = m.top.empty() ? "" : m.top + ".";
                        if (!topPre.empty() && relInstUpf.compare(0, topPre.size(), topPre) == 0) relInstUpf = relInstUpf.substr(topPre.size());
                        else if (relInstUpf == m.top) relInstUpf.clear();
                        for (char& ch : relInstUpf) if (ch == '.') ch = '/';
                    }
                    bool outMapped = false;
                    int nsup = 0;
                    std::string supConns;   // tied off in the bind: the runtime drives them from the supply nets
                    for (auto& mp : mports) {
                        std::string ref;
                        for (auto& [port, r2] : rm.portMap) if (port == mp.name) ref = r2;
                        bool inv = !ref.empty() && ref[0] == '~';
                        std::string x = inv ? ref.substr(1) : ref;
                        std::string neg = inv ? "~" : "";
                        std::string e2;
                        if (ref.empty()) {
                            if (mp.dir == 1 && std::count_if(mports.begin(), mports.end(), [](const MP& y) { return y.dir == 1; }) == 1 &&
                                std::none_of(rm.portMap.begin(), rm.portMap.end(), [](auto& pm) { return pm.second == "UPF_GENERIC_OUTPUT"; })) {
                                e2 = "upf_q";
                                outMapped = true;
                            } else if (mp.dir == 0) fail("input port " + mp.name + " of " + rm.model + " is not connected by -port_map");
                            else continue;
                        } else if (x == "UPF_GENERIC_CLOCK") e2 = neg + "upf_clk";
                        else if (x == "UPF_GENERIC_DATA") e2 = neg + "upf_data";
                        else if (x == "UPF_GENERIC_ASYNC_SET_RESET" || x == "UPF_GENERIC_ASYNC_LOAD") e2 = neg + "upf_async";
                        else if (x == "UPF_GENERIC_OUTPUT") {
                            if (mp.dir != 1) fail("UPF_GENERIC_OUTPUT goes to an output port, not " + mp.name);
                            e2 = "upf_q";
                            outMapped = true;
                        } else if (x == "save_signal") e2 = neg + "upf_save";
                        else if (x == "restore_signal") e2 = neg + "upf_restore";
                        else {
                            // a supply: a supply net or port, retention_supply.f, primary_supply.f
                            std::string net;
                            std::string j = Model::join(rm.scope, x);
                            if (x.rfind("retention_supply.", 0) == 0) net = m.fnNet(r->supply + x.substr(16));
                            else if (x.rfind("primary_supply.", 0) == 0) net = m.fnNet(m.primaryOf(rm.domain) + x.substr(14));
                            else if (m.nets.count(j) || m.ports.count(j)) net = j;
                            else if (!m.fnNet(j).empty()) net = m.fnNet(j);
                            if (!net.empty()) {
                                if (mp.dir != 0) fail("supply " + x + " goes to an input port, not " + mp.name);
                                std::string sp = "upf_sup_" + std::to_string(nsup++);
                                ports += std::string(", input ") + (mp.supply ? "UPF::upfSupplyTypeT " : "logic ") + sp;
                                e2 = sp;
                                supConns += ", ." + sp + "('0)";
                                std::string upfPort = (relInstUpf.empty() ? "" : relInstUpf + "/") + inst + "/" + sp;
                                m.connects[net].push_back(upfPort);
                                m.connectWhere[{net, upfPort}] = rm.where;
                            } else {
                                // a logic net
                                if (inv) fail("~ is only for the symbolic references, not " + x);
                                e2 = hier(j);
                            }
                        }
                        conns += std::string(conns.empty() ? "" : ", ") + "." + mp.name + "(" + e2 + ")";
                    }
                    if (!outMapped) fail("no output port of " + rm.model + " drives " + vname + "; map one to UPF_GENERIC_OUTPUT");
                    std::string sv = r->save.net.empty() ? "1'b0" : hier(r->save.net), rs = r->restore.net.empty() ? "1'b0" : hier(r->restore.net);
                    out += "// map_retention_cell " + rm.strategies[0] + " -lib_model_name " + rm.model + ": " + relInst + "." + vname +
                           " (IEEE 1801-2024 6.39)\nmodule " + modName + "(" + ports + ");\n  logic [" + std::to_string(w - 1) +
                           ":0] upf_q;\n  " + mod + " u_model(" + conns + ");\nendmodule\nbind " + instPath + " " + modName + " " + inst +
                           "(.upf_clk(" + clkT + "), .upf_data(" + data + "), .upf_async(" + async + "), .upf_save(" + sv +
                           "), .upf_restore(" + rs + ")" + supConns + ");\n";
                    RetModelEl el;
                    el.el = relInst + "." + vname;
                    el.inst = relInst + "." + inst;
                    el.q = el.inst + ".upf_q";
                    el.strategy = r->name;
                    retModelEls.push_back(el);
                    retModelInsts.push_back(el.inst);
                    note("map_retention_cell: " + el.el + " is simulated by " + rm.model + " (" + el.inst + ")");
                }
            }
        }
        return out;
    }
#endif
    void copyRetModels() {
        for (auto& e : retModelEls) {
            if (e.qSig < 0) continue;
            Val v = S.get(e.qSig);
            if (e.copied && v == e.last) continue;
            e.copied = true;
            e.last = v;
            S.holdOn(e.elSig, konst(v), true);
        }
    }
    // use_interface_cell -force_function (6.62): an isolation cell of the
    // strategy is simulated by the first of -lib_cells, an HDL module. For
    // each port the strategy isolates (and the command selects), a wrapper
    // bound into the power top holds an instance of the model; the model's
    // one input and one output that -port_map leaves open are the data
    // input and output. The runtime holds the data input at the port's
    // value and the port takes the model's output; the strategy's
    // -isolation_sense does not apply. The cell's output is still corrupted
    // while the isolation supply is not NORMAL (9.8).
    struct IfcEl { std::string port, d, y; int dSig = -1, ySig = -1; bool held = false; Val last; };
    std::vector<IfcEl> ifcEls;
    std::map<std::string, int> ifcElOf;   // "inst.port" -> ifcEls index
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    std::string ifcModelSource(slang::ast::Compilation& comp, const std::string& simTop) {
        using namespace slang::ast;
        std::string out;
        DomainMap dm2;
        dm2.build(m);
        // the port connections, named as the runtime names them
        struct Site { PortSite p; std::string model; };
        std::vector<Site> sites;
        std::function<void(const Scope&, const std::string&)> walk = [&](const Scope& sc, const std::string& path) {
            auto in = [&](const std::string& n) { return path.empty() ? n : path + "." + n; };
            for (const Symbol& x : sc.members()) {
                if (x.kind == SymbolKind::Instance) {
                    const auto& ch = x.as<InstanceSymbol>();
                    std::string p = in(std::string(ch.name));
                    for (const PortConnection* c : ch.getPortConnections()) {
                        if (!c || c->port.kind != SymbolKind::Port || !c->getExpression()) continue;
                        const auto& ps = c->port.as<PortSymbol>();
                        Site st;
                        st.p.inst = p;
                        st.p.port = std::string(ps.name);
                        st.p.dir = ps.direction == ArgumentDirection::In ? 0 : ps.direction == ArgumentDirection::Out ? 1 : 2;
                        st.p.width = (int)ps.getType().getBitWidth();
                        st.model = std::string(ch.getDefinition().name);
                        sites.push_back(st);
                    }
                    walk(ch.body, p);
                } else if (x.kind == SymbolKind::GenerateBlock) {
                    const auto& g = x.as<GenerateBlockSymbol>();
                    if (!g.isUninstantiated) walk(g, in(g.getExternalName()));
                } else if (x.kind == SymbolKind::GenerateBlockArray) {
                    const auto& ga = x.as<GenerateBlockArraySymbol>();
                    std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
                    for (const auto* e : ga.entries)
                        if (!e->isUninstantiated)
                            walk(*e, in(an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "]"));
                }
            }
        };
        const InstanceSymbol* topInst = nullptr;
        for (auto* t : comp.getRoot().topInstances) if (std::string(t->name) == simTop) topInst = t;
        if (!topInst) return out;
        walk(topInst->body, "");
        // the wrappers go into the power top
        std::string ptRel = m.top;   // design path of the power top, relative to simTop
        std::string ptFull = simTop + (ptRel.empty() ? "" : "." + ptRel);
        std::string root = simTop + ".";
        auto hier = [&](const std::string& upfName) { return root + m.design(upfName); };
        int k = 0;
        for (auto& im : m.ifcModels) {
            auto fail = [&](const std::string& msg) -> void { throw Error(im.where + ": use_interface_cell -force_function " + im.model + ": " + msg + " (6.62)"); };
            const Isolation* is = nullptr;
            for (auto& x : m.isos) if (x.name == im.strategy && x.domain == im.domain) is = &x;
            const LevelShifter* ls = nullptr;
            for (auto& x : m.lss) if (x.name == im.ls && x.domain == im.domain) ls = &x;
            std::string mod = im.model.substr(im.model.rfind('.') == std::string::npos ? 0 : im.model.rfind('.') + 1);
            auto def = comp.tryGetDefinition(mod, comp.getRoot());
            if (!def.definition || def.definition->kind != SymbolKind::Definition) {
                // a library cell with no HDL model (define_isolation_cell, Liberty): the 9.8 algorithm
                std::cerr << "msim: UPF: " << im.where << ": use_interface_cell -force_function: no HDL module " << im.model
                          << "; isolation " << im.strategy << " is simulated as 9.8 describes\n";
                continue;
            }
            auto& minst = InstanceSymbol::createDefault(comp, def.definition->as<DefinitionSymbol>());
            struct MP { std::string name; int dir; bool supply; int w; };
            std::vector<MP> mports;
            for (const Symbol* q : minst.body.getPortList()) {
                if (q->kind != SymbolKind::Port) fail(im.model + " has a port that is not a plain port");
                auto& qp = q->as<PortSymbol>();
                int dir = qp.direction == ArgumentDirection::In ? 0 : qp.direction == ArgumentDirection::Out ? 1 : 2;
                if (dir == 2) fail(im.model + " has an inout port " + std::string(qp.name));
                mports.push_back({std::string(qp.name), dir, supplyType(qp.getType()), (int)qp.getType().getBitWidth()});
            }
            for (auto& [port, ref] : im.portMap)
                if (std::none_of(mports.begin(), mports.end(), [&](const MP& x) { return x.name == port; }))
                    fail(im.model + " has no port " + port);
            // the data input and output: the ports -port_map leaves open
            const MP *din = nullptr, *dout = nullptr;
            for (auto& mp : mports) {
                if (std::any_of(im.portMap.begin(), im.portMap.end(), [&](auto& pm) { return pm.first == mp.name; })) {
                    if (mp.dir == 1) fail("-port_map connects output port " + mp.name + "; only the data output is an output");
                    continue;
                }
                const MP*& slot = mp.dir == 0 ? din : dout;
                if (slot) fail("port " + mp.name + " is not connected: " + slot->name + " is already the data " + (mp.dir == 0 ? "input" : "output"));
                slot = &mp;
            }
            if (!din) fail(im.model + " has no data input: -port_map connects every input");
            if (!dout) fail(im.model + " has no data output");
            if (din->w != dout->w) fail("data input " + din->name + " and output " + dout->name + " differ in width");
            int made = 0;
            for (auto& st : sites) {
                const PortSite& p = st.p;
                if (p.dir == 2) continue;
                std::string child = dm2.of(p.inst), parent = dm2.of(parentOf(p.inst));
                if (child == parent || m.portAttr(p.inst, st.model, p.port, p.dir, "UPF_is_analog") == "TRUE") continue;
                Crossing c{&p, p.dir == 1 ? child : parent, p.dir == 1 ? parent : child};
                const Isolation* first = nullptr;
                for (auto& x : m.isos) if (isoApplies(m, x, c)) { first = &x; break; }
                if (first != is || is->noIso) continue;   // one strategy per port: the first that applies
                if (!im.elements.empty() && !namesPort(m, im.elements, p)) continue;
                if (!im.exclude.empty() && namesPort(m, im.exclude, p)) continue;
                std::string clamp = is->clamp;
                if (std::string pc = m.portAttr(p.inst, st.model, p.port, p.dir, "UPF_clamp_value");
                    !is->clampSet && (pc == "0" || pc == "1" || pc == "Z" || pc == "latch"))
                    clamp = pc;
                if (!im.clamp.empty() && im.clamp != clamp) continue;
                int W = p.width;
                if (din->w != W && din->w != 1)
                    fail("the data ports of " + im.model + " are " + std::to_string(din->w) + " bits; port " + p.inst + "." + p.port + " is " + std::to_string(W));
                std::string id = std::to_string(k++);
                std::string modName = "upf_isom_mod_" + id, inst = "upf_isom_" + id;
                std::string rel = ptRel.empty() ? inst : ptRel + "." + inst;
                std::string ports, conns, bindConns;
                int nsup = 0;
                for (auto& mp : mports) {
                    std::string e2;
                    if (&mp == din) e2 = "upf_d";
                    else if (&mp == dout) e2 = "upf_y";
                    else {
                        std::string ref;
                        for (auto& [port, r2] : im.portMap) if (port == mp.name) ref = r2;
                        bool inv = ref[0] == '~';
                        std::string x = inv ? ref.substr(1) : ref;
                        static const std::regex sigRef(R"(isolation_signal(\[([0-9]+)\])?)");
                        static const std::regex fnRef(R"((isolation_supply|input_supply|output_supply)(\[[0-9]+\])?\.(\w+))");
                        std::smatch mm;
                        std::string sp = "upf_in_" + std::to_string(nsup++);
                        if (std::regex_match(x, mm, sigRef)) {
                            size_t i = mm[2].matched ? std::stoul(mm[2].str()) : 0;
                            ports += std::string(ports.empty() ? "" : ", ") + "input logic " + sp;
                            bindConns += std::string(bindConns.empty() ? "" : ", ") + "." + sp + "(" + hier(is->signals[i]) + ")";
                            e2 = (inv ? "~" : "") + sp;
                        } else {
                            std::string net, j = Model::join(im.scope, x);
                            if (std::regex_match(x, mm, fnRef)) {
                                std::string k2 = mm[1].str(), set = k2 == "isolation_supply" ? m.setOf(is->supply)
                                                                  : k2 == "input_supply"   ? m.setOf(ls->inSupply)
                                                                                           : m.setOf(ls->outSupply);
                                net = m.fnNet(set + "." + mm[3].str());
                                if (net.empty()) fail("supply set " + set + " has no function " + mm[3].str() + " for " + mp.name);
                            } else if (m.nets.count(j) || m.ports.count(j)) net = j;
                            else if (!m.fnNet(j).empty()) net = m.fnNet(j);
                            if (!net.empty()) {
                                ports += std::string(ports.empty() ? "" : ", ") + "input " + (mp.supply ? "UPF::upfSupplyTypeT " : "logic ") + sp;
                                bindConns += std::string(bindConns.empty() ? "" : ", ") + "." + sp + "('0)";
                                std::string upfPort = inst + "/" + sp;   // a UPF path from the power top
                                m.connects[net].push_back(upfPort);
                                m.connectWhere[{net, upfPort}] = im.where;
                            } else {   // a logic net
                                ports += std::string(ports.empty() ? "" : ", ") + "input logic [" + std::to_string(mp.w - 1) + ":0] " + sp;
                                bindConns += std::string(bindConns.empty() ? "" : ", ") + "." + sp + "(" + hier(j) + ")";
                            }
                            e2 = sp;
                        }
                    }
                    conns += std::string(conns.empty() ? "" : ", ") + "." + mp.name + "(" + e2 + ")";
                }
                std::string range = "[" + std::to_string(W - 1) + ":0]";
                bool arr = din->w != W;   // a one-bit cell on each bit
                out += "// use_interface_cell -force_function " + im.model + ": " + p.inst + "." + p.port +
                       " (IEEE 1801-2024 6.62)\nmodule " + modName + "(" + ports + ");\n  logic " + range + " upf_d;\n  wire " + range +
                       " upf_y;\n  " + mod + " u_model" + (arr ? range : std::string()) + "(" + conns + ");\nendmodule\nbind " + ptFull +
                       " " + modName + " " + inst + "(" + bindConns + ");\n";
                IfcEl el;
                el.port = p.inst + "." + p.port;
                el.d = rel + ".upf_d";
                el.y = rel + ".upf_y";
                ifcElOf[el.port] = (int)ifcEls.size();
                ifcEls.push_back(el);
                retModelInsts.push_back(rel);
                note("use_interface_cell: isolation " + is->name + " on " + el.port + " is simulated by " + im.model + " (" + rel + ")");
                made++;
            }
            if (!made) std::cerr << "msim: UPF: " << im.where << ": use_interface_cell: isolation " << im.strategy << " -force_function applies to no port\n";
        }
        return out;
    }
#endif
    // The HDL ports that drive supply nets: their value is the root supply.
    void readHdlPorts() {
        for (auto& h : hdlPorts) {
            if (!h.hdl2upf) continue;
            Val v = S.get(h.sig);
            if (h.fOff >= 0) v = v_slice(v, h.fOff + h.w - 1, h.fOff);   // -field
            SupplyVal sv;
            if (h.supply) {   // {state, voltage in microvolts}, state in the upper word
                if (v.x) sv.st = S_UNDETERMINED;
                else {
                    uint64_t st = (v.v >> 32) & 0xffffffffu;
                    sv.st = st <= 3 ? (int)st : S_UNDETERMINED;
                    sv.v = (double)(int32_t)(uint32_t)(v.v & 0xffffffffu) / 1e6;
                    sv.hasV = true;
                }
            } else if (h.vcm) {
                uint64_t mask = h.w >= 64 ? ~0ull : ((1ull << h.w) - 1);
                bool unknown = (v.x & mask) != 0;
                double x = 0;
                std::string label;
                if (h.cat == 'r') {
                    x = Vcm::bitsReal(v.v);
                    if (v.v == Vcm::XrealBits || v.v == Vcm::ZrealBits) unknown = true;
                } else {
                    uint64_t u = v.v & mask;
                    int64_t iv = (int64_t)u;
                    if (h.sgn && h.w < 64 && (u >> (h.w - 1)) & 1) iv = (int64_t)(u | ~mask);
                    x = (double)iv;
                    if (h.cat == 'e') {
                        auto it = h.labelOf.find(iv);
                        if (it != h.labelOf.end()) label = it->second;
                        else unknown = true;
                    }
                }
                int st;
                bool setV;
                double nv = 0;
                h.vcm->toUpf(x, label, unknown, v.v, st, setV, nv);
                sv.st = st;
                if (setV) { h.lastV = nv; h.hasV = true; }
                sv.v = h.lastV;
                sv.hasV = h.hasV;
            } else {
                char c = (v.z & 1) ? 'Z' : (v.x & 1) ? 'X' : (v.v & 1) ? '1' : '0';
                sv.st = h.vct->toUpf(c);
            }
            drv[root(h.net)] = sv;
        }
    }
    // The HDL ports that supply nets drive: held at the converted value.
    void driveHdlPorts() {
        for (auto& h : hdlPorts) {
            if (h.hdl2upf) continue;
            auto it = val.find(root(h.net));
            SupplyVal sv = it == val.end() ? SupplyVal{} : it->second;
            if (h.supply || h.vcm) {
                Val out;
                if (h.supply) {
                    int64_t uv = (int64_t)std::llround(sv.v * 1e6);
                    out = make(((uint64_t)sv.st << 32) | (uint32_t)uv, 0, 64, 0);
                } else {
                    const Vcm::Row* r = h.vcm->toHdl(sv.st, sv.v);
                    if (!r) {   // 6.30.4 f
                        if (!h.warned)
                            error("supply net " + h.net + " is " + stateName(sv.st) + (sv.hasV ? " " + std::to_string(sv.v) : "") +
                                  ", which VCM " + h.vcm->name + " has no row for (6.30.4)");
                        h.warned = true;
                        continue;
                    }
                    h.warned = false;
                    uint64_t mask = h.w >= 64 ? ~0ull : ((1ull << h.w) - 1);
                    switch (r->h) {
                        case Vcm::Row::Pass: out = mkreal(sv.v); break;
                        case Vcm::Row::XReal: out = mkreal(Vcm::bitsReal(Vcm::XrealBits)); break;
                        case Vcm::Row::ZReal: out = mkreal(Vcm::bitsReal(Vcm::ZrealBits)); break;
                        case Vcm::Row::Label: out = make((uint64_t)h.valueOf.at(r->label) & mask, 0, h.w, 0); break;
                        default:
                            out = h.cat == 'r' ? mkreal(r->num) : make((uint64_t)(int64_t)r->num & mask, 0, h.w, 0);
                    }
                }
                hold(h, out);
                continue;
            }
            char c = h.vct->toHdl(sv.st);
            if (h.fOff < 0 && c == h.last) continue;
            h.last = c;
            bool one = c == '1' || c == 'H', zero = c == '0' || c == 'L', z = c == 'Z';
            hold(h, make(one ? 1 : 0, one || zero || z ? 0 : 1, 1, z ? 1 : 0));
        }
    }
    // Holds an HDL port at a converted value. With -field the value goes into
    // the field's bits; the other fields keep what the other VCMs of the
    // port put there (x until then).
    std::map<int, Val> fieldHeld;
    void hold(HdlPort& h, Val out) {
        if (h.fOff >= 0) {
            int W = S.sigs[(size_t)h.sig].w;
            auto it = fieldHeld.find(h.sig);
            if (it == fieldHeld.end()) it = fieldHeld.emplace(h.sig, allx(W)).first;
            Val f = make(out.v, out.x, h.w, out.z);
            f.hi = out.hi;
            std::vector<Val> parts;
            if (h.fOff + h.w < W) parts.push_back(v_slice(it->second, W - 1, h.fOff + h.w));
            parts.push_back(f);
            if (h.fOff > 0) parts.push_back(v_slice(it->second, h.fOff - 1, 0));
            it->second = v_concat(parts);
            out = it->second;
        }
        if (h.held && out == h.lastVal) return;
        h.held = true;
        h.lastVal = out;
        S.holdOn(h.sig, konst(out), true);
    }

    // Isolation and level-shifter cells on the port connections that cross
    // a domain boundary.
#ifndef UPF_RT_NO_DESIGN   // simv (msim -compile) has no slang design
    void bindCells(LowerResult& lw) {
        std::vector<PortSite> sites;
        std::vector<size_t> procOf;
        std::set<std::string> hasLogic;   // instances with a process in or below them
        for (auto& lp : lw.procs) {
            if (!lp.port.empty() || lp.kind == "upf_wire") continue;
            for (std::string sc = lp.scope; !sc.empty(); sc = parentOf(sc)) hasLogic.insert(sc);
        }
        for (size_t i = 0; i < lw.procs.size(); i++) {
            auto& lp = lw.procs[i];
            if (lp.port.empty()) continue;
            PortSite p;
            size_t d = lp.port.rfind('.');
            p.inst = lp.port.substr(0, d);
            p.port = lp.port.substr(d + 1);
            p.dir = lp.portDir;
            sites.push_back(p);
            procOf.push_back(i);
        }
        for (size_t k = 0; k < sites.size(); k++) {
            const PortSite& p = sites[k];
            if (p.dir == 2) continue;
            std::string child = dm.of(p.inst), parent = dm.of(parentOf(p.inst));
            std::string model = modelOf(p.inst);
            auto attr = [&](const char* name) { return m.portAttr(p.inst, model, p.port, p.dir, name); };
            bool cross = child != parent && attr("UPF_is_analog") != "TRUE";   // analog ports take no strategy
            Crossing c{&p, p.dir == 1 ? child : parent, p.dir == 1 ? parent : child};
            std::vector<int> made;
            // The driver of the port is not in the design (6.51): a primary
            // input of the power top, or an output of an instance with no
            // logic in it. The port is corrupted with UPF_driver_supply.
            // PORT_CORR_ONLY (6.56): the outputs of a hard macro follow its supply.
            {
                std::string ds = attr("UPF_driver_supply");
                bool empty = !hasLogic.count(p.inst);
                std::string portSupply;
                if (!ds.empty()) {
                    if ((p.dir == 0 && p.inst == m.top) || (p.dir == 1 && empty)) portSupply = m.setOf(ds);
                    else if (p.dir == 1) {
                        std::string actual = supplySetOf(p.inst);
                        if (!actual.empty() && m.eqOf(actual) != m.eqOf(ds))
                            throw Error("set_port_attributes -driver_supply " + ds + " on " + p.inst + "." + p.port +
                                        ": the logic that drives it is on " + actual + " (6.51)");
                    }
                }
                auto [beh, at] = behaviorOf(p.inst);
                if (portSupply.empty() && p.dir == 1 && beh == "PORT_CORR_ONLY" && at == p.inst) portSupply = supplySetOf(p.inst);
                if (!portSupply.empty() && sets.count(portSupply)) {
                    Cell cell;
                    cell.kind = 2;
                    cell.port = p.inst + "." + p.port;
                    cell.supplies.push_back(portSupply);
                    made.push_back((int)cells.size());
                    cells.push_back(cell);
                    note("port " + cell.port + " is corrupted with supply set " + portSupply);
                }
            }
            const Isolation* isP = cross ? pickStrategy(m, m.isos, c, isoApplies, +[](const Isolation& x) { return x.noIso; }, "isolation") : nullptr;
            if (isP && !isP->noIso) {
                const Isolation& is = *isP;
                Cell cell;
                cell.kind = 0;
                cell.iso = &is;
                cell.port = p.inst + "." + p.port;
                // the port's own clamp requirement, when the strategy gives none (6.51)
                cell.clamp = is.clamp;
                if (std::string pc = attr("UPF_clamp_value"); !is.clampSet && (pc == "0" || pc == "1" || pc == "Z" || pc == "latch")) cell.clamp = pc;
                if (!is.supply.empty()) cell.supplies.push_back(m.setOf(is.supply));
                for (size_t q = 0; q < is.signals.size(); q++) cell.senseLow.push_back(q < is.senseLow.size() ? is.senseLow[q] : 0);
                if (auto f = ifcElOf.find(cell.port); f != ifcElOf.end()) cell.model = f->second;
                if (is.signals.empty()) throw Error(is.where + ": isolation " + is.name + " has no -isolation_signal");
                made.push_back((int)cells.size());
                cells.push_back(cell);
                note("isolation " + is.name + " on " + cell.port);
            }
            const LevelShifter* lsP = cross ? pickStrategy(m, m.lss, c, lsApplies, +[](const LevelShifter& x) { return x.noShift; }, "level shifter") : nullptr;
            if (lsP && !lsP->noShift) {
                const LevelShifter& l = *lsP;
                Cell cell;
                cell.kind = 1;
                cell.port = p.inst + "." + p.port;
                std::string in = l.inSupply.empty() ? (c.from.empty() ? "" : m.primaryOf(c.from)) : m.setOf(l.inSupply);
                std::string out = l.outSupply.empty() ? (c.to.empty() ? "" : m.primaryOf(c.to)) : m.setOf(l.outSupply);
                if (!in.empty()) cell.supplies.push_back(in);
                if (!out.empty()) cell.supplies.push_back(out);
                made.push_back((int)cells.size());
                cells.push_back(cell);
            }
            // 9.10: the repeater's output follows its supply's simstate
            if (const Repeater* rpP = cross ? pickStrategy(m, m.reps, c, repApplies, +[](const Repeater&) { return false; }, "repeater") : nullptr) {
                const Repeater& rp = *rpP;
                Cell cell;
                cell.kind = 2;
                cell.port = p.inst + "." + p.port;
                std::string sn = m.domains.count(rp.supply) ? m.primaryOf(rp.supply) : m.setOf(rp.supply);
                if (!sets.count(sn)) throw Error(rp.where + ": repeater " + rp.name + ": no supply set '" + rp.supply + "'");
                cell.supplies.push_back(sn);
                made.push_back((int)cells.size());
                cells.push_back(cell);
                note("repeater " + rp.name + " on " + cell.port);
            }
            if (made.empty()) continue;
            // Wrap the connection's value in the cells (isolation first, at
            // the driver side), and wake it on the control signals and the
            // cells' supplies.
            Prog& pr = lw.procs[procOf[k]].prog;
            std::vector<std::pair<int, Edge>> more;
            for (int ci : made) {
                const Cell& cell = cells[(size_t)ci];
                for (auto& sn : cell.supplies)
                    if (sets.count(sn)) more.push_back({sets[sn].sig, ANY});
                if (cell.model >= 0) more.push_back({ifcEls[(size_t)cell.model].ySig, ANY});
                if (cell.kind == 0)
                    for (auto& s : cell.iso->signals) {
                        int sg = sigOf(s);
                        if (sg < 0) throw Error(cell.iso->where + ": isolation " + cell.iso->name + ": no signal '" + m.design(s) + "' in the design");
                        more.push_back({sg, ANY});
                    }
            }
            for (auto& in : pr.code)
                if (in.k == IK::Assign) {
                    for (int ci : made) {
                        std::vector<E> kids{in.e};
                        if (cells[(size_t)ci].kind == 0)
                            for (auto& s : cells[(size_t)ci].iso->signals) kids.push_back(ref(sigOf(s)));
                        auto n = node(Op::SysFn, kids);
                        n->hi = SF_UPF;
                        n->lo = (cells[(size_t)ci].kind == 0 ? UPF_ISO : UPF_LS) | (ci << 8);
                        in.e = n;
                    }
                }
            bool hasWait = false;
            for (auto& in : pr.code)
                if (in.k == IK::WaitEdge) {
                    hasWait = true;
                    std::vector<std::pair<int, Edge>> ev = in.events.size() > 1 ? in.events : std::vector<std::pair<int, Edge>>{{in.sig, in.edge}};
                    for (auto& x : more) ev.push_back(x);
                    in = waitInstr(ev);
                }
            if (!hasWait) {
                pr.emit(waitInstr(more));
                Instr j; j.k = IK::Jmp; j.target = 0;
                pr.emit(j);
                lw.procs[procOf[k]].loops = true;
            }
        }
    }
#endif

    // Which retention strategy, if any, retains state element s (5.7.3):
    // the most specific element wins (the signal, its instance, the lowest
    // ancestor, the whole domain), then -no_retention.
    const Retention* retentionOf(int s, int dom) {
        const std::string& n = S.sigs[(size_t)s].name;
        const Retention* best = nullptr;
        const Retention* tie = nullptr;
        long br = -1;
        for (auto& r : m.rets) {
            if (domIdx.at(r.domain) != dom) continue;
            long g = r.elements.empty() ? 0 : -1;
            for (auto& e : r.elements) {
                std::string d = m.design(e);
                if (!DomainMap::within(n, d)) continue;
                long ge = d == n ? 4000 : d == parentOf(n) ? 3000 : 1000 + (long)std::count(d.begin(), d.end(), '.');
                g = std::max(g, ge);
            }
            for (auto& e : r.exclude) if (DomainMap::within(n, m.design(e))) g = -1;
            if (r.appliesTo == "flop" && latchSigs.count(s)) g = -1;   // -applies_to (6.54)
            if (r.appliesTo == "latch" && !latchSigs.count(s)) g = -1;
            if (g < 0) continue;
            long rank = g * 2 + (r.noRet ? 1 : 0);
            if (rank > br) { best = &r; br = rank; tie = nullptr; }
            else if (rank == br) tie = &r;
        }
        if (tie && !best->noRet)
            throw Error(tie->where + ": retention strategies " + best->name + " and " + tie->name + " both apply to " + n +
                        " with the same precedence (5.7.3, 5.7.10)");
        return best;
    }
    void bindRetention() {
        for (auto& r : m.rets) {
            if (r.noRet) continue;
            RetRt rr;
            rr.r = &r;
            rr.dom = domIdx.at(r.domain);
            rr.saveSig = sigOf(r.save.net);
            rr.restoreSig = sigOf(r.restore.net);
            if (rr.saveSig < 0) throw Error(r.where + ": retention " + r.name + ": no signal '" + m.design(r.save.net) + "' in the design");
            if (rr.restoreSig < 0) throw Error(r.where + ": retention " + r.name + ": no signal '" + m.design(r.restore.net) + "' in the design");
            rr.single = r.save.net == r.restore.net && r.save.sense < 2 && r.restore.sense < 2 && r.save.sense != r.restore.sense;
            for (int s : stateSigs[(size_t)rr.dom])
                if (retentionOf(s, rr.dom) == &r) rr.els.push_back({s, allx(S.sigs[(size_t)s].w), false});
            bool modelled = std::any_of(retModelEls.begin(), retModelEls.end(), [&](const RetModelEl& e) { return e.strategy == r.name; });
            if (rr.els.empty() && !modelled)
                std::cerr << "msim: UPF: " << r.where << ": retention " << r.name << " applies to no state element\n";
            note("retention " + r.name + ": " + std::to_string(rr.els.size()) + " state elements");
            rets.push_back(std::move(rr));
        }
    }

    // ------------------------------------------------------------ evaluation
    // A power switch's output (6.23): its states' contributions. cat is the
    // state class for -ack_port: 0 off, 1 on, 2 partial on, 3 error.
    EvalCtx swCtx(const Switch& sw) {
        EvalCtx c;
        c.id = [this, &sw](const std::string& id, EVal& out) {
            for (auto& [p, net] : sw.ctrls)
                if (p == id) {
                    int s = sigOf(net);
                    if (s < 0) return false;
                    out = logicOf(s);
                    return true;
                }
            int s = sigOf(Model::join(scopeOfName(sw.name), id));
            if (s >= 0) { out = logicOf(s); return true; }
            return false;
        };
        c.inState = [](const std::string&, const std::string&) { return false; };
        return c;
    }
    SupplyVal evalSwitch(const Switch& sw) {
        EvalCtx c = swCtx(sw);
        auto inVal = [&](const std::string& port) {
            auto it = val.find(root(sw.name + "/" + port));
            return it == val.end() ? SupplyVal{} : it->second;
        };
        SupplyVal und{S_UNDETERMINED, 0, false};
        for (auto& e : sw.error)
            if (truthOf(evalEx(e.e, c, sw.where)) == 1) { swNote(sw, e.name, 3); return und; }
        bool offTrue = false;
        std::string st;
        for (auto& e : sw.off) if (truthOf(evalEx(e.e, c, sw.where)) == 1) { offTrue = true; st = e.name; }
        std::vector<SupplyVal> contrib;
        bool anyOn = false;
        std::string onName, partName;
        auto take = [&](const std::vector<SwitchState>& v, bool partial) {
            for (auto& e : v) {
                SupplyVal in = inVal(e.inPort);
                int t = truthOf(evalEx(e.e, c, sw.where));
                if (t == 2 && in.st != S_OFF) { contrib.push_back(und); anyOn = true; continue; }
                if (t != 1) continue;
                anyOn = true;
                (partial ? partName : onName) = e.name;
                if (partial && in.st == S_FULL_ON) in.st = S_PARTIAL_ON;
                contrib.push_back(in);
            }
        };
        take(sw.on, false);
        take(sw.partial, true);
        if (offTrue && anyOn) { swNote(sw, "ERROR", 3); return und; }
        for (size_t i = 0; i < contrib.size(); i++) {
            if (contrib[i].st == S_UNDETERMINED) { swNote(sw, "UNDETERMINED", 3); return und; }
            for (size_t j = 0; j < i; j++)
                if (contrib[i].st != S_OFF && contrib[j].st != S_OFF && contrib[i].hasV && contrib[j].hasV &&
                    std::fabs(contrib[i].v - contrib[j].v) > 1e-9) { swNote(sw, "UNDETERMINED", 3); return und; }
        }
        if (!onName.empty() || !partName.empty()) swNote(sw, onName.empty() ? partName : onName, onName.empty() ? 2 : 1);
        else swNote(sw, st.empty() ? "OFF" : st, 0);
        for (auto& v : contrib) if (v.st == S_FULL_ON) return v;
        for (auto& v : contrib) if (v.st == S_PARTIAL_ON) return v;
        return SupplyVal{};
    }
    std::map<std::string, int> swCat;   // switch -> its state class
    std::map<std::string, int> swPrevCat;
    void swNote(const Switch& sw, const std::string& st, int cat) {
        std::string& old = swState[sw.name];
        auto pc = swCat.find(sw.name);
        int prevCat = pc == swCat.end() ? -1 : pc->second;
        if (old != st) {
            note("power switch " + sw.name + " state " + st);
            if (cat == 3) error("power switch " + sw.name + " is in an error state");
        }
        if (prevCat != cat) swPrevCat[sw.name] = prevCat;
        old = st;
        swCat[sw.name] = cat;
        // -ack_port (6.23, 9.2.2): a constant acknowledges turning on (and
        // its inverse turning off); an expression of the control ports is
        // driven as it is; an error state drives x.
        EvalCtx c = swCtx(sw);
        for (auto& a : sw.acks) {
            if (a.net.empty()) continue;
            int s = sigOf(a.net);
            if (s < 0) continue;
            Val v;
            if (cat == 3) v = allx(1);
            else if (a.e->k == Ex::Num) {
                int k = (int)(a.e->v & 1);
                int pcat = swPrevCat[sw.name];
                bool on = cat == 1 || (cat == 2 && pcat == 0);
                v = make((uint64_t)(on ? k : !k), 0, 1, 0);
            } else {
                int t = truthOf(evalEx(a.e, c, sw.where));
                v = t == 2 ? allx(1) : make((uint64_t)t, 0, 1, 0);
            }
            std::string key = sw.name + "/" + a.port;
            auto it = ackVal.find(key);
            if (it != ackVal.end() && it->second == v) continue;
            ackVal[key] = v;
            uint64_t gen = ++ackGen[key];
            if (a.delay) S.spawn(delayedAck(s, v, a.delay, key, gen));
            else S.set(s, v);
        }
    }
    std::map<std::string, Val> ackVal;
    std::map<std::string, uint64_t> ackGen;
    // An inertial delay: a newer acknowledge value cancels this one.
    static Task delayedAck(int s, Val v, uint64_t d, std::string key, uint64_t gen) {
        co_await Delay{d};
        if (rt().ackGen[key] == gen) S.set(s, v);
    }

    void evalSupplies() {
        for (int iter = 0; iter < 32; iter++) {
            bool changed = false;
            for (auto& [r, v] : val) {
                SupplyVal nv;
                auto sw = swOf.find(r);
                if (sw != swOf.end()) {
                    std::vector<SupplyVal> outs;
                    for (auto& n : sw->second) outs.push_back(evalSwitch(m.switches.at(n)));
                    nv = outs[0];
                    if (outs.size() > 1) {   // parallel / one_hot resolution (6.25.3)
                        int on = 0, part = 0;
                        SupplyVal best;
                        for (auto& o : outs) {
                            if (o.st == S_UNDETERMINED) { best = o; on = 99; break; }
                            if (o.st == S_FULL_ON) { on++; best = o; }
                            else if (o.st == S_PARTIAL_ON) { part++; if (!on) best = o; }
                        }
                        nv = best;
                        std::string res;
                        for (auto& [n2, rs] : m.resolve) if (root(n2) == r) res = rs;
                        if ((res == "one_hot" || res == "parallel_one_hot") && on + part > 1) nv = SupplyVal{S_UNDETERMINED, 0, false};
                        if (res == "parallel" && on == 0 && part) nv.st = S_PARTIAL_ON;
                    }
                } else {
                    auto d = drv.find(r);
                    if (d != drv.end()) nv = d->second;
                }
                if (nv != v) {
                    v = nv;
                    changed = true;
                    note("supply " + r + " = " + stateName(nv.st) + (nv.hasV ? " " + fmtV(nv.v) + " V" : ""));
                }
            }
            if (!changed) return;
        }
        error("the supply network does not settle");
    }
    static std::string fmtV(double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%g", v);
        return b;
    }

    void evalSets() {
        for (auto& [n, s] : m.sets) {
            SetRt& r = sets[n];
            auto cx = ctx(scopeOfName(n), &s);
            bool allOn = true;
            for (auto& [fn, net] : s.fn) if (supplyOf(net).sv.st != S_FULL_ON) allOn = false;
            std::vector<const PowerState*> act;
            const PowerState* onDef = nullptr;
            const PowerState* offDef = nullptr;
            for (auto& ps : s.states) {
                if (ps.name == "ON") onDef = &ps;
                if (ps.name == "OFF") offDef = &ps;
                bool deferred = !ps.supplyExpr && !ps.logicExpr;
                bool a;
                if (deferred) a = r.forced == ps.name && ps.name != "ON" && ps.name != "OFF";
                else {
                    a = true;
                    if (ps.supplyExpr) a = a && truthOf(evalEx(ps.supplyExpr, cx, ps.where)) == 1;
                    if (ps.logicExpr) a = a && truthOf(evalEx(ps.logicExpr, cx, ps.where)) == 1;
                }
                if (a) act.push_back(&ps);
            }
            // ON and OFF are the coarsest states: a user state that is also
            // active refines them.
            if (act.size() > 1)
                std::erase_if(act, [](const PowerState* p) { return p->name == "ON" || p->name == "OFF"; });
            std::string st;
            int sim = SS_NONE;
            if (act.size() > 1) {
                bool same = true;
                for (auto* p : act) same &= p->simstate == act[0]->simstate;
                if (!same) { st = "ERROR"; sim = SS_CORRUPT; }
                else { st = act[0]->name; sim = act[0]->simstate; }
            } else if (act.size() == 1) {
                st = act[0]->name;
                sim = act[0]->simstate;
                if (act[0]->illegal) error("supply set " + n + " is in illegal power state " + st);
            } else if (r.forced == "ON" || (!r.forced.empty() && r.forced != "OFF" && false)) {
                st = "ON";
            } else if (r.forced == "OFF") {
                st = "OFF";
            } else if ((!onDef || (!onDef->supplyExpr && !onDef->logicExpr)) && allOn) {
                st = "ON";
            } else {
                st = "OFF";
                if (s.complete && !offDef) error("supply set " + n + " matches none of its power states (-complete)");
            }
            if (sim == SS_NONE) {
                if (st == "ON") sim = onDef && onDef->simstate != SS_NONE ? onDef->simstate : SS_NORMAL;
                else if (st == "OFF") sim = offDef && offDef->simstate != SS_NONE ? offDef->simstate : SS_CORRUPT;
                else sim = r.known ? r.sim : SS_CORRUPT;
            }
            if (st != r.state || !r.known) transition(n, "supply set", st, s.complete);
            if (st != r.state || sim != r.sim || !r.known) {
                if (r.known || st != "OFF") note("supply set " + n + " power state " + st + " (" + simstateName(sim) + ")");
                bool simChanged = sim != r.sim || !r.known;
                r.state = st;
                r.sim = sim;
                r.known = true;
                if (simChanged && r.sig >= 0) S.set(r.sig, make(S.get(r.sig).v == 1 ? 0 : 1, 0, 1, 0));
            }
        }
    }

    void evalDomainStates() {
        for (auto& d : doms) {
            if (d.region) continue;
            const Domain& dd = m.domains.at(d.name);
            if (dd.states.empty()) continue;
            auto cx = ctx(dd.scope, nullptr);
            std::vector<const PowerState*> act;
            for (auto& ps : dd.states) {
                bool a = ps.logicExpr ? truthOf(evalEx(ps.logicExpr, cx, ps.where)) == 1 : d.forced == ps.name;
                if (a && ps.supplyExpr) a = truthOf(evalEx(ps.supplyExpr, cx, ps.where)) == 1;
                if (a) act.push_back(&ps);
            }
            std::string st = act.size() == 1 ? act[0]->name : act.empty() ? "UNDEFINED" : "ERROR";
            if (st == d.state) continue;
            d.state = st;
            note("power domain " + d.name + " power state " + st);
            transition(d.name, "power domain", st, dd.complete);
            pendingChecks.push_back([this, &d, st, act, &dd]() {
                if (d.state != st) return;
                if (st == "ERROR") error("power domain " + d.name + " is in more than one power state");
                else if (act.size() == 1 && act[0]->illegal) error("power domain " + d.name + " is in illegal power state " + st);
                else if (st == "UNDEFINED" && dd.complete)
                    error("power domain " + d.name + " matches none of its power states (-complete)");
            });
        }
    }
    std::vector<std::function<void()>> pendingChecks;

    // Power state groups (6.22): the legal combinations of the power states
    // of other objects.
    void evalGroups() {
        for (auto& g : groups) {
            const StateGroup& gg = m.groups.at(g.name);
            auto cx = ctx(gg.scope, nullptr);
            std::vector<const PowerState*> act;
            for (auto& ps : gg.states) {
                bool a = ps.logicExpr ? truthOf(evalEx(ps.logicExpr, cx, ps.where)) == 1 : g.forced == ps.name;
                if (a && ps.supplyExpr) a = truthOf(evalEx(ps.supplyExpr, cx, ps.where)) == 1;
                if (a) act.push_back(&ps);
            }
            std::string st = act.size() == 1 ? act[0]->name : act.empty() ? "UNDEFINED" : "ERROR";
            if (st == g.state) continue;
            g.state = st;
            note("power state group " + g.name + " power state " + st);
            transition(g.name, "power state group", st, gg.complete);
            pendingChecks.push_back([this, &g, st, act, &gg]() {
                if (g.state != st) return;
                if (st == "ERROR") error("power state group " + g.name + " is in more than one power state");
                else if (act.size() == 1 && act[0]->illegal) error("power state group " + g.name + " is in illegal power state " + st);
                else if (st == "UNDEFINED" && gg.complete)
                    error("power state group " + g.name + " matches none of its power states (-complete)");
            });
        }
    }

    // Power state transition detection (9.4): the object went to state `to`.
    // The transition is from the last state that was not UNDEFINED, which
    // is an allowed intermediate state while the object's power states are
    // not complete. An -illegal transition is an error (6.7), and so is a
    // transition that is not named when the transitions are -complete.
    void transition(const std::string& obj, const char* what, const std::string& to, bool statesComplete) {
        if (to == "ERROR" || to.empty()) return;
        if (to == "UNDEFINED") {
            if (statesComplete) lastState[obj] = to;
            return;
        }
        auto prev = lastState.find(obj);
        std::string from = prev == lastState.end() ? "" : prev->second;
        lastState[obj] = to;
        if (from != to) stateHits[obj][to]++;
        if (from.empty() || from == to || from == "UNDEFINED") return;
        auto it = m.transitions.find(obj);
        if (it == m.transitions.end()) return;
        const Transition* legal = nullptr;
        const Transition* illegal = nullptr;
        for (auto& t : it->second.tr)
            if (t.matches(from, to)) (t.illegal ? illegal : legal) = &t;
        if (legal && !illegal) transHits[obj][legal->name]++;
        if (illegal)
            error(std::string(what) + " " + obj + ": illegal state transition " + illegal->name + " (" + from + " -> " + to + ")");
        else if (legal)
            note(std::string(what) + " " + obj + ": state transition " + legal->name + " (" + from + " -> " + to + ")");
        else if (it->second.complete)
            error(std::string(what) + " " + obj + ": state transition " + from + " -> " + to + " is not defined (-complete)");
    }

    // Apply each domain's simstate (9.6).
    void applyDomains() {
        for (size_t i = 0; i < doms.size(); i++) {
            DomRt& d = doms[i];
            int nm = d.alwaysOn ? SS_NORMAL : sets[d.set].sim;
            if (d.applied && nm == d.mode) continue;
            int old = d.applied ? d.mode : -1;
            d.mode = nm;
            g_pdMode[i] = nm;
            d.applied = true;
            if (old != -1 || nm != SS_NORMAL) note((d.region ? "elements on " : "power domain ") + d.name + " simstate " + simstateName(nm));
            if (nm == SS_NORMAL && old != -1 && old != SS_NORMAL && S.now > 0)
                for (size_t k = 0; k < replays.size(); k++)
                    if (replays[k].second == (int)i) {
                        note("sim_replay_control: replaying an initial block on the power-up of " + d.name);
                        if (procFns) S.spawn(procFns[(size_t)replayProc[k]]());   // simv: the compiled process
                        else S.spawn(process(replays[k].first));
                    }
            if (nm == SS_CORRUPT && old != SS_CORRUPT) {
                // Retention: the retained value is kept by the retention
                // supply; the register itself loses power (9.7.4).
                std::set<int> keep;
                for (auto& r : rets) {
                    if (r.dom != (int)i) continue;
                    for (auto& e : r.els) {
                        if (r.r->useAsPrimary && e.have) { keep.insert(e.sig); hold(e.sig, e.saved, 1); }
                    }
                }
                for (int s : d.owned) if (!keep.count(s)) corrupt(s);
            } else if (nm != SS_CORRUPT && old == SS_CORRUPT) {
                for (int s : d.owned) release(s);
                // a level-sensitive retention control still active restores at power-up
                for (auto& r : rets)
                    if (r.dom == (int)i && (r.inRet || r.restoring)) {
                        for (auto& e : r.els) if (e.have) hold(e.sig, e.saved, 1);
                        note("retention " + r.r->name + ": restored at power-up");
                    }
                int ps = g_pdSig[i];
                S.set(ps, make(S.get(ps).v == 1 ? 0 : 1, 0, 1, 0));
            } else if (old == -1 && nm != SS_CORRUPT) {
                int ps = g_pdSig[i];
                S.set(ps, make(S.get(ps).v == 1 ? 0 : 1, 0, 1, 0));
            }
        }
    }

    // Retention controls (9.7).
    bool condOk(const ExP& e, const std::string& scope) {
        if (!e) return true;
        return truthOf(evalEx(e, ctx(scope, nullptr), "retention condition")) == 1;
    }
    static int bitv(const Val& v) { return (v.x | v.z) & 1 ? 2 : (int)(v.v & 1); }
    void evalRetention() {
        for (auto& r : rets) {
            const Retention& R = *r.r;
            std::string sc = scopeOfName(R.domain);
            int sv = bitv(S.get(r.saveSig)), rv = bitv(S.get(r.restoreSig));
            int ps = r.lastSave, pr = r.lastRestore;
            r.lastSave = sv;
            r.lastRestore = rv;
            bool powered = doms[(size_t)r.dom].mode != SS_CORRUPT;
            bool retOk = R.supply.empty() || sets[m.setOf(R.supply)].sim == SS_NORMAL;
            auto active = [](int v, int sense) { return v == (sense == 1 ? 0 : 1); };
            auto edge = [](int from, int to, int sense) {
                return sense == 2 ? (from == 0 && to == 1) : sense == 3 ? (from == 1 && to == 0) : false;
            };
            auto doSave = [&]() {
                bool ok = powered && condOk(R.saveCond, sc);
                for (auto& e : r.els) {
                    e.saved = ok ? S.get(e.sig) : allx(S.sigs[(size_t)e.sig].w);
                    e.have = true;
                }
                if (!powered) error("retention " + R.name + ": save while " + doms[(size_t)r.dom].name + " is off");
                else retHits[R.name].first++;
                note("retention " + R.name + ": saved" + (ok ? "" : " (corrupt)"));
            };
            auto doRestore = [&](bool keepHeld) {
                if (!powered) {
                    for (auto& e : r.els) e.saved = allx(S.sigs[(size_t)e.sig].w);
                    error("retention " + R.name + ": restore while " + doms[(size_t)r.dom].name + " is off");
                    return;
                }
                bool ok = condOk(R.restoreCond, sc) && condOk(R.restorePeriodCond, sc);
                retHits[R.name].second++;
                for (auto& e : r.els) {
                    Val v = e.have && ok ? e.saved : allx(S.sigs[(size_t)e.sig].w);
                    if (keepHeld) hold(e.sig, v, 1);
                    else { release(e.sig); S.set(e.sig, v); }
                }
                note("retention " + R.name + ": restored" + (ok ? "" : " (corrupt)"));
            };
            if (r.single) {
                // One control: active save level = retention mode. Entering
                // it saves; while in it the register holds the retained
                // value (and shows it again at power-up); leaving it hands
                // the register back to its logic.
                bool in = active(sv, R.save.sense), was = ps != 2 && active(ps, R.save.sense);
                if (sv == 2 && ps != 2) {
                    for (auto& e : r.els) { e.saved = allx(S.sigs[(size_t)e.sig].w); e.have = true; }
                    if (powered) for (auto& e : r.els) corrupt(e.sig);
                } else if (in && !was) {
                    doSave();
                    r.inRet = true;
                    if (powered) for (auto& e : r.els) hold(e.sig, e.saved, 1);
                } else if (!in && was) {
                    r.inRet = false;
                    if (powered) {
                        retHits[R.name].second++;
                        bool ok = condOk(R.restoreCond, sc) && condOk(R.restorePeriodCond, sc);
                        for (auto& e : r.els) {
                            release(e.sig);
                            if (!ok) S.set(e.sig, allx(S.sigs[(size_t)e.sig].w));
                        }
                    } else for (auto& e : r.els) e.saved = allx(S.sigs[(size_t)e.sig].w);
                }
            } else {
                // Save: an edge, or the trailing edge of a level (6.53).
                if (ps != 2 && (R.save.sense >= 2 ? edge(ps, sv, R.save.sense)
                                                   : active(ps, R.save.sense) && !active(sv, R.save.sense)))
                    doSave();
                if (R.restore.sense >= 2) {
                    if (pr != 2 && edge(pr, rv, R.restore.sense)) doRestore(false);
                } else {
                    bool in = active(rv, R.restore.sense), was = pr != 2 && active(pr, R.restore.sense);
                    if (in && !was) { r.restoring = true; doRestore(true); }
                    else if (!in && was) {
                        r.restoring = false;
                        if (powered) for (auto& e : r.els) release(e.sig);
                    }
                }
                // SAV_RES_COR: save and restore active together corrupts the register
                if (R.savResCor && R.save.sense < 2 && R.restore.sense < 2 && active(sv, R.save.sense) &&
                    active(rv, R.restore.sense) && powered)
                    for (auto& e : r.els) { release(e.sig); S.set(e.sig, allx(S.sigs[(size_t)e.sig].w)); }
            }
            // The retained value is lost without its supply (RET_SUP_COR)
            // or when the power-down condition fails while off.
            if (!retOk && R.retSupCor)
                for (auto& e : r.els) {
                    e.saved = allx(S.sigs[(size_t)e.sig].w);
                    if (powered) { release(e.sig); S.set(e.sig, e.saved); }
                }
            if (!powered && R.powerdownCond && !condOk(R.powerdownCond, sc))
                for (auto& e : r.els) e.saved = allx(S.sigs[(size_t)e.sig].w);
        }
    }

    // Legacy PSTs (6.24, 6.6): the supplies' port states must match a row.
    std::string portStateOf(const std::string& supply) {
        SupplyVal v = supplyOf(supply).sv;
        auto it = m.portStates.find(supply);
        if (it == m.portStates.end()) return v.st == S_OFF ? "OFF" : stateName(v.st);
        for (auto& d : it->second) {
            if (d.off && v.st == S_OFF) return d.name;
            if (!d.off && v.st != S_OFF && (!d.partial || v.st == S_PARTIAL_ON) && v.hasV && v.v >= d.vmin - 1e-6 && v.v <= d.vmax + 1e-6) return d.name;
        }
        if (v.st == S_OFF) return "OFF";
        return std::string(stateName(v.st)) + (v.hasV ? " " + fmtV(v.v) + "V" : "");
    }
    void checkPst() {
        for (auto& t : m.psts) {
            std::vector<std::string> cur;
            for (auto& s : t.supplies) cur.push_back(portStateOf(s));
            bool ok = false;
            for (auto& row : t.rows) {
                bool match = true;
                for (size_t k = 0; k < cur.size(); k++) {
                    const std::string& want = row.st[k];
                    if (want == "*" || want == cur[k]) continue;
                    if ((want == "off" || want == "OFF") && (cur[k] == "off" || cur[k] == "OFF")) continue;
                    match = false;
                }
                ok |= match;
            }
            std::string combo;
            for (size_t k = 0; k < cur.size(); k++) combo += (k ? " " : "") + t.supplies[k] + "=" + cur[k];
            if (ok) { reported.erase(t.name); continue; }
            std::string key = t.name;
            if (pstLast[key] == combo) continue;
            pstLast[key] = combo;
            error("supplies are in no state of PST " + t.name + ": " + combo);
        }
    }
    std::map<std::string, std::string> pstLast;

    bool checkArmed = false;
    void evaluate() {
        copyVcmResults();
        copyRetModels();
        readHdlPorts();
        evalSupplies();
        driveHdlPorts();
        evalSets();
        evalDomainStates();
        evalGroups();
        applyDomains();
        evalRetention();
        if (!checkArmed && chkSig >= 0) {   // legality is checked once the time step settles
            checkArmed = true;
            S.set(chkSig, make(S.get(chkSig).v == 1 ? 0 : 1, 0, 1, 0));
        }
    }
    static Task watcher() {
        Rt& r = rt();
        r.evaluate();
        if (r.watch.empty()) co_return;
        for (;;) {
            if (r.watch.size() > 1) co_await WaitAny{&r.watch};
            else co_await Wait{r.watch[0].first, r.watch[0].second};
            r.evaluate();
        }
    }
    static Task checker() {
        Rt& r = rt();
        for (bool first = true;; first = false) {
            if (!first) co_await Wait{r.chkSig, ANY};
            co_await Delay{0};
            r.checkArmed = false;
            r.checkPst();
            r.applyAsserts();   // once the time step settles: supplies the testbench turns on at time 0 are on
            auto pc = std::move(r.pendingChecks);
            r.pendingChecks.clear();
            for (auto& f : pc) f();
        }
    }
    void start() {
        S.spawn(checker());
        S.spawn(watcher());
    }

    // --------------------------------------------------------------- hooks
    static Rt& rt() {
        static Rt r;
        return r;
    }
    Val ret(bool ok) { return make(ok ? 1 : 0, 0, 1, 0); }
    Val hook(int op, const std::vector<Val>& a) {
        int base = op & 0xff, idx = op >> 8;
        if (base == UPF_ISO || base == UPF_LS) return cell((size_t)idx, a);
        std::string name = a.empty() ? "" : v_str(a[0]);
        if (base == UPF_SET_POWER_STATE) return ret(setPowerState(name, a.size() > 1 ? v_str(a[1]) : ""));
        std::string sup = supplyByName(name);
        if (sup.empty()) {
            error("no supply port, net or supply set function '" + name + "'");
            return base == UPF_GET_SUPPLY_VALUE ? make(0, 0, 64, 0) : ret(false);
        }
        std::string r = root(sup);
        if (base == UPF_GET_SUPPLY_VALUE) {
            SupplyVal v = val[r];
            int64_t uv = (int64_t)std::llround(v.v * 1e6);
            return make(((uint64_t)v.st << 32) | (uint32_t)uv, 0, 64, 0);
        }
        if (swOf.count(r)) {
            error("'" + name + "' is driven by a power switch; it cannot be set from the testbench (11.2.3.3.4)");
            return ret(false);
        }
        SupplyVal v = drv[r];
        switch (base) {
            case UPF_SUPPLY_ON: v.st = S_FULL_ON; v.v = a.size() > 1 ? v_real(a[1]) : 1.0; v.hasV = true; break;
            case UPF_SUPPLY_PARTIAL_ON: v.st = S_PARTIAL_ON; v.v = a.size() > 1 ? v_real(a[1]) : 1.0; v.hasV = true; break;
            case UPF_SUPPLY_OFF: v.st = S_OFF; break;
            case UPF_SET_SUPPLY_STATE: {
                int s = a.size() > 1 ? (int)(a[1].v & 3) : 0;
                v.st = s;
                break;
            }
        }
        drv[r] = v;
        evaluate();
        return ret(true);
    }
    // set_power_state (9.3.1): a supply set's state sets its functions; a
    // domain's state recurses into the objects its logic expression names.
    bool setPowerState(std::string obj, const std::string& st, int depth = 0) {
        if (depth > 16) return false;
        while (!obj.empty() && obj[0] == '/') obj.erase(0, 1);
        for (;;) {
            if (m.domains.count(obj) || m.sets.count(m.setOf(obj))) break;
            size_t s = obj.find_first_of("/.");
            if (s == std::string::npos) { error("set_power_state: no object '" + obj + "'"); return false; }
            obj = obj.substr(s + 1);
        }
        if (m.domains.count(obj)) {
            const Domain& d = m.domains.at(obj);
            const PowerState* ps = nullptr;
            for (auto& p : d.states) if (p.name == st) ps = &p;
            if (!ps) { error("set_power_state: power domain " + obj + " has no power state " + st); return false; }
            DomRt& dr = doms[(size_t)domIdx.at(obj)];
            dr.forced = st;
            std::function<void(const ExP&)> walk = [&](const ExP& e) {
                if (!e) return;
                if (e->k == Ex::Eq && e->kids[0]->k == Ex::Id && e->kids[1]->k == Ex::Id) {
                    std::string o = Model::join(d.scope, e->kids[0]->s);
                    if (m.domains.count(o) || m.sets.count(m.setOf(o))) setPowerState(o, e->kids[1]->s, depth + 1);
                }
                if (e->k == Ex::And) for (auto& k : e->kids) walk(k);
            };
            walk(ps->logicExpr);
            evaluate();
            return true;
        }
        std::string sn = m.setOf(obj);
        const SupplySet& s = m.sets.at(sn);
        const PowerState* ps = nullptr;
        for (auto& p : s.states) if (p.name == st) ps = &p;
        if (!ps && st != "ON" && st != "OFF") { error("set_power_state: supply set " + sn + " has no power state " + st); return false; }
        sets[sn].forced = st;
        auto setFn = [&](const std::string& net, SupplyVal v) {
            std::string r = root(net);
            if (swOf.count(r)) return;
            drv[r] = v;
        };
        if (ps && ps->supplyExpr) {
            std::function<void(const ExP&)> walk = [&](const ExP& e) {
                if (!e) return;
                if ((e->k == Ex::Eq || e->k == Ex::Ne) && e->kids[0]->k == Ex::Id) {
                    auto f = s.fn.find(e->kids[0]->s);
                    std::string net = f != s.fn.end() ? f->second : "";
                    if (!net.empty()) {
                        SupplyVal v = drv[root(net)];
                        if (e->k == Ex::Ne) v.st = S_UNDETERMINED;
                        else if (e->kids[1]->k == Ex::Id) v.st = std::max(0, stateOf(e->kids[1]->s));
                        else if (e->kids[1]->k == Ex::Tuple && !e->kids[1]->kids.empty()) {
                            v.st = std::max(0, stateOf(e->kids[1]->kids[0]->s));
                            if (e->kids[1]->kids.size() > 1) { v.v = e->kids[1]->kids[1]->real; v.hasV = true; }
                        }
                        setFn(net, v);
                    }
                }
                if (e->k == Ex::And) for (auto& k : e->kids) walk(k);
            };
            walk(ps->supplyExpr);
        } else {
            bool off = st == "OFF" || (ps && ps->simstate == SS_CORRUPT);
            for (auto& [fn, net] : s.fn) {
                SupplyVal v = drv[root(net)];
                v.st = off ? S_OFF : S_FULL_ON;
                setFn(net, v);
            }
        }
        evaluate();
        return true;
    }
    // Isolation (9.8) and level-shifter (9.9) cells.
    bool supplyNormal(const std::string& sn) {
        auto it = sets.find(sn);
        return it == sets.end() || it->second.sim == SS_NORMAL;
    }
    Val cell(size_t idx, const std::vector<Val>& a) {
        Cell& c = cells[idx];
        const Val& d = a[0];
        bool ok = true;
        for (auto& sn : c.supplies) ok &= supplyNormal(sn);
        if (c.kind != 0) return ok ? d : allx(d.w);   // level shifter (9.9), repeater (9.10)
        if (c.model >= 0) {   // use_interface_cell -force_function (6.62): the model's output
            IfcEl& e = ifcEls[(size_t)c.model];
            if (!e.held || !(d == e.last)) {
                e.held = true;
                e.last = d;
                S.holdOn(e.dSig, konst(d), true);
            }
            if (!ok) return allx(d.w);
            Val y = S.get(e.ySig);
            return y.w == d.w ? y : allx(d.w);
        }
        bool latch = c.clamp == "latch";
        if (!ok) {
            if (latch) { c.latch = allx(d.w); c.latched = true; }
            return allx(d.w);
        }
        int act = 0;   // 0 off, 1 isolating, 2 unknown
        for (size_t k = 1; k < a.size(); k++) {
            int b = bitv(a[k]);
            if (b == 2) { act = std::max(act, 2); continue; }
            if (b == (c.senseLow[k - 1] ? 0 : 1)) act = act == 2 ? 2 : 1;
        }
        if (act == 2) {
            if (latch) { c.latch = allx(d.w); c.latched = true; }
            return allx(d.w);
        }
        if (act == 1 && !c.isolating) c.enables++;
        c.isolating = act == 1;
        if (act == 0) {
            if (latch) { c.latch = d; c.latched = true; }
            return d;
        }
        const std::string& cv = c.clamp;
        if (cv == "0") return make(0, 0, d.w, 0);
        if (cv == "1") return d.w <= 64 ? make(d.w >= 64 ? ~0ull : ((1ull << d.w) - 1), 0, d.w, 0) : v_not(make(0, 0, d.w, 0));
        if (cv == "Z" || cv == "z") return allz(d.w);
        return c.latched ? c.latch : allx(d.w);
    }

    // ------------------------------------------- msim -compile (simv)
    // simv has no slang design: msim binds the power intent to the design,
    // then writes what simv needs to run it into a snapshot. simv reads
    // the UPF file again with the design's answers that msim recorded (the
    // DesignView for find_objects, set_design_top, apply_power_model,
    // the HDL attributes), then takes the bound state from the snapshot.
    struct LoadRec {
        std::string path;
        bool topFixed = false;
        std::vector<HdlAttr> attrs;
        std::map<std::string, std::vector<std::string>> applies;   // apply_power_model: module -> instances
        DesignView dv;
    } lrec;
    // Why the bound state cannot run in simv yet, or "".
    std::string simvGap() const {
        if (!override.empty() || !pendingChecks.empty()) return "a power state held at bind time";
        return "";
    }
    // VCMs and VCTs by name: the model simv reads holds its own.
    std::string vcmName(const Vcm* v) const {
        if (!v) return "";
        for (auto& [n, vv] : m.vcms) if (&vv == v) return n;
        throw Error("msim -compile: a VCM outside the power intent");
    }
    const Vcm* vcmNamed(const std::string& n) const { return n.empty() ? nullptr : &m.vcms.at(n); }
    std::string vctName(const Vct* v) const {
        if (!v) return "";
        for (auto& [n, vv] : m.vcms) if (&vv.bits == v) return "#" + n;   // a bit VCM's table
        if (m.vct(v->name) != v) throw Error("msim -compile: VCT " + v->name + " is not found by its name");
        return v->name;
    }
    const Vct* vctNamed(const std::string& n) const {
        if (n.empty()) return nullptr;
        if (n[0] == '#') return &m.vcms.at(n.substr(1)).bits;
        return m.vct(n);
    }
    template <class A> void snapshot(A& a) {
        auto& M = m;
        a(M.top, M.connects, M.connectWhere, dm.el, dm.excl, g.up);
        {   // connect_supply_net -vcm: by name
            std::map<std::pair<std::string, std::string>, std::string> cv;
            if (a.out)
                for (auto& [k, v] : M.connectVcm)
                    for (auto& [n, vv] : M.vcms) if (&vv == v) cv[k] = n;
            a(cv);
            if (!a.out) {
                M.connectVcm.clear();
                for (auto& [k, n] : cv) M.connectVcm[k] = &M.vcms.at(n);
            }
        }
        a(on, verbose, sigByName, drv, val, swOf, swState, domIdx, groupIdx, lastState, watch, reported, chkSig, errors);
        size_t n = sets.size();
        a(n);
        if (!a.out) sets.clear();
        auto si = sets.begin();
        for (size_t k = 0; k < n; k++) {
            std::string key = a.out ? si->first : std::string();
            a(key);
            SetRt& r = a.out ? (si++)->second : sets[key];
            a(r.state, r.sim, r.sig, r.known, r.forced);
        }
        a.seq(doms, [&](DomRt& d) { a(d.name, d.set, d.mode, d.applied, d.alwaysOn, d.region, d.owned, d.state, d.forced); });
        a.seq(cells, [&](Cell& c) {
            int iso = c.iso ? (int)(c.iso - M.isos.data()) : -1;
            a(c.kind, c.clamp, iso, c.supplies, c.senseLow, c.latch, c.latched, c.port, c.model, c.enables, c.isolating);
            c.iso = iso >= 0 ? &M.isos[(size_t)iso] : nullptr;
        });
        a.seq(groups, [&](GroupRt& gr) { a(gr.name, gr.state, gr.forced); });
        a.seq(rets, [&](RetRt& r) {
            int ri = r.r ? (int)(r.r - M.rets.data()) : -1;
            a(ri, r.dom, r.saveSig, r.restoreSig, r.lastSave, r.lastRestore, r.single, r.inRet, r.restoring);
            r.r = ri >= 0 ? &M.rets[(size_t)ri] : nullptr;
            a.seq(r.els, [&](RetEl& e) { a(e.sig, e.saved, e.have); });
        });
        a(regionIdx, covPath, stateHits, transHits, retHits, stateSigs, latchSigs, noCorr, realSigs, intSigs);
        a.seq(retModelEls, [&](RetModelEl& e) { a(e.el, e.q, e.inst, e.strategy, e.elSig, e.qSig, e.copied, e.last); });
        a(retModelInsts, ifcElOf);
        a.seq(ifcEls, [&](IfcEl& e) { a(e.port, e.d, e.y, e.dSig, e.ySig, e.held, e.last); });
        a.seq(hdlPorts, [&](HdlPort& h) {
            std::string vct = a.out ? vctName(h.vct) : "", vcm = a.out ? vcmName(h.vcm) : "";
            a(vct, vcm, h.sig, h.net, h.port, h.supply, h.hdl2upf, h.cat, h.sgn, h.w, h.labelOf, h.valueOf, h.last, h.lastVal, h.held,
              h.lastV, h.hasV, h.warned, h.fOff, h.copyFrom, h.target, h.lastCopy, h.copied);
            if (!a.out) { h.vct = vctNamed(vct); h.vcm = vcmNamed(vcm); }
        });
        {
            size_t nb = vcmBinds.size();
            a(nb);
            if (!a.out) vcmBinds.clear();
            auto it = vcmBinds.begin();
            for (size_t k = 0; k < nb; k++) {
                std::pair<std::string, std::string> key;
                VcmBind b;
                std::string vn;
                if (a.out) { key = it->first; b = it->second; vn = vcmName(b.vcm); ++it; }
                a(key, vn, b.hdl2upf, b.inst);
                if (!a.out) { b.vcm = vcmNamed(vn); vcmBinds[key] = b; }
            }
        }
        a(fieldHeld, swCat, swPrevCat, ackVal, ackGen, pstLast, checkArmed, g_pdMode, g_pdSig, S.vcdTop, instCells());
        // sim_replay_control: by process number; simv runs its compiled process
        if (a.out) {
            replayProc.clear();
            for (auto& [p, d] : replays) {
                auto it = std::find(progs.begin(), progs.end(), p);
                if (it == progs.end()) throw Error("msim -compile: a replayed initial block is not a process of the design");
                replayProc.push_back((int)(it - progs.begin()));
            }
        }
        std::vector<int> rd;
        for (auto& r : replays) rd.push_back(r.second);
        a(replayProc, rd);
        if (!a.out) {
            replays.clear();
            for (int d : rd) replays.push_back({nullptr, d});
        }
        a.seq(asrtCtls, [&](AsrtCtlRt& x) {   // sim_assertion_control
            int ri = x.r ? (int)(x.r - M.assertCtls.data()) : -1;
            a(ri, x.id, x.dom, x.active, x.dead);
            x.r = ri >= 0 ? &M.assertCtls[(size_t)ri] : nullptr;
        });
    }
    std::vector<const Prog*> progs;   // msim: the design's processes in simv's order (proc<n>)
    // msim: the load record and the bound state.
    bool writeSnapshot(const std::string& path, std::vector<const Prog*> procs) {
        progs = std::move(procs);
        std::ofstream f(path, std::ios::binary);
        snap::Out a{f};
        a(lrec.path, lrec.topFixed, m.loadStart, m.topSeen, lrec.applies);
        a.seq(lrec.attrs, [&](HdlAttr& h) { a(h.model, h.port, h.name, h.value, h.where); });
        a(lrec.dv.models, lrec.dv.instances, lrec.dv.signals, lrec.dv.model, lrec.dv.portsOf);
        a.seq(lrec.dv.ports, [&](PortSite& p) { a(p.inst, p.port, p.dir, p.width, p.where); });
        snapshot(a);
        return (bool)f;
    }
    // simv: the same model, then the bound state.
    bool readSnapshot(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        snap::In a{f};
        std::string start;
        std::map<std::string, std::string> seen;
        a(lrec.path, lrec.topFixed, start, seen, lrec.applies);
        a.seq(lrec.attrs, [&](HdlAttr& h) { a(h.model, h.port, h.name, h.value, h.where); });
        a(lrec.dv.models, lrec.dv.instances, lrec.dv.signals, lrec.dv.model, lrec.dv.portsOf);
        a.seq(lrec.dv.ports, [&](PortSite& p) { a(p.inst, p.port, p.dir, p.width, p.where); });
        if (!f) return false;
        std::ostringstream quiet;   // Tcl puts printed once already, by msim
        m.out = &quiet;
        m.dv = &lrec.dv;
        m.topFixed = lrec.topFixed;
        m.resolveTop = [seen](const std::string& t) {
            auto it = seen.find(t);
            return it == seen.end() ? t : it->second;
        };
        m.load(lrec.path, start);
        m.dv = nullptr;
        m.resolveTop = nullptr;
        m.out = &std::cout;
        m.addHdlAttrs(lrec.attrs);
        m.resolveApplies([this](const std::string& mod) { return lrec.applies[mod]; });
        snapshot(a);
        if (!f) return false;
        g_upf = on;
        g_upfHook = [](int op, const std::vector<Val>& a) { return rt().hook(op, a); };
        return true;
    }
};

inline Rt& rt() { return Rt::rt(); }

}  // namespace upfrt
