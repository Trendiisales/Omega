#!/usr/bin/env python3
"""
G4 lint -- check_no_globals_in_hotpath.py
==========================================

Scans the two files that together form the Omega sweep harness hot path:

    backtest/OmegaTimeShim.hpp
    include/SweepableEngines.hpp

...for namespace-scope mutable variable declarations that are not in the
allow-list. Prints file:line for each offence and exits with code 1 on any
violation, or code 0 if the files are clean.

This is the mechanical companion to backtest/CONCURRENCY.md (section 5).
The allow-list mirrors CONCURRENCY.md section 2c exactly. Adding a new
allow-list entry without a corresponding update to CONCURRENCY.md is a
process violation, not a lint violation; the lint cannot detect that.

USAGE
    cd <repo root>
    python3 scripts/check_no_globals_in_hotpath.py

EXIT
    0  clean
    1  at least one disallowed global found
    2  configuration error (file missing, etc.)

DESIGN
    This is a text scanner, not a C++ parser. It strips block comments,
    line comments, and string literals, then walks brace depth tracking
    namespace context. At namespace scope (no enclosing class/struct/
    union/enum), declarations that match a typed-variable pattern are
    classified:

        thread_local      -> safe (per-thread storage)
        constexpr         -> safe (compile-time constant)
        const / static const  -> safe (immutable)
        std::atomic<T>    -> safe (explicit memory ordering)
        in ALLOWED_GLOBALS by name -> safe (explicitly listed)
        otherwise         -> FAIL

    The scanner deliberately does NOT try to handle:
      - macro-defined types
      - typedef'd struct names as variable types (rare in this code)
      - templates with `<>` containing `;` or `,` -- handled approximately
        by skipping anything inside `<>` brackets
      - adversarial bypasses (aliases, pointer-launder, etc.)

    These limitations are acceptable for the regression-prevention use case.

ALLOW-LIST DRIFT
    The allow-list below MUST be kept in sync with the table in
    backtest/CONCURRENCY.md section 2c. The lint script does not parse
    CONCURRENCY.md; alignment is by-hand discipline. A docstring in this
    file and a "see also" comment at the allow-list site are the only
    cross-references.
"""

from __future__ import annotations

import os
import re
import sys


# -----------------------------------------------------------------------------
# Allow-list. Each entry is a (qualified_name, file_basename) pair. The lint
# matches by trailing identifier name AND the file the declaration appears
# in, which prevents an allow-list entry from accidentally whitelisting an
# unrelated variable of the same name in a different file.
#
# *** KEEP THIS IN SYNC WITH backtest/CONCURRENCY.md SECTION 2c ***
# -----------------------------------------------------------------------------
ALLOWED_GLOBALS: set[tuple[str, str]] = {
    # OmegaTimeShim.hpp -- per-thread simulated clock state. All three are
    # `inline thread_local`; the lint will see the thread_local qualifier and
    # let them through anyway, but listing them here is documentation: these
    # are the only namespace-scope mutables we expect to find in the file.
    ("g_sim_now_ms",   "OmegaTimeShim.hpp"),
    ("g_sim_start_ms", "OmegaTimeShim.hpp"),
    ("g_sim_started",  "OmegaTimeShim.hpp"),
}


# -----------------------------------------------------------------------------
# Files in scope. The lint refuses to run on any other file even if invoked
# with arguments; the scope is fixed and matches CONCURRENCY.md.
# -----------------------------------------------------------------------------
TARGET_FILES = [
    "backtest/OmegaTimeShim.hpp",
    "include/SweepableEngines.hpp",
]


