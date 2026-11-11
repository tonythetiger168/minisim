// spice.hpp - mspice's circuit, analyses and measurements.
//
//   Netlist --Builder--> Circuit (nodes, devices, matrix pattern)
//   Circuit::op / dcSweep / ac / tran --> Result (one table per analysis)
//   measure() evaluates .measure lines on a Result.
//
// The equations are modified nodal analysis: one unknown per node voltage and
// one per branch current (voltage sources, inductors, E and H sources).
// Nonlinear circuits are solved by Newton iteration with SPICE's junction
// limiting; when a plain start fails, by stepping a shunt conductance down
// and then by ramping the sources up. Transient analysis integrates with the
// trapezoidal rule (backward Euler for the first step after a waveform
// corner) and chooses the step from the local truncation error.
#pragma once
#include <cstdio>
#include <deque>
#include <tuple>
#include <unordered_set>

#include "spice_behav.hpp"

namespace spice {

struct Model {
    std::string name, type;
    std::map<std::string, double> p;
    mutable std::set<std::string> used;
    bool has(const std::string& k) const { used.insert(k); return p.count(k) != 0; }
    double get(const std::string& k, double d) const {
        used.insert(k);
        auto it = p.find(k);
        return it == p.end() ? d : it->second;
    }
};

struct Probe {
    std::string label;
    char kind = 'v';   // 'v' node voltage (a minus b), 'i' branch current (a)
    int a = -1, b = -1;
    char fn = 0;       // AC: 0 or 'm' magnitude, 'd' dB, 'p' phase in degrees, 'r' real, 'i' imaginary
};

struct Result {
    std::string kind;    // op, dc, ac, tran
    std::string xname;   // sweep variable: time, frequency, a source name
    std::vector<std::string> cols;
    std::vector<double> x;
    std::vector<std::vector<double>> y;   // y[point][column]
    std::vector<std::string> printed;     // the .print columns, in order (empty: none asked)
    std::vector<std::pair<std::string, double>> meas;
    double tstep = 0;                     // tran: the print interval
    std::vector<std::string> text;        // more to print: noise contributions, Fourier tables
    size_t iterations = 0, accepted = 0, rejected = 0;

    int col(const std::string& label) const {
        for (size_t i = 0; i < cols.size(); i++)
            if (cols[i] == label) return (int)i;
        return -1;
    }
    // Column c at x, by linear interpolation: the first place the sweep
    // passes x (a nested .dc passes it once per outer value).
    double at(int c, double xv) const {
        if (x.empty()) return NAN;
        for (size_t k = 1; k < x.size(); k++) {
            double x0 = x[k - 1], x1 = x[k];
            if ((x0 - xv) * (x1 - xv) > 0) continue;
            return x1 == x0 ? y[k - 1][c] : y[k - 1][c] + (y[k][c] - y[k - 1][c]) * (xv - x0) / (x1 - x0);
        }
        return std::fabs(x.front() - xv) <= std::fabs(x.back() - xv) ? y.front()[c] : y.back()[c];
    }
};

struct AnalysisSpec {
    enum Type { OP, DC, AC, TRAN, NOISE, TF, SENS } type = OP;
    std::string out;   // .noise, .tf, .sens: the output, v(node), v(a,b) or i(source)
    std::string where;
    std::string src1, src2;
    double a1 = 0, b1 = 0, s1 = 0, a2 = 0, b2 = 0, s2 = 0;
    std::string acMode = "dec";
    int npts = 10;
    double f1 = 1, f2 = 1;
    double tstep = 0, tstop = 0, tstart = 0, tmax = 0;
    bool uic = false;
    const char* name() const {
        static const char* n[] = {"op", "dc", "ac", "tran", "noise", "tf", "sens"};
        return n[type];
    }
};

struct Circuit {
    Options opt;
    std::map<std::string, int> nodeIndex;
    std::vector<std::string> names;   // unknowns: node voltages, then branch currents
    int numNodes = 0, n = 0;
    // Devices live in one container per type: the loops that run every
    // Newton iteration walk contiguous memory and call no virtual function.
    // Stamps are therefore added type by type, each type in netlist order.
    template <class... T> using PoolsOf = std::tuple<std::deque<T>...>;
    PoolsOf<Resistor, Capacitor, Inductor, VSource, ISource, VCVS, VCCS, CCCS, CCVS, Diode, Mosfet, Bjt, Behav, Switch> pools;
    template <class D> std::deque<D>& pool() { return std::get<std::deque<D>>(pools); }
    template <class F> void eachPool(F&& f) {
        std::apply([&](auto&... p) { (f(p), ...); }, pools);
    }
    std::vector<Device*> devs;   // every device, in netlist order
    std::unique_ptr<Bsim4Set> bsim4;   // the BSIM4 transistors, when there are any
    std::vector<char> voltLike;        // per unknown: a node voltage (also a device's internal node)
    std::map<std::string, Device*> byName;
    bool linear = true;
    std::map<int, double> ic, nodeset;
    std::vector<std::string> warnings;
    Pattern pat;
    std::vector<int> diag;
    SparseLU<double> lu;
    std::vector<double> val, rhsBuf, xBuf, xnewBuf;
    double* rhs = nullptr;
    double* x = nullptr;   // x[-1] is ground
    double* xnew = nullptr;
    Pool* pool_ = nullptr;
    double gdiag = 0;       // shunt on every node (gmin stepping, .option gshunt)
    bool holdIC = false, holdNodeset = false;
    // A linear circuit's matrix depends only on these, so its factors are reused.
    double factoredA = -1, factoredG = -1;
    bool factoredHold = false, factored = false;
    size_t iterations = 0;

    int node(const std::string& name) {
        if (name == "0" || name == "gnd") return -1;
        auto [it, fresh] = nodeIndex.try_emplace(name, (int)names.size());
        if (fresh) names.push_back(name);
        return it->second;
    }

    template <class D> D* add(const std::string& name) {
        if (byName.count(name)) throw Error("two elements are named '" + name + "'");
        D* raw = &pool<D>().emplace_back();
        raw->name = name;
        byName[name] = raw;
        devs.push_back(raw);
        return raw;
    }

    void finalize() {
        numNodes = (int)names.size();
        Setup S{pat, names};
        for (Device* d : devs) d->branches(S);
        for (Device* d : devs) d->bind(S);
        std::vector<int> inner;
        if (bsim4) inner = bsim4->setup(S, opt);
        n = (int)names.size();
        voltLike.assign(n, 0);
        std::fill(voltLike.begin(), voltLike.begin() + numNodes, 1);
        for (int i : inner) voltLike[i] = 1;
        diag.resize(numNodes);
        for (int i = 0; i < numNodes; i++) diag[i] = pat.slot(i, i);
        pat.finalize(n);
        lu.init(pat);
        val.assign(pat.slots.size(), 0);
        rhsBuf.assign(n + 1, 0); xBuf.assign(n + 1, 0); xnewBuf.assign(n + 1, 0);
        rhs = rhsBuf.data() + 1; x = xBuf.data() + 1; xnew = xnewBuf.data() + 1;
        linear = pool<Diode>().empty() && pool<Mosfet>().empty() && pool<Bjt>().empty() && pool<Behav>().empty() &&
                 pool<Switch>().empty() && !bsim4;
    }

    Ctx context() const {
        Ctx c;
        c.x = x;
        c.gmin = opt.gmin;
        c.vt = kBoltzOverQ * (opt.temp + 273.15);
        c.reltol = opt.reltol; c.abstol = opt.abstol;
        c.bypass = opt.bypass;
        return c;
    }

    template <class D> void evalPool(const Ctx& c) {
        auto& p = pool<D>();
        if (pool_ && pool_->threads() > 1 && p.size() >= (size_t)opt.parMin)
            pool_->chunks(p.size(), std::clamp<size_t>(p.size() / (8 * pool_->threads()), 1, 256),
                          [&](size_t b, size_t e, size_t) { for (size_t i = b; i < e; i++) p[i].eval(c); });
        else
            for (D& d : p) d.eval(c);
    }
    // Parallel evaluation and assembly of the MOSFETs.
    //
    // The MOSFETs are cut into chunks of consecutive devices. A matrix entry
    // or right-hand-side row that only one chunk touches is "private": that
    // chunk's worker adds to it directly. The others ("shared": a supply
    // node, a node on a chunk boundary) are added afterwards on one thread.
    // Either way an entry receives its MOSFET terms in device order, after
    // the element types loaded before and ahead of those loaded after, so
    // the sums are those of the serial loop, bit for bit, whatever the chunk
    // size or thread count.
    struct MosPlan {
        size_t chunk = 0, count = 0;
        std::vector<uint32_t> priv;      // per device; bit 4i+j: matrix entry (i, j), bit 16+i: row i
        std::vector<uint32_t> first;     // per device: its first shared term
        std::vector<uint8_t> bit;        // per shared term: which entry of the device
        std::vector<int> to;             // per shared term: the matrix entry or row it is added to
        std::vector<double> term;        // per shared term: its value this iteration
    } mosPlan_;

    void planMosfets(size_t chunk) {
        auto& p = pool<Mosfet>();
        MosPlan& pl = mosPlan_;
        pl.chunk = chunk; pl.count = p.size();
        std::vector<int> slotOwner(pat.slots.size(), -1), rowOwner(n, -1);
        auto claim = [](int& owner, int k) { owner = owner == -1 || owner == k ? k : -2; };
        for (size_t i = 0; i < p.size(); i++) {
            int k = (int)(i / chunk);
            for (int a = 0; a < 4; a++) {
                if (p[i].node[a] >= 0) claim(rowOwner[p[i].node[a]], k);
                for (int b = 0; b < 4; b++)
                    if (p[i].sl.s[a][b]) claim(slotOwner[p[i].sl.s[a][b]], k);
            }
        }
        pl.priv.assign(p.size(), 0);
        pl.first.clear(); pl.bit.clear(); pl.to.clear();
        for (size_t i = 0; i < p.size(); i++) {
            pl.first.push_back((uint32_t)pl.bit.size());
            auto shared = [&](int bit, int to) { pl.bit.push_back((uint8_t)bit); pl.to.push_back(to); };
            // Matrix entries first, then rows: the order of the serial loop.
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                    if (int s = p[i].sl.s[a][b]) {
                        if (slotOwner[s] >= 0) pl.priv[i] |= 1u << (4 * a + b);
                        else shared(4 * a + b, s);
                    }
            for (int a = 0; a < 4; a++)
                if (int nd = p[i].node[a]; nd >= 0) {
                    if (rowOwner[nd] >= 0) pl.priv[i] |= 1u << (16 + a);
                    else shared(16 + a, nd);
                }
        }
        pl.first.push_back((uint32_t)pl.bit.size());
        pl.term.assign(pl.bit.size(), 0);
    }

    void loadMosfets(const Ctx& c) {
        auto& p = pool<Mosfet>();
        size_t chunk = std::clamp<size_t>(p.size() / (8 * pool_->threads()), 4, 256);
        if (mosPlan_.count != p.size() || mosPlan_.chunk != chunk) planMosfets(chunk);
        MosPlan& pl = mosPlan_;
        double* v = val.data();
        size_t chunks = (p.size() + chunk - 1) / chunk;
        pool_->run(chunks, [&](size_t k) {
            size_t end = std::min(p.size(), (k + 1) * chunk);
            for (size_t i = k * chunk; i < end; i++) {
                Mosfet& d = p[i];
                d.eval(c);
                double m[4][4], r[4];
                d.stamp(m, r);
                uint32_t bits = pl.priv[i];
                for (int a = 0; a < 4; a++) {
                    if (bits >> (16 + a) & 1) rhs[d.node[a]] += r[a];
                    for (int b = 0; b < 4; b++)
                        if (bits >> (4 * a + b) & 1) v[d.sl.s[a][b]] += m[a][b];
                }
                for (uint32_t t = pl.first[i]; t < pl.first[i + 1]; t++)
                    pl.term[t] = pl.bit[t] < 16 ? m[pl.bit[t] / 4][pl.bit[t] % 4] : r[pl.bit[t] - 16];
            }
        });
        for (size_t t = 0; t < pl.term.size(); t++) (pl.bit[t] < 16 ? v[pl.to[t]] : rhs[pl.to[t]]) += pl.term[t];
    }

