// msdc.cpp - a constraint checker: reads a design and its SDC file and says
// what is wrong or missing in the constraints before timing or synthesis runs.
//
//   msdc design.sv -top top -sdc top.sdc
//
// The design is elaborated on the same front end as msim and mlint, and seen
// as registers and the combinational logic between them. The SDC file is read
// by src/sdc.hpp. Object names follow the synthesis tools: '/' between
// hierarchy levels, a register `q` in instance u_a is the cell u_a/q_reg with
// pins CK, D and Q, and bus bits are name[3].
//
// Rules (msdc -list): reading the file (SDC001..004), clocks (SDC010..018),
// I/O delays (SDC020..027), exceptions (SDC030..037), clock relationships
// against the paths the design really has (SDC040..042), case analysis
// (SDC050) and the electrical environment (SDC060, SDC061).
//
// The checks that walk the design (paths between clocks, paths behind an
// exception, port fan-out) run on every logical core; -threads N sets the
// count and the report is the same for any N.
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "lintir.hpp"
#include "parallel.hpp"
#include "sdc.hpp"
#include "version.hpp"

using namespace lintir;

namespace {

// ------------------------------------------------------------- the report

struct Out {
    std::string file;
    int line = 0;
    std::string rule, text;
    bool error = false;
    bool operator<(const Out& o) const {
        return std::tie(file, line, rule, text) < std::tie(o.file, o.line, o.rule, o.text);
    }
    bool operator==(const Out& o) const { return file == o.file && line == o.line && rule == o.rule && text == o.text; }
};

struct RuleDoc { const char* id; const char* text; };
const RuleDoc kRules[] = {
    {"SDC001", "a syntax error, a bad value or an unknown option in the SDC file"},
    {"SDC002", "a command that is not an SDC command"},
    {"SDC003", "an object query that matches nothing in the design"},
    {"SDC004", "a clock that is named but not defined"},
    {"SDC010", "a clock name defined twice (the later definition replaces the earlier)"},
    {"SDC011", "two clocks on one source without -add (the later replaces the earlier)"},
    {"SDC012", "a clock with a bad period or waveform"},
    {"SDC013", "a generated clock with no usable master or no divide/multiply/edges"},
    {"SDC014", "a clock that clocks nothing and is used by no constraint"},
    {"SDC015", "registers that no clock reaches"},
    {"SDC016", "registers reached by several clocks that are not declared exclusive"},
    {"SDC018", "a clock with no set_clock_uncertainty"},
    {"SDC020", "an input port with no set_input_delay"},
    {"SDC021", "an output port with no set_output_delay"},
    {"SDC022", "an I/O delay larger than the clock period"},
    {"SDC023", "an I/O delay whose -min is above its -max"},
    {"SDC024", "an I/O delay on the wrong kind of port"},
    {"SDC025", "an I/O delay with no -clock"},
    {"SDC027", "an I/O delay that replaces an earlier one (no -add_delay)"},
    {"SDC030", "an exception whose -from, -through or -to names nothing"},
    {"SDC031", "an exception -from that is not a timing startpoint"},
    {"SDC032", "an exception -to that is not a timing endpoint"},
    {"SDC033", "an exception between points with no path in the design"},
    {"SDC034", "a setup multicycle path with no matching hold multicycle"},
    {"SDC035", "an exception that repeats or is overridden by another"},
    {"SDC037", "an exception with a value that cannot be meant"},
    {"SDC040", "paths between two clocks with no declared relationship"},
    {"SDC041", "clocks declared asynchronous or exclusive that derive from one another"},
    {"SDC042", "a set_clock_groups that is malformed"},
    {"SDC050", "conflicting or bad set_case_analysis"},
    {"SDC060", "input ports with no driving cell, drive or input transition"},
    {"SDC061", "output ports with no load"},
};

struct Rep {
    std::set<std::string> disabled;
    std::vector<Out> out;
    void add(const char* rule, const std::string& file, int line, std::string text, bool error = false) {
        if (disabled.count(rule)) return;
        out.push_back({file, line, rule, std::move(text), error});
    }
    void add(const char* rule, const sdc::Loc& l, std::string text, bool error = false) {
        add(rule, l.file, l.line, std::move(text), error);
    }
};

using par::parallelFor;   // each call writes only its own slot: same result on any thread count

// A visited set that is cleared in O(1), one per thread.
struct Marks {
    std::vector<uint32_t> stamp;
    uint32_t epoch = 0;
    void begin(size_t n) {
        if (stamp.size() < n) stamp.assign(n, 0), epoch = 0;
        if (++epoch == 0) { std::fill(stamp.begin(), stamp.end(), 0); epoch = 1; }
    }
    bool test(int i) const { return stamp[i] == epoch; }
    bool set(int i) {
        if (stamp[i] == epoch) return false;
        stamp[i] = epoch;
        return true;
    }
};

// ----------------------------------------------------------- the RTL view

struct Reg {
    std::string name;  // u_a/q_reg
    int node = -1;
    std::string file;
    int line = 0;
    int width = 1;
    std::vector<int> blocks;  // the sequential blocks that assign it
};

struct Seq {  // one edge-triggered block
    int clk = -1;
    std::vector<int> reads, regs;
    std::string file;
    int line = 0;
};

struct Port {
    std::string name;
    int dir = 0;  // 0 in, 1 out, 2 inout
    int node = -1;
    int width = 1;
    std::string file;
    int line = 0;
};

struct InstPin { int node; int dir; };

struct Rtl : sdc::Design {
    // names for the reader
    std::vector<std::string> portNames, pinNames, cellNames, netNames;
    std::unordered_map<std::string, int> portIdx;         // base name -> ports
    std::unordered_map<std::string, int> regIdx;          // cell base name -> regs
    std::unordered_map<std::string, std::string> instDef; // instance cell -> module
    std::unordered_map<std::string, InstPin> instPins;    // inst/port (base)
    std::unordered_map<std::string, int> netNode;         // base name -> node
    std::vector<Port> ports;
    std::vector<Reg> regs;
    std::vector<Seq> seqs;
    // the graph: nodes are signals, an edge is combinational logic
    int n = 0;
    std::vector<std::string> nodeName;
    std::vector<int> fwdAt, fwd, revAt, rev;  // CSR
    std::vector<int> nodeReg;                 // node -> regs index, or -1
    std::vector<std::vector<int>> readers;    // node -> seq blocks that read it
    std::vector<int> nodePortOut;             // node -> output port index, or -1
    const sdc::Reader* reader = nullptr;
    std::function<std::vector<uint64_t>(const sdc::Constraints&, int&)> clockMasks;  // for all_registers -clock

    static std::string stripIdx(const std::string& s) {
        if (!s.empty() && s.back() == ']') {
            size_t b = s.rfind('[');
            if (b != std::string::npos && b > 0) return s.substr(0, b);
        }
        return s;
    }
    const Port* port(const std::string& name) const {
        auto it = portIdx.find(name);
        if (it == portIdx.end()) it = portIdx.find(stripIdx(name));
        return it == portIdx.end() ? nullptr : &ports[it->second];
    }
    int reg(const std::string& cell) const {
        auto it = regIdx.find(cell);
        if (it == regIdx.end()) it = regIdx.find(stripIdx(cell));
        return it == regIdx.end() ? -1 : it->second;
    }
    // A pin of a register: the register and which pin (C, D, Q), or -1.
    int regPin(const std::string& pin, char& which) const {
        size_t sl = pin.rfind('/');
        if (sl == std::string::npos) return -1;
        std::string p = stripIdx(pin.substr(sl + 1));
        which = p == "CK" || p == "CP" || p == "CLK" || p == "C" ? 'C' : p == "D" ? 'D' : p == "Q" || p == "QN" ? 'Q' : 0;
        if (!which) return -1;
        return reg(pin.substr(0, sl));
    }
    const InstPin* instPin(const std::string& pin) const {
        auto it = instPins.find(pin);
        if (it == instPins.end()) it = instPins.find(stripIdx(pin));
        return it == instPins.end() ? nullptr : &it->second;
    }
    bool isInst(const std::string& cell) const { return instDef.count(cell) != 0; }

    // ---- sdc::Design
    const std::vector<std::string>& names(sdc::Kind k) const override {
        switch (k) {
            case sdc::Kind::Port: return portNames;
            case sdc::Kind::Pin: return pinNames;
            case sdc::Kind::Cell: return cellNames;
            default: return netNames;
        }
    }
    int portDir(const std::string& name) const override {
        const Port* p = port(name);
        return p ? p->dir : -1;
    }
    sdc::ObjList registers(const sdc::RegQuery& q) const override {
        sdc::ObjList o;
        if (!q.edgeTriggered) return o;  // latches are not registers here
        std::vector<uint64_t> masks;
        int words = 0;
        std::vector<int> want;
        if (!q.clocks.empty() && reader && clockMasks) {
            masks = clockMasks(reader->sdc, words);
            std::vector<std::string> names;
            for (auto& c : reader->sdc.clocks)
                if (std::find(names.begin(), names.end(), c.name) == names.end()) names.push_back(c.name);
            for (auto& c : q.clocks)
                for (size_t i = 0; i < names.size(); i++)
                    if (names[i] == c) want.push_back((int)i);
        }
        for (auto& r : regs) {
            if (!q.clocks.empty()) {
                bool hit = false;
                for (int b : r.blocks)
                    for (int c : want)
                        if (seqs[b].clk >= 0 && words && (masks[size_t(seqs[b].clk) * words + c / 64] >> (c % 64) & 1)) hit = true;
                if (!hit) continue;
            }
            auto each = [&](const std::string& suffix, sdc::Kind k) {
                if (r.width == 1) o.push_back({k, r.name + suffix, true});
                else
                    for (auto& bit : bitNames.at(r.name)) o.push_back({k, bit + suffix, true});
            };
            if (q.cells) each("", sdc::Kind::Cell);
            if (q.clockPins) each("/CK", sdc::Kind::Pin);
            if (q.dataPins) each("/D", sdc::Kind::Pin);
            if (q.outputPins) each("/Q", sdc::Kind::Pin);
        }
        return o;
    }
    sdc::ObjList related(sdc::Kind want, const sdc::Obj& of) const override {
        sdc::ObjList o;
        if (want == sdc::Kind::Pin && of.kind == sdc::Kind::Cell) {
            if (reg(of.name) >= 0) {
                for (const char* p : {"/CK", "/D", "/Q"}) o.push_back({sdc::Kind::Pin, of.name + p, true});
            } else {
                auto it = pinsOfInst.find(of.name);
                if (it != pinsOfInst.end())
                    for (auto& p : it->second) o.push_back({sdc::Kind::Pin, p, true});
            }
        } else if (want == sdc::Kind::Cell && of.kind == sdc::Kind::Pin) {
            size_t sl = of.name.rfind('/');
            if (sl != std::string::npos) o.push_back({sdc::Kind::Cell, of.name.substr(0, sl), true});
        }
        return o;
    }
    std::optional<std::string> attribute(const sdc::Obj& o, const std::string& a) const override {
        if (o.kind == sdc::Kind::Cell) {
            bool r = reg(o.name) >= 0;
            if (a == "is_sequential") return r ? "true" : "false";
            if (a == "is_hierarchical") return r ? "false" : "true";
            if (a == "is_combinational") return "false";
            if (a == "ref_name") {
                auto it = instDef.find(o.name);
                if (it != instDef.end()) return it->second;
            }
        } else if (o.kind == sdc::Kind::Pin && (a == "direction" || a == "pin_direction")) {
            char w = 0;
            if (regPin(o.name, w) >= 0) return w == 'Q' ? "out" : "in";
            if (const InstPin* p = instPin(o.name)) return p->dir == 0 ? "in" : p->dir == 1 ? "out" : "inout";
        } else if (o.kind == sdc::Kind::Pin && a == "is_clock_pin") {
            char w = 0;
            if (regPin(o.name, w) >= 0) return w == 'C' ? "true" : "false";
        }
        return std::nullopt;
    }

