// Unit tests for the parasitic extraction engine (src/rcx.hpp) and the SPEF
// reader and writer (src/spef.hpp). Expected values are worked out here from
// the published formulas, not copied from the tool's output.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

#include "rcx.hpp"

using namespace rcx;

static bool near(double a, double b, double rel = 1e-9) {
    return std::fabs(a - b) <= rel * std::max(std::fabs(a), std::fabs(b)) + 1e-15;
}

// One metal layer, 0.1 um wide and 0.15 um thick, 0.4 um above the substrate.
static const char* kItf =
    "$ test stack\n"
    "TECHNOLOGY = t1\n"
    "GLOBAL_TEMPERATURE = 25\n"
    "DIELECTRIC top { THICKNESS = 0.5 ER = 3.9 }\n"
    "CONDUCTOR m2 { THICKNESS = 0.2 WMIN = 0.1 SMIN = 0.1 RPSQ = 0.3 }\n"
    "DIELECTRIC ild { THICKNESS = 0.35 ER = 2.9 }\n"
    "CONDUCTOR m1 { THICKNESS = 0.15 WMIN = 0.1 SMIN = 0.1 RPSQ = 0.4 CRT1 = 0.002 CRT2 = 0.00001 }\n"
    "DIELECTRIC pmd { THICKNESS = 0.4 ER = 3.9 }\n"
    "VIA v1 { FROM = m1 TO = m2 RPV = 6 CRT1 = 0.001 }\n";

static Layout base() {
    Layout L;
    L.design = "t";
    L.dbu = 1000;
    L.layers.push_back({"m1", 0.1, 0.1, 0.4, -1, -1, -1, -1});
    L.layers.push_back({"m2", 0.1, 0.1, 0.3, -1, -1, -1, -1});
    return L;
}
static Net wire(const char* name, int layer, i64 x0, i64 y0, i64 x1, i64 y1) {
    Net n;
    n.name = name;
    n.wires.push_back({layer, x0, y0, x1, y1, 100, 0, 0});
    return n;
}
static double coupling(const Result& R, int a, int b) {
    double c = 0;
    for (auto& k : R.nets[static_cast<size_t>(a)].cc)
        if (k.onet == b) c += k.c;
    return c;
}

