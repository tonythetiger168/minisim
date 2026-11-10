// protect.hpp - protected envelopes: `pragma protect begin/end regions are
// encrypted into begin_protected/end_protected envelopes, and envelopes are
// decrypted back into source text for the compiler. Two formats:
//
//  - IEEE 1735-2023 version 3 (encryptSourceV3, namespace v3; the default):
//    see the comment at "IEEE 1735-2023 version 3" below.
//  - IEEE 1800-2023 clause 34 (encryptSource; msimprotect -v1).
//
// decryptSource opens both. Clause 34 envelope layout written by
// encryptSource (one key_block per recipient):
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

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>

#include <cctype>
#include <cstdio>
#include <functional>
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
    // IEEE 1735 forms that are not 22.11 pragma expressions (control,
    // viewport, interface_viewport, end_protected="<signature>"): exprs holds
    // just the keyword and args the rest of the line.
    std::string_view args;
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
            std::string_view rest = body.substr(i);
            size_t j = 0;
            while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) j++;
            size_t k = j;
            while (k < rest.size() && isIdent(rest[k])) k++;
            std::string_view first = rest.substr(j, k - j);
            if (first == "control" || first == "viewport" || first == "interface_viewport" || first == "end_protected") {
                PExpr e;
                e.key = std::string(first);
                d.exprs.push_back(std::move(e));
                d.args = rest.substr(k);
                while (!d.args.empty() && (d.args.back() == '\r' || d.args.back() == ' ' || d.args.back() == '\t'))
                    d.args.remove_suffix(1);
            } else {
                d.exprs = ExprParser(rest).all();
            }
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
    std::string owner;     // key_keyowner; empty: any
    std::string keyname;   // key_keyname; empty: any key_keyname
    std::string path;
    PKey key;
    std::string pubDer;
};

// Words of a keyring table line: whitespace separated, "double quoted" words
// may hold spaces. A # starts a comment.
inline std::vector<std::string> keyringWords(std::string_view line) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '#') break;
        std::string t;
        if (c == '"') {
            for (i++; i < line.size() && line[i] != '"'; i++) t += line[i];
            if (i >= line.size()) throw Error("unterminated \" in keyring line");
            i++;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') t += line[i++];
        }
        w.push_back(std::move(t));
    }
    return w;
}

inline std::string dirOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string() : path.substr(0, s + 1);
}
inline bool isAbsolute(const std::string& p) {
    return !p.empty() && (p[0] == '/' || p[0] == '\\' || (p.size() > 1 && p[1] == ':'));
}

class Keyring {
public:
    // "<pem>", "<key_keyname>=<pem>", or "<key_keyowner>:<key_keyname>=<pem>".
    void add(const std::string& spec) {
        size_t eq = spec.find('=');
        if (eq == std::string::npos || eq == 0) { addKey("", "", spec); return; }
        std::string id = spec.substr(0, eq), path = spec.substr(eq + 1);
        size_t colon = id.rfind(':');
        if (colon == std::string::npos) addKey("", id, path);
        else addKey(id.substr(0, colon), id.substr(colon + 1), path);
    }
    void add(PKey key, std::string keyname = "", std::string owner = "") {
        PrivateKey k;
        k.owner = std::move(owner);
        k.keyname = std::move(keyname);
        checkUnique(k.owner, k.keyname);
        k.pubDer = publicDer(key.get());
        k.key = std::move(key);
        keys.push_back(std::move(k));
    }
    // A keyring file: one key per line, `<key_keyowner> <key_keyname> <private.pem>`,
    // the PEM path relative to the keyring file.
    void loadFile(const std::string& path) {
        std::string text = readFile(path);
        size_t b = 0;
        int line = 0;
        while (b <= text.size()) {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            line++;
            auto w = keyringWords(std::string_view(text).substr(b, e - b));
            b = e + 1;
            if (w.empty()) continue;
            if (w.size() != 3)
                throw Error(path + ":" + std::to_string(line) + ": expected <key_keyowner> <key_keyname> <private.pem>");
            std::string pem = isAbsolute(w[2]) ? w[2] : dirOf(path) + w[2];
            addKey(w[0], w[1], pem);
        }
    }
    bool empty() const { return keys.empty(); }
    std::vector<PrivateKey> keys;

private:
    // IEEE 1735 6.2.1: two keys may not share a key_keyowner / key_keyname.
    void checkUnique(const std::string& owner, const std::string& name) {
        if (name.empty()) return;
        for (auto& k : keys)
            if (k.keyname == name && k.owner == owner)
                throw Error("two keys named key_keyowner=" + quote(owner) + " key_keyname=" + quote(name));
    }
    void addKey(std::string owner, std::string name, const std::string& path) {
        PrivateKey k;
        checkUnique(owner, name);
        k.owner = std::move(owner);
        k.keyname = std::move(name);
        k.path = path;
        k.key = loadPrivateKey(path);
        k.pubDer = publicDer(k.key.get());
        keys.push_back(std::move(k));
    }
};

// A visibility pragma found in decrypted text (IEEE 1735 9.5): `pragma
// protect viewport [name] or interface_viewport [name]. Without a name it is
// for the declaration or design unit after it, [targetBegin, targetEnd).
struct Viewport {
    bool iface = false;
    std::string name;
    size_t at = 0;                          // offset of the pragma in Decrypted::text
    size_t targetBegin = 0, targetEnd = 0;  // unnamed: the next declaration
};

// One decrypted envelope.
struct Envelope {
    int version = 0;                       // 3 (IEEE 1735-2023), or 0 (1800-2023 clause 34)
    size_t begin = 0, end = 0;             // its cleartext in Decrypted::text
    std::string owner, keyname;            // the tool block whose key opened it (version 3)
    std::map<std::string, std::string> rights;   // that tool block's control values (1735 clause 7)
    std::vector<Viewport> viewports;
    std::string right(const std::string& k, const std::string& def = "") const {
        auto it = rights.find(k);
        return it == rights.end() ? def : it->second;
    }
};

struct DecryptOptions {
    // A license or license_string call in a right (1735 clause 8): nullopt
    // when the feature is not licensed, else the license_string value.
    std::function<std::optional<std::string>(const std::string&)> license;
    std::function<void(const std::string&)> warn;
};

