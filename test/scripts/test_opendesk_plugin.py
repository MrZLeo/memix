"""Offline tests for the OpenDesk plugin package."""

import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PACKAGER = ROOT / "scripts/package_opendesk_plugin.sh"
MANIFEST = ROOT / "plugin/opendesk/opendesk.plugin.json"
ENTRY = ROOT / "plugin/opendesk/index.mjs"


class OpenDeskPluginTests(unittest.TestCase):
    def test_manifest_matches_memix_and_declares_static_skill(self):
        manifest = json.loads(MANIFEST.read_text())
        cmake = (ROOT / "CMakeLists.txt").read_text()
        version = re.search(r"project\(memix VERSION ([^ )]+)", cmake).group(1)
        self.assertEqual(manifest["version"], version)
        self.assertEqual(manifest["id"], "memix")
        self.assertEqual(manifest["capabilities"], ["skill", "hook"])
        self.assertEqual(manifest["resources"]["skills"], ["./skills/memix-cli"])
        self.assertEqual(manifest["platforms"], ["darwin", "linux", "win32"])

    def test_entry_is_valid_javascript(self):
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        subprocess.run([node, "--check", str(ENTRY)], check=True)

    def test_packager_creates_self_contained_universal_archive(self):
        with tempfile.TemporaryDirectory(prefix="memix-opendesk-test-") as temp:
            root = Path(temp)
            binaries = []
            for name in ("linux", "macos", "windows.exe"):
                binary = root / name
                binary.write_bytes((name + " fixture\n").encode())
                binaries.append(binary)
            archive = root / "memix-opendesk-plugin.tgz"
            subprocess.run(
                ["bash", str(PACKAGER), str(archive), *map(str, binaries)],
                check=True,
            )
            with tarfile.open(archive, "r:gz") as package:
                names = set(package.getnames())
                prefix = "memix-opendesk/"
                for expected in (
                    "opendesk.plugin.json",
                    "index.mjs",
                    "README.md",
                    "skills/memix-cli/SKILL.md",
                    "skills/memix-cli/agents/openai.yaml",
                    "bin/linux-x86_64/memix",
                    "bin/macos-arm64/memix",
                    "bin/windows-x86_64/memix.exe",
                ):
                    self.assertIn(prefix + expected, names)
                self.assertTrue(
                    package.getmember(prefix + "bin/linux-x86_64/memix").mode
                    & 0o111
                )
                self.assertTrue(
                    package.getmember(prefix + "bin/macos-arm64/memix").mode
                    & 0o111
                )
                extracted = root / "extracted"
                package.extractall(extracted)

            node = shutil.which("node")
            if node:
                plugin = extracted / "memix-opendesk"
                script = """
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const entry = await import(pathToFileURL(process.argv[1]));
const callbacks = new Map();
const options = new Map();
const disposed = [];
const returned = entry.default.setup({
  pluginDir: process.argv[2],
  hooks: {
    on(name, handler, hookOptions) {
      if (callbacks.has(name)) throw new Error(`duplicate hook: ${name}`);
      callbacks.set(name, handler);
      options.set(name, hookOptions);
      return { dispose() { disposed.push(name); } };
    }
  }
});
for (const name of ['shell.env', 'tool.definition', 'tool.execute.before']) {
  if (typeof callbacks.get(name) !== 'function') {
    throw new Error(`plugin did not register ${name}`);
  }
}
if (typeof returned !== 'function') {
  throw new Error('plugin did not return a disposer');
}
const output = { env: { PATH: '/usr/bin' } };
callbacks.get('shell.env')({}, output);
const expected = path.dirname(entry.bundledBinary(process.argv[2]));
if (output.env.PATH.split(path.delimiter)[0] !== expected) {
  throw new Error(`bundled CLI was not prepended: ${output.env.PATH}`);
}

for (const [tool, workflow] of [
  ['memory-addMemory', 'save'],
  ['memory-updateMemory', 'save'],
  ['memory-removeMemory', 'save'],
  ['memory-searchMemory', 'recall'],
  ['memory-getMemory', 'recall']
]) {
  const definition = { description: 'native description' };
  callbacks.get('tool.definition')({ tool }, definition);
  if (!definition.description.includes('pluginMgr/memix-cli') ||
      !definition.description.includes(`memix agent ${workflow}`) ||
      definition.description.includes('native description')) {
    throw new Error(`native memory definition was not redirected: ${tool}`);
  }

  const execution = {};
  callbacks.get('tool.execute.before')({ tool }, execution);
  if (!execution.deny?.message?.includes('was not read or changed') ||
      execution.deny?.metadata?.policy !== 'memix-memory-authority' ||
      execution.deny?.metadata?.workflow !== `memix agent ${workflow}`) {
    throw new Error(`native memory execution was not denied: ${tool}`);
  }
}

for (const tool of ['memory-updateUserPortrait', 'files-read']) {
  const definition = { description: 'unchanged' };
  callbacks.get('tool.definition')({ tool }, definition);
  if (definition.description !== 'unchanged') {
    throw new Error(`unrelated tool definition changed: ${tool}`);
  }
  const execution = {};
  callbacks.get('tool.execute.before')({ tool }, execution);
  if (execution.deny) throw new Error(`unrelated tool was denied: ${tool}`);
}

if (options.get('tool.execute.before')?.failureMode !== 'closed') {
  throw new Error('memory execution guard is not fail-closed');
}
await returned();
if (disposed.join(',') !== 'tool.execute.before,tool.definition,shell.env') {
  throw new Error(`unexpected disposal order: ${disposed.join(',')}`);
}
"""
                subprocess.run(
                    [
                        node,
                        "--input-type=module",
                        "--eval",
                        script,
                        str(plugin / "index.mjs"),
                        str(plugin),
                    ],
                    check=True,
                )


if __name__ == "__main__":
    unittest.main()
