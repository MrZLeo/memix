# Memix

**Reproducible, versioned memory for AI agents.**

Inspired by Nix, Memix turns rules, documents, and past conversations into reusable context with stable identities. Select the sources once, compose them for a project, and let agents read the result as ordinary files under .memory/.

## Get started with your agent

Install Memix and its discovery skill once, then use **Pi, Claude Code, Codex,
OpenCode, or DeepSeek Harness** with ordinary requests.

### Install interactively

**Linux / macOS**

```sh
curl -fsSL https://github.com/MrZLeo/memix/releases/latest/download/install.sh | bash
```

**Windows (PowerShell)**

```powershell
irm https://github.com/MrZLeo/memix/releases/latest/download/install.ps1 | iex
```

The installer detects your OS/architecture and asks **two separate `[Y/n]`
questions**: install the binary, then run skill setup. Press Enter for yes or
`n` to skip. EOF or an unattended shell never counts as consent.

Supported releases: Linux x86_64, macOS 26+ arm64, and Windows x86_64. Downloads
are checked against the same release's `SHA256SUMS` before replacing an existing
binary. The default destination is `~/.local/bin` (`memix.exe` on Windows).
Follow the PATH notice if needed; no sudo, PATH edits, or forced skill
replacement are performed.

You can inspect/download [install.sh](install.sh) or [install.ps1](install.ps1)
first. Their help covers custom directories, pinned releases, agent selection,
and project scope. Skill setup otherwise uses detected agents and user scope.

### Or ask your agent to help

Copy this into your agent:

```text
Install Memix by following this guide:
https://raw.githubusercontent.com/MrZLeo/memix/refs/heads/master/AGENT_INSTALL.md
```

The short guide hands installation to the same interactive script, then tells
the agent to verify the result and read `memix skill show` in the current session.
You still answer the two installation prompts yourself.

### Everyday use: just ask

Once the skill is installed, use normal language rather than CLI commands:

| Goal | Example prompt |
| --- | --- |
| Save a decision | "Remember that we chose SQLite for this project's local cache because it needs to work offline." |
| Recall context | "What did we decide about caching in this project? Check Memix and show me the source." |
| Update a rule | "Update this project's saved test command to `pnpm test`." |
| Reuse memory in another project | "Attach my saved coding preferences alongside this project's rules at `.memory/`." |
| Refresh attached context | "Refresh this project's `.memory/` from its latest saved rules and decisions." |
| Roll back an attachment | "Restore the previous memory snapshot for this workspace." |

A few things to keep in mind:

- **Saved memory and attached context are separate.** Saving an update does not
  change an existing snapshot. Ask to refresh it when you want the new version.
- **Treat `.memory/` as read-only context.** Ask the agent to update the saved
  source and refresh the attachment instead of editing files through it.
- **Rollback restores the attachment, not the saved sources.** Earlier versions
  remain available without undoing newer source updates.

### Manage the agent skill

The CLI bundles a lightweight skill and its version-matched guides:

```sh
memix skill setup --dry-run       # preview setup for detected agents
memix skill setup --agent pi      # install/update for a selected agent
memix skill status --json         # inspect all known locations
memix skill show                  # read the entry guide, not every topic
memix skill remove --agent pi     # remove managed skill files, not memory
```

Supported agents: `claude`, `codex`, `opencode`, `pi`, and `dsh`. Repeat `--agent`
for multiple agents; omit it from setup to detect config directories or programs
on `PATH`. Add `--project` to use the nearest Git/Jujutsu repository instead of
your account. Setup/remove support `--dry-run`; setup/status/remove support
`--json`.

Codex, OpenCode, and Pi normally share `.agents/skills/memix-cli`; changing that
path affects all its readers. Agent-home overrides and existing native copies
are respected. Setup updates managed files but reports foreign conflicts;
`--force` is an explicit setup-only override for regular files. Remove preserves
foreign bundles and extra user files. No hooks, settings, or memory Stores are
changed. See `memix skill --help` for options and the
[agent installation guide](AGENT_INSTALL.md) for the handoff and verification.

