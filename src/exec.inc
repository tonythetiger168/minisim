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
        sig = R(i.arr->base + (int)off);
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
    for (int64_t ad = start;; ad += step) {
        int64_t off = a.offset({ad});
        if (off >= 0) {
            Val v = S.get(R(a.base + (int)off));
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

static void readMem(const Instr& i) {
    const ArrShape& a = *i.arr;
    auto [l, r] = a.dims[0];
    int64_t lowA = std::min(l, r), highA = std::max(l, r);
    int64_t start = lowA, finish = highA;
    bool haveFinish = false;
    if (i.args.size() > 0) { Val v = eval(i.args[0], rd); if (!v.x) start = v_to_i64(v); }
    if (i.args.size() > 1) { Val v = eval(i.args[1], rd); if (!v.x) { finish = v_to_i64(v); haveFinish = true; } }
    int64_t step = (haveFinish && finish < start) ? -1 : 1;
    if (!haveFinish) finish = highA;
    std::ifstream in(i.fmt);
    if (!in) { std::cerr << "$readmem: cannot open " << i.fmt << "\n"; return; }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int64_t addr = start;
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
            int64_t off = a.offset({addr});
            if (off < 0) break;
            if (a.twoState) {
                acc.v &= ~acc.x; acc.x = acc.z = 0;
                for (auto& h : acc.hi) { h.v &= ~h.x; h.x = h.z = 0; }
            }
            S.set(R(a.base + (int)off), acc);
            addr += step;
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
        int64_t off = a.offset({addr});
        if (off < 0) break;
        if (a.twoState) { v &= ~x; x = z = 0; }
        S.set(R(a.base + (int)off), make(v, x, a.elemW, z));
        addr += step;
        words++;
    }
    // A file with no @address whose word count differs from the range warns.
    int64_t range = (step > 0 ? finish - start : start - finish) + 1;
    if (!sawAddr && words != range)
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
    auto& d = dynStore()[sig];
    auto arg = [&](size_t n) { return eval(i.args[n], rd); };
    auto idx = [&](size_t n) { Val v = arg(n); return v.x ? (int64_t)-1 : v_to_i64(v); };
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
            if (dsrc >= 0) {
                auto src = dynStore()[dsrc].vec;
                for (size_t k = 0; k < nv.size() && k < src.size(); k++) nv[k] = src[k];
            }
            d.vec = std::move(nv);
            break;
        }
        case D_DELETE: d.vec.clear(); d.map.clear(); break;
        case D_DELETE_AT: {
            if (d.kind == 2) { d.map.erase(d.key(arg(0))); break; }
            int64_t k = idx(0);
            if (k < 0 || k >= (int64_t)d.vec.size()) { dynWarn("delete(i) outside the queue"); return; }
            d.vec.erase(d.vec.begin() + k);
            break;
        }
        case D_PUSH_BACK: d.vec.push_back(eval(i.e, rd)); if (full()) d.vec.pop_back(); break;
        case D_PUSH_FRONT: d.vec.insert(d.vec.begin(), eval(i.e, rd)); if (full()) d.vec.pop_back(); break;
        case D_INSERT: {
            int64_t k = idx(0);
            if (k < 0 || k > (int64_t)d.vec.size()) { dynWarn("insert(i) outside the queue"); return; }
            d.vec.insert(d.vec.begin() + k, eval(i.e, rd));
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
            auto& s = dynStore()[dsrc];
            if (d.kind == 2) d.map = s.map;
            else { d.vec = s.kind == 2 ? std::vector<Val>{} : s.vec; if (d.bound >= 0 && (int64_t)d.vec.size() > d.bound + 1) d.vec.resize((size_t)d.bound + 1); }
            break;
        }
        case D_APPEND: {   // {q, r}: every element of r
            auto s = dynStore()[dsrc].vec;
            d.vec.insert(d.vec.end(), s.begin(), s.end());
            break;
        }
        case D_APPEND_SLICE: {   // q[a:b] (7.10.1): out-of-range parts are left out
            auto s = dynStore()[dsrc].vec;
            int64_t a = std::max<int64_t>(idx(0), 0), b = std::min<int64_t>(idx(1), (int64_t)s.size() - 1);
            for (int64_t k = a; k <= b; k++) d.vec.push_back(s[(size_t)k]);
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

// One running process: its children (for wait fork / disable fork) and the
// fork it belongs to. A disabled process ends at once for everyone waiting
// on it; its coroutine, if it is resumed later, just returns.
struct ProcCtl {
    Handle h{};
    bool killed = false, ended = false, waitingFork = false;
    weak_ptr<ProcCtl> parent;
    vector<shared_ptr<ProcCtl>> kids;
    shared_ptr<JoinState> join;
    // Deferred assertions (16.4.2): a resume after an event control is a
    // flush point; it bumps `epoch`, and reports queued before it are dropped.
    uint64_t epoch = 0;
    // Written before the process starts: a deferred assertion action's
    // captured arguments, written into their slots (16.4.1).
    vector<pair<Expr*, Val>> init;
    bool kidsDone() const {
        for (auto& k : kids) if (!k->ended) return false;
        return true;
    }
};
inline unordered_map<int, vector<weak_ptr<ProcCtl>>>& rootProcs() {
    static unordered_map<int, vector<weak_ptr<ProcCtl>>> m;
    return m;
}
inline void procEnd(ProcCtl& c) {
    if (c.ended) return;
    c.ended = true;
    if (c.join) c.join->branchEnded();
    if (auto p = c.parent.lock())
        if (p->waitingFork && p->kidsDone()) { p->waitingFork = false; S.active.push_back(p->h); }
}
inline void procKill(const shared_ptr<ProcCtl>& c) {
    if (c->ended) return;
    c->killed = true;
    for (auto& k : c->kids) procKill(k);
    S.dead.insert(c->h.address());
    procEnd(*c);
}
struct WaitFork {
    shared_ptr<ProcCtl> c;
    bool await_ready() const { return c->kidsDone(); }
    void await_suspend(Handle h) const { c->h = h; c->waitingFork = true; }
    void await_resume() const {}
};

Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl, FrameP fr);
inline Task startProc(const Prog* p, shared_ptr<ProcCtl> c, FrameP fr = nullptr) {
    Task t = exec(p, c, std::move(fr));
    c->h = t.h;
    for (int r : p->roots) rootProcs()[r].push_back(c);
    return t;
}
// A top-level process (initial, always, final).
inline Task process(const Prog* p) { return startProc(p, make_shared<ProcCtl>(), nullptr); }
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
void Sim::runObserved() {
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
            if (!pc->killed && pc->epoch == p.epoch) c.matured.push_back(p.caps);
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
void Sim::matureFinal() {
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
        S.all.push_back(t.h);
        S.reactive.push_back(t.h);
    }
    S.rare = true;
}
// One clock tick of a concurrent assertion, in the observed region: start an
// attempt, then step every attempt on the sampled values of the atoms.
static void concTick(ConcAssert& c) {
    AssertInfo& a = asserts()[c.id];
    if (!c.att.empty() && c.att.front().killGen != a.killGen)   // $assertkill (20.11)
        std::erase_if(c.att, [&](const ConcAssert::Attempt& t) { return t.killGen != a.killGen; });
    if (c.disable && truth(eval(c.disable, rd)) == 1) {   // disabled: no pass, no fail (16.12)
        c.att.clear();
        c.pending.clear();
        c.matured.clear();
        if (a.on) a.evals++;
        return;
    }
    if (c.procedural) {   // instances queued by the procedure in this step, or matured earlier
        for (auto& caps : c.matured) { c.att.push_back({c.root, false, 1, a.killGen, caps}); a.evals++; }
        c.matured.clear();
        for (auto& p : c.pending) {
            auto* pc = static_cast<ProcCtl*>(p.ctl.get());
            if (!pc->killed && pc->epoch == p.epoch) { c.att.push_back({c.root, false, 1, a.killGen, p.caps}); a.evals++; }
        }
        c.pending.clear();
    } else if (a.on) {
        c.att.push_back({c.root, false, 1, a.killGen, nullptr});
        a.evals++;
    }
    if (c.att.empty()) return;
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
    vector<ConcAssert::Attempt> next;
    for (auto& at : c.att) {
        values(at.caps);
        dv.memo.clear();
        dv.nonvac = at.nonvac;
        dv.matches = 0;
        const sva::Node* t = dv.d(at.t);
        if (c.coverSeq) {   // every match counts (16.14.3)
            if (dv.matches) {
                a.passes += at.count * dv.matches;
                if (a.passOn) runAction(c.pass, at.count * dv.matches, &c.capSlot, at.caps.get());
            }
            if (t->k == sva::DONE) continue;
        } else if (t->k == sva::PTRUE) {
            a.passes += at.count;
            if (dv.nonvac ? a.passOn : a.vacOn) runAction(c.pass, at.count, &c.capSlot, at.caps.get());
            continue;
        } else if (t->k == sva::PFALSE) {
            a.fails += at.count;
            if (a.failOn) runAction(c.fail, at.count, &c.capSlot, at.caps.get());
            continue;
        }
        bool merged = false;
        for (auto& n : next)
            if (n.t == t && n.nonvac == dv.nonvac && (n.caps == at.caps || (n.caps && at.caps && *n.caps == *at.caps))) {
                n.count += at.count;
                merged = true;
                break;
            }
        if (!merged) next.push_back({t, dv.nonvac, at.count, at.killGen, at.caps});
    }
    c.att.swap(next);
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
inline Task concProc(int k) {
    ConcAssert& c = concAsserts()[k];
    const auto* ev = &c.ev;
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        if (c.clkIff && truth(eval(c.clkIff, rd)) != 1) continue;   // @(posedge clk iff en)
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
        if (truth(eval(c.disable, rd)) == 1) c.att.clear();
    }
}
inline void startConcAsserts() {
    for (size_t k = 0; k < concAsserts().size(); k++) {
        S.spawn(concProc((int)k));
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

Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl, FrameP fr) {
    const Prog* p = entry;
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
    for (auto& [slot, v] : ctl->init) slot->k = v;
    while (p) {
        g_fr = fr.get();
        g_ret = lastRet.get();
        if (ctl->killed) goto done;
        if (pc >= p->code.size()) {
            if (rets.empty()) goto done;
            ret();
            continue;
        }
        const Instr& i = p->code[pc];
        if (g_cov) covHit(i.hits);
        switch (i.k) {
            case IK::Assign:
            case IK::Nba: {
                Val v = eval(i.e, rd);
                if (i.drv >= 0) {
                    if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);
                    if (!i.dly.empty()) S.driveLater(i.sig, i.drv, v, i.s0, i.s1, i.dly);   // assign #d (10.3.3)
                    else S.drive(i.sig, i.drv, v, i.s0, i.s1);
                    pc++; break;
                }
                if (i.dyn >= 0) {   // q[i] = v, aa[key] = v
                    Instr set = i;
                    set.k = IK::Dyn; set.sig = i.dyn; set.dcmd = D_SET;
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
            case IK::JmpIfNot:
                if (truth(eval(i.e, rd)) == 1) pc++;
                else { if (g_cov) covHit(i.taken); pc = i.target; }
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
                ctl->epoch++;   // a deferred assertion flush point (16.4.2)
                pc++; break;
            case IK::WaitDelay: {
                uint64_t d = i.delay;
                if (i.de) { Val v = eval(i.de, rd); d = v_time(v); }
                co_await Delay{d};
                pc++; break;
            }
            case IK::Display:
                S.printSys(i.fmt, i.args, !i.nonl);
                if (S.finished) goto done;  // task ran; $finish already requested
                pc++;
                break;
            case IK::Strobe:
                if (fr) { vector<E> a; for (auto& x : i.args) a.push_back(bindRel(x)); S.strobe(i.fmt, a); }
                else S.strobe(i.fmt, i.args);
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Monitor:
                if (fr) { vector<E> a; for (auto& x : i.args) a.push_back(bindRel(x)); S.armMon(i.fmt, a); }
                else S.armMon(i.fmt, i.args);
                if (S.finished) goto done;
                pc++;
                break;
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
                    FrameP nf = newFrame(i.lay, nullptr, fr ? fr->self : 0);
                    for (auto& [off, e] : i.init) {   // arguments, read in the caller's frame
                        Val v = eval(e, rd);
                        S.set(nf->base + off, v);
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
                    ctl->kids.push_back(c);
                    FrameP bf = bn < i.blays.size() && i.blays[bn] ? newFrame(i.blays[bn], ff, ff ? ff->self : 0) : ff;
                    Task t = startProc(b, c, std::move(bf));
                    S.all.push_back(t.h);
                    S.active.push_back(t.h);
                }
                pc++;
                if (i.join != 2) co_await JoinWait{js};
                if (S.finished) goto done;
                break;
            }
            case IK::Disable: {   // 9.6.2
                auto& v = rootProcs()[i.sig];
                auto list = v;
                for (auto& w : list)
                    if (auto c = w.lock()) procKill(c);
                std::erase_if(v, [](auto& w) { auto c = w.lock(); return !c || c->ended; });
                pc++;
                break;   // the loop top stops this process if it was one of them
            }
            case IK::WaitFork:
                pc++;
                co_await WaitFork{ctl};
                break;
            case IK::DisableFork:
                for (auto& k : ctl->kids) procKill(k);
                ctl->kids.clear();
                pc++;
                break;
            case IK::Assert: pc = runAssert(i, ctl, pc); break;
            case IK::AssertCtl: assertCtl(i); pc++; break;
            case IK::CycleWait: {   // ##n (14.11)
                Val nv = eval(i.e, rd);
                uint64_t n = nv.x ? 0 : nv.v;
                if (n == 0) {   // ##0: only if the clocking block event has not happened in this step
                    Val t = S.get(i.sig + 1);
                    if (t.x || t.v != S.now + 1) co_await Wait{i.sig, ANY};
                } else
                    for (uint64_t k = 0; k < n; k++) co_await Wait{i.sig, ANY};
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
    if (g_cov) covHit(p->code[0].hits);   // the clock wait this function stands for
    while (pc < end) {
        const Instr& i = p->code[pc];
        if (g_cov) covHit(i.hits);
        switch (i.k) {
            case IK::Nba: {
                Val v = eval(i.e, rd);
                int sig = i.sig, hi = i.hi, lo = i.lo;
                if (target(i, sig, hi, lo, v)) out.push_back({sig, v, hi, lo});
                pc++;
                break;
            }
            case IK::JmpIfNot:
                if (truth(eval(i.e, rd)) == 1) pc++;
                else { if (g_cov) covHit(i.taken); pc = i.target; }
                break;
            case IK::Jmp: pc = i.target; break;
            default: pc++; break;  // seqShape admits nothing else
        }
    }
}

// A combinational process (prog.hpp: combShape) run as a function: its
// leading Assign instructions, exactly as exec() would run them.
static void execComb(const void* ctx) {
    const Prog* p = static_cast<const Prog*>(ctx);
    for (const Instr& i : p->code) {
        if (i.k != IK::Assign) return;
        if (g_cov) covHit(i.hits);
        Val v = eval(i.e, rd);
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
