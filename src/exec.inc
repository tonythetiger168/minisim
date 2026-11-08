// exec.inc - #include in minisim after the Delay/Wait awaiters (needs Sim S, Task).
// Runs one Prog as a coroutine. `entry` must outlive the simulation.
// `rets` is a coroutine local. A #delay inside a callee hits co_await here,
// and the return address is still on `rets` when the process resumes.
static Val rd(int s) { return S.get(s); }

struct RetFrame { const Prog* prog; size_t pc; };

// Resolve a run-time array index and bit index. false: no write happens.
// v is the value to write; a part-select that is partly out of range writes
// only the bits inside the vector (11.5.1), so v is shifted to match.
static bool target(const Instr& i, int& sig, int& hi, int& lo, Val& val) {
    if (i.arr) {
        std::vector<int64_t> idx;
        for (auto& a : i.aidx) {
            Val v = eval(a, rd);
            if (v.x) return false;
            idx.push_back(v_to_i64(v));
        }
        int64_t off = i.arr->offset(idx);
        if (off < 0) return false;
        sig = i.arr->base + (int)off;
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
        S.set(a.base + (int)off, make(v, x, a.elemW, z));
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

Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl);
inline Task startProc(const Prog* p, shared_ptr<ProcCtl> c) {
    Task t = exec(p, c);
    c->h = t.h;
    for (int r : p->roots) rootProcs()[r].push_back(c);
    return t;
}
// A top-level process (initial, always, final).
inline Task process(const Prog* p) { return startProc(p, make_shared<ProcCtl>()); }
inline Task exec(const Prog* p) { return process(p); }

Task exec(const Prog* entry, shared_ptr<ProcCtl> ctl) {
    const Prog* p = entry;
    size_t pc = 0;
    vector<RetFrame> rets;
    while (p) {
        if (ctl->killed) goto done;
        if (pc >= p->code.size()) {
            if (rets.empty()) goto done;
            RetFrame f = rets.back();
            rets.pop_back();
            p = f.prog;
            pc = f.pc;
            continue;
        }
        const Instr& i = p->code[pc];
        switch (i.k) {
            case IK::Assign:
            case IK::Nba: {
                Val v = eval(i.e, rd);
                if (i.drv >= 0) {
                    if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);
                    S.drive(i.sig, i.drv, v, i.s0, i.s1);
                    pc++; break;
                }
                int sig = i.sig, hi = i.hi, lo = i.lo;
                // The index of a[i] / v[i] is read when the statement runs,
                // also for a nonblocking write. An invalid one skips it (7.4.6, 11.5.1).
                if (!target(i, sig, hi, lo, v)) { pc++; break; }
                if (i.k == IK::Nba && i.nbaLater) {
                    uint64_t d = i.delay;
                    if (i.de) { Val dv = eval(i.de, rd); d = dv.x ? 0 : dv.v; }
                    S.nbaAt(S.now + d, sig, v, hi, lo);
                } else if (i.k == IK::Nba) S.nba(sig, v, hi, lo);
                else S.set(sig, hi >= 0 ? v_insert(S.get(sig), v, hi, lo) : v);
                pc++; break;
            }
            case IK::ReadMem: readMem(i); pc++; break;
            case IK::JmpIfNot:  pc = (truth(eval(i.e, rd)) == 1) ? pc + 1 : i.target; break;
            case IK::Jmp:       pc = i.target; break;
            case IK::WaitEdge:
                if (i.events.size() > 1) co_await WaitAny{&i.events};
                else co_await Wait{i.sig, i.edge};
                pc++; break;
            case IK::WaitDelay: {
                uint64_t d = i.delay;
                if (i.de) { Val v = eval(i.de, rd); d = v.x ? 0 : v.v; }
                co_await Delay{d};
                pc++; break;
            }
            case IK::Display:
                S.printSys(i.fmt, i.args, !i.nonl);
                if (S.finished) goto done;  // task ran; $finish already requested
                pc++;
                break;
            case IK::Strobe:
                S.strobe(i.fmt, i.args);
                if (S.finished) goto done;
                pc++;
                break;
            case IK::Monitor:
                S.armMon(i.fmt, i.args);
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
                goto done;
            case IK::Call: {
                const Prog* callee = i.callee;
                rets.push_back({p, pc + 1});
                p = callee;
                pc = 0;
                break;
            }
            case IK::Fork: {
                auto js = make_shared<JoinState>();
                js->join = i.join;
                js->left = (int)i.branches.size();
                std::erase_if(ctl->kids, [](auto& k) { return k->ended; });
                for (const Prog* b : i.branches) {
                    auto c = make_shared<ProcCtl>();
                    c->parent = ctl;
                    c->join = js;
                    ctl->kids.push_back(c);
                    Task t = startProc(b, c);
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
            case IK::Ret:
                if (rets.empty()) goto done;
                {
                    RetFrame f = rets.back();
                    rets.pop_back();
                    p = f.prog;
                    pc = f.pc;
                }
                break;
        }
    }
done:
    procEnd(*ctl);
}

// A combinational process (prog.hpp: combShape) run as a function: its
// leading Assign instructions, exactly as exec() would run them.
static void execComb(const void* ctx) {
    const Prog* p = static_cast<const Prog*>(ctx);
    for (const Instr& i : p->code) {
        if (i.k != IK::Assign) return;
        Val v = eval(i.e, rd);
        if (i.drv >= 0) {
            // A driver of part of a net drives Z on the other bits, as in exec().
            if (i.hi >= 0) v = v_insert(allz(S.sigs[i.sig].w), v, i.hi, i.lo);
            S.drive(i.sig, i.drv, v, i.s0, i.s1);
        } else
            S.set(i.sig, i.hi >= 0 ? v_insert(S.get(i.sig), v, i.hi, i.lo) : v);
    }
}
