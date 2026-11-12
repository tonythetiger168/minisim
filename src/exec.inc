// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `entry` must outlive the simulation.
// `rets` is a coroutine local. A #delay inside a callee hits co_await here,
// and the return address is still on `rets` when the process resumes.
static Val rd(int s) { return S.get(s); }

struct RetFrame { const Prog* prog; size_t pc; FrameP fr; };

// Resolve a run-time array index and bit index. false: no write happens.
// v is the value to write; a part-select that is partly out of range writes
// only the bits inside the vector (11.5.1), so v is shifted to match.
static bool target(const Instr& i, int& sig, int& hi, int& lo, Val& val) {
    sig = R(sig);
    if (i.arr) {
        std::vector<int64_t> idx;
        for (auto& a : i.aidx) {
            Val v = eval(a, rd);
            if (v.x) return false;
            idx.push_back(v_to_i64(v));
        }
        int64_t off = i.arr->offset(idx);
        if (off < 0) return false;
        sig = R(i.arr->base) + (int)off;
    }
    if (i.bidx) {
        Val v = eval(i.bidx, rd);
        if (v.x) return false;
        int64_t b = v_to_i64(v) - i.lo;   // lo holds the vector's low bound here
        if (!i.pw) {
            if (b < 0 || b >= S.sigs[sig].w) return false;
            hi = lo = (int)b;
            return true;
        }
        int64_t l = i.pdown ? b - i.pw + 1 : b, h = l + i.pw - 1;
        int64_t cl = std::max<int64_t>(l, 0), ch = std::min<int64_t>(h, S.sigs[sig].w - 1);
        if (cl > ch) return false;
        val = v_slice(val, (int)(ch - l), (int)(cl - l));
        hi = (int)ch;
        lo = (int)cl;
    }
    return true;
}

// $writememh / $writememb (21.5): one word per line, every digit written
// (x and z as x and z), no @ addresses. The default range is the whole
// memory, lowest address first; start and finish pick a range and order.
// A multidimensional memory in a $readmem / $writemem file (21.4.3): an
// address picks a word of the highest dimension, and its words of the
// lower dimensions follow in row-major order, each dimension from its low
// address up. k numbers those words; memSub is how many there are.
static int64_t memSub(const ArrShape& a) {
    int64_t n = 1;
    for (size_t d = 1; d < a.dims.size(); d++) n *= std::abs(a.dims[d].first - a.dims[d].second) + 1;
    return n;
}
static std::vector<int64_t> memIdx(const ArrShape& a, int64_t addr, int64_t k) {
    std::vector<int64_t> idx(a.dims.size());
    idx[0] = addr;
    for (size_t d = a.dims.size(); d-- > 1;) {
        auto [l, r] = a.dims[d];
        int64_t n = std::abs(l - r) + 1;
        idx[d] = std::min(l, r) + k % n;
        k /= n;
    }
    return idx;
}
static void writeMem(const Instr& i) {
    const ArrShape& a = *i.arr;
    auto [l, r] = a.dims[0];
    int64_t lowA = std::min(l, r), highA = std::max(l, r);
    int64_t start = lowA, finish = highA;
    if (i.args.size() > 0) { Val v = eval(i.args[0], rd); if (!v.x) start = v_to_i64(v); }
    if (i.args.size() > 1) { Val v = eval(i.args[1], rd); if (!v.x) finish = v_to_i64(v); }
    std::ofstream out(i.fmt);
    if (!out) { std::cerr << "$writemem: cannot open " << i.fmt << "\n"; return; }
    int64_t step = finish < start ? -1 : 1;
    const int64_t sub = memSub(a);
    for (int64_t ad = start;; ad += step) {
      for (int64_t k = 0; k < sub; k++) {
        int64_t off = a.offset(memIdx(a, ad, k));
        if (off >= 0) {
            Val v = S.get(R(a.base) + (int)off);
            v.sgn = false;
            int bits = i.hex ? 4 : 1;
            int n = (a.elemW + bits - 1) / bits;
            std::string s(n, '0');
            for (int d = 0; d < n; d++) {
                int val = 0, xs = 0, zs = 0, cnt = 0;
                for (int b = 0; b < bits && d * bits + b < a.elemW; b++, cnt++) {
                    int t = bitAt(v, d * bits + b);
                    if (t == 1) val |= 1 << b;
                    else if (t == 2) xs++;
                    else if (t == 3) zs++;
                }
                s[n - 1 - d] = zs == cnt ? 'z' : (xs || zs) ? 'x' : "0123456789abcdef"[val];
            }
            out << s << "\n";
        }
      }
        if (ad == finish) break;
    }
}

// $readmemh / $readmemb (21.4) into a one-dimensional memory. Addresses are
// index values. Without a range, loading starts at the lowest address and goes
// up; with start and finish it goes from start toward finish. "@hex" in the
// file moves the address. 2-state elements read x/z digits as 0 (21.4.2).
// A user-defined primitive (29). Values are 0, 1, 2 (x); z inputs read as x
// (29.3.5). `last` holds each input's previous value in two bits (all X
// before the first evaluation). Inputs that changed are taken one at a time, in port order; for
// each, a matching level row wins over a matching edge row (29.9, 29.10),
// and a change no row covers gives x (29.6). '-' keeps the state.
static bool udpLevel(char c, int v) {
    switch (c) {
        case '0': return v == 0;
        case '1': return v == 1;
        case 'x': return v == 2;
        case 'b': return v < 2;
        case '?': return true;
        default: return false;
    }
}
static bool udpEdge(const std::string& f, int a, int b) {
    if (f.size() == 4) return udpLevel(f[1], a) && udpLevel(f[2], b);
    switch (f[0]) {
        case 'r': return a == 0 && b == 1;
        case 'f': return a == 1 && b == 0;
        case 'p': return (a == 0 && b != 0) || (a == 2 && b == 1);
        case 'n': return (a == 1 && b != 1) || (a == 2 && b == 0);
        case '*': return true;
        default: return false;
    }
}
static char udpMatch(const UdpTable& t, const std::vector<int>& in, int q, int edgeAt, int from) {
    for (const UdpRow& r : t.rows) {
        if (r.edge != edgeAt) continue;
        if (t.seq && !udpLevel(r.state, q)) continue;
        bool ok = true;
        for (int k = 0; k < t.nin && ok; k++)
            ok = k == edgeAt ? udpEdge(r.in[(size_t)k], from, in[(size_t)k]) : udpLevel(r.in[(size_t)k][0], in[(size_t)k]);
        if (ok) return r.out;
    }
    return 0;
}
static void udpExec(const Instr& i) {
    const UdpTable& t = *i.udp;
    Val qv = S.get(i.sig), lv = S.get(i.dres);
    int q = (qv.x & 1) ? 2 : (int)(qv.v & 1);
    std::vector<int> prev((size_t)t.nin), cur((size_t)t.nin);
    for (int k = 0; k < t.nin; k++) {
        Val a = eval(i.args[(size_t)k], rd);
        cur[(size_t)k] = (a.x & 1) ? 2 : (int)(a.v & 1);
        int code = bitAt(lv, 2 * k) | (bitAt(lv, 2 * k + 1) == 1 ? 2 : 0);
        prev[(size_t)k] = bitAt(lv, 2 * k) >= 2 ? 2 : code;
    }
    // The first evaluation (time 0) takes the inputs as they are: their
    // starting values are not transitions.
    bool first = t.nin > 0 && bitAt(lv, 0) >= 2;
    if (first) prev = cur;
    auto apply = [&](char r) { if (r == '0') q = 0; else if (r == '1') q = 1; else if (r != '-') q = 2; };
    if (!t.seq) {
        char r = udpMatch(t, cur, q, -1, 0);
        apply(r ? r : 'x');
    } else {
        std::vector<int> now = prev;
        bool any = false;
        for (int k = 0; k < t.nin; k++) {
            if (cur[(size_t)k] == prev[(size_t)k]) continue;
            any = true;
            now[(size_t)k] = cur[(size_t)k];
            char r = udpMatch(t, now, q, -1, 0);
            if (!r && t.edges) r = udpMatch(t, now, q, k, prev[(size_t)k]);
            apply(r ? r : 'x');
        }
        if (!any && first) {   // a sequential UDP's first evaluation: its level rows only
            char r = udpMatch(t, cur, q, -1, 0);
            if (r) apply(r);
        }
    }
    Val nl = make(0, 0, 2 * t.nin);
    if (2 * t.nin > 64) nl = maskWidth(nl, 2 * t.nin);
    for (int k = 0; k < t.nin; k++) {
        int v = cur[(size_t)k];
        setBit(nl, 2 * k, v == 2 ? 0 : v);
        setBit(nl, 2 * k + 1, v == 2 ? 1 : 0);
    }
    S.set(i.dres, nl);
    S.set(i.sig, q == 2 ? allx(1) : make((uint64_t)q, 0, 1));
}

// $fread into a memory (21.3.4.4): args are fd, start, count (either may be
// unset). Elements load from start toward the highest address; dres gets
// the number of bytes read.
static void freadMem(const Instr& i) {
    const ArrShape& a = *i.arr;
    auto [l, r] = a.dims[0];
    int64_t lowA = std::min(l, r), highA = std::max(l, r);
    FILE* f = fdFile((uint32_t)v_to_i64(eval(i.args[0], rd)));
    int64_t start = lowA, count = highA - lowA + 1;
    if (i.args[1]) { Val v = eval(i.args[1], rd); if (!v.x) start = v_to_i64(v); }
    if (i.args[2]) { Val v = eval(i.args[2], rd); if (!v.x) count = v_to_i64(v); }
    int64_t total = 0;
    std::string buf;
    for (int64_t ad = start; f && ad <= highA && count > 0; ad++, count--) {
        int64_t off = a.offset({ad});
        if (off < 0) break;
        int sig = a.base + (int)off, w = S.sigs[sig].w;
        size_t n = (size_t)(w + 7) / 8;
        buf.assign(n, '\0');
        size_t got = fread(buf.data(), 1, n, f);
        total += (int64_t)got;
        if (!got) break;
        buf.resize(got);
        S.set(sig, bytesValue(buf, n, w));
        if (got < n) break;
    }
    S.set(i.dres, make((uint64_t)total, 0, 32, 0, true));
}

