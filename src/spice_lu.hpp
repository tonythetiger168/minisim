// spice_lu.hpp - the sparse linear solver of mspice.
//
// Pattern   the structure of the circuit matrix: every (row, column) a device
//           stamps gets a slot once, at setup, and a load is then
//           `val[slot] += g` with no searching (the classic SPICE pointers).
// SparseLU  LU factors of that matrix, real or complex.
//             analyze():  elimination with threshold pivoting on a
//                         minimum-degree column order; records the pivot rows
//                         and the fill.
//             refactor(): numbers only, on the recorded structure. Newton
//                         iterations and time steps change values, not
//                         structure, so nearly every factorization is this.
//           A refactor that meets a pivot that has become too small reports
//           it and the caller analyzes again.
// Pool      the tool line's shared pool (parallel.hpp). Work is handed out by
//           index and results are stored by index, so a result never depends
//           on how many threads ran.
#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "parallel.hpp"

namespace spice {

struct Pattern {
    int n = 0;
    std::vector<std::pair<int, int>> slots{{-1, -1}};   // slot 0 takes stamps on ground
    std::map<std::pair<int, int>, int> index;
    // By row, once finalized: (slot, column).
    std::vector<int> rowStart;
    std::vector<std::pair<int, int>> rowEnt;
    std::vector<int> order;                              // column elimination order

    int slot(int r, int c) {
        if (r < 0 || c < 0) return 0;
        auto [it, fresh] = index.try_emplace({r, c}, (int)slots.size());
        if (fresh) slots.push_back({r, c});
        return it->second;
    }

    void finalize(int size) {
        n = size;
        rowStart.assign(n + 1, 0);
        for (size_t s = 1; s < slots.size(); s++) rowStart[slots[s].first + 1]++;
        for (int i = 0; i < n; i++) rowStart[i + 1] += rowStart[i];
        rowEnt.resize(slots.size() - 1);
        std::vector<int> fill(rowStart.begin(), rowStart.end() - 1);
        for (size_t s = 1; s < slots.size(); s++) rowEnt[fill[slots[s].first]++] = {(int)s, slots[s].second};
        minDegree();
    }

    // Minimum-degree order on the graph of A + A^T: repeatedly eliminate the
    // node with the fewest neighbours and join its neighbours to each other.
    void minDegree() {
        std::vector<std::set<int>> adj(n);
        for (size_t s = 1; s < slots.size(); s++) {
            auto [r, c] = slots[s];
            if (r != c) { adj[r].insert(c); adj[c].insert(r); }
        }
        order.clear();
        order.reserve(n);
        // A row with a single entry (a voltage source to ground: v = E) fixes
        // its unknown outright. Those columns go first: the pivot makes no
        // fill, and a supply node is out of the way before the rest is
        // ordered. Left for later, the supply column collects the product of
        // the gains of a long chain of stages and its own equation, a plain
        // 1, is no longer an acceptable pivot.
        std::vector<char> first(n, 0);
        for (int r = 0; r < n; r++)
            if (rowStart[r + 1] - rowStart[r] == 1) {
                int c = rowEnt[rowStart[r]].second;
                if (first[c]) continue;
                first[c] = 1;
                order.push_back(c);
                for (int u : adj[c]) adj[u].erase(c);
                adj[c].clear();
            }
        std::set<std::pair<int, int>> heap;
        for (int i = 0; i < n; i++)
            if (!first[i]) heap.insert({(int)adj[i].size(), i});
        while (!heap.empty()) {
            int v = heap.begin()->second;
            heap.erase(heap.begin());
            order.push_back(v);
            std::vector<int> nb(adj[v].begin(), adj[v].end());
            for (int u : nb) {
                heap.erase({(int)adj[u].size(), u});
                adj[u].erase(v);
            }
            for (size_t a = 0; a < nb.size(); a++)
                for (size_t b = a + 1; b < nb.size(); b++) {
                    adj[nb[a]].insert(nb[b]);
                    adj[nb[b]].insert(nb[a]);
                }
            for (int u : nb) heap.insert({(int)adj[u].size(), u});
            adj[v].clear();
        }
    }
};

struct SingularMatrix : std::runtime_error {
    int column;   // the unknown that has no equation left to fix it
    explicit SingularMatrix(int c) : std::runtime_error("singular matrix"), column(c) {}
};

template <class T>
struct SparseLU {
    const Pattern* pat = nullptr;
    int n = 0;
    bool symbolic = false;
    double pivtol = 1e-13;
    std::vector<int> prow;                 // pivot position -> original row
    std::vector<int> colOf;                // pivot position -> original column
    // Row i of L (strictly lower, ascending columns) and of U (diagonal first).
    std::vector<int> lp, li, up, ui;
    std::vector<T> lx, ux, dinv, w, y;
    // Row i of A in pivot coordinates: (slot, column position).
    std::vector<int> ap;
    std::vector<std::pair<int, int>> ae;

