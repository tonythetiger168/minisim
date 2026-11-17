// mlsp: the minisim language server (Language Server Protocol 3.17 over
// stdio) for SystemVerilog and Verilog.
//
//   mlsp                     # an editor starts it; it talks LSP on stdin/stdout
//   mlsp -check FILE...      # the same analysis once, diagnostics on stdout
//
// It does not have a front end of its own. Every answer comes from slang, the
// front end msim and mlint compile with, run the way msim runs it (IEEE
// 1800-2023, use before declaration allowed, the same diagnostics turned into
// warnings), so the editor shows the errors msim would give. The lint
// findings are mlint's: on save, mlsp runs the mlint next to it with -json
// and shows what it reports. VS Code (editors/vscode/), Vim, Emacs, Helix,
// Sublime and any other LSP client can use it.
//
// What goes into one analysis: the open file, then every file of the
// workspace that defines a module, interface, program, package, primitive
// or checker it uses, and so on down (src/lsp/scan.hpp keeps that index), or
// exactly the files of a -f file list when the client gives one. Open files
// are read from the editor's buffers, saved or not.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/AllDiags.h"
#include "slang/diagnostics/Diagnostics.h"
#include "slang/driver/Driver.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxTree.h"
#include "slang/text/SourceManager.h"

#include "lsp/rpc.hpp"
#include "lsp/scan.hpp"
#include "parallel.hpp"
#include "vendor_tasks.hpp"
#include "version.hpp"

extern char** environ;

namespace {

using namespace slang;
using namespace slang::ast;
using namespace slang::syntax;
using slang::parsing::Token;
namespace fs = std::filesystem;
using lsp::Obj;
using lsp::quote;

std::string canon(const std::string& p) {
    std::error_code ec;
    auto c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? fs::path(p).lexically_normal().string() : c.string();
}

std::string dirOf(const std::string& p) { return fs::path(p).parent_path().string(); }

// ---------------------------------------------------------------- settings

struct Settings {
    std::vector<std::string> args;      // extra front-end options: +incdir+d, +define+X, -Ddir, --std=...
    std::string fileList;               // a -f file: then exactly these files
    std::string binDir;                 // where mlint is (default: next to mlsp, then PATH)
    std::string uvmHome;                // UVM sources for `import uvm_pkg::*` (default $UVM_HOME)
    bool mlint = true;                  // run mlint on save
    std::vector<std::string> mlintArgs; // e.g. -goal, -config FILE
    size_t maxFiles = 20000;            // index at most this many workspace files

    void read(const lsp::Json* v) {
        if (!v || v->kind != lsp::Json::Obj) return;
        if (auto a = lsp::at(v, "args"); a && a->kind == lsp::Json::Arr) {
            args.clear();
            for (auto& x : a->a) if (x.kind == lsp::Json::Str) args.push_back(x.s);
        }
        if (lsp::at(v, "fileList")) fileList = lsp::str(v, "fileList");
        if (lsp::at(v, "binDir")) binDir = lsp::str(v, "binDir");
        if (lsp::at(v, "uvmHome")) uvmHome = lsp::str(v, "uvmHome");
        mlint = lsp::boolean(v, "mlint.enable", mlint);
        if (auto a = lsp::at(v, "mlint.args"); a && a->kind == lsp::Json::Arr) {
            mlintArgs.clear();
            for (auto& x : a->a) if (x.kind == lsp::Json::Str) mlintArgs.push_back(x.s);
        }
        if (auto n = lsp::num(v, "maxFiles", 0); n > 0) maxFiles = (size_t)n;
    }
};

// ---------------------------------------------------------------- results

struct Range {
    unsigned l0 = 0, c0 = 0, l1 = 0, c1 = 0;
    bool contains(unsigned l, unsigned c) const {
        if (l < l0 || l > l1) return false;
        if (l == l0 && c < c0) return false;
        if (l == l1 && c > c1) return false;
        return true;
    }
    bool smallerThan(const Range& o) const {
        if (l1 - l0 != o.l1 - o.l0) return l1 - l0 < o.l1 - o.l0;
        return (c1 - c0) < (o.c1 - o.c0);
    }
    bool operator<(const Range& o) const {
        return std::tie(l0, c0, l1, c1) < std::tie(o.l0, o.c0, o.l1, o.c1);
    }
    bool operator==(const Range& o) const {
        return l0 == o.l0 && c0 == o.c0 && l1 == o.l1 && c1 == o.c1;
    }
    std::string json() const {
        return Obj()
            .raw("start", Obj().i("line", l0).i("character", c0).str())
            .raw("end", Obj().i("line", l1).i("character", c1).str())
            .str();
    }
};

struct Def {
    std::string name, path, hover;
    int lspKind = 13;  // LSP SymbolKind
    Range r;
};

struct Ref {
    Range r;
    int def;
    bool operator<(const Ref& o) const { return std::tie(r, def) < std::tie(o.r, o.def); }
    bool operator==(const Ref& o) const { return r == o.r && def == o.def; }
};

struct DocSym {
    std::string name, detail;
    int kind = 13;
    Range r, sel;
    std::vector<DocSym> kids;
    std::string json() const {
        std::vector<std::string> k;
        for (auto& c : kids) k.push_back(c.json());
        Obj o;
        o.s("name", name.empty() ? "?" : name).i("kind", kind).raw("range", r.json()).raw("selectionRange", sel.json());
        if (!detail.empty()) o.s("detail", detail);
        if (!k.empty()) o.raw("children", lsp::arr(k));
        return o.str();
    }
};

struct Analysis {
    std::string root;                          // the open file it was made for
    std::map<std::string, int> versions;       // open files it read, and their versions
    std::vector<std::string> files;            // the design files given to slang
    std::vector<std::string> args;             // the options with them
    std::vector<std::string> tops;
    std::vector<Def> defs;
    std::map<std::string, std::vector<Ref>> refs;   // path -> sorted
    std::map<std::string, std::vector<std::string>> diags;  // path -> LSP Diagnostic objects
    std::vector<DocSym> outline;               // of `root`
    std::set<std::string> names;               // declared in `root`, for completion
    double ms = 0;
};

// ---------------------------------------------------------------- the index

class Index {
public:
    void build(const std::string& root, size_t limit, unsigned threads) {
        auto files = lsp::listSources(root, limit);
        std::vector<lsp::FileScan> scans(files.size());
        par::parallelFor(files.size(), threads, [&](size_t k) {
            std::string t;
            if (lsp::readFile(files[k], t)) scans[k] = lsp::scanText(t, files[k]);
        });
        std::lock_guard<std::mutex> lk(m_);
        files_.clear();
        for (size_t k = 0; k < files.size(); k++) files_[files[k]] = std::move(scans[k]);
        reindex();
        built_ = true;
    }
    bool built() const { return built_; }
    void update(const std::string& path) {
        std::string t;
        std::lock_guard<std::mutex> lk(m_);
        if (lsp::isSvSource(path) && lsp::readFile(path, t)) files_[path] = lsp::scanText(t, path);
        else files_.erase(path);
        reindex();
    }
    void remove(const std::string& path) {
        std::lock_guard<std::mutex> lk(m_);
        files_.erase(path);
        reindex();
    }
    // A copy that the analysis reads without holding the lock.
    struct Snapshot {
        std::map<std::string, lsp::FileScan> files;
        std::multimap<std::string, lsp::UnitDef> byName;
        std::map<std::string, std::vector<std::string>> byBase;
    };
    std::shared_ptr<const Snapshot> snapshot() const {
        std::lock_guard<std::mutex> lk(m_);
        return snap_;
    }

private:
    void reindex() {
        auto s = std::make_shared<Snapshot>();
        s->files = files_;
        for (auto& [p, f] : files_) {
            for (auto& d : f.defs) s->byName.emplace(d.name, d);
            s->byBase[fs::path(p).filename().string()].push_back(p);
        }
        snap_ = s;
    }
    mutable std::mutex m_;
    std::map<std::string, lsp::FileScan> files_;
    std::shared_ptr<const Snapshot> snap_ = std::make_shared<Snapshot>();
    std::atomic<bool> built_{false};
};

// The candidate nearest to `from`: same directory first, then the longest
// common path prefix, then the shortest and first in name order.
std::string nearest(const std::vector<std::string>& cands, const std::string& from) {
    std::string best;
    long bestScore = -1;
    for (auto& c : cands) {
        size_t n = 0;
        while (n < c.size() && n < from.size() && c[n] == from[n]) n++;
        long score = (dirOf(c) == dirOf(from) ? 1000000 : 0) + (long)n * 100 - (long)std::min<size_t>(c.size(), 99);
        if (score > bestScore || (score == bestScore && c < best)) {
            best = c;
            bestScore = score;
        }
    }
    return best;
}

// ---------------------------------------------------------------- the server

struct OpenDoc {
    std::string uri, text;
    int version = 0;
};

class Server {
public:
    Server(lsp::Channel& ch, unsigned threads) : ch_(ch), threads_(threads) {}

    void run();
    // mlsp -check: one analysis per file, diagnostics printed.
    int check(const std::vector<std::string>& files, const std::vector<std::string>& extra);

