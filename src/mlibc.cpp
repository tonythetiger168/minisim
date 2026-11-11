// mlibc - cell library checks and reports.
//
//   mlibc check  lib.lib [more.lib ...] [-lef tech.lef -lef cells.lef]
//   mlibc report lib.lib [-cell NAME]
//   mlibc compare slow.lib fast.lib
//
// "check" reads each Liberty file through the shared reader (liberty.hpp)
// and looks for the mistakes that make a library unusable downstream: a
// function that disagrees with its timing arcs, tables that go the wrong
// way, a flop without setup or hold, leakage states that do not cover the
// inputs, a cell whose LEF macro is missing or a different size. "report"
// prints one line per cell. "compare" checks a slow corner against a fast one.
//
// Files are checked on separate threads and the cells of a file in parallel
// chunks (-threads N, default every logical CPU). Findings are collected
// per cell and printed in file and cell order, so the output is the same for
// any thread count.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "lef.hpp"
#include "liberty.hpp"
#include "parallel.hpp"
#include "version.hpp"

using namespace liberty;

namespace {

struct Rule {
    const char* id;
    const char* what;
};
const Rule kRules[] = {
    {"LIB001", "an output pin has no function"},
    {"LIB002", "timing_sense disagrees with the pin's function"},
    {"LIB003", "the function depends on an input that has no timing arc to the output"},
    {"LIB004", "a combinational arc from an input the function does not depend on"},
    {"LIB005", "a delay or transition table decreases as the load grows"},
    {"LIB006", "a negative delay or transition"},
    {"LIB007", "an input pin has no capacitance"},
    {"LIB008", "a sequential cell's data pin has no setup or no hold arc, or its clock pin is not marked"},
    {"LIB009", "a cell has no area"},
    {"LIB010", "an output pin has no max_capacitance"},
    {"LIB011", "a function names something that is not a pin or a state variable"},
    {"LIB012", "leakage_power states overlap, leave an input state out, or there is no cell_leakage_power"},
    {"LIB013", "a stronger drive of the same cell is slower or smaller than a weaker one"},
    {"LIB014", "a table index is not strictly increasing"},
    {"LIB015", "an output pin has no timing arc"},
    {"LEF001", "a cell has no LEF macro"},
    {"LEF002", "the Liberty area is not the LEF macro's size"},
    {"LEF003", "a pin is in the Liberty cell or the LEF macro but not in both, or its direction differs"},
    {"LEF004", "the macro is not a whole number of sites wide, or not one site tall"},
    {"LEF005", "a signal pin has no shape on a routing track crossing"},
    {"CMP001", "a cell or pin is in one corner and not in the other"},
    {"CMP002", "the slow corner is faster than the fast corner"},
    {"CMP003", "area, function or pin capacitance differs between corners"},
};

struct Finding {
    std::string rule, cell, text;
};

struct Options {
    std::set<std::string> disabled;
    int threads = 0;
    bool werror = false;
};

int hwThreads() { return (int)par::defaultThreads(); }

using par::parallelFor;

std::string fmt(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// How an output follows one input, from the function alone.
// 1 positive, -1 negative, 0 both ways, 2 not at all.
int unateness(const Expr& f, const std::vector<std::string>& vars, size_t k) {
    bool up = false, down = false;
    size_t n = vars.size();
    for (uint64_t m = 0; m < (1ull << n); m++) {
        if ((m >> k) & 1) continue;
        auto ev = [&](uint64_t x) {
            return f.eval([&](const std::string& name) {
                for (size_t i = 0; i < n; i++)
                    if (vars[i] == name) return ((x >> i) & 1) != 0;
                return false;
            });
        };
        bool lo = ev(m), hi = ev(m | (1ull << k));
        if (hi && !lo) up = true;
        if (lo && !hi) down = true;
    }
    if (up && down) return 0;
    if (up) return 1;
    if (down) return -1;
    return 2;
}

bool increasing(const std::vector<double>& x) {
    for (size_t i = 1; i < x.size(); i++)
        if (!(x[i] > x[i - 1])) return false;
    return true;
}

// A representative delay of an arc: the mean of rise and fall at the given
// slew (ns) and load (pF).
double arcDelay(const Arc& a, double slew, double load) {
    double s = 0;
    int n = 0;
    if (!a.cellRise.empty()) { s += a.cellRise.lookup(slew, load); n++; }
    if (!a.cellFall.empty()) { s += a.cellFall.lookup(slew, load); n++; }
    return n ? s / n : 0;
}

void checkTable(const Cell& c, const Arc& a, const Table& t, const char* name, bool delayLike,
                std::vector<Finding>& out) {
    if (t.empty()) return;
    auto where = [&] { return std::string(name) + " of " + a.related + " -> " + a.to; };
    if (!increasing(t.idx1) || !increasing(t.idx2))
        out.push_back({"LIB014", c.name, where() + ": index values must increase"});
    if (!delayLike) return;
    size_t n1 = std::max<size_t>(1, t.idx1.size()), n2 = std::max<size_t>(1, t.idx2.size());
    if (t.v.size() != n1 * n2) return;
    for (double v : t.v)
        if (v < 0) {
            out.push_back({"LIB006", c.name, where() + ": " + fmt(v) + " ns"});
            break;
        }
    // along the load axis
    bool loadIs1 = t.var1 == Var::Load, loadIs2 = t.var2 == Var::Load;
    if (!loadIs1 && !loadIs2) return;
    for (size_t i = 0; i < n1; i++)
        for (size_t j = 0; j < n2; j++) {
            double v = t.v[i * n2 + j];
            double nxt = v;
            if (loadIs2 && j + 1 < n2) nxt = t.v[i * n2 + j + 1];
            if (loadIs1 && i + 1 < n1) nxt = t.v[(i + 1) * n2 + j];
            if (nxt < v - 1e-12) {
                out.push_back({"LIB005", c.name, where() + ": " + fmt(v) + " then " + fmt(nxt) + " ns at a larger load"});
                return;
            }
        }
}

void checkCell(const Library& L, const Cell& c, std::vector<Finding>& out) {
    auto add = [&](const char* rule, std::string text) { out.push_back({rule, c.name, std::move(text)}); };
    bool physical = true;   // no signal pins at all: a filler or a decap
    for (auto& p : c.pins) (void)p, physical = false;
    if (c.area <= 0) add("LIB009", "area is " + fmt(c.area));
    if (physical) return;

    std::set<std::string> state;   // IQ, IQN ...
    for (auto& f : c.ffs) { state.insert(f.iq); state.insert(f.iqn); }
    for (auto& l : c.latches) { state.insert(l.iq); state.insert(l.iqn); }
    std::vector<const Pin*> inputs, outputs;
    for (auto& p : c.pins) {
        if (p.dir == Dir::In) inputs.push_back(&p);
        if (p.dir == Dir::Out || p.dir == Dir::InOut) outputs.push_back(&p);
    }
    for (auto* p : inputs)
        if (p->cap <= 0 && p->riseCap <= 0) add("LIB007", "pin " + p->name);

    for (auto* o : outputs) {
        if (!o->hasFunction()) {
            if (!c.group || !c.group->first("statetable")) add("LIB001", "pin " + o->name);
            continue;
        }
        Expr f;
        try {
            f = o->functionExpr();
        } catch (const Error& e) {
            add("LIB011", "pin " + o->name + ": " + e.what());
            continue;
        }
        std::vector<std::string> vars = f.vars();
        bool bad = false, usesState = false;
        for (auto& v : vars) {
            if (state.count(v)) { usesState = true; continue; }
            if (!c.pin(v)) { add("LIB011", "pin " + o->name + ": '" + v + "' in \"" + o->function + "\""); bad = true; }
        }
        bool constant = vars.empty();
        auto arcs = c.arcsTo(o->name);
        if (!constant && arcs.empty()) add("LIB015", "pin " + o->name);
        if (!constant && o->maxCapacitance <= 0) add("LIB010", "pin " + o->name);
        if (bad || usesState || vars.size() > 12) continue;
        // the function against the combinational arcs
        std::map<std::string, std::vector<const Arc*>> byRel;
        for (auto* a : arcs)
            if (a->type == ArcType::Comb && a->typeName.rfind("three_state", 0) != 0) byRel[a->related].push_back(a);
        for (size_t k = 0; k < vars.size(); k++) {
            int u = unateness(f, vars, k);
            auto it = byRel.find(vars[k]);
            if (it == byRel.end()) {
                if (u != 2) add("LIB003", vars[k] + " -> " + o->name);
                continue;
            }
            // Several arcs from one pin (with "when" conditions) may each
            // be unate where the whole function is not; only a single
            // unconditional arc is compared.
            if (it->second.size() != 1 || !it->second[0]->when.empty()) continue;
            const Arc* a = it->second[0];
            int declared = a->sense == Sense::Positive ? 1 : a->sense == Sense::Negative ? -1 : 0;
            if (u != 2 && declared != u) {
                static const char* nm[] = {"negative_unate", "non_unate", "positive_unate"};
                add("LIB002", vars[k] + " -> " + o->name + ": timing_sense is " + nm[declared + 1] + ", \"" + o->function +
                                  "\" is " + nm[u + 1]);
            }
        }
        for (auto& [rel, v] : byRel)
            if (std::find(vars.begin(), vars.end(), rel) == vars.end() && o->threeState.empty())
                add("LIB004", rel + " -> " + o->name + ": \"" + o->function + "\" does not use " + rel);
    }

    for (auto& a : c.arcs) {
        bool d = !a.isCheck() && a.type != ArcType::Other;
        checkTable(c, a, a.cellRise, "cell_rise", d, out);
        checkTable(c, a, a.cellFall, "cell_fall", d, out);
        checkTable(c, a, a.riseTrans, "rise_transition", d, out);
        checkTable(c, a, a.fallTrans, "fall_transition", d, out);
        checkTable(c, a, a.riseCons, "rise_constraint", false, out);
        checkTable(c, a, a.fallCons, "fall_constraint", false, out);
    }

    // Sequential cells: every pin the next state or the latch data reads
    // needs setup and hold against the clock, and the clock is marked.
    auto needChecks = [&](const std::string& expr, const std::string& clockExpr) {
        std::vector<std::string> data, clk;
        try {
            data = parseFunction(expr).vars();
            clk = parseFunction(clockExpr).vars();
        } catch (const Error&) {
            return;
        }
        for (auto& ck : clk) {
            const Pin* p = c.pin(ck);
            if (p && !p->clock) add("LIB008", "pin " + ck + " clocks the cell and has no \"clock : true\"");
        }
        for (auto& dname : data) {
            if (state.count(dname) || !c.pin(dname)) continue;
            bool setup = false, hold = false;
            for (auto& a : c.arcs)
                if (a.to == dname && a.isCheck()) (a.isSetup() ? setup : hold) = true;
            if (!setup || !hold)
                add("LIB008", "pin " + dname + " has no " + (!setup && !hold ? "setup or hold" : !setup ? "setup" : "hold") + " arc");
        }
    };
    for (auto& f : c.ffs) needChecks(f.nextState, f.clockedOn);
    for (auto& l : c.latches)
        if (!l.dataIn.empty() && !l.enable.empty()) needChecks(l.dataIn, l.enable);

    // Leakage: a default value, and when-states that split the input space.
    bool hasDefault = c.group && c.group->has("cell_leakage_power");
    std::vector<const Leakage*> whens;
    for (auto& l : c.leakages) {
        if (l.when.empty()) hasDefault = true;
        else whens.push_back(&l);
    }
    if (!hasDefault) add("LIB012", "no cell_leakage_power");
    if (!whens.empty() && inputs.size() <= 10) {
        std::vector<std::string> names;
        for (auto* p : inputs) names.push_back(p->name);
        std::vector<Expr> ex;
        bool ok = true;
        for (auto* l : whens) {
            try {
                ex.push_back(parseFunction(l->when));
            } catch (const Error& e) {
                add("LIB012", std::string("when \"") + l->when + "\": " + e.what());
                ok = false;
            }
        }
        size_t missing = 0, twice = 0;
        for (uint64_t m = 0; ok && m < (1ull << names.size()); m++) {
            int hits = 0;
            for (auto& e : ex)
                hits += e.eval([&](const std::string& n) {
                    for (size_t i = 0; i < names.size(); i++)
                        if (names[i] == n) return ((m >> i) & 1) != 0;
                    return false;
                });
            if (hits == 0) missing++;
            if (hits > 1) twice++;
        }
        // A library may give a few states and leave the rest to the default.
        if (twice) add("LIB012", std::to_string(twice) + " input state(s) match more than one leakage_power group");
        if (missing && !c.group->has("cell_leakage_power"))
            add("LIB012", std::to_string(missing) + " input state(s) have no leakage_power group and there is no default");
    }
}

// Cells that differ only in drive: NAME_X<n>. Stronger must be faster at a
// heavy load and no smaller.
void checkDrives(const Library& L, std::vector<Finding>& out) {
    std::map<std::string, std::vector<std::pair<int, const Cell*>>> fam;
    for (auto& c : L.cells) {
        size_t p = c.name.rfind("_X");
        if (p == std::string::npos || p + 2 >= c.name.size()) continue;
        std::string d = c.name.substr(p + 2);
        if (d.find_first_not_of("0123456789") != std::string::npos) continue;
        fam[c.name.substr(0, p)].push_back({std::atoi(d.c_str()), &c});
    }
    for (auto& [name, v] : fam) {
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 1; i < v.size(); i++) {
            const Cell& w = *v[i - 1].second;
            const Cell& s = *v[i].second;
            if (s.area < w.area) out.push_back({"LIB013", s.name, "area " + fmt(s.area) + " is less than " + w.name + "'s " + fmt(w.area)});
            // the same arc in both, at the weaker cell's largest load
            for (auto& a : w.arcs) {
                if (a.isCheck() || a.type == ArcType::Other || a.cellRise.empty()) continue;
                const Arc* b = nullptr;
                for (auto& x : s.arcs)
                    if (x.related == a.related && x.to == a.to && x.typeName == a.typeName && x.when == a.when) { b = &x; break; }
                if (!b) continue;
                const Table& t = a.cellRise;
                const std::vector<double>& loads = t.var1 == Var::Load ? t.idx1 : t.idx2;
                const std::vector<double>& slews = t.var1 == Var::Load ? t.idx2 : t.idx1;
                if (loads.empty() || slews.empty()) continue;
                double load = loads.back(), slew = slews[slews.size() / 2];
                double dw = arcDelay(a, slew, load), ds = arcDelay(*b, slew, load);
                if (ds > dw * 1.0001) {
                    out.push_back({"LIB013", s.name, a.related + " -> " + a.to + ": " + fmt(ds) + " ns, " + w.name + " takes " +
                                                         fmt(dw) + " ns at " + fmt(load) + " pF"});
                    break;
                }
            }
        }
    }
}

void checkLef(const Library& L, const Cell& c, const phys::Tech& tech, std::vector<Finding>& out) {
    auto add = [&](const char* rule, std::string text) { out.push_back({rule, c.name, std::move(text)}); };
    const phys::Macro* m = tech.macro(c.name);
    if (!m) { add("LEF001", ""); return; }
    double um2 = (double)m->w / tech.dbu * (double)m->h / tech.dbu;
    if (c.area > 0 && std::fabs(um2 - c.area) > 0.005 * c.area)
        add("LEF002", "area " + fmt(c.area) + ", the macro is " + fmt((double)m->w / tech.dbu) + " x " + fmt((double)m->h / tech.dbu) +
                          " = " + fmt(um2));
    auto st = tech.sites.find(m->site);
    if (st != tech.sites.end() && st->second.w > 0) {
        if (m->w % st->second.w != 0 || m->h != st->second.h)
            add("LEF004", fmt((double)m->w / tech.dbu) + " x " + fmt((double)m->h / tech.dbu) + " on site " + m->site + " " +
                              fmt((double)st->second.w / tech.dbu) + " x " + fmt((double)st->second.h / tech.dbu));
    }
    for (auto& p : c.pins) {
        if (p.dir == Dir::Internal) continue;        // a state node, not a terminal
        const phys::MacroPin* mp = m->pin(p.name);
        if (!mp) { add("LEF003", "pin " + p.name + " is not in the macro"); continue; }
        std::string want = p.dir == Dir::In ? "INPUT" : p.dir == Dir::Out ? "OUTPUT" : "INOUT";
        if (mp->dir.rfind(want, 0) != 0) add("LEF003", "pin " + p.name + " is " + mp->dir + " in the macro");
    }
    // The first horizontal and the first vertical routing layer give the
    // track grid the pins must sit on.
    const phys::Layer *h = nullptr, *v = nullptr;
    for (auto* l : tech.routing()) {
        if (l->dir == 'H' && !h) h = l;
        if (l->dir == 'V' && !v) v = l;
    }
    for (auto& mp : m->pins) {
        if (mp.use == "POWER" || mp.use == "GROUND") continue;
        if (!c.pin(mp.name)) add("LEF003", "pin " + mp.name + " is not in the Liberty cell");
        if (!h || !v || h->pitch() <= 0 || v->pitch() <= 0) continue;
        bool on = false;
        for (auto& s : mp.ports) {
            // a track at offset + k*pitch inside [lo, hi]
            auto hasTrack = [](int64_t lo, int64_t hi, int64_t off, int64_t pitch) {
                int64_t k = (lo - off + pitch - 1) / pitch;
                if (lo - off < 0) k = -((off - lo) / pitch);
                return off + k * pitch <= hi;
            };
            if (hasTrack(s.r.x0, s.r.x1, v->offset(), v->pitch()) && hasTrack(s.r.y0, s.r.y1, h->offset(), h->pitch())) on = true;
        }
        if (!on) add("LEF005", "pin " + mp.name);
    }
}

struct FileResult {
    std::string path, error;
    Library lib;
    std::vector<std::vector<Finding>> perCell;
    std::vector<Finding> whole;
};

void usage() {
    std::cout <<
        "mlibc - cell library checks and reports\n"
        "  mlibc check lib.lib [more.lib ...] [-lef file.lef ...]   check Liberty files, and against LEF\n"
        "  mlibc report lib.lib [-cell NAME]                        one line per cell, or one cell in full\n"
        "  mlibc compare slow.lib fast.lib                          a slow corner against a fast one\n"
        "options:\n"
        "  -threads N      worker threads (default: every logical CPU; 1 for none)\n"
        "  -disable RULE   leave a rule out (repeatable)\n"
        "  -Werror         exit 1 when anything is found\n"
        "  -list           list the rules\n"
        "  -version\n";
}

int doCheck(const std::vector<std::string>& libs, const std::vector<std::string>& lefs, const Options& opt) {
    phys::Tech tech;
    try {
        for (auto& f : lefs) phys::readLef(f, tech);
    } catch (const std::exception& e) {
        std::cerr << "mlibc: " << e.what() << "\n";
        return 2;
    }
    int threads = opt.threads > 0 ? opt.threads : hwThreads();
    std::vector<FileResult> res(libs.size());
    // Files first, each on its own thread; then the cells of all files.
    parallelFor(libs.size(), threads, [&](size_t i) {
        res[i].path = libs[i];
        try {
            res[i].lib = liberty::load(libs[i]);
        } catch (const std::exception& e) {
            res[i].error = e.what();
        }
    });
    struct Job { size_t file, cell; };
    std::vector<Job> jobs;
    for (size_t i = 0; i < res.size(); i++) {
        res[i].perCell.resize(res[i].lib.cells.size());
        for (size_t k = 0; k < res[i].lib.cells.size(); k++) jobs.push_back({i, k});
    }
    parallelFor(jobs.size(), threads, [&](size_t j) {
        FileResult& r = res[jobs[j].file];
        const Cell& c = r.lib.cells[jobs[j].cell];
        checkCell(r.lib, c, r.perCell[jobs[j].cell]);
        if (!lefs.empty()) checkLef(r.lib, c, tech, r.perCell[jobs[j].cell]);
    });
    parallelFor(res.size(), threads, [&](size_t i) {
        if (res[i].error.empty()) checkDrives(res[i].lib, res[i].whole);
    });

    size_t total = 0;
    int rc = 0;
    for (auto& r : res) {
        if (!r.error.empty()) {
            std::cerr << "mlibc: " << r.error << "\n";
            rc = 2;
            continue;
        }
        size_t n = 0;
        auto print = [&](const Finding& f) {
            if (opt.disabled.count(f.rule)) return;
            std::cout << r.path << ": " << f.cell << ": [" << f.rule << "] ";
            for (auto& k : kRules)
                if (f.rule == k.id) std::cout << k.what;
            if (!f.text.empty()) std::cout << ": " << f.text;
            std::cout << "\n";
            n++;
        };
        for (auto& v : r.perCell)
            for (auto& f : v) print(f);
        for (auto& f : r.whole) print(f);
        std::cout << r.path << ": " << r.lib.cells.size() << " cells, " << n << (n == 1 ? " finding" : " findings") << "\n";
        total += n;
    }
    if (rc) return rc;
    return (opt.werror && total) ? 1 : 0;
}

int doReport(const std::string& path, const std::string& only) {
    Library L;
    try {
        L = liberty::load(path);
    } catch (const std::exception& e) {
        std::cerr << "mlibc: " << e.what() << "\n";
        return 2;
    }
    std::cout << "library " << L.name << ": " << L.cells.size() << " cells, " << fmt(L.nomVoltage) << " V, " << fmt(L.nomTemperature)
              << " C, leakage in " << (L.leakagePowerUnit.empty() ? "library units" : L.leakagePowerUnit) << "\n";
    int nameW = 14, leakW = 9;                        // widened to the longest cell name and value
    for (auto& c : L.cells) {
        nameW = std::max(nameW, (int)c.name.size());
        leakW = std::max(leakW, (int)fmt(c.leakage).size());
    }
    if (only.empty())
        std::printf("%-*s %8s %*s %4s %9s %9s  %s\n", nameW, "cell", "area", leakW, "leakage", "pins", "cap pF", "delay ns", "function");
    double area = 0;
    for (auto& c : L.cells) {
        area += c.area;
        if (!only.empty() && c.name != only) continue;
        // delay: the slowest arc at the middle of its table
        double worst = 0, cap = 0;
        for (auto& a : c.arcs) {
            if (a.isCheck() || a.type == ArcType::Other || a.cellRise.empty()) continue;
            const Table& t = a.cellRise;
            const auto& loads = t.var1 == Var::Load ? t.idx1 : t.idx2;
            const auto& slews = t.var1 == Var::Load ? t.idx2 : t.idx1;
            if (loads.empty() || slews.empty()) continue;
            worst = std::max(worst, arcDelay(a, slews[slews.size() / 2], loads[loads.size() / 2]));
        }
        std::string fn;
        for (auto& p : c.pins) {
            if (p.dir == Dir::In) cap = std::max(cap, p.cap);
            if (p.hasFunction()) fn += (fn.empty() ? "" : "  ") + p.name + "=" + p.function;
        }
        if (!c.ffs.empty()) fn += "  [ff " + c.ffs[0].clockedOn + ": " + c.ffs[0].nextState + "]";
        if (!c.latches.empty()) fn += "  [latch " + c.latches[0].enable + ": " + c.latches[0].dataIn + "]";
        if (only.empty()) {
            std::printf("%-*s %8s %*s %4zu %9s %9s  %s\n", nameW, c.name.c_str(), fmt(c.area).c_str(), leakW, fmt(c.leakage).c_str(), c.pins.size(),
                        fmt(cap).c_str(), fmt(worst).c_str(), fn.c_str());
            continue;
        }
        std::cout << "cell " << c.name << "\n  area " << fmt(c.area) << "\n  leakage " << fmt(c.leakage);
        for (auto& l : c.leakages)
            if (!l.when.empty()) std::cout << "\n    when " << l.when << ": " << fmt(l.value);
        std::cout << "\n";
        for (auto& p : c.pins) {
            std::cout << "  pin " << p.name << " " << (p.dir == Dir::In ? "input" : p.dir == Dir::Out ? "output" : "inout");
            if (p.dir == Dir::In) std::cout << " cap " << fmt(p.cap) << " pF";
            if (p.clock) std::cout << " clock";
            if (p.hasFunction()) std::cout << " function \"" << p.function << "\"";
            if (!p.threeState.empty()) std::cout << " three_state \"" << p.threeState << "\"";
            if (p.maxCapacitance > 0) std::cout << " max_capacitance " << fmt(p.maxCapacitance) << " pF";
            std::cout << "\n";
        }
        for (auto& a : c.arcs) {
            std::cout << "  arc " << a.related << " -> " << a.to << " " << a.typeName;
            if (!a.when.empty()) std::cout << " when " << a.when;
            auto range = [&](const char* nm, const Table& t) {
                if (t.empty()) return;
                auto mm = std::minmax_element(t.v.begin(), t.v.end());
                std::cout << "  " << nm << " " << fmt(*mm.first) << ".." << fmt(*mm.second);
            };
            range("rise", a.cellRise);
            range("fall", a.cellFall);
            range("rise_constraint", a.riseCons);
            range("fall_constraint", a.fallCons);
            std::cout << "\n";
        }
    }
    if (only.empty()) std::cout << "total area " << fmt(area) << "\n";
    else if (!L.cell(only)) { std::cerr << "mlibc: no cell " << only << " in " << path << "\n"; return 1; }
    return 0;
}

int doCompare(const std::string& slowPath, const std::string& fastPath, const Options& opt) {
    Library S, F;
    try {
        parallelFor(2, opt.threads > 0 ? opt.threads : hwThreads(), [&](size_t i) {
            (i ? F : S) = liberty::load(i ? fastPath : slowPath);
        });
    } catch (const std::exception& e) {
        std::cerr << "mlibc: " << e.what() << "\n";
        return 2;
    }
    int threads = opt.threads > 0 ? opt.threads : hwThreads();
    std::vector<std::vector<Finding>> per(S.cells.size());
    parallelFor(S.cells.size(), threads, [&](size_t i) {
        const Cell& s = S.cells[i];
        auto add = [&](const char* rule, std::string text) { per[i].push_back({rule, s.name, std::move(text)}); };
        const Cell* f = F.cell(s.name);
        if (!f) { add("CMP001", "not in " + fastPath); return; }
        if (std::fabs(s.area - f->area) > 1e-9) add("CMP003", "area " + fmt(s.area) + " and " + fmt(f->area));
        for (auto& p : s.pins) {
            const Pin* q = f->pin(p.name);
            if (!q) { add("CMP001", "pin " + p.name + " is not in " + fastPath); continue; }
            if (p.function != q->function) add("CMP003", "pin " + p.name + ": \"" + p.function + "\" and \"" + q->function + "\"");
        }
        for (auto& p : f->pins)
            if (!s.pin(p.name)) add("CMP001", "pin " + p.name + " is not in " + slowPath);
        for (auto& a : s.arcs) {
            if (a.isCheck() || a.type == ArcType::Other || a.cellRise.empty()) continue;
            for (auto& b : f->arcs) {
                if (b.related != a.related || b.to != a.to || b.typeName != a.typeName || b.when != a.when) continue;
                const Table& t = a.cellRise;
                const auto& loads = t.var1 == Var::Load ? t.idx1 : t.idx2;
                const auto& slews = t.var1 == Var::Load ? t.idx2 : t.idx1;
                if (loads.empty() || slews.empty()) break;
                double slew = slews[slews.size() / 2], load = loads[loads.size() / 2];
                double ds = arcDelay(a, slew, load), df = arcDelay(b, slew, load);
                if (ds < df) add("CMP002", a.related + " -> " + a.to + ": " + fmt(ds) + " ns slow, " + fmt(df) + " ns fast");
                break;
            }
        }
    });
    size_t n = 0;
    auto print = [&](const Finding& f) {
        if (opt.disabled.count(f.rule)) return;
        std::cout << f.cell << ": [" << f.rule << "] ";
        for (auto& k : kRules)
            if (f.rule == k.id) std::cout << k.what;
        if (!f.text.empty()) std::cout << ": " << f.text;
        std::cout << "\n";
        n++;
    };
    for (auto& v : per)
        for (auto& f : v) print(f);
    for (auto& c : F.cells)
        if (!S.cell(c.name)) print({"CMP001", c.name, "not in " + slowPath});
    // the ratio over the whole library, as a summary
    double sum = 0;
    int cnt = 0;
    for (auto& s : S.cells) {
        const Cell* f = F.cell(s.name);
        if (!f) continue;
        for (auto& a : s.arcs) {
            if (a.isCheck() || a.type == ArcType::Other || a.cellRise.empty()) continue;
            for (auto& b : f->arcs)
                if (b.related == a.related && b.to == a.to && b.typeName == a.typeName && b.when == a.when && !b.cellRise.empty()) {
                    double x = a.cellRise.v[a.cellRise.v.size() / 2], y = b.cellRise.v[b.cellRise.v.size() / 2];
                    if (y > 0) { sum += x / y; cnt++; }
                    break;
                }
        }
    }
    std::cout << S.cells.size() << " cells, " << cnt << " arcs compared";
    if (cnt) std::cout << ", slow/fast delay ratio " << fmt(std::round(sum / cnt * 1000) / 1000);
    std::cout << ", " << n << (n == 1 ? " finding" : " findings") << "\n";
    return (opt.werror && n) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> pos, lefs;
    std::string cell;
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::cerr << "mlibc: " << a << " needs " << what << "\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        if (a == "-version" || a == "--version") { std::cout << "mlibc " << MINISIM_VERSION << "\n"; return 0; }
        if (a == "-list") {
            for (auto& r : kRules) std::cout << r.id << "  " << r.what << "\n";
            return 0;
        }
        if (a == "-lef") { lefs.push_back(next("a LEF file")); continue; }
        if (a == "-cell") { cell = next("a cell name"); continue; }
        if (a == "-disable") { opt.disabled.insert(next("a rule")); continue; }
        if (a == "-Werror") { opt.werror = true; continue; }
        if (a == "-threads" && i + 1 < argc && std::string(argv[i + 1]) == "all") {
            opt.threads = hwThreads();
            i++;
            continue;
        }
        if (unsigned t = 0; par::threadsArg(argc, argv, i, t, "mlibc")) { opt.threads = (int)t; continue; }
        if (!a.empty() && a[0] == '-') { std::cerr << "mlibc: unknown option " << a << "\n"; return 2; }
        pos.push_back(a);
    }
    if (pos.empty()) { usage(); return 2; }
    std::string cmd = pos[0];
    pos.erase(pos.begin());
    if (cmd == "check") {
        if (pos.empty()) { std::cerr << "mlibc: check needs a Liberty file\n"; return 2; }
        return doCheck(pos, lefs, opt);
    }
    if (cmd == "report") {
        if (pos.size() != 1) { std::cerr << "mlibc: report needs one Liberty file\n"; return 2; }
        return doReport(pos[0], cell);
    }
    if (cmd == "compare") {
        if (pos.size() != 2) { std::cerr << "mlibc: compare needs a slow and a fast Liberty file\n"; return 2; }
        return doCompare(pos[0], pos[1], opt);
    }
    std::cerr << "mlibc: unknown command " << cmd << " (check, report, compare)\n";
    return 2;
}
