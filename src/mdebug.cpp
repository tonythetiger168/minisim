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
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "lintir.hpp"
#include "slang/ast/expressions/ConversionExpression.h"
#include "slang/ast/expressions/LiteralExpressions.h"
#include "slang/ast/expressions/OperatorExpressions.h"
#include "slang/ast/expressions/SelectExpressions.h"
#include "slang/ast/statements/LoopStatements.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/types/AllTypes.h"
#include "version.hpp"
#ifdef MINISIM_FST
#include "fstapi.h"
#endif

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
    std::string kind;                     // assign, always_ff, always_comb, always, initial, final, port
    SourceLocation loc;
    SourceRange range;                    // the assignment as written; empty for a port
    const Expression* rhs = nullptr;      // null for an output port
    std::vector<Guard> guards;
    bool seq = false;
    bool whole = false;                   // assigns the whole target, not a select of it
    std::string clock;                    // the event control as written, when seq
    int block = -1;                       // which process; -1 outside one
    std::vector<const Symbol*> reads;     // data: the right-hand side
    std::vector<const Symbol*> creads;    // control: guards and select indices
};

struct ScopeRec {
    std::string path, def;
    SourceLocation loc;                   // the module definition
    SourceLocation inst;                  // where it is instantiated
};

struct DB {
    Sources src;
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

    void assign(const AssignmentExpression& a) {
        std::vector<const Symbol*> targets, idx;
        lhsTargets(a.left(), targets, idx);
        for (const Symbol* t : targets) {
            Driver d = proto;
            d.target = t;
            d.loc = a.sourceRange.start();
            d.range = a.sourceRange;
            d.rhs = &a.right();
            d.whole = a.left().kind == ExpressionKind::NamedValue ||
                      a.left().kind == ExpressionKind::HierarchicalValue;
            d.guards = gs;
            namesIn(a.right(), d.reads);
            d.creads = idx;
            for (const Guard& g : gs) {
                if (g.cond) namesIn(*g.cond, d.creads);
                if (g.cs) {
                    namesIn(g.cs->expr, d.creads);
                    if (g.kind == Guard::CaseItem)
                        for (const Expression* e : g.cs->items[g.item].expressions) namesIn(*e, d.creads);
                }
            }
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
                    [&](auto&, const AssignmentExpression& a) { assign(a); }));
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
    void handle(const NetSymbol& v) { signal(v); }

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

// One value change: when, and where its bits are in the mapped file.
struct Change {
    uint64_t t;
    uint64_t off : 40;
    uint64_t len : 24;
};

struct Wave {
    int width = 1;
    const char* base = nullptr;
    std::vector<Change> ch;

    // The last change at or before t, or -1.
    int at(uint64_t t) const {
        int lo = 0, hi = (int)ch.size();
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (ch[mid].t <= t) lo = mid + 1; else hi = mid;
        }
        return lo - 1;
    }

    // The bits of change i, msb first, lower case, at the full width.
    std::string bits(int i) const {
        std::string b(base + ch[i].off, ch[i].len);
        for (char& c : b) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        // A VCD drops leading bits: 0 unless the first bit is x or z.
        if ((int)b.size() < width) {
            char pad = (b[0] == 'x' || b[0] == 'z') ? b[0] : '0';
            b.insert(0, width - b.size(), pad);
        }
        return b;
    }
};

// Run f(0..n-1) on up to `threads` threads. The work is split by index, so
// the result does not depend on the thread count.
template<typename F>
void parallelFor(size_t n, unsigned threads, F&& f) {
    if (threads <= 1 || n <= 1) {
        for (size_t i = 0; i < n; i++) f(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    unsigned k = (unsigned)std::min<size_t>(threads, n);
    for (unsigned t = 0; t < k; t++)
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) f(i);
        });
    for (auto& th : pool) th.join();
}

// A VCD reader built for large dumps: the file is mapped, the body is cut at
// time stamps into one piece per thread, and every piece is scanned twice,
// once to count each signal's changes and once to write them in place. No
// value is copied: a change points at its bits in the mapping.
struct Vcd {
    std::vector<Wave> waves;
    std::map<std::string, int> byName;
    uint64_t end = 0;
    std::string timescale;
    const char* data = nullptr;
    size_t size = 0;

    ~Vcd() { if (data && size) munmap((void*)data, size); }

