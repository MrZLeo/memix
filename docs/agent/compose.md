# Select and compose

A Derivation (D) is an immutable selection of source files. An Environment (E)
mounts existing Ds. A Quest records a reusable recipe and its result history.
Reuse suitable results: one-off reads need no new selection or composition.

## Choose inputs and materialize

Check `memix view find --covers <type/path> --json` and Quest metadata before
creating another selection. Use the user's intended sources and versions.

```text
memix view select --source documents/projects/example/ --name "Example decisions" --intent "Project decisions; start at documents/projects/example/decisions.md" --json
memix view compose --mount <derivation-id> --intent "Example workspace" --json
```

Selection returns D; composition returns E without creating another D.
Retain the returned Quest and full `result: {kind, id}`.
If only a recipe is wanted, use `define --kind selection` or
`define --kind composition` without materializing it.

Source locators are `type/path`; directories end in `/` before a version suffix.
Unqualified locators follow latest when resolved. Append `@<full-generation-id>`
for immutable inputs; `@latest`, `@generation/000001`, and existing `@ref/name`
select source refs, not Profile generations. Never refresh an explicitly
selected historical version.

Compose only D IDs with repeatable `--mount D[=MOUNT]`; omit the mount for root.
Use safe relative paths for named mounts. Do not pass E IDs, Quests, or Store
paths, and do not copy/re-ingest outputs to combine them. Even one D must be
composed into E before activation.

## Preserve meaning and versions

Composition pins D IDs. Resolving it again does not follow newer source content.
When an update is authorized, resolve the selection and explicitly compose the
new D bindings; replacing bindings changes the composition Quest. Existing D/E
remain unchanged.

Use name/intent for supported scope and entry paths. Changing catalog annotations
does not update frozen guidance; materialize again to capture intended changes.
Changing only composition guidance changes E, not its Ds.

`file_tree` packages bytes; it does not summarize or resolve conflicting claims.
Colliding output files, even identical ones, fail composition; root `README.md`
is reserved. Choose distinct mounts or ask which inputs are intended rather
than silently dropping sources.

Inspect the result with typed `tree/cat`. Load `memix agent attach` before
activation, or `memix agent recovery` on failure.
