import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(os.name == "posix", "build-release.sh runs on Linux")
class ReleaseChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.client = self.root / "cache" / "v0.0.1"
        self.client.mkdir(parents=True)
        for name in ("LiberNTUpdate.exe", "luupboot.exe"):
            (self.client / name).write_bytes(name.encode())
        (self.client / "SHA256SUMS").write_text("".join(
            f"{hashlib.sha256((self.client / name).read_bytes()).hexdigest()}  {name}\n"
            for name in ("LiberNTUpdate.exe", "luupboot.exe")))
        (self.client / "VERSION").write_text("v0.0.1\n")
        self.source = self.root / "src"
        self.source.mkdir()
        self.feed = self.root / "feed"
        self.feed.mkdir()
        (self.feed / "feed.json").write_text(json.dumps({"releases": [{"commit": "a" * 40, "client": "v0.0.1"}]}))
        (self.root / "signing.pub").write_text("test-public-key")
        self.log = self.root / "calls"
        self.env = dict(os.environ, SRC=str(self.source), FEED=str(self.feed), CLIENT_CACHE=str(self.root / "cache"),
                        KEY=str(self.root / "signing.key"), LUUP="test-publisher", FEED_PUSH="example:/feed/",
                        REAL_PYTHON=sys.executable, CALL_LOG=str(self.log), PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        self.executable("curl", "printf '%s\\n' '{\"tag_name\":\"v0.0.1\"}'\n")
        self.executable("git", 'if [ "$1" = status ] && [ "${DIRTY:-0}" = 1 ]; then echo " M changed.c"; fi\n'
                              'if [ "$1" = rev-parse ]; then echo aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa; fi\n')
        self.executable("python3", 'if [ "$1" = -c ]; then exec "$REAL_PYTHON" "$@"; fi\n'
                                  'echo "$2" >> "$CALL_LOG"\n'
                                  'if [ "$2" = verify ] && [ "${BAD_FEED:-0}" = 1 ]; then exit 7; fi\n')
        self.executable("rsync", 'echo PUSH >> "$CALL_LOG"\n')

    def executable(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def run_release(self):
        result = subprocess.run(["sh", str(Path(__file__).with_name("build-release.sh"))],
                                env=self.env, capture_output=True, text=True)
        calls = self.log.read_text().splitlines() if self.log.exists() else []
        return result, calls

    def test_already_published_feed_is_verified_before_push(self):
        result, calls = self.run_release()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(calls[0], "verify")
        self.assertIn("PUSH", calls)

    def test_failed_verification_blocks_the_next_already_published_push(self):
        self.env["BAD_FEED"] = "1"
        result, calls = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, ["verify"])

    def test_corrupt_cached_client_is_rejected_before_publication(self):
        (self.client / "LiberNTUpdate.exe").write_bytes(b"corrupt")
        result, calls = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(calls, [])

    def test_dirty_source_is_not_built_or_published(self):
        self.env["DIRTY"] = "1"
        result, calls = self.run_release()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source checkout is dirty", result.stderr)
        self.assertEqual(calls, [])


if __name__ == "__main__":
    unittest.main()
