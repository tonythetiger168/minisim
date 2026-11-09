// expr.hpp - 4-state (0/1/X/Z) values, Verilog operators and an expression tree.
// Replaces Val/mk/add from minisim2.cpp.
//
// A value wider than 64 bits is a little-endian sequence of Limbs. Each Limb
// is 64 bits of the three planes v (0/1), x (unknown), and z (Z, a subset of
// x). Limb 0 lives in Val::v / Val::x / Val::z, so a value of 64 bits or less
// still has its whole state in those fields and an empty `hi`. Limb k (k > 0)
// is hi[k-1] and holds bits [64k, 64k+63]. Operators walk every limb; the
// single-word path is the same code the 64-bit results were checked with.
//
// Encoding: bit i is unknown if x bit i = 1; otherwise its value is v bit i.
// An unknown bit is Z if z bit i = 1 (z is always a subset of x), else X.
// Most operators treat Z as X and return X (the result has no Z).
// Logical shifts (<<, >>) keep Z on bits that move and fill vacated bits
// with 0. An X or Z bit in the shift count makes the whole result X.
// >>> is an arithmetic right shift: a signed value fills vacated bits with
// its MSB (0, 1, X, or Z). An unsigned >>> is a logical >>.
// Val::sgn is the signedness of an expression result. Nets store bits only;
// a read comes back unsigned and the expression that reads it sets sgn.
// Add, subtract, multiply, divide, remainder, equality, and the relational
// operators are signed only when both operands are signed. Otherwise they
// zero-extend.
// ?: with a known 0/1 condition selects that arm, Z included. With an X or Z
// condition, a bit is kept when both arms have that same 0, 1, X, or Z;
// any other pair becomes X (IEEE 1364/1800 conditional operator).
// slice/concat/insert/index and === / casez / casex also preserve or inspect Z.
// Invariant (enforced by make()): v bits are 0 wherever x bits are 1,
// z is within x, and nothing is set above bit w-1.
//
// Integration with minisim2.cpp:
//   - #include "expr.hpp" and delete its Val / mk / add definitions.
//   - Sim::get(s) should return the stored Val with .w = sigs[s].w.
//   - Evaluate with: eval(expr, [&](int s){ return S.get(s); })
//   - `if (cond)` is: truth(eval(...)) == 1   (X counts as false, like Verilog)
//
// Example (cnt <= cnt + 4'd1):
//   auto e = bin(Op::Add, ref(cnt), konst(mk(1, 4)));
//   S.nba(cnt, eval(e, rd));
#pragma once
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <sys/wait.h>

// One 64-bit limb of a wide value. v/x/z use the same encoding as Val.
struct Limb {
    uint64_t v = 0, x = 0, z = 0;
    bool operator==(const Limb& o) const { return v == o.v && x == o.x && z == o.z; }
};

// The high limbs of a Val. Most values are 64 bits or less, so this is
// almost always empty, and copying an empty one must cost no more than two
// stores: a std::vector here made Val's copy constructor and assignment
// about a fifth of a -compile run. Only the operations Val uses.
class LimbVec {
    Limb* p_ = nullptr;
    uint32_t n_ = 0;
    void copyFrom(const LimbVec& o) {
        n_ = o.n_;
        p_ = n_ ? new Limb[n_] : nullptr;
        for (uint32_t i = 0; i < n_; i++) p_[i] = o.p_[i];
    }
public:
    LimbVec() = default;
    LimbVec(std::initializer_list<Limb> l) : p_(l.size() ? new Limb[l.size()] : nullptr), n_((uint32_t)l.size()) {
        uint32_t i = 0;
        for (const Limb& x : l) p_[i++] = x;
    }
    LimbVec(const LimbVec& o) { if (o.n_) copyFrom(o); }
    LimbVec(LimbVec&& o) noexcept : p_(o.p_), n_(o.n_) { o.p_ = nullptr; o.n_ = 0; }
    LimbVec& operator=(const LimbVec& o) {
        if (this == &o) return *this;
        if (!n_ && !o.n_) return *this;
        if (n_ == o.n_) { for (uint32_t i = 0; i < n_; i++) p_[i] = o.p_[i]; return *this; }
        delete[] p_;
        p_ = nullptr; n_ = 0;
        if (o.n_) copyFrom(o);
        return *this;
    }
    LimbVec& operator=(LimbVec&& o) noexcept {
        if (this != &o) { delete[] p_; p_ = o.p_; n_ = o.n_; o.p_ = nullptr; o.n_ = 0; }
        return *this;
    }
    ~LimbVec() { delete[] p_; }
    size_t size() const { return n_; }
    bool empty() const { return n_ == 0; }
    void clear() { delete[] p_; p_ = nullptr; n_ = 0; }
    void resize(size_t n) {   // new limbs are zero
        if (n == n_) return;
        if (n == 0) { clear(); return; }
        Limb* q = new Limb[n];
        for (size_t i = 0; i < n && i < n_; i++) q[i] = p_[i];
        delete[] p_;
        p_ = q; n_ = (uint32_t)n;
    }
    Limb& operator[](size_t i) { return p_[i]; }
    const Limb& operator[](size_t i) const { return p_[i]; }
    Limb* begin() { return p_; }
    Limb* end() { return p_ + n_; }
    const Limb* begin() const { return p_; }
    const Limb* end() const { return p_ + n_; }
};

struct Val {
    uint64_t v = 0, x = ~0ULL;  // default: all X. Limb 0 (bits [0,63]).
    int w = 1;
    uint64_t z = 0;             // which unknown bits are Z (subset of x)
    bool sgn = false;           // expression result is signed; not stored on a net
    bool real = false;          // v holds an IEEE double (6.12); w is 64
    bool str = false;           // v is a string id (6.16); see strPool
    LimbVec hi;                 // limbs 1.. : hi[k] is bits [64*(k+1), 64*(k+2))
    // Missing high limbs compare as 0, so a 64-bit value (empty hi) still
    // matches the way it did before hi existed.
    bool operator==(const Val& o) const {
        if (v != o.v || x != o.x || z != o.z) return false;
        size_t n = hi.size() > o.hi.size() ? hi.size() : o.hi.size();
        for (size_t i = 0; i < n; i++) {
            Limb a = i < hi.size() ? hi[i] : Limb{};
            Limb b = i < o.hi.size() ? o.hi[i] : Limb{};
            if (!(a == b)) return false;
        }
        return true;
    }
};

inline uint64_t mask(int w) { return w >= 64 ? ~0ULL : ((1ULL << w) - 1); }

// How many limbs a w-bit value occupies, and the mask of limb i.
inline int limbCount(int w) { return w <= 0 ? 1 : (w + 63) / 64; }
inline uint64_t limbMask(int w, int i) {
    int bits = w - i * 64;
    if (bits >= 64) return ~0ULL;
    if (bits <= 0) return 0;
    return (1ULL << bits) - 1;
}
inline uint64_t limbV(const Val& a, int i) {
    if (i <= 0) return a.v;
    return i - 1 < (int)a.hi.size() ? a.hi[i - 1].v : 0;
}
inline uint64_t limbX(const Val& a, int i) {
    if (i <= 0) return a.x;
    return i - 1 < (int)a.hi.size() ? a.hi[i - 1].x : 0;
}
inline uint64_t limbZ(const Val& a, int i) {
    if (i <= 0) return a.z;
    return i - 1 < (int)a.hi.size() ? a.hi[i - 1].z : 0;
}
inline void setLimb(Val& a, int i, uint64_t v, uint64_t x, uint64_t z) {
    if (i <= 0) { a.v = v; a.x = x; a.z = z; return; }
    if ((int)a.hi.size() < i) a.hi.resize(i);
    a.hi[i - 1] = Limb{v, x, z};
}
// Enforce the make() invariant on every limb and drop limbs past w.
inline Val finish(Val a) {
    if (a.w <= 64) {
        uint64_t m = mask(a.w);
        a.x &= m;
        a.v &= m & ~a.x;
        a.z &= a.x;
        a.hi.clear();
        return a;
    }
    int n = limbCount(a.w);
    if ((int)a.hi.size() != n - 1) a.hi.resize(n - 1);
    auto apply = [](uint64_t& v, uint64_t& x, uint64_t& z, uint64_t m) {
        x &= m;
        v &= m & ~x;
        z &= x;
    };
    apply(a.v, a.x, a.z, ~0ULL);
    for (int i = 1; i < n; i++)
        apply(a.hi[i - 1].v, a.hi[i - 1].x, a.hi[i - 1].z, limbMask(a.w, i));
    return a;
}
inline Val make(uint64_t v, uint64_t x, int w, uint64_t z = 0, bool sgn = false) {
    if (w > 64) {
        Val a;
        a.v = v; a.x = x; a.z = z; a.w = w; a.sgn = sgn;
        return finish(a);
    }
    uint64_t m = mask(w);
    x &= m;
    v &= m & ~x;
    z &= x;
    return Val{v, x, w, z, sgn};
}
// Known 0/1 words, little-endian, no X/Z. Used for literals wider than 64 bits.
inline Val wordsToVal(int w, bool sgn, const uint64_t* p, int nwords) {
    Val a;
    a.w = w;
    a.sgn = sgn;
    int n = limbCount(w);
    if (n > 1) a.hi.resize(n - 1);
    for (int i = 0; i < n; i++) setLimb(a, i, (p && i < nwords) ? p[i] : 0, 0, 0);
    return finish(a);
}
inline bool anyX(const Val& a) {
    if (a.x) return true;
    for (const auto& h : a.hi) if (h.x) return true;
    return false;
}
inline bool anyV(const Val& a) {
    if (a.v) return true;
    for (const auto& h : a.hi) if (h.v) return true;
    return false;
}
inline bool isZero(const Val& a) { return !anyV(a); }
// 0, 1, 2 = X, 3 = Z. A bit past the stored limbs is 0.
inline int bitAt(const Val& a, int i) {
    if (i < 0) return 2;
    uint64_t m = 1ULL << (i & 63);
    uint64_t v = limbV(a, i >> 6), x = limbX(a, i >> 6), z = limbZ(a, i >> 6);
    if (z & m) return 3;
    if (x & m) return 2;
    return (v & m) ? 1 : 0;
}
inline void setBit(Val& a, int i, int t) {
    if (i < 0) return;
    int li = i >> 6;
    uint64_t m = 1ULL << (i & 63);
    uint64_t v = limbV(a, li), x = limbX(a, li), z = limbZ(a, li);
    v &= ~m; x &= ~m; z &= ~m;
    if (t == 1) v |= m;
    else if (t == 2) x |= m;
    else if (t == 3) { x |= m; z |= m; }
    setLimb(a, li, v, x, z);
}
// Copy n bits from src[srcBit] into dst[dstBit]. Bits past either value read as 0.
inline void copyBits(Val& dst, int dstBit, const Val& src, int srcBit, int n) {
    int done = 0;
    while (done < n) {
        int d = dstBit + done, s = srcBit + done;
        int dsh = d & 63, ssh = s & 63;
        int chunk = n - done;
        if (chunk > 64 - dsh) chunk = 64 - dsh;
        if (chunk > 64 - ssh) chunk = 64 - ssh;
        uint64_t sm = chunk == 64 ? ~0ULL : ((1ULL << chunk) - 1);
        uint64_t sv = (limbV(src, s >> 6) >> ssh) & sm;
        uint64_t sx = (limbX(src, s >> 6) >> ssh) & sm;
        uint64_t sz = (limbZ(src, s >> 6) >> ssh) & sm;
        uint64_t dm = sm << dsh;
        int di = d >> 6;
        setLimb(dst, di, (limbV(dst, di) & ~dm) | (sv << dsh),
                (limbX(dst, di) & ~dm) | (sx << dsh),
                (limbZ(dst, di) & ~dm) | (sz << dsh));
        done += chunk;
    }
}
// Mask to w the way set() does: drop bits past w, do not clear v under x.
inline Val maskWidth(Val nv, int w) {
    if (w <= 64) {
        uint64_t m = mask(w);
        nv.v &= m; nv.x &= m; nv.z &= m;
        nv.hi.clear();
        nv.sgn = false;
        return nv;
    }
    int n = limbCount(w);
    if ((int)nv.hi.size() != n - 1) nv.hi.resize(n - 1);
    uint64_t m = limbMask(w, n - 1);
    nv.hi[n - 2].v &= m;
    nv.hi[n - 2].x &= m;
    nv.hi[n - 2].z &= m;
    nv.w = w;
    nv.sgn = false;
    return nv;
}
// make()'s invariant across every limb. A read of a stored signal uses this.
inline Val clipRead(const Val& src, int w) {
    if (w <= 64) return make(src.v, src.x, w, src.z);
    Val a;
    a.w = w;
    a.hi.resize(limbCount(w) - 1);
    for (int i = 0; i < limbCount(w); i++) {
        uint64_t m = limbMask(w, i);
        uint64_t x = limbX(src, i) & m;
        uint64_t v = limbV(src, i) & m & ~x;
        uint64_t z = limbZ(src, i) & x;
        setLimb(a, i, v, x, z);
    }
    return a;
}
// Widen or truncate. signFill replicates bit (a.w-1) into the new high bits,
// including when that bit is X or Z. Truncation keeps the low bits.
inline Val v_slice(Val a, int hi, int lo);
inline Val v_resize(Val a, int w, bool signFill) {
    if (w == a.w) return a;
    if (a.w <= 64 && w <= 64) {
        if (w < a.w) return make(a.v, a.x, w, a.z);
        if (!signFill) return make(a.v, a.x, w, a.z);
        uint64_t sb = 1ULL << (a.w - 1);
        uint64_t hi = mask(w) & ~mask(a.w);
        return make(a.v | ((a.v & sb) ? hi : 0), a.x | ((a.x & sb) ? hi : 0), w,
                    a.z | ((a.z & sb) ? hi : 0));
    }
    if (w < a.w) {
        Val r = v_slice(a, w - 1, 0);
        r.sgn = a.sgn;
        return r;
    }
    Val r = a;
    r.w = w;
    if (limbCount(w) > 1 && (int)r.hi.size() < limbCount(w) - 1) r.hi.resize(limbCount(w) - 1);
    if (!signFill) return finish(r);
    int sb = a.w > 0 ? bitAt(a, a.w - 1) : 0;
    r = finish(r);
    for (int i = a.w; i < w; i++) setBit(r, i, sb);
    r.sgn = a.sgn;
    return finish(r);
}
// Signed iff both operands are. Extend both to that common width first.
inline void align_signed(Val& a, Val& b) {
    bool s = a.sgn && b.sgn;
    int w = a.w > b.w ? a.w : b.w;
    a = v_resize(a, w, s);
    b = v_resize(b, w, s);
    a.sgn = b.sgn = s;
}
// Signed compare of two w-bit patterns. Same sign bit: unsigned order matches.
inline int scmp(uint64_t a, uint64_t b, int w) {
    uint64_t sign = 1ULL << (w >= 64 ? 63 : w - 1);
    bool as = a & sign, bs = b & sign;
    if (as != bs) return as ? -1 : 1;
    if (a == b) return 0;
    return a < b ? -1 : 1;
}
inline Val mk(uint64_t v, int w = 1) { return make(v, 0, w); }
inline Val allx(int w) {
    if (w <= 64) return make(0, ~0ULL, w);
    Val a;
    a.w = w;
    a.x = ~0ULL;
    a.hi.resize(limbCount(w) - 1);
    for (int i = 1; i < limbCount(w); i++) a.hi[i - 1].x = limbMask(w, i);
    return a;
}
inline Val allz(int w) {
    if (w <= 64) return make(0, ~0ULL, w, ~0ULL);
    Val a = allx(w);
    a.z = a.x;
    for (auto& h : a.hi) h.z = h.x;
    return a;
}
// Every bit 1. Pullup of a wide net, and a sized all-ones literal built by hand.
inline Val v_ones(int w) {
    if (w <= 64) return mk(mask(w), w);
    Val a;
    a.w = w;
    a.v = ~0ULL;
    a.x = 0;
    a.z = 0;
    a.hi.resize(limbCount(w) - 1);
    for (int i = 1; i < limbCount(w); i++) a.hi[i - 1].v = limbMask(w, i);
    return a;
}
inline Val xfill(int w, bool sgn = false) { Val a = allx(w); a.sgn = sgn; return a; }
inline int wmax(const Val& a, const Val& b) { return a.w > b.w ? a.w : b.w; }

