// test_protect.cpp - protected envelopes (IEEE 1800-2023 clause 34):
// encodings, encrypt/decrypt round trips, key selection, tampering, nesting,
// and a hand-built envelope in the raw encoding with envelope keywords that
// precede begin_protected (34.4 lexical scope).
#include <algorithm>
#include <cassert>
#include <iostream>
#include <string>

#include "protect.hpp"

using namespace protect;

static PKey genKey() {
    EVP_PKEY* k = EVP_RSA_gen(2048);
    assert(k);
    return own(k);
}

// A recipient from a key pair's public half.
static Recipient recipient(EVP_PKEY* k, const std::string& owner, const std::string& name) {
    Recipient r;
    r.owner = owner;
    r.name = name;
    r.pub = publicFromDer(publicDer(k));
    return r;
}

static PKey copyPriv(EVP_PKEY* k) {
    EVP_PKEY_up_ref(k);
    return own(k);
}

template <typename F> static bool throws(F f, const char* expect = nullptr) {
    try { f(); } catch (const Error& e) {
        if (expect && std::string(e.what()).find(expect) == std::string::npos) {
            std::cerr << "unexpected message: " << e.what() << "\n";
            return false;
        }
        return true;
    }
    return false;
}

static const char* SRC =
    "module secret(input a, output reg b);\n"
    "`pragma protect begin\n"
    "  // the crown jewels\n"
    "  always @(a) b = ~a;\n"
    "`pragma protect end\n"
    "endmodule\n"
    "// `pragma protect begin   (in a comment: not a directive)\n"
    "module after; endmodule\n";

