# Memix CLI Reference

Read the main [SKILL.md](../SKILL.md) for task routing and authorization. These are syntax templates, not commands to execute against the user's Store as a demonstration. Replace example paths and IDs with inspected or returned values. Use the task's command paths and shell quoting. For an isolated experiment, create a fresh temporary directory and pass its absolute path before every subcommand:

```text
memix-store --store-root "<absolute-store-path>" tree --all --depth 2 --json
memix-view --store-root "<absolute-store-path>" quests --json
```

The examples below omit that common option. Without it, the Store resolves from `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`, then `$HOME/.memix`.

## Source Files and Generations

Supported types are exactly `rules`, `chat`, and `documents`. Logical paths are relative within a type, such as `projects/example/decisions.md`.

```text
memix-store ingest --type documents --dest projects/example/decisions.md --message "record decision" decisions.md
memix-store ingest --type rules --dest projects/example/AGENTS.md AGENTS.md
memix-store cat --type documents projects/example/decisions.md
memix-store rules cat projects/example/AGENTS.md
memix-store cat --type documents --generation <full-generation-id> projects/example/decisions.md
```

- `--dest` accepts one source file. Each ingest commits a new logical-tree generation, retaining other entries, and advances that type's latest ref. Use separate ingests for multiple logical files.
- `--message` is optional and requires `--dest`. Keep the generation ID returned by a successful ingest when exact replay matters.
- To update an entry, prepare changed content outside the Store and ingest to the same destination. Neither old generations nor existing Environments change.
- `cat`, `tree`, `rules cat`, and `rules tree` accept `--generation <full-generation-id>`. This expects the ID itself, **not** `000001` or `generation/000001`.

Raw-object storage, only when logical retrieval is not wanted:

```text
memix-store ingest --type documents notes.md transcript.json
memix-store list --type documents --json
```

Without `--dest`, ingest may take several files but creates blobs only. They appear in `list`, not in the logical tree or `latest/`, and cannot be selected by logical `cat` or `build --source`. `list` is a blob inventory, not a list of current logical memory.

Logical retraction:

```text
memix-store rm --type documents projects/example/obsolete.md --message "retract obsolete note"
memix-store rm --type chat sessions/old-project
```

`--type` is required, and the file or directory subtree must exist in the latest tree. Removal creates a new generation and refreshes the latest projection. Blobs, earlier generations, and pinned Views remain readable. This is not secure erasure; the CLI provides no garbage collection or physical-forgetting workflow.

## Inspect and Search Sources

```text
memix-store tree --all --depth 2 --json
memix-store tree --type documents --path projects/example --depth 2 --json
memix-store tree --type rules
memix-store rules tree
```

Catalog output includes type, logical path, entry kind, recursive file count, total bytes, latest timestamp, and file blob IDs. Supplying `--all`, `--path`, `--depth`, or `--json` selects this catalog form; plain `tree --type <type>` retains compact path/blob output. These are path-oriented inventories, not semantic rankings.

`<store-root>/latest/` is a derived, read-only hardlink projection of current logical trees. Scope text searches to a relevant type or project first:

```text
rg -n -i -F -- "deployment" "<store-root>/latest/documents/projects/example"
```

A projection hit at `latest/documents/projects/example/decisions.md` maps to locator `documents/projects/example/decisions.md`. Read enough surrounding content to establish its meaning. Use format-specific extractors for PDFs and other binary payloads. Neither the CLI nor the projection supplies a full-text/vector index.

Ingest with `--dest` and logical removal refresh the affected projection after committing. A refresh failure warns without undoing the commit. If the projection is missing or was reported stale, and current-source exploration is allowed, repair it with:

```text
memix-store expose
```

Never edit projection files: they hardlink immutable blobs. Do not use `expose` or a latest-source search to bypass a deliberately restricted source set or a requested historical version. For historical reads, use the specified generation or Environment.

## Quests: Discover, Define, Resolve

A Quest stores a reusable query plus name/intent annotations; resolutions associate it with built Environments. Use the public catalog:

```text
memix-view find deployment --json
memix-view find deployment postgres --json
memix-view find --covers documents/projects/example/decisions.md --json
memix-view quests --json
```

`find` matches substrings in name and intent, with ASCII case-insensitivity. Several terms are an AND filter, not alternatives or semantic search. Terms and `--covers` also combine as filters. Plain tables truncate long annotations; use JSON for their full text.

`--covers` checks structural relationships against the Quest's direct context inputs, not whether content supports a claim. It does not traverse composed `--view` inputs. Thus, no coverage match does not rule out a relevant composed Environment; inspect its metadata and derived paths instead.

```text
memix-view define --source documents/projects/example/ --name "Example decisions" --intent "Project decisions; start at documents/projects/example/decisions.md" --json
memix-view resolve <quest-id> --json
memix-view freshness <quest-id> --json
```

`define` creates or annotates a Quest without building. `resolve` builds its query; `build` defines and resolves in one operation. Equivalent canonical queries reuse their Quest identity. Retain the Quest ID from `define` or catalog JSON; an Environment ID is not a Quest ID.

