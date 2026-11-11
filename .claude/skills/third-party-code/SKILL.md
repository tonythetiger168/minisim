---
name: third-party-code
description: License rules for using outside code in minisim - what may be linked or vendored, what may only run as an external program, and how to record it in THIRD_PARTY_NOTICES.md. Use before adding a dependency, copying code, FetchContent-ing a project, or shipping a file from another project.
---

# Third-party code

Reuse open source as much as possible, within these limits.

## What is allowed

| License | Link or vendor into minisim | Run as an external program |
|---|---|---|
| MIT, BSD (2/3-clause), Apache-2.0, ISC, zlib, Boost | yes | yes |
| Anything else permissive (for example ECL-2.0) | ask the repository owner first, per component | yes |
| GPL, LGPL, AGPL (Icarus, OpenSTA, KLayout, Yosys-GPL parts, ...) | **no** | yes: as a baseline or cross-check that tests skip when it is missing |
| No license, or "research only" | no | ask first |

"External program" means minisim starts it as a separate process or CI
installs it for a comparison. No GPL source, header, table or generated
file is copied in, and nothing links against it. Reading GPL code to port
it line by line is also out; work from the published paper or format spec.

Data counts too: cell libraries, PDK files, test designs and benchmark
netlists each have a license. Record them the same way.

## Recording it

Every vendored or fetched component gets an entry in
`THIRD_PARTY_NOTICES.md` at the repository root: name, version or commit,
URL, license name, where it lives in the tree, and the license text (or the
path of the copied license file).

- **Append, never overwrite.** Several branches add this file. When it
  already exists, add your entry at the end; on a merge conflict keep every
  entry from both sides.
- Keep the upstream license file next to vendored sources.
- Apache-2.0 components: keep their `NOTICE` content as well.
- Pin fetched dependencies to a tag or commit.

## minisim's own license

See `LICENSE` and `docs/PRODUCT.md` for which parts are in the free
open-source tier. Code placed in the open tier must not depend on anything
outside it, and nothing here may name or describe private repositories.
