// sta_spef.hpp - back-annotates parasitics from a SPEF file (spef.hpp) onto
// msta's timing graph.
//
// For each *D_NET that names a net of the design:
//   * the driver's load becomes the pin capacitances plus the net's wire
//     capacitance (ground capacitors, and coupling capacitors counted once to
//     ground); set_load on that net no longer applies;
//   * each driver to load arc gets the Elmore delay of the RC network: the
//     sum over the resistors on the way of the resistance times all the
//     capacitance behind it, load pin capacitances included. A network that
//     is not a tree is timed along the tree a breadth-first walk from the
//     driver finds.
// The driver itself is still timed into the total capacitance (no effective
// capacitance), which is pessimistic for resistive nets.
#pragma once
#include "spef.hpp"
#include "sta.hpp"

namespace sta {

struct SpefResult {
    size_t nets = 0, annotated = 0, unknown = 0, noDriver = 0, loops = 0;
};

inline SpefResult annotateSpef(Sta& s, const spef::File& f) {
    SpefResult res;
    res.nets = f.nets.size();
    if (s.spefCap.empty()) s.spefCap.assign(s.nl->nets.size(), -1.0);
    if (s.wireDelay.empty()) s.wireDelay.assign(s.edges.size(), 0.0f);
    const double toPF = f.cScale * 1e12, toOhm = f.rScale;
    char delim = f.h.delimiter;
    // The lazy name tables are not safe to build from several threads.
    s.findInst("");
    s.findNet("");
    // "inst:pin" or a port name -> node.
    auto nodeOf = [&](const std::string& name) {
        size_t c = name.rfind(delim);
        if (c == std::string::npos) {
            auto it = s.portByName.find(name);
            return it == s.portByName.end() ? -1 : it->second;
        }
        int inst = s.findInst(name.substr(0, c));
        if (inst < 0) return -1;
        for (int k = s.instNodeOff[inst]; k < s.instNodeOff[inst + 1]; k++)
            if (name.compare(c + 1, std::string::npos, *s.nodePin[k]) == 0) return k;
        return -1;
    };
    std::vector<char> status(f.nets.size(), 0);  // 1 annotated, 2 unknown, 3 no driver, +4 loop
    auto doNet = [&](size_t ni) {
        const spef::Net& n = f.nets[ni];
        int net = s.findNet(n.name);
        std::vector<std::pair<std::string, int>> pins;  // SPEF node name -> graph node
        for (auto& c : n.conns) {
            int node = nodeOf(c.name);
            if (node < 0) continue;
            pins.push_back({c.name, node});
            if (net < 0) net = s.nodes[node].net;
        }
        if (net < 0) { status[ni] = 2; return; }
        double wire = 0;
        for (auto& c : n.caps) wire += c.c * toPF;
        if (n.caps.empty()) wire = n.total * toPF;
        s.spefCap[net] = wire;
        status[ni] = 1;
        if (n.res.empty()) return;
        // The RC network with local node numbers.
        std::unordered_map<std::string, int> id;
        std::vector<double> cap;
        auto local = [&](const std::string& name) {
            auto it = id.find(name);
            if (it != id.end()) return it->second;
            id.emplace(name, (int)cap.size());
            cap.push_back(0);
            return (int)cap.size() - 1;
        };
        struct R { int a, b; double r; };
        std::vector<R> rs;
        for (auto& r : n.res) rs.push_back({local(r.a), local(r.b), r.r * toOhm});
        for (auto& c : n.caps) {
            // A capacitor at a node of this net; the far end of a coupling
            // capacitor belongs to another net.
            auto it = id.find(c.a);
            if (it != id.end()) cap[it->second] += c.c * toPF;
            else if (!c.b.empty() && (it = id.find(c.b)) != id.end()) cap[it->second] += c.c * toPF;
        }
        int root = -1, rootNode = -1;
        for (auto& [name, node] : pins) {
            auto it = id.find(name);
            if (it == id.end()) continue;
            if (s.nodes[node].driver && root < 0) {
                root = it->second;
                rootNode = node;
            } else if (!s.nodes[node].driver) {
                cap[it->second] += s.nodes[node].cap + (s.isPort(node) ? s.portLoad[node] : 0);
            }
        }
        if (root < 0) { status[ni] = 3; return; }
        // A tree from the driver, breadth first.
        size_t N = cap.size();
        std::vector<std::vector<std::pair<int, double>>> adj(N);
        for (auto& r : rs) {
            adj[r.a].push_back({r.b, r.r});
            adj[r.b].push_back({r.a, r.r});
        }
        std::vector<int> parent(N, -2), order;
        std::vector<double> rUp(N, 0), down(cap), delay(N, 0);
        parent[root] = -1;
        order.push_back(root);
        bool loop = false;
        for (size_t i = 0; i < order.size(); i++) {
            int u = order[i];
            for (auto& [v, r] : adj[u]) {
                if (parent[v] != -2) {
                    if (v != parent[u]) loop = true;
                    continue;
                }
                parent[v] = u;
                rUp[v] = r;
                order.push_back(v);
            }
        }
        for (size_t i = order.size(); i-- > 1;) down[parent[order[i]]] += down[order[i]];
        for (size_t i = 1; i < order.size(); i++) {
            int v = order[i];
            delay[v] = delay[parent[v]] + rUp[v] * down[v] * 1e-3;  // ohm * pF = 1e-3 ns
        }
        if (loop) status[ni] += 4;
        for (auto& [name, node] : pins) {
            if (node == rootNode) continue;
            auto it = id.find(name);
            if (it == id.end() || parent[it->second] == -2) continue;
            for (int k = s.finOff[node]; k < s.finOff[node + 1]; k++) {
                int ei = s.finIdx[k];
                if (s.edges[ei].kind == NetEdge && s.edges[ei].from == rootNode) s.wireDelay[ei] = (float)delay[it->second];
            }
        }
    };
    if (s.pool) s.pool->chunks(f.nets.size(), 256, [&](size_t b, size_t e, size_t) { for (size_t i = b; i < e; i++) doNet(i); });
    else for (size_t i = 0; i < f.nets.size(); i++) doNet(i);
    for (char st : status) {
        if ((st & 3) == 1) res.annotated++;
        else if ((st & 3) == 2) res.unknown++;
        else if ((st & 3) == 3) { res.annotated++; res.noDriver++; }
        if (st & 4) res.loops++;
    }
    s.spefNets += res.annotated;
    s.timed = false;
    return res;
}

}  // namespace sta
