// solve.hpp - elaboration of a PSS model and the scenario solver of mpss.
//
// A Design is the elaborated model: the component tree, its pools, and which
// actions produce or consume each flow object type. It is read-only once
// built, so any number of threads can solve scenarios against it.
//
// A Scenario is one solved test: the tree of action instances (sequences and
// parallel blocks of atomic actions), the flow objects bound between them,
// the resource instances they hold, and a value for every random field.
//
// Solving one scenario:
//   1. expand the root action's activity (select / repeat / if are decided
//      here; a decision that reads random fields solves that action first);
//   2. bind every input to an output: a buffer to an action that ran
//      before, a stream to an action running in parallel. When no action
//      in the activity fits, one that produces the object is inferred and
//      added;
//   3. collect the constraints: action, struct and flow object constraints,
//      inline "with" constraints, and for resources that two actions
//      running at the same time never lock the same instance;
//   4. solve. Variables that share no constraint are solved apart. Within a
//      group each variable's range is halved again and again, keeping a half
//      only while every constraint can still hold there (interval
//      arithmetic, the scheme of msim's randomize() solver in rand.inc), and
//      backtracking into the other half on a dead end.
// A dead end overall starts again with other random choices.
#pragma once
#include <algorithm>
#include <functional>
#include <numeric>

#include "../interval.hpp"
#include "parse.hpp"

namespace pss {

// --------------------------------------------------------------------- random
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed = 0) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    // Uniform in [0, n), n >= 1.
    i128 below(i128 n) {
        if (n <= 1) return 0;
        unsigned __int128 r = ((unsigned __int128)next() << 64) | next();
        return (i128)(r % (unsigned __int128)n);
    }
    static uint64_t mix(uint64_t a, uint64_t b) {
        Rng r(a * 0x100000001b3ull ^ (b + 0x9e3779b97f4a7c15ull));
        r.next();
        return r.next();
    }
};

// ------------------------------------------------------------------ intervals
// The interval rules are the ones msim's randomize() solver uses.
using ival::Op;
using ival::IV;
using ival::ivOp;
using ival::ivOf;
using ival::ivEmpty;

inline Op unOp(const std::string& s) { return s == "-" ? Op::Neg : s == "!" ? Op::LNot : Op::BNot; }
inline Op binOp(const std::string& s) {
    static const std::map<std::string, Op> t = {
        {"+", Op::Add}, {"-", Op::Sub}, {"*", Op::Mul}, {"/", Op::Div}, {"%", Op::Mod}, {"<<", Op::Shl},
        {">>", Op::Shr}, {"<", Op::Lt}, {"<=", Op::Le}, {">", Op::Gt}, {">=", Op::Ge}, {"==", Op::Eq},
        {"!=", Op::Ne}, {"&", Op::BAnd}, {"|", Op::BOr}, {"^", Op::BXor}, {"&&", Op::LAnd}, {"||", Op::LOr},
        {"**", Op::Pow}};
    return t.at(s);
}

// ----------------------------------------------------------------- the design
struct CompInst {
    TypeDecl* type = nullptr;
    std::string path;
    CompInst* parent = nullptr;
    std::vector<std::pair<std::string, CompInst*>> kids;
    std::map<std::string, i128> attrs;
    std::map<std::string, std::string> sattrs;   // string fields
    std::vector<int> pools;
    int id = 0;
};
struct PoolInst {
    TypeDecl* type;
    i128 size;
    std::string name;
    CompInst* owner;
};

using Locals = std::vector<std::pair<std::string, i128>>;

struct Design {
    Model& m;
    std::vector<std::unique_ptr<CompInst>> comps;
    CompInst* root = nullptr;
    std::vector<PoolInst> pools;
    std::map<const TypeDecl*, std::vector<CompInst*>> byType;
    std::map<const TypeDecl*, std::vector<std::pair<TypeDecl*, std::string>>> producers, consumers;
    std::map<std::string, std::vector<TypeDecl*>> bySimple;
    TypeDecl* rootAction = nullptr;

    explicit Design(Model& model) : m(model) {}

    // A type by name as written inside `ctx`: enclosing scopes first, then
    // the full name, then a unique simple name.
    TypeDecl* lookup(const std::string& name, const TypeDecl* ctx) const {
        if (ctx) {
            std::string q = ctx->qname;
            for (;;) {
                auto it = m.byName.find(q + "::" + name);
                if (it != m.byName.end()) return it->second;
                size_t p = q.rfind("::");
                if (p == std::string::npos) break;
                q.resize(p);
            }
        }
        auto it = m.byName.find(name);
        if (it != m.byName.end()) return it->second;
        size_t p = name.rfind("::");
        auto s = bySimple.find(p == std::string::npos ? name : name.substr(p + 2));
        if (s != bySimple.end()) {
            if (p == std::string::npos) return s->second.front();
            for (TypeDecl* t : s->second)
                if (t->qname.size() >= name.size() && t->qname.compare(t->qname.size() - name.size(), name.size(), name) == 0)
                    return t;
        }
        return nullptr;
    }
    TypeRef resolved(TypeRef t) const {
        for (int i = 0; i < 16 && t.k == TypeRef::Named; i++) {
            auto it = m.typedefs.find(t.name);
            size_t q = t.name.rfind("::");
            if (it == m.typedefs.end() && q != std::string::npos) it = m.typedefs.find(t.name.substr(q + 2));
            if (it == m.typedefs.end()) break;
            t = it->second;
        }
        return t;
    }
    TypeDecl* declOf(const TypeRef& t, const TypeDecl* ctx, Loc l) const {
        TypeDecl* d = lookup(t.name, ctx);
        if (!d) m.fail(l, "unknown type '" + t.name + "'");
        return d;
    }
    const std::pair<std::string, i128>* enumItem(const std::string& n, const TypeDecl* only = nullptr) const {
        for (TypeDecl* t : m.all)
            if (t->k == TypeDecl::Enum && (!only || t == only))
                for (auto& v : t->values)
                    if (v.first == n) return &v;
        return nullptr;
    }

    // A constant expression: literals, enum items, package constants, the
    // given locals, and (with a component) its attributes.
    i128 constEval(const Expr& e, const Locals* loc = nullptr, const CompInst* comp = nullptr, int depth = 0) const {
        auto sub = [&](const EP& x) { return constEval(*x, loc, comp, depth); };
        switch (e.k) {
        case Expr::Num: return e.num;
        case Expr::Unary: return ivOp(unOp(e.s), ivOf(sub(e.a[0])), ivOf(0), -1).lo;
        case Expr::Binary: {
            IV r = ivOp(binOp(e.s), ivOf(sub(e.a[0])), ivOf(sub(e.a[1])));
            if (r.empty()) m.fail(e.loc, "division by zero in a constant expression");
            return r.lo;
        }
        case Expr::Cond: return sub(e.a[0]) ? sub(e.a[1]) : sub(e.a[2]);
        case Expr::In: {
            i128 v = sub(e.a[0]);
            for (auto& r : e.ranges) {
                i128 lo = sub(r.first), hi = r.second ? sub(r.second) : lo;
                if (v >= lo && v <= hi) return 1;
            }
            return 0;
        }
        case Expr::Ref: {
            if (!e.scope.empty()) {
                std::string tn;
                for (auto& s : e.scope) tn += (tn.empty() ? "" : "::") + s;
                TypeDecl* t = lookup(tn, nullptr);
                if (t && t->k == TypeDecl::Enum)
                    if (auto* it = enumItem(e.path[0].name, t)) return it->second;
                auto pc = m.consts.find(e.path[0].name);
                if (!t && pc != m.consts.end() && e.path.size() == 1) return constEval(*pc->second, nullptr, nullptr, depth + 1);
                m.fail(e.loc, "unknown name '" + tn + "::" + e.path[0].name + "'");
            }
            const std::string& n = e.path[0].name;
            if (e.path.size() == 1 && !e.path[0].index) {
                if (loc)
                    for (auto it = loc->rbegin(); it != loc->rend(); ++it)
                        if (it->first == n) return it->second;
            }
            if (comp) {
                const CompInst* c = comp;
                std::string key;
                size_t i = n == "comp" ? 1 : 0;
                for (; i < e.path.size(); i++) {
                    std::string k = e.path[i].name;
                    if (e.path[i].index) k += "[" + i128str(sub(e.path[i].index)) + "]";
                    const CompInst* kid = nullptr;
                    if (key.empty())
                        for (auto& kv : c->kids)
                            if (kv.first == k) kid = kv.second;
                    if (kid) { c = kid; continue; }
                    key += (key.empty() ? "" : ".") + k;
                }
                auto it = c->attrs.find(key);
                if (it != c->attrs.end()) return it->second;
                if (n == "comp") m.fail(e.loc, "no component attribute '" + key + "' in " + c->path);
            }
            if (e.path.size() == 1 && !e.path[0].index) {
                if (auto* it = enumItem(n)) return it->second;
                auto c = m.consts.find(n);
                if (c != m.consts.end()) {
                    if (depth > 32) m.fail(e.loc, "constant '" + n + "' is defined through itself");
                    return constEval(*c->second, nullptr, nullptr, depth + 1);
                }
            }
            m.fail(e.loc, "'" + n + "' is not a constant here");
        }
        default: m.fail(e.loc, "not a constant expression");
        }
    }

