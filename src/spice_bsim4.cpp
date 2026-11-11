// spice_bsim4.cpp - BSIM4 (.model level 14/54) for mspice.
//
// The model equations are the BSIM group's own C code (BSIM4 4.8.3,
// Educational Community License 2.0), kept unmodified in third_party/bsim4.
// That code was written against the SPICE3 device interface. This file is
// the other side of that interface, on top of mspice's solver:
//
//   - node 0 is ground there and unknown -1 here, so node k is unknown k-1
//     and the solution and right-hand side are handed over one element early;
//   - every matrix entry the model asks for is a "cell" owned by this file.
//     The model adds into the cells; load() moves them into the real matrix.
//     The same cells take the complex AC stamps;
//   - the state vectors (voltages of the last evaluation, charges, charge
//     currents) are four arrays here: now, and the last three accepted points;
//   - NIintegrate() integrates a charge with mspice's coefficients, and the
//     step control uses mspice's own error estimate (Charge::maxStep).
//
// Threads: BSIM4LoadOMP() evaluates one transistor and keeps its stamps in
// the instance; BSIM4LoadRhsMat() then adds all of them serially. Any number
// of threads therefore gives the same bits.
#include <atomic>
#include <cstdarg>
#include <deque>
#include <mutex>

#include "spice_dev.hpp"

extern "C" {
#include "spice_bsim4/shim.h"
#include "bsim4def.h"
#include "bsim4ext.h"
extern IFparm BSIM4pTable[];
extern IFparm BSIM4mPTable[];
extern int BSIM4pTSize, BSIM4mPTSize;
int BSIM4LoadOMP(BSIM4instance* here, CKTcircuit* ckt);
void BSIM4LoadRhsMat(GENmodel* model, CKTcircuit* ckt);
}

namespace spice {
namespace {

// Setup (parameter checks keep a static buffer) and the frequency-dependent
// AC path run one at a time.
std::mutex gSetup;
thread_local std::string gMessages;

struct Set final : Bsim4Set {
    std::map<std::string, BSIM4model*> byName;
    BSIM4model* first = nullptr;
    BSIM4model* last = nullptr;
    std::map<BSIM4model*, BSIM4instance*> tail;
    std::vector<BSIM4instance*> inst;
    mutable CKTcircuit ckt{};
    std::vector<double> st[4];
    mutable std::vector<double> cells;   // two doubles each: real, imaginary
    std::vector<int> slot;               // matrix slot of each cell
    mutable double trash[2] = {0, 0};    // where ground's row and column go
    std::vector<double> acG, acC;
    bool acAffine = true;
    std::deque<CKTnode> nodes;
    std::vector<int> internal;
    Setup* S = nullptr;
    std::atomic<int> noncon{0};
    mutable std::mutex acMu;
    std::deque<std::string> strings;

    ~Set() override {
        for (BSIM4model* m = first; m;) {
            BSIM4model* next = BSIM4nextModel(m);
            for (BSIM4instance* h = BSIM4instances(m); h;) {
                BSIM4instance* nh = BSIM4nextInstance(h);
                std::free(h);
                h = nh;
            }
            // The size-dependent parameter sets the model code allocated.
            for (auto* p = m->pSizeDependParamKnot; p;) {
                auto* np = p->pNext;
                std::free(p);
                p = np;
            }
            std::free(m->BSIM4InstanceArray);
            std::free(m);
            m = next;
        }
    }

    char* keep(const std::string& s) { return strings.emplace_back(s).data(); }

    // Hand one name=value to the model code through its parameter table.
    template <class Apply>
    static bool give(IFparm* table, int size, const std::string& key, double v, Set& self, Apply apply) {
        for (int i = 0; i < size; i++) {
            if (key != table[i].keyword || !(table[i].dataType & IF_SET)) continue;
            IFvalue val;
            std::memset(&val, 0, sizeof val);
            switch (table[i].dataType & IF_VARTYPES) {
            case IF_REAL: val.rValue = v; break;
            case IF_INTEGER: val.iValue = (int)std::lround(v); break;
            case IF_FLAG: val.iValue = v != 0; break;
            case IF_STRING: {
                char b[40];
                std::snprintf(b, sizeof b, "%g", v);
                val.sValue = self.keep(b);
                break;
            }
            default: return false;
            }
            return apply(table[i].id, &val) == OK;
        }
        return false;
    }

