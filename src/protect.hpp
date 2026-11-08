// protect.hpp - protected envelopes (IEEE 1800-2023 clause 34, the IEEE 1735
// encryption scheme): `pragma protect begin/end regions are encrypted into
// begin_protected/end_protected envelopes, and envelopes are decrypted back
// into source text for the compiler.
//
// Envelope layout written by encryptSource (one key_block per recipient):
//
//   `pragma protect begin_protected
//   `pragma protect encrypt_agent="msimprotect", encrypt_agent_info="minisim"
//   `pragma protect digest_method="sha256"
//   `pragma protect key_keyowner="Acme", key_keyname="acme-2026", key_method="rsa"
//   `pragma protect encoding=(enctype="base64", line_length=76, bytes=294)
//   `pragma protect key_public_key
//   <the recipient's public key, DER SubjectPublicKeyInfo>
//   `pragma protect encoding=(enctype="base64", line_length=76, bytes=256)
//   `pragma protect key_block
//   <session key, RSA (PKCS#1 v1.5) encrypted with the recipient's public key>
//   `pragma protect encoding=(enctype="base64", line_length=76, bytes=48)
//   `pragma protect digest_block
//   <IV + AES-CBC(session key, sha256(session key))>          (34.5.22)
//   `pragma protect data_method="aes256-cbc"
//   `pragma protect encoding=(enctype="base64", line_length=76, bytes=N)
//   `pragma protect data_block
//   <IV + AES-CBC(session key, source text)>                  (34.5.15)
//   `pragma protect encoding=(enctype="base64", line_length=76, bytes=48)
//   `pragma protect digest_block
//   <IV + AES-CBC(session key, sha256(source text))>
//   `pragma protect end_protected
//   `pragma reset protect
//
// The session key is fresh random bytes for every envelope (34.3.1), every IV
// is random, and the crypto is OpenSSL's. Error messages name the file, the
// line and the cleartext envelope keywords (key owner and name) but never any
// decrypted text.
#pragma once

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace protect {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- encodings (34.5.9)

inline std::string wrap(const std::string& s, int lineLen) {
    if (lineLen <= 0) return s + "\n";
    std::string out;
    for (size_t i = 0; i < s.size(); i += (size_t)lineLen) out += s.substr(i, (size_t)lineLen) + "\n";
    return out;
}

inline std::string base64Encode(std::string_view bin, int lineLen) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    size_t i = 0;
    for (; i + 2 < bin.size(); i += 3) {
        uint32_t v = (uint8_t)bin[i] << 16 | (uint8_t)bin[i + 1] << 8 | (uint8_t)bin[i + 2];
        s += t[v >> 18]; s += t[v >> 12 & 63]; s += t[v >> 6 & 63]; s += t[v & 63];
    }
    if (i + 1 == bin.size()) {
        uint32_t v = (uint8_t)bin[i] << 16;
        s += t[v >> 18]; s += t[v >> 12 & 63]; s += "==";
    } else if (i + 2 == bin.size()) {
        uint32_t v = (uint8_t)bin[i] << 16 | (uint8_t)bin[i + 1] << 8;
        s += t[v >> 18]; s += t[v >> 12 & 63]; s += t[v >> 6 & 63]; s += '=';
    }
    return wrap(s, lineLen);
}

inline std::string base64Decode(std::string_view text) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        else throw Error("bad character in base64 block");
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out += (char)(acc >> bits & 0xff); }
    }
    return out;
}

// Historical uuencode (IEEE Std 1003.1): each line is a length character
// (32 + n) and n bytes in groups of three as four characters 32 + 6 bits,
// with 0 written as '`'. "begin"/"end" framing lines are accepted on input.
inline std::string uuEncode(std::string_view bin) {
    auto ch = [](uint32_t v) { return v ? (char)(32 + v) : '`'; };
    std::string out;
    for (size_t i = 0; i < bin.size(); i += 45) {
        size_t n = std::min<size_t>(45, bin.size() - i);
        out += ch((uint32_t)n);
        for (size_t j = 0; j < n; j += 3) {
            uint32_t v = 0;
            for (size_t k = 0; k < 3; k++) v = v << 8 | (j + k < n ? (uint8_t)bin[i + j + k] : 0);
            out += ch(v >> 18); out += ch(v >> 12 & 63); out += ch(v >> 6 & 63); out += ch(v & 63);
        }
        out += '\n';
    }
    return out + "`\n";
}

inline std::string uuDecode(std::string_view text) {
    std::string out;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string_view line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.substr(0, 6) == "begin " || line == "end") continue;
        size_t n = (size_t)((line[0] - 32) & 63);
        if (n == 0) break;
        size_t got = 0;
        for (size_t i = 1; got < n; i += 4) {
            uint32_t v = 0;
            for (size_t k = 0; k < 4; k++) {
                char c = i + k < line.size() ? line[i + k] : '`';
                if (c < 32 || c > 96) throw Error("bad character in uuencode block");
                v = v << 6 | (uint32_t)((c - 32) & 63);
            }
            for (int k = 2; k >= 0 && got < n; k--, got++) out += (char)(v >> (8 * k) & 0xff);
        }
    }
    return out;
}

// ---------------------------------------------------------------- pragma expressions (22.11)

