# Instructions for Claude Code

The repository's working rules are shared with GitHub Copilot and kept in one place:

@.github/copilot-instructions.md

## Versions: bump with every change

Every app (`apps/<name>/`) and the launcher (`launcher/`) has a MAJOR.MINOR version in its `version.txt`, such as `1.0` or `2.3`. The root README's "Versions" section has the full scheme. In every session, whenever you change an app or the launcher:

1. **Bump its `version.txt`** once per pull request, from the version on the pull request's base branch:
   - **MINOR** (`1.2` to `1.3`) for features, fixes and other changes that keep it working as before.
   - **MAJOR** (`1.3` to `2.0`, MINOR back to 0) for breaking or big changes: a redesign, settings or saved data that don't carry over, a changed NVS layout or partition needs, or anything an older launcher or app can't work with.
   If the branch already bumped it, don't bump it again; update that version's changelog entry instead.
2. **Add a changelog entry** at the top of the `## Changelog` section of that folder's `README.md`, newest first:

   ```markdown
   ### 1.3 (YYYY-MM-DD)

   - What changed, in a line or two.
   ```

3. **A new app** starts with `version.txt` at `1.0`, a `## Changelog` section with a `### 1.0` entry saying what it does, and a row in the root README's app table.

Changes to Markdown alone need no bump. A change to shared code in `components/` bumps the apps whose behavior it changes on purpose.

CI enforces this: the `versions` job (`tools/check_versions.py`) fails `ci-result` when a changed app or the launcher isn't bumped, or when the newest changelog entry doesn't match `version.txt`. Check before pushing with `python tools/check_versions.py origin/main` (after committing; it compares commits).

Don't put the version anywhere else: ESP-IDF reads `version.txt` into `esp_app_desc_t.version`, which the launcher reads from each slot, and CI labels the published image and fills the catalog's `app_version` from it.
