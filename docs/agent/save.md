# Save and update

A clear request to remember or correct durable information authorizes that save.
Save only the requested material, not incidental secrets or an unsolicited
conversation dump.

1. Choose scope and type: `rules` for standing instructions, `chat` for
   conversation records, `documents` for notes, facts, and decisions. Follow
   established paths; ask if user/project scope or meaning is ambiguous.
2. Inspect the relevant logical tree with `memix store tree` and read an existing
   entry with `cat`. For corrections, reuse its logical path and preserve
   unrelated content instead of creating competing current copies.
3. Prepare the file outside the Store. Preserve necessary attribution, date,
   and applicability; distinguish confirmed decisions from assistant inferences.
4. Ingest one file with `--dest`, then verify the saved content using the returned
   full source generation ID:

   ```text
   memix store ingest --type documents --dest projects/example/decisions.md decisions.md
   memix store cat --type documents --generation <full-generation-id> projects/example/decisions.md
   ```

`--dest` is relative within the type. Each ingest retains other logical entries.
Without `--dest`, ingest creates raw blobs only, not normally retrievable logical
memory. Never edit Store objects or attachment files.

For an authorized retraction, `memix store rm --type <type> <path>` removes the
file or subtree from the latest logical tree in a new generation. Blobs, history,
and pinned results remain readable: this is not physical deletion or complete
forgetting.

Report the saved/retracted path and verification result. Saving does not update
existing Derivations, Environments, or attachments. Load `memix agent compose`
only if the task also needs updated reusable results, then `memix agent attach`
only if activation is needed.
