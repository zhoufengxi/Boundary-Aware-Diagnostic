#!/usr/bin/env python3
"""Replay AOSP compile_commands.json entries with a custom clang (compile-only, no link).

Filters entries belonging to a given set of modules, rewrites the compiler to a
custom clang, fixes version-incompatible flags, appends `-c -o`, and compiles in
parallel. See module docstring of the plan for background.
"""

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import subprocess
import sys
from collections import defaultdict

AOSP_ROOT = None
COMPDB = None
CUSTOM_CLANG_BIN = None
AOSP_CLANG_PREFIX = "prebuilts/clang/host/linux-x86/clang-r522817/bin/"

# source dir -> module name
MODULE_DIRS = {
    "system/libbase/": "libbase",
    "system/core/libutils/": "libutils",
    "system/logging/liblog/": "liblog",
    "frameworks/native/libs/binder/": "libbinder",
    "frameworks/native/libs/ui/": "libui",
    "frameworks/native/libs/gui/": "libgui",
}

# flags dropped because the custom clang 18.1.6 build cannot handle them:
# - -gz=zstd: this clang build has neither zstd nor zlib debug-compression
#   support (fatal under -Werror), and no replacement is appended
DROP_FLAGS = frozenset([
    "-gz=zstd",
])

# Use the AOSP prebuilt clang's resource dir instead of the custom clang's own.
# This provides (all from clang 18.0.1, same major version as the custom 18.1.6):
# - builtin headers matching AOSP's libc++ (its stdatomic.h is C++-aware and
#   pairs with the r522817 libc++ <atomic>; the vanilla 18.1.6 stdatomic.h
#   leaks C macros into C++ TUs and breaks <atomic>)
# - include/fuzzer/FuzzedDataProvider.h for -fsanitize=fuzzer-no-link entries
# - share/cfi_ignorelist.txt required by -fsanitize=cfi
AOSP_RESOURCE_DIR = None

# NOTE: joined form "-resource-dir=<dir>", not the separate form. Tools that
# parse compile_commands.json with simple argument classifiers (e.g.
# CodeChecker's log_parser) keep tokens starting with '-' but drop bare
# positional tokens, which would silently eat the separate value and leave a
# dangling "-resource-dir" flag that swallows the next option.
APPEND_FLAGS = []


def module_of(path):
    """Return the module name for a compdb file path, or None."""
    for prefix, name in MODULE_DIRS.items():
        if path.startswith(prefix):
            return name
    # generated sources (aidl etc.) live under out/soong/.intermediates/<module dir>/...
    if path.startswith("out/soong/.intermediates/"):
        rest = path[len("out/soong/.intermediates/"):]
        for prefix, name in MODULE_DIRS.items():
            if rest.startswith(prefix):
                return name
    return None


def missing_inputs(entry):
    """Return list of missing inputs (source file / -I out dirs) for an entry."""
    missing = []
    if not os.path.exists(os.path.join(AOSP_ROOT, entry["file"])):
        missing.append(entry["file"])
    args = entry["arguments"]
    i = 0
    while i < len(args):
        a = args[i]
        d = None
        if a.startswith("-Iout/"):
            d = a[2:]
        elif a == "-I" and i + 1 < len(args) and args[i + 1].startswith("out/"):
            d = args[i + 1]
        elif a == "-isystem" and i + 1 < len(args) and args[i + 1].startswith("out/"):
            d = args[i + 1]
        if d is not None and not os.path.isdir(os.path.join(AOSP_ROOT, d)):
            missing.append(d)
        i += 1
    return missing


def dequote_args(args):
    """Repair shell-quoting artifacts baked into compdb arguments by Soong.

    The Soong compdb exporter keeps shell quoting literally and even splits
    quoted strings on embedded spaces, producing tokens like:
      "'-D__INTRODUCED_IN(n)='"                       (self-contained)
      "'-DDATE=\"Dec", "31", "1969\"'"                (split across tokens)
    Direct replay treats them as (nonexistent) input files; CodeChecker drops
    them as positional args. Either way the intended flag is lost. Rejoin
    split regions and remove one layer of surrounding single quotes.
    """
    out = []
    i = 0
    n = len(args)
    while i < n:
        a = args[i]
        if a.startswith("'"):
            if a.count("'") >= 2 and a.endswith("'"):
                out.append(a[1:-1])          # self-contained quoted token
                i += 1
                continue
            # opening quote only: join until a token ending with '
            buf = [a]
            j = i + 1
            while j < n and j - i <= 8 and not args[j].endswith("'"):
                buf.append(args[j])
                j += 1
            if j < n and args[j].endswith("'"):
                buf.append(args[j])
                joined = " ".join(buf)
                out.append(joined[1:-1])     # strip the outer quote pair
                i = j + 1
                continue
        out.append(a)
        i += 1
    return out