struct Decrypted {
    std::string text;
    std::vector<std::pair<size_t, size_t>> ranges;   // [begin, end) offsets of decrypted text in `text`
    std::vector<Envelope> envs;                      // the outermost envelopes, in order
    int envelopes = 0;
};

// ---------------------------------------------------------------- IEEE 1735-2023 version 3

// A version 3 envelope (IEEE 1735-2023 5.3-5.8): a random 64-byte root key,
// RSA encrypted without padding for each tool block (its least significant
// 64 bytes are the root key, the rest random); the first 32 bytes of the root
// key are the AES-256-CBC key of the data_block (IV first), the last 32 the
// HMAC-SHA512 key of the signature in end_protected="...", computed over the
// envelope text from its begin_protected line to end_protected with carriage
// returns removed. The signature is checked before anything is decrypted.
namespace v3 {

constexpr const char* kDataMethod = "aes256-cbc-hmac-sha512";
constexpr const char* kKeyMethod = "rsa_no_padding";
constexpr size_t kRootLen = 64;

struct BnFree { void operator()(BIGNUM* b) const { BN_free(b); } };
using Bn = std::unique_ptr<BIGNUM, BnFree>;

inline Bn rsaParam(EVP_PKEY* k, const char* name) {
    BIGNUM* b = nullptr;
    if (!EVP_PKEY_get_bn_param(k, name, &b)) throw Error("cannot read the RSA key: " + sslError());
    return Bn(b);
}

// RSA without padding (5.4.2): the input is exactly the modulus size.
inline std::optional<std::string> rsaRaw(EVP_PKEY* k, std::string_view in, bool encrypt) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(k, nullptr);
    if (!ctx) throw Error("RSA: " + sslError());
    std::optional<std::string> r;
    size_t n = 0;
    auto run = [&](unsigned char* out, size_t* len) {
        return encrypt ? EVP_PKEY_encrypt(ctx, out, len, (const unsigned char*)in.data(), in.size())
                       : EVP_PKEY_decrypt(ctx, out, len, (const unsigned char*)in.data(), in.size());
    };
    if ((encrypt ? EVP_PKEY_encrypt_init(ctx) : EVP_PKEY_decrypt_init(ctx)) > 0 &&
        EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_NO_PADDING) > 0 && run(nullptr, &n) > 0) {
        std::string out(n, '\0');
        if (run((unsigned char*)out.data(), &n) > 0) { out.resize(n); r = std::move(out); }
    }
    EVP_PKEY_CTX_free(ctx);
    ERR_clear_error();
    return r;
}

// Key checks of 5.4.2: public exponent at least 65537, modulus over 512 bits.
// Returns the modulus size in bits.
inline int checkKey(EVP_PKEY* k) {
    if (EVP_PKEY_get_base_id(k) != EVP_PKEY_RSA) throw Error("not an RSA key");
    Bn e = rsaParam(k, OSSL_PKEY_PARAM_RSA_E);
    Bn f4(BN_new());
    BN_set_word(f4.get(), 65537);
    if (BN_cmp(e.get(), f4.get()) < 0) throw Error("RSA public exponent below 65537 (IEEE 1735 5.4.2)");
    int bits = EVP_PKEY_get_bits(k);
    if (bits < 513) throw Error("RSA key of " + std::to_string(bits) + " bits; IEEE 1735 needs 513 or more");
    return bits;
}

// The key_block for one tool block: a random number below the modulus whose
// least significant 64 bytes are the root key, RSA encrypted without padding.
inline std::string keyBlock(EVP_PKEY* pub, const std::string& root) {
    checkKey(pub);
    Bn n = rsaParam(pub, OSSL_PKEY_PARAM_RSA_N);
    int bits = BN_num_bits(n.get());
    size_t len = (size_t)BN_num_bytes(n.get());
    if (len < kRootLen + 1) throw Error("RSA key too small for a 64-byte root key");
    for (int tries = 0; tries < 1000; tries++) {
        std::string block = randomBytes(len);
        std::memcpy(block.data() + len - kRootLen, root.data(), kRootLen);
        int excess = (int)(len * 8) - bits;
        block[0] = (char)((unsigned char)block[0] & (0xffu >> excess));
        Bn v(BN_bin2bn((const unsigned char*)block.data(), (int)len, nullptr));
        bool ok = BN_cmp(v.get(), n.get()) < 0;
        if (ok) {
            auto c = rsaRaw(pub, block, true);
            OPENSSL_cleanse(block.data(), block.size());
            if (!c) throw Error("RSA encryption failed");
            return *c;
        }
        OPENSSL_cleanse(block.data(), block.size());
    }
    throw Error("cannot draw an RSA block below the modulus");
}

inline std::string hmac512(std::string_view key, std::string_view text) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (!HMAC(EVP_sha512(), key.data(), (int)key.size(), (const unsigned char*)text.data(), text.size(), md, &n))
        throw Error("HMAC-SHA512: " + sslError());
    return std::string((const char*)md, n);
}

// The signed text of 5.8: carriage returns removed.
inline std::string canonical(std::string_view t) {
    std::string s;
    s.reserve(t.size());
    for (char c : t) if (c != '\r') s += c;
    return s;
}

// base64 with mandatory padding (5.4.1).
inline std::string strictBase64(std::string_view enc, const std::string& what) {
    size_t n = 0;
    for (char c : enc) if (c != '\n' && c != '\r') n++;
    if (n == 0 || n % 4) throw Error(what + " is not padded base64 (IEEE 1735 5.4.1)");
    return base64Decode(enc);
}

// A 1735 STRING (Annex A, 5.2.1): printable ASCII without " or backslash.
inline bool validString(std::string_view s) {
    for (char c : s) if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
    return true;
}

// ---- rights (1735 clause 7): `pragma protect control a = "x", b = license("f") ? "t" : "e"

struct ControlTok {
    enum K { Ident, Str, Sym, End } k;
    std::string s;
};

