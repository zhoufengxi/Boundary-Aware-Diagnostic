# Android 15 Reproduction

This directory contains the compilation-database replay utility used for the
Android experiment. The Android source checkout, Soong output, CTU ASTs,
rendered HTML reports, and baseline plist reports are intentionally not
distributed.  Complete sanitized ours plist reports and their `metadata.json`
are provided as an external release package; see
[`../results/README.md`](../results/README.md).

After extracting that package, reproduce the ours-only report and evidence
statistics with:

```bash
python experiments/analysis/real_project_evidence.py --dataset android
```

## 1. Obtain and configure Android

```bash
export AOSP_ROOT=/absolute/path/to/aosp
export ARTIFACT_ROOT=/absolute/path/to/Paper1_submitted

repo init -u https://android.googlesource.com/platform/manifest \
  -b android-15.0.0_r1
repo sync -c

cd "$AOSP_ROOT"
source build/envsetup.sh
lunch aosp_cf_x86_64_phone-trunk_staging-eng
```

## 2. Generate the Soong compilation database

```bash
export SOONG_GEN_COMPDB=1
export SOONG_GEN_COMPDB_DEBUG=1
export SOONG_LINK_COMPDB_TO="$AOSP_ROOT"

m libbase libutils liblog libbinder libui libgui
```

Soong links the generated database to `$AOSP_ROOT/compile_commands.json`.

## 3. Build the analyzer configurations

Build the baseline from the official `llvmorg-18.1.6` source. For the artifact
configuration, start from a separate checkout of the same tag and overlay the
contents of `clang18/` onto its `clang/` directory before configuring LLVM:

```bash
cp -a "$ARTIFACT_ROOT/clang18/." "$OURS_LLVM_ROOT/clang/"
```

Configure both checkouts with the same release options and targets. The LLVM 15
commands in the root README show the corresponding CMake layout; substitute the
18.1.6 source, build, and installation directories. Keep the two `bin`
directories separate so that the compilation database can be regenerated with
the toolchain used by each CodeChecker run.

## 4. Replay the database with Clang 18.1.6

The script retains the original module selection, generated-input checks,
argument repair, and unanalyzable-action exclusions. It only replaces embedded
machine paths with explicit parameters. It may be run from its artifact path or
copied into the Android `tools` directory:

```bash
cp "$ARTIFACT_ROOT/experiments/android/tools/clang18_replay.py" \
  "$AOSP_ROOT/tools/clang18_replay.py"

export CLANG18_BIN=/absolute/path/to/clang-18.1.6/bin

python3 "$AOSP_ROOT/tools/clang18_replay.py" \
  --aosp-root "$AOSP_ROOT" \
  --input-compdb "$AOSP_ROOT/compile_commands.json" \
  --clang-bin "$CLANG18_BIN" \
  --emit-compdb "$AOSP_ROOT/compile_commands_clang18_clean.json" \
  --built-variants-only \
  --drop-unanalyzable
```

In the paper's build environment, this command emits 30,634 analysis actions.
The `--clang-bin` value must correspond to the Clang installation used by the
following CodeChecker run. Regenerate the cleaned database after switching
between baseline and artifact toolchains if their installation paths differ.

## 5. Run CodeChecker

Both configurations use CodeChecker 6.26.2 and the same checker groups. Put the
appropriate Clang 18.1.6 binaries on `PATH` before each run.

```bash
# Baseline Clang/CSA:
CodeChecker analyze \
  --ctu \
  --ctu-ast-mode load-from-pch \
  compile_commands_clang18_clean.json \
  -o reports_raw2_64 \
  --disable-all \
  --enable cplusplus \
  --enable core \
  --enable unix \
  --disable core.UndefinedBinaryOperatorResult \
  --keep-gcc-intrin

# Artifact analyzer:
CodeChecker analyze \
  --ctu \
  --ctu-ast-mode load-from-pch \
  compile_commands_clang18_clean.json \
  -o reports_exp2_64 \
  --skip-pre-analysis \
  --disable-all \
  --enable cplusplus \
  --enable core \
  --enable unix \
  --disable core.UndefinedBinaryOperatorResult \
  --keep-gcc-intrin
```
