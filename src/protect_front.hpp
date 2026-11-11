// protect_front.hpp - protected IP on the slang front end, shared by msim,
// mlint and mdft: decrypt envelopes before slang reads the files (IEEE
// 1800-2023 clause 34, IEEE 1735-2023), keep diagnostics that point into
// protected code from showing it, and decide which protected objects a tool
// may show (1735 clause 9: viewport / interface_viewport pragmas and rights).
//
// Rights minisim reads from its tool block (`pragma protect control ...):
//   decryption = "allow" | "deny"            deny: the tools refuse the IP
//   visibility = "none" | "interface" | "full"
//       none (default): only viewport objects are visible; interface: also
//       the ports of protected modules; full: everything, including
//       diagnostics, waveforms, coverage and lint findings
//   coverage   = "off" | "on"                line / branch coverage inside
//                                            the protected code (default off)
//   min_version = "<x.y.z>"                  oldest minisim that may open it
#pragma once

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "slang/ast/Compilation.h"
#include "slang/ast/Symbol.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/diagnostics/DiagnosticClient.h"
#include "slang/diagnostics/DiagnosticEngine.h"
#include "slang/diagnostics/TextDiagnosticClient.h"
#include "slang/driver/Driver.h"
#include "slang/text/SourceManager.h"
#include "version.hpp"
#ifdef MINISIM_HAVE_OPENSSL
#include "licproxy.hpp"
#include "protect.hpp"
#endif

namespace protfront {

// Command-line options for protected IP.
struct Options {
    std::vector<std::string> keyfiles;   // -keyfile [[owner:]name=]priv.pem
    std::vector<std::string> keyrings;   // -keyring file
    bool licenseStop = false;            // -license_fail stop: no proxy connection is an error (1735 8.9)
    std::vector<std::string> dirs;       // +incdir+ / -I / -y directories

    // Takes the protect options out of a command line; returns true when
    // argv[i] was one (i then points at its last word).
    bool take(int argc, char** argv, int& i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << a << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-keyfile") { keyfiles.push_back(next()); return true; }
        if (a == "-keyring") { keyrings.push_back(next()); return true; }
        if (a == "-license") {
            // The old test stand-in let the IP user grant a licensed right to
            // themselves; only the IP author's proxy can answer (1735 clause 8).
            std::cerr << "-license is gone: licensed rights are checked with the IP author's license proxy.\n"
                         "Set the environment variable the IP names (license_proxyenv) to <port>@<host>; see docs/PROTECT.md.\n";
            std::exit(2);
        }
        if (a == "-license_fail") {
            std::string v = next();
            if (v != "stop" && v != "deny") { std::cerr << "-license_fail takes stop or deny\n"; std::exit(2); }
            licenseStop = v == "stop";
            return true;
        }
        // Directories that may hold protected `include files or library cells
        // (the option itself still goes to the front end).
        if (a.rfind("+incdir+", 0) == 0) {
            for (size_t p = 8; p < a.size();) {
                size_t e = a.find('+', p);
                if (e == std::string::npos) e = a.size();
                if (e > p) dirs.push_back(a.substr(p, e - p));
                p = e + 1;
            }
        } else if ((a == "-I" || a == "-y" || a == "--libdir" || a == "--include-directory") && i + 1 < argc) {
            dirs.push_back(argv[i + 1]);
        } else if (a.size() > 2 && (a.rfind("-I", 0) == 0 || a.rfind("-y", 0) == 0)) {
            dirs.push_back(a.substr(2));
        }
        return false;
    }
    static const char* help() {
        return "  -keyfile [[<owner>:]<keyname>=]<private.pem>  key for protected envelopes (repeatable)\n"
               "  -keyring <file>  private keys, one `<key_keyowner> <key_keyname> <private.pem>` per line\n"
               "  -license_fail stop|deny  when a protected IP's license proxy cannot be reached: stop, or go on\n"
               "                   with its licensed rights denied (default; IEEE 1735 8.9)\n";
    }
};

#ifdef MINISIM_HAVE_OPENSSL
using Envelope = protect::Envelope;
#else
struct Envelope {
    size_t begin = 0, end = 0;
    std::string right(const std::string&, const std::string& def = "") const { return def; }
};
#endif

// Protected code: for each file (canonical path), the envelopes decrypted
// from it, as byte ranges of the buffer slang reads.
struct ProtectedCode {
    std::map<std::string, std::vector<Envelope>> files;
    const slang::SourceManager* sm = nullptr;
    bool any() const { return !files.empty(); }

