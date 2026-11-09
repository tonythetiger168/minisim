#!/usr/bin/env python3
"""mbug self-test against fake GitHub, Jira and Redmine servers (no network).

    python3 tests/mbug_test.py <source dir> <scratch dir>

For each tracker: a first run creates one issue per failure signature, with
the reproduction command and build link; a second run comments instead of
filing duplicates; a closed issue gets a new issue that links to it. Also:
the default tracker is GitHub, Jira's Server /search fallback, Redmine and
Jira attachments, mlint grouping, sync-github both ways, -dry-run, the
max_issues cap, and that a config file holding a token is refused.
"""

import http.server
import json
import os
import re
import subprocess
import sys
import threading
import urllib.parse

SRC, OUT = sys.argv[1], sys.argv[2]
MBUG = [sys.executable, os.path.join(SRC, "tools", "mbug.py")]


class Store:
    def __init__(self, jira_cloud=True):
        self.jira_cloud = jira_cloud
        self.issues = {}     # id -> dict(title, body, labels, state, comments, attachments, project, closed_at, updated)
        self.labels = set()
        self.log = []
        self.n = 0

    def new(self, **kw):
        self.n += 1
        d = dict(comments=[], attachments=[], state="open", closed_at=None, labels=[], updated="2026-11-01T00:00:00Z")
        d.update(kw)
        self.issues[self.n] = d
        return self.n


