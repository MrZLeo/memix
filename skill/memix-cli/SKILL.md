---
name: memix-cli
description: "Recall, load, inspect, compose, or save durable user and project memory with Memix. Use when users ask to remember something; recover or use prior decisions, notes, chats, rules, documents, or context without necessarily naming Memix; ask what was stored previously; or need memix-store, memix-view, view discovery/composition, activation, generations, rollbacks, profiles, or workspace attachments. Do not use for RAM, memory leaks, allocators, programming-language memory models, or information already available in the current conversation unless the user wants it persisted."
---

# Memix CLI

## Mental Model

Treat Memix as a Nix-inspired, provenance-aware build system for agent context, not as a mutable notes directory.

- Treat stored blobs as immutable evidence. Raw ingest without `--dest` creates blob objects only; ingest with `--dest` also creates a new logical-tree generation. Ingest changed content again instead of editing files inside the store directly.
- Treat logical trees and generation objects as immutable history, and refs such as `latest` as movable version selectors. Use `latest` intentionally, and pin a generation when replay, auditability, or reproducibility matters.
- Treat views as first-class reproducible derived objects, not as new sources of truth or store object types. A view environment may be inspected, referenced by another view, mounted below a path, and composed transitively. Reuse an existing environment instead of copying or modifying its output.
- Treat movable refs and profile `current` pointers as the small mutable control layer. Profiles record numbered generations and switch one `current` pointer; workspaces attach through a symlink (or a disposable copy) and never edit memory in place.
- Preserve provenance without inventing visibility. The environment ID is the durable reference to a view: retain it from `build` or `list` for later composition, `activate`, or inspection. Retain profile generation numbers from `activate` or `generations` for `rollback`, which does not accept an environment ID. Environment JSON exposes binding and transitive closure IDs; `tree` and `cat` expose the derived file surface. Full locator-to-generation provenance remains in the derivation manifest.
- Keep selection explainable and attributable. Infer sources only from the user's request, the active attachment or profile, or an inspected logical tree with an unambiguous match. Do not invent hidden dependencies or silently add unrelated sources. Distinguish a suggested view from one that was successfully activated.
- Treat `file_tree` as packaging: it assembles selected files at stable paths but does not merge Markdown, apply directive precedence, summarize content, or resolve semantic conflicts.
- Respect current capability boundaries. Inventory is path-oriented exploration, not semantic ranking. The CLI does not yet provide directive resolution, context-pack generation, full-text/vector indexing, garbage collection, or remote synchronization.

## Implicit Memory Workflow

Treat a clear request to recall, load, use, inspect, or save durable memory as sufficient reason to use Memix. Do not require the user to mention `$memix-cli`, request `build` or `activate`, or supply an environment ID. Match the user's intent, not the presence of the word "memory"; do not trigger this workflow for RAM, memory leaks, allocators, or programming-language memory models.

For recall or load requests, follow this discovery ladder:

1. Check the expected attachment with `memix-view status --into ./.memory`. If it is active and contains the requested context, read through `./.memory`.
2. Discover reusable views with `memix-view list --json`. Inspect promising ones with `memix-view tree <environment> --depth 2 --json`, then use `memix-view cat <environment> <path>` for the few files whose logical paths match the request.
3. If no existing view covers the request, explore the store broadly once with `memix-store tree --all --depth 2 --json`. Narrow only promising areas with `--type`, `--path`, and `--depth`; use store `cat` only after a candidate path is identified.
4. Prefer an existing environment when it already covers the request. If it is close, compose it with `build --view`; add `--source` only for missing context. Build directly from sources only when no reusable view fits.
5. Activate only when the task benefits from a workspace attachment, repeated multi-file reads, or tools that need a directory. For one or two reads, use view `tree`/`cat` without changing the active profile.
6. Ask one focused clarification only when materially different selections remain plausible after exploration. Do not ask the user to manually enumerate sources that the inventories can resolve.

Use an exploration budget unless the user asks for an exhaustive audit:

- Inspect at most three candidate views, initially at depth 2.
- Perform one broad store inventory and at most two narrower subtree inventories before deciding.
- Read candidate file contents only when their type and logical path plausibly match the request.
- If more than three materially different candidates remain or choosing one would change the meaning of the task, summarize the alternatives and ask.

For save requests, do not require the user to name `ingest`. Infer the type and logical destination only when the request or an established workspace convention makes them clear; otherwise ask for the missing choice before storing.

