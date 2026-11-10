// spice_dev.hpp - the device models of mspice.
//
// Every device follows the same three steps per Newton iteration:
//   eval()  read the present solution, limit junction voltages, and work out
//           currents, conductances and charges. It touches only the device's
//           own state, so devices are evaluated in parallel.
//   load()  add the linearized device to the matrix and the right-hand side.
//           Always serial and in netlist order, which keeps the sums (and so
//           the results) identical for any number of threads.
//   accept() the time step is accepted: charges and fluxes become history.
// loadAC() stamps the small-signal model around the last evaluated point.
#pragma once
#include "spice_lu.hpp"
#include "spice_netlist.hpp"

namespace spice {

using cplx = std::complex<double>;
constexpr double kBoltzOverQ = 8.617333262e-5;   // V/K
constexpr double kEpsOx = 3.9 * 8.854187817e-12; // F/m

struct Options {
    double reltol = 1e-3, abstol = 1e-12, vntol = 1e-6, gmin = 1e-12, trtol = 7;
    double temp = 27, tnom = 27, gshunt = 0;
    int itl1 = 100, itl4 = 10;
    bool euler = false;   // method=euler: backward Euler only
    bool bypass = true;
    int parMin = 4096;    // evaluate a device type in parallel from this many instances
};

struct Ctx {
    const double* x = nullptr;   // x[-1] is ground and reads 0
    double a = 0, b = 0;         // i = a*(q - q0) - b*i0; a == 0 means DC
    double time = 0, tstep = 0, tstop = 0;
    double gmin = 1e-12, srcScale = 1, vt = 0.025865;
    bool tran = false;           // sources follow their waveform
    bool initJct = false;        // first iteration of an operating point
    bool noLimit = false;        // evaluate exactly at x
    double reltol = 1e-3, abstol = 1e-12;
    bool bypass = true;          // latent devices keep their last linearization
};

struct Setup {
    Pattern& pat;
    std::vector<std::string>& names;
    int newUnknown(const std::string& name) {
        names.push_back(name);
        return (int)names.size() - 1;
    }
    int slot(int r, int c) { return pat.slot(r, c); }
};

// Slots for every (terminal, terminal) pair of an N-terminal device.
template <int N>
struct Slots {
    int s[N][N] = {};
    void init(Setup& S, const int* node) {
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) s[i][j] = S.slot(node[i], node[j]);
    }
    // A conductance between terminals a and b.
    template <class T, class U> void g(T* v, int a, int b, U y) const {
        v[s[a][a]] += y; v[s[a][b]] -= y; v[s[b][a]] -= y; v[s[b][b]] += y;
    }
    // A current y*(v[c] - v[d]) flowing from a to b.
    template <class T, class U> void vccs(T* v, int a, int b, int c, int d, U y) const {
        v[s[a][c]] += y; v[s[a][d]] -= y; v[s[b][c]] -= y; v[s[b][d]] += y;
    }
};

// A charge (or flux) and its integration: trapezoidal or backward Euler.
struct Charge {
    double q0 = 0, i0 = 0, q = 0, i = 0;
    double q1 = 0, q2 = 0;   // older accepted values, for the error estimate
    // Returns the equivalent conductance a*cap and sets the current i.
    double integ(const Ctx& c, double qnow, double cap) {
        q = qnow;
        if (c.a == 0) { i = 0; return 0; }
        i = c.a * (q - q0) - c.b * i0;
        return c.a * cap;
    }
    void accept() { q2 = q1; q1 = q0; q0 = q; i0 = i; }
    void start() { q1 = q2 = q0; i0 = 0; }   // the state before the first step

    // SPICE's truncation error test. Returns the largest step this charge
    // allows (order 1, backward Euler) or its square (order 2, trapezoidal).
    // h[0] is the step just taken, h[1] and h[2] the ones before.
    double maxStep(const double* h, int order, double reltol, double abstol, double trtol) const {
        if (q == q0 && q0 == q1 && q1 == q2) return 1e300;   // at rest
        double diff[4] = {q, q0, q1, q2}, del[3] = {h[0], h[1], h[2]};
        for (int j = order;; ) {
            for (int k = 0; k <= j; k++) diff[k] = (diff[k] - diff[k + 1]) / del[k];
            if (--j < 0) break;
            for (int k = 0; k <= j; k++) del[k] = del[k + 1] + h[k];
        }
        double tol = std::max(abstol + reltol * std::max(std::fabs(i), std::fabs(i0)),
                              reltol * std::max({std::fabs(q), std::fabs(q0), 1e-14}) / h[0]);
        double lte = std::max(abstol, (order == 1 ? 0.5 : 1.0 / 12) * std::fabs(diff[0]));
        return trtol * tol / lte;
    }
};

struct Device {
    std::string name;
    // Set by eval(): this iteration cannot be the last one, because a
    // junction voltage was limited or the device current is not yet what the
    // previous linearization predicted.
    bool limited = false;
    virtual ~Device() = default;
    virtual void branches(Setup&) {}                 // allocate branch currents
    virtual void bind(Setup&) = 0;                   // allocate matrix slots
    virtual void eval(const Ctx&) {}
    virtual void load(const Ctx&, double* val, double* rhs) = 0;
    virtual void loadAC(double w, cplx* val, cplx* rhs) const = 0;
    virtual void accept(const Ctx&) {}
    virtual void update(const Ctx&) {}               // after Newton: states of linear elements
    virtual void charges(std::vector<Charge*>&) {}   // what the step control watches
    virtual void uicPre(double*) {}                  // .tran uic: set branch currents
    virtual void uicPost() {}                        // .tran uic: element IC= values
    virtual void breaks(double, double, std::vector<double>&) const {}
    virtual bool nonlinear() const { return false; }
    virtual int branch() const { return -1; }
};

