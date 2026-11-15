// vhdl/vhdl.hpp - msim's VHDL front end: read VHDL files, translate the
// design units into SystemVerilog text for the one msim engine.
#pragma once

#include <fstream>
#include <sstream>

#include "cond.hpp"
#include "reflection.hpp"
#include "sv.hpp"

namespace vhdl {

inline bool isVhdlFile(const std::string& f) {
    std::string l = lowerStr(f);
    auto ends = [&](const char* e) { std::string x(e); return l.size() > x.size() && l.compare(l.size() - x.size(), x.size(), x) == 0; };
    return ends(".vhd") || ends(".vhdl") || ends(".vho") || ends(".vht");
}

// Parse every file (in order) into the translator. Throws vhdl::Error.
inline void readFiles(Translator& tr, const std::vector<std::string>& files) {
    std::vector<std::string> srcs;
    bool reflect = false;
    for (auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) throw Error("cannot read " + f);
        std::stringstream ss;
        ss << in.rdbuf();
        srcs.push_back(ss.str());
        std::string l = srcs.back();
        for (auto& c : l) c = (char)std::tolower((unsigned char)c);
        reflect |= l.find("reflect") != std::string::npos;
    }
    // std.reflection (16.12) is VHDL source of its own, analysed first
    if (reflect) {
        Lexer lx(reflectionSource(), "std.reflection");
        Parser p(lx.run(), "std.reflection");
        tr.add(p.designFile());
    }
    for (size_t fi = 0; fi < files.size(); ++fi) {
        const std::string& f = files[fi];
        std::string src = srcs[fi];
        if (src.find('`') != std::string::npos) {   // conditional analysis (24.2)
            CondAnalysis ca(condAnalysisIds(tr.condIds), f);
            src = ca.run(src);
            tr.warnings.insert(tr.warnings.end(), ca.warnings.begin(), ca.warnings.end());
        }
        {   // std.env's GET_CALL_PATH needs a frame in every subprogram
            std::string l = src;
            for (auto& c : l) c = (char)std::tolower((unsigned char)c);
            if (l.find("get_call_path") != std::string::npos) tr.callPath = true;
        }
        Lexer lx(src, f);
        Parser p(lx.run(), f);
        tr.add(p.designFile());
    }
}

}  // namespace vhdl
