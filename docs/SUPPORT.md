# msupport: an AI support agent for each product

Every minisim product has its own support agent: a Claude model that answers
users' questions about that one product from that product's documentation,
can run a small example on the product's own tool, and files a ticket for a
person when the documentation does not answer. `msupport` is the program
that runs them.

```sh
pip install anthropic                       # the only dependency; needs ANTHROPIC_API_KEY
msupport -list                              # the products and what each agent may run
msupport -product msim "How do I write an FST waveform?"
msupport -product mlint -chat               # a conversation in the terminal
msupport -product msim -search "maxtime"    # what the documentation search returns; no model
msupport -product msim -show-prompt         # the agent's instructions; no model
msupport -serve 8080 -tickets tickets.jsonl # web page and JSON API on 127.0.0.1
```

Run it from a checkout, as `python3 tools/msupport.py`; it is written
`msupport` on this page and is not in the release package yet.

**Status: built and tested, not deployed.** Nothing here is reachable from
the internet. The agents go online when the products they answer for are
complete; "Before going online" below is the list of what that takes.

## How an agent is put together

```
user ── web page / JSON API ── Service (sessions, rate limit, customer check)
                                  │
                                Desk (one per product)
                                  │  instructions + list of documentation sections
                                  ▼
                               Claude  ⇄  search_docs   BM25 over the product's manuals
                                       ⇄  read_doc      one section in full
                                       ⇄  run_example   the product's tool, in a sandbox
                                       ⇄  escalate      a ticket for a support engineer
```

- **One agent per product, not one for everything.** An agent sees only its
  product's manuals, so it cannot answer an `msim` question with an option
  that belongs to another tool, and a product can be added, changed or
  withdrawn without touching the others. A question about another product
  is passed to that product's agent.
- **Answers come from the documentation.** The model is told to search before
  it answers, to name the section it used (`README.md#Compiled mode`), and to
  say "that is not documented" instead of guessing. The "Limits / Not yet"
  section of each manual is in the index, so "is X supported" gets the honest
  answer.
- **Retrieval is a tool, and it is small.** Each manual is cut at its
  headings, and the sections are ranked with BM25 in memory. A product has
  tens to a few hundred sections, so there is no embedding model, no vector
  database and nothing to keep in sync: the index is rebuilt from the
  markdown at start-up, and a manual that changes is live at the next
  restart. The model sees the list of section names in its instructions and
  asks for what it needs.
- **The model.** `claude-opus-5-5` at medium effort by default; `model` and
  `effort` in a product file change it per product. The instructions and the
  section list are the same on every turn and are sent with prompt caching.
  A request the model declines is retried on a fallback model by the API.

## A product file

One JSON file per product in `support/products/`:

```json
{
  "name": "msim",
  "title": "msim SystemVerilog simulator",
  "edition": "free",
  "access": "public",
  "summary": "Compiles, elaborates and simulates ...",
  "docs": ["README.md#Simulating your own design", "docs/UVM.md", "CHANGELOG.md"],
  "related": ["mwave", "mcov"],
  "run": {"binary": "msim", "source_ext": ".sv", "fixed": ["-threads", "2"],
          "flags": ["-sv", "-xprop"], "valued": ["-top", "-maxtime"], "plusargs": true,
          "cpu_seconds": 10, "wall_seconds": 20, "memory_mb": 2048}
}
```

| Key | Meaning |
|---|---|
| `docs` | Markdown files under the documentation root. `file#Heading` takes only the sections whose title starts with `Heading`. A file or section that no longer exists is an error at start-up, and the test fails, so a renamed heading cannot silently empty an agent |
| `access` | `public`: anyone may ask. `customer`: the request must carry a licence that covers the product (see below) |
| `run` | Leave it out and the agent has nothing to run. `flags` and `valued` are the only options the agent may pass |
| `model`, `effort` | Per-product model settings |
| `docs_root` | Where `docs` are read from; default is this repository for the files here, and the product file's own directory for files from elsewhere |

The nine free products each have a file: `msim`, `mwave`, `mcov`, `mregress`,
`mplan`, `mbug`, `mlint`, `mdft`, `mdebug`. `msim` and `mlint` run examples.

### Products that are not in this repository