// ------------------------------------------------------------------ sources

struct Wave {
    enum Kind { DC, PULSE, SIN, PWL, EXP } kind = DC;
    std::vector<double> p;
    double dc = 0, acMag = 0, acPhase = 0;
    bool hasDC = false;

    double par(size_t i, double dflt) const { return i < p.size() ? p[i] : dflt; }

    double at(double t, double tstep, double tstop) const {
        switch (kind) {
        case DC: return dc;
        case PULSE: {
            double v1 = par(0, 0), v2 = par(1, 0), td = par(2, 0), tr = par(3, 0), tf = par(4, 0);
            double pw = par(5, 0), per = par(6, 0);
            if (tr <= 0) tr = tstep;
            if (tf <= 0) tf = tstep;
            if (pw <= 0) pw = tstop;
            if (per <= 0) per = tstop;
            if (t < td) return v1;
            double tt = std::fmod(t - td, per);
            if (tt < tr) return v1 + (v2 - v1) * tt / tr;
            if (tt < tr + pw) return v2;
            if (tt < tr + pw + tf) return v2 + (v1 - v2) * (tt - tr - pw) / tf;
            return v1;
        }
        case SIN: {
            double vo = par(0, 0), va = par(1, 0), f = par(2, 0), td = par(3, 0), th = par(4, 0);
            double ph = par(5, 0) * M_PI / 180;
            if (f <= 0) f = 1 / tstop;
            if (t < td) return vo + va * std::sin(ph);
            return vo + va * std::exp(-(t - td) * th) * std::sin(2 * M_PI * f * (t - td) + ph);
        }
        case PWL: {
            if (p.size() < 2) return 0;
            if (t <= p[0]) return p[1];
            for (size_t i = 2; i + 1 < p.size(); i += 2)
                if (t <= p[i]) return p[i - 1] + (p[i + 1] - p[i - 1]) * (t - p[i - 2]) / (p[i] - p[i - 2]);
            return p[p.size() - 1];
        }
        case EXP: {
            double v1 = par(0, 0), v2 = par(1, 0), td1 = par(2, 0), tau1 = par(3, tstep);
            double td2 = par(4, td1 + tstep), tau2 = par(5, tstep);
            double v = v1;
            if (t > td1) v += (v2 - v1) * (1 - std::exp(-(t - td1) / tau1));
            if (t > td2) v += (v1 - v2) * (1 - std::exp(-(t - td2) / tau2));
            return v;
        }
        }
        return dc;
    }
    // The value for .op, .dc and the operating point of .ac.
    double dcValue(double tstep, double tstop) const { return hasDC || kind == DC ? dc : at(0, tstep, tstop); }
    double value(const Ctx& c) const {
        return c.srcScale * (c.tran ? at(c.time, c.tstep, c.tstop) : dcValue(c.tstep, c.tstop));
    }
    // Corners of the waveform: the integrator must land on them.
    void breaks(double tstep, double tstop, std::vector<double>& out) const {
        if (kind == PULSE) {
            double td = par(2, 0), tr = par(3, 0), tf = par(4, 0), pw = par(5, 0), per = par(6, 0);
            if (tr <= 0) tr = tstep;
            if (tf <= 0) tf = tstep;
            if (pw <= 0) pw = tstop;
            if (per <= 0) per = tstop;
            for (double t0 = td; t0 < tstop; t0 += per)
                for (double d : {0.0, tr, tr + pw, tr + pw + tf})
                    if (t0 + d < tstop) out.push_back(t0 + d);
        } else if (kind == PWL) {
            for (size_t i = 0; i < p.size(); i += 2) out.push_back(p[i]);
        } else if (kind == SIN) {
            out.push_back(par(3, 0));
        } else if (kind == EXP) {
            out.push_back(par(2, 0));
            out.push_back(par(4, par(2, 0) + tstep));
        }
    }
};

struct VSource final : Device {
    int p, n, br = -1;
    Wave w;
    int pb, nb, bp, bn;
    void branches(Setup& S) override { br = S.newUnknown(name + "#branch"); }
    void bind(Setup& S) override {
        pb = S.slot(p, br); nb = S.slot(n, br); bp = S.slot(br, p); bn = S.slot(br, n);
    }
    void load(const Ctx& c, double* val, double* rhs) override {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1;
        rhs[br] += w.value(c);
    }
    void rhsOnly(const Ctx& c, double* rhs) { rhs[br] += w.value(c); }
    void loadAC(double, cplx* val, cplx* rhs) const override {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1;
        rhs[br] += std::polar(w.acMag, w.acPhase * M_PI / 180);
    }
    void breaks(double tstep, double tstop, std::vector<double>& out) const override { w.breaks(tstep, tstop, out); }
    int branch() const override { return br; }
};

struct ISource final : Device {
    int p, n;
    Wave w;
    void bind(Setup&) override {}
    void load(const Ctx& c, double*, double* rhs) override {
        double i = w.value(c);
        rhs[p] -= i; rhs[n] += i;
    }
    void rhsOnly(const Ctx& c, double* rhs) { load(c, nullptr, rhs); }
    void loadAC(double, cplx*, cplx* rhs) const override {
        cplx i = std::polar(w.acMag, w.acPhase * M_PI / 180);
        rhs[p] -= i; rhs[n] += i;
    }
    void breaks(double tstep, double tstop, std::vector<double>& out) const override { w.breaks(tstep, tstop, out); }
};