    Settings settings;
    std::string root;
    std::string selfDir;

private:
    // protocol
    void handle(const lsp::Json& msg);
    void reply(const lsp::Json* id, const std::string& result) {
        if (!id) return;
        ch_.write(Obj().s("jsonrpc", "2.0").raw("id", lsp::write(*id)).raw("result", result).str());
    }
    void replyError(const lsp::Json* id, int code, const std::string& msg) {
        if (!id) return;
        ch_.write(Obj().s("jsonrpc", "2.0").raw("id", lsp::write(*id))
                      .raw("error", Obj().i("code", code).s("message", msg).str()).str());
    }
    void notify(const std::string& method, const std::string& params) {
        ch_.write(Obj().s("jsonrpc", "2.0").s("method", method).raw("params", params).str());
    }
    void logMessage(int type, const std::string& msg) {
        notify("window/logMessage", Obj().i("type", type).s("message", msg).str());
    }

    // requests
    std::string onInitialize(const lsp::Json* params);
    std::string onDefinition(const std::string& path, unsigned line, unsigned ch);
    std::string onReferences(const std::string& path, unsigned line, unsigned ch, bool decl, bool highlight);
    std::string onHover(const std::string& path, unsigned line, unsigned ch);
    std::string onDocumentSymbol(const std::string& path);
    std::string onWorkspaceSymbol(const std::string& query);
    std::string onCompletion(const std::string& path, unsigned line, unsigned ch);
    std::string onCompileInfo(const std::string& path);

    // analysis
    void schedule(const std::string& path, int delayMs);
    void worker();
    std::shared_ptr<Analysis> analyze(const std::string& path, const std::map<std::string, OpenDoc>& docs,
                                      const std::shared_ptr<const Index::Snapshot>& idx);
    std::shared_ptr<const Analysis> fresh(const std::string& path);
    void publish(const std::set<std::string>& paths);
    std::string uriOf(const std::string& path);
    std::string textOf(const std::string& path);
    void ensureIndex();

    // mlint
    void mlintWorker();
    std::string findTool(const std::string& name);

    lsp::Channel& ch_;
    unsigned threads_;
    lsp::Encoding enc_;
    Index index_;
    std::mutex indexBuild_;

    std::mutex m_;
    std::condition_variable cv_, doneCv_;
    std::map<std::string, OpenDoc> docs_;                                 // canonical path -> doc
    std::map<std::string, std::shared_ptr<const Analysis>> analyses_;     // root -> latest
    std::map<std::string, std::chrono::steady_clock::time_point> queue_;  // root -> when
    std::set<std::string> running_;
    std::map<std::string, std::map<std::string, std::vector<std::string>>> mlintDiags_;  // root -> path -> diags
    std::map<std::string, std::set<std::string>> published_;  // path -> what the client has
    std::deque<std::string> mlintQueue_;
    std::condition_variable mlintCv_;
    bool shutdown_ = false, exit_ = false;
};

std::string Server::uriOf(const std::string& path) {
    auto it = docs_.find(path);
    return it != docs_.end() ? it->second.uri : lsp::pathToUri(path);
}

std::string Server::textOf(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = docs_.find(path);
        if (it != docs_.end()) return it->second.text;
    }
    std::string t;
    lsp::readFile(path, t);
    return t;
}

void Server::ensureIndex() {
    std::lock_guard<std::mutex> lk(indexBuild_);
    if (index_.built() || root.empty()) return;
    auto t0 = std::chrono::steady_clock::now();
    index_.build(root, settings.maxFiles, threads_);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    auto s = index_.snapshot();
    logMessage(4, "mlsp: indexed " + std::to_string(s->files.size()) + " files under " + root + " in " +
                      std::to_string((int)ms) + " ms");
}

std::string Server::findTool(const std::string& name) {
    for (auto& d : {settings.binDir, selfDir}) {
        if (d.empty()) continue;
        auto p = fs::path(d) / name;
        if (access(p.c_str(), X_OK) == 0) return p.string();
    }
    if (const char* path = std::getenv("PATH")) {
        std::string ps = path;
        size_t b = 0;
        while (b <= ps.size()) {
            size_t e = ps.find(':', b);
            if (e == std::string::npos) e = ps.size();
            auto p = fs::path(ps.substr(b, e - b)) / name;
            if (e > b && access(p.c_str(), X_OK) == 0) return p.string();
            b = e + 1;
        }
    }
    return "";
}

// ---------------------------------------------------------------- slang -> LSP

struct Mapper {
    const SourceManager& sm;
    const lsp::Encoding& enc;

    // A file location's path and LSP position; false for a macro body or a
    // buffer with no file.
    bool pos(SourceLocation loc, std::string& path, unsigned& line, unsigned& ch, size_t* offset = nullptr) const {
        if (!loc.valid() || !sm.isFileLoc(loc)) return false;
        auto& p = sm.getFullPath(loc.buffer());
        if (p.empty()) return false;
        path = p.string();
        std::string_view text = sm.getSourceText(loc.buffer());
        size_t off = loc.offset();
        if (off > text.size()) return false;
        size_t ls = off;
        while (ls > 0 && text[ls - 1] != '\n') ls--;
        line = (unsigned)sm.getLineNumber(loc) - 1;
        ch = enc.units(text.substr(ls), off - ls);
        if (offset) *offset = off;
        return true;
    }
    bool range(SourceRange r, std::string& path, Range& out) const {
        std::string p2;
        if (!pos(r.start(), path, out.l0, out.c0)) return false;
        if (!pos(r.end(), p2, out.l1, out.c1) || p2 != path || out.l1 < out.l0 ||
            (out.l1 == out.l0 && out.c1 < out.c0)) {
            out.l1 = out.l0;
            out.c1 = out.c0;
        }
        return true;
    }
    // A name at `loc`, `len` bytes long, on one line.
    bool name(SourceLocation loc, size_t len, std::string& path, Range& out) const {
        size_t off;
        if (!pos(loc, path, out.l0, out.c0, &off)) return false;
        std::string_view text = sm.getSourceText(loc.buffer());
        size_t ls = off;
        while (ls > 0 && text[ls - 1] != '\n') ls--;
        len = std::min(len, text.size() - off);
        out.l1 = out.l0;
        out.c1 = enc.units(text.substr(ls), off - ls + len);
        return true;
    }
    // The word at `loc` (for a diagnostic with no range).
    bool word(SourceLocation loc, std::string& path, Range& out) const {
        if (!loc.valid() || !sm.isFileLoc(loc)) return false;
        std::string_view text = sm.getSourceText(loc.buffer());
        size_t e = loc.offset();
        while (e < text.size() && (std::isalnum((unsigned char)text[e]) || text[e] == '_' || text[e] == '$')) e++;
        size_t len = e - loc.offset();
        if (len == 0 && loc.offset() < text.size() && text[loc.offset()] != '\n') len = 1;
        return name(loc, len, path, out);
    }
};

int lspKindOf(const Symbol& s) {
    switch (s.kind) {
        case SymbolKind::Definition:
        case SymbolKind::Instance:
        case SymbolKind::InstanceBody: return 2;   // Module
        case SymbolKind::Package: return 4;
        case SymbolKind::ClassType:
        case SymbolKind::GenericClassDef: return 5;
        case SymbolKind::Subroutine: return s.getParentScope() && s.getParentScope()->asSymbol().kind == SymbolKind::ClassType ? 6 : 12;
        case SymbolKind::ClassProperty: return 7;
        case SymbolKind::Field: return 8;
        case SymbolKind::Parameter:
        case SymbolKind::Specparam: return 14;
        case SymbolKind::TypeParameter: return 26;
        case SymbolKind::EnumValue: return 22;
        case SymbolKind::TypeAlias: return 26;
        case SymbolKind::Port:
        case SymbolKind::MultiPort:
        case SymbolKind::InterfacePort: return 7;
        case SymbolKind::Net: return 13;
        case SymbolKind::Modport: return 11;
        case SymbolKind::CovergroupType: return 23;
        default: return 13;
    }
}

std::string trimSyntax(std::string t, size_t maxLines = 30) {
    // drop leading blank lines and trailing white space
    size_t b = 0;
    while (b < t.size() && (t[b] == '\n' || t[b] == '\r' || t[b] == ' ' || t[b] == '\t')) b++;
    t = t.substr(b);
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    size_t lines = 0, k = 0;
    for (; k < t.size(); k++)
        if (t[k] == '\n' && ++lines >= maxLines) break;
    if (k < t.size()) t = t.substr(0, k) + "\n...";
    return t;
}