struct PExpr;
struct PVal {
    enum Kind { None, Str, Num, Ident, List } kind = None;
    std::string s;               // Str / Num / Ident
    std::vector<PExpr> list;     // List
};
struct PExpr {
    std::string key;             // empty when the expression is a bare value
    PVal val;
};

class ExprParser {
public:
    explicit ExprParser(std::string_view s) : s(s) {}
    std::vector<PExpr> all() {
        std::vector<PExpr> r;
        skip();
        if (p >= s.size()) return r;
        r.push_back(expr());
        for (skip(); p < s.size() && s[p] == ','; skip()) { p++; r.push_back(expr()); }
        skip();
        if (p < s.size() && s.substr(p, 2) != "//") throw Error("bad `pragma protect expression");
        return r;
    }

private:
    std::string_view s;
    size_t p = 0;
    void skip() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')) p++; }
    static bool idc(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$' || c == '-' || c == '.'; }
    PExpr expr() {
        skip();
        PExpr e;
        if (p < s.size() && (std::isalpha((unsigned char)s[p]) || s[p] == '_')) {
            size_t b = p;
            while (p < s.size() && idc(s[p])) p++;
            std::string id(s.substr(b, p - b));
            skip();
            if (p < s.size() && s[p] == '=') { p++; e.key = id; e.val = value(); }
            else e.key = id;   // a bare keyword: begin, end, data_block, ...
            return e;
        }
        e.val = value();
        return e;
    }
    PVal value() {
        skip();
        PVal v;
        if (p >= s.size()) throw Error("bad `pragma protect expression");
        if (s[p] == '"') {
            v.kind = PVal::Str;
            for (p++; p < s.size() && s[p] != '"'; p++) {
                if (s[p] == '\\' && p + 1 < s.size()) p++;
                v.s += s[p];
            }
            if (p >= s.size()) throw Error("unterminated string in `pragma protect");
            p++;
        } else if (s[p] == '(') {
            v.kind = PVal::List;
            p++;
            skip();
            if (p < s.size() && s[p] == ')') { p++; return v; }
            v.list.push_back(expr());
            for (skip(); p < s.size() && s[p] == ','; skip()) { p++; v.list.push_back(expr()); }
            if (p >= s.size() || s[p] != ')') throw Error("missing ) in `pragma protect");
            p++;
        } else if (std::isdigit((unsigned char)s[p])) {
            v.kind = PVal::Num;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '\'' || s[p] == '_')) v.s += s[p++];
        } else if (std::isalpha((unsigned char)s[p]) || s[p] == '_') {
            v.kind = PVal::Ident;
            while (p < s.size() && idc(s[p])) v.s += s[p++];
        } else {
            throw Error("bad `pragma protect expression");
        }
        return v;
    }
};

inline std::string quote(std::string_view s) {
    std::string r = "\"";
    for (char c : s) { if (c == '"' || c == '\\') r += '\\'; r += c; }
    return r + "\"";
}

// ---------------------------------------------------------------- source scanning

// A `pragma directive found in source text: [start, lineEnd) is its text and
// next is the first character of the following line.
struct Directive {
    size_t start = 0, lineEnd = 0, next = 0;
    int line = 0;
    enum Kind { Protect, ResetProtect, Other } kind = Other;
    std::vector<PExpr> exprs;   // Protect only
};

// Walks a source buffer directive by directive, skipping comments and string
// literals, so a `pragma inside either is not taken for a directive.
class Scanner {
public:
    explicit Scanner(std::string_view t) : text(t) {}
    std::string_view text;
    size_t pos = 0;
    int line = 1;

    int lineAt(size_t to) {
        for (; lpos < to && lpos < text.size(); lpos++) if (text[lpos] == '\n') line++;
        return line;
    }

    // The next `pragma directive at or after pos, or nullopt at the end.
    std::optional<Directive> next() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
                size_t e = text.find('\n', pos);
                pos = e == std::string_view::npos ? text.size() : e;
            } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '*') {
                size_t e = text.find("*/", pos + 2);
                pos = e == std::string_view::npos ? text.size() : e + 2;
            } else if (c == '"') {
                for (pos++; pos < text.size() && text[pos] != '"' && text[pos] != '\n'; pos++)
                    if (text[pos] == '\\') pos++;
                pos++;
            } else if (c == '`' && text.substr(pos, 7) == "`pragma" &&
                       (pos + 7 >= text.size() || !isIdent(text[pos + 7]))) {
                return directive();
            } else {
                pos++;
            }
        }
        return std::nullopt;
    }

    // Encoded block text that follows a key_block / data_block / digest_block /
    // *_public_key directive, starting at pos. For enctype raw the block is
    // exactly `bytes` bytes; otherwise it runs to the next `pragma line.
    std::string_view block(const std::string& enctype, long bytes) {
        size_t b = pos;
        if (enctype == "raw") {
            if (bytes < 0) throw Error("raw encoding needs bytes=");
            if (b + (size_t)bytes > text.size()) throw Error("truncated raw block");
            pos = b + (size_t)bytes;
            if (pos < text.size() && text[pos] == '\r') pos++;
            if (pos < text.size() && text[pos] == '\n') pos++;
            return text.substr(b, (size_t)bytes);
        }
        size_t p = b;
        while (p < text.size()) {
            size_t q = p;
            while (q < text.size() && (text[q] == ' ' || text[q] == '\t')) q++;
            if (text.substr(q, 7) == "`pragma") break;
            size_t e = text.find('\n', p);
            p = e == std::string_view::npos ? text.size() : e + 1;
        }
        pos = p;
        return text.substr(b, p - b);
    }

