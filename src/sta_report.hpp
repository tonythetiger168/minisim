// sta_report.hpp - msta's reports (report_timing, report_constraint,
// check_timing, report_clocks, report_qor), the SDF writer and the JSON dump,
// on top of the engine in sta.hpp. Times and capacitances are printed in the
// units of the first library.
#pragma once
#include <iomanip>
#include <ostream>
#include <sstream>

#include "sta.hpp"

namespace sta {

struct ReportOpts {
    bool late = true;          // -delay_type max
    int maxPaths = 1;          // per group
    int nworst = 1;            // per endpoint (only 1 is kept by the engine)
    int digits = 3;
    bool inputPins = false, showTrans = false, showCap = false;
    bool endOnly = false;      // -path end: one line per path
    std::string group;         // only this path group
    double slackLess = INF;    // -slack_lesser_than
    bool hasFrom = false, hasTo = false, hasThrough = false;
    std::unordered_set<int> from, to, through;
    std::unordered_set<int> fromClks, toClks;
};

class Reporter {
public:
    Sta& s;
    explicit Reporter(Sta& sta) : s(sta) {}

    std::string num(double v, int digits) const {
        char b[64];
        std::snprintf(b, sizeof b, "%.*f", digits, v);
        // No negative zero.
        bool zero = true;
        for (const char* p = b; *p; p++) if (*p >= '1' && *p <= '9') zero = false;
        if (zero && b[0] == '-') return std::string(b + 1);
        return b;
    }
    std::string t(double ns, int digits) const { return num(ns / s.timeUnit, digits); }
    std::string c(double pf, int digits) const { return num(pf / s.capUnit, digits); }

    std::string point(int n) const { return s.name(n) + " (" + s.refName(n) + ")"; }

    std::string groupOf(const PathEnd& pe) const {
        if (pe.kind == EndKind::Recovery || pe.kind == EndKind::Removal) return "**async_default**";
        return s.clocks[pe.capClk].name;
    }

    // Where the data part of a traced path begins: the first step that is
    // not in the clock network (0 when the whole path is clock network).
    size_t launchIndex(const std::vector<Sta::Step>& steps) const {
        size_t li = 0;
        while (li < steps.size() && s.tags[steps[li].node][steps[li].tag].key.isClk) li++;
        return li == steps.size() ? 0 : li;
    }
    int startNode(const std::vector<Sta::Step>& steps) const {
        size_t li = launchIndex(steps);
        return li > 0 ? steps[li - 1].node : steps[0].node;
    }

    std::string describe(int node, int rf, int clk, bool start) const {
        std::string ck = clk >= 0 ? " clocked by " + s.clocks[clk].name : "";
        if (s.isPort(node)) {
            if (s.nodes[node].clockSrc && start) return "clock source '" + s.clocks[clk].name + "'";
            return std::string(s.nl->ports[node].dir == gnl::Dir::In ? "input" : s.nl->ports[node].dir == gnl::Dir::Out ? "output" : "inout") + " port" + ck;
        }
        const CellInfo* ci = s.instInfo[s.nodes[node].inst];
        if (ci && ci->latch) return std::string(rf == RISE ? "positive" : "negative") + " level-sensitive latch" + ck;
        if (ci && ci->cell->sequential()) return std::string(rf == RISE ? "rising" : "falling") + " edge-triggered flip-flop" + ck;
        return "internal pin" + ck;
    }
    std::string instOrPort(int node) const {
        return s.isPort(node) ? s.name(node) : s.nl->insts[s.nodes[node].inst].name;
    }

    // The path ends to report: per group, the worst `maxPaths` with distinct endpoints.
    std::vector<const PathEnd*> select(const ReportOpts& o) {
        s.update();
        std::vector<const PathEnd*> out;
        std::map<std::string, int> count;
        std::set<std::pair<std::string, int>> seen;
        bool filter = o.hasFrom || o.hasThrough;
        for (auto& pe : s.ends) {  // sorted by slack
            if (isLate(pe.kind) != o.late) continue;
            if (pe.slack >= o.slackLess) continue;
            std::string g = groupOf(pe);
            if (!o.group.empty() && g != o.group) continue;
            if (o.hasTo && !o.to.count(pe.node) && !o.toClks.count(pe.capClk)) continue;
            if (count[g] >= o.maxPaths) continue;
            if (seen.count({g, pe.node})) continue;
            if (filter) {
                auto steps = s.trace(pe.node, pe.tag, o.late ? LATE : EARLY, pe.rf);
                if (o.hasFrom) {
                    size_t li = launchIndex(steps);
                    bool hit = o.fromClks.count(pe.launchClk) || o.from.count(steps[li].node) ||
                               (li > 0 && o.from.count(steps[li - 1].node));
                    if (!hit) continue;
                }
                if (o.hasThrough) {
                    bool hit = false;
                    for (auto& st : steps) if (o.through.count(st.node)) hit = true;
                    if (!hit) continue;
                }
            }
            seen.insert({g, pe.node});
            count[g]++;
            out.push_back(&pe);
        }
        // Group by group, in clock order, worst first inside a group.
        std::stable_sort(out.begin(), out.end(), [&](const PathEnd* a, const PathEnd* b) {
            return groupRank(*a) < groupRank(*b);
        });
        return out;
    }
    int groupRank(const PathEnd& pe) const {
        return pe.kind == EndKind::Recovery || pe.kind == EndKind::Removal ? (int)s.clocks.size() : pe.capClk;
    }

