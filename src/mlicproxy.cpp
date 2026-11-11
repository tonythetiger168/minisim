// mlicproxy.cpp - a license proxy for protected IP (IEEE 1735-2023 clause 8).
//
// The IP author runs it (or gives it to the customer to run) next to the
// protected IP. A tool that decrypts an envelope whose rights hold a
// license("feature") or license_string("feature") call connects here over
// TLS, sends the feature name, and gets back granted / denied, a string, and
// an explanatory message (8.8). Checks are existence checks: nothing is
// counted or checked out, and the answer depends only on the license file
// and the date (8.5).
//
//   openssl req -x509 -newkey rsa:3072 -nodes -days 3650 -subj /CN=acme-proxy
//       -keyout proxy_key.pem -out proxy_cert.pem          (one command line)
//   mlicproxy -cert proxy_cert.pem -key proxy_key.pem -port 27100 -licenses acme.lic
//   mlicproxy -cert proxy_cert.pem -pragmas ACME_PROXY      # the common block lines for the IP source
//
// and on the tool side:  ACME_PROXY=27100@licserver msim -keyfile ... ip.svp tb.sv
//
// The license file has one feature per line:
//
//   # comment
//   acme_debug                                  granted
//   acme_tier = "gold"                          granted; license_string returns gold
//   acme_eval expires 2027-06-30                granted through that day (UTC)
//
//   -cert <pem> -key <pem>   the proxy's certificate and private key
//   -port <n>                port to listen on (0: any free port)
//   -bind <address>          address to listen on (default: all)
//   -licenses <file>         the license file (re-read when it changes)
//   -portfile <file>         write the port number here once listening
//   -banner <text>           sent to each tool on connection (request id 0, 8.8.3)
//   -pragmas <ENVNAME>       print the license_proxyenv / license_certificate lines and exit
//   -log                     print each request to stderr
#include <atomic>
#include <csignal>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sys/stat.h>

#include "licproxy.hpp"

namespace {

struct Feature {
    std::string str;
    std::string expires;   // YYYY-MM-DD, empty: never
};

struct Licenses {
    std::string path;
    std::mutex mu;
    std::map<std::string, Feature> features;
    time_t mtime = 0;
    std::string error;

    bool load() {
        std::ifstream in(path);
        if (!in) { error = "cannot read " + path; return false; }
        std::map<std::string, Feature> f;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            n++;
            if (size_t h = line.find('#'); h != std::string::npos) line.erase(h);
            std::istringstream ss(line);
            std::string name, w;
            if (!(ss >> name)) continue;
            Feature feat;
            auto bad = [&](const std::string& why) {
                error = path + ":" + std::to_string(n) + ": " + why;
                return false;
            };
            // name=“x” without spaces
            if (size_t eq = name.find('='); eq != std::string::npos) {
                std::string rest = name.substr(eq);
                name.erase(eq);
                std::string tail;
                std::getline(ss, tail);
                ss.clear();
                ss.str(rest + tail);
            }
            if (name.empty() || !licproxy::isIa5(name)) return bad("bad feature name");
            while (ss >> w) {
                if (w[0] == '=') {
                    std::string v = w.substr(1), more;
                    if (v.empty() && !(ss >> v)) return bad("= needs a string");
                    if (v[0] != '"') return bad("the string after = must be quoted");
                    while (v.size() < 2 || v.back() != '"') {
                        std::string nx;
                        if (!(ss >> nx)) return bad("unterminated string");
                        v += " " + nx;
                    }
                    feat.str = v.substr(1, v.size() - 2);
                    if (!licproxy::isIa5(feat.str)) return bad("the string must be 7-bit ASCII");
                } else if (w == "expires") {
                    if (!(ss >> feat.expires) || feat.expires.size() != 10 || feat.expires[4] != '-' || feat.expires[7] != '-')
                        return bad("expires needs a date YYYY-MM-DD");
                } else {
                    return bad("unexpected " + w);
                }
            }
            f[name] = feat;
        }
        std::lock_guard<std::mutex> g(mu);
        features = std::move(f);
        return true;
    }
    // Re-read the file when it changed on disk.
    void refresh() {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || st.st_mtime == mtime) return;
        mtime = st.st_mtime;
        if (!load()) std::cerr << "mlicproxy: " << error << " (keeping the previous licenses)\n";
    }
    static std::string today() {
        time_t t = time(nullptr);
        tm g;
        gmtime_r(&t, &g);
        char b[16];
        strftime(b, sizeof b, "%Y-%m-%d", &g);
        return b;
    }
    // granted, string, explanatory message
    std::tuple<bool, std::string, std::string> check(const std::string& name) {
        refresh();
        std::lock_guard<std::mutex> g(mu);
        auto it = features.find(name);
        if (it == features.end()) return {false, "", "License '" + name + "' denied"};
        if (!it->second.expires.empty() && today() > it->second.expires)
            return {false, "", "License '" + name + "' expired on " + it->second.expires};
        return {true, it->second.str, "License '" + name + "' granted"};
    }
};