private:
    size_t lpos = 0;
    static bool isIdent(char c) { return std::isalnum((unsigned char)c) || c == '_' || c == '$'; }
    Directive directive() {
        Directive d;
        d.start = pos;
        d.line = lineAt(pos);
        size_t e = text.find('\n', pos);
        d.lineEnd = e == std::string_view::npos ? text.size() : e;
        d.next = e == std::string_view::npos ? text.size() : e + 1;
        std::string_view body = text.substr(pos + 7, d.lineEnd - pos - 7);
        size_t i = 0;
        auto word = [&]() {
            while (i < body.size() && (body[i] == ' ' || body[i] == '\t')) i++;
            size_t b = i;
            while (i < body.size() && isIdent(body[i])) i++;
            return body.substr(b, i - b);
        };
        std::string_view w = word();
        if (w == "protect") {
            d.kind = Directive::Protect;
            d.exprs = ExprParser(body.substr(i)).all();
        } else if (w == "reset") {
            for (std::string_view n = word(); !n.empty(); n = word())
                if (n == "protect") d.kind = Directive::ResetProtect;
        }
        pos = d.next;
        return d;
    }
};

// ---------------------------------------------------------------- OpenSSL helpers

using PKey = std::shared_ptr<EVP_PKEY>;
inline PKey own(EVP_PKEY* k) { return PKey(k, EVP_PKEY_free); }

inline std::string sslError() {
    unsigned long e = ERR_get_error();
    ERR_clear_error();
    if (!e) return "OpenSSL error";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

// IEEE 1800-2023 Table 34-3 identifiers -> OpenSSL cipher names.
inline const char* cipherName(std::string_view m) {
    if (m == "aes128-cbc") return "AES-128-CBC";
    if (m == "aes192-cbc") return "AES-192-CBC";
    if (m == "aes256-cbc") return "AES-256-CBC";
    if (m == "des-cbc") return "DES-CBC";
    if (m == "3des-cbc") return "DES-EDE3-CBC";
    if (m == "blowfish-cbc") return "BF-CBC";
    if (m == "cast128-cbc") return "CAST5-CBC";
    return nullptr;
}

// Table 34-4 identifiers (plus the SHA-2 family) -> OpenSSL digest names.
inline const char* digestName(std::string_view m) {
    if (m == "sha1") return "SHA1";
    if (m == "md5") return "MD5";
    if (m == "sha256") return "SHA256";
    if (m == "sha384") return "SHA384";
    if (m == "sha512") return "SHA512";
    if (m == "ripemd-160") return "RIPEMD160";
    return nullptr;
}

struct CipherFree { void operator()(EVP_CIPHER* c) const { EVP_CIPHER_free(c); } };
using Cipher = std::unique_ptr<EVP_CIPHER, CipherFree>;

// DES, Blowfish and CAST live in OpenSSL 3's legacy provider.
inline Cipher fetchCipher(std::string_view method) {
    const char* n = cipherName(method);
    if (!n) throw Error("unsupported data_method \"" + std::string(method) + "\"");
    EVP_CIPHER* c = EVP_CIPHER_fetch(nullptr, n, nullptr);
    if (!c) {
        ERR_clear_error();
        static OSSL_PROVIDER* legacy = OSSL_PROVIDER_load(nullptr, "legacy");
        static OSSL_PROVIDER* deflt = legacy ? OSSL_PROVIDER_load(nullptr, "default") : nullptr;
        (void)deflt;
        c = EVP_CIPHER_fetch(nullptr, n, nullptr);
    }
    if (!c) { ERR_clear_error(); throw Error("data_method \"" + std::string(method) + "\" is not available in this OpenSSL"); }
    return Cipher(c);
}

inline std::string randomBytes(size_t n) {
    std::string s(n, '\0');
    if (RAND_bytes((unsigned char*)s.data(), (int)n) != 1) throw Error("random generator failed: " + sslError());
    return s;
}

// CBC encrypt with PKCS#7 padding; the IV block is prepended (34.5.15).
inline std::string cbcEncrypt(std::string_view method, const std::string& key, std::string_view plain) {
    Cipher c = fetchCipher(method);
    int ivLen = EVP_CIPHER_get_iv_length(c.get());
    if ((int)key.size() != EVP_CIPHER_get_key_length(c.get())) throw Error("wrong session key length");
    std::string iv = randomBytes((size_t)ivLen);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::string out = iv;
    out.resize(iv.size() + plain.size() + (size_t)EVP_CIPHER_get_block_size(c.get()) + 16);
    int n1 = 0, n2 = 0;
    bool ok = EVP_EncryptInit_ex2(ctx, c.get(), (const unsigned char*)key.data(), (const unsigned char*)iv.data(), nullptr) == 1 &&
              EVP_EncryptUpdate(ctx, (unsigned char*)out.data() + iv.size(), &n1, (const unsigned char*)plain.data(), (int)plain.size()) == 1 &&
              EVP_EncryptFinal_ex(ctx, (unsigned char*)out.data() + iv.size() + n1, &n2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) throw Error("encryption failed: " + sslError());
    out.resize(iv.size() + (size_t)n1 + (size_t)n2);
    return out;
}

// CBC decrypt of IV + ciphertext; nullopt when the key is wrong (bad padding).
inline std::optional<std::string> cbcDecrypt(std::string_view method, const std::string& key, std::string_view data) {
    Cipher c = fetchCipher(method);
    size_t ivLen = (size_t)EVP_CIPHER_get_iv_length(c.get());
    if ((int)key.size() != EVP_CIPHER_get_key_length(c.get())) return std::nullopt;
    size_t bs = (size_t)EVP_CIPHER_get_block_size(c.get());
    if (data.size() < ivLen + bs || (data.size() - ivLen) % bs) return std::nullopt;
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    std::string out(data.size(), '\0');
    int n1 = 0, n2 = 0;
    bool ok = EVP_DecryptInit_ex2(ctx, c.get(), (const unsigned char*)key.data(), (const unsigned char*)data.data(), nullptr) == 1 &&
              EVP_DecryptUpdate(ctx, (unsigned char*)out.data(), &n1, (const unsigned char*)data.data() + ivLen, (int)(data.size() - ivLen)) == 1 &&
              EVP_DecryptFinal_ex(ctx, (unsigned char*)out.data() + n1, &n2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    ERR_clear_error();
    if (!ok) { OPENSSL_cleanse(out.data(), out.size()); return std::nullopt; }
    out.resize((size_t)n1 + (size_t)n2);
    return out;
}

inline std::string digest(std::string_view method, std::string_view data) {
    const char* n = digestName(method);
    if (!n) throw Error("unsupported digest_method \"" + std::string(method) + "\"");
    EVP_MD* md = EVP_MD_fetch(nullptr, n, nullptr);
    if (!md) { ERR_clear_error(); throw Error("digest_method \"" + std::string(method) + "\" is not available in this OpenSSL"); }
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned len = 0;
    bool ok = EVP_Digest(data.data(), data.size(), buf, &len, md, nullptr) == 1;
    EVP_MD_free(md);
    if (!ok) throw Error("digest failed: " + sslError());
    return std::string((const char*)buf, len);
}

// RSA with PKCS#1 v1.5 padding: key_method="rsa" (Table 34-3, RFC 2437).
inline std::string rsaEncrypt(EVP_PKEY* pub, std::string_view plain) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(pub, nullptr);
    size_t len = 0;
    std::string out;
    bool ok = ctx && EVP_PKEY_encrypt_init(ctx) == 1 &&
              EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1 &&
              EVP_PKEY_encrypt(ctx, nullptr, &len, (const unsigned char*)plain.data(), plain.size()) == 1;
    if (ok) {
        out.resize(len);
        ok = EVP_PKEY_encrypt(ctx, (unsigned char*)out.data(), &len, (const unsigned char*)plain.data(), plain.size()) == 1;
        out.resize(len);
    }
    EVP_PKEY_CTX_free(ctx);
    if (!ok) throw Error("RSA key encryption failed: " + sslError());
    return out;
}

inline std::optional<std::string> rsaDecrypt(EVP_PKEY* priv, std::string_view data) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(priv, nullptr);
    size_t len = 0;
    std::string out;
    bool ok = ctx && EVP_PKEY_decrypt_init(ctx) == 1 &&
              EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1 &&
              EVP_PKEY_decrypt(ctx, nullptr, &len, (const unsigned char*)data.data(), data.size()) == 1;
    if (ok) {
        out.resize(len);
        ok = EVP_PKEY_decrypt(ctx, (unsigned char*)out.data(), &len, (const unsigned char*)data.data(), data.size()) == 1;
        out.resize(len);
    }
    EVP_PKEY_CTX_free(ctx);
    ERR_clear_error();
    if (!ok) return std::nullopt;
    return out;
}

