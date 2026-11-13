// msim -vcd-sva / -vcd-power: what the simulator knows about assertions and
// power as signals in the waveform (Sim::StatView), as Verdi shows them from
// an FSDB written with $fsdbDumpSVA or "+power".
//
//   -vcd-sva    each assertion, assume and cover tb.u.a_gnt gets
//               tb.u.a_gnt.passes and tb.u.a_gnt.fails (32 bits): how many
//               attempts have passed (a cover: matched) and failed so far.
//               A failure is a step in .fails at the time it is reported.
//   -vcd-power  each power domain PD (msim -upf) gets TOP.upf.PD.simstate
//               (0 NORMAL, 1 CORRUPT_STATE_ON_CHANGE, 2 CORRUPT_STATE_ON_ACTIVITY,
//               3 CORRUPT_ON_CHANGE, 4 CORRUPT_ON_ACTIVITY, 5 CORRUPT; 9.6),
//               each supply net upf.NET.state (0 OFF, 1 FULL_ON, 2 PARTIAL_ON,
//               3 UNDETERMINED) and upf.NET.mV (its voltage in millivolts).
//
// They are written at the end of each time slot, as the class views
// (objview.inc), and are the same for any -threads.
#pragma once

namespace statview {

// A name below the top, as msim names signals (tb.u.a -> u.a).
inline std::string rel(const std::string& n) {
    if (n.size() > S.vcdTop.size() && n.compare(0, S.vcdTop.size(), S.vcdTop) == 0 && n[S.vcdTop.size()] == '.')
        return n.substr(S.vcdTop.size() + 1);
    return n;
}

inline int mk(const std::string& n, int w) {
    int s = S.sig(n, w);
    S.sigs[(size_t)s].val = make(0, 0, w);
    return s;
}

// Each view starts at its value now, so the first slot has no step.
inline void prime(size_t from) {
    for (size_t k = from; k < S.statViews.size(); k++)
        S.sigs[(size_t)S.statViews[k].sig].val = make(S.statViews[k].get(), 0, S.sigs[(size_t)S.statViews[k].sig].w);
}

inline size_t sva() {
    size_t made = 0, from = S.statViews.size();
    for (size_t i = 0; i < asserts().size(); i++) {
        const AssertInfo& a = asserts()[i];
        if (a.hidden || a.prot || a.msgProt || a.name.empty()) continue;   // protected code (1735) stays out
        std::string n = rel(a.name);
        S.statViews.push_back({mk(n + ".passes", 32), [i] { return asserts()[i].passes; }});
        S.statViews.push_back({mk(n + ".fails", 32), [i] { return asserts()[i].fails; }});
        made++;
    }
    prime(from);
    return made;
}

// UPF names use '/' for the hierarchy; the waveform uses '.'.
inline std::string dotted(std::string n) {
    for (char& c : n)
        if (c == '/') c = '.';
    return n;
}

inline size_t power() {
    auto& rt = upfrt::rt();
    if (!rt.on) return 0;
    size_t made = 0, from = S.statViews.size();
    for (size_t i = 0; i < rt.doms.size(); i++) {
        if (rt.doms[i].region) continue;
        S.statViews.push_back({mk("upf." + dotted(rt.doms[i].name) + ".simstate", 3),
                               [i] { return (uint64_t)std::max(0, g_pdMode[i]); }});
        made++;
    }
    for (auto& [net, v] : rt.val) {
        std::string n = "upf." + dotted(net);
        std::string key = net;
        S.statViews.push_back({mk(n + ".state", 2), [key] {
                                   auto it = upfrt::rt().val.find(key);
                                   return it == upfrt::rt().val.end() ? (uint64_t)0 : (uint64_t)it->second.st;
                               }});
        S.statViews.push_back({mk(n + ".mV", 32), [key] {
                                   auto it = upfrt::rt().val.find(key);
                                   if (it == upfrt::rt().val.end() || !it->second.hasV || it->second.st == upf::S_OFF) return (uint64_t)0;
                                   return (uint64_t)std::llround(it->second.v * 1000.0);
                               }});
        made++;
    }
    prime(from);
    return made;
}

}   // namespace statview
