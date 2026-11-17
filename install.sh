#!/usr/bin/env bash
# Interactive installer for published Memix binaries. Requires Bash 3.2+.
set -euo pipefail

main() {
  repo=MrZLeo/memix
  version=latest
  install_dir=${HOME:?Set HOME before running the installer}/.local/bin
  skill_args=()
  stage=

  usage() {
    printf '%s\n' \
      'Usage: bash install.sh [--version TAG] [--bin-dir PATH] [--agent NAME ...] [--project]' \
      '' \
      'Install a release binary, then optionally set up the bundled agent skill.' \
      'Both steps require an interactive [Y/n] confirmation. There is no --yes option.' \
      'Default: latest release, ~/.local/bin, detected agents, user scope.' \
      'Supported: Linux x86_64 and macOS 26+ arm64. Use install.ps1 on Windows.' \
      'Private releases require an authenticated GitHub CLI (gh).' \
      'The installer never uses sudo, edits PATH, or forces skill replacement.'
  }

  die() { printf 'Error: %s\n' "$*" >&2; exit 1; }
  cleanup() { if [[ -n $stage ]]; then rm -rf -- "$stage"; fi; }
  trap cleanup EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  trap 'exit 129' HUP

  while (($#)); do
    case $1 in
      --version|--bin-dir|--agent)
        (($# >= 2)) || die "$1 requires a value"
        case $1 in
          --version) version=$2 ;;
          --bin-dir) install_dir=$2 ;;
          --agent)
            case $2 in claude|codex|opencode|pi|dsh) ;; *) die "Unknown agent: $2" ;; esac
            skill_args+=(--agent "$2") ;;
        esac
        shift 2 ;;
      --project) skill_args+=(--project); shift ;;
      --help|-h) usage; exit 0 ;;
      *) die "Unknown option: $1 (see --help)" ;;
    esac
  done
  [[ $install_dir == /* ]] || die '--bin-dir must be an absolute path'
  install_dir=${install_dir%/}
  [[ -n $install_dir ]] || die 'Do not install into the filesystem root'
  destination=$install_dir/memix

  system=$(uname -s)
  arch=$(uname -m)
  case "$system/$arch" in
    Linux/x86_64|Linux/amd64) asset=memix-linux-x86_64 ;;
    Darwin/*)
      # A terminal running through Rosetta may report x86_64 on Apple Silicon.
      if [[ $arch == x86_64 ]] && [[ $(sysctl -n hw.optional.arm64 2>/dev/null || true) == 1 ]]; then
        arch=arm64
      fi
      [[ $arch == arm64 || $arch == aarch64 ]] || die "No release binary for $system/$arch"
      macos=$(sw_vers -productVersion)
      major=${macos%%.*}
      if [[ ! $major =~ ^[0-9]+$ ]] || ((major < 26)); then die 'macOS 26 or newer is required'; fi
      asset=memix-macos-arm64 ;;
    MINGW*|MSYS*|CYGWIN*) die 'Use install.ps1 from a Windows PowerShell terminal' ;;
    *) die "No release binary for $system/$arch; no source build will be attempted" ;;
  esac

  # Do not read answers from a downloaded script or an unattended input stream.
  if [[ -t 0 ]]; then
    exec 3<&0
  elif ! { exec 3</dev/tty; } 2>/dev/null; then
    die 'An interactive terminal is required; download the script and run it in a terminal'
  fi
  confirm() {
    local answer
    while true; do
      printf '%s [Y/n] ' "$1"
      if ! IFS= read -r answer <&3; then
        printf '\n' >&2
        die 'Confirmation cancelled (EOF); no further actions will be performed'
      fi
      case $answer in
        ''|y|Y|yes|Yes|YES) return 0 ;;
        n|N|no|No|NO) return 1 ;;
        *) printf 'Please answer y or n.\n' ;;
      esac
    done
  }

  resolve_release() {
    transport=curl
    if command -v gh >/dev/null 2>&1 && gh auth status --hostname github.com >/dev/null 2>&1; then
      transport=gh
    else
      command -v curl >/dev/null 2>&1 || die 'Install curl or an authenticated GitHub CLI (gh) first'
    fi
    fetch_url() {
      curl --fail --location --silent --show-error --proto '=https' --proto-redir '=https' \
        --connect-timeout 15 --max-time 300 --retry 2 "$@"
    }
    if [[ $version == latest ]]; then
      if [[ $transport == gh ]]; then
        version=$(gh release view --repo "github.com/$repo" --json tagName --jq '.tagName') \
          || die 'Could not resolve the latest release; check GitHub authentication'
      else
        release_url=$(fetch_url --output /dev/null --write-out '%{url_effective}' \
          "https://github.com/$repo/releases/latest") || die 'Could not resolve the latest public release; use authenticated gh for private releases'
        prefix=https://github.com/$repo/releases/tag/
        [[ $release_url == "$prefix"* ]] || die 'GitHub did not return a published release tag'
        version=${release_url#"$prefix"}
      fi
    fi
    [[ $version =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || die "Invalid release tag: $version"
  }

  download_asset() {
    local name=$1 output=$2
    if [[ $transport == gh ]]; then
      gh release download "$version" --repo "github.com/$repo" --pattern "$name" --output "$output"
    else
      fetch_url --output "$output" "https://github.com/$repo/releases/download/$version/$name"
    fi
  }
  check_destination() {
    [[ ! -L $destination ]] || die "Refusing to replace a symlink: $destination"
    [[ ! -e $destination || -f $destination ]] || die "Destination is not a regular file: $destination"
  }
  verify_binary() {
    local reported
    reported=$("$1" --version) || die 'The binary failed its version check'
    [[ $reported == memix\ * && $reported != *$'\n'* ]] || die 'Unexpected binary version output'
    "$1" skill --help >/dev/null || die 'This binary does not support memix skill; install a newer release'
    printf '%s\n' "$reported"
  }

  [[ $version =~ ^[A-Za-z0-9][A-Za-z0-9._+-]*$ ]] || die "Invalid release tag: $version"
  check_destination
  printf '\nMemix installer\n\n1/2  Binary\n  Platform: %s / %s\n  Release:  %s\n  Target:   %s\n\n' "$system" "$arch" "$version" "$destination"
  if [[ -f $destination ]]; then printf 'The existing binary will be replaced only after verification.\n'; fi
  if confirm 'Install this binary?'; then
    resolve_release
    printf '\nSelected release: %s\n' "$version"
    if command -v sha256sum >/dev/null 2>&1; then
      hash_command=(sha256sum)
    elif command -v shasum >/dev/null 2>&1; then
      hash_command=(shasum -a 256)
    else
      die 'SHA-256 verification requires sha256sum or shasum'
    fi
    mkdir -p -- "$install_dir"
    stage=$(mktemp -d "$install_dir/.memix-install.XXXXXX")
    download_asset SHA256SUMS "$stage/SHA256SUMS" || die 'Could not download SHA256SUMS; this release cannot be installed safely'
    expected=$(awk -v name="$asset" '$2 == name { count++; digest = $1 } END { if (count != 1) exit 1; print digest }' "$stage/SHA256SUMS") \
      || die "SHA256SUMS must contain exactly one entry for $asset"
    [[ $expected =~ ^[0-9a-f]{64}$ ]] || die 'Malformed SHA-256 checksum'
    download_asset "$asset" "$stage/$asset" || die "Could not download $asset from release $version"
    actual=$("${hash_command[@]}" "$stage/$asset")
    [[ ${actual%% *} == "$expected" ]] || die 'Checksum mismatch; the existing binary was not replaced'
    chmod 755 "$stage/$asset"
    reported=$(verify_binary "$stage/$asset")
    check_destination
    mv -f -- "$stage/$asset" "$destination"
    binary=$destination
    printf '\nInstalled %s\n  %s\n' "$reported" "$binary"
  else
    printf '\nSkipped binary installation.\n'
    if [[ -x $destination ]]; then binary=$destination; else binary=$(command -v memix || true); fi
    if [[ -z $binary ]]; then
      printf 'No existing memix found; skill setup was skipped.\n'
      exit 0
    fi
    reported=$(verify_binary "$binary")
    printf 'Using %s\n  %s\n' "$reported" "$binary"
  fi

  # Do not silently edit shell profiles or persistent environment settings.
  resolved=$(command -v memix || true)
  if [[ $resolved != "$binary" ]]; then
    printf '\nPATH notice: this binary is not the memix currently resolved by your shell.\n'
    printf 'Add its directory before other Memix installations in PATH, for example:\n'
    printf '  export PATH=%q:"%s"\n' "${binary%/*}" "\$PATH"
  fi

  printf '\n2/2  Agent skill\n\n'
  if ! "$binary" skill setup --dry-run ${skill_args[@]+"${skill_args[@]}"}; then
    printf '\nThe preview reported problems. Setup may only succeed for some paths.\n'
  fi
  printf '\nSetup will not use --force, install hooks, or modify memory.\n'
  if confirm 'Run skill setup with these options?'; then
    if ! "$binary" skill setup ${skill_args[@]+"${skill_args[@]}"}; then
      die 'Skill setup was incomplete; the binary remains installed. Inspect memix skill status before retrying'
    fi
    printf '\nDone. Read the operating instructions with:\n  memix skill show\n'
  else
    printf '\nSkipped skill setup. You can run memix skill setup later.\n'
  fi
}

# Keep the entry point last: a truncated download must not start installation.
main "$@"