# -----------------------------------------------------------------------------
# Tokenisation -- strip block comments, line comments, and string literals
# while preserving line numbers (replace with spaces of equal length).
# -----------------------------------------------------------------------------
def strip_block_comments(src: str) -> str:
    out = []
    i = 0
    n = len(src)
    while i < n:
        if i + 1 < n and src[i] == "/" and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            if j < 0:
                # Unterminated -- treat rest of file as comment.
                # Preserve newlines for accurate line numbers.
                for ch in src[i:]:
                    out.append("\n" if ch == "\n" else " ")
                i = n
            else:
                # Replace comment with spaces, preserving newlines.
                for ch in src[i:j + 2]:
                    out.append("\n" if ch == "\n" else " ")
                i = j + 2
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


def strip_line_comments(src: str) -> str:
    out = []
    for line in src.splitlines(keepends=True):
        # Find // outside of strings. Quick approximation: find the first //
        # that isn't preceded by an open quote on this line that hasn't
        # closed. We've already stripped block comments, so // is the only
        # remaining comment form.
        in_str = False
        cut = -1
        i = 0
        while i < len(line):
            ch = line[i]
            if ch == '"' and (i == 0 or line[i - 1] != "\\"):
                in_str = not in_str
            elif (
                not in_str
                and ch == "/"
                and i + 1 < len(line)
                and line[i + 1] == "/"
            ):
                cut = i
                break
            i += 1
        if cut >= 0:
            # Replace comment portion with spaces, preserve newline.
            tail = line[cut:]
            spaces = "".join(" " if c != "\n" else "\n" for c in tail)
            out.append(line[:cut] + spaces)
        else:
            out.append(line)
    return "".join(out)


def strip_strings(src: str) -> str:
    # Replace contents of "..." with equal-length spaces, preserving the
    # quotes so other parsers don't mistake the body for code.
    out = []
    i = 0
    n = len(src)
    in_str = False
    while i < n:
        ch = src[i]
        if ch == '"' and (i == 0 or src[i - 1] != "\\"):
            in_str = not in_str
            out.append(ch)
        elif in_str:
            out.append("\n" if ch == "\n" else " ")
        else:
            out.append(ch)
        i += 1
    return "".join(out)


# -----------------------------------------------------------------------------
# Brace + namespace tracking
# -----------------------------------------------------------------------------
# We track two stacks side-by-side as we walk the source character by
# character. The brace stack records, for each open `{`, what kind of scope
# it opened: "ns" (namespace), "cls" (class/struct/union), "fn" (function
# body or other), or "blk" (anonymous nested block). A declaration is at
# namespace scope iff every entry on the brace stack is "ns".
#
# We classify a `{` by looking back at the recent tokens before it. The
# rules are deliberately simple:
#   - `namespace foo {`  -> "ns"
#   - `namespace {`      -> "ns" (anonymous namespace)
#   - `class X ... {`    -> "cls"
#   - `struct X ... {`   -> "cls"
#   - `union X ... {`    -> "cls"
#   - `enum [class] ... {` -> "cls" (won't have decls anyway)
#   - everything else    -> "fn" (function body, control flow, init list)