    void reportTiming(const ReportOpts& o, std::ostream& os) {
        auto sel = select(o);
        if (sel.empty()) {
            os << "No constrained " << (o.late ? "max" : "min") << " paths.\n";
            return;
        }
        if (o.endOnly) {
            size_t w = 8;
            for (auto* pe : sel) w = std::max(w, point(pe->node).size());
            os << std::left << std::setw((int)w + 2) << "Endpoint" << std::right << std::setw(12) << "Required"
               << std::setw(12) << "Arrival" << std::setw(12) << "Slack" << "\n" << std::string(w + 38, '-') << "\n";
            for (auto* pe : sel)
                os << std::left << std::setw((int)w + 2) << point(pe->node) << std::right << std::setw(12)
                   << t(pe->required, o.digits) << std::setw(12) << t(pe->arrival, o.digits) << std::setw(12)
                   << t(pe->slack, o.digits) << (pe->slack < 0 ? "  (VIOLATED)" : "") << "\n";
            return;
        }
        for (auto* pe : sel) path(*pe, o, os);
    }

    struct Line {
        std::string text;
        bool hasIncr = false, hasPath = false, hasCap = false, hasTrans = false;
        double incr = 0, path = 0, cap = 0, trans = 0;
        char rf = ' ';
    };

    void path(const PathEnd& pe, const ReportOpts& o, std::ostream& os) {
        int m = isLate(pe.kind) ? LATE : EARLY;
        auto steps = s.trace(pe.node, pe.tag, m, pe.rf);
        size_t li = launchIndex(steps);
        const Clock& L = s.clocks[pe.launchClk];
        const Clock& Cc = s.clocks[pe.capClk];
        std::vector<Line> lines;
        auto add = [&](const std::string& text, double incr, double path, char rf = ' ') {
            Line l;
            l.text = text;
            l.hasIncr = l.hasPath = true;
            l.incr = incr;
            l.path = path;
            l.rf = rf;
            lines.push_back(l);
            return &lines.back();
        };
        auto total = [&](const std::string& text, double v) {
            Line l;
            l.text = text;
            l.hasPath = true;
            l.path = v;
            lines.push_back(l);
        };
        auto rule = [&] { lines.push_back(Line{"-"}); };
        auto blank = [&] { lines.push_back(Line{""}); };

        // ---- data arrival
        double base = pe.launchT, prev;
        bool clockData = s.tags[steps[0].node][steps[0].tag].key.isClk && li == 0;
        add("clock " + L.name + " (" + (pe.launchEdge == RISE ? "rise" : "fall") + " edge)", base, base);
        prev = base;
        size_t first = li;
        if (li > 0) {
            double ck = steps[li - 1].arr;
            add(std::string("clock network delay (") + (L.propagated ? "propagated" : "ideal") + ")", ck, base + ck);
            prev = base + ck;
            add(point(steps[li - 1].node), 0, prev, steps[li - 1].rf == RISE ? 'r' : 'f');
            // A latch that passed on time borrowed by the path into it.
            auto bi = s.borrows.find(steps[li].node);
            if (bi != s.borrows.end())
                for (auto& b : bi->second)
                    if (m == LATE && b.rf == steps[li].rf && b.key == s.tags[steps[li].node][steps[li].tag].key &&
                        std::fabs(b.arr - steps[li].arr) < 1e-12) {
                        prev += b.given;
                        add("time given to startpoint", b.given, prev);
                    }
        } else if (clockData) {
            double lat = steps[0].arr;
            add(std::string("clock network delay (") + (L.propagated ? "propagated" : "ideal") + ")", lat, base + lat);
            prev = base + lat;
        } else {
            double lat = L.srcLat[m] + L.netLat[m], ext = 0;
            auto it = s.seeds.find(steps[0].node);
            if (it != s.seeds.end())
                for (auto& sd : it->second)
                    if (sd.input && sd.key.clk == pe.launchClk && sd.key.edge == pe.launchEdge) ext = sd.ext[m];
            add("clock network delay (ideal)", lat, base + lat);
            add("input external delay", ext, base + lat + ext, steps[0].rf == RISE ? 'r' : 'f');
            prev = base + lat + ext;
        }
        for (size_t i = first; i < steps.size(); i++) {
            const Sta::Step& st = steps[i];
            const Node& x = s.nodes[st.node];
            bool last = i + 1 == steps.size();
            if (!(x.driver || last || o.inputPins)) continue;
            Line* l = add(point(st.node), base + st.arr - prev, base + st.arr, st.rf == RISE ? 'r' : 'f');
            prev = base + st.arr;
            if (x.driver) { l->hasCap = true; l->cap = x.load; }
            l->hasTrans = true;
            l->trans = x.slew[m][st.rf];
        }
        total("data arrival time", pe.arrival);
        blank();

        // ---- data required
        double req;
        bool late = isLate(pe.kind);
        if (pe.pathDelay) {
            add(late ? "max_delay" : "min_delay", pe.pathDelayValue, pe.pathDelayValue);
            req = pe.pathDelayValue;
        } else {
            add("clock " + Cc.name + " (" + (pe.capEdge == RISE ? "rise" : "fall") + " edge)", pe.capT, pe.capT);
            req = pe.capT;
        }
        req += pe.capArr;
        add(std::string("clock network delay (") + (Cc.propagated && pe.clkNode >= 0 ? "propagated" : "ideal") + ")", pe.capArr, req);
        if (pe.latch) {
            // Against the opening edge, plus what is borrowed.
            add(point(pe.clkNode), 0, req, pe.clkRf == RISE ? 'r' : 'f');
            req += pe.borrow;
            add("time borrowed from endpoint", pe.borrow, req);
        } else {
        if (pe.crpr != 0) {
            req += late ? pe.crpr : -pe.crpr;
            add("clock reconvergence pessimism", late ? pe.crpr : -pe.crpr, req);
        }
        if (pe.unc != 0) {
            req += late ? -pe.unc : pe.unc;
            add("clock uncertainty", late ? -pe.unc : pe.unc, req);
        }
        }
        if (pe.latch) {
        } else if (pe.clkNode >= 0) {
            add(point(pe.clkNode), 0, req, pe.clkRf == RISE ? 'r' : 'f');
            req += late ? -pe.lib : pe.lib;
            bool gate = pe.kind == EndKind::GateSetup || pe.kind == EndKind::GateHold;
            add(std::string(gate ? "" : "library ") + kindLabel(pe.kind) + " time", late ? -pe.lib : pe.lib, req);
        } else {
            req -= pe.ext;
            add("output external delay", -pe.ext, req);
        }
        total("data required time", pe.required);
        rule();
        total("data required time", pe.required);
        total("data arrival time", -pe.arrival);
        rule();

        // ---- print
        size_t w = 40;
        for (auto& l : lines) w = std::max(w, l.text.size() + 2);
        int nw = 8 + o.digits;
        size_t width = w + 2 * nw + 2 + (o.showCap ? nw : 0) + (o.showTrans ? nw : 0);
        int startN = startNode(steps);
        int startRf = li > 0 ? steps[li - 1].rf : steps[0].rf;
        os << "  Startpoint: " << instOrPort(startN) << " (" << describe(startN, startRf, pe.launchClk, true) << ")\n";
        os << "  Endpoint: " << instOrPort(pe.node) << " ("
           << (pe.kind == EndKind::Recovery || pe.kind == EndKind::Removal
                   ? std::string(kindLabel(pe.kind)) + " check against " + (pe.clkRf == RISE ? "rising" : "falling") + "-edge clock " + Cc.name
                   : pe.kind == EndKind::GateSetup || pe.kind == EndKind::GateHold
                         ? std::string(pe.kind == EndKind::GateSetup ? (pe.clkRf == RISE ? "rising" : "falling")
                                                                     : (pe.clkRf == RISE ? "falling" : "rising")) +
                               " clock gating-check end-point clocked by " + Cc.name
                         : describe(pe.node, pe.clkRf, pe.capClk, false))
           << ")\n";
        os << "  Path Group: " << groupOf(pe) << "\n";
        os << "  Path Type: " << (late ? "max" : "min") << "\n\n";
        os << "  " << std::left << std::setw((int)w) << "Point" << std::right;
        if (o.showCap) os << std::setw(nw) << "Cap";
        if (o.showTrans) os << std::setw(nw) << "Trans";
        os << std::setw(nw) << "Incr" << std::setw(nw) << "Path" << "\n";
        os << "  " << std::string(width, '-') << "\n";
        for (auto& l : lines) {
            if (l.text == "-") { os << "  " << std::string(width, '-') << "\n"; continue; }
            if (l.text.empty()) { os << "\n"; continue; }
            os << "  " << std::left << std::setw((int)w) << l.text << std::right;
            if (o.showCap) os << std::setw(nw) << (l.hasCap ? c(l.cap, o.digits) : "");
            if (o.showTrans) os << std::setw(nw) << (l.hasTrans ? t(l.trans, o.digits) : "");
            os << std::setw(nw) << (l.hasIncr ? t(l.incr, o.digits) : "") << std::setw(nw)
               << (l.hasPath ? t(l.path, o.digits) : "");
            if (l.rf != ' ') os << " " << l.rf;
            os << "\n";
        }
        os << "  " << std::left << std::setw((int)w) << (pe.slack < 0 ? "slack (VIOLATED)" : "slack (MET)") << std::right;
        if (o.showCap) os << std::setw(nw) << "";
        if (o.showTrans) os << std::setw(nw) << "";
        os << std::setw(nw) << "" << std::setw(nw) << t(pe.slack, o.digits) << "\n\n";
        if (pe.latch) {
            auto row = [&](const std::string& text, double v) {
                os << "  " << std::left << std::setw(36) << text << std::right << std::setw(nw) << t(v, o.digits) << "\n";
            };
            os << "  Time Borrowing Information\n  " << std::string(36 + nw, '-') << "\n";
            row(Cc.name + " pulse width", pe.pulse);
            if (pe.lib != 0) row("library setup time", -pe.lib);
            if (pe.unc != 0) row("clock uncertainty", -pe.unc);
            if (pe.crpr != 0) row("clock reconvergence pessimism", pe.crpr);
            os << "  " << std::string(36 + nw, '-') << "\n";
            row("max time borrow", pe.maxBorrow);
            row("actual time borrow", pe.borrow);
            os << "  " << std::string(36 + nw, '-') << "\n\n";
        }
        os << "\n";
    }