Before following a Quest's latest result:

- If `resolution_count` is zero or `latest_environment_id` is empty, resolve it when building fits the request. `freshness` on an unresolved Quest reports an error, not a fresh result.
- Otherwise check `freshness`, and reuse the catalog's latest Environment or resolve stale inputs as appropriate. Resolve failure does not authorize dropping inputs or substituting unrelated material.
- Freshness compares recorded context-input generations with their selected versions. Deleted followed refs have `current_generation: null`, `pinned: false`, and `fresh: false`.
- Composed Environment inputs are content-pinned and omitted from this comparison. A View-only Quest has `inputs: []` and `stale: false`, even if the original upstream sources have changed.

## Source Locators and Version Choice

| Locator | Meaning |
| --- | --- |
| `documents/projects/example/decisions.md` | One file at latest when resolved |
| `documents/projects/example/` | A recursive subtree at latest when resolved |
| `rules/projects/example/AGENTS.md@latest` | Explicitly follow latest |
| `documents/projects/example/@generation/000001` | Select a numbered source generation ref |
| `documents/projects/example/decisions.md@<full-generation-id>` | Pin an immutable source generation |
| `rules/projects/example/AGENTS.md@ref/name` | Select an existing custom ref |

Directory locators end in `/` before any `@version`. These are source-generation refs, not Profile generation numbers. The CLI can read custom refs but does not create or manage them.

An unqualified locator follows latest at build/resolve time. The resulting Environment remains immutable; rerunning the query after its refs move may produce another Environment. Use followed sources for ongoing context and explicit generation pins for reproducible inputs. Do not silently refresh a supplied historical Environment.

## Build and Compose Environments

```text
memix-view build --source rules/projects/example/AGENTS.md --source documents/projects/example/ --name "Example workspace" --intent "Project rules and decisions for current work" --json
memix-view build --view <environment-a> --view <environment-b> --intent "Combined project context" --json
memix-view build --view <environment-a>=prior --source documents/projects/example/decisions.md --json
```

The only target is `file_tree`, also the default. It packages selected files at stable paths without merging their contents. Use one source if that is sufficient; do not create a wrapper View solely to select an existing Environment.

`--view ENVIRONMENT[=MOUNT]` accepts a full Environment ID or unique prefix and may repeat. It pins the resolved full ID, not a Profile's mutable current pointer. With no mount, upstream paths stay unchanged; an optional safe relative mount prefixes them.

- Same output path and same blob deduplicate.
- Same output path and different blobs conflict. Use distinct mounts if both versions are needed, or choose the intended version; do not silently discard a source.
- Composition preserves direct and transitive upstream Environment, Derivation, artifact, and raw-object IDs. A View is a derived input, not another source type; do not ingest its output merely to reference it.

Give the query a concise searchable name. Intent should explain source-supported scope and use; include entry paths, directory roles, and version conventions when useful for navigation. Do not claim coverage absent from the selected files.

Quest name, intent, and query are snapshotted into the Derivation at build time and participate in its identity. Changing annotations and rebuilding can change the Environment even with identical payloads. Updating the catalog alone leaves existing Environments unchanged; source `freshness` does not detect an annotation-only change. Rebuild/resolve when the new navigation description is wanted.

Activation generates a root `README.md` with Environment/Derivation IDs and the frozen Quest ID, name, and intent; it omits the full query definition. Re-activation and rollback reproduce that snapshot, not the latest catalog wording. The generated README is an attachment file, not part of the derived payload tree. Root `README.md` is reserved for activation; an Environment already containing that output path cannot be activated.

## Read, Audit, and Interpret Usage

```text
memix-view tree <environment-id> --depth 2 --json
memix-view tree <environment-id> --path documents/projects/example --depth 2 --json
memix-view cat <environment-id> documents/projects/example/decisions.md
memix-view show <environment-id> --json
memix-view stats <quest-id> --json
```

Use derived paths from `tree`, especially for mounted compositions. `tree` and `cat` work without activation. `show` is manifest/provenance inspection, not a payload read.

`build --json`, `resolve --json`, and `show --json` expose the Environment ID, binding Derivation IDs, and closure IDs, not filesystem paths or each source's resolved generation. Plain build/show output includes the Environment manifest path, not the materialized tree path. For exact source provenance, retain ingest generation IDs or inspect the Derivation manifest; do not infer resolved generations from Environment JSON alone.

`memix-view list --json` is a low-level Environment inventory with full IDs, Derivation IDs, file/byte counts, and upstream counts. Use it for debugging, not as the default discovery catalog.

Successful CLI `cat`, `tree`, and `activate` record best-effort consumption receipts. Activation records even a no-op reactivation; rollback records the Environment it recommits; deactivate records nothing. Ordinary attachment reads bypass CLI receipts. `stats` and catalog usage counts show observed operations, not payload comprehension, semantic relevance, or evidence quality.

The CLI does not provide semantic conflict resolution, summarization/context-pack generation, or remote synchronization. Those are not implicit effects of a successful build.

For activation, shared Profiles, copies, rollback, or recovery, read [Profiles and recovery](profiles-and-recovery.md).