// E: v(p,n) = gain * v(cp,cn)
struct VCVS final : Device {
    int p, n, cp, cn, br = -1;
    double gain = 0;
    int pb, nb, bp, bn, bcp, bcn;
    void branches(Setup& S) override { br = S.newUnknown(name + "#branch"); }
    void bind(Setup& S) override {
        pb = S.slot(p, br); nb = S.slot(n, br); bp = S.slot(br, p); bn = S.slot(br, n);
        bcp = S.slot(br, cp); bcn = S.slot(br, cn);
    }
    template <class T> void stamp(T* val) const {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1; val[bcp] -= gain; val[bcn] += gain;
    }
    void load(const Ctx&, double* val, double*) override { stamp(val); }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
    int branch() const override { return br; }
};

// G: i(p -> n) = gm * v(cp,cn)
struct VCCS final : Device {
    int p, n, cp, cn;
    double gm = 0;
    int s[4];
    void bind(Setup& S) override {
        s[0] = S.slot(p, cp); s[1] = S.slot(p, cn); s[2] = S.slot(n, cp); s[3] = S.slot(n, cn);
    }
    template <class T> void stamp(T* val) const { val[s[0]] += gm; val[s[1]] -= gm; val[s[2]] -= gm; val[s[3]] += gm; }
    void load(const Ctx&, double* val, double*) override { stamp(val); }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
};

// F: i(p -> n) = gain * i(Vctl)
struct CCCS final : Device {
    int p, n;
    double gain = 0;
    Device* ctl = nullptr;
    int sp, sn;
    void bind(Setup& S) override { sp = S.slot(p, ctl->branch()); sn = S.slot(n, ctl->branch()); }
    template <class T> void stamp(T* val) const { val[sp] += gain; val[sn] -= gain; }
    void load(const Ctx&, double* val, double*) override { stamp(val); }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
};

// H: v(p,n) = r * i(Vctl)
struct CCVS final : Device {
    int p, n, br = -1;
    double r = 0;
    Device* ctl = nullptr;
    int pb, nb, bp, bn, bc;
    void branches(Setup& S) override { br = S.newUnknown(name + "#branch"); }
    void bind(Setup& S) override {
        pb = S.slot(p, br); nb = S.slot(n, br); bp = S.slot(br, p); bn = S.slot(br, n);
        bc = S.slot(br, ctl->branch());
    }
    template <class T> void stamp(T* val) const {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1; val[bc] -= r;
    }
    void load(const Ctx&, double* val, double*) override { stamp(val); }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
    int branch() const override { return br; }
};

// ------------------------------------------------------------------ R, C, L

struct Resistor final : Device {
    int p, n;
    double g = 0;
    Slots<2> sl;
    void bind(Setup& S) override { int nd[2] = {p, n}; sl.init(S, nd); }
    void load(const Ctx&, double* val, double*) override { sl.g(val, 0, 1, g); }
    void loadAC(double, cplx* val, cplx*) const override { sl.g(val, 0, 1, g); }
};

struct Capacitor final : Device {
    int p, n;
    double c = 0, ic = 0;
    bool hasIC = false;
    Charge ch;
    Slots<2> sl;
    void bind(Setup& S) override { int nd[2] = {p, n}; sl.init(S, nd); }
    void load(const Ctx& k, double* val, double* rhs) override {
        double v = k.x[p] - k.x[n];
        double geq = ch.integ(k, c * v, c);
        if (geq == 0) return;
        sl.g(val, 0, 1, geq);
        double ieq = ch.i - geq * v;
        rhs[p] -= ieq; rhs[n] += ieq;
    }
    // With the matrix already factored, a linear capacitor only moves the right-hand side.
    void rhsOnly(const Ctx& k, double* rhs) {
        if (k.a == 0) return;
        double ieq = -k.a * ch.q0 - k.b * ch.i0;
        rhs[p] -= ieq; rhs[n] += ieq;
    }
    void loadAC(double w, cplx* val, cplx*) const override { sl.g(val, 0, 1, cplx(0, w * c)); }
    // A linear circuit takes one Newton iteration, so load() saw the old
    // voltage: take the charge from the solution itself.
    void update(const Ctx& k) override { ch.integ(k, c * (k.x[p] - k.x[n]), c); }
    void accept(const Ctx&) override { ch.accept(); }
    void charges(std::vector<Charge*>& out) override { out.push_back(&ch); }
    void uicPost() override { if (hasIC) ch.q0 = c * ic; }
};

