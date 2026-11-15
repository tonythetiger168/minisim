-- std.reflection (IEEE 1076-2019 16.12), msim's implementation.
-- The mirrors are protected types; 'REFLECT builds them through the msim_*
-- constructors below, which the translator calls from generated functions.
use std.textio.all;
package reflection is
  type index is range integer'low to integer'high;
  subtype natural_index is index range 0 to index'high;
  subtype positive_index is index range 1 to index'high;
  subtype dimension is index range 1 to index'high;
  type index_vector is array (dimension range <>) of index;

  type value_mirror;
  type subtype_mirror;

  type enumeration_subtype_mirror;
  type enumeration_value_mirror_pt is protected
    impure function get_subtype_mirror return enumeration_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function pos return integer;
    impure function image return string;
    procedure msim_init(s : enumeration_subtype_mirror; p : integer; w : value_mirror);
  end protected;
  type enumeration_value_mirror is access enumeration_value_mirror_pt;
  type enumeration_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function enumeration_literal(literal_idx : natural_index) return enumeration_value_mirror;
    impure function enumeration_literal(literal_name : string) return enumeration_value_mirror;
    impure function simple_name return string;
    impure function left return enumeration_value_mirror;
    impure function right return enumeration_value_mirror;
    impure function low return enumeration_value_mirror;
    impure function high return enumeration_value_mirror;
    impure function length return positive_index;
    impure function ascending return boolean;
    procedure msim_init(me : enumeration_subtype_mirror; w : subtype_mirror; n : string; lits : string; lp, rp : integer; asc : boolean);
    impure function msim_literal(p : integer) return string;
  end protected;
  type enumeration_subtype_mirror is access enumeration_subtype_mirror_pt;

  type integer_subtype_mirror;
  type integer_value_mirror_pt is protected
    impure function get_subtype_mirror return integer_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function value return integer;
    impure function image return string;
    procedure msim_init(s : integer_subtype_mirror; v : integer; w : value_mirror);
  end protected;
  type integer_value_mirror is access integer_value_mirror_pt;
  type integer_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function simple_name return string;
    impure function left return integer_value_mirror;
    impure function right return integer_value_mirror;
    impure function low return integer_value_mirror;
    impure function high return integer_value_mirror;
    impure function length return index;
    impure function ascending return boolean;
    procedure msim_init(me : integer_subtype_mirror; w : subtype_mirror; n : string; l, r : integer; asc : boolean);
  end protected;
  type integer_subtype_mirror is access integer_subtype_mirror_pt;

  type floating_subtype_mirror;
  type floating_value_mirror_pt is protected
    impure function get_subtype_mirror return floating_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function value return real;
    impure function image return string;
    procedure msim_init(s : floating_subtype_mirror; v : real; w : value_mirror);
  end protected;
  type floating_value_mirror is access floating_value_mirror_pt;
  type floating_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function simple_name return string;
    impure function left return floating_value_mirror;
    impure function right return floating_value_mirror;
    impure function low return floating_value_mirror;
    impure function high return floating_value_mirror;
    impure function ascending return boolean;
    procedure msim_init(me : floating_subtype_mirror; w : subtype_mirror; n : string; l, r : real; asc : boolean);
  end protected;
  type floating_subtype_mirror is access floating_subtype_mirror_pt;

  type physical_subtype_mirror;
  type physical_value_mirror_pt is protected
    impure function get_subtype_mirror return physical_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function unit_index return index;
    impure function value return integer;
    impure function image return string;
    procedure msim_init(s : physical_subtype_mirror; v : real; w : value_mirror);
  end protected;
  type physical_value_mirror is access physical_value_mirror_pt;
  type physical_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function units_length return index;
    impure function unit_name(unit_idx : index) return string;
    impure function unit_index(unit_name : string) return index;
    impure function scale(unit_idx : index) return natural;
    impure function scale(unit_name : string) return natural;
    impure function simple_name return string;
    impure function left return physical_value_mirror;
    impure function right return physical_value_mirror;
    impure function low return physical_value_mirror;
    impure function high return physical_value_mirror;
    impure function length return index;
    impure function ascending return boolean;
    procedure msim_init(me : physical_subtype_mirror; w : subtype_mirror; n : string; unit_list : string; scales : real_vector; l, r : real; asc : boolean);
    impure function msim_scale(unit_idx : index) return real;
  end protected;
  type physical_subtype_mirror is access physical_subtype_mirror_pt;

  type record_subtype_mirror;
  type record_value_mirror_pt is protected
    impure function get_subtype_mirror return record_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function get(element_idx : index) return value_mirror;
    impure function get(element_name : string) return value_mirror;
    procedure msim_init(s : record_subtype_mirror; n : natural; w : value_mirror);
    procedure msim_set(i : natural; v : value_mirror);
  end protected;
  type record_value_mirror is access record_value_mirror_pt;
  type record_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function length return index;
    impure function element_name(element_idx : index) return string;
    impure function element_index(element_name : string) return index;
    impure function element_subtype(element_idx : index) return subtype_mirror;
    impure function element_subtype(element_name : string) return subtype_mirror;
    impure function simple_name return string;
    procedure msim_init(me : record_subtype_mirror; w : subtype_mirror; n : string; names : string; count : natural);
    procedure msim_set(i : natural; s : subtype_mirror);
  end protected;
  type record_subtype_mirror is access record_subtype_mirror_pt;

  type array_subtype_mirror;
  type array_value_mirror_pt is protected
    impure function get_subtype_mirror return array_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function get(idx : index) return value_mirror;
    impure function get(idx1, idx2 : index) return value_mirror;
    impure function get(idx1, idx2, idx3 : index) return value_mirror;
    impure function get(idx : index_vector) return value_mirror;
    procedure msim_init(s : array_subtype_mirror; n : natural; w : value_mirror);
    procedure msim_set(i : natural; v : value_mirror);
  end protected;
  type array_value_mirror is access array_value_mirror_pt;
  type array_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function dimensions return dimension;
    impure function index_subtype(idx : dimension := 1) return subtype_mirror;
    impure function element_subtype return subtype_mirror;
    impure function simple_name return string;
    impure function left(idx : dimension := 1) return index;
    impure function right(idx : dimension := 1) return index;
    impure function low(idx : dimension := 1) return index;
    impure function high(idx : dimension := 1) return index;
    impure function length(idx : dimension := 1) return index;
    impure function ascending(idx : dimension := 1) return boolean;
    procedure msim_init(me : array_subtype_mirror; w : subtype_mirror; n : string; dims : natural; e : subtype_mirror);
    procedure msim_dim(d : natural; s : subtype_mirror; l, r : integer; asc : boolean);
    impure function msim_offset(idx : index_vector) return natural;
  end protected;
  type array_subtype_mirror is access array_subtype_mirror_pt;

  type access_subtype_mirror;
  type access_value_mirror_pt is protected
    impure function get_subtype_mirror return access_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function get return value_mirror;
    impure function is_null return boolean;
    procedure msim_init(s : access_subtype_mirror; v : value_mirror; w : value_mirror);
  end protected;
  type access_value_mirror is access access_value_mirror_pt;
  type access_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function simple_name return string;
    impure function designated_subtype return subtype_mirror;
    procedure msim_init(w : subtype_mirror; n : string; d : subtype_mirror);
  end protected;
  type access_subtype_mirror is access access_subtype_mirror_pt;

  type file_subtype_mirror;
  type file_value_mirror_pt is protected
    impure function get_subtype_mirror return file_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    impure function get_file_logical_name return string;
    impure function get_file_open_kind return file_open_kind;
    procedure msim_init(s : file_subtype_mirror; w : value_mirror);
  end protected;
  type file_value_mirror is access file_value_mirror_pt;
  type file_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function simple_name return string;
    impure function designated_subtype return subtype_mirror;
    procedure msim_init(w : subtype_mirror; n : string; d : subtype_mirror);
  end protected;
  type file_subtype_mirror is access file_subtype_mirror_pt;

  type protected_subtype_mirror;
  type protected_value_mirror_pt is protected
    impure function get_subtype_mirror return protected_subtype_mirror;
    impure function to_value_mirror return value_mirror;
    procedure msim_init(s : protected_subtype_mirror; w : value_mirror);
  end protected;
  type protected_value_mirror is access protected_value_mirror_pt;
  type protected_subtype_mirror_pt is protected
    impure function to_subtype_mirror return subtype_mirror;
    impure function simple_name return string;
    procedure msim_init(w : subtype_mirror; n : string);
  end protected;
  type protected_subtype_mirror is access protected_subtype_mirror_pt;

  type type_class is (class_enumeration, class_integer, class_floating, class_physical, class_record,
                      class_array, class_access, class_file, class_protected);
  alias value_class is type_class;

  type subtype_mirror_pt is protected
    impure function get_type_class return type_class;
    impure function to_enumeration return enumeration_subtype_mirror;
    impure function to_integer return integer_subtype_mirror;
    impure function to_floating return floating_subtype_mirror;
    impure function to_physical return physical_subtype_mirror;
    impure function to_record return record_subtype_mirror;
    impure function to_array return array_subtype_mirror;
    impure function to_access return access_subtype_mirror;
    impure function to_file return file_subtype_mirror;
    impure function to_protected return protected_subtype_mirror;
    impure function simple_name return string;
    procedure msim_init(c : type_class; n : string);
    procedure msim_enum(m : enumeration_subtype_mirror);
    procedure msim_int(m : integer_subtype_mirror);
    procedure msim_float(m : floating_subtype_mirror);
    procedure msim_phys(m : physical_subtype_mirror);
    procedure msim_rec(m : record_subtype_mirror);
    procedure msim_arr(m : array_subtype_mirror);
    procedure msim_acc(m : access_subtype_mirror);
    procedure msim_file(m : file_subtype_mirror);
    procedure msim_prot(m : protected_subtype_mirror);
  end protected;
  type subtype_mirror is access subtype_mirror_pt;

  type value_mirror_pt is protected
    impure function get_value_class return value_class;
    impure function get_subtype_mirror return subtype_mirror;
    impure function to_enumeration return enumeration_value_mirror;
    impure function to_integer return integer_value_mirror;
    impure function to_floating return floating_value_mirror;
    impure function to_physical return physical_value_mirror;
    impure function to_record return record_value_mirror;
    impure function to_array return array_value_mirror;
    impure function to_access return access_value_mirror;
    impure function to_file return file_value_mirror;
    impure function to_protected return protected_value_mirror;
    procedure msim_init(c : value_class; s : subtype_mirror);
    procedure msim_enum(m : enumeration_value_mirror);
    procedure msim_int(m : integer_value_mirror);
    procedure msim_float(m : floating_value_mirror);
    procedure msim_phys(m : physical_value_mirror);
    procedure msim_rec(m : record_value_mirror);
    procedure msim_arr(m : array_value_mirror);
    procedure msim_acc(m : access_value_mirror);
    procedure msim_file(m : file_value_mirror);
    procedure msim_prot(m : protected_value_mirror);
  end protected;
  type value_mirror is access value_mirror_pt;

  -- msim: the constructors 'REFLECT calls
  impure function msim_enum_sm(n, lits : string; lp, rp : integer; asc : boolean) return subtype_mirror;
  impure function msim_enum_vm(s : subtype_mirror; p : integer) return value_mirror;
  impure function msim_int_sm(n : string; l, r : integer; asc : boolean) return subtype_mirror;
  impure function msim_int_vm(s : subtype_mirror; v : integer) return value_mirror;
  impure function msim_float_sm(n : string; l, r : real; asc : boolean) return subtype_mirror;
  impure function msim_float_vm(s : subtype_mirror; v : real) return value_mirror;
  impure function msim_phys_sm(n, unit_list : string; scales : real_vector; l, r : real; asc : boolean) return subtype_mirror;
  impure function msim_phys_vm(s : subtype_mirror; v : real) return value_mirror;
  impure function msim_rec_sm(n, names : string; count : natural) return subtype_mirror;
  impure function msim_rec_vm(s : subtype_mirror) return value_mirror;
  impure function msim_arr_sm(n : string; dims : natural; e : subtype_mirror) return subtype_mirror;
  impure function msim_arr_vm(s : subtype_mirror; count : natural) return value_mirror;
  impure function msim_acc_sm(n : string; d : subtype_mirror) return subtype_mirror;
  impure function msim_acc_vm(s : subtype_mirror; v : value_mirror) return value_mirror;
  impure function msim_file_sm(n : string; d : subtype_mirror) return subtype_mirror;
  impure function msim_file_vm(s : subtype_mirror) return value_mirror;
  impure function msim_prot_sm(n : string) return subtype_mirror;
  impure function msim_prot_vm(s : subtype_mirror) return value_mirror;
