// vhdl/parse.hpp - recursive-descent parser for VHDL (IEEE 1076-1993 and
// the commonly used parts of 1076-2008), producing vhdl/ast.hpp trees.
#pragma once

#include <functional>
#include <set>

#include "ast.hpp"

namespace vhdl {

class Parser {
public:
    Parser(std::vector<Token> toks, std::string file) : t(std::move(toks)), file(std::move(file)) {}

    std::vector<UP> designFile() {
        std::vector<UP> units;
        std::vector<DP> ctx;
        while (!at(Tok::End)) {
            if (isKw("library")) { ctx.push_back(libraryClause()); continue; }
            if (isKw("use")) { ctx.push_back(useClause()); continue; }
            if (isKw("context") && peekKw(2, "is") == false) {   // context reference: context lib.ctx;
                int ln = cur().line;
                next();
                auto d = std::make_shared<Decl>(DK::Context);
                d->line = ln;
                do { d->uses.push_back(name()); } while (accept(","));
                expect(";");
                ctx.push_back(d);
                continue;
            }
            UP u = designUnit();
            u->ctx = ctx;
            u->file = file;
            ctx.clear();
            units.push_back(u);
        }
        return units;
    }

private:
    std::vector<Token> t;
    std::string file;
    size_t p = 0;

    const Token& cur() const { return t[p]; }
    const Token& la(size_t k) const { return t[std::min(p + k, t.size() - 1)]; }
    bool at(Tok k) const { return cur().kind == k; }
    bool isKw(const char* w) const { return cur().kind == Tok::Id && cur().low == w; }
    bool peekKw(size_t k, const char* w) const { return la(k).kind == Tok::Id && la(k).low == w; }
    bool isOp(const char* o) const { return cur().kind == Tok::Op && cur().low == o; }
    bool peekOp(size_t k, const char* o) const { return la(k).kind == Tok::Op && la(k).low == o; }
    void next() { if (p + 1 < t.size()) ++p; }

    [[noreturn]] void fail(const std::string& m) const {
        std::string got = cur().kind == Tok::End ? "end of file" : "'" + (cur().kind == Tok::Str ? "\"" + cur().text + "\"" : cur().text) + "'";
        throw Error(file + ":" + std::to_string(cur().line) + ": " + m + " (found " + got + ")");
    }
    bool accept(const char* w) {
        if ((cur().kind == Tok::Op || cur().kind == Tok::Id) && cur().low == w) { next(); return true; }
        return false;
    }
    void expect(const char* w) {
        if (!accept(w)) fail(std::string("expected '") + w + "'");
    }
    bool isIdent() const { return (cur().kind == Tok::Id && !isReserved(cur().low)) || cur().kind == Tok::ExtId; }
    std::pair<std::string, std::string> ident() {
        if (!isIdent()) fail("expected an identifier");
        auto r = std::make_pair(cur().low, cur().text);
        if (cur().kind == Tok::ExtId) r.second = cur().text;
        next();
        return r;
    }
    // end [kw] [kw2] [name] ;
    void endOf(std::initializer_list<const char*> kws) {
        expect("end");
        for (auto* k : kws)
            if (isKw(k)) next();
        if (isIdent() || at(Tok::Str)) next();
        expect(";");
    }

    // ---------------------------------------------------------------- units
    DP libraryClause() {
        auto d = std::make_shared<Decl>(DK::Library);
        d->line = cur().line;
        expect("library");
        do { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); } while (accept(","));
        expect(";");
        return d;
    }
    DP useClause() {
        auto d = std::make_shared<Decl>(DK::Use);
        d->line = cur().line;
        expect("use");
        do { d->uses.push_back(name()); } while (accept(","));
        expect(";");
        return d;
    }

    UP designUnit() {
        auto u = std::make_shared<Unit>();
        u->line = cur().line;
        if (accept("entity")) {
            u->k = Unit::Entity;
            std::tie(u->name, u->text) = ident();
            expect("is");
            if (isKw("generic")) { next(); u->generics = interfaceList(DK::Generic); expect(";"); }
            if (isKw("port")) { next(); u->ports = interfaceList(DK::Port); expect(";"); }
            u->decls = declarations();
            if (accept("begin")) u->stmts = concurrentStmts();
            endOf({"entity"});
        } else if (accept("architecture")) {
            u->k = Unit::Arch;
            std::tie(u->name, u->text) = ident();
            expect("of");
            std::tie(u->of, u->ofText) = ident();
            expect("is");
            u->decls = declarations();
            expect("begin");
            u->stmts = concurrentStmts();
            endOf({"architecture"});
        } else if (accept("package")) {
            if (accept("body")) {
                u->k = Unit::PackageBody;
                std::tie(u->name, u->text) = ident();
                expect("is");
                u->decls = declarations();
                endOf({"package", "body"});
            } else {
                std::tie(u->name, u->text) = ident();
                expect("is");
                if (accept("new")) {   // package instantiation
                    u->k = Unit::PackageInst;
                    EP n = name();
                    u->of = lastName(n);
                    u->ofText = u->of;
                    if (accept("generic")) { expect("map"); u->genericMap = assocList(); }
                    expect(";");
                    return u;
                }
                u->k = Unit::Package;
                if (isKw("generic")) { next(); u->generics = interfaceList(DK::Generic); expect(";"); }
                u->decls = declarations();
                endOf({"package"});
            }
        } else if (accept("configuration")) {
            u->k = Unit::Config;
            std::tie(u->name, u->text) = ident();
            expect("of");
            std::tie(u->of, u->ofText) = ident();
            expect("is");
            while (isKw("use") || isKw("attribute") || isKw("group")) declarations1();
            configBlock(*u, true);
            endOf({"configuration"});
        } else if (accept("context")) {
            u->k = Unit::Context;
            std::tie(u->name, u->text) = ident();
            expect("is");
            while (!isKw("end")) {
                if (isKw("library")) u->decls.push_back(libraryClause());
                else if (isKw("use")) u->decls.push_back(useClause());
                else if (accept("context")) {
                    auto d = std::make_shared<Decl>(DK::Context);
                    do { d->uses.push_back(name()); } while (accept(","));
                    expect(";");
                    u->decls.push_back(d);
                } else fail("expected a library, use or context clause");
            }
            endOf({"context"});
        } else {
            fail("expected a design unit (entity, architecture, package, configuration)");
        }
        return u;
    }

