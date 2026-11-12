#!/usr/bin/env python3
"""mplan serve / push end to end (tests/mplan.cmake): serve_test.py MPLAN.PY PLAN.VPLAN RESULTS_DIR OUT_DIR"""
import json
import os
import re
import subprocess
import sys
import threading
import urllib.error
import urllib.request

mplan, vplan, results, out = sys.argv[1:5]
os.makedirs(out, exist_ok=True)
users = os.path.join(out, "users.txt")
with open(users, "w") as f:
    f.write("# name:token[:role]\nalice:alice-token-1\nbob:bob-token-22:read\ndave:dave-token-333\nerin:erin-token-4444\n")


def call(url, token=None, body=None, ctype="application/json"):
    req = urllib.request.Request(url, data=None if body is None else json.dumps(body).encode(), method="POST" if body is not None else "GET")
    if body is not None:
        req.add_header("Content-Type", ctype)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def expect(what, cond, detail=""):
    if not cond:
        raise SystemExit(f"serve_test: {what} failed\n{detail}")


def run(*args, env=None):
    p = subprocess.run([sys.executable, mplan, *args], capture_output=True, text=True, env={**os.environ, **(env or {})})
    return p.returncode, p.stdout + p.stderr


plan_d = os.path.join(out, "plan")
run("report", vplan, "-results", results, "-o", plan_d, "-label", "s1", "-time", "2026-11-01T00:00:00Z")
srv = subprocess.Popen([sys.executable, mplan, "serve", "-root", os.path.join(out, "root"), "-port", "0", "-users", users, "-q"],
                       stdout=subprocess.PIPE, text=True)
