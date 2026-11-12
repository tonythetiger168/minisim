# Protected IP (IEEE 1735-2023, IEEE 1800-2023 clause 34)

An IP vendor can ship a SystemVerilog model whose source is encrypted, so a
customer can simulate, lint and test it without reading it. minisim
implements the IEEE 1735-2023 recommended practice (version 3 envelopes)
and the older envelopes of IEEE 1800-2023 clause 34:

- `msimprotect` turns each `` `pragma protect begin `` … `` `pragma protect end ``
  region into a `begin_protected` … `end_protected` decryption envelope, like
  `xmprotect` or `vcs +protect`. Version 3 is the default; `-v1` writes the
  clause 34 form.
- `msim`, `mlint` and `mdft` take `-keyfile` / `-keyring`, verify and decrypt
  each envelope in memory before compiling, and keep the protected code out
  of what they print and write: diagnostics, waveforms, coverage, lint
  findings, the DFT chain plan.

All cryptography is OpenSSL's libcrypto (RSA, AES, HMAC, the random
generator); nothing in minisim implements a cipher. The tools are built when
cmake finds OpenSSL (`libssl-dev` on Debian / Ubuntu).

## Quick start

```sh
# The IP vendor's key pair (the private key stays with the tool that decrypts).
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out acme.pem
openssl pkey -in acme.pem -pubout -out acme_pub.pem

# Encrypt: one tool block for the key, rights in it.
msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-1 \
            -control visibility=interface examples/protect/secret_core.sv    # -> secret_core.svp

# Simulate, lint, check DFT.
msim  -keyfile Acme:acme-1=acme.pem secret_core.svp examples/protect/core_tb.sv -vcd waves.vcd
mlint -keyfile Acme:acme-1=acme.pem secret_core.svp examples/protect/core_tb.sv
```

## The version 3 envelope

```systemverilog
`pragma protect begin_protected
`pragma protect version=3
`pragma protect encrypt_agent="msimprotect"
`pragma protect encrypt_agent_info="minisim 0.1.0"
`pragma protect data_method="aes256-cbc-hmac-sha512"
`pragma protect begin_toolblock
`pragma protect key_keyowner="Acme", key_keyname="acme-1"
`pragma protect key_method="rsa_no_padding"
`pragma protect key_block
<base64: the root key, RSA encrypted for this tool>
`pragma protect control visibility = "interface"
`pragma protect end_toolblock
`pragma protect data_block
<base64: IV + AES-256-CBC of the source>
`pragma protect end_protected="<base64 HMAC-SHA512 signature>"
```

For each envelope `msimprotect` draws a fresh random 64-byte root key and a
fresh IV. Its first 32 bytes are the AES-256-CBC key of the `data_block`; its
last 32 bytes are the HMAC-SHA512 key of the signature. For every tool block
the root key goes in the low 64 bytes of a random number below the RSA
modulus, encrypted without padding (`rsa_no_padding`); a key needs a public
exponent of at least 65537 and more than 512 bits, and `msimprotect` warns
below 3072. The signature covers the envelope text from the
`begin_protected` line to `end_protected`, with carriage returns removed, so
CR LF line ends do not matter and any other change does.

On decryption the tool picks the first tool block whose `key_keyowner` /
`key_keyname` it has a key for, checks the signature, and only then
decrypts. A changed envelope (a right, a name, one base64 character) gives
one error, `signature verification failed`, and nothing else. Envelopes may
not nest in version 3. Version 1 and 2 envelopes are refused with a message;
clause 34 envelopes (no `version`) still open.

minisim's decryptor and encryptor were checked against the test vector of
IEEE 1735-2023 Annex B (its RSA block, key split, AES and HMAC all match).
The vector is copyrighted with the standard, so it is not in this
repository; `tests/msim_protect.cmake` instead builds a version 3 envelope
with the `openssl` command line tool alone and runs it, and checks the
signature of `msimprotect`'s output with `openssl dgst`.

## msimprotect

```sh
msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-1 secret.sv        # -> secret.svp
msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-1 \
            -key xyz_pub.pem  -keyowner XYZ  -keyname sim-2026 secret.sv       # two tool blocks
