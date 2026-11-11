#include <stdint.h>
#include "../../include/mhls_stream.h"
uint8_t local(uint8_t a) {
  hls::stream<uint8_t> q;
  q.write(a);
  return q.read();
}
