# Releasing minisim

## Version numbers

minisim uses [Semantic Versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.
The one place the number lives is `project(minisim VERSION x.y.z)` in
`CMakeLists.txt`; `msim -version` prints it with the git commit.

- **0.x** (now): the language subset grows every minor release, and command
  line options may still change between minors.
- **MINOR**: new language support or features (one or more spec rounds).
- **PATCH**: bug fixes only, made on a `release/x.y` branch.
- **1.0**: the IEEE 1800-2023 clause suite passes, UVM runs, and the command
  line is stable. After 1.0, removing or renaming an option needs a major.

## Branches

- `main` is always releasable: CI is green on every merge.
- `release/x.y` is cut from `main` when `x.y.0` is tagged, and only takes fixes
  (cherry-picked from `main`) for `x.y.1`, `x.y.2`, ...

## Cutting a release

1. On `main`, set `project(minisim VERSION x.y.z)` in `CMakeLists.txt`.
2. In `CHANGELOG.md`, rename `## [x.y.z] - unreleased` to
   `## [x.y.z] - YYYY-MM-DD`, and leave an empty `## [Unreleased]` above it.
3. Merge that as a PR, then tag the merge commit and push the tag:
   ```sh
   git tag -a vx.y.z -m "minisim x.y.z"
   git push origin vx.y.z
   ```
4. The **Release** workflow (`.github/workflows/release.yml`) checks that the
   tag matches the CMake version, builds with slang, runs the tests, builds
   `minisim-x.y.z-Linux.tar.gz` and `minisim_x.y.z_amd64.deb`, installs the
   `.deb` and runs `msim -compile` on an example, then publishes a GitHub
   release whose notes are that version's CHANGELOG section.
5. For a minor or major release, create `release/x.y` from the tag.

To try the packaging without publishing, run the Release workflow by hand
(Actions > Release > Run workflow): the packages are kept as workflow
artifacts.

## Installing a package

```sh
sudo apt install ./minisim_x.y.z_amd64.deb      # Debian / Ubuntu
# or
tar xzf minisim-x.y.z-Linux.tar.gz && export PATH=$PWD/minisim-x.y.z-Linux/bin:$PATH
msim -version
```

`msim -compile` needs a C++20 compiler (g++ 11 or newer, or clang 14 or
newer) on the machine; the interpreter needs nothing else. The kernel sources
it compiles against are installed in `share/minisim/include`
(`MINISIM_INCLUDE` overrides the directory).

From source: `cmake -B build -DMINISIM_WITH_SLANG=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local && cmake --build build -j && cmake --install build`.

## Platforms

Linux x86_64 is built and tested on every release. Windows users can run the
Linux package under WSL 2. A native Windows build needs `msim -compile` to stop
using `/bin/sh` and `sys/wait.h`, which is on the roadmap (`docs/PRODUCT.md`).
