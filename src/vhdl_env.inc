// vhdl_env.inc - helpers for VHDL-2019 std.env (IEEE 1076-2019 16.5.5 - 16.5.7):
// calendar time, environment variables and directories. The VHDL front end's
// vhdl_env package calls them through DPI-C; msim exports them (-rdynamic),
// so the DPI run time finds them in msim itself. #include in msim.cpp only.
#include <sys/time.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>

extern "C" {

double msim_vhdl_epoch() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

// field f of the time `t` (seconds since 1970 UTC) broken down in local time or UTC:
// 0 microsecond, 1 second, 2 minute, 3 hour, 4 day, 5 month (0-11), 6 year, 7 weekday, 8 day of year
int msim_vhdl_tm(double t, int loc, int f) {
    double fl = t < 0 ? -std::ceil(-t) : std::floor(t);
    time_t s = (time_t)fl;
    struct tm b;
    if (loc) localtime_r(&s, &b);
    else gmtime_r(&s, &b);
    switch (f) {
    case 0: return (int)std::lround((t - fl) * 1e6) % 1000000;
    case 1: return b.tm_sec;
    case 2: return b.tm_min;
    case 3: return b.tm_hour;
    case 4: return b.tm_mday;
    case 5: return b.tm_mon;
    case 6: return b.tm_year + 1900;
    case 7: return b.tm_wday;
    default: return b.tm_yday;
    }
}

// seconds since 1970 UTC of a broken-down time in local time (loc) or UTC
double msim_vhdl_mktime(int us, int s, int mi, int h, int d, int mo, int y, int loc) {
    struct tm b = {};
    b.tm_sec = s;
    b.tm_min = mi;
    b.tm_hour = h;
    b.tm_mday = d;
    b.tm_mon = mo;
    b.tm_year = y - 1900;
    b.tm_isdst = -1;
    time_t r = loc ? mktime(&b) : timegm(&b);
    return (double)r + us / 1e6;
}

const char* msim_vhdl_getenv(const char* n) {
    const char* v = std::getenv(n);
    return v ? v : "";
}

const char* msim_vhdl_cwd() {
    static std::string s;
    std::error_code ec;
    s = std::filesystem::current_path(ec).string();
    return s.c_str();
}

static int msim_vhdl_errStatus(const std::error_code& ec, int notFound, int denied, int other) {
    if (ec == std::errc::no_such_file_or_directory) return notFound;
    if (ec == std::errc::permission_denied || ec == std::errc::operation_not_permitted) return denied;
    return other;
}

// DIR_OPEN_STATUS: 0 ok, 1 not found, 2 no directory, 3 access denied, 4 error
int msim_vhdl_chdir(const char* p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return 1;
    if (!std::filesystem::is_directory(p, ec)) return 2;
    std::filesystem::current_path(p, ec);
    return ec ? msim_vhdl_errStatus(ec, 1, 3, 4) : 0;
}

// 0: exists, 1: is a directory, 2: is a regular file
int msim_vhdl_stat(const char* p, int what) {
    std::error_code ec;
    if (what == 1) return std::filesystem::is_directory(p, ec);
    if (what == 2) return std::filesystem::is_regular_file(p, ec);
    return std::filesystem::exists(p, ec);
}

// DIR_CREATE_STATUS: 0 ok, 1 item exists, 2 access denied, 3 error
int msim_vhdl_mkdir(const char* p, int parents) {
    std::error_code ec;
    if (std::filesystem::exists(p, ec)) return 1;
    bool ok = parents ? std::filesystem::create_directories(p, ec) : std::filesystem::create_directory(p, ec);
    if (ok && !ec) return 0;
    return msim_vhdl_errStatus(ec, 3, 2, 3);
}

// DIR_DELETE_STATUS: 0 ok, 1 no directory, 2 not empty, 3 access denied, 4 error
int msim_vhdl_rmdir(const char* p, int recursive) {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec)) return 1;
    if (!recursive && !std::filesystem::is_empty(p, ec)) return 2;
    if (recursive) std::filesystem::remove_all(p, ec);
    else std::filesystem::remove(p, ec);
    return ec ? msim_vhdl_errStatus(ec, 1, 3, 4) : 0;
}

// FILE_DELETE_STATUS: 0 ok, 1 no file, 2 access denied, 3 error
int msim_vhdl_unlink(const char* p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return 1;
    std::filesystem::remove(p, ec);
    return ec ? msim_vhdl_errStatus(ec, 1, 2, 3) : 0;
}

}  // extern "C"