    std::vector<std::string> addModel(const std::string& name, bool pmos, const Params& p) override {
        auto* m = (BSIM4model*)std::calloc(1, sizeof(BSIM4model));
        m->BSIM4modName = keep(name);
        if (last) last->gen.GENnextModel = &m->gen; else first = m;
        last = m;
        byName[name] = m;
        IFvalue one;
        std::memset(&one, 0, sizeof one);
        one.iValue = 1;
        BSIM4mParam(pmos ? BSIM4_MOD_PMOS : BSIM4_MOD_NMOS, &one, &m->gen);
        std::vector<std::string> unknown;
        for (auto& [k, v] : p)
            if (!give(BSIM4mPTable, BSIM4mPTSize, k, v, *this,
                      [&](int id, IFvalue* val) { return BSIM4mParam(id, val, &m->gen); }))
                unknown.push_back(k);
        return unknown;
    }

    std::vector<std::string> addInstance(const std::string& model, const std::string& name, const int node[4],
                                         const Params& p) override {
        BSIM4model* m = byName.at(model);
        auto* h = (BSIM4instance*)std::calloc(1, sizeof(BSIM4instance));
        h->gen.GENmodPtr = &m->gen;
        h->BSIM4name = keep(name);
        if (tail.count(m)) tail[m]->gen.GENnextInstance = &h->gen; else m->gen.GENinstances = &h->gen;
        tail[m] = h;
        *const_cast<int*>(&h->BSIM4dNode) = node[0] + 1;
        *const_cast<int*>(&h->BSIM4gNodeExt) = node[1] + 1;
        *const_cast<int*>(&h->BSIM4sNode) = node[2] + 1;
        *const_cast<int*>(&h->BSIM4bNode) = node[3] + 1;
        std::vector<std::string> unknown;
        for (auto& [k, v] : p)
            if (!give(BSIM4pTable, BSIM4pTSize, k, v, *this,
                      [&](int id, IFvalue* val) { return BSIM4param(id, val, &h->gen, nullptr); }))
                unknown.push_back(k);
        return unknown;
    }

    size_t size() const override { return inst.size(); }

    std::vector<int> setup(Setup& setup, const Options& opt) override {
        std::lock_guard<std::mutex> g(gSetup);
        S = &setup;
        size_t count = 0;
        for (BSIM4model* m = first; m; m = BSIM4nextModel(m))
            for (BSIM4instance* h = BSIM4instances(m); h; h = BSIM4nextInstance(h)) count++;
        cells.reserve(count * 2 * 128);   // the model asks for at most 93 entries a transistor
        slot.reserve(count * 128);
        ckt.host = this;
        ckt.CKTtemp = opt.temp + CONSTCtoK;
        ckt.CKTnomTemp = opt.tnom + CONSTCtoK;
        ckt.CKTreltol = opt.reltol; ckt.CKTabstol = opt.abstol; ckt.CKTvoltTol = opt.vntol;
        ckt.CKTgmin = opt.gmin;
        gMessages.clear();
        int states = 0;
        if (first) {
            int err = BSIM4setup((SMPmatrix*)this, &first->gen, &ckt, &states);
            if (err) throw Error("BSIM4: setup failed" + (gMessages.empty() ? std::string() : ": " + gMessages));
            for (auto& s : st) s.assign(states, 0.0);
            point();
            if (BSIM4temp(&first->gen, &ckt))
                throw Error("BSIM4: a model parameter is out of range" + (gMessages.empty() ? std::string() : ": " + gMessages));
            inst.assign(first->BSIM4InstanceArray, first->BSIM4InstanceArray + first->BSIM4InstCount);
        }
        S = nullptr;
        return internal;
    }

