# Changelog

`CHANGELOG.md` (repo root) is the list of changes users see: **Help → Changelog** shows all of it, and the first
launch after an update opens **What's new** with the entries the user hasn't seen yet. "Seen" is the highest PR
number in the entries; it is kept in the app settings (`ui.changelogSeen`).

## Adding entries

Every PR with a change a user can notice adds one bullet (fixes too, when the user would have hit the bug):

```
scripts/changelog.py add 207 "Short title" "What it does for the user, in plain words."
```

That puts `- **Short title.** What it does... (#207)` under today's date in `CHANGELOG.md` and regenerates
`ui/changelog.js`. Commit both files. You can also edit `CHANGELOG.md` by hand (format: `## YYYY-MM-DD` headings,
newest first, `- **Title.** Text (#PR)` bullets), then run `scripts/changelog.py build`.

Write for the person using the app, not for developers: no class or file names, say what they can do now.
Skip purely internal changes (CI, tests, refactors).

CI runs `scripts/changelog.py check` and fails if `ui/changelog.js` doesn't match `CHANGELOG.md`.
After a merge conflict in either file, resolve `CHANGELOG.md`, then run `scripts/changelog.py build`.
