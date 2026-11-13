#pragma once
// upf_query.hpp - the UPF information model (IEEE 1801-2024 clause 10) and
// its Tcl query commands (11.1).
//
// The model is built from the power intent read so far (upf::Model) and
// the design (upf::DesignView) at the first query command, which starts
// phase 5 of UPF processing (11.1.1). Every object has a UPF handle (10.3.2):
// a hierarchical path from the UPF root scope ("/u_dut/PD_CORE",
// "/u_dut/PD_CORE.iso", "/u_dut/VDD@upfSupplyNetT") or a tool-generated ID
// ("#UPFEXTENT3#"). Objects carry the properties of their class (10.6) as
// Tcl strings: a basic value, an enumerated ID, a handle or a list of handles.
//
//   upf_query_object_properties <handle> [-property <name>]   11.1.2.1
//   upf_query_object_type <handle>                             11.1.2.2
//   upf_object_in_class <handle> -class <class ID>             11.1.2.3
//   upf_query_object_pathname <handle> [-relative_to <handle>] 11.1.2.4
//   query_upf <domain | scope> -pattern <p> [...]              11.1.3.2
//
// Dynamic properties (10.2.3.2, upf_current_state ...) belong to the HDL
// package interface (11.2) and are not served here.
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "upf.hpp"

namespace upf {

// ------------------------------------------------------------ classes (10.6)
struct ImClass {
    std::vector<std::string> membership;   // the class itself first, then its base classes
    std::vector<std::string> props;        // static properties
    std::vector<std::string> dynamic;      // dynamic properties (11.2 only)
};

inline const std::map<std::string, ImClass>& imClasses() {
    static const std::map<std::string, ImClass> all = [] {
        std::map<std::string, ImClass> m;
        const std::vector<std::string> named = {"upf_name", "upf_parent", "upf_file", "upf_line", "upf_creation_scope"};
        auto cat = [](std::vector<std::string> a, const std::vector<std::string>& b) {
            a.insert(a.end(), b.begin(), b.end());
            return a;
        };
        const std::vector<std::string> upfBase = {"upfBaseUpfT", "upfBaseNamedT", "upfBaseT"};
        const std::vector<std::string> boundary = {"upf_effective_extents", "upf_supply_set_handles", "upf_logic_refs",
                                                   "upf_location", "upf_applies_to", "upf_source_filter", "upf_sink_filter",
                                                   "upf_name_prefix", "upf_name_suffix", "upf_is_use_equivalence"};
        auto add = [&](const std::string& c, std::vector<std::string> bases, std::vector<std::string> props,
                       std::vector<std::string> dyn = {}) {
            bases.insert(bases.begin(), c);
            m[c] = ImClass{bases, props, dyn};
        };
        // 10.6.1 UPF objects
        add("upfPowerDomainT", cat({"upfExtentClassT"}, upfBase),
            cat(named, {"upf_effective_extents", "upf_supply_set_handles", "upf_upper_boundary", "upf_lower_boundary",
                        "upf_level_shifter_strategies", "upf_retention_strategies", "upf_isolation_strategies",
                        "upf_repeater_strategies", "upf_pd_states", "upf_pd_state_transitions", "upf_available_supplies",
                        "upf_boundary_supplies"}),
            {"upf_current_state", "upf_current_simstate"});
        add("upfRetentionStrategyT", cat({"upfStrategyT", "upfExtentClassT"}, upfBase),
            cat(named, {"upf_effective_extents", "upf_supply_set_handles", "upf_logic_refs", "upf_is_no_retention",
                        "upf_save_condition", "upf_restore_condition", "upf_retention_condition",
                        "upf_is_use_retention_as_primary", "upf_save_signal", "upf_restore_signal",
                        "upf_retention_parameters", "upf_applies_to", "upf_extent_groups", "upf_save_event_condition",
                        "upf_restore_event_condition", "upf_powerdown_period_condition", "upf_restore_period_condition"}));
        add("upfIsolationStrategyT", cat({"upfBoundaryStrategyT", "upfStrategyT", "upfExtentClassT"}, upfBase),
            cat(cat(named, boundary), {"upf_is_diff_supply_only", "upf_is_no_isolation", "upf_is_force_isolation",
                                       "upf_clamp_values", "upf_user_clamp_values", "upf_isolation_controls",
                                       "upf_applies_to_boundary", "upf_async_set_reset_ctrl", "upf_async_clamp_value"}));
        add("upfLevelShifterStrategyT", cat({"upfBoundaryStrategyT", "upfStrategyT", "upfExtentClassT"}, upfBase),
            cat(cat(named, boundary), {"upf_is_no_shift", "upf_is_force_shift", "upf_threshold_value",
                                       "upf_level_shift_rule", "upf_applies_to_boundary"}));
        add("upfRepeaterStrategyT", cat({"upfBoundaryStrategyT", "upfStrategyT", "upfExtentClassT"}, upfBase),
            cat(cat(named, boundary), {"upf_applies_to_boundary"}));
        add("upfSupplySetT", upfBase,
            cat(named, {"upf_functions", "upf_ss_states", "upf_ss_transitions", "upf_equivalent_sets", "upf_is_virtual"}),
            {"upf_current_state", "upf_current_simstate"});
        add("upfNamedRefT", upfBase, cat(named, {"upf_ref_kind", "upf_ref_object"}));
        add("upfSupplyNetT", cat({"upfNetClassT", "upfNetworkClassT"}, upfBase),
            cat(named, {"upf_hdl_implementation", "upf_root_driver", "upf_fanin_conn", "upf_fanout_conn",
                        "upf_resolve_type", "upf_user_resolution_func", "upf_sn_states", "upf_correlated_supplies",
                        "upf_is_virtual", "upf_is_tunneled"}),
            {"upf_current_value"});
        add("upfSupplyPortT", cat({"upfPortClassT", "upfNetworkClassT"}, upfBase),
            cat(named, {"upf_hdl_implementation", "upf_root_driver", "upf_network_attributes", "upf_hiconn",
                        "upf_loconn", "upf_port_dir", "upf_sp_states", "upf_correlated_supplies", "upf_is_virtual",
                        "upf_is_tunneled"}),
            {"upf_current_value"});
        add("upfLogicNetT", cat({"upfNetClassT", "upfNetworkClassT"}, upfBase),
            cat(named, {"upf_hdl_implementation", "upf_root_driver", "upf_fanin_conn", "upf_fanout_conn"}),
            {"upf_current_value"});
        add("upfLogicPortT", cat({"upfPortClassT", "upfNetworkClassT"}, upfBase),
            cat(named, {"upf_hdl_implementation", "upf_root_driver", "upf_network_attributes", "upf_hiconn",
                        "upf_loconn", "upf_port_dir"}),
            {"upf_current_value"});
        add("upfAbstractPowerSourceT", cat({"upfExtentClassT"}, upfBase),
            cat(named, {"upf_supply_set_handles", "upf_output_supply_set", "upf_input_supply_sets", "upf_control_ports",
                        "upf_ack_ports", "upf_power_source_states", "upf_power_switches"}));
        add("upfPowerSwitchT", cat({"upfExtentClassT"}, upfBase),
            cat(named, {"upf_effective_extents", "upf_supply_set_handles", "upf_output_supply_port",
                        "upf_input_supply_ports", "upf_control_ports", "upf_ack_ports", "upf_sw_states",
                        "upf_switch_type"}));
        add("upfAckPortT", cat({"upfPortClassT", "upfNetworkClassT"}, upfBase),
            cat(named, {"upf_hdl_implementation", "upf_root_driver", "upf_hiconn", "upf_port_dir", "upf_ack_delay",
                        "upf_ack_expr"}),
            {"upf_current_value"});
        add("upfPowerStateT", cat({"upfStateClassT"}, upfBase),
            cat(named, {"upf_is_illegal", "upf_logic_expr", "upf_supply_expr", "upf_simstate", "upf_power_expr"}),
            {"upf_is_active"});
        add("upfPowerSourceStateT", cat({"upfStateClassT"}, upfBase), cat(named, {"upf_power_source_expr"}));
        add("upfPowerSwitchStateT", cat({"upfStateClassT"}, upfBase),
            cat(named, {"upf_is_illegal", "upf_switch_expr", "upf_switch_output_state", "upf_input_supply_port"}));
        add("upfSupplyPortStateT", cat({"upfStateClassT"}, upfBase),
            cat(named, {"upf_supply_state", "upf_volt_min", "upf_volt_nom", "upf_volt_max", "upf_volt_kind"}));
        add("upfPstStateT", cat({"upfStateClassT"}, upfBase), cat(named, {"upf_is_illegal", "upf_supply_states"}),
            {"upf_is_active"});
        add("upfPowerStateTableT", upfBase, cat(named, {"upf_pst_states", "upf_pst_header"}), {"upf_current_state"});
        add("upfPowerStateTransitionT", cat({"upfStateClassT"}, upfBase),
            cat(named, {"upf_is_illegal", "upf_to_states", "upf_from_states"}));
        add("upfCompositeDomainT", upfBase,
            cat(named, {"upf_supply_set_handles", "upf_pd_states", "upf_pd_state_transitions", "upf_subdomains"}),
            {"upf_current_state"});
        add("upfPowerStateGroupT", upfBase, cat(named, {"upf_group_states", "upf_group_state_transitions"}),
            {"upf_current_state"});
        add("upfPowerModelT", cat({"upfExtentClassT"}, upfBase),
            {"upf_name", "upf_file", "upf_line", "upf_effective_extents", "upf_for_models"});
        // 10.6.2 HDL objects
        const std::vector<std::string> hdlBase = {"upfBaseHdlT", "upfBaseNamedT", "upfBaseT"};
        const std::vector<std::string> hdl = {"upf_name", "upf_parent", "upf_cell_info", "upf_hdl_attributes", "upf_extents"};
        add("upfHdlScopeT", hdlBase,
            cat(hdl, {"upf_hdl_ports", "upf_hdl_items", "upf_items", "upf_child_instances", "upf_power_domains"}));
        add("upfHdlPortBitT", cat({"upfHdlPortClassT", "upfHdlDeclT"}, hdlBase),
            cat(hdl, {"upf_port_dir", "upf_normalized_idx", "upf_smallest_atomic_slice"}));
        add("upfHdlPortMultiBitT", cat({"upfHdlPortClassT", "upfHdlDeclT"}, hdlBase),
            cat(hdl, {"upf_port_dir", "upf_hdl_width", "upf_normalized_bits"}));
        add("upfHdlNetBitT", cat({"upfHdlNetClassT", "upfHdlDeclT"}, hdlBase),
            cat(hdl, {"upf_normalized_idx", "upf_smallest_atomic_slice"}));
        add("upfHdlNetMultiBitT", cat({"upfHdlNetClassT", "upfHdlDeclT"}, hdlBase),
            cat(hdl, {"upf_hdl_width", "upf_normalized_bits"}));
        add("upfHdlMultiBitSliceT", {"upfBaseNamedT", "upfBaseT"},
            {"upf_name", "upf_parent", "upf_msb", "upf_lsb", "upf_slice_bits"});
        // 10.6.3 relationship objects
        const std::vector<std::string> rel = {"upfBaseRelationshipT", "upfBaseT"};
        add("upfExtentT", rel, {"upf_hdl_element", "upf_cells", "upf_object", "upf_next_extent"});
        add("upfSignalSenseT", rel, {"upf_signal_sensitivity", "upf_control_signal"});
        add("upfCellT", rel, {"upf_model_name", "upf_cell_kind", "upf_hdl_cell_kind", "upf_cell_origin", "upf_source_extents"});
        add("upfExpressionT", rel, {"upf_expr_string", "upf_expr_operands"}, {"upf_current_value"});
        add("upfRetentionExtentGroupT", rel,
            {"upf_ret_strategy", "upf_group_clock", "upf_group_async_set_reset", "upf_effective_extents",
             "upf_save_event_condition", "upf_restore_event_condition", "upf_powerdown_period_condition",
             "upf_restore_period_condition"});
        add("upfAttributeT", rel, {"upf_file", "upf_line", "upf_attr_name", "upf_attr_value"});
        add("upfDesignT", rel, {"upf_power_models", "upf_partial_on_translation", "upf_power_domains", "upf_hdl_attributes"});
        return m;
    }();
    return all;
}

// Every class ID: the concrete classes and the base classes (Table 17).
inline const std::set<std::string>& imClassIds() {
    static const std::set<std::string> ids = [] {
        std::set<std::string> s;
        for (auto& [c, k] : imClasses())
            for (auto& b : k.membership) s.insert(b);
        return s;
    }();
    return ids;
}
inline bool imPropertyName(const std::string& p) {
    static const std::set<std::string> names = [] {
        std::set<std::string> s;
        for (auto& [c, k] : imClasses()) {
            s.insert(k.props.begin(), k.props.end());
            s.insert(k.dynamic.begin(), k.dynamic.end());
        }
        return s;
    }();
    return names.count(p) != 0;
}

// --------------------------------------------------------------- the model
struct ImObj {
    std::string cls;
    std::vector<std::pair<std::string, std::string>> props;   // in the class's order, only those that are set
    std::string path;   // upf_query_object_pathname; empty for relationship objects
    void set(const std::string& k, const std::string& v) {
        for (auto& [pk, pv] : props)
            if (pk == k) { pv = v; return; }
        props.push_back({k, v});
    }
    void add(const std::string& k, const std::string& h) {   // append a handle to a list property
        for (auto& [pk, pv] : props)
            if (pk == k) { pv += (pv.empty() ? "" : " ") + tcl::listElem(h); return; }
        props.push_back({k, tcl::listElem(h)});
    }
    const std::string* get(const std::string& k) const {
        for (auto& [pk, pv] : props)
            if (pk == k) return &pv;
        return nullptr;
    }
};

struct InfoModel {
    std::map<std::string, ImObj> objs;
    std::map<std::string, int> counters;
    std::string root;   // handle of the UPF root scope's HDL scope

