// The minisim extension for VS Code. It has no engine of its own: language
// features come from mlsp (slang, the front end msim uses), lint from mlint
// (through mlsp, on save), runs from msim, waveforms from mdebug's served
// viewer, coverage from mcov's lcov output, and agents get the minisim MCP
// server (mmcp). See docs/LSP.md in the minisim repository.
import * as cp from "child_process";
import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from "vscode-languageclient/node";
import { CompileInfo, findTool, mmcpArgs, msimArgs, parseLcov, parseServeUrl } from "./util";

let client: LanguageClient | undefined;
let output: vscode.OutputChannel;
let coverage: vscode.TestController | undefined;
const viewers: cp.ChildProcess[] = [];

function cfg() {
  return vscode.workspace.getConfiguration("minisim");
}

function binDir(): string {
  return cfg().get<string>("binDir", "");
}

function tool(name: string): string | undefined {
  const p = findTool(name, binDir());
  if (!p) {
    void vscode.window.showErrorMessage(
      `minisim: cannot find ${name}. Set minisim.binDir to the directory with the minisim programs, or add it to PATH.`,
    );
  }
  return p;
}

function serverSettings() {
  const c = cfg();
  return {
    args: c.get<string[]>("args", []),
    fileList: c.get<string>("fileList", ""),
    binDir: c.get<string>("binDir", ""),
    uvmHome: c.get<string>("uvmHome", ""),
    mlint: { enable: c.get<boolean>("mlint.enable", true), args: c.get<string[]>("mlint.args", []) },
  };
}

async function startClient(context: vscode.ExtensionContext): Promise<void> {
  const mlsp = tool("mlsp");
  if (!mlsp) return;
  const serverOptions: ServerOptions = { command: mlsp, args: [], transport: TransportKind.stdio };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "systemverilog" },
      { scheme: "file", language: "verilog" },
      { scheme: "untitled", language: "systemverilog" },
    ],
    initializationOptions: serverSettings(),
    outputChannel: output,
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.{sv,svh,svi,sva,v,vh}"),
    },
  };
  client = new LanguageClient("minisim", "minisim (mlsp)", serverOptions, clientOptions);
  await client.start();
  context.subscriptions.push({ dispose: () => void client?.stop() });
}

/** The design of the active editor, from mlsp. */
async function compileInfo(): Promise<{ info: CompileInfo; doc: vscode.TextDocument } | undefined> {
  const ed = vscode.window.activeTextEditor;
  if (!ed || !client) {
    void vscode.window.showWarningMessage("minisim: open a SystemVerilog file first.");
    return undefined;
  }
  if (ed.document.isDirty) await ed.document.save();
  const info = await client.sendRequest<CompileInfo | null>("minisim/compileInfo", {
    textDocument: { uri: ed.document.uri.toString() },
  });
  if (!info || (info.files.length === 0 && !info.args.includes("-f"))) {
    void vscode.window.showWarningMessage("minisim: mlsp has no design for this file yet.");
    return undefined;
  }
  return { info, doc: ed.document };
}

async function pickTop(info: CompileInfo): Promise<string | undefined> {
  if (info.tops.length === 1) return info.tops[0];
  const tb = info.tops.filter((t) => /^(tb|test|top|.*_tb|tb_.*)$/i.test(t));
  const items = [...tb, ...info.tops.filter((t) => !tb.includes(t))];
  return vscode.window.showQuickPick(items, { placeHolder: "Top module to simulate" });
}

function workDir(info: CompileInfo): string {
  const root = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? info.root;
  const d = path.join(root, ".minisim");
  fs.mkdirSync(d, { recursive: true });
  return d;
}

let lastRun: { info: CompileInfo; top: string; wave?: string; cov?: string } | undefined;