inline std::string publicDer(EVP_PKEY* k) {
    unsigned char* der = nullptr;
    int n = i2d_PUBKEY(k, &der);
    if (n <= 0) throw Error("cannot encode public key: " + sslError());
    std::string s((const char*)der, (size_t)n);
    OPENSSL_free(der);
    return s;
}

inline std::string readFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw Error("cannot read " + path);
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

// A passphrase-protected PEM is refused instead of prompting on the terminal.
static int noPassphrase(char*, int, int, void*) { return -1; }

inline PKey loadPrivateKey(const std::string& path) {
    std::string pem = readFile(path);
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    EVP_PKEY* k = PEM_read_bio_PrivateKey(bio, nullptr, noPassphrase, nullptr);
    BIO_free(bio);
    OPENSSL_cleanse(pem.data(), pem.size());
    if (!k) { ERR_clear_error(); throw Error(path + ": not an unencrypted PEM private key"); }
    if (EVP_PKEY_get_base_id(k) != EVP_PKEY_RSA) { EVP_PKEY_free(k); throw Error(path + ": not an RSA key"); }
    return own(k);
}

// A public key PEM, or the public half of a private key PEM.
inline PKey loadPublicKey(const std::string& path) {
    std::string pem = readFile(path);
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    EVP_PKEY* k = PEM_read_bio_PUBKEY(bio, nullptr, noPassphrase, nullptr);
    BIO_free(bio);
    if (!k) {
        ERR_clear_error();
        bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
        k = PEM_read_bio_PrivateKey(bio, nullptr, noPassphrase, nullptr);
        BIO_free(bio);
    }
    OPENSSL_cleanse(pem.data(), pem.size());
    if (!k) { ERR_clear_error(); throw Error(path + ": not a PEM public key"); }
    if (EVP_PKEY_get_base_id(k) != EVP_PKEY_RSA) { EVP_PKEY_free(k); throw Error(path + ": not an RSA key"); }
    return own(k);
}

