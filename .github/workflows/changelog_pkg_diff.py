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
import sys


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

    p_diff = sub.add_parser("diff", help="print a unified diff for one package")
    p_diff.add_argument("old")
    p_diff.add_argument("new")
    p_diff.add_argument("package")

    args = ap.parse_args()

    old_blocks = parse_blocks(read_maybe_xz(args.old))
    new_blocks = parse_blocks(read_maybe_xz(args.new))

    if args.cmd == "list":
        for name in sorted(set(old_blocks) | set(new_blocks)):
            if old_blocks.get(name) != new_blocks.get(name):
                print(name)
    elif args.cmd == "diff":
        name = args.package
        if name not in old_blocks and name not in new_blocks:
            print(f"Unknown package: {name}", file=sys.stderr)
            sys.exit(1)
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


if __name__ == "__main__":
    main()
