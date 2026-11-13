// cover.hpp - covergroups (IEEE 1800-2023 clause 19), shared by lowering
// (lower_cover.inc) and the kernel (cover_rt.inc).
//
// Lowering turns each covergroup type into a CgDef: the coverpoint and
// cross expressions, and recipes for their bins whose bounds are
// expressions (they may read the constructor's arguments, 19.5.1). The
// kernel builds each instance's bins when `new` runs, samples instances,
// and computes coverage (19.11).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "prog.hpp"
#include "sva_engine.hpp"

// ---- definitions (from lowering) ----
// One element of a covergroup_range_list: a value, or [lo:hi] where a null
// bound is $ (the type's minimum or maximum).
struct CgItem {
    E lo, hi;                      // 64 bits; signed bounds are sign-extended
    bool range = false;
    bool loMin = false, hiMax = false;
    bool loSgn = false, hiSgn = false;
    // A real coverpoint (19.5.1): lo and hi are reals; tol 1 is [lo +/- hi],
    // 2 is [lo +%- hi] (a tolerance range, 11.4.13).
    int tol = 0;
};
// One step of a transition (19.5.2): a set of values, repeated.
struct CgStep {
    std::vector<CgItem> items;
    int rep = 0;   // 0 none, 1 [*], 2 [->], 3 [=]
    E from, to;    // repeat counts; to null: the same as from
};
// Coverage options (19.7): the members an option assignment can set.
enum CgOpt : int {
    CO_WEIGHT, CO_GOAL, CO_AT_LEAST, CO_AUTO_BIN_MAX, CO_PER_INSTANCE, CO_GET_INST_COVERAGE,
    CO_CROSS_NUM_PRINT_MISSING, CO_DETECT_OVERLAP, CO_NAME, CO_COMMENT,
    CO_TYPE_WEIGHT, CO_TYPE_GOAL, CO_TYPE_STROBE, CO_TYPE_MERGE, CO_TYPE_DISTRIBUTE_FIRST, CO_TYPE_COMMENT, CO_N
};
struct CgOptSet { int which; E e; };
struct CgBinDef {
    std::string name;
    int kind = 0;                 // 0 bins, 1 ignore_bins, 2 illegal_bins
    bool array = false;           // name[] or name[N]
    E count;                      // N of name[N]; null for name[]
    bool wildcard = false, dflt = false, dfltSeq = false;
    bool allValues = false;       // `bins b[] = cp with (...)`: every value of the coverpoint
    std::vector<CgItem> items;    // value bins
    E with; int itemSig = -1;     // with (expr on item)
    // bins b = set; (19.5.1.2): the values of a container, read when the
    // bins are built. setItem reads element setIdx of it; setSize its size.
    int setIdx = -1; E setSize; CgItem setItem;
    std::vector<std::vector<CgStep>> trans;   // transition bins: each a sequence of steps
    E iff;
};
struct CgCpDef {
    std::string name;
    E expr, iff;
    // Temporaries the expression and iff read, set first at each sample
    // (an element of a container property through a handle: tr.data[0]).
    std::vector<std::pair<int, E>> pre, preIff;
    // ... or, when the expression calls a function or method (tr.ones() > 0),
    // the code that computes those temporaries, run to its end first.
    std::shared_ptr<Prog> preProg, preIffProg;
    int w = 1;
    bool sgn = false;
    std::vector<std::pair<std::string, uint64_t>> enumVals;   // an enum coverpoint's values
    bool isEnum = false;
    bool isReal = false;        // a real coverpoint: keys order the doubles (19.5.1)
    // Wider than 64 bits: explicit value bins only. A key is the value's
    // place among the sorted bin bounds of the instance (CgCpInst::wb):
    // 2i+1 on bound i, 2i between bounds i-1 and i.
    bool wide = false;
    E realInterval;             // type_option.real_interval; null: the group's, else 1.0
    std::vector<CgBinDef> bins;
    std::vector<CgOptSet> opts;
};
// A cross bin's select expression (19.6.1), as a tree.
struct CgSel {
    int kind = 0;   // 0 binsof(cp[.bin]) [intersect items], 1 !, 2 &&, 3 ||, 4 every tuple (the cross's name),
                    // 5 kids[0] with (expr) [matches n] (19.6.1.2)
    E with, matches;          // kind 5: matches null is 1; a matches of $ sets all
    bool all = false;
    std::vector<int> valSigs;   // kind 5: each coverpoint's value, as the with expression reads it
    int cp = -1;    // the index in the cross's coverpoint list
    std::string bin;   // empty: any bin of cp
    std::vector<CgItem> isect;
    std::vector<std::shared_ptr<CgSel>> kids;
};
struct CgCrossBinDef {
    std::string name;
    int kind = 0;   // 0 bins, 1 ignore_bins, 2 illegal_bins
    std::shared_ptr<CgSel> sel;
};
struct CgCrossDef {
    std::string name;
    std::vector<int> cps;   // coverpoint indices in the group
    E iff;
    std::vector<CgCrossBinDef> bins;
    std::vector<CgOptSet> opts;
};
struct CgDef {
    std::string name;               // the covergroup type, e.g. top.cg
    bool prot = false;              // declared in protected code (34): kept out of the coverage database
    std::vector<int> argSigs;       // constructor arguments: hidden signals
    std::vector<char> argRef;       // ref arguments: bound to a signal per instance
    std::vector<int> sampleSigs;    // `with function sample(...)` arguments
    std::vector<std::pair<int, Edge>> ev;   // the sampling event, if any
    E evIff;
    std::vector<CgOptSet> opts;
    E realInterval;                 // type_option.real_interval of the group (19.7.1)
    std::vector<CgCpDef> cps;
    std::vector<CgCrossDef> crosses;
    std::vector<int> insts;         // instance ids, in creation order
    bool inClass = false;           // embedded in a class (19.4): new() passes `this` last
};
inline std::vector<CgDef>& cgDefs() { static std::vector<CgDef> v; return v; }

