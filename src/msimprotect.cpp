// msimprotect.cpp - encrypt the `pragma protect begin ... `pragma protect end
// regions of SystemVerilog sources into decryption envelopes, in the style of
// `xmprotect` / `vcs +protect`:
//
//   msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-2026 secret.sv
//       -> secret.svp, which `msim -keyfile Acme:acme-2026=acme.pem secret.svp` runs
//
// The default output is an IEEE 1735-2023 version 3 envelope: a random root
// key RSA-encrypted without padding for each tool block, the source in
// AES-256-CBC, and an HMAC-SHA512 signature over the envelope in
// end_protected. -v1 writes the older IEEE 1800-2023 clause 34 envelope
// (PKCS#1 v1.5 key_block, digest_block) instead.
//
// Recipe pragmas in the source (version, author, author_info, data_method,
// begin_toolblock with key_keyowner / key_keyname / key_method / control /
// key_public_key, begin_commonblock) apply to the regions after them, and are
// removed from the output. Text outside begin/end regions is copied unchanged.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "protect.hpp"
#include "version.hpp"

static void usage() {
    std::cerr << "usage: msimprotect [options] <files...>\n"
                 "  -key <public.pem>      encrypt for this RSA key: one tool block (repeatable)\n"
                 "  -keyowner <o>          key_keyowner of the -key before it (default minisim)\n"
                 "  -keyname <n>           key_keyname of the -key before it (default: the file name)\n"
                 "  -keyring <file>        public keys by key_keyowner / key_keyname: IEEE 1735 6.3.1\n"
                 "                         key exchange pragmas, or lines `<owner> <name> <public.pem>`.\n"
                 "                         With no -key and no recipe tool block, every key in it is used\n"
                 "  -control <right>=<v>   a right in the tool block of the -key before it (before any\n"
                 "                         -key: in every command-line tool block); see docs/PROTECT.md\n"
                 "  -author <s>, -author_info <s>   author pragmas when the source has none\n"
                 "  -o <file>              output file (one input only; default <input>p)\n"
                 "  -all                   encrypt files that have no `pragma protect begin as a whole\n"
                 "  -export-key <pem>      print the public key in the 1735 key exchange format\n"
                 "                         (with -keyowner / -keyname) and exit\n"
                 "  -v1                    write IEEE 1800-2023 clause 34 envelopes instead of 1735 v3:\n"
                 "    -data_method <m>     aes256-cbc (default), aes192-cbc, aes128-cbc, 3des-cbc\n"
                 "    -digest <m>          sha256 (default), sha1, md5, none\n"
                 "    -encoding <e>        base64 (default) or uuencode\n";
}