    // ---------------------------------------------------------- constraints

    // Returns the number of violations.
    int reportConstraint(bool allViolators, int digits, std::ostream& os) {
        s.update();
        int viol = 0;
        os << "  " << std::left << std::setw(22) << "Group" << std::setw(8) << "Type" << std::right << std::setw(12)
           << "WNS" << std::setw(12) << "TNS" << std::setw(12) << "Violations" << std::setw(12) << "Endpoints" << "\n";
        os << "  " << std::string(78, '-') << "\n";
        for (int late = 1; late >= 0; late--) {
            for (size_t k = 0; k < s.clocks.size(); k++) {
                Sta::Summary sm = s.summary(late, (int)k);
                if (!sm.total) continue;
                os << "  " << std::left << std::setw(22) << s.clocks[k].name << std::setw(8) << (late ? "max" : "min")
                   << std::right << std::setw(12) << t(sm.wns, digits) << std::setw(12) << t(sm.tns, digits)
                   << std::setw(12) << sm.violations << std::setw(12) << sm.total << "\n";
                viol += sm.violations;
            }
        }
        os << "\n";
        if (allViolators) {
            for (int late = 1; late >= 0; late--) {
                std::vector<const PathEnd*> v;
                std::set<int> seen;
                for (auto& pe : s.ends)
                    if (isLate(pe.kind) == (bool)late && pe.slack < 0 && seen.insert(pe.node).second) v.push_back(&pe);
                if (v.empty()) continue;
                size_t w = 8;
                for (auto* pe : v) w = std::max(w, s.name(pe->node).size());
                os << "  " << (late ? "max_delay/setup" : "min_delay/hold") << "\n\n";
                os << "  " << std::left << std::setw((int)w + 2) << "Endpoint" << std::right << std::setw(12)
                   << "Required" << std::setw(12) << "Actual" << std::setw(12) << "Slack" << "\n";
                os << "  " << std::string(w + 38, '-') << "\n";
                for (auto* pe : v)
                    os << "  " << std::left << std::setw((int)w + 2) << s.name(pe->node) << std::right << std::setw(12)
                       << t(pe->required, digits) << std::setw(12) << t(pe->arrival, digits) << std::setw(12)
                       << t(pe->slack, digits) << "  (VIOLATED)\n";
                os << "\n";
            }
        }
        auto drc = [&](const char* title, const std::vector<Violation>& v, bool cap) {
            viol += (int)v.size();
            if (v.empty()) return;
            os << "  " << title << ": " << v.size() << " violation" << (v.size() == 1 ? "" : "s") << "\n";
            if (!allViolators) {
                os << "    worst: " << s.name(v[0].node) << "  limit " << (cap ? c(v[0].limit, digits) : t(v[0].limit, digits))
                   << "  actual " << (cap ? c(v[0].actual, digits) : t(v[0].actual, digits)) << "\n\n";
                return;
            }
            size_t w = 8;
            for (auto& x : v) w = std::max(w, s.name(x.node).size());
            os << "\n  " << std::left << std::setw((int)w + 2) << (cap ? "Pin" : "Pin") << std::right << std::setw(12)
               << "Required" << std::setw(12) << "Actual" << std::setw(12) << "Slack" << "\n";
            os << "  " << std::string(w + 38, '-') << "\n";
            for (auto& x : v)
                os << "  " << std::left << std::setw((int)w + 2) << s.name(x.node) << std::right << std::setw(12)
                   << (cap ? c(x.limit, digits) : t(x.limit, digits)) << std::setw(12)
                   << (cap ? c(x.actual, digits) : t(x.actual, digits)) << std::setw(12)
                   << (cap ? c(x.slack, digits) : t(x.slack, digits)) << "  (VIOLATED)\n";
            os << "\n";
        };
        drc("max_transition", s.transViol, false);
        drc("max_capacitance", s.capViol, true);
        // Minimum pulse width and period: the violations.
        size_t np = 0, nper = 0;
        while (np < s.pulses.size() && s.pulses[np].slack < 0) np++;
        while (nper < s.periods.size() && s.periods[nper].slack < 0) nper++;
        viol += (int)(np + nper);
        if (np) {
            os << "  min_pulse_width: " << np << " violation" << (np == 1 ? "" : "s") << "\n";
            if (allViolators) {
                os << "\n";
                pulseTable(np, digits, os);
            } else {
                const PulseCheck& pc = s.pulses[0];
                os << "    worst: " << s.name(pc.node) << " (" << (pc.high ? "high" : "low") << ")  required "
                   << t(pc.required, digits) << "  actual " << t(pc.actual, digits) << "\n\n";
            }
        }
        if (nper) {
            os << "  min_period: " << nper << " violation" << (nper == 1 ? "" : "s") << "\n";
            if (allViolators) {
                os << "\n";
                periodTable(nper, digits, os);
            } else {
                const PeriodCheck& pc = s.periods[0];
                os << "    worst: " << s.name(pc.node) << "  required " << t(pc.required, digits) << "  period "
                   << t(pc.period, digits) << "\n\n";
            }
        }
        if (!viol) os << "  All constraints are met.\n";
        return viol;
    }