static void readMem(const Instr& i) {
    if (i.pw == 1) { freadMem(i); return; }
    const ArrShape& a = *i.arr;
    auto [l, r] = a.dims[0];
    int64_t lowA = std::min(l, r), highA = std::max(l, r);
    int64_t start = lowA, finish = highA;
    bool haveFinish = false;
    if (i.args.size() > 0) { Val v = eval(i.args[0], rd); if (!v.x) start = v_to_i64(v); }
    if (i.args.size() > 1) { Val v = eval(i.args[1], rd); if (!v.x) { finish = v_to_i64(v); haveFinish = true; } }
    int64_t step = (haveFinish && finish < start) ? -1 : 1;
    if (!haveFinish) finish = highA;
    std::string text;
    if (i.pw == 2) {   // $sreadmemb / $sreadmemh (D.14): the data is in the string arguments
        for (size_t k = 2; k < i.args.size(); k++) { Val sv = eval(i.args[k], rd); sv.str = true; text += v_str(sv) + "\n"; }
    } else {
        std::ifstream in(i.fmt);
        if (!in) { std::cerr << "$readmem: cannot open " << i.fmt << "\n"; return; }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    int64_t addr = start, k = 0;
    const int64_t sub = memSub(a);
    int bits = i.hex ? 4 : 1;
    int64_t words = 0;
    bool sawAddr = false;
    size_t p = 0;
    auto inRange = [&](int64_t ad) {
        return step > 0 ? (ad >= start && ad <= finish) : (ad <= start && ad >= finish);
    };
    while (p < text.size()) {
        char c = text[p];
        if (isspace((unsigned char)c)) { p++; continue; }
        if (c == '/' && p + 1 < text.size() && text[p + 1] == '/') { while (p < text.size() && text[p] != '\n') p++; continue; }
        if (c == '/' && p + 1 < text.size() && text[p + 1] == '*') {
            size_t e = text.find("*/", p + 2);
            p = e == std::string::npos ? text.size() : e + 2;
            continue;
        }
        size_t q = p + (c == '@' ? 1 : 0);
        size_t e = q;
        while (e < text.size() && !isspace((unsigned char)text[e]) && text[e] != '/') e++;
        std::string tok = text.substr(q, e - q);
        p = e;
        if (c == '@') {
            addr = (int64_t)std::stoull(tok, nullptr, 16);
            k = 0;
            sawAddr = true;
            if (!inRange(addr)) { std::cerr << "$readmem: address " << tok << " out of range in " << i.fmt << "\n"; return; }
            continue;
        }
        if (a.elemW > 64) {
            Val acc = make(0, 0, a.elemW);
            Val sh = mk((uint64_t)bits, 8);
            for (char d : tok) {
                if (d == '_') continue;
                Val dig;
                if (d == 'x' || d == 'X') dig = allx(bits);
                else if (d == 'z' || d == 'Z' || d == '?') dig = allz(bits);
                else if (i.hex && isxdigit((unsigned char)d))
                    dig = mk((uint64_t)std::stoi(std::string(1, d), nullptr, 16), bits);
                else if (!i.hex && (d == '0' || d == '1')) dig = mk((uint64_t)(d - '0'), bits);
                else { std::cerr << "$readmem: bad digit '" << d << "' in " << i.fmt << "\n"; return; }
                acc = v_or(v_shl(acc, sh), dig);
            }
            if (!inRange(addr)) break;
            int64_t off = a.offset(memIdx(a, addr, k));
            if (off < 0) break;
            if (a.twoState) {
                acc.v &= ~acc.x; acc.x = acc.z = 0;
                for (auto& h : acc.hi) { h.v &= ~h.x; h.x = h.z = 0; }
            }
            S.set(R(a.base) + (int)off, acc);
            if (++k == sub) { k = 0; addr += step; }
            words++;
            continue;
        }
        uint64_t v = 0, x = 0, z = 0;
        for (char d : tok) {
            if (d == '_') continue;
            v <<= bits; x <<= bits; z <<= bits;
            uint64_t m = (1ULL << bits) - 1;
            if (d == 'x' || d == 'X') x |= m;
            else if (d == 'z' || d == 'Z' || d == '?') { x |= m; z |= m; }
            else if (i.hex && isxdigit((unsigned char)d)) v |= (uint64_t)std::stoi(std::string(1, d), nullptr, 16);
            else if (!i.hex && (d == '0' || d == '1')) v |= (uint64_t)(d - '0');
            else { std::cerr << "$readmem: bad digit '" << d << "' in " << i.fmt << "\n"; return; }
        }
        if (!inRange(addr)) break;
        int64_t off = a.offset(memIdx(a, addr, k));
        if (off < 0) break;
        if (a.twoState) { v &= ~x; x = z = 0; }
        S.set(R(a.base) + (int)off, make(v, x, a.elemW, z));
        if (++k == sub) { k = 0; addr += step; }
        words++;
    }
    // A file with no @address whose word count differs from the range warns
    // (21.4): the range given by start and finish addresses, or, with no
    // finish address, a file with more words than the memory has room for.
    int64_t range = ((step > 0 ? finish - start : start - finish) + 1) * sub;
    if (!sawAddr && (haveFinish ? words != range : words > range))
        std::cerr << "$readmem: warning: " << i.fmt << " has " << words << " words for a range of " << range << "\n";
}

// One fork...join. Each branch reports when it ends; the parent resumes when
// every branch has ended (join) or the first one has (join_any).
inline void dynBump(int sig) { Val v = S.get(sig); S.set(sig, make((v.x ? 0 : v.v) + 1, 0, 64)); }
inline void dynWarn(const char* what) { std::cerr << "warning: " << what << "\n"; }
inline void dynExec(const Instr& i) {
    // Container ids may be relative (automatic variables): resolve them once.
    const int sig = R(i.sig), dsrc = i.dsrc >= 0 ? R(i.dsrc) : -1;
    const int dres = i.dres >= 0 ? R(i.dres) : -1, dkey = i.dkey >= 0 ? R(i.dkey) : -1;
    if (i.dcmd != D_DELETE && i.dcmd != D_DELETE_AT && i.dcmd != D_FIRST && i.dcmd != D_LAST && i.dcmd != D_NEXT && i.dcmd != D_PREV)
        dynAttach(sig);   // a write to an element container made by a read: it joins its array now
    // VPI cbSizeChange: compare the size before and after this command.
    struct SizeWatch {
        int sig = -1;
        size_t n = 0;
        static size_t size(int s) {
            auto it = dynStore().find(s);
            return it == dynStore().end() ? 0 : it->second.kind == 2 ? it->second.map.size() : it->second.vec.size();
        }
        ~SizeWatch() { if (sig >= 0 && size(sig) != n) g_sizeHook(sig); }
    } watch;
    if (__builtin_expect(g_sizeHook != nullptr, 0)) { watch.sig = sig; watch.n = SizeWatch::size(sig); }
    auto& d = dynStore()[sig];
    auto arg = [&](size_t n) { return eval(i.args[n], rd); };
    auto idx = [&](size_t n) { Val v = arg(n); return v.x ? (int64_t)-1 : v_to_i64(v); };
    // A new element: e's value; for a container of containers a copy of
    // container dsrc, or an empty one.
    auto elemVal = [&]() -> Val {
        if (!d.elem) return eval(i.e, rd);
        if (dsrc >= 0) { DynObj one; one.elem = d.elem; return dynCopyElem(one, make((uint64_t)dsrc, 0, 64)); }
        return dynNewElem(d);
    };
    auto full = [&]() {   // a bounded queue drops what does not fit (7.10)
        bool f = d.kind == 1 && d.bound >= 0 && (int64_t)d.vec.size() > d.bound + 1;
        if (f) dynWarn("write past the bound of a bounded queue is ignored");
        return f;
    };
    switch (i.dcmd) {
        case D_SET: {
            Val v = eval(i.e, rd);
            if (d.kind == 2) { d.map[d.key(arg(0))] = v; break; }
            int64_t k = idx(0);
            if (k >= 0 && k < (int64_t)d.vec.size()) d.vec[(size_t)k] = v;
            else if (d.kind == 1 && k == (int64_t)d.vec.size()) { d.vec.push_back(v); if (full()) d.vec.pop_back(); }
            else { dynWarn("write to an index outside the array is ignored (7.4.6)"); return; }
            break;
        }
        case D_NEW: {   // new[n] or new[n](src) (7.5.1)
            int64_t n = idx(0);
            if (n < 0) { dynWarn("new[] with a negative or unknown size"); n = 0; }
            std::vector<Val> nv((size_t)n, d.dflt);
            DynObj src;
            if (dsrc >= 0) src = dynStore()[dsrc];
            for (size_t k = 0; k < nv.size(); k++)
                nv[k] = k < src.vec.size() ? dynCopyElem(src, src.vec[k]) : dynNewElem(d);
            d.vec = std::move(nv);
            break;
        }
        case D_DELETE: d.vec.clear(); d.map.clear(); break;
        case D_DEFAULT:   // with a value: the new default; without: back to the type's own
            if (i.e) { if (!d.userDflt) d.typeDflt = d.dflt; d.dflt = eval(i.e, rd); d.userDflt = true; }
            else if (d.userDflt) { d.dflt = d.typeDflt; d.userDflt = false; }
            break;
        case D_DELETE_AT: {
            if (d.kind == 2) { d.map.erase(d.key(arg(0))); break; }
            int64_t k = idx(0);
            if (k < 0 || k >= (int64_t)d.vec.size()) { dynWarn("delete(i) outside the queue"); return; }
            d.vec.erase(d.vec.begin() + k);
            break;
        }
        case D_PUSH_BACK: d.vec.push_back(elemVal()); if (full()) d.vec.pop_back(); break;
        case D_PUSH_FRONT: d.vec.insert(d.vec.begin(), elemVal()); if (full()) d.vec.pop_back(); break;
        case D_INSERT: {
            int64_t k = idx(0);
            if (k < 0 || k > (int64_t)d.vec.size()) { dynWarn("insert(i) outside the queue"); return; }
            d.vec.insert(d.vec.begin() + k, elemVal());
            if (full()) d.vec.pop_back();
            break;
        }
        case D_POP_FRONT:
        case D_POP_BACK: {
            Val v = d.dflt;
            if (d.vec.empty()) dynWarn("pop from an empty queue");
            else if (i.dcmd == D_POP_FRONT) { v = d.vec.front(); d.vec.erase(d.vec.begin()); }
            else { v = d.vec.back(); d.vec.pop_back(); }
            if (dres >= 0) S.set(dres, v);
            break;
        }
        case D_FIRST: case D_LAST: case D_NEXT: case D_PREV: {   // 7.9.4-7.9.7
            bool ok = false;
            DynKey cur = d.key(S.get(dkey));
            std::map<DynKey, Val>::iterator it = d.map.end();
            if (i.dcmd == D_FIRST) it = d.map.begin();
            else if (i.dcmd == D_LAST) { if (!d.map.empty()) it = std::prev(d.map.end()); }
            else if (i.dcmd == D_NEXT) it = d.map.upper_bound(cur);
            else { it = d.map.lower_bound(cur); it = it == d.map.begin() ? d.map.end() : std::prev(it); }
            if (it != d.map.end()) { ok = true; S.set(dkey, d.keyVal(it->first)); }
            if (dres >= 0) { Val r = make(ok, 0, 32); r.sgn = true; S.set(dres, r); }
            return;   // the container did not change
        }
        case D_COPY: {
            dynAssign(d, dynStore()[dsrc]);
            if (const DynObj& so = dynStore()[dsrc]; so.userDflt && d.kind == 2) {   // the default travels with the array (7.9.11)
                if (!d.userDflt) d.typeDflt = d.dflt;
                d.dflt = so.dflt; d.userDflt = true;
            }
            break;
        }
        case D_APPEND: {   // {q, r}: every element of r
            DynObj s = dynStore()[dsrc];
            for (const Val& e : s.vec) d.vec.push_back(dynCopyElem(s, e));
            break;
        }
        case D_APPEND_SLICE: {   // q[a:b] (7.10.1): out-of-range parts are left out
            DynObj so = dynStore()[dsrc];
            const auto& s = so.vec;
            int64_t a = std::max<int64_t>(idx(0), 0), b = std::min<int64_t>(idx(1), (int64_t)s.size() - 1);
            for (int64_t k = a; k <= b; k++) d.vec.push_back(dynCopyElem(so, s[(size_t)k]));
            break;
        }
        case D_REVERSE: std::reverse(d.vec.begin(), d.vec.end()); break;
        case D_SORT:
        case D_RSORT: {
            bool down = i.dcmd == D_RSORT;
            std::stable_sort(d.vec.begin(), d.vec.end(), [&](const Val& a, const Val& b) {
                if (a.str || b.str) return down ? v_str(b) < v_str(a) : v_str(a) < v_str(b);
                if (a.real || b.real) return down ? v_real(b) < v_real(a) : v_real(a) < v_real(b);
                Val lt = down ? v_lt(b, a) : v_lt(a, b);
                return !lt.x && lt.v;
            });
            break;
        }
        case D_UNPACK: {   // the stream e, cut into elements from the left; the last is padded on the right
            Val v = eval(i.e, rd);
            int ew = d.dflt.w, W = v.w;
            d.vec.clear();
            for (int hi = W - 1; hi >= 0; hi -= ew) {
                int lo = hi - ew + 1;
                Val el = lo >= 0 ? v_slice(v, hi, lo) : v_concat({v_slice(v, hi, 0), make(0, 0, -lo, 0)});
                el.sgn = d.dflt.sgn;
                d.vec.push_back(el);
                if (full()) { d.vec.pop_back(); break; }
            }
            break;
        }
        case D_SORT_BY:
        case D_RSORT_BY: {   // order d by the keys in dsrc, one per element
            bool down = i.dcmd == D_RSORT_BY;
            const auto& ks = dynStore()[i.dsrc].vec;
            if (ks.size() != d.vec.size()) break;
            std::vector<size_t> ord(ks.size());
            for (size_t k = 0; k < ord.size(); k++) ord[k] = k;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t x, size_t y) {
                const Val& a = down ? ks[y] : ks[x];
                const Val& b = down ? ks[x] : ks[y];
                if (a.str || b.str) return v_str(a) < v_str(b);
                if (a.real || b.real) return v_real(a) < v_real(b);
                Val lt = v_lt(a, b);
                return !lt.x && lt.v;
            });
            std::vector<Val> nv;
            nv.reserve(ord.size());
            for (size_t k : ord) nv.push_back(d.vec[k]);
            d.vec = std::move(nv);
            break;
        }
        case D_SHUFFLE: {
            static uint64_t seed = 0x9e3779b97f4a7c15ULL;
            for (size_t k = d.vec.size(); k > 1; k--) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                std::swap(d.vec[k - 1], d.vec[seed % k]);
            }
            break;
        }
    }
    dynBump(sig);
}

