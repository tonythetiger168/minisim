// SPDX-License-Identifier: Apache-2.0
// mspice: behavioral sources and switches.
//
// A behavioral source (B element, or E/G with an expression) is an arbitrary
// function of node voltages, branch currents and time. The expression is
// compiled once into a list of operations, children before parents; one
// forward pass gives the value and one backward pass the derivative with
// respect to every input, which Newton's method needs.
#pragma once
#include "spice_dev.hpp"

namespace spice {

class BExpr {
public:
    struct Var {
        char kind;            // 'v': v(a) - v(b), 'i': the current of element a
        std::string a, b;     // names as written; the netlist builder resolves them
    };
    std::vector<Var> vars;
    bool usesTime = false;

    BExpr() = default;
    BExpr(const std::string& text, Scope* sc, double temp) : text_(text), p_(text_.c_str()), sc_(sc), temp_(temp) {
        root_ = ternary();
        ws();
        if (*p_) fail("unexpected '" + std::string(p_) + "'");
        val_.assign(ops_.size(), 0);
        adj_.assign(ops_.size(), 0);
        p_ = nullptr;
        sc_ = nullptr;
    }

    // in[k] is the value of vars[k]; grad[k] receives d/d vars[k].
    double eval(const double* in, double time, double* grad) {
        size_t n = ops_.size();
        double* v = val_.data();
        for (size_t i = 0; i < n; i++) {
            const Op& o = ops_[i];
            double a = o.a >= 0 ? v[o.a] : 0, b = o.b >= 0 ? v[o.b] : 0;
            switch (o.op) {
            case CONST: v[i] = o.k; break;
            case VAR: v[i] = in[o.a]; break;
            case TIME: v[i] = time; break;
            case NEG: v[i] = -a; break;
            case NOT: v[i] = a == 0; break;
            case ADD: v[i] = a + b; break;
            case SUB: v[i] = a - b; break;
            case MUL: v[i] = a * b; break;
            case DIV: v[i] = a / b; break;
            case MOD: v[i] = std::fmod(a, b); break;
            case POW: v[i] = std::pow(a, b); break;
            case LT: v[i] = a < b; break;
            case LE: v[i] = a <= b; break;
            case GT: v[i] = a > b; break;
            case GE: v[i] = a >= b; break;
            case EQ: v[i] = a == b; break;
            case NE: v[i] = a != b; break;
            case AND: v[i] = a != 0 && b != 0; break;
            case OR: v[i] = a != 0 || b != 0; break;
            case SEL: v[i] = a != 0 ? b : v[o.c]; break;
            case MIN: v[i] = std::min(a, b); break;
            case MAX: v[i] = std::max(a, b); break;
            case ATAN2: v[i] = std::atan2(a, b); break;
            case LIMIT: v[i] = std::min(std::max(a, b), v[o.c]); break;
            case SQRT: v[i] = std::sqrt(a); break;
            case EXP: v[i] = std::exp(a); break;
            case LN: v[i] = std::log(a); break;
            case LOG10: v[i] = std::log10(a); break;
            case ABS: v[i] = std::fabs(a); break;
            case SIN: v[i] = std::sin(a); break;
            case COS: v[i] = std::cos(a); break;
            case TAN: v[i] = std::tan(a); break;
            case ASIN: v[i] = std::asin(a); break;
            case ACOS: v[i] = std::acos(a); break;
            case ATAN: v[i] = std::atan(a); break;
            case SINH: v[i] = std::sinh(a); break;
            case COSH: v[i] = std::cosh(a); break;
            case TANH: v[i] = std::tanh(a); break;
            case FLOOR: v[i] = std::floor(a); break;
            case CEIL: v[i] = std::ceil(a); break;
            case INT: v[i] = std::trunc(a); break;
            case SGN: v[i] = (a > 0) - (a < 0); break;
            case DB: v[i] = 20 * std::log10(std::fabs(a)); break;
            case STEP: v[i] = a > 0; break;
            case RAMP: v[i] = a > 0 ? a : 0; break;
            }
        }
        for (size_t k = 0; k < vars.size(); k++) grad[k] = 0;
        double* d = adj_.data();
        std::fill(adj_.begin(), adj_.end(), 0.0);
        d[root_] = 1;
        for (size_t i = n; i-- > 0;) {
            double g = d[i];
            // Nothing flows into the branch a condition did not take, so an
            // infinite slope there (ln at zero) cannot spoil the result.
            if (g == 0) continue;
            const Op& o = ops_[i];
            double a = o.a >= 0 && o.op != VAR ? v[o.a] : 0, b = o.b >= 0 ? v[o.b] : 0;
            switch (o.op) {
            case VAR: grad[o.a] += g; break;
            case NEG: d[o.a] -= g; break;
            case ADD: d[o.a] += g; d[o.b] += g; break;
            case SUB: d[o.a] += g; d[o.b] -= g; break;
            case MUL: d[o.a] += g * b; d[o.b] += g * a; break;
            case DIV: d[o.a] += g / b; d[o.b] -= g * v[i] / b; break;
            case MOD: d[o.a] += g; break;
            case POW:
                if (b != 0) d[o.a] += g * b * std::pow(a, b - 1);
                if (a > 0) d[o.b] += g * v[i] * std::log(a);
                break;
            case SEL: d[a != 0 ? o.b : o.c] += g; break;
            case MIN: d[a <= b ? o.a : o.b] += g; break;
            case MAX: d[a >= b ? o.a : o.b] += g; break;
            case ATAN2: { double r = a * a + b * b; if (r > 0) { d[o.a] += g * b / r; d[o.b] -= g * a / r; } break; }
            case LIMIT: if (a > b && a < v[o.c]) d[o.a] += g; break;
            case SQRT: d[o.a] += g * 0.5 / v[i]; break;
            case EXP: d[o.a] += g * v[i]; break;
            case LN: d[o.a] += g / a; break;
            case LOG10: d[o.a] += g / (a * M_LN10); break;
            case ABS: d[o.a] += a < 0 ? -g : g; break;
            case SIN: d[o.a] += g * std::cos(a); break;
            case COS: d[o.a] -= g * std::sin(a); break;
            case TAN: d[o.a] += g * (1 + v[i] * v[i]); break;
            case ASIN: d[o.a] += g / std::sqrt(1 - a * a); break;
            case ACOS: d[o.a] -= g / std::sqrt(1 - a * a); break;
            case ATAN: d[o.a] += g / (1 + a * a); break;
            case SINH: d[o.a] += g * std::cosh(a); break;
            case COSH: d[o.a] += g * std::sinh(a); break;
            case TANH: d[o.a] += g * (1 - v[i] * v[i]); break;
            case DB: d[o.a] += g * 20 / (a * M_LN10); break;
            case RAMP: if (a > 0) d[o.a] += g; break;
            default: break;   // constants, comparisons, steps: flat
            }
        }
        return v[root_];
    }

private:
    enum {
        CONST, VAR, TIME, NEG, NOT, ADD, SUB, MUL, DIV, MOD, POW, LT, LE, GT, GE, EQ, NE, AND, OR, SEL,
        MIN, MAX, ATAN2, LIMIT,
        SQRT, EXP, LN, LOG10, ABS, SIN, COS, TAN, ASIN, ACOS, ATAN, SINH, COSH, TANH, FLOOR, CEIL, INT, SGN, DB, STEP, RAMP
    };
    struct Op {
        int op, a = -1, b = -1, c = -1;
        double k = 0;
    };
    std::vector<Op> ops_;
    std::vector<double> val_, adj_;
    int root_ = 0;
    std::string text_;
    const char* p_ = nullptr;
    Scope* sc_ = nullptr;
    double temp_ = 27;

