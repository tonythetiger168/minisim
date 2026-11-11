// mdebug.cpp - source-level debug for SystemVerilog on the msim front end.
//
//   mdebug tb.sv dut.sv -top tb -drivers tb.u.q        who assigns it
//   mdebug ...            -loads tb.u.q                who reads it
//   mdebug ...            -trace tb.u.q -depth 3       the fan-in cone
//   mdebug ... -vcd w.vcd -active tb.u.q -at 45        which assignment gave
//                                                      it its value at t=45,
//                                                      and what caused that
//   mdebug ... [-vcd w.vcd] -html debug.html           one-file viewer:
//                                                      hierarchy, source,
//                                                      drivers/loads,
//                                                      schematic, waves
//
// The model: every assignment that can give a signal a value is a Driver
// (a continuous assign, a procedural assignment with the if/case conditions
// that guard it, or a port connection). Loads are the same records read the
// other way. The active-driver trace evaluates the guards against the values
// in a VCD: a flop's guards are sampled just before the time its output
// changed, combinational logic at that time.
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

#include "fsm_extract.hpp"
#include "lintir.hpp"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "slang/syntax/AllSyntax.h"
#include "parallel.hpp"
#include "version.hpp"
#include "wave_reader.hpp"

using namespace lintir;

namespace {

#include "mdebug_view.inc"

// ------------------------------------------------------------------ source

struct Where {
    int file = -1;
    size_t line = 0, col = 0;
};

struct SrcFile {
    std::string name;
    BufferID buf;
    std::vector<std::string_view> lines;
};

struct Sources {
    const SourceManager* sm = nullptr;
    std::vector<SrcFile> files;
    std::map<uint32_t, int> byBuf;

    Where where(SourceLocation loc) {
        Where w;
        if (!sm || !loc) return w;
        auto full = sm->getFullyOriginalLoc(loc);
        BufferID b = full.buffer();
        auto it = byBuf.find(b.getId());
        if (it == byBuf.end()) {
            SrcFile f;
            f.name = std::string(sm->getFileName(full));
            f.buf = b;
            std::string_view text = sm->getSourceText(b);
            size_t p = 0;
            while (p <= text.size()) {
                size_t e = text.find('\n', p);
                if (e == std::string_view::npos) e = text.size();
                f.lines.push_back(text.substr(p, e - p));
                p = e + 1;
            }
            it = byBuf.emplace(b.getId(), (int)files.size()).first;
            files.push_back(std::move(f));
        }
        w.file = it->second;
        w.line = sm->getLineNumber(full);
        w.col = sm->getColumnNumber(full);
        return w;
    }

    std::string at(const Where& w) const {
        if (w.file < 0) return "<unknown>";
        return files[w.file].name + ":" + std::to_string(w.line);
    }

    // The source line, without its indentation or a trailing comment's spaces.
    std::string lineText(const Where& w) const {
        if (w.file < 0 || w.line == 0 || w.line > files[w.file].lines.size()) return "";
        std::string_view s = files[w.file].lines[w.line - 1];
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return std::string(s);
    }

    // The text of an expression as written.
    std::string text(SourceRange r) const {
        if (!sm || !r.start() || !r.end()) return "?";
        auto a = sm->getFullyOriginalLoc(r.start()), b = sm->getFullyOriginalLoc(r.end());
        if (a.buffer() != b.buffer() || b.offset() <= a.offset()) return "?";
        std::string_view t = sm->getSourceText(a.buffer());
        if (b.offset() > t.size()) return "?";
        std::string out;
        for (char c : t.substr(a.offset(), b.offset() - a.offset())) {
            if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            if (c == ' ' && !out.empty() && out.back() == ' ') continue;
            out += c;
        }
        return out;
    }
};

// ------------------------------------------------------------------- model

struct Guard {
    enum Kind { If, Else, CaseItem, CaseDefault, Opaque } kind = Opaque;
    const Expression* cond = nullptr;     // If, Else
    const CaseStatement* cs = nullptr;    // CaseItem, CaseDefault
    size_t item = 0;
};

struct Driver {
    const Symbol* target = nullptr;
    std::string kind;                     // assign, always_ff, always_comb, always, initial, final, port, "task name"
    SourceLocation loc;
    SourceRange range;                    // the assignment as written; empty for a port
    const Expression* rhs = nullptr;      // null for an output port
    const Expression* lhs = nullptr;      // the left side as written, when an assignment
    std::vector<Guard> guards;
    bool seq = false;
    bool whole = false;                   // assigns the whole target, not a select of it
    int lo = -1, w = 0;                   // the bits written, when a field or a constant select; lo -1: not known
    std::string clock;                    // the event control as written, when seq
    std::string how;                      // "", or force, assign (procedural), call (an output argument)
    int block = -1;                       // which process; -1 outside one
    std::vector<const Symbol*> reads;     // data: the right-hand side
    std::vector<const Symbol*> creads;    // control: guards and select indices
};

struct ScopeRec {
    std::string path, def;
    SourceLocation loc;                   // the module definition
    SourceLocation inst;                  // where it is instantiated
};

// A small JSON reader, enough for msim's coverage database.
struct JVal {
    enum K { Null, Num, Str, Arr, Obj } k = Null;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const std::string& key) const {
        for (auto& [kk, v] : o) if (kk == key) return &v;
        return nullptr;
    }
};
struct JReader {
    const std::string& t;
    size_t p = 0;
    bool bad = false;
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) p++; }
    std::string str() {
        std::string o;
        p++;
        while (p < t.size() && t[p] != '"') {
            if (t[p] == '\\' && p + 1 < t.size()) {
                char c = t[++p];
                if (c == 'n') o += '\n';
                else if (c == 't') o += '\t';
                else if (c == 'u' && p + 4 < t.size()) { o += (char)std::strtoul(t.substr(p + 1, 4).c_str(), nullptr, 16); p += 4; }
                else o += c;
            } else o += t[p];
            p++;
        }
        p++;
        return o;
    }
    JVal val() {
        JVal v;
        ws();
        if (p >= t.size()) { bad = true; return v; }
        char c = t[p];
        if (c == '"') { v.k = JVal::Str; v.s = str(); }
        else if (c == '[') {
            v.k = JVal::Arr;
            p++;
            for (ws(); p < t.size() && t[p] != ']'; ws()) {
                v.a.push_back(val());
                ws();
                if (p < t.size() && t[p] == ',') p++;
                else if (p >= t.size() || t[p] != ']') { bad = true; break; }
                if (bad) break;
            }
            p++;
        } else if (c == '{') {
            v.k = JVal::Obj;
            p++;
            for (ws(); p < t.size() && t[p] != '}'; ws()) {
                if (t[p] != '"') { bad = true; break; }
                std::string key = str();
                ws();
                if (p >= t.size() || t[p] != ':') { bad = true; break; }
                p++;
                v.o.emplace_back(std::move(key), val());
                ws();
                if (p < t.size() && t[p] == ',') p++;
                else if (p >= t.size() || t[p] != '}') { bad = true; break; }
                if (bad) break;
            }
            p++;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            v.k = JVal::Num;
            char* e = nullptr;
            v.n = std::strtod(t.c_str() + p, &e);
            p = e - t.c_str();
        } else if (t.compare(p, 4, "true") == 0) { v.k = JVal::Num; v.n = 1; p += 4; }
        else if (t.compare(p, 5, "false") == 0) { v.k = JVal::Num; p += 5; }
        else if (t.compare(p, 4, "null") == 0) p += 4;
        else bad = true;
        return v;
    }
};

// An assert, assume or cover directive of the design.
struct AssertRec {
    std::string name;                     // tb.u.a_gnt, or tb.u.<file>:<line> without a label
    std::string kind;                     // "assert property", "assert", "cover property" ...
    SourceLocation loc;
    SourceRange range;
    bool concurrent = false;
    std::vector<const Symbol*> reads;     // the signals it looks at, clock first when it has one
    const Symbol* clock = nullptr;
    EdgeKind edge = EdgeKind::None;
};

struct DB {
    Sources src;
    std::vector<AssertRec> asserts;
    std::vector<ScopeRec> scopes;
    std::vector<const ValueSymbol*> signals;
    std::unordered_map<const Symbol*, int> sigIx;
    std::map<std::string, const Symbol*> byPath;
    std::vector<Driver> drivers;
    std::unordered_map<const Symbol*, std::vector<int>> driversOf, loadsOf;

    void add(Driver d) {
        if (!d.target) return;
        auto uniq = [](std::vector<const Symbol*>& v) {
            std::vector<const Symbol*> o;
            for (const Symbol* s : v)
                if (std::find(o.begin(), o.end(), s) == o.end()) o.push_back(s);
            v = std::move(o);
        };
        uniq(d.reads);
        uniq(d.creads);
        int ix = (int)drivers.size();
        driversOf[d.target].push_back(ix);
        for (const Symbol* s : d.reads) loadsOf[s].push_back(ix);
        for (const Symbol* s : d.creads)
            if (std::find(d.reads.begin(), d.reads.end(), s) == d.reads.end()) loadsOf[s].push_back(ix);
        drivers.push_back(std::move(d));
    }
};

// Where a struct field sits in its struct's flat vector (bit 0 is the LSB),
// which is how msim writes a struct to the waveform: a packed struct as
// declared, an unpacked one with its first field in the high bits.
bool fieldRange(const MemberAccessExpression& m, int& lo, int& w) {
    if (m.member.kind != SymbolKind::Field) return false;
    const auto& f = m.member.as<FieldSymbol>();
    const Type& st = m.value().type->getCanonicalType();
    const Type& ft = f.getType();
    if (!ft.isIntegral()) return false;
    w = (int)ft.getBitWidth();
    if (st.kind == SymbolKind::PackedStructType) { lo = (int)f.bitOffset; return true; }
    if (st.kind != SymbolKind::UnpackedStructType) return false;
    uint64_t total = 0;
    for (const FieldSymbol* o : st.as<UnpackedStructType>().fields) {
        if (!o->getType().isIntegral()) return false;
        total += o->getType().getBitWidth();
    }
    lo = (int)(total - f.bitOffset - (uint64_t)w);
    return lo >= 0;
}

// The bits of its base variable an lvalue writes, when that is fixed:
// s.f, v[3], v[7:4], s.f[2] and so on.
bool flatRange(const Expression& e, int& lo, int& w) {
    switch (e.kind) {
        case ExpressionKind::NamedValue:
        case ExpressionKind::HierarchicalValue: {
            const Type& t = e.type->getCanonicalType();
            lo = 0;
            if (t.isIntegral()) { w = (int)t.getBitWidth(); return true; }
            if (t.kind != SymbolKind::UnpackedStructType) return false;
            w = 0;
            for (const FieldSymbol* o : t.as<UnpackedStructType>().fields) {
                if (!o->getType().isIntegral()) return false;
                w += (int)o->getType().getBitWidth();
            }
            return true;
        }
        case ExpressionKind::MemberAccess: {
            const auto& m = e.as<MemberAccessExpression>();
            int bl, bw, fl, fw;
            if (!flatRange(m.value(), bl, bw) || !fieldRange(m, fl, fw)) return false;
            lo = bl + fl; w = fw;
            return true;
        }
        case ExpressionKind::ElementSelect: {
            const auto& x = e.as<ElementSelectExpression>();
            const Type& vt = *x.value().type;
            int bl, bw;
            if (!vt.isIntegral() || e.type->getBitWidth() != 1 || x.selector().kind != ExpressionKind::IntegerLiteral ||
                !flatRange(x.value(), bl, bw)) return false;
            auto ix = x.selector().as<IntegerLiteral>().getValue().as<int32_t>();
            ConstantRange r = vt.getFixedRange();
            if (!ix || !r.containsPoint(*ix)) return false;
            lo = bl + r.translateIndex(*ix); w = 1;
            return true;
        }
        case ExpressionKind::RangeSelect: {
            const auto& x = e.as<RangeSelectExpression>();
            const Type& vt = *x.value().type;
            int bl, bw;
            if (!vt.isIntegral() || x.getSelectionKind() != RangeSelectionKind::Simple ||
                x.left().kind != ExpressionKind::IntegerLiteral || x.right().kind != ExpressionKind::IntegerLiteral ||
                !flatRange(x.value(), bl, bw)) return false;
            auto a = x.left().as<IntegerLiteral>().getValue().as<int32_t>();
            auto b = x.right().as<IntegerLiteral>().getValue().as<int32_t>();
            ConstantRange r = vt.getFixedRange();
            if (!a || !b || !r.containsPoint(*a) || !r.containsPoint(*b)) return false;
            int h = r.translateIndex(*a), l = r.translateIndex(*b);
            if (h < l) std::swap(h, l);
            lo = bl + l; w = h - l + 1;
            return true;
        }
        default: return false;
    }
}

// The variable an lvalue or a select is a part of.
const Symbol* baseOf(const Expression& e) {
    const Expression* b = &e;
    for (;;) {
        switch (b->kind) {
            case ExpressionKind::MemberAccess: b = &b->as<MemberAccessExpression>().value(); break;
            case ExpressionKind::ElementSelect: b = &b->as<ElementSelectExpression>().value(); break;
            case ExpressionKind::RangeSelect: b = &b->as<RangeSelectExpression>().value(); break;
            case ExpressionKind::NamedValue: return &b->as<NamedValueExpression>().symbol;
            case ExpressionKind::HierarchicalValue: return &b->as<HierarchicalValueExpression>().symbol;
            default: return nullptr;
        }
    }
}