    std::string gen(const std::string& kind) { return "#UPF" + kind + std::to_string(++counters[kind]) + "#"; }
    ImObj& make(const std::string& h, const std::string& cls, const std::string& path) {
        ImObj& o = objs[h];
        o.cls = cls;
        o.path = path;
        return o;
    }
    const ImObj* find(const std::string& h) const {
        auto it = objs.find(h);
        return it == objs.end() ? nullptr : &it->second;
    }
};

struct ImBuilder {
    const Model& m;
    const DesignView* dv;
    InfoModel& im;
    std::string topP;   // the power top, '/'-separated

    static std::string slash(std::string p) {
        for (char& c : p) if (c == '.') c = '/';
        return p;
    }
    // A simulator path ("u_dut.u_core.q") as a handle; "" is the root scope.
    static std::string hdlH(const std::string& p) { return "/" + slash(p); }
    // A UPF name, rooted at the power top ("u_core/PD_C"), as a handle.
    std::string upfH(const std::string& n) const {
        if (n.empty() || n == ".") return "/" + topP;
        return topP.empty() ? "/" + n : "/" + topP + "/" + n;
    }
    static std::pair<std::string, std::string> splitName(const std::string& n) {
        size_t s = n.rfind('/');
        if (s == std::string::npos) return {"", n};
        return {n.substr(0, s), n.substr(s + 1)};
    }
    static void where(ImObj& o, const std::string& w) {
        size_t c = w.rfind(':');
        if (c == std::string::npos) return;
        o.set("upf_file", w.substr(0, c));
        o.set("upf_line", w.substr(c + 1));
    }
    static const char* boolS(bool b) { return b ? "true" : "false"; }