    std::unordered_map<std::string, std::vector<std::string>> bitNames;    // bus base -> its bit names
    std::unordered_map<std::string, std::vector<std::string>> pinsOfInst;  // instance -> its pins
};

struct Builder {
    Rtl& r;
    Compilation& comp;
    const SourceManager& sm;
    lintir::Design d;
    Report dummy;
    std::unordered_map<const Symbol*, int> nodeOf;
    std::unordered_map<const Symbol*, std::string> symName;
    std::vector<std::pair<int, int>> edges;

    Builder(Rtl& rtl, Compilation& c) : r(rtl), comp(c), sm(*c.getSourceManager()) {}

    int node(const Symbol* s) {
        const Symbol* c = d.join.find(s);
        auto [it, fresh] = nodeOf.try_emplace(c, (int)r.nodeName.size());
        if (fresh) {
            auto nm = symName.find(s);
            r.nodeName.push_back(nm != symName.end() ? nm->second : path(*s));
        }
        return it->second;
    }
    void where(SourceLocation loc, std::string& file, int& line) {
        if (!loc) return;
        auto full = sm.getFullyOriginalLoc(loc);
        file = std::string(sm.getFileName(full));
        line = (int)sm.getLineNumber(full);
    }
    // name, and name[i] for each bit of a packed vector.
    std::vector<std::string> bits(const std::string& name, const Type& t) {
        std::vector<std::string> o;
        if (t.isIntegral() && t.getBitWidth() > 1 && t.getBitWidth() <= 4096 && t.hasFixedRange()) {
            auto rg = t.getFixedRange();
            for (int i = rg.upper(); i >= rg.lower(); i--) o.push_back(name + "[" + std::to_string(i) + "]");
            r.bitNames[name] = o;
        } else {
            o.push_back(name);
        }
        return o;
    }

    // Combinational edges that lintir does not keep: continuous assignments,
    // port connections that are expressions, gate primitives.
    struct Extra : ASTVisitor<Extra> {
        Builder& b;
        std::vector<std::pair<const Symbol*, const Symbol*>> raw;  // from -> to
        explicit Extra(Builder& bb) : b(bb) {}
        void link(const Expression& from, const Symbol* to) {
            std::vector<const Symbol*> n;
            namesIn(from, n);
            for (const Symbol* s : n) raw.push_back({s, to});
        }
        void handle(const GenerateBlockSymbol& g) {
            if (g.isUninstantiated) return;
            visitDefault(g);
        }
        void handle(const ContinuousAssignSymbol& ca) {
            const Expression& e = ca.getAssignment();
            if (e.kind != ExpressionKind::Assignment) return;
            const auto& a = e.as<AssignmentExpression>();
            std::vector<const Symbol*> l;
            namesIn(a.left(), l);
            if (l.empty()) return;
            link(a.right(), l[0]);
            for (size_t i = 1; i < l.size(); i++) raw.push_back({l[i], l[0]});  // an index is a read
        }
        void handle(const InstanceSymbol& inst) {
            for (const PortConnection* conn : inst.getPortConnections()) {
                if (!conn || conn->port.kind != SymbolKind::Port) continue;
                const auto& ps = conn->port.as<PortSymbol>();
                const Expression* x = conn->getExpression();
                if (!x || !ps.internalSymbol) continue;
                const Expression* outer = x;
                if (outer->kind == ExpressionKind::Assignment) outer = &outer->as<AssignmentExpression>().left();
                if (oneName(*outer)) continue;  // joined: the same wire
                std::vector<const Symbol*> n;
                namesIn(*outer, n);
                for (const Symbol* s : n) {
                    if (ps.direction == ArgumentDirection::In) raw.push_back({s, ps.internalSymbol});
                    else if (ps.direction == ArgumentDirection::Out) raw.push_back({ps.internalSymbol, s});
                    else { raw.push_back({s, ps.internalSymbol}); raw.push_back({ps.internalSymbol, s}); }
                }
            }
            visitDefault(inst);
        }
        void handle(const PrimitiveInstanceSymbol& p) {
            auto conns = p.getPortConnections();
            if (conns.size() < 2 || !conns[0]) return;
            const Expression* o = conns[0];
            if (o->kind == ExpressionKind::Assignment) o = &o->as<AssignmentExpression>().left();
            const Symbol* out = oneName(*o);
            if (!out) return;
            for (size_t i = 1; i < conns.size(); i++)
                if (conns[i]) link(*conns[i], out);
        }
    };

    void walk(const Scope& sc, const std::string& pre) {
        for (const Symbol& m : sc.members()) {
            if (m.kind == SymbolKind::Variable || m.kind == SymbolKind::Net) {
                std::string nm = pre + std::string(m.name);
                symName[&m] = nm;
                for (auto& b : bits(nm, m.as<ValueSymbol>().getType())) r.netNames.push_back(b);
                netSyms.push_back({nm, &m});
            } else if (m.kind == SymbolKind::Instance) {
                const auto& ch = m.as<InstanceSymbol>();
                if (!ch.isModule()) continue;
                std::string p = pre + std::string(ch.name);
                r.cellNames.push_back(p);
                r.instDef[p] = std::string(ch.getDefinition().name);
                auto& mine = r.pinsOfInst[p];
                for (const Symbol* ps : ch.body.getPortList()) {
                    if (ps->kind != SymbolKind::Port) continue;
                    const auto& port = ps->as<PortSymbol>();
                    std::string pn = p + "/" + std::string(port.name);
                    int dir = port.direction == ArgumentDirection::In ? 0 : port.direction == ArgumentDirection::Out ? 1 : 2;
                    for (auto& b : bits(pn, port.getType())) { r.pinNames.push_back(b); mine.push_back(b); }
                    if (port.internalSymbol) pinSyms.push_back({pn, port.internalSymbol, dir});
                }
                walk(ch.body, p + "/");
            } else if (m.kind == SymbolKind::GenerateBlock) {
                const auto& g = m.as<GenerateBlockSymbol>();
                if (g.isUninstantiated) continue;
                walk(g, pre + g.getExternalName() + ".");
            } else if (m.kind == SymbolKind::GenerateBlockArray) {
                const auto& ga = m.as<GenerateBlockArraySymbol>();
                std::string an = ga.name.empty() ? std::string("genblk") : std::string(ga.name);
                for (const auto* e : ga.entries) {
                    if (e->isUninstantiated) continue;
                    walk(*e, pre + an + "[" + (e->getArrayIndex() ? e->getArrayIndex()->toString() : std::string("?")) + "].");
                }
            }
        }
    }
    struct PinSym { std::string name; const Symbol* sym; int dir; };
    std::vector<std::pair<std::string, const Symbol*>> netSyms;
    std::vector<PinSym> pinSyms;

    std::function<void(const char*)> lap = [](const char*) {};
    bool build(std::string& err) {
        auto tops = comp.getRoot().topInstances;
        if (tops.empty()) { err = "no top module"; return false; }
        const InstanceSymbol& top = *tops[0];
        d.gather(comp, dummy);
        lap("  elaborate, find blocks");
        Extra ex(*this);
        comp.getRoot().visit(ex);
        lap("  assignments");
        walk(top.body, "");
        lap("  names");

        // ports of the top
        for (const Symbol* ps : top.body.getPortList()) {
            if (ps->kind != SymbolKind::Port) continue;
            const auto& port = ps->as<PortSymbol>();
            if (!port.internalSymbol) continue;
            Port p;
            p.name = std::string(port.name);
            p.dir = port.direction == ArgumentDirection::In ? 0 : port.direction == ArgumentDirection::Out ? 1 : 2;
            p.width = (int)port.getType().getBitWidth();
            where(port.location, p.file, p.line);
            symName[port.internalSymbol] = p.name;
            p.node = node(port.internalSymbol);
            for (auto& b : bits(p.name, port.getType())) r.portNames.push_back(b);
            r.portIdx[p.name] = (int)r.ports.size();
            r.ports.push_back(std::move(p));
        }
        for (auto& [nm, s] : netSyms) r.netNode[nm] = node(s);
        for (auto& ps : pinSyms) r.instPins[ps.name] = {node(ps.sym), ps.dir};

        // blocks: registers and combinational logic
        for (const Block& b : d.blocks) {
            auto kind = b.sym->procedureKind;
            if (kind == ProceduralBlockKind::Initial || kind == ProceduralBlockKind::Final) continue;
            std::vector<int> reads;
            for (const Symbol* s : b.reads) reads.push_back(node(s));
            std::sort(reads.begin(), reads.end());
            reads.erase(std::unique(reads.begin(), reads.end()), reads.end());
            std::set<const Symbol*> targets = b.nba;
            targets.insert(b.blocking.begin(), b.blocking.end());
            if (!b.seq || !b.clk) {
                for (const Symbol* t : targets) {
                    int tn = node(t);
                    for (int rd : reads)
                        if (rd != tn) edges.push_back({rd, tn});
                }
                continue;
            }
            Seq s;
            s.clk = node(b.clk);
            s.reads = reads;
            where(b.sym->location, s.file, s.line);
            int si = (int)r.seqs.size();
            for (const Symbol* t : targets) {
                if (b.locals.count(t)) continue;
                auto nm = symName.find(t);
                std::string name = (nm != symName.end() ? nm->second : path(*t)) + "_reg";
                auto [it, fresh] = r.regIdx.try_emplace(name, (int)r.regs.size());
                if (fresh) {
                    Reg rg;
                    rg.name = name;
                    rg.node = node(t);
                    rg.file = s.file;
                    rg.line = s.line;
                    if (auto* v = t->as_if<ValueSymbol>()) {
                        auto bn = bits(name, v->getType());
                        rg.width = (int)bn.size();
                        for (auto& bit : bn) {
                            r.cellNames.push_back(bit);
                            for (const char* p : {"/CK", "/D", "/Q"}) r.pinNames.push_back(bit + p);
                        }
                    }
                    r.regs.push_back(std::move(rg));
                }
                r.regs[it->second].blocks.push_back(si);
                s.regs.push_back(it->second);
            }
            r.seqs.push_back(std::move(s));
        }
        for (auto& [from, to] : ex.raw) {
            int a = node(from), b = node(to);
            if (a != b) edges.push_back({a, b});
        }

        lap("  registers");
        // CSR, both ways
        r.n = (int)r.nodeName.size();
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        r.fwdAt.assign(r.n + 1, 0);
        r.revAt.assign(r.n + 1, 0);
        for (auto& [a, b] : edges) { r.fwdAt[a + 1]++; r.revAt[b + 1]++; }
        for (int i = 0; i < r.n; i++) { r.fwdAt[i + 1] += r.fwdAt[i]; r.revAt[i + 1] += r.revAt[i]; }
        r.fwd.resize(edges.size());
        r.rev.resize(edges.size());
        std::vector<int> fa(r.fwdAt.begin(), r.fwdAt.end() - 1), ra(r.revAt.begin(), r.revAt.end() - 1);
        for (auto& [a, b] : edges) { r.fwd[fa[a]++] = b; r.rev[ra[b]++] = a; }
        r.nodeReg.assign(r.n, -1);
        for (size_t i = 0; i < r.regs.size(); i++) r.nodeReg[r.regs[i].node] = (int)i;
        r.readers.assign(r.n, {});
        for (size_t i = 0; i < r.seqs.size(); i++)
            for (int rd : r.seqs[i].reads) r.readers[rd].push_back((int)i);
        r.nodePortOut.assign(r.n, -1);
        for (size_t i = 0; i < r.ports.size(); i++)
            if (r.ports[i].dir != 0) r.nodePortOut[r.ports[i].node] = (int)i;
        return true;
    }
};

// --------------------------------------------------------------- the checks

struct Checker {
    const sdc::Reader& rd;
    const sdc::Constraints& c;
    const Rtl* rtl;  // null: the SDC file alone
    Rep& rep;
    unsigned threads;