int main(int argc, char** argv) {
    protect::EncryptOptions o1;           // -v1
    protect::V3Options o3;
    protect::PublicKeyring ring;
    o3.agentInfo = std::string("minisim ") + MINISIM_VERSION;
    o3.warn = [](const std::string& m) { std::cerr << "msimprotect: " << m << "\n"; };
    std::vector<std::string> inputs, globalControls;
    std::string outPath, exportPem, exportOwner = "minisim", exportName;
    bool all = false, v1 = false, haveRing = false;
    bool lastKeyAdded = false;
    try {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) { std::cerr << "msimprotect: " << a << " needs a value\n"; std::exit(2); }
                return argv[++i];
            };
            if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
            if (a == "-version" || a == "--version") { std::cout << "msimprotect " << MINISIM_VERSION << "\n"; return 0; }
            if (a == "-key") {
                std::string path = next();
                size_t slash = path.find_last_of("/\\");
                std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
                protect::Recipient r;
                r.owner = "minisim";
                r.name = name;
                r.pub = protect::loadPublicKey(path);
                o1.recipients.push_back(r);
                protect::ToolRecipe t;
                t.owner = r.owner;
                t.name = r.name;
                t.pub = r.pub;
                o3.tools.push_back(std::move(t));
                lastKeyAdded = true;
            } else if (a == "-keyowner" || a == "-keyname") {
                std::string v = next();
                if (!exportPem.empty() && !lastKeyAdded) {
                    (a == "-keyowner" ? exportOwner : exportName) = v;
                    continue;
                }
                if (o3.tools.empty()) { std::cerr << "msimprotect: " << a << " must follow a -key\n"; return 2; }
                (a == "-keyowner" ? o1.recipients.back().owner : o1.recipients.back().name) = v;
                (a == "-keyowner" ? o3.tools.back().owner : o3.tools.back().name) = v;
            } else if (a == "-keyring") {
                ring.loadFile(next());
                haveRing = true;
            } else if (a == "-control") {
                std::string c = next();
                size_t eq = c.find('=');
                if (eq == std::string::npos || eq == 0) { std::cerr << "msimprotect: -control needs <right>=<value>\n"; return 2; }
                std::string expr = c.substr(0, eq) + " = " + protect::quote(c.substr(eq + 1));
                if (o3.tools.empty()) globalControls.push_back(expr);
                else o3.tools.back().controls.push_back(expr);
            } else if (a == "-author") {
                o3.author = next();
            } else if (a == "-author_info") {
                o3.authorInfo = next();
            } else if (a == "-export-key") {
                exportPem = next();
                lastKeyAdded = false;
            } else if (a == "-v1") {
                v1 = true;
            } else if (a == "-data_method") {
                o1.dataMethod = next();
                protect::fetchCipher(o1.dataMethod);
                v1 = true;
            } else if (a == "-digest") {
                o1.digestMethod = next();
                if (o1.digestMethod == "none") o1.digestMethod.clear();
                else if (!protect::digestName(o1.digestMethod)) { std::cerr << "msimprotect: unknown digest " << o1.digestMethod << "\n"; return 2; }
                v1 = true;
            } else if (a == "-encoding") {
                o1.enctype = next();
                if (o1.enctype != "base64" && o1.enctype != "uuencode") { std::cerr << "msimprotect: unknown encoding " << o1.enctype << "\n"; return 2; }
                v1 = true;
            } else if (a == "-o") {
                outPath = next();
            } else if (a == "-all") {
                all = true;
            } else if (!a.empty() && a[0] == '-') {
                std::cerr << "msimprotect: unknown option " << a << "\n";
                usage();
                return 2;
            } else {
                inputs.push_back(a);
            }
        }
        if (!exportPem.empty()) {
            protect::PublicKeyEntry e;
            e.owner = exportOwner;
            e.name = exportName;
            if (e.name.empty()) {
                size_t slash = exportPem.find_last_of("/\\");
                e.name = slash == std::string::npos ? exportPem : exportPem.substr(slash + 1);
            }
            e.pub = protect::loadPublicKey(exportPem);
            protect::v3::checkKey(e.pub.get());
            std::string text = protect::exportKey(e);
            if (outPath.empty()) std::cout << text;
            else {
                std::ofstream f(outPath, std::ios::binary);
                f << text;
                if (!f) { std::cerr << "msimprotect: cannot write " << outPath << "\n"; return 1; }
            }
            return 0;
        }
        if (inputs.empty()) { usage(); return 2; }
        if (!outPath.empty() && inputs.size() != 1) { std::cerr << "msimprotect: -o needs exactly one input\n"; return 2; }
        if (v1 && haveRing) { std::cerr << "msimprotect: -keyring is for version 3 envelopes (drop -v1)\n"; return 2; }
        if (haveRing) {
            o3.keyring = &ring;
            if (o3.tools.empty() && !globalControls.empty()) {
                // Rights for every keyring key used.
                for (auto& k : ring.keys) {
                    protect::ToolRecipe t;
                    t.owner = k.owner;
                    t.name = k.name;
                    t.pub = k.pub;
                    o3.tools.push_back(std::move(t));
                }
            }
        }
        for (auto& t : o3.tools)
            t.controls.insert(t.controls.begin(), globalControls.begin(), globalControls.end());

        for (auto& in : inputs) {
            std::string text = protect::readFile(in);
            int n = 0;
            auto run = [&](const std::string& t) {
                return v1 ? protect::encryptSource(t, o1, in, &n) : protect::encryptSourceV3(t, o3, in, &n);
            };
            std::string enc = run(text);
            if (n == 0 && all) {
                text = "`pragma protect begin\n" + text + (text.empty() || text.back() == '\n' ? "" : "\n") +
                       "`pragma protect end\n";
                enc = run(text);
            }
            OPENSSL_cleanse(text.data(), text.size());
            std::string out = outPath.empty() ? in + "p" : outPath;
            if (out == in) { std::cerr << "msimprotect: refusing to overwrite the input " << in << "\n"; return 2; }
            std::ofstream f(out, std::ios::binary);
            f << enc;
            if (!f) { std::cerr << "msimprotect: cannot write " << out << "\n"; return 1; }
            if (n == 0) std::cerr << "msimprotect: warning: " << in << " has no `pragma protect begin; copied unchanged (use -all)\n";
            else std::cerr << "msimprotect: " << in << " -> " << out << " (" << n << " envelope" << (n == 1 ? "" : "s") << ")\n";
        }
    } catch (const protect::Error& ex) {
        std::cerr << "msimprotect: " << ex.what() << "\n";
        return 1;
    }
    return 0;
}
