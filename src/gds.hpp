// gds.hpp - GDSII stream reader and writer, shared by the physical tools
// (mdrc, mlvs, and anything that needs to read or write a layout).
//
// The model is the GDSII one: a library of cells; a cell holds polygons,
// paths, text labels and references to other cells (single or arrayed).
// Coordinates are integers in database units; Library::dbu is the size of one
// database unit in microns (0.001 for a 1 nm grid).
//
//   gds::Library lib;
//   std::string err;
//   if (!gds::read("chip.gds", lib, err)) ...
//   gds::write("out.gds", lib, err);
//
// BOX elements are read as polygons. NODE elements and properties are
// skipped. Header-only, no dependencies.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace gds {

struct Point {
    int32_t x = 0, y = 0;
};

struct Polygon {
    int16_t layer = 0, datatype = 0;
    std::vector<Point> pts;   // not closed: the last point joins the first
};

struct Path {
    int16_t layer = 0, datatype = 0;
    int16_t pathtype = 0;     // 0 flush, 1 round, 2 half-width extension, 4 custom
    int32_t width = 0;
    int32_t bgnextn = 0, endextn = 0;   // pathtype 4 only
    std::vector<Point> pts;
};

struct Text {
    int16_t layer = 0, texttype = 0;
    Point at;
    std::string str;
};

// A reference to another cell. The transform is: mirror about the x axis
// (if mirror), then magnify, then rotate by angle degrees counter-clockwise,
// then move to origin. cols/rows > 1 make it an array with the given steps.
struct Ref {
    std::string cell;
    Point origin;
    bool mirror = false;
    double mag = 1.0;
    double angle = 0.0;
    int32_t cols = 1, rows = 1;
    Point colStep, rowStep;
};

struct Cell {
    std::string name;
    std::vector<Polygon> polygons;
    std::vector<Path> paths;
    std::vector<Text> texts;
    std::vector<Ref> refs;
};

struct Library {
    std::string name = "LIB";
    double dbu = 0.001;       // microns per database unit
    double userUnit = 0.001;  // user units per database unit
    std::vector<Cell> cells;

    const Cell* find(const std::string& n) const {
        for (const Cell& c : cells)
            if (c.name == n) return &c;
        return nullptr;
    }
    // Cells that no other cell references, in file order.
    std::vector<std::string> topCells() const {
        std::unordered_map<std::string, bool> used;
        for (const Cell& c : cells)
            for (const Ref& r : c.refs) used[r.cell] = true;
        std::vector<std::string> out;
        for (const Cell& c : cells)
            if (!used.count(c.name)) out.push_back(c.name);
        return out;
    }
};