struct Inductor final : Device {
    int p, n, br = -1;
    double l = 0, ic = 0;
    bool hasIC = false;
    struct Mutual { Inductor* other; double m; int slot; };
    std::vector<Mutual> mut;
    Charge fl;   // flux, and the voltage across the inductor as its "current"
    int pb, nb, bp, bn, bb;
    void branches(Setup& S) override { br = S.newUnknown(name + "#branch"); }
    void bind(Setup& S) override {
        pb = S.slot(p, br); nb = S.slot(n, br); bp = S.slot(br, p); bn = S.slot(br, n); bb = S.slot(br, br);
        for (auto& m : mut) m.slot = S.slot(br, m.other->br);
    }
    void load(const Ctx& c, double* val, double* rhs) override {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1;
        if (c.a == 0) return;   // DC: a short
        val[bb] -= c.a * l;
        for (auto& m : mut) val[m.slot] -= c.a * m.m;
        rhs[br] += -c.a * fl.q0 - c.b * fl.i0;
    }
    void rhsOnly(const Ctx& c, double* rhs) {
        if (c.a != 0) rhs[br] += -c.a * fl.q0 - c.b * fl.i0;
    }
    void loadAC(double w, cplx* val, cplx*) const override {
        val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1;
        val[bb] -= cplx(0, w * l);
        for (auto& m : mut) val[m.slot] -= cplx(0, w * m.m);
    }
    void update(const Ctx& c) override {
        double phi = l * c.x[br];
        for (auto& m : mut) phi += m.m * c.x[m.other->br];
        fl.integ(c, phi, l);
    }
    void accept(const Ctx&) override { fl.accept(); }
    void charges(std::vector<Charge*>& out) override { out.push_back(&fl); }
    void uicPre(double* x) override { if (hasIC) x[br] = ic; }
    int branch() const override { return br; }
};

// ------------------------------------------------------------------ junctions

// Is the current the device now carries the one its last linearization
// predicted? SPICE's device convergence test.
struct Tol { double reltol = 1e-3, abstol = 1e-12; };
inline bool agrees(double predicted, double actual, const Tol& t) {
    return std::fabs(predicted - actual) <= t.reltol * std::max(std::fabs(predicted), std::fabs(actual)) + t.abstol;
}

// SPICE's junction voltage limiting: keep a Newton step from running up the
// exponential.
inline double pnjlim(double vnew, double vold, double vt, double vcrit, bool& lim) {
    if (vnew > vcrit && std::fabs(vnew - vold) > 2 * vt) {
        if (vold > 0) {
            double arg = 1 + (vnew - vold) / vt;
            vnew = arg > 0 ? vold + vt * std::log(arg) : vcrit;
        } else {
            vnew = vt * std::log(vnew / vt);
        }
        lim = true;
    }
    return vnew;
}

inline double fetlim(double vnew, double vold, double vto) {
    double vtsthi = std::fabs(2 * (vold - vto)) + 2, vtstlo = vtsthi / 2 + 2, vtox = vto + 3.5;
    double delv = vnew - vold;
    if (vold >= vto) {
        if (vold >= vtox) {
            if (delv <= 0) {
                if (vnew >= vtox) { if (-delv > vtstlo) vnew = vold - vtstlo; }
                else vnew = std::max(vnew, vto + 2);
            } else if (delv >= vtsthi) vnew = vold + vtsthi;
        } else {
            vnew = delv <= 0 ? std::max(vnew, vto - 0.5) : std::min(vnew, vto + 4);
        }
    } else if (delv <= 0) {
        if (-delv > vtsthi) vnew = vold - vtsthi;
    } else {
        double vtemp = vto + 0.5;
        if (vnew <= vtemp) { if (delv > vtstlo) vnew = vold + vtstlo; }
        else vnew = vtemp;
    }
    return vnew;
}

inline double limvds(double vnew, double vold) {
    if (vold >= 3.5) {
        if (vnew > vold) vnew = std::min(vnew, 3 * vold + 2);
        else if (vnew < 3.5) vnew = std::max(vnew, 2.0);
    } else {
        vnew = vnew > vold ? std::min(vnew, 4.0) : std::max(vnew, -0.5);
    }
    return vnew;
}

// Depletion charge and capacitance of a junction, linear past fc*vj.
inline void depletion(double v, double cj0, double vj, double m, double fc, double& q, double& c) {
    if (cj0 == 0) return;
    m = std::min(m, 0.9);
    if (v < fc * vj) {
        double arg = 1 - v / vj, sarg = m == 0.5 ? 1 / std::sqrt(arg) : std::exp(-m * std::log(arg));
        q += vj * cj0 * (1 - arg * sarg) / (1 - m);
        c += cj0 * sarg;
    } else {
        double f1 = vj * (1 - std::pow(1 - fc, 1 - m)) / (1 - m), f2 = std::pow(1 - fc, 1 + m), f3 = 1 - fc * (1 + m);
        q += cj0 * (f1 + (f3 * (v - fc * vj) + (m / (2 * vj)) * (v * v - fc * fc * vj * vj)) / f2);
        c += cj0 * (f3 + m * v / vj) / f2;
    }
}

// May a device keep its last linearization? Only when the voltage has barely
// moved from the point it was made at: far inside the convergence tolerance.
inline bool same(double v, double at) { return std::fabs(v - at) <= 1e-9 + 1e-6 * std::max(std::fabs(v), std::fabs(at)); }

struct Diode final : Device {
    int a, k;
    // Model, already scaled by area and temperature.
    double isat = 1e-14, nf = 1, cj0 = 0, vj = 1, mj = 0.5, tt = 0, fc = 0.5;
    double bv = 0;   // already moved so that I(bv) = ibv
    bool off = false;
    double vcrit = 0, vdOld = 0;
    double gd = 0, cap = 0, gtot = 0, ieq = 0, id = 0, itot = 0, qd = 0;
    double vdE = 0;   // the voltage of the last evaluation (vdOld: of the last linearization)
    bool valid = false;
    Charge ch;
    Slots<2> sl;
    bool nonlinear() const override { return true; }
    void bind(Setup& S) override { int nd[2] = {a, k}; sl.init(S, nd); }