// The bits of sym that an expression reads: [lo, end) around every fixed
// part that is read (s.f, v[7:4]), or whole when sym is read as it is.
struct BitsRead {
    int lo = INT_MAX, end = -1;
    bool whole = false;
    bool part() const { return !whole && end > lo; }
};
void bitsRead(const Expression& e, const Symbol* sym, BitsRead& out) {
    auto take = [&](const Expression& x) {
        int lo, w;
        if (baseOf(x) != sym || !flatRange(x, lo, w)) return false;
        out.lo = std::min(out.lo, lo);
        out.end = std::max(out.end, lo + w);
        return true;
    };
    e.visit(makeVisitor(
        [&](auto& v, const MemberAccessExpression& x) { if (!take(x)) v.visitDefault(x); },
        [&](auto& v, const ElementSelectExpression& x) { if (!take(x)) v.visitDefault(x); },
        [&](auto& v, const RangeSelectExpression& x) { if (!take(x)) v.visitDefault(x); },
        [&](auto&, const NamedValueExpression& x) { if (&x.symbol == sym) out.whole = true; },
        [&](auto&, const HierarchicalValueExpression& x) { if (&x.symbol == sym) out.whole = true; }));
}
// The part of sym that driver expressions read, as lo and width; false when
// it is read whole or the part is not fixed.
bool partRead(const Expression* rhs, const std::vector<const Expression*>& conds, const Symbol* sym, int& lo, int& w) {
    BitsRead b;
    if (!rhs) return false;
    bitsRead(*rhs, sym, b);
    for (const Expression* c : conds) if (c) bitsRead(*c, sym, b);
    if (!b.part()) return false;
    lo = b.lo; w = b.end - b.lo;
    return true;
}
// "s.f" for a field of a struct, "v[7:4]" for other bits.
std::string partName(const Symbol& s, int lo, int w) {
    if (s.kind == SymbolKind::Variable || s.kind == SymbolKind::Net) {
        const Type& t = s.as<ValueSymbol>().getType().getCanonicalType();
        std::vector<const FieldSymbol*> fields;
        bool packed = t.kind == SymbolKind::PackedStructType;
        if (packed) for (const FieldSymbol& f : t.as<PackedStructType>().membersOfType<FieldSymbol>()) fields.push_back(&f);
        else if (t.kind == SymbolKind::UnpackedStructType) for (const FieldSymbol* f : t.as<UnpackedStructType>().fields) fields.push_back(f);
        int total = 0;
        for (const FieldSymbol* f : fields) total += f->getType().isIntegral() ? (int)f->getType().getBitWidth() : 0;
        for (const FieldSymbol* f : fields) {
            if (!f->getType().isIntegral()) continue;
            int fw = (int)f->getType().getBitWidth();
            int fl = packed ? (int)f->bitOffset : total - (int)f->bitOffset - fw;
            if (fl == lo && fw == w) return "." + std::string(f->name);
        }
    }
    return w == 1 ? "[" + std::to_string(lo) + "]" : "[" + std::to_string(lo + w - 1) + ":" + std::to_string(lo) + "]";
}

// The symbols an assignment's left side writes; select indices are reads.
void lhsTargets(const Expression& e, std::vector<const Symbol*>& targets, std::vector<const Symbol*>& idx) {
    switch (e.kind) {
        case ExpressionKind::NamedValue: targets.push_back(&e.as<NamedValueExpression>().symbol); break;
        case ExpressionKind::HierarchicalValue:
            targets.push_back(&e.as<HierarchicalValueExpression>().symbol);
            break;
        case ExpressionKind::ElementSelect: {
            const auto& s = e.as<ElementSelectExpression>();
            lhsTargets(s.value(), targets, idx);
            namesIn(s.selector(), idx);
            break;
        }
        case ExpressionKind::RangeSelect: {
            const auto& s = e.as<RangeSelectExpression>();
            lhsTargets(s.value(), targets, idx);
            namesIn(s.left(), idx);
            namesIn(s.right(), idx);
            break;
        }
        case ExpressionKind::MemberAccess: lhsTargets(e.as<MemberAccessExpression>().value(), targets, idx); break;
        case ExpressionKind::Conversion: lhsTargets(e.as<ConversionExpression>().operand(), targets, idx); break;
        case ExpressionKind::Concatenation:
            for (const Expression* op : e.as<ConcatenationExpression>().operands()) lhsTargets(*op, targets, idx);
            break;
        default: namesIn(e, targets); break;
    }
}

// One process body: every assignment, with the conditions on the way to it.
struct Walker {
    DB& db;
    Driver proto;               // kind, seq, clock, block
    std::vector<Guard> gs;

    void controls(Driver& d) {
        d.guards = gs;
        for (const Guard& g : gs) {
            if (g.cond) namesIn(*g.cond, d.creads);
            if (g.cs) {
                namesIn(g.cs->expr, d.creads);
                if (g.kind == Guard::CaseItem)
                    for (const Expression* e : g.cs->items[g.item].expressions) namesIn(*e, d.creads);
            }
        }
    }

    // A task or function call: each output, inout or ref argument is driven
    // by the call, from the arguments that go in.
    void call(const CallExpression& c) {
        std::vector<const Expression*> outs;
        std::vector<const Symbol*> ins;
        for (const Expression* a : c.arguments()) {
            if (a->kind == ExpressionKind::Assignment &&
                a->as<AssignmentExpression>().right().kind == ExpressionKind::EmptyArgument)
                outs.push_back(&a->as<AssignmentExpression>().left());
            else
                namesIn(*a, ins);
        }
        // What goes in reaches the task's own assignments through its formals.
        if (c.subroutine.index() == 0) {
            auto formals = std::get<0>(c.subroutine)->getArguments();
            auto args = c.arguments();
            for (size_t i = 0; i < formals.size() && i < args.size(); i++) {
                if (args[i]->kind == ExpressionKind::Assignment || args[i]->kind == ExpressionKind::EmptyArgument) continue;
                Driver d = proto;
                d.how = "call";
                d.target = formals[i];
                d.loc = c.sourceRange.start();
                d.range = c.sourceRange;
                namesIn(*args[i], d.reads);
                controls(d);
                db.add(std::move(d));
            }
        }
        for (const Expression* o : outs) {
            std::vector<const Symbol*> targets, idx;
            lhsTargets(*o, targets, idx);
            for (const Symbol* t : targets) {
                Driver d = proto;
                d.how = "call";
                d.target = t;
                d.loc = c.sourceRange.start();
                d.range = c.sourceRange;
                d.reads = ins;
                d.creads = idx;
                controls(d);
                db.add(std::move(d));
            }
        }
    }

    // how: "" for a plain assignment, "force" or "assign" for the procedural
    // continuous forms, which override the other drivers while they hold.
    void assign(const AssignmentExpression& a, const char* how = "") {
        std::vector<const Symbol*> targets, idx;
        lhsTargets(a.left(), targets, idx);
        for (const Symbol* t : targets) {
            Driver d = proto;
            d.how = how;
            d.target = t;
            d.loc = a.sourceRange.start();
            d.range = a.sourceRange;
            d.rhs = &a.right();
            d.lhs = &a.left();
            d.whole = a.left().kind == ExpressionKind::NamedValue ||
                      a.left().kind == ExpressionKind::HierarchicalValue;
            if (targets.size() == 1) {
                int lo, w;
                if (flatRange(a.left(), lo, w)) { d.lo = lo; d.w = w; }
            }
            namesIn(a.right(), d.reads);
            d.creads = idx;
            controls(d);
            db.add(std::move(d));
        }
    }

    void opaque(const Statement& body) {
        gs.push_back(Guard{});
        walk(body);
        gs.pop_back();
    }

    void walk(const Statement& s) {
        switch (s.kind) {
            case StatementKind::List:
                for (const Statement* c : s.as<StatementList>().list) walk(*c);
                break;
            case StatementKind::Block: walk(s.as<BlockStatement>().body); break;
            case StatementKind::Timed: walk(s.as<TimedStatement>().stmt); break;
            case StatementKind::ExpressionStatement: {
                const Expression& e = s.as<ExpressionStatement>().expr;
                if (e.kind == ExpressionKind::Assignment) assign(e.as<AssignmentExpression>());
                else if (e.kind == ExpressionKind::Call) call(e.as<CallExpression>());
                break;
            }
            case StatementKind::ProceduralAssign: {
                const auto& pa = s.as<ProceduralAssignStatement>();
                if (pa.assignment.kind == ExpressionKind::Assignment)
                    assign(pa.assignment.as<AssignmentExpression>(), pa.isForce ? "force" : "assign");
                break;
            }
            case StatementKind::Conditional: {
                const auto& c = s.as<ConditionalStatement>();
                bool plain = c.conditions.size() == 1 && !c.conditions[0].pattern;
                Guard g;
                g.kind = plain ? Guard::If : Guard::Opaque;
                g.cond = c.conditions[0].expr;
                gs.push_back(g);
                walk(c.ifTrue);
                if (c.ifFalse) {
                    if (plain) gs.back().kind = Guard::Else;
                    walk(*c.ifFalse);
                }
                gs.pop_back();
                break;
            }
            case StatementKind::Case: {
                const auto& c = s.as<CaseStatement>();
                for (size_t i = 0; i < c.items.size(); i++) {
                    gs.push_back(Guard{Guard::CaseItem, nullptr, &c, i});
                    walk(*c.items[i].stmt);
                    gs.pop_back();
                }
                if (c.defaultCase) {
                    gs.push_back(Guard{Guard::CaseDefault, nullptr, &c, 0});
                    walk(*c.defaultCase);
                    gs.pop_back();
                }
                break;
            }
            case StatementKind::ForLoop: opaque(s.as<ForLoopStatement>().body); break;
            case StatementKind::ForeachLoop: opaque(s.as<ForeachLoopStatement>().body); break;
            case StatementKind::WhileLoop: opaque(s.as<WhileLoopStatement>().body); break;
            case StatementKind::DoWhileLoop: opaque(s.as<DoWhileLoopStatement>().body); break;
            case StatementKind::RepeatLoop: opaque(s.as<RepeatLoopStatement>().body); break;
            case StatementKind::ForeverLoop: walk(s.as<ForeverLoopStatement>().body); break;
            default: {
                // Forks, waits and the rest: the assignments inside are still
                // drivers, but their conditions are not followed.
                gs.push_back(Guard{});
                s.visit(makeVisitor(
                    [&](auto& v, std::derived_from<Statement> auto& inner) { v.visitDefault(inner); },
                    [&](auto&, const AssignmentExpression& a) {
                        if (a.right().kind != ExpressionKind::EmptyArgument) assign(a);
                    },
                    [&](auto& v, const CallExpression& c) {
                        call(c);
                        v.visitDefault(c);
                    }));
                gs.pop_back();
                break;
            }
        }
    }
};

struct Collect : ASTVisitor<Collect> {
    DB& db;
    int nblock = 0;
    explicit Collect(DB& d) : db(d) {}

    void signal(const ValueSymbol& v) {
        // Only what lives in the design hierarchy: not a task's locals.
        const Scope* sc = v.getParentScope();
        if (!sc) return;
        SymbolKind k = sc->asSymbol().kind;
        if (k != SymbolKind::InstanceBody && k != SymbolKind::GenerateBlock && k != SymbolKind::Package) return;
        if (db.sigIx.count(&v)) return;
        db.sigIx[&v] = (int)db.signals.size();
        db.signals.push_back(&v);
        db.byPath[path(v)] = &v;
    }
    void handle(const VariableSymbol& v) { signal(v); }
    void handle(const NetSymbol& v) {
        signal(v);
        // wire n = a + b; is a continuous assignment.
        if (const Expression* init = v.getInitializer(); init && db.sigIx.count(&v)) {
            Driver d;
            d.kind = "assign";
            d.target = &v;
            d.loc = v.location;
            d.rhs = init;
            d.whole = true;
            namesIn(*init, d.reads);
            db.add(std::move(d));
        }
    }

    void handle(const GenerateBlockSymbol& g) {
        if (g.isUninstantiated) return;
        visitDefault(g);
    }

    void handle(const InstanceSymbol& inst) {
        if (inst.body.flags.has(InstanceFlags::Uninstantiated)) return;
        ScopeRec sr;
        sr.path = path(inst);
        sr.def = std::string(inst.getDefinition().name);
        sr.loc = inst.getDefinition().location;
        sr.inst = inst.location;
        db.scopes.push_back(std::move(sr));
        for (const PortConnection* conn : inst.getPortConnections()) {
            if (!conn || conn->port.kind != SymbolKind::Port) continue;
            const auto& port = conn->port.as<PortSymbol>();
            const Symbol* in = port.internalSymbol;
            const Expression* ext = conn->getExpression();
            if (!in || !ext) continue;
            const Expression* outer = ext;
            if (outer->kind == ExpressionKind::Assignment) outer = &outer->as<AssignmentExpression>().left();
            Driver proto;
            proto.kind = "port";
            proto.loc = outer->sourceRange.start() ? outer->sourceRange.start() : inst.location;
            if (port.direction == ArgumentDirection::In || port.direction == ArgumentDirection::InOut) {
                Driver d = proto;
                d.target = in;
                d.rhs = outer;
                d.whole = true;
                namesIn(*outer, d.reads);
                db.add(std::move(d));
            }
            if (port.direction == ArgumentDirection::Out || port.direction == ArgumentDirection::InOut) {
                std::vector<const Symbol*> targets, idx;
                lhsTargets(*outer, targets, idx);
                for (const Symbol* t : targets) {
                    Driver d = proto;
                    d.target = t;
                    d.reads.push_back(in);
                    d.creads = idx;
                    db.add(std::move(d));
                }
            }
        }
        visitDefault(inst);
    }

    void handle(const ContinuousAssignSymbol& ca) {
        const Expression& e = ca.getAssignment();
        if (e.kind != ExpressionKind::Assignment) return;
        Walker w{db, {}, {}};
        w.proto.kind = "assign";
        w.assign(e.as<AssignmentExpression>());
    }

    // A task or function declared in the design writes its signals like any
    // process does; the body is one block.
    void handle(const SubroutineSymbol& sub) {
        const Scope* sc = sub.getParentScope();
        if (!sc) return;
        SymbolKind k = sc->asSymbol().kind;
        if (k != SymbolKind::InstanceBody && k != SymbolKind::GenerateBlock) return;
        if (sub.flags.has(MethodFlags::DPIImport) || sub.flags.has(MethodFlags::Pure)) return;
        Walker w{db, {}, {}};
        w.proto.kind = (sub.subroutineKind == SubroutineKind::Task ? "task " : "function ") + std::string(sub.name);
        w.proto.block = nblock++;
        w.walk(sub.getBody());
    }

