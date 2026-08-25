# Diagnostic State Suite v3 (Export)

Regression and variant tests for **Boundary-Aware Diagnostic State Preservation**, plus
the scripts used to run Clang Static Analyzer (via CodeChecker) over the suite and
summarize the results.

## Layout

- `01_closure_materialization/` — lambda closure and captured-state materialization
- `02_callback_state/` — callback-carried state through template-instantiated algorithms
- `03_subobject_invalidation/` — non-trivial subobject construction and sibling-field state
- `04_receiver_field_state/` — receiver/member/getter based field-state propagation
- `05_template_semantic_target/` — template semantic-target availability
- `06_dependency_import_conflict/` — dependency-level import visibility and include perturbation
- `07_composite_dispatch/` — composed boundary cases
- `08_anchor_constrained_witness/` — anchor-constrained witness cases
- `99_negative_controls/` — controls where precise state preservation should remain conservative
- `main1.cpp` — standalone `std::for_each` lambda test case
- `Makefile` — builds all test groups (`SUBDIRS`)
- `run_analysis.sh` — one-shot build + CTU analysis + HTML report pipeline
- `collect_all_reports.py` — aggregates all plist reports and writes an HTML summary
- `parse_plist.py` — plist parser used by `collect_all_reports.py`

Naming convention:

- Core tests use descriptive names, e.g. `mixed_capture_string.cpp`.
- Controlled variants end with `_variant.cpp`.
- Negative/control tests live under `99_negative_controls/`.

## Requirements

- `clang++` with C++17 support (test cases are compiled with `-std=c++17`)
- GNU `make`
- [bear](https://github.com/rizsotto/Bear) — generates `compile_commands.json`
- [CodeChecker](https://github.com/Ericsson/codechecker) — runs the Clang Static
  Analyzer with CTU (`--ctu --ctu-ast-mode load-from-pch`) and renders HTML reports
- Python 3 with `pandas` and `tqdm` (only for `collect_all_reports.py`)

## Usage

### Build only

```sh
make            # build all groups
make clean      # remove all .o files
```

Extra compiler flags can be appended, e.g.:

```sh
make EXTRA_CXXFLAGS="-O2 -g"
```

### Full analysis pipeline

```sh
./run_analysis.sh
```

This performs, in order:

1. `make clean` and removes `reports/`, `html_out/`, `compile_commands.json`
2. `bear make` — rebuild while recording the compilation database
3. `CodeChecker analyze --ctu` — static analysis with the
   `cplusplus.NewDeleteLeaks` and `cplusplus.NewDelete` checkers into `reports/`
4. `CodeChecker parse -e html` — renders the reports as HTML into `html_out/`

### Aggregate the reports

After an analysis run (i.e. when `reports/` exists):

```sh
python3 collect_all_reports.py                 # uses ./reports, writes summary_report.html
python3 collect_all_reports.py reports -o out.html
```

The script parses every `*.plist` in the given reports directory, classifies each
finding as `leak` / `double_free` / `UAF`, and writes a per-directory summary table
to an HTML file.