    template <class D> bool anyLimited() {
        for (D& d : pool<D>())
            if (d.limited) return true;
        return false;
    }

    // Build the linear system at x. With rhsOnly the matrix is known to be
    // what it was (a linear circuit, same integration coefficients), and only
    // the right-hand side is made.
    void loadAll(const Ctx& c, bool rhsOnly = false) {
        std::fill(rhsBuf.begin(), rhsBuf.end(), 0.0);
        if (rhsOnly) {
            for (auto& d : pool<Capacitor>()) d.rhsOnly(c, rhs);
            for (auto& d : pool<Inductor>()) d.rhsOnly(c, rhs);
            for (auto& d : pool<VSource>()) d.rhsOnly(c, rhs);
            for (auto& d : pool<ISource>()) d.rhsOnly(c, rhs);
        } else {
            std::fill(val.begin(), val.end(), 0.0);
            bool parMos = pool_ && pool_->threads() > 1 && pool<Mosfet>().size() >= (size_t)opt.parMin;
            evalPool<Diode>(c);
            if (!parMos) evalPool<Mosfet>(c);
            evalPool<Bjt>(c);
            double* v = val.data();
            eachPool([&](auto& p) {
                if constexpr (std::is_same_v<std::decay_t<decltype(p)>, std::deque<Mosfet>>) {
                    if (parMos) { loadMosfets(c); return; }
                }
                for (auto& d : p) d.load(c, v, rhs);
            });
            if (bsim4) {
                bsim4->eval(c, xBuf.data(), pool_);
                bsim4->load(v, rhsBuf.data());
            }
            if (gdiag != 0)
                for (int i = 0; i < numNodes; i++) val[diag[i]] += gdiag;
        }
        auto hold = [&](const std::map<int, double>& m) {
            for (auto& [nd, v] : m) {
                if (!rhsOnly) val[diag[nd]] += 1e10;
                rhs[nd] += 1e10 * v;
            }
        };
        if (holdIC) hold(ic);
        if (holdNodeset) hold(nodeset);
    }

    [[noreturn]] void singular(const SingularMatrix& e) {
        std::string nm = names[e.column];
        if (e.column < numNodes)
            throw Error("singular matrix at node '" + nm + "': it has no DC path to ground (or a loop of "
                        "voltage sources and inductors fixes it twice)");
        throw Error("singular matrix at '" + nm + "': a loop of voltage sources or inductors");
    }

    // Newton iteration from x. True when converged; x holds the result.
    bool newton(Ctx& c, int maxIter, bool initJct = false) {
        for (int it = 0; it < maxIter; it++) {
            c.initJct = initJct && it == 0;
            c.iter = it;
            bool hold = holdIC || holdNodeset;
            bool reuse = linear && factored && factoredA == c.a && factoredG == gdiag && factoredHold == hold;
            loadAll(c, reuse);
            iterations++;
            if (!reuse) {
                try { lu.factor(val.data()); }
                catch (const SingularMatrix& e) { singular(e); }
                factored = true; factoredA = c.a; factoredG = gdiag; factoredHold = hold;
            }
            lu.solve(rhs, xnew);
            bool conv = linear || (it > 0 && !anyLimited<Diode>() && !anyLimited<Mosfet>() && !anyLimited<Bjt>() && !anyLimited<Switch>() &&
                                  !(bsim4 && bsim4->limited()));
            bool finite = true;
            if (linear) {
                for (int i = 0; i < n; i++) finite &= std::isfinite(xnew[i]);
            } else {
                double rt = opt.reltol;
                for (int i = 0; i < n; i++) {
                    double a = xnew[i], b = x[i], d = std::fabs(a - b);
                    finite &= std::isfinite(a);
                    if (d > rt * std::max(std::fabs(a), std::fabs(b)) + (voltLike[i] ? opt.vntol : opt.abstol)) conv = false;
                }
            }
            if (!finite) { c.initJct = false; return false; }
            std::copy(xnew, xnew + n, x);
            if (conv) { c.initJct = false; return true; }
        }
        c.initJct = false;
        return false;
    }

    // Evaluate the devices exactly at x (no limiting), so that charges and
    // small-signal values belong to the solution.
    void settle(Ctx& c) {
        c.noLimit = true;
        loadAll(c);
        c.noLimit = false;
    }

    // DC operating point into x.
    void op(Ctx& c) {
        c.a = 0; c.b = 0;
        std::vector<double> start(x, x + n);
        auto restart = [&] { std::copy(start.begin(), start.end(), x); };
        gdiag = opt.gshunt;
        c.srcScale = 1;
        holdNodeset = !nodeset.empty();
        bool ok = newton(c, opt.itl1, true);
        if (ok && holdNodeset) { holdNodeset = false; ok = newton(c, opt.itl1); }
        holdNodeset = false;
        if (!ok) {
            // Shunt stepping: a conductance from every node to ground makes
            // the circuit easy. Take it away in steps, each from the last
            // solution; when a step fails, go back and take a smaller one.
            restart();
            double target = std::max(opt.gmin, 1e-13), factor = 10, g = 1e-2, oldG = 1e-2;
            std::vector<double> good(x, x + n);
            bool first = true;
            for (ok = false;;) {
                gdiag = opt.gshunt + g;
                int before = (int)iterations;
                if (newton(c, opt.itl1, first)) {
                    int used = (int)iterations - before;
                    if (g <= target) { ok = true; break; }
                    good.assign(x, x + n);
                    if (used <= opt.itl1 / 4) factor = std::min(factor * std::sqrt(factor), 10.0);
                    if (used > 3 * opt.itl1 / 4) factor = std::max(std::sqrt(factor), 1.00005);
                    oldG = g;
                    if (g < factor * target) g = target;
                    else g /= factor;
                    first = false;
                } else {
                    if (first || factor < 1.00005) break;
                    factor = std::sqrt(std::sqrt(factor));
                    g = oldG / factor;
                    std::copy(good.begin(), good.end(), x);
                }
            }
            gdiag = opt.gshunt;
            if (ok) ok = newton(c, opt.itl1);
        }
        if (!ok) {
            // Source stepping: ramp every independent source up from zero.
            restart();
            double s = 0, step = 0.1;
            c.srcScale = 0;
            ok = newton(c, opt.itl1, true);
            std::vector<double> good(x, x + n);
            while (ok && s < 1) {
                double trial = std::min(1.0, s + step);
                c.srcScale = trial;
                if (newton(c, opt.itl1)) {
                    s = trial; step *= 1.5;
                    good.assign(x, x + n);
                } else {
                    std::copy(good.begin(), good.end(), x);
                    step /= 4;
                    if (step < 1e-6) ok = false;
                }
            }
            c.srcScale = 1;
        }
        if (!ok) throw Error("no convergence in the DC operating point (direct, shunt stepping and source stepping all failed)");
        settle(c);
    }

    double probe(const Probe& p) const { return p.kind == 'i' ? x[p.a] : x[p.a] - x[p.b]; }

    void record(Result& r, const std::vector<Probe>& probes, double xv) const {
        r.x.push_back(xv);
        std::vector<double> row(probes.size());
        for (size_t i = 0; i < probes.size(); i++) row[i] = probe(probes[i]);
        r.y.push_back(std::move(row));
    }

    Wave& sourceWave(const std::string& name, const std::string& where) {
        auto it = byName.find(name);
        if (it != byName.end()) {
            if (auto* v = dynamic_cast<VSource*>(it->second)) return v->w;
            if (auto* i = dynamic_cast<ISource*>(it->second)) return i->w;
        }
        throw Error(where + ": .dc needs an independent source, and '" + name + "' is not one");
    }

    void runOp(Result& r, const std::vector<Probe>& probes) {
        Ctx c = context();
        holdIC = !ic.empty();
        op(c);
        holdIC = false;
        record(r, probes, 0);
    }

    void runDC(const AnalysisSpec& a, Result& r, const std::vector<Probe>& probes) {
        Ctx c = context();
        auto count = [&](double lo, double hi, double st) {
            if (st == 0 || (hi - lo) / st < 0) throw Error(a.where + ": .dc step has the wrong sign or is zero");
            return (size_t)std::floor((hi - lo) / st + 1e-9) + 1;
        };
        Wave& w1 = sourceWave(a.src1, a.where);
        Wave* w2 = a.src2.empty() ? nullptr : &sourceWave(a.src2, a.where);
        size_t n1 = count(a.a1, a.b1, a.s1), n2 = w2 ? count(a.a2, a.b2, a.s2) : 1;
        Wave save1 = w1, save2 = w2 ? *w2 : Wave();
        for (size_t j = 0; j < n2; j++) {
            if (w2) { w2->kind = Wave::DC; w2->hasDC = true; w2->dc = a.a2 + j * a.s2; }
            for (size_t i = 0; i < n1; i++) {
                w1.kind = Wave::DC; w1.hasDC = true; w1.dc = a.a1 + i * a.s1;
                bool ok = false;
                if (i > 0) {
                    std::vector<double> keep(x, x + n);
                    ok = newton(c, 50);
                    if (ok) settle(c);
                    else std::copy(keep.begin(), keep.end(), x);
                }
                if (!ok) op(c);
                record(r, probes, w1.dc);
                if (w2) r.y.back().push_back(w2->dc);
            }
        }
        w1 = save1;
        if (w2) *w2 = save2;
    }

    static std::vector<double> frequencies(const AnalysisSpec& a) {
        std::vector<double> f;
        if (a.f1 <= 0 || a.f2 < a.f1 || a.npts < 1) throw Error(a.where + ": .ac needs 0 < fstart <= fstop and at least one point");
        if (a.acMode == "lin") {
            for (int i = 0; i < a.npts; i++) f.push_back(a.npts == 1 ? a.f1 : a.f1 + (a.f2 - a.f1) * i / (a.npts - 1));
        } else {
            double per = a.acMode == "oct" ? 2 : 10;
            double total = std::log(a.f2 / a.f1) / std::log(per) * a.npts;
            int cnt = (int)std::floor(total + 1e-9);
            for (int i = 0; i <= cnt; i++) f.push_back(a.f1 * std::pow(per, (double)i / a.npts));
        }
        return f;
    }

    static double reduce(cplx z, char fn) {
        switch (fn) {
        case 'd': return 20 * std::log10(std::abs(z));
        case 'p': return std::arg(z) * 180 / M_PI;
        case 'r': return z.real();
        case 'i': return z.imag();
        default: return std::abs(z);
        }
    }

    // The small-signal matrix at one frequency, and the room to solve it.
    struct ACWork {
        std::vector<cplx> val, rhs, x;
        SparseLU<cplx> lu;
    };
    void loadACAt(ACWork& w, double freq) {
        std::fill(w.val.begin(), w.val.end(), cplx());
        std::fill(w.rhs.begin(), w.rhs.end(), cplx());
        double om = 2 * M_PI * freq;
        cplx* v = w.val.data();
        cplx* r = w.rhs.data() + 1;
        eachPool([&](auto& p) { for (const auto& d : p) d.loadAC(om, v, r); });
        if (bsim4) bsim4->loadAC(om, v);
        if (opt.gshunt != 0)
            for (int i = 0; i < numNodes; i++) w.val[diag[i]] += opt.gshunt;
    }

    // The operating point that a small-signal analysis linearizes around.
    void smallSignalPoint() {
        Ctx c = context();
        holdIC = !ic.empty();
        op(c);
        holdIC = false;
        if (bsim4) bsim4->prepareAC();
    }