inline PKey publicFromDer(const std::string& der) {
    const unsigned char* p = (const unsigned char*)der.data();
    EVP_PKEY* k = d2i_PUBKEY(nullptr, &p, (long)der.size());
    if (!k) { ERR_clear_error(); throw Error("bad key_public_key"); }
    return own(k);
}

// ---------------------------------------------------------------- keyword state

// The protect keywords in effect at a point of the source (34.4: their scope
// is lexical). Values are kept as their source spelling.
struct State {
    std::map<std::string, PVal> kw;
    std::string str(const std::string& k, const std::string& def = "") const {
        auto it = kw.find(k);
        return it == kw.end() || it->second.kind == PVal::List ? def : it->second.s;
    }
    std::string enctype() const {
        auto it = kw.find("encoding");
        if (it == kw.end()) return "raw";
        for (auto& e : it->second.list) if (e.key == "enctype") return e.val.s;
        return "raw";
    }
    long bytes() const {
        auto it = kw.find("encoding");
        if (it == kw.end()) return -1;
        for (auto& e : it->second.list)
            if (e.key == "bytes") { try { return std::stol(e.val.s); } catch (...) { return -1; } }
        return -1;
    }
};

inline std::string decodeBlock(std::string_view enc, const State& st) {
    std::string t = st.enctype();
    std::string bin;
    if (t == "base64") bin = base64Decode(enc);
    else if (t == "uuencode") bin = uuDecode(enc);
    else if (t == "raw") bin = std::string(enc);
    else throw Error("unsupported encoding enctype=\"" + t + "\"");
    long n = st.bytes();
    if (n >= 0 && (size_t)n != bin.size()) throw Error("block length does not match encoding bytes=");
    return bin;
}

inline std::string encodeBlock(std::string_view bin, const std::string& enctype, int lineLen) {
    if (enctype == "base64") return base64Encode(bin, lineLen);
    if (enctype == "uuencode") return uuEncode(bin);
    throw Error("unsupported output encoding \"" + enctype + "\" (use base64 or uuencode)");
}

inline std::string encodingDirective(const std::string& enctype, int lineLen, size_t bytes) {
    std::string s = "`pragma protect encoding=(enctype=" + quote(enctype);
    if (enctype == "base64") s += ", line_length=" + std::to_string(lineLen);
    return s + ", bytes=" + std::to_string(bytes) + ")\n";
}

// Keywords whose directive is followed by an encoded block on the next lines.
inline bool takesBlock(const std::string& k) {
    return k == "key_block" || k == "data_block" || k == "digest_block" || k == "key_public_key" ||
           k == "data_public_key" || k == "digest_public_key" || k == "data_decrypt_key" || k == "digest_decrypt_key";
}

// ---------------------------------------------------------------- decryption (34.3.2)

struct PrivateKey {
    std::string keyname;   // empty: any key_keyname
    std::string path;
    PKey key;
    std::string pubDer;
};

class Keyring {
public:
    // "<pem>" or "<key_keyname>=<pem>".
    void add(const std::string& spec) {
        PrivateKey k;
        size_t eq = spec.find('=');
        k.path = spec;
        if (eq != std::string::npos && eq > 0) {
            k.keyname = spec.substr(0, eq);
            k.path = spec.substr(eq + 1);
        }
        k.key = loadPrivateKey(k.path);
        k.pubDer = publicDer(k.key.get());
        keys.push_back(std::move(k));
    }
    void add(PKey key, std::string keyname = "") {
        PrivateKey k;
        k.keyname = std::move(keyname);
        k.pubDer = publicDer(key.get());
        k.key = std::move(key);
        keys.push_back(std::move(k));
    }
    bool empty() const { return keys.empty(); }
    std::vector<PrivateKey> keys;
};

struct Decrypted {
    std::string text;
    std::vector<std::pair<size_t, size_t>> ranges;   // [begin, end) offsets of decrypted text in `text`
    int envelopes = 0;
};