## Architecture

```text
+------------------------------------------------+
| Store: rules / documents / chat                |
| immutable blobs + versioned trees              |
+------------------------+-----------------------+
                         | select
            +------------+------------+.............+
            v                         v             :
+----------------------+  +----------------------+  : +----------------------+
| Derivation A         |  | Derivation B         |  : | Quests               |
| shared context       |  | project context      |  +.+ recipes + intent     |
+-----------+----------+  +-----------+----------+  : | result history       |
            |                         |             : | on-demand freshness  |
            +------------+------------+             : +----------------------+
                         | compose                  :
                         v                          :
+------------------------------------------------+  :
| Environment                                    +..+
| /shared -> A    /project -> B                  |
+------------------------+-----------------------+
                         | activate
                         v
+------------------------------------------------+         +------------+
| Profile: generations -> current                +-------->| .memory/   |
+------------------------------------------------+         +------------+
```

Dotted links connect Quests to selection and composition.

| Object | Responsibility |
| --- | --- |
| **Store** | Keeps immutable SHA-256 blobs in versioned trees, separately for `rules`, `documents`, and `chat`. Edits reuse unchanged subtrees; previous generations remain readable. |
| **Derivation** | Freezes a selection of files, their source generations, blob identities, and provenance. Each selection can be read independently and reused across projects. |
| **Environment** | Mounts one or more Derivations into a single file tree. Composition references existing Derivations; it does not create a combined Derivation. |
| **Quest** | Records a selection or composition recipe, searchable intent, result history, and CLI usage receipts. Selection freshness is checked on demand against source generations. |
| **Profile** | Binds an Environment to a workspace through numbered activations and a movable `current` pointer, supporting explicit upgrades and rollback. |

**Selection and composition are separate.** Two projects can share a Derivation
but mount it differently. Changing a mount or replacing a Derivation creates a
new Environment; existing views keep their identities and contents.

Store updates do not silently change delivered memory. Refresh the selection,
compose a new Environment, then activate it. A Profile can roll back to a
previously activated Environment without reselecting sources.

The Store's read-only `latest/` projection supports discovery with ordinary
filesystem tools. Built views pin their inputs instead of following that live
projection.

**Current scope:** raw file-tree selection, composition, explicit refresh, and
activation. Generated summaries/embeddings, semantic conflict resolution, and
proactive invalidation are not implemented. Composition rejects path collisions
instead of silently overriding files.

## Build from source (optional)

Agent-assisted installation uses prebuilt releases. For development or a manual
source build, follow the steps below.

Requires **C++23, CMake 4.4+, and Ninja**. macOS builds target **26 or newer**;
set `-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0` when configuring on macOS. Run
`nix develop` first for the pinned Linux/macOS toolchain. CMake embeds agent Markdown as standard C++ string data
and tracks document edits for incremental builds; no compiler extensions or
runtime documentation files are needed. These commands build the current source tree:

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The CLI is `build/bin/Release/memix`. Prebuilt binaries are available under
[Releases](https://github.com/MrZLeo/memix/releases).

## Documentation

- [Agent installation guide](AGENT_INSTALL.md)
- [Agent skill bootstrap](skill/memix-cli/SKILL.md)
- [Agent workflow guide](docs/agent/guide.md): run `memix agent guide`, then
  load only the required topic with `memix agent recall`, `save`, `compose`,
  `attach`, or `recovery`. Instructions are bundled with the installed CLI;
  these commands do not open or initialize a Store.
- [Attachment and Profiles](docs/agent/attach.md)
- [Recovery and rollback](docs/agent/recovery.md)
- [Development and contributing](CONTRIBUTING.md)
- [BSD 3-Clause license](LICENSE)
