// Formal-assisted structural checks: questions about what values the logic
// can take, answered exhaustively instead of by pattern.
//
//   FML001  an if condition or case item that can never be true where it
//           stands: the enclosing conditions and the earlier branches rule
//           it out (dead code).
//   FML002  an index or part select that can fall outside the array or
//           vector it selects from, on some path that reaches it.
//   FML003  a register that never leaves its reset value.
//
// How. Each question is a set of conditions over a few signals. The signals
// are enumerated over every value they can hold, and each combination is
// evaluated with slang's own expression evaluator, so the semantics are the
// language's, not a re-implementation. A signal driven by a single
// continuous assignment is replaced by its driver, so the free signals are
// the inputs and registers of the cone. A small register with a reset gets
// the set of values it can reach from reset (a fixed point over its next-state
// assignments), so a counter that wraps at 5 is known to stay below 6. When a
// question needs more than formal.max_bits bits of free signals (default 16)
// it is left unanswered: these rules only report what they proved.
//
// Values are two-state (0/1): X and Z are not enumerated. Register values
// are those reachable after reset.
#pragma once

#include <deque>
#include <functional>
#include <set>
#include <unordered_map>
#include <optional>

#include "lintir.hpp"
#include "parallel.hpp"
#include "slang/ast/EvalContext.h"
#include "slang/ast/expressions/CallExpression.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"

namespace lintformal {
using namespace lintir;

struct Options {
    unsigned maxBits = 16;     // free bits one question may enumerate
    unsigned maxRegBits = 8;   // registers this wide or narrower get reachable-value sets
    bool fast = true;          // the shortcuts (independent parts, whole cases, the case cache); off: the
                               // plain search, which gives the same findings (tests compare the two)
};

inline const std::vector<const char*>& ruleIds() {
    static const std::vector<const char*> r = {"FML001", "FML002", "FML003"};
    return r;
}

// ------------------------------------------------------------ conditions

// One fact on a path: an expression is true / false, or a case selector
// matches item k and none before it, or matches none of them.
struct Cond {
    enum Kind { Bool, Item } kind = Bool;
    const Expression* e = nullptr;  // Bool: the condition; Item: the selector
    bool want = true;
    const CaseStatement* cs = nullptr;
    size_t item = 0;          // Item: matches items[item] (or no item when item == items.size())
    bool alive = true;        // false once a signal it reads is assigned again
    std::set<const ValueSymbol*> reads;
};

// What an expression reads, and whether it can be evaluated at all.
struct Support {
    std::set<const ValueSymbol*> vars;
    bool params = false;  // names a parameter (code written for one parameterization)
    bool ok = true;       // false: a call, a class member, something not evaluated
};

inline void support(const Expression& e, Support& s) {
    e.visit(makeVisitor(
        [&](auto&, const NamedValueExpression& n) {
            switch (n.symbol.kind) {
                case SymbolKind::Parameter:
                case SymbolKind::Specparam:
                    s.params = true;
                    break;
                case SymbolKind::EnumValue:
                    break;
                default:
                    if (n.symbol.kind == SymbolKind::Variable || n.symbol.kind == SymbolKind::Net ||
                        n.symbol.kind == SymbolKind::Port || n.symbol.kind == SymbolKind::FormalArgument)
                        s.vars.insert(&n.symbol.as<ValueSymbol>());
                    else
                        s.ok = false;
            }
        },
        [&](auto&, const HierarchicalValueExpression&) { s.ok = false; },
        [&](auto& v, const CallExpression& c) {
            if (!c.isSystemCall()) {
                s.ok = false;
                return;
            }
            std::string_view n = c.getSubroutineName();
            static const std::set<std::string_view> pure = {"$signed", "$unsigned", "$clog2", "$bits",
                                                            "$countones", "$onehot", "$onehot0", "$size",
                                                            "$high", "$low", "$left", "$right"};
            if (!pure.count(n)) s.ok = false;
            else v.visitDefault(c);
        },
        [&](auto&, const MemberAccessExpression& m) {
            // A packed struct field is fine; a class member is not.
            if (!m.value().type->isIntegral()) s.ok = false;
            else support(m.value(), s);
        }));
}

// ------------------------------------------------------------ the solver

// Shared, read-only during the parallel phase.
struct Model {
    // A net with exactly one continuous assignment, whole: net -> driver.
    std::unordered_map<const ValueSymbol*, const Expression*> comb;
    // Registers with a known set of reachable values.
    std::unordered_map<const ValueSymbol*, std::vector<uint64_t>> domain;
    // Drivers inside a generate block: a parameter chose them.
    std::unordered_set<const ValueSymbol*> combInGen;
    Options opt;
};

struct Leaf {
    const ValueSymbol* sym;
    bitwidth_t w;
    bool sgn;
    const std::vector<uint64_t>* dom;  // null: every value of w bits
    uint64_t size() const { return dom ? dom->size() : (uint64_t(1) << w); }
};

// Expands the free signals of a question through single-driver nets.
struct Cone {
    std::vector<Leaf> leaves;
    std::vector<std::pair<const ValueSymbol*, const Expression*>> derived;  // evaluation order
    bool ok = true;
    bool params = false;

    void add(const Model& m, const ValueSymbol* v, std::set<const ValueSymbol*>& seen, int depth) {
        if (!ok || !seen.insert(v).second) return;
        auto c = m.comb.find(v);
        if (c != m.comb.end() && depth < 6) {
            Support s;
            support(*c->second, s);
            if (s.ok) {
                params |= s.params || m.combInGen.count(v);
                for (auto* u : s.vars) add(m, u, seen, depth + 1);
                derived.push_back({v, c->second});
                return;
            }
        }
        const Type& t = v->getType();
        if (!t.isIntegral() || t.getBitWidth() > 63) {
            ok = false;
            return;
        }
        auto d = m.domain.find(v);
        leaves.push_back({v, t.getBitWidth(), t.isSigned(), d == m.domain.end() ? nullptr : &d->second});
    }
};

// Enumerates every combination of the free signals of `conds`; calls `each`
// with the context set up for each combination where all conds hold. `each`
// returns false to stop. Returns -1 when the question is too big or a value
// could not be evaluated, else the number of combinations where all held
// (capped by the early stop).
// Which item of a case with constant items a selector value goes to, kept
// across the questions of one block: >= 0 the first item that matches (the
// item count: none), -(j + 1) an item that could not be evaluated at j.
struct FirstCache {
    std::unordered_map<const CaseStatement*, bool> constant;
    std::unordered_map<const CaseStatement*, std::unordered_map<uint64_t, int32_t>> first;
};

struct Solver {
    const Model& m;
    const Symbol& scope;
    bool params = false;  // the last question read a parameter, here or in a cone
    FirstCache* cache = nullptr;

