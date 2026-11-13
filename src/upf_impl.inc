// Implementation-only UPF commands (IEEE 1801-2024 6.37 to 6.41, 6.43,
// 6.57, 6.62 and clause 7). They tell an implementation tool which library
// cells to use and how names are made; a simulation of the RTL does not
// change. They are read, checked against the errors the standard lists and
// kept in Model::implCmds, so that a mistake in them is reported where it is
// written instead of in the implementation flow. Included in upf.hpp inside
// namespace upf, after struct Model.

inline bool Model::implCommand(const Cmd& c, const std::string& scope) {
    const std::string& n = c.w[0];
    struct Spec {
        const char* clause;
        std::set<std::string> values, flags;
        size_t minPos, maxPos;
    };
    static const std::map<std::string, Spec> specs = {
        {"map_power_switch", {"6.37", {"-lib_cells", "-port_map"}, {}, 1, 1}},
        {"map_repeater_cell", {"6.38", {"-domain", "-elements", "-exclude_elements", "-lib_cells"}, {}, 1, 1}},
        {"map_retention_cell",
         {"6.39",
          {"-domain", "-elements", "-exclude_elements", "-lib_cells", "-lib_cell_type", "-lib_model_name", "-port_map"},
          {}, 1, 1}},
        {"map_retention_clamp_cell", {"6.40", {"-domain", "-clock_clamp_lib_cells", "-async_clamp_lib_cells"}, {}, 1, 1}},
        {"name_format",
         {"6.41",
          {"-isolation_prefix", "-isolation_suffix", "-level_shift_prefix", "-level_shift_suffix",
           "-implicit_supply_suffix", "-implicit_logic_prefix", "-implicit_logic_suffix"},
          {}, 0, 0}},
        {"set_correlated", {"6.43", {"-nets", "-sets"}, {}, 0, 0}},
        {"set_variation", {"6.57", {"-supply", "-range"}, {}, 0, 0}},
        {"use_interface_cell",
         {"6.62",
          {"-strategy", "-domain", "-lib_cells", "-port_map", "-elements", "-exclude_elements", "-applies_to_clamp",
           "-inverter_supply_set"},
          {"-force_function"}, 1, 1}},
        {"define_always_on_cell",
         {"7.2", {"-cells", "-power", "-ground", "-power_switchable", "-ground_switchable", "-isolated_pins", "-enable"},
          {}, 0, 0}},
        {"define_diode_clamp", {"7.3", {"-cells", "-data_pins", "-type", "-power", "-ground"}, {}, 0, 0}},
        {"define_isolation_cell",
         {"7.4",
          {"-cells", "-power", "-ground", "-enable", "-clamp_cell", "-pin_groups", "-no_enable", "-always_on_pins",
           "-aux_enables", "-power_switchable", "-ground_switchable", "-valid_location"},
          {"-non_dedicated"}, 0, 0}},
        {"define_level_shifter_cell",
         {"7.5",
          {"-cells", "-input_voltage_range", "-output_voltage_range", "-ground_input_voltage_range",
           "-ground_output_voltage_range", "-direction", "-input_power_pin", "-output_power_pin", "-input_ground_pin",
           "-output_ground_pin", "-ground", "-power", "-enable", "-pin_groups", "-valid_location", "-bypass_enable",
           "-multi_stage"},
          {}, 0, 0}},
        {"define_power_switch_cell",
         {"7.6",
          {"-cells", "-type", "-stage_1_enable", "-stage_1_output", "-stage_2_enable", "-stage_2_output",
           "-power_switchable", "-power", "-ground_switchable", "-ground", "-always_on_pins", "-gate_bias_pin"},
          {}, 0, 0}},
        {"define_retention_cell",
         {"7.7",
          {"-cells", "-power", "-ground", "-cell_type", "-always_on_pins", "-restore_function", "-save_function",
           "-restore_check", "-save_check", "-retention_check", "-hold_check", "-always_on_components",
           "-power_switchable", "-ground_switchable"},
          {}, 0, 0}},
    };
    auto sp = specs.find(n);
    if (sp == specs.end()) return false;
    const Spec& s = sp->second;
    const std::string cl = std::string(" (") + s.clause + ")";
    auto fail = [&](const std::string& msg) -> void { throw Error(c.where() + ": " + n + ": " + msg + cl); };

    std::set<std::string> flags = s.flags;
    Opts o = opts(c, flags);
    for (auto& [k, v] : o.v) {
        (void)v;
        if (!s.values.count(k) && !s.flags.count(k)) fail("unknown option " + k);
    }
    if (o.pos.size() < s.minPos) fail(s.minPos == 1 ? "needs a name" : "needs an argument");
    if (o.pos.size() > s.maxPos) fail("unexpected argument " + o.pos[s.maxPos]);
    // Options other than -attribute-like ones are given once.
    for (auto& k : s.values)
        if (o.v.count(k) > 1) fail(k + " is given more than once");
    auto req = [&](const char* k) { if (!o.has(k)) fail(std::string("needs ") + k); };
    auto nonEmpty = [&](const char* k) {
        if (o.has(k) && splitList(o.get(k)).empty()) fail(std::string(k) + " is an empty list");
    };
    auto oneOf = [&](const char* k, std::initializer_list<const char*> l) {
        if (!o.has(k)) return;
        std::string v = o.get(k), all;
        for (auto x : l) {
            if (v == x) return;
            all += all.empty() ? x : std::string(", ") + x;
        }
        fail(std::string(k) + " " + v + " is not one of " + all);
    };
    // A list of {a b} pairs; each sublist has exactly `k` (or k..k2) items.
    auto groups = [&](const char* k, size_t lo, size_t hi) {
        std::vector<std::vector<std::string>> r;
        for (auto& g : o.list(k)) {
            auto l = splitList(g);
            if (l.size() < lo || l.size() > hi)
                fail(std::string(k) + " {" + g + "} has " + std::to_string(l.size()) + " items, not " +
                     std::to_string(lo) + (hi != lo ? " to " + std::to_string(hi) : ""));
            r.push_back(l);
        }
        return r;
    };
    auto domainOf = [&]() {
        req("-domain");
        std::string dn = join(scope, o.get("-domain"));
        if (!domains.count(dn)) fail("no power domain " + o.get("-domain"));
        return dn;
    };
    // Is element e (rooted) within one of the element lists l (rooted)? An
    // empty list is the whole domain.
    auto within = [](const std::string& e, const std::vector<std::string>& l) {
        if (l.empty()) return true;
        for (auto& x : l) {
            if (e == x) return true;
            std::string px = x == "." ? "" : x;
            if (px.empty() || e.compare(0, px.size() + 1, px + "/") == 0 || e.compare(0, px.size() + 1, px + ".") == 0)
                return true;
        }
        return false;
    };
    auto isSupply = [&](const std::string& full) {
        return nets.count(full) || ports.count(full) || !fnNet(full).empty();
    };
    auto pinGroups = [&]() {
        std::set<std::string> ins, outs;
        for (auto& g : groups("-pin_groups", 2, 3)) {
            if (!ins.insert(g[0]).second) fail("input pin " + g[0] + " is in more than one of -pin_groups");
            if (!outs.insert(g[1]).second) fail("output pin " + g[1] + " is in more than one of -pin_groups");
        }
    };
    auto number = [&](const std::string& t, const std::string& what) {
        char* e = nullptr;
        double d = std::strtod(t.c_str(), &e);
        if (t.empty() || *e) fail(what + " " + t + " is not a number");
        return d;
    };

    if (n == "map_power_switch") {
        auto names = splitList(o.pos[0]);
        if (names.empty()) fail("the switch list is empty");
        req("-lib_cells");
        nonEmpty("-lib_cells");
        for (auto& sw : names) {
            auto it = switches.find(join(scope, sw));
            if (it == switches.end()) fail("no power switch " + sw);
            const Switch& w = it->second;
            for (auto& m : groups("-port_map", 2, 2)) {
                bool port = m[1] == w.outPort;
                for (auto& [p, net] : w.ins) port |= m[1] == p;
                for (auto& [p, net] : w.ctrls) port |= m[1] == p;
                for (auto& a : w.acks) port |= m[1] == a.port;
                if (!port && !isSupply(join(scope, m[1])))
                    fail("-port_map " + m[1] + " is neither a port of switch " + sw + " nor a supply net");
            }
        }
    } else if (n == "map_repeater_cell") {
        std::string dn = domainOf();
        const Repeater* rp = nullptr;
        for (auto& r : reps) if (r.name == o.pos[0] && r.domain == dn) rp = &r;
        if (!rp) fail("no repeater strategy " + o.pos[0] + " in domain " + o.get("-domain"));
        nonEmpty("-elements");
        nonEmpty("-lib_cells");
    } else if (n == "map_retention_cell") {
        auto names = splitList(o.pos[0]);
        if (names.empty()) fail("the retention strategy list is empty");
        std::string dn = domainOf();
        std::vector<const Retention*> rs;
        for (auto& nm : names) {
            const Retention* rt = nullptr;
            for (auto& r : rets) if (r.name == nm && r.domain == dn) rt = &r;
            if (!rt) fail("no retention strategy " + nm + " in domain " + o.get("-domain"));
            rs.push_back(rt);
        }
        if (!o.has("-lib_cells") && !o.has("-lib_cell_type") && !o.has("-lib_model_name"))
            fail("needs -lib_cells, -lib_cell_type or -lib_model_name");
        if (o.has("-lib_model_name") && !o.has("-port_map")) fail("-lib_model_name needs -port_map");
        if (o.has("-port_map") && !o.has("-lib_model_name")) fail("-port_map needs -lib_model_name");
        groups("-port_map", 2, 2);
        for (auto& e : o.list("-elements")) {
            std::string full = join(scope, e);
            for (auto* r : rs)
                if (!within(full, r->elements))
                    fail("element " + e + " is not in the elements of retention strategy " + r->name);
        }
        if (o.has("-lib_model_name")) {
            RetModel rm;
            rm.strategies = names;
            rm.domain = dn;
            rm.scope = scope;
            rm.where = c.where();
            rm.model = o.get("-lib_model_name");
            for (auto& e : o.list("-elements")) rm.elements.push_back(join(scope, e));
            std::set<std::string> seen;
            for (auto& g : groups("-port_map", 2, 2)) {
                if (!seen.insert(g[0]).second) fail("-port_map maps model port " + g[0] + " twice");
                rm.portMap.push_back({g[0], g[1]});
            }
            for (auto* r : rs)
                for (auto& [port, ref] : rm.portMap) {
                    std::string x = ref[0] == '~' ? ref.substr(1) : ref;
                    if (x == "save_signal" && r->save.net.empty()) fail("strategy " + r->name + " has no save signal for " + port);
                    if (x == "restore_signal" && r->restore.net.empty()) fail("strategy " + r->name + " has no restore signal for " + port);
                    if (x.rfind("retention_supply.", 0) == 0 && r->supply.empty())
                        fail("strategy " + r->name + " has no -retention_supply for " + port);
                }
            retModels.push_back(rm);
        }
    } else if (n == "map_retention_clamp_cell") {
        auto names = splitList(o.pos[0]);
        if (names.empty()) fail("the retention strategy list is empty");
        std::string dn = domainOf();
        req("-clock_clamp_lib_cells");
        req("-async_clamp_lib_cells");
        nonEmpty("-clock_clamp_lib_cells");
        nonEmpty("-async_clamp_lib_cells");
        bool any = false;
        for (auto& nm : names)
            for (auto& r : rets) any |= r.name == nm && r.domain == dn;
        if (!any) fail("none of the strategies " + o.pos[0] + " is a retention strategy of domain " + o.get("-domain"));
    } else if (n == "name_format") {
        if (o.v.empty()) nameFormat.clear();   // no options: back to the defaults
        for (auto& [k, v] : o.v) nameFormat[k.substr(1)] = v;
    } else if (n == "set_correlated") {
        if (o.has("-nets") == o.has("-sets")) fail("needs -nets or -sets, not both");
        bool isNets = o.has("-nets");
        const char* k = isNets ? "-nets" : "-sets";
        auto gs = groups(k, 2, SIZE_MAX);
        if (gs.empty()) fail(std::string(k) + " is an empty list");
        for (auto& g : gs) {
            std::vector<std::string> full;
            for (auto& x : g) {
                std::string f = join(scope, x);
                if (isNets) {
                    if (!isSupply(f)) fail("no supply net, port or supply set function " + x);
                } else {
                    if (!sets.count(setOf(f))) setRef(f, c.where());   // throws for an unknown set
                    f = setOf(f);
                }
                full.push_back(f);
            }
            correlated.push_back({isNets, full});
        }
    } else if (n == "set_variation") {
        req("-supply");
        req("-range");
        auto r = splitList(o.get("-range"));
        if (r.size() != 2) fail("-range needs {low_factor high_factor}");
        double lo = number(r[0], "-range factor"), hi = number(r[1], "-range factor");
        if (lo <= 0 || hi <= 0) fail("-range factors are positive");
        if (lo > hi) fail("-range low factor " + r[0] + " is above the high factor " + r[1]);
        auto sup = o.list("-supply");
        if (sup.empty()) fail("-supply is an empty list");
        // electrical equivalence of supply nets: connections and set_equivalent -nets
        std::map<std::string, std::string> up;
        std::function<std::string(std::string)> find = [&](std::string x) {
            while (up.count(x) && up[x] != x) x = up[x];
            return x;
        };
        auto unite = [&](const std::string& a, const std::string& b) {
            std::string x = find(a), y = find(b);
            if (x != y) up[x] = y;
        };
        for (auto& [net, ps] : connects) for (auto& p : ps) unite(p, net);
        for (auto& e : eqNets)
            if (!e.functionOnly) for (auto& x : e.names) unite(x, e.names[0]);
        for (auto& x : sup) {
            std::string f = join(scope, x);
            std::vector<std::string> objs;
            if (isSupply(f)) objs.push_back(fnNet(f).empty() ? f : fnNet(f));
            else if (sets.count(setOf(f)))
                for (auto& [fn, net] : sets.at(setOf(f)).fn) objs.push_back(net);
            else fail("no supply port, net, set or set function " + x);
            for (auto& ob : objs) {
                for (auto& [other, v] : variations) {
                    if (find(other) != find(ob)) continue;
                    if (v.lo != lo || v.hi != hi)
                        fail("variation {" + r[0] + " " + r[1] + "} of " + x + " differs from {" +
                             std::to_string(v.lo) + " " + std::to_string(v.hi) + "} given for the equivalent supply " +
                             other + " at " + v.where);
                }
                variations[ob] = {lo, hi, c.where()};
            }
        }
    } else if (n == "use_interface_cell") {
        req("-strategy");
        auto st = o.list("-strategy");
        if (st.empty()) fail("-strategy is an empty list");
        std::string dn = domainOf();
        req("-lib_cells");
        nonEmpty("-lib_cells");
        int isoN = 0, lsN = 0;
        std::vector<const std::vector<std::string>*> elems;
        for (auto& nm : st) {
            const Isolation* is = nullptr;
            const LevelShifter* ls = nullptr;
            for (auto& x : isos) if (x.name == nm && x.domain == dn) is = &x;
            for (auto& x : lss) if (x.name == nm && x.domain == dn) ls = &x;
            if (!is && !ls) fail("no isolation or level-shifter strategy " + nm + " in domain " + o.get("-domain"));
            if (is) { isoN++; elems.push_back(&is->elements); }
            else { lsN++; elems.push_back(&ls->elements); }
        }
        if (isoN > 1) fail("more than one isolation strategy");
        if (lsN > 1) fail("more than one level-shifter strategy");
        groups("-port_map", 2, 2);
        for (auto& e : o.list("-elements")) {
            std::string full = join(scope, e);
            for (size_t k = 0; k < elems.size(); k++)
                if (!within(full, *elems[k])) fail("element " + e + " is not in strategy " + st[k]);
        }
        if (o.has("-applies_to_clamp") && o.get("-applies_to_clamp").empty()) fail("-applies_to_clamp needs a value");
        for (auto& x : o.list("-inverter_supply_set")) setRef(join(scope, x), c.where());
    } else if (n == "define_always_on_cell") {
        req("-cells");
        nonEmpty("-cells");
        req("-power");
        req("-ground");
        if (!o.has("-power_switchable") && !o.has("-ground_switchable"))
            fail("needs -power_switchable or -ground_switchable");
        if (o.has("-enable") && !o.has("-isolated_pins")) fail("-enable needs -isolated_pins");
        if (o.has("-enable") && splitList(o.get("-enable")).size() != splitList(o.get("-isolated_pins")).size())
            fail("-enable has " + std::to_string(splitList(o.get("-enable")).size()) + " expressions for " +
                 std::to_string(splitList(o.get("-isolated_pins")).size()) + " lists of -isolated_pins");
    } else if (n == "define_diode_clamp") {
        req("-cells");
        nonEmpty("-cells");
        req("-data_pins");
        nonEmpty("-data_pins");
        oneOf("-type", {"power", "ground", "both"});
        if (!o.has("-power") && !o.has("-ground")) fail("needs -power or -ground");
        std::string t = o.get("-type", "both");
        if ((t == "power" || t == "both") && !o.has("-power")) fail("-type " + t + " needs -power");
        if ((t == "ground" || t == "both") && !o.has("-ground")) fail("-type " + t + " needs -ground");
    } else if (n == "define_isolation_cell") {
        req("-cells");
        nonEmpty("-cells");
        int forms = o.has("-enable") + o.has("-pin_groups") + o.has("-no_enable");
        if (forms != 1) fail("needs exactly one of -enable, -pin_groups and -no_enable");
        oneOf("-clamp_cell", {"high", "low"});
        oneOf("-no_enable", {"high", "low", "hold"});
        oneOf("-valid_location", {"source", "sink", "on", "off", "any"});
        if (o.has("-clamp_cell") && !o.has("-enable")) fail("-clamp_cell needs -enable");
        std::string cc = o.get("-clamp_cell");
        if (cc == "high" && o.has("-ground")) fail("a -clamp_cell high cell has only a -power pin");
        if (cc == "low" && o.has("-power")) fail("a -clamp_cell low cell has only a -ground pin");
        if (cc.empty() && (!o.has("-power") || !o.has("-ground"))) fail("needs -power and -ground");
        pinGroups();
    } else if (n == "define_level_shifter_cell") {
        req("-cells");
        nonEmpty("-cells");
        if (!o.has("-input_voltage_range") && !o.has("-ground_input_voltage_range"))
            fail("needs -input_voltage_range or -ground_input_voltage_range");
        auto ranges = [&](const char* in, const char* out) {
            if (o.has(in) != o.has(out)) fail(std::string(in) + " and " + out + " go together");
            auto a = groups(in, 2, 2), b = groups(out, 2, 2);
            if (a.size() != b.size())
                fail(std::string(in) + " has " + std::to_string(a.size()) + " ranges, " + out + " has " +
                     std::to_string(b.size()));
            for (auto* l : {&a, &b})
                for (auto& g : *l)
                    if (number(g[0], "voltage") > number(g[1], "voltage"))
                        fail("voltage range {" + g[0] + " " + g[1] + "} has its lower bound above the upper bound");
        };
        ranges("-input_voltage_range", "-output_voltage_range");
        ranges("-ground_input_voltage_range", "-ground_output_voltage_range");
        oneOf("-direction", {"low_to_high", "high_to_low", "both"});
        oneOf("-valid_location", {"source", "sink", "either", "any"});
        if (o.has("-ground") && (o.has("-input_ground_pin") || o.has("-output_ground_pin")))
            fail("-ground is for power-shifting cells and does not go with -input_ground_pin or -output_ground_pin");
        if (o.has("-power") && (o.has("-input_power_pin") || o.has("-output_power_pin")))
            fail("-power is for ground-shifting cells and does not go with -input_power_pin or -output_power_pin");
        if (o.has("-enable") && o.has("-pin_groups")) fail("-enable and -pin_groups do not go together");
        pinGroups();
        if (o.has("-multi_stage")) {
            double k = number(o.get("-multi_stage"), "-multi_stage");
            if (k < 1 || k != std::floor(k)) fail("-multi_stage " + o.get("-multi_stage") + " is not a positive integer");
        }
    } else if (n == "define_power_switch_cell") {
        req("-cells");
        nonEmpty("-cells");
        req("-type");
        oneOf("-type", {"header", "footer"});
        req("-stage_1_enable");
        if (o.has("-stage_2_output") && !o.has("-stage_2_enable")) fail("-stage_2_output needs -stage_2_enable");
        bool pw = o.has("-power_switchable"), gd = o.has("-ground_switchable");
        if (pw && gd) fail("-power_switchable and -ground_switchable do not go together");
        if (pw && !o.has("-power")) fail("-power_switchable needs -power");
        if (gd && !o.has("-ground")) fail("-ground_switchable needs -ground");
        if (pw && o.get("-type") == "footer") fail("a footer cell switches ground, not -power_switchable");
        if (gd && o.get("-type") == "header") fail("a header cell switches power, not -ground_switchable");
        expr(o.get("-stage_1_enable"), c);
        if (o.has("-stage_2_enable")) expr(o.get("-stage_2_enable"), c);
    } else if (n == "define_retention_cell") {
        req("-cells");
        nonEmpty("-cells");
        req("-power");
        req("-ground");
        auto fn = [&](const char* k) {
            std::pair<std::string, std::string> r;
            if (!o.has(k)) return r;
            auto l = splitList(o.get(k));
            if (l.size() == 1) l = splitList(l[0]);
            if (l.size() != 2 || senseOf(l[1]) < 0) fail(std::string(k) + " needs {pin high|low|posedge|negedge}");
            return std::make_pair(l[0], l[1]);
        };
        auto sv = fn("-save_function"), rs = fn("-restore_function");
        if (!sv.first.empty() && sv == rs)
            fail("-save_function and -restore_function are both {" + sv.first + " " + sv.second + "}");
        // -hold_check pins keep one value from save to restore: a check that
        // needs the pin high and one that needs it low conflict.
        auto hold = o.list("-hold_check");
        std::map<std::string, std::pair<int, std::string>> need;   // pin -> value, the option needing it
        for (const char* k : {"-save_check", "-restore_check", "-retention_check"}) {
            if (!o.has(k)) continue;
            ExP e = expr(o.get(k), c);
            // the literals of a conjunction: pin, !pin, pin == 0/1
            std::function<void(const Ex*)> lits = [&](const Ex* x) {
                if (!x) return;
                if (x->k == Ex::And || x->k == Ex::BAnd) {
                    for (auto& a : x->kids) lits(a.get());
                    return;
                }
                std::string pin;
                int v = -1;
                if (x->k == Ex::Id) pin = x->s, v = 1;
                else if (x->k == Ex::Not && x->kids.size() == 1 && x->kids[0]->k == Ex::Id)
                    pin = x->kids[0]->s, v = 0;
                if (pin.empty() || std::find(hold.begin(), hold.end(), pin) == hold.end()) return;
                auto [it, fresh] = need.insert({pin, {v, k}});
                if (!fresh && it->second.first != v)
                    fail(std::string(k) + " needs " + pin + " " + (v ? "high" : "low") + ", " + it->second.second +
                         " needs it " + (v ? "low" : "high") + ", and -hold_check holds " + pin);
            };
            lits(e.get());
        }
    }
    ImplCmd ic;
    ic.cmd = n;
    ic.scope = scope;
    ic.where = c.where();
    ic.pos = o.pos;
    ic.opts = o.v;
    implCmds.push_back(std::move(ic));
    return true;
}
