--! args: -top vhdl -enable FSM003
-- mlint on VHDL: the rules see the design through msim's VHDL front end
-- (src/vhdl/), and nothing of how the translation is written.
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity vhdl is
  port (
    clk_a : in  std_logic;
    clk_b : in  std_logic;
    rst   : in  std_logic;
    din   : in  std_logic_vector(7 downto 0);
    spare : in  std_logic; --~ UNU004
    sel   : in  std_logic_vector(1 downto 0);
    en1   : in  std_logic;
    en2   : in  std_logic;
    dout  : out std_logic_vector(7 downto 0);
    flag  : out std_logic;
    bus_o : out std_logic_vector(3 downto 0);
    st_o  : out std_logic
  );
end entity;

architecture rtl of vhdl is
  type state_t is (IDLE, RUN, DONE);
  type ctrl_t is record
    run  : std_logic;
    halt : std_logic;
  end record;
  signal a_reg   : std_logic_vector(7 downto 0);
  signal b_reg   : std_logic_vector(7 downto 0);
  signal lonely  : std_logic; --~ UNU002
  signal never   : std_logic; --~ UNU003
  signal lat     : std_logic;
  signal state   : state_t;
  signal ctrl    : ctrl_t;
  signal bus_l   : std_logic_vector(3 downto 0);
begin
  -- the asynchronous reset form, compared with '1'
  process (clk_a, rst)
  begin
    if rst = '1' then
      a_reg <= (others => '0');
      state <= IDLE;
    elsif rising_edge(clk_a) then
      a_reg <= din;
      case state is   -- every value listed: VHDL needs no "others"
        when IDLE => state <= RUN;
        when RUN  => state <= DONE;
        when DONE => state <= IDLE;
      end case;
    end if;
  end process;

  process (clk_b)
  begin
    if rising_edge(clk_b) then
      b_reg <= a_reg; --~ CDC001
    end if;
  end process;

  process (sel, din) --~ LAT001
  begin
    if sel = "01" then
      lat <= din(0);
    end if;
  end process;

  -- two fields of one record from two processes
  ctrl.run <= '1' when din(7) = '1' else '0';
  process (din)
  begin
    ctrl.halt <= din(6);
  end process;

  -- a tri-state bus: a resolved signal with two drivers
  bus_l <= x"5" when en1 = '1' else (others => 'Z');
  bus_l <= x"A" when en2 = '1' else (others => 'Z');

  lonely <= din(1);
  dout <= b_reg;
  flag <= never and lat and ctrl.run and ctrl.halt;
  bus_o <= bus_l;
  st_o <= '1' when state = DONE else '0';
  assert din /= x"FF" report "din is " & to_hstring(din) severity note;
end architecture;