def classify_brace(prefix: str) -> str:
    # `prefix` is the source up to (and including) the `{` we just saw.
    # Strip the trailing `{` and look at what immediately precedes it.
    #
    # The classification only needs to distinguish "ns" (namespace), "cls"
    # (class/struct/union/enum), and everything else ("fn"). Implementation:
    # walk backwards from the brace through whitespace and identifiers
    # collecting characters; stop on a syntactic break. Then look for the
    # *most recent* (rightmost) keyword in the collected tokens -- that's
    # the keyword that introduces the new scope. We can't use tokens[0]
    # because the strip_strings pre-pass replaces string contents with
    # spaces but leaves the surrounding `"` quotes, which can appear as
    # leading tokens when the brace follows an `#include "..."` block.
    tail = prefix[:-1].rstrip()
    if not tail:
        return "fn"

    i = len(tail) - 1
    collected = []
    paren_depth = 0
    angle_depth = 0
    while i >= 0:
        ch = tail[i]
        if ch == ")":
            paren_depth += 1
            collected.append(ch)
            i -= 1
            continue
        if ch == "(":
            if paren_depth > 0:
                paren_depth -= 1
                collected.append(ch)
                i -= 1
                continue
            else:
                # Unmatched `(` -- this `{` follows a function-style construct.
                break
        if ch == ">":
            angle_depth += 1
            collected.append(ch)
            i -= 1
            continue
        if ch == "<":
            if angle_depth > 0:
                angle_depth -= 1
                collected.append(ch)
                i -= 1
                continue
            else:
                break
        if ch in (";", "}", "{"):
            break
        if ch == "#":
            # Preprocessor line; treat as a hard syntactic break.
            break
        collected.append(ch)
        i -= 1
    decl = "".join(reversed(collected)).strip()
    if not decl:
        return "fn"

    # Look at the rightmost keyword in `decl`. That keyword introduces the
    # scope opened by the `{`. We can't use the leading token because string
    # fragments and other noise can appear before the real keyword.
    # Use word-boundary regex; iterate matches and pick the last.
    keyword_re = re.compile(r"\b(namespace|class|struct|union|enum)\b")
    matches = list(keyword_re.finditer(decl))
    if not matches:
        return "fn"
    last_kw = matches[-1].group(1)
    if last_kw == "namespace":
        return "ns"
    # class/struct/union/enum all map to class-like scope.
    return "cls"


# -----------------------------------------------------------------------------
# Declaration matching
# -----------------------------------------------------------------------------
# A "candidate declaration" is a line at namespace scope that looks like:
#
#   [inline] [static] [thread_local] [constexpr] [const]
#       <typename> <ident> [= <expr>] ;
#
# where <typename> is one of a small whitelisted set of fundamental and
# common library scalar types. We deliberately don't try to handle every
# possible type spelling; the goal is to catch the bug-class.

TYPE_PATTERN = (
    r"(?:std::)?(?:int(?:8|16|32|64)?_t|uint(?:8|16|32|64)?_t|"
    r"int|long|short|size_t|ssize_t|time_t|"
    r"double|float|bool|char)"
)

# Atomics are detected separately and treated as safe.
ATOMIC_PATTERN = r"std::atomic\s*<[^>]*>"

DECL_RE = re.compile(
    r"^\s*"
    r"(?P<quals>(?:inline\s+|static\s+|thread_local\s+|constexpr\s+|const\s+){0,5})"
    r"(?P<type>" + ATOMIC_PATTERN + r"|" + TYPE_PATTERN + r")"
    r"\s+(?P<name>[A-Za-z_]\w*)"
    r"\s*(?:=[^;]*)?;\s*$"
)


def is_safe(quals: str, type_str: str, name: str, file_basename: str) -> bool:
    if "thread_local" in quals:
        return True
    if "constexpr" in quals:
        return True
    if "const " in (quals + " ") and "thread_local" not in quals:
        # `const` (or `static const`) is safe -- read-only.
        return True
    if "std::atomic" in type_str:
        return True
    if (name, file_basename) in ALLOWED_GLOBALS:
        return True
    return False


