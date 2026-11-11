"""Prints the CI build matrix: one {"app", "target"} entry per app folder that
changed since BASE and per chip target in its supported-targets.txt.

A change to the shared code (components/, external/, tools/ or the
workflow) rebuilds every app, as does an empty or unknown BASE. An app that
compiles another app's sources lists that app in its uses-apps.txt, and is
rebuilt when that app changes.

Usage: python tools/ci_plan.py [BASE]   (run from the repository root)
"""

import json
import subprocess
import sys
from pathlib import Path

SHARED = ("components/", "external/", "tools/", ".github/workflows/", ".gitmodules")


def changed_files(base):
    """The files changed between base and HEAD, or None to rebuild all."""
    if not base or set(base) == {"0"}:
        return None
    result = subprocess.run(["git", "diff", "--name-only", base, "HEAD"],
                            capture_output=True, text=True)
    if result.returncode != 0:
        return None
    return result.stdout.split()


def uses(app):
    """The other apps whose sources app compiles (its uses-apps.txt)."""
    path = Path("apps", app, "uses-apps.txt")
    return set(path.read_text().split()) if path.is_file() else set()


def main():
    apps = sorted(p.name for p in Path("apps").iterdir()
                  if (p / "supported-targets.txt").is_file())
    files = changed_files(sys.argv[1] if len(sys.argv) > 1 else "")
    if files is None or any(f.startswith(SHARED) for f in files):
        selected = apps
    else:
        changed = {f.split("/")[1] for f in files
                   if f.startswith("apps/") and not f.endswith(".md")
                   and f.split("/")[1] in apps}
        selected = sorted(changed | {app for app in apps if uses(app) & changed})

    matrix = []
    for app in selected:
        targets = Path("apps", app, "supported-targets.txt").read_text().split()
        matrix += [{"app": app, "target": t} for t in targets]
    print(json.dumps(matrix))


if __name__ == "__main__":
    main()
