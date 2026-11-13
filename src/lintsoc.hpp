// The SoC flow: check blocks one at a time, then the top with the blocks as
// black boxes.
//
//   mlint blk.sv -top blk -abstract-out blk.abs.json   # block sign-off
//   mlint top.sv -abstract blk.abs.json                 # top, without blk's RTL
//
// A block abstract records the block's ports (direction, width, and what the
// block does with each: an input it never reads, an output it never drives,
// an input it uses as a clock), its parameters, and the findings left after
// waivers. At the top, mlint builds an empty module from each abstract so the
// top elaborates without the block's RTL, and checks the connections against
// what the block said:
//
//   ABS001  a block output the block never drives is connected at the top
//   ABS002  the top drives a block input the block never reads
//   ABS003  a block clock input is left open or tied to a constant
//   ABS004  the block was signed off with errors or warnings not waived
//
// -bbox PATTERN makes modules black boxes without an abstract: their RTL is
// read, so the top elaborates, but nothing inside them is reported.
#pragma once

#include <fstream>
#include <sstream>

#include "lintir.hpp"
#include "tcl.hpp"
#include "vp/json.hpp"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/symbols/ParameterSymbols.h"

namespace lintsoc {
using namespace lintir;

inline const std::vector<const char*>& ruleIds() {
    static const std::vector<const char*> r = {"ABS001", "ABS002", "ABS003", "ABS004"};
    return r;
}

struct PortFact {
    std::string name, dir;  // in, out, inout
    uint64_t width = 1;                          // packed bits (of one element)
    std::vector<std::pair<int32_t, int32_t>> dims;  // unpacked dimensions, outermost first
    bool used = false, driven = false, clock = false;
};

struct ParamFact {
    std::string name;
    std::string value;  // decimal, when the value is an integer
};

struct BlockFact {
    std::string name;
    std::vector<PortFact> ports;
    std::vector<ParamFact> params;
    int errors = 0, warnings = 0;
    std::map<std::string, int> findings;  // not waived, by rule
};

// ------------------------------------------------------------ block facts

// What one module body does with its ports.
struct PortUse;
// The ports a body uses only as clocks (its own edges, or a child's clock
// port it feeds), keyed by body: computed once per body.
using ClockMemo = std::map<const InstanceBodySymbol*, std::set<const Symbol*>>;
inline const std::set<const Symbol*>& clockPorts(const InstanceBodySymbol& body, ClockMemo& memo);

struct PortUse {
    std::set<const Symbol*> used, driven, edges;
    ClockMemo* memo = nullptr;

    void lhs(const Expression& e, auto& v) {
        switch (e.kind) {
            case ExpressionKind::NamedValue:
                driven.insert(&e.as<NamedValueExpression>().symbol);
                return;
            case ExpressionKind::ElementSelect: {
                auto& s = e.as<ElementSelectExpression>();
                lhs(s.value(), v);
                (s.selector()).visit(v);
                return;
            }
            case ExpressionKind::RangeSelect: {
                auto& s = e.as<RangeSelectExpression>();
                lhs(s.value(), v);
                (s.left()).visit(v);
                (s.right()).visit(v);
                return;
            }
            case ExpressionKind::MemberAccess:
                lhs(e.as<MemberAccessExpression>().value(), v);
                return;
            case ExpressionKind::Concatenation:
                for (auto* x : e.as<ConcatenationExpression>().operands()) lhs(*x, v);
                return;
            default:
                (e).visit(v);
        }
    }