    int poolFor(const CompInst* c, const TypeDecl* t) const {
        for (; c; c = c->parent)
            for (int p : c->pools)
                if (pools[p].type == t) return p;
        return -1;
    }

    // --------------------------------------------------------- elaboration
    void flatten(TypeDecl* t, int depth = 0) {
        if (t->flattened) return;
        t->flattened = true;
        if (t->super.empty()) return;
        TypeDecl* s = lookup(t->super, t->owner ? t->owner : t);
        if (!s || s == t) m.fail(t->loc, "unknown base type '" + t->super + "'");
        if (s->k != t->k) m.fail(t->loc, "'" + t->super + "' is not the same kind of type as " + t->qname);
        if (depth > 64) m.fail(t->loc, "inheritance cycle at " + t->qname);
        flatten(s, depth + 1);
        std::vector<Field> f = s->fields;
        for (auto& x : t->fields) {
            for (auto& y : f)
                if (y.name == x.name) m.fail(x.loc, "'" + x.name + "' is already declared in base type " + s->qname);
            f.push_back(x);
        }
        t->fields = std::move(f);
        std::vector<CP> c = s->cons;
        c.insert(c.end(), t->cons.begin(), t->cons.end());
        t->cons = std::move(c);
        if (t->activity.empty()) t->activity = s->activity;
        std::vector<Exec> x;
        for (auto& e : s->execs) {
            bool over = false;
            for (auto& d : t->execs) over |= d.kind == e.kind && d.lang == e.lang;
            if (!over) x.push_back(e);
        }
        x.insert(x.end(), t->execs.begin(), t->execs.end());
        t->execs = std::move(x);
    }

    void declAttrs(CompInst* c, const std::string& key, TypeRef tr, const Field& f) {
        tr = resolved(tr);
        if (tr.k == TypeRef::Named) {
            TypeDecl* d = declOf(tr, c->type, f.loc);
            if (d->k == TypeDecl::Enum) c->attrs[key] = d->values.empty() ? 0 : d->values[0].second;
            else if (d->k == TypeDecl::Struct)
                for (auto& sf : d->fields) declAttrs(c, key + "." + sf.name, sf.type, sf);
            else m.fail(f.loc, "'" + f.name + "' cannot be a component field of type " + d->qname);
        } else if (tr.k != TypeRef::String) {
            c->attrs[key] = 0;
        }
    }

    CompInst* instantiate(TypeDecl* t, CompInst* parent, const std::string& name, int depth) {
        if (depth > 64) m.fail(t->loc, "component " + t->qname + " contains itself");
        auto up = std::make_unique<CompInst>();
        CompInst* c = up.get();
        c->type = t;
        c->parent = parent;
        c->path = parent ? parent->path + "." + name : name;
        c->id = (int)comps.size();
        comps.push_back(std::move(up));
        byType[t].push_back(c);
        for (auto& f : t->fields) {
            if (f.k == Field::Pool) {
                TypeDecl* pt = declOf(resolved(f.type), t, f.loc);
                if (!pt->isFlow() && pt->k != TypeDecl::Resource)
                    m.fail(f.loc, "a pool holds buffer, stream, state or resource objects, not " + pt->qname);
                i128 size = f.poolSize ? constEval(*f.poolSize, nullptr, c) : 1;
                if (size < 1) m.fail(f.loc, "pool size must be at least 1");
                c->pools.push_back((int)pools.size());
                pools.push_back({pt, size, f.name, c});
                continue;
            }
            if (f.k != Field::Data) m.fail(f.loc, "'" + f.name + "': input, output, lock and share belong in actions");
            TypeRef tr = resolved(f.type);
            TypeDecl* d = tr.k == TypeRef::Named ? declOf(tr, t, f.loc) : nullptr;
            i128 n = f.dim ? constEval(*f.dim, nullptr, c) : -1;
            if (n > 65536) m.fail(f.loc, "array '" + f.name + "' is too large");
            for (i128 i = 0; i < (n < 0 ? 1 : n); i++) {
                std::string en = n < 0 ? f.name : f.name + "[" + i128str(i) + "]";
                if (d && d->k == TypeDecl::Component) {
                    c->kids.push_back({en, instantiate(d, c, en, depth + 1)});
                } else {
                    declAttrs(c, en, tr, f);
                    if (tr.k == TypeRef::String) {
                        if (f.init && f.init->k != Expr::Str) m.fail(f.loc, "a string field needs a string literal");
                        c->sattrs[en] = f.init ? f.init->s : "";
                        continue;
                    }
                    if (f.init && c->attrs.count(en)) c->attrs[en] = constEval(*f.init, nullptr, c);
                }
            }
        }
        return c;
    }

    void runInit(CompInst* c, const char* kind, const char* kind2) {
        for (auto& x : c->type->execs) {
            if (x.kind != kind && x.kind != kind2) continue;
            for (auto& s : x.stmts) {
                if (s->k != Stmt::Assign || s->op != "=" || s->lhs->k != Expr::Ref)
                    m.fail(s->loc, "an exec " + x.kind + " block may only hold assignments to component fields");
                CompInst* t = c;
                std::string key;
                for (auto& pe : s->lhs->path) {
                    std::string k = pe.name;
                    if (pe.index) k += "[" + i128str(constEval(*pe.index, nullptr, c)) + "]";
                    CompInst* kid = nullptr;
                    if (key.empty())
                        for (auto& kv : t->kids)
                            if (kv.first == k) kid = kv.second;
                    if (kid) { t = kid; continue; }
                    key += (key.empty() ? "" : ".") + k;
                }
                if (!t->attrs.count(key)) m.fail(s->loc, "no component field '" + key + "' in " + t->path);
                t->attrs[key] = constEval(*s->e, nullptr, c);
            }
        }
    }
    void initDown(CompInst* c) {
        runInit(c, "init_down", "init_down");
        for (auto& k : c->kids) initDown(k.second);
    }
    void initUp(CompInst* c) {
        for (auto& k : c->kids) initUp(k.second);
        runInit(c, "init", "init_up");
    }

    void elaborate(const std::string& top, const std::string& rootName) {
        for (TypeDecl* t : m.all) {
            m.byName[t->qname] = t;
            bySimple[t->name].push_back(t);
        }
        for (auto& f : m.funcs) m.funcByName[f.name] = &f;
        // Enum values first: widths and sizes may use them.
        for (TypeDecl* t : m.all)
            if (t->k == TypeDecl::Enum) {
                i128 next = 0;
                for (auto& it : t->items) {
                    if (it.second) next = constEval(*it.second);
                    t->values.push_back({it.first, next++});
                }
                if (t->values.empty()) m.fail(t->loc, "enum " + t->qname + " has no items");
            }
        for (TypeDecl* t : m.all) flatten(t);
        for (TypeDecl* t : m.all) {
            if (t->k != TypeDecl::Action) {
                if (!t->activity.empty()) m.fail(t->loc, "only an action can have an activity");
                continue;
            }
            for (auto& f : t->fields) {
                if (f.k == Field::Data || f.k == Field::Pool) continue;
                TypeDecl* d = declOf(resolved(f.type), t, f.loc);
                if (f.dim) m.fail(f.loc, "arrays of flow object or resource references are not supported yet");
                bool res = f.k == Field::Lock || f.k == Field::Share;
                if (res != (d->k == TypeDecl::Resource) || (!res && !d->isFlow()))
                    m.fail(f.loc, "'" + f.name + "': " + d->qname + (res ? " is not a resource" : " is not a buffer, stream or state"));
                if (d->k == TypeDecl::State) m.fail(f.loc, "state objects are not supported yet");
                if (t->isAbstract) continue;
                if (f.k == Field::Output) producers[d].push_back({t, f.name});
                if (f.k == Field::Input) consumers[d].push_back({t, f.name});
            }
        }
        TypeDecl* topT = lookup(top, nullptr);
        if (!topT || topT->k != TypeDecl::Component) {
            // Without a pss_top: the one component no other component holds.
            std::set<TypeDecl*> held;
            std::vector<TypeDecl*> cs;
            for (TypeDecl* t : m.all)
                if (t->k == TypeDecl::Component) {
                    cs.push_back(t);
                    for (auto& f : t->fields)
                        if (f.k == Field::Data && resolved(f.type).k == TypeRef::Named)
                            if (TypeDecl* d = lookup(resolved(f.type).name, t)) held.insert(d);
                }
            std::vector<TypeDecl*> roots;
            for (TypeDecl* t : cs)
                if (!held.count(t)) roots.push_back(t);
            if (top != "pss_top" || roots.size() != 1)
                throw Error("no component '" + top + "' (name the root component with -top)");
            topT = roots[0];
        }
        root = instantiate(topT, nullptr, topT->name, 0);
        initDown(root);
        initUp(root);
        if (!rootName.empty()) {
            rootAction = lookup(rootName, topT);
            if (!rootAction || rootAction->k != TypeDecl::Action) throw Error("no action '" + rootName + "' (-root)");
        } else {
            std::vector<TypeDecl*> as;
            for (auto& n : topT->nested)
                if (n->k == TypeDecl::Action && !n->isAbstract) as.push_back(n.get());
            for (TypeDecl* a : as)
                if (a->name == "entry") rootAction = a;
            if (!rootAction) {
                std::vector<TypeDecl*> compound;
                for (TypeDecl* a : as)
                    if (!a->activity.empty()) compound.push_back(a);
                if (compound.size() == 1) rootAction = compound[0];
                else if (as.size() == 1) rootAction = as[0];
                else throw Error("name the root action with -root <component>::<action>");
            }
        }
        if (rootAction->isAbstract) throw Error("the root action " + rootAction->qname + " is abstract");
    }
};