    [[noreturn]] void fail(const std::string& why) { throw Error("expression '" + text_ + "': " + why); }
    int emit(int op, int a = -1, int b = -1, int c = -1, double k = 0) {
        ops_.push_back({op, a, b, c, k});
        return (int)ops_.size() - 1;
    }
    int constant(double k) { return emit(CONST, -1, -1, -1, k); }
    void ws() { while (std::isspace((unsigned char)*p_)) p_++; }
    bool eat(const char* s) {
        ws();
        size_t n = std::strlen(s);
        if (std::strncmp(p_, s, n)) return false;
        p_ += n;
        return true;
    }
    int ternary() {
        int c = lor();
        if (!eat("?")) return c;
        int a = ternary();
        if (!eat(":")) fail("expected ':'");
        int b = ternary();
        return emit(SEL, c, a, b);
    }
    int lor() {
        int v = land();
        while (eat("||")) v = emit(OR, v, land());
        return v;
    }
    int land() {
        int v = cmp();
        while (eat("&&")) v = emit(AND, v, cmp());
        return v;
    }
    int cmp() {
        int v = add();
        for (;;) {
            if (eat("<=")) v = emit(LE, v, add());
            else if (eat(">=")) v = emit(GE, v, add());
            else if (eat("==")) v = emit(EQ, v, add());
            else if (eat("!=")) v = emit(NE, v, add());
            else if (eat("<")) v = emit(LT, v, add());
            else if (eat(">")) v = emit(GT, v, add());
            else return v;
        }
    }
    int add() {
        int v = mul();
        for (;;) {
            if (eat("+")) v = emit(ADD, v, mul());
            else if (eat("-")) v = emit(SUB, v, mul());
            else return v;
        }
    }
    int mul() {
        int v = unary();
        for (;;) {
            ws();
            if (p_[0] == '*' && p_[1] != '*') { p_++; v = emit(MUL, v, unary()); }
            else if (eat("/")) v = emit(DIV, v, unary());
            else if (eat("%")) v = emit(MOD, v, unary());
            else return v;
        }
    }
    int unary() {
        if (eat("-")) return emit(NEG, unary());
        if (eat("+")) return unary();
        if (eat("!")) return emit(NOT, unary());
        int b = primary();
        if (eat("**") || eat("^")) return emit(POW, b, unary());
        return b;
    }
    int variable(char kind) {
        // After "v(" or "i(": names run to ',' or ')'.
        std::string a, b;
        std::string* cur = &a;
        for (;; p_++) {
            if (!*p_) fail("expected ')'");
            if (*p_ == ')') { p_++; break; }
            if (*p_ == ',' && cur == &a && kind == 'v') { cur = &b; continue; }
            if (!std::isspace((unsigned char)*p_)) *cur += *p_;
        }
        if (a.empty()) fail("an output needs a name");
        for (size_t k = 0; k < vars.size(); k++)
            if (vars[k].kind == kind && vars[k].a == a && vars[k].b == b) return emit(VAR, (int)k);
        vars.push_back({kind, a, b});
        return emit(VAR, (int)vars.size() - 1);
    }
    int primary() {
        ws();
        if (eat("(")) {
            int v = ternary();
            if (!eat(")")) fail("expected ')'");
            return v;
        }
        if (std::isdigit((unsigned char)*p_) || *p_ == '.') {
            double v;
            const char* e = parseNumber(p_, v);
            if (!e) fail("bad number");
            p_ = e;
            return constant(v);
        }
        if (std::isalpha((unsigned char)*p_) || *p_ == '_') {
            std::string id;
            while (std::isalnum((unsigned char)*p_) || *p_ == '_') id += *p_++;
            if (eat("(")) {
                if (id == "v" || id == "i") return variable(id[0]);
                std::vector<int> a;
                if (!eat(")")) {
                    do a.push_back(ternary()); while (eat(","));
                    if (!eat(")")) fail("expected ')'");
                }
                return call(id, a);
            }
            if (id == "pi") return constant(M_PI);
            if (id == "time") { usesTime = true; return emit(TIME); }
            if (id == "temper") return constant(temp_);
            if (id == "hertz") return constant(0);
            double v;
            if (sc_ && sc_->lookup(id, v)) return constant(v);
            fail("unknown parameter '" + id + "'");
        }
        fail("unexpected '" + std::string(p_) + "'");
    }
    int call(const std::string& f, const std::vector<int>& a) {
        auto need = [&](size_t n) { if (a.size() != n) fail(f + "() takes " + std::to_string(n) + " argument(s)"); };
        static const std::map<std::string, int> one = {
            {"sqrt", SQRT}, {"exp", EXP}, {"ln", LN}, {"log", LN}, {"log10", LOG10}, {"abs", ABS}, {"sin", SIN},
            {"cos", COS}, {"tan", TAN}, {"asin", ASIN}, {"acos", ACOS}, {"atan", ATAN}, {"sinh", SINH},
            {"cosh", COSH}, {"tanh", TANH}, {"floor", FLOOR}, {"ceil", CEIL}, {"int", INT}, {"sgn", SGN},
            {"db", DB}, {"u", STEP}, {"stp", STEP}, {"uramp", RAMP}};
        if (auto it = one.find(f); it != one.end()) { need(1); return emit(it->second, a[0]); }
        if (f == "min") { need(2); return emit(MIN, a[0], a[1]); }
        if (f == "max") { need(2); return emit(MAX, a[0], a[1]); }
        if (f == "pow" || f == "pwr") { need(2); return emit(POW, a[0], a[1]); }
        if (f == "atan2") { need(2); return emit(ATAN2, a[0], a[1]); }
        if (f == "limit") { need(3); return emit(LIMIT, a[0], a[1], a[2]); }
        if (f == "if") { need(3); return emit(SEL, a[0], a[1], a[2]); }
        fail("unknown function '" + f + "'");
    }
};

// B element: v = f(...) between p and n, or a current i = f(...) from p to n.
struct Behav final : Device {
    int p = -1, n = -1, br = -1;
    bool isV = false;
    BExpr ex;
    struct In {
        bool cur;      // a branch current (unknown a), else v(a) - v(b)
        int a, b;
        Device* src;   // for a current: the element that carries it
        int s[4];      // matrix slots: (row1, a) (row1, b) (row2, a) (row2, b)
    };
    std::vector<In> in;
    std::vector<double> xin, g;
    double f = 0;
    int pb = 0, nb = 0, bp = 0, bn = 0;
    bool nonlinear() const override { return true; }
    void branches(Setup& S) override { if (isV) br = S.newUnknown(name + "#branch"); }
    void bind(Setup& S) override {
        // A voltage's derivatives sit in its branch row; a current's in the rows of p and n.
        int r1 = isV ? br : p, r2 = isV ? -1 : n;
        for (In& k : in) {
            if (k.cur) k.a = k.src->branch();
            k.s[0] = S.slot(r1, k.a); k.s[2] = S.slot(r2, k.a);
            k.s[1] = k.cur ? k.s[0] : S.slot(r1, k.b);
            k.s[3] = k.cur ? k.s[2] : S.slot(r2, k.b);
        }
        if (isV) { pb = S.slot(p, br); nb = S.slot(n, br); bp = S.slot(br, p); bn = S.slot(br, n); }
        xin.assign(in.size(), 0);
        g.assign(in.size(), 0);
    }
    template <class T> void stamp(T* val) const {
        double sign = isV ? -1 : 1;
        for (size_t k = 0; k < in.size(); k++) {
            const In& i = in[k];
            double y = sign * g[k];
            val[i.s[0]] += y;
            if (!i.cur) val[i.s[1]] -= y;
            if (!isV) {
                val[i.s[2]] -= y;
                if (!i.cur) val[i.s[3]] += y;
            }
        }
        if (isV) { val[pb] += 1; val[nb] -= 1; val[bp] += 1; val[bn] -= 1; }
    }
    void load(const Ctx& c, double* val, double* rhs) override {
        for (size_t k = 0; k < in.size(); k++) xin[k] = in[k].cur ? c.x[in[k].a] : c.x[in[k].a] - c.x[in[k].b];
        f = ex.eval(xin.data(), c.time, g.data());
        // A value that is not a number (exp of a wild first guess) makes the
        // solution not a number too: the iteration fails and the operating
        // point's continuation methods take over.
        stamp(val);
        double eq = f;
        for (size_t k = 0; k < in.size(); k++) eq -= g[k] * xin[k];
        if (isV) rhs[br] += eq;
        else { rhs[p] -= eq; rhs[n] += eq; }
    }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
    int branch() const override { return br; }
};

// S and W elements: a resistor that a voltage or a current switches between
// roff and ron.
//   abrupt (.model sw vt vh, csw it ih): on above vt + vh, off below vt - vh,
//          and in between what it was at the last accepted point;
//   smooth (.model vswitch von voff, iswitch ion ioff): log(R) follows a
//          smooth S-curve between the two levels, which Newton's method and
//          the time step control like much better.
struct Switch final : Device {
    int p = -1, n = -1, cp = -1, cn = -1, cb = -1;   // control: v(cp) - v(cn), or the current of branch cb
    double ron = 1, roff = 1e6, vt = 0, vh = 0, von = 1, voff = 0;
    Device* ctl = nullptr;   // W: the element whose current switches it
    bool smooth = false;
    bool on = false, onAcc = false;
    double g = 0, gc = 0;   // conductance, and d(current)/d(control)
    Slots<2> sl;
    int sc[4] = {};
    bool nonlinear() const override { return true; }
    void bind(Setup& S) override {
        int nd[2] = {p, n};
        sl.init(S, nd);
        if (ctl) cb = ctl->branch();
        if (cb >= 0) { sc[0] = sc[1] = S.slot(p, cb); sc[2] = sc[3] = S.slot(n, cb); }
        else { sc[0] = S.slot(p, cp); sc[1] = S.slot(p, cn); sc[2] = S.slot(n, cp); sc[3] = S.slot(n, cn); }
    }
    template <class T> void stamp(T* val) const {
        sl.g(val, 0, 1, g);
        if (gc == 0) return;
        val[sc[0]] += gc; val[sc[2]] -= gc;
        if (cb < 0) { val[sc[1]] -= gc; val[sc[3]] += gc; }
    }
    void load(const Ctx& c, double* val, double* rhs) override {
        double vc = cb >= 0 ? c.x[cb] : c.x[cp] - c.x[cn], v = c.x[p] - c.x[n];
        if (smooth) {
            double t = std::min(1.0, std::max(0.0, (vc - voff) / (von - voff)));
            double span = std::log(ron / roff);
            g = 1 / (roff * std::exp(span * t * t * (3 - 2 * t)));
            gc = -g * span * 6 * t * (1 - t) / (von - voff) * v;
            limited = false;
        } else {
            bool was = on;
            if (vc > vt + vh) on = true;
            else if (vc < vt - vh) on = false;
            else if (c.tran) on = onAcc;
            if (!c.tran) onAcc = on;
            limited = on != was;   // one more iteration with the new state
            g = on ? 1 / ron : 1 / roff;
            gc = 0;
        }
        stamp(val);
        double eq = -gc * vc;
        rhs[p] -= eq; rhs[n] += eq;
    }
    void loadAC(double, cplx* val, cplx*) const override { stamp(val); }
    void accept(const Ctx&) override { onAcc = on; }
    void noise(double kelvin, std::vector<NoiseSrc>& out) const override { out.push_back({p, n, 4 * kBoltz * kelvin * g, 0}); }
};

}  // namespace spice
