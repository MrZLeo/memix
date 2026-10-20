# Profiles and Recovery

Read the main [SKILL.md](../SKILL.md) for task routing and authorization. Examples below use the configured Store; for an explicit Store, put `--store-root "<absolute-store-path>"` before every subcommand. Replace example IDs, names, and paths with the task's actual values.

## Activate Only When an Attachment Is Needed

Only E can be activated. Generation/marker v2 records reference E, not a single
D. Old view/Profile formats are unsupported; `--force` is not a migration
command. Report incompatibility without deleting or rewriting Store history.

```text
memix view activate <environment-id> --into ./.memory
memix view status --into ./.memory
memix view generations --into ./.memory
```

Activation records a numbered generation within a Store Profile and switches its `current` pointer. The workspace holds a symlink in the default `--mode link`, or disposable real files in `--mode copy`. The default attachment path is `./.memory`; use the supplied `--into` path consistently.

Before replacing an existing active attachment, understand its role in the current task. Inspect `status` when its Environment, Profile, or state is uncertain. Do not switch unrelated workspace context just to answer a one-off recall question; `tree` / `cat` need no activation.

Read `<attachment>/README.md` for the activated Environment's frozen Quest scope and entry points, then read the relevant payload. Retain the Environment ID, Profile generation number, and attachment path returned by activation. A successful mount alone does not mean the model has read the memory.

Activation never grants write access to memory. Never edit through a linked attachment; copied files are disposable caches, not editable sources. To update memory, ingest changed source content, select/resolve the intended D, explicitly compose updated D bindings into E, then activate that E. D itself cannot be activated.

## Profile Selection

Without `--profile`, Memix derives `ws-<hash>` from the canonical workspace directory. Separate workspaces therefore do not switch each other's default Profiles.

Use `--profile NAME` only when workspaces should intentionally share one mutable current pointer. Explicit names match `[a-z0-9][a-z0-9._-]*`, are at most 64 characters, and cannot start with `ws-`.

```text
memix view activate <environment-id> --profile shared-project --into ./.memory --mode link
memix view generations --profile shared-project --into ./.memory
```

- Linked workspaces follow the shared Profile's current generation. Activating or rolling back that Profile can change all their visible memory.
- A copy generation records one workspace and its attachment. Other copied workspaces do not update automatically when the Profile switches.
- Use the same `--profile` / `--into` selection for activation, generation inspection, and rollback. `status --into <path>` inspects the actual attachment.

If sharing would affect workspaces outside the requested scope, ask before switching their Profile. Do not choose a shared Profile merely for a convenient short name.

## Link and Copy Behavior

Reactivating the current Environment through an already-correct link attachment is a no-op. A different Environment creates the next Profile generation and switches `current`; earlier generations remain readable under the Profile's `generations/` directory.

Copy mode always rebuilds its disposable workspace copy, even for an already-current Environment. Use it to repair deleted or modified copied files:

```text
memix view activate <current-environment-id> --into ./.memory --mode copy
```

For an explicit Profile, also repeat `--profile NAME`. `status` reports the Profile, generation, Environment, and attachment staleness. Here `stale: true` means a copied attachment is behind its Profile's current generation, **not** that a Quest's source refs have moved.

When refreshing a stale copy, obtain the Profile's current Environment from its generation information and preserve the same Profile, `--into`, and `--mode copy`. Omitting the mode selects link mode instead of refreshing the copy. Do not confuse this operation with resolving a stale Quest against newer sources.

## Rollback

```text
memix view generations --into ./.memory
memix view rollback --into ./.memory
memix view rollback --to 2 --into ./.memory
```

Without `--to`, rollback selects the previous Profile generation. `--to N` expects a **Profile generation number**, not an Environment ID, source generation ID, or `@generation/NNNNNN` source ref. Inspect history before choosing a target; use the full retained reference, not an ID reconstructed from a displayed prefix.

For rollback, `--into` helps derive the default Profile when `--profile` is omitted; it does not choose which workspace to rebuild. Rolling back to a copy generation rematerializes the workspace recorded in that generation's metadata, possibly different from the path passed to select the Profile. Check that recorded workspace and the effect on linked sharing workspaces before proceeding.

Rollback restores the selected Environment and its frozen README. It does not revert the source Store's latest generations or update Quest annotations.

## Deactivate and Remove Profile History

```text
memix view deactivate --into ./.memory
```

Ordinary deactivation removes the workspace attachment, not its Profile history or stored source memory.

`deactivate --into <path> --wipe-profile` additionally deletes Profile history. Use it only when the user intends that removal and the impact on every sharing workspace has been checked. It is not a routine cleanup step.

Wiping a shared Profile leaves other link attachments dangling. Other copied files remain, but their activation markers reference a missing Profile and can no longer be validated or managed as active attachments. Neither deactivation nor wiping a Profile is physical erasure of stored memory.

## Foreign Paths and Force

By default activation refuses to replace a real file, an unmarked directory, or a foreign symlink. Do not bypass this protection just to complete an activation.

`activate ... --force` permits replacing an existing symlink, including a foreign one, only when that replacement is intended. It never permits replacing a real file or a real directory without a Memix activation marker. Ask about an alternative attachment path or an explicitly authorized move of the blocking path; do not silently move user files aside.

If the user cancels a choice, leave the attachment and Profile unchanged. Never turn cancellation into a guessed activation, rollback, or deletion.

## Failure Recovery

Use root or subcommand help to verify syntax:

```text
memix store --help
memix store ingest --help
memix view activate --help
memix view rollback --help
```

- **No latest ref or missing source:** inspect the logical tree and requested version. A new source type needs an intentional `ingest --dest` before logical reads/builds can use it. Do not create placeholder memory to hide a recall failure.
- **Invalid locator:** check the supported type, safe relative path, version selector, and trailing `/` for directories. See [CLI reference](cli.md).
- **Missing/stale projection:** use `memix store expose` only when current-source exploration is permitted. It repairs the derived projection, not source content or authorization.
- **Path conflict during composition:** choose the intended version or put both inputs under distinct mounts. Do not drop a source merely to make the build pass.
- **Lock reported:** a lock may belong to a live Memix operation. Confirm that no operation is using that exact Profile or workspace and obtain approval before removal. Remove only the exact reported stale lock directory, never a glob or its parent. Profile locks are `lock` directories under `views/profiles/`; workspace locks are sibling `.memix-activation-lock-<hash>` directories beside the attachment. If ownership or liveness is uncertain, stop and report it.

Report the failed operation and what remains unchanged. Do not claim memory was saved, rebuilt, attached, refreshed, or removed until the corresponding command succeeds.