// new (8.7) and a shallow copy `new h` (8.12). Objects are never freed.
inline void newObject(const Instr& i) {
    int src = 0, cls = i.hi;
    if (i.e) {
        Val h = eval(i.e, rd);
        if (h.x || !h.v) nullAccess();
        src = (int)h.v;
        cls = (int)S.sigs[src].val.v;
    }
    const FrameLayout& L = *classRt()[(size_t)cls].lay;
    int obj = g_allocSigs(L);
    g_initSigs(L, obj);
    if (src)
        for (size_t k = 0; k < L.w.size(); k++) S.sigs[obj + (int)k].val = S.sigs[src + (int)k].val;
    if (src)
        for (int off : L.noCopy) S.sigs[obj + off].val = L.init[(size_t)off];
    if (src)
        for (auto& [off, o] : L.dyn) dynStore()[obj + off] = dynStore()[src + off];
    // Its own random generator (18.14.3): seeded from the $urandom stream, or
    // the source's state for a copy.
    if (src) objRng()[obj] = objRng()[src];
    else { uint64_t a = urandomNext(urandomState()), b = urandomNext(urandomState()); objRng()[obj] = seedState(a << 32 | b); }
    S.set(R(i.sig), make((uint64_t)obj, 0, 64));
    if (__builtin_expect(g_newObjHook != nullptr, 0)) g_newObjHook(obj);
}

struct JoinState {
    int left = 0;          // branches still running
    int join = 0;          // 0 join, 1 join_any, 2 join_none
    Handle parent{};
    bool waiting = false;  // parent is suspended on this fork
    bool ready() const { return join == 2 || left == 0 || (join == 1 && done > 0); }
    int done = 0;
    void branchEnded() {
        left--;
        done++;
        if (waiting && ready()) { waiting = false; S.active.push_back(parent); }
    }
};
struct JoinWait {
    shared_ptr<JoinState> js;
    bool await_ready() const { return js->ready(); }
    void await_suspend(Handle h) const { js->parent = h; js->waiting = true; }
    void await_resume() const {}
};

struct ProcCtl;
// DPI-C imports (35) are run by dpi.inc, which msim links in. A call that
// returns a JoinState is an import task waiting on an exported task that
// consumes time: the process waits for it, then calls resume. kill runs a
// waiting import task's C code to its end after the process is disabled (35.9).
struct DpiHook {
    shared_ptr<JoinState> (*call)(const Instr&, void*& fiber, const shared_ptr<ProcCtl>& ctl) = nullptr;
    shared_ptr<JoinState> (*resume)(void* fiber) = nullptr;
    void (*kill)(void* fiber) = nullptr;
};
inline DpiHook& dpiHook() { static DpiHook h; return h; }
// VPI (36-38), set by vpi.inc: run the calltf of a user system task or function.
inline void (*&vpiCallHook())(const Instr&) { static void (*h)(const Instr&) = nullptr; return h; }

// One running process: its children (for wait fork / disable fork) and the
// fork it belongs to. A disabled process ends at once for everyone waiting
// on it; its coroutine, if it is resumed later, just returns.
struct ProcCtl {
    Handle h{};
    bool killed = false, ended = false, waitingFork = false;
    // std::process (9.7): suspended processes park at their next resume
    // point; await() waiters resume when this one ends.
    bool suspended = false, parked = false;
    vector<Handle> awaiters;
    int obj = 0;   // its process object, once process::self() made one
    // Its random generator (18.14.2): seeded from its parent's when forked,
    // from the global one for a top-level process. rngp names it: rng, the
    // process object's once there is one (process::srandom), or the caller's
    // for a synchronous helper (pre_randomize, a constraint's function).
    uint64_t rng = 1;
    uint64_t* rngp = &rng;
    ProcCtl() = default;
    ProcCtl(const ProcCtl&) = delete;
    ~ProcCtl() { if (g_threadRng == &rng) g_threadRng = nullptr; }
    weak_ptr<ProcCtl> parent;
    vector<shared_ptr<ProcCtl>> kids;
    shared_ptr<JoinState> join;
    void* dpiFiber = nullptr;   // an import task waiting on an exported task (dpi.inc)
    // Deferred assertions (16.4.2): a resume after an event control is a
    // flush point; it bumps `epoch`, and reports queued before it are dropped.
    uint64_t epoch = 0;
    // Written before the process starts: a deferred assertion action's
    // captured arguments, written into their slots (16.4.1).
    vector<pair<Expr*, Val>> init;
    // A program thread (24.3.1): the program instance, or -1. It and every
    // thread it forks run in the reactive region set. progRoot: one of the
    // program's initial procedures.
    int program = -1;
    bool progRoot = false;
    bool reactive = false;   // a program or checker thread: runs in the reactive region set
    bool kidsDone() const {
        for (auto& k : kids) if (!k->ended) return false;
        return true;
    }
    // Where it is, for disable (9.6.2): the running coroutine's call stack,
    // code and frame (its locals), the code it started from, and where its
    // parent forked it. A disable that moves a waiting process starts a new
    // coroutine at the new place (restart) and bumps gen: the old one stops
    // when it next runs.
    vector<RetFrame>* rets = nullptr;
    const Prog** pp = nullptr;
    size_t* ppc = nullptr;
    FrameP* pfr = nullptr;
    const Prog* entry = nullptr;
    FrameP fr0;
    uint64_t gen = 0;
    struct Restart { vector<RetFrame> rets; const Prog* p; size_t pc; FrameP fr; };
    std::unique_ptr<Restart> restart;
    size_t forkDepth = 0, forkPc = 0;
    const Prog* forkProg = nullptr;
};
inline unordered_map<int, vector<weak_ptr<ProcCtl>>>& rootProcs() {
    static unordered_map<int, vector<weak_ptr<ProcCtl>>> m;
    return m;
}
inline void programRootEnded(int p);
inline void procEnd(ProcCtl& c) {
    if (c.ended) return;
    c.ended = true;
    if (c.progRoot) programRootEnded(c.program);
    for (Handle h : c.awaiters) S.active.push_back(h);
    c.awaiters.clear();
    if (c.join) c.join->branchEnded();
    if (auto p = c.parent.lock())
        if (p->waitingFork && p->kidsDone()) { p->waitingFork = false; S.active.push_back(p->h); }
}
inline void procKill(const shared_ptr<ProcCtl>& c) {
    if (c->ended) return;
    c->killed = true;
    for (auto& k : c->kids) procKill(k);
    if (c->dpiFiber && dpiHook().kill) {
        void* f = c->dpiFiber;
        c->dpiFiber = nullptr;
        dpiHook().kill(f);
    }
    S.dead.insert(c->h.address());
    procEnd(*c);
}
struct WaitFork {
    shared_ptr<ProcCtl> c;
    bool await_ready() const { return c->kidsDone(); }
    void await_suspend(Handle h) const { c->h = h; c->waitingFork = true; }
    void await_resume() const {}
};

inline Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl, FrameP fr);

