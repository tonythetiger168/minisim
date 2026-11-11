// oasis.hpp - OASIS (SEMI P39) reader for the physical tools.
//
// An OASIS file is read into the same model as GDSII (gds::Library):
//
//   gds::Library lib;
//   std::string err;
//   if (!oasis::read("chip.oas", lib, err)) ...
//   if (!oasis::readLayout("chip.oas or chip.gds", lib, err)) ...   // by content
//
// What is read: cells by name or reference number (names may come after
// their use), RECTANGLE, POLYGON, PATH, TRAPEZOID, CTRAPEZOID, CIRCLE, TEXT
// and PLACEMENT records with all modal variables, absolute and relative xy
// mode, every repetition and point-list type, and compressed blocks
// (CBLOCK, DEFLATE; the decoder is in this file). Properties, layer names
// and extension records are parsed and dropped.
//
// A repetition of a shape or text becomes one element per position. A
// regular repetition of a placement becomes an arrayed gds::Ref; an
// irregular one, one Ref per position. A circle becomes a 64-sided polygon.
// Header-only, no dependencies.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include "gds.hpp"

namespace oasis {

inline bool isOasis(const std::string& data) { return data.compare(0, 13, "%SEMI-OASIS\r\n") == 0; }

namespace detail {

struct Fail { std::string msg; };

// ------------------------------------------------------------ DEFLATE
// RFC 1951, decoding only.
class Inflate {
public:
    Inflate(const uint8_t* in, size_t n, std::string& out) : in_(in), n_(n), out_(out) {}
    void run() {
        for (bool last = false; !last;) {
            last = bits(1);
            int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) { fixed(); codes(); }
            else if (type == 2) { dynamic(); codes(); }
            else throw Fail{"bad block in compressed data"};
        }
    }

private:
    struct Huff { uint16_t count[16] = {}; uint16_t symbol[288] = {}; };
    const uint8_t* in_;
    size_t n_, pos_ = 0;
    uint32_t buf_ = 0;
    int cnt_ = 0;
    std::string& out_;
    Huff len_, dist_;

    int bits(int need) {
        while (cnt_ < need) {
            if (pos_ >= n_) throw Fail{"compressed data ends early"};
            buf_ |= uint32_t(in_[pos_++]) << cnt_;
            cnt_ += 8;
        }
        int v = int(buf_ & ((1u << need) - 1));
        buf_ >>= need;
        cnt_ -= need;
        return v;
    }
    static void build(Huff& h, const uint16_t* length, int n) {
        for (uint16_t& c : h.count) c = 0;
        for (int i = 0; i < n; ++i) h.count[length[i]]++;
        h.count[0] = 0;
        int left = 1;
        for (int len = 1; len <= 15; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) throw Fail{"bad code lengths in compressed data"};
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; ++len) offs[len + 1] = uint16_t(offs[len] + h.count[len]);
        for (int i = 0; i < n; ++i)
            if (length[i]) h.symbol[offs[length[i]]++] = uint16_t(i);
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= 15; ++len) {
            code |= bits(1);
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Fail{"bad code in compressed data"};
    }
    void stored() {
        buf_ = 0;
        cnt_ = 0;
        if (pos_ + 4 > n_) throw Fail{"compressed data ends early"};
        unsigned len = in_[pos_] | (in_[pos_ + 1] << 8), nlen = in_[pos_ + 2] | (in_[pos_ + 3] << 8);
        pos_ += 4;
        if ((len ^ 0xffff) != nlen || pos_ + len > n_) throw Fail{"bad stored block in compressed data"};
        out_.append(reinterpret_cast<const char*>(in_ + pos_), len);
        pos_ += len;
    }
    void fixed() {
        uint16_t l[288];
        int i = 0;
        for (; i < 144; ++i) l[i] = 8;
        for (; i < 256; ++i) l[i] = 9;
        for (; i < 280; ++i) l[i] = 7;
        for (; i < 288; ++i) l[i] = 8;
        build(len_, l, 288);
        for (i = 0; i < 30; ++i) l[i] = 5;
        build(dist_, l, 30);
    }
    void dynamic() {
        static const int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) throw Fail{"bad table sizes in compressed data"};
        uint16_t l[320] = {};
        for (int i = 0; i < ncode; ++i) l[order[i]] = uint16_t(bits(3));
        Huff cl;
        build(cl, l, 19);
        for (int i = 0; i < nlen + ndist;) {
            int sym = decode(cl);
            if (sym < 16) { l[i++] = uint16_t(sym); continue; }
            int prev = 0, rep;
            if (sym == 16) {
                if (i == 0) throw Fail{"bad repeat in compressed data"};
                prev = l[i - 1];
                rep = 3 + bits(2);
            } else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (i + rep > nlen + ndist) throw Fail{"bad repeat in compressed data"};
            while (rep--) l[i++] = uint16_t(prev);
        }
        build(len_, l, nlen);
        build(dist_, l + nlen, ndist);
    }
    void codes() {
        static const int lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const int dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(len_);
            if (sym < 256) { out_.push_back(char(sym)); continue; }
            if (sym == 256) return;
            sym -= 257;
            if (sym >= 29) throw Fail{"bad length in compressed data"};
            int len = lbase[sym] + bits(lext[sym]);
            int ds = decode(dist_);
            if (ds >= 30) throw Fail{"bad distance in compressed data"};
            size_t dist = size_t(dbase[ds] + bits(dext[ds]));
            if (dist > out_.size()) throw Fail{"bad distance in compressed data"};
            size_t from = out_.size() - dist;
            for (int i = 0; i < len; ++i) out_.push_back(out_[from + size_t(i)]);
        }
    }
};

