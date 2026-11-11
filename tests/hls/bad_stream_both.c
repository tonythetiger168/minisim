#include <stdint.h>
#include "../../include/mhls_stream.h"
uint8_t echo(hls::stream<uint8_t>& s) {
  uint8_t v = s.read();
  s.write(v);
  return v;
}
