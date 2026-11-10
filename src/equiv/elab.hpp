#pragma once
// elab.hpp - turn an elaborated slang design into gates.
//
// mequiv reads both sides of a comparison the same way: RTL, a structural
// netlist with its Verilog cell models, or a mix. The hierarchy is flattened
// and every signal becomes a vector of AIG literals (bv.hpp). The result is a
// Design: named primary inputs, named outputs, and state elements (flops and
// latches), each with a next-state function and a trigger.
//
// How logic is found
//   Every continuous assignment, port connection, gate primitive and always
//   block is a driver of some bits. Signals are evaluated on demand: asking
//   for a bit runs the driver that claims it, which asks for the bits it
//   reads. So no ordering pass is needed and a design is only as "sorted" as
//   its own dependencies.
//   An always block is executed symbolically: assignments under an if/case
//   become muxes on the path condition, loops are unrolled, functions are
//   inlined.
//   - Edge-triggered block: every bit it assigns is a flop. Its output is a
//     fresh AIG input (Q); the executed body gives the next-state function,
//     which includes any asynchronous set/reset as written (if (!rst_n) ...).
//     The trigger is the OR of the event list's active levels (clk | !rst_n),
//     so two flops agree only if clock, edge and async controls all agree.
//   - Level-sensitive block: a bit read before it is assigned, or not assigned
//     on every path, holds its value: a latch. That is decided by proof (does
//     the result depend on the held value?), not by syntax, so a full case
//     with no default is plain logic.
//
// This elaborator is deliberately independent of the synthesizer's
// (src/synth): an equivalence checker that read RTL through the code it is
// checking could not see that code's mistakes.
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/EvalContext.h"
#include "slang/ast/Expression.h"
#include "slang/ast/Statement.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/AssignmentExpressions.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/ConditionalStatements.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/statements/MiscStatements.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/text/SourceManager.h"

#include "aig.hpp"
#include "bv.hpp"
#include "cnf.hpp"

namespace equiv {

using namespace slang;
using namespace slang::ast;

struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct State {
    std::string name;  // hierarchical bit name, e.g. u_core.cnt[3]
    std::string inst;  // path of the instance that holds it ("" at the top)
    std::string cell;  // that instance's module name
    std::vector<std::string> aliases;     // other signals that carry Q unchanged (the net on a flop cell's Q pin)
    std::vector<std::string> invAliases;  // signals that carry !Q
    Lit q = kFalse;    // the AIG input standing for the stored value
    Bit next;          // value stored at the next trigger
    Bit trig;          // flop: OR of the event list's active levels. latch: unused
    bool latch = false;
};

struct Design {
    std::string top;
    std::vector<std::pair<std::string, Lit>> inputs;
    std::vector<std::pair<std::string, Bit>> outputs;
    std::vector<State> states;
    std::vector<std::string> warnings;
    std::vector<std::pair<Lit, Lit>> lemmas;  // literals equal by construction (bv.hpp)
};

struct ElabOptions {
    std::set<std::string> blackboxes;  // module names not to look inside
    size_t maxLoop = 1u << 20;         // iterations before a loop is called unbounded
};

class Elab {
public:
    Elab(Aig& g, Compilation& comp, Design& out, const ElabOptions& opt) :
        g(g), bv(g), comp(comp), d(out), opt(opt),
        ectx(ASTContext(comp.getRoot(), LookupLocation::max)) {}

    void run(const InstanceSymbol& top) {
        d.top = std::string(top.name);
        bv.lemmas = &d.lemmas;
        topPrefix = d.top + ".";
        Proc global;
        global.kind = Proc::Global;
        cur = &global;

        // Top-level ports.
        std::vector<const PortSymbol*> outs;
        for (const Symbol* ps : top.body.getPortList()) {
            if (ps->kind != SymbolKind::Port)
                throw Unsupported("port '" + std::string(ps->name) + "' of the top module is not a plain port");
            auto& p = ps->as<PortSymbol>();
            if (!p.internalSymbol)
                throw Unsupported("top port '" + std::string(p.name) + "' has no internal signal");
            if (p.direction == ArgumentDirection::Out) { outs.push_back(&p); continue; }
            if (p.direction != ArgumentDirection::In)
                warn("inout port '" + std::string(p.name) + "' is treated as an input");
            SymInfo& si = info(*p.internalSymbol);
            const auto& names = bitNames(*p.internalSymbol);
            for (uint32_t i = 0; i < si.width; ++i) {
                Lit in = g.input();
                d.inputs.push_back({names[i], in});
                si.val[i] = {in, kFalse};
                si.drv[i] = kPrimary;
            }
        }
        walkScope(top.body);
        for (const PortSymbol* p : outs) {
            const auto& names = bitNames(*p->internalSymbol);
            Bits b = readGlobal(*p->internalSymbol, 0, info(*p->internalSymbol).width);
            for (size_t i = 0; i < b.size(); ++i) d.outputs.push_back({names[i], b[i]});
        }
        for (size_t i = 0; i < drivers.size(); ++i)
            if (drivers[i].kind == Driver::FF) runFF(drivers[i]);
        for (auto& bb : bboxIns) {
            Bits b = evalExpr(*bb.expr);
            for (size_t i = 0; i < b.size(); ++i)
                d.outputs.push_back({bb.names[i], b[i]});
        }
        cur = nullptr;
        resolvePlaceholders();
        findAliases();
    }

private:
    // ----------------------------------------------------------- data model
    static constexpr int32_t kNone = -1, kPrimary = -2;

    struct SymInfo {
        uint32_t width = 0;
        Bits val;                  // kUnset until evaluated
        std::vector<int32_t> drv;  // driver index per bit
        bool warned = false;
    };
    struct Claim {
        const Symbol* sym;
        uint32_t lo, w;
    };
    struct Event {
        EdgeKind edge;
        const Expression* expr;
    };
    struct Driver {
        enum Kind { Assign, PortIn, PortOut, Comb, FF, Prim, BBoxOut } kind;
        const Expression* lhs = nullptr;
        const Expression* rhs = nullptr;
        const Symbol* sym = nullptr;  // PortIn: the port's signal. PortOut: likewise
        const Statement* body = nullptr;
        const PrimitiveInstanceSymbol* prim = nullptr;
        const Symbol* owner = nullptr;  // for messages
        std::vector<Event> events;
        std::vector<std::string> names;  // BBoxOut
        std::unordered_map<const Symbol*, std::vector<bool>> claims;
        std::vector<Claim> claimList;
        int state = 0;  // 0 not run, 1 running, 2 done
        struct Placeholder {
            const Symbol* sym;
            uint32_t bit;
            Bit p;
        };
        std::vector<Placeholder> placeholders;  // bits asked for while running
    };
    // Storage a process writes while it executes. Each if/case branch runs in
    // its own layer on top of the enclosing one; when the branches are done
    // their layers are merged into muxes on the branch condition.
    struct Key {
        const Symbol* sym;
        uint32_t frame;  // 0: the process itself; else one call of a subroutine
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return std::hash<const void*>()(k.sym) * 31 + k.frame; }
    };
    struct Layer {
        std::unordered_map<Key, Bits, KeyHash> env;         // blocking view
        std::unordered_map<const Symbol*, Bits> nba;        // pending non-blocking writes
    };
    struct Flags {
        Bit ret = kB0, brk = kB0, cont = kB0;
    };
    struct Proc {
        enum Kind { Global, Comb, FF } kind = Global;
        Driver* drv = nullptr;
        std::vector<Layer> layers{1};
        std::vector<uint32_t> frames{0};
        uint32_t nextFrame = 1;
        std::map<std::pair<const Symbol*, uint32_t>, Lit> holds;
        Flags f;
    };
    struct BBoxIn {
        const Expression* expr;
        std::vector<std::string> names;
    };

    Aig& g;
    Bv bv;
    Compilation& comp;
    Design& d;
    const ElabOptions& opt;
    EvalContext ectx;
    std::string topPrefix;
    std::unordered_map<const Symbol*, SymInfo> syms;
    std::unordered_map<const Symbol*, std::vector<std::string>> names;
    std::unordered_map<const Symbol*, bool> localCache;
    std::deque<Driver> drivers;
    std::vector<BBoxIn> bboxIns;
    std::unordered_map<uint32_t, Lit> defs;             // placeholder node -> its logic
    std::unordered_map<uint32_t, std::string> defName;  // for the loop message
    Proc* cur = nullptr;
    Bit guard = kB1;
    Bits emptyArg;
    std::vector<const Expression*> lvalStack;
    int callDepth = 0;

    void warn(std::string s) { d.warnings.push_back(std::move(s)); }

    [[noreturn]] void fail(const std::string& what, SourceLocation loc) {
        std::string where;
        if (auto* sm = comp.getSourceManager(); sm && loc) {
            where = std::string(sm->getFileName(loc)) + ":" + std::to_string(sm->getLineNumber(loc)) + ": ";
            if (auto p = where.rfind('/', where.find(':')); p != std::string::npos) where = where.substr(p + 1);
        }
        throw Unsupported(where + what);
    }
    [[noreturn]] void fail(const std::string& what, const Expression& e) { fail(what, e.sourceRange.start()); }
    [[noreturn]] void fail(const std::string& what, const Statement& s) { fail(what, s.sourceRange.start()); }

    std::string pathOf(const Symbol& s) {
        std::string p = s.getHierarchicalPath();
        if (p.starts_with(topPrefix)) p.erase(0, topPrefix.size());
        return p;
    }

    static uint32_t widthOf(const Type& t) { return uint32_t(t.getBitstreamWidth()); }

    SymInfo& info(const Symbol& s) {
        auto it = syms.find(&s);
        if (it != syms.end()) return it->second;
        const Type& t = s.as<ValueSymbol>().getType();
        if (!t.isFixedSize() || t.isFloating() || t.isString() || t.isClass() || t.isEvent())
            throw Unsupported("signal '" + pathOf(s) + "' has a type with no fixed bit width (" + t.toString() + ")");
        SymInfo si;
        si.width = widthOf(t);
        si.val.assign(si.width, kUnset);
        si.drv.assign(si.width, kNone);
        return syms.emplace(&s, std::move(si)).first->second;
    }

