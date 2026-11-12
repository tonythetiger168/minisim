// profile.hpp - msim -profile: where simulation time goes (like the VCS
// simprofile report). Every process, static combinational block and clocked
// block is timed each time it runs; the scheduler regions that are not one
// process (NBA updates, assertions in the observed region, postponed
// $strobe / $monitor) are timed as a whole. The run is single-threaded so
// the times add up, and the report lists the busiest processes, then the
// time per instance and per kind of process.
//
// Times are wall-clock and change from run to run; activation counts do not.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace prof {

struct Ent {
    std::string kind, scope, loc;   // "always_ff", "top.u_core", "core.sv:42"
    uint64_t ns = 0, calls = 0;
};

struct Profiler {
    bool on = false;
    std::string path = "simprofile.txt";
    std::vector<Ent> ents;
    std::unordered_map<const void*, int> byHandle;   // coroutine frame -> entry
    std::vector<int> combEnt, seqEnt;                // comb rank / seq id -> entry
    int other = -1;                                  // processes started at run time (fork, tasks)
    // Scheduler regions timed as a whole.
    enum Region { NBA, OBSERVED, REACTIVE, POSTPONED, NREG };
    uint64_t regNs[NREG] = {}, regCalls[NREG] = {};
    std::chrono::steady_clock::time_point t0;

    int add(std::string kind, std::string scope, std::string loc) {
        ents.push_back({std::move(kind), std::move(scope), std::move(loc)});
        return (int)ents.size() - 1;
    }
    int otherEnt() {
        if (other < 0) other = add("(processes started at run time)", "", "");
        return other;
    }
    int ofHandle(const void* h) {
        auto it = byHandle.find(h);
        return it == byHandle.end() ? otherEnt() : it->second;
    }
    static uint64_t nowNs() {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void charge(int e, uint64_t ns) { ents[e].ns += ns; ents[e].calls++; }
    void chargeRegion(Region r, uint64_t ns) { regNs[r] += ns; regCalls[r]++; }

    static std::string ms(uint64_t ns) {
        char b[32];
        std::snprintf(b, sizeof b, "%.3f", (double)ns / 1e6);
        return b;
    }
    static std::string pct(uint64_t part, uint64_t whole) {
        char b[16];
        std::snprintf(b, sizeof b, "%5.1f%%", whole ? 100.0 * (double)part / (double)whole : 0.0);
        return b;
    }
    // The report. `simTime` is the last simulation time, in `unit`.
    bool write(uint64_t simTime, const std::string& unit, size_t top = 50) const {
        std::ofstream o(path);
        if (!o) return false;
        uint64_t wall = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        uint64_t inProcs = 0, calls = 0, inRegs = 0;
        for (const Ent& e : ents) { inProcs += e.ns; calls += e.calls; }
        for (int r = 0; r < NREG; r++) inRegs += regNs[r];
        uint64_t kernel = wall > inProcs + inRegs ? wall - inProcs - inRegs : 0;
        o << "msim simulation profile\n\n";
        o << "Simulation time: " << simTime << " (ticks of " << unit << ")\n";
        o << "Run time:        " << ms(wall) << " ms\n";
        o << "Activations:     " << calls << "\n\n";
        o << "== Time by category ==\n";
        o << "  " << pct(inProcs, wall) << "  " << ms(inProcs) << " ms  processes\n";
        static const char* rn[NREG] = {"NBA updates", "observed region (assertions)",
                                       "reactive region (programs, checkers)", "postponed region ($strobe, $monitor)"};
        for (int r = 0; r < NREG; r++)
            if (regCalls[r]) o << "  " << pct(regNs[r], wall) << "  " << ms(regNs[r]) << " ms  " << rn[r] << "\n";
        o << "  " << pct(kernel, wall) << "  " << ms(kernel) << " ms  scheduler and other\n\n";

        std::vector<int> idx;
        for (size_t i = 0; i < ents.size(); i++)
            if (ents[i].calls) idx.push_back((int)i);
        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return ents[a].ns > ents[b].ns; });
        o << "== Processes by time (top " << std::min(top, idx.size()) << " of " << idx.size() << ") ==\n";
        o << "     time%        ms  activations  kind            instance  source\n";
        for (size_t k = 0; k < idx.size() && k < top; k++) {
            const Ent& e = ents[idx[k]];
            char b[64];
            std::snprintf(b, sizeof b, "%12s  %11llu", ms(e.ns).c_str(), (unsigned long long)e.calls);
            o << "  " << pct(e.ns, wall) << " " << b << "  " << e.kind << "  " << (e.scope.empty() ? "-" : e.scope)
              << "  " << (e.loc.empty() ? "-" : e.loc) << "\n";
        }
        auto group = [&](const char* title, auto key) {
            std::map<std::string, std::pair<uint64_t, uint64_t>> g;
            for (const Ent& e : ents)
                if (e.calls) { auto& p = g[key(e)]; p.first += e.ns; p.second += e.calls; }
            std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> v(g.begin(), g.end());
            std::stable_sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
            o << "\n== " << title << " ==\n";
            o << "     time%        ms  activations  name\n";
            for (auto& [k, p] : v) {
                char b[64];
                std::snprintf(b, sizeof b, "%12s  %11llu", ms(p.first).c_str(), (unsigned long long)p.second);
                o << "  " << pct(p.first, wall) << " " << b << "  " << (k.empty() ? "-" : k) << "\n";
            }
        };
        group("Time by instance", [](const Ent& e) { return e.scope; });
        group("Time by source file", [](const Ent& e) { return e.loc.substr(0, e.loc.rfind(':')); });   // "-": ports, initializers
        group("Time by kind of process", [](const Ent& e) { return e.kind; });
        return true;
    }
};

inline Profiler& P() {
    static Profiler p;
    return p;
}

}   // namespace prof
