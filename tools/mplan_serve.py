#!/usr/bin/env python3
"""mplan serve / mplan push: a shared dashboard for many plans and many people.

    mplan serve -root /srv/mplan [-port 8080] [-bind 127.0.0.1] [-users users.txt]
    mplan push  http://host:8080 plan.d [-project uart] [-token T]

The server keeps one directory per project under ``-root``: the snapshot
history (``history.jsonl``), the last ``plan.json`` and ``plan.html``, the
bucket annotations people made (``annotations.json``) and who changed what
(``audit.jsonl``). ``mplan push`` uploads the output directory of ``mplan
report`` (a CI job does that after its regression). A project directory
that ``mplan report -o`` wrote straight into works too.

Pages: ``/`` lists the projects (score, status, tests, requirements,
failure buckets, change since the previous snapshot, a sparkline);
``/p/NAME/`` is one project (trend, snapshots, the failure buckets with a
form to set an owner, a status, a bug id and a note); ``/p/NAME/plan.html``
is the full report.

JSON: ``GET /api/projects``, ``GET /api/p/NAME``, ``GET /api/p/NAME/history``,
``GET /api/p/NAME/rules`` (the annotations, which ``mplan report -rules
http://host:8080/api/p/NAME/rules`` reads as triage rules, so the next
report carries what people set on the dashboard), ``POST /api/p/NAME/push``, ``POST /api/p/NAME/annotate``.

Users. ``-users FILE`` has one ``name:token[:role]`` a line; the role is
``read``, ``write`` (default) or ``admin``. A request carries the token as
``Authorization: Bearer TOKEN``; the browser signs in at ``/login``.
Without ``-users`` everybody may read and nobody may write, unless
``-open`` (anyone who reaches the port may write: for a trusted network
only). With ``-private`` reading needs a token too. The server binds to
127.0.0.1 unless ``-bind`` says otherwise, and speaks plain HTTP: put a
TLS proxy in front of it for anything beyond a trusted network.
"""

import datetime
import hmac
import html
import http.server
import json
import os
import re
import threading
import urllib.error
import urllib.parse
import urllib.request

NAME_RX = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.\-]{0,63}$")
MAX_BODY = 256 << 20
STATUSES = ("open", "known", "waived")
ROLES = ("read", "write", "admin")

PAGE_CSS = """
a{color:var(--series-1)}nav{margin:0 0 16px;color:var(--text-2)}nav a{margin-right:14px}
input,select,button{font:inherit;color:inherit;background:var(--surface);border:1px solid var(--grid);border-radius:5px;padding:2px 6px}
input{width:9em}button{cursor:pointer}.ok{color:var(--good)}.err{color:var(--critical)}
svg.spark{width:120px;height:24px;vertical-align:middle}
"""

PAGE_JS = """
document.querySelectorAll('form.ann').forEach(function(f){f.addEventListener('submit',function(e){e.preventDefault();
var b={signature:f.dataset.sig};['status','owner','bug','note'].forEach(function(k){b[k]=f.elements[k].value});
var m=f.querySelector('.msg');m.textContent='';
fetch(f.dataset.url,{method:'POST',headers:{'Content-Type':'application/json'},credentials:'same-origin',body:JSON.stringify(b)})
.then(function(r){return r.json().then(function(j){m.className='msg '+(r.ok?'ok':'err');m.textContent=r.ok?'saved':(j.error||r.status)})})
.catch(function(x){m.className='msg err';m.textContent=String(x)})})});
"""


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def load_users(path):
    users = {}
    if not path:
        return users
    with open(path, encoding="utf-8") as f:
        for no, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(":")
            if len(parts) not in (2, 3) or not parts[0] or not re.match(r"^[A-Za-z0-9_.~+/=\-]{8,}$", parts[1]):
                raise SystemExit(f"mplan: {path}:{no}: expected name:token[:role], the token 8 or more of A-Z a-z 0-9 _ . ~ + / = -")
            role = parts[2] if len(parts) == 3 else "write"
            if role not in ROLES:
                raise SystemExit(f"mplan: {path}:{no}: the role is read, write or admin")
            users[parts[1]] = (parts[0], role)
    return users


