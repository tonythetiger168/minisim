// spef.hpp - SPEF (Standard Parasitic Exchange Format, IEEE 1481) reader and
// writer, shared by the tools that produce or use parasitics: mrcx writes it,
// timing and power analysis read it.
//
//   spef::File f = spef::read("design.spef");          // all logical cores
//   for (const spef::Net& n : f.nets) {
//       double farads = n.total * f.cScale;
//       for (const spef::Cap& c : n.caps) ...           // c.b empty: to ground
//       for (const spef::Res& r : n.res) ...
//   }
//   spef::write(out, f);
//
// Reading: the *NAME_MAP is applied and backslash escapes are removed, so
// names come out as they are in the netlist ("u1/u2:A", "bus[3]"). Values
// stay in the file's units; File::cScale, rScale, tScale and lScale turn them
// into farads, ohms, seconds and henries. Of a min:typ:max triplet one value
// is kept (typ unless asked otherwise). *D_NET sections are parsed in
// parallel; the result does not depend on the thread count. *R_NET (reduced)
// sections are skipped.
//
// Writing: spef::write(FILE*, File) writes any File. A tool that holds its
// own network (mrcx) writes nets itself with the same pieces: writeHeader,
// escape, appendNum.
#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "parallel.hpp"

namespace spef {

struct Header {
    std::string design, date, vendor, program, version;
    std::vector<std::string> flow;   // *DESIGN_FLOW strings
    char divider = '/', delimiter = ':', busOpen = '[', busClose = ']';
    double tUnit = 1, cUnit = 1, rUnit = 1, lUnit = 1;
    std::string tSuffix = "NS", cSuffix = "FF", rSuffix = "OHM", lSuffix = "HENRY";
};

struct Conn {
    std::string name;        // "inst:pin", or the port's name
    bool port = false;       // *P (true) or *I (false)
    char dir = 'B';          // I, O or B
    bool hasXY = false;
    double x = 0, y = 0;     // *C
    double load = -1;        // *L, -1 when absent
    std::string cell;        // *D
};
struct Cap { std::string a, b; double c = 0; };   // b empty: capacitance to ground
struct Res { std::string a, b; double r = 0; };
struct Net {
    std::string name;
    double total = 0;        // total capacitance, as the *D_NET line gives it
    std::vector<Conn> conns;
    std::vector<Cap> caps;
    std::vector<Res> res;
};
struct Port { std::string name; char dir = 'B'; };

struct File {
    Header h;
    double tScale = 1e-9, cScale = 1e-15, rScale = 1, lScale = 1;   // file unit -> SI
    std::vector<Port> ports;
    std::vector<Net> nets;
};

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// A netlist name as SPEF text: characters SPEF reserves get a backslash.
// A backslash already in the name (a DEF or Verilog escape) is kept with the
// character it protects.
inline std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) { o += c; o += s[++i]; continue; }
        bool plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     c == '_' || c == '/' || c == '[' || c == ']';
        if (!plain) o += '\\';
        o += c;
    }
    return o;
}
// Like escape, for "inst:pin" node names: the last ':' stays a delimiter.
inline std::string escapeNode(const std::string& s) {
    size_t k = s.rfind(':');
    if (k == std::string::npos) return escape(s);
    return escape(s.substr(0, k)) + ':' + escape(s.substr(k + 1));
}
inline void appendNum(std::string& o, double v) {   // six significant digits
    char tmp[40];
    auto r = std::to_chars(tmp, tmp + sizeof tmp, v, std::chars_format::general, 6);
    o.append(tmp, r.ptr);
}
inline void appendInt(std::string& o, long long v) {
    char tmp[24];
    auto r = std::to_chars(tmp, tmp + sizeof tmp, v);
    o.append(tmp, r.ptr);
}

