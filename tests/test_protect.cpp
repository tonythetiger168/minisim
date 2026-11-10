// test_protect.cpp - protected envelopes (IEEE 1800-2023 clause 34):
// encodings, encrypt/decrypt round trips, key selection, tampering, nesting,
// and a hand-built envelope in the raw encoding with envelope keywords that
// precede begin_protected (34.4 lexical scope). Then IEEE 1735-2023 version 3
// envelopes: round trips, the signature over the envelope (any change is one
// "signature verification failed"), CR/LF line ends, tool block selection by
// key_keyowner / key_keyname, rights with license calls, viewports, recipe
// pragmas, keyring files, and the syntax errors of 5.4.9 / 5.6.
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


    // ------------------------------------------------ IEEE 1735-2023 version 3
    {
        auto pub = [](EVP_PKEY* k) { return publicFromDer(publicDer(k)); };
        auto tool = [&](EVP_PKEY* k, const std::string& owner, const std::string& name) {
            ToolRecipe t;
            t.owner = owner;
            t.name = name;
            t.pub = pub(k);
            return t;
        };
        V3Options v;
        v.tools.push_back(tool(alice.get(), "Alice Corp", "alice-1"));
        v.tools.push_back(tool(bob.get(), "Bob", "bob-7"));
        v.tools[1].controls.push_back("visibility = \"interface\", coverage=\"on\"");
        int n3 = 0;
        std::string e3 = encryptSourceV3(SRC, v, "secret.sv", &n3);
        assert(n3 == 1);
        assert(e3.find("crown jewels") == std::string::npos && e3.find("always") == std::string::npos);
        assert(e3.find("`pragma protect begin_protected\n`pragma protect version=3\n") != std::string::npos);
        assert(e3.find("`pragma protect data_method=\"aes256-cbc-hmac-sha512\"\n") != std::string::npos);
        assert(e3.find("key_keyowner=\"Alice Corp\", key_keyname=\"alice-1\"") != std::string::npos);
        assert(e3.find("`pragma protect key_method=\"rsa_no_padding\"\n") != std::string::npos);
        assert(e3.find("`pragma protect control visibility = \"interface\", coverage=\"on\"\n") != std::string::npos);
        size_t ep = e3.find("`pragma protect end_protected=\"");
        assert(ep != std::string::npos);
        size_t q = e3.find('"', ep + 31);
        assert(q - (ep + 31) == 88);   // 512-bit signature, base64 with padding, one line (5.4.1)
        assert(e3.find("module after; endmodule") != std::string::npos);

        Keyring a, b, wrongOwner, wild;
        a.add(copyPriv(alice.get()), "alice-1", "Alice Corp");
        b.add(copyPriv(bob.get()), "bob-7", "Bob");
        wrongOwner.add(copyPriv(alice.get()), "alice-1", "Mallory");
        wild.add(copyPriv(bob.get()));
        Decrypted da = decryptSource(e3, a, "secret.svp");
        assert(da.text.find("  always @(a) b = ~a;\n") != std::string::npos);
        assert(da.envs.size() == 1 && da.envs[0].version == 3 && da.envs[0].owner == "Alice Corp");
        assert(da.envs[0].rights.empty());   // rights are per tool block; Alice's has none
        Decrypted db = decryptSource(e3, b, "secret.svp");
        assert(db.envs[0].keyname == "bob-7" && db.envs[0].right("visibility") == "interface" &&
               db.envs[0].right("coverage") == "on");
        assert(decryptSource(e3, wild, "secret.svp").envs[0].keyname == "bob-7");   // an unnamed key is tried
        assert(throws([&] { decryptSource(e3, none, "secret.svp"); }, "no key for this protected envelope (key_keyowner=\"Alice Corp\""));
        assert(throws([&] { decryptSource(e3, wrongOwner, "secret.svp"); }, "no key"));

        // A fresh root key and IV every time: two encryptions differ.
        assert(encryptSourceV3(SRC, v, "secret.sv") != e3);

        // CR LF line ends do not change the signature (5.8.2).
        std::string crlf;
        for (char c : e3) { if (c == '\n') crlf += '\r'; crlf += c; }
        assert(decryptSource(crlf, a, "crlf.svp").text.find("always @(a)") != std::string::npos);

        // Any other change fails the signature, before anything is decrypted:
        // a right, a key name, whitespace, the data, the signature itself.
        auto tampered = [&](std::string t) {
            return throws([&] { decryptSource(t, b, "t.svp"); }, "signature verification failed");
        };
        auto sub = [&](std::string from, std::string to) {
            std::string t = e3;
            size_t at = t.find(from);
            assert(at != std::string::npos);
            return t.replace(at, from.size(), to);
        };
        assert(tampered(sub("visibility = \"interface\"", "visibility = \"full\"")));
        assert(tampered(sub("version=3\n", "version=3 \n")));
        assert(tampered(sub("encrypt_agent=\"msimprotect\"", "encrypt_agent=\"msimprotecT\"")));
        {
            std::string t = e3;
            size_t d = t.find("`pragma protect data_block\n") + 27;
            t[d] = t[d] == 'A' ? 'B' : 'A';
            assert(tampered(t));
            t = e3;
            t[ep + 40] = t[ep + 40] == 'A' ? 'B' : 'A';
            assert(tampered(t));
        }

        // Rights with license calls (7.4.3): licensed features come from the caller.
        V3Options vl;
        vl.tools.push_back(tool(alice.get(), "Alice Corp", "alice-1"));
        vl.tools[0].controls.push_back("visibility = license(\"vis_lic\") ? \"full\" : \"none\", tag = license_string(\"tag_lic\")");
        std::string el = encryptSourceV3(SRC, vl, "l.sv");
        DecryptOptions lic;
        lic.license = [](const std::string& f) -> std::optional<std::string> {
            if (f == "vis_lic") return "";
            if (f == "tag_lic") return "gold";
            return std::nullopt;
        };
        Decrypted dl = decryptSource(el, a, "l.svp", lic);
        assert(dl.envs[0].right("visibility") == "full" && dl.envs[0].right("tag") == "gold");
        Decrypted dn = decryptSource(el, a, "l.svp");
        assert(dn.envs[0].right("visibility") == "none" && dn.envs[0].right("tag").empty());
        vl.tools[0].controls = {"bad-name = \"x\""};
        assert(throws([&] { encryptSourceV3(SRC, vl, "l.sv"); }, "expected = in control"));

        // Viewports are encrypted verbatim and come back as positions (9.5);
        // their lines are blanked for the compiler.
        const char* VP =
            "`pragma protect begin\n"
            "module m(input a);\n"
            "  `pragma protect viewport\n"
            "  reg [1:0] seen, also;\n"
            "  reg hidden;\n"
            "  `pragma protect viewport m.hidden2\n"
            "  `pragma protect interface_viewport\n"
            "endmodule\n"
            "`pragma protect end\n";
        V3Options va;
        va.tools.push_back(tool(alice.get(), "Alice Corp", "alice-1"));
        std::string ev = encryptSourceV3(VP, va, "vp.sv");
        assert(ev.find("viewport") == std::string::npos);
        Decrypted dv = decryptSource(ev, a, "vp.svp");
        assert(dv.text.find("viewport") == std::string::npos);
        auto& vps = dv.envs[0].viewports;
        assert(vps.size() == 3);
        assert(!vps[0].iface && vps[0].name.empty());
        assert(dv.text.substr(vps[0].targetBegin, vps[0].targetEnd - vps[0].targetBegin) == "reg [1:0] seen, also;");
        assert(vps[1].name == "m.hidden2" && !vps[1].iface);
        assert(vps[2].iface && vps[2].name.empty());
        assert(dv.text.substr(vps[2].targetBegin, 9) == "endmodule");

        // Recipe pragmas in the source (5.6.1), with the key in a keyring
        // file in the 6.3.1 pragma exchange format; recipe lines are removed.
        PublicKeyring pk;
        PublicKeyEntry pe;
        pe.owner = "Bob";
        pe.name = "bob-7";
        pe.pub = pub(bob.get());
        std::string exch = exportKey(pe, "`protect");
        assert(exch.find("`protect key_keyowner = \"Bob\"\n`protect key_method = \"rsa_no_padding\"\n") == 0);
        {
            FILE* f = std::fopen("test_protect_ring.txt", "wb");
            std::fwrite(exch.data(), 1, exch.size(), f);
            std::fclose(f);
        }
        pk.loadFile("test_protect_ring.txt");
        assert(pk.keys.size() == 1 && pk.find("Bob", "bob-7"));
        assert(throws([&] { pk.loadFile("test_protect_ring.txt"); }, "two keys named"));   // 6.2.1
        std::remove("test_protect_ring.txt");
        V3Options vr;
        vr.keyring = &pk;
        const char* RECIPE =
            "`pragma protect version=3\n"
            "`pragma protect author=\"IP Co\", author_info=\"rev 2\"\n"
            "`pragma protect data_method=\"aes256-cbc-hmac-sha512\"\n"
            "`pragma protect begin_toolblock\n"
            "`pragma protect key_keyowner=\"Bob\", key_keyname=\"bob-7\"\n"
            "`pragma protect key_method=\"rsa_no_padding\"\n"
            "`pragma protect control visibility=\"full\"\n"
            "`pragma protect end_toolblock\n"
            "// kept: a comment is HDL, not recipe (5.4.8)\n"
            "`pragma protect begin\n"
            "module r1; endmodule\n"
            "`pragma protect end\n"
            "`pragma protect begin\n"
            "module r2; endmodule\n"
            "`pragma protect end\n";
        int nr = 0;
        std::string er = encryptSourceV3(RECIPE, vr, "r.sv", &nr);
        assert(nr == 2);
        assert(er.find("begin_toolblock\n`pragma protect key_keyowner=\"Bob\"") != std::string::npos);
        assert(er.find("`pragma protect author=\"IP Co\"\n`pragma protect author_info=\"rev 2\"\n") != std::string::npos);
        assert(er.find("// kept: a comment is HDL") != std::string::npos);
        assert(er.find("`pragma protect version=3\n`pragma protect author") == std::string::npos);   // recipe removed
        Decrypted dr3 = decryptSource(er, b, "r.svp");
        assert(dr3.envs.size() == 2 && dr3.envs[1].right("visibility") == "full");
        assert(dr3.text.find("module r1; endmodule") != std::string::npos && dr3.text.find("module r2;") != std::string::npos);
        // Two envelopes from one recipe have different root keys: their key_blocks differ.
        size_t k1 = er.find("key_block\n"), k2 = er.find("key_block\n", k1 + 1);
        assert(er.substr(k1, 80) != er.substr(k2, 80));
        // A version pragma resets the recipe (5.6): no tool block is left.
        std::string reset = std::string(RECIPE).insert(std::string(RECIPE).find("// kept"), "`pragma protect version=3\n");
        assert(throws([&] { encryptSourceV3(reset, V3Options{}, "r.sv"); }, "no tool block to encrypt for"));

        // Syntax rules.
        assert(throws([&] { encryptSourceV3("`pragma protect begin\n`pragma protect begin\n`pragma protect end\n", va, "n.sv"); },
                      "nested protection"));
        assert(throws([&] { encryptSourceV3("`pragma protect begin\n`pragma protect author=\"x\"\n`pragma protect end\n", va, "n.sv"); },
                      "only viewport pragmas"));
        assert(throws([&] { encryptSourceV3("`pragma protect data_method=\"aes128-cbc\"\n", va, "n.sv"); }, "only data_method"));
        assert(throws([&] { encryptSourceV3("`pragma protect encrypt_agent=\"me\"\n", va, "n.sv"); }, "written by the encryption tool"));
        assert(throws([&] { encryptSourceV3("`pragma protect version=2\n", va, "n.sv"); }, "-v1"));
        assert(throws([&] { encryptSourceV3("`pragma protect encoding=(enctype=\"base64\")\n", va, "n.sv"); }, "not version 3"));
        {
            // The version pragma must follow begin_protected, alone; a second one is an error.
            std::string t = e3;
            t.insert(t.find("`pragma protect data_method"), "`pragma protect version=3\n");
            assert(throws([&] { decryptSource(t, a, "v.svp"); }, "second version"));
            std::string u = sub("`pragma protect version=3\n", "`pragma protect version=3, author=\"x\"\n");
            assert(throws([&] { decryptSource(u, a, "v.svp"); }, "alone"));
            std::string w = sub("`pragma protect version=3\n", "`pragma protect version=2\n");
            assert(throws([&] { decryptSource(w, a, "v.svp"); }, "version 2 envelopes are not supported"));
            std::string nosig = e3.substr(0, ep) + "`pragma protect end_protected\n";
            assert(throws([&] { decryptSource(nosig, a, "v.svp"); }, "no signature"));
        }
        // A small key is refused; IEEE 1735 needs over 512 bits and e >= 65537.
        EVP_PKEY* tiny = EVP_RSA_gen(512);
        V3Options vt;
        vt.tools.push_back(tool(tiny, "T", "t"));
        EVP_PKEY_free(tiny);
        assert(throws([&] { encryptSourceV3(SRC, vt, "t.sv"); }, "513 or more"));
        // A 2048-bit key works with a warning (3072 recommended).
        std::string warned;
        V3Options vw = v;
        vw.warn = [&](const std::string& m) { warned += m; };
        encryptSourceV3(SRC, vw, "w.sv");
        assert(warned.find("under 3072 bits") != std::string::npos);
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