async function runSimulation(): Promise<void> {
  const ci = await compileInfo();
  if (!ci) return;
  const msim = tool("msim");
  if (!msim) return;
  const top = await pickTop(ci.info);
  if (!top) return;
  const dir = workDir(ci.info);
  const wave = cfg().get<"fst" | "vcd" | "none">("waveform", "fst");
  const waveFile = path.join(dir, `${top}.${wave === "vcd" ? "vcd" : "fst"}`);
  const cov = cfg().get<boolean>("coverage", false);
  const covFile = path.join(dir, `${top}.cov.json`);
  const args = msimArgs(ci.info, top, {
    extra: cfg().get<string[]>("msim.args", []),
    wave,
    waveFile,
    coverage: cov,
    covFile,
  });
  lastRun = { info: ci.info, top, wave: wave === "none" ? undefined : waveFile, cov: cov ? covFile : undefined };
  const task = new vscode.Task(
    { type: "minisim", tool: "msim", top },
    vscode.TaskScope.Workspace,
    `msim ${top}`,
    "minisim",
    new vscode.ProcessExecution(msim, args, { cwd: ci.info.root || dir }),
    "$msim",
  );
  task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true };
  await vscode.tasks.executeTask(task);
}

async function lint(): Promise<void> {
  const ci = await compileInfo();
  if (!ci) return;
  const mlint = tool("mlint");
  if (!mlint) return;
  const args = [...cfg().get<string[]>("mlint.args", []), ...ci.info.args, ...ci.info.files];
  const task = new vscode.Task(
    { type: "minisim", tool: "mlint" },
    vscode.TaskScope.Workspace,
    "mlint",
    "minisim",
    new vscode.ProcessExecution(mlint, args, { cwd: ci.info.root }),
    "$msim",
  );
  task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true };
  await vscode.tasks.executeTask(task);
}

async function openWaveform(): Promise<void> {
  let wave = lastRun?.wave;
  let run = lastRun;
  if (!wave || !fs.existsSync(wave)) {
    const picked = await vscode.window.showOpenDialog({
      canSelectMany: false,
      filters: { Waveforms: ["fst", "vcd"] },
      title: "Waveform to open in mdebug",
    });
    if (!picked) return;
    wave = picked[0].fsPath;
    const ci = await compileInfo();
    if (!ci) return;
    const top = await pickTop(ci.info);
    if (!top) return;
    run = { info: ci.info, top, wave };
  }
  const mdebug = tool("mdebug");
  if (!mdebug || !run) return;
  const args = [...run.info.args, ...run.info.files, "-top", run.top, wave.endsWith(".vcd") ? "-vcd" : "-fst", wave, "-serve", "0"];
  output.appendLine(`mdebug ${args.join(" ")}`);
  const p = cp.spawn(mdebug, args, { cwd: run.info.root });
  viewers.push(p);
  let text = "";
  const url = await new Promise<string | undefined>((resolve) => {
    const onData = (b: Buffer) => {
      text += b.toString();
      output.append(b.toString());
      const u = parseServeUrl(text);
      if (u) resolve(u);
    };
    p.stdout.on("data", onData);
    p.stderr.on("data", (b: Buffer) => output.append(b.toString()));
    p.on("exit", () => resolve(undefined));
    setTimeout(() => resolve(undefined), 120000);
  });
  if (!url) {
    output.show();
    void vscode.window.showErrorMessage("minisim: mdebug did not start its viewer (see the minisim output).");
    return;
  }
  // The viewer is a web page served on 127.0.0.1; inside VS Code when the
  // Simple Browser is there, else in the system browser.
  try {
    await vscode.commands.executeCommand("simpleBrowser.show", url);
  } catch {
    await vscode.env.openExternal(vscode.Uri.parse(url));
  }
}

