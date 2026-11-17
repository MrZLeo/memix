#!/usr/bin/env bash
set -euo pipefail

if (($# != 4)); then
  printf 'Usage: %s OUTPUT LINUX_BINARY MACOS_BINARY WINDOWS_BINARY\n' "$0" >&2
  exit 2
fi

output=$1
linux_binary=$2
macos_binary=$3
windows_binary=$4
root=$(cd "$(dirname "$0")/.." && pwd -P)

for binary in "$linux_binary" "$macos_binary" "$windows_binary"; do
  if [[ ! -f $binary ]]; then
    printf 'Error: binary not found: %s\n' "$binary" >&2
    exit 1
  fi
done

case $output in
  /*) ;;
  *) output=$PWD/$output ;;
esac
mkdir -p "$(dirname "$output")"
stage=$(mktemp -d "${TMPDIR:-/tmp}/memix-opendesk-package.XXXXXX")
trap 'rm -rf -- "$stage"' EXIT
plugin=$stage/memix-opendesk

mkdir -p "$plugin/bin/linux-x86_64" \
  "$plugin/bin/macos-arm64" \
  "$plugin/bin/windows-x86_64" \
  "$plugin/skills"
cp "$root/plugin/opendesk/opendesk.plugin.json" "$plugin/"
cp "$root/plugin/opendesk/index.mjs" "$plugin/"
cp "$root/plugin/opendesk/README.md" "$plugin/"
cp -R "$root/skill/memix-cli" "$plugin/skills/"
cp "$linux_binary" "$plugin/bin/linux-x86_64/memix"
cp "$macos_binary" "$plugin/bin/macos-arm64/memix"
cp "$windows_binary" "$plugin/bin/windows-x86_64/memix.exe"
chmod 755 "$plugin/bin/linux-x86_64/memix" "$plugin/bin/macos-arm64/memix"

tar -czf "$output" -C "$stage" memix-opendesk
printf 'Created %s\n' "$output"