    // The assertions written in (or as) a procedure.
    void assertions(const ProceduralBlockSymbol& p, const Statement& body) {
        const Scope* sc = p.getParentScope();
        std::string scope;
        if (sc) scope = path(sc->asSymbol());
        auto start = [&](const Statement& st, AssertionKind k, const char* what) {
            AssertRec a;
            a.loc = st.sourceRange.start();
            a.range = st.sourceRange;
            a.kind = std::string(k == AssertionKind::Assume ? "assume" : k == AssertionKind::CoverProperty || k == AssertionKind::CoverSequence ? "cover"
                               : k == AssertionKind::Restrict ? "restrict" : k == AssertionKind::Expect ? "expect" : "assert") + what;
            std::string label;
            if (st.syntax && slang::syntax::StatementSyntax::isKind(st.syntax->kind))
                if (auto* l = st.syntax->as<slang::syntax::StatementSyntax>().label) label = std::string(l->name.valueText());
            if (label.empty()) {
                Where w = db.src.where(a.loc);
                std::string f = w.file < 0 ? "?" : db.src.files[w.file].name;
                label = f.substr(f.find_last_of('/') + 1) + ":" + std::to_string(w.line);
            }
            a.name = scope.empty() ? label : scope + "." + label;
            return a;
        };
        body.visit(makeVisitor(
            [&](auto&, const ConcurrentAssertionStatement& st) {
                AssertRec a = start(st, st.assertionKind, " property");
                a.concurrent = true;
                auto names = makeVisitor(
                    [&](auto& v, const ClockingAssertionExpr& c) {
                        std::vector<const SignalEventControl*> evs;
                        clocksOf(c.clocking, evs);
                        if (!a.clock && !evs.empty()) {
                            std::vector<const Symbol*> ck;
                            namesIn(evs[0]->expr, ck);
                            if (ck.size() == 1) { a.clock = ck[0]; a.edge = evs[0]->edge; }
                        }
                        v.visitDefault(c);
                    },
                    [&](auto&, const NamedValueExpression& e) {
                        if (std::find(a.reads.begin(), a.reads.end(), &e.symbol) == a.reads.end()) a.reads.push_back(&e.symbol);
                    },
                    [&](auto&, const HierarchicalValueExpression& e) {
                        if (std::find(a.reads.begin(), a.reads.end(), &e.symbol) == a.reads.end()) a.reads.push_back(&e.symbol);
                    });
                st.propertySpec.visit(names);
                db.asserts.push_back(std::move(a));
            },
            [&](auto&, const ImmediateAssertionStatement& st) {
                AssertRec a = start(st, st.assertionKind, st.isDeferred ? (st.isFinal ? " final" : " #0") : "");
                namesIn(st.cond, a.reads);
                db.asserts.push_back(std::move(a));
            }));
    }

    void handle(const ProceduralBlockSymbol& p) {
        Walker w{db, {}, {}};
        switch (p.procedureKind) {
            case ProceduralBlockKind::AlwaysFF: w.proto.kind = "always_ff"; break;
            case ProceduralBlockKind::AlwaysComb: w.proto.kind = "always_comb"; break;
            case ProceduralBlockKind::AlwaysLatch: w.proto.kind = "always_latch"; break;
            case ProceduralBlockKind::Always: w.proto.kind = "always"; break;
            case ProceduralBlockKind::Initial: w.proto.kind = "initial"; break;
            default: w.proto.kind = "final"; break;
        }
        w.proto.block = nblock++;
        const Statement& body = p.getBody();
        assertions(p, body);
        if (body.kind == StatementKind::Timed && w.proto.kind != "initial") {
            const auto& t = body.as<TimedStatement>();
            std::vector<const SignalEventControl*> evs;
            clocksOf(t.timing, evs);
            for (const SignalEventControl* e : evs)
                if (e->edge != EdgeKind::None) w.proto.seq = true;
            if (w.proto.seq) w.proto.clock = "@" + db.src.text(t.timing.sourceRange);
        }
        w.walk(body);
    }
};

// --------------------------------------------------------------- waveforms

using wave::Wave;
using wave::showBits;
using par::parallelFor;
using Vcd = wave::Reader;

// --------------------------------------------------------------- evaluator

// Expressions over waveform values, enough for the conditions RTL is written
// with. Anything it does not know gives nullopt, which the trace shows as "?".
struct Eval {
    const Vcd& vcd;
    uint64_t t = 0;

    std::optional<SVInt> lookup(const Symbol& s, const Type& type) {
        if (s.kind == SymbolKind::Parameter) {
            const ConstantValue& cv = s.as<ParameterSymbol>().getValue();
            if (cv.isInteger()) return cv.integer();
            return std::nullopt;
        }
        if (s.kind == SymbolKind::EnumValue) {
            const ConstantValue& cv = s.as<EnumValueSymbol>().getValue();
            if (cv.isInteger()) return cv.integer();
            return std::nullopt;
        }
        return named(path(s), type);
    }

    std::optional<SVInt> named(const std::string& name, const Type& type) {
        const Wave* w = vcd.find(name);
        if (!w || !(type.isIntegral() || type.getCanonicalType().kind == SymbolKind::UnpackedStructType))
            return std::nullopt;
        int i = w->at(t);
        std::string bits = i < 0 ? std::string(w->width, 'x') : w->bits(i);
        SVInt v = SVInt::fromString(std::to_string(bits.size()) + "'b" + bits);
        v.setSigned(type.isIntegral() && type.isSigned());
        return v;
    }

    static SVInt bit(logic_t l) { return SVInt(l); }

    std::optional<SVInt> eval(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::NamedValue: return lookup(e.as<NamedValueExpression>().symbol, *e.type);
            case ExpressionKind::HierarchicalValue:
                return lookup(e.as<HierarchicalValueExpression>().symbol, *e.type);
            case ExpressionKind::IntegerLiteral: return e.as<IntegerLiteral>().getValue();
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {
                if (!e.type->isIntegral()) return std::nullopt;
                logic_t l = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                bitwidth_t w = e.type->getBitWidth();
                SVInt one(l);
                return w <= 1 ? one : one.replicate(SVInt(32, w, false));
            }
            case ExpressionKind::Conversion: {
                const auto& c = e.as<ConversionExpression>();
                auto v = eval(c.operand());
                if (!v || !e.type->isIntegral() || !c.operand().type->isIntegral()) return std::nullopt;
                bitwidth_t w = e.type->getBitWidth();
                SVInt r = w > v->getBitWidth() ? v->extend(w, c.operand().type->isSigned())
                                               : (w < v->getBitWidth() ? v->trunc(w) : *v);
                r.setSigned(e.type->isSigned());
                return r;
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                auto v = eval(u.operand());
                if (!v) return std::nullopt;
                switch (u.op) {
                    case UnaryOperator::Plus: return v;
                    case UnaryOperator::Minus: return -*v;
                    case UnaryOperator::BitwiseNot: return ~*v;
                    case UnaryOperator::BitwiseAnd: return bit(v->reductionAnd());
                    case UnaryOperator::BitwiseOr: return bit(v->reductionOr());
                    case UnaryOperator::BitwiseXor: return bit(v->reductionXor());
                    case UnaryOperator::BitwiseNand: return bit(!v->reductionAnd());
                    case UnaryOperator::BitwiseNor: return bit(!v->reductionOr());
                    case UnaryOperator::BitwiseXnor: return bit(!v->reductionXor());
                    case UnaryOperator::LogicalNot: return bit(!*v);
                    default: return std::nullopt;
                }
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                auto l = eval(b.left()), r = eval(b.right());
                if (!l || !r) return std::nullopt;
                switch (b.op) {
                    case BinaryOperator::Add: return *l + *r;
                    case BinaryOperator::Subtract: return *l - *r;
                    case BinaryOperator::Multiply: return *l * *r;
                    case BinaryOperator::Divide: return *l / *r;
                    case BinaryOperator::Mod: return *l % *r;
                    case BinaryOperator::BinaryAnd: return *l & *r;
                    case BinaryOperator::BinaryOr: return *l | *r;
                    case BinaryOperator::BinaryXor: return *l ^ *r;
                    case BinaryOperator::BinaryXnor: return l->xnor(*r);
                    case BinaryOperator::Equality: return bit(*l == *r);
                    case BinaryOperator::Inequality: return bit(*l != *r);
                    case BinaryOperator::CaseEquality: return bit(logic_t(exactlyEqual(*l, *r)));
                    case BinaryOperator::CaseInequality: return bit(logic_t(!exactlyEqual(*l, *r)));
                    case BinaryOperator::GreaterThanEqual: return bit(*l >= *r);
                    case BinaryOperator::GreaterThan: return bit(*l > *r);
                    case BinaryOperator::LessThanEqual: return bit(*l <= *r);
                    case BinaryOperator::LessThan: return bit(*l < *r);
                    case BinaryOperator::LogicalAnd: return bit(*l && *r);
                    case BinaryOperator::LogicalOr: return bit(*l || *r);
                    case BinaryOperator::LogicalShiftLeft:
                    case BinaryOperator::ArithmeticShiftLeft: return l->shl(*r);
                    case BinaryOperator::LogicalShiftRight: return l->lshr(*r);
                    case BinaryOperator::ArithmeticShiftRight:
                        return b.left().type->isSigned() ? l->ashr(*r) : l->lshr(*r);
                    default: return std::nullopt;
                }
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) return std::nullopt;
                auto p = eval(*c.conditions[0].expr), l = eval(c.left()), r = eval(c.right());
                if (!p || !l || !r) return std::nullopt;
                return SVInt::conditional(*p, *l, *r);
            }
            case ExpressionKind::ElementSelect: {
                const auto& s = e.as<ElementSelectExpression>();
                auto i = eval(s.selector());
                if (!i || i->hasUnknown()) return std::nullopt;
                auto ix = i->as<int32_t>();
                if (!ix) return std::nullopt;
                const Type& vt = *s.value().type;
                if (vt.isIntegral()) {
                    auto v = eval(s.value());
                    if (!v || e.type->getBitWidth() != 1) return std::nullopt;
                    ConstantRange range = vt.getFixedRange();
                    if (!range.containsPoint(*ix)) return bit(logic_t::x);
                    return bit((*v)[range.translateIndex(*ix)]);
                }
                // An unpacked array element is its own waveform: mem[3].
                if (const Symbol* sym = oneName(s.value());
                    sym && s.value().kind == ExpressionKind::NamedValue)
                    return named(path(*sym) + "[" + std::to_string(*ix) + "]", *e.type);
                return std::nullopt;
            }
            case ExpressionKind::MemberAccess: {
                // A struct is one vector in the waveform; a field is a slice.
                const auto& m = e.as<MemberAccessExpression>();
                int lo, w;
                if (!fieldRange(m, lo, w)) return std::nullopt;
                auto v = eval(m.value());
                if (!v || lo + w > (int)v->getBitWidth()) return std::nullopt;
                SVInt f = v->slice(lo + w - 1, lo);
                f.setSigned(e.type->isSigned());
                return f;
            }
            case ExpressionKind::RangeSelect: {
                const auto& s = e.as<RangeSelectExpression>();
                const Type& vt = *s.value().type;
                auto v = eval(s.value()), l = eval(s.left()), r = eval(s.right());
                if (!vt.isIntegral() || !v || !l || !r || l->hasUnknown() || r->hasUnknown()) return std::nullopt;
                auto a = l->as<int32_t>(), b = r->as<int32_t>();
                if (!a || !b) return std::nullopt;
                ConstantRange range = vt.getFixedRange();
                int32_t hi = *a, lo = *b;
                if (s.getSelectionKind() == RangeSelectionKind::IndexedUp) {
                    lo = *a; hi = *a + *b - 1;
                    if (!range.isDescending()) std::swap(hi, lo);
                } else if (s.getSelectionKind() == RangeSelectionKind::IndexedDown) {
                    hi = *a; lo = *a - *b + 1;
                    if (!range.isDescending()) std::swap(hi, lo);
                }
                if (!range.containsPoint(hi) || !range.containsPoint(lo)) return std::nullopt;
                return v->slice(range.translateIndex(hi), range.translateIndex(lo));
            }
            case ExpressionKind::Concatenation: {
                std::vector<SVInt> parts;
                for (const Expression* op : e.as<ConcatenationExpression>().operands()) {
                    auto v = eval(*op);
                    if (!v) return std::nullopt;
                    parts.push_back(*v);
                }
                if (parts.empty()) return std::nullopt;
                return SVInt::concat(parts);
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                auto n = eval(r.count()), v = eval(r.concat());
                if (!n || !v || n->hasUnknown()) return std::nullopt;
                return v->replicate(*n);
            }
            default: return std::nullopt;
        }
    }

    // Does this item of the case match? (12.5: ===, or the casez/casex rule)
    std::optional<bool> itemMatches(const CaseStatement& cs, size_t item, const SVInt& sel) {
        if (cs.condition == CaseStatementCondition::Inside) return std::nullopt;
        for (const Expression* ie : cs.items[item].expressions) {
            auto v = eval(*ie);
            if (!v) return std::nullopt;
            bitwidth_t w = std::max(sel.getBitWidth(), v->getBitWidth());
            SVInt a = sel.getBitWidth() < w ? sel.extend(w, sel.isSigned()) : sel;
            SVInt b = v->getBitWidth() < w ? v->extend(w, v->isSigned()) : *v;
            b.setSigned(a.isSigned());
            bool m = cs.condition == CaseStatementCondition::WildcardJustZ ? caseZWildcardEqual(a, b)
                   : cs.condition == CaseStatementCondition::WildcardXOrZ ? caseXWildcardEqual(a, b)
                   : exactlyEqual(a, b);
            if (m) return true;
        }
        return false;
    }

    std::optional<bool> holds(const Guard& g) {
        switch (g.kind) {
            case Guard::If:
            case Guard::Else: {
                auto v = eval(*g.cond);
                if (!v) return std::nullopt;
                bool taken = exactlyEqual(v->reductionOr(), logic_t(true));   // x takes the else
                return g.kind == Guard::If ? taken : !taken;
            }
            case Guard::CaseItem:
            case Guard::CaseDefault: {
                auto sel = eval(g.cs->expr);
                if (!sel) return std::nullopt;
                size_t upto = g.kind == Guard::CaseItem ? g.item : g.cs->items.size();
                for (size_t i = 0; i < upto; i++) {
                    auto m = itemMatches(*g.cs, i, *sel);
                    if (!m) return std::nullopt;
                    if (*m) return false;   // an earlier item wins
                }
                if (g.kind == Guard::CaseDefault) return true;
                return itemMatches(*g.cs, g.item, *sel);
            }
            default: return std::nullopt;
        }
    }
};

// ----------------------------------------------------------------- queries

struct Tool {
    DB db;
    Vcd vcd;
    bool haveVcd = false;
    unsigned threads = 1;