msimprotect -keyring vendors.keys secret.sv                                    # every key in the file
msimprotect -key acme_pub.pem -all whole_file.svh -o inc/whole_file.svh        # no begin/end needed
msimprotect -export-key acme_pub.pem -keyowner Acme -keyname acme-1 > acme.keys
```

| Option | Meaning |
|---|---|
| `-key <public.pem>` | Add a tool block for this RSA key (a private key PEM works too: only its public half is used). Repeatable |
| `-keyowner <o>`, `-keyname <n>` | `key_keyowner` / `key_keyname` of the `-key` before it (defaults `minisim` and the file name) |
| `-keyring <file>` | Public keys by owner and name (below). With no `-key` and no tool block in the source, every key in it gets a tool block |
| `-control <right>=<value>` | A right in the tool block of the `-key` before it; before any `-key`, in every command-line tool block |
| `-author <s>`, `-author_info <s>` | Author pragmas when the source has none |
| `-o <file>` | Output for a single input. Default: the input name plus `p` (`x.sv` → `x.svp`) |
| `-all` | Encrypt a file that has no `begin` as one region |
| `-export-key <pem>` | Print the public key in the IEEE 1735 key exchange format, for IP authors |
| `-v1` | Write IEEE 1800-2023 clause 34 envelopes; then `-data_method`, `-digest`, `-encoding` apply |

### Recipes in the source

The encryption recipe can also be written in the source, as IEEE 1735
describes; it applies to every `begin` / `end` region after it, each of
which gets its own envelope with its own root key. Recipe pragmas are
removed from the output; comments around them stay (they are HDL).

```systemverilog
`pragma protect version=3
`pragma protect author="Acme IP", author_info="core rev 4"
`pragma protect data_method="aes256-cbc-hmac-sha512"
`pragma protect begin_toolblock
`pragma protect key_keyowner="Acme", key_keyname="acme-1"
`pragma protect key_method="rsa_no_padding"
`pragma protect control visibility = license("acme_debug") ? "full" : "none"
`pragma protect key_public_key
MIIBigKCAYEA...            (DER RSAPublicKey; or leave it out and give -key / -keyring)
`pragma protect end_toolblock
`pragma protect begin
module core(...);
  `pragma protect viewport
  logic [7:0] status;      // visible in waves and coverage
  ...
endmodule
`pragma protect end
```

A `version` pragma resets the recipe. Between `begin` and `end` only
`viewport` / `interface_viewport` pragmas may appear; a nested `begin` or
`begin_protected` is an error. `begin_commonblock` … `end_commonblock`
(license_proxyenv, license_certificate) is copied into the envelope.

## Keyrings

Two files, both plain text:

- **Public keys** (`msimprotect -keyring`): the IEEE 1735 pragma exchange
  format, which is how a tool vendor hands its public key to IP authors
  (`msimprotect -export-key` writes it), and table lines.

  ```
  `protect key_keyowner = "Acme"
  `protect key_method = "rsa_no_padding"
  `protect key_keyname = "acme-1"
  `protect key_public_key
  MIIBigKCAYEA...
  "XYZ Corp"  sim-2026  xyz_pub.pem
  ```

- **Private keys** (`msim -keyring`, also mlint and mdft): one key per line,
  `<key_keyowner> <key_keyname> <private.pem>`, the path relative to the
  keyring; quote names with spaces. Keep this file and the PEMs private.

  ```
  # owner   name      key
  "Acme"    acme-1    keys/acme.pem
  XYZ       sim-2026  keys/xyz.pem
  ```

Two keys with the same owner and name are an error. `-keyfile` adds one key:
`Acme:acme-1=acme.pem` (owner and name), `acme-1=acme.pem` (name, any owner),
or `acme.pem` (tried on every tool block; only a valid signature accepts it).
Keys must be unencrypted PEM RSA private keys.

## Rights

Rights are `control` pragmas in a tool block (IEEE 1735 clause 7); each tool
defines its own. minisim reads these from the tool block it opened:

| Right | Values | Effect |
|---|---|---|
| `decryption` | `"allow"` (default), `"deny"` | `deny`: msim, mlint and mdft refuse the IP |
| `visibility` | `"none"` (default), `"interface"`, `"full"` | What the tools show of protected objects: only viewports; also the ports of protected modules; everything, including diagnostics |
| `coverage` | `"off"` (default), `"on"` | Line and branch coverage inside the protected code (toggle coverage follows `visibility`) |
| `min_version` | `"0.2"`, … | The oldest minisim that may open the IP |

Other rights are ignored with a warning. A right may depend on a license:
`right = license("feature") ? "a" : "b"` or `right = license_string("feature")`.

## Licenses

A licensed right is decided by the IP author's license proxy, not by the tool
or its user (IEEE 1735-2023 clause 8). The envelope's common block names the
proxy, and msim, mlint and mdft ask it over TLS each time they decrypt. Any
proxy that speaks clause 8 will do; `mlicproxy` is one, and ships with
`msimprotect` in the paid edition.

The IP author, once:

```sh
# The proxy's certificate and key. 8.7.1 asks for RSA of 3072 bits or more, or a 256-bit curve.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
    -subj /CN=acme-license-proxy -keyout proxy_key.pem -out proxy_cert.pem