class ControlParser {
public:
    explicit ControlParser(std::string_view a) : s(a) {}
    // name -> value expression; calls `license` for license / license_string.
    std::vector<std::pair<std::string, std::string>> parse(
        const std::function<std::optional<std::string>(const std::string&)>& license) {
        std::vector<std::pair<std::string, std::string>> out;
        for (;;) {
            ControlTok id = tok();
            if (id.k != ControlTok::Ident) throw Error("expected a right name after control");
            for (char c : id.s)
                if (!(std::isalnum((unsigned char)c) || c == '_')) throw Error("bad right name " + id.s);
            expect("=");
            ControlTok v = tok();
            std::string val;
            if (v.k == ControlTok::Str) {
                val = v.s;
            } else if (v.k == ControlTok::Ident && (v.s == "license" || v.s == "license_string")) {
                std::string feature = arg();
                auto lic = license ? license(feature) : std::nullopt;
                if (v.s == "license_string") {
                    val = lic.value_or("");
                } else {
                    expect("?");
                    ControlTok t = tok();
                    if (t.k != ControlTok::Str) throw Error("expected a string after ?");
                    expect(":");
                    ControlTok f = tok();
                    if (f.k != ControlTok::Str) throw Error("expected a string after :");
                    val = lic ? t.s : f.s;
                }
            } else {
                throw Error("a right's value is a string, license(...) ? ... : ..., or license_string(...)");
            }
            out.emplace_back(id.s, val);
            ControlTok n = tok();
            if (n.k == ControlTok::End) break;
            if (n.k != ControlTok::Sym || n.s != ",") throw Error("expected , between rights");
        }
        return out;
    }

private:
    std::string_view s;
    size_t p = 0;
    ControlTok tok() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
        if (p >= s.size()) return {ControlTok::End, ""};
        char c = s[p];
        if (c == '"') {
            size_t e = s.find('"', p + 1);
            if (e == std::string_view::npos) throw Error("unterminated string in control");
            std::string v(s.substr(p + 1, e - p - 1));
            p = e + 1;
            return {ControlTok::Str, v};
        }
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t b = p;
            while (p < s.size() && (std::isalnum((unsigned char)s[p]) || s[p] == '_')) p++;
            return {ControlTok::Ident, std::string(s.substr(b, p - b))};
        }
        p++;
        return {ControlTok::Sym, std::string(1, c)};
    }
    void expect(const char* sym) {
        ControlTok t = tok();
        if (t.k != ControlTok::Sym || t.s != sym) throw Error(std::string("expected ") + sym + " in control");
    }
    // license("f"), license "f" and license = "f" are all read.
    std::string arg() {
        ControlTok t = tok();
        bool paren = false;
        if (t.k == ControlTok::Sym && (t.s == "(" || t.s == "=")) { paren = t.s == "("; t = tok(); }
        if (t.k != ControlTok::Str) throw Error("a license call takes a quoted feature name");
        if (paren) expect(")");
        return t.s;
    }
};

// The declaration or design unit after `at` (an unnamed viewport, 9.6.2):
// a module / interface / program / package / checker / class to its end
// keyword, otherwise up to the next ';'.
inline std::pair<size_t, size_t> nextDeclaration(std::string_view t, size_t at) {
    size_t p = at;
    auto skipSpace = [&] {
        for (;;) {
            while (p < t.size() && std::isspace((unsigned char)t[p])) p++;
            if (t.substr(p, 2) == "//") { size_t e = t.find('\n', p); p = e == t.npos ? t.size() : e; continue; }
            if (t.substr(p, 2) == "/*") { size_t e = t.find("*/", p + 2); p = e == t.npos ? t.size() : e + 2; continue; }
            if (t.substr(p, 2) == "(*") { size_t e = t.find("*)", p + 2); p = e == t.npos ? t.size() : e + 2; continue; }
            break;
        }
    };
    skipSpace();
    size_t b = p;
    size_t w = p;
    while (w < t.size() && (std::isalnum((unsigned char)t[w]) || t[w] == '_')) w++;
    std::string word(t.substr(p, w - p));
    if (word == "virtual" || word == "extern") {
        // virtual class ...
        size_t q = w;
        while (q < t.size() && std::isspace((unsigned char)t[q])) q++;
        size_t r = q;
        while (r < t.size() && (std::isalnum((unsigned char)t[r]) || t[r] == '_')) r++;
        if (t.substr(q, r - q) == "class") word = "class";
    }
    static const std::map<std::string, std::string> units{
        {"module", "endmodule"}, {"macromodule", "endmodule"}, {"interface", "endinterface"},
        {"program", "endprogram"}, {"package", "endpackage"}, {"checker", "endchecker"}, {"class", "endclass"}};
    auto it = units.find(word);
    if (it != units.end()) {
        size_t q = w;
        int depth = 1;
        while (q < t.size()) {
            size_t a = q;
            if (!(std::isalnum((unsigned char)t[q]) || t[q] == '_')) { q++; continue; }
            while (q < t.size() && (std::isalnum((unsigned char)t[q]) || t[q] == '_')) q++;
            if (a > 0 && (std::isalnum((unsigned char)t[a - 1]) || t[a - 1] == '_' || t[a - 1] == '$')) continue;
            std::string_view x = t.substr(a, q - a);
            if (x == word) depth++;
            else if (x == it->second && --depth == 0) return {b, q};
        }
        return {b, t.size()};
    }
    size_t e = t.find(';', p);
    return {b, e == t.npos ? t.size() : e + 1};
}

struct ToolBlock {
    std::string owner, name, method;
    std::string keyBlock;
    std::vector<std::string> controls;   // the text after each control keyword
    int line = 0;
};

