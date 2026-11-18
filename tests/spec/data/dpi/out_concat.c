/* C side of 35.5.6-output-into-concatenation.sv: fills a bit [15:0] output. */
#include "svdpi.h"
void fill16(svBitVecVal* d) { d[0] = 0xa5c3u; }