    static std::string lastName(const EP& e) {
        if (!e) return "";
        if (e->k == EK::Name) return e->name;
        if (e->k == EK::Sel) return e->name;
        return "";
    }

    // configuration block: for arch { for label : comp use entity lib.e(a); end for; } end for;
    void configBlock(Unit& u, bool top) {
        expect("for");
        EP spec = name();
        if (top) u.configArch = lastName(spec);
        while (isKw("use")) declarations1();
        while (isKw("for")) {
            // component configuration or nested block configuration
            size_t save = p;
            next();
            std::vector<std::string> labels;
            bool isComp = false;
            if (isKw("all") || isKw("others")) { labels.push_back(cur().low); next(); isComp = accept(":"); }
            else {
                labels.push_back(cur().low);
                next();
                while (accept(",")) { labels.push_back(cur().low); next(); }
                if (isOp("(")) { skipParens(); }
                isComp = accept(":");
            }
            if (!isComp) { p = save; configBlock(u, false); continue; }
            auto [comp, compText] = ident();
            Unit::Bind b;
            b.comp = comp;
            if (accept("use")) bindingIndication(b);
            if (isKw("for")) {   // the bound architecture's own configuration
                b.sub = std::make_shared<Unit>();
                b.sub->k = Unit::Config;
                configBlock(*b.sub, true);
            }
            for (auto& l : labels) { auto bb = b; bb.label = l; u.binds.push_back(bb); }
            expect("end");
            expect("for");
            expect(";");
        }
        expect("end");
        expect("for");
        expect(";");
    }
    // entity lib.e(a) | configuration lib.c | open, then optional maps and ';'
    void bindingIndication(Unit::Bind& b) {
        if (accept("entity")) {
            EP en = name();
            if (en->k == EK::Call) {   // e(arch)
                if (!en->args.empty() && en->args[0].actual) b.arch = lastName(en->args[0].actual);
                en = en->a;
            }
            b.entity = lastName(en);
        } else if (accept("configuration")) {
            b.entity = lastName(name());
            b.isConfig = true;
        } else if (accept("open")) {
        }
        while (isKw("generic") || isKw("port")) { next(); expect("map"); assocList(); }
        expect(";");
    }
    void skipParens() {
        int depth = 0;
        do {
            if (isOp("(")) ++depth;
            else if (isOp(")")) --depth;
            next();
        } while (depth > 0 && !at(Tok::End));
    }

    // ----------------------------------------------------------- interfaces
    std::vector<DP> interfaceList(DK kind) {
        std::vector<DP> r;
        expect("(");
        do {
            if (isOp(")")) break;
            r.push_back(interfaceDecl(kind));
        } while (accept(";"));
        expect(")");
        return r;
    }

