-- real'image and to_string(real) in GHDL's form (16.2.2)
entity realimg_tb is end;
architecture t of realimg_tb is
  type ra is array (natural range <>) of real;
  constant v : ra := (0.0, 1.0, 1.5, -2.25, 0.1, 100.0, 123456.789, 1.0e10, 1.0e-10, 3.14159265358979, 1.0e300, -0.0, 2.0e-5, 12345678.0, 0.5e-3);
begin
  process begin
    for i in v'range loop report real'image(v(i)) & " " & to_string(v(i)); end loop;
    wait;
  end process;
end;
