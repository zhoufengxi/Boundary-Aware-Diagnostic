#!/usr/bin/env python3
"""Deterministically merge OpenHarmony compilation-database shards.

The original experiment discarded shard entries containing ``-resource-dir``;
that behavior is retained because those entries were not part of the analyzed
database.
"""

import argparse
import json
from pathlib import Path


def merge_databases(input_dir: Path) -> tuple[list[dict], int]:
    entries: list[dict] = []
    dropped_resource_dir = 0

    for path in sorted(input_dir.glob("*.json"), key=lambda item: item.name):
        with path.open("r", encoding="latin1") as stream:
            shard = json.load(stream)
        if not isinstance(shard, list):
            raise ValueError(f"{path} does not contain a JSON array")
        for entry in shard:
            arguments = entry.get("arguments")
            if not isinstance(arguments, list):
                raise ValueError(f"entry in {path} has no arguments array")
            if "-resource-dir" in arguments:
                dropped_resource_dir += 1
                continue
            entries.append(entry)

    return entries, dropped_resource_dir


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True, type=Path,
                        help="directory containing per-process JSON shards")
    parser.add_argument("--output", required=True, type=Path,
                        help="merged compilation database")
    args = parser.parse_args()

    if not args.input_dir.is_dir():
        parser.error(f"input directory does not exist: {args.input_dir}")

    entries, dropped = merge_databases(args.input_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="latin1") as stream:
        json.dump(entries, stream, indent=4)
        stream.write("\n")

    print(f"Merged {len(entries)} entries into {args.output}")
    print(f"Dropped {dropped} entries containing a separate -resource-dir option")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