// ---- programs (24.3, 24.7) ----
// When every initial procedure of a program has ended, the program ends:
// the threads they forked are killed. When every program with an initial
// procedure has ended, the run ends with an implicit $finish.
struct ProgramRun { vector<shared_ptr<ProcCtl>> roots; int live = 0; bool ended = false; };
inline vector<ProgramRun>& programRuns() { static vector<ProgramRun> v; return v; }
inline int& programsLive() { static int n = 0; return n; }
inline void programRootEnded(int p) {
    ProgramRun& r = programRuns()[p];
    if (--r.live > 0 || r.ended) return;
    r.ended = true;
    for (auto& c : r.roots) {
        auto kids = c->kids;
        for (auto& k : kids) procKill(k);
    }
    if (--programsLive() == 0) S.finished = true;
}
// $exit (24.7): end the calling thread's program. Outside a program it is
// ignored, with a warning.
inline void programExit(const shared_ptr<ProcCtl>& ctl) {
    if (ctl->program < 0) {
        std::cerr << "msim: warning: $exit outside a program is ignored (24.7)\n";
        return;
    }
    auto roots = programRuns()[ctl->program].roots;
    for (auto& c : roots) procKill(c);
}
// std::process (9.7): process objects index this table.
inline vector<shared_ptr<ProcCtl>>& procTable() { static vector<shared_ptr<ProcCtl>> v; return v; }
struct ProcAwait {   // p.await(): until p ends
    ProcCtl* c;
    bool await_ready() const { return c->ended; }
    void await_suspend(Handle h) const { c->awaiters.push_back(h); }
    void await_resume() const {}
};
struct ProcPark {    // a suspended process, until resume()
    ProcCtl* c;
    bool await_ready() const { return !c->suspended; }
    void await_suspend(Handle h) const { c->h = h; c->parked = true; }
    void await_resume() const {}
};
static ProcCtl* procOf(const Instr& i) {
    Val h = eval(i.e, rd);
    if (h.x || !h.v) nullAccess();
    Val n = S.get((int)h.v + i.dkey);
    if (n.x || n.v >= procTable().size()) relFail("a process object with no process");
    return procTable()[n.v].get();
}
static int procStatus(const ProcCtl* c, const ProcCtl* self) {
    if (c->killed) return 4;      // KILLED
    if (c->ended) return 0;       // FINISHED
    if (c->suspended) return 3;   // SUSPENDED
    return c == self ? 1 : 2;     // RUNNING, WAITING
}
static void procResume(ProcCtl* c) {
    c->suspended = false;
    if (c->parked && !c->ended) { c->parked = false; S.active.push_back(c->h); }
}
static int doRandomize(int obj, const RandCall* rc);   // rand.inc
static void randModeAll(int obj, bool blocks, bool on);
inline vector<weak_ptr<ProcCtl>>& liveProcs() { static vector<weak_ptr<ProcCtl>> v; return v; }
inline Task startProc(const Prog* p, shared_ptr<ProcCtl> c, FrameP fr = nullptr) {
    c->entry = p;
    c->fr0 = fr;
    Task t = exec(p, c, std::move(fr));
    c->h = t.h;
    for (int r : p->roots) rootProcs()[r].push_back(c);
    if (anySpans()) liveProcs().push_back(c);
    return t;
}
// disable of block or task id (9.6.2) for every process inside it: it
// resumes after the block, or after the call of the task; what it forked
// in there ends. Returns whether self was moved (its own locals are set).
inline bool disableSpans(int id, const shared_ptr<ProcCtl>& self) {
    auto& L = liveProcs();
    std::erase_if(L, [](auto& w) { auto c = w.lock(); return !c || c->ended; });
    auto list = L;
    bool moved = false;
    for (auto& w : list) {
        auto c = w.lock();
        if (!c || c->ended || c->killed || !c->rets) continue;
        auto& R = *c->rets;
        size_t n = R.size();
        // The outermost frame inside the span: frame k runs R[k].prog at
        // R[k].pc (the return address of its call) for k < n, else the
        // current code and pc.
        size_t hit = SIZE_MAX;
        const Prog::Span* sp = nullptr;
        for (size_t k = 0; k <= n && hit == SIZE_MAX; k++) {
            const Prog* P = k < n ? R[k].prog : *c->pp;
            size_t pc = k < n ? R[k].pc : *c->ppc;
            for (auto& x : P->spans) {
                if (x.id != id) continue;
                bool in = x.e == SIZE_MAX ? k > 0
                        : k < n ? x.s < pc && pc <= x.e
                        : (x.s <= pc && pc < x.e) || (pc == x.e && pc > x.s && P->code[pc - 1].k == IK::Fork);
                if (in) { hit = k; sp = &x; break; }
            }
        }
        if (hit == SIZE_MAX) continue;
        bool task = sp->e == SIZE_MAX;
        const Prog* hp = hit < n ? R[hit].prog : *c->pp;
        // What it forked in there ends with it.
        for (auto& kid : c->kids) {
            if (kid->ended) continue;
            bool inside = task ? kid->forkDepth >= hit
                        : kid->forkDepth > hit || (kid->forkDepth == hit && kid->forkProg == hp && sp->s <= kid->forkPc && kid->forkPc < sp->e);
            if (inside) procKill(kid);
        }
        std::erase_if(c->kids, [](auto& k) { return k->ended; });
        auto r = std::make_unique<ProcCtl::Restart>();
        if (task) {   // back in the caller, after the call
            r->rets.assign(R.begin(), R.begin() + (long)(hit - 1));
            r->p = R[hit - 1].prog; r->pc = R[hit - 1].pc; r->fr = R[hit - 1].fr;
        } else {      // after the block, in the frame that runs it
            r->rets.assign(R.begin(), R.begin() + (long)hit);
            r->p = hp; r->pc = sp->e; r->fr = hit < n ? R[hit].fr : *c->pfr;
        }
        if (c == self) {
            *c->rets = std::move(r->rets);
            *c->pp = r->p; *c->ppc = r->pc; *c->pfr = std::move(r->fr);
            moved = true;
            continue;
        }
        // A waiting process: its wait is dropped, and it goes on from there now.
        c->gen++;
        c->rets = nullptr;   // until the new coroutine runs
        S.dead.insert(c->h.address());
        c->restart = std::move(r);
        c->waitingFork = false;
        c->parked = false;
        Task t = exec(c->entry, c, c->fr0);
        c->h = t.h;
        S.all.push_back(t.h);
        S.active.push_back(t.h);
    }
    return moved;
}
// A top-level process (initial, always, final).
// A new top-level process's generator (18.14.2), seeded in start order.
inline void seedProc(ProcCtl& c) {
    uint64_t& g = globalRandomState();   // seeded in the order the processes start
    uint64_t a = urandomNext(g), b = urandomNext(g);
    c.rng = seedState(a << 32 | b);
}
inline Task process(const Prog* p) {
    auto c = make_shared<ProcCtl>();
    seedProc(*c);
    return startProc(p, std::move(c), nullptr);
}
// A process of a program (24.3): an initial procedure, or a continuous assign.
inline Task programProc(const Prog* prog, int program, bool initial) {
    auto c = make_shared<ProcCtl>();
    seedProc(*c);
    c->program = program;
    c->reactive = true;
    if (initial && program >= 0) {
        auto& runs = programRuns();
        if ((size_t)program >= runs.size()) runs.resize(program + 1);
        ProgramRun& r = runs[program];
        if (r.roots.empty()) programsLive()++;
        r.roots.push_back(c);
        r.live++;
        c->progRoot = true;
    }
    return startProc(prog, c, nullptr);
}
inline Task exec(const Prog* p) { return process(p); }