std::string hoverOf(const Symbol& s) {
    std::string code, note;
    try {
        switch (s.kind) {
            case SymbolKind::Definition: {
                if (auto syn = s.getSyntax(); syn && syn->kind != SyntaxKind::Unknown) {
                    if (auto* md = syn->as_if<ModuleDeclarationSyntax>())
                        code = trimSyntax(md->header->toString());
                }
                if (code.empty()) code = "module " + std::string(s.name);
                break;
            }
            case SymbolKind::Instance: {
                auto& inst = s.as<InstanceSymbol>();
                code = std::string(inst.getDefinition().name) + " " + std::string(s.name);
                note = inst.getHierarchicalPath();
                break;
            }
            case SymbolKind::Parameter: {
                auto& p = s.as<ParameterSymbol>();
                code = std::string(p.isLocalParam() ? "localparam " : "parameter ") + p.getType().toString() + " " +
                       std::string(s.name) + " = " + p.getValue().toString();
                break;
            }
            case SymbolKind::TypeParameter:
                code = "parameter type " + std::string(s.name);
                break;
            case SymbolKind::EnumValue: {
                auto& e = s.as<EnumValueSymbol>();
                code = std::string(s.name) + " = " + e.getValue().toString();
                break;
            }
            case SymbolKind::Port: {
                auto& p = s.as<PortSymbol>();
                std::string dir = toString(p.direction) == "In" ? "input" : toString(p.direction) == "Out" ? "output"
                                : toString(p.direction) == "Ref" ? "ref" : "inout";
                code = dir + " " + p.getType().toString() + " " + std::string(s.name);
                break;
            }
            case SymbolKind::Subroutine: {
                if (auto syn = s.getSyntax()) {
                    if (auto* fd = syn->as_if<FunctionDeclarationSyntax>()) code = trimSyntax(fd->prototype->toString());
                    else code = trimSyntax(syn->toString(), 3);
                }
                if (code.empty()) code = "function " + std::string(s.name);
                break;
            }
            case SymbolKind::TypeAlias: {
                auto& t = s.as<TypeAliasType>();
                code = "typedef " + t.targetType.getType().toString() + " " + std::string(s.name);
                break;
            }
            case SymbolKind::ClassType: {
                auto& c = s.as<ClassType>();
                code = "class " + std::string(s.name);
                if (auto b = c.getBaseClass()) code += " extends " + b->toString();
                break;
            }
            case SymbolKind::GenericClassDef:
                code = "class " + std::string(s.name) + " #(...)";
                break;
            case SymbolKind::Package:
                code = "package " + std::string(s.name);
                break;
            case SymbolKind::Modport:
                code = "modport " + std::string(s.name);
                break;
            default:
                if (s.isValue()) {
                    auto& v = s.as<ValueSymbol>();
                    code = v.getType().toString() + " " + std::string(s.name);
                    if (s.kind == SymbolKind::Net) code = "wire " + code;
                }
                else {
                    code = std::string(toString(s.kind)) + " " + std::string(s.name);
                }
        }
        if (note.empty() && s.kind != SymbolKind::Definition && s.kind != SymbolKind::Package &&
            s.getParentScope() && s.kind != SymbolKind::ClassType && s.kind != SymbolKind::GenericClassDef)
            note = s.getHierarchicalPath();
    }
    catch (...) {
        code = std::string(s.name);
    }
    std::string md = "```systemverilog\n" + code + "\n```";
    if (!note.empty() && note != s.name) md += "\n\n`" + note + "`";
    return md;
}

// Collects every reference to a named thing slang resolved.
struct Collector {
    Mapper map;
    Analysis& a;
    std::unordered_map<const Symbol*, int> ids;
    std::set<std::string> want;  // files whose references are kept

    int defOf(const Symbol& s) {
        auto it = ids.find(&s);
        if (it != ids.end()) return it->second;
        Def d;
        d.name = std::string(s.name);
        d.lspKind = lspKindOf(s);
        if (!map.name(s.location, s.name.size(), d.path, d.r)) {
            ids[&s] = -1;
            return -1;
        }
        d.hover = hoverOf(s);
        int id = (int)a.defs.size();
        a.defs.push_back(std::move(d));
        ids[&s] = id;
        return id;
    }
    void ref(const std::string& path, const Range& r, const Symbol& s) {
        if (!want.count(path)) return;
        if (s.name.empty() || !s.location.valid()) return;
        int d = defOf(s);
        if (d >= 0) a.refs[path].push_back({r, d});
    }
    void refRange(SourceRange sr, const Symbol& s) {
        std::string p;
        Range r;
        if (map.range(sr, p, r)) ref(p, r, s);
    }
    void refName(SourceLocation loc, size_t len, const Symbol& s) {
        std::string p;
        Range r;
        if (map.name(loc, len, p, r)) ref(p, r, s);
    }

    void bindNames(const SyntaxNode& n, const Scope& scope) {
        if (n.kind == SyntaxKind::IdentifierName || n.kind == SyntaxKind::ScopedName) {
            std::string name = n.toString();
            name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return std::isspace((unsigned char)c); }),
                       name.end());
            try {
                if (auto* sym = scope.lookupName(name)) refRange(n.sourceRange(), *sym);
            }
            catch (...) {
            }
            return;
        }
        for (size_t k = 0; k < n.getChildCount(); k++)
            if (auto* c = n.childNode(k)) bindNames(*c, scope);
    }

    void symbol(const Symbol& s) {
        if (!s.name.empty() && s.location.valid() && s.kind != SymbolKind::InstanceBody &&
            s.kind != SymbolKind::CompilationUnit && s.kind != SymbolKind::Root)
            refName(s.location, s.name.size(), s);
        // Names in a declared type and its dimensions ([W-1:0]): slang keeps
        // their values, not the expressions, so they are looked up here.
        if (auto dt = s.getDeclaredType(); dt && s.getParentScope()) {
            if (auto* ts = dt->getTypeSyntax()) bindNames(*ts, *s.getParentScope());
            if (auto* ds = dt->getDimensionSyntax())
                for (auto* d : *ds) bindNames(*d, *s.getParentScope());
        }
        // the declared type: `my_t x;` and `pkg::my_t x;`
        if (auto dt = s.getDeclaredType(); dt && dt->getTypeSyntax()) {
            auto* ts = dt->getTypeSyntax();
            if (ts->kind == SyntaxKind::NamedType) {
                const Type& t = dt->getType();
                if (!t.name.empty() && t.location.valid()) refRange(ts->sourceRange(), t);
            }
        }
        if (s.kind == SymbolKind::Instance) {
            auto& inst = s.as<InstanceSymbol>();
            auto* syn = inst.getSyntax();
            if (syn && syn->kind == SyntaxKind::HierarchicalInstance) {
                auto& hi = syn->as<HierarchicalInstanceSyntax>();
                if (hi.parent && hi.parent->kind == SyntaxKind::HierarchyInstantiation) {
                    auto& hs = hi.parent->as<HierarchyInstantiationSyntax>();
                    const auto& def = inst.getDefinition();
                    refName(hs.type.location(), hs.type.valueText().size(), def);
                    if (hs.parameters) {
                        for (auto* pa : hs.parameters->parameters) {
                            if (pa->kind != SyntaxKind::NamedParamAssignment) continue;
                            auto& np = pa->as<NamedParamAssignmentSyntax>();
                            if (auto* p = inst.body.find(np.name.valueText()))
                                refName(np.name.location(), np.name.valueText().size(), *p);
                        }
                    }
                }
                for (auto* pc : hi.connections) {
                    if (pc->kind != SyntaxKind::NamedPortConnection) continue;
                    auto& np = pc->as<NamedPortConnectionSyntax>();
                    if (auto* p = inst.body.findPort(np.name.valueText()))
                        refName(np.name.location(), np.name.valueText().size(), *p);
                }
            }
        }
        if (s.kind == SymbolKind::ClassType) {
            auto& c = s.as<ClassType>();
            if (auto* syn = c.getSyntax(); syn && syn->kind == SyntaxKind::ClassDeclaration) {
                auto& cd = syn->as<ClassDeclarationSyntax>();
                if (cd.extendsClause && c.getBaseClass()) {
                    const Type& b = *c.getBaseClass();
                    const Symbol* target = &b;
                    if (b.kind == SymbolKind::ClassType && b.as<ClassType>().genericClass)
                        target = b.as<ClassType>().genericClass;
                    refRange(cd.extendsClause->baseName->sourceRange(), *target);
                }
            }
        }
    }

    void expr(const Expression& e) {
        switch (e.kind) {
            case ExpressionKind::NamedValue:
            case ExpressionKind::HierarchicalValue:
                refRange(e.sourceRange, e.as<ValueExpressionBase>().symbol);
                break;
            case ExpressionKind::ArbitrarySymbol:
                refRange(e.sourceRange, *e.as<ArbitrarySymbolExpression>().symbol);
                break;
            case ExpressionKind::MemberAccess:
                refRange(e.sourceRange, e.as<MemberAccessExpression>().member);
                break;
            case ExpressionKind::Call: {
                auto& c = e.as<CallExpression>();
                if (c.isSystemCall()) break;
                auto* sub = std::get<0>(c.subroutine);
                if (!sub) break;
                SourceRange sr = e.sourceRange;
                if (e.syntax && e.syntax->kind == SyntaxKind::InvocationExpression)
                    sr = e.syntax->as<InvocationExpressionSyntax>().left->sourceRange();
                refRange(sr, *sub);
                break;
            }
            default: break;
        }
    }
};

struct RefVisitor : public ASTVisitor<RefVisitor, VisitFlags::AllCanonical> {
    Collector& c;
    explicit RefVisitor(Collector& c) : c(c) {}
    template<typename T>
    void handle(const T& t) {
        if constexpr (std::is_base_of_v<Symbol, T>) c.symbol(t);
        if constexpr (std::is_base_of_v<Expression, T>) c.expr(t);
        visitDefault(t);
    }
};

// The outline of one file, from its syntax tree (it works on a file that
// does not elaborate).
struct Outliner {
    Mapper map;
    std::string path;