    void eval(const Ctx& c) override {
        double nvt = nf * c.vt, vd = c.x[a] - c.x[k];
        bool lim = false, check = false;
        double predicted = 0;
        if (c.bypass && valid && !c.initJct && !c.noLimit && same(vd, vdOld)) {
            // Latent: the exponentials are still right. Below, the device
            // is its own linear model around vdOld.
        } else {
            predicted = itot + gtot * (vd - vdE);
            if (c.initJct) { vd = off ? 0 : vcrit; lim = true; }
            else if (!c.noLimit) {
                if (bv > 0 && vd < std::min(0.0, -bv + 10 * nvt)) {
                    double t = pnjlim(-(vd + bv), -(vdOld + bv), nvt, vcrit, lim);
                    vd = -(t + bv);
                } else vd = pnjlim(vd, vdOld, nvt, vcrit, lim);
            }
            check = !c.noLimit && !lim;
            vdOld = vd;
            double ij, gj;
            if (vd >= -3 * nvt) {
                double ev = std::exp(vd / nvt);
                ij = isat * (ev - 1); gj = isat * ev / nvt;
            } else if (bv <= 0 || vd >= -bv) {
                double arg = 3 * nvt / (vd * M_E);
                arg = arg * arg * arg;
                ij = -isat * (1 + arg); gj = isat * 3 * arg / vd;
            } else {
                double ev = std::exp(-(bv + vd) / nvt);
                ij = -isat * ev; gj = isat * ev / nvt;
            }
            id = ij + c.gmin * vd;
            gd = gj + c.gmin;
            qd = tt * ij;
            cap = tt * gj;
            depletion(vd, cj0, vj, mj, fc, qd, cap);
            valid = true;
        }
        limited = lim;
        double dv = vd - vdOld;
        double geq = ch.integ(c, qd + cap * dv, cap);
        gtot = gd + geq;
        itot = id + gd * dv + ch.i;
        ieq = itot - gtot * vd;
        vdE = vd;
        if (check && !agrees(predicted, itot, {c.reltol, c.abstol})) limited = true;
    }
    void load(const Ctx&, double* val, double* rhs) override {
        sl.g(val, 0, 1, gtot);
        rhs[a] -= ieq; rhs[k] += ieq;
    }
    void loadAC(double w, cplx* val, cplx*) const override { sl.g(val, 0, 1, cplx(gd, w * cap)); }
    void accept(const Ctx&) override { ch.accept(); }
    void charges(std::vector<Charge*>& out) override { out.push_back(&ch); }
};

// MOSFET level 1 (Shichman-Hodges) with Meyer gate capacitances.
struct Mosfet final : Device {
    enum { D, G, S, B };
    int node[4];
    double type = 1;   // +1 NMOS, -1 PMOS
    // Model and instance, already combined.
    double vto = 0, beta = 0, gamma = 0, phi = 0.6, lambda = 0, isat = 1e-14, pb = 0.8, fc = 0.5;
    double cbd0 = 0, cbs0 = 0, cbdsw = 0, cbssw = 0, mj = 0.5, mjsw = 0.5;
    double cox = 0, covgs = 0, covgd = 0, covgb = 0;
    // The linearization point (limited voltages) and what was found there.
    double vgs = 0, vds = 0, vbs = 0, von = 0, vdsat = 0, vcrit = 0;
    int mode = 1;
    bool valid = false;
    double gm = 0, gds = 0, gmbs = 0, ids = 0, ieqds = 0;
    double gbd = 0, gbs = 0, ibd = 0, ibs = 0, cbd = 0, cbs = 0, qbdQ = 0, qbsQ = 0;
    double gbdT = 0, gbsT = 0, ibdT = 0, ibsT = 0, ieqbd = 0, ieqbs = 0;
    double cgs = 0, cgd = 0, cgb = 0;                 // capacitances for this step and AC
    double mcs = 0, mcd = 0, mcb = 0, pcs = 0, pcd = 0, pcb = 0;   // Meyer: now, and at the last accepted point
    double geqgs = 0, geqgd = 0, geqgb = 0, ieqgs = 0, ieqgd = 0, ieqgb = 0;
    double vgs0 = 0, vgd0 = 0, vgb0 = 0;              // gate voltages at the last accepted point
    double evgs = 0, evds = 0, evbs = 0;              // voltages of the last evaluation
    Charge qbd, qbs, qgs, qgd, qgb;
    Slots<4> sl;
    bool nonlinear() const override { return true; }
    void bind(Setup& S) override { sl.init(S, node); }

    // Gate capacitances at the linearization point (in the device's own mode).
    void meyer() {
        double cs = 0, cd = 0, cb = 0;
        if (cox > 0) {
            double Vgs = mode > 0 ? vgs : vgs - vds, Vds = mode * vds;
            double vgst = Vgs - von, vsat = std::max(vdsat, 0.025);
            // Below saturation the channel charge is shared with the drain.
            auto share = [&](double full) {
                if (Vds >= vsat) { cs = full; cd = 0; return; }
                double vddif = 2 * vsat - Vds, vddif1 = vsat - Vds, vddif2 = vddif * vddif;
                cd = full * (1 - vsat * vsat / vddif2);
                cs = full * (1 - vddif1 * vddif1 / vddif2);
            };
            if (vgst <= -phi) cb = cox;
            else if (vgst <= -phi / 2) cb = -vgst * cox / phi;
            else if (vgst <= 0) { cb = -vgst * cox / phi; share(vgst * cox / (0.75 * phi) + cox * 2 / 3); }
            else share(cox * 2 / 3);
            if (mode < 0) std::swap(cs, cd);
        }
        mcs = cs; mcd = cd; mcb = cb;
    }