    // ------------------------------------------------------------- HDL
    std::string scope(const std::string& simPath) {   // the HDL scope object of an instance
        std::string h = hdlH(simPath);
        if (im.objs.count(h)) return h;
        ImObj& o = im.make(h, "upfHdlScopeT", h);
        size_t d = simPath.rfind('.');
        std::string name = d == std::string::npos ? simPath : simPath.substr(d + 1);
        if (simPath.empty()) name = dv && dv->model.count("") ? dv->model.at("") : "";
        o.set("upf_name", name);
        if (!simPath.empty()) {
            std::string ph = scope(d == std::string::npos ? "" : simPath.substr(0, d));
            im.objs[h].set("upf_parent", ph);
            im.objs[ph].add("upf_child_instances", h);
        }
        return h;
    }
    std::string upfScope(const std::string& upfScopeName) { return scope(m.design(upfScopeName)); }
    // An HDL port or net by simulator name; "" when the design has no such object.
    std::string decl(const std::string& simName, bool mustExist = true) {
        std::string h = hdlH(simName);
        if (im.objs.count(h)) return h;
        if (!dv) {
            if (mustExist) return "";
        } else if (!dv->signals.count(simName)) {
            return "";
        }
        size_t d = simName.rfind('.');
        std::string inst = d == std::string::npos ? "" : simName.substr(0, d), leaf = simName.substr(d == std::string::npos ? 0 : d + 1);
        int dir = -1;
        if (dv)
            if (auto it = dv->portsOf.find(inst); it != dv->portsOf.end())
                for (auto& [pn, pd] : it->second) if (pn == leaf) dir = pd;
        int w = 1;
        if (dv) if (auto it = dv->width.find(simName); it != dv->width.end()) w = it->second;
        std::string cls = dir >= 0 ? (w > 1 ? "upfHdlPortMultiBitT" : "upfHdlPortBitT") : (w > 1 ? "upfHdlNetMultiBitT" : "upfHdlNetBitT");
        std::string ph = scope(inst);
        ImObj& o = im.make(h, cls, h);
        o.set("upf_name", leaf);
        o.set("upf_parent", ph);
        if (dir >= 0) o.set("upf_port_dir", dir == 0 ? "UPF_DIR_IN" : dir == 1 ? "UPF_DIR_OUT" : "UPF_DIR_INOUT");
        if (w > 1) o.set("upf_hdl_width", std::to_string(w));
        else o.set("upf_normalized_idx", "0");
        im.objs[ph].add(dir >= 0 ? "upf_hdl_ports" : "upf_hdl_items", h);
        return h;
    }
    // An element of a UPF element list (an instance or a port), as an HDL handle.
    std::string element(const std::string& upfName) {
        std::string p = m.design(upfName);
        if (!dv || dv->instances.count(p) || p == m.top) return scope(p);
        std::string d = decl(p);
        return d.empty() ? scope(p) : d;
    }
    void buildDesign() {
        im.root = scope("");
        if (!dv) return;
        for (auto& p : dv->instances) scope(p);
        for (auto& [inst, ps] : dv->portsOf)
            for (auto& [pn, pd] : ps) decl(inst.empty() ? pn : inst + "." + pn);
        for (auto& sg : dv->signals) decl(sg);
    }

    // ------------------------------------------------------------ UPF
    std::string extents(const std::string& objH, const std::vector<std::string>& els) {
        std::string head, prev;
        for (auto& e : els) {
            std::string x = im.gen("EXTENT");
            ImObj& o = im.make(x, "upfExtentT", "");
            std::string eh = element(e);
            im.objs[x].set("upf_hdl_element", eh);
            im.objs[x].set("upf_object", objH);
            im.objs[eh].add("upf_extents", x);
            (void)o;
            if (prev.empty()) head = x;
            else im.objs[prev].set("upf_next_extent", x);
            prev = x;
        }
        return head;
    }
    void named(ImObj& o, const std::string& name, const std::string& parent, const std::string& creation, const std::string& w) {
        o.set("upf_name", name);
        o.set("upf_parent", parent);
        where(o, w);
        o.set("upf_creation_scope", creation);
    }
    // An expression object; its operands are the objects it names that the model has.
    std::string expr(const ExP& e, const std::string& scopeName) {
        if (!e) return "";
        std::string x = im.gen("EXPR");
        im.make(x, "upfExpressionT", "");
        im.objs[x].set("upf_expr_string", e->text);
        std::vector<std::string> ids;
        exprIds(e, ids);
        std::set<std::string> seen;
        for (auto& id : ids) {
            std::string h = resolveName(Model::join(scopeName, id));
            if (h.empty()) h = resolveName(id);
            if (!h.empty() && seen.insert(h).second) im.objs[x].add("upf_expr_operands", h);
        }
        return x;
    }
    std::string sense(const std::string& net, int sense) {
        std::string x = im.gen("SIGSENSE");
        im.make(x, "upfSignalSenseT", "");
        static const char* k[] = {"UPF_SENSE_HIGH", "UPF_SENSE_LOW", "UPF_SENSE_POSEDGE", "UPF_SENSE_NEGEDGE"};
        im.objs[x].set("upf_signal_sensitivity", k[sense < 0 || sense > 3 ? 0 : sense]);
        std::string h = signal(net);
        if (!h.empty()) im.objs[x].set("upf_control_signal", h);
        return x;
    }
    // A logic signal named in UPF: its HDL object.
    std::string signal(const std::string& upfName) {
        std::string d = decl(m.design(upfName), dv != nullptr);
        return d;
    }
    // A supply net or port, by UPF name.
    std::string supplyH(const std::string& n) {
        if (m.ports.count(n) || isSwitchPort(n)) return upfH(n);
        if (m.nets.count(n)) return upfH(n) + (m.ports.count(n) ? "@upfSupplyNetT" : "");
        return "";
    }
    bool isSwitchPort(const std::string& n) const {
        auto [sc, leaf] = splitName(n);
        auto it = m.switches.find(sc);
        if (it == m.switches.end()) return false;
        if (it->second.outPort == leaf) return true;
        for (auto& [p, net] : it->second.ins) if (p == leaf) return true;
        return false;
    }
    std::string netH(const std::string& n) { return upfH(n) + (m.ports.count(n) ? "@upfSupplyNetT" : ""); }
    // Any UPF name an expression or option can use, as a handle; "" if none.
    std::string resolveName(const std::string& n) {
        if (m.domains.count(n)) return upfH(n);
        std::string f = m.fnNet(n);
        if (!f.empty()) {
            size_t d = n.rfind('.');
            std::string set = setHandle(n.substr(0, d));
            if (!set.empty()) return set + "." + n.substr(d + 1);
        }
        std::string s = setHandle(n);
        if (!s.empty()) return s;
        std::string sp = supplyH(n);
        if (!sp.empty()) return sp;
        return signal(n);
    }
    // A supply set or supply set handle by UPF name: "SS", "PD.primary".
    // Objects whose supply set handles are named <object>.<handle>.
    bool owner(const std::string& n) const { return m.domains.count(n) || m.composites.count(n) || m.sources.count(n); }
    std::string setHandle(const std::string& n) {
        size_t d = n.rfind('.');
        if (d != std::string::npos && owner(n.substr(0, d))) return upfH(n.substr(0, d)) + "." + n.substr(d + 1);
        if (m.sets.count(n)) return upfH(n);
        return "";
    }
    static const char* refKind(const std::string& fn) {
        if (fn == "power") return "UPF_REF_POWER";
        if (fn == "ground") return "UPF_REF_GROUND";
        if (fn == "pwell") return "UPF_REF_PWELL";
        if (fn == "nwell") return "UPF_REF_NWELL";
        if (fn == "deeppwell") return "UPF_REF_DEEPPWELL";
        if (fn == "deepnwell") return "UPF_REF_DEEPNWELL";
        return "UPF_REF_USER_DEFINED";
    }
    // A supply set object at handle h with the functions and states of set `set`.
    void supplySet(const std::string& h, const std::string& name, const std::string& parent, const std::string& creation,
                   const std::string& set) {
        if (im.objs.count(h)) return;
        im.make(h, "upfSupplySetT", h);
        auto it = m.sets.find(set);
        named(im.objs[h], name, parent, creation, it == m.sets.end() ? "" : it->second.where);
        if (it == m.sets.end()) return;
        for (auto& [fn, net] : it->second.fn) {
            std::string r = h + "." + fn;
            im.make(r, "upfNamedRefT", r);
            named(im.objs[r], fn, h, creation, it->second.where);
            im.objs[r].set("upf_ref_kind", refKind(fn));
            std::string sh = supplyH(net);
            if (!sh.empty()) im.objs[r].set("upf_ref_object", sh);
            im.objs[h].add("upf_functions", r);
        }
        for (auto& ps : it->second.states) im.objs[h].add("upf_ss_states", powerState(h, ps, creation, splitName(set).first, true));
    }
    std::string powerState(const std::string& owner, const PowerState& ps, const std::string& creation,
                           const std::string& scopeName, bool ofSet) {
        std::string h = owner + "." + ps.name;
        if (im.objs.count(h)) return h;
        im.make(h, "upfPowerStateT", h);
        named(im.objs[h], ps.name, owner, creation, ps.where);
        im.objs[h].set("upf_is_illegal", boolS(ps.illegal));
        std::string le = expr(ps.logicExpr, scopeName), se = expr(ps.supplyExpr, scopeName);
        if (!le.empty()) im.objs[h].set("upf_logic_expr", le);
        if (!se.empty()) im.objs[h].set("upf_supply_expr", se);
        if (ofSet && ps.simstate != SS_NONE) im.objs[h].set("upf_simstate", ps.simstate == SS_CORRUPT ? "CORRUPT" : simstateName(ps.simstate));
        return h;
    }
    void ownedBy(const std::string& scopeH, const std::string& h) { im.objs[scopeH].add("upf_items", h); }