    void pulseTable(size_t count, int digits, std::ostream& os) {
        size_t w = 8;
        for (size_t i = 0; i < count; i++) w = std::max(w, s.name(s.pulses[i].node).size() + 7);
        os << "  " << std::left << std::setw((int)w + 2) << "Pin" << std::right << std::setw(12) << "Required"
           << std::setw(12) << "Actual" << std::setw(12) << "Slack" << "\n";
        os << "  " << std::string(w + 38, '-') << "\n";
        for (size_t i = 0; i < count; i++) {
            const PulseCheck& pc = s.pulses[i];
            os << "  " << std::left << std::setw((int)w + 2) << (s.name(pc.node) + (pc.high ? " (high)" : " (low)"))
               << std::right << std::setw(12) << t(pc.required, digits) << std::setw(12) << t(pc.actual, digits)
               << std::setw(12) << t(pc.slack, digits) << (pc.slack < 0 ? "  (VIOLATED)" : "") << "\n";
        }
        os << "\n";
    }

    void periodTable(size_t count, int digits, std::ostream& os) {
        size_t w = 8;
        for (size_t i = 0; i < count; i++) w = std::max(w, s.name(s.periods[i].node).size());
        os << "  " << std::left << std::setw((int)w + 2) << "Pin" << std::right << std::setw(12) << "Required"
           << std::setw(12) << "Period" << std::setw(12) << "Slack" << "\n";
        os << "  " << std::string(w + 38, '-') << "\n";
        for (size_t i = 0; i < count; i++) {
            const PeriodCheck& pc = s.periods[i];
            os << "  " << std::left << std::setw((int)w + 2) << s.name(pc.node) << std::right << std::setw(12)
               << t(pc.required, digits) << std::setw(12) << t(pc.period, digits) << std::setw(12)
               << t(pc.slack, digits) << (pc.slack < 0 ? "  (VIOLATED)" : "") << "\n";
        }
        os << "\n";
    }