namespace detail {

enum : uint8_t {
    HEADER = 0x00, BGNLIB = 0x01, LIBNAME = 0x02, UNITS = 0x03, ENDLIB = 0x04,
    BGNSTR = 0x05, STRNAME = 0x06, ENDSTR = 0x07, BOUNDARY = 0x08, PATH = 0x09,
    SREF = 0x0A, AREF = 0x0B, TEXT = 0x0C, LAYER = 0x0D, DATATYPE = 0x0E,
    WIDTH = 0x0F, XY = 0x10, ENDEL = 0x11, SNAME = 0x12, COLROW = 0x13,
    NODE = 0x15, TEXTTYPE = 0x16, PRESENTATION = 0x17, STRING = 0x19,
    STRANS = 0x1A, MAG = 0x1B, ANGLE = 0x1C, PATHTYPE = 0x21, BOX = 0x2D,
    BOXTYPE = 0x2E, BGNEXTN = 0x30, ENDEXTN = 0x31
};

inline int16_t be16(const uint8_t* p) { return int16_t((p[0] << 8) | p[1]); }
inline int32_t be32(const uint8_t* p) {
    return int32_t((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
}
// GDSII 8-byte real: sign bit, 7-bit excess-64 base-16 exponent, 56-bit mantissa.
inline double real8(const uint8_t* p) {
    uint64_t m = 0;
    for (int i = 1; i < 8; ++i) m = (m << 8) | p[i];
    if (!m) return 0.0;
    double v = std::ldexp(double(m), 4 * (int(p[0] & 0x7f) - 64) - 56);
    return (p[0] & 0x80) ? -v : v;
}
inline void putReal8(std::string& o, double v) {
    uint8_t b[8] = {0};
    if (v != 0.0) {
        bool neg = v < 0;
        if (neg) v = -v;
        int e = 64;
        while (v >= 1.0) { v /= 16.0; ++e; }
        while (v < 1.0 / 16.0) { v *= 16.0; --e; }
        uint64_t m = uint64_t(std::ldexp(v, 56) + 0.5);
        if (m >> 56) { m >>= 4; ++e; }
        b[0] = uint8_t(e) | (neg ? 0x80 : 0);
        for (int i = 7; i >= 1; --i) { b[i] = uint8_t(m); m >>= 8; }
    }
    o.append(reinterpret_cast<char*>(b), 8);
}

struct Writer {
    std::string o;
    void rec(uint8_t type, uint8_t dt, size_t bytes) {
        size_t n = bytes + 4;
        o.push_back(char(n >> 8)); o.push_back(char(n));
        o.push_back(char(type)); o.push_back(char(dt));
    }
    void i16(int v) { o.push_back(char(v >> 8)); o.push_back(char(v)); }
    void i32(int32_t v) {
        o.push_back(char(v >> 24)); o.push_back(char(v >> 16));
        o.push_back(char(v >> 8)); o.push_back(char(v));
    }
    void none(uint8_t t) { rec(t, 0, 0); }
    void s16(uint8_t t, int v) { rec(t, 2, 2); i16(v); }
    void s32(uint8_t t, int32_t v) { rec(t, 3, 4); i32(v); }
    void str(uint8_t t, const std::string& s) {
        size_t n = s.size() + (s.size() & 1);
        rec(t, 6, n);
        o += s;
        if (s.size() & 1) o.push_back('\0');
    }
    void real(uint8_t t, double a) { rec(t, 5, 8); putReal8(o, a); }
    void xy(const std::vector<Point>& p, bool close) {
        // One XY record holds at most 8191 points (16-bit record length).
        size_t n = p.size() + (close ? 1 : 0);
        if (n > 8191) n = 8191;
        rec(XY, 3, n * 8);
        size_t body = close ? n - 1 : n;
        for (size_t i = 0; i < body; ++i) { i32(p[i].x); i32(p[i].y); }
        if (close) { i32(p[0].x); i32(p[0].y); }
    }
    void strans(const Ref& r) {
        if (!r.mirror && r.mag == 1.0 && r.angle == 0.0) return;
        rec(STRANS, 1, 2);   // a bit array, not an integer
        i16(r.mirror ? 0x8000 : 0);
        if (r.mag != 1.0) real(MAG, r.mag);
        if (r.angle != 0.0) real(ANGLE, r.angle);
    }
};

}  // namespace detail

// Parses a GDSII stream held in memory.
inline bool parse(const uint8_t* d, size_t n, Library& lib, std::string& err) {
    using namespace detail;
    lib = Library();
    Cell* cell = nullptr;
    enum { None, EPoly, EPath, ERef, EText, ESkip } el = None;
    Polygon poly; Path path; Ref ref; Text text;
    bool isAref = false, sawLib = false;
    size_t pos = 0;
    auto fail = [&](const std::string& m) {
        err = m + " at byte " + std::to_string(pos);
        return false;
    };
    while (pos + 4 <= n) {
        size_t len = (size_t(d[pos]) << 8) | d[pos + 1];
        uint8_t type = d[pos + 2];
        if (len < 4) {
            if (len == 0) break;   // null padding after ENDLIB
            return fail("bad record length");
        }
        if (pos + len > n) return fail("truncated record");
        const uint8_t* p = d + pos + 4;
        size_t bytes = len - 4;
        auto str = [&] {
            std::string s(reinterpret_cast<const char*>(p), bytes);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            return s;
        };
        auto pts = [&](std::vector<Point>& out) {
            out.resize(bytes / 8);
            for (size_t i = 0; i < out.size(); ++i) out[i] = {be32(p + 8 * i), be32(p + 8 * i + 4)};
        };
        switch (type) {
        case HEADER: sawLib = true; break;
        case LIBNAME: lib.name = str(); break;
        case UNITS:
            if (bytes < 16) return fail("short UNITS record");
            lib.userUnit = real8(p);
            lib.dbu = real8(p + 8) * 1e6;
            break;
        case BGNSTR:
            lib.cells.emplace_back();
            cell = &lib.cells.back();
            break;
        case STRNAME:
            if (!cell) return fail("STRNAME outside a structure");
            cell->name = str();
            break;
        case ENDSTR: cell = nullptr; break;
        case BOUNDARY: case BOX: el = EPoly; poly = Polygon(); break;
        case PATH: el = EPath; path = Path(); break;
        case SREF: el = ERef; ref = Ref(); isAref = false; break;
        case AREF: el = ERef; ref = Ref(); isAref = true; break;
        case TEXT: el = EText; text = Text(); break;
        case NODE: el = ESkip; break;
        case LAYER:
            if (bytes < 2) return fail("short LAYER record");
            poly.layer = path.layer = text.layer = be16(p);
            break;
        case DATATYPE: case BOXTYPE:
            if (bytes < 2) return fail("short DATATYPE record");
            poly.datatype = path.datatype = be16(p);
            break;
        case TEXTTYPE: if (bytes >= 2) text.texttype = be16(p); break;
        case PATHTYPE: if (bytes >= 2) path.pathtype = be16(p); break;
        case WIDTH: if (bytes >= 4) path.width = be32(p); break;
        case BGNEXTN: if (bytes >= 4) path.bgnextn = be32(p); break;
        case ENDEXTN: if (bytes >= 4) path.endextn = be32(p); break;
        case SNAME: ref.cell = str(); break;
        case STRANS:
            if (bytes >= 2 && el == ERef) ref.mirror = (p[0] & 0x80) != 0;
            break;
        case MAG: if (bytes >= 8 && el == ERef) ref.mag = real8(p); break;
        case ANGLE: if (bytes >= 8 && el == ERef) ref.angle = real8(p); break;
        case COLROW:
            if (bytes < 4) return fail("short COLROW record");
            ref.cols = be16(p); ref.rows = be16(p + 2);
            break;
        case STRING: text.str = str(); break;
        case XY: {
            if (el == EPoly) {
                pts(poly.pts);
                if (poly.pts.size() > 1 && poly.pts.front().x == poly.pts.back().x &&
                    poly.pts.front().y == poly.pts.back().y)
                    poly.pts.pop_back();
            } else if (el == EPath) {
                pts(path.pts);
            } else if (el == EText) {
                if (bytes >= 8) text.at = {be32(p), be32(p + 4)};
            } else if (el == ERef) {
                if (bytes < (isAref ? 24u : 8u)) return fail("short XY record in a reference");
                ref.origin = {be32(p), be32(p + 4)};
                if (isAref) {
                    // The second and third points are the far ends of the
                    // column and row axes: origin + cols*colStep, origin + rows*rowStep.
                    int c = ref.cols > 0 ? ref.cols : 1, r = ref.rows > 0 ? ref.rows : 1;
                    ref.colStep = {(be32(p + 8) - ref.origin.x) / c, (be32(p + 12) - ref.origin.y) / c};
                    ref.rowStep = {(be32(p + 16) - ref.origin.x) / r, (be32(p + 20) - ref.origin.y) / r};
                }
            }
            break;
        }
        case ENDEL:
            if (!cell) return fail("element outside a structure");
            if (el == EPoly) cell->polygons.push_back(std::move(poly));
            else if (el == EPath) cell->paths.push_back(std::move(path));
            else if (el == ERef) cell->refs.push_back(std::move(ref));
            else if (el == EText) cell->texts.push_back(std::move(text));
            el = None;
            break;
        case ENDLIB:
            return true;
        default: break;   // properties, attributes, and records with no geometry
        }
        pos += len;
    }
    if (!sawLib) return fail("not a GDSII stream");
    return fail("missing ENDLIB");
}

inline bool readFile(const std::string& file, std::string& data, std::string& err) {
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) { err = "cannot open " + file; return false; }
    char buf[1 << 16];
    size_t k;
    while ((k = std::fread(buf, 1, sizeof buf, f)) > 0) data.append(buf, k);
    std::fclose(f);
    return true;
}

inline bool read(const std::string& file, Library& lib, std::string& err) {
    std::string data;
    if (!readFile(file, data, err)) return false;
    if (!parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), lib, err)) {
        err = file + ": " + err;
        return false;
    }
    return true;
}