int main() {
    Stack itf = Stack::parseItf(kItf, "test.itf");
    assert(itf.name == "t1" && itf.cond.size() == 2 && itf.diel.size() == 3 && itf.vias.size() == 1);
    assert(itf.cond[0].name == "m1" && near(itf.cond[0].zbot, 0.4));
    assert(itf.cond[1].name == "m2" && near(itf.cond[1].zbot, 0.75));
    assert(near(itf.erAt(0.1), 3.9) && near(itf.erAt(0.5), 2.9) && near(itf.erAt(0.9), 3.9));
    // 0.2 um of er 3.9 in series with 0.2 um of er 2.9
    assert(near(itf.erBetween(0.2, 0.6), 0.4 / (0.2 / 3.9 + 0.2 / 2.9)));
    try { Stack::parseItf("CONDUCTOR x { WMIN = 1 }", "bad.itf"); assert(false); }
    catch (const std::runtime_error& e) { assert(std::string(e.what()).find("bad.itf:1") == 0); }

    Options opt;
    opt.threads = 1;
    opt.maxSeg = 0;

    // --- two parallel wires, 10 um long, 0.1 um apart
    {
        Layout L = base();
        L.nets.push_back(wire("a", 0, 0, 0, 10000, 0));
        L.nets.push_back(wire("b", 0, 0, 200, 10000, 200));
        Stack st = makeStack(L, &itf);
        Result R = extract(L, st, opt);
        // R = 0.4 ohm/sq x 10 um / 0.1 um
        assert(R.nets[0].res.size() == 1 && near(R.nets[0].res[0].r, 40.0));
        const double w = 0.1, t = 0.15, h = 0.4, s = 0.1, len = 10;
        double epsG = kEps0 * 3.9, epsL = kEps0 * 2.9;
        double cg = epsG * (1.15 * w / h + 2.80 * std::pow(t / h, 0.222)) * len;
        double cc = epsL * (0.03 * w / h + 0.83 * t / h - 0.07 * std::pow(t / h, 0.222)) * std::pow(s / h, -1.34) * len;
        // the neighbour takes 0.2h / (s + 0.2h) of the fringe on its side from ground
        double moved = epsG * 1.40 * std::pow(t / h, 0.222) * len * (0.2 * h / (s + 0.2 * h));
        cg -= moved;
        cc += moved;
        assert(near(R.nets[0].cgTotal, cg) && near(R.nets[1].cgTotal, cg));
        assert(near(coupling(R, 0, 1), cc) && near(coupling(R, 1, 0), cc));
        assert(near(R.nets[0].wirelen, 10.0) && R.nets[0].opens == 0 && R.shorts.empty());
        // half the capacitance at each end of the one piece
        assert(R.nets[0].nodes.size() == 2 && near(R.nets[0].nodes[0].cg, cg / 2));

        // scale factors and grounding
        Options o2 = opt;
        o2.rscale = 2; o2.cscale = 3; o2.ccscale = 0.5;
        Result R2 = extract(L, st, o2);
        assert(near(R2.nets[0].res[0].r, 80.0) && near(R2.nets[0].cgTotal, 3 * cg) && near(coupling(R2, 0, 1), 1.5 * cc));
        Options o3 = opt;
        o3.coupled = false; o3.ccFactor = 2;
        Result R3 = extract(L, st, o3);
        assert(R3.nets[0].cc.empty() && near(R3.nets[0].cgTotal, cg + 2 * cc));
        Options o4 = opt;
        o4.ccThreshold = 1e9;
        assert(extract(L, st, o4).nets[1].cc.empty());

        // temperature: 0.4 x (1 + 0.002 x 100 + 0.00001 x 100^2) = 0.52 ohm/sq
        Options o5 = opt;
        o5.temp = 125; o5.tempGiven = true;
        assert(near(extract(L, st, o5).nets[0].res[0].r, 52.0));

        // cutting into pieces keeps the totals
        Options o6 = opt;
        o6.maxSeg = 3;
        Result R6 = extract(L, st, o6);
        assert(R6.nets[0].res.size() == 4 && near(R6.nets[0].rTotal, 40.0));
        assert(near(R6.nets[0].cgTotal, cg) && near(coupling(R6, 0, 1), cc));
    }

    // --- a wire between two others hides them from each other
    {
        Layout L = base();
        L.nets.push_back(wire("a", 0, 0, 0, 10000, 0));
        L.nets.push_back(wire("mid", 0, 2000, 200, 8000, 200));
        L.nets.push_back(wire("c", 0, 0, 400, 10000, 400));
        Result R = extract(L, makeStack(L, &itf), opt);
        const double w = 0.1, t = 0.15, h = 0.4;
        double k = kEps0 * 2.9 * (0.03 * w / h + 0.83 * t / h - 0.07 * std::pow(t / h, 0.222));
        double fr = kEps0 * 3.9 * 1.40 * std::pow(t / h, 0.222);
        // a sees c only over the 4 um that mid does not cover, at 0.3 um
        assert(near(coupling(R, 0, 2), (k * std::pow(0.3 / h, -1.34) + fr * 0.2 * h / (0.3 + 0.2 * h)) * 4.0));
        assert(near(coupling(R, 0, 1), (k * std::pow(0.1 / h, -1.34) + fr * 0.2 * h / (0.1 + 0.2 * h)) * 6.0));
        // moving fringe from ground to coupling leaves a's total as it was
        double total = kEps0 * 3.9 * (1.15 * w / h + 2.80 * std::pow(t / h, 0.222)) * 10 +
                       k * (std::pow(0.3 / h, -1.34) * 4.0 + std::pow(0.1 / h, -1.34) * 6.0);
        assert(near(R.nets[0].cgTotal + R.nets[0].ccTotal, total));
    }

    // --- power metal next to a wire loads it to ground
    {
        Layout L = base();
        L.nets.push_back(wire("a", 0, 0, 0, 10000, 0));
        Net vdd = wire("VDD", 0, 0, 200, 10000, 200);
        vdd.power = true;
        L.nets.push_back(vdd);
        Result R = extract(L, makeStack(L, &itf), opt);
        const double w = 0.1, t = 0.15, h = 0.4;
        double cg = kEps0 * 3.9 * (1.15 * w / h + 2.80 * std::pow(t / h, 0.222)) * 10;
        double cc = kEps0 * 2.9 * (0.03 * w / h + 0.83 * t / h - 0.07 * std::pow(t / h, 0.222)) * std::pow(0.1 / h, -1.34) * 10;
        assert(R.nets[0].cc.empty() && near(R.nets[0].cgTotal, cg + cc));
        assert(R.nets[1].nodes.empty());
    }

    // --- a crossing on the next layer, and a via
    {
        Layout L = base();
        L.nets.push_back(wire("a", 0, 0, 0, 10000, 0));
        L.nets.push_back(wire("x", 1, 5000, -5000, 5000, 5000));
        Net v = wire("v", 0, 0, 3000, 2000, 3000);
        v.wires.push_back({1, 2000, 3000, 2000, 6000, 100, 0, 0});
        Via via;
        via.lo = 0; via.hi = 1; via.x = 2000; via.y = 3000; via.cuts = 2;
        via.rlo = {1950, 2950, 2050, 3050}; via.rhi = via.rlo;
        v.vias.push_back(via);
        Pin p;
        p.inst = "u1"; p.pin = "A"; p.cell = "INV"; p.dir = 'I';
        p.shapes.push_back({0, {-50, 2900, 50, 3100}});
        v.pins.push_back(p);
        L.nets.push_back(v);
        Stack st = makeStack(L, &itf);
        Result R = extract(L, st, opt);
        // m2 bottom 0.75, m1 top 0.55: a 0.2 um gap of er 2.9 over a 0.1 x 0.1 um crossing
        double d = 0.2, tbar = 0.175;
        double cx = kEps0 * 2.9 * (1.15 * 0.01 / d + 0.70 * 0.4 * std::pow(tbar / d, 0.222));
        assert(near(coupling(R, 0, 1), cx) && near(coupling(R, 1, 0), cx));
        const NetOut& n = R.nets[2];
        assert(n.opens == 0 && n.nodes[0].pin == 0);
        double rvia = 0, rwire = 0, rpin = 0;
        for (auto& r : n.res) {
            if (near(r.r, 3.0)) rvia += r.r;          // 6 ohm per cut, two cuts
            else if (r.r < 0.01) rpin += r.r;         // the pin's contact
            else rwire += r.r;
        }
        assert(near(rvia, 3.0) && rpin > 0 && near(rwire, 0.4 * 20 + 0.3 * 30));
    }

    // --- two layers apart: coupling through what the layer between leaves open
    {
        Stack three = Stack::parseItf(
            "DIELECTRIC d3 { THICKNESS = 0.5 ER = 3.9 }\n"
            "CONDUCTOR m3 { THICKNESS = 0.2 RPSQ = 0.1 }\n"
            "DIELECTRIC d2 { THICKNESS = 0.4 ER = 3.9 }\n"
            "CONDUCTOR m2 { THICKNESS = 0.2 RPSQ = 0.3 }\n"
            "DIELECTRIC d1 { THICKNESS = 0.35 ER = 3.9 }\n"
            "CONDUCTOR m1 { THICKNESS = 0.15 RPSQ = 0.4 }\n"
            "DIELECTRIC d0 { THICKNESS = 0.4 ER = 3.9 }\n", "three.itf");
        Layout L = base();
        L.layers.push_back({"m3", 0.1, 0.1, 0.1, -1, -1, -1, -1});
        L.nets.push_back(wire("lo", 0, 0, 0, 10000, 0));
        L.nets.push_back(wire("hi", 2, 0, 0, 10000, 0));
        Stack st = makeStack(L, &three);
        Result R = extract(L, st, opt);
        // m1 top 0.55, m3 bottom 1.15: 0.6 um; the wires overlap over 10 x 0.1 um
        double d = 0.6, tbar = 0.175;
        double c2 = kEps0 * 3.9 * (1.15 * 1.0 / d + 0.70 * 2 * 10.1 * std::pow(tbar / d, 0.222));
        assert(near(coupling(R, 1, 0), c2) && near(coupling(R, 0, 1), c2));
        // a wire of a third net on m2 over 4 um of the 10 hides that part
        L.nets.push_back(wire("mid", 1, 3000, 0, 7000, 0));
        Result S = extract(L, makeStack(L, &three), opt);
        assert(near(coupling(S, 1, 0), 0.6 * c2));
        assert(coupling(S, 2, 0) > 0 && coupling(S, 1, 2) > 0);
    }

    // --- opens and shorts
    {
        Layout L = base();
        Net n = wire("open", 0, 0, 0, 1000, 0);
        n.wires.push_back({0, 5000, 0, 6000, 0, 100, 0, 0});
        L.nets.push_back(n);
        L.nets.push_back(wire("s1", 0, 0, 1000, 5000, 1000));
        L.nets.push_back(wire("s2", 0, 2000, 500, 2000, 1500));
        Result R = extract(L, makeStack(L, &itf), opt);
        assert(R.nets[0].opens == 1 && R.nets[1].opens == 0);
        assert(R.shorts.size() == 1 && R.shorts[0].netA == 1 && R.shorts[0].netB == 2);
    }

    // --- without a process file the LEF values are used
    {
        Layout L = base();
        L.layers[0].areaCap = 0.08; L.layers[0].edgeCap = 0.04;
        L.nets.push_back(wire("a", 0, 0, 0, 10000, 0));
        Result R = extract(L, makeStack(L, nullptr), opt);
        assert(near(R.nets[0].cgTotal, 0.08 * 10 * 0.1 + 0.04 * 2 * 10));
        assert(near(R.nets[0].res[0].r, 40.0));
    }
    // a LEF layer the process file does not have
    {
        Layout L = base();
        L.layers[1].name = "metal2";
        try { makeStack(L, &itf); assert(false); }
        catch (const std::runtime_error& e) { assert(std::string(e.what()).find("-map") != std::string::npos); }
        Stack st = makeStack(L, &itf, {{"metal2", "m2"}});
        assert(st.cond[1].name == "metal2" && near(st.cond[1].zbot, 0.75));
    }

    // --- a random layout: the same answer on 1 and 4 threads, and SPEF round trip
    {
        Layout L = base();
        std::mt19937 rng(7);
        for (int i = 0; i < 3000; i++) {
            Net n;
            n.name = i % 7 == 0 ? "bus[" + std::to_string(i) + "]" : "top/n" + std::to_string(i);
            i64 x = static_cast<i64>(rng() % 200) * 200, y = static_cast<i64>(rng() % 400) * 200;
            i64 len = (1 + static_cast<i64>(rng() % 30)) * 200;
            n.wires.push_back({0, x, y, x + len, y, 100, 50, 50});
            n.wires.push_back({1, x + len, y, x + len, y + 600, 100, 50, 50});
            Via v;
            v.lo = 0; v.hi = 1; v.x = x + len; v.y = y;
            v.rlo = {v.x - 50, v.y - 50, v.x + 50, v.y + 50}; v.rhi = v.rlo;
            n.vias.push_back(v);
            Pin p;
            p.inst = "top/u" + std::to_string(i); p.pin = "Z"; p.cell = "BUF"; p.dir = 'O';
            p.shapes.push_back({0, {x - 50, y - 50, x + 50, y + 50}});
            n.pins.push_back(p);
            Pin q;
            q.pin = "out" + std::to_string(i); q.dir = 'O';
            q.shapes.push_back({1, {x + len - 50, y + 550, x + len + 50, y + 650}});
            n.pins.push_back(q);
            L.nets.push_back(n);
        }
        Stack st = makeStack(L, &itf);
        Options o1;
        o1.threads = 1;
        Options o4 = o1;
        o4.threads = 4;
        Result A = extract(L, st, o1), B = extract(L, st, o4);
        size_t ncc = 0;
        for (size_t i = 0; i < L.nets.size(); i++) {
            const NetOut& a = A.nets[i];
            const NetOut& b = B.nets[i];
            assert(a.nodes.size() == b.nodes.size() && a.res.size() == b.res.size() && a.cc.size() == b.cc.size());
            for (size_t k = 0; k < a.nodes.size(); k++) assert(a.nodes[k].cg == b.nodes[k].cg);
            for (size_t k = 0; k < a.cc.size(); k++)
                assert(a.cc[k].c == b.cc[k].c && a.cc[k].onet == b.cc[k].onet && a.cc[k].onode == b.cc[k].onode);
            assert(a.cgTotal == b.cgTotal && a.ccTotal == b.ccTotal);
            ncc += a.cc.size();
        }
        assert(ncc > 1000);
        // every coupling capacitor is in both nets with one value
        for (size_t i = 0; i < L.nets.size(); i++)
            for (auto& c : A.nets[i].cc) {
                bool found = false;
                for (auto& d : A.nets[static_cast<size_t>(c.onet)].cc)
                    found |= d.onet == static_cast<int>(i) && d.onode == c.node && d.node == c.onode && d.c == c.c;
                assert(found);
            }

        for (int nameMap = 0; nameMap < 2; nameMap++) {
            std::FILE* f = std::tmpfile();
            assert(f);
            SpefOptions so;
            so.version = "test"; so.date = "today"; so.nameMap = nameMap; so.threads = 4;
            writeSpef(f, L, A, so);
            long size = std::ftell(f);
            std::rewind(f);
            std::string text(static_cast<size_t>(size), 0);
            assert(std::fread(text.data(), 1, text.size(), f) == text.size());
            std::fclose(f);
            spef::File one = spef::parse(text, "mem.spef", 1), four = spef::parse(text, "mem.spef", 4);
            assert(one.h.design == "t" && one.h.program == "mrcx" && one.h.flow.size() == 3);
            assert(near(one.cScale, 1e-15) && near(one.tScale, 1e-9) && near(one.rScale, 1.0));
            assert(one.nets.size() == L.nets.size() && four.nets.size() == one.nets.size());
            assert(one.ports.size() == L.nets.size());
            for (size_t i = 0; i < L.nets.size(); i++) {
                const spef::Net& n = one.nets[i];
                assert(n.name == L.nets[i].name && four.nets[i].name == n.name);
                assert(n.caps.size() == four.nets[i].caps.size() && n.res.size() == A.nets[i].res.size());
                assert(n.conns.size() == 2 && !n.conns[0].port && n.conns[1].port);
                assert(n.conns[0].name == L.nets[i].pins[0].inst + ":Z" && n.conns[0].cell == "BUF");
                assert(n.conns[1].name == L.nets[i].pins[1].pin && n.conns[0].hasXY);
                double sum = 0;
                for (auto& c : n.caps) sum += c.c;
                assert(near(sum, n.total, 1e-4) && near(n.total, A.nets[i].cgTotal + A.nets[i].ccTotal, 1e-5));
                for (auto& c : n.caps)
                    if (!c.b.empty()) assert(c.a.rfind(n.name + ":", 0) == 0 || c.a == n.conns[0].name || c.a == n.conns[1].name);
            }
            // the generic writer, read back, is the same file
            std::FILE* g = std::tmpfile();
            spef::write(g, one);
            size = std::ftell(g);
            std::rewind(g);
            std::string text2(static_cast<size_t>(size), 0);
            assert(std::fread(text2.data(), 1, text2.size(), g) == text2.size());
            std::fclose(g);
            spef::File again = spef::parse(text2, "mem2.spef");
            assert(again.nets.size() == one.nets.size() && again.ports.size() == one.ports.size());
            for (size_t i = 0; i < one.nets.size(); i++) {
                assert(again.nets[i].name == one.nets[i].name && again.nets[i].caps.size() == one.nets[i].caps.size());
                for (size_t k = 0; k < one.nets[i].caps.size(); k++)
                    assert(again.nets[i].caps[k].a == one.nets[i].caps[k].a && again.nets[i].caps[k].b == one.nets[i].caps[k].b &&
                           again.nets[i].caps[k].c == one.nets[i].caps[k].c);
            }
        }
    }

    // --- SPEF as other tools write it: triplets, comments, units, *N nodes
    {
        const char* text =
            "*SPEF \"IEEE 1481-1998\"\n*DESIGN \"top\"\n*DATE \"d\"\n*VENDOR \"v\"\n*PROGRAM \"p\"\n*VERSION \"1\"\n"
            "*DESIGN_FLOW \"A\" \"B\"\n*DIVIDER /\n*DELIMITER :\n*BUS_DELIMITER []\n"
            "*T_UNIT 1 PS\n*C_UNIT 10 PF\n*R_UNIT 1 KOHM\n*L_UNIT 1 HENRY\n"
            "// a comment\n*NAME_MAP\n*1 u1/n\\[3\\]\n*2 u1/buf\n*PORTS\n*1 I *C 0 0\n"
            "*D_NET *1 0.5:0.6:0.7\n*CONN\n*P *1 I *C 1.0 2.0\n*I *2:A I *L 0.01 *D BUF\n*N *1:2 *C 3 4\n"
            "*CAP\n1 *1:2 0.1:0.2:0.3\n2 *2:A other:1 0.4\n*RES\n1 *1 *1:2 5 // r\n2 *1:2 *2:A 7\n*END\n"
            "*R_NET x 1\n*END\n"
            "*D_NET second 1\n*END\n";
        spef::File f = spef::parse(text, "x.spef", 2);
        assert(f.h.design == "top" && f.h.busOpen == '[' && f.h.busClose == ']' && f.h.flow.size() == 2);
        assert(near(f.tScale, 1e-12) && near(f.cScale, 1e-11) && near(f.rScale, 1e3));
        assert(f.ports.size() == 1 && f.ports[0].name == "u1/n[3]" && f.ports[0].dir == 'I');
        assert(f.nets.size() == 2 && f.nets[1].name == "second");
        const spef::Net& n = f.nets[0];
        assert(n.name == "u1/n[3]" && near(n.total, 0.6));
        assert(n.conns.size() == 2 && n.conns[0].port && n.conns[0].hasXY && near(n.conns[0].y, 2.0));
        assert(n.conns[1].name == "u1/buf:A" && near(n.conns[1].load, 0.01) && n.conns[1].cell == "BUF");
        assert(n.caps.size() == 2 && n.caps[0].a == "u1/n[3]:2" && n.caps[0].b.empty() && near(n.caps[0].c, 0.2));
        assert(n.caps[1].b == "other:1" && near(n.caps[1].c, 0.4));
        assert(n.res.size() == 2 && n.res[0].a == "u1/n[3]" && near(n.res[1].r, 7.0));
        assert(near(spef::parse(text, "x.spef", 1, 0).nets[0].total, 0.5));
        assert(near(spef::parse(text, "x.spef", 1, 2).nets[0].caps[0].c, 0.3));
        try { spef::parse("*SPEF \"x\"\n*D_NET a 1\n*CAP\n1 a:1 zz", "bad.spef"); assert(false); }
        catch (const std::runtime_error& e) { assert(std::string(e.what()).find("bad.spef:4") == 0); }
        assert(spef::escape("a.b[1]") == "a\\.b[1]" && spef::escape("a\\[1\\]") == "a\\[1\\]");
    }
    std::puts("test_rcx: all passed");
    return 0;
}
