// vpi_covmerge.inc - vpi_control(vpiCoverageMerge, type, name) (IEEE
// 1800-2023 40.5.1): add the counts of a coverage database that msim wrote
// (-cov, or vpiCoverageSave) to this run's counts of one coverage type.
// #included in vpi.inc, inside namespace vpi.

// A small JSON reader for the database ("format": "msim-cov").
struct CovJ {
    enum K { Null, Num, Str, Arr, Obj } k = Null;
    std::string s;   // a string, or the text of a number
    std::vector<CovJ> a;
    std::vector<std::pair<std::string, CovJ>> o;
    const CovJ* get(const char* key) const {
        for (auto& [n, v] : o)
            if (n == key) return &v;
        return nullptr;
    }
    uint64_t u() const { return k == Num ? std::strtoull(s.c_str(), nullptr, 10) : 0; }
};
struct CovJReader {
    const std::string& t;
    size_t p = 0;
    bool bad = false;
    void ws() { while (p < t.size() && std::isspace((unsigned char)t[p])) p++; }
    std::string str() {
        std::string r;
        p++;   // "
        while (p < t.size() && t[p] != '"') {
            if (t[p] == '\\' && p + 1 < t.size()) {
                char c = t[++p];
                if (c == 'n') r += '\n';
                else if (c == 't') r += '\t';
                else if (c == 'u' && p + 4 < t.size()) {   // msim writes \u00XX only
                    r += (char)std::strtol(t.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                } else r += c;
            } else r += t[p];
            p++;
        }
        if (p >= t.size()) bad = true;
        p++;
        return r;
    }
    CovJ value(int depth = 0) {
        CovJ j;
        ws();
        if (p >= t.size() || depth > 64) { bad = true; return j; }
        char c = t[p];
        if (c == '{' || c == '[') {
            j.k = c == '{' ? CovJ::Obj : CovJ::Arr;
            char end = c == '{' ? '}' : ']';
            p++;
            ws();
            if (p < t.size() && t[p] == end) { p++; return j; }
            while (!bad) {
                ws();
                if (j.k == CovJ::Obj) {
                    if (p >= t.size() || t[p] != '"') { bad = true; break; }
                    std::string key = str();
                    ws();
                    if (p >= t.size() || t[p] != ':') { bad = true; break; }
                    p++;
                    j.o.emplace_back(std::move(key), value(depth + 1));
                } else j.a.push_back(value(depth + 1));
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == end) { p++; break; }
                bad = true;
            }
            return j;
        }
        if (c == '"') { j.k = CovJ::Str; j.s = str(); return j; }
        if (c == '-' || std::isdigit((unsigned char)c)) {
            j.k = CovJ::Num;
            size_t b = p;
            while (p < t.size() && (std::isdigit((unsigned char)t[p]) || std::strchr("-+.eE", t[p]))) p++;
            j.s = t.substr(b, p - b);
            return j;
        }
        for (const char* w : {"null", "true", "false"})
            if (t.compare(p, std::strlen(w), w) == 0) { p += std::strlen(w); return j; }
        bad = true;
        return j;
    }
};
// MSB-first hex (as the database writes toggles) ORed into a bit mask.
inline void covOrHex(std::vector<uint64_t>& m, const std::string& h, int w) {
    if (m.size() < (size_t)(w + 63) / 64) m.resize((size_t)(w + 63) / 64, 0);
    int n = (int)h.size();
    for (int d = 0; d < n; d++) {
        char c = h[(size_t)(n - 1 - d)];
        uint64_t v = std::isdigit((unsigned char)c) ? (uint64_t)(c - '0') : (uint64_t)(std::tolower((unsigned char)c) - 'a' + 10);
        for (int b = 0; b < 4; b++) {
            int bit = d * 4 + b;
            if (bit < w && ((v >> b) & 1)) m[(size_t)bit / 64] |= 1ULL << (bit % 64);
        }
    }
}
inline bool covMerge(int type, const std::string& name) {
    std::ifstream f(name);
    if (!f) { error(vpiError, "vpi_control: cannot read the coverage database " + name); return false; }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CovJReader r{text};
    CovJ db = r.value();
    const CovJ* fmt = db.get("format");
    if (r.bad || db.k != CovJ::Obj || !fmt || fmt->s != "msim-cov") {
        error(vpiError, "vpi_control: " + name + " is not a coverage database msim wrote");
        return false;
    }
    auto list = [&](const char* key) -> const std::vector<CovJ>& {
        static const std::vector<CovJ> none;
        const CovJ* v = db.get(key);
        return v && v->k == CovJ::Arr ? v->a : none;
    };
    auto& s = st();
    if (type == vpiStatementCoverage) {   // [file, line, hits]: every statement of that line
        std::vector<uint32_t> fileId;
        for (const CovJ& x : list("files")) fileId.push_back(covFileId(x.s));
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> add;
        for (const CovJ& x : list("lines"))
            if (x.a.size() == 3 && x.a[0].u() < fileId.size()) add[{fileId[(size_t)x.a[0].u()], (uint32_t)x.a[1].u()}] += x.a[2].u();
        for (const Prog* p : s.progs)
            for (const Instr& i : p->code) {
                if (!i.line) continue;
                auto it = add.find({i.file, i.line});
                if (it != add.end()) i.hits += it->second;
            }
        return true;
    }
    if (type == vpiToggleCoverage) {   // [name, width, rise, fall]
        std::string top = S.vcdTop.empty() ? std::string() : S.vcdTop + ".";
        std::unordered_map<std::string, size_t> byName;
        for (size_t k = 0; k < S.sigs.size(); k++)
            if (!S.sigs[k].hide) byName.emplace(top + S.sigs[k].name, k);
        if (S.togRise.size() < S.sigs.size()) S.togRise.resize(S.sigs.size());
        if (S.togFall.size() < S.sigs.size()) S.togFall.resize(S.sigs.size());
        for (const CovJ& x : list("toggles")) {
            if (x.a.size() != 4) continue;
            auto it = byName.find(x.a[0].s);
            if (it == byName.end() || (int)x.a[1].u() != S.sigs[it->second].w) continue;
            covOrHex(S.togRise[it->second], x.a[2].s, S.sigs[it->second].w);
            covOrHex(S.togFall[it->second], x.a[3].s, S.sigs[it->second].w);
        }
        return true;
    }
    if (type == vpiFsmStateCoverage) {   // states [value, name, hits], trans [from, to, hits, reset]
        if (S.fsmSeen.size() < fsmDefs().size()) S.fsmSeen.resize(fsmDefs().size());
        if (S.fsmTrans.size() < fsmDefs().size()) S.fsmTrans.resize(fsmDefs().size());
        for (const CovJ& x : list("fsms")) {
            const CovJ* inst = x.get("inst");
            const CovJ* nm = x.get("name");
            if (!inst || !nm) continue;
            for (size_t k = 0; k < fsmDefs().size(); k++) {
                if (fsmDefs()[k].inst != inst->s || fsmDefs()[k].name != nm->s) continue;
                if (const CovJ* st = x.get("states"))
                    for (const CovJ& v : st->a)
                        if (v.a.size() == 3 && v.a[2].u()) S.fsmSeen[k][v.a[0].u()] += v.a[2].u();
                for (const char* key : {"trans", "extra"})
                    if (const CovJ* tr = x.get(key))
                        for (const CovJ& v : tr->a)
                            if (v.a.size() >= 3 && v.a[2].u()) S.fsmTrans[k][{v.a[0].u(), v.a[1].u()}] += v.a[2].u();
            }
        }
        return true;
    }
    // vpiAssertCoverage: [name, kind, attempts, successes, failures]
    std::unordered_map<std::string, size_t> byName;
    for (size_t k = 0; k < asserts().size(); k++) byName.emplace(asserts()[k].name, k);
    for (const CovJ& x : list("assertions")) {
        if (x.a.size() != 5) continue;
        auto it = byName.find(x.a[0].s);
        if (it == byName.end()) continue;
        AssertInfo& a = asserts()[it->second];
        a.evals += x.a[2].u();
        a.passes += x.a[3].u();
        a.fails += x.a[4].u();
        if (it->second < s.assertCov.size()) {
            s.assertCov[it->second][0] += x.a[2].u();
            s.assertCov[it->second][1] += x.a[3].u();
            s.assertCov[it->second][2] += x.a[4].u();
        }
    }
    return true;
}
