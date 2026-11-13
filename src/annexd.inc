// annexd.inc - the optional system tasks and functions of IEEE 1800-2023
// Annex D that need the kernel: $countdrivers (D.2), $log / $nolog (D.7),
// $key / $nokey (D.5), $input (D.4), $reset / $reset_count / $reset_value
// (D.8) and $save / $incsave / $restart (D.9). The lowering (lower.hpp)
// handles $getpattern, $scale, $scope, $showscopes, $showvars, $list and
// $sreadmemb / $sreadmemh itself.
//
// $reset and $restart start msim again (the same program and arguments):
// a deterministic simulation run again from time 0 reaches the same state.
//   $reset   - MSIM_RESET_COUNT and MSIM_RESET_VALUE carry $reset_count and
//              $reset_value into the new run.
//   $save    - writes a checkpoint file naming the n-th $save / $incsave of
//              the run. $restart(file) runs again with every output muted
//              until that $save is reached, then carries on from there.
// msim has no interactive mode, so a $reset with stop_value 0 carries on
// processing, $input has nothing to read commands into, and the key file
// of $key / $nokey is never written.

#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

inline void annexEnd();
namespace annexd {

// VPI simulation feature callbacks (38.36.3): msim.cpp's VPI sets this.
enum Feature { StartOfSave, EndOfSave, StartOfRestart, EndOfRestart, StartOfReset };
inline void (*featureHook)(Feature) = nullptr;
inline void feature(Feature f) { if (featureHook) featureHook(f); }

inline std::vector<std::string>& argv0() { static std::vector<std::string> v; return v; }
inline int counts[5];   // the last $countdrivers: 01x, 0, 1, x drivers; [4] forced

// $log: a copy of everything printed to the standard output.
struct Tee : std::streambuf {
    std::streambuf* out = nullptr;
    std::ofstream log;
    bool on = false;
    int overflow(int c) override {
        if (c == EOF) return 0;
        if (on) log.put((char)c);
        return out->sputc((char)c);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        if (on) log.write(s, n);
        return out->sputn(s, n);
    }
    int sync() override { if (on) log.flush(); return out->pubsync(); }
};
inline Tee& tee() { static Tee t; return t; }

inline long envLong(const char* n) { const char* v = getenv(n); return v ? strtol(v, nullptr, 10) : 0; }

// A digest of every signal's value: a checkpoint keeps it, and a run that goes
// back to the checkpoint checks it got the same state (a design that reads
// the clock, the environment or a DPI model may not).
inline std::string stateDigest() {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&](uint64_t x) { for (int b = 0; b < 8; b++) { h ^= (x >> (8 * b)) & 0xff; h *= 1099511628211ULL; } };
    for (size_t s = 0; s < S.sigs.size(); s++) {
        const Val& v = S.sigs[s].val;
        mix(v.v); mix(v.x); mix(v.z); mix((uint64_t)v.w | (uint64_t)v.real << 32 | (uint64_t)v.str << 33);
        for (size_t k = 0; k < v.hi.size(); k++) { mix(v.hi[k].v); mix(v.hi[k].x); mix(v.hi[k].z); }
    }
    mix(S.now);
    char b[32];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
    return b;
}

// Muted output while a $restart runs again up to its checkpoint.
inline int savedOut = -1, savedErr = -1;
inline long saveSeq = 0;
inline void unmute() {
    if (savedOut < 0) return;
    std::cout.flush();
    std::cerr.flush();
    fflush(stdout);
    fflush(stderr);
    dup2(savedOut, 1);
    dup2(savedErr, 2);
    close(savedOut);
    close(savedErr);
    savedOut = savedErr = -1;
}

[[noreturn]] inline void again() {
    std::cout.flush();
    std::cerr.flush();
    fflush(nullptr);
    if (tee().on) tee().log.flush();
    std::vector<char*> av;
    for (auto& a : argv0()) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);
    execv("/proc/self/exe", av.data());
    std::cerr << "msim: cannot start again: " << strerror(errno) << "\n";
    std::exit(1);
}

// $reset (D.8), also vpi_control(vpiReset) (38.6): start again from time 0.
[[noreturn]] inline void reset(long value, long diag) {
    feature(StartOfReset);
    if (diag > 0) std::cerr << "msim: $reset at time " << S.now << "\n";
    setenv("MSIM_RESET_COUNT", std::to_string(envLong("MSIM_RESET_COUNT") + 1).c_str(), 1);
    setenv("MSIM_RESET_VALUE", std::to_string(value).c_str(), 1);
    unsetenv("MSIM_RESTORE_AT");
    again();
}

