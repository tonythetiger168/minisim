// vhdl/vhdl.hpp - msim's VHDL front end: read VHDL files, translate the
// design units into SystemVerilog text for the one msim engine.
#pragma once

#include <fstream>
#include <sstream>

#include "cond.hpp"
#include "sv.hpp"

namespace vhdl {

inline bool isVhdlFile(const std::string& f) {
    std::string l = lowerStr(f);
    auto ends = [&](const char* e) { std::string x(e); return l.size() > x.size() && l.compare(l.size() - x.size(), x.size(), x) == 0; };
    return ends(".vhd") || ends(".vhdl") || ends(".vho") || ends(".vht");
}

// Parse every file (in order) into the translator. Throws vhdl::Error.
inline void readFiles(Translator& tr, const std::vector<std::string>& files) {
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) throw Error("cannot read " + f);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string src = ss.str();
        if (src.find('`') != std::string::npos) {   // conditional analysis (24.2)
            CondAnalysis ca(condAnalysisIds(tr.condIds), f);
            src = ca.run(src);
            tr.warnings.insert(tr.warnings.end(), ca.warnings.begin(), ca.warnings.end());
        }
        Lexer lx(src, f);
        Parser p(lx.run(), f);
        tr.add(p.designFile());
    }
}

}  // namespace vhdl
