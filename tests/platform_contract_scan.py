#!/usr/bin/env python3
"""Structural guard for the platform abstraction contract.

We ship Linux, Windows and macOS but compile only one of them per build, so the
two classes of defect that would ship broken on the two uncompiled OSes are
invisible to a normal build. Both are mechanically checkable from the source
text, which is what this does.

RULE 1 - pure virtual coverage.
    Every pure virtual on engine::Platform and engine::FileTypeDispatcher must
    be overridden by all three adaptors of the matching interface. A pure
    virtual with no override means that OS silently gets the interface default,
    and there is no build here that would notice. Each override must either have
    an inline body in the adaptor header or be defined in the adaptor's .cpp, so
    a declaration without either is caught too.

RULE 2 - GMM_PLATFORM selection chains.
    2a. A `#if defined(GMM_PLATFORM_X) ... #elif defined(GMM_PLATFORM_Y)`
        chain must terminate in a populated `#else`. A chain that ends at
        `#endif` gives the unhandled OS a silent do-nothing path - that is the
        exact shape of the null-Platform bug in app/core.cpp, where a chain
        covering linux + windows and forgetting macos left platform_ null.
    2b. A chain that names a concrete adaptor class (LinuxPlatform,
        WindowsPlatform, MacOSPlatform) must name all three OS tokens
        explicitly. Selecting an adaptor is never an "else" decision.

    A chain naming a single token (`#ifdef GMM_PLATFORM_LINUX` for a Linux-only
    kernel feature) is not a platform selection and is not flagged.

Both rules are pure text with no OS dependence, so they run on every build.

Usage:
    platform_contract_scan.py --src <dir> [--verbose]

Exit codes: 0 contract holds, 1 contract violated, 2 usage/IO error.
"""

import argparse
import os
import re
import sys

# interface header -> { adaptor class name: adaptor source stem }
IFACE_ADAPTORS = {
    "platform/platform.h": {
        "LinuxPlatform": "platform/linux/linux_platform",
        "MacOSPlatform": "platform/macos/macos_platform",
        "WindowsPlatform": "platform/windows/windows_platform",
    },
    "platform/file_type_dispatch.h": {
        "LinuxFileTypeDispatcher": "platform/linux/linux_file_type_dispatch",
        "MacOSFileTypeDispatcher": "platform/macos/macos_file_type_dispatch",
        "WindowsFileTypeDispatcher": "platform/windows/windows_file_type_dispatch",
    },
}

PLATFORM_TOKENS = frozenset({
    "GMM_PLATFORM_LINUX",
    "GMM_PLATFORM_WINDOWS",
    "GMM_PLATFORM_MACOS",
})

ADAPTOR_CLASS_RE = re.compile(r"\b(LinuxPlatform|MacOSPlatform|WindowsPlatform)\b")

BRANCH_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
TOKEN_RE = re.compile(r"\b(GMM_PLATFORM_(?:LINUX|WINDOWS|MACOS))\b")


def read(path):
    with open(path, "r", errors="replace") as fh:
        return fh.read()


def strip_comments(text):
    """Blank out // comments so prose about an OS is not parsed as code."""
    out = []
    for line in text.splitlines():
        idx = line.find("//")
        out.append("" if idx == 0 else (line if idx < 0 else line[:idx]))
    return "\n".join(out)


def _after_params(text, open_idx):
    """Index of the ")" balancing the "(" at `open_idx`, or -1."""
    depth, i = 0, open_idx
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def pure_virtuals(text):
    """Method names of every `= 0;` pure virtual in a header.

    Scanned rather than regexed: a declaration can wrap lines and its parameter
    list can contain `= {}` default arguments, so the parameter parens have to
    be balanced before `= 0;` can be recognised.
    """
    text = strip_comments(text)
    names = []
    for m in re.finditer(r"\bvirtual\b", text):
        # The method name is the identifier immediately before the FIRST "(".
        open_idx = text.find("(", m.end())
        if open_idx < 0:
            continue
        fname = re.findall(r"([A-Za-z_]\w*)\s*$", text[m.end():open_idx])
        if not fname:
            continue
        close_idx = _after_params(text, open_idx)
        if close_idx < 0:
            continue
        stop = text.find(";", close_idx)
        if stop < 0:
            continue
        if re.search(r"=\s*0\s*$", text[close_idx + 1:stop]):
            names.append(fname[-1])
    return sorted(set(names))


def overrides(text):
    """{method name: has an inline body} for every `override` declaration."""
    text = strip_comments(text)
    found = {}
    for m in re.finditer(r"([A-Za-z_]\w*)\s*\(", text):
        open_idx = m.end() - 1
        close_idx = _after_params(text, open_idx)
        if close_idx < 0:
            continue
        stop = text.find(";", close_idx)
        if stop < 0:
            continue
        tail = text[close_idx + 1:stop]
        if "override" in tail:
            found[m.group(1)] = "{" in tail
    return found