    // report_min_pulse_width: the worst `count` checks (0: all, or the
    // violations only); verbose shows how each width is made up.
    int reportMinPulseWidth(bool violators, bool verbose, size_t count, int digits, std::ostream& os) {
        s.update();
        size_t n = s.pulses.size();
        if (violators) {
            n = 0;
            while (n < s.pulses.size() && s.pulses[n].slack < 0) n++;
        }
        if (count && count < n) n = count;
        if (!n) {
            os << (violators ? "  No min_pulse_width violations.\n" : "  No min_pulse_width checks.\n");
            return 0;
        }
        if (!verbose) {
            pulseTable(n, digits, os);
            return (int)n;
        }
        int nw = 8 + digits;
        for (size_t i = 0; i < n; i++) {
            const PulseCheck& pc = s.pulses[i];
            const Clock& c = s.clocks[pc.clk];
            auto line = [&](const std::string& text, double incr, double path, bool hasIncr = true) {
                os << "  " << std::left << std::setw(40) << text << std::right << std::setw(nw)
                   << (hasIncr ? t(incr, digits) : std::string()) << std::setw(nw) << t(path, digits) << "\n";
            };
            const char* net = c.propagated ? "clock network delay (propagated)" : "clock network delay (ideal)";
            os << "  Pin: " << s.name(pc.node) << "\n  Check: min_pulse_width (" << (pc.high ? "high" : "low") << ")\n\n";
            line("clock " + c.name + " (" + (pc.openEdge == RISE ? "rise" : "fall") + " edge)", pc.openT, pc.openT);
            line(net, pc.openArr, pc.openT + pc.openArr);
            line("open edge arrival time", 0, pc.openT + pc.openArr, false);
            os << "\n";
            line("clock " + c.name + " (" + (pc.closeEdge == RISE ? "rise" : "fall") + " edge)", pc.closeT, pc.closeT);
            line(net, pc.closeArr, pc.closeT + pc.closeArr);
            if (pc.crpr != 0) line("clock reconvergence pessimism", pc.crpr, pc.closeT + pc.closeArr + pc.crpr);
            line("close edge arrival time", 0, pc.closeT + pc.closeArr + pc.crpr, false);
            os << "  " << std::string(40 + 2 * nw, '-') << "\n";
            line("required pulse width", 0, pc.required, false);
            line("actual pulse width", 0, pc.actual, false);
            os << "  " << std::string(40 + 2 * nw, '-') << "\n";
            line(pc.slack < 0 ? "slack (VIOLATED)" : "slack (MET)", 0, pc.slack, false);
            os << "\n\n";
        }
        return (int)n;
    }

