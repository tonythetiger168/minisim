#pragma once
// cdc_sdc.hpp - the part of SDC that the clock- and reset-domain crossing
// checks read (mlint -sdc).
//
// Runs the file on the shared Tcl interpreter (tcl.hpp) and keeps only what
// says where clocks start and how they are related:
//
//   create_clock, create_generated_clock     clocks and their sources
//   set_clock_groups                         asynchronous / exclusive groups
//   set_false_path                           (kept; a clock-to-clock false path
//                                            makes the pair asynchronous)
//   set_input_delay, set_output_delay        the clock of a port
//   set_case_analysis                        constant signals
//   get_ports, get_pins, get_cells, get_nets, get_clocks, all_clocks,
//   all_inputs, all_outputs                  object queries on the design
//
// Every other SDC command (timing values, loads, exceptions other than false
// paths, design rules) is accepted and ignored, since none of it changes
// which domain a signal is in. A command that is not SDC at all is reported.
//
// The design is seen through `Design`: the names of its ports, pins, cells
// and nets, the same names a timing tool shows (instance path with `/`).
#include <string>
#include <vector>

#include "tcl.hpp"

namespace cdcsdc {

enum class Kind { Port, Pin, Cell, Net, Clock };

struct Obj {
    Kind kind;
    std::string name;
};

struct Design {
    virtual ~Design() = default;
    virtual const std::vector<std::string>& names(Kind k) const = 0;
    virtual int portDir(const std::string&) const { return -1; }  // 0 in, 1 out, 2 inout
};

struct Clock {
    std::string name;
    std::vector<Obj> sources;  // none: a virtual clock
    bool generated = false;
    std::string masterClock;   // -master_clock
    std::vector<Obj> masterPins;  // -source
};

struct ClockGroups {
    enum Type { None, Asynchronous, Logical, Physical } type = None;
    std::vector<std::vector<std::string>> groups;  // clock names
};

struct Path {
    std::vector<Obj> from, to, through;
};

struct Exception {
    enum Type { FalsePath } type = FalsePath;
    Path path;
};

struct CaseAnalysis {
    std::string value;
    std::vector<Obj> objects;
};

struct IoDelay {
    bool input = true;
    std::string clock;
    std::vector<Obj> ports;
};

struct Constraints {
    std::vector<Clock> clocks;
    std::vector<ClockGroups> clockGroups;
    std::vector<Exception> exceptions;
    std::vector<CaseAnalysis> caseAnalysis;
    std::vector<IoDelay> ioDelays;
};

using Diag = tcl::Diag;

class Reader : public tcl::Interp {
public:
    const Design* design = nullptr;
    Constraints sdc;

    Reader() {
        busBrackets = true;
        addCommand("get_ports", [this](const W& w) { return query(w, Kind::Port); });
        addCommand("get_pins", [this](const W& w) { return query(w, Kind::Pin); });
        addCommand("get_cells", [this](const W& w) { return query(w, Kind::Cell); });
        addCommand("get_nets", [this](const W& w) { return query(w, Kind::Net); });
        addCommand("get_clocks", [this](const W& w) { return query(w, Kind::Clock); });
        addCommand("all_clocks", [this](const W&) {
            std::vector<std::string> out;
            for (auto& c : sdc.clocks) out.push_back(tag(Kind::Clock, c.name));
            return tcl::joinList(out);
        });
        addCommand("all_inputs", [this](const W& w) { return allPorts(w, true); });
        addCommand("all_outputs", [this](const W& w) { return allPorts(w, false); });
        addCommand("create_clock", [this](const W& w) { return createClock(w, false); });
        addCommand("create_generated_clock", [this](const W& w) { return createClock(w, true); });
        addCommand("set_clock_groups", [this](const W& w) { return clockGroups(w); });
        addCommand("set_false_path", [this](const W& w) { return falsePath(w); });
        addCommand("set_case_analysis", [this](const W& w) { return caseAnalysis(w); });
        addCommand("set_input_delay", [this](const W& w) { return ioDelay(w, true); });
        addCommand("set_output_delay", [this](const W& w) { return ioDelay(w, false); });
        for (const char* c : ignored) addCommand(c, [](const W&) { return std::string(); });
    }

private:
    using W = std::vector<std::string>;

    // Commands of SDC (2.1 and the common tool additions) that do not change
    // a signal's domain.
    static constexpr const char* ignored[] = {
        "current_design", "current_instance", "set_units", "set_hierarchy_separator", "sdc_version",
        "set_clock_uncertainty", "set_clock_latency", "set_clock_transition", "set_propagated_clock",
        "set_clock_gating_check", "set_clock_sense", "set_sense", "set_ideal_network", "set_ideal_latency",
        "set_ideal_transition", "set_max_delay", "set_min_delay", "set_multicycle_path", "set_data_check",
        "set_disable_timing", "set_load", "set_driving_cell", "set_drive", "set_input_transition",
        "set_fanout_load", "set_port_fanout_number", "set_max_transition", "set_max_capacitance",
        "set_min_capacitance", "set_max_fanout", "set_max_area", "set_max_time_borrow",
        "set_operating_conditions", "set_wire_load_model", "set_wire_load_mode",
        "set_wire_load_min_block_size", "set_wire_load_selection_group", "set_timing_derate",
        "set_resistance", "set_min_pulse_width", "set_max_dynamic_power", "set_max_leakage_power",
        "set_voltage", "set_level_shifter_strategy", "set_level_shifter_threshold", "create_voltage_area",
        "set_logic_dc", "set_logic_one", "set_logic_zero", "group_path", "set_dont_touch",
        "set_dont_touch_network", "set_dont_use", "set_size_only", "set_critical_range",
        "set_input_delay_ref", "set_clock_exclusivity", "set_mode", "current_mode", "remove_clock_groups",
        "get_libs", "get_lib_cells", "get_lib_pins", "all_registers", "all_fanin", "all_fanout",
    };