    std::string guardText(const Guard& g) {
        switch (g.kind) {
            case Guard::If: return db.src.text(g.cond->sourceRange);
            case Guard::Else: return "!(" + db.src.text(g.cond->sourceRange) + ")";
            case Guard::CaseItem: {
                std::string s = db.src.text(g.cs->expr.sourceRange) + " == ";
                bool first = true;
                for (const Expression* e : g.cs->items[g.item].expressions) {
                    s += (first ? "" : ", ") + db.src.text(e->sourceRange);
                    first = false;
                }
                return s;
            }
            case Guard::CaseDefault: return db.src.text(g.cs->expr.sourceRange) + ": default";
            default: return "...";
        }
    }

    std::string when(const Driver& d) {
        std::string s;
        for (const Guard& g : d.guards) s += (s.empty() ? "" : " && ") + guardText(g);
        return s;
    }

    std::string label(const Driver& d) {
        std::string s = d.kind;
        if (!d.clock.empty()) s += " " + d.clock;
        if (!d.how.empty()) s += ", " + d.how;
        return s;
    }

    // The assignment as written; a port connection shows its line.
    std::string text(const Driver& d) {
        if (d.range.start()) {
            std::string t = db.src.text(d.range);
            if (t != "?") return t;
        }
        return db.src.lineText(db.src.where(d.loc));
    }

    // "  dut.sv:12: q <= d;  [always_ff @(posedge clk)]  when en"
    std::string line(const Driver& d) {
        Where w = db.src.where(d.loc);
        std::string s = db.src.at(w) + ": " + text(d) + "  [" + label(d) + "]";
        if (std::string c = when(d); !c.empty()) s += "  when " + c;
        // With a coverage database: whether this line ever ran.
        if (auto h = hitsOf(w)) s += *h ? "  (line ran " + std::to_string(*h) + (*h == 1 ? " time)" : " times)") : "  (line never ran)";
        return s;
    }

    const Symbol* find(const std::string& name) {
        auto it = db.byPath.find(name);
        if (it != db.byPath.end()) return it->second;
        // A unique tail is enough: u.q for tb.u.q.
        const Symbol* hit = nullptr;
        int n = 0;
        for (auto& [p, s] : db.byPath)
            if (p.size() > name.size() && p.ends_with(name) && p[p.size() - name.size() - 1] == '.') {
                hit = s;
                n++;
            }
        if (n == 1) return hit;
        std::cerr << "mdebug: " << (n ? "'" + name + "' names " + std::to_string(n) + " signals; give the full path"
                                      : "no signal '" + name + "' in the design")
                  << "\n";
        return nullptr;
    }

    bool drivers(const std::string& name) {
        const Symbol* s = find(name);
        if (!s) return false;
        auto& v = db.driversOf[s];
        std::cout << "drivers of " << path(*s) << ": " << v.size() << "\n";
        for (int i : v) std::cout << "  " << line(db.drivers[i]) << "\n";
        return true;
    }

    bool loads(const std::string& name) {
        const Symbol* s = find(name);
        if (!s) return false;
        auto& v = db.loadsOf[s];
        std::cout << "loads of " << path(*s) << ": " << v.size() << "\n";
        for (int i : v) {
            const Driver& d = db.drivers[i];
            bool data = std::find(d.reads.begin(), d.reads.end(), s) != d.reads.end();
            std::cout << "  " << line(d) << "  -> " << path(*d.target) << (data ? "" : "  (control)") << "\n";
        }
        return true;
    }

    // The fan-in cone, one level per indent.
    void cone(const Symbol* s, int depth, int indent, std::set<const Symbol*>& onPath) {
        std::string pad(indent * 2, ' ');
        std::cout << pad << path(*s);
        auto it = db.driversOf.find(s);
        if (it == db.driversOf.end()) { std::cout << "  (no driver: an input or a testbench value)\n"; return; }
        if (onPath.count(s)) { std::cout << "  (loop)\n"; return; }
        std::cout << "\n";
        if (depth <= 0) return;
        onPath.insert(s);
        for (int i : it->second) {
            const Driver& d = db.drivers[i];
            std::cout << pad << "  <- " << line(d) << "\n";
            std::vector<const Symbol*> in = d.reads;
            for (const Symbol* c : d.creads)
                if (std::find(in.begin(), in.end(), c) == in.end()) in.push_back(c);
            for (const Symbol* r : in)
                if (db.sigIx.count(r) || db.driversOf.count(r)) cone(r, depth - 1, indent + 2, onPath);
        }
        onPath.erase(s);
    }

