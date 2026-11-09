// sva.hpp - assertions (IEEE 1800-2023 clause 16) and sampled values (16.5.1,
// 16.9.3), shared by lowering (lower.hpp) and the kernel (minisim.cpp).
//
// Every assertion, immediate or concurrent, has an entry in asserts(). The
// entry holds what assertion control (20.11) changes: on/off, lock, and
// whether pass and fail actions run. Lowering creates the entries; the
// kernel reads and changes them.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "prog.hpp"

// assertion_type bits (Table 20-6) and directive_type bits (Table 20-7).
enum AssertType : int {
    AT_CONCURRENT = 1, AT_IMMEDIATE = 2, AT_OBSERVED = 4, AT_FINAL = 8, AT_EXPECT = 16,
    AT_UNIQUE = 32, AT_UNIQUE0 = 64, AT_PRIORITY = 128
};
enum AssertDir : int { AD_ASSERT = 1, AD_COVER = 2, AD_ASSUME = 4 };
// control_type values (Table 20-5).
enum AssertCtlType : int {
    AC_LOCK = 1, AC_UNLOCK, AC_ON, AC_OFF, AC_KILL, AC_PASSON, AC_PASSOFF, AC_FAILON, AC_FAILOFF,
    AC_NONVACON, AC_VACOFF
};

struct AssertInfo {
    std::string name;    // hierarchical name, e.g. top.u1.a1 (16.3: the label names a scope)
    std::string inst;    // the module instance it is in, e.g. top.u1
    int type = AT_IMMEDIATE;
    int dir = AD_ASSERT;
    bool on = true, locked = false;
    bool passOn = true, failOn = true;   // PassOn/PassOff, FailOn/FailOff
    bool vacOn = true;                   // VacuousOff clears it, PassOn sets it
    uint64_t killGen = 0;                // Kill: pending deferred reports and attempts made before it die
    // Deferred assertions (16.4): the action subroutine call as a process,
    // and the argument expressions captured when the assertion is evaluated.
    // In the action, each captured argument is a constant node (a slot); the
    // values are written into the slots just before the action starts.
    const Prog* pass = nullptr;
    const Prog* fail = nullptr;
    std::vector<E> passCap, failCap;
    std::vector<E> passSlot, failSlot;
    // Statistics (16.3: a cover counts evaluations and successes).
    uint64_t evals = 0, passes = 0, fails = 0;
};
inline std::vector<AssertInfo>& asserts() { static std::vector<AssertInfo> v; return v; }

// One $assertcontrol call (20.11). Each target is a scope (an instance path)
// or one assertion's name. Levels count instance levels below a scope: 0 is
// all of them, 1 is the scope itself.
inline void assertControl(int ctl, int atype, int dtype, int levels, const std::vector<std::string>& targets) {
    auto under = [](const std::string& s, const std::string& t) {
        return s.size() > t.size() && s.compare(0, t.size(), t) == 0 && s[t.size()] == '.';
    };
    // 1 for the assertion itself or one in scope t; n for one n-1 instances below t.
    auto depthIn = [&](const AssertInfo& a, const std::string& t) -> int {
        if (a.name == t || a.inst == t) return 1;
        if (under(a.inst, t)) {
            int d = 1;
            for (size_t i = t.size(); i < a.inst.size(); i++) d += a.inst[i] == '.';
            return d;
        }
        if (under(a.name, t)) return 1;   // t is a named block or task inside a.inst
        return 0;
    };
    for (auto& a : asserts()) {
        if (!(a.type & atype) || !(a.dir & dtype)) continue;
        bool hit = targets.empty();
        int depth = 0;
        if (hit) {   // the whole design: levels count from the top-level instances
            depth = 1;
            for (char c : a.inst) depth += c == '.';
        } else
            for (auto& t : targets)
                if (int d = depthIn(a, t)) { hit = true; depth = d; break; }
        if (!hit || (levels > 0 && depth > levels)) continue;
        if (ctl == AC_UNLOCK) { a.locked = false; continue; }
        if (a.locked) continue;
        switch (ctl) {
            case AC_LOCK: a.locked = true; break;
            case AC_ON: a.on = true; break;
            case AC_OFF: a.on = false; break;
            case AC_KILL: a.on = false; a.killGen++; break;
            case AC_PASSON: a.passOn = true; a.vacOn = true; break;
            case AC_PASSOFF: a.passOn = false; a.vacOn = false; break;
            case AC_FAILON: a.failOn = true; break;
            case AC_FAILOFF: a.failOn = false; break;
            case AC_NONVACON: a.passOn = true; break;
            case AC_VACOFF: a.vacOn = false; break;
            default: break;
        }
    }
}

// A clock for sampled value functions (16.9.3). At each tick of `ev` (when
// `gate` is true, $past's expression2), every stage shifts: stage k takes
// the value stage k-1 had, and stage 0 takes the sampled value of its
// expression. A $past(e, n) reads the signal of stage n-1. The shift happens
// at the end of the time slot, so everything in the slot reads the values
// from before the tick (the most recent strictly prior tick, 16.9.3).
struct SampleClock {
    std::vector<std::pair<int, Edge>> ev;
    E gate;                               // null: always
    struct Line { E src; std::vector<int> stages; };
    std::vector<Line> lines;
};

// ---- lowering state for sampled values ----
// Every signal's name and width, by id (lower() records them as it makes them).
inline std::vector<std::pair<std::string, int>>& sigInfo() { static std::vector<std::pair<std::string, int>> v; return v; }
// (signal, shadow) pairs: the shadow is the signal's sampled value (16.5.1).
inline std::vector<std::pair<int, int>>& shadowPairs() { static std::vector<std::pair<int, int>> v; return v; }
inline std::vector<SampleClock>& sampleClocks() { static std::vector<SampleClock> v; return v; }
