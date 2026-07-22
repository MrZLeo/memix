# Contributing to memix

Thanks for helping improve `memix`.

## Before you start

* Open an issue or start a discussion before making a large change.
* Keep each pull request focused on one problem.
* Add or update tests whenever the change affects behavior.

## Development

Build the project from the repository root:

```bash
cmake -G Ninja -S . -B build -DCPM_SOURCE_CACHE=$HOME/.cache/CPM
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build -C Debug -VV
```

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