class Store:
    """The project directories under the root. Writes are atomic and one at a time per project."""

    def __init__(self, root):
        self.root = os.path.abspath(root)
        os.makedirs(self.root, exist_ok=True)
        self.locks = {}
        self.guard = threading.Lock()

    def lock(self, name):
        with self.guard:
            return self.locks.setdefault(name, threading.Lock())

    def path(self, name, *parts):
        if not NAME_RX.match(name) or name in (".", ".."):
            raise KeyError(name)
        return os.path.join(self.root, name, *parts)

    def projects(self):
        out = []
        for n in sorted(os.listdir(self.root)):
            if NAME_RX.match(n) and os.path.isfile(os.path.join(self.root, n, "history.jsonl")):
                out.append(n)
        return out

    def history(self, name):
        snaps = []
        try:
            with open(self.path(name, "history.jsonl"), encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            snaps.append(json.loads(line))
                        except ValueError:
                            pass
        except OSError:
            pass
        return snaps

    def read_json(self, name, fname, default):
        try:
            with open(self.path(name, fname), "rb") as f:
                return json.loads(f.read())
        except (OSError, ValueError):
            return default

    def write(self, name, fname, text):
        path = self.path(name, fname)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp = f"{path}.tmp{threading.get_ident()}"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, path)

    def audit(self, name, who, what, detail):
        with open(self.path(name, "audit.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps({"time": now_iso(), "user": who, "action": what, **detail}, sort_keys=True) + "\n")

    def push(self, name, who, plan, page):
        snap = plan.get("snapshot")
        if plan.get("format") != "mplan" or not isinstance(snap, dict) or "score" not in snap:
            raise ValueError("the body's plan is not an mplan plan.json")
        with self.lock(name):
            hist = self.history(name)
            if hist and snap.get("label") and hist[-1].get("label") == snap.get("label"):
                hist.pop()                      # the same label again replaces that snapshot
            hist.append(snap)
            self.write(name, "history.jsonl", "".join(json.dumps(s, sort_keys=True) + "\n" for s in hist))
            self.write(name, "plan.json", json.dumps(plan, sort_keys=True) + "\n")
            if page:
                self.write(name, "plan.html", page)
            self.audit(name, who, "push", {"label": snap.get("label"), "score": snap.get("score")})
        return len(hist)

    def annotate(self, name, who, body):
        sig = body.get("signature")
        if not isinstance(sig, str) or not sig or len(sig) > 2000:
            raise ValueError("signature is missing")
        ann = {"status": str(body.get("status") or "open"), "owner": str(body.get("owner") or "")[:80],
               "bug": str(body.get("bug") or "")[:120], "note": str(body.get("note") or "")[:500]}
        if ann["status"] not in STATUSES:
            raise ValueError("status is open, known or waived")
        with self.lock(name):
            if not os.path.isfile(self.path(name, "history.jsonl")):
                raise KeyError(name)
            anns = self.read_json(name, "annotations.json", {})
            anns[sig] = {**ann, "by": who, "time": now_iso()}
            self.write(name, "annotations.json", json.dumps(anns, indent=1, sort_keys=True) + "\n")
            self.audit(name, who, "annotate", {"signature": sig, **ann})
        return anns[sig]

    def summary(self, name):
        hist = self.history(name)
        last = hist[-1] if hist else {}
        prev = hist[-2] if len(hist) > 1 else None
        plan = self.read_json(name, "plan.json", {})
        feats = last.get("features") or {}
        return {"name": name, "title": (plan.get("plan") or {}).get("title") or last.get("plan") or name,
                "score": last.get("score"), "status": last.get("status"), "label": last.get("label"), "time": last.get("time"),
                "change": round(last.get("score", 0) - prev.get("score", 0), 2) if prev else None,
                "features": len(feats), "features_met": sum(1 for v in feats.values() if v[1] == "met"),
                "tests": last.get("tests") or {}, "reqs": last.get("reqs"), "coverage": last.get("coverage") or {},
                "buckets": len(last.get("buckets") or {}), "snapshots": len(hist),
                "trend": [s.get("score", 0) for s in hist[-40:]]}


def spark(vals):
    if len(vals) < 2:
        return ""
    w, h = 120, 24
    pts = " ".join(f"{2 + (w - 4) * i / (len(vals) - 1):.1f},{h - 2 - (h - 4) * max(0.0, min(100.0, v)) / 100:.1f}" for i, v in enumerate(vals))
    return (f"<svg class='spark' viewBox='0 0 {w} {h}' role='img' aria-label='plan score over the last {len(vals)} snapshots'>"
            f"<polyline fill='none' stroke='var(--series-1)' stroke-width='2' stroke-linejoin='round' points='{pts}'/></svg>")


def page(mp, title, body, user, script=""):
    esc = html.escape
    who = f"signed in as {esc(user[0])} ({user[1]})" if user else "<a href='/login'>sign in</a>"
    return (f"<!DOCTYPE html><html><head><meta charset='utf-8'><title>{esc(title)}</title><style>{mp.CSS}{PAGE_CSS}</style></head>"
            f"<body><div class='viz-root'><nav><a href='/'>All plans</a>{who}</nav>{body}"
            + (f"<script>{script}</script>" if script else "") + "</div></body></html>")


def st(status):
    s = html.escape(str(status or "no data"))
    return f"<span class='st {s.split()[0]}'>{s}</span>"


def index_page(mp, store, user):
    esc = html.escape
    rows = []
    for n in store.projects():
        s = store.summary(n)
        t = s["tests"]
        ran = sum(t.get(k, 0) for k in ("pass", "fail", "xfail"))
        score = s["score"] or 0.0
        ch = "" if s["change"] is None else mp.signed(s["change"])
        rq = f"{s['reqs']['verified']}/{s['reqs']['total']}" if s.get("reqs") else ""
        rows.append(f"<tr><td><a href='/p/{esc(n)}/'>{esc(s['title'])}</a><br><code>{esc(n)}</code></td><td class='num'>{score:.1f}%</td>"
                    f"<td><span class='bar'><i style='width:{max(0.0, min(100.0, score)):.1f}%'></i></span></td><td class='num'>{ch}</td>"
                    f"<td>{st(s['status'])}</td><td class='num'>{s['features_met']}/{s['features']}</td>"
                    f"<td class='num'>{t.get('pass', 0)}/{ran}</td><td class='num'>{rq}</td><td class='num'>{s['buckets']}</td>"
                    f"<td>{spark(s['trend'])}</td><td>{esc(str(s['label'] or ''))}<br><span class='sub'>{esc(str(s['time'] or ''))}</span></td></tr>")
    body = "<h1>Verification plans</h1>"
    if rows:
        body += ("<table><tr><th>Plan</th><th>Score</th><th></th><th>Change</th><th>Status</th><th>Features met</th><th>Tests passing</th>"
                 "<th>Requirements</th><th>Failure buckets</th><th>Trend</th><th>Last snapshot</th></tr>" + "".join(rows) + "</table>")
    else:
        body += "<p class='sub'>No plans yet. Upload one with <code>mplan push URL plan.d</code>.</p>"
    return page(mp, "mplan: verification plans", body, user)


def project_page(mp, store, name, user, can_write):
    esc = html.escape
    hist = store.history(name)
    if not hist:
        return None
    s = store.summary(name)
    plan = store.read_json(name, "plan.json", {})
    anns = store.read_json(name, "annotations.json", {})
    t = s["tests"]
    ran = sum(t.get(k, 0) for k in ("pass", "fail", "xfail"))
    tiles = [(f"{(s['score'] or 0):.1f}%", "plan score"), (f"{s['features_met']}/{s['features']}", "features met"),
             (f"{t.get('pass', 0)}/{ran}", "tests passing"), (str(s["buckets"]), "failure buckets")]
    if s.get("reqs"):
        tiles.append((f"{s['reqs']['verified']}/{s['reqs']['total']}", "requirements verified"))
    body = [f"<h1>{esc(s['title'])}</h1><p class='sub'>Snapshot {esc(str(s['label']))}, {esc(str(s['time']))}. Status: {st(s['status'])} "
            f"<a href='plan.html'>Full report</a> <a href='/api/p/{esc(name)}'>JSON</a></p><div class='tiles'>"
            + "".join(f"<div class='tile'><b>{a}</b><span>{esc(b)}</span></div>" for a, b in tiles) + "</div>",
            "<h2>Progress</h2>" + mp.trend_svg(hist)]
    buckets = (plan.get("triage") or {}).get("buckets") or []
    body.append("<h2>Failure buckets</h2>")
    if buckets:
        rows = []
        for b in buckets:
            a = anns.get(b["signature"], {})
            cur = {k: a.get(k, b.get(k, "") if k != "owner" else ", ".join(b.get("owners") or [])) for k in ("status", "owner", "bug", "note")}
            by = f"<br><span class='sub'>{esc(a['by'])}, {esc(a['time'])}</span>" if a.get("by") else ""
            if can_write:
                opts = "".join(f"<option{' selected' if cur['status'] == x else ''}>{x}</option>" for x in STATUSES)
                form = (f"<form class='ann' data-url='/api/p/{esc(name)}/annotate' data-sig='{esc(b['signature'], quote=True)}'>"
                        f"<select name='status' aria-label='status'>{opts}</select> "
                        f"<input name='owner' aria-label='owner' placeholder='owner' value='{esc(cur['owner'], quote=True)}'> "
                        f"<input name='bug' aria-label='bug' placeholder='bug' value='{esc(cur['bug'], quote=True)}'> "
                        f"<input name='note' aria-label='note' placeholder='note' value='{esc(cur['note'], quote=True)}'> "
                        f"<button>Save</button> <span class='msg'></span></form>{by}")
            else:
                form = esc(", ".join(x for x in (cur["status"], cur["owner"], cur["bug"], cur["note"]) if x)) + by
            rows.append(f"<tr><td class='num'>{b['runs']}</td><td><span class='tag {esc(b['class'])}'>{esc(b['class'])}</span></td>"
                        f"<td><code>{esc(b['signature'])}</code><br><span class='sub'>{esc(', '.join(b['tests'][:8]))}</span></td>"
                        f"<td>{esc(', '.join(b.get('features') or []))}</td><td>{form}</td></tr>")
        body.append("<table><tr><th>Runs</th><th>Class</th><th>Signature and tests</th><th>Features blocked</th><th>Triage</th></tr>"
                    + "".join(rows) + "</table>")
        if not can_write:
            body.append("<p class='sub'>Sign in with a write token to set owners and statuses.</p>")
    else:
        body.append("<p>No failing runs in the last snapshot.</p>")
    return page(mp, f"mplan: {s['title']}", "".join(body), user, mp.CHART_JS + PAGE_JS)


def make_handler(mp, store, users, open_write, private):
    class Handler(http.server.BaseHTTPRequestHandler):
        server_version = f"mplan/{mp.VERSION}"
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            if not getattr(self.server, "quiet", False):
                http.server.BaseHTTPRequestHandler.log_message(self, fmt, *args)

        def user(self):
            token = ""
            auth = self.headers.get("Authorization", "")
            if auth.startswith("Bearer "):
                token = auth[7:].strip()
            else:
                for part in self.headers.get("Cookie", "").split(";"):
                    k, _, v = part.strip().partition("=")
                    if k == "mplan_token":
                        token = v
            for known, who in users.items():
                if token and hmac.compare_digest(known.encode(), token.encode()):
                    return who
            return None

        def send(self, code, body, ctype="application/json", extra=()):
            if isinstance(body, (dict, list)):
                body = json.dumps(body, sort_keys=True) + "\n"
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype + ("; charset=utf-8" if ctype.startswith("text/") or ctype == "application/json" else ""))
            self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Cache-Control", "no-store")
            for k, v in extra:
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(data)

        def file(self, name, fname, ctype):
            try:
                with open(store.path(name, fname), "rb") as f:
                    # An uploaded report runs its chart script in a sandbox: no access to this site's cookie.
                    self.send(200, f.read(), ctype, [("Content-Security-Policy", "sandbox allow-scripts")] if ctype == "text/html" else ())
            except (OSError, KeyError):
                self.send(404, {"error": "not found"})

        def do_HEAD(self):
            self.do_GET()

        def do_GET(self):
            url = urllib.parse.urlsplit(self.path)
            parts = [urllib.parse.unquote(p) for p in url.path.split("/") if p]
            user = self.user()
            if parts == ["login"]:
                return self.send(200, page(mp, "mplan: sign in", "<h1>Sign in</h1><form method='post' action='/login'>"
                                           "<input type='password' name='token' aria-label='token' placeholder='token' style='width:20em'> "
                                           "<button>Sign in</button></form>", user), "text/html")
            if private and user is None:
                if parts[:1] == ["api"]:
                    return self.send(401, {"error": "a token is needed"}, extra=[("WWW-Authenticate", "Bearer")])
                return self.send(303, "", "text/plain", [("Location", "/login")])
            can_write = open_write or (user is not None and user[1] in ("write", "admin"))
            try:
                if not parts:
                    return self.send(200, index_page(mp, store, user), "text/html")
                if parts == ["api", "projects"]:
                    return self.send(200, [store.summary(n) for n in store.projects()])
                if parts[0] == "p" and len(parts) >= 2:
                    name = parts[1]
                    if len(parts) == 2:
                        if not url.path.endswith("/"):
                            return self.send(303, "", "text/plain", [("Location", f"/p/{urllib.parse.quote(name)}/")])
                        doc = project_page(mp, store, name, user, can_write)
                        return self.send(200, doc, "text/html") if doc else self.send(404, {"error": "no such plan"})
                    if parts[2:] == ["plan.html"]:
                        return self.file(name, "plan.html", "text/html")
                    if parts[2:] == ["plan.json"]:
                        return self.file(name, "plan.json", "application/json")
                if parts[:2] == ["api", "p"] and len(parts) >= 3:
                    name = parts[2]
                    if name not in store.projects():
                        return self.send(404, {"error": "no such plan"})
                    if len(parts) == 3:
                        return self.send(200, {**store.summary(name), "annotations": store.read_json(name, "annotations.json", {})})
                    if parts[3:] == ["history"]:
                        return self.send(200, store.history(name))
                    if parts[3:] == ["rules"]:
                        return self.send(200, {"format": "mplan-annotations", "buckets": store.read_json(name, "annotations.json", {})})
            except KeyError:
                pass
            self.send(404, {"error": "not found"})

        def do_POST(self):
            url = urllib.parse.urlsplit(self.path)
            parts = [urllib.parse.unquote(p) for p in url.path.split("/") if p]
            try:
                n = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                n = -1
            if n < 0 or n > MAX_BODY:
                self.close_connection = True
                return self.send(413, {"error": "the body is too large"})
            raw = self.rfile.read(n)
            if parts == ["login"]:
                token = urllib.parse.parse_qs(raw.decode("utf-8", "replace")).get("token", [""])[0]
                ok = any(hmac.compare_digest(k.encode(), token.encode()) for k in users)
                if not ok:
                    return self.send(403, page(mp, "mplan: sign in", "<h1>Sign in</h1><p class='err'>That token is not known.</p>"
                                               "<p><a href='/login'>Try again</a></p>", None), "text/html")
                return self.send(303, "", "text/plain", [("Location", "/"), ("Set-Cookie", f"mplan_token={token}; Path=/; HttpOnly; SameSite=Strict")])
            user = self.user()
            if not (open_write or (user is not None and user[1] in ("write", "admin"))):
                return self.send(401 if user is None else 403, {"error": "a write token is needed"}, extra=[("WWW-Authenticate", "Bearer")])
            # A form post from another site cannot carry this content type: no cross-site writes.
            if self.headers.get("Content-Type", "").split(";")[0].strip() != "application/json":
                return self.send(415, {"error": "send application/json"})
            who = user[0] if user else "anonymous"
            try:
                body = json.loads(raw)
                if not isinstance(body, dict):
                    raise ValueError("the body is not a JSON object")
                if parts[:2] == ["api", "p"] and len(parts) == 4 and parts[3] == "push":
                    count = store.push(parts[2], who, body.get("plan") or {}, body.get("html") or "")
                    return self.send(200, {"ok": True, "project": parts[2], "snapshots": count})
                if parts[:2] == ["api", "p"] and len(parts) == 4 and parts[3] == "annotate":
                    return self.send(200, {"ok": True, "annotation": store.annotate(parts[2], who, body)})
            except KeyError:
                return self.send(404, {"error": "no such plan (a name is letters, digits, '_', '-' and '.')"})
            except ValueError as e:
                return self.send(400, {"error": str(e)})
            self.send(404, {"error": "not found"})

    return Handler


def serve(a, mp):
    users = load_users(a.users)
    if a.private and not users:
        raise SystemExit("mplan: -private needs -users FILE")
    store = Store(a.root)
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), make_handler(mp, store, users, a.open, a.private))
    srv.daemon_threads = True
    srv.quiet = a.q
    host, port = srv.server_address[:2]
    print(f"mplan: serving {store.root} on http://{host}:{port}/ ({len(store.projects())} plan(s), "
          + ("anyone may write" if a.open else f"{len(users)} user(s)" if users else "read-only") + ")", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
    return 0


def push(a, mp):
    try:
        with open(os.path.join(a.dir, "plan.json"), "rb") as f:
            plan = json.loads(f.read())
    except (OSError, ValueError) as e:
        raise SystemExit(f"mplan: {os.path.join(a.dir, 'plan.json')}: {e}")
    page_text = ""
    try:
        with open(os.path.join(a.dir, "plan.html"), encoding="utf-8") as f:
            page_text = f.read()
    except OSError:
        pass
    name = a.project or (plan.get("plan") or {}).get("id") or ""
    if not NAME_RX.match(name):
        raise SystemExit(f"mplan: push: {name!r} is not a project name (letters, digits, '_', '-', '.'); use -project")
    url = f"{a.url.rstrip('/')}/api/p/{urllib.parse.quote(name)}/push"
    if not url.startswith(("http://", "https://")):
        raise SystemExit("mplan: push: the server is an http:// or https:// URL")
    req = urllib.request.Request(url, data=json.dumps({"plan": plan, "html": page_text}).encode("utf-8"),
                                 headers={"Content-Type": "application/json"}, method="POST")
    token = a.token or os.environ.get("MPLAN_TOKEN", "")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(req, timeout=a.timeout) as r:
            got = json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            msg = json.loads(e.read()).get("error", "")
        except ValueError:
            msg = ""
        print(f"mplan: push: {url}: HTTP {e.code}" + (f": {msg}" if msg else ""))
        return 1
    except (urllib.error.URLError, OSError) as e:
        print(f"mplan: push: {url}: {getattr(e, 'reason', e)}")
        return 1
    print(f"mplan: pushed {name} to {a.url.rstrip('/')}/p/{name}/ ({got.get('snapshots')} snapshot(s))")
    return 0