# The common block for the IP source: the environment variable's name and the certificate.
mlicproxy -cert proxy_cert.pem -pragmas ACME_PROXY > common.svh
```

```systemverilog
`pragma protect version=3
`pragma protect begin_commonblock
`pragma protect license_proxyenv = "ACME_PROXY"
`pragma protect license_certificate
MIIBfTCCASOgAwIBAgIU...
`pragma protect end_commonblock
`pragma protect begin_toolblock
`pragma protect key_keyowner="Acme", key_keyname="acme-1"
`pragma protect key_method="rsa_no_padding"
`pragma protect control visibility = license("acme_debug") ? "full" : "none"
`pragma protect end_toolblock
`pragma protect begin
module secret_core ...
```

Where the IP is used, the proxy runs with a license file and the variable
points at it:

```sh
mlicproxy -cert proxy_cert.pem -key proxy_key.pem -port 27100 -licenses acme.lic
export ACME_PROXY="27100@licserver;27100@backup"     # port@host pairs, tried in order
msim -keyfile Acme:acme-1=acme.pem secret_core.svp tb.sv
```

```
# acme.lic: one feature per line
acme_debug                          # granted
acme_tier = "gold"                  # granted; license_string("acme_tier") gives gold
acme_eval expires 2027-06-30        # granted through that day (UTC)
```

How the tool side behaves, by subclause:

- **8.3, 8.7.** `$ACME_PROXY` is read when the call is evaluated: `port@host`
  pairs separated by `;`, the host a name, a fully qualified name, or an IPv4
  or IPv6 address. The first pair that gives a secure connection is used. A
  later pair is tried only when the connection fails, never because the
  answer was "denied".
- **8.3.2, 8.7.1.** The connection is secure when TLS is up and the proxy's
  certificate is byte for byte the envelope's `license_certificate`. No
  certificate authority, validity period or revocation list is consulted.
  TLS 1.2 is the oldest version accepted and TLS 1.3 is supported; the cipher
  suites use 256-bit keys with SHA-384; sessions are never resumed. One
  connection per proxy is kept open for the run.
- **8.8.** Messages are DER: `SEQUENCE { INTEGER 3, INTEGER request id, ... }`
  with IA5String arguments. Request ids count up from 1 on each connection.
  A reply's third field is the `license` value, the fourth (optional) the
  `license_string` value, the fifth (optional) an explanatory message, which
  the tool prints as `license proxy: ...`. Messages with request id 0 are
  printed and not answered. The standard's examples write the sequence
  identifier as `0x10`, and X.690 writes it as `0x30`: both are read, `0x10`
  is written, and `MINISIM_LICENSE_SEQTAG=30` writes `0x30` for a proxy that
  insists on it.
- **8.7.1.** A reply that is not a well-formed message, or that carries a
  request id the tool did not send, closes the connection; a new one is
  negotiated and the request sent again, once per pair.
- **8.7.2.** A proxy that does not answer within 10 seconds
  (`MINISIM_LICENSE_TIMEOUT=<seconds>`) is given up on and the next pair tried.