    void eval(const Ctx& c) override {
        const double* x = c.x;
        double vt = c.vt;
        double rgs = type * (x[node[G]] - x[node[S]]), rds = type * (x[node[D]] - x[node[S]]);
        double rbs = type * (x[node[B]] - x[node[S]]);
        bool lim = false, check = false;
        double predIds = 0, predBd = 0, predBs = 0;
        // The voltages this evaluation is made at: the solution's, unless limited.
        double egs = rgs, eds = rds, ebs = rbs;
        if (c.bypass && valid && !c.initJct && !c.noLimit && same(rgs, vgs) && same(rds, vds) && same(rbs, vbs)) {
            // Latent: keep the channel and junction values of the last
            // linearization. Below, the device is its own linear model.
        } else {
            double vgsO = vgs, vdsO = vds, vbsO = vbs, vonO = von;
            int oldMode = mode;
            double pVgs = mode > 0 ? vgs : vgs - vds, pVds = mode * vds, pVbs = mode > 0 ? vbs : vbs - vds;
            vgs = rgs; vds = rds; vbs = rbs;
            if (c.initJct) {
                vgs = type * vto; vds = 0; vbs = -1;
                lim = true;
            } else if (!c.noLimit) {
                double vgd = vgs - vds, vgdO = vgsO - vdsO;
                if (vdsO >= 0) {
                    vgs = fetlim(vgs, vgsO, vonO);
                    vds = vgs - vgd;
                    vds = limvds(vds, vdsO);
                } else {
                    vgd = fetlim(vgd, vgdO, vonO);
                    vds = vgs - vgd;
                    vds = -limvds(-vds, -vdsO);
                    vgs = vgd + vds;
                }
                if (vds >= 0) vbs = pnjlim(vbs, vbsO, vt, vcrit, lim);
                else vbs = pnjlim(vbs - vds, vbsO - vdsO, vt, vcrit, lim) + vds;
                auto moved = [](double a, double b) { return std::fabs(a - b) > 1e-9 + 1e-9 * std::fabs(b); };
                if (moved(vgs, rgs) || moved(vds, rds)) lim = true;
            }
            double vbd = vbs - vds, vgd = vgs - vds;
            mode = vds >= 0 ? 1 : -1;
            double Vgs = mode > 0 ? vgs : vgd, Vds = mode * vds, Vbs = mode > 0 ? vbs : vbd;
            check = !c.noLimit && !lim;
            if (check && mode != oldMode) lim = true;
            predIds = ids + gm * (Vgs - pVgs) + gds * (Vds - pVds) + gmbs * (Vbs - pVbs);
            predBd = ibdT + gbdT * (vbd - (evbs - evds));
            predBs = ibsT + gbsT * (vbs - evbs);
            egs = vgs; eds = vds; ebs = vbs;
            double sarg;
            if (Vbs <= 0) sarg = std::sqrt(phi - Vbs);
            else { sarg = std::sqrt(phi); sarg = std::max(0.0, sarg - Vbs / (2 * sarg)); }
            von = type * vto - gamma * std::sqrt(phi) + gamma * sarg;
            double vgst = Vgs - von;
            vdsat = std::max(vgst, 0.0);
            double arg = sarg > 0 ? gamma / (2 * sarg) : 0;
            double betap = beta * (1 + lambda * Vds);
            if (vgst <= 0) { ids = 0; gm = 0; gds = 0; gmbs = 0; }
            else if (vgst <= Vds) {
                ids = betap * vgst * vgst / 2; gm = betap * vgst; gds = lambda * beta * vgst * vgst / 2; gmbs = gm * arg;
            } else {
                ids = betap * Vds * (vgst - Vds / 2); gm = betap * Vds;
                gds = betap * (vgst - Vds) + lambda * beta * Vds * (vgst - Vds / 2); gmbs = gm * arg;
            }
            ieqds = type * (ids - gm * Vgs - gds * Vds - gmbs * Vbs);
            if (check && !agrees(predIds, ids, {c.reltol, c.abstol})) lim = true;
            // Bulk junctions.
            auto junction = [&](double v, double& g, double& i) {
                if (v <= -3 * vt) { g = c.gmin; i = g * v - isat; }
                else { double ev = std::exp(std::min(v / vt, 700.0)); g = isat * ev / vt + c.gmin; i = isat * (ev - 1) + c.gmin * v; }
            };
            junction(vbd, gbd, ibd);
            junction(vbs, gbs, ibs);
            qbdQ = 0; cbd = 0;
            depletion(vbd, cbd0, pb, mj, fc, qbdQ, cbd);
            depletion(vbd, cbdsw, pb, mjsw, fc, qbdQ, cbd);
            qbsQ = 0; cbs = 0;
            depletion(vbs, cbs0, pb, mj, fc, qbsQ, cbs);
            depletion(vbs, cbssw, pb, mjsw, fc, qbsQ, cbs);
            meyer();
            valid = true;
        }
        limited = lim;
        // Integration: cheap, and the history changes at every step.
        double ebd = ebs - eds, egd = egs - eds, dbd = ebd - (vbs - vds), dbs = ebs - vbs;
        gbdT = gbd + qbd.integ(c, qbdQ + cbd * dbd, cbd);
        ibdT = ibd + gbd * dbd + qbd.i;
        ieqbd = type * (ibdT - gbdT * ebd);
        gbsT = gbs + qbs.integ(c, qbsQ + cbs * dbs, cbs);
        ibsT = ibs + gbs * dbs + qbs.i;
        ieqbs = type * (ibsT - gbsT * ebs);
        if (check && (!agrees(predBd, ibdT, {c.reltol, c.abstol}) || !agrees(predBs, ibsT, {c.reltol, c.abstol}))) limited = true;
        // Gate capacitances (Meyer). Over a time step each is the mean of
        // its value now and at the last accepted point, as in SPICE.
        if (c.a == 0) {
            cgs = mcs + covgs; cgd = mcd + covgd; cgb = mcb + covgb;
            geqgs = geqgd = geqgb = ieqgs = ieqgd = ieqgb = 0;
            qgs.q = qgd.q = qgb.q = 0;
        } else {
            double egb = egs - ebs;
            cgs = (mcs + pcs) / 2 + covgs; cgd = (mcd + pcd) / 2 + covgd; cgb = (mcb + pcb) / 2 + covgb;
            geqgs = qgs.integ(c, qgs.q0 + cgs * (egs - vgs0), cgs);
            ieqgs = type * (qgs.i - geqgs * egs);
            geqgd = qgd.integ(c, qgd.q0 + cgd * (egd - vgd0), cgd);
            ieqgd = type * (qgd.i - geqgd * egd);
            geqgb = qgb.integ(c, qgb.q0 + cgb * (egb - vgb0), cgb);
            ieqgb = type * (qgb.i - geqgb * egb);
        }
        evgs = egs; evds = eds; evbs = ebs;
    }