    int reportMinPeriod(bool violators, size_t count, int digits, std::ostream& os) {
        s.update();
        size_t n = s.periods.size();
        if (violators) {
            n = 0;
            while (n < s.periods.size() && s.periods[n].slack < 0) n++;
        }
        if (count && count < n) n = count;
        if (!n) {
            os << (violators ? "  No min_period violations.\n" : "  No min_period checks.\n");
            return 0;
        }
        periodTable(n, digits, os);
        return (int)n;
    }

    // Returns the number of problems found.
    int checkTiming(bool verbose, std::ostream& os) {
        s.update();
        int total = 0;
        auto list = [&](const char* what, const std::vector<int>& v) {
            if (v.empty()) return;
            total += (int)v.size();
            os << "Warning: " << v.size() << " " << what << "\n";
            size_t n = verbose ? v.size() : std::min<size_t>(v.size(), 10);
            for (size_t i = 0; i < n; i++) os << "    " << s.name(v[i]) << "\n";
            if (n < v.size()) os << "    ... (" << v.size() - n << " more, -verbose lists all)\n";
        };
        if (s.clocks.empty()) {
            os << "Warning: no clocks are defined\n";
            total++;
        }
        list("register clock pins with no clock (no_clock)", s.unclocked);
        list("endpoints that are not constrained (unconstrained_endpoints)", s.unconstrained);
        list("input ports with no input delay (no_input_delay)", s.noInputDelay);
        list("combinational loops, cut at (loops)", s.loopNodes);
        std::vector<int> multi;
        for (size_t n = 0; n < s.nodes.size(); n++) {
            std::set<int> ck;
            for (auto& tg : s.tags[n]) if (tg.key.isClk) ck.insert(tg.key.clk);
            bool reg = false;
            for (int k = s.foutOff[n]; k < s.foutOff[n + 1]; k++) if (s.edges[s.foutIdx[k]].kind == LaunchEdge) reg = true;
            if (reg && ck.size() > 1) multi.push_back((int)n);
        }
        list("register clock pins reached by more than one clock (multiple_clock)", multi);
        for (auto& e : s.excs)
            if (e.ignored) {
                os << "Warning: the path exception at " << e.def->loc.str() << " names no object and is ignored\n";
                total++;
            }
        for (auto& w : s.warnings) {
            os << "Warning: " << w.text << " (" << w.code << ")\n";
            total++;
        }
        if (!total) os << "check_timing: no problems found.\n";
        return total;
    }

    void reportClocks(std::ostream& os) {
        os << "  " << std::left << std::setw(20) << "Clock" << std::right << std::setw(10) << "Period" << "   "
           << std::left << std::setw(20) << "Waveform" << std::setw(6) << "Attrs" << "Sources\n";
        os << "  " << std::string(76, '-') << "\n";
        for (auto& ck : s.clocks) {
            std::string wf = "{" + t(ck.edgeT[RISE], 3) + " " + t(ck.edgeT[FALL], 3) + "}";
            std::string at = std::string(ck.propagated ? "p" : "") + (ck.generated ? "G" : "") + (ck.sources.empty() ? "v" : "");
            std::string src;
            for (int n : ck.sources) src += (src.empty() ? "" : " ") + s.name(n);
            os << "  " << std::left << std::setw(20) << ck.name << std::right << std::setw(10) << t(ck.period, 3) << "   "
               << std::left << std::setw(20) << wf << std::setw(6) << at << "{" << src << "}\n";
        }
        os << "  p: propagated  G: generated  v: virtual\n";
    }

    void reportQor(std::ostream& os) {
        s.update();
        for (int late = 1; late >= 0; late--)
            for (size_t k = 0; k < s.clocks.size(); k++) {
                Sta::Summary sm = s.summary(late, (int)k);
                if (!sm.total) continue;
                const PathEnd* worst = nullptr;
                for (auto& pe : s.ends)
                    if (isLate(pe.kind) == (bool)late && pe.capClk == (int)k) { worst = &pe; break; }
                auto steps = s.trace(worst->node, worst->tag, late ? LATE : EARLY, worst->rf);
                int logic = 0;
                for (size_t i = launchIndex(steps); i < steps.size(); i++)
                    if (steps[i].edge >= 0 && s.edges[steps[i].edge].kind == CombEdge) logic++;
                os << "  Timing Path Group '" << s.clocks[k].name << "' (" << (late ? "max" : "min") << ")\n";
                os << "  -----------------------------------\n";
                os << "  Levels of Logic:        " << std::setw(10) << logic << "\n";
                os << "  Critical Path Length:   " << std::setw(10) << t(worst->arrival - worst->launchT, 3) << "\n";
                os << "  Critical Path Slack:    " << std::setw(10) << t(sm.wns, 3) << "\n";
                os << "  Critical Path Clk Period:" << std::setw(9) << t(s.clocks[k].period, 3) << "\n";
                os << "  Total Negative Slack:   " << std::setw(10) << t(sm.tns, 3) << "\n";
                os << "  No. of Violating Paths: " << std::setw(10) << sm.violations << "\n\n";
            }
        size_t comb = 0, seq = 0, bufinv = 0, black = 0;
        double area = 0;
        for (size_t i = 0; i < s.instInfo.size(); i++) {
            const CellInfo* ci = s.instInfo[i];
            if (!ci) { black++; continue; }
            area += ci->cell->area;
            if (ci->cell->sequential()) seq++;
            else comb++;
            if (!ci->cell->sequential() && ci->cell->pins.size() == 2) bufinv++;
        }
        os << "  Cell Count\n  -----------------------------------\n";
        os << "  Leaf Cell Count:        " << std::setw(10) << s.instInfo.size() << "\n";
        os << "  Buf/Inv Cell Count:     " << std::setw(10) << bufinv << "\n";
        os << "  Combinational Cells:    " << std::setw(10) << comb << "\n";
        os << "  Sequential Cells:       " << std::setw(10) << seq << "\n";
        if (black) os << "  Cells not in a library: " << std::setw(10) << black << "\n";
        os << "\n  Area\n  -----------------------------------\n";
        os << "  Cell Area:              " << std::setw(10) << num(area, 3) << "\n\n";
        os << "  Design Rules\n  -----------------------------------\n";
        os << "  Nets:                   " << std::setw(10) << s.nl->nets.size() << "\n";
        os << "  Max Trans Violations:   " << std::setw(10) << s.transViol.size() << "\n";
        os << "  Max Cap Violations:     " << std::setw(10) << s.capViol.size() << "\n";
    }

