#pragma once
// upf_vct.hpp - value conversion tables (IEEE 1801-2024 4.10, 6.18, 6.28,
// Annex B).
//
// A supply net carries a supply state (and a voltage); an HDL port that is
// not of a supply type carries a logic value. Where connect_supply_net
// joins the two, a value conversion table (VCT) says how one becomes the
// other:
//
//   hdl2upf  the HDL port drives the supply net (a regulator model's output)
//   upf2hdl  the supply net drives the HDL port (a macro's VDD input)
//
// The tables of Annex B are predefined; create_hdl2upf_vct and
// create_upf2hdl_vct add more. This file holds the tables and the
// conversion only; upf.hpp reads the commands and upf_rt.inc applies them.
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace upf {

struct Vct {
    std::string name, where;
    bool hdl2upf = true;
    std::string lang = "sv";   // sv or vhdl
    std::string hdlType;       // the typename of -hdl_type, if one was given
    // hdl2upf: HDL value -> supply state name; upf2hdl: supply state name -> HDL value
    std::vector<std::pair<std::string, std::string>> table;

    static std::string upper(std::string s) {
        for (char& c : s) c = (char)std::toupper((unsigned char)c);
        return s;
    }
    // A supply state name as its number (upfSupplyStateE), or -1.
    static int stateNum(const std::string& s) {
        std::string u = upper(s);
        if (u == "OFF") return 0;
        if (u == "FULL_ON") return 1;
        if (u == "PARTIAL_ON") return 2;
        if (u == "UNDETERMINED") return 3;
        return -1;
    }
    // An HDL value of a table, as one character: 0 1 X Z for SystemVerilog,
    // U X 0 1 Z W L H - for VHDL std_logic. Quotes ('1') are dropped. 0 if
    // it is not a value of the language.
    static char hdlChar(const std::string& v, const std::string& lang) {
        std::string t;
        for (char c : v) if (c != '\'' && c != '"') t += c;
        if (t.size() != 1) return 0;
        char c = (char)std::toupper((unsigned char)t[0]);
        const char* ok = lang == "vhdl" ? "UX01ZWLH-" : "01XZ";
        for (const char* p = ok; *p; p++) if (*p == c) return c;
        return 0;
    }
    // hdl2upf: the supply state for an HDL value; UNDETERMINED when the
    // table has no row for it.
    int toUpf(char hdl) const {
        for (auto& [from, to] : table)
            if (hdlChar(from, lang) == (char)std::toupper((unsigned char)hdl)) return stateNum(to);
        return 3;
    }
    // upf2hdl: the HDL value for a supply state; X when the table has no row.
    char toHdl(int state) const {
        for (auto& [from, to] : table)
            if (stateNum(from) == state) return hdlChar(to, lang);
        return 'X';
    }
};

// A value conversion method (6.30): a table for one HDL type and one
// direction, an HDL function or module that converts, or a list of VCMs to
// choose from per port (-vcms). Single-bit tables convert through a Vct.
struct Vcm {
    enum Kind { Table, Function, Model, Bundle } kind = Table;
    std::string name, where;          // name: "lib::name"
    bool hdl2upf = true;
    std::string lang = "sv", hdlType, field;
    // The HDL side of a table: b single bit, r real, i integer, e enum
    char cat = 'b';
    Vct bits;                         // cat b
    struct Row {
        // HDL side. hdl2upf: what matches; upf2hdl: what is driven.
        enum H { Any, Range, Exact, XReal, ZReal, Label, Pass } h = Exact;
        double lo = -INFINITY, hi = INFINITY, num = 0;   // Range [lo, hi); Exact num
        std::string label;
        // UPF side: the state, and its voltage: none, a value, '*' (the HDL
        // value), or (upf2hdl) a range [vlo, vhi)
        int st = 3;
        enum V { None, Value, Star, VRange } vk = None;
        double v = 0, vlo = -INFINITY, vhi = INFINITY;
    };
    std::vector<Row> rows;
    std::string function, model;
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<const Vcm*> members;  // Bundle

    // Xreal and Zreal of package UPF (Annex D: $bitstoreal of these).
    static constexpr uint64_t XrealBits = 0xFFFFFFFE00000000ull, ZrealBits = 0xFFFFFFFF00000000ull;
    static double bitsReal(uint64_t b) {
        double d;
        std::memcpy(&d, &b, sizeof d);
        return d;
    }
    // Every Table VCM, flattened out of nested lists.
    void flatten(std::vector<const Vcm*>& out) const {
        if (kind != Bundle) { out.push_back(this); return; }
        for (auto* m : members) m->flatten(out);
    }
    // hdl2upf: the HDL value (x: unknown, isX/isZ: Xreal/Zreal or x/z bits)
    // to a supply state, and the voltage to set (setV); UNDETERMINED and no
    // voltage when no row matches (6.30.3 i).
    void toUpf(double x, const std::string& label, bool unknown, uint64_t raw, int& st, bool& setV, double& v) const {
        st = 3;
        setV = false;
        for (const Row& r : rows) {
            bool hit = false;
            switch (r.h) {
                case Row::Any: hit = true; break;
                case Row::Range: hit = !unknown && x >= r.lo && x < r.hi; break;
                case Row::Exact: hit = !unknown && x == r.num; break;
                case Row::XReal: hit = raw == XrealBits; break;
                case Row::ZReal: hit = raw == ZrealBits; break;
                case Row::Label: hit = !unknown && label == r.label; break;
                default: break;
            }
            if (!hit) continue;
            st = r.st;
            if (r.vk == Row::Value) { setV = true; v = r.v; }
            else if (r.vk == Row::Star && !unknown) { setV = true; v = x; }
            return;
        }
    }
    // upf2hdl: the row for a supply state and voltage (volts), or null.
    const Row* toHdl(int st, double volts) const {
        for (const Row& r : rows) {
            if (r.st != st) continue;
            if (r.vk == Row::Value && std::fabs(r.v - volts) > 1e-9) continue;
            if (r.vk == Row::VRange && !(volts >= r.vlo && volts < r.vhi)) continue;
            return &r;
        }
        return nullptr;
    }
};

// The predefined VCTs of Annex B.
inline const std::map<std::string, Vct>& predefinedVcts() {
    static const std::map<std::string, Vct> all = [] {
        std::map<std::string, Vct> m;
        using T = std::vector<std::pair<std::string, std::string>>;
        auto add = [&](const char* name, bool h2u, const char* lang, T t) {
            Vct v;
            v.name = name;
            v.where = "Annex B";
            v.hdl2upf = h2u;
            v.lang = lang;
            v.table = std::move(t);
            m[name] = v;
        };
        const char* U = "UNDETERMINED";
        // B.2, B.3: VHDL std_logic, power is '1'
        add("VHDL_SL2UPF", true, "vhdl",
            {{"U", U}, {"X", U}, {"0", "OFF"}, {"1", "FULL_ON"}, {"Z", U}, {"W", U}, {"L", "OFF"}, {"H", "FULL_ON"}, {"-", U}});
        add("UPF2VHDL_SL", false, "vhdl", {{"OFF", "0"}, {"FULL_ON", "1"}, {"PARTIAL_ON", "X"}, {"UNDETERMINED", "X"}});
        // B.4, B.5: VHDL std_logic, a ground: on is '0'
        add("VHDL_SL2UPF_GNDZERO", true, "vhdl",
            {{"U", U}, {"X", U}, {"0", "FULL_ON"}, {"1", "OFF"}, {"Z", U}, {"W", U}, {"L", "FULL_ON"}, {"H", "OFF"}, {"-", U}});
        add("UPF_GNDZERO2VHDL_SL", false, "vhdl", {{"OFF", "1"}, {"FULL_ON", "0"}, {"PARTIAL_ON", "X"}, {"UNDETERMINED", "X"}});
        // B.6, B.7: SystemVerilog logic, power is 1
        add("SV_LOGIC2UPF", true, "sv", {{"X", U}, {"Z", U}, {"0", "OFF"}, {"1", "FULL_ON"}});
        add("UPF2SV_LOGIC", false, "sv", {{"OFF", "0"}, {"FULL_ON", "1"}, {"PARTIAL_ON", "X"}, {"UNDETERMINED", "X"}});
        // B.8, B.9: SystemVerilog logic, a ground: on is 0
        add("SV_LOGIC2UPF_GNDZERO", true, "sv", {{"X", U}, {"Z", U}, {"0", "FULL_ON"}, {"1", "OFF"}});
        add("UPF_GNDZERO2SV_LOGIC", false, "sv", {{"OFF", "1"}, {"FULL_ON", "0"}, {"PARTIAL_ON", "X"}, {"UNDETERMINED", "X"}});
        // B.10 to B.13: an HDL port tied to a supply (upf2hdl): the tie value while
        // the supply is FULL_ON, X otherwise. VHDL_TIED_LO maps PARTIAL_ON to '0' as
        // 1801-2024 Annex B prints it.
        add("SV_TIED_HI", false, "sv", {{"UNDETERMINED", "X"}, {"FULL_ON", "1"}, {"PARTIAL_ON", "X"}, {"OFF", "X"}});
        add("SV_TIED_LO", false, "sv", {{"UNDETERMINED", "X"}, {"FULL_ON", "0"}, {"PARTIAL_ON", "X"}, {"OFF", "X"}});
        add("VHDL_TIED_HI", false, "vhdl", {{"UNDETERMINED", "X"}, {"FULL_ON", "1"}, {"PARTIAL_ON", "X"}, {"OFF", "X"}});
        add("VHDL_TIED_LO", false, "vhdl", {{"UNDETERMINED", "X"}, {"FULL_ON", "0"}, {"PARTIAL_ON", "0"}, {"OFF", "X"}});
        return m;
    }();
    return all;
}

}   // namespace upf