    static std::string tag(Kind k, const std::string& n) {
        static const char* p[] = {"@port:", "@pin:", "@cell:", "@net:", "@clock:"};
        return p[(int)k] + n;
    }

    void warn(const char* code, std::string text) { diags.push_back({Diag::Warning, code, where(), std::move(text)}); }
    [[noreturn]] void error(const std::string& text) { throw tcl::Error(text); }

    // name matches pattern: exactly, a bus by its base name, or (hierarchical)
    // the part after any `/`.
    static bool matches(const std::string& pat, const std::string& name, bool hier) {
        auto one = [&](const char* s) {
            if (tcl::globMatch(pat.c_str(), s, false)) return true;
            const char* b = strchr(s, '[');
            return b && std::string(s, b) == pat;  // bus d stands for d[3], d[2] ...
        };
        if (one(name.c_str())) return true;
        if (hier)
            for (size_t p = name.find('/'); p != std::string::npos; p = name.find('/', p + 1))
                if (one(name.c_str() + p + 1)) return true;
        return false;
    }

    std::string query(const W& w, Kind k) {
        bool hier = false, quiet = false;
        std::vector<std::string> pats;
        for (size_t i = 1; i < w.size(); i++) {
            const std::string& a = w[i];
            if (a == "-hierarchical" || a == "-hier") hier = true;
            else if (a == "-quiet") quiet = true;
            else if (a == "-regexp" || a == "-nocase" || a == "-exact" || a == "-include_generated_clocks") {}
            else if (a == "-of_objects" || a == "-filter") i++;  // not modeled
            else for (auto& p : tcl::splitList(a)) pats.push_back(p);
        }
        std::vector<std::string> out;
        for (const std::string& pat : pats) {
            size_t before = out.size();
            if (k == Kind::Clock) {
                for (auto& c : sdc.clocks)
                    if (tcl::globMatch(pat.c_str(), c.name.c_str(), true)) out.push_back(tag(k, c.name));
            } else if (design) {
                for (const std::string& n : design->names(k))
                    if (matches(pat, n, hier)) out.push_back(tag(k, n));
            }
            if (out.size() == before && !quiet)
                warn("no-match", w[0] + " " + pat + ": nothing matches");
        }
        return tcl::joinList(out);
    }

    std::string allPorts(const W& w, bool in) {
        std::vector<std::string> out;
        bool noClocks = false;
        for (size_t i = 1; i < w.size(); i++) if (w[i] == "-no_clocks") noClocks = true;
        if (design)
            for (const std::string& n : design->names(Kind::Port)) {
                int d = design->portDir(n);
                if (d != 2 && d != (in ? 0 : 1)) continue;
                if (noClocks && isClockSource(n)) continue;
                out.push_back(tag(Kind::Port, n));
            }
        return tcl::joinList(out);
    }
    bool isClockSource(const std::string& port) const {
        for (auto& c : sdc.clocks)
            for (auto& o : c.sources)
                if (o.kind == Kind::Port && o.name == port) return true;
        return false;
    }

    // A list of objects: query results, or bare names (a clock where a clock
    // is expected, otherwise a port, pin, net or cell, in that order).
    std::vector<Obj> objects(const std::string& list, bool clockFirst) {
        std::vector<Obj> out;
        for (const std::string& e : tcl::splitList(list)) {
            if (e.size() > 1 && e[0] == '@') {
                static const std::pair<const char*, Kind> kinds[] = {
                    {"@port:", Kind::Port}, {"@pin:", Kind::Pin}, {"@cell:", Kind::Cell},
                    {"@net:", Kind::Net}, {"@clock:", Kind::Clock}};
                bool done = false;
                for (auto& [p, k] : kinds)
                    if (e.rfind(p, 0) == 0) { out.push_back({k, e.substr(strlen(p))}); done = true; break; }
                if (done) continue;
            }
            bool found = false;
            if (clockFirst)
                for (auto& c : sdc.clocks)
                    if (tcl::globMatch(e.c_str(), c.name.c_str(), true)) { out.push_back({Kind::Clock, c.name}); found = true; }
            for (Kind k : {Kind::Port, Kind::Pin, Kind::Net, Kind::Cell}) {
                if (found || !design) break;
                for (const std::string& n : design->names(k))
                    if (matches(e, n, false)) { out.push_back({k, n}); found = true; }
            }
            if (!found) warn("no-match", e + ": nothing matches");
        }
        return out;
    }
    std::string clockName(const std::string& word) {
        auto o = objects(word, true);
        for (auto& x : o) if (x.kind == Kind::Clock) return x.name;
        return word.rfind("@clock:", 0) == 0 ? word.substr(7) : word;
    }

