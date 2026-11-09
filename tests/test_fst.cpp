// Waveform checker for msim's FST output: prints a waveform as one line per
// value change, "<time> <scope.path.name> <bits>", sorted, so an FST file and
// a VCD file of the same run can be compared line for line.
//   test_fst <file.fst | file.vcd>
// The FST side uses libfst's reader; the VCD side is a small parser for the
// subset msim writes ($scope / $var / $upscope, #time, scalar and b-vector).
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "fstapi.h"

static std::vector<std::string> out;

static int fromFst(const char* path) {
    fstReaderContext* r = fstReaderOpen(path);
    if (!r) { std::cerr << "cannot open " << path << "\n"; return 1; }
    std::map<fstHandle, std::vector<std::string>> names;
    std::vector<std::string> scope;
    while (fstHier* h = fstReaderIterateHier(r)) {
        if (h->htyp == FST_HT_SCOPE) scope.push_back(h->u.scope.name);
        else if (h->htyp == FST_HT_UPSCOPE) scope.pop_back();
        else if (h->htyp == FST_HT_VAR) {
            std::string n;
            for (auto& s : scope) n += s + ".";
            names[h->u.var.handle].push_back(n + h->u.var.name);
        }
    }
    fstReaderSetFacProcessMaskAll(r);
    struct Ctx { std::map<fstHandle, std::vector<std::string>>* names; } ctx{&names};
    fstReaderIterBlocks(
        r,
        [](void* p, uint64_t t, fstHandle h, const unsigned char* v) {
            auto* c = (Ctx*)p;
            for (auto& n : (*c->names)[h]) out.push_back(std::to_string(t) + " " + n + " " + (const char*)v);
        },
        &ctx, nullptr);
    fstReaderClose(r);
    return 0;
}

static int fromVcd(const char* path) {
    std::ifstream in(path);
    if (!in) { std::cerr << "cannot open " << path << "\n"; return 1; }
    std::map<std::string, std::vector<std::pair<std::string, int>>> ids;  // id -> (name, width)
    std::vector<std::string> scope;
    std::string tok;
    uint64_t t = 0;
    while (in >> tok) {
        if (tok == "$scope") { std::string kind, name; in >> kind >> name >> tok; scope.push_back(name); }
        else if (tok == "$upscope") { in >> tok; scope.pop_back(); }
        else if (tok == "$var") {
            std::string kind, id, name; int w;
            in >> kind >> w >> id >> name >> tok;
            std::string n;
            for (auto& s : scope) n += s + ".";
            ids[id].push_back({n + name, w});
        } else if (tok[0] == '$') {
            if (tok == "$dumpvars" || tok == "$end") continue;
            while (in >> tok && tok != "$end") {}
        } else if (tok[0] == '#') t = std::stoull(tok.substr(1));
        else {
            std::string val, id;
            if (tok[0] == 'b') { val = tok.substr(1); in >> id; }
            else { val = tok.substr(0, 1); id = tok.substr(1); }
            for (auto& [n, w] : ids[id]) {
                std::string v = val;
                // VCD may drop leading digits; msim writes every bit, but pad anyway.
                if ((int)v.size() < w) v = std::string(w - v.size(), v[0] == '1' ? '0' : v[0]) + v;
                out.push_back(std::to_string(t) + " " + n + " " + v);
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: test_fst <file.fst|file.vcd>\n"; return 2; }
    std::string p = argv[1];
    int rc = p.size() > 4 && p.substr(p.size() - 4) == ".vcd" ? fromVcd(argv[1]) : fromFst(argv[1]);
    if (rc) return rc;
    std::stable_sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        uint64_t ta = std::stoull(a.substr(0, a.find(' '))), tb = std::stoull(b.substr(0, b.find(' ')));
        return ta != tb ? ta < tb : a < b;
    });
    for (auto& l : out) std::cout << l << "\n";
    return 0;
}
