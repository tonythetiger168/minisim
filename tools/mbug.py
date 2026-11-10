#!/usr/bin/env python3
"""mbug: file regression, lint and DFT failures in a bug tracker.

    mbug regress regress.d/results.json [-tracker github] [-config mbug.json] [-dry-run]
    mlint -top top rtl/*.sv | mbug lint -
    mbug lint dft.log -tool mdft -group finding
    mbug sync-github -repo owner/name -tracker jira [-two-way] [-backlink]

Trackers (``-tracker``, the ``tracker`` config key, or MBUG_TRACKER):

    github    GitHub Issues (the default; free)    GITHUB_TOKEN (or GH_TOKEN)
    redmine   Redmine (free, self-hosted)          REDMINE_URL, REDMINE_KEY
    jira      Jira Cloud                           JIRA_URL, JIRA_USER, JIRA_TOKEN
              Jira Server / Data Center            JIRA_URL, JIRA_PAT

Credentials come from the environment only; a -config file that holds a
token is refused. GITHUB_API_URL points at GitHub Enterprise.

One issue per distinct failure. A failure is identified by its *signature*:
for mregress the first error line with numbers, paths and hex replaced by
placeholders (the same grouping as report.html); for mlint and mdft the rule
(``-group rule``, the default: one issue per rule listing every finding) or
the rule plus the finding (``-group finding``). The signature is stored on
the issue as ``mbug-<hash>``: a label on GitHub and Jira, a line in the
description on Redmine (or a custom field, see ``signature_field``). The
next run looks it up:

* no open issue: a new one is created, with the failing tests and seeds,
  the reason, a reproduction command, the log tail, the CI build link, and
  (Jira, Redmine) the first failing run's sim.log and cmd.sh attached. If an
  earlier issue with the signature was closed, the new one links to it.
* an open issue: a comment for this occurrence instead of a duplicate.
  ``-no-comment`` turns that off.

Settings: ``-config FILE`` (JSON) and the options, options winning.

    tracker      github | redmine | jira
    project      GitHub: owner/name. Jira: project key. Redmine: project identifier
    labels       extra labels (Redmine: listed in the description)
    assignee     GitHub: login. Jira: account id (Cloud) / user name (Server).
                 Redmine: user id
    max_issues   create at most this many per run (default 20), so a broken
                 build does not file hundreds
    attach       attach logs where the tracker can (default true)
    Jira:        issuetype ("Bug"), components, priority, fields {id: value}
    Redmine:     tracker_name ("Bug"), priority, markup ("textile" or
                 "markdown"), signature_field (custom field id), fields
                 (extra issue keys, e.g. {"category_id": 3})
    sync-github: close_state / reopen_state (Jira transition or Redmine
                 status name, e.g. "Done" / "To Do")

CI: BUILD_URL, JOB_NAME, BUILD_NUMBER, GIT_COMMIT and GIT_BRANCH (Jenkins),
or GITHUB_SERVER_URL / GITHUB_REPOSITORY / GITHUB_RUN_ID (GitHub Actions),
are read from the environment, and every issue and comment links the build.

sync-github: every open issue of a GitHub repository gets one issue in Jira
or Redmine (marked ``gh-<owner>-<repo>-<number>``, linking back; pull
requests are left out), then the states are kept in step: closed on GitHub
-> a comment and ``close_state``; open on GitHub and updated after the other
side resolved it -> a comment and ``reopen_state``; with ``-two-way``,
resolved on the other side after the last GitHub activity -> the GitHub
issue is closed with a comment. ``-backlink`` comments the new key on the
GitHub issue. ``-gh-label`` (repeatable), ``-since`` and ``-include-closed``
pick the issues.

``-o FILE`` writes what was done as JSON; ``-dry-run`` prints the plan and
talks to no server.

Exit status: 0 done (also when there was nothing to file), 1 a tracker
request failed, 2 bad usage.
"""

import argparse
import base64
import hashlib
import json
import mimetypes
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import uuid
from datetime import datetime, timezone

VERSION = "0.1.0"
TRACKERS = ("github", "redmine", "jira")
DEFAULTS = {"labels": [], "attach": True, "max_issues": 20}
LOG_LIMIT = 1 << 20      # attach at most the last 1 MiB of a log
TAIL_LINES = 30          # log lines quoted in the description


class TrackerError(Exception):
    pass


def signature(reason):
    """Same as mregress.signature: numbers, paths and hex become placeholders."""
    s = re.sub(r"0x[0-9a-fA-F]+", "<hex>", reason)
    s = re.sub(r"(/[^\s:'\"]+)+", "<path>", s)
    s = re.sub(r"\d+", "<n>", s)
    return s[:160]


def sig_label(kind, sig):
    return "mbug-" + hashlib.sha1(f"{kind}\0{sig}".encode()).hexdigest()[:12]


def norm_time(t):
    """ISO time (Jira, Redmine or GitHub style) as a sortable UTC string."""
    if not t:
        return ""
    t = t.replace("Z", "+00:00")
    t = re.sub(r"([+-]\d\d)(\d\d)$", r"\1:\2", t)
    return datetime.fromisoformat(t).astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")


