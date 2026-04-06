#!/usr/bin/env python3
"""Update Fedora versions in build-rpm.yml based on endoflife.date API data.

Fetches the list of Fedora releases from https://endoflife.date/api/fedora.json,
filters to releases whose end-of-life date is today or in the future (or not yet
set), then rewrites the fedora matrix entries in .github/workflows/build-rpm.yml.
"""

import json
import re
import sys
import urllib.request
from datetime import date
from pathlib import Path

EOL_URL = "https://endoflife.date/api/fedora.json"
WORKFLOW_FILE = Path(__file__).parent.parent / ".github" / "workflows" / "build-rpm.yml"


def get_supported_fedora_versions() -> list[str]:
    """Return sorted list of currently-supported Fedora version strings."""
    print(f"Fetching Fedora lifecycle data from {EOL_URL} ...")
    with urllib.request.urlopen(EOL_URL) as response:  # noqa: S310
        data = json.loads(response.read())

    today = date.today()
    supported: list[str] = []

    for release in data:
        cycle = release.get("cycle")
        eol = release.get("eol")

        if eol is False or eol is None:
            # No end-of-life date — still supported
            supported.append(cycle)
        elif isinstance(eol, str):
            if date.fromisoformat(eol) >= today:
                supported.append(cycle)

    supported.sort(key=lambda x: int(x))
    return supported


def update_workflow(versions: list[str]) -> None:
    """Rewrite the fedora matrix entries in build-rpm.yml."""
    content = WORKFLOW_FILE.read_text()

    # Detect indentation used for existing fedora entries so we preserve it
    match = re.search(r"^([ \t]+)- fedora:\d+", content, re.MULTILINE)
    if not match:
        print("ERROR: Could not find any '- fedora:NN' entries in build-rpm.yml", file=sys.stderr)
        sys.exit(1)
    indent = match.group(1)

    new_entries = "".join(f"{indent}- fedora:{v}\n" for v in versions)

    # Replace all consecutive fedora:NN lines (one or more) with the new list
    pattern = rf"({re.escape(indent)}- fedora:\d+\n)+"
    new_content = re.sub(pattern, new_entries, content)

    if new_content == content:
        print("No changes needed — Fedora versions are already up to date.")
        return

    WORKFLOW_FILE.write_text(new_content)
    print(f"Updated {WORKFLOW_FILE} with Fedora versions: {versions}")


def main() -> None:
    versions = get_supported_fedora_versions()
    print(f"Currently supported Fedora versions: {versions}")
    update_workflow(versions)


if __name__ == "__main__":
    main()