    static bool space(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

    // Identifier codes are short printable strings: a number, then a table.
    static uint64_t code(const char* p, size_t n) {
        uint64_t c = 0;
        for (size_t i = 0; i < n; i++) c = c * 95 + (uint64_t)((unsigned char)p[i] - 32);
        return c;
    }

    struct Ids {
        std::vector<int> dense;
        std::unordered_map<uint64_t, int> sparse;
        static constexpr uint64_t kDense = 1u << 22;
        void put(uint64_t c, int ix) {
            if (c < kDense) {
                if (dense.size() <= c) dense.resize(c + 1, -1);
                dense[c] = ix;
            } else sparse[c] = ix;
        }
        int get(uint64_t c) const {
            if (c < dense.size()) return dense[c];
            if (c < kDense) return -1;
            auto it = sparse.find(c);
            return it == sparse.end() ? -1 : it->second;
        }
    };

    // Scan [p, e) of the body. `emit(wave, time, bits, len)` for each change.
    template<typename F>
    static uint64_t scan(const char* base, const char* p, const char* e, const Ids& ids, F&& emit) {
        uint64_t now = 0, last = 0;
        auto tokEnd = [&](const char* q) { while (q < e && !space(*q)) q++; return q; };
        while (p < e) {
            while (p < e && space(*p)) p++;
            if (p >= e) break;
            const char* q = tokEnd(p);
            char c = *p;
            if (c == '#') {
                now = 0;
                for (const char* d = p + 1; d < q && *d >= '0' && *d <= '9'; d++) now = now * 10 + (uint64_t)(*d - '0');
                last = std::max(last, now);
            } else if (c == 'b' || c == 'B') {
                const char* i0 = q;
                while (i0 < e && space(*i0)) i0++;
                const char* i1 = tokEnd(i0);
                int w = ids.get(code(i0, (size_t)(i1 - i0)));
                if (w >= 0) emit(w, now, (uint64_t)(p + 1 - base), (uint64_t)(q - p - 1));
                q = i1;
            } else if (c == 'r' || c == 'R' || c == 's' || c == 'S') {
                const char* i0 = q;
                while (i0 < e && space(*i0)) i0++;
                q = tokEnd(i0);
            } else if (c == '$') {
                // $dumpvars / $end bracket ordinary changes; a comment is skipped.
                if (q - p == 8 && !std::memcmp(p, "$comment", 8)) {
                    const char* x = (const char*)memmem(q, (size_t)(e - q), "$end", 4);
                    q = x ? x + 4 : e;
                }
            } else if (q - p >= 2) {
                int w = ids.get(code(p + 1, (size_t)(q - p - 1)));
                if (w >= 0) emit(w, now, (uint64_t)(p - base), 1);
            }
            p = q;
        }
        return last;
    }

    bool load(const std::string& file, unsigned threads, std::string& err) {
        int fd = open(file.c_str(), O_RDONLY);
        struct stat sb;
        if (fd < 0 || fstat(fd, &sb) != 0) { err = "cannot open " + file; if (fd >= 0) close(fd); return false; }
        size = (size_t)sb.st_size;
        if (size == 0) { close(fd); err = file + " is empty"; return false; }
        void* m = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m == MAP_FAILED) { size = 0; err = "cannot map " + file; return false; }
        data = (const char*)m;
        madvise(m, size, MADV_SEQUENTIAL);
        if (size >= (1ull << 40)) { err = file + " is larger than mdebug reads (1 TB)"; return false; }

        // The header: scopes and variables, up to $enddefinitions.
        Ids ids;
        std::unordered_map<uint64_t, int> seen;
        std::vector<std::string> scope;
        const char* p = data;
        const char* e = data + size;
        auto tok = [&]() -> std::string_view {
            while (p < e && space(*p)) p++;
            const char* q = p;
            while (q < e && !space(*q)) q++;
            std::string_view t(p, (size_t)(q - p));
            p = q;
            return t;
        };
        auto skipToEnd = [&] { for (std::string_view t = tok(); !t.empty() && t != "$end"; t = tok()) {} };
        for (;;) {
            std::string_view t = tok();
            if (t.empty()) break;
            if (t == "$scope") {
                tok();
                scope.emplace_back(tok());
                skipToEnd();
            } else if (t == "$upscope") {
                if (!scope.empty()) scope.pop_back();
                skipToEnd();
            } else if (t == "$var") {
                std::string type(tok());
                int width = std::atoi(std::string(tok()).c_str());
                std::string_view id = tok();
                std::string name(tok()), rest;
                // "q [7:0]" is the vector q; "mem[3]" keeps its index.
                for (std::string_view r = tok(); !r.empty() && r != "$end"; r = tok()) rest += r;
                if (type != "real" && type != "event" && type != "string") {
                    std::string full;
                    for (auto& sc : scope) full += sc + ".";
                    full += name;
                    if (!rest.empty() && rest.find(':') == std::string::npos) full += rest;
                    uint64_t c = code(id.data(), id.size());
                    auto it = seen.find(c);
                    if (it == seen.end()) {
                        it = seen.emplace(c, (int)waves.size()).first;
                        ids.put(c, it->second);
                        waves.emplace_back().width = width;
                        waves.back().base = data;
                    }
                    byName[full] = it->second;
                }
            } else if (t == "$timescale") {
                for (std::string_view r = tok(); !r.empty() && r != "$end"; r = tok()) timescale += r;
            } else if (t == "$enddefinitions") {
                skipToEnd();
                break;
            } else if (t[0] == '$') {
                skipToEnd();
            }
        }

        // The body, cut at time stamps so every piece knows what time it is.
        const char* body = p;
        size_t left = (size_t)(e - body);
        size_t pieces = std::max<size_t>(1, std::min<size_t>(threads, left / (4u << 20)));
        std::vector<const char*> cut{body};
        for (size_t i = 1; i < pieces; i++) {
            const char* guess = body + left / pieces * i;
            const char* x = (const char*)memmem(guess, (size_t)(e - guess), "\n#", 2);
            if (!x) break;
            if (x + 1 > cut.back()) cut.push_back(x + 1);
        }
        cut.push_back(e);
        pieces = cut.size() - 1;
        size_t nw = waves.size();
        std::vector<std::vector<uint32_t>> count(pieces, std::vector<uint32_t>(nw, 0));
        std::vector<uint64_t> lastT(pieces, 0);
        parallelFor(pieces, threads, [&](size_t k) {
            auto& c = count[k];
            lastT[k] = scan(data, cut[k], cut[k + 1], ids, [&](int w, uint64_t, uint64_t, uint64_t) { c[w]++; });
        });
        for (uint64_t t : lastT) end = std::max(end, t);
        // Where each piece writes each signal: the pieces before it, in order.
        for (size_t w = 0; w < nw; w++) {
            uint64_t total = 0;
            for (size_t k = 0; k < pieces; k++) {
                uint32_t n = count[k][w];
                count[k][w] = (uint32_t)total;
                total += n;
            }
            waves[w].ch.resize(total);
        }
        parallelFor(pieces, threads, [&](size_t k) {
            auto& c = count[k];
            scan(data, cut[k], cut[k + 1], ids, [&](int w, uint64_t t, uint64_t off, uint64_t len) {
                waves[w].ch[c[w]++] = Change{t, off, len};
            });
        });
        compact(threads);
        return true;
    }