    void load(const Ctx&, double* val, double* rhs) override {
        int dp = mode > 0 ? D : S, sp = mode > 0 ? S : D;
        // Every matrix entry of the device, each written once.
        double m[4][4] = {};
        auto g2 = [&](int a, int b, double y) { m[a][a] += y; m[a][b] -= y; m[b][a] -= y; m[b][b] += y; };
        auto vc = [&](int a, int b, int cc, int d, double y) { m[a][cc] += y; m[a][d] -= y; m[b][cc] -= y; m[b][d] += y; };
        vc(dp, sp, G, sp, gm);
        g2(dp, sp, gds);
        vc(dp, sp, B, sp, gmbs);
        g2(B, D, gbdT);
        g2(B, S, gbsT);
        g2(G, S, geqgs); g2(G, D, geqgd); g2(G, B, geqgb);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) val[sl.s[i][j]] += m[i][j];
        double r[4] = {};
        r[dp] -= ieqds; r[sp] += ieqds;
        r[B] -= ieqbd + ieqbs; r[D] += ieqbd; r[S] += ieqbs;
        r[G] -= ieqgs + ieqgd + ieqgb; r[S] += ieqgs; r[D] += ieqgd; r[B] += ieqgb;
        for (int i = 0; i < 4; i++) rhs[node[i]] += r[i];
    }
    void loadAC(double w, cplx* val, cplx*) const override {
        int dp = mode > 0 ? D : S, sp = mode > 0 ? S : D;
        sl.vccs(val, dp, sp, G, sp, gm);
        sl.g(val, dp, sp, gds);
        sl.vccs(val, dp, sp, B, sp, gmbs);
        sl.g(val, B, D, cplx(gbd, w * cbd));
        sl.g(val, B, S, cplx(gbs, w * cbs));
        sl.g(val, G, S, cplx(0, w * cgs));
        sl.g(val, G, D, cplx(0, w * cgd));
        sl.g(val, G, B, cplx(0, w * cgb));
    }
    void accept(const Ctx&) override {
        qbd.accept(); qbs.accept(); qgs.accept(); qgd.accept(); qgb.accept();
        vgs0 = evgs; vgd0 = evgs - evds; vgb0 = evgs - evbs;
        pcs = mcs; pcd = mcd; pcb = mcb;
    }
    void charges(std::vector<Charge*>& out) override { out.insert(out.end(), {&qbd, &qbs, &qgs, &qgd, &qgb}); }
};

// Bipolar transistor: Gummel-Poon transport model with Early effect, high
// injection, leakage, junction and diffusion charges.
struct Bjt final : Device {
    enum { C, B, E };
    int node[3];
    double type = 1;   // +1 NPN, -1 PNP
    double isat = 1e-16, bf = 100, br = 1, nf = 1, nr = 1, ise = 0, ne = 1.5, isc = 0, nc = 2;
    double ivaf = 0, ivar = 0, iikf = 0, iikr = 0;   // inverses: 0 means infinite
    double cje = 0, vje = 0.75, mje = 0.33, cjc = 0, vjc = 0.75, mjc = 0.33, tf = 0, tr = 0, fc = 0.5;
    bool off = false, valid = false;
    double vcrit = 0, vbeO = 0, vbcO = 0;   // the last linearization point
    double vbeE = 0, vbcE = 0;              // the last evaluation
    double gpi = 0, gmu = 0, gf = 0, gr = 0, cbe = 0, cbc = 0, cbx = 0, gx = 0;
    double ibe = 0, ibc = 0, ict = 0, qbeQ = 0, qbcQ = 0;
    double gpiT = 0, gmuT = 0, ieqbe = 0, ieqbc = 0, ieqct = 0;
    double icT = 0, ibT = 0;   // terminal currents at the last linearization point
    Charge qbe, qbc;
    Slots<3> sl;
    bool nonlinear() const override { return true; }
    void bind(Setup& S) override { sl.init(S, node); }