    // point(i, w, lu) for every frequency, with the matrix of f[i] factored
    // in lu and the sources' AC values in w.rhs. The sweep is cut in one
    // piece per thread; point must write only to what belongs to i.
    template <class F>
    void sweepAC(const std::vector<double>& f, F&& point) {
        auto fresh = [&](ACWork& w) {
            w.val.assign(pat.slots.size(), cplx());
            w.rhs.assign(n + 1, cplx());
            w.x.assign(n + 1, cplx());
        };
        // One pivot order, found at the first frequency, for every worker:
        // a point's result is then the same whichever thread computes it.
        ACWork base;
        fresh(base);
        base.lu.init(pat);
        loadACAt(base, f[0]);
        try { base.lu.factor(base.val.data()); }
        catch (const SingularMatrix& e) { singular(e); }
        size_t nt = std::min<size_t>(pool_ ? pool_->threads() : 1, f.size());
        std::vector<ACWork> work(nt);
        for (auto& w : work) { fresh(w); w.lu.adopt(base.lu); }
        auto one = [&](size_t i, size_t piece) {
            ACWork& w = work[piece];
            loadACAt(w, f[i]);
            if (w.lu.refactor(w.val.data())) {
                point(i, w, w.lu);
            } else {
                SparseLU<cplx> own;
                own.init(pat);
                own.factor(w.val.data());
                point(i, w, own);
            }
        };
        try {
            auto piece = [&](size_t k) {
                for (size_t i = k * f.size() / nt; i < (k + 1) * f.size() / nt; i++) one(i, k);
            };
            if (pool_) pool_->run(nt, piece);
            else for (size_t k = 0; k < nt; k++) piece(k);
        } catch (const SingularMatrix& e) { singular(e); }
    }

    void runAC(const AnalysisSpec& a, Result& r, const std::vector<Probe>& probes) {
        smallSignalPoint();
        std::vector<double> f = frequencies(a);
        r.x = f;
        r.y.assign(f.size(), {});
        sweepAC(f, [&](size_t i, ACWork& w, SparseLU<cplx>& lu) {
            lu.solve(w.rhs.data() + 1, w.x.data() + 1);
            const cplx* xs = w.x.data() + 1;
            std::vector<double> row(probes.size());
            for (size_t k = 0; k < probes.size(); k++) {
                const Probe& p = probes[k];
                cplx z = p.kind == 'i' ? xs[p.a] : xs[p.a] - xs[p.b];
                row[k] = reduce(z, p.fn);
            }
            r.y[i] = std::move(row);
        });
    }

    // The independent source an analysis names as its input.
    Device* inputSource(const std::string& name, const std::string& where, const char* what) {
        auto it = byName.find(name);
        if (it != byName.end() && (dynamic_cast<VSource*>(it->second) || dynamic_cast<ISource*>(it->second))) return it->second;
        throw Error(where + ": " + what + " needs an independent source as its input, and '" + name + "' is not one");
    }

    // The integral of a spectral density that follows a power law between points.
    static double integrate(const std::vector<double>& f, const std::vector<double>& s) {
        double sum = 0;
        for (size_t i = 0; i + 1 < f.size(); i++) {
            double f1 = f[i], f2 = f[i + 1], s1 = s[i], s2 = s[i + 1];
            if (!(s1 > 0) || !(s2 > 0) || !(f2 > f1)) { sum += 0.5 * (s1 + s2) * (f2 - f1); continue; }
            double e = std::log(s2 / s1) / std::log(f2 / f1);
            sum += std::fabs(e + 1) < 1e-10 ? s1 * f1 * std::log(f2 / f1) : s1 * f1 * (std::pow(f2 / f1, e + 1) - 1) / (e + 1);
        }
        return sum;
    }

    // .noise: every device's noise at the output, by the adjoint circuit.
    // One transposed solve per frequency gives the transfer from every
    // noise source, and from the input source, to the output.
    void runNoise(const AnalysisSpec& a, const Probe& out, Result& r) {
        if (out.kind != 'v') throw Error(a.where + ": the output of .noise is a voltage, v(node) or v(a,b)");
        Device* src = inputSource(a.src1, a.where, ".noise");
        smallSignalPoint();
        if (bsim4 && bsim4->size()) warnings.push_back(a.where + ": BSIM4 transistors are noiseless in .noise for now");
        std::vector<double> f = frequencies(a);
        struct Src { NoiseSrc s; int dev; };
        std::vector<Src> srcs;
        std::vector<NoiseSrc> tmp;
        double kelvin = opt.temp + 273.15;
        for (size_t d = 0; d < devs.size(); d++) {
            tmp.clear();
            devs[d]->noise(kelvin, tmp);
            for (auto& x : tmp)
                if (x.a != x.b && (x.white > 0 || x.flicker > 0)) srcs.push_back({x, (int)d});
        }
        auto* vs = dynamic_cast<VSource*>(src);
        auto* is = dynamic_cast<ISource*>(src);
        std::vector<double> on(f.size()), in(f.size());
        // Each device's share at each frequency, unless that table would be huge.
        bool shares = (double)devs.size() * f.size() <= 2e7;
        std::vector<std::vector<double>> share(shares ? f.size() : 0);
        sweepAC(f, [&](size_t i, ACWork& w, SparseLU<cplx>& lu) {
            std::fill(w.rhs.begin(), w.rhs.end(), cplx());
            cplx* e = w.rhs.data() + 1;
            e[out.a] += 1; e[out.b] -= 1;
            w.rhs[0] = 0;
            lu.solveT(w.rhs.data() + 1, w.x.data() + 1);
            const cplx* y = w.x.data() + 1;
            if (shares) share[i].assign(devs.size(), 0);
            double sum = 0;
            for (const Src& k : srcs) {
                double psd = (k.s.white + k.s.flicker / f[i]) * std::norm(y[k.s.a] - y[k.s.b]);
                sum += psd;
                if (shares) share[i][k.dev] += psd;
            }
            double gain = std::abs(vs ? y[vs->br] : y[is->n] - y[is->p]);
            on[i] = sum;
            in[i] = gain > 0 ? sum / (gain * gain) : 0;
        });
        r.x = f;
        r.cols = {"onoise", "inoise"};
        r.y.resize(f.size());
        for (size_t i = 0; i < f.size(); i++) r.y[i] = {std::sqrt(on[i]), std::sqrt(in[i])};
        double ot = integrate(f, on), it = integrate(f, in);
        r.meas.push_back({"onoise_total", std::sqrt(ot)});
        r.meas.push_back({"inoise_total", std::sqrt(it)});
        if (shares && f.size() > 1 && ot > 0) {
            std::vector<std::pair<double, int>> tot;
            std::vector<double> col(f.size());
            for (size_t d = 0; d < devs.size(); d++) {
                bool any = false;
                for (size_t i = 0; i < f.size(); i++) { col[i] = share[i][d]; any = any || col[i] > 0; }
                if (any) tot.push_back({integrate(f, col), (int)d});
            }
            std::sort(tot.begin(), tot.end(), [&](auto& p, auto& q) { return p.first != q.first ? p.first > q.first : p.second < q.second; });
            r.text.push_back("largest contributions to the output noise (V rms, share of the power):");
            for (size_t k = 0; k < tot.size() && k < 10; k++) {
                char buf[200];
                std::snprintf(buf, sizeof buf, "  %-20s %.6e  %5.1f %%", devs[tot[k].second]->name.c_str(), std::sqrt(tot[k].first), 100 * tot[k].first / ot);
                r.text.push_back(buf);
            }
        }
    }

    // .tf: the small-signal DC gain from a source to an output, the
    // resistance the source sees and the resistance seen at the output.
    void runTF(const AnalysisSpec& a, const Probe& out, Result& r) {
        Device* src = inputSource(a.src1, a.where, ".tf");
        smallSignalPoint();
        auto* vs = dynamic_cast<VSource*>(src);
        auto* is = dynamic_cast<ISource*>(src);
        double tf = 0, rin = 0, rout = 0;
        auto ohms = [](cplx i) { return i.real() == 0 ? 1e20 : -1 / i.real(); };
        sweepAC({0.0}, [&](size_t, ACWork& w, SparseLU<cplx>& lu) {
            cplx* b = w.rhs.data() + 1;
            const cplx* xs = w.x.data() + 1;
            auto solve = [&] { w.rhs[0] = 0; lu.solve(w.rhs.data() + 1, w.x.data() + 1); };
            std::fill(w.rhs.begin(), w.rhs.end(), cplx());
            if (vs) b[vs->br] = 1;
            else { b[is->p] -= 1; b[is->n] += 1; }
            solve();
            tf = (out.kind == 'i' ? xs[out.a] : xs[out.a] - xs[out.b]).real();
            rin = vs ? ohms(xs[vs->br]) : (xs[is->n] - xs[is->p]).real();
            std::fill(w.rhs.begin(), w.rhs.end(), cplx());
            if (out.kind == 'i') {
                b[out.a] = 1;
                solve();
                rout = ohms(xs[out.a]);
            } else {
                b[out.a] += 1; b[out.b] -= 1;
                solve();
                rout = (xs[out.a] - xs[out.b]).real();
            }
        });
        r.cols = {"transfer_function", "input_resistance", "output_resistance"};
        r.x = {0};
        r.y = {{tf, rin, rout}};
    }

    // .sens: how much the DC value of an output moves per unit of every
    // resistance and every source value. One adjoint solve gives them all.
    void runSens(const AnalysisSpec& a, const Probe& out, Result& r) {
        smallSignalPoint();
        std::vector<double> row;
        sweepAC({0.0}, [&](size_t, ACWork& w, SparseLU<cplx>& lu) {
            std::fill(w.rhs.begin(), w.rhs.end(), cplx());
            cplx* e = w.rhs.data() + 1;
            if (out.kind == 'i') e[out.a] = 1;
            else { e[out.a] += 1; e[out.b] -= 1; }
            w.rhs[0] = 0;
            lu.solveT(w.rhs.data() + 1, w.x.data() + 1);
            const cplx* y = w.x.data() + 1;
            for (Device* d : devs) {
                double v;
                if (auto* res = dynamic_cast<Resistor*>(d)) v = res->g * res->g * (x[res->p] - x[res->n]) * (y[res->p] - y[res->n]).real();
                else if (auto* vsrc = dynamic_cast<VSource*>(d)) v = y[vsrc->br].real();
                else if (auto* isrc = dynamic_cast<ISource*>(d)) v = (y[isrc->n] - y[isrc->p]).real();
                else continue;
                r.cols.push_back(d->name);
                row.push_back(v);
            }
        });
        r.x = {0};
        r.y = {row};
    }

    void runTran(const AnalysisSpec& a, Result& r, const std::vector<Probe>& probes) {
        if (a.tstep <= 0 || a.tstop <= 0) throw Error(a.where + ": .tran needs tstep > 0 and tstop > 0");
        Ctx c = context();
        c.tran = true; c.time = 0; c.tstep = a.tstep; c.tstop = a.tstop;
        if (a.uic) {
            std::fill(xBuf.begin(), xBuf.end(), 0.0);
            for (auto& [nd, v] : ic) x[nd] = v;
            for (Device* d : devs) d->uicPre(x);
            c.a = 0;
            settle(c);
        } else {
            holdIC = !ic.empty();
            op(c);
            holdIC = false;
        }
        std::vector<Charge*> charges;
        for (Device* d : devs) {
            d->update(c);
            d->accept(c);
            if (a.uic) d->uicPost();
            d->charges(charges);
        }
        for (Charge* q : charges) q->start();
        if (bsim4) { bsim4->accept(); bsim4->start(); }
        std::vector<double> bps;
        for (Device* d : devs) d->breaks(a.tstep, a.tstop, bps);
        bps.push_back(a.tstop);
        std::sort(bps.begin(), bps.end());
        std::vector<double> bp;
        for (double t : bps)
            if (t > 0 && t <= a.tstop && (bp.empty() || t - bp.back() > 1e-12 * a.tstop)) bp.push_back(t);
        if (bp.empty() || bp.back() < a.tstop) bp.push_back(a.tstop);

        double hmax = a.tmax > 0 ? a.tmax : std::min(a.tstep, (a.tstop - a.tstart) / 50);
        double hmin = 1e-9 * hmax;
        // Start small, and never cross a corner in one unchecked step.
        double t = 0, h = std::min(std::min(hmax, a.tstep) / 10, 0.1 * bp[0]);
        std::vector<double> x0(x, x + n);
        double hist[3] = {h, h, h};   // the present step and the two before it
        bool be = true;               // backward Euler for the first step after a corner
        size_t ib = 0;
        if (a.tstart <= 0) record(r, probes, 0);
        while (ib < bp.size()) {
            double gap = bp[ib] - t;
            bool hit = false;
            if (gap <= h * (1 + 1e-9)) { h = gap; hit = true; }
            else if (gap < 2 * h) h = gap / 2;
            bool euler = be || opt.euler;
            c.a = euler ? 1 / h : 2 / h;
            c.b = euler ? 0 : 1;
            c.time = hit ? bp[ib] : t + h;
            bool ok = newton(c, opt.itl4);
            if (!ok) {
                std::copy(x0.begin(), x0.end(), x);
                h /= 8;
                be = true;
                r.rejected++;
                if (h < hmin) throw Error("transient: time step too small at t = " + fmt(t) + " (no convergence)");
                continue;
            }
            for (auto& d : pool<Capacitor>()) d.update(c);
            for (auto& d : pool<Inductor>()) d.update(c);
            // Local truncation error of every charge and flux.
            hist[0] = h;
            double room = 1e300;
            for (Charge* q : charges)
                room = std::min(room, q->maxStep(hist, euler ? 1 : 2, opt.reltol, opt.abstol, opt.trtol, opt.chgtol));
            if (bsim4) room = std::min(room, bsim4->maxStep(hist, euler ? 1 : 2, opt));
            double hnew = std::min(2 * h, euler ? room : std::sqrt(room));
            if (hnew < 0.9 * h && h > 4 * hmin) {
                std::copy(x0.begin(), x0.end(), x);
                h = std::max(hnew, h / 10);
                r.rejected++;
                continue;
            }
            eachPool([&](auto& p) { for (auto& d : p) d.accept(c); });
            if (bsim4) bsim4->accept();
            t = c.time;
            x0.assign(x, x + n);
            hist[2] = hist[1]; hist[1] = h;
            r.accepted++;
            if (t >= a.tstart) record(r, probes, t);
            if (hit) {
                ib++;
                be = true;
                if (ib < bp.size()) h = std::min(hnew, 0.1 * std::min(h, bp[ib] - t));
            } else {
                be = false;
                h = hnew;
            }
            h = std::min(h, hmax);
        }
    }

