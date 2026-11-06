"""Writes manifest.json, the catalog the launcher reads to list the apps.

For each folder in apps/, it takes the release tagged with the folder name
and lists one asset per chip target: firmware.bin is the esp32 image and
firmware-<target>.bin the image for any other target. An asset's version is
its updated_at time, which is what the launcher saves for a downloaded slot.

Usage: GH_TOKEN=... GITHUB_REPOSITORY=owner/repo python tools/make_catalog.py OUT
"""

import json
import os
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


def releases(repository, token):
    """Every release in the repository, following the API's pagination."""
    url = f"https://api.github.com/repos/{repository}/releases?per_page=100"
    found = []
    while url:
        request = urllib.request.Request(url, headers={
            "Accept": "application/vnd.github+json",
            "Authorization": f"Bearer {token}",
        })
        with urllib.request.urlopen(request) as response:
            found += json.load(response)
            url = None
            for link in response.headers.get("Link", "").split(","):
                if 'rel="next"' in link:
                    url = link[link.index("<") + 1:link.index(">")]
    return found


def asset_target(name):
    if name == "firmware.bin":
        return "esp32"
    if name.startswith("firmware-") and name.endswith(".bin"):
        return name[len("firmware-"):-len(".bin")]
    return None


def main():
    output = Path(sys.argv[1])
    by_tag = {r["tag_name"]: r for r in releases(os.environ["GITHUB_REPOSITORY"],
                                                 os.environ["GH_TOKEN"])}
    apps = []
    for folder in sorted(Path("apps").iterdir()):
        targets_file = folder / "supported-targets.txt"
        if not targets_file.is_file():
            continue
        release = by_tag.get(folder.name)
        assets = {}
        for asset in release["assets"] if release else []:
            target = asset_target(asset["name"])
            if target is not None:
                assets[target] = {
                    "url": asset["browser_download_url"],
                    "size": asset["size"],
                    "version": asset["updated_at"],
                }
        for target in targets_file.read_text().split():
            if target not in assets:
                print(f"::warning::{folder.name} has no {target} image yet")
        if assets:
            apps.append({"name": folder.name, "assets": assets})

    catalog = {
        "schema": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "apps": apps,
    }
    output.write_text(json.dumps(catalog, indent=1) + "\n")
    print(f"{len(apps)} apps, {output.stat().st_size} bytes")


if __name__ == "__main__":
    main()