    // ------------------------------------------------------------------ SDF

    static std::string sdfName(const std::string& n) {
        std::string r;
        for (char ch : n) {
            if (ch == '[' || ch == ']' || ch == '$' || ch == '.' || ch == ' ') r += '\\';
            r += ch;
        }
        return r;
    }
    static std::string sdfPort(const std::string& n) {
        // A bus bit keeps its select; anything else odd is escaped.
        return n;
    }

    // SDF 3.0: one CELL per instance, IOPATH delays as (min::max) triples
    // for the rise and fall of the output, and the timing checks.
    void writeSdf(std::ostream& os, const std::string& version = "") {
        s.update();
        auto trip = [&](double mn, double mx) {
            return "(" + num(mn / s.timeUnit, 4) + "::" + num(mx / s.timeUnit, 4) + ")";
        };
        os << "(DELAYFILE\n  (SDFVERSION \"3.0\")\n  (DESIGN \"" << s.nl->top << "\")\n  (VENDOR \"minisim\")\n"
           << "  (PROGRAM \"msta\")\n  (VERSION \"" << version << "\")\n  (DIVIDER /)\n  (TIMESCALE "
           << (s.timeUnit == 1 ? "1ns" : s.timeUnit == 1e-3 ? "1ps" : s.timeUnit == 1e-2 ? "10ps" : s.timeUnit == 1e-1 ? "100ps" : "1ns")
           << ")\n";
        double scale = s.timeUnit == 1 || s.timeUnit == 1e-3 || s.timeUnit == 1e-2 || s.timeUnit == 1e-1 ? s.timeUnit : 1;
        auto tr = [&](double mn, double mx) {
            return "(" + num(mn / scale, 4) + "::" + num(mx / scale, 4) + ")";
        };
        (void)trip;
        // Checks by instance.
        std::vector<std::vector<int>> chk(s.nl->insts.size());
        for (size_t i = 0; i < s.checks.size(); i++) chk[s.checks[i].inst].push_back((int)i);
        for (size_t i = 0; i < s.nl->insts.size(); i++) {
            const CellInfo* ci = s.instInfo[i];
            if (!ci) continue;
            std::ostringstream io, tc;
            for (int n = s.instNodeOff[i]; n < s.instNodeOff[i + 1]; n++)
                for (int k = s.finOff[n]; k < s.finOff[n + 1]; k++) {
                    const Edge& e = s.edges[s.finIdx[k]];
                    if (e.kind == NetEdge) continue;
                    // Output rise and fall: the worst and best over the input transitions.
                    double mx[2] = {-INF, -INF}, mn[2] = {INF, INF};
                    int irfOnly = -1;
                    for (int irf = 0; irf < 2; irf++)
                        for (int orf = 0; orf < 2; orf++) {
                            if (!e.has(irf, orf)) continue;
                            mx[orf] = std::max(mx[orf], e.d[LATE][irf][orf]);
                            mn[orf] = std::min(mn[orf], e.d[EARLY][irf][orf]);
                            irfOnly = irf;
                        }
                    for (int orf = 0; orf < 2; orf++)
                        if (!valid(mx[orf])) { mx[orf] = valid(mx[!orf]) ? mx[!orf] : 0; mn[orf] = valid(mn[!orf]) ? mn[!orf] : 0; }
                    std::string from = sdfPort(*s.nodePin[e.from]);
                    if (e.kind == LaunchEdge) from = std::string(irfOnly == RISE ? "(posedge " : "(negedge ") + from + ")";
                    io << "        (IOPATH " << from << " " << sdfPort(*s.nodePin[n]) << " " << tr(mn[RISE], mx[RISE]) << " "
                       << tr(mn[FALL], mx[FALL]) << ")\n";
                }
            for (int c : chk[i]) {
                const Check& ck = s.checks[c];
                const liberty::Arc& a = *ck.arc;
                bool late = a.isSetup();
                int crf = a.rising() ? RISE : FALL;
                double v = -INF;
                for (int rf = 0; rf < 2; rf++) {
                    const liberty::Table& tb = rf == RISE ? a.riseCons : a.fallCons;
                    if (tb.empty()) continue;
                    v = std::max(v, tb.constraint(s.nodes[ck.data].slew[late ? LATE : EARLY][rf], s.nodes[ck.clk].slew[late ? EARLY : LATE][crf]));
                }
                if (!valid(v)) continue;
                const char* kw = a.type == liberty::ArcType::SetupRising || a.type == liberty::ArcType::SetupFalling ? "SETUP"
                               : a.type == liberty::ArcType::HoldRising || a.type == liberty::ArcType::HoldFalling ? "HOLD"
                               : a.isSetup() ? "RECOVERY" : "REMOVAL";
                tc << "      (" << kw << " " << sdfPort(*s.nodePin[ck.data]) << " (" << (crf == RISE ? "posedge " : "negedge ")
                   << sdfPort(*s.nodePin[ck.clk]) << ") (" << num(v / scale, 4) << "))\n";
            }
            if (io.str().empty() && tc.str().empty()) continue;
            os << "  (CELL\n    (CELLTYPE \"" << s.nl->insts[i].cell << "\")\n    (INSTANCE " << sdfName(s.nl->insts[i].name) << ")\n";
            if (!io.str().empty()) os << "    (DELAY\n      (ABSOLUTE\n" << io.str() << "      )\n    )\n";
            if (!tc.str().empty()) os << "    (TIMINGCHECK\n" << tc.str() << "    )\n";
            os << "  )\n";
        }
        os << ")\n";
    }