int main() {
    // Encodings round-trip every length 0..200.
    std::string bin;
    for (int n = 0; n <= 200; n++) {
        assert(base64Decode(base64Encode(bin, 64)) == bin);
        assert(uuDecode(uuEncode(bin)) == bin);
        bin += (char)(n * 37 + 11);
    }
    assert(base64Encode("Man", 0) == "TWFu\n");
    assert(base64Decode("TWE=") == "Ma");

    PKey alice = genKey(), bob = genKey(), eve = genKey();

    // Round trip with one recipient; cleartext is gone from the output.
    EncryptOptions o;
    o.recipients.push_back(recipient(alice.get(), "Alice Corp", "alice-1"));
    int n = 0;
    std::string enc = encryptSource(SRC, o, "secret.sv", &n);
    assert(n == 1);
    assert(enc.find("crown jewels") == std::string::npos);
    assert(enc.find("always") == std::string::npos);
    assert(enc.find("`pragma protect begin_protected") != std::string::npos);
    assert(enc.find("module secret(input a, output reg b);\n") == 0);
    assert(enc.find("module after; endmodule") != std::string::npos);
    assert(enc.find("key_keyowner=\"Alice Corp\", key_keyname=\"alice-1\"") != std::string::npos);

    Keyring ra;
    ra.add(copyPriv(alice.get()));
    Decrypted d = decryptSource(enc, ra, "secret.svp");
    assert(d.envelopes == 1);
    assert(d.ranges.size() == 1);
    std::string inner = d.text.substr(d.ranges[0].first, d.ranges[0].second - d.ranges[0].first);
    assert(inner == "  // the crown jewels\n  always @(a) b = ~a;\n");
    assert(d.text.find("module secret(input a, output reg b);\n") == 0);
    // After the envelope, `line restores the .svp file's own numbering.
    {
        size_t at = enc.find("`pragma reset protect\nendmodule\n");
        int line = 1 + (int)std::count(enc.begin(), enc.begin() + (long)at, '\n');
        assert(d.text.find("`line " + std::to_string(line) + " \"secret.svp\" 0\n`pragma reset protect\nendmodule\n") != std::string::npos);
    }

    // No key, or the wrong key: an error naming the key, not the text.
    Keyring none, re;
    re.add(copyPriv(eve.get()));
    assert(throws([&] { decryptSource(enc, none, "s.svp"); }, "no key for this protected envelope (key_keyowner=\"Alice Corp\""));
    assert(throws([&] { decryptSource(enc, re, "s.svp"); }, "no key"));

    // Two recipients: either key alone opens the envelope.
    EncryptOptions o2 = o;
    o2.recipients.clear();
    o2.recipients.push_back(recipient(alice.get(), "Alice Corp", "alice-1"));
    o2.recipients.push_back(recipient(bob.get(), "Bob Inc", "bob-7"));
    std::string enc2 = encryptSource(SRC, o2, "s.sv");
    Keyring rb;
    rb.add(copyPriv(bob.get()));
    assert(decryptSource(enc2, ra, "s").text == decryptSource(enc2, rb, "s").text);

    // Tampering with the data_block is caught (bad padding or digest mismatch).
    {
        size_t p = enc.find("`pragma protect data_block\n");
        assert(p != std::string::npos);
        p += 27 + 30;   // a character inside the first base64 line
        std::string bad = enc;
        bad[p] = bad[p] == 'A' ? 'B' : 'A';
        assert(throws([&] { decryptSource(bad, ra, "s"); }));
    }

    // Nesting: an envelope inside an encrypted region decrypts too (34.3.2).
    {
        std::string twice = encryptSource("`pragma protect begin\n" + enc + "`pragma protect end\n", o, "s");
        Decrypted dd = decryptSource(twice, ra, "s");
        assert(dd.envelopes == 2);
        assert(dd.text.find("  always @(a) b = ~a;\n") != std::string::npos);
    }

    // Other methods and encodings; no digest.
    for (auto [method, enct, dig] : {std::tuple{"aes128-cbc", "uuencode", "sha1"}, std::tuple{"aes192-cbc", "base64", "md5"},
                                     std::tuple{"3des-cbc", "base64", ""}}) {
        EncryptOptions ox = o;
        ox.dataMethod = method;
        ox.enctype = enct;
        ox.digestMethod = dig;
        std::string e = encryptSource(SRC, ox, "s");
        assert(e.find(std::string("data_method=\"") + method + "\"") != std::string::npos);
        assert(decryptSource(e, ra, "s").text.find("  always @(a) b = ~a;\n") != std::string::npos);
    }

    // Keywords in the source: data_method / encoding before begin, and a
    // key_public_key recipient given in the source itself.
    {
        std::string pubB64 = base64Encode(publicDer(bob.get()), 0);
        pubB64.pop_back();
        std::string src = "`pragma protect data_method=\"aes128-cbc\"\n"
                          "`pragma protect key_keyowner=\"Bob Inc\", key_keyname=\"bob-src\"\n"
                          "`pragma protect encoding=(enctype=\"base64\")\n"
                          "`pragma protect key_public_key\n" + pubB64 + "\n"
                          "`pragma protect begin\nwire secret_w;\n`pragma protect end\n";
        EncryptOptions os;
        os.recipients.clear();
        std::string e = encryptSource(src, os, "s");
        assert(e.find("data_method=\"aes128-cbc\"") != std::string::npos);
        assert(e.find("key_keyname=\"bob-src\"") != std::string::npos);
        assert(e.find("secret_w") == std::string::npos);
        assert(decryptSource(e, rb, "s").text.find("wire secret_w;\n") != std::string::npos);
    }

    // A hand-built envelope: raw encoding with bytes=, keywords before
    // begin_protected and on its line, no key_public_key (the key is found by
    // key_keyname), and a data_method given as an envelope keyword.
    {
        std::string sk = randomBytes(32);
        std::string kb = rsaEncrypt(alice.get(), sk);
        std::string db = cbcEncrypt("aes256-cbc", sk, "reg hidden;\n");
        std::string src = "`pragma protect key_keyowner=\"Alice Corp\", key_keyname=\"alice-1\", key_method=\"rsa\"\n"
                          "`pragma protect data_method=\"aes256-cbc\", begin_protected\n"
                          "`pragma protect encoding=(enctype=\"raw\", bytes=" + std::to_string(kb.size()) + "), key_block\n" + kb + "\n"
                          "`pragma protect encoding=(enctype=\"raw\", bytes=" + std::to_string(db.size()) + "), data_block\n" + db + "\n"
                          "`pragma protect end_protected\n`pragma reset protect\nwire w;\n";
        Keyring named, wrongName;
        named.add(copyPriv(alice.get()), "alice-1");
        wrongName.add(copyPriv(alice.get()), "alice-2");
        Decrypted dr = decryptSource(src, named, "raw.sv");
        // Raw bytes may hold newlines; `line counts the file's physical lines.
        size_t at = src.find("`pragma reset protect");
        int line = 1 + (int)std::count(src.begin(), src.begin() + (long)at, '\n');
        assert(dr.text.find("reg hidden;\n`line " + std::to_string(line) + " \"raw.sv\" 0\n`pragma reset protect\nwire w;\n") != std::string::npos);
        assert(throws([&] { decryptSource(src, wrongName, "raw.sv"); }, "no key"));
    }

    // Malformed input.
    assert(throws([&] { encryptSource("`pragma protect begin\nx\n", o, "f.sv"); }, "f.sv:1: `pragma protect begin without end"));
    assert(throws([&] { encryptSource("`pragma protect begin\n`pragma protect begin\n`pragma protect end\n", o, "f.sv"); }, "nested"));
    assert(throws([&] { encryptSource("`pragma protect end\n", o, "f.sv"); }, "without begin"));
    assert(throws([&] { encryptSource("`pragma protect begin\nx\n`pragma protect end\n", EncryptOptions{}, "f.sv"); }, "no key to encrypt with"));
    assert(throws([&] { decryptSource("`pragma protect begin_protected\n", ra, "f.sv"); }, "f.sv:1: begin_protected without end_protected"));
    assert(throws([&] { decryptSource("`pragma protect data_block\nAAAA\n", ra, "f.sv"); }, "outside"));
    // A file without envelopes passes through unchanged.
    assert(decryptSource(SRC, none, "s").text == SRC);

    std::cout << "test_protect: all passed\n";
    return 0;
}