    static std::string fmt(double v) {
        char b[40];
        std::snprintf(b, sizeof b, "%.6e", v);
        return b;
    }
};

// ------------------------------------------------------------------ building

// Everything the control lines say, evaluated for one run.
struct Controls {
    std::vector<AnalysisSpec> analyses;
    std::map<std::string, std::vector<std::string>> prints;   // by analysis name
    std::vector<Card> meas;
    struct Four {
        std::string where;
        double freq = 0;
        std::vector<std::string> outs;
    };
    std::vector<Four> fours;
};

class Builder {
public:
    Builder(const Netlist& nl, Circuit& c, const std::map<std::string, double>& overrides)
        : nl_(nl), c_(c), overrides_(overrides) {}

    Controls build() {
        Env top;
        top.blk = &nl_.top;
        for (const Card& k : nl_.control)
            if (k.tok[0] == ".global")
                for (size_t i = 1; i < k.tok.size(); i++) globals_.insert(k.tok[i]);
        declare(top);
        for (auto& [k, v] : overrides_) top.sc.set(k, v);
        // A Monte Carlo run (.step monte N gives each run its number as the
        // parameter "monte"): gauss(), agauss(), unif() and aunif() draw.
        if (auto it = overrides_.find("monte"); it != overrides_.end()) {
            double seed = 1;
            for (const Card& k : nl_.control)
                if (k.tok[0] == ".option" || k.tok[0] == ".options")
                    for (auto& t : k.tok)
                        if (t.rfind("seed=", 0) == 0) seed = value(t.substr(5), nullptr);
            random_ = std::make_unique<Random>((uint64_t)seed, (uint64_t)it->second);
            top.sc.random = random_.get();
        }
        // Options first: the temperature shapes the models.
        for (const Card& k : nl_.control) {
            if (k.tok[0] == ".option" || k.tok[0] == ".options") options(k, top);
            if (k.tok[0] == ".temp" && k.tok.size() > 1) c_.opt.temp = value(k.tok[1], &top.sc);
        }
        elements(top);
        for (auto& [name, m] : allModels_)
            for (auto& [k, v] : m->p)
                if (!m->used.count(k) && k != "level")
                    c_.warnings.push_back("model " + name + ": parameter '" + k + "' is not used by mspice");
        c_.finalize();
        return controls(top);
    }

private:
    struct Env {
        Scope sc;
        std::map<std::string, std::unique_ptr<Model>> models;
        Env* parent = nullptr;
        const Block* blk = nullptr;
        std::string prefix;
        std::map<std::string, std::string> ports;
    };
    const Netlist& nl_;
    Circuit& c_;
    const std::map<std::string, double>& overrides_;
    std::unique_ptr<Random> random_;
    std::set<std::string> globals_;
    std::vector<std::pair<std::string, Model*>> allModels_;
    int depth_ = 0;

    static bool isKV(const std::string& t) { return t.find('=') != std::string::npos; }

    // .param and .model of this block: known before any element is read.
    void declare(Env& e) {
        for (const Card& k : e.blk->cards) {
            if (k.tok[0] == ".param" || k.tok[0] == ".params") {
                for (size_t i = 1; i < k.tok.size(); i++) {
                    auto eq = k.tok[i].find('=');
                    if (eq == std::string::npos) throw Error(k.where + ": .param needs name=value");
                    e.sc.set(k.tok[i].substr(0, eq), k.tok[i].substr(eq + 1));
                }
            }
        }
        for (const Card& k : e.blk->cards) {
            if (k.tok[0] != ".model") continue;
            if (k.tok.size() < 3) throw Error(k.where + ": .model needs a name and a type");
            auto m = std::make_unique<Model>();
            m->name = k.tok[1];
            m->type = k.tok[2];
            for (size_t i = 3; i < k.tok.size(); i++) {
                auto eq = k.tok[i].find('=');
                if (eq == std::string::npos) throw Error(k.where + ": model parameter '" + k.tok[i] + "' has no value");
                std::string key = k.tok[i].substr(0, eq), text = k.tok[i].substr(eq + 1);
                // version=4.8.3 is a name, not a number: keep its "4.8".
                if (key == "version") { m->p[key] = std::strtod(text.c_str(), nullptr); continue; }
                try { m->p[key] = value(text, &e.sc); }
                catch (const Error& x) { throw Error(k.where + ": " + x.what()); }
            }
            allModels_.push_back({e.prefix + m->name, m.get()});
            e.models[m->name] = std::move(m);
        }
    }

    const Model* model(Env& e, const std::string& name) {
        for (Env* s = &e; s; s = s->parent)
            if (auto it = s->models.find(name); it != s->models.end()) return it->second.get();
        return nullptr;
    }

    int node(Env& e, const std::string& t) {
        if (t == "0" || t == "gnd") return -1;
        if (auto it = e.ports.find(t); it != e.ports.end()) return c_.node(it->second);
        if (globals_.count(t)) return c_.node(t);
        return c_.node(e.prefix + t);
    }
    // The flat name of a node, for port maps.
    std::string nodeName(Env& e, const std::string& t) {
        if (t == "0" || t == "gnd") return "0";
        if (auto it = e.ports.find(t); it != e.ports.end()) return it->second;
        if (globals_.count(t)) return t;
        return e.prefix + t;
    }

    // E and G with vol=, value= or cur= instead of a gain are behavioral sources.
    static bool hasExpression(const Card& k) {
        for (size_t i = 3; i < k.tok.size(); i++)
            for (const char* w : {"vol=", "value=", "cur="})
                if (k.tok[i].rfind(w, 0) == 0) return true;
        return false;
    }

    // B elements, and E/G written with an expression. The expression is
    // taken from the line as written, because v(a,b) must stay together.
    void behavioral(const Card& k, Env& e, const std::string& name, char kind) {
        const std::string& s = k.text;
        // Skip the name and the two nodes.
        size_t i = 0;
        for (int w = 0; w < 3; w++) {
            while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
            while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        }
        size_t eq = s.find('=', i);
        if (eq == std::string::npos) throw Error("expected " + std::string(kind == 'b' ? "v= or i=" : kind == 'e' ? "vol= or value=" : "cur= or value=") + " and an expression");
        std::string key = s.substr(i, eq - i), text = s.substr(eq + 1);
        while (!key.empty() && std::isspace((unsigned char)key.back())) key.pop_back();
        auto trim = [](std::string& x) {
            while (!x.empty() && std::isspace((unsigned char)x.back())) x.pop_back();
            size_t b = 0;
            while (b < x.size() && std::isspace((unsigned char)x[b])) b++;
            x.erase(0, b);
        };
        trim(text);
        if (text.size() >= 2 && ((text.front() == '{' && text.back() == '}') || (text.front() == '\'' && text.back() == '\'')))
            text = text.substr(1, text.size() - 2);
        bool isV;
        if (kind == 'b' && (key == "v" || key == "i")) isV = key == "v";
        else if (kind == 'e' && (key == "vol" || key == "value")) isV = true;
        else if (kind == 'g' && (key == "cur" || key == "value")) isV = false;
        else throw Error("'" + key + "=' is not how this source takes an expression");
        BExpr ex(text, &e.sc, c_.opt.temp);
        auto* d = c_.add<Behav>(name);
        d->isV = isV;
        d->p = node(e, k.tok[1]); d->n = node(e, k.tok[2]);
        for (const BExpr::Var& v : ex.vars) {
            Behav::In in{};
            if (v.kind == 'v') {
                in.a = node(e, v.a);
                in.b = v.b.empty() ? -1 : node(e, v.b);
            } else {
                auto it = c_.byName.find(e.prefix + v.a);
                if (it == c_.byName.end()) throw Error("i(" + v.a + "): no element '" + v.a + "' defined before this line");
                if (!dynamic_cast<VSource*>(it->second) && !dynamic_cast<Inductor*>(it->second) &&
                    !dynamic_cast<VCVS*>(it->second) && !dynamic_cast<CCVS*>(it->second) && !dynamic_cast<Behav*>(it->second))
                    throw Error("i(" + v.a + "): only voltage sources, inductors, E and H sources carry a current to read");
                if (auto* bs = dynamic_cast<Behav*>(it->second); bs && !bs->isV)
                    throw Error("i(" + v.a + "): only voltage sources, inductors, E and H sources carry a current to read");
                in.cur = true;
                in.src = it->second;
            }
            d->in.push_back(in);
        }
        d->ex = std::move(ex);
    }

    static bool waveKeyword(const std::string& t) {
        return t == "dc" || t == "ac" || t == "pulse" || t == "sin" || t == "pwl" || t == "exp";
    }

    Wave wave(const Card& k, Env& e, size_t i) {
        Wave w;
        const auto& t = k.tok;
        while (i < t.size()) {
            if (t[i] == "dc") {
                if (i + 1 < t.size() && !waveKeyword(t[i + 1])) { w.dc = value(t[++i], &e.sc); w.hasDC = true; }
                i++;
            } else if (t[i] == "ac") {
                w.acMag = 1;
                if (i + 1 < t.size() && !waveKeyword(t[i + 1])) w.acMag = value(t[++i], &e.sc);
                if (i + 1 < t.size() && !waveKeyword(t[i + 1])) w.acPhase = value(t[++i], &e.sc);
                i++;
            } else if (t[i] == "pulse" || t[i] == "sin" || t[i] == "pwl" || t[i] == "exp") {
                w.kind = t[i] == "pulse" ? Wave::PULSE : t[i] == "sin" ? Wave::SIN : t[i] == "pwl" ? Wave::PWL : Wave::EXP;
                for (i++; i < t.size() && !waveKeyword(t[i]); i++) w.p.push_back(value(t[i], &e.sc));
                if (w.kind == Wave::PWL) {
                    if (w.p.size() < 2 || w.p.size() % 2) throw Error(k.where + ": PWL needs time/value pairs");
                    for (size_t j = 2; j < w.p.size(); j += 2)
                        if (w.p[j] <= w.p[j - 2]) throw Error(k.where + ": PWL times must increase");
                }
            } else if (t[i] == "sffm" || t[i] == "am") {
                throw Error(k.where + ": source function '" + t[i] + "' is not supported yet");
            } else {
                w.dc = value(t[i++], &e.sc);
                w.hasDC = true;
            }
        }
        return w;
    }

