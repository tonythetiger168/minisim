-- Configurations (3.4): choosing architectures and bindings with a
-- configuration declaration, a configuration specification (7.3),
-- entity e(a), and a configuration instance.
entity gate is
  port (a, b : in bit; y : out bit);
end;
architecture and_a of gate is begin y <= a and b; end;
architecture or_a of gate is begin y <= a or b; end;
architecture xor_a of gate is begin y <= a xor b; end;

entity other_gate is
  port (a, b : in bit; y : out bit);
end;
architecture nand_a of other_gate is begin y <= a nand b; end;

entity pair is
  port (a, b : in bit; y1, y2 : out bit);
end;
architecture s of pair is
  component gate port (a, b : in bit; y : out bit); end component;
begin
  g1: gate port map (a, b, y1);
  g2: gate port map (a, b, y2);
end;

configuration pair_or of pair is
  for s
    for g1 : gate use entity work.gate(or_a); end for;
    for g2 : gate use entity work.gate(and_a); end for;
  end for;
end;

entity config_tb is end;
architecture t of config_tb is
  component gate port (a, b : in bit; y : out bit); end component;
  component pair port (a, b : in bit; y1, y2 : out bit); end component;
  for c1 : gate use entity work.gate(and_a);
  for others : gate use entity work.other_gate(nand_a);
  signal a, b, y0, y1, y2, y3, y4, p1, p2, q1, q2 : bit;
begin
  e0: entity work.gate port map (a, b, y0);           -- last analyzed: xor
  e1: entity work.gate(or_a) port map (a, b, y1);     -- entity e(a)
  c1: gate port map (a, b, y2);                      -- configuration specification
  c2: gate port map (a, b, y3);                      -- for others
  i1: configuration work.pair_or port map (a, b, p1, p2);
  i2: pair port map (a, b, q1, q2);                  -- default binding
  process begin
    for i in 0 to 3 loop
      a <= bit'val(i / 2); b <= bit'val(i mod 2);
      wait for 1 ns;
      report "a=" & bit'image(a) & " b=" & bit'image(b) & " xor=" & bit'image(y0) & " or=" & bit'image(y1) &
             " and=" & bit'image(y2) & " nand=" & bit'image(y3) & " cfg=" & bit'image(p1) & bit'image(p2) &
             " dflt=" & bit'image(q1) & bit'image(q2);
    end loop;
    wait;
  end process;
end;

configuration top_cfg of config_tb is
  for t
    for i2 : pair
      use entity work.pair(s);
      for s
        for all : gate use entity work.other_gate(nand_a); end for;
      end for;
    end for;
  end for;
end;
