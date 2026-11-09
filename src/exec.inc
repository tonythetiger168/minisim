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
static size_t runAssert(const Instr& i, const shared_ptr<ProcCtl>& ctl, size_t pc) {
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