namespace detail {

struct Block {
    State st;                 // keywords in effect at the block
    std::string bin;          // decoded bytes
    std::optional<std::string> digest;   // the digest_block that follows it, decoded
    std::string digestMethod;
};

inline std::string where(std::string_view file, int line) {
    return std::string(file) + ":" + std::to_string(line) + ": ";
}

inline bool digestMatches(const Block& b, const std::string& sessionKey, std::string_view dataMethod,
                          std::string_view plain) {
    if (!b.digest) return true;
    std::string method = b.st.str("digest_key_method", std::string(dataMethod));
    auto d = cbcDecrypt(method, sessionKey, *b.digest);
    if (!d) return false;
    std::string want = digest(b.digestMethod, plain);
    bool ok = d->size() == want.size() && CRYPTO_memcmp(d->data(), want.data(), want.size()) == 0;
    return ok;
}

inline void decryptInto(std::string_view text, const Keyring& ring, std::string_view file, int depth,
                        Decrypted& out, bool top);

// The envelope that starts after a begin_protected directive; sc.pos is just
// past that directive's line. Returns the cleartext.
inline std::string envelope(Scanner& sc, State st, const Keyring& ring, std::string_view file, int beginLine) {
    std::vector<Block> keys;
    std::optional<Block> data;
    Block* last = nullptr;
    bool closed = false;
    while (auto d = sc.next()) {
        if (d->kind == Directive::ResetProtect) { st.kw.clear(); continue; }
        if (d->kind != Directive::Protect) continue;
        for (auto& e : d->exprs) {
            if (e.key == "end_protected") { closed = true; break; }
            if (e.key == "begin_protected") throw Error(where(file, d->line) + "begin_protected inside an envelope's keywords");
            if (e.key == "reset") { st.kw.clear(); continue; }
            if (takesBlock(e.key)) {
                std::string bin = decodeBlock(sc.block(st.enctype(), st.bytes()), st);
                if (e.key == "key_block") {
                    keys.push_back({st, std::move(bin), std::nullopt, ""});
                    last = &keys.back();
                    st.kw.erase("key_public_key");   // a public key belongs to one key_block
                } else if (e.key == "data_block") {
                    data = Block{st, std::move(bin), std::nullopt, ""};
                    last = &*data;
                } else if (e.key == "digest_block") {
                    if (!last) throw Error(where(file, d->line) + "digest_block without a key_block or data_block before it");
                    last->digest = std::move(bin);
                    last->digestMethod = st.str("digest_method", "sha1");
                    last = nullptr;
                } else if (e.key == "key_public_key") {
                    PVal v;
                    v.kind = PVal::Str;
                    v.s = std::move(bin);
                    st.kw["key_public_key"] = std::move(v);
                }
                // data_public_key, *_decrypt_key: read and not used.
                continue;
            }
            if (!e.key.empty()) st.kw[e.key] = e.val;
        }
        if (closed) break;
    }
    if (!closed) throw Error(where(file, beginLine) + "begin_protected without end_protected");
    if (!data) throw Error(where(file, beginLine) + "protected envelope has no data_block");

    std::string dataMethod = data->st.str("data_method");
    if (dataMethod.empty()) throw Error(where(file, beginLine) + "protected envelope has no data_method");
    Cipher c = fetchCipher(dataMethod);
    size_t keyLen = (size_t)EVP_CIPHER_get_key_length(c.get());

    // Each key_block a held key can open, in order; the first session key that
    // passes the digests and the padding check wins.
    std::string tried;
    for (auto& kb : keys) {
        std::string owner = kb.st.str("key_keyowner"), name = kb.st.str("key_keyname");
        std::string method = kb.st.str("key_method", "rsa");
        if (!tried.empty()) tried += ", ";
        tried += "key_keyowner=" + quote(owner) + " key_keyname=" + quote(name);
        if (method != "rsa") continue;
        auto pub = kb.st.kw.find("key_public_key");
        for (auto& k : ring.keys) {
            if (pub != kb.st.kw.end()) {
                if (pub->second.s != k.pubDer) continue;
            } else if (!k.keyname.empty() && k.keyname != name) {
                continue;
            }
            auto sk = rsaDecrypt(k.key.get(), kb.bin);
            if (!sk || sk->size() != keyLen) continue;
            if (!digestMatches(kb, *sk, dataMethod, *sk)) continue;
            auto plain = cbcDecrypt(dataMethod, *sk, data->bin);
            if (!plain) { OPENSSL_cleanse(sk->data(), sk->size()); continue; }
            bool dataOk = digestMatches(*data, *sk, dataMethod, *plain);
            OPENSSL_cleanse(sk->data(), sk->size());
            if (!dataOk) {
                OPENSSL_cleanse(plain->data(), plain->size());
                throw Error(where(file, beginLine) + "data_block digest does not match: the envelope was modified");
            }
            return std::move(*plain);
        }
    }
    throw Error(where(file, beginLine) + "no key for this protected envelope (" +
                (tried.empty() ? std::string("it has no key_block") : tried) + "); pass the private key with -keyfile");
}

// Apply one keyword outside an envelope; reads the block a *_public_key
// directive carries so the scanner stays in step.
inline void applyKeyword(Scanner& sc, State& st, const PExpr& e) {
    if (e.key == "reset") { st.kw.clear(); return; }
    if (e.key == "key_public_key" || e.key == "data_public_key" || e.key == "digest_public_key" ||
        e.key == "data_decrypt_key" || e.key == "digest_decrypt_key") {
        PVal v;
        v.kind = PVal::Str;
        v.s = decodeBlock(sc.block(st.enctype(), st.bytes()), st);
        st.kw[e.key] = std::move(v);
        return;
    }
    if (!e.key.empty()) st.kw[e.key] = e.val;
}

inline void decryptInto(std::string_view text, const Keyring& ring, std::string_view file, int depth,
                        Decrypted& out, bool top) {
    Scanner sc(text);
    State st;
    size_t copied = 0;
    while (auto d = sc.next()) {
        if (d->kind == Directive::ResetProtect) { st.kw.clear(); continue; }
        if (d->kind != Directive::Protect) continue;
        size_t bi = d->exprs.size();
        for (size_t i = 0; i < d->exprs.size(); i++)
            if (d->exprs[i].key == "begin_protected") { bi = i; break; }
        for (size_t i = 0; i < bi; i++) {
            const PExpr& e = d->exprs[i];
            if (e.key == "end_protected") throw Error(where(file, d->line) + "end_protected without begin_protected");
            if (e.key == "key_block" || e.key == "data_block" || e.key == "digest_block")
                throw Error(where(file, d->line) + e.key + " outside a begin_protected envelope");
            applyKeyword(sc, st, e);
        }
        if (bi == d->exprs.size()) continue;
        if (depth >= 16) throw Error(where(file, d->line) + "protected envelopes nested more than 16 deep");
        State env = st;
        for (size_t i = bi + 1; i < d->exprs.size(); i++)
            if (!d->exprs[i].key.empty() && !takesBlock(d->exprs[i].key)) env.kw[d->exprs[i].key] = d->exprs[i].val;
        out.text.append(text.substr(copied, d->start - copied));
        std::string plain = envelope(sc, env, ring, file, d->line);
        Decrypted inner;
        decryptInto(plain, ring, file, depth + 1, inner, false);
        OPENSSL_cleanse(plain.data(), plain.size());
        size_t b = out.text.size();
        out.text += inner.text;
        OPENSSL_cleanse(inner.text.data(), inner.text.size());
        if (!out.text.empty() && out.text.back() != '\n') out.text += '\n';
        out.ranges.push_back({b, out.text.size()});
        out.envelopes += 1 + inner.envelopes;
        copied = sc.pos;
        // Line numbers after the envelope are those of the file as written (22.12).
        if (top) {
            int line = sc.lineAt(copied);
            out.text += "`line " + std::to_string(line) + " " + quote(file) + " 0\n";
        }
    }
    out.text.append(text.substr(copied));
}

}  // namespace detail