    void point() { for (int k = 0; k < 4; k++) ckt.CKTstates[k] = st[k].data(); }

    // The voltages the model would read from the solution, written into the
    // state instead: an evaluation in small-signal mode then is exactly at x.
    void statesFromSolution(const double* x) {
        double* s0 = st[0].data();
        for (BSIM4instance* h : inst) {
            double type = BSIM4modPtr(h)->BSIM4type, vs = x[h->BSIM4sNodePrime];
            double* s = s0 + h->BSIM4states;
            double vds = type * (x[h->BSIM4dNodePrime] - vs), vbs = type * (x[h->BSIM4bNodePrime] - vs);
            double vdbs = type * (x[h->BSIM4dbNode] - vs);
            s[3] = vds;
            s[2] = type * (x[h->BSIM4gNodePrime] - vs);
            s[1] = vbs;
            s[0] = vbs - vds;
            s[7] = type * (x[h->BSIM4gNodeExt] - vs);
            s[8] = type * (x[h->BSIM4gNodeMid] - vs);
            s[4] = vdbs;
            s[5] = vdbs - vds;
            s[6] = type * (x[h->BSIM4sbNode] - vs);
            s[9] = type * (x[h->BSIM4sNode] - vs);
            s[10] = type * (x[h->BSIM4dNode] - vs);
            s[27] = type * x[h->BSIM4qNode];
        }
    }

    void eval(const Ctx& c, double* xBuf, par::Pool* pool) override {
        ckt.CKTrhsOld = xBuf;
        ckt.CKTgmin = c.gmin;
        ckt.CKTreltol = c.reltol; ckt.CKTabstol = c.abstol;
        ckt.CKTag[0] = c.a; ckt.CKTag[1] = c.b;
        ckt.CKTbypass = c.bypass;
        if (c.noLimit) {
            statesFromSolution(xBuf);
            ckt.CKTmode = MODEDCOP | MODEINITSMSIG;
        } else if (c.a == 0) {
            ckt.CKTmode = MODEDCOP | (c.initJct ? MODEINITJCT : MODEINITFLOAT);
        } else {
            ckt.CKTmode = MODETRAN | MODEINITFLOAT;
            if (c.iter == 0) {
                // A new step (or a new try at it): start from the accepted
                // point. The charge currents are set to what the new step
                // gives for an unchanged charge, which is what a transistor
                // that stays latent (bypassed) in this step needs; one that
                // is evaluated integrates its charges again anyway.
                st[0] = st[1];
                double* s0 = st[0].data();
                for (const BSIM4instance* h : inst) {
                    double* s = s0 + h->BSIM4states;
                    for (int q = 12; q <= 26; q += 2) s[q] *= -c.b;
                }
            }
        }
        noncon.store(0, std::memory_order_relaxed);
        auto some = [&](size_t b, size_t e, size_t) {
            CKTcircuit local = ckt;
            local.CKTnoncon = 0;
            for (size_t i = b; i < e; i++) BSIM4LoadOMP(inst[i], &local);
            if (local.CKTnoncon) noncon.fetch_add(local.CKTnoncon, std::memory_order_relaxed);
        };
        size_t n = inst.size();
        if (pool && pool->threads() > 1 && n >= 32)
            pool->chunks(n, std::clamp<size_t>(n / (8 * pool->threads()), 1, 64), some);
        else
            some(0, n, 0);
    }

    void scatter(double* val) const {
        double* c = cells.data();
        for (size_t k = 0, e = slot.size(); k < e; k++, c += 2) {
            val[slot[k]] += c[0];
            c[0] = 0;
        }
    }

    void load(double* val, double* rhsBuf) override {
        if (!first) return;
        ckt.CKTrhs = rhsBuf;
        BSIM4LoadRhsMat(&first->gen, &ckt);
        scatter(val);
    }

