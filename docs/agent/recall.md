# Recall and discover

## Start with what is known

- **Attachment:** read its `README.md`, then relevant payload files. Honor the
  supplied path (default `./.memory`) and its frozen guidance, not newer catalog
  annotations. Do not switch attachments just to read memory.
- **Source path:** use `memix store cat --type <type> <path>`; for a historical
  version, add `--generation <full-generation-id>`, not a generation number.
- **Derivation or Environment:** use the typed locator `derivation:ID` or
  `environment:ID`. Preserve that snapshot; bare IDs mean Environment.
- **Only a topic:** discover candidates below.

## Discover, read, stop

1. Search `memix view find <term> --json` using names, topics, or code symbols.
   This matches name/intent substrings, not payloads or vectors. Multiple terms
   must all match; try alternative names separately. `--covers <type/path>`
   finds direct source selectors, not transitive composition contents.
   Browse `memix view quests --json` only when needed.
2. Retain each candidate's `latest_result: {kind, id}`. If absent, use
   `memix view resolve <quest> --json` only when materialization fits the request.
   For an existing result whose current version is wanted, check
   `memix view freshness <quest> --json`; refresh only when authorized.
   Freshness requires an existing resolution. A composition pins its
   Derivations: `stale: false` does not mean latest sources. Updating those
   bindings requires the compose workflow, not repeated resolve.
3. Read plausible candidates, retaining full IDs and their kinds:

   ```text
   memix view tree <kind>:<id> --depth 2 --json
   memix view cat <kind>:<id> <output-path>
   ```

   Use `kind` from the result and paths from the tree. Read payloads, not just
   metadata; expand relevant paths without rereading shared files.
4. If metadata is insufficient and current-source exploration is allowed,
   search a scoped subtree of `<store-root>/latest/<type>/<path>` with ordinary
   text tools. Inspect `memix store tree --all --depth 2 --json` if necessary.
   Projection hits map to the same type/path for `store cat`. Binary documents
   need format-specific extraction. An empty metadata search does not prove
   memory is absent.

Stop when evidence is sufficient, scoped leads run out, or the task budget is
reached. Report relevant sources, versions, conflicts, and gaps; do not claim
exhaustive absence. Check dates and applicability before applying old decisions.

Only for reusable selections, load `memix agent compose`; only for workspace
attachments, load `memix agent attach`. Load `memix agent recovery` on failure.
