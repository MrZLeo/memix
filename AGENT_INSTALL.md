# Install memix (agent instructions)

Follow these instructions when the user asks you to install memix.

## 1. Select release assets

Install the prebuilt release binaries. Do not build memix from source.

Detect the operating system and CPU architecture, then select the matching
asset suffix:

| Platform | Accepted architecture names | Asset suffix | Executable suffix |
| --- | --- | --- | --- |
| Linux | `x86_64`, `amd64` | `linux-x86_64` | none |
| macOS | `arm64`, `aarch64` | `macos-arm64` | none |
| Windows | `x86_64`, `amd64` | `windows-x86_64` | `.exe` |

If the platform does not match this table, stop and report that no prebuilt
binary is available. Do not silently fall back to a source build.

Resolve the latest published release tag for `MrZLeo/memix`. Prefer an
authenticated GitHub client when one is available; it also works when the
repository or release is not public. Keep the resolved tag for the skill step
below. If no published release can be resolved, stop and report that memix is
not available for prebuilt installation yet.

Check that the release includes the unified `memix` platform asset. If it is
an older release with only `memix-store` / `memix-view`, follow `AGENT_INSTALL.md`
from that exact tag instead; do not mix release versions or rename a legacy
binary to `memix`.

Download the platform asset and the platform-independent agent skill archive
from that exact release:

```text
https://github.com/MrZLeo/memix/releases/download/<tag>/memix-<asset-suffix><executable-suffix>
https://github.com/MrZLeo/memix/releases/download/<tag>/memix-cli-skill.tar.gz
```

## 2. Install and verify the command

Install the binary in a user-owned directory on `PATH`. Do not use
administrator or root privileges unless the user explicitly requests a
system-wide installation.

Rename the installed file to its canonical command name:

- Linux and macOS: `memix`
- Windows: `memix.exe`

On Linux and macOS, make the file executable. Run `memix --version`,
`memix store --help`, `memix view --help`, and `memix tui --help`; treat any
non-zero exit status as an installation failure. This single binary includes
all three namespaces and needs no sibling executables. Legacy command assets
remain available for existing scripts, but are not needed for a new installation.

## 3. Install the matching CLI skill

Extract `memix-cli-skill.tar.gz` into a temporary directory first. Verify that
the archive contains this complete layout:

```text
memix-cli/
├── SKILL.md
├── agents/
│   └── openai.yaml
└── references/
    ├── cli.md
    └── profiles-and-recovery.md
```

The `agents/openai.yaml` file is Codex-specific interface metadata. Other
harnesses ignore it; keep it in place so the installed directory stays
identical across agents.

Install the complete `memix-cli` directory under the skill root that matches
the current agent:

- Codex: use `$CODEX_HOME/skills` when `CODEX_HOME` is set, otherwise
  `$HOME/.codex/skills`.
- pi: use `$HOME/.pi/agent/skills` for a user-wide installation. When the
  user asked for a project-local installation, use `.pi/skills` at the
  project root instead. The shared `$HOME/.agents/skills` directory also
  works when the user wants one installation served to multiple harnesses.
- Claude Code: use `$HOME/.claude/skills` for a user-wide installation.
  When the user asked for a project-local installation, use
  `.claude/skills` at the project root instead.
- DeepSeek Harness (DSH): use `$DSH_HOME/skills` when `DSH_HOME` is set,
  otherwise `$HOME/.dsh/skills`, for a user-wide installation. When the user
  asked for a project-local installation, use `.dsh/skills` at the project
  root instead.

Keep every file in the layout above, including both `references/` documents,
under `<skill-root>/memix-cli/`. The main skill links to those documents for
on-demand CLI and Profile guidance. Do not flatten the directory or create
an extra nested `memix-cli/memix-cli` directory. If a destination named
`memix-cli` already exists but is not a previous installation of this skill,
stop instead of overwriting it.

If the current agent is neither Codex, pi, Claude Code, nor DeepSeek Harness,
keep the extracted skill in a temporary location rather than guessing an
installation directory.

Read the entire extracted `SKILL.md` in the current session before running
memix for the user; a newly installed skill might only be discovered
automatically by a later agent session. Follow the file as the operating
instructions for `memix store` and `memix view`, loading its relative reference
files when the task needs them. If the exact-tag skill archive
cannot be downloaded or does not have the expected layout, stop instead of
substituting instructions from a different version.

Finally, report the installed release tag, the command path, the skill
path when installed, and the results of the version and help checks. When running as
pi, also mention that later sessions can load the skill explicitly with
`/skill:memix-cli`. When running as Claude Code, mention that later sessions
can invoke it with `/memix-cli`, and that Claude Code only discovers new
skills at session start — which is why the current session must follow the
extracted `SKILL.md` directly.
