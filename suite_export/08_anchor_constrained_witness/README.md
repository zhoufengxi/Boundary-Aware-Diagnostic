# Anchor-Constrained Diagnostic Witness Tests

This group targets report-path construction after the analyzer has already
reached a terminal defect. A baseline failure means that the baseline analyzer
still emits the target diagnostic, but its selected path omits a required
witness. A modified-analyzer success means that the same terminal defect is
reported with the expected anchor-bearing witness.

All three standard-algorithm tests and the core test use the same three-way
terminal branch: double-free, UAF, and leak. Each allocation is stored in a
trivial `AnchorData` object. The allocation-bearing path passes a constant
function pointer, the object, and the branch through one CTU dispatcher before
returning to a callback defined in the driver translation unit.

Each callback is also analyzed as an independent entry. For double-free and UAF
this creates a shorter report to the same marked terminal location without an
allocation origin. For leak, an independent callback cannot create a leak
report because its pointer parameter is not known to be allocated; the
allocation-bearing algorithm path instead competes between a short zero-
iteration report without `branch` evidence and the callback path with the
branch witness.

## Expected results

| Test | Baseline | Modified analyzer |
|---|---|---|
| `for_each_leak_anchor.cpp` | Double-free/UAF: `has_allocated=False`, `has_assume=True`. Leak: `has_allocated=True`, `has_assume=False`. | All three matched terminals have `has_allocated=True`, `has_assume=True`. |
| `transform_leak_anchor_variant.cpp` | Double-free/UAF: `has_allocated=False`, `has_assume=True`. Leak: `has_allocated=True`, `has_assume=False`. | All three matched terminals have `has_allocated=True`, `has_assume=True`. |
| `stored_for_each_leak_anchor_variant.cpp` | Double-free/UAF: `has_allocated=False`, `has_assume=True`. Leak: `has_allocated=True`, `has_assume=False`. | All three matched terminals have `has_allocated=True`, `has_assume=True`. |
| `double_free_report_path_selection_anchor.cpp` | Double-free/UAF: `has_allocated=False`, `has_assume=True`. Its leak remains allocation-bearing because no independently reported callback leak exists. | Double-free/UAF retain the same terminal and gain allocation evidence; the leak retains its allocation and branch witnesses. |

`anchor_callback_dispatch.cpp` is the only auxiliary CTU unit. It contains
neither an allocation nor a terminal memory operation, so the result collector
excludes its standalone analysis action.

The suite is intended for compile/analyze experiments, not for executing the
programs.
