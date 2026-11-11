// mpnr clock tree synthesis: every clock net with more loads than one buffer
// may drive gets a tree of buffers. The loads are clustered by position into
// groups of at most `fanout`; each group gets a buffer at its centre; the
// buffers are clustered the same way, level by level, down to one root buffer
// at the centre of the loads, which the net's own driver drives. Every load
// ends up behind the same number of buffers, and the groups of one level
// are of near-equal size. The buffers are added after global placement and legalized with
// the other cells.
#pragma once
#include "pnr_db.hpp"

namespace pnr {

struct CtsOptions {
  bool on = true;
  int fanout = 16;                    // most loads on one buffer (or on the clock's driver)
  std::string buf;                    // buffer cell; empty: found in the LEF by name
  std::vector<std::string> clocks;    // clock nets by name; empty: found from the cells' clock pins
};

struct CtsTree {
  std::string net;
  int sinks = 0, buffers = 0, levels = 0;
  int64_t minLen = 0, maxLen = 0;     // driver to load along the tree, Manhattan, dbu
};

class Cts {
public:
  Cts(DB& d, const CtsOptions& o) : db(d), opt(o) {}

  // Finds the clock nets and the buffer cell. Returns the area the buffers
  // will take (dbu^2), for the floorplan.
  double plan() {
    if (!opt.on) return 0;
    if (opt.fanout < 2) throw std::runtime_error("-cts_fanout must be 2 or more");
    findClocks();
    if (clocks.empty()) return 0;
    pickBuffer();
    double n = 0;
    for (int net : clocks) n += count((int)loads(net).size());
    return n * (double)buf->w * buf->h;
  }

  // Builds the trees. Cells must have their global placement.
  void build() {
    for (int net : clocks) buildTree(net);
  }

  // The trees with their lengths at the cells' present places.
  std::vector<CtsTree> report() const {
    std::vector<CtsTree> out;
    for (auto& t : trees) {
      CtsTree r = t;
      r.minLen = INT64_MAX; r.maxLen = 0;
      walk(rootOf.at(t.net), 0, r);
      if (r.minLen == INT64_MAX) r.minLen = 0;
      out.push_back(r);
    }
    return out;
  }

  const phys::Macro* bufferCell() const { return buf; }
  int buffers() const { int n = 0; for (auto& t : trees) n += t.buffers; return n; }

private:
  DB& db;
  CtsOptions opt;
  const phys::Macro* buf = nullptr;
  int bufIn = -1, bufOut = -1;
  std::vector<int> clocks;                       // net ids
  std::vector<CtsTree> trees;
  std::map<std::string, int> rootOf;             // tree -> its net
  std::unordered_map<int, int> bufOutNet;        // buffer instance -> the net it drives
  int serial = 0;

  static bool clockPin(const phys::MacroPin& p) {
    if (p.use == "CLOCK") return true;
    static const char* names[] = {"CK", "CLK", "CP", "CKN", "CLKN", "GCK"};
    for (auto n : names) if (p.name == n) return true;
    return false;
  }
  bool drives(int pid) const {
    const Pin& p = db.pins[pid];
    if (p.inst < 0) return db.ios[p.idx].dir == "INPUT";
    return db.insts[p.inst].m->pins[p.idx].dir == "OUTPUT";
  }
  std::vector<int> loads(int net) const {
    std::vector<int> l;
    for (int p : db.nets[net].pins) if (!drives(p)) l.push_back(p);
    return l;
  }
  // Buffers a tree over n loads needs.
  int count(int n) const {
    int c = 0;
    if (n <= opt.fanout) return 0;
    do { n = (n + opt.fanout - 1) / opt.fanout; c += n; } while (n > 1);
    return c;
  }

  void findClocks() {
    clocks.clear();
    if (!opt.clocks.empty()) {
      for (auto& name : opt.clocks) {
        int id = -1;
        for (size_t i = 0; i < db.nets.size(); i++) if (db.nets[i].name == name) id = (int)i;
        if (id < 0) throw std::runtime_error("-clock: no net '" + name + "'");
        if ((int)loads(id).size() > opt.fanout) clocks.push_back(id);
      }
      return;
    }
    for (size_t i = 0; i < db.nets.size(); i++) {
      bool clk = false;
      for (int p : db.nets[i].pins) {
        const Pin& pn = db.pins[p];
        if (pn.inst >= 0 && clockPin(db.insts[pn.inst].m->pins[pn.idx])) { clk = true; break; }
      }
      if (clk && (int)loads((int)i).size() > opt.fanout) clocks.push_back((int)i);
    }
  }

  // A buffer: one input and one output signal pin. By name a clock buffer
  // is preferred to a plain one, and the widest (strongest) of its kind.
  void pickBuffer() {
    auto shape = [&](const phys::Macro& m, int& in, int& out) {
      in = out = -1;
      int n = 0;
      for (size_t i = 0; i < m.pins.size(); i++) {
        const phys::MacroPin& p = m.pins[i];
        if (p.use == "POWER" || p.use == "GROUND") continue;
        n++;
        if (p.dir == "INPUT") in = (int)i; else if (p.dir == "OUTPUT") out = (int)i;
      }
      return n == 2 && in >= 0 && out >= 0;
    };
    if (!opt.buf.empty()) {
      buf = db.tech.macro(opt.buf);
      if (!buf) throw std::runtime_error("-cts_buf: the LEF has no cell '" + opt.buf + "'");
      if (!shape(*buf, bufIn, bufOut)) throw std::runtime_error("-cts_buf: " + opt.buf + " is not a buffer (one input, one output)");
      return;
    }
    int bestRank = 0;
    for (auto& [name, m] : db.tech.macros) {
      int in, out;
      if (!m.isCore() || !shape(m, in, out)) continue;
      std::string u = name;
      for (auto& c : u) c = (char)toupper((unsigned char)c);
      if (u.find("TBUF") != std::string::npos || u.find("INV") != std::string::npos) continue;
      int rank = u.find("CLKBUF") != std::string::npos || u.find("CKBUF") != std::string::npos ? 2 : u.find("BUF") != std::string::npos ? 1 : 0;
      if (!rank) continue;
      if (rank > bestRank || (rank == bestRank && (m.w > buf->w || (m.w == buf->w && name < buf->name)))) { bestRank = rank; buf = &m; bufIn = in; bufOut = out; }
    }
    if (!buf) throw std::runtime_error("clock tree: the LEF has no buffer cell; name one with -cts_buf CELL, or use -no_cts");
  }