    // Names of a signal's bits, bit 0 first: a[0], m[2][7], s.field[1].
    void nameBits(const Type& type, const std::string& prefix, std::vector<std::string>& out) {
        const Type& t = type.getCanonicalType();
        if (t.kind == SymbolKind::PackedArrayType || t.kind == SymbolKind::FixedSizeUnpackedArrayType) {
            ConstantRange r = t.getFixedRange();
            const Type& el = *t.getArrayElementType();
            for (int32_t p = 0; p < int32_t(r.width()); ++p) {
                int32_t idx = r.isDescending() ? r.lower() + p : r.upper() - p;
                nameBits(el, prefix + "[" + std::to_string(idx) + "]", out);
            }
        } else if (t.kind == SymbolKind::PackedStructType || t.kind == SymbolKind::UnpackedStructType) {
            auto fields = fieldsOf(t);
            for (size_t i = fields.size(); i-- > 0;)
                nameBits(fields[i]->getType(), prefix + "." + std::string(fields[i]->name), out);
        } else {
            uint32_t w = widthOf(t);
            if (w == 1 && t.isScalar()) out.push_back(prefix);
            else
                for (uint32_t i = 0; i < w; ++i) out.push_back(prefix + "[" + std::to_string(i) + "]");
        }
    }
    static std::vector<const FieldSymbol*> fieldsOf(const Type& t) {
        std::vector<const FieldSymbol*> fields;
        const Scope* sc = t.kind == SymbolKind::PackedStructType ? static_cast<const Scope*>(&t.as<PackedStructType>())
                        : t.kind == SymbolKind::UnpackedStructType ? static_cast<const Scope*>(&t.as<UnpackedStructType>())
                        : nullptr;
        if (sc)
            for (auto& f : sc->membersOfType<FieldSymbol>()) fields.push_back(&f);
        return fields;
    }
    const std::vector<std::string>& bitNames(const Symbol& s) {
        auto it = names.find(&s);
        if (it != names.end()) return it->second;
        std::vector<std::string> v;
        nameBits(s.as<ValueSymbol>().getType(), pathOf(s), v);
        v.resize(info(s).width);
        return names.emplace(&s, std::move(v)).first->second;
    }

    // A variable that lives in a subroutine or is automatic: per-call storage.
    bool isLocal(const Symbol& s) {
        auto it = localCache.find(&s);
        if (it != localCache.end()) return it->second;
        bool local = false;
        if (s.kind == SymbolKind::FormalArgument) local = true;
        else if (s.kind == SymbolKind::Variable && s.as<VariableSymbol>().lifetime == VariableLifetime::Automatic)
            local = true;
        else
            for (const Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
                SymbolKind k = sc->asSymbol().kind;
                if (k == SymbolKind::Subroutine) { local = true; break; }
                if (k != SymbolKind::StatementBlock) break;
            }
        return localCache[&s] = local;
    }

    // --------------------------------------------------------- collection
    void walkScope(const Scope& scope) {
        for (const Symbol& m : scope.members()) {
            switch (m.kind) {
                case SymbolKind::Instance: walkInstance(m.as<InstanceSymbol>()); break;
                case SymbolKind::InstanceArray: walkScope(m.as<InstanceArraySymbol>()); break;
                case SymbolKind::GenerateBlock:
                    if (!m.as<GenerateBlockSymbol>().isUninstantiated) walkScope(m.as<GenerateBlockSymbol>());
                    break;
                case SymbolKind::GenerateBlockArray:
                    for (auto* e : m.as<GenerateBlockArraySymbol>().entries)
                        if (!e->isUninstantiated) walkScope(*e);
                    break;
                case SymbolKind::ContinuousAssign: {
                    auto& a = m.as<ContinuousAssignSymbol>().getAssignment().as<AssignmentExpression>();
                    Driver dr;
                    dr.kind = Driver::Assign;
                    dr.lhs = &a.left();
                    dr.rhs = &a.right();
                    dr.owner = &m;
                    addDriver(std::move(dr), {dr.lhs});
                    break;
                }
                case SymbolKind::Net: {
                    auto& n = m.as<NetSymbol>();
                    if (const Expression* init = n.getInitializer()) {
                        Driver dr;
                        dr.kind = Driver::PortIn;
                        dr.sym = &n;
                        dr.rhs = init;
                        dr.owner = &m;
                        claimWhole(dr, n);
                        pushDriver(std::move(dr));
                    }
                    break;
                }
                case SymbolKind::ProceduralBlock: walkProc(m.as<ProceduralBlockSymbol>()); break;
                case SymbolKind::PrimitiveInstance: walkPrim(m.as<PrimitiveInstanceSymbol>()); break;
                case SymbolKind::UninstantiatedDef:
                    throw Unsupported("module '" + std::string(m.as<UninstantiatedDefSymbol>().definitionName) +
                                      "' (instance '" + pathOf(m) + "') has no definition; add its source or a cell library");
                default: break;
            }
        }
    }

    void walkInstance(const InstanceSymbol& inst) {
        if (inst.isInterface()) throw Unsupported("interface instance '" + pathOf(inst) + "'");
        bool bbox = opt.blackboxes.count(std::string(inst.getDefinition().name)) > 0;
        for (const PortConnection* pc : inst.getPortConnections()) {
            if (pc->port.kind != SymbolKind::Port)
                throw Unsupported("port '" + std::string(pc->port.name) + "' of instance '" + pathOf(inst) +
                                  "' is an interface or multi-port");
            auto& port = pc->port.as<PortSymbol>();
            const Expression* e = pc->getExpression();
            if (!e) continue;  // unconnected: an input reads X, an output goes nowhere
            if (!port.internalSymbol)
                throw Unsupported("port '" + std::string(port.name) + "' of '" + pathOf(inst) + "' has no internal signal");
            const Symbol& in = *port.internalSymbol;
            if (bbox) {
                std::vector<std::string> nm;
                nameBits(port.getType(), pathOf(inst) + "." + std::string(port.name), nm);
                if (port.direction == ArgumentDirection::In) {
                    bboxIns.push_back({e, std::move(nm)});
                } else if (port.direction == ArgumentDirection::Out) {
                    Driver dr;
                    dr.kind = Driver::BBoxOut;
                    dr.lhs = e->kind == ExpressionKind::Assignment ? &e->as<AssignmentExpression>().left() : e;
                    dr.names = std::move(nm);
                    dr.owner = &inst;
                    addDriver(std::move(dr), {dr.lhs});
                } else {
                    throw Unsupported("inout port on black box '" + pathOf(inst) + "'");
                }
                continue;
            }
            Driver dr;
            dr.owner = &inst;
            dr.sym = &in;
            if (port.direction == ArgumentDirection::In) {
                dr.kind = Driver::PortIn;
                dr.rhs = e;
                claimWhole(dr, in);
                pushDriver(std::move(dr));
            } else if (port.direction == ArgumentDirection::Out) {
                dr.kind = Driver::PortOut;
                if (e->kind == ExpressionKind::Assignment) {
                    dr.lhs = &e->as<AssignmentExpression>().left();
                    dr.rhs = &e->as<AssignmentExpression>().right();
                } else {
                    dr.lhs = e;
                }
                addDriver(std::move(dr), {dr.lhs});
            } else {
                throw Unsupported("inout port '" + std::string(port.name) + "' on instance '" + pathOf(inst) + "'");
            }
        }
        if (!bbox) walkScope(inst.body);
    }

