#include <stdint.h>
#include "../../include/mhls_stream.h"
uint8_t peek(hls::stream<uint8_t>& s) {
  if (s.empty()) return 0;
  return s.read();
}
