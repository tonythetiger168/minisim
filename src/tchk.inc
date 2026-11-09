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

// A stability window check with two limits ($setuphold, $recrem): lim1
// before the reference event (setup / removal), lim2 after it (hold /
// recovery). Simultaneous events violate unless both limits are zero.
// Negative limits need the delayed signals of 31.9 and count as zero.
inline void windowEvent(const TimingCheck& tc, State& st, int side, int64_t now, int64_t before, int64_t after) {
    if (before < 0) before = 0;
    if (after < 0) after = 0;
    int64_t tRef = st.t[tc.ref], tData = st.t[tc.data];
    if (side == 0) {   // reference
        if (tData != NONE && (before > 0 || after > 0) && tData == now) report(tc, tc.ref, now, tData);
        else if (before > 0 && tData != NONE && tData > now - before && tData < now) report(tc, tc.ref, now, tData);
        st.t[tc.ref] = now;
    } else {
        if (tRef != NONE && (before > 0 || after > 0) && tRef == now) report(tc, tc.ref, tRef, now);
        else if (after > 0 && tRef != NONE && now >= tRef && now < tRef + after) report(tc, tc.ref, tRef, now);
        st.t[tc.data] = now;
    }
}

// The events of check id that happened in this wake: ref and data are
// set for an event whose edge matched, refOn / dataOn when its condition
// also holds.
inline void step(int id) {
    const TimingCheck& tc = timingChecks()[(size_t)id];
    State& st = states()[(size_t)id];
    int64_t now = (int64_t)S.now;
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

inline void startTimingChecks() {
    if (!timingChecksOn()) return;
    tchk::states().assign(timingChecks().size(), {});
    for (size_t k = 0; k < timingChecks().size(); k++)
        if (!timingChecks()[k].watch.empty()) S.spawn(tchk::proc((int)k));
}
