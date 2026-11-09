// IEEE 1800-2023 13.5.3, 8.20: a default argument that calls a method of the
// callee's class runs on the object the method is called on, not on the
// caller's `this`.
//! icarus-skip: Icarus 13 does not support virtual methods with default arguments
module top;
  class Event;
    string name;
    function new(string n); name = n; endfunction
    virtual function string default_data(); return {name, "-data"}; endfunction
    virtual function void trigger(string data = default_data());
      $display("%s triggered with %s", name, data);
    endfunction
  endclass
  class Item;
    Event begin_event;
    string name = "item";
    function new(); begin_event = new("begin"); endfunction
    virtual function string default_data(); return "wrong"; endfunction
    function void start(); begin_event.trigger(); endfunction
  endclass
  Item it;
  initial begin
    it = new;
    it.start();
    it.begin_event.trigger("given");
  end
endmodule

//> begin triggered with begin-data
//> begin triggered with given