    void walkProc(const ProceduralBlockSymbol& p) {
        if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final) return;
        const Statement* body = &p.getBody();
        Driver dr;
        dr.owner = &p;
        dr.kind = Driver::Comb;
        if (body->kind == StatementKind::Timed) {
            auto& t = body->as<TimedStatement>();
            body = &t.stmt;
            std::vector<const TimingControl*> list;
            if (t.timing.kind == TimingControlKind::EventList)
                for (auto* ev : t.timing.as<EventListControl>().events) list.push_back(ev);
            else
                list.push_back(&t.timing);
            bool anyEdge = false, anyLevel = false;
            for (auto* tc : list) {
                if (tc->kind == TimingControlKind::ImplicitEvent) { anyLevel = true; continue; }
                if (tc->kind != TimingControlKind::SignalEvent)
                    fail("always block with a delay or unusual timing control", p.location);
                auto& se = tc->as<SignalEventControl>();
                if (se.iffCondition) fail("'iff' in an event control", p.location);
                if (se.edge == EdgeKind::PosEdge || se.edge == EdgeKind::NegEdge) {
                    anyEdge = true;
                    dr.events.push_back({se.edge, &se.expr});
                } else if (se.edge == EdgeKind::BothEdges) {
                    fail("flop on both edges of one signal", p.location);
                } else {
                    anyLevel = true;
                }
            }
            if (anyEdge && anyLevel) fail("event list mixes edges and levels", p.location);
            if (anyEdge) dr.kind = Driver::FF;
        } else if (p.procedureKind == ProceduralBlockKind::Always) {
            fail("always block with no event control", p.location);
        }
        dr.body = body;
        std::vector<Claim> cl;
        collectClaims(*body, cl);
        for (auto& c : cl) claim(dr, c);
        pushDriver(std::move(dr));
    }

    void walkPrim(const PrimitiveInstanceSymbol& p) {
        Driver dr;
        dr.kind = Driver::Prim;
        dr.prim = &p;
        dr.owner = &p;
        auto conns = p.getPortConnections();
        std::vector<const Expression*> outs;
        auto kind = p.primitiveType.primitiveKind;
        if (kind == PrimitiveSymbol::NOutput) {
            for (size_t i = 0; i + 1 < conns.size(); ++i) outs.push_back(conns[i]);
        } else if (kind == PrimitiveSymbol::BiDiSwitch) {
            throw Unsupported("bidirectional switch primitive '" + pathOf(p) + "'");
        } else if (!conns.empty()) {
            outs.push_back(conns[0]);
        }
        for (auto*& o : outs)
            if (o->kind == ExpressionKind::Assignment) o = &o->as<AssignmentExpression>().left();
        addDriver(std::move(dr), outs);
    }

    void claim(Driver& dr, Claim c) {
        if (c.w == 0) return;
        auto& v = dr.claims[c.sym];
        if (v.empty()) v.assign(info(*c.sym).width, false);
        bool fresh = false;
        for (uint32_t i = c.lo; i < c.lo + c.w && i < v.size(); ++i)
            if (!v[i]) v[i] = fresh = true;
        if (fresh) dr.claimList.push_back(c);
    }
    void claimWhole(Driver& dr, const Symbol& s) { claim(dr, {&s, 0, info(s).width}); }

    void addDriver(Driver dr, std::vector<const Expression*> lvalues) {
        std::vector<Claim> cl;
        for (auto* e : lvalues) claimLv(*e, cl);
        for (auto& c : cl) claim(dr, c);
        pushDriver(std::move(dr));
    }

    void pushDriver(Driver dr) {
        int32_t id = int32_t(drivers.size());
        drivers.push_back(std::move(dr));
        Driver& D = drivers.back();
        std::vector<const Symbol*> shared;
        for (auto& [sym, mask] : D.claims) {
            if (isLocal(*sym)) { shared.push_back(sym); continue; }
            SymInfo& si = info(*sym);
            for (uint32_t i = 0; i < si.width; ++i) {
                if (!mask[i]) continue;
                if (si.drv[i] != kNone) {
                    if (sym->kind == SymbolKind::Variable && sym->as<VariableSymbol>().getType().isPredefinedInteger()) {
                        shared.push_back(sym);  // a loop counter two blocks share: private to each
                        break;
                    }
                    throw Unsupported("'" + bitNames(*sym)[i] + "' has more than one driver");
                }
                si.drv[i] = id;
                if (D.kind == Driver::FF) si.val[i] = {g.input(), kFalse};
            }
        }
        for (const Symbol* sym : shared) {
            localCache[sym] = true;
            SymInfo& si = info(*sym);
            si.drv.assign(si.width, kNone);
            si.val.assign(si.width, kUnset);
            for (Driver& o : drivers) {
                o.claims.erase(sym);
                std::erase_if(o.claimList, [&](const Claim& c) { return c.sym == sym; });
            }
        }
    }

    // ------------------------------------------------------------- lvalues
    // One level of selection: which elements of the parent it picks.
    struct Level {
        const Expression* parent = nullptr;
        uint32_t ew = 1;    // element width in bits
        uint32_t cnt = 1;   // elements selected
        uint32_t n = 1;     // elements in the parent
        bool isConst = false;
        int64_t pos = 0;    // first selected element, counted from the LSB end
        Bits dyn;           // otherwise: the position as a signed vector
    };

    std::optional<int64_t> constInt(const Expression& e, bool symbolic) {
        if (symbolic) {
            Bits b = evalExpr(e);
            uint64_t v;
            if (b.size() > 64 || !Bv::constValue(b, v)) return std::nullopt;
            if (e.type->isSigned() && !b.empty() && b.size() < 64 && (v >> (b.size() - 1) & 1))
                v |= ~0ull << b.size();
            return int64_t(v);
        }
        ConstantValue cv = e.eval(ectx);
        if (!cv.isInteger() || cv.integer().hasUnknown()) return std::nullopt;
        auto v = cv.integer().as<int64_t>();
        if (!v) return std::nullopt;
        return *v;
    }

    // `symbolic`: indices may be read from the executing process (loop
    // variables are constants there). `wantDyn`: build the position vector
    // when the index is not constant.
    Level level(const Expression& e, bool symbolic, bool wantDyn) {
        Level L;
        if (e.kind == ExpressionKind::MemberAccess) {
            auto& ma = e.as<MemberAccessExpression>();
            L.parent = &ma.value();
            const Type& st = ma.value().type->getCanonicalType();
            if (ma.member.kind != SymbolKind::Field) fail("member access that is not a struct field", e);
            L.n = widthOf(st);
            L.cnt = widthOf(*e.type);
            L.isConst = true;
            if (st.kind == SymbolKind::PackedUnionType || st.kind == SymbolKind::UnpackedUnionType) {
                if (st.kind == SymbolKind::UnpackedUnionType) fail("unpacked union", e);
                L.pos = 0;
                return L;
            }
            uint64_t off = 0;
            bool found = false;
            auto fields = fieldsOf(st);
            for (size_t i = fields.size(); i-- > 0;) {
                if (fields[i] == &ma.member) { found = true; break; }
                off += widthOf(fields[i]->getType());
            }
            if (!found) fail("struct field not found", e);
            L.pos = int64_t(off);
            return L;
        }
        const Expression* value;
        if (e.kind == ExpressionKind::ElementSelect) value = &e.as<ElementSelectExpression>().value();
        else value = &e.as<RangeSelectExpression>().value();
        L.parent = value;
        const Type& vt = value->type->getCanonicalType();
        if (!vt.hasFixedRange()) fail("select on a type with no fixed range (" + vt.toString() + ")", e);
        ConstantRange r = vt.getFixedRange();
        L.n = uint32_t(r.width());
        L.ew = widthOf(vt) / L.n;
        bool desc = r.isDescending();
        // position = desc ? idx - lower : upper - idx, for the lowest selected element
        const Expression* idxExpr;
        int64_t adjust = 0;  // added to the index before translating
        if (e.kind == ExpressionKind::ElementSelect) {
            idxExpr = &e.as<ElementSelectExpression>().selector();
        } else {
            auto& rs = e.as<RangeSelectExpression>();
            L.cnt = widthOf(*e.type) / L.ew;
            if (rs.getSelectionKind() == RangeSelectionKind::Simple) {
                auto l = constInt(rs.left(), symbolic), rr = constInt(rs.right(), symbolic);
                if (!l || !rr) fail("range select bounds are not constant", e);
                int64_t lowIdx = desc ? std::min(*l, *rr) : std::max(*l, *rr);
                L.isConst = true;
                L.pos = desc ? lowIdx - r.lower() : r.upper() - lowIdx;
                return L;
            }
            idxExpr = &rs.left();
            bool up = rs.getSelectionKind() == RangeSelectionKind::IndexedUp;
            // Element of the selection nearest the LSB end.
            if (desc) adjust = up ? 0 : -(int64_t(L.cnt) - 1);
            else adjust = up ? int64_t(L.cnt) - 1 : 0;
        }
        if (auto k = constInt(*idxExpr, symbolic)) {
            L.isConst = true;
            int64_t idx = *k + adjust;
            L.pos = desc ? idx - r.lower() : r.upper() - idx;
            return L;
        }
        if (!wantDyn) return L;
        Bits idx = evalExpr(*idxExpr);
        size_t W = std::max<size_t>(idx.size(), 32) + 2;
        idx = Bv::resize(idx, W, idxExpr->type->isSigned());
        if (desc) L.dyn = bv.sub(idx, Bv::constant(uint64_t(int64_t(r.lower()) - adjust), W));
        else L.dyn = bv.sub(Bv::constant(uint64_t(int64_t(r.upper()) - adjust), W), idx);
        return L;
    }

    static bool isSelect(const Expression& e) {
        return e.kind == ExpressionKind::ElementSelect || e.kind == ExpressionKind::RangeSelect ||
               e.kind == ExpressionKind::MemberAccess;
    }
    static const Symbol* valueSym(const Expression& e) {
        if (e.kind == ExpressionKind::NamedValue) return &e.as<NamedValueExpression>().symbol;
        if (e.kind == ExpressionKind::HierarchicalValue) return &e.as<HierarchicalValueExpression>().symbol;
        return nullptr;
    }
    static bool isSignal(const Symbol& s) {
        return s.kind == SymbolKind::Variable || s.kind == SymbolKind::Net || s.kind == SymbolKind::FormalArgument;
    }

    // The fixed bit range of a signal an expression names, if it has one.
    // `partial`: when an index is not constant, return the enclosing range.
    // An out-of-range constant index gives w == 0.
    bool staticRange(const Expression& e, bool symbolic, bool partial, Claim& out) {
        if (const Symbol* s = valueSym(e)) {
            if (!isSignal(*s)) return false;
            out = {s, 0, widthOf(*e.type)};
            return true;
        }
        if (e.kind == ExpressionKind::Conversion) {
            auto& c = e.as<ConversionExpression>();
            if (widthOf(*c.operand().type) != widthOf(*e.type)) return false;
            return staticRange(c.operand(), symbolic, partial, out);
        }
        if (!isSelect(e)) return false;
        Level L = level(e, symbolic, false);
        Claim p;
        if (!staticRange(*L.parent, symbolic, partial, p)) return false;
        if (!L.isConst) {
            if (!partial) return false;
            out = p;
            return true;
        }
        if (p.w == 0 || L.pos < 0 || uint64_t(L.pos) + L.cnt > L.n) {
            out = {p.sym, 0, 0};
            return true;
        }
        out = {p.sym, p.lo + uint32_t(L.pos) * L.ew, L.cnt * L.ew};
        return true;
    }

    void claimLv(const Expression& e, std::vector<Claim>& out) {
        if (e.kind == ExpressionKind::Concatenation) {
            for (auto* op : e.as<ConcatenationExpression>().operands()) claimLv(*op, out);
            return;
        }
        if (e.kind == ExpressionKind::Assignment) return claimLv(e.as<AssignmentExpression>().left(), out);
        if (e.kind == ExpressionKind::Conversion) return claimLv(e.as<ConversionExpression>().operand(), out);
        if (e.kind == ExpressionKind::EmptyArgument) return;
        Claim c;
        if (!staticRange(e, false, true, c)) fail("cannot tell which signal this assigns", e);
        if (!isLocal(*c.sym)) out.push_back(c);
    }

    struct ClaimVisitor : ASTVisitor<ClaimVisitor, VisitFlags::AllGood> {
        Elab& e;
        std::vector<Claim>& out;
        std::unordered_set<const SubroutineSymbol*> seen;
        ClaimVisitor(Elab& e, std::vector<Claim>& out) : e(e), out(out) {}
        void handle(const AssignmentExpression& a) {
            e.claimLv(a.left(), out);
            this->visitDefault(a);
        }
        void handle(const UnaryExpression& u) {
            if (u.op == UnaryOperator::Preincrement || u.op == UnaryOperator::Predecrement ||
                u.op == UnaryOperator::Postincrement || u.op == UnaryOperator::Postdecrement)
                e.claimLv(u.operand(), out);
            this->visitDefault(u);
        }
        void handle(const CallExpression& c) {
            if (c.subroutine.index() == 0) {
                const SubroutineSymbol* sub = std::get<0>(c.subroutine);
                auto formals = sub->getArguments();
                auto args = c.arguments();
                for (size_t i = 0; i < args.size() && i < formals.size(); ++i)
                    if (formals[i]->direction != ArgumentDirection::In) e.claimLv(*args[i], out);
                if (seen.insert(sub).second) sub->getBody().visit(*this);
            }
            this->visitDefault(c);
        }
    };
    void collectClaims(const Statement& s, std::vector<Claim>& out) {
        ClaimVisitor v(*this, out);
        s.visit(v);
    }

    // ------------------------------------------------------- signal values
    Bit getBit(const Symbol& s, uint32_t i) {
        SymInfo* si = &info(s);
        if (isSet(si->val[i])) return si->val[i];
        int32_t id = si->drv[i];
        if (id >= 0) {
            Driver& D = drivers[id];
            if (D.state == 1) {
                // The driver of this bit is running and (through some other
                // logic) asks for its own output. Usually that is one bit of
                // a bus or one output of a block feeding another: hand out a
                // placeholder and tie it to the real value when the driver is
                // done. A true loop is found when placeholders are resolved.
                Bit p{g.input(), g.input()};
                D.placeholders.push_back({&s, i, p});
                return si->val[i] = p;
            }
            runDriver(D);
            si = &info(s);
            if (!isSet(si->val[i])) si->val[i] = kBX;
            return si->val[i];
        }
        // Undriven.
        Bit v = kBX;
        bool known = false;
        if (s.kind == SymbolKind::Net) {
            auto k = s.as<NetSymbol>().netType.netKind;
            if (k == NetType::Supply0 || k == NetType::Tri0) v = kB0, known = true;
            if (k == NetType::Supply1 || k == NetType::Tri1) v = kB1, known = true;
        } else if (s.kind == SymbolKind::Variable) {
            if (const Expression* init = s.as<VariableSymbol>().getInitializer()) {
                ConstantValue cv = init->eval(ectx);
                if (cv) {
                    Bits b = fromConst(cv, s.as<ValueSymbol>().getType());
                    si = &info(s);
                    for (uint32_t k = 0; k < si->width; ++k)
                        if (si->drv[k] == kNone && !isSet(si->val[k])) si->val[k] = b[k];
                    return si->val[i];
                }
            }
        }
        if (!known && !si->warned) {
            si->warned = true;
            warn("'" + pathOf(s) + "' is not driven; it reads as X");
        }
        return si->val[i] = v;
    }
    Bits readGlobal(const Symbol& s, uint32_t lo, uint32_t w) {
        Bits r(w);
        for (uint32_t i = 0; i < w; ++i) r[i] = getBit(s, lo + i);
        return r;
    }

    Lit hold(const Symbol& s, uint32_t i) {
        auto [it, fresh] = cur->holds.try_emplace({&s, i}, kFalse);
        if (fresh) it->second = g.input();
        return it->second;
    }
    bool claimed(const Symbol& s, uint32_t i) {
        if (!cur->drv) return false;
        auto it = cur->drv->claims.find(&s);
        return it != cur->drv->claims.end() && it->second[i];
    }

    Key keyOf(const Symbol& s) { return {&s, isLocal(s) ? cur->frames.back() : 0}; }
    bool findEnv(const Key& k, uint32_t i, Bit& out) {
        for (size_t L = cur->layers.size(); L-- > 0;) {
            auto it = cur->layers[L].env.find(k);
            if (it != cur->layers[L].env.end() && isSet(it->second[i])) { out = it->second[i]; return true; }
        }
        return false;
    }
    bool findNba(const Symbol& s, uint32_t i, Bit& out) {
        for (size_t L = cur->layers.size(); L-- > 0;) {
            auto it = cur->layers[L].nba.find(&s);
            if (it != cur->layers[L].nba.end() && isSet(it->second[i])) { out = it->second[i]; return true; }
        }
        return false;
    }
    Bit readKey(const Key& k, uint32_t i) {
        Bit b;
        if (k.frame || isLocal(*k.sym)) return findEnv(k, i, b) ? b : kBX;
        if (cur->kind != Proc::Global) {
            if (findEnv(k, i, b)) return b;
            if (cur->kind == Proc::Comb && claimed(*k.sym, i)) return {hold(*k.sym, i), kFalse};
        }
        return getBit(*k.sym, i);
    }
    // What a read of bit i of s sees right now in the executing process.
    Bit readBit(const Symbol& s, uint32_t i) { return readKey(keyOf(s), i); }
    // The value a write leaves in place when it does not happen.
    Bit pending(const Symbol& s, uint32_t i, bool nb) {
        if (nb) {
            Bit b;
            if (findNba(s, i, b)) return b;
            if (cur->kind == Proc::FF) return getBit(s, i);
        }
        return readBit(s, i);
    }
    void writeBit(const Symbol& s, uint32_t i, Bit val, Bit gd, bool nb) {
        Key k = keyOf(s);
        if (k.frame || isLocal(s)) {
            if (!isTrue(gd)) val = bv.mux(gd, val, readKey(k, i));
            auto& vec = cur->layers.back().env[k];
            if (vec.empty()) vec.assign(info(s).width, kUnset);
            vec[i] = val;
            return;
        }
        if (cur->kind == Proc::Global) {
            info(s).val[i] = val;
            return;
        }
        if (!claimed(s, i))
            throw Unsupported("'" + bitNames(s)[i] + "' is assigned from a place the block's analysis did not see");
        if (!isTrue(gd)) val = bv.mux(gd, val, pending(s, i, nb));
        auto& vec = nb ? cur->layers.back().nba[&s] : cur->layers.back().env[k];
        if (vec.empty()) vec.assign(info(s).width, kUnset);
        vec[i] = val;
    }

    // Run one branch in a layer of its own and hand the layer back.
    template<typename F>
    Layer branch(Flags& flags, F&& body) {
        Flags saved = cur->f;
        cur->layers.emplace_back();
        body();
        Layer L = std::move(cur->layers.back());
        cur->layers.pop_back();
        flags = cur->f;
        cur->f = saved;
        return L;
    }
    // c ? T : E, bit by bit; a bit only one side wrote keeps, on the other
    // side, what it had before the branch.
    Layer mergeLayers(Bit c, Layer&& T, Layer&& E) {
        Layer out;
        auto one = [&](Bits* t, Bits* e, Bits& o, auto&& below) {
            size_t w = t ? t->size() : e->size();
            o.assign(w, kUnset);
            for (uint32_t i = 0; i < w; ++i) {
                bool ts = t && isSet((*t)[i]), es = e && isSet((*e)[i]);
                if (!ts && !es) continue;
                Bit tv = ts ? (*t)[i] : below(i), ev = es ? (*e)[i] : below(i);
                o[i] = bv.mux(c, tv, ev);
            }
        };
        for (auto& [k, tb] : T.env) {
            if (k.frame && k.frame != cur->frames.back()) continue;  // a finished call's locals
            auto ie = E.env.find(k);
            one(&tb, ie == E.env.end() ? nullptr : &ie->second, out.env[k], [&](uint32_t i) { return readKey(k, i); });
        }
        for (auto& [k, eb] : E.env) {
            if (T.env.count(k) || (k.frame && k.frame != cur->frames.back())) continue;
            one(nullptr, &eb, out.env[k], [&](uint32_t i) { return readKey(k, i); });
        }
        for (auto& [sym, tb] : T.nba) {
            auto ie = E.nba.find(sym);
            const Symbol* sp = sym;
            one(&tb, ie == E.nba.end() ? nullptr : &ie->second, out.nba[sym], [&](uint32_t i) { return pending(*sp, i, true); });
        }
        for (auto& [sym, eb] : E.nba) {
            if (T.nba.count(sym)) continue;
            const Symbol* sp = sym;
            one(nullptr, &eb, out.nba[sym], [&](uint32_t i) { return pending(*sp, i, true); });
        }
        return out;
    }
    Flags mergeFlags(Bit c, const Flags& t, const Flags& e) {
        return {bv.mux(c, t.ret, e.ret), bv.mux(c, t.brk, e.brk), bv.mux(c, t.cont, e.cont)};
    }
    void absorb(Layer&& L) {
        Layer& top = cur->layers.back();
        for (auto& [k, b] : L.env) {
            auto& vec = top.env[k];
            if (vec.empty()) { vec = std::move(b); continue; }
            for (size_t i = 0; i < b.size(); ++i) if (isSet(b[i])) vec[i] = b[i];
        }
        for (auto& [sym, b] : L.nba) {
            auto& vec = top.nba[sym];
            if (vec.empty()) { vec = std::move(b); continue; }
            for (size_t i = 0; i < b.size(); ++i) if (isSet(b[i])) vec[i] = b[i];
        }
    }

    Bits readRange(const Claim& c) {
        Bits r(c.w);
        for (uint32_t i = 0; i < c.w; ++i) r[i] = readBit(*c.sym, c.lo + i);
        return r;
    }

    // Select elements out of a parent value.
    Bits select(const Bits& parent, const Level& L) {
        Bits r(size_t(L.cnt) * L.ew, kBX);
        if (L.isConst) {
            for (uint32_t k = 0; k < L.cnt; ++k) {
                int64_t p = L.pos + k;
                if (p < 0 || p >= int64_t(L.n)) continue;
                for (uint32_t b = 0; b < L.ew; ++b) r[size_t(k) * L.ew + b] = parent[size_t(p) * L.ew + b];
            }
            return r;
        }
        // A mux tree over the position bits; anything out of range is X.
        size_t nb = 0;
        while ((size_t(1) << nb) < L.n) ++nb;
        std::vector<Bits> cand(size_t(1) << nb, Bits(r.size(), kBX));
        for (size_t p = 0; p < L.n; ++p)
            for (uint32_t k = 0; k < L.cnt && p + k < L.n; ++k)
                for (uint32_t b = 0; b < L.ew; ++b)
                    cand[p][size_t(k) * L.ew + b] = parent[(p + k) * L.ew + b];
        for (size_t j = 0; j < nb; ++j) {
            std::vector<Bits> next(cand.size() / 2);
            for (size_t p = 0; p < next.size(); ++p) next[p] = bv.mux(L.dyn[j], cand[2 * p + 1], cand[2 * p]);
            cand.swap(next);
        }
        Bit oor = kB0;
        for (size_t j = nb; j < L.dyn.size(); ++j) oor = bv.or_(oor, L.dyn[j]);
        return bv.mux(oor, Bits(r.size(), kBX), cand[0]);
    }
    // The parent value with the selected elements replaced.
    Bits splice(Bits parent, const Level& L, const Bits& val) {
        if (L.isConst) {
            for (uint32_t k = 0; k < L.cnt; ++k) {
                int64_t p = L.pos + k;
                if (p < 0 || p >= int64_t(L.n)) continue;
                for (uint32_t b = 0; b < L.ew; ++b) parent[size_t(p) * L.ew + b] = val[size_t(k) * L.ew + b];
            }
            return parent;
        }
        for (size_t p = 0; p < L.n; ++p)
            for (uint32_t k = 0; k < L.cnt && k <= p; ++k) {
                Bit hit = bv.eq(L.dyn, Bv::constant(p - k, L.dyn.size()));
                if (isFalse(hit)) continue;
                for (uint32_t b = 0; b < L.ew; ++b)
                    parent[p * L.ew + b] = bv.mux(hit, val[size_t(k) * L.ew + b], parent[p * L.ew + b]);
            }
        return parent;
    }

    // Current value of an lvalue, as the next write to it would see it.
    Bits readLv(const Expression& e, bool nb) {
        Claim c;
        if (staticRange(e, true, false, c)) {
            Bits r(c.w);
            for (uint32_t i = 0; i < c.w; ++i) r[i] = pending(*c.sym, c.lo + i, nb);
            r.resize(widthOf(*e.type), kBX);
            return r;
        }
        if (e.kind == ExpressionKind::Conversion) return readLv(e.as<ConversionExpression>().operand(), nb);
        if (!isSelect(e)) fail("unsupported assignment target", e);
        Level L = level(e, true, true);
        return select(readLv(*L.parent, nb), L);
    }

    void assign(const Expression& lhs, const Bits& val, Bit gd, bool nb) {
        if (lhs.kind == ExpressionKind::Concatenation) {
            size_t hi = val.size();
            for (auto* op : lhs.as<ConcatenationExpression>().operands()) {
                size_t w = widthOf(*op->type);
                assign(*op, Bits(val.begin() + (hi - w), val.begin() + hi), gd, nb);
                hi -= w;
            }
            return;
        }
        Claim c;
        if (staticRange(lhs, true, false, c)) {
            for (uint32_t i = 0; i < c.w && i < val.size(); ++i) writeBit(*c.sym, c.lo + i, val[i], gd, nb);
            return;
        }
        if (lhs.kind == ExpressionKind::Conversion) {
            auto& cv = lhs.as<ConversionExpression>();
            return assign(cv.operand(), Bv::resize(val, widthOf(*cv.operand().type), false), gd, nb);
        }
        if (!isSelect(lhs)) fail("unsupported assignment target", lhs);
        Level L = level(lhs, true, true);
        assign(*L.parent, splice(readLv(*L.parent, nb), L, val), gd, nb);
    }

    // ----------------------------------------------------------- constants
    Bits fromSVInt(const SVInt& v, size_t w, bool sign) {
        Bits r(v.getBitWidth());
        for (bitwidth_t i = 0; i < v.getBitWidth(); ++i) {
            logic_t b = v[int32_t(i)];
            r[i] = b.isUnknown() ? kBX : (b.value ? kB1 : kB0);
        }
        return Bv::resize(std::move(r), w, sign);
    }
    Bits fromConst(const ConstantValue& cv, const Type& type) {
        size_t w = widthOf(type);
        if (cv.isInteger()) return fromSVInt(cv.integer(), w, cv.integer().isSigned());
        if (cv.isUnpacked()) {
            const Type& t = type.getCanonicalType();
            auto elems = cv.elements();
            Bits r;
            if (t.kind == SymbolKind::FixedSizeUnpackedArrayType) {
                const Type& et = *t.getArrayElementType();
                for (size_t i = elems.size(); i-- > 0;) {  // elements()[0] is the left index: the high end
                    Bits e = fromConst(elems[i], et);
                    r.insert(r.end(), e.begin(), e.end());
                }
            } else if (t.kind == SymbolKind::UnpackedStructType) {
                auto fields = fieldsOf(t);
                for (size_t i = elems.size(); i-- > 0;) {
                    Bits e = fromConst(elems[i], fields[i]->getType());
                    r.insert(r.end(), e.begin(), e.end());
                }
            } else {
                throw Unsupported("constant of type " + type.toString());
            }
            r.resize(w, kBX);
            return r;
        }
        throw Unsupported("constant of type " + type.toString());
    }

    // --------------------------------------------------------- expressions
    Bit truth(const Expression& e) { return bv.redOr(evalExpr(e)); }

    Bits evalExpr(const Expression& e) {
        size_t w = widthOf(*e.type);
        switch (e.kind) {
            case ExpressionKind::IntegerLiteral:
                return fromSVInt(e.as<IntegerLiteral>().getValue(), w, e.type->isSigned());
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {
                logic_t b = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                return Bits(w, b.isUnknown() ? kBX : (b.value ? kB1 : kB0));
            }
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue: {
                const Symbol& s = *valueSym(e);
                if (!isSignal(s)) break;  // parameter, enum value, genvar: a constant
                return readRange({&s, 0, info(s).width});
            }
            case ExpressionKind::ElementSelect:
            case ExpressionKind::RangeSelect:
            case ExpressionKind::MemberAccess: {
                Claim c;
                if (staticRange(e, true, false, c)) {
                    Bits r = readRange(c);
                    r.resize(w, kBX);
                    return r;
                }
                Level L = level(e, true, true);
                return select(evalExpr(*L.parent), L);
            }
            case ExpressionKind::UnaryOp: return evalUnary(e.as<UnaryExpression>());
            case ExpressionKind::BinaryOp: return evalBinary(e.as<BinaryExpression>());
            case ExpressionKind::ConditionalOp: {
                auto& c = e.as<ConditionalExpression>();
                Bit s = kB1;
                for (auto& cond : c.conditions) {
                    if (cond.pattern) fail("pattern in a conditional expression", e);
                    s = bv.and_(s, truth(*cond.expr));
                }
                if (isTrue(s)) return evalExpr(c.left());
                if (isFalse(s)) return evalExpr(c.right());
                return bv.mux(s, evalExpr(c.left()), evalExpr(c.right()));
            }
            case ExpressionKind::Concatenation: {
                Bits r;
                auto ops = e.as<ConcatenationExpression>().operands();
                for (size_t i = ops.size(); i-- > 0;) {
                    if (ops[i]->type->isVoid()) continue;
                    Bits b = evalExpr(*ops[i]);
                    r.insert(r.end(), b.begin(), b.end());
                }
                r.resize(w, kB0);
                return r;
            }
            case ExpressionKind::Replication: {
                auto& rep = e.as<ReplicationExpression>();
                Bits one = evalExpr(rep.concat());
                Bits r;
                if (!one.empty())
                    while (r.size() < w) r.insert(r.end(), one.begin(), one.end());
                r.resize(w, kB0);
                return r;
            }
            case ExpressionKind::Conversion: {
                auto& c = e.as<ConversionExpression>();
                const Type& from = *c.operand().type;
                if (from.isFloating() || e.type->isFloating() || from.isString()) break;
                return Bv::resize(evalExpr(c.operand()), w, from.isSigned());
            }
            case ExpressionKind::Assignment: return evalAssign(e.as<AssignmentExpression>());
            case ExpressionKind::Call: {
                auto& c = e.as<CallExpression>();
                if (c.subroutine.index() == 0) return callSub(c);
                std::string_view n = c.getSubroutineName();
                if (n == "$signed" || n == "$unsigned") return Bv::resize(evalExpr(*c.arguments()[0]), w, false);
                break;
            }
            case ExpressionKind::SimpleAssignmentPattern:
            case ExpressionKind::StructuredAssignmentPattern:
            case ExpressionKind::ReplicatedAssignmentPattern: {
                std::span<const Expression* const> elems;
                size_t times = 1;
                if (e.kind == ExpressionKind::SimpleAssignmentPattern)
                    elems = e.as<SimpleAssignmentPatternExpression>().elements();
                else if (e.kind == ExpressionKind::StructuredAssignmentPattern)
                    elems = e.as<StructuredAssignmentPatternExpression>().elements();
                else {
                    auto& rp = e.as<ReplicatedAssignmentPatternExpression>();
                    elems = rp.elements();
                    auto k = constInt(rp.count(), true);
                    if (!k) fail("replication count is not constant", e);
                    times = size_t(*k);
                }
                Bits one;
                for (size_t i = elems.size(); i-- > 0;) {
                    Bits b = evalExpr(*elems[i]);
                    one.insert(one.end(), b.begin(), b.end());
                }
                Bits r;
                for (size_t t = 0; t < times; ++t) r.insert(r.end(), one.begin(), one.end());
                if (r.size() != w) fail("assignment pattern does not fill its type", e);
                return r;
            }
            case ExpressionKind::Inside: {
                auto& in = e.as<InsideExpression>();
                Bits l = evalExpr(in.left());
                bool sign = in.left().type->isSigned();
                Bit any = kB0;
                for (auto* r : in.rangeList()) {
                    if (r->kind == ExpressionKind::ValueRange) {
                        auto& vr = r->as<ValueRangeExpression>();
                        Bits lo = Bv::resize(evalExpr(vr.left()), l.size(), vr.left().type->isSigned());
                        Bits hi = Bv::resize(evalExpr(vr.right()), l.size(), vr.right().type->isSigned());
                        any = bv.or_(any, bv.and_(bv.not_(bv.lt(l, lo, sign)), bv.not_(bv.lt(hi, l, sign))));
                    } else {
                        any = bv.or_(any, wildEq(l, *r, true));
                    }
                }
                return Bv::resize({any}, w, false);
            }
            case ExpressionKind::EmptyArgument: return Bv::resize(emptyArg, w, false);
            case ExpressionKind::LValueReference:
                if (lvalStack.empty()) fail("stray lvalue reference", e);
                return evalExpr(*lvalStack.back());
            case ExpressionKind::MinTypMax: return evalExpr(e.as<MinTypMaxExpression>().typ());
            default: break;
        }
        // Anything else must be a constant.
        ConstantValue cv = e.eval(ectx);
        if (!cv) fail("unsupported expression (" + std::string(toString(e.kind)) + ")", e);
        return fromConst(cv, *e.type);
    }

    // l == item, where x/z bits of a constant item match anything.
    Bit wildEq(const Bits& l, const Expression& item, bool zOnlyToo) {
        ConstantValue cv = item.eval(ectx);
        if (cv.isInteger() && cv.integer().hasUnknown()) {
            SVInt v = cv.integer();
            Bit r = kB1;
            for (size_t i = 0; i < l.size(); ++i) {
                logic_t b = i < v.getBitWidth() ? v[int32_t(i)] : logic_t(0);
                if (b.isUnknown()) {
                    if (!zOnlyToo && b.value != logic_t::Z_VALUE) return kB0;  // casez: an x never matches
                    continue;
                }
                r = bv.and_(r, b.value ? l[i] : bv.not_(l[i]));
            }
            return r;
        }
        return bv.eq(l, Bv::resize(evalExpr(item), l.size(), item.type->isSigned()));
    }

    Bits evalUnary(const UnaryExpression& u) {
        size_t w = widthOf(*u.type);
        using U = UnaryOperator;
        switch (u.op) {
            case U::Preincrement:
            case U::Predecrement:
            case U::Postincrement:
            case U::Postdecrement: {
                Bits old = evalExpr(u.operand());
                Bits one = Bv::constant(1, old.size());
                bool inc = u.op == U::Preincrement || u.op == U::Postincrement;
                Bits nv = inc ? bv.add(old, one) : bv.sub(old, one);
                assign(u.operand(), nv, guard, false);
                return (u.op == U::Preincrement || u.op == U::Predecrement) ? nv : old;
            }
            default: break;
        }
        Bits a = evalExpr(u.operand());
        switch (u.op) {
            case U::Plus: return a;
            case U::Minus: return bv.negate(a);
            case U::BitwiseNot: return bv.not_(a);
            case U::BitwiseAnd: return Bv::resize({bv.redAnd(a)}, w, false);
            case U::BitwiseOr: return Bv::resize({bv.redOr(a)}, w, false);
            case U::BitwiseXor: return Bv::resize({bv.redXor(a)}, w, false);
            case U::BitwiseNand: return Bv::resize({bv.not_(bv.redAnd(a))}, w, false);
            case U::BitwiseNor: return Bv::resize({bv.not_(bv.redOr(a))}, w, false);
            case U::BitwiseXnor: return Bv::resize({bv.not_(bv.redXor(a))}, w, false);
            case U::LogicalNot: return Bv::resize({bv.not_(bv.redOr(a))}, w, false);
            default: fail("unsupported unary operator", u);
        }
    }

    Bits evalBinary(const BinaryExpression& b) {
        size_t w = widthOf(*b.type);
        using B = BinaryOperator;
        auto one = [&](Bit x) { return Bv::resize({x}, w, false); };
        // Short-circuit forms first: the right side may have side effects only in
        // testbench code, so both sides are simply evaluated.
        Bits l = evalExpr(b.left());
        if (b.op == B::LogicalAnd || b.op == B::LogicalOr || b.op == B::LogicalImplication ||
            b.op == B::LogicalEquivalence) {
            Bit x = bv.redOr(l), y = truth(b.right());
            switch (b.op) {
                case B::LogicalAnd: return one(bv.and_(x, y));
                case B::LogicalOr: return one(bv.or_(x, y));
                case B::LogicalImplication: return one(bv.or_(bv.not_(x), y));
                default: return one(bv.not_(bv.xor_(x, y)));
            }
        }
        if (b.op == B::WildcardEquality) return one(wildEq(l, b.right(), true));
        if (b.op == B::WildcardInequality) return one(bv.not_(wildEq(l, b.right(), true)));
        Bits r = evalExpr(b.right());
        bool sign = b.left().type->isSigned() && b.right().type->isSigned();
        auto same = [&] {
            if (l.size() != r.size()) {
                size_t m = std::max(l.size(), r.size());
                l = Bv::resize(l, m, b.left().type->isSigned());
                r = Bv::resize(r, m, b.right().type->isSigned());
            }
        };
        switch (b.op) {
            case B::Add: same(); return bv.add(l, r);
            case B::Subtract: same(); return bv.sub(l, r);
            case B::Multiply: same(); return bv.mul(l, r);
            case B::Divide: same(); return bv.divmod(l, r, sign, false);
            case B::Mod: same(); return bv.divmod(l, r, sign, true);
            case B::BinaryAnd:
            case B::BinaryOr:
            case B::BinaryXor:
            case B::BinaryXnor: {
                same();
                Bits o(l.size());
                for (size_t i = 0; i < l.size(); ++i)
                    o[i] = b.op == B::BinaryAnd ? bv.and_(l[i], r[i])
                         : b.op == B::BinaryOr ? bv.or_(l[i], r[i])
                         : b.op == B::BinaryXor ? bv.xor_(l[i], r[i])
                         : bv.not_(bv.xor_(l[i], r[i]));
                return o;
            }
            case B::Equality:
            case B::CaseEquality: same(); return one(bv.eq(l, r));
            case B::Inequality:
            case B::CaseInequality: same(); return one(bv.not_(bv.eq(l, r)));
            case B::LessThan: same(); return one(bv.lt(l, r, sign));
            case B::GreaterThan: same(); return one(bv.lt(r, l, sign));
            case B::LessThanEqual: same(); return one(bv.not_(bv.lt(r, l, sign)));
            case B::GreaterThanEqual: same(); return one(bv.not_(bv.lt(l, r, sign)));
            case B::LogicalShiftLeft:
            case B::ArithmeticShiftLeft: return bv.shift(l, r, 0);
            case B::LogicalShiftRight: return bv.shift(l, r, 1);
            case B::ArithmeticShiftRight: return bv.shift(l, r, b.type->isSigned() ? 2 : 1);
            case B::Power: {
                uint64_t base, ex;
                if (Bv::constValue(r, ex) && ex < 64) {  // x ** constant
                    Bits acc = Bv::constant(1, w), m = Bv::resize(l, w, b.left().type->isSigned());
                    for (uint64_t i = 0; i < ex; ++i) acc = bv.mul(acc, m);
                    return acc;
                }
                if (Bv::constValue(l, base) && base == 2)  // 2 ** n
                    return bv.shift(Bv::constant(1, w), r, 0);
                fail("'**' needs a constant exponent or a base of 2", b);
            }
            default: fail("unsupported binary operator", b);
        }
    }

    Bits evalAssign(const AssignmentExpression& a) {
        lvalStack.push_back(&a.left());
        Bits rhs = evalExpr(a.right());
        lvalStack.pop_back();
        rhs = Bv::resize(rhs, widthOf(*a.left().type), a.right().type->isSigned());
        assign(a.left(), rhs, guard, a.isNonBlocking());
        return rhs;
    }

    Bits callSub(const CallExpression& c) {
        const SubroutineSymbol& sub = *std::get<0>(c.subroutine);
        if (++callDepth > 512) fail("function recursion is too deep (or not bounded by constants)", c);
        auto formals = sub.getArguments();
        auto args = c.arguments();
        std::vector<std::pair<const Symbol*, Bits>> bind;
        for (size_t i = 0; i < formals.size() && i < args.size(); ++i) {
            if (formals[i]->direction == ArgumentDirection::Out) continue;
            const Expression* a = args[i];
            if (a->kind == ExpressionKind::Assignment) a = &a->as<AssignmentExpression>().left();
            bind.push_back({formals[i], Bv::resize(evalExpr(*a), widthOf(formals[i]->getType()), a->type->isSigned())});
        }
        Flags sFlags = cur->f;
        Bit sGuard = guard;
        cur->f = {};
        uint32_t fid = cur->nextFrame++;
        cur->frames.push_back(fid);
        for (auto& [sym, b] : bind) cur->layers.back().env[Key{sym, fid}] = std::move(b);
        retVars.push_back(sub.returnValVar);
        execStmt(sub.getBody(), sGuard);
        retVars.pop_back();
        auto local = [&](const Symbol* s) {
            Bits r(widthOf(s->as<ValueSymbol>().getType()), kBX);
            Bit b;
            for (uint32_t i = 0; i < r.size(); ++i)
                if (findEnv(Key{s, fid}, i, b)) r[i] = b;
            return r;
        };
        std::vector<Bits> outs(formals.size());
        for (size_t i = 0; i < formals.size() && i < args.size(); ++i)
            if (formals[i]->direction != ArgumentDirection::In) outs[i] = local(formals[i]);
        Bits result;
        if (sub.returnValVar) result = local(sub.returnValVar);
        cur->frames.pop_back();
        cur->f = sFlags;
        guard = sGuard;
        for (size_t i = 0; i < formals.size() && i < args.size(); ++i) {
            if (formals[i]->direction == ArgumentDirection::In) continue;
            Bits v = outs[i];
            const Expression* a = args[i];
            if (a->kind == ExpressionKind::Assignment) {
                auto& as = a->as<AssignmentExpression>();
                emptyArg = v;
                v = evalExpr(as.right());
                a = &as.left();
            }
            assign(*a, Bv::resize(v, widthOf(*a->type), false), guard, false);
        }
        --callDepth;
        return result;
    }

    // ---------------------------------------------------------- statements
    Bit live(Bit gd) {
        Bit dead = bv.or_(cur->f.ret, bv.or_(cur->f.brk, cur->f.cont));
        return isFalse(dead) ? gd : bv.and_(gd, bv.not_(dead));
    }

    void execStmt(const Statement& s, Bit gd) {
        gd = live(gd);
        if (isFalse(gd)) return;
        Bit saved = guard;
        guard = gd;
        switch (s.kind) {
            case StatementKind::Empty:
            case StatementKind::ImmediateAssertion:
            case StatementKind::ConcurrentAssertion: break;
            case StatementKind::List:
                for (auto* c : s.as<StatementList>().list) execStmt(*c, gd);
                break;
            case StatementKind::Block: execStmt(s.as<BlockStatement>().body, gd); break;
            case StatementKind::Timed: execStmt(s.as<TimedStatement>().stmt, gd); break;
            case StatementKind::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Call && e.as<CallExpression>().subroutine.index() != 0) break;  // $display etc.
                evalExpr(e);
                break;
            }
            case StatementKind::VariableDeclaration: {
                const VariableSymbol& v = s.as<VariableDeclStatement>().symbol;
                declare(v);
                break;
            }
            case StatementKind::Conditional: {
                auto& c = s.as<ConditionalStatement>();
                Bit cond = kB1;
                for (auto& cc : c.conditions) {
                    if (cc.pattern) fail("pattern in an if", s);
                    cond = bv.and_(cond, truth(*cc.expr));
                }
                if (isTrue(cond)) { execStmt(c.ifTrue, gd); break; }
                if (isFalse(cond)) { if (c.ifFalse) execStmt(*c.ifFalse, gd); break; }
                Flags ft, fe;
                Layer T = branch(ft, [&] { execStmt(c.ifTrue, gd); });
                Layer E = branch(fe, [&] { if (c.ifFalse) execStmt(*c.ifFalse, gd); });
                absorb(mergeLayers(cond, std::move(T), std::move(E)));
                cur->f = mergeFlags(cond, ft, fe);
                break;
            }
            case StatementKind::Case: {
                auto& c = s.as<CaseStatement>();
                Bits sel = evalExpr(c.expr);
                struct Arm {
                    Bit m;
                    Layer L;
                    Flags f;
                };
                std::vector<Arm> arms;
                bool closed = false;  // an arm that always matches ends the list
                for (auto& item : c.items) {
                    Bit m = kB0;
                    for (auto* ie : item.expressions) {
                        Bit one;
                        if (c.condition == CaseStatementCondition::Inside) {
                            fail("case inside", s);
                        } else if (c.condition == CaseStatementCondition::Normal) {
                            ConstantValue cv = ie->eval(ectx);
                            if (cv.isInteger() && cv.integer().hasUnknown()) one = kB0;
                            else one = bv.eq(sel, Bv::resize(evalExpr(*ie), sel.size(), ie->type->isSigned()));
                        } else {
                            one = wildEq(sel, *ie, c.condition == CaseStatementCondition::WildcardXOrZ);
                        }
                        m = bv.or_(m, one);
                    }
                    if (isFalse(m)) continue;
                    Arm a;
                    a.m = m;
                    a.L = branch(a.f, [&] { execStmt(*item.stmt, gd); });
                    arms.push_back(std::move(a));
                    if (isTrue(m)) { closed = true; break; }
                }
                Flags accF = cur->f;
                Layer acc;
                if (closed) {
                    acc = std::move(arms.back().L);
                    accF = arms.back().f;
                    arms.pop_back();
                } else if (c.defaultCase) {
                    acc = branch(accF, [&] { execStmt(*c.defaultCase, gd); });
                } else if (c.check == UniquePriorityCheck::Unique || c.check == UniquePriorityCheck::Priority) {
                    // unique/priority with no default: no arm matching is an
                    // error in the design, so those values are don't-cares.
                    for (auto& a : arms) {
                        for (auto& [k, b] : a.L.env) {
                            auto& v = acc.env[k];
                            if (v.empty()) v.assign(b.size(), kUnset);
                            for (size_t i = 0; i < b.size(); ++i) if (isSet(b[i])) v[i] = kBX;
                        }
                        for (auto& [sym, b] : a.L.nba) {
                            auto& v = acc.nba[sym];
                            if (v.empty()) v.assign(b.size(), kUnset);
                            for (size_t i = 0; i < b.size(); ++i) if (isSet(b[i])) v[i] = kBX;
                        }
                    }
                }
                for (size_t k = arms.size(); k-- > 0;) {
                    acc = mergeLayers(arms[k].m, std::move(arms[k].L), std::move(acc));
                    accF = mergeFlags(arms[k].m, arms[k].f, accF);
                }
                absorb(std::move(acc));
                cur->f = accF;
                break;
            }
            case StatementKind::ForLoop: {
                auto& f = s.as<ForLoopStatement>();
                // Loop control runs unguarded: the loop variable is only
                // meaningful inside the loop, and it must stay a constant.
                guard = kB1;
                for (auto* v : f.loopVars) declare(*v);
                for (auto* init : f.initializers) {
                    if (init->kind == ExpressionKind::Assignment)
                        if (const Symbol* lv = valueSym(init->as<AssignmentExpression>().left())) loopCounters.insert(lv);
                    evalExpr(*init);
                }
                loop(s, f.stopExpr, &f.body, f.steps, gd, false);
                break;
            }
            case StatementKind::WhileLoop: {
                auto& wl = s.as<WhileLoopStatement>();
                loop(s, &wl.cond, &wl.body, {}, gd, false);
                break;
            }
            case StatementKind::DoWhileLoop: {
                auto& wl = s.as<DoWhileLoopStatement>();
                loop(s, &wl.cond, &wl.body, {}, gd, true);
                break;
            }
            case StatementKind::RepeatLoop: {
                auto& r = s.as<RepeatLoopStatement>();
                auto n = constInt(r.count, true);
                if (!n) fail("repeat count is not constant", s);
                Bit sBrk = cur->f.brk, sCont = cur->f.cont;
                cur->f.brk = kB0;
                for (int64_t i = 0; i < *n && !isTrue(cur->f.brk); ++i) {
                    cur->f.cont = kB0;
                    execStmt(r.body, gd);
                }
                cur->f.brk = sBrk, cur->f.cont = sCont;
                break;
            }
            case StatementKind::Return: {
                auto& r = s.as<ReturnStatement>();
                if (r.expr) {
                    Bits v = evalExpr(*r.expr);
                    const Symbol* rv = retVars.empty() ? nullptr : retVars.back();
                    if (!rv) fail("return with a value outside a function", s);
                    for (uint32_t i = 0; i < v.size() && i < info(*rv).width; ++i) writeBit(*rv, i, v[i], gd, false);
                }
                cur->f.ret = bv.or_(cur->f.ret, gd);
                break;
            }
            case StatementKind::Break: cur->f.brk = bv.or_(cur->f.brk, gd); break;
            case StatementKind::Continue: cur->f.cont = bv.or_(cur->f.cont, gd); break;
            default: fail("unsupported statement (" + std::string(toString(s.kind)) + ")", s);
        }
        guard = saved;
    }

    std::vector<const Symbol*> retVars;
    std::unordered_set<const Symbol*> loopCounters;

    void declare(const VariableSymbol& v) {
        Bits b(info(v).width, kBX);
        if (const Expression* init = v.getInitializer())
            b = Bv::resize(evalExpr(*init), b.size(), init->type->isSigned());
        cur->layers.back().env[keyOf(v)] = std::move(b);
    }

    void loop(const Statement& s, const Expression* cond, const Statement* body,
              std::span<const Expression* const> steps, Bit gd, bool bodyFirst) {
        Bit sBrk = cur->f.brk, sCont = cur->f.cont;
        cur->f.brk = kB0;
        for (size_t iter = 0;; ++iter) {
            if (iter > opt.maxLoop) fail("loop does not end within " + std::to_string(opt.maxLoop) + " iterations", s);
            if (cond && !(bodyFirst && iter == 0)) {
                guard = kB1;
                Bit c = truth(*cond);
                if (!isConst(c)) fail("loop condition is not a constant when the loop is unrolled", s);
                if (isFalse(c)) break;
            }
            if (isTrue(cur->f.brk) || isTrue(cur->f.ret)) break;
            cur->f.cont = kB0;
            execStmt(*body, gd);
            cur->f.cont = kB0;
            guard = kB1;
            for (auto* st : steps) evalExpr(*st);
        }
        cur->f.brk = sBrk, cur->f.cont = sCont;
    }

    // -------------------------------------------------------------- drivers
    void runDriver(Driver& dr) {
        if (dr.state == 2) return;
        dr.state = 1;
        Proc proc;
        Proc* savedCur = cur;
        Bit savedGuard = guard;
        auto savedLv = std::move(lvalStack);
        lvalStack.clear();
        cur = &proc;
        guard = kB1;
        switch (dr.kind) {
            case Driver::Assign: {
                Bits v = evalExpr(*dr.rhs);
                assign(*dr.lhs, Bv::resize(v, widthOf(*dr.lhs->type), dr.rhs->type->isSigned()), kB1, false);
                break;
            }
            case Driver::PortIn: {
                Bits v = Bv::resize(evalExpr(*dr.rhs), info(*dr.sym).width, dr.rhs->type->isSigned());
                SymInfo& si = info(*dr.sym);
                for (uint32_t i = 0; i < si.width; ++i) si.val[i] = v[i];
                break;
            }
            case Driver::PortOut: {
                Bits v = readGlobal(*dr.sym, 0, info(*dr.sym).width);
                if (dr.rhs) {
                    emptyArg = v;
                    v = evalExpr(*dr.rhs);
                }
                assign(*dr.lhs, Bv::resize(v, widthOf(*dr.lhs->type), false), kB1, false);
                break;
            }
            case Driver::BBoxOut: {
                Bits v(dr.names.size());
                for (size_t i = 0; i < v.size(); ++i) {
                    Lit in = g.input();
                    d.inputs.push_back({dr.names[i], in});
                    v[i] = {in, kFalse};
                }
                assign(*dr.lhs, Bv::resize(v, widthOf(*dr.lhs->type), false), kB1, false);
                break;
            }
            case Driver::Prim: runPrim(dr); break;
            case Driver::Comb: runComb(dr, proc); break;
            case Driver::FF: break;  // Q exists already; next state comes from runFF
        }
        cur = savedCur;
        guard = savedGuard;
        lvalStack = std::move(savedLv);
        dr.state = 2;
        for (auto& ph : dr.placeholders) {
            SymInfo& si = info(*ph.sym);
            if (si.val[ph.bit] == ph.p) si.val[ph.bit] = kBX;  // claimed, never assigned
            defs[nodeOf(ph.p.v)] = si.val[ph.bit].v;
            defs[nodeOf(ph.p.x)] = si.val[ph.bit].x;
            defName[nodeOf(ph.p.v)] = defName[nodeOf(ph.p.x)] = bitNames(*ph.sym)[ph.bit];
        }
        dr.placeholders.clear();
    }

    // A flop inside a library cell is called whatever the cell calls it (Q);
    // what identifies it is the net on its output. Record every signal whose
    // value is exactly a state element's Q.
    void findAliases() {
        std::unordered_map<Lit, size_t> byQ;
        // A latch is seen through its transparent output, not its stored value.
        for (size_t i = 0; i < d.states.size(); ++i) byQ[d.states[i].latch ? d.states[i].next.v : d.states[i].q] = i;
        for (auto& [sym, si] : syms) {
            if (isLocal(*sym)) continue;
            const std::vector<std::string>* nm = nullptr;
            for (uint32_t i = 0; i < si.width; ++i) {
                if (!isSet(si.val[i]) || si.val[i].x != kFalse) continue;
                bool inv = false;
                auto it = byQ.find(si.val[i].v);
                if (it == byQ.end()) it = byQ.find(neg(si.val[i].v)), inv = true;
                if (it == byQ.end()) continue;
                if (!nm) nm = &bitNames(*sym);
                State& st = d.states[it->second];
                if ((*nm)[i] != st.name) (inv ? st.invAliases : st.aliases).push_back((*nm)[i]);
            }
        }
        for (auto& st : d.states)
            for (auto* v : {&st.aliases, &st.invAliases}) {
                std::sort(v->begin(), v->end(), [](const std::string& a, const std::string& b) {
                    return a.size() != b.size() ? a.size() < b.size() : a < b;
                });
                if (v->size() > 8) v->resize(8);
            }
    }

    void resolvePlaceholders() {
        if (defs.empty()) return;
        std::vector<Lit> memo(g.size(), kNoLit);
        std::vector<uint8_t> active(g.size(), 0);
        auto fix = [&](Bit& b) {
            for (Lit* l : {&b.v, &b.x}) {
                uint32_t culprit = 0;
                Lit r = g.resolve(*l, memo, defs, active, &culprit);
                if (r == kNoLit) throw Unsupported("combinational loop through '" + defName[culprit] + "'");
                *l = r;
            }
        };
        for (auto& o : d.outputs) fix(o.second);
        for (auto& st : d.states) fix(st.next), fix(st.trig);
    }

    void runPrim(Driver& dr) {
        const PrimitiveInstanceSymbol& p = *dr.prim;
        auto conns = p.getPortConnections();
        std::string_view name = p.primitiveType.name;
        auto kind = p.primitiveType.primitiveKind;
        auto out = [&](const Expression* e, Bit v) {
            if (e->kind == ExpressionKind::Assignment) e = &e->as<AssignmentExpression>().left();
            assign(*e, Bv::resize({v}, widthOf(*e->type), false), kB1, false);
        };
        auto in = [&](const Expression* e) { return evalExpr(*e)[0]; };
        if (kind == PrimitiveSymbol::NInput) {
            Bit acc = in(conns[1]);
            for (size_t i = 2; i < conns.size(); ++i) {
                Bit b = in(conns[i]);
                if (name == "and" || name == "nand") acc = bv.and_(acc, b);
                else if (name == "or" || name == "nor") acc = bv.or_(acc, b);
                else acc = bv.xor_(acc, b);
            }
            if (name == "nand" || name == "nor" || name == "xnor") acc = bv.not_(acc);
            out(conns[0], acc);
        } else if (kind == PrimitiveSymbol::NOutput) {
            Bit v = in(conns.back());
            if (name == "not") v = bv.not_(v);
            for (size_t i = 0; i + 1 < conns.size(); ++i) out(conns[i], v);
        } else if (kind == PrimitiveSymbol::Fixed) {
            if (name == "pullup") return out(conns[0], kB1);
            if (name == "pulldown") return out(conns[0], kB0);
            if (name == "bufif0" || name == "bufif1" || name == "notif0" || name == "notif1") {
                Bit dv = in(conns[1]), en = in(conns[2]);
                if (name[0] == 'n') dv = bv.not_(dv);
                if (name.back() == '0') en = bv.not_(en);
                return out(conns[0], bv.mux(en, dv, kBX));  // off is Z; nothing else drives the net
            }
            throw Unsupported("primitive '" + std::string(name) + "' (" + pathOf(p) + ")");
        } else {
            // User-defined primitive: the table, row by row.
            const PrimitiveSymbol& u = p.primitiveType;
            if (u.isSequential)
                throw Unsupported("sequential UDP '" + std::string(name) + "' (" + pathOf(p) +
                                  "); use a Liberty library (-lib) or behavioural cell models for flops and latches");
            std::vector<Bit> ins;
            for (size_t i = 1; i < conns.size(); ++i) ins.push_back(in(conns[i]));
            Lit m1 = kFalse, m0 = kFalse, anyx = kFalse;
            for (Bit b : ins) anyx = g.or_(anyx, b.x);
            for (auto& row : u.table) {
                Lit m = kTrue;
                size_t k = 0;
                for (char ch : row.inputs) {
                    if (ch == ' ' || ch == '\t') continue;
                    if (k >= ins.size()) break;
                    Lit v = ins[k++].v;
                    if (ch == '0') m = g.and_(m, neg(v));
                    else if (ch == '1') m = g.and_(m, v);
                    else if (ch == 'x' || ch == 'X') m = kFalse;
                }
                if (row.output == '1') m1 = g.or_(m1, m);
                else if (row.output == '0') m0 = g.or_(m0, m);
            }
            Lit x = g.or_(anyx, neg(g.or_(m1, m0)));
            out(conns[0], {g.and_(m1, neg(x)), x});
        }
    }

    void runComb(Driver& dr, Proc& proc) {
        proc.kind = Proc::Comb;
        proc.drv = &dr;
        execStmt(*dr.body, kB1);
        // Gather the result of every claimed bit.
        struct Res {
            const Symbol* sym;
            uint32_t bit;
            Bit v;
        };
        std::vector<Res> res;
        for (auto& [sym, mask] : dr.claims) {
            auto& base = proc.layers[0];
            auto ie = base.env.find(Key{sym, 0});
            auto in = base.nba.find(sym);
            for (uint32_t i = 0; i < mask.size(); ++i) {
                if (!mask[i]) continue;
                Bit v = kBX;
                if (in != base.nba.end() && isSet(in->second[i])) v = in->second[i];
                else if (ie != base.env.end() && isSet(ie->second[i])) v = ie->second[i];
                res.push_back({sym, i, v});
            }
        }
        if (!proc.holds.empty()) resolveHolds(dr, proc, res);
        for (auto& r : res) info(*r.sym).val[r.bit] = r.v;
    }

    // A held value the block read. If no result depends on it, it was never
    // really needed (a full case with no default): fix it at 0. Otherwise the
    // bit is a latch whose stored value is that hold.
    template<typename Res>
    void resolveHolds(Driver& dr, Proc& proc, std::vector<Res>& res) {
        std::unordered_map<uint32_t, Lit> fix;
        std::vector<std::pair<std::pair<const Symbol*, uint32_t>, Lit>> real;
        for (auto& [key, h] : proc.holds) {
            // Cofactor every result against this hold and compare.
            std::unordered_map<uint32_t, Lit> m0, m1;
            m0[nodeOf(h)] = kFalse;
            m1[nodeOf(h)] = kTrue;
            Lit diff = kFalse;
            for (auto& r : res) {
                // Only definite values count: an X result is a don't-care
                // whatever the held value was.
                Lit v0 = g.rebuildSparse(r.v.v, m0), v1 = g.rebuildSparse(r.v.v, m1);
                if (v0 == v1) continue;
                Lit x0 = g.rebuildSparse(r.v.x, m0), x1 = g.rebuildSparse(r.v.x, m1);
                diff = g.or_(diff, g.and_(g.xor_(v0, v1), g.and_(neg(x0), neg(x1))));
            }
            bool depends = false;
            if (diff != kFalse) {
                Cnf cnf(g);
                depends = cnf.solve(diff, 200000) != 0;
            }
            if (depends) real.push_back({key, h});
            else fix[nodeOf(h)] = kFalse;
        }
        if (!fix.empty())
            for (auto& r : res) {
                r.v.v = g.rebuildSparse(r.v.v, fix);
                r.v.x = g.rebuildSparse(r.v.x, fix);
            }
        for (auto& [key, h] : real) {
            State st;
            st.name = bitNames(*key.first)[key.second];
            owner(*key.first, st);
            st.q = h;
            st.latch = true;
            st.next = kBX;
            for (auto& r : res)
                if (r.sym == key.first && r.bit == key.second) st.next = r.v;
            d.states.push_back(std::move(st));
        }
        (void)dr;
    }

    void owner(const Symbol& s, State& st) {
        for (const Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope())
            if (sc->asSymbol().kind == SymbolKind::InstanceBody) {
                auto& body = sc->asSymbol().as<InstanceBodySymbol>();
                st.cell = std::string(body.getDefinition().name);
                if (body.parentInstance) st.inst = pathOf(*body.parentInstance);
                if (st.inst == d.top) st.inst.clear();
                return;
            }
    }

    void runFF(Driver& dr) {
        Proc proc;
        proc.kind = Proc::FF;
        proc.drv = &dr;
        Proc* savedCur = cur;
        cur = &proc;
        guard = kB1;
        execStmt(*dr.body, kB1);
        Proc global;
        cur = &global;
        Bit trig = kB0;
        for (auto& ev : dr.events) {
            Bit b = evalExpr(*ev.expr)[0];
            trig = bv.or_(trig, ev.edge == EdgeKind::PosEdge ? b : bv.not_(b));
        }
        cur = savedCur;
        std::vector<const Symbol*> order;
        for (auto& c : dr.claimList)
            if (std::find(order.begin(), order.end(), c.sym) == order.end()) order.push_back(c.sym);
        for (const Symbol* sym : order) {
            auto& mask = dr.claims[sym];
            auto& base = proc.layers[0];
            auto ie = base.env.find(Key{sym, 0});
            auto in = base.nba.find(sym);
            const auto& nm = bitNames(*sym);
            for (uint32_t i = 0; i < mask.size(); ++i) {
                if (!mask[i]) continue;
                State st;
                st.name = nm[i];
                owner(*sym, st);
                st.q = info(*sym).val[i].v;
                st.next = {st.q, kFalse};
                if (in != base.nba.end() && isSet(in->second[i])) st.next = in->second[i];
                else if (ie != base.env.end() && isSet(ie->second[i])) st.next = ie->second[i];
                st.trig = trig;
                // A loop counter in a clocked block is not a register anyone built.
                if (loopCounters.count(sym)) continue;
                d.states.push_back(std::move(st));
            }
        }
    }
};

}  // namespace equiv
