# -*- coding: utf-8 -*-
"""Verify and fix the per-module atlas JSON files in data/.

- Confirms every class entry's file exists and the declared name appears near
  the claimed line; if not, greps the file for the declaration and fixes the
  line in place.
- Reports header declarations (class/struct/enum class) that no JSON entry
  covers, so gaps can be backfilled.

Exit code 1 when unresolvable (BROKEN) entries remain. GAP lines are
informational: aggregate topic entries (kind "functions") and types shown as
members of a covered parent are expected to appear here.
"""
import io
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
ATLAS = Path(__file__).resolve().parent / "data"

MODULE_HEADER_DIRS = {
    "core": ["modules/core/source"],
    "domain": ["modules/domain/source"],
    "vision-image": ["modules/vision/source", "modules/image/source"],
    "annotation": ["modules/annotation/source"],
    "controller": ["modules/controller/source"],
    "engine": ["modules/engine/source"],
    "script": ["modules/script/source"],
    "entry-cli": ["entry/cli"],
    "entry-workbench": ["entry/workbench"],
}

DECL_RE = re.compile(r"^\s*(?:export\s+)?(class|struct|enum\s+class|enum)\s+([A-Za-z_]\w*)")
FWD_RE = re.compile(r"^\s*(?:class|struct)\s+[A-Za-z_]\w*\s*;\s*$")


def load(path):
    with io.open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, data):
    # LF plus a trailing newline, because scripts/fix_format.py normalizes every
    # .json in the repository that way. Writing platform newlines or omitting the
    # final newline made every auto-fixed data file fail the format gate right
    # after the atlas pipeline had just been run as documented.
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")


def verify_module(json_path):
    data = load(json_path)
    fixed, broken = [], []
    for cls in data.get("classes", []):
        rel = cls["file"].replace("\\", "/")
        src = REPO / rel
        if not src.exists():
            broken.append((cls["name"], rel, "file missing"))
            continue
        lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
        short = cls["name"].split("::")[-1]
        line = int(cls.get("line", 0))
        is_topic = cls.get("kind") == "functions" or not short.isidentifier()
        if is_topic:  # aggregate topic entry, name is not a real declaration
            if not (1 <= line <= len(lines)):
                cls["line"] = 1
                fixed.append((cls["name"], rel, "line clamped"))
            continue
        lo, hi = max(0, line - 3), min(len(lines), line + 2)
        if any(short in lines[i] for i in range(lo, hi)):
            continue
        hit = next((i + 1 for i, text in enumerate(lines)
                    if re.search(r"\b(class|struct|enum)\b.*\b%s\b" % re.escape(short), text)
                    and not FWD_RE.match(text)), None)
        if hit is None:
            hit = next((i + 1 for i, text in enumerate(lines) if re.search(r"\b%s\b" % re.escape(short), text)), None)
        if hit is not None:
            fixed.append((cls["name"], rel, "line %d -> %d" % (line, hit)))
            cls["line"] = hit
        else:
            broken.append((cls["name"], rel, "name not found in file"))
    save(json_path, data)
    return data, fixed, broken


def coverage_gaps(module_key, data):
    covered = {c["name"].split("::")[-1] for c in data.get("classes", [])}
    gaps = []
    for d in MODULE_HEADER_DIRS.get(module_key, []):
        root = REPO / d
        if not root.exists():
            continue
        for hpp in root.rglob("*.hpp"):
            text = hpp.read_text(encoding="utf-8", errors="replace")
            for raw in text.splitlines():
                if FWD_RE.match(raw):
                    continue
                m = DECL_RE.match(raw)
                if m and raw.rstrip().endswith((";",)) and "{" not in raw:
                    continue  # forward declaration
                if m and m.group(2) not in covered:
                    gaps.append((m.group(2), str(hpp.relative_to(REPO)).replace("\\", "/")))
                    covered.add(m.group(2))  # report each name once
    return gaps


def main():
    total_fixed = total_broken = 0
    for json_path in sorted(ATLAS.glob("*.json")):
        data, fixed, broken = verify_module(json_path)
        key = data.get("module", json_path.stem)
        gaps = coverage_gaps(key, data)
        print("== %s: %d classes ==" % (key, len(data.get("classes", []))))
        for name, rel, what in fixed:
            print("  FIXED  %-52s %s (%s)" % (name, rel, what))
        for name, rel, what in broken:
            print("  BROKEN %-52s %s (%s)" % (name, rel, what))
        for name, rel in gaps:
            print("  GAP    %-52s %s" % (name, rel))
        total_fixed += len(fixed)
        total_broken += len(broken)
    print("\nfixed=%d broken=%d" % (total_fixed, total_broken))
    return 1 if total_broken else 0


if __name__ == "__main__":
    sys.exit(main())
