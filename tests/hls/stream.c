// Stream arguments: valid/ready ports, one transfer per read() or write().
#include <stdint.h>
#include "../../include/mhls_stream.h"

// One in, one out.
void scale(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out, uint8_t gain, uint8_t n) {
  for (int i = 0; i < (n & 15); i++) out.write((uint16_t)(in.read() * gain) + i);
}

// A packet: a length byte, then that many bytes; sends the sum and the
// largest. Reads depend on data read before.
uint8_t packet(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out) {
  uint8_t len = in.read() & 7;
  uint16_t sum = 0;
  uint8_t top = 0;
  for (int i = 0; i < len; i++) {
    uint8_t b = in.read();
    sum += b;
    if (b > top) top = b;
  }
  out.write(sum);
  if (len) out.write(top);
  return len;
}

// Two sources merged: the smaller head goes out, the other is kept.
void merge(hls::stream<uint8_t>& a, hls::stream<uint8_t>& b, hls::stream<uint8_t>& out, uint8_t n) {
  uint8_t x = a.read(), y = b.read();
  for (int i = 0; i < (n & 15); i++) {
    if (x <= y) {
      out.write(x);
      x = a.read();
    } else {
      out.write(y);
      y = b.read();
    }
  }
}

// Streams handed to a function, and a read inside a branch.
static uint8_t next(hls::stream<uint8_t>& s, uint8_t skip) {
  uint8_t v = s.read();
  if (skip) v = s.read();
  return v;
}
uint16_t relay(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out, uint8_t mode) {
  uint16_t acc = 0;
  for (int i = 0; i < 4; i++) {
    uint8_t v = next(in, (mode >> i) & 1);
    acc += v;
    if (v & 1) out.write(acc);
  }
  return acc;
}

// A pipelined loop over streams: one element per cycle, and the whole
// pipeline waits while the source is empty or the sink is full.
uint16_t peak(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out, uint8_t gain, uint8_t n) {
  uint16_t top = 0;
  for (int i = 0; i < (n & 31); i++) {
#pragma hls pipeline
    uint16_t y = (uint16_t)(in.read() * gain) >> 2;
    if (y > top) top = y;
    if (y & 1) out.write(y);
  }
  return top;
}