    bool rangeOf(const SyntaxNode& n, Range& r) {
        std::string p;
        return map.range(n.sourceRange(), p, r) && p == path;
    }
    bool nameOf(Token t, Range& r) {
        std::string p;
        return t.valid() && map.name(t.location(), t.rawText().size(), p, r) && p == path;
    }
    void declarators(const SeparatedSyntaxList<DeclaratorSyntax>& ds, const SyntaxNode& whole, int kind,
                     const std::string& detail, std::vector<DocSym>& out) {
        for (auto* d : ds) {
            DocSym s;
            s.name = std::string(d->name.valueText());
            s.kind = kind;
            s.detail = detail;
            if (!nameOf(d->name, s.sel)) continue;
            if (!rangeOf(whole, s.r)) s.r = s.sel;
            out.push_back(std::move(s));
        }
    }
    void walk(const SyntaxNode& n, std::vector<DocSym>& out) {
        switch (n.kind) {
            case SyntaxKind::ModuleDeclaration:
            case SyntaxKind::InterfaceDeclaration:
            case SyntaxKind::ProgramDeclaration:
            case SyntaxKind::PackageDeclaration: {
                auto& md = n.as<ModuleDeclarationSyntax>();
                DocSym s;
                s.name = std::string(md.header->name.valueText());
                s.kind = n.kind == SyntaxKind::PackageDeclaration ? 4 : n.kind == SyntaxKind::InterfaceDeclaration ? 11 : 2;
                s.detail = std::string(md.header->moduleKeyword.valueText());
                if (!nameOf(md.header->name, s.sel) || !rangeOf(n, s.r)) break;
                if (md.header->parameters) walk(*md.header->parameters, s.kids);
                if (md.header->ports) walk(*md.header->ports, s.kids);
                for (auto* m : md.members) walk(*m, s.kids);
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::ClassDeclaration: {
                auto& cd = n.as<ClassDeclarationSyntax>();
                DocSym s;
                s.name = std::string(cd.name.valueText());
                s.kind = 5;
                s.detail = "class";
                if (!nameOf(cd.name, s.sel) || !rangeOf(n, s.r)) break;
                for (auto* m : cd.items) walk(*m, s.kids);
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::FunctionDeclaration:
            case SyntaxKind::TaskDeclaration: {
                auto& fd = n.as<FunctionDeclarationSyntax>();
                DocSym s;
                auto* nm = fd.prototype->name.get();
                s.name = trimSyntax(nm->toString());
                s.kind = 12;
                s.detail = std::string(fd.prototype->keyword.valueText());
                if (!rangeOf(*nm, s.sel) || !rangeOf(n, s.r)) break;
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::DataDeclaration: {
                auto& dd = n.as<DataDeclarationSyntax>();
                declarators(dd.declarators, n, 13, trimSyntax(dd.type->toString()), out);
                return;
            }
            case SyntaxKind::NetDeclaration: {
                auto& nd = n.as<NetDeclarationSyntax>();
                declarators(nd.declarators, n, 13, std::string(nd.netType.valueText()), out);
                return;
            }
            case SyntaxKind::ParameterDeclaration: {
                auto& pd = n.as<ParameterDeclarationSyntax>();
                declarators(pd.declarators, n, 14, std::string(pd.keyword.valueText()), out);
                return;
            }
            case SyntaxKind::PortDeclaration: {
                auto& pd = n.as<PortDeclarationSyntax>();
                declarators(pd.declarators, n, 7, trimSyntax(pd.header->toString()), out);
                return;
            }
            case SyntaxKind::ImplicitAnsiPort: {
                auto& ip = n.as<ImplicitAnsiPortSyntax>();
                DocSym s;
                s.name = std::string(ip.declarator->name.valueText());
                s.kind = 7;
                s.detail = trimSyntax(ip.header->toString());
                if (!nameOf(ip.declarator->name, s.sel) || !rangeOf(n, s.r)) return;
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::TypedefDeclaration: {
                auto& td = n.as<TypedefDeclarationSyntax>();
                DocSym s;
                s.name = std::string(td.name.valueText());
                s.kind = 26;
                s.detail = "typedef";
                if (!nameOf(td.name, s.sel) || !rangeOf(n, s.r)) return;
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::HierarchyInstantiation: {
                auto& hs = n.as<HierarchyInstantiationSyntax>();
                for (auto* hi : hs.instances) {
                    if (!hi->decl) continue;
                    DocSym s;
                    s.name = std::string(hi->decl->name.valueText());
                    s.kind = 19;  // Object
                    s.detail = std::string(hs.type.valueText());
                    if (!nameOf(hi->decl->name, s.sel) || !rangeOf(*hi, s.r)) continue;
                    out.push_back(std::move(s));
                }
                return;
            }
            case SyntaxKind::GenerateBlock: {
                auto& gb = n.as<GenerateBlockSyntax>();
                Token nm = gb.beginName ? gb.beginName->name : gb.label ? gb.label->name : Token();
                if (!nm.valid()) break;
                DocSym s;
                s.name = std::string(nm.valueText());
                s.kind = 3;  // Namespace
                s.detail = "generate";
                if (!nameOf(nm, s.sel) || !rangeOf(n, s.r)) break;
                for (auto* m : gb.members) walk(*m, s.kids);
                out.push_back(std::move(s));
                return;
            }
            case SyntaxKind::AlwaysBlock:
            case SyntaxKind::AlwaysCombBlock:
            case SyntaxKind::AlwaysFFBlock:
            case SyntaxKind::AlwaysLatchBlock:
            case SyntaxKind::InitialBlock:
            case SyntaxKind::FinalBlock:
                return;  // statements hold no declarations worth an outline entry
            default: break;
        }
        for (size_t k = 0; k < n.getChildCount(); k++)
            if (auto* c = n.childNode(k)) walk(*c, out);
    }
};

std::shared_ptr<const Analysis> Server::fresh(const std::string& path) {
    std::unique_lock<std::mutex> lk(m_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    for (;;) {
        auto d = docs_.find(path);
        auto a = analyses_.find(path);
        bool upToDate = a != analyses_.end() && (d == docs_.end() || a->second->versions.count(path) == 0 ||
                                                 a->second->versions.at(path) >= d->second.version);
        bool pending = queue_.count(path) || running_.count(path);
        if ((upToDate && !pending) || (!pending && a != analyses_.end())) return a->second;
        if (!pending && d != docs_.end()) {
            queue_[path] = std::chrono::steady_clock::now();
            cv_.notify_all();
        }
        if (d == docs_.end() && a == analyses_.end() && !pending) {
            // not open: analyze it as it is on disk
            queue_[path] = std::chrono::steady_clock::now();
            cv_.notify_all();
        }
        if (doneCv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            auto a2 = analyses_.find(path);
            return a2 != analyses_.end() ? a2->second : nullptr;
        }
    }
}

void Server::schedule(const std::string& path, int delayMs) {
    std::lock_guard<std::mutex> lk(m_);
    queue_[path] = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
    cv_.notify_all();
}

void Server::worker() {
    for (;;) {
        std::string path;
        std::map<std::string, OpenDoc> docs;
        {
            std::unique_lock<std::mutex> lk(m_);
            for (;;) {
                if (exit_) return;
                if (!queue_.empty()) {
                    auto it = std::min_element(queue_.begin(), queue_.end(),
                                               [](auto& x, auto& y) { return x.second < y.second; });
                    if (it->second <= std::chrono::steady_clock::now()) {
                        path = it->first;
                        queue_.erase(it);
                        break;
                    }
                    cv_.wait_until(lk, it->second);
                }
                else {
                    cv_.wait(lk);
                }
            }
            running_.insert(path);
            docs = docs_;
        }
        ensureIndex();
        std::shared_ptr<Analysis> a;
        try {
            a = analyze(path, docs, index_.snapshot());
        }
        catch (const std::exception& e) {
            logMessage(1, "mlsp: analysis of " + path + " failed: " + e.what());
        }
        std::set<std::string> touched;
        {
            std::lock_guard<std::mutex> lk(m_);
            running_.erase(path);
            if (a) {
                if (auto old = analyses_.find(path); old != analyses_.end())
                    for (auto& [p, d] : old->second->diags) touched.insert(p);
                for (auto& [p, d] : a->diags) touched.insert(p);
                touched.insert(path);
                analyses_[path] = a;
            }
        }
        doneCv_.notify_all();
        if (a) publish(touched);
    }
}

void Server::publish(const std::set<std::string>& paths) {
    std::vector<std::pair<std::string, std::string>> out;
    {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& p : paths) {
            std::set<std::string> d;
            auto own = analyses_.find(p);
            if (own != analyses_.end() && docs_.count(p)) {
                if (auto it = own->second->diags.find(p); it != own->second->diags.end()) d.insert(it->second.begin(), it->second.end());
            }
            else {
                for (auto& [r, a] : analyses_)
                    if (auto it = a->diags.find(p); it != a->diags.end()) d.insert(it->second.begin(), it->second.end());
            }
            for (auto& [r, m] : mlintDiags_)
                if (auto it = m.find(p); it != m.end()) d.insert(it->second.begin(), it->second.end());
            if (d == published_[p]) continue;
            published_[p] = d;
            out.emplace_back(uriOf(p), lsp::arr(std::vector<std::string>(d.begin(), d.end())));
        }
    }
    for (auto& [uri, arr] : out)
        notify("textDocument/publishDiagnostics", Obj().s("uri", uri).raw("diagnostics", arr).str());
}

std::shared_ptr<Analysis> Server::analyze(const std::string& path0, const std::map<std::string, OpenDoc>& docs,
                                          const std::shared_ptr<const Index::Snapshot>& idx) {
    auto t0 = std::chrono::steady_clock::now();
    auto a = std::make_shared<Analysis>();
    a->root = path0;
    std::string path = path0;

    auto scanOf = [&](const std::string& p) -> lsp::FileScan {
        if (auto d = docs.find(p); d != docs.end()) return lsp::scanText(d->second.text, p);
        if (auto f = idx->files.find(p); f != idx->files.end()) return f->second;
        std::string t;
        return lsp::readFile(p, t) ? lsp::scanText(t, p) : lsp::FileScan{};
    };

    // A header is analyzed inside a file that includes it.
    if (lsp::isHeader(path)) {
        std::vector<std::string> incl;
        std::string base = fs::path(path).filename().string();
        for (auto& [p, f] : idx->files)
            if (!lsp::isHeader(p) && std::find(f.includes.begin(), f.includes.end(), base) != f.includes.end())
                incl.push_back(p);
        if (!incl.empty()) path = nearest(incl, path);
    }

    std::vector<std::string> args = {"mlsp"};
    bool userStd = false;
    for (auto& s : settings.args) userStd |= s.rfind("--std", 0) == 0;
    if (!userStd) args.push_back("--std=1800-2023");
    for (auto& s : settings.args) args.push_back(s);

    std::vector<std::string> files, incdirs;
    std::set<std::string> prelude;  // files whose `defines others use
    std::string uvm = settings.uvmHome;
    if (uvm.empty() && std::getenv("UVM_HOME")) uvm = std::getenv("UVM_HOME");
    bool listMode = !settings.fileList.empty();
    std::string fileList = settings.fileList;
    if (listMode && fs::path(fileList).is_relative() && !root.empty()) fileList = (fs::path(root) / fileList).string();

    if (listMode) {
        args.push_back("-f");
        args.push_back(fileList);
        std::string t;
        lsp::readFile(fileList, t);
        if (t.find(fs::path(path).filename().string()) == std::string::npos) files.push_back(path);
    }
    else {
        // The closure: files defining the design units the open file uses.
        std::set<std::string> chosen{path}, defined, definedMacros, seenIncl;
        std::set<std::string> includedBases;
        std::deque<std::string> work{path};
        files.push_back(path);
        std::set<std::string> incSet;
        auto addIncludes = [&](const lsp::FileScan& sc, const std::string& from, auto&& self) -> void {
            for (auto& inc : sc.includes) {
                std::string base = fs::path(inc).filename().string();
                includedBases.insert(base);
                if (!seenIncl.insert(base).second) continue;
                auto it = idx->byBase.find(base);
                std::string hdr;
                if (fs::exists(fs::path(dirOf(from)) / inc)) hdr = canon((fs::path(dirOf(from)) / inc).string());
                else if (it != idx->byBase.end()) {
                    hdr = nearest(it->second, from);
                    // `include "sub/x.svh": the directory that holds sub/
                    std::string dir = dirOf(hdr);
                    for (auto q = fs::path(inc).parent_path(); !q.empty(); q = q.parent_path())
                        dir = dirOf(dir);
                    if (incSet.insert(dir).second) incdirs.push_back(dir);
                }
                if (hdr.empty()) continue;
                auto hs = scanOf(hdr);
                for (auto& d : hs.defs) (d.kind == "macro" ? definedMacros : defined).insert(d.name);
                // what the header uses counts as used by the includer
                for (auto& id : hs.uses) work.push_back("\x01" + id);
                for (auto& m : hs.macros) work.push_back("\x02" + m);
                self(hs, hdr, self);
            }
        };
        while (!work.empty() && files.size() < 3000) {
            std::string f = work.front();
            work.pop_front();
            std::vector<std::string> ids, macros;
            if (f[0] == '\x01') ids.push_back(f.substr(1));
            else if (f[0] == '\x02') macros.push_back(f.substr(1));
            else {
                auto sc = scanOf(f);
                for (auto& d : sc.defs) (d.kind == "macro" ? definedMacros : defined).insert(d.name);
                ids.assign(sc.uses.begin(), sc.uses.end());
                macros.assign(sc.macros.begin(), sc.macros.end());
                addIncludes(sc, f, addIncludes);
            }
            for (auto& id : ids) {
                if (defined.count(id)) continue;
                auto [b, e] = idx->byName.equal_range(id);
                std::vector<std::string> cands;
                for (auto it = b; it != e; ++it) {
                    auto& k = it->second.kind;
                    if (k == "class" || k == "macro") continue;
                    if (lsp::isHeader(it->second.path) || includedBases.count(fs::path(it->second.path).filename().string()))
                        continue;
                    cands.push_back(it->second.path);
                }
                if (cands.empty()) {
                    if (id == "uvm_pkg" && !uvm.empty()) {
                        std::string pkg = (fs::path(uvm) / "src" / "uvm_pkg.sv").string();
                        std::string src = (fs::path(uvm) / "src").string();
                        if (fs::exists(pkg) && chosen.insert(pkg).second) {
                            files.insert(files.begin(), pkg);
                            if (incSet.insert(src).second) incdirs.push_back(src);
                            defined.insert("uvm_pkg");
                        }
                    }
                    continue;
                }
                std::string pick = nearest(cands, path);
                defined.insert(id);
                if (chosen.insert(pick).second) {
                    files.push_back(pick);
                    work.push_back(pick);
                }
            }
            for (auto& m : macros) {
                if (definedMacros.count(m)) continue;
                auto [b, e] = idx->byName.equal_range(m);
                std::vector<std::string> cands;
                for (auto it = b; it != e; ++it)
                    if (it->second.kind == "macro") cands.push_back(it->second.path);
                if (cands.empty()) continue;
                std::string pick = nearest(cands, path);
                definedMacros.insert(m);
                // its other macros come along
                for (auto& d : scanOf(pick).defs) if (d.kind == "macro") definedMacros.insert(d.name);
                if (prelude.insert(pick).second && !chosen.count(pick)) {
                    chosen.insert(pick);
                    work.push_back(pick);
                }
            }
        }
        if (!prelude.empty()) {
            // `defines from other files reach this one: one compilation unit,
            // the defining files first (as a -f file list in that order would).
            std::vector<std::string> ordered;
            for (auto& p : prelude) ordered.push_back(p);
            for (auto& p : files) if (!prelude.count(p)) ordered.push_back(p);
            files = ordered;
            args.push_back("--single-unit");
        }
    }
    for (auto& d : incdirs) args.push_back("+incdir+" + d);

    // slang, set up as msim sets it up.
    driver::Driver drv;
    drv.addStandardArgs();
    std::vector<std::string> cl = args;
    std::set<std::string> openInSet;
    // An open file is named by its path too (the source manager hands out
    // the editor's text for it, below); only one not saved anywhere yet goes
    // in as a bare buffer.
    for (auto& f : files) {
        std::error_code ec;
        if (!docs.count(f) || fs::exists(f, ec)) cl.push_back(f);
        else openInSet.insert(f);
    }
    std::vector<const char*> cargv;
    for (auto& s : cl) cargv.push_back(s.c_str());
    if (!drv.parseCommandLine((int)cargv.size(), cargv.data())) throw std::runtime_error("bad front-end options");
    if (auto& f = drv.options.compilationFlags[CompilationFlags::AllowUseBeforeDeclare]; !f) f = true;
    drv.options.errorLimit = 0;
    if (!drv.processOptions()) throw std::runtime_error("bad front-end options");
    drv.diagEngine.setSeverity(diag::DuplicateDefinition, DiagnosticSeverity::Warning);
    drv.diagEngine.setSeverity(diag::RangeOOB, DiagnosticSeverity::Warning);
    drv.diagEngine.setSeverity(diag::IndexOOB, DiagnosticSeverity::Warning);
    drv.diagEngine.setSeverity(diag::RangeWidthOOB, DiagnosticSeverity::Warning);
    // Every open file is read from the editor, whether it is a design file
    // or an `included header.
    for (auto& [p, d] : docs) {
        auto buf = drv.sourceManager.assignText(p, d.text);
        if (openInSet.count(p)) drv.sourceLoader.addBuffer(buf);
        a->versions[p] = d.version;
    }
    // Keep the given order: prelude files first.
    if (!drv.parseAllSources()) {
        // missing files were reported as diagnostics; go on with what parsed
    }
    auto comp = drv.createCompilation();
    vendor::declare(*comp);
    if (!drv.options.timeScale) {
        for (auto b : drv.sourceManager.getAllBuffers()) {
            std::string_view t = drv.sourceManager.getSourceText(b);
            if (t.find("`timescale") != t.npos || t.find("timeunit") != t.npos) {
                // as msim: a mix of timescale and none is not an error
                comp->getOptions();
                break;
            }
        }
    }

    Mapper map{drv.sourceManager, enc_};
    std::set<std::string> inSet(files.begin(), files.end());
    for (auto& d : comp->getAllDiagnostics()) {
        auto sev = drv.diagEngine.getSeverity(d.code, d.location);
        if (sev == DiagnosticSeverity::Ignored) continue;
        int lsev = sev == DiagnosticSeverity::Error || sev == DiagnosticSeverity::Fatal ? 1
                 : sev == DiagnosticSeverity::Warning ? 2 : 3;
        std::string p;
        Range r;
        SourceLocation loc = drv.sourceManager.getFullyExpandedLoc(d.location);
        bool ok = false;
        if (!d.ranges.empty()) {
            SourceRange sr(drv.sourceManager.getFullyExpandedLoc(d.ranges[0].start()),
                           drv.sourceManager.getFullyExpandedLoc(d.ranges[0].end()));
            if (sr.start().buffer() == loc.buffer() && sr.start() <= loc && loc <= sr.end()) ok = map.range(sr, p, r);
        }
        if (!ok) ok = map.word(loc, p, r);
        if (!ok) continue;
        std::string msg = drv.diagEngine.formatMessage(d);
        // notes ("declared here") as related information
        std::vector<std::string> rel;
        for (auto& n : d.notes) {
            std::string np;
            Range nr;
            if (map.word(drv.sourceManager.getFullyExpandedLoc(n.location), np, nr))
                rel.push_back(Obj().raw("location", Obj().s("uri", lsp::pathToUri(np)).raw("range", nr.json()).str())
                                  .s("message", drv.diagEngine.formatMessage(n)).str());
        }
        Obj o;
        o.raw("range", r.json()).i("severity", lsev).s("source", "slang").s("code", std::string(toString(d.code))).s("message", msg);
        if (!rel.empty()) o.raw("relatedInformation", lsp::arr(rel));
        a->diags[p].push_back(o.str());
    }
    for (auto& [p, v] : a->diags) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }

    // References, for every file in the set and the headers they include.
    Collector col{map, *a, {}, {}};
    for (auto b : drv.sourceManager.getAllBuffers()) {
        auto& fp = drv.sourceManager.getFullPath(b);
        if (!fp.empty()) col.want.insert(fp.string());
    }
    RefVisitor rv(col);
    auto& rootSym = comp->getRoot();
    rootSym.visit(rv);
    for (auto* pkg : comp->getPackages()) {
        std::string p;
        unsigned l, c;
        if (map.pos(pkg->location, p, l, c) && col.want.count(p)) pkg->visit(rv);
    }
    for (auto& [p, v] : a->refs) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    for (auto* t : rootSym.topInstances) a->tops.push_back(std::string(t->name));
    std::sort(a->tops.begin(), a->tops.end());

    // The outline of the open file.
    for (auto& tree : drv.syntaxTrees) {
        Outliner ol{map, path0};
        std::vector<DocSym> out;
        ol.walk(tree->root(), out);
        for (auto& s : out) a->outline.push_back(std::move(s));
    }
    std::function<void(const std::vector<DocSym>&)> names = [&](const std::vector<DocSym>& v) {
        for (auto& s : v) {
            a->names.insert(s.name);
            names(s.kids);
        }
    };
    names(a->outline);
    for (auto& d : a->defs)
        if (d.path == path0) a->names.insert(d.name);

    a->files = files;
    a->args.assign(args.begin() + 1, args.end());
    a->ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return a;
}

// ---------------------------------------------------------------- requests

const Ref* refAt(const Analysis& a, const std::string& path, unsigned line, unsigned ch) {
    auto it = a.refs.find(path);
    if (it == a.refs.end()) return nullptr;
    const Ref* best = nullptr;
    for (auto& r : it->second)
        if (r.r.contains(line, ch) && (!best || r.r.smallerThan(best->r))) best = &r;
    return best;
}

std::string wordAt(const std::string& text, unsigned line, unsigned ch, const lsp::Encoding& enc, bool* tick = nullptr) {
    auto starts = lsp::lineStarts(text);
    std::string_view l = lsp::lineOf(text, starts, line);
    size_t k = enc.bytes(l, ch);
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; };
    size_t b = k, e = k;
    while (b > 0 && isId(l[b - 1])) b--;
    while (e < l.size() && isId(l[e])) e++;
    if (tick) *tick = b > 0 && l[b - 1] == '`';
    return std::string(l.substr(b, e - b));
}

std::string locJson(const std::string& uri, const Range& r) {
    return Obj().s("uri", uri).raw("range", r.json()).str();
}

std::string Server::onDefinition(const std::string& path, unsigned line, unsigned ch) {
    auto a = fresh(path);
    if (a) {
        if (auto* r = refAt(*a, path, line, ch)) {
            auto& d = a->defs[r->def];
            std::lock_guard<std::mutex> lk(m_);
            return locJson(uriOf(d.path), d.r);
        }
    }
    std::string text = textOf(path);
    // `include "file"
    {
        auto starts = lsp::lineStarts(text);
        std::string l(lsp::lineOf(text, starts, line));
        size_t inc = l.find("`include");
        size_t q1 = l.find('"', inc == std::string::npos ? 0 : inc), q2 = q1 == std::string::npos ? q1 : l.find('"', q1 + 1);
        if (inc != std::string::npos && q2 != std::string::npos) {
            std::string name = l.substr(q1 + 1, q2 - q1 - 1);
            std::vector<std::string> cand;
            if (fs::exists(fs::path(dirOf(path)) / name)) cand.push_back(canon((fs::path(dirOf(path)) / name).string()));
            if (a)
                for (auto& arg : a->args)
                    if (arg.rfind("+incdir+", 0) == 0 && fs::exists(fs::path(arg.substr(8)) / name))
                        cand.push_back(canon((fs::path(arg.substr(8)) / name).string()));
            if (cand.empty()) {
                auto idx = index_.snapshot();
                if (auto it = idx->byBase.find(fs::path(name).filename().string()); it != idx->byBase.end())
                    cand.push_back(nearest(it->second, path));
            }
            if (!cand.empty()) {
                std::lock_guard<std::mutex> lk(m_);
                return locJson(uriOf(cand[0]), Range{});
            }
        }
    }
    // A name slang did not resolve (the file does not elaborate, a macro):
    // the workspace index.
    bool tick = false;
    std::string w = wordAt(text, line, ch, enc_, &tick);
    if (w.empty()) return "null";
    auto idx = index_.snapshot();
    auto [b, e] = idx->byName.equal_range(w);
    std::vector<std::string> out;
    for (auto it = b; it != e; ++it) {
        if (tick != (it->second.kind == "macro")) continue;
        std::string t;
        Range r;
        r.l0 = r.l1 = it->second.line;
        if (lsp::readFile(it->second.path, t)) {
            auto st = lsp::lineStarts(t);
            auto l = lsp::lineOf(t, st, it->second.line);
            r.c0 = enc_.units(l, it->second.col);
            r.c1 = enc_.units(l, it->second.col + w.size());
        }
        std::lock_guard<std::mutex> lk(m_);
        out.push_back(locJson(uriOf(it->second.path), r));
    }
    return out.empty() ? "null" : lsp::arr(out);
}

std::string Server::onReferences(const std::string& path, unsigned line, unsigned ch, bool decl, bool highlight) {
    auto a = fresh(path);
    if (!a) return "[]";
    auto* r = refAt(*a, path, line, ch);
    if (!r) return "[]";
    int def = r->def;
    auto& d = a->defs[def];
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(m_);
    for (auto& [p, v] : a->refs) {
        if (highlight && p != path) continue;
        for (auto& x : v) {
            if (x.def != def) continue;
            bool isDecl = p == d.path && x.r == d.r;
            if (highlight) out.push_back(Obj().raw("range", x.r.json()).i("kind", isDecl ? 3 : 2).str());
            else if (decl || !isDecl) out.push_back(locJson(uriOf(p), x.r));
        }
    }
    return lsp::arr(out);
}

std::string Server::onHover(const std::string& path, unsigned line, unsigned ch) {
    auto a = fresh(path);
    if (!a) return "null";
    auto* r = refAt(*a, path, line, ch);
    if (!r) return "null";
    auto& d = a->defs[r->def];
    return Obj().raw("contents", Obj().s("kind", "markdown").s("value", d.hover).str()).raw("range", r->r.json()).str();
}

std::string Server::onDocumentSymbol(const std::string& path) {
    auto a = fresh(path);
    if (!a) return "[]";
    std::vector<std::string> out;
    for (auto& s : a->outline) out.push_back(s.json());
    return lsp::arr(out);
}

std::string Server::onWorkspaceSymbol(const std::string& query) {
    ensureIndex();
    auto idx = index_.snapshot();
    std::string q = query;
    for (auto& c : q) c = (char)std::tolower((unsigned char)c);
    std::vector<std::string> out;
    for (auto& [name, d] : idx->byName) {
        std::string n = name;
        for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        if (!q.empty() && n.find(q) == std::string::npos) continue;
        int kind = d.kind == "package" ? 4 : d.kind == "class" ? 5 : d.kind == "interface" ? 11 : d.kind == "macro" ? 14 : 2;
        Range r;
        r.l0 = r.l1 = d.line;
        r.c0 = d.col;
        r.c1 = d.col + (unsigned)name.size();
        std::string uri;
        {
            std::lock_guard<std::mutex> lk(m_);
            uri = uriOf(d.path);
        }
        out.push_back(Obj().s("name", (d.kind == "macro" ? "`" : "") + name).i("kind", kind)
                          .s("containerName", d.kind).raw("location", locJson(uri, r)).str());
        if (out.size() >= 1000) break;
    }
    return lsp::arr(out);
}

const std::vector<std::string>& svKeywords() {
    static const std::vector<std::string> k = {
        "module", "endmodule", "input", "output", "inout", "logic", "wire", "reg", "bit", "byte", "int",
        "integer", "shortint", "longint", "real", "time", "string", "parameter", "localparam", "assign",
        "always", "always_comb", "always_ff", "always_latch", "initial", "final", "begin", "end", "if", "else",
        "case", "casez", "casex", "unique", "priority", "endcase", "default", "for", "foreach", "while", "do",
        "repeat", "forever", "generate", "endgenerate", "genvar", "function", "endfunction", "task", "endtask",
        "return", "typedef", "enum", "struct", "union", "packed", "signed", "unsigned", "interface",
        "endinterface", "modport", "package", "endpackage", "import", "export", "class", "endclass", "extends",
        "virtual", "pure", "extern", "static", "automatic", "local", "protected", "new", "this", "super", "null",
        "rand", "randc", "constraint", "covergroup", "endgroup", "coverpoint", "cross", "bins", "property",
        "endproperty", "sequence", "endsequence", "assert", "assume", "cover", "posedge", "negedge", "edge",
        "or", "and", "not", "fork", "join", "join_any", "join_none", "wait", "disable", "program", "endprogram",
        "clocking", "endclocking", "inside", "const", "var", "void", "event", "mailbox", "semaphore", "ref",
        "specify", "endspecify", "timeunit", "timeprecision", "checker", "endchecker"};
    return k;
}

std::string Server::onCompletion(const std::string& path, unsigned line, unsigned ch) {
    std::string text = textOf(path);
    auto starts = lsp::lineStarts(text);
    std::string_view l = lsp::lineOf(text, starts, line);
    size_t k = enc_.bytes(l, ch), b = k;
    while (b > 0 && (std::isalnum((unsigned char)l[b - 1]) || l[b - 1] == '_')) b--;
    char lead = b > 0 ? l[b - 1] : 0;
    std::map<std::string, int> items;  // label -> CompletionItemKind
    auto idx = index_.snapshot();
    if (lead == '`') {
        for (auto& [n, d] : idx->byName)
            if (d.kind == "macro") items[n] = 15;  // Snippet-like: macro
        for (auto* m : {"include", "define", "ifdef", "ifndef", "else", "elsif", "endif", "timescale", "undef",
                        "default_nettype", "resetall"})
            items[m] = 14;
    }
    else if (lead == '$') {
        for (auto* s : {"display", "write", "strobe", "monitor", "finish", "stop", "time", "realtime", "random",
                        "urandom", "urandom_range", "fopen", "fclose", "fdisplay", "fwrite", "readmemh", "readmemb",
                        "writememh", "dumpfile", "dumpvars", "signed", "unsigned", "clog2", "bits", "size",
                        "countones", "onehot", "onehot0", "isunknown", "rose", "fell", "stable", "past", "sformatf",
                        "error", "warning", "info", "fatal", "cast", "value$plusargs", "test$plusargs"})
            items[s] = 3;  // Function
    }
    else {
        for (auto& kw : svKeywords()) items[kw] = 14;
        for (auto& [n, d] : idx->byName)
            if (d.kind != "macro") items[n] = d.kind == "class" ? 7 : d.kind == "package" ? 9 : 9;
        std::shared_ptr<const Analysis> a;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (auto it = analyses_.find(path); it != analyses_.end()) a = it->second;
        }
        if (a)
            for (auto& n : a->names) items[n] = 6;  // Variable
    }
    std::vector<std::string> out;
    for (auto& [n, kind] : items) {
        out.push_back(Obj().s("label", n).i("kind", kind).str());
        if (out.size() >= 5000) break;
    }
    return Obj().b("isIncomplete", false).raw("items", lsp::arr(out)).str();
}

// minisim/compileInfo: what the editor needs to run msim, mlint or mdebug
// on the same design: the files, the options and the top modules.
std::string Server::onCompileInfo(const std::string& path) {
    auto a = fresh(path);
    if (!a) return "null";
    std::vector<std::string> f, ar, t;
    for (auto& x : a->files) f.push_back(quote(x));
    for (auto& x : a->args) ar.push_back(quote(x));
    for (auto& x : a->tops) t.push_back(quote(x));
    return Obj().raw("files", lsp::arr(f)).raw("args", lsp::arr(ar)).raw("tops", lsp::arr(t))
        .s("root", root).i("ms", (long long)a->ms).str();
}

// ---------------------------------------------------------------- mlint

void Server::mlintWorker() {
    for (;;) {
        std::string path;
        {
            std::unique_lock<std::mutex> lk(m_);
            mlintCv_.wait(lk, [&] { return exit_ || !mlintQueue_.empty(); });
            if (exit_) return;
            path = mlintQueue_.front();
            mlintQueue_.pop_front();
            // a later save of the same file supersedes this one
            if (std::find(mlintQueue_.begin(), mlintQueue_.end(), path) != mlintQueue_.end()) continue;
        }
        if (!settings.mlint) continue;
        std::string mlint = findTool("mlint");
        if (mlint.empty()) continue;
        auto a = fresh(path);
        if (!a) continue;
        char tmpl[] = "/tmp/mlsp_lintXXXXXX.json";
        int fd = mkstemps(tmpl, 5);
        if (fd < 0) continue;
        close(fd);
        std::vector<std::string> argv{mlint, "-q", "-json", tmpl};
        for (auto& s : settings.mlintArgs) argv.push_back(s);
        for (auto& s : a->args) argv.push_back(s);
        for (auto& s : a->files) argv.push_back(s);
        std::vector<char*> cargv;
        for (auto& s : argv) cargv.push_back(s.data());
        cargv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        if (!root.empty()) posix_spawn_file_actions_addchdir_np(&fa, root.c_str());
        pid_t pid;
        int rc = posix_spawn(&pid, mlint.c_str(), &fa, nullptr, cargv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) { std::remove(tmpl); continue; }
        int status = 0;
        waitpid(pid, &status, 0);
        std::string text;
        lsp::readFile(tmpl, text);
        std::remove(tmpl);
        std::map<std::string, std::vector<std::string>> out;
        try {
            auto j = vp::json::parse(text);
            const lsp::Json* fsj = j.get("findings");
            if (fsj && fsj->kind == lsp::Json::Arr) {
                std::map<std::string, std::string> texts;
                for (auto& f : fsj->a) {
                    if (lsp::boolean(&f, "waived", false)) continue;
                    std::string file = lsp::str(&f, "file");
                    if (file.empty()) continue;
                    if (fs::path(file).is_relative() && !root.empty()) file = (fs::path(root) / file).string();
                    file = canon(file);
                    long line = lsp::num(&f, "line", 1) - 1, col = lsp::num(&f, "col", 1) - 1;
                    if (line < 0) line = 0;
                    if (col < 0) col = 0;
                    if (!texts.count(file)) texts[file] = textOf(file);
                    auto& t = texts[file];
                    auto st = lsp::lineStarts(t);
                    auto l = lsp::lineOf(t, st, (size_t)line);
                    size_t e = (size_t)col;
                    while (e < l.size() && (std::isalnum((unsigned char)l[e]) || l[e] == '_' || l[e] == '$')) e++;
                    if (e == (size_t)col) e = std::min(l.size(), e + 1);
                    Range r;
                    r.l0 = r.l1 = (unsigned)line;
                    r.c0 = enc_.units(l, (size_t)col);
                    r.c1 = enc_.units(l, e);
                    std::string sev = lsp::str(&f, "severity");
                    int ls = sev == "error" ? 1 : sev == "warning" ? 2 : 3;
                    std::string rule = lsp::str(&f, "rule");
                    out[file].push_back(Obj().raw("range", r.json()).i("severity", ls).s("source", "mlint")
                                            .s("code", rule).s("message", lsp::str(&f, "text")).str());
                }
            }
        }
        catch (const std::exception& e) {
            if (WIFEXITED(status) && WEXITSTATUS(status) >= 2)
                logMessage(2, "mlint did not run on " + path + " (exit " + std::to_string(WEXITSTATUS(status)) + ")");
            continue;
        }
        std::set<std::string> touched;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& [p, v] : mlintDiags_[path]) touched.insert(p);
            for (auto& [p, v] : out) touched.insert(p);
            mlintDiags_[path] = out;
        }
        publish(touched);
    }
}