end package reflection;

use std.textio.all;
package body reflection is
  type names_ptr is access line_vector;
  type sm_vec is array (natural range <>) of subtype_mirror;
  type sm_vec_ptr is access sm_vec;
  type vm_vec is array (natural range <>) of value_mirror;
  type vm_vec_ptr is access vm_vec;
  type int_vec_ptr is access integer_vector;

  -- names separated by LF, as the translator passes them
  impure function split(s : string) return names_ptr is
    variable n, k, st : natural := 0;
    variable r : names_ptr;
  begin
    if s'length = 0 then return new line_vector(0 to -1); end if;
    n := 1;
    for i in s'range loop
      if s(i) = LF then n := n + 1; end if;
    end loop;
    r := new line_vector(0 to n - 1);
    st := s'left;
    for i in s'range loop
      if s(i) = LF then
        r(k) := new string'(s(st to i - 1));
        k := k + 1;
        st := i + 1;
      end if;
    end loop;
    r(k) := new string'(s(st to s'right));
    return r;
  end;
  impure function find(l : names_ptr; s : string) return integer is
  begin
    for i in l'range loop
      if l(i).all = s then return i; end if;
    end loop;
    return -1;
  end;
  procedure fail(msg : string) is
  begin
    report "std.reflection: " & msg severity failure;
  end;

  -- enumeration ------------------------------------------------------
  type enumeration_value_mirror_pt is protected body
    variable sm : enumeration_subtype_mirror;
    variable ps : integer;
    variable wr : value_mirror;
    impure function get_subtype_mirror return enumeration_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function pos return integer is begin return ps; end;
    impure function image return string is begin return sm.msim_literal(ps); end;
    procedure msim_init(s : enumeration_subtype_mirror; p : integer; w : value_mirror) is
    begin
      sm := s; ps := p; wr := w;
    end;
  end protected body;
  type enumeration_subtype_mirror_pt is protected body
    variable self : enumeration_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable lit : names_ptr;
    variable lpos, rpos : integer;
    variable up : boolean;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function enumeration_literal(literal_idx : natural_index) return enumeration_value_mirror is
      variable v : value_mirror;
    begin
      v := msim_enum_vm(wr, integer(literal_idx));
      return v.to_enumeration;
    end;
    impure function enumeration_literal(literal_name : string) return enumeration_value_mirror is
      variable i : integer := find(lit, literal_name);
    begin
      if i < 0 then fail("no enumeration literal " & literal_name); end if;
      return enumeration_literal(natural_index(i));
    end;
    impure function simple_name return string is begin return nm.all; end;
    impure function left return enumeration_value_mirror is begin return enumeration_literal(natural_index(lpos)); end;
    impure function right return enumeration_value_mirror is begin return enumeration_literal(natural_index(rpos)); end;
    impure function low return enumeration_value_mirror is
    begin
      if up then return left; end if;
      return right;
    end;
    impure function high return enumeration_value_mirror is
    begin
      if up then return right; end if;
      return left;
    end;
    impure function length return positive_index is begin return positive_index(abs (rpos - lpos) + 1); end;
    impure function ascending return boolean is begin return up; end;
    procedure msim_init(me : enumeration_subtype_mirror; w : subtype_mirror; n : string; lits : string; lp, rp : integer; asc : boolean) is
    begin
      self := me; wr := w; nm := new string'(n); lit := split(lits); lpos := lp; rpos := rp; up := asc;
    end;
    impure function msim_literal(p : integer) return string is begin return lit(p).all; end;
  end protected body;

  -- integer ----------------------------------------------------------
  type integer_value_mirror_pt is protected body
    variable sm : integer_subtype_mirror;
    variable vl : integer;
    variable wr : value_mirror;
    impure function get_subtype_mirror return integer_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function value return integer is begin return vl; end;
    impure function image return string is begin return integer'image(vl); end;
    procedure msim_init(s : integer_subtype_mirror; v : integer; w : value_mirror) is
    begin
      sm := s; vl := v; wr := w;
    end;
  end protected body;
  type integer_subtype_mirror_pt is protected body
    variable self : integer_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable lb, rb : integer;
    variable up : boolean;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function simple_name return string is begin return nm.all; end;
    impure function mk(v : integer) return integer_value_mirror is
      variable m : value_mirror;
    begin
      m := msim_int_vm(wr, v);
      return m.to_integer;
    end;
    impure function left return integer_value_mirror is begin return mk(lb); end;
    impure function right return integer_value_mirror is begin return mk(rb); end;
    impure function low return integer_value_mirror is
    begin
      if up then return mk(lb); end if;
      return mk(rb);
    end;
    impure function high return integer_value_mirror is
    begin
      if up then return mk(rb); end if;
      return mk(lb);
    end;
    impure function length return index is
    begin
      if up then return index(maximum(rb - lb + 1, 0)); end if;
      return index(maximum(lb - rb + 1, 0));
    end;
    impure function ascending return boolean is begin return up; end;
    procedure msim_init(me : integer_subtype_mirror; w : subtype_mirror; n : string; l, r : integer; asc : boolean) is
    begin
      self := me; wr := w; nm := new string'(n); lb := l; rb := r; up := asc;
    end;
  end protected body;

  -- floating ---------------------------------------------------------
  type floating_value_mirror_pt is protected body
    variable sm : floating_subtype_mirror;
    variable vl : real;
    variable wr : value_mirror;
    impure function get_subtype_mirror return floating_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function value return real is begin return vl; end;
    impure function image return string is begin return real'image(vl); end;
    procedure msim_init(s : floating_subtype_mirror; v : real; w : value_mirror) is
    begin
      sm := s; vl := v; wr := w;
    end;
  end protected body;
  type floating_subtype_mirror_pt is protected body
    variable self : floating_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable lb, rb : real;
    variable up : boolean;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function simple_name return string is begin return nm.all; end;
    impure function mk(v : real) return floating_value_mirror is
      variable m : value_mirror;
    begin
      m := msim_float_vm(wr, v);
      return m.to_floating;
    end;
    impure function left return floating_value_mirror is begin return mk(lb); end;
    impure function right return floating_value_mirror is begin return mk(rb); end;
    impure function low return floating_value_mirror is
    begin
      if up then return mk(lb); end if;
      return mk(rb);
    end;
    impure function high return floating_value_mirror is
    begin
      if up then return mk(rb); end if;
      return mk(lb);
    end;
    impure function ascending return boolean is begin return up; end;
    procedure msim_init(me : floating_subtype_mirror; w : subtype_mirror; n : string; l, r : real; asc : boolean) is
    begin
      self := me; wr := w; nm := new string'(n); lb := l; rb := r; up := asc;
    end;
  end protected body;

  -- physical: values are kept in primary units as REAL ------------------
  type physical_value_mirror_pt is protected body
    variable sm : physical_subtype_mirror;
    variable vl : real;
    variable wr : value_mirror;
    impure function get_subtype_mirror return physical_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    -- the largest unit that divides the value
    impure function unit_index return index is
    begin
      for i in sm.units_length - 1 downto 1 loop
        if floor(vl / sm.msim_scale(i)) = vl / sm.msim_scale(i) then
          return i;
        end if;
      end loop;
      return 0;
    end;
    -- the value in units of unit_index
    impure function value return integer is begin return integer(vl / sm.msim_scale(unit_index)); end;
    impure function image return string is begin return integer'image(value) & " " & sm.unit_name(unit_index); end;
    procedure msim_init(s : physical_subtype_mirror; v : real; w : value_mirror) is
    begin
      sm := s; vl := v; wr := w;
    end;
  end protected body;
  type physical_subtype_mirror_pt is protected body
    variable self : physical_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable un : names_ptr;
    variable sc : real_vector(0 to 31);
    variable lb, rb : real;
    variable up : boolean;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function units_length return index is begin return index(un'length); end;
    impure function unit_name(unit_idx : index) return string is begin return un(integer(unit_idx)).all; end;
    impure function unit_index(unit_name : string) return index is
      variable i : integer := find(un, unit_name);
    begin
      if i < 0 then fail("no unit " & unit_name); end if;
      return index(i);
    end;
    impure function msim_scale(unit_idx : index) return real is begin return sc(integer(unit_idx)); end;
    impure function scale(unit_idx : index) return natural is
    begin
      if sc(integer(unit_idx)) > real(natural'high) then return natural'high; end if;
      return natural(sc(integer(unit_idx)));
    end;
    impure function scale(unit_name : string) return natural is begin return scale(unit_index(unit_name)); end;
    impure function simple_name return string is begin return nm.all; end;
    impure function mk(v : real) return physical_value_mirror is
      variable m : value_mirror;
    begin
      m := msim_phys_vm(wr, v);
      return m.to_physical;
    end;
    impure function left return physical_value_mirror is begin return mk(lb); end;
    impure function right return physical_value_mirror is begin return mk(rb); end;
    impure function low return physical_value_mirror is
    begin
      if up then return mk(lb); end if;
      return mk(rb);
    end;
    impure function high return physical_value_mirror is
    begin
      if up then return mk(rb); end if;
      return mk(lb);
    end;
    impure function length return index is
    begin
      if abs (rb - lb) + 1.0 > real(integer'high) then return index'high; end if;
      return index(abs (rb - lb) + 1.0);
    end;
    impure function ascending return boolean is begin return up; end;
    procedure msim_init(me : physical_subtype_mirror; w : subtype_mirror; n : string; unit_list : string; scales : real_vector; l, r : real; asc : boolean) is
    begin
      self := me; wr := w; nm := new string'(n); un := split(unit_list); lb := l; rb := r; up := asc;
      for i in 0 to scales'length - 1 loop
        sc(i) := scales(scales'left + i);
      end loop;
    end;
  end protected body;

  -- record -------------------------------------------------------------
  type record_value_mirror_pt is protected body
    variable sm : record_subtype_mirror;
    variable el : vm_vec_ptr;
    variable wr : value_mirror;
    impure function get_subtype_mirror return record_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function get(element_idx : index) return value_mirror is begin return el(integer(element_idx)); end;
    impure function get(element_name : string) return value_mirror is begin return el(integer(sm.element_index(element_name))); end;
    procedure msim_init(s : record_subtype_mirror; n : natural; w : value_mirror) is
    begin
      sm := s; wr := w; el := new vm_vec(0 to n - 1);
    end;
    procedure msim_set(i : natural; v : value_mirror) is begin el(i) := v; end;
  end protected body;
  type record_subtype_mirror_pt is protected body
    variable self : record_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable en : names_ptr;
    variable es : sm_vec_ptr;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function length return index is begin return index(en'length); end;
    impure function element_name(element_idx : index) return string is begin return en(integer(element_idx)).all; end;
    impure function element_index(element_name : string) return index is
      variable i : integer := find(en, element_name);
    begin
      if i < 0 then fail("no record element " & element_name); end if;
      return index(i);
    end;
    impure function element_subtype(element_idx : index) return subtype_mirror is begin return es(integer(element_idx)); end;
    impure function element_subtype(element_name : string) return subtype_mirror is begin return es(integer(element_index(element_name))); end;
    impure function simple_name return string is begin return nm.all; end;
    procedure msim_init(me : record_subtype_mirror; w : subtype_mirror; n : string; names : string; count : natural) is
    begin
      self := me; wr := w; nm := new string'(n); en := split(names); es := new sm_vec(0 to count - 1);
    end;
    procedure msim_set(i : natural; s : subtype_mirror) is begin es(i) := s; end;
  end protected body;

  -- array: elements in row-major order -----------------------------------
  type array_subtype_mirror_pt is protected body
    variable self : array_subtype_mirror;
    variable wr : subtype_mirror;
    variable nm : line;
    variable nd : natural;
    variable es : subtype_mirror;
    variable ix : sm_vec_ptr;
    variable lb, rb : int_vec_ptr;
    variable up : boolean_vector(1 to 16);
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function dimensions return dimension is begin return dimension(nd); end;
    impure function index_subtype(idx : dimension := 1) return subtype_mirror is begin return ix(integer(idx)); end;
    impure function element_subtype return subtype_mirror is begin return es; end;
    impure function simple_name return string is begin return nm.all; end;
    impure function left(idx : dimension := 1) return index is begin return index(lb(integer(idx))); end;
    impure function right(idx : dimension := 1) return index is begin return index(rb(integer(idx))); end;
    impure function low(idx : dimension := 1) return index is
    begin
      if up(integer(idx)) then return left(idx); end if;
      return right(idx);
    end;
    impure function high(idx : dimension := 1) return index is
    begin
      if up(integer(idx)) then return right(idx); end if;
      return left(idx);
    end;
    impure function length(idx : dimension := 1) return index is
      variable d : integer := integer(idx);
    begin
      if up(d) then return index(maximum(rb(d) - lb(d) + 1, 0)); end if;
      return index(maximum(lb(d) - rb(d) + 1, 0));
    end;
    impure function ascending(idx : dimension := 1) return boolean is begin return up(integer(idx)); end;
    procedure msim_init(me : array_subtype_mirror; w : subtype_mirror; n : string; dims : natural; e : subtype_mirror) is
    begin
      self := me; wr := w; nm := new string'(n); nd := dims; es := e;
      ix := new sm_vec(1 to dims);
      lb := new integer_vector(1 to dims);
      rb := new integer_vector(1 to dims);
    end;
    procedure msim_dim(d : natural; s : subtype_mirror; l, r : integer; asc : boolean) is
    begin
      ix(d) := s; lb(d) := l; rb(d) := r; up(d) := asc;
    end;
    impure function msim_offset(idx : index_vector) return natural is
      variable o : natural := 0;
      variable k : integer;
    begin
      if idx'length /= nd then fail("wrong number of indices"); end if;
      for d in 1 to nd loop
        k := integer(idx(idx'left + d - 1));
        if (up(d) and (k < lb(d) or k > rb(d))) or (not up(d) and (k > lb(d) or k < rb(d))) then
          fail("index " & integer'image(k) & " out of range");
        end if;
        if up(d) then k := k - lb(d); else k := lb(d) - k; end if;
        o := o * integer(length(dimension(d))) + k;
      end loop;
      return o;
    end;
  end protected body;

  type array_value_mirror_pt is protected body
    variable sm : array_subtype_mirror;
    variable el : vm_vec_ptr;
    variable wr : value_mirror;
    impure function get_subtype_mirror return array_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function get(idx : index_vector) return value_mirror is begin return el(sm.msim_offset(idx)); end;
    impure function get(idx : index) return value_mirror is
      variable v : index_vector(1 to 1);
    begin
      v(1) := idx;
      return get(v);
    end;
    impure function get(idx1, idx2 : index) return value_mirror is
      variable v : index_vector(1 to 2);
    begin
      v(1) := idx1; v(2) := idx2;
      return get(v);
    end;
    impure function get(idx1, idx2, idx3 : index) return value_mirror is
      variable v : index_vector(1 to 3);
    begin
      v(1) := idx1; v(2) := idx2; v(3) := idx3;
      return get(v);
    end;
    procedure msim_init(s : array_subtype_mirror; n : natural; w : value_mirror) is
    begin
      sm := s; wr := w; el := new vm_vec(0 to n - 1);
    end;
    procedure msim_set(i : natural; v : value_mirror) is begin el(i) := v; end;
  end protected body;
  -- access, file, protected ----------------------------------------------
  type access_value_mirror_pt is protected body
    variable sm : access_subtype_mirror;
    variable vm : value_mirror;
    variable wr : value_mirror;
    impure function get_subtype_mirror return access_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function get return value_mirror is
    begin
      if vm = null then fail("get of a null access value"); end if;
      return vm;
    end;
    impure function is_null return boolean is begin return vm = null; end;
    procedure msim_init(s : access_subtype_mirror; v : value_mirror; w : value_mirror) is
    begin
      sm := s; vm := v; wr := w;
    end;
  end protected body;
  type access_subtype_mirror_pt is protected body
    variable wr : subtype_mirror;
    variable nm : line;
    variable ds : subtype_mirror;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function simple_name return string is begin return nm.all; end;
    impure function designated_subtype return subtype_mirror is begin return ds; end;
    procedure msim_init(w : subtype_mirror; n : string; d : subtype_mirror) is
    begin
      wr := w; nm := new string'(n); ds := d;
    end;
  end protected body;
  type file_value_mirror_pt is protected body
    variable sm : file_subtype_mirror;
    variable wr : value_mirror;
    impure function get_subtype_mirror return file_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    impure function get_file_logical_name return string is begin return ""; end;
    impure function get_file_open_kind return file_open_kind is begin return read_mode; end;
    procedure msim_init(s : file_subtype_mirror; w : value_mirror) is
    begin
      sm := s; wr := w;
    end;
  end protected body;
  type file_subtype_mirror_pt is protected body
    variable wr : subtype_mirror;
    variable nm : line;
    variable ds : subtype_mirror;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function simple_name return string is begin return nm.all; end;
    impure function designated_subtype return subtype_mirror is begin return ds; end;
    procedure msim_init(w : subtype_mirror; n : string; d : subtype_mirror) is
    begin
      wr := w; nm := new string'(n); ds := d;
    end;
  end protected body;
  type protected_value_mirror_pt is protected body
    variable sm : protected_subtype_mirror;
    variable wr : value_mirror;
    impure function get_subtype_mirror return protected_subtype_mirror is begin return sm; end;
    impure function to_value_mirror return value_mirror is begin return wr; end;
    procedure msim_init(s : protected_subtype_mirror; w : value_mirror) is
    begin
      sm := s; wr := w;
    end;
  end protected body;
  type protected_subtype_mirror_pt is protected body
    variable wr : subtype_mirror;
    variable nm : line;
    impure function to_subtype_mirror return subtype_mirror is begin return wr; end;
    impure function simple_name return string is begin return nm.all; end;
    procedure msim_init(w : subtype_mirror; n : string) is
    begin
      wr := w; nm := new string'(n);
    end;
  end protected body;

  -- the common mirrors -------------------------------------------------------
  type subtype_mirror_pt is protected body
    variable cl : type_class;
    variable nm : line;
    variable e : enumeration_subtype_mirror;
    variable i : integer_subtype_mirror;
    variable f : floating_subtype_mirror;
    variable p : physical_subtype_mirror;
    variable r : record_subtype_mirror;
    variable a : array_subtype_mirror;
    variable ac : access_subtype_mirror;
    variable fi : file_subtype_mirror;
    variable pr : protected_subtype_mirror;
    procedure want(c : type_class) is
    begin
      if cl /= c then fail("subtype mirror of class " & type_class'image(cl) & ", not " & type_class'image(c)); end if;
    end;
    impure function get_type_class return type_class is begin return cl; end;
    impure function to_enumeration return enumeration_subtype_mirror is begin want(class_enumeration); return e; end;
    impure function to_integer return integer_subtype_mirror is begin want(class_integer); return i; end;
    impure function to_floating return floating_subtype_mirror is begin want(class_floating); return f; end;
    impure function to_physical return physical_subtype_mirror is begin want(class_physical); return p; end;
    impure function to_record return record_subtype_mirror is begin want(class_record); return r; end;
    impure function to_array return array_subtype_mirror is begin want(class_array); return a; end;
    impure function to_access return access_subtype_mirror is begin want(class_access); return ac; end;
    impure function to_file return file_subtype_mirror is begin want(class_file); return fi; end;
    impure function to_protected return protected_subtype_mirror is begin want(class_protected); return pr; end;
    impure function simple_name return string is begin return nm.all; end;
    procedure msim_init(c : type_class; n : string) is begin cl := c; nm := new string'(n); end;
    procedure msim_enum(m : enumeration_subtype_mirror) is begin e := m; end;
    procedure msim_int(m : integer_subtype_mirror) is begin i := m; end;
    procedure msim_float(m : floating_subtype_mirror) is begin f := m; end;
    procedure msim_phys(m : physical_subtype_mirror) is begin p := m; end;
    procedure msim_rec(m : record_subtype_mirror) is begin r := m; end;
    procedure msim_arr(m : array_subtype_mirror) is begin a := m; end;
    procedure msim_acc(m : access_subtype_mirror) is begin ac := m; end;
    procedure msim_file(m : file_subtype_mirror) is begin fi := m; end;
    procedure msim_prot(m : protected_subtype_mirror) is begin pr := m; end;
  end protected body;
  type value_mirror_pt is protected body
    variable cl : value_class;
    variable sm : subtype_mirror;
    variable e : enumeration_value_mirror;
    variable i : integer_value_mirror;
    variable f : floating_value_mirror;
    variable p : physical_value_mirror;
    variable r : record_value_mirror;
    variable a : array_value_mirror;
    variable ac : access_value_mirror;
    variable fi : file_value_mirror;
    variable pr : protected_value_mirror;
    procedure want(c : value_class) is
    begin
      if cl /= c then fail("value mirror of class " & type_class'image(cl) & ", not " & type_class'image(c)); end if;
    end;
    impure function get_value_class return value_class is begin return cl; end;
    impure function get_subtype_mirror return subtype_mirror is begin return sm; end;
    impure function to_enumeration return enumeration_value_mirror is begin want(class_enumeration); return e; end;
    impure function to_integer return integer_value_mirror is begin want(class_integer); return i; end;
    impure function to_floating return floating_value_mirror is begin want(class_floating); return f; end;
    impure function to_physical return physical_value_mirror is begin want(class_physical); return p; end;
    impure function to_record return record_value_mirror is begin want(class_record); return r; end;
    impure function to_array return array_value_mirror is begin want(class_array); return a; end;
    impure function to_access return access_value_mirror is begin want(class_access); return ac; end;
    impure function to_file return file_value_mirror is begin want(class_file); return fi; end;
    impure function to_protected return protected_value_mirror is begin want(class_protected); return pr; end;
    procedure msim_init(c : value_class; s : subtype_mirror) is begin cl := c; sm := s; end;
    procedure msim_enum(m : enumeration_value_mirror) is begin e := m; end;
    procedure msim_int(m : integer_value_mirror) is begin i := m; end;
    procedure msim_float(m : floating_value_mirror) is begin f := m; end;
    procedure msim_phys(m : physical_value_mirror) is begin p := m; end;
    procedure msim_rec(m : record_value_mirror) is begin r := m; end;
    procedure msim_arr(m : array_value_mirror) is begin a := m; end;
    procedure msim_acc(m : access_value_mirror) is begin ac := m; end;
    procedure msim_file(m : file_value_mirror) is begin fi := m; end;
    procedure msim_prot(m : protected_value_mirror) is begin pr := m; end;
  end protected body;

  -- constructors ---------------------------------------------------------------
  impure function msim_enum_sm(n, lits : string; lp, rp : integer; asc : boolean) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : enumeration_subtype_mirror := new enumeration_subtype_mirror_pt;
  begin
    w.msim_init(class_enumeration, n); w.msim_enum(m);
    m.msim_init(m, w, n, lits, lp, rp, asc);
    return w;
  end;
  impure function msim_enum_vm(s : subtype_mirror; p : integer) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : enumeration_value_mirror := new enumeration_value_mirror_pt;
  begin
    w.msim_init(class_enumeration, s); w.msim_enum(m);
    m.msim_init(s.to_enumeration, p, w);
    return w;
  end;
  impure function msim_int_sm(n : string; l, r : integer; asc : boolean) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : integer_subtype_mirror := new integer_subtype_mirror_pt;
  begin
    w.msim_init(class_integer, n); w.msim_int(m);
    m.msim_init(m, w, n, l, r, asc);
    return w;
  end;
  impure function msim_int_vm(s : subtype_mirror; v : integer) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : integer_value_mirror := new integer_value_mirror_pt;
  begin
    w.msim_init(class_integer, s); w.msim_int(m);
    m.msim_init(s.to_integer, v, w);
    return w;
  end;
  impure function msim_float_sm(n : string; l, r : real; asc : boolean) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : floating_subtype_mirror := new floating_subtype_mirror_pt;
  begin
    w.msim_init(class_floating, n); w.msim_float(m);
    m.msim_init(m, w, n, l, r, asc);
    return w;
  end;
  impure function msim_float_vm(s : subtype_mirror; v : real) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : floating_value_mirror := new floating_value_mirror_pt;
  begin
    w.msim_init(class_floating, s); w.msim_float(m);
    m.msim_init(s.to_floating, v, w);
    return w;
  end;
  impure function msim_phys_sm(n, unit_list : string; scales : real_vector; l, r : real; asc : boolean) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : physical_subtype_mirror := new physical_subtype_mirror_pt;
  begin
    w.msim_init(class_physical, n); w.msim_phys(m);
    m.msim_init(m, w, n, unit_list, scales, l, r, asc);
    return w;
  end;
  impure function msim_phys_vm(s : subtype_mirror; v : real) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : physical_value_mirror := new physical_value_mirror_pt;
  begin
    w.msim_init(class_physical, s); w.msim_phys(m);
    m.msim_init(s.to_physical, v, w);
    return w;
  end;
  impure function msim_rec_sm(n, names : string; count : natural) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : record_subtype_mirror := new record_subtype_mirror_pt;
  begin
    w.msim_init(class_record, n); w.msim_rec(m);
    m.msim_init(m, w, n, names, count);
    return w;
  end;
  impure function msim_rec_vm(s : subtype_mirror) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : record_value_mirror := new record_value_mirror_pt;
  begin
    w.msim_init(class_record, s); w.msim_rec(m);
    m.msim_init(s.to_record, natural(s.to_record.length), w);
    return w;
  end;
  impure function msim_arr_sm(n : string; dims : natural; e : subtype_mirror) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : array_subtype_mirror := new array_subtype_mirror_pt;
  begin
    w.msim_init(class_array, n); w.msim_arr(m);
    m.msim_init(m, w, n, dims, e);
    return w;
  end;
  impure function msim_arr_vm(s : subtype_mirror; count : natural) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : array_value_mirror := new array_value_mirror_pt;
  begin
    w.msim_init(class_array, s); w.msim_arr(m);
    m.msim_init(s.to_array, count, w);
    return w;
  end;
  impure function msim_acc_sm(n : string; d : subtype_mirror) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : access_subtype_mirror := new access_subtype_mirror_pt;
  begin
    w.msim_init(class_access, n); w.msim_acc(m);
    m.msim_init(w, n, d);
    return w;
  end;
  impure function msim_acc_vm(s : subtype_mirror; v : value_mirror) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : access_value_mirror := new access_value_mirror_pt;
  begin
    w.msim_init(class_access, s); w.msim_acc(m);
    m.msim_init(s.to_access, v, w);
    return w;
  end;
  impure function msim_file_sm(n : string; d : subtype_mirror) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : file_subtype_mirror := new file_subtype_mirror_pt;
  begin
    w.msim_init(class_file, n); w.msim_file(m);
    m.msim_init(w, n, d);
    return w;
  end;
  impure function msim_file_vm(s : subtype_mirror) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : file_value_mirror := new file_value_mirror_pt;
  begin
    w.msim_init(class_file, s); w.msim_file(m);
    m.msim_init(s.to_file, w);
    return w;
  end;
  impure function msim_prot_sm(n : string) return subtype_mirror is
    variable w : subtype_mirror := new subtype_mirror_pt;
    variable m : protected_subtype_mirror := new protected_subtype_mirror_pt;
  begin
    w.msim_init(class_protected, n); w.msim_prot(m);
    m.msim_init(w, n);
    return w;
  end;
  impure function msim_prot_vm(s : subtype_mirror) return value_mirror is
    variable w : value_mirror := new value_mirror_pt;
    variable m : protected_value_mirror := new protected_value_mirror_pt;
  begin
    w.msim_init(class_protected, s); w.msim_prot(m);
    m.msim_init(s.to_protected, w);
    return w;
  end;
end package body reflection;