    bool dry = false;  // only check the size, as if every register could take every value
    unsigned slack = 0;  // ...allowing this many more bits, since registers usually take fewer

    template<typename F>
    long long run(const std::vector<const Cond*>& conds, const std::vector<const Expression*>& extra, F&& each) {
        Support s;
        for (const Cond* c : conds) {
            support(*c->e, s);
            if (c->kind == Cond::Item)
                for (auto& it : c->cs->items)
                    for (const Expression* x : it.expressions) support(*x, s);
        }
        for (const Expression* x : extra) support(*x, s);
        params = s.params;
        if (!s.ok) return -1;
        Cone cone;
        cone.params = s.params;
        std::set<const ValueSymbol*> seen;
        for (auto* v : s.vars) cone.add(m, v, seen, 0);
        params |= cone.params;
        if (!cone.ok) return -1;
        double bits = 0;
        uint64_t total = 1;
        if (dry)
            for (auto& l : cone.leaves) l.dom = nullptr;
        for (auto& l : cone.leaves) {
            bits += std::log2((double)std::max<uint64_t>(1, l.size()));
            if (bits > m.opt.maxBits + (dry ? slack : 0)) return -1;
            total *= std::max<uint64_t>(1, l.size());
            if (l.size() == 0) return 0;  // a register with no reachable value
        }
        if (dry) return 0;
        EvalContext ctx(ASTContext(*scope.getParentScope(), LookupLocation::max), EvalFlags::IsScript);
        ctx.pushEmptyFrame();
        const size_t L = cone.leaves.size();
        std::vector<ConstantValue*> slot;
        for (auto& l : cone.leaves) slot.push_back(ctx.createLocal(l.sym, SVInt(l.w, l.dom ? (*l.dom)[0] : 0, l.sgn)));
        std::vector<ConstantValue*> dslot;
        for (auto& [v, e] : cone.derived) dslot.push_back(ctx.createLocal(v, nullptr));

        // Independent parts. A condition that shares no signal (through the
        // cone) with the question only has to be satisfiable once, so the
        // parts are searched one after another, not as a product: the same
        // answer, and the same first witness, in the sum of their sizes.
        std::vector<int> comp(L);
        for (size_t i = 0; i < L; i++) comp[i] = (int)i;
        std::function<int(int)> find = [&](int x) { return comp[x] == x ? x : comp[x] = find(comp[x]); };
        std::unordered_map<const ValueSymbol*, int> leafAt;
        for (size_t i = 0; i < L; i++) leafAt[cone.leaves[i].sym] = (int)i;
        auto leavesOf = [&](auto&& addSupport) {
            Support su;
            addSupport(su);
            Cone c;
            std::set<const ValueSymbol*> sn;
            for (auto* v : su.vars) c.add(m, v, sn, 0);
            std::vector<int> out;
            for (auto& l : c.leaves)
                if (auto it = leafAt.find(l.sym); it != leafAt.end()) out.push_back(it->second);
            return out;
        };
        auto join = [&](const std::vector<int>& ls) {
            for (size_t i = 1; i < ls.size(); i++) comp[find(ls[i])] = find(ls[0]);
        };
        std::vector<std::vector<int>> condLeaves;
        for (const Cond* c : conds) {
            condLeaves.push_back(leavesOf([&](Support& su) {
                support(*c->e, su);
                if (c->kind == Cond::Item)
                    for (auto& it : c->cs->items)
                        for (const Expression* x : it.expressions) support(*x, su);
            }));
            join(condLeaves.back());
        }
        std::vector<int> mainLeaves = leavesOf([&](Support& su) {
            for (const Expression* x : extra) support(*x, su);
        });
        if (extra.empty() && !conds.empty()) mainLeaves = condLeaves.back();
        join(mainLeaves);
        int mainComp = mainLeaves.empty() ? -1 : find(mainLeaves[0]);
        std::vector<int> parts;   // the other parts, in leaf order
        for (size_t i = 0; i < L; i++) {
            int r = find((int)i);
            if (r != mainComp && std::find(parts.begin(), parts.end(), r) == parts.end()) parts.push_back(r);
        }
        if (!m.opt.fast) {   // one part: the plain product
            for (size_t i = 0; i < L; i++) comp[i] = 0;
            mainComp = L ? 0 : -1;
            parts.clear();
        }
        if (mainComp < 0 && !parts.empty()) {   // nothing to ask about: the last part is the question
            mainComp = parts.back();
            parts.pop_back();
        }
        auto inPart = [&](int part, const std::vector<int>& ls) {
            if (ls.empty()) return part == mainComp;   // a constant condition goes with the question
            return find(ls[0]) == part;
        };

        // Every combination of the leaves of one part (the rest stay as they
        // are); stop when `hit` returns false. -1 on a value that cannot be
        // evaluated.
        auto search = [&](int part, auto&& hit) -> long long {
            std::vector<size_t> mine;
            uint64_t n = 1;
            for (size_t i = 0; i < L; i++)
                if (find((int)i) == part) mine.push_back(i), n *= std::max<uint64_t>(1, cone.leaves[i].size());
            std::vector<const Cond*> mineConds;
            for (size_t k = 0; k < conds.size(); k++)
                if (inPart(part, condLeaves[k])) mineConds.push_back(conds[k]);
            // A condition that pins a leaf to constants (case (x) with constant
            // items, x == constant) lets the leaf run over those values only:
            // a superset of the values where it holds, in the same order.
            std::vector<std::vector<uint64_t>> only(mine.size());
            std::vector<char> pinned(mine.size(), 0);
            if (m.opt.fast)
                for (const Cond* cd : mineConds) {
                    const Expression* x = cd->e;
                    std::vector<const Expression*> vals;
                    if (cd->kind == Cond::Item) {
                        if (cd->item >= cd->cs->items.size() || cd->cs->condition != CaseStatementCondition::Normal)
                            continue;
                        for (const Expression* v : cd->cs->items[cd->item].expressions) vals.push_back(v);
                    } else if (cd->want && x->kind == ExpressionKind::BinaryOp &&
                               x->as<BinaryExpression>().op == BinaryOperator::Equality) {
                        auto& be = x->as<BinaryExpression>();
                        x = &be.left();
                        vals.push_back(&be.right());
                    } else {
                        continue;
                    }
                    if (x->kind != ExpressionKind::NamedValue) continue;
                    auto li = leafAt.find(&x->as<NamedValueExpression>().symbol.as<ValueSymbol>());
                    if (li == leafAt.end()) continue;
                    size_t j = std::find(mine.begin(), mine.end(), (size_t)li->second) - mine.begin();
                    if (j == mine.size()) continue;
                    const Leaf& l = cone.leaves[mine[j]];
                    std::set<uint64_t> want;
                    bool ok = true;
                    for (const Expression* v : vals) {
                        Support su;
                        support(*v, su);
                        ConstantValue cv = su.ok && su.vars.empty() ? v->eval(ctx) : ConstantValue();
                        if (cv.bad() || !cv.isInteger() || cv.integer().hasUnknown()) { ok = false; break; }
                        // x matches only if its w bits equal the low w bits of the
                        // value (a narrower signed value would be sign-extended: left alone)
                        const SVInt& iv = cv.integer();
                        if (iv.getBitWidth() < l.w && iv.isSigned()) { ok = false; break; }
                        SVInt low = iv.getBitWidth() > l.w ? iv.trunc(l.w) : iv;
                        want.insert(*low.zext(64).as<uint64_t>());
                    }
                    if (!ok) continue;
                    std::vector<uint64_t> list;
                    if (pinned[j]) {
                        for (uint64_t v : only[j]) if (want.count(v)) list.push_back(v);
                    } else if (l.dom) {
                        for (uint64_t v : *l.dom) if (want.count(v)) list.push_back(v);
                    } else {
                        for (uint64_t v : want) if (l.w >= 64 || v < (uint64_t(1) << l.w)) list.push_back(v);
                    }
                    only[j] = std::move(list);
                    pinned[j] = 1;
                }
            auto sizeOf = [&](size_t j) { return pinned[j] ? (uint64_t)only[j].size() : cone.leaves[mine[j]].size(); };
            n = 1;
            for (size_t j = 0; j < mine.size(); j++) {
                if (sizeOf(j) == 0) return 0;
                n *= sizeOf(j);
            }
            std::vector<uint64_t> idx(mine.size(), 0);
            long long hits = 0;
            for (uint64_t c = 0; c < n; c++) {
                for (size_t j = 0; j < mine.size(); j++) {
                    auto& l = cone.leaves[mine[j]];
                    uint64_t v = pinned[j] ? only[j][idx[j]] : l.dom ? (*l.dom)[idx[j]] : idx[j];
                    *slot[mine[j]] = SVInt(l.w, v, l.sgn);
                }
                for (size_t i = 0; i < cone.derived.size(); i++) {
                    ConstantValue cv = cone.derived[i].second->eval(ctx);
                    if (cv.bad()) return -1;
                    const Type& t = cone.derived[i].first->getType();
                    *dslot[i] = cv.convertToInt(t.getBitWidth(), t.isSigned(), false);
                }
                bool all = true;
                for (const Cond* cd : mineConds) {
                    int h = holds(*cd, ctx);
                    if (h < 0) return -1;
                    if (!h) {
                        all = false;
                        break;
                    }
                }
                if (all) {
                    hits++;
                    if (!hit()) return hits;
                }
                for (size_t j = 0; j < idx.size(); j++) {   // next combination
                    if (++idx[j] < sizeOf(j)) break;
                    idx[j] = 0;
                }
            }
            return hits;
        };
        for (int part : parts) {
            long long r = search(part, [&] { return false; });   // satisfiable? keep the first combination
            if (r <= 0) return r;
        }
        if (mainComp < 0) {   // no leaves at all: one combination
            for (const Cond* cd : conds) {
                int h = holds(*cd, ctx);
                if (h < 0) return -1;
                if (!h) return 0;
            }
            each(ctx, cone);
            return 1;
        }
        return search(mainComp, [&] { return each(ctx, cone); });
    }