    // Split the tokens after the nodes into positional words and name=value.
    struct Args {
        std::vector<std::string> pos;
        std::map<std::string, std::string> kv;
        std::set<std::string> flags;
    };
    static Args args(const Card& k, size_t from) {
        Args a;
        for (size_t i = from; i < k.tok.size(); i++) {
            auto eq = k.tok[i].find('=');
            if (eq == std::string::npos) a.pos.push_back(k.tok[i]);
            else a.kv[k.tok[i].substr(0, eq)] = k.tok[i].substr(eq + 1);
        }
        return a;
    }
    static double kv(const Args& a, Env& e, const std::string& key, double dflt) {
        auto it = a.kv.find(key);
        return it == a.kv.end() ? dflt : value(it->second, &e.sc);
    }

    void need(const Card& k, size_t ntok, const char* form) {
        if (k.tok.size() < ntok) throw Error(k.where + ": expected " + form);
    }

    // A transistor of a BSIM4 model: every model and instance parameter goes
    // to the model code, which knows their names.
    void bsim4(const Card& k, Env& e, const Model& m, const std::string& name) {
#ifdef MSPICE_BSIM4
        if (!c_.bsim4) c_.bsim4 = makeBsim4();
        auto [it, fresh] = bsim4Models_.try_emplace(&m, "");
        if (fresh) {
            // Models of the same name in different subcircuits are told apart.
            it->second = m.name;
            for (int k = 2; !bsim4ModelNames_.insert(it->second).second; k++) it->second = m.name + "#" + std::to_string(k);
            Bsim4Set::Params p;
            for (auto& [key, v] : m.p) {
                m.used.insert(key);
                if (key != "level") p[key] = v;
            }
            for (auto& bad : c_.bsim4->addModel(it->second, m.type == "pmos", p))
                c_.warnings.push_back("model " + m.name + ": BSIM4 has no parameter '" + bad + "'");
        }
        if (c_.byName.count(name) || !bsim4Names_.insert(name).second)
            throw Error("two elements are named '" + name + "'");
        Args a = args(k, 6);
        Bsim4Set::Params p;
        for (auto& [key, text] : a.kv) p[key] = value(text, &e.sc);
        for (auto& flag : a.pos) p[flag] = 1;
        const auto& t = k.tok;
        int nodes[4] = {node(e, t[1]), node(e, t[2]), node(e, t[3]), node(e, t[4])};
        auto bad = c_.bsim4->addInstance(it->second, name, nodes, p);
        if (!bad.empty()) throw Error(k.where + ": BSIM4 has no instance parameter '" + bad[0] + "'");
#else
        (void)e; (void)name;
        throw Error(k.where + ": model '" + m.name + "' is BSIM4 (level 14/54), which this build of mspice leaves out");
#endif
    }
    std::map<const Model*, std::string> bsim4Models_;
    std::set<std::string> bsim4Names_, bsim4ModelNames_;

    Resistor* resistor(const std::string& name, int p, int n, double r, const Card& k) {
        if (!(r != 0) || !std::isfinite(r)) throw Error(k.where + ": resistance of '" + name + "' must not be zero");
        auto* d = c_.add<Resistor>(name);
        d->p = p; d->n = n; d->g = 1 / r;
        return d;
    }

    void elements(Env& e) {
        for (const Card& k : e.blk->cards) {
            if (k.tok[0][0] == '.') continue;
            try { element(k, e); }
            catch (const Error& x) {
                std::string w = x.what();
                throw Error(w.rfind(k.where, 0) == 0 || w.find(".cir:") != std::string::npos || w.find(": ") < w.find(' ')
                                ? w : k.where + ": " + w);
            }
        }
    }

