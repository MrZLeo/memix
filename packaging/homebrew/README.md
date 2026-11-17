# Homebrew packaging

Memix is distributed from the `MrZLeo/homebrew-tap` tap (`mrzleo/tap`). The
Formula installs the versioned, checksummed prebuilt CLI binary produced by the
tagged release workflow on macOS 26+ (Apple Silicon) and Linux x86_64.

Generate a Formula for an existing stable release with:

```sh
ruby packaging/homebrew/generate-formula.rb \
  v0.7.0 \
  packaging/homebrew/testdata/v0.7.0-SHA256SUMS \
  /tmp/memix.rb
```

The release workflow runs this generator against the newly created `SHA256SUMS`,
uploads `memix.rb` as a release asset, and pushes `Formula/memix.rb` to
`MrZLeo/homebrew-tap` after publishing the GitHub release when the
`HOMEBREW_TAP_DEPLOY_KEY` Actions secret is configured. That secret holds the
private half of a dedicated repository SSH deploy key registered with write
access on `MrZLeo/homebrew-tap`.

To validate a generated Formula in a temporary local tap:

```sh
brew tap-new --no-git mrzleo/memix-test
brew trust mrzleo/memix-test
install -m 0644 /tmp/memix.rb "$(brew --repository mrzleo/memix-test)/Formula/memix.rb"
brew style --formula mrzleo/memix-test/memix
brew readall --aliases --os=all --arch=all mrzleo/memix-test
brew audit --strict --online --formula mrzleo/memix-test/memix
brew install mrzleo/memix-test/memix
brew test mrzleo/memix-test/memix
brew uninstall mrzleo/memix-test/memix
brew untrust mrzleo/memix-test
brew untap mrzleo/memix-test
```