    void scan(const InstanceBodySymbol& body) {
        auto visitor = makeVisitor(
            [&](auto& v, const AssignmentExpression& a) {
                lhs(a.left(), v);
                (a.right()).visit(v);
            },
            [&](auto&, const NamedValueExpression& n) { used.insert(&n.symbol); },
            // An edge on a port is a clock, unless the body also reads the
            // port as a value (an asynchronous reset: if (!rst_n) ...).
            [&](auto& v, const SignalEventControl& ev) {
                if (ev.edge != EdgeKind::None && ev.expr.kind == ExpressionKind::NamedValue) {
                    edges.insert(&ev.expr.as<NamedValueExpression>().symbol);
                    if (ev.iffCondition) (*ev.iffCondition).visit(v);
                } else {
                    v.visitDefault(ev);
                }
            },
            [&](auto& v, const NetSymbol& n) {
                if (n.getInitializer()) driven.insert(&n);
                v.visitDefault(n);
            },
            [&](auto& v, const VariableSymbol& n) {
                if (n.getInitializer()) driven.insert(&n);
                v.visitDefault(n);
            },
            [&](auto& v, const GenerateBlockSymbol& g) {
                if (!g.isUninstantiated) v.visitDefault(g);
            },
            // A child instance: only its connections, not its insides.
            [&](auto& v, const InstanceSymbol& inst) {
                for (auto* c : inst.getPortConnections()) {
                    const Expression* e = c->getExpression();
                    if (!e) continue;
                    bool out = c->port.kind == SymbolKind::Port &&
                               c->port.as<PortSymbol>().direction != ArgumentDirection::In;
                    if (out && e->kind != ExpressionKind::Assignment) {
                        lhs(*e, v);
                        continue;
                    }
                    // Feeding a child's clock is a clock use, not a read.
                    if (!out && memo && e->kind == ExpressionKind::NamedValue &&
                        c->port.kind == SymbolKind::Port && c->port.as<PortSymbol>().internalSymbol &&
                        !inst.body.flags.has(InstanceFlags::Uninstantiated) &&
                        clockPorts(inst.body, *memo).count(c->port.as<PortSymbol>().internalSymbol)) {
                        edges.insert(&e->as<NamedValueExpression>().symbol);
                        continue;
                    }
                    (*e).visit(v);
                }
            });
        for (auto& m : body.members()) m.visit(visitor);
    }
};

inline const std::set<const Symbol*>& clockPorts(const InstanceBodySymbol& body, ClockMemo& memo) {
    auto it = memo.find(&body);
    if (it != memo.end()) return it->second;
    memo[&body];  // a cycle cannot happen, but stay finite
    PortUse u;
    u.memo = &memo;
    u.scan(body);
    std::set<const Symbol*> out;
    for (const Symbol* s : u.edges)
        if (!u.used.count(s)) out.insert(s);
    return memo[&body] = std::move(out);
}

inline std::string dirName(ArgumentDirection d) {
    switch (d) {
        case ArgumentDirection::In: return "in";
        case ArgumentDirection::Out: return "out";
        case ArgumentDirection::InOut: return "inout";
        default: return "ref";
    }
}

inline BlockFact facts(const InstanceSymbol& inst) {
    BlockFact b;
    b.name = std::string(inst.getDefinition().name);
    ClockMemo memo;
    PortUse u;
    u.memo = &memo;
    u.scan(inst.body);
    for (auto* ps : inst.body.getPortList()) {
        if (ps->kind != SymbolKind::Port) continue;
        auto& p = ps->as<PortSymbol>();
        PortFact f;
        f.name = std::string(p.name);
        f.dir = dirName(p.direction);
        const Type* t = &p.getType().getCanonicalType();
        while (t->kind == SymbolKind::FixedSizeUnpackedArrayType) {
            auto r = t->getFixedRange();
            f.dims.push_back({r.left, r.right});
            t = &t->getArrayElementType()->getCanonicalType();
        }
        f.width = t->getBitWidth();
        if (const Symbol* in = p.internalSymbol) {
            f.used = u.used.count(in) || u.edges.count(in);
            f.driven = u.driven.count(in);
            f.clock = u.edges.count(in) && !u.used.count(in);
        }
        b.ports.push_back(f);
    }
    for (auto* p : inst.body.getParameters()) {
        if (p->symbol.kind != SymbolKind::Parameter) continue;
        auto& ps = p->symbol.as<ParameterSymbol>();
        if (ps.isLocalParam()) continue;
        ParamFact pf;
        pf.name = std::string(ps.name);
        const ConstantValue& cv = ps.getValue();
        if (cv.isInteger() && !cv.integer().hasUnknown()) {
            auto x = cv.integer().as<int64_t>();
            if (x) pf.value = std::to_string(*x);
        }
        b.params.push_back(pf);
    }
    return b;
}

// ------------------------------------------------------------ files

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}

