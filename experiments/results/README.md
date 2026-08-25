# Released project-scale plist results

The external release contains only reports produced by our analyzer:

- `openharmony-ours-plists.tar.zst`: 2,976 OpenHarmony plist files from
  `reports_exp_64`;
- `android-ours-plists.tar.zst`: 30,495 Android plist files and the associated
  `metadata.json` from `reports_exp2_64`.

Baseline plist files are not distributed.  Consequently, the released inputs
reproduce ours-only report and evidence statistics.  Recomputing
baseline-versus-ours recovery requires separately supplied baseline inputs.

## Integrity and extraction

Place downloaded archives or all of their `.part-NNNN` files beside the
release `manifest.json` and `SHA256SUMS`.  Verify each distributed file:

```bash
sha256sum -c SHA256SUMS
```

If an archive is split, concatenate its parts in lexical order first:

```bash
cat android-ours-plists.tar.zst.part-* > android-ours-plists.tar.zst
```

Extract from the artifact root into the location used by the analysis script:

```bash
mkdir -p experiments/results/unpacked
tar --zstd -xf openharmony-ours-plists.tar.zst \
  -C experiments/results/unpacked
tar --zstd -xf android-ours-plists.tar.zst \
  -C experiments/results/unpacked
```

The resulting directories are:

```text
experiments/results/unpacked/
├── openharmony/reports_exp_64/
└── android/reports_exp2_64/
```

Then run:

```bash
python experiments/analysis/real_project_evidence.py
```

## Sanitization

The packages are deterministic sanitized copies.  Source-tree prefixes are
mapped to `/artifact/openharmony` and `/artifact/android`; compiler and other
tool paths are mapped to `/artifact/toolchain`.  The original reports are not
modified. Packaging performs only configured literal path-prefix replacements,
verifies that diagnostic cardinality is unchanged, and rejects remaining
personal home-directory paths. After extraction, the standalone analysis parses
every sanitized plist and validates all diagnostic and path fields against the
saved ours-only metrics.

`manifest.json` records file counts, byte counts, diagnostic counts,
replacement categories, archive hashes, and any split parts.  It intentionally
does not disclose the private source prefixes.

## Derived results

- `expected/ours-only/` contains results generated directly from the released
  ours reports.
- `expected/paired/` contains the compact paper-analysis summary generated
  when both configurations were locally available.
- `analysis_output/` is ignored and may be regenerated at any time.

The binary archives are external release assets and are not stored in Git or
Git LFS.
