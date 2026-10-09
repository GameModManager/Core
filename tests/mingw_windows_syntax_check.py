#!/usr/bin/env python3
"""Syntax-check every Windows platform source with the mingw cross-compiler.

We ship a Windows build and compile it nowhere: CMake picks
GMM_PLATFORM_WINDOWS from the host, so nothing in this repo or CI ever
compiles windows_platform.cpp. A Windows-only compile error therefore ships
silently - which is how the null-Platform macOS arm reached main
(Workspace-kgvn).

x86_64-w64-mingw32-g++ reads the real Windows headers from
/usr/x86_64-w64-mingw32/include, so -fsyntax-only against them catches every
error that a real Windows build would catch short of linking. It does not
catch link errors and it is not MSVC, so it is a floor, not a substitute for
a Windows CI runner.

Deliberately NOT passed: -DWIN32_LEAN_AND_MEAN. The project does not define
it, and windows_file_type_dispatch.cpp relies on windows.h pulling in
ShellExecuteW transitively; with the flag the check fails for a reason that is
an artefact of the harness rather than a defect in the code.

Exits 0 when every file checks clean (or the compiler is absent, which is a
skip), 1 on a compile error, 2 on a usage error.
"""

import argparse
import os
import shutil
import subprocess
import sys

# Every source under this directory is compiled by CMake for the Windows
# build, so every source is checked here. Discovered rather than listed: a new
# file that nobody adds to this list would silently go unchecked.
SCAN_DIR = os.path.join("src", "platform", "windows")
SOURCE_EXTENSIONS = (".c", ".cpp")

BASE_FLAGS = [
    "-fsyntax-only",
    "-std=c++20",
    "-D_WIN32",
    "-DGMM_PLATFORM_WINDOWS",
    "-Isrc",
]

# The mingw toolchain defaults to a MinGW-w64 target whose headers are in the
# multi-toolchain sysroot, so the compiler finds them on its own. Adding an
# explicit -I would risk picking up the host's headers instead.


def sources(root):
    out = []
    scan = os.path.join(root, SCAN_DIR)
    for name in sorted(os.listdir(scan)):
        if name.endswith(SOURCE_EXTENSIONS):
            out.append(os.path.join(SCAN_DIR, name))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument(
        "--compiler",
        default="x86_64-w64-mingw32-g++",
        help="mingw-w64 cross compiler",
    )
    parser.add_argument(
        "--skip-if-missing",
        action="store_true",
        help="exit 0 with a SKIP notice when the compiler is absent (for ctest)",
    )
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(os.path.join(root, SCAN_DIR)):
        sys.stderr.write("not a directory: %s\n" % os.path.join(root, SCAN_DIR))
        return 2

    if shutil.which(args.compiler) is None:
        notice = "mingw cross compiler not found: %s" % args.compiler
        if args.skip_if_missing:
            # A machine without mingw is not a failure; it is a machine that
            # cannot run this check. Same convention as the rest of the suite.
            print("SKIP: %s" % notice)
            return 0
        sys.stderr.write(notice + "\n")
        return 2

    files = sources(root)
    if not files:
        sys.stderr.write("no sources found under %s\n" % SCAN_DIR)
        return 2

    failed = []
    for rel in files:
        # cwd=root because -Isrc is relative, and ctest runs a registered test
        # from the build directory, not the source root.
        proc = subprocess.run(
            [args.compiler] + BASE_FLAGS + [rel],
            cwd=root,
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            failed.append((rel, proc.stderr))
            print("FAIL %s" % rel)
        else:
            print("ok   %s" % rel)

    if failed:
        sys.stderr.write("\nmingw syntax check failed:\n")
        for rel, err in failed:
            sys.stderr.write("\n--- %s ---\n%s" % (rel, err))
        return 1

    print("mingw syntax check: %d Windows platform sources clean" % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())