    // One entry per change: the later of two at one time, and no entry
    // that repeats the value before it ($dumpvars, then the same at #0).
    void compact(unsigned threads) {
        parallelFor(waves.size(), threads, [&](size_t w) {
            auto& ch = waves[w].ch;
            const char* data = waves[w].base;
            size_t n = 0;
            for (size_t i = 0; i < ch.size(); i++) {
                if (n && ch[n - 1].t == ch[i].t) { ch[n - 1] = ch[i]; continue; }
                if (n && ch[n - 1].len == ch[i].len &&
                    !std::memcmp(data + ch[n - 1].off, data + ch[i].off, ch[i].len))
                    continue;
                ch[n++] = ch[i];
            }
            // Replacing at one time can leave a repeat behind it.
            size_t m = 0;
            for (size_t i = 0; i < n; i++) {
                if (m && ch[m - 1].len == ch[i].len &&
                    !std::memcmp(data + ch[m - 1].off, data + ch[i].off, ch[i].len))
                    continue;
                ch[m++] = ch[i];
            }
            ch.resize(m);
        });
    }

#ifdef MINISIM_FST
    // An FST (msim -fst): libfst unpacks the blocks; each signal's values go
    // into a buffer of its own.
    std::vector<std::string> pools;

    bool loadFst(const std::string& file, unsigned threads, std::string& err) {
        fstReaderContext* ctx = fstReaderOpen(file.c_str());
        if (!ctx) { err = "cannot open " + file; return false; }
        std::vector<int> byHandle;   // FST handle -> wave
        std::vector<std::string> scope;
        while (struct fstHier* h = fstReaderIterateHier(ctx)) {
            if (h->htyp == FST_HT_SCOPE) scope.emplace_back(h->u.scope.name);
            else if (h->htyp == FST_HT_UPSCOPE) { if (!scope.empty()) scope.pop_back(); }
            else if (h->htyp == FST_HT_VAR) {
                unsigned char ty = h->u.var.typ;
                if (ty == FST_VT_VCD_REAL || ty == FST_VT_VCD_EVENT || ty == FST_VT_GEN_STRING ||
                    ty == FST_VT_VCD_REAL_PARAMETER || ty == FST_VT_VCD_REALTIME || ty == FST_VT_SV_SHORTREAL)
                    continue;
                std::string name(h->u.var.name), full;
                // "q [7:0]" is the vector q; "mem[3]" keeps its index.
                if (size_t sp = name.find(' '); sp != std::string::npos) {
                    std::string rest = name.substr(sp + 1);
                    name.resize(sp);
                    if (rest.find(':') == std::string::npos) name += rest;
                }
                for (auto& sc : scope) full += sc + ".";
                full += name;
                fstHandle hd = h->u.var.handle;
                if (byHandle.size() <= hd) byHandle.resize(hd + 1, -1);
                if (byHandle[hd] < 0) {
                    byHandle[hd] = (int)waves.size();
                    waves.emplace_back().width = (int)h->u.var.length;
                }
                byName[full] = byHandle[hd];
            }
        }
        pools.resize(waves.size());
        struct Ctx { Vcd* v; std::vector<int>* byHandle; } cx{this, &byHandle};
        fstReaderSetFacProcessMaskAll(ctx);
        fstReaderIterBlocks(ctx, [](void* u, uint64_t t, fstHandle hd, const unsigned char* val) {
            Ctx& c = *(Ctx*)u;
            if (hd >= c.byHandle->size()) return;
            int w = (*c.byHandle)[hd];
            if (w < 0) return;
            std::string& pool = c.v->pools[w];
            size_t len = (size_t)c.v->waves[w].width;
            if (pool.size() + len >= (1ull << 40) || len >= (1u << 24)) return;
            c.v->waves[w].ch.push_back(Change{t, pool.size(), len});
            pool.append((const char*)val, len);
        }, &cx, nullptr);
        end = fstReaderGetEndTime(ctx);
        int ts = fstReaderGetTimescale(ctx);
        static const char* unit[] = {"s", "ms", "us", "ns", "ps", "fs"};
        if (int k = -ts; k >= 0 && k <= 15) {   // 10^ts seconds
            int idx = (k + 2) / 3, pw = 3 * idx - k;
            timescale = std::string(pw == 0 ? "1" : pw == 1 ? "10" : "100") + unit[idx];
        }
        fstReaderClose(ctx);
        for (size_t w = 0; w < waves.size(); w++) waves[w].base = pools[w].data();
        compact(threads);
        return true;
    }
#endif