def check_rule1(src_dir, verbose):
    """Returns (problem messages, total pure virtuals parsed)."""
    problems = []
    total = 0
    for rel, adaptors in IFACE_ADAPTORS.items():
        iface_path = os.path.join(src_dir, rel)
        if not os.path.exists(iface_path):
            problems.append("%s: interface header is missing" % rel)
            continue
        methods = pure_virtuals(read(iface_path))
        total += len(methods)
        if verbose:
            print("  %s pure virtuals: %s" % (rel, ", ".join(methods)))
        for method in methods:
            for cls, stem in adaptors.items():
                header = os.path.join(src_dir, stem + ".h")
                source = os.path.join(src_dir, stem + ".cpp")
                if not os.path.exists(header) or not os.path.exists(source):
                    problems.append(
                        "%s: %s has no header/source pair (%s)"
                        % (rel, cls, stem))
                    continue
                declared = overrides(read(header))
                if method not in declared:
                    problems.append(
                        "%s: %s.%s is not overridden in %s.h - that OS silently "
                        "gets the interface default" % (rel, cls, method, stem))
                elif not declared[method] and not re.search(
                        r"\b%s::%s\s*\(" % (cls, method), read(source)):
                    problems.append(
                        "%s: %s.%s is declared override in %s.h but has no body "
                        "there and is never defined in %s.cpp"
                        % (rel, cls, method, stem, stem))
    if total == 0:
        problems.append(
            "no pure virtuals parsed from the interface headers - the guard "
            "itself is broken, not the code")
    return problems, total


def check_rule2(src_dir, verbose):
    """Returns (problem messages, number of multi-arm OS chains seen)."""
    problems = []
    chains = 0
    for path in walk(src_dir):
        rel = os.path.relpath(path, src_dir).replace(os.sep, "/")
        lines = read(path).splitlines()
        i = 0
        while i < len(lines):
            m = BRANCH_RE.match(lines[i])
            if not m or m.group(1) not in ("if", "ifdef", "ifndef"):
                i += 1
                continue
            start = i
            tokens, body, has_else = set(), [], False
            while i < len(lines):
                b = BRANCH_RE.match(lines[i])
                if not b:
                    body.append(lines[i])
                    i += 1
                    continue
                kw, rest = b.group(1), b.group(2)
                if kw == "else":
                    has_else = True
                elif kw == "endif":
                    if tokens:
                        chains += 1
                        _judge(problems, rel, start + 1, tokens, has_else, body)
                    break
                elif kw == "elif":
                    if not tokens:
                        break  # an #elif with no token-bearing #if starts a chain
                    tokens |= set(TOKEN_RE.findall(rest))
                else:
                    tokens |= set(TOKEN_RE.findall(rest))
                body.append(lines[i])
                i += 1
            else:
                if tokens:
                    chains += 1
                    _judge(problems, rel, start + 1, tokens, has_else, body)
            i += 1
    if verbose:
        print("  GMM_PLATFORM chains with 2+ OS arms: %d" % chains)
    return problems, chains


def _has_content(body):
    """Does the `#else` branch do something?

    A bare `#else` / `#endif` is not content. `#error` and `#warning` are -
    that is exactly the "no arm for this OS, fail loudly" spelling.
    """
    for line in body:
        stripped = line.strip()
        if not stripped or stripped in ("#", "#else", "#endif"):
            continue
        return True
    return False


def _judge(problems, rel, lineno, tokens, has_else, body):
    if len(tokens) < 2:
        return  # single-token guard, not a platform selection
    short = "/".join(sorted(t[len("GMM_PLATFORM_"):] for t in tokens))
    if not has_else or not _has_content(body):
        problems.append(
            "%s:%d: GMM_PLATFORM chain covers %s but ends without a populated "
            "#else - the unhandled OS takes a silent no-op path"
            % (rel, lineno, short))
    missing = PLATFORM_TOKENS - tokens
    if missing and ADAPTOR_CLASS_RE.search("\n".join(body)):
        problems.append(
            "%s:%d: chain selects an OS adaptor but names only %s - a concrete "
            "adaptor is never an else decision; add the missing arm(s): %s"
            % (rel, lineno, short,
               "/".join(sorted(t[len("GMM_PLATFORM_"):] for t in missing))))


def walk(src_dir):
    for dirpath, dirnames, filenames in os.walk(src_dir):
        dirnames[:] = sorted(d for d in dirnames if d not in ("build", ".git"))
        for name in sorted(filenames):
            if name.endswith((".c", ".cc", ".cpp", ".h", ".hpp")):
                yield os.path.join(dirpath, name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--src", required=True, help="root of the source tree")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if not os.path.isdir(args.src):
        sys.stderr.write("not a directory: %s\n" % args.src)
        return 2

    problems, total = check_rule1(args.src, args.verbose)
    chain_problems, chains = check_rule2(args.src, args.verbose)
    problems = problems + chain_problems

    if problems:
        sys.stderr.write("platform contract: %d violation(s)\n\n" % len(problems))
        for p in problems:
            sys.stderr.write("  %s\n" % p)
        sys.stderr.write(
            "\nA pure virtual an adaptor does not override means that OS\n"
            "silently inherits the interface default, and we only ever\n"
            "compile one of the three. A GMM_PLATFORM chain without an\n"
            "explicit arm means the unhandled OS runs a silent no-op.\n")
        return 1

    n_adaptors = sum(len(v) for v in IFACE_ADAPTORS.values())
    print("platform contract: %d pure virtuals across %d adaptors, %d "
          "multi-arm OS chains - all covered"
          % (total, n_adaptors, chains))
    return 0


if __name__ == "__main__":
    sys.exit(main())