// The envelope whose begin_protected line starts at lineStart; sc is just past
// that line. Returns the cleartext; info gets the tool block, rights and
// viewports (viewport offsets relative to the cleartext).
inline std::string envelope(std::string_view text, size_t lineStart, Scanner& sc, const Keyring& ring,
                            const DecryptOptions& opt, std::string_view file, int beginLine, Envelope& info) {
    auto where = [&](int line) { return std::string(file) + ":" + std::to_string(line) + ": "; };
    std::vector<ToolBlock> tbs;
    ToolBlock* cur = nullptr;
    bool inCommon = false, sawVersion = false, done = false, proxy = false;
    std::string dataMethod, sigB64;
    std::optional<std::string> data;
    size_t signedEnd = 0;
    int endLine = beginLine;
    while (auto d = sc.next()) {
        if (d->kind != Directive::Protect)
            throw Error(where(d->line) + "only `pragma protect lines belong in a version 3 envelope");
        for (size_t xi = 0; xi < d->exprs.size(); xi++) {
            const PExpr& e = d->exprs[xi];
            const std::string& k = e.key;
            auto str = [&]() -> std::string {
                if (e.val.kind != PVal::Str) throw Error(where(d->line) + k + " needs a quoted string");
                return e.val.s;
            };
            if (!sawVersion) {
                if (k != "version" || d->exprs.size() != 1)
                    throw Error(where(d->line) + "version = 3 must be alone on the line after begin_protected (5.4.9)");
                sawVersion = true;
                continue;
            }
            if (k == "version") throw Error(where(d->line) + "a second version pragma in a version 3 envelope (5.4.9)");
            if (k == "end_protected") {
                if (cur || inCommon) throw Error(where(d->line) + "end_protected inside a tool or common block");
                std::string_view line = text.substr(d->start, d->lineEnd - d->start);
                signedEnd = d->start + line.find("end_protected") + std::strlen("end_protected");
                std::string_view a = d->args;
                size_t p = 0;
                while (p < a.size() && (a[p] == ' ' || a[p] == '\t')) p++;
                if (p >= a.size() || a[p] != '=') throw Error(where(d->line) + "end_protected has no signature (5.8.4)");
                p++;
                while (p < a.size() && (a[p] == ' ' || a[p] == '\t')) p++;
                size_t q = p < a.size() && a[p] == '"' ? a.find('"', p + 1) : std::string_view::npos;
                if (q == std::string_view::npos || q + 1 != a.size())
                    throw Error(where(d->line) + "end_protected signature must be one quoted base64 string (5.8.4)");
                sigB64 = std::string(a.substr(p + 1, q - p - 1));
                endLine = d->line;
                done = true;
                break;
            }
            if (k == "begin_toolblock") {
                if (cur || inCommon) throw Error(where(d->line) + "begin_toolblock inside a block");
                if (data) throw Error(where(d->line) + "tool block after the data_block");
                tbs.emplace_back();
                cur = &tbs.back();
                cur->line = d->line;
                continue;
            }
            if (k == "end_toolblock") {
                if (!cur) throw Error(where(d->line) + "end_toolblock without begin_toolblock");
                if (e.val.kind != PVal::None)
                    throw Error(where(d->line) + "end_toolblock with a value is version 2 syntax (5.1.2)");
                cur = nullptr;
                continue;
            }
            if (k == "begin_commonblock") {
                if (cur || inCommon || !tbs.empty()) throw Error(where(d->line) + "the common block goes before the tool blocks");
                inCommon = true;
                continue;
            }
            if (k == "end_commonblock") {
                if (!inCommon) throw Error(where(d->line) + "end_commonblock without begin_commonblock");
                inCommon = false;
                continue;
            }
            if (k == "key_keyowner" || k == "key_keyname" || k == "key_method") {
                if (!cur) throw Error(where(d->line) + k + " outside a tool block");
                (k == "key_keyowner" ? cur->owner : k == "key_keyname" ? cur->name : cur->method) = str();
                continue;
            }
            if (k == "key_block") {
                if (!cur) throw Error(where(d->line) + "key_block outside a tool block");
                cur->keyBlock = strictBase64(sc.block("base64", -1), "key_block");
                continue;
            }
            if (k == "control") {
                if (!cur) throw Error(where(d->line) + "control outside a tool block (7.4)");
                cur->controls.emplace_back(d->args);
                continue;
            }
            if (k == "license_proxyenv") {
                if (!inCommon) throw Error(where(d->line) + "license_proxyenv outside the common block");
                str();
                proxy = true;
                continue;
            }
            if (k == "license_certificate") {
                if (!inCommon) throw Error(where(d->line) + "license_certificate outside the common block");
                strictBase64(sc.block("base64", -1), "license_certificate");
                continue;
            }
            if (k == "data_block") {
                if (cur || inCommon) throw Error(where(d->line) + "data_block inside a block");
                if (data) throw Error(where(d->line) + "a second data_block");
                data = strictBase64(sc.block("base64", -1), "data_block");
                continue;
            }
            if (k == "data_method") { dataMethod = str(); continue; }
            if (k == "author" || k == "author_info" || k == "encrypt_agent" || k == "encrypt_agent_info") { str(); continue; }
            if (k == "encoding" || k == "rights_digest_method" || k == "digest_block" || k == "digest_method" ||
                k == "key_public_key" || k == "begin" || k == "end" || k == "begin_protected")
                throw Error(where(d->line) + "'" + k + "' does not belong in a version 3 envelope (5.1.2, 5.5.6)");
            throw Error(where(d->line) + "unknown keyword '" + k + "' in a version 3 envelope");
        }
        if (done) break;
    }
    if (!done) throw Error(where(beginLine) + "begin_protected without end_protected");
    if (dataMethod != kDataMethod)
        throw Error(where(beginLine) + "version 3 envelope needs data_method = \"" + kDataMethod + "\"");
    if (tbs.empty()) throw Error(where(beginLine) + "version 3 envelope has no tool block (5.4.2)");
    if (!data) throw Error(where(beginLine) + "protected envelope has no data_block");
    std::string sig = strictBase64(sigB64, "end_protected signature");
    if (sig.size() != 64) throw Error(where(endLine) + "end_protected signature is not 512 bits (5.4.1)");
    if (data->size() < 32 || data->size() % 16) throw Error(where(beginLine) + "data_block is not IV + AES-CBC blocks");

    std::string signedText = canonical(text.substr(lineStart, signedEnd - lineStart));

    // Pick the tool block (5.4.2): the first whose key_keyowner / key_keyname
    // a key in the keyring has; keys given without a name are tried on every
    // tool block, and only a valid signature accepts one.
    const ToolBlock* used = nullptr;
    std::string root;
    bool recognized = false;
    auto tryKey = [&](const ToolBlock& tb, const PrivateKey& k) {
        if (tb.keyBlock.empty()) return false;
        auto m = rsaRaw(k.key.get(), tb.keyBlock, false);
        if (!m || m->size() < kRootLen) return false;
        std::string r = m->substr(m->size() - kRootLen);
        OPENSSL_cleanse(m->data(), m->size());
        std::string mac = hmac512(std::string_view(r).substr(32, 32), signedText);
        bool ok = CRYPTO_memcmp(mac.data(), sig.data(), 64) == 0;
        if (ok) root = std::move(r);
        else OPENSSL_cleanse(r.data(), r.size());
        return ok;
    };
    for (auto& tb : tbs) {
        if (tb.method != kKeyMethod) continue;
        for (auto& k : ring.keys) {
            if (k.keyname.empty() || k.keyname != tb.name || (!k.owner.empty() && k.owner != tb.owner)) continue;
            recognized = true;
            if (tryKey(tb, k)) used = &tb;
            break;
        }
        if (recognized) break;
    }
    if (!recognized)
        for (auto& tb : tbs) {
            if (tb.method != kKeyMethod) continue;
            for (auto& k : ring.keys)
                if (k.keyname.empty() && tryKey(tb, k)) { used = &tb; break; }
            if (used) break;
        }
    if (!used) {
        if (recognized) throw Error(where(beginLine) + "signature verification failed");
        bool anyWild = false;
        for (auto& k : ring.keys) anyWild = anyWild || k.keyname.empty();
        std::string list;
        for (auto& tb : tbs)
            list += (list.empty() ? "" : ", ") + std::string("key_keyowner=") + quote(tb.owner) + " key_keyname=" + quote(tb.name);
        throw Error(where(beginLine) + "no key for this protected envelope (" + list + "); " +
                    (anyWild ? "no -keyfile key opens it, or it was modified"
                             : "pass the private key with -keyfile or -keyring"));
    }

    // The signature holds: decrypt.
    auto plain = cbcDecrypt("aes256-cbc", root.substr(0, 32), *data);
    OPENSSL_cleanse(root.data(), root.size());
    if (!plain) throw Error(where(beginLine) + "data_block does not decrypt");

    info.version = 3;
    info.owner = used->owner;
    info.keyname = used->name;
    DecryptOptions o = opt;
    bool warned = false;
    auto license = [&](const std::string& f) -> std::optional<std::string> {
        if (o.license) return o.license(f);
        if (proxy && !warned && o.warn) {
            o.warn(where(beginLine) + "the envelope names a license proxy; license checks are not run, so licensed rights take their unlicensed value");
            warned = true;
        }
        return std::nullopt;
    };
    for (auto& c : used->controls) {
        try {
            for (auto& [n, v] : ControlParser(c).parse(license)) info.rights[n] = v;
        } catch (const Error& ex) {
            OPENSSL_cleanse(plain->data(), plain->size());
            throw Error(where(used->line) + ex.what());
        }
    }

    // Nested envelopes are not allowed (5.1.2); viewports are read and their
    // lines blanked so the compiler does not see them.
    Scanner ps(*plain);
    while (auto d = ps.next()) {
        if (d->kind != Directive::Protect) continue;
        for (auto& e : d->exprs) {
            if (e.key == "begin_protected") {
                OPENSSL_cleanse(plain->data(), plain->size());
                throw Error(where(beginLine) + "a version 3 envelope may not hold another envelope (5.1.2)");
            }
            if (e.key == "viewport" || e.key == "interface_viewport") {
                Viewport v;
                v.iface = e.key == "interface_viewport";
                std::string_view a = d->args;
                while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.remove_prefix(1);
                v.name = std::string(a);
                v.at = d->start;
                for (size_t i = d->start; i < d->lineEnd; i++) (*plain)[i] = ' ';
                if (v.name.empty()) {
                    auto [b, en] = nextDeclaration(*plain, d->next);
                    v.targetBegin = b;
                    v.targetEnd = en;
                }
                info.viewports.push_back(std::move(v));
            }
        }
    }
    return std::move(*plain);
}

