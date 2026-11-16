// msim's verdict for one run, for regression scripts (included by msim.cpp).
//
// - UVM report counts: msim reads its own standard output as it is written
//   and counts the UVM_INFO / UVM_WARNING / UVM_ERROR / UVM_FATAL messages.
//   When the report server prints its summary ("UVM_ERROR :    2"), the
//   summary's numbers are used instead, since a report catcher can demote
//   or drop messages.
// - -uvm-error-status: exit status 3 when that count of UVM_ERROR or
//   UVM_FATAL is above zero (and nothing gave a non-zero status first).
// - -maxtime: when the limit stops a run that still has events pending,
//   msim says so on standard error; with -maxtime-status it also exits
//   with status 4 (without it the status stays 0, as before, and the same
//   as with -compile).
// - -result FILE: a JSON object with the verdict and each cause on its own.
#pragma once
#include <cstdio>
#include <fstream>
#include <streambuf>
#include <string>

namespace runres {

struct Result {
    std::string path;              // -result FILE
    bool uvmStatus = false;        // -uvm-error-status
    bool limitStatus = false;      // -maxtime-status
    int phase = 0;                 // 0: options and compilation, 1: simulation started, 2: simulation ended
    bool compiledMode = false;     // -compile: the run's output is not read here
    bool scanned = false;          // standard output was read
    long long msg[4] = {0, 0, 0, 0};   // UVM_INFO, UVM_WARNING, UVM_ERROR, UVM_FATAL messages seen
    long long sum[4] = {-1, -1, -1, -1};   // the report summary's counts, -1 until printed
    bool limitHit = false;         // -maxtime stopped the run
    bool finishCalled = false;     // $finish (or $fatal) ended the run
    int fatalStatus = 0;           // $fatal's status, 0 when not called
    std::string endTime;           // simulation time at the end, with its unit
    long long count(int k) const { return sum[k] >= 0 ? sum[k] : msg[k]; }
    bool uvmFailed() const { return count(2) > 0 || count(3) > 0; }
};
inline Result& R() { static Result r; return r; }

// A stream buffer in front of std::cout: passes everything on and looks at
// the start of each line.
struct ScanBuf : std::streambuf {
    std::streambuf* out = nullptr;
    std::string line;   // the current line, up to its first 64 characters
    bool full = false;
    void endLine() {
        static const char* sev[4] = {"UVM_INFO", "UVM_WARNING", "UVM_ERROR", "UVM_FATAL"};
        for (int k = 0; k < 4; k++) {
            size_t n = std::char_traits<char>::length(sev[k]);
            if (line.compare(0, n, sev[k]) != 0 || line.size() == n) continue;
            char c = line[n];
            if (c != ' ' && c != '\t') continue;
            size_t p = line.find_first_not_of(" \t", n);
            if (p != std::string::npos && line[p] == ':') {   // the summary: "UVM_ERROR :    2"
                size_t q = line.find_first_not_of(" \t", p + 1);
                if (q != std::string::npos && std::isdigit((unsigned char)line[q]))
                    R().sum[k] = std::atoll(line.c_str() + q);
            } else R().msg[k]++;
        }
        line.clear();
        full = false;
    }
    void take(const char* s, std::streamsize n) {
        for (std::streamsize i = 0; i < n; i++) {
            if (s[i] == '\n') { endLine(); continue; }
            if (!full) {
                line.push_back(s[i]);
                full = line.size() >= 64;
            }
        }
    }
    int_type overflow(int_type c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return out->pubsync() == 0 ? 0 : traits_type::eof();
        char ch = traits_type::to_char_type(c);
        take(&ch, 1);
        return out->sputc(ch);
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        take(s, n);
        return out->sputn(s, n);
    }
    int sync() override { return out->pubsync(); }
};

// Start reading standard output (at the start of simulation).
inline void scanStdout() {
    static ScanBuf buf;
    if (buf.out) return;
    buf.out = std::cout.rdbuf();
    std::cout.rdbuf(&buf);
    R().scanned = true;
}

// "250ns" from a time in ticks and the tick ("1ns", "10ps").
inline std::string timeText(uint64_t t, const std::string& tick) {
    size_t d = 0;
    while (d < tick.size() && std::isdigit((unsigned char)tick[d])) d++;
    uint64_t mult = d ? std::stoull(tick.substr(0, d)) : 1;
    return std::to_string(t * mult) + tick.substr(d);
}

inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
    return o + "\"";
}

// The final exit status from the one main returned, and the -result file.
inline int finish(int rc) {
    Result& r = R();
    if (r.phase >= 1) std::cout.flush();
    if (rc == 0 && r.phase == 2 && r.uvmStatus && r.scanned && r.uvmFailed()) rc = 3;
    if (rc == 0 && r.limitHit && r.limitStatus) rc = 4;
    if (r.path.empty()) return rc;
    std::string status;
    if (r.phase == 0) status = rc == 0 ? "pass" : rc == 2 ? "usage_error" : "compile_error";
    else if (r.fatalStatus) status = "fatal";
    else if (r.scanned && r.uvmFailed()) status = "uvm_error";
    else if (r.limitHit) status = "timeout";
    else status = rc == 0 ? "pass" : "error";
    std::ofstream f(r.path);
    if (!f) {
        std::cerr << "msim: cannot write " << r.path << "\n";
        return rc ? rc : 1;
    }
    auto b = [](bool v) { return v ? "true" : "false"; };
    f << "{\n"
      << "  \"tool\": \"msim\",\n"
      << "  \"version\": " << jsonStr(MINISIM_VERSION) << ",\n"
      << "  \"status\": \"" << status << "\",\n"
      << "  \"exit_status\": " << rc << ",\n"
      << "  \"compile_error\": " << b(r.phase == 0 && rc != 0 && rc != 2) << ",\n"
      << "  \"simulated\": " << b(r.phase >= 1) << ",\n"
      << "  \"fatal\": " << b(r.fatalStatus != 0) << ",\n"
      << "  \"finish_called\": " << b(r.finishCalled) << ",\n"
      << "  \"maxtime_reached\": " << b(r.limitHit) << ",\n"
      << "  \"end_time\": " << (r.endTime.empty() ? std::string("null") : jsonStr(r.endTime)) << ",\n";
    if (r.scanned) {
        f << "  \"uvm\": {\"info\": " << r.count(0) << ", \"warning\": " << r.count(1) << ", \"error\": " << r.count(2)
          << ", \"fatal\": " << r.count(3) << ", \"summary_seen\": " << b(r.sum[2] >= 0) << "}\n";
    } else f << "  \"uvm\": null\n";
    f << "}\n";
    return rc;
}

}  // namespace runres