// ---------------------------------------------------------------- dispatch

std::string Server::onInitialize(const lsp::Json* params) {
    std::string r = lsp::str(params, "rootUri");
    if (r.empty()) r = lsp::str(params, "rootPath");
    if (auto wf = lsp::at(params, "workspaceFolders"); wf && wf->kind == lsp::Json::Arr && !wf->a.empty())
        if (r.empty()) r = lsp::str(&wf->a[0], "uri");
    if (!r.empty()) root = canon(lsp::uriToPath(r));
    settings.read(lsp::at(params, "initializationOptions"));
    if (auto pe = lsp::at(params, "capabilities.general.positionEncodings"); pe && pe->kind == lsp::Json::Arr)
        for (auto& e : pe->a)
            if (e.kind == lsp::Json::Str && e.s == "utf-8") enc_.utf8 = true;
    return Obj()
        .raw("capabilities",
             Obj()
                 .s("positionEncoding", enc_.utf8 ? "utf-8" : "utf-16")
                 .raw("textDocumentSync",
                      Obj().b("openClose", true).i("change", 1).raw("save", Obj().b("includeText", false).str()).str())
                 .b("definitionProvider", true)
                 .b("referencesProvider", true)
                 .b("hoverProvider", true)
                 .b("documentSymbolProvider", true)
                 .b("workspaceSymbolProvider", true)
                 .b("documentHighlightProvider", true)
                 .raw("completionProvider", Obj().raw("triggerCharacters", "[\"`\",\"$\"]").str())
                 .raw("executeCommandProvider", Obj().raw("commands", "[]").str())
                 .str())
        .raw("serverInfo", Obj().s("name", "mlsp").s("version", MINISIM_VERSION).str())
        .str();
}