    // The block view of one scope: its instances, what each port connects
    // to, and which instance feeds which.
    bool blocks(const std::string& name) {
        auto parent = [](const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? std::string() : p.substr(0, d); };
        auto leaf = [](const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1); };
        const ScopeRec* sc = nullptr;
        int n = 0;
        for (auto& r : db.scopes) {
            if (r.path == name) { sc = &r; n = 1; break; }
            if (r.path.size() > name.size() && r.path.compare(r.path.size() - name.size() - 1, std::string::npos, "." + name) == 0) { sc = &r; n++; }
        }
        if (n != 1) {
            std::cerr << "mdebug: " << (n ? "more than one instance ends in '" : "no instance '") << name << "'\n";
            return false;
        }
        struct Blk { const ScopeRec* r; std::vector<std::string> in, out; };
        std::vector<Blk> bs;
        std::map<std::string, size_t> ix;
        for (auto& r : db.scopes)
            if (parent(r.path) == sc->path) { ix[r.path] = bs.size(); bs.push_back({&r, {}, {}}); }
        // net in this scope -> the ports that drive it, and the ports it feeds
        std::map<std::string, std::vector<std::string>> from, to;
        for (const Driver& d : db.drivers) {
            if (d.kind != "port" || !db.sigIx.count(d.target)) continue;
            std::string tp = path(*d.target), ts = parent(tp);
            if (auto it = ix.find(ts); it != ix.end()) {
                if (!d.rhs) continue;   // an output of an instance one level further down
                std::string nets;
                for (const Symbol* r : d.reads) {
                    if (!db.sigIx.count(r)) continue;
                    std::string rp = path(*r);
                    nets += (nets.empty() ? "" : ", ") + rp;
                    to[rp].push_back(leaf(ts) + "." + leaf(tp));
                }
                if (nets.empty()) nets = d.rhs ? db.src.text(d.rhs->sourceRange) : "?";
                bs[it->second].in.push_back(leaf(tp) + " <- " + nets);
            }
            else if (ts == sc->path) {
                for (const Symbol* r : d.reads) {
                    if (!db.sigIx.count(r)) continue;
                    std::string rp = path(*r);
                    auto jt = ix.find(parent(rp));
                    if (jt == ix.end()) continue;
                    bs[jt->second].out.push_back(leaf(rp) + " -> " + tp);
                    from[tp].push_back(leaf(parent(rp)) + "." + leaf(rp));
                }
            }
        }
        std::cout << "blocks of " << sc->path << ": " << bs.size() << "\n";
        for (auto& b : bs) {
            std::cout << "  " << leaf(b.r->path) << " (" << b.r->def << ")\n";
            for (auto& l : b.in) std::cout << "    in  " << l << "\n";
            for (auto& l : b.out) std::cout << "    out " << l << "\n";
        }
        for (auto& [net, srcs] : from) {
            auto it = to.find(net);
            if (it == to.end()) continue;
            for (auto& a : srcs)
                for (auto& b : it->second) std::cout << "  " << a << " -> " << b << "  (" << net << ")\n";
        }
        return true;
    }

    bool trace(const std::string& name, int depth) {
        const Symbol* s = find(name);
        if (!s) return false;
        std::set<const Symbol*> onPath;
        cone(s, depth, 0, onPath);
        return true;
    }

    std::string valueAt(const Symbol* s, uint64_t t) {
        const Wave* w = vcd.find(path(*s));
        if (!w) return "?";
        int i = w->at(t);
        return i < 0 ? "x" : showBits(w->bits(i));
    }

    // The elements of the unpacked array arr that e selects, as "[3]" or
    // "[1][2]", with the indices evaluated at ev's time. An index that cannot
    // be evaluated gives "?" in the list.
    void elemsIn(const Expression& e, const Symbol* arr, Eval& ev, std::vector<std::string>& out) {
        e.visit(makeVisitor([&](auto& v, const ElementSelectExpression& x) {
            // Walk down mem[i][j] to its base, collecting the indices.
            std::vector<const Expression*> ix;
            const Expression* b = &x;
            while (b->kind == ExpressionKind::ElementSelect && b->as<ElementSelectExpression>().value().type->isUnpackedArray()) {
                ix.push_back(&b->as<ElementSelectExpression>().selector());
                b = &b->as<ElementSelectExpression>().value();
            }
            const Symbol* sym = b->kind == ExpressionKind::NamedValue ? &b->as<NamedValueExpression>().symbol
                              : b->kind == ExpressionKind::HierarchicalValue ? &b->as<HierarchicalValueExpression>().symbol : nullptr;
            if (sym != arr || ix.empty()) { v.visitDefault(x); return; }
            std::string name;
            for (auto it = ix.rbegin(); it != ix.rend(); ++it) {
                auto i = ev.eval(**it);
                auto n = i && !i->hasUnknown() ? i->template as<int32_t>() : std::nullopt;
                if (!n) { name = "?"; break; }
                name += "[" + std::to_string(*n) + "]";
            }
            if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
            for (const Expression* i : ix) i->visit(v);
        }));
    }

    using Seen = std::set<std::tuple<const Symbol*, uint64_t, std::string>>;

    // The part of r that driver d reads (one struct field, some bits), when
    // it does not read r whole.
    bool readPart(const Driver& d, const Symbol* r, int& lo, int& w) {
        std::vector<const Expression*> conds{d.lhs};
        for (const Guard& g : d.guards) {
            conds.push_back(g.cond);
            if (!g.cs) continue;
            conds.push_back(&g.cs->expr);
            for (const auto& item : g.cs->items)
                for (const Expression* x : item.expressions) conds.push_back(x);
        }
        return partRead(d.rhs, conds, r, lo, w);
    }

    // The assignment that gave s its value at time t, then what caused it.
    // elem: one element of an unpacked array, "[3]", which has its own
    // waveform when msim ran with -vcd-arrays.
    // plo, pw: only these bits of s matter (a struct field that was read).
    void active(const Symbol* s, uint64_t t, int depth, int indent, Seen& seen, const std::string& elem = "",
                int plo = -1, int pw = 0) {
        std::string pad(indent * 2, ' ');
        std::string name = path(*s) + elem;
        const Wave* w = vcd.find(name);
        if (!w) {
            bool arr = s->kind == SymbolKind::Variable && s->as<ValueSymbol>().getType().isUnpackedArray();
            std::cout << pad << name << (arr ? "  (not in the waveform: run msim with -vcd-arrays)\n" : "  (not in the waveform)\n");
            return;
        }
        int ci = w->at(t);
        if (plo == 0 && ci >= 0 && pw >= (int)w->bits(ci).size()) plo = -1;   // all of it
        // A part: its own value, and the last time those bits changed.
        auto part = [&](int i) {
            std::string b = w->bits(i);
            return (int)b.size() >= plo + pw ? b.substr(b.size() - plo - pw, pw) : b;
        };
        int pi = ci;
        if (plo >= 0 && ci >= 0) {
            name += partName(*s, plo, pw);
            while (pi > 0 && part(pi - 1) == part(pi)) pi--;
        }
        uint64_t tc = pi < 0 ? 0 : w->ch[pi].t;
        std::cout << pad << name << " = " << (ci < 0 ? "x" : showBits(plo >= 0 ? part(ci) : w->bits(ci))) << " @" << t;
        if (tc != t) std::cout << "  (since " << tc << ")";
        std::cout << "\n";
        auto it = db.driversOf.find(s);
        if (it == db.driversOf.end()) { std::cout << pad << "  no driver: an input or a testbench value\n"; return; }
        if (!seen.insert({s, tc, plo < 0 ? elem : elem + "#" + std::to_string(plo) + ":" + std::to_string(pw)}).second) { std::cout << pad << "  (shown above)\n"; return; }

        // Which drivers ran? A flop's conditions are sampled before the edge.
        struct Hit { int ix; int state; uint64_t ts; };   // state: 1 yes, 0 no, -1 unknown
        std::vector<Hit> hits;
        for (int i : it->second) {
            const Driver& d = db.drivers[i];
            uint64_t ts = d.seq && tc > 0 ? tc - 1 : tc;
            Eval ev{vcd, ts};
            int state = 1;
            for (const Guard& g : d.guards) {
                auto h = ev.holds(g);
                if (!h) { if (state == 1) state = -1; }
                else if (!*h) { state = 0; break; }
            }
            // Of an array's drivers, only one that wrote this element.
            if (state && !elem.empty() && d.lhs) {
                std::vector<std::string> wrote;
                elemsIn(*d.lhs, s, ev, wrote);
                if (std::find(wrote.begin(), wrote.end(), elem) == wrote.end()) {
                    if (std::find(wrote.begin(), wrote.end(), "?") != wrote.end() || wrote.empty()) { if (state == 1 && !wrote.empty()) state = -1; }
                    else state = 0;
                }
            }
            hits.push_back({i, state, ts});
        }
        // An assignment whose right side does not give the value seen is not
        // the one (two assignments in one initial block, at different times).
        if (ci >= 0) {
            std::vector<bool> differs(hits.size(), false);
            bool match = false;
            for (size_t k = 0; k < hits.size(); k++) {
                const Driver& d = db.drivers[hits[k].ix];
                if (hits[k].state == 0 || !d.whole || !d.rhs) continue;
                Eval ev{vcd, hits[k].ts};
                auto v = ev.eval(*d.rhs);
                if (!v) continue;
                const std::string bits = w->bits(ci);
                SVInt seen = SVInt::fromString(std::to_string(bits.size()) + "'b" + bits);
                SVInt got = v->getBitWidth() < seen.getBitWidth() ? v->extend(seen.getBitWidth(), v->isSigned())
                          : v->getBitWidth() > seen.getBitWidth() ? v->trunc(seen.getBitWidth()) : *v;
                got.setSigned(false);
                if (exactlyEqual(got, seen)) match = true;
                else differs[k] = true;
            }
            if (match)
                for (size_t k = 0; k < hits.size(); k++)
                    if (differs[k]) hits[k].state = 0;
        }
        // In one process the last assignment that ran is the one that counts.
        // A later assignment to other bits (another field) does not replace it.
        auto covered = [&](size_t k) {
            const Driver& a = db.drivers[hits[k].ix];
            if (a.block < 0) return false;
            std::vector<std::pair<int, int>> later;   // [lo, end) of the assignments after it
            for (size_t j = k + 1; j < hits.size(); j++) {
                const Driver& b = db.drivers[hits[j].ix];
                if (hits[j].state != 1 || b.block != a.block) continue;
                if (b.whole) return true;
                if (b.lo >= 0) later.push_back({b.lo, b.lo + b.w});
                else if (a.lo < 0 && !a.whole) return true;
            }
            if (a.lo < 0) return false;
            std::sort(later.begin(), later.end());
            int at = a.lo;
            for (auto [l, e] : later) {
                if (l > at) break;
                at = std::max(at, e);
            }
            return at >= a.lo + a.w;
        };
        bool any = false;
        for (size_t k = 0; k < hits.size(); k++) {
            const Driver& d = db.drivers[hits[k].ix];
            if (hits[k].state == 0) continue;
            if (hits[k].state == 1 && covered(k)) continue;
            // An assignment to other bits is not a cause of this part.
            if (plo >= 0 && d.lo >= 0 && (d.lo + d.w <= plo || d.lo >= plo + pw)) continue;
            any = true;
            std::cout << pad << "  " << (hits[k].state == 1 ? "<- " : "?? ") << line(d) << "\n";
            if (depth <= 0) continue;
            uint64_t ts = hits[k].ts;
            // Conditions: their values at the sample time.
            std::string ctl;
            for (const Symbol* c : d.creads)
                if (db.sigIx.count(c)) ctl += (ctl.empty() ? "" : ", ") + std::string(c->name) + "=" + valueAt(c, ts);
            if (!ctl.empty()) std::cout << pad << "     with " << ctl << " @" << ts << "\n";
            // Combinational: a condition that moved at that time is a cause too.
            std::vector<const Symbol*> follow = d.reads;
            if (!d.seq)
                for (const Symbol* c : d.creads) {
                    const Wave* cw = db.sigIx.count(c) ? vcd.find(path(*c)) : nullptr;
                    int k2 = cw ? cw->at(tc) : -1;
                    if (k2 >= 0 && cw->ch[k2].t == tc && tc > 0 &&
                        std::find(follow.begin(), follow.end(), c) == follow.end())
                        follow.push_back(c);
                }
            for (const Symbol* r : follow) {
                if (!db.sigIx.count(r)) continue;
                const Wave* rw = vcd.find(path(*r));
                // Combinational: follow the input that moved at the same time.
                // With several inputs, the quiet ones are listed, not followed.
                if (!d.seq && rw && follow.size() > 1) {
                    int ri = rw->at(tc);
                    if (ri < 0 || rw->ch[ri].t != tc) {
                        std::cout << pad << "    " << path(*r) << " = " << valueAt(r, tc) << "  (unchanged @" << tc << ")\n";
                        continue;
                    }
                }
                // A memory: the element that was read, by the index at that time.
                if (!rw && r->kind == SymbolKind::Variable && r->as<ValueSymbol>().getType().isUnpackedArray()) {
                    Eval ev{vcd, ts};
                    std::vector<std::string> els;
                    if (d.rhs) elemsIn(*d.rhs, r, ev, els);
                    for (const Guard& g : d.guards) if (g.cond) elemsIn(*g.cond, r, ev, els);
                    bool any2 = false;
                    for (const std::string& el : els)
                        if (el != "?") { active(r, ts, depth - 1, indent + 2, seen, el); any2 = true; }
                    if (any2) continue;
                }
                // A struct: only the field that was read.
                int rlo = -1, rw2 = 0;
                if (!readPart(d, r, rlo, rw2)) rlo = -1;
                active(r, ts, depth - 1, indent + 2, seen, "", rlo, rw2);
            }
        }
        if (!any) std::cout << pad << "  no assignment's conditions hold at " << tc << " (" << it->second.size() << " drivers)\n";
    }

    bool activeTrace(const std::string& name, std::optional<uint64_t> at, int depth) {
        if (!haveVcd) { std::cerr << "mdebug: -active needs -vcd <file>\n"; return false; }
        // -active tb.mem[3]: one element of a memory.
        std::string base = name, elem;
        size_t dot = name.rfind('.');
        size_t br = name.find('[', dot == std::string::npos ? 0 : dot + 1);
        if (br != std::string::npos && name.back() == ']' && !db.byPath.count(name)) {
            base = name.substr(0, br);
            elem = name.substr(br);
        }
        const Symbol* s = find(base);
        if (!s) return false;
        Seen seen;
        active(s, at ? *at : vcd.end, depth, 0, seen, elem);
        return true;
    }

    // ------------------------------------------------------------ database

    static std::string js(std::string_view s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': break;
                case '\t': o += "\\t"; break;
                case '<': o += "\\u003c"; break;   // safe inside <script>
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                    else o += (char)c;
            }
        }
        return o + "\"";
    }

    static constexpr size_t kPageChanges = 2000000;

    // An expression for the viewer's own evaluator (the active-driver trace
    // in the page): nested arrays, "null" for what it does not follow.
    static std::string bitsOf(const SVInt& v) {
        std::string b;
        for (int i = (int)v.getBitWidth() - 1; i >= 0; i--) {
            logic_t l = v[i];
            b += l.isUnknown() ? (exactlyEqual(l, logic_t::z) ? 'z' : 'x') : (bool(l) ? '1' : '0');
        }
        return b;
    }
    static std::string constJson(const SVInt& v) {
        if (v.getBitWidth() > 4096) return "null";
        return "[\"c\",\"" + bitsOf(v) + "\"," + (v.isSigned() ? "1" : "0") + "]";
    }
    std::string symJson(const Symbol& sym, const Type& type) {
        if (sym.kind == SymbolKind::Parameter) {
            const ConstantValue& cv = sym.as<ParameterSymbol>().getValue();
            return cv.isInteger() ? constJson(cv.integer()) : "null";
        }
        if (sym.kind == SymbolKind::EnumValue) {
            const ConstantValue& cv = sym.as<EnumValueSymbol>().getValue();
            return cv.isInteger() ? constJson(cv.integer()) : "null";
        }
        auto it = db.sigIx.find(&sym);
        bool flat = type.getCanonicalType().kind == SymbolKind::UnpackedStructType;
        if (it == db.sigIx.end() || !(type.isIntegral() || flat)) return "null";
        return "[\"s\"," + std::to_string(it->second) + "," + (!flat && type.isSigned() ? "1" : "0") + "]";
    }
    std::string exprJson(const Expression& e) {
        auto bad = [](const std::string& x) { return x == "null"; };
        switch (e.kind) {
            case ExpressionKind::NamedValue: return symJson(e.as<NamedValueExpression>().symbol, *e.type);
            case ExpressionKind::HierarchicalValue: return symJson(e.as<HierarchicalValueExpression>().symbol, *e.type);
            case ExpressionKind::IntegerLiteral: return constJson(e.as<IntegerLiteral>().getValue());
            case ExpressionKind::UnbasedUnsizedIntegerLiteral: {
                if (!e.type->isIntegral() || e.type->getBitWidth() > 4096) return "null";
                logic_t l = e.as<UnbasedUnsizedIntegerLiteral>().getLiteralValue();
                char c = l.isUnknown() ? (exactlyEqual(l, logic_t::z) ? 'z' : 'x') : (bool(l) ? '1' : '0');
                return "[\"c\",\"" + std::string(e.type->getBitWidth(), c) + "\",0]";
            }
            case ExpressionKind::Conversion: {
                const auto& c = e.as<ConversionExpression>();
                std::string v = exprJson(c.operand());
                if (bad(v) || !e.type->isIntegral() || !c.operand().type->isIntegral()) return "null";
                return "[\"cv\"," + v + "," + std::to_string(e.type->getBitWidth()) + "," +
                       (c.operand().type->isSigned() ? "1" : "0") + "," + (e.type->isSigned() ? "1" : "0") + "]";
            }
            case ExpressionKind::UnaryOp: {
                const auto& u = e.as<UnaryExpression>();
                std::string v = exprJson(u.operand());
                if (bad(v)) return "null";
                return "[\"u\",\"" + std::string(toString(u.op)) + "\"," + v + "]";
            }
            case ExpressionKind::BinaryOp: {
                const auto& b = e.as<BinaryExpression>();
                std::string l = exprJson(b.left()), r = exprJson(b.right());
                if (bad(l) || bad(r)) return "null";
                return "[\"b\",\"" + std::string(toString(b.op)) + "\"," + l + "," + r + "]";
            }
            case ExpressionKind::ConditionalOp: {
                const auto& c = e.as<ConditionalExpression>();
                if (c.conditions.size() != 1 || c.conditions[0].pattern) return "null";
                std::string p = exprJson(*c.conditions[0].expr), l = exprJson(c.left()), r = exprJson(c.right());
                if (bad(p) || bad(l) || bad(r)) return "null";
                return "[\"?:\"," + p + "," + l + "," + r + "]";
            }
            case ExpressionKind::ElementSelect: {
                const auto& x = e.as<ElementSelectExpression>();
                const Type& vt = *x.value().type;
                if (!vt.isIntegral() || e.type->getBitWidth() != 1) return "null";
                std::string v = exprJson(x.value()), i = exprJson(x.selector());
                if (bad(v) || bad(i)) return "null";
                ConstantRange r = vt.getFixedRange();
                return "[\"bit\"," + v + "," + i + "," + std::to_string(r.left) + "," + std::to_string(r.right) + "]";
            }
            case ExpressionKind::MemberAccess: {
                const auto& m = e.as<MemberAccessExpression>();
                int lo, w;
                if (!fieldRange(m, lo, w)) return "null";
                std::string v = exprJson(m.value());
                if (bad(v)) return "null";
                return "[\"fld\"," + v + "," + std::to_string(lo) + "," + std::to_string(w) + "," +
                       (e.type->isSigned() ? "1" : "0") + "]";
            }
            case ExpressionKind::RangeSelect: {
                const auto& x = e.as<RangeSelectExpression>();
                const Type& vt = *x.value().type;
                if (!vt.isIntegral()) return "null";
                std::string v = exprJson(x.value()), l = exprJson(x.left()), r = exprJson(x.right());
                if (bad(v) || bad(l) || bad(r)) return "null";
                ConstantRange rg = vt.getFixedRange();
                int kind = x.getSelectionKind() == RangeSelectionKind::Simple ? 0
                         : x.getSelectionKind() == RangeSelectionKind::IndexedUp ? 1 : 2;
                return "[\"rng\"," + v + "," + l + "," + r + "," + std::to_string(kind) + "," +
                       std::to_string(rg.left) + "," + std::to_string(rg.right) + "]";
            }
            case ExpressionKind::Concatenation: {
                std::string o = "[\"cat\",[";
                bool first = true;
                for (const Expression* op : e.as<ConcatenationExpression>().operands()) {
                    std::string v = exprJson(*op);
                    if (bad(v)) return "null";
                    o += (first ? "" : ",") + v;
                    first = false;
                }
                return first ? "null" : o + "]]";
            }
            case ExpressionKind::Replication: {
                const auto& r = e.as<ReplicationExpression>();
                std::string n = exprJson(r.count()), v = exprJson(r.concat());
                if (bad(n) || bad(v)) return "null";
                return "[\"rep\"," + n + "," + v + "]";
            }
            default: return "null";
        }
    }
    // Case statements are shared by the drivers under them: one table.
    std::map<const CaseStatement*, int> caseIx;
    std::vector<const CaseStatement*> cases;
    std::string guardsJson(const Driver& d) {
        std::string o = "[";
        for (const Guard& g : d.guards) {
            if (o.size() > 1) o += ",";
            switch (g.kind) {
                case Guard::If: o += "[\"if\"," + exprJson(*g.cond) + "]"; break;
                case Guard::Else: o += "[\"else\"," + exprJson(*g.cond) + "]"; break;
                case Guard::CaseItem:
                case Guard::CaseDefault: {
                    auto it = caseIx.find(g.cs);
                    if (it == caseIx.end()) {
                        it = caseIx.emplace(g.cs, (int)cases.size()).first;
                        cases.push_back(g.cs);
                    }
                    o += "[\"case\"," + std::to_string(it->second) + "," +
                         (g.kind == Guard::CaseDefault ? "-1" : std::to_string(g.item)) + "]";
                    break;
                }
                default: o += "[\"?\"]"; break;
            }
        }
        return o + "]";
    }
    // -------------------------------------------------------------- FSMs
    // State machines come from the shared extractor (src/fsm_extract.hpp),
    // the one msim -cov and mlint use.
    std::vector<ExtractedFsm> fsms;

    static std::string stateName(const ExtractedFsm& f, uint64_t v) {
        for (auto& [val, name] : f.states)
            if (val == v) return name.empty() || std::isdigit((unsigned char)name[0]) ? std::to_string(v) : name;
        return std::to_string(v);
    }
    static bool isState(const ExtractedFsm& f, uint64_t v) {
        for (auto& st : f.states) if (st.first == v) return true;
        return false;
    }
    // The register's values in the waveform: (time, value) with unknown = ~0.
    std::vector<std::pair<uint64_t, uint64_t>> stateSeq(const ExtractedFsm& f) {
        std::vector<std::pair<uint64_t, uint64_t>> seq;
        const Wave* w = haveVcd && f.var ? vcd.find(path(*f.var)) : nullptr;
        if (!w) return seq;
        for (size_t i = 0; i < w->ch.size(); i++) {
            const std::string b = w->bits((int)i);
            uint64_t v = 0;
            bool unk = b.size() > 64;
            for (char c : b) { if (c != '0' && c != '1') unk = true; v = (v << 1) | (c == '1'); }
            seq.push_back({w->ch[i].t, unk ? ~0ull : v});
        }
        return seq;
    }

    // -fsm [NAME]: the state machines under an instance (or one register):
    // states, transitions and where they are written; with a waveform, the
    // state at -at T, how often each transition ran, the ones that never did
    // and any value that is not a state.
    bool fsm(const std::string& name, std::optional<uint64_t> at) {
        bool any = false;
        for (const ExtractedFsm& f : fsms) {
            if (!f.var || f.states.empty()) continue;
            const std::string reg = path(*f.var);
            if (!name.empty() && reg != name && reg.rfind(name + ".", 0) != 0) continue;
            any = true;
            std::cout << reg << "  (" << f.module << ", " << f.w << " bit" << (f.w == 1 ? "" : "s") << ", " << f.states.size()
                      << " states" << (f.onehot ? ", one-hot" : "") << ")  " << db.src.at(db.src.where(f.var->location)) << "\n";
            std::cout << "  states:";
            for (auto& [v, n] : f.states) {
                bool rst = std::find(f.resetStates.begin(), f.resetStates.end(), v) != f.resetStates.end();
                // A name that is a literal (2'h1) says no more than the value.
                bool lit = n.empty() || std::isdigit((unsigned char)n[0]);
                std::cout << " " << (lit ? std::to_string(v) : n + "=" + std::to_string(v)) << (rst ? "(reset)" : "");
            }
            std::cout << "\n";
            auto seq = stateSeq(f);
            const Wave* w = haveVcd ? vcd.find(reg) : nullptr;
            // Transitions seen: from -> to, count and first time.
            std::map<std::pair<uint64_t, uint64_t>, std::pair<size_t, uint64_t>> seen;
            std::map<uint64_t, size_t> visits;
            for (size_t i = 0; i < seq.size(); i++) {
                if (seq[i].second == ~0ull) continue;
                visits[seq[i].second]++;
                if (i && seq[i - 1].second != ~0ull) {
                    auto [it, fresh] = seen.try_emplace({seq[i - 1].second, seq[i].second}, 0, seq[i].first);
                    it->second.first++;
                }
            }
            std::set<std::pair<uint64_t, uint64_t>> known;
            for (const auto& t : f.trans) {
                if (t.from == t.to || !known.insert({t.from, t.to}).second) continue;
                std::cout << "  " << stateName(f, t.from) << " -> " << stateName(f, t.to) << "  "
                          << db.src.at(db.src.where(t.loc)) << (t.reset ? "  (reset)" : "");
                if (w) {
                    auto it = seen.find({t.from, t.to});
                    if (it == seen.end()) std::cout << "  never taken";
                    else std::cout << "  taken " << it->second.first << (it->second.first == 1 ? " time" : " times") << ", first @" << it->second.second;
                }
                std::cout << "\n";
            }
            if (!w) {
                if (haveVcd) std::cout << "  (not in the waveform)\n";
                continue;
            }
            for (auto& [k, v] : seen)
                if (!known.count(k))
                    std::cout << "  " << stateName(f, k.first) << " -> " << stateName(f, k.second) << "  taken " << v.first
                              << (v.first == 1 ? " time" : " times") << ", first @" << v.second
                              << (isState(f, k.first) && isState(f, k.second) ? "  (not a transition found in the source)\n" : "  (a value that is not a state)\n");
            for (auto& [v, n] : f.states)
                if (!visits.count(v)) std::cout << "  state " << (n.empty() ? std::to_string(v) : n) << " never entered\n";
            for (auto& [t, v] : seq)
                if (v != ~0ull && !isState(f, v)) { std::cout << "  value " << v << " @" << t << " is not a state\n"; break; }
            uint64_t t = at ? *at : vcd.end;
            int ci = w->at(t);
            if (ci >= 0) {
                uint64_t v = seq[ci].second;
                std::cout << "  @" << t << ": " << (v == ~0ull ? std::string("x") : stateName(f, v)) << "  (since " << seq[ci].first << ")";
                // The road to it: the last states before.
                int from = std::max(0, ci - 5);
                std::cout << "  after";
                for (int i = from; i < ci; i++)
                    std::cout << " " << (seq[i].second == ~0ull ? std::string("x") : stateName(f, seq[i].second)) << "@" << seq[i].first;
                if (from == ci) std::cout << " nothing";
                std::cout << "\n";
            }
        }
        if (!any) {
            std::cerr << "mdebug: no state machine " << (name.empty() ? "in the design" : "at " + name) << "\n";
            return name.empty();
        }
        return true;
    }

    std::string fsmsJson() {
        std::string o = "[";
        for (const ExtractedFsm& f : fsms) {
            auto ix = f.var ? db.sigIx.find(f.var) : db.sigIx.end();
            if (ix == db.sigIx.end() || f.states.empty()) continue;
            o += std::string(o.size() > 1 ? ",\n" : "") + "{\"sig\":" + std::to_string(ix->second) + ",\"states\":[";
            for (size_t i = 0; i < f.states.size(); i++) {
                bool rst = std::find(f.resetStates.begin(), f.resetStates.end(), f.states[i].first) != f.resetStates.end();
                o += std::string(i ? "," : "") + "[" + std::to_string(f.states[i].first) + "," +
                     js(stateName(f, f.states[i].first)) + "," + (rst ? "1" : "0") + "]";
            }
            o += "],\"trans\":[";
            std::set<std::pair<uint64_t, uint64_t>> known;
            bool first = true;
            for (const auto& t : f.trans) {
                if (t.from == t.to || !known.insert({t.from, t.to}).second) continue;
                Where wh = db.src.where(t.loc);
                o += std::string(first ? "" : ",") + "[" + std::to_string(t.from) + "," + std::to_string(t.to) + "," + (t.reset ? "1" : "0") +
                     "," + std::to_string(wh.file) + "," + std::to_string(wh.line) + "]";
                first = false;
            }
            o += "]}";
        }
        return o + "]";
    }

    // ----------------------------------------------------------- coverage
    // -cov FILE: msim's coverage database (msim -cov, or mcov merge). Line
    // hits say which assignments ever ran; toggles which bits ever moved.
    bool haveCov = false;
    std::map<std::pair<std::string, size_t>, uint64_t> covLines;    // (file name, line) -> hits
    std::map<std::pair<std::string, size_t>, uint64_t> covBase;     // by the name without directories
    struct Tog { int w = 0; std::string rise, fall; };              // hex masks of the bits seen 0->1, 1->0
    std::map<std::string, Tog> covTog;

    static std::string baseName(const std::string& f) { return f.substr(f.find_last_of('/') + 1); }

    bool readCov(const std::string& file) {
        std::ifstream is(file, std::ios::binary);
        if (!is) { std::cerr << "mdebug: cannot read " << file << "\n"; return false; }
        std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
        JReader r{text};
        JVal d = r.val();
        const JVal* files = d.get("files");
        const JVal* lines = d.get("lines");
        if (r.bad || d.k != JVal::Obj || !files || !lines) {
            std::cerr << "mdebug: " << file << " is not a coverage database (msim -cov FILE)\n";
            return false;
        }
        for (const JVal& l : lines->a) {
            if (l.a.size() < 3 || (size_t)l.a[0].n >= files->a.size()) continue;
            const std::string& f = files->a[(size_t)l.a[0].n].s;
            covLines[{f, (size_t)l.a[1].n}] += (uint64_t)l.a[2].n;
            covBase[{baseName(f), (size_t)l.a[1].n}] += (uint64_t)l.a[2].n;
        }
        if (const JVal* tg = d.get("toggles"))
            for (const JVal& t : tg->a)
                if (t.a.size() >= 4) covTog[t.a[0].s] = {(int)t.a[1].n, t.a[2].s, t.a[3].s};
        haveCov = true;
        return true;
    }

    // How often a source line ran; nullopt when the database has no record of it.
    std::optional<uint64_t> hitsOf(const Where& w) {
        if (!haveCov || w.file < 0) return std::nullopt;
        const std::string& f = db.src.files[w.file].name;
        if (auto it = covLines.find({f, (size_t)w.line}); it != covLines.end()) return it->second;
        if (auto it = covBase.find({baseName(f), (size_t)w.line}); it != covBase.end()) return it->second;
        return std::nullopt;
    }

    // The bits of a signal that never rose or never fell: "" when all did.
    std::string stuck(const std::string& p) {
        auto it = covTog.find(p);
        if (it == covTog.end()) return "";
        const Tog& t = it->second;
        auto count = [&](const std::string& hex) {
            int n = 0;
            for (char c : hex) {
                int v = c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
                n += __builtin_popcount(v & 15);
            }
            return n;
        };
        int r = count(t.rise), f = count(t.fall);
        if (r >= t.w && f >= t.w) return "";
        if (t.w == 1) return r == 0 && f == 0 ? "never changed" : r == 0 ? "never rose" : "never fell";
        std::string o;
        if (r < t.w) o = std::to_string(t.w - r) + " of " + std::to_string(t.w) + " bits never rose";
        if (f < t.w) o += (o.empty() ? "" : ", ") + std::to_string(t.w - f) + " of " + std::to_string(t.w) + " bits never fell";
        return o;
    }

    // -uncovered [SCOPE]: the assignments that never ran and the signals with
    // bits that never toggled, in the design or under one instance.
    bool uncovered(const std::string& scope) {
        if (!haveCov) { std::cerr << "mdebug: -uncovered needs -cov <file> (msim -cov FILE)\n"; return false; }
        const std::string pre = scope.empty() ? "" : scope + ".";
        std::set<std::pair<int, size_t>> done;
        size_t nd = 0, ns = 0;
        std::cout << "assignments on lines that never ran" << (scope.empty() ? "" : " under " + scope) << ":\n";
        for (const Driver& d : db.drivers) {
            if (!pre.empty() && path(*d.target).rfind(pre, 0) != 0) continue;
            if (d.kind == "port") continue;
            Where w = db.src.where(d.loc);
            auto h = hitsOf(w);
            if (!h || *h || !done.insert({w.file, w.line}).second) continue;
            std::cout << "  " << line(d) << "\n";
            nd++;
        }
        if (!nd) std::cout << "  none\n";
        std::cout << "signals that never toggled fully:\n";
        for (auto* s : db.signals) {
            const std::string p = path(*s);
            if (!pre.empty() && p.rfind(pre, 0) != 0) continue;
            std::string st = stuck(p);
            if (st.empty()) continue;
            std::cout << "  " << p << "  (" << st << ")\n";
            ns++;
        }
        if (!ns) std::cout << "  none\n";
        return true;
    }

    std::string covJson() {
        // Per file of the page's own file table: line -> hits; per signal: stuck text.
        std::string o = "{\"lines\":{";
        bool firstF = true;
        for (size_t f = 0; f < db.src.files.size(); f++) {
            std::string part;
            for (size_t l = 1; l <= db.src.files[f].lines.size(); l++) {
                Where w;
                w.file = (int)f;
                w.line = l;
                if (auto h = hitsOf(w)) part += (part.empty() ? "" : ",") + ("\"" + std::to_string(l) + "\":") + std::to_string(*h);
            }
            if (part.empty()) continue;
            o += std::string(firstF ? "" : ",") + "\"" + std::to_string(f) + "\":{" + part + "}";
            firstF = false;
        }
        o += "},\"stuck\":{";
        bool first = true;
        for (size_t i = 0; i < db.signals.size(); i++) {
            std::string st = stuck(path(*db.signals[i]));
            if (st.empty()) continue;
            o += std::string(first ? "" : ",") + "\"" + std::to_string(i) + "\":" + js(st);
            first = false;
        }
        return o + "}}";
    }

    // --------------------------------------------------------- assertions
    // The failures in a simulation log: msim (like VCS) prints
    //   Error: "file.sv", 12: tb.u.a_gnt: at time 55
    std::map<std::string, std::vector<uint64_t>> assertFails;   // by assertion name
    std::map<std::pair<std::string, size_t>, std::vector<uint64_t>> assertFailsAt;   // by file name and line
    bool haveLog = false;

    bool readLog(const std::string& file) {
        std::ifstream is(file);
        if (!is) { std::cerr << "mdebug: cannot read " << file << "\n"; return false; }
        haveLog = true;
        for (std::string ln; std::getline(is, ln);) {
            size_t q1 = ln.find('"'), q2 = q1 == std::string::npos ? q1 : ln.find('"', q1 + 1);
            size_t at = ln.rfind(": at time ");
            if (q2 == std::string::npos || at == std::string::npos || at < q2) continue;
            if (ln.rfind("Error:", 0) != 0 && ln.rfind("Fatal:", 0) != 0 && ln.rfind("Warning:", 0) != 0) continue;
            // "file", LINE: NAME: at time T
            size_t c1 = ln.find(':', q2);
            if (c1 == std::string::npos || c1 >= at) continue;
            std::string fname = ln.substr(q1 + 1, q2 - q1 - 1);
            size_t lineNo = std::strtoull(ln.c_str() + q2 + 2, nullptr, 10);
            std::string name = c1 + 2 <= at ? ln.substr(c1 + 2, at - c1 - 2) : "";
            uint64_t t = std::strtoull(ln.c_str() + at + 10, nullptr, 10);
            assertFails[name].push_back(t);
            assertFailsAt[{fname.substr(fname.find_last_of('/') + 1), lineNo}].push_back(t);
        }
        return true;
    }

    const std::vector<uint64_t>* failsOf(const AssertRec& a) {
        if (auto it = assertFails.find(a.name); it != assertFails.end()) return &it->second;
        Where w = db.src.where(a.loc);
        if (w.file < 0) return nullptr;
        const std::string& f = db.src.files[w.file].name;
        auto it = assertFailsAt.find({f.substr(f.find_last_of('/') + 1), (size_t)w.line});
        // By place only when one assertion is written there (one instance).
        if (it == assertFailsAt.end()) return nullptr;
        int same = 0;
        for (const AssertRec& b : db.asserts) {
            Where wb = db.src.where(b.loc);
            same += wb.file == w.file && wb.line == w.line;
        }
        return same == 1 ? &it->second : nullptr;
    }

    // -assert [NAME]: the assertions (all, under an instance, or one), and
    // with -log and a waveform, each failure: the values the assertion saw at
    // the clock ticks up to it, and what drove each signal.
    bool asserts(const std::string& name, std::optional<uint64_t> at) {
        bool any = false;
        for (const AssertRec& a : db.asserts) {
            if (!name.empty() && a.name != name && a.name.rfind(name + ".", 0) != 0) continue;
            any = true;
            std::cout << a.name << "  " << a.kind << "  " << db.src.at(db.src.where(a.loc)) << "\n";
            std::string text = db.src.text(a.range);
            if (text == "?") text = db.src.lineText(db.src.where(a.loc));
            if (text.size() > 160) text = text.substr(0, 157) + "...";
            for (char& ch : text) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            std::cout << "  " << text << "\n";
            const std::vector<uint64_t>* fails = haveLog ? failsOf(a) : nullptr;
            if (!haveLog) continue;
            if (!fails) { std::cout << "  no failure in the log\n"; continue; }
            std::cout << "  failed " << fails->size() << (fails->size() == 1 ? " time:" : " times:");
            for (size_t i = 0; i < fails->size() && i < 8; i++) std::cout << " @" << (*fails)[i];
            if (fails->size() > 8) std::cout << " ...";
            std::cout << "\n";
            if (!haveVcd) continue;
            // The failure to explain: the one at -at T, else the first.
            uint64_t tf = (*fails)[0];
            if (at) for (uint64_t t : *fails) if (t <= *at) tf = t;
            // The clock ticks up to the failure; values are sampled before each.
            std::vector<uint64_t> ticks;
            const Wave* cw = a.clock ? vcd.find(path(*a.clock)) : nullptr;
            if (cw) {
                for (int i = cw->at(tf); i >= 0 && ticks.size() < 4; i--) {
                    const std::string b = cw->bits(i);
                    bool hit = a.edge == EdgeKind::PosEdge ? b == "1" : a.edge == EdgeKind::NegEdge ? b == "0" : true;
                    if (hit && cw->ch[i].t <= tf) ticks.insert(ticks.begin(), cw->ch[i].t);
                }
            }
            if (ticks.empty()) ticks.push_back(tf);
            auto sample = [&](uint64_t t) { return a.concurrent && t > 0 ? t - 1 : t; };
            size_t wide = 4;
            std::vector<const Symbol*> sigs;
            for (const Symbol* r : a.reads)
                if (r != a.clock && db.sigIx.count(r)) { sigs.push_back(r); wide = std::max(wide, path(*r).size()); }
            std::cout << "  failure @" << tf << (a.concurrent ? ", values sampled before each clock tick:\n" : ", values then:\n");
            std::cout << "    " << std::string(wide, ' ');
            for (uint64_t t : ticks) { std::string h = "@" + std::to_string(t); std::cout << "  " << std::string(h.size() < 8 ? 8 - h.size() : 0, ' ') << h; }
            std::cout << "\n";
            for (const Symbol* r : sigs) {
                const std::string p = path(*r);
                std::cout << "    " << p << std::string(wide - p.size(), ' ');
                for (uint64_t t : ticks) { std::string v = valueAt(r, sample(t)); std::cout << "  " << std::string(v.size() < 8 ? 8 - v.size() : 0, ' ') << v; }
                std::cout << "\n";
            }
            std::cout << "  what drove them:\n";
            Seen seen;
            for (const Symbol* r : sigs) active(r, sample(tf), 1, 2, seen);
        }
        if (!any) {
            std::cerr << "mdebug: no assertion " << (name.empty() ? "in the design" : "at " + name) << "\n";
            return name.empty();
        }
        return true;
    }

    std::string assertsJson() {
        std::string o = "[";
        for (size_t i = 0; i < db.asserts.size(); i++) {
            const AssertRec& a = db.asserts[i];
            Where w = db.src.where(a.loc);
            o += std::string(i ? ",\n" : "") + "{\"name\":" + js(a.name) + ",\"kind\":" + js(a.kind) + ",\"file\":" +
                 std::to_string(w.file) + ",\"line\":" + std::to_string(w.line) + ",\"reads\":[";
            bool first = true;
            for (const Symbol* r : a.reads)
                if (auto it = db.sigIx.find(r); it != db.sigIx.end()) { o += (first ? "" : ",") + std::to_string(it->second); first = false; }
            o += "],\"fails\":[";
            if (const std::vector<uint64_t>* f = haveLog ? failsOf(a) : nullptr)
                for (size_t k = 0; k < f->size() && k < 1000; k++) o += (k ? "," : "") + std::to_string((*f)[k]);
            o += "]}";
        }
        return o + "]";
    }

    // ------------------------------------------------------------- script
    // -script FILE (- for stdin): one command per line, one JSON object per
    // answer line. The design and the waveform are loaded once, so a script
    // or another program can ask many questions of one mdebug process.
    std::string drvJson(const Driver& d) {
        Where w = db.src.where(d.loc);
        return "{\"file\":" + js(w.file < 0 ? "" : db.src.files[w.file].name) + ",\"line\":" + std::to_string(w.line) +
               ",\"text\":" + js(text(d)) + ",\"kind\":" + js(label(d)) + ",\"when\":" + js(when(d)) +
               ",\"target\":" + js(path(*d.target)) + "}";
    }
    int script(std::istream& in, std::ostream& out) {
        int bad = 0;
        std::string ln;
        while (std::getline(in, ln)) {
            std::istringstream is(ln);
            std::vector<std::string> a;
            for (std::string t; is >> t;) a.push_back(t);
            if (a.empty() || a[0][0] == '#') continue;
            if (a[0] == "quit" || a[0] == "exit") break;
            auto num = [&](size_t i) -> std::optional<uint64_t> {
                if (i >= a.size()) return std::nullopt;
                return std::strtoull(a[i].c_str(), nullptr, 10);
            };
            // What the query functions print is captured, never mixed into the answers.
            std::ostringstream txt, err;
            std::streambuf* so = std::cout.rdbuf(txt.rdbuf());
            std::streambuf* se = std::cerr.rdbuf(err.rdbuf());
            bool ok = true, asText = false;
            std::string body;
            const std::string& c = a[0];
            auto sig = [&]() -> const Symbol* {
                if (a.size() < 2) { std::cerr << "mdebug: " << c << " needs a signal\n"; return nullptr; }
                return find(a[1]);
            };
            auto needWave = [&](const Symbol* s) -> const Wave* {
                if (!haveVcd) { std::cerr << "mdebug: " << c << " needs -vcd <file>\n"; return nullptr; }
                const Wave* w = vcd.find(path(*s));
                if (!w) std::cerr << "mdebug: " << path(*s) << " is not in the waveform\n";
                return w;
            };
            if (c == "drivers" || c == "loads") {
                const Symbol* s = sig();
                if (!s) ok = false;
                else {
                    body = "\"signal\":" + js(path(*s)) + ",\"" + c + "\":[";
                    auto& v = c == "drivers" ? db.driversOf[s] : db.loadsOf[s];
                    for (size_t i = 0; i < v.size(); i++) {
                        const Driver& d = db.drivers[v[i]];
                        std::string o = drvJson(d);
                        if (c == "loads") {
                            bool data = std::find(d.reads.begin(), d.reads.end(), s) != d.reads.end();
                            o.insert(o.size() - 1, std::string(",\"control\":") + (data ? "false" : "true"));
                        }
                        body += (i ? "," : "") + o;
                    }
                    body += "]";
                }
            } else if (c == "value") {
                const Symbol* s = sig();
                const Wave* w = s ? needWave(s) : nullptr;
                if (!w) ok = false;
                else {
                    uint64_t t = num(2).value_or(vcd.end);
                    int i = w->at(t);
                    body = "\"signal\":" + js(path(*s)) + ",\"time\":" + std::to_string(t) + ",\"value\":" +
                           js(i < 0 ? "x" : showBits(w->bits(i))) + ",\"bits\":" + js(i < 0 ? "" : w->bits(i)) +
                           ",\"since\":" + std::to_string(i < 0 ? 0 : w->ch[i].t);
                }
            } else if (c == "changes") {
                const Symbol* s = sig();
                const Wave* w = s ? needWave(s) : nullptr;
                if (!w) ok = false;
                else {
                    uint64_t t0 = num(2).value_or(0), t1 = num(3).value_or(vcd.end);
                    const size_t cap = 10000;
                    size_t n = 0;
                    bool more = false;
                    body = "\"signal\":" + js(path(*s)) + ",\"changes\":[";
                    for (int i = std::max(0, w->at(t0)); i < (int)w->ch.size() && w->ch[i].t <= t1; i++) {
                        if (w->ch[i].t < t0) continue;
                        if (n == cap) { more = true; break; }
                        body += std::string(n++ ? "," : "") + "[" + std::to_string(w->ch[i].t) + "," + js(w->bits(i)) + "]";
                    }
                    body += std::string("],\"more\":") + (more ? "true" : "false");
                }
            } else if (c == "scopes") {
                body = "\"scopes\":[";
                for (size_t i = 0; i < db.scopes.size(); i++) {
                    Where w = db.src.where(db.scopes[i].loc);
                    body += std::string(i ? "," : "") + "{\"path\":" + js(db.scopes[i].path) + ",\"def\":" + js(db.scopes[i].def) +
                            ",\"file\":" + js(w.file < 0 ? "" : db.src.files[w.file].name) + ",\"line\":" + std::to_string(w.line) + "}";
                }
                body += "]";
            } else if (c == "signals") {
                // signals, or signals SCOPE: the ones directly inside it.
                const std::string pre = a.size() > 1 ? a[1] + "." : "";
                body = "\"signals\":[";
                bool first = true;
                for (auto* s : db.signals) {
                    const std::string p = path(*s);
                    if (!pre.empty() && (p.rfind(pre, 0) != 0 || p.find('.', pre.size()) != std::string::npos)) continue;
                    Where w = db.src.where(s->location);
                    body += std::string(first ? "" : ",") + "{\"path\":" + js(p) + ",\"type\":" + js(s->getType().toString()) +
                            ",\"file\":" + js(w.file < 0 ? "" : db.src.files[w.file].name) + ",\"line\":" + std::to_string(w.line) + "}";
                    first = false;
                }
                body += "]";
            } else if (c == "end") {
                if (!haveVcd) { std::cerr << "mdebug: end needs -vcd <file>\n"; ok = false; }
                else body = "\"end\":" + std::to_string(vcd.end) + ",\"timescale\":" + js(vcd.timescale);
            } else if (c == "active") {
                asText = true;
                ok = a.size() > 1 ? activeTrace(a[1], num(2), (int)num(3).value_or(3)) : (std::cerr << "mdebug: active needs a signal\n", false);
            } else if (c == "trace") {
                asText = true;
                ok = a.size() > 1 ? trace(a[1], (int)num(2).value_or(2)) : (std::cerr << "mdebug: trace needs a signal\n", false);
            } else if (c == "blocks") {
                asText = true;
                ok = a.size() > 1 ? blocks(a[1]) : (std::cerr << "mdebug: blocks needs an instance\n", false);
            } else if (c == "fsm") {
                // fsm, fsm NAME, fsm NAME T
                asText = true;
                bool nameIsTime = a.size() == 2 && std::isdigit((unsigned char)a[1][0]);
                ok = fsm(a.size() > 1 && !nameIsTime ? a[1] : "", nameIsTime ? num(1) : num(2));
            } else if (c == "uncovered") {
                asText = true;
                ok = uncovered(a.size() > 1 ? a[1] : "");
            } else if (c == "assert") {
                asText = true;
                bool nameIsTime = a.size() == 2 && std::isdigit((unsigned char)a[1][0]);
                ok = asserts(a.size() > 1 && !nameIsTime ? a[1] : "", nameIsTime ? num(1) : num(2));
            } else if (c == "help") {
                body = "\"commands\":[\"drivers SIG\",\"loads SIG\",\"value SIG [T]\",\"changes SIG [T0 [T1]]\",\"active SIG [T [DEPTH]]\","
                       "\"trace SIG [DEPTH]\",\"blocks INST\",\"fsm [NAME] [T]\",\"assert [NAME] [T]\",\"uncovered [INST]\",\"scopes\",\"signals [SCOPE]\",\"end\",\"quit\"]";
            } else {
                std::cerr << "mdebug: unknown command '" << c << "' (help lists them)\n";
                ok = false;
            }
            std::cout.rdbuf(so);
            std::cerr.rdbuf(se);
            std::string e = err.str();
            while (!e.empty() && e.back() == '\n') e.pop_back();
            if (e.rfind("mdebug: ", 0) == 0) e = e.substr(8);
            out << "{\"cmd\":" << js(ln) << ",\"ok\":" << (ok ? "true" : "false");
            if (!ok) { out << ",\"error\":" << js(e); bad++; }
            if (!body.empty()) out << "," << body;
            if (asText && ok) {
                out << ",\"text\":[";
                std::istringstream ts(txt.str());
                bool first = true;
                for (std::string l; std::getline(ts, l);) { out << (first ? "" : ",") << js(l); first = false; }
                out << "]";
            }
            out << "}" << std::endl;   // flushed: the caller may be waiting on a pipe
        }
        return bad;
    }

    std::string casesJson() {
        std::string o = "[";
        for (size_t i = 0; i < cases.size(); i++) {
            const CaseStatement& c = *cases[i];
            const char* k = c.condition == CaseStatementCondition::Normal ? "n"
                          : c.condition == CaseStatementCondition::WildcardJustZ ? "z"
                          : c.condition == CaseStatementCondition::WildcardXOrZ ? "x" : "?";
            o += std::string(i ? ",\n" : "") + "{\"k\":\"" + k + "\",\"sel\":" + exprJson(c.expr) + ",\"items\":[";
            for (size_t j = 0; j < c.items.size(); j++) {
                o += j ? ",[" : "[";
                bool first = true;
                for (const Expression* e : c.items[j].expressions) {
                    o += (first ? "" : ",") + exprJson(*e);
                    first = false;
                }
                o += "]";
            }
            o += "]}";
        }
        return o + "]";
    }

    std::optional<uint64_t> center;   // -at T

    void json(std::ostream& os, size_t maxChanges) {
        // Resolve every place first, so the file table is complete.
        std::vector<Where> sw, sd, si, dw;
        for (auto* s : db.signals) sw.push_back(db.src.where(s->location));
        for (auto& s : db.scopes) { sd.push_back(db.src.where(s.loc)); si.push_back(db.src.where(s.inst)); }
        for (auto& d : db.drivers) dw.push_back(db.src.where(d.loc));
        const std::string fsmText = fsmsJson(), assertText = assertsJson();
        os << "{\"version\":" << js(MINISIM_VERSION) << ",\n\"files\":[";
        for (size_t i = 0; i < db.src.files.size(); i++) {
            auto& f = db.src.files[i];
            os << (i ? ",\n" : "") << "{\"name\":" << js(f.name) << ",\"text\":"
               << js(db.src.sm->getSourceText(f.buf)) << "}";
        }
        os << "],\n\"scopes\":[";
        for (size_t i = 0; i < db.scopes.size(); i++)
            os << (i ? ",\n" : "") << "{\"path\":" << js(db.scopes[i].path) << ",\"def\":" << js(db.scopes[i].def)
               << ",\"file\":" << sd[i].file << ",\"line\":" << sd[i].line << ",\"ifile\":" << si[i].file
               << ",\"iline\":" << si[i].line << "}";
        os << "],\n\"signals\":[";
        for (size_t i = 0; i < db.signals.size(); i++) {
            const ValueSymbol* s = db.signals[i];
            const Type& ty = s->getType();
            os << (i ? ",\n" : "") << "{\"path\":" << js(path(*s)) << ",\"width\":"
               << (ty.isIntegral() ? (int)ty.getBitWidth() : 0) << ",\"type\":" << js(ty.toString())
               << ",\"file\":" << sw[i].file << ",\"line\":" << sw[i].line << "}";
        }
        os << "],\n\"drivers\":[";
        auto ids = [&](const std::vector<const Symbol*>& v) {
            std::string o = "[";
            for (const Symbol* s : v)
                if (auto it = db.sigIx.find(s); it != db.sigIx.end())
                    o += (o.size() > 1 ? "," : "") + std::to_string(it->second);
            return o + "]";
        };
        // "part": the signals a driver reads only a part of, as [lo, width, name].
        auto parts = [&](const Driver& d) {
            std::string o;
            std::vector<const Symbol*> all = d.reads;
            all.insert(all.end(), d.creads.begin(), d.creads.end());
            std::set<const Symbol*> done;
            for (const Symbol* r : all) {
                auto ix = db.sigIx.find(r);
                int lo, w;
                if (ix == db.sigIx.end() || !done.insert(r).second || !readPart(d, r, lo, w)) continue;
                o += (o.empty() ? "" : ",") + ("\"" + std::to_string(ix->second) + "\":[") + std::to_string(lo) + "," +
                     std::to_string(w) + "," + js(partName(*r, lo, w)) + "]";
            }
            return o.empty() ? o : ",\"part\":{" + o + "}";
        };
        bool first = true;
        for (size_t i = 0; i < db.drivers.size(); i++) {
            const Driver& d = db.drivers[i];
            auto it = db.sigIx.find(d.target);
            if (it == db.sigIx.end()) continue;
            os << (first ? "" : ",\n") << "{\"sig\":" << it->second << ",\"kind\":" << js(label(d))
               << ",\"file\":" << dw[i].file << ",\"line\":" << dw[i].line << ",\"text\":" << js(text(d))
               << ",\"when\":" << js(when(d)) << ",\"seq\":" << (d.seq ? 1 : 0) << ",\"reads\":" << ids(d.reads)
               << ",\"ctl\":" << ids(d.creads) << ",\"blk\":" << d.block << ",\"whole\":" << (d.whole ? 1 : 0) << ",\"lo\":" << d.lo << ",\"w\":" << d.w
               << parts(d) << ",\"g\":" << guardsJson(d) << ",\"rhs\":" << (d.rhs && d.whole ? exprJson(*d.rhs) : "null") << "}";
            first = false;
        }
        os << "],\n\"cases\":" << casesJson() << ",\n\"fsms\":" << fsmText << ",\n\"asserts\":" << assertText;
        if (haveCov) os << ",\n\"cov\":" << covJson();
        if (haveVcd) {
            os << ",\n\"end\":" << vcd.end << ",\"at\":" << (center ? std::min(*center, vcd.end) : vcd.end) << ",\"timescale\":" << js(vcd.timescale) << ",\"waves\":{";
            first = true;
            std::vector<std::string> part(db.signals.size());
            std::vector<const Wave*> wv(db.signals.size());
            for (size_t i = 0; i < db.signals.size(); i++) wv[i] = vcd.find(path(*db.signals[i]));
            // A page a browser can open: a budget of changes over all signals.
            size_t waved = 0, cutSigs = 0;
            for (const Wave* w : wv) waved += w != nullptr;
            maxChanges = std::min(maxChanges, std::max<size_t>(64, kPageChanges / std::max<size_t>(waved, 1)));
            for (const Wave* w : wv) cutSigs += w && w->ch.size() > maxChanges;
            if (cutSigs)
                std::cerr << "mdebug: the waves of " << cutSigs << " signals are cut to " << maxChanges << " changes each in the page, "
                          << (center ? "around -at" : "from the start; -at T keeps the changes around T")
                          << " (-max-changes N for more per signal)\n";
            parallelFor(db.signals.size(), threads, [&](size_t i) {
                const Wave* w = wv[i];
                if (!w) return;
                std::string& o = part[i];
                o = "\"" + std::to_string(i) + "\":[";
                // Cut waves keep the window around -at T (else the start).
                size_t n = std::min(w->ch.size(), maxChanges), k0 = 0;
                if (center && n < w->ch.size()) {
                    int c = w->at(*center);
                    k0 = (size_t)std::max(0, c - (int)(n / 2));
                    k0 = std::min(k0, w->ch.size() - n);
                }
                for (size_t k = k0; k < k0 + n; k++) {
                    if (k > k0) o += ',';
                    o += '[';
                    o += std::to_string(w->ch[k].t);
                    o += ",\"";
                    o += w->bits((int)k);
                    o += "\"]";
                }
                o += ']';
            });
            for (size_t i = 0; i < part.size(); i++) {
                if (!wv[i]) continue;
                os << (first ? "" : ",\n") << part[i];
                first = false;
            }
            os << "}";
        }
        os << "}\n";
    }
};