    bool limited() const override { return noncon.load(std::memory_order_relaxed) != 0; }

    void accept() override {
        std::swap(st[3], st[2]);
        std::swap(st[2], st[1]);
        std::swap(st[1], st[0]);
        point();   // st[0] is refilled from st[1] when the next step starts
    }

    // The charges of a transistor: offsets into its states (the current of
    // a charge is the next state).
    template <class F> static void eachCharge(const BSIM4instance* h, F f) {
        f(11); f(13); f(15);                       // bulk, gate, drain
        if (h->BSIM4trnqsMod) f(25);
        if (h->BSIM4rbodyMod) { f(19); f(21); }
        if (h->BSIM4rgateMod == 3) f(17);
    }

    void start() override {
        for (BSIM4instance* h : inst)
            for (int q = 11; q <= 25; q += 2)
                st[0][h->BSIM4states + q + 1] = st[1][h->BSIM4states + q + 1] = 0;
        st[0] = st[1];
        st[2] = st[1];
        st[3] = st[1];
        point();
    }

    double maxStep(const double* hist, int order, const Options& opt) const override {
        double room = 1e300;
        for (const BSIM4instance* h : inst) {
            int base = h->BSIM4states;
            eachCharge(h, [&](int q) {
                Charge ch;
                size_t i = base + q;
                ch.q = st[0][i]; ch.i = st[0][i + 1];
                ch.q0 = st[1][i]; ch.i0 = st[1][i + 1];
                ch.q1 = st[2][i]; ch.q2 = st[3][i];
                room = std::min(room, ch.maxStep(hist, order, opt.reltol, opt.abstol, opt.trtol, opt.chgtol));
            });
        }
        return room;
    }

    void acStamps(double w) const {
        ckt.CKTomega = w;
        ckt.CKTmode = MODEAC;
        BSIM4acLoad(&first->gen, &ckt);
    }

    // Without the non-quasi-static option a transistor's AC stamps are
    // G + jwC. Found once, they are added at any frequency from any thread.
    void prepareAC() override {
        if (!first) return;
        size_t n = slot.size();
        std::fill(cells.begin(), cells.end(), 0.0);
        acStamps(1);
        std::vector<double> one(cells.begin(), cells.begin() + 2 * n);
        std::fill(cells.begin(), cells.end(), 0.0);
        acStamps(3);
        acG.resize(n); acC.resize(n);
        acAffine = true;
        for (size_t k = 0; k < n; k++) {
            acG[k] = one[2 * k]; acC[k] = one[2 * k + 1];
            double g3 = cells[2 * k], c3 = cells[2 * k + 1];
            if (std::fabs(g3 - acG[k]) > 1e-9 * std::fabs(g3) || std::fabs(c3 - 3 * acC[k]) > 1e-9 * std::fabs(c3))
                acAffine = false;
        }
        std::fill(cells.begin(), cells.end(), 0.0);
    }

    void loadAC(double w, cplx* val) const override {
        if (!first) return;
        size_t n = slot.size();
        if (acAffine) {
            for (size_t k = 0; k < n; k++)
                val[slot[k]] += cplx(acG[k], w * acC[k]);
            return;
        }
        std::lock_guard<std::mutex> g(acMu);
        acStamps(w);
        double* c = cells.data();
        for (size_t k = 0; k < n; k++, c += 2) {
            val[slot[k]] += cplx(c[0], c[1]);
            c[0] = c[1] = 0;
        }
    }
};

Set* host(CKTcircuit* ckt) { return static_cast<Set*>(ckt->host); }

}   // namespace

std::unique_ptr<Bsim4Set> makeBsim4() { return std::make_unique<Set>(); }

}   // namespace spice

// ---------------------------------------------------------------------------
// What the model code calls.