// ------------------------------------------------------------- reader
struct P64 { int64_t x = 0, y = 0; };

struct Repetition {
    bool regular = true;
    int64_t n = 1, m = 1;       // regular: n positions along a, m along b
    P64 a, b;
    std::vector<P64> offs;      // irregular: every position, the first is (0, 0)
    size_t size() const { return regular ? size_t(n) * size_t(m) : offs.size(); }
    template <class F>
    void each(F&& f) const {
        if (!regular) { for (const P64& p : offs) f(p); return; }
        for (int64_t j = 0; j < m; ++j)
            for (int64_t i = 0; i < n; ++i) f(P64{i * a.x + j * b.x, i * a.y + j * b.y});
    }
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n, gds::Library& lib) : lib_(lib) { stack_.push_back({d, 0, n}); }

    void run() {
        if (stack_[0].n < 13 || std::memcmp(stack_[0].d, "%SEMI-OASIS\r\n", 13) != 0) throw Fail{"not an OASIS file"};
        stack_[0].pos = 13;
        lib_ = gds::Library();
        bool started = false;
        for (;;) {
            uint64_t rec = uint_();
            if (!started && rec != 1) throw Fail{"the file does not begin with a START record"};
            switch (rec) {
                case 0: break;                                   // PAD
                case 1: start(); started = true; break;
                case 2: finish(); return;                        // END
                case 3: cellNames_[nextCellName_++] = str(); break;
                case 4: { std::string s = str(); cellNames_[uint_()] = s; break; }
                case 5: textStrings_[nextText_++] = str(); break;
                case 6: { std::string s = str(); textStrings_[uint_()] = s; break; }
                case 7: str(); break;                            // PROPNAME
                case 8: str(); uint_(); break;
                case 9: str(); break;                            // PROPSTRING
                case 10: str(); uint_(); break;
                case 11: case 12: str(); interval(); interval(); break;   // LAYERNAME
                case 13: {
                    newCell();
                    cellFix_.push_back({lib_.cells.size() - 1, uint_()});
                    break;
                }
                case 14: newCell(); lib_.cells.back().name = str(); break;
                case 15: relative_ = false; break;
                case 16: relative_ = true; break;
                case 17: case 18: placement(rec == 18); break;
                case 19: text(); break;
                case 20: rectangle(); break;
                case 21: polygon(); break;
                case 22: path(); break;
                case 23: case 24: case 25: trapezoid(int(rec)); break;
                case 26: ctrapezoid(); break;
                case 27: circle(); break;
                case 28: property(); break;
                case 29: break;                                  // PROPERTY, as the last one
                case 30: uint_(); str(); break;                  // XNAME
                case 31: uint_(); str(); uint_(); break;
                case 32: uint_(); str(); break;                  // XELEMENT
                case 33: xgeometry(); break;
                case 34: cblock(); break;
                default: throw Fail{"unknown record " + std::to_string(rec)};
            }
        }
    }