    void buildDomains() {
        for (auto& dn : m.domainOrder) {
            const Domain& d = m.domains.at(dn);
            std::string h = upfH(dn), cs = upfScope(d.scope);
            im.make(h, "upfPowerDomainT", h);
            named(im.objs[h], splitName(dn).second, cs, cs, d.where);
            ownedBy(cs, h);
            im.objs[cs].add("upf_power_domains", h);
            std::vector<std::string> els;
            for (auto& e : d.elements)
                if (std::find(d.exclude.begin(), d.exclude.end(), e) == d.exclude.end()) els.push_back(e);
            std::string x = extents(h, els);
            if (!x.empty()) im.objs[h].set("upf_effective_extents", x);
            for (auto& e : els) im.objs[h].add("upf_upper_boundary", element(e));
            for (auto& [k, set] : d.handles) {
                std::string sh = h + "." + k;
                supplySet(sh, k, h, cs, m.setOf(set));
                im.objs[h].add("upf_supply_set_handles", sh);
                im.objs[h].add("upf_available_supplies", sh);
            }
            for (auto& ps : d.states) im.objs[h].add("upf_pd_states", powerState(h, ps, cs, d.scope, false));
        }
    }
    void buildSets() {
        for (auto& [n, s] : m.sets) {
            size_t d = n.rfind('.');
            if (d != std::string::npos && owner(n.substr(0, d))) continue;   // a handle of a domain or power source
            auto [sc, leaf] = splitName(n);
            std::string cs = upfScope(sc);
            supplySet(upfH(n), leaf, cs, cs, n);
            ownedBy(cs, upfH(n));
        }
    }
    std::string resolveE(const std::string& r) {
        if (r == "unresolved" || r.empty()) return "UNRESOLVED";
        if (r == "one_hot") return "ONE_HOT";
        if (r == "parallel") return "PARALLEL";
        if (r == "parallel_one_hot") return "PARALLEL_ONE_HOT";
        return "USER_RESOLUTION_FUNC";
    }
    void portStatesOf(const std::string& n, const std::string& h, const std::string& cs, const char* prop) {
        auto it = m.portStates.find(n);
        if (it == m.portStates.end()) return;
        for (auto& st : it->second) {
            std::string sh = h + "." + st.name;
            if (im.objs.count(sh)) continue;
            im.make(sh, "upfSupplyPortStateT", sh);
            named(im.objs[sh], st.name, h, cs, "");
            im.objs[sh].set("upf_supply_state", st.off ? "OFF" : "FULL_ON");
            if (!st.off) {
                if (st.vmin == st.vmax) {
                    im.objs[sh].set("upf_volt_nom", tcl::fmtNum(st.vmin));
                    im.objs[sh].set("upf_volt_kind", "NOM");
                } else {
                    im.objs[sh].set("upf_volt_min", tcl::fmtNum(st.vmin));
                    im.objs[sh].set("upf_volt_max", tcl::fmtNum(st.vmax));
                    im.objs[sh].set("upf_volt_kind", "DOUBLET");
                }
            }
            im.objs[h].add(prop, sh);
        }
    }
    void buildSupplies() {
        for (auto& n : m.ports) {
            auto [sc, leaf] = splitName(n);
            std::string h = upfH(n), cs = upfScope(sc);
            im.make(h, "upfSupplyPortT", h);
            auto w = m.supplyWhere.find("port:" + n);
            named(im.objs[h], leaf, cs, cs, w == m.supplyWhere.end() ? "" : w->second);
            auto d = m.portDir.find(n);
            std::string dir = d == m.portDir.end() ? "in" : d->second;
            im.objs[h].set("upf_port_dir", dir == "out" ? "UPF_DIR_OUT" : dir == "inout" ? "UPF_DIR_INOUT" : "UPF_DIR_IN");
            ownedBy(cs, h);
            portStatesOf(n, h, cs, "upf_sp_states");
        }
        for (auto& n : m.nets) {
            auto [sc, leaf] = splitName(n);
            std::string h = netH(n), cs = upfScope(sc);
            im.make(h, "upfSupplyNetT", upfH(n));
            auto w = m.supplyWhere.find("net:" + n);
            named(im.objs[h], leaf, cs, cs, w == m.supplyWhere.end() ? "" : w->second);
            auto r = m.resolve.find(n);
            std::string rs = r == m.resolve.end() ? "" : r->second;
            im.objs[h].set("upf_resolve_type", resolveE(rs));
            if (resolveE(rs) == "USER_RESOLUTION_FUNC") im.objs[h].set("upf_user_resolution_func", rs);
            ownedBy(cs, h);
            portStatesOf(n, h, cs, "upf_sn_states");
        }
        // connections: a port in the net's own scope is its loconn, a port below is its hiconn;
        // what drives the net is its fanin, what reads it its fanout
        for (auto& [net, ps] : m.connects) {
            if (!m.nets.count(net)) continue;
            std::string nh = netH(net), nsc = splitName(net).first;
            for (auto& p : ps) {
                std::string ph = supplyH(p);
                if (ph.empty() || !im.objs.count(ph)) {
                    // an HDL port on a supply net (4.10)
                    std::string d = decl(m.design(p), dv != nullptr);
                    if (!d.empty()) im.objs[nh].add("upf_fanout_conn", d);
                    continue;
                }
                std::string psc = splitName(p).first;
                bool same = psc == nsc;
                im.objs[ph].add(same ? "upf_loconn" : "upf_hiconn", nh);
                auto d = m.portDir.find(p);
                bool in = d == m.portDir.end() || d->second == "in";
                bool drives = isSwitchPort(p) ? splitName(p).second == m.switches.at(psc).outPort : (same ? in : !in);
                im.objs[nh].add(drives ? "upf_fanin_conn" : "upf_fanout_conn", ph);
                if (drives && !im.objs[nh].get("upf_root_driver")) im.objs[nh].set("upf_root_driver", ph);
            }
        }
    }
    void buildSwitches() {
        for (auto& [n, sw] : m.switches) {
            auto [sc, leaf] = splitName(n);
            std::string h = upfH(n), cs = upfScope(sc);
            im.make(h, "upfPowerSwitchT", h);
            named(im.objs[h], leaf, cs, cs, sw.where);
            ownedBy(cs, h);
            auto port = [&](const std::string& p, const char* dir) {
                std::string ph = h + "/" + p;
                if (!im.objs.count(ph)) {
                    im.make(ph, "upfSupplyPortT", ph);
                    named(im.objs[ph], p, h, cs, sw.where);
                    im.objs[ph].set("upf_port_dir", dir);
                }
                return ph;
            };
            // a switch port's hiconn is the supply net it is connected to; the output drives that net
            auto hook = [&](const std::string& ph, const std::string& net, bool drives) {
                if (net.empty() || !m.nets.count(net)) return;
                std::string nh = netH(net);
                im.objs[ph].add("upf_hiconn", nh);
                im.objs[nh].add(drives ? "upf_fanin_conn" : "upf_fanout_conn", ph);
                if (drives && !im.objs[nh].get("upf_root_driver")) im.objs[nh].set("upf_root_driver", ph);
            };
            if (!sw.outPort.empty()) {
                std::string ph = port(sw.outPort, "UPF_DIR_OUT");
                im.objs[h].set("upf_output_supply_port", ph);
                hook(ph, sw.outNet, true);
            }
            for (auto& [p, net] : sw.ins) {
                std::string ph = port(p, "UPF_DIR_IN");
                im.objs[h].add("upf_input_supply_ports", ph);
                hook(ph, net, false);
            }
            for (auto& [p, net] : sw.ctrls) {
                std::string ph = h + "/" + p;
                im.make(ph, "upfLogicPortT", ph);
                named(im.objs[ph], p, h, cs, sw.where);
                im.objs[ph].set("upf_port_dir", "UPF_DIR_IN");
                std::string s = net.empty() ? "" : signal(net);
                if (!s.empty()) im.objs[ph].add("upf_hiconn", s);
                im.objs[h].add("upf_control_ports", ph);
            }
            for (auto& a : sw.acks) {
                std::string ph = h + "/" + a.port;
                im.make(ph, "upfAckPortT", ph);
                named(im.objs[ph], a.port, h, cs, sw.where);
                im.objs[ph].set("upf_port_dir", "UPF_DIR_OUT");
                std::string s = a.net.empty() ? "" : signal(a.net);
                if (!s.empty()) im.objs[ph].add("upf_hiconn", s);
                im.objs[ph].set("upf_ack_delay", std::to_string(a.delay));
                std::string x = expr(a.e, sc);
                if (!x.empty()) im.objs[ph].set("upf_ack_expr", x);
                im.objs[h].add("upf_ack_ports", ph);
            }
            auto states = [&](const std::vector<SwitchState>& v, const char* out) {
                for (auto& st : v) {
                    std::string sh = h + "." + st.name;
                    im.make(sh, "upfPowerSwitchStateT", sh);
                    named(im.objs[sh], st.name, h, cs, sw.where);
                    im.objs[sh].set("upf_is_illegal", "false");
                    std::string x = expr(st.e, sc);
                    if (!x.empty()) im.objs[sh].set("upf_switch_expr", x);
                    im.objs[sh].set("upf_switch_output_state", out);
                    if (!st.inPort.empty()) im.objs[sh].set("upf_input_supply_port", h + "/" + st.inPort);
                    im.objs[h].add("upf_sw_states", sh);
                }
            };
            states(sw.on, "FULL_ON");
            states(sw.partial, "PARTIAL_ON");
            states(sw.off, "OFF");
            states(sw.error, "UNDETERMINED");
        }
    }
    // The extent of a strategy: its elements, or by default its domain's.
    std::vector<std::string> strategyEls(const std::string& dom, const std::vector<std::string>& els,
                                         const std::vector<std::string>& exclude) {
        std::vector<std::string> r;
        const std::vector<std::string>& src = els.empty() && m.domains.count(dom) ? m.domains.at(dom).elements : els;
        for (auto& e : src)
            if (std::find(exclude.begin(), exclude.end(), e) == exclude.end()) r.push_back(e);
        return r;
    }
    std::string strategy(const std::string& cls, const std::string& name, const std::string& dom, const std::string& w,
                         const char* listProp, const std::vector<std::string>& els) {
        std::string dh = upfH(dom), h = dh + "." + name;
        std::string cs = upfScope(m.domains.count(dom) ? m.domains.at(dom).scope : splitName(dom).first);
        im.make(h, cls, h);
        named(im.objs[h], name, dh, cs, w);
        if (im.objs.count(dh)) im.objs[dh].add(listProp, h);
        std::string x = extents(h, els);
        if (!x.empty()) im.objs[h].set("upf_effective_extents", x);
        return h;
    }
    static const char* appliesE(int a) {
        return a == 0 ? "UPF_FILTER_INPUTS" : a == 1 ? "UPF_FILTER_OUTPUTS" : "UPF_FILTER_BOTH";
    }
    void localSet(const std::string& strategyH, const std::string& handle, const std::string& set, const std::string& cs) {
        if (set.empty()) return;
        std::string sh = strategyH + "." + handle;
        supplySet(sh, handle, strategyH, cs, m.setOf(set));
        im.objs[strategyH].add("upf_supply_set_handles", sh);
    }
    void logicRef(const std::string& strategyH, const std::string& name, const char* kind, const std::string& net,
                  const std::string& cs, const std::string& w) {
        std::string r = strategyH + "." + name;
        im.make(r, "upfNamedRefT", r);
        named(im.objs[r], name, strategyH, cs, w);
        im.objs[r].set("upf_ref_kind", kind);
        std::string s = signal(net);
        if (!s.empty()) im.objs[r].set("upf_ref_object", s);
        im.objs[strategyH].add("upf_logic_refs", r);
    }
    void buildStrategies() {
        for (auto& is : m.isos) {
            std::string h = strategy("upfIsolationStrategyT", is.name, is.domain, is.where, "upf_isolation_strategies",
                                     strategyEls(is.domain, is.elements, is.exclude));
            std::string cs = *im.objs[h].get("upf_creation_scope");
            localSet(h, "isolation_supply", is.supply, cs);
            for (size_t k = 0; k < is.signals.size(); k++) {
                logicRef(h, k ? "isolation_signal" + std::to_string(k) : "isolation_signal", "UPF_REF_ISO_SIGNAL",
                         is.signals[k], cs, is.where);
                bool low = k < is.senseLow.size() && is.senseLow[k];
                im.objs[h].add("upf_isolation_controls", sense(is.signals[k], low ? 1 : 0));
            }
            im.objs[h].set("upf_applies_to", appliesE(is.appliesTo));
            if (!is.source.empty()) { std::string s = resolveName(is.source); if (!s.empty()) im.objs[h].set("upf_source_filter", s); }
            if (!is.sink.empty()) { std::string s = resolveName(is.sink); if (!s.empty()) im.objs[h].set("upf_sink_filter", s); }
            im.objs[h].set("upf_is_diff_supply_only", boolS(is.diffSupplyOnly));
            im.objs[h].set("upf_is_no_isolation", boolS(is.noIso));
            std::string cv = is.clamp == "z" ? "Z" : is.clamp;
            im.objs[h].set("upf_clamp_values", tcl::listElem(cv));
        }
        for (auto& l : m.lss) {
            std::string h = strategy("upfLevelShifterStrategyT", l.name, l.domain, l.where, "upf_level_shifter_strategies",
                                     strategyEls(l.domain, l.elements, l.exclude));
            std::string cs = *im.objs[h].get("upf_creation_scope");
            localSet(h, "input_supply", l.inSupply, cs);
            localSet(h, "output_supply", l.outSupply, cs);
            im.objs[h].set("upf_applies_to", appliesE(l.appliesTo));
            im.objs[h].set("upf_is_no_shift", boolS(l.noShift));
            im.objs[h].set("upf_level_shift_rule",
                           l.rule == "low_to_high" ? "UPF_LS_LOW_TO_HIGH" : l.rule == "high_to_low" ? "UPF_LS_HIGH_TO_LOW" : "UPF_LS_BOTH");
        }
        for (auto& r : m.rets) {
            std::string h = strategy("upfRetentionStrategyT", r.name, r.domain, r.where, "upf_retention_strategies",
                                     strategyEls(r.domain, r.elements, r.exclude));
            std::string cs = *im.objs[h].get("upf_creation_scope");
            std::string sc = m.domains.count(r.domain) ? m.domains.at(r.domain).scope : "";
            localSet(h, "retention_supply", r.supply, cs);
            localSet(h, "primary_supply", m.primaryOf(r.domain), cs);
            if (!r.save.net.empty()) {
                logicRef(h, "save_signal", "UPF_REF_SAVE_SIGNAL", r.save.net, cs, r.where);
                im.objs[h].set("upf_save_signal", sense(r.save.net, r.save.sense));
            }
            if (!r.restore.net.empty()) {
                logicRef(h, "restore_signal", "UPF_REF_RESTORE_SIGNAL", r.restore.net, cs, r.where);
                im.objs[h].set("upf_restore_signal", sense(r.restore.net, r.restore.sense));
            }
            auto cond = [&](const char* prop, const ExP& e) {
                std::string x = expr(e, sc);
                if (!x.empty()) im.objs[h].set(prop, x);
            };
            cond("upf_save_event_condition", r.saveCond);
            cond("upf_restore_event_condition", r.restoreCond);
            cond("upf_restore_period_condition", r.restorePeriodCond);
            cond("upf_powerdown_period_condition", r.powerdownCond);
            im.objs[h].set("upf_is_no_retention", boolS(r.noRet));
            im.objs[h].set("upf_is_use_retention_as_primary", boolS(r.useAsPrimary));
            im.objs[h].set("upf_retention_parameters", std::string(r.retSupCor ? "RET_SUP_COR" : "NO_RET_SUP_COR") + " " +
                                                           (r.savResCor ? "SAV_RES_COR" : "NO_SAV_RES_COR"));
            im.objs[h].set("upf_applies_to", r.appliesTo == "flop"    ? "UPF_RET_FILTER_FLOP"
                                             : r.appliesTo == "latch" ? "UPF_RET_FILTER_LATCH"
                                                                      : "UPF_RET_FILTER_ANY");
        }
    }
    void buildPsts() {
        for (auto& t : m.psts) {
            auto [sc, leaf] = splitName(t.name);
            std::string h = upfH(t.name), cs = upfScope(sc);
            im.make(h, "upfPowerStateTableT", h);
            named(im.objs[h], leaf, cs, cs, t.where);
            ownedBy(cs, h);
            std::vector<std::string> cols;
            for (auto& s : t.supplies) {
                std::string sh = supplyH(s);
                cols.push_back(sh);
                im.objs[h].add("upf_pst_header", sh.empty() ? upfH(s) : sh);
            }
            for (auto& row : t.rows) {
                std::string rh = h + "." + row.name;
                im.make(rh, "upfPstStateT", rh);
                named(im.objs[rh], row.name, h, cs, row.where);
                im.objs[rh].set("upf_is_illegal", "false");
                for (size_t k = 0; k < row.st.size() && k < cols.size(); k++)
                    if (row.st[k] != "*" && !cols[k].empty()) im.objs[rh].add("upf_supply_states", cols[k] + "." + row.st[k]);
                im.objs[h].add("upf_pst_states", rh);
            }
        }
    }
    // add_state_transition (6.7): the transitions of an object at handle h.
    void transitionsOf(const std::string& obj, const std::string& h, const char* prop) {
        auto it = m.transitions.find(obj);
        if (it == m.transitions.end() || !im.objs.count(h)) return;
        const std::string cs = im.objs[h].get("upf_creation_scope") ? *im.objs[h].get("upf_creation_scope") : im.root;
        for (auto& t : it->second.tr) {
            std::string th = h + "." + t.name;
            if (im.objs.count(th)) continue;
            im.make(th, "upfPowerStateTransitionT", th);
            named(im.objs[th], t.name, h, cs, t.where);
            im.objs[th].set("upf_is_illegal", boolS(t.illegal));
            for (auto& f : t.from) im.objs[th].add("upf_from_states", h + "." + f);
            for (auto& to : t.to) im.objs[th].add("upf_to_states", h + "." + to);
            for (auto& [f, to] : t.paired) {
                im.objs[th].add("upf_from_states", h + "." + f);
                im.objs[th].add("upf_to_states", h + "." + to);
            }
            im.objs[h].add(prop, th);
        }
    }
    void buildTransitions() {
        for (auto& dn : m.domainOrder) transitionsOf(dn, upfH(dn), "upf_pd_state_transitions");
        for (auto& [obj, ts] : m.transitions) {
            if (ts.kind != 's') continue;
            std::string h = setHandle(obj);
            if (!h.empty()) transitionsOf(obj, h, "upf_ss_transitions");
        }
    }
    // create_composite_domain (6.17) and create_power_state_group (6.22): their
    // states are kept as a group's.
    void buildGroups() {
        for (auto& gn : m.groupOrder) {
            auto git = m.groups.find(gn);
            if (git == m.groups.end()) continue;
            const StateGroup& g = git->second;
            bool comp = m.composites.count(gn) != 0;
            auto [sc, leaf] = splitName(gn);
            std::string h = upfH(gn), cs = upfScope(g.scope);
            im.make(h, comp ? "upfCompositeDomainT" : "upfPowerStateGroupT", h);
            named(im.objs[h], leaf, cs, cs, g.where);
            ownedBy(cs, h);
            const char* st = comp ? "upf_pd_states" : "upf_group_states";
            for (auto& ps : g.states) im.objs[h].add(st, powerState(h, ps, cs, g.scope, false));
            if (comp) {
                for (auto& sd : m.composites.at(gn).subs) im.objs[h].add("upf_subdomains", upfH(sd));
                if (m.setAlias.count(gn + ".primary") || m.sets.count(gn + ".primary")) {
                    std::string sh = h + ".primary";
                    supplySet(sh, "primary", h, cs, m.setOf(gn + ".primary"));
                    im.objs[h].add("upf_supply_set_handles", sh);
                }
            }
            transitionsOf(gn, h, comp ? "upf_pd_state_transitions" : "upf_group_state_transitions");
        }
    }
    // create_abstract_power_source (6.16).
    void buildSources() {
        for (auto& [n, ps] : m.sources) {
            auto [sc, leaf] = splitName(n);
            std::string h = upfH(n), cs = upfScope(sc);
            im.make(h, "upfAbstractPowerSourceT", h);
            named(im.objs[h], leaf, cs, cs, ps.where);
            ownedBy(cs, h);
            auto handle = [&](const std::string& k) {
                std::string sh = h + "." + k;
                supplySet(sh, k, h, cs, m.setOf(n + "." + k));
                im.objs[h].add("upf_supply_set_handles", sh);
                return sh;
            };
            if (!ps.out.empty()) im.objs[h].set("upf_output_supply_set", handle(ps.out));
            for (auto& i : ps.ins) im.objs[h].add("upf_input_supply_sets", handle(i));
            for (auto& [p, net] : ps.ctrls) {
                std::string ph = h + "/" + p;
                im.make(ph, "upfLogicPortT", ph);
                named(im.objs[ph], p, h, cs, ps.where);
                im.objs[ph].set("upf_port_dir", "UPF_DIR_IN");
                std::string s = net.empty() ? "" : signal(net);
                if (!s.empty()) im.objs[ph].add("upf_hiconn", s);
                im.objs[h].add("upf_control_ports", ph);
            }
            for (auto& a : ps.acks) {
                std::string ph = h + "/" + a.port;
                im.make(ph, "upfAckPortT", ph);
                named(im.objs[ph], a.port, h, cs, ps.where);
                im.objs[ph].set("upf_port_dir", "UPF_DIR_OUT");
                std::string s = a.net.empty() ? "" : signal(a.net);
                if (!s.empty()) im.objs[ph].add("upf_hiconn", s);
                im.objs[ph].set("upf_ack_delay", std::to_string(a.delay));
                std::string x = expr(a.e, sc);
                if (!x.empty()) im.objs[ph].set("upf_ack_expr", x);
                im.objs[h].add("upf_ack_ports", ph);
            }
            auto states = [&](const std::vector<SwitchState>& v) {
                for (auto& st : v) {
                    std::string sh = h + "." + st.name;
                    im.make(sh, "upfPowerSourceStateT", sh);
                    named(im.objs[sh], st.name, h, cs, ps.where);
                    std::string x = expr(st.e, sc);
                    if (!x.empty()) im.objs[sh].set("upf_power_source_expr", x);
                    im.objs[h].add("upf_power_source_states", sh);
                }
            };
            states(ps.on);
            states(ps.off);
            for (auto& sw : ps.switches) im.objs[h].add("upf_power_switches", upfH(sw));
        }
    }
    // set_repeater (6.50).
    void buildRepeaters() {
        for (auto& r : m.reps) {
            std::string h = strategy("upfRepeaterStrategyT", r.name, r.domain, r.where, "upf_repeater_strategies",
                                     strategyEls(r.domain, r.elements, r.exclude));
            std::string cs = *im.objs[h].get("upf_creation_scope");
            localSet(h, "repeater_supply", r.supply, cs);
            im.objs[h].set("upf_applies_to", appliesE(r.appliesTo));
            if (!r.source.empty()) { std::string x = resolveName(r.source); if (!x.empty()) im.objs[h].set("upf_source_filter", x); }
            if (!r.sink.empty()) { std::string x = resolveName(r.sink); if (!x.empty()) im.objs[h].set("upf_sink_filter", x); }
            im.objs[h].set("upf_applies_to_boundary", r.boundary == 1 ? "UPF_BOUNDARY_LOWER" : r.boundary == 2 ? "UPF_BOUNDARY_UPPER" : "UPF_BOUNDARY_BOTH");
        }
    }
    // define_power_model / begin_power_model (6.31, 6.11): the instances it is
    // applied to are its extents.
    void buildPowerModels() {
        std::map<std::string, std::vector<std::string>> applied;
        for (auto& [inst, pm] : m.appliedModels) applied[pm].push_back(inst);
        for (auto& [n, pm] : m.powerModels) {
            std::string h = upfH(n);
            if (im.objs.count(h)) continue;
            im.make(h, "upfPowerModelT", h);
            im.objs[h].set("upf_name", splitName(n).second);
            where(im.objs[h], pm.where);
            std::string x = extents(h, applied[n]);
            if (!x.empty()) im.objs[h].set("upf_effective_extents", x);
            for (auto& f : pm.fors) im.objs[h].add("upf_for_models", f);
        }
    }
    // set_design_attributes (6.44) and set_port_attributes (6.51): attribute
    // objects (10.6.3) on the HDL objects they name.
    void attr(const std::string& onH, const std::string& name, const std::string& value, const std::string& w) {
        if (onH.empty() || !im.objs.count(onH)) return;
        std::string x = im.gen("ATTR");
        im.make(x, "upfAttributeT", "");
        where(im.objs[x], w);
        im.objs[x].set("upf_attr_name", name);
        im.objs[x].set("upf_attr_value", value);
        im.objs[onH].add("upf_hdl_attributes", x);
    }
    std::vector<std::string> instancesOf(const std::string& model) const {
        std::vector<std::string> r;
        if (!dv) return r;
        for (auto& [p, md] : dv->model)
            if (md == model && (m.top.empty() || p == m.top || p.rfind(m.top + ".", 0) == 0)) r.push_back(p);
        return r;
    }
    void buildAttributes() {
        for (auto& r : m.designAttrs) {
            std::vector<std::string> hs;
            if (r.hasElements) {
                for (auto& e : r.elements)
                    if (std::find(r.exclude.begin(), r.exclude.end(), e) == r.exclude.end()) hs.push_back(element(e));
            } else {
                for (auto& md : r.models) {
                    if (md == ".") hs.push_back(upfScope(r.scope));
                    else for (auto& p : instancesOf(md)) hs.push_back(scope(p));
                }
            }
            for (auto& h : hs)
                for (auto& [an, av] : r.attrs) attr(h, an, av, r.where);
        }
        for (auto& r : m.portAttrs) {
            std::vector<std::string> hs;
            auto portsOfInst = [&](const std::string& inst, const std::vector<std::string>& names) {
                std::vector<std::string> ps = names;
                if (ps.empty() && dv)
                    if (auto it = dv->portsOf.find(inst); it != dv->portsOf.end())
                        for (auto& [pn, pd] : it->second) ps.push_back(pn);
                for (auto& pn : ps) {
                    if (std::find(r.excludePorts.begin(), r.excludePorts.end(), pn) != r.excludePorts.end()) continue;
                    hs.push_back(decl(inst.empty() ? pn : inst + "." + pn, dv != nullptr));
                }
            };
            if (!r.model.empty()) {
                for (auto& p : instancesOf(r.model)) portsOfInst(p, r.ports);
            } else if (r.hasPorts) {
                for (auto& pn : r.ports) hs.push_back(decl(m.design(pn), dv != nullptr));
            } else {
                for (auto& e : r.elements)
                    if (std::find(r.exclude.begin(), r.exclude.end(), e) == r.exclude.end()) portsOfInst(m.design(e), {});
            }
            for (auto& h : hs)
                for (auto& [an, av] : r.attrs) attr(h, an, av, r.where);
        }
    }
    void build() {
        topP = slash(m.top);
        buildDesign();
        buildSupplies();
        buildSwitches();
        buildSets();
        buildSources();
        buildDomains();
        buildGroups();
        buildStrategies();
        buildRepeaters();
        buildPsts();
        buildTransitions();
        buildPowerModels();
        buildAttributes();
    }
};

// ---------------------------------------------------------------- queries
inline std::string Model::query(const Cmd& c, const std::string& scope) {
    const std::string& n = c.w[0];
    auto err = [&](const std::string& t) -> Error { return Error(c.where() + ": " + n + ": " + t); };
    if (!phase5) {
        phase5 = true;
        im = std::make_shared<InfoModel>();
        ImBuilder b{*this, dv, *im, ""};
        b.build();
    }
    ImBuilder hb{*this, dv, *im, ImBuilder::slash(top)};
    std::string cur = hb.upfH(scope);
    // A handle as written: absolute, relative to the current scope, "." or a tool ID (10.3.2.2.4).
    auto handle = [&](const std::string& h) -> std::string {
        if (h.empty()) throw err("an empty handle is not a valid UPF handle");
        if (h == ".") return cur;
        if (h[0] == '#') {
            if (h.size() < 3 || h.back() != '#') throw err("'" + h + "' is not a valid UPF handle");
            return h;
        }
        if (h[0] == '/') return h;
        return cur == "/" ? "/" + h : cur + "/" + h;
    };
    auto lookup = [&](const std::string& h) -> const ImObj& {
        const ImObj* o = im->find(h);
        if (!o) {
            // a supply port and net of the same name: the name alone is the port (10.3.2.2.3)
            throw err("no object '" + h + "' in the UPF information model");
        }
        return *o;
    };
    auto opts2 = [&](std::set<std::string> valued) {
        std::map<std::string, std::string> o;
        std::vector<std::string> pos;
        for (size_t i = 1; i < c.w.size(); i++) {
            const std::string& a = c.w[i];
            if (a.size() > 1 && a[0] == '-' && !std::isdigit((unsigned char)a[1])) {
                if (!valued.count(a)) throw err("unknown option " + a);
                if (i + 1 >= c.w.size()) throw err(a + " needs a value");
                o[a] = c.w[++i];
            } else {
                pos.push_back(a);
            }
        }
        if (pos.size() != 1) throw err("needs one object handle");
        return std::make_pair(o, pos[0]);
    };
    if (n == "upf_query_object_properties") {   // 11.1.2.1
        auto [o, h] = opts2({"-property"});
        const ImObj& ob = lookup(handle(h));
        const ImClass& k = imClasses().at(ob.cls);
        if (o.count("-property")) {
            const std::string& p = o["-property"];
            if (!imPropertyName(p)) throw err("'" + p + "' is not a property of the UPF information model");
            if (std::find(k.dynamic.begin(), k.dynamic.end(), p) != k.dynamic.end())
                throw err(p + " is a dynamic property: it is read through the UPF package in simulation (11.2)");
            if (std::find(k.props.begin(), k.props.end(), p) == k.props.end())
                throw err(p + " is not a property of " + ob.cls);
            const std::string* v = ob.get(p);
            return v ? *v : std::string();
        }
        std::vector<std::string> all;
        for (auto& p : k.props)
            if (const std::string* v = ob.get(p)) all.push_back(tcl::joinList({p, *v}));
        return tcl::joinList(all);
    }
    if (n == "upf_query_object_type") {   // 11.1.2.2
        auto [o, h] = opts2({});
        return lookup(handle(h)).cls;
    }
    if (n == "upf_object_in_class") {   // 11.1.2.3
        auto [o, h] = opts2({"-class"});
        if (!o.count("-class")) throw err("needs -class");
        const std::string& cls = o["-class"];
        if (!imClassIds().count(cls)) throw err("'" + cls + "' is not a class of the UPF information model");
        const ImObj& ob = lookup(handle(h));
        auto& mem = imClasses().at(ob.cls).membership;
        return std::find(mem.begin(), mem.end(), cls) != mem.end() ? "1" : "0";
    }
    if (n == "upf_query_object_pathname") {   // 11.1.2.4
        auto [o, h] = opts2({"-relative_to"});
        const ImObj& ob = lookup(handle(h));
        if (ob.path.empty()) return "";
        if (!o.count("-relative_to") || o["-relative_to"] == "upf_null") return ob.path;
        const ImObj& anc = lookup(handle(o["-relative_to"]));
        if (anc.path.empty()) return "";
        std::string a = anc.path;
        if (a == "/") return ob.path.substr(1);
        if (ob.path.size() > a.size() + 1 && ob.path.compare(0, a.size(), a) == 0 &&
            (ob.path[a.size()] == '/' || ob.path[a.size()] == '.'))
            return ob.path.substr(a.size() + 1);
        return "";
    }
    // query_upf (11.1.3.2): find_objects, or the extent of a power domain
    static const std::set<std::string> flags = {"-regexp", "-exact", "-ignore_case", "-leaf_only", "-non_leaf"};
    Opts o = opts(c, flags, {"-transitive"});
    static const std::set<std::string> known = {"-pattern", "-object_type", "-inst_type", "-direction", "-transitive",
                                                "-regexp", "-exact", "-ignore_case", "-leaf_only", "-non_leaf"};
    for (auto& [k, v] : o.v)
        if (!known.count(k)) throw err("unknown option " + k);
    if (o.pos.size() != 1) throw err("needs one power domain or scope");
    const std::string& root = o.pos[0];
    if (root.rfind("..", 0) == 0 || (!root.empty() && root[0] == '/'))
        throw err("the domain or scope must be in or below the current scope, not '" + root + "'");
    std::string type = o.get("-object_type");
    static const std::set<std::string> types = {"inst", "port", "supply_port", "net", "supply_net", "supply_set"};
    if (!type.empty() && !types.count(type))
        throw err("-object_type " + type + ": one of inst, port, supply_port, net, supply_net, supply_set");
    std::string inst = o.get("-inst_type", "all");
    static const std::set<std::string> insts = {"level_shifter", "isolation_cell", "switch_cell", "retention_cell", "all"};
    if (!insts.count(inst)) throw err("-inst_type " + inst + ": one of level_shifter, isolation_cell, switch_cell, retention_cell, all");
    // the find_objects options, for one object type
    auto sub = [&](const std::string& t) {
        Cmd f = c;
        f.w = {"find_objects", root};
        for (auto& [k, v] : o.v) {
            if (k == "-object_type" || k == "-inst_type") continue;
            if (k == "-direction" && t != "port" && t != "supply_port") continue;
            if ((k == "-leaf_only" || k == "-non_leaf") && t != "inst") continue;
            f.w.push_back(k);
            if (!flags.count(k)) f.w.push_back(v);
        }
        f.w.push_back("-object_type");
        f.w.push_back(t);
        return f;
    };
    std::string dn = join(scope, root);
    bool domainCentric = domains.count(dn) != 0;
    std::vector<std::string> out;
    // a port is also a net of its module: list each name once
    auto uniq = [&] {
        std::set<std::string> seen;
        std::vector<std::string> u;
        for (auto& e : out) if (seen.insert(e).second) u.push_back(e);
        return tcl::joinList(u);
    };
    auto addAll = [&](const std::string& l) { for (auto& e : splitList(l)) out.push_back(e); };
    std::vector<std::string> want = type.empty() ? std::vector<std::string>{"inst", "port", "net", "supply_port", "supply_net", "supply_set"}
                                                 : std::vector<std::string>{type};
    if (inst != "all") {
        // msim inserts no cells: isolation, level shifters, switches and retention are simulated on the
        // port connections and flops of the design, so there are no such instances to return
        if (type.empty() || type == "inst") want.erase(std::remove(want.begin(), want.end(), "inst"), want.end());
    }
    // a name that matches the pattern
    std::string pat = o.get("-pattern");
    if (!o.has("-pattern")) throw err("needs -pattern");
    bool icase = o.has("-ignore_case");
    auto lower = [](std::string t) { for (char& ch : t) ch = (char)std::tolower((unsigned char)ch); return t; };
    std::regex re;
    if (o.has("-regexp")) {
        try { re = std::regex(pat, icase ? std::regex::extended | std::regex::icase : std::regex::extended); }
        catch (const std::regex_error&) { throw err("bad -regexp pattern '" + pat + "'"); }
    }
    auto match = [&](const std::string& name) {
        if (o.has("-regexp")) return std::regex_search(name, re);
        if (o.has("-exact")) return icase ? lower(name) == lower(pat) : name == pat;
        std::string p = icase ? lower(pat) : pat, nm = icase ? lower(name) : name;
        return tcl::globMatch(p.c_str(), nm.c_str(), true);
    };
    if (!domainCentric) {
        for (auto& t : want) {
            if (t == "supply_set") {
                std::string rs = join(scope, root == "." ? "" : root);
                bool transitive = o.get("-transitive", "FALSE") == "TRUE";
                for (auto& [sn, s] : sets) {
                    size_t d = sn.rfind('.');
                    if (d != std::string::npos && domains.count(sn.substr(0, d))) continue;
                    auto [sc, leaf] = ImBuilder::splitName(sn);
                    bool under = sc == rs || (transitive && (rs.empty() || sc.rfind(rs + "/", 0) == 0));
                    if (under && match(leaf)) out.push_back(scope.empty() ? sn : sn.substr(std::min(sn.size(), scope.size() + 1)));
                }
                continue;
            }
            addAll(findObjects(sub(t), scope));
        }
        return uniq();
    }
    // domain-centric: the logic in the extent of the domain (-transitive is ignored)
    if (!dv) throw err("query_upf on a power domain needs the design; it is not available here");
    DomainMap dm;
    dm.build(*this);
    std::string base = design(scope);
    auto rel = [&](const std::string& p) {
        std::string r = base.empty() ? p : p.substr(std::min(p.size(), base.size() + 1));
        for (char& ch : r) if (ch == '.') ch = '/';
        return r;
    };
    auto leafOf = [](const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? p : p.substr(d + 1); };
    std::vector<std::string> scopes;   // instances in the domain
    if (dm.of(top) == dn) scopes.push_back(top);
    for (auto& p : dv->instances)
        if ((top.empty() || p.rfind(top + ".", 0) == 0) && dm.of(p) == dn) scopes.push_back(p);
    std::set<std::string> inDom(scopes.begin(), scopes.end());
    for (auto& t : want) {
        if (t == "inst") {
            for (auto& p : scopes) {
                if (p == top || !dv->model.count(p) || !match(leafOf(p))) continue;
                bool leaf = true;
                for (auto it = dv->instances.upper_bound(p + "."); it != dv->instances.end() && it->rfind(p + ".", 0) == 0; ++it)
                    if (dv->model.count(*it)) { leaf = false; break; }
                if ((o.has("-leaf_only") && !leaf) || (o.has("-non_leaf") && leaf)) continue;
                out.push_back(rel(p));
            }
        } else if (t == "port") {
            std::string dirS = o.get("-direction");
            int dir = dirS.empty() ? -1 : dirS == "in" ? 0 : dirS == "out" ? 1 : dirS == "inout" ? 2 : -2;
            if (dir == -2) throw err("-direction " + dirS + ": in, out or inout");
            for (auto& p : scopes)
                if (auto it = dv->portsOf.find(p); it != dv->portsOf.end())
                    for (auto& [pn, pd] : it->second)
                        if (match(pn) && (dir < 0 || pd == dir)) out.push_back(rel(p.empty() ? pn : p + "." + pn));
        } else if (t == "net") {
            for (auto& sg : dv->signals) {
                size_t d = sg.rfind('.');
                std::string sc = d == std::string::npos ? "" : sg.substr(0, d);
                if (inDom.count(sc) && match(leafOf(sg))) out.push_back(rel(sg));
            }
        } else {   // supply objects made in a scope of the domain
            auto inExtent = [&](const std::string& upfName) {
                return inDom.count(design(ImBuilder::splitName(upfName).first)) != 0;
            };
            auto relU = [&](const std::string& un) { return scope.empty() ? un : un.substr(std::min(un.size(), scope.size() + 1)); };
            if (t == "supply_port") {
                for (auto& p : ports) if (inExtent(p) && match(ImBuilder::splitName(p).second)) out.push_back(relU(p));
            } else if (t == "supply_net") {
                for (auto& p : nets) if (inExtent(p) && match(ImBuilder::splitName(p).second)) out.push_back(relU(p));
            } else {
                for (auto& [k, set] : domains.at(dn).handles) if (match(k)) out.push_back(relU(dn + "." + k));
            }
        }
    }
    return uniq();
}

}   // namespace upf
