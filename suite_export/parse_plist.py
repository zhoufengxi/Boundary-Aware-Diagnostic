#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import plistlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Union


@dataclass
class SourceLocation:
    file_id: Optional[int]
    file_path: Optional[str]
    line: Optional[int]
    col: Optional[int]

    def to_dict(self) -> Dict[str, Any]:
        return {
            "file_id": self.file_id,
            "file_path": self.file_path,
            "line": self.line,
            "col": self.col,
        }


@dataclass
class SourceRange:
    start: SourceLocation
    end: SourceLocation

    def to_dict(self) -> Dict[str, Any]:
        return {
            "start": self.start.to_dict(),
            "end": self.end.to_dict(),
        }


@dataclass
class CSAEvent:
    """
    A single event node in the path of a CSA plist.

    For example:
      message = "Entering loop body"
    is stored as:
      event_name = "Entering loop body"
    """
    index: int
    event_name: str
    location: Optional[SourceLocation]
    ranges: List[SourceRange] = field(default_factory=list)
    depth: Optional[int] = None
    kind: str = "event"

    # Keep the raw plist node for later debugging or extension
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    @property
    def message(self) -> str:
        return self.event_name

    def to_dict(self) -> Dict[str, Any]:
        return {
            "index": self.index,
            "kind": self.kind,
            "event_name": self.event_name,
            "message": self.message,
            "depth": self.depth,
            "location": self.location.to_dict() if self.location else None,
            "ranges": [r.to_dict() for r in self.ranges],
        }


@dataclass
class CSABugReport:
    """
    A CSA bug report, corresponding to one dict in the plist diagnostics array.
    """
    bug_type: str
    checker_name: str
    path: List[CSAEvent]

    category: Optional[str] = None
    location: Optional[SourceLocation] = None
    issue_hash: Optional[str] = None

    # Optional statistics: the raw path may contain both control and event nodes
    raw_path_len: int = 0
    control_count: int = 0

    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "bug_type": self.bug_type,
            "checker_name": self.checker_name,
            "category": self.category,
            "issue_hash": self.issue_hash,
            "location": self.location.to_dict() if self.location else None,
            "raw_path_len": self.raw_path_len,
            "control_count": self.control_count,
            "event_count": len(self.path),
            "path": [e.to_dict() for e in self.path],
        }


def _resolve_file(file_id: Any, files: List[str]) -> Optional[str]:
    if not isinstance(file_id, int):
        return None
    if 0 <= file_id < len(files):
        return files[file_id]
    return None


def _parse_location(
    loc: Optional[Dict[str, Any]],
    files: List[str],
) -> Optional[SourceLocation]:
    if not isinstance(loc, dict):
        return None

    file_id = loc.get("file")
    if not isinstance(file_id, int):
        file_id = None

    return SourceLocation(
        file_id=file_id,
        file_path=_resolve_file(file_id, files),
        line=loc.get("line"),
        col=loc.get("col"),
    )


def _parse_ranges(
    ranges_obj: Any,
    files: List[str],
) -> List[SourceRange]:
    """
    CSA plist ranges generally look like this:

    ranges:
      [
        [
          {file, line, col},   # start
          {file, line, col},   # end
        ],
        ...
      ]
    """
    result: List[SourceRange] = []

    if not isinstance(ranges_obj, list):
        return result

    for item in ranges_obj:
        if not isinstance(item, list) or len(item) != 2:
            continue

        start = _parse_location(item[0], files)
        end = _parse_location(item[1], files)

        if start is None or end is None:
            continue

        result.append(SourceRange(start=start, end=end))

    return result


def _parse_event(
    event_index: int,
    path_node: Dict[str, Any],
    files: List[str],
) -> CSAEvent:
    message = path_node.get("message", "")

    return CSAEvent(
        index=event_index,
        event_name=message,
        location=_parse_location(path_node.get("location"), files),
        ranges=_parse_ranges(path_node.get("ranges"), files),
        depth=path_node.get("depth"),
        kind=path_node.get("kind", "event"),
        raw=path_node,
    )


def _parse_bug_report(
    diag: Dict[str, Any],
    files: List[str],
) -> CSABugReport:
    raw_path = diag.get("path", [])
    if not isinstance(raw_path, list):
        raw_path = []

    events: List[CSAEvent] = []
    control_count = 0

    for node in raw_path:
        if not isinstance(node, dict):
            continue

        kind = node.get("kind")

        if kind == "event":
            events.append(_parse_event(len(events), node, files))
        elif kind == "control":
            control_count += 1

    return CSABugReport(
        bug_type=diag.get("description", ""),
        checker_name=diag.get("check_name", ""),
        category=diag.get("category"),
        location=_parse_location(diag.get("location"), files),
        issue_hash=diag.get("issue_hash_content_of_line_in_context"),
        path=events,
        raw_path_len=len(raw_path),
        control_count=control_count,
        raw=diag,
    )


def parse_csa_plist(plist_path: Union[str, Path]) -> List[CSABugReport]:
    """
    Parse a plist report generated by Clang Static Analyzer / CodeChecker.

    Args:
      plist_path: path to the plist file

    Returns:
      List[CSABugReport]
      A single plist may contain multiple bug reports.
    """
    plist_path = Path(plist_path)

    with plist_path.open("rb") as f:
        data = plistlib.load(f)

    files = data.get("files", [])
    if not isinstance(files, list):
        files = []

    diagnostics = data.get("diagnostics", [])
    if not isinstance(diagnostics, list):
        diagnostics = []

    reports: List[CSABugReport] = []

    for diag in diagnostics:
        if not isinstance(diag, dict):
            continue
        reports.append(_parse_bug_report(diag, files))

    return reports


def print_report_summary(reports: List[CSABugReport]) -> None:
    for i, report in enumerate(reports):
        print(f"========== Report #{i} ==========")
        print(f"bug_type    : {report.bug_type}")
        print(f"checker_name: {report.checker_name}")
        print(f"category    : {report.category}")
        print(f"issue_hash  : {report.issue_hash}")

        if report.location:
            print(
                "location    : "
                f"{report.location.file_path}:"
                f"{report.location.line}:"
                f"{report.location.col}"
            )

        print(f"raw_path_len : {report.raw_path_len}")
        print(f"control_count: {report.control_count}")
        print(f"event_count  : {len(report.path)}")
        print()

        for event in report.path:
            loc = event.location
            if loc:
                loc_text = f"{loc.file_path}:{loc.line}:{loc.col}"
            else:
                loc_text = "<unknown>"

            print(f"  [{event.index}] {event.event_name}")
            print(f"      location: {loc_text}")

        print()


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("plist_path")
    args = parser.parse_args()

    reports = parse_csa_plist(args.plist_path)
    print_report_summary(reports)