private:
    struct Buf { const uint8_t* d; size_t pos, n; };
    gds::Library& lib_;
    std::vector<Buf> stack_;
    std::deque<std::string> inflated_;

    std::unordered_map<uint64_t, std::string> cellNames_, textStrings_;
    uint64_t nextCellName_ = 0, nextText_ = 0;
    struct Fix { size_t cell, index; uint64_t ref; };
    std::vector<std::pair<size_t, uint64_t>> cellFix_;   // cell index, reference number
    std::vector<Fix> refFix_, textFix_;

    // Modal variables.
    bool relative_ = false;
    Repetition rep_;
    int64_t placeX_ = 0, placeY_ = 0, geomX_ = 0, geomY_ = 0, textX_ = 0, textY_ = 0;
    bool placeByRef_ = false, textByRef_ = false;
    std::string placeName_, textStr_;
    uint64_t placeRef_ = 0, textRef_ = 0;
    uint64_t layer_ = 0, datatype_ = 0, textLayer_ = 0, textType_ = 0;
    int64_t w_ = 0, h_ = 0, halfWidth_ = 0, startExt_ = 0, endExt_ = 0, radius_ = 0;
    uint64_t ctrapType_ = 0;
    std::vector<P64> polyPts_, pathPts_;

    // ---- bytes
    uint8_t byte() {
        for (;;) {
            Buf& b = stack_.back();
            if (b.pos < b.n) return b.d[b.pos++];
            if (stack_.size() == 1) throw Fail{"the file ends in the middle of a record"};
            stack_.pop_back();
        }
    }
    uint64_t uint_() {
        uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            uint8_t b = byte();
            if (shift >= 64 && (b & 0x7f)) throw Fail{"an integer does not fit in 64 bits"};
            if (shift < 64) v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return v;
        }
    }
    int64_t sint() {
        uint64_t u = uint_();
        int64_t v = int64_t(u >> 1);
        return (u & 1) ? -v : v;
    }
    double real(uint64_t type) {
        switch (type) {
            case 0: return double(uint_());
            case 1: return -double(uint_());
            case 2: return 1.0 / double(uint_());
            case 3: return -1.0 / double(uint_());
            case 4: { double a = double(uint_()); return a / double(uint_()); }
            case 5: { double a = double(uint_()); return -a / double(uint_()); }
            case 6: {
                uint8_t b[4];
                for (uint8_t& x : b) x = byte();
                uint32_t u = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
                float f;
                std::memcpy(&f, &u, 4);
                return double(f);
            }
            case 7: {
                uint64_t u = 0;
                for (int i = 0; i < 8; ++i) u |= uint64_t(byte()) << (8 * i);
                double f;
                std::memcpy(&f, &u, 8);
                return f;
            }
        }
        throw Fail{"unknown real number type " + std::to_string(type)};
    }
    double real() { return real(uint_()); }
    std::string str() {
        uint64_t n = uint_();
        std::string s;
        s.reserve(size_t(std::min<uint64_t>(n, 1 << 20)));
        for (uint64_t i = 0; i < n; ++i) s.push_back(char(byte()));
        return s;
    }
    void interval() {
        uint64_t t = uint_();
        if (t == 0) return;
        if (t > 4) throw Fail{"bad interval type"};
        uint_();
        if (t == 4) uint_();
    }

    // ---- deltas
    static P64 dir8(unsigned d, int64_t v) {
        switch (d) {
            case 0: return {v, 0};
            case 1: return {0, v};
            case 2: return {-v, 0};
            case 3: return {0, -v};
            case 4: return {v, v};
            case 5: return {-v, v};
            case 6: return {-v, -v};
            default: return {v, -v};
        }
    }
    P64 delta2() { uint64_t u = uint_(); return dir8(unsigned(u & 3), int64_t(u >> 2)); }
    P64 delta3() { uint64_t u = uint_(); return dir8(unsigned(u & 7), int64_t(u >> 3)); }
    P64 gdelta() {
        uint64_t u = uint_();
        if (!(u & 1)) return dir8(unsigned((u >> 1) & 7), int64_t(u >> 4));
        int64_t x = int64_t(u >> 2);
        if (u & 2) x = -x;
        int64_t y = sint();
        return {x, y};
    }

    void repetition() {
        uint64_t t = uint_();
        if (t == 0) return;   // the same as the last one
        Repetition r;
        auto count = [&] {
            uint64_t n = uint_() + 2;
            if (n > (1u << 30)) throw Fail{"a repetition is too large"};
            return int64_t(n);
        };
        switch (t) {
            case 1: { r.n = count(); r.m = count(); r.a = {int64_t(uint_()), 0}; r.b = {0, int64_t(uint_())}; break; }
            case 2: { r.n = count(); r.a = {int64_t(uint_()), 0}; break; }
            case 3: { r.n = count(); r.a = {0, int64_t(uint_())}; break; }
            case 4: case 5: case 6: case 7: {
                int64_t n = count();
                int64_t grid = (t == 5 || t == 7) ? int64_t(uint_()) : 1;
                r.regular = false;
                P64 p;
                r.offs.push_back(p);
                for (int64_t i = 1; i < n; ++i) {
                    int64_t s = int64_t(uint_()) * grid;
                    if (t <= 5) p.x += s; else p.y += s;
                    r.offs.push_back(p);
                }
                break;
            }
            case 8: { r.n = count(); r.m = count(); r.a = gdelta(); r.b = gdelta(); break; }
            case 9: { r.n = count(); r.a = gdelta(); break; }
            case 10: case 11: {
                int64_t n = count();
                int64_t grid = t == 11 ? int64_t(uint_()) : 1;
                r.regular = false;
                P64 p;
                r.offs.push_back(p);
                for (int64_t i = 1; i < n; ++i) {
                    P64 d = gdelta();
                    p.x += d.x * grid;
                    p.y += d.y * grid;
                    r.offs.push_back(p);
                }
                break;
            }
            default: throw Fail{"unknown repetition type " + std::to_string(t)};
        }
        rep_ = std::move(r);
    }

    // The points after the first one, relative to it.
    void pointList(std::vector<P64>& out, bool polygon) {
        uint64_t t = uint_(), n = uint_();
        if (n > (1u << 28)) throw Fail{"a point list is too long"};
        out.clear();
        P64 p;
        switch (t) {
            case 0: case 1: {
                bool horizontal = t == 0;
                for (uint64_t i = 0; i < n; ++i, horizontal = !horizontal) {
                    int64_t d = sint();
                    if (horizontal) p.x += d; else p.y += d;
                    out.push_back(p);
                }
                // A polygon closes with one more point, so that the last two edges are axis-parallel too.
                if (polygon) {
                    if (horizontal) p.x = 0; else p.y = 0;
                    out.push_back(p);
                }
                break;
            }
            case 2: for (uint64_t i = 0; i < n; ++i) { P64 d = delta2(); p.x += d.x; p.y += d.y; out.push_back(p); } break;
            case 3: for (uint64_t i = 0; i < n; ++i) { P64 d = delta3(); p.x += d.x; p.y += d.y; out.push_back(p); } break;
            case 4: for (uint64_t i = 0; i < n; ++i) { P64 d = gdelta(); p.x += d.x; p.y += d.y; out.push_back(p); } break;
            case 5: {
                P64 step;
                for (uint64_t i = 0; i < n; ++i) {
                    P64 d = gdelta();
                    step.x += d.x; step.y += d.y;
                    p.x += step.x; p.y += step.y;
                    out.push_back(p);
                }
                break;
            }
            default: throw Fail{"unknown point list type " + std::to_string(t)};
        }
    }

    // ---- records
    void start() {
        std::string version = str();
        if (version != "1.0") throw Fail{"OASIS version " + version + " is not supported"};
        double unit = real();
        if (!(unit > 0)) throw Fail{"bad unit in the START record"};
        lib_.dbu = 1.0 / unit;
        lib_.userUnit = lib_.dbu;
        if (uint_() == 0)
            for (int i = 0; i < 12; ++i) uint_();   // the table offsets; the file is read in order anyway
    }

    void newCell() {
        lib_.cells.emplace_back();
        relative_ = false;
        placeX_ = placeY_ = geomX_ = geomY_ = textX_ = textY_ = 0;
    }
    gds::Cell& cell() {
        if (lib_.cells.empty()) throw Fail{"an element before the first CELL record"};
        return lib_.cells.back();
    }
    void xy(bool hasX, bool hasY, int64_t& mx, int64_t& my) {
        if (hasX) { int64_t v = sint(); mx = relative_ ? mx + v : v; }
        if (hasY) { int64_t v = sint(); my = relative_ ? my + v : v; }
    }
    static gds::Point pt(int64_t x, int64_t y) {
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX) throw Fail{"a coordinate does not fit in 32 bits"};
        return {int32_t(x), int32_t(y)};
    }
    void layerDatatype(uint8_t info) {
        if (info & 0x01) layer_ = uint_();
        if (info & 0x02) datatype_ = uint_();
        if (layer_ > 32767 || datatype_ > 32767) throw Fail{"layer or datatype above 32767"};
    }
    template <class F>
    void repeated(bool hasRep, F&& f) {
        if (!hasRep) { f(P64{}); return; }
        repetition();
        rep_.each(f);
    }
    void addPolygon(const std::vector<P64>& pts, int64_t x, int64_t y, bool hasRep) {
        repeated(hasRep, [&](P64 o) {
            gds::Polygon p;
            p.layer = int16_t(layer_);
            p.datatype = int16_t(datatype_);
            p.pts.reserve(pts.size());
            for (const P64& q : pts) p.pts.push_back(pt(x + o.x + q.x, y + o.y + q.y));
            cell().polygons.push_back(std::move(p));
        });
    }

    void placement(bool general) {
        uint8_t info = byte();
        if (info & 0x80) {
            placeByRef_ = info & 0x40;
            if (placeByRef_) placeRef_ = uint_(); else placeName_ = str();
        }
        gds::Ref r;
        if (general) {
            if (info & 0x04) r.mag = real();
            if (info & 0x02) r.angle = real();
        } else {
            r.angle = 90.0 * ((info >> 1) & 3);
        }
        r.mirror = info & 0x01;
        xy(info & 0x20, info & 0x10, placeX_, placeY_);
        if (!placeByRef_) r.cell = placeName_;
        auto add = [&](const gds::Ref& one) {
            cell().refs.push_back(one);
            if (placeByRef_) refFix_.push_back({lib_.cells.size() - 1, cell().refs.size() - 1, placeRef_});
        };
        if (!(info & 0x08)) {
            r.origin = pt(placeX_, placeY_);
            add(r);
            return;
        }
        repetition();
        if (rep_.regular) {
            r.origin = pt(placeX_, placeY_);
            r.cols = int32_t(rep_.n);
            r.rows = int32_t(rep_.m);
            r.colStep = pt(rep_.a.x, rep_.a.y);
            r.rowStep = pt(rep_.b.x, rep_.b.y);
            add(r);
            return;
        }
        for (const P64& o : rep_.offs) {
            r.origin = pt(placeX_ + o.x, placeY_ + o.y);
            add(r);
        }
    }

    void text() {
        uint8_t info = byte();
        if (info & 0x40) {
            textByRef_ = info & 0x20;
            if (textByRef_) textRef_ = uint_(); else textStr_ = str();
        }
        if (info & 0x01) textLayer_ = uint_();
        if (info & 0x02) textType_ = uint_();
        if (textLayer_ > 32767 || textType_ > 32767) throw Fail{"text layer or type above 32767"};
        xy(info & 0x10, info & 0x08, textX_, textY_);
        repeated(info & 0x04, [&](P64 o) {
            gds::Text t;
            t.layer = int16_t(textLayer_);
            t.texttype = int16_t(textType_);
            t.at = pt(textX_ + o.x, textY_ + o.y);
            if (!textByRef_) t.str = textStr_;
            cell().texts.push_back(std::move(t));
            if (textByRef_) textFix_.push_back({lib_.cells.size() - 1, cell().texts.size() - 1, textRef_});
        });
    }

    void rectangle() {
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x40) w_ = int64_t(uint_());
        if (info & 0x20) h_ = int64_t(uint_());
        if (info & 0x80) h_ = w_;   // a square
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        std::vector<P64> pts = {{0, 0}, {w_, 0}, {w_, h_}, {0, h_}};
        addPolygon(pts, geomX_, geomY_, info & 0x04);
    }

    void polygon() {
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x20) pointList(polyPts_, true);
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        std::vector<P64> pts;
        pts.push_back({0, 0});
        pts.insert(pts.end(), polyPts_.begin(), polyPts_.end());
        addPolygon(pts, geomX_, geomY_, info & 0x04);
    }

    void path() {
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x40) halfWidth_ = int64_t(uint_());
        if (info & 0x80) {
            uint64_t scheme = uint_();
            auto ext = [&](unsigned s, int64_t& e) {
                if (s == 1) e = 0;
                else if (s == 2) e = halfWidth_;
                else if (s == 3) e = sint();
            };
            ext(unsigned((scheme >> 2) & 3), startExt_);
            ext(unsigned(scheme & 3), endExt_);
        }
        if (info & 0x20) pointList(pathPts_, false);
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        repeated(info & 0x04, [&](P64 o) {
            gds::Path p;
            p.layer = int16_t(layer_);
            p.datatype = int16_t(datatype_);
            if (halfWidth_ > INT32_MAX / 2) throw Fail{"a path is too wide"};
            p.width = int32_t(2 * halfWidth_);
            if (startExt_ == 0 && endExt_ == 0) p.pathtype = 0;
            else if (startExt_ == halfWidth_ && endExt_ == halfWidth_) p.pathtype = 2;
            else {
                p.pathtype = 4;
                gds::Point e = pt(startExt_, endExt_);
                p.bgnextn = e.x;
                p.endextn = e.y;
            }
            p.pts.push_back(pt(geomX_ + o.x, geomY_ + o.y));
            for (const P64& q : pathPts_) p.pts.push_back(pt(geomX_ + o.x + q.x, geomY_ + o.y + q.y));
            cell().paths.push_back(std::move(p));
        });
    }

    void trapezoid(int rec) {
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x40) w_ = int64_t(uint_());
        if (info & 0x20) h_ = int64_t(uint_());
        int64_t da = rec != 25 ? sint() : 0, db = rec != 24 ? sint() : 0;
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        std::vector<P64> pts;
        if (info & 0x80)   // the parallel sides are vertical
            pts = {{0, std::max<int64_t>(da, 0)}, {0, h_ + std::min<int64_t>(db, 0)}, {w_, h_ - std::max<int64_t>(db, 0)}, {w_, -std::min<int64_t>(da, 0)}};
        else
            pts = {{std::max<int64_t>(da, 0), h_}, {w_ + std::min<int64_t>(db, 0), h_}, {w_ - std::max<int64_t>(db, 0), 0}, {-std::min<int64_t>(da, 0), 0}};
        addPolygon(pts, geomX_, geomY_, info & 0x04);
    }

    void ctrapezoid() {
        // Each vertex is (a*w + b*h, c*w + d*h).
        static const int8_t table[26][4][4] = {
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, -1, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, -1, 0, 0}},
            {{0, 0, 0, 0}, {0, 1, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 1, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 1, 0, 1}, {1, -1, 0, 1}, {1, 0, 0, 0}},
            {{0, 1, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, -1, 0, 0}},
            {{0, 0, 0, 0}, {0, 1, 0, 1}, {1, 0, 0, 1}, {1, -1, 0, 0}},
            {{0, 1, 0, 0}, {0, 0, 0, 1}, {1, -1, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 0, -1, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, -1, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 1, 0}},
            {{0, 0, 1, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 0, -1, 1}, {1, 0, 1, 0}},
            {{0, 0, 1, 0}, {0, 0, -1, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, -1, 1}, {1, 0, 0, 1}, {1, 0, 1, 0}},
            {{0, 0, 1, 0}, {0, 0, 0, 1}, {1, 0, -1, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 1, 0}, {0, 0, 0, 0}},
            {{0, 0, 0, 0}, {1, 0, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 0}},
            {{0, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 0, 0}, {0, 0, 1, 0}},
            {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 2, 0, 0}, {0, 0, 0, 0}},
            {{0, 0, 0, 1}, {0, 2, 0, 1}, {0, 1, 0, 0}, {0, 0, 0, 1}},
            {{0, 0, 0, 0}, {0, 0, 2, 0}, {1, 0, 1, 0}, {0, 0, 0, 0}},
            {{1, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 2, 0}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 0}},
            {{0, 0, 0, 0}, {0, 0, 1, 0}, {1, 0, 1, 0}, {1, 0, 0, 0}},
        };
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x80) ctrapType_ = uint_();
        if (info & 0x40) w_ = int64_t(uint_());
        if (info & 0x20) h_ = int64_t(uint_());
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        if (ctrapType_ > 25) throw Fail{"unknown compact trapezoid type " + std::to_string(ctrapType_)};
        // The types with one free size fix the other one, also for the records that follow.
        if ((ctrapType_ >= 16 && ctrapType_ <= 19) || ctrapType_ == 25) h_ = w_;
        else if (ctrapType_ == 20 || ctrapType_ == 21) w_ = 2 * h_;
        else if (ctrapType_ == 22 || ctrapType_ == 23) h_ = 2 * w_;
        std::vector<P64> pts;
        for (int i = 0; i < 4; ++i) {
            const int8_t* t = table[ctrapType_][i];
            P64 p{t[0] * w_ + t[1] * h_, t[2] * w_ + t[3] * h_};
            if (pts.empty() || pts.back().x != p.x || pts.back().y != p.y) pts.push_back(p);
        }
        if (pts.size() > 1 && pts.back().x == pts[0].x && pts.back().y == pts[0].y) pts.pop_back();
        addPolygon(pts, geomX_, geomY_, info & 0x04);
    }

    void circle() {
        uint8_t info = byte();
        layerDatatype(info);
        if (info & 0x20) radius_ = int64_t(uint_());
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        std::vector<P64> pts;
        const int n = 64;
        const double pi = 3.14159265358979323846;
        for (int i = 0; i < n; ++i) {
            double a = 2 * pi * (i + 0.5) / n, r = double(radius_) / std::cos(pi / n);   // the sides touch the circle
            pts.push_back({std::llround(r * std::cos(a)), std::llround(r * std::sin(a))});
        }
        addPolygon(pts, geomX_, geomY_, info & 0x04);
    }

    void xgeometry() {
        uint8_t info = byte();
        uint_();
        layerDatatype(info);
        str();
        xy(info & 0x10, info & 0x08, geomX_, geomY_);
        if (info & 0x04) repetition();
    }

    void property() {
        uint8_t info = byte();
        if (info & 0x04) {
            if (info & 0x02) uint_(); else str();
        }
        if (info & 0x08) return;   // the last value list again
        uint64_t n = info >> 4;
        if (n == 15) n = uint_();
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t t = uint_();
            if (t <= 7) real(t);
            else if (t == 8) uint_();
            else if (t == 9) sint();
            else if (t <= 12) str();
            else if (t <= 15) uint_();
            else throw Fail{"unknown property value type " + std::to_string(t)};
        }
    }

    void cblock() {
        uint64_t type = uint_(), rawSize = uint_(), packedSize = uint_();
        if (type != 0) throw Fail{"unknown compression type " + std::to_string(type)};
        Buf& b = stack_.back();
        if (packedSize > b.n - b.pos) throw Fail{"a compressed block runs past the end of the file"};
        inflated_.emplace_back();
        std::string& out = inflated_.back();
        out.reserve(size_t(std::min<uint64_t>(rawSize, 1u << 30)));
        Inflate(b.d + b.pos, size_t(packedSize), out).run();
        if (out.size() != rawSize) throw Fail{"a compressed block has the wrong size"};
        b.pos += size_t(packedSize);
        stack_.push_back({reinterpret_cast<const uint8_t*>(out.data()), 0, out.size()});
    }

    void finish() {
        auto name = [&](const std::unordered_map<uint64_t, std::string>& m, uint64_t ref, const char* what) -> const std::string& {
            auto it = m.find(ref);
            if (it == m.end()) throw Fail{std::string(what) + " " + std::to_string(ref) + " is used but has no name"};
            return it->second;
        };
        for (auto [ci, ref] : cellFix_) lib_.cells[ci].name = name(cellNames_, ref, "cell");
        for (const Fix& f : refFix_) lib_.cells[f.cell].refs[f.index].cell = name(cellNames_, f.ref, "cell");
        for (const Fix& f : textFix_) lib_.cells[f.cell].texts[f.index].str = name(textStrings_, f.ref, "text string");
    }
};

}  // namespace detail

inline bool parse(const uint8_t* d, size_t n, gds::Library& lib, std::string& err) {
    try {
        detail::Reader(d, n, lib).run();
    } catch (const detail::Fail& f) {
        err = f.msg;
        return false;
    }
    return true;
}

inline bool read(const std::string& file, gds::Library& lib, std::string& err) {
    std::string data;
    if (!gds::readFile(file, data, err)) return false;
    if (!oasis::parse(reinterpret_cast<const uint8_t*>(data.data()), data.size(), lib, err)) {
        err = file + ": " + err;
        return false;
    }
    return true;
}

// A layout file of either format, told apart by its first bytes.
inline bool readLayout(const std::string& file, gds::Library& lib, std::string& err) {
    std::string data;
    if (!gds::readFile(file, data, err)) return false;
    const uint8_t* d = reinterpret_cast<const uint8_t*>(data.data());
    bool ok = isOasis(data) ? oasis::parse(d, data.size(), lib, err) : gds::parse(d, data.size(), lib, err);
    if (!ok) err = file + ": " + err;
    return ok;
}

}  // namespace oasis
