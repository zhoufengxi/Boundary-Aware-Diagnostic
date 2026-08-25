#!/usr/bin/env python3
"""Copy text-derived results while replacing private path prefixes."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


PERSONAL_PATH = re.compile(r"(?:[A-Za-z]:\\Users\\|/home/)[^/\\<>\s]+")


def parse_replacement(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("replacement must have the form OLD=NEW")
    old, new = value.split("=", 1)
    if not old:
        raise argparse.ArgumentTypeError("replacement prefix may not be empty")
    return old.replace("\\", "/").rstrip("/"), new.rstrip("/")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--replace", type=parse_replacement, action="append", default=[])
    args = parser.parse_args()
    rules = sorted(args.replace, key=lambda item: len(item[0]), reverse=True)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for source in sorted(args.input_dir.iterdir(), key=lambda item: item.name):
        if not source.is_file():
            continue
        text = source.read_text(encoding="utf-8")
        for old, new in rules:
            text = text.replace(old, new)
        if PERSONAL_PATH.search(text):
            raise RuntimeError(f"personal path remains in {source.name}")
        (args.output_dir / source.name).write_text(text, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