def clean_label(l):
    return re.sub(r"[\s,]+", "_", l)


# --------------------------------------------------------------- documents
#
# Issue text is built as a list of blocks and rendered in the tracker's
# markup: Markdown (GitHub, Redmine with markdown), Jira wiki, Textile
# (Redmine's default).
#   ("p", text)             a paragraph; text may hold Link and Em items
#   ("kv", key, value)      **key:** value
#   ("pre", text)           preformatted text
#   ("code", text)          a shell command
#   ("table", head, rows)


class Link:
    def __init__(self, text, url):
        self.text, self.url = text, url


class Em:
    def __init__(self, text):
        self.text = text


def inline(parts, fmt):
    out = []
    for p in parts if isinstance(parts, (list, tuple)) else [parts]:
        if isinstance(p, Link):
            out.append({"md": f"[{p.text}]({p.url})", "jira": f"[{p.text}|{p.url}]",
                        "textile": f"\"{p.text}\":{p.url}"}[fmt])
        elif isinstance(p, Em):
            out.append(f"_{p.text}_")
        else:
            out.append(str(p))
    return "".join(out)


def render(blocks, fmt):
    lines = []
    for b in blocks:
        kind = b[0]
        if kind == "p":
            lines += [inline(b[1], fmt), ""]
        elif kind == "kv":
            star = "**" if fmt == "md" else "*"
            lines += [f"{star}{b[1]}:{star} {inline(b[2], fmt)}", ""]
        elif kind in ("pre", "code"):
            text = b[1]
            if fmt == "md":
                fence = "```" if "```" not in text else "~~~~"
                lines += [fence + ("sh" if kind == "code" else "text"), text, fence, ""]
            elif fmt == "jira":
                lines += [("{code:bash}" if kind == "code" else "{noformat}") + text
                          + ("{code}" if kind == "code" else "{noformat}"), ""]
            else:
                lines += ["<pre>" + text.replace("<", "&lt;") + "</pre>", ""]
        elif kind == "table":
            head, rows = b[1], b[2]
            cell = (lambda c: str(c).replace("|", "\\|")) if fmt == "md" else (lambda c: str(c).replace("|", "/"))
            if fmt == "md":
                lines.append("| " + " | ".join(head) + " |")
                lines.append("|" + "---|" * len(head))
                lines += ["| " + " | ".join(cell(c) for c in r) + " |" for r in rows]
            elif fmt == "jira":
                lines.append("||" + "||".join(head) + "||")
                lines += ["|" + "|".join(cell(c) for c in r) + "|" for r in rows]
            else:
                lines.append("|_. " + " |_. ".join(head) + " |")
                lines += ["| " + " | ".join(cell(c) for c in r) + " |" for r in rows]
            lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def ci_info():
    env = os.environ
    ci = {k: env[k] for k in ("BUILD_URL", "JOB_NAME", "BUILD_NUMBER", "GIT_COMMIT", "GIT_BRANCH") if env.get(k)}
    if "BUILD_URL" not in ci and env.get("GITHUB_RUN_ID") and env.get("GITHUB_REPOSITORY"):
        server = env.get("GITHUB_SERVER_URL", "https://github.com")
        ci["BUILD_URL"] = f"{server}/{env['GITHUB_REPOSITORY']}/actions/runs/{env['GITHUB_RUN_ID']}"
        ci.setdefault("JOB_NAME", env.get("GITHUB_WORKFLOW", "GitHub Actions"))
        ci.setdefault("BUILD_NUMBER", env.get("GITHUB_RUN_NUMBER", env["GITHUB_RUN_ID"]))
        if env.get("GITHUB_SHA"):
            ci.setdefault("GIT_COMMIT", env["GITHUB_SHA"])
        if env.get("GITHUB_REF_NAME"):
            ci.setdefault("GIT_BRANCH", env["GITHUB_REF_NAME"])
    return ci


def ci_inline(ci):
    if not ci:
        return ["run outside CI"]
    build = f"{ci.get('JOB_NAME', 'build')} #{ci.get('BUILD_NUMBER', '?')}"
    parts = [Link(build, ci["BUILD_URL"]) if "BUILD_URL" in ci else build]
    if "GIT_COMMIT" in ci:
        parts.append(f", commit {ci['GIT_COMMIT'][:12]}")
    if "GIT_BRANCH" in ci:
        parts.append(f" on {ci['GIT_BRANCH']}")
    return parts


def tail(path, n):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()[-n:]
    except OSError:
        return []


# --------------------------------------------------------------- inputs

class Group:
    """One signature: what becomes one issue."""

    def __init__(self, kind, sig, summary):
        self.kind, self.sig, self.summary = kind, sig, summary[:250]
        self.label = sig_label(kind, sig)
        self.body = []           # blocks for a new issue
        self.occurrence = []     # blocks for a repeat
        self.attachments = []    # file paths
        self.labels = []