    // The words of a command: options (with the value they take) and the rest.
    struct Args {
        std::map<std::string, std::string> opt;
        std::vector<std::string> pos;
        std::vector<std::pair<std::string, std::string>> all;  // in order, for repeated options
        bool has(const std::string& o) const { return opt.count(o); }
        std::string get(const std::string& o) const { auto it = opt.find(o); return it == opt.end() ? "" : it->second; }
    };
    Args args(const W& w, std::initializer_list<const char*> withValue) {
        Args a;
        for (size_t i = 1; i < w.size(); i++) {
            const std::string& x = w[i];
            if (x.size() > 1 && x[0] == '-' && !isdigit((unsigned char)x[1]) && x[1] != '.') {
                bool val = false;
                for (const char* v : withValue) val = val || x == v;
                std::string v = val && i + 1 < w.size() ? w[++i] : "";
                a.opt[x] = v;
                a.all.push_back({x, v});
            } else {
                a.pos.push_back(x);
            }
        }
        return a;
    }

    std::string createClock(const W& w, bool gen) {
        Args a = args(w, {"-name", "-period", "-waveform", "-source", "-master_clock", "-divide_by", "-multiply_by",
                          "-edges", "-edge_shift", "-duty_cycle", "-comment"});
        Clock c;
        c.generated = gen;
        for (auto& p : a.pos)
            for (auto& o : objects(p, false)) c.sources.push_back(o);
        c.name = a.get("-name");
        if (c.name.empty()) {
            if (c.sources.empty()) error(w[0] + ": a clock with no source needs -name");
            c.name = c.sources[0].name;
        }
        if (gen) {
            if (a.has("-master_clock")) c.masterClock = clockName(a.get("-master_clock"));
            if (a.has("-source")) c.masterPins = objects(a.get("-source"), false);
            if (c.masterClock.empty() && c.masterPins.empty()) error(w[0] + ": needs -source");
        }
        // A clock of the same name without -add replaces the old one.
        for (auto it = sdc.clocks.begin(); it != sdc.clocks.end(); ++it)
            if (it->name == c.name) { *it = c; return tag(Kind::Clock, c.name); }
        sdc.clocks.push_back(c);
        return tag(Kind::Clock, c.name);
    }

    std::string clockGroups(const W& w) {
        Args a = args(w, {"-name", "-group", "-comment"});
        ClockGroups g;
        if (a.has("-asynchronous")) g.type = ClockGroups::Asynchronous;
        else if (a.has("-logically_exclusive")) g.type = ClockGroups::Logical;
        else if (a.has("-physically_exclusive")) g.type = ClockGroups::Physical;
        else error("set_clock_groups: needs -asynchronous, -logically_exclusive or -physically_exclusive");
        for (auto& [o, v] : a.all) {
            if (o != "-group") continue;
            std::vector<std::string> names;
            for (auto& x : objects(v, true))
                if (x.kind == Kind::Clock) names.push_back(x.name);
            g.groups.push_back(std::move(names));
        }
        if (g.groups.empty()) error("set_clock_groups: needs -group");
        sdc.clockGroups.push_back(std::move(g));
        return "";
    }

    std::string falsePath(const W& w) {
        Args a = args(w, {"-from", "-to", "-through", "-rise_from", "-fall_from", "-rise_to", "-fall_to",
                          "-rise_through", "-fall_through", "-comment"});
        Exception e;
        for (auto& [o, v] : a.all) {
            if (o.find("from") != std::string::npos) for (auto& x : objects(v, true)) e.path.from.push_back(x);
            else if (o.find("through") != std::string::npos) for (auto& x : objects(v, false)) e.path.through.push_back(x);
            else if (o.find("to") != std::string::npos) for (auto& x : objects(v, true)) e.path.to.push_back(x);
        }
        sdc.exceptions.push_back(std::move(e));
        return "";
    }

    std::string caseAnalysis(const W& w) {
        if (w.size() < 3) error("set_case_analysis: expected VALUE OBJECTS");
        CaseAnalysis c;
        c.value = w[1];
        for (size_t i = 2; i < w.size(); i++)
            for (auto& o : objects(w[i], false)) c.objects.push_back(o);
        sdc.caseAnalysis.push_back(std::move(c));
        return "";
    }

    std::string ioDelay(const W& w, bool in) {
        Args a = args(w, {"-clock", "-reference_pin", "-comment"});
        IoDelay d;
        d.input = in;
        if (a.has("-clock")) d.clock = clockName(a.get("-clock"));
        // The first word left is the delay; the next ones are the ports.
        for (size_t i = 1; i < a.pos.size(); i++)
            for (auto& o : objects(a.pos[i], false)) d.ports.push_back(o);
        sdc.ioDelays.push_back(std::move(d));
        return "";
    }
};

}  // namespace cdcsdc
