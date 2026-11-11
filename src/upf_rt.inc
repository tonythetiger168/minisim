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
#include "upf.hpp"

namespace upfrt {
using namespace upf;

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
    void bind(LowerResult& lw) {
        on = true;
        g_upf = true;
        dm.build(m);
        g.build(m);
        for (size_t i = 0; i < S.sigs.size(); i++) sigByName.emplace(S.sigs[i].name, (int)i);
        for (auto& w : m.warnings) std::cerr << "msim: UPF: " << w << "\n";
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
        for (auto& e : intentErrors(m, dm, [this](const std::string& p) { return modelOf(p); }))
            throw Error(e.where + ": " + e.text);
        bindLogic(lw);
        // processes: domain, state or not; what each domain drives
        std::vector<std::set<int>> owned(doms.size());
        stateSigs.assign(doms.size(), {});
        for (auto& lp : lw.procs) {
            if (!lp.port.empty() || lp.kind == "upf_wire") continue;   // a port connection is a wire, not logic of either side
            // slang names procedure kinds "Initial", "AlwaysFF", ...; lowering adds "assign", "init", "sampled"
            const std::string& k = lp.kind;
            if (k == "Initial" || k == "Final" || k == "init" || k == "sampled") continue;
            std::string dn = dm.of(lp.scope);
            // set_simstate_behavior (6.56): DISABLE and PORT_CORR_ONLY leave the logic alone
            auto [beh, behAt] = behaviorOf(lp.scope);
            if (beh == "DISABLE" || beh == "PORT_CORR_ONLY") continue;
            int pd;
            if (const SupplyConn* sc = explicitConn(lp.scope)) pd = regionOf(sc->set);   // connect_supply_set (6.15)
            else if (!dn.empty()) pd = domIdx.at(dn);
            else continue;
            if (owned.size() < doms.size()) { owned.resize(doms.size()); stateSigs.resize(doms.size()); }
            lp.prog.pd = pd;
            bool st = k == "AlwaysFF" || k == "AlwaysLatch";
            if (k == "Always")
                for (auto& in : lp.prog.code)
                    if (in.k == IK::WaitEdge) {
                        if (in.events.size() > 1) { for (auto& [s, e] : in.events) if (e != ANY) st = true; }
                        else if (in.edge != ANY) st = true;
                    }
            lp.prog.pstate = st;
            std::vector<const Prog*> seen;
            std::function<void(const Prog&)> writes = [&](const Prog& p) {
                for (const Prog* q : seen) if (q == &p) return;
                seen.push_back(&p);
                for (auto& in : p.code) {
                    if ((in.k == IK::Assign || in.k == IK::Nba) && in.sig >= 0 && !isRel(in.sig)) {
                        if (in.arr) for (int k = 0; k < in.arr->count; k++) owned[(size_t)pd].insert(in.arr->base + k);
                        else owned[(size_t)pd].insert(in.sig);
                    }
                    if (in.k == IK::Call && in.callee) writes(*in.callee);
                    for (const Prog* b : in.branches) writes(*b);
                }
            };
            writes(lp.prog);
            if (st) {
                std::set<int> w;
                for (auto& in : lp.prog.code)
                    if ((in.k == IK::Nba || in.k == IK::Assign) && in.sig >= 0 && !isRel(in.sig) && !in.arr) w.insert(in.sig);
                for (int s : w) stateSigs[(size_t)pd].insert(s);
                // a domain's retention strategies also reach the elements on another supply
                if (doms[(size_t)pd].region && !dn.empty())
                    for (int s : w) stateSigs[(size_t)domIdx.at(dn)].insert(s);
            }
        }
        for (size_t d = 0; d < doms.size(); d++) {
            for (int s : owned[d])
                if (S.sigs[(size_t)s].name.find('$') == std::string::npos) doms[d].owned.push_back(s);
        }
        bindCells(lw);
        bindRetention();
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
        for (int s : w) watch.push_back({s, ANY});
        chkSig = S.sig("$upf.check", 1);
        g_upfHook = [](int op, const std::vector<Val>& a) { return rt().hook(op, a); };
    }
    std::vector<std::set<int>> stateSigs;

