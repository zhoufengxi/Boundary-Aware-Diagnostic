#!/usr/bin/env python3
"""Capture one compiler invocation as a Bear compilation-database shard."""

import argparse
import os
from pathlib import Path
import sys

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output",
                        help="output shard; otherwise OHOS_COMPDB_SHARDS/<pid>_compile_commands.json")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    parsed_args = parser.parse_args()

    # Available beside this wrapper in an OpenHarmony checkout. Import lazily
    # so that the standalone artifact can still expose --help.
    import wrapper_utils

    output = parsed_args.output
    if output is None:
        shard_dir = os.environ.get("OHOS_COMPDB_SHARDS")
        if not shard_dir:
            parser.error("set OHOS_COMPDB_SHARDS or pass --output")
        output = str(Path(shard_dir) / f"{os.getpid()}_compile_commands.json")

    output_path = Path(output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["bear", "--append", "--output", str(output_path), "--"] + parsed_args.args

    log_path = os.environ.get("OHOS_COMPDB_LOG")
    if log_path:
        log = Path(log_path)
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"PID:{os.getpid()} {' '.join(cmd)}\n")

    return_code, _ = wrapper_utils.capture_command_stderr(
        wrapper_utils.command_to_run(cmd))
    return return_code


if __name__ == "__main__":
    sys.exit(main())
