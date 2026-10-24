#!/usr/bin/env python3
"""Structural guard: the C++ SDK veneer must not leak into Core.

Spec 2.4: Core compiles against the C header set (external/abi/include) only.
The SDK (external/abi/sdk/include) is plugin-side. If any Core source or
CMake file references the SDK, the veneer has leaked.

Checks:
  1. No Core source file includes an SDK header (gmm/plugin.h, gmm/world.h,
     gmm/game.h, gmm/capability.h, gmm/cap/*, gmm/detail/thunk.h, or any
     path containing sdk/include).
  2. No Core CMakeLists.txt references sdk/include.

Exit 0: clean. Exit 1: violations.
"""
import argparse
import os
import re
import sys

SDK_INCLUDE_PATTERNS = [
    re.compile(r"sdk/include"),
    re.compile(r"gmm/plugin\.h"),
    re.compile(r"gmm/world\.h"),
    re.compile(r"gmm/game\.h"),
    re.compile(r"gmm/capability\.h"),
    re.compile(r"gmm/cap/"),
    re.compile(r"gmm/detail/thunk\.h"),
    re.compile(r"gmm/basic_game\.h"),
]

def walk(src_dir):
    for dirpath, dirnames, filenames in os.walk(src_dir):
        dirnames[:] = sorted(d for d in dirnames if d not in ("build", ".git"))
        for name in sorted(filenames):
            if name.endswith((".c", ".cc", ".cpp", ".h", ".hpp", ".cmake")) or name == "CMakeLists.txt":
                yield os.path.join(dirpath, name)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--src", required=True, help="root of Core source tree")
    parser.add_argument("--cmake-root", default=None, help="Core root for CMakeLists scan")
    args = parser.parse_args()

    problems = []
    root = args.src
    for path in walk(root):
        rel = os.path.relpath(path, root)
        try:
            with open(path, "r", errors="replace") as fh:
                text = fh.read()
        except OSError:
            continue
        for line in text.splitlines():
            low = line.strip()
            if "sdk/include" in low or "sdk\\include" in low:
                problems.append("%s: references sdk/include (%s)" % (rel, low))
                break
            if low.startswith("#include"):
                for pat in SDK_INCLUDE_PATTERNS:
                    if pat.search(low) and "sdk/include" not in low:
                        # exclude the C header gmm_abi_v3.h itself
                        if "gmm_abi_v3.h" in low:
                            continue
                        problems.append("%s: includes SDK header (%s)" % (rel, low))
                        break
                else:
                    continue
                break
    # Actually, to avoid duplicate spam, dedup messages.
    seen = set()
    uniq = []
    for p in problems:
        if p not in seen:
            seen.add(p)
            uniq.append(p)
    problems = uniq

    # Scan CMakeLists for sdk/include
    if args.cmake_root:
        cmake_root = args.cmake_root
    else:
        cmake_root = os.path.dirname(os.path.normpath(root))
    # root is src; cmake root is parent of src
    if os.path.basename(os.path.normpath(root)) == "src":
        parent = os.path.dirname(os.path.normpath(root))
        if parent:
            cmake_root = parent
        else:
            cmake_root = "."
    for dirpath, dirnames, filenames in os.walk(cmake_root):
        dirnames[:] = sorted(d for d in dirnames if d not in ("build", ".git", "src"))
        for name in filenames:
            if name == "CMakeLists.txt" or name.endswith(".cmake"):
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, cmake_root)
                try:
                    with open(path, "r", errors="replace") as fh:
                        text = fh.read()
                except OSError:
                    continue
                if "sdk/include" in text or "sdk\\include" in text:
                    problems.append("%s: references sdk/include in CMake" % rel)

    if problems:
        sys.stderr.write("abi boundary: %d violation(s)\n\n" % len(problems))
        for p in problems:
            sys.stderr.write("  %s\n" % p)
        return 1
    print("abi boundary: OK - no SDK veneer leak into Core")
    return 0

if __name__ == "__main__":
    sys.exit(main())