    void element(const Card& k, Env& e) {
        const auto& t = k.tok;
        std::string name = e.prefix + t[0];
        double vt = kBoltzOverQ * (c_.opt.temp + 273.15);
        double tratio = (c_.opt.temp + 273.15) / (c_.opt.tnom + 273.15);
        switch (t[0][0]) {
        case 'r': {
            need(k, 4, "Rname n1 n2 value");
            Args a = args(k, 3);
            double r = a.kv.count("r") ? kv(a, e, "r", 0) : a.pos.empty() ? 0 : value(a.pos.back(), &e.sc);
            resistor(name, node(e, t[1]), node(e, t[2]), r, k);
            break;
        }
        case 'c': {
            need(k, 4, "Cname n1 n2 value [ic=v]");
            Args a = args(k, 3);
            auto* d = c_.add<Capacitor>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]);
            d->c = a.kv.count("c") ? kv(a, e, "c", 0) : a.pos.empty() ? 0 : value(a.pos.back(), &e.sc);
            if (a.kv.count("ic")) { d->hasIC = true; d->ic = kv(a, e, "ic", 0); }
            break;
        }
        case 'l': {
            need(k, 4, "Lname n1 n2 value [ic=i]");
            Args a = args(k, 3);
            auto* d = c_.add<Inductor>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]);
            d->l = a.kv.count("l") ? kv(a, e, "l", 0) : a.pos.empty() ? 0 : value(a.pos.back(), &e.sc);
            if (a.kv.count("ic")) { d->hasIC = true; d->ic = kv(a, e, "ic", 0); }
            break;
        }
        case 'k': {
            need(k, 4, "Kname L1 L2 coupling");
            auto find = [&](const std::string& n) {
                auto it = c_.byName.find(e.prefix + n);
                auto* l = it == c_.byName.end() ? nullptr : dynamic_cast<Inductor*>(it->second);
                if (!l) throw Error("'" + n + "' is not an inductor defined before this line");
                return l;
            };
            Inductor* l1 = find(t[1]);
            Inductor* l2 = find(t[2]);
            double kk = value(t[3], &e.sc);
            if (std::fabs(kk) > 1) throw Error("coupling must be between -1 and 1");
            double m = kk * std::sqrt(l1->l * l2->l);
            l1->mut.push_back({l2, m, 0});
            l2->mut.push_back({l1, m, 0});
            break;
        }
        case 'v': {
            need(k, 3, "Vname n+ n- [dc] value [ac mag [phase]] [pulse|sin|pwl|exp ...]");
            auto* d = c_.add<VSource>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]);
            d->w = wave(k, e, 3);
            break;
        }
        case 'i': {
            need(k, 3, "Iname n+ n- [dc] value [ac mag [phase]] [pulse|sin|pwl|exp ...]");
            auto* d = c_.add<ISource>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]);
            d->w = wave(k, e, 3);
            break;
        }
        case 'b': {
            // Bname n+ n- v=expression | i=expression
            need(k, 4, "Bname n+ n- v=expression or i=expression");
            behavioral(k, e, name, 'b');
            break;
        }
        case 's':
        case 'w': {
            bool byI = t[0][0] == 'w';
            need(k, byI ? 5 : 6, byI ? "Wname n+ n- Vcontrol model [on|off]" : "Sname n+ n- nc+ nc- model [on|off]");
            size_t mi = byI ? 4 : 5;
            const Model* m = model(e, t[mi]);
            const char* abrupt = byI ? "csw" : "sw";
            const char* soft = byI ? "iswitch" : "vswitch";
            if (!m || (m->type != abrupt && m->type != soft))
                throw Error("'" + t[mi] + "' is not a " + abrupt + " or " + soft + " model");
            Device* ctl = nullptr;
            if (byI) {
                auto it = c_.byName.find(e.prefix + t[3]);
                if (it == c_.byName.end() || !dynamic_cast<VSource*>(it->second))
                    throw Error("'" + t[3] + "' is not a voltage source defined before this line");
                ctl = it->second;
            }
            auto* d = c_.add<Switch>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]);
            if (byI) d->ctl = ctl;
            else { d->cp = node(e, t[3]); d->cn = node(e, t[4]); }
            d->ron = m->get("ron", 1); d->roff = m->get("roff", m->type == soft ? 1e6 : 1e12);
            if (!(d->ron > 0) || !(d->roff > 0)) throw Error("a switch needs ron and roff above zero");
            if (m->type == soft) {
                d->smooth = true;
                d->von = m->get(byI ? "ion" : "von", byI ? 1e-3 : 1);
                d->voff = m->get(byI ? "ioff" : "voff", 0);
                if (d->von == d->voff) throw Error("a smooth switch needs two different control levels");
            } else {
                d->vt = m->get(byI ? "it" : "vt", 0);
                d->vh = std::fabs(m->get(byI ? "ih" : "vh", 0));
            }
            for (size_t i = mi + 1; i < t.size(); i++) {
                if (t[i] == "on") d->on = d->onAcc = true;
                else if (t[i] != "off") throw Error("a switch takes on or off after its model, not '" + t[i] + "'");
            }
            break;
        }
        case 'e': {
            if (hasExpression(k)) { behavioral(k, e, name, 'e'); break; }
            need(k, 6, "Ename n+ n- nc+ nc- gain");
            auto* d = c_.add<VCVS>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]); d->cp = node(e, t[3]); d->cn = node(e, t[4]);
            d->gain = value(t[5], &e.sc);
            break;
        }
        case 'g': {
            if (hasExpression(k)) { behavioral(k, e, name, 'g'); break; }
            need(k, 6, "Gname n+ n- nc+ nc- transconductance");
            auto* d = c_.add<VCCS>(name);
            d->p = node(e, t[1]); d->n = node(e, t[2]); d->cp = node(e, t[3]); d->cn = node(e, t[4]);
            d->gm = value(t[5], &e.sc);
            break;
        }
        case 'f':
        case 'h': {
            need(k, 5, "Fname|Hname n+ n- Vcontrol gain");
            auto it = c_.byName.find(e.prefix + t[3]);
            if (it == c_.byName.end() || !dynamic_cast<VSource*>(it->second))
                throw Error("'" + t[3] + "' is not a voltage source defined before this line");
            if (t[0][0] == 'f') {
                auto* d = c_.add<CCCS>(name);
                d->p = node(e, t[1]); d->n = node(e, t[2]); d->ctl = it->second; d->gain = value(t[4], &e.sc);
            } else {
                auto* d = c_.add<CCVS>(name);
                d->p = node(e, t[1]); d->n = node(e, t[2]); d->ctl = it->second; d->r = value(t[4], &e.sc);
            }
            break;
        }
        case 'd': {
            need(k, 4, "Dname anode cathode model [area]");
            const Model* m = model(e, t[3]);
            if (!m || m->type != "d") throw Error("'" + t[3] + "' is not a diode model");
            Args a = args(k, 4);
            double area = kv(a, e, "area", 1);
            for (auto& p : a.pos) {
                if (p == "off") continue;
                area = value(p, &e.sc);
            }
            auto* d = c_.add<Diode>(name);
            d->off = std::count(a.pos.begin(), a.pos.end(), "off") > 0;
            int an = node(e, t[1]);
            d->k = node(e, t[2]);
            double rs = m->get("rs", 0);
            if (rs > 0) {
                int in = c_.node(name + "#anode");
                resistor(name + "#rs", an, in, rs / area, k);
                an = in;
            }
            d->a = an;
            d->nf = m->get("n", 1);
            double is = m->get("is", 1e-14), eg = m->get("eg", 1.11), xti = m->get("xti", 3);
            is *= std::pow(tratio, xti / d->nf) * std::exp((tratio - 1) * eg / (d->nf * vt));
            d->isat = is * area;
            d->cj0 = (m->has("cjo") ? m->get("cjo", 0) : m->get("cj0", 0)) * area;
            d->vj = m->get("vj", 1); d->mj = m->get("m", 0.5); d->tt = m->get("tt", 0);
            d->fc = m->get("fc", 0.5);
            d->kf = m->get("kf", 0); d->af = m->get("af", 1);
            // Breakdown: the knee is placed so that the current at -BV is IBV.
            double bv = m->get("bv", 0), ibv = m->get("ibv", 1e-3) * area, nvt = d->nf * vt;
            if (bv > 0) d->bv = ibv < d->isat * bv / vt ? bv : bv - nvt * std::log(1 + ibv / d->isat);
            d->vcrit = d->nf * vt * std::log(d->nf * vt / (M_SQRT2 * d->isat));
            break;
        }
        case 'm': {
            need(k, 6, "Mname drain gate source bulk model [w= l= ad= as= pd= ps= m=]");
            const Model* m = model(e, t[5]);
            if (!m || (m->type != "nmos" && m->type != "pmos")) throw Error("'" + t[5] + "' is not an nmos or pmos model");
            if (int level = (int)m->get("level", 1); level == 14 || level == 54) {
                bsim4(k, e, *m, name);
                break;
            }
            if (m->get("level", 1) != 1)
                throw Error("model '" + t[5] + "' is MOSFET level " + std::to_string((int)m->get("level", 1)) +
                            "; mspice has level 1 and BSIM4 (level 14 or 54)");
            Args a = args(k, 6);
            auto* d = c_.add<Mosfet>(name);
            d->type = m->type == "nmos" ? 1 : -1;
            double w = kv(a, e, "w", 1e-4), l = kv(a, e, "l", 1e-4), mult = kv(a, e, "m", 1);
            double ad = kv(a, e, "ad", 0), as = kv(a, e, "as", 0), pd = kv(a, e, "pd", 0), ps = kv(a, e, "ps", 0);
            double ld = m->get("ld", 0), leff = l - 2 * ld;
            if (leff <= 0) throw Error("effective channel length is not positive");
            double tox = m->get("tox", 0), coxPerArea = tox > 0 ? kEpsOx / tox : 0;
            double kp = m->has("kp") ? m->get("kp", 2e-5) : tox > 0 ? m->get("uo", 600) * 1e-4 * coxPerArea : 2e-5;
            if (m->has("kp")) m->has("uo");
            kp *= std::pow(tratio, -1.5);
            d->vto = m->has("vto") ? m->get("vto", 0) : m->get("vt0", 0);
            d->beta = kp * w / leff * mult;
            d->gamma = m->get("gamma", 0); d->phi = m->get("phi", 0.6); d->lambda = m->get("lambda", 0);
            d->isat = m->get("is", 1e-14) * mult; d->pb = m->get("pb", 0.8); d->fc = m->get("fc", 0.5);
            d->mj = m->get("mj", 0.5); d->mjsw = m->get("mjsw", 0.5);
            double cj = m->get("cj", 0), cjsw = m->get("cjsw", 0);
            d->cbd0 = (m->has("cbd") ? m->get("cbd", 0) : cj * ad) * mult;
            d->cbs0 = (m->has("cbs") ? m->get("cbs", 0) : cj * as) * mult;
            d->cbdsw = cjsw * pd * mult; d->cbssw = cjsw * ps * mult;
            d->cox = coxPerArea * w * leff * mult;
            d->kf = m->get("kf", 0); d->af = m->get("af", 1); d->flickDen = coxPerArea * coxPerArea * w * leff * mult;
            d->covgs = m->get("cgso", 0) * w * mult; d->covgd = m->get("cgdo", 0) * w * mult;
            d->covgb = m->get("cgbo", 0) * leff * mult;
            d->vcrit = vt * std::log(vt / (M_SQRT2 * d->isat));
            int nd = node(e, t[1]), ng = node(e, t[2]), ns = node(e, t[3]), nb = node(e, t[4]);
            double rd = m->get("rd", 0), rs = m->get("rs", 0);
            if (rd > 0) { int in = c_.node(name + "#drain"); resistor(name + "#rd", nd, in, rd / mult, k); nd = in; }
            if (rs > 0) { int in = c_.node(name + "#source"); resistor(name + "#rs", ns, in, rs / mult, k); ns = in; }
            d->node[Mosfet::D] = nd; d->node[Mosfet::G] = ng; d->node[Mosfet::S] = ns; d->node[Mosfet::B] = nb;
            break;
        }
        case 'q': {
            need(k, 5, "Qname collector base emitter [substrate] model [area]");
            size_t mi = 4;
            const Model* m = model(e, t[4]);
            if (!m && t.size() > 5) { m = model(e, t[5]); mi = 5; }
            if (!m || (m->type != "npn" && m->type != "pnp")) throw Error("no npn or pnp model named on this line");
            Args a = args(k, mi + 1);
            double area = kv(a, e, "area", 1);
            for (auto& p : a.pos)
                if (p != "off") area = value(p, &e.sc);
            auto* d = c_.add<Bjt>(name);
            d->off = std::count(a.pos.begin(), a.pos.end(), "off") > 0;
            d->type = m->type == "npn" ? 1 : -1;
            double eg = m->get("eg", 1.11), xti = m->get("xti", 3);
            double tf = std::pow(tratio, xti) * std::exp((tratio - 1) * eg / vt);
            d->isat = m->get("is", 1e-16) * area * tf;
            d->bf = m->get("bf", 100); d->br = m->get("br", 1); d->nf = m->get("nf", 1); d->nr = m->get("nr", 1);
            d->ise = m->get("ise", 0) * area; d->ne = m->get("ne", 1.5);
            d->isc = m->get("isc", 0) * area; d->nc = m->get("nc", 2);
            auto inv = [&](const char* key) { double v = m->get(key, 0); return v > 0 ? 1 / v : 0; };
            d->ivaf = inv("vaf"); d->ivar = inv("var");
            d->iikf = inv("ikf") / area; d->iikr = inv("ikr") / area;
            d->cje = m->get("cje", 0) * area; d->vje = m->get("vje", 0.75); d->mje = m->get("mje", 0.33);
            d->cjc = m->get("cjc", 0) * area; d->vjc = m->get("vjc", 0.75); d->mjc = m->get("mjc", 0.33);
            d->tf = m->get("tf", 0); d->tr = m->get("tr", 0); d->fc = m->get("fc", 0.5);
            d->kf = m->get("kf", 0); d->af = m->get("af", 1);
            d->vcrit = vt * std::log(vt / (M_SQRT2 * d->isat));
            int nc = node(e, t[1]), nb = node(e, t[2]), ne = node(e, t[3]);
            auto series = [&](const char* key, const char* tag, int& nd) {
                double r = m->get(key, 0);
                if (r <= 0) return;
                int in = c_.node(name + "#" + tag);
                resistor(name + "#" + key, nd, in, r / area, k);
                nd = in;
            };
            series("rc", "collector", nc); series("rb", "base", nb); series("re", "emitter", ne);
            d->node[Bjt::C] = nc; d->node[Bjt::B] = nb; d->node[Bjt::E] = ne;
            break;
        }
        case 'x': {
            size_t last = t.size();
            while (last > 1 && (isKV(t[last - 1]) || t[last - 1] == "params:")) last--;
            if (last < 2) throw Error("expected Xname nodes... subcircuit");
            const std::string& sub = t[last - 1];
            const Block* b = e.blk->findSubckt(sub);
            if (!b) throw Error("no .subckt named '" + sub + "'");
            if (last - 2 != b->ports.size())
                throw Error("'" + sub + "' has " + std::to_string(b->ports.size()) + " ports, " +
                            std::to_string(last - 2) + " given");
            if (++depth_ > 64) throw Error("subcircuits nested too deep (is '" + sub + "' inside itself?)");
            Env child;
            child.parent = &e;
            child.blk = b;
            child.prefix = name + ".";
            child.sc.parent = &e.sc;
            for (size_t i = 0; i < b->ports.size(); i++) child.ports[b->ports[i]] = nodeName(e, t[1 + i]);
            for (auto& [pn, pe] : b->defaults) child.sc.set(pn, pe);
            // Instance parameters are evaluated where the instance is.
            for (size_t i = last; i < t.size(); i++) {
                auto eq = t[i].find('=');
                if (eq == std::string::npos) continue;
                child.sc.set(t[i].substr(0, eq), value(t[i].substr(eq + 1), &e.sc));
            }
            declare(child);
            elements(child);
            depth_--;
            break;
        }
        default:
            throw Error("element '" + t[0] + "' is of a kind mspice does not have yet");
        }
    }

    void options(const Card& k, Env& e) {
        Options& o = c_.opt;
        for (size_t i = 1; i < k.tok.size(); i++) {
            auto eq = k.tok[i].find('=');
            std::string key = k.tok[i].substr(0, eq), v = eq == std::string::npos ? "" : k.tok[i].substr(eq + 1);
            auto num = [&] { return value(v, &e.sc); };
            if (key == "reltol") o.reltol = num();
            else if (key == "abstol") o.abstol = num();
            else if (key == "vntol") o.vntol = num();
            else if (key == "gmin") o.gmin = num();
            else if (key == "seed") {}   // read when a Monte Carlo run starts
            else if (key == "trtol") o.trtol = num();
            else if (key == "chgtol") o.chgtol = num();
            else if (key == "itl1") o.itl1 = (int)num();
            else if (key == "itl4") o.itl4 = (int)num();
            else if (key == "temp") o.temp = num();
            else if (key == "tnom") o.tnom = num();
            else if (key == "gshunt") o.gshunt = num();
            else if (key == "bypass") o.bypass = v.empty() || num() != 0;
            else if (key == "nobypass") o.bypass = false;
            else if (key == "parmin") o.parMin = std::max(1, (int)num());
            else if (key == "method") {
                if (v == "euler" || v == "be") o.euler = true;
                else if (v != "trap" && v != "trapezoidal") c_.warnings.push_back(k.where + ": method=" + v + " is not available; using trap");
            } else c_.warnings.push_back(k.where + ": option '" + key + "' is ignored");
        }
    }

    Controls controls(Env& top) {
        Controls out;
        Scope* sc = &top.sc;
        for (const Card& k : nl_.control) {
            const auto& t = k.tok;
            const std::string& d = t[0];
            auto num = [&](size_t i) {
                if (i >= t.size()) throw Error(k.where + ": " + d + " has too few values");
                try { return value(t[i], sc); }
                catch (const Error& x) { throw Error(k.where + ": " + x.what()); }
            };
            if (d == ".op") {
                AnalysisSpec a;
                a.where = k.where;
                out.analyses.push_back(a);
            } else if (d == ".dc") {
                AnalysisSpec a;
                a.type = AnalysisSpec::DC; a.where = k.where;
                if (t.size() < 5) throw Error(k.where + ": expected .dc source start stop step");
                a.src1 = t[1]; a.a1 = num(2); a.b1 = num(3); a.s1 = num(4);
                if (t.size() >= 9) { a.src2 = t[5]; a.a2 = num(6); a.b2 = num(7); a.s2 = num(8); }
                out.analyses.push_back(a);
            } else if (d == ".ac") {
                AnalysisSpec a;
                a.type = AnalysisSpec::AC; a.where = k.where;
                if (t.size() < 5 || (t[1] != "dec" && t[1] != "oct" && t[1] != "lin"))
                    throw Error(k.where + ": expected .ac dec|oct|lin points fstart fstop");
                a.acMode = t[1]; a.npts = (int)num(2); a.f1 = num(3); a.f2 = num(4);
                out.analyses.push_back(a);
            } else if (d == ".tran") {
                AnalysisSpec a;
                a.type = AnalysisSpec::TRAN; a.where = k.where;
                std::vector<double> v;
                for (size_t i = 1; i < t.size(); i++) {
                    if (t[i] == "uic") a.uic = true;
                    else v.push_back(num(i));
                }
                if (v.size() < 2) throw Error(k.where + ": expected .tran tstep tstop [tstart [tmax]] [uic]");
                a.tstep = v[0]; a.tstop = v[1];
                if (v.size() > 2) a.tstart = v[2];
                if (v.size() > 3) a.tmax = v[3];
                out.analyses.push_back(a);
            } else if (d == ".noise") {
                // .noise v(out[,ref]) source dec|oct|lin points fstart fstop
                AnalysisSpec a;
                a.type = AnalysisSpec::NOISE; a.where = k.where;
                if (t.size() < 7 || (t[3] != "dec" && t[3] != "oct" && t[3] != "lin"))
                    throw Error(k.where + ": expected .noise v(out) source dec|oct|lin points fstart fstop");
                a.out = t[1]; a.src1 = t[2];
                a.acMode = t[3]; a.npts = (int)num(4); a.f1 = num(5); a.f2 = num(6);
                out.analyses.push_back(a);
            } else if (d == ".tf") {
                AnalysisSpec a;
                a.type = AnalysisSpec::TF; a.where = k.where;
                if (t.size() != 3) throw Error(k.where + ": expected .tf output source");
                a.out = t[1]; a.src1 = t[2];
                out.analyses.push_back(a);
            } else if (d == ".sens") {
                if (t.size() < 2) throw Error(k.where + ": expected .sens output ...");
                for (size_t i = 1; i < t.size(); i++) {
                    AnalysisSpec a;
                    a.type = AnalysisSpec::SENS; a.where = k.where;
                    a.out = t[i];
                    out.analyses.push_back(a);
                }
            } else if (d == ".four" || d == ".fourier") {
                if (t.size() < 3) throw Error(k.where + ": expected .four frequency outputs...");
                Controls::Four f;
                f.where = k.where;
                f.freq = num(1);
                if (!(f.freq > 0)) throw Error(k.where + ": .four needs a frequency above zero");
                f.outs.assign(t.begin() + 2, t.end());
                out.fours.push_back(f);
            } else if (d == ".print" || d == ".plot" || d == ".probe" || d == ".save") {
                if (d != ".print") continue;
                if (t.size() < 2) throw Error(k.where + ": expected .print op|dc|ac|tran outputs");
                for (size_t i = 2; i < t.size(); i++) out.prints[t[1]].push_back(t[i]);
            } else if (d == ".meas" || d == ".measure") {
                out.meas.push_back(k);
            } else if (d == ".ic" || d == ".nodeset") {
                for (size_t i = 1; i < t.size(); i++) {
                    auto eq = t[i].find('=');
                    if (eq == std::string::npos || t[i].rfind("v(", 0) != 0 || t[i][eq - 1] != ')')
                        throw Error(k.where + ": expected " + d + " v(node)=value");
                    std::string nd = t[i].substr(2, eq - 3);
                    auto it = c_.nodeIndex.find(nd);
                    if (it == c_.nodeIndex.end()) throw Error(k.where + ": no node '" + nd + "'");
                    (d == ".ic" ? c_.ic : c_.nodeset)[it->second] = value(t[i].substr(eq + 1), sc);
                }
            } else if (d == ".option" || d == ".options" || d == ".temp" || d == ".global" || d == ".step" ||
                       d == ".title" || d == ".width") {
            } else {
                c_.warnings.push_back(k.where + ": " + d + " is not supported and was ignored");
            }
        }
        return out;
    }
};

