// 8-tap FIR filter: static delay line, constant coefficients, unrolled loops.
#define TAPS 8
static const short coef[TAPS] = {-3, 11, 42, 78, 78, 42, 11, -3};
int fir(short x) {
  static short line[TAPS];
  int acc = 0;
#pragma hls unroll
  for (int i = TAPS - 1; i > 0; i--) line[i] = line[i - 1];
  line[0] = x;
#pragma hls unroll
  for (int i = 0; i < TAPS; i++) acc += line[i] * coef[i];
  return acc;
}