    // The clocks that stand: one per name, the last definition.
    std::vector<const sdc::Clock*> clk;
    std::unordered_map<std::string, int> clkIdx;
    int W = 1;                        // 64-bit words per clock set
    std::vector<uint64_t> reach;      // per node: the clocks that arrive as a clock
    std::vector<uint64_t> launch;     // per node: the clocks that launch its data
    std::vector<std::vector<int>> roots;  // per clock: the source clocks it derives from

    Checker(const sdc::Reader& reader, const Rtl* r, Rep& rp, unsigned th) : rd(reader), c(reader.sdc), rtl(r), rep(rp), threads(th) {}

    using Mask = std::vector<uint64_t>;
    bool bit(const uint64_t* m, int i) const { return m[i / 64] >> (i % 64) & 1; }
    const uint64_t* at(const std::vector<uint64_t>& v, int node) const { return v.data() + size_t(node) * W; }
    std::vector<int> list(const uint64_t* m) const {
        std::vector<int> o;
        for (int i = 0; i < (int)clk.size(); i++)
            if (bit(m, i)) o.push_back(i);
        return o;
    }
    bool any(const uint64_t* m) const {
        for (int w = 0; w < W; w++)
            if (m[w]) return true;
        return false;
    }

    // ---- objects to graph nodes
    int nodeOfSource(const sdc::Obj& o) const {
        if (!rtl || !o.resolved) return -1;
        if (o.kind == sdc::Kind::Port) { const Port* p = rtl->port(o.name); return p ? p->node : -1; }
        if (o.kind == sdc::Kind::Pin) {
            char w = 0;
            int rg = rtl->regPin(o.name, w);
            if (rg >= 0) return w == 'Q' ? rtl->regs[rg].node : w == 'C' && !rtl->regs[rg].blocks.empty() ? rtl->seqs[rtl->regs[rg].blocks[0]].clk : -1;
            if (const InstPin* p = rtl->instPin(o.name)) return p->node;
            return -1;
        }
        if (o.kind == sdc::Kind::Net) {
            auto it = rtl->netNode.find(o.name);
            if (it == rtl->netNode.end()) it = rtl->netNode.find(Rtl::stripIdx(o.name));
            return it == rtl->netNode.end() ? -1 : it->second;
        }
        return -1;
    }

    // Clock propagation: from each source through combinational logic, never
    // through a register, and stopping where another clock is defined
    // without -add.
    static std::vector<uint64_t> propagate(const Rtl& rtl, const std::vector<const sdc::Clock*>& clk, int W,
                                           const std::function<int(const sdc::Obj&)>& nodeOf) {
        std::vector<uint64_t> m(size_t(rtl.n) * W, 0);
        std::vector<char> blocked(rtl.n, 0);
        std::vector<int> work;
        for (size_t i = 0; i < clk.size(); i++)
            for (auto& s : clk[i]->sources) {
                int nd = nodeOf(s);
                if (nd < 0) continue;
                m[size_t(nd) * W + i / 64] |= 1ull << (i % 64);
                if (!clk[i]->add) blocked[nd] = 1;
                work.push_back(nd);
            }
        // A source with any -add clock lets the others through as well.
        for (size_t i = 0; i < clk.size(); i++)
            if (clk[i]->add)
                for (auto& s : clk[i]->sources) {
                    int nd = nodeOf(s);
                    if (nd >= 0) blocked[nd] = 0;
                }
        while (!work.empty()) {
            int u = work.back();
            work.pop_back();
            for (int e = rtl.fwdAt[u]; e < rtl.fwdAt[u + 1]; e++) {
                int v = rtl.fwd[e];
                if (blocked[v] || rtl.nodeReg[v] >= 0) continue;
                bool ch = false;
                for (int w = 0; w < W; w++) {
                    uint64_t nv = m[size_t(v) * W + w] | m[size_t(u) * W + w];
                    if (nv != m[size_t(v) * W + w]) { m[size_t(v) * W + w] = nv; ch = true; }
                }
                if (ch) work.push_back(v);
            }
        }
        return m;
    }

    void setupClocks() {
        for (auto& k : c.clocks) {
            auto it = clkIdx.find(k.name);
            if (it == clkIdx.end()) { clkIdx[k.name] = (int)clk.size(); clk.push_back(&k); }
            else clk[it->second] = &k;
        }
        // A later clock on the same source, without -add, removes the earlier one.
        {
            std::map<sdc::Obj, size_t> owner;
            std::set<const sdc::Clock*> gone;
            for (auto& k : c.clocks) {
                if (c.clock(k.name) != &k) continue;
                for (auto& s : k.sources) {
                    auto [it, fresh] = owner.try_emplace(s, size_t(&k - c.clocks.data()));
                    if (fresh) continue;
                    if (!k.add) gone.insert(&c.clocks[it->second]);
                    it->second = size_t(&k - c.clocks.data());
                }
            }
            if (!gone.empty()) {
                std::vector<const sdc::Clock*> keep;
                for (auto k : clk)
                    if (!gone.count(k)) keep.push_back(k);
                clk = keep;
                clkIdx.clear();
                for (size_t i = 0; i < clk.size(); i++) clkIdx[clk[i]->name] = (int)i;
            }
        }
        W = std::max(1, ((int)clk.size() + 63) / 64);
        if (rtl) reach = propagate(*rtl, clk, W, [&](const sdc::Obj& o) { return nodeOfSource(o); });
        // The source clocks each clock derives from.
        roots.assign(clk.size(), {});
        std::vector<std::vector<int>> master(clk.size());
        for (size_t i = 0; i < clk.size(); i++) {
            if (!clk[i]->generated) continue;
            if (!clk[i]->masterClock.empty()) {
                auto it = clkIdx.find(clk[i]->masterClock);
                if (it != clkIdx.end()) master[i].push_back(it->second);
            } else if (rtl) {
                for (auto& p : clk[i]->masterPins) {
                    int nd = nodeOfSource(p);
                    if (nd < 0) continue;
                    for (int m : list(at(reach, nd)))
                        if (m != (int)i) master[i].push_back(m);
                }
            }
        }
        for (size_t i = 0; i < clk.size(); i++) {
            std::set<int> seen{(int)i}, out;
            std::vector<int> st{(int)i};
            while (!st.empty()) {
                int k = st.back();
                st.pop_back();
                if (master[k].empty()) out.insert(k);
                for (int m : master[k])
                    if (seen.insert(m).second) st.push_back(m);
            }
            roots[i].assign(out.begin(), out.end());
        }
        masters_ = std::move(master);
    }
    std::vector<std::vector<int>> masters_;
    // The period of a clock; of a generated clock, from its master. 0: unknown.
    double periodOf(int i, int depth = 0) const {
        const sdc::Clock& k = *clk[i];
        if (!k.generated) return k.period;
        if (masters_[i].empty() || depth > 16) return 0;
        double p = periodOf(masters_[i][0], depth + 1);
        if (k.divideBy > 0) p *= k.divideBy;
        if (k.multiplyBy > 0) p /= k.multiplyBy;
        if (k.edges.size() == 3 && k.divideBy == 0 && k.multiplyBy == 0) p *= (k.edges[2] - k.edges[0]) / 2.0;
        return p;
    }
    bool related(int a, int b) const {
        for (int x : roots[a])
            for (int y : roots[b])
                if (x == y) return true;
        return false;
    }

    // ---- reading the file
    void checkDiags() {
        for (auto& d : rd.diags) {
            const char* rule = d.code == "unknown-command" ? "SDC002" : d.code == "no-match" ? "SDC003" : d.code == "no-clock" ? "SDC004" : "SDC001";
            if (d.code == "unsupported") {
                rep.add("SDC001", d.loc, d.text);
                continue;
            }
            rep.add(rule, d.loc, d.text, d.sev == sdc::Diag::ErrorSev);
        }
    }