// 0, 1, or 2 (= X). v is already 0 under X bits, so a known 1 in any limb is true.
inline int truth(const Val& a) {
    if (a.v) return 1;
    for (const auto& h : a.hi) if (h.v) return 1;
    if (a.x) return 2;
    for (const auto& h : a.hi) if (h.x) return 2;
    return 0;
}
inline Val from_truth(int t) { return t == 2 ? allx(1) : mk(t, 1); }

// ---- bitwise (X-aware: 0&X=0, 1|X=1) ----
// One limb of AND/OR/XOR. `kind` 0 and, 1 or, 2 xor. Result z is 0.
inline void bitPlane(uint64_t av, uint64_t ax, uint64_t bv, uint64_t bx, int kind,
                     uint64_t& vv, uint64_t& xx) {
    if (kind == 0) {
        uint64_t a0 = ~ax & ~av, b0 = ~bx & ~bv;
        vv = av & bv;
        xx = (ax | bx) & ~(a0 | b0);
    } else if (kind == 1) {
        vv = av | bv;
        xx = (ax | bx) & ~vv;
    } else {
        vv = av ^ bv;
        xx = ax | bx;
    }
}
inline Val v_bitwise(Val a, Val b, int kind) {
    if (a.w <= 64 && b.w <= 64 && a.hi.empty() && b.hi.empty()) {
        uint64_t vv, xx;
        bitPlane(a.v, a.x, b.v, b.x, kind, vv, xx);
        return make(vv, xx, wmax(a, b), 0, a.sgn && b.sgn);
    }
    int w = wmax(a, b);
    Val r;
    r.w = w;
    r.sgn = a.sgn && b.sgn;
    int n = limbCount(w);
    if (n > 1) r.hi.resize(n - 1);
    for (int i = 0; i < n; i++) {
        uint64_t vv, xx;
        bitPlane(limbV(a, i), limbX(a, i), limbV(b, i), limbX(b, i), kind, vv, xx);
        setLimb(r, i, vv, xx, 0);
    }
    return finish(r);
}
inline Val v_and(Val a, Val b) { return v_bitwise(a, b, 0); }
inline Val v_or(Val a, Val b) { return v_bitwise(a, b, 1); }
inline Val v_xor(Val a, Val b) { return v_bitwise(a, b, 2); }
inline Val v_not(Val a) {
    if (a.w <= 64 && a.hi.empty()) return make(~a.v, a.x, a.w, 0, a.sgn);
    Val r = a;
    r.v = ~a.v; r.z = 0;
    for (size_t i = 0; i < a.hi.size(); i++) { r.hi[i].v = ~a.hi[i].v; r.hi[i].z = 0; }
    return finish(r);
}

// ---- arithmetic: any X input -> all-X result ----
// Both signed: sign-extend to the wider operand, result stays signed.
// Either unsigned: zero-extend, result is unsigned. Two's-complement
// add/sub/mul then matches an unsigned op on those extended bits.
// sub: a + ~b + 1. The bits of ~b above the width do not feed back into
// the bits that finish() keeps.
inline Val addWide(const Val& a, const Val& b, bool sub) {
    int n = limbCount(a.w);
    Val r;
    r.w = a.w;
    r.sgn = a.sgn;
    if (n > 1) r.hi.resize(n - 1);
    uint64_t carry = sub ? 1 : 0;
    for (int i = 0; i < n; i++) {
        uint64_t bv = limbV(b, i);
        if (sub) bv = ~bv;
        unsigned __int128 s = (unsigned __int128)limbV(a, i) + bv + carry;
        setLimb(r, i, (uint64_t)s, 0, 0);
        carry = (uint64_t)(s >> 64);
    }
    return finish(r);
}
inline Val mulWide(const Val& a, const Val& b) {
    int n = limbCount(a.w);
    std::vector<uint64_t> acc(n, 0);
    for (int i = 0; i < n; i++) {
        unsigned __int128 carry = 0;
        for (int j = 0; j < n - i; j++) {
            unsigned __int128 s = (unsigned __int128)acc[i + j] +
                                  (unsigned __int128)limbV(a, i) * limbV(b, j) + carry;
            acc[i + j] = (uint64_t)s;
            carry = s >> 64;
        }
    }
    Val r;
    r.w = a.w;
    r.sgn = a.sgn;
    if (n > 1) r.hi.resize(n - 1);
    for (int i = 0; i < n; i++) setLimb(r, i, acc[i], 0, 0);
    return finish(r);
}
inline Val v_add(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b)) return xfill(a.w, a.sgn);
    if (a.w <= 64) return make(a.v + b.v, 0, a.w, 0, a.sgn);
    return addWide(a, b, false);
}
inline Val v_sub(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b)) return xfill(a.w, a.sgn);
    if (a.w <= 64) return make(a.v - b.v, 0, a.w, 0, a.sgn);
    return addWide(a, b, true);
}
inline Val v_mul(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b)) return xfill(a.w, a.sgn);
    if (a.w <= 64) return make(a.v * b.v, 0, a.w, 0, a.sgn);
    return mulWide(a, b);
}
// Negate a w-bit pattern. The minimum negative value has magnitude 2^(w-1),
// which fits in uint64_t through 64 bits. Negating that pattern with a
// signed integer is undefined at 64 bits, so the magnitude stays unsigned.
inline uint64_t neg_w(uint64_t v, int w) { return (0 - v) & mask(w); }
inline Val negVal(const Val& a) {
    if (a.w <= 64 && a.hi.empty()) return make(neg_w(a.v, a.w), 0, a.w, 0, a.sgn);
    Val z = make(0, 0, a.w, 0, a.sgn);
    return addWide(z, a, true);
}
inline int cmpMag(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    for (int i = (int)a.size() - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}
inline void subMag(std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    uint64_t br = 0;
    for (size_t i = 0; i < a.size(); i++) {
        unsigned __int128 aa = a[i];
        unsigned __int128 bb = (unsigned __int128)b[i] + br;
        if (aa < bb) {
            a[i] = (uint64_t)(aa + ((unsigned __int128)1 << 64) - bb);
            br = 1;
        } else {
            a[i] = (uint64_t)(aa - bb);
            br = 0;
        }
    }
}
// Restoring division of two w-bit magnitudes. Remainder has one extra limb
// so a left shift at a multiple of 64 bits does not lose the top bit.
inline void divMag(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b, int w,
                   std::vector<uint64_t>& q, std::vector<uint64_t>& r) {
    int n = (int)a.size();
    q.assign(n, 0);
    std::vector<uint64_t> rem(n + 1, 0), divs(n + 1, 0);
    for (int i = 0; i < n; i++) divs[i] = i < (int)b.size() ? b[i] : 0;
    for (int i = w - 1; i >= 0; i--) {
        uint64_t c = 0;
        for (int k = 0; k <= n; k++) {
            uint64_t nc = rem[k] >> 63;
            rem[k] = (rem[k] << 1) | c;
            c = nc;
        }
        if ((a[i >> 6] >> (i & 63)) & 1) rem[0] |= 1;
        if (cmpMag(rem, divs) >= 0) {
            subMag(rem, divs);
            q[i >> 6] |= 1ULL << (i & 63);
        }
    }
    r.assign(rem.begin(), rem.begin() + n);
    uint64_t m = limbMask(w, n - 1);
    q[n - 1] &= m;
    r[n - 1] &= m;
}
inline std::vector<uint64_t> magOf(const Val& a) {
    int n = limbCount(a.w);
    std::vector<uint64_t> m(n);
    for (int i = 0; i < n; i++) m[i] = limbV(a, i) & limbMask(a.w, i);
    return m;
}
inline Val fromMag(const std::vector<uint64_t>& m, int w, bool sgn) {
    Val a;
    a.w = w;
    a.sgn = sgn;
    int n = limbCount(w);
    if (n > 1) a.hi.resize(n - 1);
    for (int i = 0; i < n; i++) setLimb(a, i, i < (int)m.size() ? m[i] : 0, 0, 0);
    return finish(a);
}
inline Val v_divmod(Val a, Val b, bool rem) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b) || isZero(b)) return xfill(a.w, a.sgn);
    if (a.w <= 64) {
        uint64_t av = a.v, bv = b.v;
        bool nq = false, nr = false;
        if (a.sgn) {
            uint64_t sign = 1ULL << (a.w >= 64 ? 63 : a.w - 1);
            if (av & sign) { av = neg_w(av, a.w); nq = nr = true; }
            if (bv & sign) { bv = neg_w(bv, a.w); nq = !nq; }
        }
        uint64_t bits = rem ? av % bv : av / bv;
        if (rem ? nr : nq) bits = neg_w(bits, a.w);
        return make(bits, 0, a.w, 0, a.sgn);
    }
    bool nq = false, nr = false;
    Val av = a, bv = b;
    if (a.sgn) {
        if (bitAt(a, a.w - 1) == 1) { av = negVal(a); nq = nr = true; }
        if (bitAt(b, b.w - 1) == 1) { bv = negVal(b); nq = !nq; }
    }
    std::vector<uint64_t> q, r;
    divMag(magOf(av), magOf(bv), a.w, q, r);
    Val out = fromMag(rem ? r : q, a.w, a.sgn);
    if (rem ? nr : nq) out = negVal(out);
    out.sgn = a.sgn;
    return out;
}
inline Val v_div(Val a, Val b) { return v_divmod(a, b, false); }
inline Val v_mod(Val a, Val b) { return v_divmod(a, b, true); }

// ---- comparison (1-bit result) ----
inline bool sameBits(const Val& a, const Val& b) {
    if (a.v != b.v || a.x != b.x || a.z != b.z) return false;
    size_t n = a.hi.size() > b.hi.size() ? a.hi.size() : b.hi.size();
    for (size_t i = 0; i < n; i++) {
        Limb A = i < a.hi.size() ? a.hi[i] : Limb{};
        Limb B = i < b.hi.size() ? b.hi[i] : Limb{};
        if (!(A == B)) return false;
    }
    return true;
}
inline Val v_eq(Val a, Val b) {
    align_signed(a, b);  // sign-extend only when both sides are signed
    if (a.w <= 64 && a.hi.empty() && b.hi.empty()) {
        if ((a.v ^ b.v) & ~a.x & ~b.x) return mk(0);  // a known bit differs
        return (a.x || b.x) ? allx(1) : mk(1);
    }
    bool xz = false;
    int n = limbCount(a.w);
    for (int i = 0; i < n; i++) {
        uint64_t m = limbMask(a.w, i);
        uint64_t av = limbV(a, i), ax = limbX(a, i), bv = limbV(b, i), bx = limbX(b, i);
        if (((av ^ bv) & ~ax & ~bx) & m) return mk(0);
        if ((ax | bx) & m) xz = true;
    }
    return xz ? allx(1) : mk(1);
}
inline Val v_ne(Val a, Val b) { Val e = v_eq(a, b); return e.x ? e : mk(!e.v); }
inline Val v_case_eq(Val a, Val b) { return mk(sameBits(a, b)); }
// casez: Z bits on either side are wildcards. casex: X and Z bits are wildcards.
// Remaining bits compare exactly (an X outside the wildcards must match X).
inline Val v_case_wild(Val a, Val b, bool casex) {
    if (a.w <= 64 && b.w <= 64 && a.hi.empty() && b.hi.empty()) {
        if (casex) {
            uint64_t care = ~(a.x | b.x);
            return mk(((a.v ^ b.v) & care) == 0);
        }
        uint64_t care = ~(a.z | b.z);
        return mk((((a.v ^ b.v) | (a.x ^ b.x)) & care) == 0);
    }
    int w = a.w > b.w ? a.w : b.w;
    int n = limbCount(w);
    for (int i = 0; i < n; i++) {
        uint64_t m = limbMask(w, i);
        uint64_t av = limbV(a, i), ax = limbX(a, i), az = limbZ(a, i);
        uint64_t bv = limbV(b, i), bx = limbX(b, i), bz = limbZ(b, i);
        uint64_t care = casex ? ~(ax | bx) : ~(az | bz);
        uint64_t diff = casex ? (av ^ bv) : ((av ^ bv) | (ax ^ bx));
        if ((diff & care) & m) return mk(0);
    }
    return mk(1);
}
inline Val v_casez_eq(Val a, Val b) { return v_case_wild(a, b, false); }
inline Val v_casex_eq(Val a, Val b) { return v_case_wild(a, b, true); }
inline double v_real(const Val& a) {
    if (a.real) {
        double d;
        std::memcpy(&d, &a.v, 8);
        return d;
    }
    // an integer used as a real. X and Z bits count as 0.
    if (a.w <= 64 && a.hi.empty()) {
        uint64_t v = a.v & ~a.x;
        if (a.sgn && a.w < 64 && a.w > 0 && ((v >> (a.w - 1)) & 1)) v |= ~mask(a.w);
        return a.sgn ? (double)(int64_t)v : (double)v;
    }
    int n = limbCount(a.w);
    std::vector<uint64_t> m(n);
    for (int i = 0; i < n; i++) m[i] = (limbV(a, i) & ~limbX(a, i)) & limbMask(a.w, i);
    bool neg = false;
    if (a.sgn && a.w > 0 && ((m[n - 1] >> ((a.w - 1) & 63)) & 1)) {
        neg = true;
        uint64_t c = 1;
        for (int i = 0; i < n; i++) {
            unsigned __int128 s = (unsigned __int128)(~m[i]) + c;
            m[i] = (uint64_t)s;
            c = (uint64_t)(s >> 64);
        }
        m[n - 1] &= limbMask(a.w, n - 1);
    }
    double acc = 0, scale = 1;
    for (int i = 0; i < n; i++) {
        acc += (double)m[i] * scale;
        scale *= 18446744073709551616.0;
    }
    return neg ? -acc : acc;
}
inline Val mkreal(double d) {
    Val r;
    std::memcpy(&r.v, &d, 8);
    r.x = 0; r.z = 0; r.w = 64; r.real = true;
    return r;
}
// Real to integer (6.12.1): round to nearest, ties away from zero.
inline Val v_rtoi(const Val& a, int w, bool sgn, bool trunc) {
    double d = v_real(a);
    if (std::isnan(d)) return allx(w);
    double r = trunc ? std::trunc(d) : std::round(d);
    if (w <= 64) {
        uint64_t bits = r < 0 ? (uint64_t)(int64_t)r : (r >= 18446744073709551616.0 ? ~0ULL : (uint64_t)r);
        Val v = make(bits, 0, w);
        v.sgn = sgn;
        return v;
    }
    bool neg = r < 0;
    double mag = neg ? -r : r;
    Val out = make(0, 0, w, 0, sgn);
    if (!std::isfinite(mag)) return out;
    int n = limbCount(w);
    for (int i = 0; i < n && mag >= 1; i++) {
        double chunk = std::fmod(mag, 18446744073709551616.0);
        setLimb(out, i, (uint64_t)chunk, 0, 0);
        mag = std::floor(mag / 18446744073709551616.0);
    }
    out = finish(out);
    out.sgn = sgn;
    if (neg) out = negVal(out);
    out.sgn = sgn;
    return out;
}
enum MathFn { M_LN, M_LOG10, M_EXP, M_SQRT, M_POW, M_FLOOR, M_CEIL, M_SIN, M_COS, M_TAN, M_ASIN, M_ACOS,
              M_ATAN, M_ATAN2, M_HYPOT, M_SINH, M_COSH, M_TANH, M_ASINH, M_ACOSH, M_ATANH,
              M_F32 };   // M_F32: round to shortreal (6.12), not a user function