    void init(const Pattern& p) { pat = &p; n = p.n; symbolic = false; }

    // Copy another factorization's structure (same Pattern). Used to give
    // every AC worker the same pivot order.
    template <class U>
    void adopt(const SparseLU<U>& o) {
        pat = o.pat; n = o.n; symbolic = o.symbolic; pivtol = o.pivtol;
        prow = o.prow; colOf = o.colOf; lp = o.lp; li = o.li; up = o.up; ui = o.ui; ap = o.ap; ae = o.ae;
        lx.assign(li.size(), T{}); ux.assign(ui.size(), T{}); dinv.assign(n, T{}); w.assign(n, T{}); y.assign(n, T{});
    }

    void analyze(const T* val) {
        const Pattern& P = *pat;
        std::vector<int> pos(n);
        for (int k = 0; k < n; k++) pos[P.order[k]] = k;
        using Row = std::vector<std::pair<int, T>>;
        std::vector<Row> rows(n);
        std::vector<int> head(n, 0);
        std::vector<std::vector<int>> colRows(n), lpat(n);
        for (int r = 0; r < n; r++) {
            Row& row = rows[r];
            for (int e = P.rowStart[r]; e < P.rowStart[r + 1]; e++)
                row.push_back({pos[P.rowEnt[e].second], val[P.rowEnt[e].first]});
            std::sort(row.begin(), row.end(), [](auto& a, auto& b) { return a.first < b.first; });
            for (auto& [c, v] : row) colRows[c].push_back(r);
        }
        std::vector<char> done(n, 0);
        prow.assign(n, -1);
        Row merged;
        for (int k = 0; k < n; k++) {
            double big = 0;
            for (int r : colRows[k])
                if (!done[r]) big = std::max(big, std::abs(rows[r][head[r]].second));
            if (!(big > pivtol)) throw SingularMatrix(P.order[k]);
            // Among the rows whose entry is large enough, take the shortest:
            // it makes the least fill. Ties go to the diagonal, then to size.
            int p = -1;
            size_t plen = 0;
            double pabs = 0;
            for (int r : colRows[k]) {
                if (done[r]) continue;
                double a = std::abs(rows[r][head[r]].second);
                if (a < 1e-3 * big) continue;
                size_t len = rows[r].size() - head[r];
                bool better = p < 0 || len < plen ||
                              (len == plen && ((r == P.order[k]) != (p == P.order[k]) ? r == P.order[k]
                                               : a > pabs || (a == pabs && r < p)));
                if (better) { p = r; plen = len; pabs = a; }
            }
            if (p < 0) throw SingularMatrix(P.order[k]);
            done[p] = 1;
            prow[k] = p;
            const Row& pr = rows[p];
            T piv = pr[head[p]].second;
            for (int r : colRows[k]) {
                if (done[r]) continue;
                Row& row = rows[r];
                T f = row[head[r]].second / piv;
                lpat[r].push_back(k);
                merged.clear();
                size_t a = head[r] + 1, b = head[p] + 1;
                while (a < row.size() || b < pr.size()) {
                    if (b == pr.size() || (a < row.size() && row[a].first < pr[b].first)) merged.push_back(row[a++]);
                    else if (a == row.size() || pr[b].first < row[a].first) {
                        merged.push_back({pr[b].first, -f * pr[b].second});
                        colRows[pr[b].first].push_back(r);
                        b++;
                    } else {
                        merged.push_back({row[a].first, row[a].second - f * pr[b].second});
                        a++; b++;
                    }
                }
                row.swap(merged);
                head[r] = 0;
            }
        }
        colOf = P.order;
        lp.assign(n + 1, 0); up.assign(n + 1, 0); ap.assign(n + 1, 0);
        li.clear(); ui.clear(); ae.clear();
        for (int i = 0; i < n; i++) {
            int r = prow[i];
            for (int k : lpat[r]) li.push_back(k);
            for (size_t e = head[r]; e < rows[r].size(); e++) ui.push_back(rows[r][e].first);
            for (int e = P.rowStart[r]; e < P.rowStart[r + 1]; e++)
                ae.push_back({P.rowEnt[e].first, pos[P.rowEnt[e].second]});
            lp[i + 1] = (int)li.size(); up[i + 1] = (int)ui.size(); ap[i + 1] = (int)ae.size();
        }
        lx.assign(li.size(), T{}); ux.assign(ui.size(), T{}); dinv.assign(n, T{});
        w.assign(n, T{}); y.assign(n, T{});
        symbolic = true;
    }

