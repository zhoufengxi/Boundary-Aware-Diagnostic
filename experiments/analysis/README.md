# Real-project plist analysis

`real_project_evidence.py` uses only the Python standard library and never
modifies its input reports.  It prints the complete Markdown summary to the
terminal and writes CSV, JSON, and Markdown results under
`experiments/results/analysis_output/`.

## Analyze the released ours results

Download and extract both packages as described in
`../results/README.md`.  From the artifact root, run:

```bash
python experiments/analysis/real_project_evidence.py
```

The default `ours-only` mode reports action and plist counts, checker and
defect distributions, allocation/release/branch evidence, lifecycle
completeness for applicable memory diagnostics, parse errors, and the
OpenHarmony case-study counts.  Select one project with
`--dataset openharmony` or `--dataset android`; use `--jobs N` to control
parallel plist parsing.

## Optional paired comparison

Baseline reports are intentionally not distributed.  If they are available
locally, the original evidence-independent comparison can be reproduced with:

```bash
python experiments/analysis/real_project_evidence.py \
  --mode paired \
  --openharmony-baseline /path/to/reports_raw_64 \
  --android-baseline /path/to/reports_raw2_64
```

Terminal bug matching does not use allocation, release, or branch evidence.
Duplicate instances are paired with evidence-stripped path skeletons before
witness transitions are calculated.  The saved `expected/paired` summary is
the paper-analysis output, but it cannot be independently recomputed without
the undistributed baseline reports.

## Tests and release packaging

Run all synthetic tests from this directory:

```bash
python -m unittest -v
```

`package_plist_results.py` creates deterministic, path-sanitized `.tar.zst`
packages.  It requires the `zstd` command and validates every plist before it
is added.  Private path prefixes are command-line inputs and are never written
to the public manifest. Input preparation uses a bounded worker pool while tar
members remain in deterministic order. `verify_release_archives.py` then
stream-verifies every published member against its sanitized source and parses
every plist as XML. See `../results/README.md` for the release layout.