inline double mathFn(int f, double a, double b) {
    switch (f) {
        case M_LN: return std::log(a);       case M_LOG10: return std::log10(a);
        case M_EXP: return std::exp(a);      case M_SQRT: return std::sqrt(a);
        case M_POW: return std::pow(a, b);   case M_FLOOR: return std::floor(a);
        case M_CEIL: return std::ceil(a);    case M_SIN: return std::sin(a);
        case M_COS: return std::cos(a);      case M_TAN: return std::tan(a);
        case M_ASIN: return std::asin(a);    case M_ACOS: return std::acos(a);
        case M_ATAN: return std::atan(a);    case M_ATAN2: return std::atan2(a, b);
        case M_HYPOT: return std::hypot(a, b); case M_SINH: return std::sinh(a);
        case M_COSH: return std::cosh(a);    case M_TANH: return std::tanh(a);
        case M_ASINH: return std::asinh(a);  case M_ACOSH: return std::acosh(a);
        case M_ATANH: return std::atanh(a);
        case M_F32: return (double)(float)a;
    }
    return 0;
}
struct StrPool {
    std::vector<std::string> text{""};
    std::unordered_map<std::string, uint64_t> id{{"", 0}};
    uint64_t of(const std::string& s) {
        auto [it, fresh] = id.try_emplace(s, text.size());
        if (fresh) text.push_back(s);
        return it->second;
    }
};
inline StrPool& strPool() { static StrPool p; return p; }
inline Val mkstr(const std::string& s) {
    Val r = make(strPool().of(s), 0, 64);
    r.str = true;
    return r;
}
inline const std::string& v_str(const Val& a) {
    static const std::string empty;
    if (!a.str || a.x || a.v >= strPool().text.size()) return empty;
    return strPool().text[a.v];
}
inline std::string bytesToStr(const Val& a) {   // integral to string: 0 bytes dropped
    std::string s;
    for (int b = (a.w + 7) / 8 - 1; b >= 0; b--) {
        unsigned char c = 0;
        if (a.w <= 64) c = (unsigned char)(((a.v & ~a.x) >> (8 * b)) & 0xff);
        else for (int k = 0; k < 8; k++) if (bitAt(a, 8 * b + k) == 1) c |= (unsigned char)(1u << k);
        if (c) s += (char)c;
    }
    return s;
}
// Decimal magnitude of a known value. A single word uses the host conversion.
inline std::string uToDec(const Val& a) {
    if (a.w <= 64 && a.hi.empty()) return std::to_string(a.v & mask(a.w));
    int n = limbCount(a.w);
    std::vector<uint64_t> d(n);
    bool z = true;
    for (int i = 0; i < n; i++) {
        d[i] = limbV(a, i) & limbMask(a.w, i);
        if (d[i]) z = false;
    }
    if (z) return "0";
    std::string s;
    for (;;) {
        bool az = true;
        for (uint64_t x : d) if (x) { az = false; break; }
        if (az) break;
        uint64_t rem = 0;
        for (int i = n - 1; i >= 0; i--) {
            unsigned __int128 cur = ((unsigned __int128)rem << 64) | d[i];
            d[i] = (uint64_t)(cur / 10);
            rem = (uint64_t)(cur % 10);
        }
        s.push_back(char('0' + rem));
    }
    for (size_t i = 0; i < s.size() / 2; i++) {
        char t = s[i];
        s[i] = s[s.size() - 1 - i];
        s[s.size() - 1 - i] = t;
    }
    return s;
}
inline void countXZ(const Val& a, int& nx, int& nz) {
    nx = nz = 0;
    if (a.w <= 0) return;
    for (int i = 0; i < limbCount(a.w); i++) {
        uint64_t m = limbMask(a.w, i);
        uint64_t x = limbX(a, i) & m, z = limbZ(a, i) & m;
        nz += __builtin_popcountll(z);
        nx += __builtin_popcountll(x & ~z);
    }
}
// A delay. X/Z is 0. A magnitude past 64 bits saturates.
inline uint64_t v_time(const Val& v) {
    if (v.x) return 0;
    for (const auto& h : v.hi) if (h.x) return 0;
    for (const auto& h : v.hi) if (h.v) return ~0ULL;
    return v.v;
}
enum StrFn { S_LEN, S_GETC, S_TOUPPER, S_TOLOWER, S_COMPARE, S_ICOMPARE, S_SUBSTR, S_ATOI, S_ATOHEX,
             S_ATOOCT, S_ATOBIN, S_ATOREAL, S_PUTC, S_ITOA, S_HEXTOA, S_OCTTOA, S_BINTOA, S_REALTOA };
// The hook $sformatf uses: the kernel's format engine (set by the kernel).
using Reader = std::function<Val(int)>;
struct Expr;
using E = std::shared_ptr<Expr>;
inline std::string (*g_sformat)(const std::string&, const std::vector<E>&, const Reader&) = nullptr;
// The contents of every dynamic container, by handle signal. A change bumps
// the handle's value, so a process waiting on the container wakes.
inline int64_t v_to_i64(Val v);
struct DynKey {
    int64_t i = 0;     // integral key (unsigned keys have the top bit flipped, so order holds)
    std::string s;     // string key
    bool operator<(const DynKey& o) const { return s != o.s ? s < o.s : i < o.i; }
};
struct DynObj {
    int kind = 0;               // 0 dynamic array, 1 queue, 2 associative array
    std::vector<Val> vec;
    std::map<DynKey, Val> map;
    Val dflt;                   // what a missing element reads as
    int64_t bound = -1;         // queue [$:N]: at most N + 1 elements
    bool strKey = false;        // associative: string index
    int keyW = 64;
    bool keySgn = true;
    DynKey key(const Val& k) const {
        DynKey d;
        if (strKey) {   // a signal holds a string as its pool id, without the flag
            Val t = k;
            t.str = true;
            d.s = v_str(t);
            return d;
        }
        uint64_t u = k.v & ~k.x & mask(keyW);
        if (keySgn && keyW < 64 && ((u >> (keyW - 1)) & 1)) u |= ~mask(keyW);
        d.i = keySgn ? (int64_t)u : (int64_t)(u ^ (1ULL << 63));
        return d;
    }
    Val keyVal(const DynKey& d) const {
        if (strKey) return mkstr(d.s);
        uint64_t u = keySgn ? (uint64_t)d.i : ((uint64_t)d.i ^ (1ULL << 63));
        Val v = make(u, 0, keyW);
        v.sgn = keySgn;
        return v;
    }
    size_t size() const { return kind == 2 ? map.size() : vec.size(); }
    Val get(const Val& k) const {
        if (kind == 2) {
            if (k.x && !strKey) return dflt;
            auto it = map.find(key(k));
            return it == map.end() ? dflt : it->second;
        }
        if (k.x) return dflt;
        int64_t i = v_to_i64(k);
        if (i < 0 || i >= (int64_t)vec.size()) return dflt;
        return vec[(size_t)i];
    }
};
inline std::unordered_map<int, DynObj>& dynStore() { static std::unordered_map<int, DynObj> m; return m; }
inline int ucmp(const Val& a, const Val& b) {
    int n = limbCount(a.w > b.w ? a.w : b.w);
    for (int i = n - 1; i >= 0; i--) {
        uint64_t av = limbV(a, i) & limbMask(a.w, i);
        uint64_t bv = limbV(b, i) & limbMask(b.w, i);
        if (av != bv) return av < bv ? -1 : 1;
    }
    return 0;
}
inline int scmpWide(const Val& a, const Val& b) {
    int as = bitAt(a, a.w - 1) == 1, bs = bitAt(b, b.w - 1) == 1;
    if (as != bs) return as ? -1 : 1;
    return ucmp(a, b);
}
inline Val v_wild_eq(Val a, Val b) {
    align_signed(a, b);
    if (a.w <= 64 && a.hi.empty() && b.hi.empty()) {
        uint64_t care = ~b.x;
        if ((a.v ^ b.v) & ~a.x & care) return mk(0);
        return (a.x & care) ? allx(1) : mk(1);
    }
    bool xz = false;
    int n = limbCount(a.w);
    for (int i = 0; i < n; i++) {
        uint64_t m = limbMask(a.w, i);
        uint64_t av = limbV(a, i), ax = limbX(a, i), bv = limbV(b, i), bx = limbX(b, i);
        uint64_t care = ~bx & m;
        if ((av ^ bv) & ~ax & care) return mk(0);
        if (ax & care) xz = true;
    }
    return xz ? allx(1) : mk(1);
}
inline Val v_lt(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b)) return allx(1);
    if (a.w <= 64) {
        if (a.sgn) return mk(scmp(a.v, b.v, a.w) < 0);
        return mk(a.v < b.v);
    }
    if (a.sgn) return mk(scmpWide(a, b) < 0);
    return mk(ucmp(a, b) < 0);
}
inline Val v_le(Val a, Val b) {
    align_signed(a, b);
    if (a.x || b.x || anyX(a) || anyX(b)) return allx(1);
    if (a.w <= 64) {
        if (a.sgn) return mk(scmp(a.v, b.v, a.w) <= 0);
        return mk(a.v <= b.v);
    }
    if (a.sgn) return mk(scmpWide(a, b) <= 0);
    return mk(ucmp(a, b) <= 0);
}
inline Val v_gt(Val a, Val b) { return v_lt(b, a); }
inline Val v_ge(Val a, Val b) { return v_le(b, a); }

// Logical << / >>. Vacated bits are 0. Z that remains in range stays Z.
// Any X or Z in the count (z is part of x) -> all X. The result keeps the
// left operand's signedness so a later widen can sign-extend it.
// v_asr (>>>) sign-fills when the left operand is signed, else it is v_shr.
// A count with a limb above bit 63 set is at least 2^64, which is past any
// width that fits in an int, so the shifted word is all vacated bits.
inline bool shiftUnknown(const Val& n) {
    if (n.x) return true;
    for (const auto& h : n.hi) if (h.x) return true;
    return false;
}
inline bool shiftHuge(const Val& n) {
    for (const auto& h : n.hi) if (h.v) return true;
    return false;
}
inline Val shlBits(const Val& a, int sh) {
    if (sh <= 0) return a;
    if (sh >= a.w) return make(0, 0, a.w, 0, a.sgn);
    Val r = make(0, 0, a.w, 0, a.sgn);
    copyBits(r, sh, a, 0, a.w - sh);
    r.sgn = a.sgn;
    return finish(r);
}
inline Val shrBits(const Val& a, int sh) {
    if (sh <= 0) return a;
    if (sh >= a.w) return make(0, 0, a.w, 0, a.sgn);
    Val r = make(0, 0, a.w, 0, a.sgn);
    copyBits(r, 0, a, sh, a.w - sh);
    r.sgn = a.sgn;
    return finish(r);
}
inline Val v_shl(Val a, Val n) {
    if (shiftUnknown(n)) return xfill(a.w, a.sgn);
    if (!shiftHuge(n) && a.w <= 64 && a.hi.empty()) {
        if (n.v >= 64) return make(0, 0, a.w, 0, a.sgn);
        return make(a.v << n.v, a.x << n.v, a.w, a.z << n.v, a.sgn);
    }
    if (shiftHuge(n) || n.v >= (uint64_t)a.w) return make(0, 0, a.w, 0, a.sgn);
    return shlBits(a, (int)n.v);
}
inline Val v_shr(Val a, Val n) {
    if (shiftUnknown(n)) return xfill(a.w, a.sgn);
    if (!shiftHuge(n) && a.w <= 64 && a.hi.empty()) {
        if (n.v >= 64) return make(0, 0, a.w, 0, a.sgn);
        return make(a.v >> n.v, a.x >> n.v, a.w, a.z >> n.v, a.sgn);
    }
    if (shiftHuge(n) || n.v >= (uint64_t)a.w) return make(0, 0, a.w, 0, a.sgn);
    return shrBits(a, (int)n.v);
}
inline Val v_asr(Val a, Val n) {
    if (!a.sgn) return v_shr(a, n);
    if (shiftUnknown(n)) return xfill(a.w, true);
    if (!shiftHuge(n) && a.w <= 64 && a.hi.empty()) {
        if (n.v == 0) return a;
        uint64_t sb = 1ULL << (a.w - 1);
        uint64_t m = mask(a.w);
        if (n.v >= (uint64_t)a.w) {
            return make((a.v & sb) ? m : 0, (a.x & sb) ? m : 0, a.w, (a.z & sb) ? m : 0, true);
        }
        Val r = v_shr(a, n);
        uint64_t vac = mask((int)n.v) << (a.w - (int)n.v);
        return make(r.v | ((a.v & sb) ? vac : 0), r.x | ((a.x & sb) ? vac : 0), a.w,
                    r.z | ((a.z & sb) ? vac : 0), true);
    }
    int sh = (shiftHuge(n) || n.v >= (uint64_t)a.w) ? a.w : (int)n.v;
    if (sh == 0) return a;
    int sb = a.w > 0 ? bitAt(a, a.w - 1) : 0;
    Val r = sh >= a.w ? make(0, 0, a.w, 0, true) : shrBits(a, sh);
    int from = sh >= a.w ? 0 : a.w - sh;
    for (int i = from; i < a.w; i++) setBit(r, i, sb);
    r.sgn = true;
    return finish(r);
}

