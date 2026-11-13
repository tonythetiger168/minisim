// IEEE 1800.2-2020 rows with no test of their own until now: uvm_time
// (5.6), uvm_heartbeat (10.6), uvm_sequence_library (14.4), the report
// macros (B.1), uvm_callback_iter (D.1), uvm_component::print_config and
// friends (D.2), package scope functions (F.3), uvm_run_test_callback
// (F.6) and uvm_root find / find_all / set_timeout (F.7).
//! args: +UVM_NO_RELNOTES
`include "uvm_macros.svh"
`timescale 1ns/1ps
module top;
  import uvm_pkg::*;
  int fails = 0;
  function automatic bit near(real a, real b); return a > b - 1e-6 && a < b + 1e-6; endfunction
  `define CHK(id, c) if (c) $display("ok %s", id); else begin $display("FAIL %s", id); fails++; end

  class item extends uvm_sequence_item;
    `uvm_object_utils(item)
    rand int d;
    function new(string name = "item"); super.new(name); endfunction
  endclass
  int ran[string];
  class seq_a extends uvm_sequence #(item);
    `uvm_object_utils(seq_a)
    function new(string name = "seq_a"); super.new(name); endfunction
    task body(); ran["a"]++; endtask
  endclass
  class seq_b extends uvm_sequence #(item);
    `uvm_object_utils(seq_b)
    function new(string name = "seq_b"); super.new(name); endfunction
    task body(); ran["b"]++; endtask
  endclass
  class lib extends uvm_sequence_library #(item);
    `uvm_object_utils(lib)
    `uvm_sequence_library_utils(lib)
    function new(string name = "lib"); super.new(name); init_sequence_library(); endfunction
  endclass

  // D.1: callbacks seen through uvm_callback_iter
  virtual class my_cb extends uvm_callback;
    function new(string name = "my_cb"); super.new(name); endfunction
    pure virtual function int tag();
  endclass
  class cb_n extends my_cb;
    int n;
    function new(string name = "cb", int n = 0); super.new(name); this.n = n; endfunction
    virtual function int tag(); return n; endfunction
  endclass
  class leaf extends uvm_component;
    `uvm_component_utils(leaf)
    `uvm_register_cb(leaf, my_cb)
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
  endclass

  // F.6
  class rt_cb extends uvm_run_test_callback;
    int pre, post;
    function new(string name = "rt_cb"); super.new(name); endfunction
    virtual function void pre_run_test(); pre++; endfunction
    virtual function void post_run_test(); post++; $display("post_run_test pre=%0d", pre); endfunction
  endclass
  rt_cb rtc;

  // demote the heartbeat's fatal to a count
  class hb_catch extends uvm_report_catcher;
    int n;
    function new(string name = "hb_catch"); super.new(name); endfunction
    virtual function action_e catch();
      if (get_id() == "HBFAIL") begin n++; return CAUGHT; end
      return THROW;
    endfunction
  endclass

  class test extends uvm_test;
    `uvm_component_utils(test)
    leaf l0, l1;
    uvm_sequencer #(item) sqr;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
      l0 = leaf::type_id::create("l0", this);
      l1 = leaf::type_id::create("l1", this);
      sqr = new("sqr", this);
      uvm_config_db #(int)::set(this, "l0", "knob", 3);
    endfunction
    task run_phase(uvm_phase phase);
      phase.raise_objection(this);
      `CHK("F.6 pre_run_test", rtc.pre == 1 && rtc.post == 0)
      begin   // 5.6
        uvm_time t = new("t", 1e-9), u = new("u", 1e-12);
        t.set_abstime(4, 1e-9);
        `CHK("5.6 get_abstime", t.get_abstime(1e-9) == 4 && near(t.get_abstime(1e-12), 4000))
        u.set_abstime(2500, 1e-12);
        `CHK("5.6 get_realtime", near(u.get_realtime(1ns, 1e-9), 2.5))
        t.incr(6, 1ns, 1e-9);
        `CHK("5.6 incr", t.get_abstime(1e-9) == 10)
        t.decr(3, 1ns, 1e-9);
        `CHK("5.6 decr", t.get_abstime(1e-9) == 7 && t.get_name() == "t")
        t.reset();
        `CHK("5.6 reset", t.get_abstime(1e-9) == 0)
      end
      begin   // 10.6
        uvm_objection ob = new("ob");
        uvm_heartbeat hb = new("hb", this, ob);
        uvm_event#() ev = new("ev");
        hb_catch hc = new;
        uvm_report_cb::add(null, hc);
        hb.set_mode(UVM_ANY_ACTIVE);
        hb.add(l0);
        hb.add(l1);
        hb.start(ev);
        fork
          repeat (6) begin #10; ev.trigger(); end
          begin
            repeat (3) begin #10; ob.raise_objection(l0); ob.drop_objection(l0); end   // alive for three beats
          end
        join
        `CHK("10.6 heartbeat missed", hc.n >= 1 && hc.n <= 3)
        hb.stop();
        hc.n = 0;
        #10; ev.trigger(); #1;
        `CHK("10.6 stop", hc.n == 0)
        uvm_report_cb::delete(null, hc);
      end
      begin   // 14.4
        lib sl = lib::type_id::create("sl");
        sl.add_sequence(seq_a::get_type());
        sl.add_sequence(seq_b::get_type());
        sl.selection_mode = UVM_SEQ_LIB_RANDC;
        sl.min_random_count = 6;
        sl.max_random_count = 6;
        if (!sl.randomize()) `uvm_error("L", "library randomize")
        sl.start(sqr);
        `CHK("14.4 library ran", ran["a"] + ran["b"] == 6 && ran["a"] == 3 && ran["b"] == 3)
        begin uvm_object_wrapper ws[$]; sl.get_sequences(ws); `CHK("14.4 get_sequences", ws.size() == 2) end
        sl.remove_sequence(seq_b::get_type());
        ran.delete();
        sl.selection_mode = UVM_SEQ_LIB_RAND;
        sl.min_random_count = 4;
        sl.max_random_count = 4;
        if (!sl.randomize()) `uvm_error("L", "library randomize")
        sl.start(sqr);
        `CHK("14.4 remove_sequence", ran["a"] == 4 && !ran.exists("b"))
      end
      begin   // B.1
        uvm_report_server rs = uvm_report_server::get_server();
        int w0 = rs.get_severity_count(UVM_WARNING), e0 = rs.get_severity_count(UVM_ERROR), i0 = rs.get_severity_count(UVM_INFO);
        uvm_report_message m;
        set_report_severity_action(UVM_ERROR, UVM_DISPLAY | UVM_COUNT);
        `uvm_info("B1", "shown at low", UVM_LOW)
        `uvm_info("B1", "hidden at debug", UVM_DEBUG)
        `uvm_warning("B1", "a warning")
        `uvm_error("B1", "an error")
        `uvm_info_context("B1", "from l0", UVM_NONE, l0)
        `uvm_warning_context("B1", "warn from l1", l1)
        `uvm_info_begin("B1", "with elements", UVM_NONE, m)
          `uvm_message_add_int(w0, UVM_DEC)
          `uvm_message_add_string("abc", "s")
          `uvm_message_add_tag("color", "red")
        `uvm_info_end
        `CHK("B.1 counts", rs.get_severity_count(UVM_WARNING) == w0 + 2 && rs.get_severity_count(UVM_ERROR) == e0 + 1 &&
                           rs.get_severity_count(UVM_INFO) == i0 + 3)
        `CHK("B.1 id count", rs.get_id_count("B1") == 6)
        rs.set_severity_count(UVM_ERROR, e0);
      end
      begin   // D.1
        cb_n c1 = new("c1", 1), c2 = new("c2", 2), c3 = new("c3", 3);
        uvm_callback_iter #(leaf, my_cb) it = new(l0);
        my_cb c; int sum, n; string order;
        uvm_callbacks #(leaf, my_cb)::add(l0, c1);
        uvm_callbacks #(leaf, my_cb)::add(l0, c2);
        uvm_callbacks #(leaf, my_cb)::add(l0, c3, UVM_PREPEND);
        c2.callback_mode(0);   // a disabled callback is skipped
        for (c = it.first(); c != null; c = it.next()) begin n++; sum += c.tag(); order = {order, c.get_name()}; `CHK("D.1 get_cb", it.get_cb() == c) end
        `CHK("D.1 first/next", n == 2 && sum == 4 && order == "c3c1")
        order = "";
        for (c = it.last(); c != null; c = it.prev()) order = {order, c.get_name()};
        `CHK("D.1 last/prev", order == "c1c3")
        begin uvm_callback_iter #(leaf, my_cb) it1 = new(l1); `CHK("D.1 none", it1.first() == null) end
      end
      begin   // D.2
        int k;
        `CHK("D.2 config", uvm_config_db #(int)::get(l0, "", "knob", k) && k == 3)
        l0.print_config(1, 1);
        l0.print_config_with_audit(0);
        uvm_component::print_config_matches = 1;
        `CHK("D.2 print_config_matches", uvm_component::print_config_matches == 1)
        uvm_component::print_config_matches = 0;
      end
      begin   // F.3
        uvm_bitstream_t b = uvm_string_to_bits("AZ");
        string parts[$];
        uvm_enum_wrapper #(uvm_verbosity)::from_name("UVM_HIGH", begin_v);
        `CHK("F.3 string_to_bits", b == 16'h415a && uvm_bits_to_string(b) == "AZ")
        `CHK("F.3 is_match", uvm_is_match("a*c", "abbc") && !uvm_is_match("a?c", "abbc"))
        `CHK("F.3 enum_wrapper", begin_v == UVM_HIGH)
        uvm_split_string("a,b,,c", ",", parts);
        `CHK("F.3 split_string", parts.size() == 3 && parts[1] == "b" && parts[2] == "c")
        `CHK("F.3 report_enabled", uvm_report_enabled(UVM_LOW, UVM_INFO, "x") && !uvm_report_enabled(UVM_DEBUG, UVM_INFO, "x"))
        `CHK("F.3 core_state", get_core_state() == UVM_CORE_RUNNING)
        begin time t0 = $time; uvm_wait_for_nba_region(); `CHK("F.3 wait_for_nba_region", $time == t0) end
        uvm_report_info("F3", "package scope report", UVM_NONE);
      end
      begin   // F.7
        uvm_root r = uvm_root::get(); uvm_component cs[$]; uvm_component c;
        r.find_all("uvm_test_top.l?", cs);
        `CHK("F.7 find_all", cs.size() == 2)
        cs.delete();
        r.find_all("*", cs, this);
        `CHK("F.7 find_all under", cs.size() >= 3)
        c = r.find("*.l1");
        `CHK("F.7 find", c == l1 && r.get_child("uvm_test_top") == this)
        r.set_timeout(1ms, 1);
        `CHK("F.7 root", r.get_full_name() == "" && r.get_type_name() == "uvm_root" && uvm_top == r)
        r.enable_print_topology = 0;
        r.print_topology();
      end
      phase.drop_objection(this);
    endtask
    uvm_verbosity begin_v;
    function void report_phase(uvm_phase phase);
      if (fails == 0) $display("ALL OK"); else $display("%0d FAILED", fails);
    endfunction
  endclass
  initial begin
    rtc = new;
    uvm_run_test_callback::add(rtc);
    run_test("test");
  end
endmodule
//> ok F.6 pre_run_test
//> ok 5.6 get_abstime
//> ok 5.6 get_realtime
//> ok 5.6 incr
//> ok 5.6 decr
//> ok 5.6 reset
//> ok 10.6 heartbeat missed
//> ok 10.6 stop
//> ok 14.4 library ran
//> ok 14.4 get_sequences
//> ok 14.4 remove_sequence
//> ok B.1 counts
//> ok B.1 id count
//> ok D.1 get_cb
//> ok D.1 get_cb
//> ok D.1 first/next
//> ok D.1 last/prev
//> ok D.1 none
//> ok D.2 config
//> ok D.2 print_config_matches
//> ok F.3 string_to_bits
//> ok F.3 is_match
//> ok F.3 enum_wrapper
//> ok F.3 split_string
//> ok F.3 report_enabled
//> ok F.3 core_state
//> ok F.3 wait_for_nba_region
//> ok F.7 find_all
//> ok F.7 find_all under
//> ok F.7 find
//> ok F.7 root
//> ALL OK
