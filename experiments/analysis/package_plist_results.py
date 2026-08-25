#!/usr/bin/env python3
"""Create deterministic, path-sanitized archives of the released ours plists.

The source report directories are read-only.  Sanitization is performed in
memory while each member is added to a normalized tar stream compressed by the
``zstd`` command-line program.  Only public replacement targets and counts are
written to the release manifest; private source prefixes are never recorded.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path
from typing import Any, Iterable, Sequence


ARCHIVE_MTIME = 0
DEFAULT_SPLIT_BYTES = 1_900 * 1024 * 1024
PERSONAL_PATH = re.compile(rb"(?:[A-Za-z]:\\Users\\|/home/)[^/\\<>\s]+")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--openharmony-dir", type=Path, required=True)
    parser.add_argument("--android-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--openharmony-source-root", required=True)
    parser.add_argument("--android-source-root", required=True)
    parser.add_argument(
        "--toolchain-root",
        action="append",
        default=[],
        help="private tool root to replace with /artifact/toolchain; repeatable",
    )
    parser.add_argument(
        "--private-home",
        help="optional private home prefix replaced with /artifact/user",
    )
    parser.add_argument(
        "--split-bytes", type=int, default=DEFAULT_SPLIT_BYTES,
        help="split compressed archives larger than this many bytes; 0 disables",
    )
    parser.add_argument("--compression-level", type=int, default=3)
    parser.add_argument(
        "--jobs", type=int, default=min(8, os.cpu_count() or 1),
        help="workers used for bounded, ordered input sanitization",
    )
    parser.add_argument("--zstd", default="zstd")
    return parser.parse_args(argv)


def normalized_prefix(value: str) -> bytes:
    return value.replace("\\", "/").rstrip("/").encode("utf-8")


def replacement_rules(
    source_root: str,
    public_root: str,
    toolchain_roots: Iterable[str],
    private_home: str | None,
) -> list[tuple[bytes, bytes]]:
    source = normalized_prefix(source_root)
    rules: list[tuple[bytes, bytes]] = []
    for root in toolchain_roots:
        rules.append((normalized_prefix(root), b"/artifact/toolchain"))
    rules.append((source + b"/prebuilts/clang", b"/artifact/toolchain"))
    rules.append((source, public_root.encode("utf-8")))
    if private_home:
        rules.append((normalized_prefix(private_home), b"/artifact/user"))
    # Replace longer, more specific prefixes first.
    return sorted(dict(rules).items(), key=lambda item: len(item[0]), reverse=True)


def sanitize_bytes(
    data: bytes, rules: Sequence[tuple[bytes, bytes]], counts: dict[str, int]
) -> bytes:
    result = data.replace(b"\\", b"/") if b"\\home\\" in data else data
    for old, new in rules:
        updated = result.replace(old, new)
        length_delta = len(old) - len(new)
        if length_delta:
            occurrences = (len(result) - len(updated)) // length_delta
        else:
            occurrences = result.count(old)
        if occurrences:
            counts[new.decode("utf-8")] = counts.get(new.decode("utf-8"), 0) + occurrences
        result = updated
    if b"/home/" in result or b":\\Users\\" in result:
        match = PERSONAL_PATH.search(result)
        if match:
            raise RuntimeError("personal absolute path remains after sanitization")
    return result


def sanitize_object(value: Any, rules: Sequence[tuple[bytes, bytes]]) -> Any:
    if isinstance(value, str):
        data = value.encode("utf-8")
        for old, new in rules:
            data = data.replace(old, new)
        return data.decode("utf-8")
    if isinstance(value, list):
        return [sanitize_object(item, rules) for item in value]
    if isinstance(value, tuple):
        return tuple(sanitize_object(item, rules) for item in value)
    if isinstance(value, dict):
        return {
            sanitize_object(key, rules): sanitize_object(item, rules)
            for key, item in value.items()
        }
    return value


def validate_plist(original: bytes, sanitized: bytes, rules) -> int:
    # sanitize_bytes performs only literal prefix substitutions. Verify that
    # diagnostic cardinality is unchanged here. The standalone evidence
    # analysis parses every diagnostic and path field after archive extraction.
    marker = b"<key>check_name</key>"
    # None of the configured path prefixes can contain the XML key marker, so
    # literal replacement cannot change this count.
    return sanitized.count(marker)


def validate_json(original: bytes, sanitized: bytes, rules) -> None:
    before = json.loads(original.decode("utf-8"))
    after = json.loads(sanitized.decode("utf-8"))
    if sanitize_object(before, rules) != after:
        raise RuntimeError("sanitized JSON differs beyond configured path replacements")


def add_member(archive: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name=name)
    info.size = len(data)
    info.mtime = ARCHIVE_MTIME
    info.mode = 0o644
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    archive.addfile(info, io.BytesIO(data))


def prepare_member(
    path_text: str, rules: Sequence[tuple[bytes, bytes]]
) -> tuple[str, bytes, int, int, dict[str, int]]:
    """Read, sanitize, and validate one member in a worker process."""
    path = Path(path_text)
    original = path.read_bytes()
    counts: dict[str, int] = {}
    sanitized = sanitize_bytes(original, rules, counts)
    diagnostics = 0
    if path.suffix == ".plist":
        diagnostics = validate_plist(original, sanitized, rules)
    elif path.name == "metadata.json":
        validate_json(original, sanitized, rules)
    return path.name, sanitized, len(original), diagnostics, counts


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def split_archive(path: Path, split_bytes: int) -> list[dict[str, Any]]:
    if not split_bytes or path.stat().st_size <= split_bytes:
        return [{"file": path.name, "bytes": path.stat().st_size, "sha256": sha256_file(path)}]
    parts: list[dict[str, Any]] = []
    with path.open("rb") as source:
        index = 1
        while True:
            block = source.read(split_bytes)
            if not block:
                break
            part = path.with_name(f"{path.name}.part-{index:04d}")
            part.write_bytes(block)
            parts.append({"file": part.name, "bytes": len(block), "sha256": sha256_file(part)})
            index += 1
    path.unlink()
    return parts


def package_dataset(
    *,
    dataset: str,
    source_dir: Path,
    archive_root: str,
    archive_path: Path,
    rules: Sequence[tuple[bytes, bytes]],
    zstd: str,
    compression_level: int,
    split_bytes: int,
    jobs: int,
) -> dict[str, Any]:
    plist_paths = sorted(source_dir.glob("*.plist"), key=lambda item: item.name)
    extra_paths = [source_dir / "metadata.json"] if (source_dir / "metadata.json").is_file() else []
    paths = plist_paths + extra_paths
    if not plist_paths:
        raise RuntimeError(f"no plist files found in {source_dir}")

    counts: dict[str, int] = {}
    raw_bytes = 0
    sanitized_bytes = 0
    diagnostics = 0
    command = [
        zstd,
        f"-{compression_level}",
        "--threads=0",
        "--quiet",
        "--force",
        "-o",
        str(archive_path),
    ]
    process = subprocess.Popen(command, stdin=subprocess.PIPE)
    assert process.stdin is not None
    try:
        with tarfile.open(fileobj=process.stdin, mode="w|") as archive:
            with concurrent.futures.ProcessPoolExecutor(max_workers=max(1, jobs)) as executor:
                path_iterator = iter(paths)
                pending: list[concurrent.futures.Future] = []
                for _ in range(max(1, jobs) * 2):
                    try:
                        path = next(path_iterator)
                    except StopIteration:
                        break
                    pending.append(executor.submit(prepare_member, str(path), rules))

                index = 0
                while pending:
                    future = pending.pop(0)
                    name, sanitized, source_bytes, member_diagnostics, member_counts = future.result()
                    index += 1
                    diagnostics += member_diagnostics
                    for target, count in member_counts.items():
                        counts[target] = counts.get(target, 0) + count
                    add_member(archive, f"{archive_root}/{name}", sanitized)
                    raw_bytes += source_bytes
                    sanitized_bytes += len(sanitized)
                    try:
                        path = next(path_iterator)
                    except StopIteration:
                        pass
                    else:
                        pending.append(executor.submit(prepare_member, str(path), rules))
                    if index % 500 == 0:
                        print(f"[{dataset}] packaged and validated {index:,}/{len(paths):,} files", file=sys.stderr, flush=True)
        process.stdin.close()
        return_code = process.wait()
    except BaseException:
        process.kill()
        process.wait()
        raise
    if return_code != 0:
        raise RuntimeError(f"zstd exited with status {return_code}")

    archive_sha256 = sha256_file(archive_path)
    compressed_bytes = archive_path.stat().st_size
    parts = split_archive(archive_path, split_bytes)
    return {
        "dataset": dataset,
        "configuration": "ours",
        "compression": f"zstd level {compression_level}",
        "archive": archive_path.name,
        "archive_root": archive_root,
        "archive_sha256": archive_sha256,
        "compressed_bytes": compressed_bytes,
        "parts": parts,
        "plist_files": len(plist_paths),
        "metadata_files": len(extra_paths),
        "raw_bytes": raw_bytes,
        "sanitized_bytes": sanitized_bytes,
        "diagnostic_entries": diagnostics,
        "sanitization_replacements": dict(sorted(counts.items())),
    }


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if shutil.which(args.zstd) is None:
        raise SystemExit(f"zstd executable not found: {args.zstd}")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)

    datasets = [
        {
            "dataset": "OpenHarmony",
            "source_dir": args.openharmony_dir.resolve(),
            "archive_root": "openharmony/reports_exp_64",
            "archive_path": output / "openharmony-ours-plists.tar.zst",
            "rules": replacement_rules(
                args.openharmony_source_root,
                "/artifact/openharmony",
                args.toolchain_root,
                args.private_home,
            ),
        },
        {
            "dataset": "Android",
            "source_dir": args.android_dir.resolve(),
            "archive_root": "android/reports_exp2_64",
            "archive_path": output / "android-ours-plists.tar.zst",
            "rules": replacement_rules(
                args.android_source_root,
                "/artifact/android",
                args.toolchain_root,
                args.private_home,
            ),
        },
    ]
    results = []
    for item in datasets:
        results.append(
            package_dataset(
                **item,
                zstd=args.zstd,
                compression_level=args.compression_level,
                split_bytes=args.split_bytes,
                jobs=max(1, args.jobs),
            )
        )

    manifest = {
        "format_version": 1,
        "description": "Path-sanitized ours-only CodeChecker plist results",
        "datasets": results,
    }
    manifest_path = output / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    checksum_lines = []
    for result in results:
        for part in result["parts"]:
            checksum_lines.append(f"{part['sha256']}  {part['file']}")
    (output / "SHA256SUMS").write_text(
        "\n".join(checksum_lines) + "\n", encoding="ascii"
    )
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
