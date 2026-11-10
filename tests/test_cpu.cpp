// cpu.hpp: topology from a fake sysfs tree. Thread placement must use one
// logical CPU per physical core first (P cores, then E cores), SMT siblings last.
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include "cpu.hpp"

namespace fs = std::filesystem;

static void put(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s << "\n";
}
// cpu i -> core id
static fs::path tree(const std::string& name, const std::vector<int>& coreOf,
                     const std::string& pList = "", const std::string& eList = "") {
    fs::path r = fs::temp_directory_path() / ("msim_cpu_" + name);
    fs::remove_all(r);
    for (size_t i = 0; i < coreOf.size(); i++) {
        fs::path t = r / "system/cpu" / ("cpu" + std::to_string(i)) / "topology";
        put(t / "core_id", std::to_string(coreOf[i]));
        put(t / "physical_package_id", "0");
    }
    if (!pList.empty()) put(r / "cpu_core/cpus", pList);
    if (!eList.empty()) put(r / "cpu_atom/cpus", eList);
    return r;
}
static std::vector<int> range(int a, int b) { std::vector<int> v; for (int i = a; i <= b; i++) v.push_back(i); return v; }

int main() {
    {   // i7-14700 as Linux numbers it: 8 P cores x 2 threads (cpu 2k, 2k+1), 12 E cores.
        std::vector<int> core;
        for (int k = 0; k < 8; k++) { core.push_back(k * 4); core.push_back(k * 4); }
        for (int k = 0; k < 12; k++) core.push_back(32 + k);
        fs::path r = tree("hybrid", core, "0-15", "16-27");
        CpuTopo t;
        t.detectFrom(r.string(), range(0, 27), false);
        assert(t.logical() == 28 && t.physical == 20);
        assert(t.hybrid && t.pCores == 8 && t.eCores == 12 && t.fastCores() == 8);
        std::vector<int> want;
        for (int k = 0; k < 8; k++) want.push_back(2 * k);        // one thread per P core
        for (int k = 16; k < 28; k++) want.push_back(k);          // then the E cores
        for (int k = 0; k < 8; k++) want.push_back(2 * k + 1);    // then the P cores' siblings
        assert(t.order == want);
        fs::remove_all(r);
    }
    {   // 4 cores x 2 SMT, siblings numbered n and n+4 (common on AMD / servers).
        fs::path r = tree("smt", {0, 1, 2, 3, 0, 1, 2, 3});
        CpuTopo t;
        t.detectFrom(r.string(), range(0, 7), false);
        assert(t.logical() == 8 && t.physical == 4 && !t.hybrid && t.fastCores() == 4);
        assert((t.order == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
        // taskset -c 0,4,5: two cores usable, one with both siblings.
        CpuTopo u;
        u.detectFrom(r.string(), {0, 4, 5}, false);
        assert(u.logical() == 3 && u.physical == 2);
        assert((u.order == std::vector<int>{0, 5, 4}));
        fs::remove_all(r);
    }
    {   // No SMT, no hybrid.
        fs::path r = tree("flat", {0, 1, 2, 3});
        CpuTopo t;
        t.detectFrom(r.string(), range(0, 3), false);
        assert(t.logical() == 4 && t.physical == 4 && t.fastCores() == 4);
        fs::remove_all(r);
    }
    std::printf("cpu topology ok\n");
}
