#!/usr/bin/env bash

set -euo pipefail

PKG_ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"

if (( $# == 2 )) && [[ "$1" == "--root" ]]; then
    if [[ ! -d "$2" ]]; then
        printf 'check-docs: root directory is missing: %s\n' "$2" >&2
        exit 64
    fi
    PKG_ROOT="$(cd "$2" && pwd -P)"
elif (( $# != 0 )); then
    printf 'usage: %s [--root directory]\n' "$0" >&2
    exit 64
fi

python3 - "$PKG_ROOT" <<'PY'
from __future__ import annotations

import html
import pathlib
import re
import sys
import urllib.parse


root = pathlib.Path(sys.argv[1]).resolve()
docs_dir = root / "docs"
errors: list[str] = []


def display(path: pathlib.Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return str(path)


if not docs_dir.is_dir():
    raise SystemExit("check-docs: docs directory is missing")

chinese_docs = sorted(
    path for path in docs_dir.glob("*.md") if not path.name.endswith(".en.md")
)
english_docs = sorted(docs_dir.glob("*.en.md"))

pairs: list[tuple[pathlib.Path, pathlib.Path]] = [
    (root / "README.md", root / "README.en.md"),
    (root / "CHANGELOG.md", root / "CHANGELOG.en.md"),
]

for chinese in chinese_docs:
    english = chinese.with_name(f"{chinese.stem}.en.md")
    if not english.is_file():
        errors.append(f"{display(chinese)}: missing English pair {english.name}")
    else:
        pairs.append((chinese, english))

for english in english_docs:
    chinese_name = f"{english.name[:-len('.en.md')]}.md"
    chinese = english.with_name(chinese_name)
    if not chinese.is_file():
        errors.append(f"{display(english)}: missing Chinese pair {chinese.name}")

for chinese, english in pairs:
    if not chinese.is_file():
        errors.append(f"{display(chinese)}: missing Chinese document")
        continue
    if not english.is_file():
        errors.append(f"{display(english)}: missing English document")
        continue

    chinese_switch = f"[English]({english.name})"
    english_switch = f"[简体中文]({chinese.name})"
    chinese_text = chinese.read_text(encoding="utf-8")
    english_text = english.read_text(encoding="utf-8")
    if chinese_switch not in chinese_text:
        errors.append(
            f"{display(chinese)}: missing language switch {chinese_switch}"
        )
    if english_switch not in english_text:
        errors.append(
            f"{display(english)}: missing language switch {english_switch}"
        )

required_root_docs = {
    root / "README.md",
    root / "README.en.md",
    root / "NOTICE.md",
    root / "CHANGELOG.md",
    root / "CHANGELOG.en.md",
}
optional_root_docs = {
    root / "NOTICE.en.md",
}
markdown_files = sorted(
    {
        *required_root_docs,
        *(path for path in optional_root_docs if path.is_file()),
        *docs_dir.glob("*.md"),
    }
)
inline_link = re.compile(
    r"!?\[[^\]]*\]\(\s*(<[^>]+>|[^)\s]+)"
    r"(?:\s+['\"][^)]*['\"])?\s*\)"
)
reference_definition = re.compile(
    r"^ {0,3}\[([^\]]+)\]:\s*(<[^>]+>|\S+)"
)
reference_usage = re.compile(r"(?<!\\)!?\[([^\]]+)\]\s*\[([^\]]*)\]")
fence_start = re.compile(r"^ {0,3}(`{3,}|~{3,})(.*)$")
heading_start = re.compile(r"^ {0,3}#{1,6}\s+(.+?)\s*$")
anchor_cache: dict[pathlib.Path, set[str]] = {}


def normalize_reference_label(label: str) -> str:
    """Apply Markdown's case-insensitive, whitespace-collapsing label rules."""
    return " ".join(html.unescape(label).split()).casefold()


def reference_definitions(lines: list[str]) -> dict[str, tuple[int, str]]:
    definitions: dict[str, tuple[int, str]] = {}
    open_fence: tuple[str, int] | None = None

    for line_number, line in enumerate(lines, start=1):
        fence = fence_start.match(line)
        if fence:
            marker = fence.group(1)
            remainder = fence.group(2)
            if open_fence is None:
                open_fence = (marker[0], len(marker))
            elif (
                marker[0] == open_fence[0]
                and len(marker) >= open_fence[1]
                and not remainder.strip()
            ):
                open_fence = None
            continue
        if open_fence is not None:
            continue

        definition = reference_definition.match(line)
        if not definition:
            continue
        label = normalize_reference_label(definition.group(1))
        if label and label not in definitions:
            definitions[label] = (line_number, definition.group(2))

    return definitions


def markdown_anchors(path: pathlib.Path) -> set[str]:
    cached = anchor_cache.get(path)
    if cached is not None:
        return cached

    anchors: set[str] = set()
    occurrences: dict[str, int] = {}
    open_fence: tuple[str, int] | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fence = fence_start.match(line)
        if fence:
            marker = fence.group(1)
            remainder = fence.group(2)
            if open_fence is None:
                open_fence = (marker[0], len(marker))
            elif (
                marker[0] == open_fence[0]
                and len(marker) >= open_fence[1]
                and not remainder.strip()
            ):
                open_fence = None
            continue
        if open_fence is not None:
            continue

        heading_match = heading_start.match(line)
        if not heading_match:
            continue
        heading = re.sub(r"\s+#+\s*$", "", heading_match.group(1))
        heading = re.sub(r"!?\[([^\]]+)\]\([^)]*\)", r"\1", heading)
        heading = re.sub(r"<[^>]+>", "", heading)
        heading = html.unescape(heading).replace("`", "").lower()
        slug = "".join(
            character
            for character in heading
            if character.isalnum()
            or character.isspace()
            or character in {"-", "_"}
        )
        slug = re.sub(r"\s", "-", slug)
        if not slug:
            continue
        occurrence = occurrences.get(slug, 0)
        occurrences[slug] = occurrence + 1
        anchor = slug if occurrence == 0 else f"{slug}-{occurrence}"
        anchors.add(anchor)

    anchor_cache[path] = anchors
    return anchors


def validate_target(source: pathlib.Path, line_number: int, raw: str) -> None:
    target = raw[1:-1] if raw.startswith("<") and raw.endswith(">") else raw
    parsed = urllib.parse.urlsplit(target)
    if parsed.scheme in {"http", "https", "mailto"} or target.startswith("//"):
        return
    if parsed.scheme:
        errors.append(
            f"{display(source)}:{line_number}: unsupported link scheme in {target!r}"
        )
        return

    decoded_path = urllib.parse.unquote(parsed.path)
    resolved = source
    if decoded_path:
        relative = pathlib.Path(decoded_path)
        if relative.is_absolute():
            errors.append(
                f"{display(source)}:{line_number}: local link must be relative: "
                f"{target!r}"
            )
            return
        resolved = (source.parent / relative).resolve()
    elif not parsed.fragment:
        return

    try:
        resolved.relative_to(root)
    except ValueError:
        errors.append(
            f"{display(source)}:{line_number}: local link escapes repository: {target!r}"
        )
        return
    if not resolved.exists():
        errors.append(
            f"{display(source)}:{line_number}: relative link target is missing: "
            f"{target!r}"
        )
        return

    if parsed.fragment and resolved.is_file() and resolved.suffix.lower() == ".md":
        fragment = urllib.parse.unquote(parsed.fragment)
        if fragment not in markdown_anchors(resolved):
            errors.append(
                f"{display(source)}:{line_number}: Markdown fragment is missing: "
                f"{target!r}"
            )


for path in markdown_files:
    if not path.is_file():
        errors.append(f"{display(path)}: Markdown file is missing")
        continue

    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    definitions = reference_definitions(lines)
    open_fence: tuple[str, int, int] | None = None

    for line_number, line in enumerate(lines, start=1):
        if line.endswith((" ", "\t")):
            errors.append(f"{display(path)}:{line_number}: trailing whitespace")

        fence = fence_start.match(line)
        if fence:
            marker = fence.group(1)
            remainder = fence.group(2)
            if open_fence is None:
                open_fence = (marker[0], len(marker), line_number)
                continue
            fence_char, fence_length, _ = open_fence
            if (
                marker[0] == fence_char
                and len(marker) >= fence_length
                and not remainder.strip()
            ):
                open_fence = None
            continue

        if open_fence is not None:
            continue

        seen: set[tuple[int, str]] = set()
        for match in inline_link.finditer(line):
            seen.add((match.start(1), match.group(1)))
        definition = reference_definition.match(line)
        if definition:
            seen.add((definition.start(2), definition.group(2)))
        for _, target in sorted(seen):
            validate_target(path, line_number, target)

        for reference in reference_usage.finditer(line):
            label = reference.group(2) or reference.group(1)
            normalized = normalize_reference_label(label)
            if normalized not in definitions:
                errors.append(
                    f"{display(path)}:{line_number}: undefined reference-style "
                    f"link: {label!r}"
                )

    if open_fence is not None:
        _, _, opening_line = open_fence
        errors.append(
            f"{display(path)}:{opening_line}: Markdown fence is not closed"
        )

if errors:
    for error in errors:
        print(f"check-docs: {error}", file=sys.stderr)
    raise SystemExit(1)

print(
    f"Documentation checks passed ({len(markdown_files)} files, "
    f"{len(pairs)} language pairs)."
)
PY