void Server::handle(const lsp::Json& msg) {
    std::string method = lsp::str(&msg, "method");
    const lsp::Json* id = msg.get("id");
    const lsp::Json* params = msg.get("params");
    auto docPath = [&]() { return canon(lsp::uriToPath(lsp::str(params, "textDocument.uri"))); };
    auto line = [&]() { return (unsigned)lsp::num(params, "position.line"); };
    auto chr = [&]() { return (unsigned)lsp::num(params, "position.character"); };

    if (method.empty()) return;  // a response to something we sent
    if (shutdown_ && method != "exit") {
        replyError(id, -32600, "the server is shutting down");
        return;
    }
    if (method == "initialize") {
        reply(id, onInitialize(params));
        return;
    }
    if (method == "initialized") {
        // index in the background so the first analysis does not wait for it
        std::thread([this] { ensureIndex(); }).detach();
        return;
    }
    if (method == "shutdown") {
        shutdown_ = true;
        reply(id, "null");
        return;
    }
    if (method == "exit") {
        std::lock_guard<std::mutex> lk(m_);
        exit_ = true;
        cv_.notify_all();
        mlintCv_.notify_all();
        return;
    }
    if (method == "textDocument/didOpen") {
        std::string p = docPath();
        {
            std::lock_guard<std::mutex> lk(m_);
            auto& d = docs_[p];
            d.uri = lsp::str(params, "textDocument.uri");
            d.text = lsp::str(params, "textDocument.text");
            d.version = (int)lsp::num(params, "textDocument.version");
        }
        if (root.empty()) root = dirOf(p);
        schedule(p, 0);
        return;
    }
    if (method == "textDocument/didChange") {
        std::string p = docPath();
        auto ch = lsp::at(params, "contentChanges");
        if (ch && ch->kind == lsp::Json::Arr && !ch->a.empty()) {
            std::lock_guard<std::mutex> lk(m_);
            auto& d = docs_[p];
            d.text = lsp::str(&ch->a.back(), "text");
            d.version = (int)lsp::num(params, "textDocument.version", d.version + 1);
        }
        schedule(p, 300);
        return;
    }
    if (method == "textDocument/didSave") {
        std::string p = docPath();
        index_.update(p);
        std::vector<std::string> dependents;
        {
            std::lock_guard<std::mutex> lk(m_);
            // open files whose design includes this one see the change too
            for (auto& [r, a] : analyses_)
                if (r != p && docs_.count(r) && std::find(a->files.begin(), a->files.end(), p) != a->files.end())
                    dependents.push_back(r);
            mlintQueue_.push_back(p);
            mlintCv_.notify_all();
        }
        schedule(p, 0);
        for (auto& r : dependents) schedule(r, 200);
        return;
    }
    if (method == "textDocument/didClose") {
        std::string p = docPath();
        std::set<std::string> touched;
        {
            std::lock_guard<std::mutex> lk(m_);
            docs_.erase(p);
            queue_.erase(p);
            if (auto a = analyses_.find(p); a != analyses_.end()) {
                for (auto& [q, d] : a->second->diags) touched.insert(q);
                analyses_.erase(a);
            }
            if (auto m = mlintDiags_.find(p); m != mlintDiags_.end()) {
                for (auto& [q, d] : m->second) touched.insert(q);
                mlintDiags_.erase(m);
            }
            touched.insert(p);
        }
        publish(touched);
        return;
    }
    if (method == "workspace/didChangeWatchedFiles") {
        if (auto ch = lsp::at(params, "changes"); ch && ch->kind == lsp::Json::Arr)
            for (auto& c : ch->a) {
                std::string p = canon(lsp::uriToPath(lsp::str(&c, "uri")));
                if (lsp::num(&c, "type") == 3) index_.remove(p);
                else index_.update(p);
            }
        return;
    }
    if (method == "workspace/didChangeConfiguration") {
        const lsp::Json* s = lsp::at(params, "settings.minisim");
        if (!s) s = lsp::at(params, "settings");
        settings.read(s);
        std::vector<std::string> open;
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto& [p, d] : docs_) open.push_back(p);
        }
        for (auto& p : open) schedule(p, 0);
        return;
    }
    if (method == "textDocument/definition" || method == "textDocument/declaration") {
        reply(id, onDefinition(docPath(), line(), chr()));
        return;
    }
    if (method == "textDocument/references") {
        reply(id, onReferences(docPath(), line(), chr(), lsp::boolean(params, "context.includeDeclaration", true), false));
        return;
    }
    if (method == "textDocument/documentHighlight") {
        reply(id, onReferences(docPath(), line(), chr(), true, true));
        return;
    }
    if (method == "textDocument/hover") {
        reply(id, onHover(docPath(), line(), chr()));
        return;
    }
    if (method == "textDocument/documentSymbol") {
        reply(id, onDocumentSymbol(docPath()));
        return;
    }
    if (method == "workspace/symbol") {
        reply(id, onWorkspaceSymbol(lsp::str(params, "query")));
        return;
    }
    if (method == "textDocument/completion") {
        reply(id, onCompletion(docPath(), line(), chr()));
        return;
    }
    if (method == "minisim/compileInfo") {
        reply(id, onCompileInfo(docPath()));
        return;
    }
    if (method == "$/cancelRequest" || method == "$/setTrace" || method.rfind("$/", 0) == 0) return;
    replyError(id, -32601, "mlsp does not handle " + method);
}

