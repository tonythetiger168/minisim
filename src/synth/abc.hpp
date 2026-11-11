// synth/abc.hpp - logic restructuring by ABC, run as a separate program.
//
// ABC (Berkeley, https://github.com/berkeley-abc/abc, a permissive
// BSD-style licence) rewrites and-inverter graphs well. msynth hands it the
// combinational logic of the design as an AIGER file, runs a script
// (rewriting, refactoring, balancing), reads the result back, and maps it
// with its own mapper. ABC is not linked in: it carries its own copies of
// SAT solvers that msynth's tools also link, and a separate process keeps a
// failure there from taking msynth down.
//
// The program is $MSYNTH_ABC, else the first of `abc`, `berkeley-abc`,
// `yosys-abc` on PATH. Without one, nothing happens.
//
// Flops stay outside: their data, enable, clock and reset cones are outputs
// of the file, so a load enable is still an enable afterwards. The result
// is read with the AIGER reader of src/fpga.hpp.
#pragma once
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../aig.hpp"
#include "../fpga.hpp"

extern char** environ;

namespace synth {

// The default script: DAG-aware rewriting and refactoring for area, the
// second half also taking zero-gain moves.
inline const char* abcDefaultScript() { return "drw; drf; drw; drwsat; drw -z; drf -z"; }

// The ABC program to run, or "" when there is none.
inline std::string abcProgram() {
    namespace fs = std::filesystem;
    auto onPath = [](const std::string& name) -> std::string {
        if (name.find('/') != std::string::npos) {
            std::error_code ec;
            return fs::exists(name, ec) ? name : std::string();
        }
        const char* path = std::getenv("PATH");
        std::stringstream ss(path ? path : "");
        std::string dir;
        while (std::getline(ss, dir, ':')) {
            if (dir.empty()) continue;
            std::string p = dir + "/" + name;
            if (access(p.c_str(), X_OK) == 0) return p;
        }
        return std::string();
    };
    if (const char* e = std::getenv("MSYNTH_ABC")) return *e ? onPath(e) : std::string();
    for (const char* n : {"abc", "berkeley-abc", "yosys-abc"}) {
        std::string p = onPath(n);
        if (!p.empty()) return p;
    }
    return std::string();
}

struct AbcError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The design with its combinational logic restructured by `script`.
// The word operator and memory tables are not carried over.
inline aig::Design abcRestructure(const aig::Design& in, const std::string& program, const std::string& script) {
    using namespace aig;
    namespace fs = std::filesystem;
    for (char c : script)
        if (c == '\n' || c == '\r') throw AbcError("the ABC script must be one line");
    const Graph& g = in.g;
    const uint32_t N = (uint32_t)g.size();
    // AIGER variables: leaves (inputs and flop outputs) first, then the ANDs.
    std::vector<uint32_t> var(N, 0), leaves;
    uint32_t nv = 0;
    for (uint32_t n = 1; n < N; n++) {
        const Node& nd = g.nodes[n];
        if (nd.k == NK::Wire) throw AbcError("internal: the graph has wires");
        if (nd.k == NK::Input || nd.k == NK::Dff) {
            var[n] = ++nv;
            leaves.push_back(n);
        }
    }
    const uint32_t I = nv;
    std::vector<uint32_t> ands;
    for (uint32_t n = 1; n < N; n++)
        if (g.nodes[n].k == NK::And) {
            if (lnode(g.nodes[n].a) >= n || lnode(g.nodes[n].b) >= n) throw AbcError("internal: the graph is not in order");
            var[n] = ++nv;
            ands.push_back(n);
        }
    auto L = [&](Lit l) { return var[lnode(l)] * 2 + (lisneg(l) ? 1u : 0u); };
    // Outputs: the port bits, then per flop data, enable, clock, reset.
    std::vector<Lit> roots;
    for (auto& p : in.ports)
        if (p.out)
            for (Lit b : p.bits) roots.push_back(b);
    std::vector<uint8_t> hasEn(g.flops.size(), 0);
    for (size_t i = 0; i < g.flops.size(); i++) {
        const Flop& f = g.flops[i];
        Lit en = L1, data = f.d;
        if (flopEnable(g, f, en, data)) hasEn[i] = 1;
        else { en = L1; data = f.d; }
        roots.push_back(data);
        roots.push_back(en);
        roots.push_back(f.clk);
        roots.push_back(f.arst == NOLIT ? L0 : f.arst);
    }
    if (ands.empty()) return in;

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / ("msynth_abc_" + std::to_string((long)getpid()));
    fs::create_directories(dir, ec);
    struct Cleanup {
        fs::path d;
        ~Cleanup() { std::error_code e; fs::remove_all(d, e); }
    } cleanupDir{dir};
    std::string fin = (dir / "in.aig").string(), fout = (dir / "out.aig").string(), flog = (dir / "abc.log").string(),
                fscr = (dir / "run.abc").string();
    {
        std::ofstream f(fin, std::ios::binary);
        f << "aig " << nv << " " << I << " 0 " << roots.size() << " " << ands.size() << "\n";
        for (Lit r : roots) f << L(r) << "\n";
        for (uint32_t n : ands) {
            uint32_t lhs = var[n] * 2, r0 = L(g.nodes[n].a), r1 = L(g.nodes[n].b);
            if (r0 < r1) std::swap(r0, r1);
            for (uint32_t d : {lhs - r0, r0 - r1}) {
                while (d & ~0x7fu) {
                    f.put((char)((d & 0x7f) | 0x80));
                    d >>= 7;
                }
                f.put((char)d);
            }
        }
        if (!f) throw AbcError("cannot write " + fin);
    }
    {
        std::ofstream f(fscr);
        f << "read_aiger " << fin << "\n" << script << "\nwrite_aiger " << fout << "\n";
    }
    // run: program -f script, output to the log, no shell in between
    {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, flog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        std::string a0 = program, a1 = "-f", a2 = fscr;
        char* argv[] = {a0.data(), a1.data(), a2.data(), nullptr};
        pid_t pid = 0;
        int rc = posix_spawn(&pid, program.c_str(), &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) throw AbcError("cannot run " + program);
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || !fs::exists(fout, ec)) {
            std::ifstream lf(flog);
            std::string line, last;
            while (std::getline(lf, line))
                if (!line.empty()) last = line;
            throw AbcError(program + " failed" + (last.empty() ? "" : ": " + last));
        }
    }
    fpga::AigerDesign rd;
    try {
        rd = fpga::read_aiger_file(fout);
    } catch (std::exception& e) {
        throw AbcError(std::string("cannot read ABC's result: ") + e.what());
    }
    const Design& r = rd.design;
    // its ports: one bit each, inputs in our leaf order, outputs in our root order
    std::vector<Lit> rin, rout;
    for (auto& p : r.ports)
        for (Lit b : p.bits) (p.out ? rout : rin).push_back(b);
    if (rin.size() != I || rout.size() != roots.size() || !r.g.flops.empty())
        throw AbcError("ABC's result does not have the design's inputs and outputs");