    static int truth(const ConstantValue& v) {
        if (v.bad()) return -1;
        if (v.isInteger() && v.integer().hasUnknown()) return 0;
        return v.isTrue() ? 1 : 0;
    }

    static int matches(const CaseStatement& cs, const ConstantValue& sel, const Expression& item, EvalContext& ctx) {
        ConstantValue iv = item.eval(ctx);
        if (iv.bad() || !sel.isInteger() || !iv.isInteger()) return -1;
        // Both sides are extended to the wider of the two, as in a case.
        const SVInt& a = sel.integer();
        const SVInt& b = iv.integer();
        bitwidth_t w = std::max(a.getBitWidth(), b.getBitWidth());
        SVInt x = a.getBitWidth() < w ? extend(a, w) : a;
        SVInt y = b.getBitWidth() < w ? extend(b, w) : b;
        switch (cs.condition) {
            case CaseStatementCondition::WildcardXOrZ: return caseXWildcardEqual(x, y);
            case CaseStatementCondition::WildcardJustZ: return caseZWildcardEqual(x, y);
            default: return exactlyEqual(x, y);
        }
    }
    static SVInt extend(const SVInt& v, bitwidth_t w) { return v.isSigned() ? v.sext(w) : v.zext(w); }

    int holds(const Cond& c, EvalContext& ctx) {
        if (c.kind == Cond::Bool) {
            int t = truth(c.e->eval(ctx));
            return t < 0 ? -1 : (t == (c.want ? 1 : 0));
        }
        ConstantValue sel = c.e->eval(ctx);
        if (sel.bad()) return -1;
        size_t n = c.cs->items.size();
        if (cache && m.opt.fast && sel.isInteger() && !sel.integer().hasUnknown() && sel.integer().getBitWidth() <= 64) {
            auto [ci, fresh] = cache->constant.try_emplace(c.cs, false);
            if (fresh) {
                Support su;
                for (auto& it : c.cs->items)
                    for (const Expression* x : it.expressions) support(*x, su);
                ci->second = su.ok && su.vars.empty();
            }
            if (ci->second) {
                uint64_t key = *sel.integer().as<uint64_t>();
                auto& tab = cache->first[c.cs];
                auto it = tab.find(key);
                int32_t f;
                if (it != tab.end()) {
                    f = it->second;
                } else {
                    f = (int32_t)n;
                    for (size_t k = 0; k < n && f == (int32_t)n; k++)
                        for (const Expression* x : c.cs->items[k].expressions) {
                            int mm = matches(*c.cs, sel, *x, ctx);
                            if (mm < 0) { f = -(int32_t)k - 1; break; }
                            if (mm == 1) f = (int32_t)k;
                        }
                    tab.emplace(key, f);
                }
                size_t want = c.item;
                if (f < 0) {
                    size_t j = (size_t)(-f - 1);
                    if (j <= want) return -1;   // reached before the item asked about
                    return want < n ? 0 : 1;    // (unreachable for want == n: j < n)
                }
                if ((size_t)f < want) return 0;
                if ((size_t)f == want) return 1;
                return want < n ? 0 : 1;        // nothing up to the item matched
            }
        }
        for (size_t k = 0; k <= std::min(c.item, n - 1) && k < n; k++) {
            bool any = false;
            for (const Expression* x : c.cs->items[k].expressions) {
                int mm = matches(*c.cs, sel, *x, ctx);
                if (mm < 0) return -1;
                any |= mm == 1;
            }
            if (k < c.item && any) return 0;   // an earlier item takes it
            if (k == c.item) return any ? 1 : 0;
        }
        return 1;  // item == n: no item matched
    }
};

// ------------------------------------------------------------ the walk

struct Found {
    const char* rule;
    SourceLocation loc;
    std::string text;
    std::string signal;
};

inline std::string valueText(const ConstantValue& v) {
    if (!v.isInteger()) return v.toString();
    auto x = v.integer().as<int64_t>();
    return x ? std::to_string(*x) : v.toString();
}

inline std::string witness(EvalContext& ctx, const Cone& cone) {
    std::string w;
    size_t n = 0;
    for (auto& l : cone.leaves) {
        if (n++ == 3) {
            w += ", ...";
            break;
        }
        ConstantValue* v = ctx.findLocal(l.sym);
        w += (w.empty() ? "" : ", ") + std::string(l.sym->name) + " = " + (v ? valueText(*v) : "?");
    }
    return w;
}

// An assignment the reachable-value pass needs: register <= rhs under path.
struct RegAssign {
    const ValueSymbol* reg;
    const Expression* rhs;
    std::vector<const Cond*> path;
    bool resetBranch;
};

struct Walk {
    const Model& m;
    const Rules& rules;
    const Symbol& scope;
    std::vector<Found>* out = nullptr;        // checking
    std::vector<RegAssign>* regs = nullptr;   // collecting register assignments
    std::set<const ValueSymbol*> blockingInBlock;
    std::deque<Cond> store;
    std::vector<Cond*> path;
    int loops = 0;
    bool inReset = false;
    std::set<const ValueSymbol*> loopRegs;  // assigned inside a loop: no reachable set
    FirstCache fcache;   // case items by selector value, for every question of this block

