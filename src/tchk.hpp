// Timing checks (IEEE 1800-2023 clause 31): the checks a specify block
// declares, as lowered for the kernel. The run-time side is tchk.inc.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "prog.hpp"

enum TcKind {
    TC_SETUP, TC_HOLD, TC_SETUPHOLD, TC_RECOVERY, TC_REMOVAL, TC_RECREM,
    TC_SKEW, TC_TIMESKEW, TC_FULLSKEW, TC_PERIOD, TC_WIDTH, TC_NOCHANGE
};

// Transitions, one bit each (31.5): 01 0x 10 1x x0 x1. z counts as x.
enum : uint8_t {
    TE_01 = 1, TE_0X = 2, TE_10 = 4, TE_1X = 8, TE_X0 = 16, TE_X1 = 32,
    TE_POS = TE_01 | TE_0X | TE_X1, TE_NEG = TE_10 | TE_X0 | TE_1X, TE_ALL = 63
};

// One event argument: a terminal, an optional edge, and an optional
// &&& condition (31.7). edges == 0: any change of the terminal's value (a
// vector changing in any bits is one transition, 31.8).
struct TcEvent {
    E e;
    E cond;
    bool nondet = false;   // the condition is == or !=: x enables the check
    uint8_t edges = 0;
    std::string text;      // the argument as written, for messages
    int sig = -1;          // the signal the terminal reads (31.9 delays it), or -1
    int delayed = -1;      // an explicit delayed signal of it (31.9), or -1
};

struct TimingCheck {
    int kind = TC_SETUP;
    std::string name;            // "$setup"
    TcEvent ev[2];               // the event arguments, in the order written
    int nev = 2;
    int ref = 0, data = 1;       // which of ev[] is the reference / data event
    int64_t lim1 = 0, lim2 = 0;  // ticks
    int64_t thresh = 0;          // $width
    bool eventBased = false, remainActive = false;   // $timeskew, $fullskew
    int notifier = -1;           // signal, or -1
    // $setuphold / $recrem (31.9.2): conditions on the event that comes
    // first (the timestamp) and the one that comes second (the timecheck).
    E tsCond, tcCond;
    int64_t dly[2] = {0, 0};     // the delay of each event's signal (31.9), set at the start
    std::string limText;         // the limits as written
    std::string inst, file;
    int line = 0;
    uint64_t unit = 1;           // ticks per time unit of the module, for messages
    std::vector<std::pair<int, Edge>> watch;   // the signals the terminals read
};
inline std::vector<TimingCheck>& timingChecks() { static std::vector<TimingCheck> v; return v; }
// +notimingcheck turns every timing check off.
inline bool& timingChecksOn() { static bool on = true; return on; }
// +neg_tchk: negative $setuphold / $recrem limits shift the window with
// delayed signals (31.9). Without it they count as 0 (31.9.4).
inline bool& negTimingChecks() { static bool on = false; return on; }