void Server::run() {
    std::thread w([this] { worker(); });
    std::thread ml([this] { mlintWorker(); });
    std::string body;
    while (ch_.read(body)) {
        try {
            auto msg = vp::json::parse(body);
            handle(msg);
        }
        catch (const std::exception& e) {
            ch_.write(Obj().s("jsonrpc", "2.0").raw("id", "null")
                          .raw("error", Obj().i("code", -32700).s("message", e.what()).str()).str());
        }
        if (exit_) break;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        exit_ = true;
        cv_.notify_all();
        mlintCv_.notify_all();
    }
    w.join();
    ml.join();
}

int Server::check(const std::vector<std::string>& files, const std::vector<std::string>& extra) {
    settings.args = extra;
    if (root.empty()) root = files.empty() ? canon(".") : canon(dirOf(canon(files[0])));
    index_.build(root, settings.maxFiles, threads_);
    std::map<std::string, OpenDoc> none;
    int errors = 0;
    for (auto& f : files) {
        auto a = analyze(canon(f), none, index_.snapshot());
        std::cout << "== " << f << ": " << a->files.size() << " files, tops";
        for (auto& t : a->tops) std::cout << " " << t;
        std::cout << ", " << a->defs.size() << " symbols\n";
        for (auto& [p, v] : a->diags)
            for (auto& d : v) {
                auto j = vp::json::parse(d);
                long sev = lsp::num(&j, "severity");
                errors += sev == 1;
                std::cout << p << ":" << lsp::num(&j, "range.start.line") + 1 << ":"
                          << lsp::num(&j, "range.start.character") + 1 << ": "
                          << (sev == 1 ? "error" : sev == 2 ? "warning" : "note") << ": " << lsp::str(&j, "message")
                          << " [" << lsp::str(&j, "code") << "]\n";
            }
    }
    return errors ? 1 : 0;
}

