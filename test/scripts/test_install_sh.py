"""Offline installer tests. All downloads/agents live in a temporary sandbox.

Run with: python3 -m unittest discover -s test/scripts -p 'test_*.py' -v
The PTY tests also run against macOS's Bash 3.2; no third-party packages needed.
"""

import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import shutil
import subprocess
import sys
import tempfile
import termios
import time
import unittest


INSTALLER = Path(__file__).resolve().parents[2] / "install.sh"

# A single fake executable implements the downloader, platform probes, and the
# downloaded binary. Verification uses real sha256sum/shasum and real file I/O.
FAKE_TOOL = r'''
import hashlib
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ['CALL_LOG'], 'a') as log:
    log.write(json.dumps([name, *args]) + '\n')
if name == 'uname':
    print(os.environ.get('FAKE_OS', 'Linux') if args == ['-s'] else os.environ.get('FAKE_ARCH', 'x86_64'))
elif name == 'sw_vers':
    print(os.environ.get('FAKE_MACOS', '14.0'))
elif name == 'sysctl':
    print(os.environ.get('FAKE_ROSETTA', '0'))
elif name in ('curl', 'gh'):
    if name == 'gh' and args[0] == 'auth':
        sys.exit(0 if os.environ.get('FAKE_GH') == '1' else 1)
    if name == 'gh' and args[:2] == ['release', 'view']:
        print('v0.7.0')
        sys.exit()
    if name == 'curl' and args[-1].endswith('/releases/latest'):
        print('https://github.com/MrZLeo/memix/releases/tag/v0.7.0', end='')
        sys.exit()
    asset = args[args.index('--pattern') + 1] if name == 'gh' else args[-1].rsplit('/', 1)[-1]
    if os.environ.get('FAKE_FAIL_DOWNLOAD') == asset:
        sys.exit(1)
    output = Path(args[args.index('--output') + 1])
    fixture = Path(os.environ['FIXTURE']).read_bytes()
    if asset == 'SHA256SUMS':
        digest = hashlib.sha256(fixture).hexdigest()
        if os.environ.get('FAKE_BAD_CHECKSUM'):
            digest = '0' * 64
        if os.environ.get('FAKE_MALFORMED_CHECKSUM'):
            digest = 'not-a-checksum'
        manifest = ''.join(digest + '  memix-' + platform + '\n' for platform in ('linux-x86_64', 'macos-arm64'))
        if os.environ.get('FAKE_DUPLICATE_CHECKSUM'):
            manifest += manifest
        output.write_text(manifest)
    else:
        output.write_bytes(fixture)
else:
    if args == ['--version']:
        if os.environ.get('FAKE_BAD_BINARY'):
            sys.exit(1)
        print('memix 0.7.0')
    elif args == ['skill', '--help']:
        sys.exit(1 if os.environ.get('FAKE_OLD_BINARY') else 0)
    elif args[:2] == ['skill', 'setup']:
        if '--dry-run' in args:
            print('Preview only; no files were changed.')
            sys.exit(1 if os.environ.get('FAKE_PREVIEW_FAILURE') else 0)
        Path(os.environ['SETUP_MARKER']).write_text('setup')
        sys.exit(1 if os.environ.get('FAKE_SETUP_FAILURE') else 0)
    else:
        sys.exit(2)
'''


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="memix-install-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.home = self.root / "home"
        self.home.mkdir()
        self.tools = self.root / "tools"
        self.tools.mkdir()
        self.bin_dir = self.home / "bin with spaces"
        self.destination = self.bin_dir / "memix"
        self.log = self.root / "calls.jsonl"
        self.marker = self.root / "setup-ran"
        self.fixture = self.root / "fixture"
        self.fixture.write_text(f"#!{sys.executable}\n" + FAKE_TOOL)
        self.fixture.chmod(0o755)
        for name in ("uname", "curl", "gh", "sw_vers", "sysctl"):
            (self.tools / name).symlink_to(self.fixture)
        for name in ("mkdir", "mktemp", "rm", "mv", "chmod", "awk", "sha256sum", "shasum"):
            tool = shutil.which(name)
            if tool:
                (self.tools / name).symlink_to(tool)
        self.env = dict(os.environ, HOME=str(self.home), PATH=str(self.tools),
                        CALL_LOG=str(self.log), FIXTURE=str(self.fixture), SETUP_MARKER=str(self.marker))
        for key in list(self.env):
            if key.startswith("FAKE_"):
                del self.env[key]
        self.bash = os.environ.get("TEST_BASH", "/bin/bash")

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def existing_binary(self):
        self.bin_dir.mkdir()
        shutil.copy2(self.fixture, self.destination)
        with self.destination.open("a") as output:
            output.write("\n# Previous installation\n")
        return self.destination.read_bytes()

    def run_installer(self, answers=(), extra=(), piped_script=False, tty=True):
        args = ["--bin-dir", str(self.bin_dir), *extra]
        if not tty:
            result = subprocess.run([self.bash, str(INSTALLER), *args], input="y\ny\n",
                                    text=True, capture_output=True, env=self.env, start_new_session=True)
            return result.returncode, result.stdout + result.stderr
        master, slave = pty.openpty()

        def terminal_session():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        script = None
        if piped_script:
            script = INSTALLER.open("rb")
            command = [self.bash, "-s", "--", *args]
        else:
            command = [self.bash, str(INSTALLER), *args]
        process = subprocess.Popen(command, stdin=script if script else slave, stdout=slave,
                                   stderr=slave, env=self.env, preexec_fn=terminal_session)
        os.close(slave)
        if script:
            script.close()
        output = b""
        sent = 0
        deadline = time.monotonic() + 15
        try:
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        break
                    if not chunk:
                        break
                    output += chunk
                    # Feed answers only after the installer actually asks.
                    while sent < output.count(b"[Y/n] "):
                        self.assertLess(sent, len(answers), output.decode(errors="replace"))
                        answer = answers[sent]
                        os.write(master, b"\x04" if answer is None else (answer + "\n").encode())
                        sent += 1
                elif process.poll() is not None:
                    break
            else:
                self.fail("installer timed out:\n" + output.decode(errors="replace"))
            return process.wait(timeout=5), output.decode(errors="replace").replace("\r\n", "\n")
        finally:
            if process.poll() is None:
                process.kill()
            process.wait()
            os.close(master)

    def assert_clean_stage(self):
        self.assertFalse(list(self.bin_dir.glob(".memix-install.*")))

    def test_default_yes_installs_and_sets_up_after_two_confirmations(self):
        code, output = self.run_installer(["", ""])
        self.assertEqual(code, 0, output)
        self.assertEqual(output.count("[Y/n] "), 2)
        self.assertEqual(self.destination.read_bytes(), self.fixture.read_bytes())
        self.assertTrue(os.access(self.destination, os.X_OK))
        self.assertTrue(self.marker.exists())
        downloads = [call[-1] for call in self.calls() if call[0] == "curl"]
        self.assertTrue(any('/download/v0.7.0/SHA256SUMS' in url for url in downloads))
        self.assertTrue(any('/download/v0.7.0/memix-linux-x86_64' in url for url in downloads))
        self.assert_clean_stage()
        self.assertFalse((self.home / ".profile").exists())

    def test_no_binary_and_decline_does_not_download_or_setup(self):
        code, output = self.run_installer(["n"])
        self.assertEqual(code, 0, output)
        self.assertFalse(self.bin_dir.exists())
        self.assertFalse(self.marker.exists())
        self.assertFalse(any('/download/' in part for call in self.calls() for part in call))

    def test_decline_update_reuses_existing_binary_and_confirms_setup(self):
        old = self.existing_binary()
        (self.tools / "curl").unlink()
        (self.tools / "gh").unlink()
        code, output = self.run_installer(["n", "y"])
        self.assertEqual(code, 0, output)
        self.assertEqual(self.destination.read_bytes(), old)
        self.assertTrue(self.marker.exists())
        self.assertFalse(any('/download/' in part for call in self.calls() for part in call))

    def test_decline_both_steps_preserves_existing_install(self):
        old = self.existing_binary()
        code, output = self.run_installer(["n", "n"])
        self.assertEqual(code, 0, output)
        self.assertEqual(self.destination.read_bytes(), old)
        self.assertFalse(self.marker.exists())

    def test_install_but_decline_setup(self):
        code, output = self.run_installer(["y", "n"])
        self.assertEqual(code, 0, output)
        self.assertTrue(self.destination.exists())
        self.assertFalse(self.marker.exists())

    def test_eof_is_not_consent(self):
        code, output = self.run_installer([None])
        self.assertNotEqual(code, 0, output)
        self.assertIn("EOF", output)
        self.assertFalse(self.destination.exists())
        self.assertFalse(self.marker.exists())

    def test_eof_at_setup_preserves_binary_but_never_sets_up(self):
        code, output = self.run_installer(["y", None])
        self.assertNotEqual(code, 0, output)
        self.assertTrue(self.destination.exists())
        self.assertFalse(self.marker.exists())
        self.assert_clean_stage()

    def test_invalid_answer_reprompts(self):
        code, output = self.run_installer(["maybe", "n"])
        self.assertEqual(code, 0, output)
        self.assertIn("Please answer y or n", output)
        self.assertFalse(self.destination.exists())

    def test_noninteractive_input_cannot_confirm(self):
        code, output = self.run_installer(tty=False)
        self.assertNotEqual(code, 0, output)
        self.assertIn("interactive terminal", output)
        self.assertFalse(self.bin_dir.exists())

    def test_piped_script_uses_terminal_for_answers(self):
        code, output = self.run_installer(["y", "n"], piped_script=True)
        self.assertEqual(code, 0, output)
        self.assertTrue(self.destination.exists())
        self.assertFalse(self.marker.exists())

    def test_failed_download_hash_or_binary_never_replaces_existing_install(self):
        old = self.existing_binary()
        failures = [
            ("FAKE_FAIL_DOWNLOAD", "SHA256SUMS"),
            ("FAKE_FAIL_DOWNLOAD", "memix-linux-x86_64"),
            ("FAKE_BAD_CHECKSUM", "1"),
            ("FAKE_MALFORMED_CHECKSUM", "1"),
            ("FAKE_DUPLICATE_CHECKSUM", "1"),
            ("FAKE_BAD_BINARY", "1"),
            ("FAKE_OLD_BINARY", "1"),
        ]
        for variable, value in failures:
            with self.subTest(variable=variable, value=value):
                self.env[variable] = value
                code, output = self.run_installer(["y"])
                self.assertNotEqual(code, 0, output)
                self.assertEqual(self.destination.read_bytes(), old)
                self.assertFalse(self.marker.exists())
                self.assert_clean_stage()
                del self.env[variable]

    def test_symlinks_and_directories_are_not_replaced(self):
        self.bin_dir.mkdir()
        self.destination.symlink_to(self.fixture)
        code, output = self.run_installer()
        self.assertNotEqual(code, 0, output)
        self.assertTrue(self.destination.is_symlink())
        self.destination.unlink()
        self.destination.mkdir()
        code, output = self.run_installer()
        self.assertNotEqual(code, 0, output)
        self.assertTrue(self.destination.is_dir())

    def test_supported_macos_and_rosetta(self):
        self.env.update(FAKE_OS="Darwin", FAKE_ARCH="arm64")
        code, output = self.run_installer(["y", "n"])
        self.assertEqual(code, 0, output)
        self.assertTrue(any('memix-macos-arm64' in part for call in self.calls() for part in call))
        self.env.update(FAKE_ARCH="x86_64", FAKE_ROSETTA="1")
        code, output = self.run_installer(["y", "n"])
        self.assertEqual(code, 0, output)

    def test_unsupported_platforms_stop_without_prompt_or_download(self):
        for system, arch in [("Linux", "aarch64"), ("Darwin", "x86_64"), ("FreeBSD", "x86_64"), ("MINGW64_NT", "x86_64")]:
            with self.subTest(system=system, arch=arch):
                self.env.update(FAKE_OS=system, FAKE_ARCH=arch)
                code, output = self.run_installer()
                self.assertNotEqual(code, 0, output)
                self.assertFalse(self.bin_dir.exists())
                self.assertFalse(any(call[0] in ('curl', 'gh') for call in self.calls()))
        self.env.update(FAKE_OS="Darwin", FAKE_ARCH="arm64", FAKE_MACOS="13.7")
        code, output = self.run_installer()
        self.assertNotEqual(code, 0, output)
        self.assertIn("macOS 14", output)

    def test_authenticated_gh_and_pinned_release(self):
        self.env["FAKE_GH"] = "1"
        code, output = self.run_installer(["y", "n"], ["--version", "v0.7.0"])
        self.assertEqual(code, 0, output)
        self.assertFalse(any(call[0] == "curl" for call in self.calls()))
        downloads = [call for call in self.calls() if call[0:3] == ["gh", "release", "download"]]
        self.assertEqual(len(downloads), 2)
        self.assertTrue(all(call[3] == "v0.7.0" for call in downloads))

    def test_setup_options_are_identical_in_preview_and_apply_and_never_force(self):
        extra = ["--agent", "pi", "--agent", "codex", "--project"]
        code, output = self.run_installer(["y", "y"], extra)
        self.assertEqual(code, 0, output)
        calls = [call[1:] for call in self.calls() if call[1:3] == ["skill", "setup"]]
        self.assertEqual(calls, [["skill", "setup", "--dry-run", *extra], ["skill", "setup", *extra]])
        self.assertNotIn("--force", [part for call in self.calls() for part in call])

    def test_partial_setup_failure_is_reported_without_removing_binary(self):
        self.env.update(FAKE_PREVIEW_FAILURE="1", FAKE_SETUP_FAILURE="1")
        code, output = self.run_installer(["y", "y"])
        self.assertNotEqual(code, 0, output)
        self.assertIn("incomplete", output)
        self.assertTrue(self.destination.exists())
        self.assert_clean_stage()

    @unittest.skipUnless(os.environ.get("MEMIX_TEST_BINARY"), "set MEMIX_TEST_BINARY for a real CLI smoke test")
    def test_real_cli_installs_only_inside_sandbox(self):
        self.env["FIXTURE"] = str(Path(os.environ["MEMIX_TEST_BINARY"]).resolve())
        for variable in ("CODEX_HOME", "CLAUDE_CONFIG_DIR", "PI_CODING_AGENT_DIR",
                         "DSH_HOME", "XDG_CONFIG_HOME", "MEMIX_STORE_ROOT"):
            self.env.pop(variable, None)
        code, output = self.run_installer(["y", "y"], ["--agent", "pi"])
        self.assertEqual(code, 0, output)
        self.assertTrue((self.home / ".agents/skills/memix-cli/SKILL.md").exists())
        self.assertTrue((self.home / ".agents/skills/memix-cli/agents/openai.yaml").exists())
        self.assertEqual({path.name for path in self.home.iterdir()}, {"bin with spaces", ".agents"})
        self.assert_clean_stage()

    def test_help_and_invalid_arguments_need_no_terminal(self):
        code, output = self.run_installer(extra=["--help"], tty=False)
        self.assertEqual(code, 0, output)
        for extra in (["--yes"], ["--agent", "unknown"], ["--version"], ["--bin-dir", "relative"]):
            with self.subTest(extra=extra):
                code, output = self.run_installer(extra=extra, tty=False)
                self.assertNotEqual(code, 0, output)
        self.assertFalse(self.bin_dir.exists())


if __name__ == "__main__":
    unittest.main()