After automatic composition or activation, briefly report reused environment IDs, added source locators, the resulting environment ID, and—if activated—the profile generation and attachment path. Never say memory was loaded or activated until the relevant command succeeds.

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
- Without `--dest`, `ingest` creates raw blob objects only. They appear in `list` but cannot be selected by `cat`, `tree`, or `memix-view build` because they have no logical path or context generation.
- `--dest` accepts one source file only and advances that type's latest generation.
- To place multiple files in a logical tree or view, run `ingest` once per file with a distinct `--dest`; each new generation carries forward the preceding logical tree.
- `--message` is valid only with `--dest`.
- A successful `--dest` ingest logs the full generation ID. Retain it when a later view must pin the exact input rather than use a numbered ref.
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

Explore every logical tree with bounded depth:

```text
memix-store --store-root "<store-root>" tree --all --depth 2
memix-store --store-root "<store-root>" tree --all --depth 2 --json
```

Catalog output includes object type, logical path, entry kind, recursive file count, total bytes, latest timestamp, and blob ID for files. Use it as the broad path-oriented discovery surface.

Show the latest logical tree for a type, or narrow one subtree:

```text
memix-store --store-root "<store-root>" tree --type documents
memix-store --store-root "<store-root>" tree --type documents --path notes/project --depth 2 --json
memix-store --store-root "<store-root>" rules tree
```

The original `tree --type <type>` form retains its compact path/blob output. Supplying `--all`, `--path`, `--depth`, or `--json` selects catalog output with directory aggregates.

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

Compose existing view environments directly:

```text
memix-view --store-root "<store-root>" build --view 7f3a2c9b1d0e
memix-view --store-root "<store-root>" build --view 7f3a2c9b1d0e=prior --source documents/notes/design.md
```

`--view ENVIRONMENT[=MOUNT]` accepts a full environment ID or unique prefix and may be repeated. With no mount, the upstream paths remain unchanged. With `=MOUNT`, every upstream path is prefixed by that safe relative mount. `--source` remains backward compatible and can be mixed with `--view`; builds containing a view input use the typed v2 manifests, while pure `--source` builds retain v1 hashes and manifest bytes.

Composition follows these rules:

- Environment inputs are pinned to the resolved full environment ID; they do not follow a mutable profile.
- Same output path plus the same blob deduplicates. Same output path plus different blobs fails with a conflict; use distinct mounts to resolve it.
- The new environment closure includes direct and transitive upstream environment, derivation, artifact, and raw object IDs.
- A view is a derived input, not a `rules`, `chat`, or `documents` store type. Do not ingest a view back into the store merely to reference it.

Plain output prints the environment ID (the input `activate` needs) and the environment manifest's filesystem path. Use `--json` for a machine-readable environment manifest containing the full environment ID, bindings, and closure IDs, but no filesystem paths.

```text
memix-view --store-root "<store-root>" build --source rules/repo/AGENTS.md --json
```

An unqualified locator or `@latest` is resolved when the build runs. The resulting derivation remains immutable, but rerunning the same expression after `latest` moves can produce a different derivation. When the command itself must replay the same input, use a numbered generation ref such as `@generation/000001` or a full generation ID already known from ingest or prior bookkeeping. Current `build --json` and `show --json` output does not reveal each source's resolved generation, so do not promise that `latest` can be converted to a pinned locator from that output alone.

Source locator syntax:

- `<type>/<logical-path>` selects the latest version of one file.
- `<type>/<logical-directory>/` selects all files under a logical directory.
- Append `@latest`, `@generation/000001`, `@ref/name`, or `@<full-generation-sha>` to pin or select a version.
- Examples: `rules/repo/AGENTS.md@latest`, `chat/task/2026-05-20/@latest`, `documents/notes/design.md@generation/000001`.
- `@ref/name` selects an existing custom ref. The current CLI can read such a ref but does not create or manage custom refs.

## Activate Views

Use `memix-view activate` to attach a built environment to the workspace, Nix-profile style. Activation records a numbered generation inside a store profile and switches the profile's `current` pointer; the workspace itself only holds a symlink (or, with `--mode copy`, real files) at the `--into` path, which defaults to `./.memory`.

```text
memix-view --store-root "<store-root>" activate 7f3a2c9b1d0e
memix-view --store-root "<store-root>" activate 7f3a2c9b1d0e --into ./.memory --mode link
```

After activation, read memory through the attach path with ordinary file tools (`ls`, `cat`, `grep`, editor). Never write through it: in link mode the store blobs are read-only, and copied activations are disposable caches that a fresh `activate` rebuilds.

Profile selection:

- Without `--profile`, the profile name is `ws-<hash>` derived from the canonical workspace directory, so parallel workspaces never switch each other's memory.
- Use `--profile NAME` (`[a-z0-9][a-z0-9._-]*`, at most 64 characters, no `ws-` prefix) only when several workspaces should intentionally share one switchable memory.
- Sharing a profile behaves differently per mode: link workspaces all follow the profile's one current generation, while a copy activation records one workspace per generation. A copied workspace goes stale when another workspace activates the same profile; refresh it by activating the profile's current environment with the same profile selection, the same `--into` path, and `--mode copy`.

Re-activating the environment that is already current through an already-correct link attachment is a no-op. Copy activation always rebuilds the disposable workspace copy, even when its environment is already current, so rerun `activate` to repair deleted or modified copied files. Activating a different environment creates the next generation and switches `current`; old generations stay readable under the profile's `generations/` directory.

Use `generations`, `rollback`, `status`, and `deactivate` to manage the profile and the workspace attachment:

```text
memix-view --store-root "<store-root>" generations
memix-view --store-root "<store-root>" rollback              # previous generation
memix-view --store-root "<store-root>" rollback --to 2       # explicit generation
memix-view --store-root "<store-root>" status --into ./.memory
memix-view --store-root "<store-root>" deactivate --into ./.memory
memix-view --store-root "<store-root>" deactivate --into ./.memory --wipe-profile
```

`generations` and `rollback` take the same `--profile` / `--into` pair as `activate` to resolve which profile to operate on. For `rollback`, `--into` only helps derive the default profile when `--profile` is omitted; it does not select the workspace to rebuild. Rolling back to a copy generation rematerializes the workspace recorded in that generation's metadata, which may differ from the `--into` path used to select the profile. `status` reports the profile, generation, environment, and staleness of the workspace attachment (`stale: true` means a copied activation is behind the profile's current generation).

`deactivate` only removes the workspace attachment unless `--wipe-profile` is given. Treat `--wipe-profile` as destructive and use it only when the user intends to remove the profile history and the effect on every sharing workspace has been checked. Wiping a shared profile leaves link attachments dangling; copied files remain, but their activation markers reference a missing profile and can no longer be validated or managed as active attachments.

By default, activation never replaces foreign paths: if the `--into` path holds a real file, a real directory without a memix activation marker, or a symlink created for another purpose, the command fails instead of clobbering it. `--force` permits replacing an existing symlink, including a foreign symlink, but never a real file or an unmarked directory.

## Inspect View Environments

List reusable environments with full IDs, derivation IDs, file/byte counts, and transitive upstream counts:

```text
memix-view --store-root "<store-root>" list
memix-view --store-root "<store-root>" list --json
```

Explore or read a view without activating it:

```text
memix-view --store-root "<store-root>" tree 7f3a2c9b1d0e --depth 2 --json
memix-view --store-root "<store-root>" tree 7f3a2c9b1d0e --path documents/notes --depth 2
memix-view --store-root "<store-root>" cat 7f3a2c9b1d0e documents/notes/design.md
```

Use `tree` to choose files by their stable derived paths and `cat` for a targeted read. Prefer these commands over activation when no workspace attachment is needed.

Use `memix-view show` to inspect an existing environment manifest by full ID or unique prefix, mainly for provenance debugging and audit:

```text
memix-view --store-root "<store-root>" show 7f3a2c9b1d0e
memix-view --store-root "<store-root>" show 7f3a2c9b1d0e --json
```

Plain output includes the environment manifest's filesystem path; `--json` prints the manifest with the full environment and binding derivation IDs plus closure IDs, including upstream environments for composed v2 views. Neither mode reports the materialized derivation tree's filesystem path. For file discovery and reads, prefer `tree` and `cat` over parsing `show` output.

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
- If an operation reports a lock after a previous memix process crashed, first confirm that no memix process is still operating on that exact profile or workspace and obtain approval before deleting anything. Remove only the exact reported lock directory, never a glob or a parent directory: profile locks are `lock` directories under `views/profiles/`, while workspace locks are sibling directories named `.memix-activation-lock-<hash>` beside the attachment path.
- If `status` reports `stale: true`, re-run `activate` with the profile's current environment, the same profile selection, the same `--into` path, and `--mode copy`. Omitting `--mode copy` selects the default link mode instead of refreshing the copied attachment.
- If `activate` refuses to replace the `--into` path, use `--force` only when the path is a symlink you intend to replace. Move a real file or unmarked directory aside yourself; memix never clobbers those paths.
- If output should be isolated from the user's real store, create a new per-task temporary directory and rerun with its path as `--store-root`.