async function showCoverage(): Promise<void> {
  let cov = lastRun?.cov;
  if (!cov || !fs.existsSync(cov)) {
    const picked = await vscode.window.showOpenDialog({
      canSelectMany: false,
      filters: { "Coverage (msim -cov JSON, or lcov)": ["json", "info"] },
      title: "Coverage database",
    });
    if (!picked) return;
    cov = picked[0].fsPath;
  }
  let lcovPath = cov;
  if (cov.endsWith(".json")) {
    const mcov = tool("mcov");
    if (!mcov) return;
    lcovPath = cov.replace(/\.json$/, ".info");
    const r = cp.spawnSync(mcov, ["report", cov, "-lcov", lcovPath], { encoding: "utf8" });
    if (r.status !== 0) {
      output.appendLine(r.stdout + r.stderr);
      output.show();
      void vscode.window.showErrorMessage("minisim: mcov report failed (see the minisim output).");
      return;
    }
  }
  const files = parseLcov(fs.readFileSync(lcovPath, "utf8"), path.dirname(lcovPath));
  if (!coverage) {
    coverage = vscode.tests.createTestController("minisim.coverage", "minisim coverage");
  }
  const run = coverage.createTestRun(new vscode.TestRunRequest(), "minisim coverage", false);
  for (const f of files) {
    const details: vscode.FileCoverageDetail[] = [];
    const branchesByLine = new Map<number, vscode.BranchCoverage[]>();
    for (const b of f.branches) {
      const list = branchesByLine.get(b.line) ?? [];
      list.push(new vscode.BranchCoverage(b.taken, undefined, b.branch === 0 ? "true" : "false"));
      branchesByLine.set(b.line, list);
    }
    for (const [ln, hits] of f.lines) {
      details.push(
        new vscode.StatementCoverage(hits, new vscode.Position(Math.max(0, ln - 1), 0), branchesByLine.get(ln) ?? []),
      );
    }
    run.addCoverage(vscode.FileCoverage.fromDetails(vscode.Uri.file(f.path), details));
  }
  run.end();
  void vscode.commands.executeCommand("testing.coverageView.focus").then(undefined, () => undefined);
}

function registerMcp(context: vscode.ExtensionContext): void {
  // VS Code 1.101+: offer mmcp, the minisim MCP server, to agent mode. It is
  // the one tool interface for agents; the extension adds none of its own.
  const lm = vscode.lm as unknown as {
    registerMcpServerDefinitionProvider?: (id: string, p: vscode.McpServerDefinitionProvider) => vscode.Disposable;
  };
  if (!lm.registerMcpServerDefinitionProvider) return;
  const changed = new vscode.EventEmitter<void>();
  context.subscriptions.push(
    lm.registerMcpServerDefinitionProvider("minisim.mcp", {
      onDidChangeMcpServerDefinitions: changed.event,
      provideMcpServerDefinitions: () => {
        if (!cfg().get<boolean>("mcp.enable", true)) return [];
        const mmcp = findTool("mmcp", binDir());
        if (!mmcp) return [];
        const out: vscode.McpServerDefinition[] = [];
        for (const f of vscode.workspace.workspaceFolders ?? []) {
          const label = (vscode.workspace.workspaceFolders?.length ?? 0) > 1 ? `minisim (${f.name})` : "minisim";
          out.push(new vscode.McpStdioServerDefinition(label, mmcp, mmcpArgs(f.uri.fsPath, binDir())));
        }
        return out;
      },
    }),
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("minisim.binDir") || e.affectsConfiguration("minisim.mcp")) changed.fire();
    }),
    vscode.workspace.onDidChangeWorkspaceFolders(() => changed.fire()),
  );
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  output = vscode.window.createOutputChannel("minisim");
  context.subscriptions.push(
    output,
    vscode.commands.registerCommand("minisim.runSimulation", runSimulation),
    vscode.commands.registerCommand("minisim.lint", lint),
    vscode.commands.registerCommand("minisim.openWaveform", openWaveform),
    vscode.commands.registerCommand("minisim.showCoverage", showCoverage),
    vscode.commands.registerCommand("minisim.restartServer", async () => {
      await client?.stop();
      client = undefined;
      await startClient(context);
    }),
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (!client || !e.affectsConfiguration("minisim")) return;
      if (e.affectsConfiguration("minisim.binDir")) {
        await vscode.commands.executeCommand("minisim.restartServer");
        return;
      }
      await client.sendNotification("workspace/didChangeConfiguration", { settings: { minisim: serverSettings() } });
    }),
    { dispose: () => viewers.forEach((p) => p.kill()) },
  );
  registerMcp(context);
  await startClient(context);
}

export async function deactivate(): Promise<void> {
  viewers.forEach((p) => p.kill());
  await client?.stop();
}