    DP interfaceDecl(DK kind) {
        auto d = std::make_shared<Decl>(kind);
        d->line = cur().line;
        if (isKw("signal") || isKw("variable") || isKw("constant") || isKw("file")) { d->cls = cur().low; next(); }
        else if (isKw("type")) {   // VHDL-2008 generic type
            next();
            d->cls = "type";
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            if (accept("is")) { /* VHDL-2019 type classes */ while (!isOp(";") && !isOp(")")) next(); }
            return d;
        } else if (isKw("function") || isKw("procedure") || isKw("pure") || isKw("impure")) {
            d->cls = "subprogram";
            d->sub = subprogramSpec();
            d->names.push_back(d->sub->name);
            d->texts.push_back(d->sub->text);
            if (accept("is")) {
                if (accept("<>")) d->mode = "box";   // default: the visible subprogram of the same name
                else d->target = name();
            }
            return d;
        } else if (isKw("package")) {
            next();
            d->cls = "package";
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            expect("is");
            expect("new");
            d->target = name();
            expect("generic");
            expect("map");
            if (isOp("(") && peekOp(1, "<>")) { next(); next(); expect(")"); }
            else if (isOp("(") && peekKw(1, "default")) { next(); next(); expect(")"); }
            else assocList();
            return d;
        }
        do { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); } while (accept(","));
        expect(":");
        if (isKw("in") || isKw("out") || isKw("inout") || isKw("buffer") || isKw("linkage")) { d->mode = cur().low; next(); }
        else if (isKw("view")) { next(); name(); return d; }
        d->type = subtypeIndication();
        if (accept("bus")) {}
        if (accept(":=")) d->init = expression();
        if (d->mode.empty()) d->mode = "in";
        return d;
    }

    SubInd subtypeIndication() {
        SubInd s;
        // optional resolution indication: a function name or (element resolution)
        if (isOp("(")) { skipParens(); }
        EP m = typeMark();
        if (isIdent() && !isKw("range")) {   // resolution function name followed by the type mark
            s.resolution = m;
            m = typeMark();
        }
        s.mark = m;
        if (isKw("range")) { next(); s.range = range(); }
        else if (isOp("(")) {
            next();
            do {
                if (isKw("open")) { next(); s.idx.push_back(std::make_shared<Expr>(EK::Open)); continue; }
                s.idx.push_back(discreteRange());
            } while (accept(","));
            expect(")");
            while (isOp("(")) {   // element constraint
                next();
                std::vector<EP> e;
                do {
                    if (isKw("open")) { next(); e.push_back(std::make_shared<Expr>(EK::Open)); continue; }
                    e.push_back(discreteRange());
                } while (accept(","));
                expect(")");
                s.elemIdx.push_back(e);
            }
        }
        return s;
    }
    EP typeMark() {
        auto [l, x] = ident();
        EP e = std::make_shared<Expr>(EK::Name, cur().line);
        e->name = l;
        e->text = x;
        while (isOp(".") ) {
            next();
            auto [l2, x2] = ident();
            EP s = std::make_shared<Expr>(EK::Sel, e->line);
            s->a = e;
            s->name = l2;
            s->text = x2;
            e = s;
        }
        if (isOp("'") && (peekKw(1, "subtype") || peekKw(1, "base") || peekKw(1, "element"))) {
            next();
            EP at = std::make_shared<Expr>(EK::Attr, e->line);
            at->a = e;
            at->name = cur().low;
            next();
            e = at;
        }
        return e;
    }

    EP range() {
        EP l = simpleExpression();
        if (isKw("to") || isKw("downto")) {
            EP r = std::make_shared<Expr>(EK::Range, l->line);
            r->downto = isKw("downto");
            next();
            r->a = l;
            r->b = simpleExpression();
            return r;
        }
        return l;   // x'range, x'reverse_range
    }
    EP discreteRange() {
        if (isOp("<>")) { next(); return std::make_shared<Expr>(EK::Box); }
        EP l = simpleExpression();
        if (isKw("to") || isKw("downto")) {
            EP r = std::make_shared<Expr>(EK::Range, l->line);
            r->downto = isKw("downto");
            next();
            r->a = l;
            r->b = simpleExpression();
            return r;
        }
        if (isKw("range")) {   // subtype mark range <> | range constraint
            next();
            EP sr = std::make_shared<Expr>(EK::SubtypeRange, l->line);
            sr->a = l;
            if (isOp("<>")) { next(); sr->b = std::make_shared<Expr>(EK::Box); }
            else sr->b = range();
            return sr;
        }
        return l;
    }

    // --------------------------------------------------------- declarations
    std::vector<DP> declarations() {
        std::vector<DP> r;
        for (;;) {
            auto v = declarations1();
            if (v.empty()) break;
            for (auto& d : v) r.push_back(d);
        }
        return r;
    }

    std::vector<DP> declarations1() {
        std::vector<DP> out;
        int ln = cur().line;
        auto one = [&](DK k) {
            auto d = std::make_shared<Decl>(k);
            d->line = ln;
            out.push_back(d);
            return d;
        };
        if (isKw("signal") || isKw("variable") || isKw("constant") || (isKw("shared") && peekKw(1, "variable"))) {
            bool shared = accept("shared");
            DK k = isKw("signal") ? DK::Signal : isKw("variable") ? DK::Variable : DK::Constant;
            next();
            auto d = one(k);
            d->shared = shared;
            do { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); } while (accept(","));
            expect(":");
            d->type = subtypeIndication();
            if (isKw("register") || isKw("bus")) next();
            if (accept(":=")) d->init = expression();
            expect(";");
        } else if (isKw("file")) {
            next();
            auto d = one(DK::File);
            do { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); } while (accept(","));
            expect(":");
            d->type = subtypeIndication();
            if (accept("open")) d->openKind = expression();
            if (accept("is")) {
                if (isKw("in") || isKw("out")) { d->mode = cur().low; next(); }   // VHDL-87
                d->target = expression();
            }
            expect(";");
        } else if (isKw("type")) {
            next();
            auto d = one(DK::Type);
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            d->tdef = std::make_shared<TypeDef>();
            if (accept("is")) typeDef(*d->tdef);
            else d->tdef->k = TypeDef::Incomplete;
            expect(";");
        } else if (isKw("subtype")) {
            next();
            auto d = one(DK::Subtype);
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            expect("is");
            d->type = subtypeIndication();
            expect(";");
        } else if (isKw("component")) {
            next();
            auto d = one(DK::Component);
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            accept("is");
            if (accept("generic")) { d->generics = interfaceList(DK::Generic); expect(";"); }
            if (accept("port")) { d->ports = interfaceList(DK::Port); expect(";"); }
            expect("end");
            expect("component");
            if (isIdent()) next();
            expect(";");
        } else if (isKw("function") || isKw("procedure") || isKw("pure") || isKw("impure")) {
            auto d = one(DK::Subprog);
            d->sub = subprogramSpec();
            d->names.push_back(d->sub->name);
            d->texts.push_back(d->sub->text);
            if (accept("is")) {
                if (accept("new")) {   // subprogram instantiation (4.4)
                    d->target = name();
                    if (isOp("[")) { while (!isOp("]")) next(); next(); }   // signature
                    if (accept("generic")) { expect("map"); d->genericMap = assocList(); }
                    expect(";");
                    d->k = DK::SubInst;
                    return out;
                }
                d->sub->hasBody = true;
                d->sub->decls = declarations();
                expect("begin");
                d->sub->body = sequentialStmts();
                expect("end");
                if (isKw("function") || isKw("procedure")) next();
                if (isIdent() || at(Tok::Str)) next();
            }
            expect(";");
        } else if (isKw("alias")) {
            next();
            auto d = one(DK::Alias);
            if (at(Tok::Char) || at(Tok::Str)) { d->names.push_back(cur().text); d->texts.push_back(cur().text); next(); }
            else { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); }
            if (accept(":")) d->type = subtypeIndication();
            expect("is");
            d->target = name();
            if (isOp("[")) { while (!isOp("]")) next(); next(); }   // signature
            expect(";");
        } else if (isKw("attribute")) {
            next();
            auto [l, x] = ident();
            if (accept(":")) {
                auto d = one(DK::Attribute);
                d->names.push_back(l);
                d->texts.push_back(x);
                d->type = subtypeIndication();
            } else {
                auto d = one(DK::AttrSpec);
                d->attrName = l;
                expect("of");
                do {
                    if (isKw("others") || isKw("all")) next();
                    else if (at(Tok::Str) || at(Tok::Char)) next();
                    else ident();
                    if (isOp("[")) { while (!isOp("]")) next(); next(); }
                } while (accept(","));
                expect(":");
                next();   // entity class
                expect("is");
                d->target = expression();
            }
            expect(";");
        } else if (isKw("use")) {
            out.push_back(useClause());
        } else if (isKw("library")) {
            out.push_back(libraryClause());
        } else if (isKw("for")) {   // configuration specification: for u : c use entity ...;
            auto d = one(DK::Config);
            next();
            std::vector<std::string> labels;
            do { labels.push_back(cur().low); next(); } while (accept(","));
            expect(":");
            auto [comp, compText] = ident();
            Unit::Bind b;
            b.comp = comp;
            if (accept("use")) bindingIndication(b);
            else expect(";");
            d->cfg = std::make_shared<Unit>();
            d->cfg->k = Unit::Config;
            for (auto& l : labels) { auto bb = b; bb.label = l; d->cfg->binds.push_back(bb); }
            if (isKw("end") && peekKw(1, "for")) { next(); next(); expect(";"); }
        } else if (isKw("disconnect")) {
            one(DK::Disconnect);
            while (!isOp(";")) next();
            next();
        } else if (isKw("group")) {
            one(DK::Group);
            while (!isOp(";")) next();
            next();
        } else if (isKw("package")) {   // nested package or package instantiation (2008)
            auto d = one(DK::PackageInst);
            next();
            if (accept("body")) { while (!(isKw("end") && (peekKw(1, "package") || la(1).kind == Tok::Id))) { declarations1(); if (isKw("end")) break; } endOf({"package", "body"}); return out; }
            auto [l, x] = ident();
            d->names.push_back(l);
            d->texts.push_back(x);
            expect("is");
            if (accept("new")) {
                d->target = name();
                d->cfg = std::make_shared<Unit>();
                d->cfg->k = Unit::PackageInst;
                d->cfg->name = l;
                d->cfg->text = x;
                d->cfg->of = lastName(d->target);
                d->cfg->ofText = d->cfg->of;
                d->cfg->line = d->line;
                if (accept("generic")) { expect("map"); d->cfg->genericMap = assocList(); }
                expect(";");
            } else {
                declarations();
                endOf({"package"});
            }
        } else if (isKw("context")) {
            next();
            while (!isOp(";")) next();
            next();
        }
        return out;
    }

    std::shared_ptr<Subprog> subprogramSpec() {
        auto s = std::make_shared<Subprog>();
        s->line = cur().line;
        if (accept("pure")) s->pure = true;
        else if (accept("impure")) s->pure = false;
        s->isFunc = isKw("function");
        next();
        if (at(Tok::Str)) { s->name = "\"" + lowerStr(cur().text) + "\""; s->text = s->name; next(); }
        else std::tie(s->name, s->text) = ident();
        if (accept("generic")) s->generics = interfaceList(DK::Generic);
        accept("parameter");
        if (isOp("(")) s->params = interfaceList(DK::Param);
        for (auto& pd : s->params) {
            if (pd->cls.empty()) pd->cls = (s->isFunc || pd->mode == "in") ? "constant" : "variable";
        }
        if (s->isFunc && !(isKw("is") && peekKw(1, "new"))) {   // function f is new g: no return type
            expect("return");
            if (isIdent() && peekKw(1, "of")) { ident(); next(); }   // return identifier of (2019)
            s->ret = subtypeIndication();
        }
        return s;
    }

    void typeDef(TypeDef& td) {
        if (isOp("(")) {
            td.k = TypeDef::Enum;
            next();
            do {
                if (at(Tok::Char)) { td.lits.push_back("'" + cur().text + "'"); td.litText.push_back("'" + cur().text + "'"); next(); }
                else { auto [l, x] = ident(); td.lits.push_back(l); td.litText.push_back(x); }
            } while (accept(","));
            expect(")");
        } else if (accept("range")) {
            td.range = range();
            td.k = TypeDef::Int;
            if (accept("units")) {
                td.k = TypeDef::Phys;
                td.baseUnit = ident().first;
                expect(";");
                while (!isKw("end")) {
                    auto [u, ut] = ident();
                    expect("=");
                    EP v = primary();
                    td.units.push_back({u, v});
                    expect(";");
                }
                expect("end");
                expect("units");
                if (isIdent()) next();
            } else if (isReal(td.range)) td.k = TypeDef::Real;
        } else if (accept("array")) {
            td.k = TypeDef::Array;
            expect("(");
            do {
                EP r = discreteRange();
                if (r->k == EK::SubtypeRange && r->b && r->b->k == EK::Box) td.unconstrained = true;
                td.index.push_back(r);
            } while (accept(","));
            expect(")");
            expect("of");
            td.elem = subtypeIndication();
        } else if (accept("record")) {
            td.k = TypeDef::Record;
            while (!isKw("end")) {
                auto d = std::make_shared<Decl>(DK::Field);
                d->line = cur().line;
                do { auto [l, x] = ident(); d->names.push_back(l); d->texts.push_back(x); } while (accept(","));
                expect(":");
                d->type = subtypeIndication();
                expect(";");
                td.fields.push_back(d);
            }
            expect("end");
            expect("record");
            if (isIdent()) next();
        } else if (accept("access")) {
            td.k = TypeDef::Access;
            td.elem = subtypeIndication();
        } else if (accept("file")) {
            td.k = TypeDef::File;
            expect("of");
            td.elem = subtypeIndication();
        } else if (accept("protected")) {
            if (accept("body")) {
                td.k = TypeDef::ProtectedBody;
                td.decls = declarations();
                expect("end");
                expect("protected");
                expect("body");
            } else {
                td.k = TypeDef::Protected;
                td.decls = declarations();
                expect("end");
                expect("protected");
            }
            if (isIdent()) next();
        } else {
            fail("expected a type definition");
        }
    }
    static bool isReal(const EP& r) {
        if (!r) return false;
        if (r->k == EK::Real) return true;
        if (r->k == EK::Range) return isReal(r->a) || isReal(r->b);
        if (r->k == EK::Un) return isReal(r->a);
        return false;
    }

    // ----------------------------------------------------------- statements
    bool isLabel() const { return isIdent() && peekOp(1, ":") && !peekOp(1, ":="); }

    std::vector<SP> concurrentStmts() {
        std::vector<SP> r;
        while (!isKw("end") && !isKw("elsif") && !isKw("else") && !isKw("when") && !at(Tok::End)) {
            r.push_back(concurrentStmt());
        }
        return r;
    }

    SP concurrentStmt() {
        std::string label;
        int ln = cur().line;
        if (isLabel()) { label = cur().text; next(); next(); }
        bool postponed = accept("postponed");
        int col = cur().col;
        SP s;
        if (isKw("process")) s = process();
        else if (isKw("block")) s = block();
        else if (isKw("for") && !label.empty()) s = forGenerate();
        else if (isKw("if") && !label.empty()) s = ifGenerate();
        else if (isKw("case") && !label.empty()) s = caseGenerate();
        else if (isKw("assert")) {
            s = std::make_shared<Stmt>(SK::ConcAssert, ln);
            next();
            assertTail(*s);
        } else if (isKw("component") || isKw("entity") || isKw("configuration")) {
            s = instance();
        } else if (isKw("with")) {
            s = selectedAssign(true);
        } else if (!label.empty() && isIdent() && (peekKw(1, "port") || peekKw(1, "generic"))) {
            s = instance();
        } else if (!label.empty() && isIdent() && peekOp(1, ";") ) {
            // component instance without maps, or a procedure call
            s = instance();
        } else {
            // concurrent signal assignment or procedure call
            EP target = isOp("(") ? primary() : name();
            if (isOp("<=") || isOp("<=")) {
                next();
                s = std::make_shared<Stmt>(SK::ConcAssign, ln);
                s->target = target;
                if (accept("guarded")) s->guarded = true;
                delayMechanism(*s);
                std::vector<Wave> w = waveform();
                if (isKw("when")) {
                    s->k = SK::ConcCond;
                    Alt a;
                    a.waves = w;
                    next();
                    a.cond = expression();
                    s->alts.push_back(a);
                    while (accept("else")) {
                        Alt b;
                        b.waves = waveform();
                        if (accept("when")) b.cond = expression();
                        s->alts.push_back(b);
                    }
                } else s->waves = w;
                expect(";");
            } else {
                s = std::make_shared<Stmt>(SK::ConcCall, ln);
                s->call = target;
                expect(";");
            }
        }
        s->label = label.empty() ? s->label : label;
        s->line = ln;
        s->col = col;
        s->postponed = postponed;
        return s;
    }

    void delayMechanism(Stmt& s) {
        if (accept("transport")) s.transport = true;
        else if (isKw("reject")) { next(); s.reject = expression(); expect("inertial"); }
        else accept("inertial");
    }

    std::vector<Wave> waveform() {
        std::vector<Wave> r;
        if (accept("unaffected")) return r;
        do {
            Wave w;
            if (accept("null")) w.val = std::make_shared<Expr>(EK::Null);
            else w.val = expression();
            if (accept("after")) w.after = expression();
            r.push_back(w);
        } while (accept(","));
        return r;
    }

    SP selectedAssign(bool conc) {
        int ln = cur().line;
        expect("with");
        EP sel = expression();
        expect("select");
        bool matching = accept("?");
        EP target = isOp("(") ? primary() : name();
        SP s;
        if (accept(":=")) {
            s = std::make_shared<Stmt>(SK::SelVarAssign, ln);
        } else {
            expect("<=");
            s = std::make_shared<Stmt>(conc ? SK::ConcSel : SK::SelSigAssign, ln);
            if (accept("guarded")) s->guarded = true;
            delayMechanism(*s);
        }
        s->matching = matching;
        s->value = sel;
        s->target = target;
        do {
            Alt a;
            if (s->k == SK::SelVarAssign) a.waves.push_back({expression(), nullptr});
            else a.waves = waveform();
            expect("when");
            a.choices = choices();
            s->alts.push_back(a);
        } while (accept(","));
        expect(";");
        return s;
    }

    SP process() {
        auto s = std::make_shared<Stmt>(SK::Process, cur().line);
        expect("process");
        if (accept("(")) {
            s->hasSens = true;
            if (accept("all")) s->sensAll = true;
            else
                do { s->sens.push_back(name()); } while (accept(","));
            expect(")");
        }
        accept("is");
        s->decls = declarations();
        expect("begin");
        s->body = sequentialStmts();
        expect("end");
        accept("postponed");
        expect("process");
        if (isIdent()) next();
        expect(";");
        return s;
    }

    SP block() {
        auto s = std::make_shared<Stmt>(SK::Block, cur().line);
        expect("block");
        if (accept("(")) { s->cond = expression(); expect(")"); }
        accept("is");
        if (accept("generic")) { auto g = interfaceList(DK::Generic); expect(";"); if (accept("generic")) { expect("map"); s->genericMap = assocList(); expect(";"); } for (auto& d : g) s->decls.push_back(d); }
        if (accept("port")) { auto pl = interfaceList(DK::Port); expect(";"); if (accept("port")) { expect("map"); s->portMap = assocList(); expect(";"); } for (auto& d : pl) s->decls.push_back(d); }
        auto more = declarations();
        for (auto& d : more) s->decls.push_back(d);
        expect("begin");
        s->body = concurrentStmts();
        expect("end");
        expect("block");
        if (isIdent()) next();
        expect(";");
        return s;
    }

    // generate body: [decls begin] stmts [end [label];]
    void generateBody(std::vector<DP>& decls, std::vector<SP>& body, bool alt) {
        // declarations are allowed only with "begin"
        size_t save = p;
        auto d = declarations();
        if (accept("begin")) {
            decls = d;
        } else {
            p = save;
        }
        body = concurrentStmts();
        if (alt && isKw("end") && !peekKw(1, "generate")) {   // end [alt_label];  (2008)
            next();
            if (isIdent()) next();
            expect(";");
        }
    }

    SP forGenerate() {
        auto s = std::make_shared<Stmt>(SK::ForGen, cur().line);
        expect("for");
        s->var = ident().second;
        expect("in");
        s->range = discreteRange();
        expect("generate");
        generateBody(s->decls, s->body, true);
        expect("end");
        expect("generate");
        if (isIdent()) next();
        expect(";");
        return s;
    }

    SP ifGenerate() {
        auto s = std::make_shared<Stmt>(SK::IfGen, cur().line);
        expect("if");
        for (;;) {
            Alt a;
            if (isLabel()) { a.label = cur().text; next(); next(); }
            a.cond = expression();
            expect("generate");
            generateBody(a.decls, a.body, true);
            s->alts.push_back(a);
            if (accept("elsif")) continue;
            if (accept("else")) {
                Alt e;
                if (isLabel()) { e.label = cur().text; next(); next(); }
                expect("generate");
                generateBody(e.decls, e.body, true);
                s->alts.push_back(e);
            }
            break;
        }
        expect("end");
        expect("generate");
        if (isIdent()) next();
        expect(";");
        return s;
    }

    SP caseGenerate() {
        auto s = std::make_shared<Stmt>(SK::CaseGen, cur().line);
        expect("case");
        s->value = expression();
        expect("generate");
        while (accept("when")) {
            Alt a;
            if (isLabel()) { a.label = cur().text; next(); next(); }
            a.choices = choices();
            expect("=>");
            generateBody(a.decls, a.body, true);
            s->alts.push_back(a);
        }
        expect("end");
        expect("generate");
        if (isIdent()) next();
        expect(";");
        return s;
    }

    SP instance() {
        auto s = std::make_shared<Stmt>(SK::Inst, cur().line);
        if (isKw("component") || isKw("entity") || isKw("configuration")) { s->unitKind = cur().low; next(); }
        EP n = name();
        if (s->unitKind == "entity" && n->k == EK::Call) {   // lib.e(arch)
            if (!n->args.empty() && n->args[0].actual && n->args[0].actual->k == EK::Name) s->arch = n->args[0].actual->name;
            n = n->a;
        }
        s->unit = n;
        if (accept("generic")) { expect("map"); s->genericMap = assocList(); }
        if (accept("port")) { expect("map"); s->portMap = assocList(); }
        expect(";");
        return s;
    }

    std::vector<SP> sequentialStmts() {
        std::vector<SP> r;
        while (!isKw("end") && !isKw("elsif") && !isKw("else") && !isKw("when") && !at(Tok::End)) r.push_back(sequentialStmt());
        return r;
    }

    void assertTail(Stmt& s) {
        s.cond = expression();
        if (accept("report")) s.report = expression();
        if (accept("severity")) s.severity = expression();
        expect(";");
    }

    SP sequentialStmt() {
        std::string label;
        int ln = cur().line;
        if (isLabel()) { label = cur().text; next(); next(); }
        int col = cur().col;
        SP s = seqStmtBody(ln);
        s->label = label;
        s->col = col;
        return s;
    }
    SP seqStmtBody(int ln) {
        SP s;
        if (accept("if")) {
            s = std::make_shared<Stmt>(SK::If, ln);
            Alt a;
            a.cond = expression();
            expect("then");
            a.body = sequentialStmts();
            s->alts.push_back(a);
            for (;;) {
                if (accept("elsif")) {
                    Alt b;
                    b.cond = expression();
                    expect("then");
                    b.body = sequentialStmts();
                    s->alts.push_back(b);
                } else if (accept("else")) {
                    Alt b;
                    b.body = sequentialStmts();
                    s->alts.push_back(b);
                } else break;
            }
            expect("end");
            expect("if");
            if (isIdent()) next();
            expect(";");
        } else if (accept("case")) {
            s = std::make_shared<Stmt>(SK::Case, ln);
            s->matching = accept("?");
            s->value = expression();
            expect("is");
            while (accept("when")) {
                Alt a;
                a.choices = choices();
                expect("=>");
                a.body = sequentialStmts();
                s->alts.push_back(a);
            }
            expect("end");
            expect("case");
            accept("?");
            if (isIdent()) next();
            expect(";");
        } else if (isKw("for") || isKw("while") || isKw("loop")) {
            s = std::make_shared<Stmt>(SK::Loop, ln);
            if (accept("for")) {
                s->loop = Stmt::For;
                s->var = ident().second;
                expect("in");
                s->range = discreteRange();
            } else if (accept("while")) {
                s->loop = Stmt::While;
                s->cond = expression();
            }
            expect("loop");
            s->body = sequentialStmts();
            expect("end");
            expect("loop");
            if (isIdent()) next();
            expect(";");
        } else if (isKw("next") || isKw("exit")) {
            s = std::make_shared<Stmt>(isKw("next") ? SK::Next : SK::Exit, ln);
            next();
            if (isIdent()) s->loopLabel = lowerStr(ident().second);
            if (accept("when")) s->cond = expression();
            expect(";");
        } else if (accept("return")) {
            s = std::make_shared<Stmt>(SK::Return, ln);
            if (!isOp(";")) s->value = expression();
            expect(";");
        } else if (accept("null")) {
            s = std::make_shared<Stmt>(SK::Null, ln);
            expect(";");
        } else if (accept("wait")) {
            s = std::make_shared<Stmt>(SK::Wait, ln);
            if (accept("on")) do { s->on.push_back(name()); } while (accept(","));
            if (accept("until")) s->cond = expression();
            if (accept("for")) s->forTime = expression();
            expect(";");
        } else if (accept("assert")) {
            s = std::make_shared<Stmt>(SK::Assert, ln);
            assertTail(*s);
        } else if (accept("report")) {
            s = std::make_shared<Stmt>(SK::Report, ln);
            s->report = expression();
            if (accept("severity")) s->severity = expression();
            expect(";");
        } else if (isKw("with")) {
            s = selectedAssign(false);
        } else {
            EP target = isOp("(") ? primary() : name();
            if (accept(":=")) {
                s = std::make_shared<Stmt>(SK::VarAssign, ln);
                s->target = target;
                s->value = expression();
                if (isKw("when")) {
                    s->k = SK::CondVarAssign;
                    Alt a;
                    a.waves.push_back({s->value, nullptr});
                    next();
                    a.cond = expression();
                    s->alts.push_back(a);
                    while (accept("else")) {
                        Alt b;
                        b.waves.push_back({expression(), nullptr});
                        if (accept("when")) b.cond = expression();
                        s->alts.push_back(b);
                    }
                }
                expect(";");
            } else if (accept("<=")) {
                s = std::make_shared<Stmt>(SK::SigAssign, ln);
                s->target = target;
                if (accept("force")) {
                    s->k = SK::Force;
                    if (isKw("in") || isKw("out")) next();
                    s->value = expression();
                    expect(";");
                } else if (accept("release")) {
                    s->k = SK::Release;
                    if (isKw("in") || isKw("out")) next();
                    expect(";");
                } else {
                    delayMechanism(*s);
                    std::vector<Wave> w = waveform();
                    if (isKw("when")) {
                        s->k = SK::CondSigAssign;
                        Alt a;
                        a.waves = w;
                        next();
                        a.cond = expression();
                        s->alts.push_back(a);
                        while (accept("else")) {
                            Alt b;
                            b.waves = waveform();
                            if (accept("when")) b.cond = expression();
                            s->alts.push_back(b);
                        }
                    } else s->waves = w;
                    expect(";");
                }
            } else {
                s = std::make_shared<Stmt>(SK::Call, ln);
                s->call = target;
                expect(";");
            }
        }
        return s;
    }

    std::vector<EP> choices() {
        std::vector<EP> r;
        do {
            if (accept("others")) { r.push_back(std::make_shared<Expr>(EK::Others)); continue; }
            r.push_back(discreteRange());
        } while (accept("|"));
        return r;
    }

    std::vector<Assoc> assocList() {
        std::vector<Assoc> r;
        expect("(");
        do {
            Assoc a;
            EP e = assocElem();
            if (accept("=>")) {
                a.formal = e;
                a.actual = assocElem();
            } else a.actual = e;
            r.push_back(a);
        } while (accept(","));
        expect(")");
        return r;
    }
    EP assocElem() {
        if (accept("open")) return std::make_shared<Expr>(EK::Open);
        if (accept("inertial")) return expression();
        return discreteRangeOrExpr();
    }
    EP discreteRangeOrExpr() {
        EP l = expression();
        if (isKw("to") || isKw("downto")) {
            EP r = std::make_shared<Expr>(EK::Range, l->line);
            r->downto = isKw("downto");
            next();
            r->a = l;
            r->b = simpleExpression();
            return r;
        }
        if (isKw("range")) {
            next();
            EP sr = std::make_shared<Expr>(EK::SubtypeRange, l->line);
            sr->a = l;
            sr->b = isOp("<>") ? (next(), std::make_shared<Expr>(EK::Box)) : range();
            return sr;
        }
        return l;
    }

    // ---------------------------------------------------------- expressions
    EP mk(EK k, const std::string& op, EP a, EP b = nullptr) {
        EP e = std::make_shared<Expr>(k, a ? a->line : cur().line);
        e->op = op;
        e->a = a;
        e->b = b;
        return e;
    }

