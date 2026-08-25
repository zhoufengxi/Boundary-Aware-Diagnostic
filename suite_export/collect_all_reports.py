#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Collect CSABugReports from all plist files in the reports/ directory into a single list.
"""

from pathlib import Path
from typing import List
from parse_plist import CSABugReport, parse_csa_plist
import pandas as pd
from tqdm import tqdm
# Files without an `int *p = new int(<number>);` statement (callee/worker/listener and other helper files)
EXCLUDED_FILES = [
    "05_template_semantic_target/template_direct_callee.cpp",
    "05_template_semantic_target/template_outer_callee.cpp",
    "06_dependency_import_conflict/worker.cpp",
    "07_composite_dispatch/listener.cpp",
    "08_anchor_constrained_witness/anchor_callback_dispatch.cpp",
    "99_negative_controls/user_conflict_negative/worker.cpp",
]

def collect_all_reports(reports_dir: str) -> List[CSABugReport]:
    """
    Parse all .plist files in reports_dir and combine all CSABugReports into a single list.

    Args:
        reports_dir: path to the reports directory

    Returns:
        List[CSABugReport] — combined bug reports from all plist files
    """
    base = Path(reports_dir)
    all_reports: List[CSABugReport] = []

    for plist_file in sorted(base.glob("*.plist")):
        try:
            reports = parse_csa_plist(plist_file)
            all_reports.extend(reports)
        except Exception as e:
            print(f"[WARN] Failed to parse {plist_file.name}: {e}")

    return all_reports


def _bug_kind(report: CSABugReport) -> str:
    """
    Get the bug kind of a CSABugReport.

    Args:
        report: CSABugReport instance """
    kind = report.bug_type
    if "Potential leak of memory pointed to" in kind or "Potential memory leak" in kind:
        return "leak"
    elif "Attempt to free released memory" in kind:
        return "double_free"
    elif "Use of memory after it is freed" in kind:
        return "UAF"
    else:
        return ""


def _has_allocated(report: CSABugReport) -> bool:
    """
    Check whether a CSABugReport involves a memory allocation.

    Args:
        report: CSABugReport instance
    """
    for event in report.path:
        if "Memory is allocated" in event.message:
            return True
    return False

def _has_assume(report: CSABugReport) -> bool:
    """
    Check whether a CSABugReport contains an assumption.

    Args:
        report: CSABugReport instance
    """
    for event in report.path:
        if "Assuming 'branch'" in event.message:
            return True
    return False


def statistics(reports: List[CSABugReport]) -> pd.DataFrame:
    """
    Store statistics of CSABugReports into a pandas DataFrame.

    Args:
        reports: list of CSABugReport

    Returns:
        pd.DataFrame — with columns file_path, bug_kind, has_allocated, has_assume
    """
    rows = []
    for report in tqdm(reports):
        kind = _bug_kind(report)
        if kind in ("leak", "double_free", "UAF"):
            # Attribute the report to the file of the first event in the path
            first_event_file = (
                report.path[0].location.file_path
                if report.path
                else report.location.file_path
            )
            rows.append({
                "file_path": first_event_file,
                "bug_kind": kind,
                "has_allocated": _has_allocated(report),
                "has_assume": _has_assume(report),
            })
        else:
            continue
            # assert False, f"Unknown bug kind: {kind!r} from '{report.bug_type}'"
    df = pd.DataFrame(rows)

    # For leak kind: when a file has multiple leaks, drop those with has_assume=False
    # and keep those with has_assume=True
    leak_mask = df["bug_kind"] == "leak"
    leak_df = df[leak_mask]
    leak_counts = leak_df.groupby("file_path").size()
    multi_leak_files = leak_counts[leak_counts > 1].index

    drop_indices = []
    for f in multi_leak_files:
        file_leaks = df[df["file_path"] == f]
        # Only drop rows with has_assume=False, keep rows with has_assume=True
        to_drop = file_leaks[(file_leaks["bug_kind"] == "leak") & (~file_leaks["has_assume"])]
        drop_indices.extend(to_drop.index)

    df = df.drop(drop_indices).reset_index(drop=True)

    # Drop files without an `int *p = new int(<number>);` statement
    exclude_mask = df["file_path"].apply(
        lambda p: any(p.endswith(f) for f in EXCLUDED_FILES)
    )
    df = df[~exclude_mask].reset_index(drop=True)

    return df


def summary_table(df: pd.DataFrame) -> pd.DataFrame:
    """
    Build a summary table grouped by test case directory and bug kind.

    Each row corresponds to a (category, bug_kind) combination, counting:
      - allocated: number with has_allocated=True
      - assume:    number with has_assume=True
      - both:      number with has_allocated=True and has_assume=True
      - total:     total number of reports in the combination

    Finally appends TOTAL rows aggregated by bug_kind.

    Args:
        df: DataFrame returned by statistics()

    Returns:
        pd.DataFrame — with columns category, bug_kind, allocated, assume, both, total
    """
    from pathlib import Path

    df = df.copy()
    # Extract the parent directory name as the category (e.g. "01_closure_materialization")
    df["category"] = df["file_path"].apply(lambda p: Path(p).parent.name)

    # Compute the "both conditions" column
    df["has_both"] = df["has_allocated"] & df["has_assume"]

    # Group and aggregate by category + bug_kind
    grouped = (
        df.groupby(["category", "bug_kind"])
        .agg(
            allocated=("has_allocated", "sum"),
            assume=("has_assume", "sum"),
            both=("has_both", "sum"),
            total=("file_path", "count"),
        )
        .reset_index()
    )

    # Sort: first by category, then by a fixed bug_kind order
    bug_order = {"leak": 0, "double_free": 1, "UAF": 2}
    grouped["_bug_sort"] = grouped["bug_kind"].map(bug_order)
    grouped = grouped.sort_values(["category", "_bug_sort"]).drop(
        columns=["_bug_sort"]
    )

    # ---- Append summary rows (aggregate all categories by bug_kind) ----
    total_rows = (
        grouped.groupby("bug_kind")
        .agg(
            category=("bug_kind", lambda _: "TOTAL"),
            allocated=("allocated", "sum"),
            assume=("assume", "sum"),
            both=("both", "sum"),
            total=("total", "sum"),
        )
        .reset_index()
    )
    total_rows = total_rows.sort_values("bug_kind", key=lambda s: s.map(bug_order))
    total_rows["category"] = "TOTAL"

    result = pd.concat([grouped, total_rows], ignore_index=True)
    return result



def _write_summary_html(summary: pd.DataFrame, output_path: str) -> None:
    """
    Write the summary table to an HTML file that can be opened directly in a browser.

    Args:
        summary: DataFrame returned by summary_table()
        output_path: output HTML file path
    """
    html_template = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>Bug Summary Statistics</title>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; margin: 40px; }}
  h1 {{ color: #333; }}
  table {{ border-collapse: collapse; width: 100%; max-width: 900px; }}
  th, td {{ padding: 8px 12px; text-align: left; border: 1px solid #ddd; }}
  th {{ background: #4CAF50; color: white; position: sticky; top: 0; }}
  tr:nth-child(even) {{ background: #f9f9f9; }}
  tr:hover {{ background: #e8f5e9; }}
  .total-row {{ font-weight: bold; background: #fff3cd !important; }}
  .total-row td {{ border-top: 3px solid #f0ad4e; }}
</style>
</head>
<body>
<h1>Bug Summary Statistics — Test Case Directories x Bug Kinds</h1>
{table_html}
<p><em>Generated at: {timestamp}</em></p>
</body>
</html>"""
    from datetime import datetime

    rows = []
    for _, row in summary.iterrows():
        cls = 'class="total-row"' if row["category"] == "TOTAL" else ""
        rows.append(
            f'<tr {cls}>'
            f'<td>{row["category"]}</td>'
            f'<td>{row["bug_kind"]}</td>'
            f'<td>{row["allocated"]}</td>'
            f'<td>{row["assume"]}</td>'
            f'<td>{row["both"]}</td>'
            f'<td>{row["total"]}</td>'
            f'</tr>'
        )
    table_html = (
        '<table>\n<thead><tr>'
        '<th>category</th><th>bug_kind</th>'
        '<th>allocated</th><th>assume</th><th>both</th><th>total</th>'
        '</tr></thead>\n<tbody>\n'
        + '\n'.join(rows)
        + '\n</tbody></table>'
    )
    html = html_template.format(
        table_html=table_html,
        timestamp=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    )
    Path(output_path).write_text(html, encoding="utf-8")
    print(f"Summary HTML written to: {output_path}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Collect CSABugReports from all plists in the reports directory"
    )
    parser.add_argument(
        "reports_dir",
        nargs="?",
        default="reports",
        help="path to the reports directory (default: reports)",
    )
    parser.add_argument(
        "-o", "--output",
        default="summary_report.html",
        help="output HTML file path (default: summary_report.html)",
    )
    args = parser.parse_args()

    reports = collect_all_reports(args.reports_dir)
    print(f"Parsed {len(reports)} CSABugReports in total")

    df = statistics(reports)
    print(df.to_string())

    print("\n" + "=" * 60)
    print("Summary: Test Case Directories x Bug Kinds")
    print("=" * 60)
    summary = summary_table(df)
    print(summary.to_string(index=False))

    _write_summary_html(summary, args.output)
