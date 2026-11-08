# Protected IP (IEEE 1800-2023 clause 34, IEEE 1735)

An IP vendor can ship a SystemVerilog model whose source is encrypted, so a
customer can simulate it without reading it. minisim implements the protected
envelopes of IEEE 1800-2023 clause 34, the `` `pragma protect `` format that
IEEE 1735 also uses:

- `msimprotect` turns each `` `pragma protect begin `` … `` `pragma protect end ``
  region into a `begin_protected` … `end_protected` decryption envelope
  (34.3.1, like `xmprotect` or `vcs +protect`).
- `msim -keyfile <private.pem>` replaces each envelope with its cleartext
  before compiling (34.3.2), in memory only.

Both use OpenSSL's libcrypto; nothing in minisim implements a cipher itself.
They are built when cmake finds OpenSSL (`libssl-dev` on Debian / Ubuntu).

## How an envelope is made

For each `begin`/`end` region, `msimprotect`:

1. draws a fresh random session key for the `data_method` (default
   `aes256-cbc`, so 32 bytes);
2. encrypts the region with it in CBC mode, with a random IV as the first
   cipher block (34.5.15): the `data_block`;
3. encrypts the session key with each recipient's RSA public key (key_method
   `rsa`, PKCS#1 v1.5 padding): one `key_block` per `-key`;
4. adds a `digest_block` after each key_block and data_block (34.5.22): the
   SHA-256 of the cleartext, encrypted with the session key, so a modified
   envelope is refused instead of decrypting to garbage;
5. writes the recipient's public key as `key_public_key` (DER, so a decrypting
   tool can pick the matching private key without trying every one), and base64
   encodes every block, with its decoded length in `encoding=(… bytes=N)`.

```systemverilog
module secret_alu(input logic clk, input logic [7:0] a, input logic [7:0] b, output logic [7:0] y);
`pragma protect begin_protected
`pragma protect encrypt_agent="msimprotect", encrypt_agent_info="minisim"
`pragma protect digest_method="sha256"
`pragma protect key_keyowner="Acme", key_keyname="acme-1", key_method="rsa"
`pragma protect encoding=(enctype="base64", line_length=76, bytes=294)
`pragma protect key_public_key
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEA...
`pragma protect encoding=(enctype="base64", line_length=76, bytes=256)
`pragma protect key_block
...
`pragma protect data_method="aes256-cbc"
`pragma protect encoding=(enctype="base64", line_length=76, bytes=208)
`pragma protect data_block
...
`pragma protect end_protected
`pragma reset protect
endmodule
```

Text outside the regions, including the module header and ports in this
example, is copied unchanged. Envelopes that are already `begin_protected`
are copied too, or encrypted again as plain bytes when they sit inside a
`begin`/`end` region (34.5.1); msim then decrypts both layers.

## msimprotect

```sh
msimprotect -key acme_pub.pem -keyowner Acme -keyname acme-1 secret_alu.sv      # -> secret_alu.svp
msimprotect -key acme_pub.pem -key other_pub.pem -o out/alu.svp secret_alu.sv   # two recipients
msimprotect -key acme_pub.pem -all whole_file.svh -o inc/whole_file.svh         # no begin/end needed
```

| Option | Meaning |
|---|---|
| `-key <public.pem>` | Add a recipient: one key_block encrypted with this RSA key. A private key PEM also works; only its public half is used. Repeatable |
| `-keyowner <name>`, `-keyname <name>` | `key_keyowner` / `key_keyname` of the `-key` before it (defaults `minisim` and the PEM file name) |
| `-data_method <m>` | `aes256-cbc` (default), `aes192-cbc`, `aes128-cbc`, `3des-cbc`; `des-cbc`, `blowfish-cbc`, `cast128-cbc` with OpenSSL's legacy provider |
| `-digest <m>` | `sha256` (default), `sha1`, `md5`, `sha384`, `sha512`, `ripemd-160`, or `none` |
| `-encoding <e>` | `base64` (default) or `uuencode` |
| `-o <file>` | Output file for a single input. Default: the input name plus `p` (`x.sv` → `x.svp`) |
| `-all` | Encrypt a file that has no `begin` as one whole region |

Keywords written in the source apply too, in the lexical order of 34.4:
`key_keyowner`, `key_keyname`, `key_public_key` (adds a recipient),
`data_method`, `encoding`, `digest_method`, `digest_block` (asks for digests),
`author`, `author_info`.

## msim -keyfile

```sh
msim -keyfile acme.pem secret_alu.svp alu_tb.sv
msim -keyfile acme-1=acme.pem -keyfile other.pem ...
```

A key given as `name=file` only opens key_blocks whose `key_keyname` is
`name`, or whose `key_public_key` is that key. A plain file is tried on every
key_block; a wrong guess is rejected by the digest or the CBC padding. Keys
must be unencrypted PEM RSA private keys (msim does not prompt for a
passphrase). Envelopes from another tool open when they use the methods
above; `tests/msim_protect.cmake` runs one built with the `openssl` command
line tool alone.

msim finds envelopes in the source files, and in files with a Verilog
extension (`.v .sv .vh .svh .vp .svp .inc .svi .vg`) in their directories and
in `+incdir+` / `-I` / `-y` directories given on the msim command line, so a
protected `` `include `` file works when its directory is given that way.

What msim does to keep protected text from leaking:

- The cleartext only exists in memory; nothing is written to disk.
- A diagnostic whose location, macro expansion, range, or note points into
  protected code prints as `file: error: in protected code (details hidden)`,
  with no message, names, or source line.
- Signals declared in protected code are left out of the VCD. Ports declared
  in the clear stay in it.
- A lowering error ("construct not supported yet") in a design with protected
  code is reported without its details, since it may name a protected signal.
- `-compile` runs the interpreter instead, because the generated C++ would
  hold the design in the clear.
- Line numbers after an envelope are those of the encrypted file (msim
  inserts a `` `line `` directive), so errors in the clear parts point to the
  right line.

Protected code still runs: its `$display` output and `%m` names print as the
vendor wrote them.

## Not yet

- `decrypt_license`, `runtime_license`, and `viewport` are read and ignored.
- `key_method` other than `rsa` (a symmetric key_block, `pgp-rsa`, `elgamal`),
  and `data_decrypt_key` / `digest_decrypt_key`.
- The IEEE 1735 version 2 "rights" (`control` pragmas) and the vendor key
  rings of commercial tools: an envelope encrypted for a commercial
  simulator's key only opens with that vendor's private key, which minisim
  does not have.
