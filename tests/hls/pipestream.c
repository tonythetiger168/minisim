// Pipelined functions (-pipeline) with stream arguments: one transfer per
// stream per call, and the whole pipeline waits in a cycle where a stream
// that a stage needs is not ready.
#include <stdint.h>
#include "../../include/mhls_stream.h"
// one element in, one out, per call
void scale(hls::stream<int16_t>& in, hls::stream<int32_t>& out, int16_t g) {
  int16_t x = in.read();
  out.write(x * g + 7);
}
// read always, write when above a threshold; a static counter and a result too
int32_t filt(hls::stream<uint8_t>& in, hls::stream<uint16_t>& out, uint8_t th) {
  static uint16_t n;
  n = n + 1;
  uint8_t x = in.read();
  if (x > th) out.write(n ^ x);
  return n + x;
}
// out only
void gen(hls::stream<uint32_t>& out, uint32_t a, uint32_t b) { out.write(a * b + (a >> 3)); }
// in only, with a memory
int32_t sum(hls::stream<int32_t>& in, int32_t t[16], uint8_t k) {
  int32_t x = in.read();
  return x + t[k & 15];
}
// Two reads of one stream in one call: refused.
int32_t twice(hls::stream<int16_t>& in) {
  int16_t a = in.read();
  int16_t b = in.read();
  return a * b;
}
