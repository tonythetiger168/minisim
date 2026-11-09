// Timing checks (IEEE 1800-2023 clause 31), run time. Each check is a
// process that wakes on any change of the signals its terminals read, works
// out which of its events happened (31.5, 31.7), and runs the check's state
// machine (31.3, 31.4). A violation prints a report and toggles the notifier
// (31.6). Included from exec.inc.

namespace tchk {

constexpr int64_t NONE = INT64_MIN;

struct State {
    Val prev[2];
    int64_t t[2] = {NONE, NONE};   // the last event on ev[0] / ev[1]
    // $timeskew / $fullskew: active, which side holds the timestamp, and a
    // generation that cancels a pending timer.
    bool active = false;
    int stampSide = -1;
    int64_t tStamp = NONE;
    uint64_t gen = 0;
    // $nochange: inside the reference level, its leading and trailing edges,
    // and data events inside the level waiting for the trailing edge.
    bool high = false;
    int64_t tLead = NONE, tTrail = NONE;
    vector<int64_t> pend;
    // $width: the start of the pulse.
    int64_t tPulse = NONE;
};
inline vector<State>& states() { static vector<State> v; return v; }

// The level of the least significant bit: 0, 1, or 2 for x and z.
inline int lsb(const Val& v) { return (v.x & 1) ? 2 : (int)(v.v & 1); }
inline uint8_t transition(const Val& a, const Val& b) {
    static const uint8_t bit[3][3] = {{0, TE_01, TE_0X}, {TE_10, 0, TE_1X}, {TE_X0, TE_X1, 0}};
    return bit[lsb(a)][lsb(b)];
}
// The reference edges of $width / $nochange turned around: posedge becomes
// negedge, and each transition its reverse.
inline uint8_t opposite(uint8_t e) {
    uint8_t r = 0;
    if (e & TE_01) r |= TE_10;
    if (e & TE_10) r |= TE_01;
    if (e & TE_0X) r |= TE_1X;
    if (e & TE_1X) r |= TE_0X;
    if (e & TE_X0) r |= TE_X1;
    if (e & TE_X1) r |= TE_X0;
    return r;
}
// 31.7: a deterministic condition enables the check only when it is 1; a
// == or != condition also when it is x. A vector uses its LSB.
inline bool condOn(const TcEvent& ev) {
    if (!ev.cond) return true;
    Val c = eval(ev.cond, rd);
    if (c.x & 1) return ev.nondet;
    return (c.v & 1) != 0;
}

inline string timeStr(int64_t t, uint64_t unit) {
    if (t == NONE) return "";
    if (unit <= 1 || t % (int64_t)unit == 0) return std::to_string(t / (int64_t)unit);
    std::ostringstream o;
    o << (double)t / (double)unit;
    return o.str();
}

// A violation (31.6): the report names the check, the times of its two
// events in the module's time unit, and the limits; the notifier toggles
// (x becomes 1, z stays z).
inline void violation(const TimingCheck& tc, int64_t t0, int64_t t1) {
    // With delayed signals (31.9) the check sees each event late; the report
    // gives the time of the event itself.
    if (t0 != NONE) t0 -= tc.dly[0];
    if (t1 != NONE) t1 -= tc.dly[tc.nev == 2 ? 1 : 0];
    std::cout << "\"" << tc.file << "\", " << tc.line << ": Timing violation in " << tc.inst << "\n    "
              << tc.name << "( " << tc.ev[0].text << ":" << timeStr(t0, tc.unit);
    if (tc.nev == 2) std::cout << ", " << tc.ev[1].text << ":" << timeStr(t1, tc.unit);
    else std::cout << ", :" << timeStr(t1, tc.unit);
    std::cout << ", " << tc.limText << " );\n";
    if (tc.notifier >= 0) {
        Val n = S.get(tc.notifier);
        bool z = n.z & 1, x = n.x & 1;
        if (z) return;
        uint64_t b = x ? 1 : !(n.v & 1);
        Val nv = n;
        nv.v = (n.v & ~1ull) | b;
        nv.x = n.x & ~1ull;
        nv.z = n.z & ~1ull;
        S.set(tc.notifier, nv);
    }
}

// The time of event k, for the report: ev[0] / ev[1] in the order written.
inline void report(const TimingCheck& tc, int k0, int64_t tk0, int64_t tk1) {
    if (k0 == 0) violation(tc, tk0, tk1);
    else violation(tc, tk1, tk0);
}

Task timer(int id, uint64_t gen, uint64_t d);
inline void startTimer(int id, State& st, int64_t lim) {
    Task t = timer(id, st.gen, (uint64_t)lim);
    S.all.push_back(t.h);
    S.active.push_back(t.h);
}

// One event of a $timeskew / $fullskew: side is 0 for the reference event
// and 1 for the data event; on says whether its condition holds.
inline void skewEvent(int id, const TimingCheck& tc, State& st, int side, bool on, int64_t now) {
    bool full = tc.kind == TC_FULLSKEW;
    auto limitOf = [&](int s) { return (s == 0 || !full) ? tc.lim1 : tc.lim2; };
    int ix = side == 0 ? tc.ref : tc.data;
    if (!full && side == 1) {   // $timeskew: data events only check
        if (!on || !st.active) return;
        int64_t d = now - st.tStamp;
        if (d > tc.lim1) {
            if (tc.eventBased) {
                report(tc, tc.ref, st.tStamp, now);
                if (!tc.remainActive) { st.active = false; st.gen++; }
            }
        } else if (!tc.eventBased) { st.active = false; st.gen++; }
        return;
    }
    (void)ix;
    if (!on) {   // a conditioned timestamp event whose condition is false
        if (st.active && st.stampSide == side && !tc.remainActive) { st.active = false; st.gen++; }
        return;
    }
    if (full && st.active && st.stampSide != side) {   // a timecheck event
        int64_t d = now - st.tStamp;
        int sideIx = st.stampSide == 0 ? tc.ref : tc.data;
        if (d <= limitOf(st.stampSide)) { st.active = false; st.gen++; return; }
        if (!tc.eventBased) return;   // the timer already reported it
        report(tc, sideIx, st.tStamp, now);
        // It ends that window and starts a new one as its timestamp.
    }
    st.active = true;
    st.stampSide = side;
    st.tStamp = now;
    st.gen++;
    if (!tc.eventBased) startTimer(id, st, limitOf(side));
}

inline void timeout(int id, uint64_t gen) {
    const TimingCheck& tc = timingChecks()[(size_t)id];
    State& st = states()[(size_t)id];
    if (st.gen != gen || !st.active) return;
    int ix = st.stampSide == 0 ? tc.ref : tc.data;
    report(tc, ix, st.tStamp, (int64_t)S.now);
    st.active = false;
    st.gen++;
}
Task timer(int id, uint64_t gen, uint64_t d) {
    co_await Delay{d};
    co_await Delay{0};   // after a timestamp event at the same time (31.4.2)
    timeout(id, gen);
}

// A stability window check with two limits ($setuphold, $recrem): before
// the reference event (setup / removal) and after it (hold / recovery).
// Simultaneous events violate unless both limits are zero. Each event
// checks the other's last timestamp, if the timecheck condition holds, and
// becomes a timestamp itself if the timestamp condition holds (31.9.2).
// The limits are the ones adjusted for the delayed signals (31.9).
inline void windowEvent(const TimingCheck& tc, State& st, int side, int64_t now, int64_t before, int64_t after) {
    if (before < 0) before = 0;
    if (after < 0) after = 0;
    bool tcOn = !tc.tcCond || truth(eval(tc.tcCond, rd)) == 1;
    bool tsOn = !tc.tsCond || truth(eval(tc.tsCond, rd)) == 1;
    int64_t tRef = st.t[tc.ref], tData = st.t[tc.data];
    if (side == 0) {   // reference
        if (tcOn && tData != NONE) {
            if ((before > 0 || after > 0) && tData == now) report(tc, tc.ref, now, tData);
            else if (before > 0 && tData > now - before && tData < now) report(tc, tc.ref, now, tData);
        }
        if (tsOn) st.t[tc.ref] = now;
    } else {
        if (tcOn && tRef != NONE) {
            if ((before > 0 || after > 0) && tRef == now) report(tc, tc.ref, tRef, now);
            else if (after > 0 && now >= tRef && now < tRef + after) report(tc, tc.ref, tRef, now);
        }
        if (tsOn) st.t[tc.data] = now;
    }
}

// Deliver the events of check id: changed[k] says event argument k
// changed, tr[k] is its transition. With delayed signals (31.9) this runs
// when the delayed copy changes; conditions are read then.
inline void deliver(int id, const bool changed[2], const uint8_t tr[2]) {
    const TimingCheck& tc = timingChecks()[(size_t)id];
    State& st = states()[(size_t)id];
    int64_t now = (int64_t)S.now;
    auto hit = [&](int k, uint8_t edges) { return changed[k] && (edges ? (tr[k] & edges) != 0 : true); };
    const int R = tc.ref, D = tc.data;
    switch (tc.kind) {
        case TC_SETUP:     // timestamp data, timecheck reference: (t - lim, t)
        case TC_REMOVAL:
            if (hit(D, tc.ev[D].edges) && condOn(tc.ev[D])) st.t[D] = now;
            if (hit(R, tc.ev[R].edges) && condOn(tc.ev[R])) {
                int64_t td = st.t[D];
                if (tc.lim1 > 0 && td != NONE && td > now - tc.lim1 && td < now) report(tc, R, now, td);
                st.t[R] = now;
            }
            break;
        case TC_HOLD:      // timestamp reference, timecheck data: [t, t + lim)
        case TC_RECOVERY: {
            bool r = hit(R, tc.ev[R].edges) && condOn(tc.ev[R]);
            bool d = hit(D, tc.ev[D].edges) && condOn(tc.ev[D]);
            if (r) {
                if (tc.lim1 > 0 && st.t[D] == now && !d) report(tc, R, now, now);
                st.t[R] = now;
            }
            if (d) {
                int64_t t0 = st.t[R];
                if (tc.lim1 > 0 && t0 != NONE && now >= t0 && now < t0 + tc.lim1) report(tc, R, t0, now);
                st.t[D] = now;
            }
            break;
        }
        case TC_SETUPHOLD:
        case TC_RECREM: {
            // $setuphold(ref, data, setup, hold); $recrem(ref, data, recovery, removal):
            // removal is the window before the reference event, recovery after.
            int64_t before = tc.kind == TC_SETUPHOLD ? tc.lim1 : tc.lim2;
            int64_t after = tc.kind == TC_SETUPHOLD ? tc.lim2 : tc.lim1;
            bool d = hit(D, tc.ev[D].edges) && condOn(tc.ev[D]);
            bool r = hit(R, tc.ev[R].edges) && condOn(tc.ev[R]);
            if (d) windowEvent(tc, st, 1, now, before, after);
            if (r) windowEvent(tc, st, 0, now, before, after);
            break;
        }
        case TC_SKEW:      // every data event later than lim after the last reference event
            if (hit(R, tc.ev[R].edges) && condOn(tc.ev[R])) st.t[R] = now;
            if (hit(D, tc.ev[D].edges) && condOn(tc.ev[D]))
                if (st.t[R] != NONE && now - st.t[R] > tc.lim1) report(tc, R, st.t[R], now);
            break;
        case TC_TIMESKEW:
        case TC_FULLSKEW:
            if (hit(R, tc.ev[R].edges)) skewEvent(id, tc, st, 0, condOn(tc.ev[R]), now);
            if (hit(D, tc.ev[D].edges)) skewEvent(id, tc, st, 1, condOn(tc.ev[D]), now);
            break;
        case TC_PERIOD:    // the same edge again sooner than lim
            if (hit(0, tc.ev[0].edges) && condOn(tc.ev[0])) {
                if (st.t[0] != NONE && now - st.t[0] < tc.lim1) violation(tc, st.t[0], now);
                st.t[0] = now;
            }
            break;
        case TC_WIDTH: {   // the opposite edge sooner than lim, unless the pulse is below the threshold
            bool on = condOn(tc.ev[0]);
            if (hit(0, opposite(tc.ev[0].edges)) && on && st.tPulse != NONE) {
                int64_t w = now - st.tPulse;
                if (tc.thresh < w && w < tc.lim1) violation(tc, st.tPulse, now);
                st.tPulse = NONE;
            }
            if (hit(0, tc.ev[0].edges) && on) st.tPulse = now;
            break;
        }
        case TC_NOCHANGE: {
            // A data event inside (leading edge - start, trailing edge + end).
            int64_t start = tc.lim1, end = tc.lim2;
            bool lead = hit(R, tc.ev[R].edges) && condOn(tc.ev[R]);
            bool trail = hit(R, opposite(tc.ev[R].edges)) && st.high;
            bool d = hit(D, tc.ev[D].edges) && condOn(tc.ev[D]);
            if (d) {
                if (st.high) {
                    if (now > st.tLead - start) {
                        if (end >= 0) report(tc, R, st.tLead, now);
                        else st.pend.push_back(now);
                    }
                } else if (st.tTrail != NONE && end > 0 && now < st.tTrail + end) report(tc, R, st.tLead, now);
                st.t[D] = now;
            }
            if (trail) {
                st.high = false;
                st.tTrail = now;
                for (int64_t p : st.pend)
                    if (p < now + end) report(tc, R, st.tLead, p);
                st.pend.clear();
            }
            if (lead) {
                st.high = true;
                st.tLead = now;
                st.tTrail = NONE;
                st.pend.clear();
                int64_t td = st.t[D];
                if (start > 0 && td != NONE && td > now - start && td < now) report(tc, R, now, td);
            }
            break;
        }
    }
}

Task deliverLater(int id, int k, uint8_t tr, uint64_t d) {
    co_await Delay{d};
    bool ch[2] = {false, false};
    uint8_t t2[2] = {0, 0};
    ch[k] = true;
    t2[k] = tr;
    deliver(id, ch, t2);
}

// A terminal of check id changed: find each event's transition and deliver
// it now, or after its signal's delay (31.9).
inline void step(int id) {
    const TimingCheck& tc = timingChecks()[(size_t)id];
    State& st = states()[(size_t)id];
    uint8_t tr[2] = {0, 0};
    bool changed[2] = {false, false};
    for (int k = 0; k < tc.nev; k++) {
        Val v = eval(tc.ev[k].e, rd);
        if (!(v == st.prev[k])) {
            changed[k] = true;
            tr[k] = transition(st.prev[k], v);
            st.prev[k] = v;
        }
    }
    bool now0[2] = {false, false};
    for (int k = 0; k < tc.nev; k++) {
        if (!changed[k]) continue;
        if (tc.dly[k] == 0) { now0[k] = true; continue; }
        Task t = deliverLater(id, k, tr[k], (uint64_t)tc.dly[k]);
        S.all.push_back(t.h);
        S.active.push_back(t.h);
    }
    if (now0[0] || now0[1]) deliver(id, now0, tr);
}

Task proc(int id) {
    const TimingCheck& tc = timingChecks()[(size_t)id];
    State& st = states()[(size_t)id];
    for (int k = 0; k < tc.nev; k++) st.prev[k] = eval(tc.ev[k].e, rd);
    const auto* ev = &tc.watch;
    for (;;) {
        if (ev->size() > 1) co_await WaitAny{ev};
        else co_await Wait{(*ev)[0].first, (*ev)[0].second};
        step(id);
    }
}

}   // namespace tchk