def transform(entry, out_root, syntax_only, create_dirs=True):
    """Rewrite a compdb entry into a compile-only command for the custom clang.

    Returns (cmd, out_path) where cmd is the full argv list.
    """
    args = entry["arguments"]
    compiler = os.path.basename(args[0])  # clang or clang++
    # dequote literal-quote artifacts, drop incompatible flags and the trailing
    # positional source file (re-added explicitly with -c below)
    new_args = dequote_args(args[1:])
    new_args = [a for a in new_args if a not in DROP_FLAGS and a != entry["file"]]
    new_args.extend(APPEND_FLAGS)
    if syntax_only:
        cmd = [os.path.join(CUSTOM_CLANG_BIN, compiler)] + new_args + [
            "-fsyntax-only", entry["file"],
        ]
        return cmd, None
    # variant hash distinguishes different variants compiling the same source
    digest = hashlib.sha1("\0".join(new_args).encode()).hexdigest()[:8]
    out_path = os.path.join(out_root, entry["file"] + "_" + digest + ".o")
    if create_dirs:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
    cmd = [os.path.join(CUSTOM_CLANG_BIN, compiler)] + new_args + [
        "-c", entry["file"], "-o", out_path,
    ]
    return cmd, out_path


def run_one(job):
    """Execute one transformed command. Returns a result dict."""
    idx, module, entry, cmd, out_path = job
    r = subprocess.run(cmd, cwd=AOSP_ROOT, capture_output=True, text=True)
    category = ""
    if r.returncode != 0:
        if "no such file or directory" in r.stderr or "file not found" in r.stderr:
            category = "missing-input"
        else:
            category = "compile-error"
    return {
        "idx": idx,
        "module": module,
        "file": entry["file"],
        "returncode": r.returncode,
        "category": category,
        "stderr_tail": r.stderr[-2000:] if r.returncode != 0 else "",
        "out": out_path,
        "cmd": cmd,
    }


# Files that crash the clang-18 static analyzer itself (SIGSEGV / frontend
# failure under CTU, observed empirically; plain compilation succeeds).
# clang 18.1.6 and the AOSP r522817 (18.0.1) share the analyzer code, so
# these are upstream analyzer bugs on these TUs, not DB issues.
ANALYZER_CRASH_FILES = frozenset([
    "external/deqp/framework/platform/tcuMain.cpp",
    "external/swiftshader/third_party/llvm-16.0/llvm/lib/Object/Decompressor.cpp",
])


