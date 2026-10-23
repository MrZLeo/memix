# Memix

**Reproducible, versioned memory for AI agents.**

Inspired by Nix, Memix turns rules, documents, and past conversations into reusable context with stable identities. Select the sources once, compose them for a project, and let agents read the result as ordinary files under .memory/.

## Get started with your agent

You can use an agent with terminal and download access, such as **pi, Claude Code, Codex, or DeepSeek Harness**, and give it the prompts below.

### Ask your agent to install Memix

Copy this into your agent:

```text
Install Memix by following this guide:
https://raw.githubusercontent.com/MrZLeo/memix/refs/heads/master/AGENT_INSTALL.md
```

The guide handles platform detection, installation, and verification. Prebuilt
installation targets Linux x86_64, macOS arm64, and Windows x86_64.

It also tells the agent to read the skill immediately, so you can continue
in the same session. In later session, agent can decide whether to read the skill by itself.

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

Requires **C++23, CMake 4.4+, and Ninja**. Run `nix develop` first for the pinned
Linux/macOS toolchain. These commands build the current source tree:

```bash
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The CLI is `build/bin/Release/memix`. Prebuilt binaries are available under
[Releases](https://github.com/MrZLeo/memix/releases).

## Documentation

- [Agent installation guide](AGENT_INSTALL.md)
- [Agent skill and workflows](skill/memix-cli/SKILL.md)
- [CLI reference](skill/memix-cli/references/cli.md)
- [Profiles and rollback](skill/memix-cli/references/profiles-and-recovery.md)
- [Development and contributing](CONTRIBUTING.md)
- [BSD 3-Clause license](LICENSE)
