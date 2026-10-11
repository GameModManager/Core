#!/usr/bin/env bash
# URI/source fence gate.
#
# engine/uri/** owns the scheme:// grammar: what the user clicked, and which
# instance owns it. engine/source/** owns provenance: where bytes come from,
# who the account is, what the API says. A URI handler must never reach into a
# concrete provider, and a provider must never reach into the grammar.
#
# This is a review gate, not a compile error: it catches a future regression
# where someone adds the crossed include back. Comments that merely name the
# other side are ignored.
#
# ALLOWED, for every file under engine/uri/
#   engine/source/interface.h
#       The declared interface. Nothing in engine/uri/ implements it today;
#       it is permitted so a future transport can, without the gate becoming
#       the thing that has to be edited first.
#   engine/source/download/manager.h
#       For exactly one symbol: percent_decode, the shared RFC 3986 unescape
#       that modl:// query strings need. That primitive genuinely lives in the
#       transfer layer today and relocating it is a separate change. The
#       header is wide, so the symbols a URI handler must NOT use out of it
#       are barred by name below - the grammar layer cannot start downloading.
#
# ALLOWED, additionally, for a DECLARED TRANSPORT only
#   engine/uri/modl/provider.cpp -> engine/source/download/curl_download.h,
#                                  engine/network/network_manager.h,
#                                  engine::download::{curl_download, Options,
#                                  Progress, url_path_basename}
#       A modl:// handler has to put bytes on disk, exactly as a Source::Nexus
#       provider does; it is a transport, not a grammar layer. The permission
#       is per-file, never per-tree: move one more file into the transport
#       table and the grammar layer starts downloading.
#
# FORBIDDEN, everywhere under engine/uri/, transports included
#   engine/source/{nexus,steam,loverslab,modpub,git,update}/**
#   engine::Source::{Nexus,Steam,LoversLab,ModPub}::Provider
#   That is the fence. A URI handler never reads an API key, an account or a
#   session; it parses what the user clicked and hands it to the provenance
#   side.
#
# FORBIDDEN, for every file under engine/source/
#   engine/uri/**
#
# Usage:
#   scripts/check_uri_fence.sh
#
# Works both from the Core repo root and from the Workspace hub
# (projects/Core).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ENGINE_DIR="$ROOT/src/engine"
if [ ! -d "$ENGINE_DIR" ]; then
    ENGINE_DIR="$ROOT/projects/Core/src/engine"
fi
if [ ! -d "$ENGINE_DIR" ]; then
    echo "URI fence: engine dir not found (looked in" >&2
    echo "  $ROOT/src/engine and $ROOT/projects/Core/src/engine)" >&2
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "URI fence: python3 is required to run this gate" >&2
    exit 1
fi

python3 - "$ENGINE_DIR" <<'PY'
import sys, re, pathlib

engine_dir = pathlib.Path(sys.argv[1])

# Directories / file-name patterns that are test code and exempt from the gate.
test_markers = ("test", "tests", "_test")

# Source-side headers engine/uri/ may include, each with its justification.
ALLOWED_URI_INCLUDES = {
    "engine/source/interface.h",        # the declared interface
    "engine/source/download/manager.h",  # percent_decode only - see header
}

# engine/uri/ files that are declared transports: they have to move bytes, so
# they are allowed the shared transfer primitive. Keyed by engine-relative
# path, so the permission follows the file and not the directory.
TRANSPORT_FILES = {
    "uri/modl/provider.cpp",
}
TRANSPORT_INCLUDES = {
    "engine/source/download/curl_download.h",
    "engine/network/network_manager.h",
}

# Source-side subtree prefixes that must not be visible from engine/uri/.
BARRED_FROM_URI = (
    "engine/source/nexus/",
    "engine/source/steam/",
    "engine/source/loverslab/",
    "engine/source/modpub/",
    "engine/source/git/",
    "engine/source/update/",
)

# Naming a concrete provider is the fence, and it binds transports too.
BARRED_PROVIDER_SYMBOLS = (
    "Source::Nexus",
    "Source::Steam",
    "Source::LoversLab",
    "Source::Loverslab",
    "Source::ModPub",
    "Source::Modl",
    "Source::Direct",
    "Nexus::Provider",
    "Steam::Provider",
    "LoversLab::Provider",
    "ModPub::Provider",
)

# Reaching the transfer engine is barred everywhere EXCEPT a declared
# transport. The grammar layer reads a string; it does not fetch bytes.
BARRED_NON_TRANSPORT_SYMBOLS = (
    "download::curl_download",
    "download::Options",
    "download::Progress",
    "download::parse_content_disposition_filename",
    "download::url_path_basename",
)

include_re = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def is_test(path: pathlib.Path) -> bool:
    parts = set(p.lower() for p in path.parts)
    if parts & set(test_markers):
        return True
    name = path.name.lower()
    return "test" in name or name.endswith("_test.cpp") or name.endswith("_test.h")


def code_lines(text: str):
    """Yield (lineno, code) with comments and continued block comments stripped."""
    in_block = False
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw
        if in_block:
            end = line.find("*/")
            if end == -1:
                continue
            line = line[end + 2:]
            in_block = False
        start = line.find("/*")
        if start != -1:
            end = line.find("*/", start + 2)
            if end == -1:
                in_block = True
                line = line[:start]
            else:
                line = line[:start] + line[end + 2:]
        yield lineno, line.split("//", 1)[0]


violations = 0

for path in sorted(engine_dir.rglob("*")):
    if path.suffix not in (".cpp", ".h"):
        continue
    if is_test(path):
        continue
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        continue

    rel = path.relative_to(engine_dir).as_posix()
    in_uri = rel.startswith("uri/")
    in_source = rel.startswith("source/")

    if not (in_uri or in_source):
        continue

    is_transport = rel in TRANSPORT_FILES

    for lineno, line in code_lines(text):
        m = include_re.match(line)
        if m:
            inc = m.group(1)
            if in_uri:
                allowed = set(ALLOWED_URI_INCLUDES)
                if is_transport:
                    allowed |= TRANSPORT_INCLUDES
                if inc.startswith(BARRED_FROM_URI) \
                        or (inc.startswith("engine/source/")
                            and inc not in allowed):
                    print(f"VIOLATION: {rel}:{lineno}: engine/uri/ includes {inc!r}")
                    violations += 1
            elif in_source and inc.startswith("engine/uri/"):
                print(f"VIOLATION: {rel}:{lineno}: engine/source/ includes {inc!r}")
                violations += 1
            continue

        if in_uri:
            barred = BARRED_PROVIDER_SYMBOLS
            if not is_transport:
                barred = barred + BARRED_NON_TRANSPORT_SYMBOLS
            for sym in barred:
                if re.search(r"\b" + re.escape(sym) + r"\b", line):
                    print(f"VIOLATION: {rel}:{lineno}: engine/uri/ names {sym!r}")
                    violations += 1
                    break

if violations:
    print(f"\nURI fence FAILED: {violations} violation(s) between "
          f"engine/uri/ and engine/source/.")
    sys.exit(1)
print("URI fence OK: engine/uri/ and engine/source/ stay on their own side.")
PY