# -----------------------------------------------------------------------------
# Per-file scan
# -----------------------------------------------------------------------------
def scan_file(path: str) -> list[str]:
    """Return a list of human-readable offence strings; empty if clean."""
    if not os.path.isfile(path):
        return [f"{path}: file not found (lint configuration error)"]

    with open(path, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()

    src = strip_block_comments(src)
    src = strip_line_comments(src)
    src = strip_strings(src)

    file_basename = os.path.basename(path)

    offences: list[str] = []

    # First pass: walk character by character, tracking brace stack.
    # Record (line_number, kind) for each `{` encountered. We also need
    # a quick "is the scope at the start of line N pure namespace?" lookup.
    #
    # Rather than re-walking per declaration, we annotate each line with
    # the brace stack that's active at the *start* of that line.

    line_no = 1
    stack: list[str] = []
    line_start_stacks: list[list[str]] = [list(stack)]  # index = line_no - 1

    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        if ch == "\n":
            line_no += 1
            line_start_stacks.append(list(stack))
            i += 1
            continue
        if ch == "{":
            kind = classify_brace(src[: i + 1])
            stack.append(kind)
            i += 1
            continue
        if ch == "}":
            if stack:
                stack.pop()
            i += 1
            continue
        i += 1

    # Pad in case the file doesn't end with a newline.
    while len(line_start_stacks) < line_no + 1:
        line_start_stacks.append(list(stack))

    # Second pass: per-line declaration check.
    lines = src.splitlines()
    for ln_idx, line in enumerate(lines):
        ln = ln_idx + 1
        stk = line_start_stacks[ln_idx] if ln_idx < len(line_start_stacks) else []
        # A declaration is at namespace scope iff every entry is "ns".
        if any(s != "ns" for s in stk):
            continue
        m = DECL_RE.match(line)
        if not m:
            continue
        quals = m.group("quals") or ""
        type_str = m.group("type")
        name = m.group("name")
        if is_safe(quals, type_str, name, file_basename):
            continue
        offences.append(
            f"{path}:{ln}: disallowed namespace-scope mutable global "
            f"`{name}` of type `{type_str}` "
            f"(quals=`{quals.strip() or '(none)'}`).\n"
            f"    line: {line.strip()}\n"
            f"    fix : add `thread_local` (preferred), make it `constexpr`/"
            f"`const`/`std::atomic<>`, or add ({name!r}, "
            f"{file_basename!r}) to the\n"
            f"          ALLOWED_GLOBALS set in this script AND a row to "
            f"backtest/CONCURRENCY.md section 2c\n"
            f"          in the same commit."
        )

    return offences


# -----------------------------------------------------------------------------
# Entry point
# -----------------------------------------------------------------------------
def main() -> int:
    # Refuse to run with arguments -- scope is fixed.
    if len(sys.argv) > 1:
        print(
            "error: this lint takes no arguments. Scope is fixed to "
            + ", ".join(TARGET_FILES),
            file=sys.stderr,
        )
        return 2

    # Locate repo root by walking up from this script's directory until we
    # find a `.git/` or fall back to the cwd.
    here = os.path.abspath(os.path.dirname(__file__))
    root = here
    for _ in range(5):
        if os.path.isdir(os.path.join(root, ".git")):
            break
        parent = os.path.dirname(root)
        if parent == root:
            break
        root = parent
    else:
        root = os.getcwd()

    # Confirm target files exist relative to root.
    full_paths = [os.path.join(root, p) for p in TARGET_FILES]
    missing = [p for p in full_paths if not os.path.isfile(p)]
    if missing:
        print(
            "error: G4 lint cannot find the target files. Expected paths:",
            file=sys.stderr,
        )
        for p in missing:
            print(f"    {p}", file=sys.stderr)
        print(
            "Run from the Omega repo root, or fix TARGET_FILES at the top "
            "of the script.",
            file=sys.stderr,
        )
        return 2

    all_offences: list[str] = []
    for p in full_paths:
        rel = os.path.relpath(p, root)
        print(f"scanning {rel} ...")
        offs = scan_file(p)
        if offs:
            all_offences.extend(offs)
        else:
            print(f"  clean ({rel})")

    if all_offences:
        print()
        print(f"FAIL: {len(all_offences)} disallowed global(s) found:")
        print("-" * 78)
        for o in all_offences:
            print(o)
            print("-" * 78)
        print()
        print(
            "See backtest/CONCURRENCY.md for the rules. The runtime "
            "G2 self-test in OmegaSweepHarness will also catch any "
            "non-determinism this lint missed."
        )
        return 1

    print()
    print(
        f"OK: {len(TARGET_FILES)} file(s) clean. No disallowed namespace-"
        f"scope mutable globals found in the sweep hot path."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