inline void writeHeader(std::FILE* f, const Header& h) {
    std::fprintf(f, "*SPEF \"IEEE 1481-2009\"\n*DESIGN \"%s\"\n*DATE \"%s\"\n*VENDOR \"%s\"\n*PROGRAM \"%s\"\n*VERSION \"%s\"\n",
                 h.design.c_str(), h.date.c_str(), h.vendor.c_str(), h.program.c_str(), h.version.c_str());
    std::fputs("*DESIGN_FLOW", f);
    if (h.flow.empty()) std::fputs(" \"EXTRACTION\"", f);
    for (auto& s : h.flow) std::fprintf(f, " \"%s\"", s.c_str());
    std::string u;
    auto unit = [&](const char* key, double v, const std::string& suffix) {
        u += key; u += ' '; appendNum(u, v); u += ' '; u += suffix; u += '\n';
    };
    unit("*T_UNIT", h.tUnit, h.tSuffix);
    unit("*C_UNIT", h.cUnit, h.cSuffix);
    unit("*R_UNIT", h.rUnit, h.rSuffix);
    unit("*L_UNIT", h.lUnit, h.lSuffix);
    std::fprintf(f, "\n*DIVIDER %c\n*DELIMITER %c\n*BUS_DELIMITER %c %c\n%s\n", h.divider, h.delimiter, h.busOpen,
                 h.busClose, u.c_str());
}

inline void write(std::FILE* f, const File& file) {
    writeHeader(f, file.h);
    if (!file.ports.empty()) {
        std::fputs("*PORTS\n", f);
        for (auto& p : file.ports) std::fprintf(f, "%s %c\n", escape(p.name).c_str(), p.dir);
        std::fputc('\n', f);
    }
    std::string b;
    for (auto& n : file.nets) {
        b.clear();
        b += "*D_NET "; b += escape(n.name); b += ' '; appendNum(b, n.total); b += '\n';
        if (!n.conns.empty()) b += "*CONN\n";
        for (auto& c : n.conns) {
            b += c.port ? "*P " : "*I ";
            b += escapeNode(c.name); b += ' '; b += c.dir;
            if (c.hasXY) { b += " *C "; appendNum(b, c.x); b += ' '; appendNum(b, c.y); }
            if (c.load >= 0) { b += " *L "; appendNum(b, c.load); }
            if (!c.cell.empty()) { b += " *D "; b += escape(c.cell); }
            b += '\n';
        }
        if (!n.caps.empty()) b += "*CAP\n";
        long long k = 0;
        for (auto& c : n.caps) {
            appendInt(b, ++k); b += ' '; b += escapeNode(c.a);
            if (!c.b.empty()) { b += ' '; b += escapeNode(c.b); }
            b += ' '; appendNum(b, c.c); b += '\n';
        }
        if (!n.res.empty()) b += "*RES\n";
        k = 0;
        for (auto& r : n.res) {
            appendInt(b, ++k); b += ' '; b += escapeNode(r.a); b += ' '; b += escapeNode(r.b);
            b += ' '; appendNum(b, r.r); b += '\n';
        }
        b += "*END\n\n";
        std::fwrite(b.data(), 1, b.size(), f);
    }
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

namespace detail {

struct Scan {
    const char* p;
    const char* end;
    const std::string* file;
    const char* base;

    [[noreturn]] void fail(const std::string& m) const {
        size_t line = 1;
        for (const char* q = base; q < p && q < end; q++) line += *q == '\n';
        throw std::runtime_error(*file + ":" + std::to_string(line) + ": " + m);
    }
    void skip() {
        for (;;) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
            if (p + 1 < end && p[0] == '/' && p[1] == '/') {
                while (p < end && *p != '\n') p++;
                continue;
            }
            return;
        }
    }
    // The next token; empty at the end. A quoted string comes without quotes.
    std::string_view tok() {
        skip();
        if (p >= end) return {};
        if (*p == '"') {
            const char* s = ++p;
            while (p < end && *p != '"') p++;
            std::string_view t(s, static_cast<size_t>(p - s));
            if (p < end) p++;
            return t;
        }
        const char* s = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
            if (*p == '\\' && p + 1 < end) p++;
            p++;
        }
        return {s, static_cast<size_t>(p - s)};
    }
    std::string_view peek() {
        const char* save = p;
        std::string_view t = tok();
        p = save;
        return t;
    }
};

