# Homebrew Tap

This directory contains the `memix` Homebrew formula. To publish it as a tap,
create a GitHub repository named `homebrew-memix` under `MrZLeo` and copy
`Formula/memix.rb` into that repository at the same path.

Homebrew's one-argument tap shorthand maps `mrzleo/memix` to the GitHub
repository `MrZLeo/homebrew-memix`. Once that tap exists, users can install with:

```sh
brew install mrzleo/memix/memix
```

Or:

```sh
brew tap mrzleo/memix
brew install memix
```

`brew install mrzleo/memix` is not a valid fully-qualified formula name for a
third-party tap. Homebrew expects `<user>/<tap>/<formula>` when installing
directly from a tap.

For this command to work from a clean machine, both the tap repository and the
source repository referenced by the formula must be public, or the user must
have GitHub authentication configured for Homebrew's clone/fetch step.

## Dependencies

The formula declares:

* build dependencies: `cmake`, `ninja`
* runtime/build link dependency: `openssl@3`
* bundled source resources: CPM.cmake 0.42.0, `simdjson` 4.6.2, `argum` 2.8,
  and `spdlog` 1.17.0

The formula preloads the source resources into the CPM cache before running
CMake. That keeps the Homebrew build offline while preserving memix's current
linking model: `simdjson`, `argum`, and `spdlog` are built with the project from
source, and only OpenSSL is linked from Homebrew.

## Release Publishing

The formula currently points at the `master` branch so the first tap can be
tested before a new release tag exists. For a stable release, replace the source
block in `memix.rb` with a tag and immutable revision:

```ruby
url "https://github.com/MrZLeo/memix.git",
    tag: "v0.2.0",
    revision: "<tag commit sha>"
```

Then bump `version` to match the tag.

Recommended local checks from the tap repository:

```sh
brew install --build-from-source mrzleo/memix/memix
brew test mrzleo/memix/memix
brew audit --strict --online mrzleo/memix/memix
```