inline std::string str(const Val& v) { Val t = v; t.str = true; return v_str(t); }

inline Val countDrivers(int s, int bit) {
    const Sig& g = S.sigs[(size_t)s];
    int c01x = 0, c0 = 0, c1 = 0, cx = 0;
    auto add = [&](const Val& v) {
        Val b = v_index(v, mk((uint64_t)bit, 32));
        if (b.x & 1) {
            if (b.z & 1) return;   // off
            cx++;
        } else if (b.v & 1) c1++;
        else c0++;
        c01x++;
    };
    if (g.drv.empty()) add(g.val);   // a net with a single driver kept as its value
    else
        for (const Drv& d : g.drv)
            if (!(d.s0 == STR_HIZ && d.s1 == STR_HIZ)) add(d.v);
    counts[0] = c01x; counts[1] = c0; counts[2] = c1; counts[3] = cx;
    counts[4] = (g.fl & Sim::SF_HELD) ? 1 : 0;
    return make(c01x > 1 ? 1 : 0, 0, 1, 0);
}

inline Val hook(int op, const std::vector<Val>& a) {
    auto i32 = [](int64_t x) { Val r = make((uint64_t)(uint32_t)x, 0, 32, 0); r.sgn = true; return r; };
    auto num = [&](size_t k, int64_t dflt) { return k < a.size() && !a[k].x ? v_to_i64(a[k]) : dflt; };
    switch (op) {
        case AD_COUNT: return countDrivers((int)a[0].v, (int)num(1, 0));
        case AD_COUNT_GET: {   // forced, 01x, 0, 1, x in argument order
            static const int map[5] = {4, 0, 1, 2, 3};
            int k = (int)a[0].v;
            return i32(k < 5 ? counts[map[k]] : 0);
        }
        case AD_LOG: {
            Tee& t = tee();
            if (!t.out) { t.out = std::cout.rdbuf(); std::cout.rdbuf(&t); std::atexit(annexEnd); }
            if (!a.empty() || !t.log.is_open()) {
                std::string f = a.empty() ? std::string("msim.log") : str(a[0]);
                if (t.log.is_open()) t.log.close();
                t.log.open(f);
                if (!t.log) { std::cerr << "$log: cannot open " << f << "\n"; t.on = false; return i32(0); }
            }
            t.on = true;
            return i32(0);
        }
        case AD_NOLOG:
            std::cout.flush();
            if (tee().on) tee().log.flush();
            tee().on = false;
            return i32(0);
        case AD_KEY:
        case AD_NOKEY: return i32(0);   // no interactive mode: nothing is typed in
        case AD_INPUT:
            std::cerr << "$input: msim has no interactive mode; " << (a.empty() ? std::string() : str(a[0])) << " is not read\n";
            return i32(0);
        case AD_RESET_COUNT: return i32(envLong("MSIM_RESET_COUNT"));
        case AD_RESET_VALUE: return i32(envLong("MSIM_RESET_VALUE"));
        case AD_RESET: reset(num(1, 0), num(2, 0));
        case AD_SAVE:
        case AD_INCSAVE: {
            ++saveSeq;
            if (savedOut >= 0 && envLong("MSIM_RESTORE_AT") == saveSeq) {   // the checkpoint of a $restart
                unsetenv("MSIM_RESTORE_AT");
                unmute();
                const char* want = getenv("MSIM_RESTORE_STATE");
                if (want && *want && stateDigest() != want)
                    std::cerr << "msim: warning: the state at the checkpoint differs from the one saved; the run is "
                                 "not repeatable (it reads the clock, the environment or a DPI model?)\n";
                else if (want && *want) std::cerr << "msim: restored the checkpoint at time " << S.now << "\n";
                unsetenv("MSIM_RESTORE_STATE");
                feature(EndOfRestart);
                return i32(0);
            }
            if (savedOut >= 0) return i32(0);   // running again: the checkpoint files are already there
            feature(StartOfSave);
            std::string f = a.empty() ? std::string() : str(a[0]);
            std::ofstream o(f);
            if (!o) { std::cerr << (op == AD_SAVE ? "$save" : "$incsave") << ": cannot write " << f << "\n"; feature(EndOfSave); return i32(0); }
            // What msim -restore needs to run the same simulation again: the
            // directory and the command line, and the state to compare.
            char cwd[4096];
            o << "msim checkpoint\nsave " << saveSeq << "\ntime " << S.now << "\nstate " << stateDigest() << "\n";
            if (getcwd(cwd, sizeof cwd)) o << "cwd " << cwd << "\n";
            for (auto& x : argv0()) o << "arg " << x << "\n";
            o.close();
            feature(EndOfSave);
            return i32(0);
        }
        case AD_RESTART: {
            if (savedOut >= 0) return i32(0);   // already on the way back to a checkpoint
            std::string f = a.empty() ? std::string() : str(a[0]);
            std::ifstream in(f);
            std::string magic, key;
            long seq = 0;
            std::getline(in, magic);
            in >> key >> seq;
            if (!in || magic != "msim checkpoint" || key != "save" || seq <= 0) {
                std::cerr << "$restart: " << f << " is not a checkpoint written by $save\n";
                return i32(0);
            }
            setenv("MSIM_RESTORE_AT", std::to_string(seq).c_str(), 1);
            for (std::string ln; std::getline(in, ln);)
                if (ln.rfind("state ", 0) == 0) setenv("MSIM_RESTORE_STATE", ln.c_str() + 6, 1);
            feature(StartOfRestart);
            again();
        }
    }
    return i32(0);
}

}   // namespace annexd

