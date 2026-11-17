// wtrack.hpp - which pages of plain memory have been written since a point.
//
// FENCE.I has to drop what a hart decoded from code that changed. The pages
// code came from are watched: read-only on the host, so the first store to
// one (from any path: a hart's fast loop or translated code, a device's DMA,
// a write buffer written back, a debugger) faults; the handler counts a
// write on the page and makes it writable again. FENCE.I then compares only
// pages with a write since it last looked, and watches them again.
//
// Every write into guest memory has to be an ordinary store from user space
// (memcpy and the like): a system call such as read(2) into a watched page
// fails with EFAULT instead of faulting. Where pages are not 4 KiB on the
// host, or there are no signals, nothing is watched and FENCE.I compares.
#pragma once

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if (defined(__linux__) || defined(__APPLE__)) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/mman.h>
#include <unistd.h>
#define MVPLAT_WTRACK 1
#else
#define MVPLAT_WTRACK 0
#endif

namespace vp {

class WriteWatch {
public:
    struct Region {
        uint8_t* base;
        size_t pages;
        std::atomic<uint32_t>* gen;  // writes counted per page
        std::atomic<uint8_t>* ro;    // 1: read-only now
    };
    static constexpr int MAX = 64;

    // A region of host memory (page aligned): its id, or -1 if it cannot be
    // watched.
    static int add(uint8_t* base, size_t size) {
#if MVPLAT_WTRACK
        if (getenv("MVPLAT_NO_WTRACK")) return -1;
        if (sysconf(_SC_PAGESIZE) != 4096 || ((uintptr_t)base & 4095) || !size) return -1;
        State& s = state();
        for (int i = 0; i < s.n; i++)
            if (s.r[i].base == base) return i;
        if (s.n == MAX || !install()) return -1;
        Region& r = s.r[s.n];
        r.pages = (size + 4095) / 4096;
        r.gen = new std::atomic<uint32_t>[r.pages]();
        r.ro = new std::atomic<uint8_t>[r.pages]();
        r.base = base;  // last: the handler looks at regions [0, n)
        std::atomic_thread_fence(std::memory_order_release);
        return s.n++;
#else
        (void)base; (void)size;
        return -1;
#endif
    }
    static const uint8_t* base(int id) { return state().r[id].base; }
    static uint32_t gen(int id, size_t pg) { return state().r[id].gen[pg].load(std::memory_order_acquire); }
    static bool readOnly(int id, size_t pg) { return state().r[id].ro[pg].load(std::memory_order_acquire) != 0; }
    // Make the page read-only (again); false if that failed.
    static bool watch(int id, size_t pg) {
#if MVPLAT_WTRACK
        Region& r = state().r[id];
        if (r.ro[pg].load()) return true;
        if (mprotect(r.base + pg * 4096, 4096, PROT_READ)) return false;
        r.ro[pg].store(1, std::memory_order_release);
        return true;
#else
        (void)id; (void)pg;
        return false;
#endif
    }
    // The region was mapped anew (writable): every page counts as written.
    static void remapped(uint8_t* base) {
        State& s = state();
        for (int i = 0; i < s.n; i++)
            if (s.r[i].base == base)
                for (size_t p = 0; p < s.r[i].pages; p++) {
                    s.r[i].ro[p].store(0);
                    s.r[i].gen[p].fetch_add(1);
                }
    }

private:
    struct State {
        Region r[MAX] = {};
        int n = 0;
        bool installed = false;
        struct sigaction oldSegv = {}, oldBus = {};
    };
    static State& state() {
        static State s;
        return s;
    }
#if MVPLAT_WTRACK
    static bool install() {
        State& s = state();
        if (s.installed) return true;
        struct sigaction sa = {};
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGSEGV, &sa, &s.oldSegv)) return false;
        sigaction(SIGBUS, &sa, &s.oldBus);  // macOS reports a write to a read-only page so
        s.installed = true;
        return true;
    }
    static void handler(int sig, siginfo_t* si, void* uc) {
        State& s = state();
        uint8_t* a = (uint8_t*)si->si_addr;
        for (int i = 0; i < s.n; i++) {
            Region& r = s.r[i];
            if (a >= r.base && a < r.base + r.pages * 4096) {
                const size_t pg = (size_t)(a - r.base) >> 12;
                // (another thread may have made it writable meanwhile)
                if (r.ro[pg].exchange(0, std::memory_order_acq_rel)) {
                    r.gen[pg].fetch_add(1, std::memory_order_acq_rel);
                    mprotect(r.base + pg * 4096, 4096, PROT_READ | PROT_WRITE);
                }
                return;  // the store runs again, and succeeds
            }
        }
        // not ours: what was there before
        struct sigaction& o = sig == SIGBUS ? s.oldBus : s.oldSegv;
        if (o.sa_flags & SA_SIGINFO) {
            if (o.sa_sigaction) { o.sa_sigaction(sig, si, uc); return; }
        } else if (o.sa_handler != SIG_DFL && o.sa_handler != SIG_IGN && o.sa_handler) {
            o.sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);
        raise(sig);
    }
#endif
};

}  // namespace vp