// ---------------------------------------------------------------- a scenario
struct CN {
    Op op = Op::Const;
    int a = -1, b = -1, c = -1;
    i128 v = 0;
};

struct Var {
    std::vector<std::pair<i128, i128>> dom;   // sorted, disjoint
    std::string name;
    const TypeDecl* enumT = nullptr;
    int width = 32;
    bool sgn = false, isBool = false, isStr = false, isRand = false;
    std::string str;
    i128 val = 0;
};

using StructList = std::vector<std::pair<const TypeDecl*, std::string>>;

struct Obj {
    const TypeDecl* type = nullptr;
    int pool = -1;
    std::map<std::string, int> vars;
    StructList structs;
    int idVar = -1;
    int producer = -1, consumers = 0;
    int ai = -1;          // resource claim: the claiming action
    bool lock = false;
    std::string name;
};

struct WithCons {
    CP c;
    int outerAi;
    Locals locals;
    bool own;             // an activity-level constraint of the action itself
};

struct AI {
    const TypeDecl* type = nullptr;
    const CompInst* comp = nullptr;
    int parent = -1;
    std::string name;
    std::map<std::string, int> vars;
    StructList structs;
    std::map<std::string, int> refs;                 // flow / resource field -> object (-1 unbound)
    std::map<std::string, std::vector<int>> handles; // action handle -> its traversals
    std::vector<WithCons> extra;
    int node = -1;
    bool inferred = false;
};

struct Node {
    enum K { Seq, Par, Leaf } k = Seq;
    int ai = -1;
    std::vector<int> kids;
    int parent = -1;
};

struct Scope {
    int ai = -1, obj = -1;
    std::string prefix;
    const TypeDecl* ptype = nullptr;   // the struct type `prefix` leads to
    Locals locals;
    std::vector<std::pair<std::string, EP>> aliases;
    const Scope* outer = nullptr;
    int sel = 0;
};

struct Res {
    enum K { NotFound, Unbound, Const, VarRef, Str, Array } k = NotFound;
    i128 v = 0;
    int var = -1;
    std::string s;
};

struct Retry {
    std::string why;
};
struct UnboundRef {};

struct Scenario {
    const Design& d;
    Rng rng;
    std::vector<Var> vars;
    std::vector<Obj> objs;
    std::vector<AI> ais;
    std::vector<Node> nodes;
    std::vector<CN> ar;
    std::vector<std::pair<int, const Act*>> binds;
    std::vector<Locals> bindLocals;
    int root = -1;
    int index = 0, attempt = 0;
    uint64_t seed = 0;
    long checks = 0;

    explicit Scenario(const Design& design) : d(design) {}

    // ------------------------------------------------------------ variables
    int newVar(const std::string& name) {
        vars.emplace_back();
        vars.back().name = name;
        return (int)vars.size() - 1;
    }
    void domFromRanges(Var& v, const std::vector<std::pair<EP, EP>>& rs, const CompInst* comp, Loc l) {
        i128 lo0 = v.dom[0].first, hi0 = v.dom[0].second;
        std::vector<std::pair<i128, i128>> r;
        for (auto& x : rs) {
            i128 lo = d.constEval(*x.first, nullptr, comp), hi = x.second ? d.constEval(*x.second, nullptr, comp) : lo;
            lo = std::max(lo, lo0);
            hi = std::min(hi, hi0);
            if (lo <= hi) r.push_back({lo, hi});
        }
        std::sort(r.begin(), r.end());
        std::vector<std::pair<i128, i128>> o;
        for (auto& x : r) {
            if (!o.empty() && x.first <= o.back().second + 1) o.back().second = std::max(o.back().second, x.second);
            else o.push_back(x);
        }
        if (o.empty()) d.m.fail(l, "'" + v.name + "' has an empty domain");
        v.dom = o;
    }
    void declType(std::map<std::string, int>& vm, StructList& structs, const std::string& key, TypeRef tr, bool rand,
                  const Field& f, const TypeDecl* ctx, const CompInst* comp, const std::string& owner, int depth = 0) {
        tr = d.resolved(tr);
        if (tr.k == TypeRef::Named) {
            TypeDecl* t = d.declOf(tr, ctx, f.loc);
            if (t->k == TypeDecl::Action) return;   // an action handle
            if (t->k == TypeDecl::Struct) {
                if (depth > 32) d.m.fail(f.loc, "struct " + t->qname + " contains itself");
                structs.push_back({t, key + "."});
                for (auto& sf : t->fields) declField(vm, structs, key + ".", sf, rand && sf.rand, t, comp, owner, depth + 1);
                return;
            }
            if (t->k != TypeDecl::Enum) d.m.fail(f.loc, "'" + f.name + "' cannot be a field of type " + t->qname);
            int id = newVar(owner + key);
            Var& v = vars[id];
            v.enumT = t;
            v.isRand = rand;
            std::vector<i128> vals;
            for (auto& it : t->values) vals.push_back(it.second);
            std::sort(vals.begin(), vals.end());
            vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
            if (rand) {
                for (i128 x : vals) {
                    if (!v.dom.empty() && v.dom.back().second + 1 == x) v.dom.back().second = x;
                    else v.dom.push_back({x, x});
                }
            } else {
                i128 x = f.init ? d.constEval(*f.init, nullptr, comp) : t->values[0].second;
                v.dom = {{x, x}};
            }
            vm[key] = id;
            return;
        }
        int id = newVar(owner + key);
        Var& v = vars[id];
        v.isRand = rand;
        vm[key] = id;
        if (tr.k == TypeRef::String) {
            v.isStr = true;
            if (f.init) {
                if (f.init->k != Expr::Str) d.m.fail(f.loc, "a string field needs a string literal");
                v.str = f.init->s;
            }
            v.dom = {{0, 0}};
            return;
        }
        if (tr.k == TypeRef::Bool) {
            v.isBool = true;
            v.width = 1;
            v.dom = {{0, 1}};
        } else {
            i128 w = tr.width ? d.constEval(*tr.width, nullptr, comp) : (tr.k == TypeRef::Bit ? 1 : 32);
            if (w < 1 || w > 64) d.m.fail(f.loc, "'" + f.name + "': width must be 1 to 64");
            v.width = (int)w;
            v.sgn = tr.k == TypeRef::Int;
            if (v.sgn) v.dom = {{-((i128)1 << (w - 1)), ((i128)1 << (w - 1)) - 1}};
            else v.dom = {{0, ((i128)1 << w) - 1}};
            if (!tr.dom.empty()) domFromRanges(v, tr.dom, comp, f.loc);
        }
        if (!rand) {
            i128 x = f.init ? d.constEval(*f.init, nullptr, comp) : (v.dom[0].first > 0 ? v.dom[0].first : 0);
            v.dom = {{x, x}};
        }
    }
    void declField(std::map<std::string, int>& vm, StructList& structs, const std::string& prefix, const Field& f,
                   bool rand, const TypeDecl* ctx, const CompInst* comp, const std::string& owner, int depth = 0) {
        i128 n = f.dim ? d.constEval(*f.dim, nullptr, comp) : -1;
        if (n > 65536) d.m.fail(f.loc, "array '" + f.name + "' is too large");
        if (n < 0) declType(vm, structs, prefix + f.name, f.type, rand, f, ctx, comp, owner, depth);
        else
            for (i128 i = 0; i < n; i++)
                declType(vm, structs, prefix + f.name + "[" + i128str(i) + "]", f.type, rand, f, ctx, comp, owner, depth);
    }

