// msim -vcd-class [N]: class objects in the waveform (Sim::ObjView).
//
// A waveform holds signals, and an object is not one: it is made by new,
// is reached through handles that change, and is gone when nothing points
// to it. So the waveform shows what a handle variable points to. For each
// class handle variable of a module (tb.p, a 64-bit signal holding the
// object's first signal id), every property of its declared class becomes a
// signal tb.p.addr that follows the handle: the property of the object tb.p
// points to now, x while tb.p is null. Handle properties are followed N
// levels down (default 1: tb.p.next is the handle itself; with 2,
// tb.p.next.addr too). Views are written at the end of each time slot.
//
// Left out: string, real, event, array and container properties, handles in
// automatic variables, and handles held only inside other objects (a UVM
// component tree): use msim -i (print tb.h.next.id) or mdebug -tr for those.
#pragma once

namespace objview {

inline const ClassInfo* infoOf(const slang::ast::ClassType& ct) {
    for (auto& c : classStore())
        if (c->ty == &ct) return c.get();
    return nullptr;
}

// The properties of ct, base class first, as views under `name`.
inline void addProps(const slang::ast::ClassType& ct, const std::string& name, int handle,
                     const std::vector<int>& offs, int depth, size_t& made) {
    const ClassInfo* ci = infoOf(ct);
    if (!ci || !ci->lay || ci->builtin) return;
    if (const slang::ast::Type* b = ct.getBaseClass())
        if (b->getCanonicalType().isClass()) addProps(b->getCanonicalType().as<slang::ast::ClassType>(), name, handle, offs, depth, made);
    for (const auto& p : ct.membersOfType<slang::ast::ClassPropertySymbol>()) {
        if (p.lifetime == slang::ast::VariableLifetime::Static) continue;
        auto it = propMap().find(&p);
        if (it == propMap().end() || it->second.kind != 0) continue;   // arrays, containers
        const auto& ty = p.getType().getCanonicalType();
        if (ty.isString() || ty.isFloating() || ty.isEvent() || ty.isCHandle() || ty.isVirtualInterface()) continue;
        int off = it->second.off;
        if (off < 0 || (size_t)off >= ci->lay->w.size()) continue;
        int w = ci->lay->w[(size_t)off];
        if (w <= 0) continue;
        std::vector<int> o = offs;
        o.push_back(off);
        std::string n = name + "." + std::string(p.name);
        int sg = S.sig(n, w);
        S.sigs[(size_t)sg].val = allx(w);   // as a null handle shows it
        S.objViews.push_back({sg, handle, o});
        made++;
        if (ty.isClass() && depth > 1) addProps(ty.as<slang::ast::ClassType>(), n, handle, o, depth - 1, made);
    }
}

// Every class handle variable of every instance. Returns the views made.
inline size_t build(slang::ast::Compilation& comp, int depth) {
    std::unordered_map<std::string, int> byName;
    for (size_t i = 0; i < S.sigs.size(); i++) byName.emplace(S.sigs[i].name, (int)i);
    size_t made = 0;
    std::function<void(const slang::ast::Scope&, const std::string&)> walk = [&](const slang::ast::Scope& sc, const std::string& pre) {
        for (const auto& m : sc.members()) {
            if (m.kind == slang::ast::SymbolKind::Instance) {
                const auto& in = m.as<slang::ast::InstanceSymbol>();
                walk(in.body, pre + std::string(in.name) + ".");
            } else if (m.kind == slang::ast::SymbolKind::GenerateBlock) {
                const auto& g = m.as<slang::ast::GenerateBlockSymbol>();
                if (!g.isUninstantiated && !g.name.empty()) walk(g, pre + std::string(g.name) + ".");
            } else if (m.kind == slang::ast::SymbolKind::Variable) {
                const auto& v = m.as<slang::ast::VariableSymbol>();
                const auto& ty = v.getType().getCanonicalType();
                if (!ty.isClass()) continue;
                auto it = byName.find(pre + std::string(v.name));
                if (it == byName.end()) continue;
                addProps(ty.as<slang::ast::ClassType>(), it->first, it->second, {}, depth, made);
            }
        }
    };
    for (auto* t : comp.getRoot().topInstances)
        walk(t->body, t->name == S.vcdTop ? std::string() : std::string(t->name) + ".");
    return made;
}

}  // namespace objview
