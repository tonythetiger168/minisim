// IEEE 1800-2023 11.4.14.4: with [...] picks the items of a one-dimensional
// array to stream. Packing, it acts like a slice (items past the end of a
// container read as missing elements). Unpacking, each range is evaluated
// just before its array, so it can use a length unpacked earlier in the same
// stream; a container grows to hold its range. A container with no range
// takes what the fixed-size targets after it leave (11.4.14.3), and a stream
// too short for the targets is an error. Unpacked structs of integral
// members stream their members in order (11.4.14.1).
//! icarus-skip: Icarus 13 does not support with [] or containers in streams
//> 2233 2211 44
//> 02030405 0203 0a0b 0b0c44
//> 15 bytes, first 46 last 11
//> 11223344 3 '{-95, -94, -93} 00000146
//> 01 '{2, 3, 4} 05
//> 0a '{0, 11, 12, 0}
//> 7 2 '{1, 2} 3
//> 123456 563412 '{18, 52, 86, -85, -51}
//> 01 0203
//> Error: the stream is too short for the targets of a streaming assignment (11.4.14.3)
module top;
  typedef struct { byte a; shortint b; } s_t;
  typedef struct packed { byte a; byte b; } p_t;
  class Pkt;
    int header, len;
    byte payload[];
    int crc;
  endclass
  byte fa[4] = '{8'h11, 8'h22, 8'h33, 8'h44};
  byte d[] = '{8'h01, 8'h02, 8'h03, 8'h04, 8'h05};
  byte q[$] = '{8'ha, 8'hb, 8'hc};
  byte stream[$];
  byte payload[];
  int n = 2, header, len, crc;
  logic [31:0] w;
  logic [23:0] v;
  logic [15:0] h, h2;
  logic [7:0] a, b;
  s_t s;
  p_t pk;
  Pkt p, r;
  initial begin
    // packing
    h = {>>{fa with [1:2]}};
    h2 = {<<byte{fa with [0+:2]}};
    b = {>>{fa with [3]}};
    $display("%h %h %h", h, h2, b);
    w = {>>{d with [1:4]}};
    h = {>>{d with [n-:2]}};
    h2 = {>>{q with [0:n-1]}};
    v = {>>{q with [1:2], fa with [3]}};
    $display("%h %h %h %h", w, h, h2, v);
    // a packet with a length field, little-endian bytes
    header = 32'h11223344; len = 3; payload = '{8'ha1, 8'ha2, 8'ha3}; crc = 32'h146;
    stream = {<<byte{header, len, payload with [0 +: len], crc}};
    $display("%0d bytes, first %h last %h", stream.size(), stream[0], stream[$]);
    header = 0; len = 0; payload.delete(); crc = 0;
    {<<byte{header, len, payload with [0 +: len], crc}} = stream;
    $display("%h %0d %p %h", header, len, payload, crc);
    // greedy container, fixed array with a range
    {>>{a, q, b}} = 40'h01_02_03_04_05;
    $display("%h %p %h", a, q, b);
    fa = '{0, 0, 0, 0};
    {>>{a, fa with [1:2]}} = 24'h0a_0b_0c;
    $display("%h %p", a, fa);
    // class properties
    p = new; p.header = 7; p.len = 2; p.payload = '{1, 2}; p.crc = 3;
    stream = {<<byte{p.header, p.len, p.payload with [0 +: p.len], p.crc}};
    r = new;
    {<<byte{r.header, r.len, r.payload with [0 +: r.len], r.crc}} = stream;
    $display("%0d %0d %p %0d", r.header, r.len, r.payload, r.crc);
    // unpacked structs
    s = '{8'h12, 16'h3456}; pk = 16'habcd;
    v = {>>{s}};
    w = {<<byte{s}};
    q = {>>byte{s, pk}};
    $display("%h %h %p", v, w[31:8], q);
    {>>{s}} = 24'h010203;
    $display("%h %h", s.a, s.b);
    // too short: q would need five bytes
    len = 4;
    {>>{a, q with [0:len]}} = 16'hffee;
  end
endmodule