    int newObj(const TypeDecl* t, const CompInst* comp, const std::string& name, Loc l) {
        Obj o;
        o.type = t;
        o.name = name;
        o.pool = d.poolFor(comp, t);
        if (o.pool < 0)
            d.m.fail(l, "no pool of " + t->qname + " is visible from component " + comp->path + " (for '" + name + "')");
        int id = (int)objs.size();
        objs.push_back(std::move(o));
        std::map<std::string, int> vm;
        StructList sl;
        for (auto& f : t->fields)
            if (f.k == Field::Data) declField(vm, sl, "", f, f.rand, t, comp, name + ".");
        if (t->k == TypeDecl::Resource) {
            int v = newVar(name + ".instance_id");
            vars[v].dom = {{0, d.pools[objs[id].pool].size - 1}};
            vars[v].isRand = true;
            vars[v].sgn = true;   // an int, as the standard declares instance_id
            objs[id].idVar = v;
        }
        objs[id].vars = std::move(vm);
        objs[id].structs = std::move(sl);
        return id;
    }

    // ----------------------------------------------------------- the tree
    int newNode(Node::K k, int ai, int parent) {
        Node n;
        n.k = k;
        n.ai = ai;
        n.parent = parent;
        nodes.push_back(n);
        int id = (int)nodes.size() - 1;
        if (parent >= 0) nodes[parent].kids.push_back(id);
        return id;
    }
    // How a stands to b: 'b' a ends before b starts, 'a' after, 'p' they
    // run in parallel, 'n' one contains the other.
    char relation(int a, int b) const {
        std::vector<int> pa, pb;
        for (int x = a; x >= 0; x = nodes[x].parent) pa.push_back(x);
        for (int x = b; x >= 0; x = nodes[x].parent) pb.push_back(x);
        std::reverse(pa.begin(), pa.end());
        std::reverse(pb.begin(), pb.end());
        size_t i = 0;
        while (i < pa.size() && i < pb.size() && pa[i] == pb[i]) i++;
        if (i == pa.size() || i == pb.size()) return 'n';
        const Node& l = nodes[pa[i - 1]];
        if (l.k == Node::Par) return 'p';
        for (int k : l.kids) {
            if (k == pa[i]) return 'b';
            if (k == pb[i]) return 'a';
        }
        return 'n';
    }

    static bool under(const CompInst* c, const CompInst* top) {
        for (; c; c = c->parent)
            if (c == top) return true;
        return false;
    }

    int newAI(const TypeDecl* t, int parent, const std::string& hname, const CompInst* forced, Loc l) {
        if (t->isAbstract) d.m.fail(l, "action " + t->qname + " is abstract and cannot be traversed");
        if (ais.size() > 200000) throw Retry{"the scenario grew past 200000 actions"};
        AI a;
        a.type = t;
        a.parent = parent;
        if (forced) a.comp = forced;
        else if (!t->owner) a.comp = parent >= 0 ? ais[parent].comp : d.root;
        else {
            auto it = d.byType.find(t->owner);
            if (it == d.byType.end()) d.m.fail(l, "component " + t->owner->qname + " of action " + t->name + " is not instantiated");
            std::vector<const CompInst*> c;
            if (parent >= 0)
                for (CompInst* x : it->second)
                    if (under(x, ais[parent].comp)) c.push_back(x);
            if (c.empty()) c.assign(it->second.begin(), it->second.end());
            a.comp = c[(size_t)rng.below((i128)c.size())];
        }
        a.name = parent >= 0 ? ais[parent].name + "." + hname : hname;
        int id = (int)ais.size();
        std::map<std::string, int> vm;
        StructList sl;
        std::map<std::string, int> refs;
        for (auto& f : t->fields) {
            if (f.k == Field::Data) declField(vm, sl, "", f, f.rand, t, a.comp, a.name + ".");
            else if (f.k == Field::Input) refs[f.name] = -1;
            else {
                const TypeDecl* ot = d.declOf(d.resolved(f.type), t, f.loc);
                int o = newObj(ot, a.comp, a.name + "." + f.name, f.loc);
                if (f.k == Field::Output) objs[o].producer = id;
                else { objs[o].ai = id; objs[o].lock = f.k == Field::Lock; }
                refs[f.name] = o;
            }
        }
        a.vars = std::move(vm);
        a.structs = std::move(sl);
        a.refs = std::move(refs);
        ais.push_back(std::move(a));
        return id;
    }

    // ------------------------------------------------------- name resolution
    struct Cursor {
        int ai, obj;
        std::string prefix;
        const TypeDecl* type;
    };
    // Follows e.path from `c`. NotFound only when the first name is no
    // field of c.type.
    Res walk(Cursor c, const Expr& e, const Scope& sc) {
        Res r;
        for (size_t i = 0; i < e.path.size(); i++) {
            const PathElem& pe = e.path[i];
            bool last = i + 1 == e.path.size();
            if (c.type->k == TypeDecl::Resource && pe.name == "instance_id" && c.prefix.empty()) {
                if (!last) d.m.fail(e.loc, "instance_id has no fields");
                r.k = Res::VarRef;
                r.var = objs[c.obj].idVar;
                return r;
            }
            const Field* f = c.type->field(pe.name);
            if (!f || f->k == Field::Pool) {
                if (i == 0) return r;
                d.m.fail(e.loc, "no field '" + pe.name + "' in " + c.type->qname);
            }
            std::string idx;
            if (pe.index) {
                i128 k = compileConst(*pe.index, sc);
                i128 n = f->dim ? d.constEval(*f->dim, nullptr, c.ai >= 0 ? ais[c.ai].comp : nullptr) : -1;
                if (n < 0) d.m.fail(e.loc, "'" + pe.name + "' is not an array");
                if (k < 0 || k >= n) throw Retry{d.m.where(e.loc) + ": index " + i128str(k) + " is outside '" + pe.name + "'"};
                idx = "[" + i128str(k) + "]";
            }
            if (f->k != Field::Data) {
                int o = ais[c.ai].refs[pe.name];
                if (o < 0) { r.k = Res::Unbound; return r; }
                if (last) d.m.fail(e.loc, "'" + pe.name + "' is an object, not a value");
                c = {-1, o, "", objs[o].type};
                continue;
            }
            TypeRef tr = d.resolved(f->type);
            const TypeDecl* t = tr.k == TypeRef::Named ? d.declOf(tr, c.type, f->loc) : nullptr;
            if (f->dim && !pe.index) {
                if (!last) d.m.fail(e.loc, "'" + pe.name + "' is an array; give an index");
                r.k = Res::Array;
                r.v = d.constEval(*f->dim, nullptr, c.ai >= 0 ? ais[c.ai].comp : nullptr);
                return r;
            }
            if (t && t->k == TypeDecl::Action) {
                if (c.ai < 0 || !c.prefix.empty()) d.m.fail(e.loc, "'" + pe.name + "' is not reachable here");
                auto it = ais[c.ai].handles.find(pe.name + idx);
                if (it == ais[c.ai].handles.end() || it->second.empty()) { r.k = Res::Unbound; return r; }
                if (last) d.m.fail(e.loc, "'" + pe.name + "' is an action, not a value");
                int sub = it->second[std::min((size_t)sc.sel, it->second.size() - 1)];
                c = {sub, -1, "", ais[sub].type};
                continue;
            }
            if (t && t->k == TypeDecl::Struct) {
                if (last) d.m.fail(e.loc, "'" + pe.name + "' is a struct, not a value");
                c.prefix += pe.name + idx + ".";
                c.type = t;
                continue;
            }
            if (!last) d.m.fail(e.loc, "'" + pe.name + "' has no fields");
            auto& vm = c.obj >= 0 ? objs[c.obj].vars : ais[c.ai].vars;
            auto it = vm.find(c.prefix + pe.name + idx);
            if (it == vm.end()) d.m.fail(e.loc, "cannot read '" + pe.name + "' here");
            const Var& v = vars[it->second];
            if (v.isStr) { r.k = Res::Str; r.s = v.str; return r; }
            r.k = Res::VarRef;
            r.var = it->second;
            return r;
        }
        d.m.fail(e.loc, "empty reference");
    }

    Res resolveRef(const Expr& e, const Scope& sc) {
        Res r;
        if (!e.scope.empty()) {
            r.k = Res::Const;
            r.v = d.constEval(e);
            return r;
        }
        const std::string& n = e.path[0].name;
        for (const Scope* s = &sc; s; s = s->outer) {
            if (e.path.size() == 1 && !e.path[0].index)
                for (auto it = s->locals.rbegin(); it != s->locals.rend(); ++it)
                    if (it->first == n) { r.k = Res::Const; r.v = it->second; return r; }
            if (!e.path[0].index)
                for (auto it = s->aliases.rbegin(); it != s->aliases.rend(); ++it)
                    if (it->first == n) {
                        Expr x = *it->second;
                        x.path.insert(x.path.end(), e.path.begin() + 1, e.path.end());
                        // The alias was made in this scope, minus itself.
                        Scope inner = *s;
                        inner.aliases.assign(s->aliases.begin(), s->aliases.begin() + (s->aliases.rend() - it - 1));
                        return resolveRef(x, inner);
                    }
            if (s->ai < 0 && s->obj < 0) continue;
            const TypeDecl* top = s->obj >= 0 ? objs[s->obj].type : ais[s->ai].type;
            Scope use = sc;
            use.sel = s->sel;
            if (!s->prefix.empty()) {
                r = walk({s->ai, s->obj, s->prefix, s->ptype}, e, use);
                if (r.k != Res::NotFound) return r;
            }
            r = walk({s->ai, s->obj, "", top}, e, use);
            if (r.k != Res::NotFound) return r;
        }
        const CompInst* comp = nullptr;
        for (const Scope* s = &sc; s && !comp; s = s->outer)
            if (s->ai >= 0) comp = ais[s->ai].comp;
        if (comp && n == "comp" && e.path.size() == 2 && !e.path[1].index) {
            auto it = comp->sattrs.find(e.path[1].name);
            if (it != comp->sattrs.end()) { r.k = Res::Str; r.s = it->second; return r; }
        }
        r.k = Res::Const;
        r.v = d.constEval(e, nullptr, comp);
        return r;
    }

