// msimprotect.cpp - encrypt the `pragma protect begin ... `pragma protect end
// regions of SystemVerilog sources into IEEE 1800-2023 clause 34 / IEEE 1735
// decryption envelopes (begin_protected ... end_protected), in the style of
// `xmprotect` / `vcs +protect`:
//
//   msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-2026 secret.sv
//       -> secret.svp, which `msim -keyfile acme-2026=acme_priv.pem secret.svp` runs
//
//   -key <public.pem>     encrypt the session key for this key (repeatable;
//                         each -key adds one key_block). A private key PEM
//                         works too: only its public half is used.
//   -keyowner <name>      key_keyowner for the -key before it (default "minisim")
//   -keyname <name>       key_keyname for the -key before it (default: the PEM file name)
//   -data_method <m>      aes128-cbc, aes192-cbc, aes256-cbc (default), 3des-cbc, ...
//   -digest <m>           digest_method for the digest_blocks (default sha256; none: no digests)
//   -encoding <e>         base64 (default) or uuencode
//   -o <file>             output file (one input only); default <input>p (x.sv -> x.svp)
//   -all                  encrypt the whole file even where it has no `pragma protect begin
//
// Text outside begin/end regions is copied unchanged, and keywords given in
// the source (key_keyowner, key_public_key, data_method, encoding, ...)
// apply to the envelopes that follow them.
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "protect.hpp"

static void usage() {
    std::cerr << "usage: msimprotect -key <public.pem> [-keyowner <o>] [-keyname <n>] [options] <files...>\n"
                 "  -data_method <m>  aes256-cbc (default), aes192-cbc, aes128-cbc, 3des-cbc\n"
                 "  -digest <m>       sha256 (default), sha1, md5, none\n"
                 "  -encoding <e>     base64 (default) or uuencode\n"
                 "  -o <file>         output file (one input only; default <input>p)\n"
                 "  -all              encrypt files that have no `pragma protect begin as a whole\n";
}

int main(int argc, char** argv) {
    protect::EncryptOptions o;
    std::vector<std::string> inputs;
    std::string outPath;
    bool all = false;
    try {
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) { std::cerr << "msimprotect: " << a << " needs a value\n"; std::exit(2); }
                return argv[++i];
            };
            if (a == "-h" || a == "-help" || a == "--help") { usage(); return 0; }
            if (a == "-key") {
                std::string path = next();
                protect::Recipient r;
                r.owner = "minisim";
                size_t slash = path.find_last_of('/');
                r.name = slash == std::string::npos ? path : path.substr(slash + 1);
                r.pub = protect::loadPublicKey(path);
                o.recipients.push_back(std::move(r));
            } else if (a == "-keyowner" || a == "-keyname") {
                std::string v = next();
                if (o.recipients.empty()) { std::cerr << "msimprotect: " << a << " must follow a -key\n"; return 2; }
                (a == "-keyowner" ? o.recipients.back().owner : o.recipients.back().name) = v;
            } else if (a == "-data_method") {
                o.dataMethod = next();
                protect::fetchCipher(o.dataMethod);
            } else if (a == "-digest") {
                o.digestMethod = next();
                if (o.digestMethod == "none") o.digestMethod.clear();
                else if (!protect::digestName(o.digestMethod)) { std::cerr << "msimprotect: unknown digest " << o.digestMethod << "\n"; return 2; }
            } else if (a == "-encoding") {
                o.enctype = next();
                if (o.enctype != "base64" && o.enctype != "uuencode") { std::cerr << "msimprotect: unknown encoding " << o.enctype << "\n"; return 2; }
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
        if (inputs.empty()) { usage(); return 2; }
        if (!outPath.empty() && inputs.size() != 1) { std::cerr << "msimprotect: -o needs exactly one input\n"; return 2; }

        for (auto& in : inputs) {
            std::string text = protect::readFile(in);
            int n = 0;
            std::string enc = protect::encryptSource(text, o, in, &n);
            if (n == 0 && all) {
                text = "`pragma protect begin\n" + text + (text.empty() || text.back() == '\n' ? "" : "\n") +
                       "`pragma protect end\n";
                enc = protect::encryptSource(text, o, in, &n);
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