// Serializes a library. The timestamps are fixed so that the same library
// always gives the same bytes.
inline std::string serialize(const Library& lib) {
    using namespace detail;
    Writer w;
    auto stamp = [&](uint8_t t) {
        w.rec(t, 2, 24);
        for (int i = 0; i < 2; ++i) { w.i16(2026); w.i16(1); w.i16(1); w.i16(0); w.i16(0); w.i16(0); }
    };
    w.s16(HEADER, 600);
    stamp(BGNLIB);
    w.str(LIBNAME, lib.name);
    w.rec(UNITS, 5, 16);
    putReal8(w.o, lib.userUnit);
    putReal8(w.o, lib.dbu * 1e-6);
    for (const Cell& c : lib.cells) {
        stamp(BGNSTR);
        w.str(STRNAME, c.name);
        for (const Polygon& p : c.polygons) {
            if (p.pts.size() < 3) continue;
            w.none(BOUNDARY);
            w.s16(LAYER, p.layer);
            w.s16(DATATYPE, p.datatype);
            w.xy(p.pts, true);
            w.none(ENDEL);
        }
        for (const Path& p : c.paths) {
            if (p.pts.size() < 2) continue;
            w.none(PATH);
            w.s16(LAYER, p.layer);
            w.s16(DATATYPE, p.datatype);
            if (p.pathtype) w.s16(PATHTYPE, p.pathtype);
            w.s32(WIDTH, p.width);
            if (p.pathtype == 4) { w.s32(BGNEXTN, p.bgnextn); w.s32(ENDEXTN, p.endextn); }
            w.xy(p.pts, false);
            w.none(ENDEL);
        }
        for (const Ref& r : c.refs) {
            bool arr = r.cols > 1 || r.rows > 1;
            w.none(arr ? AREF : SREF);
            w.str(SNAME, r.cell);
            w.strans(r);
            if (arr) {
                w.rec(COLROW, 2, 4); w.i16(r.cols); w.i16(r.rows);
                std::vector<Point> p = {r.origin,
                    {r.origin.x + r.cols * r.colStep.x, r.origin.y + r.cols * r.colStep.y},
                    {r.origin.x + r.rows * r.rowStep.x, r.origin.y + r.rows * r.rowStep.y}};
                w.xy(p, false);
            } else {
                w.xy({r.origin}, false);
            }
            w.none(ENDEL);
        }
        for (const Text& t : c.texts) {
            w.none(TEXT);
            w.s16(LAYER, t.layer);
            w.s16(TEXTTYPE, t.texttype);
            w.xy({t.at}, false);
            w.str(STRING, t.str);
            w.none(ENDEL);
        }
        w.none(ENDSTR);
    }
    w.none(ENDLIB);
    return std::move(w.o);
}

inline bool write(const std::string& file, const Library& lib, std::string& err) {
    std::string s = serialize(lib);
    FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) { err = "cannot write " + file; return false; }
    bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) err = "write failed: " + file;
    return ok;
}

}  // namespace gds
