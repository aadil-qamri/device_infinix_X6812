#!/usr/bin/env python3
"""Compare proprietary-files.txt and extract-files.py against x6812.txt (source of truth).

Comments out missing blobs/entries, or removes them when --remove is passed.
"""

import argparse
import os
import re
import sys

TAG = "# [MISSING-IN-TRUTH]"


def normalize_truth_entry(path: str) -> str:
    path = path.strip()
    if path.startswith("system/system/"):
        return "system/" + path[len("system/system/"):]
    return path


def load_truth(truth_file: str):
    exact_set = set()
    basename_map = {}  # basename -> list of normalized paths

    with open(truth_file, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            p = line.strip()
            if not p or p.startswith("#"):
                continue
            norm = normalize_truth_entry(p)
            exact_set.add(norm)
            exact_set.add(p)

            bname = os.path.basename(norm)
            basename_map.setdefault(bname, []).append(norm)

    return exact_set, basename_map


def parse_blob_target(raw_line: str) -> str:
    """Extract source path from a proprietary-files.txt line."""
    line = raw_line.strip()
    if not line or line.startswith("#"):
        return ""

    if line.startswith("-"):
        line = line[1:].strip()

    line = line.split("|")[0].split(";")[0].strip()

    if ":" in line:
        parts = line.split(":", 1)
        src = parts[0].strip()
        if src:
            return src
    return line


def resolve_truth_path(path: str, exact_set: set, basename_map: dict) -> str | None:
    """Find matching path in truth, auto-mapping partitions and paths."""
    if not path:
        return None

    if path in exact_set:
        return path

    norm = normalize_truth_entry(path)
    if norm in exact_set:
        return norm

    # 1. Partition redirects: odm/... -> vendor/odm/... or vendor/...
    if path.startswith("odm/"):
        rel = path[len("odm/"):]
        if f"vendor/odm/{rel}" in exact_set:
            return f"vendor/odm/{rel}"
        if f"vendor/{rel}" in exact_set:
            return f"vendor/{rel}"
        # odm/etc/audio/audio_param/... -> vendor/etc/audio_param/...
        if rel.startswith("etc/audio/"):
            alt = f"vendor/etc/{rel[len('etc/audio/'):]}"
            if alt in exact_set:
                return alt

    if path.startswith("system_ext/"):
        rel = path[len("system_ext/"):]
        if f"system/system_ext/{rel}" in exact_set:
            return f"system/system_ext/{rel}"
        if f"system/{rel}" in exact_set:
            return f"system/{rel}"

    # 2. Basename fallback matching same bitness (lib/lib64)
    bname = os.path.basename(path)
    candidates = basename_map.get(bname, [])
    if candidates:
        is_lib64 = "lib64" in path
        is_lib32 = "/lib/" in path and "lib64" not in path

        for cand in candidates:
            cand_lib64 = "lib64" in cand
            cand_lib32 = "/lib/" in cand and "lib64" not in cand

            if is_lib64 and not cand_lib64:
                continue
            if is_lib32 and not cand_lib32:
                continue
            return cand

    return None


def replace_blob_target(raw_line: str, old_target: str, new_target: str) -> str:
    """Replace target path in a proprietary-files line preserving flags/pins."""
    return raw_line.replace(old_target, new_target, 1)


def process_proprietary_files(prop_path: str, exact_set: set, basename_map: dict, remove: bool):
    with open(prop_path, "r", encoding="utf-8") as f:
        lines = f.readlines()

    out_lines = []
    commented_count = 0
    removed_count = 0
    renamed_count = 0
    seen_blobs = set()

    for line in lines:
        stripped = line.strip()
        if not stripped:
            out_lines.append(line)
            continue

        if stripped.startswith("#"):
            if remove and stripped.startswith(TAG):
                removed_count += 1
                continue
            out_lines.append(line)
            continue

        target = parse_blob_target(stripped)
        resolved = resolve_truth_path(target, exact_set, basename_map)

        if resolved:
            if resolved != target:
                renamed_count += 1
                line = replace_blob_target(line, target, resolved)
                target = resolved

            # Deduplicate if renaming produced a duplicate already present
            if target in seen_blobs:
                continue
            seen_blobs.add(target)
            out_lines.append(line)
        else:
            if remove:
                removed_count += 1
            else:
                commented_count += 1
                out_lines.append(f"{TAG} {line}")

    with open(prop_path, "w", encoding="utf-8") as f:
        f.writelines(out_lines)

    print(f"proprietary-files.txt: {renamed_count} renamed, {removed_count} removed, {commented_count} commented out.")


def process_extract_files_py(py_path: str, exact_set: set, basename_map: dict, remove: bool):
    with open(py_path, "r", encoding="utf-8") as f:
        content = f.read()

    lines = content.splitlines(keepends=True)
    out_lines = []
    commented_count = 0
    removed_count = 0
    renamed_count = 0

    path_pattern = re.compile(r"""^(\s*)(['"])([^'"]+)\2(\s*:\s*blob_fixup\(\).*)""")
    tuple_item_pattern = re.compile(r"""^(\s*)(['"])([^'"]+)\2(\s*,?\s*)$""")

    in_blob_fixups = False

    for line in lines:
        stripped = line.strip()

        if "blob_fixups:" in line or "blob_fixups =" in line:
            in_blob_fixups = True

        if remove and stripped.startswith(TAG):
            removed_count += 1
            continue

        if in_blob_fixups:
            m_single = path_pattern.match(line)
            if m_single:
                path = m_single.group(3)
                resolved = resolve_truth_path(path, exact_set, basename_map)
                if resolved:
                    if resolved != path:
                        renamed_count += 1
                        out_lines.append(f"{m_single.group(1)}{m_single.group(2)}{resolved}{m_single.group(2)}{m_single.group(4)}\n")
                    else:
                        out_lines.append(line)
                else:
                    if remove:
                        removed_count += 1
                    else:
                        commented_count += 1
                        out_lines.append(f"{m_single.group(1)}{TAG} {m_single.group(2)}{path}{m_single.group(2)}{m_single.group(4)}\n")
                continue

            m_tuple = tuple_item_pattern.match(line)
            if m_tuple and ("/" in m_tuple.group(3) or m_tuple.group(3).endswith(".so")):
                path = m_tuple.group(3)
                resolved = resolve_truth_path(path, exact_set, basename_map)
                if resolved:
                    if resolved != path:
                        renamed_count += 1
                        out_lines.append(f"{m_tuple.group(1)}{m_tuple.group(2)}{resolved}{m_tuple.group(2)}{m_tuple.group(4)}\n")
                    else:
                        out_lines.append(line)
                else:
                    if remove:
                        removed_count += 1
                    else:
                        commented_count += 1
                        out_lines.append(f"{m_tuple.group(1)}{TAG} {m_tuple.group(2)}{path}{m_tuple.group(2)}{m_tuple.group(4)}\n")
                continue

        out_lines.append(line)

    with open(py_path, "w", encoding="utf-8") as f:
        f.writelines(out_lines)

    print(f"extract-files.py: {renamed_count} renamed, {removed_count} removed, {commented_count} commented out.")


def main():
    parser = argparse.ArgumentParser(description="Audit and filter blobs against x6812.txt truth.")
    parser.add_argument("--truth", default="x6812.txt", help="Path to source of truth file")
    parser.add_argument("--prop", default="proprietary-files.txt", help="Path to proprietary-files.txt")
    parser.add_argument("--extract", default="extract-files.py", help="Path to extract-files.py")
    parser.add_argument("--remove", action="store_true", help="Remove all commented out or missing lines")
    args = parser.parse_args()

    if not os.path.exists(args.truth):
        sys.exit(f"Truth file not found: {args.truth}")

    exact_set, basename_map = load_truth(args.truth)

    if os.path.exists(args.prop):
        process_proprietary_files(args.prop, exact_set, basename_map, args.remove)
    else:
        print(f"File not found: {args.prop}")

    if os.path.exists(args.extract):
        process_extract_files_py(args.extract, exact_set, basename_map, args.remove)
    else:
        print(f"File not found: {args.extract}")


if __name__ == "__main__":
    main()
