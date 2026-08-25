import json
import contextlib
import io
import plistlib
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

from package_plist_results import main
from package_plist_results import replacement_rules
from verify_release_archives import verify_archive


def write_report(path: Path, source: str) -> None:
    document = {
        "files": [source],
        "diagnostics": [{
            "check_name": "cplusplus.NewDeleteLeaks",
            "description": "Potential memory leak",
            "location": {"file": 0, "line": 4, "col": 2},
            "path": [],
        }],
    }
    with path.open("wb") as stream:
        plistlib.dump(document, stream)


class PackageTests(unittest.TestCase):
    def test_archives_are_sanitized_and_deterministic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            oh = root / "oh"
            android = root / "android"
            oh.mkdir()
            android.mkdir()
            write_report(oh / "oh.plist", "/home/reviewer/oh/src/a.cpp")
            write_report(android / "android.plist", "/home/reviewer/aosp/src/b.cpp")
            metadata = {
                "tools": [{
                    "working_directory": "/home/reviewer/aosp",
                    "action_num": 1,
                    "command": ["CodeChecker", "analyze"],
                    "result_source_files": {
                        "/home/reviewer/aosp/results/android.plist":
                        "/home/reviewer/aosp/src/b.cpp"
                    },
                }]
            }
            (android / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")

            manifests = []
            for name in ("out1", "out2"):
                output = root / name
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(main([
                        "--openharmony-dir", str(oh),
                        "--android-dir", str(android),
                        "--output-dir", str(output),
                        "--openharmony-source-root", "/home/reviewer/oh",
                        "--android-source-root", "/home/reviewer/aosp",
                        "--private-home", "/home/reviewer",
                        "--compression-level", "1",
                    ]), 0)
                manifests.append(json.loads((output / "manifest.json").read_text(encoding="utf-8")))
            self.assertEqual(manifests[0], manifests[1])

            archive = root / "out1" / "android-ours-plists.tar.zst"
            process = subprocess.Popen(["zstd", "-d", "-q", "-c", str(archive)], stdout=subprocess.PIPE)
            assert process.stdout is not None
            with tarfile.open(fileobj=process.stdout, mode="r|") as stream:
                payloads = [member_file.read() for member in stream if (member_file := stream.extractfile(member))]
            process.stdout.close()
            self.assertEqual(process.wait(), 0)
            joined = b"\n".join(payloads)
            self.assertIn(b"/artifact/android", joined)
            self.assertNotIn(b"reviewer", joined)

            verified = verify_archive(
                dataset="Android",
                archive=archive,
                archive_root="android/reports_exp2_64",
                source_dir=android,
                rules=replacement_rules(
                    "/home/reviewer/aosp", "/artifact/android", [], "/home/reviewer"
                ),
                jobs=2,
                zstd="zstd",
            )
            self.assertEqual(verified["plist_files"], 1)
            self.assertEqual(verified["metadata_files"], 1)
            self.assertTrue(verified["byte_exact_sanitized_source_match"])


if __name__ == "__main__":
    unittest.main()
