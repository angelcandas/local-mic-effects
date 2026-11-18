#!/usr/bin/env python3
"""CHANGELOG.md <-> ui/changelog.js.

  scripts/changelog.py build            regenerate ui/changelog.js (commit it with CHANGELOG.md)
  scripts/changelog.py check            fail if ui/changelog.js is out of date (CI runs this)
  scripts/changelog.py add PR "Title" "What it does for the user"
                                        add a bullet under today's heading (creates it), then build

CHANGELOG.md format: "## YYYY-MM-DD" headings, newest first, each followed by
"- **Title.** Text (#12, #13)" bullets. The PR numbers in the brackets are how the app knows
which entries you have already seen.
"""
import datetime, json, pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
MD, JS = ROOT / "CHANGELOG.md", ROOT / "ui" / "changelog.js"


def parse(text):
    days = []
    for line in text.splitlines():
        m = re.match(r"##\s+(\d{4}-\d{2}-\d{2})\s*$", line)
        if m:
            days.append({"date": m.group(1), "entries": []})
            continue
        m = re.match(r"-\s+(.*)$", line)
        if m and days:
            body = m.group(1)
            prs = []
            tail = re.search(r"\s*\(((?:#\d+(?:,\s*)?)+)\)\s*$", body)
            if tail:
                prs = [int(n) for n in re.findall(r"#(\d+)", tail.group(1))]
                body = body[: tail.start()]
            t = re.match(r"\*\*(.+?)\*\*\s*(.*)$", body)
            title, detail = (t.group(1).rstrip("."), t.group(2)) if t else ("", body)
            days[-1]["entries"].append({"title": title, "text": detail, "prs": prs})
    return days


def render(days):
    latest = max((n for d in days for e in d["entries"] for n in e["prs"]), default=0)
    data = {"latest": latest, "days": days}
    return ("// Generated from CHANGELOG.md by scripts/changelog.py. Do not edit by hand.\n"
            "export const CHANGELOG = " + json.dumps(data, indent=1, ensure_ascii=False) + ";\n")


def main(argv):
    cmd = argv[1] if len(argv) > 1 else "build"
    if cmd == "add" and len(argv) == 5:
        pr, title, text = argv[2].lstrip("#"), argv[3].rstrip("."), argv[4]
        lines = MD.read_text().split("\n")
        today = datetime.date.today().isoformat()
        bullet = f"- **{title}.** {text} (#{pr})"
        first = next(i for i, l in enumerate(lines) if l.startswith("## "))
        if lines[first] == f"## {today}":
            lines.insert(first + 2, bullet)
        else:
            lines[first:first] = [f"## {today}", "", bullet, ""]
        MD.write_text("\n".join(lines))
        cmd = "build"
    elif cmd not in ("build", "check"):
        print(__doc__)
        return 2
    out = render(parse(MD.read_text()))
    if cmd == "check":
        if not JS.exists() or JS.read_text() != out:
            print("ui/changelog.js is out of date: run scripts/changelog.py build and commit it")
            return 1
        return 0
    JS.write_text(out)
    return 0


sys.exit(main(sys.argv))