public:
    EP expression() {
        if (isOp("??")) { next(); return mk(EK::Un, "??", primary()); }
        EP l = relation();
        static const char* logic[] = {"and", "or", "xor", "nand", "nor", "xnor"};
        for (;;) {
            bool found = false;
            for (auto* o : logic)
                if (isKw(o)) {
                    next();
                    l = mk(EK::Bin, o, l, relation());
                    found = true;
                    break;
                }
            if (!found) break;
        }
        return l;
    }

private:
    EP relation() {
        EP l = shiftExpr();
        static const char* rel[] = {"=", "/=", "<", "<=", ">", ">=", "?=", "?/=", "?<", "?<=", "?>", "?>="};
        for (auto* o : rel)
            if (isOp(o)) {
                next();
                return mk(EK::Bin, o, l, shiftExpr());
            }
        return l;
    }
    EP shiftExpr() {
        EP l = simpleExpression();
        static const char* sh[] = {"sll", "srl", "sla", "sra", "rol", "ror"};
        for (auto* o : sh)
            if (isKw(o)) {
                next();
                return mk(EK::Bin, o, l, simpleExpression());
            }
        return l;
    }

public:
    EP simpleExpression() {
        EP l;
        if (isOp("+") || isOp("-")) {
            std::string o = cur().low;
            next();
            l = mk(EK::Un, o, term());
        } else l = term();
        while (isOp("+") || isOp("-") || isOp("&")) {
            std::string o = cur().low;
            next();
            l = mk(EK::Bin, o, l, term());
        }
        return l;
    }