    Walk(const Model& mm, const Rules& r, const Symbol& s) : m(mm), rules(r), scope(s) {}

    Cond* push(Cond c) {
        Support s;
        support(*c.e, s);
        if (c.kind == Cond::Item)
            for (auto& it : c.cs->items)
                for (const Expression* x : it.expressions) support(*x, s);
        c.reads = s.vars;
        store.push_back(std::move(c));
        path.push_back(&store.back());
        return path.back();
    }
    void pop(size_t n) { path.resize(path.size() - n); }

    std::vector<const Cond*> live() const {
        std::vector<const Cond*> v;
        for (Cond* c : path)
            if (c->alive) v.push_back(c);
        return v;
    }
    bool anyParams(const std::vector<const Cond*>& cs) const {
        for (const Cond* c : cs) {
            Support s;
            support(*c->e, s);
            if (c->kind == Cond::Item)
                for (auto& it : c->cs->items)
                    for (const Expression* x : it.expressions) support(*x, s);
            if (s.params) return true;
        }
        return false;
    }

    void kill(const ValueSymbol* v) {
        for (Cond* c : path)
            if (c->reads.count(v)) c->alive = false;
    }
    void killAll() {
        for (Cond* c : path) c->alive = false;
    }

    static const ValueSymbol* target(const Expression& lhs) {
        const Expression* e = &lhs;
        while (true) {
            if (e->kind == ExpressionKind::ElementSelect) e = &e->as<ElementSelectExpression>().value();
            else if (e->kind == ExpressionKind::RangeSelect) e = &e->as<RangeSelectExpression>().value();
            else if (e->kind == ExpressionKind::MemberAccess) e = &e->as<MemberAccessExpression>().value();
            else break;
        }
        if (e->kind == ExpressionKind::NamedValue) {
            auto& s = e->as<NamedValueExpression>().symbol;
            if (s.kind == SymbolKind::Variable || s.kind == SymbolKind::Net) return &s.as<ValueSymbol>();
        }
        return nullptr;
    }

    // Did this condition alone, or with the path, prove nothing can reach here?
    // Returns true when the branch is dead and not because of a parameter.
    bool dead(const Cond& c) {
        Solver sv{m, scope};
        sv.cache = &fcache;
        auto p = live();
        p.push_back(&c);
        if (anyParams(p)) return false;
        long long r = sv.run(p, {}, [](EvalContext&, const Cone&) { return false; });
        return r == 0 && !sv.params;
    }