// ---- assertions (16.3, 16.4, 20.11) ----
// An immediate assertion. Returns the next pc. A simple one picks its
// action by jumping; a deferred one queues a report on this process with
// the action's arguments evaluated now (16.4).
static void queueConc(const Instr& i, const shared_ptr<ProcCtl>& ctl);
static void cbDrive(const Instr& i, Val v, int hi, int lo);
static size_t runAssert(const Instr& i, const shared_ptr<ProcCtl>& ctl, size_t pc) {
    if (i.join == 3) { queueConc(i, ctl); return pc + 1; }   // a procedural concurrent assertion
    AssertInfo& a = asserts()[i.sig];
    if (!a.on) return i.join ? pc + 1 : i.hi;
    bool ok = truth(eval(i.e, rd)) == 1;
    a.evals++;
    (ok ? a.passes : a.fails)++;
    if (assertHook()) {   // an immediate assertion's attempt starts and ends at once (39.4.2)
        assertEvent(i.sig, AE_START, S.now);
        assertEvent(i.sig, ok ? AE_SUCCESS : AE_FAILURE, S.now);
    }
    if (!i.join) {
        if (ok) return a.passOn ? pc + 1 : i.hi;
        return a.failOn ? i.target : i.hi;
    }
    if (ok ? (!a.pass || !a.passOn) : (!a.fail || !a.failOn)) return pc + 1;
    DeferRep r{i.sig, ok, ctl, ctl->epoch, a.killGen, {}};
    const auto& cap = ok ? a.passCap : a.failCap;
    const auto& sl = ok ? a.passSlot : a.failSlot;
    for (size_t k = 0; k < cap.size(); k++) r.caps.push_back({sl[k].get(), eval(cap[k], rd)});
    (i.join == 2 ? S.defFin : S.defObs).push_back(std::move(r));
    S.rare = true;
    return pc + 1;
}
// Reports still standing when they mature: not flushed by their process
// (a later flush point or a disable), and not killed by $assertkill.
static bool reportLive(const DeferRep& r) {
    return !r.ctl->killed && r.ctl->epoch == r.epoch && asserts()[r.id].killGen == r.killGen;
}
static Task startReport(DeferRep& r) {
    const AssertInfo& a = asserts()[r.id];
    auto c = make_shared<ProcCtl>();
    c->init = std::move(r.caps);
    return startProc(r.pass ? a.pass : a.fail, c);
}
// Observed region: concurrent assertions waiting here run, then observed
// deferred reports mature and their actions go to the reactive region (16.4.1).
inline void Sim::runObserved() {
    region = R_OBSERVED;
    while (!observedQ.empty()) {
        Handle h = observedQ.front();
        observedQ.pop_front();
        if (!h.done()) h.resume();
    }
    // Pending procedural concurrent assertions mature (16.14.6); those whose
    // clock did not tick in this step start at its next tick.
    for (int k : concPending()) {
        ConcAssert& c = concAsserts()[k];
        for (auto& p : c.pending) {
            auto* pc = static_cast<ProcCtl*>(p.ctl.get());
            if (!pc->killed && pc->epoch == p.epoch) c.matured.push_back(p);
        }
        c.pending.clear();
    }
    concPending().clear();
    vector<DeferRep> reps;
    reps.swap(defObs);
    for (auto& r : reps)
        if (reportLive(r)) {
            Task t = startReport(r);
            all.push_back(t.h);
            reactive.push_back(t.h);
            rare = true;
        }
    region = R_ACTIVE;
}
// Postponed region: final deferred reports mature and their actions run now.
inline void Sim::matureFinal() {
    vector<DeferRep> reps;
    reps.swap(defFin);
    for (auto& r : reps)
        if (reportLive(r)) {
            Task t = startReport(r);
            all.push_back(t.h);
            t.h.resume();
        }
}
// $assertcontrol and friends (20.11). args: control, assertion type,
// directive type, levels; fmt: the target scopes and assertions, one per line.
static void assertCtl(const Instr& i) {
    int v[4];
    for (int k = 0; k < 4; k++) v[k] = (int)v_to_i64(eval(i.args[k], rd));
    vector<string> targets;
    size_t b = 0;
    while (b < i.fmt.size()) {
        size_t e = i.fmt.find('\n', b);
        if (e == string::npos) e = i.fmt.size();
        if (e > b) targets.push_back(i.fmt.substr(b, e - b));
        b = e + 1;
    }
    assertControl(v[0], v[1], v[2], v[3], targets);
    if (v[0] == AC_KILL)   // pending deferred reports of killed assertions are flushed
        for (auto* q : {&S.defObs, &S.defFin})
            std::erase_if(*q, [](const DeferRep& r) { return asserts()[r.id].killGen != r.killGen; });
}
// ---- concurrent assertions (16.14) ----
struct ToObserved {   // resume in the observed region of this time slot (4.4.2.5)
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const { S.observedQ.push_back(h); S.rare = true; }
    void await_resume() const {}
};
// Pass and fail actions run in the reactive region (16.14.1), once per attempt.
// `slots` take the attempt's captured automatic values (16.14.6.1).
static void runAction(const Prog* p, uint64_t n, const vector<E>* slots = nullptr, const vector<Val>* caps = nullptr) {
    if (!p) return;
    for (uint64_t k = 0; k < n; k++) {
        auto ctl = make_shared<ProcCtl>();
        if (slots && caps)
            for (size_t j = 0; j < slots->size(); j++) ctl->init.push_back({(*slots)[j].get(), (*caps)[j]});
        Task t = startProc(p, ctl);
        S.oneShot.insert(t.h.address());   // freed after its run (Sim::oneShot)
        S.reactive.push_back(t.h);
    }
    S.rare = true;
}
// Local variables of a concurrent assertion (16.10): environments are
// interned value vectors; an atom or a match item that reads local variables
// is evaluated with their hidden signals set (without events) from an environment.
struct LocalEnv final : sva::LocalHook {
    ConcAssert& c;
    int loaded = -1;
    vector<vector<Val>> tbl;                    // this attempt's environments
    uint64_t count = 1;                         // the attempts merged into this one
    std::unordered_map<uint64_t, bool> cache;   // (env, atom) -> value, for one attempt
    explicit LocalEnv(ConcAssert& c) : c(c) {}
    static void poke(int s, Val v) {
        int w = S.sigs[s].w;
        if (w <= 64) {
            uint64_t m = w >= 64 ? ~0ULL : ((1ULL << w) - 1);
            v.v &= m; v.x &= m; v.z &= m; v.sgn = false;
            v.hi.clear();
        } else v = maskWidth(v, w);
        S.sigs[s].val = v;
    }
    void start(const ConcAssert::Envs& e) {
        if (e) tbl = *e;
        else {   // the initial environment: every local variable unassigned (x)
            tbl.assign(1, {});
            for (int s : c.localSig) tbl[0].push_back(allx(S.sigs[s].w));
        }
        loaded = -1;
        cache.clear();
    }
    void load(int e) {
        if (loaded == e) return;
        for (size_t k = 0; k < c.localSig.size(); k++) poke(c.localSig[k], tbl[e][k]);
        loaded = e;
    }
    int intern(vector<Val> v) {
        for (size_t i = 0; i < tbl.size(); i++)
            if (tbl[i] == v) return (int)i;
        tbl.push_back(std::move(v));
        return (int)tbl.size() - 1;
    }
    bool atom(int a, int e) override {
        uint64_t key = (uint64_t)(uint32_t)e << 32 | (uint32_t)a;
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        load(e);
        bool v = truth(eval(c.atoms[a], rd)) == 1;
        cache[key] = v;
        return v;
    }
    int apply(int act, int e) override {
        load(e);
        for (auto& item : c.acts[act]) {   // in order: later items see earlier ones
            if (item.k >= 0) { poke(c.localSig[item.k], eval(item.rhs, rd)); continue; }
            vector<Val> args;   // a subroutine call (16.11): its arguments now, the call in the reactive region
            for (const E& x : item.caps) args.push_back(eval(x, rd));
            runAction(item.call, count, &item.slots, &args);
        }
        vector<Val> v;
        for (int s : c.localSig) v.push_back(S.get(s));
        int id = intern(std::move(v));
        loaded = id;
        return id;
    }
    int merge(int ctx, int a, int b) override {
        if (a == b || b == ctx) return a;
        if (a == ctx) return b;
        vector<Val> v = tbl[a];
        for (size_t k = 0; k < v.size(); k++)
            if (!(tbl[b][k] == tbl[ctx][k])) v[k] = tbl[b][k];
        return intern(std::move(v));
    }
    // The stepped term with its environments numbered in order of use.
    std::pair<const sva::Node*, ConcAssert::Envs> finish(const sva::Node* t) {
        if (!t->envDep) return {t, nullptr};
        vector<int> used{0};
        std::unordered_set<const sva::Node*> seen;
        sva::Store::envsUsed(t, used, seen);
        std::unordered_map<int, int> to;
        auto out = make_shared<vector<vector<Val>>>();
        bool same = true;
        for (size_t i = 0; i < used.size(); i++) {
            to[used[i]] = (int)i;
            same = same && used[i] == (int)i;
            out->push_back(tbl[used[i]]);
        }
        if (!same) {
            std::unordered_map<const sva::Node*, const sva::Node*> memo;
            t = sva::store().renumber(t, to, memo);
        }
        return {t, out};
    }
};
// One clock tick of a concurrent assertion, in the observed region: start an
// attempt, then step every attempt on the sampled values of the atoms.
// An expect statement (16.17) waiting for its attempt's verdict.
struct ExpectWait {
    Handle h;
    shared_ptr<ProcCtl> ctl;
    int res = -1;   // 1 pass, 0 fail, 2 disabled or killed
};
struct ExpectAwait {
    ExpectWait* w;
    bool await_ready() const { return false; }
    void await_suspend(Handle h) const { w->h = h; }
    void await_resume() const {}
};
// The verdict of an expect attempt: its process goes on in the reactive region.
static void expectDone(const shared_ptr<void>& p, int res) {
    auto* w = static_cast<ExpectWait*>(p.get());
    if (!w || w->res >= 0) return;
    w->res = res;
    if (w->ctl->killed || !w->h) return;
    S.reactive.push_back(w->h);
    S.rare = true;
}
static void dropAttempts(ConcAssert& c, int ev = AE_DISABLED) {   // disabled or killed: expect statements go on
    if (assertHook() && !asserts()[c.id].hidden)
        for (auto& at : c.att)
            for (uint64_t n = 0; n < at.count; n++) assertEvent(c.id, ev, at.start);
    for (auto& at : c.att) expectDone(at.wait, 2);
    for (auto& p : c.matured) expectDone(p.wait, 2);
    c.att.clear();
    c.matured.clear();
}
// expect (property) action (16.17): an attempt from the next tick of its clock.
static shared_ptr<ExpectWait> startExpect(const Instr& i, const shared_ptr<ProcCtl>& ctl) {
    ConcAssert& c = concAsserts()[i.hi];
    auto w = make_shared<ExpectWait>();
    w->ctl = ctl;
    ConcAssert::Caps caps;
    if (!c.capSig.empty()) {
        auto v = make_shared<vector<Val>>();
        for (int s : c.capSig) v->push_back(S.get(R(s)));
        caps = v;
    }
    c.matured.push_back({ctl, ctl->epoch, caps, w});
    return w;
}
// `ticked`: the clocks that ticked (16.13); attempts start at the leading one.
static void concTick(ConcAssert& c, uint32_t ticked = ~0u) {
    AssertInfo& a = asserts()[c.id];
    if (!c.att.empty() && c.att.front().killGen != a.killGen)   // $assertkill (20.11)
        std::erase_if(c.att, [&](const ConcAssert::Attempt& t) {
            if (t.killGen == a.killGen) return false;
            if (!a.hidden) for (uint64_t n = 0; n < t.count; n++) assertEvent(c.id, AE_KILLED, t.start);
            expectDone(t.wait, 2);
            return true;
        });
    if (c.disable && truth(eval(c.disable, rd)) == 1) {   // disabled: no pass, no fail (16.12)
        dropAttempts(c);
        c.pending.clear();
        if (a.on && (ticked & 1)) {
            a.evals++;
            if (assertHook() && !a.hidden && !c.procedural) {   // the attempt of this tick starts disabled (39.4.2)
                assertEvent(c.id, AE_START, S.now);
                assertEvent(c.id, AE_DISABLED, S.now);
            }
        }
        return;
    }
    if (!(ticked & 1)) {
    } else if (c.procedural) {   // instances queued by the procedure in this step, or matured earlier
        for (auto& p : c.matured) { c.att.push_back({c.root, false, 1, a.killGen, p.caps, nullptr, p.wait, S.now}); a.evals++; assertEvent(c.id, AE_START, S.now); }
        c.matured.clear();
        for (auto& p : c.pending) {
            auto* pc = static_cast<ProcCtl*>(p.ctl.get());
            if (!pc->killed && pc->epoch == p.epoch) {
                c.att.push_back({c.root, false, 1, a.killGen, p.caps, nullptr, nullptr, S.now});
                a.evals++;
                assertEvent(c.id, AE_START, S.now);
            }
        }
        c.pending.clear();
    } else if (a.on) {
        c.att.push_back({c.root, false, 1, a.killGen, nullptr, nullptr, nullptr, S.now});
        a.evals++;
        if (!a.hidden) assertEvent(c.id, AE_START, S.now);
    }
    if (c.att.empty()) {
        for (int f : c.clearFlags) S.set(f, mk(0, 1));
        return;
    }
    // The atoms' values; with captured automatic values (16.14.6.1), once per
    // distinct set of them.
    vector<char> val(c.atoms.size());
    const vector<Val>* valCaps = nullptr;
    bool have = false;
    auto values = [&](const ConcAssert::Caps& caps) {
        if (have && (c.capSlot.empty() || valCaps == caps.get())) return;
        if (caps)
            for (size_t k = 0; k < c.capSlot.size(); k++) c.capSlot[k]->k = (*caps)[k];
        valCaps = caps.get();
        have = true;
        for (size_t i = 0; i < c.atoms.size(); i++) val[i] = truth(eval(c.atoms[i], rd)) == 1;
    };
    sva::Deriv dv{sva::store(), val};
    dv.ticking = ticked;
    std::optional<LocalEnv> lenv;
    if (!c.acts.empty()) {   // local variables or subroutine calls in match items
        lenv.emplace(c);
        dv.lh = &*lenv;
    }
    vector<ConcAssert::Attempt> next;
    bool ended = false;   // a match of an end point tracker
    for (auto& at : c.att) {
        values(at.caps);
        if (lenv) { lenv->start(at.envs); lenv->count = at.count; }
        dv.env = 0;
        dv.memo.clear();
        dv.nonvac = at.nonvac;
        dv.matches = 0;
        const sva::Node* t = dv.d(at.t);
        ConcAssert::Envs envs;
        if (lenv) std::tie(t, envs) = lenv->finish(t);
        if (a.step && assertHook() && !a.hidden) {   // VPI stepping (39.5.2): this attempt's step
            assertStep() = {at.t, t};
            for (uint64_t n = 0; n < at.count; n++) assertEvent(c.id, t->k == sva::PFALSE ? AE_STEP_FAIL : AE_STEP_OK, at.start);
        }
        if (c.coverSeq) {   // every match counts (16.14.3)
            if (dv.matches) ended = true;
            if (dv.matches && !a.hidden) {
                a.passes += at.count * dv.matches;
                for (uint64_t n = 0; assertHook() && n < at.count * dv.matches; n++) assertEvent(c.id, AE_SUCCESS, at.start);
                if (a.passOn) runAction(c.pass, at.count * dv.matches, &c.capSlot, at.caps.get());
            }
            if (t->k == sva::DONE) continue;
        } else if (t->k == sva::PTRUE) {
            a.passes += at.count;
            for (uint64_t n = 0; assertHook() && n < at.count; n++) assertEvent(c.id, dv.nonvac ? AE_SUCCESS : AE_VACUOUS, at.start);
            expectDone(at.wait, 1);
            if (dv.nonvac ? a.passOn : a.vacOn) runAction(c.pass, at.count, &c.capSlot, at.caps.get());
            continue;
        } else if (t->k == sva::PFALSE) {
            a.fails += at.count;
            for (uint64_t n = 0; assertHook() && n < at.count; n++) assertEvent(c.id, AE_FAILURE, at.start);
            expectDone(at.wait, 0);
            if (a.failOn) runAction(c.fail, at.count, &c.capSlot, at.caps.get());
            continue;
        }
        bool merged = false;
        for (auto& n : next)
            if (n.t == t && n.nonvac == dv.nonvac && !n.wait && !at.wait && (!assertHook() || n.start == at.start) && (n.caps == at.caps || (n.caps && at.caps && *n.caps == *at.caps)) &&
                (n.envs == envs || (n.envs && envs && *n.envs == *envs))) {
                n.count += at.count;
                merged = true;
                break;
            }
        if (!merged) next.push_back({t, dv.nonvac, at.count, at.killGen, at.caps, std::move(envs), at.wait, at.start});
    }
    c.att.swap(next);
    if (ended && c.endSig >= 0) {   // the sequence's end point (16.13.6)
        Val n = S.get(c.endSig);
        S.set(c.endSig, mk((n.x ? 0 : n.v) + 1, 32));
        S.set(c.endSig + 1, mk(S.now + 1, 64));
        for (int f : c.matchFlags) S.set(f, mk(1, 1));
    }
    for (int f : c.clearFlags) S.set(f, mk(0, 1));
}
// A procedural concurrent assertion reached by its procedure (16.14.6):
// queue an instance on this process, with its automatic values now.
static void queueConc(const Instr& i, const shared_ptr<ProcCtl>& ctl) {
    ConcAssert& c = concAsserts()[i.hi];
    if (!asserts()[c.id].on) return;   // $assertoff: no new instances (20.11)
    ConcAssert::Caps caps;
    if (!c.capSig.empty()) {
        auto v = make_shared<vector<Val>>();
        for (int s : c.capSig) v->push_back(S.get(R(s)));   // R: the variable in this process's frame
        caps = v;
    }
    if (c.pending.empty()) concPending().push_back(i.hi);
    c.pending.push_back({ctl, ctl->epoch, caps});
    S.rare = true;
}
// The assume set of a checker ticked (17.7.2): new random values for its
// active free variables, tried again while an assumption would fail now.
static void freeSolveNow(ConcAssert& c) {
    if (c.solveVars.empty()) return;
    for (int t = 0; t < 4096; t++) {   // random tries: no search, so a narrow assume set can still miss (allowed)
        for (int v : c.solveVars) S.set(v, S.freeValue(v));
        if (!c.solveOk || truth(eval(c.solveOk, rd)) == 1) return;
    }
}
// A multiclock assertion (16.13): one process per clock marks it ticked;
// the first in a time step runs the assertion in the observed region, once,
// with every clock that ticked in that step.
inline Task concClockProc(int k, int ci) {
    ConcAssert& c = concAsserts()[k];
    const auto* ev = ci == 0 ? &c.ev : &c.more[ci - 1].first;
    const E& iff = ci == 0 ? c.clkIff : c.more[ci - 1].second;
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        if (iff && truth(eval(iff, rd)) != 1) continue;
        if (ci == 0) freeSolveNow(c);
        c.ticked |= 1u << ci;
        if (c.due) continue;
        c.due = true;
        co_await ToObserved{};
        uint32_t t = c.ticked;
        c.ticked = 0;
        c.due = false;
        concTick(c, t);
    }
}
inline Task concProc(int k) {
    ConcAssert& c = concAsserts()[k];
    const auto* ev = &c.ev;
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        if (c.clkIff && truth(eval(c.clkIff, rd)) != 1) continue;   // @(posedge clk iff en)
        freeSolveNow(c);
        co_await ToObserved{};
        concTick(c);
    }
}
// disable iff is checked whenever its value may change, not only at ticks:
// once true, every attempt in flight is disabled (16.12).
inline Task concDisableProc(int k, shared_ptr<vector<pair<int, Edge>>> ev) {
    ConcAssert& c = concAsserts()[k];
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev.get()};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        if (truth(eval(c.disable, rd)) == 1) dropAttempts(c);
    }
}
inline void startConcAsserts() {
    for (size_t k = 0; k < concAsserts().size(); k++) {
        if (concAsserts()[k].more.empty()) S.spawn(concProc((int)k));
        else
            for (size_t ci = 0; ci <= concAsserts()[k].more.size(); ci++) S.spawn(concClockProc((int)k, (int)ci));
        if (const E& d = concAsserts()[k].disable) {
            vector<int> sigs;
            collectRefs(d, sigs);
            auto ev = make_shared<vector<pair<int, Edge>>>();
            for (int s : sigs) ev->push_back({s, ANY});
            if (!ev->empty()) S.spawn(concDisableProc((int)k, ev));
        }
    }
}
// At the end of simulation, attempts still pending fail if they owe a
// strong obligation (16.12.21). Their fail actions run with the final blocks.
inline vector<Task> concEnd() {
    vector<Task> ts;
    for (auto& c : concAsserts()) {
        AssertInfo& a = asserts()[c.id];
        for (auto& at : c.att)
            if (at.killGen == a.killGen && !sva::endValue(at.t)) {
                a.fails += at.count;
                for (uint64_t n = 0; assertHook() && !a.hidden && n < at.count; n++) assertEvent(c.id, AE_FAILURE, at.start);
                if (a.failOn && c.fail)
                    for (uint64_t n = 0; n < at.count; n++) ts.push_back(startProc(c.fail, make_shared<ProcCtl>()));
            }
        c.att.clear();
    }
    return ts;
}