// ------------------------------------------------------------------ probes

// v(a) v(a,b) i(vsrc), and for AC the suffixes m, db, p, r, i: vdb(out).
inline Probe parseProbe(const Circuit& c, const std::string& e, const std::string& where) {
    Probe p;
    p.label = e;
    size_t open = e.find('(');
    if (open == std::string::npos || e.back() != ')' || open == 0 || (e[0] != 'v' && e[0] != 'i'))
        throw Error(where + ": '" + e + "' is not an output mspice knows; use v(node), v(a,b) or i(source)");
    p.kind = e[0];
    std::string suf = e.substr(1, open - 1), arg = e.substr(open + 1, e.size() - open - 2);
    if (suf == "") p.fn = 0;
    else if (suf == "m") p.fn = 'm';
    else if (suf == "db") p.fn = 'd';
    else if (suf == "p") p.fn = 'p';
    else if (suf == "r") p.fn = 'r';
    else if (suf == "i") p.fn = 'i';
    else throw Error(where + ": '" + e + "': unknown suffix '" + suf + "'");
    auto nodeOf = [&](const std::string& nm) {
        if (nm == "0" || nm == "gnd") return -1;
        auto it = c.nodeIndex.find(nm);
        if (it == c.nodeIndex.end()) throw Error(where + ": '" + e + "': no node '" + nm + "'");
        return it->second;
    };
    if (p.kind == 'v') {
        auto comma = arg.find(',');
        p.a = nodeOf(arg.substr(0, comma));
        if (comma != std::string::npos) p.b = nodeOf(arg.substr(comma + 1));
    } else {
        auto it = c.byName.find(arg);
        if (it == c.byName.end()) throw Error(where + ": '" + e + "': no element '" + arg + "'");
        p.a = it->second->branch();
        if (p.a < 0)
            throw Error(where + ": '" + e + "': only voltage sources, inductors, E and H sources carry a current to print");
    }
    return p;
}

// ------------------------------------------------------------------ .measure

struct MeasProbes {
    // Outputs a .measure line needs recorded.
    static std::vector<std::string> of(const Card& k) {
        std::vector<std::string> out;
        for (size_t i = 3; i < k.tok.size(); i++) {
            std::string t = k.tok[i];
            // "v(out)=0.5" and "v(a)=v(b)": both sides may be outputs.
            int depth = 0;
            size_t eq = std::string::npos;
            for (size_t j = 0; j < t.size(); j++) {
                if (t[j] == '(') depth++;
                if (t[j] == ')') depth--;
                if (t[j] == '=' && !depth) { eq = j; break; }
            }
            for (const std::string& part : eq == std::string::npos
                     ? std::vector<std::string>{t} : std::vector<std::string>{t.substr(0, eq), t.substr(eq + 1)})
                if (part.size() > 3 && (part[0] == 'v' || part[0] == 'i') && part.back() == ')' && part.find('(') != std::string::npos)
                    out.push_back(part);
        }
        return out;
    }
};

class Measure {
public:
    Measure(const Result& r, Scope* sc) : r_(r), sc_(sc) {}

    // Returns false (with why) when the measurement cannot be made.
    bool eval(const Card& k, std::string& name, double& out, std::string& why) {
        t_ = &k.tok;
        where_ = k.where;
        name = k.tok.size() > 2 ? k.tok[2] : "?";
        i_ = 3;
        try {
            if (i_ >= t_->size()) throw Error("nothing to measure");
            const std::string& op = (*t_)[i_];
            if (op == "trig") {
                i_++;
                double a = event();
                if (i_ >= t_->size() || (*t_)[i_] != "targ") throw Error("expected targ after trig");
                i_++;
                double b = event();
                out = b - a;
            } else if (op == "when") {
                i_++;
                out = when();
            } else if (op == "find") {
                int c = column((*t_)[i_ + 1]);
                i_ += 2;
                if (i_ >= t_->size()) throw Error("find needs at= or when");
                double xv;
                if ((*t_)[i_] == "when") { i_++; xv = when(); }
                else if ((*t_)[i_].rfind("at=", 0) == 0) xv = value((*t_)[i_].substr(3), sc_);
                else throw Error("find needs at= or when");
                out = r_.at(c, xv);
            } else if (op == "max" || op == "min" || op == "pp" || op == "avg" || op == "rms" || op == "integ" ||
                       op == "integral") {
                out = stat(op);
            } else {
                throw Error("'" + op + "' is not a measurement mspice has (trig/targ, when, find, max, min, pp, avg, rms, integ)");
            }
        } catch (const Error& e) {
            why = e.what();
            return false;
        }
        if (!std::isfinite(out)) { why = "the condition never happens"; return false; }
        return true;
    }

private:
    const Result& r_;
    Scope* sc_;
    const std::vector<std::string>* t_ = nullptr;
    std::string where_;
    size_t i_ = 0;

    int column(const std::string& e) {
        int c = r_.col(e);
        if (c < 0) throw Error("'" + e + "' is not an output of this analysis");
        return c;
    }
    struct Edge { char dir = 'c'; int n = 1; double td = 0; };
    // rise=2 fall=last cross=1 td=1n, in any order.
    Edge edge() {
        Edge g;
        for (; i_ < t_->size(); i_++) {
            const std::string& t = (*t_)[i_];
            auto eq = t.find('=');
            if (eq == std::string::npos) break;
            std::string key = t.substr(0, eq), v = t.substr(eq + 1);
            if (key == "rise" || key == "fall" || key == "cross") {
                g.dir = key[0];
                g.n = v == "last" ? -1 : (int)value(v, sc_);
            } else if (key == "td") g.td = value(v, sc_);
            else break;
        }
        return g;
    }
    // The x where column c minus (constant rhs or column c2) crosses zero.
    double crossing(int c, int c2, double rhs, const Edge& g) {
        double found = NAN;
        int count = 0;
        for (size_t k = 1; k < r_.x.size(); k++) {
            if (r_.x[k - 1] < g.td) continue;
            double d0 = r_.y[k - 1][c] - (c2 >= 0 ? r_.y[k - 1][c2] : rhs);
            double d1 = r_.y[k][c] - (c2 >= 0 ? r_.y[k][c2] : rhs);
            bool rise = d0 < 0 && d1 >= 0, fall = d0 > 0 && d1 <= 0;
            if (!((g.dir == 'r' && rise) || (g.dir == 'f' && fall) || (g.dir == 'c' && (rise || fall)))) continue;
            double xc = r_.x[k - 1] + (r_.x[k] - r_.x[k - 1]) * (-d0) / (d1 - d0);
            found = xc;
            if (++count == g.n) return xc;
        }
        return g.n == -1 ? found : NAN;
    }
    // "v(a)=0.5 rise=1" or "v(a)=v(b)"
    double when() {
        if (i_ >= t_->size()) throw Error("when needs output=value");
        const std::string t = (*t_)[i_++];
        int depth = 0;
        size_t eq = std::string::npos;
        for (size_t j = 0; j < t.size(); j++) {
            if (t[j] == '(') depth++;
            if (t[j] == ')') depth--;
            if (t[j] == '=' && !depth) { eq = j; break; }
        }
        if (eq == std::string::npos) throw Error("when needs output=value");
        int c = column(t.substr(0, eq));
        std::string rhs = t.substr(eq + 1);
        int c2 = r_.col(rhs);
        double v = c2 >= 0 ? 0 : value(rhs, sc_);
        Edge g = edge();
        return crossing(c, c2, v, g);
    }
    // trig/targ: "v(in) val=0.5 rise=1" or "at=1n"
    double event() {
        if (i_ >= t_->size()) throw Error("trig/targ needs an output or at=");
        if ((*t_)[i_].rfind("at=", 0) == 0) return value((*t_)[i_++].substr(3), sc_);
        int c = column((*t_)[i_++]);
        if (i_ >= t_->size() || (*t_)[i_].rfind("val=", 0) != 0) throw Error("trig/targ needs val=");
        double v = value((*t_)[i_++].substr(4), sc_);
        Edge g = edge();
        return crossing(c, -1, v, g);
    }
    double stat(const std::string& op) {
        int c = column((*t_)[i_ + 1]);
        i_ += 2;
        if (r_.x.empty()) throw Error("no data");
        double from = r_.x.front(), to = r_.x.back();
        for (; i_ < t_->size(); i_++) {
            const std::string& t = (*t_)[i_];
            if (t.rfind("from=", 0) == 0) from = value(t.substr(5), sc_);
            else if (t.rfind("to=", 0) == 0) to = value(t.substr(3), sc_);
        }
        from = std::max(from, r_.x.front());
        to = std::min(to, r_.x.back());
        if (!(to >= from)) throw Error("from= is after to=");
        // The samples inside [from, to], with interpolated end points.
        std::vector<double> xs{from}, ys{r_.at(c, from)};
        for (size_t k = 0; k < r_.x.size(); k++)
            if (r_.x[k] > from && r_.x[k] < to) { xs.push_back(r_.x[k]); ys.push_back(r_.y[k][c]); }
        if (to > from) { xs.push_back(to); ys.push_back(r_.at(c, to)); }
        double lo = ys[0], hi = ys[0], area = 0, sq = 0;
        for (size_t k = 0; k < xs.size(); k++) {
            lo = std::min(lo, ys[k]); hi = std::max(hi, ys[k]);
            if (!k) continue;
            double dx = xs[k] - xs[k - 1];
            area += dx * (ys[k] + ys[k - 1]) / 2;
            sq += dx * (ys[k] * ys[k] + ys[k] * ys[k - 1] + ys[k - 1] * ys[k - 1]) / 3;
        }
        double span = to - from;
        if (op == "max") return hi;
        if (op == "min") return lo;
        if (op == "pp") return hi - lo;
        if (op == "integ" || op == "integral") return area;
        if (op == "avg") return span > 0 ? area / span : ys[0];
        return span > 0 ? std::sqrt(sq / span) : std::fabs(ys[0]);
    }
};

// ------------------------------------------------------------------ one run

struct RunOutput {
    std::vector<Result> results;
    std::vector<std::string> warnings;
    size_t nodes = 0, unknowns = 0, devices = 0;
};

// Build the circuit with these parameter overrides and run every analysis.
inline std::string fmtNum(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6e", v);
    return b;
}

