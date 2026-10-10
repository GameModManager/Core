#!/usr/bin/env python3
"""Feature map guard: docs/featuremap.md must stay a checkable claim.

The map is a tracker, not a diary: every cell is a status token plus a pointer
a reader can resolve. This makes the GMM half of that mechanical, using nothing
but this repository. It fails when any of these is true:

  1. a GMM-side pointer resolves nowhere in this tree. A file pointer resolves
     if a file of that name exists; a symbol pointer resolves if the identifier
     appears in the tree.
  2. a pointer that used to be unresolvable now resolves. That is the fix
     landing, and it must drop out of the baseline so the baseline keeps
     meaning "still broken", not "everything that ever was broken".
  3. a summary cell disagrees with the rows in its own section, a section's
     dispositions do not sum to its row count, the TOTAL disagrees with the
     sections, or the parity line disagrees with the rows.
  4. no rows parsed at all. A guard that cannot see the file it guards must
     fail, never pass.

This is a docs tool, not a product test: it is deliberately not registered in
the ctest suite and is run by hand, by an agent or by the reviewer.

The MO2 column is NOT checked. It cites MO2's own sources, and MO2 is not a
dependency of this repo, so there is nothing here to resolve those pointers
against. Those rows are carries, not claims this script can settle. Everything
else - the feature column, the GMM column, the status column - is a claim about
this tree, and every one of them is checked.

Because a few broken pointers predate this guard, (1) and (2) are a ratchet
against a checked-in baseline keyed on (row, construct) - never on line
numbers, so unrelated edits elsewhere never move it. Fix a row and regenerate
the baseline; the regenerated file is the review.

Usage:
    scripts/check_featuremap.py [--doc <featuremap.md>] [--repo <Core root>]
        [--baseline <file>] [--update]

Defaults are resolved from this script's own location, so from the repository
root it needs no arguments at all.

Exit codes: 0 clean, 1 a check failed, 2 usage/IO error.
"""

import argparse
import os
import re
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Skip build trees and vendored submodules: they are not source of truth for a
# pointer, and a submodule checkout must not move this guard.
SKIP_DIRS = frozenset({
    ".git", "build", "build-tests", "build-verify", "resources", "__pycache__",
})

# Extensions whose contents are searched for a symbol pointer. Wide on purpose:
# a symbol pointer that resolves through an unexpected file type is a pointer
# that resolves.
TEXT_EXT = frozenset({
    ".c", ".cc", ".cpp", ".h", ".hpp", ".txt", ".cmake", ".py", ".md", ".json",
    ".qss", ".ui", ".xml", ".yml", ".yaml", ".pro", ".toml", ".conf", ".sh",
    ".bat", ".ps1", ".desktop", ".csv",
})

# A pointer is `name.ext` or `path/name.ext`, optionally `:12`, `:12-34`,
# `:12, :40`. The line part is deliberately not checked: a line number inside a
# file that exists is a claim about content, and falsifying that needs a reader,
# not a regex. The row's `✔` is that reader's verdict.
FILE_PTR_RE = re.compile(r"^([A-Za-z0-9_./-]+\.[A-Za-z0-9]+)(:[0-9,\s-]+)?$")

# A lowercase short suffix means a file; a mixed-case one means a dotted C++
# expression like `profile.getActiveMods`, which is a symbol and is checked as
# one. Without this the guard would report every dotted symbol as a missing file.
FILE_EXT_RE = re.compile(r"^[a-z][a-z0-9]{0,4}$")

# Backticked spans are the only pointers; prose around them is not a claim.
CODE_RE = re.compile(r"`([^`]+)`")

# Identifier runs long enough to be a real symbol rather than prose.
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]{3,}")

# A row id in the first cell: U129, S20, U272. One letter then digits, so a
# feature that merely starts with a capitalised acronym ("MD5 lookup") is not
# mistaken for an id. Rows without one are keyed on their section and their
# feature text, so the key survives a line move.
ROW_ID_RE = re.compile(r"^([A-Z]\d{2,4})\b")

SECTION_RE = re.compile(r"^##\s+(\d+)\.\s*(.+?)\s*$")
WIN_TAG = "[win]"

