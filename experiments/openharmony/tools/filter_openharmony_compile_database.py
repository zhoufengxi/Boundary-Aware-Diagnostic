#!/usr/bin/env python3
"""Apply the OpenHarmony compilation-database filters used in the paper.

The filter keeps C++ sources, keeps ARM/AArch64 OpenHarmony targets, removes
sources listed by the CTU-connection stage, and excludes ArkCompiler sources.
"""

import argparse
import json
from pathlib import Path, PurePosixPath


CPP_SUFFIXES = ("cc", "cpp")
OHOS_TARGETS = ("--target=aarch64-linux-ohos", "--target=arm-linux-ohos")


def normalized_source(path: str, source_root: Path) -> str:
    """Return a stable source-root-relative spelling when possible."""
    normalized = path.replace("\\", "/")
    root = source_root.resolve()
    try:
        candidate = Path(path)
        if candidate.is_absolute():
            return candidate.resolve().relative_to(root).as_posix()
    except (OSError, ValueError):
        pass
    if normalized.startswith("../../"):
        normalized = normalized[6:]
    return PurePosixPath(normalized).as_posix()


def read_connection_leaves(connect_dir: Path, source_root: Path) -> set[str]:
    leaves: set[str] = set()
    if not connect_dir.is_dir():
        print(f"[WARNING] CTU connection directory does not exist: {connect_dir}")
        return leaves
    for path in sorted(connect_dir.iterdir(), key=lambda item: item.name):
        if not path.is_file():
            continue
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                value = line.strip()
                if value:
                    leaves.add(normalized_source(value, source_root))
    return leaves


def filter_database(data: list[dict], source_root: Path,
                    connect_dir: Path) -> tuple[list[dict], dict[str, int]]:
    counts = {
        "input": len(data),
        "non_cpp": 0,
        "non_ohos_target": 0,
        "ctu_connection": 0,
        "arkcompiler": 0,
    }
    leaves = read_connection_leaves(connect_dir, source_root)
    result: list[dict] = []

    for entry in data:
        source = entry["file"]
        suffix = source.rsplit(".", 1)[-1]
        if not suffix.endswith(CPP_SUFFIXES):
            counts["non_cpp"] += 1
            continue
        arguments = entry["arguments"]
        if not any(target in arguments for target in OHOS_TARGETS):
            counts["non_ohos_target"] += 1
            continue
        if leaves and normalized_source(source, source_root) in leaves:
            counts["ctu_connection"] += 1
            continue
        if "/arkcompiler/" in source.replace("\\", "/"):
            counts["arkcompiler"] += 1
            continue
        result.append(entry)

    counts["output"] = len(result)
    return result, counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path,
                        help="merged input compilation database")
    parser.add_argument("--output", required=True, type=Path,
                        help="filtered output compilation database")
    parser.add_argument("--source-root", required=True, type=Path,
                        help="root of the OpenHarmony checkout")
    parser.add_argument("--ctu-connections", required=True, type=Path,
                        help="directory containing CTU connection lists")
    args = parser.parse_args()

    with args.input.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    if not isinstance(data, list):
        parser.error("input compilation database must contain a JSON array")

    filtered, counts = filter_database(
        data, args.source_root, args.ctu_connections)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        json.dump(filtered, stream, indent=4)
        stream.write("\n")

    print(json.dumps(counts, indent=2, sort_keys=True))
    print(f"Wrote {len(filtered)} entries to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