struct Opts {
    std::string cert, key, port = "0", bind, licenses, portfile, banner, pragmas;
    bool log = false;
    // Faults for the tool-side tests (8.7.1, 8.7.2).
    int testGarble = 0;      // answer the first N requests with bytes that are not a message
    bool testSilent = false; // never answer
    bool testWrongId = false;// answer with a request id the tool did not send
};

std::atomic<int> garbleLeft{0};

void serve(SSL_CTX* ctx, int fd, Licenses& lic, const Opts& o) {
    licproxy::setTimeouts(fd, 300000);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) == 1) {
        if (!o.banner.empty()) {
            std::string m = licproxy::encodeInfo(o.banner);
            SSL_write(ssl, m.data(), (int)m.size());
        }
        std::string buf;
        for (;;) {
            size_t used = 0;
            std::vector<licproxy::Elem> e;
            licproxy::Parse p = licproxy::decode(buf, used, e);
            if (p == licproxy::Parse::Bad) break;
            if (p == licproxy::Parse::More) {
                char tmp[4096];
                int n = SSL_read(ssl, tmp, sizeof tmp);
                if (n <= 0) break;
                buf.append(tmp, (size_t)n);
                continue;
            }
            buf.erase(0, used);
            // 8.8.1: version, a positive request id, the argument.
            if (e.size() != 3 || e[0].i != licproxy::kProtocol || e[1].i <= 0 || e[2].k != licproxy::Elem::Str) break;
            auto [granted, str, msg] = lic.check(e[2].s);
            if (o.log) std::cerr << "mlicproxy: " << msg << "\n";
            if (o.testSilent) continue;
            std::string reply;
            if (garbleLeft.fetch_sub(1) > 0) reply = "\x16\x03not";
            else reply = licproxy::encodeReply(o.testWrongId ? e[1].i + 1000 : e[1].i, granted, str, msg);
            if (SSL_write(ssl, reply.data(), (int)reply.size()) <= 0) break;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    ERR_clear_error();
}

void usage() {
    std::cerr << "usage: mlicproxy -cert <cert.pem> -key <key.pem> -licenses <file> [-port <n>] [options]\n"
                 "       mlicproxy -cert <cert.pem> -pragmas <ENVNAME>\n"
                 "  -port <n>         port to listen on (default 0: any free port, printed)\n"
                 "  -bind <address>   address to listen on (default: all)\n"
                 "  -portfile <file>  write the port number to this file once listening\n"
                 "  -banner <text>    message sent to each tool when it connects\n"
                 "  -pragmas <ENV>    print the common block lines for the IP source and exit\n"
                 "  -log              print each request to stderr\n";
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::cerr << "mlicproxy: " << a << " needs a value\n"; std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
        else if (a == "-cert") o.cert = next();
        else if (a == "-key") o.key = next();
        else if (a == "-port") o.port = next();
        else if (a == "-bind") o.bind = next();
        else if (a == "-licenses") o.licenses = next();
        else if (a == "-portfile") o.portfile = next();
        else if (a == "-banner") o.banner = next();
        else if (a == "-pragmas") o.pragmas = next();
        else if (a == "-log") o.log = true;
        else if (a == "-test_garble") o.testGarble = std::atoi(next().c_str());
        else if (a == "-test_silent") o.testSilent = true;
        else if (a == "-test_wrong_id") o.testWrongId = true;
        else { std::cerr << "mlicproxy: unknown option " << a << "\n"; usage(); return 2; }
    }
    if (o.cert.empty()) { usage(); return 2; }

    // The certificate: its DER is what the envelope pins.
    X509* x = nullptr;
    if (FILE* f = std::fopen(o.cert.c_str(), "r")) {
        x = PEM_read_X509(f, nullptr, nullptr, nullptr);
        std::fclose(f);
    }
    if (!x) { std::cerr << "mlicproxy: " << o.cert << " is not a PEM certificate\n"; return 1; }
    if (EVP_PKEY* pk = X509_get0_pubkey(x)) {
        int bits = EVP_PKEY_get_bits(pk), id = EVP_PKEY_get_base_id(pk);
        // 8.7.1: at least 3072 bits for RSA, 256 for elliptic curves.
        if ((id == EVP_PKEY_RSA && bits < 3072) || (id == EVP_PKEY_EC && bits < 256))
            std::cerr << "mlicproxy: warning: the certificate's key has " << bits
                      << " bits; IEEE 1735 8.7.1 asks for at least 3072 (RSA) or 256 (elliptic curve)\n";
    }

    if (!o.pragmas.empty()) {
        if (!licproxy::legalEnvName(o.pragmas)) { std::cerr << "mlicproxy: " << o.pragmas << " is not an environment variable name\n"; return 2; }
        unsigned char* der = nullptr;
        int n = i2d_X509(x, &der);
        std::string b64(4 * ((size_t)n + 2) / 3 + 1, '\0');
        int m = EVP_EncodeBlock((unsigned char*)b64.data(), der, n);
        b64.resize((size_t)m);
        OPENSSL_free(der);
        std::cout << "`pragma protect begin_commonblock\n"
                  << "`pragma protect license_proxyenv = \"" << o.pragmas << "\"\n"
                  << "`pragma protect license_certificate\n";
        for (size_t p = 0; p < b64.size(); p += 64) std::cout << b64.substr(p, 64) << "\n";
        std::cout << "`pragma protect end_commonblock\n";
        X509_free(x);
        return 0;
    }
    if (o.key.empty() || o.licenses.empty()) { usage(); return 2; }

    Licenses lic;
    lic.path = o.licenses;
    if (!lic.load()) { std::cerr << "mlicproxy: " << lic.error << "\n"; return 1; }
    struct stat st;
    if (stat(o.licenses.c_str(), &st) == 0) lic.mtime = st.st_mtime;

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    licproxy::tlsSettings(ctx);
    if (SSL_CTX_use_certificate(ctx, x) != 1 || SSL_CTX_use_PrivateKey_file(ctx, o.key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        std::cerr << "mlicproxy: " << o.key << " is not the private key of " << o.cert << "\n";
        return 1;
    }
    X509_free(x);

    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = o.bind.empty() ? AF_INET6 : AF_UNSPEC;
    if (getaddrinfo(o.bind.empty() ? nullptr : o.bind.c_str(), o.port.c_str(), &hints, &res) != 0) {
        hints.ai_family = AF_INET;
        if (getaddrinfo(o.bind.empty() ? nullptr : o.bind.c_str(), o.port.c_str(), &hints, &res) != 0) {
            std::cerr << "mlicproxy: bad -bind / -port\n";
            return 2;
        }
    }
    int ls = -1;
    for (addrinfo* a = res; a && ls < 0; a = a->ai_next) {
        int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        int one = 1, zero = 0;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (a->ai_family == AF_INET6) setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);   // IPv4 too
        if (bind(s, a->ai_addr, a->ai_addrlen) == 0 && listen(s, 64) == 0) ls = s;
        else close(s);
    }
    freeaddrinfo(res);
    if (ls < 0) {
        // No IPv6 on this host: IPv4 only.
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)std::atoi(o.port.c_str()));
        ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (!o.bind.empty()) inet_pton(AF_INET, o.bind.c_str(), &sa.sin_addr);
        if (bind(ls, (sockaddr*)&sa, sizeof sa) != 0 || listen(ls, 64) != 0) {
            std::cerr << "mlicproxy: cannot listen on port " << o.port << ": " << std::strerror(errno) << "\n";
            return 1;
        }
    }
    sockaddr_storage ss{};
    socklen_t sl = sizeof ss;
    getsockname(ls, (sockaddr*)&ss, &sl);
    int port = ntohs(ss.ss_family == AF_INET6 ? ((sockaddr_in6*)&ss)->sin6_port : ((sockaddr_in*)&ss)->sin_port);
    std::cerr << "mlicproxy: listening on port " << port << "\n";
    if (!o.portfile.empty()) {
        std::ofstream pf(o.portfile + ".tmp");
        pf << port << "\n";
        pf.close();
        std::rename((o.portfile + ".tmp").c_str(), o.portfile.c_str());
    }
    garbleLeft = o.testGarble;
    std::signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = accept(ls, nullptr, nullptr);
        if (fd < 0) { if (errno == EINTR) continue; break; }
        std::thread([ctx, fd, &lic, &o] { serve(ctx, fd, lic, o); }).detach();
    }
    return 0;
}