    // For each item of a case, whether it is dead on the current path, as
    // dead(Cond::Item k) would answer, in one enumeration: 1 dead, 0 not.
    // Empty when the case cannot be answered as a whole.
    std::vector<char> deadItems(const CaseStatement& cs) {
        size_t n = cs.items.size();
        if (!n) return {};
        Cond first{Cond::Item, &cs.expr, true, &cs, 0};
        auto p = live();
        p.push_back(&first);
        if (anyParams(p)) return std::vector<char>(n, 0);
        p.pop_back();
        std::vector<const Expression*> extra{&cs.expr};
        for (auto& it : cs.items)
            for (const Expression* x : it.expressions) extra.push_back(x);
        Solver sv{m, scope};
        sv.cache = &fcache;
        std::vector<char> hit(n, 0);
        size_t errFrom = n;   // items from errFrom on are not decided
        long long r = sv.run(p, extra, [&](EvalContext& ctx, const Cone&) {
            ConstantValue sel = cs.expr.eval(ctx);
            if (sel.bad()) {
                errFrom = 0;
                return false;
            }
            for (size_t k = 0; k < n && k < errFrom; k++) {
                bool any = false;
                for (const Expression* x : cs.items[k].expressions) {
                    int mm = Solver::matches(cs, sel, *x, ctx);
                    if (mm < 0) {
                        errFrom = k;
                        break;
                    }
                    any |= mm == 1;
                }
                if (k >= errFrom) break;
                if (any) {
                    hit[k] = 1;
                    break;
                }
            }
            for (size_t k = 0; k < errFrom; k++)   // stop once every decidable item is hit
                if (!hit[k]) return true;
            return false;
        });
        std::vector<char> d(n, 0);
        if (r < 0 || sv.params) return d;
        for (size_t k = 0; k < errFrom; k++) d[k] = !hit[k];
        return d;
    }

    // ---- expressions: selects, and ternaries as path conditions

    void expr(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::ConditionalOp: {
                auto& c = e.as<ConditionalExpression>();
                for (auto& cc : c.conditions) expr(*cc.expr);
                if (c.conditions.size() == 1 && !c.conditions[0].pattern) {
                    push({Cond::Bool, c.conditions[0].expr, true});
                    expr(c.left());
                    pop(1);
                    push({Cond::Bool, c.conditions[0].expr, false});
                    expr(c.right());
                    pop(1);
                } else {
                    expr(c.left());
                    expr(c.right());
                }
                return;
            }
            case ExpressionKind::ElementSelect: {
                auto& s = e.as<ElementSelectExpression>();
                expr(s.value());
                expr(s.selector());
                checkIndex(s.value(), s.selector(), nullptr, e);
                return;
            }
            case ExpressionKind::RangeSelect: {
                auto& s = e.as<RangeSelectExpression>();
                expr(s.value());
                expr(s.left());
                expr(s.right());
                if (s.getSelectionKind() != RangeSelectionKind::Simple)
                    checkIndex(s.value(), s.left(), &s, e);
                return;
            }
            case ExpressionKind::Assignment: {
                auto& a = e.as<AssignmentExpression>();
                expr(a.right());
                exprLhs(a.left());
                if (const ValueSymbol* t = target(a.left())) {
                    if (!a.isNonBlocking()) {
                        kill(t);
                        blockingInBlock.insert(t);
                    } else if (regs && a.left().kind == ExpressionKind::NamedValue) {
                        regs->push_back({t, &a.right(), live(), inReset});
                        if (loops) loopRegs.insert(t);
                    } else if (regs) {
                        loopRegs.insert(t);  // a partial write: no reachable set
                    }
                }
                return;
            }
            case ExpressionKind::Call:
                if (!e.as<CallExpression>().isSystemCall()) killAll();
                break;
            default:
                break;
        }
        e.visit(makeVisitor([&](auto& v, std::derived_from<Expression> auto& inner) {
            if ((const Expression*)&inner == &e) {
                v.visitDefault(inner);
                return;
            }
            expr(inner);
        }));
    }
    // On the left of an assignment only the selects' indexes are read.
    void exprLhs(const Expression& e) {
        if (e.kind == ExpressionKind::ElementSelect) {
            auto& s = e.as<ElementSelectExpression>();
            exprLhs(s.value());
            expr(s.selector());
            checkIndex(s.value(), s.selector(), nullptr, e);
        } else if (e.kind == ExpressionKind::RangeSelect) {
            auto& s = e.as<RangeSelectExpression>();
            exprLhs(s.value());
            expr(s.left());
            if (s.getSelectionKind() != RangeSelectionKind::Simple) checkIndex(s.value(), s.left(), &s, e);
        } else if (e.kind == ExpressionKind::MemberAccess) {
            exprLhs(e.as<MemberAccessExpression>().value());
        } else if (e.kind == ExpressionKind::Concatenation) {
            for (auto* x : e.as<ConcatenationExpression>().operands()) exprLhs(*x);
        }
    }

    void checkIndex(const Expression& value, const Expression& index, const RangeSelectExpression* part,
                    const Expression& whole) {
        if (!out || !rules.enabled("FML002")) return;
        const Type& t = *value.type;
        if (!t.hasFixedRange()) return;
        ConstantRange r = t.getFixedRange();
        // A constant index is the compiler's business.
        Support s;
        support(index, s);
        if (!s.ok || s.vars.empty() || s.params) return;
        // A signal assigned earlier in this same block has a value we do not follow.
        for (auto* v : s.vars)
            if (blockingInBlock.count(v)) return;
        int64_t width = 1;
        bool up = true;
        if (part) {
            EvalContext wctx(scope);
            ConstantValue wv = part->right().eval(wctx);
            auto wi = wv.isInteger() ? wv.integer().as<int64_t>() : std::nullopt;
            if (!wi || *wi < 1) return;
            width = *wi;
            up = part->getSelectionKind() == RangeSelectionKind::IndexedUp;
        }
        Solver sv{m, scope};
        sv.cache = &fcache;
        auto p = live();
        if (anyParams(p)) return;
        std::string found;
        long long res = sv.run(p, {&index}, [&](EvalContext& ctx, const Cone& cone) {
            ConstantValue v = index.eval(ctx);
            if (v.bad() || !v.isInteger() || v.integer().hasUnknown()) return true;
            ConstantValue v64 = v.convertToInt(64, index.type->isSigned(), false);
            auto i = v64.integer().as<int64_t>();
            if (!i) return true;
            int64_t lo = up ? *i : *i - width + 1, hi = up ? *i + width - 1 : *i;
            if (lo >= r.lower() && hi <= r.upper()) return true;
            found = std::to_string(*i) + " (when " + witness(ctx, cone) + ")";
            return false;
        });
        if (res < 0 || found.empty() || sv.params) return;
        std::string what = value.kind == ExpressionKind::NamedValue
                               ? "'" + std::string(value.as<NamedValueExpression>().symbol.name) + "'"
                               : "the selected value";
        out->push_back({"FML002", index.sourceRange.start(),
                        std::string(part ? "part select can start at " : "index can be ") + found + ", outside the range of " +
                            what + ", " + std::to_string(r.lower()) + " to " + std::to_string(r.upper()),
                        value.kind == ExpressionKind::NamedValue ? lintir::path(value.as<NamedValueExpression>().symbol) : ""});
        (void)whole;
    }

