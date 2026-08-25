# OpenHarmony 6.1 Reproduction

This directory contains the build-toolchain overlay and compilation-database
utilities used for the OpenHarmony experiment. Run the commands on Ubuntu in a
case-sensitive filesystem. The OpenHarmony source tree, build output, CTU ASTs,
rendered HTML reports, and baseline plist reports are intentionally not
distributed.  Complete sanitized ours plist reports are provided as an
external release package; see [`../results/README.md`](../results/README.md).

After extracting that package, reproduce the ours-only report and evidence
statistics with:

```bash
python experiments/analysis/real_project_evidence.py --dataset openharmony
```

## 1. Obtain OpenHarmony

```bash
export OHOS_ROOT=/absolute/path/to/Harmony
export ARTIFACT_ROOT=/absolute/path/to/Paper1_submitted

repo init -u https://gitcode.com/openharmony/manifest.git \
  -b OpenHarmony-6.1-Release --no-repo-verify
repo sync -c
repo forall -c 'git lfs pull'
```

The paper uses the `rk3568` product with an ARM64 target.

## 2. Install the compilation-database overlay

Copy the supplied files over the corresponding files in the checkout. Keep a
separate clean checkout if the original build files are also needed.

```bash
cp -a "$ARTIFACT_ROOT/experiments/openharmony/toolchain_overlay/." \
  "$OHOS_ROOT/build/toolchain/"
chmod +x \
  "$OHOS_ROOT/build/toolchain/clang_static_analyzer_wrapper.py" \
  "$OHOS_ROOT/build/toolchain/use_bear_compilation_database_wrapper.py"
```

The Bear wrapper writes one compilation-database shard per compiler process.
Set the shard directory before invoking the build. Logging is optional.

```bash
export OHOS_COMPDB_SHARDS="$OHOS_ROOT/single_db"
export OHOS_COMPDB_LOG="$OHOS_ROOT/compdb_capture.log"  # optional
mkdir -p "$OHOS_COMPDB_SHARDS"

cd "$OHOS_ROOT"
bash ./build.sh --product-name rk3568 --target-cpu arm64
```

`use_bear_compilation_database.gni` contains only the Boolean capture switch.
The output location is controlled by `OHOS_COMPDB_SHARDS`; no checkout-specific
path is compiled into the GN configuration.

## 3. Merge and filter the database

The merge tool reads shards in filename order and retains the experiment's
original handling of entries with a separate `-resource-dir` argument. The
filter then keeps C++ ARM/AArch64 OpenHarmony actions, removes sources listed in
the CTU connection directory, and excludes ArkCompiler sources.

```bash
python3 "$ARTIFACT_ROOT/experiments/openharmony/tools/merge_compile_databases.py" \
  --input-dir "$OHOS_COMPDB_SHARDS" \
  --output "$OHOS_ROOT/merged_compile_commands.json"

python3 "$ARTIFACT_ROOT/experiments/openharmony/tools/filter_openharmony_compile_database.py" \
  --input "$OHOS_ROOT/merged_compile_commands.json" \
  --output "$OHOS_ROOT/compile_commands_openharmony_cpp.json" \
  --source-root "$OHOS_ROOT" \
  --ctu-connections "$OHOS_ROOT/out/rk3568/placeholder_001/ctu_connections"
```

If the CTU connection directory is absent, the filter emits a warning and
skips only that filtering stage; the other filters remain active.

## 4. Run CodeChecker

Build and select the official Clang/CSA configuration for the baseline and the
artifact implementation for `ours`. Both runs use CodeChecker 6.26.2 and the
same compilation database and checker groups.

```bash
# With the baseline Clang/CSA binaries on PATH:
CodeChecker analyze \
  --ctu \
  --ctu-ast-mode load-from-pch \
  "$OHOS_ROOT/compile_commands_openharmony_cpp.json" \
  -o "$OHOS_ROOT/reports_raw_64" \
  --disable-all \
  --enable cplusplus \
  --enable core \
  --enable unix \
  --disable core.UndefinedBinaryOperatorResult \
  --keep-gcc-intrin

# Repeat with the artifact analyzer binaries on PATH:
CodeChecker analyze \
  --ctu \
  --ctu-ast-mode load-from-pch \
  "$OHOS_ROOT/compile_commands_openharmony_cpp.json" \
  -o "$OHOS_ROOT/reports_exp_64" \
  --disable-all \
  --enable cplusplus \
  --enable core \
  --enable unix \
  --disable core.UndefinedBinaryOperatorResult \
  --keep-gcc-intrin
```

The report directories above match the names used by the paper's aggregation
scripts. They are outputs and are not required to inspect the implementation.