// A sample clock (sva.hpp): marks the slots where it ticks.
inline Task sampleClockProc(int c) {
    const auto* ev = &S.sclocks[c].ev;
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        if (std::find(S.ticked.begin(), S.ticked.end(), c) == S.ticked.end()) S.ticked.push_back(c);
        S.rare = true;
    }
}

// A wait on signals named by relative ids (automatic variables): the ids
// are resolved when the wait starts.
static vector<pair<int, Edge>> resolveEvents(const Instr& i) {
    vector<pair<int, Edge>> ev;
    if (i.dynWait) {
        vector<int> a;
        collectAddrs(i.e, a);
        std::sort(a.begin(), a.end());
        a.erase(std::unique(a.begin(), a.end()), a.end());
        for (int s : a) ev.push_back({s, ANY});
        return ev;
    }
    if (i.events.size() > 1) for (auto& [s, e] : i.events) ev.push_back({R(s), e});
    else ev.push_back({R(i.sig), i.edge});
    return ev;
}
static bool relEvents(const Instr& i) {
    if (i.dynWait) return true;
    if (i.events.size() > 1) { for (auto& ev : i.events) if (isRel(ev.first)) return true; return false; }
    return isRel(i.sig);
}

// A write by a process of a domain whose simstate corrupts on activity or
// on change (IEEE 1801-2024 9.5.4 to 9.5.7) stores a corrupt value instead.
static Val upfWrite(const Prog* e, int sig, int hi, int lo, const Val& v) {
    int m = g_pdMode[(size_t)e->pd];
    bool stateOnly = m == 1 || m == 2;    // CORRUPT_STATE_ON_CHANGE / _ON_ACTIVITY
    bool onChange = m == 1 || m == 3;     // ..._ON_CHANGE
    if (stateOnly && !e->pstate) return v;
    if (onChange) {
        Val cur = S.get(sig);
        if (hi >= 0) cur = v_slice(cur, hi, lo);
        if (cur.w == v.w && cur == v) return v;
    }
    return allx(v.w);
}