try:
    line = srv.stdout.readline()
    m = re.search(r"on (http://\S+?)/ ", line)
    expect("server start", m, line)
    base = m.group(1)
    code, text = call(base + "/")
    expect("empty index", code == 200 and "No plans yet" in text, text)
    rc, text = run("push", base, plan_d)
    expect("push without a token is refused", rc == 1 and "HTTP 401" in text, text)
    rc, text = run("push", base, plan_d, env={"MPLAN_TOKEN": "bob-token-22"})
    expect("push with a read token is refused", rc == 1 and "HTTP 403" in text, text)
    rc, text = run("push", base, plan_d, "-token", "alice-token-1")
    expect("push", rc == 0 and "pushed counter" in text and "(1 snapshot(s))" in text, text)
    run("report", vplan, "-results", results, "-o", plan_d, "-label", "s2", "-time", "2026-11-02T00:00:00Z")
    rc, text = run("push", base, plan_d, "-token", "alice-token-1")
    expect("second push", rc == 0 and "(2 snapshot(s))" in text, text)
    rc, text = run("push", base, plan_d, "-token", "alice-token-1")
    expect("same label replaces", rc == 0 and "(2 snapshot(s))" in text, text)
    rc, text = run("push", base, plan_d, "-token", "alice-token-1", "-project", "../x")
    expect("bad project name", rc != 0 and "not a project name" in text, text)

    code, text = call(base + "/")
    expect("index", code == 200 and "Counter verification plan" in text and "<svg class='spark'" in text, text)
    code, text = call(base + "/api/projects")
    got = json.loads(text)
    expect("api projects", code == 200 and got[0]["name"] == "counter" and got[0]["snapshots"] == 2 and got[0]["buckets"] == 1, text)
    code, text = call(base + "/p/counter/")
    expect("project page, read-only", code == 200 and "Failure buckets" in text and "scoreboard mismatch" in text and "<form class='ann'" not in text, text)
    code, text = call(base + "/p/counter/", "alice-token-1")
    expect("project page, writer", code == 200 and "<form class='ann'" in text, text)
    code, text = call(base + "/p/counter/plan.html")
    expect("full report", code == 200 and "<h2>Features</h2>" in text)
    code, text = call(base + "/p/nope/")
    expect("unknown project", code == 404, text)
    code, text = call(base + "/p/..%2F..%2Fusers.txt/plan.html")
    expect("no path escape", code == 404, text)

    sig = json.loads(call(base + "/p/counter/plan.json")[1])["triage"]["buckets"][0]["signature"]
    ann = {"signature": sig, "status": "known", "owner": "carol", "bug": "CNT-77", "note": "ref/ model \"bug\""}
    expect("annotate without a token", call(base + "/api/p/counter/annotate", None, ann)[0] == 401)
    expect("annotate with a read token", call(base + "/api/p/counter/annotate", "bob-token-22", ann)[0] == 403)
    expect("annotate as a form post", call(base + "/api/p/counter/annotate", "alice-token-1", ann, "text/plain")[0] == 415)
    expect("annotate with a bad status", call(base + "/api/p/counter/annotate", "alice-token-1", {**ann, "status": "x"})[0] == 400)
    code, text = call(base + "/api/p/counter/annotate", "alice-token-1", ann)
    expect("annotate", code == 200 and json.loads(text)["annotation"]["by"] == "alice", text)
    code, text = call(base + "/p/counter/")
    expect("annotation shown", "known, carol, CNT-77" in text and "alice, 20" in text, text)
    with open(os.path.join(out, "root", "counter", "audit.jsonl")) as f:
        audit = [json.loads(x) for x in f]
    expect("audit log", [a["action"] for a in audit] == ["push", "push", "push", "annotate"] and audit[-1]["user"] == "alice", str(audit))

    # The next report takes the dashboard's annotations as its triage rules.
    rc, text = run("report", vplan, "-results", results, "-o", plan_d, "-label", "s3", "-rules", base + "/api/p/counter/rules", "-fail-on-new")
    expect("report with the server's rules", "known" in text and "owner: carol; bug: CNT-77" in text and "new failure bucket" not in text, text)

    # Several clients at once: three writers push 4 runs each into one project
    # while others annotate and read it. Every push and annotation must land,
    # and the files stay whole.
    writers = [("alice", "alice-token-1"), ("dave", "dave-token-333"), ("erin", "erin-token-4444")]
    dirs = []
    for w, _ in writers:
        for i in range(4):
            d = os.path.join(out, f"multi_{w}_{i}")
            run("report", vplan, "-results", results, "-o", d, "-label", f"{w}-{i}", "-time", f"2026-11-0{i + 1}T00:00:00Z")
            dirs.append((w, d))
    run("push", base, dirs[0][1], "-token", "alice-token-1", "-project", "busy")  # the project exists before the storm
    errs = []

    def pusher(w, tok):
        for i, (who, d) in enumerate(x for x in dirs if x[0] == w):
            if (who, i) == ("alice", 0):
                continue
            rc, text = run("push", base, d, "-token", tok, "-project", "busy")
            if rc != 0:
                errs.append(f"push {d}: {text}")

    def annotator(w, tok):
        for i in range(10):
            code, text = call(base + "/api/p/busy/annotate", tok, {"signature": f"sig {w} {i}", "status": "known", "owner": w})
            if code != 200:
                errs.append(f"annotate {w} {i}: {code} {text}")

    def reader():
        for _ in range(20):
            for u in ("/", "/p/busy/", "/api/p/busy", "/api/p/busy/history", "/p/busy/plan.json"):
                code, text = call(base + u)
                if code != 200:
                    errs.append(f"read {u}: {code}")
                elif u.endswith(".json") or "/api/" in u:
                    try:
                        json.loads(text)
                    except ValueError:
                        errs.append(f"read {u}: torn JSON")

    ts = [threading.Thread(target=pusher, args=w) for w in writers]
    ts += [threading.Thread(target=annotator, args=w) for w in writers]
    ts += [threading.Thread(target=reader) for _ in range(3)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    expect("concurrent clients", not errs, "\n".join(errs[:10]))
    code, text = call(base + "/api/p/busy/history")
    labels = sorted(s["label"] for s in json.loads(text))
    expect("every concurrent push kept", labels == sorted(f"{w}-{i}" for w, _ in writers for i in range(4)), text[:2000])
    proj = os.path.join(out, "root", "busy")
    with open(os.path.join(proj, "audit.jsonl")) as f:
        audit = [json.loads(x) for x in f]
    expect("audit of concurrent clients", sum(a["action"] == "push" for a in audit) == 12 and sum(a["action"] == "annotate" for a in audit) == 30
           and {a["user"] for a in audit} == {"alice", "dave", "erin"}, str(audit)[:2000])
    with open(os.path.join(proj, "annotations.json")) as f:
        anns = json.load(f)
    expect("every concurrent annotation kept", sorted(anns) == sorted(f"sig {w} {i}" for w, _ in writers for i in range(10))
           and all(v["by"] == v["owner"] for v in anns.values()), str(anns)[:2000])
    expect("no temporary files left", not [n for n in os.listdir(proj) if ".tmp" in n], str(os.listdir(proj)))

    # -private: nothing without a token.
finally:
    srv.terminate()
    srv.wait(10)

srv = subprocess.Popen([sys.executable, mplan, "serve", "-root", os.path.join(out, "root"), "-port", "0", "-users", users, "-private", "-q"],
                       stdout=subprocess.PIPE, text=True)
try:
    base = re.search(r"on (http://\S+?)/ ", srv.stdout.readline()).group(1)
    expect("private api", call(base + "/api/projects")[0] == 401)
    expect("private api with a token", call(base + "/api/projects", "bob-token-22")[0] == 200)
finally:
    srv.terminate()
    srv.wait(10)
print("serve_test: ok")
