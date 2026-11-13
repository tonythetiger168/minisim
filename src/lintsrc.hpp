// Rules on the source text and the syntax tree, before elaboration matters:
// formatting (line length, tabs, trailing spaces), preprocessor style, stray
// semicolons, declarations that leave something implicit. (if (c); with an
// empty body is slang's own -Wempty-body warning and is not repeated here.)
//
// The rule set follows Verible's lint rules (github.com/chipsalliance/verible,
// Apache-2.0), which is where most teams know these checks from; the checks
// are written here on slang's syntax tree, no Verible code is used. The
// Verible rule each one answers to is named in the rule's -explain text.
//
// One task per source file and one per syntax tree; each keeps its findings
// to itself and they are reported in a fixed order.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "lintir.hpp"
#include "lintrules.hpp"
#include "parallel.hpp"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/syntax/SyntaxVisitor.h"

namespace lintsrc {
using namespace lintir;
using namespace slang::syntax;
using slang::parsing::Token;
using slang::parsing::TokenKind;

struct Found {
    const char* rule;
    SourceLocation loc;
    std::string text;
};

inline const std::vector<const char*>& textRules() {
    static const std::vector<const char*> r = {"FMT001", "FMT002", "FMT003", "FMT004", "STY004", "NAME014", "STY011"};
    return r;
}
inline const std::vector<const char*>& syntaxRules() {
    static const std::vector<const char*> r = {"SUS001", "STY005", "STY006", "STY007",
                                               "STY008", "STY009", "W009", "NAME015", "NAME016", "NAME017",
                                               "SUS002", "SUS003", "STY010", "STY012",
                                               "STY013", "STY014", "STY015", "SUS004",
                                               "NAME018", "NAME019"};
    return r;
}
inline bool any(const Rules& rules, const std::vector<const char*>& ids) {
    for (const char* r : ids)
        if (rules.enabled(r)) return true;
    return false;
}

// ---------------------------------------------------------------- the text

inline void checkText(BufferID buf, std::string_view text, const Rules& rules, const lintrules::Params& P,
                      std::vector<Found>& out) {
    // slang keeps a terminating NUL in the buffer.
    while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
    if (text.empty()) return;
    bool fmt1 = rules.enabled("FMT001"), fmt2 = rules.enabled("FMT002"), fmt3 = rules.enabled("FMT003");
    bool sty4 = rules.enabled("STY004"), name14 = rules.enabled("NAME014"), sty11 = rules.enabled("STY011");
    auto at = [&](size_t off) { return SourceLocation(buf, off); };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = nl == std::string_view::npos ? text.size() : nl;
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (fmt1 && line.size() > P.lineLength) {
            // A line that is one long token (a URL in a comment, an include path) cannot be wrapped.
            std::string_view tail = line.substr(line.find_first_not_of(" \t/") == std::string_view::npos
                                                    ? 0
                                                    : line.find_first_not_of(" \t/"));
            if (tail.find(' ') != std::string_view::npos)
                out.push_back({"FMT001", at(pos + P.lineLength),
                               "the line is " + std::to_string(line.size()) + " characters long; the limit is " +
                                   std::to_string(P.lineLength)});
        }
        if (fmt2) {
            size_t t = line.find('\t');
            if (t != std::string_view::npos) out.push_back({"FMT002", at(pos + t), "a tab character; use spaces"});
        }
        if (fmt3 && !line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            size_t last = line.find_last_not_of(" \t");
            out.push_back({"FMT003", at(pos + (last == std::string_view::npos ? 0 : last + 1)), "trailing whitespace"});
        }
        if (sty4 || name14 || sty11) {
            size_t f = line.find_first_not_of(" \t");
            if (f != std::string_view::npos && line[f] == '`') {
                std::string_view d = line.substr(f + 1);
                // `uvm_info(...);  The macro brings its own semicolon; a second one ends an if early.
                if (sty11 && d.substr(0, 4) == "uvm_") {
                    std::string_view code = d.substr(0, d.find("//"));
                    size_t e = code.find_last_not_of(" \t");
                    if (e != std::string_view::npos && e > 0 && code[e] == ';' && code[e - 1] == ')')
                        out.push_back({"STY011", at(pos + f + 1 + e), "a semicolon after a UVM macro"});
                }
                if (sty4 && d.substr(0, 5) == "endif" && d.find_first_not_of(" \t", 5) == std::string_view::npos)
                    out.push_back({"STY004", at(pos + f),
                                   "`endif with no comment saying which `ifdef it closes"});
                if (name14 && d.substr(0, 6) == "define" && d.size() > 6 && (d[6] == ' ' || d[6] == '\t')) {
                    size_t b = d.find_first_not_of(" \t", 6);
                    if (b != std::string_view::npos) {
                        size_t e = b;
                        while (e < d.size() && (std::isalnum((unsigned char)d[e]) || d[e] == '_' || d[e] == '$')) e++;
                        std::string_view name = d.substr(b, e - b);
                        if (!name.empty() && !P.macro.ok(name))
                            out.push_back({"NAME014", at(pos + f + 1 + b),
                                           "macro '" + std::string(name) + "' does not match " + P.macro.src});
                    }
                }
            }
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    if (rules.enabled("FMT004") && text.back() != '\n')
        out.push_back({"FMT004", at(text.size() - 1), "the file does not end with a newline"});
}

// -------------------------------------------------------------- the syntax

struct Walk : SyntaxVisitor<Walk> {
    const Rules& rules;
    const SourceManager& sm;
    const lintrules::Params& P;
    std::vector<Found>& out;
    std::vector<std::string_view> blocks;   // names of the begin blocks we are inside
    int inClass = 0;       // methods of a class are automatic already
    int autoScope = 0;     // module automatic ...: its functions are too
    // NAME019: the names declared in each scope we are inside, outermost first.
    std::vector<std::vector<std::pair<std::string_view, SourceLocation>>> scopes{1};
    // NAME018: inside always_ff; the flops already reported in this module.
    int inFF = 0;
    std::vector<std::string_view> ffSeen;
    struct Scoped {
        Walk& w;
        explicit Scoped(Walk& w) : w(w) { w.scopes.emplace_back(); }
        ~Scoped() { w.scopes.pop_back(); }
    };
    Walk(const Rules& r, const SourceManager& s, const lintrules::Params& p, std::vector<Found>& o)
        : rules(r), sm(s), P(p), out(o) {}

    void add(const char* rule, SourceLocation loc, std::string text) {
        if (rules.enabled(rule)) out.push_back({rule, loc, std::move(text)});
    }
    static bool empty(const SyntaxNode& n) { return n.kind == SyntaxKind::EmptyStatement; }

    // x = 1;;   and   endfunction;   leave an empty statement or member behind.
    void handle(const BlockStatementSyntax& s) {
        for (auto* item : s.items)
            if (item && empty(*item)) add("SUS001", item->getFirstToken().location(), "a stray semicolon");
        // begin : name   or   name : begin
        std::string_view name;
        if (s.kind == SyntaxKind::SequentialBlockStatement) {
            if (s.blockName) name = s.blockName->name.valueText();
            else if (s.label) name = s.label->name.valueText();
        }
        if (!name.empty()) blocks.push_back(name);
        Scoped sc(*this);
        visitDefault(s);
        if (!name.empty()) blocks.pop_back();
    }
    // disable name; of anything but a begin block we are inside: a fork, another process.
    void handle(const DisableStatementSyntax& s) {
        if (s.kind == SyntaxKind::DisableStatement && rules.enabled("STY012")) {
            std::string name = s.name->toString();
            while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(0, 1);
            while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
            bool mine = false;
            for (auto b : blocks) mine |= b == name;
            if (!mine)
                add("STY012", s.disable.location(),
                    "disable " + name + " stops something other than a begin block this statement is in; use disable fork");
        }
        visitDefault(s);
    }
    // void'(obj.randomize()) throws the failure away.
    void handle(const VoidCastedCallStatementSyntax& s) {
        if (rules.enabled("SUS002")) {
            std::string inner = s.expr->toString();
            for (std::string_view f : {"randomize", "uvm_hdl_read"})
                if (inner.find(f) != std::string::npos) {
                    add("SUS002", s.voidKeyword.location(),
                        "void'() throws away the result of " + std::string(f) + ": a failure goes unseen");
                    break;
                }
        }
        visitDefault(s);
    }
    void handle(const SystemNameSyntax& s) {
        std::string_view n = s.systemIdentifier.valueText();
        const char* instead = n == "$psprintf" ? "$sformatf"
                              : n == "$random" ? "$urandom"
                              : n == "$srandom" ? "process::self().srandom()"
                              : n.substr(0, 6) == "$dist_" ? "$urandom_range or constraints"
                              : nullptr;
        if (instead)
            add("SUS003", s.systemIdentifier.location(), std::string(n) + ": use " + instead);
        // $test$plusargs("x") says only whether +x was given; +x=0 reads as on.
        if (n == "$test$plusargs")
            add("SUS004", s.systemIdentifier.location(),
                "$test$plusargs: +NAME=0 still counts as given; read the value with $value$plusargs");
    }
    void handle(const LiteralExpressionSyntax& s) {
        if (s.kind == SyntaxKind::StringLiteralExpression && rules.enabled("STY010")) {
            std::string_view raw = s.literal.rawText();
            size_t at = raw.find("\\\n");
            if (at == std::string_view::npos) at = raw.find("\\\r\n");
            if (at != std::string_view::npos)
                add("STY010", s.literal.location() + at, "a string carried over the line with a backslash; join two strings with {}");
        }
        if (s.kind == SyntaxKind::StringLiteralExpression && rules.enabled("STY015")) formats(s.literal);
    }
    // "%h" should read "0x%h", "%b" "0b%b", and "%d" carries no radix prefix.
    void formats(Token lit) {
        std::string_view raw = lit.rawText();
        for (size_t i = 0; i + 1 < raw.size(); i++) {
            if (raw[i] != '%') continue;
            if (raw[i + 1] == '%') { i++; continue; }
            size_t j = i + 1;
            while (j < raw.size() && (std::isdigit((unsigned char)raw[j]) || raw[j] == '-')) j++;
            if (j >= raw.size()) break;
            char c = (char)std::tolower((unsigned char)raw[j]);
            auto before = [&](std::string_view a, std::string_view b) {
                std::string_view pre = raw.substr(0, i);
                auto ends = [&](std::string_view x) {
                    if (pre.size() < x.size()) return false;
                    for (size_t k = 0; k < x.size(); k++)
                        if (std::tolower((unsigned char)pre[pre.size() - x.size() + k]) != x[k]) return false;
                    return true;
                };
                return ends(a) || ends(b);
            };
            std::string spec(raw.substr(i, j - i + 1));
            if ((c == 'h' || c == 'x') && !before("0x", "'h"))
                add("STY015", lit.location() + i, "\"" + spec + "\" prints hex with no prefix; write 0x" + spec);
            else if (c == 'b' && !before("0b", "'b"))
                add("STY015", lit.location() + i, "\"" + spec + "\" prints binary with no prefix; write 0b" + spec);
            else if (c == 'd' && (before("0x", "'h") || before("0b", "'b")))
                add("STY015", lit.location() + i, "\"" + spec + "\" prints decimal after a hex or binary prefix");
            i = j;
        }
    }
    void handle(const ConstraintDeclarationSyntax& s) {
        if (rules.enabled("NAME016")) {
            Token t = s.name->getLastToken();
            if (!t.valueText().empty() && !P.constraint.ok(t.valueText()))
                add("NAME016", t.location(),
                    "constraint '" + std::string(t.valueText()) + "' does not match " + P.constraint.src);
        }
        visitDefault(s);
    }
    void handle(const EmptyMemberSyntax& s) {
        if (s.semi.kind == TokenKind::Semicolon && !s.semi.isMissing())
            add("SUS001", s.semi.location(), "a stray semicolon");
    }

    void handle(const ClassDeclarationSyntax& s) {
        Scoped sc(*this);
        inClass++;
        visitDefault(s);
        inClass--;
    }
    void handle(const ModuleDeclarationSyntax& s) {
        bool isAuto = s.header->lifetime.kind == TokenKind::AutomaticKeyword;
        if (s.kind == SyntaxKind::InterfaceDeclaration && rules.enabled("NAME015")) {
            Token name = s.header->name;
            if (!name.valueText().empty() && !P.interface.ok(name.valueText()))
                add("NAME015", name.location(),
                    "interface '" + std::string(name.valueText()) + "' does not match " + P.interface.src);
        }
        if (s.kind == SyntaxKind::PackageDeclaration && rules.enabled("STY005")) {
            Token name = s.header->name;
            SourceLocation full = sm.getFullyOriginalLoc(name.location());
            std::string file(sm.getFileName(full));
            size_t slash = file.find_last_of("/\\");
            std::string stem = slash == std::string::npos ? file : file.substr(slash + 1);
            stem = stem.substr(0, stem.find('.'));
            std::string pkg(name.valueText());
            auto strip = [](std::string v) {
                return v.size() > 4 && v.compare(v.size() - 4, 4, "_pkg") == 0 ? v.substr(0, v.size() - 4) : v;
            };
            // foo_pkg may live in foo_pkg.sv or foo.sv.
            if (!pkg.empty() && !stem.empty() && stem != pkg && strip(stem) != strip(pkg))
                add("STY005", name.location(), "package '" + pkg + "' is in a file named '" + stem + "'");
        }
        autoScope += isAuto;
        Scoped sc(*this);
        ffSeen.clear();
        visitDefault(s);
        autoScope -= isAuto;
    }
    void handle(const FunctionDeclarationSyntax& s) {
        if (!inClass && !autoScope && s.prototype->lifetime.kind == TokenKind::Unknown) {
            bool task = s.kind == SyntaxKind::TaskDeclaration;
            add("STY006", s.prototype->name->getFirstToken().location(),
                std::string(task ? "task" : "function") +
                    " with no lifetime: it is static, so its variables are shared by every call; say automatic (or static)");
        }
        Scoped sc(*this);
        visitDefault(s);
    }
    // extern and pure virtual methods, DPI imports: their arguments live in their own scope.
    void handle(const FunctionPrototypeSyntax& s) {
        if (s.parent && s.parent->kind != SyntaxKind::FunctionDeclaration && s.parent->kind != SyntaxKind::TaskDeclaration) {
            Scoped sc(*this);
            visitDefault(s);
        } else {
            visitDefault(s);
        }
    }
    void handle(const GenerateBlockSyntax& s) {
        Scoped sc(*this);
        visitDefault(s);
    }
    // A name declared again in an inner scope hides the outer one there.
    void handle(const DeclaratorSyntax& s) {
        std::string_view n = s.name.valueText();
        bool skip = n.empty() || !s.parent || s.parent->kind == SyntaxKind::StructUnionMember ||
                    s.parent->kind == SyntaxKind::EnumType;
        if (!skip && rules.enabled("NAME019")) {
            for (size_t i = 0; i + 1 < scopes.size(); i++)
                for (auto& [m, loc] : scopes[i])
                    if (m == n) {
                        add("NAME019", s.name.location(),
                            "'" + std::string(n) + "' hides the '" + std::string(n) + "' declared on line " +
                                std::to_string(sm.getLineNumber(loc)) + "; rename one");
                        goto done;
                    }
        done:
            scopes.back().emplace_back(n, s.name.location());
        }
        visitDefault(s);
    }
    // In always_ff, every register written should say it is one: data_q, data_ff, data_reg.
    void flop(const ExpressionSyntax& e) {
        switch (e.kind) {
            case SyntaxKind::IdentifierName: {
                Token t = e.as<IdentifierNameSyntax>().identifier;
                std::string_view n = t.valueText();
                if (n.empty() || P.dff.ok(n)) return;
                for (auto m : ffSeen)
                    if (m == n) return;
                ffSeen.push_back(n);
                add("NAME018", t.location(),
                    "register '" + std::string(n) + "' written in always_ff does not match " + P.dff.src);
                return;
            }
            case SyntaxKind::ElementSelectExpression:
                return flop(*e.as<ElementSelectExpressionSyntax>().left);
            case SyntaxKind::MemberAccessExpression:
                return flop(*e.as<MemberAccessExpressionSyntax>().left);
            case SyntaxKind::ScopedName:
                if (e.as<ScopedNameSyntax>().separator.kind == TokenKind::Dot) flop(*e.as<ScopedNameSyntax>().left);
                return;
            case SyntaxKind::ParenthesizedExpression:
                return flop(*e.as<ParenthesizedExpressionSyntax>().expression);
            case SyntaxKind::ConcatenationExpression:
                for (auto* x : e.as<ConcatenationExpressionSyntax>().expressions) flop(*x);
                return;
            default:
                return;
        }
    }
    void handle(const BinaryExpressionSyntax& s) {
        if (inFF && s.kind == SyntaxKind::NonblockingAssignmentExpression && rules.enabled("NAME018")) flop(*s.left);
        visitDefault(s);
    }
    // parameter in a module body, package or class body: overridable only by defparam (or not at all).
    void handle(const ParameterDeclarationStatementSyntax& s) {
        if (s.parameter->keyword.kind == TokenKind::ParameterKeyword)
            add("STY014", s.parameter->keyword.location(),
                "parameter outside a #(...) parameter list; use localparam for a constant, or move it into #(...)");
        visitDefault(s);
    }

    // if, else, loops and always/initial bodies without begin ... end.
    void body(const SyntaxNode& st, Token kw) {
        if (!rules.enabled("STY013")) return;
        const SyntaxNode* n = &st;
        if (n->kind == SyntaxKind::TimingControlStatement) n = n->as<TimingControlStatementSyntax>().statement.get();
        if (n->kind == SyntaxKind::SequentialBlockStatement || n->kind == SyntaxKind::ParallelBlockStatement ||
            n->kind == SyntaxKind::EmptyStatement)
            return;
        add("STY013", kw.location(), std::string(kw.valueText()) + " body with no begin ... end");
    }
    void handle(const ConditionalStatementSyntax& s) {
        body(*s.statement, s.ifKeyword);
        if (s.elseClause && s.elseClause->clause->kind != SyntaxKind::ConditionalStatement)
            body(*s.elseClause->clause, s.elseClause->elseKeyword);
        visitDefault(s);
    }
    void handle(const ForLoopStatementSyntax& s) {
        body(*s.statement, s.forKeyword);
        Scoped sc(*this);
        visitDefault(s);
    }
    void handle(const ForeachLoopStatementSyntax& s) { body(*s.statement, s.keyword); visitDefault(s); }
    void handle(const LoopStatementSyntax& s) { body(*s.statement, s.repeatOrWhile); visitDefault(s); }
    void handle(const ForeverStatementSyntax& s) { body(*s.statement, s.foreverKeyword); visitDefault(s); }
    void handle(const DoWhileStatementSyntax& s) { body(*s.statement, s.doKeyword); visitDefault(s); }
    void handle(const ProceduralBlockSyntax& s) {
        body(*s.statement, s.keyword);
        bool ff = s.kind == SyntaxKind::AlwaysFFBlock;
        inFF += ff;
        visitDefault(s);
        inFF -= ff;
    }

    void handle(const ParameterDeclarationSyntax& s) {
        // parameter DisableFoo = 0: "not disabled" is a double negative at every use.
        if (rules.enabled("NAME017"))
            for (auto* d : s.declarators) {
                std::string n(d->name.valueText());
                for (char& c : n) c = (char)std::tolower((unsigned char)c);
                if (n.rfind("disable", 0) == 0 && n.size() > 7)
                    add("NAME017", d->name.location(),
                        "parameter '" + std::string(d->name.valueText()) +
                            "' says what is turned off; name it for what is turned on (Enable...)");
            }
        if (s.type->kind == SyntaxKind::ImplicitType) {
            auto& t = s.type->as<ImplicitTypeSyntax>();
            if (t.signing.kind == TokenKind::Unknown && t.dimensions.empty() && !s.declarators.empty())
                add("STY007", s.declarators[0]->name.location(),
                    "parameter '" + std::string(s.declarators[0]->name.valueText()) +
                        "' has no type: it takes the type of whatever value overrides it");
        }
        visitDefault(s);
    }

    // +1 the left bound is the larger (descending, [7:0]); -1 ascending ([0:7]); 0 cannot tell.
    static int order(const SelectorSyntax& sel) {
        if (sel.kind != SyntaxKind::SimpleRangeSelect) return 0;
        auto& r = sel.as<RangeSelectSyntax>();
        auto lit = [](const ExpressionSyntax& e, uint64_t& v) {
            if (e.kind != SyntaxKind::IntegerLiteralExpression) return false;
            auto iv = e.as<LiteralExpressionSyntax>().literal.intValue().as<uint64_t>();
            if (!iv) return false;
            v = *iv;
            return true;
        };
        uint64_t l = 0, h = 0;
        bool ll = lit(*r.left, l), rl = lit(*r.right, h);
        if (ll && rl) return l > h ? 1 : l < h ? -1 : 0;
        if (rl && h == 0) return 1;    // [N-1:0]
        if (ll && l == 0) return -1;   // [0:N-1]
        return 0;
    }
    void handle(const VariableDimensionSyntax& s) {
        if (s.specifier && s.specifier->kind == SyntaxKind::RangeDimensionSpecifier && s.parent) {
            int o = order(*s.specifier->as<RangeDimensionSpecifierSyntax>().selector);
            bool unpacked = s.parent->kind == SyntaxKind::Declarator;
            SourceLocation loc = s.openBracket.location();
            if (unpacked && o > 0)
                add("STY009", loc, "an unpacked dimension counts down; write [0:N-1] or [N]");
            if (!unpacked && o < 0)
                add("STY008", loc, "a packed dimension counts up; write [N-1:0]");
        }
        visitDefault(s);
    }
    // 8'b101: fewer digits than the width says.
    void handle(const IntegerVectorExpressionSyntax& s) {
        if (!rules.enabled("W009")) return;
        auto width = s.size.kind == TokenKind::IntegerLiteral ? s.size.intValue().as<uint64_t>() : std::nullopt;
        std::string_view base = s.base.rawText();
        if (!width || base.empty()) return;
        char b = (char)std::tolower((unsigned char)base.back());
        int per = b == 'b' ? 1 : 0;   // 32'h10 is how hex constants are written; binary ones are bit patterns
        if (!per) return;
        size_t digits = 0;
        for (char c : s.value.rawText())
            if (c != '_') digits++;
        // 8'b0, 8'b1, 8'bx: one digit is how a small constant is written.
        if (digits == 1) return;
        size_t need = (size_t)((*width + (uint64_t)per - 1) / (uint64_t)per);
        if (digits && digits < need)
            add("W009", s.value.location(),
                std::to_string(*width) + "-bit literal written with " + std::to_string(digits) +
                    (digits == 1 ? " digit" : " digits") + " where " + std::to_string(need) +
                    " would show every bit; the upper bits are zero");
    }
};

// ----------------------------------------------------------------- driver

// `skip` names buffers that are not the user's (the generated UPF package).
template<typename Skip>
void run(Compilation& comp, Report& rep, const lintrules::Params& P, unsigned threads, Skip&& skip) {
    const SourceManager& sm = *comp.getSourceManager();
    std::vector<BufferID> bufs;
    if (any(rep.rules, textRules()))
        for (BufferID b : sm.getAllBuffers()) {
            // Macro expansions have no file of their own.
            if (sm.isMacroLoc(SourceLocation(b, 0))) continue;
            std::string_view name = sm.getRawFileName(b);
            if (name.empty() || name.front() == '<' || skip(name)) continue;
            bufs.push_back(b);
        }
    std::sort(bufs.begin(), bufs.end(), [&](BufferID a, BufferID b) {
        return std::make_pair(sm.getRawFileName(a), a.getId()) < std::make_pair(sm.getRawFileName(b), b.getId());
    });
    std::vector<std::shared_ptr<SyntaxTree>> trees;
    for (auto& t : comp.getSyntaxTrees()) {
        SourceLocation at = t->root().getFirstToken().location();
        std::string_view name = at.buffer() ? sm.getRawFileName(at.buffer()) : std::string_view{};
        if (!name.empty() && (name.front() == '<' || skip(name))) continue;   // generated text
        trees.push_back(t);
    }
    size_t nt = any(rep.rules, syntaxRules()) ? trees.size() : 0;
    std::vector<std::vector<Found>> found(bufs.size() + nt);
    par::parallelFor(found.size(), threads, [&](size_t i) {
        if (i < bufs.size()) {
            checkText(bufs[i], sm.getSourceText(bufs[i]), rep.rules, P, found[i]);
        } else {
            Walk w(rep.rules, sm, P, found[i]);
            trees[i - bufs.size()]->root().visit(w);
        }
    });
    for (auto& v : found)
        for (Found& f : v) rep.add(f.rule, f.loc, std::move(f.text));
}

}  // namespace lintsrc
