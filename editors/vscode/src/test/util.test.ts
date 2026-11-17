// Unit tests of the parts of the extension that do not need VS Code:
//   npm run compile && npm test
import * as assert from "assert";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import { test } from "node:test";
import { findTool, mmcpArgs, msimArgs, parseLcov, parseServeUrl } from "../util";

test("msimArgs: options, files, top, waveform, coverage, extra", () => {
  const info = { files: ["/w/defs.svh", "/w/alu.sv", "/w/tb.sv"], args: ["--single-unit", "+incdir+/w/inc"], tops: ["tb"], root: "/w" };
  assert.deepStrictEqual(
    msimArgs(info, "tb", { extra: ["-seed", "3"], wave: "fst", waveFile: "/w/.minisim/tb.fst", coverage: true, covFile: "/w/.minisim/tb.cov.json" }),
    ["--single-unit", "+incdir+/w/inc", "/w/defs.svh", "/w/alu.sv", "/w/tb.sv", "-top", "tb", "-fst", "/w/.minisim/tb.fst", "-cov", "/w/.minisim/tb.cov.json", "-seed", "3"],
  );
  assert.deepStrictEqual(
    msimArgs(info, "tb", { extra: [], wave: "none", waveFile: "", coverage: false, covFile: "" }),
    ["--single-unit", "+incdir+/w/inc", "/w/defs.svh", "/w/alu.sv", "/w/tb.sv", "-top", "tb"],
  );
});

test("parseServeUrl: the line mdebug -serve prints", () => {
  assert.strictEqual(parseServeUrl("mdebug: reading\nmdebug: serving http://127.0.0.1:40123/  (5 instances, 20 signals)\n"), "http://127.0.0.1:40123/");
  assert.strictEqual(parseServeUrl("mdebug: reading\n"), undefined);
});

test("parseLcov: lines, branches, relative paths", () => {
  const t = "TN:msim\nSF:rtl/alu.sv\nBRDA:12,0,0,3\nBRDA:12,0,1,-\nBRF:2\nBRH:1\nDA:11,5\nDA:12,0\nLF:2\nLH:1\nend_of_record\n";
  const f = parseLcov(t, "/w");
  assert.strictEqual(f.length, 1);
  assert.strictEqual(f[0].path, path.join("/w", "rtl/alu.sv"));
  assert.deepStrictEqual([...f[0].lines], [[11, 5], [12, 0]]);
  assert.deepStrictEqual(f[0].branches, [
    { line: 12, block: 0, branch: 0, taken: 3 },
    { line: 12, block: 0, branch: 1, taken: 0 },
  ]);
});

test("findTool: binDir first, then PATH; only executables", () => {
  const d = fs.mkdtempSync(path.join(os.tmpdir(), "minisim-ext-"));
  const exe = path.join(d, "mlsp");
  fs.writeFileSync(exe, "#!/bin/sh\n");
  fs.chmodSync(exe, 0o755);
  fs.writeFileSync(path.join(d, "msim"), "not executable");
  fs.chmodSync(path.join(d, "msim"), 0o644);
  assert.strictEqual(findTool("mlsp", d), exe);
  assert.strictEqual(findTool("mlsp", "", d), exe);
  if (process.platform !== "win32") assert.strictEqual(findTool("msim", d), undefined);
  assert.strictEqual(findTool("nothing", d), undefined);
  fs.rmSync(d, { recursive: true });
});

test("mmcpArgs: workspace root and the minisim bin directory", () => {
  assert.deepStrictEqual(mmcpArgs("/w", ""), ["-root", "/w"]);
  assert.deepStrictEqual(mmcpArgs("/w", "/opt/minisim/bin"), ["-root", "/w", "-bin", "/opt/minisim/bin"]);
});
