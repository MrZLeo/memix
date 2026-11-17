# Memix for OpenDesk

This plugin bundles the Memix CLI and the `memix-cli` skill. Installing the
plugin is sufficient: its `shell.env` hook prepends the matching bundled binary
to terminal commands run by OpenDesk, while `resources.skills` exposes the
version-matched skill without a separate import.

While the plugin is active, Memix is the authoritative backend for durable
memory. The plugin rewrites the model-visible definitions of OpenDesk's native
`addMemory`, `updateMemory`, `removeMemory`, `searchMemory`, and `getMemory`
tools, and rejects any native call that still reaches execution. The rejection
directs the agent to load `pluginMgr/memix-cli` and follow the version-matched
`memix agent save` or `memix agent recall` workflow. This prevents a request
from being silently written to or read from `memory.json` instead of Memix.
OpenDesk's separate user-portrait feature is not intercepted.

Install the release archive from OpenDesk's plugin center or with:

```sh
opendesk plugins install ./memix-opendesk-plugin.tgz
```

The archive supports macOS arm64, Linux x86_64, and Windows x86_64. It requests
only the `skill` and `hook` capabilities. The hook changes `PATH` only for
OpenDesk shell executions; it does not edit shell profiles, install a system
binary, migrate existing OpenDesk memories, or change Memix Stores. Disable the
plugin before intentionally using OpenDesk's native key-fact memory again.

Developers should build all three release binaries and run
`scripts/package_opendesk_plugin.sh`; the source directory alone intentionally
does not duplicate the canonical skill or checked-in binaries.