    // Logic ports and nets made by UPF and their connections (6.13, 6.19,
    // 6.20): each connection is one more continuous assignment, from the
    // source side to the sink side. A connection to a port of a descendant
    // instance is a port connection like those of the HDL, so the interface
    // cell strategies see it (4.5.5).
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

    // Isolation and level-shifter cells on the port connections that cross
    // a domain boundary.
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
            for (auto& is : m.isos) {
                if (!cross || !isoApplies(m, is, c)) continue;
                if (is.noIso) break;
                Cell cell;
                cell.kind = 0;
                cell.iso = &is;
                cell.port = p.inst + "." + p.port;
                // the port's own clamp requirement, when the strategy gives none (6.51)
                cell.clamp = is.clamp;
                if (std::string pc = attr("UPF_clamp_value"); !is.clampSet && (pc == "0" || pc == "1" || pc == "Z" || pc == "latch")) cell.clamp = pc;
                if (!is.supply.empty()) cell.supplies.push_back(m.setOf(is.supply));
                for (size_t q = 0; q < is.signals.size(); q++) cell.senseLow.push_back(q < is.senseLow.size() ? is.senseLow[q] : 0);
                if (is.signals.empty()) throw Error(is.where + ": isolation " + is.name + " has no -isolation_signal");
                made.push_back((int)cells.size());
                cells.push_back(cell);
                note("isolation " + is.name + " on " + cell.port);
                break;   // one strategy per port (5.7 precedence: the first that applies)
            }
            for (auto& l : m.lss) {
                if (!cross || !lsApplies(m, l, c) || l.noShift) continue;
                Cell cell;
                cell.kind = 1;
                cell.port = p.inst + "." + p.port;
                std::string in = l.inSupply.empty() ? (c.from.empty() ? "" : m.primaryOf(c.from)) : m.setOf(l.inSupply);
                std::string out = l.outSupply.empty() ? (c.to.empty() ? "" : m.primaryOf(c.to)) : m.setOf(l.outSupply);
                if (!in.empty()) cell.supplies.push_back(in);
                if (!out.empty()) cell.supplies.push_back(out);
                made.push_back((int)cells.size());
                cells.push_back(cell);
                break;
            }
            for (auto& rp : m.reps) {   // 9.10: the repeater's output follows its supply's simstate
                if (!cross || !repApplies(m, rp, c)) continue;
                Cell cell;
                cell.kind = 2;
                cell.port = p.inst + "." + p.port;
                std::string sn = m.domains.count(rp.supply) ? m.primaryOf(rp.supply) : m.setOf(rp.supply);
                if (!sets.count(sn)) throw Error(rp.where + ": repeater " + rp.name + ": no supply set '" + rp.supply + "'");
                cell.supplies.push_back(sn);
                made.push_back((int)cells.size());
                cells.push_back(cell);
                note("repeater " + rp.name + " on " + cell.port);
                break;
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
            for (int s : stateSigs[(size_t)rr.dom]) {
                const std::string& n = S.sigs[(size_t)s].name;
                bool in = r.elements.empty();
                for (auto& e : r.elements) in |= DomainMap::within(n, m.design(e));
                for (auto& e : r.exclude) if (DomainMap::within(n, m.design(e))) in = false;
                // -applies_to flop / latch is not told apart: both are state elements
                if (in) rr.els.push_back({s, allx(S.sigs[(size_t)s].w), false});
            }
            if (rr.els.empty())
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
        if (from.empty() || from == to || from == "UNDEFINED") return;
        auto it = m.transitions.find(obj);
        if (it == m.transitions.end()) return;
        const Transition* legal = nullptr;
        const Transition* illegal = nullptr;
        for (auto& t : it->second.tr)
            if (t.matches(from, to)) (t.illegal ? illegal : legal) = &t;
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
                note("retention " + R.name + ": saved" + (ok ? "" : " (corrupt)"));
            };
            auto doRestore = [&](bool keepHeld) {
                if (!powered) {
                    for (auto& e : r.els) e.saved = allx(S.sigs[(size_t)e.sig].w);
                    error("retention " + R.name + ": restore while " + doms[(size_t)r.dom].name + " is off");
                    return;
                }
                bool ok = condOk(R.restoreCond, sc) && condOk(R.restorePeriodCond, sc);
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
        evalSupplies();
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
};

inline Rt& rt() { return Rt::rt(); }

}  // namespace upfrt
