// Arbitrary-width integers: ap_uint/ap_int widths survive into the RTL.
// (No host-compiler check for this one: ap_int.h is not a standard header.)
typedef ap_uint<12> pix_t;
ap_uint<24> blend(ap_uint<12> a, ap_uint<12> b, ap_uint<5> alpha, ap_int<7> bias, sc_uint<3> mode) {
  ap_uint<17> wa = a * alpha;                 // 12 x 5 bits
  ap_uint<17> wb = b * (ap_uint<5>)(31 - alpha);
  ap_int<19> mix = (wa + wb) >> 5;
  mix += bias;
  if (mix < 0) mix = 0;
  pix_t out = mix > 4095 ? (pix_t)4095 : (pix_t)mix;
  ap_uint<24> packed = ((ap_uint<24>)out << 12) | (mode == 3 ? a : b);
  return packed;
}
