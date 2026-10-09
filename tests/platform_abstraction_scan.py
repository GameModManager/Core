#!/usr/bin/env python3
"""Cross-platform abstraction ratchet for src/ outside src/platform/.

The rule: all OS-specific code goes through src/platform/. Three constructs
break it and all three are mechanically checkable:

  1. a direct OS header include (<windows.h>, <unistd.h>, <sys/...>, ...)
  2. an OS preprocessor branch (#ifdef _WIN32, #ifdef Q_OS_WIN, __APPLE__, ...)
  3. an OS path literal (%LOCALAPPDATA%, C:\\..., /proc/..., ~/Library, ...)

There are ~230 pre-existing violations, so a zero-tolerance check would be red
on day one and get disabled. Instead this is a ratchet: findings are compared
against a checked-in baseline keyed on (file, construct) with a count, so an
unrelated edit elsewhere in src/ never moves it, and any NEW or EXTRA violation
fails. Removing violations also fails, which is the point: it forces the
baseline to be regenerated and keeps it honest.

Usage:
    platform_abstraction_scan.py --src <dir> --baseline <file> [--update]

Exit codes: 0 clean, 1 drift from the baseline, 2 usage/IO error.
"""

import argparse
import os
import re
import sys
from collections import Counter

# src/platform/ is exempt: the rule puts OS headers and OS path literals there.
# Paths are relative to --src, which is src/ itself, so this is "platform".
EXEMPT_DIR = "platform"
SOURCE_EXTENSIONS = (".c", ".cc", ".cpp", ".h", ".hpp")

# 1. Direct OS header includes. Matched case-insensitively on the last path
# component, so <Windows.h> and <windows.h> both land here.
OS_HEADERS = frozenset({
    # Windows
    "windows.h", "shlobj.h", "shellapi.h", "objbase.h", "winuser.h", "direct.h",
    "knownfolders.h", "io.h", "process.h", "winsock2.h", "ws2tcpip.h",
    "tlhelp32.h", "psapi.h", "fileapi.h", "pathcch.h", "combaseapi.h",
    "comdef.h", "atlbase.h", "dbghelp.h", "shlwapi.h",
    # POSIX / BSD
    "unistd.h", "pwd.h", "dlfcn.h", "spawn.h", "grp.h", "dirent.h", "syslog.h",
    "mman.h", "sched.h", "netdb.h", "fcntl.h", "poll.h", "termios.h",
    "utime.h", "execinfo.h", "ucontext.h",
    # macOS
    "mach-o/dyld.h", "carbon/carbon.h", "corefoundation/corefoundation.h",
    "applicationservices/applicationservices.h",
})

# <sys/...>, <linux/...>, <arpa/...>, <netinet/...>: any sys/ header is POSIX.
SYS_HEADER_RE = re.compile(r"include\s*<(sys|linux|arpa|netinet|asm|mach)/[^>]+>")

INCLUDE_RE = re.compile(r"^\s*#\s*include\s*<([^>]+)>")

# 2. OS preprocessor branches.
OS_MACRO_RE = re.compile(
    r"_WIN32|_WIN64|WIN32_LEAN|__linux__|__APPLE__|__unix__|__MACH__|__FreeBSD__|Q_OS_")
COND_RE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b")

# 3. OS path literals. Deliberately a fixed list, not a heuristic: a false
# positive here makes the whole guard untrustworthy.
PATH_LITERALS = (
    "%LOCALAPPDATA%", "%APPDATA%", "%USERPROFILE%", "%TEMP%", "%ProgramFiles%",
    "%PROGRAMDATA%", "%SystemRoot%",
    "HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER", "HKEY_CLASSES_ROOT",
    "/Applications", "~/Library", "AppData/Local", "AppData\\\\Roaming",
    "/proc/", "/sys/fs/cgroup", "/dev/shm", "/etc/machine-id",
    "/var/lib/dbus", "/usr/bin/", "/usr/local/bin/", "/opt/wine",
    ".local/share/Steam", ".steam/steam", ".steam/debian-installation",
)
PATH_LITERAL_RE = re.compile("|".join(re.escape(p) for p in PATH_LITERALS))

# A Windows drive-letter path, e.g. C:\ or "C:\\Program Files".
DRIVE_PATH_RE = re.compile(r"[A-Za-z]:\\\\")


