-- Package generics (6.5.6.4): a generic package whose generic is an
-- instance of another generic package.
package scale_g is
  generic (factor : integer; name : string);
  constant twice_factor : integer := 2 * factor;
  constant nm : string := name;
  function scale(x : integer) return integer;
end;
package body scale_g is
  function scale(x : integer) return integer is begin return x * factor; end;
end;

package acc_g is
  generic (package s is new work.scale_g generic map (<>); offset : integer := 0);
  function apply(x : integer) return integer;
  function tag return string;
end;
package body acc_g is
  function apply(x : integer) return integer is begin return s.scale(x) + offset + s.twice_factor; end;
  function tag return string is begin return s.nm & "+" & integer'image(offset); end;
end;

package by3 is new work.scale_g generic map (factor => 3, name => "by3");
package by10 is new work.scale_g generic map (factor => 10, name => "by10");
package acc3 is new work.acc_g generic map (s => work.by3, offset => 1);
package acc10 is new work.acc_g generic map (s => work.by10);

entity pkggen_tb is end;
architecture a of pkggen_tb is
begin
  process begin
    report work.acc3.tag & ": " & integer'image(work.acc3.apply(5));
    report work.acc10.tag & ": " & integer'image(work.acc10.apply(5));
    wait;
  end process;
end;