    // ---- statements

    void stmt(const Statement& s) {
        switch (s.kind) {
            case StatementKind::List:
                for (auto* c : s.as<StatementList>().list) stmt(*c);
                return;
            case StatementKind::Block:
                stmt(s.as<BlockStatement>().body);
                return;
            case StatementKind::ExpressionStatement:
                expr(s.as<ExpressionStatement>().expr);
                return;
            case StatementKind::Conditional: {
                auto& c = s.as<ConditionalStatement>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) {
                    for (auto& cc : c.conditions) expr(*cc.expr);
                    killAll();
                    stmt(c.ifTrue);
                    if (c.ifFalse) stmt(*c.ifFalse);
                    return;
                }
                const Expression& ce = *c.conditions[0].expr;
                expr(ce);
                Cond t{Cond::Bool, &ce, true};
                if (out && rules.enabled("FML001") && dead(t))
                    out->push_back({"FML001", ce.sourceRange.start(),
                                    "this condition is never true here: the conditions around it rule it out, "
                                    "so the branch is dead code"});
                push(t);
                stmt(c.ifTrue);
                pop(1);
                if (c.ifFalse) {
                    push({Cond::Bool, &ce, false});
                    stmt(*c.ifFalse);
                    pop(1);
                }
                return;
            }
            case StatementKind::Case: {
                auto& c = s.as<CaseStatement>();
                expr(c.expr);
                if (c.condition == CaseStatementCondition::Inside) {
                    killAll();
                    for (auto& it : c.items) stmt(*it.stmt);
                    if (c.defaultCase) stmt(*c.defaultCase);
                    return;
                }
                // Which items can match, from one walk over the values for the
                // whole case (dead(ic) item by item costs items^2 evaluations).
                // Good while the path is the one it was computed on.
                std::vector<char> deadItem;
                std::vector<const Cond*> at;
                if (out && rules.enabled("FML001") && m.opt.fast) {
                    at = live();
                    deadItem = deadItems(c);
                }
                for (size_t k = 0; k < c.items.size(); k++) {
                    for (auto* x : c.items[k].expressions) expr(*x);
                    Cond ic{Cond::Item, &c.expr, true, &c, k};
                    if (out && rules.enabled("FML001") &&
                        (live() == at && !deadItem.empty() ? deadItem[k] == 1 : dead(ic)))
                        out->push_back({"FML001", c.items[k].expressions[0]->sourceRange.start(),
                                        "this case item never matches here: the selector cannot take its value, "
                                        "or an earlier item takes it, so the branch is dead code"});
                    push(ic);
                    stmt(*c.items[k].stmt);
                    pop(1);
                }
                if (c.defaultCase) {
                    push({Cond::Item, &c.expr, true, &c, c.items.size()});
                    stmt(*c.defaultCase);
                    pop(1);
                }
                return;
            }
            case StatementKind::ForLoop:
            case StatementKind::RepeatLoop:
            case StatementKind::ForeachLoop:
            case StatementKind::WhileLoop:
            case StatementKind::DoWhileLoop:
            case StatementKind::ForeverLoop: {
                // What the body assigns can differ on the next trip round.
                std::set<const ValueSymbol*> assigned;
                s.visit(makeVisitor([&](auto&, const AssignmentExpression& a) {
                    if (auto* t = target(a.left())) assigned.insert(t);
                }));
                for (auto* v : assigned) kill(v);
                loops++;
                s.visit(makeVisitor(
                    [&](auto& v, std::derived_from<Statement> auto& inner) {
                        if ((const Statement*)&inner == &s) {
                            v.visitDefault(inner);
                            return;
                        }
                        stmt(inner);
                    },
                    [&](auto&, const Expression& e) { expr(e); }));
                loops--;
                for (auto* v : assigned) kill(v);
                return;
            }
            case StatementKind::Timed:
                killAll();
                stmt(s.as<TimedStatement>().stmt);
                return;
            case StatementKind::VariableDeclaration: {
                auto& v = s.as<VariableDeclStatement>().symbol;
                if (auto* init = v.getInitializer()) expr(*init);
                blockingInBlock.insert(&v);
                return;
            }
            default:
                killAll();
                s.visit(makeVisitor([&](auto&, const Expression& e) { expr(e); }));
                return;
        }
    }

    // The block's body, with the reset branch of a clocked block marked.
    void block(const ProceduralBlockSymbol& p, bool seq) {
        const Statement* body = &p.getBody();
        if (body->kind == StatementKind::Timed) body = &body->as<TimedStatement>().stmt;
        const Statement* top = body;
        while (top->kind == StatementKind::Block) top = &top->as<BlockStatement>().body;
        if (seq && top->kind == StatementKind::Conditional && regs) {
            auto& c = top->as<ConditionalStatement>();
            if (c.conditions.size() == 1 && !c.conditions[0].pattern && resetLike(*c.conditions[0].expr)) {
                const Expression& ce = *c.conditions[0].expr;
                expr(ce);
                push({Cond::Bool, &ce, true});
                inReset = true;
                stmt(c.ifTrue);
                inReset = false;
                pop(1);
                if (c.ifFalse) {
                    push({Cond::Bool, &ce, false});
                    stmt(*c.ifFalse);
                    pop(1);
                }
                return;
            }
        }
        stmt(*body);
    }

    // A one-bit signal named like a reset.
    static bool resetLike(const Expression& e) {
        Support s;
        support(e, s);
        if (!s.ok || s.vars.size() != 1) return false;
        auto* v = *s.vars.begin();
        if (v->getType().getBitWidth() != 1) return false;
        std::string n(v->name);
        for (char& ch : n) ch = (char)std::tolower((unsigned char)ch);
        return n.find("rst") != std::string::npos || n.find("reset") != std::string::npos;
    }
};

