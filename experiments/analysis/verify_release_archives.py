#!/usr/bin/env python3
"""Stream-verify released archives against their source reports."""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import subprocess
import sys
import tarfile
import xml.parsers.expat
from pathlib import Path
from typing import Any, Sequence

from package_plist_results import replacement_rules, sanitize_bytes


def verify_member(
    name: str,
    payload: bytes,
    source_dir_text: str,
    rules: Sequence[tuple[bytes, bytes]],
) -> tuple[str, int]:
    source = Path(source_dir_text) / Path(name).name
    if not source.is_file():
        raise RuntimeError(f"archive member has no source file: {name}")
    expected = sanitize_bytes(source.read_bytes(), rules, {})
    if payload != expected:
        raise RuntimeError(f"archive member differs from sanitized source: {name}")
    if name.endswith(".plist"):
        parser = xml.parsers.expat.ParserCreate()
        parser.Parse(payload, True)
        return "plist", payload.count(b"<key>check_name</key>")
    if name.endswith("metadata.json"):
        json.loads(payload.decode("utf-8"))
        return "metadata", 0
    raise RuntimeError(f"unexpected archive member: {name}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_archive(
    *,
    dataset: str,
    archive: Path,
    archive_root: str,
    source_dir: Path,
    rules,
    jobs: int,
    zstd: str,
) -> dict[str, Any]:
    process = subprocess.Popen(
        [zstd, "-d", "--quiet", "-c", str(archive)], stdout=subprocess.PIPE
    )
    assert process.stdout is not None
    plist_files = metadata_files = diagnostic_entries = processed = 0
    pending: list[concurrent.futures.Future] = []

    def consume(future):
        nonlocal plist_files, metadata_files, diagnostic_entries, processed
        kind, diagnostics = future.result()
        processed += 1
        diagnostic_entries += diagnostics
        if kind == "plist":
            plist_files += 1
        else:
            metadata_files += 1
        if processed % 500 == 0:
            print(f"[{dataset}] stream-verified {processed:,} members", file=sys.stderr, flush=True)

    try:
        with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, jobs)) as executor:
            with tarfile.open(fileobj=process.stdout, mode="r|") as stream:
                for member in stream:
                    if not member.isfile():
                        continue
                    if not member.name.startswith(archive_root + "/"):
                        raise RuntimeError(f"unexpected archive root: {member.name}")
                    extracted = stream.extractfile(member)
                    if extracted is None:
                        raise RuntimeError(f"cannot read archive member: {member.name}")
                    payload = extracted.read()
                    pending.append(
                        executor.submit(
                            verify_member,
                            member.name,
                            payload,
                            str(source_dir),
                            rules,
                        )
                    )
                    if len(pending) >= max(1, jobs) * 2:
                        consume(pending.pop(0))
                while pending:
                    consume(pending.pop(0))
        process.stdout.close()
        return_code = process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    if return_code != 0:
        raise RuntimeError(f"zstd exited with status {return_code}")
    return {
        "dataset": dataset,
        "archive": archive.name,
        "archive_sha256": sha256_file(archive),
        "plist_files": plist_files,
        "metadata_files": metadata_files,
        "diagnostic_entries": diagnostic_entries,
        "byte_exact_sanitized_source_match": True,
        "all_members_parsed": True,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release-dir", type=Path, required=True)
    parser.add_argument("--openharmony-dir", type=Path, required=True)
    parser.add_argument("--android-dir", type=Path, required=True)
    parser.add_argument("--openharmony-source-root", required=True)
    parser.add_argument("--android-source-root", required=True)
    parser.add_argument("--toolchain-root", action="append", default=[])
    parser.add_argument("--private-home")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--zstd", default="zstd")
    args = parser.parse_args()

    specifications = [
        (
            "OpenHarmony",
            "openharmony-ours-plists.tar.zst",
            "openharmony/reports_exp_64",
            args.openharmony_dir,
            replacement_rules(
                args.openharmony_source_root, "/artifact/openharmony",
                args.toolchain_root, args.private_home,
            ),
        ),
        (
            "Android",
            "android-ours-plists.tar.zst",
            "android/reports_exp2_64",
            args.android_dir,
            replacement_rules(
                args.android_source_root, "/artifact/android",
                args.toolchain_root, args.private_home,
            ),
        ),
    ]
    results = []
    for dataset, archive_name, archive_root, source_dir, rules in specifications:
        results.append(
            verify_archive(
                dataset=dataset,
                archive=args.release_dir / archive_name,
                archive_root=archive_root,
                source_dir=source_dir,
                rules=rules,
                jobs=args.jobs,
                zstd=args.zstd,
            )
        )
    payload = {"format_version": 1, "datasets": results}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
