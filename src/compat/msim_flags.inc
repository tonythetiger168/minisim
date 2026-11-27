// Vendor compatibility layer (built only with -DMINISIM_VENDOR_COMPAT=ON;
// docs/compat/COMPAT.md): another simulator's spellings of msim's own options,
// rewritten to the native spelling before msim parses its command line. One
// engine: this only renames.
#include <cctype>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace vendorflags {

inline std::vector<std::string> toNative(int argc, char** argv) {
    std::vector<std::string> out;
    out.reserve(argc + 4);
    if (argc > 0) out.push_back(argv[0]);
    bool partcompNoted = false;
    // Another simulator's coverage metric notation (src/cov_select.hpp) as
    // msim's comma list; an unreadable one is passed on for msim to reject.
    auto metrics = [](const std::string& v) {
        uint32_t m = 0;
        std::string err, out;
        if (!covsel::parseMetrics(v, m, err)) return std::string("?") + v;
        const std::pair<uint32_t, const char*> names[] = {{covsel::LINE, "line"}, {covsel::BRANCH, "branch"},
                                                          {covsel::COND, "cond"}, {covsel::TGL, "toggle"},
                                                          {covsel::FSM, "fsm"}};
        for (auto& [bit, name] : names)
            if (m & bit) out += (out.empty() ? "" : ",") + std::string(name);
        return out.empty() ? std::string("none") : out;
    };
    auto plusToComma = [](std::string v) {
        for (auto& c : v) if (c == '+') c = ',';
        return v;
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool hasNext = i + 1 < argc;
        if (a == "-sverilog") out.push_back("-sv");
        else if (a == "-ucli") out.push_back("-i");
        else if (a == "-xprop=tmerge") out.push_back("-xprop=merge");
        else if (a == "-xprop=xmerge") out.push_back("-xprop=pessimistic");
        else if (a == "-xprop=vmerge") out.push_back("-xprop=off");
        else if (a == "-power_top") out.push_back("-upf_top");
        // VCS partition compile: compiled mode, where msim chooses its own parts
        // and does not rebuild an unchanged one.
        else if (a == "-partcomp" || a.starts_with("-partcomp=") || a == "-fastpartcomp" ||
                 a.starts_with("-fastpartcomp=")) {
            out.push_back("-compile");
            if (a.starts_with("-fastpartcomp=j") && a.size() > 15 && std::isdigit((unsigned char)a[15])) {
                out.push_back("-cfiles");
                out.push_back(a.substr(15));
            }
            if (!partcompNoted) {
                std::cerr << "msim: " << a << ": compiled mode; msim chooses its own parts, and an unchanged part is not rebuilt\n";
                partcompNoted = true;
            }
        }
        else if (a.starts_with("-partcomp_dir=") || a.starts_with("-Mdir=")) {
            out.push_back("-o");
            out.push_back(a.substr(a.find('=') + 1));
        }
        else if (a == "-partcomp_sharedlib" || a == "-pcmakeprof") {}   // msim links its parts into one simv
        else if (a.starts_with("+ntb_random_seed=")) { out.push_back("-seed"); out.push_back(a.substr(17)); }
        else if (a == "-cm" && hasNext) out.push_back("-cov_metrics=" + metrics(argv[++i]));
        else if (a.starts_with("-cm=")) out.push_back("-cov_metrics=" + metrics(a.substr(4)));
        else if (a == "-cm_hier") out.push_back("-cov_scope");
        else if (a == "-cm_cond" && hasNext) { out.push_back("-cov_cond"); out.push_back(plusToComma(argv[++i])); }
        else if (a == "-dpiheader") out.push_back("-dpi_header");
        else if (a == "-load" || a == "-loadvpi") out.push_back("-vpi");
        else if (a == "-uvmhome") out.push_back("-uvm_home");
        else out.push_back(a);
    }
    return out;
}

}  // namespace vendorflags