// ------------------------------------------------------------ driver

struct Proc {
    const ProceduralBlockSymbol* sym;
    bool seq;
};

inline bool inGenerate(const Symbol& s) {
    for (const Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
        const Symbol& p = sc->asSymbol();
        if (p.kind == SymbolKind::GenerateBlock) return true;
        if (p.kind == SymbolKind::InstanceBody) return false;
    }
    return false;
}

inline void collect(const Symbol& root, std::vector<Proc>& procs, std::vector<const ContinuousAssignSymbol*>& assigns) {
    root.visit(makeVisitor(
        [&](auto& v, const InstanceSymbol& inst) {
            if (!inst.body.flags.has(InstanceFlags::Uninstantiated)) v.visitDefault(inst);
        },
        [&](auto& v, const GenerateBlockSymbol& g) {
            if (!g.isUninstantiated) v.visitDefault(g);
        },
        [&](auto&, const ProceduralBlockSymbol& p) {
            if (p.procedureKind == ProceduralBlockKind::Initial || p.procedureKind == ProceduralBlockKind::Final)
                return;
            bool seq = p.procedureKind == ProceduralBlockKind::AlwaysFF;
            if (p.procedureKind == ProceduralBlockKind::Always &&
                p.getBody().kind == StatementKind::Timed) {
                auto& tc = p.getBody().as<TimedStatement>().timing;
                std::vector<const SignalEventControl*> evs;
                clocksOf(tc, evs);
                for (auto* e : evs) seq |= e->edge != EdgeKind::None;
            }
            // A block that waits inside its body is a testbench thread.
            bool waits = false;
            const Statement* body = &p.getBody();
            if (body->kind == StatementKind::Timed) body = &body->as<TimedStatement>().stmt;
            body->visit(makeVisitor([&](auto&, const TimedStatement&) { waits = true; },
                                    [&](auto&, const WaitStatement&) { waits = true; },
                                    [&](auto&, const EventTriggerStatement&) { waits = true; }));
            if (p.procedureKind == ProceduralBlockKind::Always && p.getBody().kind != StatementKind::Timed)
                waits = true;  // always without a control: a loop with delays
            if (!waits) procs.push_back({&p, seq});
        },
        [&](auto&, const ContinuousAssignSymbol& ca) { assigns.push_back(&ca); }));
}

