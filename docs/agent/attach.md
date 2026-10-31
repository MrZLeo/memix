# Attach an Environment

Activate only when a workspace attachment is needed and authorized, not for a
one-off recall. Only Environments can be activated; for a Derivation, first load
`memix agent compose`.

1. Honor the supplied attachment path (default `./.memory`). Inspect
   `memix view status --into <path>` before replacing existing context.
2. Keep the workspace-specific default Profile unless sharing is intentional.
   A Profile has a mutable current pointer and numbered activation generations.
3. Activate the intended Environment and verify the attachment:

   ```text
   memix view activate <environment-id> --into ./.memory
   memix view status --into ./.memory
   ```

4. Retain the Environment ID, Profile generation number, and attachment path.
   Read its frozen `README.md` and relevant payload before claiming to have
   read the memory. Mounting alone does not load content into the model.

## Sharing and modes

Default `--mode link` creates a symlink following the Profile's current
generation. `--mode copy` creates disposable files in one workspace; other
copies do not follow subsequent Profile changes. Never edit either kind as
source memory.

Use `--profile NAME` only for intentional sharing: switching that Profile can
change every linked workspace. Check those effects and ask if they exceed the
requested scope. Keep the same `--profile` and `--into` for related operations.

Copy-mode `status` staleness means the attachment trails the Profile, not that
source refs changed. For refresh or repair, load `memix agent recovery` and
preserve copy mode; omitting it selects link mode.

Do not bypass foreign-path protection or move blocking user files aside.
Load `memix agent recovery` before forced replacement, rollback, deactivation,
or lock cleanup. Cancellation leaves the attachment and Profile unchanged.