def find_unanalyzable():
    """Return (verify_files, asm_files, broken_files) path sets.

    These entries cannot be usefully compiled/analyzed from the compdb:
    compiler diagnostic self-tests (-Xclang -verify), assembly files (real
    builds use yasm/nasm or per-arch rules; the compdb records them as clang
    commands), and files that fail even with the stock AOSP clang
    (cross-verified pre-existing source/compdb defects).
    """
    with open(COMPDB) as f:
        data = json.load(f)

    verify_files = set()
    asm_files = set()
    for e in data:
        src = e["file"]
        if any(a == "-verify" for a in e["arguments"]):
            verify_files.add(src)
        if src.rsplit(".", 1)[-1].lower() in ("s", "asm"):
            asm_files.add(src)

    broken_files = set()
    failed_json = os.path.join(AOSP_ROOT, "out", "clang18-obj", "failed_cmds.json")
    if os.path.exists(failed_json):
        with open(failed_json) as f:
            for r in json.load(f):
                if r.get("category") == "compile-error":
                    broken_files.add(r["file"])
                elif r.get("category") == "missing-input":
                    # Distinguish genuinely-absent headers (e.g. device kernel
                    # headers like scsi/ufs/ioctl.h not shipped in this tree)
                    # from generated headers of unbuilt modules: the error
                    # message shows the include-relative name, so a generated
                    # 'android/...' header does NOT look like an out/ path.
                    # Rule: if the entry has no out/ include dirs at all and
                    # the missing name is not found under any of its in-tree
                    # include dirs, it can never compile here -> exclude.
                    # Entries with out/ include dirs may become compilable
                    # after `m` builds, so keep them.
                    stderr = re.sub(r"\x1b\[[0-9;]*m", "", r.get("stderr_tail", ""))
                    missing = re.findall(r"fatal error: '([^']+)' file not found",
                                         stderr)
                    inc_dirs, has_out_inc = [], False
                    cmd = r.get("cmd", [])
                    for i, a in enumerate(cmd):
                        if a == "-isystem" or a == "-I":
                            d = cmd[i + 1] if i + 1 < len(cmd) else ""
                        elif a.startswith("-isystem"):
                            d = a[len("-isystem"):]
                        elif a.startswith("-I"):
                            d = a[2:]
                        else:
                            continue
                        if d.startswith("out/") or "/out/" in d:
                            has_out_inc = True
                        else:
                            inc_dirs.append(d)
                    tree_absent = (
                        missing and not has_out_inc
                        and not any(os.path.exists(os.path.join(AOSP_ROOT, d, m))
                                    for m in missing for d in inc_dirs))
                    if tree_absent:
                        broken_files.add(r["file"])
    broken_files |= ANALYZER_CRASH_FILES
    return verify_files, asm_files, broken_files


def emit_skipfile(path):
    """Write a CodeChecker skip file for entries that cannot be analyzed."""
    verify_files, asm_files, broken_files = find_unanalyzable()

    def abs_path(src):
        return src if os.path.isabs(src) else os.path.join(AOSP_ROOT, src)

    lines = []
    for files in (verify_files, asm_files, broken_files):
        for src in sorted(files):
            lines.append(f"-{abs_path(src)}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {len(lines)} skip entries to {path} "
          f"(verify: {len(verify_files)}, asm: {len(asm_files)}, "
          f"broken: {len(broken_files)})")


def resolve_clang_bin(path):
    """Validate/normalize the --clang-bin argument.

    Accepts the bin directory; if given the clang binary itself by mistake
    (e.g. .../bin/clang-18), fall back to its directory with a warning.
    """
    if os.path.isfile(path):
        print(f"warning: --clang-bin points to a file ({path}); "
              f"using its directory {os.path.dirname(path)} instead")
        path = os.path.dirname(path)
    if not os.path.isdir(path):
        sys.exit(f"error: --clang-bin {path} is not a directory")
    for tool in ("clang", "clang++"):
        if not os.path.isfile(os.path.join(path, tool)):
            sys.exit(f"error: {path}/{tool} not found; "
                     f"--clang-bin must be a clang bin directory")
    return path