    // ----------------------------------------------------------- compilation
    int node(Op op, int a = -1, int b = -1, int c = -1, i128 v = 0) {
        ar.push_back({op, a, b, c, v});
        return (int)ar.size() - 1;
    }
    int konst(i128 v) { return node(Op::Const, -1, -1, -1, v); }
    bool isConst(int n) const { return ar[n].op == Op::Const; }
    int fold(Op op, int a, int b, i128 x = 0) {
        bool un = op <= Op::BNot;
        if (isConst(a) && (un || isConst(b))) {
            IV r = ivOp(op, ivOf(ar[a].v), un ? ivOf(0) : ivOf(ar[b].v), x);
            if (!r.empty()) return konst(r.lo);
        }
        if (op == Op::LAnd && ((isConst(a) && !ar[a].v) || (isConst(b) && !ar[b].v))) return konst(0);
        if (op == Op::LOr && ((isConst(a) && ar[a].v) || (isConst(b) && ar[b].v))) return konst(1);
        return node(op, a, b, -1, x);
    }
    // An expression as a node; throws UnboundRef when it reads an input not
    // yet bound or an action not yet traversed.
    int compile(const Expr& e, const Scope& sc) {
        switch (e.k) {
        case Expr::Num: return konst(e.num);
        case Expr::Str: d.m.fail(e.loc, "a string cannot be used in an expression");
        case Expr::Call: d.m.fail(e.loc, "function calls are not supported in constraints");
        case Expr::Ref: {
            Res r = resolveRef(e, sc);
            if (r.k == Res::Unbound) throw UnboundRef{};
            if (r.k == Res::Const) return konst(r.v);
            if (r.k == Res::VarRef) {
                const Var& v = vars[r.var];
                if (v.dom.size() == 1 && v.dom[0].first == v.dom[0].second) return konst(v.dom[0].first);
                return node(Op::Var, r.var);
            }
            d.m.fail(e.loc, "'" + e.path.back().name + "' is not a number");
        }
        case Expr::Unary: {
            int a = compile(*e.a[0], sc);
            i128 mask = -1;
            if (e.s == "~" && e.a[0]->k == Expr::Ref) {
                Res r = resolveRef(*e.a[0], sc);
                if (r.k == Res::VarRef && !vars[r.var].sgn) mask = ((i128)1 << vars[r.var].width) - 1;
            }
            return fold(unOp(e.s), a, -1, mask);
        }
        case Expr::Binary: {
            int a = compile(*e.a[0], sc);
            // A constant left side decides && and || without the right side.
            if (isConst(a) && ((e.s == "&&" && !ar[a].v) || (e.s == "||" && ar[a].v))) return konst(e.s == "||");
            int b = compile(*e.a[1], sc);
            return fold(binOp(e.s), a, b);
        }
        case Expr::Cond: {
            int c = compile(*e.a[0], sc);
            if (isConst(c)) return compile(*e.a[ar[c].v ? 1 : 2], sc);
            int a = compile(*e.a[1], sc), b = compile(*e.a[2], sc);
            return node(Op::Cond, c, a, b);
        }
        case Expr::In: {
            int x = compile(*e.a[0], sc);
            int acc = konst(0);
            for (auto& r : e.ranges) {
                int lo = compile(*r.first, sc);
                int t = r.second ? fold(Op::LAnd, fold(Op::Ge, x, lo), fold(Op::Le, x, compile(*r.second, sc))) : fold(Op::Eq, x, lo);
                acc = fold(Op::LOr, acc, t);
            }
            return acc;
        }
        }
        return konst(0);
    }
    i128 compileConst(const Expr& e, const Scope& sc) {
        int n;
        try { n = compile(e, sc); }
        catch (UnboundRef&) { d.m.fail(e.loc, "this must be a constant, but reads an unbound object or action"); }
        if (isConst(n)) return ar[n].v;
        return structValue(n, sc, e.loc);
    }

    // A constraint item into root nodes (each must be true). `guard` is the
    // condition it sits under (-1: none).
    void compileCons(const Cons& c, const Scope& sc, int guard, std::vector<int>& out) {
        auto emit = [&](int n) {
            if (guard >= 0) n = fold(Op::LOr, fold(Op::LNot, guard, -1), n);
            if (isConst(n) && ar[n].v) return;
            out.push_back(n);
        };
        auto both = [&](int g) { return guard < 0 ? g : fold(Op::LAnd, guard, g); };
        switch (c.k) {
        case Cons::Expr_: emit(compile(*c.e, sc)); break;
        case Cons::If: {
            int g = compile(*c.e, sc);
            if (isConst(g)) {
                for (auto& x : ar[g].v ? c.body : c.els) compileCons(*x, sc, guard, out);
                break;
            }
            for (auto& x : c.body) compileCons(*x, sc, both(g), out);
            if (!c.els.empty()) {
                int ng = fold(Op::LNot, g, -1);
                for (auto& x : c.els) compileCons(*x, sc, both(ng), out);
            }
            break;
        }
        case Cons::Foreach: {
            Res r = resolveRef(*c.e, sc);
            if (r.k == Res::Unbound) throw UnboundRef{};
            if (r.k != Res::Array) d.m.fail(c.loc, "foreach needs an array");
            for (i128 i = 0; i < r.v; i++) {
                Scope in = sc;
                in.outer = sc.outer;
                if (!c.idx.empty()) in.locals.push_back({c.idx, i});
                if (!c.it.empty()) {
                    auto el = std::make_shared<Expr>(*c.e);
                    auto ix = std::make_shared<Expr>();
                    ix->num = i;
                    el->path.back().index = ix;
                    in.aliases.push_back({c.it, el});
                }
                for (auto& x : c.body) compileCons(*x, in, guard, out);
            }
            break;
        }
        case Cons::Unique: {
            std::vector<int> items;
            for (auto& x : c.list) {
                if (x->k == Expr::Ref) {
                    Res r = resolveRef(*x, sc);
                    if (r.k == Res::Unbound) throw UnboundRef{};
                    if (r.k == Res::Array) {
                        for (i128 i = 0; i < r.v; i++) {
                            Expr el = *x;
                            auto ix = std::make_shared<Expr>();
                            ix->num = i;
                            el.path.back().index = ix;
                            items.push_back(compile(el, sc));
                        }
                        continue;
                    }
                }
                items.push_back(compile(*x, sc));
            }
            for (size_t i = 0; i < items.size(); i++)
                for (size_t j = i + 1; j < items.size(); j++) emit(fold(Op::Ne, items[i], items[j]));
            break;
        }
        }
    }
    // Soft: an item that reads something unbound is left out.
    void compileSoft(const Cons& c, const Scope& sc, std::vector<int>& out) {
        size_t n = out.size();
        try { compileCons(c, sc, -1, out); }
        catch (UnboundRef&) { out.resize(n); }
    }

    // Every constraint that belongs to one action: its type's, its structs',
    // and those given where it was traversed.
    void actionCons(int ai, std::vector<int>& out) {
        const AI& a = ais[ai];
        size_t m = 1;
        for (auto& h : a.handles) m = std::max(m, h.second.size());
        for (size_t sel = 0; sel < m; sel++) {
            Scope sc;
            sc.ai = ai;
            sc.sel = (int)sel;
            for (auto& c : a.type->cons) compileSoft(*c, sc, out);
            for (auto& w : a.extra)
                if (w.own) {
                    Scope s2 = sc;
                    s2.locals = w.locals;
                    compileSoft(*w.c, s2, out);
                }
        }
        for (auto& st : a.structs) {
            Scope sc;
            sc.ai = ai;
            sc.prefix = st.second;
            sc.ptype = st.first;
            for (auto& c : st.first->cons) compileSoft(*c, sc, out);
        }
        for (auto& w : a.extra) {
            if (w.own) continue;
            Scope outer;
            outer.ai = w.outerAi;
            outer.locals = w.locals;
            Scope sc;
            sc.ai = ai;
            sc.outer = &outer;
            compileSoft(*w.c, sc, out);
        }
    }
    void objectCons(int o, std::vector<int>& out) {
        Scope sc;
        sc.obj = o;
        for (auto& c : objs[o].type->cons) compileSoft(*c, sc, out);
        for (auto& st : objs[o].structs) {
            Scope s2;
            s2.obj = o;
            s2.prefix = st.second;
            s2.ptype = st.first;
            for (auto& c : st.first->cons) compileSoft(*c, s2, out);
        }
    }