// A number, or the chosen one of "min:typ:max". False when t is not a number.
inline bool number(std::string_view t, int corner, double& v) {
    const char* s = t.data();
    const char* e = s + t.size();
    double vals[3];
    int n = 0;
    while (n < 3) {
        if (s < e && *s == '+') s++;
        auto r = std::from_chars(s, e, vals[n]);
        if (r.ec != std::errc()) return false;
        n++;
        s = r.ptr;
        if (s == e) break;
        if (*s != ':') return false;
        s++;
    }
    if (s != e) return false;
    v = n == 3 ? vals[corner] : vals[0];
    return true;
}

inline bool keyword(std::string_view t) {   // "*NAME", not "*12"
    return t.size() > 1 && t[0] == '*' && !(t[1] >= '0' && t[1] <= '9');
}

struct Names {
    std::unordered_map<unsigned long long, std::string> map;
    // Token to netlist name: "*12:A" -> "u1/u2:A", escapes removed.
    std::string resolve(std::string_view t) const {
        std::string o;
        size_t i = 0;
        if (t.size() > 1 && t[0] == '*' && t[1] >= '0' && t[1] <= '9') {
            unsigned long long id = 0;
            for (i = 1; i < t.size() && t[i] >= '0' && t[i] <= '9'; i++) id = id * 10 + static_cast<unsigned>(t[i] - '0');
            auto it = map.find(id);
            if (it != map.end()) o = it->second;
            else o.assign(t.data(), i);
        }
        for (; i < t.size(); i++) {
            if (t[i] == '\\' && i + 1 < t.size()) i++;
            o += t[i];
        }
        return o;
    }
};

inline char dirOf(std::string_view t) { return t.empty() ? 'B' : t[0]; }

// One "*D_NET ... *END" section; s.p is just past "*D_NET".
inline void parseNet(Scan& s, const Names& names, int corner, Net& n) {
    n.name = names.resolve(s.tok());
    if (!number(s.tok(), corner, n.total)) s.fail("*D_NET " + n.name + ": expected the total capacitance");
    enum { None, ConnS, CapS, ResS, Skip } sec = None;
    for (;;) {
        std::string_view t = s.tok();
        if (t.empty()) s.fail("*D_NET " + n.name + ": missing *END");
        if (t == "*END") return;
        if (t == "*CONN") { sec = ConnS; continue; }
        if (t == "*CAP") { sec = CapS; continue; }
        if (t == "*RES") { sec = ResS; continue; }
        if (t == "*INDUC") { sec = Skip; continue; }
        if (sec == ConnS) {
            if (t == "*P" || t == "*I") {
                Conn c;
                c.port = t == "*P";
                c.name = names.resolve(s.tok());
                c.dir = dirOf(s.tok());
                n.conns.push_back(std::move(c));
            } else if (t == "*N") {   // an internal node and its coordinates
                s.tok();
                if (s.peek() == "*C") { s.tok(); s.tok(); s.tok(); }
            } else if (t == "*C") {
                double x = 0, y = 0;
                if (!number(s.tok(), corner, x) || !number(s.tok(), corner, y)) s.fail("*C needs two numbers");
                if (!n.conns.empty()) { n.conns.back().hasXY = true; n.conns.back().x = x; n.conns.back().y = y; }
            } else if (t == "*L") {
                double v = 0;
                if (!number(s.tok(), corner, v)) s.fail("*L needs a number");
                if (!n.conns.empty()) n.conns.back().load = v;
            } else if (t == "*S") { s.tok(); s.tok(); }
            else if (t == "*D") {
                std::string cell = names.resolve(s.tok());
                if (!n.conns.empty()) n.conns.back().cell = std::move(cell);
            } else s.fail("*D_NET " + n.name + ": unexpected '" + std::string(t) + "' in *CONN");
        } else if (sec == CapS) {
            // id node value | id node node value
            Cap c;
            c.a = names.resolve(s.tok());
            std::string_view x = s.tok();
            if (!number(x, corner, c.c)) {
                c.b = names.resolve(x);
                if (!number(s.tok(), corner, c.c)) s.fail("*D_NET " + n.name + ": bad *CAP entry");
            }
            n.caps.push_back(std::move(c));
        } else if (sec == ResS) {
            Res r;
            r.a = names.resolve(s.tok());
            r.b = names.resolve(s.tok());
            if (!number(s.tok(), corner, r.r)) s.fail("*D_NET " + n.name + ": bad *RES entry");
            n.res.push_back(std::move(r));
        } else if (sec == Skip) {
            s.tok(); s.tok(); s.tok();
        } else s.fail("*D_NET " + n.name + ": unexpected '" + std::string(t) + "'");
    }
}