  struct Item { int pin; int64_t x, y; };

  // Splits items[lo, hi) into k groups of near-equal size, cutting the longer
  // side of the bounding box each time.
  void split(std::vector<Item>& items, size_t lo, size_t hi, int k, std::vector<std::pair<size_t, size_t>>& groups) const {
    if (k <= 1) { groups.push_back({lo, hi}); return; }
    int64_t x0 = INT64_MAX, x1 = INT64_MIN, y0 = INT64_MAX, y1 = INT64_MIN;
    for (size_t i = lo; i < hi; i++) { x0 = std::min(x0, items[i].x); x1 = std::max(x1, items[i].x); y0 = std::min(y0, items[i].y); y1 = std::max(y1, items[i].y); }
    bool byX = x1 - x0 >= y1 - y0;
    std::sort(items.begin() + lo, items.begin() + hi, [&](const Item& a, const Item& b) {
      int64_t ka = byX ? a.x : a.y, kb = byX ? b.x : b.y;
      return ka != kb ? ka < kb : a.pin < b.pin;
    });
    int kl = k / 2;
    size_t mid = lo + (hi - lo) * (size_t)kl / (size_t)k;
    split(items, lo, mid, kl, groups);
    split(items, mid, hi, k - kl, groups);
  }

  void buildTree(int net) {
    std::vector<int> ld = loads(net);
    CtsTree t;
    t.net = db.nets[net].name;
    t.sinks = (int)ld.size();
    std::vector<Item> items;
    for (int p : ld) { Item it; it.pin = p; db.pinPos(p, it.x, it.y); items.push_back(it); }
    // The net keeps its drivers; the loads move behind buffers.
    std::vector<int> keep;
    for (int p : db.nets[net].pins) if (drives(p)) keep.push_back(p);
    while (items.size() > 1) {
      int k = ((int)items.size() + opt.fanout - 1) / opt.fanout;
      std::vector<std::pair<size_t, size_t>> groups;
      split(items, 0, items.size(), k, groups);
      std::vector<Item> next;
      for (auto& g : groups) {
        if (g.first == g.second) continue;
        int64_t sx = 0, sy = 0;
        for (size_t i = g.first; i < g.second; i++) { sx += items[i].x; sy += items[i].y; }
        int64_t n = (int64_t)(g.second - g.first), cx = sx / n, cy = sy / n;
        int id = serial++;
        int newNet = (int)db.nets.size();
        db.nets.push_back({});
        db.nets[newNet].name = "mpnr_cts_n" + std::to_string(id);
        for (size_t i = g.first; i < g.second; i++) { db.pins[items[i].pin].net = newNet; db.nets[newNet].pins.push_back(items[i].pin); }
        Inst b;
        b.name = "mpnr_cts_b" + std::to_string(id);
        b.m = buf;
        b.x = std::min(std::max(cx - buf->w / 2, db.core.x0), db.core.x1 - buf->w);
        b.y = std::min(std::max(cy - buf->h / 2, db.core.y0), db.core.y1 - buf->h);
        int inst = (int)db.insts.size();
        int pin = (int)db.pins.size();
        db.pins.push_back({inst, bufIn, -1});
        db.pins.push_back({inst, bufOut, newNet});
        db.nets[newNet].pins.push_back(pin + 1);
        b.pins = {pin, pin + 1};
        db.insts.push_back(b);
        bufOutNet[inst] = newNet;
        Item it;
        it.pin = pin;
        it.x = cx; it.y = cy;
        next.push_back(it);
        t.buffers++;
      }
      items.swap(next);
      t.levels++;
    }
    for (auto& it : items) { db.pins[it.pin].net = net; keep.push_back(it.pin); }
    db.nets[net].pins = keep;
    rootOf[t.net] = net;
    trees.push_back(t);
  }

  void walk(int net, int64_t len, CtsTree& r) const {
    int64_t dx = 0, dy = 0;
    bool have = false;
    for (int p : db.nets[net].pins) if (drives(p)) { db.pinPos(p, dx, dy); have = true; break; }
    for (int p : db.nets[net].pins) {
      if (drives(p)) continue;
      int64_t x, y;
      db.pinPos(p, x, y);
      int64_t l = len + (have ? std::llabs(x - dx) + std::llabs(y - dy) : 0);
      auto it = db.pins[p].inst >= 0 ? bufOutNet.find(db.pins[p].inst) : bufOutNet.end();
      if (it != bufOutNet.end()) walk(it->second, l, r);
      else { r.minLen = std::min(r.minLen, l); r.maxLen = std::max(r.maxLen, l); }
    }
  }
};

}  // namespace pnr
