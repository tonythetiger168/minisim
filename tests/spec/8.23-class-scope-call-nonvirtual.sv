// IEEE 1800-2023 8.23: a method named with the class scope operator,
// Cls::m(), runs that class's body even when the method is virtual and
// the object's class overrides it; it is not dispatched virtually.
//! icarus-skip: Icarus 13 cannot parse Class::method() calls
class Animal;
  virtual function string sound(); return "..."; endfunction
  virtual task speak(output string s); s = "animal"; endtask
  function string own(); return Animal::sound(); endfunction
endclass

class Dog extends Animal;
  virtual function string sound(); return "woof"; endfunction
endclass

class Puppy extends Dog;
  virtual function string sound(); return "yip"; endfunction
  virtual task speak(output string s); s = "puppy"; endtask
  function string base_sound(); return Animal::sound(); endfunction
  function string dog_sound(); return Dog::sound(); endfunction
  task base_speak(output string s); Animal::speak(s); endtask
endclass

class Box #(type T = int) extends Animal;
endclass

class Cat extends Box #(byte);
  virtual function string sound(); return "meow"; endfunction
  function string base_sound(); return Animal::sound(); endfunction
endclass

module top;
  initial begin
    automatic Puppy p = new;
    automatic Cat c = new;
    automatic Animal a = p;
    string s;
    $display("%s %s %s", p.sound(), a.sound(), a.own());
    $display("%s %s", p.base_sound(), p.dog_sound());
    p.base_speak(s);
    $display("%s", s);
    a.speak(s);
    $display("%s", s);
    $display("%s %s %s", c.sound(), c.base_sound(), c.own());
  end
endmodule
//> yip yip ...
//> ... woof
//> animal
//> puppy
//> meow ... ...
