// IEEE 1800-2023 7.2, 8.4: unpacked struct properties and locals in a class,
// with string and class-handle members; ?: between structs (11.4.11), and
// find with on a queue of structs (7.12.1).
//! icarus-skip: Icarus 13 does not support unpacked structs with class members
module top;
  class W; string name; function new(string n); name = n; endfunction endclass
  typedef struct { W m_type; string m_type_name; } pair_t;
  typedef struct { string alias_name; pair_t orig; } alias_t;
  class F;
    pair_t p, q;
    alias_t aliases[$];
    function string pick(bit first);
      pair_t r = first ? p : q;
      return r.m_type == null ? r.m_type_name : r.m_type.name;
    endfunction
    function int count(string a);
      alias_t found[$];
      found = aliases.find(i) with (i.alias_name == a);
      return found.size();
    endfunction
  endclass
  F f;
  initial begin
    f = new;
    f.p.m_type = new("wrapped");
    f.q.m_type_name = "by-name";
    $display("%s %s", f.pick(1), f.pick(0));
    f.aliases.push_back('{"x", f.p});
    f.aliases.push_back('{"y", f.q});
    f.aliases.push_back('{"x", f.q});
    $display("%0d %0d %0d", f.count("x"), f.count("y"), f.count("z"));
  end
endmodule

//> wrapped by-name
//> 2 1 0