inline bool writeAbstract(const std::string& path, const std::vector<BlockFact>& blocks) {
    std::ofstream os(path);
    if (!os) return false;
    os << "{\n  \"format\": \"mlint-abstract-1\",\n  \"blocks\": [";
    for (size_t i = 0; i < blocks.size(); i++) {
        const BlockFact& b = blocks[i];
        os << (i ? ",\n" : "\n") << "    {\n      \"name\": " << quote(b.name) << ",\n";
        os << "      \"errors\": " << b.errors << ",\n      \"warnings\": " << b.warnings << ",\n";
        os << "      \"findings\": {";
        bool first = true;
        for (auto& [r, n] : b.findings) {
            os << (first ? "" : ", ") << quote(r) << ": " << n;
            first = false;
        }
        os << "},\n      \"parameters\": [";
        for (size_t k = 0; k < b.params.size(); k++)
            os << (k ? ", " : "") << "{\"name\": " << quote(b.params[k].name)
               << (b.params[k].value.empty() ? "" : ", \"value\": " + b.params[k].value) << "}";
        os << "],\n      \"ports\": [";
        for (size_t k = 0; k < b.ports.size(); k++) {
            const PortFact& p = b.ports[k];
            os << (k ? ",\n" : "\n") << "        {\"name\": " << quote(p.name) << ", \"dir\": " << quote(p.dir)
               << ", \"width\": " << p.width;
            if (!p.dims.empty()) {
                os << ", \"unpacked\": [";
                for (size_t d = 0; d < p.dims.size(); d++)
                    os << (d ? ", " : "") << "[" << p.dims[d].first << ", " << p.dims[d].second << "]";
                os << "]";
            }
            if (p.dir != "out") os << ", \"used\": " << (p.used ? "true" : "false");
            if (p.dir != "in") os << ", \"driven\": " << (p.driven ? "true" : "false");
            if (p.clock) os << ", \"clock\": true";
            os << "}";
        }
        os << "\n      ]\n    }";
    }
    os << "\n  ]\n}\n";
    return bool(os);
}

