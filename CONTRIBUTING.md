# Contributing to memix

Thanks for helping improve `memix`.

## Before you start

* Open an issue or start a discussion before making a large change.
* Keep each pull request focused on one problem.
* Add or update tests whenever the change affects behavior.

## Development

Builds require CMake 4.4+, Ninja, and a compiler with C++23 support. Older
C++ standards and compiler extensions are not supported.

For a pinned Linux/macOS toolchain, run `nix develop` first: GCC on Linux,
Clang on macOS. See the
[Nix development environment](README.md#nix-development-environment) section
for supported platforms and build instructions.

Build the project from the repository root:

```bash
cmake -G Ninja -S . -B build \
  -DCPM_SOURCE_CACHE=$HOME/.cache/CPM \
  -Dmemix_ENABLE_CLANG_TIDY=ON
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build -C Debug -VV
```

Verify that the library's public, private, and generated headers compile on
their own (these checks are excluded from the default build):

```bash
cmake --build build --target all_verify_header_sets --parallel
```

When the Inspector is enabled, this target also checks its Objective-C++ bridge
header. The `clang-format` target covers owned C++ headers, implementations,
CLIs, and tests, excluding generated files and third-party sources. Project
clang-tidy/cppcheck integration covers the library and CLIs, not test macros.
Use `memix_WARNINGS_AS_ERRORS=ON` to enforce compiler warnings on all project
C++ targets, including tests, without imposing those flags on package consumers.

The [Docker development environment](README.md#docker-development-environment)
uses the same flake and lock file for Linux builds. Run permission-sensitive
tests as an ordinary user, not root; the container example shows how to drop
privileges after building.

### Current toolchain limitations

The locked Linux environment combines GCC/libstdc++ 16.2 with clang-tidy
23.1.3. The project uses C++23 for both compilation and static analysis;
keep these language modes aligned when validating changes.

* The previous C++26 dynamic-width `std::format` clang-tidy failure no longer
  blocks the project's C++23 configuration. The upstream Clang/libstdc++
  incompatibility is tracked by [GCC PR127220](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=127220)
  / [LLVM #73232](https://github.com/llvm/llvm-project/issues/73232).
  Enable analysis with `-Dmemix_ENABLE_CLANG_TIDY=ON`; no application formatting
  workaround or diagnostic suppression is needed for this issue.
* GCC 16 emits `-Wnull-dereference` diagnostics in optimized CLI paths, with
  two distinct causes. The stream-buffer diagnostic reproduces using only
  `ifstream` and `istreambuf_iterator` at `-O2 -fno-exceptions`, in the known
  [GCC PR105580](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=105580) false-positive
  family. Argum's diagnostics instead involve its custom `BasicExpected`:
  testing whether `error()` returns a non-null pointer does not prove that the
  result holds a value. A callback returning a moved-from error result can
  trigger a real null dereference; no such reuse was found in memix's callbacks.
  Checking the value/error discriminant in an isolated Argum copy removes
  those diagnostics without suppressing warnings. They also reproduce with
  exceptions enabled, without Nix hardening, and in C++20/23 probes; this is
  not solely a C++26 or exception-disabled problem.
  The unmodified dependency passes the 141-test suite in a separate optimized
  ASan/UBSan build, but this is not proof that every possible input is safe.
  The normal build retains the warnings, and the full Linux CLI
  `memix_WARNINGS_AS_ERRORS=ON` gate still does not pass. No experimental Argum
  patch is applied to the dependency cache or production build.
  Nix's fortify hardening injects optimization even when CMake Debug only
  supplies `-g`, so the build type alone does not imply an unoptimized build.

The remaining GCC warnings are not suppressed in the project's warning
configuration, and `flake.lock` is not changed implicitly. macOS uses a
different Clang/Apple standard-library combination; its lint result is
separate from Linux's.

## Style

* Match the surrounding C++ style.
* Prefer small, reviewable commits.
* Run `clang-format` before submitting changes when formatting is affected.

## Pull requests

* Describe the problem being solved.
* Explain the approach you chose.
* Include the exact validation steps you ran.
* Link the related issue when there is one.

The pull request template in [PULL_REQUEST_TEMPLATE.md](PULL_REQUEST_TEMPLATE.md) covers the expected review information.