    // ---- clocks
    static std::string num(double v) {
        char b[40];
        snprintf(b, sizeof b, "%g", v);
        return b;
    }
    void checkClocks() {
        std::map<std::string, const sdc::Clock*> byName;
        std::map<sdc::Obj, const sdc::Clock*> bySource;
        for (auto& k : c.clocks) {
            if (k.name.empty()) {
                rep.add("SDC012", k.loc, "this clock has no name and no source, so nothing can refer to it; give -name or a source");
                continue;
            }
            auto [it, fresh] = byName.try_emplace(k.name, &k);
            if (!fresh) {
                rep.add("SDC010", k.loc, "clock '" + k.name + "' is defined again; this definition replaces the one at " + it->second->loc.str());
                it->second = &k;
            }
            for (auto& s : k.sources) {
                auto [st, f2] = bySource.try_emplace(s, &k);
                if (!f2 && st->second->name != k.name && !k.add)
                    rep.add("SDC011", k.loc, "clock '" + k.name + "' is on " + s.name + ", which already has clock '" + st->second->name +
                                                 "' (" + st->second->loc.str() + "); without -add the earlier clock is removed");
                if (!f2) st->second = &k;
            }
            if (!k.generated) {
                const sdc::Command* cmd = nullptr;
                for (auto& x : c.commands)
                    if (x.name == "create_clock" && x.loc.line == k.loc.line && x.loc.file == k.loc.file) cmd = &x;
                if (cmd && !cmd->has("-period")) rep.add("SDC012", k.loc, "clock '" + k.name + "' has no -period", true);
                else if (k.period <= 0) rep.add("SDC012", k.loc, "clock '" + k.name + "' has period " + num(k.period) + "; it must be above zero", true);
                else if (k.hasWaveform) {
                    auto& w = k.waveform;
                    bool inc = true;
                    for (size_t i = 1; i < w.size(); i++) inc &= w[i] > w[i - 1];
                    if (w.size() < 2 || w.size() % 2)
                        rep.add("SDC012", k.loc, "clock '" + k.name + "': -waveform needs an even number of edges (rise fall ...), it has " + std::to_string(w.size()));
                    else if (!inc)
                        rep.add("SDC012", k.loc, "clock '" + k.name + "': the -waveform edges must increase");
                    else if (w.back() - w.front() > k.period)
                        rep.add("SDC012", k.loc, "clock '" + k.name + "': the -waveform edges span " + num(w.back() - w.front()) +
                                                     ", more than the period " + num(k.period));
                }
            } else {
                if (k.sources.empty())
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "' has no source object; a generated clock cannot be virtual", true);
                if (k.masterPins.empty())
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "' has no -source (the master clock pin)", true);
                if (!k.divideBy && !k.multiplyBy && k.edges.empty() && !k.combinational && !k.invert)
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "' has none of -divide_by, -multiply_by, -edges; it is a copy of its master, say so with -divide_by 1 or -combinational");
                if (!k.edges.empty() && k.edges.size() != 3)
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "': -edges takes three edges, it has " + std::to_string(k.edges.size()));
                if (!k.edges.empty() && (k.divideBy || k.multiplyBy))
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "': -edges cannot be combined with -divide_by or -multiply_by");
                if (!k.edgeShift.empty() && k.edgeShift.size() != k.edges.size())
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "': -edge_shift needs one shift per edge of -edges");
                if ((k.divideBy < 0) || (k.multiplyBy < 0) || (c.clock(k.name) == &k && cmdHasZero(k)))
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "': -divide_by and -multiply_by must be 1 or more", true);
            }
        }
        if (!rtl) return;
        // Masters of generated clocks, in the design.
        for (size_t i = 0; i < clk.size(); i++) {
            const sdc::Clock& k = *clk[i];
            if (!k.generated || k.masterPins.empty()) continue;
            std::vector<int> arriving;
            bool anyNode = false;
            for (auto& p : k.masterPins) {
                int nd = nodeOfSource(p);
                if (nd < 0) continue;
                anyNode = true;
                for (int m : list(at(reach, nd)))
                    if (m != (int)i) arriving.push_back(m);
            }
            if (!anyNode) continue;
            if (arriving.empty())
                rep.add("SDC013", k.loc, "generated clock '" + k.name + "': no clock reaches its -source " + k.masterPins[0].name + ", so it has no master");
            else if (!k.masterClock.empty()) {
                auto it = clkIdx.find(k.masterClock);
                if (it != clkIdx.end() && std::find(arriving.begin(), arriving.end(), it->second) == arriving.end())
                    rep.add("SDC013", k.loc, "generated clock '" + k.name + "': -master_clock " + k.masterClock + " does not reach its -source " + k.masterPins[0].name);
            } else if (arriving.size() > 1) {
                std::string names;
                for (int m : arriving) names += (names.empty() ? "" : ", ") + clk[m]->name;
                rep.add("SDC013", k.loc, "generated clock '" + k.name + "': several clocks reach its -source (" + names + "); name one with -master_clock");
            }
        }
        // Which clocks clock something, and the registers nobody clocks.
        std::vector<char> used(clk.size(), 0);
        std::map<int, std::pair<int, const Seq*>> unclocked;       // clock node -> count, first block
        std::vector<char> counted(rtl->regs.size(), 0);
        std::map<std::pair<int, std::vector<int>>, std::pair<int, const Seq*>> multi;
        for (auto& s : rtl->seqs) {
            auto here = list(at(reach, s.clk));
            for (int k : here) used[k] = 1;
            int nreg = 0;
            for (int rg : s.regs)
                if (!counted[rg]) { counted[rg] = 1; nreg += rtl->regs[rg].width; }
            if (here.empty()) {
                auto& u = unclocked[s.clk];
                if (!u.second) u.second = &s;
                u.first += nreg;
            } else if (here.size() > 1) {
                bool open = false;
                for (size_t a = 0; a < here.size() && !open; a++)
                    for (size_t b = a + 1; b < here.size(); b++)
                        if (!grouped(here[a], here[b])) { open = true; break; }
                if (open && !caseSelects(s.clk)) {
                    auto& u = multi[{s.clk, here}];
                    if (!u.second) u.second = &s;
                    u.first += nreg;
                }
            }
        }
        for (auto& [nd, u] : unclocked)
            rep.add("SDC015", u.second->file, u.second->line,
                    std::to_string(u.first) + (u.first == 1 ? " register bit is" : " register bits are") + " clocked by '" + rtl->nodeName[nd] +
                        "', which no clock reaches; these registers are not timed (create_clock or create_generated_clock is missing)");
        for (auto& [key, u] : multi) {
            std::string names;
            for (int k : key.second) names += (names.empty() ? "" : ", ") + clk[k]->name;
            rep.add("SDC016", u.second->file, u.second->line,
                    "clocks " + names + " all reach '" + rtl->nodeName[key.first] + "' (" + std::to_string(u.first) +
                        (u.first == 1 ? " register bit" : " register bits") + ") and are not declared exclusive; every pair is timed against every other. "
                        "Use set_clock_groups -logically_exclusive or -physically_exclusive, or set_case_analysis on the select");
        }
        for (size_t i = 0; i < clk.size(); i++) {
            for (size_t j = 0; j < clk.size(); j++)
                for (int m : masters_[j])
                    if (m == (int)i) used[i] = 1;
            for (auto& d : c.ioDelays)
                if (d.clock == clk[i]->name) used[i] = 1;
            if (used[i]) continue;
            if (clk[i]->isVirtual())
                rep.add("SDC014", clk[i]->loc, "virtual clock '" + clk[i]->name + "' is used by no set_input_delay or set_output_delay");
            else {
                bool mapped = false;
                for (auto& s : clk[i]->sources) mapped |= nodeOfSource(s) >= 0;
                if (mapped)
                    rep.add("SDC014", clk[i]->loc, "clock '" + clk[i]->name + "' reaches no register and no constraint uses it");
            }
        }
        // Uncertainty.
        std::vector<char> unc(clk.size(), 0);
        for (auto& u : c.uncertainties) {
            for (auto& o : u.objects)
                if (o.kind == sdc::Kind::Clock && clkIdx.count(o.name)) unc[clkIdx[o.name]] = 1;
                else if (o.kind != sdc::Kind::Clock) std::fill(unc.begin(), unc.end(), 1);  // on a pin or port: not followed here
            for (auto& n : u.from) if (clkIdx.count(n)) unc[clkIdx[n]] = 1;
            for (auto& n : u.to) if (clkIdx.count(n)) unc[clkIdx[n]] = 1;
        }
        std::vector<int> bare;
        for (size_t i = 0; i < clk.size(); i++)
            if (!unc[i] && !clk[i]->isVirtual()) bare.push_back((int)i);
        if (!bare.empty()) {
            std::string names;
            for (size_t i = 0; i < bare.size() && i < 8; i++) names += (i ? ", " : "") + clk[bare[i]]->name;
            if (bare.size() > 8) names += ", ...";
            rep.add("SDC018", clk[bare[0]]->loc, (bare.size() == 1 ? "clock '" + names + "' has" : std::to_string(bare.size()) + " clocks (" + names + ") have") +
                                                     " no set_clock_uncertainty, so jitter and skew margin are zero");
        }
    }
    bool cmdHasZero(const sdc::Clock& k) const {
        for (auto& x : c.commands)
            if (x.name == "create_generated_clock" && x.loc.line == k.loc.line && x.loc.file == k.loc.file)
                return (x.has("-divide_by") && x.num("-divide_by") < 1) || (x.has("-multiply_by") && x.num("-multiply_by") < 1);
        return false;
    }
    // Is there a set_case_analysis on the logic in front of this clock net
    // (the select of a clock mux)?
    bool caseSelects(int clkNode) const {
        if (c.caseAnalysis.empty()) return false;
        std::unordered_set<int> cased;
        for (auto& ca : c.caseAnalysis)
            for (auto& o : ca.objects) {
                int nd = nodeOfSource(o);
                if (nd >= 0) cased.insert(nd);
            }
        Marks seen;
        seen.begin(rtl->n);
        std::vector<int> st{clkNode};
        seen.set(clkNode);
        while (!st.empty()) {
            int u = st.back();
            st.pop_back();
            if (cased.count(u)) return true;
            if (rtl->nodeReg[u] >= 0) continue;
            for (int e = rtl->revAt[u]; e < rtl->revAt[u + 1]; e++)
                if (seen.set(rtl->rev[e])) st.push_back(rtl->rev[e]);
        }
        return false;
    }

    // ---- clock groups
    std::vector<std::vector<char>> group_;  // 0 none, 1 async, 2 exclusive
    void setupGroups() {
        group_.assign(clk.size(), std::vector<char>(clk.size(), 0));
        for (auto& g : c.clockGroups) {
            if (g.type == sdc::ClockGroups::None)
                rep.add("SDC042", g.loc, "set_clock_groups needs one of -asynchronous, -logically_exclusive, -physically_exclusive", true);
            if (g.groups.empty()) rep.add("SDC042", g.loc, "set_clock_groups has no -group", true);
            std::map<std::string, int> where;
            std::vector<char> inAny(clk.size(), 0);
            for (size_t gi = 0; gi < g.groups.size(); gi++) {
                if (g.groups[gi].empty()) rep.add("SDC042", g.loc, "set_clock_groups: -group number " + std::to_string(gi + 1) + " names no clock");
                for (auto& n : g.groups[gi]) {
                    auto [it, fresh] = where.try_emplace(n, (int)gi);
                    if (!fresh && it->second != (int)gi)
                        rep.add("SDC042", g.loc, "set_clock_groups: clock '" + n + "' is in two groups, so it is declared unrelated to itself");
                    if (clkIdx.count(n)) inAny[clkIdx[n]] = 1;
                }
            }
            char t = g.type == sdc::ClockGroups::Asynchronous ? 1 : 2;
            auto mark = [&](int a, int b) { if (a != b) group_[a][b] = group_[b][a] = t; };
            for (size_t x = 0; x < g.groups.size(); x++) {
                for (auto& a : g.groups[x]) {
                    if (!clkIdx.count(a)) continue;
                    if (g.groups.size() == 1) {  // one group: against every other clock
                        for (size_t o = 0; o < clk.size(); o++)
                            if (!inAny[o]) mark(clkIdx[a], (int)o);
                    }
                    for (size_t y = x + 1; y < g.groups.size(); y++)
                        for (auto& b : g.groups[y])
                            if (clkIdx.count(b)) mark(clkIdx[a], clkIdx[b]);
                }
            }
        }
    }
    bool grouped(int a, int b) const { return group_[a][b] != 0; }

    // ---- I/O delays
    void checkIo() {
        struct Key {
            std::string port; bool input;
            bool operator<(const Key& o) const { return std::tie(port, input) < std::tie(o.port, o.input); }
        };
        std::map<Key, std::vector<const sdc::IoDelay*>> per;
        std::set<std::string> clockPorts;
        for (auto k : clk)
            for (auto& s : k->sources)
                if (s.kind == sdc::Kind::Port) clockPorts.insert(Rtl::stripIdx(s.name));
        for (auto& d : c.ioDelays) {
            const char* cmd = d.input ? "set_input_delay" : "set_output_delay";
            if (d.clock.empty())
                rep.add("SDC025", d.loc, std::string(cmd) + " has no -clock, so the delay is relative to nothing and constrains only port-to-port paths");
            else if (auto it = clkIdx.find(d.clock); it != clkIdx.end()) {
                const sdc::Clock& k = *clk[it->second];
                double period = k.period;
                if (!k.generated && period > 0 && std::fabs(d.value) > period)
                    rep.add("SDC022", d.loc, std::string(cmd) + " " + num(d.value) + " is more than the period of clock '" + k.name + "' (" + num(period) + "); no time is left inside the design");
            }
            std::set<std::string> seen;
            for (auto& p : d.ports) {
                if (p.kind != sdc::Kind::Port) continue;
                std::string base = Rtl::stripIdx(p.name);
                if (rtl && p.resolved && seen.insert(base).second) {
                    int dir = rtl->portDir(p.name);
                    if (d.input && dir == 1)
                        rep.add("SDC024", d.loc, "set_input_delay on '" + base + "', which is an output port");
                    if (!d.input && dir == 0)
                        rep.add("SDC024", d.loc, "set_output_delay on '" + base + "', which is an input port");
                    if (d.input && clockPorts.count(base))
                        rep.add("SDC024", d.loc, "set_input_delay on '" + base + "', which is a clock source; a delay there moves the clock's data arrival, not its edges (use set_clock_latency)");
                }
                per[{p.name, d.input}].push_back(&d);
            }
        }
        std::set<std::pair<int, int>> said;
        for (auto& [key, v] : per) {
            for (size_t i = 0; i < v.size(); i++) {
                for (size_t j = i + 1; j < v.size(); j++) {
                    const sdc::IoDelay &a = *v[i], &b = *v[j];
                    bool aMin = a.min || !a.max, aMax = a.max || !a.min, bMin = b.min || !b.max, bMax = b.max || !b.min;
                    bool edge = (!a.rise && !a.fall) || (!b.rise && !b.fall) || (a.rise && b.rise) || (a.fall && b.fall);
                    if (a.clock == b.clock && a.clockFall == b.clockFall && edge) {
                        const sdc::IoDelay* mn = a.min && !a.max ? &a : b.min && !b.max ? &b : nullptr;
                        const sdc::IoDelay* mx = a.max && !a.min ? &a : b.max && !b.min ? &b : nullptr;
                        if (mn && mx && mn != mx && mn->value > mx->value && said.insert({mn->loc.line, mx->loc.line}).second)
                            rep.add("SDC023", mn->loc, "-min delay " + num(mn->value) + " on '" + Rtl::stripIdx(key.port) + "' is above its -max delay " + num(mx->value) + " (" + mx->loc.str() + ")");
                    }
                    if (!b.add && edge && ((aMin && bMin) || (aMax && bMax)) && (a.clock != b.clock || a.clockFall != b.clockFall || a.value != b.value) &&
                        said.insert({a.loc.line, -b.loc.line}).second)
                        rep.add("SDC027", b.loc, std::string(b.input ? "set_input_delay" : "set_output_delay") + " on '" + Rtl::stripIdx(key.port) +
                                                     "' replaces the one at " + a.loc.str() + (a.clock != b.clock ? " (clock '" + a.clock + "')" : "") +
                                                     "; add -add_delay to keep both");
                }
            }
        }
    }

    // ---- exceptions
    static std::string keyOf(const sdc::PathSpec& p) {
        std::string k;
        auto add = [&](const sdc::ObjList& l, char tag) {
            std::vector<std::string> n;
            for (auto& o : l) n.push_back(std::string(sdc::kindName(o.kind)) + ":" + o.name);
            std::sort(n.begin(), n.end());
            k += tag;
            for (auto& s : n) k += s + ",";
        };
        add(p.from, 'F');
        for (auto& t : p.through) add(t, 'T');
        add(p.to, 'E');
        k += p.riseFrom ? "r" : "";
        k += p.fallFrom ? "f" : "";
        k += p.riseTo ? "R" : "";
        k += p.fallTo ? "X" : "";
        return k;
    }
    static const char* exName(sdc::Exception::Type t) {
        switch (t) {
            case sdc::Exception::FalsePath: return "set_false_path";
            case sdc::Exception::Multicycle: return "set_multicycle_path";
            case sdc::Exception::MaxDelay: return "set_max_delay";
            case sdc::Exception::MinDelay: return "set_min_delay";
            default: return "group_path";
        }
    }

    // What an exception endpoint is in the design.
    struct Ends {
        std::vector<int> nodes;  // start: signals data leaves from; end: register indices (>=0) or ~port (<0)
        Mask clocks;
        bool all = false;        // the option was not given
    };
    // Startpoints. Returns "" or why `o` is not one.
    std::string startOf(const sdc::Obj& o, Ends& e) const {
        if (o.kind == sdc::Kind::Clock) {
            auto it = clkIdx.find(o.name);
            if (it != clkIdx.end()) e.clocks[it->second / 64] |= 1ull << (it->second % 64);
            return "";
        }
        if (!rtl || !o.resolved) return "";
        if (o.kind == sdc::Kind::Port) {
            const Port* p = rtl->port(o.name);
            if (!p) return "";
            if (p->dir == 1) return "an output port";
            e.nodes.push_back(p->node);
            return "";
        }
        if (o.kind == sdc::Kind::Cell) {
            int rg = rtl->reg(o.name);
            if (rg >= 0) { e.nodes.push_back(rtl->regs[rg].node); return ""; }
            bool anyReg = false;
            std::string pre = o.name + "/";
            for (auto& r : rtl->regs)
                if (r.name.compare(0, pre.size(), pre) == 0) { e.nodes.push_back(r.node); anyReg = true; }
            return anyReg ? "" : "a cell with no register inside";
        }
        if (o.kind == sdc::Kind::Pin) {
            char w = 0;
            int rg = rtl->regPin(o.name, w);
            if (rg >= 0) {
                if (w == 'D') return "the data input of a register (an endpoint, not a startpoint)";
                e.nodes.push_back(rtl->regs[rg].node);
                return "";
            }
            return "a hierarchical pin; a path starts at a clock, an input port or a register (use -through for a pin on the way)";
        }
        return "a net; a path starts at a clock, an input port or a register (use -through for a net on the way)";
    }
    std::string endOf(const sdc::Obj& o, Ends& e) const {
        if (o.kind == sdc::Kind::Clock) {
            auto it = clkIdx.find(o.name);
            if (it != clkIdx.end()) e.clocks[it->second / 64] |= 1ull << (it->second % 64);
            return "";
        }
        if (!rtl || !o.resolved) return "";
        if (o.kind == sdc::Kind::Port) {
            const Port* p = rtl->port(o.name);
            if (!p) return "";
            if (p->dir == 0) return "an input port";
            e.nodes.push_back(~int(p - rtl->ports.data()));
            return "";
        }
        if (o.kind == sdc::Kind::Cell) {
            int rg = rtl->reg(o.name);
            if (rg >= 0) { e.nodes.push_back(rg); return ""; }
            bool anyReg = false;
            std::string pre = o.name + "/";
            for (size_t i = 0; i < rtl->regs.size(); i++)
                if (rtl->regs[i].name.compare(0, pre.size(), pre) == 0) { e.nodes.push_back((int)i); anyReg = true; }
            return anyReg ? "" : "a cell with no register inside";
        }
        if (o.kind == sdc::Kind::Pin) {
            char w = 0;
            int rg = rtl->regPin(o.name, w);
            if (rg >= 0) {
                if (w == 'Q') return "the output of a register (a startpoint, not an endpoint)";
                if (w == 'C') return "the clock pin of a register (a startpoint, not an endpoint)";
                e.nodes.push_back(rg);
                return "";
            }
            return "a hierarchical pin; a path ends at a clock, an output port or a register (use -through for a pin on the way)";
        }
        return "a net; a path ends at a clock, an output port or a register (use -through for a net on the way)";
    }
    int throughNode(const sdc::Obj& o) const {
        if (!rtl || !o.resolved) return -1;
        if (o.kind == sdc::Kind::Cell) {
            int rg = rtl->reg(o.name);
            return rg >= 0 ? rtl->regs[rg].node : -1;
        }
        if (o.kind == sdc::Kind::Pin) {
            char w = 0;
            int rg = rtl->regPin(o.name, w);
            if (rg >= 0) return w == 'Q' ? rtl->regs[rg].node : -1;
        }
        return nodeOfSource(o);
    }

    struct ExInfo {
        Ends from, to;
        bool valid = true;      // every endpoint is a real start or end
        bool mapped = true;     // every through object is a signal we know
        std::vector<std::vector<int>> through;
        std::unordered_set<int> fromSet, toSet;
    };
    std::vector<ExInfo> ex_;

    // The signals reachable from `start` through combinational logic.
    void closure(std::vector<int>& work, Marks& seen) const {
        while (!work.empty()) {
            int u = work.back();
            work.pop_back();
            for (int e = rtl->fwdAt[u]; e < rtl->fwdAt[u + 1]; e++) {
                int v = rtl->fwd[e];
                if (rtl->nodeReg[v] >= 0) continue;  // a register's output is not driven by logic
                if (seen.set(v)) work.push_back(v);
            }
        }
    }
    bool regClockedBy(int rg, const Mask& m) const {
        for (int b : rtl->regs[rg].blocks) {
            const uint64_t* r = at(reach, rtl->seqs[b].clk);
            for (int w = 0; w < W; w++)
                if (r[w] & m[w]) return true;
        }
        return false;
    }
    bool pathExists(const ExInfo& x, Marks& seen) const {
        std::vector<int> work;
        seen.begin(rtl->n);
        auto seed = [&](int nd) { if (seen.set(nd)) work.push_back(nd); };
        if (x.from.all) {
            for (auto& r : rtl->regs) seed(r.node);
            for (auto& p : rtl->ports) if (p.dir != 1) seed(p.node);
        } else {
            for (int nd : x.from.nodes) seed(nd);
            if (any(x.from.clocks.data())) {
                for (size_t i = 0; i < rtl->regs.size(); i++)
                    if (regClockedBy((int)i, x.from.clocks)) seed(rtl->regs[i].node);
                for (auto& d : c.ioDelays) {
                    auto it = clkIdx.find(d.clock);
                    if (!d.input || it == clkIdx.end() || !bit(x.from.clocks.data(), it->second)) continue;
                    for (auto& p : d.ports)
                        if (const Port* pp = rtl->port(p.name)) seed(pp->node);
                }
            }
        }
        closure(work, seen);
        for (auto& th : x.through) {
            std::vector<int> next;
            for (int nd : th)
                if (seen.test(nd)) next.push_back(nd);
            if (next.empty()) return false;
            seen.begin(rtl->n);
            for (int nd : next) seen.set(nd);
            work = next;
            closure(work, seen);
        }
        auto regHit = [&](int rg) {
            for (int b : rtl->regs[rg].blocks)
                for (int rdn : rtl->seqs[b].reads)
                    if (seen.test(rdn)) return true;
            return false;
        };
        if (x.to.all) {
            for (size_t i = 0; i < rtl->regs.size(); i++) if (regHit((int)i)) return true;
            for (auto& p : rtl->ports) if (p.dir != 0 && seen.test(p.node)) return true;
            return false;
        }
        for (int e : x.to.nodes) {
            if (e >= 0 ? regHit(e) : seen.test(rtl->ports[~e].node)) return true;
        }
        if (any(x.to.clocks.data())) {
            for (size_t i = 0; i < rtl->regs.size(); i++)
                if (regClockedBy((int)i, x.to.clocks) && regHit((int)i)) return true;
            for (auto& d : c.ioDelays) {
                auto it = clkIdx.find(d.clock);
                if (d.input || it == clkIdx.end() || !bit(x.to.clocks.data(), it->second)) continue;
                for (auto& p : d.ports)
                    if (const Port* pp = rtl->port(p.name); pp && seen.test(pp->node)) return true;
            }
        }
        return false;
    }

    void checkExceptions() {
        ex_.resize(c.exceptions.size());
        std::map<std::string, std::vector<size_t>> byKey;
        for (size_t i = 0; i < c.exceptions.size(); i++) {
            const sdc::Exception& e = c.exceptions[i];
            ExInfo& x = ex_[i];
            x.from.clocks.assign(W, 0);
            x.to.clocks.assign(W, 0);
            x.from.all = e.path.from.empty();
            x.to.all = e.path.to.empty();
            const char* name = exName(e.type);
            if (e.emptyEndpoint) {
                rep.add("SDC030", e.loc, std::string(name) + ": a -from, -through or -to names nothing, so the exception applies to no path");
                x.valid = false;
            } else if (e.path.empty() && e.type != sdc::Exception::GroupPath) {
                rep.add("SDC030", e.loc, std::string(name) + " has no -from, -through or -to: it applies to every path in the design");
            }
            std::set<std::string> said;
            for (auto& o : e.path.from) {
                std::string why = startOf(o, x.from);
                if (!why.empty()) {
                    x.valid = false;
                    if (said.insert(why).second)
                        rep.add("SDC031", e.loc, std::string(name) + " -from " + o.name + ": this is " + why);
                }
            }
            said.clear();
            for (auto& o : e.path.to) {
                std::string why = endOf(o, x.to);
                if (!why.empty()) {
                    x.valid = false;
                    if (said.insert(why).second)
                        rep.add("SDC032", e.loc, std::string(name) + " -to " + o.name + ": this is " + why);
                }
            }
            for (auto& th : e.path.through) {
                std::vector<int> nodes;
                for (auto& o : th) {
                    int nd = throughNode(o);
                    if (nd >= 0) nodes.push_back(nd);
                    else x.mapped = false;
                }
                x.through.push_back(std::move(nodes));
            }
            x.fromSet.insert(x.from.nodes.begin(), x.from.nodes.end());
            x.toSet.insert(x.to.nodes.begin(), x.to.nodes.end());
            // values
            if (e.type == sdc::Exception::Multicycle) {
                if (e.value < 0 || (e.value < 1 && !e.hold))
                    rep.add("SDC037", e.loc, "set_multicycle_path " + num(e.value) + ": a setup multiplier below 1 asks for the data before it is launched");
            } else if (e.type == sdc::Exception::MaxDelay && e.value < 0) {
                rep.add("SDC037", e.loc, "set_max_delay " + num(e.value) + " is negative");
            }
            if (e.type != sdc::Exception::GroupPath && !e.emptyEndpoint) byKey[keyOf(e.path)].push_back(i);
        }
        // Same path spec: repeats, overrides, setup without hold, min above max.
        for (auto& [key, v] : byKey) {
            const sdc::Exception* fp = nullptr;
            for (size_t i : v)
                if (c.exceptions[i].type == sdc::Exception::FalsePath && !c.exceptions[i].setup && !c.exceptions[i].hold) fp = &c.exceptions[i];
            const sdc::Exception *mn = nullptr, *mx = nullptr;
            for (size_t a = 0; a < v.size(); a++) {
                const sdc::Exception& e = c.exceptions[v[a]];
                if (e.type == sdc::Exception::MinDelay) mn = &e;
                if (e.type == sdc::Exception::MaxDelay) mx = &e;
                if (fp && e.type != sdc::Exception::FalsePath)
                    rep.add("SDC035", e.loc, std::string(exName(e.type)) + " is on the same paths as the set_false_path at " + fp->loc.str() + "; the false path wins and this constraint is ignored");
                for (size_t b = a + 1; b < v.size(); b++) {
                    const sdc::Exception& f = c.exceptions[v[b]];
                    if (e.type != f.type || e.setup != f.setup || e.hold != f.hold || e.rise != f.rise || e.fall != f.fall || e.start != f.start || e.end != f.end) continue;
                    rep.add("SDC035", f.loc, std::string(exName(f.type)) + (e.value == f.value ? " repeats the one at " : " replaces the one at ") + e.loc.str() +
                                                 (e.value == f.value ? "" : " (" + num(e.value) + " becomes " + num(f.value) + ")"));
                }
                if (e.type == sdc::Exception::Multicycle && !e.hold && e.value >= 2 && !fp) {
                    bool hold = false;
                    for (size_t b : v) {
                        const sdc::Exception& f = c.exceptions[b];
                        if (f.type == sdc::Exception::Multicycle && f.hold) hold = true;
                    }
                    if (!hold)
                        rep.add("SDC034", e.loc, "set_multicycle_path " + num(e.value) + " -setup has no matching -hold: the hold check moves to " + num(e.value - 1) +
                                                     " cycles after launch. Add set_multicycle_path " + num(e.value - 1) + " -hold on the same paths to keep it at the launch edge");
                }
            }
            if (mn && mx && mn->value > mx->value)
                rep.add("SDC037", mn->loc, "set_min_delay " + num(mn->value) + " is above the set_max_delay " + num(mx->value) + " on the same paths (" + mx->loc.str() + ")");
        }
        if (!rtl || rep.disabled.count("SDC033")) return;
        // A path must exist between the ends: one search per exception.
        std::vector<char> dead(c.exceptions.size(), 0);
        parallelFor(c.exceptions.size(), threads, [&](size_t i) {
            const sdc::Exception& e = c.exceptions[i];
            const ExInfo& x = ex_[i];
            if (!x.valid || !x.mapped || e.type == sdc::Exception::GroupPath) return;
            if (x.from.all && x.to.all && x.through.empty()) return;
            static thread_local Marks seen;
            if (!pathExists(x, seen)) dead[i] = 1;
        });
        for (size_t i = 0; i < dead.size(); i++)
            if (dead[i])
                rep.add("SDC033", c.exceptions[i].loc, std::string(exName(c.exceptions[i].type)) +
                                                           ": the design has no path between these points, so the exception does nothing (a stale name, or the wrong direction)");
    }

    // ---- clocks against the paths of the design
    bool clockLevelException(int l, int cap) const {
        for (size_t i = 0; i < c.exceptions.size(); i++) {
            const sdc::Exception& e = c.exceptions[i];
            if (e.type == sdc::Exception::GroupPath || e.type == sdc::Exception::MinDelay) continue;
            const ExInfo& x = ex_[i];
            if (!x.through.empty()) continue;
            bool f = bit(x.from.clocks.data(), l), t = bit(x.to.clocks.data(), cap);
            if ((f && (t || x.to.all)) || (t && x.from.all)) return true;
        }
        return false;
    }

    void checkCrossings() {
        if (!rtl) return;
        // Launch clocks: of a register, the clocks that clock it; of an input
        // port, the clocks of its input delays. Then through the logic.
        launch.assign(size_t(rtl->n) * W, 0);
        std::vector<int> work;
        for (auto& r : rtl->regs) {
            for (int b : r.blocks)
                for (int w = 0; w < W; w++) launch[size_t(r.node) * W + w] |= at(reach, rtl->seqs[b].clk)[w];
            work.push_back(r.node);
        }
        std::vector<char> inSeed(rtl->n, 0);
        for (auto& d : c.ioDelays) {
            auto it = clkIdx.find(d.clock);
            if (!d.input || it == clkIdx.end()) continue;
            for (auto& p : d.ports)
                if (const Port* pp = rtl->port(p.name)) {
                    launch[size_t(pp->node) * W + it->second / 64] |= 1ull << (it->second % 64);
                    inSeed[pp->node] = 1;
                    work.push_back(pp->node);
                }
        }
        while (!work.empty()) {
            int u = work.back();
            work.pop_back();
            for (int e = rtl->fwdAt[u]; e < rtl->fwdAt[u + 1]; e++) {
                int v = rtl->fwd[e];
                if (rtl->nodeReg[v] >= 0) continue;
                bool ch = false;
                for (int w = 0; w < W; w++) {
                    uint64_t nv = launch[size_t(v) * W + w] | launch[size_t(u) * W + w];
                    if (nv != launch[size_t(v) * W + w]) { launch[size_t(v) * W + w] = nv; ch = true; }
                }
                if (ch) work.push_back(v);
            }
        }
        // Capture points: every sequential block, and every output port with
        // an output delay.
        struct Cap { std::vector<int> reads; Mask clocks; std::string name; std::vector<int> regs; int port = -1; };
        std::vector<Cap> caps;
        for (auto& s : rtl->seqs) {
            Cap cp;
            cp.reads = s.reads;
            cp.clocks.assign(at(reach, s.clk), at(reach, s.clk) + W);
            cp.regs = s.regs;
            if (!s.regs.empty()) cp.name = rtl->regs[s.regs[0]].name;
            caps.push_back(std::move(cp));
        }
        std::map<int, Mask> outClocks;
        for (auto& d : c.ioDelays) {
            auto it = clkIdx.find(d.clock);
            if (d.input || it == clkIdx.end()) continue;
            for (auto& p : d.ports)
                if (const Port* pp = rtl->port(p.name)) {
                    auto& m = outClocks[int(pp - rtl->ports.data())];
                    m.resize(W, 0);
                    m[it->second / 64] |= 1ull << (it->second % 64);
                }
        }
        for (auto& [pi, m] : outClocks) {
            Cap cp;
            cp.reads = {rtl->ports[pi].node};
            cp.clocks = m;
            cp.name = rtl->ports[pi].name;
            cp.port = pi;
            caps.push_back(std::move(cp));
        }
        // The clock pairs that need a look: not the same clock, not derived
        // from one source, not declared.
        size_t nc = clk.size();
        std::vector<char> open(nc * nc, 0), sawPair(nc * nc, 0);
        std::vector<char> capOpen(caps.size(), 0);
        for (size_t ci = 0; ci < caps.size(); ci++) {
            Mask l(W, 0);
            for (int rdn : caps[ci].reads)
                for (int w = 0; w < W; w++) l[w] |= at(launch, rdn)[w];
            for (int a : list(l.data()))
                for (int b : list(caps[ci].clocks.data())) {
                    if (a == b) continue;
                    sawPair[a * nc + b] = 1;
                    if (related(a, b) || grouped(a, b) || clockLevelException(a, b)) continue;
                    open[a * nc + b] = 1;
                    capOpen[ci] = 1;
                }
        }
        // Declared unrelated, yet one is made from the other.
        for (size_t a = 0; a < nc; a++)
            for (size_t b = a + 1; b < nc; b++)
                if (group_[a][b] == 1 && related((int)a, (int)b) && (sawPair[a * nc + b] || sawPair[b * nc + a])) {
                    const sdc::ClockGroups* g = nullptr;
                    for (auto& cg : c.clockGroups) {
                        bool ha = false, hb = false;
                        for (auto& grp : cg.groups)
                            for (auto& n : grp) { ha |= n == clk[a]->name; hb |= n == clk[b]->name; }
                        if ((ha || hb) && cg.type == sdc::ClockGroups::Asynchronous) g = &cg;
                    }
                    rep.add("SDC041", g ? g->loc : clk[b]->loc,
                            "clocks '" + clk[a]->name + "' and '" + clk[b]->name + "' are declared asynchronous, but one derives from the other and the design has paths between them; those paths are synchronous and are now not timed");
                }
        // For the open pairs, find the paths: from each capture point back
        // through the logic to the registers and ports that launch.
        // Exceptions by endpoint, so a path is checked against the few that
        // can cover it and not against all of them.
        std::unordered_map<int, std::vector<int>> exTo;  // register index or ~port -> exceptions
        std::vector<int> exToWide;                       // -to a clock, or no -to
        for (size_t i = 0; i < c.exceptions.size(); i++) {
            const sdc::Exception& e = c.exceptions[i];
            if (e.type == sdc::Exception::GroupPath || e.type == sdc::Exception::MinDelay || e.emptyEndpoint) continue;
            const ExInfo& x = ex_[i];
            if (x.from.all && x.to.all && x.through.empty()) continue;
            if (x.to.all || any(x.to.clocks.data())) exToWide.push_back((int)i);
            for (int t : x.to.nodes) exTo[t].push_back((int)i);
        }
        struct Hit { int count = 0; std::string from, to; };
        std::vector<std::map<std::pair<int, int>, Hit>> found(caps.size());
        parallelFor(caps.size(), threads, [&](size_t ci) {
            if (!capOpen[ci]) return;
            const Cap& cp = caps[ci];
            static thread_local Marks seen;
            seen.begin(rtl->n);
            std::vector<int> st, srcs;
            for (int rdn : cp.reads)
                if (seen.set(rdn)) st.push_back(rdn);
            while (!st.empty()) {
                int u = st.back();
                st.pop_back();
                if (rtl->nodeReg[u] >= 0 || inSeed[u]) srcs.push_back(u);
                if (rtl->nodeReg[u] >= 0) continue;
                for (int e = rtl->revAt[u]; e < rtl->revAt[u + 1]; e++)
                    if (seen.set(rtl->rev[e])) st.push_back(rtl->rev[e]);
            }
            std::sort(srcs.begin(), srcs.end());
            auto capList = list(cp.clocks.data());
            std::vector<int> cand = exToWide;
            if (cp.port >= 0) {
                if (auto it = exTo.find(~cp.port); it != exTo.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
            }
            for (int rg : cp.regs)
                if (auto it = exTo.find(rg); it != exTo.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
            std::sort(cand.begin(), cand.end());
            cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
            for (int s : srcs) {
                const uint64_t* lm = rtl->nodeReg[s] >= 0 || inSeed[s] ? at(launchSeed_, s) : nullptr;
                if (!lm) continue;
                for (int a : list(lm))
                    for (int b : capList) {
                        if (a == b || !open[a * nc + b]) continue;
                        // An exception on these very points covers the path.
                        bool covered = false;
                        for (int i : cand) {
                            const ExInfo& x = ex_[i];
                            bool f = x.from.all || x.fromSet.count(s) || bit(x.from.clocks.data(), a);
                            if (!f) continue;
                            bool t = x.to.all || bit(x.to.clocks.data(), b);
                            if (!t && cp.port >= 0) t = x.toSet.count(~cp.port) != 0;
                            for (size_t k = 0; k < cp.regs.size() && !t; k++) t = x.toSet.count(cp.regs[k]) != 0;
                            if (t) { covered = true; break; }
                        }
                        if (covered) continue;
                        Hit& h = found[ci][{a, b}];
                        if (h.count++ == 0) {
                            h.from = rtl->nodeReg[s] >= 0 ? rtl->regs[rtl->nodeReg[s]].name : "port " + rtl->nodeName[s];
                            h.to = cp.port >= 0 ? "port " + cp.name : cp.name;
                        }
                    }
            }
        });
        std::map<std::pair<int, int>, Hit> total;
        for (auto& m : found)
            for (auto& [k, h] : m) {
                Hit& t = total[k];
                if (t.count == 0) { t.from = h.from; t.to = h.to; }
                t.count += h.count;
            }
        for (auto& [k, h] : total) {
            const sdc::Clock &a = *clk[k.first], &b = *clk[k.second];
            rep.add("SDC040", b.loc,
                    "clock '" + a.name + "' launches data that clock '" + b.name + "' captures (" + std::to_string(h.count) +
                        (h.count == 1 ? " path" : " paths") + ", such as " + h.from + " to " + h.to +
                        "), and nothing says how the two relate. They are timed as synchronous" +
                        (periodOf(k.first) > 0 && periodOf(k.second) > 0 ? " (periods " + num(periodOf(k.first)) + " and " + num(periodOf(k.second)) + ")" : std::string()) +
                        ". If they are asynchronous, add set_clock_groups -asynchronous or constrain the crossing with set_max_delay or set_false_path");
        }
    }
    std::vector<uint64_t> launchSeed_;  // launch clocks of the startpoints alone

    void seedLaunch() {
        if (!rtl) return;
        launchSeed_.assign(size_t(rtl->n) * W, 0);
        for (auto& r : rtl->regs)
            for (int b : r.blocks)
                for (int w = 0; w < W; w++) launchSeed_[size_t(r.node) * W + w] |= at(reach, rtl->seqs[b].clk)[w];
        for (auto& d : c.ioDelays) {
            auto it = clkIdx.find(d.clock);
            if (!d.input || it == clkIdx.end()) continue;
            for (auto& p : d.ports)
                if (const Port* pp = rtl->port(p.name)) launchSeed_[size_t(pp->node) * W + it->second / 64] |= 1ull << (it->second % 64);
        }
    }

    // ---- ports: delays and the electrical environment
    void checkPorts() {
        if (!rtl) return;
        std::set<std::string> inDelay, outDelay, clockPorts, cased, excFrom, excTo, driven, loaded;
        for (auto& d : c.ioDelays)
            for (auto& p : d.ports) (d.input ? inDelay : outDelay).insert(Rtl::stripIdx(p.name));
        for (auto k : clk)
            for (auto& s : k->sources)
                if (s.kind == sdc::Kind::Port) clockPorts.insert(Rtl::stripIdx(s.name));
        for (auto& ca : c.caseAnalysis)
            for (auto& o : ca.objects) cased.insert(Rtl::stripIdx(o.name));
        for (auto& e : c.exceptions) {
            if (e.type == sdc::Exception::GroupPath || e.type == sdc::Exception::Multicycle) continue;
            for (auto& o : e.path.from) if (o.kind == sdc::Kind::Port) excFrom.insert(Rtl::stripIdx(o.name));
            for (auto& o : e.path.to) if (o.kind == sdc::Kind::Port) excTo.insert(Rtl::stripIdx(o.name));
        }
        for (const char* cmd : {"set_driving_cell", "set_drive", "set_input_transition"})
            for (auto* x : c.of(cmd))
                for (auto& o : x->objs("objects")) driven.insert(Rtl::stripIdx(o.name));
        for (auto* x : c.of("set_load"))
            for (auto& o : x->objs("objects")) loaded.insert(Rtl::stripIdx(o.name));
        // A port with no delay matters when it goes somewhere: one search per port.
        std::vector<char> goes(rtl->ports.size(), 0);
        parallelFor(rtl->ports.size(), threads, [&](size_t i) {
            const Port& p = rtl->ports[i];
            if (p.dir == 1) {  // output: driven by something
                goes[i] = rtl->nodeReg[p.node] >= 0 || rtl->revAt[p.node + 1] > rtl->revAt[p.node];
                return;
            }
            if (inDelay.count(p.name) || clockPorts.count(p.name)) return;
            static thread_local Marks seen;
            seen.begin(rtl->n);
            std::vector<int> st{p.node};
            seen.set(p.node);
            while (!st.empty() && !goes[i]) {
                int u = st.back();
                st.pop_back();
                if (!rtl->readers[u].empty() || rtl->nodePortOut[u] >= 0) { if (u != p.node || !rtl->readers[u].empty()) goes[i] = 1; }
                for (int e = rtl->fwdAt[u]; e < rtl->fwdAt[u + 1]; e++) {
                    int v = rtl->fwd[e];
                    if (rtl->nodeReg[v] < 0 && seen.set(v)) st.push_back(v);
                }
            }
        });
        std::vector<std::string> noDrive, noLoad;
        for (size_t i = 0; i < rtl->ports.size(); i++) {
            const Port& p = rtl->ports[i];
            bool isClock = clockPorts.count(p.name) || any(at(reach, p.node));
            if (p.dir != 1 && !isClock) {
                if (!inDelay.count(p.name) && !cased.count(p.name) && !excFrom.count(p.name) && goes[i])
                    rep.add("SDC020", p.file, p.line, "input port '" + p.name + "' has no set_input_delay, so paths from it are not timed (or name it in a false path, or set_case_analysis if it is static)");
                if (!driven.count(p.name) && !cased.count(p.name)) noDrive.push_back(p.name);
            }
            if (p.dir != 0) {
                if (!outDelay.count(p.name) && !excTo.count(p.name) && (p.dir == 1 ? goes[i] : true) && !isClock)
                    rep.add("SDC021", p.file, p.line, "output port '" + p.name + "' has no set_output_delay, so paths to it are not timed");
                if (!loaded.count(p.name)) noLoad.push_back(p.name);
            }
        }
        auto some = [](const std::vector<std::string>& v) {
            std::string s;
            for (size_t i = 0; i < v.size() && i < 6; i++) s += (i ? ", " : "") + v[i];
            if (v.size() > 6) s += ", ...";
            return s;
        };
        const std::string& f = c.commands.empty() ? std::string() : c.commands[0].loc.file;
        if (!noDrive.empty())
            rep.add("SDC060", f, 0, std::to_string(noDrive.size()) + (noDrive.size() == 1 ? " input port has" : " input ports have") +
                                        " no set_driving_cell, set_drive or set_input_transition (" + some(noDrive) + "); they are driven by an ideal source with zero transition time");
        if (!noLoad.empty())
            rep.add("SDC061", f, 0, std::to_string(noLoad.size()) + (noLoad.size() == 1 ? " output port has" : " output ports have") +
                                        " no set_load (" + some(noLoad) + "); they drive no capacitance, so their last stage looks faster than it is");
    }

    void checkCase() {
        std::map<std::string, const sdc::CaseAnalysis*> seen;
        for (auto& ca : c.caseAnalysis) {
            static const std::set<std::string> ok{"0", "1", "zero", "one", "rising", "falling", "rise", "fall"};
            if (!ok.count(ca.value))
                rep.add("SDC050", ca.loc, "set_case_analysis value '" + ca.value + "' is not one of 0, 1, rising, falling", true);
            auto norm = [](const std::string& v) { return v == "zero" ? std::string("0") : v == "one" ? std::string("1") : v; };
            for (auto& o : ca.objects) {
                auto [it, fresh] = seen.try_emplace(std::string(sdc::kindName(o.kind)) + ":" + o.name, &ca);
                if (!fresh && norm(it->second->value) != norm(ca.value))
                    rep.add("SDC050", ca.loc, "set_case_analysis " + ca.value + " on " + o.name + " contradicts the value " + it->second->value + " set at " + it->second->loc.str());
                if (!fresh) it->second = &ca;
                if (rtl && o.kind == sdc::Kind::Port && rtl->portDir(o.name) == 1)
                    rep.add("SDC050", ca.loc, "set_case_analysis on '" + o.name + "', an output port; a constant is set where a value enters the design");
            }
        }
    }

    template <typename Lap>
    void run(Lap* lap) {
        auto mark = [&](const char* w) { if (lap) (*lap)(w); };
        checkDiags();
        setupClocks();
        setupGroups();
        mark("  clock propagation");
        checkClocks();
        checkIo();
        mark("  clocks and I/O");
        checkExceptions();
        mark("  exceptions");
        seedLaunch();
        checkCrossings();
        mark("  clock crossings");
        checkPorts();
        checkCase();
        mark("  ports");
    }

    // What the constraints cover, for the summary and -json.
    void summary(std::ostream& os) const {
        os << "msdc: " << clk.size() << (clk.size() == 1 ? " clock" : " clocks");
        size_t gen = 0, virt = 0;
        for (auto k : clk) { gen += k->generated; virt += k->isVirtual(); }
        if (gen || virt) os << " (" << gen << " generated, " << virt << " virtual)";
        os << ", " << c.ioDelays.size() << " I/O delays, " << c.exceptions.size() << " exceptions, " << c.clockGroups.size() << " clock groups\n";
        if (!rtl) return;
        size_t bits = 0, clocked = 0;
        for (auto& r : rtl->regs) {
            bits += r.width;
            bool ok = false;
            for (int b : r.blocks) ok |= any(at(reach, rtl->seqs[b].clk));
            if (ok) clocked += r.width;
        }
        os << "msdc: " << clocked << " of " << bits << " register bits are clocked";
        for (size_t i = 0; i < clk.size(); i++) {
            size_t n = 0;
            for (auto& r : rtl->regs) {
                bool ok = false;
                for (int b : r.blocks) ok |= bit(at(reach, rtl->seqs[b].clk), (int)i);
                if (ok) n += r.width;
            }
            if (n) os << (i == 0 || true ? "; " : "") << clk[i]->name << " " << n;
        }
        os << "\n";
    }
};

void usage() {
    std::cout << "usage: msdc [options] design.sv ... -sdc file.sdc\n"
                 "  checks timing constraints (SDC) against the design, on the msim front end\n\n"
                 "  -sdc <file>    the constraints (repeatable; read in order)\n"
                 "  -top M         the top module    -f <file>  an argument file\n"
                 "  -threads N     worker threads (default: every logical core; 1 turns threading off)\n"
                 "  -disable RULE  turn one rule off (repeatable)\n"
                 "  -list          list the rules\n"
                 "  -Werror        exit non-zero on any finding (the default is errors only)\n"
                 "  -quiet         print findings only, no summary\n"
                 "  -stats         print the size of the design and the time of each step\n"
                 "  -json <file>   also write the findings as JSON\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version       print the version\n"
                 "  --help-slang   list every front-end option\n\n"
                 "  With no design file, the SDC file is checked on its own.\n";
}

std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (ch == '\n') o += "\\n";
        else if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += ch;
    }
    return o + "\"";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    Rep rep;
    bool werror = false, quiet = false, haveDesign = false, stats = false;
    std::vector<std::string> sdcFiles;
    std::string jsonPath;
    unsigned threads = par::defaultThreads();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "msdc: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-list" || a == "--list") {
            for (auto& r : kRules) std::cout << r.id << "  " << r.text << "\n";
            return 0;
        }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "msdc " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-disable") { rep.disabled.insert(next("a rule name")); continue; }
        if (a == "-Werror") { werror = true; continue; }
        if (a == "-sdc") { sdcFiles.push_back(next("an SDC file")); continue; }
        if (a == "-json") { jsonPath = next("a file name"); continue; }
        if (par::threadsArg(argc, argv, i, threads, "msdc")) continue;
        if (a == "-quiet" || a == "-q") { quiet = true; continue; }
        if (a == "-stats") { stats = true; continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); haveDesign = true; continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        if (a == "-f" || a == "-F" || (a[0] != '-' && a[0] != '+')) haveDesign = true;
        if (a.size() > 4 && a.compare(a.size() - 4, 4, ".sdc") == 0 && a[0] != '-') { sdcFiles.push_back(a); continue; }
        fwd.push_back(a);
    }
    if (sdcFiles.empty()) {
        std::cerr << "msdc: no SDC file (use -sdc file.sdc)\n";
        return 2;
    }
    haveDesign = false;
    for (size_t i = 1; i < fwd.size(); i++)
        if (fwd[i] == "-f" || fwd[i] == "-F" || fwd[i] == "--help" || (fwd[i][0] != '-' && fwd[i][0] != '+' && (i == 1 || fwd[i - 1] != "--top"))) haveDesign = true;

    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<const char*, double>> phases;
    auto lap = [&](const char* what) {
        auto t1 = std::chrono::steady_clock::now();
        phases.push_back({what, std::chrono::duration<double>(t1 - t0).count()});
        t0 = t1;
    };
    FrontEnd fe;
    Rtl rtl;
    sdc::Reader reader;
    if (haveDesign) {
        if (int rc = fe.run(fwd, "msdc front end")) return rc < 0 ? 0 : rc;
        lap("parse and elaborate");
        Builder b(rtl, *fe.comp);
        std::string err;
        if (stats) b.lap = lap;
        if (!b.build(err)) {
            std::cerr << "msdc: " << err << "\n";
            return 1;
        }
        lap("build the graph");
        reader.design = &rtl;
        rtl.reader = &reader;
        rtl.clockMasks = [&](const sdc::Constraints& cs, int& words) {
            std::vector<const sdc::Clock*> clk;
            std::unordered_map<std::string, int> idx;
            for (auto& k : cs.clocks) {
                auto it = idx.find(k.name);
                if (it == idx.end()) { idx[k.name] = (int)clk.size(); clk.push_back(&k); }
                else clk[it->second] = &k;
            }
            words = std::max(1, ((int)clk.size() + 63) / 64);
            Rep none;
            Checker tmp(reader, &rtl, none, 1);
            return Checker::propagate(rtl, clk, words, [&](const sdc::Obj& o) { return tmp.nodeOfSource(o); });
        };
    }
    reader.threads = threads;
    reader.onPuts = [](const std::string& t) { std::cout << t << "\n"; };
    for (auto& f : sdcFiles) {
        try {
            reader.load(f);
        } catch (const std::exception& ex) {
            std::cerr << "msdc: " << ex.what() << "\n";
            return 2;
        }
    }

    lap("read sdc");
    Checker ck(reader, haveDesign ? &rtl : nullptr, rep, threads);
    ck.run(stats ? &lap : nullptr);
    lap("check");

    std::sort(rep.out.begin(), rep.out.end());
    rep.out.erase(std::unique(rep.out.begin(), rep.out.end()), rep.out.end());
    size_t errors = 0;
    for (const Out& f : rep.out) {
        errors += f.error;
        std::cout << (f.file.empty() ? std::string("msdc") : f.file);
        if (f.line) std::cout << ":" << f.line;
        std::cout << ": " << (f.error ? "error" : "warning") << ": [" << f.rule << "] " << f.text << "\n";
    }
    if (!quiet) {
        ck.summary(std::cout);
        std::map<std::string, int> per;
        for (const Out& f : rep.out) per[f.rule]++;
        std::cout << "msdc: " << rep.out.size() << (rep.out.size() == 1 ? " finding" : " findings");
        bool first = true;
        for (auto& [r, n] : per) {
            std::cout << (first ? " (" : ", ") << r << " " << n;
            first = false;
        }
        std::cout << (first ? "" : ")") << "\n";
    }
    if (stats) {
        std::cerr << "msdc: " << threads << " threads";
        if (haveDesign) std::cerr << ", " << rtl.n << " signals, " << rtl.fwd.size() << " logic edges, " << rtl.regs.size() << " registers, " << rtl.seqs.size() << " clocked blocks";
        std::cerr << "\n";
        for (auto& [w, t] : phases) {
            char b[96];
            snprintf(b, sizeof b, "msdc: %-22s %8.3f s\n", w, t);
            std::cerr << b;
        }
    }
    if (!jsonPath.empty()) {
        std::ofstream js(jsonPath);
        js << "{\n  \"tool\": \"msdc\",\n  \"clocks\": [";
        for (size_t i = 0; i < ck.clk.size(); i++)
            js << (i ? ", " : "") << "{\"name\": " << jsonStr(ck.clk[i]->name) << ", \"period\": " << ck.clk[i]->period
               << ", \"generated\": " << (ck.clk[i]->generated ? "true" : "false") << ", \"virtual\": " << (ck.clk[i]->isVirtual() ? "true" : "false") << "}";
        js << "],\n  \"findings\": [\n";
        for (size_t i = 0; i < rep.out.size(); i++) {
            const Out& f = rep.out[i];
            js << "    {\"rule\": " << jsonStr(f.rule) << ", \"severity\": " << (f.error ? "\"error\"" : "\"warning\"") << ", \"file\": " << jsonStr(f.file)
               << ", \"line\": " << f.line << ", \"text\": " << jsonStr(f.text) << "}" << (i + 1 < rep.out.size() ? "," : "") << "\n";
        }
        js << "  ]\n}\n";
    }
    return errors || (werror && !rep.out.empty()) ? 1 : 0;
}