void usage() {
    std::cout << "usage: mdebug [options] file.sv ... <queries>\n"
                 "  source-level debug on the msim front end: drivers, loads, tracing,\n"
                 "  and the active driver at a time in a waveform\n\n"
                 "  -drivers SIG     every assignment that can give SIG a value\n"
                 "  -loads SIG       every assignment that reads SIG\n"
                 "  -trace SIG       the fan-in cone of SIG (-depth N levels, default 2)\n"
                 "  -active SIG      with -vcd: the assignment that gave SIG its value at\n"
                 "                   -at T (default: the end), then what caused it,\n"
                 "                   -depth N steps back through time\n"
                 "  -vcd <file>      the waveform msim wrote: VCD or FST (-fst, -wave are the same)\n"
                 "  -at T            a time, in the VCD's time unit\n"
                 "  -threads N       threads for parsing and for reading the waveform\n"
                 "                   (default: every logical core; the output is the same for any N)\n"
                 "  -scopes          list the instances    -signals  list the signals\n"
                 "  -fsm [NAME]      the state machines (all, under an instance, or one register): states,\n"
                 "                   transitions and their lines; with -vcd, the state at -at T, how often\n"
                 "                   each transition ran, the ones never taken and values that are not states\n"
                 "  -assert [NAME]   the assertions (all, under an instance, or one); with -log LOG and -vcd,\n"
                 "                   each failure: what the assertion sampled at the clock ticks before it\n"
                 "                   and what drove each signal (-at T picks the failure at or before T)\n"
                 "  -cov FILE        a coverage database (msim -cov FILE): driver lines say how often they\n"
                 "                   ran, and the page marks the lines that ran and those that never did\n"
                 "  -uncovered [INST]  with -cov: the assignments that never ran and the signals with bits\n"
                 "                   that never toggled, in the design or under INST\n"
                 "  -log FILE        the simulation log with the assertion failures (msim's output)\n"
                 "  -script FILE     run commands from FILE (- for the standard input), one per line, and\n"
                 "                   answer each with one line of JSON: drivers, loads, value, changes,\n"
                 "                   active, trace, blocks, fsm, scopes, signals, end, help, quit\n"
                 "  -blocks INST     the instances inside INST, their port connections and what feeds what\n"
                 "  -html <file>     write a one-file viewer: hierarchy, source, drivers and\n"
                 "                   loads, a schematic of the cone, and waves when -vcd is given\n"
                 "  -json <file>     write the debug database the viewer reads\n"
                 "  -top M           the top module    -f <file>  an argument file\n"
                 "  +define+N[=V]  +incdir+<dir>  -I <dir>  -D N[=V]\n"
                 "  -version         print the version\n"
                 "  --help-slang     list every front-end option\n\n"
                 "  SIG is a hierarchical path (tb.u.q); a unique tail (u.q) is enough.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fwd{argv[0]};
    struct Query { std::string what, arg; int depth; std::optional<uint64_t> at; };
    std::vector<Query> queries;
    std::string vcdPath, htmlPath, jsonPath, scriptPath, logPath, covPath;
    int depth = -1;
    std::optional<uint64_t> at;
    size_t maxChanges = 20000;
    // Every logical core by default (hyper-threads included); -threads 1 for one.
    unsigned threads = par::defaultThreads();
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "mdebug: " << a << " needs " << what << "\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help" || a == "-help") { usage(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mdebug " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "-fsm") {
            // -fsm, or -fsm NAME: an instance or a state register.
            std::string arg;
            if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' && !std::filesystem::exists(argv[i + 1])) arg = argv[++i];
            queries.push_back({"fsm", arg, -1, std::nullopt});
            continue;
        }
        if (a == "-assert") {
            std::string arg;
            if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' && !std::filesystem::exists(argv[i + 1])) arg = argv[++i];
            queries.push_back({"assert", arg, -1, std::nullopt});
            continue;
        }
        if (a == "-cov") { covPath = next("a coverage database"); continue; }
        if (a == "-uncovered") {
            std::string arg;
            if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+' && !std::filesystem::exists(argv[i + 1])) arg = argv[++i];
            queries.push_back({"uncovered", arg, -1, std::nullopt});
            continue;
        }
        if (a == "-log") { logPath = next("a simulation log"); continue; }
        if (a == "-script") { scriptPath = next("a file, or - for the standard input"); continue; }
        if (a == "-blocks") { queries.push_back({"blocks", next("an instance"), -1, std::nullopt}); continue; }
        if (a == "-drivers" || a == "-loads" || a == "-trace" || a == "-active") {
            queries.push_back({a.substr(1), next("a signal"), -1, std::nullopt});
            continue;
        }
        if (a == "-scopes" || a == "-signals") { queries.push_back({a.substr(1), "", -1, std::nullopt}); continue; }
        if (a == "-depth") { depth = std::atoi(next("a number").c_str()); continue; }
        if (a == "-at") { at = std::strtoull(next("a time").c_str(), nullptr, 10); continue; }
        if (a == "-vcd" || a == "-fst" || a == "-wave") { vcdPath = next("a VCD file"); continue; }
        if (a == "-html") { htmlPath = next("a file"); continue; }
        if (a == "-json") { jsonPath = next("a file"); continue; }
        if (par::threadsArg(argc, argv, i, threads, "mdebug")) continue;
        if (a == "-max-changes") { maxChanges = std::strtoull(next("a number").c_str(), nullptr, 10); continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        fwd.push_back(a);
    }
    if (queries.empty() && htmlPath.empty() && jsonPath.empty() && scriptPath.empty() && fwd.size() == 1) { usage(); return 2; }
    // Every instance gets its own symbols, so tb.a.q and tb.b.q are two signals.
    fwd.push_back("--disable-instance-caching");
    fwd.push_back("--threads");   // the front end parses files in parallel
    fwd.push_back(std::to_string(threads));

    FrontEnd fe;
    if (int rc = fe.run(fwd, "mdebug front end")) return rc < 0 ? 0 : rc;

    Tool tool;
    tool.threads = threads;
    tool.center = at;
    tool.db.src.sm = fe.comp->getSourceManager();
    Collect c(tool.db);
    fe.comp->getRoot().visit(c);

    tool.fsms = extractFsms(fe.comp->getRoot());
    if (!logPath.empty() && !tool.readLog(logPath)) return 2;
    if (!covPath.empty() && !tool.readCov(covPath)) return 2;
    if (!vcdPath.empty()) {
        std::string err;
        if (!tool.vcd.open(vcdPath, threads, err)) { std::cerr << "mdebug: " << err << "\n"; return 2; }
        tool.haveVcd = true;
    }

    bool ok = true;
    for (const Query& q : queries) {
        if (q.what == "drivers") ok &= tool.drivers(q.arg);
        else if (q.what == "loads") ok &= tool.loads(q.arg);
        else if (q.what == "blocks") ok &= tool.blocks(q.arg);
        else if (q.what == "fsm") ok &= tool.fsm(q.arg, at);
        else if (q.what == "assert") ok &= tool.asserts(q.arg, at);
        else if (q.what == "uncovered") ok &= tool.uncovered(q.arg);
        else if (q.what == "trace") ok &= tool.trace(q.arg, depth < 0 ? 2 : depth);
        else if (q.what == "active") ok &= tool.activeTrace(q.arg, at, depth < 0 ? 3 : depth);
        else if (q.what == "scopes")
            for (auto& s : tool.db.scopes)
                std::cout << s.path << "  (" << s.def << ")  " << tool.db.src.at(tool.db.src.where(s.loc)) << "\n";
        else if (q.what == "signals")
            for (auto* s : tool.db.signals)
                std::cout << path(*s) << "  " << s->getType().toString() << "  "
                          << tool.db.src.at(tool.db.src.where(s->location)) << "\n";
    }

    if (!scriptPath.empty()) {
        if (scriptPath == "-") ok &= tool.script(std::cin, std::cout) == 0;
        else {
            std::ifstream is(scriptPath);
            if (!is) { std::cerr << "mdebug: cannot read " << scriptPath << "\n"; return 2; }
            ok &= tool.script(is, std::cout) == 0;
        }
    }
    if (!jsonPath.empty()) {
        std::ofstream os(jsonPath);
        if (!os) { std::cerr << "mdebug: cannot write " << jsonPath << "\n"; return 2; }
        tool.json(os, maxChanges);
    }
    if (!htmlPath.empty()) {
        std::ofstream os(htmlPath);
        if (!os) { std::cerr << "mdebug: cannot write " << htmlPath << "\n"; return 2; }
        std::ostringstream data;
        tool.json(data, maxChanges);
        std::string page = kViewer;
        const std::string mark = "/*MDEBUG_DATA*/null";
        size_t p = page.find(mark);
        os << page.substr(0, p) << data.str() << page.substr(p + mark.size());
        std::cout << "mdebug: wrote " << htmlPath << " (" << tool.db.scopes.size() << " instances, "
                  << tool.db.signals.size() << " signals, " << tool.db.drivers.size() << " drivers)\n";
    }
    return ok ? 0 : 1;
}