`msupport -products DIR` (or `$MINISIM_SUPPORT_PRODUCTS`, a path list) adds
product files from other directories. A commercial product ships its product
file and its manuals in its own distribution, marks itself
`"access": "customer"`, and is served by the same program; nothing about it
is stored here.

A customer proves the entitlement with the licence file they already have
for the tools: a short signed text file that names the licensee, the tools it
covers and an expiry. `msupport -serve -license-key PUB.pem` is given the
public key those licences are signed with (the same one the commercial tools
are built against, so there is one secret, the private key, and it is not on
the support server). A request for a customer product carries the licence
text in its `license` field; the service checks the Ed25519 signature with
`openssl`, the expiry and that the licence lists the product (`licensed_as`
in the product file, default its name; `tools: all` covers every product),
on every request. Without `-license-key`, customer products are closed.

## Running examples safely

A simulator executes what it is given: `$system`, DPI-C, file I/O and
`-compile` all reach the host. So `run_example` is narrow by construction:

1. **One source text, no other files.** The agent passes the source as a
   string; it is written to an empty temporary directory that is deleted
   afterwards.
2. **Options from a list.** Only the product file's `flags` and `valued`
   options and `+plusargs` (never `+incdir`) are passed. `-compile`, `-f`,
   `-sv_lib`, C files, include paths and output paths are not on any list.
3. **Sources that reach the host are refused** before anything runs:
   `$system`, DPI, `$fopen` and the other file tasks, `$readmem*`,
   `` `include ``, token pasting. This is a second line of defence, not the
   boundary: a text filter can be talked around.
4. **Operating-system limits are the boundary.** CPU seconds, address space,
   file size and wall-clock time are capped, the environment is emptied (no
   API key reaches the child), the process group is killed on time-out, and
   the tool runs under `unshare -rn` (new user and network namespace: no
   network) when the host allows it.
5. **Off by default online.** `msupport -serve` does not offer `run_example`
   unless `-allow-run` is given, and then refuses to run anything when the
   namespace isolation is not available.

`unshare` does not hide the host's files from a tool that is tricked into
reading them. The service is meant to run inside a container that holds
nothing worth reading: no credentials on disk, a read-only root, and the API
key only in the service process's environment.

## The web service

`msupport -serve PORT` is a small HTTP server (standard library only):

| Request | Answer |
|---|---|
| `GET /` | A one-page chat with a product picker |
| `GET /products` | The products: name, title, edition, access |
| `POST /chat` `{"product", "message", "session", "license"}` | `{"session", "answer", "usage"}`; `license` only for a customer product |

It keeps each conversation in memory by session id (the questions and the
answers as text), limits questions per address per minute (`-per-minute`,
default 6), caps the size of a question and the length of a conversation,
and never sends a traceback to a user. `-tickets FILE` appends each
escalation as a JSON line: id, time, product, category, summary.

## Tests

`tests/msupport_test.py` runs without a network or an API key: every product
file loads and indexes, searches return the right section, the option and
source filters refuse what they should, a real `msim` runs an example and a
runaway loop is stopped, the service enforces its limits and checks customers' licences
(signed in the test with a throwaway key), and the whole agent loop runs through the Anthropic SDK against a
fake Messages API (skipped when the `anthropic` package is not installed).

## Before going online

- **Answer quality has not been measured.** The loop is tested against a
  fake API; no real model has answered a question through it yet. It needs
  an API key and a set of real questions per product with expected answers,
  run before launch and after every manual change.
- **Hosting.** A container image, TLS, a real rate limiter and session store
  (the ones here are in-memory and per-process), and logs.
- **Privacy.** Users paste source code and logs. Decide what is stored and
  for how long, publish it, and tell users not to paste confidential code
  into a public desk.
- **Licence handling online.** The licence text travels in each request;
  the page keeps it in the browser only. Decide whether an id plus a server
  copy is wanted instead, and how a leaked licence file is revoked.
- **Tickets.** `escalate` writes a file; connect it to the issue tracker
  (`mbug` already speaks GitHub Issues, Jira and Redmine).
- **Streaming.** Answers arrive whole; a long answer should stream.
- **Manuals for the smaller products.** `mwave` and `mdft` checks have only
  README sections; their agents are as good as those sections.