namespace tchk {

// A delayed signal (31.9): dst follows src, each change d later. Every
// change is kept (a transport delay), so a pulse shorter than d still
// arrives.
Task copyLater(int dst, Val v, uint64_t d) {
    co_await Delay{d};
    S.set(dst, v);
}
Task copyProc(int src, int dst, uint64_t d) {
    S.set(dst, S.get(src));
    for (;;) {
        co_await Wait{src, ANY};
        Val v = S.get(src);
        if (d == 0) { S.set(dst, v); continue; }
        Task t = copyLater(dst, v, d);
        S.all.push_back(t.h);
        S.active.push_back(t.h);
    }
}

inline void warn(const TimingCheck& tc, const string& what) {
    std::cerr << "msim: warning: " << tc.file << ":" << tc.line << ": " << tc.name << " in " << tc.inst << ": " << what
              << " (31.9)\n";
}
inline bool windowKind(int k) { return k == TC_SETUPHOLD || k == TC_RECREM; }
// The window of a $setuphold / $recrem: the limit before the reference
// event (setup, removal) and the one after it (hold, recovery).
inline int64_t& beforeLim(TimingCheck& t) { return t.kind == TC_SETUPHOLD ? t.lim1 : t.lim2; }
inline int64_t& afterLim(TimingCheck& t) { return t.kind == TC_SETUPHOLD ? t.lim2 : t.lim1; }

// The delay of each signal that negative limits need (31.9.1). Each
// $setuphold / $recrem with a negative limit asks that the delayed data
// and reference events straddle each other: d(ref) - d(data) >= 1 - before
// and d(data) - d(ref) >= 1 - after, one tick more so the window has room
// inside its end points. The smallest delays that meet every constraint
// are the longest paths from 0 (Bellman-Ford). When the constraints have
// no solution, the most negative limit becomes 0, with a warning, and the
// delays are worked out again.
// Delays are per instance: two cells on one net each delay their own copy.
using Key = pair<string, int>;
inline std::map<Key, int64_t> solveDelays() {
    auto& all = timingChecks();
    for (auto& t : all) {
        if (!windowKind(t.kind)) continue;
        if ((beforeLim(t) < 0 || afterLim(t) < 0) && (t.ev[t.ref].sig < 0 || t.ev[t.data].sig < 0)) {
            warn(t, "a negative limit needs whole signals as terminals; it counts as 0");
            if (beforeLim(t) < 0) beforeLim(t) = 0;
            if (afterLim(t) < 0) afterLim(t) = 0;
        }
    }
    // Each instance is solved on its own, so one inconsistent cell does not
    // change another's limits.
    std::set<string> insts;
    for (auto& t : all)
        if (windowKind(t.kind)) insts.insert(t.inst);
    std::map<Key, int64_t> out;
    for (const string& in : insts)
        for (;;) {
            struct C { Key u, v; int64_t w; };   // d[v] - d[u] >= w
            vector<C> cs;
            std::map<Key, int64_t> d;
            for (auto& t : all) {
                if (t.inst != in || !windowKind(t.kind) || (beforeLim(t) >= 0 && afterLim(t) >= 0)) continue;
                Key R{t.inst, t.ev[t.ref].sig}, D{t.inst, t.ev[t.data].sig};
                cs.push_back({D, R, 1 - beforeLim(t)});
                cs.push_back({R, D, 1 - afterLim(t)});
                d[R] = 0;
                d[D] = 0;
            }
            bool changed = true;
            for (size_t it = 0; changed && it <= d.size(); it++) {
                changed = false;
                for (auto& c : cs)
                    if (d[c.u] + c.w > d[c.v]) { d[c.v] = d[c.u] + c.w; changed = true; }
            }
            if (!changed) { out.insert(d.begin(), d.end()); break; }
            TimingCheck* worst = nullptr;
            int64_t* lim = nullptr;
            for (auto& t : all) {
                if (t.inst != in || !windowKind(t.kind)) continue;
                for (int64_t* l : {&beforeLim(t), &afterLim(t)})
                    if (*l < 0 && (!lim || *l < *lim)) { lim = l; worst = &t; }
            }
            warn(*worst, "negative limits with no consistent signal delays; the most negative one counts as 0");
            *lim = 0;
        }
    return out;
}

}   // namespace tchk