// ---- instances (built by the kernel) ----
// Coverpoint values are compared as keys: the value's w bits, with the sign
// bit flipped for a signed coverpoint, so key order is value order.
struct CgBin {
    std::string name, base;          // base: the declared name (b of b[3]), for binsof (19.6.1)
    bool dseq = false;               // default sequence (19.5.2)
    int kind = 0;   // 0 bins, 1 ignore, 2 illegal, 3 default
    std::vector<std::pair<uint64_t, uint64_t>> ranges;   // keys, inclusive
    std::vector<std::pair<uint64_t, uint64_t>> wild;     // (care mask, value): wildcard bins
    std::vector<Val> xvals;          // values with x/z, matched with === (19.5.4)
    const sva::Node* trans = nullptr;   // a transition bin
    E iff;
    bool empty = false;              // no value left: not counted (19.11.1)
    uint64_t hits = 0;
    // transition attempts in flight: (term, count)
    std::vector<std::pair<const sva::Node*, uint64_t>> att;
};
struct CgCpInst {
    std::vector<CgBin> bins;
    std::vector<Val> wb;             // a wide coverpoint: its bin bounds, sorted and distinct
    bool autoBins = false;
    std::vector<std::vector<std::pair<uint64_t, uint64_t>>> atoms;   // transition step value sets
    std::vector<int> hit;            // the bins this sample hit (for crosses)
    bool sampled = false;            // this sample counted (iff true, no x)
};
struct CgCrossInst {
    std::vector<std::vector<int>> dims;   // per coverpoint: the bins that take part
    std::vector<int> cls;            // per tuple: -1 auto, -2 ignored/illegal, or a user bin
    std::vector<std::vector<int>> userOf;   // per tuple: every user bin holding it
    std::vector<std::vector<int>> pos;      // per coverpoint: bin index -> position in dims (-1: none)
    std::vector<uint64_t> autoHits;  // per tuple
    struct UBin { std::string name; int kind; uint64_t hits = 0; size_t size = 0; };
    std::vector<UBin> ubins;
    size_t bb = 0;                   // tuples in user-defined bins (Bb, 19.11.2)
};
struct CgInst {
    int def = -1;
    std::string name;
    std::vector<Val> args;
    std::vector<int> refSigs;
    bool on = true;
    int64_t opt[CO_N] = {};
    std::vector<std::vector<int64_t>> cpOpt, crOpt;   // per coverpoint / cross: weight, goal, at_least, auto_bin_max
    std::vector<CgCpInst> cps;
    std::vector<CgCrossInst> crosses;
    FrameP self;                     // a class's covergroup (19.4): the object it samples
};
inline std::vector<CgInst>& cgInsts() { static std::vector<CgInst> v; return v; }

// ---- coverage computation (19.11) ----
// (covered, total) of a coverpoint, at an at_least threshold.
inline std::pair<double, double> cgCpCount(const CgCpInst& c, int64_t atLeast) {
    double cov = 0, tot = 0;
    for (const auto& b : c.bins) {
        if (b.kind != 0 || b.empty) continue;
        tot++;
        if ((int64_t)b.hits >= std::max<int64_t>(atLeast, 1)) cov++;
    }
    return {cov, tot};
}
inline std::pair<double, double> cgCrossCount(const CgCrossInst& x, int64_t atLeast) {
    double cov = 0, tot = 0;
    int64_t al = std::max<int64_t>(atLeast, 1);
    for (size_t t = 0; t < x.cls.size(); t++)
        if (x.cls[t] == -1) { tot++; if ((int64_t)x.autoHits[t] >= al) cov++; }
    for (const auto& u : x.ubins)
        if (u.kind == 0 && u.size) { tot++; if ((int64_t)u.hits >= al) cov++; }
    return {cov, tot};
}
// An item's coverage in percent; a zero denominator gives 0 (or 100 with weight 0).
inline double cgPct(std::pair<double, double> c, int64_t weight) {
    if (c.second == 0) return weight ? 0.0 : 100.0;
    return 100.0 * c.first / c.second;
}
inline double cgInstCoverage(const CgInst& in) {
    double num = 0, den = 0;
    for (size_t k = 0; k < in.cps.size(); k++) {
        auto c = cgCpCount(in.cps[k], in.cpOpt[k][2]);
        if (c.second == 0) continue;
        num += in.cpOpt[k][0] * (c.first / c.second);
        den += in.cpOpt[k][0];
    }
    for (size_t k = 0; k < in.crosses.size(); k++) {
        auto c = cgCrossCount(in.crosses[k], in.crOpt[k][2]);
        if (c.second == 0) continue;
        num += in.crOpt[k][0] * (c.first / c.second);
        den += in.crOpt[k][0];
    }
    if (den == 0) return in.opt[CO_WEIGHT] ? 0.0 : 100.0;
    return 100.0 * num / den;
}