    // ---------------------------------------------------------------- solver
    std::vector<IV> hull;

    IV eval(int n) const {
        const CN& c = ar[n];
        switch (c.op) {
        case Op::Const: return {c.v, c.v};
        case Op::Var: return hull[c.a];
        case Op::Neg: case Op::LNot: case Op::BNot: return ivOp(c.op, eval(c.a), IV{0, 0}, c.v);
        case Op::LAnd: {
            IV a = eval(c.a);
            if (!a.empty() && !a.canTrue()) return {0, 0};
            IV b = eval(c.b);
            if (!b.empty() && !b.canTrue()) return {0, 0};
            if (a.empty() || b.empty()) return ivEmpty();
            return ivOp(Op::LAnd, a, b);
        }
        case Op::LOr: {
            IV a = eval(c.a);
            if (!a.empty() && !a.canFalse()) return {1, 1};
            IV b = eval(c.b);
            if (!b.empty() && !b.canFalse()) return {1, 1};
            if (a.empty() || b.empty()) return ivEmpty();
            return ivOp(Op::LOr, a, b);
        }
        case Op::Cond: {
            IV g = eval(c.a);
            if (g.empty()) return g;
            if (!g.canFalse()) return eval(c.b);
            if (!g.canTrue()) return eval(c.c);
            IV a = eval(c.b), b = eval(c.c);
            if (a.empty()) return b;
            if (b.empty()) return a;
            return {std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
        }
        default: return ivOp(c.op, eval(c.a), eval(c.b));
        }
    }
    void varsOf(int n, std::vector<int>& out) const {
        const CN& c = ar[n];
        if (c.op == Op::Const) return;
        if (c.op == Op::Var) { out.push_back(c.a); return; }
        if (c.a >= 0) varsOf(c.a, out);
        if (c.b >= 0) varsOf(c.b, out);
        if (c.c >= 0) varsOf(c.c, out);
    }

    using Dom = std::vector<std::pair<i128, i128>>;
    static i128 countIn(const Dom& l, i128 lo, i128 hi) {
        i128 n = 0;
        for (auto& p : l) {
            i128 a = std::max(p.first, lo), b = std::min(p.second, hi);
            if (a <= b) n += b - a + 1;
        }
        return n;
    }
    // The k-th (from 0) value of l that is >= lo.
    static i128 kth(const Dom& l, i128 lo, i128 k) {
        for (auto& p : l) {
            i128 a = std::max(p.first, lo);
            if (a > p.second) continue;
            i128 n = p.second - a + 1;
            if (k < n) return a + k;
            k -= n;
        }
        return l.back().second;
    }

    // The smallest value of l that is >= x (or > every value: l's last + 1),
    // and the largest that is <= x (or l's first - 1).
    static i128 ceilDom(const Dom& l, i128 x) {
        for (auto& p : l)
            if (x <= p.second) return std::max(x, p.first);
        return l.back().second + 1;
    }
    static i128 floorDom(const Dom& l, i128 x) {
        for (auto it = l.rbegin(); it != l.rend(); ++it)
            if (x >= it->first) return std::min(x, it->second);
        return l.front().first - 1;
    }

    // Solves the constraints `roots` (all must hold). Leaves hull[v] a
    // single value for every variable in them. False: no solution found.
    //
    // Search with propagation. Each variable keeps a range (hull). Giving a
    // variable a value narrows the ranges of the variables it shares
    // constraints with to the bounds where their constraints can still hold
    // (binary search on each bound), and so on until nothing changes; a
    // range that empties undoes the choice. A variable's value is found by
    // halving its range, a random half first.
    bool solve(const std::vector<int>& roots, long budget) {
        hull.resize(vars.size());
        for (size_t i = 0; i < vars.size(); i++) hull[i] = {vars[i].dom.front().first, vars[i].dom.back().second};
        // Group the variables that share constraints (union-find).
        std::vector<int> uf(vars.size());
        std::iota(uf.begin(), uf.end(), 0);
        auto find = [&](int x) { while (uf[x] != x) x = uf[x] = uf[uf[x]]; return x; };
        std::vector<std::vector<int>> cvars(roots.size());
        std::vector<char> used(vars.size(), 0);
        for (size_t i = 0; i < roots.size(); i++) {
            varsOf(roots[i], cvars[i]);
            std::sort(cvars[i].begin(), cvars[i].end());
            cvars[i].erase(std::unique(cvars[i].begin(), cvars[i].end()), cvars[i].end());
            if (cvars[i].empty()) {
                if (!eval(roots[i]).canTrue()) return false;
                continue;
            }
            for (int v : cvars[i]) { used[v] = 1; uf[find(v)] = find(cvars[i][0]); }
        }
        std::vector<std::vector<int>> varCons(vars.size());   // indexes into roots
        for (size_t i = 0; i < roots.size(); i++)
            for (int v : cvars[i]) varCons[v].push_back((int)i);
        std::map<int, std::vector<int>> groups;
        for (size_t v = 0; v < vars.size(); v++)
            if (used[v]) groups[find((int)v)].push_back((int)v);

        long left = budget;
        std::vector<std::pair<int, IV>> trail;
        auto set = [&](int v, IV r) {
            trail.push_back({v, hull[v]});
            hull[v] = r;
        };
        auto undo = [&](size_t mark) {
            while (trail.size() > mark) {
                hull[trail.back().first] = trail.back().second;
                trail.pop_back();
            }
        };
        // Can v's constraints hold with v in [lo, hi]? (hull[v] is restored.)
        auto feasibleIn = [&](int v, i128 lo, i128 hi) {
            IV save = hull[v];
            hull[v] = {lo, hi};
            bool ok = true;
            for (int c : varCons[v]) {
                checks++;
                left--;
                if (!eval(roots[c]).canTrue()) { ok = false; break; }
            }
            hull[v] = save;
            return ok;
        };
        // Shrinks v's range to the bounds that can hold: -1 empty, 1 changed.
        auto narrow = [&](int v) {
            const Dom& l = vars[v].dom;
            i128 lo = hull[v].lo, hi = hull[v].hi;
            if (!feasibleIn(v, lo, hi)) return -1;
            if (lo == hi) return 0;
            i128 a = lo, b = hi;
            if (feasibleIn(v, a, a)) b = a;
            while (a < b) {
                i128 m = a + (b - a) / 2;
                if (feasibleIn(v, lo, m)) b = m; else a = m + 1;
            }
            i128 nlo = ceilDom(l, a);
            a = nlo;
            b = hi;
            if (a > b) return -1;
            if (a < b && feasibleIn(v, b, b)) a = b;
            while (a < b) {
                i128 m = a + (b - a + 1) / 2;
                if (feasibleIn(v, m, hi)) a = m; else b = m - 1;
            }
            i128 nhi = floorDom(l, a);
            if (nlo > nhi) return -1;
            if (nlo == lo && nhi == hi) return 0;
            set(v, {nlo, nhi});
            return 1;
        };
        std::vector<int> queue;
        std::vector<char> queued(vars.size(), 0);
        // Narrows the neighbours of the queued variables to a fixed point.
        auto propagate = [&]() {
            long rounds = 20000;
            bool ok = true;
            for (size_t q = 0; q < queue.size(); q++) {
                int v = queue[q];
                queued[v] = 0;
                if (!ok || --rounds < 0 || left < 0) continue;
                for (int c : varCons[v]) {
                    for (int u : cvars[c]) {
                        if (u == v || hull[u].single()) continue;
                        int r = narrow(u);
                        if (r < 0) { ok = false; break; }
                        if (r > 0 && !queued[u]) { queued[u] = 1; queue.push_back(u); }
                    }
                    if (!ok) break;
                }
            }
            queue.clear();
            return ok;
        };

        struct Fr {
            int i;
            i128 lo, hi, lo2, hi2;
            size_t mark;
            uint8_t st;
            bool halve;   // go straight to halving (the values were tried one by one)
        };
        std::vector<Fr> st;
        for (auto& g : groups) {
            const std::vector<int>& order = g.second;
            int n = (int)order.size();
            // First: every variable to its feasible bounds.
            for (int v : order) {
                if (narrow(v) < 0) return false;
                queued[v] = 1;
                queue.push_back(v);
            }
            if (!propagate()) return false;
            trail.clear();
            st.clear();
            st.push_back({0, hull[order[0]].lo, hull[order[0]].hi, 0, 0, 0, 0, false});
            bool call = true, done = false;
            while (!st.empty() && !done) {
                Fr& f = st.back();
                int v = order[f.i];
                if (call) {
                    if (left < 0) return false;
                    f.mark = trail.size();
                    set(v, {f.lo, f.hi});
                    bool ok = feasibleIn(v, f.lo, f.hi);
                    if (ok && f.lo == f.hi) {
                        queued[v] = 1;
                        queue.push_back(v);
                        ok = propagate();
                    }
                    if (!ok) { undo(f.mark); st.pop_back(); call = false; continue; }
                    if (f.lo == f.hi) {
                        if (f.i + 1 == n) { done = true; break; }
                        f.st = 3;
                        int nv = order[f.i + 1];
                        st.push_back({f.i + 1, hull[nv].lo, hull[nv].hi, 0, 0, 0, 0, false});
                        continue;
                    }
                    const Dom& l = vars[v].dom;
                    i128 cnt = countIn(l, f.lo, f.hi), half = cnt / 2;
                    if (cnt <= 16 && !f.halve) {
                        // A few values: one of those that can hold, each as likely.
                        i128 ok16[16];
                        int nok = 0;
                        for (i128 k = 0; k < cnt; k++) {
                            i128 x = kth(l, f.lo, k);
                            if (feasibleIn(v, x, x)) ok16[nok++] = x;
                        }
                        if (!nok) { undo(f.mark); st.pop_back(); call = false; continue; }
                        i128 x = ok16[(int)rng.below(nok)];
                        f.st = 1;
                        f.lo2 = f.lo;
                        f.hi2 = f.hi;
                        st.push_back({f.i, x, x, 0, 0, 0, 0, false});
                        continue;
                    }
                    i128 m = kth(l, f.lo, half - 1), m2 = kth(l, f.lo, half);
                    bool lowFirst = rng.below(cnt) < half;
                    Fr c1{f.i, f.lo, m, 0, 0, 0, 0, false}, c2{f.i, m2, f.hi, 0, 0, 0, 0, false};
                    if (!lowFirst) std::swap(c1, c2);
                    f.st = 1;
                    f.lo2 = c2.lo;
                    f.hi2 = c2.hi;
                    st.push_back(c1);
                } else if (f.st == 1) {
                    f.st = 2;
                    Fr c2{f.i, f.lo2, f.hi2, 0, 0, 0, 0, f.lo2 == f.lo && f.hi2 == f.hi};
                    st.push_back(c2);
                    call = true;
                } else {
                    undo(f.mark);
                    st.pop_back();
                }
            }
            if (!done) return false;
        }
        return true;
    }

    // The value of node n, deciding now the random fields it reads: the
    // action in scope is solved by itself and those fields are fixed.
    i128 structValue(int n, const Scope& sc, Loc l) {
        std::vector<int> roots;
        for (const Scope* s = &sc; s; s = s->outer)
            if (s->ai >= 0) actionCons(s->ai, roots);
        if (!solve(roots, 200000)) throw Retry{d.m.where(l) + ": no solution for the fields this expression reads"};
        std::vector<int> vs;
        varsOf(n, vs);
        for (int v : vs) {
            if (!hull[v].single()) {
                const Dom& l2 = vars[v].dom;
                i128 x = kth(l2, l2.front().first, rng.below(countIn(l2, l2.front().first, l2.back().second)));
                hull[v] = {x, x};
            }
            vars[v].dom = {{hull[v].lo, hull[v].lo}};
        }
        IV r = eval(n);
        if (r.empty()) throw Retry{d.m.where(l) + ": division by zero"};
        return r.lo;
    }

    // -------------------------------------------------------------- activity
    int expandAction(int ai, int parentNode, int depth) {
        if (depth > 200) d.m.fail(ais[ai].type->loc, "action " + ais[ai].type->qname + " traverses itself without end");
        const TypeDecl* t = ais[ai].type;
        if (t->activity.empty()) {
            int n = newNode(Node::Leaf, ai, parentNode);
            ais[ai].node = n;
            return n;
        }
        int n = newNode(Node::Seq, ai, parentNode);
        ais[ai].node = n;
        Locals loc;
        for (auto& a : t->activity)
            for (auto& s : a->body) expandStmt(*s, ai, n, loc, depth);
        return n;
    }
    i128 actValue(const Expr& e, int ai, const Locals& loc) {
        Scope sc;
        sc.ai = ai;
        sc.locals = loc;
        return compileConst(e, sc);
    }
    void traverse(const Act& s, const TypeDecl* t, const std::string& hname, int ai, int parentNode, const Locals& loc, int depth) {
        int sub = newAI(t, ai, hname, nullptr, s.loc);
        for (auto& c : s.with) ais[sub].extra.push_back({c, ai, loc, false});
        ais[ai].handles[hname].push_back(sub);
        expandAction(sub, parentNode, depth + 1);
    }
    void expandStmt(const Act& s, int ai, int parentNode, Locals& loc, int depth) {
        switch (s.k) {
        case Act::Seq:
        case Act::Par: {
            int n = newNode(s.k == Act::Seq ? Node::Seq : Node::Par, -1, parentNode);
            for (auto& b : s.body) expandStmt(*b, ai, n, loc, depth);
            break;
        }
        case Act::Sched: {
            // Any order is a legal schedule; pick one.
            int n = newNode(Node::Seq, -1, parentNode);
            std::vector<const Act*> o;
            for (auto& b : s.body) o.push_back(b.get());
            for (size_t i = o.size(); i > 1; i--) std::swap(o[i - 1], o[(size_t)rng.below((i128)i)]);
            for (auto* b : o) expandStmt(*b, ai, n, loc, depth);
            break;
        }
        case Act::Select: {
            std::vector<size_t> ok;
            std::vector<i128> w;
            i128 total = 0;
            for (size_t i = 0; i < s.body.size(); i++) {
                if (s.guards[i] && !actValue(*s.guards[i], ai, loc)) continue;
                i128 x = s.weights[i] ? actValue(*s.weights[i], ai, loc) : 1;
                if (x <= 0) continue;
                ok.push_back(i);
                w.push_back(x);
                total += x;
            }
            if (ok.empty()) throw Retry{d.m.where(s.loc) + ": no branch of this select can be taken"};
            i128 r = rng.below(total);
            size_t k = 0;
            while (r >= w[k]) r -= w[k++];
            expandStmt(*s.body[ok[k]], ai, parentNode, loc, depth);
            break;
        }
        case Act::Repeat:
        case Act::Replicate: {
            i128 n = actValue(*s.e, ai, loc);
            if (n < 0 || n > 100000) throw Retry{d.m.where(s.loc) + ": repeat count " + i128str(n) + " is out of range"};
            int into = s.k == Act::Repeat ? newNode(Node::Seq, -1, parentNode) : parentNode;
            for (i128 i = 0; i < n; i++) {
                if (!s.idx.empty()) loc.push_back({s.idx, i});
                for (auto& b : s.body) expandStmt(*b, ai, into, loc, depth);
                if (!s.idx.empty()) loc.pop_back();
            }
            break;
        }
        case Act::If: {
            bool c = actValue(*s.e, ai, loc) != 0;
            for (auto& b : c ? s.body : s.els) expandStmt(*b, ai, parentNode, loc, depth);
            break;
        }
        case Act::Traverse: {
            const Field* f = ais[ai].type->field(s.handle.name);
            const TypeDecl* t = nullptr;
            if (f && f->k == Field::Data && d.resolved(f->type).k == TypeRef::Named)
                t = d.declOf(d.resolved(f->type), ais[ai].type, f->loc);
            if (!t || t->k != TypeDecl::Action)
                d.m.fail(s.loc, "'" + s.handle.name + "' is not an action handle of " + ais[ai].type->qname);
            std::string hn = s.handle.name;
            if (s.handle.index) hn += "[" + i128str(actValue(*s.handle.index, ai, loc)) + "]";
            traverse(s, t, hn, ai, parentNode, loc, depth);
            break;
        }
        case Act::Do: {
            const TypeDecl* t = d.lookup(s.type, ais[ai].type);
            if (!t || t->k != TypeDecl::Action) d.m.fail(s.loc, "no action '" + s.type + "'");
            traverse(s, t, "do_" + t->name + "#" + std::to_string(ais.size()), ai, parentNode, loc, depth);
            break;
        }
        case Act::Constraint:
            for (auto& c : s.with) ais[ai].extra.push_back({c, ai, loc, true});
            break;
        case Act::Bind:
            binds.push_back({ai, &s});
            bindLocals.push_back(loc);
            break;
        }
    }

    // --------------------------------------------------------------- binding
    // The (action, field) an activity bind endpoint names.
    std::pair<int, const Field*> endpoint(const Expr& e, int ai, const Locals& loc) {
        int cur = ai;
        for (size_t i = 0; i < e.path.size(); i++) {
            const PathElem& pe = e.path[i];
            const Field* f = ais[cur].type->field(pe.name);
            if (!f) d.m.fail(e.loc, "no field '" + pe.name + "' in " + ais[cur].type->qname);
            if (i + 1 == e.path.size()) {
                if (f->k != Field::Input && f->k != Field::Output) d.m.fail(e.loc, "bind needs an input or an output");
                return {cur, f};
            }
            std::string hn = pe.name;
            if (pe.index) hn += "[" + i128str(actValue(*pe.index, ai, loc)) + "]";
            auto it = ais[cur].handles.find(hn);
            if (it == ais[cur].handles.end()) throw Retry{d.m.where(e.loc) + ": '" + pe.name + "' is not traversed"};
            cur = it->second.back();
        }
        d.m.fail(e.loc, "bad bind");
    }
    void explicitBinds() {
        for (size_t b = 0; b < binds.size(); b++) {
            const Act& s = *binds[b].second;
            int out = -1;
            std::vector<std::pair<int, const Field*>> ins;
            for (auto& e : s.binds) {
                auto ep = endpoint(*e, binds[b].first, bindLocals[b]);
                if (ep.second->k == Field::Output) {
                    if (out >= 0) d.m.fail(s.loc, "a bind has one output");
                    out = ais[ep.first].refs[ep.second->name];
                } else ins.push_back(ep);
            }
            if (out < 0) d.m.fail(s.loc, "a bind needs an output");
            for (auto& in : ins) {
                const TypeDecl* t = d.declOf(d.resolved(in.second->type), ais[in.first].type, in.second->loc);
                if (t != objs[out].type) d.m.fail(s.loc, "bind: the objects are of different types");
                ais[in.first].refs[in.second->name] = out;
                objs[out].consumers++;
            }
        }
    }
    // Puts node `fresh` next to node `at` under a new Seq (fresh first) or
    // Par node that takes at's place.
    void wrap(int at, int fresh, Node::K k) {
        int p = nodes[at].parent;
        int w = newNode(k, -1, -1);
        nodes[w].parent = p;
        if (p >= 0) {
            for (int& x : nodes[p].kids)
                if (x == at) x = w;
        } else root = w;
        nodes[w].kids = {fresh, at};
        nodes[fresh].parent = w;
        nodes[at].parent = w;
    }
    void subtreeAIs(int n, std::vector<int>& out) const {
        if (nodes[n].ai >= 0) out.push_back(nodes[n].ai);
        for (int k : nodes[n].kids) subtreeAIs(k, out);
    }
    int infer(const TypeDecl* ot, int pool, bool producer, int forAi, int depth, Loc l, std::string& field) {
        auto& table = producer ? d.producers : d.consumers;
        std::vector<std::tuple<const TypeDecl*, std::string, const CompInst*>> c;
        auto it = table.find(ot);
        if (it != table.end())
            for (auto& pf : it->second) {
                if (!pf.first->owner) continue;
                auto bt = d.byType.find(pf.first->owner);
                if (bt == d.byType.end()) continue;
                for (CompInst* ci : bt->second)
                    if (d.poolFor(ci, ot) == pool) c.push_back({pf.first, pf.second, ci});
            }
        if (c.empty()) {
            if (!producer) return -1;
            throw Retry{d.m.where(l) + ": no action can produce the " + ot->qname + " that " + ais[forAi].name + " needs"};
        }
        if (depth > 16) throw Retry{d.m.where(l) + ": more than 16 inferred actions in a row for " + ais[forAi].name};
        auto& pick = c[(size_t)rng.below((i128)c.size())];
        field = std::get<1>(pick);
        int a = newAI(std::get<0>(pick), -1, "inferred" + std::to_string(ais.size()), std::get<2>(pick), l);
        ais[a].inferred = true;
        return a;
    }
    void bindInputs(int ai, int depth) {
        const TypeDecl* t = ais[ai].type;
        for (auto& f : t->fields) {
            if (f.k != Field::Input || ais[ai].refs[f.name] >= 0) continue;
            const TypeDecl* ot = d.declOf(d.resolved(f.type), t, f.loc);
            int pool = d.poolFor(ais[ai].comp, ot);
            if (pool < 0)
                d.m.fail(f.loc, "no pool of " + ot->qname + " is visible from component " + ais[ai].comp->path);
            bool stream = ot->k == TypeDecl::Stream;
            std::vector<int> c;
            for (size_t o = 0; o < objs.size(); o++) {
                const Obj& ob = objs[o];
                if (ob.type != ot || ob.pool != pool || ob.producer < 0 || ob.producer == ai) continue;
                if (stream && ob.consumers) continue;
                if (relation(ais[ob.producer].node, ais[ai].node) == (stream ? 'p' : 'b')) c.push_back((int)o);
            }
            int o;
            if (!c.empty() && (attempt < 8 || rng.below(2))) {
                o = c[(size_t)rng.below((i128)c.size())];
            } else {
                std::string of;
                int a = infer(ot, pool, true, ai, depth, f.loc, of);
                o = ais[a].refs[of];
                int n = expandAction(a, -1, 0);
                wrap(ais[ai].node, n, stream ? Node::Par : Node::Seq);
                std::vector<int> fresh;
                subtreeAIs(n, fresh);
                // The stream is taken before the new actions look for theirs.
                ais[ai].refs[f.name] = o;
                objs[o].consumers++;
                for (int x : fresh) bindInputs(x, depth + 1);
                continue;
            }
            ais[ai].refs[f.name] = o;
            objs[o].consumers++;
        }
    }
    void bindAll() {
        explicitBinds();
        std::vector<int> order;
        subtreeAIs(root, order);
        for (int ai : order) bindInputs(ai, 0);
        // A stream nobody reads: add a reader where the model has one.
        for (size_t o = 0; o < objs.size(); o++) {
            if (objs[o].type->k != TypeDecl::Stream || objs[o].producer < 0 || objs[o].consumers) continue;
            std::string inf;
            int p = objs[o].producer;
            int a = infer(objs[o].type, objs[o].pool, false, p, 0, objs[o].type->loc, inf);
            if (a < 0) continue;
            ais[a].refs[inf] = (int)o;
            objs[o].consumers++;
            int n = expandAction(a, -1, 0);
            wrap(ais[p].node, n, Node::Par);
            std::vector<int> fresh;
            subtreeAIs(n, fresh);
            for (int x : fresh) bindInputs(x, 1);
        }
    }

    // ----------------------------------------------------------------- build
    void build() {
        int a = newAI(d.rootAction, -1, d.rootAction->name, nullptr, d.rootAction->loc);
        root = expandAction(a, -1, 0);
        bindAll();
        ar.clear();
        std::vector<int> roots;
        for (size_t i = 0; i < ais.size(); i++) actionCons((int)i, roots);
        for (size_t i = 0; i < objs.size(); i++) objectCons((int)i, roots);
        // Resources: two claims that overlap in time, at least one a lock,
        // take different instances.
        std::map<int, std::vector<int>> claims;
        for (size_t i = 0; i < objs.size(); i++)
            if (objs[i].idVar >= 0) claims[objs[i].pool].push_back((int)i);
        for (auto& pc : claims) {
            auto& c = pc.second;
            for (size_t i = 0; i < c.size(); i++)
                for (size_t j = i + 1; j < c.size(); j++) {
                    const Obj& x = objs[c[i]];
                    const Obj& y = objs[c[j]];
                    if (!x.lock && !y.lock) continue;
                    if (x.ai != y.ai) {
                        char r = relation(ais[x.ai].node, ais[y.ai].node);
                        if (r == 'a' || r == 'b') continue;
                    }
                    roots.push_back(node(Op::Ne, node(Op::Var, x.idVar), node(Op::Var, y.idVar)));
                }
        }
        if (!solve(roots, 2000000)) throw Retry{"the constraints have no solution the solver could find"};
        for (size_t v = 0; v < vars.size(); v++) {
            if (!hull[v].single()) {
                const Dom& l = vars[v].dom;
                i128 x = kth(l, l.front().first, rng.below(countIn(l, l.front().first, l.back().second)));
                hull[v] = {x, x};
            }
            vars[v].val = hull[v].lo;
            vars[v].dom = {{vars[v].val, vars[v].val}};
        }
        // Check: every constraint holds exactly.
        for (int r : roots) {
            IV x = eval(r);
            if (x.empty() || !x.single() || !x.lo) throw Retry{"internal: a solved constraint does not hold"};
        }
    }
};

constexpr int kAttempts = 64;

// Scenario `index` of the run with `seed`. The result depends on those two
// only, so scenarios can be solved in any order on any number of threads.
inline std::unique_ptr<Scenario> generate(const Design& d, uint64_t seed, int index, std::string& err) {
    for (int k = 0; k < kAttempts; k++) {
        auto s = std::make_unique<Scenario>(d);
        s->seed = seed;
        s->index = index;
        s->attempt = k;
        s->rng = Rng(Rng::mix(Rng::mix(seed, (uint64_t)index), (uint64_t)k));
        try {
            s->build();
            return s;
        } catch (Retry& r) {
            err = r.why;
        }
    }
    return nullptr;
}

} // namespace pss