inline void startTimingChecks() {
    using namespace tchk;
    auto& all = timingChecks();
    bool neg = negTimingChecks() && timingChecksOn();
    std::map<Key, int64_t> d;
    if (neg) d = solveDelays();
    auto delayOf = [&](const TimingCheck& t, int sig) -> int64_t {
        auto it = d.find(Key{t.inst, sig});
        return it == d.end() ? 0 : it->second;
    };
    // Delayed signals are copies of their signals, delayed only when
    // negative limits need it (31.9.4).
    std::set<pair<int, int>> copies;
    for (auto& t : all)
        for (int k = 0; k < t.nev; k++)
            if (t.ev[k].delayed >= 0 && t.ev[k].sig >= 0 && copies.insert({t.ev[k].sig, t.ev[k].delayed}).second)
                S.spawn(copyProc(t.ev[k].sig, t.ev[k].delayed, (uint64_t)delayOf(t, t.ev[k].sig)));
    if (!timingChecksOn()) return;
    // Every check but the skew checks sees the delayed events, and its
    // limits shift by the difference of the two delays (31.9.1). A positive
    // limit that drops to 0 or below becomes 0, with a warning.
    if (neg && !d.empty())
        for (auto& t : all) {
            if (t.kind == TC_SKEW || t.kind == TC_TIMESKEW || t.kind == TC_FULLSKEW || t.nev < 2) continue;
            int64_t dR = delayOf(t, t.ev[t.ref].sig), dD = delayOf(t, t.ev[t.data].sig);
            if (!dR && !dD) continue;
            t.dly[t.ref] = dR;
            t.dly[t.data] = dD;
            int64_t sh = dR - dD;   // how much later the delayed reference comes
            auto adjust = [&](int64_t& l, int64_t by) {
                int64_t was = l;
                l += by;
                if (was > 0 && l <= 0) { warn(t, "a limit adjusted for the delayed signals is 0 or less; it counts as 0"); l = 0; }
            };
            switch (t.kind) {
                case TC_SETUP: case TC_REMOVAL: adjust(t.lim1, sh); break;
                case TC_HOLD: case TC_RECOVERY: adjust(t.lim1, -sh); break;
                case TC_SETUPHOLD: case TC_RECREM: adjust(beforeLim(t), sh); adjust(afterLim(t), -sh); break;
                case TC_NOCHANGE: t.lim1 += sh; t.lim2 -= sh; break;
            }
        }
    tchk::states().assign(timingChecks().size(), {});
    for (size_t k = 0; k < timingChecks().size(); k++)
        if (!timingChecks()[k].watch.empty()) S.spawn(tchk::proc((int)k));
}