inline Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl, FrameP fr) {
    const Prog* p = entry;
    const FrameP fr0 = fr;
    size_t pc = 0;
    vector<RetFrame> rets;
    FrameP lastRet;   // the frame the last call returned from (mode kRelRet)
    vector<pair<int, Edge>> evbuf;
    auto ret = [&]() {
        RetFrame f = std::move(rets.back());
        rets.pop_back();
        p = f.prog;
        pc = f.pc;
        if (f.fr != fr) { lastRet = std::move(fr); fr = std::move(f.fr); }
    };
    const uint64_t gen0 = ctl->gen;
    if (ctl->restart) {   // moved here by a disable (9.6.2)
        rets = std::move(ctl->restart->rets);
        p = ctl->restart->p;
        pc = ctl->restart->pc;
        fr = std::move(ctl->restart->fr);
        ctl->restart.reset();
    } else
        for (auto& [slot, v] : ctl->init) slot->k = v;
    ctl->rets = &rets; ctl->pp = &p; ctl->ppc = &pc; ctl->pfr = &fr;
    // A program thread runs in the reactive region set (24.3.1): it starts
    // there, and goes back there after every wait.
#define TO_REACTIVE if (__builtin_expect(ctl->reactive, 0) && S.region != R_REACTIVE) co_await ToReactive{}
    TO_REACTIVE;
    while (p) {
        g_fr = fr.get();
        g_ret = lastRet.get();
        g_threadRng = ctl->rngp;
        if (ctl->killed) goto done;
        if (ctl->gen != gen0) [[unlikely]] co_return;   // a disable moved it to a new coroutine
        // A process of a powered-off domain is inactive (1801 9.5.3): it
        // sleeps until the domain powers up, then starts over from its top,
        // so combinational logic re-evaluates and edge-triggered logic waits
        // for its next edge (9.6.7).
        if (__builtin_expect(g_upf, 0) && entry->pd >= 0 && g_pdMode[(size_t)entry->pd] == 5) {
            while (g_pdMode[(size_t)entry->pd] == 5) co_await Wait{g_pdSig[(size_t)entry->pd], ANY};
            p = entry;
            pc = 0;
            rets.clear();
            fr = fr0;
            lastRet = nullptr;
            continue;
        }
        if (ctl->suspended) [[unlikely]] {   // 9.7: stop here until resume()
            co_await ProcPark{ctl.get()};
            if (ctl->killed) goto done;
        }
        if (pc >= p->code.size()) {
            if (rets.empty()) goto done;
            ret();
            continue;
        }
        const Instr& i = p->code[pc];
        g_runLoc = {i.file, i.line};
        if (g_cov | g_hook) {
            if (g_cov) covHit(i.hits);
            if (i.stmtCb) g_stmtHook(i);
        }
        switch (i.k) {
            case IK::Assign:
            case IK::Nba: {
                Val v = eval(i.e, rd);
                if (i.tmp) { S.poke(i.sig, v, i.hi, i.lo); pc++; break; }   // a temporary of its own (msim -xprop)
                if (__builtin_expect(g_upf, 0) && entry->pd >= 0 && g_pdMode[(size_t)entry->pd] > 0 && i.sig >= 0 && !isRel(i.sig) && !i.arr)
                    v = upfWrite(entry, i.sig, i.hi, i.lo, v);
                if (i.drv >= 0) {
                    if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);
                    if (i.gate >= 0 && gateRecs()[(size_t)i.gate].annotated)   // SDF DEVICE delays (32.4.1)
                        S.driveGate(i.sig, i.drv, v, i.s0, i.s1, gateRecs()[(size_t)i.gate].d);
                    else if (!i.dly.empty()) S.driveLater(i.sig, i.drv, v, i.s0, i.s1, i.dly);   // assign #d (10.3.3)
                    else S.drive(i.sig, i.drv, v, i.s0, i.s1);
                    pc++; break;
                }
                if (i.dyn >= 0) {   // q[i] = v, aa[key] = v
                    Instr set = i;
                    set.k = IK::Dyn; set.sig = i.dyn; set.dcmd = D_SET;
                    if (i.hi >= 0 || i.bidx) {   // q[i].f = v, q[i][3:0] = v: the element with those bits changed
                        Val old = dynStore()[R(i.dyn)].get(eval(i.args[0], rd));
                        int hi = i.hi, lo = i.lo;
                        if (i.bidx) {
                            Val bv = eval(i.bidx, rd);
                            if (bv.x) { pc++; break; }
                            int64_t b = v_to_i64(bv) - i.lo;
                            int64_t l = i.pw ? (i.pdown ? b - i.pw + 1 : b) : b, h = i.pw ? l + i.pw - 1 : b;
                            int64_t cl = std::max<int64_t>(l, 0), ch = std::min<int64_t>(h, old.w - 1);
                            if (cl > ch) { pc++; break; }
                            v = v_slice(v, (int)(ch - l), (int)(cl - l));
                            hi = (int)ch;
                            lo = (int)cl;
                        }
                        if (hi >= old.w) { pc++; break; }
                        Val nv = v_insert(old, v, hi, lo);
                        nv.sgn = old.sgn;
                        set.e = konst(nv);
                    }
                    dynExec(set);
                    pc++; break;
                }
                int sig = i.sig, hi = i.hi, lo = i.lo;
                // The index of a[i] / v[i] is read when the statement runs,
                // also for a nonblocking write. An invalid one skips it (7.4.6, 11.5.1).
                if (!target(i, sig, hi, lo, v)) { pc++; break; }
                if (i.k == IK::Nba && i.nbaLater) {
                    uint64_t d = i.delay;
                    if (i.de) { Val dv = eval(i.de, rd); d = v_time(dv); }
                    S.nbaAt(S.now + d, sig, v, hi, lo);
                } else if (i.k == IK::Nba) S.nba(sig, v, hi, lo);
                else S.set(sig, hi >= 0 ? v_insert(S.get(sig), v, hi, lo) : v);
                pc++; break;
            }
            case IK::ReadMem:
            case IK::WriteMem: {
                if (i.e) {   // a file name known only at run time: a string variable or expression
                    Instr n = i;
                    n.fmt = v_str(eval(i.e, rd));
                    if (i.k == IK::ReadMem) readMem(n); else writeMem(n);
                } else if (i.k == IK::ReadMem) readMem(i);
                else writeMem(i);
                pc++; break;
            }
            case IK::Force: S.holdOn(i.sig, i.e, i.hi != 0, i.pw ? i.dkey + i.pw - 1 : -1, i.dkey); pc++; break;
            case IK::Release: S.holdOff(i.sig, i.hi != 0, i.pw ? i.dkey + i.pw - 1 : -1, i.dkey); pc++; break;
            case IK::Dyn: dynExec(i); pc++; break;
            case IK::Udp: udpExec(i); pc++; break;
            case IK::Tran: {   // 28.8: 1 on, 0 off, 2 an X or Z control
                int st = 1;
                if (i.e) {
                    Val c = eval(i.e, rd);
                    st = (c.x & 1) ? 2 : (int)((c.v & 1) ^ (i.pdown ? 1 : 0));
                }
                S.tranCtl(i.sig, st);
                pc++; break;
            }
            case IK::Dump: {
                if (i.dcmd >= DC_PORTS) {   // extended VCD: [size,] file name
                    size_t k = 0;
                    int64_t size = 0;
                    std::string file;
                    if (i.dcmd == DC_PLIMIT) { Val sz = eval(i.args[k++], rd); size = sz.x ? 0 : v_to_i64(sz); }
                    if (k < i.args.size()) { Val f = eval(i.args[k], rd); f.str = true; file = v_str(f); }
                    S.dumpPorts(i.dcmd, file, size, i.dsel);
                    pc++; break;
                }
                Val a = i.args.empty() ? Val{} : eval(i.args[0], rd);
                std::string str;
                int64_t num = -1;
                if (!i.args.empty()) {
                    if (i.dcmd == DC_FILE) { a.str = true; str = v_str(a); }
                    else if (!a.x) num = v_to_i64(a);
                }
                S.dumpTask(i.dcmd, str, num, i.dsel);
                pc++; break;
            }
            case IK::Vpi:
                if (!vpiCallHook()) { std::cerr << "msim: VPI call without VPI support\n"; S.finished = true; goto done; }
                vpiCallHook()(i);
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Dpi: {
                if (!dpiHook().call) { std::cerr << "msim: DPI call without DPI support\n"; S.finished = true; goto done; }
                void* fib = nullptr;
                auto js = dpiHook().call(i, fib, ctl);
                while (js) {
                    ctl->dpiFiber = fib;
                    co_await JoinWait{js};
                    if (ctl->killed) goto done;   // kill already ran the C code to its end (35.9)
                    ctl->dpiFiber = nullptr;
                    js = dpiHook().resume(fib);
                }
                if (S.finished) goto done;
                pc++;
                break;
            }
            case IK::New: newObject(i); pc++; break;
            case IK::Randomize: {   // 18.6, 18.12
                int obj = 0;
                if (i.e) {
                    Val h = eval(i.e, rd);
                    if (h.x || !h.v) nullAccess();
                    obj = (int)h.v;
                    if (i.rc && i.rc->objTmp >= 0) S.set(R(i.rc->objTmp), make((uint64_t)obj, 0, 64));
                }
                int r = doRandomize(obj, i.rc.get());
                g_fr = fr.get();
                g_ret = lastRet.get();
                if (i.sig >= 0) S.set(R(i.sig), make((uint64_t)r, 0, 32, 0, true));
                pc++; break;
            }
            case IK::ProcOp: {   // std::process (9.7)
                if (i.hi == PO_SELF) {
                    if (!ctl->obj) {
                        const FrameLayout& L = *classRt()[(size_t)i.lo].lay;
                        int obj = g_allocSigs(L);
                        g_initSigs(L, obj);
                        S.sigs[obj + i.dkey].val = make((uint64_t)procTable().size(), 0, 64);
                        procTable().push_back(ctl);
                        ctl->obj = obj;
                        // its srandom / get_randstate (18.13.3) work on the process's own generator
                        objRng()[obj] = *ctl->rngp;
                        ctl->rngp = &objRng()[obj];
                        g_threadRng = ctl->rngp;
                    }
                    S.set(R(i.sig), make((uint64_t)ctl->obj, 0, 64));
                    pc++; break;
                }
                ProcCtl* c = procOf(i);
                pc++;
                if (i.hi == PO_STATUS) S.set(R(i.sig), make((uint64_t)procStatus(c, ctl.get()), 0, 32, 0, true));
                else if (i.hi == PO_KILL) {
                    if (!c->ended) {
                        for (auto& t : procTable()) if (t.get() == c) { procKill(t); break; }
                    }
                } else if (i.hi == PO_AWAIT) {
                    if (c == ctl.get()) relFail("a process awaiting itself (9.7)");
                    co_await ProcAwait{c};
                } else if (i.hi == PO_SUSPEND) {
                    if (!c->ended) c->suspended = true;
                } else if (i.hi == PO_RESUME) procResume(c);
                break;
            }
            case IK::RandMode: {   // obj.rand_mode(v) / obj.constraint_mode(v) (18.8, 18.9)
                Val h = eval(i.e, rd);
                if (h.x || !h.v) nullAccess();
                Val on = eval(i.de, rd);   // the on / off value
                randModeAll((int)h.v, i.hi != 0, truth(on) == 1);
                pc++; break;
            }
            case IK::JmpIfNot:
                if (int tv = truth(eval(i.e, rd)); tv == 1) pc++;
                else { if (g_cov) covHit(i.taken); pc = tv == 2 && i.xt != Instr::NOX ? i.xt : i.target; }
                break;
            case IK::Jmp:       pc = i.target; break;
            case IK::WaitEdge:
                if (relEvents(i)) {
                    evbuf = resolveEvents(i);
                    if (evbuf.empty()) relFail("wait on nothing");
                    if (evbuf.size() > 1) co_await WaitAny{&evbuf};
                    else co_await Wait{evbuf[0].first, evbuf[0].second};
                } else if (i.events.size() > 1) co_await WaitAny{&i.events};
                else co_await Wait{i.sig, i.edge};
                TO_REACTIVE;
                ctl->epoch++;   // a deferred assertion flush point (16.4.2)
                pc++; break;
            case IK::WaitDelay: {
                uint64_t d = i.delay;
                if (i.de) { Val v = eval(i.de, rd); d = v_time(v); }
                co_await Delay{d};
                TO_REACTIVE;
                pc++; break;
            }
            case IK::Display:
                S.printSys(i.fmt, i.args, !i.nonl);
                if (g_errHook && (i.fmt.rfind("Error: ", 0) == 0 || i.fmt.rfind("Fatal: ", 0) == 0)) g_errHook(i.fmt);
                if (S.finished) goto done;  // task ran; $finish already requested
                pc++;
                break;
            case IK::Strobe:
                {
                    vector<E> a;
                    if (fr) for (auto& x : i.args) a.push_back(bindRel(x));
                    const vector<E>& use = fr ? a : i.args;
                    if (i.fd) S.strobeF((uint32_t)v_to_i64(eval(i.fd, rd)), i.fmt, use);
                    else S.strobe(i.fmt, use);
                }
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Monitor:
                {
                    vector<E> a;
                    if (fr) for (auto& x : i.args) a.push_back(bindRel(x));
                    const vector<E>& use = fr ? a : i.args;
                    if (i.fd) S.armFMon((uint32_t)v_to_i64(eval(i.fd, rd)), i.fmt, use);
                    else S.armMon(i.fmt, use);
                }
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Exit:
                programExit(ctl);
                if (ctl->killed) goto done;
                pc++; break;
            case IK::Finish:
                // Stop this process. The rest of this time slot still runs,
                // including NBA and postponed, so a $strobe/$monitor already
                // queued still prints. The next time does not start. Another
                // process already queued runs until its next system task;
                // that task itself runs (Icarus 13).
                S.finished = true;
                if (i.hi > S.exitCode) S.exitCode = i.hi;   // $fatal
                goto done;
            case IK::Call: {
                const Prog* callee = i.callee;
                if (i.lay) {   // an automatic subroutine: a new frame (13.3.2)
                    int self = fr ? fr->self : 0;
                    const FrameLayout* lay = i.lay;
                    if (i.noSelf) self = 0;
                    else if (i.self) {
                        Val h = eval(i.self, rd);
                        if (h.x || !h.v) nullAccess();
                        self = (int)h.v;
                    }
                    if (i.vslot >= 0 || i.vslot <= -2) {   // virtual (8.20): the body of the object's own class
                        if (!self) nullAccess();
                        const ClassRt& cr = classRt()[(size_t)S.sigs[self].val.v];
                        int vs = i.vslot;
                        if (vs <= -2) {   // an interface class method (8.26): k = -2 - vslot
                            size_t k = (size_t)(-2 - vs);
                            vs = k < cr.imap.size() ? cr.imap[k] : -1;
                            if (vs < 0) {
                                cerr << "error: class " << cr.name << " has no implementation of an interface method\n";
                                exit(1);
                            }
                        }
                        if ((size_t)vs >= cr.vt.size()) {
                            cerr << "internal error: virtual slot " << vs << " past the methods of class " << cr.name << runLocStr() << "\n";
                            exit(1);
                        }
                        const auto& ve = cr.vt[(size_t)vs];
                        if (!ve.first) {
                            cerr << "error: call of a pure virtual method at time " << S.now << runLocStr() << "\n";
                            exit(1);
                        }
                        callee = ve.first;
                        lay = ve.second;
                    }
                    static const bool callTrace = getenv("MSIM_CALLTRACE") != nullptr;
                    if (callTrace) cerr << "call " << (lay ? lay->name : "?") << " self=" << self << runLocStr() << "\n";
                    FrameP nf = newFrame(lay, nullptr, self);
                    for (auto& [off, e] : i.init) {   // arguments, read in the caller's frame
                        Val v = eval(e, rd);
                        S.set(nf->base + off, v);
                    }
                    for (auto [off, src] : i.dinit) {   // container arguments (7.7)
                        dynAssign(dynStore()[nf->base + off], dynStore()[R(src)]);
                    }
                    rets.push_back({p, pc + 1, fr});
                    fr = std::move(nf);
                } else rets.push_back({p, pc + 1, fr});
                p = callee;
                pc = 0;
                break;
            }
            case IK::Fork: {
                auto js = make_shared<JoinState>();
                js->join = i.join;
                js->left = (int)i.branches.size();
                std::erase_if(ctl->kids, [](auto& k) { return k->ended; });
                // The fork block's automatic variables get one frame, set
                // in the parent before any branch starts (9.3.2).
                FrameP ff = fr;
                if (i.lay) {
                    ff = newFrame(i.lay, fr, fr ? fr->self : 0);
                    for (auto& [off, e] : i.init) { Val v = eval(e, rd); S.set(ff->base + off, v); }
                }
                for (size_t bn = 0; bn < i.branches.size(); bn++) {
                    const Prog* b = i.branches[bn];
                    auto c = make_shared<ProcCtl>();
                    c->parent = ctl;
                    c->join = js;
                    c->program = ctl->program;
                    c->reactive = ctl->reactive;
                    {   // 18.14.2: a forked process's generator is seeded from its parent's
                        uint64_t a = urandomNext(*ctl->rngp), b = urandomNext(*ctl->rngp);
                        c->rng = seedState(a << 32 | b);
                    }
                    c->forkDepth = rets.size(); c->forkProg = p; c->forkPc = pc;
                    ctl->kids.push_back(c);
                    FrameP bf = bn < i.blays.size() && i.blays[bn] ? newFrame(i.blays[bn], ff, ff ? ff->self : 0) : ff;
                    Task t = startProc(b, c, std::move(bf));
                    S.all.push_back(t.h);
                    S.active.push_back(t.h);
                }
                pc++;
                if (i.join != 2) { co_await JoinWait{js}; TO_REACTIVE; }
                if (S.finished) goto done;
                break;
            }
            case IK::Disable: {   // 9.6.2
                auto& v = rootProcs()[i.sig];
                auto list = v;
                for (auto& w : list)
                    if (auto c = w.lock()) procKill(c);
                std::erase_if(v, [](auto& w) { auto c = w.lock(); return !c || c->ended; });
                if (anySpans() && !ctl->killed && disableSpans(i.sig, ctl)) break;   // this process was inside it
                pc++;
                break;   // the loop top stops this process if it was one of them
            }
            case IK::WaitFork:
                pc++;
                co_await WaitFork{ctl};
                TO_REACTIVE;
                break;
            case IK::DisableFork:
                for (auto& k : ctl->kids) procKill(k);
                ctl->kids.clear();
                pc++;
                break;
            case IK::Assert:
                if (i.join == 4) {   // expect (16.17): wait for the verdict
                    auto w = startExpect(i, ctl);
                    S.rare = true;
                    co_await ExpectAwait{w.get()};
                    ctl->epoch++;
                    pc = w->res == 1 ? pc + 1 : w->res == 0 ? (size_t)i.target : (size_t)i.lo;
                    break;
                }
                pc = runAssert(i, ctl, pc);
                break;
            case IK::AssertCtl: assertCtl(i); pc++; break;
            case IK::CycleWait: {   // ##n (14.11)
                Val nv = eval(i.e, rd);
                uint64_t n = nv.x ? 0 : nv.v;
                if (n == 0) {   // ##0: only if the clocking block event has not happened in this step
                    Val t = S.get(i.sig + 1);
                    if (t.x || t.v != S.now + 1) co_await Wait{i.sig, ANY};
                } else
                    for (uint64_t k = 0; k < n; k++) co_await Wait{i.sig, ANY};
                TO_REACTIVE;
                ctl->epoch++;
                pc++; break;
            }
            case IK::CbDrive: {   // cb.v <= ##n e (14.16)
                Val v = eval(i.e, rd);
                int sig = i.sig, hi = i.hi, lo = i.lo;
                if (target(i, sig, hi, lo, v)) cbDrive(i, v, hi, lo);
                pc++; break;
            }
            case IK::Ret:
                if (rets.empty()) goto done;
                ret();
                break;
        }
    }