// .four: the DC value and the first nine harmonics of an output over the
// last period of a transient, and the total harmonic distortion. The output
// is read on a uniform grid by linear interpolation. A harmonic is
// magnitude * sin(k w t + phase), with t the simulation time.
inline void fourier(Result& r, const std::string& label, const Controls::Four& f, std::vector<std::string>& warnings) {
    int col = r.col(label);
    double T = 1 / f.freq;
    std::string thd = "thd_";
    for (char ch : label.substr(label.find('(') + 1))
        if (ch != ')') thd += std::isalnum((unsigned char)ch) ? ch : '_';
    if (label[0] == 'i') thd.insert(4, "i_");
    if (col < 0 || r.x.size() < 2 || r.x.back() - r.x.front() < T * (1 - 1e-9)) {
        warnings.push_back(f.where + ": .four of " + label + " needs a transient of at least one period");
        r.meas.push_back({thd, NAN});
        return;
    }
    const int N = 4096, H = 9;
    double t0 = r.x.back() - T, dc = 0, ca[H + 1] = {}, sa[H + 1] = {};
    size_t k = 1;
    for (int i = 0; i < N; i++) {
        double t = t0 + T * i / N;
        while (k + 1 < r.x.size() && r.x[k] < t) k++;
        double x0 = r.x[k - 1], x1 = r.x[k];
        double y = x1 == x0 ? r.y[k][col] : r.y[k - 1][col] + (r.y[k][col] - r.y[k - 1][col]) * (t - x0) / (x1 - x0);
        dc += y;
        for (int h = 1; h <= H; h++) {
            double ph = 2 * M_PI * h * f.freq * t;
            ca[h] += y * std::cos(ph);
            sa[h] += y * std::sin(ph);
        }
    }
    dc /= N;
    double mag[H + 1] = {}, sum = 0;
    char buf[200];
    std::snprintf(buf, sizeof buf, "fourier analysis of %s, fundamental %s Hz", label.c_str(), fmtNum(f.freq).c_str());
    r.text.push_back(buf);
    r.text.push_back("  harmonic  frequency      magnitude      phase(deg)     normalized");
    std::snprintf(buf, sizeof buf, "  %-9d %-14s %-14s", 0, fmtNum(0).c_str(), fmtNum(dc).c_str());
    r.text.push_back(buf);
    for (int h = 1; h <= H; h++) {
        double a = 2 * ca[h] / N, b = 2 * sa[h] / N;
        mag[h] = std::hypot(a, b);
        if (h > 1) sum += mag[h] * mag[h];
        // Phase of a harmonic that is only rounding noise means nothing: print 0.
        double phase = mag[h] > 1e-12 * std::max(mag[1], 1e-300) ? std::atan2(a, b) * 180 / M_PI : 0;
        std::snprintf(buf, sizeof buf, "  %-9d %-14s %-14s %-14s %-14s", h, fmtNum(h * f.freq).c_str(), fmtNum(mag[h]).c_str(),
                      fmtNum(phase).c_str(), fmtNum(mag[1] > 0 ? mag[h] / mag[1] : 0).c_str());
        r.text.push_back(buf);
    }
    // The distortion in percent.
    r.meas.push_back({thd, mag[1] > 0 ? 100 * std::sqrt(sum) / mag[1] : NAN});
}

inline RunOutput runNetlist(const Netlist& nl, const std::map<std::string, double>& overrides, Pool* pool) {
    RunOutput out;
    Circuit c;
    Builder b(nl, c, overrides);
    Controls ctl = b.build();
    c.pool_ = pool;
    out.nodes = c.numNodes; out.unknowns = c.n; out.devices = c.devs.size() + (c.bsim4 ? c.bsim4->size() : 0);
    // A scope for values on .measure lines.
    Scope sc;
    for (const Card& k : nl.top.cards)
        if (k.tok[0] == ".param" || k.tok[0] == ".params")
            for (size_t i = 1; i < k.tok.size(); i++) {
                auto eq = k.tok[i].find('=');
                sc.set(k.tok[i].substr(0, eq), k.tok[i].substr(eq + 1));
            }
    for (auto& [k, v] : overrides) sc.set(k, v);
    if (ctl.analyses.empty()) c.warnings.push_back("the netlist asks for no analysis (.op, .dc, .ac, .tran, .noise, .tf or .sens)");
    if (!ctl.fours.empty() && std::none_of(ctl.analyses.begin(), ctl.analyses.end(), [](auto& a) { return a.type == AnalysisSpec::TRAN; }))
        c.warnings.push_back(ctl.fours[0].where + ": .four needs a .tran analysis");
    for (const AnalysisSpec& a : ctl.analyses) {
        Result r;
        r.kind = a.name();
        r.tstep = a.tstep;
        if (a.type == AnalysisSpec::NOISE || a.type == AnalysisSpec::TF || a.type == AnalysisSpec::SENS) {
            // These make their own columns from one output.
            Probe o = parseProbe(c, a.out, a.where);
            if (o.fn) throw Error(a.where + ": '" + a.out + "': use v(node), v(a,b) or i(source) here");
            size_t it0 = c.iterations;
            if (a.type == AnalysisSpec::NOISE) {
                r.xname = "frequency";
                c.runNoise(a, o, r);
                if (ctl.prints.count("noise")) r.printed = r.cols;
                // .measure noise works on the columns onoise and inoise.
                for (const Card& k : ctl.meas) {
                    if (k.tok.size() < 4 || k.tok[1] != "noise") continue;
                    Measure m(r, &sc);
                    std::string name, why;
                    double v = NAN;
                    if (!m.eval(k, name, v, why)) {
                        c.warnings.push_back(k.where + ": .measure " + name + " failed: " + why);
                        v = NAN;
                    }
                    r.meas.push_back({name, v});
                }
            } else if (a.type == AnalysisSpec::TF) {
                c.runTF(a, o, r);
                r.text.push_back("from " + a.src1 + " to " + a.out);
            } else {
                c.runSens(a, o, r);
                r.text.push_back("of " + a.out + ", per unit of each element's value");
            }
            r.iterations = c.iterations - it0;
            out.results.push_back(std::move(r));
            continue;
        }
        bool ac = a.type == AnalysisSpec::AC;
        std::vector<Probe> probes;
        std::unordered_set<std::string> have;
        auto addProbe = [&](const std::string& e, const std::string& where) {
            if (have.insert(e).second) probes.push_back(parseProbe(c, e, where));
        };
        auto pr = ctl.prints.find(r.kind);
        if (pr != ctl.prints.end()) {
            for (auto& e : pr->second) { addProbe(e, a.where); r.printed.push_back(e); }
        } else {
            // Nothing asked for: every node and every source current.
            for (int i = 0; i < c.numNodes; i++) {
                if (c.names[i].find('#') != std::string::npos) continue;
                if (ac) { addProbe("vm(" + c.names[i] + ")", a.where); addProbe("vp(" + c.names[i] + ")", a.where); }
                else addProbe("v(" + c.names[i] + ")", a.where);
            }
            if (!ac)
                for (auto& d : c.devs)
                    if (dynamic_cast<VSource*>(d)) addProbe("i(" + d->name + ")", a.where);
        }
        if (a.type == AnalysisSpec::TRAN)
            for (auto& f : ctl.fours)
                for (auto& e : f.outs) addProbe(e, f.where);
        std::vector<const Card*> mine;
        for (const Card& k : ctl.meas) {
            if (k.tok.size() < 4) throw Error(k.where + ": expected .measure analysis name ...");
            if (k.tok[1] != r.kind) continue;
            mine.push_back(&k);
            for (auto& e : MeasProbes::of(k)) addProbe(e, k.where);
        }
        for (auto& p : probes) r.cols.push_back(p.label);
        size_t it0 = c.iterations;
        switch (a.type) {
        case AnalysisSpec::OP: r.xname = ""; c.runOp(r, probes); break;
        case AnalysisSpec::DC:
            r.xname = a.src1;
            c.runDC(a, r, probes);
            if (!a.src2.empty()) r.cols.push_back(a.src2);
            break;
        case AnalysisSpec::AC: r.xname = "frequency"; c.runAC(a, r, probes); break;
        case AnalysisSpec::TRAN: r.xname = "time"; c.runTran(a, r, probes); break;
        }
        r.iterations = c.iterations - it0;
        if (a.type == AnalysisSpec::TRAN)
            for (auto& f : ctl.fours)
                for (auto& e : f.outs) fourier(r, e, f, c.warnings);
        for (const Card* k : mine) {
            Measure m(r, &sc);
            std::string name, why;
            double v = NAN;
            if (!m.eval(*k, name, v, why)) {
                c.warnings.push_back(k->where + ": .measure " + name + " failed: " + why);
                v = NAN;
            }
            r.meas.push_back({name, v});
        }
        out.results.push_back(std::move(r));
    }
    out.warnings = c.warnings;
    return out;
}

// .step: the parameter sets to run, in order. One empty set when there is none.
struct StepPlan {
    std::vector<std::string> names;
    std::vector<std::vector<double>> sets;
};

inline StepPlan stepPlan(const Netlist& nl, const std::map<std::string, double>& defines) {
    Scope sc;
    for (const Card& k : nl.top.cards)
        if (k.tok[0] == ".param" || k.tok[0] == ".params")
            for (size_t i = 1; i < k.tok.size(); i++) {
                auto eq = k.tok[i].find('=');
                sc.set(k.tok[i].substr(0, eq), k.tok[i].substr(eq + 1));
            }
    for (auto& [k, v] : defines) sc.set(k, v);
    StepPlan plan;
    plan.sets.push_back({});
    for (const Card& k : nl.control) {
        if (k.tok[0] != ".step") continue;
        std::vector<std::string> t(k.tok.begin() + 1, k.tok.end());
        std::string mode = "lin";
        if (!t.empty() && (t[0] == "lin" || t[0] == "dec" || t[0] == "oct")) { mode = t[0]; t.erase(t.begin()); }
        if (!t.empty() && t[0] == "param") t.erase(t.begin());
        // .step monte N (or monte=N): N Monte Carlo runs, numbered from 1.
        if (!t.empty() && t[0].rfind("monte=", 0) == 0) t = {"monte", t[0].substr(6)};
        bool monte = t.size() == 2 && t[0] == "monte";
        if (t.size() < 3 && !monte) throw Error(k.where + ": expected .step param name start stop step, .step param name list v1 v2 ..., or .step monte N");
        std::string name = t[0];
        std::vector<double> vals;
        auto num = [&](size_t i) {
            try { return value(t[i], &sc); }
            catch (const Error& x) { throw Error(k.where + ": " + x.what()); }
        };
        if (monte) {
            double cnt = num(1);
            if (!(cnt >= 1) || cnt > 1e7) throw Error(k.where + ": .step monte needs a number of runs");
            for (int i = 1; i <= (int)cnt; i++) vals.push_back(i);
        } else if (t[1] == "list") {
            for (size_t i = 2; i < t.size(); i++) vals.push_back(num(i));
        } else {
            if (t.size() < 4) throw Error(k.where + ": .step needs start, stop and step");
            double a = num(1), b = num(2), s = num(3);
            if (mode == "lin") {
                if (s == 0 || (b - a) / s < 0) throw Error(k.where + ": .step increment has the wrong sign or is zero");
                size_t cnt = (size_t)std::floor((b - a) / s + 1e-9) + 1;
                for (size_t i = 0; i < cnt; i++) vals.push_back(a + i * s);
            } else {
                double per = mode == "oct" ? 2 : 10;
                if (a <= 0 || b < a || s < 1) throw Error(k.where + ": .step dec|oct needs 0 < start <= stop and points per interval");
                int cnt = (int)std::floor(std::log(b / a) / std::log(per) * s + 1e-9);
                for (int i = 0; i <= cnt; i++) vals.push_back(a * std::pow(per, i / s));
            }
        }
        if (vals.empty()) throw Error(k.where + ": .step has no values");
        // Later .step lines vary fastest.
        std::vector<std::vector<double>> next;
        for (auto& set : plan.sets)
            for (double v : vals) {
                auto s2 = set;
                s2.push_back(v);
                next.push_back(std::move(s2));
            }
        plan.names.push_back(name);
        plan.sets = std::move(next);
    }
    return plan;
}

}  // namespace spice
