#!/usr/bin/env python3
"""Analyze the released real-project plist reports without rerunning analyzers.

The default ``ours-only`` mode reports the diagnostics and evidence contained
in the released OpenHarmony and Android result sets.  The optional ``paired``
mode is deliberately split into four independent layers:

1. pair analysis actions without consulting diagnostics;
2. identify the same terminal bug without consulting path evidence;
3. pair duplicate diagnostic paths using an evidence-stripped skeleton; and
4. compare allocation, release, and branch witnesses only after pairing.

This separation is important: using an allocation or release location to pair
reports would make the evidence being measured part of the matching rule.

The script uses only the Python standard library.  It never modifies its plist
inputs and writes all derived artifacts to a separate output tree.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import hashlib
import html
import json
import math
import mmap
import os
import plistlib
import re
import shlex
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from functools import lru_cache
from pathlib import Path, PurePosixPath
from typing import Any, Iterable, Iterator, Sequence


TARGET_CHECKER_PREFIXES = ("cplusplus.", "core.", "unix.")
LIFECYCLE_CHECKERS = {
    "cplusplus.NewDelete",
    "cplusplus.NewDeleteLeaks",
    "unix.Malloc",
}
EVIDENCE_NAMES = ("allocation", "release", "branch")
MAX_EXACT_ASSIGNMENT_CELLS = 400
MAX_EXACT_SIMILARITY_TOKENS = 100_000
STREAMING_PLIST_BYTES = 1 * 1024 * 1024
PROJECT_MARKERS = (
    "/aosp/",
    "/openharmony/",
    "/openharmony-v5.0-beta1/",
    "/harmony/",
)


@dataclass(frozen=True)
class Location:
    path: str
    line: int
    col: int

    def token(self) -> str:
        return f"{self.path}:{self.line}:{self.col}"


@dataclass
class Report:
    dataset: str
    side: str
    action_id: str
    plist_name: str
    diagnostic_index: int
    checker: str
    kind: str
    description: str
    normalized_description: str
    issue_hash: str
    terminal: Location
    allocation_locations: tuple[str, ...]
    release_locations: tuple[str, ...]
    branch_locations: tuple[str, ...]
    skeleton: tuple[str, ...]
    path_events: int

    @property
    def report_id(self) -> str:
        return f"{self.action_id}|{self.plist_name}|{self.diagnostic_index}"

    @property
    def terminal_key(self) -> tuple[str, str, str, int, int]:
        return (
            self.checker,
            self.kind,
            self.terminal.path,
            self.terminal.line,
            self.terminal.col,
        )

    @property
    def global_issue_key(self) -> tuple[str, str, str, int, int, str]:
        return (*self.terminal_key, self.issue_hash)

    @property
    def checker_family(self) -> str:
        return self.checker.split(".", 1)[0]

    @property
    def lifecycle_applicable(self) -> bool:
        return self.checker in LIFECYCLE_CHECKERS and self.kind in {
            "leak",
            "uaf",
            "double_free",
        }

    def has(self, evidence: str) -> bool:
        return bool(getattr(self, f"{evidence}_locations"))

    def lifecycle_complete(self) -> bool | None:
        if not self.lifecycle_applicable:
            return None
        if self.kind == "leak":
            return self.has("allocation")
        if self.kind in {"uaf", "double_free"}:
            return self.has("allocation") and self.has("release")
        return None


@dataclass
class ActionGroup:
    dataset: str
    action_id: str
    source_path: str
    status: str
    reason: str
    action_count: int
    baseline_plists: tuple[Path, ...] = field(default_factory=tuple)
    ours_plists: tuple[Path, ...] = field(default_factory=tuple)
    baseline_configs: tuple[str, ...] = field(default_factory=tuple)
    ours_configs: tuple[str, ...] = field(default_factory=tuple)


@dataclass
class IssueGroup:
    dataset: str
    action_id: str
    key: tuple[str, str, str, int, int]
    baseline: list[Report]
    ours: list[Report]
    status: str
    match_tier: str
    reason: str = ""


def normalize_path(value: str, working_directory: str | None = None) -> str:
    """Return a stable project-relative POSIX path where possible."""
    text = (value or "").replace("\\", "/")
    if working_directory:
        wd = working_directory.replace("\\", "/").rstrip("/")
        if text.startswith(wd + "/"):
            text = text[len(wd) + 1 :]
    lower = text.lower()
    for marker in PROJECT_MARKERS:
        pos = lower.rfind(marker)
        if pos >= 0:
            text = text[pos + len(marker) :]
            break
    while text.startswith("./"):
        text = text[2:]
    try:
        text = str(PurePosixPath(text))
    except Exception:
        pass
    return text


def normalize_message(value: str) -> str:
    text = re.sub(r"'[^']*'", "'<symbol>'", value or "")
    text = re.sub(r'"[^"]*"', '"<symbol>"', text)
    text = re.sub(r"\b0x[0-9a-fA-F]+\b", "<addr>", text)
    text = re.sub(r"\s+", " ", text).strip().lower()
    return text


def classify_kind(checker: str, description: str) -> str:
    text = (description or "").lower()
    if checker.endswith("NewDeleteLeaks") or "leak" in text:
        return "leak"
    if "use of memory after it is freed" in text or "use after free" in text:
        return "uaf"
    if (
        "attempt to free released memory" in text
        or "attempt to delete released memory" in text
        or "double free" in text
        or "already released" in text
    ):
        return "double_free"
    return "other"


def checker_is_in_scope(checker: str) -> bool:
    return checker.startswith(TARGET_CHECKER_PREFIXES)


def location_from_dict(raw: dict[str, Any] | None, files: Sequence[str]) -> Location:
    raw = raw or {}
    file_index = raw.get("file", -1)
    path = "<unknown>"
    if isinstance(file_index, int) and 0 <= file_index < len(files):
        path = normalize_path(str(files[file_index]))
    return Location(path, int(raw.get("line", 0) or 0), int(raw.get("col", 0) or 0))


def event_location(event: dict[str, Any], files: Sequence[str]) -> Location:
    loc = event.get("location")
    if isinstance(loc, dict):
        return location_from_dict(loc, files)
    edges = event.get("edges") or []
    if edges:
        endpoint = (edges[-1].get("end") or [{}])[-1]
        if isinstance(endpoint, dict):
            return location_from_dict(endpoint, files)
    return Location("<unknown>", 0, 0)


def evidence_kind(message: str, is_terminal: bool) -> str | None:
    text = (message or "").strip().lower()
    if "memory is allocated" in text or text.startswith("allocated memory"):
        return "allocation"
    if not is_terminal and (
        "memory is released" in text
        or "memory was released" in text
        or text.startswith("released memory")
    ):
        return "release"
    if text.startswith("assuming ") or text.startswith("assume "):
        return "branch"
    if "taking true branch" in text or "taking false branch" in text:
        return "branch"
    return None


def structural_event_token(event: dict[str, Any], files: Sequence[str]) -> str:
    loc = event_location(event, files)
    kind = str(event.get("kind", "unknown"))
    message = normalize_message(str(event.get("message", "")))
    if message.startswith("calling "):
        message_class = "call:" + message[8:]
    elif message.startswith("entered call"):
        message_class = "enter:" + message
    elif message.startswith("returning from "):
        message_class = "return:" + message[15:]
    elif kind == "control":
        message_class = "control"
    else:
        message_class = "event:" + message
    return f"{kind}|{loc.token()}|{message_class}"


def parse_plist_reports(
    plist_path: Path, dataset: str, side: str, action_id: str
) -> list[Report]:
    try:
        # CodeChecker emits one plist per completed action, including actions
        # with no diagnostics.  Avoid constructing an XML tree for the tens of
        # thousands of empty files while still counting them in action_pairs.
        size = plist_path.stat().st_size
        # An empty CodeChecker plist is only a few hundred bytes; even the
        # smallest target diagnostic is substantially larger than 1 KiB.
        if size < 1024:
            return []
        if size < STREAMING_PLIST_BYTES:
            payload = plist_path.read_bytes()
            if not any(prefix.encode("utf-8") in payload for prefix in TARGET_CHECKER_PREFIXES):
                return []
            document = plistlib.loads(payload)
            files = [normalize_path(str(path)) for path in document.get("files", [])]
            diagnostics = document.get("diagnostics", [])
        else:
            if not file_contains_target_checker(plist_path):
                return []
            return parse_large_plist_reports_streaming(
                plist_path, dataset, side, action_id
            )
    except Exception as exc:
        raise RuntimeError(f"cannot parse {plist_path}: {exc}") from exc

    reports: list[Report] = []
    for diagnostic_index, diagnostic in enumerate(diagnostics):
        checker = str(diagnostic.get("check_name", ""))
        if not checker_is_in_scope(checker):
            continue
        description = str(diagnostic.get("description", ""))
        kind = classify_kind(checker, description)
        terminal = location_from_dict(diagnostic.get("location"), files)
        evidence: dict[str, list[str]] = {name: [] for name in EVIDENCE_NAMES}
        skeleton: list[str] = []
        path = diagnostic.get("path", []) or []
        lifecycle_applicable = checker in LIFECYCLE_CHECKERS and kind in {
            "leak", "uaf", "double_free"
        }
        if lifecycle_applicable:
            for index, event in enumerate(path):
                message = str(event.get("message", ""))
                loc = event_location(event, files)
                is_terminal = (
                    index == len(path) - 1
                    or (
                        loc == terminal
                        and normalize_message(message) == normalize_message(description)
                    )
                )
                category = evidence_kind(message, is_terminal)
                if category:
                    evidence[category].append(loc.token())
                    continue
                if is_terminal:
                    continue
                skeleton.append(structural_event_token(event, files))

        reports.append(
            Report(
                dataset=dataset,
                side=side,
                action_id=action_id,
                plist_name=plist_path.name,
                diagnostic_index=diagnostic_index,
                checker=checker,
                kind=kind,
                description=description,
                normalized_description=normalize_message(description),
                issue_hash=str(
                    diagnostic.get("issue_hash_content_of_line_in_context", "")
                ),
                terminal=terminal,
                allocation_locations=tuple(sorted(set(evidence["allocation"]))),
                release_locations=tuple(sorted(set(evidence["release"]))),
                branch_locations=tuple(sorted(set(evidence["branch"]))),
                skeleton=tuple(skeleton),
                path_events=len(path),
            )
        )
    return reports


def file_contains_target_checker(path: Path) -> bool:
    needles = tuple(prefix.encode("utf-8") for prefix in TARGET_CHECKER_PREFIXES)
    overlap = max(map(len, needles)) - 1
    tail = b""
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                return False
            data = tail + chunk
            if any(needle in data for needle in needles):
                return True
            tail = data[-overlap:]


def extract_files_from_large_plist(path: Path) -> list[str]:
    """Extract the flat plist files array without loading a huge path tree."""
    with path.open("rb") as stream:
        with mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as mapped:
            key = mapped.find(b"<key>files</key>")
            if key < 0:
                return []
            start = mapped.find(b"<array>", key)
            end = mapped.find(b"</array>", start)
            if start < 0 or end < 0:
                return []
            fragment = bytes(mapped[start:end])
    values = re.findall(rb"<string>(.*?)</string>", fragment, flags=re.DOTALL)
    return [normalize_path(html.unescape(value.decode("utf-8"))) for value in values]


def plist_xml_value(element: ET.Element) -> Any:
    tag = element.tag.rsplit("}", 1)[-1]
    if tag == "dict":
        children = list(element)
        result: dict[str, Any] = {}
        index = 0
        while index + 1 < len(children):
            key = children[index].text or ""
            result[key] = plist_xml_value(children[index + 1])
            index += 2
        return result
    if tag == "array":
        return [plist_xml_value(child) for child in element]
    if tag in {"string", "key", "date"}:
        return element.text or ""
    if tag == "integer":
        return int(element.text or 0)
    if tag == "real":
        return float(element.text or 0)
    if tag == "true":
        return True
    if tag == "false":
        return False
    if tag == "data":
        return element.text or ""
    return element.text or ""


def iter_large_plist_diagnostics(path: Path) -> Iterator[dict[str, Any]]:
    """Stream one diagnostic dictionary at a time from a very large plist."""
    stack: list[ET.Element] = []
    last_key_at_depth: dict[int, str] = {}
    diagnostics_array_depth: int | None = None
    diagnostic_depth: int | None = None
    for event, element in ET.iterparse(path, events=("start", "end")):
        if event == "start":
            stack.append(element)
            if element.tag == "array" and last_key_at_depth.get(len(stack) - 1) == "diagnostics":
                diagnostics_array_depth = len(stack)
            elif (
                element.tag == "dict"
                and diagnostics_array_depth is not None
                and len(stack) == diagnostics_array_depth + 1
            ):
                diagnostic_depth = len(stack)
            continue

        if diagnostic_depth is not None and len(stack) == diagnostic_depth and element.tag == "dict":
            value = plist_xml_value(element)
            if isinstance(value, dict):
                yield value
            element.clear()
            diagnostic_depth = None
        elif element.tag == "key" and diagnostic_depth is None:
            last_key_at_depth[len(stack) - 1] = element.text or ""
            element.clear()
        elif (
            element.tag == "array"
            and diagnostics_array_depth is not None
            and len(stack) == diagnostics_array_depth
        ):
            element.clear()
            diagnostics_array_depth = None
        elif diagnostic_depth is None:
            element.clear()
        stack.pop()


def parse_large_plist_reports_streaming(
    plist_path: Path, dataset: str, side: str, action_id: str
) -> list[Report]:
    """Extract reports from a large plist without materializing whole paths.

    Each path event is converted and removed from its XML parent as soon as it
    ends.  The last event is delayed and then discarded as the terminal event,
    matching the ordinary parser's evidence-independent skeleton construction.
    """
    files = extract_files_from_large_plist(plist_path)
    reports: list[Report] = []
    stack: list[ET.Element] = []
    last_key_at_depth: dict[int, str] = {}
    diagnostics_array_depth: int | None = None
    diagnostic_depth: int | None = None
    path_array_depth: int | None = None
    event_depth: int | None = None
    direct_key: str | None = None
    pending_event: dict[str, Any] | None = None
    diagnostic_index = -1
    state: dict[str, Any] | None = None

    def process_nonterminal(raw_event: dict[str, Any]) -> None:
        assert state is not None
        message = str(raw_event.get("message", ""))
        loc = event_location(raw_event, files)
        is_terminal = (
            loc == state.get("terminal")
            and normalize_message(message) == state.get("normalized_description", "")
        )
        category = evidence_kind(message, is_terminal)
        if category:
            state["evidence"][category].append(loc.token())
        elif not is_terminal:
            state["skeleton"].append(structural_event_token(raw_event, files))

    for xml_event, element in ET.iterparse(plist_path, events=("start", "end")):
        tag = element.tag.rsplit("}", 1)[-1]
        if xml_event == "start":
            stack.append(element)
            depth = len(stack)
            if tag == "array" and last_key_at_depth.get(depth - 1) == "diagnostics":
                diagnostics_array_depth = depth
            elif (
                tag == "dict"
                and diagnostics_array_depth is not None
                and depth == diagnostics_array_depth + 1
            ):
                diagnostic_index += 1
                diagnostic_depth = depth
                direct_key = None
                pending_event = None
                state = {
                    "checker": "",
                    "description": "",
                    "normalized_description": "",
                    "kind": "other",
                    "issue_hash": "",
                    "terminal": Location("<unknown>", 0, 0),
                    "evidence": {name: [] for name in EVIDENCE_NAMES},
                    "skeleton": [],
                    "path_events": 0,
                }
            elif state is not None and diagnostic_depth is not None:
                if (
                    tag == "array"
                    and depth == diagnostic_depth + 1
                    and direct_key == "path"
                ):
                    path_array_depth = depth
                elif (
                    tag == "dict"
                    and path_array_depth is not None
                    and depth == path_array_depth + 1
                ):
                    event_depth = depth
            continue

        depth = len(stack)
        parent = stack[-2] if len(stack) >= 2 else None

        if (
            state is not None
            and event_depth is not None
            and depth == event_depth
            and tag == "dict"
        ):
            track_path = (
                state["checker"] in LIFECYCLE_CHECKERS
                and state["kind"] in {"leak", "uaf", "double_free"}
            )
            if track_path:
                raw_event = plist_xml_value(element)
                if isinstance(raw_event, dict):
                    if pending_event is not None:
                        process_nonterminal(pending_event)
                    pending_event = raw_event
            state["path_events"] += 1
            if parent is not None:
                # The path array contains only already-consumed event dicts at
                # this point. Clearing it avoids Element.remove's repeated
                # linear child search on very long analyzer paths.
                parent.clear()
            element.clear()
            event_depth = None
        elif (
            state is not None
            and path_array_depth is not None
            and depth == path_array_depth
            and tag == "array"
        ):
            # pending_event is the diagnostic terminal and is deliberately
            # omitted from both evidence and the matching skeleton.
            pending_event = None
            path_array_depth = None
            direct_key = None
            element.clear()
        elif (
            state is not None
            and diagnostic_depth is not None
            and path_array_depth is None
            and depth == diagnostic_depth + 1
        ):
            if tag == "key":
                direct_key = element.text or ""
            elif direct_key is not None:
                value = plist_xml_value(element)
                if direct_key == "check_name":
                    state["checker"] = str(value)
                    if state["description"]:
                        state["kind"] = classify_kind(
                            str(value), str(state["description"])
                        )
                elif direct_key == "description":
                    state["description"] = str(value)
                    state["normalized_description"] = normalize_message(str(value))
                    state["kind"] = classify_kind(str(state["checker"]), str(value))
                elif direct_key == "issue_hash_content_of_line_in_context":
                    state["issue_hash"] = str(value)
                elif direct_key == "location" and isinstance(value, dict):
                    state["terminal"] = location_from_dict(value, files)
                direct_key = None
            element.clear()
        elif (
            state is not None
            and diagnostic_depth is not None
            and depth == diagnostic_depth
            and tag == "dict"
        ):
            checker = str(state["checker"])
            if checker_is_in_scope(checker):
                description = str(state["description"])
                evidence = state["evidence"]
                reports.append(
                    Report(
                        dataset=dataset,
                        side=side,
                        action_id=action_id,
                        plist_name=plist_path.name,
                        diagnostic_index=diagnostic_index,
                        checker=checker,
                        kind=str(state["kind"]),
                        description=description,
                        normalized_description=str(state["normalized_description"]),
                        issue_hash=str(state["issue_hash"]),
                        terminal=state["terminal"],
                        allocation_locations=tuple(
                            sorted(set(evidence["allocation"]))
                        ),
                        release_locations=tuple(sorted(set(evidence["release"]))),
                        branch_locations=tuple(sorted(set(evidence["branch"]))),
                        skeleton=tuple(state["skeleton"]),
                        path_events=int(state["path_events"]),
                    )
                )
            if parent is not None:
                parent.remove(element)
            element.clear()
            diagnostic_depth = None
            state = None
            direct_key = None
        elif tag == "key" and diagnostic_depth is None:
            last_key_at_depth[depth - 1] = element.text or ""
            element.clear()
        elif (
            tag == "array"
            and diagnostics_array_depth is not None
            and depth == diagnostics_array_depth
        ):
            element.clear()
            diagnostics_array_depth = None
        elif state is None:
            element.clear()

        stack.pop()

    return reports


def command_tokens(entry: dict[str, Any]) -> list[str]:
    if isinstance(entry.get("arguments"), list):
        return [str(item) for item in entry["arguments"]]
    command = str(entry.get("command", ""))
    return shlex.split(command, posix=True)


def canonicalize_compile_command(entry: dict[str, Any]) -> tuple[str, str]:
    """Return source path and a semantic command digest.

    The executable, generated outputs, dependency files, diagnostics output,
    and analyzer-only controls do not define the compiled program.  All other
    arguments are retained in order, including target, macros, includes,
    language standard, and optimization options.
    """
    directory = str(entry.get("directory", ""))
    source = normalize_path(str(entry.get("file", "")), directory)
    tokens = command_tokens(entry)
    if tokens:
        tokens = tokens[1:]
    stripped: list[str] = []
    skip_next = False
    pair_options = {
        "-o",
        "-MF",
        "-MT",
        "-MQ",
        "-MJ",
        "--serialize-diagnostics",
        "-dependency-file",
    }
    analyzer_pair_options = {"-Xanalyzer", "-analyzer-config"}
    for token in tokens:
        if skip_next:
            skip_next = False
            continue
        if token in pair_options or token in analyzer_pair_options:
            skip_next = True
            continue
        if token in {"--analyze", "-analyze"}:
            continue
        if token.startswith("-o") and len(token) > 2:
            continue
        if token.startswith("-fdiagnostics-file="):
            continue
        if token.startswith("-analyzer-"):
            continue
        normalized = token.replace("\\", "/")
        for marker in PROJECT_MARKERS:
            pos = normalized.lower().rfind(marker)
            if pos >= 0:
                normalized = normalized[pos + len(marker) :]
                break
        stripped.append(normalized)
    payload = "\0".join(stripped).encode("utf-8", errors="replace")
    return source, hashlib.sha256(payload).hexdigest()[:20]


def load_compile_configs(path: Path) -> tuple[dict[str, Counter[str]], int]:
    with path.open("r", encoding="utf-8") as stream:
        entries = json.load(stream)
    configs: dict[str, Counter[str]] = defaultdict(Counter)
    for entry in entries:
        source, digest = canonicalize_compile_command(entry)
        configs[source][digest] += 1
    return dict(configs), len(entries)


def load_android_metadata(report_dir: Path) -> tuple[dict[str, str], dict[str, Any]]:
    metadata_path = report_dir / "metadata.json"
    with metadata_path.open("r", encoding="utf-8") as stream:
        metadata = json.load(stream)
    tool = metadata["tools"][0]
    working_directory = str(tool.get("working_directory", ""))
    mapping: dict[str, str] = {}
    for result, source in tool.get("result_source_files", {}).items():
        mapping[Path(result).name] = normalize_path(str(source), working_directory)
    return mapping, tool


def openharmony_action_groups(baseline_dir: Path, ours_dir: Path) -> list[ActionGroup]:
    baseline = {path.name: path for path in baseline_dir.glob("*.plist")}
    ours = {path.name: path for path in ours_dir.glob("*.plist")}
    rows: list[ActionGroup] = []
    for name in sorted(set(baseline) | set(ours)):
        if name in baseline and name in ours:
            status, reason = "paired", "exact_source_and_command_hash"
        elif name in baseline:
            status, reason = "baseline_only", "missing_ours_action"
        else:
            status, reason = "ours_only", "missing_baseline_action"
        source = name.rsplit("_clangsa_", 1)[0]
        rows.append(
            ActionGroup(
                dataset="OpenHarmony",
                action_id=f"oh:{name}",
                source_path=source,
                status=status,
                reason=reason,
                action_count=1,
                baseline_plists=(baseline[name],) if name in baseline else (),
                ours_plists=(ours[name],) if name in ours else (),
            )
        )
    return rows


def openharmony_ours_action_groups(ours_dir: Path) -> list[ActionGroup]:
    """Create independently analyzable action groups for a released ours run."""
    rows: list[ActionGroup] = []
    for path in sorted(ours_dir.glob("*.plist"), key=lambda item: item.name):
        source = path.name.rsplit("_clangsa_", 1)[0]
        rows.append(
            ActionGroup(
                dataset="OpenHarmony",
                action_id=f"oh:{path.name}",
                source_path=source,
                status="ours_only_input",
                reason="released_ours_result",
                action_count=1,
                ours_plists=(path,),
            )
        )
    return rows


def enabled_checker_groups(tool: dict[str, Any]) -> tuple[str, ...]:
    command = [str(value) for value in tool.get("command", [])]
    enabled: list[str] = []
    for index, token in enumerate(command[:-1]):
        if token == "--enable":
            enabled.append(command[index + 1])
    return tuple(enabled)


def android_action_groups(
    baseline_dir: Path, ours_dir: Path
) -> tuple[list[ActionGroup], dict[str, Any]]:
    baseline_meta, baseline_tool = load_android_metadata(baseline_dir)
    ours_meta, ours_tool = load_android_metadata(ours_dir)

    baseline_plists: dict[str, list[Path]] = defaultdict(list)
    ours_plists: dict[str, list[Path]] = defaultdict(list)
    baseline_missing: list[tuple[str, str]] = []
    ours_missing: list[tuple[str, str]] = []
    baseline_existing = {
        entry.name: Path(entry.path)
        for entry in os.scandir(baseline_dir)
        if entry.name.endswith(".plist")
    }
    ours_existing = {
        entry.name: Path(entry.path)
        for entry in os.scandir(ours_dir)
        if entry.name.endswith(".plist")
    }
    for name, source in baseline_meta.items():
        if name in baseline_existing:
            baseline_plists[source].append(baseline_existing[name])
        else:
            baseline_missing.append((name, source))
    for name, source in ours_meta.items():
        if name in ours_existing:
            ours_plists[source].append(ours_existing[name])
        else:
            ours_missing.append((name, source))

    sources = sorted(set(baseline_plists) | set(ours_plists))
    rows: list[ActionGroup] = []
    for source in sources:
        b_plists = tuple(sorted(baseline_plists.get(source, [])))
        o_plists = tuple(sorted(ours_plists.get(source, [])))
        if not b_plists:
            status, reason = "ours_only", "missing_baseline_result"
        elif not o_plists:
            status, reason = "baseline_only", "missing_ours_result"
        elif len(b_plists) != 1 or len(o_plists) != 1:
            status, reason = "ambiguous", "multiple_result_plists_for_source"
        else:
            status, reason = "paired", "unique_normalized_source_in_metadata"
        digest = hashlib.sha256(source.encode("utf-8")).hexdigest()[:20]
        rows.append(
            ActionGroup(
                dataset="Android",
                action_id=f"android:{source}:{digest}",
                source_path=source,
                status=status,
                reason=reason,
                action_count=1,
                baseline_plists=b_plists,
                ours_plists=o_plists,
            )
        )
    metadata = {
        "baseline_planned_actions": int(baseline_tool.get("action_num", 0)),
        "ours_planned_actions": int(ours_tool.get("action_num", 0)),
        "baseline_result_plists": len(baseline_meta),
        "ours_result_plists": len(ours_meta),
        "baseline_existing_result_plists": sum(map(len, baseline_plists.values())),
        "ours_existing_result_plists": sum(map(len, ours_plists.values())),
        "baseline_missing_result_plists": len(baseline_missing),
        "ours_missing_result_plists": len(ours_missing),
        "baseline_missing_results": [
            {"plist": name, "source": source} for name, source in baseline_missing
        ],
        "ours_missing_results": [
            {"plist": name, "source": source} for name, source in ours_missing
        ],
        "baseline_checker_groups": list(enabled_checker_groups(baseline_tool)),
        "ours_checker_groups": list(enabled_checker_groups(ours_tool)),
        "baseline_working_directory": str(baseline_tool.get("working_directory", "")),
        "ours_working_directory": str(ours_tool.get("working_directory", "")),
        "baseline_analyzer_version": str(baseline_tool.get("analyzer_version", "")),
        "ours_analyzer_version": str(ours_tool.get("analyzer_version", "")),
    }
    return rows, metadata


def android_ours_action_groups(
    ours_dir: Path,
) -> tuple[list[ActionGroup], dict[str, Any]]:
    """Group a released Android ours run using its CodeChecker metadata."""
    ours_meta, ours_tool = load_android_metadata(ours_dir)
    existing = {
        entry.name: Path(entry.path)
        for entry in os.scandir(ours_dir)
        if entry.name.endswith(".plist")
    }
    by_source: dict[str, list[Path]] = defaultdict(list)
    missing: list[tuple[str, str]] = []
    for name, source in ours_meta.items():
        if name in existing:
            by_source[source].append(existing[name])
        else:
            missing.append((name, source))

    rows: list[ActionGroup] = []
    for source in sorted(by_source):
        digest = hashlib.sha256(source.encode("utf-8")).hexdigest()[:20]
        rows.append(
            ActionGroup(
                dataset="Android",
                action_id=f"android:{source}:{digest}",
                source_path=source,
                status="ours_only_input",
                reason="released_ours_result",
                action_count=1,
                ours_plists=tuple(sorted(by_source[source])),
            )
        )
    metadata = {
        "ours_planned_actions": int(ours_tool.get("action_num", 0)),
        "ours_result_plists": len(ours_meta),
        "ours_existing_result_plists": sum(map(len, by_source.values())),
        "ours_missing_result_plists": len(missing),
        "ours_missing_results": [
            {"plist": name, "source": source} for name, source in missing
        ],
        "ours_checker_groups": list(enabled_checker_groups(ours_tool)),
        "ours_analyzer_version": str(ours_tool.get("analyzer_version", "")),
    }
    return rows, metadata


def split_issue_groups(
    dataset: str, action_id: str, baseline: list[Report], ours: list[Report]
) -> list[IssueGroup]:
    by_terminal_b: dict[tuple[str, str, str, int, int], list[Report]] = defaultdict(list)
    by_terminal_o: dict[tuple[str, str, str, int, int], list[Report]] = defaultdict(list)
    for report in baseline:
        by_terminal_b[report.terminal_key].append(report)
    for report in ours:
        by_terminal_o[report.terminal_key].append(report)

    result: list[IssueGroup] = []
    for key in sorted(set(by_terminal_b) | set(by_terminal_o)):
        b_reports = by_terminal_b.get(key, [])
        o_reports = by_terminal_o.get(key, [])
        if not b_reports:
            result.append(IssueGroup(dataset, action_id, key, [], o_reports, "ours_only", "terminal"))
            continue
        if not o_reports:
            result.append(IssueGroup(dataset, action_id, key, b_reports, [], "baseline_only", "terminal"))
            continue

        b_hashes = {item.issue_hash for item in b_reports if item.issue_hash}
        o_hashes = {item.issue_hash for item in o_reports if item.issue_hash}
        if len(b_hashes) <= 1 and len(o_hashes) <= 1:
            tier = "terminal+hash" if b_hashes == o_hashes else "terminal+unique-relaxed"
            result.append(IssueGroup(dataset, action_id, key, b_reports, o_reports, "common", tier))
            continue

        consumed_b: set[str] = set()
        consumed_o: set[str] = set()
        for issue_hash in sorted(b_hashes & o_hashes):
            bg = [item for item in b_reports if item.issue_hash == issue_hash]
            og = [item for item in o_reports if item.issue_hash == issue_hash]
            result.append(IssueGroup(dataset, action_id, key, bg, og, "common", "terminal+hash"))
            consumed_b.update(item.report_id for item in bg)
            consumed_o.update(item.report_id for item in og)
        remaining_b = [item for item in b_reports if item.report_id not in consumed_b]
        remaining_o = [item for item in o_reports if item.report_id not in consumed_o]
        if remaining_b or remaining_o:
            if remaining_b and not remaining_o:
                result.append(
                    IssueGroup(dataset, action_id, key, remaining_b, [], "baseline_only", "terminal+hash")
                )
                continue
            if remaining_o and not remaining_b:
                result.append(
                    IssueGroup(dataset, action_id, key, [], remaining_o, "ours_only", "terminal+hash")
                )
                continue
            b_desc = {item.normalized_description for item in remaining_b}
            o_desc = {item.normalized_description for item in remaining_o}
            if remaining_b and remaining_o and len(b_desc) == len(o_desc) == 1 and b_desc == o_desc:
                result.append(
                    IssueGroup(dataset, action_id, key, remaining_b, remaining_o, "common", "terminal+description")
                )
            else:
                result.append(
                    IssueGroup(
                        dataset,
                        action_id,
                        key,
                        remaining_b,
                        remaining_o,
                        "ambiguous",
                        "terminal",
                        "multiple_issue_variants_at_terminal",
                    )
                )
    return result


@lru_cache(maxsize=None)
def skeleton_features(skeleton: tuple[str, ...]) -> frozenset[tuple[str, str]]:
    """Ordered, evidence-free bigrams used for linear-time path comparison."""
    if not skeleton:
        return frozenset()
    if len(skeleton) == 1:
        return frozenset({("<start>", skeleton[0])})
    return frozenset(zip(skeleton, skeleton[1:]))


def path_cost(baseline: Report, ours: Report) -> int:
    before = skeleton_features(baseline.skeleton)
    after = skeleton_features(ours.skeleton)
    if not before and not after:
        ratio = 1.0
    else:
        ratio = (2.0 * len(before & after)) / (len(before) + len(after))
    description_penalty = 0 if baseline.normalized_description == ours.normalized_description else 25
    length_penalty = min(50, abs(len(baseline.skeleton) - len(ours.skeleton)))
    return int(round((1.0 - ratio) * 1000)) + description_penalty + length_penalty


def hungarian(cost: Sequence[Sequence[int]]) -> tuple[int, list[tuple[int, int]]]:
    """Minimum-cost assignment for a rectangular integer matrix."""
    if not cost or not cost[0]:
        return 0, []
    rows, cols = len(cost), len(cost[0])
    transposed = rows > cols
    matrix = [list(row) for row in cost]
    if transposed:
        matrix = [list(row) for row in zip(*matrix)]
        rows, cols = cols, rows
    u = [0] * (rows + 1)
    v = [0] * (cols + 1)
    p = [0] * (cols + 1)
    way = [0] * (cols + 1)
    for i in range(1, rows + 1):
        p[0] = i
        minv = [math.inf] * (cols + 1)
        used = [False] * (cols + 1)
        j0 = 0
        while True:
            used[j0] = True
            i0 = p[j0]
            delta = math.inf
            j1 = 0
            for j in range(1, cols + 1):
                if used[j]:
                    continue
                current = matrix[i0 - 1][j - 1] - u[i0] - v[j]
                if current < minv[j]:
                    minv[j] = current
                    way[j] = j0
                if minv[j] < delta:
                    delta, j1 = minv[j], j
            for j in range(cols + 1):
                if used[j]:
                    u[p[j]] += int(delta)
                    v[j] -= int(delta)
                else:
                    minv[j] -= delta
            j0 = j1
            if p[j0] == 0:
                break
        while True:
            j1 = way[j0]
            p[j0] = p[j1]
            j0 = j1
            if j0 == 0:
                break
    assignment: list[tuple[int, int]] = []
    for j in range(1, cols + 1):
        if p[j]:
            i = p[j] - 1
            jj = j - 1
            assignment.append((jj, i) if transposed else (i, jj))
    assignment.sort()
    return sum(cost[i][j] for i, j in assignment), assignment


def assignment_is_ambiguous(cost: list[list[int]], optimum: int, pairs: list[tuple[int, int]]) -> bool:
    if len(pairs) <= 1:
        return False
    # Re-solving once per matched edge is exact and inexpensive for ordinary
    # duplicate groups.  For large groups, use a conservative tie test: any
    # equal-cost alternative in an assigned row or column makes the group
    # ambiguous.  This may exclude a uniquely solvable large group, but cannot
    # manufacture an evidence gain by forcing a questionable pairing.
    if len(pairs) > 24:
        for row, col in pairs:
            assigned = cost[row][col]
            if sum(value == assigned for value in cost[row]) > 1:
                return True
            if sum(cost[r][col] == assigned for r in range(len(cost))) > 1:
                return True
        return False
    forbidden = 10**9
    for row, col in pairs:
        changed = [values[:] for values in cost]
        changed[row][col] = forbidden
        alternative, alternative_pairs = hungarian(changed)
        if alternative_pairs and alternative == optimum:
            return True
    return False


def transition(before: bool, after: bool) -> str:
    if before and after:
        return "retained"
    if not before and after:
        return "gained"
    if before and not after:
        return "lost"
    return "absent_in_both"


def overall_class(baseline: Report, ours: Report) -> str:
    states = [transition(baseline.has(name), ours.has(name)) for name in EVIDENCE_NAMES]
    gained = "gained" in states
    lost = "lost" in states
    if gained and lost:
        return "mixed"
    if gained:
        return "improved"
    if lost:
        return "degraded"
    return "unchanged"


def issue_identifier(group: IssueGroup, ordinal: int) -> str:
    checker, kind, path, line, col = group.key
    payload = f"{group.dataset}|{group.action_id}|{checker}|{kind}|{path}|{line}|{col}|{ordinal}"
    return hashlib.sha256(payload.encode()).hexdigest()[:20]


def write_csv(path: Path, fieldnames: Sequence[str], rows: Iterable[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        for row in rows:
            writer.writerow(row)


def report_row(report: Report) -> dict[str, Any]:
    return {
        "dataset": report.dataset,
        "side": report.side,
        "action_id": report.action_id,
        "report_id": report.report_id,
        "plist_name": report.plist_name,
        "diagnostic_index": report.diagnostic_index,
        "checker_family": report.checker_family,
        "checker": report.checker,
        "kind": report.kind,
        "description": report.description,
        "issue_hash": report.issue_hash,
        "terminal_path": report.terminal.path,
        "terminal_line": report.terminal.line,
        "terminal_col": report.terminal.col,
        "has_allocation": report.has("allocation"),
        "has_release": report.has("release"),
        "has_branch": report.has("branch"),
        "lifecycle_applicable": report.lifecycle_applicable,
        "lifecycle_complete": report.lifecycle_complete(),
        "path_events": report.path_events,
    }


def duplicate_group_requires_audit(
    baseline: Sequence[Report], ours: Sequence[Report]
) -> bool:
    assignment_cells = len(baseline) * len(ours)
    similarity_tokens = (
        len(ours) * sum(len(report.skeleton) for report in baseline)
        + len(baseline) * sum(len(report.skeleton) for report in ours)
    )
    return (
        assignment_cells > MAX_EXACT_ASSIGNMENT_CELLS
        or similarity_tokens > MAX_EXACT_SIMILARITY_TOKENS
    )


def parse_action_reports(
    action: ActionGroup,
) -> tuple[dict[str, list[Report]], list[dict[str, str]]]:
    """Parse one selected action; safe to execute in a worker process."""
    reports: dict[str, list[Report]] = {"baseline": [], "ours": []}
    parse_errors: list[dict[str, str]] = []
    if action.status not in {"paired", "ours_only_input"}:
        return reports, parse_errors
    for side, plist_paths in (
        ("baseline", action.baseline_plists),
        ("ours", action.ours_plists),
    ):
        for plist_path in plist_paths:
            try:
                reports[side].extend(
                    parse_plist_reports(
                        plist_path, action.dataset, side, action.action_id
                    )
                )
            except RuntimeError as exc:
                parse_errors.append(
                    {
                        "dataset": action.dataset,
                        "side": side,
                        "action_id": action.action_id,
                        "plist": str(plist_path),
                        "error": str(exc),
                    }
                )
    return reports, parse_errors


def analyze_dataset(actions: list[ActionGroup], jobs: int = 1) -> dict[str, Any]:
    action_rows: list[dict[str, Any]] = []
    issue_rows: list[dict[str, Any]] = []
    instance_rows: list[dict[str, Any]] = []
    evidence_rows: list[dict[str, Any]] = []
    all_reports: dict[str, list[Report]] = {"baseline": [], "ours": []}
    parse_errors: list[dict[str, str]] = []

    executor: concurrent.futures.ProcessPoolExecutor | None = None
    if jobs > 1:
        executor = concurrent.futures.ProcessPoolExecutor(max_workers=jobs)
        parsed_actions = executor.map(parse_action_reports, actions, chunksize=4)
    else:
        parsed_actions = map(parse_action_reports, actions)

    try:
      for action_index, (action, parsed) in enumerate(
          zip(actions, parsed_actions), start=1
      ):
        if action_index % 500 == 0:
            print(
                f"[{action.dataset}] processed {action_index:,}/{len(actions):,} action groups",
                file=sys.stderr,
                flush=True,
            )
        action_rows.append(
            {
                "dataset": action.dataset,
                "action_id": action.action_id,
                "source_path": action.source_path,
                "status": action.status,
                "reason": action.reason,
                "action_count": action.action_count,
                "baseline_plist_count": len(action.baseline_plists),
                "ours_plist_count": len(action.ours_plists),
                "baseline_configs": ";".join(action.baseline_configs),
                "ours_configs": ";".join(action.ours_configs),
            }
        )
        if action.status not in {"paired", "ours_only_input"}:
            continue
        reports, action_parse_errors = parsed
        parse_errors.extend(action_parse_errors)
        for side in ("baseline", "ours"):
            all_reports[side].extend(reports[side])

        groups = split_issue_groups(
            action.dataset, action.action_id, reports["baseline"], reports["ours"]
        )
        for ordinal, group in enumerate(groups):
            issue_id = issue_identifier(group, ordinal)
            checker, kind, path, line, col = group.key
            issue_row = {
                "dataset": group.dataset,
                "action_id": group.action_id,
                "issue_pair_id": issue_id,
                "status": group.status,
                "match_tier": group.match_tier,
                "reason": group.reason,
                "checker": checker,
                "kind": kind,
                "terminal_path": path,
                "terminal_line": line,
                "terminal_col": col,
                "baseline_instances": len(group.baseline),
                "ours_instances": len(group.ours),
                "baseline_hashes": ";".join(sorted({r.issue_hash for r in group.baseline})),
                "ours_hashes": ";".join(sorted({r.issue_hash for r in group.ours})),
            }
            issue_rows.append(issue_row)
            if group.status != "common":
                for side, side_reports in (("baseline", group.baseline), ("ours", group.ours)):
                    for report in side_reports:
                        instance_rows.append(
                            {
                                "dataset": group.dataset,
                                "action_id": group.action_id,
                                "issue_pair_id": issue_id,
                                "status": group.status,
                                "baseline_report_id": report.report_id if side == "baseline" else "",
                                "ours_report_id": report.report_id if side == "ours" else "",
                                "match_cost": "",
                                "baseline_complete": report.lifecycle_complete() if side == "baseline" else "",
                                "ours_complete": report.lifecycle_complete() if side == "ours" else "",
                            }
                        )
                continue

            # Exact Hungarian assignment is cubic in the duplicate count.  A
            # very large same-terminal group is also intrinsically difficult
            # to identify one-to-one without using the evidence under test.
            # Exclude it conservatively instead of forcing a costly or
            # evidence-biased pairing.
            if duplicate_group_requires_audit(group.baseline, group.ours):
                issue_rows[-1]["status"] = "ambiguous"
                issue_rows[-1]["reason"] = "large_or_long_duplicate_group_requires_audit"
                for report in group.baseline:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "ambiguous",
                            "baseline_report_id": report.report_id,
                            "ours_report_id": "",
                            "match_cost": "",
                            "baseline_complete": report.lifecycle_complete(),
                            "ours_complete": "",
                        }
                    )
                for report in group.ours:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "ambiguous",
                            "baseline_report_id": "",
                            "ours_report_id": report.report_id,
                            "match_cost": "",
                            "baseline_complete": "",
                            "ours_complete": report.lifecycle_complete(),
                        }
                    )
                continue

            cost = [[path_cost(b, o) for o in group.ours] for b in group.baseline]
            optimum, pairs = hungarian(cost)
            ambiguous = assignment_is_ambiguous(cost, optimum, pairs)
            if ambiguous:
                issue_rows[-1]["status"] = "ambiguous"
                issue_rows[-1]["reason"] = "multiple_equal_cost_path_assignments"
                for report in group.baseline:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "ambiguous",
                            "baseline_report_id": report.report_id,
                            "ours_report_id": "",
                            "match_cost": "",
                            "baseline_complete": report.lifecycle_complete(),
                            "ours_complete": "",
                        }
                    )
                for report in group.ours:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "ambiguous",
                            "baseline_report_id": "",
                            "ours_report_id": report.report_id,
                            "match_cost": "",
                            "baseline_complete": "",
                            "ours_complete": report.lifecycle_complete(),
                        }
                    )
                continue

            paired_b: set[int] = set()
            paired_o: set[int] = set()
            for b_index, o_index in pairs:
                paired_b.add(b_index)
                paired_o.add(o_index)
                b_report = group.baseline[b_index]
                o_report = group.ours[o_index]
                lifecycle_pair = (
                    b_report.lifecycle_applicable and o_report.lifecycle_applicable
                )
                classification = (
                    overall_class(b_report, o_report)
                    if lifecycle_pair
                    else "not_applicable"
                )
                instance_rows.append(
                    {
                        "dataset": group.dataset,
                        "action_id": group.action_id,
                        "issue_pair_id": issue_id,
                        "status": "matched",
                        "baseline_report_id": b_report.report_id,
                        "ours_report_id": o_report.report_id,
                        "match_cost": cost[b_index][o_index],
                        "baseline_complete": b_report.lifecycle_complete(),
                        "ours_complete": o_report.lifecycle_complete(),
                        "overall_class": classification,
                    }
                )
                if lifecycle_pair:
                    for name in EVIDENCE_NAMES:
                        evidence_rows.append(
                            {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "baseline_report_id": b_report.report_id,
                            "ours_report_id": o_report.report_id,
                            "kind": b_report.kind,
                            "checker": b_report.checker,
                            "checker_family": b_report.checker_family,
                            "lifecycle_applicable": b_report.lifecycle_applicable,
                            "evidence": name,
                            "transition": transition(b_report.has(name), o_report.has(name)),
                            "baseline_locations": ";".join(getattr(b_report, f"{name}_locations")),
                            "ours_locations": ";".join(getattr(o_report, f"{name}_locations")),
                            }
                        )
            for index, report in enumerate(group.baseline):
                if index not in paired_b:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "baseline_surplus",
                            "baseline_report_id": report.report_id,
                            "ours_report_id": "",
                            "match_cost": "",
                            "baseline_complete": report.lifecycle_complete(),
                            "ours_complete": "",
                        }
                    )
            for index, report in enumerate(group.ours):
                if index not in paired_o:
                    instance_rows.append(
                        {
                            "dataset": group.dataset,
                            "action_id": group.action_id,
                            "issue_pair_id": issue_id,
                            "status": "ours_surplus",
                            "baseline_report_id": "",
                            "ours_report_id": report.report_id,
                            "match_cost": "",
                            "baseline_complete": "",
                            "ours_complete": report.lifecycle_complete(),
                        }
                    )
    finally:
        if executor is not None:
            executor.shutdown(wait=True, cancel_futures=False)

    return {
        "action_rows": action_rows,
        "issue_rows": issue_rows,
        "instance_rows": instance_rows,
        "evidence_rows": evidence_rows,
        "reports": all_reports,
        "report_rows": [
            report_row(report)
            for side in ("baseline", "ours")
            for report in all_reports[side]
        ],
        "parse_errors": parse_errors,
    }


def summarize(dataset_result: dict[str, Any], action_metadata: dict[str, Any] | None = None) -> dict[str, Any]:
    action_rows = dataset_result["action_rows"]
    issue_rows = dataset_result["issue_rows"]
    instance_rows = dataset_result["instance_rows"]
    evidence_rows = dataset_result["evidence_rows"]
    reports = dataset_result["reports"]

    action_status = Counter(row["status"] for row in action_rows)
    paired_action_count = sum(
        int(row["action_count"]) for row in action_rows if row["status"] == "paired"
    )
    issue_status = Counter(row["status"] for row in issue_rows)
    instance_status = Counter(row["status"] for row in instance_rows)
    matched_instances = [row for row in instance_rows if row["status"] == "matched"]
    overall = Counter(row.get("overall_class", "") for row in matched_instances)
    overall.pop("", None)

    transitions: dict[str, Counter[str]] = {
        name: Counter() for name in EVIDENCE_NAMES
    }
    transitions_by_kind: dict[str, dict[str, Counter[str]]] = defaultdict(
        lambda: {name: Counter() for name in EVIDENCE_NAMES}
    )
    lifecycle_transitions: dict[str, Counter[str]] = {
        name: Counter() for name in EVIDENCE_NAMES
    }
    for row in evidence_rows:
        transitions[row["evidence"]][row["transition"]] += 1
        transitions_by_kind[row["kind"]][row["evidence"]][row["transition"]] += 1
        if row.get("lifecycle_applicable"):
            lifecycle_transitions[row["evidence"]][row["transition"]] += 1

    unique_sets: dict[str, set[tuple[str, str, str, int, int, str]]] = {}
    reports_by_unique: dict[str, dict[tuple[str, str, str, int, int, str], list[Report]]] = {}
    for side in ("baseline", "ours"):
        grouped: dict[tuple[str, str, str, int, int, str], list[Report]] = defaultdict(list)
        for report in reports[side]:
            grouped[report.global_issue_key].append(report)
        reports_by_unique[side] = dict(grouped)
        unique_sets[side] = set(grouped)

    # Build canonical unique issues from raw fingerprints plus verified
    # common-issue links.  This prevents a hash-only change at the exact same
    # terminal bug from being counted as one removed and one new issue.
    all_nodes = unique_sets["baseline"] | unique_sets["ours"]
    parent = {node: node for node in all_nodes}

    def find(node: tuple[str, str, str, int, int, str]):
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    def union(left, right):
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            if repr(left_root) <= repr(right_root):
                parent[right_root] = left_root
            else:
                parent[left_root] = right_root

    issue_pair_nodes: dict[str, list[tuple[str, str, str, int, int, str]]] = defaultdict(list)
    for row in issue_rows:
        base = (
            row["checker"], row["kind"], row["terminal_path"],
            int(row["terminal_line"]), int(row["terminal_col"]),
        )
        b_nodes = [(*base, value) for value in str(row["baseline_hashes"]).split(";") if value]
        o_nodes = [(*base, value) for value in str(row["ours_hashes"]).split(";") if value]
        nodes = [node for node in b_nodes + o_nodes if node in parent]
        issue_pair_nodes[row["issue_pair_id"]].extend(nodes)
        if row["status"] == "common" and b_nodes and o_nodes:
            anchor = b_nodes[0]
            for node in b_nodes[1:] + o_nodes:
                if anchor in parent and node in parent:
                    union(anchor, node)

    components: dict[Any, set[Any]] = defaultdict(set)
    for node in all_nodes:
        components[find(node)].add(node)
    baseline_components = {
        root for root, nodes in components.items() if nodes & unique_sets["baseline"]
    }
    ours_components = {
        root for root, nodes in components.items() if nodes & unique_sets["ours"]
    }
    common_components = baseline_components & ours_components
    ours_only_components = ours_components - baseline_components
    baseline_only_components = baseline_components - ours_components

    side_report_summary: dict[str, Any] = {}
    for side in ("baseline", "ours"):
        side_reports = reports[side]
        applicable_reports = [report for report in side_reports if report.lifecycle_applicable]
        side_report_summary[side] = {
            "report_instances": len(side_reports),
            "unique_terminal_issues": len(unique_sets[side]),
            "by_kind": dict(Counter(report.kind for report in side_reports)),
            "by_checker_family": dict(Counter(report.checker_family for report in side_reports)),
            "by_checker": dict(Counter(report.checker for report in side_reports)),
            "lifecycle_applicable_instances": len(applicable_reports),
            "lifecycle_complete_instances": sum(
                report.lifecycle_complete() is True for report in applicable_reports
            ),
        }

    ours_only_ids = {
        row["issue_pair_id"] for row in issue_rows if row["status"] == "ours_only"
    }
    baseline_only_ids = {
        row["issue_pair_id"] for row in issue_rows if row["status"] == "baseline_only"
    }
    ours_only_rows = [
        row for row in instance_rows if row["issue_pair_id"] in ours_only_ids and row["ours_report_id"]
    ]
    baseline_only_rows = [
        row
        for row in instance_rows
        if row["issue_pair_id"] in baseline_only_ids and row["baseline_report_id"]
    ]
    def unique_completeness(side: str, roots: set[Any]) -> dict[str, int]:
        groups = reports_by_unique[side]
        reports_per_component: list[list[Report]] = []
        for root in roots:
            component_reports: list[Report] = []
            for key in components[root] & unique_sets[side]:
                component_reports.extend(groups[key])
            reports_per_component.append(component_reports)
        applicable_groups = [
            group for group in reports_per_component if group and group[0].lifecycle_applicable
        ]
        return {
            "count": len(roots),
            "applicable": len(applicable_groups),
            "all_instances_complete": sum(
                all(report.lifecycle_complete() is True for report in group)
                for group in applicable_groups
            ),
            "at_least_one_instance_complete": sum(
                any(report.lifecycle_complete() is True for report in group)
                for group in applicable_groups
            ),
        }

    def unique_profiles(side: str, roots: set[Any]) -> dict[str, dict[str, int]]:
        groups = reports_by_unique[side]
        profiles: dict[str, Counter[str]] = defaultdict(Counter)
        for root in roots:
            component_reports: list[Report] = []
            for key in components[root] & unique_sets[side]:
                component_reports.extend(groups[key])
            if not component_reports:
                continue
            kind = component_reports[0].kind
            profiles[kind]["issues"] += 1
            if component_reports[0].lifecycle_applicable:
                profiles[kind]["lifecycle_applicable"] += 1
                profiles[kind]["all_instances_complete"] += int(
                    all(report.lifecycle_complete() is True for report in component_reports)
                )
            for evidence in EVIDENCE_NAMES:
                profiles[kind][f"all_with_{evidence}"] += int(
                    all(report.has(evidence) for report in component_reports)
                )
                profiles[kind][f"any_with_{evidence}"] += int(
                    any(report.has(evidence) for report in component_reports)
                )
        return {kind: dict(values) for kind, values in profiles.items()}

    pair_to_component: dict[str, Any] = {}
    for pair_id, nodes in issue_pair_nodes.items():
        if nodes:
            pair_to_component[pair_id] = find(nodes[0])
    matched_by_component: dict[Any, list[str]] = defaultdict(list)
    for row in matched_instances:
        component = pair_to_component.get(row["issue_pair_id"])
        if component is not None:
            matched_by_component[component].append(row.get("overall_class", "unchanged"))

    def aggregate_classes(values: list[str]) -> str:
        applicable = [value for value in values if value != "not_applicable"]
        if not applicable:
            return "not_applicable"
        gained = any(value in {"improved", "mixed"} for value in applicable)
        lost = any(value in {"degraded", "mixed"} for value in applicable)
        if gained and lost:
            return "mixed"
        if gained:
            return "improved"
        if lost:
            return "degraded"
        return "unchanged"

    matched_unique_classes = Counter(
        aggregate_classes(values) for values in matched_by_component.values()
    )

    summary: dict[str, Any] = {
        "action_pairing": {
            "status_groups": dict(action_status),
            "paired_analysis_actions": paired_action_count,
        },
        "reports_in_paired_actions": side_report_summary,
        "global_unique_issues": {
            "baseline": len(baseline_components),
            "ours": len(ours_components),
            "common": len(common_components),
            "ours_only": len(ours_only_components),
            "baseline_only": len(baseline_only_components),
            "ours_only_completeness": unique_completeness("ours", ours_only_components),
            "baseline_only_completeness": unique_completeness("baseline", baseline_only_components),
            "ours_only_by_kind": unique_profiles("ours", ours_only_components),
            "baseline_only_by_kind": unique_profiles("baseline", baseline_only_components),
        },
        "terminal_issue_groups": dict(issue_status),
        "path_instances": dict(instance_status),
        "matched_path_classification": dict(overall),
        "matched_unique_issue_classification": dict(matched_unique_classes),
        "evidence_transitions": {
            name: dict(counter) for name, counter in transitions.items()
        },
        "lifecycle_evidence_transitions": {
            name: dict(counter) for name, counter in lifecycle_transitions.items()
        },
        "evidence_transitions_by_kind": {
            kind: {name: dict(counter) for name, counter in values.items()}
            for kind, values in transitions_by_kind.items()
        },
        "ours_only_instances": {
            "count": len(ours_only_rows),
            "lifecycle_applicable": sum(
                row["ours_complete"] in {True, False} for row in ours_only_rows
            ),
            "lifecycle_complete": sum(row["ours_complete"] is True for row in ours_only_rows),
        },
        "baseline_only_instances": {
            "count": len(baseline_only_rows),
            "lifecycle_applicable": sum(
                row["baseline_complete"] in {True, False} for row in baseline_only_rows
            ),
            "lifecycle_complete": sum(
                row["baseline_complete"] is True for row in baseline_only_rows
            ),
        },
        "parse_errors": len(dataset_result["parse_errors"]),
    }
    if action_metadata:
        summary["input_metadata"] = action_metadata
    return summary


def summarize_ours_only(
    dataset_result: dict[str, Any], action_metadata: dict[str, Any] | None = None
) -> dict[str, Any]:
    """Summarize one released analyzer configuration without comparison claims."""
    reports: list[Report] = dataset_result["reports"]["ours"]
    applicable = [report for report in reports if report.lifecycle_applicable]
    unique = {report.global_issue_key for report in reports}
    evidence = {
        name: {
            "all_reports_with_witness": sum(report.has(name) for report in reports),
            "lifecycle_applicable_with_witness": sum(
                report.has(name) for report in applicable
            ),
        }
        for name in EVIDENCE_NAMES
    }
    lifecycle_by_kind: dict[str, dict[str, int]] = {}
    for kind in sorted({report.kind for report in applicable}):
        kind_reports = [report for report in applicable if report.kind == kind]
        lifecycle_by_kind[kind] = {
            "applicable": len(kind_reports),
            "complete": sum(
                report.lifecycle_complete() is True for report in kind_reports
            ),
        }
    summary: dict[str, Any] = {
        "mode": "ours-only",
        "analyzed_actions": sum(
            int(row["action_count"]) for row in dataset_result["action_rows"]
        ),
        "result_plists": sum(
            int(row["ours_plist_count"]) for row in dataset_result["action_rows"]
        ),
        "report_instances": len(reports),
        "unique_terminal_issues": len(unique),
        "by_checker_family": dict(
            sorted(Counter(report.checker_family for report in reports).items())
        ),
        "by_checker": dict(sorted(Counter(report.checker for report in reports).items())),
        "by_kind": dict(sorted(Counter(report.kind for report in reports).items())),
        "evidence_presence": evidence,
        "lifecycle": {
            "applicable_instances": len(applicable),
            "complete_instances": sum(
                report.lifecycle_complete() is True for report in applicable
            ),
            "by_kind": lifecycle_by_kind,
        },
        "parse_errors": len(dataset_result["parse_errors"]),
    }
    if action_metadata:
        # Deliberately omit working-directory fields from public derived data.
        summary["input_metadata"] = action_metadata
    return summary


CASE_RULES = (
    {
        "case": "shared_event_data_uaf",
        "checker": "cplusplus.NewDelete",
        "kind": "uaf",
        "terminal_suffix": "event_manager.cpp",
        "line": 104,
    },
    {
        "case": "failed_napi_wrap_cleanup",
        "checker": "cplusplus.NewDeleteLeaks",
        "kind": "leak",
        "terminal_suffix": "webview_javascript_execute_callback.cpp",
        "line": 222,
    },
    {
        "case": "custom_shared_ptr_ownership",
        "checker": "cplusplus.NewDeleteLeaks",
        "kind": "leak",
        "terminal_suffix": "object_factory.h",
        "line": 71,
    },
)


def case_study_metrics(dataset_result: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    reports = dataset_result["reports"]
    for rule in CASE_RULES:
        for side in ("baseline", "ours"):
            matches = [
                report
                for report in reports[side]
                if report.checker == rule["checker"]
                and report.kind == rule["kind"]
                and report.terminal.path.endswith(str(rule["terminal_suffix"]))
                and report.terminal.line == rule["line"]
            ]
            rows.append(
                {
                    "dataset": "OpenHarmony",
                    "case": rule["case"],
                    "side": side,
                    "report_instances": len(matches),
                    "unique_issues": len({report.global_issue_key for report in matches}),
                    "with_allocation": sum(report.has("allocation") for report in matches),
                    "with_release": sum(report.has("release") for report in matches),
                    "with_branch": sum(report.has("branch") for report in matches),
                    "lifecycle_complete": sum(
                        report.lifecycle_complete() is True for report in matches
                    ),
                    "terminal_locations": ";".join(
                        sorted({report.terminal.token() for report in matches})
                    ),
                }
            )
    return rows


def checker_summary_rows(dataset_result: dict[str, Any]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for side in ("baseline", "ours"):
        groups: dict[str, list[Report]] = defaultdict(list)
        for report in dataset_result["reports"][side]:
            groups[report.checker].append(report)
        for checker in sorted(groups):
            reports = groups[checker]
            applicable = [report for report in reports if report.lifecycle_applicable]
            rows.append(
                {
                    "dataset": reports[0].dataset,
                    "side": side,
                    "checker_family": reports[0].checker_family,
                    "checker": checker,
                    "report_instances": len(reports),
                    "unique_issues": len({report.global_issue_key for report in reports}),
                    "lifecycle_applicable": len(applicable),
                    "lifecycle_complete": sum(
                        report.lifecycle_complete() is True for report in applicable
                    ),
                }
            )
    return rows


def markdown_summary(summaries: dict[str, Any]) -> str:
    lines = [
        "# Real-project diagnostic evidence re-evaluation",
        "",
        "Ours-only results describe evidence in the released reports. In paired mode, matching never uses allocation, release, or branch witnesses; terminal locations identify bugs and evidence-stripped path skeletons pair duplicate instances.",
        "",
    ]
    ordered_datasets = [name for name in ("OpenHarmony", "Android") if name in summaries]
    ordered_datasets.extend(name for name in summaries if name not in ordered_datasets)
    for dataset in ordered_datasets:
        summary = summaries[dataset]
        if summary.get("mode") == "ours-only":
            lifecycle = summary["lifecycle"]
            lines.extend(
                [
                    f"## {dataset} (ours only)",
                    "",
                    f"- Analyzed actions: {summary['analyzed_actions']:,}",
                    f"- Result plist files: {summary['result_plists']:,}",
                    f"- Report instances / unique terminal issues: {summary['report_instances']:,} / {summary['unique_terminal_issues']:,}",
                    f"- Reports by checker family: `{json.dumps(summary['by_checker_family'], sort_keys=True)}`",
                    f"- Reports by checker: `{json.dumps(summary['by_checker'], sort_keys=True)}`",
                    f"- Reports by defect kind: `{json.dumps(summary['by_kind'], sort_keys=True)}`",
                    f"- Lifecycle-complete applicable reports: {lifecycle['complete_instances']:,}/{lifecycle['applicable_instances']:,}",
                    f"- Lifecycle completeness by defect kind: `{json.dumps(lifecycle['by_kind'], sort_keys=True)}`",
                    "",
                    "Evidence present in emitted paths:",
                    "",
                    "| Evidence | All reports | Lifecycle-applicable reports |",
                    "|---|---:|---:|",
                ]
            )
            for name in EVIDENCE_NAMES:
                values = summary["evidence_presence"][name]
                lines.append(
                    f"| {name} | {values['all_reports_with_witness']} | {values['lifecycle_applicable_with_witness']} |"
                )
            lines.extend(["", f"- Parse errors: {summary['parse_errors']}"])
            if "input_metadata" in summary:
                lines.append(
                    f"- Input metadata: `{json.dumps(summary['input_metadata'], sort_keys=True)}`"
                )
            if summary.get("case_studies"):
                lines.extend(["", "Case-study metrics:", ""])
                for row in summary["case_studies"]:
                    if row["side"] != "ours":
                        continue
                    lines.append(
                        f"- {row['case']}: {row['report_instances']} reports, "
                        f"{row['unique_issues']} unique issues, {row['lifecycle_complete']} lifecycle-complete"
                    )
            lines.append("")
            continue
        actions = summary["action_pairing"]
        reports = summary["reports_in_paired_actions"]
        unique = summary["global_unique_issues"]
        lines.extend(
            [
                f"## {dataset}",
                "",
                f"- Paired analysis actions: {actions['paired_analysis_actions']:,}",
                f"- Action groups by status: `{json.dumps(actions['status_groups'], sort_keys=True)}`",
                f"- Baseline reports / unique terminal issues: {reports['baseline']['report_instances']:,} / {reports['baseline']['unique_terminal_issues']:,}",
                f"- Ours reports / unique terminal issues: {reports['ours']['report_instances']:,} / {reports['ours']['unique_terminal_issues']:,}",
                f"- Baseline reports by checker family: `{json.dumps(reports['baseline']['by_checker_family'], sort_keys=True)}`",
                f"- Ours reports by checker family: `{json.dumps(reports['ours']['by_checker_family'], sort_keys=True)}`",
                f"- Baseline reports by checker: `{json.dumps(reports['baseline']['by_checker'], sort_keys=True)}`",
                f"- Ours reports by checker: `{json.dumps(reports['ours']['by_checker'], sort_keys=True)}`",
                f"- Baseline reports by defect kind: `{json.dumps(reports['baseline']['by_kind'], sort_keys=True)}`",
                f"- Ours reports by defect kind: `{json.dumps(reports['ours']['by_kind'], sort_keys=True)}`",
                f"- Global unique issues (common / ours-only / baseline-only): {unique['common']:,} / {unique['ours_only']:,} / {unique['baseline_only']:,}",
                f"- Terminal issue groups: `{json.dumps(summary['terminal_issue_groups'], sort_keys=True)}`",
                f"- Path instances: `{json.dumps(summary['path_instances'], sort_keys=True)}`",
                f"- Matched-path classes: `{json.dumps(summary['matched_path_classification'], sort_keys=True)}`",
                "",
                "Lifecycle-applicable matched paths (non-applicable diagnostics are excluded):",
                "",
                "| Evidence | Gained | Retained | Lost | Absent in both |",
                "|---|---:|---:|---:|---:|",
            ]
        )
        for evidence in EVIDENCE_NAMES:
            values = summary["lifecycle_evidence_transitions"].get(evidence, {})
            lines.append(
                f"| {evidence} | {values.get('gained', 0)} | {values.get('retained', 0)} | {values.get('lost', 0)} | {values.get('absent_in_both', 0)} |"
            )
        ours_only = summary["ours_only_instances"]
        baseline_only = summary["baseline_only_instances"]
        new_complete = unique["ours_only_completeness"]
        removed_complete = unique["baseline_only_completeness"]
        lines.extend(
            [
                "",
                f"- Matched unique-issue classes: `{json.dumps(summary['matched_unique_issue_classification'], sort_keys=True)}`",
                f"- Ours-only lifecycle-applicable unique issues complete in every instance: {new_complete['all_instances_complete']}/{new_complete['applicable']} (all ours-only issues: {new_complete['count']})",
                f"- Baseline-only lifecycle-applicable unique issues complete in every instance: {removed_complete['all_instances_complete']}/{removed_complete['applicable']} (all baseline-only issues: {removed_complete['count']})",
                f"- Action-local ours-only lifecycle-complete instances: {ours_only['lifecycle_complete']}/{ours_only['lifecycle_applicable']} (all instances: {ours_only['count']})",
                f"- Action-local baseline-only lifecycle-complete instances: {baseline_only['lifecycle_complete']}/{baseline_only['lifecycle_applicable']} (all instances: {baseline_only['count']})",
                f"- Parse errors: {summary['parse_errors']}",
            ]
        )
        if "input_metadata" in summary:
            lines.append(
                f"- Input metadata: `{json.dumps(summary['input_metadata'], sort_keys=True)}`"
            )
        if summary.get("case_studies"):
            lines.extend(["", "Case-study metrics:", ""])
            for row in summary["case_studies"]:
                lines.append(
                    f"- {row['case']} ({row['side']}): {row['report_instances']} reports, "
                    f"{row['unique_issues']} unique issues, {row['lifecycle_complete']} lifecycle-complete"
                )
        lines.append("")
    return "\n".join(lines)


def write_results(output: Path, combined: dict[str, Any], summaries: dict[str, Any]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    action_fields = [
        "dataset", "action_id", "source_path", "status", "reason", "action_count",
        "baseline_plist_count", "ours_plist_count", "baseline_configs", "ours_configs",
    ]
    issue_fields = [
        "dataset", "action_id", "issue_pair_id", "status", "match_tier", "reason",
        "checker", "kind", "terminal_path", "terminal_line", "terminal_col",
        "baseline_instances", "ours_instances", "baseline_hashes", "ours_hashes",
    ]
    instance_fields = [
        "dataset", "action_id", "issue_pair_id", "status", "baseline_report_id",
        "ours_report_id", "match_cost", "baseline_complete", "ours_complete", "overall_class",
    ]
    evidence_fields = [
        "dataset", "action_id", "issue_pair_id", "baseline_report_id", "ours_report_id",
        "checker_family", "checker", "kind", "lifecycle_applicable", "evidence",
        "transition", "baseline_locations", "ours_locations",
    ]
    report_fields = [
        "dataset", "side", "action_id", "report_id", "plist_name", "diagnostic_index",
        "checker_family", "checker", "kind", "description", "issue_hash",
        "terminal_path", "terminal_line", "terminal_col", "has_allocation", "has_release",
        "has_branch", "lifecycle_applicable", "lifecycle_complete", "path_events",
    ]
    checker_fields = [
        "dataset", "side", "checker_family", "checker", "report_instances",
        "unique_issues", "lifecycle_applicable", "lifecycle_complete",
    ]
    case_fields = [
        "dataset", "case", "side", "report_instances", "unique_issues",
        "with_allocation", "with_release", "with_branch", "lifecycle_complete",
        "terminal_locations",
    ]
    error_fields = ["dataset", "side", "action_id", "plist", "error"]
    write_csv(output / "action_pairs.csv", action_fields, combined["action_rows"])
    write_csv(output / "issue_pairs.csv", issue_fields, combined["issue_rows"])
    write_csv(output / "instance_pairs.csv", instance_fields, combined["instance_rows"])
    write_csv(output / "evidence_changes.csv", evidence_fields, combined["evidence_rows"])
    write_csv(output / "report_instances.csv", report_fields, combined["report_rows"])
    write_csv(output / "checker_summary.csv", checker_fields, combined["checker_rows"])
    write_csv(output / "case_study_metrics.csv", case_fields, combined["case_rows"])
    write_csv(output / "parse_errors.csv", error_fields, combined["parse_errors"])
    with (output / "summary.json").open("w", encoding="utf-8") as stream:
        json.dump(summaries, stream, indent=2, sort_keys=True)
        stream.write("\n")
    (output / "summary.md").write_text(markdown_summary(summaries), encoding="utf-8")
    with (output / "paper_metrics.json").open("w", encoding="utf-8") as stream:
        json.dump(summaries, stream, indent=2, sort_keys=True)
        stream.write("\n")


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--workspace",
        type=Path,
        default=Path(__file__).resolve().parents[2],
        help="artifact root used to resolve relative input and output paths",
    )
    parser.add_argument(
        "--mode", choices=("ours-only", "paired"), default="ours-only"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("experiments/results/analysis_output"),
    )
    parser.add_argument("--openharmony-baseline", type=Path)
    parser.add_argument(
        "--openharmony-ours",
        type=Path,
        default=Path("experiments/results/unpacked/openharmony/reports_exp_64"),
    )
    parser.add_argument(
        "--android-baseline", type=Path
    )
    parser.add_argument(
        "--android-ours",
        type=Path,
        default=Path("experiments/results/unpacked/android/reports_exp2_64"),
    )
    parser.add_argument(
        "--dataset", choices=("all", "openharmony", "android"), default="all"
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(8, os.cpu_count() or 1),
        help="number of worker processes used to parse independent actions",
    )
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    workspace = args.workspace.resolve()
    output = args.output
    if not output.is_absolute():
        output = workspace / output

    def resolved_input(path: Path) -> Path:
        return path if path.is_absolute() else workspace / path

    def require_directory(path: Path, label: str) -> Path:
        resolved = resolved_input(path)
        if not resolved.is_dir():
            raise SystemExit(
                f"{label} directory not found: {resolved}\n"
                "Download and extract the result packages as described in "
                "experiments/results/README.md, or pass an explicit directory."
            )
        return resolved

    if args.mode == "paired":
        missing: list[str] = []
        if args.dataset in {"all", "openharmony"} and args.openharmony_baseline is None:
            missing.append("--openharmony-baseline")
        if args.dataset in {"all", "android"} and args.android_baseline is None:
            missing.append("--android-baseline")
        if missing:
            raise SystemExit(
                "paired mode requires baseline inputs: " + ", ".join(missing)
            )

    print(f"[configuration] mode: {args.mode}", file=sys.stderr)
    print("[configuration] checker scope: cplusplus.*, core.*, unix.*", file=sys.stderr)
    print(
        "[configuration] lifecycle evidence: cplusplus.NewDelete, "
        "cplusplus.NewDeleteLeaks, unix.Malloc (classified leak/UAF/double free)",
        file=sys.stderr,
    )
    print(f"[configuration] parser jobs: {max(1, args.jobs)}", file=sys.stderr)
    if args.dataset in {"all", "openharmony"}:
        baseline_text = (
            str(resolved_input(args.openharmony_baseline))
            if args.openharmony_baseline is not None else "not supplied"
        )
        print(f"[configuration] OpenHarmony baseline={baseline_text} ours={resolved_input(args.openharmony_ours)}", file=sys.stderr)
    if args.dataset in {"all", "android"}:
        baseline_text = (
            str(resolved_input(args.android_baseline))
            if args.android_baseline is not None else "not supplied"
        )
        print(f"[configuration] Android baseline={baseline_text} ours={resolved_input(args.android_ours)}", file=sys.stderr)
    print(f"[configuration] output={output}", file=sys.stderr, flush=True)

    combined = {
        "action_rows": [], "issue_rows": [], "instance_rows": [],
        "evidence_rows": [], "report_rows": [], "checker_rows": [],
        "case_rows": [], "parse_errors": [],
    }
    summaries: dict[str, Any] = {}

    if args.dataset in {"all", "openharmony"}:
        ours_dir = require_directory(args.openharmony_ours, "OpenHarmony ours")
        if args.mode == "paired":
            assert args.openharmony_baseline is not None
            baseline_dir = require_directory(
                args.openharmony_baseline, "OpenHarmony baseline"
            )
            actions = openharmony_action_groups(baseline_dir, ours_dir)
        else:
            actions = openharmony_ours_action_groups(ours_dir)
        result = analyze_dataset(actions, jobs=max(1, args.jobs))
        summaries["OpenHarmony"] = (
            summarize(result)
            if args.mode == "paired"
            else summarize_ours_only(result)
        )
        cases = case_study_metrics(result)
        summaries["OpenHarmony"]["case_studies"] = cases
        for key in ("action_rows", "issue_rows", "instance_rows", "evidence_rows", "report_rows", "parse_errors"):
            combined[key].extend(result[key])
        combined["checker_rows"].extend(checker_summary_rows(result))
        combined["case_rows"].extend(cases)

    if args.dataset in {"all", "android"}:
        ours_dir = require_directory(args.android_ours, "Android ours")
        if args.mode == "paired":
            assert args.android_baseline is not None
            baseline_dir = require_directory(args.android_baseline, "Android baseline")
            actions, metadata = android_action_groups(baseline_dir, ours_dir)
        else:
            actions, metadata = android_ours_action_groups(ours_dir)
        expected_groups = ["cplusplus", "core", "unix"]
        sides = ("baseline", "ours") if args.mode == "paired" else ("ours",)
        for side in sides:
            actual = metadata[f"{side}_checker_groups"]
            if actual != expected_groups:
                raise RuntimeError(
                    f"Android {side} checker groups are {actual}, expected {expected_groups}"
                )
        result = analyze_dataset(actions, jobs=max(1, args.jobs))
        summaries["Android"] = (
            summarize(result, metadata)
            if args.mode == "paired"
            else summarize_ours_only(result, metadata)
        )
        for key in ("action_rows", "issue_rows", "instance_rows", "evidence_rows", "report_rows", "parse_errors"):
            combined[key].extend(result[key])
        combined["checker_rows"].extend(checker_summary_rows(result))

    write_results(output, combined, summaries)
    print(markdown_summary(summaries))
    return 0


if __name__ == "__main__":
    sys.exit(main())