def strip_comment(line):
    """Blank out a // comment so prose about an OS is not counted as code."""
    idx = line.find("//")
    if idx < 0:
        return line
    # Do not eat a '//' that is inside a string literal on this line. Good
    # enough: these files have no line-initial 'http://'-style literals, and
    # an over-strip only ever under-reports, which the ratchet will catch.
    return line[:idx]


def scan_file(path, rel):
    with open(path, "r", errors="replace") as fh:
        lines = fh.readlines()

    found = []
    for i, raw in enumerate(lines, start=1):
        line = raw if INCLUDE_RE.match(raw) else strip_comment(raw)

        m = INCLUDE_RE.match(line)
        if m:
            header = m.group(1)
            base = header.split("/")[-1].lower()
            if base in OS_HEADERS or SYS_HEADER_RE.search(line):
                found.append(("include:" + header.lower(), i))
                continue

        if COND_RE.match(line) and OS_MACRO_RE.search(line):
            found.append(("osbranch", i))
            continue

        for literal in PATH_LITERALS:
            if literal in line:
                found.append(("pathlit:" + literal, i))
                break
        else:
            if DRIVE_PATH_RE.search(line):
                found.append(("pathlit:drive-letter", i))
    return found


def walk_sources(src_dir):
    for dirpath, dirnames, filenames in os.walk(src_dir):
        dirnames[:] = sorted(d for d in dirnames if d not in ("build", ".git"))
        for name in sorted(filenames):
            if name.endswith(SOURCE_EXTENSIONS):
                yield os.path.join(dirpath, name)


def collect(src_dir):
    """Return a Counter of (relative file, construct) -> count."""
    counts = Counter()
    src_norm = os.path.normpath(src_dir)
    for path in walk_sources(src_norm):
        rel = os.path.relpath(path, src_norm)
        if os.path.normpath(rel).startswith(EXEMPT_DIR):
            continue
        for construct, _line in scan_file(path, rel):
            counts[(rel.replace(os.sep, "/"), construct)] += 1
    return counts


def format_counts(counts):
    return "".join(
        "%s\t%s\t%d\n" % (rel, construct, n)
        for (rel, construct), n in sorted(counts.items())
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", required=True, help="root of the source tree")
    parser.add_argument("--baseline", required=True,
                        help="checked-in baseline of known violations")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline from the current tree")
    args = parser.parse_args()

    if not os.path.isdir(args.src):
        sys.stderr.write("not a directory: %s\n" % args.src)
        return 2

    current = collect(args.src)
    actual = format_counts(current)

    if args.update:
        with open(args.baseline, "w") as fh:
            fh.write(actual)
        print("baseline rewritten: %d distinct findings, %d occurrences"
              % (len(current), sum(current.values())))
        return 0

    try:
        with open(args.baseline) as fh:
            expected = fh.read()
    except OSError as exc:
        sys.stderr.write("cannot read baseline: %s\n" % exc)
        return 2

    if actual == expected:
        print("platform abstraction: %d known violations, unchanged"
              % sum(current.values()))
        return 0

    expected_counts = Counter()
    for line in expected.splitlines():
        if not line.strip():
            continue
        rel, construct, n = line.rsplit("\t", 2)
        expected_counts[(rel, construct)] = int(n)

    sys.stderr.write("platform abstraction ratchet: drift from baseline\n\n")
    for key in sorted(set(expected_counts) | set(current)):
        before = expected_counts.get(key, 0)
        after = current.get(key, 0)
        if before == after:
            continue
        if after == 0:
            verb = "resolved"
        elif before == 0:
            verb = "NEW VIOLATION"
        else:
            verb = "extra occurrences"
        sys.stderr.write("  %-16s %s: %d -> %d\n"
                         % (verb, key[1], before, after))
    sys.stderr.write(
        "\nEvery construct above is OS-specific code outside src/platform/.\n"
        "Route it through the adaptor in src/platform/, or - if you removed\n"
        "some - regenerate the baseline:\n"
        "  %s --src %s --baseline %s --update\n"
        % (sys.argv[0], args.src, args.baseline))
    return 1


if __name__ == "__main__":
    sys.exit(main())