// ---- reductions and logical ops ----
inline Val v_red_and(Val a) {
    if (a.w <= 64 && a.hi.empty()) {
        if (mask(a.w) & ~a.x & ~a.v) return mk(0);  // some known 0
        return a.x ? allx(1) : mk(1);
    }
    int n = limbCount(a.w);
    for (int i = 0; i < n; i++) {
        uint64_t m = limbMask(a.w, i);
        if (m & ~limbX(a, i) & ~limbV(a, i)) return mk(0);
    }
    return anyX(a) ? allx(1) : mk(1);
}
inline Val v_red_or(Val a) {
    if (a.w <= 64 && a.hi.empty()) return a.v ? mk(1) : (a.x ? allx(1) : mk(0));
    return anyV(a) ? mk(1) : (anyX(a) ? allx(1) : mk(0));
}
inline Val v_red_xor(Val a) {
    if (a.w <= 64 && a.hi.empty()) return a.x ? allx(1) : mk(__builtin_parityll(a.v));
    if (anyX(a)) return allx(1);
    int p = 0;
    for (int i = 0; i < limbCount(a.w); i++) p ^= __builtin_parityll(limbV(a, i) & limbMask(a.w, i));
    return mk(p);
}
inline Val v_lnot(Val a) { int t = truth(a); return t == 2 ? allx(1) : mk(!t); }
inline Val v_land(Val a, Val b) {
    int p = truth(a), q = truth(b);
    if (p == 0 || q == 0) return mk(0);
    return (p == 1 && q == 1) ? mk(1) : allx(1);
}
inline Val v_lor(Val a, Val b) {
    int p = truth(a), q = truth(b);
    if (p == 1 || q == 1) return mk(1);
    return (p == 0 && q == 0) ? mk(0) : allx(1);
}

// a[i] with a run-time index; out-of-range or unknown index -> X
inline Val v_index(Val a, Val i) {
    if (shiftUnknown(i) || shiftHuge(i) || i.v >= (uint64_t)a.w) return allx(1);
    if (a.w <= 64 && a.hi.empty()) return make(a.v >> i.v, a.x >> i.v, 1, a.z >> i.v);
    return v_slice(a, (int)i.v, (int)i.v);
}
// base with bits [hi:lo] replaced by `part` (for a[hi:lo] = ... style assignments)
inline Val v_insert(Val base, Val part, int hi, int lo) {
    if (base.w <= 64 && base.hi.empty() && hi < 64 && lo >= 0 && lo < 64) {
        uint64_t m = mask(hi - lo + 1) << lo;
        uint64_t pv = part.v << lo;
        uint64_t px = part.x << lo;
        uint64_t pz = part.z << lo;
        return make((base.v & ~m) | (pv & m), (base.x & ~m) | (px & m), base.w, (base.z & ~m) | (pz & m));
    }
    if (hi < lo || lo < 0) return base;
    Val r = base;
    if (limbCount(r.w) > 1 && (int)r.hi.size() < limbCount(r.w) - 1) r.hi.resize(limbCount(r.w) - 1);
    int pw = hi - lo + 1;
    int n = pw;
    if (lo + n > r.w) n = r.w - lo;
    if (n > 0) copyBits(r, lo, part, 0, n);
    return finish(r);
}

// ---- slice a[hi:lo], concat {a,b,...} (first = most significant), ?: ----
inline Val v_slice(Val a, int hi, int lo) {
    int w = hi - lo + 1;
    if (w <= 0) return make(0, 0, 1);
    if (a.hi.empty() && a.w <= 64 && lo >= 0 && lo < 64 && hi < 64)
        return make(a.v >> lo, a.x >> lo, w, a.z >> lo);
    // Bits outside the vector are X. In-range bits are copied over that.
    Val r = allx(w);
    if (lo < 0 || lo >= a.w) return r;
    int n = w;
    if (lo + n > a.w) n = a.w - lo;
    copyBits(r, 0, a, lo, n);
    return finish(r);
}
// w bits of a starting at bit lo; a bit outside a reads x (11.5.1).
inline Val v_dynslice(Val a, int64_t lo, int w) {
    if (w <= 64 && a.w <= 64 && a.hi.empty()) {
        uint64_t v = 0, x = 0, z = 0;
        for (int b = 0; b < w; b++) {
            int64_t s = lo + b;
            if (s < 0 || s >= a.w) { x |= 1ULL << b; continue; }
            v |= ((a.v >> s) & 1) << b;
            x |= ((a.x >> s) & 1) << b;
            z |= ((a.z >> s) & 1) << b;
        }
        return make(v, x, w, z);
    }
    if (w <= 0) return allx(1);
    Val r = allx(w);
    for (int b = 0; b < w; b++) {
        int64_t s = lo + b;
        if (s < 0 || s >= a.w) continue;
        setBit(r, b, bitAt(a, (int)s));
    }
    return finish(r);
}
inline Val v_concat(const std::vector<Val>& parts) {
    int total = 0;
    for (auto& p : parts) total += p.w;
    if (total <= 64) {
        Val r{0, 0, 0};
        for (auto& p : parts) {
            uint64_t rv = p.w >= 64 ? 0 : r.v << p.w, rx = p.w >= 64 ? 0 : r.x << p.w,
                     rz = p.w >= 64 ? 0 : r.z << p.w;
            r = make(rv | p.v, rx | p.x, r.w + p.w, rz | p.z);
        }
        return r;
    }
    Val r = make(0, 0, total);
    int off = 0;
    for (int i = (int)parts.size() - 1; i >= 0; i--) {
        if (parts[i].w <= 0) continue;
        r = v_insert(r, parts[i], off + parts[i].w - 1, off);
        off += parts[i].w;
    }
    return r;
}
// X/Z condition: per bit, both arms equal (0, 1, X, or Z) keeps that value;
// otherwise X. A known condition selects that arm unchanged.
inline Val v_mux(Val c, Val a, Val b) {
    int t = truth(c);
    if (t == 1) return a;
    if (t == 0) return b;
    int w = wmax(a, b);
    if (w <= 64 && a.hi.empty() && b.hi.empty()) {
        uint64_t m = mask(w);
        uint64_t same = ~a.x & ~b.x & ~(a.v ^ b.v) & m; // both 0 or both 1
        uint64_t bothz = a.z & b.z & m;
        return make(a.v & same, m & ~same, w, bothz, a.sgn && b.sgn);
    }
    Val r;
    r.w = w;
    r.sgn = a.sgn && b.sgn;
    int n = limbCount(w);
    if (n > 1) r.hi.resize(n - 1);
    for (int i = 0; i < n; i++) {
        uint64_t m = limbMask(w, i);
        uint64_t av = limbV(a, i), ax = limbX(a, i), az = limbZ(a, i);
        uint64_t bv = limbV(b, i), bx = limbX(b, i), bz = limbZ(b, i);
        uint64_t same = ~ax & ~bx & ~(av ^ bv) & m;
        uint64_t bothz = az & bz & m;
        setLimb(r, i, av & same, m & ~same, bothz);
    }
    return finish(r);
}

// IEEE 1364 drive strength, weakest to strongest. A driver has one level for
// the 0 it can drive and one for the 1. HighZ contributes nothing.
enum : uint8_t {
    STR_HIZ = 0,
    STR_SMALL = 1,
    STR_MEDIUM = 2,
    STR_WEAK = 3,
    STR_LARGE = 4,
    STR_PULL = 5,
    STR_STRONG = 6,
    STR_SUPPLY = 7
};

// One continuous driver. s0 is the strength of a driven 0, s1 of a driven 1.
// The resolved net value is still a plain Val. Strength stays on the driver.
struct Drv {
    Val v;
    uint8_t s0 = STR_STRONG;
    uint8_t s1 = STR_STRONG;
};

// One bit of a resolved net, in the form Icarus Verilog uses.
// A 0 or a 1 is unambiguous: both strengths are that side's strength, and
// both flags match. An X keeps s0 and s1 apart and stays X, even when one
// strength is larger. Z is no drive. Combining bits takes the stronger
// unambiguous value; equal strengths on opposite values become X.
struct SBit {
    unsigned s0 = 0, s1 = 0;
    bool f0 = false, f1 = false;
};
inline bool sbit_z(SBit a) { return a.s0 == 0 && a.s1 == 0; }
inline bool sbit_unamb(SBit a) { return a.s0 == a.s1 && a.f0 == a.f1; }
inline SBit sbit_0(unsigned s) { return s ? SBit{s, s, false, false} : SBit{}; }
inline SBit sbit_1(unsigned s) { return s ? SBit{s, s, true, true} : SBit{}; }
inline SBit sbit_x(unsigned s0, unsigned s1) {
    if (s0 == 0 && s1 == 0) return {};
    return SBit{s0, s1, false, true};
}
inline int sbit_logic(SBit a) {
    if (sbit_z(a)) return 3;
    if (!a.f0 && !a.f1) return 0;
    if (a.f0 && a.f1) return 1;
    return 2;
}
inline SBit sbit_resolv(SBit a, SBit b) {
    if (sbit_z(a)) return b;
    if (sbit_z(b)) return a;
    if (a.s0 == b.s0 && a.s1 == b.s1 && a.f0 == b.f0 && a.f1 == b.f1) return a;
    if (sbit_unamb(a) && sbit_unamb(b)) {
        if (b.s0 > a.s0) return b;
        if (b.s0 == a.s0) return sbit_x(a.s0, a.s0);
        return a;
    }
    if (sbit_unamb(a) || sbit_unamb(b)) {
        SBit u = sbit_unamb(a) ? a : b;
        SBit m = sbit_unamb(a) ? b : a;
        SBit r;
        if (u.s1 > m.s1) { r.s1 = u.s1; r.f1 = u.f1; }
        else { r.s1 = m.s1; r.f1 = m.f1; }
        if (u.s0 > m.s0) { r.s0 = u.s0; r.f0 = u.f0; }
        else { r.s0 = m.s0; r.f0 = m.f0; }
        return r;
    }
    int sv1a = a.f1 ? (int)a.s1 : -(int)a.s1;
    int sv0a = a.f0 ? (int)a.s0 : -(int)a.s0;
    int sv1b = b.f1 ? (int)b.s1 : -(int)b.s1;
    int sv0b = b.f0 ? (int)b.s0 : -(int)b.s0;
    int sv1 = sv1a, sv0 = sv0a;
    if (sv0a > sv1) sv1 = sv0a;
    if (sv1b > sv1) sv1 = sv1b;
    if (sv0b > sv1) sv1 = sv0b;
    if (sv1a < sv0) sv0 = sv1a;
    if (sv1b < sv0) sv0 = sv1b;
    if (sv0b < sv0) sv0 = sv0b;
    SBit r;
    if (sv1 > 0) { r.f1 = true; r.s1 = (unsigned)sv1; }
    else { r.f1 = a.f1 && b.f1; r.s1 = (unsigned)(-sv1); }
    if (sv0 > 0) { r.f0 = true; r.s0 = (unsigned)sv0; }
    else r.s0 = (unsigned)(-sv0);
    if (sbit_z(r)) return {};
    return r;
}

// Net types that resolve differently from wire and tri (6.6.3-6.6.6).
enum NetResolve : uint8_t { NR_WIRE, NR_WAND, NR_WOR, NR_TRI0, NR_TRI1, NR_TRIREG, NR_SUPPLY0, NR_SUPPLY1 };
// The value of a net of kind k from its drivers; `prev` is its current value,
// which a trireg keeps on bits no driver drives (charge storage, 6.6.4).
inline Val resolveDrives(const Drv* d, size_t n, int w);
inline Val resolveNet(uint8_t k, const Drv* d, size_t n, int w, const Val& prev) {
    Val r = make(0, 0, w);
    for (int b = 0; b < w; b++) {
        int t = 3;
        if (k == NR_SUPPLY0) t = 0;
        else if (k == NR_SUPPLY1) t = 1;
        else if (k == NR_WAND || k == NR_WOR) {   // wired AND / OR (6.6.3): z drivers do not take part
            bool any0 = false, any1 = false, anyx = false;
            for (size_t i = 0; i < n; i++) {
                int v = bitAt(d[i].v, b);
                if (v == 0) any0 = true; else if (v == 1) any1 = true; else if (v == 2) anyx = true;
            }
            if (k == NR_WAND) t = any0 ? 0 : anyx ? 2 : any1 ? 1 : 3;
            else t = any1 ? 1 : anyx ? 2 : any0 ? 0 : 3;
        } else {
            t = 3;
            if (n) t = bitAt(resolveDrives(d, n, w), b);
            if (t == 3) t = k == NR_TRI0 ? 0 : k == NR_TRI1 ? 1 : k == NR_TRIREG ? bitAt(prev, b) : 3;
        }
        setBit(r, b, t);
    }
    return r;
}
inline Val resolveDrives(const Drv* d, size_t n, int w) {
    if (w > 64) {
        Val r = make(0, 0, w);
        for (int b = 0; b < w; b++) {
            SBit acc;
            for (size_t i = 0; i < n; i++) {
                int t = bitAt(d[i].v, b);
                SBit cur = t == 3 ? SBit{} : t == 2 ? sbit_x(d[i].s0, d[i].s1)
                         : t == 1 ? sbit_1(d[i].s1) : sbit_0(d[i].s0);
                acc = sbit_resolv(acc, cur);
            }
            int lg = sbit_logic(acc);
            if (lg == 1) setBit(r, b, 1);
            else if (lg == 2) setBit(r, b, 2);
            else if (lg == 3) setBit(r, b, 3);
        }
        return finish(r);
    }
    uint64_t rv = 0, rx = 0, rz = 0;
    for (int b = 0; b < w && b < 64; b++) {
        uint64_t bit = 1ULL << b;
        SBit acc;
        for (size_t i = 0; i < n; i++) {
            SBit cur;
            if (d[i].v.z & bit) cur = {};
            else if (d[i].v.x & bit) cur = sbit_x(d[i].s0, d[i].s1);
            else if (d[i].v.v & bit) cur = sbit_1(d[i].s1);
            else cur = sbit_0(d[i].s0);
            acc = sbit_resolv(acc, cur);
        }
        int lg = sbit_logic(acc);
        if (lg == 1) rv |= bit;
        else if (lg == 2) rx |= bit;
        else if (lg == 3) { rx |= bit; rz |= bit; }
    }
    return make(rv, rx, w, rz);
}

