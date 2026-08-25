import unittest
import json
import plistlib
import tempfile
from pathlib import Path

from real_project_evidence import (
    Location,
    Report,
    analyze_dataset,
    assignment_is_ambiguous,
    android_action_groups,
    android_ours_action_groups,
    canonicalize_compile_command,
    checker_is_in_scope,
    classify_kind,
    duplicate_group_requires_audit,
    hungarian,
    overall_class,
    path_cost,
    parse_large_plist_reports_streaming,
    parse_plist_reports,
    summarize_ours_only,
    split_issue_groups,
    transition,
)


def report(side, *, allocation=False, release=False, branch=False, skeleton=("a",), issue_hash="h"):
    return Report(
        dataset="Synthetic",
        side=side,
        action_id="action",
        plist_name=f"{side}.plist",
        diagnostic_index=0,
        checker="cplusplus.NewDeleteLeaks",
        kind="leak",
        description="Potential memory leak",
        normalized_description="potential memory leak",
        issue_hash=issue_hash,
        terminal=Location("source.cpp", 10, 3),
        allocation_locations=("source.cpp:1:1",) if allocation else (),
        release_locations=("source.cpp:5:1",) if release else (),
        branch_locations=("source.cpp:7:1",) if branch else (),
        skeleton=tuple(skeleton),
        path_events=len(skeleton),
    )