extern "C" {

double CONSTroot2 = 1.4142135623730950488;
double CONSTvt0 = CONSTboltz * (27 + CONSTCtoK) / CHARGE;
double CONSTKoverQ = CONSTboltz / CHARGE;
double CONSTe = 2.7182818284590452354;

// No analysis list: the model asks only whether a noise analysis is planned.
static TSKtask gTask = {nullptr};
static struct circ gCirc = {&gTask};
struct circ* ft_curckt = &gCirc;

static void message(int flags, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (flags & (ERR_FATAL | ERR_PANIC)) spice::gMessages += b;
    else std::fprintf(stderr, "mspice: BSIM4: %s\n", b);
}
static IFfrontEnd gFront = {message};
IFfrontEnd* SPfrontEnd = &gFront;

char* copy(const char* s) {
    if (!s) return nullptr;
    char* r = (char*)std::malloc(std::strlen(s) + 1);
    std::strcpy(r, s);
    return r;
}

char* tprintf(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    char* r = (char*)std::malloc((size_t)n + 1);
    std::vsnprintf(r, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return r;
}

FILE* mspice_bsim4_nofile(const char*, const char*) { return nullptr; }

void wl_append_word(wordlist** first, wordlist** last, char* word) {
    auto* w = (wordlist*)std::calloc(1, sizeof(wordlist));
    w->wl_word = word;
    w->wl_prev = *last;
    if (*last) (*last)->wl_next = w;
    else *first = w;
    *last = w;
}

void wl_free(wordlist* w) {
    while (w) {
        wordlist* n = w->wl_next;
        std::free(w->wl_word);
        std::free(w);
        w = n;
    }
}

// No interpreter variables: no "scale", and the parameter check stays on.
int cp_getvar(char*, enum cp_types, void*, size_t) { return 0; }

int CKTmkVolt(CKTcircuit* ckt, CKTnode** node, IFuid name, char* suffix) {
    spice::Set* h = spice::host(ckt);
    int idx = h->S->newUnknown(std::string(name) + "#" + suffix);
    CKTnode& n = h->nodes.emplace_back();
    n.number = idx + 1;
    h->internal.push_back(idx);
    *node = &n;
    return OK;
}

int CKTinst2Node(CKTcircuit*, void*, int, CKTnode**, IFuid*) { return E_NOTERM; }
int CKTdltNNum(CKTcircuit*, int) { return OK; }

double* SMPmakeElt(SMPmatrix* m, int row, int col) {
    auto* h = reinterpret_cast<spice::Set*>(m);
    if (row <= 0 || col <= 0) return h->trash;   // ground's row or column
    if (h->cells.size() + 2 > h->cells.capacity()) return nullptr;
    h->slot.push_back(h->S->slot(row - 1, col - 1));
    h->cells.push_back(0);
    h->cells.push_back(0);
    return &h->cells[h->cells.size() - 2];
}

// i = a*(q - q1) - b*i1: backward Euler (a = 1/h, b = 0) or trapezoidal
// (a = 2/h, b = 1), as everywhere in mspice.
int NIintegrate(CKTcircuit* ckt, double* geq, double* ceq, double cap, int qcap) {
    double a = ckt->CKTag[0], b = ckt->CKTag[1];
    double* s0 = ckt->CKTstate0;
    const double* s1 = ckt->CKTstate1;
    s0[qcap + 1] = a * (s0[qcap] - s1[qcap]) - b * s1[qcap + 1];
    *geq = a * cap;
    *ceq = s0[qcap + 1] - a * s0[qcap];
    return OK;
}

// The step control asks Bsim4Set::maxStep() instead.
void CKTterr(int, CKTcircuit*, double*) {}

double DEVfetlim(double vnew, double vold, double vto) { return spice::fetlim(vnew, vold, vto); }
double DEVlimvds(double vnew, double vold) { return spice::limvds(vnew, vold); }
double DEVpnjlim(double vnew, double vold, double vt, double vcrit, int* icheck) {
    bool lim = false;
    double v = spice::pnjlim(vnew, vold, vt, vcrit, lim);
    *icheck = lim;
    return v;
}

}   // extern "C"
