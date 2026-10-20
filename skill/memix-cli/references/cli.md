# Memix CLI Reference

Read the main [SKILL.md](../SKILL.md) for task routing and authorization. These are syntax templates, not commands to execute against the user's Store as a demonstration. Replace example paths and IDs with inspected or returned values. Use the task's command paths and shell quoting. For an isolated experiment, create a fresh temporary directory and pass its absolute path before every subcommand:

```text
memix store --store-root "<absolute-store-path>" tree --all --depth 2 --json
memix view --store-root "<absolute-store-path>" quests --json
```

The examples below omit that common option. Without it, the Store resolves from `MEMIX_STORE_HOME`, then `$XDG_DATA_HOME/memix`, then `$HOME/.memix`.

## Source Files and Generations

Supported types are exactly `rules`, `chat`, and `documents`. Logical paths are relative within a type, such as `projects/example/decisions.md`.

```text
memix store ingest --type documents --dest projects/example/decisions.md --message "record decision" decisions.md
memix store ingest --type rules --dest projects/example/AGENTS.md AGENTS.md
memix store cat --type documents projects/example/decisions.md
memix store rules cat projects/example/AGENTS.md
memix store cat --type documents --generation <full-generation-id> projects/example/decisions.md
```

- `--dest` accepts one source file. Each ingest commits a new logical-tree generation, retaining other entries, and advances that type's latest ref. Use separate ingests for multiple logical files.
- `--message` is optional and requires `--dest`. Keep the generation ID returned by a successful ingest when exact replay matters.
- To update an entry, prepare changed content outside the Store and ingest to the same destination. Neither old generations nor existing Environments change.
- `cat`, `tree`, `rules cat`, and `rules tree` accept `--generation <full-generation-id>`. This expects the ID itself, **not** `000001` or `generation/000001`.

Raw-object storage, only when logical retrieval is not wanted:

```text
memix store ingest --type documents notes.md transcript.json
memix store list --type documents --json
```

Without `--dest`, ingest may take several files but creates blobs only. They appear in `list`, not in the logical tree or `latest/`, and cannot be selected by logical `cat` or `build --source`. `list` is a blob inventory, not a list of current logical memory.

Logical retraction:

```text
memix store rm --type documents projects/example/obsolete.md --message "retract obsolete note"
memix store rm --type chat sessions/old-project
```

`--type` is required, and the file or directory subtree must exist in the latest tree. Removal creates a new generation and refreshes the latest projection. Blobs, earlier generations, and pinned Views remain readable. This is not secure erasure; the CLI provides no garbage collection or physical-forgetting workflow.

## Inspect and Search Sources

```text
memix store tree --all --depth 2 --json
memix store tree --type documents --path projects/example --depth 2 --json
memix store tree --type rules
memix store rules tree
```

Catalog output includes type, logical path, entry kind, recursive file count, total bytes, latest timestamp, and file blob IDs. Supplying `--all`, `--path`, `--depth`, or `--json` selects this catalog form; plain `tree --type <type>` retains compact path/blob output. These are path-oriented inventories, not semantic rankings.

`<store-root>/latest/` is a derived, read-only hardlink projection of current logical trees. Scope text searches to a relevant type or project first:

```text
rg -n -i -F -- "deployment" "<store-root>/latest/documents/projects/example"
```

A projection hit at `latest/documents/projects/example/decisions.md` maps to locator `documents/projects/example/decisions.md`. Read enough surrounding content to establish its meaning. Use format-specific extractors for PDFs and other binary payloads. Neither the CLI nor the projection supplies a full-text/vector index.

Ingest with `--dest` and logical removal refresh the affected projection after committing. A refresh failure warns without undoing the commit. If the projection is missing or was reported stale, and current-source exploration is allowed, repair it with:

```text
memix store expose
```

Never edit projection files: they hardlink immutable blobs. Do not use `expose` or a latest-source search to bypass a deliberately restricted source set or a requested historical version. For historical reads, use the specified generation or Environment.

## Quests: Discover, Define, Resolve

A Quest stores a canonical recipe and mutable name/intent annotations.
Selection results are D; composition results are E. Discover by metadata:

```text
memix view find deployment --json
memix view find deployment postgres --json
memix view find --covers documents/projects/example/decisions.md --json
memix view quests --json
memix view define --kind selection --source documents/projects/example/ --name "Example decisions" --intent "Project decisions" --json
memix view resolve <quest-id> --json
memix view freshness <quest-id> --json
```

`find` is ASCII case-insensitive substring matching on name/intent, not payload
or semantic search. Multiple terms and `--covers` are AND filters.
`--covers` inspects direct Store selectors, not a composition's transitive
files. An empty metadata search is not proof that memory is absent.