done:
    procEnd(*ctl);
}
#undef TO_REACTIVE

// The value of a one-assign combinational process, and its write
// (Sim::Comb compute / commit). compute only reads signals.
static Val combValue(const void* ctx) {
    if (g_cov) covHit(static_cast<const Prog*>(ctx)->code[0].hits);
    return eval(static_cast<const Prog*>(ctx)->code[0].e, rd);
}
static void combCommit(const void* ctx, Val v) {
    const Instr& i = static_cast<const Prog*>(ctx)->code[0];
    if (i.drv >= 0) {
        if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);  // as in exec()
        S.drive(i.sig, i.drv, v, i.s0, i.s1);
    } else
        S.set(i.sig, i.hi >= 0 ? v_insert(S.get(i.sig), v, i.hi, i.lo) : v);
}

// A clocked process (prog.hpp: seqShape) run as a function: the body after
// its wait, with each NBA going to `out` instead of the queue.
static void execSeq(const void* ctx, vector<Sim::NbaItem>& out) {
    const Prog* p = static_cast<const Prog*>(ctx);
    size_t pc = 1, end = p->code.size() - 1;
    if (p->xpFast && S.xAt != S.now) pc = p->xpFast;   // msim -xprop: no edge can be ambiguous
    if (g_cov) covHit(p->code[0].hits);   // the clock wait this function stands for
    while (pc < end) {
        const Instr& i = p->code[pc];
        if (g_cov | g_hook) {
            if (g_cov) covHit(i.hits);
            if (i.stmtCb) g_stmtHook(i);
        }
        switch (i.k) {
            case IK::Nba: {
                Val v = eval(i.e, rd);
                int sig = i.sig, hi = i.hi, lo = i.lo;
                if (target(i, sig, hi, lo, v)) out.push_back({sig, v, hi, lo});
                pc++;
                break;
            }
            case IK::JmpIfNot:
                if (int tv = truth(eval(i.e, rd)); tv == 1) pc++;
                else { if (g_cov) covHit(i.taken); pc = tv == 2 && i.xt != Instr::NOX ? i.xt : i.target; }
                break;
            case IK::Jmp: pc = i.target; break;
            case IK::Assign: S.poke(i.sig, eval(i.e, rd), i.hi, i.lo); pc++; break;   // a temporary of its own
            default: pc++; break;  // seqShape admits nothing else
        }
    }
}

// A combinational process (prog.hpp: combShape) run as a function: its body
// (assignments and forward jumps) up to the wait, exactly as exec() would.
static void execComb(const void* ctx) {
    const Prog* p = static_cast<const Prog*>(ctx);
    if (p->combCtl) static_cast<ProcCtl*>(p->combCtl.get())->epoch++;   // a flush point for its deferred reports
    for (size_t pc = 0; pc < p->code.size();) {
        const Instr& i = p->code[pc];
        if (i.k == IK::Assert) {
            if (g_cov) covHit(i.hits);
            runAssert(i, std::static_pointer_cast<ProcCtl>(p->combCtl), pc);
            pc++;
            continue;
        }
        if (i.k == IK::JmpIfNot) {
            if (g_cov) covHit(i.hits);
            if (int tv = truth(eval(i.e, rd)); tv == 1) pc++;
            else { if (g_cov) covHit(i.taken); pc = tv == 2 && i.xt != Instr::NOX ? i.xt : i.target; }
            continue;
        }
        if (i.k == IK::Jmp) { if (g_cov) covHit(i.hits); pc = i.target; continue; }
        if (i.k != IK::Assign) return;
        pc++;
        if (g_cov) covHit(i.hits);
        Val v = eval(i.e, rd);
        if (i.tmp) { S.poke(i.sig, v, i.hi, i.lo); continue; }   // a temporary of its own
        if (i.bidx) {   // v[i] = ...: the bits are chosen now; an invalid index writes nothing
            int sg = i.sig, hi = i.hi, lo = i.lo;
            if (target(i, sg, hi, lo, v)) S.set(sg, hi >= 0 ? v_insert(S.get(sg), v, hi, lo) : v);
            continue;
        }
        if (i.drv >= 0) {
            // A driver of part of a net drives Z on the other bits, as in exec().
            if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);
            S.drive(i.sig, i.drv, v, i.s0, i.s1);
        } else
            S.set(i.sig, i.hi >= 0 ? v_insert(S.get(i.sig), v, i.hi, i.lo) : v);
    }
}

// ---- clocking blocks (IEEE 1800-2023 clause 14) ----
// A synchronous drive maturing: bits [hi:lo] of output `out` (hi < 0: all)
// take v, in the re-NBA region, `skew` ticks after the clocking event (14.16).
inline Task cbLater(int s, Val v, int hi, int lo, uint64_t d) {
    co_await Delay{d};
    S.reNbaq.push_back({s, v, hi, lo});
    S.rare = true;
}
static void cbSchedule(ClockingRT& c, int out, const Val& v, int hi, int lo) {
    const ClockingRT::Out& o = c.outs[out];
    if (hi < 0) { hi = o.w - 1; lo = 0; }
    int pl = 0;   // the clockvar bit where this piece starts
    for (const auto& p : o.pieces) {
        int a = std::max(lo, pl), b = std::min(hi, pl + p.w - 1);
        if (a <= b) {
            Val part = v_slice(v, b - lo, a - lo);
            int dhi = p.lo + (b - pl), dlo = p.lo + (a - pl);
            if (dlo == 0 && dhi == S.sigs[p.sig].w - 1) dhi = -1;
            if (o.skew == 0) { S.reNbaq.push_back({p.sig, part, dhi, dlo}); S.rare = true; }
            else S.spawn(cbLater(p.sig, part, dhi, dlo, o.skew));
        }
        pl += p.w;
    }
}
// Did the clocking event happen in this time step? Each event signal is
// compared with its value from before the step.
static bool cbEdgeNow(const ClockingRT& c) {
    auto bit = [](const Val& v) { return (v.x & 1) ? 2 : (int)(v.v & 1); };
    bool hit = false;
    for (size_t k = 0; k < c.ev.size() && !hit; k++) {
        Val cur = S.get(c.ev[k].first), pre = S.get(c.evShadow[k]);
        int a = bit(pre), b = bit(cur);
        switch (c.ev[k].second) {
            case POS: hit = (a == 0 && b != 0) || (a == 2 && b == 1); break;
            case NEG: hit = (a == 1 && b != 1) || (a == 2 && b == 0); break;
            default: hit = !(cur == pre) || cur.x != pre.x; break;
        }
    }
    return hit && (!c.iff || truth(eval(c.iff, rd)) == 1);
}
// cb.v[hi:lo] <= ##n e: at the clocking event n cycles on. A drive at a time
// with no clocking event (yet) in it counts the next event as cycle 1 (14.16).
static void cbDrive(const Instr& i, Val v, int hi, int lo) {
    ClockingRT& c = clockings()[i.dkey];
    uint64_t n = 0;
    if (i.de) { Val d = eval(i.de, rd); n = d.x ? 0 : d.v; }
    uint64_t tick;
    if (c.lastTick == S.now) tick = c.ticks + n;   // after the clocking event
    else if (cbEdgeNow(c)) tick = c.ticks + 1 + n;   // its event is in this step, not processed yet
    else tick = c.ticks + std::max<uint64_t>(n, 1);
    if (tick == c.ticks) cbSchedule(c, i.dres, v, hi, lo);
    else c.drives.push_back({tick, i.dres, v, hi, lo});
}
// An input with skew #N: its value at the end of time t (14.13).
static Val cbHistAt(ClockingRT::In& in, uint64_t t) {
    auto& h = in.hist;
    size_t k = 0;
    while (k + 1 < h.size() && h[k + 1].first <= t) k++;
    h.erase(h.begin(), h.begin() + (ptrdiff_t)k);   // later ticks look at later times
    return h.front().second;
}
inline Task cbHist(int k, int j, shared_ptr<vector<pair<int, Edge>>> ev) {
    for (bool first = true;; first = false) {
        if (!first) {
            if (ev->size() > 1) co_await WaitAny{ev.get()};
            else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        }
        auto& in = clockings()[k].ins[j];
        Val v = eval(in.src, rd);
        if (!in.hist.empty() && in.hist.back().first == S.now) in.hist.back().second = v;
        else in.hist.push_back({S.now, v});
    }
}
// A net output is driven from its hidden variable (14.16).
inline Task cbNetDriver(int var, int net, int drv, int lo) {
    for (;;) {
        co_await Wait{var, ANY};
        Val v = S.get(var);
        S.drive(net, drv, v_insert(allz(S.sigs[net].w), v, lo + v.w - 1, lo));
    }
}
inline Task cbProc(int k) {
    ClockingRT& c = clockings()[k];
    vector<Val> early(c.ins.size());
    for (;;) {
        if (c.ev.size() > 1) co_await WaitAny{&c.ev};
        else co_await Wait{c.ev[0].first, c.ev[0].second};
        if (c.iff && truth(eval(c.iff, rd)) != 1) continue;
        c.ticks++;
        c.lastTick = S.now;
        // Inputs: #1step reads the values from before this time step, #N
        // the values N ticks back. They are updated now (14.13).
        for (size_t j = 0; j < c.ins.size(); j++) {
            auto& in = c.ins[j];
            if (in.kind == 0) S.set(in.cv, eval(in.src, rd));
            else if (in.kind == 2) S.set(in.cv, S.now >= in.skew ? cbHistAt(in, S.now - in.skew) : in.hist.front().second);
        }
        // Drives due at this clocking event.
        if (!c.drives.empty()) {
            vector<ClockingRT::Drive> due;
            std::erase_if(c.drives, [&](const ClockingRT::Drive& d) {
                if (d.tick != c.ticks) return false;
                due.push_back(d);
                return true;
            });
            for (auto& d : due) cbSchedule(c, d.out, d.v, d.hi, d.lo);
        }
        // #0 inputs are sampled in the observed region, and then the
        // clocking block event is triggered (14.10, 14.13).
        co_await ToObserved{};
        for (auto& in : c.ins)
            if (in.kind == 1) S.set(in.cv, eval(in.src, rd));
        Val n = S.get(c.evSig);
        S.set(c.evSig, mk((n.x ? 0 : n.v) + 1, 32));
        S.set(c.evSig + 1, mk(S.now + 1, 64));
    }
}
inline void startClockings() {
    for (size_t k = 0; k < clockings().size(); k++) {
        auto& c = clockings()[k];
        S.set(c.evSig, mk(0, 32));
        S.set(c.evSig + 1, mk(0, 64));
        for (size_t j = 0; j < c.ins.size(); j++) {
            auto& in = c.ins[j];
            if (in.kind != 2) continue;
            in.hist.push_back({0, eval(in.src, rd)});   // cbHist updates it once the initializers have run
            if (in.refs.empty()) continue;
            auto ev = make_shared<vector<pair<int, Edge>>>();
            for (int s : in.refs) ev->push_back({s, ANY});
            S.spawn(cbHist((int)k, (int)j, ev));
        }
        for (auto& o : c.outs)
            for (auto& p : o.pieces)
                if (p.net >= 0) {
                    S.set(p.sig, allz(p.w));
                    S.spawn(cbNetDriver(p.sig, p.net, S.newDriver(p.net), p.netLo));
                }
        S.spawn(cbProc((int)k));
    }
}
#include "cover_rt.inc"
#include "tchk.inc"
#include "sdf_annotate.inc"
#include "annexd.inc"