    // By what the file is, not by its name.
    bool loadAny(const std::string& file, unsigned threads, std::string& err) {
        std::ifstream in(file, std::ios::binary);
        if (!in) { err = "cannot open " + file; return false; }
        int c = in.get();
        in.close();
        if (c == '$' || c == '#' || c == ' ' || c == '\n' || c == '\t' || c == '\r') return load(file, threads, err);
#ifdef MINISIM_FST
        return loadFst(file, threads, err);
#else
        err = file + " is not a VCD (this mdebug was built without FST)";
        return false;
#endif
    }

    const Wave* find(const std::string& name) const {
        auto it = byName.find(name);
        return it == byName.end() ? nullptr : &waves[it->second];
    }
};

std::string showBits(const std::string& bits) {
    if (bits.size() == 1) return bits;
    bool unk = bits.find_first_of("xz") != std::string::npos;
    if (unk) return std::to_string(bits.size()) + "'b" + bits;
    std::string hex;
    size_t n = bits.size();
    for (size_t i = 0; i < n;) {
        size_t take = (n - i) % 4 ? (n - i) % 4 : 4;
        int v = 0;
        for (size_t k = 0; k < take; k++) v = v * 2 + (bits[i + k] == '1');
        hex += "0123456789abcdef"[v];
        i += take;
    }
    return std::to_string(n) + "'h" + hex;
}

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
        if (!w || !type.isIntegral()) return std::nullopt;
        int i = w->at(t);
        std::string bits = i < 0 ? std::string(w->width, 'x') : w->bits(i);
        SVInt v = SVInt::fromString(std::to_string(bits.size()) + "'b" + bits);
        v.setSigned(type.isSigned());
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
                if (db.sigIx.count(r)) cone(r, depth - 1, indent + 2, onPath);
        }
        onPath.erase(s);
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

    // The assignment that gave s its value at time t, then what caused it.
    void active(const Symbol* s, uint64_t t, int depth, int indent, std::set<std::pair<const Symbol*, uint64_t>>& seen) {
        std::string pad(indent * 2, ' ');
        const Wave* w = vcd.find(path(*s));
        if (!w) { std::cout << pad << path(*s) << "  (not in the waveform)\n"; return; }
        int ci = w->at(t);
        uint64_t tc = ci < 0 ? 0 : w->ch[ci].t;
        std::cout << pad << path(*s) << " = " << (ci < 0 ? "x" : showBits(w->bits(ci))) << " @" << t;
        if (tc != t) std::cout << "  (since " << tc << ")";
        std::cout << "\n";
        auto it = db.driversOf.find(s);
        if (it == db.driversOf.end()) { std::cout << pad << "  no driver: an input or a testbench value\n"; return; }
        if (!seen.insert({s, tc}).second) { std::cout << pad << "  (shown above)\n"; return; }

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
        std::map<int, int> last;
        for (size_t k = 0; k < hits.size(); k++)
            if (hits[k].state == 1 && db.drivers[hits[k].ix].block >= 0) last[db.drivers[hits[k].ix].block] = (int)k;
        bool any = false;
        for (size_t k = 0; k < hits.size(); k++) {
            const Driver& d = db.drivers[hits[k].ix];
            if (hits[k].state == 0) continue;
            if (hits[k].state == 1 && d.block >= 0 && last[d.block] != (int)k) continue;
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
                active(r, ts, depth - 1, indent + 2, seen);
            }
        }
        if (!any) std::cout << pad << "  no assignment's conditions hold at " << tc << " (" << it->second.size() << " drivers)\n";
    }

    bool activeTrace(const std::string& name, std::optional<uint64_t> at, int depth) {
        if (!haveVcd) { std::cerr << "mdebug: -active needs -vcd <file>\n"; return false; }
        const Symbol* s = find(name);
        if (!s) return false;
        std::set<std::pair<const Symbol*, uint64_t>> seen;
        active(s, at ? *at : vcd.end, depth, 0, seen);
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
        if (it == db.sigIx.end() || !type.isIntegral()) return "null";
        return "[\"s\"," + std::to_string(it->second) + "," + (type.isSigned() ? "1" : "0") + "]";
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
        bool first = true;
        for (size_t i = 0; i < db.drivers.size(); i++) {
            const Driver& d = db.drivers[i];
            auto it = db.sigIx.find(d.target);
            if (it == db.sigIx.end()) continue;
            os << (first ? "" : ",\n") << "{\"sig\":" << it->second << ",\"kind\":" << js(label(d))
               << ",\"file\":" << dw[i].file << ",\"line\":" << dw[i].line << ",\"text\":" << js(text(d))
               << ",\"when\":" << js(when(d)) << ",\"seq\":" << (d.seq ? 1 : 0) << ",\"reads\":" << ids(d.reads)
               << ",\"ctl\":" << ids(d.creads) << ",\"blk\":" << d.block << ",\"whole\":" << (d.whole ? 1 : 0)
               << ",\"g\":" << guardsJson(d) << ",\"rhs\":" << (d.rhs && d.whole ? exprJson(*d.rhs) : "null") << "}";
            first = false;
        }
        os << "],\n\"cases\":" << casesJson();
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
    std::string vcdPath, htmlPath, jsonPath;
    int depth = -1;
    std::optional<uint64_t> at;
    size_t maxChanges = 20000;
    // Every logical core by default (hyper-threads included); -threads 1 for one.
    unsigned threads = std::max(1u, std::min(64u, std::thread::hardware_concurrency()));
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
        if (a == "-threads" || a == "-j") {
            threads = (unsigned)std::max(1, std::atoi(next("a number").c_str()));
            continue;
        }
        if (a == "-max-changes") { maxChanges = std::strtoull(next("a number").c_str(), nullptr, 10); continue; }
        if (a == "--help-slang") { fwd.push_back("--help"); continue; }
        if (a == "-top") { fwd.push_back("--top"); fwd.push_back(next("a module name")); continue; }
        if (a == "-sv" || a == "-sverilog") continue;
        fwd.push_back(a);
    }
    if (queries.empty() && htmlPath.empty() && jsonPath.empty() && fwd.size() == 1) { usage(); return 2; }
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

    if (!vcdPath.empty()) {
        std::string err;
        if (!tool.vcd.loadAny(vcdPath, threads, err)) { std::cerr << "mdebug: " << err << "\n"; return 2; }
        tool.haveVcd = true;
    }

    bool ok = true;
    for (const Query& q : queries) {
        if (q.what == "drivers") ok &= tool.drivers(q.arg);
        else if (q.what == "loads") ok &= tool.loads(q.arg);
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