private:
    EP term() {
        EP l = factor();
        while (isOp("*") || isOp("/") || isKw("mod") || isKw("rem")) {
            std::string o = cur().low;
            next();
            l = mk(EK::Bin, o, l, factor());
        }
        return l;
    }
    EP factor() {
        if (isKw("abs") || isKw("not")) {
            std::string o = cur().low;
            next();
            return mk(EK::Un, o, primary());
        }
        static const char* red[] = {"and", "or", "xor", "nand", "nor", "xnor"};
        for (auto* o : red)
            if (isKw(o)) {
                next();
                return mk(EK::Un, o, primary());
            }
        EP l = primary();
        if (isOp("**")) {
            next();
            l = mk(EK::Bin, "**", l, primary());
        }
        return l;
    }

    EP primary() {
        int ln = cur().line;
        const Token& k = cur();
        if (k.kind == Tok::Int || k.kind == Tok::Real) {
            EP e = std::make_shared<Expr>(k.kind == Tok::Int ? EK::Int : EK::Real, ln);
            e->ival = k.ival;
            e->rval = k.real;
            e->text = k.text;
            next();
            // physical literal: number unit
            if (isIdent() && !isReserved(cur().low) && isUnitName(cur().low)) {
                e->k = EK::Phys;
                e->name = cur().low;
                next();
            }
            return e;
        }
        if (k.kind == Tok::Char) {
            EP e = std::make_shared<Expr>(EK::Char, ln);
            e->text = k.text;
            next();
            return suffixes(e);
        }
        if (k.kind == Tok::BitStr) {
            EP e = std::make_shared<Expr>(EK::BitStr, ln);
            e->text = k.text;
            next();
            return e;
        }
        if (k.kind == Tok::Str) {
            EP e = std::make_shared<Expr>(EK::Str, ln);
            e->text = k.text;
            next();
            if (isOp("(")) {   // operator symbol call: "+"(a, b)
                EP n = std::make_shared<Expr>(EK::Name, ln);
                n->name = "\"" + lowerStr(e->text) + "\"";
                n->text = n->name;
                return suffixes(n);
            }
            return e;
        }
        if (accept("null")) return std::make_shared<Expr>(EK::Null, ln);
        if (accept("new")) {
            EP e = std::make_shared<Expr>(EK::New, ln);
            e->a = name();
            if (isOp("(") ) {}
            return e;
        }
        if (isOp("<<")) {   // external name
            next();
            EP e = std::make_shared<Expr>(EK::External, ln);
            e->name = cur().low;   // signal/variable/constant
            next();
            std::string path;
            while (!isOp(":")) { path += cur().kind == Tok::Op ? cur().text : cur().text; next(); }
            next();
            e->text = path;
            SubInd si = subtypeIndication();
            e->a = si.mark;
            expect(">>");
            return suffixes(e);
        }
        if (isOp("(")) {
            next();
            std::vector<AggElem> elems;
            do {
                AggElem el;
                EP first;
                if (accept("others")) first = std::make_shared<Expr>(EK::Others, ln);
                else first = discreteRangeOrExpr();
                if (isOp("|") || isOp("=>")) {
                    el.choices.push_back(first);
                    while (accept("|")) {
                        if (accept("others")) el.choices.push_back(std::make_shared<Expr>(EK::Others, ln));
                        else el.choices.push_back(discreteRangeOrExpr());
                    }
                    expect("=>");
                    el.value = expression();
                } else el.value = first;
                elems.push_back(el);
            } while (accept(","));
            expect(")");
            if (elems.size() == 1 && elems[0].choices.empty()) {
                EP e = elems[0].value;
                // parenthesized expression: keep it from being re-associated
                EP par = mk(EK::Un, "()", e);
                return par;
            }
            EP e = std::make_shared<Expr>(EK::Agg, ln);
            e->agg = elems;
            return e;
        }
        if (isIdent()) return name();
        fail("expected an expression");
    }

    static bool isUnitName(const std::string& u) {
        static const char* units[] = {"fs", "ps", "ns", "us", "ms", "sec", "min", "hr"};
        for (auto* x : units)
            if (u == x) return true;
        return extraUnits().count(u) > 0;
    }