def regress_groups(path):
    """Failures in an mregress results.json, one Group per signature."""
    with open(path, encoding="utf-8") as f:
        res = json.load(f)
    outdir = os.path.dirname(os.path.abspath(path))
    sim = res.get("simulator", "msim")
    lists = res.get("lists") or []
    by = {}
    for r in res.get("runs", []):
        if r.get("status") not in ("fail", "xpass"):
            continue
        sig = r.get("signature") or signature(r.get("reason", ""))
        by.setdefault(sig, []).append(r)
    groups = []
    for sig, runs in sorted(by.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        first = runs[0]
        tests = sorted({r["test"] for r in runs})
        g = Group("mregress", sig, f"[mregress] {sig}")
        g.labels = ["mregress", sim] + (["xpass"] if first["status"] == "xpass" else [])
        seeds = ", ".join(f"{r['test']} seed {r['seed']}" for r in runs[:20]) + (" ..." if len(runs) > 20 else "")
        if lists or res.get("spec"):
            src = " ".join(lists) + (f" -spec {res['spec']}" if res.get("spec") else "")
            repro = f"mregress {src.strip()} -sim {sim} -filter '^{first['test']}$' -seed {first['seed']} -seeds 1"
        else:
            repro = f"sh {os.path.join(outdir, os.path.dirname(first.get('log', '')), 'cmd.sh')}"
        g.body = [
            ("p", f"mregress found {len(runs)} failing run(s) on {sim} with this signature."),
            ("kv", "Reason", ""), ("pre", first.get("reason", "")),
            ("kv", "Tests", ", ".join(tests)),
            ("kv", "Runs", seeds),
            ("kv", "Reproduce", ""), ("code", repro),
            ("kv", "Simulator command", ""), ("code", first.get("cmd", "")),
        ]
        log = os.path.join(outdir, first["log"]) if first.get("log") else ""
        lt = tail(log, TAIL_LINES) if log else []
        if lt:
            g.body += [("kv", f"Last {len(lt)} lines of {first['log']}", ""), ("pre", "\n".join(lt))]
        g.occurrence = [("p", f"Failed again: {len(runs)} run(s), {seeds}."),
                        ("kv", "Reason", ""), ("pre", first.get("reason", ""))]
        if log and os.path.exists(log):
            g.attachments.append(log)
            cmd_sh = os.path.join(os.path.dirname(log), "cmd.sh")
            if os.path.exists(cmd_sh):
                g.attachments.append(cmd_sh)
        groups.append(g)
    return groups


FINDING = re.compile(r"^(?P<file>.*?):(?P<line>\d+):(?P<col>\d+): (?:warning|error|info): \[(?P<rule>[A-Z]+\d+)\] (?P<text>.*)$")


def lint_groups(text, tool, group_by, source):
    """mlint / mdft findings ("file:line:col: warning: [RULE] text")."""
    findings = [m.groupdict() for m in (FINDING.match(l.strip()) for l in text.splitlines()) if m]
    by = {}
    for f in findings:
        # The finding text names the hierarchy, which is stable; the line
        # number is left out so an edit above it does not open a new issue.
        key = f["rule"] if group_by == "rule" else f"{f['rule']} {os.path.basename(f['file'])}: {signature(f['text'])}"
        by.setdefault(key, []).append(f)
    groups = []
    for key, fs in sorted(by.items()):
        rule = fs[0]["rule"]
        if group_by == "rule":
            summary = f"[{tool}] {rule}: {len(fs)} finding(s)"
        else:
            summary = f"[{tool}] {rule} {os.path.basename(fs[0]['file'])}: {fs[0]['text']}"
        g = Group(tool, key, summary)
        g.labels = [tool, rule]
        rows = [(f["file"], f["line"], f["text"]) for f in fs[:200]]
        g.body = [("p", f"{tool} reports {len(fs)} {rule} finding(s)."), ("table", ["File", "Line", "Finding"], rows)]
        if len(fs) > 200:
            g.body.append(("p", f"... and {len(fs) - 200} more"))
        g.occurrence = [("p", f"Still reported: {len(fs)} {rule} finding(s)."),
                        ("table", ["File", "Line", "Finding"], rows[:50])]
        if source and os.path.isfile(source):
            g.attachments.append(source)
        groups.append(g)
    return groups


# --------------------------------------------------------------- HTTP

def http(method, url, headers, body=None, raw=None, what="", with_link=False):
    h = {"Accept": "application/json", "User-Agent": f"mbug/{VERSION}"}
    h.update(headers)
    data = raw
    if body is not None:
        data = json.dumps(body).encode()
        h["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=h)
    path = urllib.parse.urlparse(url).path
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            obj = json.loads(text) if text.strip() else {}
            return (obj, resp.headers.get("Link", "")) if with_link else obj
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace")[:500]
        raise TrackerError(f"{what} {method} {path}: HTTP {e.code} {detail}") from None
    except urllib.error.URLError as e:
        raise TrackerError(f"{what} {method} {path}: {e.reason}") from None


def read_tail(path):
    with open(path, "rb") as f:
        f.seek(0, 2)
        size = f.tell()
        f.seek(max(0, size - LOG_LIMIT))
        return f.read()


# --------------------------------------------------------------- trackers
#
# A tracker finds issues by marker (the mbug-<hash> or gh-... string),
# creates, comments, attaches, and moves issues between open and closed.
# find() returns [{"key", "url", "done", "resolved"}], newest first.

class Tracker:
    name = "?"
    markup = "md"
    can_attach = False

    def __init__(self, cfg):
        self.cfg = cfg

    def find_many(self, markers):
        return {m: self.find(m) for m in markers}


class GitHubTracker(Tracker):
    name, markup = "github", "md"

    def __init__(self, cfg, repo=None):
        super().__init__(cfg)
        self.repo = repo or cfg.get("project")
        if not self.repo or not re.fullmatch(r"[\w.-]+/[\w.-]+", self.repo):
            raise TrackerError("GitHub needs the repository as owner/name (-project or \"project\")")
        self.token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
        self.api = os.environ.get("GITHUB_API_URL", "https://api.github.com").rstrip("/")
        self.made_labels = set()

    def req(self, method, path, body=None, with_link=False):
        h = {"Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"}
        if self.token:
            h["Authorization"] = f"Bearer {self.token}"
        url = path if path.startswith("http") else self.api + path
        return http(method, url, h, body, what="GitHub", with_link=with_link)

    def need_token(self):
        if not self.token:
            raise TrackerError("writing to GitHub needs GITHUB_TOKEN (or GH_TOKEN)")

    def issues(self, labels=None, since=None, state="all"):
        q = {"state": state, "per_page": 100, "sort": "created", "direction": "desc"}
        if labels:
            q["labels"] = ",".join(labels)
        if since:
            q["since"] = since
        url, out = f"/repos/{self.repo}/issues?" + urllib.parse.urlencode(q), []
        while url:
            page, link = self.req("GET", url, with_link=True)
            out += [i for i in page if "pull_request" not in i]
            m = re.search(r'<([^>]+)>;\s*rel="next"', link)
            url = m.group(1) if m else None
        return out

    def find(self, marker):
        return [{"key": str(i["number"]), "url": i["html_url"], "done": i["state"] == "closed",
                 "resolved": i.get("closed_at") or ""} for i in self.issues([marker])]

    def ensure_label(self, name, color="ededed"):
        if name in self.made_labels:
            return
        try:
            self.req("POST", f"/repos/{self.repo}/labels",
                     {"name": name, "color": color, "description": "mbug signature" if name.startswith("mbug-") else ""})
        except TrackerError as e:
            if "HTTP 422" not in str(e):      # 422: the label exists already
                raise
        self.made_labels.add(name)

    def create(self, g, text):
        self.need_token()
        labels = sorted(set(clean_label(l) for l in ["mbug", g.label] + g.labels + self.cfg.get("labels", [])))
        for l in labels:
            self.ensure_label(l)
        body = {"title": g.summary, "body": text, "labels": labels}
        if self.cfg.get("assignee"):
            body["assignees"] = [self.cfg["assignee"]]
        i = self.req("POST", f"/repos/{self.repo}/issues", body)
        return str(i["number"]), i.get("html_url", "")

    def comment(self, key, text):
        self.need_token()
        self.req("POST", f"/repos/{self.repo}/issues/{key}/comments", {"body": text})

    def set_open(self, key, is_open):
        self.need_token()
        self.req("PATCH", f"/repos/{self.repo}/issues/{key}",
                 {"state": "open"} if is_open else {"state": "closed", "state_reason": "completed"})



class JiraTracker(Tracker):
    name, markup, can_attach = "jira", "jira", True

    def __init__(self, cfg):
        super().__init__(cfg)
        env = os.environ
        self.url = (env.get("JIRA_URL") or "").rstrip("/")
        if not self.url:
            raise TrackerError("set JIRA_URL")
        if env.get("JIRA_PAT"):
            self.auth = f"Bearer {env['JIRA_PAT']}"
        elif env.get("JIRA_USER") and env.get("JIRA_TOKEN"):
            self.auth = "Basic " + base64.b64encode(f"{env['JIRA_USER']}:{env['JIRA_TOKEN']}".encode()).decode()
        else:
            raise TrackerError("set JIRA_USER and JIRA_TOKEN (Jira Cloud) or JIRA_PAT (Server / Data Center)")
        if not cfg.get("project"):
            raise TrackerError("Jira needs a project key (-project or \"project\")")

    def req(self, method, path, body=None, raw=None, headers=None):
        h = {"Authorization": self.auth}
        h.update(headers or {})
        return http(method, self.url + path, h, body, raw, what="Jira")

    def search(self, jql):
        # Jira Cloud replaced /search with /search/jql; Server / DC only has /search.
        q = urllib.parse.urlencode({"jql": jql, "fields": "status,labels,resolutiondate", "maxResults": 100})
        try:
            issues = self.req("GET", f"/rest/api/2/search/jql?{q}").get("issues", [])
        except TrackerError as e:
            if "HTTP 404" not in str(e) and "HTTP 405" not in str(e):
                raise
            issues = self.req("GET", f"/rest/api/2/search?{q}").get("issues", [])
        return [{"key": i["key"], "url": f"{self.url}/browse/{i['key']}",
                 "done": i["fields"]["status"].get("statusCategory", {}).get("key") == "done",
                 "resolved": i["fields"].get("resolutiondate") or "",
                 "labels": i["fields"].get("labels", [])} for i in issues]

    def jql_project(self):
        return f"project = {jql_quote(self.cfg['project'])}"

    def find(self, marker):
        return self.search(f"{self.jql_project()} AND labels = {jql_quote(marker)} ORDER BY created DESC")

    def find_many(self, markers):
        out, markers = {m: [] for m in markers}, list(markers)
        for k in range(0, len(markers), 50):
            chunk = ", ".join(jql_quote(m) for m in markers[k:k + 50])
            for i in self.search(f"{self.jql_project()} AND labels in ({chunk}) ORDER BY created DESC"):
                for m in markers[k:k + 50]:
                    if m in i["labels"]:
                        out[m].append(i)
        return out

    def fields(self, g, text):
        cfg = self.cfg
        f = {"project": {"key": cfg["project"]}, "issuetype": {"name": cfg.get("issuetype", "Bug")},
             "summary": g.summary, "description": text,
             "labels": sorted(set(clean_label(l) for l in ["mbug", g.label] + g.labels + cfg.get("labels", [])))}
        if cfg.get("components"):
            f["components"] = [{"name": c} for c in cfg["components"]]
        if cfg.get("priority"):
            f["priority"] = {"name": cfg["priority"]}
        if cfg.get("assignee"):
            f["assignee"] = {("accountId" if os.environ.get("JIRA_USER") else "name"): cfg["assignee"]}
        f.update(cfg.get("fields", {}))
        return f

    def create(self, g, text):
        key = self.req("POST", "/rest/api/2/issue", {"fields": self.fields(g, text)})["key"]
        return key, f"{self.url}/browse/{key}"

    def comment(self, key, text):
        self.req("POST", f"/rest/api/2/issue/{key}/comment", {"body": text})

    def attach(self, key, path):
        name = os.path.basename(path)
        boundary = uuid.uuid4().hex
        ctype = mimetypes.guess_type(name)[0] or "text/plain"
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
                f"Content-Type: {ctype}\r\n\r\n").encode() + read_tail(path) + f"\r\n--{boundary}--\r\n".encode()
        self.req("POST", f"/rest/api/2/issue/{key}/attachments", raw=body,
                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}", "X-Atlassian-Token": "no-check"})

    def set_state(self, key, name):
        ts = self.req("GET", f"/rest/api/2/issue/{key}/transitions").get("transitions", [])
        t = next((t for t in ts if t["name"].lower() == name.lower()
                  or t.get("to", {}).get("name", "").lower() == name.lower()), None)
        if not t:
            raise TrackerError(f"{key}: no transition '{name}' (has: {', '.join(x['name'] for x in ts) or 'none'})")
        self.req("POST", f"/rest/api/2/issue/{key}/transitions", {"transition": {"id": t["id"]}})



class RedmineTracker(Tracker):
    name, can_attach = "redmine", True

    def __init__(self, cfg):
        super().__init__(cfg)
        self.url = (os.environ.get("REDMINE_URL") or "").rstrip("/")
        self.key = os.environ.get("REDMINE_KEY")
        if not self.url or not self.key:
            raise TrackerError("set REDMINE_URL and REDMINE_KEY (My account > API access key)")
        if not cfg.get("project"):
            raise TrackerError("Redmine needs a project identifier (-project or \"project\")")
        self.markup = "md" if cfg.get("markup", "textile") in ("markdown", "common_mark", "md") else "textile"
        self.cache = {}

    def req(self, method, path, body=None, raw=None, headers=None):
        h = {"X-Redmine-API-Key": self.key}
        h.update(headers or {})
        return http(method, self.url + path, h, body, raw, what="Redmine")

    def lookup(self, path, kind, name):
        """Id of a tracker / status / priority by name."""
        if path not in self.cache:
            self.cache[path] = self.req("GET", path)
        items = next(v for k, v in self.cache[path].items() if isinstance(v, list))
        hit = next((i for i in items if i["name"].lower() == str(name).lower()), None)
        if not hit:
            raise TrackerError(f"Redmine has no {kind} '{name}' (has: {', '.join(i['name'] for i in items)})")
        return hit["id"]

    def find(self, marker):
        q = [("project_id", self.cfg["project"]), ("sort", "id:desc"), ("limit", "100"),
             ("f[]", "status_id"), ("op[status_id]", "*")]
        if self.cfg.get("signature_field"):
            q += [(f"cf_{self.cfg['signature_field']}", marker)]
        else:
            q += [("f[]", "description"), ("op[description]", "~"), ("v[description][]", marker)]
        issues = self.req("GET", "/issues.json?" + urllib.parse.urlencode(q)).get("issues", [])
        if not self.cfg.get("signature_field"):     # "~" is a substring match; keep exact lines only
            issues = [i for i in issues if re.search(rf"\bmbug-marker: {re.escape(marker)}\b", i.get("description", ""))]
        return [{"key": str(i["id"]), "url": f"{self.url}/issues/{i['id']}",
                 "done": bool(i.get("closed_on")), "resolved": i.get("closed_on") or ""} for i in issues]

    def marker_text(self, g, text, labels):
        extra = f"\n\nLabels: {', '.join(labels)}" if labels else ""
        return text + extra + f"\n\nmbug-marker: {g.label}\n"

    def issue_fields(self, g, text):
        cfg = self.cfg
        labels = sorted(set(clean_label(l) for l in g.labels + cfg.get("labels", [])))
        f = {"project_id": cfg["project"], "subject": g.summary, "description": self.marker_text(g, text, labels)}
        if cfg.get("tracker_name"):
            f["tracker_id"] = self.lookup("/trackers.json", "tracker", cfg["tracker_name"])
        if cfg.get("priority"):
            f["priority_id"] = self.lookup("/enumerations/issue_priorities.json", "priority", cfg["priority"])
        if cfg.get("assignee"):
            f["assigned_to_id"] = cfg["assignee"]
        if cfg.get("signature_field"):
            f["custom_fields"] = [{"id": int(cfg["signature_field"]), "value": g.label}]
        f.update(cfg.get("fields", {}))
        return f

    def upload(self, path):
        name = os.path.basename(path)
        tok = self.req("POST", "/uploads.json?" + urllib.parse.urlencode({"filename": name}), raw=read_tail(path),
                       headers={"Content-Type": "application/octet-stream"})["upload"]["token"]
        return {"token": tok, "filename": name, "content_type": mimetypes.guess_type(name)[0] or "text/plain"}

    def create(self, g, text, attachments=()):
        f = self.issue_fields(g, text)
        if attachments:
            f["uploads"] = [self.upload(p) for p in attachments]
        i = self.req("POST", "/issues.json", {"issue": f})["issue"]
        return str(i["id"]), f"{self.url}/issues/{i['id']}"

    def comment(self, key, text):
        self.req("PUT", f"/issues/{key}.json", {"issue": {"notes": text}})

    def set_state(self, key, name):
        sid = self.lookup("/issue_statuses.json", "status", name)
        self.req("PUT", f"/issues/{key}.json", {"issue": {"status_id": sid}})



def jql_quote(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


class DryRun(Tracker):
    """Renders what would be filed; talks to nobody."""

    def __init__(self, cfg, real_name):
        super().__init__(cfg)
        self.name = real_name
        self.markup = {"github": "md", "jira": "jira"}.get(real_name, "md" if cfg.get("markup") in ("markdown", "md", "common_mark") else "textile")

    def find(self, marker):
        return []


def make_tracker(name, cfg, dry_run):
    if dry_run:
        return DryRun(cfg, name)
    return {"github": GitHubTracker, "jira": JiraTracker, "redmine": RedmineTracker}[name](cfg)


def preview(name, cfg, g, text):
    """The fields a create would send, for -dry-run's -o file."""
    if name == "jira":
        f = {"project": {"key": cfg.get("project")}, "issuetype": {"name": cfg.get("issuetype", "Bug")},
             "summary": g.summary, "description": text}
    elif name == "redmine":
        f = {"project_id": cfg.get("project"), "subject": g.summary, "description": text}
    else:
        f = {"repo": cfg.get("project"), "title": g.summary, "body": text}
    f["labels"] = sorted(set(clean_label(l) for l in ["mbug", g.label] + g.labels + cfg.get("labels", [])))
    return f


# --------------------------------------------------------------- filing

def file_groups(groups, cfg, tr, ci, dry_run, comment, out):
    done, created, failed = [], 0, 0
    for g in groups:
        rec = {"signature": g.sig, "marker": g.label, "summary": g.summary}
        foot = [("kv", "Build", ci_inline(ci)), ("p", [Em(f"Filed by mbug {VERSION}; signature {g.label}.")])]
        try:
            hits = tr.find(g.label)
            open_ = [i for i in hits if not i["done"]]
            if open_:
                rec.update(key=open_[0]["key"], url=open_[0]["url"], action="comment" if comment else "seen")
                if comment:
                    tr.comment(open_[0]["key"], render(g.occurrence + foot, tr.markup))
            elif created >= cfg["max_issues"]:
                rec["action"] = "skipped (max_issues)"
            else:
                body = list(g.body)
                if hits:
                    body = [("p", ["This came back: the same signature was filed as ",
                                   Link(hits[0]["key"], hits[0]["url"]), " and closed."])] + body
                    rec["reopens"] = hits[0]["key"]
                if not tr.can_attach and g.attachments and "BUILD_URL" in ci and "artifact" not in ci["BUILD_URL"]:
                    if "JOB_NAME" in ci and "/actions/runs/" not in ci["BUILD_URL"]:   # Jenkins archives the run dirs
                        body.append(("kv", "Logs", Link("build artifacts", ci["BUILD_URL"].rstrip("/") + "/artifact/")))
                text = render(body + foot, tr.markup)
                rec["action"] = "create"
                if dry_run:
                    rec.update(fields=preview(tr.name, cfg, g, text), attachments=g.attachments)
                else:
                    atts = g.attachments if (cfg.get("attach") and tr.can_attach) else []
                    if isinstance(tr, RedmineTracker):
                        key, url = tr.create(g, text, atts)
                    else:
                        key, url = tr.create(g, text)
                        for p in atts:
                            try:
                                tr.attach(key, p)
                            except TrackerError as e:     # an attachment is not worth failing the run
                                print(f"mbug: {key}: cannot attach {p}: {e}", file=sys.stderr)
                    rec.update(key=key, url=url)
                created += 1
        except TrackerError as e:
            rec.update(action="error", error=str(e))
            failed += 1
        done.append(rec)
        print(f"mbug: {rec['action']:<8} {rec.get('key', ''):<10} {g.summary}" + (f"  ({rec['error']})" if "error" in rec else ""))
    write_out(out, {"tracker": tr.name, "build": ci, "issues": done})
    return failed


def write_out(path, obj):
    if path:
        with open(path, "w", encoding="utf-8") as f:
            json.dump(obj, f, indent=1)


# --------------------------------------------------------------- sync-github

def gh_marker(repo, number):
    return re.sub(r"[^A-Za-z0-9_.-]", "-", f"gh-{repo.replace('/', '-')}-{number}")


def sync_github(cfg, gh, tr, a):
    issues = gh.issues(a.gh_label, a.since)
    marks = {i["number"]: gh_marker(gh.repo, i["number"]) for i in issues}
    found = tr.find_many(list(marks.values())) if issues else {}
    done, failed, created = [], 0, 0
    for i in sorted(issues, key=lambda i: i["number"]):
        n, gh_open = i["number"], i["state"] == "open"
        rec = {"github": n, "marker": marks[n], "summary": i["title"]}
        hits = found.get(marks[n], [])
        j = hits[0] if hits else None
        try:
            if j is None:
                if not gh_open and not a.include_closed:
                    rec["action"] = "skip (closed on GitHub)"
                elif created >= cfg["max_issues"]:
                    rec["action"] = "skipped (max_issues)"
                else:
                    g = Group("github", str(n), f"[gh#{n}] {i['title']}")
                    g.label = marks[n]
                    g.labels = ["github"] + ["gh:" + l["name"] for l in i.get("labels", [])]
                    body = (i.get("body") or "").strip()
                    if len(body) > 20000:
                        body = body[:20000] + "\n... (cut; see the GitHub issue)"
                    blocks = [("p", ["From GitHub: ", Link(f"{gh.repo}#{n}", i["html_url"]),
                                     f", opened by {i['user']['login']} on {i['created_at'][:10]}."])]
                    if i.get("labels"):
                        blocks.append(("kv", "GitHub labels", ", ".join(l["name"] for l in i["labels"])))
                    blocks += [("pre", body or "(no description)"), ("p", [Em(f"Synced by mbug {VERSION}.")])]
                    text = render(blocks, tr.markup)
                    rec["action"] = "create"
                    if a.dry_run:
                        rec["fields"] = preview(tr.name, cfg, g, text)
                    else:
                        key, url = tr.create(g, text)
                        rec.update(key=key, url=url)
                        if a.backlink:
                            gh.comment(n, f"Tracked in {tr.name.capitalize()} as [{key}]({url}).\n\n_Filed by mbug {VERSION}._")
                        if not gh_open and cfg.get("close_state"):
                            tr.set_state(key, cfg["close_state"])
                    created += 1
            else:
                rec.update(key=j["key"], url=j["url"])
                other_later = bool(j["resolved"]) and norm_time(j["resolved"]) > norm_time(i["updated_at"])
                if not gh_open and not j["done"]:
                    rec["action"] = "close"
                    if not a.dry_run:
                        who = (i.get("closed_by") or {}).get("login", "someone")
                        tr.comment(j["key"], render([("p", [f"Closed on GitHub by {who}: ", Link(f"#{n}", i["html_url"])])], tr.markup))
                        if cfg.get("close_state"):
                            tr.set_state(j["key"], cfg["close_state"])
                elif gh_open and j["done"] and a.two_way and other_later:
                    rec["action"] = "close on GitHub"
                    if not a.dry_run:
                        gh.comment(n, f"Resolved in {tr.name.capitalize()} as [{j['key']}]({j['url']}).\n\n_Closed by mbug {VERSION}._")
                        gh.set_open(str(n), False)
                elif gh_open and j["done"] and not other_later:
                    rec["action"] = "reopen"
                    if not a.dry_run:
                        tr.comment(j["key"], render([("p", ["Open again on GitHub: ", Link(f"#{n}", i["html_url"])])], tr.markup))
                        if cfg.get("reopen_state"):
                            tr.set_state(j["key"], cfg["reopen_state"])
                else:
                    rec["action"] = "in step"
        except TrackerError as e:
            rec.update(action="error", error=str(e))
            failed += 1
        done.append(rec)
        if rec["action"] != "in step" or not a.q:
            print(f"mbug: {rec['action']:<15} {rec.get('key', ''):<10} #{n} {i['title']}"
                  + (f"  ({rec['error']})" if "error" in rec else ""))
    write_out(a.o, {"tracker": tr.name, "repo": gh.repo, "issues": done})
    return failed


# --------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(prog="mbug", description="File regression / lint / DFT failures in GitHub Issues, "
                                 "Redmine or Jira, and sync GitHub issues (see the module docstring).")
    sub = ap.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-tracker", choices=TRACKERS, help="github (default), redmine or jira")
    common.add_argument("-config", metavar="FILE", help="JSON settings (tracker, project, labels, ...)")
    common.add_argument("-project", help="GitHub owner/name, Jira project key, or Redmine project identifier")
    common.add_argument("-label", action="append", default=[], help="extra label (repeatable)")
    common.add_argument("-max-issues", type=int, dest="max_issues", help="create at most N issues (default 20)")
    common.add_argument("-no-attach", action="store_true", help="do not attach logs")
    common.add_argument("-dry-run", action="store_true", help="print what would be filed; no network")
    common.add_argument("-o", metavar="FILE", help="write the actions taken as JSON")
    r = sub.add_parser("regress", parents=[common], help="failures in an mregress results.json")
    r.add_argument("results", help="<mregress -o dir>/results.json")
    r.add_argument("-no-comment", action="store_true", help="do not comment on an issue that is already open")
    l = sub.add_parser("lint", parents=[common], help="mlint / mdft findings")
    l.add_argument("log", help="mlint or mdft output file, or - for stdin")
    l.add_argument("-tool", default="mlint", help="mlint (default) or mdft: names the issues")
    l.add_argument("-group", choices=("rule", "finding"), default="rule",
                   help="one issue per rule (default) or per finding")
    l.add_argument("-no-comment", action="store_true", help="do not comment on an issue that is already open")
    s = sub.add_parser("sync-github", parents=[common], help="sync a GitHub repository's issues into Jira or Redmine")
    s.add_argument("-repo", required=True, help="GitHub owner/name")
    s.add_argument("-gh-label", action="append", default=[], dest="gh_label",
                   help="only GitHub issues with this label (repeatable: all of them)")
    s.add_argument("-since", help="only issues updated since this ISO time")
    s.add_argument("-include-closed", action="store_true", help="also file issues already closed on GitHub")
    s.add_argument("-two-way", action="store_true", dest="two_way",
                   help="close a GitHub issue when the other side resolves it")
    s.add_argument("-backlink", action="store_true", help="comment the new key on each GitHub issue filed")
    s.add_argument("-q", action="store_true", help="do not list issues that are already in step")
    ap.add_argument("-version", action="version", version=f"mbug {VERSION}")
    a = ap.parse_args(argv)

    cfg = dict(DEFAULTS)
    if a.config:
        with open(a.config, encoding="utf-8") as f:
            user = json.load(f)
        bad = [k for k in user if re.search(r"token|password|secret|api_?key|pat$", k, re.I)]
        if bad:
            ap.error(f"{a.config}: {', '.join(bad)}: credentials go in the environment, not in a file")
        cfg.update(user)
    if a.project:
        cfg["project"] = a.project
    if a.max_issues is not None:
        cfg["max_issues"] = a.max_issues
    cfg["labels"] = list(cfg.get("labels", [])) + a.label
    if a.no_attach:
        cfg["attach"] = False
    name = a.tracker or os.environ.get("MBUG_TRACKER") or cfg.get("tracker") or "github"
    if name not in TRACKERS:
        ap.error(f"unknown tracker {name} (github, redmine or jira)")
    if a.cmd == "sync-github" and name == "github":
        ap.error("sync-github copies GitHub issues into another tracker: pass -tracker jira or -tracker redmine")
    if name == "github" and not cfg.get("project") and os.environ.get("GITHUB_REPOSITORY"):
        cfg["project"] = os.environ["GITHUB_REPOSITORY"]
    if not cfg.get("project"):
        ap.error("no project: pass -project (GitHub owner/name, Jira key, Redmine identifier) or set it in -config")

    try:
        tr = make_tracker(name, cfg, a.dry_run)
        if a.cmd == "sync-github":
            gh = GitHubTracker(cfg, a.repo)
            if (a.two_way or a.backlink) and not gh.token and not a.dry_run:
                ap.error("-two-way and -backlink write to GitHub: set GITHUB_TOKEN")
            return 1 if sync_github(cfg, gh, tr, a) else 0
    except TrackerError as e:
        if a.cmd == "sync-github" and "HTTP" in str(e):
            print(f"mbug: {e}", file=sys.stderr)
            return 1
        ap.error(str(e))

    if a.cmd == "regress":
        if not os.path.exists(a.results):
            ap.error(f"{a.results}: no such file")
        groups = regress_groups(a.results)
    else:
        if a.log == "-":
            text, source = sys.stdin.read(), None
        else:
            with open(a.log, encoding="utf-8", errors="replace") as f:
                text, source = f.read(), a.log
        groups = lint_groups(text, a.tool, a.group, source)
    if not groups:
        print("mbug: nothing to file")
        write_out(a.o, {"tracker": name, "build": ci_info(), "issues": []})
        return 0
    failed = file_groups(groups, cfg, tr, ci_info(), a.dry_run, not a.no_comment, a.o)
    if a.dry_run:
        print(f"mbug: dry run ({name}), nothing sent")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