# Column 1 of a data row is MO2's; every other column makes a claim about this
# repo, so every other column is the one that gets resolved against it.
MO2_COLUMN = 1


def index_tree(root):
    """Return (set of file names, concatenation of text file contents).

    The contents stay bytes: the needles are ASCII identifiers, and decoding
    tens of MB of source only to compare ASCII would cost seconds.
    """
    names = set()
    chunks = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            names.add(name)
            if name.endswith(tuple(TEXT_EXT)):
                try:
                    with open(os.path.join(dirpath, name), "rb") as fh:
                        chunks.append(fh.read())
                except OSError:
                    pass
    return names, b"\n".join(chunks)


def split_row(line):
    """Split a markdown table row into stripped cells, or None if not one."""
    stripped = line.strip()
    if not stripped.startswith("|") or re.match(r"^\|[\s:|-]+\|$", stripped):
        return None
    return [c.strip() for c in stripped.strip("|").split("|")]


def parse_rows(text):
    """Return (rows, summary, total, markers, win_tag).

    rows: list of (key, section_number, status_cell, cells)
    summary: {section_number: (label, [counts])} plus the TOTAL counts
    markers: the disposition markers, taken from the summary table header so
             this script never hard-codes a symbol it could inherit instead.
    """
    rows = []
    summary = {}
    total = None
    markers = []
    win_tag = ""
    section = None

    for line in text.split("\n"):
        m = SECTION_RE.match(line)
        if m:
            section = m.group(1)
            continue
        cells = split_row(line)
        if not cells or not cells[0]:
            continue

        if len(cells) == 8:
            head = cells[0].lower()
            if head == "section":
                # The last header cell is the [win] tag, which is a cross-cutting
                # tag rather than a seventh disposition; markers are the rest.
                header = [c.strip("` ") for c in cells[1:]]
                markers = header[:-1]
                win_tag = header[-1] if header else ""
            elif head == "**total**":
                total = cells[1:]
            elif cells[0][0].isdigit():
                num = cells[0].split(".", 1)[0].strip()
                summary[num] = (cells[0], cells[1:])
            continue

        # Data rows are the four-cell tables: Feature | MO2 | GMM | Status.
        if len(cells) != 4 or cells[0].lower() == "feature":
            continue
        rid = ROW_ID_RE.match(cells[0])
        feat = re.sub(r"\s+", " ", re.sub(r"`", "", cells[0]))
        key = rid.group(1) if rid else "%s:%s" % (section or "?", feat[:70])
        rows.append((key, section, cells[3], cells))

    return rows, summary, total, markers, win_tag


def pointers_in(cell):
    """The backticked pointer tokens in one cell, whitespace-stripped."""
    return [tok.strip() for tok in CODE_RE.findall(cell) if tok.strip()]


def resolves(construct, names, blob):
    """True if the pointer names something that exists in this tree."""
    match = FILE_PTR_RE.match(construct)
    if match and FILE_EXT_RE.match(match.group(1).rsplit(".", 1)[1]):
        return os.path.basename(match.group(1)) in names
    # Symbol pointer: the dotted suffix of a symbol like `usvfs_x64.dll` is a
    # name, not a type, so drop it and look for the stem.
    ident = IDENT_RE.findall(re.sub(r"\.[A-Za-z0-9]+", " ", construct))
    if not ident:
        # Nothing checkable (a bare emoji, a `1`): not a pointer we can fail on.
        return True
    return any(sym.encode() in blob or sym in names for sym in ident)


def collect_findings(rows, core):
    """Unresolvable (row, construct) pairs over every GMM-side cell."""
    core_names, core_blob = core
    found = set()
    for key, _section, _status, cells in rows:
        for index, cell in enumerate(cells):
            if index == MO2_COLUMN:
                continue
            for tok in pointers_in(cell):
                if not resolves(tok, core_names, core_blob):
                    found.add((key, tok))
    return found


def recount(rows, markers):
    """Disposition counts per section, recounted from the rows themselves."""
    per_section = {}
    for _key, section, status, _cells in rows:
        counts = per_section.setdefault(section, Counter())
        for marker in markers:
            if status.startswith(marker):
                counts[marker] += 1
                break
        if WIN_TAG in status:
            counts[WIN_TAG] += 1
    return per_section