inline bool readAbstract(const std::string& path, std::vector<BlockFact>& out, std::string& err) {
    std::ifstream is(path);
    if (!is) {
        err = "cannot read '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << is.rdbuf();
    try {
        vp::json::Value v = vp::json::parse(ss.str());
        const vp::json::Value* f = v.get("format");
        if (!f || f->str() != "mlint-abstract-1") throw std::runtime_error("not an mlint abstract (format mlint-abstract-1)");
        const vp::json::Value* bs = v.get("blocks");
        if (!bs || bs->kind != vp::json::Value::Arr) throw std::runtime_error("no blocks");
        for (auto& b : bs->a) {
            BlockFact bf;
            bf.name = b.get("name")->str();
            if (auto* e = b.get("errors")) bf.errors = (int)e->u64();
            if (auto* w = b.get("warnings")) bf.warnings = (int)w->u64();
            if (auto* fs = b.get("findings"))
                for (auto& [r, n] : fs->o) bf.findings[r] = (int)n.u64();
            if (auto* ps = b.get("parameters"))
                for (auto& p : ps->a) bf.params.push_back({p.get("name")->str(), p.get("value") ? p.get("value")->s : ""});
            for (auto& p : b.get("ports")->a) {
                PortFact pf;
                pf.name = p.get("name")->str();
                pf.dir = p.get("dir")->str();
                pf.width = p.get("width")->u64();
                if (auto* u = p.get("unpacked"))
                    for (auto& d : u->a) pf.dims.push_back({(int32_t)d.a.at(0).num(), (int32_t)d.a.at(1).num()});
                pf.used = p.get("used") && p.get("used")->b;
                pf.driven = p.get("driven") && p.get("driven")->b;
                pf.clock = p.get("clock") && p.get("clock")->b;
                bf.ports.push_back(pf);
            }
            out.push_back(std::move(bf));
        }
    } catch (const std::exception& e) {
        err = path + ": " + e.what();
        return false;
    }
    return true;
}

// The empty module the top elaborates against.
inline std::string stubs(const std::vector<BlockFact>& blocks) {
    std::string s = "// Generated by mlint from block abstracts (-abstract).\n";
    for (auto& b : blocks) {
        s += "module " + b.name;
        if (!b.params.empty()) {
            s += " #(";
            for (size_t k = 0; k < b.params.size(); k++)
                s += (k ? ", " : "") + std::string("parameter ") + b.params[k].name + " = " +
                     (b.params[k].value.empty() ? "0" : b.params[k].value);
            s += ")";
        }
        s += " (";
        for (size_t k = 0; k < b.ports.size(); k++) {
            const PortFact& p = b.ports[k];
            std::string dir = p.dir == "in" ? "input" : p.dir == "out" ? "output" : p.dir == "inout" ? "inout" : "ref";
            std::string ty = p.dir == "inout" ? "wire" : "logic";
            s += std::string(k ? ", " : "") + dir + " " + ty +
                 (p.width > 1 ? " [" + std::to_string(p.width - 1) + ":0]" : "") + " " + p.name;
            for (auto& [l, r] : p.dims) s += " [" + std::to_string(l) + ":" + std::to_string(r) + "]";
        }
        s += ");\nendmodule\n";
    }
    return s;
}

// ------------------------------------------------------------ top checks

inline bool constant(const Expression& e) {
    std::vector<const Symbol*> n;
    namesIn(e, n);
    for (const Symbol* s : n)
        if (s->kind != SymbolKind::Parameter && s->kind != SymbolKind::EnumValue) return false;
    return true;
}

inline void checkTop(Compilation& comp, const std::vector<BlockFact>& blocks, Report& rep) {
    std::map<std::string, const BlockFact*> byName;
    for (auto& b : blocks) byName[b.name] = &b;
    std::set<std::string> told;
    comp.getRoot().visit(makeVisitor(
        [&](auto& v, const GenerateBlockSymbol& g) {
            if (!g.isUninstantiated) v.visitDefault(g);
        },
        [&](auto& v, const InstanceSymbol& inst) {
            if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
            auto it = byName.find(std::string(inst.getDefinition().name));
            if (it == byName.end()) {
                v.visitDefault(inst);
                return;
            }
            const BlockFact& b = *it->second;
            if ((b.errors || b.warnings) && told.insert(b.name).second) {
                std::string list;
                for (auto& [r, n] : b.findings) list += (list.empty() ? "" : ", ") + r + " " + std::to_string(n);
                rep.add("ABS004", inst.location,
                        "block '" + b.name + "' was signed off with " + std::to_string(b.errors) + " errors and " +
                            std::to_string(b.warnings) + " warnings not waived (" + list + ")");
            }
            std::map<std::string, const PortConnection*> conns;
            for (auto* c : inst.getPortConnections()) conns[std::string(c->port.name)] = c;
            for (const PortFact& p : b.ports) {
                auto c = conns.find(p.name);
                const Expression* e = c == conns.end() ? nullptr : c->second->getExpression();
                if (e && e->kind == ExpressionKind::Assignment) e = &e->as<AssignmentExpression>().left();
                bool open = !e || e->kind == ExpressionKind::EmptyArgument;
                SourceLocation loc = e ? e->sourceRange.start() : inst.location;
                std::string where = std::string(inst.name) + "." + p.name;
                if (p.dir == "out" && !p.driven && !open)
                    rep.add("ABS001", loc,
                            "output '" + where + "' is connected, but block '" + b.name + "' never drives it");
                if (p.dir == "in" && !p.used && !open && !constant(*e))
                    rep.add("ABS002", loc,
                            "input '" + where + "' is driven here, but block '" + b.name + "' never reads it");
                if (p.dir == "in" && p.clock && (open || constant(*e)))
                    rep.add("ABS003", loc,
                            "clock input '" + where + "' of block '" + b.name + "' is " +
                                (open ? "left open" : "tied to a constant"));
            }
        }));
}

// ------------------------------------------------------------ black boxes

// The lines of the module declarations -bbox names: findings there are dropped.
struct Boxes {
    std::vector<std::tuple<std::string, size_t, size_t>> ranges;  // file, first line, last line

    void add(Compilation& comp, const std::vector<std::string>& pats) {
        if (pats.empty()) return;
        const SourceManager& sm = *comp.getSourceManager();
        for (const Symbol* d : comp.getDefinitions()) {
            bool hit = false;
            for (auto& p : pats) hit |= tcl::globMatch(p.c_str(), std::string(d->name).c_str(), false);
            if (!hit || !d->getSyntax()) continue;
            SourceRange r = d->getSyntax()->sourceRange();
            auto a = sm.getFullyOriginalLoc(r.start()), z = sm.getFullyOriginalLoc(r.end());
            ranges.emplace_back(std::string(sm.getFileName(a)), sm.getLineNumber(a), sm.getLineNumber(z));
        }
    }
    bool inside(const Finding& f) const {
        for (auto& [file, a, z] : ranges)
            if (f.file == file && f.line >= a && f.line <= z) return true;
        return false;
    }
};

}  // namespace lintsoc
