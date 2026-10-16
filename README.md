<div align="center">

# 🧠 memix

**A Nix-inspired, provenance-aware build system for agent context**

Treat memory as immutable evidence plus reproducible views — not a mutable notes directory.

[![Ubuntu](https://github.com/MrZLeo/memix/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/ubuntu.yml)
[![MacOS](https://github.com/MrZLeo/memix/actions/workflows/macos.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/macos.yml)
[![Windows](https://github.com/MrZLeo/memix/actions/workflows/windows.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/MrZLeo/memix?include_prereleases&color=blue)](https://github.com/MrZLeo/memix/releases)
[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](LICENSE)
[![C++26](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=c%2B%2B&logoColor=white)](https://en.cppreference.com/w/cpp/26)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)](#-self-contained-binaries)

[🚀 Build](#-build) •
[🤖 Agent Install](#-install-with-an-agent) •
[▶️ Usage](#️-run) •
[📦 Releases](https://github.com/MrZLeo/memix/releases) •
[🤝 Contributing](CONTRIBUTING.md)

[![Stars](https://img.shields.io/github/stars/MrZLeo/memix?style=social)](https://github.com/MrZLeo/memix/stargazers)

</div>

---

## 💡 Mental model

Treat memory as immutable evidence plus reproducible views, not as a mutable notes directory. Agents should consume an explicitly selected, versioned, provenance-carrying view rather than an untracked collection of files.

* 📌 **Immutable blobs** — Stored blobs are immutable and content-addressed. Changed content becomes a new object instead of overwriting history.
* 🌳 **Versioned trees** — Logical trees, generations, and refs describe how memory evolves while allowing old states to coexist and remain replayable.
* 🔁 **Composable views** — Views are first-class derived artifacts, not new sources of truth. They can reference and mount existing view environments while preserving transitive provenance.
* 🧾 **Provenance built-in** — A view retains its source generations, blob identities, derivation, artifacts, and environment closure.
* 🎯 **Explicit selection** — Memix does not invent hidden dependencies or silently activate additional memory.
* 🔗 **Profile-style activation** — Built views attach to a workspace through numbered profile generations and one switchable `current` pointer, the only mutable layer in the system.
* 📂 **Honest packaging** — A `file_tree` view packages selected files at stable paths; it does not merge Markdown, resolve directive precedence, summarize content, or settle semantic conflicts.

> [!NOTE]
> The current CLI implements immutable storage, versioned logical trees, reproducible `file_tree` views, and profile-style activation with generations and rollback. Directive resolution and context-pack generation are separate future layers.

## 🗂️ Project layout

`memix` is implemented as a CMake-based C++ project with a Rust-style split between library code and executable entrypoints:

* `include/` and `src/` contain the `memix` library
* `src/bin/*.cpp` are auto-discovered and built as executables

The CLIs use [argum](https://github.com/gershnik/argum) for argument parsing.

## 📦 Dependencies

The project uses [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) to fetch:

| Dependency | Version | Purpose |
| ---------- | ------- | ------- |
| [argum](https://github.com/gershnik/argum/releases/tag/v2.8) | 2.8 | CLI parsing |
| [simdjson](https://github.com/simdjson/simdjson/releases/tag/v5.0.3) | 5.0.3 | Metadata JSON parsing and writing |
| [spdlog](https://github.com/gabime/spdlog/releases/tag/v1.17.0) | 1.17.0 | Executable logging |
| [Catch2](https://github.com/catchorg/Catch2/releases/tag/v3.11.0) | 3.11.0 | Tests |

## 🚀 Build

Building from source requires **CMake 4.4 or newer**, **Ninja**, and a compiler
with C++26 mode and the standard library features used by memix. The project
requires C++26 without compiler extensions; it does not fall back to an older
standard. This applies to the library, CLIs, tests, and the optional
Objective-C++ Inspector bridge. Individual C++26 features still depend on the
compiler and standard library implementation.

Use `nix develop` for the pinned toolchain described below. CI installs current
CMake and Ninja releases explicitly rather than relying on runner defaults.

```bash
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake --build build
```

The default build always produces the `memix` library and also builds every `src/bin/*.cpp` entrypoint as an executable.

### Nix development environment

The flake supports `x86_64-linux`, `aarch64-linux`, and `aarch64-darwin`
(Apple Silicon). `nix develop` uses GCC on Linux and Clang on macOS, through
nixpkgs' official `stdenv`. It provides the CMake 4.4+ / C++26 toolchain, plus
Ninja, Git, pkg-config, ccache, and clang-tools (clang-tidy, clang-format,
clangd). On Linux, `glibc.static` is available through a `LIBRARY_PATH`
fallback for static linking, without overriding shared libc during ordinary
compiler checks and dynamic links.

With [Nix](https://nixos.org/download/) installed and `nix-command` / `flakes`
enabled, build in a separate directory from any host-toolchain build:

```bash
nix develop
cmake -G Ninja -S . -B build-nix \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCPM_SOURCE_CACHE="$HOME/.cache/CPM"
cmake --build build-nix --parallel
ctest --test-dir build-nix --output-on-failure
```

Add `-Dmemix_ENABLE_CLANG_TIDY=ON` to enable project analysis. The current
locked Linux clang-tidy/libstdc++ pair has an upstream C++26 `<format>`
compatibility issue, so leave this option off for Linux compiler/runtime
validation until it is fixed. That is not a passing lint result; see
[current toolchain limitations](CONTRIBUTING.md#current-toolchain-limitations).

Core tools use the latest packaged versions in the locked `nixpkgs-unstable`
revision. To update versions, run `nix flake update nixpkgs` and commit the
lock-file change. Validate with `nix flake check --all-systems --no-build`;
format the flake with `nix fmt`.

Ubuntu CI and Linux release builds use this same locked shell through
`nix develop --no-update-lock-file`. Their compiler, CMake, Ninja, and coverage
`gcov` come from the flake rather than the runner's preinstalled tools.
Windows CI uses native `clang-cl` and `lld-link` with the MSVC standard library
and static runtime; it does not use Nix.

These are native C++ environments, not cross-compilation toolchains; Intel
macOS and Windows are not included. CPM downloads project dependencies on the
first configure. The optional SwiftUI Inspector still requires Xcode/Swift.

### Docker development environment

The Docker image uses the same `flake.nix` and `flake.lock`, not a separate
apt/pip toolchain. Docker BuildKit selects the matching rolling
`nixpkgs/nix-flakes:latest-x86_64-linux` or `latest-aarch64-linux` base for
amd64 or arm64. These architecture-qualified tags are published independently;
the Dockerfile does not use the old, unsuffixed `latest` manifest. The bases
already enable flakes. The matching Linux dev shell supplies GCC and static
libc. The image build preloads that shell, and every container command runs through
`nix develop --no-update-lock-file --command`. Tool versions come from the
lock file; required lock-file changes fail rather than being made silently.

Build the image and open a development shell:

```bash
docker build --pull -t memix-dev .
docker run --rm -it \
  --mount type=bind,source="$PWD",target=/workspace \
  memix-dev
```

Or build and test directly with the default compiler-only configuration:

```bash
docker run --rm \
  --mount type=bind,source="$PWD",target=/workspace \
  memix-dev bash -c '
    set -e
    cmake -G Ninja -S . -B build-container \
      -DCMAKE_BUILD_TYPE=Debug -Dmemix_ENABLE_CLANG_TIDY=OFF
    cmake --build build-container --parallel
    mkdir -p build-container/Testing
    chown -R 1000:1000 build-container/Testing
    chroot --userspec=1000:1000 / \
      "$(command -v env)" HOME=/tmp TMPDIR=/tmp \
      "$(command -v ctest)" --test-dir /workspace/build-container \
      --output-on-failure
  '
```

Use a separate `build-container` directory: this is a Linux build, even on a
macOS host. The initial image build downloads Linux Nix packages and can take
time; unchanged base images and flake metadata reuse the cached image layer.
CPM uses its default per-build dependency storage, which persists with the
build directory in the source bind mount; no separate cache volume or cache
variable is required. Builds run as root; the test command drops to UID/GID
1000 because permission-sensitive tests must not run as root. Only the test log directory
changes ownership. Linux bind mounts may therefore contain root- or
UID-1000-owned build files. The image contains only the tools declared in the
flake; add optional development tools there rather than maintaining another
Docker package list.

#### Apple container on macOS

On Apple Silicon, the same Dockerfile also works with Apple's
[`container`](https://github.com/apple/container) CLI:

```bash
container system start --enable-kernel-install
container build --platform linux/arm64 \
  --build-arg TARGETARCH=arm64 --pull -t memix-dev .
container run --rm -it --cpus 6 --memory 16G \
  --mount type=bind,source="$PWD",target=/workspace \
  memix-dev
```

Inside the shell, use the CMake/build/test commands from the Docker example.
Adjust the CPU and memory limits for your machine. With `container` 1.5.0,
pass `TARGETARCH=arm64` explicitly: `--platform` alone does not populate that
build argument. This is a native Linux arm64 build, not a macOS build. The
bind-mounted build directory survives removal of the container.

### CMake package consumers

After `cmake --install build --prefix /your/prefix`, consumers can use:

```cmake
find_package(memix 0.6 CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE memix::memix)
```

Pass the prefix in `CMAKE_PREFIX_PATH`. The exported target supplies public
headers and the C++26 requirement, but not memix's warning or exception flags.
Installations using the default relative install directories can be moved to a
new prefix. Package matching follows CMake's `SemanticVersion` policy: 0.x
releases require the same minor version (`0.6.0` does not satisfy `0.5.x`);
from 1.x onward, compatibility is within the same major version. In either
case, the installed version must be at least the requested version.

## 💿 Self-contained binaries

`memix_STATIC_LINK=ON` is the default. Each generated CLI is self-contained
with respect to memix and its third-party dependencies:

* 🐧 **Linux** — executables are fully static and have no ELF interpreter or shared library dependency.
* 🪟 **Windows** — executables use the static MSVC runtime (`/MT`).
* 🍎 **macOS** — executables statically include memix and its third-party dependencies. Apple does not support fully static user executables, so `libc++` and `libSystem` remain system-provided dynamic libraries.

The static-link setting also applies to Linux/MinGW test executables; tests
retain exception support. To restore platform-default dynamic runtime
linking, configure with `-Dmemix_STATIC_LINK=OFF`.

Tag builds publish the standalone CLIs and Codex skill as GitHub release
assets:

| Platform | Assets |
| -------- | ------ |
| Linux x86_64 | `memix-store-linux-x86_64`, `memix-view-linux-x86_64`, `memix-tui-linux-x86_64` |
| macOS arm64 | `memix-store-macos-arm64`, `memix-view-macos-arm64`, `memix-tui-macos-arm64`, `MemixInspector-macos-arm64.zip` |
| Windows x86_64 | `memix-store-windows-x86_64.exe`, `memix-view-windows-x86_64.exe`, `memix-tui-windows-x86_64.exe` |
| Any | `memix-cli-skill.tar.gz` (platform-independent) |

On Linux or macOS, mark a downloaded asset executable before first use:

```bash
chmod +x memix-store-linux-x86_64
./memix-store-linux-x86_64 --version
```

## 🤖 Install with an agent

Copy this request into an agent:

```text
Install memix by reading and following https://raw.githubusercontent.com/MrZLeo/memix/master/AGENT_INSTALL.md. If your harness supports Agent Skills packages (for example Codex, pi, or Claude Code), install the complete version-matched memix CLI skill package before using either command.
```

The full bootstrap procedure is also available in
[AGENT_INSTALL.md](AGENT_INSTALL.md).

## ▶️ Run

```bash
./build/bin/Release/memix-store --store-root /tmp/memix-store ingest --type rules --dest README.md README.md
./build/bin/Release/memix-store --store-root /tmp/memix-store ingest --type documents --dest notes/README.md README.md
./build/bin/Release/memix-store --store-root /tmp/memix-store rules tree
./build/bin/Release/memix-store --store-root /tmp/memix-store tree --all --depth 2 --json
./build/bin/Release/memix-store --store-root /tmp/memix-store list
./build/bin/Release/memix-store --store-root /tmp/memix-store list --json
./build/bin/Release/memix-store --store-root /tmp/memix-store expose
./build/bin/Release/memix-store --version
./build/bin/Release/memix-store --help
```

Build a reproducible `file_tree` view and attach it to the current directory
as `./.memory`, Nix-profile style. Activation records a numbered generation
in a per-workspace store profile and switches its `current` pointer;
read memory with ordinary file tools, roll back with `rollback`, and inspect
the attachment with `status`:

```bash
QUEST=$(./build/bin/Release/memix-view --store-root /tmp/memix-store define --source rules/README.md --intent "repository documentation" --name "README context" | head -n1 | awk '{print $2}')
./build/bin/Release/memix-view --store-root /tmp/memix-store quests
./build/bin/Release/memix-view --store-root /tmp/memix-store find repository --covers rules/README.md
ENV=$(./build/bin/Release/memix-view --store-root /tmp/memix-store resolve "$QUEST" | head -n1 | awk '{print $2}')
./build/bin/Release/memix-view --store-root /tmp/memix-store freshness "$QUEST"
./build/bin/Release/memix-view --store-root /tmp/memix-store stats "$QUEST" --json
./build/bin/Release/memix-view --store-root /tmp/memix-store list --json
./build/bin/Release/memix-view --store-root /tmp/memix-store tree "$ENV" --depth 2
./build/bin/Release/memix-view --store-root /tmp/memix-store cat "$ENV" rules/README.md
./build/bin/Release/memix-view --store-root /tmp/memix-store build --view "$ENV=prior" --source documents/notes/README.md --intent "README with prior context" --name "composed README"
./build/bin/Release/memix-view --store-root /tmp/memix-store activate "$ENV" --into ./.memory
cat ./.memory/README.md
cat ./.memory/rules/README.md
./build/bin/Release/memix-view --store-root /tmp/memix-store generations --into ./.memory
./build/bin/Release/memix-view --store-root /tmp/memix-store status --into ./.memory
./build/bin/Release/memix-view --store-root /tmp/memix-store deactivate --into ./.memory
```

Every `build` persists its canonical query as a quest and records the resulting
derivation/environment resolution. `define` creates or annotates that quest
without building; `find` searches intent/name text or uses `--covers` for
structural path lookup; `quests` lists the public quest catalog; `freshness`
compares the latest resolution with the refs it followed; and `resolve`
re-runs a quest against current refs. `build --intent TEXT --name TEXT`
upserts the same annotations while building. The older `list` command remains
available for low-level environment debugging.

`activate` includes a read-only `README.md` at the attachment root in both
link and copy modes. It records the activated environment and derivation IDs,
plus the corresponding quest's ID and frozen name and intent. The complete
query definition is omitted from the README; it remains in the store with its
existing version bindings.
The runtime quest instructions are frozen in the derivation at build time and
participate in its identity. Changing the quest's name, intent, or query and
then rebuilding produces a new environment. Updating the quest catalog alone
does not change an existing environment: repeated activation, activation in
another workspace, and rollback reproduce the same README from its build
snapshot, even if the quest has since been deleted.

Legacy derivations without an instruction snapshot use a fixed missing-snapshot
notice instead of current quest annotations. Older generation links and cached
READMEs are upgraded on activation or rollback. The original environment tree
is unchanged. An environment with a root `README.md` entry cannot be activated
because that path is reserved for the generated README.

The plain `quests` and `find` tables shorten long intent/name cells with `...`
so columns stay separate; their `--json` forms always retain the complete
strings. Freshness uses the derivation manifest's `input_resolutions`: one
record per context input before file deduplication, including optional inputs
that resolved no files. Composed `--view` inputs are omitted because their
resolved environment IDs are content-pinned; therefore a quest containing only
view inputs reports an empty input list and `stale: false`. A deleted followed
ref is reported with `pinned: false`, `current_generation: null`, and
`fresh: false`. Legacy manifests without `input_resolutions` fall back to
matching locators in `resolved_inputs`; any context locator absent there is
conservatively reported `fresh: false` rather than silently skipped.

Successful `memix-view cat` and `tree` calls, including `tree --json`, and
every successful `activate` append best-effort consumption receipts to the
quest's `receipts.jsonl`. Activation is recorded even for a no-op
re-activation, and rollback records the environment it recommits. Once
attached, ordinary reads through `./.memory` bypass the CLI; activation is the
observable proxy for that otherwise invisible use and the strongest admission
signal. Receipt bookkeeping never makes the primary operation fail, legacy
environments without a quest are silently skipped, and `deactivate` records
nothing. `stats` presents activation totals and per-profile counts before read
totals, cat/tree counts, and per-environment reuse. `quests` and `find` show
`READS` and `ACTS`; their JSON also includes read and activation recency.
This remains evidence for future admission policy, not a promotion or eviction
policy itself. `show` is provenance debugging and does not count as
consumption.

`memix-store` defaults to `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`,
then `$HOME/.memix`. The supported immutable object types are `rules`, `chat`,
and `documents`. Stored bytes live as immutable SHA-256 blobs under each
type's `blobs/` directory; when ingest uses `--dest`, memix also records the
blob in that type's canonical context tree and advances the type's latest
generation ref. `memix-store rm --type <type> <path>` retracts a logical file
or directory subtree by committing a new generation whose tree no longer
contains the path. Removal is logical and history-preserving: blobs and prior
generations remain immutable and readable, so pinned generation locators keep
resolving.

### Discovery surface

`<store-root>/latest/` is a derived, read-only hardlink projection of every
type's latest logical tree. It contains real directories and logical-path
filenames, so ordinary tools work without special symlink flags:

```bash
rg -i "search terms" /tmp/memix-store/latest
find /tmp/memix-store/latest -type f
grep -R -i "search terms" /tmp/memix-store/latest
```

An ingest with `--dest` refreshes the affected type after committing its new
generation, and `rm` refreshes it the same way after a removal. Blob-only
ingests have no logical path and do not appear. The
projection contains no source-of-truth state and can be deleted or repaired
with `memix-store --store-root /tmp/memix-store expose`; a refresh failure
warns without rolling back the committed ingest. Searching this surface is
untracked exploration. Turn useful paths into quests with `find --covers`,
`define`, or `build` so selection and subsequent consumption remain tracked.

<details>
<summary><b>🔍 Manual smoke check</b> (against the real default store root)</summary>

```bash
./build/bin/Release/memix-store ingest --type rules --dest README.md README.md
./build/bin/Release/memix-store rules tree
./build/bin/Release/memix-store list --type rules
```

All object types can ingest plain blobs. Passing `--dest` stores the blob at a
logical path inside that type's canonical context tree and advances its latest
generation ref. Commands under `rules` operate on the rules logical tree. Refs
such as `refs/rules/latest`, `refs/documents/latest`, and
`refs/rules/generations/000001` are small mutable files containing immutable
generation ids; each generation then points at its immutable root tree.

That writes outside the repository, so use `--store-root` when you want an
isolated test store.

</details>

## ✅ Tests

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake --build build
ctest --test-dir build -C Release -VV
```

## 🍺 Homebrew

The repository includes a Homebrew formula under `Formula/memix.rb`. It is
intended for a `MrZLeo/homebrew-memix` tap and declares the Homebrew dependencies
needed to build and run the CLI. See `Formula/README.md` for publishing steps.

## ⚙️ Common Options

| Option | Default | Description |
| ------ | ------- | ----------- |
| `memix_BUILD_EXECUTABLE` | `ON` | Build CLI executables |
| `memix_STATIC_LINK` | `ON` | Self-contained static binaries |
| `memix_ENABLE_UNIT_TESTING` | `ON` | Build unit tests |
| `memix_ENABLE_CLANG_TIDY` | `OFF` | Analyze library and CLI sources with clang-tidy |
| `memix_ENABLE_CPPCHECK` | `OFF` | Analyze library and CLI sources with cppcheck |
| `memix_ENABLE_CCACHE` | `ON` | Use ccache unless a compiler launcher is explicitly set |
| `memix_WARNINGS_AS_ERRORS` | `OFF` | Treat project compiler warnings as errors |
| `memix_ENABLE_CODE_COVERAGE` | `OFF` | Enable code coverage |
| `memix_ENABLE_DOXYGEN` | `OFF` | Generate Doxygen docs |
| `memix_ENABLE_LTO` | `OFF` | Enable link-time optimization |
| `memix_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer |

For a shared library, configure with `-Dmemix_STATIC_LINK=OFF -DBUILD_SHARED_LIBS=ON`.
With `memix_BUILD_EXECUTABLE=OFF`, library tests still build, but the `store_test`
and `quest_test` targets are explicitly skipped because they also exercise the
CLIs. Set `memix_ENABLE_UNIT_TESTING=OFF` for a library-only build without tests.
The old header-only mode is unsupported; the template's `USE_ALT_NAMES` and
`VERBOSE_OUTPUT` options have been removed. Use CMake's `--log-level=VERBOSE`
(or `CMAKE_MESSAGE_LOG_LEVEL=VERBOSE`) for verbose configuration output.
