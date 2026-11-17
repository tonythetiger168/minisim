// Pieces of the extension that do not need VS Code, so the unit tests
// (src/test/util.test.ts) run them under plain node.
import * as fs from "fs";
import * as path from "path";

/** What mlsp answers to minisim/compileInfo: the design the open file is in. */
export interface CompileInfo {
  files: string[];
  args: string[];
  tops: string[];
  root: string;
}

/** A minisim program: in binDir if set, else the first match on PATH. */
export function findTool(name: string, binDir: string, envPath = process.env.PATH ?? ""): string | undefined {
  const exe = process.platform === "win32" ? [name + ".exe", name + ".cmd", name] : [name];
  const dirs = binDir ? [binDir] : envPath.split(path.delimiter).filter((d) => d);
  for (const d of dirs) {
    for (const e of exe) {
      const p = path.join(d, e);
      try {
        fs.accessSync(p, fs.constants.X_OK);
        if (fs.statSync(p).isFile()) return p;
      } catch {
        // not here
      }
    }
  }
  return undefined;
}

/** The msim command line for one run of the design. */
export function msimArgs(
  info: CompileInfo,
  top: string,
  opts: { extra: string[]; wave: "fst" | "vcd" | "none"; waveFile: string; coverage: boolean; covFile: string },
): string[] {
  const a: string[] = [...info.args];
  for (const f of info.files) a.push(f);
  a.push("-top", top);
  if (opts.wave !== "none") a.push(opts.wave === "fst" ? "-fst" : "-vcd", opts.waveFile);
  if (opts.coverage) a.push("-cov", opts.covFile);
  a.push(...opts.extra);
  return a;
}

/** The address `mdebug -serve 0` prints when it is ready. */
export function parseServeUrl(output: string): string | undefined {
  const m = /serving (http:\/\/127\.0\.0\.1:\d+\/)/.exec(output);
  return m ? m[1] : undefined;
}

export interface LcovFile {
  path: string;
  lines: Map<number, number>; // 1-based line -> hits
  branches: { line: number; block: number; branch: number; taken: number }[];
}

/** An lcov tracefile (what `mcov report -lcov` writes). */
export function parseLcov(text: string, baseDir = ""): LcovFile[] {
  const out: LcovFile[] = [];
  let cur: LcovFile | undefined;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.startsWith("SF:")) {
      let p = line.slice(3);
      if (baseDir && !path.isAbsolute(p)) p = path.join(baseDir, p);
      cur = { path: p, lines: new Map(), branches: [] };
    } else if (line.startsWith("DA:") && cur) {
      const [ln, hits] = line.slice(3).split(",");
      cur.lines.set(Number(ln), Number(hits));
    } else if (line.startsWith("BRDA:") && cur) {
      const [ln, block, branch, taken] = line.slice(5).split(",");
      cur.branches.push({
        line: Number(ln),
        block: Number(block),
        branch: Number(branch),
        taken: taken === "-" ? 0 : Number(taken),
      });
    } else if (line === "end_of_record" && cur) {
      out.push(cur);
      cur = undefined;
    }
  }
  return out;
}

/** The arguments to start mmcp (the MCP server) for one workspace folder. */
export function mmcpArgs(root: string, binDir: string): string[] {
  const a = ["-root", root];
  if (binDir) a.push("-bin", binDir);
  return a;
}