def check_summary(rows, summary, total, markers, formula, parity_claim):
    """Return a list of human-readable failures."""
    problems = []
    if not summary:
        return ["no summary table found"]
    per_section = recount(rows, markers)

    missing = sorted(set(per_section) - set(summary), key=lambda s: (s is None, s))
    extra = sorted(set(summary) - set(per_section), key=lambda s: (s is None, s))
    for num in missing:
        problems.append("section %s has rows but no summary row" % num)
    for num in extra:
        problems.append("summary row %s has no rows" % summary[num][0])

    grand = Counter()
    for num in sorted(summary):
        label, claims = summary[num]
        actual = per_section.get(num, Counter())
        for i, marker in enumerate(markers + [WIN_TAG]):
            want = claims[i].strip("*")
            got = str(actual.get(marker, 0))
            if want != got:
                problems.append("section %s %s: summary says %s, rows say %s"
                                % (label, marker, want, got))
            grand[marker] += int(want or 0)

        # A section's dispositions must account for every row it holds. `[win]`
        # is a tag rather than a disposition, so it is counted beside them, not
        # in the sum: a `[win]` row also carries one disposition marker.
        claimed = sum(int(claims[i].strip("*") or 0) for i in range(len(markers)))
        held = sum(1 for _k, s, _st, _c in rows if s == num)
        if claimed != held:
            problems.append("section %s: dispositions cover %d rows, section holds %d"
                            % (label, claimed, held))

    if total is not None:
        for i, marker in enumerate(markers + [WIN_TAG]):
            want = total[i].strip("*")
            got = str(grand.get(marker, 0))
            if want != got:
                problems.append("TOTAL %s: table says %s, sections sum to %s"
                                % (marker, want, got))

    # The parity formula is read from the doc, never from marker order: the
    # header puts the GMM-exclusive marker third, and scoring it would be the
    # kind of silent redefinition this guard exists to prevent.
    numerator, scored_markers = formula
    ok = grand[numerator]
    scored = sum(grand[m] for m in scored_markers)
    if parity_claim.get("rows") != len(rows):
        problems.append("rows in file: arithmetic says %s, parsed %d"
                        % (parity_claim.get("rows"), len(rows)))
    if parity_claim.get("scored") != scored:
        problems.append("scored rows: arithmetic says %s, rows give %d"
                        % (parity_claim.get("scored"), scored))
    if parity_claim.get("ok") != ok:
        problems.append("parity numerator: arithmetic says %s, rows give %d"
                        % (parity_claim.get("ok"), ok))
    if scored and parity_claim.get("pct") != round(100.0 * ok / scored, 1):
        problems.append("parity percent: arithmetic says %s, rows give %.1f"
                        % (parity_claim.get("pct"), 100.0 * ok / scored))
    return problems


def parse_formula(text, markers):
    """Read the parity formula the map declares, as (numerator, scored list).

    The doc states it once, as `<marker> / (<markers>)`. Reading it here keeps
    one definition of "scored" instead of a copy in this script that drifts.
    """
    m = re.search(r"MO2 parity`?\s*=\s*`([^`]+)`", text)
    if not m:
        return None
    expr = m.group(1)
    numerator, _, rest = expr.partition("/")
    numerator = numerator.strip()
    scored = [t.strip() for t in rest.strip().strip("()").split("+")]
    if numerator not in markers or any(s not in markers for s in scored):
        return None
    return numerator, scored


def parse_parity(text):
    """Pull the arithmetic block's four numbers, or {} if it is absent."""
    m = re.search(r"rows in file\s+(\d+)", text)
    n = re.search(r"scored rows \(ok \+ part \+ miss\)\s+(\d+)", text)
    p = re.search(r"MO2 parity\s+ok\s+/\s*scored\s+(\d+)\s*/\s*(\d+)\s*=\s*([\d.]+)%", text)
    if not (m and n and p):
        return {}
    return {"rows": int(m.group(1)), "scored": int(n.group(1)),
            "ok": int(p.group(1)), "pct": float(p.group(3))}


