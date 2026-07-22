---
name: memix-cli
description: "Use when the agent needs to operate the memix CLI tool group for user-level memory storage and viewing tasks: storing files with memix-store, listing stored objects, reading logical context paths, inspecting agents/chat/documents trees, selecting generations, building memix-view file-tree environments, or showing existing view environments. Focus only on using memix-store, memix-view, and related CLI behavior."
---

# Memix CLI

## Core Rules

Use the installed or user-provided `memix-store` and `memix-view` commands. If a binary path is given, preserve it in examples. If the commands are not available, ask for the command path or installation context.

For experiments, examples, and isolated work, create a fresh temporary directory for each task and pass its absolute path with `--store-root`. Never reuse a fixed shared temporary store. In the examples below, replace `"<store-root>"` with that path and adapt quoting to the active shell; do not use the placeholder literally.

If the user wants the real default store, omit `--store-root`; memix resolves the store root from `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`, then `$HOME/.memix`.

Keep common options before the subcommand:

```text
memix-store --store-root "<store-root>" list
memix-view --store-root "<store-root>" build --source agents/repo/AGENTS.md
```

Supported object types are `agents`, `chat`, and `documents`.

## Store Files

Use `memix-store ingest` to place files in the immutable store.

Store one or more raw blobs without adding a logical context path:

```text
memix-store --store-root "<store-root>" ingest --type documents notes.md transcript.json
```

Store a single file at a logical path inside a type's context tree:

```text
memix-store --store-root "<store-root>" ingest --type agents --dest repo/AGENTS.md --message "add repo agents" AGENTS.md
```

Remember these constraints:

- `--type` is required for `ingest`; use exactly `agents`, `chat`, or `documents`.
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
memix-store --store-root "<store-root>" list --type agents
memix-store --store-root "<store-root>" list --json
```

Show the latest logical tree for a type:

```text
memix-store --store-root "<store-root>" tree --type documents
memix-store --store-root "<store-root>" agents tree
```

Read a logical file from the latest tree:

```text
memix-store --store-root "<store-root>" cat --type documents notes/design.md
memix-store --store-root "<store-root>" agents cat repo/AGENTS.md
```

Use `--generation <generation-id>` with `cat`, `tree`, `agents cat`, or `agents tree` to inspect an older immutable generation. The numbered refs under `refs/<type>/generations/000001` point to generation ids; the CLI option expects the generation id itself.

## Build Views

Use `memix-view build` to create a file-tree view environment from logical store sources. The only supported target is `file_tree`, and it is the default.

```text
memix-view --store-root "<store-root>" build --source agents/repo/AGENTS.md --source chat/task/2026-05-20/
```

Use plain output when the caller needs a human-readable summary or the environment manifest's filesystem path. Use `--json` for a machine-readable environment manifest containing the full environment ID, bindings, and closure IDs. The JSON does not contain filesystem paths.

```text
memix-view --store-root "<store-root>" build --source agents/repo/AGENTS.md --json
```

Source locator syntax:

- `<type>/<logical-path>` selects the latest version of one file.
- `<type>/<logical-directory>/` selects all files under a logical directory.
- Append `@latest`, `@generation/000001`, `@ref/name`, or `@<full-generation-sha>` to pin or select a version.
- Examples: `agents/repo/AGENTS.md@latest`, `chat/task/2026-05-20/@latest`, `documents/notes/design.md@generation/000001`.

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