public:
    static std::set<std::string>& extraUnits() {
        static std::set<std::string> s;
        return s;
    }

    EP name() {
        int ln = cur().line;
        EP e;
        if (at(Tok::Str)) {
            e = std::make_shared<Expr>(EK::Name, ln);
            e->name = "\"" + lowerStr(cur().text) + "\"";
            e->text = e->name;
            next();
        } else if (at(Tok::Char)) {
            e = std::make_shared<Expr>(EK::Char, ln);
            e->text = cur().text;
            next();
        } else if (isOp("<<")) {
            return primary();
        } else {
            auto [l, x] = ident();
            e = std::make_shared<Expr>(EK::Name, ln);
            e->name = l;
            e->text = x;
        }
        return suffixes(e);
    }

private:
    EP suffixes(EP e) {
        for (;;) {
            int ln = cur().line;
            if (isOp(".")) {
                next();
                EP s = std::make_shared<Expr>(EK::Sel, ln);
                s->a = e;
                if (accept("all")) { s->name = "all"; s->text = "all"; }
                else if (at(Tok::Char)) { s->name = "'" + cur().text + "'"; s->text = s->name; next(); }
                else if (at(Tok::Str)) { s->name = "\"" + lowerStr(cur().text) + "\""; s->text = s->name; next(); }
                else std::tie(s->name, s->text) = ident();
                e = s;
            } else if (isOp("(")) {
                // a(args) -- but not "(" that begins something else; names are followed by ( only for calls/indexes
                EP c = std::make_shared<Expr>(EK::Call, ln);
                c->a = e;
                c->args = assocList();
                e = c;
            } else if (isOp("'") ) {
                if (peekOp(1, "(")) {   // qualified expression
                    next();
                    EP q = std::make_shared<Expr>(EK::Qual, ln);
                    q->a = e;
                    q->b = primary();
                    if (q->b->k == EK::Un && q->b->op == "()") q->b = q->b->a;
                    e = q;
                } else if (la(1).kind == Tok::Id || la(1).kind == Tok::ExtId) {
                    next();
                    EP at = std::make_shared<Expr>(EK::Attr, ln);
                    at->a = e;
                    at->name = cur().low;
                    at->text = cur().text;
                    next();
                    if (isOp("(") && attrTakesArg(at->name)) {
                        next();
                        at->b = expression();
                        expect(")");
                    }
                    e = at;
                } else break;
            } else if (isOp("[")) {   // signature: skip
                while (!isOp("]")) next();
                next();
            } else break;
        }
        return e;
    }
    static bool attrTakesArg(const std::string& a) {
        static const char* w[] = {"image", "value", "pos", "val", "succ", "pred", "leftof", "rightof", "high", "low", "left",
                                  "right", "length", "range", "reverse_range", "ascending", "delayed", "stable", "quiet",
                                  "event", "last_value"};
        for (auto* x : w)
            if (a == x) return true;
        return false;
    }
};

}  // namespace vhdl