    Design d;
    d.top = in.top;
    d.warnings = in.warnings;
    Graph& ng = d.g;
    std::vector<Lit> leafLit(N, NOLIT);
    for (uint32_t n = 1; n < N; n++) {
        if (g.nodes[n].k == NK::Input) leafLit[n] = ng.input(g.inputNames[g.nodes[n].aux]);
        else if (g.nodes[n].k == NK::Dff) leafLit[n] = ng.dff(g.flops[g.nodes[n].aux]);
    }
    std::vector<Lit> m(r.g.size(), NOLIT);
    m[0] = L0;
    for (uint32_t i = 0; i < I; i++) m[lnode(rin[i])] = lxorc(leafLit[leaves[i]], lisneg(rin[i]));
    auto tr = [&](Lit l) -> Lit {
        // the reader's graph is in order as well; walk it once, lazily
        std::vector<uint32_t> st{lnode(l)};
        while (!st.empty()) {
            uint32_t n = st.back();
            if (m[n] != NOLIT) { st.pop_back(); continue; }
            const Node& nd = r.g.nodes[n];
            if (nd.k != NK::And) throw AbcError("ABC's result has an unexpected node");
            uint32_t a = lnode(nd.a), b = lnode(nd.b);
            if (m[a] == NOLIT || m[b] == NOLIT) {
                if (m[a] == NOLIT) st.push_back(a);
                if (m[b] == NOLIT) st.push_back(b);
                continue;
            }
            m[n] = ng.andl(lxorc(m[a], lisneg(nd.a)), lxorc(m[b], lisneg(nd.b)));
            st.pop_back();
        }
        return lxorc(m[lnode(l)], lisneg(l));
    };
    size_t k = 0;
    for (auto& p : in.ports) {
        Port np = p;
        for (Lit& b : np.bits) b = p.out ? tr(rout[k++]) : leafLit[lnode(b)] ^ (b & 1u);
        d.ports.push_back(std::move(np));
    }
    for (size_t i = 0; i < ng.flops.size(); i++) {
        Flop& f = ng.flops[i];
        Lit data = tr(rout[k++]), en = tr(rout[k++]), clk = tr(rout[k++]), rst = tr(rout[k++]);
        f.d = hasEn[i] ? ng.muxRaw(en, data, f.q) : data;
        f.clk = clk;
        if (f.arst != NOLIT) f.arst = rst;
    }
    return d;
}

}  // namespace synth