class EvidenceTests(unittest.TestCase):
    def test_checker_prefix_scope(self):
        self.assertTrue(checker_is_in_scope("cplusplus.NewDelete"))
        self.assertTrue(checker_is_in_scope("core.NullDereference"))
        self.assertTrue(checker_is_in_scope("unix.Malloc"))
        self.assertFalse(checker_is_in_scope("deadcode.DeadStores"))

    def test_non_lifecycle_checker_is_not_incomplete(self):
        item = report("baseline")
        item.checker = "core.NullDereference"
        item.kind = "other"
        self.assertFalse(item.lifecycle_applicable)
        self.assertIsNone(item.lifecycle_complete())

    def test_lifecycle_definitions(self):
        leak = report("ours", allocation=True)
        self.assertTrue(leak.lifecycle_complete())
        leak.allocation_locations = ()
        self.assertFalse(leak.lifecycle_complete())
        uaf = report("ours", allocation=True, release=True)
        uaf.checker = "cplusplus.NewDelete"
        uaf.kind = "uaf"
        self.assertTrue(uaf.lifecycle_complete())
        uaf.release_locations = ()
        self.assertFalse(uaf.lifecycle_complete())

    def test_delete_released_memory_is_double_free(self):
        self.assertEqual(
            classify_kind(
                "cplusplus.NewDelete", "Attempt to delete released memory"
            ),
            "double_free",
        )

    def test_gained_allocation_is_improved(self):
        before = report("baseline")
        after = report("ours", allocation=True)
        self.assertEqual(transition(before.has("allocation"), after.has("allocation")), "gained")
        self.assertEqual(overall_class(before, after), "improved")

    def test_evidence_does_not_affect_path_cost(self):
        before = report("baseline", skeleton=("call", "control"))
        after = report("ours", allocation=True, release=True, branch=True, skeleton=("call", "control"))
        self.assertEqual(path_cost(before, after), 0)

    def test_same_terminal_relaxed_hash_match(self):
        groups = split_issue_groups(
            "Synthetic", "action", [report("baseline", issue_hash="a")], [report("ours", issue_hash="b")]
        )
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0].status, "common")
        self.assertEqual(groups[0].match_tier, "terminal+unique-relaxed")

    def test_unmatched_hash_variant_is_side_only_not_ambiguous(self):
        baseline_common = report("baseline", issue_hash="shared")
        baseline_extra = report("baseline", issue_hash="baseline-extra")
        baseline_extra.diagnostic_index = 1
        ours_common = report("ours", issue_hash="shared")
        groups = split_issue_groups(
            "Synthetic", "action", [baseline_common, baseline_extra], [ours_common]
        )
        self.assertEqual([group.status for group in groups], ["common", "baseline_only"])

    def test_different_terminal_is_not_matched_by_evidence(self):
        baseline = report("baseline", allocation=True)
        ours = report("ours", allocation=True)
        ours.terminal = Location("source.cpp", 11, 3)
        groups = split_issue_groups("Synthetic", "action", [baseline], [ours])
        self.assertEqual({group.status for group in groups}, {"baseline_only", "ours_only"})

    def test_equal_cost_duplicate_paths_are_ambiguous(self):
        cost = [[0, 0], [0, 0]]
        optimum, pairs = hungarian(cost)
        self.assertTrue(assignment_is_ambiguous(cost, optimum, pairs))

    def test_large_duplicate_group_is_reserved_for_audit(self):
        baseline = []
        ours = []
        for index in range(21):
            left = report("baseline", skeleton=(f"b{index}",))
            left.diagnostic_index = index
            baseline.append(left)
            right = report("ours", skeleton=(f"o{index}",))
            right.diagnostic_index = index
            ours.append(right)
        self.assertTrue(duplicate_group_requires_audit(baseline, ours))

    def test_long_path_pair_is_reserved_for_audit(self):
        long_skeleton = ("event",) * 50_001
        baseline = report("baseline", skeleton=long_skeleton)
        ours = report("ours", skeleton=long_skeleton)
        self.assertTrue(duplicate_group_requires_audit([baseline], [ours]))

    def test_compile_identity_retains_semantic_flags(self):
        base = {
            "directory": "/work/aosp",
            "file": "src/a.cpp",
            "command": "/tool/clang++ -target x86_64 -std=c++17 -DVALUE=1 -Iinc -o raw/a.o src/a.cpp",
        }
        same = {
            "directory": "/work/aosp",
            "file": "src/a.cpp",
            "command": "/other/clang++ -target x86_64 -std=c++17 -DVALUE=1 -Iinc -o exp/a.o src/a.cpp",
        }
        different = {
            "directory": "/work/aosp",
            "file": "src/a.cpp",
            "command": "/other/clang++ -target arm64 -std=c++17 -DVALUE=1 -Iinc -o exp/a.o src/a.cpp",
        }
        self.assertEqual(canonicalize_compile_command(base), canonicalize_compile_command(same))
        self.assertNotEqual(canonicalize_compile_command(base), canonicalize_compile_command(different))

    def test_android_metadata_pairing_reports_missing_plist(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = root / "baseline"
            ours = root / "ours"
            baseline.mkdir()
            ours.mkdir()
            (baseline / "a.plist").write_text("empty", encoding="utf-8")
            (ours / "b.plist").write_text("empty", encoding="utf-8")
            command = [
                "CodeChecker", "analyze", "db.json", "--enable", "cplusplus",
                "--enable", "core", "--enable", "unix",
            ]
            baseline_meta = {
                "tools": [{
                    "working_directory": "/work/aosp",
                    "action_num": 2,
                    "command": command,
                    "result_source_files": {
                        "/work/aosp/baseline/a.plist": "/work/aosp/src/a.cpp"
                    },
                }]
            }
            ours_meta = {
                "tools": [{
                    "working_directory": "/work/aosp",
                    "action_num": 3,
                    "command": command,
                    "result_source_files": {
                        "/work/aosp/ours/b.plist": "/work/aosp/src/a.cpp",
                        "/work/aosp/ours/missing.plist": "/work/aosp/src/missing.cpp",
                    },
                }]
            }
            (baseline / "metadata.json").write_text(json.dumps(baseline_meta), encoding="utf-8")
            (ours / "metadata.json").write_text(json.dumps(ours_meta), encoding="utf-8")
            actions, metadata = android_action_groups(baseline, ours)
            self.assertEqual(len(actions), 1)
            self.assertEqual(actions[0].status, "paired")
            self.assertEqual(metadata["ours_missing_result_plists"], 1)

    def test_streaming_parser_matches_standard_parser(self):
        document = {
            "files": ["/work/aosp/src/a.cpp"],
            "diagnostics": [{
                "check_name": "cplusplus.NewDelete",
                "description": "Use of memory after it is freed",
                "issue_hash_content_of_line_in_context": "hash",
                "location": {"file": 0, "line": 9, "col": 2},
                "path": [
                    {
                        "kind": "event",
                        "location": {"file": 0, "line": 1, "col": 1},
                        "message": "Memory is allocated",
                    },
                    {
                        "kind": "event",
                        "location": {"file": 0, "line": 5, "col": 1},
                        "message": "Memory is released",
                    },
                    {
                        "kind": "event",
                        "location": {"file": 0, "line": 9, "col": 2},
                        "message": "Use of memory after it is freed",
                    },
                ],
            }],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "sample.plist"
            with path.open("wb") as stream:
                plistlib.dump(document, stream)
            standard = parse_plist_reports(path, "Synthetic", "baseline", "action")
            streaming = parse_large_plist_reports_streaming(
                path, "Synthetic", "baseline", "action"
            )
            fields = lambda item: (
                item.checker,
                item.kind,
                item.terminal,
                item.allocation_locations,
                item.release_locations,
                item.branch_locations,
                item.skeleton,
                item.path_events,
            )
            self.assertEqual(list(map(fields, standard)), list(map(fields, streaming)))

    def test_ours_only_android_run_is_self_contained(self):
        document = {
            "files": ["/artifact/android/src/a.cpp"],
            "diagnostics": [{
                "check_name": "cplusplus.NewDeleteLeaks",
                "description": "Potential memory leak",
                "issue_hash_content_of_line_in_context": "hash",
                "location": {"file": 0, "line": 9, "col": 2},
                "path": [{
                    "kind": "event",
                    "location": {"file": 0, "line": 1, "col": 1},
                    "message": "Memory is allocated",
                }],
            }],
        }
        with tempfile.TemporaryDirectory() as directory:
            ours = Path(directory)
            with (ours / "a.plist").open("wb") as stream:
                plistlib.dump(document, stream)
            metadata = {
                "tools": [{
                    "working_directory": "/artifact/android",
                    "action_num": 1,
                    "command": [
                        "CodeChecker", "analyze", "db.json", "--enable",
                        "cplusplus", "--enable", "core", "--enable", "unix",
                    ],
                    "result_source_files": {
                        "/artifact/android/results/a.plist": "/artifact/android/src/a.cpp"
                    },
                }]
            }
            (ours / "metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
            actions, input_metadata = android_ours_action_groups(ours)
            result = analyze_dataset(actions)
            summary = summarize_ours_only(result, input_metadata)
            self.assertEqual(summary["report_instances"], 1)
            self.assertEqual(summary["lifecycle"]["complete_instances"], 1)
            self.assertEqual(summary["parse_errors"], 0)


if __name__ == "__main__":
    unittest.main()