inline void run(Compilation& comp, Report& rep, const Options& opt, unsigned threads) {
    bool any = false;
    for (const char* r : ruleIds()) any |= rep.rules.enabled(r);
    if (!any) return;
    std::vector<Proc> procs;
    std::vector<const ContinuousAssignSymbol*> assigns;
    collect(comp.getRoot(), procs, assigns);

    Model m;
    m.opt = opt;
    // Nets with one whole continuous assignment.
    std::unordered_map<const ValueSymbol*, int> writers;
    std::unordered_map<const ValueSymbol*, const Expression*> rhs;
    for (auto* ca : assigns) {
        auto& a = ca->getAssignment();
        if (a.kind != ExpressionKind::Assignment) continue;
        auto& ae = a.as<AssignmentExpression>();
        const ValueSymbol* t = Walk::target(ae.left());
        if (!t) continue;
        writers[t]++;
        if (ae.left().kind == ExpressionKind::NamedValue) rhs[t] = &ae.right();
        else writers[t] += 100;  // part of it: not a single driver
        if (inGenerate(*ca)) m.combInGen.insert(t);
    }
    for (auto& p : procs)
        p.sym->getBody().visit(makeVisitor([&](auto&, const AssignmentExpression& a) {
            if (auto* t = Walk::target(a.left())) writers[t] += 100;
        }));
    for (auto& [v, n] : writers)
        if (n == 1 && rhs.count(v)) m.comb[v] = rhs[v];

    // Reachable values of small registers with a reset.
    std::vector<std::vector<RegAssign>> regAs(procs.size());
    std::vector<std::set<const ValueSymbol*>> loopRegs(procs.size());
    std::vector<std::deque<Cond>> keep(procs.size());
    for (size_t i = 0; i < procs.size(); i++) {
        if (!procs[i].seq) continue;
        Walk w(m, rep.rules, *procs[i].sym);
        w.regs = &regAs[i];
        w.block(*procs[i].sym, true);
        loopRegs[i] = w.loopRegs;
        keep[i] = std::move(w.store);  // the conds the paths point to
    }
    std::unordered_map<const ValueSymbol*, std::vector<const RegAssign*>> byReg;
    std::unordered_map<const ValueSymbol*, size_t> owner;
    std::set<const ValueSymbol*> bad;
    for (size_t i = 0; i < procs.size(); i++)
        for (auto& ra : regAs[i]) {
            auto [it, fresh] = owner.emplace(ra.reg, i);
            if (!fresh && it->second != i) bad.insert(ra.reg);
            if (loopRegs[i].count(ra.reg)) bad.insert(ra.reg);
            byReg[ra.reg].push_back(&ra);
        }
    std::vector<const ValueSymbol*> regsList;
    for (auto& [v, as] : byReg) {
        if (bad.count(v) || writers[v] != 100 * (int)as.size()) continue;  // written elsewhere too
        const Type& t = v->getType();
        if (!t.isIntegral() || t.getBitWidth() > opt.maxRegBits) continue;
        bool hasReset = false, fits = true;
        for (auto* ra : as) {
            hasReset |= ra->resetBranch;
            if (ra->resetBranch) continue;
            Solver sv{m, *procs[owner[v]].sym};
            sv.dry = true;
            sv.slack = 8;
            fits &= sv.run(ra->path, {ra->rhs}, [](EvalContext&, const Cone&) { return true; }) >= 0;
        }
        if (hasReset && fits) regsList.push_back(v);
    }
    std::set<const ValueSymbol*> paramRegs;  // a parameter decides its value: not reported as stuck
    std::sort(regsList.begin(), regsList.end(), [](auto* a, auto* b) {
        return std::tie(a->location, a->name) < std::tie(b->location, b->name);
    });
    // Fixed point. A register whose set cannot be computed drops out, and the
    // rest start again without it.
    std::set<const ValueSymbol*> active(regsList.begin(), regsList.end());
    bool settled = false;
    for (int round = 0; round < 32 && !settled; round++) {
        m.domain.clear();
        for (auto* v : regsList)
            if (active.count(v)) m.domain[v] = {};
        std::set<const ValueSymbol*> failed;
        // Reset values first.
        for (auto* v : regsList) {
            if (!active.count(v)) continue;
            for (auto* ra : byReg[v]) {
                if (!ra->resetBranch) continue;
                Support s;
                support(*ra->rhs, s);
                EvalContext rctx(*procs[owner[v]].sym);
                ConstantValue cv = s.vars.empty() && s.ok ? ra->rhs->eval(rctx) : ConstantValue();
                std::optional<uint64_t> x;
                if (!cv.bad() && cv.isInteger())
                    x = cv.convertToInt(v->getType().getBitWidth(), false, false).integer().as<uint64_t>();
                if (!x) {
                    failed.insert(v);
                    break;
                }
                auto& d = m.domain[v];
                if (std::find(d.begin(), d.end(), *x) == d.end()) d.push_back(*x);
            }
        }
        bool converged = false;
        std::set<const ValueSymbol*> full;  // every value reached: nothing more to learn
        for (int it = 0; it < 300 && failed.empty(); it++) {
            // Every register from the same sets (in parallel), then merged in order.
            std::vector<const ValueSymbol*> todo;
            for (auto* v : regsList)
                if (active.count(v) && !full.count(v)) todo.push_back(v);
            std::vector<std::vector<uint64_t>> adds(todo.size());
            std::vector<char> bad(todo.size(), 0), usedParams(todo.size(), 0);
            par::parallelFor(todo.size(), threads, [&](size_t k) {
                const ValueSymbol* v = todo[k];
                bitwidth_t w = v->getType().getBitWidth();
                const auto& cur = m.domain.find(v)->second;
                std::vector<char> seen(size_t(1) << w, 0);
                size_t have = 0;
                for (uint64_t x : cur)
                    if (!seen[x]) seen[x] = 1, have++;
                for (auto* ra : byReg.find(v)->second) {
                    if (ra->resetBranch) continue;
                    Solver sv{m, *procs[owner.find(v)->second].sym};
                    long long r = sv.run(ra->path, {ra->rhs}, [&](EvalContext& ctx, const Cone&) {
                        ConstantValue cv = ra->rhs->eval(ctx);
                        if (cv.bad() || !cv.isInteger() || cv.integer().hasUnknown()) return true;
                        auto x = cv.convertToInt(w, false, false).integer().as<uint64_t>();
                        if (x && !seen[*x]) {
                            seen[*x] = 1;
                            have++;
                            adds[k].push_back(*x);
                        }
                        return have < seen.size();  // all values: stop
                    });
                    usedParams[k] |= sv.params;
                    if (r < 0) {
                        bad[k] = 1;
                        return;
                    }
                    if (have == seen.size()) return;
                }
            });
            bool grew = false;
            for (size_t k = 0; k < todo.size(); k++) {
                const ValueSymbol* v = todo[k];
                if (usedParams[k]) paramRegs.insert(v);
                if (bad[k]) {
                    failed.insert(v);
                    continue;
                }
                auto& d = m.domain[v];
                for (uint64_t x : adds[k]) d.push_back(x);
                grew |= !adds[k].empty();
                if (d.size() == (size_t(1) << v->getType().getBitWidth())) full.insert(v);
            }
            if (!failed.empty()) break;
            if (!grew) {
                converged = true;
                break;
            }
        }
        // Registers whose set could not be computed drop out; the rest start again.
        for (auto* v : failed) active.erase(v);
        if (failed.empty()) {
            if (!converged) active.clear();  // no fixed point in time: trust nothing
            settled = true;
        }
    }
    if (!settled) active.clear();
    for (auto it = m.domain.begin(); it != m.domain.end();)
        it = active.count(it->first) ? std::next(it) : m.domain.erase(it);
    for (auto& [v, d] : m.domain) std::sort(d.begin(), d.end());

    std::vector<std::vector<Found>> found(procs.size() + assigns.size() + 1);
    // FML003: one reachable value, and something tries to change it.
    if (rep.rules.enabled("FML003"))
        for (auto* v : regsList) {
            auto d = m.domain.find(v);
            if (d == m.domain.end() || d->second.size() != 1 || paramRegs.count(v)) continue;
            bool other = false;
            for (auto* ra : byReg[v]) other |= !ra->resetBranch;
            if (!other) continue;
            found.back().push_back({"FML003", v->location,
                                    "register '" + std::string(v->name) + "' never leaves its reset value " +
                                        std::to_string(d->second[0]) +
                                        ": no assignment after reset can give it another value",
                                    lintir::path(*v)});
        }

    par::parallelFor(procs.size() + assigns.size(), threads, [&](size_t i) {
        if (i < procs.size()) {
            Walk w(m, rep.rules, *procs[i].sym);
            w.out = &found[i];
            w.block(*procs[i].sym, procs[i].seq);
        } else {
            auto* ca = assigns[i - procs.size()];
            Walk w(m, rep.rules, *ca);
            w.out = &found[i];
            w.expr(ca->getAssignment());
        }
    });
    for (auto& v : found)
        for (Found& f : v) rep.add(f.rule, f.loc, std::move(f.text), std::move(f.signal));
}

}  // namespace lintformal