    const Envelope* envelopeAt(slang::SourceLocation loc) const {
        if (!sm || files.empty() || !loc.valid() || loc.buffer() == slang::SourceLocation::NoLocation.buffer())
            return nullptr;
        loc = sm->getFullyOriginalLoc(loc);
        if (!loc.buffer().valid() || !sm->isFileLoc(loc)) return nullptr;
        auto it = files.find(sm->getFullPath(loc.buffer()).string());
        if (it == files.end()) return nullptr;
        for (auto& e : it->second)
            if (loc.offset() >= e.begin && loc.offset() < e.end) return &e;
        return nullptr;
    }
    bool contains(slang::SourceLocation loc) const { return envelopeAt(loc) != nullptr; }
    // Protected, and the tool block does not grant visibility = "full".
    bool hidden(slang::SourceLocation loc) const {
        const Envelope* e = envelopeAt(loc);
        return e && e->right("visibility", "none") != "full";
    }
    // Line / branch coverage of the code at loc may be recorded.
    bool coverable(slang::SourceLocation loc) const {
        const Envelope* e = envelopeAt(loc);
        return !e || e->right("visibility", "none") == "full" || e->right("coverage", "off") == "on";
    }

    // May a tool show this object (a net or variable)? Outside protected code
    // always; inside, when the rights or a viewport make it visible (1735 9.5).
    bool visible(const slang::ast::Symbol& s) const {
        const Envelope* e = envelopeAt(s.location);
        if (!e) return true;
        std::string vis = e->right("visibility", "none");
        if (vis == "full") return true;
        const slang::ast::InstanceBodySymbol* body = nullptr;
        std::vector<std::string> path{std::string(s.name)};
        for (const slang::ast::Scope* sc = s.getParentScope(); sc; sc = sc->asSymbol().getParentScope()) {
            const slang::ast::Symbol& ss = sc->asSymbol();
            if (ss.kind == slang::ast::SymbolKind::InstanceBody) { body = &ss.as<slang::ast::InstanceBodySymbol>(); break; }
            if (ss.kind == slang::ast::SymbolKind::CompilationUnit || ss.kind == slang::ast::SymbolKind::Root) break;
            if (!ss.name.empty()) path.insert(path.begin(), std::string(ss.name));
        }
        std::string unit = body ? std::string(body->getDefinition().name) : "";
        bool port = false;
        if (body)
            for (auto* p : body->getPortList())
                if (p->kind == slang::ast::SymbolKind::Port && p->as<slang::ast::PortSymbol>().internalSymbol == &s) port = true;
        if (vis == "interface" && port) return true;
#ifdef MINISIM_HAVE_OPENSSL
        auto off = sm->getFullyOriginalLoc(s.location).offset();
        std::string rel, abs = unit;
        for (auto& p : path) { rel += (rel.empty() ? "" : ".") + p; abs += "." + p; }
        for (auto& v : e->viewports) {
            if (v.name.empty()) {
                if (off < v.targetBegin || off >= v.targetEnd) continue;
                if (!v.iface || port) return true;
                continue;
            }
            if (v.at > off) continue;   // a viewport applies to the code after it (9.6.1)
            if (v.iface) { if (port && v.name == unit) return true; continue; }
            if (v.name == unit || v.name == abs || v.name == rel) return true;
        }
#endif
        return false;
    }
};

// Passes diagnostics to the inner client, except those that point into
// hidden protected code: they are reported by severity and file only.
struct RedactingClient : slang::DiagnosticClient {
    std::shared_ptr<slang::DiagnosticClient> inner;
    const ProtectedCode& pc;
    RedactingClient(std::shared_ptr<slang::DiagnosticClient> in, const ProtectedCode& p) : inner(std::move(in)), pc(p) {}
    bool touches(const slang::ReportedDiagnosticInfo& d) const {
        if (pc.hidden(d.location)) return true;
        for (auto l : d.expansionLocs) if (pc.hidden(l)) return true;
        for (auto& r : d.ranges) if (pc.hidden(r.start())) return true;
        return false;
    }
    void report(const slang::ReportedDiagnostic& d) override {
        bool hide = touches(d);
        for (auto& n : d.notes) hide = hide || touches(n);
        if (!hide) { inner->report(d); return; }
        const char* sev = d.severity == slang::DiagnosticSeverity::Error || d.severity == slang::DiagnosticSeverity::Fatal
                              ? "error" : d.severity == slang::DiagnosticSeverity::Warning ? "warning" : "note";
        std::string file = "protected code";
        if (pc.sm && d.location.valid()) {
            auto loc = pc.sm->getFullyOriginalLoc(d.location);
            if (loc.buffer().valid()) file = std::string(pc.sm->getFileName(loc));
        }
        std::cerr << file << ": " << sev << ": in protected code (details hidden)\n";
    }
};

inline bool versionAtLeast(const std::string& have, const std::string& want) {
    auto parts = [](const std::string& v) {
        std::vector<long> r;
        size_t p = 0;
        while (p < v.size()) {
            size_t e = v.find('.', p);
            if (e == std::string::npos) e = v.size();
            try { r.push_back(std::stol(v.substr(p, e - p))); } catch (...) { r.push_back(0); }
            p = e + 1;
        }
        return r;
    };
    auto a = parts(have), b = parts(want);
    a.resize(std::max(a.size(), b.size()));
    b.resize(a.size());
    return a >= b;
}

#ifdef MINISIM_HAVE_OPENSSL
// The rights minisim reads (see the top of this file): refuse what they deny.
inline void checkRights(const protect::Decrypted& d, const std::string& shown, const protect::DecryptOptions& dopt) {
    for (auto& e : d.envs) {
        if (e.right("decryption", "allow") == "deny")
            throw protect::Error(shown + ": the IP's rights do not allow decryption by minisim");
        std::string mv = e.right("min_version");
        if (!mv.empty() && !versionAtLeast(MINISIM_VERSION, mv))
            throw protect::Error(shown + ": the IP needs minisim " + mv + " or later (this is " + MINISIM_VERSION + ")");
        static const std::set<std::string> known{"decryption", "visibility", "coverage", "min_version"};
        for (auto& [k, v] : e.rights)
            if (!known.count(k) && dopt.warn) dopt.warn(shown + ": right '" + k + "' is not a minisim right; ignored");
        std::string vis = e.right("visibility", "none");
        if (vis != "none" && vis != "interface" && vis != "full")
            throw protect::Error(shown + ": visibility must be \"none\", \"interface\" or \"full\", not \"" + vis + "\"");
        std::string cov = e.right("coverage", "off");
        if (cov != "off" && cov != "on")
            throw protect::Error(shown + ": coverage must be \"off\" or \"on\", not \"" + cov + "\"");
    }
}
#endif

// Decrypt every source the front end may read that holds an envelope, and
// hand slang the cleartext under the file's own path so it never reads the
// envelope itself. Looks at the source files, plus files with a Verilog
// extension in their directories and in the +incdir+ / -I / -y directories,
// so protected `include files and library cells are found too; an envelope in
// one of those extra files that does not open is left for slang to report
// only if the design reads that file.
inline bool preload(slang::driver::Driver& driver, const Options& o, ProtectedCode& pc, const char* tool) {
    namespace fs = std::filesystem;
    std::set<std::string> listed, files;
    std::set<std::string> searchDirs(o.dirs.begin(), o.dirs.end());
    for (auto& p : driver.sourceLoader.getFilePaths()) {
        std::error_code ec;
        auto c = fs::weakly_canonical(p, ec);
        if (ec) continue;
        listed.insert(c.string());
        files.insert(c.string());
        searchDirs.insert(c.parent_path().string());
    }
    static const std::set<std::string> exts{".v", ".sv", ".vh", ".svh", ".vp", ".svp", ".inc", ".svi", ".vg"};
    for (auto& d : searchDirs) {
        std::error_code ec;
        for (auto& e : fs::directory_iterator(d, ec)) {
            std::error_code ec2;
            if (!e.is_regular_file(ec2) || !exts.count(e.path().extension().string())) continue;
            auto c = fs::weakly_canonical(e.path(), ec2);
            if (!ec2) files.insert(c.string());
        }
    }
    std::vector<std::pair<std::string, std::string>> found;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find("begin_protected") != std::string::npos) found.emplace_back(f, std::move(text));
    }
    if (found.empty()) return true;
