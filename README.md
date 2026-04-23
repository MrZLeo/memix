# memix

`memix` is a small CMake-based C++ project with a Rust-style split between library code and executable entrypoints:

* `include/` and `src/` contain the `memix` library
* `src/bin/*.cpp` are auto-discovered and built as executables

The sample CLI uses [Lyra](https://www.bfgroup.xyz/Lyra/) for argument parsing.

## Dependencies

The project uses [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) to fetch:

* [Lyra 1.7.0](https://github.com/bfgroup/Lyra/releases/tag/1.7.0) for CLI parsing
* [Catch2 3.11.0](https://github.com/catchorg/Catch2/releases/tag/v3.11.0) for tests

Lyra itself is an official header-only library, but `memix` consumes it through its upstream CMake project and public target `bfg::lyra` instead of vendoring the generated single-header amalgamation.

## Build

```bash
cmake -S . -B build \
  -DCPM_SOURCE_CACHE=$HOME/.cache/CPM

cmake --build build
```

The default build always produces the `memix` library and also builds every `src/bin/*.cpp` entrypoint as an executable.

## Run

```bash
./build/bin/Debug/memix-store --lhs 4 --rhs 5
./build/bin/Debug/memix-store --version
./build/bin/Debug/memix-store --help
```

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
