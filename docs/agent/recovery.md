# Recovery and rollback

Errors do not authorize migration, deletion, forced changes, replacement Stores,
or refreshing historical snapshots. Old formats have no migration fallback.
Inspect first; retain IDs from partial successes and report what actually
changed. Use the failing command's `--help` to check syntax.

## Profile operations

Inspect `memix view status --into <path>` and
`memix view generations --into <path>` before changing a Profile.
Repeat its explicit `--profile NAME`, if any. Check effects on all linked
workspaces; load `memix agent attach` if sharing semantics are unclear.

- **Rollback:** `memix view rollback --into <path> --to <number>` selects a
  Profile generation, not a source generation or Environment ID. Without
  `--to`, it selects the previous generation. For a copy generation, rollback
  rebuilds the workspace recorded in that generation, not necessarily the
  supplied `--into` path. Verify that target before proceeding. Rollback does
  not revert source content or Quest annotations.
- **Refresh a copy:** obtain the Profile's current Environment from its
  generation metadata, then activate it with the same Profile, `--into`,
  and `--mode copy`. This rebuilds disposable files, even for the same E;
  it does not refresh sources.
- **Deactivate:** `memix view deactivate --into <path>` removes only the
  attachment. `--wipe-profile` additionally deletes Profile history; require
  explicit intent and check every sharing workspace. Other links become
  dangling and copied attachments lose their valid Profile. Neither operation
  erases stored memory.

## Diagnose without bypassing safeguards

- **Missing source:** inspect the logical tree and requested version; do not
  create placeholder memory to hide a recall failure.
- **Missing/stale projection:** `memix store expose` repairs the current-source
  projection only when that exploration is permitted, never to bypass a
  historical/source restriction. A projection warning after ingest may mean
  the commit succeeded; verify before retrying.
- **Invalid locator or composition conflict:** consult `memix agent compose`.
  Do not drop inputs merely to make the operation succeed.
- **Foreign attachment:** `activate --force` can replace a symlink only when
  intended; it cannot replace real files or unmarked directories. Ask for
  another path or an authorized move; never silently move user files aside.
- **Lock:** confirm no live operation owns the exact Profile/workspace lock
  and obtain approval before removing it. Remove only the reported stale lock
  directory, never a glob or parent. If liveness is uncertain, stop.
- **History error:** an operation may already have published a reusable D/E.
  Preserve that ID and report the failure; do not silently repair logs or
  switch a Profile to hide it.
