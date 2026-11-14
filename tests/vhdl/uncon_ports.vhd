-- Unconstrained ports (6.5.6.3): an entity port without a range takes its
-- index range from the actual, ascending or descending, through hierarchy.
library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity par is
  port (d : in std_ulogic_vector; p : out std_ulogic; n : out natural);
end;
architecture a of par is begin
  p <= xor d;
  n <= d'length;
  process (d) begin
    report "par: d'left=" & integer'image(d'left) & " d'right=" & integer'image(d'right) &
           " asc=" & boolean'image(d'ascending) & " len=" & integer'image(d'length) & " d=" & to_string(d);
  end process;
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity inv is
  port (a : in std_ulogic_vector; y : out std_ulogic_vector);
end;
architecture a of inv is begin
  y <= not a;
  process (a) begin if now = 0 ns then
    report "inv: a'high=" & integer'image(a'high) & " a'low=" & integer'image(a'low) &
           " y'left=" & integer'image(y'left) & " a=" & to_string(a); end if;
  end process;
end;

-- an unconstrained port passed on to another unconstrained port
library ieee; use ieee.std_logic_1164.all;
entity wrap is
  port (i : in std_ulogic_vector; o : out std_ulogic_vector; par_o : out std_ulogic);
end;
architecture a of wrap is
  signal n : natural;
begin
  u1: entity work.inv port map (a => i, y => o);
  u2: entity work.par port map (d => i, p => par_o, n => n);
end;

library ieee; use ieee.std_logic_1164.all; use ieee.numeric_std.all;
entity uncon_ports_tb is end;
architecture t of uncon_ports_tb is
  signal a : std_ulogic_vector(7 downto 0) := x"a5";
  signal b : std_ulogic_vector(0 to 2) := "110";
  signal c : std_ulogic_vector(11 downto 4) := x"3c";
  signal ca : std_ulogic_vector(11 downto 4);
  signal e : std_ulogic_vector(1 to 4) := "1000";
  signal eo : std_ulogic_vector(1 to 4);
  signal pa, pb, pc, pe : std_ulogic; signal na, nb, nc : natural;
begin
  ua: entity work.par port map (d => a, p => pa, n => na);
  ub: entity work.par port map (b, pb, nb);
  uc: entity work.par port map (d => c(7 downto 4), p => pc, n => nc);
  ui: entity work.inv port map (a => c, y => ca);
  uw: entity work.wrap port map (i => e, o => eo, par_o => pe);
  process begin
    wait for 1 ns;
    report "pa=" & std_ulogic'image(pa) & " na=" & integer'image(na) & " pb=" & std_ulogic'image(pb) &
           " nb=" & integer'image(nb) & " pc=" & std_ulogic'image(pc) & " nc=" & integer'image(nc);
    report "ca=" & to_string(ca) & " eo=" & to_string(eo) & " pe=" & std_ulogic'image(pe);
    a <= x"a4"; wait for 1 ns;
    report "pa=" & std_ulogic'image(pa);
    e <= "0110"; wait for 1 ns;
    report "eo=" & to_string(eo) & " pe=" & std_ulogic'image(pe);
    wait;
  end process;
end;