inline double unitScale(std::string_view suffix, const Scan& s) {
    std::string u(suffix);
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (u == "S" || u == "F" || u == "OHM" || u == "HENRY") return 1;
    if (u == "MS" || u == "MH") return 1e-3;
    if (u == "US" || u == "UF" || u == "UH") return 1e-6;
    if (u == "NS" || u == "NF") return 1e-9;
    if (u == "PS" || u == "PF") return 1e-12;
    if (u == "FS" || u == "FF") return 1e-15;
    if (u == "KOHM") return 1e3;
    s.fail("unknown unit '" + u + "'");
}

}  // namespace detail

// Parses SPEF text. corner picks from min:typ:max triplets: 0, 1 (typ) or 2.
// threads 0 means every logical core.
inline File parse(const std::string& text, const std::string& fname, int threads = 0, int corner = 1) {
    using namespace detail;
    File f;
    Names names;
    Scan s{text.data(), text.data() + text.size(), &fname, text.data()};
    const char* body = s.end;   // where the first net section starts
    auto unit = [&](double& v, std::string& suffix, double& scale) {
        if (!number(s.tok(), 1, v)) s.fail("expected a number");
        std::string_view u = s.tok();
        suffix = std::string(u);
        scale = v * unitScale(u, s);
    };
    for (;;) {
        s.skip();
        const char* at = s.p;
        std::string_view t = s.tok();
        if (t.empty()) break;
        if (t == "*D_NET" || t == "*R_NET") { body = at; break; }
        if (t == "*SPEF") s.tok();
        else if (t == "*DESIGN") f.h.design = std::string(s.tok());
        else if (t == "*DATE") f.h.date = std::string(s.tok());
        else if (t == "*VENDOR") f.h.vendor = std::string(s.tok());
        else if (t == "*PROGRAM") f.h.program = std::string(s.tok());
        else if (t == "*VERSION") f.h.version = std::string(s.tok());
        else if (t == "*DESIGN_FLOW") {
            for (;;) {
                s.skip();
                if (s.p >= s.end || *s.p != '"') break;
                f.h.flow.push_back(std::string(s.tok()));
            }
        }
        else if (t == "*DIVIDER") f.h.divider = dirOf(s.tok());
        else if (t == "*DELIMITER") f.h.delimiter = dirOf(s.tok());
        else if (t == "*BUS_DELIMITER") {
            std::string_view b = s.tok();
            f.h.busOpen = b.empty() ? '[' : b[0];
            if (b.size() > 1) f.h.busClose = b[1];
            else if (!keyword(s.peek())) f.h.busClose = dirOf(s.tok());
        }
        else if (t == "*T_UNIT") unit(f.h.tUnit, f.h.tSuffix, f.tScale);
        else if (t == "*C_UNIT") unit(f.h.cUnit, f.h.cSuffix, f.cScale);
        else if (t == "*R_UNIT") unit(f.h.rUnit, f.h.rSuffix, f.rScale);
        else if (t == "*L_UNIT") unit(f.h.lUnit, f.h.lSuffix, f.lScale);
        else if (t == "*NAME_MAP") {
            for (;;) {
                std::string_view k = s.peek();
                if (k.empty() || keyword(k)) break;
                s.tok();
                if (k[0] != '*') s.fail("*NAME_MAP: expected an index, got '" + std::string(k) + "'");
                unsigned long long id = 0;
                for (size_t i = 1; i < k.size(); i++) id = id * 10 + static_cast<unsigned>(k[i] - '0');
                Names none;
                names.map[id] = none.resolve(s.tok());
            }
        }
        else if (t == "*PORTS" || t == "*PHYSICAL_PORTS") {
            bool logical = t == "*PORTS";
            for (;;) {
                std::string_view k = s.peek();
                if (k.empty() || (keyword(k) && k != "*C" && k != "*L" && k != "*S" && k != "*D")) break;
                s.tok();
                if (k == "*C" || k == "*S") { s.tok(); s.tok(); continue; }
                if (k == "*L" || k == "*D") { s.tok(); continue; }
                Port p;
                p.name = names.resolve(k);
                p.dir = dirOf(s.tok());
                if (logical) f.ports.push_back(std::move(p));
            }
        }
        else if (t == "*POWER_NETS" || t == "*GROUND_NETS") {
            while (!s.peek().empty() && !keyword(s.peek())) s.tok();
        }
        else if (t == "*DEFINE" || t == "*PDEFINE") {
            // instance names, then the entity as a quoted string
            for (;;) {
                s.skip();
                if (s.p >= s.end) break;
                bool q = *s.p == '"';
                s.tok();
                if (q) break;
            }
        }
        else s.fail("unknown statement '" + std::string(t) + "'");
    }

    // Where each net section starts.
    std::vector<const char*> starts;
    for (const char* p = body; p < s.end;) {
        const char* q = static_cast<const char*>(std::memchr(p, '*', static_cast<size_t>(s.end - p)));
        if (!q) break;
        if (s.end - q >= 6 && (q == text.data() || q[-1] == '\n' || q[-1] == ' ' || q[-1] == '\t' || q[-1] == '\r') &&
            (std::memcmp(q, "*D_NET", 6) == 0 || std::memcmp(q, "*R_NET", 6) == 0))
            starts.push_back(q);
        p = q + 1;
    }
    f.nets.resize(starts.size());
    std::vector<char> keep(starts.size(), 1);
    unsigned nthreads = threads > 0 ? static_cast<unsigned>(threads) : par::defaultThreads();
    const size_t grain = 64;   // nets per block
    par::parallelFor((starts.size() + grain - 1) / grain, nthreads, [&](size_t blk) {
        for (size_t i = blk * grain; i < std::min(starts.size(), (blk + 1) * grain); i++) {
            Scan sc{starts[i] + 6, i + 1 < starts.size() ? starts[i + 1] : s.end, &fname, text.data()};
            if (starts[i][1] == 'R') { keep[i] = 0; continue; }
            parseNet(sc, names, corner, f.nets[i]);
        }
    });
    size_t o = 0;
    for (size_t i = 0; i < f.nets.size(); i++)
        if (keep[i]) {
            if (o != i) f.nets[o] = std::move(f.nets[i]);
            o++;
        }
    f.nets.resize(o);
    return f;
}

inline File read(const std::string& path, int threads = 0, int corner = 1) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string text;
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    in.seekg(0);
    if (size > 0) {
        text.resize(static_cast<size_t>(size));
        in.read(text.data(), size);
    } else {
        std::stringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    return parse(text, path, threads, corner);
}

}  // namespace spef
