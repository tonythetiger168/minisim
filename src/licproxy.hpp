// licproxy.hpp - the IEEE 1735-2023 clause 8 license system: the messages
// (8.8), the hTool side that asks an IP author's license proxy over TLS
// (8.4, 8.6, 8.7, 8.9), and the pieces a proxy needs to answer.
//
// A decryption envelope names its proxy in the common block:
//
//   `pragma protect license_proxyenv = "ACME_PROXY"
//   `pragma protect license_certificate
//   <base64 of the proxy's X.509 certificate, the PEM without its BEGIN/END lines>
//
// and a right calls it:   control visibility = license("acme_debug") ? "full" : "none"
//
// $ACME_PROXY is a list "port@host;port@host" (8.7). The tool connects to the
// first pair that gives a secure connection, and the secure connection is one
// whose TLS peer certificate is exactly the envelope's license_certificate
// (no CA and no validity period are checked, 8.3.2). TLS is OpenSSL's.
//
// Messages are DER (8.8): SEQUENCE { INTEGER 3, INTEGER request id, ... }.
// The standard's worked examples write the sequence identifier as 0x10; X.690
// DER writes a constructed sequence as 0x30. Both are read. 0x10 is written
// unless MINISIM_LICENSE_SEQTAG=30 is set.
#pragma once

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace licproxy {

constexpr long kProtocol = 3;   // the proxy protocol version, to match the envelope version (8.8)

// ---------------------------------------------------------------- DER (8.8)

struct Elem {
    enum K { Int, Bool, Str } k = Int;
    long i = 0;
    bool b = false;
    std::string s;
};

inline void derLen(std::string& out, size_t n) {
    if (n < 128) { out += (char)n; return; }
    std::string b;
    for (size_t v = n; v; v >>= 8) b.insert(b.begin(), (char)(v & 0xff));
    out += (char)(0x80 | b.size());
    out += b;
}
inline void derInt(std::string& out, long v) {
    std::string b;
    // Minimal two's complement, most significant byte first.
    for (;;) {
        b.insert(b.begin(), (char)(v & 0xff));
        long rest = v >> 8;
        if ((rest == 0 && !(v & 0x80)) || (rest == -1 && (v & 0x80))) break;
        v = rest;
    }
    out += '\x02';
    derLen(out, b.size());
    out += b;
}
inline void derBool(std::string& out, bool v) {
    out += '\x01';
    out += '\x01';
    out += v ? '\x01' : '\x00';   // the standard's examples write true as 01
}
// IA5String: 7-bit characters only.
inline bool isIa5(std::string_view s) {
    for (unsigned char c : s) if (c > 0x7f) return false;
    return true;
}
inline void derStr(std::string& out, std::string_view s) {
    out += '\x16';
    derLen(out, s.size());
    out += s;
}
inline unsigned char seqTag() {
    const char* e = std::getenv("MINISIM_LICENSE_SEQTAG");
    return e && std::string_view(e) == "30" ? 0x30 : 0x10;
}
inline std::string derSeq(const std::string& body, unsigned char tag = seqTag()) {
    std::string out(1, (char)tag);
    derLen(out, body.size());
    return out + body;
}

// hTool -> proxy (8.8.1): version, request id, the argument of the call.
inline std::string encodeRequest(long id, std::string_view arg, unsigned char tag = seqTag()) {
    std::string b;
    derInt(b, kProtocol);
    derInt(b, id);
    derStr(b, arg);
    return derSeq(b, tag);
}
// proxy -> hTool (8.8.2): version, request id, the license value, then the
// license_string value and an explanatory message, both optional.
inline std::string encodeReply(long id, bool granted, const std::optional<std::string>& str,
                               const std::optional<std::string>& message, unsigned char tag = seqTag()) {
    std::string b;
    derInt(b, kProtocol);
    derInt(b, id);
    derBool(b, granted);
    if (str || message) derStr(b, str.value_or(""));
    if (message) derStr(b, *message);
    return derSeq(b, tag);
}
// Request id 0 (8.8.3): a message that answers no request.
inline std::string encodeInfo(std::string_view message, unsigned char tag = seqTag()) {
    std::string b;
    derInt(b, kProtocol);
    derInt(b, 0);
    derStr(b, message);
    return derSeq(b, tag);
}

enum class Parse { Ok, More, Bad };

// One message from the front of `buf`: Ok sets `used` and `out`; More means
// the message is not complete yet; Bad means it is not a message at all.
inline Parse decode(std::string_view buf, size_t& used, std::vector<Elem>& out) {
    out.clear();
    auto len = [&](size_t& p, size_t& n) -> Parse {
        if (p >= buf.size()) return Parse::More;
        unsigned char c = (unsigned char)buf[p++];
        if (c < 128) { n = c; return Parse::Ok; }
        size_t k = c & 0x7f;
        if (k == 0 || k > 3) return Parse::Bad;   // a message over 16 MB is not one of ours
        if (p + k > buf.size()) return Parse::More;
        n = 0;
        for (size_t i = 0; i < k; i++) n = n << 8 | (unsigned char)buf[p++];
        return Parse::Ok;
    };
    if (buf.empty()) return Parse::More;
    unsigned char tag = (unsigned char)buf[0];
    if (tag != 0x10 && tag != 0x30) return Parse::Bad;
    size_t p = 1, n = 0;
    if (Parse r = len(p, n); r != Parse::Ok) return r;
    if (p + n > buf.size()) return Parse::More;
    size_t end = p + n;
    buf = buf.substr(0, end);   // lengths inside may not run past the sequence
    while (p < end) {
        unsigned char t = (unsigned char)buf[p++];
        size_t m = 0;
        if (len(p, m) != Parse::Ok || p + m > end) return Parse::Bad;
        Elem e;
        if (t == 0x02) {
            if (m == 0 || m > sizeof(long)) return Parse::Bad;
            long v = (buf[p] & 0x80) ? -1 : 0;
            for (size_t i = 0; i < m; i++) v = (long)((unsigned long)v << 8 | (unsigned char)buf[p + i]);
            e.k = Elem::Int;
            e.i = v;
        } else if (t == 0x01) {
            if (m != 1) return Parse::Bad;
            e.k = Elem::Bool;
            e.b = buf[p] != 0;
        } else if (t == 0x16) {
            e.k = Elem::Str;
            e.s = std::string(buf.substr(p, m));
            if (!isIa5(e.s)) return Parse::Bad;
        } else {
            return Parse::Bad;           // only integer, Boolean and IA5String are allowed (8.8)
        }
        p += m;
        out.push_back(std::move(e));
    }
    // At least: version, request id, and one more field.
    if (out.size() < 3 || out[0].k != Elem::Int || out[1].k != Elem::Int) return Parse::Bad;
    used = end;
    return Parse::Ok;
}

// ---------------------------------------------------------------- sockets and TLS

struct Endpoint {
    std::string host, port;
};

// "port@host;port@host" (8.7): no whitespace, pairs separated by semicolons.
// The host is a name, a fully qualified name, or an IPv4 / IPv6 address.
inline std::optional<std::vector<Endpoint>> parseEndpoints(std::string_view v) {
    std::vector<Endpoint> out;
    if (v.empty()) return std::nullopt;
    for (size_t p = 0; p <= v.size();) {
        size_t e = v.find(';', p);
        if (e == std::string_view::npos) e = v.size();
        std::string_view one = v.substr(p, e - p);
        p = e + 1;
        size_t at = one.find('@');
        if (at == std::string_view::npos || at == 0 || at + 1 >= one.size()) return std::nullopt;
        Endpoint ep{std::string(one.substr(at + 1)), std::string(one.substr(0, at))};
        for (char c : ep.port) if (c < '0' || c > '9') return std::nullopt;
        if (ep.port.size() > 5 || std::stol(ep.port) < 1 || std::stol(ep.port) > 65535) return std::nullopt;
        for (char c : ep.host) if (c == ' ' || c == '\t' || c == '@') return std::nullopt;
        if (ep.host.size() > 2 && ep.host.front() == '[' && ep.host.back() == ']')
            ep.host = ep.host.substr(1, ep.host.size() - 2);   // [::1]
        out.push_back(std::move(ep));
    }
    return out;
}

// An environment variable name the common block may use (8.3.1).
inline bool legalEnvName(std::string_view n) {
    if (n.empty() || (n[0] >= '0' && n[0] <= '9')) return false;
    for (char c : n)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

inline void setTimeouts(int fd, int ms) {
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

// A TCP connection within `ms` milliseconds, or -1.
inline int tcpConnect(const Endpoint& ep, int ms) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ep.host.c_str(), ep.port.c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
        int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(s, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            int err = 0;
            socklen_t el = sizeof err;
            if (poll(&p, 1, ms) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) rc = 0;
        }
        if (rc == 0) {
            fcntl(s, F_SETFL, fl);
            setTimeouts(s, ms);
            fd = s;
        } else {
            close(s);
        }
    }
    freeaddrinfo(res);
    return fd;
}

// Settings both ends share (8.7.1): the TLS library picks the version, with
// nothing older than TLS 1.2 accepted and TLS 1.3 supported; sessions are
// never resumed.
inline void tlsSettings(SSL_CTX* ctx) {
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET | SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_num_tickets(ctx, 0);
    // 256-bit keys with SHA-384 for TLS 1.2 (8.7.1's recommendation); TLS 1.3
    // suites are OpenSSL's defaults, AES-256-GCM-SHA384 first.
    SSL_CTX_set_cipher_list(ctx, "ECDHE+AESGCM+AES256:DHE+AESGCM+AES256:ECDHE+CHACHA20");
}

// ---------------------------------------------------------------- the hTool side

struct Reply {
    bool granted = false;       // what a license call returns
    std::string str;            // what a license_string call returns
    std::string message;        // the proxy's explanatory message, if any
    bool irregular = false;     // no secure connection could be had (8.9)
    std::string why;            // ... and why
};

// The proxy of one envelope: license_proxyenv and license_certificate (DER).
struct Spec {
    std::string env;
    std::string certDer;
    bool operator<(const Spec& o) const { return std::tie(env, certDer) < std::tie(o.env, o.certDer); }
};

class Client {
public:
    int timeoutMs = 10000;                                 // 8.7.2: the tool chooses the wait
    std::function<void(const std::string&)> info;          // explanatory and request-id-0 messages

    Client() {
        if (const char* e = std::getenv("MINISIM_LICENSE_TIMEOUT")) {
            int s = std::atoi(e);
            if (s > 0) timeoutMs = s * 1000;
        }
    }
    ~Client() { for (auto& [k, c] : conns) drop(c); }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // The value of a licensing call (8.4). The same call on the same proxy
    // is answered from the first reply (8.6).
    Reply check(const Spec& spec, const std::string& arg) {
        auto key = std::make_pair(spec, arg);
        if (auto it = cache.find(key); it != cache.end()) return it->second;
        Reply r = ask(spec, arg);
        if (!r.irregular) cache.emplace(key, r);
        return r;
    }

    int connections = 0;   // secure connections opened (for tests)
    int requests = 0;      // messages sent

private:
    struct Conn {
        SSL_CTX* ctx = nullptr;
        SSL* ssl = nullptr;
        int fd = -1;
        long nextId = 1;
        std::string buf;
    };
    std::map<Spec, Conn> conns;
    std::map<std::pair<Spec, std::string>, Reply> cache;

    static void drop(Conn& c) {
        if (c.ssl) { SSL_shutdown(c.ssl); SSL_free(c.ssl); }
        if (c.ctx) SSL_CTX_free(c.ctx);
        if (c.fd >= 0) close(c.fd);
        c = Conn{};
        ERR_clear_error();
    }

    // A secure connection: TLS up, and the peer's certificate is the one in
    // the envelope.
    bool open(Conn& c, const Endpoint& ep, const Spec& spec, std::string& why) {
        c.fd = tcpConnect(ep, timeoutMs);
        if (c.fd < 0) { why = "cannot connect to " + ep.port + "@" + ep.host; return false; }
        c.ctx = SSL_CTX_new(TLS_client_method());
        tlsSettings(c.ctx);
        c.ssl = SSL_new(c.ctx);
        SSL_set_fd(c.ssl, c.fd);
        in6_addr scratch;
        if (inet_pton(AF_INET, ep.host.c_str(), &scratch) != 1 && inet_pton(AF_INET6, ep.host.c_str(), &scratch) != 1)
            SSL_set_tlsext_host_name(c.ssl, ep.host.c_str());
        if (SSL_connect(c.ssl) != 1) {
            why = "no TLS connection to " + ep.port + "@" + ep.host;
            drop(c);
            return false;
        }
        X509* peer = SSL_get1_peer_certificate(c.ssl);
        std::string der;
        if (peer) {
            unsigned char* p = nullptr;
            int n = i2d_X509(peer, &p);
            if (n > 0) der.assign((const char*)p, (size_t)n);
            OPENSSL_free(p);
            X509_free(peer);
        }
        if (der.empty() || der.size() != spec.certDer.size() ||
            CRYPTO_memcmp(der.data(), spec.certDer.data(), der.size()) != 0) {
            why = ep.port + "@" + ep.host + " does not hold the envelope's license_certificate";
            drop(c);
            return false;
        }
        connections++;
        return true;
    }

    enum class Io { Ok, Timeout, Garbled };

    // Send one request and wait for its reply on this connection.
    Io exchange(Conn& c, const std::string& arg, Reply& out) {
        long id = c.nextId++;
        std::string msg = encodeRequest(id, arg);
        requests++;
        if (SSL_write(c.ssl, msg.data(), (int)msg.size()) != (int)msg.size()) return Io::Timeout;
        for (;;) {
            size_t used = 0;
            std::vector<Elem> e;
            Parse p = decode(c.buf, used, e);
            if (p == Parse::Bad) return Io::Garbled;
            if (p == Parse::More) {
                char tmp[4096];
                int n = SSL_read(c.ssl, tmp, sizeof tmp);
                if (n <= 0) return Io::Timeout;
                c.buf.append(tmp, (size_t)n);
                continue;
            }
            c.buf.erase(0, used);
            if (e[0].i != kProtocol) return Io::Garbled;
            if (e[1].i == 0) {
                // 8.8.3: exactly one argument, an IA5String; nothing is sent back.
                if (e.size() != 3 || e[2].k != Elem::Str) return Io::Garbled;
                if (info) info(e[2].s);
                continue;
            }
            // A reply to anything but the one request in flight is a
            // duplicate or unknown request id (8.7.1).
            if (e[1].i != id) return Io::Garbled;
            if (e.size() > 5 || e[2].k != Elem::Bool) return Io::Garbled;
            if (e.size() > 3 && e[3].k != Elem::Str) return Io::Garbled;
            if (e.size() > 4 && e[4].k != Elem::Str) return Io::Garbled;
            out = Reply{};
            out.granted = e[2].b;
            if (e.size() > 3) out.str = e[3].s;      // absent: the zero-length string (8.8.2)
            if (e.size() > 4) {
                out.message = e[4].s;
                if (info && !out.message.empty()) info(out.message);
            }
            return Io::Ok;
        }
    }

    Reply irregular(const std::string& why) {
        Reply r;
        r.irregular = true;
        r.why = why;
        return r;
    }

    Reply ask(const Spec& spec, const std::string& arg) {
        if (spec.env.empty()) return irregular("the envelope has no license_proxyenv");
        if (!legalEnvName(spec.env)) return irregular("license_proxyenv \"" + spec.env + "\" is not an environment variable name");
        if (spec.certDer.empty()) return irregular("the envelope has no license_certificate");
        {
            const unsigned char* p = (const unsigned char*)spec.certDer.data();
            X509* x = d2i_X509(nullptr, &p, (long)spec.certDer.size());
            if (!x) { ERR_clear_error(); return irregular("the envelope's license_certificate is not an X.509 certificate"); }
            X509_free(x);
        }
        if (!isIa5(arg)) return irregular("the licensing call's argument is not an IA5String");

        // An open connection to this proxy is reused (8.3).
        Conn& c = conns[spec];
        Reply out;
        if (c.ssl) {
            Io r = exchange(c, arg, out);
            if (r == Io::Ok) return out;
            drop(c);   // broken or garbled: negotiate again below, never resume
        }
        const char* val = std::getenv(spec.env.c_str());
        if (!val || !*val) return irregular("environment variable " + spec.env + " is not set");
        auto eps = parseEndpoints(val);
        if (!eps) return irregular("environment variable " + spec.env + " is not a port@host[;port@host...] list");
        std::string why;
        for (const Endpoint& ep : *eps) {
            // One more negotiation on the same pair after a garbled reply,
            // with the pending request sent again (8.7.1).
            for (int attempt = 0; attempt < 2; attempt++) {
                if (!open(c, ep, spec, why)) break;
                Io r = exchange(c, arg, out);
                if (r == Io::Ok) return out;
                drop(c);
                why = r == Io::Garbled ? "garbled reply from " + ep.port + "@" + ep.host
                                       : "no reply from " + ep.port + "@" + ep.host;
                if (r == Io::Timeout) break;   // 8.7.2: reject this pair, go to the next
            }
        }
        return irregular(why.empty() ? "no license proxy in " + spec.env : why);
    }
};

}  // namespace licproxy