// ================= expression tree =================
enum class Op {
    Const, Ref,
    Not, And, Or, Xor, Add, Sub, Mul, Div, Mod,
    Eq, Ne, CaseEq, CaseZEq, CaseXEq, Lt, Le, Gt, Ge, Shl, Shr, Asr, Resize,
    LogNot, LogAnd, LogOr, RedAnd, RedOr, RedXor,
    Slice, Index, Concat, Mux,
    Pow, Time,
    WildEq,    // a ==? b: x / z bits of b match anything (11.4.6)
    // real (6.12). Values are doubles in v, with Val::real set.
    FAdd, FSub, FMul, FDiv, FPow, FNeg,
    FCmp,      // hi: 0 <, 1 <=, 2 >, 3 >=, 4 ==, 5 !=; 1-bit result
    IToR,      // integer (signed if its Val is) to real; x and z bits count as 0
    RToI,      // real to an integer of hi bits, rounded (lo = 1: truncated, $rtoi)
    AsReal,    // the 64 bits of kid as a real ($bitstoreal, a real variable)
    FMath,     // hi: a math function id (20.8), one or two kids
    RealTime,  // $realtime: k.v ticks per unit
    // string (6.16). A string value is an id into strPool(); equal text has
    // one id, so == and a change on a string signal compare ids.
    AsStr,     // kid's bits are a string id
    IntToStr,  // the bytes of kid, leading zero bytes dropped (6.16)
    StrToInt,  // the text as bytes, the last char in the low byte, hi bits
    StrCat,    // {a, b, ...}
    StrRep,    // {hi{a}}
    StrCmp,    // hi as FCmp: <, <=, >, >=, ==, !=
    StrMethod, // hi: StrFn; kid 0 is the string
    SFormat,   // $sformatf: fmt, then kids as the arguments
    // Dynamic arrays, queues, associative arrays (7.5, 7.10, 7.8). sig is the
    // container's handle signal; its contents live in dynStore().
    DynGet,    // element kids[0] (index or key); a missing one reads the default
    DynSize,   // size() / num()
    DynExists, // exists(kids[0])
    DynReduce, // hi: 0 sum, 1 product, 2 and, 3 or, 4 xor (7.12.3), at k.w bits
    ArrRead,   // element of an unpacked array; kids are one index per dimension
    DynSlice,  // a[i +: w] / a[i -: w] with i known at run time: hi = w, lo = 1 for -:
    SysFn,     // hi: a SysFnId; kids are its arguments
    // Streaming with containers (11.4.14). DynBits: the elements of container
    // sig packed, the first on top; its width is known only at run time, and
    // it only appears inside StreamPack. StreamPack: kids concatenated at
    // their run-time widths, then cut into lo-bit slices from the right and
    // reversed when lo > 0, then left-aligned into hi bits (hi < 0: as is).
    DynBits, StreamPack,
    DynHas,    // kids[0] inside container sig (11.4.13): 1 if an element matches with ==?, x if none does but one may
    // Classes (8). A handle is the first signal id of its object (0: null).
    This,      // the running method's `this`
    IsA,       // 1 if kids[0] is null or an object of class hi or a class derived from it ($cast, 8.16)
    Addr,      // the signal id of slot `sig` of the object kids[0] names, or 0 for null
    AddrOf     // the signal id that sig (maybe relative) names now: a `ref` argument (13.5.2)
};
// System functions computed by Op::SysFn.
enum SysFnId : int {
    SF_CLOG2,        // $clog2 (20.8.1)
    SF_COUNTBITS,    // $countbits (20.9): lo is a mask of 1 << {0, 1, x=2, z=3}
    SF_RANDOM,       // $random with no seed (20.14.1): the simulator's own seed
    SF_RANDOM_SEED,  // $random(seed): {new seed, value}, 64 bits
    SF_DIST_UNIFORM, // $dist_uniform(seed, start, end): {new seed, value}
    SF_URANDOM,      // $urandom (18.13.1)
    SF_URANDOM_SEED, // $urandom(seed)
    SF_URANDOM_RANGE, // $urandom_range(max, min) (18.13.2)
    SF_PLUS_TEST,    // $test$plusargs(prefix) (21.6); also "is there a match" for $value$plusargs
    SF_PLUS_VALUE,   // the text after prefix kid0, read with format letter lo
    SF_TIMEFORMAT,   // $timeformat(units, precision, suffix, width) (20.4.3); returns 0
    // File I/O (21.3). A descriptor with bit 31 set is a file (0x80000000-2 are
    // stdin, stdout, stderr); otherwise each set bit is a channel (bit 0 stdout).
    SF_FOPEN,        // $fopen(name [, mode]): lo = 1 when a mode is given
    SF_FCLOSE,       // $fclose(fd)
    SF_FWRITE,       // write string kid1 to fd kid0
    SF_FFLUSH,       // $fflush([fd])
    SF_FGETC,        // $fgetc(fd): a byte, or -1 at the end
    SF_UNGETC,       // $ungetc(c, fd)
    SF_FEOF,         // $feof(fd)
    SF_FGETS,        // $fgets: reads a line into scanText(), returns its length
    SF_SCAN,         // $fscanf (lo = 1) / $sscanf (lo = 0): kid0 source, kid1 format;
                     // the values go to scanOut(), returns how many were read
    SF_SCAN_GET,     // scanOut()[kid0]
    SF_SCAN_TEXT,    // scanText() as a string
    SF_FTELL,        // $ftell(fd)
    SF_FSEEK,        // $fseek(fd, offset, whence), $rewind(fd) is fseek(fd, 0, 0)
    SF_FERROR,       // $ferror(fd, str): 0, no error text kept
    SF_SR2BITS,      // $shortrealtobits (20.5)
    SF_BITS2SR,      // $bitstoshortreal
    SF_OBJ_SRANDOM,  // obj.srandom(seed) (18.13.3): kid0 the object
    SF_OBJ_GETSTATE, // obj.get_randstate(): the generator state as a string
    SF_OBJ_SETSTATE, // obj.set_randstate(state)
    SF_CG,           // a covergroup operation (19): lo is the CgOp, run by g_cgHook
    // ---- DPI / remaining-chapters thread ----
    // $dist_* (20.14, Annex N): {new seed, value}, 64 bits, like SF_DIST_UNIFORM.
    SF_DIST_NORMAL,      // (seed, mean, standard deviation)
    SF_DIST_EXPONENTIAL, // (seed, mean)
    SF_DIST_POISSON,     // (seed, mean)
    SF_DIST_CHI_SQUARE,  // (seed, degrees of freedom)
    SF_DIST_T,           // (seed, degrees of freedom)
    SF_DIST_ERLANG,      // (seed, k, mean)
    SF_SYSTEM,           // $system(command) (20.17): the shell's exit status
    SF_ASSOC_BOUND       // $low / $high of associative array `sig` (20.7): lo 0 lowest key, 1 highest; x if empty
};
// Covergroup operations (19.3, 19.8, 19.9), run by the kernel (cover_rt.inc).
enum CgOp : int { CG_NEW, CG_SAMPLE, CG_GETCOV, CG_INSTCOV, CG_START, CG_STOP, CG_SETNAME, CG_ITEMCOV, CG_ITEMINST, CG_GLOBAL, CG_COUNT };
inline Val (*g_cgHook)(int op, const std::vector<Val>& a) = nullptr;
// How %t prints (20.4.3). A %t argument arrives in kernel ticks (10^tickExp s).
// The default units are the simulation precision, with no decimals, no
// suffix, and a minimum width of 20.
struct TimeFmt {
    int tickExp = -9;
    int units = -9;
    int prec = 0;
    std::string suffix;
    int width = 20;
};
inline TimeFmt& timeFmt() { static TimeFmt t; return t; }

// A fixed-size unpacked array (IEEE 1800-2023 7.4) is `count` consecutive
// signals starting at `base`, one per element, in row-major order. Each
// dimension keeps its declared [left:right]; offset 0 is the left bound.
struct ArrShape {
    int base = -1;
    int count = 0;
    int elemW = 1;
    bool sgn = false;
    bool twoState = false;   // bit / int elements: an invalid read is 0, not X (7.4.6)
    std::vector<std::pair<int64_t, int64_t>> dims;   // [left, right]
    // Flat element offset, or -1 when an index is outside its dimension.
    int64_t offset(const std::vector<int64_t>& idx) const {
        int64_t off = 0;
        for (size_t d = 0; d < dims.size(); d++) {
            auto [l, r] = dims[d];
            int64_t n = (l > r ? l - r : r - l) + 1;
            int64_t i = idx[d];
            int64_t o = l <= r ? i - l : l - i;
            if (o < 0 || o >= n) return -1;
            off = off * n + o;
        }
        return off;
    }
    int64_t size(size_t d) const { auto [l, r] = dims[d]; return (l > r ? l - r : r - l) + 1; }
};
struct Expr {
    std::string fmt;  // SFormat
    Op op;
    Val k;            // Const
    int sig = -1;     // Ref
    int hi = 0, lo = 0;  // Slice, or Resize dest width / sign-fill
    bool sgn = false;    // Ref result, or Resize result
    std::vector<E> kids;
    std::shared_ptr<const ArrShape> arr;   // ArrRead
    // Ref of a class property through a handle (h.x): the object is at[0]'s
    // value, and sig is the property's slot in it. Read only when evaluated,
    // so `h != null && h.x` never touches a null h.
    std::shared_ptr<Expr> at;
};

inline E node(Op op, std::vector<E> kids = {}) {
    auto e = std::make_shared<Expr>();
    e->op = op;
    e->kids = std::move(kids);
    return e;
}
inline E konst(Val k) { auto e = node(Op::Const); e->k = k; return e; }
inline E ref(int sig, bool sgn = false) {
    auto e = node(Op::Ref);
    e->sig = sig;
    e->sgn = sgn;
    return e;
}
// Resize to dstW. fill sign-extends when widening; tag is the result's signedness.
// A constant is folded. lo on a Resize node is the sign-fill flag.
inline E resize(E a, int dstW, bool fill, bool tag) {
    if (a && a->op == Op::Const) {
        Val r = v_resize(a->k, dstW, fill && dstW > a->k.w);
        r.sgn = tag;
        return konst(r);
    }
    auto e = node(Op::Resize, {std::move(a)});
    e->hi = dstW;
    e->lo = fill ? 1 : 0;
    e->sgn = tag;
    return e;
}
inline E un(Op op, E a) { return node(op, {a}); }
inline E bin(Op op, E a, E b) { return node(op, {a, b}); }
inline E slice(E a, int hi, int lo) { auto e = node(Op::Slice, {a}); e->hi = hi; e->lo = lo; return e; }
inline E concat(std::vector<E> parts) { return node(Op::Concat, std::move(parts)); }
inline E mux(E c, E a, E b) { return node(Op::Mux, {c, a, b}); }

// Current simulation time, for $time / $stime / $realtime (IEEE 1800 20.3).
// The kernel sets it when time advances.
inline uint64_t g_simTime = 0;

// a ** b (IEEE 1800 11.4.5, table 11-4). The result has a's width and sign.
// Any X/Z operand gives X. A negative exponent gives 0 except for a base of
// 1 (1), -1 (+-1 by parity), and 0 (X).
inline bool knownOne(const Val& a) {
    if (anyX(a)) return false;
    if ((limbV(a, 0) & limbMask(a.w, 0)) != 1) return false;
    for (int i = 1; i < limbCount(a.w); i++)
        if (limbV(a, i) & limbMask(a.w, i)) return false;
    return true;
}
inline bool knownAllOnes(const Val& a) {
    if (anyX(a)) return false;
    for (int i = 0; i < limbCount(a.w); i++)
        if ((limbV(a, i) & limbMask(a.w, i)) != limbMask(a.w, i)) return false;
    return true;
}
inline Val v_pow(Val a, Val b) {
    int w = a.w;
    if (a.w <= 64 && b.w <= 64 && a.hi.empty() && b.hi.empty()) {
        if (a.x || b.x) return make(0, ~0ULL, w, 0, a.sgn);
        uint64_t m = mask(w);
        uint64_t base = a.v & m;
        bool bneg = b.sgn && b.w > 0 && ((b.v >> (b.w - 1)) & 1);
        if (bneg) {
            bool aneg1 = a.sgn && base == m;  // -1
            if (base == 0) return make(0, ~0ULL, w, 0, a.sgn);
            if (base == 1) return make(1, 0, w, 0, a.sgn);
            if (aneg1) return make((b.v & 1) ? m : 1, 0, w, 0, a.sgn);
            return make(0, 0, w, 0, a.sgn);
        }
        uint64_t e = b.v & mask(b.w), r = 1;
        while (e) {
            if (e & 1) r *= base;
            base *= base;
            e >>= 1;
        }
        return make(r & m, 0, w, 0, a.sgn);
    }
    if (anyX(a) || anyX(b)) return xfill(w, a.sgn);
    bool bneg = b.sgn && b.w > 0 && bitAt(b, b.w - 1) == 1;
    if (bneg) {
        if (isZero(a)) return xfill(w, a.sgn);
        if (knownOne(a)) return make(1, 0, w, 0, a.sgn);
        if (a.sgn && knownAllOnes(a)) {
            if (bitAt(b, 0) == 1) { Val r = v_ones(w); r.sgn = a.sgn; return r; }
            return make(1, 0, w, 0, a.sgn);
        }
        return make(0, 0, w, 0, a.sgn);
    }
    Val r = make(1, 0, w, 0, a.sgn);
    Val base = a;
    std::vector<uint64_t> e = magOf(b);
    auto zero = [&]() { for (uint64_t x : e) if (x) return false; return true; };
    while (!zero()) {
        if (e[0] & 1) r = v_mul(r, base);
        base = v_mul(base, base);
        uint64_t c = 0;
        for (int i = (int)e.size() - 1; i >= 0; i--) {
            uint64_t nc = e[i] & 1;
            e[i] = (e[i] >> 1) | (c << 63);
            c = nc;
        }
    }
    return r;
}

// Integer value of a known Val, sign-extended when it is signed.
// A value that does not fit in int64 saturates, so an out-of-range index misses.
inline int64_t v_to_i64(Val v) {
    if (v.w <= 64) {
        uint64_t m = mask(v.w);
        uint64_t u = v.v & m;
        if (v.sgn && v.w < 64 && ((u >> (v.w - 1)) & 1)) u |= ~m;
        return (int64_t)u;
    }
    bool neg = v.sgn && v.w > 0 && bitAt(v, v.w - 1) == 1;
    for (int i = 1; i < limbCount(v.w); i++) {
        uint64_t m = limbMask(v.w, i);
        uint64_t got = limbV(v, i) & m;
        if (got != (neg ? m : 0)) return neg ? INT64_MIN : INT64_MAX;
    }
    if (!neg && v.v > (uint64_t)INT64_MAX) return INT64_MAX;
    return (int64_t)v.v;
}