// At the start of main: keep the arguments for $reset / $restart, and mute
// the output of a run that goes back to a $save checkpoint.
inline void annexStartup(int argc, char** argv) {
    // msim -restore <checkpoint> [+plusargs...]: run the saved command line
    // again in its directory, quietly up to the checkpoint (like simv -r).
    if (argc > 1 && (std::string(argv[1]) == "-restore" || std::string(argv[1]) == "-r")) {
        if (argc < 3) { std::cerr << "msim: -restore needs a checkpoint file\n"; std::exit(2); }
        std::ifstream in(argv[2]);
        std::string magic, ln, cwd, state;
        long seq = 0;
        bool ucli = false;   // written by msim -i save: the commands replay it (interactive.inc)
        std::vector<std::string> args;
        std::getline(in, magic);
        while (std::getline(in, ln)) {
            if (ln == "save 0") ucli = true;
            if (ln.rfind("save ", 0) == 0) seq = strtol(ln.c_str() + 5, nullptr, 10);
            else if (ln.rfind("state ", 0) == 0) state = ln.substr(6);
            else if (ln.rfind("cwd ", 0) == 0) cwd = ln.substr(4);
            else if (ln.rfind("arg ", 0) == 0) args.push_back(ln.substr(4));
        }
        if (magic == "msim checkpoint" && ucli && !args.empty()) {
            char* full = realpath(argv[2], nullptr);
            if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
                std::cerr << "msim: -restore: cannot go to " << cwd << ": " << strerror(errno) << "\n";
                std::exit(2);
            }
            setenv("MSIM_UCLI_RESTORE", full ? full : argv[2], 1);
            for (int k = 3; k < argc; k++) args.push_back(argv[k]);
            annexd::argv0() = args;
            annexd::again();
        }
        if (magic != "msim checkpoint" || seq <= 0 || args.empty()) {
            std::cerr << "msim: " << argv[2] << " is not a checkpoint written by $save (or it is from an older msim)\n";
            std::exit(2);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            std::cerr << "msim: -restore: cannot go to " << cwd << ": " << strerror(errno) << "\n";
            std::exit(2);
        }
        for (int k = 3; k < argc; k++) args.push_back(argv[k]);   // more plusargs: read after the checkpoint
        setenv("MSIM_RESTORE_AT", std::to_string(seq).c_str(), 1);
        if (!state.empty()) setenv("MSIM_RESTORE_STATE", state.c_str(), 1);
        annexd::argv0() = args;
        annexd::again();
    }
    for (int k = 0; k < argc; k++) annexd::argv0().push_back(argv[k]);
    g_annexHook = annexd::hook;
    if (annexd::envLong("MSIM_RESTORE_AT") > 0) {
        fflush(nullptr);
        annexd::savedOut = dup(1);
        annexd::savedErr = dup(2);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); close(nul); }
        std::atexit(annexEnd);
    }
}
// At the end of the run: a checkpoint never reached.
inline void annexEnd() {
    if (annexd::savedOut >= 0) {
        annexd::unmute();
        std::cerr << "$restart: the checkpoint was not reached\n";
    }
    if (annexd::tee().out) {
        std::cout.flush();
        std::cout.rdbuf(annexd::tee().out);
        annexd::tee().out = nullptr;
    }
}
