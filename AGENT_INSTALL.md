# Install Memix with an agent

Use the interactive installer instead of reproducing its platform detection,
download, and skill-installation logic. Do not build from source.

For OpenDesk 0.4.1+, prefer the single plugin installation below. It installs
both the skill and its version-matched CLI and does not require the two-step
installer flow:

```sh
opendesk plugins install https://github.com/MrZLeo/memix/releases/latest/download/memix-opendesk-plugin.tgz
```

After installation, reload plugins if OpenDesk has not done so automatically,
then verify the `memix` plugin is active. The plugin's hook exposes its bundled
CLI only inside OpenDesk shell executions; it does not modify the account PATH.
While active, its tool hooks redirect OpenDesk's native key-fact memory saves,
reads, updates, and removals to the `memix-cli` skill. It does not migrate
existing `memory.json` entries, and it leaves the OpenDesk user portrait alone.

## 1. Run the installer

Use a terminal where the **user** can answer both `[Y/n]` prompts:

**Linux / macOS**

```sh
curl -fsSL https://github.com/MrZLeo/memix/releases/latest/download/install.sh | bash
```

**Windows (PowerShell)**

```powershell
irm https://github.com/MrZLeo/memix/releases/latest/download/install.ps1 | iex
```

If your tools cannot provide an interactive terminal, ask the user to run the
command. Never pipe answers or approve the prompts on their behalf.

The installer selects a supported release binary, verifies its SHA-256 checksum
and `skill` support, then previews and separately confirms `memix skill setup`.
Default: latest release, `~/.local/bin`, detected agents, user scope. Declining
binary installation can still use an existing Memix for skill setup. Follow any
PATH notice; the installer does not edit shell profiles or environment settings.

For a specific harness, download the script first and pass `--agent pi` (Bash)
or `-Agent pi` (PowerShell). Supported IDs: `claude`, `codex`, `opencode`, `pi`,
`dsh`, `opendesk`. Use `--project` / `-Project` only when project scope was
requested; OpenDesk does not support project-scoped setup.
See the scripts' `--help` / `-Help` for a pinned release or custom binary directory.
Private releases require an authenticated `gh` client to download the installer
and its assets. Releases without `SHA256SUMS` are not supported; do not bypass
verification or silently substitute an older binary.

## 2. Verify and load instructions

After installation, run `memix --version` and `memix skill status --json`.
Confirm the intended paths are `current`; status exit code zero alone does not
prove a skill was installed. Report any skipped steps or partial failures.
Foreign skills require separate review and explicit approval before `--force`;
never retry with it automatically.

Run `memix skill show` in this session, then read only the relevant
`memix agent <topic>` guide. Reload the harness if necessary (Pi: `/reload`).
Report the binary version/path, skill paths, and verification results.

To uninstall skill files later, use `memix skill remove` with matching agent and
scope options. No hooks, agent settings, memory Stores, or Profile history are
installed or removed by skill setup/removal.
