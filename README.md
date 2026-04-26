# memix

`memix` is a small CMake-based C++ project with a Rust-style split between library code and executable entrypoints:

* `include/` and `src/` contain the `memix` library
* `src/bin/*.cpp` are auto-discovered and built as executables

The sample CLI uses [CLI11](https://github.com/CLIUtils/CLI11) for argument
parsing.

## Dependencies

The project uses [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) to fetch:

* [CLI11 2.5.0](https://github.com/CLIUtils/CLI11/releases/tag/v2.5.0) for CLI parsing
* [simdjson 4.6.2](https://github.com/simdjson/simdjson/releases/tag/v4.6.2) for metadata JSON parsing and writing
* [spdlog 1.17.0](https://github.com/gabime/spdlog/releases/tag/v1.17.0) for executable logging
* [Catch2 3.11.0](https://github.com/catchorg/Catch2/releases/tag/v3.11.0) for tests

The immutable store uses OpenSSL libcrypto through CMake's `OpenSSL::Crypto`
target for SHA-256 content hashing.

## Build

```bash
cmake -S . -B build \
  -DCPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake --build build
```

The default build always produces the `memix` library and also builds every `src/bin/*.cpp` entrypoint as an executable.

## Run

```bash
./build/bin/Debug/memix-store --store-root /tmp/memix-store ingest --type agents --dest README.md README.md
./build/bin/Debug/memix-store --store-root /tmp/memix-store agents tree
./build/bin/Debug/memix-store --store-root /tmp/memix-store list
./build/bin/Debug/memix-store --store-root /tmp/memix-store list --json
./build/bin/Debug/memix-store --version
./build/bin/Debug/memix-store --help
```

`memix-store` defaults to `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`,
then `$HOME/.memix`. The supported immutable object types are `agents`, `chat`,
and `documents`.

For a manual smoke check against the real default store root, run:

```bash
./build/bin/Debug/memix-store ingest --type agents --dest README.md README.md
./build/bin/Debug/memix-store agents tree
./build/bin/Debug/memix-store list --type agents
```

Agents ingest requires a source path plus `--dest`, the logical destination
inside the agents tree. Commands under `agents` operate on that logical tree.
Refs such as `refs/agents/latest` and `refs/agents/generations/000001` are
small mutable files containing immutable generation ids; each generation then
points at its immutable root tree.

That writes outside the repository, so use `--store-root` when you want an
isolated test store.

## Tests

```bash
cmake -S . -B build -DCPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake --build build
ctest --test-dir build -C Debug -VV
```

## Common Options

* `memix_BUILD_EXECUTABLE=ON`
* `memix_BUILD_HEADERS_ONLY=OFF`
* `memix_ENABLE_UNIT_TESTING=ON`
* `memix_ENABLE_CLANG_TIDY=ON`
* `memix_ENABLE_CPPCHECK=ON`
* `memix_ENABLE_CODE_COVERAGE=ON`
* `memix_ENABLE_DOXYGEN=ON`
* `memix_ENABLE_LTO=ON`
* `memix_ENABLE_ASAN=ON`
