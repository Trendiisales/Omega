#!/usr/bin/env python3
"""
VERIFY_ACTIVE.py - Authoritative Include Graph Verifier
========================================================
Purpose:
  - Proves that src/active is closed under includes
  - Fails if any include resolves outside src/active
  - Fails if any file in src/active is unused
  - Becomes the single source of truth

Exit codes:
  0 - OK
  1 - No .cpp entry points found
  2 - Missing or illegal includes
  3 - Unused files in src/active
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
ACTIVE = os.path.join(ROOT, "src", "active")

include_re = re.compile(r'#include\s*"([^"]+)"')

def get_includes(path):
    out = []
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                m = include_re.match(line.strip())
                if m:
                    out.append(m.group(1))
    except:
        pass
    return out

def resolve_include(src, inc):
    base = os.path.dirname(src)
    p = os.path.normpath(os.path.join(base, inc))
    if os.path.exists(p):
        return p
    p = os.path.normpath(os.path.join(ACTIVE, inc))
    if os.path.exists(p):
        return p
    return None

def collect_active_files():
    files = []
    for root, _, names in os.walk(ACTIVE):
        for n in names:
            if n.endswith(".hpp") or n.endswith(".h") or n.endswith(".cpp"):
                files.append(os.path.join(root, n))
    return files

def main():
    entry_points = []
    for n in os.listdir(ACTIVE):
        if n.endswith(".cpp"):
            entry_points.append(os.path.join(ACTIVE, n))

    if not entry_points:
        print("ERROR: no .cpp files in src/active")
        sys.exit(1)

    seen = set()
    queue = list(entry_points)
    missing = []

    while queue:
        cur = queue.pop(0)
        if cur in seen:
            continue
        if not os.path.exists(cur):
            missing.append(cur)
            continue
        seen.add(cur)
        for inc in get_includes(cur):
            resolved = resolve_include(cur, inc)
            if not resolved:
                missing.append(f"{cur} -> {inc}")
            else:
                if not resolved.startswith(ACTIVE):
                    missing.append(f"{cur} -> {inc} (outside active)")
                if resolved not in seen:
                    queue.append(resolved)

    all_active = set(collect_active_files())
    unused = all_active - seen

    if missing:
        print("MISSING OR ILLEGAL INCLUDES")
        for m in sorted(set(missing)):
            print(f"  {m}")
        sys.exit(2)

    if unused:
        print("UNUSED FILES IN src/active")
        for u in sorted(unused):
            print(f"  {u}")
        sys.exit(3)

    print("=" * 50)
    print("VERIFY_ACTIVE OK")
    print("=" * 50)
    print(f"Entry points: {len(entry_points)}")
    print(f"Files used: {len(seen)}")
    print(f"Files in active: {len(all_active)}")
    print("=" * 50)

if __name__ == "__main__":
    main()