    // Numbers on the known structure. False if a pivot is too small to trust.
    bool refactor(const T* val) {
        for (int i = 0; i < n; i++) {
            for (int e = lp[i]; e < lp[i + 1]; e++) w[li[e]] = T{};
            for (int e = up[i]; e < up[i + 1]; e++) w[ui[e]] = T{};
            double amax = 0;
            for (int e = ap[i]; e < ap[i + 1]; e++) {
                T v = val[ae[e].first];
                w[ae[e].second] = v;
                amax = std::max(amax, std::abs(v));
            }
            for (int e = lp[i]; e < lp[i + 1]; e++) {
                int k = li[e];
                T l = w[k] * dinv[k];
                lx[e] = l;
                if (l == T{}) continue;
                for (int u = up[k] + 1; u < up[k + 1]; u++) w[ui[u]] -= l * ux[u];
            }
            for (int e = up[i]; e < up[i + 1]; e++) ux[e] = w[ui[e]];
            double d = std::abs(ux[up[i]]);
            if (!(d > pivtol) || d < 1e-10 * amax) return false;
            dinv[i] = T(1.0) / ux[up[i]];
        }
        return true;
    }

    void factor(const T* val) {
        if (symbolic && refactor(val)) return;
        analyze(val);
        // The fresh pivot order is the best there is: accept its pivots
        // unless one is exactly zero.
        if (!refactor(val)) refactorNoCheck(val);
    }

    void refactorNoCheck(const T* val) {
        for (int i = 0; i < n; i++) {
            for (int e = lp[i]; e < lp[i + 1]; e++) w[li[e]] = T{};
            for (int e = up[i]; e < up[i + 1]; e++) w[ui[e]] = T{};
            for (int e = ap[i]; e < ap[i + 1]; e++) w[ae[e].second] = val[ae[e].first];
            for (int e = lp[i]; e < lp[i + 1]; e++) {
                int k = li[e];
                T l = w[k] * dinv[k];
                lx[e] = l;
                for (int u = up[k] + 1; u < up[k + 1]; u++) w[ui[u]] -= l * ux[u];
            }
            for (int e = up[i]; e < up[i + 1]; e++) ux[e] = w[ui[e]];
            if (!(std::abs(ux[up[i]]) > 0)) throw SingularMatrix(colOf[i]);
            dinv[i] = T(1.0) / ux[up[i]];
        }
    }

    // b: right-hand side by row. x: solution by unknown. They may not alias.
    void solve(const T* b, T* x) {
        for (int i = 0; i < n; i++) {
            T s = b[prow[i]];
            for (int e = lp[i]; e < lp[i + 1]; e++) s -= lx[e] * y[li[e]];
            y[i] = s;
        }
        for (int i = n - 1; i >= 0; i--) {
            T s = y[i];
            for (int e = up[i] + 1; e < up[i + 1]; e++) s -= ux[e] * y[ui[e]];
            y[i] = s * dinv[i];
        }
        for (int i = 0; i < n; i++) x[colOf[i]] = y[i];
    }

    // The transposed system (plain transpose, also for complex numbers):
    // b by unknown, x by row. This is the adjoint circuit, which gives the
    // transfer from every place in the circuit to one output in one solve.
    void solveT(const T* b, T* x) {
        for (int i = 0; i < n; i++) w[i] = b[colOf[i]];
        for (int i = 0; i < n; i++) {
            T t = w[i] * dinv[i];
            y[i] = t;
            for (int e = up[i] + 1; e < up[i + 1]; e++) w[ui[e]] -= ux[e] * t;
        }
        for (int i = n - 1; i >= 0; i--) {
            T t = y[i];
            for (int e = lp[i]; e < lp[i + 1]; e++) y[li[e]] -= lx[e] * t;
            x[prow[i]] = t;
        }
    }
};

// The threads are the tool line's shared pool (parallel.hpp).
using Pool = par::Pool;

}  // namespace spice