    // ----------------------------------------------------------------- JSON

    static std::string js(const std::string& x) {
        std::string r = "\"";
        for (char ch : x) {
            if (ch == '"' || ch == '\\') r += '\\';
            r += ch;
        }
        return r + "\"";
    }

    void writeJson(std::ostream& os, int maxPaths) {
        s.update();
        os << "{\n  \"design\": " << js(s.nl->top) << ",\n  \"time_unit_ns\": " << s.timeUnit << ",\n  \"summary\": [";
        bool first = true;
        for (int late = 1; late >= 0; late--)
            for (size_t k = 0; k < s.clocks.size(); k++) {
                Sta::Summary sm = s.summary(late, (int)k);
                if (!sm.total) continue;
                os << (first ? "\n" : ",\n") << "    {\"group\": " << js(s.clocks[k].name) << ", \"type\": \"" << (late ? "max" : "min")
                   << "\", \"wns\": " << t(sm.wns, 6) << ", \"tns\": " << t(sm.tns, 6) << ", \"violations\": " << sm.violations
                   << ", \"endpoints\": " << sm.total << "}";
                first = false;
            }
        os << "\n  ],\n  \"paths\": [";
        first = true;
        for (int late = 1; late >= 0; late--) {
            ReportOpts o;
            o.late = late;
            o.maxPaths = maxPaths;
            for (auto* pe : select(o)) {
                auto steps = s.trace(pe->node, pe->tag, late ? LATE : EARLY, pe->rf);
                os << (first ? "\n" : ",\n") << "    {\"type\": \"" << (late ? "max" : "min") << "\", \"check\": \"" << kindLabel(pe->kind)
                   << "\", \"group\": " << js(groupOf(*pe)) << ", \"startpoint\": " << js(s.name(startNode(steps)))
                   << ", \"endpoint\": " << js(s.name(pe->node)) << ", \"launch_clock\": " << js(s.clocks[pe->launchClk].name)
                   << ", \"capture_clock\": " << js(s.clocks[pe->capClk].name) << ", \"arrival\": " << t(pe->arrival, 6)
                   << ", \"required\": " << t(pe->required, 6) << ", \"crpr\": " << t(pe->crpr, 6) << ", \"slack\": " << t(pe->slack, 6) << ", \"points\": [";
                for (size_t i = 0; i < steps.size(); i++)
                    os << (i ? ", " : "") << "{\"pin\": " << js(s.name(steps[i].node)) << ", \"edge\": \"" << (steps[i].rf == RISE ? "r" : "f")
                       << "\", \"arrival\": " << t(pe->launchT + steps[i].arr, 6) << "}";
                os << "]}";
                first = false;
            }
        }
        os << "\n  ],\n  \"min_pulse_width_violations\": "
           << std::count_if(s.pulses.begin(), s.pulses.end(), [](const PulseCheck& p) { return p.slack < 0; })
           << ",\n  \"min_period_violations\": "
           << std::count_if(s.periods.begin(), s.periods.end(), [](const PeriodCheck& p) { return p.slack < 0; })
           << ",\n  \"max_transition_violations\": " << s.transViol.size() << ",\n  \"max_capacitance_violations\": "
           << s.capViol.size() << "\n}\n";
    }
};

}  // namespace sta
