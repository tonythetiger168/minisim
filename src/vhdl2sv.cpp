// vhdl2sv - print the SystemVerilog msim simulates for VHDL files (debug aid;
// msim does the same translation in memory).
#include <iostream>

#include "vhdl/vhdl.hpp"

int main(int argc, char** argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) files.push_back(argv[i]);
    if (files.empty()) { std::cerr << "usage: vhdl2sv file.vhd...\n"; return 2; }
    try {
        vhdl::Translator tr;
        vhdl::readFiles(tr, files);
        std::string sv = tr.run();
        for (auto& w : tr.warnings) std::cerr << "warning: " << w << "\n";
        std::cout << sv;
    } catch (const vhdl::Error& e) {
        std::cerr << "vhdl2sv: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