// Is the begin_protected directive at sc's position the start of a version 3
// envelope? (Looks at the next directive without consuming it.)
inline bool startsV3(const Scanner& sc, int* version) {
    Scanner peek = sc;
    auto d = peek.next();
    if (!d || d->kind != Directive::Protect || d->exprs.empty() || d->exprs[0].key != "version") return false;
    try { *version = std::stoi(d->exprs[0].val.s); } catch (...) { *version = -1; }
    return *version == 3;
}

}  // namespace v3

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

inline void decryptInto(std::string_view text, const Keyring& ring, const DecryptOptions& opt, std::string_view file,
                        int depth, Decrypted& out, bool top);

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

inline void decryptInto(std::string_view text, const Keyring& ring, const DecryptOptions& opt, std::string_view file,
                        int depth, Decrypted& out, bool top) {
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
        int version = 0;
        if (bi == 0 && d->exprs.size() == 1 && v3::startsV3(sc, &version)) {
            size_t lineStart = d->start;
            while (lineStart > 0 && text[lineStart - 1] != '\n') lineStart--;
            Envelope info;
            std::string plain = v3::envelope(text, lineStart, sc, ring, opt, file, d->line, info);
            size_t b = out.text.size();
            out.text += plain;
            OPENSSL_cleanse(plain.data(), plain.size());
            if (!out.text.empty() && out.text.back() != '\n') out.text += '\n';
            out.ranges.push_back({b, out.text.size()});
            info.begin = b;
            info.end = out.text.size();
            for (auto& v : info.viewports) { v.at += b; v.targetBegin += b; v.targetEnd += b; }
            out.envs.push_back(std::move(info));
            out.envelopes += 1;
            copied = sc.pos;
            if (top) out.text += "`line " + std::to_string(sc.lineAt(copied)) + " " + quote(file) + " 0\n";
            continue;
        }
        if (version > 0 && version != 3)
            throw Error(where(file, d->line) + "IEEE 1735 version " + std::to_string(version) +
                        " envelopes are not supported (version 3, or IEEE 1800-2023 clause 34 envelopes without a version)");
        std::string plain = envelope(sc, env, ring, file, d->line);
        Decrypted inner;
        decryptInto(plain, ring, opt, file, depth + 1, inner, false);
        OPENSSL_cleanse(plain.data(), plain.size());
        size_t b = out.text.size();
        out.text += inner.text;
        OPENSSL_cleanse(inner.text.data(), inner.text.size());
        if (!out.text.empty() && out.text.back() != '\n') out.text += '\n';
        out.ranges.push_back({b, out.text.size()});
        Envelope info;
        info.begin = b;
        info.end = out.text.size();
        out.envs.push_back(std::move(info));
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
inline Decrypted decryptSource(std::string_view text, const Keyring& ring, std::string_view file,
                               const DecryptOptions& opt = {}) {
    Decrypted out;
    detail::decryptInto(text, ring, opt, file, 0, out, true);
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


// ---------------------------------------------------------------- IEEE 1735-2023 version 3 (encryption)

// An RSA public key in IEEE 1735 5.6.2 form: DER RSAPublicKey (PKCS#1). A
// SubjectPublicKeyInfo DER is accepted too.
inline PKey publicFromPkcs1Der(const std::string& der) {
    const unsigned char* p = (const unsigned char*)der.data();
    EVP_PKEY* k = d2i_PUBKEY(nullptr, &p, (long)der.size());
    if (!k) {
        ERR_clear_error();
        p = (const unsigned char*)der.data();
        k = d2i_PublicKey(EVP_PKEY_RSA, nullptr, &p, (long)der.size());
    }
    if (!k) { ERR_clear_error(); throw Error("bad key_public_key (not a DER RSA public key)"); }
    if (EVP_PKEY_get_base_id(k) != EVP_PKEY_RSA) { EVP_PKEY_free(k); throw Error("key_public_key is not an RSA key"); }
    return own(k);
}

inline std::string pkcs1Der(EVP_PKEY* k) {
    unsigned char* der = nullptr;
    int n = i2d_PublicKey(k, &der);
    if (n <= 0) throw Error("cannot encode public key: " + sslError());
    std::string s((const char*)der, (size_t)n);
    OPENSSL_free(der);
    return s;
}

// A public key and the names it goes by (IEEE 1735 6.2.1).
struct PublicKeyEntry {
    std::string owner, name, method = v3::kKeyMethod;
    PKey pub;
};

// Public keys for encryption. A keyring file may hold the IEEE 1735 6.3.1
// pragma exchange format,
//     `protect key_keyowner = "ABC"            (or `pragma protect ...)
//     `protect key_method = "rsa_no_padding"
//     `protect key_keyname = "KEY1"
//     `protect key_public_key
//     <base64 DER RSAPublicKey>
// and table lines `<key_keyowner> <key_keyname> <public-or-private.pem>`.
class PublicKeyring {
public:
    std::vector<PublicKeyEntry> keys;
    void add(PublicKeyEntry e) {
        for (auto& k : keys)
            if (k.owner == e.owner && k.name == e.name)
                throw Error("two keys named key_keyowner=" + quote(e.owner) + " key_keyname=" + quote(e.name) +
                            " (IEEE 1735 6.2.1)");
        keys.push_back(std::move(e));
    }
    const PublicKeyEntry* find(const std::string& owner, const std::string& name) const {
        for (auto& k : keys)
            if (k.owner == owner && k.name == name) return &k;
        return nullptr;
    }
    void loadFile(const std::string& path) {
        std::string text = readFile(path);
        std::vector<std::string_view> lines;
        for (size_t b = 0; b <= text.size();) {
            size_t e = text.find('\n', b);
            if (e == std::string::npos) e = text.size();
            std::string_view l = std::string_view(text).substr(b, e - b);
            if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
            lines.push_back(l);
            b = e + 1;
        }
        std::string owner, name, method = v3::kKeyMethod;
        for (size_t i = 0; i < lines.size(); i++) {
            std::string_view l = lines[i];
            while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
            auto at = [&] { return path + ":" + std::to_string(i + 1) + ": "; };
            std::string_view rest;
            if (l.substr(0, 8) == "`protect") rest = l.substr(8);
            else if (l.substr(0, 15) == "`pragma protect") rest = l.substr(15);
            else {
                auto w = keyringWords(l);
                if (w.empty() || (l.size() >= 2 && l.substr(0, 2) == "//")) continue;
                if (w.size() != 3) throw Error(at() + "expected <key_keyowner> <key_keyname> <public.pem>");
                PublicKeyEntry e;
                e.owner = w[0];
                e.name = w[1];
                e.pub = loadPublicKey(isAbsolute(w[2]) ? w[2] : dirOf(path) + w[2]);
                v3::checkKey(e.pub.get());
                add(std::move(e));
                continue;
            }
            std::vector<PExpr> ex;
            try { ex = ExprParser(rest).all(); } catch (const Error& er) { throw Error(at() + er.what()); }
            for (auto& x : ex) {
                if (x.key == "key_keyowner") owner = x.val.s;
                else if (x.key == "key_keyname") name = x.val.s;
                else if (x.key == "key_method") method = x.val.s;
                else if (x.key == "key_public_key") {
                    std::string b64;
                    while (i + 1 < lines.size()) {
                        std::string_view n = lines[i + 1];
                        while (!n.empty() && (n.front() == ' ' || n.front() == '\t')) n.remove_prefix(1);
                        if (n.empty() || n.front() == '`') break;
                        b64 += std::string(n) + "\n";
                        i++;
                    }
                    if (owner.empty() || name.empty())
                        throw Error(at() + "key_public_key needs a key_keyowner and a key_keyname before it (6.2.1)");
                    PublicKeyEntry e;
                    e.owner = owner;
                    e.name = name;
                    e.method = method;
                    e.pub = publicFromPkcs1Der(v3::strictBase64(b64, "key_public_key"));
                    v3::checkKey(e.pub.get());
                    add(std::move(e));
                } else {
                    throw Error(at() + "unexpected '" + x.key + "' in a key exchange file");
                }
            }
        }
    }
};

// The pragma exchange form of one key (6.3.1), for handing a public key to IP authors.
inline std::string exportKey(const PublicKeyEntry& e, const std::string& prefix = "`pragma protect") {
    return prefix + " key_keyowner = " + quote(e.owner) + "\n" + prefix + " key_method = " + quote(e.method) + "\n" +
           prefix + " key_keyname = " + quote(e.name) + "\n" + prefix + " key_public_key\n" +
           base64Encode(pkcs1Der(e.pub.get()), 64);
}

struct ToolRecipe {
    std::string owner, name, method = v3::kKeyMethod;
    std::vector<std::string> controls;   // text after `control`, as written
    PKey pub;                            // from key_public_key, else looked up in the keyring
};

struct V3Options {
    std::vector<ToolRecipe> tools;       // tool blocks for every envelope (command line)
    const PublicKeyring* keyring = nullptr;
    std::string agent = "msimprotect";
    std::string agentInfo = "minisim";
    std::string author, authorInfo;      // defaults for a recipe without them
    std::function<void(const std::string&)> warn;
};

namespace v3 {

struct Recipe {
    std::string author, authorInfo;
    std::vector<std::string> common;     // directive text inside begin_commonblock
    std::vector<ToolRecipe> tools;
};

inline std::string line(const std::string& body) { return "`pragma protect " + body + "\n"; }

inline std::string makeEnvelope(std::string_view plain, const Recipe& r, const V3Options& o,
                                std::string_view file, int at) {
    auto where = [&] { return std::string(file) + ":" + std::to_string(at) + ": "; };
    std::vector<ToolRecipe> tools = r.tools;
    for (auto& t : o.tools) {
        bool dup = false;
        for (auto& x : tools) dup = dup || (x.owner == t.owner && x.name == t.name);
        if (!dup) tools.push_back(t);
    }
    if (tools.empty() && o.keyring)
        for (auto& k : o.keyring->keys) {
            ToolRecipe t;
            t.owner = k.owner;
            t.name = k.name;
            t.method = k.method;
            t.pub = k.pub;
            tools.push_back(std::move(t));
        }
    if (tools.empty())
        throw Error(where() + "no tool block to encrypt for: give -key, -keyring, or a begin_toolblock recipe");
    for (auto& t : tools) {
        if (!t.pub)
            for (auto& c : o.tools)
                if (c.owner == t.owner && c.name == t.name && c.pub) t.pub = c.pub;
        if (!t.pub && o.keyring)
            if (auto* k = o.keyring->find(t.owner, t.name)) t.pub = k->pub;
        if (!t.pub)
            throw Error(where() + "no public key for key_keyowner=" + quote(t.owner) + " key_keyname=" + quote(t.name) +
                        ": add key_public_key to the tool block or the key to a -keyring file");
        if (t.method != kKeyMethod)
            throw Error(where() + "key_method " + quote(t.method) + " is not version 3; use \"" + kKeyMethod + "\"");
        if (t.owner.empty() || t.name.empty())
            throw Error(where() + "every tool block needs a key_keyowner and a key_keyname (6.2.1)");
        for (auto* s : {&t.owner, &t.name})
            if (!validString(*s)) throw Error(where() + "key names may not hold \" or \\ (5.2.1)");
        if (checkKey(t.pub.get()) < 3072 && o.warn)
            o.warn(where() + "the key for " + quote(t.owner) + "/" + quote(t.name) +
                   " is under 3072 bits, the size IEEE 1735 recommends");
        for (auto& c : t.controls) ControlParser(c).parse({});   // syntax check
    }
    std::string author = r.author.empty() ? o.author : r.author;
    std::string authorInfo = r.authorInfo.empty() ? o.authorInfo : r.authorInfo;
    for (const std::string* s : std::initializer_list<const std::string*>{&author, &authorInfo, &o.agent, &o.agentInfo})
        if (!validString(*s)) throw Error(where() + "author and agent strings may not hold \" or \\ (5.2.1)");

    // A fresh root key and IV for every envelope (5.4.5).
    std::string root = randomBytes(kRootLen);
    std::string env = line("begin_protected") + line("version=3");
    env += line("encrypt_agent=" + quote(o.agent));
    if (!o.agentInfo.empty()) env += line("encrypt_agent_info=" + quote(o.agentInfo));
    if (!author.empty()) env += line("author=" + quote(author));
    if (!authorInfo.empty()) env += line("author_info=" + quote(authorInfo));
    env += line(std::string("data_method=\"") + kDataMethod + "\"");
    if (!r.common.empty()) {
        env += line("begin_commonblock");
        for (auto& c : r.common) env += c;
        env += line("end_commonblock");
    }
    for (auto& t : tools) {
        env += line("begin_toolblock");
        env += line("key_keyowner=" + quote(t.owner) + ", key_keyname=" + quote(t.name));
        env += line(std::string("key_method=\"") + kKeyMethod + "\"");
        env += line("key_block");
        env += base64Encode(keyBlock(t.pub.get(), root), 64);
        for (auto& c : t.controls) {
            std::string_view a = c;
            while (!a.empty() && (a.front() == ' ' || a.front() == '\t')) a.remove_prefix(1);
            env += line("control " + std::string(a));
        }
        env += line("end_toolblock");
    }
    env += line("data_block");
    env += base64Encode(cbcEncrypt("aes256-cbc", root.substr(0, 32), plain), 64);
    env += "`pragma protect end_protected";
    std::string mac = hmac512(std::string_view(root).substr(32, 32), env);
    OPENSSL_cleanse(root.data(), root.size());
    env += "=\"" + base64Encode(mac, 0);
    env.pop_back();   // base64Encode ends with a newline
    env += "\"\n";
    return env;
}

// Skip a begin_protected envelope of either kind in eTool input.
inline void skipAnyEnvelope(Scanner& sc, std::string_view file, int line) {
    State st;
    while (auto d = sc.next()) {
        if (d->kind != Directive::Protect) continue;
        for (auto& e : d->exprs) {
            if (e.key == "end_protected") return;
            if (e.key == "encoding") st.kw[e.key] = e.val;
            if (takesBlock(e.key) || e.key == "license_certificate")
                sc.block(st.kw.count("encoding") ? st.enctype() : "base64", st.bytes());
        }
    }
    throw Error(std::string(file) + ":" + std::to_string(line) + ": begin_protected without end_protected");
}

}  // namespace v3

// Replace every `pragma protect begin ... end region of `text` with an
// IEEE 1735-2023 version 3 decryption envelope (5.6). The recipe pragmas
// (version, author, author_info, data_method, begin_toolblock ... end_toolblock,
// begin_commonblock ... end_commonblock) are consumed and removed; text
// outside the regions, and existing envelopes, are copied unchanged.
inline std::string encryptSourceV3(std::string_view text, const V3Options& o, std::string_view file,
                                   int* envelopes = nullptr) {
    using namespace v3;
    auto where = [&](int l) { return std::string(file) + ":" + std::to_string(l) + ": "; };
    Scanner sc(text);
    Recipe r;
    ToolRecipe* cur = nullptr;
    bool inCommon = false;
    std::string out;
    size_t copied = 0;
    int count = 0;
    auto drop = [&](const Directive& d, size_t to) {   // remove a recipe directive
        out.append(text.substr(copied, d.start - copied));
        copied = to;
    };
    while (auto d = sc.next()) {
        if (d->kind != Directive::Protect) continue;
        if (inCommon) {
            bool end = false;
            for (auto& e : d->exprs) end = end || e.key == "end_commonblock";
            if (end) { inCommon = false; drop(*d, d->next); continue; }
            std::string body(text.substr(d->start, d->next - d->start));
            if (!body.empty() && body.back() != '\n') body += '\n';
            for (auto& e : d->exprs)
                if (e.key == "license_certificate") body += std::string(sc.block("base64", -1));
                else if (e.key != "license_proxyenv")
                    throw Error(where(d->line) + "only license_proxyenv and license_certificate belong in a common block");
            r.common.push_back(std::move(body));
            drop(*d, sc.pos);
            continue;
        }
        const std::string& k0 = d->exprs.empty() ? std::string() : d->exprs[0].key;
        if (k0 == "begin_protected") {
            if (o.warn) o.warn(where(d->line) + "warning: begin_protected in the input; the envelope is copied unchanged (5.6.3)");
            skipAnyEnvelope(sc, file, d->line);
            continue;
        }
        if (k0 == "viewport" || k0 == "interface_viewport") {
            if (o.warn) o.warn(where(d->line) + "warning: " + k0 + " outside a begin / end region has no effect (9.5.4)");
            continue;
        }
        if (k0 == "control") {
            if (!cur) throw Error(where(d->line) + "control outside a tool block (7.4)");
            cur->controls.emplace_back(d->args);
            drop(*d, d->next);
            continue;
        }
        if (k0 == "begin") {
            if (cur || d->exprs.size() != 1) throw Error(where(d->line) + "`pragma protect begin must be alone on its line, outside a tool block");
            int line = d->line;
            size_t bodyStart = d->next;
            std::optional<Directive> endDir;
            while (auto e = sc.next()) {
                if (e->kind != Directive::Protect) continue;
                const std::string& k = e->exprs.empty() ? std::string() : e->exprs[0].key;
                if (k == "end") { endDir = e; break; }
                if (k == "viewport" || k == "interface_viewport") continue;
                if (k == "begin" || k == "begin_protected")
                    throw Error(where(e->line) + "nested protection is not allowed in version 3 (5.6)");
                throw Error(where(e->line) + "only viewport pragmas may appear between begin and end (5.6); found '" + k + "'");
            }
            if (!endDir) throw Error(where(line) + "`pragma protect begin without end");
            out.append(text.substr(copied, d->start - copied));
            out += makeEnvelope(text.substr(bodyStart, endDir->start - bodyStart), r, o, file, line);
            copied = endDir->next;
            count++;
            continue;
        }
        bool recipe = true;
        for (auto& e : d->exprs) {
            const std::string& k = e.key;
            auto str = [&]() -> std::string {
                if (e.val.kind != PVal::Str) throw Error(where(d->line) + k + " needs a quoted string");
                return e.val.s;
            };
            if (k == "version") {
                if (d->exprs.size() != 1) throw Error(where(d->line) + "the version pragma must be alone on its line (5.6)");
                if (e.val.s != "3") throw Error(where(d->line) + "msimprotect writes version 3; use -v1 for IEEE 1800-2023 clause 34 envelopes");
                r = Recipe{};   // a version pragma resets the recipe (5.6)
                cur = nullptr;
            } else if (k == "author") r.author = str();
            else if (k == "author_info") r.authorInfo = str();
            else if (k == "data_method") {
                if (str() != kDataMethod) throw Error(where(d->line) + "version 3 supports only data_method = \"" + kDataMethod + "\"");
            } else if (k == "begin_toolblock") {
                if (cur) throw Error(where(d->line) + "begin_toolblock inside a tool block");
                r.tools.emplace_back();
                cur = &r.tools.back();
            } else if (k == "end_toolblock") {
                if (!cur) throw Error(where(d->line) + "end_toolblock without begin_toolblock");
                cur = nullptr;
            } else if (k == "begin_commonblock") {
                inCommon = true;
            } else if (k == "key_keyowner" || k == "key_keyname" || k == "key_method") {
                if (!cur) throw Error(where(d->line) + k + " outside a tool block");
                (k == "key_keyowner" ? cur->owner : k == "key_keyname" ? cur->name : cur->method) = str();
            } else if (k == "key_public_key") {
                if (!cur) throw Error(where(d->line) + "key_public_key outside a tool block");
                cur->pub = publicFromPkcs1Der(strictBase64(sc.block("base64", -1), "key_public_key"));
            } else if (k == "end") {
                throw Error(where(d->line) + "`pragma protect end without begin");
            } else if (k == "encrypt_agent" || k == "encrypt_agent_info") {
                throw Error(where(d->line) + k + " is written by the encryption tool, not the source (5.5.1)");
            } else if (k == "data_block" || k == "key_block" || k == "end_protected") {
                throw Error(where(d->line) + k + " outside a begin_protected envelope");
            } else if (k == "encoding" || k == "digest_block" || k == "digest_method" || k == "rights_digest_method" ||
                       k == "data_public_key" || k == "data_decrypt_key" || k == "digest_key_method") {
                throw Error(where(d->line) + "'" + k + "' is not version 3 syntax; use -v1 for IEEE 1800-2023 clause 34 envelopes");
            } else {
                recipe = false;   // another tool's pragma: left in the text
            }
        }
        if (recipe) drop(*d, sc.pos);
    }
    if (cur) throw Error(std::string(file) + ": begin_toolblock without end_toolblock");
    out.append(text.substr(copied));
    if (envelopes) *envelopes = count;
    return out;
}

}  // namespace protect
