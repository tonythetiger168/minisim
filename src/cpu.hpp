// CPU topology for the thread pool: physical cores, SMT (hyper-threading)
// siblings, and on hybrid Intel parts the performance (P) and efficiency (E)
// cores. Linux only (sysfs + CPUID); elsewhere every logical CPU counts as
// its own core and nothing is pinned.
//
// placement() orders the CPUs this process may run on (sched_getaffinity,
// so taskset and cgroup limits are honoured) as:
//   1. one logical CPU of each P core (or of every core on non-hybrid parts),
//   2. one logical CPU of each E core,
//   3. the remaining SMT siblings.
// Worker k of the pool runs on placement()[k], so N threads use N distinct
// physical cores before any core gets a second thread.
#pragma once
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

struct CpuTopo {
    struct Cpu { int id, pkg, core; bool eCore; };
    std::vector<Cpu> cpus;     // usable logical CPUs
    std::vector<int> order;    // placement order (logical CPU ids)
    int physical = 0;          // distinct (package, core) pairs
    int pCores = 0, eCores = 0;
    bool hybrid = false, known = false;

    static int readInt(const std::string& path, int dflt) {
        std::ifstream f(path);
        int v;
        return f >> v ? v : dflt;
    }
    // "0-3,8,10-11" -> ids
    static std::vector<int> readList(const std::string& path) {
        std::vector<int> r;
        std::ifstream f(path);
        std::string s;
        if (!(f >> s)) return r;
        std::stringstream ss(s);
        std::string part;
        while (std::getline(ss, part, ',')) {
            auto d = part.find('-');
            int a = std::stoi(part.substr(0, d)), b = d == std::string::npos ? a : std::stoi(part.substr(d + 1));
            for (int i = a; i <= b; i++) r.push_back(i);
        }
        return r;
    }
#if (defined(__x86_64__) || defined(__i386__)) && defined(__linux__)
    // CPUID leaf 0x1A on the CPU the calling thread runs on: 0x20 = Atom (E), 0x40 = Core (P).
    static int coreTypeHere() {
        unsigned a, b, c, d;
        if (!__get_cpuid_count(7, 0, &a, &b, &c, &d) || !(d & (1u << 15))) return 0;   // not hybrid
        if (__get_cpuid_max(0, nullptr) < 0x1A) return 0;
        __cpuid_count(0x1A, 0, a, b, c, d);
        return (int)(a >> 24);
    }
#endif
    void detect() {
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        std::vector<int> ids;
        if (sched_getaffinity(0, sizeof mask, &mask) == 0)
            for (int i = 0; i < CPU_SETSIZE; i++)
                if (CPU_ISSET(i, &mask)) ids.push_back(i);
        detectFrom("/sys/devices", ids, true);
#else
        detectFrom("", {}, false);
#endif
    }
    // `sys` is the sysfs devices directory (a fake tree in tests); `ids` the
    // usable logical CPUs; `probe`: may pin this thread to run CPUID per CPU.
    void detectFrom(const std::string& sys, const std::vector<int>& ids, bool probe) {
        (void)probe;
#if defined(__linux__)
        if (ids.empty()) goto fallback;
        {
        // Hybrid: the kernel's PMU lists when present, else CPUID on each CPU
        // (WSL2 and VMs often lack the sysfs lists but pass CPUID through).
        std::vector<int> atom = readList(sys + "/cpu_atom/cpus");
        std::vector<int> core = readList(sys + "/cpu_core/cpus");
        std::map<int, bool> eOf;
        if (!atom.empty() && !core.empty()) {
            for (int i : atom) eOf[i] = true;
            for (int i : core) eOf[i] = false;
        }
#if defined(__x86_64__) || defined(__i386__)
        else if (probe && coreTypeHere() != 0) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            for (int i : ids) CPU_SET(i, &mask);
            pthread_t self = pthread_self();
            for (int i : ids) {
                cpu_set_t one;
                CPU_ZERO(&one);
                CPU_SET(i, &one);
                if (pthread_setaffinity_np(self, sizeof one, &one) != 0) continue;
                eOf[i] = coreTypeHere() == 0x20;
            }
            pthread_setaffinity_np(self, sizeof mask, &mask);   // back to where we may run
        }
#endif
        for (int i : ids) {
            std::string t = sys + "/system/cpu/cpu" + std::to_string(i) + "/topology/";
            int pkg = readInt(t + "physical_package_id", 0), cid = readInt(t + "core_id", i);
            auto it = eOf.find(i);
            cpus.push_back({i, pkg, cid, it != eOf.end() && it->second});
        }
        known = true;
        }
    fallback:
#endif
        if (!known) {
            int hw = (int)std::thread::hardware_concurrency();
            for (int i = 0; i < std::max(hw, 1); i++) cpus.push_back({i, 0, i, false});
        }
        std::map<std::pair<int, int>, std::vector<int>> byCore;   // (pkg, core) -> logical ids
        std::map<std::pair<int, int>, bool> eCore;
        for (auto& c : cpus) {
            byCore[{c.pkg, c.core}].push_back(c.id);
            eCore[{c.pkg, c.core}] = c.eCore;
        }
        physical = (int)byCore.size();
        for (auto& [k, e] : eCore) (e ? eCores : pCores)++;
        hybrid = pCores > 0 && eCores > 0;
        for (int pass = 0; pass < 2; pass++)          // first P (or all), then E cores
            for (auto& [k, v] : byCore)
                if (eCore[k] == (pass == 1)) order.push_back(v[0]);
        for (size_t s = 1;; s++) {                    // then SMT siblings, round-robin
            bool any = false;
            for (auto& [k, v] : byCore)
                if (s < v.size()) { order.push_back(v[s]); any = true; }
            if (!any) break;
        }
    }
    int logical() const { return (int)order.size(); }
    // Physical cores worth one thread each by default: P cores on a hybrid
    // part (an E core is slower and holds up every batch it is in), else all.
    int fastCores() const { return hybrid ? pCores : physical; }
    std::string describe() const {
        std::ostringstream o;
        o << logical() << " logical CPUs, " << physical << " physical cores";
        if (hybrid) o << " (" << pCores << " P + " << eCores << " E)";
        if (logical() > physical) o << ", SMT on";
        if (!known) o << " (topology unknown)";
        return o.str();
    }
};

inline const CpuTopo& cpuTopo() {
    static const CpuTopo t = [] { CpuTopo x; x.detect(); return x; }();
    return t;
}

// Pin the calling thread to one logical CPU. Returns false if not supported.
inline bool pinToCpu(int cpu) {
#if defined(__linux__)
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    return pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0;
#else
    (void)cpu;
    return false;
#endif
}