- **8.6.** The same call on the same proxy is asked once per run.
- **8.9.** With no secure connection (no `license_proxyenv` or
  `license_certificate`, the variable unset or malformed, no pair reachable,
  the wrong certificate), the tool warns and goes on as if `license` returned
  false and `license_string` returned `""`. `-license_fail stop` makes it an
  error instead.

`mlicproxy` answers existence checks only (8.5): nothing is counted or
checked out, and the answer depends on the license file and the date. It
re-reads the file when it changes. `-banner <text>` sends a request-id-0
message to each tool that connects, `-log` prints each request, and
`-bind <address>` limits where it listens.

The earlier `-license <feature>` option is gone. It let the IP user grant a
licensed right to themselves.

## Viewports

`` `pragma protect viewport `` inside protected text makes objects visible
whatever the rights (IEEE 1735 9.5):

- with no name, the declaration after it (`logic [7:0] a, b;`) or, before a
  `module`, the whole module;
- with a name, that object in the code after it: `sig`, `mod.sig`,
  `mod.genblk.sig`, or `mod` for a whole module;
- `interface_viewport [mod]`: the ports of the next (or named) module.

The pragmas are encrypted with the source; msim blanks their lines before
the compiler sees them.

## What the tools hide

Unless `visibility = "full"`:

- **msim diagnostics**: an error or warning whose location, macro expansion,
  range or note points into protected code prints as
  `file: error: in protected code (details hidden)`. A construct msim cannot
  simulate yet is reported without its details.
- **Assertion failures**: the default `$error` of an assertion in protected
  code prints `"file", protected code: tb.u.<protected assertion>`, with no
  label or line. The IP's own `$display` and `%m` print as written: that is
  the model's behavior, not the tool's.
- **Waveforms (VCD / FST)**: signals declared in protected code are left out,
  except viewport objects (and ports with `visibility = "interface"`).
  Signals declared in the clear stay, unless they sit below an instance of
  protected code: a clear module instantiated inside the IP, or bound into it
  with `bind`, is hidden, because its path goes through the IP (1735 9.2,
  9.4.2.2). msim's own temporaries are left out too.
- **Coverage (`-cov`, mcov)**: no line or branch entries for protected code
  (unless `coverage = "on"`), and no toggle entries for hidden signals.
- **mlint / mdft**: findings located in protected code are counted, not
  shown (`mlint: 1 finding in protected code (details hidden)`), and hidden
  objects in other findings and in the chain plan print as `<protected>`.
- **-compile** runs the interpreter, because the generated C++ would hold the
  design in the clear.
- The cleartext only exists in memory. Line numbers after an envelope are
  those of the encrypted file (msim inserts a `` `line `` directive).