`define` registers a recipe without materialization; its default kind is
selection. `define --kind composition --mount D[=MOUNT]` registers pinned
bindings. `select/compose/resolve --json` return `quest`, typed `result`
and `inserted`. Discovery returns `result_kind` and `latest_result`.

Before choosing a current result:

- A null `latest_result` means there is no usable latest result in the catalog.
  Resolve only when appropriate; freshness requires an existing resolution.
- Otherwise preserve its `kind` and full `id`. An explicit historical D/E
  must not be replaced just because a newer result exists.
- Selection freshness compares per-input source generations. Deleted refs have
  `current_generation: null`, `pinned: false`, `fresh: false`.
- Composition pins D IDs and has no ref drift. Re-resolving it does not refresh
  the Ds. Resolve the selection and explicitly compose updated D IDs when
  authorized; replacing a D changes the composition Quest.

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

An unqualified locator follows latest at select/resolve time. The resulting D remains immutable; changed refs may produce a new D. Existing E bindings do not follow it. Use explicit source-generation pins for reproducible inputs; never silently refresh a supplied historical D/E.

## Select Derivations and Compose Environments

```text
memix view select --source rules/projects/example/AGENTS.md --source documents/projects/example/ --name "Example selection" --intent "Project rules and decisions" --json
memix view compose --mount <derivation-a>=rules --mount <derivation-b>=docs --intent "Combined project context" --json
memix view compose --mount <derivation-id> --json
```

The implemented target is raw-only `file_tree`. Selection produces D without
E; composition binds existing Ds without producing another D.
`--mount DERIVATION[=MOUNT]` accepts a D ID or unique D prefix, then pins the
full ID. Root mount is omitted or `.`; paths must be safe relative POSIX paths.

- Raw selection may deduplicate the same path/blob while preserving all input
  resolutions. Different blobs at the same selected path conflict.
- Composition canonicalizes binding order. Duplicate bindings, any colliding
  final file paths (even identical bytes), and file/directory conflicts fail.
  Directories may merge. Use distinct mounts or ask which sources are intended.
- Composition does not accept E, Store paths, Quest IDs, latest, or subtree
  mounts. Reuse D bindings explicitly; do not re-ingest outputs to compose.

Name/intent explain supported scope and entry paths. D and E each freeze their
own recipe/guidance; updating catalog annotations does not alter saved replay.
Changing composition guidance and recomposing changes only E. Activation's
README lists E plus every canonical D binding and frozen guidance, not the
complete machine recipe. The assembled E root `README.md` is reserved.

Old `build/--view`, D-v1/v2/v3, E-v1/v2, and old Quest/Profile formats are
unsupported. There is no migration or fallback. Report errors; do not delete,
rehash, or silently replace the user's Store to get a successful command.

## Read, Audit, and Interpret Usage

```text
memix view ls derivation:<derivation-id>
memix view tree derivation:<derivation-id> --depth 2 --json
memix view cat derivation:<derivation-id> documents/projects/example/decisions.md
memix view tree environment:<environment-id> --path docs --depth 2 --json
memix view cat environment:<environment-id> docs/documents/projects/example/decisions.md
memix view show derivation:<derivation-id> --json
memix view show environment:<environment-id> --json
memix view stats <quest-id> --json
```

Use output paths returned by `ls/tree`, especially under mounts. D browsing
needs no E or Profile. Bare IDs always mean E; they never probe both namespaces.
`cat` returns verified bytes; `show` is receipt-free provenance inspection.
D manifests expose frozen recipes and source resolutions; E manifests expose
composition recipes, bindings and closure. Follow D bindings for source
provenance rather than assuming E has one owner.

`list --json` is an E inventory with binding/file/byte counts.
`list --kind derivation --json` inventories D. Use the Quest catalog for
task discovery rather than repeatedly dumping these inventories.

Successful `cat/ls/tree` add best-effort typed receipts to the object's own
Quest. E activation (including reactivation) and rollback belong to its
composition Quest, not its mounted selection Quests. No per-binding fan-out
occurs. `ls` counts separately; `stats` separates reads by D and E.
Inspector/show/deactivation do not count consumption; ordinary attachment
reads bypass CLI receipts. Usage is not comprehension, relevance, or truth.

`stats` reports corrupt logs; history refuses append-through-corruption.
A history error may name an already published reusable D/E. Retain that ID,
report the failure, and do not silently repair logs or switch a Profile.

There is no semantic conflict resolution, external derive, generated/local
payload support, context-pack generation or remote synchronization.
For activation, shared Profiles, Copy, rollback and recovery, read
[Profiles and recovery](profiles-and-recovery.md).