#ifdef MINISIM_HAVE_OPENSSL
    try {
        protect::Keyring ring;
        for (auto& k : o.keyrings) ring.loadFile(k);
        for (auto& k : o.keyfiles) ring.add(k);
        protect::DecryptOptions dopt;
        dopt.warn = [tool](const std::string& m) { std::cerr << tool << ": warning: " << m << "\n"; };
        // The license system (1735 clause 8): one client for the run, so a
        // proxy connection is shared by the envelopes that name it (8.3) and
        // a repeated call is answered from the first reply (8.6).
        licproxy::Client proxy;
        proxy.info = [tool](const std::string& m) { std::cerr << tool << ": license proxy: " << m << "\n"; };
        std::set<std::string> reported;
        dopt.proxy = [&](const protect::LicenseSpec& spec, const std::string& arg) -> protect::LicenseValue {
            licproxy::Reply r = proxy.check(licproxy::Spec{spec.env, spec.certDer}, arg);
            if (r.irregular) {
                // 8.9: stop, or go on as if the call returned false / "".
                if (o.licenseStop) throw protect::Error("license check failed: " + r.why);
                if (reported.insert(r.why).second)
                    std::cerr << tool << ": warning: license check failed (" << r.why
                              << "); licensed rights are denied\n";
            }
            return {r.granted, r.str};
        };
        for (auto& [path, text] : found) {
            std::string shown = fs::proximate(path).string();
            protect::Decrypted d;
            try {
                d = protect::decryptSource(text, ring, shown, dopt);
                checkRights(d, shown, dopt);
            } catch (const protect::Error&) {
                OPENSSL_cleanse(d.text.data(), d.text.size());
                if (listed.count(path)) throw;
                continue;   // not on the command line: only an error if the design uses it
            }
            if (d.envelopes == 0) continue;   // "begin_protected" only in a comment
            driver.sourceManager.assignText(path, d.text);
            OPENSSL_cleanse(d.text.data(), d.text.size());
            pc.files[path] = std::move(d.envs);
        }
    } catch (const protect::Error& ex) {
        std::cerr << tool << ": " << ex.what() << "\n";
        return false;
    }
    pc.sm = &driver.sourceManager;
    return true;
#else
    (void)o;
    for (auto& [path, text] : found)
        if (listed.count(path)) {
            std::cerr << tool << ": " << path << " has a protected envelope, and this " << tool << " was built without OpenSSL\n";
            return false;
        }
    return true;
#endif
}

// After preload: route the driver's diagnostics through the redacting client.
inline void redact(slang::driver::Driver& driver, const ProtectedCode& pc) {
    if (!pc.any()) return;
    driver.diagEngine.removeClient(driver.textDiagClient);
    driver.diagEngine.addClient(std::make_shared<RedactingClient>(driver.textDiagClient, pc));
}

}  // namespace protfront
