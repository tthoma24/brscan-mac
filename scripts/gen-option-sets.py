#!/usr/bin/env python3
"""Generates gui/BrscanConfigCore's Swift option-set constants from
config/option-sets.json (Task 1e.1's shared source of truth).

Usage:
    scripts/gen-option-sets.py           # regenerate the committed file
    scripts/gen-option-sets.py --check   # fail (with a diff) if the
                                          # committed file is stale

Only the Python 3 standard library is used, per task-1e1anti-brief.md's
Part 2 (no extra tooling needed to run this on a bare macOS + python3).
"""

import argparse
import difflib
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
JSON_PATH = REPO_ROOT / "config" / "option-sets.json"
OUTPUT_PATH = (
    REPO_ROOT
    / "gui"
    / "BrscanConfigCore"
    / "Sources"
    / "BrscanConfigCore"
    / "GeneratedOptionSets.swift"
)

# (JSON top-level key, Swift constant name, doc comment). Each of these
# JSON entries is an object with a "tokens" array of strings -- see
# config/option-sets.json's own comments for exactly which daemon parser
# each one mirrors.
TOKEN_SETS = [
    ("mode", "mode", "`<dest>.mode` tokens (daemon/config.cpp's ParseModeString)."),
    (
        "source",
        "source",
        "`<dest>.source` tokens (daemon/config.cpp's ParseSourceString).",
    ),
    (
        "format",
        "format",
        "`<dest>.format` tokens (daemon/config.cpp's ParseFormatString).",
    ),
    (
        "tiff_compression",
        "tiffCompression",
        "`<dest>.tiff_compression` tokens (daemon/config.cpp's "
        "ParseTiffCompressionString).",
    ),
    (
        "ocr_format",
        "ocrFormat",
        "`ocr.ocr_format` tokens (daemon/config.cpp's ParseOcrFormatString). "
        "OCR-only: the OCR route's output sub-format (searchable pdf, or the "
        "txt/html/rtf text sinks).",
    ),
    (
        "separation_mode",
        "separationMode",
        "`<dest>.separation` *modes* -- combine/off take no suffix; image/page "
        "each take a ':N' positive-int suffix (e.g. \"image:3\"). \"every:N\" is "
        "also accepted by the daemon as a backward-compat alias for "
        "\"image:N\" (same mode, not a separate one), so it isn't listed here.",
    ),
    (
        "paper",
        "paper",
        "`<dest>.paper` tokens (daemon/paper_size.h's IsKnownPaper / "
        "kPaperTable).",
    ),
]

HEADER = """\
// GENERATED FILE -- DO NOT EDIT BY HAND.
//
// Regenerate with `python3 scripts/gen-option-sets.py` from the repo root.
// Source of truth: config/option-sets.json -- see that file for exactly
// which daemon parser (daemon/config.cpp, daemon/paper_size.h) each token
// set mirrors. A CTest entry (see CMakeLists.txt's OptionSetsUpToDate)
// regenerates this file into a temp location and diffs it against this
// committed copy, so an edit to option-sets.json without a matching
// `gen-option-sets.py` run fails CI.

/// The valid `<dest>.<key>` config tokens the daemon (daemon/config.cpp)
/// accepts, and the paper tokens daemon/paper_size.h's `IsKnownPaper`
/// recognizes -- generated from config/option-sets.json so the GUI can
/// never offer a value the daemon would silently ignore.
public enum OptionSets {
"""

FOOTER = "}\n"


def swift_string_literal(token: str) -> str:
    """A Swift string literal for `token` (only characters JSON's own
    string escaping and Swift's happen to agree on are expected to ever
    appear in config/option-sets.json's tokens: plain ASCII, no quotes)."""
    escaped = token.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def swift_string_array(tokens):
    literals = ", ".join(swift_string_literal(t) for t in tokens)
    return f"[{literals}]"


def render(data: dict) -> str:
    lines = [HEADER]
    for json_key, swift_name, doc in TOKEN_SETS:
        entry = data.get(json_key)
        if entry is None or "tokens" not in entry:
            raise SystemExit(
                f"gen-option-sets.py: config/option-sets.json has no "
                f"'{json_key}.tokens' -- update TOKEN_SETS in this script "
                f"to match."
            )
        tokens = entry["tokens"]
        lines.append(f"  /// {doc}\n")
        lines.append(
            f"  public static let {swift_name}: [String] = "
            f"{swift_string_array(tokens)}\n"
        )
        lines.append("\n")

    dpi = data.get("dpi")
    if dpi is None or "default" not in dpi:
        raise SystemExit(
            "gen-option-sets.py: config/option-sets.json has no 'dpi.default'"
        )
    lines.append(
        "  /// `<dest>.dpi` is not an enumerated set: any positive integer "
        "is\n"
        "  /// accepted by daemon/config.cpp's ParsePositiveInt. This is "
        "the\n"
        "  /// documented default (brscan::Params's own default "
        "constructor).\n"
    )
    lines.append(f"  public static let dpiDefault: Int = {int(dpi['default'])}\n")

    lines.append(FOOTER)
    return "".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Don't write anything; fail (with a diff) if the committed "
        "Swift file is stale relative to config/option-sets.json.",
    )
    args = parser.parse_args()

    with JSON_PATH.open(encoding="utf-8") as f:
        data = json.load(f)

    generated = render(data)

    if args.check:
        if not OUTPUT_PATH.exists():
            print(f"error: {OUTPUT_PATH} does not exist -- run "
                  "scripts/gen-option-sets.py to generate it.",
                  file=sys.stderr)
            return 1
        committed = OUTPUT_PATH.read_text(encoding="utf-8")
        if committed != generated:
            diff = difflib.unified_diff(
                committed.splitlines(keepends=True),
                generated.splitlines(keepends=True),
                fromfile=str(OUTPUT_PATH) + " (committed)",
                tofile=str(OUTPUT_PATH) + " (regenerated)",
            )
            sys.stderr.write("".join(diff))
            print(
                "\nerror: gui/BrscanConfigCore/Sources/BrscanConfigCore/"
                "GeneratedOptionSets.swift is stale relative to "
                "config/option-sets.json. Run "
                "`python3 scripts/gen-option-sets.py` and commit the result.",
                file=sys.stderr,
            )
            return 1
        return 0

    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT_PATH.write_text(generated, encoding="utf-8")
    print(f"Wrote {OUTPUT_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