// Does the text contain a decryption envelope? (A cheap test before decrypting.)
inline bool hasEnvelope(std::string_view text) { return text.find("begin_protected") != std::string_view::npos; }

// Replace every decryption envelope in `text` by its cleartext (34.3.2),
// including envelopes nested in decrypted text. `file` is used in messages
// and in the `line directive that restores line numbers after an envelope.
inline Decrypted decryptSource(std::string_view text, const Keyring& ring, std::string_view file) {
    Decrypted out;
    detail::decryptInto(text, ring, file, 0, out, true);
    return out;
}

// ---------------------------------------------------------------- encryption (34.3.1)

struct Recipient {
    std::string owner, name;
    PKey pub;
};

struct EncryptOptions {
    std::vector<Recipient> recipients;     // a key_block for each
    std::string dataMethod = "aes256-cbc";
    std::string digestMethod = "sha256";   // empty: no digest_block unless the source asks for one
    std::string enctype = "base64";
    int lineLen = 76;
    std::string agent = "msimprotect";
    std::string agentInfo = "minisim";
};

namespace detail {

inline std::string valueText(const PVal& v);
inline std::string exprText(const PExpr& e) {
    if (e.key.empty()) return valueText(e.val);
    if (e.val.kind == PVal::None) return e.key;
    return e.key + "=" + valueText(e.val);
}
inline std::string valueText(const PVal& v) {
    if (v.kind == PVal::Str) return quote(v.s);
    if (v.kind == PVal::List) {
        std::string s = "(";
        for (size_t i = 0; i < v.list.size(); i++) s += (i ? ", " : "") + exprText(v.list[i]);
        return s + ")";
    }
    return v.s;
}

// Skip an existing begin_protected envelope (its blocks included) in the input.
inline void skipEnvelope(Scanner& sc, State st, std::string_view file, int line) {
    while (auto d = sc.next()) {
        if (d->kind == Directive::ResetProtect) { st.kw.clear(); continue; }
        if (d->kind != Directive::Protect) continue;
        for (auto& e : d->exprs) {
            if (e.key == "end_protected") return;
            if (takesBlock(e.key)) { sc.block(st.enctype(), st.bytes()); continue; }
            if (e.key == "reset") st.kw.clear();
            else if (!e.key.empty()) st.kw[e.key] = e.val;
        }
    }
    throw Error(where(file, line) + "begin_protected without end_protected");
}

inline std::string block(const std::string& keyword, std::string_view bin, const std::string& enctype, int lineLen) {
    return encodingDirective(enctype, lineLen, bin.size()) + "`pragma protect " + keyword + "\n" +
           encodeBlock(bin, enctype, lineLen);
}

inline std::string makeEnvelope(std::string_view plain, const State& st, const EncryptOptions& o,
                                std::string_view file, int line) {
    std::string dataMethod = st.str("data_method", o.dataMethod);
    std::string keyMethod = st.str("key_method", "rsa");
    if (keyMethod != "rsa") throw Error(where(file, line) + "key_method \"" + keyMethod + "\" is not supported (use \"rsa\")");
    if (st.kw.count("digest_key_method") && st.str("digest_key_method") != dataMethod)
        throw Error(where(file, line) + "a digest_key_method other than the data_method is not supported");
    std::string digestMethod = o.digestMethod;
    if (st.kw.count("digest_block") || st.kw.count("digest_method")) digestMethod = st.str("digest_method", "sha256");
    if (!digestMethod.empty() && !digestName(digestMethod))
        throw Error(where(file, line) + "unsupported digest_method \"" + digestMethod + "\"");
    std::string enctype = o.enctype;
    int lineLen = o.lineLen;
    if (auto it = st.kw.find("encoding"); it != st.kw.end()) {
        for (auto& e : it->second.list) {
            if (e.key == "enctype") enctype = e.val.s;
            if (e.key == "line_length") { try { lineLen = std::stoi(e.val.s); } catch (...) {} }
        }
    }
    if (enctype != "base64" && enctype != "uuencode")
        throw Error(where(file, line) + "output encoding \"" + enctype + "\" is not supported (use base64 or uuencode)");

    // Recipients: the command line's, plus a key_public_key given in the source.
    std::vector<const Recipient*> rs;
    for (auto& r : o.recipients) rs.push_back(&r);
    Recipient fromSource;
    if (auto it = st.kw.find("key_public_key"); it != st.kw.end()) {
        fromSource.owner = st.str("key_keyowner");
        fromSource.name = st.str("key_keyname");
        fromSource.pub = publicFromDer(it->second.s);
        rs.push_back(&fromSource);
    }
    if (rs.empty())
        throw Error(where(file, line) + "no key to encrypt with: give -key <public.pem> or a key_public_key");

    Cipher c = fetchCipher(dataMethod);
    std::string sk = randomBytes((size_t)EVP_CIPHER_get_key_length(c.get()));
    std::string env = "`pragma protect begin_protected\n";
    env += "`pragma protect encrypt_agent=" + quote(o.agent) + ", encrypt_agent_info=" + quote(o.agentInfo) + "\n";
    for (const char* k : {"author", "author_info"})
        if (st.kw.count(k)) env += std::string("`pragma protect ") + k + "=" + valueText(st.kw.at(k)) + "\n";
    if (!digestMethod.empty()) env += "`pragma protect digest_method=" + quote(digestMethod) + "\n";
    for (const Recipient* r : rs) {
        env += "`pragma protect key_keyowner=" + quote(r->owner) + ", key_keyname=" + quote(r->name) +
               ", key_method=\"rsa\"\n";
        env += block("key_public_key", publicDer(r->pub.get()), enctype, lineLen);
        env += block("key_block", rsaEncrypt(r->pub.get(), sk), enctype, lineLen);
        if (!digestMethod.empty())
            env += block("digest_block", cbcEncrypt(dataMethod, sk, digest(digestMethod, sk)), enctype, lineLen);
    }
    env += "`pragma protect data_method=" + quote(dataMethod) + "\n";
    env += block("data_block", cbcEncrypt(dataMethod, sk, plain), enctype, lineLen);
    if (!digestMethod.empty())
        env += block("digest_block", cbcEncrypt(dataMethod, sk, digest(digestMethod, plain)), enctype, lineLen);
    OPENSSL_cleanse(sk.data(), sk.size());
    env += "`pragma protect end_protected\n`pragma reset protect\n";
    return env;
}

}  // namespace detail

