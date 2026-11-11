// hls::stream<T> for running mhls sources with a host compiler. mhls itself
// knows the type and ignores this header: a stream parameter becomes a
// valid/ready port, read() and write() become transfers.
#ifndef MHLS_STREAM_H
#define MHLS_STREAM_H
#include <deque>

namespace hls {
template <class T>
class stream {
 public:
  T read() {
    if (q_.empty()) return T();   // hardware would wait; a host run has nobody to wait for
    T v = q_.front();
    q_.pop_front();
    return v;
  }
  void write(const T& v) { q_.push_back(v); }
  bool empty() const { return q_.empty(); }
  unsigned size() const { return (unsigned)q_.size(); }

 private:
  std::deque<T> q_;
};
}   // namespace hls
#endif
