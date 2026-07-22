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

Download both platform assets and the platform-independent Codex skill archive
from that exact release:

```text
https://github.com/MrZLeo/memix/releases/download/<tag>/memix-store-<asset-suffix><executable-suffix>
https://github.com/MrZLeo/memix/releases/download/<tag>/memix-view-<asset-suffix><executable-suffix>
https://github.com/MrZLeo/memix/releases/download/<tag>/memix-cli-skill.tar.gz
```

## 2. Install and verify the commands

Install the two assets in a user-owned directory on `PATH`. Do not use
administrator or root privileges unless the user explicitly requests a
system-wide installation.

Rename the installed files to their canonical command names:

- Linux and macOS: `memix-store` and `memix-view`
- Windows: `memix-store.exe` and `memix-view.exe`

On Linux and macOS, make both files executable. Run both commands with
`--version` and treat any non-zero exit status as an installation failure.

## 3. Install the matching CLI skill

Extract `memix-cli-skill.tar.gz` into a temporary directory first. Verify that
the archive contains this complete layout:

```text
memix-cli/
├── SKILL.md
└── agents/
    └── openai.yaml
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

The resulting paths must be `<skill-root>/memix-cli/SKILL.md` and
`<skill-root>/memix-cli/agents/openai.yaml`. Do not flatten the directory or
create an extra nested `memix-cli/memix-cli` directory. If a destination named
`memix-cli` already exists but is not a previous installation of this skill,
stop instead of overwriting it.

If the current agent is neither Codex, pi, nor Claude Code, keep the
extracted skill in a temporary location rather than guessing an installation
directory.

Read the entire extracted `SKILL.md` in the current session before running
memix for the user; a newly installed skill might only be discovered
automatically by a later agent session. Follow the file as the operating
instructions for `memix-store` and `memix-view`. If the exact-tag skill archive
cannot be downloaded or does not have the expected layout, stop instead of
substituting instructions from a different version.

Finally, report the installed release tag, the two command paths, the skill
path when installed, and the result of both version checks. When running as
pi, also mention that later sessions can load the skill explicitly with
`/skill:memix-cli`. When running as Claude Code, mention that later sessions
can invoke it with `/memix-cli`, and that Claude Code only discovers new
skills at session start — which is why the current session must follow the
extracted `SKILL.md` directly.