def handler(st):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def reply(self, code, obj=None, headers=None):
            body = json.dumps(obj if obj is not None else {}).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def body(self):
            return self.rfile.read(int(self.headers.get("Content-Length", 0)))

        def route(self, method):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            p = u.path
            st.log.append((method, p))
            data = self.body() if method in ("POST", "PUT", "PATCH") else b""
            # ---------------- GitHub
            m = re.match(r"/repos/(\w+/\w+)/(issues|labels)(?:/(\d+))?(?:/(comments))?$", p)
            if m:
                if method != "GET" and self.headers.get("Authorization") != "Bearer ghtok":
                    return self.reply(401, {"message": "Bad credentials"})
                repo, what, num, sub = m.groups()
                if what == "labels":
                    name = json.loads(data)["name"]
                    if name in st.labels:
                        return self.reply(422, {"message": "already_exists"})
                    st.labels.add(name)
                    return self.reply(201, {"name": name})
                if method == "GET" and not num:
                    want = q.get("labels", [""])[0].split(",") if "labels" in q else []
                    items = [self.gh(k, v) for k, v in sorted(st.issues.items(), reverse=True)
                             if v.get("project") == repo and all(w in v["labels"] for w in want if w)]
                    # Two pages, to exercise the Link header.
                    page = int(q.get("page", ["1"])[0])
                    if "labels" not in q and len(items) > 1:
                        half = (len(items) + 1) // 2
                        if page == 1:
                            nxt = f"<http://{self.headers['Host']}{p}?{u.query}&page=2>; rel=\"next\""
                            return self.reply(200, items[:half] + [{"number": 999, "pull_request": {}}], {"Link": nxt})
                        return self.reply(200, items[half:])
                    return self.reply(200, items)
                if method == "POST" and not num:
                    b = json.loads(data)
                    for l in b["labels"]:
                        if l not in st.labels:
                            return self.reply(422, {"message": f"label {l} does not exist"})
                    k = st.new(project=repo, title=b["title"], body=b["body"], labels=b["labels"],
                               assignees=b.get("assignees", []))
                    return self.reply(201, self.gh(k, st.issues[k]))
                iss = st.issues.get(int(num))
                if sub == "comments":
                    iss["comments"].append(json.loads(data)["body"])
                    return self.reply(201, {})
                if method == "PATCH":
                    b = json.loads(data)
                    iss["state"] = b["state"]
                    iss["closed_at"] = "2026-11-09T12:00:00Z" if b["state"] == "closed" else None
                    return self.reply(200, self.gh(int(num), iss))
            # ---------------- Jira
            if p.startswith("/rest/api/2/"):
                a = self.headers.get("Authorization", "")
                if not (a.startswith("Basic ") or a == "Bearer pat123"):
                    return self.reply(401)
                if p == "/rest/api/2/search/jql" and not st.jira_cloud:
                    return self.reply(404)
                if p in ("/rest/api/2/search/jql", "/rest/api/2/search"):
                    jql = q["jql"][0]
                    project = re.search(r'project = "([^"]+)"', jql).group(1)
                    want = re.findall(r'"([^"]+)"', jql.split("labels", 1)[1])
                    hits = [{"key": f"VER-{k}", "fields": {
                        "labels": v["labels"], "resolutiondate": v["closed_at"],
                        "status": {"statusCategory": {"key": "done" if v["state"] == "closed" else "new"}}}}
                        for k, v in sorted(st.issues.items(), reverse=True)
                        if v["project"] == project and any(w in v["labels"] for w in want)]
                    return self.reply(200, {"issues": hits})
                if p == "/rest/api/2/issue" and method == "POST":
                    f = json.loads(data)["fields"]
                    if f["project"]["key"] == "BAD":
                        return self.reply(400, {"errors": {"project": "valid project is required"}})
                    k = st.new(project=f["project"]["key"], title=f["summary"], body=f["description"],
                               labels=f["labels"], fields=f)
                    return self.reply(201, {"key": f"VER-{k}"})
                m = re.match(r"/rest/api/2/issue/VER-(\d+)/(comment|attachments|transitions)$", p)
                if m:
                    iss = st.issues[int(m.group(1))]
                    if m.group(2) == "comment":
                        iss["comments"].append(json.loads(data)["body"])
                        return self.reply(201, {})
                    if m.group(2) == "attachments":
                        assert self.headers.get("X-Atlassian-Token") == "no-check"
                        iss["attachments"].append(re.search(rb'filename="([^"]+)"', data).group(1).decode())
                        return self.reply(200, [])
                    if method == "GET":
                        return self.reply(200, {"transitions": [{"id": "31", "name": "Done", "to": {"name": "Done"}},
                                                                {"id": "11", "name": "To Do", "to": {"name": "To Do"}}]})
                    tid = json.loads(data)["transition"]["id"]
                    iss["state"] = "closed" if tid == "31" else "open"
                    iss["closed_at"] = "2026-11-09T12:00:00.000+0000" if tid == "31" else None
                    return self.reply(204)
            # ---------------- Redmine
            if p.endswith(".json"):
                if self.headers.get("X-Redmine-API-Key") != "rmkey":
                    return self.reply(401)
                if p == "/trackers.json":
                    return self.reply(200, {"trackers": [{"id": 1, "name": "Bug"}, {"id": 2, "name": "Feature"}]})
                if p == "/issue_statuses.json":
                    return self.reply(200, {"issue_statuses": [{"id": 1, "name": "New"}, {"id": 5, "name": "Closed"}]})
                if p == "/uploads.json":
                    st.n += 0
                    return self.reply(201, {"upload": {"token": f"tok-{q['filename'][0]}"}})
                if p == "/issues.json" and method == "GET":
                    assert q.get("op[status_id]") == ["*"], q
                    needle = q.get("v[description][]", [""])[0]
                    hits = [{"id": k, "description": v["body"], "closed_on": v["closed_at"]}
                            for k, v in sorted(st.issues.items(), reverse=True)
                            if v["project"] == q["project_id"][0] and needle in v["body"]]
                    return self.reply(200, {"issues": hits})
                if p == "/issues.json" and method == "POST":
                    i = json.loads(data)["issue"]
                    k = st.new(project=i["project_id"], title=i["subject"], body=i["description"], fields=i,
                               attachments=[u["filename"] for u in i.get("uploads", [])])
                    return self.reply(201, {"issue": {"id": k}})
                m = re.match(r"/issues/(\d+)\.json$", p)
                if m and method == "PUT":
                    iss, i = st.issues[int(m.group(1))], json.loads(data)["issue"]
                    if "notes" in i:
                        iss["comments"].append(i["notes"])
                    if "status_id" in i:
                        iss["state"] = "closed" if i["status_id"] == 5 else "open"
                        iss["closed_at"] = "2026-11-09T12:00:00Z" if i["status_id"] == 5 else None
                    return self.reply(204)
            self.reply(404, {"path": p})

        def gh(self, k, v):
            return {"number": k, "title": v["title"], "body": v["body"], "state": v["state"],
                    "labels": [{"name": l} for l in v["labels"]], "closed_at": v["closed_at"],
                    "updated_at": v["updated"], "created_at": "2026-11-01T00:00:00Z",
                    "user": {"login": "tony"}, "closed_by": {"login": "tony"} if v["state"] == "closed" else None,
                    "html_url": f"https://github.com/{v['project']}/issues/{k}"}

        def do_GET(self):
            self.route("GET")

        def do_POST(self):
            self.route("POST")

        def do_PUT(self):
            self.route("PUT")

        def do_PATCH(self):
            self.route("PATCH")
    return H