// Replace every encryption envelope (`pragma protect begin ... end) in
// `text` with a decryption envelope (34.3.1). Text outside the envelopes,
// including existing begin_protected envelopes, is copied unchanged.
inline std::string encryptSource(std::string_view text, const EncryptOptions& o, std::string_view file,
                                 int* envelopes = nullptr) {
    using namespace detail;
    Scanner sc(text);
    State st;
    std::string out;
    size_t copied = 0;
    int count = 0;
    while (auto d = sc.next()) {
        if (d->kind == Directive::ResetProtect) { st.kw.clear(); continue; }
        if (d->kind != Directive::Protect) continue;
        size_t bi = d->exprs.size();
        for (size_t i = 0; i < d->exprs.size(); i++) {
            const PExpr& e = d->exprs[i];
            if (e.key == "begin") { bi = i; break; }
            if (e.key == "end") throw Error(where(file, d->line) + "`pragma protect end without begin");
            if (e.key == "begin_protected") {
                State env = st;
                skipEnvelope(sc, env, file, d->line);
                bi = d->exprs.size();
                goto next;
            }
            if (e.key == "key_block" || e.key == "data_block")
                throw Error(where(file, d->line) + e.key + " outside a begin_protected envelope");
            if (e.key == "digest_block") { st.kw["digest_block"] = PVal{}; continue; }   // a request (34.5.22)
            applyKeyword(sc, st, e);
        }
        if (bi < d->exprs.size()) {
            State env = st;
            for (size_t i = bi + 1; i < d->exprs.size(); i++)
                if (!d->exprs[i].key.empty() && !takesBlock(d->exprs[i].key)) env.kw[d->exprs[i].key] = d->exprs[i].val;
            int line = d->line;
            size_t bodyStart = d->next;
            std::optional<Directive> endDir;
            while (auto e = sc.next()) {
                if (e->kind != Directive::Protect) continue;
                bool isEnd = false;
                for (auto& x : e->exprs) {
                    if (x.key == "begin") throw Error(where(file, e->line) + "nested `pragma protect begin (34.5.1)");
                    if (x.key == "end") { isEnd = true; break; }
                    if (x.key == "begin_protected") { skipEnvelope(sc, State{}, file, e->line); break; }
                }
                if (isEnd) { endDir = e; break; }
            }
            if (!endDir) throw Error(where(file, line) + "`pragma protect begin without end");
            out.append(text.substr(copied, d->start - copied));
            if (bi > 0) {
                out += "`pragma protect ";
                for (size_t i = 0; i < bi; i++) out += (i ? ", " : "") + exprText(d->exprs[i]);
                out += "\n";
            }
            out += makeEnvelope(text.substr(bodyStart, endDir->start - bodyStart), env, o, file, line);
            copied = endDir->next;
            count++;
        }
    next:;
    }
    out.append(text.substr(copied));
    if (envelopes) *envelopes = count;
    return out;
}

}  // namespace protect