// $random and $dist_uniform follow the algorithm that IEEE 1800-2023 Annex N
// defines, on 32-bit integers, so a seed gives the same values as other
// simulators. The seed steps as seed = 69069 * seed + 1.
inline double rtlUniform(int32_t& seed, int32_t start, int32_t end) {
    if (seed == 0) seed = 259341593;
    double a = 0.0, b = 2147483647.0;
    if (start < end) { a = start; b = end; }
    seed = (int32_t)(69069u * (uint32_t)seed + 1u);
    uint32_t bits = ((uint32_t)seed >> 9) | 0x3f800000u;
    float f;
    memcpy(&f, &bits, 4);
    double c = f;
    c = c + c * 0.00000011920928955078125;
    return (b - a) * (c - 1.0) + a;
}
inline int32_t rtlDistUniform(int32_t& seed, int32_t start, int32_t end) {
    if (start >= end) return start;
    auto flr = [](double r) { return r >= 0 ? (int64_t)r : (int64_t)(r - 1); };
    int64_t i;
    if (end != INT32_MAX) {
        int64_t e1 = (int64_t)end + 1;
        i = flr(rtlUniform(seed, start, (int32_t)e1));
        if (i < start) i = start;
        if (i >= e1) i = e1 - 1;
    } else if (start != INT32_MIN) {
        int64_t s1 = (int64_t)start - 1;
        i = flr(rtlUniform(seed, (int32_t)s1, end) + 1.0);
        if (i <= s1) i = s1 + 1;
        if (i > end) i = end;
    } else {
        double r = (rtlUniform(seed, start, end) + 2147483648.0) / 4294967295.0;
        r = r * 4294967296.0 - 2147483648.0;
        i = flr(r);
    }
    return (int32_t)i;
}
inline int32_t& globalRandomSeed() { static int32_t s = 0; return s; }
// The other Annex N distributions, in the same 32-bit arithmetic.
inline double rtlNormal(int32_t& seed, int32_t mean, int32_t dev) {
    double v1 = 0, v2 = 0, s = 1.0;
    while (s >= 1.0 || s == 0.0) {
        v1 = rtlUniform(seed, -1, 1);
        v2 = rtlUniform(seed, -1, 1);
        s = v1 * v1 + v2 * v2;
    }
    s = v1 * std::sqrt(-2.0 * std::log(s) / s);
    return s * (double)dev + (double)mean;
}
inline double rtlExponential(int32_t& seed, int32_t mean) {
    double n = rtlUniform(seed, 0, 1);
    if (n != 0) n = -std::log(n) * mean;
    return n;
}
inline int32_t rtlPoisson(int32_t& seed, int32_t mean) {
    int32_t n = 0;
    double p = std::exp(-(double)mean), q = rtlUniform(seed, 0, 1);
    while (p < q) {
        n++;
        q = rtlUniform(seed, 0, 1) * q;
    }
    return n;
}
inline double rtlChiSquare(int32_t& seed, int32_t df) {
    double x = 0.0;
    if (df % 2) { x = rtlNormal(seed, 0, 1); x = x * x; }
    for (int32_t k = 2; k <= df; k += 2) x = x + 2 * rtlExponential(seed, 1);
    return x;
}
inline double rtlT(int32_t& seed, int32_t df) {
    double chi2 = rtlChiSquare(seed, df);
    double root = std::sqrt(chi2 / (double)df);
    return rtlNormal(seed, 0, 1) / root;
}
inline double rtlErlang(int32_t& seed, int32_t k, int32_t mean) {
    double x = 1.0;
    for (int32_t i = 1; i <= k; i++) x = x * rtlUniform(seed, 0, 1);
    return -(double)mean * std::log(x) / (double)k;
}
// Round half away from zero, as the rtl_dist_* wrappers do.
inline int32_t rtlRound(double r) { return r >= 0 ? (int32_t)(r + 0.5) : -(int32_t)(-r + 0.5); }
inline void rtlWarn(const char* what) { std::cerr << "WARNING: " << what << "\n"; }
// $urandom: the generator is implementation-defined (18.13). minisim uses
// one 64-bit xorshift* stream, seeded by -seed (default 1), so a run repeats.
inline uint64_t& urandomState() { static uint64_t s = 0x9E3779B97F4A7C15ull; return s; }
inline uint32_t urandomNext(uint64_t& s) {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
}
// Random stability (18.14.3): every object has its own generator, seeded
// from the $urandom stream when the object is made; its randomize() and
// srandom() use that one.
inline std::unordered_map<int, uint64_t>& objRng() { static std::unordered_map<int, uint64_t> m; return m; }
inline uint64_t seedState(uint64_t seed) {
    uint64_t s = (seed + 1) * 0x9E3779B97F4A7C15ull;
    return s ? s : 1;
}
// Runtime plusargs (21.6): every +arg on the command line, without the +.
inline std::vector<std::string>& plusArgs() { static std::vector<std::string> v; return v; }
inline const std::string* findPlusArg(const std::string& prefix) {
    for (const auto& p : plusArgs())
        if (p.compare(0, prefix.size(), prefix) == 0) return &p;
    return nullptr;
}
// Open files for $fopen (21.3.1): 32-bit descriptors and multichannel bits.
struct FileTab {
    std::vector<FILE*> fds{nullptr, nullptr, nullptr};   // fd 0x80000000 + i
    std::vector<FILE*> mcd = std::vector<FILE*>(31, nullptr);   // channel bit i (bit 0 is stdout)
};
inline FileTab& files() { static FileTab t; return t; }
inline FILE* fdFile(uint32_t fd) {
    if (!(fd & 0x80000000u)) return nullptr;
    uint32_t i = fd & 0x7fffffffu;
    if (i == 0) return stdin;
    if (i == 1) return stdout;
    if (i == 2) return stderr;
    return i < files().fds.size() ? files().fds[i] : nullptr;
}
inline void fileWrite(uint32_t fd, const std::string& t) {
    if (fd & 0x80000000u) {
        uint32_t i = fd & 0x7fffffffu;
        if (i == 1) { std::cout << t; return; }
        if (i == 2) { std::cout.flush(); std::cerr << t; return; }
        if (FILE* f = fdFile(fd)) fwrite(t.data(), 1, t.size(), f);
        return;
    }
    if (fd & 1) std::cout << t;
    for (int b = 1; b < 31; b++)
        if ((fd >> b) & 1 && files().mcd[b]) fwrite(t.data(), 1, t.size(), files().mcd[b]);
}
inline std::string& scanText() { static std::string s; return s; }
inline std::vector<Val>& scanOut() { static std::vector<Val> v; return v; }
// $fscanf / $sscanf (21.3.4) from a character source: next() gives a char or
// EOF, back() returns the last one. The values read go to scanOut(); returns
// the number of conversions, or -1 if the input ended before the first.
template <typename Next, typename Back>
inline int scanFormat(const std::string& fmt, Next next, Back back) {
    scanOut().clear();
    int n = 0;
    bool any = false, eof = false;
    auto skipWs = [&] {
        int c;
        while ((c = next()) != EOF && isspace(c)) {}
        if (c == EOF) eof = true; else back(c);
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        char f = fmt[i];
        if (isspace((unsigned char)f)) { skipWs(); continue; }
        if (f != '%' || i + 1 >= fmt.size()) {
            int c = next();
            if (c != (unsigned char)f) { if (c == EOF) eof = true; else back(c); break; }
            continue;
        }
        i++;
        if (fmt[i] == '%') {
            skipWs();
            int c = next();
            if (c != '%') { if (c != EOF) back(c); break; }
            continue;
        }
        bool skip = false;
        if (fmt[i] == '*') { skip = true; i++; }
        int width = 0;
        while (i < fmt.size() && isdigit((unsigned char)fmt[i])) width = width * 10 + (fmt[i++] - '0');
        if (i >= fmt.size()) break;
        char cv = (char)tolower((unsigned char)fmt[i]);
        if (cv == 'm') { if (!skip) { scanOut().push_back(mkstr("")); n++; } continue; }
        if (cv != 'c') skipWs();
        if (eof) break;
        std::string tok;
        int lim = width > 0 ? width : (cv == 'c' ? 1 : 1 << 30);
        auto ok = [&](int c) {
            switch (cv) {
                case 'c': return true;
                case 's': return !isspace(c);
                case 'd': case 'u': case 't': return isdigit(c) || ((c == '-' || c == '+') && tok.empty()) || c == '_' ||
                                           strchr("xXzZ?", c) != nullptr;
                case 'h': case 'x': return isxdigit(c) || c == '_' || strchr("xXzZ?", c) != nullptr;
                case 'o': return (c >= '0' && c <= '7') || c == '_' || strchr("xXzZ?", c) != nullptr;
                case 'b': return c == '0' || c == '1' || c == '_' || strchr("xXzZ?", c) != nullptr;
                case 'f': case 'e': case 'g': return isdigit(c) || strchr("+-.eE", c) != nullptr;
                default: return false;
            }
        };
        int c;
        while ((int)tok.size() < lim && (c = next()) != EOF) {
            if (!ok(c)) { back(c); break; }
            tok += (char)c;
        }
        if (tok.empty()) { if (!any && c == EOF) eof = true; break; }
        any = true;
        if (skip) continue;
        Val v;
        if (cv == 's') v = mkstr(tok);
        else if (cv == 'c') v = make((unsigned char)tok[0], 0, 8, 0);
        else if (cv == 'f' || cv == 'e' || cv == 'g') v = mkreal(atof(tok.c_str()));
        else {
            int bits = cv == 'h' || cv == 'x' ? 4 : cv == 'o' ? 3 : cv == 'b' ? 1 : 0;
            uint64_t val = 0, x = 0, z = 0;
            bool neg = false;
            size_t k = 0;
            if (!bits && (tok[0] == '-' || tok[0] == '+')) { neg = tok[0] == '-'; k = 1; }
            bool unk = false;
            for (; k < tok.size(); k++) {
                char d = (char)tolower((unsigned char)tok[k]);
                if (d == '_') continue;
                if (!bits) {
                    if (d == 'x' || d == 'z' || d == '?') { unk = true; continue; }
                    val = val * 10 + (d - '0');
                    continue;
                }
                uint64_t m = (1ull << bits) - 1;
                val <<= bits; x <<= bits; z <<= bits;
                if (d == 'x') x |= m;
                else if (d == 'z' || d == '?') { x |= m; z |= m; }
                else val |= (uint64_t)(isdigit((unsigned char)d) ? d - '0' : d - 'a' + 10);
            }
            if (neg) val = ~val + 1;
            v = unk ? allx(64) : make(val, x, 64, z);
            v.sgn = !bits;
        }
        scanOut().push_back(v);
        n++;
    }
    return (n == 0 && eof) ? -1 : n;
}
inline Val v_sysfn(int id, int ctl, const std::vector<Val>& a) {
    auto u64 = [](const Val& v) { return v.v & ~v.x & mask(std::min(64, v.w)); };
    auto i32 = [&](const Val& v) { return (int32_t)(uint32_t)u64(v); };
    auto sint = [](int64_t x) { Val r = make((uint64_t)(uint32_t)x, 0, 32, 0); r.sgn = true; return r; };
    switch (id) {
        case SF_CLOG2: {
            if (anyX(a[0])) return xfill(32, true);
            // the ceiling of log2, for a value up to its full width
            int top = -1, ones = 0;
            for (int b = 0; b < a[0].w; b++) if (bitAt(a[0], b) == 1) { top = b; ones++; }
            return sint(top < 0 ? 0 : ones == 1 ? top : top + 1);
        }
        case SF_COUNTBITS: {
            int n = 0;
            for (int b = 0; b < a[0].w; b++) if (ctl & (1 << bitAt(a[0], b))) n++;
            return sint(n);
        }
        case SF_RANDOM: return sint(rtlDistUniform(globalRandomSeed(), INT32_MIN, INT32_MAX));
        case SF_RANDOM_SEED:
        case SF_DIST_UNIFORM: {
            int32_t seed = i32(a[0]);
            int32_t r = id == SF_RANDOM_SEED ? rtlDistUniform(seed, INT32_MIN, INT32_MAX)
                                             : rtlDistUniform(seed, i32(a[1]), i32(a[2]));
            return make(((uint64_t)(uint32_t)seed << 32) | (uint32_t)r, 0, 64, 0);
        }
        case SF_DIST_NORMAL: case SF_DIST_EXPONENTIAL: case SF_DIST_POISSON:
        case SF_DIST_CHI_SQUARE: case SF_DIST_T: case SF_DIST_ERLANG: {
            int32_t seed = i32(a[0]), r = 0;
            int32_t p1 = a.size() > 1 ? i32(a[1]) : 0, p2 = a.size() > 2 ? i32(a[2]) : 0;
            switch (id) {
                case SF_DIST_NORMAL: r = rtlRound(rtlNormal(seed, p1, p2)); break;
                case SF_DIST_EXPONENTIAL:
                    if (p1 > 0) r = rtlRound(rtlExponential(seed, p1));
                    else rtlWarn("Exponential distribution must have a positive mean");
                    break;
                case SF_DIST_POISSON:
                    if (p1 > 0) r = rtlPoisson(seed, p1);
                    else rtlWarn("Poisson distribution must have a positive mean");
                    break;
                case SF_DIST_CHI_SQUARE:
                    if (p1 > 0) r = rtlRound(rtlChiSquare(seed, p1));
                    else rtlWarn("Chi_square distribution must have positive degree of freedom");
                    break;
                case SF_DIST_T:
                    if (p1 > 0) r = rtlRound(rtlT(seed, p1));
                    else rtlWarn("t distribution must have positive degree of freedom");
                    break;
                default:
                    if (p1 > 0) r = rtlRound(rtlErlang(seed, p1, p2));
                    else rtlWarn("k-stage erlangian distribution must have positive k");
                    break;
            }
            return make(((uint64_t)(uint32_t)seed << 32) | (uint32_t)r, 0, 64, 0);
        }
        case SF_SYSTEM: {   // 20.17: run the command in a shell; the result is its exit status
            Val c = a.empty() ? mkstr("") : a[0];
            c.str = true;
            std::cout.flush();
            fflush(stdout);
            int st = std::system(v_str(c).c_str());
            if (a.empty()) return sint(st != 0);   // $system() with no command: is a shell there
            return sint(st == -1 ? -1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
        }
        case SF_URANDOM: return make(urandomNext(urandomState()), 0, 32, 0);
        case SF_OBJ_SRANDOM:
            objRng()[(int)u64(a[0])] = seedState(u64(a[1]));
            return make(0, 0, 32, 0);
        case SF_OBJ_GETSTATE: {
            char b[32];
            snprintf(b, sizeof b, "MS%016llx", (unsigned long long)objRng()[(int)u64(a[0])]);
            return mkstr(b);
        }
        case SF_OBJ_SETSTATE: {
            const std::string& t = v_str(a[1]);
            if (t.size() == 18 && t[0] == 'M' && t[1] == 'S') objRng()[(int)u64(a[0])] = std::stoull(t.substr(2), nullptr, 16);
            return make(0, 0, 32, 0);
        }
        case SF_URANDOM_SEED: {
            uint64_t s = (u64(a[0]) + 1) * 0x9E3779B97F4A7C15ull;
            if (!s) s = 1;
            urandomState() = s;
            return make(urandomNext(urandomState()), 0, 32, 0);
        }
        case SF_URANDOM_RANGE: {
            uint32_t hi = (uint32_t)u64(a[0]), lo = a.size() > 1 ? (uint32_t)u64(a[1]) : 0;
            if (hi < lo) std::swap(hi, lo);
            uint64_t span = (uint64_t)hi - lo + 1;
            uint32_t r = urandomNext(urandomState());
            return make(lo + (uint32_t)(r % span), 0, 32, 0);
        }
        case SF_PLUS_TEST: {
            Val p = a[0]; p.str = true;
            return make(findPlusArg(v_str(p)) ? 1 : 0, 0, 32, 0, true);
        }
        case SF_FOPEN: {
            Val nm = a[0]; nm.str = true;
            std::string name = v_str(nm), mode = "w";
            if (ctl) { Val m = a[1]; m.str = true; mode = v_str(m); }
            for (char& ch : mode) if (ch == 'b') ch = 'b';
            FILE* f = fopen(name.c_str(), mode.c_str());
            if (!f) return make(0, 0, 32, 0, true);
            if (ctl) {   // a file descriptor
                auto& fds = files().fds;
                size_t i = 3;
                while (i < fds.size() && fds[i]) i++;
                if (i == fds.size()) fds.push_back(nullptr);
                fds[i] = f;
                return make(0x80000000u | (uint32_t)i, 0, 32, 0, true);
            }
            for (int b = 1; b < 31; b++)   // a multichannel descriptor
                if (!files().mcd[b]) { files().mcd[b] = f; return make(1u << b, 0, 32, 0, true); }
            fclose(f);
            return make(0, 0, 32, 0, true);
        }
        case SF_FCLOSE: {
            uint32_t fd = (uint32_t)u64(a[0]);
            if (fd & 0x80000000u) {
                uint32_t i = fd & 0x7fffffffu;
                if (i >= 3 && i < files().fds.size() && files().fds[i]) { fclose(files().fds[i]); files().fds[i] = nullptr; }
            } else {
                for (int b = 1; b < 31; b++)
                    if ((fd >> b) & 1 && files().mcd[b]) { fclose(files().mcd[b]); files().mcd[b] = nullptr; }
            }
            return make(0, 0, 1, 0);
        }
        case SF_FWRITE: {
            Val t = a[1]; t.str = true;
            fileWrite((uint32_t)u64(a[0]), v_str(t) + (ctl ? "\n" : ""));
            return make(0, 0, 1, 0);
        }
        case SF_FFLUSH: {
            std::cout.flush();
            if (a.empty()) { fflush(nullptr); return make(0, 0, 1, 0); }
            uint32_t fd = (uint32_t)u64(a[0]);
            if (FILE* f = fdFile(fd)) fflush(f);
            else for (int b = 1; b < 31; b++) if ((fd >> b) & 1 && files().mcd[b]) fflush(files().mcd[b]);
            return make(0, 0, 1, 0);
        }
        case SF_FGETC: {
            FILE* f = fdFile((uint32_t)u64(a[0]));
            int c = f ? fgetc(f) : EOF;
            return sint(c == EOF ? -1 : c);
        }
        case SF_UNGETC: {
            FILE* f = fdFile((uint32_t)u64(a[1]));
            int c = f ? ungetc((int)(u64(a[0]) & 0xff), f) : EOF;
            return sint(c == EOF ? -1 : 0);
        }
        case SF_FEOF: {
            FILE* f = fdFile((uint32_t)u64(a[0]));
            if (!f) return sint(1);
            int c = fgetc(f);
            if (c == EOF) return sint(1);
            ungetc(c, f);
            return sint(0);
        }
        case SF_FGETS: {
            FILE* f = fdFile((uint32_t)u64(a[0]));
            scanText().clear();
            int c;
            while (f && (c = fgetc(f)) != EOF) {
                scanText() += (char)c;
                if (c == '\n') break;
            }
            return sint((int)scanText().size());
        }
        case SF_SCAN: {
            Val fm = a[1]; fm.str = true;
            std::string fmt = v_str(fm);
            if (ctl) {
                FILE* f = fdFile((uint32_t)u64(a[0]));
                if (!f) return sint(-1);
                return sint(scanFormat(fmt, [&] { return fgetc(f); }, [&](int c) { ungetc(c, f); }));
            }
            Val sv = a[0]; sv.str = true;
            std::string src = v_str(sv);
            size_t p = 0;
            return sint(scanFormat(fmt, [&] { return p < src.size() ? (int)(unsigned char)src[p++] : (p++, EOF); },
                                   [&](int) { p--; }));
        }
        case SF_SCAN_GET: {
            size_t k = (size_t)u64(a[0]);
            return k < scanOut().size() ? scanOut()[k] : xfill(64, false);
        }
        case SF_SCAN_TEXT: return mkstr(scanText());
        case SF_FTELL: {
            FILE* f = fdFile((uint32_t)u64(a[0]));
            return sint(f ? (int)ftell(f) : -1);
        }
        case SF_FSEEK: {
            FILE* f = fdFile((uint32_t)u64(a[0]));
            int wh = a.size() > 2 ? (int)u64(a[2]) : 0;
            int64_t off = a.size() > 1 ? (int64_t)(int32_t)(uint32_t)u64(a[1]) : 0;
            int r = f ? fseek(f, off, wh == 1 ? SEEK_CUR : wh == 2 ? SEEK_END : SEEK_SET) : -1;
            return sint(r == 0 ? 0 : -1);
        }
        case SF_FERROR: return sint(0);
        case SF_SR2BITS: { float f = (float)v_real(a[0]); uint32_t b; memcpy(&b, &f, 4); return make(b, 0, 32, 0); }
        case SF_CG: return g_cgHook ? g_cgHook(ctl, a) : mkreal(0);
        case SF_BITS2SR: { uint32_t b = (uint32_t)u64(a[0]); float f; memcpy(&f, &b, 4); return mkreal(f); }
        case SF_TIMEFORMAT: {
            TimeFmt& t = timeFmt();
            auto i64 = [&](const Val& v) { return v_to_i64(v); };
            if (a.empty()) { t.units = t.tickExp; t.prec = 0; t.suffix.clear(); t.width = 20; }
            if (a.size() > 0) t.units = (int)i64(a[0]);
            if (a.size() > 1) t.prec = (int)i64(a[1]);
            if (a.size() > 2) { Val s = a[2]; s.str = true; t.suffix = v_str(s); }
            if (a.size() > 3) t.width = (int)i64(a[3]);
            return make(0, 0, 1, 0);
        }
        case SF_PLUS_VALUE: {   // the caller has checked that a match exists
            Val p = a[0]; p.str = true;
            const std::string& pre = v_str(p);
            const std::string* m = findPlusArg(pre);
            std::string t = m ? m->substr(pre.size()) : "";
            if (ctl == 's') return mkstr(t);
            if (ctl == 'f' || ctl == 'e' || ctl == 'g') { Val r = make(0, 0, 64, 0); double d = atof(t.c_str()); memcpy(&r.v, &d, 8); r.real = true; return r; }
            int base = ctl == 'h' || ctl == 'x' ? 16 : ctl == 'o' ? 8 : ctl == 'b' ? 2 : 10;
            uint64_t v = 0, x = 0;
            bool neg = false;
            size_t i = 0;
            if (base == 10 && i < t.size() && (t[i] == '-' || t[i] == '+')) neg = t[i++] == '-';
            int bits = base == 16 ? 4 : base == 8 ? 3 : 1;
            for (; i < t.size(); i++) {
                char c = (char)tolower((unsigned char)t[i]);
                if (c == '_') continue;
                if (base != 10 && (c == 'x' || c == 'z')) { v <<= bits; x = (x << bits) | mask(bits); continue; }
                int d = isdigit((unsigned char)c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 99;
                if (d >= base) break;
                v = base == 10 ? v * 10 + d : (v << bits) | d;
                x <<= base == 10 ? 0 : bits;
            }
            if (neg) v = ~v + 1;
            Val r = make(v, x, 64, 0);
            r.sgn = base == 10;
            return r;
        }
    }
    return xfill(32, false);
}

// ---- frames: automatic variables (6.21, 13.3.2) and class objects (8) ----
// A static variable is a signal with a fixed id. An automatic variable lives
// in a frame: a block of signals allocated when its subroutine is called (or
// its fork branch starts) and freed when the last user is done with it. Code
// refers to a frame slot with a relative id, resolved by R() at run time:
//   kRel | mode << 26 | offset
// mode 0..7: the frame `mode` levels up from the running one (a fork branch
//            reaches its parent's locals at depth 1),
// mode 8:    a property of `this` (the running method's object),
// mode 9:    a slot of the frame that the last call returned from (the
//            caller copies a function result and output arguments from it),
// mode 10:   indirect: signal `offset` holds the signal id,
// mode 11+d: indirect: slot `offset` of the frame d levels up (d < 5) holds
//            the signal id. Op::Addr makes such an id for a property reached
//            through a handle (h.q, h.a[i]); a `ref` argument (13.5.2)
//            holds its actual's id. The slot holds id + 1, so 0 is null.
// An object is a block of signals too; a handle holds its first signal id
// (0 is null). Every relative id stays inside its mode as long as the
// offset is below 2^26, so arr->base + k still resolves correctly.
constexpr int kRel = 1 << 30;
constexpr int kRelOffBits = 26;
constexpr int kRelThis = 8, kRelRet = 9, kRelIndSig = 10, kRelIndFrame = 11;   // kRelIndFrame + depth, up to 15
inline int relSig(int mode, int off) { return kRel | (mode << kRelOffBits) | off; }
inline bool isRel(int s) { return s >= kRel; }
inline int relMode(int s) { return (s >> kRelOffBits) & 15; }
inline int relOff(int s) { return s & ((1 << kRelOffBits) - 1); }
// Slot shape of a frame: widths, initial values, containers. Shared by every
// frame of one subroutine; freed frames go back to `pool` and are reused.
struct FrameLayout {
    std::vector<int> w;              // width of each slot
    std::vector<Val> init;           // its value when the frame is allocated
    std::vector<std::pair<int, DynObj>> dyn;   // container slots and their empty shape
    std::vector<int> noCopy;         // slots a shallow copy leaves at their initial value
    std::string name;                // for messages
    mutable std::vector<int> pool;   // first signal of each free frame
    int add(int width, Val v) { w.push_back(width); init.push_back(v); return (int)w.size() - 1; }
};
// The kernel allocates frame signals (Sim::sig is not visible here).
inline int (*g_allocSigs)(const FrameLayout&) = nullptr;
inline void (*g_initSigs)(const FrameLayout&, int base) = nullptr;
inline void (*g_nullAccess)() = nullptr;
inline Val (*g_getSig)(int) = nullptr;
inline bool (*g_isA)(int obj, int cls) = nullptr;   // is object obj of class cls or one derived from it
// > 0 while an Op::Addr computes an address: reading through a null handle
// there gives null (the access that uses the address reports it, if it runs).
inline int g_nullOk = 0;
inline int g_nullSig = 0;   // a signal that always holds 0, read in place of a null object's slot
struct Frame {
    int base = 0;                    // first signal of this frame's slots
    int self = 0;                    // `this` (a handle), 0 outside methods
    const FrameLayout* lay = nullptr;
    std::shared_ptr<Frame> up;       // the lexically enclosing frame (fork branches)
    Frame() = default;
    Frame(const Frame&) = delete;
    ~Frame() { if (lay && !lay->w.empty()) lay->pool.push_back(base); }
};
using FrameP = std::shared_ptr<Frame>;
inline FrameP newFrame(const FrameLayout* lay, FrameP up, int self) {
    auto f = std::make_shared<Frame>();
    f->lay = lay;
    f->up = std::move(up);
    f->self = self;
    if (lay && !lay->w.empty()) {
        if (!lay->pool.empty()) { f->base = lay->pool.back(); lay->pool.pop_back(); }
        else f->base = g_allocSigs(*lay);
        g_initSigs(*lay, f->base);
    }
    return f;
}
// The running process's frame and the frame its last call returned from.
// Only the kernel's main thread runs processes, so these are plain globals;
// exec() sets them before every instruction.
inline Frame* g_fr = nullptr;
inline Frame* g_ret = nullptr;
[[noreturn]] inline void relFail(const char* what) {
    fprintf(stderr, "internal error: %s\n", what);
    abort();
}
inline int relSlow(int s) {
    int m = relMode(s), off = relOff(s);
    if (m == kRelThis) {
        if (!g_fr || !g_fr->self) { if (g_nullAccess) g_nullAccess(); relFail("this outside a method"); }
        return g_fr->self + off;
    }
    if (m == kRelRet) {
        if (!g_ret) relFail("no returned frame");
        return g_ret->base + off;
    }
    if (m >= kRelIndSig) {
        int at = off;
        if (m >= kRelIndFrame) {
            Frame* f = g_fr;
            for (int d = 0; d < m - kRelIndFrame && f; d++) f = f->up.get();
            if (!f) relFail("indirect id outside a frame");
            at = f->base + off;
        }
        Val a = g_getSig(at);   // the signal id + 1; 0 is a null handle
        if (a.x || !a.v) {
            if (g_nullOk) return g_nullSig;
            if (g_nullAccess) g_nullAccess();
            relFail("null object access");
        }
        return (int)a.v - 1;
    }
    Frame* f = g_fr;
    for (int d = 0; d < m && f; d++) f = f->up.get();
    if (!f) relFail("automatic variable outside its frame");
    return f->base + off;
}
// The signal id a (possibly relative) id names right now.
inline int R(int s) { return s < kRel ? s : relSlow(s); }
// Every signal `e` reads, resolved now (a wait on automatic variables).
inline void collectAddrs(const std::shared_ptr<struct Expr>& e, std::vector<int>& out);

inline Val eval(const E& e, const Reader& rd) {
    auto k = [&](int i) { return eval(e->kids[i], rd); };
    switch (e->op) {
        case Op::Const: return e->k;
        case Op::Ref: {
            if (e->at) {   // h.x
                Val h = eval(e->at, rd);
                if (h.x || !h.v) {
                    if (g_nullOk) return make(0, 0, 64);
                    if (g_nullAccess) g_nullAccess();
                    relFail("null object access");
                }
                Val v = rd((int)h.v + e->sig);
                v.sgn = e->sgn;
                return v;
            }
            Val v = rd(R(e->sig));
            v.sgn = e->sgn;
            return v;
        }
        case Op::This: return make(g_fr ? (uint64_t)g_fr->self : 0, 0, 64);
        case Op::IsA: {
            Val h = k(0);
            if (h.x || !h.v) return mk(1, 1);
            return mk(g_isA ? g_isA((int)h.v, e->hi) : 0, 1);
        }
        case Op::AddrOf: return make((uint64_t)R(e->sig) + 1, 0, 64);
        case Op::Addr: {
            g_nullOk++;
            Val h = k(0);
            g_nullOk--;
            return make(h.x || !h.v ? 0 : h.v + (uint64_t)e->sig + 1, 0, 64);
        }
        case Op::Not: return v_not(k(0));
        case Op::And: return v_and(k(0), k(1));
        case Op::Or: return v_or(k(0), k(1));
        case Op::Xor: return v_xor(k(0), k(1));
        case Op::Add: return v_add(k(0), k(1));
        case Op::Sub: return v_sub(k(0), k(1));
        case Op::Mul: return v_mul(k(0), k(1));
        case Op::Div: return v_div(k(0), k(1));
        case Op::Mod: return v_mod(k(0), k(1));
        case Op::Eq: return v_eq(k(0), k(1));
        case Op::Ne: return v_ne(k(0), k(1));
        case Op::CaseEq: return v_case_eq(k(0), k(1));
        case Op::CaseZEq: return v_casez_eq(k(0), k(1));
        case Op::CaseXEq: return v_casex_eq(k(0), k(1));
        case Op::Lt: return v_lt(k(0), k(1));
        case Op::WildEq: return v_wild_eq(k(0), k(1));
        case Op::FAdd: return mkreal(v_real(k(0)) + v_real(k(1)));
        case Op::FSub: return mkreal(v_real(k(0)) - v_real(k(1)));
        case Op::FMul: return mkreal(v_real(k(0)) * v_real(k(1)));
        case Op::FDiv: return mkreal(v_real(k(0)) / v_real(k(1)));
        case Op::FPow: return mkreal(std::pow(v_real(k(0)), v_real(k(1))));
        case Op::FNeg: return mkreal(-v_real(k(0)));
        case Op::FCmp: {
            double a = v_real(k(0)), b = v_real(k(1));
            bool r = false;
            switch (e->hi) {
                case 0: r = a < b; break;  case 1: r = a <= b; break;
                case 2: r = a > b; break;  case 3: r = a >= b; break;
                case 4: r = a == b; break; default: r = a != b; break;
            }
            return mk(r);
        }
        case Op::IToR: return mkreal(v_real(k(0)));
        case Op::RToI: return v_rtoi(k(0), e->hi, e->sgn, e->lo);
        case Op::AsReal: { Val v = k(0); Val r = mkreal(0); r.v = v.v & ~v.x; return r; }
        case Op::FMath: return mkreal(mathFn(e->hi, v_real(k(0)), e->kids.size() > 1 ? v_real(k(1)) : 0));
        case Op::RealTime: return mkreal((double)g_simTime / (double)(e->k.v ? e->k.v : 1));
        case Op::AsStr: { Val v = k(0); Val r = make(v.v, v.x, 64); r.str = true; return r; }
        case Op::IntToStr: { Val v = k(0); return v.str ? v : mkstr(bytesToStr(v)); }
        case Op::StrToInt: {
            const std::string& s = v_str(k(0));
            uint64_t v = 0;
            for (size_t i = 0; i < s.size(); i++) v = (v << 8) | (unsigned char)s[i];
            return make(v, 0, e->hi);
        }
        case Op::StrCat: {
            std::string s;
            for (size_t i = 0; i < e->kids.size(); i++) { Val v = k((int)i); s += v.str ? v_str(v) : bytesToStr(v); }
            return mkstr(s);
        }
        case Op::StrRep: {
            std::string one = v_str(k(0)), s;
            for (int i = 0; i < e->hi; i++) s += one;
            return mkstr(s);
        }
        case Op::StrCmp: {
            int c = v_str(k(0)).compare(v_str(k(1)));
            bool r = false;
            switch (e->hi) {
                case 0: r = c < 0; break;  case 1: r = c <= 0; break;
                case 2: r = c > 0; break;  case 3: r = c >= 0; break;
                case 4: r = c == 0; break; default: r = c != 0; break;
            }
            return mk(r);
        }
        case Op::StrMethod: {
            std::string s = v_str(k(0));
            auto ival = [&](int i) { return v_to_i64(k(i)); };
            auto num = [](int64_t v, int w) { Val r = make((uint64_t)v, 0, w); r.sgn = true; return r; };
            auto parse = [&](int base) {
                int64_t v = 0;
                size_t i = 0;
                bool neg = false;
                if (base == 10 && i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
                for (; i < s.size(); i++) {
                    char c = (char)std::tolower((unsigned char)s[i]);
                    if (c == '_') continue;
                    int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 99;
                    if (d >= base) break;
                    v = v * base + d;
                }
                return num(neg ? -v : v, 32);
            };
            auto radix = [&](int base) {
                Val v = k(1);
                uint64_t u = v.v & ~v.x;
                if (base == 10) return std::to_string(v.sgn ? v_to_i64(v) : (int64_t)u);
                if (!u) return std::string("0");
                std::string r;
                while (u) { r += "0123456789abcdef"[u % base]; u /= base; }
                return std::string(r.rbegin(), r.rend());
            };
            switch (e->hi) {
                case S_LEN: return num((int64_t)s.size(), 32);
                case S_GETC: { int64_t i = ival(1); return make(i >= 0 && i < (int64_t)s.size() ? (unsigned char)s[i] : 0, 0, 8); }
                case S_TOUPPER: for (auto& c : s) c = (char)std::toupper((unsigned char)c); return mkstr(s);
                case S_TOLOWER: for (auto& c : s) c = (char)std::tolower((unsigned char)c); return mkstr(s);
                case S_COMPARE: { int c = s.compare(v_str(k(1))); return num(c < 0 ? -1 : c > 0, 32); }
                case S_ICOMPARE: {
                    std::string a = s, b = v_str(k(1));
                    for (auto& c : a) c = (char)std::tolower((unsigned char)c);
                    for (auto& c : b) c = (char)std::tolower((unsigned char)c);
                    int c = a.compare(b);
                    return num(c < 0 ? -1 : c > 0, 32);
                }
                case S_SUBSTR: {   // s.substr(i, j): chars i..j, or "" when out of range
                    int64_t i = ival(1), j = ival(2);
                    if (i < 0 || j >= (int64_t)s.size() || i > j) return mkstr("");
                    return mkstr(s.substr((size_t)i, (size_t)(j - i + 1)));
                }
                case S_ATOI: return parse(10);
                case S_ATOHEX: return parse(16);
                case S_ATOOCT: return parse(8);
                case S_ATOBIN: return parse(2);
                case S_ATOREAL: return mkreal(std::strtod(s.c_str(), nullptr));
                case S_PUTC: {   // a new string; the statement writes it back
                    int64_t i = ival(1);
                    Val c = k(2);
                    if (i >= 0 && i < (int64_t)s.size() && (c.v & 0xff)) s[i] = (char)(c.v & 0xff);
                    return mkstr(s);
                }
                case S_ITOA: return mkstr(radix(10));
                case S_HEXTOA: return mkstr(radix(16));
                case S_OCTTOA: return mkstr(radix(8));
                case S_BINTOA: return mkstr(radix(2));
                case S_REALTOA: { char b[64]; snprintf(b, sizeof b, "%g", v_real(k(1))); return mkstr(b); }
            }
            return mkstr(s);
        }
        case Op::DynGet: return dynStore()[R(e->sig)].get(k(0));
        case Op::DynSize: { Val r = make(dynStore()[R(e->sig)].size(), 0, 32); r.sgn = true; return r; }
        case Op::DynExists: {
            auto& d = dynStore()[R(e->sig)];
            Val key = k(0);
            if (d.kind != 2) { int64_t i = v_to_i64(key); return mk(!key.x && i >= 0 && i < (int64_t)d.vec.size()); }
            return mk(d.map.count(d.key(key)) > 0);
        }
        case Op::DynHas: {
            auto& d = dynStore()[R(e->sig)];
            Val x = k(0);
            bool unk = false;
            for (auto& el : d.vec) {
                int w = std::max(x.w, el.w);
                Val r = v_wild_eq(v_resize(x, w, x.sgn && el.sgn), v_resize(el, w, x.sgn && el.sgn));
                if (r.x) unk = true;
                else if (r.v & 1) return mk(1);
            }
            return unk ? allx(1) : mk(0);
        }
        case Op::DynBits: {
            auto& d = dynStore()[R(e->sig)];
            std::vector<Val> parts;
            int ew = d.dflt.w;
            if (d.kind == 2) for (auto& [key, v] : d.map) parts.push_back(v_resize(v, ew, false));
            else for (auto& v : d.vec) parts.push_back(v_resize(v, ew, false));
            if (parts.empty()) { Val z = make(0, 0, 1, 0); z.w = 0; return z; }
            return v_concat(parts);
        }
        case Op::StreamPack: {
            std::vector<Val> parts;
            for (auto& kid : e->kids) { Val v = eval(kid, rd); if (v.w > 0) parts.push_back(v); }
            int W = 0;
            for (auto& p : parts) W += p.w;
            Val v = W > 0 ? v_concat(parts) : make(0, 0, 1, 0);
            if (W > 0 && e->lo > 0 && e->lo < W) {
                std::vector<Val> sl;
                for (int lo = 0; lo < W; lo += e->lo) sl.push_back(v_slice(v, std::min(lo + e->lo, W) - 1, lo));
                v = v_concat(sl);
            }
            if (e->hi < 0) return v;
            if (W == 0) return make(0, 0, e->hi, 0);
            if (e->hi > W) return v_concat({v, make(0, 0, e->hi - W, 0)});
            if (e->hi < W) return v_slice(v, W - 1, W - e->hi);
            return v;
        }
        case Op::DynReduce: {
            auto& d = dynStore()[R(e->sig)];
            int w = e->k.w;
            Val acc = make(e->hi == 1 ? 1 : e->hi == 2 ? ~0ULL : 0, 0, w);
            auto one = [&](const Val& x) {
                Val v = v_resize(x, w, x.sgn);
                switch (e->hi) {
                    case 0: acc = v_add(acc, v); break;
                    case 1: acc = v_mul(acc, v); break;
                    case 2: acc = v_and(acc, v); break;
                    case 3: acc = v_or(acc, v); break;
                    default: acc = v_xor(acc, v); break;
                }
            };
            if (d.kind == 2) for (auto& [key, v] : d.map) one(v);
            else for (auto& v : d.vec) one(v);
            acc.sgn = e->k.sgn;
            return acc;
        }
        case Op::SFormat: return mkstr(g_sformat ? g_sformat(e->fmt, std::vector<E>(e->kids.begin(), e->kids.end()), rd) : "");
        case Op::Le: return v_le(k(0), k(1));
        case Op::Gt: return v_gt(k(0), k(1));
        case Op::Ge: return v_ge(k(0), k(1));
        case Op::Shl: return v_shl(k(0), k(1));
        case Op::Shr: return v_shr(k(0), k(1));
        case Op::Asr: return v_asr(k(0), k(1));
        case Op::Resize: {
            Val v = k(0);
            Val r = v_resize(v, e->hi, e->lo && e->hi > v.w);
            r.sgn = e->sgn;
            r.real = false;   // the bits of a real are an integer now ($realtobits)
            return r;
        }
        case Op::LogNot: return v_lnot(k(0));
        case Op::LogAnd: return v_land(k(0), k(1));
        case Op::LogOr: return v_lor(k(0), k(1));
        case Op::RedAnd: return v_red_and(k(0));
        case Op::RedOr: return v_red_or(k(0));
        case Op::RedXor: return v_red_xor(k(0));
        case Op::Slice: return v_slice(k(0), e->hi, e->lo);
        case Op::Index: return v_index(k(0), k(1));
        case Op::Concat: {
            std::vector<Val> parts;
            for (size_t i = 0; i < e->kids.size(); i++) parts.push_back(k((int)i));
            return v_concat(parts);
        }
        case Op::Mux: return v_mux(k(0), k(1), k(2));
        case Op::Pow: return v_pow(k(0), k(1));
        case Op::Time: {   // k.v ticks per time unit of the calling module
            uint64_t d = e->k.v ? e->k.v : 1;
            return make((g_simTime + d / 2) / d, 0, 64, 0);
        }
        case Op::SysFn: {
            if (e->hi == SF_ASSOC_BOUND) {
                auto& d = dynStore()[e->sig];
                if (d.map.empty()) return xfill(32, true);
                const DynKey& key = e->lo ? std::prev(d.map.end())->first : d.map.begin()->first;
                Val kv = d.keyVal(key);
                Val r = make((uint64_t)(uint32_t)v_to_i64(kv), 0, 32, 0);
                r.sgn = true;
                return r;
            }
            std::vector<Val> a;
            for (size_t i = 0; i < e->kids.size(); i++) a.push_back(k((int)i));
            return v_sysfn(e->hi, e->lo, a);
        }
        case Op::DynSlice: {
            Val a = k(0), i = k(1);
            if (shiftUnknown(i)) return allx(e->hi);
            int64_t s = v_to_i64(i);
            return v_dynslice(a, e->lo ? s - e->hi + 1 : s, e->hi);
        }
        case Op::ArrRead: {
            // An index with x/z bits, or one outside the bounds, reads the
            // element type's default: X for 4-state, 0 for 2-state (7.4.6).
            const ArrShape& a = *e->arr;
            Val bad = a.twoState ? make(0, 0, a.elemW, 0, a.sgn) : xfill(a.elemW, a.sgn);
            std::vector<int64_t> idx;
            for (size_t i = 0; i < e->kids.size(); i++) {
                Val v = k((int)i);
                if (shiftUnknown(v)) return bad;
                idx.push_back(v_to_i64(v));
            }
            int64_t off = a.offset(idx);
            if (off < 0) return bad;
            Val r = rd(R(a.base) + (int)off);
            r.sgn = a.sgn;
            return r;
        }
    }
    return allx(1);
}

inline void collectAddrs(const E& e, std::vector<int>& out) {
    if (!e) return;
    if (e->op == Op::Ref && e->at) {   // h.x: the slot of the object h names now
        Val h = eval(e->at, [](int s) { return g_getSig(s); });
        if (!h.x && h.v) out.push_back((int)h.v + e->sig);
        collectAddrs(e->at, out);
        return;
    }
    if (e->op == Op::Ref) out.push_back(R(e->sig));
    if (e->op == Op::ArrRead)
        for (int i = 0; i < e->arr->count; i++) out.push_back(R(e->arr->base) + i);
    if (e->op == Op::DynGet || e->op == Op::DynSize || e->op == Op::DynExists || e->op == Op::DynReduce || e->op == Op::DynBits || e->op == Op::DynHas) out.push_back(R(e->sig));
    for (auto& k : e->kids) collectAddrs(k, out);
}
// A copy of `e` with every relative id resolved now: a $strobe / $monitor
// argument is read later, when the frame it names may be gone or another
// process may be running.
inline E bindRel(const E& e) {
    if (!e) return e;
    bool rel = isRel(e->sig) && !e->at && (e->op == Op::Ref || e->op == Op::DynGet || e->op == Op::DynSize ||
                                            e->op == Op::DynExists || e->op == Op::DynReduce || e->op == Op::DynBits || e->op == Op::DynHas);
    if (e->op == Op::This) return konst(make(g_fr ? (uint64_t)g_fr->self : 0, 0, 64));
    bool arel = e->op == Op::ArrRead && e->arr && isRel(e->arr->base);
    std::vector<E> kids;
    bool kch = false;
    for (auto& k : e->kids) { kids.push_back(bindRel(k)); kch |= kids.back() != k; }
    E at = bindRel(e->at);
    if (!rel && !arel && !kch && at == e->at) return e;
    auto c = std::make_shared<Expr>(*e);
    c->kids = std::move(kids);
    c->at = at;
    if (rel) c->sig = R(e->sig);
    if (arel) { auto a = std::make_shared<ArrShape>(*e->arr); a->base = R(a->base); c->arr = a; }
    return c;
}
