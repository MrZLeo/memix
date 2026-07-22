---
name: memix-cli
description: "Use when the agent needs to operate the memix CLI tool group for user-level memory storage and viewing tasks: storing files with memix-store, listing stored objects, reading logical context paths, inspecting rules/chat/documents trees, selecting generations, building memix-view file-tree environments, or showing existing view environments. Follow Memix's immutable, versioned, provenance-aware model while using memix-store, memix-view, and related CLI behavior."
---

# Memix CLI

## Mental Model

Treat Memix as a Nix-inspired, provenance-aware build system for agent context, not as a mutable notes directory.

- Treat stored blobs as immutable evidence. Ingest changed content as a new object and generation; never edit files inside the store directly.
- Treat logical trees and refs as version selectors over immutable history. Use `latest` intentionally, and pin a generation when replay, auditability, or reproducibility matters.
- Treat views as reproducible derived artifacts, not as new sources of truth. Reuse an identical derivation instead of copying or modifying its output.
- Preserve provenance without inventing visibility. Retain the environment ID and the environment manifest path from plain output when handing off a view. Environment JSON exposes binding and closure IDs, but not source locators, resolved generations, or locator-to-blob mappings; do not claim otherwise. Full resolved-input provenance lives in the derivation manifest, whose filesystem path is not currently reported by `build` or `show`.
- Keep selection explicit. Do not invent hidden dependencies, silently add sources, or imply that a suggestion activated memory.
- Treat `file_tree` as packaging: it assembles selected files at stable paths but does not merge Markdown, apply directive precedence, summarize content, or resolve semantic conflicts.
- Respect current capability boundaries. The CLI does not yet provide directive resolution, context-pack generation, search/indexing, activation/rollback commands, garbage collection, or remote synchronization.

## Core Rules

Use the installed or user-provided `memix-store` and `memix-view` commands. If a binary path is given, preserve it in examples. If the commands are not available, ask for the command path or installation context.

For experiments, examples, and isolated work, create a fresh temporary directory for each task and pass its absolute path with `--store-root`. Never reuse a fixed shared temporary store. In the examples below, replace `"<store-root>"` with that path and adapt quoting to the active shell; do not use the placeholder literally.

If the user wants the real default store, omit `--store-root`; memix resolves the store root from `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`, then `$HOME/.memix`.

Keep common options before the subcommand:

```text
memix-store --store-root "<store-root>" list
memix-view --store-root "<store-root>" build --source rules/repo/AGENTS.md
```

Supported object types are `rules`, `chat`, and `documents`.

## Store Files

Use `memix-store ingest` to place files in the immutable store.

Store one or more raw blobs without adding a logical context path:

```text
memix-store --store-root "<store-root>" ingest --type documents notes.md transcript.json
```

Store a single file at a logical path inside a type's context tree:

```text
memix-store --store-root "<store-root>" ingest --type rules --dest repo/AGENTS.md --message "add repo rules" AGENTS.md
```

Remember these constraints:

- `--type` is required for `ingest`; use exactly `rules`, `chat`, or `documents`.
- `--dest` accepts one source file only and advances that type's latest generation.
- `--message` is valid only with `--dest`.
- Logical paths are relative paths inside the chosen type, such as `repo/AGENTS.md`, `sessions/root.json`, or `notes/design.md`.

## Inspect Store Contents

List all stored blobs:

```text
memix-store --store-root "<store-root>" list
```

Filter by type or emit machine-readable metadata:

```text
memix-store --store-root "<store-root>" list --type rules
memix-store --store-root "<store-root>" list --json
```

Show the latest logical tree for a type:

```text
memix-store --store-root "<store-root>" tree --type documents
memix-store --store-root "<store-root>" rules tree
```

Read a logical file from the latest tree:

```text
memix-store --store-root "<store-root>" cat --type documents notes/design.md
memix-store --store-root "<store-root>" rules cat repo/AGENTS.md
```

Use `--generation <generation-id>` with `cat`, `tree`, `rules cat`, or `rules tree` to inspect an older immutable generation. The numbered refs under `refs/<type>/generations/000001` point to generation ids; the CLI option expects the generation id itself.

## Build Views

Use `memix-view build` to create a file-tree view environment from logical store sources. The only supported target is `file_tree`, and it is the default.

```text
memix-view --store-root "<store-root>" build --source rules/repo/AGENTS.md --source chat/task/2026-05-20/
```

Use plain output when the caller needs a human-readable summary or the environment manifest's filesystem path. Use `--json` for a machine-readable environment manifest containing the full environment ID, bindings, and closure IDs. The JSON does not contain filesystem paths.

```text
memix-view --store-root "<store-root>" build --source rules/repo/AGENTS.md --json
```

An unqualified locator or `@latest` is resolved when the build runs. The resulting derivation remains immutable, but rerunning the same expression after `latest` moves can produce a different derivation. When the command itself must replay the same input, use a numbered generation ref such as `@generation/000001` or a full generation ID already known from ingest or prior bookkeeping. Current `build --json` and `show --json` output does not reveal each source's resolved generation, so do not promise that `latest` can be converted to a pinned locator from that output alone.

Source locator syntax:

- `<type>/<logical-path>` selects the latest version of one file.
- `<type>/<logical-directory>/` selects all files under a logical directory.
- Append `@latest`, `@generation/000001`, `@ref/name`, or `@<full-generation-sha>` to pin or select a version.
- Examples: `rules/repo/AGENTS.md@latest`, `chat/task/2026-05-20/@latest`, `documents/notes/design.md@generation/000001`.

## Show Views

Use `memix-view show` to inspect an existing environment by full id or unique prefix:

```text
memix-view --store-root "<store-root>" show 7f3a2c9b1d0e
memix-view --store-root "<store-root>" show 7f3a2c9b1d0e --json
```

The same output distinction applies to `show`: plain output includes the environment manifest's filesystem path, while JSON includes the full environment and binding derivation IDs plus the closure's derivation, artifact, and raw object IDs, but no filesystem paths. Neither mode currently reports the materialized derivation tree's filesystem path.

## Troubleshooting

Use `--help` on the root command or a subcommand when option details are uncertain:

```text
memix-store --help
memix-store ingest --help
memix-view build --help
```

Common fixes:

- If `cat`, `tree`, or `memix-view build` says a latest ref is missing, first ingest a file with `--dest` for that type.
- If a view source is missing, check the logical tree with `memix-store tree --type <type>`.
- If a locator fails, verify the type is supported, the path is relative, and directory locators end with `/`.
- If output should be isolated from the user's real store, create a new per-task temporary directory and rerun with its path as `--store-root`.
