// clocking.hpp - clocking blocks (IEEE 1800-2023 clause 14), shared by
// lowering (lower.hpp) and the kernel (exec.inc).
//
// A clocking block is a process on its clocking event. At each tick it
// samples its inputs into their clockvars (14.13), matures the synchronous
// drives due at that tick (14.16), and in the observed region triggers its
// clocking block event, which @(cb) and ## wait on (14.10, 14.11).
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "prog.hpp"

struct ClockingRT {
    std::string name;
    std::vector<std::pair<int, Edge>> ev;   // the clocking event
    E iff;                                  // @(posedge clk iff en): null if none
    std::vector<int> evShadow;              // each event signal's sampled value: did it change in this step?
    int evSig = -1;                         // the clocking block event: a trigger count; evSig+1 is its time + 1
    uint64_t ticks = 0;                     // clocking events so far
    uint64_t lastTick = ~0ull;              // time of the last one
    // An input or inout (14.13). kind 0: #1step, `src` reads sampled values
    // (the values just before the time step); 1: #0, sampled in the observed
    // region; 2: #N, the value N ticks before the clocking event, from `hist`.
    struct In {
        E src;
        int cv;          // the clockvar signal
        int kind = 0;
        uint64_t skew = 0;
        std::vector<int> refs;                           // kind 2: the signals src reads
        std::deque<std::pair<uint64_t, Val>> hist;       // kind 2: (time, value), oldest first
        // kind 3: an edge skew (input negedge): the value at the last such
        // edge of the clock signal edgeSig before the clocking event (14.3);
        // with a delay (input negedge #N), the value N ticks before that edge,
        // from hist as for kind 2.
        int edgeSig = -1;
        int edge = 0;
        uint64_t lastAt = ~0ull;
        Val last;
    };
    std::vector<In> ins;
    // An output or inout (14.16): its clockvar's bits go to pieces of signals,
    // lowest bits first ({a, b[3:0]} is b[3:0] then a).
    // A net target is driven through a hidden variable `sig` (starting at
    // Z) and a driver on `net`, bits [netLo + w - 1 : netLo] (14.16).
    struct Piece { int sig; int lo; int w; int net = -1; int netLo = 0; };
    // An output with an edge skew (output negedge #1) is driven at the next
    // such edge of the clock signal edgeSig, then skew ticks later (14.3).
    struct Out { int cv; int w; std::vector<Piece> pieces; uint64_t skew = 0; int edgeSig = -1; int edge = 0; };
    std::vector<Out> outs;
    // Drives waiting for a later clocking event: at tick `tick`, bits
    // [hi:lo] of output `out` (hi < 0: all of it) take `v`.
    struct Drive { uint64_t tick; int out; Val v; int hi, lo; };
    std::vector<Drive> drives;
};
inline std::vector<ClockingRT>& clockings() { static std::vector<ClockingRT> v; return v; }
