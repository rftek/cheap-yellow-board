"""Checks the MAJOR.MINOR versions of the apps and the launcher.

Every apps/<name>/ folder and launcher/ must have:
- version.txt holding one MAJOR.MINOR version, such as 1.0 or 2.3;
- a "## Changelog" section in its README.md whose first entry is a
  "### <version>" heading for that version (newest first).

Given BASE, every app or launcher folder with changes since BASE other than
Markdown must also have a higher version than it had at BASE. The root
README's "Versions" section describes the scheme.

Usage: python tools/check_versions.py [BASE]   (run from the repository root)
"""

import re
import subprocess
import sys
from pathlib import Path

VERSION = re.compile(r"(\d+)\.(\d+)")


def projects():
    apps = sorted(p for p in Path("apps").iterdir() if (p / "supported-targets.txt").is_file())
    return apps + [Path("launcher")]


def parse(text):
    match = VERSION.fullmatch(text.strip())
    return (int(match[1]), int(match[2])) if match else None


def git(*args):
    result = subprocess.run(["git", *args], capture_output=True, text=True)
    return result.stdout if result.returncode == 0 else None


def first_changelog_version(readme):
    """The version in the first ### heading under ## Changelog, or None."""
    in_changelog = False
    for line in readme.splitlines():
        if line.startswith("## "):
            in_changelog = line.strip().lower() == "## changelog"
        elif in_changelog and line.startswith("### "):
            return line[4:].split()[0] if line[4:].split() else ""
    return None


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else ""
    changed = None
    if base and set(base) != {"0"}:
        output = git("diff", "--name-only", base, "HEAD")
        changed = output.split() if output is not None else None

    errors = []
    for folder in projects():
        path = folder.as_posix()
        version_file = folder / "version.txt"
        if not version_file.is_file():
            errors.append(f"{path}/version.txt is missing; start a new app at 1.0")
            continue
        text = version_file.read_text().strip()
        version = parse(text)
        if version is None:
            errors.append(f"{path}/version.txt holds {text!r}, not MAJOR.MINOR such as 1.0")
            continue

        readme = folder / "README.md"
        entry = first_changelog_version(readme.read_text(encoding="utf-8")) \
            if readme.is_file() else None
        if entry is None:
            errors.append(f"{path}/README.md needs a '## Changelog' section "
                          f"starting with '### {text}'")
        elif entry != text:
            errors.append(f"{path}/README.md: the newest changelog entry is {entry!r}, "
                          f"but version.txt is {text}; add '### {text}' at the top")

        if changed is None:
            continue
        touched = [f for f in changed if f.startswith(path + "/") and not f.endswith(".md")]
        if not touched:
            continue
        before = git("show", f"{base}:{path}/version.txt")
        old = parse(before) if before is not None else None
        if old is not None and version <= old:
            errors.append(f"{path} changed ({touched[0]}{' ...' if len(touched) > 1 else ''}) "
                          f"but its version is still {text}: bump version.txt "
                          f"(MINOR for features and fixes, MAJOR for breaking or big "
                          f"changes) and add a changelog entry")
        else:
            was = before.strip() if before is not None else "new"
            print(f"{path}: {was} -> {text}")

    for error in errors:
        print(f"::error::{error}")
    if errors:
        sys.exit(1)
    print(f"{len(projects())} versions checked")


if __name__ == "__main__":
    main()