def format_findings(found):
    return "".join("%s\t%s\n" % (key, construct) for key, construct in sorted(found))


def read_baseline(path):
    entries = set()
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if line:
                key, _, construct = line.partition("\t")
                entries.add((key, construct))
    return entries


def main():
    parser = argparse.ArgumentParser(description="featuremap claim guard")
    parser.add_argument("--doc", default=os.path.join(REPO, "docs", "featuremap.md"),
                        help="docs/featuremap.md")
    parser.add_argument("--repo", default=REPO, help="Core source root")
    parser.add_argument("--baseline",
                        default=os.path.join(HERE, "featuremap_baseline.txt"),
                        help="checked-in baseline of unresolvable GMM-side pointers")
    parser.add_argument("--update", action="store_true",
                        help="rewrite the baseline from the current map")
    args = parser.parse_args()

    for label, path in (("doc", args.doc), ("repo", args.repo)):
        if not os.path.exists(path):
            sys.stderr.write("%s does not exist: %s\n" % (label, path))
            return 2

    try:
        with open(args.doc, encoding="utf-8") as fh:
            text = fh.read()
    except OSError as exc:
        sys.stderr.write("cannot read %s: %s\n" % (args.doc, exc))
        return 2

    rows, summary, total, markers, win_tag = parse_rows(text)

    if not rows:
        sys.stderr.write(
            "featuremap guard: 0 rows parsed from %s.\n"
            "The guard saw nothing, so it checked nothing. A pass here means\n"
            "the map was not read, not that the map is true.\n" % args.doc)
        return 1
    if not markers or win_tag != WIN_TAG:
        sys.stderr.write("featuremap guard: the summary table header is not a "
                         "disposition list ending in the %s tag.\n" % WIN_TAG)
        return 1

    dupes = [k for k, n in Counter(key for key, _, _, _ in rows).items() if n > 1]
    if dupes:
        sys.stderr.write("featuremap guard: ambiguous row keys (baseline is "
                         "keyed on them): %s\n" % ", ".join(sorted(dupes)[:5]))
        return 1

    current = collect_findings(rows, index_tree(args.repo))

    if args.update:
        with open(args.baseline, "w", encoding="utf-8") as fh:
            fh.write(format_findings(current))
        print("featuremap baseline rewritten: %d unresolvable pointers over "
              "%d rows" % (len(current), len(rows)))
    else:
        try:
            expected = read_baseline(args.baseline)
        except OSError as exc:
            sys.stderr.write("cannot read baseline: %s\n" % exc)
            return 2

        if current != expected:
            sys.stderr.write("featuremap ratchet: pointers do not resolve as baselined\n\n")
            for key, construct in sorted(current - expected):
                sys.stderr.write("  NEW       %-42s %s\n" % (key, construct))
            for key, construct in sorted(expected - current):
                sys.stderr.write("  RESOLVED  %-42s %s\n" % (key, construct))
            sys.stderr.write(
                "\nNEW points at a file or symbol that does not exist, so the\n"
                "row is a stale claim. RESOLVED means the fix landed: drop it\n"
                "from the baseline with\n"
                "  python3 scripts/check_featuremap.py --update\n")
            return 1

    formula = parse_formula(text, markers)
    if formula is None:
        sys.stderr.write(
            "featuremap guard: no parity formula. The map must state it once, as\n"
            "`MO2 parity` = `<marker> / (<marker> + ...)`, built from markers this\n"
            "table defines, so scored rows have one definition and not two.\n")
        return 1

    problems = check_summary(rows, summary, total, markers, formula, parse_parity(text))
    if problems:
        sys.stderr.write("featuremap guard: the summary does not match the rows\n\n")
        for line in problems:
            sys.stderr.write("  %s\n" % line)
        sys.stderr.write("\nRecount the section from its rows. Do not adjust the\n"
                         "cell to make the arithmetic work - the row is the claim.\n")
        return 1

    clean = len(rows) - len({key for key, _ in current})
    print("featuremap: %d rows, %d markers, %d unresolvable GMM-side pointers, "
          "%d rows with every pointer resolved"
          % (len(rows), len(markers), len(current), clean))
    return 0


if __name__ == "__main__":
    sys.exit(main())