void usage() {
    std::cout << "mlsp " << MINISIM_VERSION
              << " - minisim language server for SystemVerilog and Verilog (LSP 3.17 on stdio)\n\n"
                 "  mlsp                       serve an editor on stdin/stdout\n"
                 "  mlsp -check FILE... [-- OPTIONS]\n"
                 "                             analyze each file as the editor would, print the diagnostics\n\n"
                 "Options:\n"
                 "  -root DIR       the workspace (default: the client's root, or the first file's directory)\n"
                 "  -f FILE         use exactly the files of this -f list instead of following the design\n"
                 "  -threads N      threads for indexing (default: all cores)\n"
                 "  -version        print the version\n\n"
                 "Editor settings (initializationOptions, or workspace/didChangeConfiguration under \"minisim\"):\n"
                 "  args            front-end options, e.g. [\"+incdir+rtl/inc\", \"+define+SIM\"]\n"
                 "  fileList        a -f file; relative to the workspace\n"
                 "  binDir          where mlint is (default: next to mlsp, then PATH)\n"
                 "  uvmHome         the UVM library for import uvm_pkg::* (default $UVM_HOME)\n"
                 "  mlint.enable    run mlint on save (default true)\n"
                 "  mlint.args      extra mlint options, e.g. [\"-goal\", \"design\"]\n\n"
                 "See docs/LSP.md.\n";
}

}  // namespace

int main(int argc, char** argv) {
    // The protocol owns the real stdout; anything else that prints goes to
    // stderr, so a stray message cannot break a frame.
    int out = dup(1);
    unsigned threads = par::defaultThreads();
    std::vector<std::string> checkFiles, extra;
    bool check = false;
    std::string root, fileList;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        if (a == "-version" || a == "--version" || a == "-v") {
            std::cout << "mlsp " << MINISIM_VERSION << " (" << MINISIM_GIT << ")\n";
            return 0;
        }
        if (a == "--stdio") continue;  // what VS Code's client adds
        if (par::threadsArg(argc, argv, i, threads, "mlsp")) continue;
        if (a == "-root" && i + 1 < argc) { root = argv[++i]; continue; }
        if (a == "-f" && i + 1 < argc) { fileList = argv[++i]; continue; }
        if (a == "-check") { check = true; continue; }
        if (a == "--") {
            for (i++; i < argc; i++) extra.push_back(argv[i]);
            break;
        }
        if (check && a[0] != '-') { checkFiles.push_back(a); continue; }
        std::cerr << "mlsp: unknown option '" << a << "' (mlsp -help)\n";
        return 2;
    }
    std::string self;
    {
        std::error_code ec;
        auto p = fs::read_symlink("/proc/self/exe", ec);
        if (!ec) self = p.parent_path().string();
    }
    if (check) {
        lsp::Channel none(-1, -1);
        Server s(none, threads);
        s.selfDir = self;
        if (!root.empty()) s.root = canon(root);
        s.settings.fileList = fileList;
        return s.check(checkFiles, extra);
    }
    signal(SIGPIPE, SIG_IGN);
    dup2(2, 1);
    lsp::Channel ch(0, out);
    Server s(ch, threads);
    s.selfDir = self;
    if (!root.empty()) s.root = canon(root);
    s.settings.fileList = fileList;
    s.run();
    return 0;
}