def main():
    global AOSP_ROOT, COMPDB, CUSTOM_CLANG_BIN, AOSP_RESOURCE_DIR, APPEND_FLAGS

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--aosp-root", required=True, metavar="DIR",
                    help="root of the Android source checkout")
    ap.add_argument("--input-compdb", metavar="PATH",
                    help="input Soong compilation database; default: "
                         "<aosp-root>/compile_commands.json")
    ap.add_argument("--clang-bin", required=True, metavar="DIR",
                    help="bin dir of the custom clang (containing clang/clang++). "
                         "IMPORTANT: must match the clang CodeChecker actually uses "
                         "(i.e. the clang on PATH when running CodeChecker), "
                         "otherwise CodeChecker injects the DB compiler's implicit "
                         "include dirs into the analyzer's commands and header "
                         "sets collide.")
    ap.add_argument("--module", action="append",
                    help="only this module (repeatable); default: all six")
    ap.add_argument("--all-entries", action="store_true",
                    help="attempt EVERY entry in the compdb (no module filter, "
                         "no missing-inputs pre-filter); failures are classified "
                         "as missing-input vs compile-error")
    ap.add_argument("--limit", type=int, default=0, help="compile at most N entries")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count(),
                    help="parallel workers (default: cpu count)")
    ap.add_argument("--syntax-only", action="store_true",
                    help="use -fsyntax-only instead of producing .o files")
    ap.add_argument("--emit-compdb", metavar="PATH",
                    help="write the transformed compilation database to PATH and exit "
                         "(no compilation); with --module, only those modules' entries "
                         "are included, otherwise the full tree")
    ap.add_argument("--emit-skipfile", metavar="PATH",
                    help="write a CodeChecker skip file (-i) to PATH and exit. "
                         "Collects entries that cannot be analyzed: compiler "
                         "diagnostic self-tests (-verify), assembly files "
                         "(.S/.s/.asm, compdb artifacts), and files listed as "
                         "compile-error in out/clang18-obj/failed_cmds.json")
    ap.add_argument("--built-variants-only", action="store_true",
                    help="with --emit-compdb: drop entries whose inputs "
                         "(generated out/ files) do not exist")
    ap.add_argument("--drop-unanalyzable", action="store_true",
                    help="with --emit-compdb: also drop entries that cannot be "
                         "analyzed (compiler -verify self-tests, .S/.s/.asm "
                         "assembly, files failing even with the stock clang). "
                         "Needed because CodeChecker ignores -i skip files in "
                         "--ctu mode (pre_analysis_skip_handlers is only set "
                         "for explicit --ctu-collect)")
    ap.add_argument("--out-root",
                    help="where .o files and reports go; default: "
                         "<aosp-root>/out/clang18-obj")
    ap.add_argument("--keep-going", action="store_true",
                    help="do not stop early after many failures")
    args = ap.parse_args()

    AOSP_ROOT = os.path.abspath(args.aosp_root)
    if not os.path.isdir(AOSP_ROOT):
        ap.error(f"AOSP root does not exist: {AOSP_ROOT}")
    COMPDB = os.path.abspath(
        args.input_compdb or os.path.join(AOSP_ROOT, "compile_commands.json"))
    if not os.path.isfile(COMPDB):
        ap.error(f"input compilation database does not exist: {COMPDB}")
    args.out_root = os.path.abspath(
        args.out_root or os.path.join(AOSP_ROOT, "out", "clang18-obj"))

    CUSTOM_CLANG_BIN = resolve_clang_bin(os.path.abspath(args.clang_bin))
    AOSP_RESOURCE_DIR = os.path.join(
        AOSP_ROOT,
        "prebuilts/clang/host/linux-x86/clang-r522817/lib/clang/18",
    )
    APPEND_FLAGS = ["-resource-dir=" + AOSP_RESOURCE_DIR]

    if args.emit_skipfile:
        emit_skipfile(args.emit_skipfile)
        return 0

    wanted = set(args.module) if args.module else set(MODULE_DIRS.values())
    unknown = wanted - set(MODULE_DIRS.values())
    if unknown:
        sys.exit(f"unknown module(s): {sorted(unknown)}; choices: {sorted(set(MODULE_DIRS.values()))}")

    print(f"loading {COMPDB} ...", flush=True)
    with open(COMPDB) as f:
        data = json.load(f)
    print(f"{len(data)} entries total", flush=True)

    if args.emit_compdb:
        excluded = set()
        if args.drop_unanalyzable:
            excluded = set().union(*find_unanalyzable())
        out_entries = []
        n_skipped = 0
        n_excluded = 0
        for e in data:
            mod = module_of(e["file"])
            if args.module and (mod is None or mod not in wanted):
                continue
            if e["file"] in excluded:
                n_excluded += 1
                continue
            if args.built_variants_only and missing_inputs(e):
                n_skipped += 1
                continue
            cmd, _ = transform(e, args.out_root, syntax_only=False, create_dirs=False)
            out_entries.append({
                "directory": e["directory"],
                "arguments": cmd,
                "file": e["file"],
            })
        with open(args.emit_compdb, "w") as f:
            json.dump(out_entries, f, indent=1)
        print(f"wrote {len(out_entries)} entries to {args.emit_compdb}"
              + (f" (skipped {n_skipped} entries with missing inputs)"
                 if n_skipped else "")
              + (f" (excluded {n_excluded} unanalyzable entries)"
                 if n_excluded else ""))
        return 0

    # filter to wanted modules, dedupe identical (file, flags)
    selected = []
    seen = set()
    per_module = defaultdict(int)
    for e in data:
        mod = module_of(e["file"])
        if mod is None:
            if not args.all_entries:
                continue
            # label by top-two-level dir for a useful breakdown (e.g. external/zlib)
            mod = "/".join(e["file"].split("/")[:2])
        key = (e["file"], "\0".join(e["arguments"]))
        if key in seen:
            continue
        seen.add(key)
        selected.append((mod, e))
        per_module[mod] += 1

    # pre-filter entries whose inputs (generated headers/sources) were not built
    ready = []
    skipped = defaultdict(list)
    for mod, e in selected:
        if args.all_entries:
            ready.append((mod, e))  # attempt everything, classify failures later
            continue
        miss = missing_inputs(e)
        if miss:
            skipped[mod].append((e["file"], miss[:3]))
        else:
            ready.append((mod, e))

    n_modules = len(per_module)
    top = sorted(per_module.items(), key=lambda kv: -kv[1])[:10]
    print(f"selected {len(selected)} entries across {n_modules} module dirs, top: {dict(top)}",
          flush=True)
    if not args.all_entries:
        print(f"skipped {sum(len(v) for v in skipped.values())} entries with missing inputs "
              f"(unbuilt variants): "
              f"{ {m: len(v) for m, v in skipped.items() if v} }", flush=True)
    if args.limit:
        ready = ready[:args.limit]
        print(f"--limit {args.limit}: compiling {len(ready)} entries", flush=True)
    if not ready:
        print("nothing to compile")
        return 0

    os.makedirs(args.out_root, exist_ok=True)
    jobs = []
    for idx, (mod, e) in enumerate(ready):
        cmd, out_path = transform(e, args.out_root, args.syntax_only)
        jobs.append((idx, mod, e, cmd, out_path))

    results = []
    failed = []
    keep_going = args.keep_going or args.all_entries
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for res in pool.map(run_one, jobs, chunksize=8):
            results.append(res)
            if res["returncode"] != 0:
                failed.append(res)
                if not keep_going and len(failed) >= 20 and len(results) < 100:
                    print("too many early failures, aborting (use --keep-going to override)",
                          flush=True)
                    pool.shutdown(cancel_futures=True)
                    break
            done = len(results)
            if done % 2000 == 0 or done == len(jobs):
                print(f"  {done}/{len(jobs)} compiled, {len(failed)} failed", flush=True)

    ok_by_mod = defaultdict(int)
    fail_by_mod = defaultdict(int)
    cat_by_mod = defaultdict(lambda: defaultdict(int))
    for res in results:
        if res["returncode"] == 0:
            ok_by_mod[res["module"]] += 1
        else:
            fail_by_mod[res["module"]] += 1
            cat_by_mod[res["module"]][res["category"]] += 1

    all_mods = sorted(set(ok_by_mod) | set(fail_by_mod))
    by_module_full = {m: {"ok": ok_by_mod.get(m, 0),
                          "failed": fail_by_mod.get(m, 0),
                          **{c: n for c, n in cat_by_mod.get(m, {}).items()}}
                      for m in all_mods}
    # keep the printed report scannable when there are many groups
    shown = dict(sorted(by_module_full.items(),
                        key=lambda kv: -(kv[1]["ok"] + kv[1]["failed"]))[:40])
    if len(by_module_full) > len(shown):
        shown["..."] = {"groups_omitted": len(by_module_full) - 40}
    report = {
        "total": len(results),
        "succeeded": len(results) - len(failed),
        "failed": len(failed),
        "failure_categories": {
            cat: sum(c[cat] for c in cat_by_mod.values())
            for cat in ("missing-input", "compile-error")
        },
        "by_module": shown,
    }
    print(json.dumps(report, indent=2))
    summary_path = os.path.join(args.out_root, "summary.json")
    os.makedirs(args.out_root, exist_ok=True)
    with open(summary_path, "w") as f:
        json.dump({**report, "by_module": by_module_full}, f, indent=2)
    print(f"full summary written to {summary_path}")

    if failed:
        fail_path = os.path.join(args.out_root, "failed_cmds.json")
        with open(fail_path, "w") as f:
            json.dump([{k: r[k] for k in ("module", "file", "returncode", "category", "stderr_tail", "cmd")}
                       for r in failed], f, indent=2)
        print(f"{len(failed)} failures written to {fail_path}")
        for r in failed[:5]:
            print(f"--- [{r['category']}] {r['module']} {r['file']}\n{r['stderr_tail'][-500:]}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
