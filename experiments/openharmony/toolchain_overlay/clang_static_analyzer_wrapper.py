#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright 2017 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Add a static-analysis step to Clang C/C++ compiler invocations."""

import argparse
import os
from pathlib import Path
import sys

ANALYZER_ENABLE_FLAGS = ["--analyze"]
ANALYZER_OPTION_FLAGS = [
    "-analyzer-checker=core,unix,deadcode,security,cplusplus",
    "-analyzer-config",
    "suppress-c++-stdlib=true",
    "-analyzer-output=html",
]


def interleave_args(args, token):
    """Prepend ``token`` to every item in ``args``."""
    return list(sum(zip([token] * len(args), args), ()))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["clang", "cl"], required=True,
                        help="compiler argument convention")
    parser.add_argument("--output-dir",
                        default=os.environ.get("OHOS_ANALYZER_REPORT_DIR", "analyzer_reports"),
                        help="HTML analyzer report directory")
    parser.add_argument("args", nargs=argparse.REMAINDER)
    parsed_args = parser.parse_args()

    # Available beside this wrapper in an OpenHarmony checkout. Import lazily
    # so that the standalone artifact can still expose --help.
    import wrapper_utils

    output_dir = Path(parsed_args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = "-Xclang" if parsed_args.mode == "cl" else "-Xanalyzer"
    analyzer_cmd = (
        parsed_args.args
        + ANALYZER_ENABLE_FLAGS
        + interleave_args(ANALYZER_OPTION_FLAGS, prefix)
        + ["-o", str(output_dir)]
    )

    analyzer_code, analyzer_stderr = wrapper_utils.capture_command_stderr(
        wrapper_utils.command_to_run(analyzer_cmd))
    sys.stderr.write(analyzer_stderr.decode("utf-8", errors="replace"))

    compile_code, compile_stderr = wrapper_utils.capture_command_stderr(
        wrapper_utils.command_to_run(parsed_args.args))
    sys.stderr.write(compile_stderr.decode("utf-8", errors="replace"))

    log_path = os.environ.get("OHOS_ANALYZER_LOG")
    if analyzer_code > 0 and log_path:
        log = Path(log_path)
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as stream:
            stream.write(f"analyzer command: {' '.join(analyzer_cmd)}\n")
            stream.write(f"analyzer exit: {analyzer_code}\n")
            stream.write(analyzer_stderr.decode("utf-8", errors="replace"))
            stream.write(f"\ncompiler command: {' '.join(parsed_args.args)}\n")
            stream.write(f"compiler exit: {compile_code}\n")
            stream.write(compile_stderr.decode("utf-8", errors="replace"))
            stream.write("\n" + "=" * 80 + "\n")

    return compile_code


if __name__ == "__main__":
    sys.exit(main())
