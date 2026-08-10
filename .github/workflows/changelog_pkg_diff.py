#!/usr/bin/env python3
"""Per-package diffing of mkosi's ManifestFormat=changelog output.

Splits a changelog file into per-SourcePackage blocks so a package's
changelog can be listed or diffed individually, instead of diffing the
whole file at once (which produces a diff too large for an LLM's context
window when even one package's history changes substantially).
"""

import argparse
import difflib
import lzma
import re
import sys

VERSION_RE = re.compile(r"^(\S+) \(([^)]+)\) [\w,\-]+; urgency=\S+", re.M)
CVE_RE = re.compile(r"CVE-\d{4}-\d+")
MAX_CVES_SHOWN = 8


def read_maybe_xz(path: str) -> str:
    opener = lzma.open if path.endswith(".xz") else open
    with opener(path, "rt", errors="replace") as f:
        return f.read()


def parse_blocks(text: str) -> dict[str, str]:
    blocks: dict[str, str] = {}
    name = None
    buf: list[str] = []
    for line in text.splitlines(keepends=True):
        if line.startswith("SourcePackage: "):
            if name is not None:
                blocks[name] = "".join(buf)
            name = line[len("SourcePackage: "):].strip()
            buf = [line]
        elif name is not None:
            buf.append(line)
    if name is not None:
        blocks[name] = "".join(buf)
    return blocks


def main() -> None:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_list = sub.add_parser("list", help="print names of packages whose block changed")
    p_list.add_argument("old")
    p_list.add_argument("new")

    p_diff_all = sub.add_parser(
        "diff-all", help="print unified diffs for every package whose block changed"
    )
    p_diff_all.add_argument("old")
    p_diff_all.add_argument("new")

    p_summarize = sub.add_parser(
        "summarize",
        help="print a deterministic human-readable Markdown summary of package "
        "changes, grouped by security fixes / other updates / added / removed",
    )
    p_summarize.add_argument("old")
    p_summarize.add_argument("new")

    args = ap.parse_args()

    old_blocks = parse_blocks(read_maybe_xz(args.old))
    new_blocks = parse_blocks(read_maybe_xz(args.new))
    changed = sorted(
        name
        for name in set(old_blocks) | set(new_blocks)
        if old_blocks.get(name) != new_blocks.get(name)
    )

    if args.cmd == "list":
        for name in changed:
            print(name)
    elif args.cmd == "diff-all":
        for name in changed:
            old_text = old_blocks.get(name, f"SourcePackage: {name}\n(package did not exist previously)\n")
            new_text = new_blocks.get(name, f"SourcePackage: {name}\n(package removed)\n")
            sys.stdout.writelines(
                difflib.unified_diff(
                    old_text.splitlines(keepends=True),
                    new_text.splitlines(keepends=True),
                    fromfile="old",
                    tofile="new",
                )
            )
    elif args.cmd == "summarize":
        print(summarize(old_blocks, new_blocks))


def first_version(block: str) -> str | None:
    m = VERSION_RE.search(block)
    return m.group(2) if m else None


def summarize(old_blocks: dict[str, str], new_blocks: dict[str, str]) -> str:
    """Build a deterministic Markdown summary: no maintainer names, no diff
    markup, grouped by security fixes / other version bumps / added / removed.
    Used as the fallback when an LLM-generated summary isn't available, and
    good enough to ship as-is.
    """
    changed = sorted(set(old_blocks) | set(new_blocks))

    security: list[tuple[str, str | None, str | None, list[str]]] = []
    other_updates: list[tuple[str, str | None, str | None]] = []
    added: list[tuple[str, str]] = []
    removed: list[tuple[str, str]] = []

    for name in changed:
        old_text = old_blocks.get(name, "")
        new_text = new_blocks.get(name, "")
        if old_text == new_text:
            continue

        old_ver = first_version(old_text)
        new_ver = first_version(new_text)

        if not old_text and new_ver:
            added.append((name, new_ver))
            continue
        if not new_text and old_ver:
            removed.append((name, old_ver))
            continue

        added_lines = "\n".join(
            line[1:]
            for line in difflib.unified_diff(
                old_text.splitlines(), new_text.splitlines(), lineterm=""
            )
            if line.startswith("+") and not line.startswith("+++")
        )
        cves = sorted(set(CVE_RE.findall(added_lines)))

        if old_ver == new_ver and not cves:
            continue  # rebuild-only noise, nothing user-visible changed
        if cves:
            security.append((name, old_ver, new_ver, cves))
        else:
            other_updates.append((name, old_ver, new_ver))

    lines = [
        "Package versions changed across the image; this note lists security "
        "fixes pulled from each package's changelog. Purely internal/"
        "packaging-only updates are omitted.",
        "",
    ]

    if security:
        lines.append("## Security fixes")
        lines.append("")
        for name, ov, nv, cves in security:
            shown = cves[:MAX_CVES_SHOWN]
            rest = len(cves) - len(shown)
            cve_str = ", ".join(shown) + (f", and {rest} more" if rest > 0 else "")
            lines.append(f"- **{name}** (`{ov or 'unknown'}` → `{nv or 'unknown'}`): {cve_str}")
        lines.append("")

    if other_updates:
        lines.append("## Other package updates")
        lines.append("")
        for name, ov, nv in other_updates:
            lines.append(f"- {name}: `{ov or 'unknown'}` → `{nv or 'unknown'}`")
        lines.append("")

    if added:
        lines.append("## Added packages")
        lines.append("")
        for name, nv in added:
            lines.append(f"- {name} (`{nv}`)")
        lines.append("")

    if removed:
        lines.append("## Removed packages")
        lines.append("")
        for name, ov in removed:
            lines.append(f"- {name} (was `{ov}`)")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


if __name__ == "__main__":
    main()