    void eval(const Ctx& c) override {
        const double* x = c.x;
        double vt = c.vt;
        double vbe = type * (x[node[B]] - x[node[E]]), vbc = type * (x[node[B]] - x[node[C]]);
        bool lim = false, check = false;
        double predIc = 0, predIb = 0;
        if (c.bypass && valid && !c.initJct && !c.noLimit && same(vbe, vbeO) && same(vbc, vbcO)) {
            // Latent: below, the device is its own linear model.
        } else {
            if (c.initJct) { vbe = off ? 0 : vcrit; vbc = 0; lim = true; }
            else if (!c.noLimit) {
                vbe = pnjlim(vbe, vbeO, nf * vt, vcrit, lim);
                vbc = pnjlim(vbc, vbcO, nr * vt, vcrit, lim);
            }
            check = !c.noLimit && !lim;
            double pbe = vbe - vbeE, pbc = vbc - vbcE;
            predIc = icT + gf * pbe + (gr - gmuT) * pbc;
            predIb = ibT + gpiT * pbe + (gmuT + gx) * pbc;
            vbeO = vbe; vbcO = vbc;
            auto ex = [](double v) { return std::exp(std::min(v, 700.0)); };
            double ef = ex(vbe / (nf * vt)), er = ex(vbc / (nr * vt));
            double If = isat * (ef - 1), gif = isat * ef / (nf * vt);
            double Ir = isat * (er - 1), gir = isat * er / (nr * vt);
            ibe = If / bf + c.gmin * vbe; ibc = Ir / br + c.gmin * vbc;
            gpi = gif / bf + c.gmin; gmu = gir / br + c.gmin;
            if (ise > 0) { double e = ex(vbe / (ne * vt)); ibe += ise * (e - 1); gpi += ise * e / (ne * vt); }
            if (isc > 0) { double e = ex(vbc / (nc * vt)); ibc += isc * (e - 1); gmu += isc * e / (nc * vt); }
            double q1 = 1 / (1 - vbc * ivaf - vbe * ivar);
            double q2 = If * iikf + Ir * iikr;
            double sq = std::sqrt(std::max(1 + 4 * q2, 1e-30)), s = (1 + sq) / 2, qb = q1 * s;
            double dqbe = s * q1 * q1 * ivar + q1 * gif * iikf / sq;
            double dqbc = s * q1 * q1 * ivaf + q1 * gir * iikr / sq;
            ict = (If - Ir) / qb;
            gf = (gif - ict * dqbe) / qb;
            gr = (-gir - ict * dqbc) / qb;
            // The forward transit charge is TF times the collector transport
            // current If/qb, so it also moves with vbc (cbx).
            qbeQ = tf * If / qb;
            cbe = tf * (gif - If / qb * dqbe) / qb;
            cbx = -tf * If / qb * dqbc / qb;
            depletion(vbe, cje, vje, mje, fc, qbeQ, cbe);
            qbcQ = tr * Ir;
            cbc = tr * gir;
            depletion(vbc, cjc, vjc, mjc, fc, qbcQ, cbc);
            valid = true;
        }
        limited = lim;
        double dbe = vbe - vbeO, dbc = vbc - vbcO;
        double ictE = ict + gf * dbe + gr * dbc, ibeE = ibe + gpi * dbe, ibcE = ibc + gmu * dbc;
        ieqct = type * (ictE - gf * vbe - gr * vbc);
        gpiT = gpi + qbe.integ(c, qbeQ + cbe * dbe + cbx * dbc, cbe);
        gx = c.a * cbx;
        ieqbe = type * (ibeE + qbe.i - gpiT * vbe - gx * vbc);
        gmuT = gmu + qbc.integ(c, qbcQ + cbc * dbc, cbc);
        ieqbc = type * (ibcE + qbc.i - gmuT * vbc);
        icT = ictE - ibcE - qbc.i;
        ibT = ibeE + qbe.i + ibcE + qbc.i;
        vbeE = vbe; vbcE = vbc;
        Tol tol{c.reltol, c.abstol};
        if (check && (!agrees(predIc, icT, tol) || !agrees(predIb, ibT, tol))) limited = true;
    }
    void load(const Ctx&, double* val, double* rhs) override {
        sl.g(val, B, E, gpiT); rhs[node[B]] -= ieqbe; rhs[node[E]] += ieqbe;
        if (gx != 0) sl.vccs(val, B, E, B, C, gx);
        sl.g(val, B, C, gmuT); rhs[node[B]] -= ieqbc; rhs[node[C]] += ieqbc;
        sl.vccs(val, C, E, B, E, gf);
        sl.vccs(val, C, E, B, C, gr);
        rhs[node[C]] -= ieqct; rhs[node[E]] += ieqct;
    }
    void loadAC(double w, cplx* val, cplx*) const override {
        sl.g(val, B, E, cplx(gpi, w * cbe));
        sl.vccs(val, B, E, B, C, cplx(0, w * cbx));
        sl.g(val, B, C, cplx(gmu, w * cbc));
        sl.vccs(val, C, E, B, E, gf);
        sl.vccs(val, C, E, B, C, gr);
    }
    void accept(const Ctx&) override { qbe.accept(); qbc.accept(); }
    void charges(std::vector<Charge*>& out) override { out.insert(out.end(), {&qbe, &qbc}); }
};

}  // namespace spice