SERVERS = []


def serve(st):
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler(st))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    SERVERS.append(srv)
    return f"http://127.0.0.1:{srv.server_address[1]}"


def run(args, env, stdin=None, want=0):
    p = subprocess.run(MBUG + args, env=env, input=stdin, capture_output=True, text=True)
    if p.returncode != want:
        sys.exit(f"mbug {' '.join(args)} exited {p.returncode}, expected {want}\n{p.stdout}{p.stderr}")
    return p.stdout + p.stderr


def check(cond, msg):
    if not cond:
        sys.exit("FAIL: " + msg)


def make_results(d):
    """A results.json like mregress writes: 3 failing runs in 2 signatures."""
    runs = []
    for test, seed, reason in [("fifo", 11, "Error: fifo overflow at 120ns"),
                               ("fifo", 12, "Error: fifo overflow at 340ns"),
                               ("alu", 3, "output differs at line 2: expected 'x=1', got 'x=0'")]:
        rd = os.path.join(d, "runs", f"{test}_s{seed}")
        os.makedirs(rd, exist_ok=True)
        with open(os.path.join(rd, "sim.log"), "w") as f:
            f.write("".join(f"line {i}\n" for i in range(100)) + reason + "\n")
        with open(os.path.join(rd, "cmd.sh"), "w") as f:
            f.write(f"#!/bin/sh\nmsim tb.sv -seed {seed}\n")
        runs.append({"test": test, "seed": seed, "status": "fail", "reason": reason, "seconds": 1.0,
                     "log": f"runs/{test}_s{seed}/sim.log", "cmd": f"msim tb.sv -seed {seed}"})
    runs.append({"test": "ok", "seed": 1, "status": "pass", "reason": "", "seconds": 1.0, "log": "", "cmd": ""})
    path = os.path.join(d, "results.json")
    with open(path, "w") as f:
        json.dump({"simulator": "msim", "lists": ["/w/tests.list"], "passed": 1, "failed": 3, "runs": runs}, f)
    return path


def base_env(**kw):
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("JIRA_", "REDMINE_", "GITHUB_", "GH_", "MBUG_", "BUILD_", "JOB_", "GIT_"))}
    env.update(BUILD_URL="https://jenkins.example.com/job/nightly/42/", JOB_NAME="nightly", BUILD_NUMBER="42")
    env.update(kw)
    return env


def filing(tracker, st, env, results, project, repro_in="body"):
    """The shared create / comment / came-back sequence for one tracker."""
    args = ["regress", results, "-project", project, "-label", "nightly"] + (["-tracker", tracker] if tracker else [])
    out = run(args + ["-o", os.path.join(OUT, f"{tracker}.json")], env)
    check(len(st.issues) == 2, f"{tracker}: expected 2 issues, got {len(st.issues)}\n{out}")
    fifo_id = next(k for k, v in st.issues.items() if "overflow" in v["title"])
    fifo = st.issues[fifo_id]
    check("[mregress] Error: fifo overflow at <n>ns" == fifo["title"], f"{tracker}: title {fifo['title']}")
    b = fifo["body"]
    check("2 failing run(s)" in b and "fifo seed 11" in b and "fifo seed 12" in b, f"{tracker}: runs\n{b}")
    check("mregress /w/tests.list -sim msim -filter '^fifo$' -seed 11 -seeds 1" in b, f"{tracker}: repro\n{b}")
    check("line 99" in b and "line 50" not in b, f"{tracker}: log tail")
    rec = json.load(open(os.path.join(OUT, f"{tracker}.json")))
    check([i["action"] for i in rec["issues"]] == ["create", "create"], f"{tracker}: actions")

    env2 = dict(env, BUILD_NUMBER="43")
    run(args, env2)
    check(len(st.issues) == 2, f"{tracker}: second run created duplicates")
    check(all(len(v["comments"]) == 1 for v in st.issues.values()), f"{tracker}: one comment each")
    check("nightly #43" in fifo["comments"][0] and "Failed again" in fifo["comments"][0], fifo["comments"][0])
    run(args + ["-no-comment"], env2)
    check(all(len(v["comments"]) == 1 for v in st.issues.values()), f"{tracker}: -no-comment")

    fifo["state"], fifo["closed_at"] = "closed", "2026-11-09T12:00:00Z"
    run(args, env2)
    check(len(st.issues) == 3, f"{tracker}: a closed signature should get a new issue")
    check("This came back" in st.issues[3]["body"], f"{tracker}: link to the closed issue\n{st.issues[3]['body']}")
    return fifo


