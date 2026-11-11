// switch: an ALU as multiplexers (returns in the cases), fall-through,
// several labels on one case, a default in the middle, and a switch with a
// break nested in an if inside a loop, where continue still means the loop.
#include <stdint.h>
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_SHL, OP_SHR, OP_SLT, OP_MUL = 12 };

static int32_t alu1(uint8_t op, int32_t a, int32_t b) {
  switch (op) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_AND: return a & b;
    case OP_OR: return a | b;
    case OP_XOR: return a ^ b;
    case OP_SHL: return (int32_t)((uint32_t)a << (b & 31));
    case OP_SHR: return a >> (b & 31);
    case OP_SLT: return a < b;
    case OP_MUL: return a * b;
  }
  return 0;
}

int32_t alu(uint8_t op, int32_t a, int32_t b, uint8_t *flags) {
  int32_t r = alu1(op & 15, a, b);
  uint8_t f = 0;
  switch (op >> 4) {
    case 1:
      f |= 1;            // falls through
    case 2:
    case 3:
      f |= 2;
      break;
    default:
      f = 0x80;
      break;
    case 7:
      f = (uint8_t)(r == 0) << 2;
  }
  for (int i = 0; i < 4; i++) {
    switch ((a >> (i * 2)) & 3) {
      case 0: continue;
      case 1:
        if (b & (1 << i)) break;
        f ^= 0x10;
        break;
      case 2: f += 0x20; break;
      default: r ^= i;
    }
    r += i;
  }
  *flags = f;
  return r;
}