msim finds envelopes in the source files and in files with a Verilog
extension (`.v .sv .vh .svh .vp .svp .inc .svi .vg`) in their directories and
in `+incdir+` / `-I` / `-y` directories, so protected `` `include `` files and
library cells work. An envelope in such an extra file that does not open is
only an error if the design reads it.

## VPI and DPI

A user's C code cannot be used to look into protected IP (IEEE 1800-2023
37.3.6 and clause 38, IEEE 1735-2023 9.7). An object is protected when it is
declared in an envelope and no right or viewport makes it visible, or when it
sits below an instance of a protected module (9.2: a path through protected
IP is opaque, even to clear text below it).

- `vpi_get(vpiIsProtected, h)` is 1 for such an object. `vpiType` can be
  read; every other property, `vpi_get_str`, `vpi_handle`, `vpi_iterate`,
  `vpi_handle_by_index`, `vpi_get_value` and `vpi_put_value` (force and
  release included) are errors that `vpi_chk_error` reports.
- `vpi_handle_by_name` is an error for a protected scope and for a name in
  protected code, disclosed or not. Objects the IP author opened with a
  viewport or a right are found by their full name.
- `vpi_register_cb` puts no callback on a protected object: it returns NULL
  and prints an error message.
- Model fidelity: a system task or function the IP itself calls keeps
  working. Its arguments can be iterated (38.36) and their `vpiSize` read
  (37.57), and while its `calltf` runs it reads and writes its own arguments
  and its result. A handle kept from such a call is closed again when the
  call returns.
- DPI: an import called from protected code runs as usual.
  `svGetNameFromScope` gives `<protected>` for a scope in protected IP and
  `svGetCallerInfo` gives `<protected>` and line 0 for a call in protected
  code. `svGetScopeFromName` with the disclosed name still returns a scope
  that `svSetScope` and the user data functions accept.

## Commercial tools

A version 3 envelope opens in any IEEE 1735 tool that holds the private key
of one of its tool blocks. To encrypt for a commercial simulator, get that
vendor's public key (in the key exchange format above) and add it with
`-keyring`; each tool block carries its own rights. The reverse, opening an
envelope encrypted only for a commercial tool, needs that vendor's private
key, which minisim does not have and does not try to obtain.

## IEEE 1735-2023 coverage

| Clause | Topic | Status |
|---|---|---|
| 5.2 | Strings are printable ASCII without `"` or `\` (no escapes); keywords and standardized strings lower case; key names and rights may not differ in case alone; an end-of-file character inside an envelope is an error | ✅ `test_protect`; end to end beside `msimprotect` |
| 5.3–5.4 | Version 3 cryptography: RSA no-padding root key, AES-256-CBC, HMAC-SHA512, base64 | ✅ |
| 5.4.9 | version=3 second and alone; one version per envelope | ✅ |
| 5.5 | Envelope anatomy: author / agent pragmas, common block, tool blocks, data_block | ✅ |
| 5.6 | eTool input: recipes, reset on version, no nesting, viewports verbatim, one root key per region | ✅ |
| 5.6.2, 6.3 | DER RSAPublicKey, pragma key exchange format | ✅ |
| 5.7 | Transform tools (re-encryption with the same root key) | — (minisim is not a tTool) |
| 5.8 | Signature over the canonical envelope text, checked before decryption | ✅ |
| 6.2.1 | Keys named by owner and name, duplicates refused | ✅ |
| 7 | Rights in tool blocks, licensed rights syntax | ✅ (minisim rights above) |
| 8 | License system: license specification, licensing calls, TLS connection to the proxy, DER messages, request id 0, return value reuse, irregularities; `mlicproxy` is a proxy | ✅ `test_protect`; msim against a proxy beside `msimprotect` |
| 9.2 | Error handling and model fidelity | ✅ msim, mlint, mdft |
| 9.1, 9.3 | Build phase: the user's HDL can reference protected names, nothing is shown; run phase: protected names are not shown or accepted | ✅ end to end beside `msimprotect` |
| 9.4 | Protected text regions; child visibility: clear modules instantiated in, or bound into, protected IP are hidden, since their path goes through it | ✅ `test_protect`; end to end beside `msimprotect` |
| 9.5–9.6 | viewport / interface_viewport | ✅ named (a list, `.` or `:` between scopes) and unnamed; named ones match by lexical name |
| 9.7 | VPI / DPI visibility | ✅ see "VPI and DPI"; end to end beside `msimprotect` |
| 9.8 | Visibility through rights | ✅ `visibility`, `coverage` |
| 9.9 | Dynamic objects: the named handle, queue or array variables in the IP are protected; the objects themselves stay usable through them | ✅ `test_protect`; end to end beside `msimprotect` |
| Annex A | Syntax: what a tool block and a common block may hold, BASE64_TEXT without spaces or inner padding, INTEGER version, STRING; msimprotect output checked against it | ✅ `test_protect`; end to end beside `msimprotect` |
| Annex B | Test vector | ✅ checked locally (not redistributable) |

## IEEE 1800-2023 clause 34 envelopes (`-v1`)

The older format: a session key for the `data_method` (default
`aes256-cbc`), RSA PKCS#1 v1.5 `key_block`s (key_method `rsa`), a
`digest_block` after each key_block and data_block (default SHA-256), the
recipient's `key_public_key`, and `encoding=(enctype=..., bytes=...)`
directives. `msim` opens these with the same `-keyfile` options, including
envelopes nested in decrypted text and envelopes made with the `openssl` tool
alone. Not supported there: `decrypt_license`, `runtime_license`, the 1800
`viewport = (object=..., access=...)` form, `key_method` other than `rsa`,
and `data_decrypt_key` / `digest_decrypt_key`.