def main():
    os.makedirs(OUT, exist_ok=True)
    results = make_results(os.path.join(OUT, "regress"))

    # ---- GitHub Issues, the default tracker (no -tracker), Markdown.
    gh = Store()
    env = base_env(GITHUB_API_URL=serve(gh), GITHUB_TOKEN="ghtok")
    fifo = filing("", gh, env, results, "tony/minisim")
    check({"mbug", "mregress", "msim", "nightly"} <= set(fifo["labels"]), f"labels {fifo['labels']}")
    check(any(l.startswith("mbug-") for l in fifo["labels"]) and all(l in gh.labels for l in fifo["labels"]),
          "GitHub labels are created before use")
    check("**Reason:**" in fifo["body"] and "```sh" in fifo["body"], "Markdown body\n" + fifo["body"])
    check("[nightly #42](https://jenkins.example.com/job/nightly/42/)" in fifo["body"], "Jenkins link (Markdown)")
    check("[build artifacts](https://jenkins.example.com/job/nightly/42/artifact/)" in fifo["body"],
          "GitHub cannot attach, so it links the Jenkins artifacts")
    out = run(["regress", results, "-project", "tony/minisim", "-max-issues", "0"], dict(env, GITHUB_TOKEN=""), want=1)
    check("needs GITHUB_TOKEN" in out, out)
    # GitHub Actions: the repository and the run link come from the environment.
    gh2 = Store()
    env_a = base_env(GITHUB_API_URL=serve(gh2), GITHUB_TOKEN="ghtok", GITHUB_REPOSITORY="tony/minisim",
                     GITHUB_SERVER_URL="https://github.com", GITHUB_RUN_ID="777", GITHUB_RUN_NUMBER="5",
                     GITHUB_WORKFLOW="ci")
    for k in ("BUILD_URL", "JOB_NAME", "BUILD_NUMBER"):
        env_a.pop(k)
    run(["regress", results], env_a)
    check("[ci #5](https://github.com/tony/minisim/actions/runs/777)" in gh2.issues[1]["body"], "Actions run link")

    # ---- Jira Cloud: wiki markup, attachments, components.
    jira = Store()
    env = base_env(JIRA_URL=serve(jira), JIRA_USER="ci@example.com", JIRA_TOKEN="tok")
    fifo = filing("jira", jira, env, results, "VER")
    check("*Reason:*" in fifo["body"] and "{code:bash}" in fifo["body"], "Jira wiki body")
    check("[nightly #42|https://jenkins.example.com/job/nightly/42/]" in fifo["body"], "Jenkins link (Jira)")
    check(fifo["attachments"] == ["sim.log", "cmd.sh"], f"Jira attachments {fifo['attachments']}")
    check(fifo["fields"]["issuetype"] == {"name": "Bug"}, "issuetype")
    out = run(["regress", results, "-tracker", "jira", "-project", "BAD"], env, want=1)
    check("HTTP 400" in out and "valid project is required" in out, out)
    # Server / DC: PAT, /search fallback, max_issues, -no-attach.
    jira2 = Store(jira_cloud=False)
    env2 = base_env(JIRA_URL=serve(jira2), JIRA_PAT="pat123")
    out = run(["regress", results, "-tracker", "jira", "-project", "VER", "-max-issues", "1", "-no-attach"], env2)
    check(len(jira2.issues) == 1 and "skipped (max_issues)" in out, out)
    check(("GET", "/rest/api/2/search") in jira2.log, "Server fallback to /search")
    check(jira2.issues[1]["attachments"] == [], "-no-attach")

    # ---- Redmine: Textile, marker line in the description, uploads, config file.
    rm = Store()
    env = base_env(REDMINE_URL=serve(rm), REDMINE_KEY="rmkey")
    cfgp = os.path.join(OUT, "redmine.json")
    with open(cfgp, "w") as f:
        json.dump({"tracker": "redmine", "tracker_name": "Bug"}, f)
    args_cfg = ["-config", cfgp]
    fifo = None
    # filing() passes -tracker; here it comes from the config file instead.
    out = run(["regress", results, "-project", "soc"] + args_cfg, env)
    check(len(rm.issues) == 2, out)
    fifo = next(v for v in rm.issues.values() if "overflow" in v["title"])
    check("<pre>" in fifo["body"] and '"nightly #42":https://jenkins.example.com/job/nightly/42/' in fifo["body"],
          "Textile body\n" + fifo["body"])
    check(re.search(r"mbug-marker: mbug-[0-9a-f]{12}", fifo["body"]), "Redmine marker line")
    check(fifo["fields"]["tracker_id"] == 1, "tracker_name -> tracker_id")
    check(fifo["attachments"] == ["sim.log", "cmd.sh"], f"Redmine uploads {fifo['attachments']}")
    run(["regress", results, "-project", "soc"] + args_cfg, env)
    check(len(rm.issues) == 2 and all(len(v["comments"]) == 1 for v in rm.issues.values()), "Redmine repeat -> notes")
    fifo["closed_at"] = "2026-11-09T12:00:00Z"
    run(["regress", results, "-project", "soc"] + args_cfg, env)
    check(len(rm.issues) == 3 and "This came back" in rm.issues[3]["body"], "Redmine came back")
    md = os.path.join(OUT, "redmine-md.json")
    with open(md, "w") as f:
        json.dump({"tracker": "redmine", "markup": "markdown"}, f)
    rm2 = Store()
    run(["regress", results, "-project", "soc", "-config", md], base_env(REDMINE_URL=serve(rm2), REDMINE_KEY="rmkey"))
    check("```sh" in rm2.issues[1]["body"], "Redmine with markdown")

    # ---- mlint findings: one issue per rule, or per finding.
    lint = ("rtl/t.sv:5:3: warning: [L001] 't.y' is not assigned on every path of an always_comb\n"
            "rtl/t.sv:9:3: warning: [L001] 't.q' is not assigned on every path of an always_comb\n"
            "rtl/t.sv:12:7: warning: [L004] 't.z' is assigned by 2 processes\n"
            "mlint: 3 findings (L001 2, L004 1)\n")
    gl = Store()
    env = base_env(GITHUB_API_URL=serve(gl), GITHUB_TOKEN="ghtok")
    run(["lint", "-", "-project", "tony/minisim"], env, stdin=lint)
    titles = sorted(v["title"] for v in gl.issues.values())
    check(titles == ["[mlint] L001: 2 finding(s)", "[mlint] L004: 1 finding(s)"], f"{titles}")
    l001 = next(v for v in gl.issues.values() if "L001" in v["title"])
    check("| rtl/t.sv | 9 | 't.q' is not assigned" in l001["body"], "lint table\n" + l001["body"])
    run(["lint", "-", "-project", "tony/minisim"], env, stdin=lint)
    check(len(gl.issues) == 2, "lint rerun duplicated")
    run(["lint", "-", "-project", "tony/minisim", "-group", "finding", "-tool", "mdft"], env, stdin=lint)
    check(len(gl.issues) == 5, f"per-finding grouping: {len(gl.issues)}")
    out = run(["lint", "-", "-project", "tony/minisim"], env, stdin="mlint: 0 findings\n")
    check("nothing to file" in out, out)

    # ---- sync-github: GitHub issues -> Jira, then back.
    src, dst = Store(), Store()
    gurl, jurl = serve(src), serve(dst)
    a = src.new(project="tony/minisim", title="msim crashes on %p", body="repro: ...", labels=["bug"])
    b = src.new(project="tony/minisim", title="add $sformatf", body="", labels=["enhancement"])
    c = src.new(project="tony/minisim", title="old one", body="", state="closed", closed_at="2026-11-02T00:00:00Z")
    env = base_env(GITHUB_API_URL=gurl, GITHUB_TOKEN="ghtok", JIRA_URL=jurl, JIRA_USER="u", JIRA_TOKEN="t")
    cfgp = os.path.join(OUT, "sync.json")
    with open(cfgp, "w") as f:
        json.dump({"project": "VER", "close_state": "Done", "reopen_state": "To Do"}, f)
    sync = ["sync-github", "-repo", "tony/minisim", "-tracker", "jira", "-config", cfgp]
    run(sync + ["-backlink"], env)
    check(len(dst.issues) == 2, f"open GitHub issues -> 2 Jira issues, got {len(dst.issues)}")
    j1 = dst.issues[1]
    check(j1["title"] == "[gh#1] msim crashes on %p" and "gh-tony-minisim-1" in j1["labels"] and "gh:bug" in j1["labels"],
          f"{j1['title']} {j1['labels']}")
    check("[tony/minisim#1|https://github.com/tony/minisim/issues/1]" in j1["body"], j1["body"])
    check("VER-1" in src.issues[a]["comments"][0], "backlink comment")
    run(sync + ["-backlink"], env)
    check(len(dst.issues) == 2 and len(src.issues[a]["comments"]) == 1, "second sync is a no-op")
    # Closed on GitHub -> comment + Done in Jira.
    src.issues[a].update(state="closed", closed_at="2026-11-09T10:00:00Z", updated="2026-11-09T10:00:00Z")
    run(sync, env)
    check(dst.issues[1]["state"] == "closed" and "Closed on GitHub by tony" in dst.issues[1]["comments"][0], "close in Jira")
    # Reopened on GitHub after Jira resolved it -> To Do in Jira.
    src.issues[a].update(state="open", closed_at=None, updated="2026-11-09T13:00:00Z")
    run(sync, env)
    check(dst.issues[1]["state"] == "open" and "Open again on GitHub" in dst.issues[1]["comments"][1], "reopen in Jira")
    # Resolved in Jira after the last GitHub activity: -two-way closes the GitHub issue.
    dst.issues[2].update(state="closed", closed_at="2026-11-09T14:00:00.000+0000")
    run(sync, env)
    check(src.issues[b]["state"] == "open", "without -two-way GitHub is not written")
    check(dst.issues[2]["state"] == "closed", "Jira resolved after the last GitHub update stays resolved")
    run(sync + ["-two-way"], env)
    check(src.issues[b]["state"] == "closed" and "Resolved in Jira as [VER-2]" in src.issues[b]["comments"][-1],
          "two-way close on GitHub")
    check(len(dst.issues) == 2, "an issue already closed on GitHub (#%d) is not filed" % c)
    run(sync + ["-include-closed"], env)
    check(len(dst.issues) == 3 and dst.issues[3]["state"] == "closed", "-include-closed files it, closed")
    out = run(["sync-github", "-repo", "tony/minisim", "-project", "x/y"], env, want=2)
    check("pass -tracker jira" in out, out)
    # Redmine as the target.
    rs = Store()
    env_r = base_env(GITHUB_API_URL=gurl, REDMINE_URL=serve(rs), REDMINE_KEY="rmkey")
    run(["sync-github", "-repo", "tony/minisim", "-tracker", "redmine", "-project", "minisim"], env_r)
    check(len(rs.issues) == 1 and "mbug-marker: gh-tony-minisim-1" in rs.issues[1]["body"], "sync to Redmine")
    run(["sync-github", "-repo", "tony/minisim", "-tracker", "redmine", "-project", "minisim"], env_r)
    check(len(rs.issues) == 1, "Redmine sync is idempotent")

    # ---- Dry run, refused config, missing credentials.
    before = sum(len(s.log) for s in (gh, jira, rm))
    run(["regress", results, "-tracker", "jira", "-project", "VER", "-dry-run", "-o", os.path.join(OUT, "dry.json")],
        base_env())
    check(sum(len(s.log) for s in (gh, jira, rm)) == before, "dry run used the network")
    dry = json.load(open(os.path.join(OUT, "dry.json")))
    check(dry["issues"][0]["action"] == "create" and "{code:bash}" in dry["issues"][0]["fields"]["description"], "dry.json")
    bad = os.path.join(OUT, "bad.json")
    with open(bad, "w") as f:
        json.dump({"project": "VER", "api_token": "x"}, f)
    out = run(["regress", results, "-config", bad], base_env(), want=2)
    check("credentials go in the environment" in out, out)
    out = run(["regress", results, "-tracker", "jira", "-project", "VER"], base_env(JIRA_URL="http://x"), want=2)
    check("JIRA_USER and JIRA_TOKEN" in out, out)
    out = run(["regress", results, "-tracker", "redmine", "-project", "p"], base_env(), want=2)
    check("REDMINE_URL and REDMINE_KEY" in out, out)

    for s in SERVERS:
        s.shutdown()
    print("mbug: all checks passed")


main()
