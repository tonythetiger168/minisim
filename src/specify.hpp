// Specify path delays (IEEE 1800-2023 30.4, 30.5) and what SDF
// back-annotation (32) needs to find in the design: the instances and their
// cells, the ports, the paths and the specparams. Filled while lowering;
// the run-time path delay selection is in minisim.cpp (Sim::pathDelay).
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "prog.hpp"

// The 12 transitions of a path or interconnect delay (30.5.1, Table 30-2),
// in the order of Table 32-4.
enum { T01, T10, T0Z, TZ1, T1Z, TZ0, T0X, TX1, T1X, TX0, TXZ, TZX };

// The delay index of the transition from level f to level t (0, 1, 2 = x, 3 = z).
inline int transIdx(int f, int t) {
    static const int8_t tab[4][4] = {
        {-1, T01, T0X, T0Z}, {T10, -1, T1X, T1Z}, {TX0, TX1, -1, TXZ}, {TZ0, TZ1, TZX, -1}};
    return tab[f & 3][t & 3];
}

// n path delay values (1, 2, 3, 6 or 12) as all 12 transitions: Table
// 30-2, then the x transitions of Table 30-3.
inline std::vector<uint64_t> expand12(const std::vector<uint64_t>& v) {
    std::vector<uint64_t> d(12, 0);
    size_t n = v.size();
    if (n == 0) return d;
    if (n == 12) return v;
    if (n == 1) { std::fill(d.begin(), d.end(), v[0]); return d; }
    if (n == 2 || n == 3) {
        uint64_t r = v[0], f = v[1], z = n == 3 ? v[2] : 0;
        d[T01] = r; d[T10] = f; d[TZ1] = r; d[TZ0] = f;
        d[T0Z] = n == 3 ? z : r;
        d[T1Z] = n == 3 ? z : f;
    } else {   // 6
        for (int k = 0; k < 6; k++) d[k] = v[(size_t)k];
    }
    d[T0X] = std::min(d[T0Z], d[T01]);
    d[T1X] = std::min(d[T1Z], d[T10]);
    d[TZX] = std::min(d[TZ1], d[TZ0]);
    d[TX0] = std::max(d[TZ0], d[T10]);
    d[TX1] = std::max(d[TZ1], d[T01]);
    d[TXZ] = std::max(d[T1Z], d[T0Z]);
    return d;
}

// One input-to-output pair of a module path. A path declared with several
// inputs or outputs ((a, b *> y)) is one PathRec per pair.
struct PathRec {
    std::string inst;            // the instance, "top.u1"
    std::string in, out;         // the port names as written ("a", "d[3]")
    std::string cond;            // if (cond): the expression, spaces removed
    bool hasCond = false, ifnone = false;
    uint8_t edge = 0;            // 0, 1 posedge, 2 negedge, 3 edge
    int src = -1, dst = -1;      // signals
    E condE;                     // the condition, evaluated when dst changes
    uint64_t d[12] = {};         // ticks, one per transition
    uint64_t unit = 1;           // ticks per time unit of the module
    bool annotated = false;
};
inline std::vector<PathRec>& pathRecs() { static std::vector<PathRec> v; return v; }

// A port of an instance: the formal signal inside it.
struct PortRec {
    int sig = -1;
    int dir = 0;                 // 0 input, 1 output, 2 inout
    bool net = true;
};
inline std::map<std::string, PortRec>& portRecs() { static std::map<std::string, PortRec> m; return m; }   // "top.u1.a"
// Every instance and the cell (module) it is.
inline std::map<std::string, std::string>& instCells() { static std::map<std::string, std::string> m; return m; }
// The specparams an SDF LABEL (32.4.3) gave a value: "top.u1.tpd".
inline std::map<std::string, double>& labelledNames() { static std::map<std::string, double> m; return m; }

// A $sdf_annotate call (32.9) whose arguments are known when the design is
// lowered, or one given on the command line (msim -sdf): annotated before
// time 0.
struct SdfRequest {
    std::string file, scope, config, log, mtm, scale, scaleType;
    std::string where;           // "file.sv:12" of the call, for messages
    bool cli = false;            // msim -sdf
};
inline std::vector<SdfRequest>& sdfRequests() { static std::vector<SdfRequest> v; return v; }
// +mindelays / +typdelays / +maxdelays (and $sdf_annotate's TOOL_CONTROL): 0, 1, 2.
inline int& mtmSelect() { static int m = 1; return m; }
// A $sdf_annotate whose arguments are only known when it runs.
inline bool& sdfAtRunTime() { static bool b = false; return b; }
