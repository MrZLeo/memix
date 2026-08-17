<div align="center">

# 🧠 memix

**A Nix-inspired, provenance-aware build system for agent context**

Treat memory as immutable evidence plus reproducible views — not a mutable notes directory.

[![Ubuntu](https://github.com/MrZLeo/memix/actions/workflows/ubuntu.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/ubuntu.yml)
[![MacOS](https://github.com/MrZLeo/memix/actions/workflows/macos.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/macos.yml)
[![Windows](https://github.com/MrZLeo/memix/actions/workflows/windows.yml/badge.svg)](https://github.com/MrZLeo/memix/actions/workflows/windows.yml)
[![Release](https://img.shields.io/github/v/release/MrZLeo/memix?include_prereleases&color=blue)](https://github.com/MrZLeo/memix/releases)
[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=c%2B%2B&logoColor=white)](https://en.cppreference.com/w/cpp/23)
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
| [simdjson](https://github.com/simdjson/simdjson/releases/tag/v4.6.2) | 4.6.2 | Metadata JSON parsing and writing |
| [spdlog](https://github.com/gabime/spdlog/releases/tag/v1.17.0) | 1.17.0 | Executable logging |
| [Catch2](https://github.com/catchorg/Catch2/releases/tag/v3.11.0) | 3.11.0 | Tests |

## 🚀 Build

```bash
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake --build build
```

The default build always produces the `memix` library and also builds every `src/bin/*.cpp` entrypoint as an executable.

## 💿 Self-contained binaries

`memix_STATIC_LINK=ON` is the default. Each generated CLI is self-contained
with respect to memix and its third-party dependencies:

* 🐧 **Linux** — executables are fully static and have no ELF interpreter or shared library dependency.
* 🪟 **Windows** — executables use the static MSVC runtime (`/MT`).
* 🍎 **macOS** — executables statically include memix and its third-party dependencies. Apple does not support fully static user executables, so `libc++` and `libSystem` remain system-provided dynamic libraries.

To restore platform-default dynamic runtime linking, configure with
`-Dmemix_STATIC_LINK=OFF`.

Tag builds publish the standalone CLIs and Codex skill as GitHub release
assets:

| Platform | Assets |
| -------- | ------ |
| Linux x86_64 | `memix-store-linux-x86_64`, `memix-view-linux-x86_64` |
| macOS arm64 | `memix-store-macos-arm64`, `memix-view-macos-arm64` |
| Windows x86_64 | `memix-store-windows-x86_64.exe`, `memix-view-windows-x86_64.exe` |
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
| `memix_BUILD_HEADERS_ONLY` | `OFF` | Use as header-only library |
| `memix_STATIC_LINK` | `ON` | Self-contained static binaries |
| `memix_ENABLE_UNIT_TESTING` | `ON` | Build unit tests |
| `memix_ENABLE_CLANG_TIDY` | `OFF` | Run clang-tidy checks |
| `memix_ENABLE_CPPCHECK` | `OFF` | Run cppcheck analysis |
| `memix_ENABLE_CODE_COVERAGE` | `OFF` | Enable code coverage |
| `memix_ENABLE_DOXYGEN` | `OFF` | Generate Doxygen docs |
| `memix_ENABLE_LTO` | `OFF` | Enable link-time optimization |
| `memix_ENABLE_ASAN` | `OFF` | Enable AddressSanitizer |
